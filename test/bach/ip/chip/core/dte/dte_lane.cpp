// 通道与地址生成。
//
// 对齐《DTE DSA》后任务按 4 段位走：软件逐段配地址与长度，端点由地址范围译码，
// 展开后的地址由 agcu 算好交给 lane 直接用；scale 段地址随数据、长度按
// bytes/32； 通道内按序激活，读那一半的领先量受 buffer 与 outstanding
// 限额约束；MM → CM 只写 Core Mem；带 scale 的任务 scale 随数据搬。

#include <gtest/gtest.h>

#include <iostream>

#include <memory>
#include <string>
#include <vector>

#include "bach/ip/chip/core/dte/agcu.h"
#include "bach/ip/chip/core/dte/hmem.h"
#include "bach/ip/chip/core/dte/lane.h"
#include "bach/ip/chip/core/memory/core_mem.h"
#include "bach/ip/chip/core/memory/matrix_mem.h"
#include "bach/ip/chip/core/mu/gen_ep_info.h"
#include "base/clock.h"
#include "base/runtime.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

constexpr uint64_t kStride = 0x8000;

// 造一笔任务：段 1 装数据，带 scale 时再补段 2。src/dst 是软件配的段地址，展开
// 成 src_addr/dst_addr 并译码端点走与 regfile Fire 同一条路径。
std::shared_ptr<Descriptor> Task(uint64_t commit_seq, Route route,
                                 uint64_t stream, uint64_t bytes, uint64_t src,
                                 uint64_t dst, bool scale = false) {
  auto d = std::make_shared<Descriptor>();
  d->valid = true;
  d->commit_seq = commit_seq;
  d->route = route;
  d->stream_id = stream;
  // 段地址带端点 tag（与 kernel dte_move 一致）：Mmem 出发的源端打 Mmem
  // tag，scale 段打 scale tag；dst 是收方落点，写纯地址不带 tag。
  uint64_t mm_tag = uint64_t(SegEndpoint::kMmem) << kEpShift;
  uint64_t sc_tag = 0x100000;
  bool from_mm = route == Route::kMmToRouter || route == Route::kMmToCm;
  // 段 0 = 包头（计算 core 落 Hmem）：镜像 regfile Fire 的“段 0 恒参与”，地址打
  // header tag（0x4），端点译码到 kHeader。出核读/写两侧都跳过它，不占
  // payload。
  uint64_t hdr_tag = 0x30000000;
  d->seg[0].valid = false;
  d->seg[0].is_header = true;
  d->seg[0].len = 32;
  d->seg[0].src = hdr_tag | (stream * 32);
  d->seg[0].dst = hdr_tag | (stream * 32);
  d->seg[1].valid = true;
  d->seg[1].src = from_mm ? (src | mm_tag) : src;
  d->seg[1].dst = dst;
  d->seg[1].stride = kStride;
  d->seg[1].len = bytes;
  if (scale) {
    // 段 2 = scale 旁带：地址带 scale tag，低位给对应数据地址，长度按
    // bytes/32。
    d->seg[2].valid = true;
    d->seg[2].src = from_mm ? (0x10000000ull + 32 * 1024 * 1024 + src / 32)
                            : (src / 32 | sc_tag);
    d->seg[2].dst = dst / 32 | sc_tag;
    d->seg[2].stride = kStride / 32;
    d->seg[2].len = bytes / 32;
  }
  Agcu agcu;
  for (uint64_t i = 0; i < 4; ++i) {
    if (d->seg[i].valid)
      agcu.ExpandOut(d->seg[i], stream, route);
  }
  auto m = std::make_shared<Message>();
  m->size = bytes;
  m->payload.assign(bytes, 0);
  m->payload_contains_topk = true;
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

  LaneHarness(ClockPtr c, Lane &target)
      : BachModule(c, "harness"), ln(target), headers(c, "headers", 0, false) {
    ln.AttachHmem(headers);
    ln.AttachHeader(headers.LanePort(0));
  }

  std::vector<Job> jobs;
  // 存储回响应的延迟。
  uint64_t mem_latency = 2;
  // Router 那一路收不收。
  bool router_ready = true;
  uint64_t router_ready_from = 0;

  struct MemOp {
    uint64_t at = 0, addr = 0;
    bool write = false, to_cmem = true;
  };
  std::vector<MemOp> ops;
  uint64_t router_beats = 0, first_router_at = 0;
  std::vector<uint64_t> router_times;
  std::vector<bool> router_heads, router_tails;
  uint64_t peak_outstanding = 0;
  uint64_t next_job = 0;

protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做：先看上一拍摆出来的请求。
    Serve(ln.Cmem(), true, now);
    Serve(ln.Mmem(), false, now);
    auto output = ReadCoreData(ln.ToRouter());
    if (output.valid && output.seq != last_router_seq) {
      last_router_seq = output.seq;
      if (!first_router_at) first_router_at = now;
      ++router_beats;
      router_times.push_back(now);
      router_heads.push_back(output.hdr);
      router_tails.push_back(output.last);
    }
    ln.ToRouter().DriveReady(router_ready && now >= router_ready_from);

    // 扮演 Commit：到点的任务按序送进来，通道报满就等着，有空位了再送。
    bool drove = false;
    if (next_job < jobs.size() && jobs[next_job].at <= now &&
        ln.AdmitPtr()->Ready()) {
      ln.AdmitPtr()->Drive(jobs[next_job].d, ++admit_seq);
      ++next_job;
      drove = true;
    }
    if (!drove)
      ln.AdmitPtr()->Idle();

    headers.RunStep();
    ln.RunStep();
  }

