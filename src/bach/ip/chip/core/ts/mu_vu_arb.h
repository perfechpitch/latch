#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_MU_VU_ARB_
#define _LATCH_BACH_IP_CHIP_CORE_TS_MU_VU_ARB_

// MU_Arb 与 VU_Arb：两条发射通路，同一份代码按 unit 参数化。
//
// 候选是 valid=1 且 task_fsm=READY 且 task_unit 是本单元的 stream。从 head_ptr
// 开始环形年龄优先，选最老的，发射宽度各 1。
//
// 三条发射通路各自独立打拍，同一拍可以并行下发 3 个 task。RV core 按 task_queue
// 是否有空槽产生 task_ack；未被接收时不能释放该 task 跳到下一个。

#include <memory>
#include <string>

#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class UnitArb : public BachModule {
 public:
  UnitArb(ClockPtr clock, const std::string& name, SendUnit which,
          uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        unit(which),
        cmd(std::make_shared<TaskCmdPort>(clock)),
        snap(std::make_shared<SnapshotPort>(clock)),
        issue(std::make_shared<StreamWritePortIf>(clock)),
        issued(clock) {}

  TaskCmdPort& Cmd() { return *cmd; }
  std::shared_ptr<TaskCmdPort> CmdPtr() const { return cmd; }
  void AttachCmd(std::shared_ptr<TaskCmdPort> p) { cmd = std::move(p); }
  void AttachSnapshot(std::shared_ptr<SnapshotPort> p) { snap = std::move(p); }
  std::shared_ptr<StreamWritePortIf> IssuePtr() const { return issue; }

  uint64_t Issued() const { return issued.Get(); }

  bool Quiescent() const override { return !holding; }

 protected:
  void Step() override {
    // 上一笔的 READY → INFLY 回写被表收下了就撤掉，没收下就下一拍补发。
    if (infly && issue->Accepted()) infly.reset();

    if (holding) {
      if (cmd->Ready()) {
        holding = false;
        ++issue_pending;
        issued_stream = hold_stream;
        issued_task = hold_task;
        auto w = std::make_shared<StreamWrite>();
        w->valid = true;
        w->stream_id = hold_stream;
        w->set_fsm = true;
        w->fsm = TaskFsm::kInfly;
        infly = w;
        infly_fresh = true;
      } else {
        DriveIssue();
        issued = issue_pending;
        return;  // 非抢占保持
      }
    }
    DriveIssue();
    Select();
    issued = issue_pending;
    TracePerCycle("issued", issue_pending);
  }

 private:
  // 一笔回写只驱一次，之后保持不动直到表收下：表按序号认它，每拍重驱会换一个
  // 序号，被当成新的一笔再执行一遍。
  void DriveIssue() {
    if (!infly) {
      issue->Idle();
      return;
    }
    if (infly_fresh) {
      issue->Drive(infly);
      infly_fresh = false;
    }
  }

  void Select() {
    if (holding) return;
    StreamSnapshotPtr s = snap->Get();
    if (!s) {
      cmd->Idle();
      return;
    }
    // 上一笔的 INFLY 已经在表里了，标记可以放开。
    if (issued_stream < kStreamNum) {
      StreamEntry const& e = s->entry[issued_stream];
      if (!e.valid || e.task_fsm != TaskFsm::kReady ||
          e.task_id != issued_task) {
        issued_stream = kStreamNum;
      }
    }
    for (uint64_t k = 0; k < kStreamNum; ++k) {
      uint64_t i = s->AgeOrder(k);
      StreamEntry const& e = s->entry[i];
      if (!e.valid || e.task_fsm != TaskFsm::kReady || e.task_unit != unit) {
        continue;
      }
      // 刚发出去的那一笔，表里的 READY → INFLY 还没落下来：写口一拍、快照一
      // 拍，这两拍里快照上它仍是 READY。不挡住的话同一个 task 会被下发两次。
      if (i == issued_stream && e.task_id == issued_task) continue;
      cmd->Drive(e.task_pc, i, e.task_id, e.user_id, e.task_path_id,
                 e.task_recv == RecvUnit::kDsa, ++cmd_seq);
      holding = true;
      hold_stream = i;
      hold_task = e.task_id;
      return;
    }
    cmd->Idle();
  }

  SendUnit unit;
  std::shared_ptr<TaskCmdPort> cmd;
  std::shared_ptr<SnapshotPort> snap;
  std::shared_ptr<StreamWritePortIf> issue;

  // 还没被表收下的那一笔状态回写。
  std::shared_ptr<StreamWrite> infly;
  bool infly_fresh = false;
  bool holding = false;
  uint64_t hold_stream = 0, hold_task = 0, cmd_seq = 0;
  // 刚发出、表里还没变成 INFLY 的那一笔。
  uint64_t issued_stream = kStreamNum, issued_task = 0;
  uint64_t issue_pending = 0;

  Logic64 issued;
};

}  // namespace bach
}  // namespace latch

#endif
