// Core 内各单元的时序基线：一条任务在每个单元里花多少拍、卡在哪、什么时候放行。
//
// 每个用例给被测单元配一个 ClkModule 驱动，在它的 Cycle 里按拍操作、按拍观察，
// JoinAll 之后主线程再断言。这样观察到的量都是协程里读的，不是事后读 Logic64。

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/module.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/compute/matrix_core.h"
#include "bach/ip/chip/core/compute/vector_core.h"
#include "bach/ip/chip/core/credit_unit.h"
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

struct AckRecord {
  uint64_t uid = 0;
  uint64_t tid = 0;
  Time at = 0;
};

class FakeScheduler : public SchedulerPort {
 public:
  void Ack(uint64_t uid, uint64_t tid) override {
    acks.push_back({uid, tid, RT::Now()});
  }
  uint64_t StreamPriority(uint64_t, bool) const override { return 0; }
  void AdmitWithPrecompletedTask(uint64_t uid) override {
    admitted.push_back(uid);
  }

  std::vector<AckRecord> acks;
  std::vector<uint64_t> admitted;
};

class FakeCredit : public CreditReturnPort {
 public:
  void ReturnCredit(int64_t core_id, uint64_t amount) override {
    returned.push_back({core_id, amount, RT::Now()});
  }

  struct Item {
    int64_t core_id = 0;
    uint64_t amount = 0;
    Time at = 0;
  };
  std::vector<Item> returned;
};

class FakeSink : public PacketSink {
 public:
  void Inject(CommInstPtr const& payload, Coord dst, uint64_t size) override {
    sent.push_back({payload, dst, size, RT::Now()});
  }

  struct Item {
    CommInstPtr payload;
    Coord dst;
    uint64_t size = 0;
    Time at = 0;
  };
  std::vector<Item> sent;
};

}  // namespace

// ---------------------------------------------------------------- 存储

TEST(BachMemory, SerializesTwoAccessesOnOneArbiter) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  MemorySystem mem(clk, tb.Context(), "mem", 0);

  Ticket a = kNoTicket, b = kNoTicket;
  Time t0 = 0, a_end = 0, b_end = 0;
  StepDriver drv(clk, [&](Time now, uint64_t k) {
    mem.Step();
    if (k == 0) {
      t0 = now;
      a = mem.DteCoreMem().Access(now, 1, 0, 256);
      b = mem.DteCoreMem().Access(now, 2, 0, 128);
    }
    if (a != kNoTicket && a_end == 0 && mem.DteCoreMem().Done(a)) a_end = now;
    if (b != kNoTicket && b_end == 0 && mem.DteCoreMem().Done(b)) b_end = now;
  });

  clk->Continue(200 * kPeriod);
  RT::JoinAll();

  // 基础寻址延迟加传输拍数：256 字节按 128 的带宽是两拍
  EXPECT_EQ(a_end - t0, tb.params.cm_arb_delay + 2);
  // 第二个从第一个放开锁那一拍才开始算
  EXPECT_EQ(b_end - a_end, tb.params.cm_arb_delay + 1);
}

TEST(BachMemory, TwoBlocksDoNotBlockEachOther) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  MemorySystem mem(clk, tb.Context(), "mem", 0);

  Ticket a = kNoTicket, b = kNoTicket;
  Time t0 = 0, a_end = 0, b_end = 0;
  StepDriver drv(clk, [&](Time now, uint64_t k) {
    mem.Step();
    if (k == 0) {
      t0 = now;
      a = mem.DteCoreMem().Access(now, 1, 0, 128);
      b = mem.DteMatrixMem().Access(now, 2, 0, 128);
    }
    if (a != kNoTicket && a_end == 0 && mem.DteCoreMem().Done(a)) a_end = now;
    if (b != kNoTicket && b_end == 0 && mem.DteMatrixMem().Done(b)) b_end = now;
  });

  clk->Continue(300 * kPeriod);
  RT::JoinAll();

  EXPECT_EQ(a_end - t0, tb.params.cm_arb_delay + 1);
  EXPECT_EQ(b_end - t0, tb.params.mm_arb_delay + 1);
}

// ---------------------------------------------------------------- 倍率表

