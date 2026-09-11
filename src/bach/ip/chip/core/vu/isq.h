#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_ISQ_
#define _LATCH_BACH_IP_CHIP_CORE_VU_ISQ_

// M2 · ISQ 压入出队。
//
// 深度 8 的宏指令队列。压入时 macro_inst_left 加一，退休时减一。软件判全部
// 宏指令是否完成看的就是它，或者看 status.BUSY。
//
// 出队的闸门是在飞条数：最多两条相邻宏指令重叠，所以在飞数到 2 就不再放。
// 重叠的那一条能不能真的往下走还要过 Scoreboard 那一关，这里只管上限。

#include <deque>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/vu/config_register.h"
#include "bach/ip/chip/core/vu/vu_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class VuIsq : public BachModule {
 public:
  VuIsq(ClockPtr clock, const std::string& name, VuConfigRegister& cr,
        uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        cfg_reg(cr),
        in(std::make_shared<VuInstPort>(clock)),
        out(std::make_shared<VuInstPort>(clock)),
        depth(clock),
        left(clock) {}

  VuInstPort& In() { return *in; }
  void AttachIn(std::shared_ptr<VuInstPort> p) { in = std::move(p); }
  VuInstPort& Out() { return *out; }
  void AttachOut(std::shared_ptr<VuInstPort> p) { out = std::move(p); }

  // M9 退休时调这一个。
  void Retire() {
    if (inflight > 0) --inflight;
    if (macro_left > 0) --macro_left;
    ++retire_cnt;
  }
  uint64_t Retired() const { return retire_cnt; }

  bool Full() const { return q.size() >= kVuIsqDepth; }
  bool Empty() const { return q.empty(); }
  uint64_t MacroInstLeft() const { return macro_left; }
  uint64_t Inflight() const { return inflight; }
  // 排队的加在飞的，就是软件读到的 macro_inst_left。
  uint64_t Left() const { return q.size() + inflight; }

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
    ++macro_left;
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
  std::shared_ptr<VuMacroInst> held;
  bool holding = false;
  uint64_t out_seq = 0, last_seq = 0;
  uint64_t macro_left = 0, inflight = 0, retire_cnt = 0;

  Logic64 depth, left;
};

}  // namespace bach
}  // namespace latch

#endif
