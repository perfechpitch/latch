#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_VEXE_NET_
#define _LATCH_BACH_IP_CHIP_CORE_VU_VEXE_NET_

// VEXE 五个分组的发射与汇合（F40c、F40d）。
//
// 分组是 VALU0 / VALU1 / VALU2 / VSFU0 / VSFU1。各自有一条流水：每拍至多收一段、
// 走完自己的首拍、每拍交一段。谁等谁由 src_sel 决定，不把五个分组串成固定链。
// 没有依赖的分组同时开工；汇合点取较晚的那一路。短的那一路靠软件插 vmv.v.v
// 补齐，这里不另做对齐队列。
//
// 不读 LU 的分组在 CM 响应回来之前就开工。发射级按段把 RF 源备好送进来；LU 的
// 数据口按段号并进来，用到 LU 的分组等到这一段的读数据到了再算。

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <string>

#include "bach/ip/chip/core/vu/mux.h"
#include "bach/ip/chip/core/vu/pipe_ctrl.h"
#include "bach/ip/chip/core/vu/valu.h"
#include "bach/ip/chip/core/vu/vsfu.h"
#include "bach/ip/chip/core/vu/vu_latency.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 一条宏指令拆成段，每拍送一段。RF 在这一拍读，与 LU 的读并行。
class VuIssue : public BachModule {
 public:
  VuIssue(ClockPtr clock, const std::string& name, VuSmux& mux, VuPipeCtrl& sb,
          uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        smux(mux),
        pipe(sb),
        in(std::make_shared<VuUopsPort>(clock)),
        out(std::make_shared<VuFlowPort>(clock)) {}

  uint64_t Holds() const { return holds; }

  VuUopsPort& In() { return *in; }
  void AttachIn(std::shared_ptr<VuUopsPort> p) { in = std::move(p); }
  VuFlowPort& Out() { return *out; }
  void AttachOut(std::shared_ptr<VuFlowPort> p) { out = std::move(p); }

  bool Quiescent() const override {
    return macros.empty() && !held;
  }

 protected:
  void Step() override {
    // 上一拍送出的段，下游这拍已经看过。ready 为真就松开，再发下一段。
    if (held && out->Ready()) {
      held.reset();
    }
    Take();
    if (!held) Pump();
    if (held) out->Drive(held->f, held->seq);
    else out->Idle();
    in->DriveReady(macros.size() < 4);
  }

 private:
  struct Macro {
    std::shared_ptr<VuUops> uops;
    uint64_t next = 0;
    uint64_t total = 1;
    bool split = false;
  };

  void Take() {
    if (macros.size() >= 4) return;
    if (!in->Valid() || in->Seq() == last_seq) return;
    std::shared_ptr<VuUops> u = in->Uops();
    if (!u) return;
    last_seq = in->Seq();
    Macro m;
    m.uops = std::move(u);
    m.split = VuCanSegment(*m.uops);
    m.total = m.split ? m.uops->inst.Entries() : 1;
    macros.push_back(std::move(m));
  }

  void Pump() {
    if (macros.empty()) return;
    Macro& m = macros.front();
    auto f = std::make_shared<VuFlow>();
    f->uops = *m.uops;
    f->seg = m.next;
    if (m.split) {
      uint64_t lanes = m.uops->inst.Bf16() ? 64 : 32;
      uint64_t vl = m.uops->inst.Vl();
      f->seg_base = m.next * lanes;
      f->seg_last = m.next + 1 == m.total;
      f->seg_len = f->seg_last ? vl - f->seg_base : lanes;
    } else {
      f->seg_last = true;
    }
    // 这一段用到的 4-entry 格还被前一条占着，先不读 RF。
    if (!pipe.SegmentReady(*m.uops, m.next, m.split)) {
      ++holds;
      return;
    }
    smux.LoadRf(*f);
    pipe.NoteSegmentRead(*m.uops, m.next, m.split, f->seg_last);
    held = Held{std::move(f), ++emit_seq};
    ++m.next;
    if (m.next >= m.total) macros.pop_front();
  }

  VuSmux& smux;
  VuPipeCtrl& pipe;
  std::shared_ptr<VuUopsPort> in;
  std::shared_ptr<VuFlowPort> out;
  std::deque<Macro> macros;
  // 出口上压着、等下游收下的那一段。
  struct Held {
    VuFlowPtr f;
    uint64_t seq = 0;
  };
  std::optional<Held> held;
  uint64_t last_seq = 0, emit_seq = 0, holds = 0;
};

// 五个分组各自流水，汇合后交给 MEXE。
class VuVexeNet : public BachModule {
 public:
  static constexpr int kGroups = 5;

