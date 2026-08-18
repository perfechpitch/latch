// 以太网交换节点的时间账、HitMap 语义换算，以及 Phase2 那道口子的信封校验。
//
// 这里不拉起整套硬件：交换节点只认“有人往某个入口交了一包”，所以驱动直接往它的
// 入口推，产出从它的投递记录上读。逐段时间都手算得出来，用它对时间账。

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/module.h"
#include "base/runtime.h"
#include "bach/ip/eth_switch/eth_switch.h"
#include "bach/ip/eth_switch/phase2_ingress.h"
#include "bach/ip/eth_switch/phase3_join.h"

namespace latch {
namespace bach {
namespace {

// 一包的默认长度：头 128 加载荷 6144 乘 2，与 Bach 的取值一致。
constexpr uint64_t kPacketBytes = 128 + 6144 * 2;

EthSwitchConfig PlainConfig() {
  EthSwitchConfig c = EthSwitchConfig::Default();
  c.processing_delay = 200;
  return c;
}

CommInstPtr MakePacket(uint64_t uid, std::vector<uint32_t> hit_map,
                       int64_t lane = 3, int64_t group = 1,
                       uint64_t layer = 0) {
  CommInstPtr c = std::make_shared<CommInst>();
  c->uid = uid;
  c->tid = 0;
  c->opcode = Opcode::kUserInit;
  c->layer_id = layer;
  c->hit_map = std::move(hit_map);
  c->phase1_lane_id = lane;
  c->phase1_group_id = group;
  return c;
}

// 按拍往交换节点交包的驱动。给的是“第几拍交哪一包”。
class Feeder : public ClkModule {
 public:
  struct Item {
    uint64_t at = 0;
    EthPort ingress = EthPort::kResIngress;
    EthRole role = EthRole::kResidual;
    HitMapSemantic semantic = HitMapSemantic::kNone;
    CommInstPtr payload;
  };

  Feeder(ClockPtr c, EthSwitch* target, std::vector<Item> plan)
      : ClkModule(c), eth(target), items(std::move(plan)) {
    RegisterName("feeder");
  }

  void Cycle() override {
    DelayCycle(1);
    const Time now = RT::Now();
    for (Item const& it : items) {
      if (it.at != static_cast<uint64_t>(now)) continue;
      EthReq r(clk);
      r.ingress = static_cast<uint64_t>(it.ingress);
      r.role = static_cast<uint64_t>(it.role);
      r.semantic = static_cast<uint64_t>(it.semantic);
      r.bytes = 0;
      r.payload = it.payload;
      eth->OpenIngress(it.ingress).Push(r);
    }
  }

 private:
  EthSwitch* eth;
  std::vector<Item> items;
};

struct Rig {
  Params p;
  ClockPtr clk;
  std::unique_ptr<EthSwitch> eth;
  std::unique_ptr<Feeder> feeder;
  uint64_t cycles = 0;

  Rig() {
    RT::Reset(2, 2);
    clk = MakeClock(0, 1);
  }

  void Build(EthSwitchConfig const& cfg, std::vector<Feeder::Item> plan,
             uint64_t run_cycles) {
    eth = std::make_unique<EthSwitch>(clk, p, 100, cfg, "eth", 0);
    feeder = std::make_unique<Feeder>(clk, eth.get(), std::move(plan));
    cycles = run_cycles;
  }

