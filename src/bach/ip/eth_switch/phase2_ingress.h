#ifndef _LATCH_BACH_IP_ETH_SWITCH_PHASE2_INGRESS_
#define _LATCH_BACH_IP_ETH_SWITCH_PHASE2_INGRESS_

// 交换节点与 Phase2 之间那道口子。
//
// 交换节点把专家 id 换算成了 EPGroup id，这里是唯一一个把换算结果交还给硬件的地方，
// 所以校验也集中在这里：包上那份 HitMap 必须与信封上的 group 一份不差，每个 group
// 的命中数必须恰好覆盖这些 group，命中数之和必须等于原来那些专家的个数。少一条，
// 换算错了也照跑，错处会一路飘到某个核的计算倍率上才发作。
//
// 校验过了就按 group 分给各个 Host：哪个 group 归哪个 Host 由 Map 的绑定表指名，
// 同一个 Host 收到的几个 group 合成一个包，包上的 tag 是这几个 group 的命中数之和。
// 注入哪个核也由 Map 指名，一个 Host 只能有一个注入目标。
//
// 注入不扣 Host 的额度、不等推包间隔：这次工作的起点在 Phase1，Host 在这条路上是
// 中转的一段，不是发起方。

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/fifo.h"
#include "base/log.h"
#include "base/time_stamp.h"
#include "bach/common/packet.h"
#include "bach/ip/eth_switch/eth_switch.h"
#include "bach/ip/external/host.h"

namespace latch {
namespace bach {

// 一个 Host 收到的那一份：给它哪几个 group、推到哪个核去。
struct Phase2Injection {
  std::string host_name;
  Coord target;
  std::vector<uint32_t> groups;
  std::vector<std::pair<uint32_t, uint32_t>> multipliers;
  uint64_t tag = 0;
  uint64_t uid = 0;
  uint64_t event_id = 0;
};

class Phase2EthIngress : public EthDeliverySink {
 public:
  explicit Phase2EthIngress(ClockPtr clock) : clk(clock) {
    LOGCHECK(clk != nullptr, "Phase2EthIngress: clock is null.");
  }

  // 一个 Host 的接入：它认哪几个 group、包推到哪个核、往哪个口推。
  void BindHost(std::string const& host_name, Coord target,
                Fifo<HostOffer>* port) {
    LOGCHECK(port != nullptr, "Phase2EthIngress: the host inject port is null.");
    auto it = hosts.find(host_name);
    if (it != hosts.end()) {
      LOGCHECK(it->second.target == target && it->second.port == port,
               "Phase2EthIngress: this host is bound twice with different "
               "targets.");
      return;
    }
    HostBinding b;
    b.name = host_name;
    b.target = target;
    b.port = port;
    hosts[host_name] = b;
  }

  void BindGroup(uint32_t group_id, std::string const& host_name) {
    LOGCHECK(hosts.count(host_name) != 0,
             "Phase2EthIngress: a group is bound to a host that is not here.");
    auto it = group_to_host.find(group_id);
    if (it != group_to_host.end()) {
      LOGCHECK(it->second == host_name,
               "Phase2EthIngress: one group is bound to two hosts.");
      return;
    }
    group_to_host[group_id] = host_name;
  }

  void Validate() const {
    LOGCHECK(!hosts.empty(),
             "Phase2EthIngress: no host is available to take the injection.");
    LOGCHECK(!group_to_host.empty(),
             "Phase2EthIngress: no group is bound to a host.");
  }

  const char* SinkName() const override { return "phase2_eth_ingress"; }

