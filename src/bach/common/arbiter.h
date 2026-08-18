#ifndef _LATCH_BACH_COMMON_ARBITER_
#define _LATCH_BACH_COMMON_ARBITER_

// Bach 用到的三种排队语义。
//
//   ExclusiveArbiter  容量 1 的独占资源，等待队列先进先出
//   PriorityArbiter   容量 N 的资源，等待队列按 (优先级, 入队序) 排，小者先
//   CreditCounter     计数信号量，get 队列先进先出且队头阻塞
//
// 三者都不阻塞调用方。调用方是跨拍状态机：Enqueue 拿一个 Ticket，之后每拍问一次
// Granted，拿到了才往下走。Cycle 体内不允许再让出，所以不存在阻塞式获取。
//
// 授予时机跟着状态变化走，不等到下一拍：Enqueue、Release、Put 之后各跑一次派发到
// 不动点。因此本拍前一个 stage 释放的资源，本拍后一个 stage 就能拿到，与 Bach 里
// 释放当刻唤醒队首的语义一致。谁先谁后由 Core 的 stage 顺序决定。
//
// 排序键里的入队序是一个单调递增的计数器，它同时蕴含了请求时刻的先后与同一拍内
// 的插入顺序，所以不需要再单独记请求时刻。请求的入队时刻是调 Enqueue 那一刻，不
// 是拿到那一刻，这一条与 Bach 一致。
//
// 等待归因需要的两个时刻记在条目里：入队拍与授予拍，差值就是这一段等待的时长。

#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/log.h"
#include "base/time_stamp.h"

namespace latch {
namespace bach {

using Ticket = uint64_t;
constexpr Ticket kNoTicket = 0;

// ---------------------------------------------------------------- 独占资源

// 对应 Bach 里容量 1 的 simpy.Resource：DTE 的两条执行通道、FIFO 与 reduction 的
// 功能单元、CreditUnit 的查账锁、内存仲裁器、MoEBitMap 的访问口。
class ExclusiveArbiter {
 public:
  ExclusiveArbiter() = default;

  // owner 只用于诊断，模型不读它。
  Ticket Enqueue(Time now, uint64_t owner = 0) {
    Ticket t = next_ticket++;
    Entry e;
    e.owner = owner;
    e.enqueue_cycle = now;
    entries.emplace(t, e);
    waiting.push_back(t);
    Dispatch(now);
    return t;
  }

  bool Granted(Ticket t) const {
    auto it = entries.find(t);
    LOGCHECK(it != entries.end(), "ExclusiveArbiter: unknown ticket.");
    return it->second.granted;
  }

  void Release(Ticket t, Time now) {
    auto it = entries.find(t);
    LOGCHECK(it != entries.end(), "ExclusiveArbiter: release unknown ticket.");
    LOGCHECK(it->second.granted, "ExclusiveArbiter: release before grant.");
    LOGCHECK(holder == t, "ExclusiveArbiter: release by non-holder.");
    entries.erase(it);
    holder = kNoTicket;
    Dispatch(now);
  }

  // 只能撤销尚未授予的请求，已授予的要走 Release。
  void Cancel(Ticket t) {
    auto it = entries.find(t);
    LOGCHECK(it != entries.end(), "ExclusiveArbiter: cancel unknown ticket.");
    LOGCHECK(!it->second.granted, "ExclusiveArbiter: cancel after grant.");
    for (auto q = waiting.begin(); q != waiting.end(); ++q) {
      if (*q == t) {
        waiting.erase(q);
        break;
      }
    }
    entries.erase(it);
  }

  bool Busy() const { return holder != kNoTicket; }
  uint64_t QueueLen() const { return waiting.size(); }
  Time EnqueueCycle(Ticket t) const { return Find(t).enqueue_cycle; }
  Time GrantCycle(Ticket t) const { return Find(t).grant_cycle; }
  Time WaitCycles(Ticket t) const {
    Entry const& e = Find(t);
    LOGCHECK(e.granted, "ExclusiveArbiter: wait length before grant.");
    return e.grant_cycle - e.enqueue_cycle;
  }

 private:
  struct Entry {
    uint64_t owner = 0;
    Time enqueue_cycle = 0;
    Time grant_cycle = 0;
    bool granted = false;
  };

  Entry const& Find(Ticket t) const {
    auto it = entries.find(t);
    LOGCHECK(it != entries.end(), "ExclusiveArbiter: unknown ticket.");
    return it->second;
  }

