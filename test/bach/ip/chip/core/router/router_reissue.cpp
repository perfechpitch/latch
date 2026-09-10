// CoreMem 重发与 Retire 的回程。
//
// 重发这一段验三样：存进去的包打上重发标记、放出来时改回去；同一个 VC 只放
// 队头，别的 VC 不受影响；暂存区满了是配置错误，直接断言而不是丢包或改走
// 「留在当前 VC 等」。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/router/coremem_reissue.h"
#include "bach/ip/chip/core/router/retire.h"
#include "bach/ip/wiring.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

FlitView MakeFlit(uint64_t user, uint64_t path, uint64_t vc) {
  auto m = std::make_shared<Message>();
  m->user_id = user;
  m->path_id = path;
  m->size = 256;
  FlitView f;
  f.valid = true;
  f.vc = vc;
  f.head = true;
  f.tail = true;
  f.bytes = 256;
  f.msg = m;
  return f;
}

// 存包、唤醒、读重发出口，都在这一个协程里。
class ReissueHarness : public BachModule {
 public:
  struct StoreJob {
    uint64_t at = 0;
    FlitView f;
  };
  struct WakeJob {
    uint64_t at = 0;
    uint64_t vc = 0;
  };

  ReissueHarness(ClockPtr c, CoreMemReissue& target, LinkEndPtr out_wire)
      : BachModule(c, "harness"), rq(target), out(std::move(out_wire)) {}

  std::vector<StoreJob> stores;
  std::vector<WakeJob> wakes;

  std::vector<uint64_t> out_users, out_paths, out_vcs, out_reissue;
  // 重发完成报给 TS 的那一路。
  std::vector<std::pair<uint64_t, uint64_t>> done_reports;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    FlitView f = ReadFlit(out->flit);
    if (f.valid && f.msg) {
      out_users.push_back(f.msg->user_id);
      out_paths.push_back(f.msg->path_id);
      out_vcs.push_back(f.vc);
      out_reissue.push_back(f.msg->reissue);
    }
    if (rq.Done().Valid() && rq.Done().Seq() != last_done_seq) {
      last_done_seq = rq.Done().Seq();
      done_reports.push_back({rq.Done().User(), rq.Done().Path()});
    }
    for (auto const& s : stores) {
      if (s.at == now) rq.Store(s.f);
    }
    for (auto const& w : wakes) {
      if (w.at == now) rq.Wake(w.vc);
    }
    rq.RunStep();
  }

 private:
  CoreMemReissue& rq;
  LinkEndPtr out;
  uint64_t last_done_seq = 0;
};

}  // namespace

// 存的时候打上重发标记，放出来时改回去：Output Port 认这个标记决定扣不扣 credit。
TEST(BachReissue, ReinjectFlagIsSetOnStoreAndClearedOnReissue) {
  std::vector<uint64_t> out_flags, out_paths;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreMemReissue rq(clk, "rq", 4, 0, false);
    LinkEndPtr out = MakeWire(clk);
    rq.AttachOut(out);
    ReissueHarness h(clk, rq, out);
    h.stores = {{2, MakeFlit(31, 7, 1)}};
    h.wakes = {{10, 1}};
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    out_flags = h.out_reissue;
    out_paths = h.out_paths;
  }
  RT::Reset();
  ASSERT_EQ(out_flags.size(), 1u);
  EXPECT_EQ(out_flags[0], 0u) << "重发时标记改回 0";
  EXPECT_EQ(out_paths[0], 7u) << "只存包，PathID 原样带着，重发时重新查表";
}

// 唤醒一个 VC 只放它的队头，同 VC 的后续包不得越过；别的 VC 不受影响。
TEST(BachReissue, WakeReleasesOnlyTheHeadOfThatVc) {
  std::vector<uint64_t> users;
  uint64_t pending_vc1 = 0, pending_vc2 = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreMemReissue rq(clk, "rq", 4, 0, false);
    LinkEndPtr out = MakeWire(clk);
    rq.AttachOut(out);
    ReissueHarness h(clk, rq, out);
    h.stores = {{2, MakeFlit(41, 7, 1)},
                {3, MakeFlit(42, 7, 1)},
                {4, MakeFlit(43, 7, 2)}};
    h.wakes = {{10, 1}};
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    users = h.out_users;
    pending_vc1 = rq.Pending(1);
    pending_vc2 = rq.Pending(2);
  }
  RT::Reset();
  ASSERT_EQ(users.size(), 1u) << "一次唤醒只放一个";
  EXPECT_EQ(users[0], 41u) << "放的是队头，先存的先出";
  EXPECT_EQ(pending_vc1, 1u) << "同 VC 的第二笔还压着";
  EXPECT_EQ(pending_vc2, 1u) << "没唤醒的 VC 一笔没动";
}

