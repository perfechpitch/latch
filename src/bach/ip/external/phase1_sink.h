#ifndef _LATCH_BACH_IP_EXTERNAL_PHASE1_SINK_
#define _LATCH_BACH_IP_EXTERNAL_PHASE1_SINK_

// Phase1 那一段的落点。通道发出来的两份各落一处，收齐一整包就交给以太网交换节点。
//
// 落点的角色决定它交到交换节点的哪个入口：
//
//   MOE     交到 phase1_moe_ingress，包上那份 HitMap 装的是专家 id
//   BYPASS  交到 res_ingress，这一份不带 HitMap
//   GENERIC 只收下、只计数，不往下交。它是接线还没通到交换节点时的落点
//
// 与结果汇聚点一样是两层收齐：链路上切开的片由它自己那个路由器攒回整拍，整拍再按
// uid、层号、tid、tag、opcode 攒成一整包。收齐才交，半包不交。

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/clock.h"
#include "base/fifo.h"
#include "base/log.h"
#include "base/module.h"
#include "base/runtime.h"
#include "bach/common/packet.h"
#include "bach/common/params.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/chip/core/router/router.h"
#include "bach/ip/eth_switch/eth_switch.h"
#include "bach/ip/node_context.h"
#include "bach/ip/route_config.h"
#include "bach/observer/span_recorder.h"

namespace latch {
namespace bach {

enum class SinkRole : uint32_t {
  kGeneric = 0,
  kMoe = 1,
  kBypass = 2,
};

inline const char* SinkRoleName(SinkRole r) {
  switch (r) {
    case SinkRole::kMoe: return "MOE";
    case SinkRole::kBypass: return "BYPASS";
    default: return "GENERIC";
  }
}

class Phase1Sink : public ClkModule, public PacketTarget {
 public:
  Phase1Sink(ClockPtr clock, Params const& params, uint64_t node, Coord position,
             RouteConfig const* route, SinkRole sink_role, uint64_t vol,
             uint64_t bandwidth, std::string const& name, uint64_t parent)
      : ClkModule(clock),
        p(params),
        ctx(MakeContext(node, &p, &recorder)),
        coord(position),
        role(sink_role),
        volume(vol),
        pcie_bandwidth(bandwidth > 0 ? bandwidth : params.pcie_bandwidth),
        self_id(RegisterId(name, parent)),
        router(clock, ctx, position, route, "router", self_id) {
    router.Connect(this);
  }

  // 接到交换节点的哪个入口上。角色决定入口，不由接线方随便挑。
  void ConnectEth(Fifo<EthReq>* port) {
    LOGCHECK(port != nullptr, "Phase1Sink: the eth ingress port is null.");
    LOGCHECK(role != SinkRole::kGeneric,
             "Phase1Sink: a generic landing point has nowhere to hand its "
             "packets on to.");
    eth = port;
  }

  Router& Rt() { return router; }
  SpanRecorder const& Recorder() const { return recorder; }
  SpanRecorder& MutableRecorder() { return recorder; }
  Coord Position() const { return coord; }
  SinkRole Role() const { return role; }

  uint64_t LandedNum() const { return landed; }
  uint64_t HandedOnNum() const { return handed; }

  void HandleComm(CommInstPtr const& payload) override {
    LOGCHECK(payload != nullptr, "Phase1Sink: null payload.");
    CommInst const& c = *payload;
    const uint64_t need =
        c.total_fragments > 0
            ? c.total_fragments
            : CalcCycles(static_cast<int64_t>(volume), pcie_bandwidth);
    LOGCHECK(c.beat_id < need,
             "Phase1Sink: beat id out of range, the route is wrong.");

    BeatKey key = MakeBeatKey(c);
    Beats& b = recv[key];
    LOGCHECK(b.seen.count(c.beat_id) == 0,
             "Phase1Sink: duplicate beat, the packet was cloned or looped.");
    if (b.seen.empty()) {
      b.first_cycle = RT::Now();
      b.payload = payload;
    }
    b.seen.insert(c.beat_id);
    if (b.seen.size() < need) return;

    CommInstPtr whole = b.payload;
    const Time first = b.first_cycle;
    recv.erase(key);

    ++landed;
    ctx.Span(Unit::kSink, whole->uid, whole->tid, first, RT::Now(),
             SpanState::kTransfer, volume, /*allow_zero=*/true);
    Check(*whole);
    ready.push_back(whole);
  }

  void Cycle() override {
    DelayCycle(1);

    router.Step();
    HandOn();
    CheckArrivals(RT::Now());

    TracePerCycle("landed", landed);
    TracePerCycle("handed", handed);
  }

 private:
  struct Beats {
    std::unordered_set<uint64_t> seen;
    Time first_cycle = 0;
    CommInstPtr payload;
  };

  static NodeContext MakeContext(uint64_t node, Params const* params,
                                 SpanRecorder* rec) {
    NodeContext c;
    c.node_id = node;
    c.params = params;
    c.rec = rec;
    return c;
  }

  // 角色与包上带的东西对不对得上。MoE 那一路必须带专家，残差那一路必须不带。
  void Check(CommInst const& c) const {
    if (role == SinkRole::kMoe) {
      LOGCHECK(!c.hit_map.empty(),
               "Phase1Sink: a MoE landing point received a packet with no "
               "expert HitMap.");
    } else if (role == SinkRole::kBypass) {
      LOGCHECK(c.hit_map.empty(),
               "Phase1Sink: a residual landing point received a packet that "
               "carries a HitMap.");
    }
  }

  void HandOn() {
    if (ready.empty()) return;
    if (eth == nullptr) {
      // GENERIC 的落点收下就完了，没有下一跳。
      LOGCHECK(role == SinkRole::kGeneric,
               "Phase1Sink: this landing point has packets to hand on but is "
               "not wired to the eth switch.");
      ready.clear();
      return;
    }
    while (!ready.empty()) {
      LOGCHECK(!eth->IsFull(),
               "Phase1Sink: the eth ingress overflowed, give it more depth.");
      CommInstPtr c = ready.front();
      ready.erase(ready.begin());
      EthReq r(clk);
      if (role == SinkRole::kMoe) {
        r.ingress = static_cast<uint64_t>(EthPort::kPhase1MoeIngress);
        r.role = static_cast<uint64_t>(EthRole::kMoeRequest);
        r.semantic = static_cast<uint64_t>(HitMapSemantic::kRoutedExpertId);
      } else {
        r.ingress = static_cast<uint64_t>(EthPort::kResIngress);
        r.role = static_cast<uint64_t>(EthRole::kResidual);
        r.semantic = static_cast<uint64_t>(HitMapSemantic::kNone);
      }
      r.bytes = 0;  // 交换节点按它自己那个默认包长算
      r.payload = c;
      eth->Push(r);
      ++handed;
    }
  }

  // 收了首拍却迟迟收不齐的包，到点就停机。
  void CheckArrivals(Time now) const {
    for (auto const& kv : recv) {
      LOGCHECK(now - kv.second.first_cycle <= p.watchdog_lifespan,
               "Phase1Sink: a packet started arriving but never completed.");
    }
  }

  Params p;
  SpanRecorder recorder;
  NodeContext ctx;
  Coord coord;
  SinkRole role;
  uint64_t volume;
  uint64_t pcie_bandwidth;
  uint64_t self_id;
  Router router;

  Fifo<EthReq>* eth = nullptr;
  std::unordered_map<BeatKey, Beats, BeatKeyHash> recv;
  std::vector<CommInstPtr> ready;
  uint64_t landed = 0;
  uint64_t handed = 0;
};

}
}

#endif
