// Completion RS 与 Done Pending：把劈开的两半合回来。
//
// 只在同一笔的 RD 与 WR 两侧都 drained 时才 Join；同一拍多个 Join 全部进 Done
// Pending，由它串行化；向 TS 的报告是 exactly-once；task_last 与 no_ack 决定
// 报不报。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/dte/completion_rs.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

std::shared_ptr<Descriptor> Task(uint64_t commit_seq, uint64_t stream,
                                 uint64_t task, bool task_last = true,
                                 bool no_ack = false) {
  auto d = std::make_shared<Descriptor>();
  d->valid = true;
  d->commit_seq = commit_seq;
  d->stream_id = stream;
  d->task_id = task;
  d->task_last = task_last;
  d->no_ack = no_ack;
  return d;
}

// 扮演 Commit 占位、扮演两侧的完成上报、收 TS 那一路。
class CompHarness : public BachModule {
 public:
  struct AdmitJob {
    uint64_t at = 0;
    std::shared_ptr<Descriptor> d;
  };
  struct HalfJob {
    uint64_t at = 0;
    uint64_t commit_seq = 0;
    bool is_rd = true;
    bool drained = true;
  };

  CompHarness(ClockPtr c, CompletionRs& target)
      : BachModule(c, "harness"), rs(target) {}

  std::vector<AdmitJob> admits;
  std::vector<HalfJob> halves;

  struct Report {
    uint64_t at = 0, stream = 0, task = 0;
  };
  std::vector<Report> reports;

  // shareMem 那一路：记下写出去的地址与数据，以及是第几拍。
  struct SmemWrite {
    uint64_t at = 0, addr = 0, data = 0;
  };
  std::vector<SmemWrite> smem;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做：先看上一拍报出来的。
    if (rs.ToTs().Valid()) {
      reports.push_back({now, rs.ToTs().stream_id.Get(),
                         rs.ToTs().task_id.Get()});
    }

    rs.RdDone().Idle();
    rs.WrDone().Idle();
    for (auto const& h : halves) {
      if (h.at != now) continue;
      if (h.is_rd) {
        rs.RdDone().Drive(h.commit_seq, kRd, h.drained);
      } else {
        rs.WrDone().Drive(h.commit_seq, kWr, h.drained);
      }
    }

    bool drove = false;
    for (auto const& a : admits) {
      if (a.at != now) continue;
      rs.AdmitPtr()->Drive(a.d, ++admit_seq);
      drove = true;
      break;
    }
    if (!drove) rs.AdmitPtr()->Idle();

    // shareMem 那一路：一直收得下，写出来的当场记下。
    MemReqView w = ReadMemReq(rs.SmemWr());
    if (w.valid && w.seq != last_smem_seq) {
      last_smem_seq = w.seq;
      uint64_t v = 0;
      if (w.wdata) {
        for (uint64_t k = 0; k < 4 && k < w.wdata->size(); ++k) {
          v |= uint64_t((*w.wdata)[k]) << (8 * k);
        }
      }
      smem.push_back({now, w.addr, v});
    }
    rs.SmemWr().DriveSlave(true, false, nullptr);

    rs.RunStep();
  }

 private:
  CompletionRs& rs;
  uint64_t admit_seq = 0, last_smem_seq = 0;
};

}  // namespace

