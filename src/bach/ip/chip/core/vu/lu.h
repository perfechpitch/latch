#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_LU_
#define _LATCH_BACH_IP_CHIP_CORE_VU_LU_

// M4 · LU 读 CM。
//
// 从 Core Mem 读向量、Mask 与标量，顺带做格式转换。CM 侧有 FP8_e4m3 / MXFP8 /
// BF16 / FP32 四种，向量通路内部只有 BF16 与 FP32 两种，所以低转高一律是精确
// 扩宽，只有 ld.fp32.v 在 DATA_TYPE=BF16 下是高转低：按 ROUND_MODE 窄化，有限
// 值上溢写饱和值、下溢写 0，Inf / NaN 原样透传，都不置位。
//
// CM 接口一次固定 1024 bit，不支持 burst，请求地址按 128 B 对齐。向量与掩码按
// 32 B 对齐、标量按 4 B 对齐，所以一条向量的两端都可能落在 128 B 块的中间：
// 本级把请求向下对齐到块边界，多读的部分收齐后截掉，这就是跨 128 B 边界的拆分
// 与重组。地址不满足当前访问格式的对齐要求时置位 CM_ADDR_ERROR（硬件到这一步
// 就把这笔 Load 丢弃、请求不发出；模型照发不误，见 vu.md 的“取舍”一节）。
//
// 14 拍的访问延迟由 Core Mem 那一侧给，本级只按 valid/ready 收发。
//
// 不读 LU 的执行分组不必等这次读回来。收下宏指令时把同一份微指令从 issued 口
// 交给发射级，RF 源和只依赖 RF 的分组可以先走；读回来的段从数据口交给
// VuVexeNet，按段号并进已经在飞的那一份。

