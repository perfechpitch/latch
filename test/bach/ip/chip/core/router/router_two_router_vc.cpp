// 两个 Router 串联时的四 VC 满载场景。
//
// router0 的 left 入口先按 VC 各攒满一个 20-flit 完整包，再开放路由；四个包从
// router0.right 发到 router1.left，并保持 VC0->VC0、...、VC3->VC3。router1
// 不配置后续路由，因此收到的 80 个 flit 全部留在 left 的 VC Buffer 中。

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/router/router.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;
constexpr uint64_t kLeft = 1;
constexpr uint64_t kRight = 2;
constexpr uint64_t kPacketFlits = 20;
constexpr uint64_t kFirstPath = 16;
constexpr uint64_t kOpenRouteAt = 90;
constexpr uint64_t kRunCycles = 240;

void EnsureSlots() { RT::Reset(8, 4); }

MessagePtr MakePacket(uint64_t vc) {
  auto msg = std::make_shared<Message>();
  msg->path_id = kFirstPath + vc;
  msg->user_id = 100 + vc;
  msg->size = kPacketFlits * kFlitBytes;
  msg->vc = vc;
  return msg;
}

// Tracetto/Perfetto 按 chipN.coreM.router.* 识别 Router 轨道，并用 core 直属信号
// 确定整份波形的结束拍。完整 Core 装配里 TS 会提供 ts_inflight；这个 Router
// 独立用例没有 TS，因此补一个同名时长锚点，不参与数据通路。
class CoreTraceAnchor : public BachModule {
 public:
  CoreTraceAnchor(ClockPtr clock, const std::string& name, uint64_t parent)
      : BachModule(clock, name, parent) {}

 protected:
  void Step() override {
    TracePerCycle("ts_inflight", CycleNow() < kRunCycles ? 1u : 0u);
  }
};

class FourVcPusher : public BachModule {
 public:
  FourVcPusher(ClockPtr clock, LinkEndPtr port)
      : BachModule(clock, "four_vc_pusher"), sink(std::move(port)) {
    for (uint64_t vc = 0; vc < kVcNum; ++vc) packets[vc] = MakePacket(vc);
  }

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    if (now < 1 || now > kVcNum * kPacketFlits) {
      sink->flit.Idle();
      return;
    }
    uint64_t index = now - 1;
    uint64_t vc = index % kVcNum;
    uint64_t beat = index / kVcNum;
    sink->flit.Drive(vc, beat == 0, beat + 1 == kPacketFlits, kFlitBytes,
                     packets[vc]);
  }

 private:
  LinkEndPtr sink;
  std::array<MessagePtr, kVcNum> packets;
};

class CreditCasePusher : public BachModule {
 public:
  CreditCasePusher(ClockPtr clock, LinkEndPtr port)
      : BachModule(clock, "credit_case_pusher"), sink(std::move(port)) {
    packets[1] = MakePacket(1);
    packets[1]->size = 4 * kFlitBytes;
    packets[2] = MakePacket(2);
    packets[2]->size = 3 * kFlitBytes;
  }

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    if (now >= 1 && now <= 4) {
      uint64_t beat = now - 1;
      sink->flit.Drive(1, beat == 0, beat == 3, kFlitBytes, packets[1]);
    } else if (now >= 5 && now <= 7) {
      uint64_t beat = now - 5;
      sink->flit.Drive(2, beat == 0, beat == 2, kFlitBytes, packets[2]);
    } else {
      sink->flit.Idle();
    }
  }

 private:
  LinkEndPtr sink;
  std::array<MessagePtr, kVcNum> packets;
};

class RouteOpener : public BachModule {
 public:
  RouteOpener(ClockPtr clock, Router& router, uint64_t open_at)
      : BachModule(clock, "route_opener"), target(router), at(open_at) {}

  uint64_t queued_before_open = 0;

 protected:
  void Step() override {
    if (CycleNow() != at) return;
    queued_before_open = target.Station(kLeft).QueueDepth();
    for (uint64_t vc = 0; vc < kVcNum; ++vc) {
      RouteEntry entry;
      entry.flow_dir = kFlowRight;
      entry.path_core_bypass = true;
      entry.operation = Operation::kForward;
      entry.nxt_vc[kRight] = vc;
      target.Preload(kFirstPath + vc, entry);
    }
  }

 private:
  Router& target;
  uint64_t at = 0;
};

class RouterLink : public BachModule {
 public:
  RouterLink(ClockPtr clock, Router& upstream, Router& downstream)
      : BachModule(clock, "router_link"),
        out(upstream.OutWire(kRight)),
        back(upstream.BackWire(kRight)),
        in(downstream.InWire(kLeft)),
        up_back(downstream.UpBackWire(kLeft)) {}

  std::array<uint64_t, kVcNum> delivered{};
  std::array<uint64_t, kVcNum> heads{};
  std::array<uint64_t, kVcNum> tails{};
  std::vector<uint64_t> order;
  bool vc_mismatch = false;

