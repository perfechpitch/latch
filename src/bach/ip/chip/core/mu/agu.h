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
// 启动前验证完整任务；CheckStep 检查每条实际原语仍满足地址边界。

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
  MuStep Peek() const { return cur; }
  MuStep Next() {
    MuStep result = cur;
    auto expert = cfg.ExpertCfg(cur.e_idx);
    if (++cur.k_idx >= expert.kblock) {
      cur.k_idx = 0;
      if (cfg.ep_reduce) {
        if (++cur.e_idx >= cfg.Experts()) {
          cur.e_idx = 0;
          if (++cur.n_idx >= cfg.nblock) cur.done = true;
        }
      } else if (++cur.n_idx >= cfg.nblock) {
        cur.n_idx = 0;
        if (++cur.e_idx >= cfg.Experts()) cur.done = true;
      }
    }
    return result;
  }
  bool Done() const { return cur.done; }

  // 三组地址。token 与结果在 Core Mem，硬件按 base + stream_id × stream_stride
  // 算，软件只写 base 与 stride；weight 在 Matrix Mem，无 stream 偏移。
  // 合并成一份的那一档，每个专家有自己的一份激活；每个专家各出一份的那一档，
  // 几个专家共用同一份激活。
  uint64_t TokenAddr(MuStep const& s) const {
    return cfg.a_addr + cfg.stream_id * cfg.a_stream_stride + ExpertOffOfA(s) +
           s.k_idx * MuAgu(cfg.ExpertCfg(s.e_idx)).TokenBytes();
  }
  // 权重按专家在本 EP Group 内的序号排，与 topK 里的先后无关，所以要外面把
  // 组内序号传进来（topK 里直接存的就是组内序号）。激活与结果那两侧按 topK 的先后排。
  uint64_t WeightAddr(MuStep const& s, uint64_t local_ep) const {
    if (cfg.decoded_mode) {
      auto expert = cfg.ExpertCfg(s.e_idx);
      uint64_t lane_bytes = MuAgu(expert).WeightBytes() / 64;
      uint64_t local = expert.b_addr + (s.n_idx * expert.kblock + s.k_idx) * lane_bytes;
      return (local / 128) * 8192 + local % 128;
    }
    return cfg.b_addr + local_ep * cfg.b_expert_stride +
           (s.n_idx * cfg.kblock + s.k_idx) * WeightBytes();
  }
  uint64_t WeightAddr(MuStep const& s) const {
    return WeightAddr(s, s.e_idx);
  }
  uint64_t OutAddr(MuStep const& s) const {
    return cfg.c_addr + cfg.stream_id * cfg.c_stream_stride + ExpertOffOfC(s) +
           s.n_idx * OutBytes();
  }
  uint64_t ExpertOffOfA(MuStep const& s) const {
    return cfg.ep_reduce ? s.e_idx * cfg.token_expert_stride : 0;
  }
  uint64_t ExpertOffOfC(MuStep const& s) const {
    return cfg.ep_reduce ? 0 : s.e_idx * cfg.output_expert_stride;
  }

  uint64_t TokenBytes() const {
    return cfg.PrimK() * numeric::ElemBitsOf(cfg.a_dtype) / 8;
  }
  uint64_t WeightBytes() const {
    return cfg.PrimK() * cfg.PrimN() * numeric::ElemBitsOf(cfg.b_dtype) / 8;
  }
  uint64_t OutBytes() const {
    // 输出 FP32 或 BF16。
    return cfg.PrimN() * (cfg.out_bf16 ? 2 : 4);
  }
  // token 那一段的 scale 个数：与 token 一一映射，随它从存储的 scale 旁带读出来，
  // 地址按元素算，primitive_type = 1 时一段 token 只占半行，取的也只是那半行的
  // scale。
  uint64_t ScaleBytes() const {
    uint64_t block = numeric::ScaleBlockOf(cfg.a_dtype);
    return block == 0 ? 0 : cfg.PrimK() / block;
  }
  // 权重那一块的 scale 个数：只有权重重是 MXFP8 那一档带，按列排，一列 K / 32 个。
  uint64_t WeightScaleBytes() const {
    uint64_t block = numeric::ScaleBlockOf(cfg.b_dtype);
    return block ? cfg.PrimK() * cfg.PrimN() / block : 0;
  }

  // token/结果至少 8B 对齐；MXFP8 token 为 32B 对齐。
  static bool Aligned(uint64_t addr, uint64_t grain) {
    return addr % grain == 0;
  }
  bool CheckStep(MuStep const& s, uint64_t cmem_size,
                 uint64_t mmem_size) const {
    MuAgu shape(cfg.ExpertCfg(s.e_idx));
    if (TokenAddr(s) > cmem_size || shape.TokenBytes() > cmem_size - TokenAddr(s)) return false;
    uint64_t weight_bytes = cfg.decoded_mode ? 8192 : shape.WeightBytes();
    uint64_t weight_base = cfg.decoded_mode ? WeightAddr(s) & ~127ull : WeightAddr(s);
    if (weight_base > mmem_size || weight_bytes > mmem_size - weight_base) return false;
    if (OutAddr(s) > cmem_size || shape.OutBytes() > cmem_size - OutAddr(s)) return false;
    if (!Aligned(TokenAddr(s), cfg.a_dtype == numeric::DataType::kMxfp8 ? 32 : 8)) return false;
    if (!Aligned(OutAddr(s), 8)) return false;
    return true;
  }

 private:
  MuTaskCfg cfg;
  MuStep cur;
};

}  // namespace bach
}  // namespace latch

#endif
