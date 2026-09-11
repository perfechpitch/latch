#ifndef _LATCH_BACH_IP_CHIP_CORE_MU_MATRIX_EXE_
#define _LATCH_BACH_IP_CHIP_CORE_MU_MATRIX_EXE_

// matrix exe：32 个物理 Lane，左右镜像各 16，单 Lane 内 10 级混合高频流水。
//
// 物理阵列规格二选一：1×K256×N32（输出带宽 128 B）或 1×K128×N64（256 B）。
//
// bit 级累加顺序是这一层的核心：CSA 树按 scale block 分组累加，块内先把乘积
// 加完再乘 scale，比逐个乘 scale 再加少一轮舍入。参考实现必须用同一个顺序：
// 浮点加法不满足结合律，顺序是结果的一部分，不是实现细节。
//
// vlane 机制：把 MAC 按 vlane 分组，在 CSA 加法树的第 128 输入层级插旁路 MUX，
// 单 lane 同时输出多个结果。vlane 有 1 和 2 两种。vlane=2 时每个 lane 算两个
// 半长的点积，所以块的划分也跟着减半。
//
// 计算异常 MATH_NAN_INF 不走 Drain & Trap、不阻塞流水，由硬件自动 Clamp。
//
// 两级累加寄存器，都是每 lane 一组，所以都只存一列那么宽：
//
//   kblock 累加  一列权重比物理阵列的 K 长时，一笔任务按 tile 走 kblock 遍，
//                每遍算出这一列的一段部分和，攒在这里
//   EP 累加      一列的部分和攒完之后乘这个专家的 topK 权重，再累加到这里
//
// 一列的几段与这一列的几个专家都连着算，所以两个寄存器都不会同时装两列的中间
// 值。浮点加法不满足结合律，段内先各自加完、段间再顺序加，与一次性把 K 个乘积
// 加完不是同一个数，参考实现按同一个分段顺序算。
//
// 专家各出一份结果的那一档（FC1 与 FC3）不经 EP 累加：每个专家的一列算完就是
// 一个结果。

#include <deque>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/common/numeric/accum.h"
#include "bach/common/numeric/mx.h"
#include "bach/ip/chip/core/mu/regfile.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 32 个物理 Lane，左右镜像各 16。
constexpr uint64_t kMuLaneNum = 32;
// 单 Lane 内 10 级流水。
constexpr uint64_t kMuLaneDepth = 10;

class MatrixExe : public BachModule {
 public:
  MatrixExe(ClockPtr clock, const std::string& name, uint64_t parent = 0,
            bool tick = true)
      : BachModule(clock, name, parent, tick),
        prims(clock),
        busy(clock),
        nan_clamps(clock) {}

