// DTE 自己持有的那几张表。
//
// Hmem 按 stream_id 索引，硬件包头与软件包头合并成一项；stream_cache 只跟随不分配；
// 本级 Reduce credit 按用户记。

#include <gtest/gtest.h>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/dte/hmem.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

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
