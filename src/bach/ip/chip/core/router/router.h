#ifndef _LATCH_BACH_IP_CHIP_CORE_ROUTER_ROUTER_
#define _LATCH_BACH_IP_CHIP_CORE_ROUTER_ROUTER_

// Router 这一组八类模块的装配。
//
// 自身没有 Cycle()：构造各模块、按接口把端口对接起来，一拍的工作全在各模块的
// Step() 里。将来 core.h 会把它连同 TS、三个 RV core、三个 DSA、三块存储一起装
// 成一个 Core。
//
// 里面装了几件：
//   RouterTable        1 份，7 个副本供并行查询的各位置各读各的
//   RouterStation      4 个：left / right / mid / core
//   Xbar               1 个，持有下游方向的 VC credit 与 Stream 表
//   CoreStation        1 个，进出核那一段
//   ReduceModule       1 个
//   CoreMemReissue     1 个
//   Retire             1 个
//   CreditMonitor      1 个
//
// 对外三组连接，与《Core》那一份的定位一致：
//   三个 R2R 方向端口   每个方向一对线，出去的与回来的
//   进 core 与出 core   接本 core 的 DTE
//   控制信号            接 TS：trigger、credit 申请与授予、reduce_done、retire
//
// 不派角色的 core 只构造 Router 这一组，TS、RV core、DSA 与存储都不构造。把
// pass_through 打开，各 station 与 Xbar 就只转发不记账。

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "bach/ip/chip/core/router/core_station.h"
#include "bach/ip/chip/core/router/coremem_reissue.h"
#include "bach/ip/chip/core/router/credit_monitor.h"
#include "bach/ip/chip/core/router/reduce_module.h"
#include "bach/ip/chip/core/router/retire.h"
#include "bach/ip/chip/core/router/router_station.h"
#include "bach/ip/chip/core/router/xbar.h"
#include "bach/ip/wiring.h"

namespace latch {
namespace bach {

struct RouterCfg {
  // 只透传的 core：不投递本 core、不记账、不参与重发。
  bool pass_through = false;
  // 溢流重发的暂存容量，取自 cmem_part 的 reissue_pkts_per_vc。
  uint64_t reissue_pkts_per_vc = 2;
  // VC Buffer 的两级深度，软件跑起来之前经 ctrl_noc 配。本级各入方向按它判收得
  // 下收不下，Xbar 的下游 credit 初值取同一组数。
  VcDepth vc_private_depth = kVcPrivateDepthDefault;
  uint64_t vc_shared_depth = kVcSharedDepth;
  // 各模块自己挂时钟，还是由外层统一驱动。
  //
  // 一个 Router 是 11 个模块，也就是 11 个常驻协程。协程槽位总数是
  // sub_thread × co_thread（默认 8 × 1 = 8），槽位不够时多余的协程排在 pending
  // 里永远等不到空位，表现为进程卡住而不是报错。48 chip × 10 core 的规模下这个
  // 数是两万多，远超槽位上限。
  //
  // 关掉 tick，Router 就只占一个协程：外层在自己的 Cycle() 里顺序调 RunStep()。
  // 因为跨模块信号全部打拍，两种驱动方式的结果逐拍相同。
  bool tick = true;
};

class Router {
 public:
  Router(ClockPtr clock, const std::string& name, RouterCfg const& setting,
         uint64_t parent = 0)
      : clk(clock), cfg(setting) {
    const uint64_t gid = TraceGroup(name, parent);
    rtab = std::make_unique<RouterTable>(clock, "rtab", gid, setting.tick);

    // 四个入口站：三个 R2R 加 core 方向。各读一份副本。ReduceModule 的结果
    // 直接给 Xbar 提请求，不再走一遍站 —— 方向在它那里已经算好了。
    static const char* kStationName[] = {"st_mid", "st_left", "st_right",
                                         "st_core"};
    static const uint64_t kStationDir[] = {kDirMid, kDirLeft, kDirRight,
                                           kDirCore};
    for (uint64_t i = 0; i < 4; ++i) {
      auto st = std::make_unique<RouterStation>(
          clock, kStationName[i], kStationDir[i], *rtab, i, gid,
          setting.tick);
      st->SetPassThrough(cfg.pass_through);
      st->SetVcDepth(cfg.vc_private_depth, cfg.vc_shared_depth);
      stations.push_back(std::move(st));
    }

    xbar = std::make_unique<Xbar>(clock, "xbar", gid, setting.tick);
    xbar->SetPassThrough(cfg.pass_through);
    xbar->SetVcDepth(cfg.vc_private_depth, cfg.vc_shared_depth);
    core_station = std::make_unique<CoreStation>(clock, "core_station",
                                                 gid, setting.tick);
    reduce = std::make_unique<ReduceModule>(clock, "reduce", *rtab, 4,
                                            gid, setting.tick);
    reissue = std::make_unique<CoreMemReissue>(clock, "reissue",
                                               cfg.reissue_pkts_per_vc, gid,
                                               setting.tick);
    reissue->SetPassThrough(cfg.pass_through);
    retire = std::make_unique<Retire>(clock, "retire", gid, setting.tick);
    monitor = std::make_unique<CreditMonitor>(clock, "monitor", *rtab, 5,
                                              gid, setting.tick);

    Wire();
  }

