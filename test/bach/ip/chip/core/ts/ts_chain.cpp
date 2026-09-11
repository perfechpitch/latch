// Stream_table 的写口仲裁与 Task_ctrl 生成后继。
//
// 这两块合在一起看：Task_ctrl 读上一拍的表快照算出下一步，把整项写回
// Stream_table。后继是当前任务之后、THROUGH_END_MASK 以内、done_bitmap 还没置位
// 的最低一项，一次 64 位优先编码一拍算完；初始状态由 wait_wake 与 credit_en 定；
// PID 更新任务带回的新 PID 只由紧邻的后继继承。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/ts/stream_table.h"
#include "bach/ip/chip/core/ts/task_ctrl.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

TaskEntry Step(uint64_t pc) {
  TaskEntry t;
  t.send_unit = SendUnit::kDte;
  t.recv_unit = RecvUnit::kDsa;
  t.task_pc = pc;
  return t;
}

// 表、配置、Task_ctrl 与灌写口的动作全在这一个协程里，次序由调用顺序定死。
class ChainBench : public BachModule {
 public:
  ChainBench(ClockPtr c, CfgReg& reg, StreamTable& tab, TaskCtrl& ctl)
      : BachModule(c, "bench"), cfg(reg), table(tab), ctrl(ctl) {}

  struct WriteJob {
    uint64_t at = 0;
    uint64_t port = 0;
    std::shared_ptr<StreamWrite> w;
  };
  std::vector<WriteJob> jobs;

  // 每个写口被收下的拍。
  std::vector<std::pair<uint64_t, uint64_t>> accepted_at;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    for (uint64_t p = 0; p < kWrPortNum; ++p) {
      if (table.Port(p).Accepted()) accepted_at.push_back({now, p});
    }
    for (auto const& j : jobs) {
      if (j.at == now) table.Port(j.port).Drive(j.w);
    }
    cfg.RunStep();
    ctrl.RunStep();
    table.RunStep();
  }

 private:
  CfgReg& cfg;
  StreamTable& table;
  TaskCtrl& ctrl;
};

struct Bench {
  ClockPtr clk;
  std::unique_ptr<CfgReg> cfg;
  std::unique_ptr<StreamTable> table;
  std::unique_ptr<TaskCtrl> ctrl;

  explicit Bench(ClockPtr c) : clk(c) {
    cfg = std::make_unique<CfgReg>(c, "cfg", 0, false);
    table = std::make_unique<StreamTable>(c, "table", 0, false);
    ctrl = std::make_unique<TaskCtrl>(c, "ctrl", *cfg, 0, false);
    ctrl->AttachSnapshot(table->SnapPtr());
    table->Rebind(kWrInstall, ctrl->InstallPtr());
  }
};

// 建一个已经跑到某一步、状态是 FINISH 的用户。
std::shared_ptr<StreamWrite> Sitting(uint64_t slot, uint64_t task_id,
                                     uint64_t done_bitmap) {
  auto w = std::make_shared<StreamWrite>();
  w->valid = true;
  w->stream_id = slot;
  w->whole = true;
  w->entry.valid = true;
  w->entry.user_id = 100 + slot;
  w->entry.user_id_vld = true;
  w->entry.task_id = task_id;
  w->entry.task_fsm = TaskFsm::kFinish;
  w->entry.done_bitmap = done_bitmap;
  return w;
}

// 一条 n 项的链，末项是 End。
void PlainChain(CfgReg& cfg, uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) {
    TaskEntry t = Step(0x100 + i);
    t.end = i + 1 == n;
    cfg.WriteTask(i, t);
  }
  cfg.SetInitFinish();
}

}  // namespace

