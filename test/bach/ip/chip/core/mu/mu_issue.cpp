// issue_q 的三段重叠，以及 topK 里的组内序号。
//
// 一笔任务要按 tile 走 kblock × nblock 遍，三个计数记的是发了几个读、算完几个、
// 写回几个；三段重叠就落在这三个计数的差上。topK 直接存组内序号（local index），
// 算 weight 地址读出来就用，中间没有 global→local 的翻译。

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/mu/gen_ep_info.h"
#include "bach/ip/chip/core/mu/issue_q.h"
#include "bach/ip/chip/core/mu/mu.h"
#include "bach/ip/chip/core/memory/core_mem.h"
#include "bach/ip/chip/core/memory/matrix_mem.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

MuTaskCfg Task(uint64_t kblock, uint64_t nblock) {
  MuTaskCfg c;
  c.kblock = kblock;
  c.nblock = nblock;
  return c;
}

}  // namespace

// 顺序执行：队头做完才出队，与执行通路上的重叠无关。
TEST(BachMuIssueQ, FinishIsInOrder) {
  ClockPtr clk = MakeClock(0, kPeriod);
  MuIssueQ q(clk, "q", 0, false);

  q.Push(Task(1, 1));
  q.Push(Task(1, 1));
  EXPECT_EQ(q.Size(), 2u);
  // 队头还没到 FINISH，出队不动它。
  q.RetireFront();
  EXPECT_EQ(q.Size(), 2u);
  q.Head()->stage = MuStage::kFinished;
  q.RetireFront();
  EXPECT_EQ(q.Size(), 1u);
  RT::Reset();
}

// 三段重叠：写回第 i 个 tile 的同时可以在算第 i+1 个、读第 i+2 个。
TEST(BachMuIssueQ, ThreeStagesOverlap) {
  ClockPtr clk = MakeClock(0, kPeriod);
  MuIssueQ q(clk, "q", 0, false);

  q.Push(Task(/*kblock=*/4, /*nblock=*/1));
  MuInflight* f = q.Head();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->total, 4u);

  // 这一笔是一列切成四段，所以只写回一次。
  EXPECT_EQ(f->OutTotal(), 1u);

  // 一开始只有“要发读”的那一笔，算那一段没得做。写回那一段问的是“结果该记
  // 给哪一笔”，这一笔的那一列还没写过，所以是它。真的有没有结果由计算那一
  // 级说了算。
  EXPECT_EQ(q.FirstToLoad(), f);
  EXPECT_EQ(q.FirstToCompute(), nullptr);
  EXPECT_EQ(q.FirstToStore(), f);

  // 发了两个 tile 的读、算完一个：三段这时各有各的活。
  f->issued = 2;
  f->computed = 1;
  EXPECT_EQ(q.FirstToLoad(), f) << "还有 tile 没发读";
  EXPECT_EQ(q.FirstToCompute(), f) << "发了两个算了一个，还能再算一个";
  EXPECT_EQ(q.FirstToStore(), f) << "这一列还没写回";

  // 全部发完读之后，读那一段就没得做了。
  f->issued = 4;
  f->computed = 4;
  f->stored = f->OutTotal();
  EXPECT_EQ(q.FirstToLoad(), nullptr);
  EXPECT_EQ(q.FirstToCompute(), nullptr);
  EXPECT_EQ(q.FirstToStore(), nullptr);
  EXPECT_TRUE(f->Done());
  RT::Reset();
}

// 前一笔还在算，后一笔就能开始读：任务之间也重叠。
TEST(BachMuIssueQ, NextTaskLoadsWhileThisOneComputes) {
  ClockPtr clk = MakeClock(0, kPeriod);
  MuIssueQ q(clk, "q", 0, false);

  q.Push(Task(1, 1));
  q.Push(Task(1, 1));
  MuInflight* first = q.Head();
  // 第一笔读完了、还没算完。
  first->issued = 1;
  EXPECT_EQ(q.FirstToCompute(), first);
  // 这时候要发读的已经是第二笔了。
  MuInflight* to_load = q.FirstToLoad();
  ASSERT_NE(to_load, nullptr);
  EXPECT_NE(to_load, first) << "第一笔读完了，读那一段轮到第二笔";
  RT::Reset();
}

// 队列满了要先看有没有空位再写 trigger。
TEST(BachMuIssueQ, DepthIsEight) {
  ClockPtr clk = MakeClock(0, kPeriod);
  MuIssueQ q(clk, "q", 0, false);
  for (uint64_t i = 0; i < kMuIssueQDepth; ++i) {
    EXPECT_TRUE(q.HasRoom()) << "第 " << i << " 笔";
    q.Push(Task(1, 1));
  }
  EXPECT_FALSE(q.HasRoom());
  EXPECT_EQ(q.Size(), kMuIssueQDepth);
  RT::Reset();
}

