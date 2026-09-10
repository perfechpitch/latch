// Xbar 的资源与仲裁。
//
// 这一份直接给 Xbar 灌请求，绕开 RouterStation：验的是 VA / SA / ST 三级本身
// 的判断 —— 四个 VC 各占各的 private、多播全有全无、五路无冲突时并行、每个
// 出口独立 RoundRobin、贪婪整包、stream 授权按 user 记。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/router/xbar.h"
#include "bach/ip/wiring.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

// 扮演一个 RouterStation：Xbar 上一拍说收得下就交一笔，交完当场换下一笔。
class ReqFeeder : public BachModule {
 public:
  struct Job {
    uint64_t at = 0;
    uint64_t out_mask = 0;
    uint64_t vc = 0;
    uint64_t user = 0;
    bool head = true, tail = true;
    bool need_stream = false;
    // 这个入口的队列里有没有一整个包。
    bool whole = false;
    // 这一笔在下游要占多少 KB。
    uint64_t require = 0;
  };

  ReqFeeder(ClockPtr c, const std::string& name,
            std::shared_ptr<XbarReqPort> p, std::vector<Job> list)
      : BachModule(c, name), port(std::move(p)), jobs(std::move(list)) {}

  uint64_t granted = 0;
  std::vector<uint64_t> grant_at;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    if (cursor < jobs.size() && jobs[cursor].at <= now && port->Room()) {
      msg = std::make_shared<Message>();
      msg->user_id = jobs[cursor].user;
      msg->path_id = 1;
      msg->size = 256;
      msg->vc = 3;  // 与请求里的 vc 不同，看它有没有被改写
      Drive();
      ++granted;
      grant_at.push_back(now);
      ++cursor;
      return;
    }
    port->IdleReq();
  }

 private:
  void Drive() {
    Job const& j = jobs[cursor];
    port->valid = 1;
    port->out_mask = j.out_mask;
    port->vc = j.vc;
    port->head = j.head ? 1 : 0;
    port->tail = j.tail ? 1 : 0;
    port->bytes = 256;
    port->path_id = 1;
    port->user_id = j.user;
    port->enters_core = 0;
    port->stall_way = 0;
    port->need_stream = j.need_stream ? 1 : 0;
    port->whole_packet = j.whole ? 1 : 0;
    port->credit_require = j.require;
    port->msg = msg;
    port->seq = cursor + 1;
  }

  std::shared_ptr<XbarReqPort> port;
  std::vector<Job> jobs;
  MessagePtr msg;
  uint64_t cursor = 0;
  bool pending = false;
};

// 收一个出口，按需回 vc_release。
class OutTap : public BachModule {
 public:
  OutTap(ClockPtr c, const std::string& name, LinkEndPtr from, LinkEndPtr back,
         bool release_on)
      : BachModule(c, name), src(std::move(from)), rel(std::move(back)),
        on(release_on) {}

  uint64_t got = 0;
  std::vector<uint64_t> at, vcs, users, header_vcs;

 protected:
  void Step() override {
    FlitView f = ReadFlit(src->flit);
    if (f.valid) {
      ++got;
      at.push_back(CycleNow());
      vcs.push_back(f.vc);
      if (f.msg) {
        users.push_back(f.msg->user_id);
        header_vcs.push_back(f.msg->vc);
      }
    }
    rel->flit.Idle();
    if (on && f.valid) {
      rel->release.Drive(true, f.vc, false, 0, false, 0);
    } else {
      rel->release.Idle();
    }
  }

 private:
  LinkEndPtr src, rel;
  bool on;
};

// 一个 Xbar 加它的三个出口。release 默认不回，用例要什么样子自己开。
struct Bench {
  ClockPtr clk;
  std::unique_ptr<Xbar> xb;
  std::vector<LinkEndPtr> out, back;

  explicit Bench(ClockPtr c) : clk(c) {
    xb = std::make_unique<Xbar>(c, "xbar");
    for (uint64_t o = 0; o < kR2RNum; ++o) {
      out.push_back(MakeWire(c));
      back.push_back(MakeWire(c));
      xb->AttachOutLink(o, out[o]);
      xb->AttachBackLink(o, back[o]);
    }
  }

  std::shared_ptr<XbarReqPort> Req(uint64_t in) {
    auto p = std::make_shared<XbarReqPort>(clk);
    xb->AttachReq(in, p);
    return p;
  }
};

