#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_DSA_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_DSA_

// DTE 的五路径仲裁。默认关闭，开启后取代执行通道那一段的先进先出排队。
//
// 一次搬运的两端各是三处之一：Router、MatrixMem、Core 本体。由此得到五条路径，
// 每条占用的物理通道由两位掩码给出：
//
//   R2M  Router 到 MatrixMem     占 IN
//   R2C  Router 到 Core          占 IN
//   M2R  MatrixMem 到 Router     占 OUT
//   C2R  Core 到 Router          占 OUT
//   M2C  MatrixMem 到 Core       占 IN 与 OUT，因为它两头都在本地
//
// 掩码相交的两条不能同时占用，不相交的可以。R2M 与 M2R 是一进一出，同时跑得起来；
// M2C 与任何一条都相交，它一开跑，整个 DTE 的搬运就停了。
//
// 每条路径一条先进先出队列，另加三条兜底队列：任务表没写路径时按方向排（进方向一条、
// 出方向一条），本核这次不激活、只把包往下游转的那类排在控制转发这条。
//
// 授予规则是最老且不冲突者优先：候选只能是各队列的队首，掩码不与正在占用的相交，
// 并且没有比它更老的等待者与它相交。少了后面这一条，一条老的双向路径会被源源不断的
// 单向路径饿死。
//
// 逻辑仲裁与物理占用是同一件事的两面：仲裁放行的那一刻同时占住对应的一条或两条物理
// 通道，两者的时刻必须一致。物理通道在放行时刻不空闲，说明有人绕过仲裁器占了它，当场
// 停机而不是接着跑。
//
// 不搬数据的那类任务不进这里：本地的残差求和、末端核不回额度的退休，它们没有两端，
// 占一条路径反而会挡住真正的搬运。

#include <algorithm>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/log.h"
#include "base/time_stamp.h"
#include "bach/common/arbiter.h"
#include "bach/common/packet.h"
#include "bach/tables/task_meta.h"

namespace latch {
namespace bach {

// ---------------------------------------------------------------- 掩码

constexpr uint32_t kDsaInMask = 0x1;
constexpr uint32_t kDsaOutMask = 0x2;
constexpr uint32_t kDsaBothMask = kDsaInMask | kDsaOutMask;

constexpr uint32_t kDsaRouteNum = 5;
// 五条路径两两组合，共十个交点。
constexpr uint32_t kDsaCrossNum = kDsaRouteNum * (kDsaRouteNum - 1) / 2;

inline uint32_t DsaRouteMask(DsaRoute r) {
  switch (r) {
    case DsaRoute::kR2M:
    case DsaRoute::kR2C: return kDsaInMask;
    case DsaRoute::kM2R:
    case DsaRoute::kC2R: return kDsaOutMask;
    case DsaRoute::kM2C: return kDsaBothMask;
    default: return 0;
  }
}

// 五条路径在统计数组里的下标。kNone 不占位。
inline uint32_t DsaRouteIndex(DsaRoute r) {
  LOGCHECK(r != DsaRoute::kNone, "DsaRouteIndex: no index for an unset route.");
  return static_cast<uint32_t>(r) - 1;
}

inline DsaRoute DsaRouteAt(uint32_t index) {
  LOGCHECK(index < kDsaRouteNum, "DsaRouteAt: index out of range.");
  return static_cast<DsaRoute>(index + 1);
}

inline const char* DsaRouteName(DsaRoute r) {
  switch (r) {
    case DsaRoute::kR2M: return "R2M";
    case DsaRoute::kR2C: return "R2C";
    case DsaRoute::kM2R: return "M2R";
    case DsaRoute::kC2R: return "C2R";
    case DsaRoute::kM2C: return "M2C";
    default: return "NONE";
  }
}

// ---------------------------------------------------------------- 队列与归类

// 一次请求排在哪条队列上。前五条是精确路径，后三条是任务表没写路径时的去处。
enum class DsaQueue : uint32_t {
  kR2M = 0,
  kR2C = 1,
  kM2R = 2,
  kC2R = 3,
  kM2C = 4,
  kDirectionIn = 5,     // 只知道是进方向
  kDirectionOut = 6,    // 只知道是出方向
  kControlForward = 7,  // 本核这次不激活，只把包往下游转
  kNonTransfer = 8,     // 不搬数据，不进仲裁器
};

constexpr uint32_t kDsaQueueNum = 9;

// 这条请求算哪一类占用。两个方向组各含两条精确路径，本地那一组只有 M2C。
enum class DsaGroup : uint32_t {
  kToCore = 0,        // 进方向，Router 到 MatrixMem 或到 Core
  kToRouter = 1,      // 出方向，MatrixMem 或 Core 到 Router
  kLocal = 2,         // 本地搬运，即 M2C
  kControlForward = 3,
  kNonTransfer = 4,
};

// 这条路径是从哪里定下来的。
enum class DsaSource : uint32_t {
  kExplicit = 0,           // 任务元数据里写了
  kDirectionFallback = 1,  // 没写，只按进出方向归类
  kInternal = 2,           // 由运行时的分支定，不是任务表给的
};

// 不搬数据的两种情形。
enum class DsaNonTransfer : uint32_t {
  kNone = 0,
  kLocalResSum = 1,  // 本地残差求和
  kLocalRetire = 2,  // 末端核的退休，不往上游回额度
};

// 抢执行通道之前定下来的那份决定：排哪条队、占哪几条物理通道、算不算得进路径分析。
struct DsaPlan {
  DsaQueue queue = DsaQueue::kNonTransfer;
  DsaGroup group = DsaGroup::kNonTransfer;
  uint32_t mask = 0;
  DsaRoute route = DsaRoute::kNone;
  DsaSource source = DsaSource::kInternal;
  bool analysis_eligible = false;
  DsaNonTransfer non_transfer = DsaNonTransfer::kNone;