 protected:
  void Step() override {
    FlitView flit = ReadFlit(out->flit);
    if (flit.valid) {
      in->flit.Drive(flit.vc, flit.head, flit.tail, flit.bytes, flit.msg);
      if (flit.vc >= kVcNum) {
        vc_mismatch = true;
      } else {
        ++delivered[flit.vc];
        order.push_back(flit.vc);
        heads[flit.vc] += flit.head ? 1 : 0;
        tails[flit.vc] += flit.tail ? 1 : 0;
        if (!flit.msg || flit.msg->vc != flit.vc ||
            flit.msg->path_id != kFirstPath + flit.vc) {
          vc_mismatch = true;
        }
      }
    } else {
      in->flit.Idle();
    }
    in->release.Idle();

    ReleaseView release = ReadRelease(up_back->release);
    back->flit.Idle();
    back->release.Drive(release.vc_valid, release.vc_id,
                        release.stream_valid, release.stream_user);
  }

 private:
  LinkEndPtr out;
  LinkEndPtr back;
  LinkEndPtr in;
  LinkEndPtr up_back;
};

}  // namespace

TEST(BachRouterTwoRouterVc, FourFullPacketsKeepTheirVcAcrossOneHop) {
  uint64_t queued_before_open = 0;
  uint64_t router0_left_remaining = 0;
  uint64_t router1_left_queued = 0;
  uint64_t sent = 0;
  uint64_t shared_credit = 0;
  std::array<uint64_t, kVcNum> delivered{};
  std::array<uint64_t, kVcNum> heads{};
  std::array<uint64_t, kVcNum> tails{};
  std::array<uint64_t, kVcNum> private_credit{};
  std::vector<uint64_t> order;
  bool vc_mismatch = false;
  {
    EnsureSlots();
    RT::GetRecorder().StartNew("router_two_router_vc");
    ClockPtr clock = MakeClock(0, kPeriod);
    uint64_t chip = TraceGroup("chip0", 0);
    CoreTraceAnchor core0(clock, "core0", chip);
    CoreTraceAnchor core1(clock, "core1", chip);
    Router router0(clock, "router", RouterCfg{}, core0.Id());
    Router router1(clock, "router", RouterCfg{}, core1.Id());
    FourVcPusher pusher(clock, router0.InWire(kLeft));
    RouteOpener opener(clock, router0, kOpenRouteAt);
    RouterLink link(clock, router0, router1);

    clock->Continue(kRunCycles * kPeriod);
    RT::JoinAll();

    queued_before_open = opener.queued_before_open;
    router0_left_remaining = router0.Station(kLeft).QueueDepth();
    router1_left_queued = router1.Station(kLeft).QueueDepth();
    sent = router0.GetXbar().SentCount(kRight);
    shared_credit = router0.GetXbar().SharedCredit(kRight);
    delivered = link.delivered;
    heads = link.heads;
    tails = link.tails;
    order = link.order;
    vc_mismatch = link.vc_mismatch;
    for (uint64_t vc = 0; vc < kVcNum; ++vc) {
      private_credit[vc] = router0.GetXbar().VcCredit(kRight, vc);
    }
  }
  RT::FlushRecorder();
  RT::Reset();

  EXPECT_EQ(queued_before_open, kVcNum * kPacketFlits)
      << "开路由前，router0.left 的四个 VC 应各有一个完整 20-flit 包";
  EXPECT_EQ(router0_left_remaining, 0u);
  EXPECT_EQ(router1_left_queued, kVcNum * kPacketFlits);
  EXPECT_EQ(sent, kVcNum * kPacketFlits);
  EXPECT_FALSE(vc_mismatch);
  ASSERT_EQ(order.size(), kVcNum * kPacketFlits);
  for (uint64_t vc = 0; vc < kVcNum; ++vc) {
    EXPECT_EQ(delivered[vc], kPacketFlits) << "VC" << vc;
    EXPECT_EQ(heads[vc], 1u) << "VC" << vc;
    EXPECT_EQ(tails[vc], 1u) << "VC" << vc;
    EXPECT_EQ(private_credit[vc], 0u) << "VC" << vc;
    uint64_t packet = (vc + kVcNum - 1) % kVcNum;
    for (uint64_t beat = 0; beat < kPacketFlits; ++beat) {
      EXPECT_EQ(order[packet * kPacketFlits + beat], vc)
          << "VC" << vc << " 应连续发送到 tail";
    }
  }
  EXPECT_EQ(shared_credit, kVcSharedDepth)
      << "每个 VC 恰好使用自己的 20 个 private credit";
}

TEST(BachRouterTwoRouterVc, CreditExhaustionMovesToNextVc) {
  std::vector<uint64_t> order;
  {
    EnsureSlots();
    RT::GetRecorder().StartNew("router_two_router_vc_credit");
    ClockPtr clock = MakeClock(0, kPeriod);
    RouterCfg cfg;
    cfg.vc_private_depth = {20, 2, 20, 20};
    cfg.vc_shared_depth = 0;
    Router router0(clock, "router0", cfg);
    Router router1(clock, "router1", cfg);
    router0.Station(kLeft).SetVcDepth({20, 4, 20, 20}, 0);
    CreditCasePusher pusher(clock, router0.InWire(kLeft));
    RouteOpener opener(clock, router0, 20);
    RouterLink link(clock, router0, router1);

    clock->Continue(80 * kPeriod);
    RT::JoinAll();
    order = link.order;
  }
  RT::FlushRecorder();
  RT::Reset();

  EXPECT_EQ(order, std::vector<uint64_t>({1, 1, 2, 2, 2}))
      << "VC1 发到 credit 耗尽后，应切到 VC2 并连续发到 tail";
}
