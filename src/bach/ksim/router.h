#ifndef _LATCH_BACH_KSIM_ROUTER_
#define _LATCH_BACH_KSIM_ROUTER_

// 每个核一个 Router，按物理连接把包一跳一跳送到目标核。
//
// 它是从属单元，不挂时钟，由 Core 在自己的 Cycle 体内调 Step，所以与 Core 同拍工作。
// 一拍里做两件事，顺序固定：
//
//   投递  本地队列里到点的包交给本核
//   仲裁  各入端口里到点的包轮流出一个，走不同出口的互不相干
//
// 每个出口方向各占各的：左邻、右邻、垂直邻居、PCIe 是四条独立的双向链路，往左发不挡着
// 往右发，堵只堵在同一条链路上。
//
// 一次服务的时间由三段相加：那条出口上一次占用还没结束的那一段是排队，字节数除以带宽
// 是服务，出口是 PCIe 口时线延迟取 PCIe 的那一档，否则取每跳的那一档。
//
// 包不在节点上落地。包头到了就往下一跳转，不等整包收完，所以走几跳只多几段线延迟。
// 只有终点要等包尾。
//
// 入端口的队列是唯一被别人碰的地方：邻居的 Core 在它自己的协程里调 Push 把包塞进来，
// 而这些协程分布在多个 OS 线程上，所以这一处要加锁。核内的其余状态只被本核的 Cycle
// 触碰，不需要同步。
//
// 一个入端口的队头出不去就挡着它后面的包，因为没有虚通道。同一条链路上两个包不交错，
// 一个大包会把后面的整个挡住；要交错得切片，那是另一件事。

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "base/log.h"
#include "base/runtime.h"
#include "base/time_stamp.h"
#include "bach/ip/sub_unit.h"
#include "bach/ksim/topology.h"
#include "bach/observer/span_recorder.h"

namespace latch {
namespace bach {
namespace ksim {

// 一个包。tag 是它属于哪一份输入，接收侧靠它认出这一包是谁的。
struct Flit {
  uint64_t src = 0;
  uint64_t dst = 0;
  uint64_t bytes = 0;
  uint64_t tag = 0;
  Time arrive = 0;
};

struct RouterParams {
  uint64_t bandwidth = 32;
  uint64_t hop_latency = 1;
  uint64_t pcie_bandwidth = 32;
  uint64_t pcie_latency = 400;
};

// 包送到目标之后交给谁。
class FlitSink {
 public:
  virtual ~FlitSink() = default;
  virtual void Deliver(Flit const& f, Time now) = 0;
};

class Router : public SubUnit {
 public:
  Router(ClockPtr clock, uint64_t core_id, Topology const& topology,
         RouterParams const& params, SpanRecorder* recorder,
         std::string const& name, uint64_t parent, bool standalone = false)
      : SubUnit(clock, standalone),
        id(core_id),
        topo(topology),
        par(params),
        rec(recorder),
        queues(kPortCount) {
    RegisterId(name, parent);
  }

  void ConnectNeighbor(Port p, Router* r) {
    LOGCHECK(static_cast<uint32_t>(p) < kPortCount, "Router: bad port.");
    peers[static_cast<uint32_t>(p)] = r;
  }
  void ConnectCore(FlitSink* c) { core = c; }
  // 这个口通向阵列外面。接上之后往外的包交给它，不再往下一跳走。
  void ConnectHostPort(FlitSink* h) { host = h; }

  uint64_t CoreId() const { return id; }

  // 本核发出一个包。它跟别处进来的包一样排队，不插队。
  void Inject(Flit f, Time now) {
    f.arrive = now;
    std::lock_guard<std::mutex> lk(mu);
    queues[static_cast<uint32_t>(Port::kCore0)].push_back(f);
  }

  // 邻居送进来。at 是这一包到达本节点的时刻，由发送侧算好。
  void Push(Flit f, Port in, Time at) {
    f.arrive = at;
    std::lock_guard<std::mutex> lk(mu);
    queues[static_cast<uint32_t>(in)].push_back(f);
  }

  void Step() override {
    const Time now = RT::Now();
    DeliverLocal(now);
    Arbitrate(now);
    TraceQueued(Queued());
  }