// topK 表由 DTE 搬运时经专用数据线按 stream_id 写进 topK_ep_table，MU 计算时按
// stream_id 读回同一份，各 stream 互不串。
TEST(BachMuGenEpInfo, DteWritesTopkMuReadsByStream) {
  ClockPtr clk = MakeClock(0, kPeriod);
  GenEpInfo g(clk, "gen", 0, false);

  auto pack = [](std::vector<TopkEntry> const& t) {
    std::vector<uint8_t> b(kTopkBytesPerStream, 0);
  b[0] = uint8_t(t.size());
  b[1] = uint8_t(t.size() >> 8);
    for (uint64_t i = 0; i < t.size(); ++i) {
      uint64_t off = 2 + i * kTopkEntryBytes;
      b[off] = uint8_t(t[i].local_ep_index & 0xFF);
      b[off + 1] = uint8_t((t[i].local_ep_index >> 8) & 0xFF);
      uint32_t w = numeric::BitsOf(t[i].weight);
      for (int k = 0; k < 4; ++k) {
        b[off + 2 + k] = uint8_t((w >> (8 * k)) & 0xFF);
      }
    }
    return b;
  };

  // DTE 按 stream_id 各写一份，写进 FF 阵列。
  g.WriteTopk(0, pack({{1, 0.5f}, {0, 0.25f}}));
  g.WriteTopk(3, pack({{2, 1.0f}}));

  // MU 按 stream_id 读回，各自那份互不串。
  ASSERT_EQ(g.Topk(0, 2).size(), 2u);
  EXPECT_EQ(g.Topk(0, 2)[0].local_ep_index, 1u);
  EXPECT_FLOAT_EQ(g.Topk(0, 2)[1].weight, 0.25f);
  ASSERT_EQ(g.Topk(3, 1).size(), 1u);
  EXPECT_EQ(g.Topk(3, 1)[0].local_ep_index, 2u);
  RT::Reset();
}

// 从 DTE 写进来的字节按每项 {local_ep_index 2 B, weight 4 B} 解开。
TEST(BachMuGenEpInfo, TopkBytesAreParsedSixBytesEach) {
  std::vector<uint8_t> bytes(256, 0);
  bytes[0] = 2;
  bytes[2] = 0x11;
  uint32_t one = numeric::BitsOf(1.0f);
  for (int k = 0; k < 4; ++k) bytes[4 + k] = uint8_t(one >> (8 * k));
  bytes[8] = 0x02;
  bytes[9] = 0x01;
  uint32_t half = numeric::BitsOf(0.5f);
  for (int k = 0; k < 4; ++k) bytes[10 + k] = uint8_t(half >> (8 * k));

  std::vector<TopkEntry> t = GenEpInfo::Parse(bytes, 2);
  ASSERT_EQ(t.size(), 2u);
  EXPECT_EQ(t[0].local_ep_index, 0x11u);
  EXPECT_FLOAT_EQ(t[0].weight, 1.0f);
  EXPECT_EQ(t[1].local_ep_index, 0x102u);
  EXPECT_FLOAT_EQ(t[1].weight, 0.5f);
  // 每 stream 的 topK 区上限 256 B。
  EXPECT_EQ(kTopkMaxExperts, 32u);
  EXPECT_EQ(kTopkTableNum, 16u);
}

namespace {

// 一个 MU 加它的三个存储桩：token / weight 读，结果写。
class MuHarness : public BachModule {
 public:
  MuHarness(ClockPtr c, Mu& target) : BachModule(c, "harness"), mu(target) {}

  uint64_t reads = 0, writes = 0;
  bool nan_token = false;
  uint64_t error_port = 3;
  std::function<void(uint64_t)> on_cycle;

 protected:
  void Step() override {
    Serve(mu.TokenPort(), 0);
    Serve(mu.WeightPort(), 1);
    Serve(mu.OutPort(), 2);
    mu.RunStep();
    if (on_cycle) on_cycle(CycleNow());
  }

 private:
  void Serve(MemPort& p, uint64_t which) {
    MemReqView r = ReadMemReq(p);
    bool rsp = false;
    if (r.valid && r.seq != last_seq[which]) {
      last_seq[which] = r.seq;
      if (r.we) {
        ++writes;
      } else {
        ++reads;
        pend[which].push_back(CycleNow() + 3);
      }
    }
    for (auto it = pend[which].begin(); it != pend[which].end(); ++it) {
      if (*it <= CycleNow()) {
        rsp = true;
        pend[which].erase(it);
        break;
      }
    }
    auto data = rsp ? std::make_shared<ByteBlock>(8192, 0) : nullptr;
    if (data && which == 0 && nan_token) { (*data)[0] = 0x81; (*data)[1] = 0x7f; }
    p.DriveSlave(true, rsp, data);
    p.rsp_error = rsp && which == error_port ? 1 : 0;
  }

  Mu& mu;
  std::array<std::vector<uint64_t>, 3> pend;
  std::array<uint64_t, 3> last_seq{};
};

// 写一笔配置再写 trigger。
class MuConfig : public BachModule {
 public:
  MuConfig(ClockPtr c, Mu& target, uint64_t at, uint64_t token_addr)
      : BachModule(c, "cfg"), mu(target), fire_at(at), addr(token_addr) {}

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    DsaCfgPort& p = mu.Cfg();
    if (cursor >= writes.size()) {
      p.Idle();
      return;
    }
    if (now < fire_at) {
      p.Idle();
      return;
    }
    if (driving && p.Ready()) {
      ++cursor;
      driving = false;
      if (cursor >= writes.size()) {
        p.Idle();
        return;
      }
    }
    p.Drive(writes[cursor].first, writes[cursor].second, cursor + 1);
    driving = true;
  }

 public:
  std::vector<std::pair<uint64_t, uint64_t>> writes;

 private:
  Mu& mu;
  uint64_t fire_at, addr;
  uint64_t cursor = 0;
  bool driving = false;
};

}  // namespace

