#ifndef _LATCH_BACH_IP_EXTERNAL_OUT_
#define _LATCH_BACH_IP_EXTERNAL_OUT_

// 结果汇聚。阵列算完的结果最后都送到这里，收齐一个 user 该有的全部分片，这个 user
// 才算完成。
//
// 两层收齐，各管各的：
//
//   一片攒成一拍  链路上按带宽切开的片，由它自己那个路由器攒回整拍
//   一拍攒成一包  一包是若干拍，按 uid、layer_id、tid、tag、opcode 攒拍号
//
// 收齐一包就记一条完成，到此为止。一个 uid 的结果可能落在好几个汇聚点上，谁也不知道
// 全局够了没有，所以"这个 user 完成了没有"不在这里判，等汇总时数分片。
//
// Phase 模式下这里不是终点：算完的结果要交回以太网交换节点，与绕过 MoE 的那份残差
// 汇合了才算完。接上那条回程之后它就不再自己记完成，全局完成权在汇合点那一处。

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
#include "base/logic.h"
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

class Out : public ClkModule, public PacketTarget {
 public:
  Out(ClockPtr clock, Params const& params, uint64_t node, Coord position,
      RouteConfig const* route, uint64_t vol, uint64_t bandwidth,
      std::string const& name, uint64_t parent)
      : ClkModule(clock),
        p(params),
        ctx(MakeContext(node, &p, &recorder)),
        coord(position),
        volume(vol),
        pcie_bandwidth(bandwidth),
        self_id(RegisterId(name, parent)),
        router(clock, ctx, position, route, "router", self_id),
        done_count(clock) {
    LOGCHECK(pcie_bandwidth > 0, "Out: bandwidth must be positive.");
    router.Connect(this);
  }

  // 接上回交换节点那条回程。接了之后收齐的结果不再算完成，往交换节点交一份，等它
  // 与残差汇合。
  void ConnectResultBridge(Fifo<EthReq>* port) {
    LOGCHECK(port != nullptr, "Out: the result bridge port is null.");
    bridge = port;
  }

  bool BridgedToEth() const { return bridge != nullptr; }
  uint64_t BridgedNum() const { return bridged; }

  Router& Rt() { return router; }
  SpanRecorder const& Recorder() const { return recorder; }
  SpanRecorder& MutableRecorder() { return recorder; }

  // 收齐的包数。一个 uid 可能收到不止一包。
  uint64_t CompletedNum() const { return landed; }
  // 跨模块可读的那一份。别的协程读它拿到的是上一拍提交的值，所以判完成会晚一拍，
  // 这一拍不影响任何模型量，只影响什么时候停时钟。
  Logic64 const& DoneCount() const { return done_count; }
  uint64_t ArrivedFragments(uint64_t uid) const {
    auto it = arrived.find(uid);
    return it == arrived.end() ? 0 : it->second;
  }

  void HandleComm(CommInstPtr const& payload) override {
    LOGCHECK(payload != nullptr, "Out: null payload.");
    CommInst const& c = *payload;
    const uint64_t need = c.total_fragments > 0
                              ? c.total_fragments
                              : CalcCycles(static_cast<int64_t>(volume),
                                           pcie_bandwidth);
    LOGCHECK(c.beat_id < need, "Out: beat id out of range, route is wrong.");

    BeatKey key = MakeBeatKey(c);
    Beats& b = recv[key];
    LOGCHECK(b.seen.count(c.beat_id) == 0,
             "Out: duplicate beat, packet was cloned or looped.");
    if (b.seen.empty()) {
      b.first_cycle = RT::Now();
      b.payload = payload;
    }
    b.seen.insert(c.beat_id);
    if (b.seen.size() < need) return;
    CommInstPtr whole = b.payload;
    recv.erase(key);

    ++arrived[c.uid];
    ++landed;
    if (bridge != nullptr) {
      ready.push_back(whole);
      return;
    }
    recorder.GlobalEnd(c.uid, RT::Now());
  }

  void Cycle() override {
    DelayCycle(1);

    router.Step();
    HandOn();
    CheckArrivals(RT::Now());

    done_count = landed;
    TracePerCycle("landed", landed);
    TracePerCycle("router_queued", router.QueuedBeats());
  }

 private:
  struct Beats {
    std::unordered_set<uint64_t> seen;
    Time first_cycle = 0;
    CommInstPtr payload;
  };

  // 把算完的结果交回交换节点。交上去的那一份仍然带着它是哪几个 group 算出来的，
  // 交换节点按它核对这份结果对应当初发出去的哪条请求，核对完才把 HitMap 清掉。
  void HandOn() {
    if (bridge == nullptr || ready.empty()) return;
    while (!ready.empty()) {
      LOGCHECK(!bridge->IsFull(),
               "Out: the result bridge overflowed, give it more depth.");
      CommInstPtr c = ready.front();
      ready.erase(ready.begin());
      LOGCHECK(c->phase1_lane_id >= 0 && c->phase1_group_id >= 0,
               "Out: a phase2 result reached the bridge without its phase "
               "identity.");
      LOGCHECK(c->opcode == Opcode::kMove || c->opcode == Opcode::kReduction,
               "Out: only a terminal MOVE or REDUCTION counts as an aggregated "
               "phase2 result.");
      LOGCHECK(!c->hit_map.empty(),
               "Out: a phase2 result reached the bridge without the groups it "
               "was computed on.");
      EthReq r(clk);
      r.ingress = static_cast<uint64_t>(EthPort::kPhase2MoeResultIngress);
      r.role = static_cast<uint64_t>(EthRole::kMoeResult);
      r.semantic = static_cast<uint64_t>(HitMapSemantic::kGroupId);
      r.bytes = 0;
      r.payload = c;
      bridge->Push(r);
      ++bridged;
    }
  }

  // 收了首拍却迟迟收不齐的包，到点就停机：这条路径上少了一拍，再等也不会来。
  void CheckArrivals(Time now) const {
    for (auto const& kv : recv) {
      LOGCHECK(now - kv.second.first_cycle <= p.watchdog_lifespan,
               "Out: a packet started arriving but never completed.");
    }
  }

  static NodeContext MakeContext(uint64_t node, Params const* params,
                                 SpanRecorder* rec) {
    NodeContext c;
    c.node_id = node;
    c.params = params;
    c.rec = rec;
    return c;
  }

  Params p;
  SpanRecorder recorder;
  NodeContext ctx;
  Coord coord;
  uint64_t volume;
  uint64_t pcie_bandwidth;
  uint64_t self_id;
  Router router;
  Logic64 done_count;

  Fifo<EthReq>* bridge = nullptr;
  std::vector<CommInstPtr> ready;
  std::unordered_map<BeatKey, Beats, BeatKeyHash> recv;
  std::unordered_map<uint64_t, uint64_t> arrived;
  uint64_t landed = 0;
  uint64_t bridged = 0;
};

}
}

#endif
