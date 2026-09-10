// 链路的行为基线：一笔进去什么时候出来、多笔排队怎么排、三种 release 各走各的。
//
// 观察量是收端自己记的到达拍与条数，不去翻链路的在途队列：那些容器只由 Step()
// 触碰，外面读就是跨线程读非 atomic 容器。

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/link/link.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

// 在指定拍往链路入口推一笔，推完就闲着。
class FlitSender : public BachModule {
 public:
  struct Job {
    uint64_t at = 0;
    uint64_t bytes = 0;
    uint64_t vc = 0;
  };

  FlitSender(ClockPtr c, LinkEnd& port, std::vector<Job> job_list)
      : BachModule(c, "sender"), dst(port), jobs(std::move(job_list)) {}

  std::vector<uint64_t> sent_at;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    bool drove = false;
    for (auto const& j : jobs) {
      if (j.at != now) continue;
      auto m = std::make_shared<Message>();
      m->size = j.bytes;
      m->user_id = now;
      dst.flit.Drive(j.vc, true, true, j.bytes, m);
      sent_at.push_back(now);
      drove = true;
      break;
    }
    if (!drove) dst.flit.Idle();
    dst.release.Idle();
  }

 private:
  LinkEnd& dst;
  std::vector<Job> jobs;
};

// 在指定拍推一笔 release，三种各推一次。
class ReleaseSender : public BachModule {
 public:
  ReleaseSender(ClockPtr c, LinkEnd& port, uint64_t fire_at)
      : BachModule(c, "rel_sender"), dst(port), at(fire_at) {}

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    dst.flit.Idle();
    if (now == at) {
      dst.release.Drive(true, 2, true, 7, true, 9);
    } else {
      dst.release.Idle();
    }
  }

 private:
  LinkEnd& dst;
  uint64_t at;
};

class Sink : public BachModule {
 public:
  Sink(ClockPtr c, LinkEnd& port) : BachModule(c, "sink"), src(port) {}

  std::vector<uint64_t> flit_at;
  std::vector<uint64_t> flit_user;
  std::vector<uint64_t> vc_at, stream_at, reduce_at;
  uint64_t vc_id = 0, stream_user = 0, reduce_user = 0;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    FlitView f = ReadFlit(src.flit);
    if (f.valid) {
      flit_at.push_back(now);
      flit_user.push_back(f.msg ? f.msg->user_id : 0);
    }
    ReleaseView r = ReadRelease(src.release);
    if (r.vc_valid) {
      vc_at.push_back(now);
      vc_id = r.vc_id;
    }
    if (r.stream_valid) {
      stream_at.push_back(now);
      stream_user = r.stream_user;
    }
    if (r.reduce_valid) {
      reduce_at.push_back(now);
      reduce_user = r.reduce_user;
    }
  }

 private:
  LinkEnd& src;
};

struct RunResult {
  std::vector<uint64_t> sent_at, flit_at, flit_user;
  std::vector<uint64_t> vc_at, stream_at, reduce_at;
  uint64_t vc_id = 0, stream_user = 0, reduce_user = 0;
};

RunResult RunFlits(LinkParams const& cfg, std::vector<FlitSender::Job> jobs,
             Time end) {
  RunResult r;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    Link link(clk, "link", cfg);
    FlitSender snd(clk, link.In(), std::move(jobs));
    Sink sink(clk, link.Out());
    clk->Continue(end * kPeriod);
    RT::JoinAll();
    r.sent_at = snd.sent_at;
    r.flit_at = sink.flit_at;
    r.flit_user = sink.flit_user;
  }
  RT::Reset();
  return r;
}

}  // namespace

// 一笔 256 B 走 R2R：发送侧写进入口，经 ceil(256/256) + 40T 到达，收端再晚一拍
// 看到（端口本身的一拍，被 latency 吸收，不额外扣时）。
TEST(BachLink, Arrive) {
  RunResult r = RunFlits(LinkR2R(), {{1, 256, 0}}, 80);
  ASSERT_EQ(r.sent_at.size(), 1u);
  ASSERT_EQ(r.flit_at.size(), 1u);
  EXPECT_EQ(r.sent_at[0], 1u);
  // 入口那一拍 + 1 + 40 + 出口那一拍
  EXPECT_EQ(r.flit_at[0], 1u + 1u + 1u + 40u + 1u);
}

