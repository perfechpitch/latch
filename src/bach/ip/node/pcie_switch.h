#ifndef _LATCH_BACH_IP_NODE_PCIE_SWITCH_
#define _LATCH_BACH_IP_NODE_PCIE_SWITCH_

// PCIe 交换节点。它是片外的一跳，坐标是不与核阵列重叠的锚点。
//
// 它与 Core 里那个路由器最大的不同是：**没有默认路由**。核阵列上的路由是先列后行算
// 出来的，交换网上的每一条都由 Map 显式给出，查不到就停机。交换网的拓扑没有几何可
// 依循，猜一条出去只会把包送到别处。
//
// 端口是 Map 起的名字，不是十二个方向之一：一个交换节点有几个口、各叫什么，由拓扑
// 决定。这里按名字开口，内部用下标。
//
// 两张路由表，按入口分的那张优先：
//
//   按目的地      目的坐标决定从哪个口出去
//   按入口加目的  同一个目的地，从不同的口进来可以从不同的口出去
//
// 一拍里的动作与核里那个路由器一样：各入端口到点的包进队列、轮流服务活跃端口。一次
// 服务的时间只有三段，比核里那个少一段：
//
//   排队    出口上一次占用还没结束
//   服务    按下一跳的带宽切片，每片各占 max(1, 片长除以带宽) 拍，累加
//   端口    出口那条线上登记的延迟
//
// 出口仍然是整个交换节点共用一个。转发之前先把上一跳切碎的片攒回整拍，再按这一跳的
// 带宽重切，与核里那个路由器同一套做法。
//
// 两条禁止：出口与入口是同一个口（包会在这里打转），以及路由指向的那个端口后面接的
// 不是目的地本人（出了 PCIe 又回到 NoC，那条路本该由核阵列自己走）。两条都在装配期
// 就查得出来，所以在装配期查。

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/clock.h"
#include "base/fifo.h"
#include "base/log.h"
#include "base/module.h"
#include "base/runtime.h"
#include "base/time_stamp.h"
#include "bach/common/packet.h"
#include "bach/common/params.h"
#include "bach/ip/node_context.h"
#include "bach/observer/span_recorder.h"

namespace latch {
namespace bach {

class PcieSwitch : public ClkModule {
 public:
  PcieSwitch(ClockPtr clock, Params const& params, uint64_t node,
             std::string const& switch_id, Coord anchor,
             std::string const& name, uint64_t parent)
      : ClkModule(clock),
        p(params),
        ctx(MakeContext(node, &p, &recorder)),
        sw_id(switch_id),
        coord(anchor) {
    LOGCHECK(!sw_id.empty(), "PcieSwitch: the switch id is empty.");
    RegisterId(name, parent);
  }

  std::string const& Id() const { return sw_id; }
  Coord Position() const { return coord; }
  SpanRecorder const& Recorder() const { return recorder; }
  SpanRecorder& MutableRecorder() { return recorder; }

  // ------------------------------------------------------------ 端口

  // 按名字开一个口，已经开过就返回原来那个。
  uint32_t AddPort(std::string const& port_name) {
    LOGCHECK(!port_name.empty(), "PcieSwitch: a port name must not be empty.");
    auto it = port_by_name.find(port_name);
    if (it != port_by_name.end()) return it->second;
    const uint32_t idx = static_cast<uint32_t>(ports.size());
    ports.emplace_back();
    ports.back().name = port_name;
    port_by_name[port_name] = idx;
    return idx;
  }

  bool HasPort(std::string const& port_name) const {
    return port_by_name.count(port_name) != 0;
  }

  uint32_t PortOf(std::string const& port_name) const {
    auto it = port_by_name.find(port_name);
    LOGCHECK(it != port_by_name.end(), "PcieSwitch: no such port name.");
    return it->second;
  }

  std::string const& PortName(uint32_t idx) const {
    LOGCHECK(idx < ports.size(), "PcieSwitch: port index out of range.");
    return ports[idx].name;
  }

  uint32_t PortNum() const { return static_cast<uint32_t>(ports.size()); }