// 非法任务不产生访存，错误队头保留到软件清除，后续合法任务仍可执行。
TEST(BachMuRecovery, InvalidTaskHoldsHeadWhileYoungerTaskExecutes) {
  RT::Reset(8, 8);
  {
    auto clk = MakeClock(0, kPeriod);
    MuCfg setting; setting.cmem_size = 0x1000;
    Mu mu(clk, "mu", setting);
    MuHarness harness(clk, mu);
    MuConfig writer(clk, mu, 2, 0);
    writer.writes = {{kMuAAddr, 0x100000}, {kMuPrimitiveDim, 0x10001},
        {kMuPrimitiveMode, kMuShareEpEn | (1u << kMuShareEpMaskShift) | kMuTaskLast},
        {kMuTaskTrigger, 0}, {kMuAAddr, 0}, {kMuCAddr, 0x800}, {kMuTaskTrigger, 0}};
    bool checked = false;
    harness.on_cycle = [&](uint64_t cycle) {
      if (cycle != 170) return;
      checked = true;
      EXPECT_EQ(mu.Error(), 1u); EXPECT_EQ(mu.ErrorReports(), 1u);
      EXPECT_EQ(mu.DoneCnt(), 0u);
      ASSERT_EQ(mu.IssueQ().Size(), 2u);
      auto younger = mu.IssueQ().AtSlot(1);
      ASSERT_NE(younger, nullptr); EXPECT_TRUE(younger->Done());
      EXPECT_EQ(harness.reads, 2u); EXPECT_EQ(harness.writes, 1u);
      mu.WriteControl(kMuCtrl, 2);
    };
    clk->Continue(180 * kPeriod); RT::JoinAll();
    EXPECT_TRUE(checked);
    EXPECT_EQ(mu.Error(), 0u);
    EXPECT_EQ(mu.DoneCnt(), 1u);
    EXPECT_TRUE(mu.Quiescent());
  }
  RT::Reset();
}

TEST(BachMuPlan, SparseSharedSlotsAndMixedRouterPrecision) {
  RT::Reset(8, 8);
  {
    auto clk = MakeClock(0, kPeriod);
    Mu mu(clk, "mu", MuCfg{});
    std::vector<uint8_t> topk(256);
    topk[0] = 1; topk[2] = 2;
    uint32_t weight = numeric::BitsOf(0.5f);
    for (uint64_t i = 0; i < 4; ++i) topk[4 + i] = uint8_t(weight >> (8 * i));
    mu.EpInfo().WriteTopk(0, topk);
    MuTaskCfg task;
    task.decoded_mode = true; task.a_dtype = numeric::DataType::kMxfp8;
    task.b_dtype = task.share_dtype = numeric::DataType::kMxfp8;
    task.router_dtype = numeric::DataType::kMxfp4;
    task.share_slot_count = 3; task.share_mask = 5;
    task.router_ep_en = true; task.router_limit = 2;
    task.kblock = 2; task.b_expert_stride = 256;
    task.output_expert_stride = 256; task.c_addr = 0x1000;
    ASSERT_TRUE(mu.Plan(task));
    ASSERT_EQ(task.experts.size(), 3u);
    EXPECT_EQ(task.experts[0].weight_base, 0u);
    EXPECT_EQ(task.experts[1].weight_base, 512u);
    EXPECT_EQ(task.experts[2].weight_base, 1024u);
    EXPECT_EQ(task.experts[0].kblock, 2u);
    EXPECT_EQ(task.experts[2].kblock, 1u);
    EXPECT_EQ(task.experts[2].coefficient, 0.5f);
    EXPECT_EQ(task.Tiles(), 5u);
    task.b_expert_stride = 64;
    EXPECT_FALSE(mu.Plan(task)) << "Used expert stride must cover its matrix";
  }
  RT::Reset();
}

TEST(BachMuPlan, EmptyTaskIgnoresUnusedFieldsAndRouterChecksLocalCount) {
  RT::Reset(8, 8);
  {
    auto clk = MakeClock(0, kPeriod);
    Mu mu(clk, "mu", MuCfg{});
    MuTaskCfg task; task.decoded_mode = true;
    task.a_addr = task.b_addr = task.c_addr = 0xffffffff;
    task.kblock = task.nblock = 0;
    EXPECT_TRUE(mu.Plan(task)); EXPECT_EQ(task.Experts(), 0u);
    task.router_ep_en = true; task.router_limit = 1;
    mu.EpInfo().WriteTopk(0, {2, 0});
    EXPECT_FALSE(mu.Plan(task)) << "Local R must not exceed configured global G";
  }
  RT::Reset();
}

TEST(BachMuRegfile, TemplateDirtyOverrideCommitsOnSecondEdge) {
  RT::Reset(8, 8);
  {
    auto clk = MakeClock(0, kPeriod);
    class Driver : public BachModule {
     public:
      Driver(ClockPtr c) : BachModule(c, "driver"), reg(c, "reg", 0, false) {}
      MuRegfile reg;
      bool early = false, committed = false;
      MuTaskCfg snapshot;
     protected:
      void Step() override {
        reg.RunStep();
        if (CycleNow() == 2) {
          reg.CfgWrite(0x1080, 0x30002);
          reg.CfgWrite(0x1084, 0x1000);
          reg.CfgWrite(0x10a0, kMuTaskLast);
          reg.CfgWrite(kMuAAddr, 0x2000);
          reg.CfgWrite(kMuTaskTrigger, 3);
          early = reg.HasPending();
        }
        if (CycleNow() == 3) {
          committed = reg.HasPending(); snapshot = reg.TakePending();
          EXPECT_EQ(reg.ConfigRead(kMuTaskTrigger), 3u);
          EXPECT_EQ(reg.ConfigRead(kMuPrimitiveDim), 0u);
          reg.CfgWrite(kMuTaskTrigger, 0);
        }
        if (CycleNow() == 4) {
          EXPECT_TRUE(reg.HasPending());
          EXPECT_EQ(reg.TakePending().kblock, 0u);
        }
      }
    } driver(clk);
    clk->Continue(8 * kPeriod); RT::JoinAll();
    EXPECT_FALSE(driver.early); EXPECT_TRUE(driver.committed);
    EXPECT_EQ(driver.snapshot.kblock, 3u); EXPECT_EQ(driver.snapshot.nblock, 2u);
    EXPECT_EQ(driver.snapshot.a_addr, 0x2000u); EXPECT_TRUE(driver.snapshot.task_last);
  }
  RT::Reset();
}

