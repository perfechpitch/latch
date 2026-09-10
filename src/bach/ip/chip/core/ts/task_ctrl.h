#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_TASK_CTRL_
#define _LATCH_BACH_IP_CHIP_CORE_TS_TASK_CTRL_

// Task_ctrl：当前任务做完之后生成后继。
//
// 每个 stream 独立推进，不需要全局 Task Pointer。从 head_ptr 开始环形扫描，只选
// valid=1 且 task_fsm=FINISH 且 end=0 的 stream。
//
// 跳过用一次 64 bit 优先编码一拍算完，连续 skip 的数量不增加周期：
//
//   GROUP_SKIP = stream.compute ? 0 : ~EXE_MASK
//   SKIP_MASK  = ~END_MASK & ( (DATA_IN_MASK & done_bitmap)
//                            | (stream.reissue ? 0 : REISSUE_MASK)
//                            | GROUP_SKIP )
//
// 四种可跳过的场景：异步 datain 已提前完成；B reissue 不需要重发；P2P 不需要
// 重发；DP+P2P 下有些用户只有 P2P 无计算 task。
//
// End task 即使已提前完成也不能被跳过，并且不会再生成后继 —— 所以 SKIP_MASK
// 最外面那个 ~END_MASK 是硬的。
//
// 新任务的 task_id、task_fsm、end 与全部下发属性必须一起原子写入才算生成成功，
// 所以走的是整项写那个口。

#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/ts/cfg_reg.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class TaskCtrl : public BachModule {
 public:
  TaskCtrl(ClockPtr clock, const std::string& name, CfgReg& reg,
           uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        cfg(reg),
        snap(std::make_shared<SnapshotPort>(clock)),
        install(std::make_shared<StreamWritePortIf>(clock)),
        installed(clock),
        skipped(clock) {}

  void AttachSnapshot(std::shared_ptr<SnapshotPort> p) { snap = std::move(p); }
  std::shared_ptr<StreamWritePortIf> InstallPtr() const { return install; }

  uint64_t Installed() const { return installed.Get(); }
  uint64_t Skipped() const { return skipped.Get(); }

  bool Quiescent() const override { return !pending; }

 protected:
  void Step() override {
    if (pending) {
      if (install->Accepted()) {
        pending = false;
        ++install_pending;
      } else {
        // 整项写失败后要重读最新表内容再来，所以这里不保持旧请求，下一拍重算。
        pending = false;
      }
    }
    if (!pending) {
      install->Idle();
      Generate();
    }
    installed = install_pending;
    skipped = skip_pending;
    TracePerCycle("installed", install_pending);
  }

 private:
  void Generate() {
    StreamSnapshotPtr s = snap->Get();
    if (!s) return;
    for (uint64_t k = 0; k < kStreamNum; ++k) {
      uint64_t i = s->AgeOrder(k);
      StreamEntry const& e = s->entry[i];
      if (!e.valid || e.task_fsm != TaskFsm::kFinish || e.end) continue;

      uint64_t skip = SkipMaskOf(e);
      uint64_t next = NextTask(e.task_id, skip, e.done_bitmap);
      if (next >= kTaskChainNum) continue;  // 后面没有可做的了

      TaskEntry const& t = cfg.Task(next);
      auto w = std::make_shared<StreamWrite>();
      w->valid = true;
      w->stream_id = i;
      w->whole = true;
      w->entry = e;
      w->entry.task_id = next;
      w->entry.task_fsm = InitFsm(t);
      ApplyAttr(w->entry, t);
      install->Drive(w);
      pending = true;
      return;  // 一拍只装一个
    }
  }

  uint64_t SkipMaskOf(StreamEntry const& e) const {
    // compute = 1 的用户一个都不跳；compute = 0 的跳过所有 TASK_EXE_MASK = 0
    // 的 task。EXE_MASK 全 1 时这一项恒为 0。
    uint64_t group_skip = e.compute ? 0 : ~cfg.ExeMask();
    // 这个用户不需要重发时，所有 reissue 任务都跳过。
    uint64_t reissue_skip = e.reissue ? 0 : cfg.ReissueMask();
    uint64_t skip = (cfg.DataInMask() & e.done_bitmap) | reissue_skip |
                    group_skip;
    // End task 不能被跳过。
    return skip & ~cfg.EndMask();
  }

  // 从当前 task 的下一项开始找第一个不跳的 valid 项。一拍算完，连续 skip 不
  // 增加周期。
  uint64_t NextTask(uint64_t cur, uint64_t skip, uint64_t done) {
    for (uint64_t i = cur + 1; i < kTaskChainNum; ++i) {
      if (!cfg.Task(i).valid) return kTaskChainNum;
      if ((skip >> i) & 1u) {
        ++skip_pending;
        continue;
      }
      return i;
    }
    return kTaskChainNum;
  }

  // 生成后的初始状态按类型分：普通 Generated 置 READY，DataIn 置 WAIT 等 ack，
  // Reissue 置 WAIT 等 credit。
  static TaskFsm InitFsm(TaskEntry const& t) {
    if (t.IsDataIn() || t.IsReissue()) return TaskFsm::kWait;
    if (t.credit_en) return TaskFsm::kWait;
    return TaskFsm::kReady;
  }

  static void ApplyAttr(StreamEntry& e, TaskEntry const& t) {
    e.task_unit = t.send_unit;
    e.task_recv = t.recv_unit;
    e.task_dsa_en = t.dsa_en;
    e.task_pc = t.task_pc;
    e.task_path_id = t.path_id;
    e.is_reissue = t.IsReissue();
    e.end = t.end;
    e.reduce_num = t.reduce ? t.reduce_num : 0;
    e.reduce_issued = 0;
    e.dte_ack_map = 0;
    e.router_done_map = 0;
  }

  CfgReg& cfg;
  std::shared_ptr<SnapshotPort> snap;
  std::shared_ptr<StreamWritePortIf> install;

  bool pending = false;
  uint64_t install_pending = 0, skip_pending = 0;

  Logic64 installed, skipped;
};

}  // namespace bach
}  // namespace latch

#endif
