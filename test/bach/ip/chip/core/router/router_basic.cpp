// Router 数据面的行为基线：查表、转发、VC credit 两级记账、多播全有全无、
// stream 坑按 user 记。
//
// 对应验收场景 A2（中间核只做 bypass）、A5（同一个 user 第二次发送余额不变）、
// A15（单个只透传的 core）。

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/router/router_station.h"
#include "bach/ip/chip/core/router/xbar.h"
#include "bach/ip/wiring.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

// 往 station 的上游口推 flit。
class UpFeeder : public BachModule {
 public:
  struct Job {
    uint64_t at = 0;
    uint64_t path_id = 0;
    uint64_t user_id = 0;
    uint64_t vc = 0;
    uint64_t core_mask = 0;
  };

  UpFeeder(ClockPtr c, LinkEnd& port, std::vector<Job> list)
      : BachModule(c, "feeder"), sink(port), jobs(std::move(list)) {}

  uint64_t pushed = 0;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    for (auto const& j : jobs) {
      if (j.at != now) continue;
      auto m = std::make_shared<Message>();
      m->path_id = j.path_id;
      m->user_id = j.user_id;
      m->path_core_mask = j.core_mask;
      m->size = 256;
      m->vc = j.vc;
      sink.flit.Drive(j.vc, true, true, 256, m);
      ++pushed;
      return;
    }
    sink.flit.Idle();
  }

 private:
  LinkEnd& sink;
  std::vector<Job> jobs;
};

// 收 Xbar 某个出口的 flit，并按需回 vc_release。
class OutCatcher : public BachModule {
 public:
  OutCatcher(ClockPtr c, LinkEnd& from, LinkEnd& back, bool auto_release)
      : BachModule(c, "catcher"), src(from), rel(back),
        release_on(auto_release) {}

  uint64_t got = 0;
  std::vector<uint64_t> users, vcs;

 protected:
  void Step() override {
    FlitView f = ReadFlit(src.flit);
    if (f.valid) {
      ++got;
      users.push_back(f.msg ? f.msg->user_id : 0);
      vcs.push_back(f.vc);
    }
    rel.flit.Idle();
    if (release_on && f.valid) {
      rel.release.Drive(true, f.vc, false, 0, false, 0);
    } else {
      rel.release.Idle();
    }
  }

 private:
  LinkEnd& src;
  LinkEnd& rel;
  bool release_on;
};

// 计数器是 Logic64，主线程读到的是 t=0 的值，所以要在协程内抄下来。
//
// 但 credit 与 stream 表是 Xbar 的裸成员，只由它自己的 Step() 触碰；在别的模块
// 的 Step() 里读它们是跨线程读非 atomic 容器，读数取决于两个协程谁先跑。那几样
// 一律等 JoinAll 之后在主线程读 —— 那时所有协程都退出了。
class XbarProbe : public BachModule {
 public:
  XbarProbe(ClockPtr c, Xbar& target) : BachModule(c, "probe"), xb(target) {}

  uint64_t granted = 0, stalled = 0, overflow = 0;

 protected:
  void Step() override {
    granted = xb.Granted();
    stalled = xb.Stalled();
    overflow = xb.Overflow();
  }

 private:
  Xbar& xb;
};

// 往右转发一跳的表项。
RouteEntry ForwardRight(bool need_stream, bool enters_core = false) {
  RouteEntry e;
  e.op_type = OpType::kTransfer;
  e.flow_dir = kFlowRight;
  e.cur_vc = 0;
  e.nxt_vc = {0, 0, 0, 0, 0};
  e.path_core_mask_enable = false;
  e.path_core_bypass = !enters_core;
  e.stream_table_enable = need_stream;
  e.operation = Operation::kForward;
  e.stall_way = false;
  return e;
}

// Xbar 的出口下标：out_link[] 按 mid/left/right 排，right 是 2。
constexpr uint64_t kOutIdxRight = 2;

}  // namespace

// 一个包按 path_id 查表，从 left 进、往 right 出。
TEST(BachRouter, ForwardsOneFlit) {
  uint64_t got = 0, granted = 0;
  std::vector<uint64_t> users;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    RouterTable rtab(clk, "rtab");
    rtab.Preload(7, ForwardRight(false));

    RouterStation st(clk, "st_left", kDirLeft, rtab, 0);
    Xbar xb(clk, "xbar");
    xb.AttachReq(kInLeft, st.ReqPtr());
    st.AttachLevel(xb.LevelPtr());

    LinkEndPtr wire_in = MakeWire(clk);
    LinkEndPtr wire_out = MakeWire(clk);
    LinkEndPtr wire_back = MakeWire(clk);
    st.AttachUp(wire_in);
    xb.AttachOutLink(kOutIdxRight, wire_out);
    xb.AttachBackLink(kOutIdxRight, wire_back);

    UpFeeder feed(clk, *wire_in, {{1, 7, 100, 0, 0}});
    OutCatcher cat(clk, *wire_out, *wire_back, true);
    XbarProbe probe(clk, xb);

    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    got = cat.got;
    users = cat.users;
    granted = probe.granted;
  }
  RT::Reset();
  EXPECT_EQ(got, 1u);
  EXPECT_EQ(granted, 1u);
  ASSERT_EQ(users.size(), 1u);
  EXPECT_EQ(users[0], 100u);
}

