#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_DTE_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_DTE_

// 搬运引擎。Core 上所有进出的数据都经过它，本地的 reduce 加法也在它这里做。
//
// 它同时是两个方向的引擎，两个方向共用同一组闸门、各自一条执行通道：
//
//   出方向  TaskScheduler 派一条 DTE 任务下来，它按任务表算出目的地与数据量，逐拍
//           把这一包送出去，送完 ack
//   入方向  链路上收到的每一拍先攒着，攒齐一整包才开始处理，处理完 ack 对应的那条
//           占位任务，TaskScheduler 才能往下走
//
// 两个方向都要过的四段闸门：
//
//   准入      优先级队列，容量 setup_ahead_depth 加 1，令牌持到任务做完
//   setup     优先级队列，容量 1，占 dte_setup_time 拍，计时结束就放开
//   执行通道  先进先出，容量 1。split 模式下出入方向各一条，shared 模式下合用一条，
//             合用意味着一次收包会挡住一次发包。开了五路径仲裁之后这一段换成按路径
//             排队，见 dsa.h
//   功能单元  FIFO 指针与 reduction 加法器各一个，容量 1。它们在两条执行通道下面，
//             临界区窄，只挡真正用到同一个功能的动作
//
// RETIRE 收包是唯一不过闸门的路径：它只把额度还给上游，不占任何资源，也不 ack。
// 这条路径要是也排队，额度归还就会被正在等额度的任务挡住，直接死锁。
//
// 收包按 uid、layer_id、tid、tag、opcode 五元组攒拍，攒齐 total_fragments 拍才算
// 一包。同一个键收到重复的拍号、越界的拍号，都是路由错乱的证据，当场断言而不是丢掉。

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/log.h"
#include "base/runtime.h"
#include "base/time_stamp.h"
#include "bach/common/arbiter.h"
#include "bach/common/packet.h"
#include "bach/common/params.h"
#include "bach/ip/chip/core/core_context.h"
#include "bach/ip/chip/core/dte/dsa.h"
#include "bach/ip/chip/core/memory/memory_system.h"
#include "bach/ip/chip/core/moe_bitmap.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/sub_unit.h"

namespace latch {
namespace bach {

class Dte : public SubUnit, public PacketTarget {
 public:
  Dte(ClockPtr clock, CoreContext const& context, std::string const& name,
      uint64_t parent, bool standalone = false)
      : SubUnit(clock, standalone),
        ctx(context),
        admission(context.P().setup_ahead_depth + 1),
        setup(1),
        dsa(&split_lane, &comm_lane, context.node_id) {
    RegisterId(name, parent);
    handle_lane = context.P().dte_execution_mode == DteExecutionMode::kShared
                      ? &comm_lane
                      : &split_lane;
    // 槽位数取不小于 stream 数的二的幂，与硬件按位图扫描空位一致。
    total_slots = 1;
    while (total_slots < context.P().stream_count) total_slots <<= 1;
    for (uint64_t i = 0; i < total_slots; ++i) free_slots.push_back(i);
    xfer_next = context.node_id << 40;
  }

  void Connect(SchedulerPort* scheduler, CreditReturnPort* credit_unit,
               PacketSink* packet_sink, MoeBitMap* bitmap, MemorySystem* mem) {
    LOGCHECK(scheduler != nullptr, "DTE: scheduler is null.");
    LOGCHECK(credit_unit != nullptr, "DTE: credit unit is null.");
    LOGCHECK(packet_sink != nullptr, "DTE: packet sink is null.");
    LOGCHECK(bitmap != nullptr, "DTE: bitmap is null.");
    LOGCHECK(mem != nullptr, "DTE: memory system is null.");
    sched = scheduler;
    credit = credit_unit;
    sink = packet_sink;
    moe_bitmap = bitmap;
    memory = mem;
  }

  // TaskScheduler 派一条出方向任务。
  void DispatchComm(Time now, uint64_t uid, uint64_t tid, uint64_t priority) {
    LOGCHECK(sched != nullptr, "DTE: dispatch before connect.");
    LOGCHECK(!advancing, "DTE: dispatch reentered from a completion.");
    jobs.emplace_back();
    Job& j = jobs.back();
    j.outgoing = true;
    j.uid = uid;
    j.tid = tid;
    j.priority = priority;
    j.opcode = ctx.Task(tid).opcode;
    j.gate = admission.Enqueue(now, priority, uid);
    RunAdvance(jobs.size() - 1, now);
  }

  // 链路交来一拍。攒齐一整包才往下走。
  void HandleComm(CommInstPtr const& payload) override {
    LOGCHECK(sched != nullptr, "DTE: handle comm before connect.");
    LOGCHECK(payload != nullptr, "DTE: null payload.");
    const Time now = RT::Now();
    CommInst const& c = *payload;

    const uint64_t expect = ExpectedBeats(c);
    LOGCHECK(c.beat_id < expect, "DTE: beat id out of range, route is wrong.");

    BeatKey key = MakeBeatKey(c);
    Beats& b = recv[key];
    LOGCHECK(b.seen.count(c.beat_id) == 0,
             "DTE: duplicate beat, packet was cloned or looped.");
    if (b.seen.empty()) b.first_cycle = now;
    b.seen.insert(c.beat_id);
    if (b.seen.size() < expect) return;

    recv.erase(key);

    // 归还额度不排队、不占资源、不 ack。它得能在任何拥塞下走通。
    if (c.opcode == Opcode::kRetire) {
      credit->ReturnCredit(static_cast<int64_t>(c.tag), 1);
      return;
    }
    LatchHeader(c);

    jobs.emplace_back();
    Job& j = jobs.back();
    j.outgoing = false;
    j.uid = c.uid;
    j.tid = c.tid;
    j.opcode = c.opcode;
    j.priority = sched->StreamPriority(c.uid, true);
    j.payload = payload;
    j.gate = admission.Enqueue(now, j.priority, c.uid);
    RunAdvance(jobs.size() - 1, now);
  }