  // 一次释放只放行队首一个，与 simpy.Resource 一致。
  void Dispatch(Time now) {
    if (holder != kNoTicket || waiting.empty()) return;
    Ticket t = waiting.front();
    waiting.pop_front();
    Entry& e = entries.at(t);
    e.granted = true;
    e.grant_cycle = now;
    holder = t;
  }

  std::unordered_map<Ticket, Entry> entries;
  std::deque<Ticket> waiting;
  Ticket holder = kNoTicket;
  Ticket next_ticket = 1;
};

// ---------------------------------------------------------------- 优先级资源

// 对应 simpy.PriorityResource：准入令牌（容量为 setup_ahead_depth 加 1）与 setup
// 单元（容量 1）。优先级取调用方给的 StreamID，数值小者先。
class PriorityArbiter {
 public:
  explicit PriorityArbiter(uint32_t cap = 1) : capacity(cap) {
    LOGCHECK(cap > 0, "PriorityArbiter: capacity must be positive.");
  }

  Ticket Enqueue(Time now, uint64_t priority, uint64_t owner = 0) {
    Ticket t = next_ticket++;
    Entry e;
    e.owner = owner;
    e.enqueue_cycle = now;
    e.key = {priority, next_seq++};
    entries.emplace(t, e);
    waiting.emplace(e.key, t);
    Dispatch(now);
    return t;
  }

  bool Granted(Ticket t) const { return Find(t).granted; }

  void Release(Ticket t, Time now) {
    auto it = entries.find(t);
    LOGCHECK(it != entries.end(), "PriorityArbiter: release unknown ticket.");
    LOGCHECK(it->second.granted, "PriorityArbiter: release before grant.");
    entries.erase(it);
    LOGCHECK(held > 0, "PriorityArbiter: release with nothing held.");
    --held;
    Dispatch(now);
  }

  void Cancel(Ticket t) {
    auto it = entries.find(t);
    LOGCHECK(it != entries.end(), "PriorityArbiter: cancel unknown ticket.");
    LOGCHECK(!it->second.granted, "PriorityArbiter: cancel after grant.");
    waiting.erase(it->second.key);
    entries.erase(it);
  }

  uint32_t Capacity() const { return capacity; }
  uint32_t Held() const { return held; }
  uint64_t QueueLen() const { return waiting.size(); }
  Time EnqueueCycle(Ticket t) const { return Find(t).enqueue_cycle; }
  Time GrantCycle(Ticket t) const { return Find(t).grant_cycle; }
  Time WaitCycles(Ticket t) const {
    Entry const& e = Find(t);
    LOGCHECK(e.granted, "PriorityArbiter: wait length before grant.");
    return e.grant_cycle - e.enqueue_cycle;
  }

 private:
  using Key = std::pair<uint64_t, uint64_t>;  // (优先级, 入队序)

  struct Entry {
    uint64_t owner = 0;
    Key key{0, 0};
    Time enqueue_cycle = 0;
    Time grant_cycle = 0;
    bool granted = false;
  };

  Entry const& Find(Ticket t) const {
    auto it = entries.find(t);
    LOGCHECK(it != entries.end(), "PriorityArbiter: unknown ticket.");
    return it->second;
  }

  void Dispatch(Time now) {
    while (held < capacity && !waiting.empty()) {
      auto q = waiting.begin();
      Ticket t = q->second;
      waiting.erase(q);
      Entry& e = entries.at(t);
      e.granted = true;
      e.grant_cycle = now;
      ++held;
    }
  }

  std::map<Key, Ticket> waiting;
  std::unordered_map<Ticket, Entry> entries;
  uint32_t capacity;
  uint32_t held = 0;
  uint64_t next_seq = 0;
  Ticket next_ticket = 1;
};

// ---------------------------------------------------------------- 计数信号量

// 对应 simpy.Container：Host 的 credit、CreditUnit 的每个下游账户、stream 槽位池。
// get 队列先进先出且队头阻塞，队首要不到的时候，后面即使能满足也不提前放行。
//
// 与前两者不同，额度在授予那一刻就扣掉了，没有归还给本次请求这回事。请求方拿到
// 之后调 Drop 清掉记录即可，真正的归还是上游另一次 Put。
class CreditCounter {
 public:
  CreditCounter(uint64_t init_level, uint64_t cap)
      : level(init_level), capacity(cap) {
    LOGCHECK(init_level <= cap, "CreditCounter: init level exceeds capacity.");
  }

