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
// 这一段是流水的：每拍收一个 RF entry，内部走 stages 级，每拍交出一个。MAS 记
// 的「全吞吐为常态，每周期接受 1 个 VRF entry；跨分组串联只增加首拍填充延迟，
// 不降低稳态吞吐」就是这个意思。本条不动这个单元时当拍透传，不占级数。
//
// 硬件不提供软件可见的缓冲队列：在飞的条数就等于级数，压不住就往上游报不收。

#include <cmath>
#include <deque>
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
    case kSrcVsfu0: return f.vsfu[0];
    case kSrcVsfu1: return f.vsfu[1];
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

// 本 VALU 的运算掩码。mask_op 里每个 VALU 一个字节，取值是 src_sel：00 不用、
// 01 LU 的 ld.mask bypass（不占 MRF 读端口）、40 MRF_rd_p0、41 MRF_rd_p1。掩码
// 位为 0 的 element 不更新目的寄存器。Mask 不能广播，一条宏指令内最多两处用
// MRF，那个上限在 M3 查过。
inline std::vector<bool> const& VuMaskOf(VuFlow const& f, uint64_t valu) {
  static const std::vector<bool> none;
  uint64_t sel = f.uops.cfg.MaskSelOf(valu);
  if (sel == kVuMaskSelLu) return f.lu.mask;
  if (sel == kVuMaskSelMrfP0) return f.mrf_rd[0].mask;
  if (sel == kVuMaskSelMrfP1) return f.mrf_rd[1].mask;
  return none;
}

// 两个收口位置——归约指令的输出 fd 与 SU 写出的输入阶段——的 NaN / Inf 处理。
//
// 替换模式（TYPE_VL.NAN_INF_REPLACE_EN=1）：NaN 换 NAN_REPLACE_VALUE、+Inf 换
// INF_REPLACE_VALUE、−Inf 换取负的本值；非替换模式：NaN / Inf 原样透传，NaN 另
// 置位 error_code.NAN_ERROR 并把上报单元记进 error_info。只在这两处收口，其余
// 数值运算的输出不做替换。
inline float VuSettleNanInf(float v, VuUops const& u, uint64_t unit,
                            VuFlow& f) {
  if (!std::isnan(v) && !std::isinf(v)) return v;
  bool nan = std::isnan(v);
  if (u.inst.NanInfReplaceEn()) {
    if (nan) {
      ++f.nan_replaced;
      return VuReplaceValue(u.nan_replace, u.inst.Bf16());
    }
    ++f.inf_replaced;
    float r = VuReplaceValue(u.inf_replace, u.inst.Bf16());
    return std::signbit(v) ? -r : r;
  }
  if (nan && (f.error & kVuErrNan) == 0) {
    f.error |= kVuErrNan;
    f.err_unit = unit;
  }
  return v;
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
  bool Quiescent() const override { return pipe.empty() && !holding; }

 protected:
  // 本条要不要动这个单元。不动就当拍透传。
  virtual bool Active(VuUops const& u) const = 0;
  // 算。改 flow 上属于本单元的那一格。
  virtual void Compute(VuFlow& f) = 0;

  void Step() override {
    // 先把在飞的推进一级，再收本拍新来的，最后看出口。收在推进之后，所以当拍
    // 收下的那一个不会被本拍推掉；透传的那一档 left 是 0，当拍就走。
    Tick();
    Accept();
    Emit();
    if (!pipe.empty() || holding) ++busy_cnt;
    done = done_cnt;
    busy_cycles = busy_cnt;
    TracePerCycle("busy", (!pipe.empty() || holding) ? 1 : 0);
    TracePerCycle("depth", pipe.size());
    TracePerCycle("hold", holding ? 1 : 0);
  }

 private:
  // 在飞的一级：本段的现场、它的序号、还差几拍出得来。
  struct Slot {
    VuFlowPtr f;
    uint64_t seq = 0;
    uint64_t left = 0;
  };

  void Tick() {
    for (Slot& s : pipe) {
      if (s.left > 0) --s.left;
    }
  }

  void Accept() {
    // 上游读的是本级上一拍发布的 ready，所以级数之外要再留一格：压到正好满
    // 才说不收的话，上游那一拍已经不发了，稳态下每两拍就空掉一拍。
    uint64_t depth = (stages > 0 ? stages : 1) + 1;
    bool room = pipe.size() < depth;
    in->DriveReady(room);
    if (!room) return;
    if (!in->Valid() || in->Seq() == last_seq) return;
    VuFlowPtr f = in->Flow();
    if (!f) return;
    last_seq = in->Seq();

    Slot s;
    s.f = f;
    s.seq = in->Seq();
    if (Active(f->uops)) {
      Compute(*f);
      s.left = stages;
    } else {
      // 本条不动这个单元：当拍透传，不占级数。
      s.left = 0;
    }
    pipe.push_back(s);
  }

  void Emit() {
    if (holding) {
      if (!out->Ready()) {
        out->Drive(held, out_seq);
        return;
      }
      holding = false;
      held = VuFlowPtr();
    }
    if (!pipe.empty() && pipe.front().left == 0) {
      held = pipe.front().f;
      out_seq = pipe.front().seq;
      pipe.pop_front();
      holding = true;
      ++done_cnt;
      out->Drive(held, out_seq);
      return;
    }
    out->Idle();
  }

  uint64_t stages;
  std::shared_ptr<VuFlowPort> in, out;
  std::deque<Slot> pipe;
  VuFlowPtr held;
  bool holding = false;
  uint64_t out_seq = 0, last_seq = 0, done_cnt = 0;
  uint64_t busy_cnt = 0;

  Logic64 done, busy_cycles;
};

}  // namespace bach
}  // namespace latch

#endif
