// 一层 MoE 按 EP6+TP8 的 KN 拆分摊在一颗 chip、一行两颗 chip 与一个 EP 组上。
//
// 每颗 chip 上：B core 把 token 广播进 8 个计算 core，各 core 算 token 第 s 段的
// FC1、FC3 部分和，沿 chip 内归约链逐跳在 Router 上相加，链尾交回 dot core；dot
// core 做 silu·dot·量化，把 FC2 输入广播回本 chip；8 个 core 各算 FC2 第 s 段，另
// 外 7 个各走自己那一条 path 发给 dot core，拼成 concat。一行 chip 的 dot core 沿
// 行链逐跳归约进本行 R core，各行 R core 逐行相加。
//
// 与 core/moe.cpp 那一份的区别：那一份只在一个 core 内，这里多出 chip 内归约、
// FC2 输入广播、concat、行链与 R core 那几段。配置表与 kernel 都从编译器产的
// bundle 装。

#include <iostream>

#include "test/bach/ip/chip/moe_common.h"

using namespace latch;
using namespace latch::bach;
using namespace latch::bach::moetest;

namespace {

// 建 n 颗 chip：第 i 颗在第 gy[i] 层、第 gx[i] 列。
std::vector<std::unique_ptr<Chip>> MakeChips(
    ClockPtr clk, std::vector<uint64_t> const& gx,
    std::vector<uint64_t> const& gy) {
  std::vector<std::unique_ptr<Chip>> owned;
  for (uint64_t i = 0; i < gx.size(); ++i) {
    ChipCfg cfg;
    cfg.gx = gx[i];
    cfg.gy = gy[i];
    cfg.core_tick = kCoreTick;
    cfg.chip_tick = kChipTick;
    owned.push_back(
        std::make_unique<Chip>(clk, "chip" + std::to_string(i), cfg));
  }
  return owned;
}

std::vector<Chip*> Raw(std::vector<std::unique_ptr<Chip>> const& owned) {
  std::vector<Chip*> all;
  for (auto const& c : owned) all.push_back(c.get());
  return all;
}

// 一颗 chip 的 8 个计算 core 铺好数据。chip 是它在 EP 组里的序号。
void SetUpChipData(Chip& chip, uint64_t group, uint64_t index) {
  for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
    SetUpCoreData(chip.GetCore(CoreOfSlot(chip.Gx(), slot)), group, index,
                  slot);
  }
}

// R core 链首那一个只有本行结果，槽的另一半一直是 0，等一笔就走。
void MarkRcoreHead(Chip& chip) {
  chip.GetCore(kRcoreId).Smem().Poke(kn::kRcHeadOff, {1, 0, 0, 0});
}

}  // namespace

