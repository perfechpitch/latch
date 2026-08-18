#ifndef _LATCH_BACH_IP_CHIP_CORE_MEMORY_MEM_ARBITER_
#define _LATCH_BACH_IP_CHIP_CORE_MEMORY_MEM_ARBITER_

// 一块存储器的单端口仲裁。
//
// 物理存储器本身不建模，只建模它的端口：一次只服务一个接口，谁先来谁先占着，
// 一次服务的时长是基础寻址延迟加传输拍数。因此仲裁器就是这块存储器。
//
// 一次访问分两段，锁在两段之间不放开：
//
//   等锁      先进先出，等多久由前面排了几个决定
//   服务      base_delay 加 beats 拍，结束时释放锁
//
// 调用方不阻塞：Request 拿一个 Ticket，之后每拍问一次 Done，完成后调 Finish 清记录。
// 服务结束那一拍就释放锁，不等调用方来收，所以后一个请求方紧接着就能开始，与 Bach
// 里 with 块退出即唤醒队首的语义一致。

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/log.h"
#include "base/runtime.h"
#include "base/time_stamp.h"
#include "bach/common/arbiter.h"
#include "bach/common/packet.h"

namespace latch {
namespace bach {

class MemArbiter {
 public:
  MemArbiter(std::string mem_name, uint64_t base, uint64_t queue_depth = 0)
      : name(std::move(mem_name)),
        base_delay(base),
        fifo_capacity(queue_depth) {}

  // beats 是传输拍数，由调用它的接口按自己的带宽算好。这里不再碰带宽。
  Ticket Request(Time now, uint64_t uid, uint64_t tid, uint64_t beats) {
    Job j;
    j.lock = lock.Enqueue(now, uid);
    j.uid = uid;
    j.tid = tid;
    j.beats = beats;
    j.enqueue_cycle = now;
    Ticket t = next_ticket++;
    jobs.emplace(t, j);
    TryStart(t, now);
    return t;
  }

  bool Done(Ticket t) const { return Find(t).done; }
  Time GrantCycle(Ticket t) const { return Find(t).grant_cycle; }
  Time EnqueueCycle(Ticket t) const { return Find(t).enqueue_cycle; }
  Time EndCycle(Ticket t) const { return Find(t).end_cycle; }

  // 完成之后清记录。未完成的请求不允许撤销，一次访存发出去就一定会做完。
  void Finish(Ticket t) {
    auto it = jobs.find(t);
    LOGCHECK(it != jobs.end(), "MemArbiter: finish unknown ticket.");
    LOGCHECK(it->second.done, "MemArbiter: finish before done.");
    jobs.erase(it);
  }

  void Step() {
    const Time now = RT::Now();
    // 先收服务到点的，锁在这一拍放开
    for (auto& kv : jobs) {
      Job& j = kv.second;
      if (j.done || !j.started) continue;
      if (now >= j.end_cycle) {
        j.done = true;
        lock.Release(j.lock, now);
      }
    }
    // 再看队首能不能接着上，这样上面刚放开的锁本拍就能被下一个拿走
    for (auto& kv : jobs) TryStart(kv.first, now);
  }

  // ---------------------------------------------------------------- 用户队列
  //
  // 广播核把这块存储器当成一条 user 队列用。队列有三个位置，它们只增不减地往前走：
  //
  //   队尾    压进来的 user 排在这里，压栈本身要走一次访存
  //   已激活  同时被 TaskScheduler 认领的 user 数受 stream 数约束，激活到这里为止
  //   已弹出  已经被读出来发给下游的位置，弹出同样要走一次访存
  //
  // 退休时从队首整条移除，两个位置各往回退一格。压栈与弹出各占一次访存，激活与退休
  // 只动指针，不占时间：它们是控制动作，不搬数据。

  bool FifoFull() const {
    return fifo_capacity > 0 && fifo.size() >= fifo_capacity;
  }

  void FifoPush(CommInstPtr const& data, uint64_t size) {
    LOGCHECK(!FifoFull(), "MemArbiter: the user queue is full.");
    fifo.push_back(Entry{data, size});
  }

  // 认领一个还没被认领的，认领不到就返回空。
  CommInstPtr FifoActivateNext() {
    if (ack_ptr >= fifo.size()) return nullptr;
    return fifo[ack_ptr++].data;
  }

  // 队首待弹出的那个。已认领的才弹得出来。
  CommInstPtr FifoPeek() const {
    if (pop_ptr >= ack_ptr) return nullptr;
    return fifo[pop_ptr].data;
  }

  void FifoCommitPop() {
    LOGCHECK(pop_ptr < ack_ptr, "MemArbiter: popped past what was activated.");
    ++pop_ptr;
  }

  // 退休。只能退队首，退别人说明退休顺序乱了。
  CommInstPtr FifoRetire(uint64_t uid) {
    if (fifo.empty()) return nullptr;
    LOGCHECK(pop_ptr > 0, "MemArbiter: a user retires before it was sent out.");
    CommInstPtr head = fifo.front().data;
    LOGCHECK(head != nullptr && head->uid == uid,
             "MemArbiter: the user queue retires out of order.");
    fifo.erase(fifo.begin());
    --ack_ptr;
    --pop_ptr;
    return head;
  }

  uint64_t FifoSize() const { return fifo.size(); }
  uint64_t FifoActivated() const { return ack_ptr; }
  uint64_t FifoPopped() const { return pop_ptr; }

  std::string const& Name() const { return name; }
  uint64_t BaseDelay() const { return base_delay; }
  uint64_t QueueLen() const { return lock.QueueLen(); }
  bool Busy() const { return lock.Busy(); }
  uint64_t InFlight() const { return jobs.size(); }

 private:
  struct Job {
    Ticket lock = kNoTicket;
    uint64_t uid = 0;
    uint64_t tid = 0;
    uint64_t beats = 0;
    Time enqueue_cycle = 0;
    Time grant_cycle = 0;
    Time end_cycle = 0;
    bool started = false;
    bool done = false;
  };

  Job const& Find(Ticket t) const {
    auto it = jobs.find(t);
    LOGCHECK(it != jobs.end(), "MemArbiter: unknown ticket.");
    return it->second;
  }

  void TryStart(Ticket t, Time now) {
    Job& j = jobs.at(t);
    if (j.started || j.done) return;
    if (!lock.Granted(j.lock)) return;
    j.started = true;
    j.grant_cycle = lock.GrantCycle(j.lock);
    j.end_cycle = j.grant_cycle + base_delay + j.beats;
  }

  struct Entry {
    CommInstPtr data;
    uint64_t size = 0;
  };

  std::string name;
  uint64_t base_delay;
  uint64_t fifo_capacity;
  std::vector<Entry> fifo;
  uint64_t ack_ptr = 0;
  uint64_t pop_ptr = 0;
  ExclusiveArbiter lock;
  std::unordered_map<Ticket, Job> jobs;
  Ticket next_ticket = 1;
};

}
}

#endif
