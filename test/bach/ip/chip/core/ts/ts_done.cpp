// 七路完成事件的合流。
//
// 哪一路才算数由当前任务的 TASK_RECV_UNIT 定；Reduce 任务的完成拆成两半，只有
// Router 的 Reduce Done 有权把它置 FINISH；datain 的完成只点亮 done_bitmap，
// 不推进 task_id。Router 那一路不带 stream_id，按 user_id 找 Stream。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/ts/stream_table.h"
#include "bach/ip/chip/core/ts/task_done.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

TaskEntry Step(RecvUnit recv) {
  TaskEntry t;
  t.send_unit = SendUnit::kDte;
  t.recv_unit = recv;
  t.dsa_en = true;
  t.exe_mask = true;
  t.task_pc = 0x100;
  return t;
}

std::shared_ptr<StreamWrite> Infly(uint64_t slot, uint64_t user,
                                   uint64_t task_id) {
  auto w = std::make_shared<StreamWrite>();
  w->valid = true;
  w->stream_id = slot;
  w->whole = true;
  w->entry.valid = true;
  w->entry.user_id = user;
  w->entry.user_id_vld = true;
  w->entry.compute = true;
  w->entry.task_id = task_id;
  w->entry.task_fsm = TaskFsm::kInfly;
  return w;
}

class DoneBench : public BachModule {
 public:
  struct Ack {
    uint64_t at = 0;
    uint64_t unit = 0;
    bool from_dsa = false;
    uint64_t stream = 0, task = 0, seq = 0;
  };
  struct RouterAck {
    uint64_t at = 0, user = 0, seq = 0;
  };
  struct Seed {
    uint64_t at = 0;
    std::shared_ptr<StreamWrite> w;
  };

  DoneBench(ClockPtr c, CfgReg& reg, StreamTable& tab, TaskDone& td)
      : BachModule(c, "bench"), cfg(reg), table(tab), done(td) {}

  std::vector<Seed> seeds;
  std::vector<Ack> acks;
  std::vector<RouterAck> router_acks;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    for (auto const& s : seeds) {
      if (s.at == now) table.Port(kWrCreate).Drive(s.w);
    }
    for (uint64_t u = 0; u < 3; ++u) {
      done.RvDone(u).Idle();
      done.DsaDone(u).Idle();
    }
    done.RdcDone().Idle();
    for (auto const& a : acks) {
      if (a.at != now) continue;
      if (a.from_dsa) {
        done.DsaDone(a.unit).Drive(a.stream, a.task, a.seq);
      } else {
        done.RvDone(a.unit).Drive(a.stream, a.task, a.seq);
      }
    }
    for (auto const& r : router_acks) {
      if (r.at == now) done.RdcDone().Drive(r.user, r.seq);
    }
    cfg.RunStep();
    done.RunStep();
    table.RunStep();
  }

 private:
  CfgReg& cfg;
  StreamTable& table;
  TaskDone& done;
};

struct Bench {
  ClockPtr clk;
  std::unique_ptr<CfgReg> cfg;
  std::unique_ptr<StreamTable> table;
  std::unique_ptr<TaskDone> done;

  explicit Bench(ClockPtr c) : clk(c) {
    cfg = std::make_unique<CfgReg>(c, "cfg", 0, false);
    table = std::make_unique<StreamTable>(c, "table", 0, false);
    done = std::make_unique<TaskDone>(c, "done", *cfg, 0, false);
    done->AttachSnapshot(table->SnapPtr());
    table->Rebind(kWrCompletion, done->CompletionPtr());
  }
};

}  // namespace

// 只调 RV core 的任务：RV core 的 ack 算数，DSA 那一路不算。
TEST(BachTsDone, RvOnlyTakesTheRvAck) {
  TaskFsm after_dsa = TaskFsm::kIdle, after_rv = TaskFsm::kIdle;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.cfg->WriteTask(0, Step(RecvUnit::kRvOnly));
    DoneBench h(clk, *b.cfg, *b.table, *b.done);
    h.seeds = {{2, Infly(0, 11, 0)}};
    h.acks = {{6, 0, /*from_dsa=*/true, 0, 0, 0}};
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    after_dsa = b.table->Peek(0).task_fsm;
  }
  RT::Reset();
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.cfg->WriteTask(0, Step(RecvUnit::kRvOnly));
    DoneBench h(clk, *b.cfg, *b.table, *b.done);
    h.seeds = {{2, Infly(0, 11, 0)}};
    h.acks = {{6, 0, /*from_dsa=*/false, 0, 0, 0}};
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    after_rv = b.table->Peek(0).task_fsm;
  }
  RT::Reset();
  EXPECT_EQ(after_dsa, TaskFsm::kInfly) << "DSA 那一路不算数";
  EXPECT_EQ(after_rv, TaskFsm::kFinish);
}