  void Step() override {
    const Time now = RT::Now();
    CheckArrivals(now);
    for (size_t i = 0; i < jobs.size(); ++i) RunAdvance(i, now);
    Sweep();
  }

  // TaskScheduler 判断某条入边这次会不会来包时要读它。
  std::vector<uint32_t> const* HitMapOf(uint64_t uid) const {
    auto it = user_hit_maps.find(uid);
    return it == user_hit_maps.end() ? nullptr : &it->second;
  }

  StorageMode Storage() const { return storage; }
  uint64_t FreeSlotNum() const { return free_slots.size(); }
  uint64_t FifoActiveNum() const { return fifo_active; }
  // 本核这次没被命中、因而绕过或丢掉的包数。它不为零，说明 HitMap 真的在挡包。
  uint64_t InactiveBypassNum() const { return inactive_bypass; }
  uint64_t InactiveDropNum() const { return inactive_drop; }
  uint64_t TotalSlots() const { return total_slots; }
  bool HasSlot(uint64_t uid) const { return user_to_slot.count(uid) != 0; }
  uint64_t InFlight() const { return jobs.size(); }
  uint64_t PendingBeats() const { return recv.size(); }

  // 五路径仲裁留下的痕迹。跑完之后交给 AnalyzeDsa，仿真期谁也不读它。
  std::vector<DsaRecord> const& DsaRecords() const { return dsa.Records(); }
  uint32_t DsaActiveMask() const { return dsa.ActiveMask(); }

 private:
  enum class Stage : uint32_t {
    kAdmission = 0,
    kSetupQueue = 1,
    kSetup = 2,
    kLane = 3,
    kDsaWait = 4,       // 五路径仲裁开着时代替 kLane
    kExec = 5,          // 进执行通道后按 opcode 分叉
    kBitmapRead = 6,
    kBitmapWrite = 7,
    kFunctionQueue = 8,
    kTimer = 9,         // 等一段固定时长走完，reduce 与本地搬运都用它
    kMemWait = 10,      // 等一次访存做完
    kSend = 11,
    kDone = 12,         // 收尾：ack、放资源、记占用
    kFinished = 13,
  };

  // 拿到功能单元之后要做的那件事。功能单元只有 FIFO 指针与 reduction 加法器两个，
  // 但同一个单元上要做的事不止一件，所以分开记。
  enum class FuncOp : uint32_t {
    kNone = 0,
    kReduce = 1,
    kFifoPush = 2,
    kFifoPop = 3,
    kFifoRetire = 4,
    kReductionInit = 5,
    kReductionRetire = 6,
  };

  // 一条任务有时要连发好几个包：本核这次不激活时，既要把包转给下游，又要把额度还给
  // 上游。它们共用一条执行通道，按顺序一个接一个发。
  struct PendingSend {
    uint64_t task_id = 0;
    Opcode opcode = Opcode::kDontCare;
    uint64_t tid = 0;
    uint64_t tag = 0;
  };

  struct Job {
    bool outgoing = true;
    uint64_t uid = 0;
    uint64_t tid = 0;
    uint64_t priority = 0;
    Opcode opcode = Opcode::kDontCare;
    Stage stage = Stage::kAdmission;

    Ticket gate = kNoTicket;      // 当前闸门用的票
    Ticket admit = kNoTicket;     // 准入令牌
    Ticket lane_ticket = kNoTicket;
    Ticket func_ticket = kNoTicket;
    Ticket bitmap_ticket = kNoTicket;
    ExclusiveArbiter* lane = nullptr;
    ExclusiveArbiter* func = nullptr;
    Time func_grant = 0;

    Time timer_end = 0;
    Time exec_start = 0;
    Time send_start = 0;
    uint64_t burst_len = 0;
    uint64_t next_beat = 0;
    uint64_t send_tag = 0;
    uint64_t send_tid = 0;
    // 从队列里弹出来的那个 user 带着自己的身份走，未必是这条任务自己的
    uint64_t send_uid = 0;
    Opcode send_opcode = Opcode::kDontCare;
    // 这一发按哪条任务行的目的地与数据量走，未必是这条任务自己那行
    uint64_t send_task = 0;
    bool send_task_set = false;
    std::vector<PendingSend> more_sends;

    bool do_ack = true;
    CommInstPtr payload;  // 入方向才有

    // 用户队列那条路上用的
    FuncOp func_op = FuncOp::kNone;
    Ticket mem_ticket = kNoTicket;
    CommInstPtr queued;          // 从队列里弹出来、这次要发的那个 user
    Stage after_bitmap = Stage::kDone;
    bool activate_after_ack = false;

    // 五路径仲裁开着时才用
    DsaPlan plan;
    Ticket dsa_ticket = kNoTicket;
    bool use_dsa = false;
    // 计时那一段记成哪种占用：本地搬运是搬运，reduce 是功能单元
    SpanState timer_state = SpanState::kFunction;
  };

  // ------------------------------------------------------------ 收包攒拍

  struct Beats {
    std::unordered_set<uint64_t> seen;
    Time first_cycle = 0;
  };

  // 收了首拍却迟迟收不齐，说明有一拍在路上丢了或者根本没发出来。它跟拥塞不一样：
  // 拥塞会慢，但一定会到。
  void CheckArrivals(Time now) const {
    for (auto const& kv : recv) {
      LOGCHECK(now - kv.second.first_cycle <= ctx.P().watchdog_lifespan,
               "DTE: a packet started arriving but never completed.");
    }
  }

