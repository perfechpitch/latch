// 一层 MoE 摊在 48 颗 chip 上连续跑 32 个 token。拓扑、配置表、kernel 与各 core
// 的数据同 moe_lpu.cpp。GPU 每 100 拍注入一个 token；R core 槽号就是 user_id，
// 不扫表。user_id 必须小于 R core 槽数。
//
// 每个 token 一个用户号，按发的先后编：系统保证有效的 user_id 唯一，一个号在某个
// core 上的 stream 退休之前不会再来。
//
// 这一份跑得久，与单 token 那一份各自一个目标，方便单独跑。

#include <algorithm>
#include <iostream>
#include <set>

#include "test/bach/ip/chip/moe_common.h"

using namespace latch;
using namespace latch::bach;
using namespace latch::bach::moetest;

namespace {

constexpr uint64_t kTokens = 32;

// 记波形的 chip 数，从第 0 颗数起。
constexpr uint64_t kTraceChips = kLpuChips;

}  // namespace

// 32 个 token 各一个用户号，第 k 个的 user_id 就是 k（也是 R core 槽号）。出口上
// 32 包结果逐包与参考实现逐字节相同：按用户号认出是第几个 token，落点按下一行
// R core 上这个用户那一槽的后一半算。跑完各计算 core 的任务链都走空。
TEST(BachMoeLpu, ThirtyTwoTokens) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  constexpr uint64_t kMaxCycles = 200000;
  Vectors want = ReadVectors("moe_lpu_tokens.txt");
  ASSERT_FALSE(want.Empty())
      << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";

  std::vector<MessagePtr> got;
  std::vector<uint64_t> inflight, sent_at, out_at;
  {
    EnsureSlots(kLpuChips * (kMaxCorePerChip + 1));
    TraceInto("moe_lpu_tokens");
    ClockPtr clk = MakeClock(0, kPeriod);
    std::vector<std::unique_ptr<Chip>> owned = MakeLpuChips(clk, kTraceChips);
    std::vector<Chip*> all;
    for (auto const& c : owned) all.push_back(c.get());
    C2cBridge feed(clk, "feed", StubCfg());
    C2cBridge sink(clk, "sink", StubCfg());

    BundleStat st = LoadLpu(all);
    ASSERT_EQ(st.chips, kLpuChips);
    ASSERT_EQ(st.cores, kLpuChips * kMaxCorePerChip);

    SpreadHarness harness(clk, all, GridLinks(kGridY, kGridX), feed, sink, 0,
                          kChipW, kLpuChips - 1, kChipE, /*at=*/2, kMaxCycles,
                          MessagePtr());
    harness.token_gap = 100;
    for (uint64_t k = 0; k < kTokens; ++k) {
      // 槽号就是 user_id，从 0 起编，必须小于 R core 槽数。
      harness.tokens.push_back(MakeToken(kBcastInPath, 0, k, k));
    }
    clk->Continue();
    RT::JoinAll();
    CheckStreamDrained(all);
    got = harness.out_msgs;
    inflight = harness.inflight;
    sent_at = harness.sent_at;
    out_at = harness.out_at;
    std::cerr << "  停钟在第 " << harness.stopped_at << " 拍\n";
  }
  TraceDone();
  RT::Reset();

  if (!sent_at.empty() && !out_at.empty()) {
    std::cerr << "  32 个 token 在第 " << sent_at.front() << "～"
              << *std::max_element(sent_at.begin(), sent_at.end())
              << " 拍发出，结果在第 " << out_at.front() << "～" << out_at.back()
              << " 拍出来\n";
  }
  if (got.size() == kTokens && sent_at.size() == kTokens) {
    for (uint64_t i = 0; i < got.size(); ++i) {
      uint64_t k = got[i]->user_id;
      std::cerr << "    token " << k << " 发 " << sent_at[k] << " 出 "
                << out_at[i] << " 走了 " << out_at[i] - sent_at[k] << "\n";
    }
  }
  ASSERT_EQ(got.size(), kTokens) << "出口上要收到每个 token 一包结果";
  std::set<uint64_t> seen;
  for (MessagePtr const& m : got) {
    uint64_t k = m->user_id;
    ASSERT_LT(k, kTokens) << "出口上来了一包不认识的用户 " << m->user_id;
    ASSERT_TRUE(seen.insert(k).second) << "第 " << k << " 个 token 的结果多出来一包";
    CheckOutMsg(*m, want.Bytes("out" + std::to_string(k)),
                kn::RcLand(m->user_id, 1),
                "第 " + std::to_string(k) + " 个 token 的结果");
  }
  CheckInflight(inflight);
}
