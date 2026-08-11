#ifndef _LATCH_RUNTIME_
#define _LATCH_RUNTIME_

#include <sys/syscall.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stack>
#include <thread>
#include <utility>
#include <vector>

#include "base/log.h"
#include "libco/co_routine.h"
#include "libco/co_routine_inner.h"
#include "recorder.h"
#include "time_stamp.h"

namespace latch {

extern thread_local Time Base;
extern thread_local Time Current;
extern thread_local Time Target;
extern thread_local uint64_t ThreadId;
constexpr uint64_t ThreadIdInit{std::numeric_limits<uint64_t>::max()};
constexpr uint64_t U64MAX{std::numeric_limits<uint64_t>::max()};

extern thread_local std::vector<std::pair<Time, int64_t>> stampLocal;
extern thread_local int curSimId;

Time GetCurrent();
Time GetBase();
Time GetTarget();

void SetBase(Time t);
void SetCurrent(Time t);
void SetTarget(Time t);

uint64_t GetThreadId();
void ResetStamp(Time t);
void CurrentIncrease(Time time);
void TargetIncrease(Time time);
void TargetAchieve();

class Recorder;

class ThreadPool {
 public:
  struct SimJob {
    Time time = 0;
    std::function<void(void)> func;

    uint32_t affinity = std::numeric_limits<uint32_t>::max();
  };
  static constexpr uint32_t kNoAffinity = std::numeric_limits<uint32_t>::max();

 private:
  class SimThread {
   public:
    struct CoTask {
      stCoRoutine_t* co;
      int isBusy = 0;
      Time time = 0;
      std::function<void(void)> func;
      uint64_t timeOfId0 = 0;
    };

    static void* co_func(void* ct);
    SimThread(int thread_id, int co_thread_num);
    ~SimThread() {}

    bool TryPushJob(SimJob& job) {
      std::lock_guard<std::mutex> lck(mutex);
      if (!pending.empty()) return false;
      bool anyFree = false;
      for (auto& c : cos) {
        if (c.isBusy == 0) { anyFree = true; break; }
      }
      if (!anyFree) return false;
      pending.emplace_back(std::move(job));
      isBusy.store(1, std::memory_order_release);
      cond.notify_all();
      return true;
    }

    void Join() {
      {
        std::unique_lock<std::mutex> lck(mutex);
        cond.wait(lck, [this] {
          return !isBusy.load(std::memory_order_acquire) &&
                 pending.empty() && allCosFree;
        });
      }
      keepalive.store(0, std::memory_order_release);
      cond.notify_all();
      if (thread.joinable()) thread.join();
    }

    std::mutex mutex;
    std::thread thread;
    std::deque<SimJob> pending;
    std::atomic<int> isBusy{0};
    std::atomic<int> keepalive{1};
    std::condition_variable cond;

    std::vector<CoTask> cos;

    std::atomic<bool> cosReady{false};

    std::atomic<bool> parked{false};
    int allCosFree;
    int co_thread;
    int thread_id;

    Time* BasePtr;
    Time* CurrentPtr;
    Time* TargetPtr;
  };

 public:
  ThreadPool()
      : oldestValue(0),
        threads(),
        lastLaunchThread(0),
        jobsToRun(),
        latch_co_thread_num(1),
        latch_sub_thread_num(8) {
    initThread();
    beginSetup();
  }
  ~ThreadPool() {}
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  void AddThread(std::function<void(void)> func, Time time,
                 uint32_t affinity = kNoAffinity) {
    std::lock_guard<std::mutex> lck1(jobsMutex);

    auto it = globalPending.find(time);
    if (it != globalPending.end()) it->second++;
    else globalPending.emplace(time, 1);
    pendingFloor.store(globalPending.begin()->first, std::memory_order_release);
    jobsToRun[time].push_back(SimJob{time, std::move(func), affinity});
    jobsEmpty.store(false, std::memory_order_release);
    issueJob();
  }

