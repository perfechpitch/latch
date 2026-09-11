#ifndef _LATCH_BACH_IP_CHIP_CORE_CORE_
#define _LATCH_BACH_IP_CHIP_CORE_CORE_

// Core：chip 阵列里的一格。
//
// 自己不打拍：全部逐拍行为在七个单元的模块里，这一层只做构造期的接线，加上
// 几条不归任何单个单元的约定。
//
// 对外只有两组连接：三个 R2R 方向各一条 256 B/T 的双向链路，与 ctrl_noc 的配置
// 写事务口。没有第三条路：core 的一切进出都过 Router。
//
// router_only 那一档是边界 chip 里不派角色的那个 core：只构造 Router 的八个
// 模块，不构造 TS、RV core、DSA 与三块存储。它仍要承担单向转发、多播、
// router-level reduce 与三类 credit 的透传，而且坐在 chip 接 PCIe Switch 的
// 那个口上，所以 Router 一个都不能少。
//
// 三个 DSA 之间没有任何直连：DSA 之间的数据一律经存储交换，控制一律经 TS 与
// 各自的 RV core。每个 DSA 与本核那个 RV core 之间另有四个身份信号直连
// （streamID、taskID、userID、pathID），DSA 在写 trigger 寄存器那一拍采样。

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "bach/ip/chip/chip_ports.h"
#include "bach/ip/module_base.h"
#include "bach/ip/chip/core/dte/dte.h"
#include "bach/ip/chip/core/memory/core_mem.h"
#include "bach/ip/chip/core/memory/matrix_mem.h"
#include "bach/ip/chip/core/memory/share_mem.h"
#include "bach/ip/chip/core/mu/mu.h"
#include "bach/ip/chip/core/router/router.h"
#include "bach/ip/chip/core/rv_core/rv_core.h"
#include "bach/ip/chip/core/ts/ts.h"
#include "bach/ip/chip/core/vu/vu.h"

namespace latch {
namespace bach {

// 一个 core 在系统里扮演什么。不派角色的那个只转发。
enum class CoreRole : uint32_t {
  kCompute = 0,     // logical compute core 0～7
  kBroadcast = 1,   // B core：第一列 chip 的 core0
  kReduce = 2,      // R core：最后一列 chip 的 core9
  kSpare = 3,       // 不派角色，只构造 Router
};

// 只读上下文，core 内各模块共用。构造期写入，之后不变。
struct CoreContext {
  uint64_t core_id = 0;      // 本 chip 内的编号
  uint64_t gx = 0, gy = 0;   // 全局坐标
  CoreRole role = CoreRole::kCompute;
  bool router_only = false;
  // R core 与 B core：进核那一笔搬完之后置这个 token 槽位的 valid 标志。表在
  // Share Mem 里，一项 4 B，第几项按落点除以槽位大小算。两个数由编译侧给，
  // entry_bytes 为 0 表示本 core 不置标志。
  uint64_t inbound_flag_base = 0;
  uint64_t inbound_entry_bytes = 0;
  // 本 core 自己挂时钟（占一个常驻协程），还是由 chip 那一层顺序调 RunStep()。
  // core 之间只经 LinkEnd 通讯，那一组端口全部打拍，所以两种驱动方式逐拍结果
  // 相同。挂时钟时 core 之间才能真正并行。
  bool tick = false;
};

class Core : public BachModule {
 public:
  Core(ClockPtr clock, const std::string& name, CoreContext const& context,
       uint64_t parent = 0)
      : BachModule(clock, name, parent, context.tick), ctx(context) {
    RouterCfg rcfg;
    // 不派角色的 core 在 Skip Mask 里标成跳过：经过它的包走完整流水线但不投递
    // local。CoreStation 永远不准入，ReduceModule 不累加。
    rcfg.pass_through = ctx.router_only;
    rcfg.tick = false;
    // 波形上一个 core 只有 EmitTrace() 那一组信号，直接挂在 core 下面，单元与
    // 模块不在层次里各占一级。所以底下的模块建出来时波形一律关掉，由本层统一
    // 发。要看某个模块自己的全部信号，跑 test/bach/ip/chip/core/ 下对应的单
    // 模块用例，那里不经这一层。
    TraceOffScope off;
    router = std::make_unique<Router>(clock, "router", rcfg, Id());
    if (ctx.router_only) return;

    TsCfg tcfg;
    tcfg.tick = false;
    ts = std::make_unique<Ts>(clock, "ts", tcfg, Id());

    DteCfg dcfg;
    dcfg.tick = false;
    dcfg.inbound = BusinessInbound();
    dcfg.inbound_no_ack = BusinessNoAck();
    dcfg.inbound_flag_base = ctx.inbound_flag_base;
    dcfg.inbound_entry_bytes = ctx.inbound_entry_bytes;
    dte = std::make_unique<Dte>(clock, "dte", dcfg, Id());

    cmem = std::make_unique<CoreMem>(clock, "cmem", Id(), false);
    mmem = std::make_unique<MatrixMem>(clock, "mmem", Id(), false);
    smem = std::make_unique<ShareMem>(clock, "smem", Id(), false);

    rv[0] = std::make_unique<RvCore>(clock, "rv_dte", RvUnit::kDte, Id());
    rv[1] = std::make_unique<RvCore>(clock, "rv_mu", RvUnit::kMu, Id());
    rv[2] = std::make_unique<RvCore>(clock, "rv_vu", RvUnit::kVu, Id());

    mu = std::make_unique<Mu>(clock, "mu", MuCfg{}, Id());
    vu = std::make_unique<Vu>(clock, "vu", Id());
    Wire();
  }

