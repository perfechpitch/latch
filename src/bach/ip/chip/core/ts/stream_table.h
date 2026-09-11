#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_STREAM_TABLE_
#define _LATCH_BACH_IP_CHIP_CORE_TS_STREAM_TABLE_

// Stream_table：16 项顺序 FIFO，每项对应一条完整用户业务流。
//
// head_ptr 与 tail_ptr 环形推进：建表推 tail，退休推 head。
//
// 六个写口按固定优先级仲裁，每口一拍一笔，请求保持到 accepted 才算生效。优先级
// 由高到低是 retirement、completion、install、issue、credit_wake、create，让
// 表项先腾空再填新的，回收类排在生成类前面，create 排最后，队头卡住时不会因为
// 新用户不断插队而饿死。
//
// 落到不同 stream 的写互不相干，同一拍可以都做；落到同一个 stream 的按优先级排，
// 没轮上的不回 accepted，请求方下一拍再来。
//
// 表内容每拍末发布成一份快照。别的模块读上一拍的快照来做判断，不直接碰这里的
// 容器：那样是跨线程读非 atomic 容器，读数取决于两个协程谁先跑。

#include <array>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class StreamTable : public BachModule {
 public:
  // 表深固定 kStreamNum 项，是物理表的规模。本次用其中几项由软件配 CFG_REG，
  // 判满的人自己去读那个值，表这边不管。
  StreamTable(ClockPtr clock, const std::string& name, uint64_t parent = 0,
              bool tick = true)
      : BachModule(clock, name, parent, tick),
        snap_port(std::make_shared<SnapshotPort>(clock)),
        in_flight(clock),
        writes(clock),
        conflicts(clock) {
    for (uint64_t p = 0; p < kWrPortNum; ++p) {
      ports.push_back(std::make_shared<StreamWritePortIf>(clock));
    }
    // 复位后先发一份空快照，免得第一拍读到空指针。
    snap_port->Drive(MakeSnapshot());
  }

  StreamWritePortIf& Port(uint64_t p) { return *ports.at(p); }
  // 装配层把请求方那一侧的口接过来：一个写口是同一个对象的两端。
  void Rebind(uint64_t p, std::shared_ptr<StreamWritePortIf> port) {
    ports.at(p) = std::move(port);
  }
  std::shared_ptr<StreamWritePortIf> PortPtr(uint64_t p) const {
    return ports.at(p);
  }
  std::shared_ptr<SnapshotPort> SnapPtr() const { return snap_port; }

  // 自启动的 core：复位后直接建满表项，不等 Router trigger。此时还没有用户信息，
  // 等自启动任务的 RV core 返回 user_id 后再补进表项。
  void SelfStart(TaskEntry const& task0, uint64_t n) {
    for (uint64_t i = 0; i < n; ++i) {
      StreamEntry& e = table[i];
      e = StreamEntry{};
      e.valid = true;
      e.user_id_vld = false;
      e.task_id = 0;
      ApplyTaskAttr(e, task0);
      e.task_fsm = InitFsmOf(task0);
    }
    tail_ptr = n;
    snap_port->Drive(MakeSnapshot());
  }

  // JoinAll 之后主线程读，验收用。
  StreamEntry const& Peek(uint64_t i) const { return table.at(i); }
  uint64_t HeadPtr() const { return head_ptr; }
  uint64_t TailPtr() const { return tail_ptr; }
  // 本拍末表里还剩几项没退休。head_ptr 与 tail_ptr 是只由本模块 Step() 触碰的
  // 普通成员，别的协程要判这个 core 的任务链走没走空，读这个打拍的值。
  uint64_t InFlight() const { return in_flight.Get(); }
  uint64_t Writes() const { return writes.Get(); }
  uint64_t Conflicts() const { return conflicts.Get(); }

  bool Quiescent() const override {
    for (uint64_t i = 0; i < kStreamNum; ++i) {
      if (table[i].valid) return false;
    }
    return true;
  }

 protected:
  void Step() override {
    Arbitrate();
    snap_port->Drive(MakeSnapshot());
    in_flight = tail_ptr - head_ptr;
    writes = write_pending;
    conflicts = conflict_pending;
    TracePerCycle("in_flight", tail_ptr - head_ptr);
    TracePerCycle("writes", write_pending);
  }

 private:
  void Arbitrate() {
    // 每个 stream 本拍最多被写一次，先到的优先级高的赢。
    std::array<bool, kStreamNum> taken{};
    std::array<bool, kWrPortNum> served{};

    for (uint64_t p = 0; p < kWrPortNum; ++p) {
      if (!ports[p]->Valid()) continue;
      // 同一笔请求会连着两拍出现在端口上，按序号认它，不执行两遍。
      if (ports[p]->Seq() == last_seq[p]) continue;
      auto w = ports[p]->Req();
      if (!w || !w->valid) continue;
      uint64_t s = w->stream_id;
      LOGCHECK(s < kStreamNum, "StreamTable: stream_id 越界。");
      if (taken[s]) {
        // 同一个 stream 上被更高优先级的口占了，这一笔下一拍再来。
        ++conflict_pending;
        continue;
      }
      Apply(p, *w);
      last_seq[p] = ports[p]->Seq();
      taken[s] = true;
      served[p] = true;
      ++write_pending;
    }

    for (uint64_t p = 0; p < kWrPortNum; ++p) {
      ports[p]->DriveAccepted(served[p]);
    }
  }

  void Apply(uint64_t port, StreamWrite const& w) {
    StreamEntry& e = table[w.stream_id];
    if (w.whole) {
      e = w.entry;
      if (port == kWrCreate) tail_ptr = (tail_ptr + 1) % (2 * kStreamNum);
      return;
    }
    if (w.clear_valid) {
      // 退休：清 valid 并推 head_ptr。
      e.valid = false;
      e.user_id_vld = false;
      head_ptr = (head_ptr + 1) % (2 * kStreamNum);
      return;
    }
    if (w.set_done_bit) {
      // done_bitmap 无条件置位。
      e.done_bitmap |= 1ull << w.done_bit;
    }
    if (w.set_user_id) {
      e.user_id = w.user_id;
      e.user_id_vld = true;
    }
    if (w.set_reissue) e.reissue = true;
    if (w.clear_reissue) e.reissue = false;
    if (w.take_rmem) e.rmem_busy = true;
    if (w.give_rmem) e.rmem_busy = false;
    if (w.set_pid) {
      e.task_path_id = w.pid;
      e.pid_pending = true;
    }
    if (w.done_mask != 0) {
      // 跳过的几项并进完成位；含当前任务的，当前任务同时算做完。
      e.done_bitmap |= w.done_mask;
      if ((w.done_mask >> e.task_id) & 1u) e.task_fsm = TaskFsm::kFinish;
    }
    if (w.set_fsm) {
      // completion 口写两样，但改 task_fsm 有条件：只有这一笔的 task_id 等于该
      // stream 当前的 task_id 时才改。异步 datain 提前完成落在这个分支上：
      // 只亮一位，不动状态机。
      if (!w.fsm_if_current || w.from_task_id == e.task_id) {
        e.task_fsm = w.fsm;
      }
    }
  }

  StreamSnapshotPtr MakeSnapshot() const {
    auto s = std::make_shared<StreamSnapshot>();
    s->entry = table;
    s->head_ptr = head_ptr % kStreamNum;
    // 环形指针用 2 倍模保留满与空的区分，快照里换算成实际在途数单独带出来：
    // 两个指针取模之后满与空都是相等，差值分不开。
    s->in_flight = (tail_ptr + 2 * kStreamNum - head_ptr) % (2 * kStreamNum);
    s->tail_ptr = (s->head_ptr + s->in_flight) % kStreamNum;
    return s;
  }

  std::vector<std::shared_ptr<StreamWritePortIf>> ports;
  std::shared_ptr<SnapshotPort> snap_port;

  // Step 独占。
  std::array<StreamEntry, kStreamNum> table{};
  uint64_t head_ptr = 0, tail_ptr = 0;
  std::array<uint64_t, kWrPortNum> last_seq{};
  uint64_t write_pending = 0, conflict_pending = 0;

  Logic64 in_flight, writes, conflicts;
};

}  // namespace bach
}  // namespace latch

#endif
