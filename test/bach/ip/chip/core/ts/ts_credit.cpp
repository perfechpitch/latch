// TS 这一侧的 credit 与退休。
//
// 退休是 Head-only 的：head_ptr 那一项第 0 项到 End 项的完成位全部置起才退。顺序
// 不能反，先向 Router 发 credit 返还请求，Router 收下之后才清该槽位的 valid 并推
// head_ptr。credit 申请只对 CREDIT_EN 的普通任务发，带 UserID、StreamID、TaskID、
// PathID 四个号；逐级 reduce 任务要的是本级 Rmem 的 credit，TS 自己记。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/ts/credit_monitor.h"
#include "bach/ip/chip/core/ts/stream_table.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

TaskEntry Step(bool credit_en, uint64_t path_id = 0, bool end = false) {
  TaskEntry t;
  t.send_unit = SendUnit::kDte;
  t.recv_unit = RecvUnit::kDsa;
  t.credit_en = credit_en;
  t.path_id = path_id;
  t.end = end;
  t.task_pc = 0x100;
  return t;
}

// 一项就是 End 的链。
void OneTaskChain(CfgReg& cfg) {
  cfg.WriteTask(0, Step(false, 0, true));
  cfg.SetInitFinish();
}

std::shared_ptr<StreamWrite> Sitting(uint64_t slot, uint64_t user,
                                     uint64_t task_id, TaskFsm fsm,
                                     uint64_t done = 0) {
  auto w = std::make_shared<StreamWrite>();
  w->valid = true;
  w->stream_id = slot;
  w->whole = true;
  w->entry.valid = true;
  w->entry.user_id = user;
  w->entry.user_id_vld = true;
  w->entry.task_id = task_id;
  w->entry.task_fsm = fsm;
  w->entry.done_bitmap = done;
  return w;
}

class CreditBench : public BachModule {
 public:
  struct Seed {
    uint64_t at = 0;
    std::shared_ptr<StreamWrite> w;
  };
  struct GrantJob {
    uint64_t at = 0, stream = 0, task = 0, path = 0;
  };

  CreditBench(ClockPtr c, CfgReg& reg, StreamTable& tab, TsCreditMonitor& cm)
      : BachModule(c, "bench"), cfg(reg), table(tab), credit(cm) {}

  std::vector<Seed> seeds;
  std::vector<GrantJob> grants;
  // 这一拍之前 Router 不收退休请求。
  uint64_t retire_accept_from = 0;

  struct Req {
    uint64_t at = 0, user = 0, stream = 0, task = 0, path = 0;
  };
  std::vector<Req> reqs;
  std::vector<uint64_t> retire_users, retire_at;
  // 计数器是打拍的，只能在仿真里读。
  uint64_t rmem_admitted = 0;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做：先看上一拍摆出来的申请与退休请求。
    if (credit.Req().req_valid.Get() != 0) {
      Req r{now, credit.Req().user_id.Get(), credit.Req().stream_id.Get(),
            credit.Req().task_id.Get(), credit.Req().path_id.Get()};
      if (reqs.empty() || reqs.back().stream != r.stream ||
          reqs.back().task != r.task) {
        reqs.push_back(r);
      }
    }
    credit.Req().DriveReady(true);

    bool ok = now >= retire_accept_from;
    if (credit.RetireReq().Valid()) {
      uint64_t u = credit.RetireReq().user_id.Get();
      if (retire_users.empty() || retire_users.back() != u) {
        retire_users.push_back(u);
        retire_at.push_back(now);
      }
    }
    credit.RetireReq().DriveAccepted(ok);

    credit.Grant().Idle();
    for (auto const& g : grants) {
      if (g.at == now) credit.Grant().Drive(g.stream, g.task, g.path);
    }
    for (auto const& s : seeds) {
      if (s.at == now) table.Port(kWrCreate).Drive(s.w);
    }
    cfg.RunStep();
    credit.RunStep();
    table.RunStep();
    rmem_admitted = credit.RmemAdmitted();
  }

 private:
  CfgReg& cfg;
  StreamTable& table;
  TsCreditMonitor& credit;
};

