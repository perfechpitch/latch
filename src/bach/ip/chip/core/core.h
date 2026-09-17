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
// 七个单元一律构造。坏 core 由 core_bad_mask 标出：Router 进透传档，TS、RV core、
// DSA 与存储不步进、不记波形。
//
// core 不保存角色，行为逐项取自配置：自启动取自 TS 的 SELF_START，B core 搬出查
// 哪几个方向取自 B_CORE_DIRECTION，进核落点、Ack 与标志表取自 DTE 的进核配置。
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

// 只读上下文，core 内各模块共用。构造期写入，之后不变。
struct CoreContext {
  uint64_t core_id = 0;      // 本 chip 内的编号
  uint64_t gx = 0, gy = 0;   // 全局坐标
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
    rcfg.core_id = ctx.core_id;
    rcfg.tick = false;
    // 波形上一个 core 记两层：EmitTrace() 那组精选信号直接挂在 core 下面，外加
    // 底下各单元模块自己的全部信号（层次 chip<i>.core<j>.<单元>.<模块>.<信号>）。
    // 全量在 48 chip 规模下信号上万、波形很大，只想看流程就把下一行取消注释、
    // 退回只发精选信号（或调小 moe_lpu 的 kTraceChips）。
    // TraceOffScope off;
    router = std::make_unique<Router>(clock, "router", rcfg, Id());

    TsCfg tcfg;
    tcfg.tick = false;
    ts = std::make_unique<Ts>(clock, "ts", tcfg, Id());

    DteCfg dcfg;
    dcfg.tick = false;
    dcfg.inbound = business_in;
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

  // core_bad_mask：本 chip 每个 core 一位，SCP 在 Router 配置阶段写，业务开始之
  // 前写完。本 core 那一位为 1 就是坏 core。
  void SetCoreBadMask(uint64_t mask) { router->SetCoreBadMask(mask); }
  bool Bad() const { return router->Bad(); }

  // 业务模式下进核那一笔的配置，对应 bundle 的 DTEIN，SCP 在 core 配置阶段写，
  // 写入即生效。
  void SetBusinessInboundCfg(InboundCfg const& in) {
    business_in = in;
    dte->SetInbound(in);
  }
  // 切进 weights 加载模式：这一阶段进来的是权重，落 Matrix Mem；这一阶段不建
  // stream 表项，进核那一笔没有可报的对象，不回 Ack，也不置标志。
  void SetWeightsInbound() {
    InboundCfg in;
    in.route = Route::kRouterToMm;
    in.no_ack = true;
    dte->SetInbound(in);
  }
  // 切回业务模式：按写入的业务模式配置重配。
  void SetBusinessInbound() { dte->SetInbound(business_in); }

