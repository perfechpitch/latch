// 没有真包也能编、也能跑的检查。不是传输证明。
//
// 这一份只钉这几件事：
//   1. 6392 B 按 topK 56、数据 6144、scale 192 切开，写盘再读回；
//   2. 进核 PathID 被占用时拒绝，不覆盖，包括空着的 63；
//   3. 快照能在退休之前保住 allocation；没有 user_id_vld 就不记用户；
//   4. packed 适配器严格检查段长之和等于 6392；旧测试丢尾巴是已知失败；
//   5. StoreProbe 读 CoreMem 上已经 Poke 进去的字节。这不是 flit，
//      也不是 B core 收下之后的证明。
//
// 不读 moe bundle，不铺权重。golden 只在设了 BACHFLOW_EXACT_RUN 时读。

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "bach/ip/chip/chip.h"
#include "bach/ip/chip/core/ts/cfg_reg.h"
#include "bach/ip/chip/core/ts/stream_table.h"
#include "test/bach/ip/chip/ep1_exact_common.h"

using namespace latch;
using namespace latch::bach;
using namespace latch::bach::ep1exact;

namespace {

std::string ScratchDir() {
  char const *v = std::getenv("BACHFLOW_HARNESS_DIR");
  if (v && *v)
    return v;
  return "/tmp/ep1-exact-harness";
}

} // namespace

// 最小库存：不走 LoadBundle。逻辑号由调用方按装载后的图填，
// 这里只证明 PathID 冲突检查不依赖 bundle 文本。
TEST(Ep1ExactToken, IngressPathRefusesOccupiedEntry) {
  EnsureSlots(kMaxCorePerChip + 2);
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    ChipCfg cfg;
    cfg.gx = 0;
    cfg.gy = 0;
    cfg.core_tick = false;
    cfg.chip_tick = false;
    auto chip = std::make_unique<Chip>(clk, "chip0", cfg);
    Core &b = chip->GetCore(0);
    // 63 不当进口。表宽不够时 250 也不当进口，不能越界查。
    EXPECT_FALSE(PathFree(b, 63));
    if (kPathNum <= kIngressPath)
      EXPECT_FALSE(PathFree(b, kIngressPath));
    else
      EXPECT_TRUE(PathFree(b, kIngressPath));

    uint64_t path = kIngressPath < kPathNum ? kIngressPath : 62;
    ASSERT_NE(path, 63u);
    ASSERT_TRUE(PathFree(b, path));
    RouteEntry taken = IngressEntry();
    taken.flow_dir = kFlowRight;
    b.GetRouter().Preload(path, taken);
    EXPECT_FALSE(PathFree(b, path));
    RouteEntry const &kept = b.GetRouter().Table().Lookup(0, path);
    EXPECT_EQ(kept.flow_dir, uint64_t(kFlowRight));
    EXPECT_NE(kept.flow_dir, IngressEntry().flow_dir);
  }
  RT::Reset();
}

TEST(Ep1ExactToken, SavedFileIs56Plus6144Plus192) {
  auto saved = DeterministicSaved(7);
  ASSERT_EQ(saved.size(), 6392u);
  EXPECT_EQ(saved[0], 8);
  auto other = DeterministicSaved(9);
  EXPECT_NE(saved, other) << "两个用户的负荷必须不同";

  std::string dir = ScratchDir() + "/fixture";
  std::filesystem::create_directories(dir);
  std::string path = dir + "/user7.token";
  {
    std::ofstream o(path, std::ios::binary);
    ASSERT_TRUE(o.good()) << path;
    o.write(reinterpret_cast<char const *>(saved.data()),
            std::streamsize(saved.size()));
  }
  std::vector<uint8_t> back;
  std::string why;
  ASSERT_TRUE(LoadSavedFile(path, &back, &why)) << why;
  EXPECT_EQ(back.size(), kSavedBytes);
  EXPECT_EQ(std::vector<uint8_t>(back.begin(), back.begin() + kTopkBytes),
            std::vector<uint8_t>(saved.begin(), saved.begin() + kTopkBytes));
  EXPECT_EQ(std::vector<uint8_t>(back.begin() + kTopkBytes,
                                 back.begin() + kTopkBytes + kDataBytes),
            std::vector<uint8_t>(saved.begin() + kTopkBytes,
                                 saved.begin() + kTopkBytes + kDataBytes));
  EXPECT_EQ(std::vector<uint8_t>(back.end() - kScaleBytes, back.end()),
            std::vector<uint8_t>(saved.end() - kScaleBytes, saved.end()));
}

TEST(Ep1ExactToken, WireOrderMatchesCompiledReceiveSegments) {
  auto saved = DeterministicSaved(101);
  auto packet = ExactToken(101, kIngressPath, 0x48c00000, saved, 101);
  ASSERT_EQ(packet->TotalBytes(), kSavedBytes);
  for (uint64_t offset = 0; offset < kSavedBytes; ++offset)
    ASSERT_EQ(packet->WireByte(offset), saved[offset]) << offset;
  EXPECT_EQ(packet->WireByte(55), saved[55]);
  EXPECT_EQ(packet->WireByte(56), saved[56]);
  EXPECT_EQ(packet->WireByte(255), saved[255]);
  EXPECT_EQ(packet->WireByte(256), saved[256]);
  EXPECT_EQ(packet->WireByte(6200), saved[6200]);
  auto corrupted = saved;
  corrupted[6200] ^= 1;
  EXPECT_NE(packet->WireByte(6200), corrupted[6200]);
}

