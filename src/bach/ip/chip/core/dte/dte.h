#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_DTE_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_DTE_

// Aggregate DTE: one simulation coroutine owns five channels, their buffers,
// the global queue, endpoint arbitration and completion state. Interconnect
// traffic uses versioned ports.

#include <memory>
#include <string>
#include <vector>

#include "bach/ip/chip/core/dte/commit.h"
#include "bach/ip/chip/core/dte/completion_rs.h"
#include "bach/ip/chip/core/dte/dma_xbar.h"
#include "bach/ip/chip/core/dte/header_parser.h"
#include "bach/ip/chip/core/dte/lane.h"
#include "bach/ip/chip/core/dte/out_arb.h"
#include "bach/ip/chip/core/dte/regfile.h"
#include "bach/ip/chip/core/memory/banked_mem.h"
#include "bach/ip/chip/core/mu/gen_ep_info.h"

namespace latch {
namespace bach {

struct DteModelCfg {
  DteWindows windows{};
  uint64_t control_base = 0x400;
};
class Dte {
public:
  Dte(ClockPtr clock, const std::string &name, uint64_t parent = 0,
      DteModelCfg const &setting = {})
      : clk(clock), agcu(setting.windows), model_cfg(setting) {
    LOGCHECK(setting.control_base >= 0x400 &&
                 setting.control_base + 40 < 0x800 &&
                 !(setting.control_base & 3),
             "DTE: model control registers must fit the control window");
    const uint64_t gid = TraceGroup(name, parent);
    // 都不自己挂时钟：外层每拍调一次 RunStep()，按末级先做的次序逐个走一遍，
    // 整个 DTE 只占外层那一个协程（同 Mu）。
    hmem = std::make_unique<Hmem>(clock, "hmem", gid, false);
    // 一个通道一份 Buffer：进核那块归 in_ch，出核那块四个出核通道各一份，
    // 各自 kDteBufFlits 项，不是四个分一份。
    in_buf = std::make_unique<DteBuffer>(clock, "in_buf", kDteBufFlits, 1, gid,
                                         false);
    out_buf = std::make_unique<DteBuffer>(clock, "out_buf",
                                          kDteBufFlits * (kLaneNum - 1),
                                          kLaneNum - 1, gid, false);
    parser = std::make_unique<HeaderParser>(clock, "parser", gid, false);
    commit = std::make_unique<Commit>(clock, "commit", *hmem, gid, false);
    out_arb = std::make_unique<DteOutArb>(clock, "out_arb", gid, false);
    reg = std::make_unique<DteRegfile>(clock, "regfile", agcu, gid, false);
    comp = std::make_unique<CompletionRs>(clock, "comp", gid, false);
    xbar = std::make_unique<DteXbar>(clock, "xbar", gid, false);
    for (uint64_t i = 0; i < kLaneNum; ++i) {
      DteBuffer &b = (i == kInCh) ? *in_buf : *out_buf;
      lanes.push_back(std::make_unique<Lane>(clock, "lane" + std::to_string(i),
                                             i, b, gid, false));
    }
    Wire();
  }

