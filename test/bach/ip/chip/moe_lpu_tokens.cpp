// 一层 MoE 摊在 48 颗 chip 上连续跑 32 个 token。拓扑、配置表、kernel 与各 core
// 的数据同 moe_lpu.cpp，差别在 GPU 那一侧一直发：GPU 一侧有 16 份额度，一个
// token 占一份，它的结果从出口出来才还回来。32 个 token 发完之前，额度一直用满。
//
// 每个 token 一个用户号，按发的先后编：系统保证有效的 user_id 唯一，一个号在某个
// core 上的 stream 退休之前不会再来。R core 按用户号模 16 取槽，第 k 个 token 与
// 第 k + 16 个落同一个槽，前一个的结果出来了后一个才发。
//
// 额度取 16：B core 的环形缓冲与 R core 的槽都是 16 格，计算 core 的 stream 表
// 与 Router 的归约上下文也是 16 项。
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
constexpr uint64_t kCredit = 16;

// 记波形的 chip 数，从第 0 颗数起。
constexpr uint64_t kTraceChips = kLpuChips;

}  // namespace

// 32 个 token 各一个用户号，第 k 个是 kUserId + k，内容各不相同。出口上 32 包
// 结果逐包与参考实现逐字节相同：按用户号认出是第几个 token，落点按下一行 R core
// 上这个用户那一槽的后一半算。跑完各计算 core 的任务链都走空。
TEST(BachMoeLpu, ThirtyTwoTokensWithSixteenCredits) {
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
    for (uint64_t k = 0; k < kTokens; ++k) {
      // 落点在发的时候按送出的笔数填。
      harness.tokens.push_back(MakeToken(kBcastInPath, 0, kUserId + k, k));
    }
    harness.credit = kCredit;
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
      uint64_t k = got[i]->user_id - kUserId;
      std::cerr << "    token " << k << " 发 " << sent_at[k] << " 出 "
                << out_at[i] << " 走了 " << out_at[i] - sent_at[k] << "\n";
    }
  }
  ASSERT_EQ(got.size(), kTokens) << "出口上要收到每个 token 一包结果";
  std::set<uint64_t> seen;
  for (MessagePtr const& m : got) {
    ASSERT_GE(m->user_id, kUserId);
    uint64_t k = m->user_id - kUserId;
    ASSERT_LT(k, kTokens) << "出口上来了一包不认识的用户 " << m->user_id;
    ASSERT_TRUE(seen.insert(k).second) << "第 " << k << " 个 token 的结果多出来一包";
    CheckOutMsg(*m, want.Bytes("out" + std::to_string(k)),
                kn::RcLand(m->user_id, 1),
                "第 " + std::to_string(k) + " 个 token 的结果");
  }
  CheckInflight(inflight);
}
