#ifndef _LATCH_CLOCK_
#define _LATCH_CLOCK_

#include <atomic>
#include <functional>

#include "runtime.h"
#include "time_stamp.h"

namespace latch {
class Clock {
 public:
  Clock() : Clock(0, 10, TimeMax) {}
  Clock(Time startTime, Time period_time) : Clock(startTime, period_time, TimeMax) {}
  Clock(Time startTime, Time period_time, Time endTime)
      : start(startTime),
        end(endTime),
        period(period_time),
        now(start),
        stop(false),
        isContd(false) {}
  Clock(Clock const&) = delete;
  void operator=(Clock const&) = delete;
  virtual ~Clock(){};

  Time Start() { return start; }
  Time Stride() { return period; }
  Time CycleToTime(uint32_t cycle) { return cycle * period; }
  uint64_t TimeToCycle(Time t) { return t / period; }

  void Continue(Time t = TimeMax) {
    LOGCHECK(!isContd.load(), "Can not Continue twice.");
    isContd.store(true);

    Time continue_tgt = std::min(t, end);
    const bool bounded = (continue_tgt < TimeMax);

    auto advance_to = [](Time tgt) {
      if (GetTarget() < tgt) TargetIncrease(tgt - GetTarget());
      TargetAchieve();
      Runtime::Synchronize();
    };
    const Time period_local = period;
    if (bounded) {
      const uint64_t cycles = (continue_tgt > now)
                                  ? (continue_tgt - now) / period
                                  : 0;
      for (size_t k = 0; k < functions.size(); ++k) {
        auto fn = functions[k];
        RT::Launch(
            [this, fn, cycles, advance_to, period_local] {
              const Time start_time = GetBase();
              for (uint64_t c = 0; c < cycles; ++c) {
                const Time pre = GetTarget();
                fn();
                const Time post = GetTarget();
                LOGCHECK(post - pre <= period_local,
                         "Clock::Continue: Cycle() advanced Target by "
                         "more than one period (DelayCycle(N>1) or "
                         "equivalent inside Cycle). Persistent-worker "
                         "model cannot pipeline multi-cycle units — "
                         "use Clock::ContinueLaunch for those.");
                advance_to(start_time + (c + 1) * period);
              }
            },
            now, static_cast<uint32_t>(k));
      }
      now += cycles * period;
    } else {
      for (size_t k = 0; k < functions.size(); ++k) {
        auto fn = functions[k];
        RT::Launch(
            [this, fn, advance_to, period_local] {
              const Time start_time = GetBase();
              uint64_t c = 0;
              while (!stop.load(std::memory_order_acquire)) {
                const Time pre = GetTarget();
                fn();
                const Time post = GetTarget();
                LOGCHECK(post - pre <= period_local,
                         "Clock::Continue: Cycle() advanced Target by "
                         "more than one period — use ContinueLaunch.");
                ++c;
                advance_to(start_time + c * period);
              }
            },
            now, static_cast<uint32_t>(k));
      }
    }

    RT::EndSetup();
  }

  void ContinueLaunch(Time t = TimeMax) {
    LOGCHECK(!isContd.load(), "Can not Continue twice.");
    isContd.store(true);

    Time continue_tgt = std::min(t, end);

    auto func = [this, continue_tgt]() {
      DelayCycle(1);
      constexpr uint32_t leadingCycles = 4;
      if (continue_tgt > leadingCycles * period + now) {
        for (uint32_t i = 0; i < leadingCycles; i++) {
          for (size_t k = 0; k < functions.size(); ++k) {
            RT::Launch(functions[k], now, static_cast<uint32_t>(k));
          }
          now += period;
        }
      }

      while (now + period <= continue_tgt) {
        DelayCycle(1);
        for (size_t k = 0; k < functions.size(); ++k) {
          RT::Launch(functions[k], now, static_cast<uint32_t>(k));
        }

        if (stop.load(std::memory_order_acquire)) break;
        now += period;
      }
    };

    RT::Launch(func, start);
    RT::EndSetup();
  }

  void Bind(std::function<void(void)> f) {
    LOGCHECK(!isContd.load(), "couldnot bind after Continue.");
    if (f != nullptr) functions.emplace_back(f);
  }

  bool IsClkEdge(Time t) { return (t - start) % period == 0; }
  void DelayCycle(uint32_t cycle = 1) { RT::Delay(cycle * period); }
  void DelayTime(Time time) { RT::Delay(time); }
  void Stop() { stop.store(true, std::memory_order_release); }
  bool IsStopped() const { return stop.load(std::memory_order_acquire); }

 private:
  const Time start;
  const Time end;
  const Time period;
  Time now;
  std::atomic<bool> stop;
  std::vector<std::function<void(void)>> functions;
  std::atomic_bool isContd;
};

using ClockPtr = std::shared_ptr<Clock>;

template <typename... Args>
ClockPtr MakeClock(Args&&... args) {
  return std::make_shared<Clock>(std::forward<Args>(args)...);
}

}
#endif
