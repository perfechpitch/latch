#ifndef _LATCH_BACH_COMMON_NUMERIC_MX_
#define _LATCH_BACH_COMMON_NUMERIC_MX_

// MX 系列：一个 block 共用一个 scale。
//
//   MXFP8   元素 FP8，block 32 个，scale 是 E8M0
//   MXFP4   元素 FP4，block 16 个，scale 是 FP8
//   NVFP4   元素 FP4，block 16 个，scale 是 FP8 的 E4M3
//
// 算力按精度组合分六档：BF16×BF16 4K MACs；MXFP8×MXFP8 8K；MXFP8×MXFP4 与
// MXFP8×NVFP4 各 16K；BF16×MXFP4 与 BF16×NVFP4 各 8K。
//
// scale block 的大小直接决定 CSA 树怎么分组，所以它是累加顺序的一部分。

#include <cstdint>
#include <vector>

#include "bach/common/numeric/formats.h"
#include "bach/common/numeric/round.h"

namespace latch {
namespace bach {
namespace numeric {

enum class DataType : uint32_t {
  kBf16 = 0,
  kMxfp8 = 1,
  kMxfp4 = 2,
  kNvfp4 = 3,
  kFp32 = 4,
};

// 各格式的 block 大小。BF16 与 FP32 没有 block scale，返回 0。
inline uint64_t ScaleBlockOf(DataType t) {
  switch (t) {
    case DataType::kMxfp8:
      return 32;
    case DataType::kMxfp4:
    case DataType::kNvfp4:
      return 16;
    default:
      return 0;
  }
}

// 一个元素占几位。
inline uint64_t ElemBitsOf(DataType t) {
  switch (t) {
    case DataType::kBf16:
      return 16;
    case DataType::kMxfp8:
      return 8;
    case DataType::kMxfp4:
    case DataType::kNvfp4:
      return 4;
    default:
      return 32;
  }
}

// 把一段字节按格式解成 FP32。scale 单独给，块内元素共用。
inline std::vector<float> Decode(DataType t, std::vector<uint8_t> const& bytes,
                                 uint64_t count) {
  std::vector<float> out;
  out.reserve(count);
  for (uint64_t i = 0; i < count; ++i) {
    switch (t) {
      case DataType::kBf16: {
        uint64_t off = i * 2;
        if (off + 1 >= bytes.size()) {
          out.push_back(0.0f);
          break;
        }
        uint16_t v = uint16_t(bytes[off]) | (uint16_t(bytes[off + 1]) << 8);
        out.push_back(FromBf16(v));
        break;
      }
      case DataType::kMxfp8:
        out.push_back(i < bytes.size() ? FromFp8E4m3(bytes[i]) : 0.0f);
        break;
      case DataType::kMxfp4:
      case DataType::kNvfp4: {
        // 一个字节装两个元素，低半字节在前。
        uint64_t off = i / 2;
        if (off >= bytes.size()) {
          out.push_back(0.0f);
          break;
        }
        uint8_t nib = (i % 2 == 0) ? (bytes[off] & 0xFu) : (bytes[off] >> 4);
        out.push_back(FromFp4E2m1(nib));
        break;
      }
      default: {
        uint64_t off = i * 4;
        if (off + 3 >= bytes.size()) {
          out.push_back(0.0f);
          break;
        }
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) v |= uint32_t(bytes[off + k]) << (8 * k);
        out.push_back(FloatOf(v));
        break;
      }
    }
  }
  return out;
}

// 把 scale 字节解成 FP32 的一组。MXFP8 用 E8M0，FP4 那两档用 FP8。
inline std::vector<float> DecodeScale(DataType t,
                                      std::vector<uint8_t> const& bytes,
                                      uint64_t nblock) {
  std::vector<float> out;
  out.reserve(nblock);
  for (uint64_t i = 0; i < nblock; ++i) {
    if (i >= bytes.size()) {
      out.push_back(1.0f);
      continue;
    }
    out.push_back(t == DataType::kMxfp8 ? FromE8m0(bytes[i])
                                        : FromFp8E4m3(bytes[i]));
  }
  return out;
}


// Decode 的对偶：把一组 FP32 编回该格式的字节。高转低按 mode 舍入。
//
// scale 由调用方给。MXFP8 的 scale 是 E8M0，FP4 那两档是 FP8 —— 与 DecodeScale
// 对称。VU 的 SU 走这一条写回 Core Mem。
inline std::vector<uint8_t> Encode(DataType t, std::vector<float> const& v,
                                   std::vector<float> const& block_scale,
                                   RoundMode mode) {
  std::vector<uint8_t> out;
  uint64_t block = ScaleBlockOf(t);
  auto scaled = [&](uint64_t i) {
    if (block == 0 || block_scale.empty()) return v[i];
    uint64_t b = i / block;
    float s = b < block_scale.size() ? block_scale[b] : 1.0f;
    return s == 0.0f ? 0.0f : v[i] / s;
  };
  switch (t) {
    case DataType::kBf16:
      out.resize(v.size() * 2);
      for (uint64_t i = 0; i < v.size(); ++i) {
        uint16_t h = NarrowBf16(v[i], mode);
        out[i * 2] = uint8_t(h & 0xFFu);
        out[i * 2 + 1] = uint8_t(h >> 8);
      }
      break;
    case DataType::kMxfp8:
      out.resize(v.size());
      for (uint64_t i = 0; i < v.size(); ++i) out[i] = ToFp8E4m3(scaled(i));
      break;
    case DataType::kMxfp4:
    case DataType::kNvfp4:
      out.resize((v.size() + 1) / 2, 0);
      for (uint64_t i = 0; i < v.size(); ++i) {
        uint8_t nib = ToFp4E2m1(scaled(i)) & 0xFu;
        if (i % 2 == 0) {
          out[i / 2] = uint8_t((out[i / 2] & 0xF0u) | nib);
        } else {
          out[i / 2] = uint8_t((out[i / 2] & 0x0Fu) | (nib << 4));
        }
      }
      break;
    default:
      out.resize(v.size() * 4);
      for (uint64_t i = 0; i < v.size(); ++i) {
        uint32_t b = BitsOf(v[i]);
        for (int k = 0; k < 4; ++k) out[i * 4 + k] = uint8_t((b >> (8 * k)) & 0xFFu);
      }
      break;
  }
  return out;
}

// 一组元素按 block 算出各块的 scale：取块内绝对值最大的那个定阶。
inline std::vector<float> MakeScale(DataType t, std::vector<float> const& v) {
  std::vector<float> out;
  uint64_t block = ScaleBlockOf(t);
  if (block == 0) return out;
  // 每档格式能表示的最大绝对值，用来把块内最大值压进范围。
  float emax = t == DataType::kMxfp8 ? 448.0f : 6.0f;
  for (uint64_t b = 0; b * block < v.size(); ++b) {
    float peak = 0.0f;
    uint64_t end = (b + 1) * block;
    if (end > v.size()) end = v.size();
    for (uint64_t i = b * block; i < end; ++i) {
      float a = v[i] < 0.0f ? -v[i] : v[i];
      if (a > peak) peak = a;
    }
    if (peak == 0.0f) {
      out.push_back(1.0f);
      continue;
    }
    float raw = peak / emax;
    // MXFP8 的 scale 只有 E8M0 这一档 2 的幂，编回去再解出来才是真正生效的值。
    out.push_back(t == DataType::kMxfp8 ? FromE8m0(ToE8m0(raw))
                                        : FromFp8E4m3(ToFp8E4m3(raw)));
  }
  return out;
}

// scale 编成字节。与 DecodeScale 对称。
inline std::vector<uint8_t> EncodeScale(DataType t,
                                        std::vector<float> const& scale) {
  std::vector<uint8_t> out;
  out.reserve(scale.size());
  for (float s : scale) {
    out.push_back(t == DataType::kMxfp8 ? ToE8m0(s) : ToFp8E4m3(s));
  }
  return out;
}

}  // namespace numeric
}  // namespace bach
}  // namespace latch

#endif
