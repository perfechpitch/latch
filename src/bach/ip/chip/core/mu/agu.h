#ifndef _LATCH_BACH_IP_CHIP_CORE_MU_AGU_
#define _LATCH_BACH_IP_CHIP_CORE_MU_AGU_

// agu ×3 与 acu：算三组访存地址，顺带检查越界与对齐。
//
// 三个 agu 分别算 token、weight、结果的地址。任务拆分顺序由内往外是 tile_K、
// 专家、tile_N。这个顺序决定了地址怎么递增，也决定了 CSA 树里哪些乘积是
// 同一个 scale block 的。
//
// 专家排在 tile_K 与 tile_N 之间：K 那一层的部分和攒在每 lane 一个的 kblock
// 累加寄存器里，乘上这个专家的 topK 权重之后进同样是每 lane 一个的 EP 累加
// 寄存器。两个寄存器都只存一列那么宽，所以同一列的几个专家要连着算完。
//
// acu 查出越界或非对齐就走 Drain & Trap 四步：阻塞任务下发 → 清理已发出的
// 访存请求（已请求的回复照常处理，不再发起新的）→ 排空计算流水线 → 恢复默认
// 状态。Drain 期间已进入脉动通路的合法数据照常算完写回，只丢越界那一笔的数据。
// 本轮只留状态位与接口名，不实现行为。

#include <cstdint>

#include "bach/ip/chip/core/mu/regfile.h"

namespace latch {
namespace bach {

// 一次原语要读多少字节。token 是 K 个元素，weight 是 K × N 个。
struct MuStep {
  uint64_t k_idx = 0;   // 走到第几个 tile_K
  uint64_t e_idx = 0;   // 走到 topK 里的第几个专家
  uint64_t n_idx = 0;   // 走到第几个 tile_N
  bool done = false;
};

class MuAgu {
 public:
  explicit MuAgu(MuTaskCfg const& task) : cfg(task) {}

  // 由内往外：tile_K、专家、tile_N。
  MuStep Next() {
    MuStep s = cur;
    ++cur.k_idx;
    if (cur.k_idx >= cfg.kblock) {
      cur.k_idx = 0;
      ++cur.e_idx;
      if (cur.e_idx >= cfg.Experts()) {
        cur.e_idx = 0;
        ++cur.n_idx;
        if (cur.n_idx >= cfg.nblock) cur.done = true;
      }
    }
    return s;
  }
  bool Done() const { return cur.done; }

  // 三组地址。token 与结果在 Core Mem，weight 在 Matrix Mem。
  // 合并成一份的那一档，每个专家有自己的一份激活；每个专家各出一份的那一档，
  // 几个专家共用同一份激活。
  uint64_t TokenAddr(MuStep const& s) const {
    return cfg.addr_token + ExpertOffOfA(s) + s.k_idx * TokenBytes();
  }
  // 权重按专家在本 EP Group 内的序号排，与 topK 里的先后无关，所以要外面把
  // 全局专家号翻成组内序号再传进来。激活与结果那两侧按 topK 的先后排。
  uint64_t WeightAddr(MuStep const& s, uint64_t local_ep) const {
    // 各 lane 访存地址相同，只发一个地址然后逐级脉动到各 lane。
    return cfg.addr_weight + local_ep * cfg.b_expert_stride +
           (s.n_idx * cfg.kblock + s.k_idx) * WeightBytes();
  }
  uint64_t WeightAddr(MuStep const& s) const {
    return WeightAddr(s, s.e_idx);
  }
  uint64_t OutAddr(MuStep const& s) const {
    return cfg.addr_out + ExpertOffOfC(s) + s.n_idx * OutBytes();
  }
  uint64_t ScaleAddr(MuStep const& s) const {
    return cfg.addr_scale + ExpertOffOfA(s) + s.k_idx * ScaleBytes();
  }

  uint64_t ExpertOffOfA(MuStep const& s) const {
    return cfg.ep_reduce ? s.e_idx * cfg.ac_expert_stride : 0;
  }
  uint64_t ExpertOffOfC(MuStep const& s) const {
    return cfg.ep_reduce ? 0 : s.e_idx * cfg.ac_expert_stride;
  }

  uint64_t TokenBytes() const {
    return cfg.PrimK() * numeric::ElemBitsOf(cfg.dtype_ab) / 8;
  }
  uint64_t WeightBytes() const {
    return cfg.PrimK() * cfg.PrimN() * numeric::ElemBitsOf(cfg.dtype_ab) / 8;
  }
  uint64_t OutBytes() const {
    // 输出 FP32 或 BF16。
    return cfg.PrimN() * (cfg.out_bf16 ? 2 : 4);
  }
  uint64_t ScaleBytes() const {
    uint64_t block = numeric::ScaleBlockOf(cfg.dtype_ab);
    return block == 0 ? 0 : cfg.PrimK() / block;
  }

  // acu：越界与对齐。Token 与 Weight 读要 16 B 对齐，结果写要 16 B 或 1 KB。
  static bool Aligned(uint64_t addr, uint64_t grain) {
    return addr % grain == 0;
  }
  bool CheckStep(MuStep const& s, uint64_t cmem_size,
                 uint64_t mmem_size) const {
    if (TokenAddr(s) + TokenBytes() > cmem_size) return false;
    if (WeightAddr(s) + WeightBytes() > mmem_size) return false;
    if (OutAddr(s) + OutBytes() > cmem_size) return false;
    if (!Aligned(TokenAddr(s), 16)) return false;
    if (!Aligned(OutAddr(s), 16)) return false;
    return true;
  }

 private:
  MuTaskCfg cfg;
  MuStep cur;
};

}  // namespace bach
}  // namespace latch

#endif
