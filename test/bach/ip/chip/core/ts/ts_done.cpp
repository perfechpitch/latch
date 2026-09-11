// 七路完成事件的合流。
//
// 哪几路才算数由当前任务的 TASK_RECV_UNIT 定：00 只要 RV core 的 ACK，01 要同一
// 个执行单元的 RV core 与 DSA 两路都到。逐级 reduce 任务只认 Router 的 Reduce
// Done，Router 那一路不带 stream_id，按 user_id 找 Stream。datain 的完成只点亮
// done_bitmap，不推进 task_id。权重加载期间的 ACK、自启动 core 上 Bypass 那一路
// 带保留身份的 ACK 都丢掉。

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
    uint64_t stream = 0, task = 0, user = 0, pid = 0;
  };
  struct RouterAck {
    uint64_t at = 0, user = 0;
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
        done.DsaDone(a.unit).Drive(a.stream, a.task);
      } else {
        done.RvDone(a.unit).Drive(a.stream, a.task, a.user, a.pid);
      }
    }
    for (auto const& r : router_acks) {
      if (r.at == now) done.RdcDone().Drive(r.user, 0);
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

// 一条单项任务链跑一遍，返回那一项的表项。
StreamEntry RunOne(TaskEntry const& t, std::vector<DoneBench::Ack> acks,
                   std::vector<DoneBench::RouterAck> router_acks = {},
                   std::shared_ptr<StreamWrite> seed = nullptr) {
  StreamEntry got;
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  Bench b(clk);
  b.cfg->WriteTask(0, t);
  DoneBench h(clk, *b.cfg, *b.table, *b.done);
  h.seeds = {{2, seed ? seed : Infly(0, 11, 0)}};
  h.acks = std::move(acks);
  h.router_acks = std::move(router_acks);
  clk->Continue(20 * kPeriod);
  RT::JoinAll();
  got = b.table->Peek(0);
  RT::Reset();
  return got;
}

}  // namespace

// 只调 RV core 的任务：RV core 的 ack 算数，DSA 那一路不算。
TEST(BachTsDone, RvOnlyTakesTheRvAck) {
  StreamEntry dsa = RunOne(Step(RecvUnit::kRvOnly), {{6, 0, true, 0, 0}});
  StreamEntry rv = RunOne(Step(RecvUnit::kRvOnly), {{6, 0, false, 0, 0}});
  EXPECT_EQ(dsa.task_fsm, TaskFsm::kInfly) << "DSA 那一路不算数";
  EXPECT_EQ(rv.task_fsm, TaskFsm::kFinish);
  EXPECT_EQ(rv.done_bitmap, 1u);
}

// RV core 与 DSA 两路都收的任务：两路都到才置 FINISH，先到哪一路都一样。
TEST(BachTsDone, BothAcksAreNeededWhenRecvIsDsa) {
  StreamEntry one = RunOne(Step(RecvUnit::kDsa), {{6, 0, true, 0, 0}});
  StreamEntry core_first =
      RunOne(Step(RecvUnit::kDsa), {{6, 0, false, 0, 0}, {8, 0, true, 0, 0}});
  StreamEntry dsa_first =
      RunOne(Step(RecvUnit::kDsa), {{6, 0, true, 0, 0}, {8, 0, false, 0, 0}});
  EXPECT_EQ(one.task_fsm, TaskFsm::kInfly) << "只到一路不算完";
  EXPECT_EQ(core_first.task_fsm, TaskFsm::kFinish);
  EXPECT_EQ(dsa_first.task_fsm, TaskFsm::kFinish);
}

// 两路要来自同一个执行单元：DTE 的 RV core ACK 配 MU 的 DSA ACK 不算。
TEST(BachTsDone, AcksOfDifferentUnitsDoNotPair) {
  StreamEntry got =
      RunOne(Step(RecvUnit::kDsa), {{6, 0, false, 0, 0}, {8, 1, true, 0, 0}});
  EXPECT_EQ(got.task_fsm, TaskFsm::kInfly);
}

// 逐级 reduce 任务：本地两路 ACK 只代表搬运完成，只有 Router 的 Reduce Done
// 才把它置 FINISH，同时还回本级 Rmem 的 credit。
TEST(BachTsDone, ReduceNeedsRouterDoneToFinish) {
  TaskEntry t = Step(RecvUnit::kDsa);
  t.task_type = TaskType::kReduce;
  t.credit_en = true;
  auto busy = [] {
    auto w = Infly(0, 11, 0);
    w->entry.rmem_busy = true;
    return w;
  };
  StreamEntry local =
      RunOne(t, {{6, 0, false, 0, 0}, {7, 0, true, 0, 0}}, {}, busy());
  StreamEntry router = RunOne(t, {{6, 0, false, 0, 0}, {7, 0, true, 0, 0}},
                              {{10, 11}}, busy());
  EXPECT_EQ(local.task_fsm, TaskFsm::kInfly) << "本地 ACK 不许置 FINISH";
  EXPECT_TRUE(local.rmem_busy);
  EXPECT_EQ(router.task_fsm, TaskFsm::kFinish);
  EXPECT_FALSE(router.rmem_busy) << "做完才还 Rmem 的 credit";
}

