// 一层 MoE 摊在 48 颗 chip 上：12 层 × 4 列，两层一个 EP 组共 6 组。token 只送
// 进第一组左上角那颗 chip 的 B core，它留一份在自己的 Matrix Mem，一边广播给
// 本组 64 个计算 core，一边把同一份往下一组的 B core 转，逐个传到六个组。每组
// 的 64 份部分和沿蛇形链归约成组结果落进本组的 R core，六个 R core 串成一条链
// 逐组相加。
//
// 这一份跑一次十几分钟，与其他用例分开一个目标，方便单独跑。

#include <iostream>

#include "test/bach/ip/chip/moe_common.h"

using namespace latch;
using namespace latch::bach;
using namespace latch::bach::moetest;

namespace {

// ── 一整个 LPU：48 颗 chip、6 个 EP 组 ──
//
// 12 层 × 4 列，两层一个 EP 组。层内左右相接、层间上下相接，与 LPU 的
// WireRow / WireCol 同一套接法。
constexpr uint64_t kLpuCols = 4;
constexpr uint64_t kLpuRows = 12;
constexpr uint64_t kLpuChips = kLpuCols * kLpuRows;
constexpr uint64_t kLpuGroups = 6;

constexpr uint64_t kRcHeadOff = 0x0380;
// 记波形的 chip 数，从第 0 颗数起。48 颗全记是一万二千五百个信号，扛得住；只想看
// 某一段就把这个数调小、把那一段的头一颗挪到 0 号位。
constexpr uint64_t kTraceChips = kLpuChips;

// 推的次序按归约链倒着来：末级先做。
std::vector<uint64_t> RunOrder(SpreadCase const& want) {
  std::vector<uint64_t> run;
  std::vector<bool> seen(want.chips, false);
  for (uint64_t i = want.order.size(); i > 0; --i) {
    uint64_t chip = want.order[i - 1] / kCorePerChip;
    if (seen[chip]) continue;
    seen[chip] = true;
    run.push_back(chip);
  }
  return run;
}

std::vector<ChipPair> LpuLinks() {
  std::vector<ChipPair> v;
  for (uint64_t gy = 0; gy < kLpuRows; ++gy) {
    for (uint64_t gx = 0; gx < kLpuCols; ++gx) {
      uint64_t a = gy * kLpuCols + gx;
      if (gx + 1 < kLpuCols) v.push_back({a, kChipE, a + 1, kChipW});
      if (gy + 1 < kLpuRows) v.push_back({a, kChipS, a + kLpuCols, kChipN});
    }
  }
  return v;
}

// 第 g 组的 R core：那一组最后一颗 chip 是 (2g+1, 3)，R core 坐在它的 core9。
uint64_t RcoreChip(uint64_t g) { return (2 * g + 1) * kLpuCols + kLpuCols - 1; }
constexpr uint64_t kRcoreId = kMaxCorePerChip - 1;

}  // namespace

// 一层 MoE 摊在 48 颗 chip 上：一个 token 广播进 6 个 EP 组共 384 个计算 core，
// 每组的 64 份部分和沿蛇形链归约成组结果落进本组的 R core，六个 R core 串成一
// 条链逐组相加，链尾从最后一颗 chip 的 S 口出来。与参考实现逐 bit 相同。
TEST(BachMoeLpu, OneLayerAcrossFortyEightChips) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  constexpr uint64_t kMaxCycles = 200000;
  SpreadCase want = ReadCase("moe_lpu.txt");
  ASSERT_GT(want.k, 0u) << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";
  ASSERT_EQ(want.chips, kLpuChips);

  std::vector<MessagePtr> got;
  std::vector<uint64_t> inflight;
  std::vector<std::vector<uint8_t>> landed;
  {
    EnsureSlots(kLpuChips * (kMaxCorePerChip + 1));
    TraceInto("moe_lpu");
    ClockPtr clk = MakeClock(0, kPeriod);
    std::vector<std::unique_ptr<Chip>> owned;
    std::vector<Chip*> all;
    for (uint64_t i = 0; i < kLpuChips; ++i) {
      SetTraceDisabled(i >= kTraceChips);
      ChipCfg cfg;
      cfg.shape = ShapeOfGx(i % kLpuCols);
      cfg.gx = i % kLpuCols;
      cfg.gy = i / kLpuCols;
      // 第一列 chip 上是 B core 的 token 槽位标志，最后一列上是 R core 的两半
      // 到齐标志。中间两列两样都没有，配什么都不影响。
      if (cfg.shape == ChipShape::kFirst) {
        cfg.inbound_flag_base = kBcFlagOff;
        cfg.inbound_entry_bytes = kBcTokenBytes;
      } else {
        cfg.inbound_flag_base = kRcFlagOff;
        cfg.inbound_entry_bytes = kRcFlagEntryBytes;
      }
      cfg.core_tick = kCoreTick;
      cfg.chip_tick = kChipTick;
      owned.push_back(
          std::make_unique<Chip>(clk, "chip" + std::to_string(i), cfg));
      all.push_back(owned.back().get());
    }
    SetTraceDisabled(false);
    C2cBridge feed(clk, "feed", StubCfg());
    C2cBridge sink(clk, "sink", StubCfg());

    // 配置表与 kernel 都从 bundle 装：那一份由 compiler 按 topo/moe_lpu.json
    // 编出来，拓扑、任务链、路由表三样都在里面。
    BundleStat st = LoadBundle(all, BundleRoot(), "moe_lpu");
    ASSERT_EQ(st.chips, kLpuChips);
    // 第一列与最后一列各 10 个 core，中间两列各 8 个。
    ASSERT_EQ(st.cores, 2 * kLpuRows * (kMaxCorePerChip + kCorePerChip));
    for (uint64_t i = 0; i < kLpuChips; ++i) {
      ChipShape shape = all[i]->Shape();
      for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
        SetUpCoreData(all[i]->GetCore(CoreOfSlot(shape, slot)), want,
                      i * kCorePerChip + slot);
      }
    }

    // 链首那一组的 R core 只有自己的组结果，槽的另一半一直是 0，等一笔就走。
    all[RcoreChip(0)]->GetCore(kRcoreId).Smem().Poke(kRcHeadOff, {1, 0, 0, 0});

    MessagePtr token = MakeToken(want);
    // 进第一个 B core 那一路与它广播出去那一路各走一个 path；落点按第几笔算。
    token->path_id = kBcastInPath;
    token->dst_addr = BcoreLand(0);
    SpreadHarness harness(clk, all, LpuLinks(), feed, sink, 0, kChipW,
                          RcoreChip(kLpuGroups - 1), kChipS, RunOrder(want),
                          /*at=*/2, kMaxCycles, token);
    clk->Continue();
    RT::JoinAll();
    got = harness.out_msgs;
    inflight = harness.inflight;
    std::cerr << "  停钟在第 " << harness.stopped_at << " 拍\n";
    for (uint64_t i = 0; i < kLpuChips; ++i) {
      ChipShape shape = all[i]->Shape();
      for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
        landed.push_back(PartialOf(all[i]->GetCore(CoreOfSlot(shape, slot))));
      }
    }
  }
  TraceDone();
  RT::Reset();
  CheckResult(want, landed, inflight, got);
}

