// CoreMem 重发与 Retire 的回程。
//
// 重发这一段验三样：存进去的包打上重发标记、放出来时改回去；同一个 VC 只放
// 队头，别的 VC 不受影响；暂存区满了是配置错误，直接断言而不是丢包或改走
// 「留在当前 VC 等」。

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/router/coremem_reissue.h"
#include "bach/ip/chip/core/router/retire.h"
#include "bach/ip/chip/core/router/router_table.h"
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

// 坏 core 不接收溢流：Router 对它不发起进 core 缓存处理。透传档由 core_bad_mask
// 里本 core 那一位打开。
TEST(BachReissue, PassThroughCoreTakesNoOverflow) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  RouterTable rtab(clk, "rtab", 0, false);
  rtab.SetCoreBadMask(0x084);
  ASSERT_TRUE(rtab.CoreBad(7));
  CoreMemReissue rq(clk, "rq", 4, 0, false);
  rq.SetPassThrough(rtab.CoreBad(7));
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

// 推 Retire 与下游还回来的 release，读三个方向出线上的回程。
class RetireHarness : public BachModule {
 public:
  RetireHarness(ClockPtr c, Retire& target, std::vector<LinkEndPtr> up,
                std::vector<LinkEndPtr> down = {})
      : BachModule(c, "harness"), rt(target), wires(std::move(up)),
        down_wires(std::move(down)) {}

  // 下游还回来的那一笔：在这一拍从 down_from 方向的进线上推 down_user。
  uint64_t down_at = 0, down_from = 0, down_user = 0;
  // TS 那一侧的退休请求：在这几拍拉高 valid，带 req_user。
  std::set<uint64_t> req_at;
  uint64_t req_user = 0;
  std::vector<uint64_t> got_dirs, got_users;
  // 交给 Xbar 那一路：方向与用户。
  std::vector<uint64_t> self_dirs, self_users;
  // 广播口上出现过的退休用户，一笔记一次。
  std::vector<uint64_t> retired;

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
    auto rel = rt.ReleasePtr();
    if (rel->Valid() && rel->Seq() != last_rel_seq) {
      last_rel_seq = rel->Seq();
      self_dirs.push_back(rel->Dir());
      self_users.push_back(rel->User());
    }
    for (uint64_t d = 0; d < down_wires.size(); ++d) {
      if (down_at != 0 && now == down_at && d == down_from) {
        down_wires[d]->release.DriveStream(true, down_user);
      } else {
        down_wires[d]->release.DriveStream(false, 0);
      }
    }
    auto bcast = rt.BroadcastPtr();
    if (bcast->Valid() && bcast->Seq() != last_bcast_seq) {
      last_bcast_seq = bcast->Seq();
      retired.push_back(bcast->User());
    }
    if (req_at.count(now) != 0) {
      rt.Req().Drive(req_user);
    } else {
      rt.Req().Idle();
    }
    rt.RunStep();
  }

 private:
  Retire& rt;
  std::vector<LinkEndPtr> wires, down_wires;
  uint64_t last_bcast_seq = 0, last_rel_seq = 0;
};

// 一套接好的 Retire：三个方向的出线与进线各一根。
struct RetireBench {
  std::unique_ptr<RouterTable> tab;
  std::unique_ptr<Retire> rt;
  std::vector<LinkEndPtr> up, down;

  explicit RetireBench(ClockPtr clk) {
    tab = std::make_unique<RouterTable>(clk, "rtab", 0, false);
    rt = std::make_unique<Retire>(clk, "retire", *tab, 0, false);
    for (uint64_t d = 0; d < kR2RNum; ++d) {
      up.push_back(MakeWire(clk));
      down.push_back(MakeWire(clk));
      rt->AttachUpLink(d, up.back());
      rt->AttachDownLink(d, down.back());
    }
  }
};

}  // namespace

// 本级退休往三个 R2R 方向各发一笔：哪个上游申请过，删的就是它那一项，没申请过
// 的查无此项丢掉。
TEST(BachRetire, LocalRetireReleasesAllThreeDirections) {
  std::vector<uint64_t> dirs, users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RetireBench b(clk);
    RetireHarness h(clk, *b.rt, b.up, b.down);
    h.req_at = {3, 4};
    h.req_user = 88;
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    dirs = h.got_dirs;
    users = h.got_users;
  }
  RT::Reset();
  ASSERT_EQ(dirs.size(), 3u) << "三个方向各一笔";
  EXPECT_EQ(dirs, std::vector<uint64_t>({kDirMid, kDirLeft, kDirRight}));
  EXPECT_EQ(users, std::vector<uint64_t>({88, 88, 88}));
}

// 下游还回来的那一笔：交给本级删那个方向的表项，同时按那个口的 RTR_RELEASE_ROUTE
// 转出去。坏 core 上配的是左进转左、右进转右。
TEST(BachRetire, DownstreamReleaseGoesToTheLocalTableAndTheStaticRoute) {
  std::vector<uint64_t> dirs, users, self_dirs, self_users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RetireBench b(clk);
    b.tab->SetCreditBypass(kDirRight, kFlowLeft);
    RetireHarness h(clk, *b.rt, b.up, b.down);
    h.down_at = 3;
    h.down_from = kDirRight;
    h.down_user = 88;
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    dirs = h.got_dirs;
    users = h.got_users;
    self_dirs = h.self_dirs;
    self_users = h.self_users;
  }
  RT::Reset();
  ASSERT_EQ(self_dirs.size(), 1u);
  EXPECT_EQ(self_dirs[0], uint64_t(kDirRight)) << "删的是来向那个方向那一项";
  EXPECT_EQ(self_users[0], 88u);
  ASSERT_EQ(dirs.size(), 1u) << "掩码里只有左边那一位";
  EXPECT_EQ(dirs[0], uint64_t(kDirLeft));
  EXPECT_EQ(users[0], 88u);
}

// 同一个用户先后退休两次，各广播一次：一笔请求连着两拍拉高只算一笔，valid 掉下
// 去过再来的是新的一笔。同一个用户在一个 core 上先后跑两个 token 就是这样。
TEST(BachRetire, SameUserRetiringTwiceBroadcastsTwice) {
  std::vector<uint64_t> retired;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RetireBench b(clk);
    RetireHarness h(clk, *b.rt, b.up, b.down);
    h.req_at = {3, 4, 8, 9};
    h.req_user = 88;
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    retired = h.retired;
  }
  RT::Reset();
  EXPECT_EQ(retired, std::vector<uint64_t>({88, 88}));
}
