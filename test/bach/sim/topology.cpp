// 三十二核那张真实 Map 跑通。
//
// 这是第二期的验收：整套硬件从中间文件装配起来，封包按十二端口的路由走完片内 mesh
// 与片间 PCIe，完成集合与丢包集合都对得上。前面那些用例的拓扑都是手写的两三个核，
// 这一组第一次让路由算法在真的四块 chip 上跑。

#include <gtest/gtest.h>

#include <memory>
#include <string>

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

std::string TopologyPath() {
  return std::string(PROJECT_TEST_DIR) + "/bach/fixture/bach_topology.bachir";
}

std::string MoePath() {
  return std::string(PROJECT_TEST_DIR) + "/bach/fixture/moe_bc_core.bachir";
}

std::string FixturePath(std::string const& name) {
  return std::string(PROJECT_TEST_DIR) + "/bach/fixture/" + name + ".bachir";
}

// 一张 Map 跑到完成，返回汇总结果。MoE 那几张都是同一套跑法，差别只在 Map 本身。
struct MapRunStats {
  uint64_t bypassed = 0;
  uint64_t dropped = 0;
};

void RunMap(std::string const& name, RunRecorder* run, uint64_t* expected,
            MapRunStats* stats = nullptr) {
  BachIr ir = LoadBachIr(FixturePath(name));
  RT::Reset(10, 10);
  ClockPtr clk = MakeClock(0, kPeriod);
  std::unique_ptr<System> sys = BuildSystem(clk, ir);
  RunControl control(clk, *sys, ExpectedPackets(ir), "run", 0);

  clk->Continue();
  RT::JoinAll();

  EXPECT_TRUE(control.Finished()) << name << " never finished";
  sys->Collect(*run);
  run->Finalize(ir.total_users);
  *expected = ir.total_users;

  for (auto const& core : sys->Cores()) {
    EXPECT_EQ(core->Scheduler().ActiveUserNum(), 0u)
        << name << " core " << core->CoreId() << " still holds a user";
    if (stats != nullptr) {
      stats->bypassed += core->DteUnit().InactiveBypassNum();
      stats->dropped += core->DteUnit().InactiveDropNum();
    }
  }
}

}  // namespace

TEST(BachTopology, BuildsTheWholeArrayFromTheCompiledMap) {
  BachIr ir = LoadBachIr(TopologyPath());
  RT::Reset(6, 6);
  ClockPtr clk = MakeClock(0, kPeriod);
  std::unique_ptr<System> sys = BuildSystem(clk, ir);

  // 四块 chip 各八个核，加一个注入源与一个汇聚点
  EXPECT_EQ(sys->CoreNum(), 32u);
  EXPECT_EQ(sys->HostNum(), 1u);
  EXPECT_EQ(sys->OutNum(), 1u);
  EXPECT_EQ(sys->NodeNum(), 34u);

  // 入口核在阵列里，注入源在阵列外
  EXPECT_TRUE(sys->CoreById(16).Rt().IsInternal());
  EXPECT_FALSE(sys->Hosts()[0]->Rt().IsInternal());
  EXPECT_FALSE(sys->Outs()[0]->Rt().IsInternal());
}

TEST(BachTopology, RunsOneUserAcrossFourChips) {
  BachIr ir = LoadBachIr(TopologyPath());
  RT::Reset(6, 6);
  ClockPtr clk = MakeClock(0, kPeriod);
  std::unique_ptr<System> sys = BuildSystem(clk, ir);

  RunControl control(clk, *sys, ExpectedPackets(ir), "run", 0);

  clk->Continue();
  RT::JoinAll();

  EXPECT_TRUE(control.Finished());
  EXPECT_EQ(control.CompletedNum(), ExpectedPackets(ir));

  RunRecorder run;
  sys->Collect(run);
  run.Finalize(ir.total_users);

  EXPECT_TRUE(run.Result().Succeeded());
  EXPECT_EQ(run.Result().completed_uids.size(), ir.total_users);
  EXPECT_TRUE(run.Result().lost_uids.empty());
  // Bach 一次 run 的结束时刻在 1e4 到 1e5 ns 量级，这份 Map 跑出来是三万五千拍：
  // 进出阵列各一次跨 node 的五千拍，链上三次跨 chip 各五千拍，其余是三十二个核的
  // 取指、计算与搬运
  EXPECT_GT(run.Result().end_time, 10000u);
  EXPECT_LT(run.Result().end_time, 200000u);

  // 每个核都把自己的 user 送走了：活跃队列空、槽位全回来了
  for (auto const& core : sys->Cores()) {
    EXPECT_EQ(core->Scheduler().ActiveUserNum(), 0u)
        << "core " << core->CoreId() << " still holds a user";
    EXPECT_EQ(core->DteUnit().FreeSlotNum(), core->DteUnit().TotalSlots())
        << "core " << core->CoreId() << " leaked a memory slot";
  }

  // 注入源的额度全部回血，说明退休信号一路走回来了
  EXPECT_EQ(sys->Hosts()[0]->CreditLevel(), ir.params.stream_count);

  // 走过路由的封包留下的占用要落在观测里
  uint64_t router_spans = 0;
  for (UnitSpan const& s : run.Spans()) {
    if (s.unit == Unit::kRouter) ++router_spans;
  }
  EXPECT_GT(router_spans, 0u);
}

