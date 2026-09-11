// TS 的配置面：TASK_CHAIN 与其余配置寄存器的写入、写 TS_INIT_FINISH 时的配置
// 检查、两张掩码的派生，以及按 PID 找搬入任务。
//
// 这几样都是 boot 期一次性配好的，配完不再变，所以与逐拍的调度分开验。

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/ts/cfg_reg.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

TaskEntry Plain(SendUnit unit = SendUnit::kDte) {
  TaskEntry t;
  t.send_unit = unit;
  t.recv_unit = RecvUnit::kDsa;
  t.task_pc = 0x200;
  return t;
}

TaskEntry Last() {
  TaskEntry t = Plain();
  t.end = true;
  return t;
}

TaskEntry Typed(TaskType type, bool credit_en, bool wait_wake = false,
                uint64_t tid = 0) {
  TaskEntry t = Plain();
  t.task_type = type;
  t.credit_en = credit_en;
  t.wait_wake = wait_wake;
  t.p2p_reissue_tid = tid;
  return t;
}

// 按 {下标, 表项} 写一条链、写 TS_INIT_FINISH，返回 TS_STATE。
uint64_t CheckChain(std::vector<std::pair<uint64_t, TaskEntry>> const& chain,
                    bool self_start = false, bool datain = true) {
  uint64_t state = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    CfgReg cfg(clk, "cfg", 0, false);
    cfg.SetSelfStart(self_start);
    for (auto const& one : chain) cfg.WriteTask(one.first, one.second);
    if (datain) cfg.WriteDatainTask(0x300, /*weights_mode=*/false);
    cfg.SetInitFinish();
    state = cfg.TsState();
  }
  RT::Reset();
  return state;
}

}  // namespace

// 每写一项，硬件自动把该项的 TASK_VALID 置起来，软件不必单独写这一位。
TEST(BachTsCfg, WriteSetsValidAutomatically) {
  ClockPtr clk = MakeClock(0, kPeriod);
  CfgReg cfg(clk, "cfg", 0, false);

  EXPECT_FALSE(cfg.Task(0).valid) << "复位后一项都不生效";
  TaskEntry t = Plain();
  t.valid = false;  // 软件没置，硬件该自己置上
  cfg.WriteTask(0, t);
  EXPECT_TRUE(cfg.Task(0).valid);
  EXPECT_EQ(cfg.Task(0).task_pc, 0x200u);
  RT::Reset();
}

// ATTR 的各个位域照写照读。
TEST(BachTsCfg, AttrFieldsAreStoredAsWritten) {
  ClockPtr clk = MakeClock(0, kPeriod);
  CfgReg cfg(clk, "cfg", 0, false);
  TaskEntry t = Plain(SendUnit::kVu);
  t.recv_unit = RecvUnit::kRvOnly;
  t.wait_wake = true;
  t.task_type = TaskType::kP2pReissueIn;
  t.p2p_reissue_tid = 5;
  t.credit_en = true;
  t.path_id = 200;
  cfg.WriteTask(2, t);
  TaskEntry const& got = cfg.Task(2);
  EXPECT_EQ(got.send_unit, SendUnit::kVu);
  EXPECT_EQ(got.recv_unit, RecvUnit::kRvOnly);
  EXPECT_TRUE(got.wait_wake);
  EXPECT_EQ(got.task_type, TaskType::kP2pReissueIn);
  EXPECT_EQ(got.p2p_reissue_tid, 5u);
  EXPECT_TRUE(got.credit_en);
  EXPECT_EQ(got.path_id, 200u);
  RT::Reset();
}

// 两张掩码是 task_chain 的纯函数：配完一次算出来，运行时只查掩码。
TEST(BachTsCfg, MasksArePureFunctionsOfTheChain) {
  ClockPtr clk = MakeClock(0, kPeriod);
  CfgReg cfg(clk, "cfg", 0, false);
  TaskEntry datain = Plain();
  datain.wait_wake = true;
  cfg.WriteTask(0, datain);
  cfg.WriteTask(1, Plain());
  cfg.WriteTask(2, datain);
  cfg.WriteTask(3, Last());
  cfg.WriteDatainTask(0x300, false);
  cfg.SetInitFinish();

  EXPECT_EQ(cfg.DataInMask(), 0b0101u);
  EXPECT_EQ(cfg.ThroughEndMask(), 0b1111u);
  EXPECT_EQ(cfg.TsState() & kStateChainError, 0u) << "这条链本身是合法的";
  RT::Reset();
}