std::vector<ReqFeeder::Job> Burst(uint64_t n, uint64_t out_mask, uint64_t vc,
                                  uint64_t user_base, bool need_stream = false) {
  std::vector<ReqFeeder::Job> jobs;
  for (uint64_t i = 0; i < n; ++i) {
    ReqFeeder::Job j;
    j.at = 1;
    j.out_mask = out_mask;
    j.vc = vc;
    j.user = user_base + i;
    j.need_stream = need_stream;
    jobs.push_back(j);
  }
  return jobs;
}

}  // namespace

// 四个 VC 各占各的 private：VC0 把 private 与 shared 都用光之后，VC1 还能发满
// 它自己那 20 个 private。
TEST(BachXbar, OneBlockedVcDoesNotStopOthers) {
  uint64_t vc0 = 0, vc1 = 0, left_priv0 = 0, left_priv1 = 0, shared = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    auto p0 = b.Req(kInLeft);
    auto p1 = b.Req(kInRight);
    // VC0 灌 40 笔：private 20 加 shared 20 正好用光这个方向的全部 credit。
    ReqFeeder f0(clk, "f0", p0, Burst(40, 1ull << kOutMid, 0, 100));
    ReqFeeder f1(clk, "f1", p1, Burst(25, 1ull << kOutMid, 1, 200));
    OutTap tap(clk, "tap", b.out[kOutMid], b.back[kOutMid], false);
    clk->Continue(400 * kPeriod);
    RT::JoinAll();
    vc0 = tap.got;
    vc1 = 0;
    left_priv0 = b.xb->VcCredit(kOutMid, 0);
    left_priv1 = b.xb->VcCredit(kOutMid, 1);
    shared = b.xb->SharedCredit(kOutMid);
  }
  RT::Reset();
  EXPECT_EQ(left_priv0, 0u) << "VC0 的 private 用光";
  EXPECT_EQ(shared, 0u) << "shared 也用光";
  EXPECT_EQ(left_priv1, 0u) << "VC1 用的是自己那 20 个 private";
  EXPECT_EQ(vc0, 60u) << "40 加 20：两个 VC 各自的 private 加共用的 shared";
  EXPECT_EQ(vc1, 0u);
}

// 每个方向一套独立的 credit：把 mid 用光，right 那一路一个都没少。
TEST(BachXbar, CreditIsPerDirection) {
  uint64_t mid_priv = 0, right_priv = 0, right_shared = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    auto p0 = b.Req(kInLeft);
    ReqFeeder f0(clk, "f0", p0, Burst(40, 1ull << kOutMid, 0, 100));
    OutTap tap(clk, "tap", b.out[kOutMid], b.back[kOutMid], false);
    clk->Continue(300 * kPeriod);
    RT::JoinAll();
    mid_priv = b.xb->VcCredit(kOutMid, 0);
    right_priv = b.xb->VcCredit(kOutRight, 0);
    right_shared = b.xb->SharedCredit(kOutRight);
  }
  RT::Reset();
  EXPECT_EQ(mid_priv, 0u);
  EXPECT_EQ(right_priv, kVcPrivateDepth);
  EXPECT_EQ(right_shared, kVcSharedDepth);
}

// 多播全有全无：一个方向的 credit 见底时，另一个方向也不发，它的 credit 不动。
TEST(BachXbar, MulticastIsAllOrNothing) {
  uint64_t both = 0, mid_priv = 0, right_priv = 0, right_shared = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    auto p0 = b.Req(kInLeft);
    auto p1 = b.Req(kInRight);
    // 先把 mid 那一路的 40 个 credit 全用掉。
    ReqFeeder f0(clk, "f0", p0, Burst(40, 1ull << kOutMid, 0, 100));
    // 再发一笔 mid 加 right 的多播，这时候 mid 已经没有 credit 了。
    std::vector<ReqFeeder::Job> mc;
    ReqFeeder::Job j;
    j.at = 200;
    j.out_mask = (1ull << kOutMid) | (1ull << kOutRight);
    j.vc = 0;
    j.user = 900;
    mc.push_back(j);
    ReqFeeder f1(clk, "f1", p1, mc);
    OutTap t0(clk, "t0", b.out[kOutMid], b.back[kOutMid], false);
    OutTap t1(clk, "t1", b.out[kOutRight], b.back[kOutRight], false);
    clk->Continue(400 * kPeriod);
    RT::JoinAll();
    both = t1.got;
    mid_priv = b.xb->VcCredit(kOutMid, 0);
    right_priv = b.xb->VcCredit(kOutRight, 0);
    right_shared = b.xb->SharedCredit(kOutRight);
  }
  RT::Reset();
  EXPECT_EQ(mid_priv, 0u) << "mid 用光了";
  EXPECT_EQ(both, 0u) << "一个方向不够就整笔都不发，另一个方向上一个 flit 都没有";
  EXPECT_EQ(right_priv, kVcPrivateDepth) << "另一个方向的 credit 一个没扣";
  EXPECT_EQ(right_shared, kVcSharedDepth);
}