// 六十四核那张广播 Map 跑通。入口核把 user 压进队列，按 stream 数认领，扇出到三个
// 下游，退休时从队首摘掉。
TEST(BachTopology, RunsTheBroadcastMap) {
  BachIr ir = LoadBachIr(MoePath());
  RT::Reset(10, 10);
  ClockPtr clk = MakeClock(0, kPeriod);
  std::unique_ptr<System> sys = BuildSystem(clk, ir);
  RunControl control(clk, *sys, ExpectedPackets(ir), "run", 0);

  clk->Continue();
  RT::JoinAll();

  EXPECT_TRUE(control.Finished());
  EXPECT_EQ(control.CompletedNum(), ExpectedPackets(ir));

  RunRecorder run;
  sys->Collect(run);
  run.Finalize(ir.total_users);

  EXPECT_TRUE(run.Result().Succeeded());
  EXPECT_TRUE(run.Result().lost_uids.empty());
  for (auto const& core : sys->Cores()) {
    EXPECT_EQ(core->Scheduler().ActiveUserNum(), 0u)
        << "core " << core->CoreId() << " still holds a user";
  }
}

TEST(BachTopology, BuildsTheMoeMapTopology) {
  BachIr ir = LoadBachIr(MoePath());
  RT::Reset(10, 10);
  ClockPtr clk = MakeClock(0, kPeriod);
  std::unique_ptr<System> sys = BuildSystem(clk, ir);

  EXPECT_EQ(sys->CoreNum(), ir.dim.CoreNum());
  EXPECT_GT(sys->HostNum(), 0u);
  EXPECT_GT(sys->OutNum(), 0u);

  // 每块 chip 的四个方向都有出口网关，跨 chip 的包先回到网关核再出去
  EXPECT_FALSE(sys->Route().chip_gateways.empty());
  EXPECT_NE(sys->Route().GatewayOf(0, Direction::kRight), nullptr);
  EXPECT_NE(sys->Route().GatewayOf(0, Direction::kBottom), nullptr);
}

// 两张真 MoE 的 Map：注入源按预生成的命中给每个 token 带上 HitMap 与倍率，没被命中的
// 那些核不参与本次计算，链路上的包该转的转、该还额度的还额度。
TEST(BachMoe, RunsNanoMoe) {
  RunRecorder run;
  uint64_t expected = 0;
  MapRunStats stats;
  RunMap("nano-moe", &run, &expected, &stats);

  EXPECT_TRUE(run.Result().Succeeded());
  EXPECT_EQ(run.Result().completed_uids.size(), expected);
  EXPECT_TRUE(run.Result().lost_uids.empty());

  // 这张 Map 上不是每个 token 都激活全部专家组，所以一定有包被挡在没命中的核上：
  // 挡住的包该绕的绕、该还额度的还额度，而不是停在那里等一个不会来的东西
  EXPECT_GT(stats.bypassed + stats.dropped, 0u);
}

TEST(BachMoe, RunsNeoMoe) {
  RunRecorder run;
  uint64_t expected = 0;
  RunMap("neo-moe", &run, &expected);

  EXPECT_TRUE(run.Result().Succeeded());
  EXPECT_EQ(run.Result().completed_uids.size(), expected);
  EXPECT_TRUE(run.Result().lost_uids.empty());
}

