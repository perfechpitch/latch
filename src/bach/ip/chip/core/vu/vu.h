#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_VU_
#define _LATCH_BACH_IP_CHIP_CORE_VU_VU_

// VU DSA 的装配。
//
// 一条宏指令走的路：
//   config_register 收配置写，写 trigger 锁成一条 → ISQ 排队，在飞数不到两条就
//   放行 → pipe_ctrl 展开成逐单元的微指令并查依赖 → LU 从 Core Mem 读入 →
//   SMUX 备好 RF 源 → 执行单元一段 → DMUX 写回 RF 或交给 SU → SU 写回 Core
//   Mem → Retire 报 dsa_done 并释放静态配置组的引用。
//
// 执行单元那一段串成 VALU0 → VALU1 → VALU2 → VSFU → MEXE → SEXE 一条链，本条不
// 动的单元当拍透传。
//
// 十四个模块由装配统一驱动（tick=false）：VRF / MRF / SRF 是存储阵列不是模块，
// SMUX 读、DMUX 写，两级在同一个协程里按固定次序跑，读写顺序确定。tick=true 时
// 各模块各占一个协程，那样两级会同时碰同一块 RF，所以这一档不开放。

#include <memory>
#include <string>

#include "bach/ip/chip/core/vu/config_register.h"
#include "bach/ip/chip/core/vu/isq.h"
#include "bach/ip/chip/core/vu/lu.h"
#include "bach/ip/chip/core/vu/mexe.h"
#include "bach/ip/chip/core/vu/mux.h"
#include "bach/ip/chip/core/vu/pipe_ctrl.h"
#include "bach/ip/chip/core/vu/profile.h"
#include "bach/ip/chip/core/vu/regfiles.h"
#include "bach/ip/chip/core/vu/retire.h"
#include "bach/ip/chip/core/vu/sexe.h"
#include "bach/ip/chip/core/vu/su.h"
#include "bach/ip/chip/core/vu/valu.h"
#include "bach/ip/chip/core/vu/vsfu.h"

namespace latch {
namespace bach {

class Vu {
 public:
  Vu(ClockPtr clock, const std::string& name, uint64_t parent = 0)
      : clk(clock) {
    const uint64_t gid = TraceGroup(name, parent);
    cfg_reg = std::make_unique<VuConfigRegister>(clock, "config", regs,
                                                 gid, false);
    isq = std::make_unique<VuIsq>(clock, "isq", *cfg_reg, gid, false);
    pipe = std::make_unique<VuPipeCtrl>(clock, "pipe_ctrl", *cfg_reg, gid,
                                        false);
    lu = std::make_unique<VuLu>(clock, "lu", gid, false);
    smux = std::make_unique<VuSmux>(clock, "smux", regs, gid, false);
    for (uint64_t i = 0; i < 3; ++i) {
      valu[i] = std::make_unique<VuValu>(clock, std::string("valu") + char('0' + i),
                                         i, gid, false);
    }
    vsfu = std::make_unique<VuVsfu>(clock, "vsfu", gid, false);
    mexe = std::make_unique<VuMexe>(clock, "mexe", gid, false);
    sexe = std::make_unique<VuSexe>(clock, "sexe", gid, false);
    dmux = std::make_unique<VuDmux>(clock, "dmux", regs, gid, false);
    su = std::make_unique<VuSu>(clock, "su", gid, false);
    retire = std::make_unique<VuRetire>(clock, "retire", *cfg_reg, *isq,
                                        *pipe, gid, false);
    profile = std::make_unique<VuProfile>(
        clock, "profile", *cfg_reg, *isq, *pipe, *lu, *su,
        std::array<VuValu*, 3>{valu[0].get(), valu[1].get(), valu[2].get()},
        *vsfu, *mexe, *sexe, gid, false);
    Bind();
  }

  // ── 对外 ──
  DsaCfgPort& Cfg() { return cfg_reg->Path(VuCfgPath::kCore); }
  std::shared_ptr<DsaCfgPort> CfgPtr() const {
    return cfg_reg->PathPtr(VuCfgPath::kCore);
  }
  void AttachCfg(std::shared_ptr<DsaCfgPort> p) {
    cfg_reg->AttachPath(VuCfgPath::kCore, std::move(p));
  }
  // boot 期直接写一组配置，不走三条配置通路。
  void Preload(uint64_t addr, uint64_t data) { cfg_reg->Preload(addr, data); }

