#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_EXE_BASE_
#define _LATCH_BACH_IP_CHIP_CORE_VU_EXE_BASE_

// M6 · 执行单元这一段的共同骨架。
//
// VALU0 / VALU1 / VALU2 / VSFU / MEXE / SEXE 各是一个独立打拍的模块，本条不动
// 它的单元当拍透传。各单元的级数设计未给，本轮统一取 4 拍并标为待定；SEXE 是
// 同一物理单元的三次串行迭代，所以是 3 倍。
//
// 单元之间的先后：硬件上它们并联，谁在谁前由静态配置里的 bypass 连接决定。建模
// 把它们串成一条固定的链 VALU0 → VALU1 → VALU2 → VSFU → MEXE → SEXE，取源时
// 从链上游的输出直接取。一条只用一个单元的宏指令因此只耗那个单元的级数，用两个
// 且有依赖的耗两段，这正是「合并点的两个源操作数不同拍到达」那件事。链序原文
// 没给，标为待定。
//
// 硬件不提供软件可见的缓冲队列，所以这一段每级只压一条，不排队。

#include <memory>
#include <string>

#include "base/log.h"
#include "bach/common/numeric/accum.h"
#include "bach/ip/chip/core/vu/vu_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 按全局 src_sel 编码取一路源。执行单元的输出直接从 flow 上取（bypass 不消耗
// RF 端口），RF 那几路由 SMUX 预先读好。
//
// 数据流方向固定为 LU → VEXE → MEXE / SEXE → SU 或 RF 写回，没有反向通路：
// MEXE 与 SEXE 的输出不能被任何 VEXE 选用。链序就是照这条排的。
inline VuOperand const& VuSrcOf(VuFlow const& f, uint64_t sel) {
  static const VuOperand none;
  switch (sel) {
    case kSrcNone: return none;
    case kSrcLu: return f.lu;
    case kSrcValu0: return f.valu[0];
    case kSrcValu1: return f.valu[1];
    case kSrcValu2: return f.valu[2];
    case kSrcVsfu: return f.vsfu;
    case kSrcMexe: return f.mexe;
    case kSrcSexe0: return f.sexe[0];
    case kSrcSexe1: return f.sexe[1];
    case kSrcSexe2: return f.sexe[2];
    case kSrcVrfP0: return f.vrf_rd[0];
    case kSrcVrfP1: return f.vrf_rd[1];
    case kSrcMrfP0: return f.mrf_rd[0];
    case kSrcMrfP1: return f.mrf_rd[1];
    default:
      if (SrcIsSrf(sel)) return f.srf_rd[SrfPortOf(sel)];
      return none;
  }
}

// 本单元的运算掩码。mask_op 每个 VEXE 两位：00 不用、01 MRF_rd_p0、10 p1。
// 位序是 VALU0、VALU1、VALU2、VSFU，掩码位为 0 的 element 不更新目的寄存器。
// Mask 不能广播，一条宏指令内最多两处用 MRF。
inline std::vector<bool> const& VuMaskOf(VuFlow const& f, uint64_t vexe) {
  static const std::vector<bool> none;
  uint64_t sel = f.uops.cfg.MaskSelOf(vexe);
  if (sel == kVuMaskP0) return f.mrf_rd[0].mask;
  if (sel == kVuMaskP1) return f.mrf_rd[1].mask;
  return none;
}

// 取一路的第 i 个元素。标量源在整条向量上广播。
inline float VuElem(VuOperand const& o, uint64_t i) {
  if (i < o.vec.size()) return o.vec[i];
  return o.scalar;
}

class VuExeStage : public BachModule {
 public:
  VuExeStage(ClockPtr clock, const std::string& name, uint64_t latency,
             uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        stages(latency),
        in(std::make_shared<VuFlowPort>(clock)),
        out(std::make_shared<VuFlowPort>(clock)),
        done(clock),
        busy_cycles(clock) {}

  VuFlowPort& In() { return *in; }
  void AttachIn(std::shared_ptr<VuFlowPort> p) { in = std::move(p); }
  VuFlowPort& Out() { return *out; }
  void AttachOut(std::shared_ptr<VuFlowPort> p) { out = std::move(p); }

  uint64_t Done() const { return done.Get(); }
  // 本单元处于 Busy 状态的累计拍数，Profile 那一档要它。
  uint64_t BusyCycles() const { return busy_cnt; }
  bool Quiescent() const override { return !busy && !holding; }

 protected:
  // 本条要不要动这个单元。不动就当拍透传。
  virtual bool Active(VuUops const& u) const = 0;
  // 算。改 flow 上属于本单元的那一格。
  virtual void Compute(VuFlow& f) = 0;

  void Step() override {
    Drain();
    Advance();
    Accept();
    if (busy) ++busy_cnt;
    done = done_cnt;
    busy_cycles = busy_cnt;
    TracePerCycle("busy", busy ? 1 : 0);
  }

 private:
  void Drain() {
    if (!holding) return;
    if (!out->Ready()) {
      out->Drive(held, out_seq);
      return;
    }
    holding = false;
    held = VuFlowPtr();
  }

  void Advance() {
    if (!busy || holding) return;
    if (left > 0) {
      --left;
      out->Idle();
      return;
    }
    Compute(*flight);
    busy = false;
    held = flight;
    flight = VuFlowPtr();
    holding = true;
    out_seq = flight_seq;
    ++done_cnt;
    out->Drive(held, out_seq);
  }

  void Accept() {
    in->DriveReady(!busy && !holding);
    if (busy || holding) return;
    if (!in->Valid() || in->Seq() == last_seq) {
      out->Idle();
      return;
    }
    VuFlowPtr f = in->Flow();
    if (!f) {
      out->Idle();
      return;
    }
    last_seq = in->Seq();
    flight_seq = in->Seq();
    if (!Active(f->uops)) {
      // 本条不动这个单元：当拍透传，不占级数。
      held = f;
      holding = true;
      out_seq = flight_seq;
      out->Drive(held, out_seq);
      return;
    }
    flight = f;
    busy = true;
    left = stages > 0 ? stages - 1 : 0;
    out->Idle();
  }

  uint64_t stages;
  std::shared_ptr<VuFlowPort> in, out;
  VuFlowPtr flight, held;
  bool busy = false, holding = false;
  uint64_t left = 0, out_seq = 0, flight_seq = 0, last_seq = 0, done_cnt = 0;
  uint64_t busy_cnt = 0;

  Logic64 done, busy_cycles;
};

}  // namespace bach
}  // namespace latch

#endif