  // ── 对外：三个 R2R 方向 ──
  // 上游发过来的数据落在 in_wire[d]，本级往下游发的数据出现在 out_wire[d]，
  // 下游还回来的 credit 落在 back_wire[d]。
  LinkEndPtr InWire(uint64_t d) const { return in_wire.at(d); }
  LinkEndPtr OutWire(uint64_t d) const { return out_wire.at(d); }
  LinkEndPtr BackWire(uint64_t d) const { return back_wire.at(d); }
  // 本级还给上游的 VC credit 走这根，由 RouterStation 写。
  LinkEndPtr UpBackWire(uint64_t d) const { return up_back_wire.at(d); }
  // 业务 credit 的回程（stream release）另走一根，由 Retire 写。
  //
  // 分成两根不是实现上的将就：链路那一份说的就是「数据与三种 release 各走各的
  // 实例，参数相同」。合在一根上会出现两个模块同一拍写同一个 Latch —— VC credit
  // 是 flit 一进一出就还的快通道，stream release 要等那个用户在下游 core 上跑完
  // 整条任务链，两者的产生点本来就不是一处。
  LinkEndPtr UpReleaseWire(uint64_t d) const { return up_release_wire.at(d); }

  // ── 对外：进出 core，接本 core 的 DTE ──
  CoreDataPort& ToDte() { return core_station->ToDte(); }
  CoreDataPort& FromDte() { return core_station->FromDte(); }
  std::shared_ptr<CoreDataPort> ToDtePtr() const {
    return core_station->ToDtePtr();
  }
  std::shared_ptr<CoreDataPort> FromDtePtr() const {
    return core_station->FromDtePtr();
  }
  // 溢流重发从 Core Mem 取出来的那一路。等 DTE 建起来由它接走：真硬件上重发是
  // DTE 从 Core Mem 读出来再经 out_core_data_ch 发回 Router，不是 Router 自己
  // 绕回去。这里先留着口子，不接一条设计里没有的路径。
  LinkEndPtr ReissueOutWire() const { return reissue_out; }