  uint64_t ExpectedBeats(CommInst const& c) const {
    if (c.opcode == Opcode::kRetire) return 1;
    if (c.total_fragments > 0) return c.total_fragments;
    LOGCHECK(ctx.HasTask(c.tid), "DTE: incoming task id is not in the table.");
    return CalcCycles(static_cast<int64_t>(ctx.Task(c.tid).time_or_vol),
                      ctx.P().dte_bandwidth);
  }

  // ------------------------------------------------------------ 推进

  void RunAdvance(size_t index, Time now) {
    advancing = true;
    Advance(jobs[index], now);
    advancing = false;
  }

  void Advance(Job& j, Time now) {
    bool moved = true;
    while (moved) {
      moved = false;
      switch (j.stage) {
        case Stage::kAdmission:
          if (!admission.Granted(j.gate)) break;
          ctx.Wait(Unit::kDte, j.uid, j.tid, admission.EnqueueCycle(j.gate),
                   admission.GrantCycle(j.gate), WaitReason::kSetupAhead);
          j.admit = j.gate;
          j.gate = setup.Enqueue(now, j.priority, j.uid);
          j.stage = Stage::kSetupQueue;
          moved = true;
          break;

        case Stage::kSetupQueue:
          if (!setup.Granted(j.gate)) break;
          ctx.Wait(Unit::kDte, j.uid, j.tid, setup.EnqueueCycle(j.gate),
                   setup.GrantCycle(j.gate), WaitReason::kCoreSetup);
          j.timer_end = setup.GrantCycle(j.gate) + ctx.P().dte_setup_time;
          j.stage = Stage::kSetup;
          moved = true;
          break;

        case Stage::kSetup:
          if (now < j.timer_end) break;
          ctx.Span(Unit::kDte, j.uid, j.tid, setup.GrantCycle(j.gate),
                   j.timer_end, SpanState::kSetup);
          setup.Release(j.gate, now);
          if (FiveRoute()) {
            BeginDsa(j, now);
            moved = true;
            break;
          }
          j.lane = j.outgoing ? &comm_lane : handle_lane;
          j.lane_ticket = j.lane->Enqueue(now, j.uid);
          j.stage = Stage::kLane;
          moved = true;
          break;

        case Stage::kLane:
          if (!j.lane->Granted(j.lane_ticket)) break;
          ctx.Wait(Unit::kDte, j.uid, j.tid, j.lane->EnqueueCycle(j.lane_ticket),
                   j.lane->GrantCycle(j.lane_ticket),
                   WaitReason::kExecutionLane);
          j.exec_start = j.lane->GrantCycle(j.lane_ticket);
          j.stage = Stage::kExec;
          moved = true;
          break;

        case Stage::kDsaWait:
          if (!dsa.Granted(j.dsa_ticket)) break;
          ctx.Wait(Unit::kDte, j.uid, j.tid, dsa.EnqueueCycle(j.dsa_ticket),
                   dsa.GrantCycle(j.dsa_ticket), WaitReason::kDsaRoute);
          j.exec_start = dsa.GrantCycle(j.dsa_ticket);
          j.stage = Stage::kExec;
          moved = true;
          break;

        case Stage::kExec:
          if (j.outgoing) BeginOutgoing(j, now);
          else BeginIncoming(j, now);
          moved = true;
          break;

        case Stage::kFunctionQueue:
          if (!j.func->Granted(j.func_ticket)) break;
          ctx.Wait(Unit::kDte, j.uid, j.tid, j.func->EnqueueCycle(j.func_ticket),
                   j.func->GrantCycle(j.func_ticket),
                   WaitReason::kFunctionUnit);
          j.func_grant = j.func->GrantCycle(j.func_ticket);
          BeginFunction(j, now);
          moved = true;
          break;

        case Stage::kMemWait:
          if (!MemDone(j)) break;
          FinishMemory(j, now);
          moved = true;
          break;

        case Stage::kTimer:
          if (now < j.timer_end) break;
          ctx.Span(Unit::kDte, j.uid, j.tid, j.func_grant, j.timer_end,
                   j.timer_state);
          if (j.func != nullptr) {
            j.func->Release(j.func_ticket, now);
            j.func = nullptr;
            j.func_ticket = kNoTicket;
          }
          j.stage = Stage::kDone;
          moved = true;
          break;

        case Stage::kBitmapRead:
          if (!moe_bitmap->Done(j.bitmap_ticket)) break;
          j.send_tag = moe_bitmap->Value(j.bitmap_ticket);
          moe_bitmap->Finish(j.bitmap_ticket);
          j.bitmap_ticket = kNoTicket;
          StartSend(j, now);
          moved = true;
          break;

        case Stage::kBitmapWrite:
          if (!moe_bitmap->Done(j.bitmap_ticket)) break;
          moe_bitmap->Finish(j.bitmap_ticket);
          j.bitmap_ticket = kNoTicket;
          if (j.after_bitmap == Stage::kFunctionQueue) {
            j.func = &fifo_function;
            j.func_ticket = j.func->Enqueue(now, j.uid);
          }
          j.stage = j.after_bitmap;
          moved = true;
          break;

        case Stage::kSend:
          // 一拍一拍地发。now 与本拍该发的拍号绑定，所以同一拍被推进两次也只发一次。
          if (j.next_beat < j.burst_len &&
              now >= j.send_start + j.next_beat) {
            Emit(j);
            ++j.next_beat;
          }
          if (now < j.send_start + j.burst_len) break;
          ctx.Span(Unit::kDte, j.uid, j.tid, j.send_start,
                   j.send_start + j.burst_len, SpanState::kTransfer,
                   ctx.Task(j.send_task).time_or_vol);
          if (!j.more_sends.empty()) {
            PendingSend next = j.more_sends.front();
            j.more_sends.erase(j.more_sends.begin());
            j.send_task = next.task_id;
            j.send_opcode = next.opcode;
            j.send_tid = next.tid;
            j.send_tag = next.tag;
            StartSend(j, now);
            moved = true;
            break;
          }
          j.stage = Stage::kDone;
          moved = true;
          break;

        case Stage::kDone:
          if (j.do_ack) sched->Ack(j.uid, j.tid);
          // 退休腾出一个位置，队列里排在后面的 user 这一刻才轮得到被认领
          if (j.activate_after_ack) ActivateFifoWindow(now);
          // 占了零拍也是一次占用：这条任务确实用过执行通道，按次数算的分析要看见它
          ctx.Span(Unit::kDte, j.uid, j.tid, j.exec_start, now,
                   SpanState::kExecution, 0, /*allow_zero=*/true);
          if (j.func != nullptr) {
            j.func->Release(j.func_ticket, now);
            j.func = nullptr;
            j.func_ticket = kNoTicket;
          }
          // 走五路径那条路时，逻辑请求与它占的一两条物理通道一起放开；不搬数据的那类
          // 请求两边都没占过，什么也不用放
          if (j.use_dsa) dsa.Release(j.dsa_ticket, now);
          else if (j.lane != nullptr) j.lane->Release(j.lane_ticket, now);
          admission.Release(j.admit, now);
          j.stage = Stage::kFinished;
          moved = true;
          break;

        case Stage::kFinished:
          break;
      }
    }
  }

