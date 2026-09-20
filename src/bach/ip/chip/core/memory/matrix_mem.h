#ifndef _LATCH_BACH_IP_CHIP_CORE_MEMORY_MATRIX_MEM_
#define _LATCH_BACH_IP_CHIP_CORE_MEMORY_MATRIX_MEM_

// Matrix Mem：32 + 4 MB，64 bank × 0.5625 MB。
//
// scale 区与 Core Mem 同样是 SRAM 旁边的寄存器，与数据地址一一映射。硬件按
// scale : data = 1 : 8 留（32 MB 数据配 4 MB），MXFP8 每 128 B 数据只用其中 4 B，
// 所以模型按每 128 B 配 4 B 建；地址空间仍按非 scale 模式的 36 MB 算。
//
// 硬约束：DTE、ctrl_noc、MU 三者不能有两个同时访问同一个 bank。真硬件上撞了只
// 执行 MU、被让路的那一笔直接丢弃并计数，而 DTE 没有重传通路，丢一笔就少一段
// 数据、结果直接错。所以模型遇到这一笔直接断言失败，不用重试掩盖。
//
// DTE 的读口与写口是同一方：B core 与 R core 上 DTE 一边把进来的数据写进 Matrix
// Mem、一边把存着的读出去。两个口撞在同一个 bank 上排队，照 Core Mem MAS 对
// DTE 端口读写 bank 冲突的那一条：仲裁二选一，只反压冲突的那个 bank。Matrix
// Mem MAS 没写这一情形。
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
      // DTE 的读与写各占一个端口，读 8T、写 9T，两个口同属 DTE 一方。
      {"dte_rd", 1, 8, 8, kMmemDteRd},
      {"dte_wr", 1, 9, 9, kMmemDteRd},
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
    c.scale_bytes = 4;
    return c;
  }
};

}  // namespace bach
}  // namespace latch

#endif
