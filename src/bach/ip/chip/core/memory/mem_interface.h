#ifndef _LATCH_BACH_IP_CHIP_CORE_MEMORY_MEM_INTERFACE_
#define _LATCH_BACH_IP_CHIP_CORE_MEMORY_MEM_INTERFACE_

// 一个访存口。它挂在某块存储器的仲裁器上，自带带宽。
//
// 同一块存储器上可以挂好几个口，它们各自的带宽不同，但共用一个仲裁器，所以带宽决定
// 这次要传几拍，仲裁器决定这几拍什么时候轮到。
//
// 拍数是 size 除以带宽向上取整，size 为 0 就是 0 拍，此时一次访问仍要花基础寻址
// 延迟。这跟链路的拍数换算不是一回事：链路上 size 不大于 0 也算一拍，因为链路上没有
// "零拍的传输"这回事。

#include <cstdint>
#include <string>

#include "base/log.h"
#include "base/time_stamp.h"
#include "bach/common/arbiter.h"
#include "bach/ip/chip/core/memory/mem_arbiter.h"

namespace latch {
namespace bach {

class MemInterface {
 public:
  MemInterface(MemArbiter* arb, std::string port, uint64_t bw)
      : arbiter(arb), port_name(std::move(port)), bandwidth(bw) {
    LOGCHECK(arbiter != nullptr, "MemInterface: arbiter is null.");
    LOGCHECK(bandwidth > 0, "MemInterface: bandwidth must be positive.");
  }

  Ticket Access(Time now, uint64_t uid, uint64_t tid, uint64_t size) {
    return arbiter->Request(now, uid, tid, (size + bandwidth - 1) / bandwidth);
  }

  bool Done(Ticket t) const { return arbiter->Done(t); }
  Time GrantCycle(Ticket t) const { return arbiter->GrantCycle(t); }
  Time EnqueueCycle(Ticket t) const { return arbiter->EnqueueCycle(t); }
  Time EndCycle(Ticket t) const { return arbiter->EndCycle(t); }
  void Finish(Ticket t) { arbiter->Finish(t); }

  std::string const& PortName() const { return port_name; }
  uint64_t Bandwidth() const { return bandwidth; }
  MemArbiter* Arbiter() const { return arbiter; }

 private:
  MemArbiter* arbiter;
  std::string port_name;
  uint64_t bandwidth;
};

}
}

#endif
