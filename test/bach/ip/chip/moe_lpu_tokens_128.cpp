// 48 颗 chip 连续处理 128 个不同 token，每 100 拍注入一个。
// 验证长期负载下两笔归约的调度、逐包数值及全部任务和业务资源的回收。
#include "test/bach/ip/chip/moe_lpu_token_case.h"

TEST(BachMoeLpu, OneHundredTwentyEightTokens) {
  latch::bach::moetest::RunTokens(128, 0, "moe_lpu_tokens_128", 60000);
}