// 两侧都到齐、都 drained 才算完：少一样就不报。
TEST(BachCompletionRs, JoinNeedsBothSidesDrained) {
  uint64_t only_rd = 0, rd_not_drained = 0, both = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CompletionRs rs(clk, "rs", 0, false);
    CompHarness h(clk, rs);
    h.admits = {{2, Task(1, 5, 3)}};
    h.halves = {{6, 1, /*is_rd=*/true, /*drained=*/true}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    only_rd = h.reports.size();
  }
  RT::Reset();
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CompletionRs rs(clk, "rs", 0, false);
    CompHarness h(clk, rs);
    h.admits = {{2, Task(1, 5, 3)}};
    // 两侧都报到了，但 WR 那一侧还没 drain 完。
    h.halves = {{6, 1, true, true}, {8, 1, false, /*drained=*/false}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    rd_not_drained = h.reports.size();
  }
  RT::Reset();
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CompletionRs rs(clk, "rs", 0, false);
    CompHarness h(clk, rs);
    h.admits = {{2, Task(1, 5, 3)}};
    h.halves = {{6, 1, true, true}, {8, 1, false, true}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    both = h.reports.size();
  }
  RT::Reset();
  EXPECT_EQ(only_rd, 0u) << "只有一侧到不算完";
  EXPECT_EQ(rd_not_drained, 0u) << "没 drain 完也不算完";
  EXPECT_EQ(both, 1u);
}

// 带 shareMem 写的那一笔：数据搬完之后先把表项写出去，写出去了才通知 TS。
// 这一路只有 B core 与 R core 用，写的是 user_id 与 token entry 的 valid 标志。
TEST(BachCompletionRs, ShareMemEntryIsWrittenBeforeTheReport) {
  std::vector<CompHarness::SmemWrite> smem;
  std::vector<CompHarness::Report> reports;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CompletionRs rs(clk, "rs", 0, false);
    CompHarness h(clk, rs);
    auto d = Task(1, 5, 3);
    d->smem_wr = true;
    d->smem_addr = 0x40;
    d->smem_data = 2;
    h.admits = {{2, d}};
    h.halves = {{6, 1, true, true}, {6, 1, false, true}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    smem = h.smem;
    reports = h.reports;
  }
  RT::Reset();
  ASSERT_EQ(smem.size(), 1u);
  EXPECT_EQ(smem[0].addr, 0x40u);
  EXPECT_EQ(smem[0].data, 2u);
  ASSERT_EQ(reports.size(), 1u);
  EXPECT_LT(smem[0].at, reports[0].at) << "写出去了才通知 TS";
}

// 不带 task_last 的那一笔照样写 shareMem，只是不通知 TS。
TEST(BachCompletionRs, ShareMemWriteDoesNotNeedTaskLast) {
  std::vector<CompHarness::SmemWrite> smem;
  uint64_t reports = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CompletionRs rs(clk, "rs", 0, false);
    CompHarness h(clk, rs);
    auto d = Task(1, 5, 3, /*task_last=*/false);
    d->smem_wr = true;
    d->smem_addr = 0x80;
    d->smem_data = 1;
    h.admits = {{2, d}};
    h.halves = {{6, 1, true, true}, {6, 1, false, true}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    smem = h.smem;
    reports = h.reports.size();
  }
  RT::Reset();
  ASSERT_EQ(smem.size(), 1u);
  EXPECT_EQ(smem[0].data, 1u);
  EXPECT_EQ(reports, 0u);
}

// 同一拍几笔一起 Join 时全部留下，由 Done Pending 一拍报一笔。
TEST(BachCompletionRs, SeveralJoinsInOneCycleAreAllKept) {
  std::vector<CompHarness::Report> reports;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CompletionRs rs(clk, "rs", 0, false);
    CompHarness h(clk, rs);
    // 三笔任务先各自把 RD 那一侧报完，再在同一拍把 WR 三侧一起报上来。
    for (uint64_t i = 0; i < 3; ++i) {
      h.admits.push_back({2 + i, Task(1 + i, i, 3)});
      h.halves.push_back({10 + i, 1 + i, /*is_rd=*/true, true});
    }
    // WR 那一路一拍只有一个口，所以分三拍报；关键是它们会在同一拍之内都变成
    // 可 Join 的状态。
    for (uint64_t i = 0; i < 3; ++i) {
      h.halves.push_back({20 + i, 1 + i, /*is_rd=*/false, true});
    }
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    reports = h.reports;
  }
  RT::Reset();
  ASSERT_EQ(reports.size(), 3u) << "三笔都要报上去，一笔都不能丢";
  EXPECT_EQ(reports[0].stream, 0u);
  EXPECT_EQ(reports[1].stream, 1u);
  EXPECT_EQ(reports[2].stream, 2u);
  // 一拍报一笔。
  EXPECT_NE(reports[0].at, reports[1].at);
  EXPECT_NE(reports[1].at, reports[2].at);
}

