#ifndef _LATCH_BACH_IP_CHIP_CORE_MU_MATRIX_EXE_
#define _LATCH_BACH_IP_CHIP_CORE_MU_MATRIX_EXE_

// matrix exe：64 个物理 lane，分布在 32 个站点，IP drain 10T，结果转换 1T。
//
// 物理阵列是 1×K128×N64（输出带宽 256 B）。
//
// bit 级累加顺序是这一层的核心：CSA 树按 scale block 分组累加，块内先把乘积
// 加完再乘 scale，比逐个乘 scale 再加少一轮舍入。MXFP8 × MXFP8 时 token 与权重
// 各有一组 scale，块内部分和乘两者之积。参考实现必须用同一个顺序：浮点加法不
// 满足结合律，顺序是结果的一部分，不是实现细节。
//
// vlane 机制：把 MAC 按 vlane 分组，在 CSA 加法树中间插旁路 MUX，单 lane 同时
// 输出多个结果。vlane 有 1 和 2 两种。vlane=2 时每个 lane 在第 64 个输入处断
// 开，算两个半长的点积，原语就是 1×K64×N128，块的划分跟着减半。
//
// NaN/Inf 正常传播；NV/OF/UF/NX 按任务累计，不阻塞算术流水。
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
#include "bach/common/numeric/fp32.h"
#include "bach/common/numeric/mx.h"
#include "bach/ip/chip/core/mu/regfile.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 64 个物理 lane，分布在 32 个站点。
constexpr uint64_t kMuLaneNum = 64;
constexpr uint64_t kMuStationNum = 32;
// 供应 IP 无阻塞 drain 为 10T，RAW_C 转 FP32 为 1T；队列等待另计。
constexpr uint64_t kMuLaneDepth = 11;

class MatrixExe : public BachModule {
 public:
  MatrixExe(ClockPtr clock, const std::string& name, uint64_t parent = 0,
            bool tick = true)
      : BachModule(clock, name, parent, tick),
        prims(clock),
        busy(clock) {}

