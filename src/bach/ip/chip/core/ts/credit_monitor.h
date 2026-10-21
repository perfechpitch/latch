#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_CREDIT_MONITOR_
#define _LATCH_BACH_IP_CHIP_CORE_TS_CREDIT_MONITOR_

// TS的方向Credit Ctrl、独立RMEM许可和Head生命周期。
// 三方向计数各初N，每SID登记新增需求Pending，同方向按用户年龄授权。
// 多方向全部满足才提交Grant：扣账、drct_used与READY同次Map更新。
// Return先入计数，下一周期参与分配；不向Router发旧式资源注册。
// Reduce在REDUCE_WAIT取得本SID RMEM许可后READY，不检查方向credit_en。
// 执行ACK与RMEM归还分别等待，归还同拍不再授权。
// 普通Head完成through-End、RMEM空闲且无DataIn查询占用后建立Retire Hold，
// 下一拍发UID/valid并释放，顶层内部ready固定1。
// SELF_START在相同资格下下一沿本地重生，不等待外部Router接受或发送Retire。
// DTE在任务下发后独立处理各VC的真实flit信用。

//
//
//
//
//
//
//
#include <deque>
#include <memory>
#include <string>

#include "bach/ip/chip/core/router/coremem_reissue.h"
#include "bach/ip/chip/core/router/credit_monitor.h"
#include "bach/ip/chip/core/router/retire.h"
#include "bach/ip/chip/core/router/router_table.h"
#include "bach/ip/chip/core/ts/cfg_reg.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"
#include "base/log.h"

namespace latch {
namespace bach {

class TsCreditMonitor : public BachModule {
public:
  TsCreditMonitor(ClockPtr clock, const std::string &name, CfgReg &reg,
                  uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick), cfg(reg),
        snap(std::make_shared<SnapshotPort>(clock)),
        req(std::make_shared<CreditReqPort>(clock)),
        grant(std::make_shared<CreditGrantPort>(clock)),
        retire_req(std::make_shared<RetirePort>(clock)),
        wake(std::make_shared<StreamWritePortIf>(clock)),
        retire_wr(std::make_shared<StreamWritePortIf>(clock)), woken(clock),
        retired(clock), rmem_admitted(clock) {}

  void AttachSnapshot(std::shared_ptr<SnapshotPort> p) { snap = std::move(p); }
  CreditReqPort &Req() { return *req; }
  void AttachReq(std::shared_ptr<CreditReqPort> p) { req = std::move(p); }
  CreditGrantPort &Grant() { return *grant; }
  void AttachGrant(std::shared_ptr<CreditGrantPort> p) { grant = std::move(p); }
  RetirePort &RetireReq() { return *retire_req; }
  std::shared_ptr<RetirePort> RetirePtr() const { return retire_req; }
  void AttachRetire(std::shared_ptr<RetirePort> p) {
    retire_req = std::move(p);
  }
  // Router 的 CoreMem 重发完成后从这个口报进来，本级据此清掉那一项的重发标记。
  // 本核 Router 表。type-4 任务的 path 在本核是纯转发（或没有本地输入）时，
  // 不在本核归约，也就不会有 ReduceDone 回来还 rmem：直接 READY，不占 rmem。
  void AttachRtab(RouterTable *t) { rtab = t; }
  void AttachReissueDone(std::shared_ptr<ReissueDonePort> p) {
    reissue_done = std::move(p);
  }
  // Router 记的下游 Stream 表。方向计数只数 TS 自己授权的发送，Xbar 直通转发
  // 的用户也占这张表；只看计数会授权一笔下游没有坑的发送，它进了交叉开关就
  // 堵在本核唯一的出核 VC 上。授权时查这张表并当场占坑。
  void SetStreamCheck(CreditMonitor::ResourceCheck fn) { stream_check = std::move(fn); }
  void SetStreamTake(CreditMonitor::ResourceTake fn) { stream_take = std::move(fn); }

  std::shared_ptr<StreamWritePortIf> WakePtr() const { return wake; }
  std::shared_ptr<StreamWritePortIf> RetireWrPtr() const { return retire_wr; }

  uint64_t Woken() const { return woken.Get(); }
  uint64_t Retired() const { return retired.Get(); }
  // 自启动 core 上 Router 收下退休请求后原地重新激活的笔数，Core 层拿它并进
  // ts_create。
  uint64_t Reactivated() const { return reactivate_cnt; }
  uint64_t RmemAdmitted() const { return rmem_admitted.Get(); }

