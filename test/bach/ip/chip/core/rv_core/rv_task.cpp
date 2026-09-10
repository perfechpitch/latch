// task_queue 的握手与 DSA 那两条通路。
//
// 队列深 2、提前接收，满了就拉低 cmd_ready 让 TS 停下；下发的字段原样交给执行
// 器；DSA 的写会被反压、读不会，读的返回按下发顺序写回 gpr。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/rv_core/dsa_iss.h"
#include "bach/ip/chip/core/rv_core/dsa_rq.h"
#include "bach/ip/chip/core/rv_core/task_queue.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

// 扮演 TS 下发 task、扮演执行器收 task 与报完成。
class QueueHarness : public BachModule {
 public:
  struct Job {
    uint64_t at = 0;
    uint64_t pc = 0, stream = 0, task = 0, user = 0, path = 0;
    bool dsa_en = false;
  };

  QueueHarness(ClockPtr c, RvTaskQueue& target)
      : BachModule(c, "harness"), tq(target) {}

  std::vector<Job> jobs;
  // 这一拍之前执行器不收 task。
  uint64_t exec_ready_from = 0;
  // 收到 task 之后隔几拍报完成。
  uint64_t run_cycles = 4;

  struct Got {
    uint64_t at = 0, pc = 0, stream = 0, user = 0, path = 0;
  };
  std::vector<Got> starts;
  std::vector<uint64_t> cmd_ready_low_at;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做：先看上一拍摆出来的。
    TaskStartPort& st = tq.Start();
    if (st.Valid() && st.seq.Get() != last_start_seq && now >= exec_ready_from) {
      last_start_seq = st.seq.Get();
      starts.push_back({now, st.task_pc.Get(), st.stream_id.Get(),
                        st.user_id.Get(), st.path_id.Get()});
      finish_at = now + run_cycles;
    }
    st.DriveReady(now >= exec_ready_from);

    // 到点报完成。
    if (finish_at != 0 && now >= finish_at) {
      tq.FinishPtr()->Drive(/*to_ts=*/true, ++finish_seq);
      finish_at = 0;
    } else {
      tq.FinishPtr()->Idle();
    }

    if (!tq.Cmd().Ready()) cmd_ready_low_at.push_back(now);
    Feed(now);
    tq.RunStep();
  }

 private:
  void Feed(uint64_t now) {
    TaskCmdPort& c = tq.Cmd();
    if (holding && c.Ready()) holding = false;
    if (!holding) {
      for (uint64_t i = cursor; i < jobs.size(); ++i) {
        if (jobs[i].at > now) break;
        held = jobs[i];
        cursor = i + 1;
        holding = true;
        ++cmd_seq;
        break;
      }
    }
    if (holding) {
      c.Drive(held.pc, held.stream, held.task, held.user, held.path,
              held.dsa_en, cmd_seq);
    } else {
      c.Idle();
    }
  }

  RvTaskQueue& tq;
  Job held;
  bool holding = false;
  uint64_t cursor = 0, cmd_seq = 0, last_start_seq = 0, finish_at = 0,
      finish_seq = 0;
};

}  // namespace

// 下发的字段原样交给执行器。
TEST(BachRvTaskQueue, TaskFieldsGoThroughUntouched) {
  std::vector<QueueHarness::Got> starts;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvTaskQueue tq(clk, "tq", 0, false);
    QueueHarness h(clk, tq);
    h.jobs = {{2, /*pc=*/0x800, /*stream=*/5, /*task=*/3, /*user=*/41,
               /*path=*/7, false}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    starts = h.starts;
  }
  RT::Reset();
  ASSERT_EQ(starts.size(), 1u);
  EXPECT_EQ(starts[0].pc, 0x800u);
  EXPECT_EQ(starts[0].stream, 5u);
  EXPECT_EQ(starts[0].user, 41u);
  EXPECT_EQ(starts[0].path, 7u);
}

// 队列深 2：装满之后拉低 cmd_ready，TS 不能跳到下一个。
TEST(BachRvTaskQueue, ReadyGoesLowWhenTheQueueIsFull) {
  uint64_t low_cycles = 0, started = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvTaskQueue tq(clk, "tq", 0, false);
    QueueHarness h(clk, tq);
    // 执行器一直不收，队列会被填满。
    h.exec_ready_from = 60;
    for (uint64_t i = 0; i < 4; ++i) {
      h.jobs.push_back({2 + i * 3, 0x800, i, i, 41 + i, 7, false});
    }
    clk->Continue(50 * kPeriod);
    RT::JoinAll();
    low_cycles = h.cmd_ready_low_at.size();
    started = h.starts.size();
  }
  RT::Reset();
  EXPECT_GT(low_cycles, 0u) << "满了要把 ready 拉下来";
  EXPECT_EQ(started, 0u) << "执行器没收，一个都没起";
}

// 前一个做完就立刻起队头缓存的那个，用户之间不留空拍。
TEST(BachRvTaskQueue, NextTaskStartsRightAfterTheFinish) {
  std::vector<uint64_t> at;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvTaskQueue tq(clk, "tq", 0, false);
    QueueHarness h(clk, tq);
    h.run_cycles = 5;
    h.jobs = {{2, 0x800, 0, 0, 41, 7, false},
              {3, 0x900, 1, 0, 42, 7, false}};
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    for (auto const& s : h.starts) at.push_back(s.at);
  }
  RT::Reset();
  ASSERT_EQ(at.size(), 2u) << "两个 task 都要起";
  EXPECT_LE(at[1] - at[0], 8u) << "前一个做完就接着起，不等 TS 再下发";
}

