// PCIe 交换节点的行为基线：一条包按显式路由走到哪、花多少拍、切了几片。
//
// 每个用例搭一小片交换拓扑，从某个核的本地口注入一拍，看它落在哪个核上。交换节点没有
// 默认路由，所以每个用例都要把路由表填全，这本身就是这一层与核阵列最大的不同。

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/router/router.h"
#include "bach/ip/node/pcie_switch.h"
#include "bach/ip/route_config.h"
#include "bach/ip/wiring.h"
#include "fixture.h"

using namespace latch;
using namespace latch::bach;
using latch::bach::test::CoreTables;
using latch::bach::test::StepDriver;

namespace {

constexpr Time kPeriod = 1;
constexpr uint64_t kLinkBandwidth = 128;
constexpr uint64_t kLinkDelay = 20;

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

CommInstPtr MakeBeat(uint64_t uid, uint64_t beat_id, uint64_t total) {
  CommInstPtr c = std::make_shared<CommInst>();
  c->uid = uid;
  c->tid = 0;
  c->opcode = Opcode::kMove;
  c->beat_id = beat_id;
  c->total_fragments = total;
  c->xfer_id = 900 + beat_id;
  return c;
}

Dim RowOfCores(uint32_t cols) {
  Dim d;
  d.chip_rows = 1;
  d.chip_cols = 1;
  d.core_rows_per_chip = 1;
  d.core_cols_per_chip = cols;
  d.node_chip_rows = 1;
  d.node_chip_cols = 1;
  return d;
}

}  // namespace

// ---------------------------------------------------------------- 一跳

TEST(BachPcieSwitch, CarriesAPacketBetweenTwoCores) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  RouteConfig cfg;
  cfg.dim = RowOfCores(2);

  Router a(clk, tb.Context(), Coord{0, 0}, &cfg, "a", 0);
  Router b(clk, tb.Context(), Coord{0, 1}, &cfg, "b", 0);
  Landing sink_a, sink_b;
  a.Connect(&sink_a);
  b.Connect(&sink_b);

  PcieSwitch sw(clk, tb.params, 100, "SW0", Coord{-1, 0}, "sw0", 0);
  const uint32_t pa = sw.AddPort("PA");
  const uint32_t pb = sw.AddPort("PB");
  AttachSwitchLink(a, Port::kPcieEast, sw, pa, kLinkBandwidth, kLinkDelay);
  AttachSwitchLink(b, Port::kPcieWest, sw, pb, kLinkBandwidth, kLinkDelay);
  sw.AddRoute(Coord{0, 1}, pb);
  sw.AddRoute(Coord{0, 0}, pa);
  // 目的地在交换节点的表里，本核就从那个口进交换网
  a.AddSwitchRoute(Coord{0, 1}, Port::kPcieEast);

  StepDriver drv(clk, [&](Time now, uint64_t k) {
    a.Step();
    b.Step();
    if (k == 0) a.Inject(MakeBeat(1, 0, 1), Coord{0, 1}, 128);
  });

  clk->Continue(400 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink_b.landed.size(), 1u);
  EXPECT_EQ(sw.ServedBeats(), 1u);
  EXPECT_EQ(sw.ForwardedFragments(), 1u);
  // 逐段：本核仲裁一拍，出口服务一拍加本地线延迟十拍加端口延迟二十拍到交换节点；
  // 交换节点服务一拍加端口延迟二十拍到对端核；对端核服务一拍加本地线延迟十拍落地
  EXPECT_EQ(sink_b.landed[0].at, 1u + 1 + 10 + 20 + 1 + 20 + 1 + 10);
}

