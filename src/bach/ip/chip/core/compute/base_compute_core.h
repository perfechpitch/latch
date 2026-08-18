#ifndef _LATCH_BACH_IP_CHIP_CORE_COMPUTE_BASE_COMPUTE_CORE_
#define _LATCH_BACH_IP_CHIP_CORE_COMPUTE_BASE_COMPUTE_CORE_

// 计算核的共同骨架。MatrixCore 与 VectorCore 只在 setup 时长、优先级取法与完成前
// 的钩子上不同，一条任务在两者里走的阶段完全一样。
//
// 一条任务的六个阶段，前四个是排队与计时，后两个才是它自己的事：
//
//   准入      优先级队列，容量是 setup_ahead_depth 加 1。令牌一直持到整条任务做完，
//             所以它限的是"同时在流水线里的任务数"，不是"同时在 setup 的任务数"
//   setup     优先级队列，容量 1，占住 setup_time 拍，计时结束就放开
//   执行通道  先进先出，容量 1，从这里一直持到 ack
//   读倍率    向 MoEBitMap 要这个 user 的专家数，占用执行通道
//   计算      Map 给的时间乘倍率，倍率为 0 时按 1 算
//   ack       通知 TaskScheduler 这条任务完成
//
// 准入与 setup 分两级，是因为它们限的不是一回事：setup 是一段独占的配置时间，准入
// 是"允许提前配置几条"。合成一级就看不出哪一级在堵。
//
// 计算核不访存。它声明了访存口，但计算过程不发起访存，计算时间只来自 Map，与数据量
// 和内存带宽无关。这是继承自 Bach 的边界，不是本方案的简化。

#include <cstdint>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/log.h"
#include "base/runtime.h"
#include "base/time_stamp.h"
#include "bach/common/arbiter.h"
#include "bach/ip/chip/core/core_context.h"
#include "bach/ip/chip/core/moe_bitmap.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/sub_unit.h"

namespace latch {
namespace bach {

class BaseComputeCore : public SubUnit {
 public:
  BaseComputeCore(ClockPtr clock, CoreContext const& context, Unit kind,
                  std::string const& name, uint64_t parent, bool standalone)
      : SubUnit(clock, standalone),
        ctx(context),
        unit_kind(kind),
        admission(context.P().setup_ahead_depth + 1),
        setup(1) {
    RegisterId(name, parent);
  }

  void Connect(AckPort* scheduler, MoeBitMap* bitmap) {
    LOGCHECK(scheduler != nullptr, "ComputeCore: scheduler is null.");
    LOGCHECK(bitmap != nullptr, "ComputeCore: bitmap is null.");
    sched = scheduler;
    moe_bitmap = bitmap;
  }

  // TaskScheduler 派一条任务进来。它同拍就往前走到第一个走不动的地方，与 Bach 里
  // 调用方一 yield 就执行到第一个让出点是一样的。
  void Dispatch(Time now, uint64_t uid, uint64_t tid, uint64_t priority) {
    LOGCHECK(sched != nullptr, "ComputeCore: dispatch before connect.");
    // 完成回调会一路调到 TaskScheduler，它不该反过来再派任务进来。真发生了就是
    // 环形调用，这里当场拦住，而不是让 jobs 在推进途中被改。
    LOGCHECK(!advancing, "ComputeCore: dispatch reentered from a completion.");
    Job j;
    j.uid = uid;
    j.tid = tid;
    j.priority = priority;
    j.state = State::kAdmission;
    j.ticket = admission.Enqueue(now, priority, uid);
    jobs.push_back(j);
    RunAdvance(jobs.back(), now);
    Sweep();
  }

  void Step() override {
    const Time now = RT::Now();
    for (size_t i = 0; i < jobs.size(); ++i) RunAdvance(jobs[i], now);
    Sweep();
  }

  uint64_t InFlight() const { return jobs.size(); }
  bool Busy() const { return !jobs.empty(); }

 protected:
  enum class State : uint32_t {
    kAdmission = 0,
    kSetupQueue = 1,
    kSetup = 2,
    kLane = 3,
    kBitmap = 4,
    kCompute = 5,
    kFinished = 6,
  };

