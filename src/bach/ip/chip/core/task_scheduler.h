#ifndef _LATCH_BACH_IP_CHIP_CORE_TASK_SCHEDULER_
#define _LATCH_BACH_IP_CHIP_CORE_TASK_SCHEDULER_

// 顺序取指。一个 user 在本核上的整张任务表由它一条条往下派，派完就退休。
//
// 它同时管着三件互相牵制的事：
//
//   活跃队列  进来的 user 按到达顺序排队，队里的位置就是它的 StreamID。硬件规定
//             StreamID 小的必须先退休，所以退休只能发生在队首，别的位置想退就是
//             逻辑违例，当场断言
//   槽位      物理槽位数与队列长度都等于 stream 数，但两者不同步：退休时先出队，
//             那条流水走完才把槽位还回去
//   两条等待  一条任务要动，得等自己前面的任务都做完，还得等前一个 StreamID 的对应
//             任务做完。后者让工作沿对角线推进：同一条任务在不同 user 之间保持顺序，
//             不同任务在同一个 user 上保持顺序
//
// 前一个 StreamID 要等的那条任务不是同一条：普通任务等它的前一条，RETIRE 等它自己。
// 差这一条，退休才能严格按 StreamID 顺序发生，而普通任务可以重叠。
//
// 每一轮取指固定花 ts_logic_time 拍，这段时间与等待无关，是取指本身的开销。
//
// SKIP 行不派给任何单元。屏障 SKIP（REDUCE、REDUCTION、CONCAT、RES）是接收侧的占位：
// 走到它就挂起，等 DTE 收到对端的包来 ack；其余 SKIP 行本地立即完成。

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/clock.h"
#include "base/log.h"
#include "base/runtime.h"
#include "base/time_stamp.h"
#include "bach/common/arbiter.h"
#include "bach/ip/chip/core/compute/matrix_core.h"
#include "bach/ip/chip/core/compute/vector_core.h"
#include "bach/ip/chip/core/core_context.h"
#include "bach/ip/chip/core/credit_unit.h"
#include "bach/ip/chip/core/dte/dte.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/sub_unit.h"

namespace latch {
namespace bach {

class TaskScheduler : public SubUnit, public SchedulerPort {
 public:
  TaskScheduler(ClockPtr clock, CoreContext const& context,
                std::string const& name, uint64_t parent,
                bool standalone = false)
      : SubUnit(clock, standalone),
        ctx(context),
        stream_pool(context.P().stream_count, context.P().stream_count) {
    RegisterId(name, parent);
  }

  void Connect(Dte* dte_unit, MatrixCore* matrix, VectorCore* vector,
               CreditUnit* credit) {
    LOGCHECK(dte_unit != nullptr, "TaskScheduler: dte is null.");
    LOGCHECK(matrix != nullptr, "TaskScheduler: matrix core is null.");
    LOGCHECK(vector != nullptr, "TaskScheduler: vector core is null.");
    LOGCHECK(credit != nullptr, "TaskScheduler: credit unit is null.");
    dte = dte_unit;
    mc = matrix;
    vc = vector;
    cu = credit;
  }

  // ------------------------------------------------------------ 被调用的口

  void Ack(uint64_t uid, uint64_t tid) override {
    LOGCHECK(ctx.HasTask(tid), "TaskScheduler: ack an unknown task id.");
    TaskEntry const& e = ctx.Task(tid);

    if (e.unit == UnitType::kSkip) {
      if (e.opcode == Opcode::kUserInit || e.opcode == Opcode::kFifoIn) {
        AdmitUser(uid, !HasAnyCompleted(uid));
      } else if (e.opcode == Opcode::kRes) {
        if (ctx.Meta(tid).recv_init) AdmitUser(uid, !HasAnyCompleted(uid));
      } else {
        LOGCHECK(IsBarrierOpcode(e.opcode),
                 "TaskScheduler: unexpected ack on a SKIP row.");
      }
    } else if (e.unit == UnitType::kDte && e.opcode == Opcode::kRetire) {
      Retire(uid);
    }

    MarkComplete(uid, tid);
  }

  uint64_t StreamPriority(uint64_t uid, bool unknown_to_tail) const override {
    for (uint64_t i = 0; i < active_users.size(); ++i) {
      if (active_users[i] == uid) return i;
    }
    LOGCHECK(unknown_to_tail, "TaskScheduler: user has no stream priority.");
    return active_users.size();
  }

  // reduction 的首包到达时，这个 user 的 Task 0 视为已经完成，直接开一条流水。
  void AdmitWithPrecompletedTask(uint64_t uid) override {
    AdmitUser(uid, false);
    MarkComplete(uid, 0);
  }

  // ------------------------------------------------------------ 推进

  void Step() override {
    const Time now = RT::Now();
    for (size_t i = 0; i < flows.size(); ++i) Advance(flows[i], now);
    Sweep();
  }

  uint64_t ActiveUserNum() const { return active_users.size(); }
  uint64_t FlowNum() const { return flows.size(); }
  uint64_t FreeSlotNum() const { return stream_pool.Level(); }
  bool IsActive(uint64_t uid) const {
    for (uint64_t u : active_users) {
      if (u == uid) return true;
    }
    return false;
  }
  bool IsTaskDone(uint64_t uid, uint64_t tid) const {
    auto it = done.find(uid);
    return it != done.end() && it->second.count(tid) != 0;
  }

