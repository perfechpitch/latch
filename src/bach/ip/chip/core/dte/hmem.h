#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_HMEM_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_HMEM_

// Hmem、Fast LUT、RouterTable 副本、本级 Reduce credit 表、stream_cache。
//
// 这几张表都是 DTE 自己持有、按索引直接读的静态或半静态内容，收在一个模块里。
//
// Hmem：一次搬运的对象是一个 MSG 包，进了 core 就按内容拆成四份、各存各的地方。
// 计算 core 上硬件包头与软件包头合并成一张表存这里，16 项按 stream_id 索引，
// 每项 {core_mask 2 B, sw_header 16 B}，共 288 B，软件只配一个地址。硬件只改
// 硬件包头的 core_mask，RV core 改软件包头。
//
// B core / R core 上包头存 Core Mem 的独立空间，走 Hmem 还是走 Core Mem 由
// hw_header_addr 这个地址本身选，不另设开关。
//
// Fast LUT：从「TS 把任务下发下来」到「总线上出现第一笔搬运请求」这一段叫 DTE
// Setup Time，目标压到 10T 以内。常规任务不走 RV core 的配置 kernel —— task_id
// 命中 Fast LUT 后硬件拿表项内容与 user_id 索引到的 User Base Register 拼出
// descriptor，直接推进 TaskQueue，命中路径 4T；未命中才启动 RV core 的 kernel，
// 代价是 Core Latency + 4T。它只加速任务配置，不改路由定义、数据通路和完成条件。
//
// stream_cache 是 Router 那张 stream 表的副本，只跟随、不分配：真正建表项只有
// Router 能做，这份靠 Router 各方向送回的 credit release 同步，用处是包要重发
// 时本地先记账。

#include <array>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/router/router_table.h"
#include "bach/ip/chip/core/dte/dte_types.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// Hmem 一项：硬件包头与软件包头合并。
struct HmemEntry {
  uint64_t core_mask = 0;                 // 2 B，硬件改
  std::array<uint8_t, 16> sw_header{};    // 16 B，RV core 改
  // DPU 写的那一对自定义包头。进核那一笔记在这里，出核造包时原样带上 —— 出口
  // 桩按它认这是哪个 GPU 的第几个 token，中途丢掉就分不清了。
  uint64_t gpu_id = 0, token_id = 0;
};

// Fast LUT 一项。
struct FastLutEntry {
  bool valid = false;
  uint64_t length = 0;
  uint64_t ctrl_flags = 0;
};

class Hmem : public BachModule {
 public:
  Hmem(ClockPtr clock, const std::string& name, uint64_t parent = 0,
       bool tick = true)
      : BachModule(clock, name, parent, tick), hits(clock), misses(clock) {
    rtab_copy.assign(kPathNum, NoOpEntry());
    path_task.assign(kPathNum, 0);
  }

  // ── Hmem：按 stream_id 索引 ──
  HmemEntry& Entry(uint64_t stream_id) {
    LOGCHECK(stream_id < kStreamCacheEntries, "Hmem: stream_id 越界。");
    return hmem[stream_id];
  }
  HmemEntry const& Entry(uint64_t stream_id) const {
    LOGCHECK(stream_id < kStreamCacheEntries, "Hmem: stream_id 越界。");
    return hmem[stream_id];
  }
  // 硬件只改 core_mask，出核时按需改写。
  void SetCoreMask(uint64_t stream_id, uint64_t mask) {
    Entry(stream_id).core_mask = mask;
  }

  // ── Fast LUT：boot 期经 ctrl_noc 配好，按 task_id 索引 ──
  void PreloadLut(uint64_t task_id, uint64_t length, uint64_t flags) {
    LOGCHECK(task_id < kPathNum, "Hmem: Fast LUT 下标越界。");
    lut[task_id] = {true, length, flags};
  }
  // 命中就能走 4T 的快路径，不命中要启动 RV core 的 kernel。
  bool LutHit(uint64_t task_id) {
    bool hit = task_id < kPathNum && lut[task_id].valid;
    if (hit) {
      ++hit_pending;
    } else {
      ++miss_pending;
    }
    return hit;
  }
  FastLutEntry const& Lut(uint64_t task_id) const { return lut.at(task_id); }