  // ── 对外：三个 R2R 方向 ──
  LinkEndPtr InWire(uint64_t d) const { return router->InWire(d); }
  LinkEndPtr OutWire(uint64_t d) const { return router->OutWire(d); }
  LinkEndPtr BackWire(uint64_t d) const { return router->BackWire(d); }
  LinkEndPtr UpBackWire(uint64_t d) const { return router->UpBackWire(d); }
  LinkEndPtr UpReleaseWire(uint64_t d) const {
    return router->UpReleaseWire(d);
  }

  // 切进 weights 加载模式：这一阶段进来的是权重，落 Matrix Mem；这一阶段不建
  // stream 表项，进核那一笔没有可报的对象，不回 Ack。
  void SetWeightsInbound() { dte->SetInbound(Route::kRouterToMm, true); }
  // 切回业务模式：进核那一档按本 core 的角色定。
  void SetBusinessInbound() {
    dte->SetInbound(BusinessInbound(), BusinessNoAck());
  }

  CoreContext const& Context() const { return ctx; }
  // 三个 RV core 都进 wait 后拉高，SCP 据此开放业务接收权限。
  bool Ready() const {
    if (ctx.router_only) return true;
    for (auto const& r : rv) {
      if (!r->Ready()) return false;
    }
    return true;
  }

  // boot 期 ctrl_noc 端点把一笔配置写交到这里，按目的模块分发。
  //
  // 走方法调用而不是端口：端点与目的模块都在装配这一个协程里，而且这几条通路
  // 只在 boot 期用；业务期是 RV core 的 dsa_iss 在写 DSA 的那个口，两者不
  // 同时。VU 那一侧例外，它本来就有三条独立的配置通路，Ctrl-NOC 占其中一条。
  void CfgWrite(CfgRoute const& r, bool we, uint64_t data) {
    if (!we) return;
    switch (r.target) {
      case kCfgRouter:
        // RouterTable 的 CSR。逐笔写按 path_id 铺表。
        router->Table().SetSkipMask(data);
        break;
      case kCfgTs:
        if (ts) ts->Cfg().SetStreamNum(data == 0 ? 1 : data);
        break;
      case kCfgDte:
        // DTE 的寄存器由它的 Commit 那一侧收，boot 期只配 Hmem 的表。
        break;
      case kCfgMu:
        if (mu) mu->Regfile().CfgWrite(r.offset, data);
        break;
      case kCfgVu:
        // VU 有三条独立的配置通路，Ctrl-NOC 占其中一条，走端口。
        break;
      case kCfgItcm:
        if (rv[r.index]) {
          std::vector<uint8_t> word(4);
          for (int k = 0; k < 4; ++k) word[k] = uint8_t((data >> (8 * k)) & 0xFFu);
          rv[r.index]->PokeItcm(r.offset, word);
        }
        break;
      case kCfgSmem:
        if (smem) PokeWord(*smem, r.offset, data);
        break;
      case kCfgCmem:
        if (cmem) PokeWord(*cmem, r.offset, data);
        break;
      case kCfgMmem:
        if (mmem) PokeWord(*mmem, r.offset, data);
        break;
      default:
        break;
    }
  }

