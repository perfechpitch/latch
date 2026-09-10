#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_VALU_
#define _LATCH_BACH_IP_CHIP_CORE_VU_VALU_

// M6 · VALU0 / VALU1 / VALU2。
//
// 三个 VALU 共用一个 8 位编码空间，各自支持其中一部分：加减乘、最值与标量广播
// 三个都有；MACC、除法、符号注入、比较生成 Mask、vfclass 与 vfmerge 只有 VALU0
// 有；归约、Top-16 与 vfmv.f.s 只有 VALU1 有；vmv.v.v 只有 VALU2 有 —— 它是把
// VALU2 当一级延迟对齐缓冲，补偿同一条宏指令内两条并行通路的级数差。
//
// 本 VALU 不支持的编码与未分配的编码一律按无操作处理，与 0x00 等效，不置异常。
//
// .vf 形态的 src1 是标量。三个 VALU 只有 src1 允许取 SRF，所以标量操作数一律挂
// src1；标量结果要送回向量运算得跨宏指令走 SRF。
//
// 归约的位序照 F45：LANES 内先归约，再走 ⌈log2 SEG⌉ 级树。参考实现必须用同一
// 顺序，否则浮点加法不结合，逐 bit 比对过不去。

#include <algorithm>
#include <string>
#include <vector>

#include "bach/ip/chip/core/vu/exe_base.h"

namespace latch {
namespace bach {

class VuValu : public VuExeStage {
 public:
  VuValu(ClockPtr clock, const std::string& name, uint64_t which,
         uint64_t parent = 0, bool tick = true)
      : VuExeStage(clock, name, kVuExeStages, parent, tick), idx(which) {}

 protected:
  bool Active(VuUops const& u) const override {
    ValuOp op = ValuOp(u.cfg.valu[idx].opcode);
    return op != ValuOp::kNop && Supports(op);
  }

