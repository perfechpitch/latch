#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_SU_
#define _LATCH_BACH_IP_CHIP_CORE_VU_SU_

// M8 · SU 写 CM。
//
// 把结果转成 CM 侧的格式写回 Core Mem。BF16 / FP32 → FP8_e4m3 / MXFP8 / BF16 /
// FP32，高转低按 TYPE_VL.ROUND_MODE 舍入。MXFP8 的 scale 由本级按块算出来，
// 地址硬件按一一映射推断，不参与软件编址。
//
// 写出的输入阶段是 NaN / Inf 替换与上报的两个收口位置之一（另一个是归约输出）：
// 替换模式下先把 NaN 换成 NAN_REPLACE_VALUE、±Inf 换成 ±INF_REPLACE_VALUE，再
// 转格式；非替换模式下 NaN 置位 error_code.NAN_ERROR，NaN / Inf 原样写出。
//
// 与 LU 对称：一次请求固定 1024 bit 不 burst，请求地址按 128 B 对齐，所以一条
// 向量的两端可能落在块中间，那两个块要先读回来再改中间那一段。本级不读，改
// 成按整块写并把两端补零，Core Mem 侧按 bytes 只取有效那一段。跨 128 B 边界的
// 拆分由本级完成。地址不满足访问格式的对齐要求时置位 CM_ADDR_ERROR。

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
  bool Quiescent() const override {
    return !busy && !holding && done_q.empty() && wr_q.empty();
  }

 protected:
  // 收在发之前：一段进来当拍就把它那一块的写请求发出去，下一拍接着收下一段。
  // 出口只在 Emit 里写一次，一拍一个端口只能写一次。
  void Step() override {
    mem_used = false;
    Accept();
    Issue();
    Emit();
    if (!mem_used) cmem->IdleReq();

    if (busy) ++busy_cnt;
    beats = beat_cnt;
    TracePerCycle("busy", busy ? 1 : 0);
    TracePerCycle("wrq", wr_q.size());
    TracePerCycle("doneq", done_q.size());
  }

 private:
  void Accept() {
    // 上一段的块还没发完就不收；发完的等着交出去，不挡下一段进来。
    bool room = !busy && done_q.size() < 2;
    in->DriveReady(room);
    if (!room) return;
    if (!in->Valid() || in->Seq() == last_seq) return;
    VuFlowPtr f = in->Flow();
    if (!f) return;
    last_seq = in->Seq();
    flow = f;
    seq = in->Seq();

    if (!f->uops.cfg.su.Active()) {
      Finish();
      return;
    }
    Plan();
  }

  void Plan() {
    VuUops const& uops = flow->uops;
    VuMacroInst const& inst = uops.inst;
    SuOp op = SuOp(uops.cfg.su.opcode);
    uint64_t vl = flow->SegLen();
    numeric::RoundMode mode = inst.Round();

    scale_bytes.clear();
    // 分段走时这一段落在整条的哪一截：按 CM 上的元素宽度换算。不分段的那一档
    // seg_base 是 0，地址就是整条的起点。
    uint64_t addr = inst.StAddr() + flow->seg_base *
                                        numeric::ElemBitsOf(CmType(op)) / 8;
    // 对齐由访问格式决定：向量与掩码 32 B，标量 4 B。不合规置 CM_ADDR_ERROR，
    // 硬件到这一步就把这笔 Store 丢弃、请求不发出；模型照发不误（见 vu.md 的
    // 「取舍」一节），只把异常位记下来。
    uint64_t grain = op == SuOp::kStSFp32 ? kVuScalarAlign : kVuCmAlign;
    if (addr % grain != 0) {
      flow->error |= kVuErrCmAddr;
      flow->err_unit = kVuErrUnitSu;
    }

    std::vector<uint8_t> body;
    if (op == SuOp::kStMask) {
      // Mask 按 8 B 为单位搬运。
      body.assign(((vl + 63) / 64) * 8, 0);
      std::vector<bool> const& m = flow->su_in.mask;
      for (uint64_t i = 0; i < vl && i < m.size(); ++i) {
        if (m[i]) body[i / 8] = uint8_t(body[i / 8] | (1u << (i % 8)));
      }
    } else if (op == SuOp::kStSFp32) {
      // 标量也过一遍替换 / 上报，再原样写出。
      float s = VuSettleNanInf(flow->su_in.scalar, uops, kVuErrUnitSu, *flow);
      uint32_t b = numeric::BitsOf(s);
      body.resize(4);
      for (int k = 0; k < 4; ++k) body[k] = uint8_t((b >> (8 * k)) & 0xFFu);
    } else {
      numeric::DataType t = CmType(op);
      std::vector<float> v = flow->su_in.vec;
      v.resize(vl, 0.0f);
      // 写出的输入阶段：先按替换规则处理 NaN / Inf，再转格式。
      for (uint64_t i = 0; i < v.size(); ++i) {
        v[i] = VuSettleNanInf(v[i], uops, kVuErrUnitSu, *flow);
      }
      // 只有 MXFP8 这一档带 scale：块内取绝对值最大的那个定阶。FP8_e4m3 是
      // 定点意义上的裸格式，没有块 scale。
      std::vector<float> scale;
      if (op == SuOp::kStMxfp8) {
        bool round_up = (uops.cfg.su.raw & kVuSuScaleRoundUp) != 0;
        scale = numeric::MakeScale(t, v, round_up);
      }
      body = numeric::Encode(t, v, scale, mode);
      scale_bytes = numeric::EncodeScale(t, scale);
    }

    // 请求按 128 B 块对齐，一条向量的两端可能落在块中间：数据前面补到块边界，
    // 每一块的写请求再带上块内有效区间，两头属于相邻数据的字节不动。
    //
    // 分段走时前后两段在 CM 上是连着的，一段的尾巴与下一段的头合起来才是一个
    // 完整的块。所以按块攒：凑满一块就排一笔，余下的留给下一段，最后一段把余
    // 量一并排掉。一段因此排一笔，不会为了跨边界那几个字节多写一次。
    if (flow->seg == 0) {
      head = addr % kVuVrfEntryBytes;
      acc_base = addr - head;
      acc.assign(head, 0);
      acc_from = head;
      blk_idx = 0;
    }
    acc.insert(acc.end(), body.begin(), body.end());
    bool has_scale = !scale_bytes.empty();
    while (acc.size() >= kVuVrfEntryBytes) {
      PushBlock(kVuVrfEntryBytes, has_scale);
    }
    if (flow->seg_last && acc.size() > acc_from) {
      uint64_t len = acc.size();
      acc.resize(kVuVrfEntryBytes, 0);
      PushBlock(len, has_scale);
    }
    busy = true;
  }

  // 攒满的那一块排进待发队列。valid_len 是这一块里到哪为止是本条向量的。
  void PushBlock(uint64_t valid_len, bool has_scale) {
    auto d = std::make_shared<ByteBlock>(acc.begin(),
                                         acc.begin() + kVuVrfEntryBytes);
    if (has_scale) {
      // 数据信号是 1056 bit = 128 B data + 4 B scale。一个 MXFP8 元素一字节，
      // 32 个共用一个 scale，所以这一块的 4 个 scale 字节各管块内一段 32 B：第
      // g 段在向量里是第 (块首 + g × 32 − head) / 32 个 scale。不属于本条向量
      // 的那几段补 0，存储只改有效区间覆盖到的那几组。
      for (uint64_t g = 0; g < 4; ++g) {
        uint64_t at = blk_idx * kVuVrfEntryBytes + g * 32;
        uint64_t idx = at >= head ? (at - head) / 32 : scale_bytes.size();
        d->push_back(at >= head && idx < scale_bytes.size() ? scale_bytes[idx]
                                                            : 0);
      }
    }
    Blk b;
    b.at = acc_base;
    b.d = d;
    b.from = acc_from;
    b.len = valid_len > acc_from ? valid_len - acc_from : 0;
    b.scale = has_scale;
    wr_q.push_back(b);
    acc.erase(acc.begin(), acc.begin() + kVuVrfEntryBytes);
    acc_base += kVuVrfEntryBytes;
    acc_from = 0;
    ++blk_idx;
  }

  void Issue() {
    if (!busy) return;
    if (wr_q.empty()) {
      // 这一段的字节还没凑满一块，留给下一段，本段这就算走完。
      busy = false;
      Finish();
      return;
    }
    if (!cmem->Ready()) {
      ++stall_cnt;
      return;
    }
    Blk const& b = wr_q.front();
    cmem->Write(b.at, b.d, b.scale, b.from, b.len);
    mem_used = true;
    wr_q.pop_front();
    ++beat_cnt;
    if (wr_q.empty()) {
      busy = false;
      Finish();
    }
  }

  // 这一段的块都发完了，排队交给下游。
  void Finish() {
    done_q.push_back({flow, seq});
    flow = VuFlowPtr();
  }

  void Emit() {
    if (holding) {
      if (!out->Ready()) {
        out->Drive(held, out_seq);
        return;
      }
      holding = false;
      held = VuFlowPtr();
    }
    if (!done_q.empty()) {
      held = done_q.front().f;
      out_seq = done_q.front().seq;
      done_q.pop_front();
      holding = true;
      out->Drive(held, out_seq);
      return;
    }
    out->Idle();
  }

  static numeric::DataType CmType(SuOp op) {
    switch (op) {
      case SuOp::kStBf16: return numeric::DataType::kBf16;
      case SuOp::kStFp8e4m3: return numeric::DataType::kMxfp8;
      case SuOp::kStMxfp8: return numeric::DataType::kMxfp8;
      default: return numeric::DataType::kFp32;
    }
  }

  // 发完块、等着交给下游的那几段。
  struct Done {
    VuFlowPtr f;
    uint64_t seq = 0;
  };

  // 攒好、等着发的一块：落在哪、块内哪一段是本条向量的。
  struct Blk {
    uint64_t at = 0;
    ByteBlockPtr d;
    uint64_t from = 0, len = 0;
    bool scale = false;
  };

  std::shared_ptr<VuFlowPort> in, out;
  std::shared_ptr<MemPort> cmem;
  std::deque<Done> done_q;
  std::deque<Blk> wr_q;

  VuFlowPtr flow, held;
  bool busy = false, holding = false, mem_used = false;
  uint64_t seq = 0, out_seq = 0, last_seq = 0;
  uint64_t head = 0, beat_cnt = 0;
  uint64_t busy_cnt = 0, stall_cnt = 0;
  // 按块攒的那一摊：acc 从 acc_base 这个块边界起，acc_from 是块内第一个属于
  // 本条向量的字节，blk_idx 是已经排出去几块。
  std::vector<uint8_t> acc;
  uint64_t acc_base = 0, acc_from = 0, blk_idx = 0;
  std::vector<uint8_t> scale_bytes;

  Logic64 beats;
};

}  // namespace bach
}  // namespace latch

#endif