// 同一张三十二核的 Map，把执行通道那一段换成五路径仲裁。换掉的是排队方式，不是要做的
// 事，所以完成集合必须一模一样。
//
// Map 编译产物现在不写显式路径，所以这一整张 Map 上的搬运全都落在按方向兜底的那两条
// 队列上。五条精确路径与它们之间那十个交点由 test/bach/ip/dsa.cpp 覆盖。
TEST(BachDsaRun, FiveRouteChangesTheQueueingNotTheOutcome) {
  BachIr ir = LoadBachIr(FixturePath("dsa_five_route"));
  ASSERT_EQ(ir.params.dte_dsa_mode, DteDsaMode::kFiveRoute);
  RT::Reset(6, 6);
  ClockPtr clk = MakeClock(0, kPeriod);
  std::unique_ptr<System> sys = BuildSystem(clk, ir);
  RunControl control(clk, *sys, ExpectedPackets(ir), "run", 0);

  clk->Continue();
  RT::JoinAll();

  EXPECT_TRUE(control.Finished());
  RunRecorder run;
  sys->Collect(run);
  run.Finalize(ir.total_users);
  EXPECT_TRUE(run.Result().Succeeded());
  EXPECT_EQ(run.Result().completed_uids.size(), ir.total_users);
  EXPECT_TRUE(run.Result().lost_uids.empty());

  std::vector<DsaRecord> all;
  for (auto const& core : sys->Cores()) {
    auto const& records = core->DteUnit().DsaRecords();
    all.insert(all.end(), records.begin(), records.end());
  }
  ASSERT_FALSE(all.empty());

  DsaAnalysis an = AnalyzeDsa(all);
  EXPECT_GT(an.direction_only, 0u);
  // 掩码相交的两条路径任何时候都不该同时占用，这是仲裁器唯一必须守住的事
  for (uint32_t i = 0; i < kDsaCrossNum; ++i) {
    if (!an.crosses[i].mask_conflict) continue;
    EXPECT_EQ(an.crosses[i].overlap, 0u);
  }

  // 这张 Map 上没有一条搬运写了显式路径，兜底的两条队列与原来那两条执行通道一一对应，
  // 所以结束时刻要与关着五路径时一模一样
  RunRecorder plain;
  uint64_t plain_users = 0;
  RunMap("bach_topology", &plain, &plain_users);
  EXPECT_EQ(run.Result().end_time, plain.Result().end_time);
}

// 同一张三十二核的 Map，注入源与汇聚点改挂在一个 PCIe 交换节点上。进出阵列的那两跳
// 不再是核到设备的直连，而是核到交换节点、交换节点到设备的两跳显式路由。
TEST(BachPcieFabric, BuildsTheSwitchAndItsRoutes) {
  BachIr ir = LoadBachIr(FixturePath("pcie_switch"));
  RT::Reset(6, 6);
  ClockPtr clk = MakeClock(0, kPeriod);
  std::unique_ptr<System> sys = BuildSystem(clk, ir);

  ASSERT_EQ(sys->SwitchNum(), 1u);
  PcieSwitch& sw = sys->SwitchById("SW0");
  EXPECT_EQ(sw.PortNum(), 4u);
  // 四个目的地各一条：注入源、汇聚点，以及接它们的那两个核
  EXPECT_EQ(sw.Destinations().size(), 4u);
  EXPECT_TRUE(sw.HasRouteTo(Coord{0, -1}));
  EXPECT_TRUE(sw.HasRouteTo(Coord{1, 16}));

  // 与交换节点相连的那两个核把它能送到的地方都登记成走这个口，别的核不受影响
  EXPECT_TRUE(sys->CoreById(16).Rt().HasSwitchRoute(Coord{1, 16}));
  EXPECT_TRUE(sys->CoreById(15).Rt().HasSwitchRoute(Coord{0, -1}));
  EXPECT_FALSE(sys->CoreById(0).Rt().HasSwitchRoute(Coord{1, 16}));
}

TEST(BachPcieFabric, RunsOneUserThroughTheSwitch) {
  BachIr ir = LoadBachIr(FixturePath("pcie_switch"));
  RT::Reset(6, 6);
  ClockPtr clk = MakeClock(0, kPeriod);
  std::unique_ptr<System> sys = BuildSystem(clk, ir);
  RunControl control(clk, *sys, ExpectedPackets(ir), "run", 0);

  clk->Continue();
  RT::JoinAll();

  EXPECT_TRUE(control.Finished());
  EXPECT_EQ(control.CompletedNum(), ExpectedPackets(ir));

  RunRecorder run;
  sys->Collect(run);
  run.Finalize(ir.total_users);

  EXPECT_TRUE(run.Result().Succeeded());
  EXPECT_EQ(run.Result().completed_uids.size(), ir.total_users);
  EXPECT_TRUE(run.Result().lost_uids.empty());

  // 进出阵列的每一跳都真的经过了交换节点
  PcieSwitch& sw = sys->SwitchById("SW0");
  EXPECT_GT(sw.ServedBeats(), 0u);
  EXPECT_EQ(sw.InTransitFragments(), 0u);

  for (auto const& core : sys->Cores()) {
    EXPECT_EQ(core->Scheduler().ActiveUserNum(), 0u)
        << "core " << core->CoreId() << " still holds a user";
  }
  EXPECT_EQ(sys->Hosts()[0]->CreditLevel(), ir.params.stream_count);

  // 交换节点转发的那几拍要落在观测里，归在它自己名下
  uint64_t switch_spans = 0;
  for (UnitSpan const& s : run.Spans()) {
    if (s.unit == Unit::kSwitch) ++switch_spans;
  }
  EXPECT_GT(switch_spans, 0u);
}

