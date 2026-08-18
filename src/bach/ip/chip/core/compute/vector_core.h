#ifndef _LATCH_BACH_IP_CHIP_CORE_COMPUTE_VECTOR_CORE_
#define _LATCH_BACH_IP_CHIP_CORE_COMPUTE_VECTOR_CORE_

// 向量计算核。骨架与矩阵核相同，setup 时长不同，排队用当前 StreamID。
//
// 它多一个完成前的钩子：Phase1 的 router_softmax_topk 任务要在这一刻把本次激活的
// EPGroup 集合写出去，让下游在这条计算结束之后、而不是更早看到它。钩子不推进时间，
// 所以不用它的任务与用它的任务时序完全一样。

#include <string>

#include "base/clock.h"
#include "bach/ip/chip/core/compute/base_compute_core.h"

namespace latch {
namespace bach {

class VectorCore : public BaseComputeCore {
 public:
  VectorCore(ClockPtr clock, CoreContext const& context,
             std::string const& name, uint64_t parent, bool standalone = false)
      : BaseComputeCore(clock, context, Unit::kVector, name, parent,
                        standalone) {}

 protected:
  uint64_t SetupTime() const override { return ctx.P().vu_setup_time; }
};

}
}

#endif
