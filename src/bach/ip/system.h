#ifndef _LATCH_BACH_IP_SYSTEM_
#define _LATCH_BACH_IP_SYSTEM_

// 一次 run 的全部硬件：所有核、所有 chip、阵列外的注入源与汇聚点，以及它们之间的线。
//
// 它是唯一持有这些对象的地方，也是唯一知道全局接线的地方。核只知道自己的十二个端口接
// 了谁，chip 只知道片内怎么连，跨 chip 与阵列外那几条线在这一层接。
//
// 路由表也放在这里，因为它必须活得比所有核都久：每个核构造时拿到它的指针，运行期每
// 一跳都要查它。
//
// 观测记录器归各节点自己，这一层只在跑完之后把它们收上来。仿真期不碰。

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/clock.h"
#include "base/log.h"
#include "bach/common/packet.h"
#include "bach/common/params.h"
#include "bach/ip/chip/chip.h"
#include "bach/ip/chip/core/core.h"
#include "bach/ip/eth_switch/eth_switch.h"
#include "bach/ip/eth_switch/phase2_ingress.h"
#include "bach/ip/eth_switch/phase3_join.h"
#include "bach/ip/external/dispatcher.h"
#include "bach/ip/external/host.h"
#include "bach/ip/external/out.h"
#include "bach/ip/external/phase1_lane.h"
#include "bach/ip/external/phase1_sink.h"
#include "bach/ip/node/pcie_switch.h"
#include "bach/ip/route_config.h"
#include "bach/ip/wiring.h"
#include "bach/observer/span_recorder.h"

namespace latch {
namespace bach {

class System {
 public:
  System(ClockPtr clock, Params const& params)
      : clk(clock), p(params) {
    LOGCHECK(clk != nullptr, "System: clock is null.");
  }

  RouteConfig& Route() { return route; }
  RouteConfig const& Route() const { return route; }
  Params const& P() const { return p; }

  // ------------------------------------------------------------ 装配

  Core& AddCore(uint64_t core_id, Coord coord, CoreContext const& ctx,
                std::string const& name) {
    LOGCHECK(core_by_id.count(core_id) == 0, "System: core id is taken.");
    cores.push_back(std::make_unique<Core>(clk, ctx, coord, &route, name, 0));
    core_by_id[core_id] = cores.back().get();
    return *cores.back();
  }

  Chip& AddChip(uint32_t chip_id) {
    LOGCHECK(chip_by_id.count(chip_id) == 0, "System: chip id is taken.");
    chips.push_back(std::make_unique<Chip>(route.dim, chip_id));
    chip_by_id[chip_id] = chips.back().get();
    return *chips.back();
  }

  Host& AddHost(uint64_t node_id, Coord coord, uint64_t volume,
                uint64_t bandwidth, uint64_t credit_capacity,
                Opcode init_opcode, std::string const& name) {
    hosts.push_back(std::make_unique<Host>(clk, p, node_id, coord, &route,
                                           volume, bandwidth, credit_capacity,
                                           init_opcode, name, 0));
    return *hosts.back();
  }

  Out& AddOut(uint64_t node_id, Coord coord, uint64_t volume,
              uint64_t bandwidth, std::string const& name) {
    outs.push_back(
        std::make_unique<Out>(clk, p, node_id, coord, &route, volume, bandwidth,
                              name, 0));
    return *outs.back();
  }

  // Phase1 那一段的通道与落点。它们的坐标不在核阵列内，与注入源、汇聚点一样各带
  // 一个路由器接进阵列。
  Phase1Lane& AddPhase1Lane(uint64_t node_id, Coord coord,
                            Phase1LaneSpec const& spec, uint64_t bandwidth,
                            std::string const& name) {
    lanes.push_back(std::make_unique<Phase1Lane>(clk, p, node_id, coord, &route,
                                                 spec, bandwidth, name, 0));
    return *lanes.back();
  }

  Phase1Sink& AddPhase1Sink(uint64_t node_id, Coord coord, SinkRole role,
                            uint64_t volume, uint64_t bandwidth,
                            std::string const& name) {
    sinks.push_back(std::make_unique<Phase1Sink>(clk, p, node_id, coord, &route,
                                                 role, volume, bandwidth, name,
                                                 0));
    return *sinks.back();
  }

