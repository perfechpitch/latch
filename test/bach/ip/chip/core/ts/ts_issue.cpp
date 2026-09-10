// 三条发射通路的选取与握手。
//
// DTE 那一条要按 Reissue、年龄、DataIn 与 Generated 的规矩选；三条通路各自独立
// 打拍，同一拍可以并行下发三个 task；选中之后非抢占保持，直到 RV core 回 ACCEPT。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/ts/dte_arb.h"
#include "bach/ip/chip/core/ts/mu_vu_arb.h"
#include "bach/ip/chip/core/ts/stream_table.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

TaskEntry Step(SendUnit unit, uint64_t pc) {
  TaskEntry t;
  t.send_unit = unit;
  t.recv_unit = RecvUnit::kDsa;
  t.dsa_en = true;
  t.exe_mask = true;
  t.task_pc = pc;
  return t;
}

// 一个停在某一步、状态 READY 的用户。
std::shared_ptr<StreamWrite> Ready(uint64_t slot, uint64_t user, SendUnit unit,
                                   uint64_t task_id, bool reissue = false) {
  auto w = std::make_shared<StreamWrite>();
  w->valid = true;
  w->stream_id = slot;
  w->whole = true;
  w->entry.valid = true;
  w->entry.user_id = user;
  w->entry.user_id_vld = true;
  w->entry.compute = true;
  w->entry.task_id = task_id;
  w->entry.task_fsm = TaskFsm::kReady;
  w->entry.task_unit = unit;
  w->entry.task_recv = RecvUnit::kDsa;
  w->entry.task_dsa_en = true;
  w->entry.task_pc = 0x1000 + task_id;
  w->entry.is_reissue = reissue;
  return w;
}

// 表、三条通路、User_Match 与两侧的驱动都在这一个协程里。
class IssueBench : public BachModule {
 public:
  struct Trig {
    uint64_t at = 0, user = 0, path = 0;
  };
  struct Seed {
    uint64_t at = 0;
    std::shared_ptr<StreamWrite> w;
  };
  // 种子表项走 install 那个整项写口：create 那个留给 User_Match。

  IssueBench(ClockPtr c, CfgReg& reg, StreamTable& tab, UserMatch& match,
             DteArb& dte, UnitArb& mu, UnitArb& vu)
      : BachModule(c, "bench"), cfg(reg), table(tab), um(match), dte_arb(dte),
        mu_arb(mu), vu_arb(vu) {}

  std::vector<Seed> seeds;
  std::vector<Trig> trigs;
  // 这一拍之前三个 RV core 都不给 ACCEPT，用来把候选攒齐。
  uint64_t ready_from = 0;

  struct Got {
    uint64_t at = 0, stream = 0, task = 0, user = 0;
  };
  std::vector<Got> dte_log, mu_log, vu_log;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    bool ok = now >= ready_from;
    Take(dte_arb.Cmd(), dte_log, now, ok, dte_key);
    Take(mu_arb.Cmd(), mu_log, now, ok, mu_key);
    Take(vu_arb.Cmd(), vu_log, now, ok, vu_key);
    FeedTrigger(now);
    for (auto const& s : seeds) {
      if (s.at == now) table.Port(kWrInstall).Drive(s.w);
    }
    cfg.RunStep();
    dte_arb.RunStep();
    mu_arb.RunStep();
    vu_arb.RunStep();
    um.RunStep();
    table.RunStep();
  }

 private:
  // 扮演 RV core：这一拍收得下就记一笔并回 ready，否则拉低。
  void Take(TaskCmdPort& cmd, std::vector<Got>& log, uint64_t now, bool ok,
            uint64_t& last_key) {
    if (!cmd.Valid() || !ok) {
      cmd.DriveReady(false);
      return;
    }
    // 认「这是不是上一笔」要连 path_id 一起看：自启动 core 的 datain 没有
    // stream，几笔的 stream_id 与 task_id 都是 0，只有 path_id 分得开。
    uint64_t key = ((cmd.stream_id.Get() * kTaskChainNum + cmd.task_id.Get()) *
                        kTaskChainNum +
                    cmd.path_id.Get()) +
                   1;
    if (key != last_key) {
      log.push_back({now, cmd.stream_id.Get(), cmd.task_id.Get(),
                     cmd.user_id.Get()});
      last_key = key;
    }
    cmd.DriveReady(true);
  }

  // trigger 是 valid/ready 握手，发出后保持两拍再看 ready。
  void FeedTrigger(uint64_t now) {
    if (cursor >= trigs.size()) {
      um.Trigger().Idle();
      return;
    }
    Trig const& t = trigs[cursor];
    if (now < t.at) {
      um.Trigger().Idle();
      return;
    }
    if (sent_at != 0 && now >= sent_at + 2 && um.Trigger().Ready()) {
      ++cursor;
      sent_at = 0;
      um.Trigger().Idle();
      return;
    }
    um.Trigger().Drive(t.user, t.path, /*reissue=*/false, /*compute=*/true);
    if (sent_at == 0) sent_at = now;
  }

  CfgReg& cfg;
  StreamTable& table;
  UserMatch& um;
  DteArb& dte_arb;
  UnitArb& mu_arb;
  UnitArb& vu_arb;
  uint64_t cursor = 0, sent_at = 0;
  uint64_t dte_key = 0, mu_key = 0, vu_key = 0;
};