private:
  void Serve(MemPort &p, bool to_cmem, uint64_t now) {
    MemReqView r = ReadMemReq(p);
    if (r.valid && r.seq != last_seq[to_cmem ? 0 : 1]) {
      last_seq[to_cmem ? 0 : 1] = r.seq;
      ops.push_back({now, r.addr, r.we, to_cmem});
      if (!r.we) {
        pend.push_back({now + mem_latency, to_cmem});
        if (pend.size() > peak_outstanding)
          peak_outstanding = pend.size();
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
  Lane &ln;
  Hmem headers;
  std::vector<Pend> pend;
  std::array<uint64_t, 2> last_seq{};
  uint64_t admit_seq = 0;
  uint64_t last_router_seq = 0;
};

} // namespace

namespace {

// 接真存储的驱动台：灌一笔任务、推 Lane、收 Router 那一路。存储自己挂时钟。
class RealMemHarness : public BachModule {
public:
  RealMemHarness(ClockPtr c, Lane &target, std::shared_ptr<Descriptor> job)
      : BachModule(c, "harness"), ln(target), headers(c, "headers", 0, false),
        d(std::move(job)) {
    ln.AttachHmem(headers);
    ln.AttachHeader(headers.LanePort(0));
  }
  std::vector<uint64_t> beat_bytes;
  MessagePtr sent;

protected:
  void Step() override {
    CoreDataView v = ReadCoreData(ln.ToRouter());
    if (v.valid && v.seq != last_seq) {
      last_seq = v.seq;
      beat_bytes.push_back(v.bytes);
      sent = v.msg;
    }
    ln.ToRouter().DriveReady(true);
    if (CycleNow() == 2) {
      ln.AdmitPtr()->Drive(d, 1);
    } else {
      ln.AdmitPtr()->Idle();
    }
    headers.RunStep();
    ln.RunStep();
  }

private:
  Lane &ln;
  Hmem headers;
  std::shared_ptr<Descriptor> d;
  uint64_t last_seq = 0;
};

} // namespace

// 出核带 scale：先按拍读完数据，再用只读 scale 的一笔把 scale 读回来接在包尾。
// 末拍只有 scale 那几个字节，不按整拍记。
TEST(BachDteLane, OutboundScaleFollowsTheData) {
  constexpr uint64_t kData = 512;
  std::vector<uint8_t> data(kData), scale(kData / 32);
  for (uint64_t i = 0; i < kData; ++i)
    data[i] = uint8_t(i * 3 + 5);
  for (uint64_t i = 0; i < scale.size(); ++i)
    scale[i] = uint8_t(100 + i);
  std::vector<uint64_t> beats;
  std::vector<uint8_t> payload;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteBuffer buf(clk, "buf", 64, 1, 0, false);
    Lane ln(clk, "lane", kOutCh0, buf, 0, false);
    CoreMem cmem(clk, "cmem");
    ln.AttachCmem(cmem.PortPtr(kCmemDteRd));
    uint64_t base = 0x100;
    cmem.Poke(base, data);
    cmem.PokeScale(base, scale);

    auto d = Task(1, Route::kCmToRouter, 0, kData, 0x100, 0, /*scale=*/true);
    d->msg->size = kData + scale.size();
    d->msg->scale_valid = 1;
    d->msg->payload.assign(d->msg->size, 0);
    RealMemHarness h(clk, ln, d);
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    beats = h.beat_bytes;
    if (h.sent)
      payload = h.sent->payload;
  }
  RT::Reset();
  ASSERT_EQ(beats.size(), 3u) << "两拍数据加一拍 scale";
  EXPECT_EQ(beats[2], scale.size());
  std::vector<uint8_t> want = data;
  want.insert(want.end(), scale.begin(), scale.end());
  EXPECT_EQ(payload, want);
}

