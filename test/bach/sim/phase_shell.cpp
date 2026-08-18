// Phase 之间的边界声明与完成账本。
//
// 这一层不跑时钟：它管的是接线合不合法，以及乱序完成怎么恢复成进来的顺序。所以用例
// 全是直接调，不搭硬件。

#include <gtest/gtest.h>

#include <vector>

#include "bach/sim/phase_shell.h"

using namespace latch;
using namespace latch::bach;

namespace {

PhasePort MakePort(std::string const& phase, std::string const& port,
                   PhasePortDir dir, std::vector<PayloadRole> roles,
                   bool multi = false) {
  PhasePort p;
  p.phase_id = phase;
  p.port_id = port;
  p.dir = dir;
  p.roles = RoleSet(roles);
  p.allow_multiple_upstreams = multi;
  return p;
}

PhaseEdge MakeEdge(std::string const& id, std::string const& src_phase,
                   std::string const& src_port, std::string const& dst_phase,
                   std::string const& dst_port,
                   std::vector<PayloadRole> roles) {
  PhaseEdge e;
  e.edge_id = id;
  e.src_phase = src_phase;
  e.src_port = src_port;
  e.dst_phase = dst_phase;
  e.dst_port = dst_port;
  e.roles = RoleSet(roles);
  return e;
}

// 三段流水的最小声明：第一段分岔成两路，MoE 那路进第二段，两路在第三段汇合。
PhaseTopology ThreeStages() {
  PhaseTopology t;

  PhaseDef one;
  one.phase_id = "phase1";
  one.ports.push_back(MakePort("phase1", "to_moe", PhasePortDir::kOutput,
                               {PayloadRole::kMoeRequest}));
  one.ports.push_back(MakePort("phase1", "to_join", PhasePortDir::kOutput,
                               {PayloadRole::kResidualBypass}));
  t.AddPhase(one);

  PhaseDef two;
  two.phase_id = "phase2";
  two.ports.push_back(MakePort("phase2", "from_phase1", PhasePortDir::kInput,
                               {PayloadRole::kMoeRequest}));
  two.ports.push_back(MakePort("phase2", "to_join", PhasePortDir::kOutput,
                               {PayloadRole::kMoeResult}));
  two.required_input_roles = RoleBit(PayloadRole::kMoeRequest);
  t.AddPhase(two);

  PhaseDef three;
  three.phase_id = "phase3";
  three.ports.push_back(MakePort("phase3", "residual", PhasePortDir::kInput,
                                 {PayloadRole::kResidualBypass}));
  three.ports.push_back(MakePort("phase3", "result", PhasePortDir::kInput,
                                 {PayloadRole::kMoeResult}));
  three.required_input_roles = RoleBit(PayloadRole::kResidualBypass) |
                               RoleBit(PayloadRole::kMoeResult);
  t.AddPhase(three);

  t.AddEdge(MakeEdge("e1", "phase1", "to_moe", "phase2", "from_phase1",
                     {PayloadRole::kMoeRequest}));
  t.AddEdge(MakeEdge("e2", "phase1", "to_join", "phase3", "residual",
                     {PayloadRole::kResidualBypass}));
  t.AddEdge(MakeEdge("e3", "phase2", "to_join", "phase3", "result",
                     {PayloadRole::kMoeResult}));
  return t;
}

}  // namespace

// ---------------------------------------------------------------- 拓扑

TEST(BachPhaseShell, AcceptsAWellFormedPipeline) {
  PhaseTopology t = ThreeStages();
  t.Validate();
  EXPECT_EQ(t.PhaseNum(), 3u);
  EXPECT_EQ(t.EdgeNum(), 3u);
}

// 一个 Phase 声明它必须收到的那几样，得真有人产。
TEST(BachPhaseShell, ARequiredRoleNeedsSomeoneToProduceIt) {
  PhaseTopology t;
  PhaseDef one;
  one.phase_id = "phase1";
  one.ports.push_back(MakePort("phase1", "to_moe", PhasePortDir::kOutput,
                               {PayloadRole::kMoeRequest}));
  t.AddPhase(one);

  PhaseDef three;
  three.phase_id = "phase3";
  three.ports.push_back(MakePort("phase3", "residual", PhasePortDir::kInput,
                                 {PayloadRole::kResidualBypass}));
  three.required_input_roles = RoleBit(PayloadRole::kResidualBypass);
  t.AddPhase(three);

  // 没有一条边通向 phase3，它要的那一样没人产
  EXPECT_DEATH(t.Validate(), "");
}

