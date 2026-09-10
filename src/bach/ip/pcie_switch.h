#ifndef _LATCH_BACH_IP_PCIE_SWITCH_
#define _LATCH_BACH_IP_PCIE_SWITCH_

// PCIe Switch：chip 阵列与片外之间的交换节点。
//
// 每层 chip 的两端各接一个，一个 Switch 接两层。同层 chip 之间、同列 chip 之间
// 是直连，不经它。
//
// 两级：
//   W1 收包查目的端口，组播时复制到多个出口队列，全有全无（按最慢收端反压）
//   W2 每端口每拍出 1 flit
//
// 双路 x16 各自独立计时、不保序：计时是外侧那两条 Link 实例的事，Switch 这一层
// 只按 flit 轮流把它们分到两路上。
//
// 组播默认关：当前选定 LPU 广播，CPU 只把 token 送进入口 chip，chip 之间自己传播。

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/common/flit.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 出口队列深度，待定。
constexpr uint64_t kSwitchQueueDepth = 32;

// 双路 x16，不支持 x32。
constexpr uint64_t kSwitchLanes = 2;

class PcieSwitch : public BachModule {
 public:
  PcieSwitch(ClockPtr clock, const std::string& name, uint64_t port_num,
             uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick), ports(port_num) {
    for (uint64_t p = 0; p < port_num; ++p) {
      in.push_back(std::make_shared<LinkEnd>(clock));
      out.push_back(std::make_shared<LinkEnd>(clock));
      out_q.emplace_back();
      lane_rr.push_back(0);
    }
    accepted = std::make_shared<Logic64>(clock);
    forwarded = std::make_shared<Logic64>(clock);
    stalled = std::make_shared<Logic64>(clock);
  }

  LinkEnd& In(uint64_t p) { return *in.at(p); }
  LinkEnd& Out(uint64_t p) { return *out.at(p); }
  LinkEndPtr InPtr(uint64_t p) const { return in.at(p); }
  LinkEndPtr OutPtr(uint64_t p) const { return out.at(p); }

  void AttachIn(uint64_t p, LinkEndPtr wire) { in.at(p) = std::move(wire); }
  void AttachOut(uint64_t p, LinkEndPtr wire) { out.at(p) = std::move(wire); }

  // 组播复制表：一个 dst 对应哪几个出口端口。组播关时每个 dst 只填一个端口。
  void SetRoute(uint64_t dst, std::vector<uint64_t> const& dst_ports) {
    LOGCHECK(!dst_ports.empty(), "PcieSwitch: 目的端口集合不能是空的。");
    if (dst_ports.size() > 1) {
      LOGCHECK(multicast, "PcieSwitch: 组播关着，一个 dst 只能配一个端口。");
    }
    for (uint64_t p : dst_ports) {
      LOGCHECK(p < ports, "PcieSwitch: 目的端口越界。");
    }
    mcast_tbl[dst] = dst_ports;
  }

  void EnableMulticast(bool on) { multicast = on; }

  // 装配好之后查这个 Switch 认得哪些目的。Switch 之间不互联，一个 Switch 只
  // 认它自己接的那几个。
  bool HasRoute(uint64_t dst) const { return mcast_tbl.count(dst) != 0; }
  std::vector<uint64_t> const& RouteOf(uint64_t dst) const {
    auto it = mcast_tbl.find(dst);
    LOGCHECK(it != mcast_tbl.end(), "PcieSwitch: 这个目的没有配路由。");
    return it->second;
  }

  uint64_t Accepted() const { return accepted->Get(); }
  uint64_t Forwarded() const { return forwarded->Get(); }
  uint64_t Stalled() const { return stalled->Get(); }

  // 一个 flit 这一拍走的是哪一路 x16。两路不保序，是设计允许的。
  uint64_t LaneOf(uint64_t p) const { return lane_rr.at(p) % kSwitchLanes; }

  bool Quiescent() const override {
    for (auto const& q : out_q) {
      if (!q.empty()) return false;
    }
    return true;
  }

 protected:
  void Step() override {
    // 末级先做：先把出口队列里的送走，腾出的空位本拍就能收新的。
    Drain();
    Accept();

    *accepted = accepted_pending;
    *forwarded = forwarded_pending;
    *stalled = stalled_pending;
    TracePerCycle("accepted", accepted_pending);
    TracePerCycle("forwarded", forwarded_pending);
    TracePerCycle("stalled", stalled_pending);
  }

 private:
  void Accept() {
    for (uint64_t p = 0; p < ports; ++p) {
      FlitView f = ReadFlit(in[p]->flit);
      if (!f.valid) continue;
      std::vector<uint64_t> const& dsts = Lookup(f);
      // 全有全无：全部目的队列都有空才收，按最慢收端反压。只收一半会让同一份
      // 数据在不同分支上错位，已收的那一份占着队列又完不成整体传输。
      bool room = true;
      for (uint64_t d : dsts) {
        if (out_q[d].size() >= kSwitchQueueDepth) room = false;
      }
      if (!room) {
        ++stalled_pending;
        continue;
      }
      for (uint64_t d : dsts) out_q[d].push_back(f);
      ++accepted_pending;
    }
  }

  void Drain() {
    for (uint64_t p = 0; p < ports; ++p) {
      if (out_q[p].empty()) {
        out[p]->flit.Idle();
        out[p]->release.Idle();
        continue;
      }
      FlitView const& f = out_q[p].front();
      out[p]->flit.Drive(f.vc, f.head, f.tail, f.bytes, f.msg);
      out[p]->release.Idle();
      out_q[p].pop_front();
      // 按 flit 轮流走两路 x16，外侧那两条 Link 各自计时。
      ++lane_rr[p];
      ++forwarded_pending;
    }
  }

  std::vector<uint64_t> const& Lookup(FlitView const& f) {
    uint64_t dst = f.msg ? f.msg->dst : 0;
    auto it = mcast_tbl.find(dst);
    LOGCHECK(it != mcast_tbl.end(), "PcieSwitch: 包头的 dst 没有配路由。");
    return it->second;
  }

  uint64_t ports;
  bool multicast = false;
  std::map<uint64_t, std::vector<uint64_t>> mcast_tbl;
  std::vector<LinkEndPtr> in, out;

  // Step 独占。
  std::vector<std::deque<FlitView>> out_q;
  std::vector<uint64_t> lane_rr;
  uint64_t accepted_pending = 0, forwarded_pending = 0, stalled_pending = 0;

  // 跨拍可读。Logic64 不能默认构造，端口数是运行期定的，所以挂 shared_ptr。
  std::shared_ptr<Logic64> accepted, forwarded, stalled;
};

}  // namespace bach
}  // namespace latch

#endif
