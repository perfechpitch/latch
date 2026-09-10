#ifndef _LATCH_BACH_IP_CHIP_CORE_MEMORY_MATRIX_MEM_
#define _LATCH_BACH_IP_CHIP_CORE_MEMORY_MATRIX_MEM_

// Matrix Mem：32 + 4 MB，64 bank × 0.5625 MB。
//
// 硬约束：DTE、ctrl_noc、MU 三者不能有两个同时访问同一个 bank。真硬件上撞了只
// 执行 MU、被让路的那一笔直接丢弃并计数，而 DTE 没有重传通路，丢一笔就少一段
// 数据、结果直接错。所以模型遇到这一笔直接断言失败，不用重试掩盖。
//
// bank 数有一处口径冲突未解：MU MAS 记 32 bank 与 32 lane 一对一，Mmem MAS 记
// 64 bank。这里按 Mmem MAS 的 64 取，直接影响 8 KB/T 怎么组织。

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
      {"mu", 0, 8, 8},        // 只读
      {"dte_rd", 1, 8, 8},    // DTE 的读与写各占一个端口，读 8T、写 9T
      {"dte_wr", 1, 9, 9},
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
    c.exclusive_bank = true;
    return c;
  }
};

}  // namespace bach
}  // namespace latch

#endif
