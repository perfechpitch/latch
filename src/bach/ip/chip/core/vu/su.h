#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_SU_
#define _LATCH_BACH_IP_CHIP_CORE_VU_SU_

// M8 · SU 写 CM。
//
// 把结果转成 CM 侧的格式写回 Core Mem。BF16 / FP32 → FP8_e4m3 / MXFP8 / BF16 /
// FP32，高转低按 TYPE_VL.ROUND_MODE 舍入。MXFP8 的 scale 由本级按块算出来，
// 地址硬件按一一映射推断，不参与软件编址。
//
// 与 LU 对称：一次请求固定 1024 bit 不 burst，请求地址按 128 B 对齐，所以一条
// 向量的两端可能落在块中间，那两个块要先读回来再改中间那一段。本级不读，改
// 成按整块写并把两端补零，Core Mem 侧按 bytes 只取有效那一段。跨 128 B 边界的
// 拆分由本级完成。

#include <memory>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/common/numeric/mx.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/chip/core/vu/vu_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class VuSu : public BachModule {
 public:
  VuSu(ClockPtr clock, const std::string& name, uint64_t parent = 0,
       bool tick = true)
      : BachModule(clock, name, parent, tick),
        in(std::make_shared<VuFlowPort>(clock)),
        out(std::make_shared<VuFlowPort>(clock)),
        cmem(std::make_shared<MemPort>(clock)),
        beats(clock) {}

  VuFlowPort& In() { return *in; }
  void AttachIn(std::shared_ptr<VuFlowPort> p) { in = std::move(p); }
  VuFlowPort& Out() { return *out; }
  void AttachOut(std::shared_ptr<VuFlowPort> p) { out = std::move(p); }
  MemPort& Cmem() { return *cmem; }
  void AttachCmem(std::shared_ptr<MemPort> p) { cmem = std::move(p); }

  uint64_t Beats() const { return beat_cnt; }
  uint64_t BusyCycles() const { return busy_cnt; }
  // CM 端口反压住本级的拍数。
  uint64_t StallCycles() const { return stall_cnt; }
  bool Quiescent() const override { return !busy && !holding; }

 protected:
  void Step() override {
    mem_used = false;
    Drain();
    Issue();
    Accept();
    if (!mem_used) cmem->IdleReq();

    if (busy) ++busy_cnt;
    beats = beat_cnt;
    TracePerCycle("busy", busy ? 1 : 0);
  }

 private:
  void Drain() {
    if (!holding) return;
    if (!out->Ready()) {
      out->Drive(held, out_seq);
      return;
    }
    holding = false;
    held = VuFlowPtr();
  }

  void Accept() {
    in->DriveReady(!busy && !holding);
    if (busy || holding) {
      if (!holding) out->Idle();
      return;
    }
    if (!in->Valid() || in->Seq() == last_seq) {
      out->Idle();
      return;
    }
    VuFlowPtr f = in->Flow();
    if (!f) {
      out->Idle();
      return;
    }
    last_seq = in->Seq();
    flow = f;
    seq = in->Seq();

    if (!f->uops.cfg.su.Active()) {
      Finish();
      return;
    }
    Plan();
    out->Idle();
  }

  void Plan() {
    VuUops const& uops = flow->uops;
    VuMacroInst const& inst = uops.inst;
    SuOp op = SuOp(uops.cfg.su.opcode);
    uint64_t vl = inst.Vl();
    numeric::RoundMode mode = inst.Round();

    std::vector<uint8_t> body;
    if (op == SuOp::kStVmMask) {
      // Mask 按 8 B 为单位搬运。
      body.assign(((vl + 63) / 64) * 8, 0);
      std::vector<bool> const& m = flow->su_in.mask;
      for (uint64_t i = 0; i < vl && i < m.size(); ++i) {
        if (m[i]) body[i / 8] = uint8_t(body[i / 8] | (1u << (i % 8)));
      }
    } else if (op == SuOp::kStSFp32) {
      uint32_t b = numeric::BitsOf(flow->su_in.scalar);
      body.resize(4);
      for (int k = 0; k < 4; ++k) body[k] = uint8_t((b >> (8 * k)) & 0xFFu);
    } else {
      numeric::DataType t = CmType(op);
      std::vector<float> v = flow->su_in.vec;
      v.resize(vl, 0.0f);
      // 只有 MXFP8 这一档带 scale：块内取绝对值最大的那个定阶。FP8_e4m3 是
      // 定点意义上的裸格式，没有块 scale。
      std::vector<float> scale;
      if (op == SuOp::kStMxfp8) scale = numeric::MakeScale(t, v);
      body = numeric::Encode(t, v, scale, mode);
      scale_bytes = numeric::EncodeScale(t, scale);
    }

    uint64_t addr = inst.StAddr();
    // 对齐由访问格式决定：向量与掩码 32 B，标量 4 B。
    uint64_t grain = op == SuOp::kStSFp32 ? kVuScalarAlign : kVuCmAlign;
    if (addr % grain != 0) flow->error |= kVuErrCmAddr;
    head = addr % kVuVrfEntryBytes;
    base = addr - head;
    // 请求按 128 B 块对齐，一条向量的两端可能落在块中间：数据前面补到块边界，
    // 每一块的写请求再带上块内有效区间，两头属于相邻数据的字节不动。
    payload.assign(head, 0);
    payload.insert(payload.end(), body.begin(), body.end());
    valid_end = payload.size();
    blocks = (valid_end + kVuVrfEntryBytes - 1) / kVuVrfEntryBytes;
    payload.resize(blocks * kVuVrfEntryBytes, 0);
    sent = 0;
    busy = true;
  }

  void Issue() {
    if (!busy) return;
    if (!cmem->Ready()) {
      ++stall_cnt;
      return;
    }
    uint64_t block_at = sent * kVuVrfEntryBytes;
    auto d = std::make_shared<ByteBlock>(
        payload.begin() + block_at,
        payload.begin() + block_at + kVuVrfEntryBytes);
    // 这一块里哪一段是本条向量的：第一块跳过对齐补的那一截，最后一块在有效
    // 长度处收住。
    uint64_t from = sent == 0 ? head : 0;
    uint64_t to = valid_end > block_at + kVuVrfEntryBytes
                      ? kVuVrfEntryBytes
                      : valid_end - block_at;
    bool has_scale = !scale_bytes.empty();
    if (has_scale) {
      // 数据信号是 1056 bit = 128 B data + 4 B scale。一个 128 B 块 32 个 MXFP8
      // 元素、正好一个 scale 字节，四个字节里只有第一个有效。
      uint64_t at = sent;
      d->push_back(at < scale_bytes.size() ? scale_bytes[at] : 0);
      d->resize(kVuVrfEntryBytes + 4, 0);
    }
    cmem->Write(base + block_at, d, has_scale, from, to - from);
    mem_used = true;
    ++sent;
    ++beat_cnt;
    if (sent >= blocks) {
      busy = false;
      Finish();
    }
  }

  void Finish() {
    held = flow;
    flow = VuFlowPtr();
    holding = true;
    out_seq = seq;
    out->Drive(held, out_seq);
  }

  static numeric::DataType CmType(SuOp op) {
    switch (op) {
      case SuOp::kStBf16: return numeric::DataType::kBf16;
      case SuOp::kStFp8e4m3: return numeric::DataType::kMxfp8;
      case SuOp::kStMxfp8: return numeric::DataType::kMxfp8;
      default: return numeric::DataType::kFp32;
    }
  }

  std::shared_ptr<VuFlowPort> in, out;
  std::shared_ptr<MemPort> cmem;

  VuFlowPtr flow, held;
  bool busy = false, holding = false, mem_used = false;
  uint64_t seq = 0, out_seq = 0, last_seq = 0;
  uint64_t base = 0, head = 0, blocks = 0, sent = 0, beat_cnt = 0;
  uint64_t busy_cnt = 0, stall_cnt = 0;
  uint64_t valid_end = 0;
  std::vector<uint8_t> payload, scale_bytes;

  Logic64 beats;
};

}  // namespace bach
}  // namespace latch

#endif