// ---------------------------------------------------------------- Phase

// 三段流水接起来：Phase1 的通道分岔成两路，MoE 那路经交换节点换算成 EPGroup 注入
// Phase2，Phase2 算完的结果再回到交换节点与残差汇合。
TEST(BachPhaseRun, BuildsTheThreeStagePipeline) {
  BachIr ir = LoadBachIr(FixturePath("phase1_eth"));
  RT::Reset(10, 10);
  ClockPtr clk = MakeClock(0, kPeriod);
  std::unique_ptr<System> sys = BuildSystem(clk, ir);

  EXPECT_EQ(sys->LaneNum(), 1u);
  EXPECT_EQ(sys->SinkNum(), 2u);
  EXPECT_TRUE(sys->HasEth());
  EXPECT_TRUE(sys->HasPhase2Ingress());
  EXPECT_TRUE(sys->HasPhase3Join());
  // 汇合点拿走全局完成权，交换节点自己那个判法必须是关着的
  EXPECT_FALSE(sys->Eth().Phase1BoundaryCompletion());
  // 结果那条回程接在汇聚点上
  EXPECT_TRUE(sys->Outs().front()->BridgedToEth());
  // 完成判据数的是工作次数，不是包数
  EXPECT_EQ(ExpectedCompletions(ir), 2u);
}

TEST(BachPhaseRun, RunsTwoUsersThroughAllThreeStages) {
  BachIr ir = LoadBachIr(FixturePath("phase1_eth"));
  RT::Reset(10, 10);
  ClockPtr clk = MakeClock(0, kPeriod);
  std::unique_ptr<System> sys = BuildSystem(clk, ir);
  RunControl control(clk, *sys, ExpectedCompletions(ir), "run", 0,
                     CompletionAuthority::kEth);

  clk->Continue();
  RT::JoinAll();

  ASSERT_TRUE(control.Finished());
  EXPECT_EQ(sys->Join().JoinedNum(), 2u);
  EXPECT_EQ(sys->Join().PendingNum(), 0u);
  // 两路各两包，加上回来的两包结果
  EXPECT_EQ(sys->Eth().DeliveredNum(), 6u);
  // 每个 user 的 MoE 请求注入一次：它命中的三个专家都落在同一个 EPGroup 上
  EXPECT_EQ(sys->Phase2().InjectedNum(), 2u);
  EXPECT_EQ(sys->Eth().BoundaryCompletions(), 0u);

  RunRecorder run;
  sys->Collect(run);
  run.Finalize(ir.total_users);
  EXPECT_TRUE(run.Result().Succeeded());
  EXPECT_EQ(run.Result().completed_uids.size(), 2u);
  EXPECT_TRUE(run.Result().lost_uids.empty());
}

// 完成权换一处：不接 Phase2 与汇合点时，交换节点自己判 Phase1 边界，两路都投出去
// 就算一次工作完成。同一张 Map 两种判法各判两次，判的是同两次工作。
TEST(BachPhaseRun, TheBoundaryJudgesWithoutPhase2) {
  BachIr ir = LoadBachIr(FixturePath("phase1_boundary"));
  RT::Reset(10, 10);
  ClockPtr clk = MakeClock(0, kPeriod);
  std::unique_ptr<System> sys = BuildSystem(clk, ir);
  RunControl control(clk, *sys, ExpectedCompletions(ir), "run", 0,
                     CompletionAuthority::kEth);

  clk->Continue();
  RT::JoinAll();

  ASSERT_TRUE(control.Finished());
  EXPECT_TRUE(sys->Eth().Phase1BoundaryCompletion());
  EXPECT_FALSE(sys->HasPhase2Ingress());
  EXPECT_FALSE(sys->HasPhase3Join());
  EXPECT_EQ(sys->Eth().BoundaryCompletions(), 2u);
  // 只走 Phase1 那两路，没有回程的结果
  EXPECT_EQ(sys->Eth().DeliveredNum(), 4u);
  EXPECT_FALSE(sys->Outs().front()->BridgedToEth());

  RunRecorder run;
  sys->Collect(run);
  run.Finalize(ir.total_users);
  EXPECT_TRUE(run.Result().Succeeded());
}
