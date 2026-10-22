#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_STREAM_TABLE_
#define _LATCH_BACH_IP_CHIP_CORE_TS_STREAM_TABLE_

// Stream_table：16 项顺序 FIFO，每项对应一条完整用户业务流。
//
// head_ptr 与 tail_ptr 环形推进：建表推 tail，退休推 head。自启动 core 上表一直
// 是满的：退休的队头在同一个 stream 原地重新激活，两个指针同一步各推一格，按
// stream_num 环回。
//
// 各写口按固定优先级仲裁，每口一拍一笔，请求保持到 accepted 才算生效。优先级
// 由高到低是 retirement、completion、install、issue、credit_wake、create，让
// 表项先腾空再填新的，回收类排在生成类前面，create 排最后，队头卡住时不会因为
// 新用户不断插队而饿死。
//
// 落到不同 stream 的写互不相干，同一拍可以都做；落到同一个 stream
// 的按优先级排， 没轮上的不回 accepted，请求方下一拍再来。
//
// 表内容每拍末发布成一份快照。别的模块读上一拍的快照来做判断，不直接碰这里的
// 容器：那样是跨线程读非 atomic 容器，读数取决于两个协程谁先跑。

#include <array>
#include <memory>
#include <string>

#include "bach/ip/chip/core/router/retire.h"
#include "bach/ip/chip/core/ts/cfg_reg.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/chip/core/ts/user_match.h"
#include "bach/ip/module_base.h"
#include "base/log.h"

namespace latch {
namespace bach {

class StreamTable : public BachModule {
public:
  // 表深固定 kStreamNum 项，是物理表的规模。本次用其中几项由软件配 CFG_REG，
  // 判满的人自己去读那个值，表这边不管。
  StreamTable(ClockPtr clock, const std::string &name, uint64_t parent = 0,
              bool tick = true)
      : BachModule(clock, name, parent, tick),
        snap_port(std::make_shared<SnapshotPort>(clock)), in_flight(clock),
        writes(clock), conflicts(clock),
        direction_owners(std::make_shared<DirectionOwnerPort>(clock)),
        slot_reuse(std::make_shared<RetireBroadcastPort>(clock)) {
    for (uint64_t p = 0; p < kWrPortNum; ++p) {
      ports.push_back(std::make_shared<StreamWritePortIf>(clock));
    }
    // 复位后先发一份空快照，免得第一拍读到空指针。
    snap_port->Drive(MakeSnapshot());
  }

  StreamWritePortIf &Port(uint64_t p) { return *ports.at(p); }
  // 装配层把请求方那一侧的口接过来：一个写口是同一个对象的两端。
  void Rebind(uint64_t p, std::shared_ptr<StreamWritePortIf> port) {
    ports.at(p) = std::move(port);
  }
  std::shared_ptr<StreamWritePortIf> PortPtr(uint64_t p) const {
    return ports.at(p);
  }
  std::shared_ptr<SnapshotPort> SnapPtr() const { return snap_port; }

  // 自启动的 core：上电配完直接建满表项，不等 Router
  // trigger。此时还没有用户信息， 等自启动任务的 RV core 返回 user_id
  // 后再补进表项。
  void AttachRetire(std::shared_ptr<RetirePort> p) {
    retire_out = std::move(p);
  }
  void AttachDirectionReturns(std::shared_ptr<DirectionReturnPort> p) {
    returns = std::move(p);
  }
  std::shared_ptr<DirectionOwnerPort> DirectionOwnersPtr() const {
    return direction_owners;
  }
  void AttachDatainGuard(std::shared_ptr<DatainCandidatePort> p) {
    datain_guard = std::move(p);
  }
  std::shared_ptr<RetireBroadcastPort> SlotReusePtr() const {
    return slot_reuse;
  }
  void AttachCfg(CfgReg const &reg) { cfg = &reg; }
  void Configure(uint64_t n) {
    LOGCHECK(n >= 1 && n <= kStreamNum, "StreamTable: 启用槽数非法。");
    slot_num = n;
    credit_count.fill(n);
  }
  void SelfStart(TaskEntry const &task0, uint64_t n) {
    Configure(n);
    for (uint64_t i = 0; i < n; ++i) {
      table[i] = SelfStartEntry(task0);
      table[i].allocation = ++generations[i];
    }
    head_ptr = 0;
    tail_ptr = n - 1;
    occupied = n;
    started = true;
    snap_port->Drive(MakeSnapshot());
  }