  // ── 观测 ──
  Router& GetRouter() { return *router; }
  Ts& GetTs() { return *ts; }
  RvCore& Rv(uint64_t i) { return *rv.at(i); }
  Dte& GetDte() { return *dte; }
  Mu& GetMu() { return *mu; }
  Vu& GetVu() { return *vu; }
  CoreMem& Cmem() { return *cmem; }
  MatrixMem& Mmem() { return *mmem; }
  ShareMem& Smem() { return *smem; }

  // 末级先做：一笔活在一拍里最多前进一级。存储排在最前，因为它是各条通路的末端。
  void Step() override {
    router->RunStep();
    if (ctx.router_only) {
      EmitTrace();
      return;
    }
    cmem->RunStep();
    mmem->RunStep();
    smem->RunStep();
    vu->RunStep();
    mu->RunStep();
    dte->RunStep();
    for (auto& r : rv) r->RunStep();
    ts->RunStep();
    EmitTrace();
  }

  // 一个 core 在波形上的全部信号，一处定完，全部平铺在 core 下面。看的是数据
  // 与任务在这个 core 上流没流动、堵没堵。各模块自己不发信号，值从它们的观测
  // 访问器读，读的是本拍这一遍 RunStep() 算完的值。
  void EmitTrace() {
    // 四个入口站：三个 R2R 方向，加本 core 的 DTE 出核进 Router 那一路。
    static const char* kFwd[4] = {"fwd_mid", "fwd_left", "fwd_right", "fwd_core"};
    static const char* kOcc[4] = {"occ_mid", "occ_left", "occ_right", "occ_core"};
    for (uint64_t d = 0; d < 4; ++d) {
      TracePerCycle(kFwd[d], router->Station(d).Forwarded());
      TracePerCycle(kOcc[d], router->Station(d).Occupancy());
    }
    TracePerCycle("xbar_stall", router->GetXbar().Stalled());
    TracePerCycle("reduce_q", router->GetReduce().OutQueued());
    TracePerCycle("core_in", router->GetCoreStation().OutBufDepth());
    EmitRouter();
    if (ctx.router_only) return;
    TracePerCycle("core_out", dte->OutBuf().Occupancy());
    TracePerCycle("ts_inflight", ts->Table().InFlight());
    TracePerCycle("ts_issue", ts->DteArbiter().Issued() +
                                  ts->MuArbiter().Issued() +
                                  ts->VuArbiter().Issued());
    TracePerCycle("ts_done", ts->Done().Finished());
    EmitIssue();
  }

  // Router 里设计文档画出来的那几个方框，各记一两个量：
  //
  //   out_mid / out_left / out_right / out_core  Xbar 往这四个出口各发出的 flit 数
  //   out_rdc   Xbar 送进 ReduceModule 三条 lane 的 flit 数之和
  //   cs_trig   CoreStation 发给 TS 的 trigger 数
  //   rdc_ctx   ReduceModule 占着几个用户上下文
  //   reissue   CoreMem 重发那一侧暂存着、还没重发出去的笔数
  //   retire    Retire 广播过的用户数
  //   cmcm_q    CreditMonitor 里排队等资源的申请数
  //
  // out_*、cs_trig、retire 是只加不清零的累计数，其余是水位。RouterTable 是静态
  // 配置，不记。
  void EmitRouter() {
    static const char* kOut[4] = {"out_mid", "out_left", "out_right", "out_core"};
    Xbar& xb = router->GetXbar();
    for (uint64_t o = 0; o < 4; ++o) TracePerCycle(kOut[o], xb.SentCount(o));
    TracePerCycle("out_rdc", xb.SentCount(kOutReduce0) + xb.SentCount(kOutReduce1) +
                                 xb.SentCount(kOutReduce2));
    TracePerCycle("cs_trig", router->GetCoreStation().Triggers());
    TracePerCycle("rdc_ctx", router->GetReduce().ContextUsed());
    CoreMemReissue& rs = router->GetReissue();
    uint64_t stored = rs.Stored(), reissued = rs.Reissued();
    TracePerCycle("reissue", stored >= reissued ? stored - reissued : 0);
    TracePerCycle("retire", router->GetRetire().Broadcast());
    TracePerCycle("cmcm_q", router->GetMonitor().QueueLen());
  }