TEST(BachMuRecovery, ClearAllDrainsOutstandingReadsAndKeepsConfiguration) {
  RT::Reset(8, 8);
  {
    auto clk = MakeClock(0, kPeriod);
    Mu mu(clk, "mu", MuCfg{});
    MuHarness harness(clk, mu);
    MuConfig writer(clk, mu, 2, 0);
    writer.writes = {{kMuPrimitiveDim, 0x10001},
        {kMuPrimitiveMode, kMuShareEpEn | (1u << kMuShareEpMaskShift)}, {kMuTaskTrigger, 0}};
    bool requested = false;
    harness.on_cycle = [&](uint64_t) {
      if (!requested && harness.reads) { requested = true; mu.WriteControl(kMuCtrl, 1); }
    };
    clk->Continue(150 * kPeriod); RT::JoinAll();
    EXPECT_TRUE(requested); EXPECT_TRUE(mu.Quiescent());
    EXPECT_EQ(mu.DoneCnt(), 0u); EXPECT_EQ(mu.ReadControl(kMuCtrl), 0u);
    EXPECT_EQ(mu.Regfile().ConfigRead(kMuPrimitiveDim), 0x10001u);
    EXPECT_EQ(mu.IssueQ().Size(), 0u);
  }
  RT::Reset();
}

TEST(BachMuRecovery, DebugStepWaitsForTaskAndRunsExactlyOne) {
  RT::Reset(8, 8);
  {
    auto clk = MakeClock(0, kPeriod);
    Mu mu(clk, "mu", MuCfg{});
    MuHarness harness(clk, mu);
    MuConfig writer(clk, mu, 10, 0);
    writer.writes = {{kMuPrimitiveDim, 0x10001},
        {kMuPrimitiveMode, kMuShareEpEn | (1u << kMuShareEpMaskShift)},
        {kMuTaskTrigger, 0}, {kMuTaskTrigger, 0}};
    harness.on_cycle = [&](uint64_t cycle) {
      if (cycle == 2) { mu.WriteControl(0xc00, 1); mu.WriteControl(0xc04, 2); }
      if (cycle == 6) { EXPECT_EQ(mu.ReadControl(0xc04), 2u); }
    };
    clk->Continue(180 * kPeriod); RT::JoinAll();
    EXPECT_EQ(mu.DoneCnt(), 1u); EXPECT_EQ(mu.IssueQ().Size(), 1u);
    EXPECT_EQ(mu.ReadControl(kMuStatus), 4u); EXPECT_EQ(mu.ReadControl(0xc04), 0u);
  }
  RT::Reset();
}

TEST(BachMuStore, WideFramesMergeWithinTaskAndFlushAtTaskBoundary) {
  for (bool separate : {false, true}) {
    RT::Reset(8, 8);
    {
      auto clk = MakeClock(0, kPeriod);
      class Driver : public BachModule {
       public:
        Driver(ClockPtr c, bool task_boundary)
            : BachModule(c, "driver"), stq(c, "stq", 0, false), boundary(task_boundary) {}
        MuStq stq;
        std::vector<uint64_t> lengths;
        std::vector<uint8_t> memory = std::vector<uint8_t>(1040, 0xa5);
        bool saw_uncommitted = false;
       protected:
        void Step() override {
          auto& port = stq.Port();
          auto req = ReadMemReq(port);
          if (req.valid && req.seq != seen) {
            seen = req.seq; lengths.push_back(req.bytes);
            ASSERT_TRUE(req.wdata);
            for (uint64_t i = 0; i < req.bytes; ++i)
              memory.at(req.addr + req.woff + i) = req.wdata->at(req.woff + i);
          }
          port.DriveSlave(true, false, nullptr);
          stq.RunStep();
          if (stq.AllocatedBytes() > stq.CommittedBytes()) saw_uncommitted = true;
          if (CycleNow() == 2) stq.Push(8, std::vector<float>(128, 1.0f), false, boundary);
          if (CycleNow() == 3) stq.Push(520, std::vector<float>(128, 2.0f), false, true);
        }
       private:
        bool boundary;
        uint64_t seen = 0;
      } driver(clk, separate);
      clk->Continue(140 * kPeriod); RT::JoinAll();
      EXPECT_TRUE(driver.stq.Quiescent()); EXPECT_TRUE(driver.saw_uncommitted);
      EXPECT_EQ(driver.lengths, separate ? (std::vector<uint64_t>{248, 256, 8, 248, 256, 8}) :
          (std::vector<uint64_t>{248, 256, 256, 256, 8}));
      for (uint64_t i = 0; i < 8; ++i) EXPECT_EQ(driver.memory[i], 0xa5);
      for (uint64_t i = 1032; i < driver.memory.size(); ++i) EXPECT_EQ(driver.memory[i], 0xa5);
      for (uint64_t i = 0; i < 256; ++i) {
        uint32_t got = 0;
        for (uint64_t b = 0; b < 4; ++b) got |= uint32_t(driver.memory[8 + i * 4 + b]) << (8 * b);
        EXPECT_EQ(got, numeric::BitsOf(i < 128 ? 1.0f : 2.0f));
      }
    }
    RT::Reset();
  }
}

