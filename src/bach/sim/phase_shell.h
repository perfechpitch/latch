#ifndef _LATCH_BACH_SIM_PHASE_SHELL_
#define _LATCH_BACH_SIM_PHASE_SHELL_

// Phase 之间的边界声明与完成账本。
//
// 它是一层软件外壳，不是硬件：这里的端口不对应任何引脚或路由端口，这里的边也不扣
// 传输时间。一次工作在物理上走了多久，由 Phase1 那条通道、以太网交换节点和核阵列
// 各自扣，这一层再扣一遍就是重复计时。
//
// 它管两件事：
//
//   装配期  谁能接谁。一条边的起点必须是输出口、终点必须是输入口，两端都得认这条
//           边上跑的是什么；一个输入口默认只许一个上游，要多个就得显式声明这里有
//           仲裁；一个 Phase 声明自己必须收到的那几样，必须真有上游能产出来
//   运行期  一次工作按进来的顺序编号，完成的顺序可以乱，交出去的顺序不能乱
//
// 第二件事的用处：一次工作在 Phase 里会分岔成几路，几路各走各的，完成先后与进来的
// 先后无关。乱序完成先攒着，攒到前面那些都完成了再一起放出去，交出去的仍是原顺序。
//
// 这一层与当前那条跑通的路径是分开的：跑一次 run 不需要它。它在的意义是把边界写成
// 装配期查得出来的东西，而不是等数据流起来之后表现成丢包或者死等。

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/log.h"
#include "base/time_stamp.h"

namespace latch {
namespace bach {

// ---------------------------------------------------------------- 角色

// 一条边上跑的是什么。它是 Phase 之间的依赖关系，不是硬件的 opcode：同一个 opcode
// 在不同的边上可以是不同的角色，反过来也一样，所以两者不能互推。
enum class PayloadRole : uint32_t {
  kAttentionOutput = 0,
  kMoeRequest = 1,
  kResidualBypass = 2,
  kMoeResult = 3,
  kAttentionInput = 4,
};

constexpr uint32_t kPayloadRoleNum = 5;

inline const char* PayloadRoleName(PayloadRole r) {
  switch (r) {
    case PayloadRole::kAttentionOutput: return "attention_output";
    case PayloadRole::kMoeRequest: return "moe_request";
    case PayloadRole::kResidualBypass: return "residual_bypass";
    case PayloadRole::kMoeResult: return "moe_result";
    default: return "attention_input";
  }
}

inline uint32_t RoleBit(PayloadRole r) {
  return 1u << static_cast<uint32_t>(r);
}

inline uint32_t RoleSet(std::vector<PayloadRole> const& roles) {
  uint32_t mask = 0;
  for (PayloadRole r : roles) mask |= RoleBit(r);
  return mask;
}

// ---------------------------------------------------------------- 端口

enum class PhasePortDir : uint32_t {
  kInput = 0,
  kOutput = 1,
};

struct PhasePort {
  std::string phase_id;
  std::string port_id;
  PhasePortDir dir = PhasePortDir::kInput;
  uint32_t roles = 0;
  // 默认一个输入口只许一个上游。要多个就得说明这里确实有仲裁，不能默许。
  bool allow_multiple_upstreams = false;
};

struct PhaseDef {
  std::string phase_id;
  std::vector<PhasePort> ports;
  uint32_t required_input_roles = 0;
};

// 一条有向边。它只说明谁接谁、这条边上跑什么，不带时间。
struct PhaseEdge {
  std::string edge_id;
  std::string src_phase;
  std::string src_port;
  std::string dst_phase;
  std::string dst_port;
  uint32_t roles = 0;
};

// ---------------------------------------------------------------- 拓扑

class PhaseTopology {
 public:
  void AddPhase(PhaseDef def) {
    LOGCHECK(!def.phase_id.empty(), "PhaseTopology: a phase has no id.");
    LOGCHECK(index.count(def.phase_id) == 0,
             "PhaseTopology: two phases share one id.");
    std::unordered_set<std::string> names;
    uint32_t accepted = 0;
    for (PhasePort const& port : def.ports) {
      LOGCHECK(port.phase_id == def.phase_id,
               "PhaseTopology: a port is declared on another phase.");
      LOGCHECK(!port.port_id.empty(), "PhaseTopology: a port has no id.");
      LOGCHECK(port.roles != 0,
               "PhaseTopology: a port that accepts nothing carries nothing.");
      LOGCHECK(names.insert(port.port_id).second,
               "PhaseTopology: two ports on one phase share one id.");
      if (port.dir == PhasePortDir::kInput) accepted |= port.roles;
    }
    LOGCHECK((def.required_input_roles & ~accepted) == 0,
             "PhaseTopology: a phase requires something none of its input "
             "ports accepts.");
    index[def.phase_id] = phases.size();
    phases.push_back(std::move(def));
  }

