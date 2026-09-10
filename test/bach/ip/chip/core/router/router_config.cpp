// Router 的配置面：RouterTable 的多副本提交、Skip Mask、Credit Bypass Route，
// 以及进核那一位的两种取法。
//
// 这几样都是构造期或 boot 期写进去的，与逐拍的数据面分开验。

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/router/router_table.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

RouteEntry SomeEntry() {
  RouteEntry e;
  e.op_type = OpType::kTransfer;
  e.flow_dir = kFlowRight;
  e.path_core_bypass = true;
  return e;
}

// 每拍抄一份表的状态：Busy 与各副本的 valid，主线程读 Logic64 只能读到 t=0。
class TableProbe : public BachModule {
 public:
  TableProbe(ClockPtr c, RouterTable& t)
      : BachModule(c, "probe", 0, false), tab(t) {}

  // 第几拍上第几份副本已经写进去了。
  std::vector<std::pair<uint64_t, uint64_t>> copies_at;
  uint64_t done_at = 0;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    uint64_t n = 0;
    for (uint64_t c = 0; c < RouterTable::kCopyNum; ++c) {
      if (tab.Lookup(c, 7).valid) ++n;
    }
    copies_at.push_back({now, n});
    if (tab.CommitDone() != 0 && done_at == 0) done_at = now;
  }

 private:
  RouterTable& tab;
};

// 到点发一笔写事务。写表的是配置面，与探针分在两个协程里会互相看不到本拍的值，
// 所以写与探都放在这一个协程里。
class TableWriter : public BachModule {
 public:
  TableWriter(ClockPtr c, RouterTable& t, uint64_t when)
      : BachModule(c, "writer", 0, false), tab(t), at(when) {}

 protected:
  void Step() override {
    if (CycleNow() != at) return;
    tab.Write(7, SomeEntry());
  }

 private:
  RouterTable& tab;
  uint64_t at;
};

}  // namespace

// 复位后全条目 bypass / no-op：配置写入前不投递任何包，各副本一律无效。
TEST(BachRouterTable, ResetLeavesEveryEntryNoOp) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  RouterTable tab(clk, "rtab", 0, false);

  for (uint64_t c = 0; c < RouterTable::kCopyNum; ++c) {
    for (uint64_t p = 0; p < kPathNum; ++p) {
      RouteEntry const& e = tab.Lookup(c, p);
      EXPECT_FALSE(e.valid) << "copy=" << c << " path=" << p;
      EXPECT_TRUE(e.path_core_bypass);
      EXPECT_EQ(e.flow_dir, 0u);
    }
  }
  EXPECT_EQ(tab.SkipMask(), 0u);
  RT::Reset();
}

// 多副本提交：一笔写事务逐份写进全部副本，全部写完才报完成，中途不暴露
// 部分新部分旧的状态。
TEST(BachRouterTable, CommitWritesEveryCopyBeforeReporting) {
  std::vector<std::pair<uint64_t, uint64_t>> trace;
  uint64_t done_at = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RouterTable tab(clk, "rtab", 0, false);
    TableWriter writer(clk, tab, 2);
    TableProbe probe(clk, tab);
    // 表自己不打拍时要有人推它：写与推放在同一个协程里，次序才定死。
    class Driver : public BachModule {
     public:
      Driver(ClockPtr c, RouterTable& t, TableWriter& w, TableProbe& p)
          : BachModule(c, "drv"), tab(t), wr(w), pr(p) {}

     protected:
      void Step() override {
        wr.RunStep();
        tab.RunStep();
        pr.RunStep();
      }

     private:
      RouterTable& tab;
      TableWriter& wr;
      TableProbe& pr;
    };
    Driver drv(clk, tab, writer, probe);
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    trace = probe.copies_at;
    done_at = probe.done_at;
  }
  RT::Reset();

  // 副本数只涨不落，一拍涨一份，最后全部写上。
  uint64_t prev = 0;
  for (auto const& kv : trace) {
    EXPECT_GE(kv.second, prev) << "第 " << kv.first << " 拍副本数回退了";
    EXPECT_LE(kv.second - prev, 1u) << "一拍最多写一份";
    prev = kv.second;
  }
  EXPECT_EQ(prev, RouterTable::kCopyNum) << "六份副本都要写到";
  EXPECT_GT(done_at, 0u) << "全部写完之后报一次完成";
}