// 两者都要的任务：等 RV core 与 DSA 都报到才置 FINISH。
TEST(BachTsDone, BothAcksAreNeededWhenRecvIsBoth) {
  TaskFsm one = TaskFsm::kIdle, both = TaskFsm::kIdle;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.cfg->WriteTask(0, Step(RecvUnit::kDteDsaRmem));
    DoneBench h(clk, *b.cfg, *b.table, *b.done);
    h.seeds = {{2, Infly(0, 11, 0)}};
    h.acks = {{6, 0, /*from_dsa=*/true, 0, 0, 0}};
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    one = b.table->Peek(0).task_fsm;
  }
  RT::Reset();
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.cfg->WriteTask(0, Step(RecvUnit::kDteDsaRmem));
    DoneBench h(clk, *b.cfg, *b.table, *b.done);
    h.seeds = {{2, Infly(0, 11, 0)}};
    h.acks = {{6, 0, /*from_dsa=*/true, 0, 0, 0},
              {8, 0, /*from_dsa=*/false, 0, 0, 0}};
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    both = b.table->Peek(0).task_fsm;
  }
  RT::Reset();
  EXPECT_EQ(one, TaskFsm::kInfly) << "只到一边不算完";
  EXPECT_EQ(both, TaskFsm::kFinish);
}

// Reduce 任务：DTE 的 ack 只代表搬运完成，只有 Router 的 Reduce Done 才有权
// 把它置 FINISH。
TEST(BachTsDone, ReduceNeedsRouterDoneToFinish) {
  TaskFsm dte_only = TaskFsm::kIdle, with_router = TaskFsm::kIdle;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    TaskEntry t = Step(RecvUnit::kDsa);
    t.reduce = true;
    t.reduce_num = 1;
    b.cfg->WriteTask(0, t);
    DoneBench h(clk, *b.cfg, *b.table, *b.done);
    h.seeds = {{2, Infly(0, 11, 0)}};
    h.acks = {{6, 0, /*from_dsa=*/true, 0, 0, 0}};
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    dte_only = b.table->Peek(0).task_fsm;
  }
  RT::Reset();
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    TaskEntry t = Step(RecvUnit::kDsa);
    t.reduce = true;
    t.reduce_num = 1;
    b.cfg->WriteTask(0, t);
    DoneBench h(clk, *b.cfg, *b.table, *b.done);
    h.seeds = {{2, Infly(0, 11, 0)}};
    h.acks = {{6, 0, /*from_dsa=*/true, 0, 0, 0}};
    h.router_acks = {{8, 11, 0}};
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    with_router = b.table->Peek(0).task_fsm;
  }
  RT::Reset();
  EXPECT_EQ(dte_only, TaskFsm::kInfly) << "只有 DTE ack 时不许置 FINISH";
  EXPECT_EQ(with_router, TaskFsm::kFinish);
}

// Router 那一路不带 stream_id，按 user_id 找对应的 Stream。
TEST(BachTsDone, RouterDoneFindsTheStreamByUserId) {
  TaskFsm s1 = TaskFsm::kIdle, s2 = TaskFsm::kIdle;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    TaskEntry t = Step(RecvUnit::kDsa);
    t.reduce = true;
    t.reduce_num = 1;
    b.cfg->WriteTask(0, t);
    DoneBench h(clk, *b.cfg, *b.table, *b.done);
    // 两个 stream，Router 报的是第二个用户。
    h.seeds = {{2, Infly(0, 11, 0)}, {3, Infly(1, 22, 0)}};
    h.acks = {{6, 0, true, 0, 0, 0}, {7, 0, true, 1, 0, 0}};
    h.router_acks = {{10, 22, 0}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    s1 = b.table->Peek(0).task_fsm;
    s2 = b.table->Peek(1).task_fsm;
  }
  RT::Reset();
  EXPECT_EQ(s1, TaskFsm::kInfly) << "没报到的那个不动";
  EXPECT_EQ(s2, TaskFsm::kFinish) << "按 user_id 找到了第二个 stream";
}

// datain 的完成只点亮 done_bitmap 对应位，不推进 task_id、不改状态机。
TEST(BachTsDone, DataInOnlyLightsTheBit) {
  uint64_t bitmap = 0, task_id = 0;
  TaskFsm fsm = TaskFsm::kIdle;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.cfg->WriteTask(0, Step(RecvUnit::kDsa));
    TaskEntry din = Step(RecvUnit::kDsa);
    din.wait_wake = true;
    b.cfg->WriteTask(3, din);
    DoneBench h(clk, *b.cfg, *b.table, *b.done);
    // 用户停在 task 0，报的是 task 3 那个 datain 完成了。
    h.seeds = {{2, Infly(0, 11, 0)}};
    h.acks = {{6, 0, /*from_dsa=*/true, 0, 3, 0}};
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    bitmap = b.table->Peek(0).done_bitmap;
    task_id = b.table->Peek(0).task_id;
    fsm = b.table->Peek(0).task_fsm;
  }
  RT::Reset();
  EXPECT_EQ(bitmap, 1ull << 3) << "只点亮那一位";
  EXPECT_EQ(task_id, 0u) << "不推进 task_id";
  EXPECT_EQ(fsm, TaskFsm::kInfly) << "主线的状态机不动";
}
