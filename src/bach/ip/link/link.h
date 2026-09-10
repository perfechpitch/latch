#ifndef _LATCH_BACH_IP_LINK_LINK_
#define _LATCH_BACH_IP_LINK_LINK_

// 链路：两个端口之间的一段带宽加延迟。
//
// 每条物理链路每个方向一个实例。R2R、C2C、PCIe ↔ Router、ETH 都是这一个模块，
// 差别只在带宽与延迟两个参数。
//
// 到达拍算在发送侧（本模块入口），不算在接收侧，这样链路的占用 last_busy_until
// 只有一个 owner：
//
//   arrive = max(now, last_busy_until) + ceil(size / bw) + latency
//   last_busy_until = arrive - latency
//
// 端口固有的一拍延迟被 latency 吸收，不额外扣时：R2R 40T、C2C 400T 都远大于一拍。
//
// 数据与三种 release 各走各的实例、参数相同，所以这里是四条互不相干的通道，各
// 记各的 last_busy_until。

#include <deque>
#include <string>

#include "base/log.h"
#include "bach/common/flit.h"
#include "bach/common/params.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class Link : public BachModule {
 public:
  Link(ClockPtr clock, const std::string& name, LinkParams const& params,
       uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        cfg(params),
        in(std::make_shared<LinkEnd>(clock)),
        out(std::make_shared<LinkEnd>(clock)),
        sent(clock),
        delivered(clock),
        inflight(clock) {}

  // 发送方把自己的出口写进 In()，接收方从 Out() 读。
  LinkEnd& In() { return *in; }
  LinkEnd& Out() { return *out; }
  LinkEndPtr InPtr() const { return in; }
  LinkEndPtr OutPtr() const { return out; }

  // 装配层接线：把本模块的某一端换成上下游共用的那根。
  void AttachIn(LinkEndPtr wire) { in = std::move(wire); }
  void AttachOut(LinkEndPtr wire) { out = std::move(wire); }

  uint64_t Sent() const { return sent.Get(); }
  uint64_t Delivered() const { return delivered.Get(); }
  uint64_t Inflight() const { return inflight.Get(); }

  bool Quiescent() const override {
    return flit_q.empty() && vc_q.empty() && stream_q.empty() &&
           reduce_q.empty();
  }

 protected:
  void Step() override {
    uint64_t now = CycleNow();

    // 末级先做：先把到点的送出去，再收本拍新进来的。
    DeliverAll(now);
    Accept(now);

    inflight = flit_q.size() + vc_q.size() + stream_q.size() + reduce_q.size();
    sent = sent_pending;
    delivered = delivered_pending;
    EmitTraces();
  }

 private:
  // 一条通道上排一笔的到达拍。size 不大于 0 算一拍，与 CalcCycles 一致。
  struct Chan {
    Time last_busy_until = 0;
    uint64_t Schedule(uint64_t now, int64_t size, LinkParams const& p) {
      uint64_t start = now > last_busy_until ? now : last_busy_until;
      uint64_t arrive = start + CalcCycles(size, p.bandwidth) + p.latency;
      last_busy_until = arrive - p.latency;
      return arrive;
    }
  };

  struct FlitItem {
    FlitView v;
    uint64_t arrive = 0;
  };
  struct RelItem {
    uint64_t user_or_vc = 0;
    uint64_t arrive = 0;
  };

  void Accept(uint64_t now) {
    FlitView f = ReadFlit(in->flit);
    if (f.valid) {
      LOGCHECK(flit_q.size() < cfg.queue_depth,
               "Link: 在途队列满了。上游按 credit 发，满就说明 credit 记错了。");
      uint64_t arrive = flit_chan.Schedule(now, int64_t(f.bytes), cfg);
      PushMonotonic(flit_q, arrive);
      flit_q.push_back({f, arrive});
      ++sent_pending;
    }

    ReleaseView r = ReadRelease(in->release);
    // release 是小消息，按一拍占用记。
    if (r.vc_valid) {
      uint64_t arrive = vc_chan.Schedule(now, 0, cfg);
      PushMonotonic(vc_q, arrive);
      vc_q.push_back({r.vc_id, arrive});
    }
    if (r.stream_valid) {
      uint64_t arrive = stream_chan.Schedule(now, 0, cfg);
      PushMonotonic(stream_q, arrive);
      stream_q.push_back({r.stream_user, arrive});
    }
    if (r.reduce_valid) {
      uint64_t arrive = reduce_chan.Schedule(now, 0, cfg);
      PushMonotonic(reduce_q, arrive);
      reduce_q.push_back({r.reduce_user, arrive});
    }
  }

  void DeliverAll(uint64_t now) {
    // 每拍最多送一个 flit。
    if (!flit_q.empty() && flit_q.front().arrive <= now) {
      FlitView const& f = flit_q.front().v;
      out->flit.Drive(f.vc, f.head, f.tail, f.bytes, f.msg);
      flit_q.pop_front();
      ++delivered_pending;
    } else {
      out->flit.Idle();
    }

    // 三种 release 各自独立，一拍可以同时送。
    bool vc_rel = !vc_q.empty() && vc_q.front().arrive <= now;
    bool stream_rel = !stream_q.empty() && stream_q.front().arrive <= now;
    bool reduce_rel = !reduce_q.empty() && reduce_q.front().arrive <= now;
    uint64_t vc_id = vc_rel ? vc_q.front().user_or_vc : 0;
    uint64_t stream_u = stream_rel ? stream_q.front().user_or_vc : 0;
    uint64_t reduce_u = reduce_rel ? reduce_q.front().user_or_vc : 0;
    out->release.Drive(vc_rel, vc_id, stream_rel, stream_u, reduce_rel,
                       reduce_u);
    if (vc_rel) vc_q.pop_front();
    if (stream_rel) stream_q.pop_front();
    if (reduce_rel) reduce_q.pop_front();
  }

  // 同一条链路上到达拍单调递增：发送侧的占用时刻单调，所以队首不会挡住更早到达
  // 的项。这一条是出队逻辑（只看队首）成立的前提，破了就说明 Schedule 算错了。
  template <typename Q>
  void PushMonotonic(Q const& q, uint64_t arrive) {
    if (!q.empty()) {
      LOGCHECK(arrive >= q.back().arrive,
               "Link: 到达拍不单调，队首会挡住更早到达的项。");
    }
  }

  void EmitTraces() {
    TracePerCycle("inflight", inflight.Get());
    TracePerCycle("sent", sent.Get());
    TracePerCycle("delivered", delivered.Get());
  }

  LinkParams cfg;
  LinkEndPtr in, out;

  // Step 独占，不需要任何同步。
  std::deque<FlitItem> flit_q;
  std::deque<RelItem> vc_q, stream_q, reduce_q;
  Chan flit_chan, vc_chan, stream_chan, reduce_chan;
  uint64_t sent_pending = 0, delivered_pending = 0;

  // 跨拍可读，末尾一次性 commit。
  Logic64 sent, delivered, inflight;
};

}  // namespace bach
}  // namespace latch

#endif
