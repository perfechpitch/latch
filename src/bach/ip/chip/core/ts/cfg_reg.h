#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_CFG_REG_
#define _LATCH_BACH_IP_CHIP_CORE_TS_CFG_REG_

// CFG_REG：TS 的全部配置寄存器与由它们派生出来的东西。
//
// 配置的下发顺序：先写全局项，再逐项写 task_chain[i]，再逐项写 path_task_map[j]，
// 再写 DATAIN_TASK，最后写 TS_INIT_FINISH。最后一步之前硬件不做任何检查。
//
// 看到 TS_INIT_FINISH 后查合规性并把结论写进 TS_STATE，同时派生四张 64 位掩码
// 供 Task_ctrl 一拍算出 SKIP_MASK。四张都是 task_chain 的纯函数，配完不再变，
// 运行时的跳过判断只查掩码、done_bitmap 与两个用户级标记，不必逐项回读 task_chain。

#include <array>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/ts/ts_types.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

enum class CoreType : uint32_t {
  kNormal = 0,
  kBroadcast = 1,
  kReduction = 2,
};

// TS_STATE 的位。本轮只留状态位，不实现异常上报的行为。
enum TsStateBit : uint64_t {
  kStateChainCfg = 1u << 0,
  kStateDatainCfg = 1u << 1,
  kStateChainError = 1u << 2,
};

// path_task_map 的一项：Router 的请求只带 path_id，本表是把它翻译成任务链上
// 第几步的唯一途径。
struct PathTaskMap {
  uint64_t task_id = 0;
  bool valid = false;
};

class CfgReg : public BachModule {
 public:
  CfgReg(ClockPtr clock, const std::string& name, uint64_t parent = 0,
         bool tick = true)
      : BachModule(clock, name, parent, tick), state(clock) {
    chain.fill(TaskEntry{});
    path_map.fill(PathTaskMap{});
  }

  // ── 配置面。boot 期由 ctrl_noc 逐项写，这里给的是等价的直接接口 ──
  void WriteTask(uint64_t idx, TaskEntry e) {
    LOGCHECK(idx < kTaskChainNum, "CfgReg: task_chain 下标越界。");
    // 每写一项硬件自动把该项的 TASK_VALID 置起来。
    e.valid = true;
    chain[idx] = e;
  }
  void WritePathMap(uint64_t path_id, uint64_t task_id) {
    LOGCHECK(path_id < kTaskChainNum, "CfgReg: path_id 越界。");
    path_map[path_id] = {task_id, true};
  }
  void WriteDatainTask(uint64_t task_pc, bool weights_mode) {
    datain_pc = task_pc;
    datain_weights_mode = weights_mode;
    datain_valid = true;
  }
  void SetStreamNum(uint64_t n) {
    LOGCHECK(n >= 1 && n <= kStreamNum, "CfgReg: stream_num 只能是 1 到 16。");
    stream_num = n;
  }
  void SetCoreType(CoreType t) { core_type = t; }
  void SetBCoreDirection(uint64_t dir) { b_core_dir = dir; }
  void SetTriggerChainEn(bool on) { trigger_chain_en = on; }
  void SetFlowCtl(uint64_t path_id, bool en, uint64_t window_n) {
    LOGCHECK(path_id < kTaskChainNum, "CfgReg: path_id 越界。");
    flowctl_en[path_id] = en;
    flowctl_window[path_id] = window_n;
  }

  // 配置的最后一步：查合规性、派生掩码。
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
  PathTaskMap const& PathMap(uint64_t path_id) const {
    LOGCHECK(path_id < kTaskChainNum, "CfgReg: path_id 越界。");
    return path_map[path_id];
  }
  uint64_t DataInMask() const { return data_in_mask; }
  uint64_t ReissueMask() const { return reissue_mask; }
  uint64_t EndMask() const { return end_mask; }
  uint64_t ExeMask() const { return exe_mask; }

