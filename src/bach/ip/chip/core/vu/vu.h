#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_VU_
#define _LATCH_BACH_IP_CHIP_CORE_VU_VU_

// VU DSA 的装配。
//
// 一条宏指令走的路：
//   config_register 收配置写，写 trigger 锁成一条 → ISQ 排队，在飞数不到两条就
//   放行 → pipe_ctrl 展开成逐单元的微指令并查依赖 → LU 从 Core Mem 读入，同时
//   把微指令交给发射级按段读 RF → VEXE 五个分组按 src_sel 各自流水、汇合后，
//   本条用到 MEXE / SEXE 才进入那一级，没有的级整级跳过 → DMUX 写回 RF 或交给
//   SU → SU 写回 Core Mem → Retire 报 dsa_done 并释放静态配置组的引用。
//
// 不读 LU 的分组不用等 CM 响应。MEXE 与 SEXE 仍在 VEXE 之后；本条没有配的那
// 一级不进入，不加握手拍。
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
#include "bach/ip/chip/core/vu/tail_route.h"
#include "bach/ip/chip/core/vu/valu.h"
#include "bach/ip/chip/core/vu/vexe_net.h"
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
    lu->BindScoreboard(*pipe);
    smux = std::make_unique<VuSmux>(clock, "smux", regs, gid, false);
    for (uint64_t i = 0; i < 3; ++i) {
      valu[i] = std::make_unique<VuValu>(clock, std::string("valu") + char('0' + i),
                                         i, gid, false);
    }
    vsfu = std::make_unique<VuVsfu>(clock, "vsfu", gid, false);
    issue = std::make_unique<VuIssue>(clock, "issue", *smux, *pipe, gid, false);
    net = std::make_unique<VuVexeNet>(
        clock, "vexe",
        std::array<VuValu*, 3>{valu[0].get(), valu[1].get(), valu[2].get()},
        *vsfu, gid, false);
    mexe = std::make_unique<VuMexe>(clock, "mexe", gid, false);
    sexe = std::make_unique<VuSexe>(clock, "sexe", gid, false);
    route = std::make_unique<VuTailRoute>(clock, "tail", gid, false);
    dmux = std::make_unique<VuDmux>(clock, "dmux", regs, *pipe, gid, false);
    su = std::make_unique<VuSu>(clock, "su", gid, false);
    retire = std::make_unique<VuRetire>(clock, "retire", *cfg_reg, *isq,
                                        *pipe, gid, false);
    profile = std::make_unique<VuProfile>(
        clock, "profile", *cfg_reg, *isq, *pipe, *lu, *su,
        std::array<VuValu*, 3>{valu[0].get(), valu[1].get(), valu[2].get()},
        *vsfu, *mexe, *sexe, gid, false);
    // 配置总线读 Profile 计数器的窗口。快照窗口由 ISQ 自己挂（它建得比这一级
    // 早，构造函数里就接上了）。
    cfg_reg->AttachCounterWindow(profile.get());
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
  uint64_t IssueHolds() const { return issue->Holds(); }
  VuValu& Valu(uint64_t i) { return *valu[i]; }
  VuProfile& Profile() { return *profile; }
  VuRetire& Retire() { return *retire; }

  // 末级先做：一条宏指令在一拍里最多前进一级，与逐模块各占协程时逐拍相同。
  void RunStep() {
    // macro_inst_left 由这里每拍写进去。归零的判据是 ISQ 空且 store 落地：离开
    // SU 的写请求还要经端口到存储，所以要连着空过两拍。kernel 多宏任务不再轮询
    // 它，完成改由最后一条的 EVENT_EN 报 dsa_done。
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
    // Pre 先放开 ready，VEXE 本拍就能把段交出来；Post 再按本条有没有 MEXE / SEXE
    // 送到下一级。下一级本拍已经跑过，下一拍才看见。
    route->Pre();
    net->RunStep();
    route->Post();
    issue->RunStep();
    lu->RunStep();
    pipe->RunStep();
    isq->RunStep();
    cfg_reg->RunStep();
  }

  bool Quiescent() const {
    return cfg_reg->Quiescent() && isq->Quiescent() && pipe->Quiescent() &&
           lu->Quiescent() && issue->Quiescent() && net->Quiescent() &&
           route->Quiescent() && mexe->Quiescent() && sexe->Quiescent() &&
           dmux->Quiescent() && su->Quiescent();
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
    auto handoff = std::make_shared<VuUopsPort>(clk);
    lu->AttachIssued(handoff);
    issue->AttachIn(handoff);

    auto segs = std::make_shared<VuFlowPort>(clk);
    issue->AttachOut(segs);
    net->AttachIn(segs);

    auto lu_data = std::make_shared<VuFlowPort>(clk);
    lu->AttachOut(lu_data);
    net->AttachLu(lu_data);

    auto vexe_out = std::make_shared<VuFlowPort>(clk);
    net->AttachOut(vexe_out);
    route->AttachVexe(vexe_out);
    auto to_mexe = std::make_shared<VuFlowPort>(clk);
    route->AttachToMexe(to_mexe);
    mexe->AttachIn(to_mexe);
    auto mexe_out = std::make_shared<VuFlowPort>(clk);
    mexe->AttachOut(mexe_out);
    route->AttachMexe(mexe_out);
    auto to_sexe = std::make_shared<VuFlowPort>(clk);
    route->AttachToSexe(to_sexe);
    sexe->AttachIn(to_sexe);
    auto sexe_out = std::make_shared<VuFlowPort>(clk);
    sexe->AttachOut(sexe_out);
    route->AttachSexe(sexe_out);
    auto to_dmux = std::make_shared<VuFlowPort>(clk);
    route->AttachToDmux(to_dmux);
    dmux->AttachIn(to_dmux);
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
  std::unique_ptr<VuIssue> issue;
  std::unique_ptr<VuVexeNet> net;
  std::unique_ptr<VuTailRoute> route;
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