// 落到不同 stream 的写互不相干，同一拍可以都做。
TEST(BachStreamTable, DifferentStreamsAreWrittenTogether) {
  std::vector<std::pair<uint64_t, uint64_t>> at;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    ChainBench h(clk, *b.cfg, *b.table, *b.ctrl);
    // create 写 stream 0，credit_wake 写 stream 1，同一拍发出。
    auto c0 = Sitting(0, 0, 0);
    auto w1 = std::make_shared<StreamWrite>();
    w1->valid = true;
    w1->stream_id = 1;
    w1->set_fsm = true;
    w1->fsm = TaskFsm::kReady;
    h.jobs = {{2, kWrCreate, c0}, {2, kWrCreditWake, w1}};
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    at = h.accepted_at;
  }
  RT::Reset();
  ASSERT_GE(at.size(), 2u);
  // 两个口在同一拍都拿到 accepted。
  EXPECT_EQ(at[0].first, at[1].first) << "不同 stream 的写不互相排队";
}

// 落到同一个 stream 的写按优先级排：高的先，低的下一拍再来。
TEST(BachStreamTable, SameStreamGoesByPriority) {
  std::vector<std::pair<uint64_t, uint64_t>> at;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    ChainBench h(clk, *b.cfg, *b.table, *b.ctrl);
    // 同一个 stream：completion 优先级高于 credit_wake。
    auto hi = std::make_shared<StreamWrite>();
    hi->valid = true;
    hi->stream_id = 3;
    hi->set_done_bit = true;
    hi->done_bit = 2;
    auto lo = std::make_shared<StreamWrite>();
    lo->valid = true;
    lo->stream_id = 3;
    lo->set_fsm = true;
    lo->fsm = TaskFsm::kReady;
    h.jobs = {{2, kWrCompletion, hi}, {2, kWrCreditWake, lo}};
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    at = h.accepted_at;
  }
  RT::Reset();
  ASSERT_GE(at.size(), 1u);
  EXPECT_EQ(at[0].second, uint64_t(kWrCompletion)) << "优先级高的先写";
}

// completion 口无条件置 done_bitmap，只有这一笔是当前 task 时才改状态机。
TEST(BachStreamTable, CompletionSetsDoneBitButGuardsTheFsm) {
  uint64_t bitmap = 0;
  TaskFsm fsm = TaskFsm::kIdle;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    ChainBench h(clk, *b.cfg, *b.table, *b.ctrl);
    // 用户当前停在 task 0，状态 INFLY。
    auto seed = Sitting(5, 0, 0);
    seed->entry.task_fsm = TaskFsm::kInfly;
    // 报的是 task 3 完成：不是当前那一步，只该亮一位。
    auto done = std::make_shared<StreamWrite>();
    done->valid = true;
    done->stream_id = 5;
    done->set_done_bit = true;
    done->done_bit = 3;
    done->set_fsm = true;
    done->fsm = TaskFsm::kFinish;
    done->fsm_if_current = true;
    done->from_task_id = 3;
    h.jobs = {{2, kWrCreate, seed}, {6, kWrCompletion, done}};
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    bitmap = b.table->Peek(5).done_bitmap;
    fsm = b.table->Peek(5).task_fsm;
  }
  RT::Reset();
  EXPECT_EQ(bitmap, 1ull << 3) << "那一位无条件亮";
  EXPECT_EQ(fsm, TaskFsm::kInfly) << "不是当前这一步，状态机不动";
}

// User_Match 补的跳过位并进 done_bitmap；包含当前任务时当前任务同时算做完。
TEST(BachStreamTable, SkipMaskFinishesOnlyTheCurrentTask) {
  uint64_t bits_a = 0, bits_b = 0;
  TaskFsm fsm_a = TaskFsm::kIdle, fsm_b = TaskFsm::kIdle;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    ChainBench h(clk, *b.cfg, *b.table, *b.ctrl);
    auto seed_a = Sitting(2, 0, 0);
    seed_a->entry.task_fsm = TaskFsm::kWait;
    auto seed_b = Sitting(3, 0, 0);
    seed_b->entry.task_fsm = TaskFsm::kWait;
    auto mask_a = std::make_shared<StreamWrite>();
    mask_a->valid = true;
    mask_a->stream_id = 2;
    mask_a->done_mask = 0b11;   // 含当前的 task 0
    auto mask_b = std::make_shared<StreamWrite>();
    mask_b->valid = true;
    mask_b->stream_id = 3;
    mask_b->done_mask = 0b110;  // 不含当前的 task 0
    h.jobs = {{2, kWrCreate, seed_a},
              {5, kWrCreate, seed_b},
              {8, kWrCreate, mask_a},
              {11, kWrCreate, mask_b}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    bits_a = b.table->Peek(2).done_bitmap;
    fsm_a = b.table->Peek(2).task_fsm;
    bits_b = b.table->Peek(3).done_bitmap;
    fsm_b = b.table->Peek(3).task_fsm;
  }
  RT::Reset();
  EXPECT_EQ(bits_a, 0b11u);
  EXPECT_EQ(fsm_a, TaskFsm::kFinish) << "跳过的里有当前任务";
  EXPECT_EQ(bits_b, 0b110u);
  EXPECT_EQ(fsm_b, TaskFsm::kWait) << "当前任务不在跳过的里面，状态机不动";
}

