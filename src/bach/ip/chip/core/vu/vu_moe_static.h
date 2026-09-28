#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_VU_MOE_STATIC_
#define _LATCH_BACH_IP_CHIP_CORE_VU_VU_MOE_STATIC_

// 与 kernel_vu.c 的 gate_setup / add_setup、bachir VUSTATIC 同一份数。
// 不走 bundle 的用例（dot 单核链、R core）开钟前调用；LoadBundle 从产物写。

#include "bach/ip/chip/core/vu/vu.h"
#include "bach/ip/chip/core/vu/vu_types.h"

namespace latch {
namespace bach {

inline uint64_t VuMoeOpWord(uint64_t opcode, uint64_t src1 = 0,
                            uint64_t src2 = 0) {
  return opcode | (src1 << 8) | (src2 << 16);
}

inline void PreloadVuGroup(Vu& vu, uint64_t group, uint64_t off,
                           uint64_t data) {
  vu.Preload(kVuStaticBase + group * kVuStaticStride + off, data);
}

// 组 1/2：VL = 256 FP32，x 在 entry 0，
// sigmoid 在 entry 8。MXFP8 scale 向下取整。
inline void PreloadVuGate(Vu& vu) {
  uint64_t const type_vl = 256;
  uint64_t const vrf_wt = (8u << 16) | 0u;
  uint64_t const vrf_rd = 8u;
  PreloadVuGroup(vu, 1, kVuLuOp, VuMoeOpWord(uint64_t(LuOp::kLdBf16)));
  PreloadVuGroup(vu, 1, kVuVsfuOp,
                 VuMoeOpWord(uint64_t(VsfuOp::kSigmoid), kSrcLu));
  PreloadVuGroup(vu, 1, kVuPrfOp, kSrcLu | (kSrcVsfu0 << 8));
  PreloadVuGroup(vu, 1, kVuStaticDupOffset + kVuVrfWtIndex, vrf_wt);
  PreloadVuGroup(vu, 1, kVuStaticDupOffset + kVuVrfRdIndex, vrf_rd);
  PreloadVuGroup(vu, 1, kVuStaticDupOffset + kVuTypeVl, type_vl);

  PreloadVuGroup(vu, 2, kVuLuOp, VuMoeOpWord(uint64_t(LuOp::kLdBf16)));
  PreloadVuGroup(vu, 2, kVuValu0Op,
                 VuMoeOpWord(uint64_t(ValuOp::kFmulVv), kSrcVrfP0, kSrcVrfP1));
  PreloadVuGroup(vu, 2, kVuValu1Op,
                 VuMoeOpWord(uint64_t(ValuOp::kFmulVv), kSrcValu0, kSrcLu));
  PreloadVuGroup(vu, 2, kVuSuOp,
                 VuMoeOpWord(uint64_t(SuOp::kStMxfp8), kSrcValu1));
  PreloadVuGroup(vu, 2, kVuPrfOp, 0);
  PreloadVuGroup(vu, 2, kVuStaticDupOffset + kVuVrfWtIndex, vrf_wt);
  PreloadVuGroup(vu, 2, kVuStaticDupOffset + kVuVrfRdIndex, vrf_rd);
  PreloadVuGroup(vu, 2, kVuStaticDupOffset + kVuTypeVl, type_vl);
}

// 组 4/5：R core 两半求和。VL = 6144，BF16。VRF 第 16 项。
inline void PreloadVuAdd(Vu& vu) {
  uint64_t const type_vl = 6144u | (1u << kVuDataTypeShift);
  uint64_t const vrf = 16;
  PreloadVuGroup(vu, 4, kVuLuOp, VuMoeOpWord(uint64_t(LuOp::kLdBf16)));
  PreloadVuGroup(vu, 4, kVuSuOp, VuMoeOpWord(uint64_t(SuOp::kNop)));
  PreloadVuGroup(vu, 4, kVuPrfOp, kSrcLu);
  PreloadVuGroup(vu, 4, kVuStaticDupOffset + kVuVrfWtIndex, vrf);
  PreloadVuGroup(vu, 4, kVuStaticDupOffset + kVuTypeVl, type_vl);

  PreloadVuGroup(vu, 5, kVuLuOp, VuMoeOpWord(uint64_t(LuOp::kLdBf16)));
  PreloadVuGroup(vu, 5, kVuValu0Op,
                 VuMoeOpWord(uint64_t(ValuOp::kFaddVv), kSrcLu, kSrcVrfP0));
  PreloadVuGroup(vu, 5, kVuSuOp,
                 VuMoeOpWord(uint64_t(SuOp::kStBf16), kSrcValu0));
  PreloadVuGroup(vu, 5, kVuPrfOp, 0);
  PreloadVuGroup(vu, 5, kVuStaticDupOffset + kVuVrfRdIndex, vrf);
  PreloadVuGroup(vu, 5, kVuStaticDupOffset + kVuTypeVl, type_vl);
}

}  // namespace bach
}  // namespace latch

#endif