TEST(BachBitMap, WriteThenReadGivesTheStoredValue) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  SpanRecorder rec;
  MoeBitMap bitmap(clk, tb.Context(&rec), "bitmap", 0);

  Ticket w = kNoTicket, r = kNoTicket;
  uint64_t value = 0;
  Time t0 = 0, w_end = 0, r_end = 0;
  StepDriver drv(clk, [&](Time now, uint64_t k) {
    bitmap.Step();
    if (k == 0) {
      t0 = now;
      w = bitmap.BeginWrite(now, 7, 0, 3);
    }
    if (w != kNoTicket && w_end == 0 && bitmap.Done(w)) {
      w_end = now;
      bitmap.Finish(w);
      w = kNoTicket;
      r = bitmap.BeginRead(now, 7, 1);
    }
    if (r != kNoTicket && r_end == 0 && bitmap.Done(r)) {
      r_end = now;
      value = bitmap.Value(r);
      bitmap.Finish(r);
      r = kNoTicket;
    }
  });

  clk->Continue(100 * kPeriod);
  RT::JoinAll();

  EXPECT_EQ(value, 3u);
  EXPECT_EQ(w_end - t0, tb.params.bitmap_access_time);
  EXPECT_EQ(r_end - w_end, tb.params.bitmap_access_time);
  // 一次读一次写，各记一条占用
  EXPECT_EQ(rec.Spans().size(), 2u);
}

TEST(BachBitMap, OnePortSerializesConcurrentReads) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  SpanRecorder rec;
  MoeBitMap bitmap(clk, tb.Context(&rec), "bitmap", 0);
  bitmap.SeedDefault(5, 0);

  Ticket a = kNoTicket, b = kNoTicket;
  Time t0 = 0, a_end = 0, b_end = 0;
  StepDriver drv(clk, [&](Time now, uint64_t k) {
    bitmap.Step();
    if (k == 0) {
      t0 = now;
      a = bitmap.BeginRead(now, 5, 0);
      b = bitmap.BeginRead(now, 5, 1);
    }
    if (a != kNoTicket && a_end == 0 && bitmap.Done(a)) a_end = now;
    if (b != kNoTicket && b_end == 0 && bitmap.Done(b)) b_end = now;
  });

  clk->Continue(100 * kPeriod);
  RT::JoinAll();

  EXPECT_EQ(a_end - t0, tb.params.bitmap_access_time);
  EXPECT_EQ(b_end - a_end, tb.params.bitmap_access_time);
  // 后一个等前一个的那一段被记成等待
  ASSERT_EQ(rec.Waits().size(), 1u);
  EXPECT_EQ(rec.Waits()[0].reason, WaitReason::kBitmapAccess);
}

// ---------------------------------------------------------------- 计算核

TEST(BachCompute, OneTaskCostsSetupPlusBitmapPlusCompute) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  tb.Task(UnitType::kMc, Opcode::kDontCare, 0, 100);
  SpanRecorder rec;
  CoreContext ctx = tb.Context(&rec);
  MoeBitMap bitmap(clk, ctx, "bitmap", 0);
  MatrixCore mc(clk, ctx, "matrix", 0);
  FakeScheduler sched;
  mc.Connect(&sched, &bitmap);
  bitmap.SeedDefault(1, 0);

  Time t0 = 0;
  StepDriver drv(clk, [&](Time now, uint64_t k) {
    bitmap.Step();
    mc.Step();
    if (k == 0) {
      t0 = now;
      mc.Dispatch(now, 1, 0, 0);
    }
  });

  clk->Continue(500 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sched.acks.size(), 1u);
  EXPECT_EQ(sched.acks[0].at - t0,
            tb.params.mu_setup_time + tb.params.bitmap_access_time + 100);
  EXPECT_EQ(mc.InFlight(), 0u);
}

TEST(BachCompute, MultiplierScalesComputeTime) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  tb.Task(UnitType::kVc, Opcode::kDontCare, 0, 50);
  SpanRecorder rec;
  CoreContext ctx = tb.Context(&rec);
  MoeBitMap bitmap(clk, ctx, "bitmap", 0);
  VectorCore vc(clk, ctx, "vector", 0);
  FakeScheduler sched;
  vc.Connect(&sched, &bitmap);
  bitmap.SeedDefault(1, 4);

  Time t0 = 0;
  StepDriver drv(clk, [&](Time now, uint64_t k) {
    bitmap.Step();
    vc.Step();
    if (k == 0) {
      t0 = now;
      vc.Dispatch(now, 1, 0, 0);
    }
  });

  clk->Continue(500 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sched.acks.size(), 1u);
  EXPECT_EQ(sched.acks[0].at - t0,
            tb.params.vu_setup_time + tb.params.bitmap_access_time + 50 * 4);
}

