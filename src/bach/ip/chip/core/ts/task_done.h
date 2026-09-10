#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_TASK_DONE_
#define _LATCH_BACH_IP_CHIP_CORE_TS_TASK_DONE_

// task_done：七路完成事件合流。
//
// 六路是 DTE、MU、VU 各自的 RV core ack 与 DSA ack，第七路是 Router 的 Reduce
// Done。七路都是脉冲，这里永远就绪、不向上游反压：完成事件在硬件里没有重发通路，
// 接收方一旦拒收就等于把那个 stream 永远停在当前 task。
//
// 哪一路才算数由当前任务的 task_recv 定：
//   只调 RV core                RV core 收尾
//   调了 DSA、RV core 不等它     DSA 收尾
//   两者都要                     等二者都完成
//
// Reduce 任务的完成拆成两半，这是最容易实现错的一条：DTE ack 只代表搬运完成，
// 执行 consume_only、不修改 stream 状态；只有 Router Reduce Done 才有权把 Reduce
// 任务置 FINISH。两个事件可以任意顺序到达。
//
// reduce_num = N 时两半各有 N 笔，按包一一配对：包头带 reduce_seq（0～N−1），
// DTE 发出时打上，Router 的 Reduce Done 原样带回。两张 N 位位图各按 reduce_seq
// 置位，只有低 N 位都满才置 FINISH。这样第 k 笔的 Router Done 只与第 k 笔的
// DTE ack 配对，不会出现两边总数凑够、实际却漏了某一笔的情况。
//
// datain 任务的完成只点亮 done_bitmap 对应位，不推进 task_id：它的执行时刻就是
// 数据到达时刻，与主线走到哪无关。

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <string>

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
    uint64_t stream_id = 0;
    uint64_t task_id = 0;
    uint64_t reduce_seq = 0;
    bool from_dsa = false;
    bool from_router = false;
    // 自启动 core 的 RV core 带回来的用户号。建表那一刻表项里没有，软件认出
    // 这一笔属于哪个用户之后随完成一起回来。
    bool has_user = false;
    uint64_t user_id = 0;
  };
  // 每个 stream 一份 reduce 配对状态。只由这个模块读写。
  struct ReducePend {
    uint64_t dte_ack_map = 0;
    uint64_t router_done_map = 0;
  };

  void Collect() {
    for (uint64_t u = 0; u < 3; ++u) {
      if (rv_done[u]->Valid()) {
        q.push_back({rv_done[u]->stream_id.Get(), rv_done[u]->task_id.Get(),
                     rv_done[u]->reduce_seq.Get(), false, false, true,
                     rv_done[u]->user_id.Get()});
      }
      if (dsa_done[u]->Valid()) {
        q.push_back({dsa_done[u]->stream_id.Get(), dsa_done[u]->task_id.Get(),
                     dsa_done[u]->reduce_seq.Get(), true, false});
      }
    }
    // Router 不携带 stream_id，按 user_id 找对应 Stream。
    if (rdc_done->Valid()) {
      uint64_t user = rdc_done->user_id.Get();
      uint64_t seq = rdc_done->reduce_seq.Get();
      StreamSnapshotPtr s = snap->Get();
      if (s) {
        for (uint64_t i = 0; i < kStreamNum; ++i) {
          if (s->entry[i].valid && s->entry[i].user_id_vld &&
              s->entry[i].user_id == user) {
            q.push_back({i, s->entry[i].task_id, seq, false, true});
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
    StreamEntry const& st = s->entry[e.stream_id];
    if (!st.valid) return;

    TaskEntry const& t = cfg.Task(e.task_id);
    auto w = std::make_shared<StreamWrite>();
    w->valid = true;
    w->stream_id = e.stream_id;

    // datain 任务：只点亮 done_bitmap，不推进 task_id。
    if (t.IsDataIn()) {
      w->set_done_bit = true;
      w->done_bit = e.task_id;
      completion->Drive(w);
      pending = true;
      return;
    }

    // Reduce 任务：两半按 reduce_seq 逐位配对。
    if (t.reduce) {
      ReducePend& p = pend[e.stream_id];
      uint64_t n = t.reduce_num == 0 ? 1 : t.reduce_num;
      if (e.from_router) {
        p.router_done_map |= 1ull << e.reduce_seq;
      } else {
        // DTE ack 只代表搬运完成，consume_only，不改 stream 状态。
        p.dte_ack_map |= 1ull << e.reduce_seq;
      }
      uint64_t full = n >= 64 ? ~0ull : ((1ull << n) - 1);
      if ((p.dte_ack_map & full) != full ||
          (p.router_done_map & full) != full) {
        return;  // 还没配齐，先到的那个照常置位、不必等
      }
      p = ReducePend{};  // 提交时清零
      ++pair_pending;
      w->set_done_bit = true;
      w->done_bit = e.task_id;
      w->set_fsm = true;
      w->fsm = TaskFsm::kFinish;
      w->fsm_if_current = true;
      w->from_task_id = e.task_id;
      completion->Drive(w);
      pending = true;
      ++finish_pending;
      return;
    }

    // 普通任务：按 task_recv 判哪一路算数。
    if (!Counts(t.recv_unit, e.from_dsa, e.stream_id, e.task_id)) return;

    // 自启动 core：建表那一刻没有用户信息，RV core 认出这一笔属于哪个用户之后
    // 随完成带回来，这一路走 completion 写口补进表项，不另设专用写口。
    if (e.has_user && !st.user_id_vld) {
      w->set_user_id = true;
      w->user_id = e.user_id;
    }
    w->set_done_bit = true;
    w->done_bit = e.task_id;
    w->set_fsm = true;
    w->fsm = TaskFsm::kFinish;
    // 只有这一笔的 task_id 等于该 stream 当前的 task_id 时才改 task_fsm。
    w->fsm_if_current = true;
    w->from_task_id = e.task_id;
    completion->Drive(w);
    pending = true;
    ++finish_pending;
  }

  // 两者都要上报时等二者都到。用一张小表记「这一笔已经收到过哪一边」。
  bool Counts(RecvUnit recv, bool from_dsa, uint64_t stream, uint64_t task) {
    if (recv == RecvUnit::kRvOnly) return !from_dsa;
    if (recv == RecvUnit::kDsa) return from_dsa;
    // kDteDsaRmem：两边都要。
    uint64_t key = stream * kTaskChainNum + task;
    uint32_t& seen = both[key];
    seen |= from_dsa ? 2u : 1u;
    if (seen != 3u) return false;
    both.erase(key);
    return true;
  }

  CfgReg& cfg;
  std::vector<std::shared_ptr<DonePort>> rv_done, dsa_done;
  std::shared_ptr<ReduceDonePort> rdc_done;
  std::shared_ptr<SnapshotPort> snap;
  std::shared_ptr<StreamWritePortIf> completion;

  // Step 独占。
  std::deque<Event> q;
  std::array<ReducePend, kStreamNum> pend{};
  std::map<uint64_t, uint32_t> both;
  bool pending = false;
  uint64_t finish_pending = 0, pair_pending = 0;

  Logic64 finished, paired;
};

}  // namespace bach
}  // namespace latch

#endif
