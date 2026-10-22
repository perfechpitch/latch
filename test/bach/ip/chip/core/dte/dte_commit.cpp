// Commit：所有任务（进核 + 出核）统一从 RV core 的配置入口来，进中央 TaskQueue
// （深度 16）再按通道资源 dispatch。
//
// 三样一起拿（Lane 读/写槽 + Completion RS）从 Fire 时刻移到 dispatch
// 时刻；链路 级 VC credit 不在这里查（由 CoreStation 的 DteReady 在 flit
// 层端到端反压兜住）； 不同通道的任务可乱序下发；中央 TaskQueue 满 16 反压 RV
// core；出核要发的那个包 在这里造好。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "bach/ip/chip/core/dte/commit.h"
#include "base/clock.h"
#include "base/runtime.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

// 进核任务由 RV core 配寄存器 + trigger 起，不带着 Router
// 送进来的原包（包头已由 Header Parser 落 Header Table），所以这里不给 msg。
std::shared_ptr<Descriptor> Inbound(uint64_t user, uint64_t path,
                                    uint64_t /*bytes*/) {
  auto d = std::make_shared<Descriptor>();
  d->valid = true;
  d->route = Route::kRouterToCm;
  d->user_id = user;
  d->path_id = path;
  d->stream_id = 0;
  d->task_id = 1;
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
  // 段 0 = 包头（计算 core 落 Hmem），出核造包时从 Hmem 读回 gpu_id /
  // token_id。
  d->seg[0].valid = true;
  d->seg[0].is_header = true;
  d->seg[0].src_kind = SegEndpoint::kHeader;
  // 段 1 = 数据，出核造包时按 PayloadBytes() 汇总长度。
  d->seg[1].valid = true;
  d->seg[1].src_kind = SegEndpoint::kCmem;
  d->seg[1].len = bytes;
  d->vc = vc;
  return d;
}

RouteEntry Forward(uint64_t flow) {
  RouteEntry e;
  e.flow_dir = flow;
  e.operation = Operation::kForward;
  return e;
}

// 单入口灌任务、扮演各 Lane 与 Completion RS 收 dispatch。
class CommitHarness : public BachModule {
public:
  struct Job {
    uint64_t at = 0;
    std::shared_ptr<Descriptor> d;
  };

  CommitHarness(ClockPtr c, Commit &target,
                std::vector<std::shared_ptr<AdmitPort>> lanes,
                std::shared_ptr<AdmitPort> rs)
      : BachModule(c, "harness"), cm(target), lane_ports(std::move(lanes)),
        rs_port(std::move(rs)) {}

  std::vector<Job> jobs;
  std::function<void(uint64_t)> control;
  // 这一拍之前 Lane 与 RS 都说收不下。
  uint64_t lane_ready_from = 0, rs_ready_from = 0;

  struct Got {
    uint64_t at = 0, lane = 0, commit_seq = 0, user = 0;
    MessagePtr msg;
    uint64_t decode_started = 0;
  };
  std::vector<Got> admits;
  // Commit 收下的 Descriptor 数（一个任务只数一次），中央 TaskQueue 深与峰值。
  uint64_t rv_accepted = 0, queued = 0, peak_queued = 0;

protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做：先看上一拍 dispatch 出的那一笔。
    for (uint64_t i = 0; i < lane_ports.size(); ++i) {
      if (!lane_ports[i]->Valid())
        continue;
      uint64_t seq = lane_ports[i]->Seq();
      if (seq == last_seq[i])
        continue;
      last_seq[i] = seq;
      auto d = lane_ports[i]->Desc();
      if (d)
        admits.push_back({now, i, d->commit_seq, d->user_id, d->msg, d->dispatch_cycle});
    }
    for (auto &p : lane_ports)
      p->DriveReady(now >= lane_ready_from);
    rs_port->DriveReady(now >= rs_ready_from);

