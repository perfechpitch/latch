#ifndef _LATCH_BACH_IP_CHIP_CORE_MEMORY_MATRIX_MEM_
#define _LATCH_BACH_IP_CHIP_CORE_MEMORY_MATRIX_MEM_

// Matrix Mem：64 lane，每 lane 512KiB data + 64KiB scale。
// MU 申请独占窗口时停止接受新的 DTE/NoC 事务，旧请求及响应排空后才授予。
// DTE读写各自按256B/拍流水；根节点返回为73T，成对收齐另计。
// MU 本地读为 9T，data 128B 同时返回最多 16B scale。

#include <string>
#include <vector>

#include "bach/ip/chip/core/memory/banked_mem.h"

namespace latch {
namespace bach {

enum MatrixMemPort : uint64_t {
  kMmemMu = 0,        // 只读
  kMmemDteRd = 1,
  kMmemDteWr = 2,
  kMmemCfg = 3,
  kMmemPortNum = 4,
};

constexpr uint64_t kMatrixMemBytes = 36ull * 1024 * 1024;

inline std::vector<MemMaster> MatrixMemMasters() {
  return {
      {"mu", 0, 9, 9},        // 只读
      // DTE读写使用独立请求通道，各自每拍可接收一笔256B pair。
      {"dte_rd", 1, 73, 73, kMmemDteRd},
      {"dte_wr", 1, 73, 73, kMmemDteRd},
      {"cfg", 2, 50, 50},  // ctrl_noc 取 Mmem 的上界
  };
}

class MatrixMem : public BankedMem {
 public:
  MatrixMem(ClockPtr clock, const std::string& name, uint64_t parent = 0,
            bool tick = true)
      : BankedMem(clock, name, Cfg(), MatrixMemMasters(), parent, tick) {}

 private:
  static BankedMemCfg Cfg() {
    BankedMemCfg c;
    c.bank_num = 64;
    c.granule = 128;
    c.capacity = kMatrixMemBytes;
    c.exclusive_bank = false;
    c.exclusive_master = kMmemMu;
    c.external_pair_latency = 1;
    c.external_bank_outstanding = 16;
    c.external_pair_issue_cycles = 1;
    c.external_full_duplex = true;
    c.scale_bytes = 16;
    return c;
  }
};

}  // namespace bach
}  // namespace latch

#endif
