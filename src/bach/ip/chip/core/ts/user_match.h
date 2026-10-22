#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_USER_MATCH_
#define _LATCH_BACH_IP_CHIP_CORE_TS_USER_MATCH_

// User_Match 与 DataIn_task_table，对应详细设计的 DataIn Module。
//
// 普通模式下 Router 的 trigger 带 {user_id, path_id,
// reissue}，这里一起判三件事：
//
//   用户      拿 user_id 与 stream_table 比对，没匹配上是新用户，从 Task 0 开始
//             建表；匹配上复用原来的 stream_id
//   搬入任务  按 path_id 在 task_chain 里找 wait_wake 的项，这个用户已经做完的
//             跳过，取最低的那一项。同一个 PID 可以对多个任务
//   跳过      按那一项的 TASK_TYPE 与 reissue 定这一次跳过哪几项：Broadcast
//   重发
//             的搬入且 reissue=0，搬入照做，配对的搬出跳过；P2P 重发的搬入且
//             reissue=0，搬入与配对的搬出都跳过，不派 DTE；reissue=1 两种都照做
//
// 配对的搬出取自搬入那一项的 TASK_P2P_REISSUE_TID。要派 DTE 的那一笔登记进
// DataIn_task_table；只跳过的不占它。跳过的位直接并进表项的 done_bitmap。新用户
// 这一次即使不派 DTE 也照样建表。
//
// 自启动 core 与权重加载模式走 Bypass：不查 stream_table、不建表，PC 取
// DATAIN_TASK，使用 SID0/TID0，不返回完成 ACK。
//
// 系统保证有效的 user_id 唯一：一个 user_id 在本 core 上的 stream
// 退休之前，不会 有另一个 token
// 用同一个号进来。老用户这一次在任务链上既没有要派的搬入任务、
// 也没有要跳过的，就是这条保证被打破了，这一笔没有去处，直接断言失败。
//
// trigger 口不设入口队列。条件不满足时直接拉低 ready，由 CoreStation
// 保持这一笔， 每拍重判一次，不丢弃、不越过。这条通路上的 trigger 与 token
// 一一对应，丢一笔就 等于丢一个
// token，所以只许反压不许丢。请求的保持责任本来就在 CoreStation 那一
// 侧，加一级队列只是把同一个反压点往后挪一格。

#include <memory>
#include <optional>
#include <set>
#include <string>

#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/chip/core/ts/cfg_reg.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"
#include "base/log.h"

