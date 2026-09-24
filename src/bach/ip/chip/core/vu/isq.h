#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_ISQ_
#define _LATCH_BACH_IP_CHIP_CORE_VU_ISQ_

// M2 · ISQ 压入出队。
//
// 深度 8 的宏指令队列。压入时 macro_inst_left 加一，退休时减一。软件判全部
// 宏指令是否完成看的就是它，或者看 status.BUSY。
//
// 出队的闸门是在飞条数：最多两条相邻宏指令重叠，所以在飞数到 2 就不再放。
// 重叠的那一条能不能真的往下走还要过 Scoreboard 那一关，这里只管上限。
//
// 队列表项自带一份动态参数快照（压入的就是那一条宏指令本身），快照窗口因此就
// 摆在 ISQ 上：在场宏指令按年龄编号，0 是最老的一条，派发状态随编号一起给出。
// sticky 那一份不在这个窗口里，它在 config_register 上随 error_info 锁存。

#include <deque>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/vu/config_register.h"
#include "bach/ip/chip/core/vu/vu_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class VuIsq : public BachModule, public VuSnapshotWindow {
 public:
  VuIsq(ClockPtr clock, const std::string& name, VuConfigRegister& cr,
        uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        cfg_reg(cr),
        in(std::make_shared<VuInstPort>(clock)),
        out(std::make_shared<VuInstPort>(clock)),
        depth(clock),
        left(clock) {
    cfg_reg.AttachSnapshotWindow(this);
  }

  VuInstPort& In() { return *in; }
  void AttachIn(std::shared_ptr<VuInstPort> p) { in = std::move(p); }
  VuInstPort& Out() { return *out; }
  void AttachOut(std::shared_ptr<VuInstPort> p) { out = std::move(p); }

  // M9 退休时调这一个。
  void Retire() {
    if (inflight > 0) --inflight;
    if (macro_left > 0) --macro_left;
    ++retire_cnt;
    if (!window.empty()) {
      cfg_reg.ClearNotDispatched(window.front().inst->seq);
      window.pop_front();
    }
  }
  uint64_t Retired() const { return retire_cnt; }
  // 被 ISQ 收下的宏指令笔数。收下这一拍就是它过门槛、真正开始算的那一拍 ——
  // 写 trigger 只是把它放进 held，还要等静态配置释放、上一条被取走。
  uint64_t Accepted() const { return accept_cnt; }
  // 开始的 task 笔数，Core 层发波形要用（这一层自己的信号在 chip 级被
  // TraceOffScope 关掉了）。完成只在置了 EVENT_EN 的那一条退休时报，一个 task
  // 只在最后一条置位，所以起点也按 task 数：收下一条时前面没有未收尾的 task
  // （还没收过，或上一条置了 EVENT_EN），这一条就是一个 task 的开头。
  uint64_t Started() const { return task_start_cnt; }
  // 最近开始的那个 task 的身份。三项都是写 trigger 那一拍从身份直连线上采的。
  uint64_t StartStream() const { return start_stream; }
  uint64_t StartTask() const { return start_task; }
  uint64_t StartUser() const { return start_user; }

  bool Full() const { return q.size() >= kVuIsqDepth; }
  bool Empty() const { return q.empty(); }
  uint64_t MacroInstLeft() const { return macro_left; }
  uint64_t Inflight() const { return inflight; }
  // 排队的加在飞的，就是软件读到的 macro_inst_left。
  uint64_t Left() const { return q.size() + inflight; }

  // ── 快照窗口：在场宏指令按年龄编号，0 是最老的一条 ──
  uint64_t SnapCount() const override { return window.size(); }
  bool SnapDispatched(uint64_t age) const override {
    if (age >= window.size() || !window[age].dispatched) return false;
    // 被调度阶段拦下的那几条没进执行单元。
    return !cfg_reg.NotDispatched(window[age].inst->seq);
  }
  uint64_t SnapTag(uint64_t age) const override {
    return age < window.size() ? window[age].inst->tag : 0;
  }
  uint64_t SnapParam(uint64_t age, uint64_t idx) const override {
    return age < window.size() ? window[age].inst->SnapParam(idx) : 0;
  }

  bool Quiescent() const override { return q.empty() && inflight == 0; }

 protected:
  void Step() override {
    // 末级先做：先看下游收没收走，再往队列里压。
    Drain();
    Accept();
    Publish();

    depth = q.size();
    left = macro_left;
    TracePerCycle("depth", q.size());
    TracePerCycle("inflight", inflight);
  }

 private:
  // 一条在场宏指令：本体加它派发没有。编号就是它在 window 里的位置。
  struct Window {
    std::shared_ptr<VuMacroInst> inst;
    bool dispatched = false;
  };

  void Drain() {
    if (holding) {
      if (!out->Ready()) {
        out->Drive(held, out_seq);
        return;
      }
      holding = false;
      ++inflight;
    }
    // 在飞数到上限就不再放：最多两条相邻宏指令重叠。
    if (q.empty() || inflight >= kVuOverlap) {
      out->Idle();
      return;
    }
    held = q.front();
    q.pop_front();
    holding = true;
    out_seq = held->seq;
    out->Drive(held, out_seq);
    MarkDispatched(held->seq);
  }

  // 出队是按发射顺序走的，所以最老的那一条没派发的就是刚出去的这一条。
  void MarkDispatched(uint64_t seq) {
    for (Window& w : window) {
      if (!w.dispatched && w.inst->seq == seq) {
        w.dispatched = true;
        return;
      }
    }
  }

  void Accept() {
    in->DriveReady(!Full());
    if (!in->Valid() || Full()) return;
    // 同一条会连着两拍出现在端口上，按序号认它。
    if (in->Seq() == last_seq) return;
    last_seq = in->Seq();
    auto inst = in->Inst();
    if (!inst) return;
    q.push_back(inst);
    window.push_back({inst, false});
    ++macro_left;
    ++accept_cnt;
    if (!task_open) {
      ++task_start_cnt;
      start_stream = inst->stream_id;
      start_task = inst->task_id;
      start_user = inst->user_id;
    }
    task_open = !inst->event_en;
    // 压进来就算引用了这一组静态配置，配置写从这一刻起被阻塞。
    cfg_reg.HoldCfg(inst->cfg_idx);
  }

  void Publish() {
    uint64_t status = 0;
    if (macro_left > 0) status |= kVuStatusBusy;
    if (Full()) status |= kVuStatusIsqFull;
    if (q.empty()) status |= kVuStatusIsqEmpty;
    if (cfg_reg.ErrorCode() != 0) status |= kVuStatusErrorFlag;
    cfg_reg.SetStatus(status);
    cfg_reg.SetMacroInstLeft(macro_left);
  }

  VuConfigRegister& cfg_reg;
  std::shared_ptr<VuInstPort> in, out;

  std::deque<std::shared_ptr<VuMacroInst>> q;
  std::deque<Window> window;
  std::shared_ptr<VuMacroInst> held;
  bool holding = false;
  uint64_t out_seq = 0, last_seq = 0;
  uint64_t macro_left = 0, inflight = 0, retire_cnt = 0;
  // 收下的宏指令笔数、开始的 task 笔数与最近开始那个 task 的身份，供 Core 层
  // 发波形。task_open：上一条收下的没有置 EVENT_EN，它所在的 task 还没收尾。
  uint64_t accept_cnt = 0, task_start_cnt = 0;
  uint64_t start_stream = 0, start_task = 0, start_user = 0;
  bool task_open = false;

  Logic64 depth, left;
};

}  // namespace bach
}  // namespace latch

#endif
