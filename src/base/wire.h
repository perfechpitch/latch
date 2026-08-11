#ifndef _LATCH_WIRE_
#define _LATCH_WIRE_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "base/log.h"
#include "libco/co_routine.h"

namespace latch {

class BlockingWire {
 public:
  explicit BlockingWire(uint64_t receiver_num) : readers(receiver_num) {
    LOGCHECK(receiver_num > 0, "receiver_num must > 0");
  }

  BlockingWire(BlockingWire const&) = delete;
  BlockingWire& operator=(BlockingWire const&) = delete;

  void Set(uint64_t v) {
    uint64_t epoch = produced_epoch.load(std::memory_order_relaxed);
    for (auto& r : readers) {
      while (r.consumed_epoch.load(std::memory_order_acquire) < epoch) {
        co_yield_ct();
      }
    }
    value.store(v, std::memory_order_relaxed);
    produced_epoch.store(epoch + 1, std::memory_order_release);
  }

  uint64_t Get(uint64_t receiver_idx) {
    LOGCHECK(receiver_idx < readers.size(), "Exceed receiver number!");
    auto& r = readers[receiver_idx];
    uint64_t want = r.consumed_epoch.load(std::memory_order_relaxed) + 1;
    while (produced_epoch.load(std::memory_order_acquire) < want) {
      co_yield_ct();
    }
    uint64_t v = value.load(std::memory_order_relaxed);
    r.consumed_epoch.store(want, std::memory_order_release);
    return v;
  }

  uint64_t ReceiverNum() const { return readers.size(); }

 private:

  static constexpr std::size_t kCacheLine = 64;

  struct alignas(kCacheLine) ReaderState {
    std::atomic<uint64_t> consumed_epoch{0};
  };

  alignas(kCacheLine) std::atomic<uint64_t> produced_epoch{0};
  alignas(kCacheLine) std::atomic<uint64_t> value{0};
  std::vector<ReaderState> readers;
};

}

#endif
