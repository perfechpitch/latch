#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_MU_VU_ARB_
#define _LATCH_BACH_IP_CHIP_CORE_TS_MU_VU_ARB_

// MU_Arb 与 VU_Arb：两条发射通路，同一份代码按 unit 参数化。
//
// 候选是 valid=1 且 task_fsm=READY 且 task_unit 是本单元的 stream。从 head_ptr
// 开始环形年龄优先，选最老的，发射宽度各 1。
//
// 三条发射通路各自独立打拍，同一拍可以并行下发 3 个 task。RV core 按 task_queue
// 是否有空槽产生 task_ack；未被接收时不能释放该 task 跳到下一个。
//
// 自启动 core 上 16 条链的 Task 0 与别的 task 一样参加仲裁、连续下发（《Task
// Scheduler》：同时激活16个用户的自启动任务，进行task仲裁，进行发射）。

#include <memory>
#include <optional>
#include <string>

#include "bach/ip/chip/core/ts/cfg_reg.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class UnitArb : public BachModule {
public:
  UnitArb(ClockPtr clock, const std::string &name, SendUnit which, CfgReg &reg,
          uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick), unit(which), cfg(reg),
        cmd(std::make_shared<TaskCmdPort>(clock)),
        snap(std::make_shared<SnapshotPort>(clock)),
        issue(std::make_shared<StreamWritePortIf>(clock)), issued(clock) {}

  TaskCmdPort &Cmd() { return *cmd; }
  std::shared_ptr<TaskCmdPort> CmdPtr() const { return cmd; }
  void AttachCmd(std::shared_ptr<TaskCmdPort> p) { cmd = std::move(p); }
  void AttachSnapshot(std::shared_ptr<SnapshotPort> p) { snap = std::move(p); }
  std::shared_ptr<StreamWritePortIf> IssuePtr() const { return issue; }

  uint64_t Issued() const { return issued.Get(); }

  bool Quiescent() const override { return !held; }

protected:
  void Step() override {
    issue->Idle();
    auto view = snap->Get();
    if (held) {
      if (!cmd->Ready()) {
        issued = issue_pending;
        return;
      }
      auto w = std::make_shared<StreamWrite>();
      w->valid = w->set_fsm = w->fsm_if_current = w->check_allocation = true;
      w->stream_id = held->stream;
      w->from_task_id = held->task;
      w->allocation = held->allocation;
      w->fsm = TaskFsm::kInfly;
      issue->Drive(w);
      issued_stream = held->stream;
      issued_task = held->task;
      issued_allocation = held->allocation;
      held.reset();
      cmd->Idle();
      ++issue_pending;
      issued = issue_pending;
      return;
    }
    if (!view) {
      cmd->Idle();
      return;
    }
    if (issued_stream < kStreamNum) {
      auto const &e = view->entry[issued_stream];
      if (!e.valid || e.task_id != issued_task ||
          e.allocation != issued_allocation || e.task_fsm != TaskFsm::kReady)
        issued_stream = kStreamNum;
    }
    for (uint64_t age = 0; age < view->slot_num; ++age) {
      uint64_t sid = view->AgeOrder(age);
      auto const &e = view->entry[sid];
      if (!e.valid || e.task_fsm != TaskFsm::kReady || e.task_unit != unit ||
          (sid == issued_stream && e.task_id == issued_task))
        continue;
      cmd->Drive(e.task_pc, sid, e.task_id, e.user_id, e.task_path_id,
                 e.task_recv != RecvUnit::kRvOnly, ++cmd_seq,
                 cfg.Route(e.task_path_id).vcid);
      held = Held{sid, e.task_id, e.allocation};
      issued = issue_pending;
      return;
    }
    cmd->Idle();
    issued = issue_pending;
  }

private:
  struct Held {
    uint64_t stream, task, allocation;
  };
  SendUnit unit;
  CfgReg &cfg;
  std::shared_ptr<TaskCmdPort> cmd;
  std::shared_ptr<SnapshotPort> snap;
  std::shared_ptr<StreamWritePortIf> issue;
  std::optional<Held> held;
  uint64_t issued_stream = kStreamNum, issued_task = 0, issued_allocation = 0;
  uint64_t cmd_seq = 0, issue_pending = 0;
  Logic64 issued;
};

} // namespace bach
} // namespace latch

#endif
