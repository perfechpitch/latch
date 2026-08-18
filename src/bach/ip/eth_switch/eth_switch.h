#ifndef _LATCH_BACH_IP_ETH_SWITCH_ETH_SWITCH_
#define _LATCH_BACH_IP_ETH_SWITCH_ETH_SWITCH_

// 以太网交换节点。它连的不是核阵列，而是三段流水之间的那几条边：Phase1 算完的
// 残差与 MoE 请求从这里进去，MoE 请求交给 Phase2，Phase2 算完的结果再回到这里与
// 残差汇合。
//
// 它与 PCIe 交换节点是两种东西，不要拿一套看：
//
//   PCIe 那个   接的是核阵列，包按链路带宽切片，端口由 Map 起名，路由按目的坐标
//   这一个      接的是三段流水，一包整个串行不切片，端口是五个固定的名字，一条
//               入口只通向一条出口
//
// 五个口，三条路：
//
//   res_ingress            → phase3_res_join_egress    残差
//   phase1_moe_ingress     → phase2_moe_request_egress MoE 请求
//   phase2_moe_result_ingress → phase3_res_join_egress MoE 结果
//
// 一包走完三段时间：入口串行加线延迟、交换节点内的固定处理时间、出口串行加线延迟。
// 出口是独占的，一个口同时只发一包，后来的排队。排队深度分两级，两级都满了就停机：
// 队列满不挡上游是全模型一致的语义，深度不够是配置错误，不是静默丢包。
//
// 它还是 HitMap 语义换算的唯一地点。Phase1 交进来的 HitMap 装的是专家 id，交给
// Phase2 的那份装的是 EPGroup id，换算就在这一跳完成：一个专家落在哪个 group 由
// Map 给的对应表指名，没给对应表才按组大小整除。整除只是没有对应表时的兜底，热点
// 专家复制之后一个专家会落在多个 group 上，那种 Map 必须把对应表写出来。
//
// 三条边界照抄 Bach，不替它补：一个专家复制到多个 group 的场景没有实测数据；
// 共享的入口没有多个上游竞争的模型；上游收不到反压。

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
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

// ---------------------------------------------------------------- 端口

enum class EthPort : uint32_t {
  kResIngress = 0,
  kPhase1MoeIngress = 1,
  kPhase2MoeResultIngress = 2,
  kPhase2MoeRequestEgress = 3,
  kPhase3ResJoinEgress = 4,
};

constexpr uint32_t kEthPortNum = 5;

inline bool IsEthIngress(EthPort p) {
  return p == EthPort::kResIngress || p == EthPort::kPhase1MoeIngress ||
         p == EthPort::kPhase2MoeResultIngress;
}

inline const char* EthPortName(EthPort p) {
  switch (p) {
    case EthPort::kResIngress: return "res_ingress";
    case EthPort::kPhase1MoeIngress: return "phase1_moe_ingress";
    case EthPort::kPhase2MoeResultIngress: return "phase2_moe_result_ingress";
    case EthPort::kPhase2MoeRequestEgress: return "phase2_moe_request_egress";
    default: return "phase3_res_join_egress";
  }
}

// 包上那份 HitMap 装的是什么。换算只发生在 Phase1 的 MoE 请求那一条路上。
enum class HitMapSemantic : uint32_t {
  kNone = 0,
  kRoutedExpertId = 1,
  kGroupId = 2,
};

inline const char* HitMapSemanticName(HitMapSemantic s) {
  switch (s) {
    case HitMapSemantic::kNone: return "none";
    case HitMapSemantic::kRoutedExpertId: return "routed_expert_id";
    default: return "group_id";
  }
}

// 这一包在三段流水之间扮演什么。由入口决定，不从 opcode 猜。
enum class EthRole : uint32_t {
  kMoeRequest = 0,
  kResidual = 1,
  kMoeResult = 2,
};

inline const char* EthRoleName(EthRole r) {
  switch (r) {
    case EthRole::kMoeRequest: return "moe_request";
    case EthRole::kResidual: return "residual";
    default: return "moe_result";
  }
}

inline EthRole DefaultEthRole(EthPort ingress) {
  switch (ingress) {
    case EthPort::kPhase1MoeIngress: return EthRole::kMoeRequest;
    case EthPort::kResIngress: return EthRole::kResidual;
    default: return EthRole::kMoeResult;
  }
}

