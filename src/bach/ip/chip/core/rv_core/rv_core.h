#ifndef _LATCH_BACH_IP_CHIP_CORE_RV_CORE_RV_CORE_
#define _LATCH_BACH_IP_CHIP_CORE_RV_CORE_RV_CORE_

// RV core 的装配：TS 与 DSA 之间的桥梁。
//
// 从 TS 收 task，按 task_pc 跑 ITCM 里的 kernel，配置 DSA 执行任务，任务发出去
// 之后立刻交还自己 —— 不等 DSA 执行完，那是「一个 task 的共同形状」里的一步。
//
// 五个独立打拍的模块：
//   task_queue  提前接 TS 下发的 task，前一个做完立刻起队头那个
//   指令执行器   src/rv32 逐条跑完，gpr 就绪表管访存与 DSA 读的延迟
//   dsa_iss     每拍最多下发一条配置或 trigger，写会被反压、读不会
//   dsa_rq      8 项，按序记已下发的读指令，返回时按序写回 gpr
//   lsq         按地址范围分流到 DTCM、Share Mem、Core Mem 与 Router I/O reg
//
// 五个模块由装配统一驱动：外层每拍调一次 RunStep()，按末级先做的次序走一遍，
// 整个 RV core 只占外层那一个协程。task_queue 与执行器之间除了端口还有一处直接
// 调用（起 task 时装身份），所以这一档不留开关。

#include <memory>
#include <string>

#include "bach/ip/chip/core/rv_core/dsa_iss.h"
#include "bach/ip/chip/core/rv_core/dsa_rq.h"
#include "bach/ip/chip/core/rv_core/exec.h"
#include "bach/ip/chip/core/rv_core/lsq.h"
#include "bach/ip/chip/core/rv_core/task_queue.h"

namespace latch {
namespace bach {

class RvCore {
 public:
  RvCore(ClockPtr clock, const std::string& name, RvUnit which,
         uint64_t parent = 0)
      : clk(clock), unit(which) {
    const uint64_t gid = TraceGroup(name, parent);
    tq = std::make_unique<RvTaskQueue>(clock, "task_q", gid, false);
    exec = std::make_unique<RvExec>(clock, "exec", which, gid, false);
    iss = std::make_unique<DsaIss>(clock, "dsa_iss", gid, false);
    rq = std::make_unique<DsaRq>(clock, "dsa_rq", gid, false);
    // cm_lsq 只有 DTE core 有。
    lsq = std::make_unique<RvLsq>(clock, "lsq", which == RvUnit::kDte,
                                  gid, false);
    Bind();
  }

  // ── 对外 ──
  TaskCmdPort& Cmd() { return tq->Cmd(); }
  void AttachCmd(std::shared_ptr<TaskCmdPort> p) { tq->AttachCmd(std::move(p)); }
  DonePort& Done() { return exec->Done(); }
  void AttachDone(std::shared_ptr<DonePort> p) { exec->AttachDone(std::move(p)); }
  DsaCfgPort& DsaCfg() { return iss->Cfg(); }
  void AttachDsaCfg(std::shared_ptr<DsaCfgPort> p) {
    iss->AttachCfg(std::move(p));
  }
  // 四个身份信号直连本核那个 DSA，不握手。
  std::shared_ptr<DsaIdsPort> DsaIdsPtr() const { return exec->DsaIdsPtr(); }
  DsaRdataPort& DsaRdata() { return rq->Rdata(); }
  void AttachDsaRdata(std::shared_ptr<DsaRdataPort> p) {
    rq->AttachRdata(std::move(p));
  }
  MemPort& Smem() { return lsq->Smem(); }
  void AttachSmem(std::shared_ptr<MemPort> p) { lsq->AttachSmem(std::move(p)); }
  MemPort& Cmem() { return lsq->Cmem(); }
  void AttachCmem(std::shared_ptr<MemPort> p) { lsq->AttachCmem(std::move(p)); }
  MemPort& Hdr() { return lsq->Hdr(); }
  void AttachHdr(std::shared_ptr<MemPort> p) { lsq->AttachHdr(std::move(p)); }

  // boot 期由 ctrl_noc 装载 ITCM 与 DTCM。
  void LoadImage(std::string const& path) { exec->LoadImage(path); }
  void PokeItcm(uint64_t addr, std::vector<uint8_t> const& bytes) {
    exec->PokeItcm(addr, bytes);
  }

  // ── 观测 ──
  RvTaskQueue& TaskQueue() { return *tq; }
  RvExec& Exec() { return *exec; }
  DsaIss& Iss() { return *iss; }
  DsaRq& Rq() { return *rq; }
  RvLsq& Lsq() { return *lsq; }
  // 进 wait 状态后拉高，core 把三个 RV core 的这一位与起来给 SCP。
  bool Ready() const { return exec->AtWait(); }

  // 末级先做：一条指令在一拍里最多前进一级。
  void RunStep() {
    rq->RunStep();
    lsq->RunStep();
    iss->RunStep();
    exec->RunStep();
    tq->RunStep();
  }

  bool Quiescent() const {
    return tq->Quiescent() && exec->Quiescent() && iss->Quiescent() &&
           rq->Quiescent() && lsq->Quiescent();
  }

 private:
  void Bind() {
    // task_queue ↔ 执行器：起 task 与交还自己各一根线。
    exec->AttachStart(tq->StartPtr());
    exec->AttachFinish(tq->FinishPtr());
    // 执行器 → dsa_iss → DSA；dsa_iss 把读指令的目的寄存器交给 dsa_rq 记着。
    iss->ReqPtr();
    exec->AttachDsaReq(iss->ReqPtr());
    rq->AttachIssue(iss->ReadIssuePtr());
    exec->AttachDsaWb(rq->WbPtr());
    // 执行器 → lsq → 三块存储；lsq 回来时写回 gpr。
    exec->AttachLsqReq(lsq->ReqPtr());
    exec->AttachLsqWb(lsq->WbPtr());
  }

  ClockPtr clk;
  RvUnit unit;
  std::unique_ptr<RvTaskQueue> tq;
  std::unique_ptr<RvExec> exec;
  std::unique_ptr<DsaIss> iss;
  std::unique_ptr<DsaRq> rq;
  std::unique_ptr<RvLsq> lsq;
};

}  // namespace bach
}  // namespace latch

#endif