// 交换节点没有几何可依循，一条路由不指名就走不了。这里验的是指名过的确实生效：
// 阵列里两个核本来相邻，走 NoC 一跳就到，登记了交换路由之后改走片外那条。
TEST(BachPcieSwitch, AnExplicitRouteWinsOverTheNeighbourLink) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  RouteConfig cfg;
  cfg.dim = RowOfCores(2);

  Router a(clk, tb.Context(), Coord{0, 0}, &cfg, "a", 0);
  Router b(clk, tb.Context(), Coord{0, 1}, &cfg, "b", 0);
  Landing sink_a, sink_b;
  a.Connect(&sink_a);
  b.Connect(&sink_b);
  AttachLink(a, Port::kEast, b);

  PcieSwitch sw(clk, tb.params, 100, "SW0", Coord{-1, 0}, "sw0", 0);
  const uint32_t pa = sw.AddPort("PA");
  const uint32_t pb = sw.AddPort("PB");
  AttachSwitchLink(a, Port::kPcieEast, sw, pa, kLinkBandwidth, kLinkDelay);
  AttachSwitchLink(b, Port::kPcieWest, sw, pb, kLinkBandwidth, kLinkDelay);
  sw.AddRoute(Coord{0, 1}, pb);
  sw.AddRoute(Coord{0, 0}, pa);
  a.AddSwitchRoute(Coord{0, 1}, Port::kPcieEast);

  StepDriver drv(clk, [&](Time now, uint64_t k) {
    a.Step();
    b.Step();
    if (k == 0) a.Inject(MakeBeat(1, 0, 1), Coord{0, 1}, 128);
  });

  clk->Continue(400 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink_b.landed.size(), 1u);
  EXPECT_EQ(sw.ServedBeats(), 1u);
}

// ---------------------------------------------------------------- 两张表

// 同一个目的地，从不同的口进来可以走不同的出口。这张表压过按目的地那张。
TEST(BachPcieSwitch, TheInputTableWinsOverTheDestinationTable) {
  RT::Reset(3, 3);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  RouteConfig cfg;
  cfg.dim = RowOfCores(2);

  Router a(clk, tb.Context(), Coord{0, 0}, &cfg, "a", 0);
  Router b(clk, tb.Context(), Coord{0, 1}, &cfg, "b", 0);
  Landing sink_a, sink_b;
  a.Connect(&sink_a);
  b.Connect(&sink_b);

  PcieSwitch near(clk, tb.params, 100, "SW0", Coord{-1, 0}, "sw0", 0);
  PcieSwitch far(clk, tb.params, 101, "SW1", Coord{-1, 1}, "sw1", 0);
  const uint32_t n_a = near.AddPort("PA");
  const uint32_t n_b = near.AddPort("PB");
  const uint32_t n_far = near.AddPort("TO_SW1");
  const uint32_t f_near = far.AddPort("FROM_SW0");
  const uint32_t f_b = far.AddPort("PB");

  AttachSwitchLink(a, Port::kPcieEast, near, n_a, kLinkBandwidth, kLinkDelay);
  AttachSwitchLink(b, Port::kPcieWest, near, n_b, kLinkBandwidth, kLinkDelay);
  AttachSwitchLink(near, n_far, far, f_near, kLinkBandwidth, kLinkDelay);
  AttachSwitchLink(b, Port::kPcieNorth, far, f_b, kLinkBandwidth, kLinkDelay);

  // 按目的地那张说走直达，按入口那张说从 A 来的绕一圈
  near.AddRoute(Coord{0, 1}, n_b);
  near.AddInRoute(n_a, Coord{0, 1}, n_far);
  far.AddRoute(Coord{0, 1}, f_b);
  a.AddSwitchRoute(Coord{0, 1}, Port::kPcieEast);

  StepDriver drv(clk, [&](Time now, uint64_t k) {
    a.Step();
    b.Step();
    if (k == 0) a.Inject(MakeBeat(1, 0, 1), Coord{0, 1}, 128);
  });

  clk->Continue(400 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink_b.landed.size(), 1u);
  // 绕的那一圈确实走了：两个交换节点各服务过一次
  EXPECT_EQ(near.ServedBeats(), 1u);
  EXPECT_EQ(far.ServedBeats(), 1u);
}

// ---------------------------------------------------------------- 切与攒

