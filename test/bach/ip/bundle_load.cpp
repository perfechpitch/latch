// 装载器的检查：bundle 里与坏 core、角色对不上的地方，在应用之前拦下来。
//
// 从 compiler 编出来的 moe_chip 那一份出发，每条检查一个反例：改一行或加一行，
// 写成一套新的 bundle，装载应当断言。原样的那一份装得进去，只改 core_bad_mask 且
// 其余都对得上的那一份也装得进去。

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/bundle_load.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

std::string BundleRoot() {
  return std::string(LATCH_SOURCE_DIR) + "/src/bach/compiler/bundle";
}

// 改出来的那几套放在跑测试时的当前目录下。
constexpr char kCheckRoot[] = "bundle_check";

// 对 moe_chip 那一份的改动：replace 把整行换成另一行，换成空串就是删掉这一行；
// append 在末尾加几行。
struct Mutation {
  std::vector<std::pair<std::string, std::string>> replace;
  std::vector<std::string> append;
};

// 写成 bundle_check/<name>/<name>.bachir。KERNEL 那几行不抄：几个反例在检查那一
// 步就停，改 core_bad_mask 的那一套只看 Router 与 TS 的配置。返回 replace 里有几
// 行在原文里找到了。
uint64_t WriteMutated(std::string const& name, Mutation const& m) {
  std::ifstream in(BundleRoot() + "/moe_chip/moe_chip.bachir");
  std::filesystem::create_directories(std::string(kCheckRoot) + "/" + name);
  std::ofstream out(std::string(kCheckRoot) + "/" + name + "/" + name +
                    ".bachir");
  uint64_t hit = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("KERNEL", 0) == 0) continue;
    for (auto const& r : m.replace) {
      if (line != r.first) continue;
      line = r.second;
      ++hit;
      break;
    }
    if (!line.empty()) out << line << "\n";
  }
  for (std::string const& a : m.append) out << a << "\n";
  return hit;
}

class BachBundleCheck : public ::testing::Test {
 protected:
  void SetUp() override {
    EnsureSlots();
    clk = MakeClock(0, kPeriod);
    chip = std::make_unique<Chip>(clk, "chip", ChipCfg{});
  }
  void TearDown() override {
    chip.reset();
    RT::Reset();
  }

  // 改完装载，应当在检查那一步断言。why 是断言信息里要出现的那一截。
  void ExpectRejected(std::string const& name, Mutation const& m,
                      char const* why = "装载检查没过") {
    ASSERT_EQ(WriteMutated(name, m), m.replace.size())
        << "要改的那几行在 moe_chip.bachir 里没找全，bundle 重新生成过就要跟着改";
    std::vector<Chip*> all = {chip.get()};
    EXPECT_DEATH(LoadBundle(all, kCheckRoot, name), why) << name;
  }

  ClockPtr clk;
  std::unique_ptr<Chip> chip;
};

}  // namespace

// 原样那一份：10 个 core 各一条 CORE，没有坏 core，B core 自启动。
TEST_F(BachBundleCheck, TheCompiledBundleLoads) {
  std::vector<Chip*> all = {chip.get()};
  BundleStat st = LoadBundle(all, BundleRoot(), "moe_chip");
  EXPECT_EQ(st.chips, 1u);
  EXPECT_EQ(st.cores, kChipCoreNum);
  for (uint64_t i = 0; i < kChipCoreNum; ++i) {
    EXPECT_FALSE(chip->GetCore(i).Bad()) << "i=" << i;
  }
  EXPECT_TRUE(chip->GetCore(0).GetTs().Cfg().SelfStartCore());
  EXPECT_FALSE(chip->GetCore(1).GetTs().Cfg().SelfStartCore());
}

