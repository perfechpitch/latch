#ifndef LATCH_BACH_IP_CHIP_CORE_MU_LDQ_H
#define LATCH_BACH_IP_CHIP_CORE_MU_LDQ_H
#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "base/log.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/module_base.h"

namespace latch { namespace bach {
constexpr uint64_t kTokenLdqDepth = 64;
constexpr uint64_t kWeightLdqDepth = 64;
constexpr uint64_t kMuRdOutstanding = 128;

class MuLdq : public BachModule {
 public:
  struct Req {
    uint64_t addr = 0, bytes = 0, tag = 0;
    bool scale_en = false;
    bool pair = false, lane_memory = false;
    uint64_t lane_bytes = 128, scale_group = 32, memory_limit = 1024 * 1024;
  };
  struct Rsp {
    uint64_t tag = 0;
    std::vector<uint8_t> data;
    uint64_t error = 0, reserved_bytes = 0;
  };
  MuLdq(ClockPtr clock, const std::string& name, uint64_t slots,
        uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick), depth(slots), capacity(slots * 256),
        port(std::make_shared<MemPort>(clock)), issued(clock), returned(clock) {}
  void SetBuffer(uint64_t bytes, uint64_t outstanding) { capacity = bytes; max_outstanding = outstanding; }
  MemPort& Port() { return *port; }
  void AttachPort(std::shared_ptr<MemPort> p) { port = std::move(p); }
  bool HasRoom() const { return reserved < capacity && order.size() + done.size() < depth; }
  bool HasRoomFor(uint64_t bytes) const { return HasRoom() && bytes <= capacity - reserved; }
  void Push(Req const& req) {
    LOGCHECK(HasRoomFor(req.bytes), "MU load buffer reservation overflow");
    auto pending = std::make_shared<Pending>(); pending->req = req;
    pending->data.resize(req.bytes + (req.scale_en ? req.bytes / req.scale_group : 0));
    reserved += req.bytes; q.push_back(pending); order.push_back(pending);
  }
  bool HasData() const { return !done.empty(); }
  Rsp TakeData() {
    auto result = std::move(done.front()); done.pop_front();
    reserved -= result.reserved_bytes; return result;
  }
  uint64_t Issued() const { return issued.Get(); }
  uint64_t Returned() const { return returned.Get(); }
  uint64_t ReservedBytes() const { return reserved; }
  void Cancel() {
    cancelling = true;
    for (auto& pending : order) pending->all_issued = true;
    q.clear();
  }
  void Resume() { cancelling = false; }
  bool Quiescent() const override { return q.empty() && inflight.empty() && done.empty() && order.empty(); }
 protected:
  void Step() override {
    Collect();
    if (cancelling) {
      while (HasData()) TakeData();
      port->IdleReq();
    } else Issue();
    issued = issue_count; returned = return_count;
    TracePerCycle("reserved_bytes", reserved);
    TracePerCycle("inflight", inflight.size());
  }
 private:
  struct Pending { Req req; ByteBlock data; uint64_t issued_bytes = 0, waiting = 0, error = 0; bool all_issued = false; };
  struct Flight { std::shared_ptr<Pending> owner; uint64_t offset, skip, count; };
  void Issue() {
    if (q.empty() || inflight.size() >= max_outstanding || !port->Ready()) { port->IdleReq(); return; }
    auto pending = q.front(); auto const& req = pending->req;
    uint64_t offset = pending->issued_bytes, skip = 0, count = req.bytes;
    if (req.lane_memory) {
      port->Read(req.addr & ~127ull, 8192, req.scale_en);
      port->req_scale_group = req.scale_group;
      skip = req.addr & 127;
    } else if (req.pair) {
      if (!port->PairReady()) { port->IdleReq(); return; }
      uint64_t address = req.addr + offset;
      uint64_t base = address & ~127ull;
      if (base + 256 > req.memory_limit) base = req.memory_limit - 256;
      skip = address - base;
      count = std::min<uint64_t>(256 - skip, req.bytes - offset);
      port->ReadPair(base);
      port->req_scale_en = req.scale_en ? kScaleWithData : kScaleNone;
    } else {
      port->Read(req.addr, req.bytes, req.scale_en);
      port->req_scale_group = req.scale_group;
    }
    inflight.push_back({pending, offset, skip, count}); ++pending->waiting;
    pending->issued_bytes += count;
    if (pending->issued_bytes == req.bytes) { pending->all_issued = true; q.pop_front(); }
    ++issue_count;
  }
  void Collect() {
    if (port->RspValid() && !inflight.empty()) {
      auto flight = inflight.front(); inflight.pop_front();
      auto pending = flight.owner; auto const& req = pending->req;
      auto data = port->RspData();
      pending->error |= port->RspError();
      if (data && req.lane_memory) {
        uint64_t row_scales = 128 / req.scale_group;
        uint64_t valid_scales = req.lane_bytes / req.scale_group;
        for (uint64_t lane = 0; lane < 64; ++lane) {
          for (uint64_t k = 0; k < req.lane_bytes; ++k) {
            uint64_t src = lane * 128 + flight.skip + k;
            if (src < data->size()) pending->data[lane * req.lane_bytes + k] = (*data)[src];
          }
          if (req.scale_en) for (uint64_t k = 0; k < valid_scales; ++k) {
            uint64_t src = 8192 + lane * row_scales + flight.skip / req.scale_group + k;
            if (src < data->size()) pending->data[req.bytes + lane * valid_scales + k] = (*data)[src];
          }
        }
      } else if (data) {
        for (uint64_t k = 0; k < flight.count; ++k)
          if (flight.skip + k < data->size()) pending->data[flight.offset + k] = (*data)[flight.skip + k];
        if (req.scale_en) {
          uint64_t body = req.pair ? 256 : req.bytes;
          for (uint64_t k = 0; k < flight.count / req.scale_group; ++k) {
            uint64_t src = body + flight.skip / req.scale_group + k;
            if (src < data->size()) pending->data[req.bytes + flight.offset / req.scale_group + k] = (*data)[src];
          }
        }
      }
      --pending->waiting; ++return_count;
    }
    if (!order.empty() && order.front()->all_issued && !order.front()->waiting) {
      auto pending = order.front(); order.pop_front();
      done.push_back({pending->req.tag, std::move(pending->data), pending->error, pending->req.bytes});
    }
  }
  uint64_t depth, capacity, max_outstanding = kMuRdOutstanding, reserved = 0;
  bool cancelling = false;
  std::shared_ptr<MemPort> port;
  std::deque<std::shared_ptr<Pending>> q, order;
  std::deque<Flight> inflight;
  std::deque<Rsp> done;
  uint64_t issue_count = 0, return_count = 0;
  Logic64 issued, returned;
};
}} // namespace latch::bach
#endif
