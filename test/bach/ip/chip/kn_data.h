#ifndef _LATCH_TEST_BACH_IP_CHIP_KN_DATA_
#define _LATCH_TEST_BACH_IP_CHIP_KN_DATA_

// 一层 MoE 按 EP6+TP8 的 KN 拆分摊开时，core 层与 chip 层几份用例共用的东西：
// 尺寸与 Core Mem、Matrix Mem 上的摆放，权重与 token 的生成，把一个 core 分到的
// 那一片权重按 tile 摆进 Matrix Mem。
//
// 尺寸与摆放与 compiler/kernel/bach.h 的 MOE_* 同源，生成规则与
// compiler/reference/vectors.py 的 kn_* 逐字节相同，改一处要一起改。

#include <cstdint>
#include <utility>
#include <vector>

namespace latch {
namespace bach {
namespace kn {

// ── 尺寸 ──
//
// 一个 EP 组两层 × 4 列共 8 颗 chip，每颗 chip 8 个计算 core。chip 在组里的序号
// c 定它分到 FC1、FC3 的哪一段 N 与 FC2 的哪一段 K；core 的逻辑槽位 s 定它分到
// FC1、FC3 的哪一段 K 与 FC2 的哪一段 N。槽位 7 是 dot core。
constexpr uint64_t kEmbed = 6144;
constexpr uint64_t kInter = 2048;
constexpr uint64_t kSlots = 8;
constexpr uint64_t kDotSlot = 7;
constexpr uint64_t kExperts = 2;
constexpr uint64_t kSegEmbed = kEmbed / kSlots;   // 768
constexpr uint64_t kSegInter = kInter / kSlots;   // 256
// FC1 与 FC3 按 1×K128×N64 切块，FC2 按 1×K64×N128（K128×N64 阵列开 vlane = 2）。
constexpr uint64_t kFc13K = 128, kFc13N = 64;
constexpr uint64_t kFc2K = 64, kFc2N = 128;
constexpr uint64_t kTileBytes = 8192;
// chip 内那条归约链的逻辑槽位次序，链尾是 dot core。
constexpr uint64_t kChipChain[kSlots] = {6, 5, 4, 0, 1, 2, 3, 7};

// ── Core Mem 上一个 stream 里的摆放 ──
//
// Core Mem 按 stream 切成 kStreamNum 片，每片 kStreamStride 字节，与 AGCU 的
// cm.stream_stride、kernel 的 CMEM_STREAM_STRIDE 同源。多 token 时每个 token 占
// 一片。topK 表不落 Core Mem，改由 DTE 直接写进 MU 的 topK_ep_table。
constexpr uint64_t kStreamNum = 16;
constexpr uint64_t kStreamStride = 64 * 1024;
constexpr uint64_t kSwHead = 16;
constexpr uint64_t kTokenOff = 0x0000;
constexpr uint64_t kTopkOff = 0x1800;
constexpr uint64_t kPartOff = 0x2000;
constexpr uint64_t kPartStride = 0x200;
constexpr uint64_t kFc1Off = kPartOff + kSwHead;
constexpr uint64_t kFc3Off = kFc1Off + kExperts * kPartStride;
constexpr uint64_t kPartBytes = kSwHead + 2 * kExperts * kPartStride;
constexpr uint64_t kRedOff = 0x2880;
constexpr uint64_t kActOff = 0x3100;
constexpr uint64_t kActStride = 0x100;
constexpr uint64_t kActBytes = kExperts * kActStride;
constexpr uint64_t kRowOff = 0x37F0;
constexpr uint64_t kConcatOff = kRowOff + kSwHead;
constexpr uint64_t kFc2Bytes = kSegEmbed * 2;
constexpr uint64_t kRowBytes = kSwHead + kSlots * kFc2Bytes;
inline uint64_t ConcatOf(uint64_t s) { return kConcatOff + s * kFc2Bytes; }

// ── Matrix Mem ──
constexpr uint64_t kMmW1 = 0x000000;
constexpr uint64_t kMmW3 = 0x100000;
constexpr uint64_t kMmW2 = 0x200000;
constexpr uint64_t kMmStride = 0x030000;

// ── R core 与 B core ──
constexpr uint64_t kRcSlots = 16;
constexpr uint64_t kRcHalfBytes = 0x3080;
constexpr uint64_t kRcSlotBytes = 2 * kRcHalfBytes;
constexpr uint64_t kRcFlagOff = 0x0000;
constexpr uint64_t kRcHeadOff = 0x0380;
constexpr uint64_t kBcTokenBytes = kEmbed;
inline uint64_t RcLand(uint64_t user, uint64_t half) {
  return (user % kRcSlots) * kRcSlotBytes + half * kRcHalfBytes;
}

// ── 生成 ──

constexpr uint64_t kW1Seed = 0x100000;
constexpr uint64_t kW3Seed = 0x200000;
constexpr uint64_t kW2Seed = 0x300000;
constexpr uint64_t kTokenSeed = 0x4001;
// topK 里两个专家的权重。
constexpr float kWep[kExperts] = {0.75f, 0.25f};

inline std::vector<uint8_t> Pattern(uint64_t n, uint64_t seed) {
  std::vector<uint8_t> v;
  v.reserve(n);
  uint64_t s = seed;
  for (uint64_t i = 0; i < n; ++i) {
    s = s * 1103515245u + 12345u;
    v.push_back(uint8_t((s >> 16) & 0xFFu));
  }
  return v;
}

// 一段 MXFP8 元素：抹掉 NaN 编码。
inline std::vector<uint8_t> Data(uint64_t count, uint64_t seed) {
  std::vector<uint8_t> v = Pattern(count, seed);
  for (uint8_t& b : v) {
    if ((b & 0x7Fu) == 0x7Fu) b = uint8_t(b & 0xFEu);
  }
  return v;
}

// 一段 E8M0 scale：收进 2^-14～2^-7。种子与同一段数据相同，另异或一个常数。
inline std::vector<uint8_t> Scale(uint64_t count, uint64_t seed) {
  std::vector<uint8_t> v = Pattern(count, seed ^ 0x5CA1Eu);
  for (uint8_t& b : v) b = uint8_t(113 + (b % 8));
  return v;
}

// 一个矩阵在第 group 个 EP 组、topK 里第 expert 个专家上的种子。
inline uint64_t SeedOf(uint64_t base, uint64_t group, uint64_t expert) {
  return base + (group * kExperts + expert) * 0x1000;
}

// 一个完整形状的矩阵，按 tile 播种：第 (r0, c0) 起的那个 tile 用
// seed + tile 编号生成，编号只由它在完整矩阵里的位置定。tile 里的数据列优先，
// scale 每列 tile_k / 32 个。
struct Matrix {
  uint64_t rows = 0, cols = 0, tile_k = 0, tile_n = 0, seed = 0;

