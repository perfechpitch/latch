#ifndef _LATCH_BUFFER_
#define _LATCH_BUFFER_

#include <cstdint>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/fifo.h"
#include "base/log.h"
#include "base/logic.h"
#include "base/module.h"
#include "base/time_stamp.h"

namespace latch {

template <typename T>
class Buffer : public Module {
 public:
  using Cell = LogicSlot<T>;

 private:
  uint64_t depth;
  std::vector<Cell>    data;
  std::vector<Logic64> is_valid;

 public:
  template <typename... Args>
  Buffer(uint64_t d, ClockPtr clk, Args&&... args) : depth(d) {
    data.reserve(d);
    is_valid.reserve(d);
    for (uint64_t i = 0; i < d; ++i) {
      data.emplace_back(clk, std::forward<Args>(args)...);
      is_valid.emplace_back(clk);
    }
  }

  template <typename U>
  std::enable_if_t<std::is_base_of_v<LogicBase, U>, uint64_t>
  Write(U const& v) {
    uint64_t e = AllocSlot();
    data[e] = v;
    return e;
  }
  template <typename U>
  std::enable_if_t<!std::is_base_of_v<LogicBase, U>, uint64_t>
  Write(U const& v) {
    uint64_t e = AllocSlot();
    data[e] = v;
    return e;
  }

  uint64_t AllocSlot() {
    for (uint64_t i = 0; i < depth; ++i) {
      if (uint64_t(is_valid[i]) == 0) {
        is_valid[i] = uint64_t{1};
        return i;
      }
    }
    LOGCHECK(false, "Buffer: no free slot.");
    return ~uint64_t{0};
  }

  Cell&       Read(uint64_t entry) {
    LOGCHECK(entry < depth && uint64_t(is_valid[entry]) > 0,
             "Buffer::Read: invalid entry.");
    return data[entry];
  }
  Cell const& Read(uint64_t entry) const {
    LOGCHECK(entry < depth && uint64_t(is_valid[entry]) > 0,
             "Buffer::Read: invalid entry.");
    return data[entry];
  }

  Cell&       At(uint64_t index)       { return data.at(index); }
  Cell const& At(uint64_t index) const { return data.at(index); }
  bool        IsValidIdx(uint64_t i) const {
    if (i >= depth) return false;
    return uint64_t(is_valid[i]) > 0;
  }

  uint64_t ValidCount() const {
    uint64_t c = 0;
    for (auto const& v : is_valid) if (uint64_t(v) > 0) c++;
    return c;
  }
  uint64_t InvalidCount() const { return depth - ValidCount(); }
  bool     IsFull()  const      { return ValidCount() >= depth; }
  bool     IsEmpty() const      { return ValidCount() == 0; }
  uint64_t Depth()   const      { return depth; }

  std::vector<uint64_t> ValidEntry() const {
    std::vector<uint64_t> out;
    for (uint64_t i = 0; i < depth; ++i)
      if (uint64_t(is_valid[i]) > 0) out.push_back(i);
    return out;
  }

  void Erase(uint64_t entry) { is_valid[entry] = uint64_t{0}; }

  void Clear() {
    for (auto& v : is_valid) v = uint64_t{0};
  }
  void Reset() {
    Clear();
    for (auto& d : data) d.Reset();
  }
};

}

#endif
