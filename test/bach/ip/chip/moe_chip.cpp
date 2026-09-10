// 一层 MoE 那一段摊在一颗 chip 的 8 个 core 与一个 EP 组的 64 个 core 上：FC1
// 与 FC3 按中间维切开，FC2 因此按 K 切开，每个 core 出一份完整长度的部分和，
// 沿一条链逐跳在 Router 的 ReduceModule 上相加。
//
// 与 core/moe.cpp 那一份的区别：那一条只在一个 core 内，专家间的加权求和落在
// MU 里；这里多出跨 core 与跨 chip 切 K 的那一层归约，加法落在 Router 上。
//
// 每个 core 的任务链、kernel 与寄存器配置完全相同 —— EPTP-NN 的特点就在这里，
// 差别只在 Matrix Mem 里那一片权重和 Router 表里那两条路由。

#include <iostream>

#include "test/bach/ip/chip/moe_common.h"

using namespace latch;
using namespace latch::bach;
using namespace latch::bach::moetest;

// 一颗 chip 上的完整一段：token 从 W 口进来，坐在那个口上的 core5 不派角色、
// 只往上转一跳，B core（core0）留一份在自己的 Matrix Mem，再往右与往下各发一
// 路，广播给两行的 8 个计算 core。各 core 算自己那一片 FC1、FC3、门控与 FC2，
// 8 份部分和沿链在 Router 上逐跳相加，链尾从 E 口出来，与参考实现逐 bit 相同。
TEST(BachMoeChip, BcoreStartsTheBroadcast) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  constexpr uint64_t kMaxCycles = 60000;
  SpreadCase want = ReadCase("moe_chip.txt");
  ASSERT_GT(want.k, 0u) << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";
  ASSERT_EQ(want.chips, 1u);
  ASSERT_EQ(want.token.size(), kBcTokenBytes) << "B core 一格装一个 token";

  std::vector<MessagePtr> got;
  std::vector<uint64_t> inflight;
  std::vector<std::vector<uint8_t>> landed;
  std::vector<uint8_t> kept;
  {
    EnsureSlots(kMaxCorePerChip + 1);
    TraceInto("moe_chip_bcast");
    ClockPtr clk = MakeClock(0, kPeriod);
    ChipCfg cfg;
    cfg.shape = ChipShape::kFirst;
    cfg.gx = 0;
    cfg.gy = 0;
    // B core 的 token 槽位标志表：硬件把一笔搬进 Matrix Mem 之后置那一格。
    cfg.inbound_flag_base = kBcFlagOff;
    cfg.inbound_entry_bytes = kBcTokenBytes;
    cfg.core_tick = kCoreTick;
    cfg.chip_tick = kChipTick;
    Chip chip(clk, "chip", cfg);
    C2cBridge feed(clk, "feed", StubCfg());
    C2cBridge sink(clk, "sink", StubCfg());

    std::vector<Chip*> all = {&chip};
    // 配置表与 kernel 都从 bundle 装：那一份由 compiler 按同一套拓扑描述编出来。
    BundleStat st = LoadBundle(all, BundleRoot(), "moe_chip");
    ASSERT_EQ(st.chips, 1u);
    ASSERT_EQ(st.cores, kMaxCorePerChip);
    for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
      SetUpCoreData(chip.GetCore(CoreOfSlot(cfg.shape, slot)), want, slot);
    }

    MessagePtr token = MakeToken(want);
    // 进来那一路与广播出去那一路各走一个 path；落点按第几笔算，这是第一笔。
    token->path_id = kBcastInPath;
    token->dst_addr = BcoreLand(0);
    SpreadHarness harness(clk, all, {}, feed, sink, 0, kChipW, 0, kChipE, {0},
                          /*at=*/2, kMaxCycles, token);
    clk->Continue();
    RT::JoinAll();
    got = harness.out_msgs;
    inflight = harness.inflight;
    std::cerr << "  停钟在第 " << harness.stopped_at << " 拍\n";
    kept = chip.GetCore(0).Mmem().Peek(BcoreLand(0), kBcTokenBytes);
    for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
      landed.push_back(chip.GetCore(CoreOfSlot(cfg.shape, slot))
                           .Cmem()
                           .Peek(kResultAt, want.out_n * 4));
    }
  }
  TraceDone();
  RT::Reset();
  EXPECT_EQ(kept, want.token) << "B core 自己留的那一份";
  CheckResult(want, landed, inflight, got);
}