  // 本拍新下发的那几笔 task，落成两个信号。
  //
  // 认「新的一笔」看的是三条发射通路各自的 seq：一笔命令会在端口上连着摆几拍
  // 等 RV core 收下，只看 cmd_valid 会把同一笔数很多遍。
  //
  //   ts_unit  位掩码，bit0 DTE、bit1 MU、bit2 VU。0 表示本拍没有新下发。
  //   ts_task  三路的 task 号各占 8 bit：dte | mu << 8 | vu << 16。
  //            那一路本拍没发就填 0xFF。
  //
  // 三条通路各管各的 stream，同一拍可以各发各的，所以两个信号都按路分位，不能
  // 只留一路。
  void EmitIssue() {
    TaskCmdPort* cmd[3] = {&ts->DteCmd(), &ts->MuCmd(), &ts->VuCmd()};
    uint64_t mask = 0, pack = 0;
    for (uint64_t u = 0; u < 3; ++u) {
      uint64_t s = cmd[u]->Seq();
      uint64_t id = 0xFFu;
      if (s != issue_seq[u]) {
        issue_seq[u] = s;
        mask |= 1ull << u;
        id = cmd[u]->task_id.Get() & 0xFFu;
      }
      pack |= id << (8 * u);
    }
    TracePerCycle("ts_unit", mask);
    TracePerCycle("ts_task", pack);
  }

  bool Quiescent() const override {
    if (!router->Quiescent()) return false;
    if (ctx.router_only) return true;
    for (auto const& r : rv) {
      if (!r->Quiescent()) return false;
    }
    return ts->Quiescent() && dte->Quiescent() && mu->Quiescent() &&
           vu->Quiescent();
  }

 private:
  // 业务模式下进核那一笔落哪块存储：R core 收到的两笔都落 Matrix Mem，它不参与
  // 计算，Core Mem 只在链二求和那几步用。B core 也落 Matrix Mem，它留下这一份
  // 再广播给本组各 core。
  Route BusinessInbound() const {
    return ctx.role == CoreRole::kReduce || ctx.role == CoreRole::kBroadcast
               ? Route::kRouterToMm
               : Route::kRouterToCm;
  }
  // B core 与 R core 上进来的包不建 stream 表项，进核那一笔不回 Ack。
  bool BusinessNoAck() const {
    return ctx.role == CoreRole::kReduce || ctx.role == CoreRole::kBroadcast;
  }

  void Wire() {
    // B core 的搬出查的是广播那几个方向的下游资源。方向由软件写进 TS 的
    // B_CORE_DIRECTION，Router 用的时候现读。
    if (ctx.role == CoreRole::kBroadcast) {
      router->SetBcastDirs([this] { return ts->Cfg().BCoreDirection(); });
    }
    WireRouterToTs();
    WireTsToRv();
    WireRvToDsa();
    WireRouterToDte();
    WireMemory();
  }

  // Router 与 TS 之间的四条控制线。
  void WireRouterToTs() {
    ts->AttachTrigger(router->TriggerPtr());
    ts->AttachCreditReq(router->CreditReqPtr());
    ts->AttachCreditGrant(router->CreditGrantPtr());
    ts->AttachRetire(router->RetireReqPtr());
    ts->AttachReduceDone(router->ReduceDonePtr());
    // Router 的 CoreMem 重发完成后报给 TS，TS 据此清掉那一项的重发标记。
    ts->AttachReissueDone(router->GetReissue().DonePtr());
  }

  // TS 下发 task 给三个 RV core，RV core 报完成回 TS。
  //
  // 一个 task 的共同形状：TS 把 task_pc 与 stream_id 配给一个 RV core → RV core
  // 向自己的 DSA 发一条异步任务指令后立刻向 TS 交还自己，不等 DSA 执行完 →
  // DSA 干完向 TS 回 ack，TS 据此推进同一 stream 的下一个 task。只做标量活的
  // task 不调 DSA，由 RV core 自己报完成。
  void WireTsToRv() {
    rv[0]->AttachCmd(ts->DteCmdPtr());
    rv[1]->AttachCmd(ts->MuCmdPtr());
    rv[2]->AttachCmd(ts->VuCmdPtr());
    for (uint64_t i = 0; i < 3; ++i) {
      rv[i]->AttachDone(ts->RvDonePtr(i));
    }
    dte->AttachToTs(ts->DsaDonePtr(0));
    mu->AttachDone(ts->DsaDonePtr(1));
    vu->AttachDone(ts->DsaDonePtr(2));
  }

