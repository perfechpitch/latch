#ifndef _LATCH_TEST_BACH_IP_FIXTURE_
#define _LATCH_TEST_BACH_IP_FIXTURE_

// 给 Core 及其单元的测试用的手写上下文。
//
// 中间文件读入那条路已经在 tables 那组测试里验过了，这里不再走它：这些测试关心的是
// 单元在给定任务表下的时序，任务表越短越好读，所以直接手写。

#include <cstdint>
#include <functional>
#include <vector>

#include "base/clock.h"
#include "base/module.h"
#include "base/runtime.h"
#include "bach/common/params.h"
#include "bach/ip/chip/core/core_context.h"
#include "bach/observer/span_recorder.h"
#include "bach/tables/credit_table.h"
#include "bach/tables/task_meta.h"
#include "bach/tables/task_table.h"

namespace latch {
namespace bach {
namespace test {

// 按拍驱动不挂时钟的单元。单元自己的 Step 由这里调。
//
// body 拿到的第二个参数是本次 run 的第几拍，从 0 起。用它而不是用时刻判断"第一拍"：
// Cycle 体内读到的时刻本身就从 0 开始，拿 0 当哨兵会把每一拍都当成第一拍。
class StepDriver : public ClkModule {
 public:
  StepDriver(ClockPtr c, std::function<void(Time, uint64_t)> fn)
      : ClkModule(c), body(std::move(fn)) {
    RegisterName("driver");
  }

  void Cycle() override {
    DelayCycle(1);
    body(RT::Now(), cycle);
    ++cycle;
  }

 private:
  std::function<void(Time, uint64_t)> body;
  uint64_t cycle = 0;
};

class CoreTables {
 public:
  explicit CoreTables(uint64_t id = 0) : core_id(id) {}

  CoreTables& Task(UnitType unit, Opcode op, uint64_t tag, uint64_t time_or_vol,
                   Coord dst = Coord{0, 0}, int64_t up = 0, int64_t down = -1) {
    TaskEntry e;
    e.unit = unit;
    e.opcode = op;
    e.tag = tag;
    e.up_cid = up;
    e.down_cid = down;
    e.dst = dst;
    e.time_or_vol = time_or_vol;
    tasks.Append(e);
    return *this;
  }

  CoreTables& Credit(uint64_t task_id, std::vector<int64_t> targets) {
    credits.Set(core_id, task_id, std::move(targets));
    return *this;
  }

  CoreTables& Meta(uint64_t task_id, TaskMeta const& m) {
    metas.Set(core_id, task_id, m);
    return *this;
  }

  // rec 传空时用自带的那个，单元单测就不必各自准备一个记录器。
  CoreContext Context(SpanRecorder* rec = nullptr) {
    CoreContext ctx;
    ctx.node_id = core_id;
    ctx.params = &params;
    ctx.tasks = &tasks;
    ctx.metas = &metas;
    ctx.skips = &skips;
    ctx.credits = &credits;
    ctx.rec = rec != nullptr ? rec : &recorder;
    return ctx;
  }

  Params params;
  SpanRecorder recorder;

 private:
  uint64_t core_id;
  CoreTaskTable tasks;
  TaskMetaTable metas;
  SkipSourceTable skips;
  CreditTable credits;
};

}
}
}

#endif