struct Bench {
  ClockPtr clk;
  std::unique_ptr<CfgReg> cfg;
  std::unique_ptr<StreamTable> table;
  std::unique_ptr<TsCreditMonitor> credit;

  explicit Bench(ClockPtr c) : clk(c) {
    cfg = std::make_unique<CfgReg>(c, "cfg", 0, false);
    table = std::make_unique<StreamTable>(c, "table", 0, false);
    credit = std::make_unique<TsCreditMonitor>(c, "credit", *cfg, 0, false);
    credit->AttachSnapshot(table->SnapPtr());
    table->Rebind(kWrCreditWake, credit->WakePtr());
    table->Rebind(kWrRetirement, credit->RetireWrPtr());
  }
};

}  // namespace

// credit 申请只对 CREDIT_EN 的 task 发，且带上四个号。
TEST(BachTsCredit, RequestCarriesTheFourIds) {
  std::vector<CreditBench::Req> reqs;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.cfg->WriteTask(0, Step(/*credit_en=*/false));
    b.cfg->WriteTask(1, Step(/*credit_en=*/true, /*path_id=*/6));
    CreditBench h(clk, *b.cfg, *b.table, *b.credit);
    // stream0 停在不要 credit 的 task 0，stream1 停在要 credit 的 task 1。
    h.seeds = {{2, Sitting(0, 11, 0, TaskFsm::kWait)},
               {3, Sitting(1, 22, 1, TaskFsm::kWait)}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    reqs = h.reqs;
  }
  RT::Reset();
  ASSERT_EQ(reqs.size(), 1u) << "只给要 credit 的那一项发";
  EXPECT_EQ(reqs[0].user, 22u);
  EXPECT_EQ(reqs[0].stream, 1u);
  EXPECT_EQ(reqs[0].task, 1u);
  EXPECT_EQ(reqs[0].path, 6u);
}

// Router 授予后把对应 task 置 READY，并把这一项的重发标记也置起来。
TEST(BachTsCredit, GrantWakesTheTaskAndSetsReissue) {
  TaskFsm fsm = TaskFsm::kIdle;
  bool reissue = false;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.cfg->WriteTask(0, Step(/*credit_en=*/true, 4));
    CreditBench h(clk, *b.cfg, *b.table, *b.credit);
    h.seeds = {{2, Sitting(0, 11, 0, TaskFsm::kWait)}};
    h.grants = {{10, 0, 0, 4}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    fsm = b.table->Peek(0).task_fsm;
    reissue = b.table->Peek(0).reissue;
  }
  RT::Reset();
  EXPECT_EQ(fsm, TaskFsm::kReady);
  EXPECT_TRUE(reissue) << "credit 到了的同时把重发标记置上";
}

// Head-only：只有 head_ptr 指着的那一项能退休，后面的即使先做完也要等。
TEST(BachTsCredit, OnlyTheHeadEntryRetires) {
  std::vector<uint64_t> users;
  bool head_valid = false, second_valid = false;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    OneTaskChain(*b.cfg);
    CreditBench h(clk, *b.cfg, *b.table, *b.credit);
    // head 那一项还没做完，第二项已经做完了。
    h.seeds = {{2, Sitting(0, 11, 0, TaskFsm::kInfly)},
               {3, Sitting(1, 22, 0, TaskFsm::kFinish, 0b1)}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    users = h.retire_users;
    head_valid = b.table->Peek(0).valid;
    second_valid = b.table->Peek(1).valid;
  }
  RT::Reset();
  EXPECT_TRUE(users.empty()) << "head 没做完，后面的不许先退";
  EXPECT_TRUE(head_valid);
  EXPECT_TRUE(second_valid);
}