TEST(BachMuRecovery, NumericFlagsDefaultToNonTrappingAndSnapshotTheEnable) {
  for (bool trap : {false, true}) {
    RT::Reset(8, 8);
    {
      auto clk = MakeClock(0, kPeriod);
      Mu mu(clk, "mu", MuCfg{});
      MuHarness harness(clk, mu); harness.nan_token = true;
      MuConfig writer(clk, mu, 2, 0);
      writer.writes = {{kMuErrInfo, trap ? 1u << 12 : 0},
          {kMuPrimitiveDim, 0x10001},
          {kMuPrimitiveMode, kMuShareEpEn | (1u << kMuShareEpMaskShift) | kMuTaskLast},
          {kMuTaskTrigger, 0}, {kMuErrInfo, trap ? 0 : 1u << 12}};
      clk->Continue(150 * kPeriod); RT::JoinAll();
      EXPECT_NE(mu.Exe().Flags() & numeric::kFpNv, 0u);
      EXPECT_EQ(harness.writes, 1u) << "Arithmetic exception still completes the output";
      EXPECT_EQ(mu.DoneCnt(), trap ? 0u : 1u);
      EXPECT_EQ(mu.ErrorReports(), trap ? 1u : 0u);
      EXPECT_EQ(mu.Error(), trap ? 2u : 0u);
      EXPECT_EQ(mu.ReadControl(kMuErrInfo) & 1, trap ? 1u : 0u);
      if (trap) {
        EXPECT_NE(mu.ReadControl(kMuErrInfo) & (1u << 9), 0u);
        EXPECT_EQ(mu.ReadControl(kMuErrInfo) & (1u << 12), 0u);
      }
    }
    RT::Reset();
  }
}

TEST(BachMuRecovery, ControlReportStaysStableAndDoesNotDeleteTaskError) {
  RT::Reset(8, 8);
  {
    auto clk = MakeClock(0, kPeriod);
    Mu mu(clk, "mu", MuCfg{});
    MuHarness harness(clk, mu);
    MuConfig writer(clk, mu, 4, 0);
    writer.writes = {{kMuPrimitiveDim, 0},
        {kMuPrimitiveMode, kMuShareEpEn | (1u << kMuShareEpMaskShift)}, {kMuTaskTrigger, 0}};
    harness.on_cycle = [&](uint64_t cycle) {
      if (cycle == 2) mu.Regfile().CfgWrite(0x1200, 0);
      if (cycle == 30) {
        mu.Regfile().CfgWrite(0x2800, 0);
        EXPECT_EQ(mu.ReadControl(0x414), 0x1200u);
        EXPECT_EQ(mu.ErrorReports(), 1u); EXPECT_EQ(mu.Error(), 4u);
        ASSERT_EQ(mu.IssueQ().Size(), 1u);
        mu.WriteControl(kMuCtrl, 2);
        EXPECT_EQ(mu.IssueQ().Size(), 1u);
      }
      if (cycle == 40) {
        EXPECT_EQ(mu.Error(), 1u); EXPECT_EQ(mu.ErrorReports(), 2u);
        mu.WriteControl(kMuCtrl, 2);
      }
    };
    clk->Continue(60 * kPeriod); RT::JoinAll();
    EXPECT_TRUE(mu.Quiescent()); EXPECT_EQ(mu.Error(), 0u);
  }
  RT::Reset();
}

TEST(BachMuRecovery, EmptyAndRejectedTasksDoNotWaitForMmemWindow) {
  for (bool invalid : {false, true}) {
    RT::Reset(8, 8);
    {
      auto clk = MakeClock(0, kPeriod);
      Mu mu(clk, "mu", MuCfg{}); MuHarness harness(clk, mu);
      uint64_t requests = 0;
      mu.BindMmemWindow([&](bool on) { if (on) ++requests; }, [] { return false; });
      MuConfig writer(clk, mu, 2, 0);
      writer.writes = {{kMuPrimitiveMode, invalid ? kMuShareEpEn | (1u << kMuShareEpMaskShift) : 0},
          {kMuTaskTrigger, 0}};
      clk->Continue(50 * kPeriod); RT::JoinAll();
      EXPECT_EQ(requests, 0u); EXPECT_EQ(harness.reads, 0u);
      EXPECT_EQ(mu.DoneCnt(), invalid ? 0u : 1u);
      EXPECT_EQ(mu.Error(), invalid ? 1u : 0u);
    }
    RT::Reset();
  }
}

TEST(BachMuRecovery, ErrorHeadDoesNotRestartYoungerWindowAcquisition) {
  RT::Reset(8, 8);
  {
    auto clk = MakeClock(0, kPeriod);
    Mu mu(clk, "mu", MuCfg{}); MuHarness harness(clk, mu); harness.nan_token = true;
    uint64_t acquisition = 0, releases = 0;
    mu.BindMmemWindow([&](bool on) { if (on) ++acquisition; else { acquisition = 0; ++releases; } },
                      [&] { return acquisition >= 3; });
    MuConfig writer(clk, mu, 2, 0);
    writer.writes = {{kMuErrInfo, 1u << 12}, {kMuPrimitiveDim, 0x10001},
        {kMuPrimitiveMode, kMuShareEpEn | (1u << kMuShareEpMaskShift)},
        {kMuTaskTrigger, 0}, {kMuTaskTrigger, 0}};
    clk->Continue(300 * kPeriod); RT::JoinAll();
    ASSERT_EQ(mu.IssueQ().Size(), 2u);
    EXPECT_TRUE(mu.IssueQ().AtSlot(1)->Done());
    // The two tasks share a continuous read window while their stages overlap.
    EXPECT_EQ(harness.writes, 2u); EXPECT_EQ(releases, 1u);
    EXPECT_EQ(mu.ErrorReports(), 1u);
  }
  RT::Reset();
}