namespace {

// ── 一个 EP 组的 8 颗 chip ──
//
// 摆成 2 层 × 4 列，编号 gy * 4 + gx。层内左右相接（E 对 W）、层间上下相接
// （S 对 N），与 LPU 的 WireRow / WireCol 同一套接法。
constexpr uint64_t kGroupCols = 4;
constexpr uint64_t kGroupChips = 8;

std::vector<ChipPair> GroupLinks() {
  std::vector<ChipPair> v;
  for (uint64_t gy = 0; gy < kGroupChips / kGroupCols; ++gy) {
    for (uint64_t gx = 0; gx < kGroupCols; ++gx) {
      uint64_t a = gy * kGroupCols + gx;
      if (gx + 1 < kGroupCols) v.push_back({a, kChipE, a + 1, kChipW});
      if (gy + 1 < kGroupChips / kGroupCols) {
        v.push_back({a, kChipS, a + kGroupCols, kChipN});
      }
    }
  }
  return v;
}


BcastPlan GroupBcast(uint64_t chip) {
  uint64_t gx = chip % kGroupCols;
  uint64_t gy = chip / kGroupCols;
  // 第 1 层的头一颗从上面下来，其余都从左边过来。
  BcastPlan p{gx == 0 && gy == 1 ? kChipN : kChipW, {}};
  if (gx + 1 < kGroupCols) p.out_port.push_back(kChipE);
  if (gx == 0 && gy == 0) p.out_port.push_back(kChipS);
  return p;
}

}  // namespace

// 一个 EP 组的 64 个 core：token 从左上角那颗 chip 的 W 口进，由它的 B core 留
// 一份再发起广播，一发覆盖 8 颗 chip 的每一个计算 core；64 份部分和沿一条蛇形
// 链逐跳归约 —— chip 内 8 跳走 core 之间的链路，chip 与 chip 之间那 7 跳走
// C2C。链尾从最后一颗 chip 的 E 口出来，与参考实现逐 bit 相同。
TEST(BachMoeChip, OneEpGroupReducesSixtyFourCores) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  // 广播与归约各要跨 7 段 C2C，一段 300 拍，加上 core 上算那一段。
  constexpr uint64_t kMaxCycles = 40000;
  SpreadCase want = ReadCase("moe_group.txt");
  ASSERT_GT(want.k, 0u) << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";
  ASSERT_EQ(want.chips, kGroupChips);

  std::vector<MessagePtr> got;
  std::vector<uint64_t> inflight;
  std::vector<std::vector<uint8_t>> landed;
  uint64_t wide = 0;
  {
    EnsureSlots(kGroupChips * (kMaxCorePerChip + 1));
    TraceInto("moe_chip_group");
    ClockPtr clk = MakeClock(0, kPeriod);
    std::vector<std::unique_ptr<Chip>> owned;
    std::vector<Chip*> all;
    for (uint64_t i = 0; i < kGroupChips; ++i) {
      ChipCfg cfg;
      cfg.shape = ShapeOfGx(i % kGroupCols);
      cfg.gx = i % kGroupCols;
      cfg.gy = i / kGroupCols;
      // 左上角那颗的 B core 是本组广播的发起点，它要按落点置 token 槽位的标志。
      cfg.inbound_flag_base = kBcFlagOff;
      cfg.inbound_entry_bytes = kBcTokenBytes;
      cfg.core_tick = kCoreTick;
      cfg.chip_tick = kChipTick;
      owned.push_back(std::make_unique<Chip>(
          clk, "chip" + std::to_string(i), cfg));
      all.push_back(owned.back().get());
    }
    C2cBridge feed(clk, "feed", StubCfg());
    C2cBridge sink(clk, "sink", StubCfg());

    for (uint64_t i = 0; i < kGroupChips; ++i) {
      ChipShape shape = all[i]->Shape();
      for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
        SetUpCore(all[i]->GetCore(CoreOfSlot(shape, slot)), want,
                  i * kCorePerChip + slot);
      }
      BcastPlan p = GroupBcast(i);
      if (i == 0) {
        // 左上角那颗：token 进它的 B core，广播从那里发起。
        LoadKernels(all[i]->GetCore(0));
        WriteBcoreChains(all[i]->GetCore(0), kInPath, kFlowRight | kFlowMid);
        WireBcoreBroadcast(*all[i], p.enter_port, p.out_port);
      } else {
        WireBroadcast(*all[i], shape, p.enter_port, p.out_port);
      }
    }
    // 链尾坐在最后一颗 chip 的 E 口上。
    WireReduceChain(all, want.order, kFlowRight);
    uint64_t last = want.order.back() / kCorePerChip;

    // 推的次序按归约链倒着来：末级先做。
    std::vector<uint64_t> run;
    for (uint64_t i = want.order.size(); i > 0; --i) {
      uint64_t chip = want.order[i - 1] / kCorePerChip;
      if (run.empty() || run.back() != chip) run.push_back(chip);
    }

    MessagePtr token = MakeToken(want);
    // 进 B core 那一路与它广播出去那一路各走一个 path；落点按第几笔算。
    token->path_id = kBcastInPath;
    token->dst_addr = BcoreLand(0);
    SpreadHarness harness(clk, all, GroupLinks(), feed, sink, 0, kChipW, last,
                          kChipE, run, /*at=*/2, kMaxCycles, token);
    clk->Continue();
    RT::JoinAll();
    got = harness.out_msgs;
    inflight = harness.inflight;
    std::cerr << "  停钟在第 " << harness.stopped_at << " 拍\n";
    for (Chip* c : all) {
      if (c->CoreNum() == kMaxCorePerChip) ++wide;
    }
    for (uint64_t i = 0; i < kGroupChips; ++i) {
      ChipShape shape = all[i]->Shape();
      for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
        landed.push_back(all[i]
                             ->GetCore(CoreOfSlot(shape, slot))
                             .Cmem()
                             .Peek(kResultAt, want.out_n * 4));
      }
    }
  }
  TraceDone();
  RT::Reset();
  // 组里 gx 为 0 与 3 的那四颗是 2×5：多出来的那一列放 B core、R core 与不派
  // 角色的那个，它们在这两条 path 上都只做转发。
  EXPECT_EQ(wide, 4u);
  CheckResult(want, landed, inflight, got);
}