  // 一次原语：token 是 K 个元素，weight 是 K × N，输出 N 个。
  //
  // 这里就是数据金标准：参考实现按同一个 AccumByScaleBlock 算，逐元素比对不留
  // 容差。算完排进流水线，kMuLaneDepth 拍后出来。
  //
  // wscale 是权重的 scale，按列排，一列 K / block 个；给空时权重按 scale 1 算。
  //
  // k_first / k_last 说这一段在这一列的哪个位置，ep_first / ep_last 说这个专家
  // 在这一列的几个专家里的哪个位置。一列只占一段、只有一个专家时四个都是真，与
  // 不分块一样。w_ep 是这个专家的 topK 权重，只在几个专家合并成一份时用。
  // 返回是否产出完整输出 frame。Prims 始终统计接收的每条 primitive，
  // K 分块及专家归约的中间项也各计一次。
  bool Issue(MuTaskCfg const& cfg, std::vector<uint8_t> const& token,
             std::vector<uint8_t> const& weight,
             std::vector<uint8_t> const& scale,
             std::vector<uint8_t> const& wscale = {}, uint64_t out_addr = 0,
             bool k_first = true, bool k_last = true, bool ep_first = true,
             bool ep_last = true, float w_ep = 1.0f, uint64_t task = ~0ull) {
    ++prim_pending;
    uint64_t k = cfg.PrimK();
    uint64_t n = cfg.PrimN();
    LOGCHECK(cfg.primitive_type == 0 || cfg.primitive_type == 1,
             "MatrixExe: primitive_type 只有 0 与 1 两档。");
    uint64_t a_block = numeric::ScaleBlockOf(cfg.a_dtype);
    uint64_t b_block = numeric::ScaleBlockOf(cfg.b_dtype);
    uint64_t block = !a_block ? b_block : !b_block ? a_block : std::min(a_block, b_block);
    auto a = numeric::Decode(cfg.a_dtype, token, k);
    auto a_scales = numeric::DecodeScale(cfg.a_dtype, scale, a_block ? k / a_block : 0);
    uint64_t b_scales_per_column = b_block ? k / b_block : 0;
    auto b_scales = numeric::DecodeScale(cfg.b_dtype, wscale, n * b_scales_per_column);
    auto add = [&](float x, float y) {
      auto result = numeric::Fp32Add(numeric::BitsOf(x), numeric::BitsOf(y));
      fp_flags |= result.flags; return result.Value();
    };
    auto mul = [&](float x, float y) {
      auto result = numeric::Fp32Mul(numeric::BitsOf(x), numeric::BitsOf(y));
      fp_flags |= result.flags; return result.Value();
    };
    if (k_first) {
      LOGCHECK(ksplit.empty(), "MatrixExe: unfinished K context");
      ksplit.assign(n, 0.0f);
    } else LOGCHECK(ksplit.size() == n, "MatrixExe: mismatched K context");
    uint64_t column_bytes = k * numeric::ElemBitsOf(cfg.b_dtype) / 8;
    for (uint64_t j = 0; j < n; ++j) {
      std::vector<uint8_t> column(column_bytes, 0);
      for (uint64_t i = 0; i < column_bytes && j * column_bytes + i < weight.size(); ++i)
        column[i] = weight[j * column_bytes + i];
      auto b = numeric::Decode(cfg.b_dtype, column, k);
      float acc = 0.0f;
      uint64_t group = block ? block : k;
      for (uint64_t start = 0; start < k; start += group) {
        float partial = 0.0f;
        for (uint64_t i = start; i < std::min(k, start + group); ++i)
          partial = add(partial, mul(a[i], b[i]));
        if (block) {
          float sa = a_block ? a_scales[start / a_block] : 1.0f;
          float sb = b_block ? b_scales[j * b_scales_per_column + start / b_block] : 1.0f;
          partial = mul(partial, mul(sa, sb));
        }
        acc = block ? add(acc, partial) : partial;
      }
      ksplit[j] = k_first ? acc : add(ksplit[j], acc);
    }
    // 一列还没算完就不往下走：结果地址只按 n 递增，中间段写出去会把前一段盖掉。
    if (!k_last) return false;

    // 几个专家合并成一份：这一列的部分和乘上这个专家的权重，进 EP 累加寄存器。
    if (cfg.ep_reduce) {
      if (ep_first) {
        ep_acc.assign(n, 0.0f);
      } else {
        LOGCHECK(ep_acc.size() == n, "MatrixExe: 同一列的两个专家 N 不一样长。");
      }
      for (uint64_t j = 0; j < n; ++j) {
        float w = mul(ksplit[j], w_ep);
        ep_acc[j] = ep_first ? w : add(ep_acc[j], w);
      }
      ksplit.clear();
      if (!ep_last) return false;
    }

    Result r;
    r.cfg = cfg;
    r.task = task;
    r.addr = out_addr;
    r.out.reserve(n);
    std::vector<float> const& src = cfg.ep_reduce ? ep_acc : ksplit;
    for (uint64_t j = 0; j < n; ++j) {
      // 输出按 DTYPE_C 转：FP32 全精度写回，或 BF16 原位舍入截断。
      float v = src[j];
      if (cfg.out_bf16) {
        auto converted = numeric::Fp32ToBf16(numeric::BitsOf(v));
        fp_flags |= converted.flags;
        v = numeric::FromBf16(uint16_t(converted.bits));
      }
      r.out.push_back(v);
    }
    ksplit.clear();
    ep_acc.clear();
    r.ready_at = CycleNow() + kMuLaneDepth;
    pipe.push_back(r);
    return true;
  }

  // 算完的那一笔。取走之后流水线上就空出这一格。
  struct Result {
    MuTaskCfg cfg;
    std::vector<float> out;
    uint64_t addr = 0;      // 这一个 tile 写回哪，由 AGU 的 OutAddr 给
    uint64_t ready_at = 0;
    uint64_t task = ~0ull;
  };

  bool HasResult() const {
    return !pipe.empty() && pipe.front().ready_at <= last_cycle;
  }
  Result const &NextResult() const { return pipe.front(); }
  bool HasRoom() const { return pipe.size() < kMuLaneDepth + 2; }
  Result TakeResult() {
    Result r = pipe.front();
    pipe.pop_front();
    return r;
  }

  void ResetFlags() { fp_flags = 0; }
  void Cancel() { pipe.clear(); ksplit.clear(); ep_acc.clear(); }
  uint32_t Flags() const { return fp_flags; }
  uint64_t Prims() const { return prims.Get(); }

  bool Quiescent() const override {
    return pipe.empty() && ksplit.empty() && ep_acc.empty();
  }

 protected:
  void Step() override {
    last_cycle = CycleNow();
    prims = prim_pending;
    busy = pipe.size();
    TracePerCycle("inflight", pipe.size());
    TracePerCycle("prims", prim_pending);
  }

 private:
  uint32_t fp_flags = 0;
  std::deque<Result> pipe;
  // 当前这一列走到一半时的部分和。一列的一个专家算完就清空。
  std::vector<float> ksplit;
  // 这一列几个专家的加权和。合并成一份的那一档用，一列算完就清空。
  std::vector<float> ep_acc;
  uint64_t last_cycle = 0;
  uint64_t prim_pending = 0;

  Logic64 prims, busy;
};

}  // namespace bach
}  // namespace latch

#endif