// MM → CM 带 scale：数据与 scale 都从 Matrix Mem 读出来，落到 Core Mem 同样
// 的相对位置上。
TEST(BachDteLane, MatrixToCoreCarriesScale) {
  constexpr uint64_t kData = 768;
  std::vector<uint8_t> data(kData), scale(kData / 32);
  for (uint64_t i = 0; i < kData; ++i)
    data[i] = uint8_t(i * 5 + 2);
  for (uint64_t i = 0; i < scale.size(); ++i)
    scale[i] = uint8_t(90 + i);
  std::vector<uint8_t> got_data, got_scale;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteBuffer buf(clk, "buf", 64, 1, 0, false);
    Lane ln(clk, "lane", kInnerLane, buf, 0, false);
    CoreMem cmem(clk, "cmem");
    MatrixMem mmem(clk, "mmem");
    ln.AttachCmem(cmem.PortPtr(kCmemDteWr));
    ln.AttachMmem(mmem.PortPtr(kMmemDteRd));
    mmem.Poke(0x9000, data);
    mmem.Poke(32 * 1024 * 1024 + 0x9000 / 32, scale);

    auto d = Task(1, Route::kMmToCm, 1, kData, 0x9000, 0x40, /*scale=*/true);
    RealMemHarness h(clk, ln, d);
    clk->Continue(300 * kPeriod);
    RT::JoinAll();
    uint64_t at = kStride + 0x40;
    got_data = cmem.Peek(at, kData);
    got_scale = cmem.PeekScale(at, scale.size());
  }
  RT::Reset();
  EXPECT_EQ(got_data, data);
  EXPECT_EQ(got_scale, scale);
}

// 端点由地址高 4 bit tag 译码决定，与 route 无关：Cmem 是 0（默认），Mmem /
// scale / topK / header 各占一个 tag。
TEST(BachAgcu, EndpointIsDecodedFromTheAddressRange) {
  EXPECT_EQ(Agcu::Decode(0x00001000ull), SegEndpoint::kCmem);
  EXPECT_EQ(Agcu::Decode(0x10001000ull), SegEndpoint::kMmem);
  EXPECT_EQ(Agcu::Decode(0x00101000ull), SegEndpoint::kScale);
  EXPECT_EQ(Agcu::Decode(0x20000100ull), SegEndpoint::kTopk);
  EXPECT_EQ(Agcu::Decode(0x30000100ull), SegEndpoint::kHeader);
}

