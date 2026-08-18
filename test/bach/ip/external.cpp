// 整条链路跑通：注入源推一个 user 进阵列，核按任务表做完，结果经路由送到汇聚点，
// 退休信号回到上游把额度还回去。
//
// 这一组同时是第一期与第二期的验收：任务表跑完、额度闭环、队首退休的约束生效，以及
// 封包真的走路由而不是直连。逐段时间在用例里手算对齐，对不上就是某一段的语义变了。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/core.h"
#include "bach/ip/external/host.h"
#include "bach/ip/external/out.h"
#include "bach/ip/route_config.h"
#include "bach/observer/span_recorder.h"
#include "fixture.h"

using namespace latch;
using namespace latch::bach;
using latch::bach::test::CoreTables;

namespace {

constexpr Time kPeriod = 1;
constexpr uint64_t kVolume = 8192;

// 阵列尺寸。外部节点的坐标落在阵列之外，所以它们不占核位。
// 一条双向物理链路：a 的 port 接 b，b 的反向口接回 a。
void Attach(Router& a, Port port, Router& b) {
  const Port back = OppositePort(port);
  a.ConnectPort(port, b.Position(), b.IsInternal(), &b.OpenPort(back));
  b.ConnectPort(back, a.Position(), a.IsInternal(), &a.OpenPort(port));
}

Dim MakeDim(uint32_t cols) {
  Dim d;
  d.chip_rows = 1;
  d.chip_cols = 1;
  d.core_rows_per_chip = 1;
  d.core_cols_per_chip = cols;
  d.node_chip_rows = 1;
  d.node_chip_cols = 1;
  return d;
}

}  // namespace

// ---------------------------------------------------------------- 单核全链路

TEST(BachSingleCore, RunsOneUserThroughTheWholeTaskTable) {
  RT::Reset(4, 4);
  ClockPtr clk = MakeClock(0, kPeriod);

  const Coord host_coord{0, -1};
  const Coord core_coord{0, 0};
  const Coord out_coord{0, 1};

  CoreTables tb(0);
  tb.Task(UnitType::kSkip, Opcode::kUserInit, 0, kVolume, core_coord)
      .Task(UnitType::kMc, Opcode::kDontCare, 0, 100)
      .Task(UnitType::kDte, Opcode::kMove, 0, kVolume, out_coord)
      .Task(UnitType::kDte, Opcode::kRetire, 0, 0, host_coord, -1);

  RouteConfig cfg;
  cfg.dim = MakeDim(1);
  cfg.AddExtNode(host_coord, core_coord, Port::kPcieWest);
  cfg.AddExtNode(out_coord, core_coord, Port::kPcieEast);

  Core core(clk, tb.Context(), core_coord, &cfg, "core0", 0);
  Host host(clk, tb.params, 100, host_coord, &cfg, kVolume,
            tb.params.pcie_bandwidth, tb.params.stream_count,
            Opcode::kUserInit, "host", 0);
  Out out(clk, tb.params, 101, out_coord, &cfg, kVolume,
          tb.params.pcie_bandwidth, "out", 0);

  Attach(core.Rt(), Port::kPcieWest, host.Rt());
  Attach(core.Rt(), Port::kPcieEast, out.Rt());

  host.SetUsers({HostUser{0, core_coord, 0, 1}});

  clk->Continue(30000 * kPeriod);
  RT::JoinAll();

  RunRecorder run;
  run.Collect(core.Recorder());
  run.Collect(host.Recorder());
  run.Collect(out.Recorder());
  run.Finalize(1);

  EXPECT_TRUE(run.Result().Succeeded());
  ASSERT_EQ(run.Result().completed_uids.size(), 1u);
  EXPECT_EQ(run.Result().completed_uids[0], 0u);
  EXPECT_TRUE(run.Result().lost_uids.empty());

  // 槽位与额度都回到起点，说明这个 user 在核上与在注入源上都完整地下了车
  EXPECT_EQ(host.CreditLevel(), tb.params.stream_count);
  EXPECT_EQ(core.Scheduler().ActiveUserNum(), 0u);
  EXPECT_EQ(core.Scheduler().FlowNum(), 0u);
  EXPECT_EQ(core.DteUnit().FreeSlotNum(), core.DteUnit().TotalSlots());
  EXPECT_EQ(out.CompletedNum(), 1u);

  // 核上这一段逐段手算。注入源与汇聚点各自带一个路由器，所以进出阵列各是两跳：
  // 外部节点的路由器一跳、网关核的路由器一跳。
  //
  //   推包间隔                                     100
  //   第 k 拍在 100+k 交给自己的路由器，下一拍被它服务
  //   一跳一拍加访问延迟十拍加跨 node 五千拍         → 网关核在 5112+k 收到
  //   网关核再一跳一拍加访问延迟十拍                 → DTE 在 5123+k 收到
  //   六十四拍收齐                                  → 5186
  //   DTE 收方向 setup 85 加写倍率表 1               → 5272 认识这个 user
  //   两轮取指 32，走过占位行停在矩阵那一行
  //   矩阵 setup 40 加读表 1 加计算 100              → 5445
  //   一轮取指 16，DTE 发方向 setup 85 加 16 拍       → 5562
  //   一轮取指 16，DTE 退休 setup 85 加 1 拍          → 5664
  const Time init_done = 5186 + tb.params.dte_setup_time +
                         tb.params.bitmap_access_time;
  const Time matrix_done = init_done + 2 * tb.params.ts_logic_time +
                           tb.params.mu_setup_time +
                           tb.params.bitmap_access_time + 100;
  const Time move_done = matrix_done + tb.params.ts_logic_time +
                         tb.params.dte_setup_time + 16;
  const Time retire_done = move_done + tb.params.ts_logic_time +
                           tb.params.dte_setup_time + 1;
  EXPECT_EQ(init_done, 5272u);
  EXPECT_EQ(matrix_done, 5445u);
  EXPECT_EQ(move_done, 5562u);
  EXPECT_EQ(retire_done, 5664u);

  // 出阵列那一段。DTE 每拍交一拍数据给路由器，路由器把 512 字节按 PCIe 的 128 切成
  // 四片，四片各占一拍，所以出口每拍只吐得出四分之一拍数据，第 k 拍要排队 3k 拍。
  //
  //   5546 + 1        路由器取到第一拍
  //   + 4             四片各一拍的服务
  //   + 10 + 5000     访问延迟加跨 node
  //   汇聚点那边前三片攒着不动，第四片到齐才继续，再一跳一拍加访问延迟十拍
  const Time first_beat_last_frag = 5547 + 4 + tb.params.noc_access_delay +
                                    tb.params.cross_node_delay;
  EXPECT_EQ(first_beat_last_frag, 10561u);
  const Time first_beat_landed =
      first_beat_last_frag + 1 + tb.params.noc_access_delay;
  EXPECT_EQ(first_beat_landed, 10572u);

  ASSERT_EQ(run.Latency().size(), 1u);
  EXPECT_EQ(run.Latency()[0].start, 100u);
  EXPECT_EQ(run.Latency()[0].end, first_beat_landed + 4 * 15);
  EXPECT_EQ(run.Result().end_time, 10632u);
}

