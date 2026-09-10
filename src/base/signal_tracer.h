#ifndef _LATCH_SIGNAL_TRACER_
#define _LATCH_SIGNAL_TRACER_

#include <cstdint>
#include <string>
#include <unordered_map>

#include "base/log.h"
#include "base/recorder.h"
#include "base/runtime.h"
#include "base/varint.h"

namespace latch {

inline bool g_trace_disabled = false;
inline void SetTraceDisabled(bool disabled) { g_trace_disabled = disabled; }
inline bool TraceDisabled() { return g_trace_disabled; }

// 建一段模块的时候把波形关掉，出了作用域还原成进来时的样子。一个模块记不记波形
// 在它建出来那一刻就定下来了，所以把不看的那几个模块的构造包进来，它们的信号一
// 个都不进波形，模块自己的代码不动。
//
// 出作用域还原而不是一律打开：外层可能本来就关着（只给头几颗 chip 记波形的那种
// 跑法），一律打开会把那几颗又打开。
class TraceOffScope {
 public:
  TraceOffScope() : saved(TraceDisabled()) { SetTraceDisabled(true); }
  ~TraceOffScope() { SetTraceDisabled(saved); }
  TraceOffScope(const TraceOffScope&) = delete;
  TraceOffScope& operator=(const TraceOffScope&) = delete;

 private:
  bool saved;
};

class SignalTracer {
 public:
  ~SignalTracer() { Flush(); }

  void Record(uint64_t owner, const std::string& name, uint64_t time,
              uint64_t value) {
    if (TraceDisabled()) return;
    LOGCHECK(owner != 0,
             "SignalTracer::Record: owner id is 0, register it first.");
    TraceSlot& slot = SlotFor(owner, name);

    if (!slot.primed) {
      slot.t_first = time;
      slot.v_first = value;
      slot.t_last  = time;
      slot.v_last  = value;
      slot.v_min   = value;
      slot.v_max   = value;
      slot.event_count = 1;
      slot.primed = true;
      return;
    }

    const uint64_t dt = time - slot.t_last;
    const int64_t  dv = static_cast<int64_t>(value) -
                        static_cast<int64_t>(slot.v_last);
    VarintEncode(slot.buf, dt);
    VarintEncode(slot.buf, ZigZagEncode(dv));

    slot.t_last = time;
    slot.v_last = value;
    if (value < slot.v_min) slot.v_min = value;
    if (value > slot.v_max) slot.v_max = value;
    ++slot.event_count;

    auto& rec = RT::GetRecorder();
    if (slot.buf.size() >= rec.FlushThreshold()) rec.AppendSegment(slot);
  }

  uint64_t IdFor(uint64_t owner, const std::string& name) {
    return SlotFor(owner, name).id;
  }

  void Flush() {
    auto& rec = RT::GetRecorder();
    for (auto& kv : slots) rec.AppendSegment(kv.second);
  }

 private:
  TraceSlot& SlotFor(uint64_t owner, const std::string& name) {

    std::string key = std::to_string(owner) + "\x1f" + name;
    auto it = slots.find(key);
    if (it != slots.end()) return it->second;
    const uint64_t sig_id = RT::GetModulePool().CreateID(owner, name);
    TraceSlot& slot = slots.emplace(key, TraceSlot{}).first->second;
    slot.id = sig_id;
    return slot;
  }

  std::unordered_map<std::string, TraceSlot> slots;
};

}

#endif
