// 进核那一路的第一道：Header Parser。
//
// 一帧一任务、靠上一帧的 TLAST 判断下一拍是新 Header、身份取自包头与
// path_task_map、同一条 path 上连着来的几个包靠帧号分开、送进来的数据一个字节
// 都不剥。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/dte/header_parser.h"

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

// 灌 AXI-Stream 的各拍、扮演 Commit 收 Descriptor、扮演下游收 payload。
class ParserHarness : public BachModule {
 public:
  struct Beat {
    uint64_t at = 0;
    uint64_t bytes = 0;
    bool last = false;
    MessagePtr msg;
  };

  ParserHarness(ClockPtr c, HeaderParser& target)
      : BachModule(c, "harness"), hp(target) {}

  std::vector<Beat> beats;
  // 这一拍之前不收 Descriptor，用来把 Header 卡住。
  uint64_t commit_from = 0;
  // 这一拍之前下游不收 payload。
  uint64_t payload_from = 0;

  std::vector<std::shared_ptr<Descriptor>> descs;
  struct Got {
    uint64_t at = 0, frame = 0, bytes = 0, off = 0;
    bool last = false;
    MessagePtr msg;
  };
  std::vector<Got> payloads;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做：先看上一拍摆出来的。
    if (hp.ToCommitPtr()->Valid() && now >= commit_from) {
      auto d = hp.ToCommitPtr()->desc.Get();
      uint64_t seq = hp.ToCommitPtr()->Seq();
      if (d && seq != last_desc_seq) {
        descs.push_back(d);
        last_desc_seq = seq;
      }
    }
    hp.ToCommitPtr()->DriveAccepted(now >= commit_from);

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
  uint64_t cursor = 0;
  bool driving = false;
  uint64_t last_desc_seq = 0, last_pl_seq = 0;
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

// 身份取自包头：stream_id 直接用，task_id 按 path_id 查本地的 path_task_map。
TEST(BachHeaderParser, IdsComeFromTheHeaderAndTheLocalMap) {
  std::shared_ptr<Descriptor> d;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    HeaderParser hp(clk, "hp", 0, false);
    // 本地的 path_task_map：path 7 是本条链上的第 3 步。
    hp.AttachPathTask([](uint64_t path) { return path == 7 ? 3 : 0; });
    ParserHarness h(clk, hp);
    // 包头里带的是发方的编号 9，收方应当按 path 查出 3。
    h.beats = Frame(2, MakeMsg(41, 7, 512, /*stream=*/5, /*task=*/9));
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    if (!h.descs.empty()) d = h.descs.front();
  }
  RT::Reset();
  ASSERT_TRUE(d);
  EXPECT_EQ(d->user_id, 41u);
  EXPECT_EQ(d->path_id, 7u);
  EXPECT_EQ(d->stream_id, 5u) << "stream_id 直接取包头里的";
  EXPECT_EQ(d->task_id, 3u) << "task_id 按 path 查本地的表，不用发方的编号";
  EXPECT_EQ(d->bytes, 512u);
}

// 落点取自包头：发方在出核造包时写进去的那个地址，收方原样用。落 Core Mem 的
// 那一档收方再叠自己的 stream 偏移，落 Matrix Mem 的那一档就是最终地址。
// 地址没对齐到 128 B 的那一帧整帧丢掉。
TEST(BachHeaderParser, LandingAddressComesFromTheHeader) {
  std::shared_ptr<Descriptor> d;
  uint64_t kept = 0, dropped = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    HeaderParser hp(clk, "hp", 0, false);
    ParserHarness h(clk, hp);
    auto m = MakeMsg(41, 7, 512, 5, 0);
    m->dst_addr = 0x2000;
    h.beats = Frame(2, m);
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    if (!h.descs.empty()) d = h.descs.front();
    kept = h.descs.size();
  }
  RT::Reset();
  ASSERT_TRUE(d);
  EXPECT_EQ(kept, 1u);
  EXPECT_EQ(d->dst_addr, 0x2000u);
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    HeaderParser hp(clk, "hp", 0, false);
    ParserHarness h(clk, hp);
    auto m = MakeMsg(41, 7, 512, 5, 0);
    m->dst_addr = 0x2004;   // 没对齐到 128 B
    h.beats = Frame(2, m);
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    dropped = h.descs.size();
  }
  RT::Reset();
  EXPECT_EQ(dropped, 0u) << "地址没对齐的那一帧整帧丢掉，不生成任务";
}

