#ifndef _LATCH_TIME_
#define _LATCH_TIME_

#include <cstdint>
#include <limits>

namespace latch {

using Time = uint64_t;

// 无穷哨兵：时钟末端、待跑队列为空、线程本地最小时间戳的初值都用它。
//
// 它原本是 uint32_max，问题不在于"上限只有 32 位"，而在于它正好等于下面那个
// 打包上限 —— 于是"一次合法的写入落在 t = 0xffffffff"和"这个槽是空的"成了同一个
// 数，读回来会静默当成没写过。Time 本身就是 uint64_t，哨兵没有理由挤在 32 位里，
// 这里用满 64 位，跟打包上限彻底分开。
constexpr Time TimeMax{std::numeric_limits<uint64_t>::max()};

// Latch 的环形槽把 (线程号, 时间) 压进一个 uint64：高 32 位线程号、低 32 位时间，
// 低 32 位全 1 是空槽标记。仿真时间因此必须严格小于 0xffff'ffff，也就是不超过
// 下面这个值 —— 1 T 一拍下约 4.29e9 拍。越界由 Clock::Continue 在一开始就拦下，
// 不会跑到一半才发现。
constexpr Time kPackedTimeMax{0xffff'fffeull};

class Period {
 public:
  Period(const Time& startPoint, const Time& durationTime)
      : start(startPoint), duration(durationTime) {}
  ~Period() {}

  Time start;
  Time duration;
};

}

#endif
