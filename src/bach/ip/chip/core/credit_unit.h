#ifndef _LATCH_BACH_IP_CHIP_CORE_CREDIT_UNIT_
#define _LATCH_BACH_IP_CHIP_CORE_CREDIT_UNIT_

// 下游流控。本核每往下游送一个 user，先在这里扣一份额度；下游那个 user 退休时把
// 额度还回来。这是整个模型里唯一的反压来源之一，另一个是 stream 槽位。
//
// 一条 CU 任务的三段：
//
//   查账锁    容量 1，先进先出。锁在整个验资期间不放开，所以一条任务卡在等下游额度
//             时，后面的 CU 任务连查都查不了。这是有意的：账本一次只允许一个任务动。
//   查账      固定 4 拍
//   扣减      向 credit 表列出的每个下游各要一份，全部到手才放行
//
// 多下游是各账户独立授予，先到手的先扣住再等其余。所以一个下游堵住，会把已经扣到的
// 那几份一直占着。这不是可以省掉的实现细节，它决定了单点拥塞会放大成扇出范围内的
// 同步停顿。
//
// 归还不排队也不占时间：DTE 收到 RETIRE 包就地把额度放回账户，本拍就可能唤醒正在等
// 它的那条 CU 任务。

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/clock.h"
#include "base/log.h"
#include "base/runtime.h"
#include "base/time_stamp.h"
#include "bach/common/arbiter.h"
#include "bach/ip/chip/core/core_context.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/sub_unit.h"

namespace latch {
namespace bach {

class CreditUnit : public SubUnit, public CreditReturnPort {
 public:
  CreditUnit(ClockPtr clock, CoreContext const& context,
             std::string const& name, uint64_t parent, bool standalone = false)
      : SubUnit(clock, standalone), ctx(context) {
    RegisterId(name, parent);
  }

  void Connect(AckPort* scheduler) {
    LOGCHECK(scheduler != nullptr, "CreditUnit: scheduler is null.");
    sched = scheduler;
  }

  // 开户。同一个下游重复开户只允许额度一致，额度不同说明装配算错了。
  void AddDownstream(int64_t core_id, uint64_t initial_credit,
                     CoreType core_type) {
    auto it = accounts.find(core_id);
    if (it != accounts.end()) {
      LOGCHECK(it->second->Capacity() == initial_credit,
               "CreditUnit: downstream reopened with another capacity.");
      LOGCHECK(types.at(core_id) == core_type,
               "CreditUnit: downstream reopened with another core type.");
      return;
    }
    accounts.emplace(core_id,
                     std::make_unique<CreditCounter>(initial_credit, initial_credit));
    types.emplace(core_id, core_type);
  }

  bool HasDownstream(int64_t core_id) const {
    return accounts.count(core_id) != 0;
  }

  uint64_t Level(int64_t core_id) const {
    auto it = accounts.find(core_id);
    LOGCHECK(it != accounts.end(), "CreditUnit: no such downstream account.");
    return it->second->Level();
  }

  uint64_t DownstreamNum() const { return accounts.size(); }

  // TaskScheduler 派一条 CU 任务进来。
  void Dispatch(Time now, uint64_t uid, uint64_t tid) {
    LOGCHECK(sched != nullptr, "CreditUnit: dispatch before connect.");
    LOGCHECK(!advancing, "CreditUnit: dispatch reentered from a completion.");
    LOGCHECK(!accounts.empty(),
             "CreditUnit: no downstream account, cannot serve a CU task.");
    jobs.emplace_back();
    Job& j = jobs.back();
    j.uid = uid;
    j.tid = tid;
    j.state = State::kLockQueue;
    j.lock_ticket = lock.Enqueue(now, uid);
    RunAdvance(jobs.size() - 1, now);
    Sweep();
  }

  // DTE 收到 RETIRE 包时调它。
  void ReturnCredit(int64_t core_id, uint64_t amount) override {
    auto it = accounts.find(core_id);
    LOGCHECK(it != accounts.end(),
             "CreditUnit: return credit to an unknown downstream.");
    it->second->Put(RT::Now(), amount);
  }

  void Step() override {
    const Time now = RT::Now();
    for (size_t i = 0; i < jobs.size(); ++i) RunAdvance(i, now);
    Sweep();
  }

  uint64_t InFlight() const { return jobs.size(); }

 private:
  enum class State : uint32_t {
    kLockQueue = 0,
    kCheck = 1,
    kCreditWait = 2,
    kFinished = 3,
  };

  struct Job {
    uint64_t uid = 0;
    uint64_t tid = 0;
    State state = State::kLockQueue;
    Ticket lock_ticket = kNoTicket;
    Time lock_grant = 0;
    Time check_end = 0;
    Time credit_start = 0;
    CreditGetGroup group;
  };

  void RunAdvance(size_t index, Time now) {
    advancing = true;
    Advance(jobs[index], now);
    advancing = false;
  }

  void Advance(Job& j, Time now) {
    bool moved = true;
    while (moved) {
      moved = false;
      switch (j.state) {
        case State::kLockQueue:
          if (!lock.Granted(j.lock_ticket)) break;
          j.lock_grant = lock.GrantCycle(j.lock_ticket);
          ctx.Wait(Unit::kCredit, j.uid, j.tid, lock.EnqueueCycle(j.lock_ticket),
                   j.lock_grant, WaitReason::kCreditLock);
          j.check_end = j.lock_grant + ctx.P().credit_check_time;
          j.state = State::kCheck;
          moved = true;
          break;

        case State::kCheck: {
          if (now < j.check_end) break;
          std::vector<int64_t> const* targets = ctx.CreditTargets(j.tid);
          LOGCHECK(targets != nullptr && !targets->empty(),
                   "CreditUnit: a CU task has no credit targets.");
          for (int64_t id : *targets) {
            auto it = accounts.find(id);
            LOGCHECK(it != accounts.end(),
                     "CreditUnit: task requests an unknown downstream credit.");
            j.group.Add(it->second.get(), it->second->Get(now, 1));
          }
          j.credit_start = now;
          j.state = State::kCreditWait;
          moved = true;
          break;
        }

        case State::kCreditWait:
          if (!j.group.AllGranted()) break;
          ctx.Wait(Unit::kCredit, j.uid, j.tid, j.credit_start,
                   j.group.GrantCycle(), WaitReason::kDownstreamCredit);
          j.group.Drop();
          sched->Ack(j.uid, j.tid);
          ctx.Span(Unit::kCredit, j.uid, j.tid, j.lock_grant, now,
                   SpanState::kFunction);
          lock.Release(j.lock_ticket, now);
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
      if (keep != i) jobs[keep] = std::move(jobs[i]);
      ++keep;
    }
    jobs.resize(keep);
  }

  CoreContext ctx;
  AckPort* sched = nullptr;

  ExclusiveArbiter lock;
  std::unordered_map<int64_t, std::unique_ptr<CreditCounter>> accounts;
  std::unordered_map<int64_t, CoreType> types;
  std::vector<Job> jobs;
  bool advancing = false;
};

}
}

#endif