struct Bench {
  ClockPtr clk;
  std::unique_ptr<CfgReg> cfg;
  std::unique_ptr<StreamTable> table;
  std::unique_ptr<UserMatch> um;
  std::unique_ptr<DteArb> dte;
  std::unique_ptr<UnitArb> mu, vu;

  explicit Bench(ClockPtr c) : clk(c) {
    cfg = std::make_unique<CfgReg>(c, "cfg", 0, false);
    table = std::make_unique<StreamTable>(c, "table", 0, false);
    um = std::make_unique<UserMatch>(c, "um", *cfg, 0, false);
    dte = std::make_unique<DteArb>(c, "dte", *um, 0, false);
    mu = std::make_unique<UnitArb>(c, "mu", SendUnit::kMu, 0, false);
    vu = std::make_unique<UnitArb>(c, "vu", SendUnit::kVu, 0, false);
    auto snap = table->SnapPtr();
    um->AttachSnapshot(snap);
    dte->AttachSnapshot(snap);
    mu->AttachSnapshot(snap);
    vu->AttachSnapshot(snap);
    table->Rebind(kWrCreate, um->CreatePtr());
    table->Rebind(kWrIssueDte, dte->IssuePtr());
    table->Rebind(kWrIssueMu, mu->IssuePtr());
    table->Rebind(kWrIssueVu, vu->IssuePtr());
  }
};

}  // namespace

// R core 与 B core 上进来的包不建 stream 表项：只把 datain 任务登记进
// DataIn_task_table，PC 取 DATAIN_TASK 那一项，同一个用户来第二次照样登记。
TEST(BachTsIssue, SelfStartCoreDatainDoesNotCreateAStream) {
  std::vector<IssueBench::Got> d;
  uint64_t used = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    // R core：task 0 自启动跑在 MU 上，datain 单独配一个 DTE 的 pc。
    TaskEntry t0 = Step(SendUnit::kMu, 0x2000);
    t0.self_start = true;
    t0.dsa_en = false;
    b.cfg->WriteTask(0, t0);
    TaskEntry t1 = Step(SendUnit::kDte, 0x2100);
    t1.end = true;
    b.cfg->WriteTask(1, t1);
    b.cfg->SetCoreType(CoreType::kReduction);
    b.cfg->WriteDatainTask(0x3000, /*weights_mode=*/false);
    b.cfg->SetStreamNum(1);
    b.cfg->SetInitFinish();

    IssueBench h(clk, *b.cfg, *b.table, *b.um, *b.dte, *b.mu, *b.vu);
    // 同一个用户的两笔数据从两个方向来，各自都要被登记一次。
    h.trigs = {{4, 77, 1}, {20, 77, 2}};
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    d = h.dte_log;
    used = b.table->TailPtr() - b.table->HeadPtr();
  }
  RT::Reset();
  // 表里那一项是自启动补出来的，不是进来的包建的：包只登记 datain 任务。
  EXPECT_EQ(used, 1u);
  ASSERT_EQ(d.size(), 2u) << "同一个用户来两次要登记两次";
  for (auto const& one : d) {
    EXPECT_EQ(one.user, 77u);
    EXPECT_EQ(one.task, 0u);
  }
}

// 三条发射通路各自独立打拍：三个 unit 各有一个就绪的 task 时，同一拍都下发。
TEST(BachTsIssue, ThreeUnitsIssueInTheSameCycle) {
  std::vector<IssueBench::Got> d, m, v;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    IssueBench h(clk, *b.cfg, *b.table, *b.um, *b.dte, *b.mu, *b.vu);
    // 一个写口一拍只收一笔，所以三个用户分三拍建表。
    h.seeds = {{2, Ready(0, 11, SendUnit::kDte, 0)},
               {3, Ready(1, 12, SendUnit::kMu, 0)},
               {4, Ready(2, 13, SendUnit::kVu, 0)}};
    // 三个用户先建好表，第 20 拍三个 RV core 一起开始收。
    h.ready_from = 20;
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    d = h.dte_log;
    m = h.mu_log;
    v = h.vu_log;
  }
  RT::Reset();
  ASSERT_FALSE(d.empty());
  ASSERT_FALSE(m.empty());
  ASSERT_FALSE(v.empty());
  EXPECT_EQ(d[0].at, m[0].at) << "三条通路互不排队";
  EXPECT_EQ(m[0].at, v[0].at);
}