  // 每个 RV core 只配自己那个 DSA 的寄存器，另有四个身份信号直连过去。
  void WireRvToDsa() {
    rv[0]->AttachDsaCfg(dte->CfgPtr());
    rv[1]->AttachDsaCfg(mu->CfgPtr());
    rv[2]->AttachDsaCfg(vu->CfgPtr());
    dte->AttachIds(rv[0]->DsaIdsPtr());
    // VU 的 stream_id 与 task_id 也从它那个 RV core 的 CSR 直连过来；MU 那一
    // 组由软件写进动态配置寄存器。
    vu->AttachIds(rv[2]->DsaIdsPtr());
    // 读 DSA 寄存器的返回值走独立的一根线回 dsa_rq。三个 RV core 各读各的那一个
    // DSA：MU 的软件轮询 SYS_STATUS 等一笔任务做完，VU 的轮询 macro_inst_left
    // 等这一批宏指令做完。
    rv[0]->AttachDsaRdata(dte->RdataPtr());
    rv[1]->AttachDsaRdata(mu->RdataPtr());
    rv[2]->AttachDsaRdata(vu->RdataPtr());
  }

  // 进 core 与出 core 两条数据通路完全并行，互不共享仲裁状态。
  void WireRouterToDte() {
    dte->AttachFromRouter(router->ToDtePtr());
    dte->AttachToRouter(router->FromDtePtr());
    // 出核任务发数据之前读 Xbar 发布的 VC credit 电平。
    dte->AttachVcLevel(router->CreditLevelPtr());
  }

  // 三块存储的各个 master 口。Matrix Mem 对三个 RV core 都不可见，VU 也读不到。
  // 要 Matrix Mem 里的数据得先由 DTE 搬到 Core Mem。
  void WireMemory() {
    // 读与写各占一个端口：存储那一侧只在 bank 冲突时才在读写之间二选一，
    // 合成一根线的话同一拍发出的读与写会互相盖掉。
    mu->AttachCmemRd(cmem->PortPtr(kCmemMuRd));
    mu->AttachCmemWr(cmem->PortPtr(kCmemMuWr));
    mu->AttachMmemRd(mmem->PortPtr(kMmemMu));
    vu->AttachCmemLd(cmem->PortPtr(kCmemVuRd));
    vu->AttachCmemSt(cmem->PortPtr(kCmemVuWr));
    // DTE 对每块存储的读与写各一个 master 口，五个通道在它自己的 DMA_XBAR
    // 里仲裁。
    dte->AttachCmemRd(cmem->PortPtr(kCmemDteRd));
    dte->AttachCmemWr(cmem->PortPtr(kCmemDteWr));
    dte->AttachMmemRd(mmem->PortPtr(kMmemDteRd));
    dte->AttachMmemWr(mmem->PortPtr(kMmemDteWr));
    // 三个 RV core 的 sm_lsq 各占 Share Mem 的一个口，DTE DSA 的 shareMem 写
    // 占第四个。
    rv[0]->AttachSmem(smem->PortPtr(kSmemDteRv));
    rv[1]->AttachSmem(smem->PortPtr(kSmemMuRv));
    rv[2]->AttachSmem(smem->PortPtr(kSmemVuRv));
    // DTE 搬完一笔之后写 shareMem 表项的那一路。
    dte->AttachSmemWr(smem->PortPtr(kSmemDteDsa));
    // cm_lsq 只有 DTE RV core 有，按地址范围分流到 Core Mem 与 Router 的包头
    // 读口。包头只有这一条读取通路，DTE DSA 不另接一条。
    rv[0]->AttachCmem(cmem->PortPtr(kCmemRvCore));
    rv[0]->AttachHdr(router->HdrPtr());
  }

  static void PokeWord(BankedMem& mem, uint64_t at, uint64_t data) {
    std::vector<uint8_t> word(4);
    for (int k = 0; k < 4; ++k) word[k] = uint8_t((data >> (8 * k)) & 0xFFu);
    mem.Poke(at, word);
  }

  CoreContext ctx;
  // 三条发射通路上一次见到的 seq，EmitIssue() 用它认新下发的那一笔。
  std::array<uint64_t, 3> issue_seq{};
  std::unique_ptr<Router> router;
  std::unique_ptr<Ts> ts;
  std::array<std::unique_ptr<RvCore>, 3> rv;
  std::unique_ptr<Dte> dte;
  std::unique_ptr<Mu> mu;
  std::unique_ptr<Vu> vu;
  std::unique_ptr<CoreMem> cmem;
  std::unique_ptr<MatrixMem> mmem;
  std::unique_ptr<ShareMem> smem;
};

}  // namespace bach
}  // namespace latch

#endif
