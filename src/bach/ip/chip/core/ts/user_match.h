#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_USER_MATCH_
#define _LATCH_BACH_IP_CHIP_CORE_TS_USER_MATCH_

// User_Match 与 DataIn_task_table，对应详细设计的 DataIn Module。
//
// 普通模式下 Router 的 trigger 带 {user_id, path_id, reissue}，这里一起判三件事：
//
//   用户      拿 user_id 与 stream_table 比对，没匹配上是新用户，从 Task 0 开始
//             建表；匹配上复用原来的 stream_id
//   搬入任务  按 path_id 在 task_chain 里找 wait_wake 的项，这个用户已经做完的
//             跳过，取最低的那一项。同一个 PID 可以对多个任务
//   跳过      按那一项的 TASK_TYPE 与 reissue 定这一次跳过哪几项：Broadcast 重发
//             的搬入且 reissue=0，搬入照做，配对的搬出跳过；P2P 重发的搬入且
//             reissue=0，搬入与配对的搬出都跳过，不派 DTE；reissue=1 两种都照做
//
// 配对的搬出取自搬入那一项的 TASK_P2P_REISSUE_TID。要派 DTE 的那一笔登记进
// DataIn_task_table；只跳过的不占它。跳过的位直接并进表项的 done_bitmap。新用户
// 这一次即使不派 DTE 也照样建表。
//
// 自启动 core 与权重加载模式走 Bypass：不查 stream_table、不建表，PC 取
// DATAIN_TASK，身份用保留的 SID 15 / TID 63。
//
// trigger 口不设入口队列。条件不满足时直接拉低 ready，由 CoreStation 保持这一笔，
// 每拍重判一次，不丢弃、不越过。这条通路上的 trigger 与 token 一一对应，丢一笔就
// 等于丢一个 token，所以只许反压不许丢。请求的保持责任本来就在 CoreStation 那一
// 侧，加一级队列只是把同一个反压点往后挪一格。

#include <memory>
#include <set>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/chip/core/ts/cfg_reg.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// DataIn_task_table：只有 1 项。空闲时把 datain 任务信息与 Router 的请求信息
// 一起登记进去；被占住时不再接收要派 DTE 的请求。
struct DatainHold {
  bool valid = false;
  uint64_t task_pc = 0;
  uint64_t task_id = 0;
  uint64_t user_id = 0;
  uint64_t path_id = 0;
  uint64_t stream_id = 0;
};