// 自启动的 core 复位后直接建满表项，此时还没有用户信息。
TEST(BachStreamTable, SelfStartFillsEveryEntry) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  Bench b(clk);
  b.cfg->SetSelfStart(true);
  b.table->SelfStart(Step(0x400), 4);
  for (uint64_t i = 0; i < 4; ++i) {
    EXPECT_TRUE(b.table->Peek(i).valid) << "i=" << i;
    EXPECT_FALSE(b.table->Peek(i).user_id_vld) << "用户信息等 RV core 回填";
    EXPECT_EQ(b.table->Peek(i).task_fsm, TaskFsm::kReady);
    EXPECT_EQ(b.table->Peek(i).task_pc, 0x400u);
  }
  EXPECT_FALSE(b.table->Peek(4).valid) << "只建 stream_num 个";
  RT::Reset();
}

// done_bitmap 里已经置位的一次跳完，不逐项各花一拍。
TEST(BachTaskCtrl, SkipsDoneTasksInOneStep) {
  uint64_t landed = 0, at = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    PlainChain(*b.cfg, 6);
    ChainBench h(clk, *b.cfg, *b.table, *b.ctrl);
    // 用户停在 task 0，1～4 都已经做完或跳过。
    h.jobs = {{2, kWrCreate, Sitting(0, 0, 0b11111)}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    landed = b.table->Peek(0).task_id;
    for (auto const& kv : h.accepted_at) {
      if (kv.second == kWrInstall && at == 0) at = kv.first;
    }
  }
  RT::Reset();
  EXPECT_EQ(landed, 5u) << "中间四项一次跳完";
  EXPECT_LE(at, 8u) << "连续跳过不该逐项各花一拍";
}

// 搜索不越过 End：第 0 项到 End 都已置位时不生成后继，留给退休那一侧。
TEST(BachTaskCtrl, NoSuccessorWhenEverythingThroughEndIsDone) {
  uint64_t task_id = 0, installs = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    PlainChain(*b.cfg, 3);
    ChainBench h(clk, *b.cfg, *b.table, *b.ctrl);
    // 停在 task 1，End（task 2）已经提前完成。
    h.jobs = {{2, kWrCreate, Sitting(0, 1, 0b111)}};
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    task_id = b.table->Peek(0).task_id;
    for (auto const& kv : h.accepted_at) {
      if (kv.second == kWrInstall) ++installs;
    }
  }
  RT::Reset();
  EXPECT_EQ(task_id, 1u);
  EXPECT_EQ(installs, 0u);
}

