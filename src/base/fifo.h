#ifndef _LATCH_FIFO_
#define _LATCH_FIFO_

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/log.h"
#include "base/logic.h"
#include "base/module.h"
#include "base/time_stamp.h"

namespace latch {

template <typename T>
using LogicSlot = std::conditional_t<
    std::is_base_of_v<LogicBase, T>,
    T,
    LogicCell<T>>;

template <typename T>
class Fifo : public Module {
 public:
  using Cell = LogicSlot<T>;

 private:
  uint64_t          depth;
  Logic64           start, end, start_ta, end_ta;
  std::vector<Cell> data;

  void StepEnd() {
    uint64_t e = end;
    e++;
    if (e >= depth) { e = 0; end_ta = uint64_t(end_ta) == 0 ? 1 : 0; }
    end = e;
  }
  void StepStart() {
    uint64_t s = start;
    s++;
    if (s >= depth) { s = 0; start_ta = uint64_t(start_ta) == 0 ? 1 : 0; }
    start = s;
  }

 public:
  template <typename... Args>
  Fifo(uint64_t d, ClockPtr clk, Args&&... args)
      : depth(d),
        start(clk),
        end(clk),
        start_ta(clk),
        end_ta(clk) {
    data.reserve(d);
    for (uint64_t i = 0; i < d; ++i) {
      data.emplace_back(clk, std::forward<Args>(args)...);
    }
  }

  template <typename U>
  std::enable_if_t<std::is_base_of_v<LogicBase, U>, void>
  Push(U const& v) {
    LOGCHECK(!IsFull(), "Fifo is full!");
    uint64_t e = end;
    data[e] = v;
    StepEnd();
  }
  template <typename U>
  std::enable_if_t<!std::is_base_of_v<LogicBase, U>, void>
  Push(U const& v) {
    LOGCHECK(!IsFull(), "Fifo is full!");
    uint64_t e = end;
    data[e] = v;
    StepEnd();
  }

  Cell&       Front()       { LOGCHECK(!IsEmpty(), ""); return data[start]; }
  Cell const& Front() const { LOGCHECK(!IsEmpty(), ""); return data[start]; }
  Cell&       Back()        { return data[(uint64_t(end) + depth - 1) % depth]; }
  void        Pop()         { LOGCHECK(!IsEmpty(), ""); StepStart(); }

  Cell&       At(uint64_t index)        { return data.at(index); }
  Cell const& At(uint64_t index) const  { return data.at(index); }
  Cell&       AtFromStart(uint64_t i)   { return data.at((uint64_t(start) + i) % depth); }

  bool IsEmpty() const {
    return (uint64_t(start) == uint64_t(end)) &&
           (uint64_t(start_ta) == uint64_t(end_ta));
  }
  bool IsFull() const {
    return (uint64_t(start) == uint64_t(end)) &&
           (uint64_t(start_ta) != uint64_t(end_ta));
  }
  uint64_t Size() const { return depth; }
  uint64_t ValidCount() const {
    if (IsEmpty()) return 0;
    if (IsFull())  return depth;
    return (uint64_t(end) + depth - uint64_t(start)) % depth;
  }
  uint64_t InvalidCount() const { return depth - ValidCount(); }

  void Reset() {
    start    = uint64_t{0};
    end      = uint64_t{0};
    start_ta = uint64_t{0};
    end_ta   = uint64_t{0};
  }
};

template <typename T>
using FifoPtr = std::shared_ptr<Fifo<T>>;

}

#endif
