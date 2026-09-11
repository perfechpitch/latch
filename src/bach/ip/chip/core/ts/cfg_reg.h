#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_CFG_REG_
#define _LATCH_BACH_IP_CHIP_CORE_TS_CFG_REG_

// CFG_REG：TS 的全部配置寄存器与由它们派生出来的东西。
//
// 寄存器照《Task Scheduler MAS》的地址映射：TASK_CHAIN_0～63 各占 PC 与 ATTR 两个
// 32 位字（0x000～0x1FC），DATAIN_TASK_PC / ATTR（0x200、0x204），STREAM_NUM
// （0x208），TS_INIT_FINISH（0x20C），TS_STATE（0x210），SELF_START（0x214），
// ROUTER_TABLE_0～63（0x400～0x4FC）。B_CORE_DIRECTION、trigger_task_chain_en 与按
// path_id 索引的流量控制取自《TS_通信机制》，MAS 的地址映射里没有。
//
// 配置顺序：权重加载阶段只配 DATAIN_TASK，WEIGHTS_MODE 置 1；业务流配置先写
// STREAM_NUM 与 SELF_START，再逐项写任务链，每项先 PC 后 ATTR，最后写
// TS_INIT_FINISH。写 TS_INIT_FINISH 后查整张配置表，查出错误写进 TS_STATE。
//
// 查表的同时派生两张 64 位掩码：DATA_IN_MASK 与 THROUGH_END_MASK。后者是第 0 项到
// 唯一 End 项，Task_ctrl 找后继、Retirement 判退休都只看这个范围。

#include <array>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/ts/ts_types.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// TS_STATE 的位：只有 TASK_CHAIN_ERROR 一位。
enum TsStateBit : uint64_t {
  kStateChainError = 1u << 0,
};

// ROUTER_TABLE 的项数，按 PID 索引。
constexpr uint64_t kTsRouteNum = 64;

// ROUTER_TABLE 的一项：这条 path 往哪几个方向走、用哪个 VC。方向只保存与读回；
// VCID 随 DTE 任务一起下发。TASK_DIR 的位：bit0 上下，bit1 左，bit2 右，bit3 本地。
struct TsRoute {
  uint64_t dir = 0;
  uint64_t vcid = 0;
};

class CfgReg : public BachModule {
 public:
  CfgReg(ClockPtr clock, const std::string& name, uint64_t parent = 0,
         bool tick = true)
      : BachModule(clock, name, parent, tick), state(clock) {
    chain.fill(TaskEntry{});
    route.fill(TsRoute{});
  }

  // ── 配置面。boot 期由 ctrl_noc 逐项写，这里给的是等价的直接接口 ──
  // 一项任务的 PC 与 ATTR 一起写；写 ATTR 时硬件把 TASK_VALID 置起来。
  void WriteTask(uint64_t idx, TaskEntry e) {
    LOGCHECK(idx < kTaskChainNum, "CfgReg: task_chain 下标越界。");
    e.valid = true;
    chain[idx] = e;
  }
  void WriteDatainTask(uint64_t task_pc, bool weights_mode) {
    datain_pc = task_pc;
    weights_mode_on = weights_mode;
    datain_valid = true;
  }
  // 超过 16 的值只能写入 16。
  void SetStreamNum(uint64_t n) {
    LOGCHECK(n >= 1, "CfgReg: stream_num 至少是 1。");
    stream_num = n > kStreamNum ? kStreamNum : n;
  }
  // SELF_START：1 表示自启动模式，Task 0 不等 Router trigger 就直接启动。B core
  // 与 R core 是这一档。
  void SetSelfStart(bool on) { self_start = on; }
  void WriteRouterTable(uint64_t pid, uint64_t dir, uint64_t vcid) {
    LOGCHECK(pid < kTsRouteNum, "CfgReg: ROUTER_TABLE 下标越界。");
    route[pid] = {dir, vcid};
  }
  void SetBCoreDirection(uint64_t dir) { b_core_dir = dir; }
  void SetTriggerChainEn(bool on) { trigger_chain_en = on; }
  void SetFlowCtl(uint64_t path_id, bool en, uint64_t window_n) {
    LOGCHECK(path_id < kTaskChainNum, "CfgReg: path_id 越界。");
    flowctl_en[path_id] = en;
    flowctl_window[path_id] = window_n;
  }

  // 配置的最后一步：查整张表、派生掩码。
  void SetInitFinish() {
    init_finish = true;
    DeriveMasks();
    Check();
  }

