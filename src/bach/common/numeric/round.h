#ifndef _LATCH_BACH_COMMON_NUMERIC_ROUND_
#define _LATCH_BACH_COMMON_NUMERIC_ROUND_

// 舍入模式与高转低的窄化。
//
// VU 的 TYPE_VL.ROUND_MODE 在 bit[19:17]，只作用于三处高转低：LU 的
// ld.fp32.vm 在 DATA_TYPE=BF16 下把 FP32 窄化成 BF16、SU 的高转低写出、
// DATA_TYPE=BF16 时标量进向量通路的 FP32 → BF16。
//
// 编码照 RISC-V 的 frm：000 RNE、001 RTZ、010 RDN、011 RUP、100 RMM。设计文档
// 只给了位置与位宽，没给编码表，取 frm 并标为待定。
//
// 窄化的做法统一成一件事：把要丢掉的低位按模式决定加不加一，再截。这样 BF16
// 与更窄的格式走同一段逻辑，位为准。

#include <cstdint>

#include "bach/common/numeric/formats.h"

namespace latch {
namespace bach {
namespace numeric {

enum class RoundMode : uint32_t {
  kRne = 0,   // 就近，平局取偶
  kRtz = 1,   // 朝零截断
  kRdn = 2,   // 朝负无穷
  kRup = 3,   // 朝正无穷
  kRmm = 4,   // 就近，平局远离零
};

inline RoundMode RoundModeOf(uint64_t v) {
  return v <= 4 ? RoundMode(v) : RoundMode::kRne;
}

// 把 bits 的低 shift 位丢掉，按模式决定是否进位。sign 是这个数的符号位。
inline uint32_t RoundShift(uint32_t bits, uint32_t shift, RoundMode mode) {
  if (shift == 0) return bits;
  uint32_t keep = bits >> shift;
  uint32_t drop = bits & ((1u << shift) - 1u);
  if (drop == 0) return keep;
  uint32_t half = 1u << (shift - 1);
  uint32_t sign = bits >> 31;
  switch (mode) {
    case RoundMode::kRtz:
      return keep;
    case RoundMode::kRdn:
      return sign ? keep + 1 : keep;
    case RoundMode::kRup:
      return sign ? keep : keep + 1;
    case RoundMode::kRmm:
      return drop >= half ? keep + 1 : keep;
    default:
      // RNE：过半进、不足半舍，正好一半时看留下来的最低位。
      if (drop > half) return keep + 1;
      if (drop < half) return keep;
      return keep + (keep & 1u);
  }
}

// FP32 → BF16，按模式舍入。NaN 直接截，保住它还是 NaN。
inline uint16_t NarrowBf16(float v, RoundMode mode) {
  uint32_t b = BitsOf(v);
  if ((b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) != 0) {
    return uint16_t(b >> 16);
  }
  // 符号位不参与舍入，先摘出去。
  uint32_t sign = b & 0x80000000u;
  uint32_t mag = b & 0x7FFFFFFFu;
  uint32_t rounded = RoundShift(mag | sign, 16, mode);
  // RoundShift 拿 bit31 当符号，进位后可能溢到指数全 1，那就是 Inf，照留。
  return uint16_t((sign >> 16) | (rounded & 0x7FFFu));
}

// 高转低是不是把有限值变成了 NaN。LU 的 ld.fp32.vm 要据此置 DATA_CVT_ERROR。
inline bool NarrowMakesNan(float v, uint16_t bf) {
  float back = FromBf16(bf);
  return back != back && v == v;
}

}  // namespace numeric
}  // namespace bach
}  // namespace latch

#endif
