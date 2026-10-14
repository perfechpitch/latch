#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_AGCU_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_AGCU_
#include "bach/ip/chip/core/dte/dte_types.h"
#include <algorithm>
#include <array>
#include <limits>
namespace latch {
namespace bach {
struct DteWindows {
  uint64_t cm_base = 0, cm_size = 2 * 1024 * 1024;
  uint64_t mm_base = 0x10000000, mm_size = 36 * 1024 * 1024;
  uint64_t topk_base = 0x20000000, topk_size = 4096;
  uint64_t header_base = 0x30000000, header_size = 512;
};
// 文档绝对地址（pcode 产物）那一套窗口，与上面的模型窗口同时认。布局与模型窗口
// 一一对应：Core Mem 1 MB 之后紧跟 32 KB scale（0x4850_0000），Matrix Mem 32 MB
// 数据之后紧跟 4 MB SF（0x4AC0_0000），topK_table 在 MU_DSA + 0x3000，
// header_table 在 DTE_DSA + 0x3000。
constexpr DteWindows kDocDteWindows{0x48400000, 2 * 1024 * 1024,
                                    0x48C00000, 36 * 1024 * 1024,
                                    0x48033000, 4096,
                                    0x4802B000, 512};
// Share Mem 的文档地址。CFG_SM_W_ADDR 可以写这一套，也可以写窗口内偏移。
constexpr uint64_t kDocSmemBase = 0x48300000;
constexpr uint64_t kDocSmemBytes = 32 * 1024;
enum DteError : uint64_t {
  kDteNoError = 0,
  kDteEndpointError = 1,
  kDteWindowError = 2,
  kDteAlignmentError = 3,
  kDteLengthError = 4,
  kDteOverflowError = 5,
  kDteRouteError = 6,
  kDteResponseError = 7,
  kDtePacketError = 8,
};
class Agcu {
public:
  explicit Agcu(DteWindows const &ranges = {}) : windows(ranges) {}
  static SegEndpoint Decode(uint64_t addr) { return Agcu{}.Endpoint(addr); }
  // 这个地址是不是文档绝对地址那一套（pcode 产物）。
  static bool IsDocAddr(uint64_t addr) {
    return EndpointIn(kDocDteWindows, addr) != SegEndpoint::kInvalid;
  }
  static uint64_t SmemOffset(uint64_t addr) {
    return In(addr, kDocSmemBase, kDocSmemBytes) ? addr - kDocSmemBase : addr;
  }
  SegEndpoint Endpoint(uint64_t addr) const {
    SegEndpoint ep = EndpointIn(windows, addr);
    return ep != SegEndpoint::kInvalid ? ep : EndpointIn(kDocDteWindows, addr);
  }
  uint64_t Offset(uint64_t addr, SegEndpoint endpoint) const {
    DteWindows const &w =
        EndpointIn(windows, addr) != SegEndpoint::kInvalid ? windows
                                                           : kDocDteWindows;
    return OffsetIn(w, addr, endpoint);
  }
  static SegEndpoint EndpointIn(DteWindows const &windows, uint64_t addr) {
    if (In(addr, windows.cm_base, windows.cm_size))
      return ((addr - windows.cm_base) & (1ull << 20)) ? SegEndpoint::kScale
                                                       : SegEndpoint::kCmem;
    if (In(addr, windows.mm_base, windows.mm_size))
      return SegEndpoint::kMmem;
    if (In(addr, windows.topk_base, windows.topk_size))
      return SegEndpoint::kTopk;
    if (In(addr, windows.header_base, windows.header_size))
      return SegEndpoint::kHeader;
    return SegEndpoint::kInvalid;
  }
  static uint64_t OffsetIn(DteWindows const &windows, uint64_t addr,
                           SegEndpoint endpoint) {
    switch (endpoint) {
    case SegEndpoint::kCmem:
      return addr - windows.cm_base;
    case SegEndpoint::kScale:
      return addr - windows.cm_base - (1ull << 20);
    case SegEndpoint::kMmem:
      return addr - windows.mm_base;
    case SegEndpoint::kTopk:
      return addr - windows.topk_base;
    case SegEndpoint::kHeader:
      return addr - windows.header_base;
    default:
      return addr;
    }
  }
  uint64_t Limit(SegEndpoint ep) const {
    switch (ep) {
    case SegEndpoint::kCmem:
      return std::min<uint64_t>(windows.cm_size, 1024 * 1024);
    case SegEndpoint::kScale:
      return windows.cm_size > 1024 * 1024
                 ? std::min<uint64_t>(windows.cm_size - 1024 * 1024, 32 * 1024)
                 : 0;
    case SegEndpoint::kMmem:
      return windows.mm_size;
    case SegEndpoint::kTopk:
      return windows.topk_size;
    case SegEndpoint::kHeader:
      return windows.header_size;
    default:
      return 0;
    }
  }
  void ExpandOut(Segment &s, uint64_t sid, Route route) const {
    bool source = !IsInbound(route);
    bool destination = IsInbound(route) || route == Route::kMmToCm;
    s.src_kind = source ? Endpoint(s.src) : SegEndpoint::kInvalid;
    s.dst_kind = destination ? Endpoint(s.dst) : SegEndpoint::kInvalid;
    if (source)
      s.src_addr = Offset(s.src, s.src_kind) +
                   (route == Route::kMmToCm ? 0 : sid * s.stride);
    if (destination)
      s.dst_addr = Offset(s.dst, s.dst_kind) + sid * s.stride;
  }
  uint64_t Validate(Descriptor &d) const {
    if (uint64_t(d.route) > 4 || d.stream_id > 15 || d.vc > 3 ||
        d.task_id >= 64 || d.user_id > 65535 || d.path_id > 255)
      return kDteRouteError;
    if (d.seg[0].len != 32)
      return kDteLengthError;
    uint64_t payload = 0;
    for (uint64_t i = 0; i < 4; ++i) {
      auto &s = d.seg[i];
      if (!s.valid)
        continue;
      if (i == 0 && IsInbound(d.route) && d.hw_header_op)
        continue;
      ExpandOut(s, d.stream_id, d.route);
      for (uint64_t end = 0; end < 2; ++end) {
        if ((end == 0 && IsInbound(d.route)) ||
            (end == 1 && !IsInbound(d.route) && d.route != Route::kMmToCm))
          continue;
        uint64_t base = end == 0 ? s.src : s.dst;
        uint64_t stride = end == 0 && d.route == Route::kMmToCm ? 0 : s.stride;
        if (base > 0xffffffffull ||
            (d.stream_id && stride > (0xffffffffull - base) / d.stream_id))
          return kDteOverflowError;
        uint64_t full = base + d.stream_id * stride;
        SegEndpoint ep = end == 0 ? s.src_kind : s.dst_kind;
        uint64_t offset = end == 0 ? s.src_addr : s.dst_addr;
        if (full > 0xffffffffull)
          return kDteOverflowError;
        if (ep == SegEndpoint::kInvalid || Endpoint(full) != ep)
          return kDteEndpointError;
        if (ep == SegEndpoint::kHeader) {
          if (i != 0 || offset >= Limit(ep))
            return kDteWindowError;
        } else {
          if (offset > Limit(ep) || s.len > Limit(ep) - offset)
            return kDteWindowError;
          if (offset & 7 || s.len & 7)
            return kDteAlignmentError;
          // 表项按 256 B 对齐。段长可以短于一项：pcode 的 topK 段是 56 B。
          if (ep == SegEndpoint::kTopk && ((offset & 255) || s.len > 256))
            return kDteAlignmentError;
        }
      }
      if (i)
        payload += s.len;
      if (d.route == Route::kMmToCm && s.dst_kind != SegEndpoint::kCmem &&
          s.dst_kind != SegEndpoint::kScale &&
          s.dst_kind != SegEndpoint::kHeader)
        return kDteRouteError;
    }
    if (d.route != Route::kMmToCm && payload > kMaxTaskBytes)
      return kDteLengthError;
    return 0;
  }

private:
  static bool In(uint64_t addr, uint64_t base, uint64_t size) {
    return addr >= base && addr - base < size;
  }
  DteWindows windows;
};
} // namespace bach
} // namespace latch
#endif