// Reissue 优先级最高：比它更老的普通任务也要让它先走。
TEST(BachTsIssue, ReissueGoesFirst) {
  std::vector<IssueBench::Got> d;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    IssueBench h(clk, *b.cfg, *b.table, *b.um, *b.dte, *b.mu, *b.vu);
    // stream0 先出现，会被先锁定；等它被收下之后，stream1（较老、普通）与
    // stream2（较新、重发）一起在候选里，这时候该轮到重发那个。
    h.seeds = {{2, Ready(0, 11, SendUnit::kDte, 0)},
               {3, Ready(1, 12, SendUnit::kDte, 0)},
               {4, Ready(2, 13, SendUnit::kDte, 0, /*reissue=*/true)}};
    h.ready_from = 20;
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    d = h.dte_log;
  }
  RT::Reset();
  ASSERT_GE(d.size(), 3u);
  EXPECT_EQ(d[0].user, 11u) << "先锁定的那一笔不被抢";
  EXPECT_EQ(d[1].user, 13u) << "重发的排在更老的普通任务前面";
  EXPECT_EQ(d[2].user, 12u);
}

// 同一个单元上有多个候选时选最老的那个 stream。
TEST(BachTsIssue, OldestStreamGoesFirst) {
  std::vector<IssueBench::Got> d;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    IssueBench h(clk, *b.cfg, *b.table, *b.um, *b.dte, *b.mu, *b.vu);
    h.seeds = {{2, Ready(0, 11, SendUnit::kDte, 0)},
               {3, Ready(1, 12, SendUnit::kDte, 0)},
               {4, Ready(2, 13, SendUnit::kDte, 0)}};
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    d = h.dte_log;
  }
  RT::Reset();
  ASSERT_GE(d.size(), 3u);
  EXPECT_EQ(d[0].user, 11u);
  EXPECT_EQ(d[1].user, 12u);
  EXPECT_EQ(d[2].user, 13u);
}

// 非抢占保持：选中之后命令与字段一直保持，直到 RV core 回 ACCEPT。
TEST(BachTsIssue, HeldCommandStaysUntilAccept) {
  std::vector<IssueBench::Got> d;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    IssueBench h(clk, *b.cfg, *b.table, *b.um, *b.dte, *b.mu, *b.vu);
    // 老的那个先被选中；RV core 三十拍里一直不收。这期间来了一个更该先走的
    // 重发任务，也不能把已经锁定的这一笔换掉。
    h.seeds = {{2, Ready(0, 11, SendUnit::kDte, 0)},
               {8, Ready(1, 12, SendUnit::kDte, 0, /*reissue=*/true)}};
    h.ready_from = 30;
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    d = h.dte_log;
  }
  RT::Reset();
  ASSERT_GE(d.size(), 2u);
  EXPECT_EQ(d[0].user, 11u) << "锁定的那一笔不被抢";
  EXPECT_GE(d[0].at, 30u) << "一直等到 RV core 收";
  EXPECT_EQ(d[1].user, 12u);
}

// DataIn 与 Generated 按年龄比：DataIn 那一格更老时先发它，不是一律垫底。
TEST(BachTsIssue, DataInGoesByAgeAgainstGenerated) {
  std::vector<IssueBench::Got> d;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    // 链：0 普通 DTE、1 datain、2 End。path 7 指向第 1 项。
    b.cfg->WriteTask(0, Step(SendUnit::kDte, 0x100));
    TaskEntry din = Step(SendUnit::kDte, 0x200);
    din.wait_wake = true;
    din.path_id = 7;
    b.cfg->WriteTask(1, din);
    TaskEntry last = Step(SendUnit::kDte, 0x300);
    last.end = true;
    b.cfg->WriteTask(2, last);
    b.cfg->WritePathMap(7, 1);
    b.cfg->WriteDatainTask(0x200, false);
    b.cfg->SetInitFinish();

    IssueBench h(clk, *b.cfg, *b.table, *b.um, *b.dte, *b.mu, *b.vu);
    // 用户 11 从 trigger 进来，占 stream0，同时把 DataIn 那一格填上。
    h.trigs = {{2, 11, 7}};
    // 之后 stream1 上摆一个就绪的普通任务，它比 DataIn 那一格新。
    h.seeds = {{20, Ready(1, 12, SendUnit::kDte, 0)}};
    // 两个用户都到位之后再放开，让 DataIn 那一格与 stream1 在同一拍上比年龄。
    h.ready_from = 30;
    clk->Continue(90 * kPeriod);
    RT::JoinAll();
    d = h.dte_log;
  }
  RT::Reset();
  ASSERT_GE(d.size(), 2u);
  // 第一笔是 stream0 自己的 task 0：同一个 stream 上 Generated 优先于 DataIn。
  EXPECT_EQ(d[0].stream, 0u);
  EXPECT_EQ(d[0].task, 0u) << "同一 Stream 时优先选 Generated";
  // 第二笔应当是那一格 DataIn（stream0，task 1），而不是更新的 stream1。
  EXPECT_EQ(d[1].stream, 0u) << "DataIn 那一格更老，排在 stream1 前面";
  EXPECT_EQ(d[1].task, 1u);
}