// 上一跳按更窄的带宽切碎的片，转发之前要攒回整拍，再按这一跳的带宽重切。
TEST(BachPcieSwitch, ReassemblesFragmentsBeforeForwardingThem) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  RouteConfig cfg;
  cfg.dim = RowOfCores(2);

  Router a(clk, tb.Context(), Coord{0, 0}, &cfg, "a", 0);
  Router b(clk, tb.Context(), Coord{0, 1}, &cfg, "b", 0);
  Landing sink_a, sink_b;
  a.Connect(&sink_a);
  b.Connect(&sink_b);

  PcieSwitch sw(clk, tb.params, 100, "SW0", Coord{-1, 0}, "sw0", 0);
  const uint32_t pa = sw.AddPort("PA");
  const uint32_t pb = sw.AddPort("PB");
  // 进来那条线窄，出去那条线宽
  AttachSwitchLink(a, Port::kPcieEast, sw, pa, 32, kLinkDelay);
  AttachSwitchLink(b, Port::kPcieWest, sw, pb, kLinkBandwidth, kLinkDelay);
  sw.AddRoute(Coord{0, 1}, pb);
  sw.AddRoute(Coord{0, 0}, pa);
  a.AddSwitchRoute(Coord{0, 1}, Port::kPcieEast);

  StepDriver drv(clk, [&](Time now, uint64_t k) {
    a.Step();
    b.Step();
    if (k == 0) a.Inject(MakeBeat(1, 0, 1), Coord{0, 1}, 128);
  });

  clk->Continue(400 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink_b.landed.size(), 1u);
  // 进来四片，攒成一拍，出去一片
  EXPECT_EQ(sw.ServedBeats(), 4u);
  EXPECT_EQ(sw.ForwardedFragments(), 1u);
  EXPECT_EQ(sw.InTransitFragments(), 0u);
}

// 出口是整个交换节点共用一个：两个包同时到，第二个要等第一个的服务时间走完。
TEST(BachPcieSwitch, OneEgressServesEveryPortInTurn) {
  RT::Reset(3, 3);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  RouteConfig cfg;
  cfg.dim = RowOfCores(3);

  Router a(clk, tb.Context(), Coord{0, 0}, &cfg, "a", 0);
  Router b(clk, tb.Context(), Coord{0, 1}, &cfg, "b", 0);
  Router c(clk, tb.Context(), Coord{0, 2}, &cfg, "c", 0);
  Landing sink_a, sink_b, sink_c;
  a.Connect(&sink_a);
  b.Connect(&sink_b);
  c.Connect(&sink_c);

  PcieSwitch sw(clk, tb.params, 100, "SW0", Coord{-1, 0}, "sw0", 0);
  const uint32_t pa = sw.AddPort("PA");
  const uint32_t pb = sw.AddPort("PB");
  const uint32_t pc = sw.AddPort("PC");
  // 出口每片要八拍，两个包的服务段就分得开
  AttachSwitchLink(a, Port::kPcieEast, sw, pa, kLinkBandwidth, kLinkDelay);
  AttachSwitchLink(b, Port::kPcieEast, sw, pb, kLinkBandwidth, kLinkDelay);
  AttachSwitchLink(c, Port::kPcieWest, sw, pc, 16, kLinkDelay);
  sw.AddRoute(Coord{0, 2}, pc);
  a.AddSwitchRoute(Coord{0, 2}, Port::kPcieEast);
  b.AddSwitchRoute(Coord{0, 2}, Port::kPcieEast);

  StepDriver drv(clk, [&](Time now, uint64_t k) {
    a.Step();
    b.Step();
    c.Step();
    if (k == 0) {
      a.Inject(MakeBeat(1, 0, 1), Coord{0, 2}, 128);
      b.Inject(MakeBeat(2, 0, 1), Coord{0, 2}, 128);
    }
  });

  clk->Continue(800 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink_c.landed.size(), 2u);
  EXPECT_EQ(sw.ServedBeats(), 2u);
  // 两个包在交换节点上是排队走的，出口每次切八片各占一拍
  EXPECT_EQ(sw.ForwardedFragments(), 16u);
  EXPECT_GT(sink_c.landed[1].at, sink_c.landed[0].at);
  EXPECT_EQ(sink_c.landed[1].at - sink_c.landed[0].at, 8u);
}
