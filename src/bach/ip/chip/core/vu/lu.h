#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_LU_
#define _LATCH_BACH_IP_CHIP_CORE_VU_LU_

// M4 · LU 读 CM。
//
// 从 Core Mem 读向量、Mask 与标量，顺带做格式转换。CM 侧有 FP8_e4m3 / MXFP8 /
// BF16 / FP32 四种，向量通路内部只有 BF16 与 FP32 两种，所以低转高一律是精确
// 扩宽，只有 ld.fp32.vm 在 DATA_TYPE=BF16 下是高转低。那一档按 ROUND_MODE
// 窄化，结果为 NaN 时置 DATA_CVT_ERROR。
//
// CM 接口一次固定 1024 bit，不支持 burst，请求地址按 128 B 对齐。向量按 32 B
// 对齐、标量按 4 B 对齐，所以一条向量的两端都可能落在 128 B 块的中间：本级把
// 请求向下对齐到块边界，多读的部分收齐后截掉，这就是跨 128 B 边界的拆分与重组。
//
// 14 拍的访问延迟由 Core Mem 那一侧给，本级只按 valid/ready 收发。

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

class VuLu : public BachModule {
 public:
  VuLu(ClockPtr clock, const std::string& name, uint64_t parent = 0,
       bool tick = true)
      : BachModule(clock, name, parent, tick),
        in(std::make_shared<VuUopsPort>(clock)),
        out(std::make_shared<VuFlowPort>(clock)),
        cmem(std::make_shared<MemPort>(clock)),
        beats(clock) {}

  VuUopsPort& In() { return *in; }
  void AttachIn(std::shared_ptr<VuUopsPort> p) { in = std::move(p); }
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
    // 末级先做：先把上一条交出去，再收响应、发请求，最后接新的一条。
    mem_used = false;
    Drain();
    Collect();
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
    auto uops = in->Uops();
    if (!uops) {
      out->Idle();
      return;
    }
    last_seq = in->Seq();

    flow = std::make_shared<VuFlow>();
    flow->uops = *uops;
    seq = in->Seq();

