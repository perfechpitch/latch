#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_REGFILES_
#define _LATCH_BACH_IP_CHIP_CORE_VU_REGFILES_

// VU 的三块寄存器堆：VRF / MRF / SRF。
//
// 这三块是存储阵列，不是独立打拍的模块。文档把它们列在存储器一节里，端口数
// 是 VRF 2R2W、MRF 2R1W、SRF 8 逻辑读 6 逻辑写。SMUX 读、DMUX 写，两级在同一
// 条流水上，由装配统一驱动，读写顺序确定。
//
// 一条向量按元素读写。硬件上 VRF 一次读 128 B、一条 VL=16384 的 FP32 向量要
// 512 拍读完；拍数由通路那一级按 Beats() 记，这里只管取到的位对不对。
//
// 端口上限由 pipe_ctrl 在展开时查（F21），这里只按下标取，不再查一遍。

#include <array>
#include <cstdint>
#include <vector>

#include "base/log.h"
#include "bach/common/numeric/formats.h"
#include "bach/ip/chip/core/vu/vu_types.h"

namespace latch {
namespace bach {

class VuRegfiles {
 public:
  VuRegfiles() : vrf(kVuVrfBytes, 0), mrf(kVuMrfBytes, 0), srf(kVuSrfBytes, 0) {}

  // ── VRF ──
  //
  // 从 entry 号 base 起连续放 count 个元素。元素按 FP32 或 BF16 存，取出来一律
  // 是 FP32：向量通路内部只有这一种表示，精度由 DATA_TYPE 决定存进去时窄不窄。
  std::vector<float> ReadVrf(uint64_t base, uint64_t count, bool bf16) const {
    std::vector<float> out;
    out.reserve(count);
    uint64_t off = base * kVuVrfEntryBytes;
    uint64_t step = bf16 ? 2 : 4;
    for (uint64_t i = 0; i < count; ++i) {
      uint64_t at = off + i * step;
      if (at + step > vrf.size()) {
        out.push_back(0.0f);
        continue;
      }
      if (bf16) {
        uint16_t h = uint16_t(vrf[at]) | (uint16_t(vrf[at + 1]) << 8);
        out.push_back(numeric::FromBf16(h));
      } else {
        uint32_t b = 0;
        for (int k = 0; k < 4; ++k) b |= uint32_t(vrf[at + k]) << (8 * k);
        out.push_back(numeric::FloatOf(b));
      }
    }
    return out;
  }

  void WriteVrf(uint64_t base, std::vector<float> const& v, bool bf16,
                numeric::RoundMode mode) {
    uint64_t off = base * kVuVrfEntryBytes;
    uint64_t step = bf16 ? 2 : 4;
    for (uint64_t i = 0; i < v.size(); ++i) {
      uint64_t at = off + i * step;
      if (at + step > vrf.size()) break;
      if (bf16) {
        uint16_t h = numeric::NarrowBf16(v[i], mode);
        vrf[at] = uint8_t(h & 0xFFu);
        vrf[at + 1] = uint8_t(h >> 8);
      } else {
        uint32_t b = numeric::BitsOf(v[i]);
        for (int k = 0; k < 4; ++k) vrf[at + k] = uint8_t((b >> (8 * k)) & 0xFFu);
      }
    }
  }

  // ── MRF ──
  //
  // 一个 entry 64 bit：FP32 精度下掩码只用低 32 bit（一个 entry 配一个 VRF
  // entry 的 32 个 element），BF16 下用满 64 bit。索引是 entry 号，一条 VL 长的
  // 掩码占 ⌈VL ÷ 每 entry 元素数⌉ 个连续 entry。
  std::vector<bool> ReadMrf(uint64_t entry, uint64_t count, bool bf16) const {
    uint64_t per = bf16 ? 64 : 32;
    std::vector<bool> out;
    out.reserve(count);
    for (uint64_t i = 0; i < count; ++i) {
      uint64_t bit = (entry + i / per) * 64 + (i % per);
      uint64_t at = bit / 8;
      out.push_back(at < mrf.size() && ((mrf[at] >> (bit % 8)) & 1u) != 0);
    }
    return out;
  }

  void WriteMrf(uint64_t entry, std::vector<bool> const& m, bool bf16) {
    uint64_t per = bf16 ? 64 : 32;
    for (uint64_t i = 0; i < m.size(); ++i) {
      uint64_t bit = (entry + i / per) * 64 + (i % per);
      uint64_t at = bit / 8;
      if (at >= mrf.size()) break;
      uint8_t one = uint8_t(1u << (bit % 8));
      if (m[i]) {
        mrf[at] = uint8_t(mrf[at] | one);
      } else {
        mrf[at] = uint8_t(mrf[at] & ~one);
      }
    }
  }

  // ── SRF ──
  //
  // 4 B/entry，只有 FP32 一种精度。
  float ReadSrf(uint64_t idx) const {
    if (idx >= kVuSrfEntry) return 0.0f;
    uint32_t b = 0;
    for (int k = 0; k < 4; ++k) b |= uint32_t(srf[idx * 4 + k]) << (8 * k);
    return numeric::FloatOf(b);
  }

  void WriteSrf(uint64_t idx, float v) {
    if (idx >= kVuSrfEntry) return;
    uint32_t b = numeric::BitsOf(v);
    for (int k = 0; k < 4; ++k) srf[idx * 4 + k] = uint8_t((b >> (8 * k)) & 0xFFu);
  }

  // ── 后门 ──
  //
  // reg_file_addr / reg_file_data 这条通路与宏指令异步，由软件保证访问期间目标
  // RF 不被在飞的宏指令读写，硬件不查。地址在 0x2000 起的 DSA-RF 区内，按
  // VRF、MRF、SRF 依次排。
  uint64_t Backdoor(uint64_t off) const {
    std::vector<uint8_t> const* mem = nullptr;
    uint64_t at = 0;
    if (!Locate(off, mem, at)) return 0;
    uint32_t b = 0;
    for (int k = 0; k < 4; ++k) {
      if (at + k < mem->size()) b |= uint32_t((*mem)[at + k]) << (8 * k);
    }
    return b;
  }

  void WriteBackdoor(uint64_t off, uint64_t v) {
    std::vector<uint8_t> const* mem = nullptr;
    uint64_t at = 0;
    if (!Locate(off, mem, at)) return;
    auto& target = const_cast<std::vector<uint8_t>&>(*mem);
    for (int k = 0; k < 4; ++k) {
      if (at + k < target.size()) target[at + k] = uint8_t((v >> (8 * k)) & 0xFFu);
    }
  }

 private:
  bool Locate(uint64_t off, std::vector<uint8_t> const*& mem,
              uint64_t& at) const {
    if (off < kVuVrfBytes) {
      mem = &vrf;
      at = off;
      return true;
    }
    off -= kVuVrfBytes;
    if (off < kVuMrfBytes) {
      mem = &mrf;
      at = off;
      return true;
    }
    off -= kVuMrfBytes;
    if (off < kVuSrfBytes) {
      mem = &srf;
      at = off;
      return true;
    }
    return false;
  }

  std::vector<uint8_t> vrf, mrf, srf;
};

}  // namespace bach
}  // namespace latch

#endif
