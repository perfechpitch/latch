// 通道与地址生成。
//
// 软件只配基址，Core Mem 那一侧的偏移由硬件按 stream_id 算，Matrix Mem 那一侧
// 配的就是最终物理地址；scale 与 topK 的长度硬件自己算；通道内按序激活，读那一
// 半的领先量受 buffer 与 outstanding 限额约束；MM → CM 只写 Core Mem。

#include <gtest/gtest.h>

#include <iostream>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/dte/lane.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

CmemLayout Layout() {
  CmemLayout l;
  l.stream_base = 0x10000;
  l.stream_stride = 0x8000;
  l.header_base = 0x1000;
  l.scale_base = 0x2000;
  l.topk_base = 0x3000;
  return l;
}

std::shared_ptr<Descriptor> Task(uint64_t commit_seq, Route route,
                                 uint64_t stream, uint64_t bytes,
                                 uint64_t src, uint64_t dst) {
  auto d = std::make_shared<Descriptor>();
  d->valid = true;
  d->commit_seq = commit_seq;
  d->route = route;
  d->stream_id = stream;
  d->bytes = bytes;
  d->src_addr = src;
  d->dst_addr = dst;
  auto m = std::make_shared<Message>();
  m->size = bytes;
  m->payload.assign(bytes, 0);
  d->msg = m;
  return d;
}

// 灌任务、扮演两块存储、收 Router 那一路。
class LaneHarness : public BachModule {
 public:
  struct Job {
    uint64_t at = 0;
    std::shared_ptr<Descriptor> d;
  };

  LaneHarness(ClockPtr c, Lane& target) : BachModule(c, "harness"), ln(target) {}

  std::vector<Job> jobs;
  // 存储回响应的延迟。
  uint64_t mem_latency = 2;

  struct MemOp {
    uint64_t at = 0, addr = 0;
    bool write = false, to_cmem = true;
  };
  std::vector<MemOp> ops;
  uint64_t router_beats = 0;
  uint64_t peak_outstanding = 0;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做：先看上一拍摆出来的请求。
    Serve(ln.Cmem(), true, now);
    Serve(ln.Mmem(), false, now);
    if (ln.ToRouter().Valid()) ++router_beats;
    ln.ToRouter().DriveReady(true);

    bool drove = false;
    for (auto const& j : jobs) {
      if (j.at != now) continue;
      ln.AdmitPtr()->Drive(j.d, ++admit_seq);
      drove = true;
      break;
    }
    if (!drove) ln.AdmitPtr()->Idle();

    ln.RunStep();
  }

 private:
  void Serve(MemPort& p, bool to_cmem, uint64_t now) {
    MemReqView r = ReadMemReq(p);
    if (r.valid && r.seq != last_seq[to_cmem ? 0 : 1]) {
      last_seq[to_cmem ? 0 : 1] = r.seq;
      ops.push_back({now, r.addr, r.we, to_cmem});
      if (!r.we) {
        pend.push_back({now + mem_latency, to_cmem});
        if (pend.size() > peak_outstanding) peak_outstanding = pend.size();
      }
    }
    bool rsp = false;
    for (auto it = pend.begin(); it != pend.end(); ++it) {
      if (it->to_cmem == to_cmem && it->at <= now) {
        rsp = true;
        pend.erase(it);
        break;
      }
    }
    p.DriveSlave(true, rsp,
                 rsp ? std::make_shared<ByteBlock>(kFlitBytes, 9) : nullptr);
  }

  struct Pend {
    uint64_t at = 0;
    bool to_cmem = true;
  };
  Lane& ln;
  std::vector<Pend> pend;
  std::array<uint64_t, 2> last_seq{};
  uint64_t admit_seq = 0;
};

}  // namespace

// Core Mem 那一侧按 stream_id 叠偏移，Matrix Mem 那一侧不叠。
TEST(BachAgcu, StreamOffsetOnCoreMemSideOnly) {
  Agcu a(Layout());
  EXPECT_EQ(a.DataAddr(0, /*core_mem=*/true, 0x40), 0x10000u + 0x40);
  EXPECT_EQ(a.DataAddr(3, true, 0x40), 0x10000u + 3 * 0x8000 + 0x40);
  // Matrix Mem 一侧配的就是最终物理地址。
  EXPECT_EQ(a.DataAddr(3, /*core_mem=*/false, 0x9000), 0x9000u);
  EXPECT_EQ(a.DataAddr(0, false, 0x9000), 0x9000u);
}