  // ── RouterTable 副本：按 PathID 查 VC 与资源需求 ──
  // 软件负责写入并保证与 Router、ReduceModule 三方一致，硬件不同步。
  void PreloadRtab(uint64_t path_id, RouteEntry e) {
    LOGCHECK(path_id < kPathNum, "Hmem: path_id 越界。");
    e.valid = true;
    rtab_copy[path_id] = e;
  }
  // path_task_map 的副本，与 TS 那一份同源、boot 期配成一样。进核搬运报完成
  // 时填的 task_id 按它查：那一笔是任务链上的第几步由收方的链定，包头里带的
  // 是发方的编号，两边对不上。
  void PreloadPathTask(uint64_t path_id, uint64_t task_id) {
    LOGCHECK(path_id < kPathNum, "Hmem: path_id 越界。");
    path_task[path_id] = task_id;
  }
  uint64_t PathTask(uint64_t path_id) const { return path_task.at(path_id); }

  RouteEntry const& Rtab(uint64_t path_id) const {
    return rtab_copy.at(path_id);
  }

  // ── 本级 Reduce credit：每用户一个 entry，flit 粒度 ──
  // 某个用户建 stream credit 表项时给这个用户分配一个 entry 的 credit 数量。
  void AllocReduceCredit(uint64_t user, uint64_t n) {
    reduce_credit[user % kStreamCacheEntries] = n;
    reduce_user[user % kStreamCacheEntries] = user;
  }
  uint64_t ReduceCredit(uint64_t user) const {
    uint64_t i = user % kStreamCacheEntries;
    return reduce_user[i] == user ? reduce_credit[i] : 0;
  }
  // 搬 Reduce 包前先检查本级 credit 是否够整包。
  bool ReduceCreditEnough(uint64_t user, uint64_t flits) const {
    return ReduceCredit(user) >= flits;
  }
  void TakeReduceCredit(uint64_t user, uint64_t flits) {
    uint64_t i = user % kStreamCacheEntries;
    LOGCHECK(reduce_user[i] == user && reduce_credit[i] >= flits,
             "Hmem: 本级 Reduce credit 不够就发了。");
    reduce_credit[i] -= flits;
  }
  // ReduceModule 每完成一次并把 flit 发给下游就释放一个，经独立通道送回。
  void ReturnReduceCredit(uint64_t user) {
    uint64_t i = user % kStreamCacheEntries;
    if (reduce_user[i] == user) ++reduce_credit[i];
  }

  // ── stream_cache：只跟随，不分配 ──
  void FollowStreamCredit(uint64_t dir, uint64_t user) {
    LOGCHECK(dir < kR2RNum, "Hmem: 方向越界。");
    for (uint64_t i = 0; i < kStreamCacheEntries; ++i) {
      if (cache_valid[dir][i] && cache_user[dir][i] == user) return;
    }
    for (uint64_t i = 0; i < kStreamCacheEntries; ++i) {
      if (!cache_valid[dir][i]) {
        cache_valid[dir][i] = true;
        cache_user[dir][i] = user;
        return;
      }
    }
  }
  void DropStreamCredit(uint64_t dir, uint64_t user) {
    for (uint64_t i = 0; i < kStreamCacheEntries; ++i) {
      if (cache_valid[dir][i] && cache_user[dir][i] == user) {
        cache_valid[dir][i] = false;
        return;
      }
    }
  }
  bool CacheHolds(uint64_t dir, uint64_t user) const {
    for (uint64_t i = 0; i < kStreamCacheEntries; ++i) {
      if (cache_valid[dir][i] && cache_user[dir][i] == user) return true;
    }
    return false;
  }

  uint64_t Hits() const { return hits.Get(); }
  uint64_t Misses() const { return misses.Get(); }

 protected:
  void Step() override {
    hits = hit_pending;
    misses = miss_pending;
    TracePerCycle("lut_hits", hit_pending);
  }

 private:
  static constexpr uint64_t kStreamCacheEntries = 16;

  std::array<HmemEntry, kStreamCacheEntries> hmem{};
  std::array<FastLutEntry, kPathNum> lut{};
  std::vector<RouteEntry> rtab_copy;
  std::vector<uint64_t> path_task;
  std::array<uint64_t, kStreamCacheEntries> reduce_credit{}, reduce_user{};
  std::array<std::array<bool, kStreamCacheEntries>, kR2RNum> cache_valid{};
  std::array<std::array<uint64_t, kStreamCacheEntries>, kR2RNum> cache_user{};
  uint64_t hit_pending = 0, miss_pending = 0;

  Logic64 hits, misses;
};

}  // namespace bach
}  // namespace latch

#endif
