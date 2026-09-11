#ifndef _LATCH_BACH_IP_CHIP_CORE_ROUTER_RETIRE_
#define _LATCH_BACH_IP_CHIP_CORE_ROUTER_RETIRE_

// Retire：用户退休与 credit 返还。
//
// 三方时序是最容易实现错的一条：
//   Core 先保证不会再有该 UserID 的搬运（全部进核、出核数据搬完，且不再发起新的），
//   然后才发 Retire；
//   Router 收到后立即删掉该用户的全部 stream credit 授权表项；
//   ReduceModule 是延迟回收：先记下 Retire，待相邻下游各方向的 Reduce credit
//   全部恢复到分配数量后才删对应用户映射。
//
// 顺序反了会让还在路上的 credit 无处归还，或者让表项提前放给别的用户。
//
// stream credit 的回程：每个 Router 用一个组合逻辑的 crossbar 汇总本级 core 与
// 所有下级出口的 pulse 加 user，发往除来向外的另两个 R2R port，逐跳传到上游；
// 跨 chip 经 C2C Bridge 透传。

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/ip/chip/core/router/router_ports.h"
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
  Retire(ClockPtr clock, const std::string& name, uint64_t parent = 0,
         bool tick = true)
      : BachModule(clock, name, parent, tick),
        req(std::make_shared<RetirePort>(clock)),
        bcast(std::make_shared<RetireBroadcastPort>(clock)),
        broadcast(clock) {
    for (uint64_t d = 0; d < kR2RNum; ++d) {
      up_wire.push_back(std::make_shared<LinkEnd>(clock));
    }
  }

  RetirePort& Req() { return *req; }
  std::shared_ptr<RetirePort> ReqPtr() const { return req; }
  // 三个 R2R 方向的回程线，stream release 逐跳往上游传。
  LinkEnd& UpLink(uint64_t d) { return *up_wire.at(d); }
  void AttachUpLink(uint64_t d, LinkEndPtr wire) {
    up_wire.at(d) = std::move(wire);
  }

  // 广播给谁：Xbar 删 stream 授权，ReduceModule 记下待回收，CoreStation 放坑。
  // 一根线，三个接收方各自读。
  std::shared_ptr<RetireBroadcastPort> BroadcastPtr() const { return bcast; }

  // 本级 core 或下级出口来的 stream release，按来向以外的另两个方向转出去。
  void RelayRelease(uint64_t from_dir, uint64_t user) {
    relay_q.push_back({from_dir, user});
  }

  uint64_t Broadcast() const { return broadcast.Get(); }

  bool Quiescent() const override { return relay_q.empty(); }

 protected:
  void Step() override {
    bcast_driven = false;
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
    uint64_t from_dir = 0, user = 0;
  };

  void TakeRequest() {
    if (!req->Valid()) {
      req->DriveAccepted(false);
      return;
    }
    uint64_t user = req->user_id.Get();
    if (user == last_user && seen_once) {
      // 同一笔会连着两拍出现（TS 要等 accepted 打一拍才撤 valid）。
      req->DriveAccepted(true);
      return;
    }
    bcast->Drive(user, ++bcast_seq);
    bcast_driven = true;
    last_user = user;
    seen_once = true;
    ++broadcast_pending;
    req->DriveAccepted(true);
  }

  // 发往除来向外的另两个 R2R port。
  void RelayOne() {
    for (uint64_t d = 0; d < kR2RNum; ++d) {
      up_wire[d]->flit.Idle();
      up_wire[d]->release.Idle();
    }
    if (relay_q.empty()) return;
    Relay r = relay_q.front();
    relay_q.pop_front();
    for (uint64_t d = 0; d < kR2RNum; ++d) {
      if (d == r.from_dir) continue;
      up_wire[d]->release.Drive(false, 0, true, r.user, false, 0);
    }
  }

  std::shared_ptr<RetirePort> req;
  std::vector<LinkEndPtr> up_wire;
  std::shared_ptr<RetireBroadcastPort> bcast;
  uint64_t bcast_seq = 0;
  bool bcast_driven = false;

  // Step 独占。
  std::deque<Relay> relay_q;
  uint64_t last_user = 0;
  bool seen_once = false;
  uint64_t broadcast_pending = 0;

  Logic64 broadcast;
};

}  // namespace bach
}  // namespace latch

#endif