// ---------------------------------------------------------------- 额度闭环

TEST(BachTwoCores, DownstreamCreditGatesTheSecondUser) {
  RT::Reset(4, 4);
  ClockPtr clk = MakeClock(0, kPeriod);

  const Coord host_coord{0, -1};
  const Coord a_coord{0, 0};
  const Coord b_coord{0, 1};
  const Coord out_coord{0, 2};

  // 上游核：收 Init、验资、把 user 转给下游、退休回注入源
  CoreTables ta(0);
  ta.Task(UnitType::kSkip, Opcode::kUserInit, 0, kVolume, a_coord)
      .Task(UnitType::kCu, Opcode::kDontCare, 0, 0)
      .Task(UnitType::kDte, Opcode::kUserInit, 0, kVolume, b_coord, 0, 1)
      .Task(UnitType::kDte, Opcode::kRetire, 0, 0, host_coord, -1)
      .Credit(1, {1});

  // 下游核：收 Init、算一段、把结果送出去、退休回上游核
  CoreTables tb(1);
  tb.Task(UnitType::kSkip, Opcode::kUserInit, 0, kVolume, b_coord)
      .Task(UnitType::kMc, Opcode::kDontCare, 0, 100)
      .Task(UnitType::kDte, Opcode::kMove, 0, kVolume, out_coord)
      .Task(UnitType::kDte, Opcode::kRetire, 0, 0, a_coord, 0);

  RouteConfig cfg;
  cfg.dim = MakeDim(2);
  cfg.AddExtNode(host_coord, a_coord, Port::kPcieWest);
  cfg.AddExtNode(out_coord, b_coord, Port::kPcieEast);

  Core core_a(clk, ta.Context(), a_coord, &cfg, "core0", 0);
  Core core_b(clk, tb.Context(), b_coord, &cfg, "core1", 0);
  Host host(clk, ta.params, 100, host_coord, &cfg, kVolume,
            ta.params.pcie_bandwidth, ta.params.stream_count,
            Opcode::kUserInit, "host", 0);
  Out out(clk, ta.params, 101, out_coord, &cfg, kVolume,
          ta.params.pcie_bandwidth, "out", 0);

  // 下游只给一份额度：第二个 user 必须等第一个在下游退休才能被转过去
  core_a.AddDownstream(1, 1, CoreType::kNormal);

  Attach(core_a.Rt(), Port::kEast, core_b.Rt());
  Attach(core_a.Rt(), Port::kPcieWest, host.Rt());
  Attach(core_b.Rt(), Port::kPcieEast, out.Rt());

  host.SetUsers({HostUser{0, a_coord, 0, 1}, HostUser{1, a_coord, 0, 1}});

  clk->Continue(30000 * kPeriod);
  RT::JoinAll();

  RunRecorder run;
  run.Collect(core_a.Recorder());
  run.Collect(core_b.Recorder());
  run.Collect(host.Recorder());
  run.Collect(out.Recorder());
  run.Finalize(2);

  EXPECT_TRUE(run.Result().Succeeded());
  ASSERT_EQ(run.Result().completed_uids.size(), 2u);
  EXPECT_EQ(run.Result().completed_uids[0], 0u);
  EXPECT_EQ(run.Result().completed_uids[1], 1u);
  EXPECT_EQ(core_a.Credit().Level(1), 1u);
  EXPECT_EQ(host.CreditLevel(), ta.params.stream_count);
  EXPECT_EQ(core_a.DteUnit().FreeSlotNum(), core_a.DteUnit().TotalSlots());
  EXPECT_EQ(core_b.DteUnit().FreeSlotNum(), core_b.DteUnit().TotalSlots());

  // 第二个 user 卡在等下游额度上，这一段在等待桶里必须找得到
  bool saw_credit_wait = false;
  for (UnitWait const& w : run.Waits()) {
    if (w.reason == WaitReason::kDownstreamCredit && w.uid == 1) {
      saw_credit_wait = true;
      EXPECT_GT(w.end - w.start, 0u);
    }
  }
  EXPECT_TRUE(saw_credit_wait);
}

