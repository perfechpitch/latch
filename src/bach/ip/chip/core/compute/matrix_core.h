#ifndef _LATCH_BACH_IP_CHIP_CORE_COMPUTE_MATRIX_CORE_
#define _LATCH_BACH_IP_CHIP_CORE_COMPUTE_MATRIX_CORE_

// 矩阵计算核。一次只算一条任务，时间由 Map 给。
//
// 它跟向量核的差别只有两处：setup 时长，以及排队用的优先级。优先级不在这里取，由
// TaskScheduler 在派发时给：矩阵核用 user 的全局到达序，向量核用当前 StreamID。
// 两者不同是因为矩阵核的任务通常是一个 user 里最长的一段，用到达序排能避免后到的
// user 因为 StreamID 变小而插队。

#include <string>

#include "base/clock.h"
#include "bach/ip/chip/core/compute/base_compute_core.h"

namespace latch {
namespace bach {

class MatrixCore : public BaseComputeCore {
 public:
  MatrixCore(ClockPtr clock, CoreContext const& context,
             std::string const& name, uint64_t parent, bool standalone = false)
      : BaseComputeCore(clock, context, Unit::kMatrix, name, parent,
                        standalone) {}

 protected:
  uint64_t SetupTime() const override { return ctx.P().mu_setup_time; }
};

}
}

#endif