  void AddEdge(PhaseEdge edge) {
    LOGCHECK(!edge.edge_id.empty(), "PhaseTopology: an edge has no id.");
    LOGCHECK(edge_ids.insert(edge.edge_id).second,
             "PhaseTopology: two edges share one id.");
    LOGCHECK(edge.roles != 0,
             "PhaseTopology: an edge that carries nothing is not an edge.");

    PhasePort const& src = PortOf(edge.src_phase, edge.src_port);
    PhasePort const& dst = PortOf(edge.dst_phase, edge.dst_port);
    LOGCHECK(src.dir == PhasePortDir::kOutput,
             "PhaseTopology: an edge starts at a port that does not send.");
    LOGCHECK(dst.dir == PhasePortDir::kInput,
             "PhaseTopology: an edge ends at a port that does not receive.");
    LOGCHECK((edge.roles & ~src.roles) == 0,
             "PhaseTopology: the sending port does not carry everything this "
             "edge carries.");
    LOGCHECK((edge.roles & ~dst.roles) == 0,
             "PhaseTopology: the receiving port does not take everything this "
             "edge carries.");

    const std::string pair = edge.src_phase + "." + edge.src_port + ">" +
                             edge.dst_phase + "." + edge.dst_port;
    LOGCHECK(wired.insert(pair).second,
             "PhaseTopology: these two ports are already wired together.");
    edges.push_back(std::move(edge));
  }

  // 装配期查完的两件事：一个输入口有没有偷偷接了几个上游，一个 Phase 必须收到的
  // 那几样有没有人产。
  void Validate() const {
    std::unordered_map<std::string, uint32_t> drivers;
    std::unordered_map<std::string, uint32_t> supplied;
    for (PhaseEdge const& e : edges) {
      const std::string key = e.dst_phase + "." + e.dst_port;
      ++drivers[key];
      supplied[key] |= e.roles;
    }
    for (auto const& kv : drivers) {
      if (kv.second <= 1) continue;
      const size_t dot = kv.first.rfind('.');
      PhasePort const& port =
          PortOf(kv.first.substr(0, dot), kv.first.substr(dot + 1));
      LOGCHECK(port.allow_multiple_upstreams,
               "PhaseTopology: an input port has several upstreams without "
               "saying who arbitrates between them.");
    }
    for (PhaseDef const& def : phases) {
      uint32_t produced = 0;
      for (PhasePort const& port : def.ports) {
        if (port.dir != PhasePortDir::kInput) continue;
        auto it = supplied.find(def.phase_id + "." + port.port_id);
        if (it != supplied.end()) produced |= it->second;
      }
      LOGCHECK((def.required_input_roles & ~produced) == 0,
               "PhaseTopology: nobody produces something a phase requires.");
    }
  }

  uint64_t PhaseNum() const { return phases.size(); }
  uint64_t EdgeNum() const { return edges.size(); }
  std::vector<PhaseDef> const& Phases() const { return phases; }
  std::vector<PhaseEdge> const& Edges() const { return edges; }

  PhasePort const& PortOf(std::string const& phase_id,
                          std::string const& port_id) const {
    auto it = index.find(phase_id);
    LOGCHECK(it != index.end(), "PhaseTopology: no such phase.");
    for (PhasePort const& port : phases[it->second].ports) {
      if (port.port_id == port_id) return port;
    }
    LOGCHECK(false, "PhaseTopology: no such port on that phase.");
    return phases[it->second].ports.front();
  }

 private:
  std::vector<PhaseDef> phases;
  std::vector<PhaseEdge> edges;
  std::unordered_map<std::string, size_t> index;
  std::unordered_set<std::string> edge_ids;
  std::unordered_set<std::string> wired;
};

// ---------------------------------------------------------------- 完成账本

// 一次工作的软件身份。uid 说明这是哪个 user，序号说明它是第几个进来的：一次工作会
// 分岔成几路乱序完成，光看 uid 认不出原来的先后。
struct PhaseWork {
  uint64_t seq = 0;
  uint64_t uid = 0;
};

struct PhaseDone {
  PhaseWork work;
  Time at = 0;
};

class PhaseOrchestrator {
 public:
  // 一次工作进来，按进来的先后编号。
  PhaseWork RegisterIngress(uint64_t uid) {
    PhaseWork w;
    w.seq = next_seq++;
    w.uid = uid;
    LOGCHECK(live.insert(w.seq).second,
             "PhaseOrchestrator: this work is registered twice.");
    return w;
  }

  // 某一次工作完成了。完成的先后可以与进来的先后不同，先攒着。
  void Complete(PhaseWork const& work, Time at) {
    LOGCHECK(live.count(work.seq) != 0,
             "PhaseOrchestrator: a completion arrived for work that never "
             "entered.");
    LOGCHECK(work.seq >= next_release && held.count(work.seq) == 0,
             "PhaseOrchestrator: this work completed twice.");
    PhaseDone d;
    d.work = work;
    d.at = at;
    held[work.seq] = d;
    Release();
  }

  // 前面那些都完成了才放出去，放出去的顺序就是进来的顺序。
  void Release() {
    while (true) {
      auto it = held.find(next_release);
      if (it == held.end()) break;
      released.push_back(it->second);
      held.erase(it);
      ++next_release;
    }
  }

  bool AllReleased() const {
    return next_seq > 0 && released.size() == next_seq;
  }

  uint64_t IngressNum() const { return next_seq; }
  uint64_t ReleasedNum() const { return released.size(); }
  // 完成了但前面还有没完成的，攒在这里等。
  uint64_t HeldNum() const { return held.size(); }
  std::vector<PhaseDone> const& Released() const { return released; }

  std::vector<uint64_t> CompletedUids() const {
    std::vector<uint64_t> out;
    out.reserve(released.size());
    for (PhaseDone const& d : released) out.push_back(d.work.uid);
    return out;
  }

 private:
  uint64_t next_seq = 0;
  uint64_t next_release = 0;
  std::unordered_set<uint64_t> live;
  std::unordered_map<uint64_t, PhaseDone> held;
  std::vector<PhaseDone> released;
};

}
}

#endif
