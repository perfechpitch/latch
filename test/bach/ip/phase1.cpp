// Phase1 那一段：一条通道分岔成两路，两个落点各收一路，收齐了交给以太网交换节点。
//
// 用例搭的是最小一片：通道与两个落点都挂在同一个核上，包在核里中转一跳。中间那个
// 核只出一个路由器，因为这里关心的是通道的分段与落点的收齐，不是核内的流水。

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/router/router.h"
#include "bach/ip/eth_switch/eth_switch.h"
#include "bach/ip/external/phase1_lane.h"
#include "bach/ip/external/phase1_sink.h"
#include "bach/ip/route_config.h"
#include "bach/ip/wiring.h"
#include "fixture.h"

using namespace latch;
using namespace latch::bach;
using latch::bach::test::CoreTables;
using latch::bach::test::StepDriver;

namespace {

constexpr Time kPeriod = 1;
constexpr uint64_t kVolume = 128;
constexpr uint64_t kBandwidth = 128;
// 链路延迟要显式给。不给就按跨 node 算，那是五千拍，用例跑不完。
constexpr uint64_t kLinkDelay = 10;

Dim OneCore() {
  Dim d;
  d.chip_rows = 1;
  d.chip_cols = 1;
  d.core_rows_per_chip = 1;
  d.core_cols_per_chip = 1;
  d.node_chip_rows = 1;
  d.node_chip_cols = 1;
  return d;
}

Phase1LaneSpec PlainSpec() {
  Phase1LaneSpec s;
  s.layer_id = 0;
  s.group_id = 2;
  s.bypass_target = Coord{-1, 1};
  s.moe_target = Coord{-1, 2};
  s.has_bypass = true;
  s.has_moe = true;
  s.bypass_volume = kVolume;
  s.moe_volume = kVolume;
  s.fc0_delay = 5;
  s.res_delay = 5;
  s.norm_delay = 5;
  s.router_delay = 5;
  s.push_delay = 10;
  s.hit_map = {0, 1, 17};
  return s;
}

// 一条通道、两个落点、一个交换节点，全挂在同一个核上。
struct Rig {
  CoreTables tb;
  RouteConfig cfg;
  ClockPtr clk;
  std::unique_ptr<Router> hub;
  std::unique_ptr<Phase1Lane> lane;
  std::unique_ptr<Phase1Sink> bypass;
  std::unique_ptr<Phase1Sink> moe;
  std::unique_ptr<EthSwitch> eth;
  std::unique_ptr<StepDriver> drv;

  explicit Rig(Phase1LaneSpec const& spec, std::vector<uint64_t> uids) {
    // 五个挂时钟的模块：通道、两个落点、交换节点，加上驱动中转那个核的
    RT::Reset(3, 3);
    clk = MakeClock(0, kPeriod);
    cfg.dim = OneCore();
    cfg.AddExtNode(Coord{-1, 0}, Coord{0, 0}, Port::kPcieWest);
    cfg.AddExtNode(Coord{-1, 1}, Coord{0, 0}, Port::kPcieNorth);
    cfg.AddExtNode(Coord{-1, 2}, Coord{0, 0}, Port::kPcieSouth);

    hub = std::make_unique<Router>(clk, tb.Context(), Coord{0, 0}, &cfg, "hub",
                                   0);
    lane = std::make_unique<Phase1Lane>(clk, tb.params, 10, Coord{-1, 0}, &cfg,
                                        spec, kBandwidth, "lane", 0);
    bypass = std::make_unique<Phase1Sink>(clk, tb.params, 11, Coord{-1, 1},
                                          &cfg, SinkRole::kBypass, kVolume,
                                          kBandwidth, "bypass", 0);
    moe = std::make_unique<Phase1Sink>(clk, tb.params, 12, Coord{-1, 2}, &cfg,
                                       SinkRole::kMoe, kVolume, kBandwidth,
                                       "moe", 0);
    eth = std::make_unique<EthSwitch>(clk, tb.params, 13,
                                      EthSwitchConfig::Default(), "eth", 0);

    AttachLink(*hub, Port::kPcieWest, lane->Rt(), kBandwidth, kLinkDelay);
    AttachLink(*hub, Port::kPcieNorth, bypass->Rt(), kBandwidth, kLinkDelay);
    AttachLink(*hub, Port::kPcieSouth, moe->Rt(), kBandwidth, kLinkDelay);
    bypass->ConnectEth(&eth->OpenIngress(EthPort::kResIngress));
    moe->ConnectEth(&eth->OpenIngress(EthPort::kPhase1MoeIngress));

    lane->SetLaneId(4);
    lane->SetUsers(std::move(uids));
    Router* r = hub.get();
    drv = std::make_unique<StepDriver>(clk, [r](Time, uint64_t) { r->Step(); });
  }

  void Run(uint64_t cycles) {
    clk->Continue(cycles * kPeriod);
    RT::JoinAll();
  }
};

}  // namespace

// 一个 user 走完一条通道：两路各发一份，两个落点各收一份。
TEST(BachPhase1Lane, SplitsOneUserIntoTwoBranches) {
  Rig rig(PlainSpec(), {7});
  rig.Run(200);

  EXPECT_EQ(rig.lane->PushedNum(), 1u);
  EXPECT_EQ(rig.lane->SentBypass(), 1u);
  EXPECT_EQ(rig.lane->SentMoe(), 1u);
  EXPECT_EQ(rig.bypass->LandedNum(), 1u);
  EXPECT_EQ(rig.moe->LandedNum(), 1u);
}

