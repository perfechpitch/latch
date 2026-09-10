#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_TS_
#define _LATCH_BACH_IP_CHIP_CORE_TS_TS_

// TS 这一组九个模块的装配。
//
// 自身没有 Cycle()：构造各模块、把写口接到 Stream_table 上、把表快照分发给要读
// 表的那几个。
//
// TS 的全部工作收在两张表和四个动作里：task_chain 说清这类 core 的操作流长什么样，
// stream_table 说清每个在途用户走到了哪一步；四个动作是新用户到了建表、判当前
// 这一步的前置条件齐了没有、同一个执行单元有多个候选时选最老的、收到完成事件
// 推进度并在链尾退休。
//
// 对外五组接口：与 Router 的五组信号、与三个 RV core 的 task 下发与 ack、六路
// DSA 与 RV core 的完成、ctrl_noc 的配置口、异常上报口（本轮只留接口名）。

#include <memory>
#include <string>
#include <vector>

#include "bach/ip/chip/core/ts/cfg_reg.h"
#include "bach/ip/chip/core/ts/credit_monitor.h"
#include "bach/ip/chip/core/ts/dte_arb.h"
#include "bach/ip/chip/core/ts/mu_vu_arb.h"
#include "bach/ip/chip/core/ts/stream_table.h"
#include "bach/ip/chip/core/ts/task_ctrl.h"
#include "bach/ip/chip/core/ts/task_done.h"
#include "bach/ip/chip/core/ts/user_match.h"

namespace latch {
namespace bach {

struct TsCfg {
  // 各模块自己挂时钟，还是由外层统一驱动。理由见 RouterCfg::tick。
  bool tick = true;
};

class Ts {
 public:
  Ts(ClockPtr clock, const std::string& name, TsCfg const& setting,
     uint64_t parent = 0)
      : clk(clock), cfg_setting(setting) {
    const uint64_t gid = TraceGroup(name, parent);
    cfg = std::make_unique<CfgReg>(clock, "cfg", gid, setting.tick);
    table = std::make_unique<StreamTable>(clock, "stream_table", gid,
                                          setting.tick);
    user_match = std::make_unique<UserMatch>(clock, "user_match", *cfg,
                                             gid, setting.tick);
    task_ctrl = std::make_unique<TaskCtrl>(clock, "task_ctrl", *cfg, gid,
                                           setting.tick);
    dte_arb = std::make_unique<DteArb>(clock, "dte_arb", *user_match, gid,
                                       setting.tick);
    mu_arb = std::make_unique<UnitArb>(clock, "mu_arb", SendUnit::kMu,
                                       gid, setting.tick);
    vu_arb = std::make_unique<UnitArb>(clock, "vu_arb", SendUnit::kVu,
                                       gid, setting.tick);
    task_done = std::make_unique<TaskDone>(clock, "task_done", *cfg, gid,
                                           setting.tick);
    credit = std::make_unique<TsCreditMonitor>(clock, "credit", *cfg, gid,
                                               setting.tick);
    Wire();
  }

  // ── 配置面 ──
  CfgReg& Cfg() { return *cfg; }

  // ── 对外：接 Router ──
  TriggerPort& Trigger() { return user_match->Trigger(); }
  void AttachTrigger(std::shared_ptr<TriggerPort> p) {
    user_match->AttachTrigger(std::move(p));
  }
  void AttachCreditReq(std::shared_ptr<CreditReqPort> p) {
    credit->AttachReq(std::move(p));
  }
  void AttachCreditGrant(std::shared_ptr<CreditGrantPort> p) {
    credit->AttachGrant(std::move(p));
  }
  void AttachRetire(std::shared_ptr<RetirePort> p) {
    credit->AttachRetire(std::move(p));
  }
  void AttachReduceDone(std::shared_ptr<ReduceDonePort> p) {
    task_done->AttachRdcDone(std::move(p));
  }
  void AttachReissueDone(std::shared_ptr<ReissueDonePort> p) {
    credit->AttachReissueDone(std::move(p));
  }

