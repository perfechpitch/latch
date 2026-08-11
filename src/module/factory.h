#ifndef _LATCH_MODULE_FACTORY_
#define _LATCH_MODULE_FACTORY_

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/fifo.h"
#include "base/module.h"

namespace latch {

struct FacReq {
  uint64_t ticket = 0;
  uint64_t latency = 0;
  uint64_t addr = 0;
  uint8_t  isWrite = 0;
  uint32_t nbytes = 0;
};

inline void AppendLineReqs(std::vector<FacReq>& out, uint64_t addr, int64_t nbytes, bool isWrite,
                           uint32_t rows, int64_t rowStride, uint64_t lineBytes) {
  if (rows <= 1) { rows = 1; rowStride = 0; }
  for (uint32_t r = 0; r < rows; ++r) {
    uint64_t a0 = addr + (uint64_t)r * (uint64_t)rowStride;
    uint64_t end = a0 + (uint64_t)(nbytes > 0 ? nbytes : 1);
    for (uint64_t a = a0 & ~(lineBytes - 1); a < end; a += lineBytes) {
      uint64_t lo = a > a0 ? a : a0;
      uint64_t hi = a + lineBytes < end ? a + lineBytes : end;
      FacReq q;
      q.addr = lo;
      q.nbytes = (uint32_t)(hi - lo);
      q.isWrite = isWrite ? 1 : 0;
      out.push_back(q);
    }
  }
}

class Factory : public ClkModule {
 public:

  Factory(ClockPtr c, uint32_t bandwidth, uint64_t latency,
          const std::string& name, uint32_t nports = 1, uint64_t depth = 256,
          uint64_t adoptOwnerId = 0)
      : ClkModule(c), clk(c), bw(bandwidth), lat(latency) {
    for (uint32_t p = 0; p < nports; ++p) {
      reqFifo.push_back(std::make_shared<Fifo<FacReq>>(depth, c));
      respFifo.push_back(std::make_shared<Fifo<uint64_t>>(depth, c));
    }
    if (adoptOwnerId) obj_id = adoptOwnerId;
    else RegisterId(name, 0);
  }

  uint32_t Ports() const { return static_cast<uint32_t>(reqFifo.size()); }
  Fifo<FacReq>& Req(uint32_t port = 0) { return *reqFifo[port]; }
  Fifo<uint64_t>& Resp(uint32_t port = 0) { return *respFifo[port]; }

  void Cycle() override {
    DelayCycle(1);
    ++cyc;

    for (auto it = inflight.begin(); it != inflight.end();) {
      if (it->done <= cyc && !respFifo[it->port]->IsFull()) {
        respFifo[it->port]->Push(it->ticket);
        ++served;
        it = inflight.erase(it);
      } else {
        ++it;
      }
    }
    if (!inflight.empty()) ++busyCycles;

    while (inflight.size() < bw) {
      bool accepted = false;
      for (uint32_t p = 0; p < reqFifo.size() && inflight.size() < bw; ++p) {
        uint32_t port = (rr + p) % reqFifo.size();
        if (!reqFifo[port]->IsEmpty()) {
          FacReq r = reqFifo[port]->Front();
          reqFifo[port]->Pop();
          uint64_t useLat = r.latency ? r.latency : lat;
          inflight.push_back({port, r.ticket, cyc + useLat});
          accepted = true;
        }
      }
      if (!accepted) break;
      ++rr;
    }

    Trace("inflight", static_cast<uint64_t>(inflight.size()));
    Trace("served", served);
    Trace("busy", inflight.empty() ? 0u : 1u);
  }

  uint64_t Cycles() const { return cyc; }
  uint64_t Served() const { return served; }
  uint64_t BusyCycles() const { return busyCycles; }

 protected:

  std::vector<std::shared_ptr<Fifo<FacReq>>> reqFifo;
  std::vector<std::shared_ptr<Fifo<uint64_t>>> respFifo;

 private:
  struct InFlight {
    uint32_t port;
    uint64_t ticket;
    uint64_t done;
  };
  ClockPtr clk;
  uint32_t bw;
  uint64_t lat;
  std::deque<InFlight> inflight;
  uint32_t rr = 0;
  uint64_t cyc = 0;
  uint64_t served = 0;
  uint64_t busyCycles = 0;
};

}

#endif
