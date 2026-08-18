#ifndef _LATCH_BACH_IP_EXTERNAL_HOST_
#define _LATCH_BACH_IP_EXTERNAL_HOST_

// 注入源。它按顺序把一批 user 推进阵列，推一个扣一份额度，收到那个 user 的 RETIRE
// 才把额度还回来。
//
// 额度容量就是入口核的 stream 数，所以在飞的 user 数不会超过入口核能同时装下的数。
// 这是整条链路上第一道反压，也是唯一一道由注入侧感知得到的反压。
//
// 一个 user 的注入分三步：等额度、等推包间隔、逐拍把 Init 包发出去。第三步不挡住下
// 一个 user 的第一步，所以发包与下一轮等额度是重叠的。
//
// 它自己带一个路由器。外部设备不直接挂在网关核的端口上：包先进自己这个路由器的本地
// 口，由它按 PCIe 链路的带宽与延迟送到网关核；回来的包也在这里落地重组，再交给自己。
// 这样注入与接收各走一条正常的链路，时间该扣的一次不少。
//
// 它记全局起跑时刻。一次 run 的端到端延迟就是这个时刻到 Out 收齐那一刻。

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/fifo.h"
#include "base/log.h"
#include "base/logic.h"
#include "base/module.h"
#include "base/runtime.h"
#include "bach/common/arbiter.h"
#include "bach/common/packet.h"
#include "bach/common/params.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/chip/core/router/router.h"
#include "bach/ip/node_context.h"
#include "bach/ip/route_config.h"
#include "bach/observer/span_recorder.h"

namespace latch {
namespace bach {

// 一个要注入的 user：往哪个坐标推，包上带什么 tag，Out 那边等它几个分片。
//
// MoE 下还带两样：本次激活了哪几个 EPGroup，以及每个 group 上命中几个专家。它们由
// Map 编译时按同一个种子预先算好，一路随包传到各个核，谁认不认这个包就看它。
struct HostUser {
  uint64_t uid = 0;
  Coord target;
  uint64_t tag = 0;
  uint64_t expected_fragments = 1;
  std::vector<uint32_t> hit_map;
  std::vector<std::pair<uint32_t, uint32_t>> moe_bitmap;
};

// 上游交下来的一个 user。Phase 模式下注入名单不是预先给好的，交换节点把 Phase1 的
// MoE 请求换算完才知道该往哪个 Host 推哪几个 group。
//
// 走这条路进来的 user 不扣额度、不等推包间隔、不记起跑：这次工作的起点在 Phase1，
// Host 在这条路上是中转的一段，不是发起方。
class HostOffer : public Logic {
 public:
  Logic64 uid;
  Logic64 tag;
  Logic64 target;    // EncodeCoord 编码
  Logic64 layer_id;
  Logic64 lane_id;
  Logic64 group_id;
  LogicPtr<CommInst> tmpl;  // hit_map 与 moe_bitmap 是变长的，挂在这里带过来

  explicit HostOffer(ClockPtr c)
      : uid(c), tag(c), target(c), layer_id(c), lane_id(c), group_id(c),
        tmpl(c) {
    Fields(uid, tag, target, layer_id, lane_id, group_id);
    Fields(tmpl);
  }
};

class Host : public ClkModule, public PacketTarget {
 public:
  // init_opcode 决定推进去的包在入口核那边算哪种任务：普通核收 USER_INIT，广播核
  // 收 FIFO_IN。它不是注入源自己的属性，是入口核任务表第一行的 opcode。
  Host(ClockPtr clock, Params const& params, uint64_t node, Coord position,
       RouteConfig const* route, uint64_t vol, uint64_t bandwidth,
       uint64_t credit_capacity, Opcode init_opcode, std::string const& name,
       uint64_t parent)
      : ClkModule(clock),
        p(params),
        ctx(MakeContext(node, &p, &recorder)),
        coord(position),
        volume(vol),
        pcie_bandwidth(bandwidth),
        credits(credit_capacity, credit_capacity),
        init_op(init_opcode),
        self_id(RegisterId(name, parent)),
        router(clock, ctx, position, route, "router", self_id) {
    LOGCHECK(pcie_bandwidth > 0, "Host: bandwidth must be positive.");
    LOGCHECK(init_op == Opcode::kUserInit || init_op == Opcode::kFifoIn,
             "Host: an entry core takes either USER_INIT or FIFO_IN.");
    router.Connect(this);
    xfer_next = node << 40;
  }

