#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_MEXE_
#define _LATCH_BACH_IP_CHIP_CORE_VU_MEXE_

// M6 · MEXE。
//
// 15 条 Mask 指令：八条逻辑运算，vcpop.m 数 1 的个数、vfirst.m 找第一个 1，
// vmsbf / vmsif / vmsof.m 三条按第一个 1 的位置生成新 Mask，vmiuset.mv 与
// vmiset.mv 按 16 个 INT16 索引清位或置位。后两条配合 VALU1 的 Top-K 做迭代
// 查找：取完一轮 Top-16 就把这 16 位清掉，下一轮取的是次大的一批。
//
// MEXE 只接受 Mask 数据且位于 VALU 下游，不能取 LU / VALU1 / VALU2 / VSFU /
// SEXE 的输出，也不能取 VRF / SRF。它的标量输出是整数，所以也不能当 SEXE 的源，
// 因为 SEXE 只有浮点通路。

#include <string>

#include "bach/ip/chip/core/vu/exe_base.h"

namespace latch {
namespace bach {

class VuMexe : public VuExeStage {
 public:
  VuMexe(ClockPtr clock, const std::string& name, uint64_t parent = 0,
         bool tick = true)
      : VuExeStage(clock, name, kVuExeStages, parent, tick) {}

 protected:
  bool Active(VuUops const& u) const override {
    return u.cfg.mexe.Active();
  }

  void Compute(VuFlow& f) override {
    VuOpReg const& r = f.uops.cfg.mexe;
    MexeOp op = MexeOp(r.opcode);
    uint64_t vl = f.SegLen();
    // 源操作数 1 恒为掩码：LU 的 ld.mask bypass（不占 MRF 读端口）、VALU0 的
    // 比较 / vfclass 结果（同样不占），或 MRF_rd_p0 / p1。源操作数 2 随编码而定：
    // 双操作数掩码逻辑时是第二个掩码，vmiuset / vmiset 时是 16 个 INT16 索引。
    std::vector<bool> const& a = VuSrcOf(f, r.src1).mask;
    std::vector<bool> const& b = VuSrcOf(f, r.src2).mask;
    VuOperand& o = f.mexe;
    o = VuOperand();

    switch (op) {
      case MexeOp::kAnd: case MexeOp::kNand: case MexeOp::kAndn:
      case MexeOp::kXor: case MexeOp::kOr: case MexeOp::kNor:
      case MexeOp::kOrn: case MexeOp::kXnor:
        o.mask.resize(vl);
        for (uint64_t i = 0; i < vl; ++i) {
          o.mask[i] = Logic(op, Bit(a, i), Bit(b, i));
        }
        break;
      case MexeOp::kCpop: {
        uint64_t n = 0;
        for (uint64_t i = 0; i < vl; ++i) {
          if (Bit(a, i)) ++n;
        }
        o.scalar = float(n);
        o.has_scalar = true;
        break;
      }
      case MexeOp::kFirst: {
        // 没有 1 时输出 −1。
        float at = -1.0f;
        for (uint64_t i = 0; i < vl; ++i) {
          if (Bit(a, i)) {
            at = float(i);
            break;
          }
        }
        o.scalar = at;
        o.has_scalar = true;
        break;
      }
      case MexeOp::kSbf: case MexeOp::kSif: case MexeOp::kSof: {
        uint64_t first = vl;
        for (uint64_t i = 0; i < vl; ++i) {
          if (Bit(a, i)) {
            first = i;
            break;
          }
        }
        o.mask.assign(vl, false);
        for (uint64_t i = 0; i < vl; ++i) {
          if (op == MexeOp::kSbf) o.mask[i] = i < first;         // 第一个 1 之前
          else if (op == MexeOp::kSif) o.mask[i] = i <= first;   // 含第一个 1
          else o.mask[i] = i == first;                           // 只留第一个 1
        }
        break;
      }
      case MexeOp::kIuset: case MexeOp::kIset: {
        // 掩码走 src1，16 个 INT16 索引走 src2：取 0x03（VALU1 的 Top-K 输出
        // 直接接过来）或 0x30/0x31（读回此前落在 VRF 里的索引）。取完一轮就把
        // 这 16 位清掉或置上，下一轮取的是次大的一批。
        //
        // 索引向量定长 1 个 VRF entry、与 VL 无关；有效候选不足 16 个时多余位置
        // 由 Top-K 填 −1（全 1），这里按无效索引忽略，不改变任何 Mask 位。
        o.mask.assign(vl, false);
        for (uint64_t i = 0; i < vl; ++i) o.mask[i] = Bit(a, i);
        std::vector<uint16_t> const& idx = VuSrcOf(f, r.src2).index;
        for (uint16_t at : idx) {
          if (at != 0xFFFFu && at < o.mask.size()) {
            o.mask[at] = op == MexeOp::kIset;
          }
        }
        break;
      }
      default:
        break;
    }
  }

 private:
  static bool Bit(std::vector<bool> const& m, uint64_t i) {
    return i < m.size() && m[i];
  }

  static bool Logic(MexeOp op, bool x, bool y) {
    switch (op) {
      case MexeOp::kAnd: return x && y;
      case MexeOp::kNand: return !(x && y);
      case MexeOp::kAndn: return x && !y;
      case MexeOp::kXor: return x != y;
      case MexeOp::kOr: return x || y;
      case MexeOp::kNor: return !(x || y);
      case MexeOp::kOrn: return x || !y;
      default: return x == y;
    }
  }
};

}  // namespace bach
}  // namespace latch

#endif
