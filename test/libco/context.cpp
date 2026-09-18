// Standalone regression test: no simulator, HP-Socket, or GoogleTest dependency.
#include "co_routine.h"
#include "co_routine_inner.h"

#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); } } while (0)

// libco's simulator clock hooks, isolated per OS thread just like the runtime.
namespace latch {
thread_local uint64_t base, current, target, thread_id;
uint64_t GetBase() { return base; }
uint64_t GetCurrent() { return current; }
uint64_t GetTarget() { return target; }
uint64_t GetThreadId() { return thread_id; }
void SetBase(uint64_t v) { base = v; }
void SetCurrent(uint64_t v) { current = v; }
void SetTarget(uint64_t v) { target = v; }
void SetThreadId(uint64_t v) { thread_id = v; }
}

#if defined(__aarch64__)
extern "C" int co_test_registers(uint64_t seed);
thread_local coctx_t* raw_current = nullptr;
thread_local coctx_t* raw_main = nullptr;
extern "C" void co_test_yield() {
  if (raw_current) coctx_swap(raw_current, raw_main);
  else co_yield_ct();
}

struct RawJob {
  coctx_t context{};
  uint64_t seed;
  int steps = 0;
};

void* RawEntry(void* arg, void* second) {
  auto* job = static_cast<RawJob*>(arg);
  CHECK(second == raw_main);
  CHECK(std::fegetround() == FE_DOWNWARD);
  for (int i = 0; i < 1000; ++i) {
    CHECK(co_test_registers(job->seed) == 0);
    ++job->steps;
  }
  coctx_swap(&job->context, raw_main);
  std::abort();
}

void RunRawContexts() {
  coctx_t main_context{};
  RawJob jobs[2];
  std::vector<char> stacks[2] = {std::vector<char>(65536), std::vector<char>(65536)};
  raw_main = &main_context;
  CHECK(std::fesetround(FE_DOWNWARD) == 0);
  for (int i = 0; i < 2; ++i) {
    jobs[i].seed = 313 + i;
    coctx_init(&jobs[i].context);
    jobs[i].context.ss_sp = stacks[i].data();
    // Deliberately unaligned top: coctx_make must align it itself.
    jobs[i].context.ss_size = stacks[i].size() - 3;
    coctx_make(&jobs[i].context, RawEntry, &jobs[i], &main_context);
    CHECK(reinterpret_cast<uintptr_t>(jobs[i].context.regs[12]) % 16 == 0);
  }
  CHECK(std::fesetround(FE_TONEAREST) == 0);
  for (int step = 0; step <= 1000; ++step) {
    for (auto& job : jobs) {
      raw_current = &job.context;
      coctx_swap(&main_context, &job.context);
      CHECK(std::fegetround() == FE_TONEAREST);
    }
  }
  for (auto& job : jobs) CHECK(job.steps == 1000);
  raw_current = raw_main = nullptr;
}
#endif

struct Job {
  uint64_t seed;
  int steps = 0;
  int runs = 0;
};

__attribute__((noinline)) void StackFrames(Job* job, int depth) {
  volatile uint64_t canary[64];
  for (int i = 0; i < 64; ++i) canary[i] = job->seed + depth * 64 + i;
  if (depth) {
    StackFrames(job, depth - 1);
  } else {
    for (int i = 0; i < 1000; ++i) {
      latch::SetBase(job->seed);
      latch::SetCurrent(i);
      latch::SetTarget(job->seed + i);
      latch::SetThreadId(job->seed + 1);
#if defined(__aarch64__)
      const int rounding = job->seed % 2 ? FE_DOWNWARD : FE_UPWARD;
      CHECK(std::fesetround(rounding) == 0);
      std::feclearexcept(FE_ALL_EXCEPT);
      if (job->seed % 2) std::feraiseexcept(FE_INVALID);
      CHECK(co_test_registers(job->seed) == 0);
      CHECK(std::fegetround() == rounding);
      CHECK(!!std::fetestexcept(FE_INVALID) == !!(job->seed % 2));
#else
      co_yield_ct();
#endif
      CHECK(latch::GetBase() == job->seed);
      CHECK(latch::GetCurrent() == static_cast<uint64_t>(i));
      CHECK(latch::GetTarget() == job->seed + i);
      CHECK(latch::GetThreadId() == job->seed + 1);
      ++job->steps;
    }
  }
  for (int i = 0; i < 64; ++i) CHECK(canary[i] == job->seed + depth * 64 + i);
}

void* Worker(void* arg) {
  auto* job = static_cast<Job*>(arg);
  StackFrames(job, 4);
  ++job->runs;
  return nullptr;
}

void RunPair(bool shared) {
  stCoRoutineAttr_t attr;
  if (shared) attr.share_stack = co_alloc_sharestack(1, attr.stack_size);
  Job jobs[] = {{101}, {202}};
  stCoRoutine_t* coroutines[2];
  for (int i = 0; i < 2; ++i) CHECK(co_create(&coroutines[i], &attr, Worker, &jobs[i]) == 0);
  for (int run = 1; run <= 2; ++run) {
    for (int step = 0; step <= 1000; ++step) {
      for (auto* coroutine : coroutines) {
        co_resume(coroutine);
        CHECK(latch::GetBase() == 0);
        CHECK(latch::GetCurrent() == 0);
        CHECK(latch::GetTarget() == 0);
        CHECK(latch::GetThreadId() == 0);
#if defined(__aarch64__)
        CHECK(std::fegetround() == FE_TONEAREST);
        CHECK(std::fetestexcept(FE_INVALID) == 0);
#endif
      }
    }
    for (int i = 0; i < 2; ++i) {
      CHECK(jobs[i].steps == run * 1000);
      CHECK(jobs[i].runs == run);
      CHECK(coroutines[i]->cEnd);
      co_reset(coroutines[i]);
    }
  }
  for (auto* coroutine : coroutines) co_release(coroutine);
  if (shared) {
    auto* stack = attr.share_stack;
    std::free(stack->stack_array[0]->stack_buffer);
    std::free(stack->stack_array[0]);
    std::free(stack->stack_array);
    std::free(stack);
  }
}

void* Child(void* arg) {
  auto* value = static_cast<int*>(arg);
  ++*value;
  co_yield_ct();
  ++*value;
  return nullptr;
}

void* Parent(void* arg) {
  stCoRoutine_t* child;
  co_create(&child, nullptr, Child, arg);
  co_resume(child);
  CHECK(*static_cast<int*>(arg) == 1);
  co_yield_ct();
  co_resume(child);
  CHECK(child->cEnd);
  co_release(child);
  return nullptr;
}

void RunThread() {
#if defined(__aarch64__)
  RunRawContexts();
#endif
  RunPair(false);
  RunPair(true);
  int value = 0;
  stCoRoutine_t* parent;
  co_create(&parent, nullptr, Parent, &value);
  co_resume(parent);
  CHECK(value == 1);
  co_resume(parent);
  CHECK(value == 2 && parent->cEnd);
  co_release(parent);
}

int main() {
  RunThread();
  std::thread a(RunThread), b(RunThread);
  a.join();
  b.join();
  std::puts("PASS: context registers, FP state, private/shared stacks, reset, nested resume, threads");
}
