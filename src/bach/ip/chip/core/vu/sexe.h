#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_SEXE_
#define _LATCH_BACH_IP_CHIP_CORE_VU_SEXE_

// M6 · SEXE。
//
// 7 条标量浮点指令。物理上只有一组，SEXE0 / SEXE1 / SEXE2 是同一物理单元在一条
// 宏指令内的 3 次串行迭代 —— 所以本模块的级数是别的单元的 3 倍，三次迭代在这
// 一段里依次算完。
//
// 迭代之间天然链式依赖：SEXE0 可取 VALU1 的输出或 SRF_rd_p4/p5，SEXE1 可取
// SEXE0 的输出或 SRF_rd_p6，SEXE2 可取 SEXE1 的输出或 SRF_rd_p7。SEXE1 与
// SEXE2 各只有 1 个 SRF 读端口，所以它们两个操作数中最多 1 个取自 SRF，且至少
// 1 个须取自前一次迭代。不支持立即数，也不能取 MEXE 为源 —— MEXE 的标量输出
// 是整数，SEXE 只有浮点通路。
//
// 标量只有 FP32 一种精度，不随 DATA_TYPE 变。

#include <cmath>
#include <string>

#include "bach/ip/chip/core/vu/exe_base.h"

namespace latch {
namespace bach {

class VuSexe : public VuExeStage {
 public:
  VuSexe(ClockPtr clock, const std::string& name, uint64_t parent = 0,
         bool tick = true)
      : VuExeStage(clock, name, kVuExeStages * 3, parent, tick) {}

 protected:
  bool Active(VuUops const& u) const override {
    return u.cfg.sexe[0].Active() || u.cfg.sexe[1].Active() ||
           u.cfg.sexe[2].Active();
  }

  void Compute(VuFlow& f) override {
    // 三次迭代依次算，第 k 次算完 f.sexe[k] 才有值，第 k+1 次按 src_sel 取的
    // SEXE(k) 就取得到 —— 这就是「至少一个操作数须取自前一次迭代」那条约束
    // 在建模上的落点。
    for (uint64_t k = 0; k < 3; ++k) {
      VuOpReg const& r = f.uops.cfg.sexe[k];
      f.sexe[k] = VuOperand();
      SexeOp op = SexeOp(r.opcode);
      if (op == SexeOp::kNop) continue;
      float x = Scalar(f, r.src1);
      float y = Scalar(f, r.src2);
      f.sexe[k].scalar = numeric::ClampNanInf(One(op, x, y));
      f.sexe[k].has_scalar = true;
    }
  }

 private:
  // 取一路标量源。SEXE 只有浮点通路，源只有四处：SRF 读端口、VALU1 的归约
  // 输出、LU 的 ld.s.fp32 结果、前一次迭代的结果。
  static float Scalar(VuFlow const& f, uint64_t sel) {
    VuOperand const& o = VuSrcOf(f, sel);
    if (o.has_scalar) return o.scalar;
    if (!o.vec.empty()) return o.vec[0];
    return o.scalar;
  }

  static float One(SexeOp op, float x, float y) {
    switch (op) {
      case SexeOp::kFadd: return x + y;
      case SexeOp::kFsub: return x - y;
      case SexeOp::kFmul: return x * y;
      case SexeOp::kFdiv: return x / y;
      case SexeOp::kFsqrt: return std::sqrt(x);
      case SexeOp::kFrsqrt: return 1.0f / std::sqrt(x);
      case SexeOp::kFrcp: return 1.0f / x;
      default: return x;
    }
  }
};

}  // namespace bach
}  // namespace latch

#endif
