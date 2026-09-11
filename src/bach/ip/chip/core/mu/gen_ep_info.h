#ifndef _LATCH_BACH_IP_CHIP_CORE_MU_GEN_EP_INFO_
#define _LATCH_BACH_IP_CHIP_CORE_MU_GEN_EP_INFO_

// gen_ep_info：把 topK 里的全局专家号翻成组内序号。
//
// topK 存的是 global index，算 weight 访存地址要的是 local index，中间隔着
// local_ep_table，它记录当前 EP Group 内有哪些专家、各自在组内第几个。
//
// topK_ep_table 是 MU 自己从 Core Mem 载入的一份副本：DTE 进核时把 topK 写进
// Core Mem 的 topK 区，MU 在 task 启动时按 topk_base + stream_id × 256 B 读进来。
// 载入未完成时这一级等着，不会读到半新半旧的一组专家。
//
// token 数据与 topK 分开存：FC1 / FC3 只需要 ids，FC2 还要 weights。
// router_expert_count 为 0 时整个 topK 那一组寄存器都忽略。

#include <map>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/common/numeric/formats.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 每 stream 的 topK 区上限 256 B，每项 {expert_id 2 B, weight 4 B}。
constexpr uint64_t kTopkBytesPerStream = 256;
constexpr uint64_t kTopkEntryBytes = 6;

struct TopkEntry {
  uint64_t expert_id = 0;   // global index
  float weight = 0.0f;
};

class GenEpInfo : public BachModule {
 public:
  GenEpInfo(ClockPtr clock, const std::string& name, uint64_t parent = 0,
            bool tick = true)
      : BachModule(clock, name, parent, tick), lookups(clock), misses(clock) {}

  // 编译侧读入：本 EP Group 内有哪些专家、各自组内第几个。
  void SetLocalEpTable(std::vector<uint64_t> const& experts) {
    local_ep.clear();
    for (uint64_t i = 0; i < experts.size(); ++i) local_ep[experts[i]] = i;
  }

  // task 启动时从 Core Mem 载入这一份。载入完成前 Ready 是 false。
  void LoadTopk(std::vector<TopkEntry> entries) {
    topk = std::move(entries);
    loaded = true;
  }
  void Invalidate() { loaded = false; }
  bool Ready() const { return loaded; }

  // 全局专家号翻成组内序号。不在本组里就返回 false，那说明 topK 与
  // local_ep_table 对不上，是配置错误，不是正常工作点。
  bool ToLocal(uint64_t global, uint64_t* local) {
    ++lookup_pending;
    auto it = local_ep.find(global);
    if (it == local_ep.end()) {
      ++miss_pending;
      return false;
    }
    *local = it->second;
    return true;
  }

  std::vector<TopkEntry> const& Topk() const { return topk; }
  uint64_t Lookups() const { return lookups.Get(); }
  uint64_t Misses() const { return misses.Get(); }

  // 从 Core Mem 读回来的字节解成 topK 表。
  static std::vector<TopkEntry> Parse(std::vector<uint8_t> const& bytes,
                                      uint64_t count) {
    std::vector<TopkEntry> out;
    for (uint64_t i = 0; i < count; ++i) {
      uint64_t off = i * kTopkEntryBytes;
      if (off + kTopkEntryBytes > bytes.size()) break;
      TopkEntry e;
      e.expert_id = uint64_t(bytes[off]) | (uint64_t(bytes[off + 1]) << 8);
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
  void Step() override {
    lookups = lookup_pending;
    misses = miss_pending;
  }

 private:
  std::map<uint64_t, uint64_t> local_ep;
  std::vector<TopkEntry> topk;
  bool loaded = false;
  uint64_t lookup_pending = 0, miss_pending = 0;

  Logic64 lookups, misses;
};

}  // namespace bach
}  // namespace latch

#endif
