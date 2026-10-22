#ifndef LATCH_BACH_IP_CHIP_CORE_MU_STQ_H
#define LATCH_BACH_IP_CHIP_CORE_MU_STQ_H

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "base/log.h"
#include "bach/common/numeric/fp32.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

constexpr uint64_t kMuConcatBytes = 1024;
constexpr uint64_t kMuReturnBase = 4;
constexpr uint64_t kMuStoreBase = 8;

// 64 条独立 4B 回程；站点 s 的最小回程为 4+s，末端 FIFO 为 8+2*(31-s)。
// 两个 phase 的 FP32 双结果共用一次 512B 预约，完整后才成为 ring 的可读前缀。
class MuStq : public BachModule {
 public:
  MuStq(ClockPtr clock, const std::string& name, uint64_t parent = 0,
        bool tick = true)
      : BachModule(clock, name, parent, tick), port(std::make_shared<MemPort>(clock)),
        stores(clock), drained(clock) {}
  MemPort& Port() { return *port; }
  void AttachPort(std::shared_ptr<MemPort> p) { port = std::move(p); }
  static uint64_t ReturnStages(uint64_t lane) { return kMuReturnBase + lane / 2; }
  static uint64_t StoreDepth(uint64_t lane) { return kMuStoreBase + 2 * (31 - lane / 2); }
  bool HasRoom(bool wide = false) const {
    for (uint64_t lane = 0; lane < 64; ++lane)
      // Future results may still occupy the A wave, before they enter this
      // station's return chain. One A-wave slot carries the complete frame.
      if (returns[lane].size() + (wide ? 2 : 1) > StoreDepth(lane) + ReturnStages(lane) +
          (wide ? 2 : 1) * (lane / 2)) return false;
    return true;
  }
  void Push(uint64_t address, std::vector<float> const& values, bool bf16,
            bool last = true, uint64_t task = ~0ull) {
    LOGCHECK(values.size() <= 128, "MU store: primitive exceeds 128 results");
    uint64_t columns = values.size() <= 64 ? 64 : 128;
    bool wide = columns == 128 && !bf16;
    LOGCHECK(HasRoom(wide), "MU store: return capacity exhausted");
    Frame frame{++next_frame, address, columns * (bf16 ? 2 : 4), wide, last, task};
    commands.push_back(frame);
    for (uint64_t lane = 0; lane < 64; ++lane) {
      uint64_t start = std::max(next_serializer[lane], CycleNow() + lane / 2);
      uint64_t word = 0;
      for (uint64_t v = 0; v < columns / 64; ++v) {
        uint64_t index = lane * (columns / 64) + v;
        uint32_t bits = index < values.size() ? numeric::BitsOf(values[index]) : 0;
        uint64_t value = bf16 ? numeric::Fp32ToBf16(bits).bits : bits;
        word |= value << (v * (bf16 ? 16 : 32));
      }
      returns[lane].push_back({frame.id, start + ReturnStages(lane), uint32_t(word), 0});
      if (wide) returns[lane].push_back({frame.id, start + ReturnStages(lane) + 1, uint32_t(word >> 32), 1});
      next_serializer[lane] = start + (wide ? 2 : 1);
    }
  }
  uint64_t Stores() const { return stores.Get(); }
  uint64_t Drained() const { return drained.Get(); }
  uint64_t AllocatedBytes() const { return allocated; }
  uint64_t CommittedBytes() const { return committed; }
  bool HasCompletedTask() const { return !completed_tasks.empty(); }
  uint64_t TakeCompletedTask() {
    auto task = completed_tasks.front(); completed_tasks.pop_front(); return task;
  }
  void Cancel() {
    for (auto& lane : returns) lane.clear();
    commands.clear();
    allocated = committed = read_pointer = write_pointer = 0;
    wide_pending = segment = closed = false;
    next_serializer.fill(0);
    pending_completions.clear(); completed_tasks.clear();
    port->IdleReq();
  }
  bool Quiescent() const override {
    if (allocated || !commands.empty() || wide_pending || !pending_completions.empty()) return false;
    for (auto const& lane : returns) if (!lane.empty()) return false;
    return true;
  }
 protected:
  void Step() override {
    Issue();
    Collect();
    while (!pending_completions.empty() && pending_completions.front().second <= CycleNow()) {
      completed_tasks.push_back(pending_completions.front().first);
      pending_completions.pop_front();
    }
    stores = store_count;
    drained = Quiescent();
    TracePerCycle("allocated_bytes", allocated);
    TracePerCycle("committed_bytes", committed);
    LOGCHECK(committed <= allocated && allocated <= 1024, "MU ring accounting");
  }
 private:
  struct Beat { uint64_t frame, at; uint32_t data; uint64_t phase; };
  struct Frame { uint64_t id, address, bytes; bool wide, last; uint64_t task; };
  void RingWord(uint64_t base, uint64_t offset, uint32_t word, uint64_t bytes) {
    for (uint64_t i = 0; i < bytes; ++i) ring[(base + offset + i) % 1024] = uint8_t(word >> (8 * i));
  }
  void Collect() {
    if (commands.empty()) return;
    auto const& frame = commands.front();
    if (!wide_pending && segment && (closed || frame.address != expected_end)) {
      closed = true;
      if (allocated) return;
      segment = false;
    }
    if (!segment) {
      segment = true; next_address = frame.address; expected_end = frame.address;
      closed = segment_last = false; segment_task = frame.task;
    }
    uint64_t phase = wide_pending ? 1 : 0;
    for (auto const& lane : returns)
      if (lane.empty() || lane.front().frame != frame.id || lane.front().phase != phase || lane.front().at > CycleNow()) return;
    if (!wide_pending) {
      if ((1024 - allocated) / 128 < frame.bytes / 128) return;
      assembly_base = write_pointer;
      write_pointer = (write_pointer + frame.bytes) % 1024;
      allocated += frame.bytes;
    }
    uint64_t bytes_per_lane = frame.bytes / 64;
    for (uint64_t lane = 0; lane < 64; ++lane) {
      auto beat = returns[lane].front(); returns[lane].pop_front();
      RingWord(assembly_base, lane * bytes_per_lane + phase * 4, beat.data,
               std::min<uint64_t>(4, bytes_per_lane));
    }
    if (frame.wide && !phase) { wide_pending = true; return; }
    committed += frame.bytes;
    expected_end = frame.address + frame.bytes;
    closed = frame.last;
    segment_last = frame.last;
    wide_pending = false;
    commands.pop_front();
  }
  void Issue() {
    if (!committed || !port->Ready() || !port->PairReady()) { port->IdleReq(); return; }
    uint64_t prefix = next_address % 128;
    uint64_t window = 256 - prefix;
    if (committed < window && !(closed && allocated == committed)) { port->IdleReq(); return; }
    uint64_t count = std::min(window, committed);
    auto data = std::make_shared<ByteBlock>(256, 0);
    for (uint64_t i = 0; i < count; ++i) (*data)[prefix + i] = ring[(read_pointer + i) % 1024];
    port->WritePair(next_address - prefix, data, prefix, count);
    read_pointer = (read_pointer + count) % 1024;
    next_address += count;
    allocated -= count; committed -= count;
    if (!allocated && closed && segment_last && segment_task != ~0ull)
      pending_completions.push_back({segment_task, CycleNow() + 2});
    ++store_count;
  }
  std::shared_ptr<MemPort> port;
  std::array<std::deque<Beat>, 64> returns;
  std::array<uint64_t, 64> next_serializer{};
  std::deque<Frame> commands;
  std::deque<std::pair<uint64_t, uint64_t>> pending_completions;
  std::deque<uint64_t> completed_tasks;
  std::array<uint8_t, 1024> ring{};
  uint64_t allocated = 0, committed = 0, read_pointer = 0, write_pointer = 0;
  uint64_t assembly_base = 0, next_frame = 0, next_address = 0, expected_end = 0, store_count = 0;
  uint64_t segment_task = ~0ull;
  bool wide_pending = false, segment = false, closed = false, segment_last = false;
  Logic64 stores, drained;
};
} // namespace bach
} // namespace latch
#endif
