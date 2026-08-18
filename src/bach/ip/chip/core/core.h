#ifndef _LATCH_BACH_IP_CHIP_CORE_CORE_
#define _LATCH_BACH_IP_CHIP_CORE_CORE_

// 一个 Core。它是挂时钟的节点，内部八个单元都跟着它的一个协程走。
//
// 每拍的 stage 顺序按"末级先做"排：先让已经在做的事完成，再让新的事派下去。这样
// 一次完成与它引发的连锁反应落在同一拍，与 Bach 里同刻的函数调用一致：
//
//   Router     本拍到点的封包重组齐了交给 DTE，各入端口的新包进队列，仲裁转发
//   存储       访存到点的释放仲裁器，本拍排在后面的就能接上
//   倍率表     读写到点的落表，本拍要用它的计算与搬运才拿得到值
//   计算核     算完的 ack 出去
//   DTE        发完或收完的 ack 出去，收到的 RETIRE 把额度还给 CreditUnit
//   CreditUnit 拿上一步刚还回来的额度验资，验过的 ack 出去
//   TaskScheduler 把本拍所有 ack 一起看进来，推进各条流水，派下一批任务
//
// 排在最后的 TaskScheduler 看得到本拍全部完成，所以依赖链不掉拍；代价是它本拍派下去
// 的任务，各单元要到下一拍才开始动。这一拍相对 setup 的几十拍可以忽略，而依赖链掉拍
// 会直接改变屏障与退休顺序，所以两者之间取前者。
//
// Router 排在 DTE 之前，同样是这个取舍：本拍投递到的包 DTE 本拍就处理，而 DTE 本拍
// 发出去的包 Router 下一拍才仲裁。
//
// Core 对外只有 Router 的十二个端口。片内 mesh 的连线动作在 Chip 里做，但被连的端口
// 长在 Core 上。

#include <cstdint>
#include <string>

#include "base/clock.h"
#include "base/log.h"
#include "base/module.h"
#include "base/runtime.h"
#include "bach/common/packet.h"
#include "bach/ip/chip/core/compute/matrix_core.h"
#include "bach/ip/chip/core/compute/vector_core.h"
#include "bach/ip/chip/core/core_context.h"
#include "bach/ip/chip/core/credit_unit.h"
#include "bach/ip/chip/core/dte/dte.h"
#include "bach/ip/chip/core/memory/memory_system.h"
#include "bach/ip/chip/core/moe_bitmap.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/chip/core/router/router.h"
#include "bach/ip/chip/core/task_scheduler.h"
#include "bach/ip/route_config.h"
#include "bach/observer/span_recorder.h"

namespace latch {
namespace bach {

class Core : public ClkModule {
 public:
  Core(ClockPtr clock, CoreContext const& context, Coord position,
       RouteConfig const* route, std::string const& name, uint64_t parent)
      : ClkModule(clock),
        coord(position),
        ctx(WithRecorder(context, &recorder)),
        self_id(RegisterId(name, parent)),
        memory(clock, ctx, "memory", self_id),
        bitmap(clock, ctx, "bitmap", self_id),
        mc(clock, ctx, "matrix", self_id),
        vc(clock, ctx, "vector", self_id),
        dte(clock, ctx, "dte", self_id),
        cu(clock, ctx, "credit", self_id),
        ts(clock, ctx, "scheduler", self_id),
        router(clock, ctx, position, route, "router", self_id) {
    ctx.Validate();
    dte.Connect(&ts, &cu, &router, &bitmap, &memory);
    router.Connect(&dte);
    mc.Connect(&ts, &bitmap);
    vc.Connect(&ts, &bitmap);
    cu.Connect(&ts);
    ts.Connect(&dte, &mc, &vc, &cu);
  }

  // 软件 credit 图的一条边：本核向这个下游申请与归还额度。它与物理连线是两张图，
  // 谁跟谁有线不决定谁向谁要额度。
  void AddDownstream(int64_t core_id, uint64_t capacity, CoreType type) {
    cu.AddDownstream(core_id, capacity, type);
  }

  void Cycle() override {
    DelayCycle(1);

    router.Step();
    memory.Step();
    bitmap.Step();
    mc.Step();
    vc.Step();
    dte.Step();
    cu.Step();
    ts.Step();

    TracePerCycle("active_users", ts.ActiveUserNum());
    TracePerCycle("flows", ts.FlowNum());
    TracePerCycle("free_stream_slots", ts.FreeSlotNum());
    TracePerCycle("dte_jobs", dte.InFlight());
    TracePerCycle("dte_free_slots", dte.FreeSlotNum());
    TracePerCycle("router_queued", router.QueuedBeats());
  }

  uint64_t CoreId() const { return ctx.CoreId(); }
  Coord Position() const { return coord; }
  SpanRecorder const& Recorder() const { return recorder; }
  SpanRecorder& MutableRecorder() { return recorder; }

  Router& Rt() { return router; }
  TaskScheduler& Scheduler() { return ts; }
  Dte& DteUnit() { return dte; }
  CreditUnit& Credit() { return cu; }
  MoeBitMap& BitMap() { return bitmap; }
  MatrixCore& Matrix() { return mc; }
  VectorCore& Vector() { return vc; }
  MemorySystem& Memory() { return memory; }

 private:
  static CoreContext WithRecorder(CoreContext c, SpanRecorder* r) {
    c.rec = r;
    return c;
  }

  Coord coord;
  SpanRecorder recorder;
  CoreContext ctx;
  uint64_t self_id;

  MemorySystem memory;
  MoeBitMap bitmap;
  MatrixCore mc;
  VectorCore vc;
  Dte dte;
  CreditUnit cu;
  TaskScheduler ts;
  Router router;
};

}
}

#endif