    if (control)
      control(now);
    Feed(cm.FromRv(), rv_hold, now);
    cm.RunStep();

    if (cm.FromRv().Accepted())
      ++rv_accepted;
    queued = cm.Queued();
    if (queued > peak_queued)
      peak_queued = queued;
  }

private:
  struct Hold {
    std::shared_ptr<Descriptor> d;
    uint64_t seq = 0;
    bool active = false;
    uint64_t cursor = 0;
  };

  // 入口保持到被收下：等对方收下的那几拍要一直发同一个号，每拍换号的话接收方
  // 按序号去重就把同一笔认成好几笔。
  void Feed(DescPort &port, Hold &h, uint64_t now) {
    if (h.active && port.Accepted())
      h.active = false;
    if (!h.active) {
      for (uint64_t i = h.cursor; i < jobs.size(); ++i) {
        if (jobs[i].at > now)
          continue;
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

  Commit &cm;
  std::vector<std::shared_ptr<AdmitPort>> lane_ports;
  std::shared_ptr<AdmitPort> rs_port;
  std::array<uint64_t, 8> last_seq{};
  Hold rv_hold;
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
    commit->Resume();
  }
};

} // namespace

// 三样一起拿才 dispatch：Completion RS 没有空位时整体等，不产生半任务。
TEST(BachDteCommit, NoAdmitUntilAllThreeAreThere) {
  uint64_t before = 0, after = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.hmem->PreloadRtab(7, Forward(kFlowRight));
    CommitHarness h(clk, *b.commit, b.lanes, b.rs);
    h.jobs = {{2, Inbound(41, 7, 512)}};
    h.rs_ready_from = 20; // Completion RS 到第 20 拍才有位置
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    for (auto const &a : h.admits) {
      if (a.at <= 20)
        ++before;
      ++after;
    }
  }
  RT::Reset();
  EXPECT_EQ(before, 0u) << "三样没齐之前一笔都不放";
  EXPECT_EQ(after, 1u) << "齐了之后放行";
}

// 中央 TaskQueue 满 16 就反压 RV core：Lane 一直不放行，任务只进不 dispatch，
// 收满 16 笔之后第 17 笔就不收了。
TEST(BachDteCommit, CentralQueueFullBackpressuresRv) {
  uint64_t rv_accepted = 0, admits = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.hmem->PreloadRtab(8, Forward(kFlowRight));
    CommitHarness h(clk, *b.commit, b.lanes, b.rs);
    h.lane_ready_from =
        1000; // Lane 一直不放，任务只进中央 TaskQueue 不 dispatch
    h.rs_ready_from = 0;
    for (uint64_t i = 0; i < 20; ++i) {
      h.jobs.push_back({2 + i * 2, Outbound(60 + i, 8, 256)});
    }
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    rv_accepted = h.rv_accepted;
    admits = h.admits.size();
  }
  RT::Reset();
  EXPECT_EQ(admits, 0u) << "Lane 不放行，一笔都不 dispatch";
  EXPECT_EQ(rv_accepted, kCentralTaskQDepth)
      << "中央 TaskQueue 满 16 就反压，第 17 笔起不收";
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
    h.jobs = {{2, Outbound(52, 8, 640)}};
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    if (!h.admits.empty())
      msg = h.admits.front().msg;
  }
  RT::Reset();
  ASSERT_TRUE(msg) << "出核任务进来时没有包，Commit 要把它造出来";
  EXPECT_EQ(msg->path_id, 8u) << "path_id 用 TS 送来的那个";
  EXPECT_EQ(msg->size, 640u) << "size 用 RV core 配的";
  EXPECT_EQ(msg->user_id, 52u);
  EXPECT_EQ(msg->payload.size(), 640u) << "payload 的位置先留出来";
}