// 一颗第一列 chip 上的完整一段：token 从 W 口进来，坐在那个口上的 core5 不派角
// 色、只往上转一跳，B core（core0）留一份在自己的 Matrix Mem，再广播给 8 个计算
// core。部分和归约进 dot core core9，FC2 输入广播回本 chip，8 段 FC2 在 dot core
// 上拼成 concat。它是行首也是行尾，行链只有它一跳，结果经 core4 从 E 口出来。各
// 步中间量与出口上的结果都与参考实现逐字节相同。
TEST(BachMoeChip, BcoreStartsTheBroadcast) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  constexpr uint64_t kMaxCycles = 20000;
  Vectors want = ReadVectors("moe_chip.txt");
  ASSERT_FALSE(want.Empty())
      << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";

  std::vector<MessagePtr> got;
  std::vector<uint64_t> inflight;
  std::vector<uint8_t> kept, kept_scale;
  {
    EnsureSlots(kMaxCorePerChip + 1);
    TraceInto("moe_chip_bcast");
    ClockPtr clk = MakeClock(0, kPeriod);
    auto owned = MakeChips(clk, {0}, {0});
    std::vector<Chip*> all = Raw(owned);
    C2cBridge feed(clk, "feed", StubCfg());
    C2cBridge sink(clk, "sink", StubCfg());

    BundleStat st = LoadBundle(all, BundleRoot(), "moe_chip");
    ASSERT_EQ(st.chips, 1u);
    ASSERT_EQ(st.cores, kMaxCorePerChip);
    SetUpChipData(*all[0], 0, 0);

    // 进 B core 那一路与它广播出去那一路各走一个 path；落点按第几笔算，这是第
    // 一笔。
    SpreadHarness harness(clk, all, {}, feed, sink, 0, kChipW, 0, kChipE,
                          /*at=*/2, kMaxCycles,
                          MakeToken(kBcastInPath, BcoreLand(0)));
    clk->Continue();
    RT::JoinAll();
    CheckStreamDrained(all);
    got = harness.out_msgs;
    inflight = harness.inflight;
    std::cerr << "  停钟在第 " << harness.stopped_at << " 拍\n";
    Core& bcore = all[0]->GetCore(0);
    kept = bcore.Mmem().Peek(BcoreLand(0), kn::kBcTokenBytes);
    kept_scale = bcore.Mmem().PeekScale(BcoreLand(0), kn::kBcTokenBytes / 32);
    CheckChip(*all[0], want, "c0.");
  }
  TraceDone();
  RT::Reset();
  ExpectSame(kept, kn::TokenData(), "B core 自己留的那一份 token");
  ExpectSame(kept_scale, kn::TokenScale(), "B core 自己留的那一份 scale");
  CheckInflight(inflight);
  CheckOut(got, want.Bytes("out"), kn::RcLand(kUserId, 0));
}

// 一行两颗 chip：左边是中间列那种，隔着坏 core2、core7；右边是最后一列那种。
// token 从左边那颗的 W 口直接进计算 core 的广播树。两颗各自做完 chip 内那一段，
// 左边那颗的 dot core 是行首，结果经 core4 从 E 口过到右边那颗，沿第 1 行走到
// dot core core8 加上本 chip 的结果，落进 R core core9 的 Matrix Mem。R core 是
// 链首，另一半一直是 0，加完经 core4 从 E 口出去。
TEST(BachMoeChip, OneRowOfTwoChipsLandsInTheReductionCore) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  constexpr uint64_t kMaxCycles = 30000;
  Vectors want = ReadVectors("moe_two_groups.txt");
  ASSERT_FALSE(want.Empty())
      << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";
  // 这两颗在 EP 组里是第 1 颗与第 3 颗。
  constexpr uint64_t kIndex[2] = {1, 3};

  std::vector<MessagePtr> got;
  std::vector<uint64_t> inflight;
  std::vector<uint8_t> row;
  {
    EnsureSlots(2 * (kMaxCorePerChip + 1));
    TraceInto("moe_chip_two_groups");
    ClockPtr clk = MakeClock(0, kPeriod);
    auto owned = MakeChips(clk, {kIndex[0], kIndex[1]}, {0, 0});
    std::vector<Chip*> all = Raw(owned);
    C2cBridge feed(clk, "feed", StubCfg());
    C2cBridge sink(clk, "sink", StubCfg());

    BundleStat st = LoadBundle(all, BundleRoot(), "moe_two_groups");
    ASSERT_EQ(st.chips, 2u);
    for (uint64_t i = 0; i < 2; ++i) SetUpChipData(*all[i], 0, kIndex[i]);
    MarkRcoreHead(*all[1]);

    SpreadHarness harness(clk, all, {{0, kChipE, 1, kChipW}}, feed, sink, 0,
                          kChipW, 1, kChipE, /*at=*/2, kMaxCycles,
                          MakeToken(kInPath, kn::kTokenOff));
    clk->Continue();
    RT::JoinAll();
    got = harness.out_msgs;
    inflight = harness.inflight;
    std::cerr << "  停钟在第 " << harness.stopped_at << " 拍\n";
    for (uint64_t i = 0; i < 2; ++i) {
      CheckChip(*all[i], want, "c" + std::to_string(kIndex[i]) + ".");
    }
    row = RcoreHalf(*all[1], 0);
  }
  TraceDone();
  RT::Reset();
  ExpectSame(row, want.Bytes("row0"), "行链落进 R core 的那一份");
  CheckInflight(inflight);
  CheckOut(got, want.Bytes("out"), kn::RcLand(kUserId, 1));
}

