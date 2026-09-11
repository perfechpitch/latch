#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_BUFFER_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_BUFFER_

// 中间 Buffer：读一半与写一半之间顶速度差的那块。
//
// inbound 与 outbound 合计约 8 KB，按 256 B × 20～30 拍算，最大可掩盖 32 T 的
// 延迟。满时通过 TREADY 向 Router 反压。
//
// 读这一半允许领先写那一半，领先量由三件事共同约束：这里的可用 Credit、读的
// outstanding 限额、可保留的任务边界数。出口阻塞只通过 Credit 反压限制领先距离，
// 不要求读写用同一个 Active Context。
//
// 按 task_id 分段存：写那一半按任务边界取数，所以边界信息要跟着数据走。
//
// 容量按通道分。出核那一块 buffer 被四个出核通道共用，但四个 Lane 各是一个独立
// 打拍的模块、各有自己的协程：让它们同一拍去改同一个 deque 就是两个线程改同一个
// 容器，跑久了会段错误。合起来算一个共享 Credit 池也不成：那样一拍最多只能让
// 一个通道拿到最后一格，要在四个通道之间仲裁，而通道之间本来就该能同时收响应。
// 所以每个通道拿这块容量的一份，各自的 Credit 各算各的，谁也不碰谁。

#include <array>
#include <deque>
#include <map>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/dte/dte_types.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// buffer 里的一拍。
struct BufBeat {
  // 这一拍属于哪一笔：进核是 Header Parser 的帧号，出核是 Commit 的内部序号。
  uint64_t tag = 0;
  uint64_t bytes = 0;
  bool last = false;      // 这个任务的最后一拍
  // 这一拍真正要落地的那几个字节。进核是从整包 payload 里切出来的那一段，
  // 出核是存储读回来的那一块。
  ByteBlockPtr data;
  MessagePtr msg;
};

class DteBuffer : public BachModule {
 public:
  // cap 是整块的容量，users 是分这块容量的通道数：进核那块只有一个通道用，
  // 出核那块四个出核通道各拿四分之一。
  DteBuffer(ClockPtr clock, const std::string& name, uint64_t cap,
            uint64_t users = 1, uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        depth(users == 0 ? cap : cap / users),
        occupancy(clock),
        pushed(clock),
        popped(clock) {}

  // 读那一半问还能不能往里放。这就是 F20 说的可用 Credit。
  bool HasRoom(uint64_t lane) const { return q[lane].size() < depth; }
  uint64_t Credit(uint64_t lane) const {
    return depth > q[lane].size() ? depth - q[lane].size() : 0;
  }

  void Push(uint64_t lane, BufBeat const& b) {
    LOGCHECK(HasRoom(lane),
             "DteBuffer: 满了。读那一半应当先看 Credit 再发请求。");
    q[lane].push_back(b);
    ++push_pending;
  }

  bool Empty(uint64_t lane) const { return q[lane].empty(); }
  BufBeat const& Front(uint64_t lane) const { return q[lane].front(); }
  void Pop(uint64_t lane) {
    LOGCHECK(!q[lane].empty(), "DteBuffer: 空的时候取数。");
    q[lane].pop_front();
    ++pop_pending;
  }

  // 某一笔的数据在 buffer 里排空了没有。写那一半要等这个才算 drained。
  // 一笔只归一个通道，所以只扫自己那一格。扫别的通道就是去读别人正在改的
  // 容器。
  bool TaskDrained(uint64_t lane, uint64_t tag) const {
    for (auto const& b : q[lane]) {
      if (b.tag == tag) return false;
    }
    return true;
  }

  uint64_t Occupancy() const { return occupancy.Get(); }
  uint64_t Pushed() const { return pushed.Get(); }
  uint64_t Popped() const { return popped.Get(); }

  bool Quiescent() const override {
    for (auto const& lane : q) {
      if (!lane.empty()) return false;
    }
    return true;
  }

 protected:
  void Step() override {
    // 观测量：各通道占用量之和。只有本模块的协程读这几个 size，Lane 那边改的
    // 是自己那一格；这一拍读到的水位可能落后一点，做观测够用。
    uint64_t sum = 0;
    for (auto const& lane : q) sum += lane.size();

    occupancy = sum;
    pushed = push_pending;
    popped = pop_pending;
    TracePerCycle("occupancy", sum);
  }

 private:
  uint64_t depth;   // 每个通道分到的深度
  std::array<std::deque<BufBeat>, kLaneNum> q;
  uint64_t push_pending = 0, pop_pending = 0;

  Logic64 occupancy, pushed, popped;
};

}  // namespace bach
}  // namespace latch

#endif
