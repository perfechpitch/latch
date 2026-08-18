#ifndef _LATCH_BACH_IP_EXTERNAL_DISPATCHER_
#define _LATCH_BACH_IP_EXTERNAL_DISPATCHER_

// MoE 的分发器。一次 run 至多一个。
//
// MoE 下一个 user 可能同时进好几个注入源：本次命中的 EPGroup 分属不同的注入源，每个
// 注入源各推一份。这件事由分发器统一做，不是各注入源各推各的：
//
//   串行      一次只处理一个 user，处理完才轮到下一个
//   齐额度    它要进的那几个注入源，额度全部到位才发，不是各发各的
//   一次间隔  推包间隔按 user 算一次，不是每个注入源各算一次
//   记一次    一个 user 的起跑只记一次，从分发器算起
//
// 少了这几条，一个 user 进两个注入源时会被记两次起跑，端到端从早的那次算起，比实际
// 短一个推包间隔；额度也会变成各等各的，一个注入源堵住挡不住另一个先发。
//
// 分发器只管额度、节奏与起跑，包本身由注入源发：它把定好的那一份交给注入源，注入源
// 照发。所以走的还是注入源那条出口，链路时间一分不差。

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/log.h"
#include "base/module.h"
#include "base/runtime.h"
#include "bach/common/arbiter.h"
#include "bach/common/packet.h"
#include "bach/common/params.h"
#include "bach/ip/external/host.h"
#include "bach/ip/node_context.h"
#include "bach/observer/span_recorder.h"

namespace latch {
namespace bach {

// 一个 user 要进的一个注入源：往哪个核推，包上带什么 tag。
struct DispatchTarget {
  Host* host = nullptr;
  Coord target;
  uint64_t tag = 0;
};

// 一个 user 的整份分发计划。命中的 EPGroup 与每个 group 上的专家数一路随包传下去。
struct DispatchItem {
  uint64_t uid = 0;
  uint64_t expected_fragments = 1;
  std::vector<uint32_t> hit_map;
  std::vector<std::pair<uint32_t, uint32_t>> moe_bitmap;
  std::vector<DispatchTarget> targets;
};

class MoeDispatcher : public ClkModule {
 public:
  MoeDispatcher(ClockPtr clock, Params const& params, uint64_t node,
                std::string const& name, uint64_t parent)
      : ClkModule(clock),
        p(params),
        ctx(MakeContext(node, &p, &recorder)) {
    RegisterId(name, parent);
  }

  // 分发名单。顺序就是处理顺序。
  void SetPlan(std::vector<DispatchItem> list) {
    for (DispatchItem const& item : list) {
      LOGCHECK(!item.targets.empty(),
               "MoeDispatcher: a user with no target goes nowhere.");
      for (DispatchTarget const& t : item.targets) {
        LOGCHECK(t.host != nullptr, "MoeDispatcher: a target has no host.");
      }
    }
    plan = std::move(list);
  }

  SpanRecorder const& Recorder() const { return recorder; }
  SpanRecorder& MutableRecorder() { return recorder; }

  uint64_t DispatchedNum() const { return dispatched; }
  bool AllDispatched() const { return next_item >= plan.size(); }

  void Cycle() override {
    DelayCycle(1);
    Step(RT::Now());

    TracePerCycle("dispatched", dispatched);
  }

 private:
  enum class Stage : uint32_t {
    kIdle = 0,
    kWaitCredit = 1,
    kPushDelay = 2,
  };

  static NodeContext MakeContext(uint64_t node, Params const* params,
                                 SpanRecorder* rec) {
    NodeContext c;
    c.node_id = node;
    c.params = params;
    c.rec = rec;
    return c;
  }

  void Step(Time now) {
    bool moved = true;
    while (moved) {
      moved = false;
      switch (stage) {
        case Stage::kIdle: {
          if (next_item >= plan.size()) break;
          DispatchItem const& item = plan[next_item];
          tickets.clear();
          tickets.reserve(item.targets.size());
          for (DispatchTarget const& t : item.targets) {
            tickets.push_back(t.host->Credits().Get(now, 1));
          }
          credit_start = now;
          stage = Stage::kWaitCredit;
          moved = true;
          break;
        }

        case Stage::kWaitCredit: {
          DispatchItem const& item = plan[next_item];
          bool all = true;
          Time granted_at = now;
          for (size_t i = 0; i < tickets.size(); ++i) {
            Host* h = item.targets[i].host;
            if (!h->Credits().Granted(tickets[i])) { all = false; break; }
            const Time at = h->Credits().GrantCycle(tickets[i]);
            if (at > granted_at) granted_at = at;
          }
          if (!all) {
            // 等额度是正常的反压，等到天荒地老就不是了：下游有 user 没退休回来。
            LOGCHECK(now - credit_start <= p.host_credit_timeout,
                     "MoeDispatcher: waited past the credit timeout, the "
                     "pipeline is stuck downstream.");
            break;
          }
          recorder.Wait(ctx.node_id, Unit::kHost, item.uid, 0, credit_start,
                        granted_at, WaitReason::kHostCredit);
          for (size_t i = 0; i < tickets.size(); ++i) {
            item.targets[i].host->Credits().Drop(tickets[i]);
          }
          tickets.clear();
          timer_end = now + p.host_push_delay;
          stage = Stage::kPushDelay;
          moved = true;
          break;
        }

        case Stage::kPushDelay: {
          if (now < timer_end) break;
          DispatchItem const& item = plan[next_item];
          // 一个 user 的起跑只记一次，不管它进了几个注入源。
          recorder.GlobalStart(item.uid, now, item.expected_fragments);
          for (DispatchTarget const& t : item.targets) Offer(item, t, now);
          ++dispatched;
          ++next_item;
          stage = Stage::kIdle;
          moved = true;
          break;
        }
      }
    }
  }

  void Offer(DispatchItem const& item, DispatchTarget const& t, Time now) {
    Fifo<HostOffer>& port = t.host->OfferPort();
    LOGCHECK(!port.IsFull(), "MoeDispatcher: the host inject port overflowed.");
    CommInstPtr tmpl = std::make_shared<CommInst>();
    tmpl->hit_map = item.hit_map;
    tmpl->moe_bitmap = item.moe_bitmap;

    HostOffer offer(clk);
    offer.uid = item.uid;
    offer.tag = t.tag;
    offer.target = EncodeCoord(t.target);
    offer.layer_id = uint64_t{0};
    offer.lane_id = static_cast<uint64_t>(-1);
    offer.group_id = static_cast<uint64_t>(-1);
    offer.tmpl = tmpl;
    port.Push(offer);
    (void)now;
  }

  Params p;
  SpanRecorder recorder;
  NodeContext ctx;

  std::vector<DispatchItem> plan;
  std::vector<Ticket> tickets;

  size_t next_item = 0;
  uint64_t dispatched = 0;
  Time credit_start = 0;
  Time timer_end = 0;
  Stage stage = Stage::kIdle;
};

}
}

#endif