  bool IsNonTransfer() const { return mask == 0; }
  bool IsLocalMove() const { return route == DsaRoute::kM2C; }
};

// ---------------------------------------------------------------- 解析

namespace detail {

inline DsaPlan NonTransferPlan(DsaNonTransfer reason) {
  DsaPlan p;
  p.queue = DsaQueue::kNonTransfer;
  p.group = DsaGroup::kNonTransfer;
  p.mask = 0;
  p.route = DsaRoute::kNone;
  p.source = DsaSource::kInternal;
  p.analysis_eligible = false;
  p.non_transfer = reason;
  return p;
}

}  // namespace detail

// 一条任务或一个收到的包该走哪条路径。
//
// 两端到底是哪两处，只认任务元数据里写明的那一条，不从 opcode 猜：同一个 opcode 在
// 不同的 Map 上两端可以不同，猜出来的占用会把两条本可并行的路径判成互斥。写明了就按
// 写的走，没写就只认方向，这是诚实的兜底。
inline DsaPlan ResolveDsaPlan(bool outgoing, Opcode opcode, TaskMeta const& meta,
                              uint64_t core_id, bool inactive_forward = false,
                              bool local_res_sum = false,
                              bool local_retire = false) {
  (void)core_id;
  if (local_res_sum) return detail::NonTransferPlan(DsaNonTransfer::kLocalResSum);
  if (local_retire) return detail::NonTransferPlan(DsaNonTransfer::kLocalRetire);

  if (inactive_forward) {
    DsaPlan p;
    p.queue = DsaQueue::kControlForward;
    p.group = DsaGroup::kControlForward;
    // 转发既要收也要发，两条物理通道都占。
    p.mask = kDsaBothMask;
    p.route = DsaRoute::kNone;
    p.source = DsaSource::kInternal;
    p.analysis_eligible = false;
    return p;
  }

  DsaPlan p;
  switch (meta.dsa_route) {
    case DsaRoute::kR2M:
    case DsaRoute::kR2C:
      LOGCHECK(!outgoing, "ResolveDsaPlan: R2M and R2C only run on the "
                          "incoming path.");
      p.queue = meta.dsa_route == DsaRoute::kR2M ? DsaQueue::kR2M : DsaQueue::kR2C;
      p.group = DsaGroup::kToCore;
      p.mask = kDsaInMask;
      p.route = meta.dsa_route;
      p.source = DsaSource::kExplicit;
      p.analysis_eligible = true;
      return p;

    case DsaRoute::kM2R:
    case DsaRoute::kC2R:
      LOGCHECK(outgoing, "ResolveDsaPlan: M2R and C2R only run on the outgoing "
                         "path.");
      p.queue = meta.dsa_route == DsaRoute::kM2R ? DsaQueue::kM2R : DsaQueue::kC2R;
      p.group = DsaGroup::kToRouter;
      p.mask = kDsaOutMask;
      p.route = meta.dsa_route;
      p.source = DsaSource::kExplicit;
      p.analysis_eligible = true;
      return p;

    case DsaRoute::kM2C:
      // 本地搬运不上链路，所以它必须是一条出方向的 MOVE，并且元数据要点明这是本地
      // 搬运、两端分别是 MatrixMem 与 Core。少一样都说明这条路径写错了地方。
      LOGCHECK(outgoing && opcode == Opcode::kMove,
               "ResolveDsaPlan: M2C must be an outgoing MOVE task.");
      LOGCHECK(meta.dsa_local_transfer,
               "ResolveDsaPlan: M2C needs dsa_local_transfer.");
      LOGCHECK(meta.source_endpoint == Endpoint::kNone ||
                   meta.source_endpoint == Endpoint::kMatrix,
               "ResolveDsaPlan: M2C starts at MatrixMem.");
      LOGCHECK(meta.destination_endpoint == Endpoint::kNone ||
                   meta.destination_endpoint == Endpoint::kCore,
               "ResolveDsaPlan: M2C ends at the Core.");
      p.queue = DsaQueue::kM2C;
      p.group = DsaGroup::kLocal;
      p.mask = kDsaBothMask;
      p.route = DsaRoute::kM2C;
      p.source = DsaSource::kExplicit;
      p.analysis_eligible = true;
      return p;

    case DsaRoute::kNone:
      break;
  }

  // 任务表没写路径：只按方向归类，落不到某一条具体路径上。
  p.queue = outgoing ? DsaQueue::kDirectionOut : DsaQueue::kDirectionIn;
  p.group = outgoing ? DsaGroup::kToRouter : DsaGroup::kToCore;
  p.mask = outgoing ? kDsaOutMask : kDsaInMask;
  p.route = DsaRoute::kNone;
  p.source = DsaSource::kDirectionFallback;
  p.analysis_eligible = true;
  return p;
}

// ---------------------------------------------------------------- 留痕

// 一次被授予过的请求留下的东西。它只供跑完之后做分析，运行期谁也不读。
struct DsaRecord {
  DsaRoute route = DsaRoute::kNone;
  DsaQueue queue = DsaQueue::kNonTransfer;
  DsaGroup group = DsaGroup::kNonTransfer;
  uint32_t mask = 0;
  uint64_t core_id = 0;
  uint64_t uid = 0;
  uint64_t tid = 0;
  Time enqueue = 0;
  Time grant = 0;
  Time release = 0;
  bool analysis_eligible = false;
  // 入队那一刻挡在它前面的那些路径。只记得出精确路径的那些。
  std::vector<DsaRoute> blocked_by;
};

// ---------------------------------------------------------------- 仲裁

class DsaArbiter {
 public:
  DsaArbiter(ExclusiveArbiter* inbound, ExclusiveArbiter* outbound,
             uint64_t core = 0)
      : in(inbound), out(outbound), core_id(core) {
    LOGCHECK(in != nullptr && out != nullptr,
             "DsaArbiter: both physical lanes are required.");
    LOGCHECK(in != out,
             "DsaArbiter: five_route needs two separate physical lanes.");
  }