// B core 与 R core 那一档：进核那一笔搬完之后给这个 token 槽位置 valid 标志。
// 第几项按落点除以槽位大小算，写由 Completion RS 在搬完之后发出去。
TEST(BachHeaderParser, InboundSetsTheTokenEntryFlag) {
  std::shared_ptr<Descriptor> d;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    HeaderParser hp(clk, "hp", 0, false);
    hp.SetInboundFlag(/*base=*/0x100, /*entry_bytes=*/0x400);
    ParserHarness h(clk, hp);
    auto m = MakeMsg(41, 7, 512, 5, 0);
    m->dst_addr = 0x1800;   // 第 6 个槽位
    h.beats = Frame(2, m);
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    if (!h.descs.empty()) d = h.descs.front();
  }
  RT::Reset();
  ASSERT_TRUE(d);
  EXPECT_TRUE(d->smem_wr);
  EXPECT_EQ(d->smem_addr, 0x100u + 6 * 4);
  EXPECT_EQ(d->smem_data, 1u);
}

// 不配标志表的 core 上，进核那一笔不写 Share Mem。
TEST(BachHeaderParser, InboundWithoutFlagTableWritesNothing) {
  std::shared_ptr<Descriptor> d;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    HeaderParser hp(clk, "hp", 0, false);
    ParserHarness h(clk, hp);
    h.beats = Frame(2, MakeMsg(41, 7, 512, 5, 0));
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    if (!h.descs.empty()) d = h.descs.front();
  }
  RT::Reset();
  ASSERT_TRUE(d);
  EXPECT_FALSE(d->smem_wr);
}

// 同一条 path 上连着来的几个包，task_id 相同，靠帧号分开。
TEST(BachHeaderParser, FramesOnOnePathGetDistinctSeq) {
  std::vector<uint64_t> frames, tasks;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    HeaderParser hp(clk, "hp", 0, false);
    hp.AttachPathTask([](uint64_t) { return 3; });
    ParserHarness h(clk, hp);
    // 三个包，同一条 path。
    for (uint64_t k = 0; k < 3; ++k) {
      auto one = Frame(2 + k * 10, MakeMsg(41 + k, 7, 256, k, 9));
      h.beats.insert(h.beats.end(), one.begin(), one.end());
    }
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    for (auto const& d : h.descs) {
      frames.push_back(d->frame_seq);
      tasks.push_back(d->task_id);
    }
  }
  RT::Reset();
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_EQ(tasks[0], tasks[1]) << "同一条 path，task_id 本来就一样";
  EXPECT_EQ(tasks[1], tasks[2]);
  EXPECT_NE(frames[0], frames[1]) << "帧号一包一个，不重";
  EXPECT_NE(frames[1], frames[2]);
  EXPECT_NE(frames[0], frames[2]);
}

// 一帧一任务：Header 之后的那几拍都是这一帧的 payload，不会被当成新 Header。
TEST(BachHeaderParser, OneFrameIsOneTask) {
  uint64_t desc_num = 0, payload_num = 0;
  std::vector<uint64_t> offs;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    HeaderParser hp(clk, "hp", 0, false);
    hp.AttachPathTask([](uint64_t) { return 1; });
    ParserHarness h(clk, hp);
    // 一个 1 KB 的包，四拍。
    h.beats = Frame(2, MakeMsg(41, 7, 1024, 0, 0));
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    desc_num = h.descs.size();
    payload_num = h.payloads.size();
    for (auto const& p : h.payloads) offs.push_back(p.off);
  }
  RT::Reset();
  EXPECT_EQ(desc_num, 1u) << "四拍只生成一个任务";
  EXPECT_EQ(payload_num, 4u) << "四拍数据都要交下去";
  ASSERT_EQ(offs.size(), 4u);
  // 每一拍的起点按 256 B 递增，收方按它切出本拍那一段。
  EXPECT_EQ(offs[0], 0u);
  EXPECT_EQ(offs[1], 256u);
  EXPECT_EQ(offs[2], 512u);
  EXPECT_EQ(offs[3], 768u);
}

