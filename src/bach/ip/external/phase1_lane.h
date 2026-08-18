#ifndef _LATCH_BACH_IP_EXTERNAL_PHASE1_LANE_
#define _LATCH_BACH_IP_EXTERNAL_PHASE1_LANE_

// Phase1 的一条通道。它是三段流水里的第一段，在网卡那一侧，不在核阵列里。
//
// 一个 user 在它这里走两步，中间分岔：
//
//   FC0 与残差算完   把残差那一份发给它的落点，这一份不带 HitMap
//   归一化与选路算完 把 MoE 请求发给另一个落点，这一份带着本次选中的那几个专家
//
// 两个落点必须是两个，一个通道不能把两份都发到同一处：残差与 MoE 请求在下一跳走的
// 是交换节点的两条不同入口，合到一处就分不开了。
//
// 它没有额度反压。产 user 只看推包间隔，一条通道同时只处理一个 user，后面的排队等着：
// 产 user 与处理是两条并行的线，产得比处理快时队列会涨，涨到哪算哪，上游感知不到。
// 这与全模型“队列满不挡上游”是同一条语义。
//
// 一次 run 的起跑时刻由它记：Phase 模式下一个 user 的端到端是从它进这条通道那一刻
// 算起，不是从 Phase2 的注入源算起。

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/log.h"
#include "base/module.h"
#include "base/runtime.h"
#include "bach/common/packet.h"
#include "bach/common/params.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/chip/core/router/router.h"
#include "bach/ip/node_context.h"
#include "bach/ip/route_config.h"
#include "bach/observer/span_recorder.h"

namespace latch {
namespace bach {

// 一条通道的行为参数。四段延迟与两个落点都由 Map 给，这里不设默认。
struct Phase1LaneSpec {
  uint64_t layer_id = 0;
  int64_t group_id = -1;  // 这条通道属于哪个 chip 组

  Coord bypass_target;
  Coord moe_target;
  bool has_bypass = false;
  bool has_moe = false;

  uint64_t bypass_volume = 0;
  uint64_t moe_volume = 0;

  uint64_t fc0_delay = 0;
  uint64_t res_delay = 0;
  uint64_t norm_delay = 0;
  uint64_t router_delay = 0;
  uint64_t push_delay = 0;

  // 本次选中的那几个专家。交换节点按它换算成 EPGroup。
  std::vector<uint32_t> hit_map;
};

class Phase1Lane : public ClkModule, public PacketTarget {
 public:
  Phase1Lane(ClockPtr clock, Params const& params, uint64_t node, Coord position,
             RouteConfig const* route, Phase1LaneSpec const& lane_spec,
             uint64_t bandwidth, std::string const& name, uint64_t parent)
      : ClkModule(clock),
        p(params),
        ctx(MakeContext(node, &p, &recorder)),
        coord(position),
        spec(lane_spec),
        pcie_bandwidth(bandwidth > 0 ? bandwidth : params.pcie_bandwidth),
        lane_id(static_cast<int64_t>(node)),
        self_id(RegisterId(name, parent)),
        router(clock, ctx, position, route, "router", self_id) {
    LOGCHECK(spec.has_bypass || spec.has_moe,
             "Phase1Lane: a lane with no target sends nothing.");
    LOGCHECK(!spec.has_bypass || !spec.has_moe ||
                 spec.bypass_target != spec.moe_target,
             "Phase1Lane: the residual and the MoE request need two different "
             "landing points.");
    LOGCHECK(!spec.has_moe || !spec.hit_map.empty(),
             "Phase1Lane: a MoE request needs a non-empty expert HitMap.");
    router.Connect(this);
    xfer_next = node << 40;
  }

  // 这条通道负责哪些 user。顺序就是推进去的顺序。
  void SetUsers(std::vector<uint64_t> list) { uids = std::move(list); }

  // 观测里认这条通道的号。交换节点那边按包上带的这个号分辨是哪条通道来的。
  void SetLaneId(int64_t id) { lane_id = id; }
  int64_t LaneId() const { return lane_id; }

  Router& Rt() { return router; }
  SpanRecorder const& Recorder() const { return recorder; }
  SpanRecorder& MutableRecorder() { return recorder; }
  Coord Position() const { return coord; }

  uint64_t PushedNum() const { return pushed; }
  uint64_t SentBypass() const { return sent_bypass; }
  uint64_t SentMoe() const { return sent_moe; }
  uint64_t QueuedUsers() const { return queue.size(); }
  bool AllPushed() const { return next_user >= uids.size(); }
  bool Idle() const { return stage == Stage::kIdle && queue.empty(); }

  // 它只发不收。有包落到这里说明路由错了。
  void HandleComm(CommInstPtr const& payload) override {
    (void)payload;
    LOGCHECK(false, "Phase1Lane: a packet landed on a lane, the route is wrong.");
  }

  void Cycle() override {
    DelayCycle(1);

    router.Step();
    StepGenerator(RT::Now());
    StepWorker(RT::Now());

    TracePerCycle("queued", queue.size());
    TracePerCycle("pushed", pushed);
    TracePerCycle("sent_moe", sent_moe);
  }

 private:
  enum class Stage : uint32_t {
    kIdle = 0,
    kFc0Res = 1,      // FC0 与残差
    kBypassTx = 2,    // 残差那一份在发
    kNormRouter = 3,  // 归一化与选路
    kMoeTx = 4,       // MoE 请求那一份在发
  };

  static NodeContext MakeContext(uint64_t node, Params const* params,
                                 SpanRecorder* rec) {
    NodeContext c;
    c.node_id = node;
    c.params = params;
    c.rec = rec;
    return c;
  }

