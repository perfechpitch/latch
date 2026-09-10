#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_CREDIT_MONITOR_
#define _LATCH_BACH_IP_CHIP_CORE_TS_CREDIT_MONITOR_

// TS 这一侧的 credit 与退休。对应 task_state_update 的 credit 子模块加 retire。
//
// MAS 顶层的 Credit_monitor 已划删除线：出核前的资源监听在 Router 的
// CoreMemCreditMonitor，TS 这侧只做三件事 ——
//
//   申请  向 Router 注册资源申请，带 UserID、StreamID、TaskID、PathID
//   唤醒  Router 申请到后经反向控制通路通知，把对应 task 置 READY
//   退休  Head-only：只允许 head_ptr 指向的那一项退休
//
// 与 DTE 的分工：业务层的资源在这一级查完，有资源才下发；下发之后 DTE 只查 VC
// 通路上的 flit credit。
//
// 退休顺序不能反：先向 Router 持续发 credit 返还请求，Router 接收后才清该槽位的
// valid 并推进 head_ptr。反了会让还在路上的 credit 无处归还。

#include <deque>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/router/coremem_reissue.h"
#include "bach/ip/chip/core/router/credit_monitor.h"
#include "bach/ip/chip/core/router/retire.h"
#include "bach/ip/chip/core/ts/cfg_reg.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class TsCreditMonitor : public BachModule {
 public:
  TsCreditMonitor(ClockPtr clock, const std::string& name, CfgReg& reg,
                  uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        cfg(reg),
        snap(std::make_shared<SnapshotPort>(clock)),
        req(std::make_shared<CreditReqPort>(clock)),
        grant(std::make_shared<CreditGrantPort>(clock)),
        retire_req(std::make_shared<RetirePort>(clock)),
        wake(std::make_shared<StreamWritePortIf>(clock)),
        retire_wr(std::make_shared<StreamWritePortIf>(clock)),
        woken(clock),
        retired(clock) {}

  void AttachSnapshot(std::shared_ptr<SnapshotPort> p) { snap = std::move(p); }
  CreditReqPort& Req() { return *req; }
  void AttachReq(std::shared_ptr<CreditReqPort> p) { req = std::move(p); }
  CreditGrantPort& Grant() { return *grant; }
  void AttachGrant(std::shared_ptr<CreditGrantPort> p) { grant = std::move(p); }
  RetirePort& RetireReq() { return *retire_req; }
  void AttachRetire(std::shared_ptr<RetirePort> p) { retire_req = std::move(p); }
  // Router 的 CoreMem 重发完成后从这个口报进来，本级据此清掉那一项的重发标记。
  void AttachReissueDone(std::shared_ptr<ReissueDonePort> p) {
    reissue_done = std::move(p);
  }

  std::shared_ptr<StreamWritePortIf> WakePtr() const { return wake; }
  std::shared_ptr<StreamWritePortIf> RetireWrPtr() const { return retire_wr; }

  uint64_t Woken() const { return woken.Get(); }
  uint64_t Retired() const { return retired.Get(); }

  bool Quiescent() const override { return !asking && !retiring; }

 protected:
  void Step() override {
    // 末级先做：退休排在最前，让表项先腾空。
    DoRetire();
    TakeReissueDone();
    DoWake();
    DoAsk();

    woken = wake_pending;
    retired = retire_pending;
    TracePerCycle("woken", wake_pending);
    TracePerCycle("retired", retire_pending);
  }

 private:
  // Head-only 退休：只允许 head_ptr 那一项退休，条件是 valid=1 且 end=1 且
  // task_fsm=FINISH。
  void DoRetire() {
    if (retiring) {
      if (!retire_req->Accepted()) return;  // Router 还没收，保持
      // Router 收下了才清 valid 并推 head_ptr。
      auto w = std::make_shared<StreamWrite>();
      w->valid = true;
      w->stream_id = retire_slot;
      w->clear_valid = true;
      retire_wr->Drive(w);
      retire_req->Idle();
      retiring = false;
      // 清 valid 的那一笔写要下一拍才落进表、再下一拍才进快照。这中间读到的
      // 还是旧快照，同一项会被再判成「可以退休」。记住它，等快照里那一项的
      // valid 掉下去再放开。
      just_retired = retire_slot;
      just_retired_vld = true;
      ++retire_pending;
      return;
    }
    retire_wr->Idle();
    StreamSnapshotPtr s = snap->Get();
    if (!s) {
      retire_req->Idle();
      return;
    }
    if (just_retired_vld) {
      if (!s->entry[just_retired].valid) just_retired_vld = false;
    }
    uint64_t h = s->head_ptr;
    StreamEntry const& e = s->entry[h];
    if (just_retired_vld && h == just_retired) {
      retire_req->Idle();
      return;
    }
    if (e.valid && e.end && e.task_fsm == TaskFsm::kFinish) {
      retire_req->Drive(e.user_id);
      retiring = true;
      retire_slot = h;
      return;
    }
    retire_req->Idle();
  }

  // Router 的 CoreMem 重发完成：至少带 UserID 与 PathID，本级按 UserID 找到
  // 那一项，把重发标记清掉。不清的话这个用户的重发任务会被一直选中。
  void TakeReissueDone() {
    if (!reissue_done || !reissue_done->Valid()) return;
    if (reissue_done->Seq() == last_reissue_seq) return;
    last_reissue_seq = reissue_done->Seq();
    clear_q.push_back(reissue_done->User());
  }

  // Router 申请到资源后经这一路通知，唤醒对应 task 置 READY。
  void DoWake() {
    if (wake_hold) {
      if (wake->Accepted()) {
        wake_hold = false;
        ++wake_pending;
      } else {
        return;  // 只改一个字段的写失败后只重试这一笔
      }
    }
    wake->Idle();
    if (grant->Valid()) {
      uint64_t stream = grant->stream_id.Get();
      auto w = std::make_shared<StreamWrite>();
      w->valid = true;
      w->stream_id = stream;
      w->set_fsm = true;
      w->fsm = TaskFsm::kReady;
      // 这一笔 credit 对应的是一个已经建过表的老用户时，还要把 reissue 置起来。
      w->set_reissue = true;
      wake->Drive(w);
      wake_hold = true;
      return;
    }
    // 没有 credit 要处理时，用这个口把重发完成的那几笔标记清掉。
    if (clear_q.empty()) return;
    StreamSnapshotPtr s = snap->Get();
    if (!s) return;
    uint64_t user = clear_q.front();
    for (uint64_t i = 0; i < kStreamNum; ++i) {
      StreamEntry const& e = s->entry[i];
      if (!e.valid || !e.user_id_vld || e.user_id != user) continue;
      auto w = std::make_shared<StreamWrite>();
      w->valid = true;
      w->stream_id = i;
      w->clear_reissue = true;
      wake->Drive(w);
      wake_hold = true;
      clear_q.pop_front();
      return;
    }
    // 表里没有这个用户，说明它已经退休了，这一笔丢掉即可。
    clear_q.pop_front();
  }

  // 扫 stream_table，给需要 credit 又还在 WAIT 的 task 发申请。
  void DoAsk() {
    if (asking) {
      if (req->Ready()) asking = false;
      return;  // 请求保持到 Router 收下
    }
    req->Idle();
    StreamSnapshotPtr s = snap->Get();
    if (!s) return;
    for (uint64_t k = 0; k < kStreamNum; ++k) {
      uint64_t i = s->AgeOrder(k);
      StreamEntry const& e = s->entry[i];
      if (!e.valid || e.task_fsm != TaskFsm::kWait) continue;
      TaskEntry const& t = cfg.Task(e.task_id);
      if (!t.credit_en) continue;
      req->Drive(e.user_id, i, e.task_id, t.path_id);
      asking = true;
      return;  // 一拍一笔
    }
  }

  CfgReg& cfg;
  std::shared_ptr<SnapshotPort> snap;
  std::shared_ptr<CreditReqPort> req;
  std::shared_ptr<CreditGrantPort> grant;
  std::shared_ptr<RetirePort> retire_req;
  std::shared_ptr<StreamWritePortIf> wake, retire_wr;
  std::shared_ptr<ReissueDonePort> reissue_done;

  std::deque<uint64_t> clear_q;
  uint64_t last_reissue_seq = 0;

  bool asking = false, retiring = false, wake_hold = false;
  bool just_retired_vld = false;
  uint64_t retire_slot = 0, just_retired = 0;
  uint64_t wake_pending = 0, retire_pending = 0;

  Logic64 woken, retired;
};

}  // namespace bach
}  // namespace latch

#endif