namespace {

// ── 两个 EP 组在 R core 上会合 ──
//
// 两颗 chip 各是一个 EP 组：前一颗是中间列形状，后一颗是最后一列形状，R core
// 坐在它的 core9 上。这个用户的第一组是组 0，它的组结果直接送到组 1 的 R core
// 的后一半；组 1 的组结果落前一半。R core 上 VU 把两半相加再送出阵列。
//
// 两条归约链各用一个 path_id：组 0 那一条要穿过组 1 的四个计算 core 才到得了
// R core，与组 1 自己那一条在同一批 core 上，共用一个号就冲突了。
constexpr uint64_t kGroup0Path = 5;
constexpr uint64_t kGroup1Path = 6;
constexpr uint64_t kRcoreOutPath = 0;






}  // namespace

// 两个 EP 组各算各的，组结果在 R core 上相加：组 0 的那一份穿过组 1 的四个
// core 直达 R core 的后一半，组 1 的落前一半，加出来的 32 个 FP32 与参考实现
// 逐 bit 相同。
TEST(BachMoeChip, TwoEpGroupsMeetAtTheReductionCore) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  constexpr uint64_t kMaxCycles = 40000;
  SpreadCase want = ReadCase("moe_two_groups.txt");
  ASSERT_GT(want.k, 0u) << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";
  ASSERT_EQ(want.chips, 2u);

  std::vector<MessagePtr> got;
  std::vector<uint64_t> inflight;
  std::vector<std::vector<uint8_t>> landed;
  std::vector<uint8_t> summed;
  {
    EnsureSlots(2 * (kMaxCorePerChip + 1));
    TraceInto("moe_chip_two_groups");
    ClockPtr clk = MakeClock(0, kPeriod);
    ChipCfg c0;
    c0.shape = ChipShape::kMiddle;
    c0.gx = 1;
    c0.core_tick = kCoreTick;
    c0.chip_tick = kChipTick;
    ChipCfg c1;
    c1.shape = ChipShape::kLast;
    c1.gx = 3;
    c1.inbound_flag_base = kRcFlagOff;
    c1.inbound_entry_bytes = kRcHalfBytes;
    c1.core_tick = kCoreTick;
    c1.chip_tick = kChipTick;
    Chip a(clk, "g0", c0);
    Chip b(clk, "g1", c1);
    C2cBridge feed(clk, "feed", StubCfg());
    C2cBridge sink(clk, "sink", StubCfg());
    std::vector<Chip*> all = {&a, &b};

    // 两组各 8 个计算 core。组 0 是这个用户的第一组，结果送后一半。
    for (uint64_t g = 0; g < 2; ++g) {
      ChipShape shape = all[g]->Shape();
      uint64_t path = g == 0 ? kGroup0Path : kGroup1Path;
      for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
        SetUpCore(all[g]->GetCore(CoreOfSlot(shape, slot)), want,
                  g * kCorePerChip + slot, path, /*send_half=*/g == 0 ? 1 : 0);
      }
      WireBroadcast(*all[g], shape, kChipW, g == 0 ? std::vector<uint64_t>{kChipE}
                                                   : std::vector<uint64_t>{});
    }

    // 组 0 的链走完本 chip 从 E 口出去；组 1 的链在坐着 core9 左边的那个 core
    // 收尾，再往右一跳就是 R core。
    std::vector<uint64_t> order0(want.order.begin(),
                                 want.order.begin() + kCorePerChip);
    std::vector<uint64_t> order1;
    for (uint64_t i = kCorePerChip; i < want.order.size(); ++i) {
      order1.push_back(want.order[i] - kCorePerChip);
    }
    WireReduceChain({&a}, order0, kFlowRight, kGroup0Path);
    WireReduceChain({&b}, order1, kFlowRight, kGroup1Path);

    // 组 0 那一份进 chip 1 之后一路往右转发到 R core。
    for (uint64_t k = 0; k < kCols; ++k) {
      uint64_t core = CoreOfSlot(ChipShape::kLast, kCols + k);
      b.GetCore(core).GetRouter().Preload(kGroup0Path,
                                          PassThrough(kFlowRight));
    }
    Core& rc = b.GetCore(kMaxCorePerChip - 1);
    rc.GetRouter().Preload(kGroup0Path, LandInRcore());
    rc.GetRouter().Preload(kGroup1Path, LandInRcore());
    rc.GetRouter().Preload(kRcoreOutPath, PassThrough(kFlowRight));
    rc.GetDte().Tables().PreloadRtab(kRcoreOutPath, PassThrough(kFlowRight));
    rc.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
    rc.Rv(1).LoadImage(KernelDir() + "kernel_mu.hex");
    rc.Rv(2).LoadImage(KernelDir() + "kernel_vu.hex");
    WriteRcoreChains(rc, kRcoreOutPath);

    SpreadHarness harness(clk, all, {{0, kChipE, 1, kChipW}}, feed, sink, 0,
                          kChipW, 1, kChipS, {1, 0}, /*at=*/2, kMaxCycles,
                          MakeToken(want));
    clk->Continue();
    RT::JoinAll();
    got = harness.out_msgs;
    inflight = harness.inflight;
    std::cerr << "  停钟在第 " << harness.stopped_at << " 拍\n";
    for (uint64_t g = 0; g < 2; ++g) {
      ChipShape shape = all[g]->Shape();
      for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
        landed.push_back(all[g]
                             ->GetCore(CoreOfSlot(shape, slot))
                             .Cmem()
                             .Peek(kResultAt, want.out_n * 4));
      }
    }
    summed = rc.Cmem().Peek(kRcSumOff + kReduceSwHeaderBytes, want.out_n * 4);
  }
  TraceDone();
  RT::Reset();

  // R core 的 Core Mem 里那一份：出核那一步之前加完的就是它。
  ASSERT_EQ(summed.size(), want.out_bits.size() * 4);
  for (uint64_t j = 0; j < want.out_bits.size(); ++j) {
    uint32_t v = 0;
    for (int t = 0; t < 4; ++t) v |= uint32_t(summed[j * 4 + t]) << (8 * t);
    EXPECT_EQ(v, want.out_bits[j]) << "R core 上加出来的第 " << j << " 个";
  }
  CheckResult(want, landed, inflight, got);
}


