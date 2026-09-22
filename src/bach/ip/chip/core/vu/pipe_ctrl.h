#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_PIPE_CTRL_
#define _LATCH_BACH_IP_CHIP_CORE_VU_PIPE_CTRL_

// M3 · pipe_ctrl 展开与 Scoreboard。
//
// 把一条宏指令按静态配置展开成逐单元一条的微指令，同时查它与在飞宏指令之间有
// 没有 RAW / WAR / WAW。追踪的粒度是 VRF / MRF / SRF 上的区间：一条宏指令读哪
// 一段、写哪一段由各端口的索引寄存器与 VL 算得出，重叠就等。
//
// 三件事不归 Scoreboard 管：
//   CM 访存依赖不追踪，有冲突的宏指令之间由软件置 MACRO_INST_FENCE（等此前全部
//   宏指令完成）或 CM_FENCE（只等前序的 CM 访问做完）。含 Vector 数据广播的宏
//   指令必须置 MACRO_INST_FENCE：同一个源同时供给两个及以上消费者就是广播，
//   消费者包括执行单元与寄存器堆写端口。
//   配平计算依赖树是软件的责任，硬件只提供 bypass 与广播。
//
// 四条派发阻塞按 fence > cmfence > dep > eu 的优先级归因，一拍只记一项，且只在
// pipe_ctrl 有空位可接收新宏指令的周期记 —— in-flight 已达上限那是满流水，不是
// 停顿。由此得恒等式：有空位周期 = 派发周期 + 四项之和 + ISQ 为空的周期。
//
// 单条宏指令的容量上限与静态配置的合法性也在这一级查（F21 与 error_code 的
// CFG_ERROR 一档）：违反置 CFG_ERROR，本条不执行；RF 索引越过上界只置
// RF_IDX_ERROR，访问在上界内回绕、流水不停滞。

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

