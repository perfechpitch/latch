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
#include <string>

#include "bach/ip/chip/core/vu/mux.h"
#include "bach/ip/chip/core/vu/valu.h"
#include "bach/ip/chip/core/vu/vsfu.h"
#include "bach/ip/chip/core/vu/vu_latency.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 一条宏指令拆成段，每拍送一段。RF 在这一拍读，与 LU 的读并行。
class VuIssue : public BachModule {
 public:
  VuIssue(ClockPtr clock, const std::string& name, VuSmux& mux,
          uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        smux(mux),
        in(std::make_shared<VuUopsPort>(clock)),
        out(std::make_shared<VuFlowPort>(clock)) {}

  VuUopsPort& In() { return *in; }
  void AttachIn(std::shared_ptr<VuUopsPort> p) { in = std::move(p); }
  VuFlowPort& Out() { return *out; }
  void AttachOut(std::shared_ptr<VuFlowPort> p) { out = std::move(p); }

  bool Quiescent() const override {
    return macros.empty() && !holding;
  }

 protected:
  void Step() override {
    // 上一拍送出的段，下游这拍已经看过。ready 为真就松开，再发下一段。
    if (holding && out->Ready()) {
      holding = false;
      held.reset();
    }
    Take();
    if (!holding) Pump();
    if (holding) out->Drive(held, out_seq);
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
    smux.LoadRf(*f);
    held = std::move(f);
    holding = true;
    out_seq = ++emit_seq;
    ++m.next;
    if (m.next >= m.total) macros.pop_front();
  }

  VuSmux& smux;
  std::shared_ptr<VuUopsPort> in;
  std::shared_ptr<VuFlowPort> out;
  std::deque<Macro> macros;
  VuFlowPtr held;
  bool holding = false;
  uint64_t out_seq = 0, last_seq = 0, emit_seq = 0;
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

  bool Quiescent() const override {
    return segs.empty() && pending_lu.empty() && !holding;
  }

 protected:
  void Step() override {
    if (holding && out->Ready()) {
      holding = false;
      held.reset();
    }
    TakeLu();
    Tick();
    Start();
    TakeSeg();
    Start();
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

  bool On(VuUops const& u, int g) const {
    if (g < 3) return valu[g]->ActiveFor(u);
    if (g == 3) return VuVsfuOn(u.cfg, 0);
    return !u.inst.Bf16() && VuVsfuOn(u.cfg, 1);
  }

  uint64_t LatOf(VuUops const& u, int g) const {
    if (g < 3) return valu[g]->LatencyOf(u);
    return VuLatency::Get().Vsfu(VsfuOp(u.cfg.VsfuOpcode(g - 3)));
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

  bool Room(int g, uint64_t lat) const {
    uint64_t n = 0;
    for (Seg const& s : segs) {
      if (s.phase[g] == kRun) ++n;
    }
    uint64_t cap = lat + 1;
    if (cap < 1) cap = 1;
    return n < cap;
  }

  void Fire(Seg& s, int g) {
    if (g < 3) valu[g]->ComputeNow(*s.f);
    else vsfu.ComputeUnit(g - 3, *s.f);
    s.lat[g] = LatOf(s.f->uops, g);
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
    return Room(g, LatOf(s.f->uops, g));
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
    last_lu = lu_in->Seq();
    for (Seg& s : segs) {
      if (!SamePiece(*s.f, *f)) continue;
      Merge(s, f);
      return;
    }
    pending_lu.push_back(std::move(f));
  }

  static bool Finished(Seg const& s) {
    if (!s.lu_ready) return false;
    for (int g = 0; g < kGroups; ++g) {
      if (s.phase[g] != kDone && s.phase[g] != kOff) return false;
    }
    return true;
  }

  void Emit() {
    if (holding) {
      out->Drive(held, out_seq);
      return;
    }
    if (segs.empty() || !Finished(segs.front())) {
      out->Idle();
      return;
    }
    held = segs.front().f;
    segs.pop_front();
    holding = true;
    out_seq = ++emit_seq;
    out->Drive(held, out_seq);
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
  VuFlowPtr held;
  bool holding = false;
  uint64_t out_seq = 0, emit_seq = 0, last_seg = 0, last_lu = 0;
};

}  // namespace bach
}  // namespace latch

#endif
