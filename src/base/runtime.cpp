#include "runtime.h"

#include "time_stamp.h"

namespace latch {

thread_local Time Base = Time(0);
thread_local Time Current = Time(0);
thread_local Time Target = Time(0);
thread_local uint64_t ThreadId = 0;

thread_local std::vector<std::pair<Time, int64_t>> stampLocal;
thread_local int curSimId = -1;

Time GetBase() { return Base; }
Time GetCurrent() { return Current; }
Time GetTarget() { return Target; }
uint64_t GetThreadId() { return ThreadId; }

void SetBase(Time t) { Base = t; }
void SetCurrent(Time t) { Current = t; }
void SetTarget(Time t) { Target = t; }
void SetThreadId(uint64_t id) { ThreadId = id; }

void CurrentIncrease(Time time) { Current = Current + time; }

void ResetStamp(Time t) {
  Base = t;
  Current = t;
  Target = t;
}
void TargetIncrease(Time time) { Target = Target + time; }
void TargetAchieve() {

  if (Base != Target) Runtime::MoveStamp(Base, Target);
  Base = Target;
  Current = Target;
}

std::map<Time, int64_t> ThreadPool::GetTimestampCounter() {

  std::map<Time, int64_t> m;
  m[OldestStamp()] = 1;
  return m;
}
std::vector<Time> ThreadPool::GetThreadBaseTimeStamp() {
  std::vector<Time> tnow;
  tnow.reserve(256);
  for (auto t : threads) {
    tnow.push_back(*(t->BasePtr));
  }
  return tnow;
}
std::vector<Time> ThreadPool::GetThreadBusy() {
  std::vector<Time> tnow;
  tnow.reserve(256);
  for (auto t : threads) {
    tnow.push_back(t->isBusy.load(std::memory_order_relaxed));
  }
  return tnow;
}
std::vector<Time> ThreadPool::GetJobsBaseTimeStamp() {
  std::vector<Time> tnow;
  tnow.reserve(128);
  for (auto t : jobsToRun) {
    tnow.push_back(t.first);
  }
  return tnow;
}

static constexpr int kBarrierSpin = 4096;

void* ThreadPool::SimThread::co_func(void* ct) {
  CoTask* ctp = static_cast<CoTask*>(ct);
  while (true) {
    if (ctp->func != nullptr) {
      ctp->isBusy = 1;
      const Time t = ctp->time;
      ResetStamp(t);

      Runtime::IncreaseStampCounter(t);
      Runtime::GetThreadPool().ResolvePending(t);
      while (t > Runtime::OldestStamp()) co_yield_ct();
      ctp->func();
      Runtime::DecreaseStampCounter(GetBase());
      ResetStamp(0);
      ctp->func = nullptr;
      ctp->isBusy = 0;
    } else {
      co_yield_ct();
    }
  }
  return nullptr;
}

ThreadPool::SimThread::SimThread(int id, int co_thread_num)
    : mutex(),
      pending(),
      isBusy(0),
      keepalive(1),
      cond(),
      cos(),
      co_thread(co_thread_num),
      thread_id(id),
      allCosFree(false) {
  LOGCHECK(thread_id >= 0, "thread id error.");
  LOGCHECK((thread_id + 1) * co_thread_num < 65535, "total thread too large.");
  thread = std::thread(
      [](SimThread* t) {
        curSimId = t->thread_id;
        int cothread = t->co_thread;

        t->cos.reserve(cothread);
        for (int i = 0; i < cothread; i++) {
          t->cos.emplace_back();
          CoTask& cot = t->cos.back();
          co_create(&cot.co, NULL, co_func, &cot);
          cot.co->ThreadIdCo = (t->thread_id * cothread + i) << 16;
        }
        t->BasePtr = &Base;
        t->CurrentPtr = &Current;
        t->TargetPtr = &Target;
        {
          std::lock_guard<std::mutex> lk(t->mutex);
          t->cosReady.store(true, std::memory_order_release);
        }
        t->cond.notify_all();
        auto& tp = Runtime::GetThreadPool();
        while (t->keepalive) {

          uint64_t gen_snap = tp.OldestStampGen();

          bool resumedAny = false;
          bool anyBusy = false;
          bool anyFree = false;
          auto cur_oldest = tp.OldestStamp();
          for (int i = 0; i < cothread; i++) {
            if (t->cos[i].isBusy) {
              anyBusy = true;
              auto co = t->cos[i].co;

              if (co->BaseTime <= cur_oldest) {
                resumedAny = true;
                co_resume(co);
              }
            } else {
              anyFree = true;
            }
          }
          if (resumedAny) continue;

          if (anyFree && !t->pending.empty()) {
            std::unique_lock<std::mutex> lk(t->mutex);
            if (!t->pending.empty()) {
              t->allCosFree = false;
              t->isBusy = 1;
              for (int i = 0; i < cothread; i++) {
                if (t->cos[i].isBusy == 0) {
                  CoTask& cotask = t->cos[i];
                  auto tid = cotask.co->ThreadIdCo;
                  uint64_t current_id = tid & 0xffff;
                  current_id = (current_id + 1) & 0xffff;
                  if (current_id == 0) {
                    auto curr = Runtime::OldestStamp();
                    auto to0 = cotask.timeOfId0;
                    LOGCHECK(to0 < curr, "id overflow at same cycle.");
                    cotask.timeOfId0 = curr;
                  }
                  cotask.co->ThreadIdCo = (tid & 0xffff0000) | current_id;
                  auto& job = t->pending.front();
                  cotask.time = job.time;
                  cotask.func = std::move(job.func);
                  t->pending.pop_front();
                  cotask.isBusy = 1;
                  break;
                }
              }
            }
            continue;
          }

          if (anyBusy) {

            if (tp.recomputeOldest()) continue;

            auto barrierReady = [&]() {
              return !t->pending.empty() ||
                     !t->keepalive.load(std::memory_order_acquire) ||
                     tp.OldestStampGen() != gen_snap;
            };
            bool woken = false;
            for (int spin = 0; spin < kBarrierSpin; ++spin) {
              if (barrierReady()) { woken = true; break; }
#if defined(__x86_64__) || defined(__i386__)
              __builtin_ia32_pause();
#endif
            }
            if (!woken) {
              std::unique_lock<std::mutex> lk(t->mutex);
              t->parked.store(true, std::memory_order_release);
              t->cond.wait_for(lk, std::chrono::microseconds(500), barrierReady);
              t->parked.store(false, std::memory_order_release);
            }
            continue;
          }

          {
            std::unique_lock<std::mutex> lk(t->mutex);
            t->isBusy = 0;
            if (!t->pending.empty()) continue;
            t->allCosFree = true;
            t->cond.notify_all();
            t->cond.wait_for(
                lk, std::chrono::milliseconds(100), [&]() {
                  return !t->pending.empty() ||
                         !t->keepalive.load(std::memory_order_acquire);
                });
          }
        }
      },
      this);

  std::unique_lock<std::mutex> lk(mutex);
  cond.wait(lk, [this] { return cosReady.load(std::memory_order_acquire); });
}

void Runtime::Delay(Time duration) {
  TargetAchieve();
  Runtime::Synchronize();
  TargetIncrease(duration);
}

}
