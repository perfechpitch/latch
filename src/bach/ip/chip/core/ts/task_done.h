#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_TASK_DONE_
#define _LATCH_BACH_IP_CHIP_CORE_TS_TASK_DONE_

// Task Done：六路执行ACK分类、FIFO配对与独立RMEM UID返回。
// Core/DSA ACK各周期直接接收，没有Ready；Core-only、DSA-only与双ACK
// 按recv规则完成。双ACK使用每执行单元两个深16 FIFO，按队头SID/TID配对。
// 不允许越过队头寻找任意匹配；Core侧UID用于合法Task0绑定。
// Reduce本地Core/DSA双ACK完成Task，RMEM返回仅按UID归还空间，
// 不置TID完成位，不改Current PID；前次未归还不得再次占用或退休。
// Current完成置FINISH，Future完成只并其完成位；Skip不绑定匿名UID。
// 仅Current/type5/Core-only的Core ACK新PID产生pending，供紧邻后继继承。
// loading丢弃新六路ACK/RMEM返回，既有配对FIFO保留并暂停配对。
// SELF_START的SID15/TID63独立DTE Bypass ACK过滤，不影响普通计算完成。

//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "bach/ip/chip/core/router/reduce_module.h"
#include "bach/ip/chip/core/ts/cfg_reg.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"
#include "base/log.h"

namespace latch {
namespace bach {

class TaskDone : public BachModule {
public:
  TaskDone(ClockPtr clock, const std::string &name, CfgReg &reg,
           uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick), cfg(reg),
        snap(std::make_shared<SnapshotPort>(clock)),
        rdc_done(std::make_shared<ReduceDonePort>(clock)), finished(clock),
        paired(clock) {
    for (uint64_t u = 0; u < 3; ++u) {
      rv_done.push_back(std::make_shared<DonePort>(clock));
      dsa_done.push_back(std::make_shared<DonePort>(clock));
    }
    for (uint64_t k = 0; k < kDoneLaneNum; ++k) {
      completion[k] = std::make_shared<StreamWritePortIf>(clock);
    }
  }

  DonePort &RvDone(uint64_t u) { return *rv_done.at(u); }
  DonePort &DsaDone(uint64_t u) { return *dsa_done.at(u); }
  std::shared_ptr<DonePort> RvDonePtr(uint64_t u) const {
    return rv_done.at(u);
  }
  std::shared_ptr<DonePort> DsaDonePtr(uint64_t u) const {
    return dsa_done.at(u);
  }
  ReduceDonePort &RdcDone() { return *rdc_done; }
  void AttachRdcDone(std::shared_ptr<ReduceDonePort> p) {
    rdc_done = std::move(p);
  }
  void AttachSnapshot(std::shared_ptr<SnapshotPort> p) { snap = std::move(p); }
  // 第 lane 条完成 lane 的 Stream_table 写口。
  std::shared_ptr<StreamWritePortIf> CompletionPtr(uint64_t lane) const {
    return completion.at(lane);
  }

  uint64_t Finished() const { return finished.Get(); }
  // 两路 ACK 配齐的次数。
  uint64_t Paired() const { return paired.Get(); }

