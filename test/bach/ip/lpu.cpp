// LPU 装配的行为基线。
//
// 这一层自己不打拍，判据是结构性的：48 颗 chip 构造出来没有、坐标换算对不对、
// 同层左右与同列上下接的是不是直连、跨 tray 那两处换没换参数、每层两端的边缘
// 口接的是不是本 tray 那个 PCIe Switch、片外桩挂在哪。

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/lpu.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

LpuTables Tables() {
  SplitParam split;
  split.ep = 6;
  split.tp = 8;
  split.mode = SplitMode::kEptpNn;
  return BuildLpuTables(split);
}

// 一个 LPU 上五万个模块，全部由这一个协程驱动。
class LpuDriver : public BachModule {
 public:
  LpuDriver(ClockPtr c, Lpu& target) : BachModule(c, "driver"), lpu(target) {}

 protected:
  void Step() override { lpu.RunStep(); }

 private:
  Lpu& lpu;
};

// ── 坐标换算与静态表 ──

TEST(BachLpuGrid, GridIsThreeTrayByFourLayerByFourChip) {
  // 48 chip = 3 tray × 4 层 × 4 chip，摆成全局 12 × 4 的网格。
  EXPECT_EQ(kTrayNum * kLayerPerTray * kGridX, 48u);
  EXPECT_EQ(kChipNum, 48u);
  EXPECT_EQ(kGridY, 12u);
  EXPECT_EQ(kGridX, 4u);

  LpuTables t = Tables();
  std::set<std::pair<uint64_t, uint64_t>> seen;
  for (uint64_t i = 0; i < kChipNum; ++i) {
    seen.insert({t.grid[i].gx, t.grid[i].gy});
  }
  EXPECT_EQ(seen.size(), 48u) << "48 个 (gx, gy) 不重不漏";
}

TEST(BachLpuGrid, GlobalYIsTrayTimesFourPlusLayer) {
  // gy = tray 序号 × 4 + tray 内层号（0～11），gx = 层内列号（0～3）。
  for (uint64_t tray = 0; tray < kTrayNum; ++tray) {
    for (uint64_t layer = 0; layer < kLayerPerTray; ++layer) {
      uint64_t gy = GyOf(tray, layer);
      EXPECT_EQ(gy, tray * 4 + layer);
      EXPECT_EQ(TrayOf(gy), tray);
      EXPECT_EQ(LayerOf(gy), layer);
    }
  }
  LpuTables t = Tables();
  for (uint64_t i = 0; i < kChipNum; ++i) {
    GridEntry const& g = t.grid[i];
    EXPECT_EQ(g.gy, GyOf(g.tray, g.layer)) << "chip=" << i;
    EXPECT_EQ(g.gx, g.col) << "chip=" << i;
  }
}

TEST(BachLpuGrid, ShapeFollowsColumn) {
  // 形状由 gx 定：gx ∈ {1, 2} 是 2×4，gx ∈ {0, 3} 是 2×5。
  EXPECT_EQ(ShapeOfGx(0), ChipShape::kFirst);
  EXPECT_EQ(ShapeOfGx(1), ChipShape::kMiddle);
  EXPECT_EQ(ShapeOfGx(2), ChipShape::kMiddle);
  EXPECT_EQ(ShapeOfGx(3), ChipShape::kLast);

  LpuTables t = Tables();
  for (uint64_t i = 0; i < kChipNum; ++i) {
    uint64_t gx = t.grid[i].gx;
    uint64_t want = (gx == 0 || gx + 1 == kGridX) ? 10 : 8;
    EXPECT_EQ(CoresOf(t.chip_shape[i]), want) << "chip=" << i;
  }
}

TEST(BachLpuGrid, CoreCountIsFourHundredThirtyTwo) {
  // 中间两列每 chip 8 个，两侧每 chip 10 个；全 LPU 432 个 core，其中 384 个
  // 计算 core，每颗 chip 都是 8 个。
  LpuTables t = Tables();
  uint64_t cores = 0, compute = 0;
  for (uint64_t i = 0; i < kChipNum; ++i) {
    uint64_t n = CoresOf(t.chip_shape[i]);
    cores += n;
    uint64_t local = 0;
    for (uint64_t c = 0; c < n; ++c) {
      if (t.logical_map[i][c].role == CoreRole::kCompute) ++local;
    }
    EXPECT_EQ(local, 8u) << "chip=" << i;
    compute += local;
  }
  EXPECT_EQ(cores, 432u);
  EXPECT_EQ(compute, 384u);
}