  bool Quiescent() const override { return !wake_pending_port; }
  uint64_t DirectionCount(uint64_t dir) const { return counts.at(dir); }

protected:
  void Step() override {
    req->Idle();
    retire_wr->Idle();
    auto view = snap->Get();
    if (!view || cfg.WeightsMode())
      return;
    DoRetire(*view);
    if (wake_pending_port && !wake->Accepted())
      return;
    wake_pending_port = false;
    wake->Idle();
    auto batch = std::make_shared<StreamWrite>();
    batch->valid = true;
    auto budget = view->credit_count;
    counts = budget;
    bool older_unregistered = false;
    for (uint64_t age = 0; age < view->slot_num; ++age) {
      uint64_t sid = view->AgeOrder(age);
      auto const &e = view->entry[sid];
      if (!e.valid || (e.done_bitmap & (1ull << e.task_id))) {
        pending[sid].reset();
        continue;
      }
      if (e.task_fsm == TaskFsm::kReduceWait && !LocalReduce(e.task_path_id)) {
        batch->updates.push_back(Ready(sid, e));
        continue;
      }
      if (e.task_fsm == TaskFsm::kReduceWait && !e.rmem_busy) {
        auto w = Ready(sid, e);
        w->take_rmem = true;
        batch->updates.push_back(w);
        ++rmem_pending;
        continue;
      }
      if (e.task_fsm != TaskFsm::kWaitCredit) {
        pending[sid].reset();
        continue;
      }
      uint64_t need = cfg.Route(e.task_path_id).dir & 7 & ~e.drct_used;
      if (!pending[sid] || pending[sid]->tid != e.task_id ||
          pending[sid]->allocation != e.allocation ||
          pending[sid]->need != need) {
        pending[sid] = Pending{e.task_id, e.allocation, need};
        older_unregistered = true;
        continue;
      }
      if (older_unregistered)
        continue;
      bool enough = true;
      for (uint64_t dir = 0; dir < 3; ++dir)
        if ((need & (1ull << dir)) && budget[dir] == 0)
          enough = false;
      if (!enough)
        continue;
      if (stream_check && !stream_check(e.user_id, e.task_path_id))
        continue;
      if (stream_take)
        stream_take(e.user_id, e.task_path_id);
      auto w = Ready(sid, e);
      w->grant_dirs = need;
      if (cfg.Task(e.task_id).task_type == TaskType::kReissueOut)
        w->set_reissue = true;
      for (uint64_t dir = 0; dir < 3; ++dir)
        if (need & (1ull << dir))
          --budget[dir];
      batch->updates.push_back(w);
      pending[sid].reset();
      ++wake_count;
    }
    if (reissue_done && reissue_done->Valid() &&
        reissue_done->Seq() != last_reissue_seq) {
      last_reissue_seq = reissue_done->Seq();
      for (uint64_t sid = 0; sid < view->slot_num; ++sid) {
        auto const &e = view->entry[sid];
        if (e.valid && e.user_id_vld && e.user_id == reissue_done->User()) {
          auto w = std::make_shared<StreamWrite>();
          w->valid = w->clear_reissue = true;
          w->stream_id = sid;
          batch->updates.push_back(w);
        }
      }
    }
    if (!batch->updates.empty()) {
      wake->Drive(batch);
      wake_pending_port = true;
    }
    woken = wake_count;
    retired = retire_count;
    rmem_admitted = rmem_pending;
  }

private:
  struct Pending {
    uint64_t tid, allocation, need;
  };
  std::shared_ptr<StreamWrite> Ready(uint64_t sid, StreamEntry const &e) {
    auto w = std::make_shared<StreamWrite>();
    w->valid = w->set_fsm = w->fsm_if_current = w->check_allocation = true;
    w->stream_id = sid;
    w->from_task_id = e.task_id;
    w->allocation = e.allocation;
    w->fsm = TaskFsm::kReady;
    return w;
  }
  void DoRetire(StreamSnapshot const &view) {
    if (last_retired) {
      auto const &e = view.entry[last_retired->sid];
      if (retire_wr->Accepted() && !last_retired->counted) {
        if (!retire_wr->AcceptedCount()) {
          last_retired.reset();
          return;
        }
        last_retired->counted = true;
        ++retire_count;
        if (cfg.SelfStartCore())
          ++reactivate_cnt;
      }
      if (!e.valid || e.allocation != last_retired->allocation)
        last_retired.reset();
      else
        return;
    }
    uint64_t sid = view.head_ptr;
    auto const &e = view.entry[sid];
    uint64_t through = cfg.ThroughEndMask();
    if (!e.valid || !e.user_id_vld || e.rmem_busy || e.datain_busy ||
        !through || (e.done_bitmap & through) != through)
      return;
    auto w = std::make_shared<StreamWrite>();
    w->valid = true;
    w->stream_id = sid;
    if (cfg.SelfStartCore()) {
      w->whole = w->reactivate = true;
      w->entry = SelfStartEntry(cfg.Task(0));
      w->stream_num = cfg.StreamNum();
    } else {
      w->clear_valid = true;
    }
    w->check_allocation = true;
    w->allocation = e.allocation;
    retire_wr->Drive(w);
    last_retired = RetiredToken{sid, e.allocation};
  }
  struct RetiredToken {
    uint64_t sid, allocation;
    bool counted = false;
  };
  bool LocalReduce(uint64_t path_id) const {
    if (!rtab)
      return true;
    RouteEntry const &re = rtab->Lookup(0, path_id);
    return re.operation != Operation::kForward && re.reduce_in_mask != 0;
  }

  CfgReg &cfg;
  RouterTable *rtab = nullptr;
  std::shared_ptr<SnapshotPort> snap;
  std::shared_ptr<CreditReqPort> req;
  std::shared_ptr<CreditGrantPort> grant;
  std::shared_ptr<RetirePort> retire_req;
  std::shared_ptr<StreamWritePortIf> wake, retire_wr;
  std::shared_ptr<ReissueDonePort> reissue_done;
  CreditMonitor::ResourceCheck stream_check;
  CreditMonitor::ResourceTake stream_take;
  std::array<std::optional<Pending>, kStreamNum> pending{};
  std::optional<RetiredToken> last_retired;
  std::array<uint64_t, 3> counts{};
  bool wake_pending_port = false;
  uint64_t last_reissue_seq = 0, wake_count = 0, retire_count = 0,
           rmem_pending = 0;
  uint64_t reactivate_cnt = 0;
  Logic64 woken, retired, rmem_admitted;
};

} // namespace bach
} // namespace latch

#endif
