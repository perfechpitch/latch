#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_DTE_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_DTE_

// DTE DSA 的装配：三个模块——Hmem（表）、Mover（数据通路）、DteRegfile（配置前端）。
//
// 原来那八类模块（Lane / Buffer / Xbar / OutArb / Commit / CompletionRs /
// HeaderParser / TaskQueue）各自独立协程、靠 valid/ready/seq 多拍握手拼流水线的
// 结构，收敛成 Mover 一个模块按任务顺序逐拍推进（见 mover.h 的说明）。Regfile 的
// 配置入口不变，只把它的输出 DescPort 对端从 Commit 换成 Mover。
//
// 自身没有 Cycle()：构造三个模块、把 Regfile 起的任务接给 Mover。外层每拍调一次
// RunStep()，整个 DTE 只占外层那一个协程（同 Mu）。

#include <memory>
#include <string>

#include "bach/ip/chip/core/dte/hmem.h"
#include "bach/ip/chip/core/dte/mover.h"
#include "bach/ip/chip/core/dte/regfile.h"
#include "bach/ip/chip/core/memory/banked_mem.h"
#include "bach/ip/chip/core/mu/gen_ep_info.h"

namespace latch {
namespace bach {

class Dte {
 public:
  Dte(ClockPtr clock, const std::string& name, uint64_t parent = 0)
      : clk(clock) {
    const uint64_t gid = TraceGroup(name, parent);
    // 都不自己挂时钟：外层每拍调一次 RunStep()，按末级先做的次序逐个走一遍。
    hmem = std::make_unique<Hmem>(clock, "hmem", gid, false);
    mover = std::make_unique<Mover>(clock, "mover", *hmem, gid, false);
    reg = std::make_unique<DteRegfile>(clock, "regfile", agcu, gid, false);
    // 寄存器组起的任务走 DescPort 交给 Mover。
    mover->RebindRv(reg->OutPtr());
  }

  // ── 对外 ──
  CoreDataPort& FromRouter() { return mover->FromRouter(); }
  void AttachFromRouter(std::shared_ptr<CoreDataPort> p) {
    mover->AttachFromRouter(std::move(p));
  }
  CoreDataPort& ToRouter() { return mover->ToRouter(); }
  void AttachToRouter(std::shared_ptr<CoreDataPort> p) {
    mover->AttachToRouter(std::move(p));
  }
  DsaCfgPort& Cfg() { return reg->Cfg(); }
  std::shared_ptr<DsaCfgPort> CfgPtr() const { return reg->CfgPtr(); }
  std::shared_ptr<DsaRdataPort> RdataPtr() const { return reg->RdataPtr(); }
  DescPort& FromRv() { return mover->FromRv(); }
  DonePort& ToTs() { return mover->ToTs(); }
  void AttachToTs(std::shared_ptr<DonePort> p) { mover->AttachToTs(std::move(p)); }
  // shareMem 表项写，接 Share Mem 的 DTE DSA 口。
  MemPort& SmemWr() { return mover->SmemWr(); }
  std::shared_ptr<MemPort> SmemWrPtr() const { return mover->SmemWrPtr(); }
  void AttachSmemWr(std::shared_ptr<MemPort> p) {
    mover->AttachSmemWr(std::move(p));
  }
  // topK 旁带写进 MU 的那条数据线。
  void AttachMuTopk(std::shared_ptr<MuTopkPort> p) {
    mover->AttachMuTopk(std::move(p));
  }
  // 出核造包时从 MU 的 topK_ep_table 读 topK 的入口。
  void AttachMuTopkEp(GenEpInfo* ep) { mover->AttachMuTopkEp(ep); }
  // 包头上下文落 Core Mem（B/R core）时要用 Core Mem 的同步 Poke/Peek。
  void AttachCmemSync(BankedMem* m) { mover->AttachCmemSync(m); }
  // 对每块存储的读与写各一个口。
  void AttachCmemRd(std::shared_ptr<MemPort> p) { mover->AttachCmemRd(std::move(p)); }
  void AttachCmemWr(std::shared_ptr<MemPort> p) { mover->AttachCmemWr(std::move(p)); }
  void AttachMmemRd(std::shared_ptr<MemPort> p) { mover->AttachMmemRd(std::move(p)); }
  void AttachMmemWr(std::shared_ptr<MemPort> p) { mover->AttachMmemWr(std::move(p)); }

  // ── 配置面 ──
  Hmem& Tables() { return *hmem; }

  // ── 观测 ──
  // 原来那几个观测访问器（Parser/Committer/Completion/OutArb/OutBuf）现在都指向
  // Mover 的同一组计数，用轻量视图保留同名方法，core.h 与测试不用改就能读。
  struct CommitView {
    Mover const& m;
    uint64_t Admitted() const { return m.Admitted(); }
    uint64_t RvAdmitted() const { return m.RvAdmitted(); }
    uint64_t StartTask() const { return m.StartTask(); }
    uint64_t StartUser() const { return m.StartUser(); }
    uint64_t Stalled() const { return m.Stalled(); }
  };
  struct CompletionView {
    Mover const& m;
    uint64_t Reported() const { return m.Reported(); }
    uint64_t DoneTask() const { return m.DoneTask(); }
    uint64_t DoneUser() const { return m.DoneUser(); }
    uint64_t Joined() const { return m.Joined(); }
    uint64_t Used() const { return m.Used(); }
  };
  struct ParserView {
    Mover const& m;
    uint64_t Parsed() const { return m.Parsed(); }
  };
  struct OutArbView {
    Mover const& m;
    uint64_t Sent() const { return m.Sent(); }
  };
  struct OutBufView {
    Mover const& m;
    uint64_t Occupancy() const { return m.Occupancy(); }
  };

  DteRegfile& Regfile() { return *reg; }
  CommitView Committer() { return CommitView{*mover}; }
  CompletionView Completion() { return CompletionView{*mover}; }
  ParserView Parser() { return ParserView{*mover}; }
  OutArbView OutArb() { return OutArbView{*mover}; }
  OutBufView OutBuf() { return OutBufView{*mover}; }

  void RunStep() {
    hmem->RunStep();
    reg->RunStep();
    mover->RunStep();
  }

  bool Quiescent() const { return mover->Quiescent() && reg->Quiescent(); }

 private:
  ClockPtr clk;
  Agcu agcu;

  std::unique_ptr<Hmem> hmem;
  std::unique_ptr<Mover> mover;
  std::unique_ptr<DteRegfile> reg;
};

}  // namespace bach
}  // namespace latch

#endif