// 无冲突时并行：两个入口各去各的出口，同一拍都拿到授予。
TEST(BachXbar, NoConflictGrantsInTheSameCycle) {
  std::vector<uint64_t> a, b2;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    auto p0 = b.Req(kInLeft);
    auto p1 = b.Req(kInRight);
    ReqFeeder f0(clk, "f0", p0, Burst(3, 1ull << kOutMid, 0, 100));
    ReqFeeder f1(clk, "f1", p1, Burst(3, 1ull << kOutRight, 0, 200));
    OutTap t0(clk, "t0", b.out[kOutMid], b.back[kOutMid], true);
    OutTap t1(clk, "t1", b.out[kOutRight], b.back[kOutRight], true);
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    a = f0.grant_at;
    b2 = f1.grant_at;
  }
  RT::Reset();
  ASSERT_EQ(a.size(), 3u);
  ASSERT_EQ(b2.size(), 3u);
  for (uint64_t i = 0; i < 3; ++i) {
    EXPECT_EQ(a[i], b2[i]) << "第 " << i << " 笔应当同拍授予";
  }
}

// 争同一个出口时轮流：两个入口都往 mid 发，谁也不会被饿死。
TEST(BachXbar, SameOutputTakesTurns) {
  uint64_t ga = 0, gb = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    auto p0 = b.Req(kInLeft);
    auto p1 = b.Req(kInRight);
    ReqFeeder f0(clk, "f0", p0, Burst(10, 1ull << kOutMid, 0, 100));
    ReqFeeder f1(clk, "f1", p1, Burst(10, 1ull << kOutMid, 0, 200));
    OutTap t0(clk, "t0", b.out[kOutMid], b.back[kOutMid], true);
    clk->Continue(300 * kPeriod);
    RT::JoinAll();
    ga = f0.granted;
    gb = f1.granted;
  }
  RT::Reset();
  EXPECT_EQ(ga, 10u);
  EXPECT_EQ(gb, 10u) << "两个入口都发完，没有谁被饿死";
}

// 贪婪整包：三个入口争同一个出口时，上一包的 body 排在别的入口的新 head 前面。
//
// R2R 的通路允许在 flit 边界切换包，所以别的包会占掉本包两个 flit 之间那一拍
// —— 一次握手要两拍，body 在那一拍还没提出来。要看的是 body 一提出来就被授予，
// 没有被 RoundRobin 推到后面去。
TEST(BachXbar, BodyBeatsAnotherInputsHead) {
  std::vector<uint64_t> at;
  uint64_t others = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    auto p0 = b.Req(kInLeft);
    auto p1 = b.Req(kInRight);
    auto p2 = b.Req(kInMid);
    // f0 是一个三 flit 的包：head、body、tail。
    std::vector<ReqFeeder::Job> pkt;
    for (uint64_t i = 0; i < 3; ++i) {
      ReqFeeder::Job j;
      j.at = 1;
      j.out_mask = 1ull << kOutMid;
      j.vc = 0;
      j.user = 100;
      j.head = (i == 0);
      j.tail = (i == 2);
      pkt.push_back(j);
    }
    ReqFeeder f0(clk, "f0", p0, pkt);
    // 另外两个入口一直在抢同一个出口。
    ReqFeeder f1(clk, "f1", p1, Burst(6, 1ull << kOutMid, 0, 200));
    ReqFeeder f2(clk, "f2", p2, Burst(6, 1ull << kOutMid, 0, 300));
    OutTap tap(clk, "tap", b.out[kOutMid], b.back[kOutMid], true);
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    at = f0.grant_at;
    others = f1.granted + f2.granted;
  }
  RT::Reset();
  ASSERT_EQ(at.size(), 3u);
  EXPECT_EQ(at[1], at[0] + 1) << "一个包的几个 flit 连着走，中间不让别人插";
  EXPECT_EQ(at[2], at[1] + 1);
  EXPECT_GT(others, 0u) << "别的入口确实在抢同一个出口";
}