  // JoinAll 之后主线程读，验收用。
  StreamEntry const &Peek(uint64_t i) const { return table.at(i); }
  uint64_t HeadPtr() const { return head_ptr; }
  uint64_t TailPtr() const { return tail_ptr; }
  // 本拍末表里还剩几项没退休。环形指针模 2*kStreamNum，在途数按环形差来，
  // 不能做无符号直接相减：绕回以后 tail < head，减出会炸成接近 2^64 的数。
  //
  // 两个口径：
  //   InFlight() 读打拍的寄存器，别的协程在仿真里用它判这条链走没走空；
  //   Occupied() 按 head/tail 的环形差直接算，主线程 JoinAll 之后验收读它。
  // 主线程的 Base 停在 0，直接读打拍寄存器只能拿到复位值，所以要分开口径。
  uint64_t InFlight() const { return in_flight.Get(); }
  uint64_t Occupied() const { return occupied; }
  uint64_t Writes() const { return writes.Get(); }
  uint64_t Conflicts() const { return conflicts.Get(); }

  bool Quiescent() const override {
    for (uint64_t i = 0; i < kStreamNum; ++i) {
      if (table[i].valid)
        return false;
    }
    return true;
  }

protected:
  void Step() override {
    if (cfg && cfg->InitFinished() && !occupied && !started &&
        slot_num != cfg->StreamNum())
      Configure(cfg->StreamNum());
    Arbitrate();
    snap_port->Drive(MakeSnapshot());
    uint64_t occ = Occupied();
    in_flight = occ;
    writes = write_pending;
    conflicts = conflict_pending;
    TracePerCycle("in_flight", occ);
    TracePerCycle("writes", write_pending);
  }

private:
  struct Update {
    uint64_t port;
    std::shared_ptr<StreamWrite> req;
  };