// 分岔的两份带的东西不同：残差那份不带专家，MoE 那份带，tag 是专家个数。
TEST(BachPhase1Lane, OnlyTheMoeBranchCarriesTheExperts) {
  Rig rig(PlainSpec(), {7});
  rig.Run(2000);

  ASSERT_EQ(rig.eth->Deliveries().size(), 2u);
  for (EthDelivery const& d : rig.eth->Deliveries()) {
    if (d.role == EthRole::kResidual) {
      EXPECT_TRUE(d.out.packet->hit_map.empty());
      EXPECT_EQ(d.out.semantic, HitMapSemantic::kNone);
    } else {
      EXPECT_EQ(d.role, EthRole::kMoeRequest);
      EXPECT_EQ(d.out.expert_hit_map, (std::vector<uint32_t>{0, 1, 17}));
      EXPECT_EQ(d.out.semantic, HitMapSemantic::kGroupId);
    }
  }
}

// 两路都带着这次工作的身份：哪条通道、哪个 chip 组、第几层。汇合点只认这几样。
TEST(BachPhase1Lane, BothBranchesCarryThePhaseIdentity) {
  Rig rig(PlainSpec(), {7});
  rig.Run(2000);

  ASSERT_EQ(rig.eth->Deliveries().size(), 2u);
  for (EthDelivery const& d : rig.eth->Deliveries()) {
    EXPECT_EQ(d.out.packet->phase1_lane_id, 4);
    EXPECT_EQ(d.out.packet->phase1_group_id, 2);
    EXPECT_EQ(d.out.packet->layer_id, 0u);
    EXPECT_EQ(d.out.packet->uid, 7u);
  }
}

// 残差先发、MoE 后发，两段延迟按顺序扣：FC0 与残差算完才发第一份，归一化与选路
// 算完才发第二份。
TEST(BachPhase1Lane, TheResidualLeavesBeforeTheMoeRequest) {
  Rig rig(PlainSpec(), {7});
  rig.Run(200);

  std::vector<UnitSpan> compute;
  for (UnitSpan const& s : rig.lane->Recorder().Spans()) {
    if (s.unit == Unit::kLane && s.state == SpanState::kCompute) {
      compute.push_back(s);
    }
  }
  ASSERT_EQ(compute.size(), 2u);
  // 第一个 user 在第 10 拍进来，两段各十拍
  EXPECT_EQ(compute[0].start, 10u);
  EXPECT_EQ(compute[0].end, 20u);
  EXPECT_EQ(compute[1].end - compute[1].start, 10u);
  EXPECT_GT(compute[1].start, compute[0].end);
}

// 一条通道同时只处理一个 user，后面的排队。产 user 只看推包间隔，不等前一个走完。
TEST(BachPhase1Lane, OneUserAtATimeWhileTheRestQueueUp) {
  Phase1LaneSpec spec = PlainSpec();
  spec.push_delay = 2;   // 产得比处理快
  spec.fc0_delay = 20;
  spec.res_delay = 0;
  spec.norm_delay = 20;
  spec.router_delay = 0;
  Rig rig(spec, {1, 2, 3});
  rig.Run(400);

  EXPECT_EQ(rig.lane->PushedNum(), 3u);
  EXPECT_EQ(rig.lane->SentMoe(), 3u);
  EXPECT_EQ(rig.bypass->LandedNum(), 3u);
  EXPECT_EQ(rig.moe->LandedNum(), 3u);

  // 三个 user 的第一段首尾相接，不重叠：通道一次只处理一个
  std::vector<UnitSpan> compute;
  for (UnitSpan const& s : rig.lane->Recorder().Spans()) {
    if (s.unit == Unit::kLane && s.state == SpanState::kCompute) {
      compute.push_back(s);
    }
  }
  ASSERT_EQ(compute.size(), 6u);
  for (size_t i = 1; i < compute.size(); ++i) {
    EXPECT_GE(compute[i].start, compute[i - 1].end);
  }
}

// 落点收齐一整包才往下交，半包不交。
TEST(BachPhase1Sink, HandsOnOnlyWholePackets) {
  Phase1LaneSpec spec = PlainSpec();
  spec.bypass_volume = kBandwidth * 4;  // 四拍才是一整包
  spec.moe_volume = kBandwidth * 4;
  Rig rig(spec, {7});
  rig.Run(200);

  EXPECT_EQ(rig.bypass->LandedNum(), 1u);
  EXPECT_EQ(rig.bypass->HandedOnNum(), 1u);
  EXPECT_EQ(rig.moe->LandedNum(), 1u);
  EXPECT_EQ(rig.moe->HandedOnNum(), 1u);
  EXPECT_EQ(rig.eth->AdmittedNum(), 2u);
}

// 交换节点自己判 Phase1 边界时，两路都投出去了才算一次工作完成。
TEST(BachPhase1Sink, TheBoundaryCountsOneWorkPerUser) {
  Rig rig(PlainSpec(), {1, 2});
  rig.Run(3000);

  EXPECT_EQ(rig.eth->BoundaryCompletions(), 2u);
  EXPECT_EQ(rig.eth->CompletedNum(), 2u);
}
