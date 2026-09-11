#ifndef _LATCH_BACH_COMMON_NUMERIC_FORMATS_
#define _LATCH_BACH_COMMON_NUMERIC_FORMATS_

// 数值格式的编解码。
//
// 六种格式：FP32、BF16、FP8 的 E4M3 与 E5M2、FP4 的 E2M1、以及 E8M0 这种只有
// 指数的 scale。MX 系列（MXFP8 / MXFP4 / NVFP4）是「一个 block 共用一个 scale」
// 的组合，见 mx.h。
//
// 中间累加一律 FP32：MU 的 CSA 树、Router 的 reduce、VU 的归约都是。输入输出
// 的精度由各自的配置寄存器定，进来先扩到 FP32、出去再按配置转回去。
//
// 这一层只管一个数怎么编怎么解，累加顺序在 accum.h。

#include <cstdint>
#include <cstring>

namespace latch {
namespace bach {
namespace numeric {

inline uint32_t BitsOf(float v) {
  uint32_t b;
  std::memcpy(&b, &v, 4);
  return b;
}
inline float FloatOf(uint32_t b) {
  float v;
  std::memcpy(&v, &b, 4);
  return v;
}

// ── BF16：FP32 的高 16 位 ──
//
// 舍入取 round-to-nearest-even：截断会让误差单向累积，一条 K=256 的累加链上偏
// 得很明显。硬件用哪一种 MAS 没写，这里定 RNE 并标出来，等 RTL 出来核对。
inline uint16_t ToBf16(float v) {
  uint32_t b = BitsOf(v);
  // NaN 直接截，保住它是 NaN 这件事，不让进位把它变成 Inf。
  if ((b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) != 0) {
    return uint16_t(b >> 16);
  }
  uint32_t lsb = (b >> 16) & 1u;
  uint32_t rounded = b + 0x7FFFu + lsb;
  return uint16_t(rounded >> 16);
}
inline float FromBf16(uint16_t v) { return FloatOf(uint32_t(v) << 16); }

// ── FP8 E4M3：1 符号 4 指数 3 尾数，偏置 7 ──
// OCP MX 规范里 E4M3 没有 Inf，全 1 指数加全 1 尾数才是 NaN。
inline float FromFp8E4m3(uint8_t v) {
  uint32_t sign = uint32_t(v >> 7) & 1u;
  uint32_t exp = uint32_t(v >> 3) & 0xFu;
  uint32_t man = uint32_t(v) & 0x7u;
  if (exp == 0xF && man == 0x7) {
    return FloatOf((sign << 31) | 0x7FC00000u);  // NaN
  }
  if (exp == 0) {
    // 非规格化：值 = (-1)^s × 2^-6 × (man / 8)
    if (man == 0) return FloatOf(sign << 31);
    float m = float(man) / 8.0f;
    float r = m * 0.015625f;  // 2^-6
    return sign ? -r : r;
  }
  uint32_t fexp = exp - 7 + 127;
  uint32_t bits = (sign << 31) | (fexp << 23) | (man << 20);
  return FloatOf(bits);
}

inline uint8_t ToFp8E4m3(float v) {
  uint32_t b = BitsOf(v);
  uint32_t sign = b >> 31;
  uint32_t exp = (b >> 23) & 0xFFu;
  uint32_t man = b & 0x7FFFFFu;
  if (exp == 0xFF) {
    // Inf 与 NaN 都落到 E4M3 的那一个 NaN 编码上：这个格式没有 Inf。
    return uint8_t((sign << 7) | 0x7Fu);
  }
  int32_t e = int32_t(exp) - 127 + 7;
  // exp = 15 在 E4M3 里是合法的一档，只有它配上尾数全 1 才是 NaN，所以最大有限
  // 值是 0x7E（2^8 × 1.75 = 448）。上溢的界因此在 e 超过 15 时才到。
  if (e > 0xF) {
    // 上溢按 Clamp 到最大有限值，不产生 Inf。计算异常由硬件自动 Clamp，
    // 不走 Drain & Trap。
    return uint8_t((sign << 7) | 0x7Eu);
  }
  if (e <= 0) {
    // 非规格化或下溢到零。按最小步长量化，正中间取偶数倍，与下面规格化那一
    // 段同一套 RNE，两段用不同的舍入会在 2^-10 这类正中间点上分叉。
    // mag 按符号位取而不按 v < 0：负零的比较结果是 false。
    float mag = sign ? -v : v;
    float step = 0.001953125f;  // 2^-9，E4M3 最小非规格化步长
    float q = mag / step;
    int32_t lo = int32_t(q);
    float frac = q - float(lo);
    if (frac > 0.5f || (frac == 0.5f && (lo & 1) != 0)) ++lo;
    if (lo > 7) lo = 7;
    return uint8_t((sign << 7) | uint32_t(lo));
  }
  // 规格化：尾数 RNE 到 3 位
  uint32_t m = man >> 20;
  uint32_t rest = man & 0xFFFFFu;
  uint32_t half = 1u << 19;
  if (rest > half || (rest == half && (m & 1u))) {
    ++m;
    if (m > 7) {
      m = 0;
      ++e;
      if (e > 0xF) return uint8_t((sign << 7) | 0x7Eu);
    }
  }
  // exp 与尾数都到顶那一格是 NaN 的编码，有限值不能落在那里。
  if (e == 0xF && m == 7) return uint8_t((sign << 7) | 0x7Eu);
  return uint8_t((sign << 7) | (uint32_t(e) << 3) | m);
}

// ── FP8 E5M2：1 符号 5 指数 2 尾数，偏置 15。有 Inf 与 NaN ──
inline float FromFp8E5m2(uint8_t v) {
  uint32_t sign = uint32_t(v >> 7) & 1u;
  uint32_t exp = uint32_t(v >> 2) & 0x1Fu;
  uint32_t man = uint32_t(v) & 0x3u;
  if (exp == 0x1F) {
    uint32_t bits = (sign << 31) | 0x7F800000u | (man << 21);
    return FloatOf(bits);
  }
  if (exp == 0) {
    if (man == 0) return FloatOf(sign << 31);
    float m = float(man) / 4.0f;
    float r = m * 0.00006103515625f;  // 2^-14
    return sign ? -r : r;
  }
  uint32_t fexp = exp - 15 + 127;
  return FloatOf((sign << 31) | (fexp << 23) | (man << 21));
}

// ── FP4 E2M1：1 符号 2 指数 1 尾数，偏置 1 ──
// 十六个取值：±{0, 0.5, 1, 1.5, 2, 3, 4, 6}
inline float FromFp4E2m1(uint8_t v) {
  static const float kTable[8] = {0.0f, 0.5f, 1.0f, 1.5f,
                                  2.0f, 3.0f, 4.0f, 6.0f};
  uint32_t sign = uint32_t(v >> 3) & 1u;
  float m = kTable[v & 0x7u];
  return sign ? -m : m;
}

inline uint8_t ToFp4E2m1(float v) {
  static const float kTable[8] = {0.0f, 0.5f, 1.0f, 1.5f,
                                  2.0f, 3.0f, 4.0f, 6.0f};
  // 按符号位取，不按 v < 0：负零的比较结果是 false，那样会把 −0 编成 +0。
  uint8_t sign = uint8_t((BitsOf(v) >> 31) & 1u);
  float mag = sign ? -v : v;
  // 超出这一档能表示的范围就 Clamp 到最大的那一格，与 E4M3 上溢的处理一致。
  // 这一步还挡住了另一件事：mag 大到一定程度时，它减各档得到的差在 float 下
  // 会全都相等，比出来的“最近”就不是最近的那一档。
  if (!(mag < 6.0f)) return uint8_t((sign << 3) | 7u);
  // 取最近的那一档，一样近时取偶数编码。
  uint8_t best = 0;
  float bestd = -1.0f;
  for (uint8_t i = 0; i < 8; ++i) {
    float d = mag - kTable[i];
    if (d < 0) d = -d;
    if (bestd < 0 || d < bestd || (d == bestd && (i & 1u) == 0)) {
      bestd = d;
      best = i;
    }
  }
  return uint8_t((sign << 3) | best);
}

// ── E8M0：只有 8 位指数的 scale，值是 2 的幂 ──
// MXFP8 的 block scale 用它。0xFF 是 NaN。
inline float FromE8m0(uint8_t v) {
  if (v == 0xFF) return FloatOf(0x7FC00000u);
  int32_t e = int32_t(v) - 127;
  if (e < -126) return 0.0f;
  uint32_t bits = uint32_t(e + 127) << 23;
  return FloatOf(bits);
}
inline uint8_t ToE8m0(float v) {
  uint32_t b = BitsOf(v);
  uint32_t exp = (b >> 23) & 0xFFu;
  return uint8_t(exp);
}

}  // namespace numeric
}  // namespace bach
}  // namespace latch

#endif
