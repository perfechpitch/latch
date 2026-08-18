// 路由器的行为基线：往哪走、怎么切、怎么攒、出口怎么排队。
//
// 每个用例搭一小片拓扑，从某个路由器的本地口注入一拍数据，看它落到哪里、分成几片、
// 花了多少拍。观察量都是路由器自己的诊断计数与到达时刻，不去翻它的内部容器。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/router/router.h"
#include "bach/ip/route_config.h"
#include "bach/ip/wiring.h"
#include "fixture.h"

using namespace latch;
using namespace latch::bach;
using latch::bach::test::CoreTables;
using latch::bach::test::StepDriver;

namespace {

constexpr Time kPeriod = 1;

// 收下就记一笔，不再往下走。挂在某个路由器的本地口上当终点用。
class Landing : public PacketTarget {
 public:
  void HandleComm(CommInstPtr const& payload) override {
    landed.push_back({payload, RT::Now()});
  }

  struct Item {
    CommInstPtr payload;
    Time at = 0;
  };
  std::vector<Item> landed;
};

CommInstPtr MakeBeat(uint64_t uid, uint64_t tid, uint64_t beat_id,
                     uint64_t total) {
  CommInstPtr c = std::make_shared<CommInst>();
  c->uid = uid;
  c->tid = tid;
  c->opcode = Opcode::kMove;
  c->beat_id = beat_id;
  c->total_fragments = total;
  c->xfer_id = 1000 + beat_id;
  return c;
}

Dim MakeDim(uint32_t chip_rows, uint32_t chip_cols, uint32_t core_rows,
            uint32_t core_cols) {
  Dim d;
  d.chip_rows = chip_rows;
  d.chip_cols = chip_cols;
  d.core_rows_per_chip = core_rows;
  d.core_cols_per_chip = core_cols;
  d.node_chip_rows = 1;
  d.node_chip_cols = 1;
  return d;
}

}  // namespace

// ---------------------------------------------------------------- 路由

TEST(BachRouter, GoesAlongTheColumnBeforeTheRow) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  RouteConfig cfg;
  cfg.dim = MakeDim(1, 1, 2, 2);

  // 一块 2x2 的阵列，从左上角发往右下角
  Router a(clk, tb.Context(), Coord{0, 0}, &cfg, "a", 0);
  Router b(clk, tb.Context(), Coord{0, 1}, &cfg, "b", 0);
  Router c(clk, tb.Context(), Coord{1, 0}, &cfg, "c", 0);
  Router d(clk, tb.Context(), Coord{1, 1}, &cfg, "d", 0);
  Landing sink_a, sink_b, sink_c, sink_d;
  a.Connect(&sink_a);
  b.Connect(&sink_b);
  c.Connect(&sink_c);
  d.Connect(&sink_d);

  AttachLink(a, Port::kEast, b);
  AttachLink(a, Port::kSouth, c);
  AttachLink(b, Port::kSouth, d);
  AttachLink(c, Port::kEast, d);

  StepDriver drv(clk, [&](Time now, uint64_t k) {
    a.Step();
    b.Step();
    c.Step();
    d.Step();
    if (k == 0) a.Inject(MakeBeat(1, 0, 0, 1), Coord{1, 1}, 128);
  });

  clk->Continue(400 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink_d.landed.size(), 1u);
  // 先走列后走行：包从东边过去，南边那条线没被用过
  EXPECT_GT(b.ForwardedFragments(), 0u);
  EXPECT_EQ(c.ServedBeats(), 0u);
  EXPECT_EQ(d.DeliveredBeats(), 1u);
}

TEST(BachRouter, HeadsForTheChipGatewayBeforeLeavingTheChip) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  RouteConfig cfg;
  // 两块 chip，每块 1x2 个核。全局是一行四列
  cfg.dim = MakeDim(1, 2, 1, 2);
  // 左边那块 chip 朝右的出口定在片内第 0 个核，也就是全局 (0,0)
  cfg.AddChipGateway(0, Direction::kRight, Coord{0, 0});

  Router a(clk, tb.Context(), Coord{0, 0}, &cfg, "a", 0);
  Router b(clk, tb.Context(), Coord{0, 1}, &cfg, "b", 0);
  Router c(clk, tb.Context(), Coord{0, 2}, &cfg, "c", 0);
  Landing sink_a, sink_b, sink_c;
  a.Connect(&sink_a);
  b.Connect(&sink_b);
  c.Connect(&sink_c);

  AttachLink(a, Port::kEast, b);
  // 出 chip 那一跳只有 PCIe 走得通，而且它挂在网关核上
  AttachLink(a, Port::kPcieEast, c);

  StepDriver drv(clk, [&](Time now, uint64_t k) {
    a.Step();
    b.Step();
    c.Step();
    // 从片内非网关的核发往另一块 chip：要先回到网关核，再从那里出去
    if (k == 0) b.Inject(MakeBeat(1, 0, 0, 1), Coord{0, 2}, 128);
  });

  // 出 chip 那一跳按跨 chip 算，四百拍，加上两跳的线延迟
  clk->Continue(2000 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink_c.landed.size(), 1u);
  // 目标在东边，但先往西回到网关核，再从网关核的 PCIe 口出去
  EXPECT_GT(a.ForwardedFragments(), 0u);
  EXPECT_EQ(c.DeliveredBeats(), 1u);
}

