#ifndef _LATCH_BACH_IP_CHIP_CORE_MU_GEN_EP_INFO_
#define _LATCH_BACH_IP_CHIP_CORE_MU_GEN_EP_INFO_

// gen_ep_info：把 topK 里的激活专家信息读出来，供算 weight 访存地址与合并权重用。
//
// topK 存的是组内序号（local index），weight 访存地址直接用
// local_ep_index * b_expert_stride 算，中间没有 global→local 的翻译：组内序号是
// 上游在把 topK 广播进本 EP Group 之前就压好的，MU 只当本地序号用。
//
// topK_ep_table 能存 1024 份，每份 256 B，由 DTE 搬运时经专用数据线写进来，
// 下标取 topK 段地址的端内偏移：计算 core 是 stream_id，MU 计算时同时读
// topK_ep_table[stream_id]（本地）、token（Core Mem）与 weight（Matrix Mem），不再
// 有任务启动时的前置串行读；B core 是 user_id，收下 token 时按它存、广播时按它取。
//
// token 数据与 topK 分开存：FC1 / FC3 只需要 ids，FC2 还要 weights。
// router_expert_count 为 0 时整个 topK 那一组寄存器都忽略。

#include <array>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/common/numeric/formats.h"
#include "bach/ip/chip/core/ts/ts_types.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 每份 topK 上限 256 B，每项 {local_ep_index 2 B, weight 4 B}。
constexpr uint64_t kTopkBytesPerStream = 256;
// topK_ep_table 能存几份。下标越界就是上游给的 user_id 超出了这张表。
constexpr uint64_t kTopkTableNum = 1024;
constexpr uint64_t kTopkEntryBytes = 6;

struct TopkEntry {
  uint64_t local_ep_index = 0;   // 组内序号（local index），上游广播前已压好
  float weight = 0.0f;
};

class GenEpInfo : public BachModule {
 public:
  GenEpInfo(ClockPtr clock, const std::string& name, uint64_t parent = 0,
            bool tick = true)
      : BachModule(clock, name, parent, tick) {}

  // DTE 搬运时经专用数据线把这一份写进来（256 B）。写进表里第 idx 份，MU 计算时读。
  void WriteTopk(uint64_t idx, std::vector<uint8_t> bytes) {
    LOGCHECK(idx < kTopkTableNum, "GenEpInfo: topK 表下标越界。");
    table[idx] = std::move(bytes);
    dirty[idx] = true;
  }

  // 出核那一笔把这份原始字节读出来、附回要发的包（B core 广播时把 token 的 topK
  // 原样转出去）。不解析，直接给表里存的字节。下标由调用方按 topK 段的端内偏移给。
  std::vector<uint8_t> const& TopkBytes(uint64_t idx) const {
    LOGCHECK(idx < kTopkTableNum, "GenEpInfo: topK 表下标越界。");
    return table[idx];
  }

  // MU 计算时读这一份：dirty 时用 Parse 解一次，之后走缓存。
  std::vector<TopkEntry> const& Topk(uint64_t stream_id, uint64_t count) {
    LOGCHECK(stream_id < kTopkTableNum, "GenEpInfo: topK 表下标越界。");
    if (dirty[stream_id]) {
      parsed[stream_id] = Parse(table[stream_id], count);
      dirty[stream_id] = false;
    }
    return parsed[stream_id];
  }

  // 从 DTE 写进来的字节解成 topK 表。
  static std::vector<TopkEntry> Parse(std::vector<uint8_t> const& bytes,
                                      uint64_t count) {
    std::vector<TopkEntry> out;
    for (uint64_t i = 0; i < count; ++i) {
      uint64_t off = i * kTopkEntryBytes;
      if (off + kTopkEntryBytes > bytes.size()) break;
      TopkEntry e;
      e.local_ep_index = uint64_t(bytes[off]) | (uint64_t(bytes[off + 1]) << 8);
      uint32_t w = 0;
      for (int k = 0; k < 4; ++k) {
        w |= uint32_t(bytes[off + 2 + k]) << (8 * k);
      }
      e.weight = numeric::FloatOf(w);
      out.push_back(e);
    }
    return out;
  }

 protected:
  void Step() override {}

 private:
  // DTE 写进来的 topK 原始字节，每个下标一份；dirty 时才 Parse 成表。
  std::array<std::vector<uint8_t>, kTopkTableNum> table;
  std::array<std::vector<TopkEntry>, kTopkTableNum> parsed;
  std::array<bool, kTopkTableNum> dirty{};
};

}  // namespace bach
}  // namespace latch

#endif