// ---------------------------------------------------------------- 身份

// Phase 侧认一包的身份：哪个 chip 组、哪条 lane、第几层、哪个 user。uid 单独不够，
// 同一个 uid 在不同层、不同 lane 上是不同的一次工作。
struct PhaseWorkKey {
  int64_t group_id = -1;
  int64_t lane_id = -1;
  uint64_t layer_id = 0;
  uint64_t uid = 0;

  bool operator==(PhaseWorkKey const& r) const {
    return group_id == r.group_id && lane_id == r.lane_id &&
           layer_id == r.layer_id && uid == r.uid;
  }
};

inline PhaseWorkKey MakePhaseWorkKey(CommInst const& c) {
  PhaseWorkKey k;
  k.group_id = c.phase1_group_id;
  k.lane_id = c.phase1_lane_id;
  k.layer_id = c.layer_id;
  k.uid = c.uid;
  return k;
}

struct PhaseWorkKeyHash {
  size_t operator()(PhaseWorkKey const& k) const {
    size_t h = std::hash<uint64_t>()(k.uid);
    auto mix = [&h](uint64_t v) {
      h ^= std::hash<uint64_t>()(v) + 0x9e37'79b9'7f4a'7c15ull + (h << 6) +
           (h >> 2);
    };
    mix(static_cast<uint64_t>(k.group_id));
    mix(static_cast<uint64_t>(k.lane_id));
    mix(k.layer_id);
    return h;
  }
};

// ---------------------------------------------------------------- 信封

// 出去的那一包，加上换算过程留下的痕迹。下游按它校验自己收到的东西对不对。
struct EthEnvelope {
  CommInstPtr packet;
  HitMapSemantic semantic = HitMapSemantic::kNone;
  std::vector<uint32_t> expert_hit_map;
  std::vector<uint32_t> group_hit_map;
  std::vector<std::pair<uint32_t, uint32_t>> group_multipliers;
};

// 一包走完全程的时间账。每一段都记下来，是为了在归因时分得清慢在哪一段。
struct EthDelivery {
  uint64_t event_id = 0;
  EthPort ingress = EthPort::kResIngress;
  EthPort egress = EthPort::kPhase3ResJoinEgress;
  EthRole role = EthRole::kResidual;
  uint64_t packet_bytes = 0;
  Time submitted = 0;
  uint64_t ingress_serialize = 0;
  Time switch_arrived = 0;
  Time switch_done = 0;
  Time tx_start = 0;
  uint64_t egress_serialize = 0;
  Time tx_end = 0;
  Time arrived = 0;
  bool queued = false;  // 出口忙，排过队
  EthEnvelope out;
};

// 收到一条投递之后要做什么。Phase2 的注入与 Phase3 的汇合各实现一个。
class EthDeliverySink {
 public:
  virtual ~EthDeliverySink() = default;
  virtual void HandleDelivery(EthDelivery const& d, Time now) = 0;
  // 全局完成权唯一。声明要独占的那个，不能与交换节点自己的 Phase1 边界完成并存。
  virtual bool ExclusiveCompletion() const { return false; }
  // 它判完了几次。拿完成权的那个才有数，其余恒为零。
  virtual uint64_t CompletionNum() const { return 0; }
  virtual const char* SinkName() const = 0;
};

// ---------------------------------------------------------------- 配置

struct EthPortConfig {
  uint64_t bandwidth = 0;  // Byte 每拍。一 GBps 就是一 Byte 每 ns，与全模型同一单位
  uint64_t propagation = 0;
  uint64_t queue_capacity = 64;
  uint64_t pending_capacity = 64;
};

struct EthSwitchConfig {
  EthPortConfig ports[kEthPortNum];
  // 入口通向哪个出口。kEthPortNum 表示这条入口没有配路由。
  uint32_t route[kEthPortNum] = {kEthPortNum, kEthPortNum, kEthPortNum,
                                 kEthPortNum, kEthPortNum};

  uint64_t processing_delay = 200;
  uint64_t header_bytes = 128;
  uint64_t payload_numel = 6144;
  uint64_t payload_bytes_per_elem = 2;

  uint32_t expert_group_size = 16;
  uint32_t expert_count = 0;  // 0 表示不限
  // 专家落在哪几个 group 上。空表示没给对应表，按组大小整除。
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> expert_group_map;