  void ResolvePending(Time time) {
    std::lock_guard<std::mutex> lk(jobsMutex);
    auto it = globalPending.find(time);
    LOGCHECK(it != globalPending.end(), "ResolvePending: time not in globalPending.");
    if (--it->second == 0) globalPending.erase(it);
    pendingFloor.store(
        globalPending.empty() ? TimeMax : globalPending.begin()->first,
        std::memory_order_release);
  }

  void MoveStamp(Time timeStamp, Time target) {
    localIncrease(target);
    if (localDecrease(timeStamp)) recomputeOldest();
  }
  void IncreaseStampCounter(Time tp) { localIncrease(tp); }
  void DecreaseStampCounter(Time tp) {
    if (localDecrease(tp)) recomputeOldest();
  }
  Time OldestStamp() const {
    return oldestValue.load(std::memory_order_acquire);
  }
  uint64_t OldestStampGen() const {
    return oldestStampGen.load(std::memory_order_acquire);
  }

  void IssueJob() {
    std::lock_guard<std::mutex> lck1(jobsMutex);
    issueJob();
  }

  void EndSetup() {
    if (!setupPin.exchange(false, std::memory_order_acq_rel)) return;

    recomputeOldest();
  }

  void TryIssueJob() {
    if (jobsEmpty.load(std::memory_order_acquire)) return;
    if (!jobsMutex.try_lock()) return;
    issueJob();
    jobsMutex.unlock();
  }

  void JoinAll() {
    EndSetup();
    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      {
        std::lock_guard<std::mutex> lck(jobsMutex);
        if (jobsToRun.size() != 0) {
          issueJob();
          continue;
        }
      }

      bool anyActive = false;
      for (auto& lm : localMins)
        if (lm.load(std::memory_order_acquire) != TimeMax) { anyActive = true; break; }
      if (!anyActive) break;
    }
    for (auto t : threads) {
      t->Join();
      delete t;
    }
    threads.clear();
  }

  void Reset(int sub_thread = 0, int co_thread = 0) {

    for (auto t : threads) {
      t->Join();
      delete t;
    }
    threads.clear();
    lastLaunchThread = 0;
    if (co_thread > 0) latch_co_thread_num = co_thread;
    if (sub_thread > 0) latch_sub_thread_num = sub_thread;
    jobsToRun.clear();
    jobsEmpty.store(true, std::memory_order_release);
    globalPending.clear();
    pendingFloor.store(TimeMax, std::memory_order_release);
    oldestValue.store(0, std::memory_order_release);
    oldestStampGen.store(0, std::memory_order_release);
    initThread();
    beginSetup();
  }

  std::map<Time, int64_t> GetTimestampCounter();
  std::vector<Time> GetThreadBaseTimeStamp();
  std::vector<Time> GetThreadBusy();
  std::vector<uint64_t> GetThreadId();
  std::vector<Time> GetJobsBaseTimeStamp();

 private:

  void localIncrease(Time tp) {
    auto it = std::lower_bound(
        stampLocal.begin(), stampLocal.end(), tp,
        [](const std::pair<Time, int64_t>& e, Time t) { return e.first < t; });
    if (it != stampLocal.end() && it->first == tp)
      it->second++;
    else
      stampLocal.insert(it, {tp, 1});
    localMins[curSimId].store(stampLocal.front().first, std::memory_order_release);
  }

  bool localDecrease(Time tp) {
    auto it = std::lower_bound(
        stampLocal.begin(), stampLocal.end(), tp,
        [](const std::pair<Time, int64_t>& e, Time t) { return e.first < t; });
    LOGCHECK(it != stampLocal.end() && it->first == tp, "Decrease Timestamp error!");
    if (it->second > 1) {
      it->second--;
      return false;
    }
    const Time oldMin = stampLocal.front().first;
    stampLocal.erase(it);
    const Time newMin = stampLocal.empty() ? TimeMax : stampLocal.front().first;
    localMins[curSimId].store(newMin, std::memory_order_release);
    return newMin > oldMin;
  }