  // 注入名单。顺序就是推包顺序。
  void SetUsers(std::vector<HostUser> list) { users = std::move(list); }

  // 上游交 user 进来的口。Phase 模式下用它，别处不开。
  Fifo<HostOffer>& OfferPort() {
    if (offers == nullptr) {
      offers = std::make_unique<Fifo<HostOffer>>(p.link_fifo_depth, clk);
    }
    return *offers;
  }

  uint64_t TakenOffers() const { return taken; }

  // 分发器要统一申请它涉及的那几个注入源的额度，所以额度这本账要对它开放。
  CreditCounter& Credits() { return credits; }

  Router& Rt() { return router; }
  SpanRecorder const& Recorder() const { return recorder; }
  SpanRecorder& MutableRecorder() { return recorder; }

  uint64_t CreditLevel() const { return credits.Level(); }
  uint64_t PushedNum() const { return pushed; }
  bool AllPushed() const { return next_user >= users.size(); }

  // Host 只认 RETIRE，而 RETIRE 恒为一拍。别的包到这里就是路由错了。
  void HandleComm(CommInstPtr const& payload) override {
    LOGCHECK(payload != nullptr, "Host: null payload.");
    CommInst const& c = *payload;
    LOGCHECK(c.opcode == Opcode::kRetire, "Host: received a non-retire packet.");
    LOGCHECK(c.beat_id == 0 && c.total_fragments <= 1,
             "Host: retire must be a single beat.");
    credits.Put(RT::Now(), 1);
  }

  void Cycle() override {
    DelayCycle(1);

    router.Step();
    StepBursts();
    StepOffers(RT::Now());
    StepGenerator();

    TracePerCycle("credits", credits.Level());
    TracePerCycle("pushed", pushed);
    TracePerCycle("bursts", bursts.size());
  }

 private:
  enum class Stage : uint32_t {
    kIdle = 0,
    kWaitCredit = 1,
    kPushDelay = 2,
  };

  struct Burst {
    uint64_t uid = 0;
    uint64_t tag = 0;
    Coord target;
    std::vector<uint32_t> hit_map;
    std::vector<std::pair<uint32_t, uint32_t>> moe_bitmap;
    uint64_t layer_id = 0;
    int64_t lane_id = -1;
    int64_t group_id = -1;
    Time start = 0;
    uint64_t burst_len = 0;
    uint64_t next_beat = 0;
  };

  static NodeContext MakeContext(uint64_t node, Params const* params,
                                 SpanRecorder* rec) {
    NodeContext c;
    c.node_id = node;
    c.params = params;
    c.rec = rec;
    return c;
  }

  void StepBursts() {
    const Time now = RT::Now();
    for (Burst& b : bursts) {
      if (b.next_beat >= b.burst_len) continue;
      if (now < b.start + b.next_beat) continue;
      Emit(b);
    }
    size_t keep = 0;
    for (size_t i = 0; i < bursts.size(); ++i) {
      if (bursts[i].next_beat >= bursts[i].burst_len) continue;
      if (keep != i) bursts[keep] = bursts[i];
      ++keep;
    }
    bursts.resize(keep);
  }

  void Emit(Burst& b) {
    const uint64_t offset = b.next_beat * pcie_bandwidth;
    const uint64_t chunk =
        volume > offset ? std::min(pcie_bandwidth, volume - offset) : 0;
    CommInstPtr c = std::make_shared<CommInst>();
    c->uid = b.uid;
    c->tid = 0;
    c->opcode = init_op;
    c->tag = b.tag;
    c->layer_id = b.layer_id;
    c->beat_id = b.next_beat;
    c->total_fragments = b.burst_len;
    c->vol = chunk;
    c->xfer_id = xfer_next++;
    c->hit_map = b.hit_map;
    c->moe_bitmap = b.moe_bitmap;
    c->phase1_lane_id = b.lane_id;
    c->phase1_group_id = b.group_id;
    router.Inject(c, b.target, chunk);
    ++b.next_beat;
  }

