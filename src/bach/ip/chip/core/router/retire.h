#ifndef _LATCH_BACH_IP_CHIP_CORE_ROUTER_RETIRE_
#define _LATCH_BACH_IP_CHIP_CORE_ROUTER_RETIRE_

// Retire：用户退休与 stream credit 的回程。
//
// 本级退休（F-036）：Core 先保证不会再有这个 UserID 的搬运，全部进核、出核数据
// 搬完且不再发起新的，然后才发 Retire。收到后本级只放自己这一侧的坑：CoreStation
// 删进核那一项，ReduceModule 放这个用户的分区，本级记的各下游方向那几项不动，
// 它们要等那些下游各自退休时还回来。
//
// 退休的同时往三个 R2R 方向各发一笔 release，告诉上游「本 core 上这个用户跑完了」。
// 哪个上游向本 core 申请过，删的就是它那一项；没申请过的上游查无此项，丢掉。
//
// release 的回程与数据反向走：还给上游的写在往那个方向去的出线上，下游还回来的
// 从那个方向的进线上读，跨 chip 经 C2C Bridge 透传。
//
// 转发（F-018～F-020）：从某个方向进来的 release 按那个口的 RTR_RELEASE_ROUTE
// 转出去，UserID 不变。坏 core 靠这一项把 release 送过去，好 core 一律交给本级，
// 不配掩码。

#include <array>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/chip/core/router/router_table.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// TS → Retire：用户退休。
class RetirePort : public Logic {
 public:
  Logic64 valid, user_id, accepted;

  explicit RetirePort(ClockPtr c) : valid(c), user_id(c), accepted(c) {
    Fields(valid, user_id, accepted);
  }

  void Drive(uint64_t user) {
    valid = 1;
    user_id = user;
  }
  void Idle() {
    valid = 0;
    user_id = 0;
  }
  void DriveAccepted(bool ok) { accepted = ok ? 1 : 0; }
  bool Accepted() const { return accepted.Get() != 0; }
  bool Valid() const { return valid.Get() != 0; }
};

