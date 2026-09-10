#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_TASK_QUEUE_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_TASK_QUEUE_

// TaskQueue 与 Active Context：每个通道的读写两半各一份。
//
// 通道内顺序执行：TaskQueue 按序激活。read-ahead 允许 RD / WR 任务序号错位，
// 但不改变通道内的顺序。
//
// 通道之间可以乱序：某个 VC 阻塞只堵住对应的那个出核通道，别的通道照发。同一
// 通道内读写两半的状态也彼此独立，一侧的 Active Context 释放后就能激活下一个
// 任务，不等另一侧。
//
// issue_done 就允许该侧提前激活下一任务，不必等全部 drain。
//
// 这两样都只被所属 Lane 的 Step() 触碰，所以是普通类，不是 ClkModule。

#include <deque>

#include "base/log.h"
#include "bach/ip/chip/core/dte/dte_types.h"

namespace latch {
namespace bach {

// 一侧的在途上下文。
struct ActiveCtx {
  bool busy = false;
  Descriptor desc;
  uint64_t cur_addr = 0;
  uint64_t remain = 0;      // 还剩多少字节没发请求
  uint64_t outstanding = 0; // 已发出未回来的请求数
  bool issue_done = false;
  bool drained = false;
  uint64_t filled = 0;      // 出核读回来的数据已经填了多少字节进包
};

class TaskQueue {
 public:
  bool Full() const { return q.size() >= kTaskQueueDepth; }
  bool Empty() const { return q.empty(); }
  uint64_t Size() const { return q.size(); }

  void Push(Descriptor const& d) {
    LOGCHECK(!Full(), "DTE TaskQueue: 满了。Commit 应当先确认有空位再接纳。");
    q.push_back(d);
  }
  Descriptor const& Front() const { return q.front(); }
  void Pop() { q.pop_front(); }

 private:
  std::deque<Descriptor> q;
};

}  // namespace bach
}  // namespace latch

#endif
