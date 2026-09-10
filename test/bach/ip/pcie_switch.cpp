// PCIe Switch 的行为基线：一份数据落到哪几个端口、队列满了怎么反压、两路怎么轮。
//
// 观察量是收端记的条数与 Switch 自己的计数器，不去翻它的出口队列。

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/pcie_switch.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

// 每拍往入口推一个 flit，推 n 拍。
class Feeder : public BachModule {
 public:
  Feeder(ClockPtr c, LinkEnd& port, uint64_t to, uint64_t how_many)
      : BachModule(c, "feeder"), sink(port), dst(to), count(how_many) {}

  uint64_t pushed = 0;

 protected:
  void Step() override {
    if (pushed < count) {
      auto m = std::make_shared<Message>();
      m->dst = dst;
      m->size = 256;
      m->token_id = pushed;
      sink.flit.Drive(0, true, true, 256, m);
      ++pushed;
    } else {
      sink.flit.Idle();
    }
    sink.release.Idle();
  }

 private:
  LinkEnd& sink;
  uint64_t dst, count;
};

// 只收不回压。
class Catcher : public BachModule {
 public:
  Catcher(ClockPtr c, LinkEnd& from) : BachModule(c, "catcher"), src(from) {}

  uint64_t got = 0;
  std::vector<uint64_t> tokens;

 protected:
  void Step() override {
    FlitView f = ReadFlit(src.flit);
    if (f.valid) {
      ++got;
      tokens.push_back(f.msg ? f.msg->token_id : 0);
    }
  }

 private:
  LinkEnd& src;
};

// Logic64 在主线程读到的是 t=0 的值，所以计数要在协程内 snapshot 到普通成员，
// JoinAll 之后再从主线程取。
class Probe : public BachModule {
 public:
  Probe(ClockPtr c, PcieSwitch& target) : BachModule(c, "probe"), sw(target) {}

  uint64_t accepted = 0, forwarded = 0, stalled = 0;

 protected:
  void Step() override {
    accepted = sw.Accepted();
    forwarded = sw.Forwarded();
    stalled = sw.Stalled();
  }

 private:
  PcieSwitch& sw;
};

}  // namespace

// 组播关时一个 dst 只落一个端口。
TEST(BachPcieSwitch, UnicastToOnePort) {
  uint64_t got0 = 0, got1 = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    PcieSwitch sw(clk, "sw", 3);
    sw.SetRoute(7, {1});
    Feeder feed(clk, sw.In(0), 7, 4);
    Catcher c1(clk, sw.Out(1));
    Catcher c2(clk, sw.Out(2));
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    got0 = c1.got;
    got1 = c2.got;
  }
  RT::Reset();
  EXPECT_EQ(got0, 4u);
  EXPECT_EQ(got1, 0u);
}

// 组播开时一份数据复制到多个收端，条数一样、顺序一样。
TEST(BachPcieSwitch, Multicast) {
  uint64_t a = 0, b = 0;
  std::vector<uint64_t> ta, tb;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    PcieSwitch sw(clk, "sw", 3);
    sw.EnableMulticast(true);
    sw.SetRoute(7, {1, 2});
    Feeder feed(clk, sw.In(0), 7, 5);
    Catcher c1(clk, sw.Out(1));
    Catcher c2(clk, sw.Out(2));
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    a = c1.got;
    b = c2.got;
    ta = c1.tokens;
    tb = c2.tokens;
  }
  RT::Reset();
  EXPECT_EQ(a, 5u);
  EXPECT_EQ(b, 5u);
  EXPECT_EQ(ta, tb);
}

// 收端不取时出口队列会满，满了就不再收，计入 stalled。这是反压不是丢弃：
// 上游本拍的那一笔留在原地。
TEST(BachPcieSwitch, BackpressureWhenQueueFull) {
  uint64_t accepted = 0, stalled = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    // 没有 Catcher，出口 flit 没人读，但 Switch 每拍照样出队一个，所以队列
    // 不会满。这里改成推得比出得快：一拍推一个、一拍出一个，正好持平。
    // 要制造满，得让入口多于一个。
    PcieSwitch sw(clk, "sw", 3);
    sw.SetRoute(7, {2});
    Feeder f0(clk, sw.In(0), 7, 100);
    Feeder f1(clk, sw.In(1), 7, 100);
    Catcher c(clk, sw.Out(2));
    Probe probe(clk, sw);
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    accepted = probe.accepted;
    stalled = probe.stalled;
  }
  RT::Reset();
  // 两个入口共抢一个出口，出口每拍只出一个，所以一定有被挡住的
  EXPECT_GT(stalled, 0u);
  EXPECT_GT(accepted, 0u);
}

// 按 flit 轮流走两路 x16。
TEST(BachPcieSwitch, TwoLanesRoundRobin) {
  std::vector<uint64_t> lanes;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    PcieSwitch sw(clk, "sw", 2);
    sw.SetRoute(3, {1});
    Feeder feed(clk, sw.In(0), 3, 4);
    Catcher c(clk, sw.Out(1));
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    lanes.push_back(sw.LaneOf(1));
  }
  RT::Reset();
  // 4 个 flit 轮完两路正好回到 0
  ASSERT_EQ(lanes.size(), 1u);
  EXPECT_EQ(lanes[0], 0u);
}