  bool recomputeOldest() {
    if (setupPin.load(std::memory_order_acquire)) return false;

    Time gmin = pendingFloor.load(std::memory_order_acquire);
    for (auto& lm : localMins) {
      Time v = lm.load(std::memory_order_acquire);
      if (v < gmin) gmin = v;
    }
    Time cur = oldestValue.load(std::memory_order_acquire);
    while (gmin > cur) {
      if (oldestValue.compare_exchange_weak(cur, gmin, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
        oldestStampGen.fetch_add(1, std::memory_order_release);
        notifyStampReqers();
        return true;
      }

    }
    return false;
  }

  void notifyStampReqers() {
    for (auto* t : threads) {
      if (t->parked.load(std::memory_order_acquire)) {
        t->cond.notify_all();
      }
    }
  }
  void initThread() {
    threads.clear();

    localMins = std::vector<std::atomic<Time>>(latch_sub_thread_num);
    for (auto& lm : localMins) lm.store(TimeMax, std::memory_order_relaxed);
    for (int i = 0; i < latch_sub_thread_num; i++) {
      threads.push_back(new SimThread(i, latch_co_thread_num));
    }
  }

  void beginSetup() { setupPin.store(true, std::memory_order_release); }
  void issueJob() {

    const uint32_t threadCount =
        static_cast<uint32_t>(latch_sub_thread_num);
    const auto ts = threads.size() / 4;
    const bool sticky_enabled = (latch_co_thread_num > 1);
    for (size_t i = 0; i < threads.size(); ++i) {
      lastLaunchThread = (lastLaunchThread + 1) % threadCount;
      if (jobsToRun.empty()) break;
      auto& jobs = jobsToRun.begin()->second;
      const bool sticky = sticky_enabled &&
                          (jobs.front().affinity != kNoAffinity);
      const uint32_t slot = sticky ? (jobs.front().affinity % threadCount)
                                   : lastLaunchThread;
      auto t = threads.at(slot);
      if (sticky || lastLaunchThread > ts) {
        if (t->TryPushJob(jobs.front())) jobs.erase(jobs.begin());
        if (jobs.empty()) jobsToRun.erase(jobsToRun.begin());
      } else if (jobsToRun.begin()->first <= OldestStamp()) {
        if (t->TryPushJob(jobs.front())) jobs.erase(jobs.begin());
        if (jobs.empty()) jobsToRun.erase(jobsToRun.begin());
      }
    }

    jobsEmpty.store(jobsToRun.empty(), std::memory_order_release);
  }

  std::vector<std::atomic<Time>> localMins;

  alignas(64) std::atomic<uint64_t> oldestValue;
  std::atomic<uint64_t> oldestStampGen{0};

  std::atomic<bool> setupPin{true};

  std::map<Time, int64_t> globalPending;
  std::atomic<Time> pendingFloor{TimeMax};

  std::mutex jobsMutex;

  std::atomic<bool> jobsEmpty{true};

  std::vector<SimThread*> threads;
  uint32_t lastLaunchThread;
  std::map<Time, std::vector<SimJob>> jobsToRun;

  int latch_co_thread_num;
  int latch_sub_thread_num;
};

class ModulePool {
 public:
  ModulePool() : uuid_counter(0), modules() {}
  ~ModulePool() {}

  explicit ModulePool(const ModulePool&) = delete;
  void operator=(const ModulePool&) = delete;

  uint64_t CreateID(uint64_t pId, const std::string name) {
    std::lock_guard<std::mutex> lct(mutex);
    uuid_counter++;
    modules.insert({uuid_counter, {pId, name}});
    return uuid_counter;
  }
  uint64_t CreateID(const std::string name) { return CreateID(0, name); }

  std::pair<uint64_t, std::string>& GetInfo(uint64_t id) {
    std::lock_guard<std::mutex> lct(mutex);
    auto find = modules.find(id);
    if (find != modules.end()) {
      return find->second;
    }
    LOGCHECK(false, "ModulePool find error!");
  }
  std::string GetName(uint64_t id) { return GetInfo(id).second; }
  uint64_t GetPid(uint64_t id) { return GetInfo(id).first; }
  std::map<uint64_t, std::pair<uint64_t, std::string>>& Modules() {
    return modules;
  }

