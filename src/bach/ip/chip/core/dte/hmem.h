#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_HMEM_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_HMEM_

// Hmem、RouterTable 副本、stream_cache。
//
// 这几张表都是 DTE 自己持有、按索引直接读的静态或半静态内容，收在一个模块里。
//
// Hmem：一次搬运的对象是一个 MSG 包，进了 core 就按内容拆成四段、各存各的地方。
// 硬件包头与软件包头合并成一张表存这里，16 项按 stream_id 索引，每项
// {core_mask 2 B, Hardware Used 1 B, sw_header 16
// B}，软件只配一个地址。硬件只改 硬件包头的 core_mask 与 Hardware Used，RV core
// 改软件包头。
//
// 包头段（段 0）落哪块存储由段 0 地址的端点译码决定：计算 core 配 header_table
// （端点 tag 0x4）落这里；B/R core 配 Core Mem 地址（端点 tag 0x0）落 Core
// Mem， 不落这里（见 dte_types.h 的 is_header 与 lane/commit 里的分支）。
//
// stream_cache 是 Router 那张 stream 表的副本，只跟随、不分配：真正建表项只有
// Router 能做，这份靠 Router 各方向送回的 credit release 同步，用处是包要重发
// 时本地先记账。

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <tuple>

#include "bach/ip/chip/core/dte/dte_types.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/chip/core/router/router_table.h"
#include "bach/ip/module_base.h"
#include "base/log.h"

namespace latch {
namespace bach {

// Hmem 一项：硬件包头与软件包头合并。
struct HmemEntry {
  uint64_t core_mask = 0;              // 2 B，硬件改
  uint64_t hardware_used = 0;          // 1 B，硬件改（Hardware Used 字段）
  std::array<uint8_t, 16> sw_header{}; // 16 B，RV core 改
  // DPU 写的那一对自定义包头。进核那一笔记在这里，出核造包时原样带上，出口
  // 桩按它认这是哪个 GPU 的第几个 token，中途丢掉就分不清了。
  uint64_t gpu_id = 0, token_id = 0;
  uint64_t uid = 0, pid = 0, packet_length = 0;
  std::array<uint8_t, 32> raw{};
};

// CM/MM use eight little-endian 32-bit words in the hardware bit layout.
constexpr uint64_t kHeaderCtxBytes = 32;

inline ByteBlock HeaderToBytes(HmemEntry const &h) {
  ByteBlock b(h.raw.begin(), h.raw.end());
  auto put = [&b](uint64_t off, uint64_t value, uint64_t size) {
    for (uint64_t i = 0; i < size; ++i)
      b[off + i] = uint8_t(value >> (8 * i));
  };
  for (uint64_t i = 0; i < 16; ++i)
    b[i] = h.sw_header[i];
  put(28, h.core_mask, 2);
  put(26, h.hardware_used, 1);
  put(24, h.packet_length, 2);
  put(22, h.uid, 2);
  put(31, h.pid, 1);
  return b;
}
inline HmemEntry BytesToHeader(ByteBlock const &b) {
  HmemEntry h;
  if (b.size() < 32)
    return h;
  std::copy_n(b.begin(), 32, h.raw.begin());
  std::copy_n(b.begin(), 16, h.sw_header.begin());
  h.core_mask = uint64_t(b[28]) | (uint64_t(b[29]) << 8);
  h.hardware_used = b[26];
  h.pid = b[31];
  h.uid = uint64_t(b[22]) | (uint64_t(b[23]) << 8);
  h.packet_length = uint64_t(b[24]) | (uint64_t(b[25]) << 8);
  return h;
}
inline ByteBlock HeaderOfMessage(Message const &m) {
  HmemEntry h;
  h.raw = m.header_raw;
  h.sw_header = m.sw_aux;
  h.core_mask = m.path_core_mask;
  h.hardware_used = (m.loopback & ~12u) | ((m.vc & 3) << 2);
  h.uid = m.user_id;
  h.pid = m.path_id;
  h.packet_length = m.PacketLength();
  return HeaderToBytes(h);
}
inline void ApplyHeader(Message &m, ByteBlock const &b) {
  auto h = BytesToHeader(b);
  m.header_raw = h.raw;
  m.sw_aux = h.sw_header;
  m.path_core_mask = h.core_mask;
  m.loopback = uint8_t(h.hardware_used);
}

class Hmem : public BachModule {
public:
  Hmem(ClockPtr clock, const std::string &name, uint64_t parent = 0,
       bool tick = true)
      : BachModule(clock, name, parent, tick) {
    for (uint64_t lane = 0; lane < 5; ++lane)
      ports.push_back(std::make_shared<MemPort>(clock));
    rtab_copy.assign(kPathNum, NoOpEntry());
    path_task.assign(kPathNum, 0);
  }

