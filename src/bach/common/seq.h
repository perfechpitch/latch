#ifndef _LATCH_BACH_COMMON_SEQ_
#define _LATCH_BACH_COMMON_SEQ_

// 序号的回绕比较。
//
// 入口桩按 token 顺序发、按 retired 回收，序号位宽有限（SEQ_W = 16），跑久了会
// 绕回 0。直接比大小会在绕回那一刻判反，所以一律比模 2^W 的差值：只要在飞的量
// 不超过 2^(W-1)，差值就能唯一还原出谁在前。

#include <cstdint>

namespace latch {
namespace bach {

constexpr uint32_t kSeqWidth = 16;
constexpr uint64_t kSeqMod = 1ull << kSeqWidth;
constexpr uint64_t kSeqHalf = kSeqMod / 2;

inline uint64_t SeqAdd(uint64_t s, uint64_t n) { return (s + n) % kSeqMod; }

// b 比 a 前进了多少。回绕后仍然是正确的正数，前提是差值不超过半个模。
inline uint64_t SeqDiff(uint64_t a, uint64_t b) { return (b + kSeqMod - a) % kSeqMod; }

// b 是不是在 a 之后。差值落在后半程算作 a 更新，即 b 落在过去。
inline bool SeqAfter(uint64_t a, uint64_t b) {
  uint64_t d = SeqDiff(a, b);
  return d != 0 && d < kSeqHalf;
}

}  // namespace bach
}  // namespace latch

#endif