  void Arbitrate() {
    auto owners = std::make_shared<std::vector<DirectionOwner>>();
    slot_reuse->Idle();
    if (retire_out)
      retire_out->Idle();
    std::array<std::vector<Update>, kStreamNum> updates;
    std::array<uint64_t, kWrPortNum> accepted{};
    std::array<bool, kWrPortNum> consumed{};
    for (uint64_t p = 0; p < kWrPortNum; ++p) {
      if (!ports[p]->Valid() || ports[p]->Seq() == last_seq[p])
        continue;
      auto root = ports[p]->Req();
      if (!root || !root->valid)
        continue;
      last_seq[p] = ports[p]->Seq();
      consumed[p] = true;
      if (root->special_start) {
        if (!started && cfg)
          SelfStart(cfg->Task(0), cfg->StreamNum());
        ++accepted[p];
        continue;
      }
      if (!root->updates.empty()) {
        for (auto const &w : root->updates) {
          if (w && w->valid && w->stream_id < slot_num)
            updates[w->stream_id].push_back({p, w});
        }
      } else {
        LOGCHECK(root->stream_id < slot_num, "StreamTable: SID不在启用范围。");
        updates[root->stream_id].push_back({p, root});
      }
    }
    for (uint64_t sid = 0; sid < slot_num; ++sid) {
      StreamEntry &e = table[sid];
      auto const &row = updates[sid];
      if (datain_guard)
        e.datain_busy = (datain_guard->query_mask.Get() & (1ull << sid)) != 0;
      bool protect = e.datain_busy;
      for (auto const &u : row) {
        if (u.req->set_datain_busy && u.req->datain_busy)
          protect = true;
      }
      bool recreated = false;
      // 生命周期先判，普通Create与退休同拍可复用SID。
      for (auto const &u : row) {
        auto const &w = *u.req;
        if (u.port != kWrRetirement || protect || !e.valid ||
            (w.check_allocation && w.allocation != e.allocation))
          continue;
        if (cfg &&
            (!e.user_id_vld || e.rmem_busy || !cfg->ThroughEndMask() ||
             (e.done_bitmap & cfg->ThroughEndMask()) != cfg->ThroughEndMask()))
          continue;
        if (w.reactivate) {
          if (e.user_id_vld)
            slot_reuse->Drive(e.user_id, ++slot_reuse_seq);
          StreamEntry fresh = w.entry;
          fresh.allocation = ++generations[sid];
          e = fresh;
          head_ptr = (head_ptr + 1) % slot_num;
          tail_ptr = sid;
          recreated = true;
          ++accepted[u.port];
        } else if (w.clear_valid) {
          if (retire_out)
            retire_out->Drive(e.user_id);
          e = StreamEntry{};
          LOGCHECK(occupied > 0, "StreamTable: 退休空表。");
          --occupied;
          if (occupied)
            head_ptr = (head_ptr + 1) % slot_num;
          else
            head_ptr = tail_ptr = sid;
          ++accepted[u.port];
        }
      }
      for (auto const &u : row) {
        auto const &w = *u.req;
        if (u.port != kWrCreate || !w.whole || e.valid)
          continue;
        e = w.entry;
        e.allocation = ++generations[sid];
        if (!occupied)
          head_ptr = sid;
        tail_ptr = sid;
        ++occupied;
        recreated = true;
        if (cfg && e.valid) {
          auto const &t = cfg->Task(e.task_id);
          e.credit_drct = t.credit_en && !t.IsReduce()
                              ? cfg->Route(e.task_path_id).Directions()
                              : 0;
          if (e.task_fsm == TaskFsm::kWaitCredit && !e.credit_drct)
            e.task_fsm = TaskFsm::kReady;
        }
        ++accepted[u.port];
      }
      if (!e.valid)
        continue;
      // Merge all completion and skip bits before any debit, including a
      // UserMatch skip carried on a lower-priority physical update lane.
      for (auto const &u : row) {
        auto const &w = *u.req;
        if (u.port == kWrRetirement || w.install ||
            (u.port == kWrCreate && w.whole) ||
            (recreated && u.port != kWrCreate) ||
            (w.check_allocation && w.allocation != e.allocation))
          continue;
        e.done_bitmap |=
            w.done_mask | (w.set_done_bit ? 1ull << w.done_bit : 0);
      }
      bool rmem_was_busy = e.rmem_busy;
      // Completion/Skip按位合并，RMEM/claim/查询保护按字段提交。
      for (auto const &u : row) {
        auto const &w = *u.req;
        if (u.port == kWrRetirement || w.install ||
            (u.port == kWrCreate && w.whole) ||
            (recreated && u.port != kWrCreate) ||
            (w.check_allocation && w.allocation != e.allocation))
          continue;
        if (w.grant_dirs) {
          if (e.task_id != w.from_task_id ||
              e.task_fsm != TaskFsm::kWaitCredit ||
              (e.done_bitmap & (1ull << e.task_id)) ||
              (w.grant_dirs & e.drct_used) ||
              (cfg && w.grant_dirs !=
                          (cfg->Route(e.task_path_id).Directions() & ~e.drct_used)))
            continue;
          bool enough = true;
          for (uint64_t dir = 0; dir < 3; ++dir)
            if ((w.grant_dirs & (1ull << dir)) && credit_count[dir] == 0)
              enough = false;
          if (!enough)
            continue;
          for (uint64_t dir = 0; dir < 3; ++dir)
            if (w.grant_dirs & (1ull << dir))
              --credit_count[dir];
          owners->push_back({e.user_id, w.grant_dirs});
        }
        if (w.set_datain_busy)
          e.datain_busy = w.datain_busy;
        e.datain_claimed_mask |= w.claim_mask;
        e.datain_accepted_mask |= w.datain_accept_mask;
        uint64_t done = w.done_mask | (w.set_done_bit ? 1ull << w.done_bit : 0);
        e.done_bitmap |= done;
        if (w.set_user_id) {
          e.user_id = w.user_id;
          e.user_id_vld = true;
        }
        if (w.set_pid) {
          e.task_path_id = w.pid;
          e.pid_pending = true;
        }
        if (w.set_reissue)
          e.reissue = true;
        if (w.clear_reissue)
          e.reissue = false;
        if (w.give_rmem)
          e.rmem_busy = false;
        if (w.take_rmem && !w.give_rmem && !rmem_was_busy) {
          e.rmem_busy = true;
          e.rmem_task = e.task_id;
          e.rmem_pid = e.task_path_id;
        }
        if (w.grant_dirs)
          e.drct_used |= w.grant_dirs;
        if (w.set_fsm && (!w.fsm_if_current || w.from_task_id == e.task_id) &&
            !(e.done_bitmap & (1ull << e.task_id)))
          e.task_fsm = w.fsm;
        ++accepted[u.port];
      }
      // TG只替换Current属性，不覆盖其它来源提交的用户级状态。
      for (auto const &u : row) {
        auto const &w = *u.req;
        if (!(w.install || (u.port == kWrInstall && w.whole)) || recreated)
          continue;
        if ((w.check_allocation && w.allocation != e.allocation) ||
            (w.install && w.from_task_id != e.task_id) ||
            (e.done_bitmap & (1ull << w.entry.task_id))) {
          ++conflict_pending;
          continue;
        }
        uint64_t old_tid = e.task_id;
        uint64_t pid = e.pid_pending && w.entry.task_id == old_tid + 1
                           ? e.task_path_id
                           : w.entry.task_path_id;
        e.task_id = w.entry.task_id;
        e.task_unit = w.entry.task_unit;
        e.task_recv = w.entry.task_recv;
        e.task_type = w.entry.task_type;
        e.task_pc = w.entry.task_pc;
        e.task_path_id = pid;
        e.end = w.entry.end;
        e.pid_pending = false;
        e.credit_drct = w.entry.credit_drct;
        if (cfg) {
          auto const &task = cfg->Task(e.task_id);
          e.credit_drct =
              task.credit_en && !task.IsReduce() ? cfg->Route(pid).Directions() : 0;
        }
        e.task_fsm = w.entry.task_fsm;
        if (e.task_fsm == TaskFsm::kWaitCredit &&
            !(e.credit_drct & ~e.drct_used))
          e.task_fsm = TaskFsm::kReady;
        ++accepted[u.port];
      }
      if (e.datain_accepted_mask & (1ull << e.task_id))
        e.task_fsm = TaskFsm::kInfly;
      if (e.done_bitmap & (1ull << e.task_id))
        e.task_fsm = TaskFsm::kFinish;
    }
    direction_owners->owners = owners;
    uint64_t returned = returns ? returns->valid_mask.Get() : 0;
    for (uint64_t dir = 0; dir < 3; ++dir)
      if (returned & (1ull << dir)) {
        ++credit_count[dir];
        LOGCHECK(credit_count[dir] <= slot_num, "TS方向credit重复或超额归还。");
      }
    for (uint64_t p = 0; p < kWrPortNum; ++p) {
      if (consumed[p])
        ports[p]->DriveAcceptedCount(accepted[p]);
      else
        ports[p]->DriveAccepted(false);
      write_pending += accepted[p];
    }
  }