  // ------------------------------------------------------------ 五路径

  bool FiveRoute() const {
    return ctx.P().dte_dsa_mode == DteDsaMode::kFiveRoute;
  }

  // setup 走完之后定下这次走哪条路径，再去抢它。
  //
  // 三个运行时分支要在这里就判出来，因为它们决定占不占物理通道：本地的残差求和与末端
  // 核的退休两头都不动数据，不该占；本核这次不激活、只把包往下游转的那种两头都要动，
  // 占两条。剩下的按任务元数据里写明的路径走，没写就只认方向。
  void BeginDsa(Job& j, Time now) {
    TaskMeta const& m = ctx.Meta(j.tid);
    const bool local_res_sum =
        j.outgoing && j.opcode == Opcode::kRes && m.IsLocalReduce();
    const bool local_retire =
        j.outgoing && j.opcode == Opcode::kRetire && m.no_credit_return;
    const bool inactive_forward =
        !j.outgoing && j.payload != nullptr &&
        (j.opcode == Opcode::kReduction || j.opcode == Opcode::kFifoIn) &&
        !HitMapMatches(*j.payload);

    j.plan = ResolveDsaPlan(j.outgoing, j.opcode, m, ctx.node_id,
                            inactive_forward, local_res_sum, local_retire);
    if (j.plan.IsNonTransfer()) {
      j.exec_start = now;
      j.stage = Stage::kExec;
      return;
    }
    j.use_dsa = true;
    j.dsa_ticket = dsa.Request(now, j.plan, j.uid, j.tid);
    j.stage = Stage::kDsaWait;
  }

  // MatrixMem 到 Core 的那一条不上链路：按数据量占对应的拍数，做完就 ack。
  void BeginLocalMove(Job& j, Time now) {
    j.func_grant = now;
    j.timer_end =
        now + CalcCycles(static_cast<int64_t>(ctx.Task(j.tid).time_or_vol),
                         ctx.P().dte_bandwidth);
    j.timer_state = SpanState::kTransfer;
    j.stage = Stage::kTimer;
  }

  // ------------------------------------------------------------ 出方向

  void BeginOutgoing(Job& j, Time now) {
    TaskEntry const& e = ctx.Task(j.tid);
    TaskMeta const& m = ctx.Meta(j.tid);

    switch (j.opcode) {
      case Opcode::kUserInit:
        // 下游要的倍率是本核当前的值，所以先读自己的表，读到的数当 tag 发出去。
        j.send_tid = 0;
        j.bitmap_ticket = moe_bitmap->BeginRead(now, j.uid, j.tid);
        j.stage = Stage::kBitmapRead;
        return;

      case Opcode::kRetire:
        if (m.no_credit_return) {
          // 末端核的退休不往上游发包，只把槽位收回来。
          FreeSlot(j.uid);
          j.stage = Stage::kDone;
          return;
        }
        j.send_tid = j.tid;
        j.send_tag = ctx.node_id;
        if (reduction_active.count(j.uid) != 0) {
          j.func = &reduction_function;
          j.func_op = FuncOp::kReductionRetire;
          j.func_ticket = j.func->Enqueue(now, j.uid);
          j.stage = Stage::kFunctionQueue;
          return;
        }
        if (storage == StorageMode::kFifo) {
          // 广播核的退休要先把这个 user 从队列里摘掉，摘的动作占着 FIFO 指针，
          // 而指针一直持到这条任务做完，中间不放开
          j.func = &fifo_function;
          j.func_op = FuncOp::kFifoRetire;
          j.func_ticket = j.func->Enqueue(now, j.uid);
          j.stage = Stage::kFunctionQueue;
          return;
        }
        LOGCHECK(storage == StorageMode::kCore,
                 "DTE: this storage mode's retire is not modelled yet.");
        FreeSlot(j.uid);
        StartSend(j, now);
        return;

      case Opcode::kFifoIn:
      case Opcode::kFifoOut:
        // 从队列里弹一个 user 出来发给下游。FIFO_OUT 发出去时换成 USER_INIT，
        // 因为下游那边这是一个新 user 的开始，不是一次队列搬运
        j.func = &fifo_function;
        j.func_op = FuncOp::kFifoPop;
        j.func_ticket = j.func->Enqueue(now, j.uid);
        j.stage = Stage::kFunctionQueue;
        return;

      case Opcode::kReduce:
      case Opcode::kReduction:
      case Opcode::kConcat:
        // tag 位装的是接收核那边对应的 task id，本核的 id 才是包上的 tag。
        j.send_tid = e.tag;
        j.send_tag = ctx.node_id;
        StartSend(j, now);
        return;

      case Opcode::kMove:
      case Opcode::kBypass:
        if (j.plan.IsLocalMove()) {
          BeginLocalMove(j, now);
          return;
        }
        j.send_tid = j.tid;
        j.send_tag = e.tag;
        StartSend(j, now);
        return;

      default:
        LOGCHECK(false, "DTE: outgoing opcode is not modelled yet.");
        return;
    }
  }