// 空包按一拍算，与 CalcCycles 的 size ≤ 0 走同一条路。
TEST(BachLink, EmptyPacketCostsOneCycle) {
  RunResult r = RunFlits(LinkR2R(), {{1, 0, 0}}, 80);
  ASSERT_EQ(r.flit_at.size(), 1u);
  EXPECT_EQ(r.flit_at[0], 1u + 1u + 1u + 40u + 1u);
}

// 同一拍发不了两笔，但连着几拍发进来的要按占用排队：第二笔的到达拍不早于第一笔。
TEST(BachLink, Monotonic) {
  RunResult r = RunFlits(LinkR2R(), {{1, 1024, 0}, {2, 256, 0}, {3, 256, 0}}, 200);
  ASSERT_EQ(r.flit_at.size(), 3u);
  EXPECT_LT(r.flit_at[0], r.flit_at[1]);
  EXPECT_LT(r.flit_at[1], r.flit_at[2]);
  // 先发的先到，顺序不被后面的小包插队
  EXPECT_EQ(r.flit_user[0], 1u);
  EXPECT_EQ(r.flit_user[1], 2u);
  EXPECT_EQ(r.flit_user[2], 3u);
}

// 1024 B 占 4 拍，第二笔要等这 4 拍走完才起算。
TEST(BachLink, BandwidthOccupancy) {
  RunResult one = RunFlits(LinkR2R(), {{1, 1024, 0}}, 200);
  RunResult two = RunFlits(LinkR2R(), {{1, 1024, 0}, {2, 256, 0}}, 200);
  ASSERT_EQ(one.flit_at.size(), 1u);
  ASSERT_EQ(two.flit_at.size(), 2u);
  EXPECT_EQ(two.flit_at[0], one.flit_at[0]);
  // 第一笔占到第 1+4=5 拍，第二笔从 5 起算再走 1 拍加延迟
  EXPECT_EQ(two.flit_at[1], two.flit_at[0] + 1u);
}

// chip 间 400T 的 C2C：只有延迟不同，公式一样。
TEST(BachLink, C2CLatency) {
  RunResult r2r = RunFlits(LinkR2R(), {{1, 256, 0}}, 600);
  RunResult c2c = RunFlits(LinkC2C(), {{1, 256, 0}}, 600);
  ASSERT_EQ(c2c.flit_at.size(), 1u);
  EXPECT_EQ(c2c.flit_at[0] - r2r.flit_at[0], 400u - 40u);
}

// PCIe ↔ Router 左右：128 B/T，10T + 25T。256 B 要两拍。
TEST(BachLink, PcieRouterLeftRight) {
  RunResult r = RunFlits(LinkPcieRouterLr(), {{1, 256, 0}}, 200);
  ASSERT_EQ(r.flit_at.size(), 1u);
  EXPECT_EQ(r.flit_at[0], 1u + 1u + 2u + 35u + 1u);
}

// 三种 release 各走各的通道，同一拍进去同一拍出来，互不排队。
TEST(BachLink, ReleaseChannelsAreIndependent) {
  std::vector<uint64_t> vc_at, stream_at, reduce_at;
  uint64_t vc_id = 0, stream_user = 0, reduce_user = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    Link link(clk, "link", LinkR2R());
    ReleaseSender snd(clk, link.In(), 1);
    Sink sink(clk, link.Out());
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    vc_at = sink.vc_at;
    stream_at = sink.stream_at;
    reduce_at = sink.reduce_at;
    vc_id = sink.vc_id;
    stream_user = sink.stream_user;
    reduce_user = sink.reduce_user;
  }
  RT::Reset();
  ASSERT_EQ(vc_at.size(), 1u);
  ASSERT_EQ(stream_at.size(), 1u);
  ASSERT_EQ(reduce_at.size(), 1u);
  EXPECT_EQ(vc_at[0], stream_at[0]);
  EXPECT_EQ(stream_at[0], reduce_at[0]);
  EXPECT_EQ(vc_id, 2u);
  EXPECT_EQ(stream_user, 7u);
  EXPECT_EQ(reduce_user, 9u);
}