  // MoE 的分发器至多一个。一个 user 可能同时进好几个注入源，那件事只能有一处统筹。
  MoeDispatcher& AddDispatcher(uint64_t node_id, std::string const& name) {
    LOGCHECK(dispatcher == nullptr, "System: there is already a dispatcher.");
    dispatcher = std::make_unique<MoeDispatcher>(clk, p, node_id, name, 0);
    return *dispatcher;
  }

  bool HasDispatcher() const { return dispatcher != nullptr; }

  MoeDispatcher& Dispatcher() {
    LOGCHECK(dispatcher != nullptr, "System: there is no dispatcher.");
    return *dispatcher;
  }

  // 以太网交换节点至多一个。三段流水之间那几条边都从它上面过，两个就分不清一次
  // 工作的两路各走了哪一个。
  EthSwitch& AddEthSwitch(uint64_t node_id, EthSwitchConfig const& config,
                          std::string const& name) {
    LOGCHECK(eth == nullptr, "System: there is already an eth switch.");
    eth = std::make_unique<EthSwitch>(clk, p, node_id, config, name, 0);
    return *eth;
  }

  // 交换节点上那两个收投递的：一个把 MoE 请求注入 Phase2，一个判两路的汇合。
  Phase2EthIngress& AddPhase2Ingress() {
    LOGCHECK(eth != nullptr, "System: there is no eth switch to attach to.");
    LOGCHECK(phase2 == nullptr, "System: the phase2 ingress is already there.");
    phase2 = std::make_unique<Phase2EthIngress>(clk);
    eth->AttachSink(phase2.get());
    return *phase2;
  }

  Phase3Join& AddPhase3Join() {
    LOGCHECK(eth != nullptr, "System: there is no eth switch to attach to.");
    LOGCHECK(join == nullptr, "System: the join point is already there.");
    // 汇合点拿走全局完成权，所以交换节点自己那个判法必须先关掉。
    eth->SetPhase1BoundaryCompletion(false);
    join = std::make_unique<Phase3Join>(&eth->MutableRecorder());
    eth->AttachSink(join.get());
    return *join;
  }

  bool HasPhase2Ingress() const { return phase2 != nullptr; }
  bool HasPhase3Join() const { return join != nullptr; }

  Phase2EthIngress& Phase2() {
    LOGCHECK(phase2 != nullptr, "System: there is no phase2 ingress.");
    return *phase2;
  }

  Phase3Join& Join() {
    LOGCHECK(join != nullptr, "System: there is no join point.");
    return *join;
  }

  PcieSwitch& AddPcieSwitch(uint64_t node_id, std::string const& sw_id,
                            Coord anchor, std::string const& name) {
    LOGCHECK(switch_by_id.count(sw_id) == 0, "System: switch id is taken.");
    switches.push_back(
        std::make_unique<PcieSwitch>(clk, p, node_id, sw_id, anchor, name, 0));
    switch_by_id[sw_id] = switches.back().get();
    return *switches.back();
  }

  // 片内 mesh。每块 chip 各接各的，片与片之间靠 PCIe 口。
  void WireChipMeshes() {
    for (auto& chip : chips) chip->WireMesh();
  }

  // 一条片间链路。编译产物里一条物理链路写成两条有向记录，接过一次就够，第二条只
  // 核对反向那半边确实也登记过。
  void WireChipLink(uint64_t src_core, Port src_port, uint64_t dst_core,
                    Port dst_port, uint64_t bandwidth, uint64_t delay) {
    LOGCHECK(OppositePort(src_port) == dst_port,
             "System: a chip link must use two opposite ports.");
    const uint64_t here = LinkKey(src_core, src_port);
    const uint64_t back = LinkKey(dst_core, dst_port);
    if (wired.count(here) != 0) {
      LOGCHECK(wired.count(back) != 0,
               "System: only one direction of this chip link was wired.");
      return;
    }
    AttachLink(CoreById(src_core).Rt(), src_port, CoreById(dst_core).Rt(),
               bandwidth, delay);
    wired.insert(here);
    wired.insert(back);
  }

  // 把阵列外的节点接到它的网关核上。路由表里那一条要在建节点之前就登记好，因为节点
  // 构造时就要判断自己在不在阵列内。
  void AttachExternal(Router& ext, uint64_t gateway_core, Port eject,
                      uint64_t bandwidth, uint64_t delay) {
    AttachLink(CoreById(gateway_core).Rt(), eject, ext, bandwidth, delay);
  }

