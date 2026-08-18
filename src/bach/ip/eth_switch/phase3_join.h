#ifndef _LATCH_BACH_IP_ETH_SWITCH_PHASE3_JOIN_
#define _LATCH_BACH_IP_ETH_SWITCH_PHASE3_JOIN_

// Phase3 的汇合点。一次工作要等两路都到齐：绕过 MoE 的那份残差，和 MoE 算完回来
// 的那份结果。两路谁先到都行，先到的那一路等着，后到的那一路把这次工作判完。
//
// 认的是四样合起来的身份：哪个 chip 组、哪条 lane、第几层、哪个 user。同一个 user
// 的同一层上，两路报的身份必须一致，不一致就是上游把身份丢了或者串了。
//
// 三条不能放宽：同一路不能到两次，同一次工作不能判完两次，一个 user 不能在别处已经
// 判过完成。开着这个汇合点的时候，全局完成权只在这里，交换节点自己那个 Phase1 边界
// 的判法必须关掉。少了这三条，一次工作会被记成两次完成，端到端时间就没有意义了。

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/log.h"
#include "base/time_stamp.h"
#include "bach/ip/eth_switch/eth_switch.h"
#include "bach/observer/span_recorder.h"

namespace latch {
namespace bach {

// 汇合成功的一次。两路各自的到达时刻都留着，慢的那一路决定这次工作什么时候完。
struct PhaseJoin {
  PhaseWorkKey key;
  Time residual_at = 0;
  Time result_at = 0;
  Time joined_at = 0;
  uint64_t residual_event = 0;
  uint64_t result_event = 0;
};

class Phase3Join : public EthDeliverySink {
 public:
  explicit Phase3Join(SpanRecorder* rec) : recorder(rec) {
    LOGCHECK(recorder != nullptr, "Phase3Join: the span recorder is null.");
  }

  bool ExclusiveCompletion() const override { return true; }
  uint64_t CompletionNum() const override { return joins.size(); }
  const char* SinkName() const override { return "phase3_res_join"; }

  void HandleDelivery(EthDelivery const& d, Time now) override {
    if (d.egress != EthPort::kPhase3ResJoinEgress) return;
    LOGCHECK(d.role == EthRole::kResidual || d.role == EthRole::kMoeResult,
             "Phase3Join: the join point received something that is neither a "
             "residual nor a MoE result.");
    Check(d);

    const PhaseWorkKey key = MakePhaseWorkKey(*d.out.packet);
    CheckIdentity(key);

    Half& h = halves[key];
    if (d.role == EthRole::kResidual) {
      LOGCHECK(!h.has_residual, "Phase3Join: the residual arrived twice.");
      h.has_residual = true;
      h.residual_at = d.arrived;
      h.residual_event = d.event_id;
    } else {
      LOGCHECK(!h.has_result, "Phase3Join: the MoE result arrived twice.");
      h.has_result = true;
      h.result_at = d.arrived;
      h.result_event = d.event_id;
    }
    if (!h.has_residual || !h.has_result) return;

    LOGCHECK(done.count(d.out.packet->uid) == 0,
             "Phase3Join: this user already has a completion elsewhere.");
    done.insert(d.out.packet->uid);

    PhaseJoin j;
    j.key = key;
    j.residual_at = h.residual_at;
    j.result_at = h.result_at;
    j.joined_at = h.residual_at > h.result_at ? h.residual_at : h.result_at;
    j.residual_event = h.residual_event;
    j.result_event = h.result_event;

    // 先到的那一路等了多久，记在等待归因里。
    const Time early = h.residual_at < h.result_at ? h.residual_at : h.result_at;
    recorder->Wait(node_id, Unit::kEth, key.uid, 0, early, j.joined_at,
                   WaitReason::kPhaseJoin);
    recorder->GlobalEnd(key.uid, now);
    joins.push_back(j);
    halves.erase(key);
  }

  void SetNodeId(uint64_t id) { node_id = id; }

  std::vector<PhaseJoin> const& Joins() const { return joins; }
  uint64_t JoinedNum() const { return joins.size(); }
  // 只到了一路、还在等另一路的次数。
  uint64_t PendingNum() const { return halves.size(); }

 private:
  struct Half {
    bool has_residual = false;
    bool has_result = false;
    Time residual_at = 0;
    Time result_at = 0;
    uint64_t residual_event = 0;
    uint64_t result_event = 0;
  };

  // 到这里的两路都不该再带 HitMap，Phase 身份则一样都不能少：少了它就分不出同一个
  // user 在哪一层、哪条 lane 上的这次工作。
  void Check(EthDelivery const& d) const {
    LOGCHECK(d.out.semantic == HitMapSemantic::kNone,
             "Phase3Join: neither side of the join carries a HitMap.");
    LOGCHECK(d.out.packet->hit_map.empty(),
             "Phase3Join: a packet arrived at the join still carrying a "
             "HitMap.");
    LOGCHECK(d.out.packet->phase1_lane_id >= 0,
             "Phase3Join: a packet arrived without its lane id.");
    LOGCHECK(d.out.packet->phase1_group_id >= 0,
             "Phase3Join: a packet arrived without its chip group id.");
    if (d.role == EthRole::kResidual) {
      LOGCHECK(d.ingress == EthPort::kResIngress,
               "Phase3Join: the residual must come in through the residual "
               "ingress.");
    } else {
      LOGCHECK(d.ingress == EthPort::kPhase2MoeResultIngress,
               "Phase3Join: the MoE result must come in through the phase2 "
               "result ingress.");
    }
  }

  // 同一个 user 的同一层，两路报的 lane 与 chip 组必须是同一个。
  void CheckIdentity(PhaseWorkKey const& key) {
    const uint64_t k = (key.uid << 20) ^ key.layer_id;
    auto it = seen.find(k);
    if (it == seen.end()) {
      seen[k] = key;
      return;
    }
    LOGCHECK(it->second == key,
             "Phase3Join: the two sides of one join disagree on which lane or "
             "chip group this work belongs to.");
  }

  SpanRecorder* recorder;
  uint64_t node_id = 0;
  std::unordered_map<PhaseWorkKey, Half, PhaseWorkKeyHash> halves;
  std::unordered_map<uint64_t, PhaseWorkKey> seen;
  std::unordered_set<uint64_t> done;
  std::vector<PhaseJoin> joins;
};

}
}

#endif