// 生成后继时整项一起写：task_id、状态与全部下发属性同时换成新项的。
TEST(BachTaskCtrl, InstallWritesTheWholeEntryAtOnce) {
  StreamEntry got;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.cfg->WriteTask(0, Step(0x100));
    TaskEntry t1 = Step(0x777);
    t1.send_unit = SendUnit::kVu;
    t1.recv_unit = RecvUnit::kRvOnly;
    t1.task_type = TaskType::kPidUpdate;
    t1.path_id = 9;
    b.cfg->WriteTask(1, t1);
    TaskEntry last = Step(0x300);
    last.end = true;
    b.cfg->WriteTask(2, last);
    b.cfg->SetInitFinish();

    ChainBench h(clk, *b.cfg, *b.table, *b.ctrl);
    h.jobs = {{2, kWrCreate, Sitting(0, 0, 0b1)}};
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    got = b.table->Peek(0);
  }
  RT::Reset();
  EXPECT_EQ(got.task_id, 1u);
  EXPECT_EQ(got.task_pc, 0x777u);
  EXPECT_EQ(got.task_unit, SendUnit::kVu);
  EXPECT_EQ(got.task_recv, RecvUnit::kRvOnly);
  EXPECT_EQ(got.task_type, TaskType::kPidUpdate);
  EXPECT_EQ(got.task_path_id, 9u);
  EXPECT_EQ(got.task_fsm, TaskFsm::kReady);
  EXPECT_EQ(got.user_id, 100u) << "用户级标记不受影响";
}

// 后继的初始状态：wait_wake 或 credit_en 的置 WAIT，其余置 READY。
TEST(BachTaskCtrl, InitialStateFollowsWaitWakeAndCredit) {
  StreamEntry s0, s1, s2;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.cfg->WriteTask(0, Step(0x100));
    TaskEntry credit = Step(0x200);
    credit.credit_en = true;
    b.cfg->WriteTask(1, credit);
    TaskEntry wake = Step(0x300);
    wake.wait_wake = true;
    b.cfg->WriteTask(2, wake);
    TaskEntry last = Step(0x400);
    last.end = true;
    b.cfg->WriteTask(3, last);
    b.cfg->SetInitFinish();

    ChainBench h(clk, *b.cfg, *b.table, *b.ctrl);
    h.jobs = {{2, kWrCreate, Sitting(0, 0, 0b1)},
              {3, kWrCreate, Sitting(1, 1, 0b11)},
              {4, kWrCreate, Sitting(2, 2, 0b111)}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    s0 = b.table->Peek(0);
    s1 = b.table->Peek(1);
    s2 = b.table->Peek(2);
  }
  RT::Reset();
  EXPECT_EQ(s0.task_id, 1u);
  EXPECT_EQ(s0.task_fsm, TaskFsm::kWait) << "credit_en";
  EXPECT_EQ(s1.task_id, 2u);
  EXPECT_EQ(s1.task_fsm, TaskFsm::kWait) << "wait_wake";
  EXPECT_EQ(s2.task_id, 3u);
  EXPECT_EQ(s2.task_fsm, TaskFsm::kReady);
}

// PID 更新任务带回的新 PID 只由紧邻的后继继承；跳过了紧邻那一项就用所选任务
// 自己的 PID。
TEST(BachTaskCtrl, NewPidGoesOnlyToTheAdjacentTask) {
  StreamEntry near, far;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    for (uint64_t i = 0; i < 4; ++i) {
      TaskEntry t = Step(0x100 + i);
      t.path_id = 5 + i;
      if (i == 0) t.task_type = TaskType::kPidUpdate;
      t.end = i == 3;
      b.cfg->WriteTask(i, t);
    }
    b.cfg->SetInitFinish();

    ChainBench h(clk, *b.cfg, *b.table, *b.ctrl);
    auto a = Sitting(0, 0, 0b1);
    a->entry.task_path_id = 33;
    a->entry.pid_pending = true;
    auto c = Sitting(1, 0, 0b11);  // 紧邻的 task 1 已经跳过
    c->entry.task_path_id = 44;
    c->entry.pid_pending = true;
    h.jobs = {{2, kWrCreate, a}, {3, kWrCreate, c}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    near = b.table->Peek(0);
    far = b.table->Peek(1);
  }
  RT::Reset();
  EXPECT_EQ(near.task_id, 1u);
  EXPECT_EQ(near.task_path_id, 33u) << "紧邻的后继继承新 PID";
  EXPECT_FALSE(near.pid_pending);
  EXPECT_EQ(far.task_id, 2u);
  EXPECT_EQ(far.task_path_id, 7u) << "不紧邻就用自己的 PID";
  EXPECT_FALSE(far.pid_pending);
}