  StreamSnapshotPtr MakeSnapshot() const {
    auto s = std::make_shared<StreamSnapshot>();
    s->entry = table;
    s->head_ptr = head_ptr;
    s->slot_num = slot_num;
    s->started = started;
    s->credit_count = credit_count;
    // 两个指针取模之后满与空都是相等，差值分不开，在途数单独带出来。
    s->in_flight = Occupied();
    s->tail_ptr = tail_ptr;
    return s;
  }

  std::vector<std::shared_ptr<StreamWritePortIf>> ports;
  std::shared_ptr<SnapshotPort> snap_port;

  // Step 独占。
  std::array<StreamEntry, kStreamNum> table{};
  uint64_t head_ptr = 0, tail_ptr = 0, occupied = 0, slot_num = kStreamNum;
  std::array<uint64_t, kStreamNum> generations{};
  bool started = false;
  std::array<uint64_t, 3> credit_count{{16, 16, 16}};
  std::shared_ptr<DirectionReturnPort> returns;
  std::shared_ptr<RetirePort> retire_out;
  std::shared_ptr<DirectionOwnerPort> direction_owners;
  std::shared_ptr<RetireBroadcastPort> slot_reuse;
  uint64_t slot_reuse_seq = 0;
  CfgReg const *cfg = nullptr;
  std::shared_ptr<DatainCandidatePort> datain_guard;
  std::array<uint64_t, kWrPortNum> last_seq{};
  uint64_t write_pending = 0, conflict_pending = 0;

  Logic64 in_flight, writes, conflicts;
};

} // namespace bach
} // namespace latch

#endif