class UserMatch : public BachModule {
 public:
  UserMatch(ClockPtr clock, const std::string& name, CfgReg& reg,
            uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        cfg(reg),
        trigger(std::make_shared<TriggerPort>(clock)),
        snap(std::make_shared<SnapshotPort>(clock)),
        create_port(std::make_shared<StreamWritePortIf>(clock)),
        created(clock),
        matched(clock),
        stalls(clock) {}

  TriggerPort& Trigger() { return *trigger; }
  void AttachTrigger(std::shared_ptr<TriggerPort> p) { trigger = std::move(p); }
  void AttachSnapshot(std::shared_ptr<SnapshotPort> p) { snap = std::move(p); }
  std::shared_ptr<StreamWritePortIf> CreatePtr() const { return create_port; }

  // DataIn_task_table 的那一格。发射通路把它取走后释放。
  DatainHold const& Hold() const { return hold; }
  void ReleaseHold() { hold.valid = false; }

  uint64_t Created() const { return created.Get(); }
  uint64_t Matched() const { return matched.Get(); }
  uint64_t Stalls() const { return stalls.Get(); }

  bool Quiescent() const override { return !hold.valid && !pending; }

 protected:
  void Step() override {
    // 上一笔写表请求还没被 stream_table 收下就原地保持。
    if (pending) {
      if (create_port->Accepted()) {
        pending = false;
        ++created_pending;
      } else {
        trigger->DriveReady(false);
        created = created_pending;
        matched = matched_pending;
        stalls = stall_pending;
        return;
      }
    }
    create_port->Idle();

    // 自启动 core：表项从队头退休之后再激活一个新的，接着等自启动任务。这一路
    // 占的是建表口，而这一档 core 上进来的包走 Bypass、不写表，两者同一拍互不
    // 相干，所以补完表照常往下处理 trigger。
    Refill();

    bool ok = Handle();
    trigger->DriveReady(ok);

    created = created_pending;
    matched = matched_pending;
    stalls = stall_pending;
    TracePerCycle("created", created_pending);
  }

 private:
  // 这一次 trigger 要派哪一项搬入任务、要跳过哪几项。
  struct Plan {
    uint64_t task_id = kTaskChainNum;  // kTaskChainNum 表示不派 DTE
    uint64_t skip = 0;
  };

  Plan PlanFor(uint64_t path, bool reissue, uint64_t done) const {
    Plan p;
    uint64_t t = cfg.MatchDatain(path, done);
    if (t >= kTaskChainNum) return p;
    TaskEntry const& in = cfg.Task(t);
    uint64_t out = 1ull << in.p2p_reissue_tid;
    if (!reissue && in.task_type == TaskType::kP2pReissueIn) {
      p.skip = (1ull << t) | out;
      return p;
    }
    if (!reissue && in.task_type == TaskType::kBcastReissueIn) p.skip = out;
    p.task_id = t;
    return p;
  }

  // 自启动 core 上在途表项少于 stream_num 时补一个。建表那一刻没有用户信息，
  // 等 Task 0 的 RV core ACK 带回来。
  bool Refill() {
    if (!cfg.SelfStartCore()) return false;
    StreamSnapshotPtr s = snap->Get();
    // 在途上限取 CFG_REG 里软件配的那个 stream_num，自启动建满的也是它。
    if (!s || s->InFlight() >= cfg.StreamNum()) {
      refilled_vld = false;
      return false;
    }
    // 刚补过但快照还没更新的那一两拍里会被再判成缺一项，记住上一次补在哪。
    if (refilled_vld && refilled_at == s->tail_ptr) return false;

    auto w = std::make_shared<StreamWrite>();
    w->valid = true;
    w->stream_id = s->tail_ptr;
    w->whole = true;
    StreamEntry& e = w->entry;
    e.valid = true;
    e.user_id_vld = false;
    e.task_id = 0;
    ApplyTaskAttr(e, cfg.Task(0));
    e.task_fsm = InitFsmOf(cfg.Task(0));
    create_port->Drive(w);
    pending = true;
    refilled_at = s->tail_ptr;
    refilled_vld = true;
    return true;
  }

  bool Handle() {
    if (!trigger->Valid()) return true;
    // 同一笔会连着几拍出现在端口上，按序号认它，已经收下的不再处理。
    if (trigger->Seq() == last_trigger_seq) return true;
    uint64_t user = trigger->user_id.Get();
    uint64_t path = trigger->path_id.Get();
    bool reissue = trigger->reissue.Get() != 0;

    StreamSnapshotPtr s = snap->Get();
    if (!s) return false;

    // 快照里已经出现的，就不必再记在待落表的集合里了。
    for (uint64_t i = 0; i < kStreamNum && !just_created.empty(); ++i) {
      StreamEntry const& e = s->entry[i];
      if (e.valid && e.user_id_vld) just_created.erase(e.user_id);
    }

    // Bypass：自启动 core 与权重加载模式不建 stream 表项，进来的包只把 datain
    // 任务登记进 DataIn_task_table，PC 取 DATAIN_TASK，身份用保留的 SID / TID。
    if (cfg.WeightsMode() || cfg.SelfStartCore()) {
      if (!cfg.DatainValid()) return false;
      if (hold.valid) {
        ++stall_pending;
        return false;
      }
      hold.valid = true;
      hold.task_id = kBypassTid;
      hold.task_pc = cfg.DatainPc();
      hold.user_id = user;
      hold.path_id = path;
      hold.stream_id = kBypassSid;
      last_trigger_seq = trigger->Seq();
      return true;
    }

    // 老用户：复用原来的 stream_id。
    for (uint64_t i = 0; i < kStreamNum; ++i) {
      StreamEntry const& e = s->entry[i];
      if (!e.valid || !e.user_id_vld || e.user_id != user) continue;
      Plan p = PlanFor(path, reissue, e.done_bitmap);
      if (p.task_id < kTaskChainNum && hold.valid) {
        ++stall_pending;
        return false;
      }
      ++matched_pending;
      if (reissue || p.skip != 0) {
        auto w = std::make_shared<StreamWrite>();
        w->valid = true;
        w->stream_id = i;
        w->set_reissue = reissue;
        w->done_mask = p.skip;
        create_port->Drive(w);
        pending = true;
      }
      Register(p, user, path, i);
      last_trigger_seq = trigger->Seq();
      return true;
    }

    // 刚建过表但快照还没更新的那一两拍里，同一个 user 的另一笔 trigger 算不出
    // 它做完了哪几项，先等快照跟上。
    if (just_created.count(user) != 0) return false;

    // 新用户：两条同时满足才建表。
    if (!cfg.TriggerChainEn()) return false;  // 不允许启动任务链
    // 在途上限与 Refill() 那一处一样，看 CFG_REG 里软件配的 stream_num。
    if (s->InFlight() >= cfg.StreamNum()) {
      ++stall_pending;
      return false;
    }
    Plan p = PlanFor(path, reissue, 0);
    if (p.task_id < kTaskChainNum && hold.valid) {
      ++stall_pending;
      return false;
    }

    uint64_t slot = s->tail_ptr;
    auto w = std::make_shared<StreamWrite>();
    w->valid = true;
    w->stream_id = slot;
    w->whole = true;
    StreamEntry& e = w->entry;
    e.valid = true;
    e.user_id = user;
    e.user_id_vld = true;
    e.reissue = reissue;
    e.task_id = 0;
    e.done_bitmap = p.skip;
    TaskEntry const& t0 = cfg.Task(0);
    ApplyTaskAttr(e, t0);
    e.task_fsm = (p.skip & 1u) ? TaskFsm::kFinish : InitFsmOf(t0);
    create_port->Drive(w);
    pending = true;
    just_created.insert(user);
    Register(p, user, path, slot);
    last_trigger_seq = trigger->Seq();
    return true;
  }

  // 要派 DTE 的那一笔登记进 DataIn_task_table，PC 取那一项自己的 TASK_PC。
  void Register(Plan const& p, uint64_t user, uint64_t path, uint64_t stream) {
    if (p.task_id >= kTaskChainNum) return;
    hold.valid = true;
    hold.task_id = p.task_id;
    hold.task_pc = cfg.Task(p.task_id).task_pc;
    hold.user_id = user;
    hold.path_id = path;
    hold.stream_id = stream;
  }

  CfgReg& cfg;
  std::shared_ptr<TriggerPort> trigger;
  std::shared_ptr<SnapshotPort> snap;
  std::shared_ptr<StreamWritePortIf> create_port;

  // Step 独占。
  DatainHold hold;
  uint64_t refilled_at = 0;
  bool refilled_vld = false;
  // 已经发出建表请求、但还没在快照里露面的那些 user。
  std::set<uint64_t> just_created;
  uint64_t last_trigger_seq = 0;
  bool pending = false;
  uint64_t created_pending = 0, matched_pending = 0, stall_pending = 0;

  Logic64 created, matched, stalls;
};

}  // namespace bach
}  // namespace latch

#endif
