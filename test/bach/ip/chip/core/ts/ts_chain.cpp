// Stream_table 的写口仲裁与 Task_ctrl 的跳过判断。
//
// 这两块合在一起看：Task_ctrl 读上一拍的表快照算出下一步，把整项写回 Stream_table。
// 跳过用一次 64 位优先编码一拍算完，End task 不许跳，跳过的依据是四张掩码加
// done_bitmap 与两个用户级标记。

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
  t.dsa_en = true;
  t.exe_mask = true;
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
                                     bool compute, bool reissue,
                                     uint64_t done_bitmap) {
  auto w = std::make_shared<StreamWrite>();
  w->valid = true;
  w->stream_id = slot;
  w->whole = true;
  w->entry.valid = true;
  w->entry.user_id = 100 + slot;
  w->entry.user_id_vld = true;
  w->entry.compute = compute;
  w->entry.reissue = reissue;
  w->entry.task_id = task_id;
  w->entry.task_fsm = TaskFsm::kFinish;
  w->entry.done_bitmap = done_bitmap;
  return w;
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
    auto c0 = Sitting(0, 0, true, false, 0);
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
    auto seed = Sitting(5, 0, true, false, 0);
    seed->entry.task_fsm = TaskFsm::kInfly;
    // 报的是 task 3 完成 —— 不是当前那一步，只该亮一位。
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

// 自启动的 core 复位后直接建满表项，此时还没有用户信息。
TEST(BachStreamTable, SelfStartFillsEveryEntry) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  Bench b(clk);
  b.cfg->SetCoreType(CoreType::kBroadcast);
  TaskEntry t0 = Step(0x400);
  t0.self_start = true;
  b.cfg->WriteTask(0, t0);

  b.table->SelfStart(b.cfg->Task(0), 4);
  for (uint64_t i = 0; i < 4; ++i) {
    EXPECT_TRUE(b.table->Peek(i).valid) << "i=" << i;
    EXPECT_FALSE(b.table->Peek(i).user_id_vld) << "用户信息等 RV core 回填";
    EXPECT_EQ(b.table->Peek(i).task_fsm, TaskFsm::kReady);
    EXPECT_EQ(b.table->Peek(i).task_pc, 0x400u);
  }
  EXPECT_FALSE(b.table->Peek(4).valid) << "只建 stream_num 个";
  RT::Reset();
}

// End task 即使已经完成也不能被跳过。
TEST(BachTaskCtrl, EndTaskIsNeverSkipped) {
  uint64_t next_task = 0;
  bool is_end = false;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    // 链：0 普通、1 datain、2 End（也是 datain，且已完成）。
    TaskEntry t0 = Step(0x100);
    TaskEntry t1 = Step(0x200);
    t1.wait_wake = true;
    TaskEntry t2 = Step(0x300);
    t2.wait_wake = true;
    t2.end = true;
    b.cfg->WriteTask(0, t0);
    b.cfg->WriteTask(1, t1);
    b.cfg->WriteTask(2, t2);
    b.cfg->WriteDatainTask(0x500, false);
    b.cfg->SetInitFinish();

    ChainBench h(clk, *b.cfg, *b.table, *b.ctrl);
    // 用户停在 task 0（FINISH），第 1、2 位的 datain 都已完成。
    h.jobs = {{2, kWrCreate, Sitting(0, 0, true, false, 0b110)}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    next_task = b.table->Peek(0).task_id;
    is_end = b.table->Peek(0).end;
  }
  RT::Reset();
  EXPECT_EQ(next_task, 2u) << "第 1 项跳过了，End 那一项不许跳";
  EXPECT_TRUE(is_end);
}

// compute = 0 的用户跳过所有 TASK_EXE_MASK = 0 的 task；连续跳过一拍算完。
TEST(BachTaskCtrl, NonComputeUserSkipsTheWholeGroup) {
  uint64_t landed = 0, at = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    // 链：0 普通、1～4 按用户区分、5 End。
    b.cfg->WriteTask(0, Step(0x100));
    for (uint64_t i = 1; i <= 4; ++i) {
      TaskEntry t = Step(0x200 + i);
      t.exe_mask = false;
      b.cfg->WriteTask(i, t);
    }
    TaskEntry last = Step(0x900);
    last.end = true;
    b.cfg->WriteTask(5, last);
    b.cfg->WriteDatainTask(0x500, false);
    b.cfg->SetInitFinish();

    ChainBench h(clk, *b.cfg, *b.table, *b.ctrl);
    h.jobs = {{2, kWrCreate, Sitting(0, 0, /*compute=*/false, false, 0)}};
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

// 这个用户不需要重发时，链上的 reissue 任务全跳过。
TEST(BachTaskCtrl, ReissueTasksAreSkippedWhenNotNeeded) {
  uint64_t with_reissue = 0, without = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.cfg->WriteTask(0, Step(0x100));
    TaskEntry re = Step(0x200);
    re.broadcast_reissue = true;
    b.cfg->WriteTask(1, re);
    TaskEntry last = Step(0x300);
    last.end = true;
    b.cfg->WriteTask(2, last);
    b.cfg->WriteDatainTask(0x500, false);
    b.cfg->SetInitFinish();

    ChainBench h(clk, *b.cfg, *b.table, *b.ctrl);
    h.jobs = {{2, kWrCreate, Sitting(0, 0, true, /*reissue=*/true, 0)},
              {3, kWrCreate, Sitting(1, 0, true, /*reissue=*/false, 0)}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    with_reissue = b.table->Peek(0).task_id;
    without = b.table->Peek(1).task_id;
  }
  RT::Reset();
  EXPECT_EQ(with_reissue, 1u) << "要重发的走那一项";
  EXPECT_EQ(without, 2u) << "不重发的跳过它";
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
    t1.dsa_en = false;
    t1.path_id = 9;
    b.cfg->WriteTask(1, t1);
    TaskEntry last = Step(0x300);
    last.end = true;
    b.cfg->WriteTask(2, last);
    b.cfg->WritePathMap(9, 1);
    b.cfg->WriteDatainTask(0x500, false);
    b.cfg->SetInitFinish();

    ChainBench h(clk, *b.cfg, *b.table, *b.ctrl);
    h.jobs = {{2, kWrCreate, Sitting(0, 0, true, false, 0)}};
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    got = b.table->Peek(0);
  }
  RT::Reset();
  EXPECT_EQ(got.task_id, 1u);
  EXPECT_EQ(got.task_pc, 0x777u);
  EXPECT_EQ(got.task_unit, SendUnit::kVu);
  EXPECT_EQ(got.task_recv, RecvUnit::kRvOnly);
  EXPECT_FALSE(got.task_dsa_en);
  EXPECT_EQ(got.task_path_id, 9u);
  EXPECT_EQ(got.user_id, 100u) << "用户级标记不受影响";
}
