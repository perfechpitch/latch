#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_AGCU_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_AGCU_

// AGCU：地址生成。
//
// 软件只配基址，偏移由硬件用 stream_id 算出来。stream_id 是 TS 建 stream 表项
// 时定的，随任务一起给到 DTE，软件不需要知道这个 token 落在 Core Mem 的哪一片。
//
//   PhyAddr = base_addr + stream_id × stride + offset
//
// base_addr 只对 Core Mem 有效：Matrix Mem 的地址全由软件管，配任务时 src_addr
// 与 dst_addr 就是最终物理地址，硬件不再叠 stream_id × stride。理由是 Matrix Mem
// 放的是模型 weight 与按 pattern 排好序送来的 token，位置软件自己算准；Core Mem
// 按 stream 切成 16 片，谁占哪片由 TS 定，软件配的时候还不知道。
//
// 数据布局仅支持连续一维搬运，当前不支持 stride（这里说的 stride 是数据内部的
// 跨步，与上面按 stream 分片的那个 stride 不是一回事）。

#include <cstdint>

#include "bach/ip/chip/core/dte/dte_types.h"

namespace latch {
namespace bach {

// Core Mem 的四类分区跨度，取自 cmem_part。
struct CmemLayout {
  uint64_t stream_base = 0;
  uint64_t stream_stride = 64 * 1024;
  uint64_t header_base = 0;
  uint64_t header_stride = 18;      // {core_mask 2 B, sw_header 16 B}
  uint64_t scale_base = 0;
  uint64_t scale_stride = 2048;
  uint64_t topk_base = 0;
  uint64_t topk_stride = 256;       // 每 stream 上限 256 B
};

class Agcu {
 public:
  explicit Agcu(CmemLayout const& layout) : cm(layout) {}

  // Core Mem 一侧加 stream 偏移，Matrix Mem 一侧不加。
  //
  // off 是软件配的段内偏移。Core Mem 那一侧软件只配它，落在哪一片由硬件按
  // stream_id 算；Matrix Mem 那一侧软件配的就是最终物理地址。
  uint64_t DataAddr(uint64_t stream_id, bool core_mem, uint64_t off) const {
    if (!core_mem) return off;
    return cm.stream_base + stream_id * cm.stream_stride + off;
  }
  uint64_t HeaderAddr(uint64_t stream_id) const {
    return cm.header_base + stream_id * cm.header_stride;
  }
  uint64_t ScaleAddr(uint64_t stream_id) const {
    return cm.scale_base + stream_id * cm.scale_stride;
  }
  uint64_t TopkAddr(uint64_t stream_id) const {
    return cm.topk_base + stream_id * cm.topk_stride;
  }

  // 两项搬运长度硬件自己算，不用软件配。
  // scale 是 data_len / 32：32 个元素共用一个 scale。
  static uint64_t ScaleBytes(uint64_t data_len) { return data_len / 32; }
  // topK 是 router_ep_count × 6 B，每项 {expert_id 2 B, weight 4 B}。
  static uint64_t TopkBytes(uint64_t ep_count) { return ep_count * 6; }

  CmemLayout const& Layout() const { return cm; }

 private:
  CmemLayout cm;
};

}  // namespace bach
}  // namespace latch

#endif