  // 一次原语：token 是 K 个元素，weight 是 K × N，输出 N 个。
  //
  // 这里就是数据金标准：参考实现按同一个 AccumByScaleBlock 算，逐元素比对不留
  // 容差。算完排进流水线，kMuLaneDepth 拍后出来。
  //
  // k_first / k_last 说这一段在这一列的哪个位置，ep_first / ep_last 说这个专家
  // 在这一列的几个专家里的哪个位置。一列只占一段、只有一个专家时四个都是真，与
  // 不分块一样。w_ep 是这个专家的 topK 权重，只在几个专家合并成一份时用。
  void Issue(MuTaskCfg const& cfg, std::vector<uint8_t> const& token,
             std::vector<uint8_t> const& weight,
             std::vector<uint8_t> const& scale, uint64_t out_addr = 0,
             bool k_first = true, bool k_last = true, bool ep_first = true,
             bool ep_last = true, float w_ep = 1.0f) {
    uint64_t k = cfg.PrimK();
    uint64_t n = cfg.PrimN();
    uint64_t block = numeric::ScaleBlockOf(cfg.dtype_ab);
    uint64_t nblock = block == 0 ? 0 : k / block;

    std::vector<float> a = numeric::Decode(cfg.dtype_ab, token, k);
    std::vector<float> sc =
        block == 0 ? std::vector<float>()
                   : numeric::DecodeScale(cfg.dtype_ab, scale, nblock);

    if (k_first) {
      LOGCHECK(ksplit.empty(), "MatrixExe: 上一列的部分和还没输出就开了新的一列。");
      ksplit.assign(n, 0.0f);
    } else {
      LOGCHECK(ksplit.size() == n, "MatrixExe: 同一列的两段 N 不一样长。");
    }
    for (uint64_t j = 0; j < n; ++j) {
      // 取这一列的 K 个权重。weight 按列优先排：第 j 列在偏移 j × K 处。
      uint64_t elem_bits = numeric::ElemBitsOf(cfg.dtype_ab);
      uint64_t col_bytes = k * elem_bits / 8;
      std::vector<uint8_t> col;
      uint64_t begin = j * col_bytes;
      for (uint64_t t = 0; t < col_bytes && begin + t < weight.size(); ++t) {
        col.push_back(weight[begin + t]);
      }
      std::vector<float> b = numeric::Decode(cfg.dtype_ab, col, k);

      std::vector<float> prod(k, 0.0f);
      for (uint64_t i = 0; i < k; ++i) prod[i] = a[i] * b[i];

      // 有 block scale 就按块分组累加，没有就顺序加。
      float acc = block == 0 ? numeric::AccumInOrder(prod)
                             : numeric::AccumByScaleBlock(prod, sc, block);
      float clamped = numeric::ClampNanInf(acc);
      if (numeric::BitsOf(clamped) != numeric::BitsOf(acc)) ++clamp_pending;
      // 这一段的部分和加进累加寄存器。段间顺序加，每加一次也 Clamp。
      ksplit[j] = k_first ? clamped
                          : numeric::ClampNanInf(ksplit[j] + clamped);
    }
    // 一列还没算完就不往下走：结果地址只按 n 递增，中间段写出去会把前一段盖掉。
    if (!k_last) return;

    // 几个专家合并成一份：这一列的部分和乘上这个专家的权重，进 EP 累加寄存器。
    if (cfg.ep_reduce) {
      if (ep_first) {
        ep_acc.assign(n, 0.0f);
      } else {
        LOGCHECK(ep_acc.size() == n, "MatrixExe: 同一列的两个专家 N 不一样长。");
      }
      for (uint64_t j = 0; j < n; ++j) {
        float w = numeric::ClampNanInf(ksplit[j] * w_ep);
        ep_acc[j] = ep_first ? w : numeric::ClampNanInf(ep_acc[j] + w);
      }
      ksplit.clear();
      if (!ep_last) return;
    }

    Result r;
    r.cfg = cfg;
    r.addr = out_addr;
    r.out.reserve(n);
    std::vector<float> const& src = cfg.ep_reduce ? ep_acc : ksplit;
    for (uint64_t j = 0; j < n; ++j) {
      // 输出按 DTYPE_C 转：FP32 全精度写回，或 BF16 原位舍入截断。
      float v = src[j];
      if (cfg.out_bf16) v = numeric::FromBf16(numeric::ToBf16(v));
      r.out.push_back(v);
    }
    ksplit.clear();
    ep_acc.clear();
    r.ready_at = CycleNow() + kMuLaneDepth;
    pipe.push_back(r);
    ++prim_pending;
  }

  // 算完的那一笔。取走之后流水线上就空出这一格。
  struct Result {
    MuTaskCfg cfg;
    std::vector<float> out;
    uint64_t addr = 0;      // 这一个 tile 写回哪，由 AGU 的 OutAddr 给
    uint64_t ready_at = 0;
  };

  bool HasResult() const {
    return !pipe.empty() && pipe.front().ready_at <= last_cycle;
  }
  Result TakeResult() {
    Result r = pipe.front();
    pipe.pop_front();
    return r;
  }

  uint64_t Prims() const { return prims.Get(); }
  uint64_t NanClamps() const { return nan_clamps.Get(); }

  bool Quiescent() const override {
    return pipe.empty() && ksplit.empty() && ep_acc.empty();
  }

 protected:
  void Step() override {
    last_cycle = CycleNow();
    prims = prim_pending;
    busy = pipe.size();
    nan_clamps = clamp_pending;
    TracePerCycle("inflight", pipe.size());
    TracePerCycle("prims", prim_pending);
  }

 private:
  std::deque<Result> pipe;
  // 当前这一列走到一半时的部分和。一列的一个专家算完就清空。
  std::vector<float> ksplit;
  // 这一列几个专家的加权和。合并成一份的那一档用，一列算完就清空。
  std::vector<float> ep_acc;
  uint64_t last_cycle = 0;
  uint64_t prim_pending = 0, clamp_pending = 0;

  Logic64 prims, busy, nan_clamps;
};

}  // namespace bach
}  // namespace latch

#endif
