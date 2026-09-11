// Commit：两个任务入口在这里汇成同一套内部任务模型。
//
// 三样一起拿才接纳；出核任务先在 PendingTaskQ 等 credit，够了才来申请那三样；
// 两个入口竞争时先配 Router 那一侧；出核要发的那个包在这里造好。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/dte/commit.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

std::shared_ptr<Descriptor> Inbound(uint64_t user, uint64_t path,
                                    uint64_t bytes) {
  auto d = std::make_shared<Descriptor>();
  d->valid = true;
  d->route = Route::kRouterToCm;
  d->user_id = user;
  d->path_id = path;
  d->stream_id = 0;
  d->task_id = 1;
  d->bytes = bytes;
  auto m = std::make_shared<Message>();
  m->user_id = user;
  m->path_id = path;
  m->size = bytes;
  d->msg = m;
  return d;
}

std::shared_ptr<Descriptor> Outbound(uint64_t user, uint64_t path,
                                     uint64_t bytes, uint64_t vc = 0) {
  auto d = std::make_shared<Descriptor>();
  d->valid = true;
  d->route = Route::kCmToRouter;
  d->user_id = user;
  d->path_id = path;
  d->stream_id = 1;
  d->task_id = 2;
  d->bytes = bytes;
  d->vc = vc;
  return d;
}

RouteEntry Forward(uint64_t flow) {
  RouteEntry e;
  e.flow_dir = flow;
  e.operation = Operation::kForward;
  return e;
}

RouteEntry Reduce(uint64_t flow) {
  RouteEntry e;
  e.flow_dir = flow;
  e.op_type = OpType::kReduce;
  e.operation = Operation::kReduce1;
  e.reduce_in_mask = 0b011;
  return e;
}

// 两个入口灌任务、扮演各 Lane 与 Completion RS 收准入。
class CommitHarness : public BachModule {
 public:
  struct Job {
    uint64_t at = 0;
    bool from_rv = false;
    std::shared_ptr<Descriptor> d;
  };

  CommitHarness(ClockPtr c, Commit& target,
                std::vector<std::shared_ptr<AdmitPort>> lanes,
                std::shared_ptr<AdmitPort> rs)
      : BachModule(c, "harness"), cm(target), lane_ports(std::move(lanes)),
        rs_port(std::move(rs)) {}

  std::vector<Job> jobs;
  // 这一拍之前 Lane 与 RS 都说收不下。
  uint64_t lane_ready_from = 0, rs_ready_from = 0;

  struct Got {
    uint64_t at = 0, lane = 0, commit_seq = 0, user = 0;
    MessagePtr msg;
  };
  std::vector<Got> admits;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做：先看上一拍准入的那一笔。
    for (uint64_t i = 0; i < lane_ports.size(); ++i) {
      if (!lane_ports[i]->Valid()) continue;
      uint64_t seq = lane_ports[i]->Seq();
      if (seq == last_seq[i]) continue;
      last_seq[i] = seq;
      auto d = lane_ports[i]->Desc();
      if (d) admits.push_back({now, i, d->commit_seq, d->user_id, d->msg});
    }
    for (auto& p : lane_ports) p->DriveReady(now >= lane_ready_from);
    rs_port->DriveReady(now >= rs_ready_from);

    // 两个入口都保持到被收下：等对方收下的那几拍要一直发同一个号，每拍换号
    // 的话接收方按序号去重就把同一笔认成好几笔。
    Feed(*cm.ParserPortPtr(), parser_hold, false, now);
    Feed(cm.FromRv(), rv_hold, true, now);

    cm.RunStep();
  }

 private:
  struct Hold {
    std::shared_ptr<Descriptor> d;
    uint64_t seq = 0;
    bool active = false;
    uint64_t cursor = 0;
  };

  void Feed(DescPort& port, Hold& h, bool from_rv, uint64_t now) {
    if (h.active && port.Accepted()) h.active = false;
    if (!h.active) {
      for (uint64_t i = h.cursor; i < jobs.size(); ++i) {
        if (jobs[i].from_rv != from_rv || jobs[i].at > now) continue;
        h.d = jobs[i].d;
        h.seq = port.NextSeq();
        h.active = true;
        h.cursor = i + 1;
        break;
      }
    }
    if (h.active) {
      port.Drive(h.d, h.seq);
    } else {
      port.Idle();
    }
  }

  Commit& cm;
  std::vector<std::shared_ptr<AdmitPort>> lane_ports;
  std::shared_ptr<AdmitPort> rs_port;
  std::array<uint64_t, 8> last_seq{};
  Hold parser_hold, rv_hold;
};

struct Bench {
  ClockPtr clk;
  std::unique_ptr<Hmem> hmem;
  std::unique_ptr<Commit> commit;
  std::vector<std::shared_ptr<AdmitPort>> lanes;
  std::shared_ptr<AdmitPort> rs;

  explicit Bench(ClockPtr c) : clk(c) {
    hmem = std::make_unique<Hmem>(c, "hmem", 0, false);
    commit = std::make_unique<Commit>(c, "commit", *hmem, 0, false);
    for (uint64_t i = 0; i < kLaneNum; ++i) {
      lanes.push_back(std::make_shared<AdmitPort>(c));
      commit->AddLanePort(lanes.back());
    }
    rs = commit->RsPortPtr();
  }
};

}  // namespace