// 包头、scale、topK 三个分区各按各的跨度算。
TEST(BachAgcu, EachPartitionHasItsOwnStride) {
  Agcu a(Layout());
  EXPECT_EQ(a.HeaderAddr(0), 0x1000u);
  EXPECT_EQ(a.HeaderAddr(2), 0x1000u + 2 * 18);
  EXPECT_EQ(a.ScaleAddr(2), 0x2000u + 2 * 2048);
  EXPECT_EQ(a.TopkAddr(2), 0x3000u + 2 * 256);
}

// scale 与 topK 的长度硬件自己算，不用软件配。
TEST(BachAgcu, DerivedLengthsAreComputed) {
  // 32 个元素共用一个 scale。
  EXPECT_EQ(Agcu::ScaleBytes(512), 16u);
  EXPECT_EQ(Agcu::ScaleBytes(6144), 192u);
  // topK 每项 {expert_id 2 B, weight 4 B}。
  EXPECT_EQ(Agcu::TopkBytes(8), 48u);
}

// 出核读那一半的领先量卡在 outstanding 限额上，不会无限发请求。
TEST(BachDteLane, ReadAheadStopsAtTheOutstandingLimit) {
  uint64_t peak = 0, reads = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteBuffer buf(clk, "buf", 64, 1, 0, false);
    Agcu agcu(Layout());
    // 出核通道，进核那个走的是另一条路。
    Lane ln(clk, "lane", kOutCh0, buf, agcu, 0, false);
    LaneHarness h(clk, ln);
    // 存储很慢，读请求会攒着。
    h.mem_latency = 20;
    h.jobs = {{2, Task(1, Route::kCmToRouter, 0, 8 * kFlitBytes, 0, 0)}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    peak = h.peak_outstanding;
    for (auto const& o : h.ops) {
      if (!o.write) ++reads;
    }
  }
  RT::Reset();
  EXPECT_GT(reads, 0u);
  EXPECT_LE(peak, kRdOutstanding) << "在途读请求不超过限额";
}

// 通道内按序激活：先进队的先做完。
TEST(BachDteLane, TasksActivateInOrder) {
  std::vector<uint64_t> addrs;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteBuffer buf(clk, "buf", 64, 1, 0, false);
    Agcu agcu(Layout());
    Lane ln(clk, "lane", kOutCh0, buf, agcu, 0, false);
    LaneHarness h(clk, ln);
    // 两笔出核任务，源地址不同：第一笔从 0 起，第二笔从 0x100 起。
    h.jobs = {{2, Task(1, Route::kCmToRouter, 0, kFlitBytes, 0, 0)},
              {4, Task(2, Route::kCmToRouter, 0, kFlitBytes, 0x100, 0)}};
    clk->Continue(120 * kPeriod);
    RT::JoinAll();
    for (auto const& o : h.ops) {
      if (!o.write) addrs.push_back(o.addr);
    }
  }
  RT::Reset();
  ASSERT_GE(addrs.size(), 2u);
  EXPECT_EQ(addrs[0], 0x10000u) << "第一笔的源地址";
  EXPECT_EQ(addrs[1], 0x10000u + 0x100) << "第二笔排在它后面";
}

// MM → CM 那一档只写 Core Mem：出核通道的数据不会被写回 Matrix Mem。
TEST(BachDteLane, MatrixToCoreWritesOnlyCoreMem) {
  uint64_t cmem_writes = 0, mmem_writes = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteBuffer buf(clk, "buf", 64, 1, 0, false);
    Agcu agcu(Layout());
    // 走 out_ch[3]，MM → CM 固定复用它。
    Lane ln(clk, "lane", kInnerLane, buf, agcu, 0, false);
    LaneHarness h(clk, ln);
    h.jobs = {{2, Task(1, Route::kMmToCm, 1, kFlitBytes, 0x9000, 0x40)}};
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    for (auto const& o : h.ops) {
      if (!o.write) continue;
      if (o.to_cmem) {
        ++cmem_writes;
      } else {
        ++mmem_writes;
      }
    }
  }
  RT::Reset();
  EXPECT_GT(cmem_writes, 0u) << "写的是 Core Mem";
  EXPECT_EQ(mmem_writes, 0u) << "一笔都不该写回 Matrix Mem";
}