// 一个 EP 组的 8 颗 chip：两层 × 4 列，token 从左上角那颗 chip 的 W 口进它的 B
// core，一发覆盖 64 个计算 core。每颗 chip 各做 chip 内那一段；两行各沿行链归约
// 进本行 R core，第 0 行 R core 是链首，结果从 S 口下到第 1 行 R core 的后一半，
// 第 1 行 R core 加完经 core4 从 E 口出去。
TEST(BachMoeChip, OneEpGroupHasTwoReductionCores) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  constexpr uint64_t kMaxCycles = 60000;
  constexpr uint64_t kRows = 2;
  constexpr uint64_t kChips = kRows * kGridX;
  Vectors want = ReadVectors("moe_group.txt");
  ASSERT_FALSE(want.Empty())
      << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";

  std::vector<MessagePtr> got;
  std::vector<uint64_t> inflight;
  std::vector<std::vector<uint8_t>> rows, prev;
  {
    EnsureSlots(kChips * (kMaxCorePerChip + 1));
    TraceInto("moe_chip_group");
    ClockPtr clk = MakeClock(0, kPeriod);
    std::vector<uint64_t> gx, gy;
    for (uint64_t i = 0; i < kChips; ++i) {
      gx.push_back(i % kGridX);
      gy.push_back(i / kGridX);
    }
    auto owned = MakeChips(clk, gx, gy);
    std::vector<Chip*> all = Raw(owned);
    C2cBridge feed(clk, "feed", StubCfg());
    C2cBridge sink(clk, "sink", StubCfg());

    BundleStat st = LoadBundle(all, BundleRoot(), "moe_group");
    ASSERT_EQ(st.chips, kChips);
    ASSERT_EQ(st.cores, kChips * kMaxCorePerChip);
    for (uint64_t i = 0; i < kChips; ++i) SetUpChipData(*all[i], 0, i);
    MarkRcoreHead(*all[kGridX - 1]);

    SpreadHarness harness(clk, all, GridLinks(kRows, kGridX), feed, sink, 0,
                          kChipW, kChips - 1, kChipE, /*at=*/2, kMaxCycles,
                          MakeToken(kBcastInPath, BcoreLand(0)));
    clk->Continue();
    RT::JoinAll();
    got = harness.out_msgs;
    inflight = harness.inflight;
    std::cerr << "  停钟在第 " << harness.stopped_at << " 拍\n";
    for (uint64_t i = 0; i < kChips; ++i) {
      CheckChip(*all[i], want, "c" + std::to_string(i) + ".");
    }
    for (uint64_t r = 0; r < kRows; ++r) {
      Chip& last = *all[(r + 1) * kGridX - 1];
      rows.push_back(RcoreHalf(last, 0));
      prev.push_back(RcoreHalf(last, 1));
    }
  }
  TraceDone();
  RT::Reset();
  for (uint64_t r = 0; r < kRows; ++r) {
    ExpectSame(rows[r], want.Bytes("row" + std::to_string(r)),
               "第 " + std::to_string(r) + " 行落进 R core 的行链结果");
    if (r > 0) {
      ExpectSame(prev[r], want.Bytes("rcore" + std::to_string(r - 1)),
                 "第 " + std::to_string(r) + " 行 R core 收到的上一行累加结果");
    }
  }
  CheckInflight(inflight);
  CheckOut(got, want.Bytes("out"), kn::RcLand(kUserId, 1));
}