  // ── 对外：接三个 RV core ──
  TaskCmdPort& DteCmd() { return dte_arb->Cmd(); }
  TaskCmdPort& MuCmd() { return mu_arb->Cmd(); }
  TaskCmdPort& VuCmd() { return vu_arb->Cmd(); }
  std::shared_ptr<TaskCmdPort> DteCmdPtr() const { return dte_arb->CmdPtr(); }
  std::shared_ptr<TaskCmdPort> MuCmdPtr() const { return mu_arb->CmdPtr(); }
  std::shared_ptr<TaskCmdPort> VuCmdPtr() const { return vu_arb->CmdPtr(); }
  DonePort& RvDone(uint64_t u) { return task_done->RvDone(u); }
  DonePort& DsaDone(uint64_t u) { return task_done->DsaDone(u); }
  // 装配层把这几根线交给 RV core 与 DSA，两端指向同一个对象。
  std::shared_ptr<DonePort> RvDonePtr(uint64_t u) const {
    return task_done->RvDonePtr(u);
  }
  std::shared_ptr<DonePort> DsaDonePtr(uint64_t u) const {
    return task_done->DsaDonePtr(u);
  }

  // ── 观测 ──
  StreamTable& Table() { return *table; }
  UserMatch& Matcher() { return *user_match; }
  TaskCtrl& Ctrl() { return *task_ctrl; }
  DteArb& DteArbiter() { return *dte_arb; }
  UnitArb& MuArbiter() { return *mu_arb; }
  UnitArb& VuArbiter() { return *vu_arb; }
  TaskDone& Done() { return *task_done; }
  TsCreditMonitor& Credit() { return *credit; }

  // 自启动：B core 与 R core 复位后直接建满表项，不等 Router trigger。
  void SelfStart() {
    if (!cfg->SelfStartCore()) return;
    table->SelfStart(cfg->Task(0), cfg->StreamNum());
  }

  void RunStep() {
    cfg->RunStep();
    credit->RunStep();
    task_done->RunStep();
    task_ctrl->RunStep();
    dte_arb->RunStep();
    mu_arb->RunStep();
    vu_arb->RunStep();
    user_match->RunStep();
    table->RunStep();
  }

  bool Quiescent() const {
    return table->Quiescent() && user_match->Quiescent() &&
           task_ctrl->Quiescent() && dte_arb->Quiescent() &&
           mu_arb->Quiescent() && vu_arb->Quiescent() &&
           task_done->Quiescent() && credit->Quiescent();
  }

 private:
  void Wire() {
    // 八个物理写口接到 Stream_table 上，顺序就是优先级。
    Bind(kWrRetirement, credit->RetireWrPtr());
    Bind(kWrCompletion, task_done->CompletionPtr());
    Bind(kWrInstall, task_ctrl->InstallPtr());
    Bind(kWrIssueDte, dte_arb->IssuePtr());
    Bind(kWrIssueMu, mu_arb->IssuePtr());
    Bind(kWrIssueVu, vu_arb->IssuePtr());
    Bind(kWrCreditWake, credit->WakePtr());
    Bind(kWrCreate, user_match->CreatePtr());

    // 表快照分发给要读表的那几个。它们读上一拍的快照，不碰 Stream_table 的容器。
    auto snap = table->SnapPtr();
    user_match->AttachSnapshot(snap);
    task_ctrl->AttachSnapshot(snap);
    dte_arb->AttachSnapshot(snap);
    mu_arb->AttachSnapshot(snap);
    vu_arb->AttachSnapshot(snap);
    task_done->AttachSnapshot(snap);
    credit->AttachSnapshot(snap);
  }

  // 写口是同一个对象的两端：请求方写 valid 与 req，Stream_table 写 accepted。
  void Bind(uint64_t idx, std::shared_ptr<StreamWritePortIf> port) {
    table->Rebind(idx, std::move(port));
  }

  ClockPtr clk;
  TsCfg cfg_setting;

  std::unique_ptr<CfgReg> cfg;
  std::unique_ptr<StreamTable> table;
  std::unique_ptr<UserMatch> user_match;
  std::unique_ptr<TaskCtrl> task_ctrl;
  std::unique_ptr<DteArb> dte_arb;
  std::unique_ptr<UnitArb> mu_arb, vu_arb;
  std::unique_ptr<TaskDone> task_done;
  std::unique_ptr<TsCreditMonitor> credit;
};

}  // namespace bach
}  // namespace latch

#endif
