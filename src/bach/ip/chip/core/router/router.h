#ifndef _LATCH_BACH_IP_CHIP_CORE_ROUTER_ROUTER_
#define _LATCH_BACH_IP_CHIP_CORE_ROUTER_ROUTER_

// NoC 路由器。每个 Core 一个，十二个端口：本地口、四个方向口、六个 PCIe 口，外加一个
// 不参与路由的未知口。
//
// 一拍里它做三件事，顺序固定：
//
//   投递  本地投递队列里到点的包，重组齐了就交给本核的 DTE
//   收包  各入端口到点的包搬进对应的输入队列，队列由空转非空时该端口变活跃
//   仲裁  按变活跃的先后轮流服务活跃端口，每轮每个端口出一个包，直到没有可服务的
//
// 仲裁整轮在同一拍里跑完，时间不体现在拍上，体现在两处：出口的忙到什么时候，以及每
// 一片算出来的到达拍。这与 Bach 里一次唤醒服务完所有活跃端口是同一回事。
//
// 一次服务的时间由四段相加，它们的物理含义各不相同：
//
//   排队    出口上一次占用还没结束，这一段是等出口
//   服务    切片之后每片各占 max(每跳最小服务时间, 片长除以带宽) 拍，累加
//   线延迟  出口或入口是本地口时取访问延迟，否则取线延迟
//   PCIe    出口是 PCIe 口时另加，端口上登记过就用登记值，否则按跨 chip 还是跨 node 取
//
// 出口是整个 Router 共用一个，不是每个端口一个：一个 Router 每拍只发得出去那么多，
// 十二个端口抢的是同一个出口。这是它成为瓶颈的方式。
//
// 切分与重组各有一次，键不同，不能混：
//
//   出口切分  一拍数据按下一跳的带宽切成若干片，片带自己的偏移与总片数
//   转发重组  下一跳的带宽更宽时，先把上一跳切碎的片攒回整拍再切，键是传输实例号
//   本地重组  终点按字节数攒够整拍才交给 DTE，同一拍的各片天然同一个传输实例号
//
// 队列满不挡上游：达到阈值只告警，不阻塞、不丢包。整个模型唯一的反压来源是 credit
// 与 stream 槽位，这一条继承自 Bach。

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
#include "base/runtime.h"
#include "base/time_stamp.h"
#include "bach/common/packet.h"
#include "bach/common/params.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/node_context.h"
#include "bach/ip/route_config.h"
#include "bach/ip/sub_unit.h"

namespace latch {
namespace bach {

class Router : public SubUnit, public PacketSink {
 public:
  Router(ClockPtr clock, NodeContext const& context, Coord position,
         RouteConfig const* config, std::string const& name, uint64_t parent,
         bool standalone = false)
      : SubUnit(clock, standalone),
        ctx(context),
        coord(position),
        cfg(config) {
    LOGCHECK(cfg != nullptr, "Router: route config is null.");
    RegisterId(name, parent);
    internal = cfg->dim.Inside(coord) && cfg->ExtAt(coord) == nullptr;
  }

  void Connect(PacketTarget* local_sink) {
    LOGCHECK(local_sink != nullptr, "Router: local sink is null.");
    local = local_sink;
  }

  // 接一个邻居。对端的入口由对端自己开，这边只拿指针。
  void ConnectPort(Port p, Coord peer_coord, bool peer_internal,
                   Fifo<RoutedPkt>* peer_in) {
    const uint32_t idx = static_cast<uint32_t>(p);
    LOGCHECK(idx < kPortNum, "Router: port index out of range.");
    LOGCHECK(p != Port::kLocal && p != Port::kUnknown,
             "Router: local and unknown ports take no peer.");
    LOGCHECK(peer_in != nullptr, "Router: peer inbox is null.");
    LOGCHECK(peer[idx].inbox == nullptr, "Router: port is already connected.");
    peer[idx].inbox = peer_in;
    peer[idx].coord = peer_coord;
    peer[idx].internal = peer_internal;
  }

  // 显式 PCIe 交换拓扑给的那条：目的坐标在这张表里，就从登记的端口进交换网，不再
  // 按几何算方向。交换网上没有几何可依循，一条路由只能由 Map 指名。
  void AddSwitchRoute(Coord dst, Port p) {
    LOGCHECK(p != Port::kUnknown && p != Port::kLocal,
             "Router: a switch route needs a real egress port.");
    const uint64_t key = EncodeCoord(dst);
    auto it = switch_routes.find(key);
    if (it != switch_routes.end()) {
      LOGCHECK(it->second == p,
               "Router: two switch ports claim the same destination.");
      return;
    }
    switch_routes[key] = p;
  }