// 装模型那一段接在业务前面：权重不经 SCP，走 Host 那条 msg 流从数据面进来。
// 每个计算 core 的 W1 与 W2 各留一段不预置，改由两个包搬进 Matrix Mem —— 一条
// path 走遍格子里的 8 个 core，落在哪一个由包头的 path_core_mask 挑，落到哪个
// 地址由包头的 dst_addr 定。各 core 都收够之后切业务模式，再发 token，结果与
// 权重全部预置时逐 bit 相同。
TEST(BachMoeChip, WeightsComeInBeforeTheFirstToken) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  constexpr uint64_t kMaxCycles = 60000;
  // 一个包 64 flit，留够它走完广播树再落进 Matrix Mem。
  constexpr uint64_t kGap = 400;
  SpreadCase want = ReadCase("moe_chip.txt");
  ASSERT_GT(want.k, 0u) << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";
  ASSERT_EQ(want.chips, 1u);

  std::vector<MessagePtr> got;
  std::vector<uint64_t> inflight;
  std::vector<std::vector<uint8_t>> landed;
  std::vector<std::vector<uint8_t>> w1_head, w2_head;
  uint64_t switched_at = 0;
  {
    EnsureSlots(kMaxCorePerChip + 1);
    TraceInto("moe_chip_weights");
    ClockPtr clk = MakeClock(0, kPeriod);
    ChipCfg cfg;
    cfg.shape = ChipShape::kFirst;
    cfg.gx = 0;
    cfg.gy = 0;
    cfg.inbound_flag_base = kBcFlagOff;
    cfg.inbound_entry_bytes = kBcTokenBytes;
    cfg.core_tick = kCoreTick;
    cfg.chip_tick = kChipTick;
    Chip chip(clk, "chip", cfg);
    C2cBridge feed(clk, "feed", StubCfg());
    C2cBridge sink(clk, "sink", StubCfg());

    std::vector<Chip*> all = {&chip};
    // 一个 core 要收的两段权重。全局第 1 个专家在组内排第 0 位，落在 W1 与 W2
    // 两段的开头，正是这里留空的那两段。
    std::vector<MessagePtr> load;
    for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
      Core& core = chip.GetCore(CoreOfSlot(cfg.shape, slot));
      SetUpCore(core, want, slot);
      uint64_t off = slot * want.seed_stride + 1;
      std::vector<uint8_t> w1 = TameBf16(want.k * want.inter, want.w1_seed + off);
      std::vector<uint8_t> w2 =
          TameBf16(want.inter * want.out_n, want.w2_seed + off);
      w1.resize(kWeightsChunk);
      w2.resize(kWeightsChunk);
      // 这两段留给数据面搬进来。
      core.Mmem().Poke(kW1At, std::vector<uint8_t>(kWeightsChunk, 0));
      core.Mmem().Poke(kW2At, std::vector<uint8_t>(kWeightsChunk, 0));
      load.push_back(MakeWeightsMsg(slot, kW1At, w1));
      load.push_back(MakeWeightsMsg(slot, kW2At, w2));
      EnterWeightsMode(core);
    }
    LoadKernels(chip.GetCore(0));
    WriteBcoreChains(chip.GetCore(0), kInPath, kFlowRight | kFlowMid);
    WireBcoreBroadcast(chip, kChipW);
    WireWeightsPath(chip, cfg.shape, kChipW);
    WireReduceChain(all, want.order, kFlowRight);

    MessagePtr token = MakeToken(want);
    token->path_id = kBcastInPath;
    token->dst_addr = BcoreLand(0);
    SpreadHarness harness(clk, all, {}, feed, sink, 0, kChipW, 0, kChipE, {0},
                          /*at=*/2, kMaxCycles, token);
    harness.weights = load;
    harness.weights_gap = kGap;
    // SCP 查各 core 的计数：都收够两笔，loader 才中断它。
    harness.weights_done = [&chip, &cfg] {
      for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
        std::vector<uint8_t> n = chip.GetCore(CoreOfSlot(cfg.shape, slot))
                                     .Smem()
                                     .Peek(kWeightsCntOff, 4);
        if (n[0] < 2) return false;
      }
      return true;
    };
    harness.to_business = [&chip, &cfg] {
      for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
        EnterBusinessMode(chip.GetCore(CoreOfSlot(cfg.shape, slot)));
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
      Core& core = chip.GetCore(CoreOfSlot(cfg.shape, slot));
      w1_head.push_back(core.Mmem().Peek(kW1At, kWeightsChunk));
      w2_head.push_back(core.Mmem().Peek(kW2At, kWeightsChunk));
      landed.push_back(core.Cmem().Peek(kResultAt, want.out_n * 4));
    }
  }
  TraceDone();
  RT::Reset();
  EXPECT_GT(switched_at, 0u) << "权重没搬完，模式没切过去";
  for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
    uint64_t off = slot * want.seed_stride + 1;
    std::vector<uint8_t> w1 = TameBf16(want.k * want.inter, want.w1_seed + off);
    std::vector<uint8_t> w2 =
        TameBf16(want.inter * want.out_n, want.w2_seed + off);
    w1.resize(kWeightsChunk);
    w2.resize(kWeightsChunk);
    EXPECT_EQ(w1_head[slot], w1) << "第 " << slot << " 个 core 的 W1 那一段";
    EXPECT_EQ(w2_head[slot], w2) << "第 " << slot << " 个 core 的 W2 那一段";
  }
  CheckResult(want, landed, inflight, got);
}