// 构造期铺满走 Preload，跳过提交状态机：六份副本一次到位。
TEST(BachRouterTable, PreloadFillsEveryCopyAtOnce) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  RouterTable tab(clk, "rtab", 0, false);
  tab.Preload(9, SomeEntry());
  for (uint64_t c = 0; c < RouterTable::kCopyNum; ++c) {
    EXPECT_TRUE(tab.Lookup(c, 9).valid) << "copy=" << c;
  }
  EXPECT_FALSE(tab.Busy());
  RT::Reset();
}

// Skip Mask 与 RouterTable 分开配：改一个不动另一个。
TEST(BachRouterTable, SkipMaskIsConfiguredApartFromTheTable) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  RouterTable tab(clk, "rtab", 0, false);

  tab.Preload(3, SomeEntry());
  EXPECT_EQ(tab.SkipMask(), 0u) << "写表不动 Skip Mask";

  tab.SetSkipMask(0b0100);
  EXPECT_TRUE(tab.CoreSkipped(2));
  EXPECT_FALSE(tab.CoreSkipped(0));
  EXPECT_FALSE(tab.CoreSkipped(3));
  EXPECT_TRUE(tab.Lookup(0, 3).valid) << "改 Skip Mask 不动表项";
  RT::Reset();
}

// 业务 credit 旁路：每个输入端口一个静态输出方向 Mask，不查 RouterTable。
TEST(BachRouterTable, CreditBypassIsAStaticPerPortMask) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  RouterTable tab(clk, "rtab", 0, false);

  for (uint64_t p = 0; p < kDirNum; ++p) {
    EXPECT_EQ(tab.CreditBypass(p), 0u) << "复位后一个方向都不转";
  }
  tab.SetCreditBypass(kDirLeft, kFlowRight | kFlowMid);
  EXPECT_EQ(tab.CreditBypass(kDirLeft), kFlowRight | kFlowMid);
  EXPECT_EQ(tab.CreditBypass(kDirRight), 0u) << "只配了一个口，别的不受影响";
  // 与 path 无关：换哪条 path 都是这一份静态 Mask。
  tab.Preload(5, SomeEntry());
  EXPECT_EQ(tab.CreditBypass(kDirLeft), kFlowRight | kFlowMid);
  RT::Reset();
}

// 进核由两者二选一：enable 为 0 时看 bypass 位，为 1 时看包头 mask 的第 idx 位。
TEST(BachRouterTable, CoreMaskIndexPicksTheBit) {
  RouteEntry by_bypass;
  by_bypass.path_core_mask_enable = false;
  by_bypass.path_core_bypass = false;
  EXPECT_TRUE(by_bypass.EntersCore(0)) << "bypass 为 0 就进核，与包头无关";
  EXPECT_TRUE(by_bypass.EntersCore(0xFFFF));

  by_bypass.path_core_bypass = true;
  EXPECT_FALSE(by_bypass.EntersCore(0xFFFF)) << "bypass 为 1 就不进核";

  RouteEntry by_mask;
  by_mask.path_core_mask_enable = true;
  by_mask.path_core_mask_idx = 5;
  by_mask.path_core_bypass = true;  // enable 打开后这一位不起作用
  EXPECT_TRUE(by_mask.EntersCore(1ull << 5)) << "看的是第 5 位";
  EXPECT_FALSE(by_mask.EntersCore(1ull << 4));
  EXPECT_FALSE(by_mask.EntersCore(0));

  // 位到 core 的对应不是固定编码：同一份包头，两个 core 看不同的位。
  RouteEntry other = by_mask;
  other.path_core_mask_idx = 4;
  EXPECT_FALSE(other.EntersCore(1ull << 5));
  EXPECT_TRUE(other.EntersCore(1ull << 4));
}