  void Compute(VuFlow& f) override {
    VuOpReg const& r = f.uops.cfg.valu[idx];
    ValuOp op = ValuOp(r.opcode);
    VuMacroInst const& inst = f.uops.inst;
    uint64_t vl = inst.Vl();
    VuOperand const& s1 = VuSrcOf(f, r.src1);
    VuOperand const& s2 = VuSrcOf(f, r.src2);
    VuOperand const& s3 = VuSrcOf(f, r.src3);
    // 运算掩码：掩码位为 0 的 element 不更新目的寄存器。
    std::vector<bool> const& m = VuMaskOf(f, idx);
    VuOperand& o = f.valu[idx];
    VuOperand prev = o;
    o = VuOperand();

    switch (op) {
      case ValuOp::kFaddVv: case ValuOp::kFaddVf:
      case ValuOp::kFsubVv: case ValuOp::kFsubVf:
      case ValuOp::kFrsubVf:
      case ValuOp::kFmulVv: case ValuOp::kFmulVf:
      case ValuOp::kFdivVv:
      case ValuOp::kFminVv: case ValuOp::kFminVf:
      case ValuOp::kFmaxVv: case ValuOp::kFmaxVf:
      case ValuOp::kSgnjVv: case ValuOp::kSgnjVf:
      case ValuOp::kSgnjnVv: case ValuOp::kSgnjnVf:
      case ValuOp::kSgnjxVv: case ValuOp::kSgnjxVf:
        o.vec.resize(vl);
        for (uint64_t i = 0; i < vl; ++i) {
          o.vec[i] = Binary(op, VuElem(s2, i), VuElem(s1, i));
        }
        break;

      // MACC 系列：三操作数，累加器 vd 由 src3 提供。
      case ValuOp::kMaccVv: case ValuOp::kMaccVf:
      case ValuOp::kNmaccVv: case ValuOp::kNmaccVf:
      case ValuOp::kMsacVv: case ValuOp::kMsacVf:
      case ValuOp::kNmsacVv: case ValuOp::kNmsacVf:
        o.vec.resize(vl);
        for (uint64_t i = 0; i < vl; ++i) {
          o.vec[i] = Macc(op, VuElem(s1, i), VuElem(s2, i), VuElem(s3, i));
        }
        break;

      case ValuOp::kEqVv: case ValuOp::kEqVf:
      case ValuOp::kNeVv: case ValuOp::kNeVf:
      case ValuOp::kLtVv: case ValuOp::kLtVf:
      case ValuOp::kLeVv: case ValuOp::kLeVf:
      case ValuOp::kGtVf: case ValuOp::kGeVf:
        o.mask.resize(vl);
        for (uint64_t i = 0; i < vl; ++i) {
          o.mask[i] = Compare(op, VuElem(s2, i), VuElem(s1, i));
        }
        break;

      case ValuOp::kClassMv: {
        // vfclass.mv：按 10 位 CLASS_IMM 判断 src1 每个 element 的 FP 类型，
        // 符合任一条件则 Mask 置位。CLASS_IMM 复用 SRC2_SEL 与 SRC3_SEL 低位。
        uint64_t imm = (r.raw >> 16) & 0x3FFu;
        o.mask.resize(vl);
        for (uint64_t i = 0; i < vl; ++i) {
          o.mask[i] = ((1u << Classify(VuElem(s1, i))) & imm) != 0;
        }
        break;
      }

      case ValuOp::kMergeVfm: case ValuOp::kMergeVvm: {
        // 按 Mask 逐元素选：置位取 src1（.vfm 时是标量），否则取 src2。
        o.vec.resize(vl);
        for (uint64_t i = 0; i < vl; ++i) {
          bool take = i < m.size() && m[i];
          o.vec[i] = take ? VuElem(s1, i) : VuElem(s2, i);
        }
        // vfmerge 的掩码本身是选择依据，不再当运算掩码用。
        f.valu[idx] = o;
        return;
      }

      case ValuOp::kMvVf:
        // 标量广播：所有 element 取标量 src1。
        o.vec.assign(vl, VuElem(s1, 0));
        break;

      case ValuOp::kMvSf: {
        // 标量搬入向量第 ELEM_IMM 个元素，其余保持原值。ELEM_IMM 复用
        // SRC2_SEL 低 6 位。
        uint64_t at = r.src2 & 0x3Fu;
        o.vec = prev.vec;
        o.vec.resize(vl, 0.0f);
        if (at < vl) o.vec[at] = VuElem(s1, 0);
        break;
      }

      case ValuOp::kMvFs: {
        // 向量第 ELEM_IMM 个元素搬出为标量，经 SRF 虚拟写口 p1 写回。
        // ELEM_IMM 复用 SRC1_SEL 低 6 位。
        uint64_t at = r.src1 & 0x3Fu;
        o.scalar = VuElem(s2, at);
        o.has_scalar = true;
        break;
      }

      case ValuOp::kMvVv:
        // 向量原样直通：不做数值运算、不改格式、不产生舍入。
        o.vec = s2.vec;
        o.vec.resize(vl, 0.0f);
        break;

      case ValuOp::kRedusum:
        // 无序求和归约，标量初值在 src1。
        o.scalar = numeric::ClampNanInf(
            VuElem(s1, 0) + numeric::ReduceTree(Head(s2, vl), kVuLanes));
        o.has_scalar = true;
        break;

      case ValuOp::kRedmax: case ValuOp::kRedmin: {
        std::vector<float> v = Head(s2, vl);
        float r0 = VuElem(s1, 0);
        for (float x : v) {
          r0 = op == ValuOp::kRedmax ? (x > r0 ? x : r0) : (x < r0 ? x : r0);
        }
        o.scalar = r0;
        o.has_scalar = true;
        break;
      }

      case ValuOp::kSortmax16: case ValuOp::kSortmin16: {
        // Top-16 排序，同时输出 16 个 INT16 索引。并列时取下标小的那个，与
        // 参考实现同序。
        std::vector<float> v = Head(s2, vl);
        std::vector<uint64_t> order(v.size());
        for (uint64_t i = 0; i < order.size(); ++i) order[i] = i;
        bool desc = op == ValuOp::kSortmax16;
        std::stable_sort(order.begin(), order.end(),
                         [&](uint64_t x, uint64_t y) {
                           return desc ? v[x] > v[y] : v[x] < v[y];
                         });
        uint64_t k = order.size() < kVuTopK ? order.size() : kVuTopK;
        o.vec.resize(k);
        o.index.resize(k);
        for (uint64_t i = 0; i < k; ++i) {
          o.vec[i] = v[order[i]];
          o.index[i] = uint16_t(order[i]);
        }
        // 归约与 Top-K 的标量结果走 SRF 虚拟写口 p1。
        o.scalar = k > 0 ? o.vec[0] : 0.0f;
        o.has_scalar = true;
        break;
      }

      default:
        break;
    }

    // 运算掩码：位为 0 的 element 不更新目的寄存器，保持原值。
    if (!m.empty() && !o.vec.empty()) {
      for (uint64_t i = 0; i < o.vec.size(); ++i) {
        if (i < m.size() && !m[i]) {
          o.vec[i] = i < prev.vec.size() ? prev.vec[i] : 0.0f;
        }
      }
    }
  }

