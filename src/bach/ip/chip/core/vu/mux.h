#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_MUX_
#define _LATCH_BACH_IP_CHIP_CORE_VU_MUX_

// M5 · SMUX 源路由与 M7 · DMUX 结果路由。
//
// 两级夹着执行单元那一段：SMUX 按静态配置给每个单元备好 a、b 与 Mask，DMUX 把
// 各单元的结果送回 RF 或交给 SU。
//
// 执行单元之间的 bypass 与广播不消耗 RF 端口：从产生方的输出直接取。所以
// SMUX 读 RF 的次数只由取 kVrf0 / kVrf1 / kSrf 那几路决定，端口上限在 M3 已查过。
//
// 一条宏指令内多条并行通路经过的执行分组级数不同时，合并点的两个源操作数会不同
// 拍到达。配平是软件的责任：差一级用 VALU2 的 vmv.v.v 对齐，差得多就拆成多条
// 宏指令。硬件不提供软件可见的缓冲队列，所以这两级都不排队。

#include <memory>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/ip/chip/core/vu/regfiles.h"
#include "bach/ip/chip/core/vu/exe_base.h"
#include "bach/ip/chip/core/vu/vu_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class VuSmux : public BachModule {
 public:
  VuSmux(ClockPtr clock, const std::string& name, VuRegfiles& rf,
         uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        regs(rf),
        in(std::make_shared<VuFlowPort>(clock)),
        out(std::make_shared<VuFlowPort>(clock)),
        routed(clock) {}

  VuFlowPort& In() { return *in; }
  void AttachIn(std::shared_ptr<VuFlowPort> p) { in = std::move(p); }
  VuFlowPort& Out() { return *out; }
  void AttachOut(std::shared_ptr<VuFlowPort> p) { out = std::move(p); }

  uint64_t Routed() const { return routed.Get(); }
  bool Quiescent() const override { return !holding; }

 protected:
  void Step() override {
    Drain();
    Accept();
    routed = route_cnt;
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
    in->DriveReady(!holding);
    if (holding || !in->Valid() || in->Seq() == last_seq) {
      if (!holding) out->Idle();
      return;
    }
    VuFlowPtr f = in->Flow();
    if (!f) {
      out->Idle();
      return;
    }
    last_seq = in->Seq();
    Route(*f);
    held = f;
    holding = true;
    out_seq = in->Seq();
    ++route_cnt;
    out->Drive(held, out_seq);
  }

  // 把 RF 上的两个 VRF 读口、两个 MRF 读口与八个 SRF 读口按索引读出来。索引
  // 取静态副本还是动态寄存器由 STATIC_DYNAMIC_MASK 决定，那件事收在
  // VuMacroInst 里。
  //
  // 只有真被某一路 SRC*_SEL 指到的端口才读。端口占用的上限在 M3 已经查过，
  // 这里按需读，读了就是占了。
  void Route(VuFlow& f) {
    VuMacroInst const& inst = f.uops.inst;
    uint64_t vl = inst.Vl();

    for (uint64_t p = 0; p < 2; ++p) {
      if (Uses(f.uops.cfg, kSrcVrfP0 + p)) {
        f.vrf_rd[p].vec = regs.ReadVrf(inst.VrfRd(p), vl, inst.Bf16());
      }
      if (UsesMrf(f.uops.cfg, p)) {
        f.mrf_rd[p].mask = regs.ReadMrf(inst.MrfRd(p), vl, inst.Bf16());
      }
    }
    for (uint64_t p = 0; p < kVuSrfRdPorts; ++p) {
      if (!Uses(f.uops.cfg, kSrcSrfP0 + p)) continue;
      float v = regs.ReadSrf(inst.SrfRd(p));
      // 标量只有 FP32 一种精度；DATA_TYPE=BF16 时标量进向量通路由硬件按
      // ROUND_MODE 自动做 FP32 → BF16 转换，软件无需预先转换。
      if (inst.Bf16()) {
        v = numeric::FromBf16(numeric::NarrowBf16(v, inst.Round()));
      }
      f.srf_rd[p].scalar = v;
      f.srf_rd[p].has_scalar = true;
    }
  }

  // 这一条有没有哪一路源指向这个编码。
  static bool Uses(VuStaticCfg const& c, uint64_t sel) {
    const VuOpReg* regs_list[] = {&c.lu,      &c.su,      &c.valu[0],
                                  &c.valu[1], &c.valu[2], &c.vsfu,
                                  &c.mexe,    &c.sexe[0], &c.sexe[1],
                                  &c.sexe[2]};
    for (VuOpReg const* r : regs_list) {
      if (!r->Active()) continue;
      if (r->src1 == sel || r->src2 == sel || r->src3 == sel) return true;
    }
    return false;
  }

  // MRF 读口的占用方多一类：四个 VEXE 的运算掩码走 mask_op，不走 SRC*_SEL。
  static bool UsesMrf(VuStaticCfg const& c, uint64_t port) {
    if (Uses(c, kSrcMrfP0 + port)) return true;
    uint64_t want = port == 0 ? kVuMaskP0 : kVuMaskP1;
    for (uint64_t v = 0; v < 4; ++v) {
      if (c.MaskSelOf(v) == want) return true;
    }
    return false;
  }

  VuRegfiles& regs;
  std::shared_ptr<VuFlowPort> in, out;
  VuFlowPtr held;
  bool holding = false;
  uint64_t out_seq = 0, last_seq = 0, route_cnt = 0;

  Logic64 routed;
};

