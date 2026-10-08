#ifndef LATCH_TEST_BACH_IP_CHIP_MOE_LPU_TOKEN_CASE_H
#define LATCH_TEST_BACH_IP_CHIP_MOE_LPU_TOKEN_CASE_H

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <vector>

#include "test/bach/ip/chip/moe_common.h"

namespace latch {
namespace bach {
namespace moetest {

// 停钟后查 TS 与 Rmem 的任务边界，只打印行链占着分区而 TS 仍在 chip 内归约的用户。
inline void DumpReduceWaits(std::vector<Chip*> const& chips) {
  for (uint64_t chip = 0; chip < chips.size(); ++chip) {
    Core& core = chips[chip]->GetCore(CoreOfSlot(chips[chip]->Gx(), kn::kDotSlot));
    auto state = core.GetRouter().GetReduce().Inspect();
    for (auto const& task : state.tasks) {
      for (uint64_t stream = 0; stream < kStreamNum; ++stream) {
        auto const& entry = core.GetTs().Table().Peek(stream);
        if (!entry.valid || !entry.user_id_vld || entry.user_id != task.user_id ||
            entry.task_id >= task.reduce_seq) continue;
        std::cerr << "  reduce wait: chip " << chip << " user " << task.user_id
                  << " TS task " << entry.task_id << " path " << entry.task_path_id
                  << " Rmem task " << task.reduce_seq << " path " << task.path_id
                  << " received mask " << task.in_done_mask
                  << " expected mask " << task.expect_mask << "\n";
        for (uint64_t lane = 0; lane < state.input_heads.size(); ++lane) {
          auto const& f = state.input_heads[lane];
          if (!f.msg) continue;
          std::cerr << "    lane " << lane << " depth " << state.input_depth[lane]
                    << " head user " << f.msg->user_id << " task " << f.msg->reduce_seq
                    << " path " << f.msg->path_id << "\n";
        }
      }
    }
  }
}

// 按输出先后取正中间 64 个结果，统计窗口内部的 63 个相邻间隔。
inline void PrintMiddleOutputStats(std::vector<uint64_t> const& times,
                                   std::ostream& out = std::cerr) {
  constexpr size_t width = 64;
  if (times.size() < width) return;
  size_t first = (times.size() - width) / 2;
  size_t last = first + width - 1;
  std::vector<uint64_t> gaps;
  gaps.reserve(width - 1);
  for (size_t i = first + 1; i <= last; ++i) {
    gaps.push_back(times[i] - times[i - 1]);
  }
  std::sort(gaps.begin(), gaps.end());
  double average = double(times[last] - times[first]) / gaps.size();
  std::ostringstream line;
  line << "  中间 64 个结果（第 " << first + 1 << "～" << last + 1
       << " 个输出，63 个间隔）：平均 " << std::fixed << std::setprecision(1)
       << average << " cycle/token，中位数 " << gaps[gaps.size() / 2]
       << " cycle，最小 " << gaps.front() << " cycle，最大 " << gaps.back()
       << " cycle\n";
  out << line.str();
}

// 缺少参考向量时按当前引擎生成；32 与 128 token 用例各生成自己的文件和数量。
inline bool GenerateMoeLpuTokens(std::string const& name, uint64_t count) {
  std::string ref =
      std::string(LATCH_SOURCE_DIR) + "/src/bach/compiler/reference";
  std::string path = ref + "/vectors/" + name + ".txt";
  std::string cmd =
      "python3 -c '"
      "import sys; sys.path.insert(0, \"" +
      ref +
      "\"); import vectors; "
      "print(\"  比对向量走 %s\" % vectors.engine_name(), flush=True); "
      "vectors.write_moe_lpu_tokens(\"" + path + "\", " +
      std::to_string(count) + ")'";
  return std::system(cmd.c_str()) == 0;
}

inline void RunTokens(uint64_t count, uint64_t trace_chips,
                      std::string const& name, uint64_t max_cycles) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  Vectors want = ReadVectors(name + ".txt");
  if (want.Empty()) {
    ASSERT_TRUE(GenerateMoeLpuTokens(name, count)) << "比对向量没生成出来";
    want = ReadVectors(name + ".txt");
  }
  ASSERT_FALSE(want.Empty())
      << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";

  ASSERT_TRUE(want.Has("tokens"));
  ASSERT_EQ(std::stoull(want.kv.at("tokens")), count);
  for (uint64_t k = 0; k < count; ++k) {
    ASSERT_TRUE(want.Has("out" + std::to_string(k))) << "缺少 token " << k;
  }

  std::vector<MessagePtr> got;
  std::vector<uint64_t> inflight, sent_at, out_at;
  {
    EnsureSlots(kLpuChips * (kMaxCorePerChip + 1));
    TraceInto(name);
    ClockPtr clk = MakeClock(0, kPeriod);
    std::vector<std::unique_ptr<Chip>> owned = MakeLpuChips(clk, trace_chips);
    std::vector<Chip*> all;
    for (auto const& c : owned) all.push_back(c.get());
    C2cBridge feed(clk, "feed", StubCfg());
    C2cBridge sink(clk, "sink", StubCfg());

    BundleStat st = LoadLpu(all);
    ASSERT_EQ(st.chips, kLpuChips);
    ASSERT_EQ(st.cores, kLpuChips * kMaxCorePerChip);

    SpreadHarness harness(clk, all, GridLinks(kGridY, kGridX), feed, sink, 0,
                          kChipW, kLpuChips - 1, kChipE, /*at=*/2, max_cycles,
                          MessagePtr());
    harness.token_gap = 100;
    for (uint64_t k = 0; k < count; ++k) {
      // 槽号就是 user_id，从 0 起编，必须小于 R core 槽数。
      harness.tokens.push_back(MakeToken(kBcastInPath, 0, k, k));
    }
    clk->Continue();
    RT::JoinAll();
    if (harness.out_msgs.size() != count) DumpReduceWaits(all);
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
    std::cerr << "  " << count << " 个 token 在第 " << sent_at.front() << "～"
              << *std::max_element(sent_at.begin(), sent_at.end())
              << " 拍发出，结果在第 " << out_at.front() << "～" << out_at.back()
              << " 拍出来\n";
  }
  if (got.size() == count && sent_at.size() == count) {
    for (uint64_t i = 0; i < got.size(); ++i) {
      uint64_t k = got[i]->user_id;
      std::cerr << "    token " << k << " 发 " << sent_at[k] << " 出 "
                << out_at[i] << " 走了 " << out_at[i] - sent_at[k] << "\n";
    }
    PrintMiddleOutputStats(out_at);
  }
  ASSERT_EQ(got.size(), count) << "出口上要收到每个 token 一包结果";
  std::set<uint64_t> seen;
  for (MessagePtr const& m : got) {
    uint64_t k = m->user_id;
    ASSERT_LT(k, count) << "出口上来了一包不认识的用户 " << m->user_id;
    ASSERT_TRUE(seen.insert(k).second) << "第 " << k << " 个 token 的结果多出来一包";
    CheckOutMsg(*m, want.Bytes("out" + std::to_string(k)),
                kn::RcLand(m->user_id, 1),
                "第 " + std::to_string(k) + " 个 token 的结果");
  }
  CheckInflight(inflight);
}

}  // namespace moetest
}  // namespace bach
}  // namespace latch

#endif