// 段地址逐段展开：stream_start = CfgAddr + SID × stride。Cmem 一侧按段地址与
// 偏移展开，Mmem 一侧给物理地址不叠；MM→CM 源端不叠 stride。
TEST(BachAgcu, ExpandOutExpandsPerSegmentAddress) {
  Agcu a;
  uint64_t mm_tag = uint64_t(SegEndpoint::kMmem) << kEpShift;

  Segment data;
  data.src = 0x40;
  data.dst = 0x80;
  data.stride = kStride;
  // CmToRouter：源端是 Cmem，叠 SID × stride；目的端是 Router，无地址。
  a.ExpandOut(data, 3, Route::kCmToRouter);
  EXPECT_EQ(data.src_kind, SegEndpoint::kCmem);
  EXPECT_EQ(data.src_addr, 3 * 0x8000 + 0x40);
  EXPECT_EQ(data.dst_addr, 0u);

  Segment m2r;
  m2r.src = 0x9000u | mm_tag; // Mmem 出发，源端带 Mmem tag，stride 配 0
  m2r.dst = 0;
  m2r.stride = 0;
  a.ExpandOut(m2r, 2, Route::kMmToRouter);
  EXPECT_EQ(m2r.src_kind, SegEndpoint::kMmem);
  EXPECT_EQ(m2r.src_addr, 0x9000u);

  Segment m2c;
  m2c.src = 0x9000u | mm_tag;
  m2c.dst = 0x40;
  m2c.stride = kStride;
  a.ExpandOut(m2c, 1, Route::kMmToCm);
  EXPECT_EQ(m2c.src_addr, 0x9000u) << "MM→CM 源端不叠 stride";
  EXPECT_EQ(m2c.dst_kind, SegEndpoint::kCmem);
  EXPECT_EQ(m2c.dst_addr, 0x8000 + 0x40);
}