class VuDmux : public BachModule {
 public:
  VuDmux(ClockPtr clock, const std::string& name, VuRegfiles& rf,
         uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        regs(rf),
        in(std::make_shared<VuFlowPort>(clock)),
        out(std::make_shared<VuFlowPort>(clock)),
        written(clock) {}

  VuFlowPort& In() { return *in; }
  void AttachIn(std::shared_ptr<VuFlowPort> p) { in = std::move(p); }
  VuFlowPort& Out() { return *out; }
  void AttachOut(std::shared_ptr<VuFlowPort> p) { out = std::move(p); }

  uint64_t Written() const { return written.Get(); }
  bool Quiescent() const override { return !holding; }

 protected:
  void Step() override {
    Drain();
    Accept();
    written = write_cnt;
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
    in->DriveReady(!holding);
    if (holding || !in->Valid() || in->Seq() == last_seq) {
      if (!holding) out->Idle();
      return;
    }
    VuFlowPtr f = in->Flow();
    if (!f) {
      out->Idle();
      return;
    }
    last_seq = in->Seq();
    Writeback(*f);
    held = f;
    holding = true;
    out_seq = in->Seq();
    out->Drive(held, out_seq);
  }

  // PRF_op 描述一条宏指令的全部写回行为：VRF 两个写端口与 MRF 唯一写端口用
  // 「0x00 不写回、非零指定来源」的编码，SRF 六个虚拟写口与产生方硬绑定、
  // 只需逐口使能。
  void Writeback(VuFlow& f) {
    VuStaticCfg const& c = f.uops.cfg;
    VuMacroInst const& inst = f.uops.inst;

    for (uint64_t p = 0; p < 2; ++p) {
      uint64_t src = c.VrfWtSrc(p);
      if (src == kSrcNone) continue;
      VuOperand const& v = VuSrcOf(f, src);
      if (v.vec.empty()) continue;
      regs.WriteVrf(inst.VrfWt(p), v.vec, inst.Bf16(), inst.Round());
      ++write_cnt;
    }

    uint64_t mrf_src = c.MrfWtSrc();
    if (mrf_src != kSrcNone) {
      VuOperand const& m = VuSrcOf(f, mrf_src);
      if (!m.mask.empty()) {
        regs.WriteMrf(inst.MrfWt(), m.mask, inst.Bf16());
        ++write_cnt;
      }
    }

    // 六个 SRF 虚拟写口：bit0 p0(LU ld.s.fp32)、bit1 p1(VALU1 归约 / Top-K)、
    // bit2 p2(MEXE vcpop / vfirst)、bit3～5 p3～p5(SEXE0/1/2)。六个可以在同一条
    // 宏指令内全部使能：它们落在不同的时间窗口上，时分复用同一组写通路。
    uint64_t en = c.SrfWtEn();
    const VuOperand* srf_src[kVuSrfWtPorts] = {
        &f.lu, &f.valu[1], &f.mexe, &f.sexe[0], &f.sexe[1], &f.sexe[2]};
    for (uint64_t p = 0; p < kVuSrfWtPorts; ++p) {
      if ((en & (1u << p)) == 0) continue;
      regs.WriteSrf(inst.SrfWt(p), srf_src[p]->scalar);
      ++write_cnt;
    }

    // SU 的数据源由 SU_op.SRC_SEL 给，不经 RF。
    if (c.su.Active()) f.su_in = VuSrcOf(f, c.su.src1);
  }

  VuRegfiles& regs;
  std::shared_ptr<VuFlowPort> in, out;
  VuFlowPtr held;
  bool holding = false;
  uint64_t out_seq = 0, last_seq = 0, write_cnt = 0;

  Logic64 written;
};

}  // namespace bach
}  // namespace latch

#endif
