// issue_q 的三段重叠，以及 topK 的全局专家号翻成组内序号。
//
// 一笔任务要按 tile 走 kblock × nblock 遍，三个计数记的是发了几个读、算完几个、
// 写回几个；三段重叠就落在这三个计数的差上。topK 存的是 global index，算 weight
// 地址要的是 local index，中间隔着 local_ep_table。

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/mu/gen_ep_info.h"
#include "bach/ip/chip/core/mu/issue_q.h"
#include "bach/ip/chip/core/mu/mu.h"

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

  // 一开始只有「要发读」的那一笔，算那一段没得做。写回那一段问的是「结果该记
  // 给哪一笔」，这一笔的那一列还没写过，所以是它。真的有没有结果由计算那一
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
TEST(BachMuIssueQ, DepthIsSixteen) {
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

// 全局专家号经 local_ep_table 翻成组内序号；不在本组里就是配置错误。
TEST(BachMuGenEpInfo, GlobalIndexBecomesLocal) {
  ClockPtr clk = MakeClock(0, kPeriod);
  GenEpInfo g(clk, "gen", 0, false);
  // 本组内有 3 个专家，全局号分别是 17、5、23。
  g.SetLocalEpTable({17, 5, 23});

  uint64_t local = 0;
  EXPECT_TRUE(g.ToLocal(17, &local));
  EXPECT_EQ(local, 0u);
  EXPECT_TRUE(g.ToLocal(5, &local));
  EXPECT_EQ(local, 1u);
  EXPECT_TRUE(g.ToLocal(23, &local));
  EXPECT_EQ(local, 2u);
  EXPECT_FALSE(g.ToLocal(99, &local)) << "不在本组里的翻不出来";
  RT::Reset();
}

// topK 表由 MU 自己从 Core Mem 载入，载入完成前这一级不 ready。
TEST(BachMuGenEpInfo, TopkIsLoadedBeforeUse) {
  ClockPtr clk = MakeClock(0, kPeriod);
  GenEpInfo g(clk, "gen", 0, false);
  EXPECT_FALSE(g.Ready()) << "还没载入";

  std::vector<TopkEntry> t = {{17, 0.5f}, {5, 0.25f}};
  g.LoadTopk(t);
  EXPECT_TRUE(g.Ready());
  ASSERT_EQ(g.Topk().size(), 2u);
  EXPECT_EQ(g.Topk()[0].expert_id, 17u);
  EXPECT_FLOAT_EQ(g.Topk()[1].weight, 0.25f);

  // 换一个 task 就作废，免得读到半新半旧的一组专家。
  g.Invalidate();
  EXPECT_FALSE(g.Ready());
  RT::Reset();
}

// 从 Core Mem 读回来的字节按每项 {expert_id 2 B, weight 4 B} 解开。
TEST(BachMuGenEpInfo, TopkBytesAreParsedSixBytesEach) {
  std::vector<uint8_t> bytes(kTopkEntryBytes * 2, 0);
  // 第一项：expert 0x0011，weight 1.0f。
  bytes[0] = 0x11;
  bytes[1] = 0x00;
  uint32_t one = numeric::BitsOf(1.0f);
  for (int k = 0; k < 4; ++k) bytes[2 + k] = uint8_t((one >> (8 * k)) & 0xFF);
  // 第二项：expert 0x0102，weight 0.5f。
  bytes[6] = 0x02;
  bytes[7] = 0x01;
  uint32_t half = numeric::BitsOf(0.5f);
  for (int k = 0; k < 4; ++k) bytes[8 + k] = uint8_t((half >> (8 * k)) & 0xFF);

  std::vector<TopkEntry> t = GenEpInfo::Parse(bytes, 2);
  ASSERT_EQ(t.size(), 2u);
  EXPECT_EQ(t[0].expert_id, 0x11u);
  EXPECT_FLOAT_EQ(t[0].weight, 1.0f);
  EXPECT_EQ(t[1].expert_id, 0x102u);
  EXPECT_FLOAT_EQ(t[1].weight, 0.5f);
  // 每 stream 的 topK 区上限 256 B。
  EXPECT_EQ(kTopkBytesPerStream / kTopkEntryBytes, 42u);
}

namespace {

// 一个 MU 加它的三个存储桩：token / weight 读，结果写。
class MuHarness : public BachModule {
 public:
  MuHarness(ClockPtr c, Mu& target) : BachModule(c, "harness"), mu(target) {}

  uint64_t reads = 0, writes = 0;
  std::vector<MuDrain> states;

 protected:
  void Step() override {
    Serve(mu.TokenPort(), 0);
    Serve(mu.WeightPort(), 1);
    Serve(mu.OutPort(), 2);
    states.push_back(mu.DrainState());
    mu.RunStep();
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
    p.DriveSlave(true, rsp,
                 rsp ? std::make_shared<ByteBlock>(1024, 0) : nullptr);
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

// acu 查出越界就走 Drain 四步：先阻塞任务下发，收干净已发出的回复，排空计算
// 流水线，再回默认状态。
TEST(BachMuDrain, OutOfRangeTaskWalksTheFourSteps) {
  std::vector<MuDrain> seen;
  uint64_t drains = 0;
  {
    RT::Reset(8, 8);
    ClockPtr clk = MakeClock(0, kPeriod);
    MuCfg cfg;
    cfg.cmem_size = 0x1000;   // Core Mem 只有 4 KB
    cfg.mmem_size = 0x100000;
    Mu mu(clk, "mu", cfg);
    MuHarness h(clk, mu);
    MuConfig c(clk, mu, 2, 0);
    // token 地址落在 Core Mem 之外，acu 会拒。
    c.writes = {{kMuAddrToken, 0x100000},
                {kMuTaskBlock, 1 | (1u << 16)},
                {kMuSysCtrl, kMuTaskStart}};
    clk->Continue(120 * kPeriod);
    RT::JoinAll();
    seen = h.states;
    drains = mu.Drains();
  }
  RT::Reset();
  // 四步都要出现过，最后回到默认状态。
  bool block = false, clean = false, flush = false, restore = false;
  for (MuDrain d : seen) {
    if (d == MuDrain::kBlock) block = true;
    if (d == MuDrain::kClean) clean = true;
    if (d == MuDrain::kFlush) flush = true;
    if (d == MuDrain::kRestore) restore = true;
  }
  EXPECT_TRUE(block) << "第一步：阻塞任务下发";
  EXPECT_TRUE(clean) << "第二步：收干净已发出的回复";
  EXPECT_TRUE(flush) << "第三步：排空计算流水线";
  EXPECT_TRUE(restore) << "第四步：恢复默认状态";
  EXPECT_EQ(drains, 1u) << "走完一遍";
  EXPECT_EQ(seen.back(), MuDrain::kNone) << "最后回到默认状态";
}

// 地址合法的任务不触发 Drain。
TEST(BachMuDrain, LegalTaskDoesNotDrain) {
  uint64_t drains = 0;
  MuDrain last = MuDrain::kBlock;
  {
    RT::Reset(8, 8);
    ClockPtr clk = MakeClock(0, kPeriod);
    Mu mu(clk, "mu", MuCfg{});
    MuHarness h(clk, mu);
    MuConfig c(clk, mu, 2, 0);
    c.writes = {{kMuAddrToken, 0x0},
                {kMuAddrWeight, 0x0},
                {kMuAddrOut, 0x0},
                {kMuTaskBlock, 1 | (1u << 16)},
                {kMuSysCtrl, kMuTaskStart}};
    clk->Continue(120 * kPeriod);
    RT::JoinAll();
    drains = mu.Drains();
    last = mu.DrainState();
  }
  RT::Reset();
  EXPECT_EQ(drains, 0u);
  EXPECT_EQ(last, MuDrain::kNone);
}