  uint64_t PacketBytes() const {
    return header_bytes + payload_numel * payload_bytes_per_elem;
  }

  void SetRoute(EthPort in, EthPort out) {
    route[static_cast<uint32_t>(in)] = static_cast<uint32_t>(out);
  }

  // Bach 的那套默认口与默认路由。带宽的取值是它的 default_phase2_compat。
  static EthSwitchConfig Default() {
    EthSwitchConfig c;
    c.ports[static_cast<uint32_t>(EthPort::kResIngress)].bandwidth = 60;
    c.ports[static_cast<uint32_t>(EthPort::kPhase1MoeIngress)].bandwidth = 30;
    c.ports[static_cast<uint32_t>(EthPort::kPhase2MoeResultIngress)].bandwidth =
        60;
    c.ports[static_cast<uint32_t>(EthPort::kPhase2MoeRequestEgress)].bandwidth =
        30;
    c.ports[static_cast<uint32_t>(EthPort::kPhase3ResJoinEgress)].bandwidth = 60;
    c.SetRoute(EthPort::kPhase1MoeIngress, EthPort::kPhase2MoeRequestEgress);
    c.SetRoute(EthPort::kResIngress, EthPort::kPhase3ResJoinEgress);
    c.SetRoute(EthPort::kPhase2MoeResultIngress,
               EthPort::kPhase3ResJoinEgress);
    return c;
  }

  void Validate() const {
    for (uint32_t i = 0; i < kEthPortNum; ++i) {
      LOGCHECK(ports[i].bandwidth > 0,
               "EthSwitchConfig: every port needs a positive bandwidth.");
      LOGCHECK(ports[i].queue_capacity > 0,
               "EthSwitchConfig: the egress queue capacity must be positive.");
    }
    LOGCHECK(PacketBytes() > 0,
             "EthSwitchConfig: the default packet length must be positive.");
    LOGCHECK(expert_group_size > 0,
             "EthSwitchConfig: expert_group_size must be positive.");
    bool any = false;
    for (uint32_t i = 0; i < kEthPortNum; ++i) {
      const EthPort in = static_cast<EthPort>(i);
      if (route[i] == kEthPortNum) continue;
      any = true;
      LOGCHECK(IsEthIngress(in),
               "EthSwitchConfig: a route starts at a port that is not an "
               "ingress.");
      LOGCHECK(!IsEthIngress(static_cast<EthPort>(route[i])),
               "EthSwitchConfig: a route ends at a port that is not an "
               "egress.");
    }
    LOGCHECK(any, "EthSwitchConfig: no route is configured.");
  }
};

// 专家 HitMap 换算成 group HitMap，同时数出每个 group 上命中几个专家。命中数就是
// 接收核的计算倍率，顺序按专家第一次点到那个 group 的先后，不排序。
inline void TranslateExpertHitMap(
    std::vector<uint32_t> const& experts, EthSwitchConfig const& cfg,
    std::vector<uint32_t>* groups,
    std::vector<std::pair<uint32_t, uint32_t>>* multipliers) {
  LOGCHECK(!experts.empty(),
           "EthSwitch: a phase1 MoE request needs a non-empty expert HitMap.");
  std::unordered_map<uint32_t, std::vector<uint32_t>> table;
  for (auto const& kv : cfg.expert_group_map) table[kv.first] = kv.second;

  groups->clear();
  multipliers->clear();
  std::unordered_map<uint32_t, uint32_t> hits;
  std::unordered_map<uint32_t, bool> seen_expert;
  for (uint32_t e : experts) {
    LOGCHECK(!seen_expert[e],
             "EthSwitch: the same expert appears twice in one HitMap.");
    seen_expert[e] = true;
    LOGCHECK(cfg.expert_count == 0 || e < cfg.expert_count,
             "EthSwitch: an expert id falls outside the declared count.");
    auto it = table.find(e);
    if (it == table.end()) {
      LOGCHECK(table.empty(),
               "EthSwitch: the expert-to-group table does not cover a routed "
               "expert.");
      const uint32_t g = e / cfg.expert_group_size;
      if (hits.find(g) == hits.end()) groups->push_back(g);
      ++hits[g];
      continue;
    }
    for (uint32_t g : it->second) {
      if (hits.find(g) == hits.end()) groups->push_back(g);
      ++hits[g];
    }
  }
  multipliers->reserve(groups->size());
  for (uint32_t g : *groups) multipliers->emplace_back(g, hits[g]);
}