// core5 不派角色、只有 RTAB，标成坏 core 也对得上：装得进去，只有它进透传档。
TEST_F(BachBundleCheck, BadCoreWithOnlyRouterRecordsLoads) {
  Mutation m;
  // 坏 core 只转发、不记账：改成坏 core 的同时把它那条 streamNeedMask 清掉。
  m.replace = {
      {"CHIP 0 0x000", "CHIP 0 0x020"},
      {"RTAB 0 5 4 1 4 0 0 0 0 0 0 0 0 1 0 4 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
       "RTAB 0 5 4 1 4 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0"}};
  ASSERT_EQ(WriteMutated("bad_core5", m), 2u);
  std::vector<Chip*> all = {chip.get()};
  LoadBundle(all, kCheckRoot, "bad_core5");
  for (uint64_t i = 0; i < kChipCoreNum; ++i) {
    EXPECT_EQ(chip->GetCore(i).Bad(), i == 5) << "i=" << i;
  }
}

// core_bad_mask 一行至多 1 位。
TEST_F(BachBundleCheck, TwoBadCoresInOneRowAreRejected) {
  ExpectRejected("two_in_row", {{{"CHIP 0 0x000", "CHIP 0 0x006"}}, {}},
                 "一行至多 1 个");
}

// 每颗 chip 的 10 个 core 都要有 CORE 记录。
TEST_F(BachBundleCheck, MissingCoreRecordIsRejected) {
  ExpectRejected("no_core3", {{{"CORE 0 3 0", ""}}, {}});
}

// 坏 core 的角色要是不派角色：core1 是计算 core，不能标成坏 core。
TEST_F(BachBundleCheck, BadCoreWithARoleIsRejected) {
  ExpectRejected("bad_compute", {{{"CHIP 0 0x000", "CHIP 0 0x002"}}, {}});
}

// 坏 core 上只能有 RTAB 与 RELROUTE。
TEST_F(BachBundleCheck, BadCoreWithOtherRecordsIsRejected) {
  ExpectRejected("bad_with_pathtask",
                 {{{"CHIP 0 0x000", "CHIP 0 0x020"}}, {"PATHTASK 0 5 4 0"}});
}

// 坏 core 的 RTAB 表项只能转发：进 core 那一档不行。
TEST_F(BachBundleCheck, BadCoreEnteringCoreIsRejected) {
  ExpectRejected(
      "bad_enters_core",
      {{{"CHIP 0 0x000", "CHIP 0 0x020"},
        {"RTAB 0 5 5 1 1 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
         "RTAB 0 5 5 1 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0"}},
       {}});
}

// B core 的 SELF_START 要是 1。
TEST_F(BachBundleCheck, BroadcastCoreWithoutSelfStartIsRejected) {
  ExpectRejected("bcore_no_self_start",
                 {{{"CFGMISC 0 0 16 1 0x5 1", "CFGMISC 0 0 16 0 0x5 1"}}, {}});
}

// 计算 core 的 SELF_START 要是 0。
TEST_F(BachBundleCheck, ComputeCoreWithSelfStartIsRejected) {
  ExpectRejected("compute_self_start",
                 {{{"CFGMISC 0 1 16 0 0x0 1", "CFGMISC 0 1 16 1 0x0 1"}}, {}});
}

// 只有 B core 的 B_CORE_DIRECTION 非 0。
TEST_F(BachBundleCheck, ComputeCoreWithBroadcastDirsIsRejected) {
  ExpectRejected("compute_bcast_dirs",
                 {{{"CFGMISC 0 1 16 0 0x0 1", "CFGMISC 0 1 16 0 0x5 1"}}, {}});
}

// 计算 core 的 DTEIN 落 Core Mem。
TEST_F(BachBundleCheck, ComputeCoreLandingInMatrixMemIsRejected) {
  ExpectRejected("compute_to_mm",
                 {{{"DTEIN 0 1 0 0 0x0 0", "DTEIN 0 1 1 0 0x0 0"}}, {}});
}

// B core 的 DTEIN 落 Matrix Mem。
TEST_F(BachBundleCheck, BroadcastCoreLandingInCoreMemIsRejected) {
  ExpectRejected("bcore_to_cm",
                 {{{"DTEIN 0 0 1 1 0x500 6144", "DTEIN 0 0 0 1 0x500 6144"}},
                  {}});
}

// 不派角色的 core 没有 CFGMISC、TCHAIN、DATAIN、DTEIN。
TEST_F(BachBundleCheck, SpareCoreWithDatainIsRejected) {
  ExpectRejected("spare_datain", {{}, {"DATAIN 0 5 0x0 0"}});
}