  void SetInfo(uint64_t id, std::string name, std::uint64_t pid) {
    std::lock_guard<std::mutex> lct(mutex);
    auto find = modules.find(id);
    if (find != modules.end()) {
      find->second.second = name;
      find->second.first = pid;
      return;
    }
    LOGCHECK(false, "ModulePool find error!");
  }
  void SetName(uint64_t id, std::string name) {
    std::lock_guard<std::mutex> lct(mutex);
    auto find = modules.find(id);
    if (find != modules.end()) {
      find->second.second = name;
      return;
    }
    LOGCHECK(false, "ModulePool find error!");
  }
  void SetPid(uint64_t id, std::uint64_t pid) {
    std::lock_guard<std::mutex> lct(mutex);
    auto find = modules.find(id);
    if (find != modules.end()) {
      find->second.first = pid;
      return;
    }
    LOGCHECK(false, "ModulePool find error!");
  }

  void Reset() {
    uuid_counter = 0;
    modules.clear();
  }

 private:
  std::mutex mutex;
  uint64_t uuid_counter;
  std::map<uint64_t, std::pair<uint64_t, std::string>> modules;
};

class Runtime {
 public:

  static void Launch(std::function<void(void)> func, Time time,
                     uint32_t affinity) {
    Runtime::GetThreadPool().AddThread(std::move(func), time, affinity);
  }
  static void Launch(std::function<void(void)> func, Time time) {
    Runtime::GetThreadPool().AddThread(std::move(func), time);
  }
  static void Launch(std::function<void(void)> func) {
    Runtime::GetThreadPool().AddThread(std::move(func), Now());
  }
  static void JoinAll() { Runtime::GetThreadPool().JoinAll(); }
  static void Synchronize() {
    auto& threadP = Runtime::GetThreadPool();
    threadP.TryIssueJob();
    auto now = GetBase();
    while (threadP.OldestStamp() < now) {
      co_yield_ct();
    }
  };
  static Time Now() { return GetCurrent(); }
  static void MoveStamp(Time timeStamp, Time target) {
    Runtime::GetThreadPool().MoveStamp(timeStamp, target);
  }
  static void IncreaseStampCounter(Time timeStamp) {
    Runtime::GetThreadPool().IncreaseStampCounter(timeStamp);
  }
  static void DecreaseStampCounter(Time timeStamp) {
    Runtime::GetThreadPool().DecreaseStampCounter(timeStamp);
  }
  static Time OldestStamp() { return Runtime::GetThreadPool().OldestStamp(); }
  static void EndSetup() { Runtime::GetThreadPool().EndSetup(); }

  template <typename... Args>
  static inline void Log(fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::info(fmt, std::forward<Args>(args)...);
  }
  static void LogWithTime(std::string log) {
    spdlog::info("{:<6d} {}", Now(), log);
  }
  template <typename... Args>
  static void LogWithTime(fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::info(fmt::format("T:{:<6d} ", Now()) +
                 fmt::format(fmt, std::forward<Args>(args)...));
  }

  static ThreadPool& GetThreadPool() {
    static ThreadPool threadPool;
    return threadPool;
  }

  static Recorder& GetRecorder() {
    static Recorder recorder;
    return recorder;
  }

  static void FlushRecorder() { GetRecorder().Finalize(); }

  static ModulePool& GetModulePool() {
    static ModulePool modulePool;
    return modulePool;
  }
  static void Reset(int sub_thread = 8, int co_thread = 8) {
    GetThreadPool().Reset(sub_thread, co_thread);
    GetModulePool().Reset();
    ResetStamp(0);
  }

  static void Delay(Time duration);

  Runtime() {};
  ~Runtime() {};

 private:
};

typedef Runtime RT;

}

#endif