  // 产 user 与处理是两条并行的线，这一条只看推包间隔。
  void StepGenerator(Time now) {
    if (next_user >= uids.size()) return;
    if (pushed > 0 && now < last_push + spec.push_delay) return;
    if (pushed == 0 && now < spec.push_delay) return;
    const uint64_t uid = uids[next_user++];
    // Phase 模式下一次工作的起点在这里。期望的完成条数恒为一，两路汇合只判一次。
    recorder.GlobalStart(uid, now, 1);
    queue.push_back(uid);
    last_push = now;
    ++pushed;
  }

  void StepWorker(Time now) {
    bool moved = true;
    while (moved) {
      moved = false;
      switch (stage) {
        case Stage::kIdle:
          if (queue.empty()) break;
          current = queue.front();
          queue.pop_front();
          stage_start = now;
          timer_end = now + spec.fc0_delay + spec.res_delay;
          stage = Stage::kFc0Res;
          moved = true;
          break;

        case Stage::kFc0Res:
          if (now < timer_end) break;
          ctx.Span(Unit::kLane, current, 0, stage_start, now,
                   SpanState::kCompute);
          if (spec.has_bypass) {
            StartBurst(Opcode::kUserInit, 0, spec.bypass_target,
                       spec.bypass_volume, /*with_hit_map=*/false, now);
            stage = Stage::kBypassTx;
          } else {
            stage_start = now;
            timer_end = now + spec.norm_delay + spec.router_delay;
            stage = Stage::kNormRouter;
          }
          moved = true;
          break;

        case Stage::kBypassTx:
          if (!StepBurst(now)) break;
          ++sent_bypass;
          stage_start = now;
          timer_end = now + spec.norm_delay + spec.router_delay;
          stage = Stage::kNormRouter;
          moved = true;
          break;

        case Stage::kNormRouter:
          if (now < timer_end) break;
          ctx.Span(Unit::kLane, current, 0, stage_start, now,
                   SpanState::kCompute);
          if (spec.has_moe) {
            StartBurst(Opcode::kUserInit,
                       static_cast<uint64_t>(spec.hit_map.size()),
                       spec.moe_target, spec.moe_volume,
                       /*with_hit_map=*/true, now);
            stage = Stage::kMoeTx;
          } else {
            stage = Stage::kIdle;
          }
          moved = true;
          break;

        case Stage::kMoeTx:
          if (!StepBurst(now)) break;
          ++sent_moe;
          stage = Stage::kIdle;
          moved = true;
          break;
      }
    }
  }

  // 起一次发送。第一片当拍就走，之后每拍一片。发完整份才轮到下一段，一条通道上
  // 的两份是前后关系，不是并行。
  void StartBurst(Opcode op, uint64_t tag, Coord dst, uint64_t volume,
                  bool with_hit_map, Time now) {
    burst.op = op;
    burst.tag = tag;
    burst.dst = dst;
    burst.volume = volume;
    burst.with_hit_map = with_hit_map;
    burst.start = now;
    burst.len = CalcCycles(static_cast<int64_t>(volume), pcie_bandwidth);
    burst.next_beat = 0;
    Emit(now);
  }

  // 这一份发完了没有。发完最后一片还要再过一拍才交出去，与 Bach 的逐拍发送一致。
  bool StepBurst(Time now) {
    while (burst.next_beat < burst.len && now >= burst.start + burst.next_beat) {
      Emit(now);
    }
    if (burst.next_beat < burst.len) return false;
    if (now < burst.start + burst.len) return false;
    ctx.Span(Unit::kLane, current, 0, burst.start, now, SpanState::kTransfer,
             burst.volume);
    return true;
  }

  void Emit(Time now) {
    const uint64_t offset = burst.next_beat * pcie_bandwidth;
    const uint64_t chunk = burst.volume > offset
                               ? std::min(pcie_bandwidth, burst.volume - offset)
                               : 0;
    CommInstPtr c = std::make_shared<CommInst>();
    c->uid = current;
    c->tid = 0;
    c->opcode = burst.op;
    c->tag = burst.tag;
    c->layer_id = spec.layer_id;
    c->beat_id = burst.next_beat;
    c->total_fragments = burst.len;
    c->vol = chunk;
    c->xfer_id = xfer_next++;
    if (burst.with_hit_map) c->hit_map = spec.hit_map;
    c->phase1_lane_id = lane_id;
    c->phase1_group_id = spec.group_id;
    router.Inject(c, burst.dst, chunk);
    ++burst.next_beat;
    (void)now;
  }

  struct Burst {
    Opcode op = Opcode::kUserInit;
    uint64_t tag = 0;
    Coord dst;
    uint64_t volume = 0;
    bool with_hit_map = false;
    Time start = 0;
    uint64_t len = 0;
    uint64_t next_beat = 0;
  };

  Params p;
  SpanRecorder recorder;
  NodeContext ctx;
  Coord coord;
  Phase1LaneSpec spec;
  uint64_t pcie_bandwidth;
  int64_t lane_id;
  uint64_t self_id;
  Router router;

  std::vector<uint64_t> uids;
  std::deque<uint64_t> queue;
  Burst burst;

  size_t next_user = 0;
  uint64_t pushed = 0;
  uint64_t sent_bypass = 0;
  uint64_t sent_moe = 0;
  uint64_t current = 0;
  Time last_push = 0;
  Time timer_end = 0;
  Time stage_start = 0;
  Stage stage = Stage::kIdle;
  uint64_t xfer_next = 0;
};

}
}

#endif