namespace {

// 扮演执行器发 DSA 请求、扮演 DSA 收配置与回数据。
class DsaHarness : public BachModule {
 public:
  struct Job {
    uint64_t at = 0;
    bool we = false;
    uint64_t addr = 0, data = 0, rd = 0;
  };

  DsaHarness(ClockPtr c, DsaIss& issue, DsaRq& queue)
      : BachModule(c, "harness"), iss(issue), rq(queue) {}

  std::vector<Job> jobs;
  // 这一拍之前 DSA 的配置通路满着。
  uint64_t cfg_ready_from = 0;
  // 到点回一笔读数据。
  std::vector<uint64_t> rdata_at;

  struct Cfg {
    uint64_t at = 0, addr = 0, seq = 0;
    bool we = false;
  };
  std::vector<Cfg> cfgs;
  std::vector<uint64_t> wb_rds;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做：先看上一拍摆出来的配置与写回。
    DsaCfgPort& c = iss.Cfg();
    if (c.req_valid.Get() != 0) {
      cfgs.push_back({now, c.req_addr.Get(), c.req_seq.Get(),
                      c.req_we.Get() != 0});
    }
    c.DriveReady(now >= cfg_ready_from);

    if (rq.WbPtr()->Valid() && rq.WbPtr()->Seq() != last_wb_seq) {
      last_wb_seq = rq.WbPtr()->Seq();
      wb_rds.push_back(rq.WbPtr()->RdIdx());
    }

    bool drove = false;
    for (uint64_t t : rdata_at) {
      if (t != now) continue;
      rq.Rdata().Drive(0x1234, ++rdata_seq);
      drove = true;
      break;
    }
    if (!drove) rq.Rdata().Idle();

    Feed(now);
    iss.RunStep();
    rq.RunStep();
  }

 private:
  void Feed(uint64_t now) {
    DsaReqPort& p = *iss.ReqPtr();
    if (holding && p.Ready()) holding = false;
    if (!holding) {
      for (uint64_t i = cursor; i < jobs.size(); ++i) {
        if (jobs[i].at > now) break;
        held = jobs[i];
        cursor = i + 1;
        holding = true;
        ++seq;
        break;
      }
    }
    if (holding) {
      p.Drive(held.we, held.addr, held.data, held.rd, seq);
    } else {
      p.Idle();
    }
  }

  DsaIss& iss;
  DsaRq& rq;
  Job held;
  bool holding = false;
  uint64_t cursor = 0, seq = 0, last_wb_seq = 0, rdata_seq = 0;
};

}  // namespace

// 写 DSA 寄存器会被反压，等的那几拍序号保持不变：换号的话对方会把同一笔认成
// 好几笔，写 trigger 那种寄存器就执行好几遍。
TEST(BachDsaIss, WriteHoldsOneSeqWhileStalled) {
  std::vector<uint64_t> seqs;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DsaIss iss(clk, "iss", 0, false);
    DsaRq rq(clk, "rq", 0, false);
    rq.AttachIssue(iss.ReadIssuePtr());
    DsaHarness h(clk, iss, rq);
    h.jobs = {{2, /*we=*/true, 0x28, 1, 0}};
    h.cfg_ready_from = 20;  // DSA 那边二十拍之后才收
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    for (auto const& c : h.cfgs) seqs.push_back(c.seq);
  }
  RT::Reset();
  ASSERT_GT(seqs.size(), 1u) << "被压住的这几拍一直在发";
  for (uint64_t i = 1; i < seqs.size(); ++i) {
    EXPECT_EQ(seqs[i], seqs[0]) << "等的时候不换号";
  }
}

// 读不会被阻塞：DSA 那边没给 ready 也照样发出去。
TEST(BachDsaIss, ReadIsNotStalled) {
  uint64_t reads = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DsaIss iss(clk, "iss", 0, false);
    DsaRq rq(clk, "rq", 0, false);
    rq.AttachIssue(iss.ReadIssuePtr());
    DsaHarness h(clk, iss, rq);
    h.jobs = {{2, /*we=*/false, 0x10, 0, 5}};
    h.cfg_ready_from = 100000;  // 一直不给 ready
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    for (auto const& c : h.cfgs) {
      if (!c.we) ++reads;
    }
  }
  RT::Reset();
  EXPECT_GT(reads, 0u) << "读发出去就走，不等 ready";
}

// 读的返回按下发顺序写回 gpr。
TEST(BachDsaRq, ReturnsGoBackInIssueOrder) {
  std::vector<uint64_t> rds;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DsaIss iss(clk, "iss", 0, false);
    DsaRq rq(clk, "rq", 0, false);
    rq.AttachIssue(iss.ReadIssuePtr());
    DsaHarness h(clk, iss, rq);
    // 三条读，目的寄存器分别是 5、6、7。
    h.jobs = {{2, false, 0x10, 0, 5},
              {6, false, 0x14, 0, 6},
              {10, false, 0x18, 0, 7}};
    h.rdata_at = {20, 24, 28};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    rds = h.wb_rds;
  }
  RT::Reset();
  ASSERT_EQ(rds.size(), 3u);
  EXPECT_EQ(rds[0], 5u);
  EXPECT_EQ(rds[1], 6u) << "按下发顺序写回，不按返回顺序";
  EXPECT_EQ(rds[2], 7u);
}