// RF 上的一段。base 与 count 的单位随 RF 变：VRF / MRF 是 entry，SRF 是 entry。
// 同一块 RF 内部单位一致，跨块不比。
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
  bool UnitsReady() const { return !in->Valid() || in->Ready(); }
  // Profile 按成因分开记：Fence 串行化、CM_FENCE 等前序 CM 访问、Scoreboard 数据
  // 依赖、执行分组结构冒险。四项相加即「有空位却没派发」的分解。
  uint64_t FenceStalls() const { return fence_stall; }
  uint64_t CmFenceStalls() const { return cmfence_stall; }
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
    bool cm = false;   // 这一条有没有 CM 访问：CM_FENCE 要等的是这些
  };

  void Drain() {
    if (!holding) return;
    if (!out->Ready()) {
      out->Drive(held, out_seq);
      return;
    }
    holding = false;
  }

  // ready 表示这一拍真的收下了。被 Scoreboard 或 fence 挡住时不能给 ready：
  // 上游看见 ready 就换下一条，被挡住的这一条会静默丢掉。
  void Accept() {
    // 还压着一条时端口已经由 Drain 驱动过了，这里再写一次会静默盖掉它。
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

    // 1. MACRO_INST_FENCE：等此前全部宏指令完成才派发。
    if (inst->fence && !live.empty()) return Stall(fence_stall);
    // 2. CM_FENCE：等前序宏指令的 CM 访问做完才派发，纯计算的前序不等待。
    if (inst->cm_fence && AnyCmPending()) return Stall(cmfence_stall);
    // 3. Scoreboard：与任何一条在飞的有 RAW / WAR / WAW 就等。
    for (auto const& l : live) {
      if (fp.Conflict(l.fp)) return Stall(dep_stall);
    }
    // 4. 执行分组结构冒险：同一个单元被两条在飞的宏指令同时要。
    for (auto const& l : live) {
      if (l.units & UnitsOf(cfg)) return Stall(eu_stall);
    }

    in->DriveReady(true);
    last_seq = in->Seq();

    if (!Legal(*inst, cfg)) {
      // 非法配置在调度阶段被拦下：置 CFG_ERROR、不派发给执行单元。MAS 要求
      // 入队时加上的 in-flight / 静态组引用在退休时减掉，所以这里仍发一个
      // 空配置的微指令往下走，LU / SU / RF 都按无操作，M9 照常退休。
      cfg_reg.MarkNotDispatched(inst->seq);
      cfg_reg.ReportError(kVuErrCfg, kVuErrUnitNone, inst.get());
      auto uops = std::make_shared<VuUops>();
      uops->inst = *inst;
      live.push_back({inst->seq, VuFootprint{}, 0, false});
      held = uops;
      holding = true;
      out_seq = inst->seq;
      out->Drive(held, out_seq);
      return;
    }
    CheckIndexRange(*inst, cfg);

    auto uops = std::make_shared<VuUops>();
    uops->cfg = cfg;
    uops->inst = *inst;
    uops->units = UnitsOf(cfg);
    uops->inf_replace = uint32_t(cfg_reg.InfReplaceValue());
    uops->nan_replace = uint32_t(cfg_reg.NanReplaceValue());

    // CM 访问：LU 读或 SU 写都算，CM_FENCE 要等的是这些。
    live.push_back({inst->seq, fp, uops->units, fp.cm_load || VuSuOn(cfg)});
    // 端口占用按这一条要用的口各记一拍。
    for (uint64_t p = 0; p < 2; ++p) {
      if (VuUsesSrc(cfg, kSrcVrfP0 + p)) ++vrf_rd_busy[p];
      if (cfg.VrfWtSrc(p) != kSrcNone) ++vrf_wt_busy[p];
    }
    if (cfg.MrfWtSrc() != kSrcNone) ++mrf_wt_busy;
    held = uops;
    holding = true;
    out_seq = inst->seq;
    ++dispatch_cnt;
    out->Drive(held, out_seq);
  }

  bool AnyCmPending() const {
    for (auto const& l : live) {
      if (l.cm) return true;
    }
    return false;
  }

  void Stall(uint64_t& kind) {
    in->DriveReady(false);
    ++stall_cnt;
    ++kind;
    out->Idle();
  }

  // 这一条要用哪几个执行单元，一位一个（位号见 VuUnit）。
  static uint64_t UnitsOf(VuStaticCfg const& c) {
    uint64_t m = 0;
    if (VuValuOn(c, 0)) m |= 1u << uint64_t(VuUnit::kValu0);
    if (VuValuOn(c, 1)) m |= 1u << uint64_t(VuUnit::kValu1);
    if (VuValuOn(c, 2)) m |= 1u << uint64_t(VuUnit::kValu2);
    if (VuVsfuOn(c, 0)) m |= 1u << uint64_t(VuUnit::kVsfu0);
    if (VuVsfuOn(c, 1)) m |= 1u << uint64_t(VuUnit::kVsfu1);
    if (MexeSupports(c.mexe.opcode)) m |= 1u << uint64_t(VuUnit::kMexe);
    if (SexeSupports(c.sexe[0].opcode) || SexeSupports(c.sexe[1].opcode) ||
        SexeSupports(c.sexe[2].opcode)) {
      m |= 1u << uint64_t(VuUnit::kSexe);
    }
    return m;
  }

  // 这一条读写哪几段。VRF / MRF 的占用是从索引起 ⌈VL ÷ 每 entry 元素数⌉ 个连续
  // entry，SRF 一个索引一个 entry。
  static VuFootprint FootprintOf(VuMacroInst const& inst,
                                 VuStaticCfg const& cfg) {
    VuFootprint fp;
    uint64_t entries = inst.Entries();

    for (uint64_t p = 0; p < 2; ++p) {
      if (VuUsesSrc(cfg, kSrcVrfP0 + p)) {
        fp.vrf_rd.push_back({inst.VrfRd(p), entries});
      }
      if (cfg.VrfWtSrc(p) != kSrcNone) {
        fp.vrf_wr.push_back({inst.VrfWt(p), entries});
      }
      if (VuUsesMrfPort(cfg, p)) fp.mrf_rd.push_back({inst.MrfRd(p), entries});
    }
    if (cfg.MrfWtSrc() != kSrcNone) fp.mrf_wr.push_back({inst.MrfWt(), entries});

    for (uint64_t p = 0; p < kVuSrfRdPorts; ++p) {
      if (VuUsesSrc(cfg, kSrcSrfP0 + p)) fp.srf_rd.push_back({inst.SrfRd(p), 1});
    }
    uint64_t en = cfg.SrfWtEn();
    for (uint64_t p = 0; p < kVuSrfWtPorts; ++p) {
      if (en & (1u << p)) fp.srf_wr.push_back({inst.SrfWt(p), 1});
    }

    fp.cm_load = VuLuOn(cfg);
    return fp;
  }

  // 起始索引 + 占用 entry 数越过 RF 上界：硬件回绕到 entry 0 继续访问以保证流水
  // 不停滞，同时置位 RF_IDX_ERROR，该次宏指令结果不可信。
  void CheckIndexRange(VuMacroInst const& inst, VuStaticCfg const& cfg) {
    uint64_t entries = inst.Entries();
    for (uint64_t p = 0; p < 2; ++p) {
      if (VuUsesSrc(cfg, kSrcVrfP0 + p) &&
          inst.VrfRd(p) + entries > kVuVrfEntry) {
        cfg_reg.ReportError(kVuErrRfIndex, kVuErrUnitVrf, &inst);
      }
      if (cfg.VrfWtSrc(p) != kSrcNone &&
          inst.VrfWt(p) + entries > kVuVrfEntry) {
        cfg_reg.ReportError(kVuErrRfIndex, kVuErrUnitVrf, &inst);
      }
      if (VuUsesMrfPort(cfg, p) && inst.MrfRd(p) + entries > kVuMrfEntry) {
        cfg_reg.ReportError(kVuErrRfIndex, kVuErrUnitMrf, &inst);
      }
    }
    if (cfg.MrfWtSrc() != kSrcNone && inst.MrfWt() + entries > kVuMrfEntry) {
      cfg_reg.ReportError(kVuErrRfIndex, kVuErrUnitMrf, &inst);
    }
  }

  // ── 静态配置的合法性 ──
  //
  // 逐条照《VU-DSA 寄存器整理》的 error_code.CFG_ERROR 一档。判的是「本条用到的
  // 那个单元」的那些字段：编码未分配或本单元不支持的单元按无操作处理，它的全部
  // 字段一并忽略，不参与检查。
  static bool Legal(VuMacroInst const& inst, VuStaticCfg const& c) {
    bool bf16 = inst.Bf16();
    uint32_t lu_op = c.lu.opcode;
    uint32_t su_op = c.su.opcode;
    bool lu_on = VuLuOn(c), su_on = VuSuOn(c);
    bool valu_on[3] = {VuValuOn(c, 0), VuValuOn(c, 1), VuValuOn(c, 2)};
    bool vsfu_on[2] = {VuVsfuOn(c, 0), VuVsfuOn(c, 1)};
    bool mexe_on = MexeSupports(c.mexe.opcode);
    bool sexe_on[3] = {SexeSupports(c.sexe[0].opcode),
                       SexeSupports(c.sexe[1].opcode),
                       SexeSupports(c.sexe[2].opcode)};
    uint64_t vl = inst.Vl();

    // ROUND_MODE = 111 是保留编码。
    if (((inst.TypeVl() >> kVuRoundModeShift) & 0x7u) == 0x7u) return false;

    // 上游单元要使能。指到没使能的执行单元输出就是非法来源。
    auto enabled = [&](uint64_t sel) {
      switch (sel) {
        case kSrcLu: return lu_on;
        case kSrcValu0: return valu_on[0];
        case kSrcValu1: return valu_on[1];
        case kSrcValu2: return valu_on[2];
        case kSrcVsfu0: return vsfu_on[0];
        case kSrcVsfu1: return vsfu_on[1];
        case kSrcMexe: return mexe_on;
        case kSrcSexe0: return sexe_on[0];
        case kSrcSexe1: return sexe_on[1];
        case kSrcSexe2: return sexe_on[2];
        default: return true;   // RF 的口由各自的规则管
      }
    };

    // ── LU ──
    if (lu_on) {
      // ld.mask 要写 MRF 时那个写口只能给 LU；不写回（MRF_WT_SRC = 0x00）也成立，
      // 掩码可以只走 bypass 给 VALU / MEXE。指向 VALU0 / MEXE 才是冲突。
      if (lu_op == 0x05u && c.MrfWtSrc() != kSrcNone &&
          c.MrfWtSrc() != kSrcLu) {
        return false;
      }
      if (lu_op == 0x06u && (c.SrfWtEn() & 1u) == 0) return false;
      // MXFP8 访存与间隔访问都要求 VL 是 32 的整数倍。
      bool stride = (lu_op >= 0x01u && lu_op <= 0x04u) &&
                    ((c.lu.raw >> 8) & 0xFFu) != 0 &&
                    ((c.lu.raw >> 16) & 0xFFu) != 0;
      if ((lu_op == 0x02u || stride) && vl % 32 != 0) return false;
    }

    // ── SU ──
    if (su_on) {
      uint64_t sel = c.su.src1;
      if (sel == kSrcLu) {
        // 三级级联的直通要求两侧数据类型一致。
        if (su_op != lu_op) return false;
      } else if (su_op == 0x05u) {              // st.mask
        if (sel != kSrcValu0 && sel != kSrcMexe && sel != kSrcMrfP0 &&
            sel != kSrcMrfP1) {
          return false;
        }
        if (vl % 8 != 0) return false;
      } else if (su_op == 0x06u) {              // st.s.fp32
        if (sel != kSrcValu1 && sel != kSrcMexe && sel != kSrcSexe0 &&
            sel != kSrcSexe1 && sel != kSrcSexe2 && sel != kSrcSrfP0) {
          return false;
        }
      } else {                                  // 向量写出
        if (sel != kSrcValu0 && sel != kSrcValu1 && sel != kSrcValu2 &&
            sel != kSrcVsfu0 && !(sel == kSrcVsfu1 && !bf16)) {
          return false;
        }
        if (sel == kSrcVsfu1 && bf16) return false;
        if (su_op == 0x02u && vl % 32 != 0) return false;   // st.mxfp8
      }
      if (!enabled(sel)) return false;
    }

    // ── VALU ──
    for (uint64_t i = 0; i < 3; ++i) {
      if (!valu_on[i]) continue;
      ValuOp op = ValuOp(c.valu[i].opcode);
      uint64_t used = ValuSrcUsed(op);
      uint64_t const f[3] = {c.valu[i].src1, c.valu[i].src2, c.valu[i].src3};
      for (uint64_t b = 0; b < 3; ++b) {
        if ((used & (1u << b)) == 0) continue;
        uint64_t sel = f[b];
        if (sel == 0x00u) continue;
        // 三个 VALU 只有 src1 允许取 SRF，取的是本 VALU 硬连线的那个读端口。
        if (b == 0 && sel == 0x51u + i) continue;
        if (sel == kSrcVrfP0 || sel == kSrcVrfP1) continue;
        if (sel == kSrcLu || (sel >= 0x02u && sel <= 0x06u)) {
          if (sel == uint64_t(kSrcValu0) + i) return false;   // 不可回环
          if (sel == kSrcVsfu1 && bf16) return false;
          if (!enabled(sel)) return false;
          continue;
        }
        return false;   // 其余编码非法
      }
      // 掩码来源：不支持的指令忽略这个字段。
      if (!ValuNoMask(op)) {
        uint64_t sel = c.MaskSelOf(i);
        if (sel == kVuMaskSelLu) {
          if (lu_op != 0x05u) return false;
        } else if (sel != kVuMaskSelNone && sel != kVuMaskSelMrfP0 &&
                   sel != kVuMaskSelMrfP1) {
          return false;
        }
      }
      // vswap2.v 按相邻偶奇对交换，VL 为奇数时最后一个 element 没有配对者。
      if (op == ValuOp::kSwap2 && vl % 2 != 0) return false;
    }

    // ── VSFU ──
    for (uint64_t which = 0; which < 2; ++which) {
      if (!vsfu_on[which]) continue;
      uint64_t sel = c.VsfuSrc(which);
      if (sel == 0x00u) continue;
      if (sel == uint64_t(kSrcVsfu0) + which) return false;   // 不可回环
      if (sel == kSrcVsfu1 && bf16) return false;
      if (sel != kSrcLu && !(sel >= 0x02u && sel <= 0x06u) &&
          sel != kSrcVrfP0 && sel != kSrcVrfP1) {
        return false;
      }
      if (!enabled(sel)) return false;
    }
    // 两个 VSFU 之间可以单向串联，但不能互相回环。
    if (vsfu_on[0] && vsfu_on[1] && c.VsfuSrc(0) == kSrcVsfu1 &&
        c.VsfuSrc(1) == kSrcVsfu0) {
      return false;
    }

    // ── MEXE ──
    if (mexe_on) {
      uint64_t used = MexeSrcUsed(c.mexe.opcode);
      uint64_t const f[2] = {c.mexe.src1, c.mexe.src2};
      for (uint64_t b = 0; b < 2; ++b) {
        if ((used & (1u << b)) == 0) continue;
        uint64_t sel = f[b];
        if (sel == kSrcLu) {
          if (lu_op != 0x05u) return false;
        } else if (c.mexe.opcode == 0x15u || c.mexe.opcode == 0x16u) {
          // 索引向量只从 VALU1 的 Top-K 输出或 VRF 里取。
          if (b == 1 && sel != kSrcValu1 && sel != kSrcVrfP0 &&
              sel != kSrcVrfP1) {
            return false;
          }
          if (b == 0 && sel != kSrcValu0 && sel != kSrcMrfP0 &&
              sel != kSrcMrfP1 && sel != kSrcLu) {
            return false;
          }
        } else if (sel != kSrcValu0 && sel != kSrcMrfP0 && sel != kSrcMrfP1) {
          return false;
        }
        if (!enabled(sel)) return false;
      }
    }

    // ── SEXE ──
    //
    // 三个 slot 是同一物理单元的串行迭代：链首 SEXE0 的两个源各自独立（SRF 的
    // p4/p5、VALU1 的归约输出、或 LU 的 ld.s.fp32）；SEXE1 取 SEXE0 的输出与
    // SRF_rd_p6，SEXE2 取 SEXE1 的输出与 SRF_rd_p7。后两个 slot 各只有 1 个 SRF
    // 读端口，所以两个操作数中最多 1 个取自 SRF，且至少 1 个取自前一次迭代。
    for (uint64_t k = 0; k < 3; ++k) {
      if (!sexe_on[k]) continue;
      bool two = SexeUsesTwoSrc(c.sexe[k].opcode);
      uint64_t s1 = c.sexe[k].src1;
      uint64_t s2 = c.sexe[k].src2;
      uint64_t srf = k == 0 ? kSrcSrfP0 + 4 : kSrcSrfP0 + 5 + k;
      uint64_t srf2 = k == 0 ? kSrcSrfP0 + 5 : kSrcSrfP0 + 5 + k;
      uint64_t prev = k == 1 ? kSrcSexe0 : kSrcSexe1;
      uint64_t other = k == 0 ? kSrcValu1 : prev;
      auto legal1 = [&](uint64_t sel) {
        return sel == kSrcLu ? lu_op == 0x06u
                             : (sel == srf || sel == other);
      };
      auto legal2 = [&](uint64_t sel) {
        if (sel == 0x00u) return k == 0;   // 只有链首可用空操作数
        return sel == kSrcLu ? lu_op == 0x06u : (sel == srf2 || sel == other);
      };
      if (!legal1(s1)) return false;
      if (two && !legal2(s2)) return false;
      if (k > 0 && two) {
        if (s1 == srf && s2 == srf) return false;   // 只 1 个 SRF 读端口
        if (s1 != prev && s2 != prev) return false;
      }
      if (!enabled(s1)) return false;
      if (two && s2 != 0x00u && s2 != kSrcLu && !enabled(s2)) return false;
    }

    // ── PRF_op：一条宏指令的全部写回行为 ──
    uint64_t w0 = c.VrfWtSrc(0), w1 = c.VrfWtSrc(1);
    for (uint64_t w : {w0, w1}) {
      if (w == kSrcNone) continue;
      if (w == kSrcVsfu1 && bf16) return false;
      if (w != kSrcLu && w != kSrcValu0 && w != kSrcValu1 && w != kSrcValu2 &&
          w != kSrcVsfu0 && w != kSrcVsfu1) {
        return false;
      }
      if (!enabled(w)) return false;
    }
    // 两个写端口须指向不同执行单元，同时使能时写区间不得重叠。
    if (w0 != kSrcNone && w0 == w1) return false;
    if (w0 != kSrcNone && w1 != kSrcNone) {
      VuSpan a{inst.VrfWt(0), inst.Entries()};
      VuSpan b{inst.VrfWt(1), inst.Entries()};
      if (a.Overlap(b)) return false;
    }

    // MRF 只有 1 个写端口：来源三选一，且要与产生方的 opcode 对得上。
    uint64_t mw = c.MrfWtSrc();
    if (mw != kSrcNone) {
      if (mw == kSrcLu) {
        if (lu_op != 0x05u) return false;
      } else if (mw == kSrcValu0) {
        if (!valu_on[0]) return false;
        uint32_t op = c.valu[0].opcode;
        if (!((op >= 0x50u && op <= 0x59u) || op == 0x60u)) return false;
      } else if (mw == kSrcMexe) {
        if (!mexe_on) return false;
        uint32_t op = c.mexe.opcode;
        if (!((op >= 0x01u && op <= 0x08u) || (op >= 0x12u && op <= 0x16u))) {
          return false;
        }
      } else {
        return false;
      }
    }

    // SRF 6 个虚拟写口：bit[7:6] 保留，使能位要与产生方的 opcode 一致，同时
    // 使能的写口索引必须互不相同。
    uint64_t en = c.SrfWtEn();
    if (en & 0xC0u) return false;
    if ((en & (1u << 0)) && lu_op != 0x06u) return false;
    if (en & (1u << 1)) {
      if (!valu_on[1]) return false;
      uint32_t op = c.valu[1].opcode;
      if (op != 0x22u && !(op >= 0x70u && op <= 0x74u)) return false;
    }
    if (en & (1u << 2)) {
      if (!mexe_on || (c.mexe.opcode != 0x10u && c.mexe.opcode != 0x11u)) {
        return false;
      }
    }
    for (uint64_t k = 0; k < 3; ++k) {
      if ((en & (1u << (3 + k))) && !sexe_on[k]) return false;
    }
    for (uint64_t p = 0; p < kVuSrfWtPorts; ++p) {
      if ((en & (1u << p)) == 0) continue;
      for (uint64_t q = p + 1; q < kVuSrfWtPorts; ++q) {
        if ((en & (1u << q)) == 0) continue;
        if (inst.SrfWt(p) == inst.SrfWt(q)) return false;
      }
    }

    // MRF 只有 2 个读端口，掩码数据不能广播：一个读端口只服务一个消费者，两个
    // 不同的消费者必须分别选 p0 与 p1。MEXE 的两个操作数取相同编码时算 1 个。
    for (uint64_t port = 0; port < 2; ++port) {
      uint64_t want = port == 0 ? kVuMaskSelMrfP0 : kVuMaskSelMrfP1;
      uint64_t users = 0;
      for (uint64_t i = 0; i < 3; ++i) {
        if (VuValuOn(c, i) && !ValuNoMask(ValuOp(c.valu[i].opcode)) &&
            c.MaskSelOf(i) == want) {
          ++users;
        }
      }
      if (mexe_on) {
        // MEXE 的两个源选择按去重后的端口计数。
        if (c.mexe.src1 == want && (MexeSrcUsed(c.mexe.opcode) & 0b01)) ++users;
        if (c.mexe.src2 == want && (MexeSrcUsed(c.mexe.opcode) & 0b10)) ++users;
      }
      if (su_on && su_op == 0x05u && c.su.src1 == want) ++users;
      if (users > 1) return false;
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
  uint64_t fence_stall = 0, cmfence_stall = 0, dep_stall = 0, eu_stall = 0;
  std::array<uint64_t, 2> vrf_rd_busy{}, vrf_wt_busy{};
  uint64_t mrf_wt_busy = 0;

  Logic64 stalls, dispatched;
};

}  // namespace bach
}  // namespace latch

#endif