// 顺序不能反：Router 收下 credit 返还之前不清 valid，收下之后才清并推 head。
TEST(BachTsCredit, ClearsOnlyAfterRouterAccepts) {
  bool valid_while_waiting = false, valid_after = true;
  uint64_t head_after = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    OneTaskChain(*b.cfg);
    CreditBench h(clk, *b.cfg, *b.table, *b.credit);
    h.seeds = {{2, Sitting(0, 11, 0, TaskFsm::kFinish, 0b1)}};
    h.retire_accept_from = 100000;  // Router 一直不收
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    valid_while_waiting = b.table->Peek(0).valid;
  }
  RT::Reset();
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    OneTaskChain(*b.cfg);
    CreditBench h(clk, *b.cfg, *b.table, *b.credit);
    h.seeds = {{2, Sitting(0, 11, 0, TaskFsm::kFinish, 0b1)}};
    h.retire_accept_from = 20;
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    valid_after = b.table->Peek(0).valid;
    head_after = b.table->HeadPtr();
  }
  RT::Reset();
  EXPECT_TRUE(valid_while_waiting) << "Router 没收下就一直保持，不清 valid";
  EXPECT_FALSE(valid_after) << "收下之后才清";
  EXPECT_EQ(head_after, 1u) << "head_ptr 跟着推一格";
}

// 退休一项之后 head 指向下一项，它做完了也能退。
TEST(BachTsCredit, NextEntryRetiresAfterTheHead) {
  std::vector<uint64_t> users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    OneTaskChain(*b.cfg);
    CreditBench h(clk, *b.cfg, *b.table, *b.credit);
    h.seeds = {{2, Sitting(0, 11, 0, TaskFsm::kFinish, 0b1)},
               {3, Sitting(1, 22, 0, TaskFsm::kFinish, 0b1)}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    users = h.retire_users;
  }
  RT::Reset();
  ASSERT_EQ(users.size(), 2u) << "两项都退了";
  EXPECT_EQ(users[0], 11u) << "按 head 的顺序退";
  EXPECT_EQ(users[1], 22u);
}

// 退休看的是第 0 项到 End 的完成位，不看当前停在哪一项：End 提前完成、其余也都
// 做完了就退。
TEST(BachTsCredit, RetiresOnceEveryTaskThroughEndIsDone) {
  std::vector<uint64_t> partial, full;
  for (uint64_t done : {0b011u, 0b111u}) {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.cfg->WriteTask(0, Step(false));
    b.cfg->WriteTask(1, Step(false));
    b.cfg->WriteTask(2, Step(false, 0, true));
    b.cfg->SetInitFinish();
    CreditBench h(clk, *b.cfg, *b.table, *b.credit);
    h.seeds = {{2, Sitting(0, 11, 0, TaskFsm::kFinish, done)}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    (done == 0b111u ? full : partial) = h.retire_users;
    RT::Reset();
  }
  EXPECT_TRUE(partial.empty()) << "End 还没做完";
  ASSERT_EQ(full.size(), 1u);
  EXPECT_EQ(full[0], 11u);
}

// 逐级 reduce 任务用本级 Rmem 的 credit：这个用户的 credit 在就置 READY，被占着
// 就等；不向 Router 申请。
TEST(BachTsCredit, ReduceTakesTheLocalRmemCredit) {
  TaskFsm free_fsm = TaskFsm::kIdle, busy_fsm = TaskFsm::kIdle;
  uint64_t admitted = 0;
  std::vector<CreditBench::Req> reqs;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    TaskEntry red = Step(true, 4, true);
    red.task_type = TaskType::kReduce;
    b.cfg->WriteTask(0, red);
    b.cfg->SetInitFinish();
    CreditBench h(clk, *b.cfg, *b.table, *b.credit);
    auto free_one = Sitting(0, 11, 0, TaskFsm::kWait);
    free_one->entry.task_type = TaskType::kReduce;
    auto busy_one = Sitting(1, 22, 0, TaskFsm::kWait);
    busy_one->entry.task_type = TaskType::kReduce;
    busy_one->entry.rmem_busy = true;
    h.seeds = {{2, free_one}, {3, busy_one}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    free_fsm = b.table->Peek(0).task_fsm;
    busy_fsm = b.table->Peek(1).task_fsm;
    admitted = h.rmem_admitted;
    reqs = h.reqs;
  }
  RT::Reset();
  EXPECT_EQ(free_fsm, TaskFsm::kReady);
  EXPECT_EQ(busy_fsm, TaskFsm::kWait) << "上一笔 reduce 还没做完";
  EXPECT_EQ(admitted, 1u);
  EXPECT_TRUE(reqs.empty()) << "reduce 不向 Router 申请 credit";
}