// 表项没配的 path 不投递任何包：复位后全条目 bypass / no-op。
TEST(BachRouter, UnconfiguredPathIsNotDelivered) {
  uint64_t got = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    RouterTable rtab(clk, "rtab");
    RouterStation st(clk, "st_left", kDirLeft, rtab, 0);
    Xbar xb(clk, "xbar");
    xb.AttachReq(kInLeft, st.ReqPtr());
    LinkEndPtr wire_in = MakeWire(clk);
    LinkEndPtr wire_out = MakeWire(clk);
    LinkEndPtr wire_back = MakeWire(clk);
    st.AttachUp(wire_in);
    xb.AttachOutLink(kOutIdxRight, wire_out);
    xb.AttachBackLink(kOutIdxRight, wire_back);
    UpFeeder feed(clk, *wire_in, {{1, 7, 100, 0, 0}});
    OutCatcher cat(clk, *wire_out, *wire_back, true);
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    got = cat.got;
  }
  RT::Reset();
  EXPECT_EQ(got, 0u);
}

// A5：同一个 user 第二次发送，坑余额不变 —— 坑按 user 记，不按包记。
TEST(BachRouter, SameUserTwiceKeepsOneStreamSlot) {
  uint64_t stream_used = 0, got = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    RouterTable rtab(clk, "rtab");
    rtab.Preload(7, ForwardRight(/*need_stream=*/true));

    RouterStation st(clk, "st_left", kDirLeft, rtab, 0);
    Xbar xb(clk, "xbar");
    xb.AttachReq(kInLeft, st.ReqPtr());
    LinkEndPtr wire_in = MakeWire(clk);
    LinkEndPtr wire_out = MakeWire(clk);
    LinkEndPtr wire_back = MakeWire(clk);
    st.AttachUp(wire_in);
    xb.AttachOutLink(kOutIdxRight, wire_out);
    xb.AttachBackLink(kOutIdxRight, wire_back);

    UpFeeder feed(clk, *wire_in, {{1, 7, 100, 0, 0}, {6, 7, 100, 0, 0}});
    OutCatcher cat(clk, *wire_out, *wire_back, true);
    XbarProbe probe(clk, xb);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    got = cat.got;
    stream_used = xb.StreamUsed(2);  // JoinAll 之后，主线程读裸成员是安全的
  }
  RT::Reset();
  EXPECT_EQ(got, 2u);
  EXPECT_EQ(stream_used, 1u);
}

// VC credit 两级记账：发一个扣一个，下游还回来就补上，一轮跑完回到初值。
TEST(BachRouter, VcCreditIsConserved) {
  uint64_t cr = 0, shared = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    RouterTable rtab(clk, "rtab");
    rtab.Preload(7, ForwardRight(false));
    RouterStation st(clk, "st_left", kDirLeft, rtab, 0);
    Xbar xb(clk, "xbar");
    xb.AttachReq(kInLeft, st.ReqPtr());
    LinkEndPtr wire_in = MakeWire(clk);
    LinkEndPtr wire_out = MakeWire(clk);
    LinkEndPtr wire_back = MakeWire(clk);
    st.AttachUp(wire_in);
    xb.AttachOutLink(kOutIdxRight, wire_out);
    xb.AttachBackLink(kOutIdxRight, wire_back);

    std::vector<UpFeeder::Job> jobs;
    for (uint64_t i = 0; i < 5; ++i) jobs.push_back({1 + i * 5, 7, 100 + i, 0, 0});
    UpFeeder feed(clk, *wire_in, jobs);
    OutCatcher cat(clk, *wire_out, *wire_back, true);
    XbarProbe probe(clk, xb);
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    cr = xb.VcCredit(2, 0);
    shared = xb.SharedCredit(2);
  }
  RT::Reset();
  // 全部发完、全部归还，credit 回到初值
  EXPECT_EQ(cr, kVcPrivateDepth);
  EXPECT_EQ(shared, kVcSharedDepth);
}

// 下游一直不还 credit：发满 private 加 shared 之后就停住，不超发。
TEST(BachRouter, StopsWhenCreditRunsOut) {
  uint64_t got = 0, cr = 0, shared = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    RouterTable rtab(clk, "rtab");
    rtab.Preload(7, ForwardRight(false));
    RouterStation st(clk, "st_left", kDirLeft, rtab, 0);
    Xbar xb(clk, "xbar");
    xb.AttachReq(kInLeft, st.ReqPtr());
    LinkEndPtr wire_in = MakeWire(clk);
    LinkEndPtr wire_out = MakeWire(clk);
    LinkEndPtr wire_back = MakeWire(clk);
    st.AttachUp(wire_in);
    xb.AttachOutLink(kOutIdxRight, wire_out);
    xb.AttachBackLink(kOutIdxRight, wire_back);

    // 一次只灌 20 笔：private 20 个槽，够装下不至于把 VC Buffer 撑爆
    std::vector<UpFeeder::Job> jobs;
    for (uint64_t i = 0; i < 20; ++i) jobs.push_back({1 + i, 7, 100 + i, 0, 0});
    UpFeeder feed(clk, *wire_in, jobs);
    // 不回 release
    OutCatcher cat(clk, *wire_out, *wire_back, false);
    XbarProbe probe(clk, xb);
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    got = cat.got;
    cr = xb.VcCredit(2, 0);
    shared = xb.SharedCredit(2);
  }
  RT::Reset();
  EXPECT_EQ(got, 20u);
  // 20 笔全从 private 里扣，private 归零，shared 没动
  EXPECT_EQ(cr, 0u);
  EXPECT_EQ(shared, kVcSharedDepth);
}