// Router 那一路不带 stream_id，按 user_id 找对应的 Stream。
TEST(BachTsDone, RouterDoneFindsTheStreamByUserId) {
  TaskFsm s1 = TaskFsm::kIdle, s2 = TaskFsm::kIdle;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    TaskEntry t = Step(RecvUnit::kDsa);
    t.task_type = TaskType::kReduce;
    t.credit_en = true;
    b.cfg->WriteTask(0, t);
    DoneBench h(clk, *b.cfg, *b.table, *b.done);
    // 两个 stream，Router 报的是第二个用户。
    h.seeds = {{2, Infly(0, 11, 0)}, {3, Infly(1, 22, 0)}};
    h.router_acks = {{10, 22}};
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
  StreamEntry got;
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
    h.acks = {{6, 0, true, 0, 3}, {7, 0, false, 0, 3}};
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    got = b.table->Peek(0);
  }
  RT::Reset();
  EXPECT_EQ(got.done_bitmap, 1ull << 3) << "只点亮那一位";
  EXPECT_EQ(got.task_id, 0u) << "不推进 task_id";
  EXPECT_EQ(got.task_fsm, TaskFsm::kInfly) << "主线的状态机不动";
}

// 权重加载期间的完成事件全部丢掉。
TEST(BachTsDone, WeightsModeDropsEveryAck) {
  StreamEntry got;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.cfg->WriteTask(0, Step(RecvUnit::kRvOnly));
    b.cfg->WriteDatainTask(0x300, /*weights_mode=*/true);
    DoneBench h(clk, *b.cfg, *b.table, *b.done);
    h.seeds = {{2, Infly(0, 11, 0)}};
    h.acks = {{6, 0, false, 0, 0}};
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    got = b.table->Peek(0);
  }
  RT::Reset();
  EXPECT_EQ(got.task_fsm, TaskFsm::kInfly);
  EXPECT_EQ(got.done_bitmap, 0u);
}

// 自启动 core 上 Bypass datain 带 SID 15 / TID 63 回来的 ACK 丢掉；普通 core 上
// 同样的号照常更新表项。
TEST(BachTsDone, BypassAckIsDroppedOnSelfStartCore) {
  uint64_t self_start_bits = 0, normal_bits = 0;
  for (bool self_start : {true, false}) {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.cfg->SetSelfStart(self_start);
    b.cfg->WriteTask(0, Step(RecvUnit::kRvOnly));
    DoneBench h(clk, *b.cfg, *b.table, *b.done);
    h.seeds = {{2, Infly(kBypassSid, 11, 0)}};
    h.acks = {{6, 0, false, kBypassSid, kBypassTid}};
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    (self_start ? self_start_bits : normal_bits) =
        b.table->Peek(kBypassSid).done_bitmap;
    RT::Reset();
  }
  EXPECT_EQ(self_start_bits, 0u);
  EXPECT_EQ(normal_bits, 1ull << kBypassTid);
}

// PID 更新任务：RV core 的 ACK 带回新 PID，这一笔完成时写进表项，等紧邻后继继承。
TEST(BachTsDone, PidUpdateTakesThePidFromTheCoreAck) {
  TaskEntry t = Step(RecvUnit::kDsa);
  t.task_type = TaskType::kPidUpdate;
  t.path_id = 5;
  StreamEntry got =
      RunOne(t, {{6, 0, false, 0, 0, 11, 42}, {8, 0, true, 0, 0}});
  EXPECT_EQ(got.task_fsm, TaskFsm::kFinish);
  EXPECT_EQ(got.task_path_id, 42u);
  EXPECT_TRUE(got.pid_pending);
}

// 自启动 core 建表时没有用户号，Task 0 的 RV core ACK 带回来时补进表项。
TEST(BachTsDone, Task0CoreAckFillsTheUserOnSelfStartCore) {
  auto seed = Infly(0, 0, 0);
  seed->entry.user_id_vld = false;
  StreamEntry got =
      RunOne(Step(RecvUnit::kRvOnly), {{6, 0, false, 0, 0, 77}}, {}, seed);
  EXPECT_TRUE(got.user_id_vld);
  EXPECT_EQ(got.user_id, 77u);
}