  // 邻居往这里推包。一个口一个入口，满足单 producer 单 consumer。
  Fifo<RoutedPkt>& OpenPort(uint32_t idx) {
    LOGCHECK(idx < ports.size(), "PcieSwitch: port index out of range.");
    Port2& port = ports[idx];
    if (port.inbox == nullptr) {
      port.inbox = std::make_unique<Fifo<RoutedPkt>>(p.link_fifo_depth, clk);
    }
    return *port.inbox;
  }

  void ConnectPort(uint32_t idx, Coord peer, bool peer_is_switch,
                   Fifo<RoutedPkt>* peer_in, uint64_t bandwidth,
                   uint64_t delay) {
    LOGCHECK(idx < ports.size(), "PcieSwitch: port index out of range.");
    Port2& port = ports[idx];
    LOGCHECK(port.peer == nullptr, "PcieSwitch: that port is already connected.");
    LOGCHECK(peer_in != nullptr, "PcieSwitch: the peer inbox is null.");
    port.peer = peer_in;
    port.peer_coord = peer;
    port.peer_is_switch = peer_is_switch;
    port.bandwidth = bandwidth > 0 ? bandwidth : p.pcie_bandwidth;
    port.delay = delay;
    port.connected = true;
  }

  // ------------------------------------------------------------ 路由表

  void AddRoute(Coord dst, uint32_t out_port) {
    CheckEgress(dst, out_port);
    const uint64_t key = EncodeCoord(dst);
    auto it = routes.find(key);
    if (it != routes.end()) {
      LOGCHECK(it->second == out_port,
               "PcieSwitch: two ports claim the same destination.");
      return;
    }
    routes[key] = out_port;
  }

  void AddInRoute(uint32_t in_port, Coord dst, uint32_t out_port) {
    LOGCHECK(in_port < ports.size() && ports[in_port].connected,
             "PcieSwitch: an input route comes from an unconnected port.");
    CheckEgress(dst, out_port);
    std::unordered_map<uint64_t, uint32_t>& table = in_routes[in_port];
    const uint64_t key = EncodeCoord(dst);
    auto it = table.find(key);
    if (it != table.end()) {
      LOGCHECK(it->second == out_port,
               "PcieSwitch: two ports claim the same input route.");
      return;
    }
    table[key] = out_port;
  }

  bool HasRouteTo(Coord dst) const {
    return routes.count(EncodeCoord(dst)) != 0;
  }

  // 按目的地那张表能送到的地方。核那一侧要按它登记自己的出口。
  std::vector<Coord> Destinations() const {
    std::vector<Coord> out;
    for (auto const& kv : routes) out.push_back(DecodeCoord(kv.first));
    std::sort(out.begin(), out.end());
    return out;
  }