// 内部序号一笔一个：进核 + 出核任务在这里汇成同一套编号。
TEST(BachDteCommit, CommitSeqIsUniqueAcrossAllTasks) {
  std::vector<uint64_t> seqs;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.hmem->PreloadRtab(7, Forward(kFlowRight));
    b.hmem->PreloadRtab(8, Forward(kFlowRight));
    CommitHarness h(clk, *b.commit, b.lanes, b.rs);
    h.jobs = {{2, Inbound(41, 7, 256)},
              {6, Outbound(52, 8, 256)},
              {10, Inbound(42, 7, 256)}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    for (auto const &a : h.admits)
      seqs.push_back(a.commit_seq);
  }
  RT::Reset();
  ASSERT_EQ(seqs.size(), 3u);
  EXPECT_NE(seqs[0], seqs[1]);
  EXPECT_NE(seqs[1], seqs[2]);
  EXPECT_NE(seqs[0], seqs[2]);
}

TEST(BachDteCommit, DispatchKeepsEntriesUntilOrderedRetirement) {
  EnsureSlots();
  auto clock = MakeClock(0, kPeriod);
  Bench bench(clock);
  CommitHarness driver(clock, *bench.commit, bench.lanes, bench.rs);
  driver.jobs = {{2, Inbound(11, 0, 8)},
                 {4, Outbound(12, 0, 8, 1)},
                 {6, Outbound(13, 0, 8, 2)}};
  clock->Continue(30);
  RT::JoinAll();
  EXPECT_EQ(driver.admits.size(), 3u);
  EXPECT_EQ(bench.commit->Occupied(), 3u);
  EXPECT_EQ(bench.commit->ReadEntry(0x2000 + 88), 11u);
  EXPECT_EQ(bench.commit->ReadEntry(0x2100 + 88), 12u);
  EXPECT_FALSE(bench.commit->Quiescent());
  RT::Reset();
}

TEST(BachDteCommit, StopSingleStepAndCancelAreIndependentOfQueueAdmission) {
  EnsureSlots();
  auto clock = MakeClock(0, kPeriod);
  Bench bench(clock);
  bench.commit->Stop();
  CommitHarness driver(clock, *bench.commit, bench.lanes, bench.rs);
  driver.jobs = {{2, Inbound(11, 0, 8)}, {4, Outbound(12, 0, 8, 1)}};
  driver.control = [&](uint64_t now) {
    if (now == 10) {
      EXPECT_TRUE(driver.admits.empty());
      EXPECT_EQ(bench.commit->Occupied(), 2u);
      bench.commit->SingleStep();
    }
    if (now == 20) {
      EXPECT_EQ(driver.admits.size(), 1u);
      EXPECT_FALSE(bench.commit->Running());
      bench.commit->Cancel(1);
      bench.commit->Resume();
    }
  };
  clock->Continue(40);
  RT::JoinAll();
  EXPECT_EQ(driver.admits.size(), 1u);
  EXPECT_EQ(bench.commit->Occupied(), 2u);
  RT::Reset();
}

TEST(BachDteCommit, DecoderLatencyCountsSegmentsAndDoublesInnerWork) {
  for (bool inner : {false, true}) {
    EnsureSlots();
    {
      auto clk = MakeClock(0, kPeriod);
      Bench bench(clk);
      CommitHarness harness(clk, *bench.commit, bench.lanes, bench.rs);
      auto task = Outbound(1, 0, 128);
      if (inner) task->route = Route::kMmToCm;
      for (uint64_t i = 0; i < 4; ++i) {
        task->seg[i].valid = i != 0;
        task->seg[i].len = i ? 128 : 0;
      }
      harness.jobs = {{2, task}};
      clk->Continue(40 * kPeriod); RT::JoinAll();
      ASSERT_EQ(harness.admits.size(), 1u);
      auto const& got = harness.admits.front();
      // The observable port adds one registered edge after decoder completion.
      EXPECT_EQ(got.at - got.decode_started, (inner ? 2 * 3 + 2 : 3 + 2) + 1);
    }
    RT::Reset();
  }
}