  // ── 数据面读口 ──
  TaskEntry const& Task(uint64_t idx) const {
    LOGCHECK(idx < kTaskChainNum, "CfgReg: task_chain 下标越界。");
    return chain[idx];
  }
  TsRoute const& Route(uint64_t pid) const {
    LOGCHECK(pid < kTsRouteNum, "CfgReg: ROUTER_TABLE 下标越界。");
    return route[pid];
  }
  // Router 送来的 PID 对的是哪一个搬入任务：在 wait_wake 的项里找 path_id 相同、
  // 这个用户还没做完的最低一项。同一个 PID 可以对多个任务。找不到返回
  // kTaskChainNum。
  uint64_t MatchDatain(uint64_t pid, uint64_t done) const {
    for (uint64_t i = 0; i < kTaskChainNum; ++i) {
      TaskEntry const& t = chain[i];
      if (!t.valid || !t.IsDataIn() || t.path_id != pid) continue;
      if ((done >> i) & 1u) continue;
      return i;
    }
    return kTaskChainNum;
  }
  uint64_t DataInMask() const { return data_in_mask; }
  uint64_t ThroughEndMask() const { return through_end_mask; }

  uint64_t StreamNum() const { return stream_num; }
  bool SelfStartCore() const { return self_start; }
  uint64_t BCoreDirection() const { return b_core_dir; }
  bool TriggerChainEn() const { return trigger_chain_en; }
  bool InitFinished() const { return init_finish; }
  bool WeightsMode() const { return weights_mode_on; }
  bool DatainValid() const { return datain_valid; }
  uint64_t DatainPc() const { return datain_pc; }
  bool FlowCtlEn(uint64_t path_id) const { return flowctl_en.at(path_id); }
  uint64_t FlowCtlWindow(uint64_t path_id) const {
    return flowctl_window.at(path_id);
  }
  uint64_t TsState() const { return ts_state; }

 protected:
  void Step() override {
    state = ts_state;
    TracePerCycle("ts_state", ts_state);
  }

 private:
  void DeriveMasks() {
    data_in_mask = through_end_mask = 0;
    for (uint64_t i = 0; i < kTaskChainNum; ++i) {
      if (!chain[i].valid) break;
      if (chain[i].IsDataIn()) data_in_mask |= 1ull << i;
      through_end_mask |= 1ull << i;
      if (chain[i].end) break;
    }
  }

  // 写 TS_INIT_FINISH 时查整张配置表。权重加载时不查。
  void Check() {
    ts_state = 0;
    if (weights_mode_on) return;
    bool bad = false;
    uint64_t valid_cnt = 0, end_cnt = 0;
    bool hole = false, seen_invalid = false, after_end = false;
    for (uint64_t i = 0; i < kTaskChainNum; ++i) {
      TaskEntry const& t = chain[i];
      if (!t.valid) {
        seen_invalid = true;
        continue;
      }
      if (seen_invalid) hole = true;       // 任务不连续设置 valid
      if (end_cnt > 0) after_end = true;   // End 后面还有 valid
      ++valid_cnt;
      if (t.end) ++end_cnt;
      // 单 task：重发的搬出与 reduce 必须标 credit_en；P2P 重发的搬入必须标
      // wait_wake。
      if ((t.task_type == TaskType::kReissueOut ||
           t.task_type == TaskType::kReduce) && !t.credit_en) {
        bad = true;
      }
      if (t.task_type == TaskType::kP2pReissueIn && !t.wait_wake) bad = true;
      // 重发搬入配对的那一项必须是重发的搬出任务。
      if (t.task_type == TaskType::kP2pReissueIn ||
          t.task_type == TaskType::kBcastReissueIn) {
        TaskEntry const& out = chain[t.p2p_reissue_tid];
        if (!out.valid || out.task_type != TaskType::kReissueOut) bad = true;
      }
    }
    if (hole || after_end || end_cnt > 1) bad = true;
    if (valid_cnt > 0 && end_cnt == 0) bad = true;
    // 自启动模式下 task_chain 与 datain_task 都要配。
    if (self_start && (valid_cnt == 0 || !datain_valid)) bad = true;
    if (bad) ts_state |= kStateChainError;
  }

  std::array<TaskEntry, kTaskChainNum> chain;
  std::array<TsRoute, kTsRouteNum> route;
  std::array<bool, kTaskChainNum> flowctl_en{};
  std::array<uint64_t, kTaskChainNum> flowctl_window{};

  uint64_t datain_pc = 0;
  bool weights_mode_on = false, datain_valid = false;
  uint64_t stream_num = kStreamNum;
  bool self_start = false;
  uint64_t b_core_dir = 0;
  bool trigger_chain_en = true;
  bool init_finish = false;
  uint64_t ts_state = 0;

  uint64_t data_in_mask = 0, through_end_mask = 0;

  Logic64 state;
};

}  // namespace bach
}  // namespace latch

#endif