TEST(BachLpuGrid, SpecialCoresSitAtFixedPositions) {
  // 专用 core 的位置固定，不用搜：第一列 chip 的 core0 是 EP broadcast，最后
  // 一列 chip 的 core9 是 EP reduction。
  LpuTables t = Tables();
  for (uint64_t gy = 0; gy < kGridY; ++gy) {
    uint64_t first = ChipIdOf(0, gy);
    EXPECT_EQ(t.logical_map[first][0].role, CoreRole::kBroadcast);
    EXPECT_EQ(t.logical_map[first][5].role, CoreRole::kSpare);
    uint64_t last = ChipIdOf(kGridX - 1, gy);
    EXPECT_EQ(t.logical_map[last][9].role, CoreRole::kReduce);
    EXPECT_EQ(t.logical_map[last][4].role, CoreRole::kSpare);
  }
}

TEST(BachLpuGrid, SpecialCoresAreNotLogicalComputeCores) {
  // 专用 core 不映射为 logical compute core：逻辑 0～7 各出现一次，专用那个
  // 拿逻辑 8，不派角色的那个没有逻辑编号。
  LpuTables t = Tables();
  for (uint64_t i = 0; i < kChipNum; ++i) {
    std::set<uint64_t> logical;
    for (uint64_t c = 0; c < CoresOf(t.chip_shape[i]); ++c) {
      LogicalEntry const& e = t.logical_map[i][c];
      if (e.role == CoreRole::kCompute) {
        EXPECT_TRUE(e.mapped);
        EXPECT_LT(e.logical, 8u);
        EXPECT_TRUE(logical.insert(e.logical).second) << "chip=" << i;
      } else if (e.role == CoreRole::kSpare) {
        EXPECT_FALSE(e.mapped) << "chip=" << i;
      } else {
        EXPECT_EQ(e.logical, 8u) << "chip=" << i;
      }
    }
    EXPECT_EQ(logical.size(), 8u) << "chip=" << i;
  }
}

TEST(BachLpuGrid, CrossTableChecksCatchABadTable) {
  // 跨表自洽检查真的在查：把一颗 chip 的形状改错，检查应当拦下来。
  LpuTables t = Tables();
  CheckLpuTables(t);
  t.chip_shape[ChipIdOf(1, 0)] = ChipShape::kFirst;
  EXPECT_DEATH(CheckLpuTables(t), "");
}

}  // namespace

namespace {

// ── 装配 ──

TEST(BachLpu, BuildsFortyEightChips) {
  // 48 颗 chip 构造出来，432 个 core，其中 384 个计算 core。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  Lpu lpu(clk, "lpu", Tables());

  EXPECT_EQ(lpu.ChipCount(), 48u);
  EXPECT_EQ(lpu.CoreCount(), 432u);
  EXPECT_EQ(lpu.ComputeCoreCount(), 384u);
  RT::Reset();
}

TEST(BachLpu, RunsCycles) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  Lpu lpu(clk, "lpu", Tables());
  LpuDriver driver(clk, lpu);
  clk->Continue(20 * kPeriod);
  RT::JoinAll();
  EXPECT_TRUE(lpu.Quiescent()) << "没有注入，48 颗 chip 应当一直空着";
  RT::Reset();
}

TEST(BachLpu, RowsAndColumnsAreDirectLinks) {
  // 同层左右、同列上下都是 chip 到 chip 的直连，不经 PCIe Switch：同层每行
  // 3 条共 36 条，同列每列 11 条共 44 条。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  Lpu lpu(clk, "lpu", Tables());

  uint64_t row = 0, col = 0;
  for (C2cPair const& p : lpu.C2cLinks()) {
    uint64_t agx = GxOfChip(p.a_chip), agy = GyOfChip(p.a_chip);
    uint64_t bgx = GxOfChip(p.b_chip), bgy = GyOfChip(p.b_chip);
    if (agy == bgy) {
      // (gx, gy) 的 E 口与 (gx + 1, gy) 的 W 口对接。
      EXPECT_EQ(bgx, agx + 1);
      EXPECT_EQ(p.a_port, uint64_t(kChipE));
      EXPECT_EQ(p.b_port, uint64_t(kChipW));
      ++row;
    } else {
      // (gx, gy) 的 S 口与 (gx, gy + 1) 的 N 口对接。
      EXPECT_EQ(agx, bgx);
      EXPECT_EQ(bgy, agy + 1);
      EXPECT_EQ(p.a_port, uint64_t(kChipS));
      EXPECT_EQ(p.b_port, uint64_t(kChipN));
      ++col;
    }
  }
  EXPECT_EQ(row, 36u);
  EXPECT_EQ(col, 44u);
  RT::Reset();
}

