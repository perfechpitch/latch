#ifndef _LATCH_BACH_COMMON_NUMERIC_ACCUM_
#define _LATCH_BACH_COMMON_NUMERIC_ACCUM_

// 累加顺序。
//
// 浮点加法不满足结合律，所以「按什么顺序加」是结果的一部分，不是实现细节。
// 模型与参考实现必须用同一个顺序，逐元素比对才不留容差。
//
// 三处的顺序各不相同，各自的依据写在对应函数上：
//   MU 的 CSA 树      按 scale block 分组，块内先加、块间再加
//   Router 的 reduce  按到达顺序，FP32 中间精度
//   VU 的归约         LANES 内先归约，再走 ceil(log2 SEG) 级树
//
// MAS 只给了原则没给细节，这里各定一个默认取法并标出来，等 RTL 出来核对。

#include <cstdint>
#include <vector>

#include "bach/common/numeric/formats.h"

namespace latch {
namespace bach {
namespace numeric {

// MU 的 CSA 树：按 scale block 分组累加。
//
// 一个 block 内的元素共用一个 scale，所以先把块内的乘积加完再乘 scale，比
// 逐个乘 scale 再加少一轮舍入。块间用顺序加 —— CSA 树的形状 MAS 没给，顺序加
// 与平衡树加出来的位不一样，这里取顺序加并标为待定。
inline float AccumByScaleBlock(std::vector<float> const& prods,
                               std::vector<float> const& block_scale,
                               uint64_t block) {
  float total = 0.0f;
  uint64_t nblock = block_scale.size();
  for (uint64_t b = 0; b < nblock; ++b) {
    float part = 0.0f;
    uint64_t begin = b * block;
    uint64_t end = begin + block;
    if (end > prods.size()) end = prods.size();
    for (uint64_t i = begin; i < end; ++i) part += prods[i];
    total += part * block_scale[b];
  }
  return total;
}

// 没有 block scale 时（BF16 × BF16）就是顺序加。
inline float AccumInOrder(std::vector<float> const& v) {
  float total = 0.0f;
  for (float x : v) total += x;
  return total;
}

// VU 的归约：LANES 内先归约，再走 ceil(log2 SEG) 级树。
//
// 两级分开是因为 lane 内是物理相邻的加法器链、段间是一棵树，位置不同顺序就
// 不同。段数不是 2 的幂时，最后一级的落单项直接带到下一级。
//
// 《VU-DSA 微操作与编码方案》只把 vfredusum.vs 写作「无序求和归约」，没给具体
// 的累加顺序；这里的两级顺序取自《VU DSA》的建模约定，标为待定。浮点加法不满足
// 结合律，所以顺序是结果的一部分：参考实现必须照抄这一个顺序，逐 bit 比对才
// 立得住。等 RTL 出来核对真实的加法器树形状。
inline float ReduceTree(std::vector<float> const& lanes, uint64_t lane_num) {
  std::vector<float> seg;
  for (uint64_t i = 0; i < lanes.size(); i += lane_num) {
    float s = 0.0f;
    uint64_t end = i + lane_num;
    if (end > lanes.size()) end = lanes.size();
    for (uint64_t k = i; k < end; ++k) s += lanes[k];
    seg.push_back(s);
  }
  while (seg.size() > 1) {
    std::vector<float> next;
    for (uint64_t i = 0; i + 1 < seg.size(); i += 2) {
      next.push_back(seg[i] + seg[i + 1]);
    }
    if (seg.size() % 2 == 1) next.push_back(seg.back());
    seg.swap(next);
  }
  return seg.empty() ? 0.0f : seg[0];
}

// 计算异常由硬件自动 Clamp，不走 Drain & Trap、不阻塞流水。
inline float ClampNanInf(float v) {
  uint32_t b = BitsOf(v);
  uint32_t exp = (b >> 23) & 0xFFu;
  if (exp != 0xFF) return v;
  uint32_t sign = b >> 31;
  // NaN 与 Inf 都夹到该符号的最大有限值。
  return FloatOf((sign << 31) | 0x7F7FFFFFu);
}

}  // namespace numeric
}  // namespace bach
}  // namespace latch

#endif