  Ticket Request(Time now, DsaPlan const& plan, uint64_t uid, uint64_t tid) {
    LOGCHECK(!plan.IsNonTransfer(),
             "DsaArbiter: work that moves nothing must not queue for a route.");
    const Ticket t = next_ticket++;
    Entry e;
    e.plan = plan;
    e.uid = uid;
    e.tid = tid;
    e.seq = next_seq++;
    e.enqueue = now;
    e.blocked_by = BlockersOf(plan.mask);
    entries.emplace(t, e);
    queues[static_cast<uint32_t>(plan.queue)].push_back(t);
    Dispatch(now);
    return t;
  }

  bool Granted(Ticket t) const { return Find(t).granted; }
  Time EnqueueCycle(Ticket t) const { return Find(t).enqueue; }
  Time GrantCycle(Ticket t) const { return Find(t).grant; }

  void Release(Ticket t, Time now) {
    auto it = entries.find(t);
    LOGCHECK(it != entries.end(), "DsaArbiter: release an unknown request.");
    Entry& e = it->second;
    LOGCHECK(e.granted, "DsaArbiter: release before grant.");
    if (e.in_ticket != kNoTicket) in->Release(e.in_ticket, now);
    if (e.out_ticket != kNoTicket) out->Release(e.out_ticket, now);
    active.erase(t);
    Keep(e, now);
    entries.erase(it);
    Recompute();
    Dispatch(now);
  }

  // 只能撤销还没被授予的请求。
  void Cancel(Ticket t, Time now) {
    auto it = entries.find(t);
    LOGCHECK(it != entries.end(), "DsaArbiter: cancel an unknown request.");
    LOGCHECK(!it->second.granted, "DsaArbiter: cancel after grant.");
    std::deque<Ticket>& q = queues[static_cast<uint32_t>(it->second.plan.queue)];
    for (auto p = q.begin(); p != q.end(); ++p) {
      if (*p == t) {
        q.erase(p);
        break;
      }
    }
    entries.erase(it);
    Dispatch(now);
  }