TEST(BachLpu, CrossTrayLinksTakeVerticalParams) {
  // 上一 tray 最底层 chip 的 S 口接下一 tray 最顶层 chip 的 N 口，链路参数另
  // 取：tray 内那些用 PCIe C2C，跨 tray 的 8 条用纵向参数。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  Lpu lpu(clk, "lpu", Tables());

  uint64_t cross = 0;
  for (C2cPair const& p : lpu.C2cLinks()) {
    uint64_t agy = GyOfChip(p.a_chip), bgy = GyOfChip(p.b_chip);
    if (agy == bgy) continue;
    if (TrayOf(agy) == TrayOf(bgy)) {
      EXPECT_EQ(p.params.latency, LinkPcieC2C().latency);
      continue;
    }
    EXPECT_EQ(LayerOf(agy), kLayerPerTray - 1) << "上一 tray 的最底层";
    EXPECT_EQ(LayerOf(bgy), 0u) << "下一 tray 的最顶层";
    EXPECT_EQ(p.params.latency, LinkTrayVertical().latency);
    EXPECT_EQ(p.params.bandwidth, LinkTrayVertical().bandwidth);
    ++cross;
  }
  EXPECT_EQ(cross, (kTrayNum - 1) * kGridX) << "两处纵向，每处 4 列";
  RT::Reset();
}

TEST(BachLpu, EdgePortsGoToTraySwitches) {
  // 每层最左最右两颗 chip 的边缘口接本 tray 那一侧的 PCIe Switch，一个 Switch
  // 接两层。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  Lpu lpu(clk, "lpu", Tables());

  std::map<uint64_t, std::set<uint64_t>> layers_of;
  EXPECT_EQ(lpu.EdgeLinks().size(), kGridY * 2);
  for (EdgePair const& e : lpu.EdgeLinks()) {
    uint64_t gx = GxOfChip(e.chip), gy = GyOfChip(e.chip);
    EXPECT_TRUE(gx == 0 || gx + 1 == kGridX) << "只有两端的 chip 有边缘口";
    EXPECT_EQ(e.chip_port, gx == 0 ? uint64_t(kChipW) : uint64_t(kChipE));
    // Switch 与它接的 chip 在同一个 tray。
    EXPECT_EQ(e.sw / kSwitchPerTray, TrayOf(gy));
    EXPECT_EQ(e.params.latency, LinkPcieRouterLr().latency);
    layers_of[e.sw].insert(gy);
  }
  EXPECT_EQ(layers_of.size(), kSwitchNum) << "12 个 Switch 都接上了";
  for (auto const& kv : layers_of) {
    EXPECT_EQ(kv.second.size(), 2u) << "一个 Switch 接两层，sw=" << kv.first;
  }
  RT::Reset();
}

TEST(BachLpu, StubsHangOnTheCornerSwitches) {
  // 外部数据从 global_top_left 西侧进，结果从 global_bottom_right 东侧出，
  // 两个桩各挂那一侧 Switch 的第三个端口。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  Lpu lpu(clk, "lpu", Tables());

  ASSERT_EQ(lpu.ExtLinks().size(), 2u);
  EdgePair const& in_e = lpu.ExtLinks()[0];
  EXPECT_EQ(in_e.chip, ChipIdOf(0, 0));
  EXPECT_EQ(in_e.chip_port, uint64_t(kChipW)) << "从西侧进";
  EXPECT_EQ(in_e.sw, SwitchOfEdge(0, 0));
  EXPECT_EQ(in_e.sw_port, kSwitchExtPort);

  EdgePair const& out_e = lpu.ExtLinks()[1];
  EXPECT_EQ(out_e.chip, ChipIdOf(kGridX - 1, kGridY - 1));
  EXPECT_EQ(out_e.chip_port, uint64_t(kChipE)) << "从东侧出";
  EXPECT_EQ(out_e.sw, SwitchOfEdge(kGridX - 1, kGridY - 1));
  EXPECT_EQ(out_e.sw_port, kSwitchExtPort);
  RT::Reset();
}