  bool HasSwitchRoute(Coord dst) const {
    return switch_routes.count(EncodeCoord(dst)) != 0;
  }

  // 这个端口上的链路参数。只有 PCIe 口需要，其余口用全局的 NoC 带宽。
  void SetLinkAttr(Port p, uint64_t bandwidth, uint64_t delay) {
    const uint32_t idx = static_cast<uint32_t>(p);
    LOGCHECK(idx < kPortNum, "Router: port index out of range.");
    if (bandwidth > 0) peer[idx].bandwidth = bandwidth;
    if (delay > 0) peer[idx].delay = delay;
  }

  // 邻居往这里推包。一个端口一个入口，满足单 producer 单 consumer。
  Fifo<RoutedPkt>& OpenPort(Port p) {
    const uint32_t idx = static_cast<uint32_t>(p);
    LOGCHECK(idx < kPortNum, "Router: port index out of range.");
    if (in_fifo[idx] == nullptr) {
      in_fifo[idx] = std::make_unique<Fifo<RoutedPkt>>(ctx.P().link_fifo_depth,
                                                       clk);
    }
    return *in_fifo[idx];
  }

  // 外部注入源用的入口。它们的包算作从本地口进来，与本核 DTE 发出去的包同一条通道，
  // 因为外部设备本来就是借这个核接进阵列的。
  Fifo<RoutedPkt>& AddInjectPort() {
    inject.push_back(
        std::make_unique<Fifo<RoutedPkt>>(ctx.P().link_fifo_depth, clk));
    return *inject.back();
  }

  // 本核 DTE 的出口。同刻进本地口队列，下一拍轮到它被仲裁。
  void Inject(CommInstPtr const& payload, Coord dst, uint64_t size) override {
    LOGCHECK(payload != nullptr, "Router: null payload.");
    Req r;
    r.payload = payload;
    r.dst = dst;
    // 零长的包仍要占一个链路单位，否则它在链路上不占任何时间
    r.size = size > 0 ? size : ctx.P().noc_bandwidth;
    r.payload_size = r.size;
    r.src_port = Port::kLocal;
    Enqueue(std::move(r));
  }

  void Step() override {
    const Time now = RT::Now();
    DeliverLocal(now);
    DrainPorts(now);
    Arbitrate(now);
  }

  Coord Position() const { return coord; }
  bool IsInternal() const { return internal; }
  Time BusyUntil() const { return last_busy_until; }
  uint64_t QueuedBeats() const {
    uint64_t total = 0;
    for (auto const& q : port_queue) total += q.size();
    return total;
  }
  uint64_t PendingLocal() const { return local_pending.size(); }
  uint64_t InTransitFragments() const { return transit.size(); }
  // 诊断计数。服务过几次、往外发了几片、往本地交了几拍，用来看一条路径究竟走的哪边。
  uint64_t ServedBeats() const { return served; }
  uint64_t ForwardedFragments() const { return forwarded; }
  uint64_t DeliveredBeats() const { return delivered; }

 private:
  struct Peer {
    Fifo<RoutedPkt>* inbox = nullptr;
    Coord coord;
    bool internal = true;
    uint64_t bandwidth = 0;  // 0 表示用全局带宽
    uint64_t delay = 0;      // 0 表示按跨 chip 还是跨 node 取
  };

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
    Port src_port = Port::kLocal;
  };

  struct LocalItem {
    Req req;
    Time arrive = 0;
    uint64_t seq = 0;
  };

  // ------------------------------------------------------------ 收包

  void Enqueue(Req r) {
    const uint32_t idx = static_cast<uint32_t>(r.src_port);
    LOGCHECK(idx < kPortNum, "Router: source port out of range.");
    std::deque<Req>& q = port_queue[idx];
    const bool was_empty = q.empty();
    q.push_back(std::move(r));
    const uint64_t warn = ctx.P().router_queue_warn;
    if (warn != 0 && q.size() >= warn) {
      spdlog::warn("Router {} port {} queue depth {}", ctx.node_id,
                   PortName(static_cast<Port>(idx)), q.size());
    }
    if (was_empty && !port_active[idx]) {
      port_active[idx] = true;
      active.push_back(static_cast<Port>(idx));
    }
  }

