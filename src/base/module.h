#ifndef _LATCH_MODULE_
#define _LATCH_MODULE_

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include "base/clock.h"
#include "base/logic.h"
#include "base/object.h"
#include "base/runtime.h"
#include "base/signal_tracer.h"
#include "base/time_stamp.h"

namespace latch {

class Module : public Object {
 public:
  Module() : Object(){};
  ~Module() override = default;
};

class Clock;

class ClkModule : public Object {
 public:

  // 记不记波形在建出来这一刻定下来，之后不再看全局那个开关。这样一次仿真里
  // 可以只给关心的那几个模块记：构造它们的时候把开关打开，构造其余的时候关上。
  // 阵列大了之后信号数上万，全记的话看波形的那一侧一次要拿十几 MB 的信号摘要。
  ClkModule(std::shared_ptr<Clock> clock, bool tick = true)
      : clk(clock), trace_off(TraceDisabled()) {
    LOGCHECK(clk != nullptr, "clock could not be null.");
    if (tick) clk->Bind([&]() { this->Cycle(); });
  }
  ~ClkModule() override { FlushPerCycle(); }

  virtual void Cycle() = 0;
  virtual void DelayCycle(uint32_t cycle = 1) final { clk->DelayCycle(cycle); }
  virtual void DelayTime(Time t = 10) final { clk->DelayTime(t); }

  void Trace(const std::string& name, uint64_t value) {
    if (trace_off) return;
    tracer.Record(obj_id, name, RT::Now(), value);
  }

  void Trace(const std::string& name, const std::string& value) {
    if (trace_off) return;
    const uint64_t sig_id = tracer.IdFor(obj_id, name);
    const uint64_t id = RT::GetRecorder().InternString(sig_id, value);
    tracer.Record(obj_id, name, RT::Now(), id);
  }

  void TracePerCycle(std::string_view name, uint64_t value) {
    if (trace_off) return;
    const uint64_t t = RT::Now();
    std::string key(name);
    PerCycleSig& s = perCycle[key];
    if (s.open && t == s.curT) { s.curV = value; return; }
    CommitPerCycle(key, s);
    s.curT = t; s.curV = value; s.open = true;
  }

  void TraceUnder(uint64_t owner, const std::string& name, uint64_t value) {
    if (trace_off) return;
    tracer.Record(owner, name, RT::Now(), value);
  }

  template <typename L>
  std::enable_if_t<std::is_base_of_v<LogicBase, L> &&
                       std::is_convertible_v<const L&, uint64_t>,
                   void>
  Trace(const std::string& name, const L& logic) {
    Trace(name, static_cast<uint64_t>(logic));
  }

 protected:
  ClockPtr clk;
  // 建出来那一刻的波形开关，见构造函数那一段。
  bool trace_off = false;

 private:

  struct PerCycleSig { uint64_t curT = 0, curV = 0, lastV = 0; bool open = false, hasLast = false; };
  void CommitPerCycle(const std::string& name, PerCycleSig& s) {
    if (!s.open) return;
    if (!s.hasLast || s.curV != s.lastV) {
      tracer.Record(obj_id, name, s.curT, s.curV);
      s.lastV = s.curV;
      s.hasLast = true;
    }
    s.open = false;
  }
  void FlushPerCycle() {
    for (auto& kv : perCycle) CommitPerCycle(kv.first, kv.second);
  }

  SignalTracer tracer;
  std::unordered_map<std::string, PerCycleSig> perCycle;
};

}

#endif