TEST(BachLpu, FirstColumnRunsTopDown) {
  // LPU 广播那条路的拓扑一半：token 只送进左上角第一个 B core，往下沿第一列
  // 一颗接一颗，跨 tray 的两处走纵向链路。这里查这条链在不在，跑不跑得起来是
  // 端到端那一步的事。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  Lpu lpu(clk, "lpu", Tables());

  std::set<uint64_t> down;
  for (C2cPair const& p : lpu.C2cLinks()) {
    if (GxOfChip(p.a_chip) != 0 || GxOfChip(p.b_chip) != 0) continue;
    if (GyOfChip(p.a_chip) + 1 != GyOfChip(p.b_chip)) continue;
    down.insert(GyOfChip(p.a_chip));
  }
  EXPECT_EQ(down.size(), kGridY - 1) << "第一列自上而下一段不缺";
  // partial result 沿最后一列自上而下，走的是同一套同列直连。
  std::set<uint64_t> partial;
  for (C2cPair const& p : lpu.C2cLinks()) {
    if (GxOfChip(p.a_chip) + 1 != kGridX) continue;
    if (GxOfChip(p.b_chip) + 1 != kGridX) continue;
    if (GyOfChip(p.a_chip) + 1 != GyOfChip(p.b_chip)) continue;
    partial.insert(GyOfChip(p.a_chip));
  }
  EXPECT_EQ(partial.size(), kGridY - 1) << "最后一列自上而下一段不缺";
  RT::Reset();
}

TEST(BachLpu, SpareCoreSitsOnTheSwitchPort) {
  // 不派角色的 core 只构造 Router，坐在 chip 接 PCIe Switch 的那个口上：第一
  // 列的 core5 在 W 口，最后一列的 core4 在 E 口。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  Lpu lpu(clk, "lpu", Tables());

  for (uint64_t gy = 0; gy < kGridY; ++gy) {
    Chip& first = lpu.GetChip(0, gy);
    EXPECT_TRUE(first.GetCore(5).Context().router_only) << "gy=" << gy;
    EXPECT_EQ(first.GetCore(0).Context().role, CoreRole::kBroadcast);
    Chip& last = lpu.GetChip(kGridX - 1, gy);
    EXPECT_TRUE(last.GetCore(4).Context().router_only) << "gy=" << gy;
    EXPECT_EQ(last.GetCore(9).Context().role, CoreRole::kReduce);
  }
  RT::Reset();
}


TEST(BachLpu, SwitchKnowsOnlyItsOwnChips) {
  // Switch 之间不互联：一个 Switch 只认它接的那两颗 chip，出口桩这个目的只有
  // 右下角那个 Switch 认得。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  Lpu lpu(clk, "lpu", Tables());

  std::map<uint64_t, std::set<uint64_t>> chips_of;
  for (EdgePair const& e : lpu.EdgeLinks()) chips_of[e.sw].insert(e.chip);

  uint64_t out_sw = SwitchOfEdge(kGridX - 1, kGridY - 1);
  for (uint64_t s = 0; s < kSwitchNum; ++s) {
    for (uint64_t chip = 0; chip < kChipNum; ++chip) {
      bool mine = chips_of[s].count(chip) != 0;
      EXPECT_EQ(lpu.Switch(s).HasRoute(chip), mine)
          << "sw=" << s << " chip=" << chip;
    }
    EXPECT_EQ(lpu.Switch(s).HasRoute(kNodeOutStub), s == out_sw) << "sw=" << s;
  }
  // 边缘口那两条路由指的是各自的端口。
  for (EdgePair const& e : lpu.EdgeLinks()) {
    std::vector<uint64_t> const& ports = lpu.Switch(e.sw).RouteOf(e.chip);
    ASSERT_EQ(ports.size(), 1u) << "组播关着，一个目的只配一个端口";
    EXPECT_EQ(ports[0], e.sw_port);
  }
  RT::Reset();
}

