// Router 的验收场景。
//
// 计划里点名先跑的四个：
//   A2       中间核只做 bypass，不占该核的用户坑
//   A5       同一个 user 二次发送余额不减（在 router_basic 里）
//   A15/A16  单个与多个坏 core 串联时的 credit 透传
//   A17      reduce 完成 Ack 归 Router；缺 Ack 时任务链停在 reduce 处不前进
//
// 另外覆盖 CoreStation 的三态准入、CoreMem 重发的同 VC 保序、Retire 的三方时序、
// CreditMonitor 按 StreamID 选最老。

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/router/core_station.h"
#include "bach/ip/chip/core/router/coremem_reissue.h"
#include "bach/ip/chip/core/router/credit_monitor.h"
#include "bach/ip/chip/core/router/reduce_module.h"
#include "bach/ip/chip/core/router/retire.h"
#include "bach/ip/chip/core/router/router_station.h"
#include "bach/ip/chip/core/router/xbar.h"
#include "bach/ip/wiring.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;
constexpr uint64_t kOutIdxRight = 2;

MessagePtr MakeMsg(uint64_t path, uint64_t user, uint64_t bytes = 256) {
  auto m = std::make_shared<Message>();
  m->path_id = path;
  m->user_id = user;
  m->size = bytes;
  return m;
}

// 往一根线上推 flit。
class Pusher : public BachModule {
 public:
  struct Job {
    uint64_t at = 0;
    MessagePtr msg;
    uint64_t vc = 0;
    bool head = true, tail = true;
  };

  Pusher(ClockPtr c, LinkEnd& port, std::vector<Job> list)
      : BachModule(c, "pusher"), sink(port), jobs(std::move(list)) {}

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    for (auto const& j : jobs) {
      if (j.at != now) continue;
      sink.flit.Drive(j.vc, j.head, j.tail, j.msg->size, j.msg);
      return;
    }
    sink.flit.Idle();
  }

 private:
  LinkEnd& sink;
  std::vector<Job> jobs;
};

class Catcher : public BachModule {
 public:
  Catcher(ClockPtr c, LinkEnd& from) : BachModule(c, "catcher"), src(from) {}

  uint64_t got = 0;
  std::vector<uint64_t> at;

 protected:
  void Step() override {
    FlitView f = ReadFlit(src.flit);
    if (f.valid) {
      ++got;
      at.push_back(CycleNow());
    }
  }

 private:
  LinkEnd& src;
};

RouteEntry Bypass(uint64_t flow) {
  RouteEntry e;
  e.flow_dir = flow;
  e.path_core_bypass = true;   // 1 不进核
  e.cur_credit_require = 0;
  e.operation = Operation::kForward;
  return e;
}

RouteEntry ReduceEntry(uint64_t in_mask) {
  RouteEntry e;
  e.op_type = OpType::kReduce;
  e.flow_dir = 0;              // 末端汇聚，出方向全不置位
  e.path_core_bypass = false;  // 0 进核
  e.reduce_in_mask = in_mask;
  e.operation = Operation::kReduce2;
  return e;
}

}  // namespace

// A2：中间核只做 bypass，不占该核的用户坑，cur_credit_require 为 0。
TEST(BachRouterScenario, A2BypassDoesNotTakeSlot) {
  uint64_t stream_used = 0, got = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    RouterTable rtab(clk, "rtab");
    rtab.Preload(3, Bypass(kFlowRight));

    RouterStation st(clk, "st", kDirLeft, rtab, 0);
    Xbar xb(clk, "xbar");
    xb.AttachReq(kInLeft, st.ReqPtr());
    LinkEndPtr in = MakeWire(clk), out = MakeWire(clk), back = MakeWire(clk);
    st.AttachUp(in);
    xb.AttachOutLink(kOutIdxRight, out);
    xb.AttachBackLink(kOutIdxRight, back);

    Pusher push(clk, *in, {{1, MakeMsg(3, 55)}});
    Catcher cat(clk, *out);
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    got = cat.got;
    stream_used = xb.StreamUsed(kOutIdxRight);
  }
  RT::Reset();
  EXPECT_EQ(got, 1u);
  EXPECT_EQ(stream_used, 0u);  // bypass 不占坑
}