  // stream_id 与 task_id 从 VU RV core 的 CSR 直连过来。
  void AttachIds(std::shared_ptr<DsaIdsPort> p) {
    cfg_reg->AttachIds(std::move(p));
  }
  DsaCfgPort& CtrlNoc() { return cfg_reg->Path(VuCfgPath::kCtrlNoc); }
  DsaCfgPort& Debug() { return cfg_reg->Path(VuCfgPath::kDebug); }
  DonePort& Done() { return retire->Done(); }
  void AttachDone(std::shared_ptr<DonePort> p) { retire->AttachDone(std::move(p)); }
  MemPort& CmemLd() { return lu->Cmem(); }
  MemPort& CmemSt() { return su->Cmem(); }
  void AttachCmemLd(std::shared_ptr<MemPort> p) { lu->AttachCmem(std::move(p)); }
  void AttachCmemSt(std::shared_ptr<MemPort> p) { su->AttachCmem(std::move(p)); }

  // ── 观测 ──
  VuRegfiles& Regfiles() { return regs; }
  VuConfigRegister& ConfigRegister() { return *cfg_reg; }
  std::shared_ptr<DsaRdataPort> RdataPtr() const {
    return cfg_reg->RdataPtr();
  }
  VuIsq& Isq() { return *isq; }
  VuPipeCtrl& PipeCtrl() { return *pipe; }
  VuProfile& Profile() { return *profile; }
  VuRetire& Retire() { return *retire; }

  // 末级先做：一条宏指令在一拍里最多前进一级，与逐模块各占协程时逐拍相同。
  void RunStep() {
    // 软件轮询 macro_inst_left 等这一批宏指令做完，条数由这里每拍写进去。
    // 归零的判据是 ISQ 空且 store 落地：离开 SU 的写请求还要经端口到存储，所以
    // 要连着空过两拍。报早了下游按完成往下走，读到的是这一段的旧值。
    bool busy = !isq->Quiescent() || !pipe->Quiescent() || !su->Quiescent();
    if (busy) {
      idle_cycles = 0;
    } else {
      ++idle_cycles;
    }
    cfg_reg->SetLeft(idle_cycles >= 2 ? 0 : isq->Left() + 1);
    profile->RunStep();
    retire->RunStep();
    su->RunStep();
    dmux->RunStep();
    sexe->RunStep();
    mexe->RunStep();
    vsfu->RunStep();
    valu[2]->RunStep();
    valu[1]->RunStep();
    valu[0]->RunStep();
    smux->RunStep();
    lu->RunStep();
    pipe->RunStep();
    isq->RunStep();
    cfg_reg->RunStep();
  }

  bool Quiescent() const {
    return cfg_reg->Quiescent() && isq->Quiescent() && pipe->Quiescent() &&
           lu->Quiescent() && smux->Quiescent() && valu[0]->Quiescent() &&
           valu[1]->Quiescent() && valu[2]->Quiescent() && vsfu->Quiescent() &&
           mexe->Quiescent() && sexe->Quiescent() && dmux->Quiescent() &&
           su->Quiescent();
  }

 private:
  void Bind() {
    auto inst = std::make_shared<VuInstPort>(clk);
    cfg_reg->AttachOut(inst);
    isq->AttachIn(inst);

    auto issued = std::make_shared<VuInstPort>(clk);
    isq->AttachOut(issued);
    pipe->AttachIn(issued);

    auto uops = std::make_shared<VuUopsPort>(clk);
    pipe->AttachOut(uops);
    lu->AttachIn(uops);

    auto chain = [&](auto& up, auto& down) {
      auto p = std::make_shared<VuFlowPort>(clk);
      up->AttachOut(p);
      down->AttachIn(p);
    };
    chain(lu, smux);
    chain(smux, valu[0]);
    chain(valu[0], valu[1]);
    chain(valu[1], valu[2]);
    chain(valu[2], vsfu);
    chain(vsfu, mexe);
    chain(mexe, sexe);
    chain(sexe, dmux);
    chain(dmux, su);
    chain(su, retire);
  }

  ClockPtr clk;
  // 执行通路连着空了几拍，用来判 macro_inst_left 归零。
  uint64_t idle_cycles = 0;
  VuRegfiles regs;
  std::unique_ptr<VuConfigRegister> cfg_reg;
  std::unique_ptr<VuIsq> isq;
  std::unique_ptr<VuPipeCtrl> pipe;
  std::unique_ptr<VuLu> lu;
  std::unique_ptr<VuSmux> smux;
  std::array<std::unique_ptr<VuValu>, 3> valu;
  std::unique_ptr<VuVsfu> vsfu;
  std::unique_ptr<VuMexe> mexe;
  std::unique_ptr<VuSexe> sexe;
  std::unique_ptr<VuDmux> dmux;
  std::unique_ptr<VuSu> su;
  std::unique_ptr<VuRetire> retire;
  std::unique_ptr<VuProfile> profile;
};

}  // namespace bach
}  // namespace latch

#endif
