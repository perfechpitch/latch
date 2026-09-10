// TS 的配置面：task_chain 的写入、写 TS_INIT_FINISH 时的合规性检查、四张掩码
// 的派生，以及 path_task_map 这张反查表。
//
// 这几样都是 boot 期一次性配好的，配完不再变，所以与逐拍的调度分开验。

#include <gtest/gtest.h>

#include <string>

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
  t.dsa_en = true;
  t.exe_mask = true;
  t.task_pc = 0x200;
  return t;
}

// 一条最简单的合法链：两项，末项是 End。
void FillTwoTaskChain(CfgReg& cfg) {
  TaskEntry t0 = Plain();
  TaskEntry t1 = Plain();
  t1.end = true;
  cfg.WriteTask(0, t0);
  cfg.WriteTask(1, t1);
  cfg.WriteDatainTask(0x300, /*weights_mode=*/false);
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

// 硬件位域里没有的软件侧属性另存一份，与硬件表分开。
TEST(BachTsCfg, SoftwareAttributesAreStoredApart) {
  ClockPtr clk = MakeClock(0, kPeriod);
  CfgReg cfg(clk, "cfg", 0, false);

  TaskEntry t = Plain();
  t.exe_dest = 7;
  t.reduce_num = 3;
  t.reduce = true;
  cfg.WriteTask(2, t);
  EXPECT_EQ(cfg.Task(2).exe_dest, 7u);
  EXPECT_EQ(cfg.Task(2).reduce_num, 3u);
  EXPECT_TRUE(cfg.Task(2).dsa_en);
  RT::Reset();
}

// 四张掩码是 task_chain 的纯函数：配完一次算出来，运行时只查掩码。
TEST(BachTsCfg, MasksArePureFunctionsOfTheChain) {
  ClockPtr clk = MakeClock(0, kPeriod);
  CfgReg cfg(clk, "cfg", 0, false);

  TaskEntry datain = Plain();
  datain.wait_wake = true;          // datain
  TaskEntry reissue = Plain();
  reissue.broadcast_reissue = true;
  TaskEntry plain = Plain();
  plain.exe_mask = false;           // 按用户区分
  TaskEntry last = Plain();
  last.end = true;

  cfg.WriteTask(0, datain);
  cfg.WriteTask(1, reissue);
  cfg.WriteTask(2, plain);
  cfg.WriteTask(3, last);
  cfg.WritePathMap(0, 0);
  cfg.WriteDatainTask(0x300, false);
  cfg.SetInitFinish();

  EXPECT_EQ(cfg.DataInMask(), 0b0001u);
  EXPECT_EQ(cfg.ReissueMask(), 0b0010u);
  EXPECT_EQ(cfg.EndMask(), 0b1000u);
  EXPECT_EQ(cfg.ExeMask(), 0b1011u) << "第 2 项按用户区分，那一位是 0";
  EXPECT_EQ(cfg.TsState() & kStateChainError, 0u) << "这条链本身是合法的";
  RT::Reset();
}

// path_task_map 把 path_id 翻译成任务链上的第几步，且必须指回配它的那一项。
TEST(BachTsCfg, PathTaskMapMustPointBack) {
  ClockPtr clk = MakeClock(0, kPeriod);
  {
    CfgReg cfg(clk, "cfg", 0, false);
    TaskEntry t0 = Plain();
    t0.path_id = 5;
    TaskEntry t1 = Plain();
    t1.end = true;
    cfg.WriteTask(0, t0);
    cfg.WriteTask(1, t1);
    cfg.WritePathMap(5, 0);   // 指回第 0 项
    cfg.WriteDatainTask(0x300, false);
    cfg.SetInitFinish();
    EXPECT_EQ(cfg.PathMap(5).task_id, 0u);
    EXPECT_TRUE(cfg.PathMap(5).valid);
    EXPECT_EQ(cfg.TsState() & kStateChainError, 0u);
  }
  {
    CfgReg cfg(clk, "cfg", 0, false);
    TaskEntry t0 = Plain();
    t0.path_id = 5;
    TaskEntry t1 = Plain();
    t1.end = true;
    cfg.WriteTask(0, t0);
    cfg.WriteTask(1, t1);
    cfg.WritePathMap(5, 1);   // 指错了
    cfg.WriteDatainTask(0x300, false);
    cfg.SetInitFinish();
    EXPECT_NE(cfg.TsState() & kStateChainError, 0u) << "指错要报错";
  }
  {
    CfgReg cfg(clk, "cfg", 0, false);
    TaskEntry t0 = Plain();
    t0.path_id = 5;
    TaskEntry t1 = Plain();
    t1.end = true;
    cfg.WriteTask(0, t0);
    cfg.WriteTask(1, t1);
    // 干脆没配这条 path
    cfg.WriteDatainTask(0x300, false);
    cfg.SetInitFinish();
    EXPECT_NE(cfg.TsState() & kStateChainError, 0u) << "缺表项也要报错";
  }
  RT::Reset();
}

// 一条链上只许有一个 SELF_START，且普通 core 不该配它。
TEST(BachTsCfg, SelfStartIsCheckedAtInitFinish) {
  ClockPtr clk = MakeClock(0, kPeriod);
  {
    // B core 配一个 self_start：合法。
    CfgReg cfg(clk, "cfg", 0, false);
    cfg.SetCoreType(CoreType::kBroadcast);
    TaskEntry t0 = Plain();
    t0.self_start = true;
    TaskEntry t1 = Plain();
    t1.end = true;
    cfg.WriteTask(0, t0);
    cfg.WriteTask(1, t1);
    cfg.WriteDatainTask(0x300, false);
    cfg.SetInitFinish();
    EXPECT_EQ(cfg.TsState() & kStateChainError, 0u);
    EXPECT_TRUE(cfg.SelfStartCore());
  }
  {
    // 两个 self_start：不合法。
    CfgReg cfg(clk, "cfg", 0, false);
    cfg.SetCoreType(CoreType::kBroadcast);
    TaskEntry t0 = Plain();
    t0.self_start = true;
    TaskEntry t1 = Plain();
    t1.self_start = true;
    t1.end = true;
    cfg.WriteTask(0, t0);
    cfg.WriteTask(1, t1);
    cfg.WriteDatainTask(0x300, false);
    cfg.SetInitFinish();
    EXPECT_NE(cfg.TsState() & kStateChainError, 0u);
  }
  {
    // 普通 core 配了 self_start：不合法。
    CfgReg cfg(clk, "cfg", 0, false);
    cfg.SetCoreType(CoreType::kNormal);
    TaskEntry t0 = Plain();
    t0.self_start = true;
    TaskEntry t1 = Plain();
    t1.end = true;
    cfg.WriteTask(0, t0);
    cfg.WriteTask(1, t1);
    cfg.WriteDatainTask(0x300, false);
    cfg.SetInitFinish();
    EXPECT_NE(cfg.TsState() & kStateChainError, 0u);
    EXPECT_FALSE(cfg.SelfStartCore());
  }
  RT::Reset();
}

// 一项都没配就写 TS_INIT_FINISH 是错的；配好了两项才算配过链。
TEST(BachTsCfg, EmptyChainIsAnError) {
  ClockPtr clk = MakeClock(0, kPeriod);
  {
    CfgReg cfg(clk, "cfg", 0, false);
    cfg.SetInitFinish();
    EXPECT_NE(cfg.TsState() & kStateChainError, 0u);
    EXPECT_EQ(cfg.TsState() & kStateChainCfg, 0u);
  }
  {
    CfgReg cfg(clk, "cfg", 0, false);
    FillTwoTaskChain(cfg);
    cfg.SetInitFinish();
    EXPECT_NE(cfg.TsState() & kStateChainCfg, 0u);
    EXPECT_NE(cfg.TsState() & kStateDatainCfg, 0u);
    EXPECT_EQ(cfg.TsState() & kStateChainError, 0u);
  }
  RT::Reset();
}

// stream_num 只能配 1 到 16。
TEST(BachTsCfg, StreamNumIsBounded) {
  ClockPtr clk = MakeClock(0, kPeriod);
  CfgReg cfg(clk, "cfg", 0, false);
  cfg.SetStreamNum(1);
  EXPECT_EQ(cfg.StreamNum(), 1u);
  cfg.SetStreamNum(kStreamNum);
  EXPECT_EQ(cfg.StreamNum(), kStreamNum);
  EXPECT_DEATH(cfg.SetStreamNum(0), "");
  EXPECT_DEATH(cfg.SetStreamNum(kStreamNum + 1), "");
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
