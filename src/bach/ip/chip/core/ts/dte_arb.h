#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_DTE_ARB_
#define _LATCH_BACH_IP_CHIP_CORE_TS_DTE_ARB_

// DTE_Arb：DTE 那一条发射通路。
//
// 候选是 valid=1 且 task_fsm=READY 且 task_unit=DTE 的 stream。
//
// DataIn 与普通 Generated 全部按相对 head_ptr 的 Stream 年龄比较，较老者优先；
// 同一 Stream 上两者都在时优先选 Generated。自启动 core 上 Bypass 那一路 datain
// 不占 stream 表项，按队头的年龄算。
//
// 选中后非抢占保持：锁定任务上下文，命令与相关字段保持稳定，直到对应 RV core
// 返回 raw ACCEPT。
//
// 收到 ACCEPT 后，Generated 任务向 Stream_table 提交 READY → INFLY；DataIn 任务
// 只通知 DataIn_task_table 出槽，不改 stream_table 里的当前任务状态。
//
// reduce 任务收到 ACCEPT 后的那一笔回写同时占掉这个用户本级 Rmem 的 credit，
// Rmem 做完报回来才还。
//
// 命令里带 VCID：按当前任务的 PID 查 ROUTER_TABLE 得到，DTE 出核用它选 VC。
//
// 重发 task 不在用户主线任务链上，可与用户任务链并行执行。
//
// 自启动 core 上 16 条链的 Task 0 连续下发，规则与 MU_Arb、VU_Arb 相同。

#include <memory>
#include <optional>
#include <string>

#include "bach/ip/chip/core/ts/cfg_reg.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/chip/core/ts/user_match.h"
#include "bach/ip/module_base.h"
#include "base/log.h"

namespace latch {
namespace bach {

class DteArb : public BachModule {
public:
  DteArb(ClockPtr clock, const std::string &name, UserMatch &matcher,
         CfgReg &reg, uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick), um(matcher), cfg(reg),
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
      Held h = *held;
      held.reset();
      cmd->Idle();
      if (!h.bypass && !h.weight && view) {
        auto const &e = view->entry[h.stream];
        LOGCHECK(e.valid && e.allocation == h.allocation,
                 "DteArb: 持有命令的分配已失效。");
        auto w = std::make_shared<StreamWrite>();
        w->valid = w->check_allocation = true;
        w->stream_id = h.stream;
        w->allocation = h.allocation;
        w->from_task_id = h.task;
        w->fsm_if_current = w->set_fsm = true;
        w->fsm = TaskFsm::kInfly;
        if (h.datain)
          w->datain_accept_mask = 1ull << h.task;
        issue->Drive(w);
        issued_stream = h.stream;
        issued_task = h.task;
        issued_allocation = h.allocation;
      }
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
    auto candidate = um.CandidatePtr()->candidate.Get();
    bool candidate_valid =
        candidate && candidate->valid && candidate->seq != seen_candidate;
    bool normal_candidate =
        candidate_valid && !candidate->bypass && !candidate->weight &&
        candidate->stream_id < view->slot_num &&
        view->entry[candidate->stream_id].valid &&
        view->entry[candidate->stream_id].user_id_vld &&
        view->entry[candidate->stream_id].user_id == candidate->user_id;
    uint64_t winner = kStreamNum;
    bool use_datain = false;
    for (uint64_t age = 0; age < view->slot_num; ++age) {
      uint64_t sid = view->AgeOrder(age);
      auto const &e = view->entry[sid];
      if (normal_candidate && candidate->stream_id == sid) {
        winner = sid;
        use_datain = true;
        break;
      }
      if (e.valid && e.task_fsm == TaskFsm::kReady &&
          e.task_unit == SendUnit::kDte &&
          !(sid == issued_stream && e.task_id == issued_task)) {
        winner = sid;
        break;
      }
    }
    bool special = candidate_valid && candidate->bypass;
    bool weight = candidate_valid && candidate->weight;
    if (weight || (special && (winner == kStreamNum || prefer_bypass))) {
      LoadCandidate(*candidate, 0);
      if (!weight)
        prefer_bypass = false;
    } else if (winner < kStreamNum) {
      if (use_datain)
        LoadCandidate(*candidate, view->entry[winner].allocation);
      else {
        auto const &e = view->entry[winner];
        auto const &task = cfg.Task(e.task_id);
        uint64_t vc = !task.IsDataIn() ? cfg.Route(e.task_path_id).vcid : 0;
        cmd->Drive(e.task_pc, winner, e.task_id, e.user_id, e.task_path_id,
                   e.task_recv != RecvUnit::kRvOnly, ++cmd_seq, vc);
        held = Held{winner, e.task_id, e.allocation, false, false, false};
      }
      prefer_bypass = true;
    } else
      cmd->Idle();
    issued = issue_pending;
  }

private:
  void LoadCandidate(DatainHold const &h, uint64_t allocation) {
    cmd->Drive(h.task_pc, h.stream_id, h.task_id, h.user_id, h.path_id, true,
               ++cmd_seq, 0);
    held = Held{h.stream_id, h.task_id, allocation, true, h.bypass, h.weight};
    seen_candidate = h.seq;
    um.CandidatePtr()->taken_seq = h.seq;
  }
  struct Held {
    uint64_t stream, task, allocation;
    bool datain, bypass, weight;
  };
  UserMatch &um;
  CfgReg &cfg;
  std::shared_ptr<TaskCmdPort> cmd;
  std::shared_ptr<SnapshotPort> snap;
  std::shared_ptr<StreamWritePortIf> issue;
  std::optional<Held> held;
  bool prefer_bypass = false;
  uint64_t issued_stream = kStreamNum, issued_task = 0, issued_allocation = 0;
  uint64_t seen_candidate = 0, cmd_seq = 0, issue_pending = 0;
  Logic64 issued;
};

} // namespace bach
} // namespace latch

#endif