  void StartSend(Job& j, Time now) {
    if (j.send_opcode == Opcode::kDontCare) j.send_opcode = j.opcode;
    if (!j.send_task_set) {
      j.send_task = j.tid;
      j.send_task_set = true;
    }
    if (j.send_uid == 0) j.send_uid = j.uid;
    j.burst_len =
        CalcCycles(static_cast<int64_t>(ctx.Task(j.send_task).time_or_vol),
                   ctx.P().dte_bandwidth);
    j.send_start = now;
    j.next_beat = 0;
    j.stage = Stage::kSend;
  }

  void Emit(Job& j) {
    TaskEntry const& e = ctx.Task(j.send_task);
    const uint64_t bw = ctx.P().dte_bandwidth;
    const uint64_t vol = e.time_or_vol;
    const uint64_t offset = j.next_beat * bw;
    const uint64_t chunk = vol > offset ? std::min(bw, vol - offset) : 0;

    CommInstPtr c = std::make_shared<CommInst>();
    c->uid = j.send_uid;
    c->tid = j.send_tid;
    c->opcode = j.send_opcode;
    c->tag = j.send_tag;
    c->beat_id = j.next_beat;
    c->total_fragments = j.burst_len;
    c->vol = chunk;
    c->xfer_id = xfer_next++;
    AttachHeader(c.get(), j.send_opcode);
    sink->Inject(c, e.dst, chunk);
  }

  // ------------------------------------------------------------ 入方向

  void BeginIncoming(Job& j, Time now) {
    switch (j.opcode) {
      case Opcode::kUserInit:
        InitUser(j, now);
        return;

      case Opcode::kMove:
      case Opcode::kBypass:
      case Opcode::kConcat:
        // 数据写进 CoreMem 这件事本身不占额外时间，收下就算完成。
        j.stage = Stage::kDone;
        return;

      case Opcode::kFifoIn:
        if (!HitMapMatches(*j.payload)) {
          DropInactiveFifo(j, now);
          return;
        }
        InitFifoUser(j, now);
        return;

      case Opcode::kReduction:
        if (!HitMapMatches(*j.payload)) {
          BypassInactiveReduction(j, now);
          return;
        }
        j.func = &reduction_function;
        j.func_op = Registered(j.uid) ? FuncOp::kReduce : FuncOp::kReductionInit;
        j.func_ticket = j.func->Enqueue(now, j.uid);
        j.stage = Stage::kFunctionQueue;
        return;

      case Opcode::kReduce:
        if (ctx.P().dte_execution_mode == DteExecutionMode::kShared) {
          // 合用一条执行通道时不再单独抢功能单元，它已经被通道串起来了。
          j.func_grant = now;
          j.timer_end = now + ctx.P().dte_reduce_time;
          j.stage = Stage::kTimer;
          return;
        }
        j.func = &reduction_function;
        j.func_op = FuncOp::kReduce;
        j.func_ticket = j.func->Enqueue(now, j.uid);
        j.stage = Stage::kFunctionQueue;
        return;

      default:
        LOGCHECK(false, "DTE: incoming opcode is not modelled yet.");
        return;
    }
  }

  // ------------------------------------------------------------ 用户队列

  // 拿到功能单元之后真正开始做那件事。
  void BeginFunction(Job& j, Time now) {
    switch (j.func_op) {
      case FuncOp::kReduce:
        j.timer_end = j.func_grant + ctx.P().dte_reduce_time;
        j.stage = Stage::kTimer;
        return;

      case FuncOp::kFifoPush: {
        // 压栈要走一次访存，队列本身的写入等访存回来再做
        const uint64_t vol = ctx.Task(j.tid).time_or_vol;
        MemArbiter& mem = memory->MatrixMemArbiter();
        LOGCHECK(!mem.FifoFull(), "DTE: the user queue is full.");
        j.mem_ticket = memory->DteMatrixMem().Access(now, j.uid, j.tid, vol);
        j.stage = Stage::kMemWait;
        return;
      }

      case FuncOp::kFifoPop: {
        // 一个 user 会被扇出到好几个下游，队列只弹一次，之后几次复用同一份
        auto it = send_cache.find(j.uid);
        if (it != send_cache.end()) {
          j.queued = it->second.payload;
          if (--it->second.remaining == 0) send_cache.erase(it);
          ReleaseFunction(j, now);
          BeginQueuedSend(j, now);
          return;
        }
        CommInstPtr head = memory->MatrixMemArbiter().FifoPeek();
        LOGCHECK(head != nullptr,
                 "DTE: a send task found the user queue empty.");
        j.mem_ticket = memory->DteMatrixMem().Access(
            now, j.uid, j.tid, ctx.Task(j.tid).time_or_vol);
        j.stage = Stage::kMemWait;
        return;
      }

      case FuncOp::kReductionInit: {
        // 归约核的第一包要先在 MatrixMem 里给这个 user 落个脚，之后同一个 user 的
        // 后续包才认得它
        SetStorage(StorageMode::kReduction);
        j.mem_ticket = memory->DteMatrixMem().Access(
            now, j.uid, j.tid, ctx.Task(j.tid).time_or_vol);
        j.stage = Stage::kMemWait;
        return;
      }

      case FuncOp::kReductionRetire: {
        reduction_active.erase(j.uid);
        reduction_ts_active.erase(j.uid);
        ClearUserContext(j.uid);
        j.activate_after_ack = true;
        StartSend(j, now);
        return;
      }

      case FuncOp::kFifoRetire: {
        CommInstPtr gone = memory->MatrixMemArbiter().FifoRetire(j.uid);
        LOGCHECK(gone != nullptr,
                 "DTE: a user retires but the queue never held it.");
        LOGCHECK(fifo_active > 0, "DTE: the queue's active count underflowed.");
        --fifo_active;
        send_cache.erase(j.uid);
        j.activate_after_ack = true;
        StartSend(j, now);
        return;
      }

      case FuncOp::kNone:
        LOGCHECK(false, "DTE: took a function unit with nothing to do.");
        return;
    }
  }

