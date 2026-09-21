#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_AGCU_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_AGCU_

// AGCU：地址生成与端点译码。
//
// 对齐飞书《DTE DSA》后，地址公式逐段位算：
//
//   stream_start_i = CFG_ADDRi + SID × CFG_STRIDEi
//
// stride 配 0 退化为纯物理地址（Mmem 段软件给物理地址）；route 100（Mmem→Cmem）
// 源端不叠 stride。每段的端点（Mmem / Cmem / scale 旁带 / topK_table / header_table）
// 由该段地址所在的地址范围译码决定，不再由 route 决定。
//
// 端点地址范围是建模约定：飞书文档只说「由地址范围译码」，未给具体数值。这里取
// 地址高 4 bit 作端点 tag，低位为端内偏移：
//
//   0x0_______  Cmem 数据（低 20 bit 有效，1 MB）
//   0x1_______  Mmem 数据（低 26 bit 有效，36 MB）
//   0x2_______  Scale 旁带（低位给对应数据地址，len 是 scale 字节数）
//   0x3_______  topK_table（低位给 stream_id）
//   0x4_______  header_table（低位给 stream_id）
//
// 数据布局仅支持连续一维搬运，当前不支持 stride（这里说的 stride 是数据内部的
// 跨步，与上面按 stream 分片的那个 stride 不是一回事）。

#include <cstdint>

#include "bach/ip/chip/core/dte/dte_types.h"

namespace latch {
namespace bach {

// 端点 tag 的位宽与掩码（地址高 4 bit）定义在 dte_types.h（kEpShift/kEpMask/
// kEpDataMask），与 SegEndpoint 同处。

// Core Mem 的 stream 分片跨度。进核那一笔保持包驱动：包头带的落点是段内偏移，
// 收方还要叠自己的 stream 偏移才得到最终地址，这个跨度就取在这里（存疑：文档的
// 进核是配置驱动，本模型保持包驱动，见 04-dte 建模文档）。
struct CmemLayout {
  uint64_t stream_base = 0;
  uint64_t stream_stride = 64 * 1024;
};

class Agcu {
 public:
  explicit Agcu(CmemLayout const& layout) : cm(layout) {}

  // 端点译码：地址高 4 bit 选端点，其余位是端内偏移。
  static SegEndpoint Decode(uint64_t addr) {
    switch ((addr >> kEpShift) & kEpMask) {
      case 0x1: return SegEndpoint::kMmem;
      case 0x2: return SegEndpoint::kScale;
      case 0x3: return SegEndpoint::kTopk;
      case 0x4: return SegEndpoint::kHeader;
      default: return SegEndpoint::kCmem;
    }
  }

  // 出核段位：展开源/目的地址并译码端点。route 决定哪端是 Router（那一端的地址
  // 无意义），memory 端按 stream_start = CfgAddr + SID × stride 算；route 100
  // （Mmem→Cmem）源端不叠 stride。展开结果写回 seg 的 src_addr / dst_addr 与
  // src_kind / dst_kind，lane 直接用。
  void ExpandOut(Segment& s, uint64_t sid, Route route) const {
    s.src_kind = Decode(s.src);
    s.dst_kind = Decode(s.dst);
    bool src_mem = route == Route::kCmToRouter || route == Route::kMmToRouter ||
                   route == Route::kMmToCm;
    bool dst_mem = route == Route::kRouterToCm || route == Route::kRouterToMm ||
                   route == Route::kMmToCm;
    if (src_mem) {
      // MM→CM 源端是 Matrix Mem，软件给物理地址，不叠 stride。
      uint64_t stride = (route == Route::kMmToCm) ? 0 : s.stride;
      // 源端是 Core Mem（仅 CmToRouter）时叠 stream_base，Mmem 那一侧不给。
      uint64_t base = (route == Route::kCmToRouter) ? cm.stream_base : 0;
      s.src_addr = (s.src & kEpDataMask) + base + sid * stride;
    } else {
      s.src_addr = 0;
    }
    if (dst_mem) {
      // 目的端是 Core Mem（RouterToCm / MmToCm）时叠 stream_base，Mmem 不给。
      uint64_t base = (route == Route::kRouterToCm || route == Route::kMmToCm)
                          ? cm.stream_base
                          : 0;
      s.dst_addr = (s.dst & kEpDataMask) + base + sid * s.stride;
    } else {
      s.dst_addr = 0;
    }
  }

  // 进核那一路保持包驱动：落点由包头 dst_addr 给，落 Core Mem 时叠自己的 stream
  // 偏移，落 Matrix Mem 时直接用。返回最终地址。
  uint64_t InboundAddr(uint64_t sid, SegEndpoint kind, uint64_t off) const {
    if (kind == SegEndpoint::kMmem) return off;
    return cm.stream_base + sid * cm.stream_stride + off;
  }

  // Core Mem 一侧加 stream 偏移，Matrix Mem 一侧不加（保留给进核 Cmem 用）。
  uint64_t DataAddr(uint64_t stream_id, bool core_mem, uint64_t off) const {
    if (!core_mem) return off;
    return cm.stream_base + stream_id * cm.stream_stride + off;
  }

  CmemLayout const& Layout() const { return cm; }

 private:
  CmemLayout cm;
};

}  // namespace bach
}  // namespace latch

#endif