// 向 TS 的报告是 exactly-once：两侧的完成再报一遍也不会多出一笔。
TEST(BachCompletionRs, ReportsExactlyOnce) {
  uint64_t num = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CompletionRs rs(clk, "rs", 0, false);
    CompHarness h(clk, rs);
    h.admits = {{2, Task(1, 5, 3)}};
    // 同一笔的两侧各报两次。
    h.halves = {{6, 1, true, true},
                {7, 1, true, true},
                {8, 1, false, true},
                {9, 1, false, true}};
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    num = h.reports.size();
  }
  RT::Reset();
  EXPECT_EQ(num, 1u) << "重复上报不该变成两笔完成";
}

// task_last 与 no_ack 决定报不报：拆成几笔时只有最后一笔通知 TS。
TEST(BachCompletionRs, TaskLastAndNoAckDecideTheReport) {
  uint64_t not_last = 0, no_ack = 0, normal = 0;
  auto run = [](bool task_last, bool ack_off) {
    uint64_t n = 0;
    {
      EnsureSlots();
      ClockPtr clk = MakeClock(0, kPeriod);
      CompletionRs rs(clk, "rs", 0, false);
      CompHarness h(clk, rs);
      h.admits = {{2, Task(1, 5, 3, task_last, ack_off)}};
      h.halves = {{6, 1, true, true}, {8, 1, false, true}};
      clk->Continue(30 * kPeriod);
      RT::JoinAll();
      n = h.reports.size();
    }
    RT::Reset();
    return n;
  };
  not_last = run(/*task_last=*/false, false);
  no_ack = run(true, /*ack_off=*/true);
  normal = run(true, false);
  EXPECT_EQ(not_last, 0u) << "不是最后一笔就不通知 TS";
  EXPECT_EQ(no_ack, 0u) << "no_ack 的任务不回 Ack";
  EXPECT_EQ(normal, 1u);
}

// 同一个业务 task_id 的两笔任务（一笔搬入一笔搬出）靠内部序号分开，不会串。
TEST(BachCompletionRs, SameTaskIdOnTwoTasksDoesNotMix) {
  std::vector<CompHarness::Report> reports;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CompletionRs rs(clk, "rs", 0, false);
    CompHarness h(clk, rs);
    // 两笔任务带同一个 task_id，内部序号不同。
    h.admits = {{2, Task(1, 5, 3)}, {3, Task(2, 6, 3)}};
    // 只把第一笔的两侧报完。
    h.halves = {{6, 1, true, true}, {8, 1, false, true}};
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    reports = h.reports;
  }
  RT::Reset();
  ASSERT_EQ(reports.size(), 1u) << "只有报齐的那一笔算完";
  EXPECT_EQ(reports[0].stream, 5u) << "报的是第一笔的业务身份";
  EXPECT_EQ(reports[0].task, 3u);
}

// 表满了就不再接纳：Commit 那一侧要先看 ready。
TEST(BachCompletionRs, FullTableStopsAdmitting) {
  bool room_at_start = false, room_when_full = true;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CompletionRs rs(clk, "rs", 0, false);
    CompHarness h(clk, rs);
    room_at_start = rs.HasRoom();
    for (uint64_t i = 0; i < kCompRsNum; ++i) {
      h.admits.push_back({2 + i * 2, Task(1 + i, i % kStreamNum, 3)});
    }
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    room_when_full = rs.HasRoom();
  }
  RT::Reset();
  EXPECT_TRUE(room_at_start);
  EXPECT_FALSE(room_when_full) << "十六项占满之后不再有空位";
}