 private:
  enum class Stage : uint32_t {
    kSlotQueue = 0,
    kDecode = 1,
    kOwnPrev = 2,
    kStreamPred = 3,
    kDispatch = 4,
    kWaitAck = 5,
    kBarrier = 6,
    kFinished = 7,
  };

  struct Flow {
    uint64_t uid = 0;
    uint64_t tid = 0;
    Stage stage = Stage::kSlotQueue;
    Ticket slot = kNoTicket;
    Time timer_end = 0;
    Time wait_start = 0;
    uint64_t stream_id = 0;
    uint64_t pred_uid = 0;
    uint64_t pred_tid = 0;
  };

  // ------------------------------------------------------------ 队列与完成表

  void AdmitUser(uint64_t uid, bool clear_task_state) {
    LOGCHECK(active_users.size() < ctx.P().stream_count,
             "TaskScheduler: stream queue overflow.");
    LOGCHECK(!IsActive(uid), "TaskScheduler: user is already active.");
    if (clear_task_state) done.erase(uid);
    active_users.push_back(uid);
    running.insert(uid);
    if (arrival_order.count(uid) == 0) arrival_order[uid] = next_arrival++;

    flows.emplace_back();
    Flow& f = flows.back();
    f.uid = uid;
    f.stage = Stage::kSlotQueue;
    f.slot = stream_pool.Get(RT::Now(), 1);
  }

  // 退休只能发生在队首。别的位置想退，说明前面那个 user 的退休被跳过了。
  void Retire(uint64_t uid) {
    LOGCHECK(!active_users.empty(),
             "TaskScheduler: retire with an empty active queue.");
    LOGCHECK(active_users.front() == uid,
             "TaskScheduler: retire out of stream order.");
    running.erase(uid);
    active_users.pop_front();
    arrival_order.erase(uid);
  }

  void MarkComplete(uint64_t uid, uint64_t tid) { done[uid].insert(tid); }

  bool HasAnyCompleted(uint64_t uid) const {
    auto it = done.find(uid);
    return it != done.end() && !it->second.empty();
  }

  bool AllPrevDone(uint64_t uid, uint64_t tid) const {
    auto it = done.find(uid);
    if (it == done.end()) return tid == 0;
    for (uint64_t p = 0; p < tid; ++p) {
      if (it->second.count(p) == 0) return false;
    }
    return true;
  }

  // 普通任务等前一个 StreamID 的上一条，RETIRE 等它的同一条。
  static bool PredecessorTask(uint64_t tid, Opcode op, uint64_t* out) {
    if (op == Opcode::kRetire) {
      *out = tid;
      return true;
    }
    if (tid == 0) return false;
    *out = tid - 1;
    return true;
  }

  // 屏障那一行等的是某条入边上的包。MoE 下这条入边未必会来包：它下面挂的那些 group
  // 这次一个都没被激活，上游就不会往这边发。编译期已经把那条入边下面整棵树的 group
  // 收齐写进了来源表，所以这里只要看它们与本次 HitMap 有没有交集。
  //
  // 判不出来就当会来包，宁可等着：等得到是慢，放错了是丢包。
  bool BranchIsInactive(uint64_t uid, uint64_t tid, Opcode op) const {
    if (op != Opcode::kReduce && op != Opcode::kReduction) return false;
    std::vector<uint32_t> const* hit_map = dte->HitMapOf(uid);
    if (hit_map == nullptr || hit_map->empty()) return false;
    SkipSource const* src = ctx.Skip(tid);
    if (src == nullptr || src->sender_group_ids.empty()) return false;
    for (uint32_t group : src->sender_group_ids) {
      for (uint32_t hit : *hit_map) {
        if (group == hit) return false;
      }
    }
    return true;
  }

  uint64_t MatrixPriority(uint64_t uid, uint64_t stream_id) const {
    auto it = arrival_order.find(uid);
    return it == arrival_order.end() ? stream_id : it->second;
  }

  // ------------------------------------------------------------ 一条流水