TEST(BachMuStore, WaveAndReturnReservationsSustainFullRateBursts) {
  for (bool wide : {false, true}) {
    RT::Reset(8, 8);
    {
      auto clk = MakeClock(0, kPeriod);
      class Driver : public BachModule {
       public:
        Driver(ClockPtr c) : BachModule(c, "driver"), stq(c, "stq", 0, false) {}
        MuStq stq;
        uint64_t writes = 0, first_write = 0, last_write = 0;
       protected:
        void Step() override {
          auto& port = stq.Port(); auto req = ReadMemReq(port);
          if (req.valid && req.seq != seen) {
            seen = req.seq; ++writes; if (!first_write) first_write = CycleNow(); last_write = CycleNow();
          }
          port.DriveSlave(true, false, nullptr); stq.RunStep();
        }
       private:
        uint64_t seen = 0;
      } driver(clk);
      uint64_t frames = wide ? 37 : 74, bytes = wide ? 512 : 256;
      std::vector<float> values(wide ? 128 : 64, 1.0f);
      for (uint64_t i = 0; i < frames; ++i) {
        ASSERT_TRUE(driver.stq.HasRoom(wide)) << "frame " << i;
        driver.stq.Push(i * bytes, values, false, i + 1 == frames);
      }
      EXPECT_FALSE(driver.stq.HasRoom(wide));
      clk->Continue(200 * kPeriod); RT::JoinAll();
      EXPECT_TRUE(driver.stq.Quiescent()); EXPECT_EQ(driver.writes, 74u);
      EXPECT_LE(driver.first_write, 70u); EXPECT_LE(driver.last_write, 145u);
    }
    RT::Reset();
  }
}

TEST(BachMuRecovery, MisalignedCsrAccessReportsControlAddressError) {
  RT::Reset(8, 8);
  {
    auto clk = MakeClock(0, kPeriod);
    Mu mu(clk, "mu", MuCfg{}); MuHarness harness(clk, mu);
    harness.on_cycle = [&](uint64_t cycle) { if (cycle == 2) mu.Regfile().CfgWrite(0x403, 0); };
    clk->Continue(20 * kPeriod); RT::JoinAll();
    EXPECT_EQ(mu.ErrorReports(), 1u);
    EXPECT_EQ(mu.ReadControl(kMuErrInfo) & 0x803, 0x800u);
    EXPECT_EQ(mu.ReadControl(0x414), 0x403u);
    EXPECT_TRUE(mu.IssueQ().Empty());
  }
  RT::Reset();
}

TEST(BachMuRecovery, TaskAddressFaultsKeepTheirTypeAndSlaveOffset) {
  struct Case { uint64_t offset, value, info, exception, address; };
  const std::vector<Case> cases = {
      {kMuPrimitiveDim, 0, 0x801, 0x20000, 0},
      {kMuAAddr, 4, 0xc09, 0x2000a, 4},
      {kMuBAddr, 64, 0xc0d, 0x2000b, 64},
      {kMuAAddr, 0x100000, 0xc11, 0x2000c, 0x100000},
      {kMuBAddr, 0x80000, 0xc15, 0x2000d, 0x80000}};
  for (auto const& item : cases) {
    RT::Reset(8, 8);
    {
      auto clk = MakeClock(0, kPeriod);
      Mu mu(clk, "mu", MuCfg{}); MuHarness harness(clk, mu);
      MuConfig writer(clk, mu, 2, 0);
      writer.writes = {{kMuPrimitiveDim, 0x10001},
          {kMuPrimitiveMode, kMuShareEpEn | (1u << kMuShareEpMaskShift)},
          {item.offset, item.value}, {kMuTaskTrigger, 0}};
      clk->Continue(100 * kPeriod); RT::JoinAll();
      EXPECT_EQ(mu.ReadControl(kMuErrInfo), item.info);
      EXPECT_EQ(mu.ReadControl(0x202c), item.exception);
      EXPECT_EQ(mu.ReadControl(0x2030), item.address);
      EXPECT_EQ(harness.reads, 0u); EXPECT_EQ(harness.writes, 0u);
    }
    RT::Reset();
  }
}

TEST(BachMuRecovery, EccFaultsCompleteWritesAndReportTheCorrectMemory) {
  for (uint64_t memory : {0u, 1u}) {
    RT::Reset(8, 8);
    {
      auto clk = MakeClock(0, kPeriod);
      Mu mu(clk, "mu", MuCfg{}); MuHarness harness(clk, mu); harness.error_port = memory;
      MuConfig writer(clk, mu, 2, 0);
      writer.writes = {{kMuPrimitiveDim, 0x10001}, {kMuAAddr, 0x100}, {kMuBAddr, 0x80},
          {kMuCAddr, 0x800}, {kMuPrimitiveMode, kMuShareEpEn | (1u << kMuShareEpMaskShift)},
          {kMuTaskTrigger, 0}};
      clk->Continue(160 * kPeriod); RT::JoinAll();
      EXPECT_EQ(mu.ReadControl(kMuErrInfo), memory ? 0xc1du : 0xc19u);
      EXPECT_EQ(mu.ReadControl(0x202c), memory ? 0x2000fu : 0x2000eu);
      EXPECT_EQ(mu.ReadControl(0x2030), memory ? 0x80u : 0x100u);
      EXPECT_EQ(harness.writes, 1u); EXPECT_EQ(mu.ErrorReports(), 1u);
    }
    RT::Reset();
  }
}

