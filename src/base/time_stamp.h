#ifndef _LATCH_TIME_
#define _LATCH_TIME_

#include <cstdint>
#include <limits>

namespace latch {

using Time = uint64_t;

constexpr Time TimeMax{std::numeric_limits<uint32_t>::max()};

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