  void Advance(Flow& f, Time now) {
    bool moved = true;
    while (moved) {
      moved = false;
      switch (f.stage) {
        case Stage::kSlotQueue:
          if (!stream_pool.Granted(f.slot)) break;
          ctx.Wait(Unit::kScheduler, f.uid, 0,
                   stream_pool.EnqueueCycle(f.slot),
                   stream_pool.GrantCycle(f.slot), WaitReason::kStreamSlot);
          f.tid = 0;
          f.timer_end = now + ctx.P().ts_logic_time;
          f.stage = Stage::kDecode;
          moved = true;
          break;

        case Stage::kDecode:
          if (now < f.timer_end) break;
          ctx.Span(Unit::kScheduler, f.uid, f.tid,
                   f.timer_end - ctx.P().ts_logic_time, f.timer_end,
                   SpanState::kDecode);
          if (!ctx.HasTask(f.tid)) {
            Finish(f, now);
            moved = true;
            break;
          }
          f.wait_start = now;
          f.stage = Stage::kOwnPrev;
          moved = true;
          break;

        case Stage::kOwnPrev: {
          if (!AllPrevDone(f.uid, f.tid)) break;
          ctx.Wait(Unit::kScheduler, f.uid, f.tid, f.wait_start, now,
                   WaitReason::kOwnPrevTask);
          f.stream_id = StreamPriority(f.uid, false);
          uint64_t pred = 0;
          if (f.stream_id > 0 &&
              PredecessorTask(f.tid, ctx.Task(f.tid).opcode, &pred)) {
            f.pred_uid = active_users[f.stream_id - 1];
            f.pred_tid = pred;
            f.wait_start = now;
            f.stage = Stage::kStreamPred;
          } else {
            f.stage = Stage::kDispatch;
          }
          moved = true;
          break;
        }

        case Stage::kStreamPred:
          if (!IsTaskDone(f.pred_uid, f.pred_tid)) break;
          ctx.Wait(Unit::kScheduler, f.uid, f.tid, f.wait_start, now,
                   WaitReason::kStreamPredecessor);
          f.stage = Stage::kDispatch;
          moved = true;
          break;

        case Stage::kDispatch:
          Dispatch(f, now);
          moved = true;
          break;

        case Stage::kWaitAck:
          if (!IsTaskDone(f.uid, f.tid)) break;
          NextTask(f, now);
          moved = true;
          break;

        case Stage::kBarrier:
          if (!IsTaskDone(f.uid, f.tid)) break;
          ctx.Wait(Unit::kScheduler, f.uid, f.tid, f.wait_start, now,
                   WaitReason::kIncomingBarrier);
          NextTask(f, now);
          moved = true;
          break;

        case Stage::kFinished:
          break;
      }
    }
  }

  void Dispatch(Flow& f, Time now) {
    TaskEntry const& e = ctx.Task(f.tid);
    switch (e.unit) {
      case UnitType::kDte:
        dte->DispatchComm(now, f.uid, f.tid, f.stream_id);
        f.stage = Stage::kWaitAck;
        return;
      case UnitType::kMc:
        mc->Dispatch(now, f.uid, f.tid, MatrixPriority(f.uid, f.stream_id));
        f.stage = Stage::kWaitAck;
        return;
      case UnitType::kVc:
        vc->Dispatch(now, f.uid, f.tid, f.stream_id);
        f.stage = Stage::kWaitAck;
        return;
      case UnitType::kCu:
        cu->Dispatch(now, f.uid, f.tid);
        f.stage = Stage::kWaitAck;
        return;
      case UnitType::kSkip:
        if (IsBarrierOpcode(e.opcode)) {
          if (IsTaskDone(f.uid, f.tid)) {
            // 对端的包比这条流水更早到，屏障已经解开了，不用再等。
            NextTask(f, now);
            return;
          }
          if (BranchIsInactive(f.uid, f.tid, e.opcode)) {
            // 这条入边下面挂的 group 这次一个都没激活，包不会来了，自己放行
            MarkComplete(f.uid, f.tid);
            NextTask(f, now);
            return;
          }
          f.wait_start = now;
          f.stage = Stage::kBarrier;
          return;
        }
        MarkComplete(f.uid, f.tid);
        NextTask(f, now);
        return;
    }
  }

  void NextTask(Flow& f, Time now) {
    if (running.count(f.uid) == 0) {
      // RETIRE 的 ack 已经把这个 user 从活跃队列里摘掉了，这条流水到此为止。
      Finish(f, now);
      return;
    }
    ++f.tid;
    f.timer_end = now + ctx.P().ts_logic_time;
    f.stage = Stage::kDecode;
  }

  // 完成表不在这里清。后一个 StreamID 的流水可能还在等这个 user 的某条任务，它靠
  // 每拍查表判断，表一清就永远等不到了。Bach 那边等待方挂在事件上，事件先唤醒、
  // 表后清，所以它清得掉；换成逐拍查表，清表就成了死锁。
  void Finish(Flow& f, Time now) {
    stream_pool.Drop(f.slot);
    stream_pool.Put(now, 1);
    running.erase(f.uid);
    arrival_order.erase(f.uid);
    f.stage = Stage::kFinished;
  }

  void Sweep() {
    size_t keep = 0;
    for (size_t i = 0; i < flows.size(); ++i) {
      if (flows[i].stage == Stage::kFinished) continue;
      if (keep != i) flows[keep] = flows[i];
      ++keep;
    }
    flows.resize(keep);
  }

  CoreContext ctx;
  Dte* dte = nullptr;
  MatrixCore* mc = nullptr;
  VectorCore* vc = nullptr;
  CreditUnit* cu = nullptr;

  CreditCounter stream_pool;
  std::deque<uint64_t> active_users;
  std::unordered_set<uint64_t> running;
  std::unordered_map<uint64_t, uint64_t> arrival_order;
  uint64_t next_arrival = 0;

  std::unordered_map<uint64_t, std::unordered_set<uint64_t>> done;
  std::deque<Flow> flows;
};

}
}

#endif