TEST(BachMuPlan, UsedReferenceStrideMustBeAlignedEvenWhenTheIndexHidesIt) {
  RT::Reset(8, 8);
  {
    auto clk = MakeClock(0, kPeriod);
    Mu mu(clk, "mu", MuCfg{});
    std::vector<uint8_t> topk(256); topk[0] = 1; topk[2] = 2;
    mu.EpInfo().WriteTopk(0, topk);
    MuTaskCfg task; task.decoded_mode = true; task.router_ep_en = true;
    task.router_limit = 1; task.b_expert_stride = 192;
    MuInflight::Fault fault;
    EXPECT_FALSE(mu.Plan(task, &fault));
    EXPECT_EQ(fault.type, 1u); EXPECT_TRUE(fault.mmem); EXPECT_FALSE(fault.address_valid);
  }
  RT::Reset();
}

TEST(BachMuLoad, WeightPrefetchKeepsReturnedDataSeparateFromOutstandingRequests) {
  RT::Reset(8, 8);
  {
    auto clk = MakeClock(0, kPeriod);
    Mu mu(clk, "mu", MuCfg{});
    MuTaskCfg task;
    task.decoded_mode = true;
    task.share_ep_en = true;
    task.share_slot_count = 1;
    task.share_mask = 1;
    task.a_dtype = task.b_dtype = task.share_dtype = numeric::DataType::kMxfp8;
    task.kblock = 64;
    task.nblock = 1;
    task.c_addr = 0x8000;
    task.out_bf16 = true;
    task.task_last = true;
    mu.IssueQ().Push(task);
    class Driver : public BachModule {
     public:
      Driver(ClockPtr clock, Mu& target) : BachModule(clock, "driver"), mu(target) {}
      uint64_t weights = 0, before_returns = 0, while_a_waits = 0, completed = 0;
     protected:
      void Step() override {
        Serve(mu.TokenPort(), 0);
        Serve(mu.WeightPort(), 1);
        mu.OutPort().DriveSlave(true, false, nullptr);
        mu.RunStep();
        if (CycleNow() == 20) before_returns = weights;
        if (CycleNow() == 150) while_a_waits = weights;
        completed = mu.DoneCnt();
      }
     private:
      void Serve(MemPort& port, uint64_t which) {
        auto request = ReadMemReq(port);
        if (request.valid && request.seq != seen[which]) {
          seen[which] = request.seq;
          if (which == 1) ++weights;
          // Hold A long enough to fill the returned-B buffer. Delay the first
          // B returns separately to exercise the outstanding-request limit.
          pending[which].push_back(std::max<uint64_t>(which ? 40 : 200,
                                                     CycleNow() + 9));
        }
        bool response = !pending[which].empty() && pending[which].front() <= CycleNow();
        if (response) pending[which].pop_front();
        port.DriveSlave(true, response,
                       response ? std::make_shared<ByteBlock>(8448, 0) : nullptr);
      }
      Mu& mu;
      std::array<uint64_t, 2> seen{};
      std::array<std::deque<uint64_t>, 2> pending;
    } driver(clk, mu);
    clk->Continue(500 * kPeriod);
    RT::JoinAll();
    EXPECT_EQ(driver.before_returns, 12u);
    EXPECT_EQ(driver.while_a_waits, 32u);
    EXPECT_EQ(driver.weights, 64u);
    EXPECT_EQ(driver.completed, 1u);
    EXPECT_TRUE(mu.Quiescent());
  }
  RT::Reset();
}

TEST(BachMuOverlap, YoungerTasksExecuteWhileOlderWritesDrainAndRetireInOrder) {
  RT::Reset(8, 8);
  {
    auto clk = MakeClock(0, kPeriod);
    Mu mu(clk, "mu", MuCfg{});
    MuHarness harness(clk, mu);
    for (uint64_t task_id = 0; task_id < 3; ++task_id) {
      MuTaskCfg task;
      task.a_dtype = task.b_dtype = numeric::DataType::kMxfp8;
      task.kblock = 6;
      task.nblock = 4;
      task.expert_count = 2;
      task.b_expert_stride = 0x30000;
      task.c_addr = 0x10000 + task_id * 0x1000;
      task.output_expert_stride = 512;
      task.out_bf16 = task.task_last = true;
      task.task_id = task_id;
      mu.IssueQ().Push(task);
    }
    bool prefetched_before_write = false, computed_before_write = false;
    bool retired_with_younger_writes = false;
    std::vector<uint64_t> retirement;
    harness.on_cycle = [&](uint64_t) {
      if (mu.DispatchCnt() >= 2 && harness.writes == 0) prefetched_before_write = true;
      if (mu.CalcStartCnt() >= 2 && harness.writes == 0) computed_before_write = true;
      if (mu.DoneCnt() > retirement.size()) {
        retirement.push_back(mu.DoneTask());
        if (retirement.size() == 1) retired_with_younger_writes = !mu.Stq().Quiescent();
      }
    };
    clk->Continue(400 * kPeriod);
    RT::JoinAll();
    EXPECT_TRUE(prefetched_before_write);
    EXPECT_TRUE(computed_before_write);
    EXPECT_TRUE(retired_with_younger_writes);
    EXPECT_EQ(retirement, (std::vector<uint64_t>{0, 1, 2}));
    EXPECT_TRUE(mu.Quiescent());
  }
  RT::Reset();
}

