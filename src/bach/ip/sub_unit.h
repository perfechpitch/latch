#ifndef _LATCH_BACH_IP_SUB_UNIT_
#define _LATCH_BACH_IP_SUB_UNIT_

// 从属单元的共同基类。
//
// 挂时钟的节点自己有一个常驻协程，每拍由 runtime 调它的 Cycle。从属单元不挂时钟，
// 由上级节点在自己的 Cycle 体内按 stage 顺序调 Step，所以它与上级同拍工作：单元
// 之间是同刻的直接调用，不经过 Fifo，也就不多出拍。
//
// 由此得到两条约束：
//
//   Step 体内不得让出。上级的 Cycle 起手已经 DelayCycle(1)，从属单元再让出会把
//   Target 推过一个周期，runtime 当场断言。
//
//   单元的内部状态只被上级的一个调用点触碰，所以裸 vector 与标量就够，不需要任何
//   同步，也不该出现 mutex 或 atomic。
//
// 单元单测时把 standalone 传 true，它就自己挂时钟单独跑，由 Cycle 起手让出，不必
// 为了测一个单元拉起整个上级节点。

#include "base/clock.h"
#include "base/module.h"

namespace latch {
namespace bach {

class SubUnit : public ClkModule {
 public:
  SubUnit(ClockPtr clock, bool standalone)
      : ClkModule(clock, standalone), solo(standalone) {}

  // 单元的一拍工作全写在 Step 里。Cycle 只负责决定这一拍由谁让出。
  virtual void Step() = 0;

  void Cycle() final {
    if (solo) DelayCycle(1);
    Step();
  }

  bool Standalone() const { return solo; }

 private:
  const bool solo;
};

}
}

#endif