  VuVexeNet(ClockPtr clock, const std::string& name,
            std::array<VuValu*, 3> valu_units, VuVsfu& vsfu_unit,
            uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        valu(valu_units),
        vsfu(vsfu_unit),
        in(std::make_shared<VuFlowPort>(clock)),
        lu_in(std::make_shared<VuFlowPort>(clock)),
        out(std::make_shared<VuFlowPort>(clock)) {}

  VuFlowPort& In() { return *in; }
  void AttachIn(std::shared_ptr<VuFlowPort> p) { in = std::move(p); }
  VuFlowPort& LuIn() { return *lu_in; }
  void AttachLu(std::shared_ptr<VuFlowPort> p) { lu_in = std::move(p); }
  VuFlowPort& Out() { return *out; }
  void AttachOut(std::shared_ptr<VuFlowPort> p) { out = std::move(p); }
  void AttachResultBus(std::shared_ptr<VuResultBus> p) { bus = std::move(p); }

  bool Quiescent() const override {
    return segs.empty() && pending_lu.empty() && ext_inflight.empty() && !held;
  }

 protected:
  void Step() override {
    if (held && out->Ready()) {
      held.reset();
    }
    // 已交给下游、但结果还没被 DMUX / SU / MEXE / SEXE 取完的段继续占着
    // 对应分组的流水槽；全部取完才从表里摘掉。
    for (auto it = ext_inflight.begin(); it != ext_inflight.end();) {
      if ((*it)->ExtPending()) ++it;
      else it = ext_inflight.erase(it);
    }
    TakeLu();
    Tick();
    Start();
    TakeSeg();
    Start();
    PumpResults();
    Emit();
    Charge();
    in->DriveReady(true);
    lu_in->DriveReady(true);
  }

 private:
  enum Phase { kOff, kWait, kRun, kDone };

  struct Seg {
    VuFlowPtr f;
    bool lu_ready = false;
    std::array<Phase, kGroups> phase{};
    std::array<uint64_t, kGroups> left{};
    std::array<uint64_t, kGroups> lat{};
    // 本段每个分组的结果还有几个 VEXE 消费者没取；0 表示可以释放槽位。
    std::array<uint8_t, kGroups> need{};
    std::array<bool, kGroups> consumed{};
    bool simple = false;
    std::array<bool, kGroups> result_sent{};
  };

  static int GroupOf(uint64_t sel) {
    switch (sel) {
      case kSrcValu0: return 0;
      case kSrcValu1: return 1;
      case kSrcValu2: return 2;
      case kSrcVsfu0: return 3;
      case kSrcVsfu1: return 4;
      default: return -1;
    }
  }

  // 本分组 g 用到的其它 VEXE 分组，位图形式。只有 VEXE 内部的源算反压；
  // LU / VRF / MRF / SRF 由上游各自的队列负责。
  static uint64_t SrcMaskOf(VuUops const& u, int g) {
    VuStaticCfg const& c = u.cfg;
    uint64_t mask = 0;
    if (g < 3) {
      ValuOp op = ValuOp(c.valu[g].opcode);
      uint64_t used = ValuSrcUsed(op);
      uint64_t const src[3] = {c.valu[g].src1, c.valu[g].src2, c.valu[g].src3};
      for (int b = 0; b < 3; ++b) {
        if ((used & (1u << b)) == 0) continue;
        int p = GroupOf(src[b]);
        if (p >= 0 && p != g) mask |= 1ull << p;
      }
      return mask;
    }
    int p = GroupOf(c.VsfuSrc(g - 3));
    if (p >= 0 && p != g) mask |= 1ull << p;
    return mask;
  }

  static uint64_t VexeSrcOf(int g) {
    switch (g) {
      case 0: return kSrcValu0;
      case 1: return kSrcValu1;
      case 2: return kSrcValu2;
      case 3: return kSrcVsfu0;
      default: return kSrcVsfu1;
    }
  }

  // 这个分组的结果要被 VEXE 之外的几个消费者取走：DMUX 的 RF 写口、SU、
  // MEXE、SEXE。每个消费者只算一次，和消费者模块里的 Consume() 一一对应。
  static uint64_t ExtConsumerCount(VuUops const& u, int g) {
    VuStaticCfg const& c = u.cfg;
    uint64_t src = VexeSrcOf(g);
    uint64_t n = 0;
    for (uint64_t p = 0; p < 2; ++p) {
      if (c.VrfWtSrc(p) == src) ++n;
    }
    if (c.MrfWtSrc() == src) ++n;
    // SRF 虚拟写口 p1 的来源固定是 VALU1。
    if (g == 1 && (c.SrfWtEn() & (1u << 1)) != 0) ++n;
    if (c.su.Active() && c.su.src1 == src) ++n;
    if (MexeSupports(c.mexe.opcode)) {
      uint64_t used = MexeSrcUsed(c.mexe.opcode);
      if ((used & 1u) != 0 && c.mexe.src1 == src) ++n;
      if ((used & 2u) != 0 && c.mexe.src2 == src) ++n;
    }
    for (uint64_t k = 0; k < 3; ++k) {
      if (!SexeSupports(c.sexe[k].opcode)) continue;
      if (c.sexe[k].src1 == src) ++n;
      if (SexeUsesTwoSrc(c.sexe[k].opcode) && c.sexe[k].src2 == src) ++n;
    }
    return n;
  }

  // 只有“数据目的地全部来自 VEXE 分组、没有 MEXE / SEXE”的宏才走按分组
  // 结果通道；其它宏保留原来的段级流，避免影响 LU / MEXE / SEXE 的目的地。
  static bool SimpleMacro(VuUops const& u) {
    VuStaticCfg const& c = u.cfg;
    if (MexeSupports(c.mexe.opcode)) return false;
    for (uint64_t k = 0; k < 3; ++k) {
      if (SexeSupports(c.sexe[k].opcode)) return false;
    }
    for (uint64_t p = 0; p < 2; ++p) {
      uint64_t src = c.VrfWtSrc(p);
      if (src != kSrcNone && GroupOf(src) < 0) return false;
    }
    uint64_t mrf = c.MrfWtSrc();
    if (mrf != kSrcNone && GroupOf(mrf) < 0) return false;
    // SRF 只有 p1（VALU1）是 VEXE 分组来源。
    if ((c.SrfWtEn() & ~(1u << 1)) != 0) return false;
    if (c.su.Active() && GroupOf(c.su.src1) < 0) return false;
    return true;
  }

  bool On(VuUops const& u, int g) const {
    if (g < 3) return valu[g]->ActiveFor(u);
    if (g == 3) return VuVsfuOn(u.cfg, 0);
    return !u.inst.Bf16() && VuVsfuOn(u.cfg, 1);
  }

  uint64_t LatOf(VuUops const& u, int g) const {
    if (g < 3) return valu[g]->LatencyOf(u);
    return VuLatency::Get().Vsfu(VsfuOp(u.cfg.VsfuOpcode(g - 3)));
  }

  // VALU2 的 vmv.v.v 不是一条 3 级固定延迟的流水，而是一条 16 项的
  // 对齐 FIFO：写进去 1 拍后就能被下游取，最多可攒 16 项；下游 16 拍
  // 都不取才反压 VALU2。其它指令仍按自己的 latency 算深度。
  uint64_t DepthOf(VuUops const& u, int g) const {
    if (g == 2 && ValuOp(u.cfg.valu[2].opcode) == ValuOp::kMvVv) return 16;
    return LatOf(u, g);
  }

  // vmv.v.v 的最短可用延迟是 1 拍；之后由 FIFO 的 16 项深度决定能等多久。
  uint64_t OutLatOf(VuUops const& u, int g) const {
    if (g == 2 && ValuOp(u.cfg.valu[2].opcode) == ValuOp::kMvVv) return 1;
    return LatOf(u, g);
  }

  bool NeedsLu(VuUops const& u, int g) const {
    VuStaticCfg const& c = u.cfg;
    if (g < 3) {
      if (!On(u, g)) return false;
      ValuOp op = ValuOp(c.valu[g].opcode);
      uint64_t used = ValuSrcUsed(op);
      uint64_t const src[3] = {c.valu[g].src1, c.valu[g].src2, c.valu[g].src3};
      for (int b = 0; b < 3; ++b) {
        if ((used & (1u << b)) && src[b] == kSrcLu) return true;
      }
      return !ValuNoMask(op) && c.MaskSelOf(g) == kVuMaskSelLu;
    }
    return On(u, g) && c.VsfuSrc(g - 3) == kSrcLu;
  }

  bool PredsDone(Seg const& s, int g) const {
    VuUops const& u = s.f->uops;
    VuStaticCfg const& c = u.cfg;
    auto wait = [&](uint64_t sel) {
      int p = GroupOf(sel);
      if (p < 0 || p == g) return false;
      return s.phase[p] != kDone && s.phase[p] != kOff;
    };
    if (g < 3) {
      ValuOp op = ValuOp(c.valu[g].opcode);
      uint64_t used = ValuSrcUsed(op);
      uint64_t const src[3] = {c.valu[g].src1, c.valu[g].src2, c.valu[g].src3};
      for (int b = 0; b < 3; ++b) {
        if ((used & (1u << b)) && wait(src[b])) return false;
      }
      return true;
    }
    return !wait(c.VsfuSrc(g - 3));
  }

  // 容量 = 本模块自己的流水级数。既数在算的段，也数算完但消费者还没取的段；
  // 后者不清掉，模块的流水槽就相当于被下游堵住，`CanStart` 会停发。
  bool Room(int g, uint64_t lat) const {
    uint64_t n = 0;
    for (Seg const& s : segs) {
      if (s.phase[g] == kRun) ++n;
      else if (s.phase[g] == kDone && !s.consumed[g]) ++n;
      // 只有已经算出来、还没被外部消费者取走的结果才占槽；段刚收下时
      // ext_need 只是“将要有这么多消费者”，不能提前算进在飞数。
      if (s.phase[g] == kDone) n += s.f->ext_need[g];
    }
    for (VuFlowPtr const& f : ext_inflight) n += f->ext_need[g];
    // 容量 = 本条指令的流水级数 + 1 格弹性槽。多出的一格吸收
    // “本拍算完、下一拍才被下游取走”的握手拍，否则每 N 段空一拍。
    uint64_t cap = lat + 1;
    if (cap < 1) cap = 1;
    return n < cap;
  }

  void Fire(Seg& s, int g) {
    if (g < 3) valu[g]->ComputeNow(*s.f);
    else vsfu.ComputeUnit(g - 3, *s.f);
    // 消费者已经取走这些源，腾出上游模块的流水槽。
    uint64_t src = SrcMaskOf(s.f->uops, g);
    for (int p = 0; p < kGroups; ++p) {
      if ((src & (1ull << p)) == 0) continue;
      if (s.need[p] > 0) --s.need[p];
      if (s.need[p] == 0) s.consumed[p] = true;
    }
    s.lat[g] = OutLatOf(s.f->uops, g);
    if (s.lat[g] == 0) {
      s.phase[g] = kDone;
      return;
    }
    s.phase[g] = kRun;
    s.left[g] = s.lat[g];
  }

  bool CanStart(Seg const& s, int g) const {
    if (s.phase[g] != kWait) return false;
    if (!PredsDone(s, g)) return false;
    if (NeedsLu(s.f->uops, g) && !s.lu_ready) return false;
    return Room(g, DepthOf(s.f->uops, g));
  }

  void Start() {
    bool again = true;
    while (again) {
      again = false;
      for (Seg& s : segs) {
        for (int g = 0; g < kGroups; ++g) {
          if (!CanStart(s, g)) continue;
          Fire(s, g);
          again = true;
        }
      }
    }
  }

  void Tick() {
    for (Seg& s : segs) {
      for (int g = 0; g < kGroups; ++g) {
        if (s.phase[g] != kRun) continue;
        if (s.left[g] > 0) --s.left[g];
        if (s.left[g] == 0) s.phase[g] = kDone;
      }
    }
  }

  static bool SamePiece(VuFlow const& a, VuFlow const& b) {
    return a.uops.inst.seq == b.uops.inst.seq && a.seg == b.seg;
  }

  void Merge(Seg& s, VuFlowPtr const& lu) {
    if (lu->error != 0 && s.f->err_unit == kVuErrUnitNone) {
      s.f->err_unit = lu->err_unit;
    }
    s.f->error |= lu->error;
    s.f->lu = lu->lu;
    s.lu_ready = true;
  }

  void AbsorbPending(Seg& s) {
    for (auto it = pending_lu.begin(); it != pending_lu.end(); ++it) {
      if (!SamePiece(*s.f, **it)) continue;
      Merge(s, *it);
      pending_lu.erase(it);
      return;
    }
  }

  Seg MakeSeg(VuFlowPtr f) {
    Seg s;
    s.f = std::move(f);
    s.lu_ready = !VuLuOn(s.f->uops.cfg);
    for (int g = 0; g < kGroups; ++g) {
      s.phase[g] = On(s.f->uops, g) ? kWait : kOff;
    }
    // 数每个分组的结果有几个 VEXE 消费者；没有 VEXE 消费者的（例如只写
    // VRF / 交给 SU 的最终结果）算已经消费，槽位随流水走完自然释放。
    for (int g = 0; g < kGroups; ++g) {
      if (!On(s.f->uops, g)) continue;
      uint64_t src = SrcMaskOf(s.f->uops, g);
      for (int p = 0; p < kGroups; ++p) {
        if (src & (1ull << p)) ++s.need[p];
      }
    }
    for (int p = 0; p < kGroups; ++p) s.consumed[p] = (s.need[p] == 0);
    s.simple = SimpleMacro(s.f->uops);
    // VEXE 之外的消费者：DMUX 写 RF、SU、MEXE、SEXE。它们取走结果时
    // 调用 f->Consume()，把这里的计数减掉。
    for (int g = 0; g < kGroups; ++g) {
      if (!On(s.f->uops, g)) continue;
      s.f->ext_need[g] = uint8_t(ExtConsumerCount(s.f->uops, g));
    }
    AbsorbPending(s);
    return s;
  }

  void TakeSeg() {
    if (!in->Valid() || in->Seq() == last_seg) return;
    VuFlowPtr f = in->Flow();
    if (!f) return;
    last_seg = in->Seq();
    segs.push_back(MakeSeg(std::move(f)));
  }

  void TakeLu() {
    if (!lu_in->Valid() || lu_in->Seq() == last_lu) return;
    VuFlowPtr f = lu_in->Flow();
    if (!f) return;
    // 已经能并进在飞段的数据不受 pending 上限影响；只有真要排队时才设上限，
    // 否则会静默丢掉一段 LU 数据。
    for (Seg& s : segs) {
      if (!SamePiece(*s.f, *f)) continue;
      Merge(s, f);
      last_lu = lu_in->Seq();
      return;
    }
    last_lu = lu_in->Seq();
    pending_lu.push_back(std::move(f));
  }

  // 把已完成分组的数据目的地按端口送进结果总线；同一分组要写多个端口就
  // 生成多项，各端口独立排队。队列没空位就整体不推，下一拍再试。
  void PumpResults() {
    if (!bus) return;
    for (Seg& s : segs) {
      if (!s.simple) continue;
      for (int g = 0; g < kGroups; ++g) {
        if (s.result_sent[g]) continue;
        if (!On(s.f->uops, g)) {
          s.result_sent[g] = true;
          continue;
        }
        if (s.phase[g] != kDone) continue;
        uint64_t src = VexeSrcOf(g);
        VuStaticCfg const& c = s.f->uops.cfg;
        VuOperand const& data = VuSrcOf(*s.f, src);
        std::vector<VuResult> items;
        auto add = [&](uint8_t dest, uint64_t port) {
          VuResult r;
          r.f = s.f;
          r.group = g;
          r.dest = dest;
          r.port = port;
          r.data = data;
          items.push_back(std::move(r));
        };
        for (uint64_t p = 0; p < 2; ++p) {
          if (c.VrfWtSrc(p) == src) add(uint8_t(kVuDestVrf0 + p), p);
        }
        if (c.MrfWtSrc() == src) add(kVuDestMrf, 0);
        if (g == 1 && (c.SrfWtEn() & (1u << 1)) != 0) {
          add(uint8_t(kVuDestSrf0 + 1), 1);
        }
        if (c.su.Active() && c.su.src1 == src) add(kVuDestSu, 0);
        bool room = true;
        for (VuResult const& r : items) {
          if (!bus->HasRoom(r.dest)) { room = false; break; }
        }
        if (!room) break;
        for (VuResult& r : items) bus->Push(std::move(r));
        for (size_t i = 0; i < items.size(); ++i) s.f->Consume(src);
        s.result_sent[g] = true;
        s.f->result_handled[g] = true;
      }
    }
  }

  static bool Finished(Seg const& s) {
    if (!s.lu_ready) return false;
    for (int g = 0; g < kGroups; ++g) {
      if (s.phase[g] != kDone && s.phase[g] != kOff) return false;
    }
    return true;
  }

  void Emit() {
    if (held) {
      out->Drive(held->f, held->seq);
      return;
    }
    // 简单宏：中间段只负责数据，结果都进了按端口队列后就可以从段级流里
    // 摘掉；只有最后一段留下来退休，并且要等这一宏的结果全部写完。
    if (!segs.empty() && segs.front().simple && Finished(segs.front())) {
      Seg& s = segs.front();
      bool sent = true;
      for (int g = 0; g < kGroups; ++g) {
        if (On(s.f->uops, g) && !s.result_sent[g]) sent = false;
      }
      if (!sent) {
        out->Idle();
        return;
      }
      if (!s.f->seg_last) {
        // 中间段的结果已经全部进了端口队列，段级流不再需要它。
        segs.pop_front();
        return;
      }
      // 最后一段等这一宏的所有结果真正写完再退休。
      if (bus && bus->Pending(s.f->uops.inst.seq) != 0) {
        out->Idle();
        return;
      }
    }
    if (segs.empty() || !Finished(segs.front())) {
      out->Idle();
      return;
    }
    held = Held{segs.front().f, ++emit_seq};
    if (segs.front().f->ExtPending()) ext_inflight.push_back(segs.front().f);
    segs.pop_front();
    out->Drive(held->f, held->seq);
  }

  void Charge() {
    std::array<bool, 3> busy{};
    for (Seg const& s : segs) {
      for (int g = 0; g < 3; ++g) {
        if (s.phase[g] == kRun) busy[g] = true;
      }
    }
    for (int g = 0; g < 3; ++g) {
      if (busy[g]) valu[g]->ChargeBusy();
    }
  }

  std::array<VuValu*, 3> valu;
  VuVsfu& vsfu;
  std::shared_ptr<VuFlowPort> in, lu_in, out;
  std::deque<Seg> segs;
  std::deque<VuFlowPtr> pending_lu;
  // 已经离开 VEXE、但结果还被下游器件取用的段；它们继续占着对应分组的槽。
  std::deque<VuFlowPtr> ext_inflight;
  std::shared_ptr<VuResultBus> bus;
  // 出口上压着、等下游收下的那一段。
  struct Held {
    VuFlowPtr f;
    uint64_t seq = 0;
  };
  std::optional<Held> held;
  uint64_t emit_seq = 0, last_seg = 0, last_lu = 0;
};

}  // namespace bach
}  // namespace latch

#endif