  CoreContext const& Context() const { return ctx; }
  // 三个 RV core 都进 wait 后拉高，SCP 据此开放业务接收权限。坏 core 恒为真。
  bool Ready() const {
    if (Bad()) return true;
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
        // Router 段经 ctrl_noc 只收 core_bad_mask，RouterTable 由装载那一侧铺。
        if (r.offset == kCfgRouterCoreBadMask) router->SetCoreBadMask(data);
        break;
      case kCfgTs:
        ts->Cfg().SetStreamNum(data == 0 ? 1 : data);
        break;
      case kCfgDte:
        // DTE 的寄存器由它的 Commit 那一侧收，boot 期只配 Hmem 的表。
        break;
      case kCfgMu:
        mu->Regfile().CfgWrite(r.offset, data);
        break;
      case kCfgVu:
        // VU 有三条独立的配置通路，Ctrl-NOC 占其中一条，走端口。
        break;
      case kCfgItcm: {
        std::vector<uint8_t> word(4);
        for (int k = 0; k < 4; ++k) word[k] = uint8_t((data >> (8 * k)) & 0xFFu);
        rv[r.index]->PokeItcm(r.offset, word);
        break;
      }
      case kCfgSmem:
        PokeWord(*smem, r.offset, data);
        break;
      case kCfgCmem:
        PokeWord(*cmem, r.offset, data);
        break;
      case kCfgMmem:
        PokeWord(*mmem, r.offset, data);
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
  // 坏 core 只有 Router 步进。
  void Step() override {
    router->RunStep();
    if (Bad()) {
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
    // 坏 core 与 TS 没配过任务的好 core 只有 Router 在用，只记上面那一组。
    if (Bad() || !ts->Cfg().HasTask()) return;
    TracePerCycle("core_out", dte->OutBuf().Occupancy());
    TracePerCycle("ts_inflight", ts->Table().InFlight());
    TracePerCycle("ts_issue", ts->DteArbiter().Issued() +
                                  ts->MuArbiter().Issued() +
                                  ts->VuArbiter().Issued());
    TracePerCycle("ts_done", ts->Done().Finished());
    EmitIssue();
    EmitStep();
    EmitDsa();
    EmitRv();
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

  // 本拍新下发的那几笔 task，落成三个信号。
  //
  // 认「新的一笔」看的是三条发射通路各自的 seq：一笔命令会在端口上连着摆几拍
  // 等 RV core 收下，只看 cmd_valid 会把同一笔数很多遍。
  //
  //   ts_unit  位掩码，bit0 DTE、bit1 MU、bit2 VU。0 表示本拍没有新下发。
  //   ts_task  三路的 task 号各占 8 bit：dte | mu << 8 | vu << 16。
  //            那一路本拍没发就填 0xFF。
  //   ts_user  三路的 user_id 各占 16 bit：dte | mu << 16 | vu << 32。
  //            那一路本拍没发就填 0xFFFF。
  //
  // 波形只在值变了的那一拍记一个事件。同一路连着几拍都下发时 ts_unit 几拍同值，
  // 事件只落在头一拍；三个信号每拍都发，数下发要把非零的那一段逐拍展开。
  //
  // 三条通路各管各的 stream，同一拍可以各发各的，所以三个信号都按路分位，不能
  // 只留一路。
  void EmitIssue() {
    TaskCmdPort* cmd[3] = {&ts->DteCmd(), &ts->MuCmd(), &ts->VuCmd()};
    uint64_t mask = 0, pack = 0, users = 0;
    for (uint64_t u = 0; u < 3; ++u) {
      uint64_t s = cmd[u]->Seq();
      uint64_t id = 0xFFu, user = 0xFFFFu;
      if (s != issue_seq[u]) {
        issue_seq[u] = s;
        mask |= 1ull << u;
        id = cmd[u]->task_id.Get() & 0xFFu;
        user = cmd[u]->user_id.Get() & 0xFFFFu;
      }
      pack |= id << (8 * u);
      users |= user << (16 * u);
    }
    TracePerCycle("ts_unit", mask);
    TracePerCycle("ts_task", pack);
    TracePerCycle("ts_user", users);
  }

  // 本拍 TS 里新出现的这一步，与它被下发那一拍。
  //
  //   ts_create   建表笔数（单调）：新用户到了建一个表项，或自启动 core 上一条链
  //               退休后原地重新激活。
  //   ts_install  装后继笔数（单调）：上一步做完，TaskCtrl 把下一项写进表项。
  //
  // 两者是同一件事的两种来源 —— 第一个 task 走 create，其余走 install —— 都是
  // 「这一步进入 TS、可以被下发了」。与 ts_unit/ts_task/ts_user 里那一笔下发对
  // 起来，差值就是这一步在 TS 里等的时间：等 credit、等发射通路空出来、等前一笔
  // 从 RV core 那边腾出槽位。不含 RV core 与 DSA 的任何时间。
  //
  // 身份与 ts_task / ts_user 同宽：task 8 bit、user 16 bit，本拍没有就填
  // 0xFF / 0xFFFF。自启动 core 建表时还没有用户身份，user 也填 0xFFFF。
  //
  // DataIn 任务（Bypass 档从 DataIn_task_table 直接下发的那一路）既没有建表也没
  // 有装后继，它在 ts_unit 上出现时配不上这两个信号。
  void EmitStep() {
    // 自启动 core 上的重新激活不经 User_Match，笔数从 credit 与退休那一侧取。
    // 那一步是 Task 0，还没有用户身份。
    uint64_t made = ts->Matcher().Made();
    uint64_t again = ts->Credit().Reactivated();
    bool fresh = made != ts_create_seq;
    bool renewed = again != ts_reactivate_seq;
    ts_create_seq = made;
    ts_reactivate_seq = again;
    TracePerCycle("ts_create", made + again);
    TracePerCycle("ts_create_task",
                  fresh ? (ts->Matcher().MadeTask() & 0xFFu)
                        : (renewed ? 0u : 0xFFu));
    TracePerCycle("ts_create_user",
                  (fresh && ts->Matcher().MadeUserValid())
                      ? (ts->Matcher().MadeUser() & 0xFFFFu)
                      : 0xFFFFu);

    uint64_t install = ts->Ctrl().Installed();
    bool next = install != ts_install_seq;
    ts_install_seq = install;
    TracePerCycle("ts_install", install);
    TracePerCycle("ts_install_task",
                  next ? (ts->Ctrl().NextTask() & 0xFFu) : 0xFFu);
    TracePerCycle("ts_install_user",
                  (next && ts->Ctrl().NextUserValid())
                      ? (ts->Ctrl().NextUser() & 0xFFFFu)
                      : 0xFFFFu);
  }

  // 三个 DSA 各自对一笔 task 的执行时间，两个端点各发三个信号。
  //
  //   dsa_start  位掩码，bit0 DTE、bit1 MU、bit2 VU —— 与 ts_unit 同位序。
  //   dsa_done   位掩码，同上。这是 DSA 自己的完成脉冲，也就是它发给 TS 的那一路。
  //
  // 起点取的是「过门槛」那一拍，不是写 trigger 那一拍。写 trigger 只是把任务收
  // 进各自的寄存器，真正开始还要过一道闸，而且三个单元的闸门各不相同：
  //   DTE  过 Commit 准入，Lane 与 Completion RS 三样资源都拿得到。之前还要在
  //        PendingTaskQ 里等 VC credit。只算 RV core 那一路，Router 入站那一路
  //        不算一笔 DTE task。
  //   MU   进 issue_q，drain 走完且队列有空位。
  //   VU   被 ISQ 收下，静态配置已释放、上一条已被取走。
  // 这三个闸门等多久都可能，所以「收下任务」与「开始算」不能混为一谈。
  //
  // 终点是各家把完成报回来的那一拍：
  //   DTE  两侧完成条件配齐、且带 task_last 的那一笔报到 TS，no_ack 的不报。
  //   MU   整个 task 的 tile 都写回、写回落地后再空两拍。
  //   VU   每条宏指令退休报一次，与它发给 TS 的是同一拍同一笔。一个 task 发几条
  //        就会报几次 —— VU 不知道 task 的边界，那是软件的事。要「这笔 task 从下
  //        发到最后一条宏指令做完」的整段，看 TS 那一行：它会把这一笔的几段并起来。
  //        配了 kRvOnly 的档上 TS 收的是 RV core 轮询之后的 ACK，那一路在 rv_done 里。
  //
  // 身份与 ts_task / ts_user 同宽：task 8 bit、user 16 bit，本拍没有就填
  // 0xFF / 0xFFFF。三家的完成脉冲本身都不带 user（Drive 只给 stream 与 task），
  // 这里填的是各单元侧存下来的那一份：MU 与 DTE 取自 RV core 写进 DSA 的 user，
  // VU 取自写 trigger 那一拍从身份直连线上采下来、随宏指令一路带到退休的 user。
  void EmitDsa() {
    uint64_t start_mask = 0, start_task = 0, start_user = 0;
    uint64_t done_mask = 0, done_task = 0, done_user = 0;
    for (uint64_t u = 0; u < 3; ++u) {
      DsaEv s = DsaStartOf(u);
      if (s.seq != dsa_start_seq[u]) {
        dsa_start_seq[u] = s.seq;
        start_mask |= 1ull << u;
        start_task |= (s.task & 0xFFu) << (8 * u);
        start_user |= (s.user & 0xFFFFu) << (16 * u);
      }
      DsaEv d = DsaDoneOf(u);
      if (d.seq != dsa_done_seq[u]) {
        dsa_done_seq[u] = d.seq;
        done_mask |= 1ull << u;
        done_task |= (d.task & 0xFFu) << (8 * u);
        done_user |= (d.user & 0xFFFFu) << (16 * u);
      }
    }
    TracePerCycle("dsa_start", start_mask);
    TracePerCycle("dsa_start_task", start_task);
    TracePerCycle("dsa_start_user", start_user);
    TracePerCycle("dsa_done", done_mask);
    TracePerCycle("dsa_done_task", done_task);
    TracePerCycle("dsa_done_user", done_user);
  }

  // 三个 RV core 各自对一笔 task 的执行时间，两个端点各发三个信号。
  //
  //   rv_start  位掩码，bit0 DTE、bit1 MU、bit2 VU —— 与 ts_unit 同位序。
  //             1 表示这一拍执行器接下了队头那笔，PC 跳到了 task_pc。
  //   rv_done   位掩码，同上。1 表示这一拍那笔 task 从 kernel 交还了。
  //
  // 起点的判据是 RvTaskQueue::Launch() 里握手完成那一拍；终点是 kernel 执行
  // task_done 那一拍。两端都带身份，相减就是这个 RV core 对这笔 task 的执行
  // 时间。交还是一定发生的，通不通知 TS 由 kernel 写进 task_done 的值定，波形
  // 上以交还为准，这样不通知 TS 的那几档也有闭合点。
  //
  // 窗口含 kernel 里轮询 DSA 的时间（MU 的 mu_wait()、VU 轮询 MACRO_INST_LEFT），
  // 不含 DSA 自己的执行时间。
  void EmitRv() {
    uint64_t start_mask = 0, start_task = 0, start_user = 0;
    uint64_t done_mask = 0, done_task = 0, done_user = 0;
    for (uint64_t u = 0; u < 3; ++u) {
      RvTaskQueue& tq = rv[u]->TaskQueue();
      uint64_t s = tq.Started();
      if (s != rv_start_seq[u]) {
        rv_start_seq[u] = s;
        start_mask |= 1ull << u;
        start_task |= (tq.StartTask() & 0xFFu) << (8 * u);
        start_user |= (tq.StartUser() & 0xFFFFu) << (16 * u);
      }
      uint64_t d = tq.Finishes();
      if (d != rv_done_seq[u]) {
        rv_done_seq[u] = d;
        done_mask |= 1ull << u;
        done_task |= (tq.DoneTask() & 0xFFu) << (8 * u);
        done_user |= (tq.DoneUser() & 0xFFFFu) << (16 * u);
      }
    }
    TracePerCycle("rv_start", start_mask);
    TracePerCycle("rv_start_task", start_task);
    TracePerCycle("rv_start_user", start_user);
    TracePerCycle("rv_done", done_mask);
    TracePerCycle("rv_done_task", done_task);
    TracePerCycle("rv_done_user", done_user);
  }

  bool Quiescent() const override {
    if (!router->Quiescent()) return false;
    if (Bad()) return true;
    for (auto const& r : rv) {
      if (!r->Quiescent()) return false;
    }
    return ts->Quiescent() && dte->Quiescent() && mu->Quiescent() &&
           vu->Quiescent();
  }

 private:
  void Wire() {
    // B core 的搬出查的是广播那几个方向的下游资源。方向由软件写进 TS 的
    // B_CORE_DIRECTION，Router 用的时候现读；为 0 时按本 core 的进核资源查。
    router->SetBcastDirs([this] { return ts->Cfg().BCoreDirection(); });
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
  // 业务模式下进核那一笔的配置。切回业务模式时按它重配。
  InboundCfg business_in;
  // 三个 DSA 的两个端点各要从一家的访问器上读。u：0 DTE、1 MU、2 VU。
  struct DsaEv {
    uint64_t seq = 0, task = 0, user = 0xFFFFu;
  };

  DsaEv DsaStartOf(uint64_t u) {
    if (u == 0) {
      Commit& c = dte->Committer();
      return {c.RvAdmitted(), c.StartTask(), c.StartUser()};
    }
    if (u == 1) {
      MuIssueQ& iq = mu->IssueQ();
      return {iq.Issued(), iq.StartTask(), iq.StartUser()};
    }
    VuIsq& isq = vu->Isq();
    return {isq.Started(), isq.StartTask(), isq.StartUser()};
  }

  DsaEv DsaDoneOf(uint64_t u) {
    if (u == 0) {
      CompletionRs& c = dte->Completion();
      return {c.Reported(), c.DoneTask(), c.DoneUser()};
    }
    if (u == 1) {
      return {mu->DoneCnt(), mu->DoneTask(), mu->DoneUser()};
    }
    // VU 每条宏指令退休报一次，与它发给 TS 的是同一拍。
    return {vu->Retire().MacroDoneCnt(), vu->Retire().MacroDoneTask(),
            vu->Retire().MacroDoneUser()};
  }

  // 三条发射通路上一次见到的 seq，EmitIssue() 用它认新下发的那一笔。
  std::array<uint64_t, 3> issue_seq{};
  // 三个 DSA 上一次见到的起/完笔数，EmitDsa() 用它认本拍新发生的那一笔。
  std::array<uint64_t, 3> dsa_start_seq{}, dsa_done_seq{};
  // 三个 RV core 上一次见到的起/完笔数，EmitRv() 用它认本拍新发生的那一笔。
  std::array<uint64_t, 3> rv_start_seq{}, rv_done_seq{};
  // 上一次见到的建表、重新激活与装后继笔数，EmitStep() 用它认本拍新出现的那一步。
  uint64_t ts_create_seq = 0, ts_reactivate_seq = 0, ts_install_seq = 0;
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
