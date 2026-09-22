#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_VALU_
#define _LATCH_BACH_IP_CHIP_CORE_VU_VALU_

// M6 · VALU0 / VALU1 / VALU2。
//
// 三个 VALU 共用一个 8 位编码空间，各自支持其中一部分：加减乘、最值与标量广播
// 三个都有；MACC、除法、符号注入、比较生成 Mask、vfclass 与 vfmerge 只有 VALU0
// 有；归约、Top-16 与 vfmv.f.s 只有 VALU1 有；vmv.v.v、vswap2.v 与两条 slide
// 只有 VALU2 有。vmv.v.v 是把 VALU2 当一级延迟对齐缓冲，补偿同一条宏指令内两条
// 并行通路的级数差。
//
// 本 VALU 不支持的编码与未分配的编码一律按无操作处理，与 0x00 等效，不置异常。
// 编码与源选择字段的绑定照《VU-DSA 寄存器整理》：源操作数的编号与助记符里写的
// 位置无关，`s1`→`SRC1_SEL`、`s2`→`SRC2_SEL`、`s3`→`SRC3_SEL`；三个 VALU 只有
// src1 允许取 SRF，所以标量操作数一律挂 src1、向量挂 src2/src3。
//
// 归约的位序照 F45：LANES 内先归约，再走 ⌈log2 SEG⌉ 级树。参考实现必须用同一
// 顺序，否则浮点加法不结合，逐 bit 比对过不去。
//
// 归约的输出 fd 是 NaN / Inf 替换与上报的两个收口位置之一。

