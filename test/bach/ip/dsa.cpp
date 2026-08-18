// 五路径仲裁：一次搬运走哪条路径、哪两条能同时跑、谁该先放行。
//
// 前半是纯逻辑，不挂时钟：路径怎么定下来、掩码怎么判冲突、队列怎么排。后半把它接回
// DTE，看两条不冲突的路径在真的一拍一拍推进下确实并行，冲突的确实串行。

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/module.h"
#include "base/runtime.h"
#include "bach/common/arbiter.h"
#include "bach/ip/chip/core/dte/dsa.h"
#include "bach/ip/chip/core/dte/dte.h"
#include "bach/ip/chip/core/memory/memory_system.h"
#include "bach/ip/chip/core/moe_bitmap.h"
#include "bach/ip/chip/core/ports.h"
#include "fixture.h"

using namespace latch;
using namespace latch::bach;
using latch::bach::test::CoreTables;
using latch::bach::test::StepDriver;

namespace {

constexpr Time kPeriod = 1;

TaskMeta RouteMeta(DsaRoute route) {
  TaskMeta m;
  m.dsa_route = route;
  return m;
}

TaskMeta LocalMoveMeta() {
  TaskMeta m;
  m.dsa_route = DsaRoute::kM2C;
  m.dsa_local_transfer = true;
  m.source_endpoint = Endpoint::kMatrix;
  m.destination_endpoint = Endpoint::kCore;
  return m;
}

// 一个只做仲裁、不做别的事的架子。两条物理通道归它自己。
class Fabric {
 public:
  Fabric() : arb(&in, &out, 7) {}

  Ticket Ask(Time now, DsaRoute route, uint64_t uid) {
    const bool outgoing = route == DsaRoute::kM2R || route == DsaRoute::kC2R ||
                          route == DsaRoute::kM2C;
    TaskMeta m = route == DsaRoute::kM2C ? LocalMoveMeta() : RouteMeta(route);
    DsaPlan plan = ResolveDsaPlan(outgoing, Opcode::kMove, m, 7);
    return arb.Request(now, plan, uid, 0);
  }

  ExclusiveArbiter in;
  ExclusiveArbiter out;
  DsaArbiter arb;
};

class FakeScheduler : public SchedulerPort {
 public:
  void Ack(uint64_t uid, uint64_t tid) override { acks.push_back({uid, tid}); }
  uint64_t StreamPriority(uint64_t, bool) const override { return 0; }
  void AdmitWithPrecompletedTask(uint64_t) override {}