  // 从这个口进来的包能送到哪些目的地。按入口分的那张表要按它登记。
  std::vector<Coord> DestinationsFrom(uint32_t in_port) const {
    std::vector<Coord> out;
    auto it = in_routes.find(in_port);
    if (it == in_routes.end()) return out;
    for (auto const& kv : it->second) out.push_back(DecodeCoord(kv.first));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  // ------------------------------------------------------------ 每拍

  void Cycle() override {
    DelayCycle(1);
    const Time now = RT::Now();
    DrainPorts(now);
    Arbitrate(now);

    TracePerCycle("queued", QueuedBeats());
    TracePerCycle("served", served);
  }

  Time BusyUntil() const { return last_busy_until; }
  uint64_t ServedBeats() const { return served; }
  uint64_t ForwardedFragments() const { return forwarded; }
  uint64_t InTransitFragments() const { return transit.size(); }
  uint64_t QueuedBeats() const {
    uint64_t total = 0;
    for (auto const& port : ports) total += port.queue.size();
    return total;
  }

 private:
  struct Req {
    CommInstPtr payload;
    Coord dst;
    uint64_t size = 0;
    uint64_t payload_size = 0;
    uint64_t byte_offset = 0;
    uint64_t frag_id = 0;
    uint64_t total_frag = 1;
    uint64_t is_tail = 1;
    uint64_t hop_count = 0;
    uint32_t src_port = 0;
  };

  // 端口名 Port2 里的 2 是为了不与十二方向那个 Port 枚举撞名。
  struct Port2 {
    std::string name;
    std::unique_ptr<Fifo<RoutedPkt>> inbox;
    Fifo<RoutedPkt>* peer = nullptr;
    Coord peer_coord;
    bool peer_is_switch = false;
    bool connected = false;
    bool active = false;
    uint64_t bandwidth = 0;
    uint64_t delay = 0;
    std::deque<Req> queue;
  };

  static NodeContext MakeContext(uint64_t node, Params const* params,
                                 SpanRecorder* rec) {
    NodeContext c;
    c.node_id = node;
    c.params = params;
    c.rec = rec;
    return c;
  }

  // 一条路由的出口后面接的必须是目的地本人，或者另一个交换节点。接的是别的核，说明
  // 这条路出了 PCIe 又要回到 NoC，那一段本该由核阵列自己走。
  void CheckEgress(Coord dst, uint32_t out_port) const {
    LOGCHECK(out_port < ports.size() && ports[out_port].connected,
             "PcieSwitch: a route uses an unconnected port.");
    Port2 const& port = ports[out_port];
    if (port.peer_is_switch) return;
    LOGCHECK(port.peer_coord == dst,
             "PcieSwitch: this route leaves at a port that lands somewhere "
             "else, it would re-enter the NoC after PCIe.");
  }

  // ------------------------------------------------------------ 收包

  void DrainPorts(Time now) {
    for (uint32_t i = 0; i < ports.size(); ++i) {
      Port2& port = ports[i];
      if (port.inbox == nullptr) continue;
      while (!port.inbox->IsEmpty()) {
        RoutedPkt& pkt = port.inbox->Front();
        if (now < static_cast<Time>(uint64_t(pkt.arrive_cycle))) break;
        Req r;
        r.payload = pkt.payload.Get();
        r.dst = DecodeCoord(uint64_t(pkt.dst));
        r.size = uint64_t(pkt.size);
        r.payload_size = uint64_t(pkt.payload_size);
        r.byte_offset = uint64_t(pkt.byte_offset);
        r.frag_id = uint64_t(pkt.fragment_id);
        r.total_frag = uint64_t(pkt.total_frag);
        r.is_tail = uint64_t(pkt.is_tail);
        r.hop_count = uint64_t(pkt.hop_count);
        r.src_port = i;
        port.inbox->Pop();
        Enqueue(std::move(r));
      }
    }
  }

  void Enqueue(Req r) {
    const uint32_t idx = r.src_port;
    Port2& port = ports[idx];
    const bool was_empty = port.queue.empty();
    port.queue.push_back(std::move(r));
    const uint64_t warn = p.router_queue_warn;
    if (warn != 0 && port.queue.size() >= warn) {
      spdlog::warn("PcieSwitch {} port {} queue depth {}", sw_id, port.name,
                   port.queue.size());
    }
    if (was_empty && !port.active) {
      port.active = true;
      active.push_back(idx);
    }
  }

  // ------------------------------------------------------------ 仲裁

  void Arbitrate(Time now) {
    while (!active.empty()) {
      const uint32_t idx = active.front();
      active.pop_front();
      Port2& port = ports[idx];
      port.active = false;
      if (port.queue.empty()) continue;
      Req r = std::move(port.queue.front());
      port.queue.pop_front();
      if (!port.queue.empty()) {
        port.active = true;
        active.push_back(idx);
      }
      Serve(std::move(r), now);
    }
  }

  void Serve(Req r, Time now) {
    ++served;
    const Time start = std::max(now, last_busy_until);
    const uint64_t queueing = static_cast<uint64_t>(start - now);

    LOGCHECK(r.hop_count <= p.hop_count_err,
             "PcieSwitch: a packet exceeded the hop limit, the fabric loops.");

    // 上一跳把它切碎了，先攒回整拍再看往哪走
    if (!Reassemble(&r)) {
      last_busy_until = start + 1;
      return;
    }

    const uint32_t next = Route(r.dst, r.src_port);
    LOGCHECK(next != r.src_port,
             "PcieSwitch: a route hairpins back to its ingress port.");
    Port2 const& out = ports[next];

    std::vector<uint64_t> sizes = Split(r, out.bandwidth);
    std::vector<uint64_t> cycles;
    cycles.reserve(sizes.size());
    uint64_t service = 0;
    for (uint64_t s : sizes) {
      const uint64_t c = std::max<uint64_t>(1, (s + out.bandwidth - 1) / out.bandwidth);
      cycles.push_back(c);
      service += c;
    }
    last_busy_until = start + service;

    ctx.Span(Unit::kSwitch, r.payload->uid, r.payload->tid, start,
             start + service, SpanState::kTransfer, r.size);

    uint64_t cumulative = 0;
    uint64_t offset = r.byte_offset;
    for (size_t i = 0; i < sizes.size(); ++i) {
      cumulative += cycles[i];
      const Time arrive = now + queueing + cumulative + out.delay;

      RoutedPkt pkt(clk);
      pkt.dst = EncodeCoord(r.dst);
      pkt.src = EncodeCoord(coord);
      pkt.size = sizes[i];
      pkt.payload_size = r.payload_size;
      pkt.byte_offset = offset;
      pkt.fragment_id = i;
      pkt.total_frag = sizes.size();
      pkt.is_tail = i + 1 == sizes.size() ? 1 : 0;
      pkt.arrive_cycle = static_cast<uint64_t>(arrive);
      pkt.hop_count = r.hop_count + 1;
      pkt.in_port = static_cast<uint64_t>(Port::kUnknown);
      pkt.payload = r.payload;
      offset += sizes[i];

      LOGCHECK(!out.peer->IsFull(),
               "PcieSwitch: the peer inbox overflowed, give the link more "
               "depth.");
      out.peer->Push(pkt);
      ++forwarded;
    }
  }

  std::vector<uint64_t> Split(Req const& r, uint64_t bw) const {
    LOGCHECK(bw > 0, "PcieSwitch: a port has no bandwidth.");
    if (r.size <= bw) return {r.size};
    std::vector<uint64_t> out;
    const uint64_t count = (r.size + bw - 1) / bw;
    out.reserve(count);
    for (uint64_t i = 0; i < count; ++i) {
      out.push_back(std::min(bw, r.size - i * bw));
    }
    return out;
  }

  bool Reassemble(Req* r) {
    if (r->payload_size == 0 || r->size >= r->payload_size) return true;
    FragKey key;
    key.beat = MakeBeatKey(*r->payload);
    key.beat_id = r->payload->beat_id;
    key.xfer_id = r->payload->xfer_id;

    uint64_t& got = transit[key];
    got += r->size;
    if (got < r->payload_size) return false;
    LOGCHECK(got == r->payload_size,
             "PcieSwitch: transit reassembly overshot the beat size.");
    transit.erase(key);
    r->size = r->payload_size;
    r->byte_offset = 0;
    r->frag_id = 0;
    r->total_frag = 1;
    r->is_tail = 1;
    return true;
  }

  uint32_t Route(Coord dst, uint32_t in_port) const {
    auto flow = in_routes.find(in_port);
    if (flow != in_routes.end()) {
      auto hit = flow->second.find(EncodeCoord(dst));
      if (hit != flow->second.end()) return hit->second;
    }
    auto it = routes.find(EncodeCoord(dst));
    LOGCHECK(it != routes.end(),
             "PcieSwitch: no explicit route for that destination, the switch "
             "fabric has no default route.");
    return it->second;
  }

  Params p;
  SpanRecorder recorder;
  NodeContext ctx;
  std::string sw_id;
  Coord coord;

  std::vector<Port2> ports;
  std::unordered_map<std::string, uint32_t> port_by_name;
  std::deque<uint32_t> active;

  std::unordered_map<uint64_t, uint32_t> routes;
  std::unordered_map<uint32_t, std::unordered_map<uint64_t, uint32_t>> in_routes;
  std::unordered_map<FragKey, uint64_t, FragKeyHash> transit;

  Time last_busy_until = 0;
  uint64_t served = 0;
  uint64_t forwarded = 0;
};

}
}

#endif