  void DrainPorts(Time now) {
    for (uint32_t i = 0; i < kPortNum; ++i) {
      if (in_fifo[i] == nullptr) continue;
      TakeArrived(*in_fifo[i], now, static_cast<Port>(i));
    }
    for (auto& f : inject) TakeArrived(*f, now, Port::kLocal);
  }

  void TakeArrived(Fifo<RoutedPkt>& f, Time now, Port expect_src) {
    while (!f.IsEmpty()) {
      RoutedPkt& p = f.Front();
      if (now < static_cast<Time>(uint64_t(p.arrive_cycle))) break;
      Req r;
      r.payload = p.payload.Get();
      r.dst = DecodeCoord(uint64_t(p.dst));
      r.size = uint64_t(p.size);
      r.payload_size = uint64_t(p.payload_size);
      r.byte_offset = uint64_t(p.byte_offset);
      r.frag_id = uint64_t(p.fragment_id);
      r.total_frag = uint64_t(p.total_frag);
      r.is_tail = uint64_t(p.is_tail);
      r.hop_count = uint64_t(p.hop_count);
      r.src_port = expect_src;
      f.Pop();
      Enqueue(std::move(r));
    }
  }

  // ------------------------------------------------------------ 仲裁

  void Arbitrate(Time now) {
    while (!active.empty()) {
      const Port p = active.front();
      active.pop_front();
      const uint32_t idx = static_cast<uint32_t>(p);
      port_active[idx] = false;

      std::deque<Req>& q = port_queue[idx];
      if (q.empty()) continue;
      Req r = std::move(q.front());
      q.pop_front();
      if (!q.empty()) {
        port_active[idx] = true;
        active.push_back(p);
      }
      Serve(std::move(r), now);
    }
  }

  void Serve(Req r, Time now) {
    ++served;
    const Time start = std::max(now, last_busy_until);
    const uint64_t queueing = static_cast<uint64_t>(start - now);
    last_busy_until = start + ctx.P().noc_router_delay;

    LOGCHECK(r.hop_count <= ctx.P().hop_count_err,
             "Router: packet exceeded the hop limit, the network has a loop.");

    // 上一跳把它切碎了，先攒回整拍再看往哪走
    if (!Reassemble(&r)) return;

    const Port next = r.dst == coord ? Port::kLocal : ComputeRoute(r.dst);
    LOGCHECK(next != Port::kUnknown,
             "Router: no path to that destination, check the fabric.");

    const uint64_t bw = BandwidthFor(next);
    const uint64_t base_wire =
        (next == Port::kLocal || r.src_port == Port::kLocal)
            ? ctx.P().noc_access_delay
            : ctx.P().noc_wire_delay;

    std::vector<uint64_t> sizes = Split(next, r, bw);
    uint64_t service = 0;
    std::vector<uint64_t> cycles;
    cycles.reserve(sizes.size());
    for (uint64_t s : sizes) {
      const uint64_t c = ServiceCycles(next, s, bw);
      cycles.push_back(c);
      service += c;
    }
    last_busy_until = start + service;

    const uint64_t pcie = IsPciePort(next) ? PcieDelay(next, r.dst) : 0;

    ctx.Span(Unit::kRouter, r.payload->uid, r.payload->tid, start,
             start + service, SpanState::kTransfer, r.size);

    uint64_t cumulative = 0;
    uint64_t offset = r.byte_offset;
    for (size_t i = 0; i < sizes.size(); ++i) {
      cumulative += cycles[i];
      const Time arrive =
          now + queueing + cumulative + base_wire + pcie;

      Req frag;
      frag.payload = r.payload;
      frag.dst = r.dst;
      frag.size = sizes[i];
      frag.payload_size = r.payload_size;
      frag.byte_offset = offset;
      frag.frag_id = i;
      frag.total_frag = sizes.size();
      frag.is_tail = i + 1 == sizes.size() ? 1 : 0;
      frag.hop_count = r.hop_count + 1;
      offset += sizes[i];

      if (next == Port::kLocal) {
        LocalItem item;
        item.req = std::move(frag);
        item.arrive = arrive;
        item.seq = local_seq++;
        local_pending.push_back(std::move(item));
      } else {
        PushToPeer(next, frag, arrive);
        ++forwarded;
      }
    }
  }