  // ── 对外 ──
  CoreDataPort &FromRouter() { return parser->FromRouter(); }
  void AttachFromRouter(std::shared_ptr<CoreDataPort> p) {
    parser->AttachFromRouter(std::move(p));
  }
  // 四个出核通道共用 Router 那一个口，在 DteOutArb 里仲裁。
  CoreDataPort &ToRouter() { return out_arb->Out(); }
  void AttachToRouter(std::shared_ptr<CoreDataPort> p) {
    out_arb->AttachOut(std::move(p));
  }
  void AttachTxCredits(std::shared_ptr<DteTxCreditPort> port,
                       VcDepth const &depth, uint64_t shared) {
    out_arb->AttachCredits(std::move(port), depth, shared);
  }
  DsaCfgPort &Cfg() { return reg->Cfg(); }
  std::shared_ptr<DsaCfgPort> CfgPtr() const { return reg->CfgPtr(); }
  std::shared_ptr<DsaRdataPort> RdataPtr() const { return reg->RdataPtr(); }
  DescPort &FromRv() { return commit->FromRv(); }
  DonePort &ToTs() { return comp->ToTs(); }
  void AttachToTs(std::shared_ptr<DonePort> p) {
    comp->AttachToTs(std::move(p));
  }
  // shareMem 表项写，接 Share Mem 的 DTE DSA 口。
  MemPort &SmemWr() { return comp->SmemWr(); }
  std::shared_ptr<MemPort> SmemWrPtr() const { return comp->SmemWrPtr(); }
  void AttachSmemWr(std::shared_ptr<MemPort> p) {
    comp->AttachSmemWr(std::move(p));
  }
  void AttachMuTopkEp(GenEpInfo *ep) {
    LOGCHECK(ep, "DTE: topK endpoint is required");
    xbar->AttachTopkRd(ep->DteReadPtr());
    xbar->AttachTopkWr(ep->DteWritePtr());
  }
  // 对每块存储的读与写各一个口，五个通道在 DMA_XBAR 里仲裁。
  void AttachCmemRd(std::shared_ptr<MemPort> p) {
    xbar->AttachCmemRd(std::move(p));
  }
  void AttachCmemWr(std::shared_ptr<MemPort> p) {
    xbar->AttachCmemWr(std::move(p));
  }
  void AttachMmemRd(std::shared_ptr<MemPort> p) {
    xbar->AttachMmemRd(std::move(p));
  }
  void AttachMmemWr(std::shared_ptr<MemPort> p) {
    xbar->AttachMmemWr(std::move(p));
  }

  std::shared_ptr<DteExceptionPort> ExceptionPtr() const {
    return comp->ExceptionPtr();
  }
  void Resume() {
    comp->ClearError(1);
    commit->Resume();
  }
  void Stop() { commit->Stop(); }
  // ── 配置面 ──
  Hmem &Tables() { return *hmem; }

  // ── 观测 ──
  HeaderParser &Parser() { return *parser; }
  DteRegfile &Regfile() { return *reg; }
  DteOutArb &OutArb() { return *out_arb; }
  Commit &Committer() { return *commit; }
  CompletionRs &Completion() { return *comp; }
  DteBuffer &OutBuf() { return *out_buf; }

  void RunStep() {
    hmem->RunStep();
    comp->RunStep();
    if (comp->Error())
      commit->Stop();
    xbar->RunStep();
    out_arb->RunStep();
    for (auto &l : lanes)
      l->RunStep();
    in_buf->RunStep();
    out_buf->RunStep();
    commit->RunStep();
    parser->RunStep();
    reg->RunStep();
  }

