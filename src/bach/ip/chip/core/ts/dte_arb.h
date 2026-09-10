#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_DTE_ARB_
#define _LATCH_BACH_IP_CHIP_CORE_TS_DTE_ARB_

// DTE_Arb：DTE 那一条发射通路。
//
// 候选是 valid=1 且 task_fsm=READY 且 task_unit=DTE 的 stream。
//
// Reissue 任务优先级最高，从 head_ptr 选最老的 Reissue；没有 Reissue 时，DataIn
// 与普通 Generated 按相对 head_ptr 的 Stream 年龄比较，较老者优先，同一 Stream
// 时优先选 Generated。
//
// 选中后非抢占保持：锁定任务上下文，命令与相关字段保持稳定，直到对应 RV core
// 返回 raw ACCEPT。
//
// 收到 ACCEPT 后，Generated 任务向 Stream_table 提交 READY → INFLY；DataIn 任务
// 只通知 DataIn_task_table 出槽，不改 stream_table 里的当前任务状态。
//
// 重发 task 不在用户主线任务链上，可与用户任务链并行执行。

#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/ts/cfg_reg.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/chip/core/ts/user_match.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class DteArb : public BachModule {
 public:
  DteArb(ClockPtr clock, const std::string& name, UserMatch& matcher,
         uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        um(matcher),
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
    // 上一笔的 READY → INFLY 回写被表收下了就撤掉，没收下就下一拍补发：写口
    // 是握手口，落到同一个 stream 的写按优先级排，没轮上的这一拍不回 accepted。
    if (infly && issue->Accepted()) infly.reset();

    // 非抢占保持：锁定的这一笔一直发到看见 ACCEPT。
    if (holding) {
      if (cmd->Ready()) {
        Accept();
      } else {
        DriveIssue();
        issued = issue_pending;
        return;  // 命令与字段保持不变
      }
    }
    DriveIssue();
    Select();
    issued = issue_pending;
    TracePerCycle("issued", issue_pending);
  }

 private:
  void Select() {
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
    // 先找最老的 Reissue。
    for (uint64_t k = 0; k < kStreamNum; ++k) {
      uint64_t i = s->AgeOrder(k);
      StreamEntry const& e = s->entry[i];
      if (!Candidate(i, e)) continue;
      if (!e.is_reissue) continue;
      Lock(i, e);
      return;
    }
    // 没有 Reissue 时，DataIn 与普通 Generated 按相对 head_ptr 的 Stream 年龄
    // 比较，较老者优先；同一个 Stream 上两者都在时优先选 Generated。
    //
    // 把 DataIn 一律排在所有 Generated 之后是不行的：主线上只要一直有就绪的
    // Generated，那一格 DataIn 就发不出去，而它占着 DataIn_task_table 唯一的
    // 一格，新用户建表跟着一起停。
    DatainHold const& h = um.Hold();
    uint64_t datain_age = kStreamNum;
    if (h.valid) {
      datain_age = (h.stream_id + kStreamNum - s->head_ptr) % kStreamNum;
    }
    for (uint64_t k = 0; k < kStreamNum; ++k) {
      uint64_t i = s->AgeOrder(k);
      StreamEntry const& e = s->entry[i];
      if (Candidate(i, e)) {
        Lock(i, e);
        return;
      }
      if (k == datain_age) {
        LockDatain(h);
        return;
      }
    }
    if (h.valid) {
      LockDatain(h);
      return;
    }
    cmd->Idle();
  }

  bool Candidate(uint64_t i, StreamEntry const& e) const {
    if (!e.valid || e.task_fsm != TaskFsm::kReady ||
        e.task_unit != SendUnit::kDte) {
      return false;
    }
    // 刚发出去的那一笔，表里的 READY → INFLY 还没落下来：写口一拍、快照一拍，
    // 这两拍里快照上它仍是 READY。不挡住的话同一个 task 会被下发两次。
    return !(i == issued_stream && e.task_id == issued_task);
  }

  void Lock(uint64_t i, StreamEntry const& e) {
    cmd->Drive(e.task_pc, i, e.task_id, e.user_id,
               e.task_path_id, e.task_dsa_en, ++cmd_seq);
    holding = true;
    hold_stream = i;
    hold_task = e.task_id;
    hold_datain = false;
  }

  void LockDatain(DatainHold const& h) {
    cmd->Drive(h.task_pc, h.stream_id, h.task_id, h.user_id, h.path_id,
               /*dsa_en=*/true, ++cmd_seq);
    holding = true;
    hold_stream = h.stream_id;
    hold_task = h.task_id;
    hold_datain = true;
  }

  void Accept() {
    holding = false;
    ++issue_pending;
    if (hold_datain) {
      // DataIn 任务只通知 DataIn_task_table 出槽，不改 stream_table 的当前状态。
      um.ReleaseHold();
      hold_datain = false;
      return;
    }
    issued_stream = hold_stream;
    issued_task = hold_task;
    auto w = std::make_shared<StreamWrite>();
    w->valid = true;
    w->stream_id = hold_stream;
    w->set_fsm = true;
    w->fsm = TaskFsm::kInfly;
    infly = w;
  }

  void DriveIssue() {
    if (infly) {
      issue->Drive(infly);
    } else {
      issue->Idle();
    }
  }

  UserMatch& um;
  std::shared_ptr<TaskCmdPort> cmd;
  std::shared_ptr<SnapshotPort> snap;
  std::shared_ptr<StreamWritePortIf> issue;

  // 还没被表收下的那一笔状态回写。
  std::shared_ptr<StreamWrite> infly;
  bool holding = false, hold_datain = false;
  uint64_t hold_stream = 0, hold_task = 0;
  // 刚发出、表里还没变成 INFLY 的那一笔。
  uint64_t issued_stream = kStreamNum, issued_task = 0;
  uint64_t cmd_seq = 0;
  uint64_t issue_pending = 0;

  Logic64 issued;
};

}  // namespace bach
}  // namespace latch

#endif
