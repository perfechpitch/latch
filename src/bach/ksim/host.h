#ifndef _LATCH_BACH_KSIM_HOST_
#define _LATCH_BACH_KSIM_HOST_

// 阵列外面那个模块。它是挂时钟的节点。
//
// 核只做 FFN，前后的计算都在它这里，它照时序表把每一层的输入按拍喂进阵列，再把结果
// 收回来。表里写着第几拍该往哪个核喂多少字节，它本身没有别的逻辑。
//
// 喂之前先扣额度。往一个核送一份输入就扣它一份，那个核把这一层做完、结果推出去了才还
// 一份。额度是硬件参数，容量就是这个核同时装得下几份输入。扣光了时序表说该喂也喂不进
// 去，所以表排得比阵列跑得快时不会一路灌爆，堵会顶回到这里。一个核没额度不挡着别的核，
// 待发的按核分开排。
//
// 阵列两侧各有一个 PCIe Switch，进和出各走一边：喂进去的包落在目标核那一行最左边那个
// chip 的左出口上，带一次 PCIe 延迟；结果一路向东，走到最右那一列的右出口才出得去。
// 一条链路只承一个方向的流量。
//
// 每喂出一份记一条起跑，每收回一份记一条完成，两者配对就是这一份从进阵列到出阵列的
// 延迟。
//
// 收包与还额度这两个入口是被别人碰的：包由某个 Router 在它那个核的协程里送进来，额度
// 由某个核做完一层时还回来，而这些协程分布在多个 OS 线程上。全阵列只有这一个模块被所
// 有核共同触碰，所以它的状态要加锁。

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/clock.h"
#include "base/log.h"
#include "base/module.h"
#include "base/runtime.h"
#include "bach/ksim/exec.h"
#include "bach/ksim/kernel.h"
#include "bach/ksim/router.h"
#include "bach/ksim/topology.h"
#include "bach/observer/span_recorder.h"

namespace latch {
namespace bach {
namespace ksim {

// Host 往阵列里送包要经过的入口，由装配方提供。
class Ingress {
 public:
  virtual ~Ingress() = default;
  virtual Router* EntryFor(uint64_t core) = 0;
};

class Host : public ClkModule, public FlitSink, public CreditSink {
 public:
  Host(ClockPtr clock, KernelImage const& image, Ingress* ingress,
       Topology const& topology, uint64_t pcie_latency,
       uint64_t credit_capacity, std::string const& name, uint64_t parent)
      : ClkModule(clock),
        in(ingress),
        topo(topology),
        lat(pcie_latency),
        cap(credit_capacity),
        feeds(image.feeds) {
    LOGCHECK(in != nullptr, "Host: ingress is null.");
    LOGCHECK(cap > 0, "Host: credit capacity must be positive.");
    RegisterId(name, parent);
    std::stable_sort(feeds.begin(), feeds.end(),
                     [](Feed const& a, Feed const& b) { return a.cycle < b.cycle; });
    for (auto const& d : image.drains) expect_bytes += d.length;
  }

  void Cycle() override {
    DelayCycle(1);
    const Time now = RT::Now();
    // 锁内只决定这一拍该喂哪几份，放开锁再往阵列里送。送要去碰入口 Router 的队列，
    // 持着自己的锁去拿别人的，跟对面反过来送时就会互等。
    std::vector<Feed> going;
    uint64_t inflight = 0;
    {
      std::lock_guard<std::mutex> lk(mu);
      while (next < feeds.size() && feeds[next].cycle <= now) {
        pending[feeds[next].core].push_back(feeds[next]);
        ++next;
      }
      for (auto& kv : pending) {
        const uint64_t core = kv.first;
        auto& q = kv.second;
        while (!q.empty()) {
          // 头一次碰到这个核时它手里是满额的。取出来再减，别直接减一个刚插进去的零。
          auto it = credit.find(core);
          if (it == credit.end()) it = credit.emplace(core, cap).first;
          if (it->second == 0) {
            if (stall_since.find(core) == stall_since.end()) {
              stall_since[core] = now;
            }
            break;
          }
          --it->second;
          Feed f = q.front();
          q.pop_front();
          recorder.GlobalStart(fed_count, now, 1);
          f.cycle = fed_count;   // 借这一栏把包上的号带出去
          ++fed_count;
          fed_bytes += f.length;
          going.push_back(f);
        }
      }
      inflight = fed_count - drained_count;
    }
    for (Feed const& f : going) Inject(f, now);
    TraceIf("inflight", inflight, &last_inflight);
  }

  void Deliver(Flit const& f, Time now) override {
    std::lock_guard<std::mutex> lk(mu);
    drained_bytes += f.bytes;
    ++drained_count;
    recorder.GlobalEnd(f.tag, now);
  }

  void ReturnCredit(uint64_t core) override {
    std::lock_guard<std::mutex> lk(mu);
    auto it = credit.find(core);
    if (it == credit.end()) return;
    if (it->second < cap) ++it->second;
    // 这个核之前把 Host 卡住过，那一段记成等额度。
    auto s = stall_since.find(core);
    if (s != stall_since.end()) {
      recorder.Wait(core, Unit::kHost, 0, 0, s->second, RT::Now(),
                    WaitReason::kHostCredit);
      stall_since.erase(s);
    }
  }

  bool FedAll() const {
    std::lock_guard<std::mutex> lk(mu);
    if (next < feeds.size()) return false;
    for (auto const& kv : pending) {
      if (!kv.second.empty()) return false;
    }
    return true;
  }
  bool DrainedAll() const {
    std::lock_guard<std::mutex> lk(mu);
    return drained_bytes >= expect_bytes;
  }
  uint64_t FedBytes() const { return fed_bytes; }
  uint64_t FedCount() const { return fed_count; }
  uint64_t DrainedBytes() const { return drained_bytes; }
  uint64_t ExpectBytes() const { return expect_bytes; }
  SpanRecorder const& Recorder() const { return recorder; }

 private:
  void TraceIf(const char* name, uint64_t v, uint64_t* last) {
    if (v == *last) return;
    *last = v;
    Trace(name, v);
  }

  // 号已经在上面记好了，借 cycle 那一栏带下来。
  void Inject(Feed const& f, Time now) {
    Router* entry = in->EntryFor(f.core);
    LOGCHECK(entry != nullptr, "Host: no ingress router for that core.");
    Flit flit;
    flit.src = entry->CoreId();
    flit.dst = f.core;
    flit.bytes = f.length;
    flit.tag = f.cycle;
    entry->Push(flit, Port::kPcie, now + lat);
  }

  Ingress* in;
  Topology topo;
  uint64_t lat;
  uint64_t cap;
  SpanRecorder recorder;
  std::vector<Feed> feeds;
  std::size_t next = 0;
  std::unordered_map<uint64_t, std::deque<Feed>> pending;
  std::unordered_map<uint64_t, uint64_t> credit;
  std::unordered_map<uint64_t, Time> stall_since;
  uint64_t fed_bytes = 0;
  uint64_t fed_count = 0;
  uint64_t drained_bytes = 0;
  uint64_t drained_count = 0;
  uint64_t expect_bytes = 0;
  uint64_t last_inflight = ~uint64_t{0};
  mutable std::mutex mu;
};

}
}
}

#endif
