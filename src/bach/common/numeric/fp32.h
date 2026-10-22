#ifndef LATCH_BACH_NUMERIC_FP32_H
#define LATCH_BACH_NUMERIC_FP32_H

#include <cstdint>
#include "bach/common/numeric/formats.h"

namespace latch {
namespace bach {
namespace numeric {

enum FpFlag : uint32_t { kFpNx = 1, kFpUf = 2, kFpOf = 4, kFpNv = 8 };
struct FpResult {
  uint32_t bits = 0, flags = 0;
  float Value() const { return FloatOf(bits); }
};
inline bool FpNan(uint32_t b) {
  return (b & 0x7fffffffu) > 0x7f800000u;
}
inline bool FpSnan(uint32_t b) { return FpNan(b) && !(b & 0x400000u); }
inline uint64_t FpShiftJam(uint64_t value, uint64_t shift) {
  if (!shift) return value;
  if (shift >= 64) return value != 0;
  return (value >> shift) | ((value << (64 - shift)) != 0);
}
inline FpResult FpRoundInteger(bool sign, uint64_t magnitude, int exponent) {
  uint32_t sign_bit = uint32_t(sign) << 31;
  if (!magnitude) return {sign_bit, 0};
  int high = 63 - __builtin_clzll(magnitude);
  int power = high + exponent;
  if (power > 127) return {sign_bit | 0x7f800000u, kFpOf | kFpNx};
  bool tiny = power < -126;
  int shift = tiny ? -149 - exponent : high - 23;
  uint64_t rounded = magnitude;
  bool inexact = false;
  if (shift > 0) {
    if (shift > 64) {
      rounded = 0;
      inexact = true;
    } else {
      uint64_t low = shift == 64 ? magnitude : magnitude & ((1ull << shift) - 1);
      uint64_t half = 1ull << (shift - 1);
      rounded = shift == 64 ? 0 : magnitude >> shift;
      inexact = low != 0;
      if (low > half || (low == half && (rounded & 1))) ++rounded;
    }
  } else if (shift < 0) {
    rounded <<= -shift;
  }
  uint32_t flags = inexact ? uint32_t(kFpNx) : 0u;
  if (tiny) {
    if (rounded < 0x800000u && inexact) flags |= kFpUf;
    return {sign_bit | uint32_t(rounded), flags};
  }
  if (rounded >= 0x1000000u) { rounded >>= 1; ++power; }
  if (power > 127) return {sign_bit | 0x7f800000u, kFpOf | kFpNx};
  return {sign_bit | (uint32_t(power + 127) << 23) |
              (uint32_t(rounded) & 0x7fffffu), flags};
}
inline uint64_t FpSignificand(uint32_t b) {
  return (b & 0x7fffffu) | ((b & 0x7f800000u) ? 0x800000u : 0);
}
inline int FpUnitExponent(uint32_t b) {
  uint32_t e = (b >> 23) & 255;
  return e ? int(e) - 150 : -149;
}
inline FpResult Fp32Mul(uint32_t a, uint32_t b) {
  bool sign = ((a ^ b) >> 31) != 0;
  uint32_t aa = a & 0x7fffffffu, bb = b & 0x7fffffffu;
  if (FpNan(a) || FpNan(b))
    return {0x7fc00000u, (FpSnan(a) || FpSnan(b)) ? kFpNv : 0u};
  if ((aa == 0x7f800000u && !bb) || (bb == 0x7f800000u && !aa))
    return {0x7fc00000u, kFpNv};
  if (aa == 0x7f800000u || bb == 0x7f800000u)
    return {(uint32_t(sign) << 31) | 0x7f800000u, 0};
  return FpRoundInteger(sign, FpSignificand(a) * FpSignificand(b),
                        FpUnitExponent(a) + FpUnitExponent(b));
}
inline FpResult Fp32Add(uint32_t a, uint32_t b) {
  uint32_t aa = a & 0x7fffffffu, bb = b & 0x7fffffffu;
  if (FpNan(a) || FpNan(b))
    return {0x7fc00000u, (FpSnan(a) || FpSnan(b)) ? kFpNv : 0u};
  if (aa == 0x7f800000u || bb == 0x7f800000u) {
    if (aa == bb && ((a ^ b) >> 31)) return {0x7fc00000u, kFpNv};
    return {aa == 0x7f800000u ? a : b, 0};
  }
  if (!aa && !bb) return {a & b & 0x80000000u, 0};
  if (!aa) return {b, 0};
  if (!bb) return {a, 0};
  if (aa < bb) { uint32_t swap = a; a = b; b = swap; }
  int exponent = FpUnitExponent(a);
  uint64_t large = FpSignificand(a) << 32;
  uint64_t small = FpShiftJam(FpSignificand(b) << 32,
                             exponent - FpUnitExponent(b));
  bool opposite = ((a ^ b) >> 31) != 0;
  uint64_t magnitude = opposite ? large - small : large + small;
  return FpRoundInteger(magnitude ? (a >> 31) != 0 : false,
                        magnitude, exponent - 32);
}
inline FpResult Fp32ToBf16(uint32_t bits) {
  if (FpNan(bits)) return {0x7fc0u, FpSnan(bits) ? kFpNv : 0u};
  if ((bits & 0x7fffffffu) == 0x7f800000u) return {bits >> 16, 0};
  uint32_t low = bits & 0xffffu, high = bits >> 16;
  uint32_t result = high + (low > 0x8000u || (low == 0x8000u && (high & 1)));
  uint32_t flags = low ? uint32_t(kFpNx) : 0u;
  if ((result & 0x7fffu) == 0x7f80u) flags |= kFpOf | kFpNx;
  if (!(result & 0x7f80u) && low) flags |= kFpUf;
  return {result, flags};
}

} // namespace numeric
} // namespace bach
} // namespace latch
#endif