 private:
  // 每个实例支持哪些。不支持的按无操作处理。
  bool Supports(ValuOp op) const {
    switch (op) {
      // 三个 VALU 都有的：加减乘、最值、标量广播。
      case ValuOp::kFaddVv: case ValuOp::kFaddVf:
      case ValuOp::kFsubVv: case ValuOp::kFsubVf: case ValuOp::kFrsubVf:
      case ValuOp::kFmulVv: case ValuOp::kFmulVf:
      case ValuOp::kFminVv: case ValuOp::kFminVf:
      case ValuOp::kFmaxVv: case ValuOp::kFmaxVf:
      case ValuOp::kMvVf:
        return true;
      // 只有 VALU0 有的。
      case ValuOp::kFdivVv:
      case ValuOp::kMvSf:
      case ValuOp::kMaccVv: case ValuOp::kMaccVf:
      case ValuOp::kNmaccVv: case ValuOp::kNmaccVf:
      case ValuOp::kMsacVv: case ValuOp::kMsacVf:
      case ValuOp::kNmsacVv: case ValuOp::kNmsacVf:
      case ValuOp::kSgnjVv: case ValuOp::kSgnjVf:
      case ValuOp::kSgnjnVv: case ValuOp::kSgnjnVf:
      case ValuOp::kSgnjxVv: case ValuOp::kSgnjxVf:
      case ValuOp::kEqVv: case ValuOp::kEqVf:
      case ValuOp::kNeVv: case ValuOp::kNeVf:
      case ValuOp::kLtVv: case ValuOp::kLtVf:
      case ValuOp::kLeVv: case ValuOp::kLeVf:
      case ValuOp::kGtVf: case ValuOp::kGeVf:
      case ValuOp::kClassMv: case ValuOp::kMergeVfm: case ValuOp::kMergeVvm:
        return idx == 0;
      // 只有 VALU1 有的。
      case ValuOp::kMvFs:
      case ValuOp::kRedusum: case ValuOp::kRedmax: case ValuOp::kRedmin:
      case ValuOp::kSortmax16: case ValuOp::kSortmin16:
        return idx == 1;
      // 只有 VALU2 有的。
      case ValuOp::kMvVv:
        return idx == 2;
      default:
        return false;
    }
  }

  static std::vector<float> Head(VuOperand const& o, uint64_t vl) {
    std::vector<float> v = o.vec;
    if (v.size() > vl) v.resize(vl);
    return v;
  }

  // vd = f(src2, src1)。.vv 与 .vf 只差 src1 是向量还是标量，取元素那一步已经
  // 把这件事抹平了。
  static float Binary(ValuOp op, float x, float y) {
    float r = 0.0f;
    switch (op) {
      case ValuOp::kFaddVv: case ValuOp::kFaddVf: r = x + y; break;
      case ValuOp::kFsubVv: case ValuOp::kFsubVf: r = x - y; break;
      case ValuOp::kFrsubVf: r = y - x; break;   // 标量 − 向量，反减
      case ValuOp::kFmulVv: case ValuOp::kFmulVf: r = x * y; break;
      case ValuOp::kFdivVv: r = x / y; break;
      case ValuOp::kFminVv: case ValuOp::kFminVf: r = x < y ? x : y; break;
      case ValuOp::kFmaxVv: case ValuOp::kFmaxVf: r = x > y ? x : y; break;
      // 符号注入：量值取 src2，符号取 src1（n 取反、x 异或）。
      case ValuOp::kSgnjVv: case ValuOp::kSgnjVf:
        r = numeric::FloatOf((numeric::BitsOf(x) & 0x7FFFFFFFu) |
                             (numeric::BitsOf(y) & 0x80000000u));
        break;
      case ValuOp::kSgnjnVv: case ValuOp::kSgnjnVf:
        r = numeric::FloatOf((numeric::BitsOf(x) & 0x7FFFFFFFu) |
                             (~numeric::BitsOf(y) & 0x80000000u));
        break;
      default:
        r = numeric::FloatOf(numeric::BitsOf(x) ^
                             (numeric::BitsOf(y) & 0x80000000u));
        break;
    }
    return numeric::ClampNanInf(r);
  }

  // 乘积与累加器都先各自算出来再合，不写成一句 —— 写成一句编译器会合成 FMA，
  // 中间那一次舍入就没了，与硬件的位对不上。
  static float Macc(ValuOp op, float a, float b, float d) {
    float prod = a * b;
    float r = 0.0f;
    switch (op) {
      case ValuOp::kMaccVv: case ValuOp::kMaccVf: r = prod + d; break;
      case ValuOp::kNmaccVv: case ValuOp::kNmaccVf: r = -prod - d; break;
      case ValuOp::kMsacVv: case ValuOp::kMsacVf: r = prod - d; break;
      default: r = -prod + d; break;
    }
    return numeric::ClampNanInf(r);
  }

  static bool Compare(ValuOp op, float x, float y) {
    switch (op) {
      case ValuOp::kEqVv: case ValuOp::kEqVf: return x == y;
      case ValuOp::kNeVv: case ValuOp::kNeVf: return x != y;
      case ValuOp::kLtVv: case ValuOp::kLtVf: return x < y;
      case ValuOp::kLeVv: case ValuOp::kLeVf: return x <= y;
      case ValuOp::kGtVf: return x > y;
      default: return x >= y;
    }
  }

  // RISC-V fclass 的十类，位号就是分类号。
  static uint32_t Classify(float v) {
    uint32_t b = numeric::BitsOf(v);
    uint32_t sign = b >> 31;
    uint32_t exp = (b >> 23) & 0xFFu;
    uint32_t frac = b & 0x7FFFFFu;
    if (exp == 0xFF) {
      if (frac == 0) return sign ? 0 : 7;                // −Inf / +Inf
      return (frac >> 22) & 1u ? 9 : 8;                  // quiet / signaling NaN
    }
    if (exp == 0) {
      if (frac == 0) return sign ? 3 : 4;                // −0 / +0
      return sign ? 2 : 5;                               // 次正规
    }
    return sign ? 1 : 6;                                 // 正规
  }

  uint64_t idx;
};

}  // namespace bach
}  // namespace latch

#endif