// ---------------------------------------------------------------- 交进来的一包

// 上游往交换节点推的一包。Phase1 的 sink 与 Phase2 的 Out 各推各的。
class EthReq : public Logic {
 public:
  Logic64 ingress;   // EthPort
  Logic64 role;      // EthRole
  Logic64 semantic;  // 交进来时这份 HitMap 装的是什么
  Logic64 bytes;     // 0 表示按配置里那个默认包长
  LogicPtr<CommInst> payload;

  explicit EthReq(ClockPtr c)
      : ingress(c), role(c), semantic(c), bytes(c), payload(c) {
    Fields(ingress, role, semantic, bytes);
    Fields(payload);
  }
};

// ---------------------------------------------------------------- 节点

class EthSwitch : public ClkModule {
 public:
  EthSwitch(ClockPtr clock, Params const& params, uint64_t node,
            EthSwitchConfig const& config, std::string const& name,
            uint64_t parent)
      : ClkModule(clock),
        p(params),
        ctx(MakeContext(node, &p, &recorder)),
        cfg(config),
        done_count(clock) {
    cfg.Validate();
    RegisterId(name, parent);
  }

  SpanRecorder const& Recorder() const { return recorder; }
  SpanRecorder& MutableRecorder() { return recorder; }
  EthSwitchConfig const& Config() const { return cfg; }

  // 上游往这个口推包。一个口一个入口，满足单 producer 单 consumer。
  Fifo<EthReq>& OpenIngress(EthPort port) {
    const uint32_t idx = static_cast<uint32_t>(port);
    LOGCHECK(IsEthIngress(port), "EthSwitch: that port is not an ingress.");
    if (inbox[idx] == nullptr) {
      inbox[idx] = std::make_unique<Fifo<EthReq>>(p.link_fifo_depth, clk);
    }
    return *inbox[idx];
  }

  // Phase1 边界自己判完成：残差与 MoE 请求两条都投出去了，这次工作就算完。它与
  // Phase3 的汇合是同一件事的两种判法，只能开一个。
  void SetPhase1BoundaryCompletion(bool on) {
    LOGCHECK(!on || !HasExclusiveSink(),
             "EthSwitch: the phase1 boundary cannot complete a run while "
             "another sink already owns the global completion.");
    phase1_boundary = on;
  }

  bool Phase1BoundaryCompletion() const { return phase1_boundary; }

  void AttachSink(EthDeliverySink* sink) {
    LOGCHECK(sink != nullptr, "EthSwitch: a delivery sink is null.");
    LOGCHECK(!sink->ExclusiveCompletion() || !phase1_boundary,
             "EthSwitch: this sink owns the global completion, so the phase1 "
             "boundary completion must be off.");
    if (sink->ExclusiveCompletion()) {
      LOGCHECK(!HasExclusiveSink(),
               "EthSwitch: two sinks both claim the global completion.");
    }
    sinks.push_back(sink);
  }

  void Cycle() override {
    DelayCycle(1);
    const Time now = RT::Now();
    Admit(now);
    Schedule(now);
    Deliver(now);

    done_count = CompletedNum();
    TracePerCycle("pending", pending.size());
    TracePerCycle("in_flight", flight.size());
    TracePerCycle("delivered", delivered);
  }

  // ------------------------------------------------------------ 诊断

  // 这一层判完了几次工作。Phase1 边界与汇合点只有一个在生效，两个数不会同时非零。
  uint64_t CompletedNum() const {
    uint64_t total = boundary_done;
    for (EthDeliverySink const* s : sinks) total += s->CompletionNum();
    return total;
  }

  // 跨拍可读的那一份。别的协程读它拿到的是上一拍提交的值。
  Logic64 const& DoneCount() const { return done_count; }

  uint64_t DeliveredNum() const { return delivered; }
  uint64_t AdmittedNum() const { return admitted; }
  uint64_t PendingNum() const { return pending.size(); }
  uint64_t InFlightNum() const { return flight.size(); }
  // Phase1 边界判完的次数。开着 Phase3 汇合时它恒为零。
  uint64_t BoundaryCompletions() const { return boundary_done; }
  std::vector<EthDelivery> const& Deliveries() const { return log; }

