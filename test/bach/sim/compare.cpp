// 与 Bach 实跑对表。
//
// 每份中间文件旁边有一份期望结果，它是 Bach 跑同一张 Map 得到的完成集合与丢包集合，
// 由 `src/bach/tools/run_bach.py` 产出。两边读的是同一张 Map 编译出来的同一份任务表，
// 所以这里比的是同一件事的两个实现。
//
// 比三样，由粗到细：
//
//   丢包集合  必须一模一样。这一层不允许有任何差异：丢了包就是模型错了
//   完成集合  按 uid 排序后必须一模一样
//   完成顺序  按完成先后排的那一串。两边的结束时刻本来就允许有差异，所以顺序不同
//             不算错，但要能看出来，测试把它单独报出来
//
// 每个 user 的端到端延迟只打出来，不作为判据。逐 ns 复刻 Bach 要在 latch 上复刻
// SimPy 的同刻定序，那不是这一层的目标；差值落进偏差清单。
//
// 比的是延迟而不是完成时刻：完成时刻里含着注入源按推包间隔排的那一段，第几个 user
// 就多几个间隔，那一段与模型跑得对不对无关。期望结果里那个 end_time 是 Bach 整个
// 仿真停下来的时刻，也不是拿来比的。

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/observer/span_recorder.h"
#include "bach/sim/build.h"
#include "bach/sim/completion.h"
#include "bach/tables/loader.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

struct BachExpect {
  uint64_t end_time = 0;
  uint64_t users = 0;
  std::vector<uint64_t> completed;  // 按 Bach 那边的完成先后
  std::vector<uint64_t> lost;
  std::vector<std::pair<uint64_t, uint64_t>> latency;  // uid 与它的端到端延迟
};

std::string FixturePath(std::string const& name) {
  return std::string(PROJECT_TEST_DIR) + "/bach/fixture/" + name + ".bachir";
}

std::string ExpectPath(std::string const& name) {
  return std::string(PROJECT_TEST_DIR) + "/bach/fixture/expect/" + name +
         ".expect";
}

BachExpect LoadExpect(std::string const& name) {
  std::ifstream in(ExpectPath(name));
  EXPECT_TRUE(in.is_open()) << "cannot open " << ExpectPath(name);
  BachExpect e;
  bool saw_version = false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream row(line);
    std::string tag;
    row >> tag;
    if (tag == "BACHEXPECT") {
      uint64_t v = 0;
      row >> v;
      EXPECT_EQ(v, 1u) << "unknown expect format version";
      saw_version = true;
    } else if (tag == "META") {
      std::string key;
      row >> key;
      if (key == "end_time") row >> e.end_time;
    } else if (tag == "USERS") {
      row >> e.users;
    } else if (tag == "COMPLETED" || tag == "LOST") {
      std::vector<uint64_t>& into = tag == "COMPLETED" ? e.completed : e.lost;
      uint64_t uid = 0;
      while (row >> uid) into.push_back(uid);
    } else if (tag == "DONE") {
      uint64_t uid = 0, ns = 0;
      row >> uid >> ns;
      e.latency.emplace_back(uid, ns);
    }
  }
  EXPECT_TRUE(saw_version) << ExpectPath(name) << " has no version line";
  return e;
}

// 跑一份中间文件。判完成的是谁由这份文件自己说了算：有以太网交换节点就是它，
// 没有就是各汇聚点。
RunResult RunFixture(std::string const& name, uint64_t* total_users,
                     std::vector<GlobalLatency>* latency,
                     std::vector<UnitWait>* waits,
                     std::vector<UnitSpan>* spans) {
  BachIr ir = LoadBachIr(FixturePath(name));
  RT::Reset(10, 10);
  ClockPtr clk = MakeClock(0, kPeriod);
  std::unique_ptr<System> sys = BuildSystem(clk, ir);
  const CompletionAuthority who = ir.eth.present ? CompletionAuthority::kEth
                                                 : CompletionAuthority::kOut;
  RunControl control(clk, *sys, ExpectedCompletions(ir), "run", 0, who);

  clk->Continue();
  RT::JoinAll();
  EXPECT_TRUE(control.Finished()) << name << " never finished";

  RunRecorder run;
  sys->Collect(run);
  run.Finalize(ir.total_users);
  *total_users = ir.total_users;
  *latency = run.Latency();
  *waits = run.Waits();
  *spans = run.Spans();
  return run.Result();
}

std::string Join(std::vector<uint64_t> const& v) {
  std::ostringstream out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i != 0) out << " ";
    out << v[i];
  }
  return out.str();
}