  // ------------------------------------------------------------ 查询

  Core& CoreById(uint64_t core_id) {
    auto it = core_by_id.find(core_id);
    LOGCHECK(it != core_by_id.end(), "System: no such core id.");
    return *it->second;
  }

  Chip& ChipById(uint32_t chip_id) {
    auto it = chip_by_id.find(chip_id);
    LOGCHECK(it != chip_by_id.end(), "System: no such chip id.");
    return *it->second;
  }

  PcieSwitch& SwitchById(std::string const& sw_id) {
    auto it = switch_by_id.find(sw_id);
    LOGCHECK(it != switch_by_id.end(), "System: no such switch id.");
    return *it->second;
  }

  bool HasSwitch(std::string const& sw_id) const {
    return switch_by_id.count(sw_id) != 0;
  }

  uint64_t CoreNum() const { return cores.size(); }
  uint64_t HostNum() const { return hosts.size(); }
  uint64_t OutNum() const { return outs.size(); }
  uint64_t SwitchNum() const { return switches.size(); }
  uint64_t LaneNum() const { return lanes.size(); }
  uint64_t SinkNum() const { return sinks.size(); }
  bool HasEth() const { return eth != nullptr; }
  uint64_t NodeNum() const {
    return cores.size() + hosts.size() + outs.size() + switches.size() +
           lanes.size() + sinks.size() + (eth != nullptr ? 1 : 0) +
           (dispatcher != nullptr ? 1 : 0);
  }

  std::vector<std::unique_ptr<Host>> const& Hosts() const { return hosts; }
  std::vector<std::unique_ptr<Out>> const& Outs() const { return outs; }
  std::vector<std::unique_ptr<Core>> const& Cores() const { return cores; }
  std::vector<std::unique_ptr<PcieSwitch>> const& Switches() const {
    return switches;
  }
  std::vector<std::unique_ptr<Phase1Lane>> const& Lanes() const {
    return lanes;
  }
  std::vector<std::unique_ptr<Phase1Sink>> const& Sinks() const {
    return sinks;
  }

  EthSwitch& Eth() {
    LOGCHECK(eth != nullptr, "System: there is no eth switch.");
    return *eth;
  }

  EthSwitch const& Eth() const {
    LOGCHECK(eth != nullptr, "System: there is no eth switch.");
    return *eth;
  }

  // 跑完之后把各节点自己记的事件收上来。仿真期不能调它。
  void Collect(RunRecorder& run) const {
    for (auto const& c : cores) run.Collect(c->Recorder());
    for (auto const& h : hosts) run.Collect(h->Recorder());
    for (auto const& o : outs) run.Collect(o->Recorder());
    for (auto const& s : switches) run.Collect(s->Recorder());
    for (auto const& l : lanes) run.Collect(l->Recorder());
    for (auto const& s : sinks) run.Collect(s->Recorder());
    if (eth != nullptr) run.Collect(eth->Recorder());
    if (dispatcher != nullptr) run.Collect(dispatcher->Recorder());
  }

 private:
  static uint64_t LinkKey(uint64_t core_id, Port port) {
    return (core_id << 8) | static_cast<uint64_t>(port);
  }

  ClockPtr clk;
  Params p;
  RouteConfig route;

  std::vector<std::unique_ptr<Core>> cores;
  std::vector<std::unique_ptr<Chip>> chips;
  std::vector<std::unique_ptr<Host>> hosts;
  std::vector<std::unique_ptr<Out>> outs;
  std::vector<std::unique_ptr<PcieSwitch>> switches;
  std::vector<std::unique_ptr<Phase1Lane>> lanes;
  std::vector<std::unique_ptr<Phase1Sink>> sinks;
  std::unique_ptr<EthSwitch> eth;
  std::unique_ptr<MoeDispatcher> dispatcher;
  std::unique_ptr<Phase2EthIngress> phase2;
  std::unique_ptr<Phase3Join> join;

  std::unordered_map<uint64_t, Core*> core_by_id;
  std::unordered_map<uint32_t, Chip*> chip_by_id;
  std::unordered_map<std::string, PcieSwitch*> switch_by_id;
  std::unordered_set<uint64_t> wired;
};

}
}

#endif