// stream 授权也按全有全无判：一个方向的表满了，多播的另一个方向也不占坑。
TEST(BachXbar, StreamGrantIsAllOrNothing) {
  uint64_t mid_used = 0, right_used = 0, mc_granted = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    auto p0 = b.Req(kInLeft);
    auto p1 = b.Req(kInRight);
    // 16 个不同的 user 把 mid 方向的 stream 表占满。
    ReqFeeder f0(clk, "f0", p0,
                 Burst(kStreamTabEntries, 1ull << kOutMid, 0, 100, true));
    // 第 17 个 user 要往 mid 与 right 两个方向发。
    std::vector<ReqFeeder::Job> mc;
    ReqFeeder::Job j;
    j.at = 120;
    j.out_mask = (1ull << kOutMid) | (1ull << kOutRight);
    j.vc = 0;
    j.user = 900;
    j.need_stream = true;
    mc.push_back(j);
    ReqFeeder f1(clk, "f1", p1, mc);
    OutTap t0(clk, "t0", b.out[kOutMid], b.back[kOutMid], true);
    OutTap t1(clk, "t1", b.out[kOutRight], b.back[kOutRight], true);
    clk->Continue(300 * kPeriod);
    RT::JoinAll();
    mid_used = b.xb->StreamUsed(kOutMid);
    right_used = b.xb->StreamUsed(kOutRight);
    mc_granted = t1.got;
  }
  RT::Reset();
  EXPECT_EQ(mid_used, kStreamTabEntries) << "mid 的表满了";
  EXPECT_EQ(mc_granted, 0u) << "满的那一侧过不去，另一侧一个 flit 都没出";
  EXPECT_EQ(right_used, 0u) << "另一侧不占坑";
}

// 坑按 user 记：已经持有的方向不再占新坑，只给没持有的那一侧申请。
TEST(BachXbar, HeldDirectionTakesNoNewSlot) {
  uint64_t mid_used = 0, right_used = 0, granted = 0;
  bool holds_mid = false, holds_right = false;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    auto p0 = b.Req(kInLeft);
    std::vector<ReqFeeder::Job> jobs;
    // 同一个 user 先单发 mid，再发 mid 加 right 的多播。
    ReqFeeder::Job a;
    a.at = 1;
    a.out_mask = 1ull << kOutMid;
    a.vc = 0;
    a.user = 55;
    a.need_stream = true;
    jobs.push_back(a);
    ReqFeeder::Job c = a;
    c.at = 10;
    c.out_mask = (1ull << kOutMid) | (1ull << kOutRight);
    jobs.push_back(c);
    ReqFeeder f0(clk, "f0", p0, jobs);
    OutTap t0(clk, "t0", b.out[kOutMid], b.back[kOutMid], true);
    OutTap t1(clk, "t1", b.out[kOutRight], b.back[kOutRight], true);
    clk->Continue(100 * kPeriod);
    RT::JoinAll();
    mid_used = b.xb->StreamUsed(kOutMid);
    right_used = b.xb->StreamUsed(kOutRight);
    holds_mid = b.xb->StreamHolds(kOutMid, 55);
    holds_right = b.xb->StreamHolds(kOutRight, 55);
    granted = f0.granted;
  }
  RT::Reset();
  EXPECT_EQ(granted, 2u);
  EXPECT_EQ(mid_used, 1u) << "同一个 user 第二次发，mid 的余额不变";
  EXPECT_EQ(right_used, 1u) << "没持有的那一侧占一个";
  EXPECT_TRUE(holds_mid);
  EXPECT_TRUE(holds_right);
}

// 换 VC 靠改写包头：本跳把包头里的 VC 改成表里的下一跳 VC，下一跳直接取。
TEST(BachXbar, NextVcIsWrittenIntoTheHeader) {
  std::vector<uint64_t> header_vcs, link_vcs;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    auto p0 = b.Req(kInLeft);
    // 请求里的 vc 是 2，msg 里带进来的是 3。
    ReqFeeder f0(clk, "f0", p0, Burst(1, 1ull << kOutMid, 2, 100));
    OutTap tap(clk, "tap", b.out[kOutMid], b.back[kOutMid], true);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    header_vcs = tap.header_vcs;
    link_vcs = tap.vcs;
  }
  RT::Reset();
  ASSERT_EQ(header_vcs.size(), 1u);
  EXPECT_EQ(header_vcs[0], 2u) << "包头里的 VC 被改写成下一跳的";
  EXPECT_EQ(link_vcs[0], 2u);
}