  struct Job {
    uint64_t uid = 0;
    uint64_t tid = 0;
    uint64_t priority = 0;
    State state = State::kAdmission;
    Ticket ticket = kNoTicket;      // 当前排队用的票
    Ticket admit = kNoTicket;       // 准入令牌，持到最后
    Ticket lane_ticket = kNoTicket; // 执行通道，持到 ack
    Ticket bitmap_ticket = kNoTicket;
    Time timer_end = 0;
    Time exec_start = 0;
    uint64_t multiplier = 1;
    uint64_t compute_cycles = 0;
  };

  virtual uint64_t SetupTime() const = 0;
  // 计算完成、ack 之前的钩子。它不许推进时间，否则用到它的任务会跟不用它的对不上。
  virtual void PreAck(Job const&) {}

  CoreContext ctx;

 private:
  void RunAdvance(Job& j, Time now) {
    advancing = true;
    Advance(j, now);
    advancing = false;
  }

  void Advance(Job& j, Time now) {
    bool moved = true;
    while (moved) {
      moved = false;
      switch (j.state) {
        case State::kAdmission:
          if (!admission.Granted(j.ticket)) break;
          ctx.Wait(unit_kind, j.uid, j.tid, admission.EnqueueCycle(j.ticket),
                   admission.GrantCycle(j.ticket), WaitReason::kSetupAhead);
          j.admit = j.ticket;
          j.ticket = setup.Enqueue(now, j.priority, j.uid);
          j.state = State::kSetupQueue;
          moved = true;
          break;

        case State::kSetupQueue:
          if (!setup.Granted(j.ticket)) break;
          ctx.Wait(unit_kind, j.uid, j.tid, setup.EnqueueCycle(j.ticket),
                   setup.GrantCycle(j.ticket), WaitReason::kCoreSetup);
          j.timer_end = setup.GrantCycle(j.ticket) + SetupTime();
          j.state = State::kSetup;
          moved = true;
          break;

        case State::kSetup:
          if (now < j.timer_end) break;
          ctx.Span(unit_kind, j.uid, j.tid, setup.GrantCycle(j.ticket),
                   j.timer_end, SpanState::kSetup);
          setup.Release(j.ticket, now);
          j.ticket = lane.Enqueue(now, j.uid);
          j.lane_ticket = j.ticket;
          j.state = State::kLane;
          moved = true;
          break;

        case State::kLane:
          if (!lane.Granted(j.lane_ticket)) break;
          ctx.Wait(unit_kind, j.uid, j.tid, lane.EnqueueCycle(j.lane_ticket),
                   lane.GrantCycle(j.lane_ticket), WaitReason::kExecutionLane);
          j.bitmap_ticket = moe_bitmap->BeginRead(now, j.uid, j.tid);
          j.state = State::kBitmap;
          moved = true;
          break;

        case State::kBitmap: {
          if (!moe_bitmap->Done(j.bitmap_ticket)) break;
          const uint64_t bitmap = moe_bitmap->Value(j.bitmap_ticket);
          moe_bitmap->Finish(j.bitmap_ticket);
          j.bitmap_ticket = kNoTicket;
          j.multiplier = bitmap != 0 ? bitmap : 1;
          j.compute_cycles = ctx.Task(j.tid).time_or_vol * j.multiplier;
          j.exec_start = now;
          j.timer_end = now + j.compute_cycles;
          j.state = State::kCompute;
          moved = true;
          break;
        }

        case State::kCompute:
          if (now < j.timer_end) break;
          PreAck(j);
          sched->Ack(j.uid, j.tid);
          ctx.Span(unit_kind, j.uid, j.tid, j.exec_start, j.timer_end,
                   SpanState::kCompute, j.compute_cycles);
          lane.Release(j.lane_ticket, now);
          admission.Release(j.admit, now);
          j.state = State::kFinished;
          moved = true;
          break;

        case State::kFinished:
          break;
      }
    }
  }

  void Sweep() {
    size_t keep = 0;
    for (size_t i = 0; i < jobs.size(); ++i) {
      if (jobs[i].state == State::kFinished) continue;
      if (keep != i) jobs[keep] = jobs[i];
      ++keep;
    }
    jobs.resize(keep);
  }

  Unit unit_kind;
  AckPort* sched = nullptr;
  MoeBitMap* moe_bitmap = nullptr;

  PriorityArbiter admission;
  PriorityArbiter setup;
  ExclusiveArbiter lane;
  std::vector<Job> jobs;
  bool advancing = false;
};

}
}

#endif