  // Phase2 的结果回来时要核对它对应哪条请求。请求投出去时记下，结果回来时取走。
  struct ExpectedRequest {
    PhaseWorkKey key;
    std::vector<uint32_t> group_hit_map;
    std::vector<std::pair<uint32_t, uint32_t>> group_multipliers;
    uint64_t request_event = 0;
  };

  bool HasExpectedResult(PhaseWorkKey const& key) const {
    return expected.count(key) != 0;
  }

  // 取走这次工作对应的那条请求，同时核对结果带回来的 group 与当初发出去的一致。
  ExpectedRequest TakeExpectedResult(CommInst const& c,
                                     std::vector<uint32_t> const& source_groups) {
    const PhaseWorkKey key = MakePhaseWorkKey(c);
    auto it = expected.find(key);
    LOGCHECK(it != expected.end(),
             "EthSwitch: a phase2 result came back with no request on record.");
    ExpectedRequest req = it->second;
    LOGCHECK(source_groups == req.group_hit_map,
             "EthSwitch: the phase2 result carries a different group set than "
             "the request it answers.");
    expected.erase(it);
    return req;
  }

 private:
  struct Event {
    uint64_t event_id = 0;
    EthPort ingress = EthPort::kResIngress;
    EthPort egress = EthPort::kPhase3ResJoinEgress;
    EthRole role = EthRole::kResidual;
    uint64_t bytes = 0;
    Time submitted = 0;
    uint64_t ingress_serialize = 0;
    Time switch_arrived = 0;
    Time switch_done = 0;
    Time tx_start = 0;
    uint64_t egress_serialize = 0;
    Time tx_end = 0;
    Time arrived = 0;
    bool queued = false;
    EthEnvelope out;
  };

  static NodeContext MakeContext(uint64_t node, Params const* params,
                                 SpanRecorder* rec) {
    NodeContext c;
    c.node_id = node;
    c.params = params;
    c.rec = rec;
    return c;
  }

  bool HasExclusiveSink() const {
    for (EthDeliverySink const* s : sinks) {
      if (s->ExclusiveCompletion()) return true;
    }
    return false;
  }

  // ------------------------------------------------------------ 收包

  void Admit(Time now) {
    for (uint32_t i = 0; i < kEthPortNum; ++i) {
      if (inbox[i] == nullptr) continue;
      while (!inbox[i]->IsEmpty()) {
        EthReq& r = inbox[i]->Front();
        CommInstPtr payload = r.payload.Get();
        const EthPort in = static_cast<EthPort>(uint64_t(r.ingress));
        const EthRole role = static_cast<EthRole>(uint64_t(r.role));
        const HitMapSemantic sem =
            static_cast<HitMapSemantic>(uint64_t(r.semantic));
        const uint64_t bytes = uint64_t(r.bytes);
        inbox[i]->Pop();
        Enqueue(in, role, sem, bytes, payload, now);
      }
    }
  }

  void Enqueue(EthPort in, EthRole role, HitMapSemantic sem, uint64_t bytes,
               CommInstPtr const& payload, Time now) {
    LOGCHECK(payload != nullptr, "EthSwitch: null payload.");
    LOGCHECK(IsEthIngress(in), "EthSwitch: a packet entered a non-ingress port.");
    const uint32_t idx = static_cast<uint32_t>(in);
    const uint32_t out = cfg.route[idx];
    LOGCHECK(out != kEthPortNum,
             "EthSwitch: no route is configured for that ingress.");

    Event e;
    e.event_id = next_event++;
    e.ingress = in;
    e.egress = static_cast<EthPort>(out);
    e.role = role;
    e.bytes = bytes > 0 ? bytes : cfg.PacketBytes();
    e.submitted = now;
    e.out = BuildEnvelope(in, sem, payload);

    EthPortConfig const& port = cfg.ports[idx];
    e.ingress_serialize = CalcCycles(static_cast<int64_t>(e.bytes),
                                     port.bandwidth);
    e.switch_arrived = now + e.ingress_serialize + port.propagation;
    e.switch_done = e.switch_arrived + cfg.processing_delay;

    ++admitted;
    pending.push_back(std::move(e));
  }