  bool Quiescent() const override {
    for (uint64_t unit = 0; unit < 3; ++unit)
      if (!core_fifo[unit].empty() || !dsa_fifo[unit].empty())
        return false;
    return true;
  }

protected:
  void Step() override {
    for (auto const &p : completion)
      p->Idle();
    if (cfg.WeightsMode())
      return;
    auto view = snap->Get();
    if (!view)
      return;
    for (uint64_t unit = 0; unit < 3; ++unit) {
      bool paired_now = Pair(unit, *view);
      Capture(unit, false, *rv_done[unit], *view);
      Capture(unit, true, *dsa_done[unit], *view);
      if (!paired_now)
        Pair(unit, *view);
    }
    if (rdc_done->Valid()) {
      for (uint64_t sid = 0; sid < view->slot_num; ++sid) {
        auto const &e = view->entry[sid];
        if (!e.valid || !e.user_id_vld || e.user_id != rdc_done->user_id.Get())
          continue;
        auto w = std::make_shared<StreamWrite>();
        w->valid = w->give_rmem = w->check_allocation = true;
        w->stream_id = sid;
        w->allocation = e.allocation;
        completion[kDoneLaneRouter]->Drive(w);
        break;
      }
    }
    finished = finish_pending;
    paired = pair_pending;
    TracePerCycle("finished", finish_pending);
  }

private:
  struct Event {
    uint64_t sid, tid, uid, pid, allocation;
  };
  void Capture(uint64_t unit, bool dsa, DonePort const &port,
               StreamSnapshot const &view) {
    if (!port.Valid())
      return;
    uint64_t sid = port.stream_id.Get(), tid = port.task_id.Get();
    if (cfg.SelfStartCore() && unit == 0 && sid == kBypassSid &&
        tid == kBypassTid)
      return;
    if (sid >= view.slot_num || tid >= kTaskChainNum || !view.entry[sid].valid)
      return;
    auto const &task = cfg.Task(tid);
    if (!task.valid || uint64_t(task.send_unit) != unit)
      return;
    Event event{sid, tid, port.user_id.Get(), port.pid.Get(),
                view.entry[sid].allocation};
    if (task.recv_unit == RecvUnit::kRvOnly) {
      if (!dsa)
        Complete(2 * unit, event, view, true);
    } else if (task.recv_unit == RecvUnit::kDsa) {
      if (dsa)
        Complete(2 * unit + 1, event, view, false);
    } else {
      auto &fifo = dsa ? dsa_fifo[unit] : core_fifo[unit];
      LOGCHECK(fifo.size() < 16, "TaskDone: 双ACK FIFO超过16深度。");
      fifo.push_back(event);
    }
  }
  bool Pair(uint64_t unit, StreamSnapshot const &view) {
    auto &cores = core_fifo[unit];
    auto &dsas = dsa_fifo[unit];
    if (cores.empty() || dsas.empty())
      return false;
    auto const &c = cores.front();
    auto const &d = dsas.front();
    LOGCHECK(c.sid == d.sid && c.tid == d.tid,
             "TaskDone: 同执行单元两侧ACK顺序不一致。");
    Event event = c;
    cores.pop_front();
    dsas.pop_front();
    Complete(6 + unit, event, view, false);
    ++pair_pending;
    return true;
  }
  void Complete(uint64_t lane, Event const &event, StreamSnapshot const &view,
                bool core_only) {
    auto const &e = view.entry[event.sid];
    if (!e.valid || e.allocation != event.allocation)
      return;
    auto w = std::make_shared<StreamWrite>();
    w->valid = w->set_done_bit = w->check_allocation = true;
    w->stream_id = event.sid;
    w->allocation = event.allocation;
    w->done_bit = event.tid;
    if (!e.user_id_vld && event.tid == 0) {
      w->set_user_id = true;
      w->user_id = event.uid;
    }
    if (core_only && event.tid == e.task_id &&
        cfg.Task(event.tid).task_type == TaskType::kPidUpdate) {
      w->set_pid = true;
      w->pid = event.pid;
    }
    completion[lane]->Drive(w);
    ++finish_pending;
  }
  CfgReg &cfg;
  std::shared_ptr<SnapshotPort> snap;
  std::array<std::shared_ptr<StreamWritePortIf>, kDoneLaneNum> completion;
  std::shared_ptr<ReduceDonePort> rdc_done;
  std::vector<std::shared_ptr<DonePort>> rv_done, dsa_done;
  std::array<std::deque<Event>, 3> core_fifo, dsa_fifo;
  uint64_t finish_pending = 0, pair_pending = 0;
  Logic64 finished, paired;
};

} // namespace bach
} // namespace latch

#endif