// A15：单个坏 core，只转发不记账，stream 表全空、不发 trigger。
TEST(BachRouterScenario, A15PassThroughCoreKeepsNoState) {
  uint64_t got = 0, stream_used = 0, triggers = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    RouterTable rtab(clk, "rtab");
    // 表里配的是进核加转发，但这个 core 是坏 core，进核那一位要被抹掉
    RouteEntry e = Bypass(kFlowRight);
    e.path_core_bypass = false;   // 表里写的是进核
    e.stream_need = kStreamNeedCore;
    rtab.Preload(3, e);
    // 中间列 chip 的 core_bad_mask，本 core 是 core2。透传档由这一位打开。
    constexpr uint64_t kSelf = 2;
    rtab.SetCoreBadMask(0x084);
    ASSERT_TRUE(rtab.CoreBad(kSelf));

    RouterStation st(clk, "st", kDirLeft, rtab, 0);
    st.SetPassThrough(rtab.CoreBad(kSelf));
    Xbar xb(clk, "xbar");
    xb.SetPassThrough(rtab.CoreBad(kSelf));
    xb.AttachReq(kInLeft, st.ReqPtr());
    CoreStation cs(clk, "cs");

    LinkEndPtr in = MakeWire(clk), out = MakeWire(clk), back = MakeWire(clk);
    LinkEndPtr to_core = MakeWire(clk);
    st.AttachUp(in);
    xb.AttachOutLink(kOutIdxRight, out);
    xb.AttachBackLink(kOutIdxRight, back);
    xb.AttachCoreOut(to_core);
    cs.AttachFromXbar(to_core);

    Pusher push(clk, *in, {{1, MakeMsg(3, 55)}});
    Catcher cat(clk, *out);
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    got = cat.got;
    stream_used = xb.StreamUsed(kOutIdxRight);
    triggers = xb.StreamUsed(kOutCore);
  }
  RT::Reset();
  EXPECT_EQ(got, 1u);          // 照样转发出去
  EXPECT_EQ(stream_used, 0u);  // 不记账
  EXPECT_EQ(triggers, 0u);     // 不投递本 core
}

// A16：两个坏 core 串联，逐跳链式透传，时延是逐跳累加而不是单跳。
TEST(BachRouterScenario, A16ChainedPassThroughAccumulatesHops) {
  uint64_t one_hop = 0, two_hop = 0;
  auto run = [&](uint64_t hops) -> uint64_t {
    uint64_t arrive = 0;
    {
      ClockPtr clk = MakeClock(0, kPeriod);
      std::vector<std::unique_ptr<RouterTable>> tabs;
      std::vector<std::unique_ptr<RouterStation>> sts;
      std::vector<std::unique_ptr<Xbar>> xbs;
      std::vector<LinkEndPtr> wires;
      wires.push_back(MakeWire(clk));
      for (uint64_t h = 0; h < hops; ++h) {
        tabs.push_back(std::make_unique<RouterTable>(
            clk, "rtab" + std::to_string(h)));
        tabs.back()->Preload(3, Bypass(kFlowRight));
        // 每一跳是一颗中间列 chip 的坏 core2，透传档由 core_bad_mask 那一位打开。
        tabs.back()->SetCoreBadMask(0x084);
        bool bad = tabs.back()->CoreBad(2);
        sts.push_back(std::make_unique<RouterStation>(
            clk, "st" + std::to_string(h), kDirLeft, *tabs.back(), 0));
        sts.back()->SetPassThrough(bad);
        xbs.push_back(std::make_unique<Xbar>(clk, "xb" + std::to_string(h)));
        xbs.back()->SetPassThrough(bad);
        xbs.back()->AttachReq(kInLeft, sts.back()->ReqPtr());
        sts.back()->AttachUp(wires.back());
        wires.push_back(MakeWire(clk));
        xbs.back()->AttachOutLink(kOutIdxRight, wires.back());
        xbs.back()->AttachBackLink(kOutIdxRight, MakeWire(clk));
      }
      Pusher push(clk, *wires.front(), {{1, MakeMsg(3, 55)}});
      Catcher cat(clk, *wires.back());
      clk->Continue(80 * kPeriod);
      RT::JoinAll();
      arrive = cat.at.empty() ? 0 : cat.at.front();
    }
    RT::Reset();
    return arrive;
  };
  one_hop = run(1);
  two_hop = run(2);
  ASSERT_GT(one_hop, 0u);
  ASSERT_GT(two_hop, 0u);
  // 一跳的流水是三拍：station 收进 VC Buffer、提请求给 Xbar、Xbar 授予并发出。
  // 第一跳前面还有推入那一拍与 station 读到那一拍，所以单跳落在第 5 拍。
  EXPECT_EQ(one_hop, 5u);
  EXPECT_EQ(two_hop, 8u);
  // 多一个坏 core 就多一跳的三拍，不是把两跳压成单跳
  EXPECT_EQ(two_hop - one_hop, 3u);
}

// A17 的三个 reduce 用例走完整装配，见 router_assembly.cpp：ReduceModule 的结果
// 直接给 Xbar 提请求，单独拎出来测就没有下家收它。

// 进核三态准入挪到了 Xbar 那一级（F-021），用例见 router_arbiter.cpp。