  // 换算就在这一步。三条入口各有各的规矩：
  //
  //   Phase1 的 MoE 请求  交进来的是专家 id，换算成 group id 再出去
  //   Phase2 的结果       交进来时还带着它是哪几个 group 算出来的，核对完清掉
  //   残差                本来就不带 HitMap，带了就是上游写错了
  EthEnvelope BuildEnvelope(EthPort in, HitMapSemantic sem,
                            CommInstPtr const& payload) {
    EthEnvelope env;
    if (in == EthPort::kPhase1MoeIngress) {
      LOGCHECK(sem == HitMapSemantic::kRoutedExpertId,
               "EthSwitch: the phase1 MoE ingress takes an expert HitMap, not "
               "a group HitMap.");
      env.expert_hit_map = payload->hit_map;
      TranslateExpertHitMap(env.expert_hit_map, cfg, &env.group_hit_map,
                            &env.group_multipliers);
      CommInstPtr out = std::make_shared<CommInst>(*payload);
      out->hit_map = env.group_hit_map;
      out->moe_bitmap = env.group_multipliers;
      env.packet = out;
      env.semantic = HitMapSemantic::kGroupId;
      return env;
    }

    if (in == EthPort::kPhase2MoeResultIngress) {
      LOGCHECK(sem == HitMapSemantic::kGroupId,
               "EthSwitch: a phase2 result must say which groups computed it.");
      TakeExpectedResult(*payload, payload->hit_map);
      // 汇合点那一侧只认身份，不再看 HitMap，所以这里就把它清掉。
      CommInstPtr out = std::make_shared<CommInst>(*payload);
      out->tid = 0;
      out->opcode = Opcode::kMove;
      out->tag = 0;
      out->hit_map.clear();
      out->moe_bitmap.clear();
      out->beat_id = 0;
      out->total_fragments = 1;
      env.packet = out;
      env.semantic = HitMapSemantic::kNone;
      return env;
    }

    LOGCHECK(sem == HitMapSemantic::kNone,
             "EthSwitch: the residual ingress takes no HitMap.");
    LOGCHECK(payload->hit_map.empty(),
             "EthSwitch: a residual arrived carrying a HitMap.");
    env.packet = payload;
    env.semantic = HitMapSemantic::kNone;
    return env;
  }

  // ------------------------------------------------------------ 出口

  // 处理完的按先后进出口。同刻处理完的按事件号，这样一次 run 的顺序是确定的。
  void Schedule(Time now) {
    std::vector<size_t> ready;
    for (size_t i = 0; i < pending.size(); ++i) {
      if (pending[i].switch_done <= now) ready.push_back(i);
    }
    if (ready.empty()) return;
    std::sort(ready.begin(), ready.end(), [this](size_t a, size_t b) {
      if (pending[a].switch_done != pending[b].switch_done) {
        return pending[a].switch_done < pending[b].switch_done;
      }
      return pending[a].event_id < pending[b].event_id;
    });

    for (size_t i : ready) StartTx(&pending[i], now);

    std::vector<Event> keep;
    keep.reserve(pending.size() - ready.size());
    std::vector<bool> taken(pending.size(), false);
    for (size_t i : ready) taken[i] = true;
    for (size_t i = 0; i < pending.size(); ++i) {
      if (!taken[i]) keep.push_back(std::move(pending[i]));
    }
    pending.swap(keep);
  }

  void StartTx(Event* e, Time now) {
    const uint32_t idx = static_cast<uint32_t>(e->egress);
    EthPortConfig const& port = cfg.ports[idx];

    // 还没开始发的那些占着排队位。两级都满了就停机：这一层没有反压通道，深度不够
    // 是配置错误。
    uint64_t waiting = 0;
    for (Event const& f : flight) {
      if (f.egress == e->egress && f.tx_start > now) ++waiting;
    }
    LOGCHECK(waiting < port.queue_capacity + port.pending_capacity,
             "EthSwitch: both the egress queue and its overflow queue are "
             "full.");

    e->tx_start = std::max(e->switch_done, busy_until[idx]);
    e->queued = e->tx_start > e->switch_done;
    e->egress_serialize =
        CalcCycles(static_cast<int64_t>(e->bytes), port.bandwidth);
    e->tx_end = e->tx_start + e->egress_serialize;
    e->arrived = e->tx_end + port.propagation;
    busy_until[idx] = e->tx_end;

    if (e->queued) {
      ctx.Wait(Unit::kEth, e->out.packet->uid, e->out.packet->tid,
               e->switch_done, e->tx_start, WaitReason::kEthEgress);
    }
    ctx.Span(Unit::kEth, e->out.packet->uid, e->out.packet->tid, e->tx_start,
             e->tx_end, SpanState::kTransfer, e->bytes);
    flight.push_back(std::move(*e));
  }