  uint64_t Queued() const {
    uint64_t n = local.size();   // 只被本核碰，不用锁
    std::lock_guard<std::mutex> lk(mu);
    for (auto const& q : queues) n += q.size();
    return n;
  }
  uint64_t Forwarded() const { return forwarded; }
  Time BusyUntil(Port p) const { return busy[static_cast<uint32_t>(p)]; }

 private:
  // 队列长度只在它变了的时候打一个点，一条线上留下的就是它真正跳变的那些时刻。
  void TraceQueued(uint64_t v) {
    if (v == last_queued) return;
    last_queued = v;
    Trace("queued", v);
  }

  void DeliverLocal(Time now) {
    while (!local.empty() && local.front().arrive <= now) {
      LOGCHECK(core != nullptr, "Router: no core attached.");
      core->Deliver(local.front(), now);
      local.pop_front();
    }
  }

  void Arbitrate(Time now) {
    // 锁内只把这一拍能走的挑出来，放开锁再转发。转发要去碰对端的队列，持着自己的锁
    // 去拿别人的，两个互为邻居的 Router 就会各拿一半互等。
    std::vector<Flit> going;
    {
      std::lock_guard<std::mutex> lk(mu);
      for (uint32_t served = 0; served < kPortCount; ++served) {
        const uint32_t p = next_port;
        next_port = (next_port + 1) % kPortCount;
        auto& q = queues[p];
        if (q.empty() || q.front().arrive > now) continue;
        // 这一包要走的那条出口忙着，就先不动它，也就挡住了它后面的。
        Flit const& head = q.front();
        if (head.dst != id) {
          const uint32_t out =
              static_cast<uint32_t>(topo.RouteFrom(id, head.dst));
          if (busy[out] > now) continue;
        }
        going.push_back(q.front());
        q.pop_front();
      }
    }
    for (Flit const& f : going) Forward(f, now);
  }

  void Forward(Flit f, Time now) {
    if (f.dst == id) {
      local.push_back(f);
      return;
    }
    const Port out = topo.RouteFrom(id, f.dst);
    const bool pcie = out == Port::kPcie;
    const uint64_t bw = pcie ? par.pcie_bandwidth : par.bandwidth;
    const uint64_t lat = pcie ? par.pcie_latency : par.hop_latency;
    LOGCHECK(bw > 0, "Router: bandwidth must be positive.");

    const uint32_t o = static_cast<uint32_t>(out);
    const Time start = busy[o] > now ? busy[o] : now;
    const Time service = (f.bytes + bw - 1) / bw;
    busy[o] = start + service;
    ++forwarded;
    if (rec != nullptr) {
      rec->Span(id, Unit::kRouter, f.tag, o, start, busy[o],
                SpanState::kTransfer, f.bytes);
    }

    // 包头在 start + lat 到对端，包尾还要再过一次传输时间。
    const Time head = start + lat;
    const Time tail = start + service + lat;

    if (out == Port::kPcie && host != nullptr && f.dst == kHostTarget) {
      host->Deliver(f, tail);
      return;
    }
    Router* peer = peers[o];
    LOGCHECK(peer != nullptr, "Router: nothing wired to the outgoing port.");
    // 对端就是终点的话要等包尾，只是路过的话包头到了就能接着往下转。
    peer->Push(f, Facing(out), peer->CoreId() == f.dst ? tail : head);
  }

  static Port Facing(Port out) {
    switch (out) {
      case Port::kLeft: return Port::kRight;
      case Port::kRight: return Port::kLeft;
      case Port::kVert: return Port::kVert;
      case Port::kPcie: return Port::kPcie;
      default: return Port::kCore1;
    }
  }

  uint64_t id;
  Topology topo;
  RouterParams par;
  SpanRecorder* rec;
  std::vector<std::deque<Flit>> queues;
  std::deque<Flit> local;
  std::vector<Router*> peers = std::vector<Router*>(kPortCount, nullptr);
  std::vector<Time> busy = std::vector<Time>(kPortCount, 0);
  FlitSink* core = nullptr;
  FlitSink* host = nullptr;
  uint32_t next_port = 0;
  uint64_t forwarded = 0;
  uint64_t last_queued = ~uint64_t{0};
  mutable std::mutex mu;
};

}
}
}

#endif
