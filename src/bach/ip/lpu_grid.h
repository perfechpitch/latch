#ifndef _LATCH_BACH_IP_LPU_GRID_
#define _LATCH_BACH_IP_LPU_GRID_

// 全局坐标换算，以及构造期由编译侧读入的四张静态表。
//
// 48 颗 chip 摆成 12 × 4 的网格：gy = tray 序号 × 4 + tray 内层号（0～11），
// gx = 层内列号（0～3）。tray 与层不建对象。它们只起两个作用：给 chip 定全局
// 坐标，指出哪两处纵向链路换参数，两个作用都落在这里的换算与链路参数里。
//
// 四张表的填法约束照《LPU》第 3 章，跨表的自洽检查照《latch 建模计划》“读入时
// 的跨表自洽检查”的第 1、2 条。单张表内部的合规性由持有它的单元自己查，跨表
// 这一层没有哪个单元能独自看到，所以收在这里。

#include <array>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/log.h"
#include "bach/ip/chip/chip.h"

namespace latch {
namespace bach {

// ── 网格 ──
constexpr uint64_t kTrayNum = 3;
constexpr uint64_t kLayerPerTray = 4;
constexpr uint64_t kGridX = 4;                            // 层内列数
constexpr uint64_t kGridY = kTrayNum * kLayerPerTray;     // 12
constexpr uint64_t kChipNum = kGridX * kGridY;            // 48
constexpr uint64_t kMaxCorePerChip = kChipCoreNum;        // 10
constexpr uint64_t kComputePerChip = 8;
// 一个 EP 组两层，每层 4 颗 chip。
constexpr uint64_t kLayerPerGroup = 2;
// 中间两列 chip 的 core_bad_mask：core2 与 core7 是坏 core。
constexpr uint64_t kMiddleCoreBadMask = 0x084;

// 每 tray 4 个 PCIe Switch，左右各 2，一个接两层。
constexpr uint64_t kSwitchPerTray = 4;
constexpr uint64_t kSwitchNum = kTrayNum * kSwitchPerTray;  // 12
// 一个 Switch 三个端口：两层 chip 各一个，第三个留给片外桩。
constexpr uint64_t kSwitchPortNum = 3;
constexpr uint64_t kSwitchExtPort = 2;

// 包头 dst 在片外那一段的编码：0～47 是 chip，48 是出口桩，49 是入口桩。
// 阵列内部靠 path_id 走，这一项只在片外那一段有意义。
constexpr uint64_t kNodeOutStub = kChipNum;
constexpr uint64_t kNodeInStub = kChipNum + 1;

inline uint64_t GyOf(uint64_t tray, uint64_t layer) {
  return tray * kLayerPerTray + layer;
}
inline uint64_t TrayOf(uint64_t gy) { return gy / kLayerPerTray; }
inline uint64_t LayerOf(uint64_t gy) { return gy % kLayerPerTray; }
inline uint64_t ChipIdOf(uint64_t gx, uint64_t gy) { return gy * kGridX + gx; }
inline uint64_t GxOfChip(uint64_t chip) { return chip % kGridX; }
inline uint64_t GyOfChip(uint64_t chip) { return chip / kGridX; }

// core_bad_mask 由 gx 定：中间两列坏 core2、core7，两侧两列 10 个 core 全好。
inline uint64_t CoreBadMaskOfGx(uint64_t gx) {
  return gx == 0 || gx + 1 == kGridX ? 0 : kMiddleCoreBadMask;
}

// 上下相邻的两颗 chip 之间是不是跨 tray。
inline bool CrossTray(uint64_t gy) { return LayerOf(gy) + 1 == kLayerPerTray; }

// 这颗 chip 的边缘口接哪个 Switch。左侧两个编号 0、1，右侧两个编号 2、3，
// 每个接相邻的两层。
inline uint64_t SwitchOfEdge(uint64_t gx, uint64_t gy) {
  uint64_t side = gx == 0 ? 0 : 2;
  uint64_t half = LayerOf(gy) / 2;
  return TrayOf(gy) * kSwitchPerTray + side + half;
}
// 接在这个 Switch 的哪个端口：两层里靠上的那层占 0，靠下的占 1。
inline uint64_t SwitchPortOfEdge(uint64_t gy) { return LayerOf(gy) % 2; }

// ── 角色 ──

// 编译器给每个 core 分的用途。core 自己不保存角色，这一份只给逻辑编号与检查用。
enum class CoreRole : uint32_t {
  kCompute = 0,     // 计算 core，逻辑编号 0～7
  kBroadcast = 1,   // B core：组头 chip 的 core0
  kReduce = 2,      // R core：最后一列 chip 的 core9
  kSpare = 3,       // 不派角色：坏 core，以及没派用途的好 core
};

// 组头 chip：每个 EP 组左上角那颗。
inline bool GroupHead(uint64_t gx, uint64_t gy) {
  return gx == 0 && gy % kLayerPerGroup == 0;
}

// 角色分配表。
//   第一列，组头 chip  core0 是 B core，core5 不派角色
//   第一列，其余        core0、core5 不派角色
//   中间两列            坏 core2、core7 不派角色
//   最后一列            core9 是 R core，core4 不派角色
// 其余一律是计算 core。
inline CoreRole RoleOf(uint64_t gx, uint64_t gy, uint64_t core) {
  if ((CoreBadMaskOfGx(gx) >> core) & 1u) return CoreRole::kSpare;
  if (gx == 0) {
    if (core == 0) {
      return GroupHead(gx, gy) ? CoreRole::kBroadcast : CoreRole::kSpare;
    }
    if (core == 5) return CoreRole::kSpare;
  } else if (gx + 1 == kGridX) {
    if (core == 9) return CoreRole::kReduce;
    if (core == 4) return CoreRole::kSpare;
  }
  return CoreRole::kCompute;
}

// ── 四张静态表 ──

struct GridEntry {
  uint64_t tray = 0, layer = 0, col = 0;
  uint64_t gx = 0, gy = 0;
};

// 一个物理 core 的逻辑编号与角色。不派角色的 core 没有逻辑编号。
struct LogicalEntry {
  uint64_t logical = 0;
  CoreRole role = CoreRole::kCompute;
  bool mapped = false;
};

// 外部数据从 global_top_left 西侧进，结果从 global_bottom_right 东侧出。
struct EntryExit {
  uint64_t in_gx = 0, in_gy = 0;
  uint64_t out_gx = kGridX - 1, out_gy = kGridY - 1;
};

// 四种 core 级切分模式。
enum class SplitMode : uint32_t {
  kEptpNn = 0,
  kEptpNk = 1,
  kPptpNn = 2,
  kPptpNk = 3,
};

struct SplitParam {
  uint64_t ep = 6, tp = 8, pp = 1, dp = 1;
  SplitMode mode = SplitMode::kEptpNn;
  uint64_t gpu_num = 1, batch = 1;
};

struct LpuTables {
  std::array<GridEntry, kChipNum> grid;
  std::array<uint64_t, kChipNum> core_bad_mask;
  std::array<std::array<LogicalEntry, kMaxCorePerChip>, kChipNum> logical_map;
  EntryExit entry_exit;
  SplitParam split;
};

// 按填法约束生成一份。编译侧给的表要经同一套检查，两边填出来的应当一致。
inline LpuTables BuildLpuTables(SplitParam const& split) {
  LpuTables t;
  t.split = split;
  for (uint64_t gy = 0; gy < kGridY; ++gy) {
    for (uint64_t gx = 0; gx < kGridX; ++gx) {
      uint64_t i = ChipIdOf(gx, gy);
      GridEntry& g = t.grid[i];
      g.tray = TrayOf(gy);
      g.layer = LayerOf(gy);
      g.col = gx;
      g.gx = gx;
      g.gy = gy;

      t.core_bad_mask[i] = CoreBadMaskOfGx(gx);

      // 逻辑 0～7 按物理编号顺序给计算 core；B core、R core 拿逻辑 8；不派角
      // 色的不映射。
      uint64_t next = 0;
      for (uint64_t c = 0; c < kMaxCorePerChip; ++c) {
        LogicalEntry& e = t.logical_map[i][c];
        e.role = RoleOf(gx, gy, c);
        if (e.role == CoreRole::kCompute) {
          e.logical = next++;
          e.mapped = true;
        } else if (e.role == CoreRole::kSpare) {
          e.mapped = false;
        } else {
          e.logical = kComputePerChip;
          e.mapped = true;
        }
      }
    }
  }
  return t;
}

// 跨表自洽检查，构造期做完，不逐拍。
inline void CheckLpuTables(LpuTables const& t) {
  // 第 1 条：grid 覆盖 48 项且 (gx, gy) 无重复；每颗 chip 至多 2 个坏 core、一
  // 行至多 1 个；每颗 chip 都是 8 个计算 core。
  std::set<std::pair<uint64_t, uint64_t>> seen;
  for (uint64_t i = 0; i < kChipNum; ++i) {
    GridEntry const& g = t.grid[i];
    LOGCHECK(g.gy == GyOf(g.tray, g.layer),
             "LpuTables: gy 应当等于 tray 序号 × 4 加 tray 内层号。");
    LOGCHECK(g.gx == g.col, "LpuTables: gx 应当等于层内列号。");
    LOGCHECK(g.gx < kGridX && g.gy < kGridY, "LpuTables: 坐标越界。");
    LOGCHECK(seen.insert({g.gx, g.gy}).second,
             "LpuTables: 两颗 chip 占了同一个 (gx, gy)。");

    uint64_t mask = t.core_bad_mask[i];
    LOGCHECK(mask < (1ull << kMaxCorePerChip),
             "LpuTables: core_bad_mask 只有 10 位。");
    uint64_t row_bad[2] = {0, 0};
    uint64_t compute = 0;
    for (uint64_t c = 0; c < kMaxCorePerChip; ++c) {
      if ((mask >> c) & 1u) ++row_bad[c / kChipCols];
      if (t.logical_map[i][c].role == CoreRole::kCompute) ++compute;
    }
    LOGCHECK(row_bad[0] <= 1 && row_bad[1] <= 1,
             "LpuTables: 一颗 chip 至多 2 个坏 core，一行至多 1 个。");
    LOGCHECK(compute == kComputePerChip,
             "LpuTables: 每颗 chip 都应当有 8 个计算 core。");
  }
  LOGCHECK(seen.size() == kChipNum, "LpuTables: grid 没有覆盖 48 颗 chip。");

  // 第 2 条：每 chip 的 logical_map 里逻辑 0～7 各出现一次；逻辑 8 只在两侧那
  // 两列出现；坏 core 不派角色，没有逻辑编号。
  for (uint64_t i = 0; i < kChipNum; ++i) {
    uint64_t gx = t.grid[i].gx;
    bool side = gx == 0 || gx + 1 == kGridX;
    std::set<uint64_t> compute_logical;
    for (uint64_t c = 0; c < kMaxCorePerChip; ++c) {
      LogicalEntry const& e = t.logical_map[i][c];
      if ((t.core_bad_mask[i] >> c) & 1u) {
        LOGCHECK(e.role == CoreRole::kSpare && !e.mapped,
                 "LpuTables: 坏 core 不派角色，也没有逻辑编号。");
        continue;
      }
      if (e.role == CoreRole::kCompute) {
        LOGCHECK(e.mapped, "LpuTables: 计算 core 应当有逻辑编号。");
        LOGCHECK(e.logical < kComputePerChip,
                 "LpuTables: 计算 core 的逻辑编号应当在 0～7 之内。");
        LOGCHECK(compute_logical.insert(e.logical).second,
                 "LpuTables: 同一个逻辑编号给了两个计算 core。");
      } else if (e.role == CoreRole::kSpare) {
        LOGCHECK(!e.mapped, "LpuTables: 不派角色的 core 不应当有逻辑编号。");
      } else {
        LOGCHECK(e.logical == kComputePerChip,
                 "LpuTables: B core 与 R core 的逻辑编号应当是 8。");
        LOGCHECK(side, "LpuTables: 逻辑编号 8 只在两侧那两列出现。");
      }
    }
    LOGCHECK(compute_logical.size() == kComputePerChip,
             "LpuTables: 逻辑 0～7 应当各出现一次。");
  }

  // 入口在左上角，出口在右下角。
  LOGCHECK(t.entry_exit.in_gx == 0 && t.entry_exit.in_gy == 0,
           "LpuTables: 入口应当在 global_top_left。");
  LOGCHECK(t.entry_exit.out_gx + 1 == kGridX &&
               t.entry_exit.out_gy + 1 == kGridY,
           "LpuTables: 出口应当在 global_bottom_right。");
}

}  // namespace bach
}  // namespace latch

#endif
