#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_PIPE_CTRL_
#define _LATCH_BACH_IP_CHIP_CORE_VU_PIPE_CTRL_

// M3 · pipe_ctrl 展开与 Scoreboard。
//
// 把一条宏指令按静态配置展开成逐单元一条的微指令，同时查它与在飞宏指令之间有
// 没有 RAW / WAR / WAW。追踪的粒度是 VRF / MRF / SRF 上的区间：一条宏指令读哪
// 一段、写哪一段由各端口的索引寄存器与 VL 算得出，重叠就等。
//
// 三件事不归 Scoreboard 管：
//   CM 访存依赖不追踪，有冲突的宏指令之间由软件置 MACRO_INST_FENCE，那一条等
//   此前全部宏指令完成才派发。建模时若默认硬件会挡，结果会偏乐观。
//   含 Vector 数据广播的宏指令置 DATA_BROADCAST，它不参与乱序调度，等除
//   CM-Load 外的前序宏指令全部完成才派发。
//   配平计算依赖树是软件的责任，硬件只提供 bypass 与广播。
//
// 单条宏指令的五类容量上限也在这一级查：CM 1 次 Load + 1 次 Store、VRF 2R2W、
// MRF 2R1W、SRF 8 逻辑读 6 逻辑写、每个执行单元 1 次（SEXE 例外，它是同一物理
// 单元的三次串行迭代）。超出就置 CFG_ERROR。

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/ip/chip/core/vu/config_register.h"
#include "bach/ip/chip/core/vu/vu_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// RF 上的一段。base 与 count 的单位随 RF 变：VRF 是 entry，MRF 是字节，SRF 是
// entry。同一块 RF 内部单位一致，跨块不比。
struct VuSpan {
  uint64_t base = 0;
  uint64_t count = 0;
  bool Overlap(VuSpan const& o) const {
    if (count == 0 || o.count == 0) return false;
    return base < o.base + o.count && o.base < base + count;
  }
};

// 一条宏指令在三块 RF 上的读写足迹。每块可能有几个端口各占一段，所以是一组段
// 而不是一段：VRF 2 读 2 写、MRF 2 读 1 写、SRF 8 读 6 写。
struct VuFootprint {
  std::vector<VuSpan> vrf_rd, vrf_wr;
  std::vector<VuSpan> mrf_rd, mrf_wr;
  std::vector<VuSpan> srf_rd, srf_wr;
  bool cm_load = false;

  static bool Any(std::vector<VuSpan> const& a, std::vector<VuSpan> const& b) {
    for (VuSpan const& x : a) {
      for (VuSpan const& y : b) {
        if (x.Overlap(y)) return true;
      }
    }
    return false;
  }

  bool Conflict(VuFootprint const& o) const {
    // RAW：本条读别人写的；WAR：本条写别人读的；WAW：两条写同一段。
    return Any(vrf_rd, o.vrf_wr) || Any(vrf_wr, o.vrf_rd) ||
           Any(vrf_wr, o.vrf_wr) || Any(mrf_rd, o.mrf_wr) ||
           Any(mrf_wr, o.mrf_rd) || Any(mrf_wr, o.mrf_wr) ||
           Any(srf_rd, o.srf_wr) || Any(srf_wr, o.srf_rd) ||
           Any(srf_wr, o.srf_wr);
  }
};