  struct Item {
    uint64_t uid = 0;
    uint64_t tid = 0;
  };
  std::vector<Item> acks;
};

class FakeCredit : public CreditReturnPort {
 public:
  void ReturnCredit(int64_t, uint64_t) override {}
};

class FakeSink : public PacketSink {
 public:
  void Inject(CommInstPtr const&, Coord, uint64_t) override { ++sent; }
  uint64_t sent = 0;
};

// 一条路径在痕迹里的占用区间。
std::vector<std::pair<Time, Time>> SpansOf(std::vector<DsaRecord> const& records,
                                           DsaRoute route) {
  std::vector<std::pair<Time, Time>> out;
  for (DsaRecord const& r : records) {
    if (r.route == route) out.emplace_back(r.grant, r.release);
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------- 路径怎么定

TEST(BachDsaPlan, TakesTheRouteWrittenInTheTask) {
  DsaPlan in_side = ResolveDsaPlan(false, Opcode::kMove,
                                   RouteMeta(DsaRoute::kR2M), 0);
  EXPECT_EQ(in_side.route, DsaRoute::kR2M);
  EXPECT_EQ(in_side.mask, kDsaInMask);
  EXPECT_EQ(in_side.group, DsaGroup::kToCore);
  EXPECT_EQ(in_side.source, DsaSource::kExplicit);
  EXPECT_TRUE(in_side.analysis_eligible);

  DsaPlan out_side = ResolveDsaPlan(true, Opcode::kMove,
                                    RouteMeta(DsaRoute::kC2R), 0);
  EXPECT_EQ(out_side.route, DsaRoute::kC2R);
  EXPECT_EQ(out_side.mask, kDsaOutMask);
  EXPECT_EQ(out_side.group, DsaGroup::kToRouter);

  // 两头都在本地的那一条把进出两条通道都占了
  DsaPlan local = ResolveDsaPlan(true, Opcode::kMove, LocalMoveMeta(), 0);
  EXPECT_EQ(local.route, DsaRoute::kM2C);
  EXPECT_EQ(local.mask, kDsaBothMask);
  EXPECT_TRUE(local.IsLocalMove());
}

TEST(BachDsaPlan, FallsBackToTheDirectionWhenNoRouteIsWritten) {
  TaskMeta plain;
  DsaPlan in_side = ResolveDsaPlan(false, Opcode::kReduction, plain, 0);
  EXPECT_EQ(in_side.route, DsaRoute::kNone);
  EXPECT_EQ(in_side.queue, DsaQueue::kDirectionIn);
  EXPECT_EQ(in_side.mask, kDsaInMask);
  EXPECT_EQ(in_side.source, DsaSource::kDirectionFallback);
  // 兜底的仍然算得进占用，只是落不到某一条具体路径上
  EXPECT_TRUE(in_side.analysis_eligible);

  DsaPlan out_side = ResolveDsaPlan(true, Opcode::kReduce, plain, 0);
  EXPECT_EQ(out_side.queue, DsaQueue::kDirectionOut);
  EXPECT_EQ(out_side.mask, kDsaOutMask);
}

TEST(BachDsaPlan, WorkThatMovesNothingStaysOutOfTheArbiter) {
  TaskMeta plain;
  DsaPlan retire = ResolveDsaPlan(true, Opcode::kRetire, plain, 0,
                                  /*inactive_forward=*/false,
                                  /*local_res_sum=*/false,
                                  /*local_retire=*/true);
  EXPECT_TRUE(retire.IsNonTransfer());
  EXPECT_EQ(retire.mask, 0u);
  EXPECT_EQ(retire.non_transfer, DsaNonTransfer::kLocalRetire);
  EXPECT_FALSE(retire.analysis_eligible);

  DsaPlan res = ResolveDsaPlan(true, Opcode::kRes, plain, 0, false, true, false);
  EXPECT_EQ(res.non_transfer, DsaNonTransfer::kLocalResSum);
}

TEST(BachDsaPlan, ForwardingAnInactivePacketTakesBothLanes) {
  TaskMeta plain;
  DsaPlan p = ResolveDsaPlan(false, Opcode::kFifoIn, plain, 0,
                             /*inactive_forward=*/true);
  EXPECT_EQ(p.queue, DsaQueue::kControlForward);
  // 一边收一边发，两条通道都要
  EXPECT_EQ(p.mask, kDsaBothMask);
  EXPECT_EQ(p.route, DsaRoute::kNone);
  EXPECT_FALSE(p.analysis_eligible);
}

// ---------------------------------------------------------------- 掩码冲突

TEST(BachDsaArbiter, RunsOneInboundAndOneOutboundAtOnce) {
  Fabric f;
  Ticket a = f.Ask(0, DsaRoute::kR2M, 1);
  Ticket b = f.Ask(0, DsaRoute::kM2R, 2);

  // 一进一出，掩码不相交，两条一起放行
  EXPECT_TRUE(f.arb.Granted(a));
  EXPECT_TRUE(f.arb.Granted(b));
  EXPECT_EQ(f.arb.ActiveNum(), 2u);
  EXPECT_EQ(f.arb.ActiveMask(), kDsaBothMask);
  // 逻辑放行的那一刻物理通道也被占住了
  EXPECT_TRUE(f.in.Busy());
  EXPECT_TRUE(f.out.Busy());
}

TEST(BachDsaArbiter, TwoInboundRoutesTakeTurns) {
  Fabric f;
  Ticket a = f.Ask(0, DsaRoute::kR2M, 1);
  Ticket b = f.Ask(0, DsaRoute::kR2C, 2);

  EXPECT_TRUE(f.arb.Granted(a));
  EXPECT_FALSE(f.arb.Granted(b));

  f.arb.Release(a, 10);
  EXPECT_TRUE(f.arb.Granted(b));
  EXPECT_EQ(f.arb.GrantCycle(b), 10u);
}

TEST(BachDsaArbiter, TheLocalRouteBlocksEverything) {
  Fabric f;
  Ticket local = f.Ask(0, DsaRoute::kM2C, 1);
  Ticket in_side = f.Ask(0, DsaRoute::kR2M, 2);
  Ticket out_side = f.Ask(0, DsaRoute::kC2R, 3);

  EXPECT_TRUE(f.arb.Granted(local));
  EXPECT_FALSE(f.arb.Granted(in_side));
  EXPECT_FALSE(f.arb.Granted(out_side));

  // 它放开之后，被它挡住的那两条互不冲突，一起放行
  f.arb.Release(local, 20);
  EXPECT_TRUE(f.arb.Granted(in_side));
  EXPECT_TRUE(f.arb.Granted(out_side));
  EXPECT_EQ(f.arb.ActiveNum(), 2u);
}

// ---------------------------------------------------------------- 最老优先

TEST(BachDsaArbiter, AYoungerCompatibleRequestDoesNotOvertakeAnOlderBlockedOne) {
  Fabric f;
  Ticket first = f.Ask(0, DsaRoute::kR2M, 1);   // 占住进方向
  Ticket second = f.Ask(0, DsaRoute::kM2C, 2);  // 要两条，被挡
  Ticket third = f.Ask(0, DsaRoute::kR2C, 3);   // 只要进方向，也被挡

  EXPECT_TRUE(f.arb.Granted(first));
  EXPECT_FALSE(f.arb.Granted(second));
  EXPECT_FALSE(f.arb.Granted(third));

  // 第一条放开之后，两条通道都空了。只看掩码的话第三条也能走，但第二条比它老，
  // 而且两条的掩码相交，所以第三条不许越过去
  f.arb.Release(first, 10);
  EXPECT_TRUE(f.arb.Granted(second));
  EXPECT_FALSE(f.arb.Granted(third));

  f.arb.Release(second, 30);
  EXPECT_TRUE(f.arb.Granted(third));
  EXPECT_EQ(f.arb.GrantCycle(third), 30u);
}

TEST(BachDsaArbiter, OnlyTheHeadOfEachQueueIsConsidered) {
  Fabric f;
  Ticket a = f.Ask(0, DsaRoute::kR2M, 1);
  Ticket b = f.Ask(0, DsaRoute::kR2M, 2);
  Ticket c = f.Ask(0, DsaRoute::kM2R, 3);

  // 同一条路径的两个请求先来后到，另一条路径不受它们影响
  EXPECT_TRUE(f.arb.Granted(a));
  EXPECT_FALSE(f.arb.Granted(b));
  EXPECT_TRUE(f.arb.Granted(c));
  EXPECT_EQ(f.arb.QueueLen(DsaQueue::kR2M), 1u);

  f.arb.Release(a, 5);
  EXPECT_TRUE(f.arb.Granted(b));
}

TEST(BachDsaArbiter, CancelsARequestThatNeverRan) {
  Fabric f;
  Ticket a = f.Ask(0, DsaRoute::kR2M, 1);
  Ticket b = f.Ask(0, DsaRoute::kR2C, 2);
  EXPECT_EQ(f.arb.PendingNum(), 1u);

  f.arb.Cancel(b, 3);
  EXPECT_EQ(f.arb.PendingNum(), 0u);
  f.arb.Release(a, 4);
  // 撤掉的那个不留痕，只有真跑过的才留
  EXPECT_EQ(f.arb.Records().size(), 1u);
}

// ---------------------------------------------------------------- 十个交点

TEST(BachDsaAnalysis, ScoresFiveRoutesAndTenCrossings) {
  Fabric f;
  Ticket a = f.Ask(0, DsaRoute::kR2M, 1);
  Ticket b = f.Ask(0, DsaRoute::kM2R, 2);
  Ticket c = f.Ask(0, DsaRoute::kR2C, 3);
  f.arb.Release(a, 10);   // R2M 占 0 到 10，放开时 R2C 接上
  f.arb.Release(b, 14);   // M2R 占 0 到 14
  f.arb.Release(c, 26);   // R2C 占 10 到 26

  DsaAnalysis an = AnalyzeDsa(f.arb.Records());

  EXPECT_EQ(an.Route(DsaRoute::kR2M).grants, 1u);
  EXPECT_EQ(an.Route(DsaRoute::kR2M).busy, 10u);
  EXPECT_EQ(an.Route(DsaRoute::kR2C).busy, 16u);
  EXPECT_EQ(an.Route(DsaRoute::kR2C).waited, 10u);
  EXPECT_EQ(an.Route(DsaRoute::kM2R).busy, 14u);
  EXPECT_EQ(an.Route(DsaRoute::kM2C).grants, 0u);

  // 十个交点一个不少，六个掩码相交、四个不相交
  uint32_t conflicting = 0;
  for (uint32_t i = 0; i < kDsaCrossNum; ++i) {
    if (an.crosses[i].mask_conflict) ++conflicting;
  }
  EXPECT_EQ(kDsaCrossNum, 10u);
  EXPECT_EQ(conflicting, 6u);

  // 掩码相交的交点上重叠必须是零，那正是仲裁器要保证的事
  for (uint32_t i = 0; i < kDsaCrossNum; ++i) {
    if (!an.crosses[i].mask_conflict) continue;
    EXPECT_EQ(an.crosses[i].overlap, 0u)
        << DsaRouteName(an.crosses[i].a) << " 与 " << DsaRouteName(an.crosses[i].b)
        << " 不该同时占用";
  }

  // 一进一出的那个交点上确实并行过：R2M 与 M2R 重叠 10，M2R 与 R2C 重叠 4
  EXPECT_EQ(an.Cross(DsaRoute::kR2M, DsaRoute::kM2R).overlap, 10u);
  EXPECT_EQ(an.Cross(DsaRoute::kM2R, DsaRoute::kR2C).overlap, 4u);
  // R2C 入队时被 R2M 挡着
  EXPECT_EQ(an.Cross(DsaRoute::kR2M, DsaRoute::kR2C).blocks, 1u);
}

TEST(BachDsaAnalysis, SeparatesDirectionOnlyAndControlWork) {
  Fabric f;
  TaskMeta plain;
  DsaPlan fallback = ResolveDsaPlan(true, Opcode::kMove, plain, 7);
  DsaPlan forward = ResolveDsaPlan(false, Opcode::kFifoIn, plain, 7, true);

  Ticket a = f.arb.Request(0, fallback, 1, 0);
  f.arb.Release(a, 5);
  Ticket b = f.arb.Request(5, forward, 2, 0);
  f.arb.Release(b, 9);

  DsaAnalysis an = AnalyzeDsa(f.arb.Records());
  // 只知道方向的算一次占用但落不到路径上，控制转发的连占用都不算
  EXPECT_EQ(an.direction_only, 1u);
  EXPECT_EQ(an.excluded, 1u);
  for (uint32_t i = 0; i < kDsaRouteNum; ++i) {
    EXPECT_EQ(an.routes[i].grants, 0u);
  }
}

// ---------------------------------------------------------------- 接回 DTE

// 出方向占着通道时收到一个包：一进一出不冲突，收包不必等发包做完。
TEST(BachDteDsa, AnIncomingPacketRunsBesideAnOutgoingOne) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  tb.params.dte_dsa_mode = DteDsaMode::kFiveRoute;
  // 出方向那条搬得久，久到收包那条走完 setup 时它还在跑
  tb.Task(UnitType::kSkip, Opcode::kUserInit, 0, 1024)
      .Task(UnitType::kDte, Opcode::kMove, 0, 512 * 200, Coord{0, 1})
      .Meta(0, RouteMeta(DsaRoute::kR2C))
      .Meta(1, RouteMeta(DsaRoute::kC2R));
  SpanRecorder rec;
  CoreContext ctx = tb.Context(&rec);
  MoeBitMap bitmap(clk, ctx, "bitmap", 0);
  MemorySystem mem(clk, ctx, "mem", 0);
  Dte dte(clk, ctx, "dte", 0);
  FakeScheduler sched;
  FakeCredit credit;
  FakeSink sink;
  dte.Connect(&sched, &credit, &sink, &bitmap, &mem);

  bool both_at_once = false;
  StepDriver drv(clk, [&](Time now, uint64_t k) {
    bitmap.Step();
    mem.Step();
    dte.Step();
    if (k == 0) {
      dte.DispatchComm(now, 1, 1, 0);
      CommInstPtr c = std::make_shared<CommInst>();
      c->uid = 2;
      c->tid = 0;
      c->opcode = Opcode::kUserInit;
      c->beat_id = 0;
      c->total_fragments = 1;
      dte.HandleComm(c);
    }
    if (dte.DsaActiveMask() == kDsaBothMask) both_at_once = true;
  });

  clk->Continue(2000 * kPeriod);
  RT::JoinAll();

  EXPECT_TRUE(both_at_once);
  auto out_spans = SpansOf(dte.DsaRecords(), DsaRoute::kC2R);
  auto in_spans = SpansOf(dte.DsaRecords(), DsaRoute::kR2C);
  ASSERT_EQ(out_spans.size(), 1u);
  ASSERT_EQ(in_spans.size(), 1u);
  // 收包那条在发包那条还没做完时就开工了
  EXPECT_LT(in_spans[0].first, out_spans[0].second);

  DsaAnalysis an = AnalyzeDsa(dte.DsaRecords());
  EXPECT_GT(an.Cross(DsaRoute::kC2R, DsaRoute::kR2C).overlap, 0u);
}

// 两条都往外走：掩码相交，后一条只能等前一条腾出通道。
TEST(BachDteDsa, TwoOutgoingRoutesDoNotOverlap) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  tb.params.dte_dsa_mode = DteDsaMode::kFiveRoute;
  tb.Task(UnitType::kDte, Opcode::kMove, 0, 512 * 200, Coord{0, 1})
      .Task(UnitType::kDte, Opcode::kMove, 0, 512 * 4, Coord{0, 1})
      .Meta(0, RouteMeta(DsaRoute::kC2R))
      .Meta(1, RouteMeta(DsaRoute::kM2R));
  SpanRecorder rec;
  CoreContext ctx = tb.Context(&rec);
  MoeBitMap bitmap(clk, ctx, "bitmap", 0);
  MemorySystem mem(clk, ctx, "mem", 0);
  Dte dte(clk, ctx, "dte", 0);
  FakeScheduler sched;
  FakeCredit credit;
  FakeSink sink;
  dte.Connect(&sched, &credit, &sink, &bitmap, &mem);

  StepDriver drv(clk, [&](Time now, uint64_t k) {
    bitmap.Step();
    mem.Step();
    dte.Step();
    if (k == 0) {
      dte.DispatchComm(now, 1, 0, 0);
      dte.DispatchComm(now, 2, 1, 0);
    }
  });

  clk->Continue(3000 * kPeriod);
  RT::JoinAll();

  DsaAnalysis an = AnalyzeDsa(dte.DsaRecords());
  EXPECT_EQ(an.Route(DsaRoute::kC2R).grants, 1u);
  EXPECT_EQ(an.Route(DsaRoute::kM2R).grants, 1u);
  EXPECT_EQ(an.Cross(DsaRoute::kC2R, DsaRoute::kM2R).overlap, 0u);
  // 后一条确实被挡过，不是恰好错开的
  EXPECT_GT(an.Route(DsaRoute::kM2R).waited, 0u);
  EXPECT_EQ(an.Cross(DsaRoute::kC2R, DsaRoute::kM2R).blocks, 1u);
}

// 两头都在本地的那条不上链路：占着两条通道走完，一个包也不发。
TEST(BachDteDsa, TheLocalMoveSendsNothingAndHoldsBothLanes) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  tb.params.dte_dsa_mode = DteDsaMode::kFiveRoute;
  tb.Task(UnitType::kDte, Opcode::kMove, 0, 512 * 8, Coord{0, 0})
      .Meta(0, LocalMoveMeta());
  SpanRecorder rec;
  CoreContext ctx = tb.Context(&rec);
  MoeBitMap bitmap(clk, ctx, "bitmap", 0);
  MemorySystem mem(clk, ctx, "mem", 0);
  Dte dte(clk, ctx, "dte", 0);
  FakeScheduler sched;
  FakeCredit credit;
  FakeSink sink;
  dte.Connect(&sched, &credit, &sink, &bitmap, &mem);

