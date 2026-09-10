#ifndef _LATCH_BACH_IP_WIRING_
#define _LATCH_BACH_IP_WIRING_

// 接线。
//
// 一根物理线在模型里是一个 LinkEnd 对象：生产者往它写，消费者从它读，两侧都只
// 看到端口束的字段，不持有对方的类型。装配容器负责建线、把两端接上，装配顺序
// 不受构造顺序牵制。
//
// 每个模块构造时先给自己的每个端口建一根悬空的线，这样单测里不接也能跑。装配层
// 调 Attach 把两个模块换成同一根，线就通了。

#include <memory>

#include "base/clock.h"
#include "bach/common/flit.h"

namespace latch {
namespace bach {

inline LinkEndPtr MakeWire(ClockPtr clk) {
  return std::make_shared<LinkEnd>(clk);
}

}  // namespace bach
}  // namespace latch

#endif
