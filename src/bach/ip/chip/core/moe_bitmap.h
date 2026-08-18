#ifndef _LATCH_BACH_IP_CHIP_CORE_MOE_BITMAP_
#define _LATCH_BACH_IP_CHIP_CORE_MOE_BITMAP_

// 每个 Core 一张查找表，键是 uid，值是这个 user 在本核上激活的专家数。
//
// Dense 模型下这个数恒为 0，计算倍率按 1 算；MoE 模型下它是倍率本身。表项由 DTE 在
// 收到 USER_INIT 时按封包里的 tag 写入，MatrixCore 与 VectorCore 在计算前读它。
//
// 它有一个访问口，容量 1，一次读或写占一拍。这一拍不是数据量决定的，是端口本身的
// 周期，所以读写同价。
//
// 表项写入后不随退休清除，这是继承自 Bach 的边界：表长本该受 stream 数约束、退休时
// 释放，Bach 没做，重建时原样继承，不额外加回收。

#include <cstdint>
#include <string>
#include <unordered_map>

#include "base/clock.h"
#include "base/log.h"
#include "base/runtime.h"
#include "base/time_stamp.h"
#include "bach/common/arbiter.h"
#include "bach/ip/chip/core/core_context.h"
#include "bach/ip/sub_unit.h"

namespace latch {
namespace bach {

class MoeBitMap : public SubUnit {
 public:
  MoeBitMap(ClockPtr clock, CoreContext const& context,
            std::string const& name, uint64_t parent, bool standalone = false)
      : SubUnit(clock, standalone), ctx(context) {
    RegisterId(name, parent);
  }

  Ticket BeginRead(Time now, uint64_t uid, uint64_t tid) {
    return Begin(now, uid, tid, false, 0);
  }

  Ticket BeginWrite(Time now, uint64_t uid, uint64_t tid, uint64_t value) {
    return Begin(now, uid, tid, true, value);
  }

  bool Done(Ticket t) const { return Find(t).done; }

  // 读取的结果。写请求上问它没有意义，所以这里挡住。
  uint64_t Value(Ticket t) const {
    Job const& j = Find(t);
    LOGCHECK(j.done, "MoeBitMap: read value before done.");
    LOGCHECK(!j.is_write, "MoeBitMap: read value on a write ticket.");
    return j.value;
  }

  void Finish(Ticket t) {
    auto it = jobs.find(t);
    LOGCHECK(it != jobs.end(), "MoeBitMap: finish unknown ticket.");
    LOGCHECK(it->second.done, "MoeBitMap: finish before done.");
    jobs.erase(it);
  }

  // 不走访问口的建档，给那些绕开 USER_INIT 进来的 user 用，不占时间也不记事件。
  void SeedDefault(uint64_t uid, uint64_t value = 0) {
    table.emplace(uid, value);
  }

  bool Has(uint64_t uid) const { return table.count(uid) != 0; }

  void Step() override {
    const Time now = RT::Now();
    for (auto& kv : jobs) {
      Job& j = kv.second;
      if (j.done || !j.started) continue;
      if (now < j.end_cycle) continue;
      j.done = true;
      port.Release(j.lock, now);
      if (j.is_write) {
        table[j.uid] = j.value;
      } else {
        auto it = table.find(j.uid);
        LOGCHECK(it != table.end(), "MoeBitMap: read an unrecorded uid.");
        j.value = it->second;
      }
      ctx.Span(Unit::kBitmap, j.uid, j.tid, j.grant_cycle, j.end_cycle,
               SpanState::kMemoryService);
      ctx.Wait(Unit::kBitmap, j.uid, j.tid, j.enqueue_cycle, j.grant_cycle,
               WaitReason::kBitmapAccess);
    }
    for (auto& kv : jobs) TryStart(kv.first, now);
  }

  uint64_t InFlight() const { return jobs.size(); }

 private:
  struct Job {
    Ticket lock = kNoTicket;
    uint64_t uid = 0;
    uint64_t tid = 0;
    uint64_t value = 0;
    bool is_write = false;
    Time enqueue_cycle = 0;
    Time grant_cycle = 0;
    Time end_cycle = 0;
    bool started = false;
    bool done = false;
  };

  Job const& Find(Ticket t) const {
    auto it = jobs.find(t);
    LOGCHECK(it != jobs.end(), "MoeBitMap: unknown ticket.");
    return it->second;
  }

  Ticket Begin(Time now, uint64_t uid, uint64_t tid, bool is_write,
               uint64_t value) {
    Job j;
    j.lock = port.Enqueue(now, uid);
    j.uid = uid;
    j.tid = tid;
    j.value = value;
    j.is_write = is_write;
    j.enqueue_cycle = now;
    Ticket t = next_ticket++;
    jobs.emplace(t, j);
    TryStart(t, now);
    return t;
  }

  void TryStart(Ticket t, Time now) {
    Job& j = jobs.at(t);
    if (j.started || j.done) return;
    if (!port.Granted(j.lock)) return;
    j.started = true;
    j.grant_cycle = port.GrantCycle(j.lock);
    j.end_cycle = j.grant_cycle + ctx.P().bitmap_access_time;
  }

  CoreContext ctx;
  ExclusiveArbiter port;
  std::unordered_map<uint64_t, uint64_t> table;
  std::unordered_map<Ticket, Job> jobs;
  Ticket next_ticket = 1;
};

}
}

#endif