TEST(BachCompute, AdmissionWindowBoundsTasksInThePipe) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  tb.params.setup_ahead_depth = 1;
  tb.Task(UnitType::kMc, Opcode::kDontCare, 0, 100);
  SpanRecorder rec;
  CoreContext ctx = tb.Context(&rec);
  MoeBitMap bitmap(clk, ctx, "bitmap", 0);
  MatrixCore mc(clk, ctx, "matrix", 0);
  FakeScheduler sched;
  mc.Connect(&sched, &bitmap);
  for (uint64_t uid = 1; uid <= 3; ++uid) bitmap.SeedDefault(uid, 0);

  Time t0 = 0;
  StepDriver drv(clk, [&](Time now, uint64_t k) {
    bitmap.Step();
    mc.Step();
    if (k == 0) {
      t0 = now;
      mc.Dispatch(now, 1, 0, 0);
      mc.Dispatch(now, 2, 0, 1);
      mc.Dispatch(now, 3, 0, 2);
    }
  });

  clk->Continue(1000 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sched.acks.size(), 3u);
  // 后一条拿到执行通道之后才去读倍率，所以间隔是读表加计算，不只是计算
  const uint64_t step = tb.params.bitmap_access_time + 100;
  EXPECT_EQ(sched.acks[1].at - sched.acks[0].at, step);
  EXPECT_EQ(sched.acks[2].at - sched.acks[1].at, step);
  // 优先级小的先进 setup，所以完成顺序就是派发顺序
  EXPECT_EQ(sched.acks[0].uid, 1u);
  EXPECT_EQ(sched.acks[2].uid, 3u);

  // 准入窗容量是二：第三条的 setup 要等第一条整条做完才开始，而不是等 setup 单元
  // 空出来就开始
  std::vector<UnitSpan> setups;
  for (UnitSpan const& s : rec.Spans()) {
    if (s.state == SpanState::kSetup) setups.push_back(s);
  }
  ASSERT_EQ(setups.size(), 3u);
  EXPECT_EQ(setups[1].start - t0, tb.params.mu_setup_time);
  EXPECT_EQ(setups[2].start - t0, sched.acks[0].at - t0);
}

// ---------------------------------------------------------------- 额度

TEST(BachCredit, HoldsTaskUntilDownstreamReturns) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  tb.Task(UnitType::kCu, Opcode::kDontCare, 0, 0).Credit(0, {9});
  SpanRecorder rec;
  CoreContext ctx = tb.Context(&rec);
  CreditUnit cu(clk, ctx, "credit", 0);
  FakeScheduler sched;
  cu.Connect(&sched);
  cu.AddDownstream(9, 1, CoreType::kNormal);

  Time t0 = 0, give_back = 0;
  StepDriver drv(clk, [&](Time now, uint64_t k) {
    // 真实的 Core 里 DTE 排在 CreditUnit 前面，归还与验资落在同一拍，这里照此排序
    if (now == t0 + 50 && k != 0) {
      give_back = now;
      cu.ReturnCredit(9, 1);
    }
    cu.Step();
    if (k == 0) {
      t0 = now;
      cu.Dispatch(now, 1, 0);
      cu.Dispatch(now, 2, 0);
    }
  });

  clk->Continue(500 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sched.acks.size(), 2u);
  // 第一条查完账就拿到了唯一那份额度
  EXPECT_EQ(sched.acks[0].at - t0, tb.params.credit_check_time);
  // 第二条一直等到额度还回来。查账锁被第一条持到它验完，所以它的 4 拍从那时才起算
  EXPECT_EQ(sched.acks[1].at, give_back);
  EXPECT_EQ(cu.Level(9), 0u);

  bool saw_lock_wait = false, saw_credit_wait = false;
  for (UnitWait const& w : rec.Waits()) {
    if (w.reason == WaitReason::kCreditLock) saw_lock_wait = true;
    if (w.reason == WaitReason::kDownstreamCredit) saw_credit_wait = true;
  }
  EXPECT_TRUE(saw_lock_wait);
  EXPECT_TRUE(saw_credit_wait);
}