  Ticket Get(Time now, uint64_t amount = 1) {
    LOGCHECK(amount > 0, "CreditCounter: get amount must be positive.");
    LOGCHECK(amount <= capacity,
             "CreditCounter: get amount exceeds capacity, would never grant.");
    Ticket t = next_ticket++;
    Entry e;
    e.amount = amount;
    e.enqueue_cycle = now;
    entries.emplace(t, e);
    waiting.push_back(t);
    Dispatch(now);
    return t;
  }

  bool Granted(Ticket t) const { return Find(t).granted; }

  // 还回来的额度超过容量时不报错，先挂着，等有空位再补进去。
  //
  // 这不是防御式的宽容，是 Bach 的语义：那边的 put 是一个会挂起的事件，还不进去就
  // 一直等着。它出现在 credit 图与任务表对不齐的地方：某个核收得到下游的退休信号，
  // 却没有一条验资任务扣过那份额度。照搬这条，是因为改成报错会让本来跑得通的 Map
  // 跑不了，而那属于 Map 的问题，不是这一层该替它拿主意的。
  void Put(Time now, uint64_t amount = 1) {
    pending += amount;
    Dispatch(now);
  }

  // 已授予的请求用完之后清记录；未授予的请求撤回。
  void Drop(Ticket t) {
    auto it = entries.find(t);
    LOGCHECK(it != entries.end(), "CreditCounter: drop unknown ticket.");
    if (!it->second.granted) {
      for (auto q = waiting.begin(); q != waiting.end(); ++q) {
        if (*q == t) {
          waiting.erase(q);
          break;
        }
      }
    }
    entries.erase(it);
  }

  uint64_t Level() const { return level; }
  uint64_t Capacity() const { return capacity; }
  // 还回来但暂时放不下的那部分。它不为零就说明这个账户上收到的归还比扣掉的多。
  uint64_t PendingPut() const { return pending; }
  uint64_t QueueLen() const { return waiting.size(); }
  Time EnqueueCycle(Ticket t) const { return Find(t).enqueue_cycle; }
  Time GrantCycle(Ticket t) const { return Find(t).grant_cycle; }
  Time WaitCycles(Ticket t) const {
    Entry const& e = Find(t);
    LOGCHECK(e.granted, "CreditCounter: wait length before grant.");
    return e.grant_cycle - e.enqueue_cycle;
  }

 private:
  struct Entry {
    uint64_t amount = 0;
    Time enqueue_cycle = 0;
    Time grant_cycle = 0;
    bool granted = false;
  };

  Entry const& Find(Ticket t) const {
    auto it = entries.find(t);
    LOGCHECK(it != entries.end(), "CreditCounter: unknown ticket.");
    return it->second;
  }

  void Dispatch(Time now) {
    for (;;) {
      while (pending > 0 && level < capacity) {
        ++level;
        --pending;
      }
      if (waiting.empty()) return;
      Entry& e = entries.at(waiting.front());
      if (e.amount > level) return;  // 队头阻塞，后面的不越过
      level -= e.amount;
      e.granted = true;
      e.grant_cycle = now;
      waiting.pop_front();
    }
  }

  std::unordered_map<Ticket, Entry> entries;
  std::deque<Ticket> waiting;
  uint64_t level;
  uint64_t capacity;
  uint64_t pending = 0;
  Ticket next_ticket = 1;
};

// ---------------------------------------------------------------- 多下游验资

// 对应 Bach 的 env.all_of([c.get(1) for c in containers])：一条 CU 任务要向多个
// 下游同时验资。各账户独立授予，先到手的先扣住，再继续等其余，所以一个下游堵住
// 会把已扣的额度一直占着。这不是可以优化掉的实现细节，它决定了单点拥塞会放大成
// 扇出范围内的同步停顿。
class CreditGetGroup {
 public:
  void Add(CreditCounter* counter, Ticket t) {
    LOGCHECK(counter != nullptr, "CreditGetGroup: null counter.");
    items.emplace_back(counter, t);
  }

  bool Empty() const { return items.empty(); }

  bool AllGranted() const {
    for (auto const& it : items) {
      if (!it.first->Granted(it.second)) return false;
    }
    return true;
  }

  // 全部到手的那一拍，即各账户授予拍的最大值。
  Time GrantCycle() const {
    LOGCHECK(AllGranted(), "CreditGetGroup: grant cycle before all granted.");
    Time last = 0;
    for (auto const& it : items) {
      Time g = it.first->GrantCycle(it.second);
      if (g > last) last = g;
    }
    return last;
  }

  void Drop() {
    for (auto const& it : items) it.first->Drop(it.second);
    items.clear();
  }

 private:
  std::vector<std::pair<CreditCounter*, Ticket>> items;
};

}
}

#endif
