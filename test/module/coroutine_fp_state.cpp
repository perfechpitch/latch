// 协程切换要把 MXCSR 与 x87 控制字一起换掉。
//
// 这两样不在 SysV 的 caller-saved 集合里 —— 编译器假定它们跨函数调用保持不变，
// 所以 coctx_swap 那个手写汇编不保存它们的话，一个协程调了 fesetround，同线程
// 里其他协程会跟着变。对 latch 里那些按 FP32 逐 bit 比对参考实现的模型，这是会
// 静默改结果的隐雷：一处 setround 就能让另一个模块的舍入全错，而且不报任何错。
//
// 用例不走 latch 的 runtime，直接用 libco 原语，把"谁污染谁"讲清楚。

#include <gtest/gtest.h>
#include <xmmintrin.h>

#include <cstdio>

#include "libco/co_routine.h"
#include "libco/co_routine_inner.h"

namespace {

int g_seen_by_b = -1;
int g_seen_by_caller = -1;

// 把 MXCSR 改成"向上舍入"之后让出。修好之前，这个设置会留在 MXCSR 里跟着
// 控制流走到别人身上。
void* SetRoundUpThenYield(void*) {
  _MM_SET_ROUNDING_MODE(_MM_ROUND_UP);
  co_yield_ct();
  return nullptr;
}

// 什么都不改，只报自己看到的舍入模式。
void* ReportRounding(void*) {
  g_seen_by_b = _MM_GET_ROUNDING_MODE();
  return nullptr;
}

}  // namespace

TEST(CoroutineFpState, RoundingModeDoesNotLeakAcrossCoroutines) {
  const int caller_mode = _MM_GET_ROUNDING_MODE();
  ASSERT_EQ(caller_mode, static_cast<int>(_MM_ROUND_NEAREST))
      << "这个用例假定进程启动时是默认的就近舍入";

  stCoRoutine_t* a = nullptr;
  stCoRoutine_t* b = nullptr;
  ASSERT_EQ(co_create(&a, nullptr, SetRoundUpThenYield, nullptr), 0);
  ASSERT_EQ(co_create(&b, nullptr, ReportRounding, nullptr), 0);

  co_resume(a);  // A 把 MXCSR 改成向上舍入，然后让出
  g_seen_by_caller = _MM_GET_ROUNDING_MODE();
  co_resume(b);  // B 应当看到默认的就近舍入，而不是 A 留下的向上舍入
  co_resume(a);  // 收掉 A

  std::printf("  A 让出后调用方看到 %d，B 看到 %d（就近舍入 = %d）\n",
              g_seen_by_caller, g_seen_by_b,
              static_cast<int>(_MM_ROUND_NEAREST));

  EXPECT_EQ(g_seen_by_caller, static_cast<int>(_MM_ROUND_NEAREST))
      << "A 的舍入模式泄漏给了调用方";
  EXPECT_EQ(g_seen_by_b, static_cast<int>(_MM_ROUND_NEAREST))
      << "A 的舍入模式泄漏给了协程 B";

  co_free(a);
  co_free(b);
  co_free_curr_thread_env();
}