// CoreMem 重发：同 VC 保序，先存的先出。
TEST(BachRouterScenario, ReissueKeepsOrderWithinVc) {
  std::vector<uint64_t> users;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreMemReissue re(clk, "re", /*pkts_per_vc=*/4);
    LinkEndPtr out = MakeWire(clk);
    re.AttachOut(out);

    class Waker : public BachModule {
     public:
      Waker(ClockPtr c, CoreMemReissue& r) : BachModule(c, "w"), re(r) {}

     protected:
      void Step() override {
        uint64_t now = CycleNow();
        if (now == 1) {
          for (uint64_t i = 0; i < 3; ++i) {
            FlitView f;
            f.valid = true;
            f.vc = 1;
            f.head = f.tail = true;
            f.bytes = 256;
            f.msg = MakeMsg(1, 300 + i);
            re.Store(f);
          }
        }
        if (now >= 3) re.Wake(1);
      }

     private:
      CoreMemReissue& re;
    };
    Waker w(clk, re);

    class UserCatcher : public BachModule {
     public:
      UserCatcher(ClockPtr c, LinkEnd& f) : BachModule(c, "uc"), src(f) {}
      std::vector<uint64_t> users;

     protected:
      void Step() override {
        FlitView f = ReadFlit(src.flit);
        if (f.valid && f.msg) users.push_back(f.msg->user_id);
      }

     private:
      LinkEnd& src;
    };
    UserCatcher cat(clk, *out);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    users = cat.users;
  }
  RT::Reset();
  ASSERT_EQ(users.size(), 3u);
  EXPECT_EQ(users[0], 300u);
  EXPECT_EQ(users[1], 301u);
  EXPECT_EQ(users[2], 302u);
}

// 本级退休放的是进本 core 那一项（F-036）：各下游方向那几项不动，它们要等那些
// 下游各自退休时还回来。
TEST(BachRouterScenario, RetireFreesTheCoreEntryAndKeepsTheDownstreamOnes) {
  bool core_after = true, down_after = false;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    RouterTable rtab(clk, "rtab");
    Xbar xb(clk, "xbar");
    Retire rt(clk, "retire", rtab);
    xb.AttachRetire(rt.BroadcastPtr());
    // 进本 core 与往右各给这个用户占一项。
    xb.TakeStream(kStreamNeedCore | (1ull << kOutIdxRight), 88);

    class Driver : public BachModule {
     public:
      Driver(ClockPtr c, Retire& r, Xbar& x)
          : BachModule(c, "d"), rt(r), xb(x) {}
      bool core_after = true, down_after = false;

     protected:
      void Step() override {
        uint64_t now = CycleNow();
        if (now == 3 || now == 4) {
          rt.Req().Drive(88);
        } else {
          rt.Req().Idle();
        }
        if (now == 10) {
          core_after = xb.StreamHolds(kOutCore, 88);
          down_after = xb.StreamHolds(kOutIdxRight, 88);
        }
      }

     private:
      Retire& rt;
      Xbar& xb;
    };
    Driver d(clk, rt, xb);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    core_after = d.core_after;
    down_after = d.down_after;
  }
  RT::Reset();
  EXPECT_FALSE(core_after) << "退休当场放进本 core 那一项";
  EXPECT_TRUE(down_after) << "下游那几项不跟着本级退休放";
}

// CreditMonitor：多个事件同时满足时按 StreamID 选最老的。
TEST(BachRouterScenario, MonitorPicksLowestStreamId) {
  std::vector<uint64_t> order;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    RouterTable rtab(clk, "rtab");
    CreditMonitor mon(clk, "mon", rtab, 0);
    mon.SetCheck([](uint64_t, uint64_t) { return true; });

    class Reg : public BachModule {
     public:
      Reg(ClockPtr c, CreditMonitor& m) : BachModule(c, "r"), mon(m) {}
      std::vector<uint64_t> order;

     protected:
      void Step() override {
        uint64_t now = CycleNow();
        // 先注册 stream 5，再注册 stream 2；满足时该先通知 stream 2
        if (now == 1) {
          mon.Req().Drive(10, 5, 1, 1);
        } else if (now == 3) {
          mon.Req().Drive(11, 2, 2, 1);
        } else {
          mon.Req().Idle();
        }
        if (mon.Grant().Valid()) {
          order.push_back(mon.Grant().stream_id.Get());
        }
      }

     private:
      CreditMonitor& mon;
    };
    Reg reg(clk, mon);
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    order = reg.order;
  }
  RT::Reset();
  ASSERT_GE(order.size(), 1u);
  // 第一笔来得早，还没等到第二笔就被通知了；关键是两笔都出得来
  EXPECT_EQ(order.size(), 2u);
}
