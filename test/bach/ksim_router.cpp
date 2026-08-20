// Router 的选路与时间。单元自己挂时钟单独跑，不必为了测它拉起整个核。

#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "base/signal_tracer.h"
#include "bach/ksim/router.h"
#include "gtest/gtest.h"

using namespace latch;
using namespace latch::bach::ksim;

namespace {

class Catcher : public FlitSink {
 public:
  void Deliver(Flit const& f, Time now) override {
    got.push_back(f);
    at.push_back(now);
  }
  std::vector<Flit> got;
  std::vector<Time> at;
};

// 一个 chip 的八个核：同排相邻、同列上下，全双向。每个 Router 自己挂时钟。
struct Fabric {
  Topology topo{1, 1, 2, 4};
  RouterParams par{32, 1, 32, 40};
  ClockPtr clk;
  std::vector<std::unique_ptr<Router>> rs;
  std::vector<std::unique_ptr<Catcher>> cs;

  Fabric() {
    RT::Reset(2, 16);
    SetTraceDisabled(true);
    clk = MakeClock(0, 1);
    for (uint64_t i = 0; i < topo.CoreCount(); ++i) {
      rs.push_back(std::make_unique<Router>(clk, i, topo, par, nullptr,
                                            "router" + std::to_string(i), 0,
                                            /*standalone=*/true));
      cs.push_back(std::make_unique<Catcher>());
    }
    for (uint64_t i = 0; i < topo.CoreCount(); ++i) {
      rs[i]->ConnectCore(cs[i].get());
      for (Port p : {Port::kLeft, Port::kRight, Port::kVert}) {
        const int64_t n = topo.NeighborOf(i, p);
        if (n >= 0) rs[i]->ConnectNeighbor(p, rs[static_cast<uint64_t>(n)].get());
      }
    }
  }

  void RunTo(Time end) {
    clk->Continue(end);
    RT::JoinAll();
  }
};

}  // namespace

TEST(KsimRouter, SameRowHopsRight) {
  Fabric f;
  f.rs[0]->Inject(Flit{0, 2, 320, 7, 0}, 0);
  f.RunTo(200);
  ASSERT_EQ(f.cs[2]->got.size(), 1u);
  EXPECT_EQ(f.cs[2]->got[0].tag, 7u);
  EXPECT_EQ(f.cs[2]->got[0].bytes, 320u);
  // 中途的 core 1 只转发，不投递给本核。
  EXPECT_TRUE(f.cs[1]->got.empty());
  EXPECT_EQ(f.rs[1]->Forwarded(), 1u);
}

TEST(KsimRouter, ServiceTimeFollowsBandwidth) {
  Time small = 0;
  {
    Fabric f;
    f.rs[0]->Inject(Flit{0, 1, 320, 0, 0}, 0);
    f.RunTo(100);
    ASSERT_EQ(f.cs[1]->at.size(), 1u);
    small = f.cs[1]->at[0];
  }
  Fabric g;
  g.rs[0]->Inject(Flit{0, 1, 640, 0, 0}, 0);
  g.RunTo(100);
  ASSERT_EQ(g.cs[1]->at.size(), 1u);
  // 字节数翻倍，服务时间跟着翻倍，线延迟那一拍不变。
  EXPECT_EQ(g.cs[1]->at[0] - small, 10u);
}

TEST(KsimRouter, SameOutputQueuesUp) {
  Fabric f;
  f.rs[0]->Inject(Flit{0, 1, 320, 1, 0}, 0);
  f.rs[0]->Inject(Flit{0, 1, 320, 2, 0}, 0);
  f.RunTo(200);
  ASSERT_EQ(f.cs[1]->at.size(), 2u);
  // 两个包走同一条出口，第二个要等第一个让出来，相差一次服务时间。
  EXPECT_EQ(f.cs[1]->at[1] - f.cs[1]->at[0], 10u);
}

TEST(KsimRouter, DifferentOutputsDoNotBlockEachOther) {
  Fabric f;
  // core 1 往左发一个、往右发一个，走的是两条独立的链路。
  f.rs[1]->Inject(Flit{1, 0, 320, 1, 0}, 0);
  f.rs[1]->Inject(Flit{1, 2, 320, 2, 0}, 0);
  f.RunTo(200);
  ASSERT_EQ(f.cs[0]->at.size(), 1u);
  ASSERT_EQ(f.cs[2]->at.size(), 1u);
  // 谁也没等谁。差的那一点是一个入端口每拍只出一个包的仲裁粒度，不是一次十拍的服务。
  EXPECT_LE(f.cs[2]->at[0] - f.cs[0]->at[0], 2u);
}

TEST(KsimRouter, AcrossRowsGoesColumnThenVertical) {
  Fabric f;
  f.rs[0]->Inject(Flit{0, 6, 64, 0, 0}, 0);
  f.RunTo(300);
  ASSERT_EQ(f.cs[6]->got.size(), 1u);
  EXPECT_EQ(f.rs[1]->Forwarded(), 1u);
  EXPECT_EQ(f.rs[2]->Forwarded(), 1u);
  // 没绕下排。
  EXPECT_EQ(f.rs[4]->Forwarded(), 0u);
  EXPECT_EQ(f.rs[5]->Forwarded(), 0u);
}

TEST(KsimRouter, EveryPairInsideOneChipArrives) {
  for (uint64_t s = 0; s < 8; ++s) {
    for (uint64_t d = 0; d < 8; ++d) {
      if (s == d) continue;
      Fabric f;
      f.rs[s]->Inject(Flit{s, d, 64, s * 8 + d, 0}, 0);
      f.RunTo(400);
      ASSERT_EQ(f.cs[d]->got.size(), 1u) << s << " -> " << d;
      EXPECT_EQ(f.cs[d]->got[0].tag, s * 8 + d);
    }
  }
}
