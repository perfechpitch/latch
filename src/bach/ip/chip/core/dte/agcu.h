#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_AGCU_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_AGCU_

// AGCU：地址生成与端点译码。
//
//   stream_start_i = CFG_ADDRi + SID × CFG_STRIDEi
//
// 端点有两套写法。模型内部用地址高 4 bit 作 tag（latch 自己的 kernel 与用例）：
//
//   0x0_______  Cmem 数据（低 20 bit，1 MB）
//   0x1_______  Mmem 数据（低 26 bit，36 MB）
//   0x2_______  Scale 旁带
//   0x3_______  topK_table
//   0x4_______  header_table
//
// 文档绝对地址（pcode 产物）在叠过 SID × stride 之后换成 tag：
//
//   0x4840_0000 + o          Cmem 数据     → tag 0
//   0x4850_0000 + o          Cmem SF       → tag 2，偏移 o×32
//   0x48C0_0000 + o          Mmem 数据     → tag 1（容量检查仍按 36 MB）
//   0x4AC0_0000 + o          Mmem SF       → tag 2，偏移 o×32
//   0x4802_B000 + n×0x80     header_table  → tag 4
//   0x4803_3000 + n×0x100    topK_table    → tag 3

#include <cstdint>

#include "bach/ip/chip/core/dte/dte_types.h"

namespace latch {
namespace bach {

constexpr uint64_t kDocCmemBase = 0x48400000;
constexpr uint64_t kDocCmemBytes = 1024 * 1024;
constexpr uint64_t kDocCmemSfBase = 0x48500000;
constexpr uint64_t kDocCmemSfBytes = 32 * 1024;
constexpr uint64_t kDocMmemBase = 0x48C00000;
constexpr uint64_t kDocMmemBytes = 32ull * 1024 * 1024;
constexpr uint64_t kDocMmemSfBase = 0x4AC00000;
constexpr uint64_t kDocMmemSfBytes = 4ull * 1024 * 1024;
constexpr uint64_t kDocHeaderTableBase = 0x4802B000;
constexpr uint64_t kDocHeaderTableBytes = 4 * 1024;
constexpr uint64_t kDocHeaderEntryBytes = 0x80;
constexpr uint64_t kDocTopkTableBase = 0x48033000;
constexpr uint64_t kDocTopkTableBytes = 4 * 1024;
constexpr uint64_t kDocTopkEntryBytes = 0x100;
constexpr uint64_t kDocSfGroupBytes = 32;
constexpr uint64_t kDocSmemBase = 0x48300000;
constexpr uint64_t kDocSmemBytes = 32 * 1024;

class Agcu {
 public:
  static uint64_t SmemOffset(uint64_t addr) {
    if (InDoc(addr, kDocSmemBase, kDocSmemBytes)) return addr - kDocSmemBase;
    return addr;
  }
  static bool IsDocAddr(uint64_t addr) {
    return InDoc(addr, kDocCmemBase, kDocCmemBytes) ||
           InDoc(addr, kDocCmemSfBase, kDocCmemSfBytes) ||
           InDoc(addr, kDocMmemBase, kDocMmemBytes) ||
           InDoc(addr, kDocMmemSfBase, kDocMmemSfBytes) ||
           InDoc(addr, kDocHeaderTableBase, kDocHeaderTableBytes) ||
           InDoc(addr, kDocTopkTableBase, kDocTopkTableBytes);
  }
  static uint64_t FromDoc(uint64_t addr) {
    if (InDoc(addr, kDocCmemBase, kDocCmemBytes)) {
      return Tag(0x0, addr - kDocCmemBase);
    }
    if (InDoc(addr, kDocCmemSfBase, kDocCmemSfBytes)) {
      return Tag(0x2, (addr - kDocCmemSfBase) * kDocSfGroupBytes);
    }
    if (InDoc(addr, kDocMmemBase, kDocMmemBytes)) {
      return Tag(0x1, addr - kDocMmemBase);
    }
    if (InDoc(addr, kDocMmemSfBase, kDocMmemSfBytes)) {
      return Tag(0x2, (addr - kDocMmemSfBase) * kDocSfGroupBytes);
    }
    if (InDoc(addr, kDocHeaderTableBase, kDocHeaderTableBytes)) {
      return Tag(0x4, (addr - kDocHeaderTableBase) / kDocHeaderEntryBytes);
    }
    if (InDoc(addr, kDocTopkTableBase, kDocTopkTableBytes)) {
      return Tag(0x3, (addr - kDocTopkTableBase) / kDocTopkEntryBytes);
    }
    return addr;
  }

  static SegEndpoint Decode(uint64_t addr) {
    switch ((addr >> kEpShift) & kEpMask) {
      case 0x1: return SegEndpoint::kMmem;
      case 0x2: return SegEndpoint::kScale;
      case 0x3: return SegEndpoint::kTopk;
      case 0x4: return SegEndpoint::kHeader;
      default: return SegEndpoint::kCmem;
    }
  }

  void ExpandOut(Segment& s, uint64_t sid, Route route) const {
    bool src_mem = route == Route::kCmToRouter || route == Route::kMmToRouter ||
                   route == Route::kMmToCm;
    bool dst_mem = route == Route::kRouterToCm || route == Route::kRouterToMm ||
                   route == Route::kMmToCm;
    uint64_t src_stride = (route == Route::kMmToCm) ? 0 : s.stride;
    uint64_t dst_stride = s.stride;
    if (IsDocAddr(s.src)) {
      s.src = FromDoc(s.src + sid * src_stride);
      src_stride = 0;
    }
    if (IsDocAddr(s.dst)) {
      s.dst = FromDoc(s.dst + sid * dst_stride);
      dst_stride = 0;
    }
    s.src_kind = Decode(s.src);
    s.dst_kind = Decode(s.dst);
    if (src_mem) {
      s.src_addr = (s.src & kEpDataMask) + sid * src_stride;
    } else {
      s.src_addr = 0;
    }
    if (dst_mem) {
      s.dst_addr = (s.dst & kEpDataMask) + sid * dst_stride;
    } else {
      s.dst_addr = 0;
    }
  }

 private:
  static bool InDoc(uint64_t a, uint64_t base, uint64_t bytes) {
    return a >= base && a < base + bytes;
  }
  static uint64_t Tag(uint64_t tag, uint64_t off) {
    return (tag << kEpShift) | (off & kEpDataMask);
  }
};

}  // namespace bach
}  // namespace latch

#endif