// Router 送来的 PID 对的是 wait_wake 项里 path_id 相同、这个用户还没做完的最低
// 一项；同一个 PID 可以对多个任务。
TEST(BachTsCfg, MatchDatainPicksTheLowestUndoneTaskOfThatPid) {
  ClockPtr clk = MakeClock(0, kPeriod);
  CfgReg cfg(clk, "cfg", 0, false);
  TaskEntry not_datain = Plain();
  not_datain.path_id = 5;
  TaskEntry din5 = Plain();
  din5.wait_wake = true;
  din5.path_id = 5;
  TaskEntry din6 = din5;
  din6.path_id = 6;
  cfg.WriteTask(0, not_datain);
  cfg.WriteTask(1, din5);
  cfg.WriteTask(2, din6);
  cfg.WriteTask(3, din5);
  cfg.WriteTask(4, Last());

  EXPECT_EQ(cfg.MatchDatain(5, 0), 1u) << "不 wait_wake 的第 0 项不算";
  EXPECT_EQ(cfg.MatchDatain(5, 0b10), 3u) << "做完的跳过";
  EXPECT_EQ(cfg.MatchDatain(5, 0b1010), kTaskChainNum) << "都做完了";
  EXPECT_EQ(cfg.MatchDatain(6, 0), 2u);
  EXPECT_EQ(cfg.MatchDatain(7, 0), kTaskChainNum) << "没配这个 PID";
  RT::Reset();
}

// 多项之间的检查：valid 要连续，End 只有一个且后面不许再有 valid。
TEST(BachTsCfg, ChainShapeIsChecked) {
  EXPECT_EQ(CheckChain({{0, Plain()}, {1, Last()}}) & kStateChainError, 0u);
  EXPECT_NE(CheckChain({{0, Plain()}, {2, Last()}}) & kStateChainError, 0u)
      << "valid 不连续";
  EXPECT_NE(CheckChain({{0, Last()}, {1, Last()}}) & kStateChainError, 0u)
      << "两个 End";
  EXPECT_NE(CheckChain({{0, Last()}, {1, Plain()}}) & kStateChainError, 0u)
      << "End 后面还有 valid";
  EXPECT_NE(CheckChain({{0, Plain()}, {1, Plain()}}) & kStateChainError, 0u)
      << "没有 End";
  EXPECT_EQ(CheckChain({}) & kStateChainError, 0u) << "普通模式下不配链不算错";
}

// 单项的检查：重发的搬出与 reduce 要标 credit_en，P2P 重发的搬入要标
// wait_wake；重发搬入配对的那一项必须是重发的搬出任务。
TEST(BachTsCfg, TaskTypeRulesAreChecked) {
  auto err = [](std::vector<std::pair<uint64_t, TaskEntry>> const& c) {
    return (CheckChain(c) & kStateChainError) != 0;
  };
  TaskEntry out = Typed(TaskType::kReissueOut, /*credit_en=*/true);
  EXPECT_FALSE(err({{0, out}, {1, Last()}}));
  EXPECT_TRUE(err({{0, Typed(TaskType::kReissueOut, false)}, {1, Last()}}));
  EXPECT_FALSE(err({{0, Typed(TaskType::kReduce, true)}, {1, Last()}}));
  EXPECT_TRUE(err({{0, Typed(TaskType::kReduce, false)}, {1, Last()}}));

  TaskEntry p2p_in = Typed(TaskType::kP2pReissueIn, false, true, 1);
  EXPECT_FALSE(err({{0, p2p_in}, {1, out}, {2, Last()}}));
  TaskEntry no_wake = Typed(TaskType::kP2pReissueIn, false, false, 1);
  EXPECT_TRUE(err({{0, no_wake}, {1, out}, {2, Last()}})) << "没标 wait_wake";
  EXPECT_TRUE(err({{0, p2p_in}, {1, Plain()}, {2, Last()}}))
      << "配对的那一项不是重发的搬出";
  TaskEntry b_in = Typed(TaskType::kBcastReissueIn, false, true, 1);
  EXPECT_FALSE(err({{0, b_in}, {1, out}, {2, Last()}}));
}