// 靠上一帧的 TLAST 判断下一拍是新 Header：两帧连着来，第二帧照样解析出任务。
TEST(BachHeaderParser, TlastMarksTheFrameBoundary) {
  uint64_t desc_num = 0;
  std::vector<uint64_t> users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    HeaderParser hp(clk, "hp", 0, false);
    hp.AttachPathTask([](uint64_t) { return 1; });
    ParserHarness h(clk, hp);
    // 两帧首尾相接，中间不留空拍。
    auto a = Frame(2, MakeMsg(41, 7, 512, 0, 0));
    h.beats = a;
    auto b = Frame(2 + a.size(), MakeMsg(42, 7, 512, 1, 0));
    h.beats.insert(h.beats.end(), b.begin(), b.end());
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    desc_num = h.descs.size();
    for (auto const& d : h.descs) users.push_back(d->user_id);
  }
  RT::Reset();
  ASSERT_EQ(desc_num, 2u) << "两帧各一个任务";
  EXPECT_EQ(users[0], 41u);
  EXPECT_EQ(users[1], 42u);
}

// Router 送过来的东西一个字节都不剥：交下去的还是同一个 Message。
TEST(BachHeaderParser, NothingIsStrippedFromTheRouterSide) {
  MessagePtr sent, got;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    HeaderParser hp(clk, "hp", 0, false);
    hp.AttachPathTask([](uint64_t) { return 1; });
    ParserHarness h(clk, hp);
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

// Header 还没被 Commit 收下时不许 Payload Fire，这几拍对 Router 反压。
TEST(BachHeaderParser, PayloadWaitsForTheHeaderToCommit) {
  uint64_t first_payload_at = 0, desc_num = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    HeaderParser hp(clk, "hp", 0, false);
    hp.AttachPathTask([](uint64_t) { return 1; });
    ParserHarness h(clk, hp);
    h.beats = Frame(2, MakeMsg(41, 7, 1024, 0, 0));
    h.commit_from = 20;  // Commit 到第 20 拍才收
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    desc_num = h.descs.size();
    // 第二拍的 payload（off = 256）什么时候才交下去。
    for (auto const& p : h.payloads) {
      if (p.off == 256 && first_payload_at == 0) first_payload_at = p.at;
    }
  }
  RT::Reset();
  EXPECT_EQ(desc_num, 1u);
  EXPECT_GT(first_payload_at, 20u) << "Header 没落地之前后面几拍不许走";
}

// 纯包头任务：byte_count 为 0，Header 这一拍自己带 TLAST，整帧就这一拍。
TEST(BachHeaderParser, HeaderOnlyFrameIsOneBeat) {
  uint64_t desc_num = 0, bytes = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    HeaderParser hp(clk, "hp", 0, false);
    hp.AttachPathTask([](uint64_t) { return 1; });
    ParserHarness h(clk, hp);
    auto m = MakeMsg(41, 7, 0, 0, 0);
    h.beats = {{2, 0, /*last=*/true, m}};
    // 后面再来一帧，验证上一帧确实收尾了。
    auto next = Frame(20, MakeMsg(42, 7, 256, 1, 0));
    h.beats.insert(h.beats.end(), next.begin(), next.end());
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    desc_num = h.descs.size();
    if (!h.descs.empty()) bytes = h.descs.front()->bytes;
  }
  RT::Reset();
  EXPECT_EQ(desc_num, 2u) << "纯包头那一帧收完，下一帧照样认得出";
  EXPECT_EQ(bytes, 0u);
}

// 非法 Header 丢整帧：不生成任务，只消费到 TLAST 恢复帧边界，下一帧照常。
TEST(BachHeaderParser, IllegalHeaderDropsOnlyThatFrame) {
  uint64_t desc_num = 0;
  std::vector<uint64_t> users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    HeaderParser hp(clk, "hp", 0, false);
    hp.AttachPathTask([](uint64_t) { return 1; });
    ParserHarness h(clk, hp);
    // 第一帧长度超过上限，要被丢掉。
    auto bad = MakeMsg(41, 7, 64, 0, 0);
    bad->size = kMaxTaskBytes + 1;
    h.beats = {{2, 256, false, bad}, {3, 256, true, bad}};
    auto good = Frame(10, MakeMsg(42, 7, 256, 1, 0));
    h.beats.insert(h.beats.end(), good.begin(), good.end());
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    desc_num = h.descs.size();
    for (auto const& d : h.descs) users.push_back(d->user_id);
  }
  RT::Reset();
  ASSERT_EQ(desc_num, 1u) << "只丢那一帧，通道不卡死";
  EXPECT_EQ(users[0], 42u);
}
