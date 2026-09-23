// 进核那一路的数据通路单元：Header Parser。
//
// 对齐飞书《DTE DSA》后，进核是配置驱动，Header Parser 不再生成 Descriptor，只做
// 三件事：把包头上下文（core_mask / Hardware Used / gpu_id / token_id）存进 Header
// Table、逐拍转发 payload、按帧边界判定与合法性检查丢掉非法帧。
//
// 覆盖：包头落 Header Table、payload 一个字节不剥、靠上一帧 TLAST 认帧边界、纯
// 包头帧、非法 Header 只丢那一帧、连续几帧各得一个帧号。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/dte/header_parser.h"
#include "bach/ip/chip/core/dte/hmem.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

MessagePtr MakeMsg(uint64_t user, uint64_t path, uint64_t bytes,
                   uint64_t stream, uint64_t task) {
  auto m = std::make_shared<Message>();
  m->user_id = user;
  m->path_id = path;
  m->size = bytes;
  m->stream_id = stream;
  m->task_id = task;
  m->payload.assign(bytes, 0);
  for (uint64_t i = 0; i < bytes; ++i) m->payload[i] = uint8_t(i & 0xFF);
  return m;
}

// 灌 AXI-Stream 的各拍、读 Header Table、收下游的 payload。
class ParserHarness : public BachModule {
 public:
  struct Beat {
    uint64_t at = 0;
    uint64_t bytes = 0;
    bool last = false;
    MessagePtr msg;
  };

  ParserHarness(ClockPtr c, HeaderParser& target, Hmem& tables)
      : BachModule(c, "harness"), hp(target), hmem(tables) {}

  std::vector<Beat> beats;
  // 这一拍之前下游不收 payload。
  uint64_t payload_from = 0;

  struct Got {
    uint64_t at = 0, frame = 0, bytes = 0, off = 0;
    bool last = false;
    MessagePtr msg;
  };
  std::vector<Got> payloads;
  uint64_t parsed = 0, dropped = 0, beats_seen = 0;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做：先看上一拍摆出来的 payload。
    PayloadPort& p = *hp.PayloadPtr();
    if (p.valid.Get() != 0) {
      uint64_t seq = p.seq.Get();
      if (seq != last_pl_seq) {
        payloads.push_back({now, p.frame.Get(), p.bytes.Get(), p.off.Get(),
                            p.last.Get() != 0, p.msg.Get()});
        last_pl_seq = seq;
      }
    }
    p.ready = now >= payload_from ? 1 : 0;

    Feed(now);
    hp.RunStep();

    parsed = hp.Parsed();
    dropped = hp.Dropped();
    beats_seen = hp.Beats();
  }

 private:
  // Router 那一侧按 AXI-Stream 送：反压时保持不变，看见 ready 才换下一拍。
  void Feed(uint64_t now) {
    CoreDataPort& in = hp.FromRouter();
    if (cursor >= beats.size() || now < beats[cursor].at) {
      if (!driving) in.Idle();
      return;
    }
    if (driving) {
      if (!in.Ready()) return;
      ++cursor;
      driving = false;
      if (cursor >= beats.size() || now < beats[cursor].at) {
        in.Idle();
        return;
      }
    }
    Beat const& b = beats[cursor];
    in.Drive(b.bytes, b.last, cursor == 0, 0, b.msg);
    driving = true;
  }

  HeaderParser& hp;
  Hmem& hmem;
  uint64_t cursor = 0;
  bool driving = false;
  uint64_t last_pl_seq = 0;
};

// 一帧的各拍：头一拍带 256 B，其余各拍补满，最后一拍带 TLAST。
std::vector<ParserHarness::Beat> Frame(uint64_t at, MessagePtr const& m) {
  std::vector<ParserHarness::Beat> out;
  uint64_t n = FlitsOf(m->size);
  uint64_t left = m->size;
  for (uint64_t i = 0; i < n; ++i) {
    uint64_t take = left > kFlitBytes ? kFlitBytes : left;
    out.push_back({at + i, take, i + 1 == n, m});
    left -= take;
  }
  return out;
}

}  // namespace

// 包头上下文落 Header Table：硬件改的 core_mask 与 Hardware Used，加上 DPU 写的
// gpu_id / token_id，按 stream_id 索引。
TEST(BachHeaderParser, HeaderIsStoredToTheHeaderTable) {
  uint64_t mask = 0, used = 0, gpu = 0, token = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Hmem hmem(clk, "hmem", 0, false);
    HeaderParser hp(clk, "hp", hmem, 0, false);
    ParserHarness h(clk, hp, hmem);
    auto m = MakeMsg(41, 7, 512, /*stream=*/5, /*task=*/9);
    m->path_core_mask = 0xF1;
    m->gpu_id = 13;
    m->token_id = 27;
    h.beats = Frame(2, m);
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    HmemEntry const& e = hmem.Entry(5);
    mask = e.core_mask;
    used = e.hardware_used;
    gpu = e.gpu_id;
    token = e.token_id;
  }
  RT::Reset();
  EXPECT_EQ(mask, 0xF1u);
  EXPECT_EQ(used, 1u);
  EXPECT_EQ(gpu, 13u);
  EXPECT_EQ(token, 27u);
}