// 自启动模式下 task_chain 与 datain_task 都要配。
TEST(BachTsCfg, SelfStartNeedsBothChainAndDatain) {
  EXPECT_EQ(CheckChain({{0, Plain()}, {1, Last()}}, true, true) &
                kStateChainError,
            0u);
  EXPECT_NE(CheckChain({{0, Plain()}, {1, Last()}}, true, false) &
                kStateChainError,
            0u)
      << "没配 datain_task";
  EXPECT_NE(CheckChain({}, true, true) & kStateChainError, 0u)
      << "没配 task_chain";

  ClockPtr clk = MakeClock(0, kPeriod);
  CfgReg cfg(clk, "cfg", 0, false);
  EXPECT_FALSE(cfg.SelfStartCore()) << "复位值是 0";
  cfg.SetSelfStart(true);
  EXPECT_TRUE(cfg.SelfStartCore());
  RT::Reset();
}

// 权重加载时不查配置。
TEST(BachTsCfg, WeightsModeSkipsTheCheck) {
  ClockPtr clk = MakeClock(0, kPeriod);
  CfgReg cfg(clk, "cfg", 0, false);
  cfg.SetSelfStart(true);  // 自启动又没配链，不是权重加载就该报错
  cfg.WriteDatainTask(0x300, /*weights_mode=*/true);
  cfg.SetInitFinish();
  EXPECT_TRUE(cfg.WeightsMode());
  EXPECT_EQ(cfg.TsState(), 0u);
  RT::Reset();
}

// STREAM_NUM 超过 16 的值只能写入 16。
TEST(BachTsCfg, StreamNumAbove16IsWrittenAs16) {
  ClockPtr clk = MakeClock(0, kPeriod);
  CfgReg cfg(clk, "cfg", 0, false);
  cfg.SetStreamNum(1);
  EXPECT_EQ(cfg.StreamNum(), 1u);
  cfg.SetStreamNum(kStreamNum);
  EXPECT_EQ(cfg.StreamNum(), kStreamNum);
  cfg.SetStreamNum(31);
  EXPECT_EQ(cfg.StreamNum(), kStreamNum);
  EXPECT_DEATH(cfg.SetStreamNum(0), "");
  RT::Reset();
}

// ROUTER_TABLE 按 PID 存方向与 VCID。
TEST(BachTsCfg, RouterTableHoldsDirAndVcid) {
  ClockPtr clk = MakeClock(0, kPeriod);
  CfgReg cfg(clk, "cfg", 0, false);
  cfg.WriteRouterTable(9, 0b1001, 2);
  EXPECT_EQ(cfg.Route(9).dir, 0b1001u);
  EXPECT_EQ(cfg.Route(9).vcid, 2u);
  EXPECT_EQ(cfg.Route(10).dir, 0u) << "复位值是 0";
  EXPECT_EQ(cfg.Route(10).vcid, 0u);
  RT::Reset();
}

// 超前发送窗口按 path 配：开没开、窗口多大，两样各自记。
TEST(BachTsCfg, FlowControlWindowIsPerPath) {
  ClockPtr clk = MakeClock(0, kPeriod);
  CfgReg cfg(clk, "cfg", 0, false);
  EXPECT_FALSE(cfg.FlowCtlEn(3)) << "复位后不开";
  cfg.SetFlowCtl(3, true, 4);
  EXPECT_TRUE(cfg.FlowCtlEn(3));
  EXPECT_EQ(cfg.FlowCtlWindow(3), 4u);
  EXPECT_FALSE(cfg.FlowCtlEn(4)) << "只配了一条 path";
  RT::Reset();
}
