#ifndef _LATCH_BUS_
#define _LATCH_BUS_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/log.h"
#include "base/module.h"
#include "module/axi.h"

namespace latch {

class Bus : public ClkModule {
 public:
  Bus(ClockPtr clock, const AxiConfig& cfg = AxiConfig{},
      const std::string& name = "bus")
      : ClkModule(clock), cfg_(cfg), name_(name) {}

  std::shared_ptr<AxiSlave> AddSlave() {
    auto s = std::make_shared<AxiSlave>(
        clk, name_ + "_s" + std::to_string(ups_.size()), cfg_);
    ups_.push_back(Up{s, -1, false});
    LOGCHECK(ups_.size() < kMaxPort, "bus: too many slave ports");
    return s;
  }

  std::shared_ptr<AxiMaster> AddMaster(uint64_t base, uint64_t size) {
    auto m = std::make_shared<AxiMaster>(
        clk, name_ + "_m" + std::to_string(downs_.size()), cfg_);
    downs_.push_back(Down{m, base, size});
    return m;
  }

  std::shared_ptr<AxiSlave> GetSlavePort(uint32_t i) {
    LOGCHECK(i < ups_.size(), "bus: slave port index out of range");
    return ups_[i].axi;
  }
  std::shared_ptr<AxiMaster> GetMasterPort(uint32_t i) {
    LOGCHECK(i < downs_.size(), "bus: master port index out of range");
    return downs_[i].axi;
  }

  void Cycle() override {
    DelayCycle(1);
    for (uint32_t i = 0; i < ups_.size(); ++i) Forward(i);
    for (auto& d : downs_) Return(d);
  }

 private:
  static constexpr uint64_t kPortShift = 20;
  static constexpr uint64_t kIdMask = (uint64_t(1) << kPortShift) - 1;
  static constexpr uint64_t kMaxPort = uint64_t(1) << 12;

  struct Up {
    std::shared_ptr<AxiSlave> axi;
    int wDest;
    bool wActive;
  };
  struct Down {
    std::shared_ptr<AxiMaster> axi;
    uint64_t base;
    uint64_t size;
    int wOwner = -1;

  };

  int Route(uint64_t addr) const {
    for (uint32_t j = 0; j < downs_.size(); ++j)
      if (addr >= downs_[j].base && addr < downs_[j].base + downs_[j].size)
        return static_cast<int>(j);
    return -1;
  }
  uint64_t EncId(uint32_t up, uint64_t origId) const {
    return (uint64_t(up) << kPortShift) | (origId & kIdMask);
  }

  void Forward(uint32_t i) {
    Up& up = ups_[i];
    if (!up.axi->IsBound()) return;

    if (!up.axi->AR().IsEmpty()) {
      uint64_t addr = static_cast<uint64_t>(up.axi->AR().Front().addr);
      int j = Route(addr);
      LOGCHECK(j >= 0, "bus: AR address has no downstream mapping");
      if (!downs_[j].axi->AR().IsFull()) {
        auto c = up.axi->TryPopAR();
        downs_[j].axi->TryIssueRead(EncId(i, c->id), c->addr, c->len, c->size,
                                    c->burst);
      }
    }

    if (!up.wActive && !up.axi->AW().IsEmpty()) {
      uint64_t addr = static_cast<uint64_t>(up.axi->AW().Front().addr);
      int j = Route(addr);
      LOGCHECK(j >= 0, "bus: AW address has no downstream mapping");

      if (downs_[j].wOwner == -1 && !downs_[j].axi->AW().IsFull()) {
        auto c = up.axi->TryPopAW();
        downs_[j].axi->TryIssueWriteAddr(EncId(i, c->id), c->addr, c->len,
                                         c->size, c->burst);
        downs_[j].wOwner = static_cast<int>(i);
        up.wActive = true;
        up.wDest = j;
      }
    }

    if (up.wActive && !up.axi->W().IsEmpty() &&
        !downs_[up.wDest].axi->W().IsFull()) {
      auto wb = up.axi->TryPopW();
      downs_[up.wDest].axi->TryPushWBeat(wb->data, wb->last, wb->strb);
      if (wb->last) {
        downs_[up.wDest].wOwner = -1;
        up.wActive = false;
        up.wDest = -1;
      }
    }
  }

  void Return(Down& d) {
    if (!d.axi->R().IsEmpty()) {
      uint64_t encId = static_cast<uint64_t>(d.axi->R().Front().id);
      uint32_t up = static_cast<uint32_t>(encId >> kPortShift);
      LOGCHECK(up < ups_.size(), "bus: R id decodes to bad upstream port");
      if (!ups_[up].axi->R().IsFull()) {
        auto r = d.axi->TryPopR();
        ups_[up].axi->TryPushR(encId & kIdMask, r->data, r->last,
                               static_cast<AxiResp>(r->resp));
      }
    }
    if (!d.axi->B().IsEmpty()) {
      uint64_t encId = static_cast<uint64_t>(d.axi->B().Front().id);
      uint32_t up = static_cast<uint32_t>(encId >> kPortShift);
      LOGCHECK(up < ups_.size(), "bus: B id decodes to bad upstream port");
      if (!ups_[up].axi->B().IsFull()) {
        auto b = d.axi->TryPopB();
        ups_[up].axi->TryPushB(encId & kIdMask,
                               static_cast<AxiResp>(b->resp));
      }
    }
  }

  AxiConfig cfg_;
  std::string name_;
  std::vector<Up> ups_;
  std::vector<Down> downs_;
};

}
#endif