// 一笔搬得比中间 Buffer 大得多也要逐 beat 流过去，每一个字节都写到。R core
// 把一个用户的整个槽从 Matrix Mem 搬进 Core Mem 就是这个量级。
TEST(BachDteLane, LongMoveWritesEveryBeat) {
  constexpr uint64_t kBytes = 0xC200;   // 49664 B，合 194 个 beat
  uint64_t writes = 0;
  std::vector<uint64_t> addrs;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteBuffer buf(clk, "buf", kDteBufFlits, 1, 0, false);
    Agcu agcu(Layout());
    Lane ln(clk, "lane", kInnerLane, buf, agcu, 0, false);
    LaneHarness h(clk, ln);
    h.jobs = {{2, Task(1, Route::kMmToCm, 0, kBytes, 0x9000, 0)}};
    clk->Continue(8000 * kPeriod);
    RT::JoinAll();
    for (auto const& o : h.ops) {
      if (!o.write || !o.to_cmem) continue;
      ++writes;
      addrs.push_back(o.addr);
    }
  }
  RT::Reset();
  uint64_t want = (kBytes + kFlitBytes - 1) / kFlitBytes;
  EXPECT_EQ(writes, want) << "每个 beat 都要写出去";
  ASSERT_FALSE(addrs.empty());
  EXPECT_EQ(addrs.front(), Layout().stream_base) << "从段首写起";
  EXPECT_EQ(addrs.back(), Layout().stream_base + (want - 1) * kFlitBytes)
      << "写到段尾";
}

// 出核从 Matrix Mem 读时不叠 stream 偏移，配的地址就是最终地址。
TEST(BachDteLane, MatrixSideAddressIsUsedAsIs) {
  uint64_t first_read = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteBuffer buf(clk, "buf", 64, 1, 0, false);
    Agcu agcu(Layout());
    Lane ln(clk, "lane", kInnerLane, buf, agcu, 0, false);
    LaneHarness h(clk, ln);
    h.jobs = {{2, Task(1, Route::kMmToCm, /*stream=*/2, kFlitBytes, 0x9000,
                       0x40)}};
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    for (auto const& o : h.ops) {
      if (!o.write) {
        first_read = o.addr;
        break;
      }
    }
  }
  RT::Reset();
  EXPECT_EQ(first_read, 0x9000u) << "Matrix Mem 那一侧不叠 stream_id × stride";
}

// issue_done 就把上下文腾出来：第一笔的请求发完之后，第二笔当拍就能开始发，
// 不必等第一笔的响应全回来。
TEST(BachDteLane, NextTaskStartsAfterIssueDone) {
  std::vector<uint64_t> read_at;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteBuffer buf(clk, "buf", 64, 1, 0, false);
    Agcu agcu(Layout());
    Lane ln(clk, "lane", kOutCh0, buf, agcu, 0, false);
    LaneHarness h(clk, ln);
    // 存储很慢：第一笔的响应要 30 拍才回来。
    h.mem_latency = 30;
    h.jobs = {{2, Task(1, Route::kCmToRouter, 0, kFlitBytes, 0, 0)},
              {4, Task(2, Route::kCmToRouter, 0, kFlitBytes, 0x100, 0)}};
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    for (auto const& o : h.ops) {
      if (!o.write) read_at.push_back(o.at);
    }
  }
  RT::Reset();
  ASSERT_GE(read_at.size(), 2u) << "两笔的读都要发出去";
  EXPECT_LT(read_at[1] - read_at[0], 30u)
      << "第二笔不该等第一笔的响应回来才发";
}

// 等收敛的任务边界数有上限：满了就先不激活新的。
TEST(BachDteLane, DrainSlotsBoundTheReadAhead) {
  uint64_t reads = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteBuffer buf(clk, "buf", 64, 1, 0, false);
    Agcu agcu(Layout());
    Lane ln(clk, "lane", kOutCh0, buf, agcu, 0, false);
    LaneHarness h(clk, ln);
    // 响应一直不回来，几笔任务会堆在等收敛的那一队里。
    h.mem_latency = 100000;
    for (uint64_t i = 0; i < 6; ++i) {
      h.jobs.push_back({2 + i * 2,
                        Task(1 + i, Route::kCmToRouter, 0, kFlitBytes,
                             i * 0x100, 0)});
    }
    clk->Continue(120 * kPeriod);
    RT::JoinAll();
    for (auto const& o : h.ops) {
      if (!o.write) ++reads;
    }
  }
  RT::Reset();
  // 一笔在发、kDrainSlots 笔在等，再多就不激活了。
  EXPECT_LE(reads, kDrainSlots + 1) << "领先量卡在可保留的任务边界数上";
  EXPECT_GT(reads, 1u) << "但确实比「一笔一等」快";
}