TEST(BachMuOverlap, PrefetchAndComputeKeepNumericFlagsWithTheirOwnTask) {
  RT::Reset(8, 8);
  {
    auto clk = MakeClock(0, kPeriod);
    Mu mu(clk, "mu", MuCfg{});
    for (uint64_t id = 0; id < 2; ++id) {
      MuTaskCfg task;
      task.kblock = 32;
      task.a_addr = id * 0x8000;
      task.c_addr = 0x10000 + id * 0x1000;
      task.numeric_error_en = task.task_last = true;
      mu.IssueQ().Push(task);
    }
    class Driver : public BachModule {
     public:
      Driver(ClockPtr clock, Mu& target) : BachModule(clock, "driver"), mu(target) {}
      uint64_t writes = 0;
     protected:
      void Step() override {
        Serve(mu.TokenPort(), 0);
        Serve(mu.WeightPort(), 1);
        auto output = ReadMemReq(mu.OutPort());
        if (output.valid && output.seq != last_write) { last_write = output.seq; ++writes; }
        mu.OutPort().DriveSlave(true, false, nullptr);
        mu.RunStep();
      }
     private:
      void Serve(MemPort& port, uint64_t which) {
        auto request = ReadMemReq(port);
        if (request.valid && request.seq != seen[which]) {
          seen[which] = request.seq;
          auto bytes = std::make_shared<ByteBlock>(8192, 0);
          if (!which && request.addr < 0x8000) { (*bytes)[0] = 0x81; (*bytes)[1] = 0x7f; }
          pending[which].push_back({CycleNow() + 3, bytes});
        }
        bool response = !pending[which].empty() && pending[which].front().first <= CycleNow();
        auto data = response ? pending[which].front().second : nullptr;
        if (response) pending[which].pop_front();
        port.DriveSlave(true, response, data);
      }
      Mu& mu;
      uint64_t last_write = 0;
      std::array<uint64_t, 2> seen{};
      std::array<std::deque<std::pair<uint64_t, ByteBlockPtr>>, 2> pending;
    } driver(clk, mu);
    clk->Continue(300 * kPeriod);
    RT::JoinAll();
    ASSERT_EQ(mu.IssueQ().Size(), 2u);
    auto first = mu.IssueQ().AtSlot(0);
    auto second = mu.IssueQ().AtSlot(1);
    EXPECT_EQ(first->error, 2u);
    EXPECT_NE(first->numeric_flags & numeric::kFpNv, 0u);
    EXPECT_EQ(second->error, 0u);
    EXPECT_EQ(second->numeric_flags, 0u);
    EXPECT_EQ(second->stage, MuStage::kFinished);
    EXPECT_EQ(driver.writes, 2u);
  }
  RT::Reset();
}

TEST(BachMuOverlap, TopkUpdatesDoNotChangeAnAlreadyPlannedTask) {
  RT::Reset(8, 8);
  {
    auto clk = MakeClock(0, kPeriod);
    Mu mu(clk, "mu", MuCfg{});
    CoreMem tokens(clk, "tokens", 0, false);
    MatrixMem weights(clk, "weights", 0, false);
    mu.AttachCmemRd(tokens.PortPtr(kCmemMuRd));
    mu.AttachCmemWr(tokens.PortPtr(kCmemMuWr));
    mu.AttachMmemRd(weights.PortPtr(kMmemMu));
    tokens.Poke(0, ByteBlock(4096, 0x38));
    tokens.PokeScale(0, ByteBlock(128, 127));
    weights.Poke(0, ByteBlock(512 * 1024, 0x38));
    weights.PokeScale(0, ByteBlock(16 * 1024, 127));
    ByteBlock initial(256, 0);
    initial[0] = 1; initial[6] = 0x80; initial[7] = 0x3f; // expert0, coefficient1
    mu.EpInfo().WriteTopk(0, initial);
    for (uint64_t id = 0; id < 2; ++id) {
      MuTaskCfg task;
      task.decoded_mode = task.router_ep_en = task.ep_reduce = true;
      task.router_limit = 1;
      task.a_dtype = task.b_dtype = task.router_dtype = numeric::DataType::kMxfp8;
      task.kblock = 32; task.nblock = 1; task.b_expert_stride = 4096;
      task.c_addr = 0x4000 + id * 0x1000;
      task.out_bf16 = task.task_last = true;
      mu.IssueQ().Push(task);
    }
    class Driver : public BachModule {
     public:
      Driver(ClockPtr clock, Mu& unit, CoreMem& cm, MatrixMem& mm)
          : BachModule(clock, "driver"), mu(unit), tokens(cm), weights(mm) {}
      uint64_t completed = 0;
     protected:
      void Step() override {
        tokens.RunStep(); weights.RunStep(); mu.RunStep();
        if (CycleNow() == 10) {
          ByteBlock next(256, 0);
          next[0] = 1; next[2] = 1; next[7] = 0x40; // expert1, coefficient2
          mu.EpInfo().WriteTopk(0, next);
        }
        completed = mu.DoneCnt();
      }
     private:
      Mu& mu; CoreMem& tokens; MatrixMem& weights;
    } driver(clk, mu, tokens, weights);
    clk->Continue(500 * kPeriod);
    RT::JoinAll();
    ASSERT_EQ(driver.completed, 2u);
    for (uint64_t id = 0; id < 2; ++id) {
      auto data = tokens.Peek(0x4000 + id * 0x1000, 128);
      uint16_t expected = numeric::ToBf16(id ? 8192.0f : 4096.0f);
      for (uint64_t column = 0; column < 64; ++column)
        EXPECT_EQ(uint16_t(data[column * 2] | (uint16_t(data[column * 2 + 1]) << 8)), expected);
    }
  }
  RT::Reset();
}
