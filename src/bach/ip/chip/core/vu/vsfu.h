#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_VSFU_
#define _LATCH_BACH_IP_CHIP_CORE_VU_VSFU_

// M6 · VSFU0 / VSFU1。
//
// 12 条超越函数，全部是逐元素单操作数运算，所以 VSFU_op 一段只有一个源操作数。
// 物理上有 VSFU0 与 VSFU1 两个功能完全相同的单元，一个寄存器同时配它们：低
// 16-bit 配 VSFU0、高 16-bit 配 VSFU1。单个 VSFU 一拍吃 32 个 element，所以
// FP32 精度下两者独立工作、`src_sel` 的 0x05 / 0x06 分别是两者的输出；BF16
// 精度下两者拼接成一个逻辑单元、共同处理 64 element，此时 VSFU1 的两个字段被
// 忽略，`0x06` 也不可再作为来源。
//
// 源不能取自身的输出，两个 VSFU 之间可以单向串联但不能互相回环。VSFU 没有标量
// 读端口，也没有掩码字段：需要对部分 element 做超越函数时，由软件让 VSFU 对全部
// VL 个 element 运算，再用 vfmerge.vvm 按掩码挑选。
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

  // FP32 下两个单元各自记 busy；BF16 拼接时同一拍同时计入两者。
  uint64_t Busy0() const { return busy0_; }
  uint64_t Busy1() const { return busy1_; }

 protected:
  bool Active(VuUops const& u) const override {
    if (VuVsfuOn(u.cfg, 0)) return true;
    // BF16 下两个 VSFU 拼接成一个逻辑单元，VSFU1 的字段被忽略。
    return !u.inst.Bf16() && VuVsfuOn(u.cfg, 1);
  }

  void Compute(VuFlow& f) override {
    bool bf16 = f.uops.inst.Bf16();
    f.vsfu[0] = VuOperand();
    f.vsfu[1] = VuOperand();
    bool on0 = VuVsfuOn(f.uops.cfg, 0);
    bool on1 = !bf16 && VuVsfuOn(f.uops.cfg, 1);
    if (on0) Run(0, f);
    if (on1) Run(1, f);
    // 拼接后的逻辑单元只有一个结果，该拍同时计入两个计数器。
    if (bf16 && on0) {
      ++busy0_;
      ++busy1_;
    } else {
      if (on0) ++busy0_;
      if (on1) ++busy1_;
    }
  }

 private:
  void Run(uint64_t which, VuFlow& f) {
    uint32_t opcode = f.uops.cfg.VsfuOpcode(which);
    if (opcode == 0 || !VsfuSupports(opcode)) return;
    VsfuOp op = VsfuOp(opcode);
    VuOperand const& a = VuSrcOf(f, f.uops.cfg.VsfuSrc(which));
    uint64_t vl = f.SegLen();

    VuOperand o;
    o.vec.resize(vl);
    for (uint64_t i = 0; i < vl; ++i) {
      o.vec[i] = numeric::ClampNanInf(One(op, VuElem(a, i)));
    }
    f.vsfu[which] = std::move(o);
  }

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

  uint64_t busy0_ = 0, busy1_ = 0;
};

}  // namespace bach
}  // namespace latch

#endif
