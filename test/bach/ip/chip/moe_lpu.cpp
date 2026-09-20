// 一层 MoE 按 EP6+TP8 的 KN 拆分摊在 48 颗 chip 上：12 层 × 4 列，两层一个 EP 组
// 共 6 组。token 只送进第一组左上角那颗 chip 的 B core，它留一份在自己的 Matrix
// Mem，一边广播给本组 64 个计算 core，一边把同一份往下一组的 B core 转，逐个传到
// 六个组。每颗 chip 各做 chip 内归约、FC2 输入广播与 concat；每行 4 颗 chip 的结
// 果沿行链归约进本行 R core，12 个 R core 逐行串成一条链相加。
//
// 这一份跑得久，与其他用例分开一个目标，方便单独跑。

#include <iostream>

#include "test/bach/ip/chip/moe_common.h"

using namespace latch;
using namespace latch::bach;
using namespace latch::bach::moetest;

namespace {

constexpr uint64_t kLpuChips = kGridX * kGridY;
constexpr uint64_t kRowsPerGroup = 2;

// 记波形的 chip 数，从第 0 颗数起。48 颗全记扛得住；只想看某一段就把这个数调
// 小、把那一段的头一颗挪到 0 号位。
constexpr uint64_t kTraceChips = kLpuChips;

// 同时注入的 token 数：每个 token 各占一条 stream 片与一个 R core 槽，数据相同、
// 结果该一模一样，用来验证多 token 互不覆盖。同飞上限 kn::kStreamNum（16）。
constexpr uint64_t kTokenCount = 1;
// 相邻两个 token 的注入间隔（拍）。小于单个 token 的流程长时它们是并发同飞的。
constexpr uint64_t kTokenGap = 4000;

// 第 gy 行的 R core 坐在那一行最后一颗 chip 上。
uint64_t RcoreChip(uint64_t gy) { return gy * kGridX + kGridX - 1; }

}  // namespace

// 一层 MoE 摊在 48 颗 chip 上：一个 token 广播进 6 个 EP 组共 384 个计算 core，
// 每颗 chip 在 dot core 上拼出 concat，每行 4 颗 chip 的 concat 沿行链归约进本行
// R core，12 个 R core 逐行相加，第 11 行 R core 的结果经 core4 从右下角那颗
// chip 的 E 口出来。每颗 chip 的 concat、每行的行链结果、每个 R core 收到的上一
// 行累加结果与出口上的结果都与参考实现逐字节相同。
TEST(BachMoeLpu, OneLayerAcrossFortyEightChips) {
  constexpr uint64_t kMaxCycles = 200000;
  Vectors want = ReadVectors("moe_lpu.txt");
  ASSERT_FALSE(want.Empty())
      << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";

  std::vector<MessagePtr> got;
  std::vector<uint64_t> inflight;
  std::vector<std::vector<uint8_t>> concat, rows, prev;
  {
    EnsureSlots(kLpuChips * (kMaxCorePerChip + 1));
    TraceInto("moe_lpu");
    ClockPtr clk = MakeClock(0, kPeriod);
    std::vector<std::unique_ptr<Chip>> owned;
    std::vector<Chip*> all;
    for (uint64_t i = 0; i < kLpuChips; ++i) {
      SetTraceDisabled(i >= kTraceChips);
      ChipCfg cfg;
      cfg.gx = GxOfChip(i);
      cfg.gy = GyOfChip(i);
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
    // 编出来，拓扑、任务链、路由表、进核配置都在里面。
    BundleStat st = LoadBundle(all, BundleRoot(), "moe_lpu");
    ASSERT_EQ(st.chips, kLpuChips);
    // 每颗 chip 的 10 个 core 各一条 CORE 记录。
    ASSERT_EQ(st.cores, kLpuChips * kMaxCorePerChip);
    for (uint64_t i = 0; i < kLpuChips; ++i) {
      uint64_t gy = GyOfChip(i);
      uint64_t index = gy % kRowsPerGroup * kGridX + GxOfChip(i);
      for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
        SetUpCoreData(all[i]->GetCore(CoreOfSlot(all[i]->Gx(), slot)),
                      gy / kRowsPerGroup, index, slot);
      }
    }
    // 第 0 行 R core 是链首，只有本行结果，槽的另一半一直是 0，等一笔就走。
    all[RcoreChip(0)]->GetCore(kRcoreId).Smem().Poke(kn::kRcHeadOff,
                                                     {1, 0, 0, 0});

    std::vector<MessagePtr> tokens;
    for (uint64_t seq = 0; seq < kTokenCount; ++seq) {
      tokens.push_back(MakeToken(kBcastInPath, BcoreLand(seq), seq));
    }
    SpreadHarness harness(clk, all, GridLinks(kGridY, kGridX), feed, sink, 0,
                          kChipW, kLpuChips - 1, kChipE, /*at=*/2, kMaxCycles,
                          tokens.front());
    harness.tokens = tokens;
    harness.token_gap = kTokenGap;
    clk->Continue();
    RT::JoinAll();
    got = harness.out_msgs;
    inflight = harness.inflight;
    std::cerr << "  停钟在第 " << harness.stopped_at << " 拍\n";
    for (uint64_t i = 0; i < kLpuChips; ++i) {
      for (uint64_t s = 0; s < kTokenCount; ++s) {
        concat.push_back(ConcatOf(*all[i], s));
      }
    }
    for (uint64_t gy = 0; gy < kGridY; ++gy) {
      for (uint64_t s = 0; s < kTokenCount; ++s) {
        rows.push_back(RcoreHalf(*all[RcoreChip(gy)], 0, kUserId + s));
        prev.push_back(RcoreHalf(*all[RcoreChip(gy)], 1, kUserId + s));
      }
    }
  }
  TraceDone();
  RT::Reset();
  for (uint64_t i = 0; i < kLpuChips; ++i) {
    for (uint64_t s = 0; s < kTokenCount; ++s) {
      ExpectSame(concat[i * kTokenCount + s],
                 want.Bytes("concat" + std::to_string(i)),
                 "chip " + std::to_string(i) + " token " + std::to_string(s) +
                     " 的 concat");
    }
  }
  for (uint64_t gy = 0; gy < kGridY; ++gy) {
    for (uint64_t s = 0; s < kTokenCount; ++s) {
      ExpectSame(rows[gy * kTokenCount + s],
                 want.Bytes("row" + std::to_string(gy)),
                 "第 " + std::to_string(gy) + " 行 token " + std::to_string(s) +
                     " 落进 R core 的行链结果");
      if (gy > 0) {
        ExpectSame(prev[gy * kTokenCount + s],
                   want.Bytes("rcore" + std::to_string(gy - 1)),
                   "第 " + std::to_string(gy) + " 行 token " + std::to_string(s) +
                       " R core 收到的上一行累加结果");
      }
    }
  }
  CheckInflight(inflight);
  CheckOut(got, want.Bytes("out"), kn::RcLand(kUserId, 1), kTokenCount);
}