#include <deque>
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
        issued(std::make_shared<VuUopsPort>(clock)),
        cmem(std::make_shared<MemPort>(clock)),
        beats(clock) {}

  VuUopsPort& In() { return *in; }
  void AttachIn(std::shared_ptr<VuUopsPort> p) { in = std::move(p); }
  VuFlowPort& Out() { return *out; }
  void AttachOut(std::shared_ptr<VuFlowPort> p) { out = std::move(p); }
  // 收下一条宏指令时把微指令再交一份给发射级，与本级的读并行。
  VuUopsPort& Issued() { return *issued; }
  void AttachIssued(std::shared_ptr<VuUopsPort> p) { issued = std::move(p); }
  MemPort& Cmem() { return *cmem; }
  void AttachCmem(std::shared_ptr<MemPort> p) { cmem = std::move(p); }

  uint64_t Beats() const { return beat_cnt; }
  uint64_t BusyCycles() const { return busy_cnt; }
  // CM 端口反压住本级的拍数。
  uint64_t StallCycles() const { return stall_cnt; }
  bool Quiescent() const override {
    return ctxs.empty() && !holding && ready_q.empty() && issued_q.empty() &&
           !issued_hold;
  }

 protected:
  void Step() override {
    // 末级先做：先把上一条交出去，再收响应、发请求，最后接新的一条。
    mem_used = false;
    Drain();
    Collect();
    Issue();
    Accept();
    DriveIssued();
    if (!mem_used) cmem->IdleReq();

    bool busy = !ctxs.empty();
    if (busy) ++busy_cnt;
    beats = beat_cnt;
    TracePerCycle("busy", busy ? 1 : 0);
    TracePerCycle("segq", ready_q.size());
    TracePerCycle("hold", holding ? 1 : 0);
  }

 private:
  // 一条宏指令的读：读哪几块、发了几块、回了几块，收回来的字节，分段的进度。
  struct Ctx {
    VuFlowPtr flow;
    uint64_t base = 0, head = 0, blocks = 0, sent = 0, got = 0, want_bytes = 0;
    // 分段走的那一档：一段多少元素、在 CM 上多少字节、共几段、已经交出去几段。
    // seg_len 为 0 表示本条不分段。
    uint64_t seg_len = 0, seg_bytes = 0, seg_total = 1, seg_done = 0;
    std::vector<uint8_t> buf, scale_buf;
  };

  // 攒好的段排队往下发，一拍一个。下游没收下就原样压着。
  void Drain() {
    if (!holding) return;
    if (!out->Ready()) {
      out->Drive(held, out_seq);
      return;
    }
    holding = false;
    held = VuFlowPtr();
    PumpQueue();
  }

  // 相邻两条宏指令的读可以接着发（《Vector Unit DSA》pipe_ctrl：用后一条的取数段
  // 掩盖前一条的尾段，重叠深度上限为 2）：前一条的读请求都发出去了就收下一条，
  // 不等它的响应回来。最多两条同时在读；不读 CM 的那一条要等前面的读都收完，免得
  // 抢到前面去。出口排队攒多了也先停一停。
  void Accept() {
    bool issuing = !ctxs.empty() && ctxs.back().sent < ctxs.back().blocks;
    bool full = issuing || ctxs.size() >= kVuOverlap ||
                ready_q.size() >= kReadyQDepth;
    if (!full && in->Valid() && in->Seq() != last_seq) {
      auto peek = in->Uops();
      if (peek && !peek->cfg.lu.Active() && !ctxs.empty()) full = true;
    }
    in->DriveReady(!full);
    if (full) return;
    if (!in->Valid() || in->Seq() == last_seq) return;
    auto uops = in->Uops();
    if (!uops) return;
    last_seq = in->Seq();

    auto flow = std::make_shared<VuFlow>();
    flow->uops = *uops;

    HandOff(uops);
    if (!uops->cfg.lu.Active()) {
      // 本条不读 CM。数据通路上不发空段，发射级自己按 VL 分段。
      return;
    }
    Plan(flow);
  }

  // 发射级与读并行。口上的 valid 不写会回落成上一拍，所以每拍都 Drive 或 Idle。
  // ready 是上一拍的：本拍第一次 Drive 的不能当拍丢掉。
  void HandOff(std::shared_ptr<VuUops> uops) {
    issued_q.push_back(std::move(uops));
  }

  void DriveIssued() {
    if (issued_hold && issued->Ready()) {
      issued_hold = false;
      issued_q.pop_front();
    }
    if (!issued_hold && !issued_q.empty()) {
      issued->Drive(issued_q.front(), ++issued_seq);
      issued_hold = true;
      return;
    }
    if (!issued_hold) issued->Idle();
  }

  // 算这一条要读哪几个 128 B 块。
  void Plan(VuFlowPtr const& flow) {
    VuUops const& uops = flow->uops;
    VuMacroInst const& inst = uops.inst;
    LuOp op = LuOp(uops.cfg.lu.opcode);
    uint64_t vl = inst.Vl();
    numeric::DataType t = CmType(op);
    uint64_t elem_bits = numeric::ElemBitsOf(t);
    // Mask 一位一个 element，按 8 B 为单位搬运；标量固定 4 B。
    uint64_t want = op == LuOp::kLdMask ? ((vl + 63) / 64) * 8
                    : op == LuOp::kLdSFp32
                        ? 4
                        : (vl * elem_bits + 7) / 8;

    Ctx c;
    c.flow = flow;
    // 能拆的那一档按 RF entry 拆段：一段的元素数就是一个 entry 装得下的数，
    // 在 CM 上占 seg_bytes 个字节。拆不了的照旧整条走一份。
    if (VuCanSegment(uops)) {
      c.seg_len = inst.Bf16() ? 64 : 32;
      c.seg_bytes = c.seg_len * elem_bits / 8;
      c.seg_total = (vl + c.seg_len - 1) / c.seg_len;
    }

    uint64_t addr = inst.LdAddr();
    // 对齐由访问格式决定：向量与掩码 32 B，标量 4 B。不合规置 CM_ADDR_ERROR，
    // 硬件到这一步就把这笔 Load 丢弃、请求不发出；模型照发不误（见 vu.md 的
    // “取舍”一节），只把异常位记下来。
    uint64_t grain = op == LuOp::kLdSFp32 ? kVuScalarAlign : kVuCmAlign;
    if (addr % grain != 0) {
      flow->error |= kVuErrCmAddr;
      flow->err_unit = kVuErrUnitLu;
    }
    // 请求地址向下对齐到 128 B 块，前面多出来的那一截收齐后截掉。
    c.head = addr % kVuVrfEntryBytes;
    c.base = addr - c.head;
    uint64_t total = c.head + want;
    c.blocks = (total + kVuVrfEntryBytes - 1) / kVuVrfEntryBytes;
    c.want_bytes = want;
    ctxs.push_back(std::move(c));
  }

  // 按收下的次序发：只有最后收下的那一条可能还有块没发。
  void Issue() {
    if (ctxs.empty()) return;
    Ctx& c = ctxs.back();
    if (c.sent >= c.blocks) return;
    if (!cmem->Ready()) {
      ++stall_cnt;
      return;
    }
    bool need_scale = LuOp(c.flow->uops.cfg.lu.opcode) == LuOp::kLdMxfp8;
    cmem->Read(c.base + c.sent * kVuVrfEntryBytes, kVuVrfEntryBytes, need_scale);
    mem_used = true;
    ++c.sent;
    ++beat_cnt;
  }

  // 响应按发出的次序回来：记到最早那条还没收齐的上。
  void Collect() {
    if (ctxs.empty() || !cmem->RspValid()) return;
    Ctx& c = ctxs.front();
    LOGCHECK(c.got < c.sent, "VuLu: 收到响应，却没有在等的读。");
    ByteBlockPtr d = cmem->RspData();
    if (d) {
      // CM 数据信号是 1056 bit = 128 B data + 4 B scale，scale 段仅 MXFP8 有效。
      // 一次响应因此可能带 132 字节，末尾那 4 个是这一块的 scale：MXFP8 的
      // scale 与数据一一映射，地址由硬件推断，不参与软件编址。
      uint64_t body_len = d->size();
      if (body_len > kVuVrfEntryBytes) {
        body_len = kVuVrfEntryBytes;
        c.scale_buf.insert(c.scale_buf.end(), d->begin() + kVuVrfEntryBytes,
                           d->end());
      }
      c.buf.insert(c.buf.end(), d->begin(), d->begin() + body_len);
    }
    ++c.got;
    if (c.seg_len != 0) {
      EmitSegments(c);
      return;
    }
    if (c.got < c.blocks) return;

    // 收齐了：截掉对齐多读的两头，再按 CM 侧格式解成 FP32。
    std::vector<uint8_t> body;
    if (c.head < c.buf.size()) {
      uint64_t end = c.head + c.want_bytes;
      if (end > c.buf.size()) end = c.buf.size();
      body.assign(c.buf.begin() + c.head, c.buf.begin() + end);
    }
    c.flow->seg_len = c.flow->uops.inst.Vl();
    Convert(c.flow, body, c);
    ready_q.push_back(c.flow);
    ctxs.pop_front();
    PumpQueue();
  }

  // 已经收到的字节够哪几段，就把那几段各做成一份交下去。最后一段按实际剩下的
  // 元素数算，整条不一定正好铺满一个 entry。
  void EmitSegments(Ctx& c) {
    uint64_t have = c.got * kVuVrfEntryBytes;
    uint64_t vl = c.flow->uops.inst.Vl();
    while (c.seg_done < c.seg_total) {
      uint64_t from = c.head + c.seg_done * c.seg_bytes;
      uint64_t to = from + c.seg_bytes;
      bool last = c.seg_done + 1 == c.seg_total;
      if (last) to = c.head + c.want_bytes;
      if (to > have && !(last && c.got >= c.blocks)) break;
      if (to > c.buf.size()) to = c.buf.size();

      auto seg = std::make_shared<VuFlow>();
      seg->uops = c.flow->uops;
      seg->error = c.flow->error;
      seg->err_unit = c.flow->err_unit;
      seg->seg = c.seg_done;
      seg->seg_base = c.seg_done * c.seg_len;
      seg->seg_len = last ? vl - seg->seg_base : c.seg_len;
      seg->seg_last = last;
      std::vector<uint8_t> body;
      if (from < to) body.assign(c.buf.begin() + from, c.buf.begin() + to);
      Convert(seg, body, c);
      ready_q.push_back(seg);
      ++c.seg_done;
    }
    if (c.seg_done >= c.seg_total) ctxs.pop_front();
    PumpQueue();
  }

  // 队首没人压着就立刻发一个出去。
  void PumpQueue() {
    if (holding || ready_q.empty()) return;
    held = ready_q.front();
    ready_q.pop_front();
    holding = true;
    out_seq = ++emit_seq;
    out->Drive(held, out_seq);
  }

  void Convert(VuFlowPtr const& f, std::vector<uint8_t> const& body,
               Ctx const& c) {
    VuUops const& uops = f->uops;
    VuMacroInst const& inst = uops.inst;
    LuOp op = LuOp(uops.cfg.lu.opcode);
    uint64_t vl = f->SegLen();

    if (op == LuOp::kLdMask) {
      // Mask 一位一个元素，唯一能写 MRF 的 LU 指令。
      f->lu.mask.reserve(vl);
      for (uint64_t i = 0; i < vl; ++i) {
        uint64_t at = i / 8;
        f->lu.mask.push_back(at < body.size() &&
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
      f->lu.scalar = numeric::FloatOf(b);
      f->lu.has_scalar = true;
      return;
    }

    numeric::DataType t = CmType(op);
    std::vector<float> scale;
    if (op == LuOp::kLdMxfp8) {
      scale = numeric::DecodeScale(t, c.scale_buf, c.scale_buf.size());
    }
    std::vector<float> v = numeric::Decode(t, body, vl);
    if (!scale.empty()) {
      // scale 是按块从块首起接回来的：第 i 个元素在块里的偏移是 head + i，它的
      // scale 是第 (head + i) / 32 个。向量从一行中间开始时 head 不为 0。
      uint64_t block = numeric::ScaleBlockOf(t);
      for (uint64_t i = 0; i < v.size(); ++i) {
        uint64_t b = (c.head + i) / block;
        if (b < scale.size()) v[i] *= scale[b];
      }
    }

    // ld.fp32.v 在 DATA_TYPE=BF16 下是唯一的高转低，按 ROUND_MODE 窄化：有限值
    // 上溢写饱和值、下溢写 0，Inf / NaN 原样透传，都不置位。
    if (op == LuOp::kLdFp32 && inst.Bf16()) {
      numeric::RoundMode mode = inst.Round();
      for (uint64_t i = 0; i < v.size(); ++i) {
        v[i] = numeric::FromBf16(numeric::NarrowBf16(v[i], mode));
      }
    }
    f->lu.vec = std::move(v);
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
  std::shared_ptr<VuUopsPort> issued;
  std::shared_ptr<MemPort> cmem;

  std::deque<std::shared_ptr<VuUops>> issued_q;
  bool issued_hold = false;
  uint64_t issued_seq = 0;

  VuFlowPtr held;
  std::deque<Ctx> ctxs;
  std::deque<VuFlowPtr> ready_q;
  bool holding = false, mem_used = false;
  uint64_t out_seq = 0, last_seq = 0, emit_seq = 0;
  uint64_t beat_cnt = 0, busy_cnt = 0, stall_cnt = 0;
  // 出口排队最多攒几段就先不收下一条。
  static constexpr uint64_t kReadyQDepth = 4;

  Logic64 beats;
};

}  // namespace bach
}  // namespace latch

#endif
