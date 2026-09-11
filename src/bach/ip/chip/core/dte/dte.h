#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_DTE_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_DTE_

// DTE DSA 这一组八类模块的装配。
//
// 自身没有 Cycle()：构造各模块、把 Commit 的三样资源接到 Lane 与 Completion RS
// 上、把 Header Parser 收下的 Payload 接进进核通道。
//
// 里面装了几件：
//   HeaderParser   1 个，Router 入站帧的第一道
//   DteRegfile     1 个，RV core 写的软件配置寄存器，写 trigger 起一笔任务
//   Commit         1 个，两个任务入口在这里汇成同一套内部任务模型
//   Hmem           1 个，含 Fast LUT、RouterTable 副本、stream_cache
//   Lane           5 个：in_ch 加 out_ch[0..3]
//   DteXbar        1 个，五个通道与两块存储之间的仲裁
//   DteOutArb      1 个，四个出核通道与 Router 那一个口之间的仲裁
//   DteBuffer      2 个：inbound 与 outbound
//   CompletionRs   1 个，把劈开的两半合回来
//
// 对外：进出核接 Router 的 CoreStation，访存经 DMA_XBAR 接三块存储，配置口接
// DTE RV core 的 dsa_iss，完成上报接 TS。

#include <memory>
#include <string>
#include <vector>

#include "bach/ip/chip/core/dte/commit.h"
#include "bach/ip/chip/core/dte/dma_xbar.h"
#include "bach/ip/chip/core/dte/completion_rs.h"
#include "bach/ip/chip/core/dte/header_parser.h"
#include "bach/ip/chip/core/dte/lane.h"
#include "bach/ip/chip/core/dte/out_arb.h"
#include "bach/ip/chip/core/dte/regfile.h"

namespace latch {
namespace bach {

struct DteCfg {
  CmemLayout cmem;
  // 进核那一笔落 Core Mem 还是 Matrix Mem。落点的地址取自包头，落哪一块由收方
  // 定：业务模式下计算 core 落 Core Mem、B core 与 R core 落 Matrix Mem；
  // weights 加载阶段计算 core 上进来的也是权重，同样落 Matrix Mem。
  Route inbound = Route::kRouterToCm;
  // B core 与 R core：进核那一笔搬完之后给这个 token 槽位置 valid 标志。标志
  // 表在 Share Mem 里，一项 4 B，第几项按落点除以槽位大小算。entry_bytes 为 0
  // 表示本 core 不置标志。
  uint64_t inbound_flag_base = 0;
  uint64_t inbound_entry_bytes = 0;
  // B core 与 R core 上进来的包不建 stream 表项，进核那一笔不回 Ack。
  bool inbound_no_ack = false;
  bool tick = true;
};

class Dte {
 public:
  Dte(ClockPtr clock, const std::string& name, DteCfg const& setting,
      uint64_t parent = 0)
      : clk(clock), cfg(setting), agcu(setting.cmem) {
    const uint64_t gid = TraceGroup(name, parent);
    hmem = std::make_unique<Hmem>(clock, "hmem", gid, setting.tick);
    // 进核那块只有 in_ch 一个通道用，出核那块四个出核通道各分一份。
    in_buf = std::make_unique<DteBuffer>(clock, "in_buf", kDteBufFlits,
                                         1, gid, setting.tick);
    out_buf = std::make_unique<DteBuffer>(clock, "out_buf", kDteBufFlits,
                                          kLaneNum - 1, gid, setting.tick);
    parser = std::make_unique<HeaderParser>(clock, "parser", gid,
                                            setting.tick);
    parser->SetInboundRoute(setting.inbound);
    parser->SetInboundFlag(setting.inbound_flag_base,
                           setting.inbound_entry_bytes);
    parser->SetInboundNoAck(setting.inbound_no_ack);
    parser->AttachPathTask(
        [this](uint64_t path) { return hmem->PathTask(path); });
    commit = std::make_unique<Commit>(clock, "commit", *hmem, gid,
                                      setting.tick);
    out_arb = std::make_unique<DteOutArb>(clock, "out_arb", gid,
                                          setting.tick);
    reg = std::make_unique<DteRegfile>(clock, "regfile", gid, setting.tick);
    comp = std::make_unique<CompletionRs>(clock, "comp", gid, setting.tick);
    xbar = std::make_unique<DteXbar>(clock, "xbar", gid, setting.tick);
    for (uint64_t i = 0; i < kLaneNum; ++i) {
      DteBuffer& b = (i == kInCh) ? *in_buf : *out_buf;
      lanes.push_back(std::make_unique<Lane>(
          clock, "lane" + std::to_string(i), i, b, agcu, gid, setting.tick));
    }
    Wire();
  }

  // ── 对外 ──
  // SCP 切模式时重配进核那一档。weights 加载阶段与业务阶段的落点、回不回 Ack
  // 都不一样，两样都在这里改。
  void SetInbound(Route r, bool no_ack) {
    parser->SetInboundRoute(r);
    parser->SetInboundNoAck(no_ack);
  }

