#ifndef _LATCH_BACH_IP_MODULE_BASE_
#define _LATCH_BACH_IP_MODULE_BASE_

// Bach 模型里每个硬件框图模块的基类。
//
// 一个模块一拍做的事全写在 Step() 里：读入口端口上一拍锁存的值，算本拍的组合
// 逻辑，把结果写到出口端口，下拍对方才看得到。模块之间没有调用关系，同一拍里
// 各模块的执行顺序不影响结果。
//
// Step() 里一次也不许让出。Cycle() 体内除起手的 DelayCycle(1) 外再让出，会挂起
// 这个协程、卡住 OldestStamp，整个 runtime 停在那里。硬件里的每一处等待都写成
// 跨拍状态机，每拍判断一次。
//
// tick=false 时模块不自己挂时钟，由装配层在自己的 Cycle() 里顺序调 RunStep()。
// 因为跨模块信号全部打拍，两种驱动方式的结果逐拍相同。48 chip 规模下每拍一次
// 全局 barrier 的开销如果过大，改的是装配层，模块代码不动。

#include <string>

#include "base/module.h"
#include "base/runtime.h"
#include "base/time_stamp.h"

namespace latch {
namespace bach {

// 波形层次里的一级名字。Router、TS、DTE 这一层的装配类自己不是模块、不出信号，
// 只给底下的模块当父节点，层次因此是 chip<i>.core<j>.<单元>.<模块>.<信号>。
inline uint64_t TraceGroup(const std::string& name, uint64_t parent) {
  return RT::GetModulePool().CreateID(parent, name);
}

class BachModule : public ClkModule {
 public:
  // parent 非 0 时把自己挂在那个 id 下，波形层次因此是
  // chip<i>.core<j>.<单元>.<模块>.<信号>。
  BachModule(ClockPtr clock, const std::string& name, uint64_t parent = 0,
             bool tick = true)
      : ClkModule(clock, tick) {
    if (parent != 0) {
      RegisterId(name, parent);
    } else {
      RegisterName(name);
    }
  }

  void Cycle() final {
    DelayCycle(1);
    Step();
  }

  // 装配层驱动时用这个代替挂时钟。
  void RunStep() { Step(); }

  // 本拍末有没有在做的事、有没有待做的事。判完成的一方只读这个，不去翻模块的
  // 内部容器：那些容器只由 Step() 触碰，外面读就是跨线程读非 atomic 容器。
  virtual bool Quiescent() const { return true; }

  uint64_t CycleNow() const { return clk->TimeToCycle(RT::Now()); }

 protected:
  virtual void Step() = 0;
};

}  // namespace bach
}  // namespace latch

#endif
