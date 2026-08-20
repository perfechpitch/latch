// 物理连接与选路。锁住四个角的口、片内选路的次序、跨 chip 走哪个角出去。

#include "bach/ksim/topology.h"
#include "gtest/gtest.h"

using namespace latch::bach::ksim;

namespace {
Topology Small() { return Topology{2, 2, 2, 4}; }   // 2x2 chip，每 chip 2x4 核
}

TEST(KsimTopology, CoreNumbering) {
  const Topology t = Small();
  EXPECT_EQ(t.CoresPerChip(), 8u);
  EXPECT_EQ(t.CoreCount(), 32u);
  EXPECT_EQ(t.ChipOf(9), 1u);
  EXPECT_EQ(t.LocalOf(9), 1u);
  EXPECT_EQ(t.RowIn(9), 0u);
  EXPECT_EQ(t.ColIn(9), 1u);
  EXPECT_EQ(t.RowIn(13), 1u);
  EXPECT_EQ(t.ColIn(13), 1u);
}

TEST(KsimTopology, CornersCarryThePciePorts) {
  const Topology t = Small();
  EXPECT_EQ(t.TopExit(), 0u);
  EXPECT_EQ(t.RightExit(), 3u);
  EXPECT_EQ(t.LeftExit(), 4u);
  EXPECT_EQ(t.BottomExit(), 7u);
  for (uint64_t l = 0; l < 8; ++l) {
    const bool corner = l == 0 || l == 3 || l == 4 || l == 7;
    EXPECT_EQ(t.HasPcie(l), corner) << l;
  }
}

TEST(KsimTopology, NeighboursAreOnlyTheThreeDirections) {
  const Topology t = Small();
  // core 1 在上排第二列：左右都有，垂直连到下排同列。
  EXPECT_EQ(t.NeighborOf(1, Port::kLeft), 0);
  EXPECT_EQ(t.NeighborOf(1, Port::kRight), 2);
  EXPECT_EQ(t.NeighborOf(1, Port::kVert), 5);
  // 行首没有左邻，行尾没有右邻。
  EXPECT_EQ(t.NeighborOf(0, Port::kLeft), -1);
  EXPECT_EQ(t.NeighborOf(3, Port::kRight), -1);
  // 下排的垂直邻居在上排。
  EXPECT_EQ(t.NeighborOf(5, Port::kVert), 1);
  // 第二个 chip 的核号连着数，邻居不跨 chip。
  EXPECT_EQ(t.NeighborOf(9, Port::kLeft), 8);
  EXPECT_EQ(t.NeighborOf(8, Port::kLeft), -1);
}

TEST(KsimTopology, InChipRoutingGoesColumnThenRow) {
  const Topology t = Small();
  EXPECT_EQ(t.RouteFrom(0, 2), Port::kRight);
  EXPECT_EQ(t.RouteFrom(2, 0), Port::kLeft);
  // 同一列不同排才走垂直口。
  EXPECT_EQ(t.RouteFrom(0, 4), Port::kVert);
  // 目标在另一排另一列：先走列。
  EXPECT_EQ(t.RouteFrom(0, 5), Port::kRight);
  EXPECT_EQ(t.RouteFrom(1, 5), Port::kVert);
  // 到了自己就不再出口。
  EXPECT_EQ(t.RouteFrom(3, 3), Port::kCore0);
}

TEST(KsimTopology, CrossChipLeavesByTheMatchingCorner) {
  const Topology t = Small();
  // chip 0 到 chip 1 是往右，出右上角 core 3。
  EXPECT_EQ(t.ExitFor(0, 8), t.RightExit());
  EXPECT_EQ(t.RouteFrom(3, 8), Port::kPcie);
  EXPECT_EQ(t.RouteFrom(0, 8), Port::kRight);
  // chip 1 回 chip 0 是往左，出左下角 core 4，也就是核号 8+4。
  EXPECT_EQ(t.ExitFor(8, 0), t.LeftExit());
  EXPECT_EQ(t.RouteFrom(12, 0), Port::kPcie);
  // chip 0 到 chip 2 同列不同行，往下出右下角 core 7。
  EXPECT_EQ(t.ExitFor(0, 16), t.BottomExit());
  EXPECT_EQ(t.RouteFrom(7, 16), Port::kPcie);
  // chip 2 回 chip 0 往上，出左上角 core 0。
  EXPECT_EQ(t.ExitFor(16, 0), t.TopExit());
  EXPECT_EQ(t.RouteFrom(16, 0), Port::kPcie);
}

// 发回阵列外面走东边那个 Switch：片内先走到本 chip 的右出口，由它出 PCIe，一列一列
// 往东传，最右那一列的右出口接的就是外围模块。西边那个口只管进，不承回程。
TEST(KsimTopology, HostTrafficLeavesEastward) {
  const Topology t = Small();
  for (uint64_t c = 0; c < t.CoreCount(); ++c) {
    const Port p = t.RouteFrom(c, kHostTarget);
    if (t.LocalOf(c) == t.RightExit()) {
      EXPECT_EQ(p, Port::kPcie) << c;
    } else {
      EXPECT_EQ(p, t.RouteInChip(c, t.RightExit())) << c;
      EXPECT_NE(p, Port::kPcie) << c;
    }
  }
  // 每个核顺着走都落在本 chip 的右出口上，不会绕出本 chip。
  for (uint64_t c = 0; c < t.CoreCount(); ++c) {
    uint64_t here = c;
    int hops = 0;
    while (t.LocalOf(here) != t.RightExit() && hops < 16) {
      const int64_t nxt = t.NeighborOf(here, t.RouteFrom(here, kHostTarget));
      ASSERT_GE(nxt, 0) << c << " 在 " << here << " 走进死口";
      here = static_cast<uint64_t>(nxt);
      ++hops;
    }
    EXPECT_EQ(t.LocalOf(here), t.RightExit()) << c;
    EXPECT_EQ(t.ChipOf(here), t.ChipOf(c)) << c;
  }
}

TEST(KsimTopology, EveryPairReachesInFiniteHops) {
  const Topology t = Small();
  for (uint64_t s = 0; s < t.CoreCount(); ++s) {
    for (uint64_t d = 0; d < t.CoreCount(); ++d) {
      if (s == d) continue;
      uint64_t here = s;
      int hops = 0;
      while (here != d && hops < 64) {
        const Port p = t.RouteFrom(here, d);
        if (p == Port::kPcie) break;   // 出了 chip，交给上一层接线
        const int64_t nxt = t.NeighborOf(here, p);
        ASSERT_GE(nxt, 0) << s << " -> " << d << " 在 " << here << " 走进死口";
        here = static_cast<uint64_t>(nxt);
        ++hops;
      }
      EXPECT_LT(hops, 64) << s << " -> " << d << " 绕不出来";
    }
  }
}