void CompareWithBach(std::string const& name) {
  const BachExpect want = LoadExpect(name);
  uint64_t users = 0;
  std::vector<GlobalLatency> latency;
  std::vector<UnitWait> waits;
  std::vector<UnitSpan> spans;
  const RunResult got = RunFixture(name, &users, &latency, &waits, &spans);

  EXPECT_EQ(users, want.users) << name << ": the two sides disagree on how "
                                          "many users this map has";

  // 丢包集合一模一样
  std::vector<uint64_t> lost = got.lost_uids;
  std::sort(lost.begin(), lost.end());
  std::vector<uint64_t> want_lost = want.lost;
  std::sort(want_lost.begin(), want_lost.end());
  EXPECT_EQ(Join(lost), Join(want_lost)) << name << ": lost users differ";

  // 完成集合一模一样
  std::vector<uint64_t> done = got.completed_uids;
  std::sort(done.begin(), done.end());
  std::vector<uint64_t> want_done = want.completed;
  std::sort(want_done.begin(), want_done.end());
  EXPECT_EQ(Join(done), Join(want_done)) << name << ": completed users differ";

  // 完成先后：不同不算错，但要看得见
  if (Join(got.completed_uids) != Join(want.completed)) {
    ADD_FAILURE() << name << ": the two sides finished the same users in a "
                             "different order\n  bach:  "
                  << Join(want.completed) << "\n  latch: "
                  << Join(got.completed_uids);
  }

  // 等待归因按类汇总。逐 user 的延迟差落在哪一类等待上，从这里看得出来。
  {
    std::vector<std::pair<std::string, uint64_t>> by_reason;
    for (UnitWait const& w : waits) {
      const std::string key = WaitReasonName(w.reason);
      bool found = false;
      for (auto& kv : by_reason) {
        if (kv.first != key) continue;
        kv.second += static_cast<uint64_t>(w.end - w.start);
        found = true;
        break;
      }
      if (!found) by_reason.emplace_back(key, static_cast<uint64_t>(w.end - w.start));
    }
    std::sort(by_reason.begin(), by_reason.end(),
              [](auto const& a, auto const& b) { return a.second > b.second; });
    std::cout << "  " << name << " 等待归因:";
    for (auto const& kv : by_reason) {
      std::cout << " " << kv.first << "=" << kv.second;
    }
    std::cout << std::endl;
  }

  // 占用按单元汇总。等待归因对上了但端到端还差着，差的就在这里。
  {
    std::vector<std::pair<std::string, uint64_t>> by_unit;
    for (UnitSpan const& sp : spans) {
      const std::string key =
          std::string(UnitName(sp.unit)) + ":" + SpanStateName(sp.state);
      bool found = false;
      for (auto& kv : by_unit) {
        if (kv.first != key) continue;
        kv.second += static_cast<uint64_t>(sp.end - sp.start);
        found = true;
        break;
      }
      if (!found) by_unit.emplace_back(key, static_cast<uint64_t>(sp.end - sp.start));
    }
    std::sort(by_unit.begin(), by_unit.end(),
              [](auto const& a, auto const& b) { return a.second > b.second; });
    std::cout << "  " << name << " 占用:";
    for (auto const& kv : by_unit) std::cout << " " << kv.first << "=" << kv.second;
    std::cout << std::endl;
  }

  // 逐个 user 的端到端延迟只报出来，不作为判据
  for (auto const& kv : want.latency) {
    int64_t mine = 0;
    Time began = 0;
    for (GlobalLatency const& g : latency) {
      if (g.uid != kv.first) continue;
      mine = static_cast<int64_t>(g.end) - static_cast<int64_t>(g.start);
      began = g.start;
    }
    const double drift =
        kv.second == 0 ? 0.0
                       : (static_cast<double>(mine) -
                          static_cast<double>(kv.second)) *
                             100.0 / static_cast<double>(kv.second);
    std::cout << "  " << name << " uid " << kv.first << " (起跑 " << began
              << "): bach " << kv.second << " ns, latch " << mine << " ns, 差 "
              << mine - static_cast<int64_t>(kv.second) << " ns (" << drift
              << "%)" << std::endl;
  }
}

}  // namespace

TEST(BachCompare, DenseTopology) { CompareWithBach("bach_topology"); }

TEST(BachCompare, BroadcastCores) { CompareWithBach("moe_bc_core"); }

TEST(BachCompare, NanoMoe) { CompareWithBach("nano-moe"); }

TEST(BachCompare, NeoMoe) { CompareWithBach("neo-moe"); }

TEST(BachCompare, PcieSwitchFabric) { CompareWithBach("pcie_switch"); }

TEST(BachCompare, FiveRouteArbitration) { CompareWithBach("dsa_five_route"); }

TEST(BachCompare, Phase1Boundary) { CompareWithBach("phase1_boundary"); }

TEST(BachCompare, ThreeStagePipeline) { CompareWithBach("phase1_eth"); }