// ---------------------------------------------------------------- 屏障与退休

TEST(BachTwoCores, BarrierRowWaitsForTheIncomingPacket) {
  RT::Reset(4, 4);
  ClockPtr clk = MakeClock(0, kPeriod);

  const Coord host_coord{0, -1};
  const Coord a_coord{0, 0};
  const Coord b_coord{0, 1};
  const Coord out_coord{0, 2};

  // 上游核先验资再把 user 转给下游，自己算一段，再把结果加法包送过去。
  // 验资那一行不能省：下游退休时会把额度还回来，没扣过就还，账本立刻就不平了。
  CoreTables ta(0);
  ta.Task(UnitType::kSkip, Opcode::kUserInit, 0, kVolume, a_coord)
      .Task(UnitType::kCu, Opcode::kDontCare, 0, 0)
      .Task(UnitType::kDte, Opcode::kUserInit, 0, kVolume, b_coord, 0, 1)
      .Task(UnitType::kMc, Opcode::kDontCare, 0, 200)
      .Task(UnitType::kDte, Opcode::kReduce, 2, kVolume, b_coord, 0, 1)
      .Task(UnitType::kDte, Opcode::kRetire, 0, 0, host_coord, -1)
      .Credit(1, {1});

  // 下游核第 2 行是屏障：它要停在这里，等上游那个加法包到了才继续
  CoreTables tb(1);
  tb.Task(UnitType::kSkip, Opcode::kUserInit, 0, kVolume, b_coord)
      .Task(UnitType::kMc, Opcode::kDontCare, 0, 50)
      .Task(UnitType::kSkip, Opcode::kReduce, 0, kVolume, b_coord)
      .Task(UnitType::kDte, Opcode::kMove, 0, kVolume, out_coord)
      .Task(UnitType::kDte, Opcode::kRetire, 0, 0, a_coord, 0);

  RouteConfig cfg;
  cfg.dim = MakeDim(2);
  cfg.AddExtNode(host_coord, a_coord, Port::kPcieWest);
  cfg.AddExtNode(out_coord, b_coord, Port::kPcieEast);

  Core core_a(clk, ta.Context(), a_coord, &cfg, "core0", 0);
  Core core_b(clk, tb.Context(), b_coord, &cfg, "core1", 0);
  Host host(clk, ta.params, 100, host_coord, &cfg, kVolume,
            ta.params.pcie_bandwidth, ta.params.stream_count,
            Opcode::kUserInit, "host", 0);
  Out out(clk, ta.params, 101, out_coord, &cfg, kVolume,
          ta.params.pcie_bandwidth, "out", 0);

  core_a.AddDownstream(1, ta.params.stream_count, CoreType::kNormal);
  Attach(core_a.Rt(), Port::kEast, core_b.Rt());
  Attach(core_a.Rt(), Port::kPcieWest, host.Rt());
  Attach(core_b.Rt(), Port::kPcieEast, out.Rt());

  host.SetUsers({HostUser{0, a_coord, 0, 1}});

  clk->Continue(30000 * kPeriod);
  RT::JoinAll();

  RunRecorder run;
  run.Collect(core_a.Recorder());
  run.Collect(core_b.Recorder());
  run.Collect(host.Recorder());
  run.Collect(out.Recorder());
  run.Finalize(1);

  EXPECT_TRUE(run.Result().Succeeded());

  // 屏障那一行的等待要落在等待桶里，归因是"等对端数据到达"
  Time barrier_wait = 0;
  for (UnitWait const& w : run.Waits()) {
    if (w.reason == WaitReason::kIncomingBarrier) barrier_wait = w.end - w.start;
  }
  EXPECT_GT(barrier_wait, 0u);

  // 下游那条加法要等上游算完 200 拍再走一趟 NoC，所以它比下游自己那 50 拍晚得多
  EXPECT_GT(run.Result().end_time, 200u);
}