  CoreDataPort& FromRouter() { return parser->FromRouter(); }
  void AttachFromRouter(std::shared_ptr<CoreDataPort> p) {
    parser->AttachFromRouter(std::move(p));
  }
  // 四个出核通道共用 Router 那一个口，在 DteOutArb 里仲裁。
  CoreDataPort& ToRouter() { return out_arb->Out(); }
  void AttachToRouter(std::shared_ptr<CoreDataPort> p) {
    out_arb->AttachOut(std::move(p));
  }
  DsaCfgPort& Cfg() { return reg->Cfg(); }
  std::shared_ptr<DsaCfgPort> CfgPtr() const { return reg->CfgPtr(); }
  // 四个身份信号从 DTE RV core 直连过来，写 trigger 那一拍采样。
  void AttachIds(std::shared_ptr<DsaIdsPort> p) { reg->AttachIds(std::move(p)); }
  std::shared_ptr<DsaRdataPort> RdataPtr() const { return reg->RdataPtr(); }
  DescPort& FromRv() { return commit->FromRv(); }
  DonePort& ToTs() { return comp->ToTs(); }
  void AttachToTs(std::shared_ptr<DonePort> p) { comp->AttachToTs(std::move(p)); }
  // shareMem 表项写，接 Share Mem 的 DTE DSA 口。
  MemPort& SmemWr() { return comp->SmemWr(); }
  std::shared_ptr<MemPort> SmemWrPtr() const { return comp->SmemWrPtr(); }
  void AttachSmemWr(std::shared_ptr<MemPort> p) {
    comp->AttachSmemWr(std::move(p));
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
  MemPort& CmemRd() { return xbar->CmemRd(); }
  MemPort& CmemWr() { return xbar->CmemWr(); }
  MemPort& MmemRd() { return xbar->MmemRd(); }
  MemPort& MmemWr() { return xbar->MmemWr(); }

  // ── 配置面 ──
  Hmem& Tables() { return *hmem; }
  // 出核前查 VC 通路上的 flit credit。装配层接到 Xbar 每拍发布的那个电平上。
  void AttachVcLevel(std::shared_ptr<CreditLevelPort> p) {
    commit->AttachVcLevel(std::move(p));
  }

  // ── 观测 ──
  HeaderParser& Parser() { return *parser; }
  DteRegfile& Regfile() { return *reg; }
  DteXbar& Xbar() { return *xbar; }
  DteOutArb& OutArb() { return *out_arb; }
  Commit& Committer() { return *commit; }
  CompletionRs& Completion() { return *comp; }
  Lane& GetLane(uint64_t i) { return *lanes.at(i); }
  DteBuffer& InBuf() { return *in_buf; }
  DteBuffer& OutBuf() { return *out_buf; }

  void RunStep() {
    hmem->RunStep();
    comp->RunStep();
    xbar->RunStep();
    out_arb->RunStep();
    for (auto& l : lanes) l->RunStep();
    in_buf->RunStep();
    out_buf->RunStep();
    commit->RunStep();
    parser->RunStep();
    reg->RunStep();
  }

  bool Quiescent() const {
    for (auto const& l : lanes) {
      if (!l->Quiescent()) return false;
    }
    return parser->Quiescent() && commit->Quiescent() && comp->Quiescent() &&
           in_buf->Quiescent() && out_buf->Quiescent() && reg->Quiescent() &&
           xbar->Quiescent() && out_arb->Quiescent();
  }

 private:
  void Wire() {
    // Commit 准入一笔后，把它分发给归属的 Lane 与 Completion RS。两处都走
    // 端口：一根线两端是同一个对象，各自的协程只碰自己的队列。
    for (auto& l : lanes) commit->AddLanePort(l->AdmitPtr());
    comp->AttachAdmit(commit->RsPortPtr());

    // Commit 与 Header Parser 之间那根：两端指向同一个对象。
    RebindParserPort();
    // 寄存器组起的任务走同一种口交给 Commit。
    commit->RebindRv(reg->OutPtr());

    // Payload 走端口交给进核通道的 RD 侧。
    lanes[kInCh]->AttachPayload(parser->PayloadPtr());

    // 每个通道在 DMA_XBAR 上各占一份存储口，四个出核通道各占出核仲裁的一份。
    for (uint64_t i = 0; i < kLaneNum; ++i) {
      lanes[i]->AttachCmem(xbar->LaneCmem(i));
      lanes[i]->AttachMmem(xbar->LaneMmem(i));
    }
    for (uint64_t i = 0; i < DteOutArb::kOutNum; ++i) {
      lanes[kOutCh0 + i]->AttachToRouter(out_arb->LanePort(i));
    }

    // 五个通道的两侧完成都汇到 Completion RS。同一拍多个 Join 命中时全部写进
    // Done Pending，由它串行化。
    for (auto& l : lanes) {
      comp->AddSource(l->RdDonePtr(), l->WrDonePtr());
    }
  }

  void RebindParserPort() {
    // Commit 读的那个口就是 Parser 写的那个。
    auto port = parser->ToCommitPtr();
    commit_port = port;
    // Commit 侧持有同一个 shared_ptr。
    struct Rebinder {
      static void Do(Commit& c, std::shared_ptr<DescPort> p) {
        c.RebindParser(std::move(p));
      }
    };
    Rebinder::Do(*commit, port);
  }

  ClockPtr clk;
  DteCfg cfg;
  Agcu agcu;

  std::unique_ptr<Hmem> hmem;
  std::unique_ptr<DteBuffer> in_buf, out_buf;
  std::unique_ptr<HeaderParser> parser;
  std::unique_ptr<DteRegfile> reg;
  std::unique_ptr<DteXbar> xbar;
  std::unique_ptr<DteOutArb> out_arb;
  std::unique_ptr<Commit> commit;
  std::unique_ptr<CompletionRs> comp;
  std::vector<std::unique_ptr<Lane>> lanes;
  std::shared_ptr<DescPort> commit_port;
};

}  // namespace bach
}  // namespace latch

#endif