  // ── 对外：控制信号，接 TS ──
  TriggerPort& Trigger() { return core_station->Trigger(); }
  ReduceDonePort& ReduceDone() { return reduce->Done(); }
  CreditReqPort& CreditReq() { return monitor->Req(); }
  CreditGrantPort& CreditGrant() { return monitor->Grant(); }
  RetirePort& RetireReq() { return retire->Req(); }
  // 装配层把这几根线交给 TS，两端指向同一个对象。
  std::shared_ptr<TriggerPort> TriggerPtr() const {
    return core_station->TriggerPtr();
  }
  std::shared_ptr<ReduceDonePort> ReduceDonePtr() const {
    return reduce->DonePtr();
  }
  std::shared_ptr<CreditReqPort> CreditReqPtr() const {
    return monitor->ReqPtr();
  }
  std::shared_ptr<CreditGrantPort> CreditGrantPtr() const {
    return monitor->GrantPtr();
  }
  std::shared_ptr<RetirePort> RetireReqPtr() const { return retire->ReqPtr(); }

  // ── 配置面 ──
  RouterTable& Table() { return *rtab; }
  // boot 期一次性铺表。逐笔写走 Table().Write()。
  void Preload(uint64_t path_id, RouteEntry const& e) {
    rtab->Preload(path_id, e);
  }
  // 用户建 stream credit 表项时，ReduceModule 的上下文同时占用。两者一一对应。
  void AllocUser(uint64_t user) { reduce->AllocContext(user); }

  // ── 观测 ──
  // B core：搬出之前要查哪几个方向的下游资源。方向在 TS 的 B_CORE_DIRECTION
  // 里，跑起来之前由软件配，所以接的是取值的办法而不是当时的值。
  void SetBcastDirs(std::function<uint64_t()> fn) { bcast_dirs = std::move(fn); }
  Xbar& GetXbar() { return *xbar; }
  CoreStation& GetCoreStation() { return *core_station; }
  // 包头读口，接 DTE RV core 的 cm_lsq。
  std::shared_ptr<MemPort> HdrPtr() const { return core_station->HdrPtr(); }
  ReduceModule& GetReduce() { return *reduce; }
  CoreMemReissue& GetReissue() { return *reissue; }
  RouterStation& Station(uint64_t i) { return *stations.at(i); }
  // Xbar 每拍发布的各方向各 VC 可发标志。DTE 出核前读它。
  std::shared_ptr<CreditLevelPort> CreditLevelPtr() const {
    return xbar->LevelPtr();
  }

  // tick 关掉时由外层每拍调一次。顺序照「末级先做」：先让下游把东西收走，
  // 上游本拍腾出来的位置才用得上。模块之间的信号都打拍，所以这个顺序只影响
  // 同一拍内谁先算，不影响谁读到什么。
  void RunStep() {
    rtab->RunStep();
    reissue->RunStep();
    reduce->RunStep();
    core_station->RunStep();
    xbar->RunStep();
    for (auto& st : stations) st->RunStep();
    retire->RunStep();
    monitor->RunStep();
  }

  bool Quiescent() const {
    for (auto const& st : stations) {
      if (!st->Quiescent()) return false;
    }
    return xbar->Quiescent() && core_station->Quiescent() &&
           reduce->Quiescent() && reissue->Quiescent() && retire->Quiescent() &&
           monitor->Quiescent();
  }