namespace latch {
namespace bach {

// DataIn_task_table：只有 1 项。空闲时把 datain 任务信息与 Router 的请求信息
// 一起登记进去；被占住时不再接收要派 DTE 的请求。
struct DatainHold {
  bool valid = false, bypass = false, weight = false;
  uint64_t task_pc = 0, task_id = 0, user_id = 0, path_id = 0, stream_id = 0;
  uint64_t seq = 0, allocation = 0;
};

class DatainCandidatePort : public Logic {
public:
  LogicPtr<DatainHold> candidate;
  Logic64 taken_seq, query_mask;
  explicit DatainCandidatePort(ClockPtr c)
      : candidate(c), taken_seq(c), query_mask(c) {
    Fields(candidate, taken_seq, query_mask);
  }
};

class UserMatch : public BachModule {
public:
  UserMatch(ClockPtr clock, const std::string &name, CfgReg &reg,
            uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick), cfg(reg),
        trigger(std::make_shared<TriggerPort>(clock)),
        snap(std::make_shared<SnapshotPort>(clock)),
        create_port(std::make_shared<StreamWritePortIf>(clock)),
        candidate_port(std::make_shared<DatainCandidatePort>(clock)),
        created(clock), matched(clock), stalls(clock) {}
  TriggerPort &Trigger() { return *trigger; }
  void AttachTrigger(std::shared_ptr<TriggerPort> p) { trigger = std::move(p); }
  void AttachSnapshot(std::shared_ptr<SnapshotPort> p) { snap = std::move(p); }
  std::shared_ptr<StreamWritePortIf> CreatePtr() const { return create_port; }
  std::shared_ptr<DatainCandidatePort> CandidatePtr() const {
    return candidate_port;
  }
  DatainHold const &Hold() const { return hold; }
  void ReleaseHold() { hold.valid = false; }
  uint64_t Created() const { return created.Get(); }
  uint64_t Matched() const { return matched.Get(); }
  uint64_t Stalls() const { return stalls.Get(); }
  uint64_t Made() const { return made_cnt; }
  uint64_t MadeTask() const { return made_task; }
  uint64_t MadeUser() const { return made_user; }
  bool MadeUserValid() const { return made_user_vld; }
  bool Quiescent() const override { return !hold.valid && !lookup && !pending; }

protected:
  void Step() override {
    if (hold.valid && candidate_port->taken_seq.Get() == hold.seq)
      hold.valid = false;
    create_port->Idle();
    auto view = snap->Get();
    if (pending) {
      if (create_port->Accepted()) {
        pending = false;
        lookup.reset();
        ++created_pending;
      } else {
        Publish();
        return;
      }
    }
    if (lookup && view)
      PrepareOrCommit(*view);
    if (!lookup && trigger->Valid() && trigger->Seq() != last_trigger_seq &&
        view) {
      Lookup next;
      next.user = trigger->user_id.Get();
      next.path = trigger->path_id.Get();
      next.reissue = trigger->reissue.Get() != 0;
      next.seq = trigger->Seq();
      if (cfg.WeightsMode() || cfg.SelfStartCore()) {
        if (!cfg.DatainValid() || hold.valid ||
            (cfg.SelfStartCore() && !cfg.WeightsMode() &&
             !cfg.InitFinished())) {
          ++stall_pending;
          Publish();
          return;
        }
        hold = DatainHold{true,
                          cfg.SelfStartCore(),
                          cfg.WeightsMode(),
                          cfg.DatainPc(),
                          cfg.WeightsMode() ? 0 : kBypassTid,
                          cfg.WeightsMode() ? 0 : next.user,
                          cfg.WeightsMode() ? 0 : next.path,
                          cfg.WeightsMode() ? 0 : kBypassSid,
                          ++candidate_seq,
                          0};
        if (cfg.SelfStartCore() && !cfg.WeightsMode() && !view->started) {
          auto w = std::make_shared<StreamWrite>();
          w->valid = w->special_start = true;
          create_port->Drive(w);
          made_cnt += cfg.StreamNum();
          made_task = made_user = 0;
          made_user_vld = false;
        }
        last_trigger_seq = next.seq;
      } else if (cfg.InitFinished()) {
        for (uint64_t sid = 0; sid < view->slot_num; ++sid) {
          auto const &e = view->entry[sid];
          if (e.valid && e.user_id_vld && e.user_id == next.user) {
            next.hit = true;
            next.sid = sid;
            next.allocation = e.allocation;
            next.exclude = e.done_bitmap | e.datain_claimed_mask;
            break;
          }
        }
        lookup = next;
        last_trigger_seq = next.seq;
      }
    }
    Publish();
  }

private:
  struct Lookup {
    uint64_t user = 0, path = 0, seq = 0, sid = 0, allocation = 0, exclude = 0;
    uint64_t tid = kTaskChainNum, skip = 0;
    bool hit = false, reissue = false, prepared = false;
  };
  void PrepareOrCommit(StreamSnapshot const &view) {
    auto &l = *lookup;
    if (l.hit) {
      auto const &e = view.entry[l.sid];
      if (!e.valid || e.allocation != l.allocation) {
        lookup.reset();
        return;
      }
      uint64_t exclude = e.done_bitmap | e.datain_claimed_mask;
      if (exclude != l.exclude) {
        l.exclude = exclude;
        l.prepared = false;
        return;
      }
    }
    if (!l.prepared) {
      l.tid = cfg.MatchDatain(l.path, l.exclude);
      LOGCHECK(l.tid < kTaskChainNum,
               "UserMatch: PID没有未完成且未claimed的DataIn。");
      auto const &task = cfg.Task(l.tid);
      l.skip = 0;
      if (!l.reissue && task.task_type == TaskType::kP2pReissueIn)
        l.skip = (1ull << l.tid) | (1ull << task.p2p_reissue_tid);
      else if (!l.reissue && task.task_type == TaskType::kBcastReissueIn)
        l.skip = 1ull << task.p2p_reissue_tid;
      l.prepared = true;
      return;
    }
    bool candidate = !(l.skip & (1ull << l.tid));
    if ((candidate && hold.valid) ||
        (!l.hit && view.InFlight() >= cfg.StreamNum())) {
      ++stall_pending;
      return;
    }
    uint64_t sid = l.hit ? l.sid : view.AllocationSid();
    auto w = std::make_shared<StreamWrite>();
    w->valid = true;
    w->stream_id = sid;
    w->done_mask = l.skip;
    w->set_reissue = l.reissue;
    if (candidate)
      w->claim_mask = 1ull << l.tid;
    if (l.hit) {
      w->check_allocation = true;
      w->allocation = l.allocation;
      ++matched_pending;
    } else {
      if (!cfg.TriggerChainEn())
        return;
      w->whole = true;
      w->entry.valid = w->entry.user_id_vld = true;
      w->entry.user_id = l.user;
      w->entry.reissue = l.reissue;
      w->entry.done_bitmap = l.skip;
      w->entry.datain_claimed_mask = w->claim_mask;
      ApplyTaskAttr(w->entry, cfg.Task(0));
      w->entry.task_fsm =
          (l.skip & 1) ? TaskFsm::kFinish : InitFsmOf(cfg.Task(0));
      ++made_cnt;
      made_task = 0;
      made_user = l.user;
      made_user_vld = true;
    }
    create_port->Drive(w);
    pending = true;
    if (candidate)
      hold = DatainHold{true,
                        false,
                        false,
                        cfg.Task(l.tid).task_pc,
                        l.tid,
                        l.user,
                        l.path,
                        sid,
                        ++candidate_seq,
                        l.hit ? l.allocation : 0};
  }
  void Publish() {
    trigger->DriveReady(
        !lookup && !pending &&
        (!hold.valid || !(cfg.WeightsMode() || cfg.SelfStartCore())));
    candidate_port->candidate = hold.valid ? std::make_shared<DatainHold>(hold)
                                           : std::shared_ptr<DatainHold>();
    candidate_port->query_mask =
        lookup && lookup->hit ? 1ull << lookup->sid : 0;
    created = created_pending;
    matched = matched_pending;
    stalls = stall_pending;
  }
  CfgReg &cfg;
  std::shared_ptr<TriggerPort> trigger;
  std::shared_ptr<SnapshotPort> snap;
  std::shared_ptr<StreamWritePortIf> create_port;
  std::shared_ptr<DatainCandidatePort> candidate_port;
  DatainHold hold;
  std::optional<Lookup> lookup;
  bool pending = false;
  uint64_t last_trigger_seq = 0, candidate_seq = 0;
  uint64_t created_pending = 0, matched_pending = 0, stall_pending = 0;
  uint64_t made_cnt = 0, made_task = 0, made_user = 0;
  bool made_user_vld = false;
  Logic64 created, matched, stalls;
};

} // namespace bach
} // namespace latch

#endif
