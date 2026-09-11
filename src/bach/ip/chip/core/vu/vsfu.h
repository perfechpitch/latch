#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_VSFU_
#define _LATCH_BACH_IP_CHIP_CORE_VU_VSFU_

// M6 · VSFU。
//
// 12 条超越函数，全部是逐元素单操作数运算，所以 VSFU_op 只有 SRC1_SEL 一路源。
// 物理上有 VSFU0 与 VSFU1 两个功能一致的单元：FP32 精度下两者独立工作，BF16
// 精度下拼接成一个逻辑单元，那是吞吐上的事，一条宏指令看到的是一个 VSFU，
// 输出也只有一份。
//
// 源不能取自身的输出。VSFU 没有标量读端口，掩码由 mask_op.VSFU_MASK_SEL 选。
//
// 数值这一档标为待定：硬件用查表加插值，拟合方式设计未给（“自定义拟合函数暂定
// 不实现”）。这里用标准库算，参考实现用同一套函数就能逐 bit 对上，与 RTL 对不
// 上是预期之内的，等拟合表出来再换。

#include <cmath>
#include <string>

#include "bach/ip/chip/core/vu/exe_base.h"

namespace latch {
namespace bach {

class VuVsfu : public VuExeStage {
 public:
  VuVsfu(ClockPtr clock, const std::string& name, uint64_t parent = 0,
         bool tick = true)
      : VuExeStage(clock, name, kVuExeStages, parent, tick) {}

 protected:
  bool Active(VuUops const& u) const override {
    return u.cfg.vsfu.Active();
  }

  void Compute(VuFlow& f) override {
    VuOpReg const& r = f.uops.cfg.vsfu;
    VsfuOp op = VsfuOp(r.opcode);
    VuOperand const& a = VuSrcOf(f, r.src1);
    uint64_t vl = f.uops.inst.Vl();
    std::vector<bool> const& m = VuMaskOf(f, uint64_t(VuUnit::kVsfu) - 2);
    VuOperand prev = f.vsfu;

    VuOperand o;
    o.vec.resize(vl);
    for (uint64_t i = 0; i < vl; ++i) {
      if (!m.empty() && i < m.size() && !m[i]) {
        o.vec[i] = i < prev.vec.size() ? prev.vec[i] : 0.0f;
        continue;
      }
      o.vec[i] = numeric::ClampNanInf(One(op, VuElem(a, i)));
    }
    f.vsfu = o;
  }

 private:

  static float One(VsfuOp op, float x) {
    switch (op) {
      case VsfuOp::kSin: return std::sin(x);
      case VsfuOp::kCos: return std::cos(x);
      case VsfuOp::kTanh: return std::tanh(x);
      case VsfuOp::kExp: return std::exp(x);
      case VsfuOp::kExp2: return std::exp2(x);
      case VsfuOp::kLn: return std::log(x);
      case VsfuOp::kLog2: return std::log2(x);
      case VsfuOp::kRcp: return 1.0f / x;
      case VsfuOp::kRsqrt: return 1.0f / std::sqrt(x);
      case VsfuOp::kSqrt: return std::sqrt(x);
      case VsfuOp::kSigmoid: return 1.0f / (1.0f + std::exp(-x));
      // 自定义拟合函数的形态由静态配置指定，设计里暂定不实现。
      default: return x;
    }
  }
};

}  // namespace bach
}  // namespace latch

#endif
