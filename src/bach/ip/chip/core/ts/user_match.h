#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_USER_MATCH_
#define _LATCH_BACH_IP_CHIP_CORE_TS_USER_MATCH_

// User_Match 与 DataIn_task_table。
//
// Router 的 trigger 进来时这两件事一起判：拿 user_id 与 stream_table 里的比对，
// 没匹配上就是新用户、要建表；同一笔请求还要往 DataIn_task_table 登记一格。
// 两边任一处不接受，这一笔就整体停住。
//
// trigger 口不设入口队列。四条不全满足时直接拉低 ready，由 CoreStation 保持这一笔，
// 每拍重判一次，不丢弃、不越过。这条通路上的 trigger 与 token 一一对应，丢一笔就
// 等于丢一个 token，所以只许反压不许丢。
//
// 不设队列的理由：请求的保持责任本来就在 CoreStation 那一侧，加一级队列只是把
// 同一个反压点往后挪一格，既不改变正确性也不提高吞吐，反而多一处要维护的保序状态。

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
// 一起登记进去；被占住时不再接收新的 datain 请求。
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
    // 上一笔建表请求还没被 stream_table 收下就原地保持。
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
    // 占的是建表口，而这一档 core 上进来的包不建表项、只登记 DataIn_task_table
    // （F28），两者同一拍互不相干，所以补完表照常往下处理 trigger。挡住的话
    // stream_num 配大之后补表一直有活干，datain 就再也轮不上。
    Refill();

    bool ok = Handle();
    trigger->DriveReady(ok);

    created = created_pending;
    matched = matched_pending;
    stalls = stall_pending;
    TracePerCycle("created", created_pending);
  }

 private:
  // 自启动 core 上在途表项少于 stream_num 时补一个。建表那一刻没有用户信息，
  // 等自启动任务的 RV core 认出用户之后随完成补进来。
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
    e.compute = true;
    e.task_id = 0;
    e.task_fsm = TaskFsm::kReady;
    StreamTableApply(e, cfg.Task(0));
    create_port->Drive(w);
    pending = true;
    refilled_at = s->tail_ptr;
    refilled_vld = true;
    return true;
  }

  bool Handle() {
    if (!trigger->Valid()) return true;
    uint64_t user = trigger->user_id.Get();
    uint64_t path = trigger->path_id.Get();
    bool reissue = trigger->reissue.Get() != 0;
    bool compute = trigger->compute.Get() != 0;

    StreamSnapshotPtr s = snap->Get();
    if (!s) return false;

    // 快照里已经出现的，就不必再记在待落表的集合里了。
    for (uint64_t i = 0; i < kStreamNum && !just_created.empty(); ++i) {
      StreamEntry const& e = s->entry[i];
      if (e.valid && e.user_id_vld) just_created.erase(e.user_id);
    }

    // B core、R core 与 weights 加载模式这三档不建 stream 表项：进来的包只把
    // datain 任务登记进 DataIn_task_table，PC 取 DATAIN_TASK 那一项。这三档全
    // 链只有一个 datain 任务，task_chain 里不再配 DTE 的 datain。
    if (cfg.WeightsMode() || cfg.SelfStartCore()) {
      if (!cfg.DatainValid()) return false;
      if (hold.valid) {
        ++stall_pending;
        return false;
      }
      hold.valid = true;
      hold.task_id = 0;
      hold.task_pc = cfg.DatainPc();
      hold.user_id = user;
      hold.path_id = path;
      // 没有表项，也就没有槽位号。搬到哪由软件自己按包头算，不靠 stream 偏移。
      hold.stream_id = 0;
      return true;
    }

    // 老用户：复用原来的 stream_id，不改它当前的任务、状态、完成位。只有请求
    // 带着重发标记时才把该项的 reissue 置起来。
    for (uint64_t i = 0; i < kStreamNum; ++i) {
      StreamEntry const& e = s->entry[i];
      if (!e.valid || !e.user_id_vld || e.user_id != user) continue;
      ++matched_pending;
      if (reissue) {
        auto w = std::make_shared<StreamWrite>();
        w->valid = true;
        w->stream_id = i;
        w->set_reissue = true;
        create_port->Drive(w);
        pending = true;
      }
      return true;
    }

    // 刚建过表但快照还没更新的那一两拍里，同一个 user 会被再判成新用户。
    // trigger 是 valid/ready 握手，CoreStation 要保持到看见 ready，所以同一笔
    // 请求本来就会连着几拍出现在端口上 —— 不记这一笔就会建两次表。
    if (just_created.count(user) != 0) return true;

    // 新用户：四条同时满足才建表。
    if (!cfg.TriggerChainEn()) return false;      // 不允许启动任务链
    // 在途上限与 Refill() 那一处一样，看 CFG_REG 里软件配的 stream_num，不是
    // 建 Stream_table 时给的物理表深。
    if (s->InFlight() >= cfg.StreamNum()) {
      ++stall_pending;
      return false;
    }
    // 同一笔请求还要往 DataIn_task_table 登记，那一格占住时整体停住。
    PathTaskMap const& m = cfg.PathMap(path);
    bool need_datain = m.valid && cfg.Task(m.task_id).IsDataIn();
    if (need_datain && hold.valid) {
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
    e.compute = compute;
    e.task_id = 0;
    TaskEntry const& t0 = cfg.Task(0);
    StreamTableApply(e, t0);
    // Task 0 的初始状态按类型定：datain 或 reissue 置 WAIT，self_start 置 READY。
    if (t0.IsDataIn() || t0.IsReissue()) {
      e.task_fsm = TaskFsm::kWait;
    } else if (t0.self_start) {
      e.task_fsm = TaskFsm::kReady;
    } else {
      e.task_fsm = TaskFsm::kReady;
    }
    create_port->Drive(w);
    pending = true;
    just_created.insert(user);

    // datain 的 task_pc 不取 DATAIN_TASK 那一项：先按 path_id 查出 task_id，
    // 再取 task_chain[t].TASK_PC。一条链上有多个 datain 任务时靠这一步分开。
    if (need_datain) {
      hold.valid = true;
      hold.task_id = m.task_id;
      hold.task_pc = cfg.Task(m.task_id).task_pc;
      hold.user_id = user;
      hold.path_id = path;
      hold.stream_id = slot;
    }
    return true;
  }

  // 摊平当前 task 的属性。与 StreamTable::ApplyTaskAttr 同一件事，这里不引它是
  // 为了不让 UserMatch 依赖 StreamTable 的类型。
  static void StreamTableApply(StreamEntry& e, TaskEntry const& t) {
    e.task_unit = t.send_unit;
    e.task_recv = t.recv_unit;
    e.task_dsa_en = t.dsa_en;
    e.task_pc = t.task_pc;
    e.task_path_id = t.path_id;
    e.is_reissue = t.IsReissue();
    e.end = t.end;
    e.reduce_num = t.reduce ? t.reduce_num : 0;
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
  bool pending = false;
  uint64_t created_pending = 0, matched_pending = 0, stall_pending = 0;

  Logic64 created, matched, stalls;
};

}  // namespace bach
}  // namespace latch

#endif