  bool MemDone(Job const& j) const {
    return memory->DteMatrixMem().Done(j.mem_ticket);
  }

  void FinishMemory(Job& j, Time now) {
    MemInterface& port = memory->DteMatrixMem();
    ctx.Span(Unit::kMemory, j.uid, j.tid, port.GrantCycle(j.mem_ticket),
             port.EndCycle(j.mem_ticket), SpanState::kMemoryService,
             ctx.Task(j.tid).time_or_vol);
    ctx.Wait(Unit::kMemory, j.uid, j.tid, port.EnqueueCycle(j.mem_ticket),
             port.GrantCycle(j.mem_ticket), WaitReason::kMemoryArbiter);
    port.Finish(j.mem_ticket);
    j.mem_ticket = kNoTicket;

    if (j.func_op == FuncOp::kFifoPush) {
      memory->MatrixMemArbiter().FifoPush(j.payload,
                                          ctx.Task(j.tid).time_or_vol);
      ActivateFifoWindow(now);
      ReleaseFunction(j, now);
      j.stage = Stage::kDone;
      return;
    }

    if (j.func_op == FuncOp::kReductionInit) {
      reduction_active.insert(j.uid);
      reduction_pending.push_back(j.payload);
      ActivateReductionWindow();
      // 落脚之后接着做这一包的加法，功能单元一直握着不放
      j.func_grant = now;
      j.timer_end = now + ctx.P().dte_reduce_time;
      j.stage = Stage::kTimer;
      return;
    }

    LOGCHECK(j.func_op == FuncOp::kFifoPop, "DTE: unexpected memory wait.");
    MemArbiter& mem = memory->MatrixMemArbiter();
    j.queued = mem.FifoPeek();
    LOGCHECK(j.queued != nullptr, "DTE: the queue emptied while popping.");
    mem.FifoCommitPop();
    SendCache entry;
    entry.payload = j.queued;
    entry.remaining = CountFanoutSends(j.tid);
    if (entry.remaining > 1) {
      --entry.remaining;
      send_cache[j.uid] = entry;
    }
    ReleaseFunction(j, now);
    BeginQueuedSend(j, now);
  }

  void ReleaseFunction(Job& j, Time now) {
    if (j.func == nullptr) return;
    j.func->Release(j.func_ticket, now);
    j.func = nullptr;
    j.func_ticket = kNoTicket;
  }

  // 弹出来的那个 user 带着它自己的身份走，包上的 tid 也是它自己的那个，不是这条发送
  // 任务的 tid：下一个广播核那边它仍然是第一行的入口包。FIFO_OUT 另外换成 USER_INIT，
  // 因为对普通的下游来说这是一个新 user 的开始，不是一次队列搬运。
  void BeginQueuedSend(Job& j, Time now) {
    LOGCHECK(j.queued != nullptr, "DTE: nothing was popped to send.");
    if (j.opcode == Opcode::kFifoOut) {
      j.send_opcode = Opcode::kUserInit;
      j.send_tid = 0;
    } else {
      j.send_opcode = j.queued->opcode;
      j.send_tid = j.queued->tid;
    }
    j.send_uid = j.queued->uid;
    j.send_tag = j.queued->tag;
    StartSend(j, now);
  }

  // 这个 user 到退休之前还要往几个下游扇出。队列只弹一次，其余几次复用。
  uint64_t CountFanoutSends(uint64_t from_tid) const {
    uint64_t count = 0;
    for (uint64_t t = from_tid; ctx.HasTask(t); ++t) {
      TaskEntry const& e = ctx.Task(t);
      if (e.unit != UnitType::kDte) continue;
      if (e.opcode == Opcode::kRetire) break;
      if (e.opcode == Opcode::kFifoIn || e.opcode == Opcode::kFifoOut) ++count;
    }
    return count > 0 ? count : 1;
  }

  // 同时被 TaskScheduler 认领的 user 数受 stream 数约束：认领一个就占一个位置，
  // 那个 user 退休才腾出来。队列本身可以压得比这多，只是轮不到。
  void ActivateFifoWindow(Time now) {
    MemArbiter& mem = memory->MatrixMemArbiter();
    while (fifo_active < ctx.P().stream_count) {
      CommInstPtr next = mem.FifoActivateNext();
      if (next == nullptr) break;
      ++fifo_active;
      sched->Ack(next->uid, next->tid);
    }
    (void)now;
  }

