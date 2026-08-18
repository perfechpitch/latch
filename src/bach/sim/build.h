#ifndef _LATCH_BACH_SIM_BUILD_
#define _LATCH_BACH_SIM_BUILD_

// 从中间文件的读入结果装配出整套硬件。
//
// 装配的顺序不能随便调，前三步之间有硬性先后：
//
//   1 路由表  外部节点的坐标与它的网关、各 chip 朝四个方向的出口网关。它必须最先备
//             好，因为每个节点构造时就要查它判断自己在不在阵列内
//   2 核      每个核一份只读上下文，指向它自己那张任务表
//   3 连线    先片内 mesh，再片间 PCIe，最后把阵列外的节点接到各自的网关核上
//   4 交换网  显式的 PCIe 交换拓扑：建交换节点、接线、填两张路由表，再把交换节点能
//             送到的目的地登记到与它相连的那些核上
//   5 额度    按每条验资任务列出的下游开户，额度看下游核的类型
//   6 注入    每个注入源拿到自己那份 user 名单，汇聚点拿到每个 user 的期望分片数
//   7 Phase   以太网交换节点、Phase1 那一段的通道与落点、注入 Phase2 那道口子、
//             残差与结果的汇合点。它排在最后，因为它要用到前面建好的注入源与汇聚点
//
// 编译产物里没有的东西，这里一概不猜：某个核没有任务表就给它一张空表，某条线的带宽
// 或延迟写 0 就按端口类型取全局默认。猜出来的拓扑跑出来的时间没有出处。

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/log.h"
#include "bach/common/packet.h"
#include "bach/ip/system.h"
#include "bach/tables/loader.h"