  // ------------------------------------------------------------ 切分与重组

  std::vector<uint64_t> Split(Port next, Req const& r, uint64_t bw) const {
    if (next == Port::kLocal || r.size <= bw) return {r.size};
    std::vector<uint64_t> out;
    const uint64_t count = (r.size + bw - 1) / bw;
    out.reserve(count);
    for (uint64_t i = 0; i < count; ++i) {
      out.push_back(std::min(bw, r.size - i * bw));
    }
    return out;
  }

  uint64_t ServiceCycles(Port next, uint64_t size, uint64_t bw) const {
    if (next == Port::kLocal) return ctx.P().noc_router_delay;
    return std::max(ctx.P().noc_router_delay, (size + bw - 1) / bw);
  }

  uint64_t BandwidthFor(Port p) const {
    if (p == Port::kLocal) return ctx.P().noc_bandwidth;
    const uint64_t own = peer[static_cast<uint32_t>(p)].bandwidth;
    if (own > 0) return own;
    return IsPciePort(p) ? ctx.P().pcie_bandwidth : ctx.P().noc_bandwidth;
  }

  uint64_t PcieDelay(Port p, Coord dst) const {
    const uint64_t own = peer[static_cast<uint32_t>(p)].delay;
    if (own > 0) return own;
    const bool cross_node =
        cfg->ExtAt(coord) != nullptr || cfg->ExtAt(dst) != nullptr;
    return cross_node ? ctx.P().cross_node_delay : ctx.P().cross_chip_delay;
  }