  uint32_t ActiveMask() const { return active_mask; }
  uint64_t ActiveNum() const { return active.size(); }
  uint64_t PendingNum() const {
    uint64_t total = 0;
    for (auto const& q : queues) total += q.size();
    return total;
  }
  uint64_t QueueLen(DsaQueue q) const {
    return queues[static_cast<uint32_t>(q)].size();
  }
  std::vector<DsaRecord> const& Records() const { return records; }

 private:
  struct Entry {
    DsaPlan plan;
    uint64_t uid = 0;
    uint64_t tid = 0;
    uint64_t seq = 0;
    Time enqueue = 0;
    Time grant = 0;
    bool granted = false;
    Ticket in_ticket = kNoTicket;
    Ticket out_ticket = kNoTicket;
    std::vector<DsaRoute> blocked_by;
  };

  Entry const& Find(Ticket t) const {
    auto it = entries.find(t);
    LOGCHECK(it != entries.end(), "DsaArbiter: unknown request.");
    return it->second;
  }

  // 入队那一刻，正在占用的与还在排队的里面，掩码与它相交的那些。
  std::vector<DsaRoute> BlockersOf(uint32_t mask) const {
    std::vector<DsaRoute> out_list;
    for (Ticket t : active) {
      Entry const& e = entries.at(t);
      if ((e.plan.mask & mask) != 0 && e.plan.route != DsaRoute::kNone) {
        out_list.push_back(e.plan.route);
      }
    }
    for (auto const& q : queues) {
      for (Ticket t : q) {
        Entry const& e = entries.at(t);
        if ((e.plan.mask & mask) != 0 && e.plan.route != DsaRoute::kNone) {
          out_list.push_back(e.plan.route);
        }
      }
    }
    return out_list;
  }

  bool Eligible(Ticket t) const {
    Entry const& e = entries.at(t);
    if ((e.plan.mask & active_mask) != 0) return false;
    for (auto const& q : queues) {
      for (Ticket o : q) {
        Entry const& oe = entries.at(o);
        if (oe.seq < e.seq && (oe.plan.mask & e.plan.mask) != 0) return false;
      }
    }
    return true;
  }

  void Grant(Ticket t, Time now) {
    Entry& e = entries.at(t);
    std::deque<Ticket>& q = queues[static_cast<uint32_t>(e.plan.queue)];
    LOGCHECK(!q.empty() && q.front() == t,
             "DsaArbiter: only a queue head may be granted.");
    q.pop_front();

    // 逻辑上已经判过不冲突，物理通道此刻就该是空的。不是的话，有人绕过仲裁器占了它。
    if ((e.plan.mask & kDsaInMask) != 0) {
      LOGCHECK(!in->Busy() && in->QueueLen() == 0,
               "DsaArbiter: the inbound lane is held outside the arbiter.");
      e.in_ticket = in->Enqueue(now, e.uid);
      LOGCHECK(in->Granted(e.in_ticket),
               "DsaArbiter: the inbound lane did not grant at the same cycle.");
    }
    if ((e.plan.mask & kDsaOutMask) != 0) {
      LOGCHECK(!out->Busy() && out->QueueLen() == 0,
               "DsaArbiter: the outbound lane is held outside the arbiter.");
      e.out_ticket = out->Enqueue(now, e.uid);
      LOGCHECK(out->Granted(e.out_ticket),
               "DsaArbiter: the outbound lane did not grant at the same cycle.");
    }

    e.granted = true;
    e.grant = now;
    active.insert(t);
    active_mask |= e.plan.mask;
  }

  void Dispatch(Time now) {
    for (;;) {
      std::vector<Ticket> heads;
      for (auto const& q : queues) {
        if (!q.empty()) heads.push_back(q.front());
      }
      if (heads.empty()) return;
      std::sort(heads.begin(), heads.end(), [this](Ticket a, Ticket b) {
        return entries.at(a).seq < entries.at(b).seq;
      });
      bool moved = false;
      for (Ticket t : heads) {
        if (!Eligible(t)) continue;
        Grant(t, now);
        moved = true;
      }
      if (!moved) return;
    }
  }

  void Recompute() {
    active_mask = 0;
    for (Ticket t : active) active_mask |= entries.at(t).plan.mask;
  }

  void Keep(Entry const& e, Time now) {
    DsaRecord r;
    r.route = e.plan.route;
    r.queue = e.plan.queue;
    r.group = e.plan.group;
    r.mask = e.plan.mask;
    r.core_id = core_id;
    r.uid = e.uid;
    r.tid = e.tid;
    r.enqueue = e.enqueue;
    r.grant = e.grant;
    r.release = now;
    r.analysis_eligible = e.plan.analysis_eligible;
    r.blocked_by = e.blocked_by;
    records.push_back(std::move(r));
  }