 private:
  void Wire() {
    // 三个 R2R 方向：外部链路接进站，Xbar 的出口接出去。
    for (uint64_t d = 0; d < kR2RNum; ++d) {
      in_wire.push_back(MakeWire(clk));
      out_wire.push_back(MakeWire(clk));
      back_wire.push_back(MakeWire(clk));
      up_back_wire.push_back(MakeWire(clk));
      up_release_wire.push_back(MakeWire(clk));
      stations[d]->AttachUp(in_wire[d]);
      stations[d]->AttachUpBack(up_back_wire[d]);
      xbar->AttachOutLink(d, out_wire[d]);
      xbar->AttachBackLink(d, back_wire[d]);
    }

    // 五个站的请求口接进 Xbar 的五个输入。
    xbar->AttachReq(kInMid, stations[0]->ReqPtr());
    xbar->AttachReq(kInLeft, stations[1]->ReqPtr());
    xbar->AttachReq(kInRight, stations[2]->ReqPtr());
    xbar->AttachReq(kInLocal, stations[3]->ReqPtr());
    xbar->AttachReq(kInReduce, reduce->ReqPtr());
    for (auto& st : stations) st->AttachLevel(xbar->LevelPtr());

    // core 方向：Xbar 的 core 出口进 CoreStation，CoreStation 拆出来的出核数据
    // 回到 core 方向那个站。
    LinkEndPtr to_core = MakeWire(clk);
    xbar->AttachCoreOut(to_core);
    core_station->AttachFromXbar(to_core);
    LinkEndPtr out_core = MakeWire(clk);
    core_station->AttachToStation(out_core);
    stations[3]->AttachUp(out_core);
    stations[3]->AttachUpBack(MakeWire(clk));
    xbar->AttachCoreLevel(core_station->LevelPtr());

    // 三路 reduce 出口进 ReduceModule，结果经第五个站重新参与仲裁。
    for (uint64_t r = 0; r < 3; ++r) {
      LinkEndPtr w = MakeWire(clk);
      xbar->AttachReduceOut(r, w);
      reduce->AttachIn(r, w);
      xbar->AttachReduceLevel(r, reduce->LevelPtr());
    }
    // 溢流：Xbar 判定转存后把 flit 交给 CoreMemReissue。
    reissue->AttachOverflow(xbar->OverflowPtr());
    reissue_out = MakeWire(clk);
    reissue->AttachOut(reissue_out);

    // Retire 广播到三处。顺序按设计：Router 立即删 stream 授权，CoreStation 放
    // 进核的坑，ReduceModule 只记下、等 credit 全回来才删。
    // 退休广播走一根线：Retire 送号，三张表各由自己的模块改。
    auto bcast = retire->BroadcastPtr();
    xbar->AttachRetire(bcast);
    core_station->AttachRetire(bcast);
    reduce->AttachRetire(bcast);
    for (uint64_t d = 0; d < kR2RNum; ++d) {
      retire->AttachUpLink(d, up_release_wire[d]);
    }

    // CreditMonitor 查的是进核那一侧的坑：Router 的进 core 表与 TS 内部的表按
    // 完全一致的逻辑申请空项，所以「Router 通知 TS 的包一定能被 TS 接收」。
    monitor->SetCheck([this](uint64_t user, uint64_t) {
      // B core 的搬出查的是广播那几个方向的下游资源，不是本 core 的进核资源：
      // 它不算数，进来的 token 只在自己的 Matrix Mem 里存一份再发出去。
      if (bcast_dirs) {
        uint64_t mask = bcast_dirs();
        if (mask != 0) return xbar->BroadcastRoom(mask, user);
      }
      return core_station->HoldsUser(user) ||
             core_station->StreamUsed() < kStreamTabEntries;
    });
    monitor->SetTake([this](uint64_t user, uint64_t) {
      reduce->AllocContext(user);
    });
  }

  ClockPtr clk;
  RouterCfg cfg;
  // B core 往哪几个方向广播。值在 TS 的 B_CORE_DIRECTION 里，跑起来之前由软件
  // 配，所以这里存的是取值的办法，用的时候现读。
  std::function<uint64_t()> bcast_dirs;

  std::unique_ptr<RouterTable> rtab;
  std::vector<std::unique_ptr<RouterStation>> stations;
  std::unique_ptr<Xbar> xbar;
  std::unique_ptr<CoreStation> core_station;
  std::unique_ptr<ReduceModule> reduce;
  std::unique_ptr<CoreMemReissue> reissue;
  std::unique_ptr<Retire> retire;
  std::unique_ptr<CreditMonitor> monitor;

  std::vector<LinkEndPtr> in_wire, out_wire, back_wire, up_back_wire,
      up_release_wire;
  LinkEndPtr reissue_out;
};

}  // namespace bach
}  // namespace latch

#endif