  bool Quiescent() const {
    for (auto const &l : lanes) {
      if (!l->Quiescent())
        return false;
    }
    return parser->Quiescent() && commit->Quiescent() && comp->Quiescent() &&
           in_buf->Quiescent() && out_buf->Quiescent() && reg->Quiescent() &&
           xbar->Quiescent() && out_arb->Quiescent();
  }

private:
  void Wire() {
    // Commit dispatch 一笔后，把它分发给归属的 Lane 与 Completion RS。两处都走
    // 端口：一根线两端是同一个对象，各自的协程只碰自己的队列。
    for (auto &l : lanes)
      commit->AddLanePort(l->AdmitPtr());
    comp->AttachAdmit(commit->RsPortPtr());
    comp->AttachCanceled(commit->CancelPtr());
    reg->BindError([this] { return comp->Error() != 0; });
    // The source does not freeze control offsets. These are model parameters.
    reg->BindControl(
        [this](uint64_t at) -> uint64_t {
          if (at >= 0x2000 && at < 0x3000)
            return commit->ReadEntry(at);
          if (at >= 0x3000)
            return hmem->ReadSoftwareBeforeEdge(at);
          if (at == model_cfg.control_base)
            return comp->Error() ? 2 : commit->Running() ? 1 : 0;
          if (at == model_cfg.control_base + 4)
            return Quiescent();
          if (at == model_cfg.control_base + 8)
            return comp->Error();
          if (at == model_cfg.control_base + 12)
            return comp->ErrorIds();
          if (at == model_cfg.control_base + 16)
            return commit->Occupied();
          if (at == model_cfg.control_base + 28)
            return comp->ErrorEntry();
          if (at == model_cfg.control_base + 32)
            return comp->ErrorIds() >> 32;
          if (at == model_cfg.control_base + 36)
            return comp->Interrupt();
          if (at == model_cfg.control_base + 40)
            return comp->InterruptMask();
          if (at >= 0xc00 && at < 0xc00 + kLaneNum * 64) {
            uint64_t lane = (at - 0xc00) / 64, off = (at - 0xc00) % 64;
            if (off < 8)
              return lanes[lane]->ContextState(off / 4);
            if (off < 16)
              return lanes[lane]->Outstanding((off - 8) / 4);
          }
          if (at == 0x800)
            return commit->RvAdmitted();
          if (at == 0x804)
            return comp->Reported();
          return 0;
        },
        [this](uint64_t at, uint64_t data) {
          if (at >= 0x3000)
            hmem->WriteSoftware(at, data);
          else if (at == model_cfg.control_base) {
            if (data == 0)
              Stop();
            else if (data == 1 && !comp->Error())
              commit->Resume();
            else if (data == 3 && !comp->Error())
              commit->SingleStep();
          } else if (at == model_cfg.control_base + 8)
            comp->ClearError(data);
          else if (at == model_cfg.control_base + 20)
            commit->Cancel(data & 15);
          else if (at == model_cfg.control_base + 24 && (data & 1)) {
            commit->Flush();
            comp->SuppressAcks();
          } else if (at == model_cfg.control_base + 40)
            comp->SetInterruptMask(data);
        });
    commit->AttachRetired(comp->RetiredPtr());
    commit->AttachEntryState(
        [this](uint64_t seq) { return comp->EntryState(seq); },
        [this](uint64_t seq) { return comp->EntryError(seq); });

    // 寄存器组起的任务走 DescPort 交给 Commit 的中央 TaskQueue。
    commit->RebindRv(reg->OutPtr());

    // Payload 走端口交给进核通道的 RD 侧。
    lanes[kInCh]->AttachPayload(parser->PayloadPtr());
    for (auto &l : lanes)
      l->AttachHmem(*hmem);

    // 每个通道在 DMA_XBAR 上各占一份存储口，四个出核通道各占出核仲裁的一份。
    for (uint64_t i = 0; i < kLaneNum; ++i) {
      lanes[i]->AttachCmem(xbar->LaneCmem(i));
      lanes[i]->AttachMmem(xbar->LaneMmem(i));
      lanes[i]->AttachTopk(xbar->LaneTopk(i));
      lanes[i]->AttachHeader(hmem->LanePort(i));
    }
    for (uint64_t i = 0; i < DteOutArb::kOutNum; ++i) {
      lanes[kOutCh0 + i]->AttachToRouter(out_arb->LanePort(i));
      lanes[kOutCh0 + i]->AttachTailDone(out_arb->TailDonePtr(i));
    }

    // 五个通道的两侧完成都汇到 Completion RS。同一拍多个 Join 命中时全部写进
    // Done Pending，由它串行化。
    for (auto &l : lanes) {
      comp->AddSource(l->RdDonePtr(), l->WrDonePtr());
    }
  }

  ClockPtr clk;
  Agcu agcu;
  DteModelCfg model_cfg;

  std::unique_ptr<Hmem> hmem;
  std::unique_ptr<DteBuffer> in_buf, out_buf;
  std::unique_ptr<HeaderParser> parser;
  std::unique_ptr<DteRegfile> reg;
  std::unique_ptr<DteXbar> xbar;
  std::unique_ptr<DteOutArb> out_arb;
  std::unique_ptr<Commit> commit;
  std::unique_ptr<CompletionRs> comp;
  std::vector<std::unique_ptr<Lane>> lanes;
};

} // namespace bach
} // namespace latch

#endif