// 暂存区满了是配置错误：不覆盖已暂存的包，不丢包，也不退回「留在当前 VC 等」。
TEST(BachReissue, FullBufferIsAConfigurationError) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreMemReissue rq(clk, "rq", 2, 0, false);
  rq.Store(MakeFlit(51, 7, 0));
  rq.Store(MakeFlit(52, 7, 0));
  EXPECT_EQ(rq.Pending(0), 2u);
  EXPECT_DEATH(rq.Store(MakeFlit(53, 7, 0)), "");
  RT::Reset();
}

// 不派角色的 core 不接收溢流：Router 对它不发起进 core 缓存处理。
TEST(BachReissue, PassThroughCoreTakesNoOverflow) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreMemReissue rq(clk, "rq", 4, 0, false);
  rq.SetPassThrough(true);
  EXPECT_DEATH(rq.Store(MakeFlit(61, 7, 0)), "");
  RT::Reset();
}


// 重发完成后向 TS 报一笔，至少带 UserID 与 PathID：直接发送那一路由 CoreStation
// 的 trigger 通知 TS，走了暂存的这一路要在这里补上。
TEST(BachReissue, ReportsToTsAfterReissue) {
  std::vector<std::pair<uint64_t, uint64_t>> reports;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreMemReissue rq(clk, "rq", 4, 0, false);
    LinkEndPtr out = MakeWire(clk);
    rq.AttachOut(out);
    ReissueHarness h(clk, rq, out);
    h.stores = {{2, MakeFlit(71, 9, 1)}};
    h.wakes = {{10, 1}};
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    reports = h.done_reports;
  }
  RT::Reset();
  ASSERT_EQ(reports.size(), 1u) << "重发完了要报一笔";
  EXPECT_EQ(reports[0].first, 71u) << "带 UserID";
  EXPECT_EQ(reports[0].second, 9u) << "带 PathID";
}

// 没重发就不报：存进去那一刻不算完成。
TEST(BachReissue, NoReportBeforeTheReissue) {
  uint64_t reports = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreMemReissue rq(clk, "rq", 4, 0, false);
    LinkEndPtr out = MakeWire(clk);
    rq.AttachOut(out);
    ReissueHarness h(clk, rq, out);
    h.stores = {{2, MakeFlit(71, 9, 1)}};
    // 不唤醒，包一直压在暂存区里。
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    reports = h.done_reports.size();
  }
  RT::Reset();
  EXPECT_EQ(reports, 0u);
}

namespace {

// 推 Retire 并读三个方向回程线。
class RetireHarness : public BachModule {
 public:
  RetireHarness(ClockPtr c, Retire& target, std::vector<LinkEndPtr> up)
      : BachModule(c, "harness"), rt(target), wires(std::move(up)) {}

  uint64_t relay_at = 0, relay_from = 0, relay_user = 0;
  std::vector<uint64_t> got_dirs, got_users;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    for (uint64_t d = 0; d < wires.size(); ++d) {
      ReleaseView r = ReadRelease(wires[d]->release);
      if (r.stream_valid) {
        got_dirs.push_back(d);
        got_users.push_back(r.stream_user);
      }
    }
    if (relay_at != 0 && now == relay_at) {
      rt.RelayRelease(relay_from, relay_user);
    }
    rt.Req().Idle();
    rt.RunStep();
  }

 private:
  Retire& rt;
  std::vector<LinkEndPtr> wires;
};

}  // namespace

// stream release 逐跳往上游传：发往除来向以外的另两个 R2R 口。
TEST(BachRetire, ReleaseGoesToTheOtherTwoDirections) {
  std::vector<uint64_t> dirs, users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Retire rt(clk, "retire", 0, false);
    std::vector<LinkEndPtr> up;
    for (uint64_t d = 0; d < kR2RNum; ++d) {
      up.push_back(MakeWire(clk));
      rt.AttachUpLink(d, up.back());
    }
    RetireHarness h(clk, rt, up);
    h.relay_at = 3;
    h.relay_from = kDirLeft;
    h.relay_user = 88;
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    dirs = h.got_dirs;
    users = h.got_users;
  }
  RT::Reset();
  ASSERT_EQ(dirs.size(), 2u) << "三个方向里除来向以外的两个";
  EXPECT_EQ(dirs[0], uint64_t(kDirMid));
  EXPECT_EQ(dirs[1], uint64_t(kDirRight));
  EXPECT_EQ(users[0], 88u);
  EXPECT_EQ(users[1], 88u);
}