  // 上游交下来的那些，来一个发一个。
  void StepOffers(Time now) {
    if (offers == nullptr) return;
    while (!offers->IsEmpty()) {
      HostOffer& o = offers->Front();
      CommInstPtr tmpl = o.tmpl.Get();
      LOGCHECK(tmpl != nullptr, "Host: an offered user has no template.");
      Burst b;
      b.uid = uint64_t(o.uid);
      b.tag = uint64_t(o.tag);
      b.target = DecodeCoord(uint64_t(o.target));
      b.hit_map = tmpl->hit_map;
      b.moe_bitmap = tmpl->moe_bitmap;
      b.layer_id = uint64_t(o.layer_id);
      b.lane_id = static_cast<int64_t>(uint64_t(o.lane_id));
      b.group_id = static_cast<int64_t>(uint64_t(o.group_id));
      b.start = now;
      b.burst_len = CalcCycles(static_cast<int64_t>(volume), pcie_bandwidth);
      offers->Pop();
      bursts.push_back(b);
      Emit(bursts.back());
      ++pushed;
      ++taken;
    }
  }

  void StepGenerator() {
    const Time now = RT::Now();
    bool moved = true;
    while (moved) {
      moved = false;
      switch (stage) {
        case Stage::kIdle:
          if (next_user >= users.size()) break;
          credit_ticket = credits.Get(now, 1);
          credit_start = now;
          stage = Stage::kWaitCredit;
          moved = true;
          break;

        case Stage::kWaitCredit:
          if (!credits.Granted(credit_ticket)) {
            // 等额度是正常的反压，等到天荒地老就不是了：下游有 user 没退休回来。
            LOGCHECK(now - credit_start <= p.host_credit_timeout,
                     "Host: waited past the credit timeout, the pipeline is "
                     "stuck downstream.");
            break;
          }
          recorder.Wait(ctx.node_id, Unit::kHost, users[next_user].uid, 0,
                        credit_start, credits.GrantCycle(credit_ticket),
                        WaitReason::kHostCredit);
          credits.Drop(credit_ticket);
          credit_ticket = kNoTicket;
          timer_end = now + p.host_push_delay;
          stage = Stage::kPushDelay;
          moved = true;
          break;

        case Stage::kPushDelay: {
          if (now < timer_end) break;
          HostUser const& u = users[next_user];
          recorder.GlobalStart(u.uid, now, u.expected_fragments);
          Burst b;
          b.uid = u.uid;
          b.tag = u.tag;
          b.target = u.target;
          b.hit_map = u.hit_map;
          b.moe_bitmap = u.moe_bitmap;
          b.start = now;
          b.burst_len =
              CalcCycles(static_cast<int64_t>(volume), pcie_bandwidth);
          bursts.push_back(b);
          Emit(bursts.back());
          ++pushed;
          ++next_user;
          stage = Stage::kIdle;
          moved = true;
          break;
        }
      }
    }
  }

  Params p;
  SpanRecorder recorder;
  NodeContext ctx;
  Coord coord;
  uint64_t volume;
  uint64_t pcie_bandwidth;
  CreditCounter credits;
  Opcode init_op;
  uint64_t self_id;
  Router router;

  std::vector<HostUser> users;
  std::vector<Burst> bursts;
  std::unique_ptr<Fifo<HostOffer>> offers;

  size_t next_user = 0;
  uint64_t pushed = 0;
  uint64_t taken = 0;
  Stage stage = Stage::kIdle;
  Ticket credit_ticket = kNoTicket;
  Time credit_start = 0;
  Time timer_end = 0;
  uint64_t xfer_next = 0;
};

}
}

#endif