TEST(BachCredit, AllDownstreamsMustBeInHandTogether) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  tb.Task(UnitType::kCu, Opcode::kDontCare, 0, 0).Credit(0, {9, 10});
  SpanRecorder rec;
  CoreContext ctx = tb.Context(&rec);
  CreditUnit cu(clk, ctx, "credit", 0);
  FakeScheduler sched;
  cu.Connect(&sched);
  cu.AddDownstream(9, 2, CoreType::kNormal);
  cu.AddDownstream(10, 1, CoreType::kNormal);

  Time t0 = 0;
  uint64_t level_nine_mid = 99;
  StepDriver drv(clk, [&](Time now, uint64_t k) {
    if (now == t0 + 40 && k != 0) cu.ReturnCredit(10, 1);
    cu.Step();
    if (k == 0) {
      t0 = now;
      cu.Dispatch(now, 1, 0);   // 两个下游各扣一份
      cu.Dispatch(now, 2, 0);   // 只能扣到 9 那份，卡在等 10
    }
    if (now == t0 + 30) level_nine_mid = cu.Level(9);
  });

  clk->Continue(500 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sched.acks.size(), 2u);
  // 先到手的那份一直被占着，这就是单点拥塞放大成扇出范围停顿的地方
  EXPECT_EQ(level_nine_mid, 0u);
  EXPECT_EQ(sched.acks[1].at, t0 + 40);
}

// ---------------------------------------------------------------- 搬运

TEST(BachDte, SendTakesOneBeatPerBandwidthWorth) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  tb.Task(UnitType::kDte, Opcode::kMove, 5, 2048, Coord{1, 2});
  SpanRecorder rec;
  CoreContext ctx = tb.Context(&rec);
  MoeBitMap bitmap(clk, ctx, "bitmap", 0);
  MemorySystem mem(clk, ctx, "mem", 0);
  Dte dte(clk, ctx, "dte", 0);
  FakeScheduler sched;
  FakeCredit credit;
  FakeSink sink;
  dte.Connect(&sched, &credit, &sink, &bitmap, &mem);

  Time t0 = 0;
  StepDriver drv(clk, [&](Time now, uint64_t k) {
    bitmap.Step();
    mem.Step();
    dte.Step();
    if (k == 0) {
      t0 = now;
      dte.DispatchComm(now, 1, 0, 0);
    }
  });

  clk->Continue(500 * kPeriod);
  RT::JoinAll();

  // 2048 字节按 512 的出口带宽是四拍，每拍一个封包
  ASSERT_EQ(sink.sent.size(), 4u);
  EXPECT_EQ(sink.sent[0].at - t0, tb.params.dte_setup_time);
  EXPECT_EQ(sink.sent[3].at - sink.sent[0].at, 3u);
  EXPECT_EQ(sink.sent[0].size, 512u);
  EXPECT_EQ(sink.sent[0].dst.row, 1);
  EXPECT_EQ(sink.sent[0].dst.col, 2);
  EXPECT_EQ(sink.sent[0].payload->total_fragments, 4u);
  EXPECT_EQ(sink.sent[3].payload->beat_id, 3u);
  // 每一拍有自己的传输实例号，重组时才分得开
  EXPECT_NE(sink.sent[0].payload->xfer_id, sink.sent[1].payload->xfer_id);

  ASSERT_EQ(sched.acks.size(), 1u);
  EXPECT_EQ(sched.acks[0].at - t0, tb.params.dte_setup_time + 4);
}

