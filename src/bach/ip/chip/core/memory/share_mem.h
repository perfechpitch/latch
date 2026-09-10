#ifndef _LATCH_BACH_IP_CHIP_CORE_MEMORY_SHARE_MEM_
#define _LATCH_BACH_IP_CHIP_CORE_MEMORY_SHARE_MEM_

// Share Mem：32 KB，访问 5～10 拍，不需要初始化。
//
// 只被三个 RV core 的访存指令与 DTE DSA 的 shareMem 写口读写。四个 master 的
// 仲裁规则原文未给，按轮询建（待定）。
//
// 三种用途：task 之间的共享数据；B core 的 head / tail 指针；R core 的
// arrive_num 与两张 tmp_info 表。

#include <string>
#include <vector>

#include "bach/ip/chip/core/memory/banked_mem.h"

namespace latch {
namespace bach {

enum ShareMemPort : uint64_t {
  kSmemDteRv = 0,
  kSmemMuRv = 1,
  kSmemVuRv = 2,
  kSmemDteDsa = 3,
  kSmemPortNum = 4,
};

constexpr uint64_t kShareMemBytes = 32 * 1024;

inline std::vector<MemMaster> ShareMemMasters() {
  // 轮询下 priority 不参与比较，四个都填 0。延迟取 5～10 的中间值 8。
  return {
      {"dte_rv", 0, 8, 8},
      {"mu_rv", 0, 8, 8},
      {"vu_rv", 0, 8, 8},
      {"dte_dsa", 0, 8, 8},
  };
}

class ShareMem : public BankedMem {
 public:
  ShareMem(ClockPtr clock, const std::string& name, uint64_t parent = 0,
           bool tick = true)
      : BankedMem(clock, name, Cfg(), ShareMemMasters(), parent, tick) {}

 private:
  static BankedMemCfg Cfg() {
    BankedMemCfg c;
    c.bank_num = 1;
    c.granule = 4;
    c.capacity = kShareMemBytes;
    c.round_robin = true;
    return c;
  }
};

}  // namespace bach
}  // namespace latch

#endif