  // ── Hmem：按 stream_id 索引 ──
  HmemEntry &Entry(uint64_t stream_id) {
    LOGCHECK(stream_id < kStreamCacheEntries, "Hmem: stream_id 越界。");
    return hmem[stream_id];
  }
  HmemEntry const &Entry(uint64_t stream_id) const {
    LOGCHECK(stream_id < kStreamCacheEntries, "Hmem: stream_id 越界。");
    return hmem[stream_id];
  }
  // 硬件只改 core_mask，出核时按需改写。
  void SetCoreMask(uint64_t stream_id, uint64_t mask) {
    Entry(stream_id).core_mask = mask;
  }

  std::shared_ptr<MemPort> LanePort(uint64_t lane) const {
    return ports.at(lane);
  }

  HmemEntry Indexed(uint64_t index) const {
    LOGCHECK(index < 512, "HeaderTable: 索引越界。");
    HmemEntry h = hmem[index >> 5];
    h.raw.fill(0);
    h.uid = h.pid = h.packet_length = 0;
    h.hardware_used =
        (index & 31) ? hardware[index >> 5][index & 31] : h.hardware_used;
    return h;
  }
  void WriteIndexed(uint64_t index, ByteBlock const &bytes) {
    LOGCHECK(index < 512, "HeaderTable: 索引越界。");
    auto h = BytesToHeader(bytes);
    auto &group = hmem[index >> 5];
    group.core_mask = h.core_mask;
    group.sw_header = h.sw_header;
    hardware[index >> 5][index & 31] = uint8_t(h.hardware_used);
    if (!(index & 31))
      group.hardware_used = h.hardware_used;
  }
  uint32_t ReadSoftware(uint64_t address) const {
    if (address < 0x3000 || address >= 0x3400 || (address & 3))
      return 0;
    uint64_t group = (address - 0x3000) / 64, off = address & 63;
    if (off == 0)
      return hmem[group].core_mask & 0xffff;
    uint32_t value = 0;
    if (off >= 4 && off <= 16)
      for (uint64_t k = 0; k < 4; ++k)
        value |= uint32_t(hmem[group].sw_header[off - 4 + k]) << (8 * k);
    if (off >= 32)
      for (uint64_t k = 0; k < 4; ++k)
        value |= uint32_t(Indexed(group * 32 + off - 32 + k).hardware_used)
                 << (8 * k);
    return value;
  }
  uint32_t ReadSoftwareBeforeEdge(uint64_t address) const {
    if (address < 0x3000 || address >= 0x3400 || (address & 3))
      return 0;
    return software_snapshot[(address - 0x3000) / 4];
  }
  void WriteSoftware(uint64_t address, uint32_t value) {
    if (address < 0x3000 || address >= 0x3400 || (address & 3))
      return;
    uint64_t group = (address - 0x3000) / 64, off = address & 63;
    if (off == 0)
      hmem[group].core_mask = value & 0xffff;
    else if (off >= 4 && off <= 16)
      for (uint64_t k = 0; k < 4; ++k)
        hmem[group].sw_header[off - 4 + k] = uint8_t(value >> (8 * k));
    else if (off >= 32)
      for (uint64_t k = 0; k < 4; ++k) {
        uint64_t item = off - 32 + k;
        hardware[group][item] = uint8_t(value >> (8 * k));
        if (item == 0)
          hmem[group].hardware_used = hardware[group][item];
      }
  }
  struct Identity {
    uint64_t gpu = 0, token = 0, scale = 0, topk = 0;
  };
  void SaveIdentity(SegEndpoint ep, uint64_t address, Message const &m) {
    identities[{uint64_t(ep), address}] = {m.gpu_id, m.token_id, m.scale_valid,
                                           m.topk_valid};
  }
  void RestoreIdentity(SegEndpoint ep, uint64_t address, Message &m) const {
    auto it = identities.find({uint64_t(ep), address});
    if (it != identities.end()) {
      m.gpu_id = it->second.gpu;
      m.token_id = it->second.token;
      m.scale_valid = it->second.scale;
      m.topk_valid = it->second.topk;
    } else if (ep == SegEndpoint::kHeader) {
      m.gpu_id = hmem[address >> 5].gpu_id;
      m.token_id = hmem[address >> 5].token_id;
    }
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

  RouteEntry const &Rtab(uint64_t path_id) const {
    return rtab_copy.at(path_id);
  }

  // ── stream_cache：只跟随，不分配 ──
  void FollowStreamCredit(uint64_t dir, uint64_t user) {
    LOGCHECK(dir < kR2RNum, "Hmem: 方向越界。");
    for (uint64_t i = 0; i < kStreamCacheEntries; ++i) {
      if (cache_valid[dir][i] && cache_user[dir][i] == user)
        return;
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
      if (cache_valid[dir][i] && cache_user[dir][i] == user)
        return true;
    }
    return false;
  }

protected:
  void Step() override {
    for (uint64_t word = 0; word < software_snapshot.size(); ++word)
      software_snapshot[word] = ReadSoftware(0x3000 + word * 4);
    for (uint64_t lane = 0; lane < 5; ++lane) {
      auto &port = *ports[lane];
      auto req = ReadMemReq(port);
      if (req.valid && req.seq != seen[lane]) {
        LOGCHECK(requests[lane].size() < 4, "HeaderTable请求缓冲满。");
        seen[lane] = req.seq;
        requests[lane].push_back(req);
      }
      port.DriveSlave(requests[lane].size() < 2, false, nullptr);
    }
    std::set<uint64_t> responded;
    for (uint64_t rw = 0; rw < 2; ++rw) {
      uint64_t budget = 1;
      while (budget--) {
        bool granted = false;
        for (uint64_t age = 0; age < 5; ++age) {
          uint64_t lane = (turn[rw] + age) % 5;
          if (responded.count(lane) || requests[lane].empty() ||
              requests[lane].front().we != (rw == 1))
            continue;
          auto req = requests[lane].front();
          requests[lane].pop_front();
          LOGCHECK(req.addr < 512 && req.bytes == 32,
                   "HeaderTable: 非法索引或长度。");
          auto data =
              std::make_shared<ByteBlock>(HeaderToBytes(Indexed(req.addr)));
          if (rw)
            WriteIndexed(req.addr, *req.wdata);
          ports[lane]->DriveSlave(requests[lane].size() < 2, true,
                                  rw ? nullptr : data);
          granted = true;
          responded.insert(lane);
          turn[rw] = (lane + 1) % 5;
          break;
        }
        if (!granted)
          break;
      }
    }
  }

private:
  static constexpr uint64_t kStreamCacheEntries = 16;

  std::array<uint32_t, 256> software_snapshot{};
  std::vector<std::shared_ptr<MemPort>> ports;
  std::array<std::deque<MemReqView>, 5> requests;
  std::array<uint64_t, 5> seen{};
  std::array<uint64_t, 2> turn{};
  std::array<HmemEntry, kStreamCacheEntries> hmem{};
  std::array<std::array<uint8_t, 32>, 16> hardware{};
  std::map<std::pair<uint64_t, uint64_t>, Identity> identities;
  std::vector<RouteEntry> rtab_copy;
  std::vector<uint64_t> path_task;
  std::array<std::array<bool, kStreamCacheEntries>, kR2RNum> cache_valid{};
  std::array<std::array<uint64_t, kStreamCacheEntries>, kR2RNum> cache_user{};
};

} // namespace bach
} // namespace latch

#endif