class Retire : public BachModule {
 public:
  Retire(ClockPtr clock, const std::string& name, RouterTable& table,
         uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        rtab(table),
        req(std::make_shared<RetirePort>(clock)),
        bcast(std::make_shared<RetireBroadcastPort>(clock)),
        rel_out(std::make_shared<StreamReleasePort>(clock)),
        broadcast(clock) {
    for (uint64_t d = 0; d < kR2RNum; ++d) {
      up_wire.push_back(std::make_shared<LinkEnd>(clock));
      down_wire.push_back(std::make_shared<LinkEnd>(clock));
    }
  }

  RetirePort& Req() { return *req; }
  std::shared_ptr<RetirePort> ReqPtr() const { return req; }
  // 往方向 d 去的那根线：还给那个上游的 release 写在它的 release 上，数据那一路
  // 由 Xbar 写，两者各写各的字段。
  LinkEnd& UpLink(uint64_t d) { return *up_wire.at(d); }
  void AttachUpLink(uint64_t d, LinkEndPtr wire) {
    up_wire.at(d) = std::move(wire);
  }
  // 从方向 d 来的那根线：那个下游还回来的 release 从它上面读。
  void AttachDownLink(uint64_t d, LinkEndPtr wire) {
    down_wire.at(d) = std::move(wire);
  }

  // 广播给谁：ReduceModule 记下待回收，CoreStation 放进核那一项。一根线，两个
  // 接收方各自读。
  std::shared_ptr<RetireBroadcastPort> BroadcastPtr() const { return bcast; }
  // 下游还回来的那一笔交给 Xbar：删它那个方向上这个用户的表项。
  std::shared_ptr<StreamReleasePort> ReleasePtr() const { return rel_out; }

  uint64_t Broadcast() const { return broadcast.Get(); }
  // 收到过几笔下游还回来的 release。
  uint64_t Returned() const { return returned; }

  bool Quiescent() const override { return relay_q.empty() && self_q.empty(); }

 protected:
  void Step() override {
    bcast_driven = false;
    TakeDownReleases();
    NotifyOne();
    RelayOne();
    TakeRequest();
    // 端口每拍必须驱动一次：一拍不写会回落成上一拍的值，接收方会把同一笔认
    // 两遍。这一拍没广播就显式拉低。
    if (!bcast_driven) bcast->Idle();
    broadcast = broadcast_pending;
    TracePerCycle("broadcast", broadcast_pending);
  }

 private:
  struct Relay {
    // 来向：kR2RNum 表示这一笔是本级退休发起的，三个方向都要发。
    uint64_t from_dir = 0, user = 0;
  };

  void TakeRequest() {
    if (!req->Valid()) {
      req->DriveAccepted(false);
      held = false;
      return;
    }
    uint64_t user = req->user_id.Get();
    if (held && user == last_user) {
      // 同一笔会连着两拍出现（TS 要等 accepted 打一拍才撤 valid）。valid 掉下
      // 去过再来的是新的一笔：同一个用户在本 core 上先后跑两个 token，就要退休
      // 两次。
      req->DriveAccepted(true);
      return;
    }
    bcast->Drive(user, ++bcast_seq);
    bcast_driven = true;
    last_user = user;
    held = true;
    ++broadcast_pending;
    // 本级这个用户跑完了，往三个方向各还一笔。
    relay_q.push_back({kR2RNum, user});
    req->DriveAccepted(true);
  }

  // 下游还回来的 release：交给本级删那个方向的表项，同时按那个口的静态路由转发。
  void TakeDownReleases() {
    for (uint64_t d = 0; d < kR2RNum; ++d) {
      if (!down_wire[d]) continue;
      ReleaseView r = ReadRelease(down_wire[d]->release);
      if (!r.stream_valid) continue;
      ++returned;
      self_q.push_back({d, r.stream_user});
      if (rtab.CreditBypass(d) & kFlowR2R) relay_q.push_back({d, r.stream_user});
    }
  }

  // 一拍交一笔给 Xbar。
  void NotifyOne() {
    if (self_q.empty()) {
      rel_out->Idle();
      return;
    }
    Relay r = self_q.front();
    self_q.pop_front();
    rel_out->Drive(r.from_dir, r.user, ++rel_seq);
  }

  // 本级退休那一笔发往三个方向，转发那几笔按 RTR_RELEASE_ROUTE 的出方向位走。
  // 数据那一路不碰：出线上的 flit 由 Xbar 写。
  void RelayOne() {
    // 上一拍没发的口这一拍不用写：端口一拍不写就保持上一拍的值，本来就是不发。
    // 一个 core 三个口、一拍六次写 Latch，整机跑下来是几亿次空写。
    for (uint64_t d = 0; d < kR2RNum; ++d) {
      if (!up_wire[d] || !drove[d]) continue;
      up_wire[d]->release.DriveStream(false, 0);
      drove[d] = false;
    }
    if (relay_q.empty()) return;
    Relay r = relay_q.front();
    relay_q.pop_front();
    uint64_t mask = r.from_dir == kR2RNum ? kFlowR2R
                                          : rtab.CreditBypass(r.from_dir) & kFlowR2R;
    for (uint64_t d = 0; d < kR2RNum; ++d) {
      if (((mask >> d) & 1u) == 0 || !up_wire[d]) continue;
      up_wire[d]->release.DriveStream(true, r.user);
      drove[d] = true;
    }
  }

  RouterTable& rtab;
  std::shared_ptr<RetirePort> req;
  std::vector<LinkEndPtr> up_wire, down_wire;
  std::shared_ptr<RetireBroadcastPort> bcast;
  std::shared_ptr<StreamReleasePort> rel_out;
  uint64_t bcast_seq = 0, rel_seq = 0, returned = 0;
  bool bcast_driven = false;

  // Step 独占。
  std::array<bool, kR2RNum> drove{};
  std::deque<Relay> relay_q, self_q;
  uint64_t last_user = 0;
  // 上一拍收下的那一笔还没撤 valid。
  bool held = false;
  uint64_t broadcast_pending = 0;

  Logic64 broadcast;
};

}  // namespace bach
}  // namespace latch

#endif