// 出核读那一半的领先量卡在 outstanding 限额上，不会无限发请求。
TEST(BachDteLane, ReadAheadStopsAtTheOutstandingLimit) {
  uint64_t peak = 0, reads = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteBuffer buf(clk, "buf", 64, 1, 0, false);
    // 出核通道，进核那个走的是另一条路。
    Lane ln(clk, "lane", kOutCh0, buf, 0, false);
    LaneHarness h(clk, ln);
    // 存储很慢，读请求会攒着。
    h.mem_latency = 20;
    h.jobs = {{2, Task(1, Route::kCmToRouter, 0, 8 * kFlitBytes, 0, 0)}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    peak = h.peak_outstanding;
    for (auto const &o : h.ops) {
      if (!o.write)
        ++reads;
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
    Lane ln(clk, "lane", kOutCh0, buf, 0, false);
    LaneHarness h(clk, ln);
    // 两笔出核任务，源地址不同：第一笔从 0 起，第二笔从 0x100 起。
    h.jobs = {{2, Task(1, Route::kCmToRouter, 0, kFlitBytes, 0, 0)},
              {4, Task(2, Route::kCmToRouter, 0, 2 * kFlitBytes, 0x100, 0)}};
    clk->Continue(120 * kPeriod);
    RT::JoinAll();
    for (auto const &o : h.ops) {
      if (!o.write)
        addrs.push_back(o.addr);
    }
  }
  RT::Reset();
  ASSERT_GE(addrs.size(), 2u);
  EXPECT_EQ(addrs[0], 0u) << "第一笔的源地址";
  EXPECT_EQ(addrs[1], 0x100u) << "第二笔排在它后面";
}

// MM → CM 那一档只写 Core Mem：出核通道的数据不会被写回 Matrix Mem。
TEST(BachDteLane, MatrixToCoreWritesOnlyCoreMem) {
  uint64_t cmem_writes = 0, mmem_writes = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteBuffer buf(clk, "buf", 64, 1, 0, false);
    // 走 out_ch[3]，MM → CM 固定复用它。
    Lane ln(clk, "lane", kInnerLane, buf, 0, false);
    LaneHarness h(clk, ln);
    h.jobs = {{2, Task(1, Route::kMmToCm, 1, kFlitBytes, 0x9000, 0x40)}};
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    for (auto const &o : h.ops) {
      if (!o.write)
        continue;
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
  constexpr uint64_t kBytes = 0xC200; // 49664 B，合 194 个 beat
  uint64_t writes = 0;
  std::vector<uint64_t> addrs;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteBuffer buf(clk, "buf", kDteBufFlits, 1, 0, false);
    Lane ln(clk, "lane", kInnerLane, buf, 0, false);
    LaneHarness h(clk, ln);
    h.jobs = {{2, Task(1, Route::kMmToCm, 0, kBytes, 0x9000, 0)}};
    clk->Continue(8000 * kPeriod);
    RT::JoinAll();
    for (auto const &o : h.ops) {
      if (!o.write || !o.to_cmem)
        continue;
      ++writes;
      addrs.push_back(o.addr);
    }
  }
  RT::Reset();
  uint64_t want = (kBytes + kFlitBytes - 1) / kFlitBytes;
  EXPECT_EQ(writes, want) << "每个 beat 都要写出去";
  ASSERT_FALSE(addrs.empty());
  EXPECT_EQ(addrs.front(), 0u) << "从段首写起";
  EXPECT_EQ(addrs.back(), (want - 1) * kFlitBytes) << "写到段尾";
}

// 出核从 Matrix Mem 读时不叠 stream 偏移，配的地址就是最终地址。
TEST(BachDteLane, MatrixSideAddressIsUsedAsIs) {
  uint64_t first_read = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteBuffer buf(clk, "buf", 64, 1, 0, false);
    Lane ln(clk, "lane", kInnerLane, buf, 0, false);
    LaneHarness h(clk, ln);
    h.jobs = {
        {2, Task(1, Route::kMmToCm, /*stream=*/2, kFlitBytes, 0x9000, 0x40)}};
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    for (auto const &o : h.ops) {
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
    Lane ln(clk, "lane", kOutCh0, buf, 0, false);
    LaneHarness h(clk, ln);
    // 存储很慢：第一笔的响应要 30 拍才回来。
    h.mem_latency = 30;
    h.jobs = {{2, Task(1, Route::kCmToRouter, 0, kFlitBytes, 0, 0)},
              {4, Task(2, Route::kCmToRouter, 0, 2 * kFlitBytes, 0x100, 0)}};
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    for (auto const &o : h.ops) {
      if (!o.write)
        read_at.push_back(o.at);
    }
  }
  RT::Reset();
  ASSERT_GE(read_at.size(), 2u) << "两笔的读都要发出去";
  EXPECT_LT(read_at[1] - read_at[0], 30u) << "第二笔不该等第一笔的响应回来才发";
}

// 前一笔的读请求发完、响应还没收齐时，后一笔的读也要把它们算进 buffer 的占用：
// Router 那一路不收，buffer 只有 4 拍，两笔各 4 拍，在途的读请求加起来不超过
// 4 个，后一笔一个都发不出去。
TEST(BachDteLane, ReadAheadCountsThePreviousTasksResponses) {
  uint64_t peak = 0, reads = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteBuffer buf(clk, "buf", 4, 1, 0, false);
    Lane ln(clk, "lane", kOutCh0, buf, 0, false);
    LaneHarness h(clk, ln);
    h.mem_latency = 5;
    h.router_ready = false;
    h.jobs = {{2, Task(1, Route::kCmToRouter, 0, 4 * kFlitBytes, 0, 0)},
              {3, Task(2, Route::kCmToRouter, 0, 4 * kFlitBytes, 0x400, 0)}};
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    peak = h.peak_outstanding;
    for (auto const &o : h.ops) {
      if (!o.write)
        ++reads;
    }
  }
  RT::Reset();
  EXPECT_LE(peak, 4u) << "在途的读请求不超过 buffer 的位置";
  EXPECT_EQ(reads, 5u) << "buffer 被前一笔占满，后一笔发不出读请求";
}