  bool held_both = false;
  StepDriver drv(clk, [&](Time now, uint64_t k) {
    bitmap.Step();
    mem.Step();
    dte.Step();
    if (k == 0) dte.DispatchComm(now, 1, 0, 0);
    if (dte.DsaActiveMask() == kDsaBothMask) held_both = true;
  });

  clk->Continue(1000 * kPeriod);
  RT::JoinAll();

  EXPECT_TRUE(held_both);
  EXPECT_EQ(sink.sent, 0u);
  ASSERT_EQ(sched.acks.size(), 1u);
  DsaAnalysis an = AnalyzeDsa(dte.DsaRecords());
  // 8192 字节按每拍 512 是 8 拍
  EXPECT_EQ(an.Route(DsaRoute::kM2C).grants, 1u);
  EXPECT_EQ(an.Route(DsaRoute::kM2C).busy, 8u);
}

// 末端核的退休两头都不动数据，不占路径，也就不会挡住别的搬运。
TEST(BachDteDsa, WorkThatMovesNothingLeavesNoTrace) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  tb.params.dte_dsa_mode = DteDsaMode::kFiveRoute;
  TaskMeta ending;
  ending.no_credit_return = true;
  tb.Task(UnitType::kSkip, Opcode::kUserInit, 0, 512)
      .Task(UnitType::kDte, Opcode::kRetire, 0, 0, Coord{0, 0})
      .Meta(1, ending);
  SpanRecorder rec;
  CoreContext ctx = tb.Context(&rec);
  MoeBitMap bitmap(clk, ctx, "bitmap", 0);
  MemorySystem mem(clk, ctx, "mem", 0);
  Dte dte(clk, ctx, "dte", 0);
  FakeScheduler sched;
  FakeCredit credit;
  FakeSink sink;
  dte.Connect(&sched, &credit, &sink, &bitmap, &mem);

  StepDriver drv(clk, [&](Time now, uint64_t k) {
    bitmap.Step();
    mem.Step();
    dte.Step();
    if (k == 0) {
      CommInstPtr c = std::make_shared<CommInst>();
      c->uid = 1;
      c->tid = 0;
      c->opcode = Opcode::kUserInit;
      c->beat_id = 0;
      c->total_fragments = 1;
      dte.HandleComm(c);
    }
    if (k == 400) dte.DispatchComm(now, 1, 1, 0);
  });

  clk->Continue(1000 * kPeriod);
  RT::JoinAll();

  EXPECT_EQ(sink.sent, 0u);
  EXPECT_EQ(dte.FreeSlotNum(), dte.TotalSlots());
  // 收包那一条留了痕，退休那一条没有
  ASSERT_EQ(dte.DsaRecords().size(), 1u);
  EXPECT_EQ(dte.DsaRecords()[0].queue, DsaQueue::kDirectionIn);
}