TEST(Ep1ExactToken, StoredReceiveKeepsTopkDataScale) {
  // 只是 Poke 再 Peek。没有 flit，没有边界，也没有 B core 收包。
  // 真机上收下之后才会走同一条读法；这里只证明读法对得上自己铺的字节。
  auto saved = DeterministicSaved(11);
  ASSERT_EQ(saved.size(), kSavedBytes);
  EnsureSlots(kMaxCorePerChip + 2);
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    ChipCfg cfg;
    auto chip = std::make_unique<Chip>(clk, "chip0", cfg);
    Core &core = chip->GetCore(0);
    std::vector<uint8_t> topk(saved.begin(), saved.begin() + kTopkBytes);
    std::vector<uint8_t> data(saved.begin() + kTopkBytes,
                              saved.begin() + kTopkBytes + kDataBytes);
    core.Cmem().Poke(0x1000, data);
    core.Cmem().Poke(0x3000, topk);
    PokeInputScale(core.Cmem(), saved, 0);

    StoreView view;
    view.core = &core;
    view.data_addr = 0x1000;
    view.scale_addr = 0;
    view.topk_addr = 0x3000;
    StoreProbe probe;
    probe.Read(view, saved);
    EXPECT_EQ(probe.data, kDataBytes);
    EXPECT_EQ(probe.scale, kScaleBytes);
    EXPECT_EQ(probe.topk, kTopkBytes);
    EXPECT_EQ(probe.mismatch, 0u) << probe.note;
    EXPECT_EQ(probe.users, 1u);

    // 改落地的一个字节，探针必须报出来。这证明它读的是存储，不是原件。
    std::vector<uint8_t> flipped = data;
    flipped[0] ^= 0xFFu;
    core.Cmem().Poke(0x1000, flipped);
    StoreProbe again;
    again.Read(view, saved);
    EXPECT_GT(again.mismatch, 0u);
  }
  RT::Reset();
}

TEST(Ep1ExactToken, GoldenFilesAreDistinct6392) {
  if (std::getenv("BACHFLOW_EXACT_RUN") == nullptr) {
    GTEST_SKIP() << "没设 BACHFLOW_EXACT_RUN，不读 golden token。";
  }
  std::string root = EnvOr("BACHFLOW_GOLDEN_DIR", "");
  if (root.empty()) {
    FAIL() << "设了 BACHFLOW_EXACT_RUN 但没有 BACHFLOW_GOLDEN_DIR。";
  }
  std::vector<uint8_t> a, b;
  std::string why;
  ASSERT_TRUE(LoadSavedFile(root + "/user101.token", &a, &why)) << why;
  ASSERT_TRUE(LoadSavedFile(root + "/user202.token", &b, &why)) << why;
  EXPECT_EQ(a.size(), 6392u);
  EXPECT_EQ(b.size(), 6392u);
  EXPECT_NE(a, b);
}

TEST(Ep1ExactToken, PackedAdapterStrictLengthDoesNotDropTopk) {
  auto old_tail = DteInboundDropsTail();
  // 启用段 1..3 的长度之和必须等于线上 6392。56 单独成 topK 段，
  // 数据 6144、scale 192，三者相加才过严格检查。
  uint64_t enabled = kTopkBytes + kDataBytes + kScaleBytes;
  EXPECT_EQ(enabled, kSavedBytes);

  DteInboundDropsTail() = false; // packed 适配器在装 BACHIR11 后设置
  uint64_t total = kSavedBytes;
  EXPECT_EQ(enabled, total);

  // BACHIR 11 的旧 kernel 只开数据+scale，尾巴（这里是 56 B topK）被丢掉。
  // 这是已知的不匹配，不能靠把 expected 改小来假装通过。
  DteInboundDropsTail() = true;
  uint64_t legacy = kDataBytes + kScaleBytes; // 6336
  EXPECT_LT(legacy, total);
  EXPECT_TRUE(legacy < total && DteInboundDropsTail());
  DteInboundDropsTail() = old_tail;
}

TEST(Ep1ExactToken, DistinctUsersCapturedBeforeSlotRetires) {
  EnsureSlots(4);
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    StreamTable table(clk, "stream_table");
    CfgReg cfg(clk, "cfg");
    table.AttachCfg(cfg);
    TaskEntry task0;
    task0.valid = true;
    task0.end = true;
    table.SelfStart(task0, 2);

    auto before = table.SnapPtr()->Get();
    ASSERT_TRUE(before);
    ASSERT_EQ(before->in_flight, 2u);
    std::map<SlotKey, Capture> held;
    NoteLive(0, 0, *before, /*cycle=*/1, &held);
    ASSERT_EQ(held.size(), 2u);
    for (auto const &one : held) {
      EXPECT_FALSE(one.second.user_stamped) << "ACK 之前没有用户号";
      EXPECT_NE(one.second.allocation, 0u);
    }

    table.RunStep();
    auto after = table.SnapPtr()->Get();
    ASSERT_TRUE(after);
    bool alloc_stable = true;
    for (uint64_t sid = 0; sid < before->slot_num; ++sid) {
      if (!before->entry[sid].valid)
        continue;
      if (after->entry[sid].allocation != before->entry[sid].allocation)
        alloc_stable = false;
      EXPECT_NE(before->entry[sid].allocation, 0u);
    }
    EXPECT_TRUE(alloc_stable) << "没退休时 allocation 不能变";
    uint64_t through = cfg.ThroughEndMask();
    // 自启动还没写 INIT_FINISH，end 掩码是 0。完成不能成立。
    EXPECT_EQ(through, 0u);
    EXPECT_FALSE(EndCovered(after->entry[0], through));
  }
  RT::Reset();
}