// 等收敛的任务边界数有上限：满了就先不激活新的。
TEST(BachDteLane, DrainSlotsBoundTheReadAhead) {
  uint64_t reads = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteBuffer buf(clk, "buf", 64, 1, 0, false);
    Lane ln(clk, "lane", kOutCh0, buf, 0, false);
    LaneHarness h(clk, ln);
    // 响应一直不回来，几笔任务会堆在等收敛的那一队里。
    h.mem_latency = 100000;
    for (uint64_t i = 0; i < 6; ++i) {
      h.jobs.push_back({2 + i * 2, Task(1 + i, Route::kCmToRouter, 0,
                                        kFlitBytes, i * 0x100, 0)});
    }
    clk->Continue(120 * kPeriod);
    RT::JoinAll();
    for (auto const &o : h.ops) {
      if (!o.write)
        ++reads;
    }
  }
  RT::Reset();
  // 一笔在发、kDrainSlots 笔在等，再多就不激活了。
  EXPECT_LE(reads, kDrainSlots + 1) << "领先量卡在可保留的任务边界数上";
  EXPECT_EQ(reads, 1u) << "但确实比“一笔一等”快";
}

// 进核那一笔带 topK 段：topK 与数据/scale 同一条数据通道，作为数据之后的一拍进
// 来，落地时按 topK 段的端内偏移（表下标）把包里的 topK 写进 MU 端口，整笔只写
// 一次。只有进核通道挂着这条数据线。topK 随包走，进核 Descriptor 的 msg 是空，
// 得从 Header Parser 转过来的 payload 那一拍的 msg 里读。
TEST(BachDteLane, InboundTopkUsesTwo128ByteWrites) {
  EnsureSlots();
  auto clk = MakeClock(0, kPeriod);
  DteBuffer buf(clk, "buf", 32, 1, 0, false);
  Lane lane(clk, "lane", kInCh, buf, 0, false);
  CoreMem cmem(clk, "cmem", 0, false);
  GenEpInfo topk(clk, "topk", 0, false);
  Hmem headers(clk, "headers", 0, false);
  lane.AttachCmem(cmem.PortPtr(kCmemDteWr));
  lane.AttachHeader(headers.LanePort(0));
  lane.AttachHmem(headers);
  lane.AttachTopk(topk.DteWritePtr());
  auto payload = std::make_shared<PayloadPort>(clk);
  lane.AttachPayload(payload);
  auto d = Task(1, Route::kRouterToCm, 3, 256, 0, 0x40);
  d->seg[0].valid = true;
  d->seg[3].valid = true;
  d->seg[3].dst_kind = SegEndpoint::kTopk;
  d->seg[3].dst_addr = 3 * 256;
  d->seg[3].len = 256;
  d->msg->payload.resize(512);
  d->msg->size = 512;
  ByteBlock expected(256);
  for (uint64_t k = 0; k < 256; ++k)
    expected[k] = d->msg->payload[256 + k] = uint8_t(k * 3 + 7);
  struct Harness : BachModule {
    Lane &lane;
    CoreMem &memory;
    GenEpInfo &topk;
    Hmem &headers;
    std::shared_ptr<PayloadPort> payload;
    std::shared_ptr<Descriptor> desc;
    uint64_t beats = 0, writes = 0, last_seq = 0;
    Harness(ClockPtr c, Lane &l, CoreMem &m, GenEpInfo &t, Hmem &h,
            std::shared_ptr<PayloadPort> p, std::shared_ptr<Descriptor> d)
        : BachModule(c, "driver"), lane(l), memory(m), topk(t), headers(h),
          payload(p), desc(d) {}
    void Step() override {
      auto req = ReadMemReq(*topk.DteWritePtr());
      if (req.valid && req.seq != last_seq) {
        last_seq = req.seq;
        ++writes;
        EXPECT_EQ(req.bytes, 128u);
      }
      if (CycleNow() == 2)
        lane.AdmitPtr()->Drive(desc, 1);
      else
        lane.AdmitPtr()->Idle();
      if (CycleNow() > 4 && beats < 2 && payload->Ready()) {
        payload->Drive(1, 256, beats * 256, beats == 1, desc->msg);
        ++beats;
      } else
        payload->Idle();
      memory.RunStep();
      topk.RunStep();
      headers.RunStep();
      lane.RunStep();
    }
  } driver(clk, lane, cmem, topk, headers, payload, d);
  clk->Continue(200);
  RT::JoinAll();
  EXPECT_EQ(driver.writes, 2u);
  EXPECT_EQ(topk.TopkBytes(3), expected);
  EXPECT_TRUE(lane.Quiescent());
  RT::Reset();
}