    if (!uops->cfg.lu.Active()) {
      // 本条不读 CM，直接往下走。
      held = flow;
      holding = true;
      out_seq = seq;
      out->Drive(held, out_seq);
      return;
    }
    Plan(*uops);
    out->Idle();
  }

  // 算这一条要读哪几个 128 B 块。
  void Plan(VuUops const& uops) {
    VuMacroInst const& inst = uops.inst;
    LuOp op = LuOp(uops.cfg.lu.opcode);
    uint64_t vl = inst.Vl();
    numeric::DataType t = CmType(op);
    uint64_t elem_bits = numeric::ElemBitsOf(t);
    // Mask 一位一个 element，按 8 B 为单位搬运；标量固定 4 B。
    uint64_t want = op == LuOp::kLdVmMask ? ((vl + 63) / 64) * 8
                    : op == LuOp::kLdSFp32
                        ? 4
                        : (vl * elem_bits + 7) / 8;

    uint64_t addr = inst.LdAddr();
    // 对齐由访问格式决定：向量与掩码 32 B，标量 4 B。违反置 CM_ADDR_ERROR，
    // 本条照走。硬件只自检对齐、不做长度检查，不阻塞流水。
    uint64_t grain = op == LuOp::kLdSFp32 ? kVuScalarAlign : kVuCmAlign;
    if (addr % grain != 0) flow->error |= kVuErrCmAddr;
    // 请求地址向下对齐到 128 B 块，前面多出来的那一截收齐后截掉。
    head = addr % kVuVrfEntryBytes;
    base = addr - head;
    uint64_t total = head + want;
    blocks = (total + kVuVrfEntryBytes - 1) / kVuVrfEntryBytes;
    sent = 0;
    got = 0;
    want_bytes = want;
    buf.clear();
    scale_buf.clear();
    busy = true;
  }

  void Issue() {
    if (!busy || sent >= blocks) return;
    if (!cmem->Ready()) {
      ++stall_cnt;
      return;
    }
    bool need_scale = LuOp(flow->uops.cfg.lu.opcode) == LuOp::kLdMxfp8;
    cmem->Read(base + sent * kVuVrfEntryBytes, kVuVrfEntryBytes, need_scale);
    mem_used = true;
    ++sent;
    ++beat_cnt;
  }

  void Collect() {
    if (!busy || !cmem->RspValid()) return;
    ByteBlockPtr d = cmem->RspData();
    if (d) {
      // CM 数据信号是 1056 bit = 128 B data + 4 B scale，scale 段仅 MXFP8 有效。
      // 一次响应因此可能带 132 字节，末尾那 4 个是这一块的 scale：MXFP8 的
      // scale 与数据一一映射，地址由硬件推断，不参与软件编址。
      uint64_t body_len = d->size();
      if (body_len > kVuVrfEntryBytes) {
        body_len = kVuVrfEntryBytes;
        scale_buf.insert(scale_buf.end(), d->begin() + kVuVrfEntryBytes,
                         d->end());
      }
      buf.insert(buf.end(), d->begin(), d->begin() + body_len);
    }
    ++got;
    if (got < blocks) return;

    // 收齐了：截掉对齐多读的两头，再按 CM 侧格式解成 FP32。
    std::vector<uint8_t> body;
    if (head < buf.size()) {
      uint64_t end = head + want_bytes;
      if (end > buf.size()) end = buf.size();
      body.assign(buf.begin() + head, buf.begin() + end);
    }
    Convert(body);

    busy = false;
    held = flow;
    holding = true;
    out_seq = seq;
    out->Drive(held, out_seq);
  }

  void Convert(std::vector<uint8_t> const& body) {
    VuUops const& uops = flow->uops;
    VuMacroInst const& inst = uops.inst;
    LuOp op = LuOp(uops.cfg.lu.opcode);
    uint64_t vl = inst.Vl();

    if (op == LuOp::kLdVmMask) {
      // Mask 一位一个元素，唯一能写 MRF 的 LU 指令。
      flow->lu.mask.reserve(vl);
      for (uint64_t i = 0; i < vl; ++i) {
        uint64_t at = i / 8;
        flow->lu.mask.push_back(at < body.size() &&
                                ((body[at] >> (i % 8)) & 1u) != 0);
      }
      return;
    }
    if (op == LuOp::kLdSFp32) {
      // 标量只有 FP32 一种精度。SEXE 的四处源之一。
      uint32_t b = 0;
      for (int k = 0; k < 4 && uint64_t(k) < body.size(); ++k) {
        b |= uint32_t(body[k]) << (8 * k);
      }
      flow->lu.scalar = numeric::FloatOf(b);
      flow->lu.has_scalar = true;
      return;
    }

    numeric::DataType t = CmType(op);
    std::vector<float> scale;
    if (op == LuOp::kLdMxfp8) {
      uint64_t nblock = (vl + numeric::ScaleBlockOf(t) - 1) /
                        numeric::ScaleBlockOf(t);
      scale = numeric::DecodeScale(t, scale_buf, nblock);
    }
    std::vector<float> v = numeric::Decode(t, body, vl);
    if (!scale.empty()) {
      uint64_t block = numeric::ScaleBlockOf(t);
      for (uint64_t i = 0; i < v.size(); ++i) {
        uint64_t b = i / block;
        if (b < scale.size()) v[i] *= scale[b];
      }
    }

    // ld.fp32.vm 在 DATA_TYPE=BF16 下是唯一的高转低，按 ROUND_MODE 窄化。
    if (op == LuOp::kLdFp32 && inst.Bf16()) {
      numeric::RoundMode mode = inst.Round();
      for (uint64_t i = 0; i < v.size(); ++i) {
        uint16_t h = numeric::NarrowBf16(v[i], mode);
        if (numeric::NarrowMakesNan(v[i], h)) flow->error |= kVuErrDataCvt;
        v[i] = numeric::FromBf16(h);
      }
    }
    flow->lu.vec = std::move(v);
  }

  static numeric::DataType CmType(LuOp op) {
    switch (op) {
      case LuOp::kLdBf16: return numeric::DataType::kBf16;
      case LuOp::kLdFp8e4m3: return numeric::DataType::kMxfp8;
      case LuOp::kLdMxfp8: return numeric::DataType::kMxfp8;
      default: return numeric::DataType::kFp32;
    }
  }

  std::shared_ptr<VuUopsPort> in;
  std::shared_ptr<VuFlowPort> out;
  std::shared_ptr<MemPort> cmem;

  VuFlowPtr flow, held;
  bool busy = false, holding = false, mem_used = false;
  uint64_t seq = 0, out_seq = 0, last_seq = 0;
  uint64_t base = 0, head = 0, blocks = 0, sent = 0, got = 0, want_bytes = 0;
  uint64_t beat_cnt = 0, busy_cnt = 0, stall_cnt = 0;
  std::vector<uint8_t> buf, scale_buf;

  Logic64 beats;
};

}  // namespace bach
}  // namespace latch

#endif
