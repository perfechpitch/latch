#ifndef _LATCH_BACH_KSIM_TOPOLOGY_
#define _LATCH_BACH_KSIM_TOPOLOGY_

// 核阵列的物理连接与选路。
//
// chip 排成行列，每个 chip 里的核也排成行列。核里有一个 Router，它连四个地方：
//
//   左邻 右邻    同一排相邻的核
//   垂直邻居     同一列另一排的核
//   本核         两个口，Send 与 Recv 各走一个
//
// 四个角上的核各多一个 PCIe 口：左上接上、右上接右、左下接左、右下接下。所有连接都
// 是双向的，一条双向连接的两端各有一个 master 和一个 slave。
//
// 选路分两级。目标在本 chip 内就先走列再走行；不在本 chip 就先路由到该方向的角核，
// 由它出 PCIe。跨 chip 的方向先比列后比行，所以一次跨 chip 只出一个口。
//
// 阵列两侧各有一个 PCIe Switch，进和出各走一边：输入从西边那个进，落在目标核那一行
// 最左边那个 chip 的左出口上；结果往东边那个出，一路向东走到最右那一列 chip 的右出口
// 才出得去。一条链路只承一个方向的流量。
//
// 核号是全局连续的：core_id 除以每 chip 的核数是 chip 号，余数是片内号；片内号除以
// 每 chip 的列数是排号，余数是列号。

#include <cstdint>

#include "base/log.h"

namespace latch {
namespace bach {
namespace ksim {

// 阵列之外的那个模块。核只做 FFN，前后的计算都在它那里，结果发回给它就写这个目标。
constexpr uint64_t kHostTarget = ~uint64_t{0};

enum class Port : uint32_t {
  kCore0 = 0,
  kCore1 = 1,
  kLeft = 2,
  kRight = 3,
  kVert = 4,
  kPcie = 5,
  kCount = 6,
};

constexpr uint32_t kPortCount = static_cast<uint32_t>(Port::kCount);

struct Topology {
  uint64_t chip_rows = 16;
  uint64_t chip_cols = 4;
  uint64_t core_rows = 2;
  uint64_t core_cols = 4;

  uint64_t CoresPerChip() const { return core_rows * core_cols; }
  uint64_t ChipCount() const { return chip_rows * chip_cols; }
  uint64_t CoreCount() const { return ChipCount() * CoresPerChip(); }

  uint64_t ChipOf(uint64_t core) const { return core / CoresPerChip(); }
  uint64_t LocalOf(uint64_t core) const { return core % CoresPerChip(); }
  uint64_t RowIn(uint64_t core) const { return LocalOf(core) / core_cols; }
  uint64_t ColIn(uint64_t core) const { return LocalOf(core) % core_cols; }
  uint64_t ChipRowOf(uint64_t core) const { return ChipOf(core) / chip_cols; }
  uint64_t ChipColOf(uint64_t core) const { return ChipOf(core) % chip_cols; }

  uint64_t CoreAt(uint64_t chip, uint64_t local) const {
    return chip * CoresPerChip() + local;
  }
  uint64_t LocalAt(uint64_t row, uint64_t col) const {
    return row * core_cols + col;
  }

  // 四个角上的核，片内号。
  uint64_t TopExit() const { return LocalAt(0, 0); }
  uint64_t RightExit() const { return LocalAt(0, core_cols - 1); }
  uint64_t LeftExit() const { return LocalAt(core_rows - 1, 0); }
  uint64_t BottomExit() const { return LocalAt(core_rows - 1, core_cols - 1); }

  bool HasPcie(uint64_t core) const {
    const uint64_t l = LocalOf(core);
    return l == TopExit() || l == RightExit() || l == LeftExit() ||
           l == BottomExit();
  }

  // 这个口连到哪个核。没有这条连接时返回 -1。PCIe 口连的是别的 chip，这里不解，由
  // 上一层按 chip 之间的接线找对端。
  int64_t NeighborOf(uint64_t core, Port p) const {
    const uint64_t chip = ChipOf(core);
    const uint64_t r = RowIn(core);
    const uint64_t c = ColIn(core);
    switch (p) {
      case Port::kLeft:
        return c == 0 ? -1 : static_cast<int64_t>(CoreAt(chip, LocalAt(r, c - 1)));
      case Port::kRight:
        return c + 1 >= core_cols
                   ? -1
                   : static_cast<int64_t>(CoreAt(chip, LocalAt(r, c + 1)));
      case Port::kVert: {
        if (core_rows < 2) return -1;
        const uint64_t other = r == 0 ? 1 : 0;
        return static_cast<int64_t>(CoreAt(chip, LocalAt(other, c)));
      }
      default:
        return -1;
    }
  }

  // 本 chip 内从 here 走到 local 目标该出哪个口。到了就返回 kCore0。
  Port RouteInChip(uint64_t here, uint64_t dst_local) const {
    const uint64_t hc = ColIn(here);
    const uint64_t hr = RowIn(here);
    const uint64_t dc = dst_local % core_cols;
    const uint64_t dr = dst_local / core_cols;
    if (dc > hc) return Port::kRight;
    if (dc < hc) return Port::kLeft;
    if (dr != hr) return Port::kVert;
    return Port::kCore0;
  }

  // 跨 chip 时本 chip 该从哪个角核出去。先比列后比行。
  uint64_t ExitFor(uint64_t here, uint64_t dst) const {
    const uint64_t hcc = ChipColOf(here);
    const uint64_t dcc = ChipColOf(dst);
    if (dcc > hcc) return RightExit();
    if (dcc < hcc) return LeftExit();
    return ChipRowOf(dst) > ChipRowOf(here) ? BottomExit() : TopExit();
  }

  Port RouteFrom(uint64_t here, uint64_t dst) const {
    // 发回阵列外面：一路向东，先走到本 chip 的右出口，由它出 PCIe；最右那一列的右口
    // 接的就是东边那个 Switch，中间各列的右口接的是右邻 chip。
    if (dst == kHostTarget) {
      const uint64_t exit_local = RightExit();
      if (LocalOf(here) == exit_local) return Port::kPcie;
      return RouteInChip(here, exit_local);
    }
    LOGCHECK(here < CoreCount() && dst < CoreCount(),
             "Topology: core id out of range.");
    if (ChipOf(here) == ChipOf(dst)) return RouteInChip(here, LocalOf(dst));
    const uint64_t exit_local = ExitFor(here, dst);
    if (LocalOf(here) == exit_local) return Port::kPcie;
    return RouteInChip(here, exit_local);
  }
};

}
}
}

#endif