TEST(BachAgcu, WindowBoundsAlignmentAndArithmeticAreCheckedBeforeFire) {
  Agcu agcu;
  Descriptor d;
  d.route = Route::kCmToRouter;
  d.stream_id = 15;
  d.seg[0].valid = d.seg[0].is_header = true;
  d.seg[0].len = 32;
  d.seg[0].src = 0x30000000;
  d.seg[0].stride = 32;
  d.seg[1].valid = true;
  d.seg[1].src = 0xffff8;
  d.seg[1].len = 8;
  EXPECT_EQ(agcu.Validate(d), 0u);
  d.seg[1].len = 16;
  EXPECT_EQ(agcu.Validate(d), kDteWindowError);
  d.seg[1].src = 0x100000;
  d.seg[1].len = 8;
  EXPECT_EQ(agcu.Validate(d), 0u);
  d.seg[1].src = 0x108000;
  EXPECT_EQ(agcu.Validate(d), kDteWindowError);
  d.seg[1].src = 0x20000000;
  d.seg[1].len = 256;
  EXPECT_EQ(agcu.Validate(d), 0u);
  d.seg[1].src += 128;
  EXPECT_EQ(agcu.Validate(d), kDteAlignmentError);
  d.seg[1].src = 0x10000000;
  d.seg[1].stride = 0xffffffff;
  EXPECT_EQ(agcu.Validate(d), kDteOverflowError);
  d.seg[1].valid = false;
  EXPECT_EQ(agcu.Validate(d), 0u);
  d.seg[0].src = 0x300001ff;
  d.seg[0].stride = 0;
  EXPECT_EQ(agcu.Validate(d), 0u);
  d.seg[0].src++;
  EXPECT_EQ(agcu.Validate(d), kDteEndpointError);
  DteWindows windows;
  windows.mm_base = 0x50000000;
  Agcu shifted(windows);
  EXPECT_EQ(shifted.Endpoint(0x50000000), SegEndpoint::kMmem);
  EXPECT_EQ(shifted.Endpoint(0x10000000), SegEndpoint::kInvalid);
}