  void InitFifoUser(Job& j, Time now) {
    SetStorage(StorageMode::kFifo);
    // 认领的动作由激活窗口做，它 ack 的是队列里那个 user，不是这条收包任务
    j.do_ack = false;
    j.func_op = FuncOp::kFifoPush;
    j.after_bitmap = Stage::kFunctionQueue;
    const uint64_t value = j.payload != nullptr ? LocalBitmap(*j.payload) : 0;
    j.bitmap_ticket = moe_bitmap->BeginWrite(now, j.uid, j.tid, value);
    j.stage = Stage::kBitmapWrite;
  }

  void InitUser(Job& j, Time now) {
    if (user_to_slot.count(j.uid) != 0) {
      // 重复的 Init 不再开一次户，也不 ack：这条边上真正的那一次已经 ack 过了。
      j.do_ack = false;
      j.stage = Stage::kDone;
      return;
    }
    SetStorage(StorageMode::kCore);
    AllocSlot(j.uid);
    const uint64_t value = j.payload != nullptr ? LocalBitmap(*j.payload) : 0;
    j.bitmap_ticket = moe_bitmap->BeginWrite(now, j.uid, j.tid, value);
    j.stage = Stage::kBitmapWrite;
  }

  // ------------------------------------------------------------ 归约窗口

  bool Registered(uint64_t uid) const {
    return user_to_slot.count(uid) != 0 || reduction_active.count(uid) != 0;
  }

  void ClearUserContext(uint64_t uid) {
    user_hit_maps.erase(uid);
    user_bitmaps.erase(uid);
    user_phase.erase(uid);
  }

  // 归约核上同时被 TaskScheduler 认领的 user 数也受 stream 数约束：落了脚的排着队，
  // 认领一个占一个位置，那个 user 退休才腾出来。
  void ActivateReductionWindow() {
    while (reduction_ts_active.size() < ctx.P().stream_count &&
           !reduction_pending.empty()) {
      CommInstPtr next = reduction_pending.front();
      reduction_pending.erase(reduction_pending.begin());
      if (next == nullptr) continue;
      if (reduction_active.count(next->uid) == 0) continue;
      if (reduction_ts_active.count(next->uid) != 0) continue;
      reduction_ts_active.insert(next->uid);
      // 建档不走访问口：写进去的是默认值，它是一次登记，不是一次配置
      moe_bitmap->SeedDefault(next->uid, 0);
      sched->AdmitWithPrecompletedTask(next->uid);
    }
  }

  // 本核这次不激活，但这条链路要接着往下走：照本核任务表里那条归约或搬运的目的地把包
  // 转出去，本地什么都不做，也不惊动 TaskScheduler。
  void BypassInactiveReduction(Job& j, Time now) {
    for (Opcode want : {Opcode::kReduction, Opcode::kMove}) {
      for (uint64_t t = 0; ctx.HasTask(t); ++t) {
        TaskEntry const& e = ctx.Task(t);
        if (e.unit != UnitType::kDte || e.opcode != want) continue;
        ++inactive_bypass;
        j.do_ack = false;
        j.send_task = t;
        j.send_task_set = true;
        j.send_opcode = want;
        j.send_tid = want == Opcode::kReduction ? e.tag : t;
        j.send_tag = want == Opcode::kReduction ? ctx.node_id : e.tag;
        StartSend(j, now);
        return;
      }
    }
    LOGCHECK(false,
             "DTE: an inactive reduction packet has nowhere to be forwarded.");
  }

  // 本核这次不激活的广播包：本地不压队列，该转给下游的照转，再把额度还给上游。
  void DropInactiveFifo(Job& j, Time now) {
    std::vector<PendingSend> sends;
    bool has_retire = false;
    PendingSend retire;
    for (uint64_t t = 0; ctx.HasTask(t); ++t) {
      TaskEntry const& e = ctx.Task(t);
      if (e.unit != UnitType::kDte) continue;
      if (e.opcode == Opcode::kFifoIn) {
        PendingSend item;
        item.task_id = t;
        item.opcode = Opcode::kFifoIn;
        item.tid = 0;
        item.tag = j.payload != nullptr ? j.payload->tag : 0;
        sends.push_back(item);
      } else if (e.opcode == Opcode::kRetire && !has_retire) {
        has_retire = true;
        retire.task_id = t;
        retire.opcode = Opcode::kRetire;
        retire.tid = t;
        retire.tag = ctx.node_id;
      }
    }
    LOGCHECK(has_retire,
             "DTE: an inactive broadcast packet has no retire row to return "
             "credit with.");
    sends.push_back(retire);

    ++inactive_drop;
    j.do_ack = false;
    PendingSend first = sends.front();
    sends.erase(sends.begin());
    j.more_sends = std::move(sends);
    j.send_task = first.task_id;
    j.send_task_set = true;
    j.send_opcode = first.opcode;
    j.send_tid = first.tid;
    j.send_tag = first.tag;
    StartSend(j, now);
  }

  // ------------------------------------------------------------ MoE 头

  // 收到的包带着本次激活了哪几个 group、每个 group 命中几个专家。TaskScheduler 派下来
  // 的出方向任务只有 uid 与 tid，所以这两样要在收包时留下来，转发同一个 user 时补回去。
  //
  // Phase 那一段的身份同理：一次工作走到哪一层、由哪条通道进来、属于哪个 chip 组，
  // 阵列里没有一处知道，只能一路随包带着。少了它，算完的结果回到汇合点时认不出自己
  // 该跟哪一份残差配对。
  void LatchHeader(CommInst const& c) {
    if (!c.hit_map.empty()) user_hit_maps[c.uid] = c.hit_map;
    if (!c.moe_bitmap.empty()) user_bitmaps[c.uid] = c.moe_bitmap;
    if (c.phase1_lane_id >= 0 || c.phase1_group_id >= 0) {
      PhaseTag& t = user_phase[c.uid];
      t.layer_id = c.layer_id;
      t.lane_id = c.phase1_lane_id;
      t.group_id = c.phase1_group_id;
    }
  }