TEST(BachDte, IncomingPacketIsAckedOnlyWhenAllBeatsArrived) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  tb.Task(UnitType::kSkip, Opcode::kUserInit, 0, 1024);
  SpanRecorder rec;
  CoreContext ctx = tb.Context(&rec);
  MoeBitMap bitmap(clk, ctx, "bitmap", 0);
  MemorySystem mem(clk, ctx, "mem", 0);
  Dte dte(clk, ctx, "dte", 0);
  FakeScheduler sched;
  FakeCredit credit;
  FakeSink sink;
  dte.Connect(&sched, &credit, &sink, &bitmap, &mem);

  Time t0 = 0, last_beat = 0;
  StepDriver drv(clk, [&](Time now, uint64_t k) {
    bitmap.Step();
    mem.Step();
    dte.Step();
    if (k == 0) t0 = now;
    const uint64_t beat = now - t0;
    if (beat < 2) {
      CommInstPtr c = std::make_shared<CommInst>();
      c->uid = 1;
      c->tid = 0;
      c->opcode = Opcode::kUserInit;
      c->beat_id = beat;
      c->total_fragments = 2;
      c->tag = 0;
      dte.HandleComm(c);
      last_beat = now;
    }
  });

  clk->Continue(500 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sched.acks.size(), 1u);
  // 收齐那一拍才起流水：setup 之后写一次倍率表，写完才 ack
  EXPECT_EQ(sched.acks[0].at - last_beat,
            tb.params.dte_setup_time + tb.params.bitmap_access_time);
  EXPECT_TRUE(dte.HasSlot(1));
  EXPECT_EQ(dte.Storage(), StorageMode::kCore);
  EXPECT_EQ(dte.FreeSlotNum(), dte.TotalSlots() - 1);
}

TEST(BachDte, RetirePacketReturnsCreditWithoutQueueing) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  tb.Task(UnitType::kSkip, Opcode::kUserInit, 0, 1024);
  SpanRecorder rec;
  CoreContext ctx = tb.Context(&rec);
  MoeBitMap bitmap(clk, ctx, "bitmap", 0);
  MemorySystem mem(clk, ctx, "mem", 0);
  Dte dte(clk, ctx, "dte", 0);
  FakeScheduler sched;
  FakeCredit credit;
  FakeSink sink;
  dte.Connect(&sched, &credit, &sink, &bitmap, &mem);

  Time t0 = 0, sent_at = 0;
  StepDriver drv(clk, [&](Time now, uint64_t k) {
    bitmap.Step();
    mem.Step();
    dte.Step();
    if (k == 0) {
      t0 = now;
      CommInstPtr c = std::make_shared<CommInst>();
      c->uid = 1;
      c->tid = 3;
      c->opcode = Opcode::kRetire;
      c->tag = 6;
      c->beat_id = 0;
      c->total_fragments = 1;
      sent_at = now;
      dte.HandleComm(c);
    }
  });

  clk->Continue(200 * kPeriod);
  RT::JoinAll();

  // 归还额度不排队、不占资源，收下那一刻就还回去了
  ASSERT_EQ(credit.returned.size(), 1u);
  EXPECT_EQ(credit.returned[0].at, sent_at);
  EXPECT_EQ(credit.returned[0].core_id, 6);
  EXPECT_TRUE(sched.acks.empty());
  EXPECT_EQ(dte.InFlight(), 0u);
}

TEST(BachDte, ReduceCostsTheFixedAddTime) {
  RT::Reset(2, 2);
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreTables tb;
  tb.Task(UnitType::kSkip, Opcode::kReduce, 0, 1024);
  SpanRecorder rec;
  CoreContext ctx = tb.Context(&rec);
  MoeBitMap bitmap(clk, ctx, "bitmap", 0);
  MemorySystem mem(clk, ctx, "mem", 0);
  Dte dte(clk, ctx, "dte", 0);
  FakeScheduler sched;
  FakeCredit credit;
  FakeSink sink;
  dte.Connect(&sched, &credit, &sink, &bitmap, &mem);

  Time t0 = 0;
  StepDriver drv(clk, [&](Time now, uint64_t k) {
    bitmap.Step();
    mem.Step();
    dte.Step();
    if (k == 0) {
      t0 = now;
      CommInstPtr c = std::make_shared<CommInst>();
      c->uid = 1;
      c->tid = 0;
      c->opcode = Opcode::kReduce;
      c->beat_id = 0;
      c->total_fragments = 1;
      dte.HandleComm(c);
    }
  });

  clk->Continue(500 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sched.acks.size(), 1u);
  EXPECT_EQ(sched.acks[0].at - t0,
            tb.params.dte_setup_time + tb.params.dte_reduce_time);
}
