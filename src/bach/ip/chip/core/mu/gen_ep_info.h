#ifndef _LATCH_BACH_IP_CHIP_CORE_MU_GEN_EP_INFO_
#define _LATCH_BACH_IP_CHIP_CORE_MU_GEN_EP_INFO_

// gen_ep_info：把 topK 里的激活专家信息读出来，供算 weight
// 访存地址与合并权重用。
//
// topK 存的是组内序号（local index），weight 访存地址直接用
// local_ep_index * b_expert_stride 算，中间没有 global→local 的翻译：组内序号是
// 上游在把 topK 广播进本 EP Group 之前就压好的，MU 只当本地序号用。
//
// topK_ep_table 为 16 个 SID 各保存一份 256B 紧凑项，前 2B 为本地专家数。
// DTE 整项原子读写；任务 dispatch 时按 SID 取得专家快照。

#include "bach/ip/chip/core/ports.h"
#include <array>
#include <deque>
#include <string>
#include <vector>

#include "bach/common/numeric/formats.h"
#include "bach/ip/chip/core/ts/ts_types.h"
#include "bach/ip/module_base.h"
#include "base/log.h"

namespace latch {
namespace bach {

// 每份 topK 为 256 B，前 2 B 为本地专家数，每项 {local_ep_index 2 B, weight 4 B}。
constexpr uint64_t kTopkBytesPerStream = 256;
// topK_ep_table 能存几份。下标越界就是上游给的 user_id 超出了这张表。
constexpr uint64_t kTopkTableNum = 16;
constexpr uint64_t kTopkEntryBytes = 6;
constexpr uint64_t kTopkCountBytes = 2;
constexpr uint64_t kTopkMaxExperts = 32;

struct TopkEntry {
  uint64_t local_ep_index = 0; // 组内序号（local index），上游广播前已压好
  float weight = 0.0f;
};

class GenEpInfo : public BachModule {
public:
  GenEpInfo(ClockPtr clock, const std::string &name, uint64_t parent = 0,
            bool tick = true)
      : BachModule(clock, name, parent, tick),
        dte_rd(std::make_shared<MemPort>(clock)),
        dte_wr(std::make_shared<MemPort>(clock)) {}

  std::shared_ptr<MemPort> DteReadPtr() const { return dte_rd; }
  std::shared_ptr<MemPort> DteWritePtr() const { return dte_wr; }

  // DTE 搬运时经专用数据线把这一份写进来（256 B）。写进表里第 idx 份，MU
  // 计算时读。
  void WriteTopk(uint64_t idx, std::vector<uint8_t> bytes) {
    LOGCHECK(idx < kTopkTableNum, "GenEpInfo: topK 表下标越界。");
    LOGCHECK(bytes.size() <= kTopkBytesPerStream, "GenEpInfo: topK exceeds 256B");
    bytes.resize(kTopkBytesPerStream, 0);
    table[idx] = std::move(bytes);
    dirty[idx] = true;
  }

  // 出核那一笔把这份原始字节读出来、附回要发的包（B core 广播时把 token 的 topK
  // 原样转出去）。不解析，直接给表里存的字节。下标由调用方按 topK
  // 段的端内偏移给。
  std::vector<uint8_t> const &TopkBytes(uint64_t idx) const {
    LOGCHECK(idx < kTopkTableNum, "GenEpInfo: topK 表下标越界。");
    return table[idx];
  }

  // MU 计算时读这一份：dirty 时用 Parse 解一次，之后走缓存。
  std::vector<TopkEntry> const &Topk(uint64_t stream_id, uint64_t count) {
    LOGCHECK(stream_id < kTopkTableNum, "GenEpInfo: topK 表下标越界。");
    if (dirty[stream_id] || parse_count[stream_id] != count) {
      parsed[stream_id] = Parse(table[stream_id], count);
      dirty[stream_id] = false;
      parse_count[stream_id] = count;
    }
    return parsed[stream_id];
  }

  // 从 DTE 写进来的字节解成 topK 表。
  static std::vector<TopkEntry> Parse(std::vector<uint8_t> const &bytes,
                                      uint64_t count) {
    std::vector<TopkEntry> out;
    uint64_t local_count = CountOf(bytes);
    for (uint64_t i = 0; i < count && i < local_count && i < kTopkMaxExperts; ++i) {
      uint64_t off = kTopkCountBytes + i * kTopkEntryBytes;
      if (off + kTopkEntryBytes > bytes.size())
        break;
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
  static uint64_t CountOf(std::vector<uint8_t> const &bytes) {
    return bytes.size() < 2 ? 0 : uint64_t(bytes[0]) | (uint64_t(bytes[1]) << 8);
  }

protected:
  void Step() override {
    ServeMemory(*dte_rd, false, rd_seq);
    ServeMemory(*dte_wr, true, wr_seq);
  }
  void ServeMemory(MemPort &port, bool write, uint64_t &seen) {
    MemReqView req = ReadMemReq(port);
    bool response = req.valid && req.seq != seen;
    ByteBlockPtr data;
    uint64_t error = 0;
    if (response) {
      seen = req.seq;
      if (req.topk_sel != 1 || !req.pair || req.lane_mask != 3 ||
          req.hw_addr0 != req.addr / 256 || req.hw_addr1 != req.addr / 256 ||
          req.addr >= 4096 || req.bytes != 256 || (req.addr & 255))
        error = 1;
      else {
        uint64_t index = req.addr / 256, off = req.addr % 256;
        if (table[index].size() < 256)
          table[index].resize(256);
        if (write) {
          for (uint64_t k = 0;
               k < req.bytes && req.wdata && k < req.wdata->size(); ++k)
            table[index][off + k] = (*req.wdata)[k];
          dirty[index] = true;
        } else
          data = std::make_shared<ByteBlock>(table[index].begin() + off,
                                             table[index].begin() + off +
                                                 req.bytes);
      }
    }
    port.DriveSlave(true, response, data);
    port.rsp_error = error;
  }

private:
  std::shared_ptr<MemPort> dte_rd, dte_wr;
  uint64_t rd_seq = 0, wr_seq = 0;
  // DTE 写进来的 topK 原始字节，每个下标一份；dirty 时才 Parse 成表。
  std::array<std::vector<uint8_t>, kTopkTableNum> table;
  std::array<std::vector<TopkEntry>, kTopkTableNum> parsed;
  std::array<bool, kTopkTableNum> dirty{};
  std::array<uint64_t, kTopkTableNum> parse_count{};
};

} // namespace bach
} // namespace latch

#endif