  uint64_t StreamNum() const { return stream_num; }
  CoreType Type() const { return core_type; }
  uint64_t BCoreDirection() const { return b_core_dir; }
  bool TriggerChainEn() const { return trigger_chain_en; }
  bool InitFinished() const { return init_finish; }
  bool WeightsMode() const { return datain_weights_mode; }
  bool DatainValid() const { return datain_valid; }
  uint64_t DatainPc() const { return datain_pc; }
  bool FlowCtlEn(uint64_t path_id) const { return flowctl_en.at(path_id); }
  uint64_t FlowCtlWindow(uint64_t path_id) const {
    return flowctl_window.at(path_id);
  }
  uint64_t TsState() const { return ts_state; }
  // 自启动的 core：复位后直接建满表项，不等 Router trigger。
  bool SelfStartCore() const {
    return core_type == CoreType::kBroadcast || core_type == CoreType::kReduction;
  }

 protected:
  void Step() override {
    state = ts_state;
    TracePerCycle("ts_state", ts_state);
  }

 private:
  void DeriveMasks() {
    data_in_mask = reissue_mask = end_mask = exe_mask = 0;
    for (uint64_t i = 0; i < kTaskChainNum; ++i) {
      if (!chain[i].valid) continue;
      if (chain[i].IsDataIn()) data_in_mask |= 1ull << i;
      if (chain[i].IsReissue()) reissue_mask |= 1ull << i;
      if (chain[i].end) end_mask |= 1ull << i;
      // EXE_MASK 直接把每项的 TASK_EXE_MASK 收拢成一张 64 位掩码。全 1 时这一项
      // 不起作用，非 DP+P2P 的 core 就配成全 1。
      if (chain[i].exe_mask) exe_mask |= 1ull << i;
    }
  }

  // 写 TS_INIT_FINISH 时查六项，结论写进 TS_STATE。
  void Check() {
    ts_state = 0;
    uint64_t valid_cnt = 0, end_cnt = 0, self_cnt = 0;
    bool hole = false, seen_invalid = false;
    for (uint64_t i = 0; i < kTaskChainNum; ++i) {
      if (!chain[i].valid) {
        seen_invalid = true;
        continue;
      }
      // valid 的项中间不许有空洞
      if (seen_invalid) hole = true;
      ++valid_cnt;
      if (chain[i].end) ++end_cnt;
      if (chain[i].self_start) ++self_cnt;
    }
    if (valid_cnt != 0) ts_state |= kStateChainCfg;
    if (datain_valid) ts_state |= kStateDatainCfg;

    bool bad = false;
    if (valid_cnt == 0) bad = true;
    if (end_cnt > 1) bad = true;      // 一条链上不许有多个 TASK_END
    if (hole) bad = true;
    if (self_cnt > 1) bad = true;     // 不许有多个 SELF_START
    // 普通 core 不该配 SELF_START
    if (core_type == CoreType::kNormal && self_cnt > 0) bad = true;
    // 每个带 path_id 的项，其 path_id 在 path_task_map 里必须有 valid 项且指回
    // 该项自己。
    for (uint64_t i = 0; i < kTaskChainNum; ++i) {
      if (!chain[i].valid || chain[i].path_id == 0) continue;
      PathTaskMap const& m = path_map[chain[i].path_id];
      if (!m.valid || m.task_id != i) bad = true;
    }
    if (bad) ts_state |= kStateChainError;
  }

  std::array<TaskEntry, kTaskChainNum> chain;
  std::array<PathTaskMap, kTaskChainNum> path_map;
  std::array<bool, kTaskChainNum> flowctl_en{};
  std::array<uint64_t, kTaskChainNum> flowctl_window{};

  uint64_t datain_pc = 0;
  bool datain_weights_mode = false, datain_valid = false;
  uint64_t stream_num = kStreamNum;
  CoreType core_type = CoreType::kNormal;
  uint64_t b_core_dir = 0;
  bool trigger_chain_en = true;
  bool init_finish = false;
  uint64_t ts_state = 0;

  uint64_t data_in_mask = 0, reissue_mask = 0, end_mask = 0, exe_mask = 0;

  Logic64 state;
};

}  // namespace bach
}  // namespace latch

#endif