#include <algorithm>
#include <cmath>
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
    return ValuSupports(idx, ValuOp(u.cfg.valu[idx].opcode));
  }

  void Compute(VuFlow& f) override {
    VuOpReg const& r = f.uops.cfg.valu[idx];
    ValuOp op = ValuOp(r.opcode);
    if (!ValuSupports(idx, op)) return;
    VuMacroInst const& inst = f.uops.inst;
    uint64_t vl = f.SegLen();
    VuOperand const& s1 = VuSrcOf(f, r.src1);
    VuOperand const& s2 = VuSrcOf(f, r.src2);
    VuOperand const& s3 = VuSrcOf(f, r.src3);
    // 运算掩码：掩码位为 0 的 element 不参与运算。不支持掩码的指令忽略这个字段。
    std::vector<bool> const& m = VuMaskOf(f, idx);
    bool masked = !ValuNoMask(op);
    VuOperand& o = f.valu[idx];
    VuOperand prev = o;
    o = VuOperand();
    // 掩码位为 0 时目的取本指令的透传源：双操作数逐元素类取 vs2、乘累加类取
    // 累加器 vs3。写回 VRF 的数据与 bypass 给下游单元的是同一份。
    VuOperand const* pass = &s2;
    bool pass_used = false;

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
        pass_used = true;
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
        pass = &s3;
        pass_used = true;
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
        // 符合任一条件则 Mask 置位。CLASS_IMM 复用 [25:16]：SRC2_SEL 的 8 位
        // 与 SRC3_SEL 的低 2 位。
        uint64_t imm = (r.raw >> 16) & 0x3FFu;
        o.mask.resize(vl);
        for (uint64_t i = 0; i < vl; ++i) {
          o.mask[i] = ((1u << Classify(VuElem(s1, i))) & imm) != 0;
        }
        break;
      }

      case ValuOp::kMergeVfm: case ValuOp::kMergeVvm: {
        // 按 Mask 逐元素选：置位取 src1（.vfm 时是标量），否则取 src2。掩码本事
        // 是数据选择器而非谓词；VALU0_MASK_SEL 为 0x00 时按全 0 处理。
        o.vec.resize(vl);
        for (uint64_t i = 0; i < vl; ++i) {
          bool take = i < m.size() && m[i];
          o.vec[i] = take ? VuElem(s1, i) : VuElem(s2, i);
        }
        f.valu[idx] = o;
        return;
      }

      case ValuOp::kMvVf:
        // 标量广播：所有 element 取标量 src1。
        o.vec.assign(vl, VuElem(s1, 0));
        break;

      case ValuOp::kMvSf: {
        // 标量搬入向量第 ELEM_IMM 个元素，其余保持原值。ELEM_IMM 复用
        // SRC2_SEL 的低 6 bit。
        uint64_t at = r.src2 & 0x3Fu;
        o.vec = prev.vec;
        o.vec.resize(vl, 0.0f);
        if (at < vl) o.vec[at] = VuElem(s1, 0);
        break;
      }

      case ValuOp::kMvFs: {
        // 向量第 ELEM_IMM 个元素搬出为标量，经 SRF 虚拟写口 p1 写回。向量挂
        // src1、ELEM_IMM 复用 SRC2_SEL 的低 6 bit（只访问第一个 VRF entry 内的
        // element）。
        uint64_t at = r.src2 & 0x3Fu;
        o.scalar = VuElem(s1, at);
        o.has_scalar = true;
        break;
      }

      case ValuOp::kMvVv:
        // 向量原样直通：不做数值运算、不改格式、不产生舍入。
        o.vec = s1.vec;
        o.vec.resize(vl, 0.0f);
        break;

      case ValuOp::kSwap2:
        // 相邻偶奇对交换：vd[2k] = src1[2k+1]、vd[2k+1] = src1[2k]。
        o.vec.resize(vl);
        for (uint64_t i = 0; i + 1 < vl; i += 2) {
          o.vec[i] = VuElem(s1, i + 1);
          o.vec[i + 1] = VuElem(s1, i);
        }
        if (vl % 2 != 0) o.vec[vl - 1] = VuElem(s1, vl - 1);
        break;

      case ValuOp::kSlide1Up:
        // element 向高索引移动一位，低端补标量：vd[0] = 标量 src1、
        // vd[i] = src2[i−1]（1 ≤ i ≤ VL−1）。
        o.vec.resize(vl);
        for (uint64_t i = 0; i < vl; ++i) {
          o.vec[i] = i == 0 ? VuElem(s1, 0) : VuElem(s2, i - 1);
        }
        break;

      case ValuOp::kSlide1Down:
        // element 向低索引移动一位，高端补标量：vd[i] = src2[i+1]
        // （0 ≤ i ≤ VL−2）、vd[VL−1] = 标量 src1。
        o.vec.resize(vl);
        for (uint64_t i = 0; i < vl; ++i) {
          o.vec[i] = i + 1 < vl ? VuElem(s2, i + 1) : VuElem(s1, 0);
        }
        break;

      case ValuOp::kRedusum: case ValuOp::kRedmax: case ValuOp::kRedmin: {
        // 掩码决定参与归约的 element 集合，全 0 时集合为空、结果为初值 fs1。
        std::vector<float> v = Pick(Head(s2, vl), m, masked);
        float init = VuElem(s1, 0);
        if (op == ValuOp::kRedusum) {
          o.scalar = init + numeric::ReduceTree(v, kVuLanes);
        } else {
          float best = init;
          for (float x : v) {
            best = op == ValuOp::kRedmax ? (x > best ? x : best)
                                         : (x < best ? x : best);
          }
          o.scalar = best;
        }
        // 归约输出 fd 是 NaN / Inf 替换与上报的收口位置之一。
        o.scalar = VuSettleNanInf(o.scalar, f.uops, ErrUnitOf(idx), f);
        o.has_scalar = true;
        break;
      }

      case ValuOp::kSortmax16: case ValuOp::kSortmin16: {
        // Top-16 排序，同时输出 16 个 INT16 索引。并列时取下标小的那个，与
        // 参考实现同序。掩码决定候选集，输出布局与定长规则不受掩码影响。
        std::vector<float> src = Head(s1, vl);
        std::vector<uint64_t> order;
        bool desc = op == ValuOp::kSortmax16;
        for (uint64_t i = 0; i < src.size(); ++i) {
          if (masked && !m.empty() && (i >= m.size() || !m[i])) continue;
          order.push_back(i);
        }
        std::stable_sort(order.begin(), order.end(),
                         [&](uint64_t x, uint64_t y) {
                           return desc ? src[x] > src[y] : src[x] < src[y];
                         });
        // 有效候选不足 16 个时，多余位置值填 0、索引填 −1（全 1）。
        o.vec.assign(kVuTopK, 0.0f);
        o.index.assign(kVuTopK, uint16_t(0xFFFFu));
        for (uint64_t i = 0; i < kVuTopK && i < order.size(); ++i) {
          o.vec[i] = src[order[i]];
          o.index[i] = uint16_t(order[i]);
        }
        // 归约与 Top-K 的标量结果走 SRF 虚拟写口 p1。
        o.scalar = o.vec[0];
        o.has_scalar = true;
        break;
      }

      default:
        break;
    }

    // 掩码位为 0 的 element 不参与运算，目的在该位置取透传源。
    if (masked && pass_used && !m.empty() && !o.vec.empty()) {
      for (uint64_t i = 0; i < o.vec.size() && i < vl; ++i) {
        if (i < m.size() && !m[i]) o.vec[i] = VuElem(*pass, i);
      }
    }
    // 目的为 Mask 的指令（比较类与 vfclass.mv）在掩码位为 0 处写 0，等效于
    // 判定结果与 vm 相与。
    if (masked && !m.empty() && !o.mask.empty()) {
      for (uint64_t i = 0; i < o.mask.size(); ++i) {
        if (i < m.size() && !m[i]) o.mask[i] = false;
      }
    }
  }

 private:
  static uint64_t ErrUnitOf(uint64_t which) {
    return which == 0 ? kVuErrUnitValu0
                      : (which == 1 ? kVuErrUnitValu1 : kVuErrUnitValu2);
  }

  // 掩码选中的那些 element；没有掩码时就是全部。
  static std::vector<float> Pick(std::vector<float> const& v,
                                 std::vector<bool> const& m, bool masked) {
    if (!masked || m.empty()) return v;
    std::vector<float> out;
    for (uint64_t i = 0; i < v.size(); ++i) {
      if (i < m.size() && m[i]) out.push_back(v[i]);
    }
    return out;
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

  // 乘积与累加器都先各自算出来再合，不写成一句，因为写成一句编译器会合成 FMA，
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