namespace latch {
namespace bach {

namespace detail {

// 下游核的额度上限：广播与归约类的下游按 Matrix FIFO 的深度开户，其余按 stream 数。
inline uint64_t CreditCapacityFor(Params const& p, CoreType type) {
  return type == CoreType::kBroadcast || type == CoreType::kReduction
             ? p.matrix_fifo_credit
             : p.stream_count;
}

// 注入源该发哪种入口包，看入口核任务表第一行写的是什么。这一条不另外登记：任务表里
// 已经有了，再登记一份就多一处会对不上的地方。
inline Opcode EntryOpcode(BachIr const& ir, uint64_t core_id) {
  CoreTaskTable const* tasks = ir.Tasks(core_id);
  LOGCHECK(tasks != nullptr && tasks->Has(0),
           "BuildSystem: an entry core has no task table.");
  const Opcode op = tasks->At(0).opcode;
  LOGCHECK(op == Opcode::kUserInit || op == Opcode::kFifoIn,
           "BuildSystem: an entry core's first row is neither USER_INIT nor "
           "FIFO_IN.");
  return op;
}

inline CoreType TypeOfCore(BachIr const& ir, uint64_t core_id) {
  auto it = ir.cores.find(core_id);
  return it == ir.cores.end() ? CoreType::kNormal : it->second.type;
}

// 显式 PCIe 交换拓扑。四步，前后有依赖：口要先开出来才接得上线，线要先接上才查得了
// 一条路由的出口落在哪，路由表要先填好才知道该给核登记哪些目的地。
inline void WirePcieFabric(System& sys, BachIr const& ir,
                           std::unordered_map<std::string, Router*> const& ext,
                           uint64_t* next_node) {
  if (ir.pcie_switches.empty()) return;

  for (PcieSwitchInfo const& s : ir.pcie_switches) {
    sys.AddPcieSwitch((*next_node)++, s.id, s.coord, "pcie_sw_" + s.id);
  }

  for (PcieLink const& l : ir.pcie_links) {
    for (PcieEnd const* e : {&l.a, &l.b}) {
      if (e->kind == PcieEndKind::kSwitch) {
        sys.SwitchById(e->ref).AddPort(e->port_name);
      }
    }
  }

  // 核与交换节点之间的每一条线。路由表填好之后要按它给核登记出口。
  struct CoreLink {
    uint64_t core_id = 0;
    Port port = Port::kUnknown;
    std::string sw_id;
    std::string sw_port;
  };
  std::vector<CoreLink> core_links;

  for (PcieLink const& l : ir.pcie_links) {
    if (l.a.kind == PcieEndKind::kSwitch && l.b.kind == PcieEndKind::kSwitch) {
      PcieSwitch& x = sys.SwitchById(l.a.ref);
      PcieSwitch& y = sys.SwitchById(l.b.ref);
      AttachSwitchLink(x, x.PortOf(l.a.port_name), y, y.PortOf(l.b.port_name),
                       l.bandwidth, l.delay);
      continue;
    }
    PcieEnd const& sw_end = l.a.kind == PcieEndKind::kSwitch ? l.a : l.b;
    PcieEnd const& other = l.a.kind == PcieEndKind::kSwitch ? l.b : l.a;
    PcieSwitch& sw = sys.SwitchById(sw_end.ref);
    const uint32_t sw_port = sw.PortOf(sw_end.port_name);

    if (other.kind == PcieEndKind::kCore) {
      const uint64_t core_id = std::strtoull(other.ref.c_str(), nullptr, 10);
      AttachSwitchLink(sys.CoreById(core_id).Rt(), other.port, sw, sw_port,
                       l.bandwidth, l.delay);
      CoreLink item;
      item.core_id = core_id;
      item.port = other.port;
      item.sw_id = sw_end.ref;
      item.sw_port = sw_end.port_name;
      core_links.push_back(std::move(item));
      continue;
    }

    auto it = ext.find(other.ref);
    LOGCHECK(it != ext.end(), "BuildSystem: a PCIe link names an unknown "
                              "external node.");
    AttachSwitchLink(*it->second, other.port, sw, sw_port, l.bandwidth,
                     l.delay);
  }

  for (PcieRoute const& r : ir.pcie_routes) {
    PcieSwitch& sw = sys.SwitchById(r.sw_id);
    sw.AddRoute(r.dst, sw.PortOf(r.out_port));
  }
  for (PcieInRoute const& r : ir.pcie_in_routes) {
    PcieSwitch& sw = sys.SwitchById(r.sw_id);
    sw.AddInRoute(sw.PortOf(r.in_port), r.dst, sw.PortOf(r.out_port));
  }

  // 交换节点能送到的地方，对与它相连的那个核来说就该走这个口。按入口分的那张表只算
  // 从本条线进去的那些，别的入口的路由与这个核无关。
  for (CoreLink const& l : core_links) {
    PcieSwitch& sw = sys.SwitchById(l.sw_id);
    Router& r = sys.CoreById(l.core_id).Rt();
    for (Coord dst : sw.Destinations()) r.AddSwitchRoute(dst, l.port);
    for (Coord dst : sw.DestinationsFrom(sw.PortOf(l.sw_port))) {
      r.AddSwitchRoute(dst, l.port);
    }
  }
}

// 中间文件里的端口名换成交换节点上那五个口之一。名字在读入时就查过白名单，这里
// 只做换算。
inline EthPort EthPortOf(std::string const& id) {
  if (id == "res_ingress") return EthPort::kResIngress;
  if (id == "phase1_moe_ingress") return EthPort::kPhase1MoeIngress;
  if (id == "phase2_moe_result_ingress") {
    return EthPort::kPhase2MoeResultIngress;
  }
  if (id == "phase2_moe_request_egress") {
    return EthPort::kPhase2MoeRequestEgress;
  }
  LOGCHECK(id == "phase3_res_join_egress",
           "BuildSystem: unknown eth switch port name.");
  return EthPort::kPhase3ResJoinEgress;
}

inline SinkRole SinkRoleOf(std::string const& role) {
  if (role == "MOE") return SinkRole::kMoe;
  if (role == "BYPASS") return SinkRole::kBypass;
  LOGCHECK(role == "GENERIC", "BuildSystem: unknown landing point role.");
  return SinkRole::kGeneric;
}

inline EthSwitchConfig EthConfigOf(EthConfig const& src) {
  EthSwitchConfig cfg;
  cfg.processing_delay = src.processing_delay;
  cfg.header_bytes = src.header_bytes;
  cfg.payload_numel = src.payload_numel;
  cfg.payload_bytes_per_elem = src.payload_bytes_per_elem;
  cfg.expert_group_size = src.expert_group_size;
  cfg.expert_count = src.expert_count;
  for (EthPortInfo const& p : src.ports) {
    EthPortConfig& out = cfg.ports[static_cast<uint32_t>(EthPortOf(p.port_id))];
    out.bandwidth = p.bandwidth;
    out.propagation = p.propagation;
    if (p.queue_capacity > 0) out.queue_capacity = p.queue_capacity;
    if (p.pending_capacity > 0) out.pending_capacity = p.pending_capacity;
  }
  for (EthRouteInfo const& r : src.routes) {
    cfg.SetRoute(EthPortOf(r.in_port), EthPortOf(r.out_port));
  }
  for (ExpertGroupEntry const& e : src.expert_groups) {
    cfg.expert_group_map.emplace_back(e.expert_id, e.groups);
  }
  return cfg;
}

// Phase 那一段的装配。四步，前后有依赖：交换节点要先在，落点才接得上；落点要先在，
// 通道才知道往哪发；注入那道口子要先绑好 Host，投递来了才有地方去。
inline void WirePhase(System& sys, BachIr const& ir,
                      std::unordered_map<std::string, Phase1Sink*> const& sinks,
                      std::unordered_map<std::string, Host*> const& hosts,
                      std::unordered_map<std::string, Out*> const& outs,
                      uint64_t* next_node) {
  if (!ir.eth.present) return;
  EthSwitch& eth = sys.AddEthSwitch((*next_node)++, EthConfigOf(ir.eth), "eth");
  eth.SetPhase1BoundaryCompletion(ir.eth.complete_phase1_boundary);

  // 落点按角色接到它该去的入口上。GENERIC 的不接：它收下就完了，没有下一跳。
  for (Phase1SinkInfo const& s : ir.phase1_sinks) {
    const SinkRole role = SinkRoleOf(s.role);
    if (role == SinkRole::kGeneric) continue;
    auto it = sinks.find(s.name);
    LOGCHECK(it != sinks.end(), "BuildSystem: no such landing point.");
    const EthPort port = role == SinkRole::kMoe ? EthPort::kPhase1MoeIngress
                                                : EthPort::kResIngress;
    it->second->ConnectEth(&eth.OpenIngress(port));
  }

  if (ir.eth.phase2_ingress) {
    Phase2EthIngress& adapter = sys.AddPhase2Ingress();
    // 一个 Host 只有一个注入目标，所以它名下那几个 group 必须指向同一个核。
    std::unordered_map<std::string, uint64_t> target_of;
    for (HostGroupTarget const& g : ir.host_group_targets) {
      auto it = target_of.find(g.host_name);
      LOGCHECK(it == target_of.end() || it->second == g.target_core,
               "BuildSystem: one host is asked to inject into two different "
               "cores.");
      target_of[g.host_name] = g.target_core;
    }
    for (auto const& kv : target_of) {
      auto it = hosts.find(kv.first);
      LOGCHECK(it != hosts.end(), "BuildSystem: no such host.");
      adapter.BindHost(kv.first, ir.dim.CoordOf(kv.second),
                       &it->second->OfferPort());
    }
    for (HostBind const& b : ir.host_binds) {
      adapter.BindGroup(b.group_id, b.host_name);
    }
    adapter.Validate();
  }

  if (ir.eth.result_bridge) {
    LOGCHECK(outs.size() == 1,
             "BuildSystem: the result bridge takes exactly one output node.");
    outs.begin()->second->ConnectResultBridge(
        &eth.OpenIngress(EthPort::kPhase2MoeResultIngress));
    sys.AddPhase3Join();
  }
}

}  // namespace detail

// 一次 run 里各汇聚点一共会收到几包。时钟等这些都到齐才停。
//
// 它不等于各 user 的期望分片数之和：一个 user 的结果可以落在几个汇聚点上，收到第一
// 个就算它完成了，余下那些仍然会来，只是不再改结论。
inline uint64_t ExpectedPackets(BachIr const& ir) { return ir.total_packets; }

// 完成判据的分母。有 Phase 时判完成的是交换节点，数的是一次次工作，也就是各条通道
// 产出的 user 数之和；没有 Phase 时判完成的是汇聚点，数的是包。
inline uint64_t ExpectedCompletions(BachIr const& ir) {
  if (!ir.eth.present) return ExpectedPackets(ir);
  uint64_t total = 0;
  for (Phase1LaneInfo const& l : ir.phase1_lanes) total += l.uids.size();
  return total;
}

inline std::unique_ptr<System> BuildSystem(ClockPtr clk, BachIr const& ir) {
  LOGCHECK(ir.dim.CoreNum() > 0, "BuildSystem: the core array is empty.");
  auto sys = std::make_unique<System>(clk, ir.params);
  RouteConfig& route = sys->Route();
  route.dim = ir.dim;

  // 1. 路由表
  for (ExtNode const& n : ir.ext_nodes) {
    LOGCHECK(n.port != Port::kUnknown,
             "BuildSystem: an external node has no eject port.");
    route.AddExtNode(n.coord, ir.dim.CoordOf(n.target_core), n.port);
  }
  for (Gateway const& g : ir.gateways) {
    if (g.local_cores.empty()) continue;
    // 一个方向登记了几个出口时只用第一个，路由每次只挑一个出口，与 Bach 一致。
    route.AddChipGateway(g.chip_id, g.dir,
                         ir.dim.GlobalOf(g.chip_id, g.local_cores.front()));
  }

  // 2. 核。没有任务表的核给一张空表，它仍然要在阵列里占位并转发别人的包。
  static const CoreTaskTable kEmptyTasks;
  for (uint64_t id = 0; id < ir.dim.CoreNum(); ++id) {
    CoreContext ctx;
    ctx.node_id = id;
    ctx.params = &ir.params;
    auto info = ir.cores.find(id);
    ctx.group_id = info == ir.cores.end() ? -1 : info->second.group_id;
    ctx.rec = nullptr;  // Core 会换成它自己的
    CoreTaskTable const* tasks = ir.Tasks(id);
    ctx.tasks = tasks != nullptr ? tasks : &kEmptyTasks;
    ctx.metas = &ir.metas;
    ctx.skips = &ir.skip_sources;
    ctx.credits = &ir.credits;
    sys->AddCore(id, ir.dim.CoordOf(id), ctx, "core" + std::to_string(id));
  }

  // 3. 连线：片内 mesh
  const uint32_t chip_num = ir.dim.chip_rows * ir.dim.chip_cols;
  const uint32_t per_chip = ir.dim.core_rows_per_chip * ir.dim.core_cols_per_chip;
  for (uint32_t chip_id = 0; chip_id < chip_num; ++chip_id) {
    Chip& chip = sys->AddChip(chip_id);
    for (uint32_t local = 0; local < per_chip; ++local) {
      const Coord g = ir.dim.GlobalOf(chip_id, local);
      const int64_t core_id = ir.dim.CoreIdAt(g);
      LOGCHECK(core_id >= 0, "BuildSystem: a chip position falls outside.");
      chip.Place(local / ir.dim.core_cols_per_chip,
                 local % ir.dim.core_cols_per_chip,
                 &sys->CoreById(static_cast<uint64_t>(core_id)));
    }
  }
  sys->WireChipMeshes();

  // 片间 PCIe
  for (ChipLink const& l : ir.chip_links) {
    sys->WireChipLink(l.src_core, l.src_port, l.dst_core, l.dst_port,
                      l.bandwidth, l.delay);
  }

  // 阵列外的节点。挂在交换节点上的那些不在这里接线，它们那条线由 PCIELINK 给出。
  std::unordered_set<std::string> on_switch;
  for (PcieLink const& l : ir.pcie_links) {
    for (PcieEnd const* e : {&l.a, &l.b}) {
      if (e->kind == PcieEndKind::kHost || e->kind == PcieEndKind::kOut) {
        on_switch.insert(e->ref);
      }
    }
  }

  std::unordered_map<uint32_t, Host*> host_by_ext;
  std::unordered_map<std::string, Router*> ext_router;
  std::unordered_map<std::string, Host*> host_by_name;
  std::unordered_map<std::string, Out*> out_by_name;
  std::unordered_map<std::string, Phase1Sink*> sink_by_name;
  std::unordered_map<std::string, Phase1Lane*> lane_by_name;
  uint64_t next_node = ir.dim.CoreNum();
  for (uint32_t i = 0; i < ir.ext_nodes.size(); ++i) {
    ExtNode const& n = ir.ext_nodes[i];
    const uint64_t bw =
        n.pcie_bandwidth > 0 ? n.pcie_bandwidth : ir.params.pcie_bandwidth;
    const bool attached = on_switch.count(n.name) != 0;
    if (n.kind == ExtKind::kHost) {
      const uint64_t credit = detail::CreditCapacityFor(
          ir.params, detail::TypeOfCore(ir, n.target_core));
      Host& h = sys->AddHost(next_node++, n.coord, n.volume, bw, credit,
                             detail::EntryOpcode(ir, n.target_core), n.name);
      if (!attached) {
        sys->AttachExternal(h.Rt(), n.target_core, n.port, n.pcie_bandwidth,
                            n.pcie_delay);
      }
      host_by_ext[i] = &h;
      host_by_name[n.name] = &h;
      ext_router[n.name] = &h.Rt();
    } else if (n.kind == ExtKind::kOut) {
      Out& o = sys->AddOut(next_node++, n.coord, n.volume, bw, n.name);
      if (!attached) {
        sys->AttachExternal(o.Rt(), n.target_core, n.port, n.pcie_bandwidth,
                            n.pcie_delay);
      }
      out_by_name[n.name] = &o;
      ext_router[n.name] = &o.Rt();
    } else if (n.kind == ExtKind::kPhase1Sink) {
      SinkRole role = SinkRole::kGeneric;
      for (Phase1SinkInfo const& s : ir.phase1_sinks) {
        if (s.name == n.name) role = detail::SinkRoleOf(s.role);
      }
      Phase1Sink& s =
          sys->AddPhase1Sink(next_node++, n.coord, role, n.volume, bw, n.name);
      if (!attached) {
        sys->AttachExternal(s.Rt(), n.target_core, n.port, n.pcie_bandwidth,
                            n.pcie_delay);
      }
      sink_by_name[n.name] = &s;
      ext_router[n.name] = &s.Rt();
    } else {
      // 通道要知道两个落点的坐标，所以它排在落点之后建，这里先占位。
      LOGCHECK(n.kind == ExtKind::kPhase1Lane,
               "BuildSystem: this kind of external node is not modelled yet.");
    }
  }

  // Phase1 的通道。它要用落点的坐标，所以落点全建完了才轮到它。
  for (uint32_t i = 0; i < ir.ext_nodes.size(); ++i) {
    ExtNode const& n = ir.ext_nodes[i];
    if (n.kind != ExtKind::kPhase1Lane) continue;
    Phase1LaneInfo const* info = nullptr;
    for (Phase1LaneInfo const& l : ir.phase1_lanes) {
      if (l.name == n.name) info = &l;
    }
    LOGCHECK(info != nullptr, "BuildSystem: a phase1 lane has no PHASE1LANE row.");

    Phase1LaneSpec spec;
    spec.layer_id = info->layer_id;
    spec.group_id = info->group_id;
    spec.bypass_volume = info->bypass_volume;
    spec.moe_volume = info->moe_volume;
    spec.fc0_delay = info->fc0_delay;
    spec.res_delay = info->res_delay;
    spec.norm_delay = info->norm_delay;
    spec.router_delay = info->router_delay;
    spec.push_delay = info->push_delay;
    spec.hit_map = info->hit_map;
    if (!info->bypass_sink.empty()) {
      auto it = sink_by_name.find(info->bypass_sink);
      LOGCHECK(it != sink_by_name.end(), "BuildSystem: no such landing point.");
      spec.bypass_target = it->second->Position();
      spec.has_bypass = true;
    }
    if (!info->moe_sink.empty()) {
      auto it = sink_by_name.find(info->moe_sink);
      LOGCHECK(it != sink_by_name.end(), "BuildSystem: no such landing point.");
      spec.moe_target = it->second->Position();
      spec.has_moe = true;
    }

    const uint64_t bw =
        n.pcie_bandwidth > 0 ? n.pcie_bandwidth : ir.params.pcie_bandwidth;
    Phase1Lane& l =
        sys->AddPhase1Lane(next_node++, n.coord, spec, bw, n.name);
    // 通道的编号取它在中间文件里的次序，包上带的就是它，汇合点按它分辨来路。
    l.SetLaneId(static_cast<int64_t>(lane_by_name.size()));
    l.SetUsers(info->uids);
    if (on_switch.count(n.name) == 0) {
      sys->AttachExternal(l.Rt(), n.target_core, n.port, n.pcie_bandwidth,
                          n.pcie_delay);
    }
    lane_by_name[n.name] = &l;
    ext_router[n.name] = &l.Rt();
  }

  // 4. PCIe 交换拓扑
  detail::WirePcieFabric(*sys, ir, ext_router, &next_node);

  // 5. 额度开户。开户按软件 credit 图来，不从物理连线推，也不从验资任务推：一条边
  // 有没有额度、额度多大，只有那张图说了算。退休的额度要还回来，收边的核必须先开过户。
  for (CreditEdge const& e : ir.credit_edges) {
    sys->CoreById(e.src_core)
        .AddDownstream(static_cast<int64_t>(e.dst_core),
                       detail::CreditCapacityFor(ir.params, e.dst_type),
                       e.dst_type);
  }

  // 6. 注入名单与期望分片数。三种注入源分三条路：
  //
  //   注入源开跑    名单直接派给它，它自己等额度、扣间隔、记起跑
  //   分发器开跑    名单派给分发器，一个 user 可能同时进好几个注入源，那件事只能有
  //                 一处统筹，各注入源各推各的会把起跑记成几次
  //   Phase1 开跑   这里不派：它进哪个核要等交换节点把专家换算成 EPGroup 之后才知道
  std::unordered_map<uint32_t, std::vector<HostUser>> plan;
  std::vector<DispatchItem> dispatch;
  for (UserInfo const& u : ir.users) {
    if (u.src_kind == SrcKind::kPhase1Lane) continue;

    if (u.src_kind == SrcKind::kDispatcher) {
      DispatchItem item;
      item.uid = u.uid;
      item.expected_fragments = u.expected_fragments;
      item.hit_map = u.hit_map;
      item.moe_bitmap = u.moe_bitmap;
      for (UserTarget const& t : u.targets) {
        LOGCHECK(host_by_ext.count(t.host_ext) != 0,
                 "BuildSystem: a user targets a node that is not a host.");
        DispatchTarget d;
        d.host = host_by_ext[t.host_ext];
        d.target = ir.dim.CoordOf(t.target_core);
        d.tag = t.tag;
        item.targets.push_back(d);
      }
      dispatch.push_back(std::move(item));
      continue;
    }

    for (UserTarget const& t : u.targets) {
      LOGCHECK(host_by_ext.count(t.host_ext) != 0,
               "BuildSystem: a user targets a node that is not a host.");
      HostUser item;
      item.uid = u.uid;
      item.target = ir.dim.CoordOf(t.target_core);
      item.tag = t.tag;
      item.expected_fragments = u.expected_fragments;
      item.hit_map = u.hit_map;
      item.moe_bitmap = u.moe_bitmap;
      plan[t.host_ext].push_back(item);
    }
  }
  for (auto& kv : plan) host_by_ext[kv.first]->SetUsers(std::move(kv.second));
  if (!dispatch.empty()) {
    sys->AddDispatcher(next_node++, "dispatcher").SetPlan(std::move(dispatch));
  }

  // 7. Phase 那一段
  detail::WirePhase(*sys, ir, sink_by_name, host_by_name, out_by_name,
                    &next_node);

  return sys;
}

}
}

#endif