  // ------------------------------------------------------------ 投递

  void Deliver(Time now) {
    std::vector<Event> keep;
    std::vector<Event> out;
    for (Event& e : flight) {
      if (e.arrived <= now) out.push_back(std::move(e));
      else keep.push_back(std::move(e));
    }
    flight.swap(keep);
    if (out.empty()) return;
    std::sort(out.begin(), out.end(), [](Event const& a, Event const& b) {
      if (a.arrived != b.arrived) return a.arrived < b.arrived;
      return a.event_id < b.event_id;
    });

    for (Event const& e : out) {
      EthDelivery d;
      d.event_id = e.event_id;
      d.ingress = e.ingress;
      d.egress = e.egress;
      d.role = e.role;
      d.packet_bytes = e.bytes;
      d.submitted = e.submitted;
      d.ingress_serialize = e.ingress_serialize;
      d.switch_arrived = e.switch_arrived;
      d.switch_done = e.switch_done;
      d.tx_start = e.tx_start;
      d.egress_serialize = e.egress_serialize;
      d.tx_end = e.tx_end;
      d.arrived = e.arrived;
      d.queued = e.queued;
      d.out = e.out;

      ++delivered;
      log.push_back(d);
      RecordRequest(d);
      for (EthDeliverySink* s : sinks) s->HandleDelivery(d, now);
      MaybeCompleteBoundary(d, now);
    }
  }

  void RecordRequest(EthDelivery const& d) {
    if (d.role != EthRole::kMoeRequest) return;
    if (d.egress != EthPort::kPhase2MoeRequestEgress) return;
    LOGCHECK(d.out.semantic == HitMapSemantic::kGroupId,
             "EthSwitch: a MoE request must leave carrying group ids.");
    const PhaseWorkKey key = MakePhaseWorkKey(*d.out.packet);
    LOGCHECK(expected.count(key) == 0,
             "EthSwitch: the same MoE request went out twice.");
    ExpectedRequest req;
    req.key = key;
    req.group_hit_map = d.out.group_hit_map;
    req.group_multipliers = d.out.group_multipliers;
    req.request_event = d.event_id;
    expected[key] = std::move(req);
  }

  // Phase1 边界的判法：残差与 MoE 请求都投出去了就算这次工作完成。它不等 Phase2
  // 真的算完，所以只在没有 Phase3 汇合的时候用。
  void MaybeCompleteBoundary(EthDelivery const& d, Time now) {
    if (!phase1_boundary) return;
    if (d.role != EthRole::kResidual && d.role != EthRole::kMoeRequest) return;
    const PhaseWorkKey key = MakePhaseWorkKey(*d.out.packet);
    Seen& s = boundary[key];
    if (d.role == EthRole::kResidual) s.residual = true;
    else s.request = true;
    if (!s.residual || !s.request || s.done) return;
    s.done = true;
    ++boundary_done;
    recorder.GlobalEnd(d.out.packet->uid, now);
  }

  struct Seen {
    bool residual = false;
    bool request = false;
    bool done = false;
  };

  Params p;
  SpanRecorder recorder;
  NodeContext ctx;
  EthSwitchConfig cfg;

  std::unique_ptr<Fifo<EthReq>> inbox[kEthPortNum];
  Time busy_until[kEthPortNum] = {0, 0, 0, 0, 0};

  std::vector<Event> pending;  // 还在交换节点里处理
  std::vector<Event> flight;   // 已经开始发，还没到对面
  std::vector<EthDelivery> log;
  std::vector<EthDeliverySink*> sinks;

  std::unordered_map<PhaseWorkKey, ExpectedRequest, PhaseWorkKeyHash> expected;
  std::unordered_map<PhaseWorkKey, Seen, PhaseWorkKeyHash> boundary;

  Logic64 done_count;
  bool phase1_boundary = true;
  uint64_t next_event = 0;
  uint64_t admitted = 0;
  uint64_t delivered = 0;
  uint64_t boundary_done = 0;
};

}
}

#endif
