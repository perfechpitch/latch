#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_HMEM_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_HMEM_

// Hmem、RouterTable 副本、stream_cache。
//
// 这几张表都是 DTE 自己持有、按索引直接读的静态或半静态内容，收在一个模块里。
//
// Hmem：一次搬运的对象是一个 MSG 包，进了 core 就按内容拆成四段、各存各的地方。
// 硬件包头与软件包头合并成一张表存这里，16 项按 stream_id 索引，每项
// {core_mask 2 B, Hardware Used 1 B, sw_header 16 B}，软件只配一个地址。硬件只改
// 硬件包头的 core_mask 与 Hardware Used，RV core 改软件包头。
//
// 对齐后包头段（段 0）统一走 header_table（端点 tag 0x4），不再区分计算 core 与
// B/R core 各自存哪。
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
  uint64_t hardware_used = 0;             // 1 B，硬件改（Hardware Used 字段）
  std::array<uint8_t, 16> sw_header{};    // 16 B，RV core 改
  // DPU 写的那一对自定义包头。进核那一笔记在这里，出核造包时原样带上，出口
  // 桩按它认这是哪个 GPU 的第几个 token，中途丢掉就分不清了。
  uint64_t gpu_id = 0, token_id = 0;
};

class Hmem : public BachModule {
 public:
  Hmem(ClockPtr clock, const std::string& name, uint64_t parent = 0,
       bool tick = true)
      : BachModule(clock, name, parent, tick) {
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

 protected:
  void Step() override {}

 private:
  static constexpr uint64_t kStreamCacheEntries = 16;

  std::array<HmemEntry, kStreamCacheEntries> hmem{};
  std::vector<RouteEntry> rtab_copy;
  std::vector<uint64_t> path_task;
  std::array<std::array<bool, kStreamCacheEntries>, kR2RNum> cache_valid{};
  std::array<std::array<uint64_t, kStreamCacheEntries>, kR2RNum> cache_user{};
};

}  // namespace bach
}  // namespace latch

#endif
