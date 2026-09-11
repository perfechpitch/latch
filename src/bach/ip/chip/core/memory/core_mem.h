#ifndef _LATCH_BACH_IP_CHIP_CORE_MEMORY_CORE_MEM_
#define _LATCH_BACH_IP_CHIP_CORE_MEMORY_CORE_MEM_

// Core Mem：(128 KB + 4 KB) × 8 bank = 1 MB + 32 KB。
//
// 优先级：MU > VU = DTE > {Router 的 CoreMem 重发、DTE RV core、ctrl_noc}。
// 最后三个平级、谁先到谁先得，因为它们的流量都很小，排先后对吞吐没有可测的影响，
// 平级反而少一套优先级逻辑。
//
// MU、VU、DTE 三家的读与写各占一个端口：《Cmem MAS》说的是「每组读写端口有
// bank 冲突需要 arb 二选一访问，无冲突可同时访问」，读与写因此不是同一根线。
// 合成一根的话同一拍里发出的读与写会互相盖掉。
//
// 延迟按各 master 那一侧的口径：DTE 13T、MU 16T、VU 14T，其余按 Cmem 的上界 15T。
//
// 按 stream_num 均等切分那一层不在这里：分片的基址由 master 侧算好，本模块只看
// 物理地址，也不检查请求落在哪个分区。

#include <string>
#include <vector>

#include "bach/ip/chip/core/memory/banked_mem.h"

namespace latch {
namespace bach {

// 端口序号，与构造时给的 master 列表一一对应。
enum CoreMemPort : uint64_t {
  kCmemMuRd = 0,
  kCmemMuWr = 1,
  kCmemVuRd = 2,
  kCmemVuWr = 3,
  kCmemDteRd = 4,
  kCmemDteWr = 5,
  kCmemReissue = 6,
  kCmemRvCore = 7,
  kCmemCfg = 8,
  kCmemPortNum = 9,
};

constexpr uint64_t kCoreMemBytes = 1024 * 1024 + 32 * 1024;

inline std::vector<MemMaster> CoreMemMasters() {
  return {
      {"mu_rd", 0, 16, 16},     // MU 的读是关键路径，单独一档
      {"mu_wr", 0, 16, 16},
      {"vu_rd", 1, 14, 14},     // VU 与 DTE 同档
      {"vu_wr", 1, 14, 14},
      {"dte_rd", 1, 13, 13},
      {"dte_wr", 1, 13, 13},
      {"reissue", 2, 15, 15},   // 后三档平级，先到先得
      {"rv_core", 2, 15, 15},
      {"cfg", 2, 15, 15},
  };
}

class CoreMem : public BankedMem {
 public:
  CoreMem(ClockPtr clock, const std::string& name, uint64_t parent = 0,
          bool tick = true)
      : BankedMem(clock, name, Cfg(), CoreMemMasters(), parent, tick) {}

 private:
  static BankedMemCfg Cfg() {
    BankedMemCfg c;
    c.bank_num = 8;
    c.granule = 128;
    c.capacity = kCoreMemBytes;
    c.exclusive_bank = false;  // 撞了排队，不是错误
    // 每 bank 另有 4 KB 寄存器存 scale：地址深度 1024 × 4 B，与 SRAM 的
    // 128 B 一一映射。
    c.scale_bytes = 4;
    return c;
  }
};

}  // namespace bach
}  // namespace latch

#endif