// 仲裁的第二档：手里攥着一整个包的入口，排在只来了半个包的那些前面。
TEST(BachXbar, WholePacketGoesBeforeAPartialOne) {
  std::vector<uint64_t> users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    auto p0 = b.Req(kInLeft);
    auto p1 = b.Req(kInRight);
    // f0 手里只有半个包（head 还没等到 tail），f1 攥着一整个。
    std::vector<ReqFeeder::Job> partial = Burst(1, 1ull << kOutMid, 0, 100);
    partial[0].head = true;
    partial[0].tail = false;
    partial[0].whole = false;
    std::vector<ReqFeeder::Job> whole = Burst(1, 1ull << kOutMid, 0, 200);
    whole[0].whole = true;
    ReqFeeder f0(clk, "f0", p0, partial);
    ReqFeeder f1(clk, "f1", p1, whole);
    OutTap tap(clk, "tap", b.out[kOutMid], b.back[kOutMid], true);
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    users = tap.users;
  }
  RT::Reset();
  ASSERT_GE(users.size(), 2u);
  EXPECT_EQ(users[0], 200u) << "攥着整包的先走";
  EXPECT_EQ(users[1], 100u);
}

// credit 的单位是 1 KB：一笔广播扣掉的是数据加上提前预留的输出结果空间，
// 这个方向的余额跟着减。
TEST(BachXbar, CoreCreditIsCountedInKilobytes) {
  uint64_t before = 0, after_one = 0, after_same_user = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    auto p0 = b.Req(kInLeft);
    // 同一个用户发两笔，每笔申报 32 KB：8 KB 数据加 24 KB 预留。
    std::vector<ReqFeeder::Job> jobs = Burst(2, 1ull << kOutMid, 0, 55, true);
    jobs[0].user = 55;
    jobs[1].user = 55;
    jobs[0].require = 32;
    jobs[1].require = 32;
    ReqFeeder f0(clk, "f0", p0, jobs);
    OutTap tap(clk, "tap", b.out[kOutMid], b.back[kOutMid], true);
    before = kCoreCreditKb;
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    after_one = b.xb->CoreCredit(kOutMid);
    after_same_user = after_one;
  }
  RT::Reset();
  EXPECT_EQ(before, 128u);
  EXPECT_EQ(after_one, 96u) << "128 减 32；同一个用户的第二笔不再扣";
  EXPECT_EQ(after_same_user, 96u);
}

// 余额不够就不发，也不占坑。
TEST(BachXbar, NotEnoughCreditStopsTheSend) {
  uint64_t got = 0, used = 0, left = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    auto p0 = b.Req(kInLeft);
    b.xb->SetCoreCredit(kOutMid, 16);  // 只剩 16 KB
    std::vector<ReqFeeder::Job> jobs = Burst(1, 1ull << kOutMid, 0, 66, true);
    jobs[0].require = 32;              // 这一笔要 32 KB
    ReqFeeder f0(clk, "f0", p0, jobs);
    OutTap tap(clk, "tap", b.out[kOutMid], b.back[kOutMid], true);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    got = tap.got;
    used = b.xb->StreamUsed(kOutMid);
    left = b.xb->CoreCredit(kOutMid);
  }
  RT::Reset();
  EXPECT_EQ(got, 0u) << "量不够就不发";
  EXPECT_EQ(used, 0u) << "也不占坑";
  EXPECT_EQ(left, 16u) << "余额不动";
}

// 退休把这个用户占的量还回来。
TEST(BachXbar, RetireGivesTheKilobytesBack) {
  uint64_t after_send = 0, after_retire = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    auto p0 = b.Req(kInLeft);
    std::vector<ReqFeeder::Job> jobs = Burst(1, 1ull << kOutMid, 0, 77, true);
    jobs[0].require = 32;
    ReqFeeder f0(clk, "f0", p0, jobs);
    OutTap tap(clk, "tap", b.out[kOutMid], b.back[kOutMid], true);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    after_send = b.xb->CoreCredit(kOutMid);
    b.xb->RetireUser(77);
    after_retire = b.xb->CoreCredit(kOutMid);
  }
  RT::Reset();
  EXPECT_EQ(after_send, 96u);
  EXPECT_EQ(after_retire, 128u) << "退休还回那 32 KB";
}