// payload 一个字节都不剥：一帧四拍都交下去，每一拍的起点按 256 B 递增。
TEST(BachHeaderParser, PayloadIsForwardedBeatByBeat) {
  uint64_t payload_num = 0;
  std::vector<uint64_t> offs, frames;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Hmem hmem(clk, "hmem", 0, false);
    HeaderParser hp(clk, "hp", hmem, 0, false);
    ParserHarness h(clk, hp, hmem);
    h.beats = Frame(2, MakeMsg(41, 7, 1024, 0, 0));
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    payload_num = h.payloads.size();
    for (auto const& p : h.payloads) {
      offs.push_back(p.off);
      frames.push_back(p.frame);
    }
  }
  RT::Reset();
  EXPECT_EQ(payload_num, 4u) << "四拍数据都要交下去";
  ASSERT_EQ(offs.size(), 4u);
  EXPECT_EQ(offs[0], 0u);
  EXPECT_EQ(offs[1], 256u);
  EXPECT_EQ(offs[2], 512u);
  EXPECT_EQ(offs[3], 768u);
  for (auto f : frames) EXPECT_EQ(f, 1u) << "同一帧的几拍同一个帧号";
}

// 靠上一帧的 TLAST 判断下一拍是新 Header：两帧首尾相接，各存一次包头。
TEST(BachHeaderParser, TlastMarksTheFrameBoundary) {
  uint64_t parsed = 0;
  std::vector<uint64_t> frames;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Hmem hmem(clk, "hmem", 0, false);
    HeaderParser hp(clk, "hp", hmem, 0, false);
    ParserHarness h(clk, hp, hmem);
    auto a = Frame(2, MakeMsg(41, 7, 512, 0, 0));
    h.beats = a;
    auto b = Frame(2 + a.size(), MakeMsg(42, 7, 512, 1, 0));
    h.beats.insert(h.beats.end(), b.begin(), b.end());
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    parsed = h.parsed;
    for (auto const& p : h.payloads) frames.push_back(p.frame);
  }
  RT::Reset();
  EXPECT_EQ(parsed, 2u) << "两帧各存一次包头";
  // 帧号一帧一个：第一帧 1，第二帧 2。
  ASSERT_EQ(frames.size(), 4u);
  EXPECT_EQ(frames[0], 1u);
  EXPECT_EQ(frames[2], 2u);
}

// 纯包头帧：byte_count 为 0，Header 这一拍自己带 TLAST，后面那帧照样认得出。
TEST(BachHeaderParser, HeaderOnlyFrameIsOneBeat) {
  uint64_t parsed = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Hmem hmem(clk, "hmem", 0, false);
    HeaderParser hp(clk, "hp", hmem, 0, false);
    ParserHarness h(clk, hp, hmem);
    auto m = MakeMsg(41, 7, 0, 0, 0);
    h.beats = {{2, 0, /*last=*/true, m}};
    auto next = Frame(20, MakeMsg(42, 7, 256, 1, 0));
    h.beats.insert(h.beats.end(), next.begin(), next.end());
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    parsed = h.parsed;
  }
  RT::Reset();
  EXPECT_EQ(parsed, 2u) << "纯包头那一帧收完，下一帧照样认得出";
}

// 非法 Header 丢整帧：不存包头，只消费到 TLAST 恢复帧边界，下一帧照常。
TEST(BachHeaderParser, IllegalHeaderDropsOnlyThatFrame) {
  uint64_t parsed = 0, dropped = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Hmem hmem(clk, "hmem", 0, false);
    HeaderParser hp(clk, "hp", hmem, 0, false);
    ParserHarness h(clk, hp, hmem);
    // 第一帧长度超过上限，要被丢掉。
    auto bad = MakeMsg(41, 7, 64, 0, 0);
    bad->size = kMaxTaskBytes + 1;
    h.beats = {{2, 256, false, bad}, {3, 256, true, bad}};
    auto good = Frame(10, MakeMsg(42, 7, 256, 1, 0));
    h.beats.insert(h.beats.end(), good.begin(), good.end());
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    parsed = h.parsed;
    dropped = h.dropped;
  }
  RT::Reset();
  EXPECT_EQ(parsed, 1u) << "只丢那一帧，通道不卡死";
  EXPECT_EQ(dropped, 1u);
}

// Router 送过来的东西一个字节都不剥：交下去的还是同一个 Message。
TEST(BachHeaderParser, NothingIsStrippedFromTheRouterSide) {
  MessagePtr sent, got;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Hmem hmem(clk, "hmem", 0, false);
    HeaderParser hp(clk, "hp", hmem, 0, false);
    ParserHarness h(clk, hp, hmem);
    sent = MakeMsg(41, 7, 256, 0, 0);
    sent->reduce_seq = 4;
    h.beats = Frame(2, sent);
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    if (!h.payloads.empty()) got = h.payloads.front().msg;
  }
  RT::Reset();
  ASSERT_TRUE(got);
  EXPECT_EQ(got.get(), sent.get()) << "搬的是同一份";
  EXPECT_EQ(got->payload.size(), 256u);
  EXPECT_EQ(got->reduce_seq, 4u);
}

// 连续几帧各得一个帧号，不重：第 N 帧配第 N 个进核任务（FIFO）靠的就是这套编号。
TEST(BachHeaderParser, FramesGetDistinctFrameNumbers) {
  std::vector<uint64_t> first_frames;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Hmem hmem(clk, "hmem", 0, false);
    HeaderParser hp(clk, "hp", hmem, 0, false);
    ParserHarness h(clk, hp, hmem);
    for (uint64_t k = 0; k < 3; ++k) {
      auto one = Frame(2 + k * 10, MakeMsg(41 + k, 7, 256, k, 9));
      h.beats.insert(h.beats.end(), one.begin(), one.end());
    }
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    // 每帧只有一拍，帧号取各自那一拍的号。
    for (auto const& p : h.payloads) {
      if (p.off == 0) first_frames.push_back(p.frame);
    }
  }
  RT::Reset();
  ASSERT_EQ(first_frames.size(), 3u);
  EXPECT_EQ(first_frames[0], 1u);
  EXPECT_EQ(first_frames[1], 2u);
  EXPECT_EQ(first_frames[2], 3u);
}