// 装模型那一段接在业务前面：权重不经 SCP，走 Host 那条 msg 流从数据面进来。
// 每个计算 core 的 W1 与 W2 在组内第 0 个专家那一片的开头各留两个 tile 不预
// 置，数据与 scale 都清成 0，改由两个包搬进 Matrix Mem。一条 path 走遍格子里的
// 8 个 core，落在哪一个由包头的 path_core_mask 挑，落到哪个地址由包头的 dst_addr
// 定，scale 随包头的标记落进 scale 旁带。各 core 都收够之后切业务模式，再发
// token，结果与权重全部预置时逐字节相同。
TEST(BachMoeChip, WeightsComeInBeforeTheFirstToken) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  constexpr uint64_t kMaxCycles = 60000;
  // 一个包 64 flit，留够它走完那棵树再落进 Matrix Mem。
  constexpr uint64_t kGap = 400;
  // 留空的那两段：W1 与 W2 在组内第 0 个专家那一片的开头。
  constexpr uint64_t kHole[2] = {kn::kMmW1, kn::kMmW2};
  Vectors want = ReadVectors("moe_chip.txt");
  ASSERT_FALSE(want.Empty())
      << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";

  std::vector<MessagePtr> got;
  std::vector<uint64_t> inflight;
  std::vector<std::vector<uint8_t>> sent, landed;
  uint64_t switched_at = 0;
  {
    EnsureSlots(kMaxCorePerChip + 1);
    TraceInto("moe_chip_weights");
    ClockPtr clk = MakeClock(0, kPeriod);
    auto owned = MakeChips(clk, {0}, {0});
    std::vector<Chip*> all = Raw(owned);
    Chip& chip = *all[0];
    C2cBridge feed(clk, "feed", StubCfg());
    C2cBridge sink(clk, "sink", StubCfg());

    LoadBundle(all, BundleRoot(), "moe_chip");
    SetUpChipData(chip, 0, 0);
    std::vector<MessagePtr> load;
    for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
      Core& core = chip.GetCore(CoreOfSlot(0, slot));
      for (uint64_t at : kHole) {
        std::vector<uint8_t> data = core.Mmem().Peek(at, kWeightsChunk);
        std::vector<uint8_t> scale = core.Mmem().PeekScale(at, kWeightsScale);
        sent.push_back(data);
        sent.push_back(scale);
        core.Mmem().Poke(at, std::vector<uint8_t>(kWeightsChunk, 0));
        core.Mmem().PokeScale(at, std::vector<uint8_t>(kWeightsScale, 0));
        load.push_back(MakeWeightsMsg(slot, at, data, scale));
      }
      EnterWeightsMode(core);
    }
    WireWeightsPath(chip);

    SpreadHarness harness(clk, all, {}, feed, sink, 0, kChipW, 0, kChipE,
                          /*at=*/2, kMaxCycles,
                          MakeToken(kBcastInPath, BcoreLand(0)));
    harness.weights = load;
    harness.weights_gap = kGap;
    // SCP 查各 core 的计数：都收够两笔，loader 才中断它。
    harness.weights_done = [&chip] {
      for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
        std::vector<uint8_t> n =
            chip.GetCore(CoreOfSlot(0, slot)).Smem().Peek(kWeightsCntOff, 4);
        if (n[0] < 2) return false;
      }
      return true;
    };
    harness.to_business = [&chip] {
      for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
        EnterBusinessMode(chip.GetCore(CoreOfSlot(0, slot)));
      }
    };
    clk->Continue();
    RT::JoinAll();
    got = harness.out_msgs;
    inflight = harness.inflight;
    switched_at = harness.switched_at;
    std::cerr << "  切业务模式在第 " << switched_at << " 拍，停钟在第 "
              << harness.stopped_at << " 拍\n";
    for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
      Core& core = chip.GetCore(CoreOfSlot(0, slot));
      for (uint64_t at : kHole) {
        landed.push_back(core.Mmem().Peek(at, kWeightsChunk));
        landed.push_back(core.Mmem().PeekScale(at, kWeightsScale));
      }
    }
    CheckChip(chip, want, "c0.");
  }
  TraceDone();
  RT::Reset();
  EXPECT_GT(switched_at, 0u) << "权重没搬完，模式没切过去";
  ASSERT_EQ(landed.size(), sent.size());
  for (uint64_t i = 0; i < sent.size(); ++i) {
    ExpectSame(landed[i], sent[i],
               "第 " + std::to_string(i / 4) + " 个 core 搬进来的第 " +
                   std::to_string(i % 4) + " 段");
  }
  CheckInflight(inflight);
  CheckOut(got, want.Bytes("out"), kn::RcLand(kUserId, 0));
}