class VuPipeCtrl : public BachModule {
 public:
  VuPipeCtrl(ClockPtr clock, const std::string& name, VuConfigRegister& cr,
             uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        cfg_reg(cr),
        in(std::make_shared<VuInstPort>(clock)),
        out(std::make_shared<VuUopsPort>(clock)),
        stalls(clock),
        dispatched(clock) {}

  VuInstPort& In() { return *in; }
  void AttachIn(std::shared_ptr<VuInstPort> p) { in = std::move(p); }
  VuUopsPort& Out() { return *out; }
  void AttachOut(std::shared_ptr<VuUopsPort> p) { out = std::move(p); }

  // M9 退休时把这一条的足迹从 Scoreboard 上摘掉。
  void Retire(uint64_t seq) {
    for (auto it = live.begin(); it != live.end(); ++it) {
      if (it->seq == seq) {
        live.erase(it);
        return;
      }
    }
  }

  uint64_t Stalls() const { return stall_cnt; }
  uint64_t Dispatched() const { return dispatch_cnt; }
  // Profile 按成因分开记：Fence 串行化、数据广播约束、Scoreboard 数据依赖、
  // 执行分组结构冒险。
  uint64_t FenceStalls() const { return fence_stall; }
  uint64_t BroadcastStalls() const { return bcast_stall; }
  uint64_t DepStalls() const { return dep_stall; }
  uint64_t EuStalls() const { return eu_stall; }
  // RF 端口被占用的拍数：一条宏指令派发时按它要用的端口各记一拍。
  uint64_t VrfRdBusy(uint64_t port) const { return vrf_rd_busy[port]; }
  uint64_t VrfWtBusy(uint64_t port) const { return vrf_wt_busy[port]; }
  uint64_t MrfWtBusy() const { return mrf_wt_busy; }
  bool Quiescent() const override { return live.empty() && !holding; }

 protected:
  void Step() override {
    Drain();
    Accept();

    stalls = stall_cnt;
    dispatched = dispatch_cnt;
    TracePerCycle("live", live.size());
  }

 private:
  struct LiveInst {
    uint64_t seq = 0;
    VuFootprint fp;
    uint64_t units = 0;
  };

  void Drain() {
    if (!holding) return;
    if (!out->Ready()) {
      out->Drive(held, out_seq);
      return;
    }
    holding = false;
  }

  // ready 表示这一拍真的收下了。被 Scoreboard 或 fence 挡住时不能给 ready ——
  // 上游看见 ready 就换下一条，被挡住的这一条会静默丢掉。
  void Accept() {
    // 还压着一条时端口已经由 Drain 驱动过了，这里再写一次会静默盖掉它 ——
    // 同线程同拍两次写同一个 Latch 不触发断言，那一条就永远发不出去。
    if (holding) {
      in->DriveReady(false);
      return;
    }
    if (!in->Valid()) {
      in->DriveReady(true);
      out->Idle();
      return;
    }
    if (in->Seq() == last_seq) {
      in->DriveReady(true);
      out->Idle();
      return;
    }
    auto inst = in->Inst();
    if (!inst) {
      in->DriveReady(true);
      out->Idle();
      return;
    }

    VuStaticCfg const& cfg = cfg_reg.StaticCfg(inst->cfg_idx);
    VuFootprint fp = FootprintOf(*inst, cfg);

    // MACRO_INST_FENCE：等此前全部宏指令完成才派发。
    if (inst->fence && !live.empty()) return Stall(fence_stall);
    // DATA_BROADCAST：不参与乱序调度，等除 CM-Load 外的前序宏指令全完成。
    if (inst->broadcast) {
      for (auto const& l : live) {
        if (!l.fp.cm_load) return Stall(bcast_stall);
      }
    }
    // Scoreboard：与任何一条在飞的有 RAW / WAR / WAW 就等。
    for (auto const& l : live) {
      if (fp.Conflict(l.fp)) return Stall(dep_stall);
    }
    // 执行分组结构冒险：同一个单元被两条在飞的宏指令同时要。
    for (auto const& l : live) {
      if (l.units & UnitsOf(cfg)) return Stall(eu_stall);
    }

    in->DriveReady(true);
    last_seq = in->Seq();
    auto uops = std::make_shared<VuUops>();
    uops->cfg = cfg;
    uops->inst = *inst;
    Expand(*uops);
    if (!CheckCaps(*uops)) cfg_reg.RaiseError(kVuErrCfg);

    live.push_back({inst->seq, fp, UnitsOf(cfg)});
    // 端口占用按这一条要用的口各记一拍。
    for (uint64_t p = 0; p < 2; ++p) {
      if (UsesSrc(cfg, kSrcVrfP0 + p)) ++vrf_rd_busy[p];
      if (cfg.VrfWtSrc(p) != kSrcNone) ++vrf_wt_busy[p];
    }
    if (cfg.MrfWtSrc() != kSrcNone) ++mrf_wt_busy;
    held = uops;
    holding = true;
    out_seq = inst->seq;
    ++dispatch_cnt;
    out->Drive(held, out_seq);
  }

  void Stall(uint64_t& kind) {
    in->DriveReady(false);
    ++stall_cnt;
    ++kind;
    out->Idle();
  }

  // 这一条要用哪几个执行单元，一位一个。
  static uint64_t UnitsOf(VuStaticCfg const& c) {
    uint64_t m = 0;
    if (c.valu[0].Active()) m |= 1u << 0;
    if (c.valu[1].Active()) m |= 1u << 1;
    if (c.valu[2].Active()) m |= 1u << 2;
    if (c.vsfu.Active()) m |= 1u << 3;
    if (c.mexe.Active()) m |= 1u << 4;
    if (c.sexe[0].Active() || c.sexe[1].Active() || c.sexe[2].Active()) {
      m |= 1u << 5;
    }
    return m;
  }

  // 一条宏指令读写哪几段。VRF / MRF 的占用是从索引起 ⌈VL ÷ 每 entry 元素数⌉
  // 个连续 entry，SRF 一个索引一个 entry。
  static VuFootprint FootprintOf(VuMacroInst const& inst,
                                 VuStaticCfg const& cfg) {
    VuFootprint fp;
    uint64_t entries = inst.Entries();

    for (uint64_t p = 0; p < 2; ++p) {
      if (UsesSrc(cfg, kSrcVrfP0 + p)) {
        fp.vrf_rd.push_back({inst.VrfRd(p), entries});
      }
      if (cfg.VrfWtSrc(p) != kSrcNone) {
        fp.vrf_wr.push_back({inst.VrfWt(p), entries});
      }
      if (UsesMrfPort(cfg, p)) fp.mrf_rd.push_back({inst.MrfRd(p), entries});
    }
    if (cfg.MrfWtSrc() != kSrcNone) fp.mrf_wr.push_back({inst.MrfWt(), entries});

    for (uint64_t p = 0; p < kVuSrfRdPorts; ++p) {
      if (UsesSrc(cfg, kSrcSrfP0 + p)) fp.srf_rd.push_back({inst.SrfRd(p), 1});
    }
    uint64_t en = cfg.SrfWtEn();
    for (uint64_t p = 0; p < kVuSrfWtPorts; ++p) {
      if (en & (1u << p)) fp.srf_wr.push_back({inst.SrfWt(p), 1});
    }

    fp.cm_load = cfg.lu.Active();
    return fp;
  }

  // 这一条有没有哪一路 SRC*_SEL 指向这个编码。
  static bool UsesSrc(VuStaticCfg const& c, uint64_t sel) {
    const VuOpReg* list[] = {&c.lu,      &c.su,      &c.valu[0], &c.valu[1],
                             &c.valu[2], &c.vsfu,    &c.mexe,    &c.sexe[0],
                             &c.sexe[1], &c.sexe[2]};
    for (VuOpReg const* r : list) {
      if (!r->Active()) continue;
      if (r->src1 == sel || r->src2 == sel || r->src3 == sel) return true;
    }
    return false;
  }

  // MRF 读口的占用方多一类：四个 VEXE 的运算掩码走 mask_op，不走 SRC*_SEL。
  static bool UsesMrfPort(VuStaticCfg const& c, uint64_t port) {
    if (UsesSrc(c, kSrcMrfP0 + port)) return true;
    uint64_t want = port == 0 ? kVuMaskP0 : kVuMaskP1;
    for (uint64_t v = 0; v < 4; ++v) {
      if (c.MaskSelOf(v) == want) return true;
    }
    return false;
  }

  // 展开：逐单元一条。未分配与本单元不支持的编码在各执行单元的 Active() 里
  // 归成不动，这里只按 OPCODE 是不是 0 判。
  static void Expand(VuUops& u) {
    VuStaticCfg const& c = u.cfg;
    u.active[uint64_t(VuUnit::kLu)] = c.lu.Active();
    u.active[uint64_t(VuUnit::kSu)] = c.su.Active();
    u.active[uint64_t(VuUnit::kValu0)] = c.valu[0].Active();
    u.active[uint64_t(VuUnit::kValu1)] = c.valu[1].Active();
    u.active[uint64_t(VuUnit::kValu2)] = c.valu[2].Active();
    u.active[uint64_t(VuUnit::kVsfu)] = c.vsfu.Active();
    u.active[uint64_t(VuUnit::kMexe)] = c.mexe.Active();
    u.active[uint64_t(VuUnit::kSexe)] =
        c.sexe[0].Active() || c.sexe[1].Active() || c.sexe[2].Active();
  }

  // 单条宏指令的容量上限与配置合法性。违反置 CFG_ERROR，本条照走 —— 硬件不
  // 阻塞流水。
  static bool CheckCaps(VuUops const& u) {
    VuStaticCfg const& c = u.cfg;

    // MRF 只有 2 个读端口，且一个读端口的数据不能广播给多个消费者：一条宏指令
    // 内引用 MRF 的位置最多 2 处，两处必须分选 p0 与 p1。
    uint64_t mrf_use[2] = {0, 0};
    const VuOpReg* list[] = {&c.lu,      &c.su,      &c.valu[0], &c.valu[1],
                             &c.valu[2], &c.vsfu,    &c.mexe,    &c.sexe[0],
                             &c.sexe[1], &c.sexe[2]};
    for (VuOpReg const* r : list) {
      if (!r->Active()) continue;
      for (uint64_t sel : {uint64_t(r->src1), uint64_t(r->src2),
                           uint64_t(r->src3)}) {
        if (sel == kSrcMrfP0) ++mrf_use[0];
        if (sel == kSrcMrfP1) ++mrf_use[1];
      }
    }
    for (uint64_t v = 0; v < 4; ++v) {
      uint64_t sel = c.MaskSelOf(v);
      if (sel == kVuMaskP0) ++mrf_use[0];
      if (sel == kVuMaskP1) ++mrf_use[1];
      if (sel == 3) return false;   // 11 是 Reserved
    }
    if (mrf_use[0] > 1 || mrf_use[1] > 1) return false;

    // VRF 两个写端口须指向不同的执行单元，来源只能是 LU 或 VALU0/1/2、VSFU；
    // 同时使能时两段区间不得重叠。
    uint64_t w0 = c.VrfWtSrc(0), w1 = c.VrfWtSrc(1);
    for (uint64_t w : {w0, w1}) {
      if (w == kSrcNone) continue;
      if (w != kSrcLu && w != kSrcValu0 && w != kSrcValu1 && w != kSrcValu2 &&
          w != kSrcVsfu) {
        return false;
      }
    }
    if (w0 != kSrcNone && w0 == w1) return false;
    if (w0 != kSrcNone && w1 != kSrcNone) {
      VuSpan a{u.inst.VrfWt(0), u.inst.Entries()};
      VuSpan b{u.inst.VrfWt(1), u.inst.Entries()};
      if (a.Overlap(b)) return false;
    }

    // MRF 唯一写口三选一：LU 的 ld.vm_mask、VALU0 的比较类与 vfclass.mv、MEXE。
    uint64_t mw = c.MrfWtSrc();
    if (mw != kSrcNone && mw != kSrcLu && mw != kSrcValu0 && mw != kSrcMexe) {
      return false;
    }

    // SRF 写口位图的 bit[7:6] 保留，置 1 即非法；同时使能的写口索引必须互不相同。
    uint64_t en = c.SrfWtEn();
    if (en & 0xC0u) return false;
    for (uint64_t p = 0; p < kVuSrfWtPorts; ++p) {
      if ((en & (1u << p)) == 0) continue;
      for (uint64_t q = p + 1; q < kVuSrfWtPorts; ++q) {
        if ((en & (1u << q)) == 0) continue;
        if (u.inst.SrfWt(p) == u.inst.SrfWt(q)) return false;
      }
    }
    // 使能位必须与产生方的 opcode 一致。
    if ((en & (1u << 0)) && LuOp(c.lu.opcode) != LuOp::kLdSFp32) return false;
    if (en & (1u << 1)) {
      ValuOp op = ValuOp(c.valu[1].opcode);
      if (op != ValuOp::kMvFs && (op < ValuOp::kRedusum || op > ValuOp::kSortmin16)) {
        return false;
      }
    }
    if (en & (1u << 2)) {
      MexeOp op = MexeOp(c.mexe.opcode);
      if (op != MexeOp::kCpop && op != MexeOp::kFirst) return false;
    }
    for (uint64_t k = 0; k < 3; ++k) {
      if ((en & (1u << (3 + k))) && !c.sexe[k].Active()) return false;
    }

    // ld.vm_mask 写 MRF 时须置 MRF_WT_SRC = LU；ld.s.fp32 写 SRF 时须置
    // SRF_WT_EN.bit[0]。
    if (LuOp(c.lu.opcode) == LuOp::kLdVmMask && mw != kSrcLu) return false;
    if (LuOp(c.lu.opcode) == LuOp::kLdSFp32 && (en & 1u) == 0) return false;

    // RF 索引不能超出物理范围。
    if (u.inst.VrfRd(0) >= kVuVrfEntry || u.inst.VrfRd(1) >= kVuVrfEntry ||
        u.inst.VrfWt(0) >= kVuVrfEntry || u.inst.VrfWt(1) >= kVuVrfEntry) {
      return false;
    }
    return true;
  }

  VuConfigRegister& cfg_reg;
  std::shared_ptr<VuInstPort> in;
  std::shared_ptr<VuUopsPort> out;

  std::deque<LiveInst> live;
  std::shared_ptr<VuUops> held;
  bool holding = false;
  uint64_t out_seq = 0, last_seq = 0;
  uint64_t stall_cnt = 0, dispatch_cnt = 0;
  uint64_t fence_stall = 0, bcast_stall = 0, dep_stall = 0, eu_stall = 0;
  std::array<uint64_t, 2> vrf_rd_busy{}, vrf_wt_busy{};
  uint64_t mrf_wt_busy = 0;

  Logic64 stalls, dispatched;
};

}  // namespace bach
}  // namespace latch

#endif
