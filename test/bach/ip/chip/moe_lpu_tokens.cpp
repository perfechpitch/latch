// 48 颗 chip 连续处理 32 个不同 token，逐包比对数值并检查任务与资源全部回收。
#include "test/bach/ip/chip/moe_lpu_token_case.h"

TEST(BachMoeLpu, ThirtyTwoTokens) {
  latch::bach::moetest::RunTokens(32, latch::bach::moetest::kLpuChips,
                                 "moe_lpu_tokens", 200000);
}
