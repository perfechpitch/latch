// RT::Reset() 每轮把线程池整个拆掉重建。建线程时 libco 会为每个线程分配一份
// thread_local 的 env：约 960 KB 的超时时间轮（60*1000 槽 × 16 B，latch 从不用
// 它）加一个 epoll fd；每个协程再各占一个 128 KB 的栈加 8 KB 的描述符。
//
// 这些东西原先没有任何一处释放：gCoEnvPerThread 是个裸的 __thread 指针，线程
// 退出时没有析构会碰它；std::vector<CoTask> 的析构只还 CoTask 结构体，不还它
// 指向的 stCoRoutine_t。于是每轮 Reset 漏掉的量随线程数线性增长，而测试的
// SetUp/teardown 到处都在调 Reset，长跑一路涨到 OOM。
//
// 这个用例盯的就是这条：跑若干轮 Reset，看 RSS 稳不稳。

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <fstream>

#include "base/runtime.h"

using namespace latch;

namespace {

// /proc/self/statm 的第二个字段是常驻页数。
uint64_t RssBytes() {
  std::ifstream f("/proc/self/statm");
  uint64_t total_pages = 0;
  uint64_t resident_pages = 0;
  f >> total_pages >> resident_pages;
  return resident_pages * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
}

}  // namespace

TEST(RuntimeLeak, ResetReleasesCoroutineStacksAndThreadEnv) {
  constexpr int kSub = 4;
  constexpr int kCo = 8;
  constexpr int kRounds = 50;

  // 先热几轮再取基准：首轮的分配、分配器的 arena 增长、线程栈的缓存都会让 RSS
  // 抬一截，那些是一次性的，不是泄漏。量的是热起来之后的斜率。
  constexpr int kWarmup = 5;
  for (int i = 0; i < kWarmup; ++i) RT::Reset(kSub, kCo);
  const uint64_t before = RssBytes();

  for (int i = 0; i < kRounds; ++i) RT::Reset(kSub, kCo);

  const uint64_t after = RssBytes();
  const uint64_t growth = after > before ? after - before : 0;

  std::printf("  %d 轮 RT::Reset(%d,%d)：RSS %llu KB -> %llu KB（增长 %llu KB）\n",
              kRounds, kSub, kCo, static_cast<unsigned long long>(before / 1024),
              static_cast<unsigned long long>(after / 1024),
              static_cast<unsigned long long>(growth / 1024));

  // 实测：漏的话 50 轮涨约 22 MB（每轮约 450 KB，只算真正碰过的页）；修好之后
  // 热完这一轮基本是平的。4 MB 这个门限两边都隔得开。
  EXPECT_LT(growth, 4ull * 1024 * 1024)
      << "50 轮 RT::Reset 涨了 " << growth / 1024
      << " KB，协程栈或 libco 的线程环境没有释放";
}
