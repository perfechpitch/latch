#ifndef _LATCH_BACH_IP_CHIP_CORE_MEMORY_MEMORY_SYSTEM_
#define _LATCH_BACH_IP_CHIP_CORE_MEMORY_MEMORY_SYSTEM_

// Core 里的存储子系统：两块存储器，五个访存口。
//
//   CoreMem    DTE、VectorCore、MatrixCore 各一个口
//   MatrixMem  DTE、MatrixCore 各一个口
//
// 两块存储器各自一个仲裁器，所以挂在同一块上的几个口互相排队，跨块的互不干扰。
//
// 这一层只提供服务，不知道谁在用它。真正发起访存的只有 DTE 的三条路径：reduction
// 的首包写入、FIFO 压栈、FIFO 弹出。MatrixCore 与 VectorCore 声明了口但计算过程不
// 访存，计算时间只来自 Map，这是继承自 Bach 的边界。

#include <cstdint>

#include "base/clock.h"
#include "bach/ip/chip/core/core_context.h"
#include "bach/ip/chip/core/memory/mem_arbiter.h"
#include "bach/ip/chip/core/memory/mem_interface.h"
#include "bach/ip/sub_unit.h"

namespace latch {
namespace bach {

class MemorySystem : public SubUnit {
 public:
  MemorySystem(ClockPtr clock, CoreContext const& context,
               std::string const& name, uint64_t parent,
               bool standalone = false)
      : SubUnit(clock, standalone),
        ctx(context),
        core_mem("CoreMem", context.P().cm_arb_delay),
        matrix_mem("MatrixMem", context.P().mm_arb_delay,
                   context.P().matrix_fifo_credit),
        dte_cm(&core_mem, "Port_DTE_CM", context.P().if_dte_cm),
        dte_mm(&matrix_mem, "Port_DTE_MM", context.P().if_dte_mm),
        vector_cm(&core_mem, "Port_Vector", context.P().if_vu_cm),
        matrix_cm(&core_mem, "Port_Matrix_CM", context.P().if_mu_cm),
        matrix_mm(&matrix_mem, "Port_Matrix_MM", context.P().if_mu_mm) {
    RegisterId(name, parent);
  }

  void Step() override {
    core_mem.Step();
    matrix_mem.Step();
  }

  MemInterface& DteCoreMem() { return dte_cm; }
  MemInterface& DteMatrixMem() { return dte_mm; }
  MemInterface& VectorCoreMem() { return vector_cm; }
  MemInterface& MatrixCoreMem() { return matrix_cm; }
  MemInterface& MatrixMatrixMem() { return matrix_mm; }

  MemArbiter& CoreMemArbiter() { return core_mem; }
  MemArbiter& MatrixMemArbiter() { return matrix_mem; }

 private:
  CoreContext ctx;

  MemArbiter core_mem;
  MemArbiter matrix_mem;

  MemInterface dte_cm;
  MemInterface dte_mm;
  MemInterface vector_cm;
  MemInterface matrix_cm;
  MemInterface matrix_mm;
};

}
}

#endif