  ExclusiveArbiter* in;
  ExclusiveArbiter* out;
  uint64_t core_id;

  std::deque<Ticket> queues[kDsaQueueNum];
  std::unordered_map<Ticket, Entry> entries;
  std::unordered_set<Ticket> active;
  uint32_t active_mask = 0;
  uint64_t next_seq = 0;
  Ticket next_ticket = 1;
  std::vector<DsaRecord> records;
};

// ---------------------------------------------------------------- 分析

// 一条路径上的占用。
struct DsaRouteStat {
  uint64_t grants = 0;
  Time busy = 0;    // 占着物理通道的总拍数
  Time waited = 0;  // 排队等着的总拍数
};

// 两条路径之间的那一个交点。
struct DsaCrossStat {
  DsaRoute a = DsaRoute::kNone;
  DsaRoute b = DsaRoute::kNone;
  // 掩码相交，两条本就不允许同时占用。
  bool mask_conflict = false;
  // 实际同时占用的拍数。掩码相交的交点上它必须是零，否则仲裁器漏了。
  Time overlap = 0;
  // 一条入队时被另一条挡住的次数，两个方向合计。
  uint64_t blocks = 0;
};

struct DsaAnalysis {
  DsaRouteStat routes[kDsaRouteNum];
  DsaCrossStat crosses[kDsaCrossNum];
  // 只知道方向、落不到某条路径上的请求数。它偏大说明 Map 里显式路径写得不全。
  uint64_t direction_only = 0;
  // 不参与路径分析的请求数：控制转发那一类。
  uint64_t excluded = 0;

  DsaRouteStat const& Route(DsaRoute r) const {
    return routes[DsaRouteIndex(r)];
  }

  DsaCrossStat const& Cross(DsaRoute x, DsaRoute y) const {
    for (uint32_t i = 0; i < kDsaCrossNum; ++i) {
      if ((crosses[i].a == x && crosses[i].b == y) ||
          (crosses[i].a == y && crosses[i].b == x)) {
        return crosses[i];
      }
    }
    LOGCHECK(false, "DsaAnalysis: those two routes have no crossing.");
    return crosses[0];
  }
};

// 跑完之后把各核的痕迹汇总成五条路径的占用与十个交点。它只读不写，与模型无关。
inline DsaAnalysis AnalyzeDsa(std::vector<DsaRecord> const& records) {
  DsaAnalysis out;
  uint32_t next = 0;
  for (uint32_t i = 0; i < kDsaRouteNum; ++i) {
    for (uint32_t j = i + 1; j < kDsaRouteNum; ++j) {
      DsaCrossStat& c = out.crosses[next++];
      c.a = DsaRouteAt(i);
      c.b = DsaRouteAt(j);
      c.mask_conflict = (DsaRouteMask(c.a) & DsaRouteMask(c.b)) != 0;
    }
  }

  // 一条路径上的全部占用区间，供求交用。
  std::vector<std::pair<Time, Time>> spans[kDsaRouteNum];
  for (DsaRecord const& r : records) {
    if (!r.analysis_eligible) {
      ++out.excluded;
      continue;
    }
    if (r.route == DsaRoute::kNone) {
      ++out.direction_only;
      continue;
    }
    const uint32_t idx = DsaRouteIndex(r.route);
    DsaRouteStat& s = out.routes[idx];
    ++s.grants;
    s.busy += r.release - r.grant;
    s.waited += r.grant - r.enqueue;
    spans[idx].emplace_back(r.grant, r.release);

    for (DsaRoute blocker : r.blocked_by) {
      if (blocker == r.route) continue;
      for (uint32_t i = 0; i < kDsaCrossNum; ++i) {
        DsaCrossStat& c = out.crosses[i];
        if ((c.a == blocker && c.b == r.route) ||
            (c.b == blocker && c.a == r.route)) {
          ++c.blocks;
          break;
        }
      }
    }
  }

  for (uint32_t i = 0; i < kDsaCrossNum; ++i) {
    DsaCrossStat& c = out.crosses[i];
    for (auto const& x : spans[DsaRouteIndex(c.a)]) {
      for (auto const& y : spans[DsaRouteIndex(c.b)]) {
        const Time lo = std::max(x.first, y.first);
        const Time hi = std::min(x.second, y.second);
        if (hi > lo) c.overlap += hi - lo;
      }
    }
  }
  return out;
}

}
}

#endif