TEST(BachSingleCore, RetiresUsersInStreamOrder) {
  RT::Reset(4, 4);
  ClockPtr clk = MakeClock(0, kPeriod);

  const Coord host_coord{0, -1};
  const Coord core_coord{0, 0};
  const Coord out_coord{0, 1};

  CoreTables tb(0);
  tb.Task(UnitType::kSkip, Opcode::kUserInit, 0, kVolume, core_coord)
      .Task(UnitType::kMc, Opcode::kDontCare, 0, 100)
      .Task(UnitType::kDte, Opcode::kMove, 0, kVolume, out_coord)
      .Task(UnitType::kDte, Opcode::kRetire, 0, 0, host_coord, -1);

  RouteConfig cfg;
  cfg.dim = MakeDim(1);
  cfg.AddExtNode(host_coord, core_coord, Port::kPcieWest);
  cfg.AddExtNode(out_coord, core_coord, Port::kPcieEast);

  Core core(clk, tb.Context(), core_coord, &cfg, "core0", 0);
  Host host(clk, tb.params, 100, host_coord, &cfg, kVolume,
            tb.params.pcie_bandwidth, tb.params.stream_count,
            Opcode::kUserInit, "host", 0);
  Out out(clk, tb.params, 101, out_coord, &cfg, kVolume,
          tb.params.pcie_bandwidth, "out", 0);

  Attach(core.Rt(), Port::kPcieWest, host.Rt());
  Attach(core.Rt(), Port::kPcieEast, out.Rt());

  std::vector<HostUser> list;
  for (uint64_t uid = 0; uid < 4; ++uid) {
    list.push_back(HostUser{uid, core_coord, 0, 1});
  }
  host.SetUsers(list);

  clk->Continue(30000 * kPeriod);
  RT::JoinAll();

  RunRecorder run;
  run.Collect(core.Recorder());
  run.Collect(host.Recorder());
  run.Collect(out.Recorder());
  run.Finalize(4);

  EXPECT_TRUE(run.Result().Succeeded());
  // 硬件规定 StreamID 小的先退休，注入顺序就是退休顺序，也就是完成顺序
  ASSERT_EQ(run.Result().completed_uids.size(), 4u);
  for (uint64_t uid = 0; uid < 4; ++uid) {
    EXPECT_EQ(run.Result().completed_uids[uid], uid);
  }
  EXPECT_EQ(core.Scheduler().ActiveUserNum(), 0u);
  EXPECT_EQ(core.DteUnit().FreeSlotNum(), core.DteUnit().TotalSlots());

  // 后一个 user 的同一条任务要等前一个的上一条做完，这条等待必须出现
  bool saw_stream_wait = false;
  for (UnitWait const& w : run.Waits()) {
    if (w.reason == WaitReason::kStreamPredecessor) saw_stream_wait = true;
  }
  EXPECT_TRUE(saw_stream_wait);
}