  void HandleDelivery(EthDelivery const& d, Time now) override {
    if (d.egress != EthPort::kPhase2MoeRequestEgress) return;
    LOGCHECK(d.role == EthRole::kMoeRequest,
             "Phase2EthIngress: the phase2 egress carries something that is "
             "not a MoE request.");
    Check(d);

    // 同一个 Host 的几个 group 合成一份，顺序照信封上的顺序。
    std::vector<std::string> order;
    std::unordered_map<std::string, Phase2Injection> batch;
    for (size_t i = 0; i < d.out.group_hit_map.size(); ++i) {
      const uint32_t g = d.out.group_hit_map[i];
      auto bind = group_to_host.find(g);
      LOGCHECK(bind != group_to_host.end(),
               "Phase2EthIngress: this group has no host to go to.");
      auto host = hosts.find(bind->second);
      LOGCHECK(host != hosts.end(), "Phase2EthIngress: no such host.");
      if (batch.find(bind->second) == batch.end()) {
        order.push_back(bind->second);
        Phase2Injection item;
        item.host_name = bind->second;
        item.target = host->second.target;
        item.uid = d.out.packet->uid;
        item.event_id = d.event_id;
        batch[bind->second] = std::move(item);
      }
      Phase2Injection& item = batch[bind->second];
      item.groups.push_back(g);
      item.multipliers.push_back(d.out.group_multipliers[i]);
      item.tag += d.out.group_multipliers[i].second;
    }

    for (std::string const& name : order) {
      Phase2Injection const& item = batch[name];
      Push(item, *d.out.packet, now);
      log.push_back(item);
    }
  }

  std::vector<Phase2Injection> const& Injections() const { return log; }
  uint64_t InjectedNum() const { return log.size(); }

 private:
  struct HostBinding {
    std::string name;
    Coord target;
    Fifo<HostOffer>* port = nullptr;
  };

  // 信封该有的样子。这五条都对不上，说明换算那一步坏了。
  void Check(EthDelivery const& d) const {
    LOGCHECK(d.out.semantic == HitMapSemantic::kGroupId,
             "Phase2EthIngress: the phase2 side only takes group ids.");
    LOGCHECK(d.out.packet->hit_map == d.out.group_hit_map,
             "Phase2EthIngress: the packet HitMap and the envelope disagree.");
    LOGCHECK(!d.out.group_hit_map.empty(),
             "Phase2EthIngress: the group HitMap is empty.");
    LOGCHECK(d.out.group_multipliers.size() == d.out.group_hit_map.size(),
             "Phase2EthIngress: the hit counts do not cover the group HitMap.");
    uint64_t total = 0;
    for (size_t i = 0; i < d.out.group_multipliers.size(); ++i) {
      LOGCHECK(d.out.group_multipliers[i].first == d.out.group_hit_map[i],
               "Phase2EthIngress: the hit counts are in a different order than "
               "the group HitMap.");
      LOGCHECK(d.out.group_multipliers[i].second > 0,
               "Phase2EthIngress: a group is listed with no expert on it.");
      total += d.out.group_multipliers[i].second;
    }
    LOGCHECK(d.out.expert_hit_map.empty() ||
                 total == d.out.expert_hit_map.size(),
             "Phase2EthIngress: the hit counts do not add up to the number of "
             "routed experts.");
  }

  void Push(Phase2Injection const& item, CommInst const& src, Time now) {
    auto host = hosts.find(item.host_name);
    LOGCHECK(host != hosts.end(), "Phase2EthIngress: no such host.");
    Fifo<HostOffer>* port = host->second.port;
    LOGCHECK(!port->IsFull(),
             "Phase2EthIngress: the host inject port overflowed.");

    CommInstPtr tmpl = std::make_shared<CommInst>();
    tmpl->hit_map = item.groups;
    tmpl->moe_bitmap = item.multipliers;

    HostOffer offer(clk);
    offer.uid = item.uid;
    offer.tag = item.tag;
    offer.target = EncodeCoord(item.target);
    offer.layer_id = src.layer_id;
    offer.lane_id = static_cast<uint64_t>(src.phase1_lane_id);
    offer.group_id = static_cast<uint64_t>(src.phase1_group_id);
    offer.tmpl = tmpl;
    port->Push(offer);
    (void)now;
  }

  ClockPtr clk;
  std::unordered_map<std::string, HostBinding> hosts;
  std::unordered_map<uint32_t, std::string> group_to_host;
  std::vector<Phase2Injection> log;
};

}
}

#endif