// ── chip 边缘口与 PCIe Switch 之间的折算 ──

// 桥、口与两侧的灌注读出都由这一个协程驱动：出口那根线是单拍脉冲，分到两个
// 协程里会整拍错过。
class PortHarness : public BachModule {
 public:
  PortHarness(ClockPtr c, C2cBridge& bridge, SwitchPort& port,
              LinkEndPtr up, LinkEndPtr down)
      : BachModule(c, "port_harness"), br(bridge), sp(port),
        to_sw(std::move(up)), from_sw(std::move(down)) {}

  // 往 core 侧灌一个整包。
  void Send(uint64_t bytes, MessagePtr pkt) {
    out_bytes = bytes;
    out_msg = std::move(pkt);
  }
  // 从 Switch 那一侧送一个 flit 进来。
  void Recv(uint64_t bytes, MessagePtr pkt) {
    in_bytes = bytes;
    in_msg = std::move(pkt);
  }

  uint64_t up_flits = 0, up_bytes = 0;
  MessagePtr up_msg;
  uint64_t core_flits = 0, core_bytes = 0;
  MessagePtr core_msg;

 protected:
  void Step() override {
    // 末级先做：先把上一拍驱到两侧的读下来。
    FlitView up = ReadFlit(to_sw->flit);
    if (up.valid) {
      ++up_flits;
      up_bytes = up.bytes;
      up_msg = up.msg;
    }
    FlitView core = ReadFlit(br.ToCore()->flit);
    if (core.valid) {
      ++core_flits;
      core_bytes = core.bytes;
      core_msg = core.msg;
    }

    Feed();
    br.RunStep();
    sp.Move();
  }

 private:
  void Feed() {
    LinkEndPtr wire = br.FromCore();
    if (out_msg) {
      wire->flit.Drive(/*vc=*/1, /*is_head=*/true, /*is_tail=*/true, out_bytes,
                       out_msg);
      out_msg.reset();
    } else {
      wire->flit.Idle();
    }
    wire->release.Idle();

    if (in_msg) {
      from_sw->flit.Drive(/*vc=*/1, /*is_head=*/true, /*is_tail=*/true,
                          in_bytes, in_msg);
      in_msg.reset();
    } else {
      from_sw->flit.Idle();
    }
    from_sw->release.Idle();
  }

  C2cBridge& br;
  SwitchPort& sp;
  LinkEndPtr to_sw, from_sw;
  uint64_t out_bytes = 0, in_bytes = 0;
  MessagePtr out_msg, in_msg;
};

TEST(BachSwitchPort, FoldsSegmentsIntoFlitsAndBack) {
  // 出去的两拍合成一个 flit，进来的一个 flit 摊成两拍：一个包从 core 侧进去，
  // 在 Switch 那一侧只出现一次；反过来从 Switch 那一侧进来一个 flit，core 侧
  // 收到的还是原来那一个。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  C2cCfg cfg;
  // 这一段的时间由 Link 实例承担，桥的 AXI 段计 0。
  cfg.axi_latency = 0;
  C2cBridge br(clk, "br", cfg);
  LinkEndPtr to_sw = MakeWire(clk), from_sw = MakeWire(clk);
  SwitchPort port(br, to_sw, from_sw);
  PortHarness harness(clk, br, port, to_sw, from_sw);

  auto out_msg = std::make_shared<Message>();
  out_msg->user_id = 7;
  auto in_msg = std::make_shared<Message>();
  in_msg->user_id = 9;
  harness.Send(320, out_msg);
  harness.Recv(256, in_msg);

  clk->Continue(60 * kPeriod);
  RT::JoinAll();

  EXPECT_EQ(harness.up_flits, 1u) << "一段两拍，只该在 Switch 那侧出现一次";
  EXPECT_EQ(harness.up_bytes, 320u);
  ASSERT_TRUE(harness.up_msg);
  EXPECT_EQ(harness.up_msg->user_id, 7u);

  EXPECT_EQ(harness.core_flits, 1u);
  EXPECT_EQ(harness.core_bytes, 256u);
  ASSERT_TRUE(harness.core_msg);
  EXPECT_EQ(harness.core_msg->user_id, 9u);
  RT::Reset();
}

}  // namespace