// 一个输入口默认只许一个上游。要多个就得说明这里有仲裁。
TEST(BachPhaseShell, TwoUpstreamsNeedAnArbiterOnTheRecord) {
  auto build = [](bool multi) {
    PhaseTopology t;
    PhaseDef a;
    a.phase_id = "a";
    a.ports.push_back(
        MakePort("a", "out", PhasePortDir::kOutput, {PayloadRole::kMoeResult}));
    t.AddPhase(a);
    PhaseDef b;
    b.phase_id = "b";
    b.ports.push_back(
        MakePort("b", "out", PhasePortDir::kOutput, {PayloadRole::kMoeResult}));
    t.AddPhase(b);
    PhaseDef c;
    c.phase_id = "c";
    c.ports.push_back(MakePort("c", "in", PhasePortDir::kInput,
                               {PayloadRole::kMoeResult}, multi));
    t.AddPhase(c);
    t.AddEdge(MakeEdge("e1", "a", "out", "c", "in", {PayloadRole::kMoeResult}));
    t.AddEdge(MakeEdge("e2", "b", "out", "c", "in", {PayloadRole::kMoeResult}));
    t.Validate();
  };
  build(true);
  EXPECT_DEATH(build(false), "");
}

// 一条边上跑的东西，两端都得认。
TEST(BachPhaseShell, BothEndsMustAcceptWhatTheEdgeCarries) {
  PhaseTopology t;
  PhaseDef a;
  a.phase_id = "a";
  a.ports.push_back(
      MakePort("a", "out", PhasePortDir::kOutput, {PayloadRole::kMoeRequest}));
  t.AddPhase(a);
  PhaseDef b;
  b.phase_id = "b";
  b.ports.push_back(
      MakePort("b", "in", PhasePortDir::kInput, {PayloadRole::kMoeResult}));
  t.AddPhase(b);
  EXPECT_DEATH(
      t.AddEdge(MakeEdge("e", "a", "out", "b", "in",
                         {PayloadRole::kMoeRequest})),
      "");
}

// 边的方向不能反过来接。
TEST(BachPhaseShell, AnEdgeCannotRunBackwards) {
  PhaseTopology t;
  PhaseDef a;
  a.phase_id = "a";
  a.ports.push_back(
      MakePort("a", "in", PhasePortDir::kInput, {PayloadRole::kMoeRequest}));
  t.AddPhase(a);
  PhaseDef b;
  b.phase_id = "b";
  b.ports.push_back(
      MakePort("b", "in", PhasePortDir::kInput, {PayloadRole::kMoeRequest}));
  t.AddPhase(b);
  EXPECT_DEATH(
      t.AddEdge(MakeEdge("e", "a", "in", "b", "in",
                         {PayloadRole::kMoeRequest})),
      "");
}

// ---------------------------------------------------------------- 完成账本

// 完成的先后可以与进来的先后不同，交出去的顺序仍是进来的顺序。
TEST(BachPhaseOrchestrator, RestoresTheOrderWorkCameIn) {
  PhaseOrchestrator orch;
  PhaseWork a = orch.RegisterIngress(15);
  PhaseWork b = orch.RegisterIngress(14);
  PhaseWork c = orch.RegisterIngress(13);

  orch.Complete(c, 300);
  EXPECT_EQ(orch.ReleasedNum(), 0u);  // 前面两个还没完成，攒着
  EXPECT_EQ(orch.HeldNum(), 1u);

  orch.Complete(b, 200);
  EXPECT_EQ(orch.ReleasedNum(), 0u);
  EXPECT_EQ(orch.HeldNum(), 2u);

  orch.Complete(a, 100);
  EXPECT_EQ(orch.ReleasedNum(), 3u);
  EXPECT_EQ(orch.HeldNum(), 0u);
  EXPECT_EQ(orch.CompletedUids(), (std::vector<uint64_t>{15, 14, 13}));
  EXPECT_TRUE(orch.AllReleased());
}

// 按顺序完成时来一个放一个，不攒。
TEST(BachPhaseOrchestrator, ReleasesInOrderAsTheyFinish) {
  PhaseOrchestrator orch;
  PhaseWork a = orch.RegisterIngress(1);
  PhaseWork b = orch.RegisterIngress(2);
  orch.Complete(a, 10);
  EXPECT_EQ(orch.ReleasedNum(), 1u);
  EXPECT_FALSE(orch.AllReleased());
  orch.Complete(b, 20);
  EXPECT_EQ(orch.ReleasedNum(), 2u);
  EXPECT_TRUE(orch.AllReleased());
}

// 同一次工作完成两次是错的。
TEST(BachPhaseOrchestrator, OneWorkCompletesOnlyOnce) {
  PhaseOrchestrator orch;
  PhaseWork a = orch.RegisterIngress(1);
  orch.Complete(a, 10);
  EXPECT_DEATH(orch.Complete(a, 20), "");
}

// 没进来过的工作报不了完成。
TEST(BachPhaseOrchestrator, AnUnknownWorkCannotComplete) {
  PhaseOrchestrator orch;
  PhaseWork ghost;
  ghost.seq = 7;
  ghost.uid = 99;
  EXPECT_DEATH(orch.Complete(ghost, 10), "");
}