TEST(BachRouter, GatewayCoreEjectsToTheExternalNode) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  RouteConfig cfg;
  cfg.dim = MakeDim(1, 1, 1, 1);
  const Coord ext_coord{0, -1};
  cfg.AddExtNode(ext_coord, Coord{0, 0}, Port::kPcieWest);

  Router gateway(clk, tb.Context(), Coord{0, 0}, &cfg, "gateway", 0);
  Router outside(clk, tb.Context(), ext_coord, &cfg, "outside", 0);
  Landing sink_gateway, sink_outside;
  gateway.Connect(&sink_gateway);
  outside.Connect(&sink_outside);
  AttachLink(gateway, Port::kPcieWest, outside);

  EXPECT_TRUE(gateway.IsInternal());
  EXPECT_FALSE(outside.IsInternal());

  StepDriver drv(clk, [&](Time now, uint64_t k) {
    gateway.Step();
    outside.Step();
    if (k == 0) gateway.Inject(MakeBeat(1, 0, 0, 1), ext_coord, 128);
  });

  clk->Continue(20000 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink_outside.landed.size(), 1u);
  EXPECT_EQ(sink_gateway.landed.size(), 0u);
}

// ---------------------------------------------------------------- 切分与重组

TEST(BachRouter, SplitsOneBeatToTheNextLinkBandwidth) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  RouteConfig cfg;
  cfg.dim = MakeDim(1, 1, 1, 2);

  Router a(clk, tb.Context(), Coord{0, 0}, &cfg, "a", 0);
  Router b(clk, tb.Context(), Coord{0, 1}, &cfg, "b", 0);
  Landing sink_a, sink_b;
  a.Connect(&sink_a);
  b.Connect(&sink_b);
  // 这条线一次只走得动 128 字节
  AttachLink(a, Port::kEast, b, 128, 0);

  StepDriver drv(clk, [&](Time now, uint64_t k) {
    a.Step();
    b.Step();
    if (k == 0) a.Inject(MakeBeat(1, 0, 0, 1), Coord{0, 1}, 512);
  });

  clk->Continue(400 * kPeriod);
  RT::JoinAll();

  // 512 字节按 128 的带宽切成四片
  EXPECT_EQ(a.ForwardedFragments(), 4u);
  // 终点把四片攒回一拍，只往上交一次
  ASSERT_EQ(sink_b.landed.size(), 1u);
  EXPECT_EQ(b.DeliveredBeats(), 1u);
}

TEST(BachRouter, ReassemblesFragmentsBeforeForwardingThem) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  RouteConfig cfg;
  cfg.dim = MakeDim(1, 1, 1, 3);

  Router a(clk, tb.Context(), Coord{0, 0}, &cfg, "a", 0);
  Router b(clk, tb.Context(), Coord{0, 1}, &cfg, "b", 0);
  Router c(clk, tb.Context(), Coord{0, 2}, &cfg, "c", 0);
  Landing sink_a, sink_b, sink_c;
  a.Connect(&sink_a);
  b.Connect(&sink_b);
  c.Connect(&sink_c);
  // 前一段窄后一段宽：中间那个路由器要先把切碎的片攒回整拍，才好按自己的带宽重切
  AttachLink(a, Port::kEast, b, 128, 0);
  AttachLink(b, Port::kEast, c, 512, 0);

  StepDriver drv(clk, [&](Time now, uint64_t k) {
    a.Step();
    b.Step();
    c.Step();
    if (k == 0) a.Inject(MakeBeat(1, 0, 0, 1), Coord{0, 2}, 512);
  });

  clk->Continue(400 * kPeriod);
  RT::JoinAll();

  EXPECT_EQ(a.ForwardedFragments(), 4u);
  // 攒齐之后按宽带宽走，只发一片出去
  EXPECT_EQ(b.ForwardedFragments(), 1u);
  EXPECT_EQ(b.ServedBeats(), 4u);
  ASSERT_EQ(sink_c.landed.size(), 1u);
}

// ---------------------------------------------------------------- 出口

TEST(BachRouter, OneExitSerializesEveryPort) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  RouteConfig cfg;
  cfg.dim = MakeDim(1, 1, 1, 2);

  Router a(clk, tb.Context(), Coord{0, 0}, &cfg, "a", 0);
  Router b(clk, tb.Context(), Coord{0, 1}, &cfg, "b", 0);
  Landing sink_a, sink_b;
  a.Connect(&sink_a);
  b.Connect(&sink_b);
  AttachLink(a, Port::kEast, b, 128, 0);

  StepDriver drv(clk, [&](Time now, uint64_t k) {
    a.Step();
    b.Step();
    if (k == 0) {
      // 同一拍交两拍数据，它们抢的是同一个出口
      a.Inject(MakeBeat(1, 0, 0, 1), Coord{0, 1}, 128);
      a.Inject(MakeBeat(2, 0, 0, 1), Coord{0, 1}, 128);
    }
  });

  clk->Continue(400 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink_b.landed.size(), 2u);
  // 一个路由器每拍只发得出去一片，所以后一拍数据整整晚一拍到
  EXPECT_EQ(sink_b.landed[1].at - sink_b.landed[0].at, 1u);
  EXPECT_EQ(a.ServedBeats(), 2u);
}