  // 攒回整拍。上一跳按更窄的带宽切过，这一跳要按自己的带宽重切，所以先攒。
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
             "Router: transit reassembly overshot the beat size.");
    transit.erase(key);
    r->size = r->payload_size;
    r->byte_offset = 0;
    r->frag_id = 0;
    r->total_frag = 1;
    r->is_tail = 1;
    return true;
  }

  // ------------------------------------------------------------ 投递与转发

  void DeliverLocal(Time now) {
    if (local_pending.empty()) return;
    std::vector<LocalItem> ready;
    std::vector<LocalItem> rest;
    for (LocalItem& item : local_pending) {
      if (item.arrive <= now) ready.push_back(std::move(item));
      else rest.push_back(std::move(item));
    }
    local_pending.swap(rest);
    if (ready.empty()) return;

    std::sort(ready.begin(), ready.end(),
              [](LocalItem const& a, LocalItem const& b) {
                if (a.arrive != b.arrive) return a.arrive < b.arrive;
                return a.seq < b.seq;
              });

    LOGCHECK(local != nullptr, "Router: local sink is not connected.");
    for (LocalItem& item : ready) {
      Req& r = item.req;
      if (r.payload_size > 0 && r.size < r.payload_size) {
        FragKey key;
        key.beat = MakeBeatKey(*r.payload);
        key.beat_id = r.payload->beat_id;
        key.xfer_id = r.payload->xfer_id;
        uint64_t& got = arrived[key];
        got += r.size;
        if (got < r.payload_size) continue;
        arrived.erase(key);
      }
      local->HandleComm(r.payload);
      ++delivered;
    }
  }

  void PushToPeer(Port next, Req const& frag, Time arrive) {
    Peer const& target = peer[static_cast<uint32_t>(next)];
    LOGCHECK(target.inbox != nullptr, "Router: chosen port has no peer.");
    LOGCHECK(!target.inbox->IsFull(),
             "Router: peer inbox overflowed, give the link more depth.");

    RoutedPkt pkt(clk);
    pkt.dst = EncodeCoord(frag.dst);
    pkt.src = EncodeCoord(coord);
    pkt.size = frag.size;
    pkt.payload_size = frag.payload_size;
    pkt.byte_offset = frag.byte_offset;
    pkt.fragment_id = frag.frag_id;
    pkt.total_frag = frag.total_frag;
    pkt.is_tail = frag.is_tail;
    pkt.arrive_cycle = static_cast<uint64_t>(arrive);
    pkt.hop_count = frag.hop_count;
    pkt.in_port = static_cast<uint64_t>(OppositePort(next));
    pkt.payload = frag.payload;
    target.inbox->Push(pkt);
  }

  // ------------------------------------------------------------ 路由

  Port ComputeRoute(Coord dst) const {
    // 目的是外部节点：先换成接它的网关核，到了网关核再从登记的端口送出去
    ExtRoute const* ext = cfg->ExtAt(dst);
    if (ext != nullptr) {
      if (coord == ext->gateway) return ext->eject;
      dst = ext->gateway;
    }

    // 显式交换拓扑指名过的目的地，从指名的那个口出去。它排在跨 chip 换算之前：交换
    // 网是片外的直达，不必先回到本 chip 的出口网关。
    if (!switch_routes.empty()) {
      auto hit = switch_routes.find(EncodeCoord(dst));
      if (hit != switch_routes.end()) return hit->second;
    }

    // 目的在别的 chip 上：先换成本 chip 朝那个方向的网关核
    if (cfg->dim.Inside(coord) && cfg->dim.Inside(dst)) {
      const Coord src_chip = cfg->dim.ChipOfCoord(coord);
      const Coord dst_chip = cfg->dim.ChipOfCoord(dst);
      if (src_chip != dst_chip) {
        const Direction dir = RouteConfig::DirectionTo(src_chip, dst_chip);
        Coord const* gw = cfg->GatewayOf(cfg->dim.ChipIdOf(src_chip), dir);
        if (gw != nullptr && *gw != coord) dst = *gw;
      }
    }

    // 不在阵列内的节点只有一条出路，往回走就行
    if (!internal) {
      for (uint32_t i = 0; i < kPortNum; ++i) {
        if (peer[i].inbox == nullptr) continue;
        if (peer[i].coord.row >= 0 && peer[i].coord.col >= 0) {
          return static_cast<Port>(i);
        }
      }
      for (uint32_t i = 0; i < kPortNum; ++i) {
        if (peer[i].inbox != nullptr) return static_cast<Port>(i);
      }
      return Port::kUnknown;
    }

    // 先列后行。同一个方向上 PCIe 口优先于 NoC 口：跨 chip 的那一跳只有 PCIe 走得通。
    if (dst.col > coord.col && Usable(Port::kPcieEast)) return Port::kPcieEast;
    if (dst.col < coord.col && Usable(Port::kPcieWest)) return Port::kPcieWest;

    if (dst.row > coord.row) {
      if (Usable(Port::kPcieSouth)) return Port::kPcieSouth;
      if (Usable(Port::kPcieDown)) return Port::kPcieDown;
    } else if (dst.row < coord.row) {
      if (Usable(Port::kPcieNorth)) return Port::kPcieNorth;
      if (Usable(Port::kPcieUp)) return Port::kPcieUp;
    }

    if (dst.col > coord.col && Usable(Port::kEast)) return Port::kEast;
    if (dst.col < coord.col && Usable(Port::kWest)) return Port::kWest;

    if (dst.row > coord.row && Usable(Port::kSouth)) return Port::kSouth;
    if (dst.row < coord.row && Usable(Port::kNorth)) return Port::kNorth;

    // 列还没对齐但左右都走不通，绕一行再说
    if (dst.col != coord.col) {
      if (Usable(Port::kSouth)) return Port::kSouth;
      if (Usable(Port::kNorth)) return Port::kNorth;
    }
    return Port::kUnknown;
  }

  // 内部核不往阵列外的邻居发包，除非路由明确指名了那个出口。
  bool Usable(Port p) const {
    Peer const& n = peer[static_cast<uint32_t>(p)];
    if (n.inbox == nullptr) return false;
    if (internal && !n.internal) return false;
    return true;
  }

  NodeContext ctx;
  Coord coord;
  RouteConfig const* cfg;
  bool internal = true;

  PacketTarget* local = nullptr;
  Peer peer[kPortNum];
  std::unique_ptr<Fifo<RoutedPkt>> in_fifo[kPortNum];
  std::vector<std::unique_ptr<Fifo<RoutedPkt>>> inject;

  std::deque<Req> port_queue[kPortNum];
  bool port_active[kPortNum] = {false};
  std::deque<Port> active;
  Time last_busy_until = 0;

  std::unordered_map<uint64_t, Port> switch_routes;
  std::unordered_map<FragKey, uint64_t, FragKeyHash> transit;
  std::unordered_map<FragKey, uint64_t, FragKeyHash> arrived;
  std::vector<LocalItem> local_pending;
  uint64_t local_seq = 0;
  uint64_t served = 0;
  uint64_t forwarded = 0;
  uint64_t delivered = 0;
};

}
}

#endif
