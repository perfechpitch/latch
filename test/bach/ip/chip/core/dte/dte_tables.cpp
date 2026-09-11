// DTE 自己持有的那几张表，以及出核那一层的仲裁。
//
// Hmem 按 stream_id 索引，硬件包头与软件包头合并成一项；Fast LUT 按 task_id 索引，
// 命中才走快路径；stream_cache 只跟随不分配；本级 Reduce credit 按用户记。
// 出核四个通道对 Router 那一个口轮转，一个包的几拍不许被别的包插进来。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/dte/hmem.h"
#include "bach/ip/chip/core/dte/out_arb.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

}  // namespace

// Hmem 一项就是硬件包头加软件包头：硬件只改 core_mask，软件改那 16 B。
TEST(BachDteHmem, HeaderTablesAreMergedPerStream) {
  ClockPtr clk = MakeClock(0, kPeriod);
  Hmem h(clk, "hmem", 0, false);

  h.SetCoreMask(3, 0x00FF);
  h.Entry(3).sw_header[0] = 0xAB;
  EXPECT_EQ(h.Entry(3).core_mask, 0x00FFu);
  EXPECT_EQ(h.Entry(3).sw_header[0], 0xAB);
  // 各 stream 各一项，互不影响。
  EXPECT_EQ(h.Entry(4).core_mask, 0u);
  EXPECT_EQ(h.Entry(4).sw_header[0], 0);
  RT::Reset();
}

// path_id 到 task_id 的反查表由 TS 那一侧配好，DTE 只读。
TEST(BachDteHmem, PathTaskMapIsReadOnlyHere) {
  ClockPtr clk = MakeClock(0, kPeriod);
  Hmem h(clk, "hmem", 0, false);
  h.PreloadPathTask(7, 3);
  h.PreloadPathTask(8, 5);
  EXPECT_EQ(h.PathTask(7), 3u);
  EXPECT_EQ(h.PathTask(8), 5u);
  EXPECT_EQ(h.PathTask(9), 0u) << "没配过的回 0";
  RT::Reset();
}

// Fast LUT：命中走快路径，不命中要启动 RV core 的 kernel。
TEST(BachDteHmem, FastLutHitAndMiss) {
  ClockPtr clk = MakeClock(0, kPeriod);
  Hmem h(clk, "hmem", 0, false);
  EXPECT_FALSE(h.LutHit(3)) << "没配过就是不命中";
  h.PreloadLut(3, 512, 0);
  EXPECT_TRUE(h.LutHit(3));
  EXPECT_FALSE(h.LutHit(4));
  RT::Reset();
}

// stream_cache 只跟随不分配：按 Router 送回来的 release 记账，方向各记各的。
TEST(BachDteHmem, StreamCacheOnlyFollows) {
  ClockPtr clk = MakeClock(0, kPeriod);
  Hmem h(clk, "hmem", 0, false);

  EXPECT_FALSE(h.CacheHolds(kDirLeft, 41));
  h.FollowStreamCredit(kDirLeft, 41);
  EXPECT_TRUE(h.CacheHolds(kDirLeft, 41));
  EXPECT_FALSE(h.CacheHolds(kDirRight, 41)) << "方向各记各的";
  // 同一个用户跟随两次不占两项。
  h.FollowStreamCredit(kDirLeft, 41);
  h.DropStreamCredit(kDirLeft, 41);
  EXPECT_FALSE(h.CacheHolds(kDirLeft, 41));
  RT::Reset();
}

namespace {

// 四个通道往仲裁器里放拍，收 Router 那一侧发出来的。
class ArbHarness : public BachModule {
 public:
  struct Beat {
    uint64_t at = 0;
    uint64_t lane = 0;
    uint64_t user = 0;
    bool last = false;
  };

  ArbHarness(ClockPtr c, DteOutArb& target)
      : BachModule(c, "harness"), arb(target) {}

  std::vector<Beat> beats;

  struct Got {
    uint64_t at = 0, user = 0;
    bool last = false;
  };
  std::vector<Got> out;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做：先看上一拍发出去的。
    CoreDataView d = ReadCoreData(arb.Out());
    if (d.valid && d.seq != last_out_seq) {
      last_out_seq = d.seq;
      out.push_back({now, d.msg ? d.msg->user_id : 0, d.last});
    }
    arb.Out().DriveReady(true);

    std::vector<bool> drove(DteOutArb::kOutNum, false);
    for (auto const& b : beats) {
      if (b.at != now) continue;
      auto m = std::make_shared<Message>();
      m->user_id = b.user;
      m->size = 256;
      arb.LanePort(b.lane)->Drive(256, b.last, !b.last, 0, m);
      drove[b.lane] = true;
    }
    for (uint64_t i = 0; i < DteOutArb::kOutNum; ++i) {
      if (!drove[i]) arb.LanePort(i)->Idle();
    }
    arb.RunStep();
  }

 private:
  DteOutArb& arb;
  uint64_t last_out_seq = 0;
};

}  // namespace

// 四个出核通道轮转用 Router 那一个口。
TEST(BachDteOutArb, FourLanesTakeTurns) {
  std::vector<uint64_t> users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteOutArb arb(clk, "arb", 0, false);
    ArbHarness h(clk, arb);
    // 四个通道各放一个单拍的包。
    for (uint64_t i = 0; i < DteOutArb::kOutNum; ++i) {
      h.beats.push_back({2, i, 100 + i, /*last=*/true});
    }
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    for (auto const& g : h.out) users.push_back(g.user);
  }
  RT::Reset();
  ASSERT_EQ(users.size(), 4u) << "四个通道的包都要发出去";
  std::vector<uint64_t> sorted = users;
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(sorted[0], 100u);
  EXPECT_EQ(sorted[3], 103u);
}

// 一个包的几拍不许被别的包插进来：授权粘在一个通道上直到它发完带 tlast 的那拍。
TEST(BachDteOutArb, OnePacketIsNotInterleaved) {
  std::vector<uint64_t> users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteOutArb arb(clk, "arb", 0, false);
    ArbHarness h(clk, arb);
    // 通道 0 是一个三拍的包，通道 1 同时也想发。
    h.beats = {{2, 0, 100, false},
               {3, 0, 100, false},
               {4, 0, 100, true},
               {2, 1, 200, true},
               {6, 1, 201, true}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    for (auto const& g : h.out) users.push_back(g.user);
  }
  RT::Reset();
  ASSERT_GE(users.size(), 4u);
  // 通道 0 那个包的三拍要连着走完，中间不夹别人的。
  EXPECT_EQ(users[0], 100u);
  EXPECT_EQ(users[1], 100u);
  EXPECT_EQ(users[2], 100u);
  EXPECT_EQ(users[3], 200u);
}