  void Run() {
    clk->Continue(cycles);
    RT::JoinAll();
  }
};

// 一次工作的三包：Phase1 那两路，加上 Phase2 算完回来的那一路。
Feeder::Item MoeRequest(uint64_t at, uint64_t uid,
                        std::vector<uint32_t> experts) {
  Feeder::Item item;
  item.at = at;
  item.ingress = EthPort::kPhase1MoeIngress;
  item.role = EthRole::kMoeRequest;
  item.semantic = HitMapSemantic::kRoutedExpertId;
  item.payload = MakePacket(uid, std::move(experts));
  return item;
}

Feeder::Item Residual(uint64_t at, uint64_t uid) {
  Feeder::Item item;
  item.at = at;
  item.ingress = EthPort::kResIngress;
  item.role = EthRole::kResidual;
  item.semantic = HitMapSemantic::kNone;
  item.payload = MakePacket(uid, {});
  return item;
}

Feeder::Item MoeResult(uint64_t at, uint64_t uid,
                       std::vector<uint32_t> groups) {
  Feeder::Item item;
  item.at = at;
  item.ingress = EthPort::kPhase2MoeResultIngress;
  item.role = EthRole::kMoeResult;
  item.semantic = HitMapSemantic::kGroupId;
  item.payload = MakePacket(uid, std::move(groups));
  item.payload->opcode = Opcode::kMove;
  return item;
}

// 一包走完全程的三段时间。手算：入口 12416 除以 60 向上取整是 207 拍，加处理
// 200 拍，出口再 207 拍。
//
// 起点是交进来那一刻的下一拍：上游第 0 拍推进入口，交换节点第 1 拍才取到。这是
// 跨模块走 Fifo 固有的一拍，全模型一致，不是这一层特有的。
TEST(BachEthSwitch, ChargesTheThreeLegsOfOneCrossing) {
  Rig rig;
  Feeder::Item item;
  item.at = 0;
  item.ingress = EthPort::kResIngress;
  item.role = EthRole::kResidual;
  item.semantic = HitMapSemantic::kNone;
  item.payload = MakePacket(7, {});
  rig.Build(PlainConfig(), {item}, 900);
  rig.Run();

  ASSERT_EQ(rig.eth->Deliveries().size(), 1u);
  EthDelivery const& d = rig.eth->Deliveries()[0];
  const uint64_t leg = (kPacketBytes + 59) / 60;
  EXPECT_EQ(leg, 207u);
  EXPECT_EQ(d.submitted, 1u);
  EXPECT_EQ(d.ingress_serialize, leg);
  EXPECT_EQ(d.switch_arrived, d.submitted + leg);
  EXPECT_EQ(d.switch_done, d.submitted + leg + 200);
  EXPECT_EQ(d.tx_start, d.switch_done);
  EXPECT_EQ(d.egress_serialize, leg);
  EXPECT_EQ(d.arrived, d.submitted + leg * 2 + 200);
  EXPECT_FALSE(d.queued);
  EXPECT_EQ(d.egress, EthPort::kPhase3ResJoinEgress);
}

// 同一个出口上的两包串着走，后一包等前一包发完才开始。
TEST(BachEthSwitch, OneEgressSendsOnePacketAtATime) {
  Rig rig;
  std::vector<Feeder::Item> plan;
  for (uint64_t i = 0; i < 2; ++i) {
    Feeder::Item item;
    item.at = i;
    item.ingress = EthPort::kResIngress;
    item.role = EthRole::kResidual;
    item.semantic = HitMapSemantic::kNone;
    item.payload = MakePacket(i, {});
    plan.push_back(item);
  }
  rig.Build(PlainConfig(), plan, 1200);
  rig.Run();

  ASSERT_EQ(rig.eth->Deliveries().size(), 2u);
  EthDelivery const& a = rig.eth->Deliveries()[0];
  EthDelivery const& b = rig.eth->Deliveries()[1];
  EXPECT_FALSE(a.queued);
  EXPECT_TRUE(b.queued);
  EXPECT_EQ(b.tx_start, a.tx_end);
}

// 两条入口通向两个不同的出口时互不相干，各走各的。
TEST(BachEthSwitch, TwoEgressesRunSideBySide) {
  Rig rig;
  std::vector<Feeder::Item> plan;
  Feeder::Item res;
  res.at = 0;
  res.ingress = EthPort::kResIngress;
  res.role = EthRole::kResidual;
  res.semantic = HitMapSemantic::kNone;
  res.payload = MakePacket(1, {});
  plan.push_back(res);

  Feeder::Item moe;
  moe.at = 0;
  moe.ingress = EthPort::kPhase1MoeIngress;
  moe.role = EthRole::kMoeRequest;
  moe.semantic = HitMapSemantic::kRoutedExpertId;
  moe.payload = MakePacket(1, {0, 1, 17});
  plan.push_back(moe);

  rig.Build(PlainConfig(), plan, 1400);
  rig.Run();

  ASSERT_EQ(rig.eth->Deliveries().size(), 2u);
  for (EthDelivery const& d : rig.eth->Deliveries()) EXPECT_FALSE(d.queued);
}

// 专家换算成 group，同时数出每个 group 上命中几个专家。0 与 1 都落在 group 0 上，
// 17 落在 group 1 上，所以 group 0 的倍率是 2。
TEST(BachEthSwitch, TurnsExpertIdsIntoGroupIds) {
  Rig rig;
  Feeder::Item item;
  item.at = 0;
  item.ingress = EthPort::kPhase1MoeIngress;
  item.role = EthRole::kMoeRequest;
  item.semantic = HitMapSemantic::kRoutedExpertId;
  item.payload = MakePacket(5, {0, 1, 17});
  rig.Build(PlainConfig(), {item}, 1400);
  rig.Run();

  ASSERT_EQ(rig.eth->Deliveries().size(), 1u);
  EthEnvelope const& env = rig.eth->Deliveries()[0].out;
  EXPECT_EQ(env.semantic, HitMapSemantic::kGroupId);
  EXPECT_EQ(env.expert_hit_map, (std::vector<uint32_t>{0, 1, 17}));
  EXPECT_EQ(env.group_hit_map, (std::vector<uint32_t>{0, 1}));
  ASSERT_EQ(env.group_multipliers.size(), 2u);
  EXPECT_EQ(env.group_multipliers[0].first, 0u);
  EXPECT_EQ(env.group_multipliers[0].second, 2u);
  EXPECT_EQ(env.group_multipliers[1].first, 1u);
  EXPECT_EQ(env.group_multipliers[1].second, 1u);
  // 出去那一包上带的是换算之后的 group，不是原来那几个专家。
  EXPECT_EQ(rig.eth->Deliveries()[0].out.packet->hit_map,
            (std::vector<uint32_t>{0, 1}));
}

// Map 写明了一个专家落在哪几个 group 上时，按它写的来，不按整除。
TEST(BachEthSwitch, FollowsTheExplicitExpertToGroupTable) {
  Rig rig;
  EthSwitchConfig cfg = PlainConfig();
  cfg.expert_group_size = 16;
  cfg.expert_group_map.emplace_back(0, std::vector<uint32_t>{4, 9});
  cfg.expert_group_map.emplace_back(1, std::vector<uint32_t>{9});

  Feeder::Item item;
  item.at = 0;
  item.ingress = EthPort::kPhase1MoeIngress;
  item.role = EthRole::kMoeRequest;
  item.semantic = HitMapSemantic::kRoutedExpertId;
  item.payload = MakePacket(5, {0, 1});
  rig.Build(cfg, {item}, 1400);
  rig.Run();

  ASSERT_EQ(rig.eth->Deliveries().size(), 1u);
  EthEnvelope const& env = rig.eth->Deliveries()[0].out;
  EXPECT_EQ(env.group_hit_map, (std::vector<uint32_t>{4, 9}));
  ASSERT_EQ(env.group_multipliers.size(), 2u);
  EXPECT_EQ(env.group_multipliers[0].second, 1u);
  EXPECT_EQ(env.group_multipliers[1].second, 2u);
}

// 交换节点自己那个 Phase1 边界的判法：残差与 MoE 请求都投出去了才算一次。
TEST(BachEthSwitch, TheBoundaryNeedsBothHalves) {
  Rig rig;
  std::vector<Feeder::Item> plan;
  Feeder::Item res;
  res.at = 0;
  res.ingress = EthPort::kResIngress;
  res.role = EthRole::kResidual;
  res.semantic = HitMapSemantic::kNone;
  res.payload = MakePacket(9, {});
  plan.push_back(res);
  rig.Build(PlainConfig(), plan, 1400);
  rig.Run();
  EXPECT_EQ(rig.eth->BoundaryCompletions(), 0u);

  Rig both;
  std::vector<Feeder::Item> full;
  full.push_back(res);
  Feeder::Item moe;
  moe.at = 0;
  moe.ingress = EthPort::kPhase1MoeIngress;
  moe.role = EthRole::kMoeRequest;
  moe.semantic = HitMapSemantic::kRoutedExpertId;
  moe.payload = MakePacket(9, {3});
  full.push_back(moe);
  both.Build(PlainConfig(), full, 1400);
  both.Run();
  EXPECT_EQ(both.eth->BoundaryCompletions(), 1u);
}

// ---------------------------------------------------------------- Phase2

// 换算完的那一份交给 Phase2 时，包上带的与信封上记的必须一份不差。这里跑通就说明
// 五条校验都过了，跑不通会当场停机。
TEST(BachPhase2Ingress, HandsTheGroupEnvelopeToTheBoundHost) {
  Rig rig;
  Fifo<HostOffer> port(64, rig.clk);
  Phase2EthIngress adapter(rig.clk);
  adapter.BindHost("H0", Coord{-1, 0}, &port);
  adapter.BindGroup(0, "H0");
  adapter.BindGroup(1, "H0");
  adapter.Validate();

  rig.Build(PlainConfig(), {MoeRequest(0, 4, {0, 1, 17})}, 1400);
  rig.eth->AttachSink(&adapter);
  rig.Run();

  ASSERT_EQ(adapter.Injections().size(), 1u);
  Phase2Injection const& in = adapter.Injections()[0];
  EXPECT_EQ(in.host_name, "H0");
  EXPECT_EQ(in.uid, 4u);
  EXPECT_EQ(in.groups, (std::vector<uint32_t>{0, 1}));
  // 一个 Host 收到的那一份，tag 是它这几个 group 上命中的专家数之和
  EXPECT_EQ(in.tag, 3u);
}

// 几个 group 归几个 Host 时按绑定分开，各自只拿自己那几个。
TEST(BachPhase2Ingress, SplitsTheGroupsAmongTheHostsThatOwnThem) {
  Rig rig;
  Fifo<HostOffer> port_a(64, rig.clk);
  Fifo<HostOffer> port_b(64, rig.clk);
  Phase2EthIngress adapter(rig.clk);
  adapter.BindHost("H0", Coord{-1, 0}, &port_a);
  adapter.BindHost("H1", Coord{-1, 1}, &port_b);
  adapter.BindGroup(0, "H0");
  adapter.BindGroup(1, "H1");
  adapter.Validate();

  rig.Build(PlainConfig(), {MoeRequest(0, 4, {0, 1, 17})}, 1400);
  rig.eth->AttachSink(&adapter);
  rig.Run();

  ASSERT_EQ(adapter.Injections().size(), 2u);
  EXPECT_EQ(adapter.Injections()[0].host_name, "H0");
  EXPECT_EQ(adapter.Injections()[0].groups, (std::vector<uint32_t>{0}));
  EXPECT_EQ(adapter.Injections()[0].tag, 2u);
  EXPECT_EQ(adapter.Injections()[1].host_name, "H1");
  EXPECT_EQ(adapter.Injections()[1].groups, (std::vector<uint32_t>{1}));
  EXPECT_EQ(adapter.Injections()[1].tag, 1u);
}

// ---------------------------------------------------------------- Phase3

// 残差先到，结果后到。汇合的时刻是慢的那一路到的时刻。
TEST(BachPhase3Join, JoinsWhenTheResidualArrivesFirst) {
  Rig rig;
  SpanRecorder rec;
  Phase3Join join(&rec);
  rig.Build(PlainConfig(),
            {MoeRequest(0, 6, {0, 17}), Residual(1000, 6),
             MoeResult(1100, 6, {0, 1})},
            2600);
  rig.eth->SetPhase1BoundaryCompletion(false);
  rig.eth->AttachSink(&join);
  rig.Run();

  ASSERT_EQ(join.Joins().size(), 1u);
  PhaseJoin const& j = join.Joins()[0];
  EXPECT_EQ(j.key.uid, 6u);
  EXPECT_LT(j.residual_at, j.result_at);
  EXPECT_EQ(j.joined_at, j.result_at);
  EXPECT_EQ(join.PendingNum(), 0u);
  EXPECT_EQ(rig.eth->BoundaryCompletions(), 0u);
  EXPECT_EQ(rig.eth->CompletedNum(), 1u);
}

// 结果先到，残差后到。同一次工作，同样判完，汇合时刻仍取慢的那一路。
TEST(BachPhase3Join, JoinsWhenTheResultArrivesFirst) {
  Rig rig;
  SpanRecorder rec;
  Phase3Join join(&rec);
  rig.Build(PlainConfig(),
            {MoeRequest(0, 6, {0, 17}), MoeResult(1100, 6, {0, 1}),
             Residual(1200, 6)},
            2600);
  rig.eth->SetPhase1BoundaryCompletion(false);
  rig.eth->AttachSink(&join);
  rig.Run();

  ASSERT_EQ(join.Joins().size(), 1u);
  PhaseJoin const& j = join.Joins()[0];
  EXPECT_EQ(j.key.uid, 6u);
  EXPECT_LT(j.result_at, j.residual_at);
  EXPECT_EQ(j.joined_at, j.residual_at);
  EXPECT_EQ(join.PendingNum(), 0u);
  EXPECT_EQ(rig.eth->CompletedNum(), 1u);
}

// 只到一路就还不算完，一直等着。
TEST(BachPhase3Join, HalfOfAJoinIsNotAJoin) {
  Rig rig;
  SpanRecorder rec;
  Phase3Join join(&rec);
  rig.Build(PlainConfig(), {MoeRequest(0, 6, {0}), Residual(1000, 6)}, 2600);
  rig.eth->SetPhase1BoundaryCompletion(false);
  rig.eth->AttachSink(&join);
  rig.Run();

  EXPECT_EQ(join.JoinedNum(), 0u);
  EXPECT_EQ(join.PendingNum(), 1u);
  EXPECT_EQ(rig.eth->CompletedNum(), 0u);
}

// 两次工作各自汇合，互不相干。
TEST(BachPhase3Join, TwoUsersJoinIndependently) {
  Rig rig;
  SpanRecorder rec;
  Phase3Join join(&rec);
  rig.Build(PlainConfig(),
            {MoeRequest(0, 1, {0}), MoeRequest(600, 2, {17}),
             Residual(1500, 2), MoeResult(1600, 1, {0}),
             MoeResult(2200, 2, {1}), Residual(2400, 1)},
            5000);
  rig.eth->SetPhase1BoundaryCompletion(false);
  rig.eth->AttachSink(&join);
  rig.Run();

  ASSERT_EQ(join.Joins().size(), 2u);
  EXPECT_EQ(join.Joins()[0].key.uid, 2u);
  EXPECT_EQ(join.Joins()[1].key.uid, 1u);
  EXPECT_EQ(rig.eth->CompletedNum(), 2u);
}

// 完成权只有一处。汇合点接上去之后，交换节点自己那个 Phase1 边界的判法必须关掉，
// 两处都判会把一次工作记成两次完成。
TEST(BachPhase3Join, TheJoinTakesTheOnlyCompletionAuthority) {
  Rig rig;
  SpanRecorder rec;
  Phase3Join join(&rec);
  rig.Build(PlainConfig(), {}, 10);
  EXPECT_TRUE(rig.eth->Phase1BoundaryCompletion());
  rig.eth->SetPhase1BoundaryCompletion(false);
  rig.eth->AttachSink(&join);
  EXPECT_FALSE(rig.eth->Phase1BoundaryCompletion());
  rig.Run();
  EXPECT_EQ(rig.eth->CompletedNum(), 0u);
}

}
}
}