  std::pair<std::vector<uint8_t>, std::vector<uint8_t>> Tile(uint64_t r0,
                                                             uint64_t c0) const {
    uint64_t tid = (c0 / tile_n) * (rows / tile_k) + r0 / tile_k;
    return {Data(tile_k * tile_n, seed + tid),
            Scale(tile_n * (tile_k / 32), seed + tid)};
  }
};

// W1 或 W3：kEmbed 行 × kInter 列，按 K128×N64 分块。
inline Matrix W13(uint64_t base, uint64_t group, uint64_t expert) {
  return {kEmbed, kInter, kFc13K, kFc13N, SeedOf(base, group, expert)};
}
// W2：kInter 行 × kEmbed 列，按 K64×N128 分块。
inline Matrix W2(uint64_t group, uint64_t expert) {
  return {kInter, kEmbed, kFc2K, kFc2N, SeedOf(kW2Seed, group, expert)};
}

// 把一个 core 分到的那一片按 tile 摆进 Matrix Mem：第 n 个 tile_N 的第 k 个
// tile_K 在 at + (n × kblock + k) × 8192 处，scale 随它进 scale 旁带。这一片从
// 完整矩阵的第 row0 行、第 col0 列起。
template <typename Mem>
void PokeSlice(Mem& mm, uint64_t at, Matrix const& m, uint64_t row0,
               uint64_t col0, uint64_t kblock, uint64_t nblock) {
  for (uint64_t n = 0; n < nblock; ++n) {
    for (uint64_t k = 0; k < kblock; ++k) {
      auto t = m.Tile(row0 + k * m.tile_k, col0 + n * m.tile_n);
      uint64_t addr = at + (n * kblock + k) * kTileBytes;
      mm.Poke(addr, t.first);
      mm.PokeScale(addr, t.second);
    }
  }
}

// 一个 core 的三个矩阵都摆好。topK 里第 e 个专家在本组内排第 local[e] 位。
template <typename Mem>
void PokeCoreWeights(Mem& mm, uint64_t group, uint64_t chip, uint64_t slot,
                     uint64_t const local[kExperts]) {
  for (uint64_t e = 0; e < kExperts; ++e) {
    uint64_t off = local[e] * kMmStride;
    PokeSlice(mm, kMmW1 + off, W13(kW1Seed, group, e), slot * kSegEmbed,
              chip * kSegInter, kSegEmbed / kFc13K, kSegInter / kFc13N);
    PokeSlice(mm, kMmW3 + off, W13(kW3Seed, group, e), slot * kSegEmbed,
              chip * kSegInter, kSegEmbed / kFc13K, kSegInter / kFc13N);
    PokeSlice(mm, kMmW2 + off, W2(group, e), chip * kSegInter,
              slot * kSegEmbed, kSegInter / kFc2K, kSegEmbed / kFc2N);
  }
}

// 第 k 个 token：kEmbed 个 MXFP8 与它的 scale，都用种子 kTokenSeed + k 生成。只
// 发一个 token 的用例用第 0 个。
inline std::vector<uint8_t> TokenData(uint64_t k = 0) {
  return Data(kEmbed, kTokenSeed + k);
}
inline std::vector<uint8_t> TokenScale(uint64_t k = 0) {
  return Scale(kEmbed / 32, kTokenSeed + k);
}

}  // namespace kn
}  // namespace bach
}  // namespace latch

#endif
