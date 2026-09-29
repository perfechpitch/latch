#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_LATENCY_
#define _LATCH_BACH_IP_CHIP_CORE_VU_LATENCY_

// 各计算的首拍延迟。默认是暂定值，按编码覆盖；改一处即全局生效，下一条
// 用到该编码的宏指令就走新拍数。Reset() 回到暂定。
//
// 执行级按本单元这条微指令的编码取拍数，并且仍然每拍收一段（完全流水）。
// 除法文档上写非全吞吐，发起间隔还没给，这里先保持每拍收一段，只把延迟
// 放到 20。归约与 Top-16 的系数没给，仍用 kVuExeStages。SEXE 的 fdiv /
// fsqrt 文档写“更长”，拍数没给，先与其它迭代一样。

#include <cstdint>
#include <unordered_map>

#include "bach/ip/chip/core/vu/vu_types.h"

namespace latch {
namespace bach {

class VuLatency {
 public:
  static VuLatency& Get() {
    static VuLatency t;
    return t;
  }

  void SetValu(ValuOp op, uint64_t cycles) { valu_[uint32_t(op)] = cycles; }
  void SetVsfu(VsfuOp op, uint64_t cycles) { vsfu_[uint32_t(op)] = cycles; }
  void SetMexe(MexeOp op, uint64_t cycles) { mexe_[uint32_t(op)] = cycles; }
  void SetSexe(SexeOp op, uint64_t cycles) { sexe_[uint32_t(op)] = cycles; }
  void SetConfig(uint64_t cycles) { config_ = cycles; }
  void SetPipe(uint64_t cycles) { pipe_ = cycles; }

  void Reset() {
    valu_.clear();
    vsfu_.clear();
    mexe_.clear();
    sexe_.clear();
    config_ = 2;
    pipe_ = 3;
  }

  uint64_t Valu(ValuOp op) const { return Pick(valu_, uint32_t(op), DefaultValu(op)); }
  uint64_t Vsfu(VsfuOp op) const { return Pick(vsfu_, uint32_t(op), DefaultVsfu(op)); }
  uint64_t Mexe(MexeOp op) const { return Pick(mexe_, uint32_t(op), DefaultMexe(op)); }
  uint64_t Sexe(SexeOp op) const { return Pick(sexe_, uint32_t(op), DefaultSexe(op)); }
  uint64_t Config() const { return config_; }
  uint64_t Pipe() const { return pipe_; }

  // 两个 VSFU 落在同一个模块上。互相用 src_sel 串起来时首拍相加；并行时取
  // 较长的一路。BF16 拼接只认 VSFU0。
  uint64_t VsfuStage(VuStaticCfg const& c, bool bf16) const {
    bool a = VuVsfuOn(c, 0);
    bool b = !bf16 && VuVsfuOn(c, 1);
    uint64_t x = a ? Vsfu(VsfuOp(c.VsfuOpcode(0))) : 0;
    uint64_t y = b ? Vsfu(VsfuOp(c.VsfuOpcode(1))) : 0;
    if (a && b) {
      bool chain = c.VsfuSrc(0) == kSrcVsfu1 || c.VsfuSrc(1) == kSrcVsfu0;
      return chain ? x + y : (x > y ? x : y);
    }
    return a ? x : y;
  }

  // SEXE0/1/2 是同一物理单元的串行迭代，激活的几次相加。
  uint64_t SexeChain(VuStaticCfg const& c) const {
    uint64_t n = 0;
    for (uint64_t k = 0; k < 3; ++k) {
      if (!c.sexe[k].Active()) continue;
      n += Sexe(SexeOp(c.sexe[k].opcode));
    }
    return n;
  }

 private:
  static uint64_t Pick(std::unordered_map<uint32_t, uint64_t> const& over,
                       uint32_t op, uint64_t fallback) {
    auto it = over.find(op);
    return it == over.end() ? fallback : it->second;
  }

  static uint64_t DefaultValu(ValuOp op) {
    switch (op) {
      case ValuOp::kFaddVv: case ValuOp::kFaddVf:
      case ValuOp::kFsubVv: case ValuOp::kFsubVf: case ValuOp::kFrsubVf:
        return 2;
      case ValuOp::kFmulVv: case ValuOp::kFmulVf:
        return 4;
      case ValuOp::kFdivVv:
        return 20;
      case ValuOp::kFminVv: case ValuOp::kFminVf:
      case ValuOp::kFmaxVv: case ValuOp::kFmaxVf:
      case ValuOp::kMvVf: case ValuOp::kMvSf: case ValuOp::kMvFs:
      case ValuOp::kMvVv: case ValuOp::kSwap2:
      case ValuOp::kSlide1Up: case ValuOp::kSlide1Down:
      case ValuOp::kSgnjVv: case ValuOp::kSgnjVf:
      case ValuOp::kSgnjnVv: case ValuOp::kSgnjnVf:
      case ValuOp::kSgnjxVv: case ValuOp::kSgnjxVf:
      case ValuOp::kEqVv: case ValuOp::kEqVf:
      case ValuOp::kNeVv: case ValuOp::kNeVf:
      case ValuOp::kLtVv: case ValuOp::kLtVf:
      case ValuOp::kLeVv: case ValuOp::kLeVf:
      case ValuOp::kGtVf: case ValuOp::kGeVf:
      case ValuOp::kClassMv: case ValuOp::kMergeVfm: case ValuOp::kMergeVvm:
        return 3;
      case ValuOp::kMaccVv: case ValuOp::kMaccVf:
      case ValuOp::kNmaccVv: case ValuOp::kNmaccVf:
      case ValuOp::kMsacVv: case ValuOp::kMsacVf:
      case ValuOp::kNmsacVv: case ValuOp::kNmsacVf:
        return 5;
      // 归约与 Top-16 的系数未定，先占 4 拍，用 SetValu 改。
      case ValuOp::kRedusum: case ValuOp::kRedmax: case ValuOp::kRedmin:
      case ValuOp::kSortmax16: case ValuOp::kSortmin16:
        return kVuExeStages;
      default:
        return 0;
    }
  }

  static uint64_t DefaultVsfu(VsfuOp op) {
    return op == VsfuOp::kNop ? 0 : 8;
  }

  // 位运算与 vcpop/vfirst 暂定 2。前缀与按索引改位没单列，先同样取 2。
  static uint64_t DefaultMexe(MexeOp op) {
    return op == MexeOp::kNop ? 0 : 2;
  }

  // fdiv / fsqrt 的额外拍数未给，先与一次迭代同为 2。
  static uint64_t DefaultSexe(SexeOp op) {
    return op == SexeOp::kNop ? 0 : 2;
  }

  std::unordered_map<uint32_t, uint64_t> valu_, vsfu_, mexe_, sexe_;
  uint64_t config_ = 2;
  uint64_t pipe_ = 3;
};

}  // namespace bach
}  // namespace latch

#endif