// 三样一起拿才接纳：Completion RS 没有空位时整体等，不产生半任务。
TEST(BachDteCommit, NoAdmitUntilAllThreeAreThere) {
  uint64_t before = 0, after = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.hmem->PreloadRtab(7, Forward(kFlowRight));
    CommitHarness h(clk, *b.commit, b.lanes, b.rs);
    h.jobs = {{2, false, Inbound(41, 7, 512)}};
    h.rs_ready_from = 20;  // Completion RS 到第 20 拍才有位置
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    for (auto const& a : h.admits) {
      if (a.at <= 20) ++before;
      ++after;
    }
  }
  RT::Reset();
  EXPECT_EQ(before, 0u) << "三样没齐之前一笔都不放";
  EXPECT_EQ(after, 1u) << "齐了之后放行";
}

// 两个入口同一拍来时先配 Router 那一侧。
TEST(BachDteCommit, RouterSideGoesFirstWhenBothCompete) {
  std::vector<uint64_t> users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.hmem->PreloadRtab(7, Forward(kFlowRight));
    b.hmem->PreloadRtab(8, Forward(kFlowRight));
    CommitHarness h(clk, *b.commit, b.lanes, b.rs);
    // RV core 那一侧先来一拍，Router 那一侧紧接着；两笔在同一拍上竞争准入。
    h.jobs = {{2, /*from_rv=*/true, Outbound(52, 8, 256)},
              {3, /*from_rv=*/false, Inbound(41, 7, 512)}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    for (auto const& a : h.admits) users.push_back(a.user);
  }
  RT::Reset();
  ASSERT_EQ(users.size(), 2u);
  EXPECT_EQ(users[0], 41u) << "竞争时先配 Router 的那一笔";
  EXPECT_EQ(users[1], 52u);
}

// Reduce 包与其他出核包一样只看 VC credit：本级 Rmem 资源由 TS 在下发前申请，
// DTE 这一侧不另记 credit。
TEST(BachDteCommit, ReducePacketNeedsOnlyVcCredit) {
  uint64_t admitted = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.hmem->PreloadRtab(9, Reduce(kFlowRight));
    CommitHarness h(clk, *b.commit, b.lanes, b.rs);
    h.jobs = {{2, true, Outbound(52, 9, 1024)}};
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    admitted = h.admits.size();
  }
  RT::Reset();
  EXPECT_EQ(admitted, 1u) << "VC 通路收得下就放行";
}

// PendingTaskQ 满了只反压出核这条链，进核那一路照走。
TEST(BachDteCommit, FullPendingQueueOnlyStallsOutbound) {
  uint64_t inbound_admits = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.hmem->PreloadRtab(7, Forward(kFlowRight));
    b.hmem->PreloadRtab(9, Forward(kFlowRight));
    // 往右那条 VC 通路一个空位都没有：电平口不驱，读出来全是 0，出核那几笔一直
    // 堵在 PendingTaskQ。
    auto level = std::make_shared<CreditLevelPort>(clk);
    b.commit->AttachVcLevel(level);
    CommitHarness h(clk, *b.commit, b.lanes, b.rs);
    // 先把 PendingTaskQ 灌满。
    for (uint64_t i = 0; i < kPendingTaskQDepth + 2; ++i) {
      h.jobs.push_back({2 + i * 2, true, Outbound(60 + i, 9, 256)});
    }
    // 再从 Router 那一侧送一笔进核任务。
    h.jobs.push_back({2 + (kPendingTaskQDepth + 3) * 2, false,
                      Inbound(41, 7, 512)});
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    for (auto const& a : h.admits) {
      if (a.user == 41u) ++inbound_admits;
    }
  }
  RT::Reset();
  EXPECT_EQ(inbound_admits, 1u) << "出核那条链堵住不影响进核";
}

// 出核要发的那个包在 Commit 建好：路由号与长度按这一笔的配置写进包头。
TEST(BachDteCommit, OutboundMessageIsBuiltAtCommit) {
  MessagePtr msg;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.hmem->PreloadRtab(8, Forward(kFlowRight));
    CommitHarness h(clk, *b.commit, b.lanes, b.rs);
    h.jobs = {{2, true, Outbound(52, 8, 640)}};
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    if (!h.admits.empty()) msg = h.admits.front().msg;
  }
  RT::Reset();
  ASSERT_TRUE(msg) << "出核任务进来时没有包，Commit 要把它造出来";
  EXPECT_EQ(msg->path_id, 8u) << "path_id 用 TS 送来的那个";
  EXPECT_EQ(msg->size, 640u) << "size 用 RV core 配的";
  EXPECT_EQ(msg->user_id, 52u);
  EXPECT_EQ(msg->payload.size(), 640u) << "payload 的位置先留出来";
}

// 内部序号一笔一个：两个入口来的任务在这里汇成同一套编号。
TEST(BachDteCommit, CommitSeqIsUniqueAcrossBothEntries) {
  std::vector<uint64_t> seqs;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.hmem->PreloadRtab(7, Forward(kFlowRight));
    b.hmem->PreloadRtab(8, Forward(kFlowRight));
    CommitHarness h(clk, *b.commit, b.lanes, b.rs);
    h.jobs = {{2, false, Inbound(41, 7, 256)},
              {6, true, Outbound(52, 8, 256)},
              {10, false, Inbound(42, 7, 256)}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    for (auto const& a : h.admits) seqs.push_back(a.commit_seq);
  }
  RT::Reset();
  ASSERT_EQ(seqs.size(), 3u);
  EXPECT_NE(seqs[0], seqs[1]);
  EXPECT_NE(seqs[1], seqs[2]);
  EXPECT_NE(seqs[0], seqs[2]);
}
