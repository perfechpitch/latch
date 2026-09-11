#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_TASK_DONE_
#define _LATCH_BACH_IP_CHIP_CORE_TS_TASK_DONE_

// task_done：七路完成事件合流，对应详细设计的 Task Done Module。
//
// 六路是 DTE、MU、VU 各自的 RV core ACK 与 DSA ACK，第七路是 Router 的 Reduce
// Done。七路都是脉冲，这里永远就绪、不向上游反压：完成事件在硬件里没有重发通路，
// 接收方一旦拒收就等于把那个 stream 永远停在当前 task。
//
// 哪几路才算完成由 TASK_RECV_UNIT 定：00 只要 RV core 的 ACK；01 要同一个执行
// 单元的 RV core 与 DSA 两路 ACK 都到，先到的一半按 {执行单元, stream, task} 记
// 下，另一半到了才算。逐级 reduce 任务只认 Router 的 Reduce Done：Router 只带
// user_id，按它找到那个 stream，完成的就是它的当前任务；同一个用户同时最多一笔
// reduce 在做。本地的 ACK 对 reduce 任务只算搬完，不改状态。
//
// 完成的是当前任务就置 FINISH；是还没走到的后面某项，比如异步 datain 提前完成，
// 只点亮 done_bitmap 那一位。
//
// 两类 ACK 直接丢掉：权重加载期间的全部完成事件，以及自启动 core 上 Bypass
// datain 带保留身份 SID 15 / TID 63 回来的 ACK。
//
// PID 更新任务：RV core 的 ACK 带回新 PID，这一笔完成时写进表项，等紧邻后继继承。
// 自启动 core：Task 0 的 RV core ACK 带回用户号，这时补进表项。

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/ip/chip/core/router/reduce_module.h"
#include "bach/ip/chip/core/ts/cfg_reg.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class TaskDone : public BachModule {
 public:
  TaskDone(ClockPtr clock, const std::string& name, CfgReg& reg,
           uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        cfg(reg),
        snap(std::make_shared<SnapshotPort>(clock)),
        completion(std::make_shared<StreamWritePortIf>(clock)),
        rdc_done(std::make_shared<ReduceDonePort>(clock)),
        finished(clock),
        paired(clock) {
    for (uint64_t u = 0; u < 3; ++u) {
      rv_done.push_back(std::make_shared<DonePort>(clock));
      dsa_done.push_back(std::make_shared<DonePort>(clock));
    }
  }

  DonePort& RvDone(uint64_t u) { return *rv_done.at(u); }
  DonePort& DsaDone(uint64_t u) { return *dsa_done.at(u); }
  std::shared_ptr<DonePort> RvDonePtr(uint64_t u) const {
    return rv_done.at(u);
  }
  std::shared_ptr<DonePort> DsaDonePtr(uint64_t u) const {
    return dsa_done.at(u);
  }
  ReduceDonePort& RdcDone() { return *rdc_done; }
  void AttachRdcDone(std::shared_ptr<ReduceDonePort> p) {
    rdc_done = std::move(p);
  }
  void AttachSnapshot(std::shared_ptr<SnapshotPort> p) { snap = std::move(p); }
  std::shared_ptr<StreamWritePortIf> CompletionPtr() const {
    return completion;
  }

  uint64_t Finished() const { return finished.Get(); }
  // 两路 ACK 配齐的次数。
  uint64_t Paired() const { return paired.Get(); }

  bool Quiescent() const override { return q.empty(); }

 protected:
  void Step() override {
    // 上一笔写没被收下就重试这一笔，不能丢：完成事件没有重发通路。
    if (pending) {
      if (completion->Accepted()) {
        pending = false;
      } else {
        Collect();
        finished = finish_pending;
        paired = pair_pending;
        return;
      }
    }
    completion->Idle();
    Collect();
    Emit();
    finished = finish_pending;
    paired = pair_pending;
    TracePerCycle("finished", finish_pending);
  }

 private:
  struct Event {
    uint64_t unit = 0;  // 0 DTE、1 MU、2 VU
    uint64_t stream_id = 0;
    uint64_t task_id = 0;
    bool from_dsa = false;
    bool from_router = false;
    uint64_t user_id = 0;
    uint64_t pid = 0;
  };
  // 两路 ACK 配对时先到的那一半。RV core 那一半带着用户号与 PID。
  struct Half {
    bool core = false, dsa = false;
    uint64_t user_id = 0, pid = 0;
  };

  void Collect() {
    for (uint64_t u = 0; u < 3; ++u) {
      if (rv_done[u]->Valid()) {
        q.push_back({u, rv_done[u]->stream_id.Get(), rv_done[u]->task_id.Get(),
                     false, false, rv_done[u]->user_id.Get(),
                     rv_done[u]->pid.Get()});
      }
      if (dsa_done[u]->Valid()) {
        q.push_back({u, dsa_done[u]->stream_id.Get(),
                     dsa_done[u]->task_id.Get(), true, false, 0, 0});
      }
    }
    // Router 不携带 stream_id，按 user_id 找对应 Stream，完成的是它的当前任务。
    if (rdc_done->Valid()) {
      uint64_t user = rdc_done->user_id.Get();
      StreamSnapshotPtr s = snap->Get();
      if (s) {
        for (uint64_t i = 0; i < kStreamNum; ++i) {
          StreamEntry const& e = s->entry[i];
          if (e.valid && e.user_id_vld && e.user_id == user) {
            q.push_back({0, i, e.task_id, false, true, user, 0});
            break;
          }
        }
      }
    }
  }

  void Emit() {
    StreamSnapshotPtr s = snap->Get();
    if (!s || q.empty()) return;
    Event e = q.front();
    q.pop_front();
    if (cfg.WeightsMode()) return;
    if (cfg.SelfStartCore() && e.stream_id == kBypassSid &&
        e.task_id == kBypassTid) {
      return;
    }
    if (e.stream_id >= kStreamNum || e.task_id >= kTaskChainNum) return;
    StreamEntry const& st = s->entry[e.stream_id];
    if (!st.valid) return;

    TaskEntry const& t = cfg.Task(e.task_id);
    if (t.IsReduce()) {
      // 本地 ACK 只算搬完，Router 的 Reduce Done 才算做完。
      if (e.from_router) Complete(e, st, true);
      return;
    }
    if (e.from_router) return;
    if (t.recv_unit == RecvUnit::kRvOnly) {
      if (!e.from_dsa) Complete(e, st, false);
      return;
    }
    // 01：同一个执行单元的两路都要到。
    uint64_t key = (e.unit * kStreamNum + e.stream_id) * kTaskChainNum +
                   e.task_id;
    Half& h = halves[key];
    if (e.from_dsa) {
      h.dsa = true;
    } else {
      h.core = true;
      h.user_id = e.user_id;
      h.pid = e.pid;
    }
    if (!(h.core && h.dsa)) return;
    Event done = e;
    done.from_dsa = false;
    done.user_id = h.user_id;
    done.pid = h.pid;
    halves.erase(key);
    ++pair_pending;
    Complete(done, st, false);
  }

  void Complete(Event const& e, StreamEntry const& st, bool give_rmem) {
    auto w = std::make_shared<StreamWrite>();
    w->valid = true;
    w->stream_id = e.stream_id;
    w->set_done_bit = true;
    w->done_bit = e.task_id;
    w->set_fsm = true;
    w->fsm = TaskFsm::kFinish;
    // 只有这一笔的 task_id 等于该 stream 当前的 task_id 时才改 task_fsm。
    w->fsm_if_current = true;
    w->from_task_id = e.task_id;
    w->give_rmem = give_rmem;
    // 自启动 core：Task 0 的 RV core ACK 带回用户号，走 completion 写口补进表项。
    if (!e.from_router && !e.from_dsa && !st.user_id_vld) {
      w->set_user_id = true;
      w->user_id = e.user_id;
    }
    // PID 更新任务：当前任务完成时写进带回的新 PID。
    TaskEntry const& t = cfg.Task(e.task_id);
    if (t.task_type == TaskType::kPidUpdate && !e.from_router &&
        e.task_id == st.task_id) {
      w->set_pid = true;
      w->pid = e.pid;
    }
    completion->Drive(w);
    pending = true;
    ++finish_pending;
  }

  CfgReg& cfg;
  std::shared_ptr<SnapshotPort> snap;
  std::shared_ptr<StreamWritePortIf> completion;
  std::shared_ptr<ReduceDonePort> rdc_done;
  std::vector<std::shared_ptr<DonePort>> rv_done, dsa_done;

  // Step 独占。
  std::deque<Event> q;
  std::map<uint64_t, Half> halves;
  bool pending = false;
  uint64_t finish_pending = 0, pair_pending = 0;

  Logic64 finished, paired;
};

}  // namespace bach
}  // namespace latch

#endif