  // RES 与 BYPASS 走的是残差那条路，本来就不该带 group 信息，所以它们不补 HitMap。
  void AttachHeader(CommInst* c, Opcode op) const {
    if (op != Opcode::kRes && op != Opcode::kBypass) {
      auto it = user_hit_maps.find(c->uid);
      if (it != user_hit_maps.end()) c->hit_map = it->second;
    }
    auto bit = user_bitmaps.find(c->uid);
    if (bit != user_bitmaps.end()) c->moe_bitmap = bit->second;
    auto pit = user_phase.find(c->uid);
    if (pit != user_phase.end()) {
      c->layer_id = pit->second.layer_id;
      c->phase1_lane_id = pit->second.lane_id;
      c->phase1_group_id = pit->second.group_id;
    }
  }

  // 这个包在本核算不算激活。三个信号一个都没有就是 dense，一律算激活；有了信号还缺
  // group 或缺 HitMap，那是路由元数据坏了，不能当成"不激活"糊过去。
  bool HitMapMatches(CommInst const& c) const {
    if (c.hit_map.empty() && !ctx.HasGroup()) return true;
    LOGCHECK(ctx.HasGroup(),
             "DTE: a packet with a HitMap reached a core without a group.");
    LOGCHECK(!c.hit_map.empty(), "DTE: a MoE packet carries an empty HitMap.");
    return c.HitsGroup(static_cast<uint32_t>(ctx.group_id));
  }

  // 本核这次要按几倍算。包上没带 MoEBitMap 就退回 tag，那是 dense 的走法。
  uint64_t LocalBitmap(CommInst const& c) const {
    if (c.moe_bitmap.empty()) return c.tag;
    LOGCHECK(ctx.HasGroup(),
             "DTE: a packet with a MoEBitMap reached a core without a group.");
    const uint32_t self = static_cast<uint32_t>(ctx.group_id);
    for (auto const& item : c.moe_bitmap) {
      if (item.first == self) return item.second;
    }
    LOGCHECK(!c.HitsGroup(self),
             "DTE: the HitMap says this group is active but the MoEBitMap has "
             "no entry for it.");
    return 0;
  }

  // ------------------------------------------------------------ 槽位与模式

  void SetStorage(StorageMode mode) {
    if (storage == StorageMode::kUnset) {
      storage = mode;
      return;
    }
    LOGCHECK(storage == mode, "DTE: one core cannot mix two storage modes.");
  }

  void AllocSlot(uint64_t uid) {
    LOGCHECK(user_to_slot.count(uid) == 0, "DTE: user already holds a slot.");
    LOGCHECK(!free_slots.empty(), "DTE: all memory slots are taken.");
    user_to_slot[uid] = free_slots.front();
    free_slots.erase(free_slots.begin());
  }

  void FreeSlot(uint64_t uid) {
    auto it = user_to_slot.find(uid);
    LOGCHECK(it != user_to_slot.end(), "DTE: free a slot that was never taken.");
    const uint64_t slot = it->second;
    user_to_slot.erase(it);
    // 从小到大放回去，对应硬件的前导零查找：空位总是从最小的那个开始给。
    auto pos = free_slots.begin();
    while (pos != free_slots.end() && *pos < slot) ++pos;
    free_slots.insert(pos, slot);
  }

  void Sweep() {
    size_t keep = 0;
    for (size_t i = 0; i < jobs.size(); ++i) {
      if (jobs[i].stage == Stage::kFinished) continue;
      if (keep != i) jobs[keep] = std::move(jobs[i]);
      ++keep;
    }
    jobs.resize(keep);
  }

  CoreContext ctx;
  SchedulerPort* sched = nullptr;
  CreditReturnPort* credit = nullptr;
  PacketSink* sink = nullptr;
  MoeBitMap* moe_bitmap = nullptr;
  MemorySystem* memory = nullptr;

  PriorityArbiter admission;
  PriorityArbiter setup;
  ExclusiveArbiter comm_lane;
  ExclusiveArbiter split_lane;
  ExclusiveArbiter* handle_lane = nullptr;
  ExclusiveArbiter fifo_function;
  ExclusiveArbiter reduction_function;
  // 五路径仲裁。关着的时候它一次也不会被请求，两条执行通道照旧各排各的。
  DsaArbiter dsa;

  // 队列里同时被认领的 user 数，以及一个 user 被扇出到多个下游时复用的那一份。
  struct SendCache {
    CommInstPtr payload;
    uint64_t remaining = 0;
  };

  StorageMode storage = StorageMode::kUnset;
  std::unordered_set<uint64_t> reduction_active;
  std::unordered_set<uint64_t> reduction_ts_active;
  std::vector<CommInstPtr> reduction_pending;
  uint64_t fifo_active = 0;
  std::unordered_map<uint64_t, SendCache> send_cache;
  uint64_t total_slots = 0;
  std::vector<uint64_t> free_slots;
  std::unordered_map<uint64_t, uint64_t> user_to_slot;

  std::unordered_map<uint64_t, std::vector<uint32_t>> user_hit_maps;
  std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>>
      user_bitmaps;
  // 一次 Phase 工作的身份。非 Phase 流量上这张表恒为空。
  struct PhaseTag {
    uint64_t layer_id = 0;
    int64_t lane_id = -1;
    int64_t group_id = -1;
  };
  std::unordered_map<uint64_t, PhaseTag> user_phase;
  std::unordered_map<BeatKey, Beats, BeatKeyHash> recv;
  std::vector<Job> jobs;
  uint64_t xfer_next = 0;
  uint64_t inactive_bypass = 0;
  uint64_t inactive_drop = 0;
  bool advancing = false;
};

}
}

#endif