class MultiEndpointResponseHarness : public BachModule {
public:
  MultiEndpointResponseHarness(ClockPtr clock, Lane &lane, DteBuffer &buffer)
      : BachModule(clock, "driver"), target(lane), buf(buffer) {}
  uint64_t max_completed_slots = 0;
  uint64_t completed_slots = 0, output_bytes = 0;
  MessagePtr result;
protected:
  void Step() override {
    target.Cmem().DriveSlave(true, CycleNow() == 14,
        std::make_shared<ByteBlock>(256, 0x11));
    target.Mmem().DriveSlave(true, CycleNow() == 14,
        std::make_shared<ByteBlock>(256, 0x22));
    auto output = ReadCoreData(target.ToRouter());
    if (output.valid && output.seq != last_output) {
      last_output = output.seq;
      output_bytes += output.bytes;
      result = output.msg;
    }
    target.ToRouter().DriveReady(CycleNow() >= 20);
    if (CycleNow() == 2) {
      auto d = Task(1, Route::kCmToRouter, 0, 256, 0x2000, 0);
      d->seg[2].valid = true;
      d->seg[2].len = 256;
      d->seg[2].src_kind = SegEndpoint::kMmem;
      d->seg[2].src_addr = 0x2000;
      d->msg->size = 512;
      d->msg->payload.resize(512);
      target.AdmitPtr()->Drive(d, 1);
    } else target.AdmitPtr()->Idle();
    auto before = buf.Reserved(kOutCh0);
    target.RunStep();
    auto after = buf.Reserved(kOutCh0);
    if (before > after) {
      max_completed_slots = std::max(max_completed_slots, before - after);
      completed_slots += before - after;
    }
  }
private:
  Lane &target;
  DteBuffer &buf;
  uint64_t last_output = 0;
};
TEST(BachDteLane, SingleWriteBufferSerializesDifferentEndpointResponses) {
  EnsureSlots();
  auto clock = MakeClock(0, kPeriod);
  DteBuffer buffer(clock, "buf", kDteBufFlits, 1, 0, false);
  Lane lane(clock, "lane", kOutCh0, buffer, 0, false);
  MultiEndpointResponseHarness driver(clock, lane, buffer);
  clock->Continue(50);
  RT::JoinAll();
  EXPECT_EQ(driver.max_completed_slots, 1u);
  EXPECT_EQ(driver.completed_slots, 2u);
  EXPECT_EQ(driver.output_bytes, 512u);
  ASSERT_TRUE(driver.result);
  ByteBlock expected(256, 0x11);
  expected.insert(expected.end(), 256, 0x22);
  EXPECT_EQ(driver.result->payload, expected);
  EXPECT_TRUE(lane.Quiescent());
  RT::Reset();
}

TEST(BachDteLane, ChannelThreeOutputStageHoldsFlitsWithoutDuplicatingThem) {
  EnsureSlots();
  auto clock = MakeClock(0, kPeriod);
  DteBuffer buffer(clock, "buf", kDteBufFlits, 1, 0, false);
  Lane lane(clock, "lane3", kInnerLane, buffer, 0, false);
  LaneHarness driver(clock, lane);
  driver.router_ready_from = 15;
  auto task = Task(1, Route::kCmToRouter, 0, 1024, 0x2000, 0);
  task->vc = 3;
  driver.jobs = {{2, task}};
  clock->Continue(60);
  RT::JoinAll();
  ASSERT_EQ(driver.router_beats, 4u);
  EXPECT_GE(driver.first_router_at, 15u);
  EXPECT_TRUE(driver.router_heads.front());
  EXPECT_TRUE(driver.router_tails.back());
  for (uint64_t k = 1; k < driver.router_times.size(); ++k) {
    EXPECT_EQ(driver.router_times[k], driver.router_times[k - 1] + 1);
    EXPECT_FALSE(driver.router_heads[k]);
    EXPECT_FALSE(driver.router_tails[k - 1]);
  }
  EXPECT_TRUE(lane.Quiescent());
  RT::Reset();
}
TEST(BachDteLane, ChannelThreeNetworkPathAddsOneCycle) {
  EnsureSlots();
  auto clock = MakeClock(0, kPeriod);
  DteBuffer buffer(clock, "buf", 4 * kDteBufFlits, 4, 0, false);
  Lane first(clock, "lane0", kOutCh0, buffer, 0, false);
  Lane fourth(clock, "lane3", kOutCh0 + 3, buffer, 0, false);
  LaneHarness first_driver(clock, first);
  LaneHarness fourth_driver(clock, fourth);
  auto a = Task(1, Route::kCmToRouter, 0, 256, 0x2000, 0);
  auto b = Task(2, Route::kCmToRouter, 0, 256, 0x2000, 0);
  for (auto d : {a, b}) {
    d->seg[0].valid = true;
    d->seg[0].src_kind = SegEndpoint::kHeader;
    d->seg[0].src_addr = 0;
  }
  b->vc = 3;
  first_driver.jobs = {{2, a}};
  fourth_driver.jobs = {{2, b}};
  clock->Continue(40);
  RT::JoinAll();
  ASSERT_GT(first_driver.first_router_at, 0u);
  EXPECT_EQ(fourth_driver.first_router_at, first_driver.first_router_at + 1);
  RT::Reset();
}
