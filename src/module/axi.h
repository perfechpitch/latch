#ifndef _LATCH_MODULE_AXI_
#define _LATCH_MODULE_AXI_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/fifo.h"
#include "base/log.h"
#include "base/logic.h"
#include "base/module.h"

namespace latch {

enum class AxiBurst : uint8_t {
  kFixed = 0,
  kIncr  = 1,
  kWrap  = 2,
};

enum class AxiResp : uint8_t {
  kOkay   = 0,
  kExOkay = 1,
  kSlvErr = 2,
  kDecErr = 3,
};

class AxiAddrPkt : public Logic {
 public:
  Logic64 id, addr, len, size, burst;
  explicit AxiAddrPkt(ClockPtr c)
      : id(c), addr(c), len(c), size(c), burst(c) {
    Fields(id, addr, len, size, burst);
  }
};

class AxiRPkt : public Logic {
 public:
  Logic64 id, resp, last;
  LogicPtr<std::vector<uint8_t>> data;
  explicit AxiRPkt(ClockPtr c) : id(c), resp(c), last(c), data(c) {
    Fields(id, resp, last, data);
  }
};

class AxiWPkt : public Logic {
 public:
  Logic64 last;
  LogicPtr<std::vector<uint8_t>> data;
  LogicPtr<std::vector<uint8_t>> strb;
  explicit AxiWPkt(ClockPtr c) : last(c), data(c), strb(c) {
    Fields(last, data, strb);
  }
};

class AxiBPkt : public Logic {
 public:
  Logic64 id, resp;
  explicit AxiBPkt(ClockPtr c) : id(c), resp(c) {
    Fields(id, resp);
  }
};

struct AxiConfig {

  uint64_t data_bytes = 16;
  uint64_t ar_depth = 4;
  uint64_t r_depth  = 8;
  uint64_t aw_depth = 4;
  uint64_t w_depth  = 8;
  uint64_t b_depth  = 4;
};

class AxiMaster;
class AxiSlave;

class AxiBase : public ClkModule {
 public:
  using AddrPkt = AxiAddrPkt;
  using RPkt    = AxiRPkt;
  using WPkt    = AxiWPkt;
  using BPkt    = AxiBPkt;
  using Config  = AxiConfig;

  AxiBase(ClockPtr clock, const Config& c, bool tick = true) : ClkModule(clock, tick), cfg(c) {
    LOGCHECK(IsPow2(cfg.data_bytes), "AxiConfig.data_bytes must be pow2");
    LOGCHECK(cfg.data_bytes >= 1 && cfg.data_bytes <= 128,
             "AxiConfig.data_bytes out of range (1..128)");
  }

  Fifo<AddrPkt>& AR() {
    LOGCHECK(ar_fifo != nullptr, "AR channel not bound; call AxiMaster::Bind first");
    return *ar_fifo;
  }
  Fifo<RPkt>& R() {
    LOGCHECK(r_fifo != nullptr, "R channel not bound; call AxiMaster::Bind first");
    return *r_fifo;
  }
  Fifo<AddrPkt>& AW() {
    LOGCHECK(aw_fifo != nullptr, "AW channel not bound; call AxiMaster::Bind first");
    return *aw_fifo;
  }
  Fifo<WPkt>& W() {
    LOGCHECK(w_fifo != nullptr, "W channel not bound; call AxiMaster::Bind first");
    return *w_fifo;
  }
  Fifo<BPkt>& B() {
    LOGCHECK(b_fifo != nullptr, "B channel not bound; call AxiMaster::Bind first");
    return *b_fifo;
  }

  const Config& Cfg() const { return cfg; }

  bool IsBound() const { return ar_fifo != nullptr; }

  void Cycle() override {}

  friend class AxiMaster;

 protected:
  static bool IsPow2(uint64_t v) { return v && ((v & (v - 1)) == 0); }

  void AllocateChannels() {
    LOGCHECK(!ar_fifo && !r_fifo && !aw_fifo && !w_fifo && !b_fifo,
             "AxiBase::AllocateChannels: already allocated");
    ar_fifo = std::make_shared<Fifo<AddrPkt>>(cfg.ar_depth, clk);
    r_fifo  = std::make_shared<Fifo<RPkt>>   (cfg.r_depth,  clk);
    aw_fifo = std::make_shared<Fifo<AddrPkt>>(cfg.aw_depth, clk);
    w_fifo  = std::make_shared<Fifo<WPkt>>   (cfg.w_depth,  clk);
    b_fifo  = std::make_shared<Fifo<BPkt>>   (cfg.b_depth,  clk);
  }
  void AdoptChannelsFrom(AxiBase& src) {
    LOGCHECK(!ar_fifo && !r_fifo && !aw_fifo && !w_fifo && !b_fifo,
             "AdoptChannelsFrom: target already bound");
    LOGCHECK(src.ar_fifo && src.r_fifo && src.aw_fifo && src.w_fifo && src.b_fifo,
             "AdoptChannelsFrom: source has not allocated channels");
    LOGCHECK(src.clk == clk, "AXI bind: clock domain mismatch");
    LOGCHECK(src.cfg.data_bytes == cfg.data_bytes,
             "AXI bind: data_bytes mismatch");
    ar_fifo = src.ar_fifo;
    r_fifo  = src.r_fifo;
    aw_fifo = src.aw_fifo;
    w_fifo  = src.w_fifo;
    b_fifo  = src.b_fifo;
  }

  Config cfg;
  std::shared_ptr<Fifo<AddrPkt>> ar_fifo, aw_fifo;
  std::shared_ptr<Fifo<RPkt>>    r_fifo;
  std::shared_ptr<Fifo<WPkt>>    w_fifo;
  std::shared_ptr<Fifo<BPkt>>    b_fifo;
};

class AxiMaster : public AxiBase {
 public:

  AxiMaster(ClockPtr clock, const std::string& name, const Config& c, bool tick = true)
      : AxiBase(clock, c, tick) {
    RegisterId(name, 0);
    AllocateChannels();
  }

  void Bind(AxiSlave& slave);
  void Bind(std::shared_ptr<AxiSlave> slave);

  bool TryIssueRead(uint64_t id, uint64_t addr, uint8_t len, uint8_t size,
                    AxiBurst burst = AxiBurst::kIncr) {
    if (AR().IsFull()) return false;
    AddrPkt p(clk);
    p.id    = id;
    p.addr  = addr;
    p.len   = static_cast<uint64_t>(len);
    p.size  = static_cast<uint64_t>(size);
    p.burst = static_cast<uint64_t>(static_cast<uint8_t>(burst));
    AR().Push(p);
    return true;
  }

  bool TryIssueWriteAddr(uint64_t id, uint64_t addr, uint8_t len,
                         uint8_t size,
                         AxiBurst burst = AxiBurst::kIncr) {
    if (AW().IsFull()) return false;
    AddrPkt p(clk);
    p.id    = id;
    p.addr  = addr;
    p.len   = static_cast<uint64_t>(len);
    p.size  = static_cast<uint64_t>(size);
    p.burst = static_cast<uint64_t>(static_cast<uint8_t>(burst));
    AW().Push(p);
    return true;
  }

  bool TryPushWBeat(std::shared_ptr<std::vector<uint8_t>> data,
                    bool last,
                    std::shared_ptr<std::vector<uint8_t>> strb = nullptr) {
    if (W().IsFull()) return false;
    LOGCHECK(data && data->size() == cfg.data_bytes,
             "TryPushWBeat: data size must equal data_bytes");
    if (strb) {
      LOGCHECK(strb->size() == cfg.data_bytes,
               "TryPushWBeat: strb size must equal data_bytes");
    }
    WPkt p(clk);
    p.last = last ? 1u : 0u;
    p.data = data;
    p.strb = strb;
    W().Push(p);
    return true;
  }

  struct ReadBeat {
    uint64_t id;
    uint64_t resp;
    bool     last;
    std::shared_ptr<std::vector<uint8_t>> data;
  };
  std::optional<ReadBeat> TryPopR() {
    if (R().IsEmpty()) return std::nullopt;
    auto& h = R().Front();
    ReadBeat b{
        static_cast<uint64_t>(h.id),
        static_cast<uint64_t>(h.resp),
        static_cast<uint64_t>(h.last) != 0,
        h.data.Get(),
    };
    R().Pop();
    return b;
  }

  struct WriteResp {
    uint64_t id;
    uint64_t resp;
  };
  std::optional<WriteResp> TryPopB() {
    if (B().IsEmpty()) return std::nullopt;
    auto& h = B().Front();
    WriteResp b{static_cast<uint64_t>(h.id),
                static_cast<uint64_t>(h.resp)};
    B().Pop();
    return b;
  }
};

class AxiSlave : public AxiBase {
 public:

  AxiSlave(ClockPtr clock, const std::string& name, const Config& c, bool tick = true)
      : AxiBase(clock, c, tick) {
    RegisterId(name, 0);

  }

  struct AddrCmd {
    uint64_t id;
    uint64_t addr;
    uint8_t  len;
    uint8_t  size;
    AxiBurst burst;
  };
  std::optional<AddrCmd> TryPopAR() { return PopAddr(AR()); }
  std::optional<AddrCmd> TryPopAW() { return PopAddr(AW()); }

  struct WriteBeat {
    bool last;
    std::shared_ptr<std::vector<uint8_t>> data;
    std::shared_ptr<std::vector<uint8_t>> strb;
  };
  std::optional<WriteBeat> TryPopW() {
    if (W().IsEmpty()) return std::nullopt;
    auto& h = W().Front();
    WriteBeat b{static_cast<uint64_t>(h.last) != 0,
                h.data.Get(),
                h.strb.Get()};
    W().Pop();
    return b;
  }

  bool TryPushR(uint64_t id,
                std::shared_ptr<std::vector<uint8_t>> data,
                bool last,
                AxiResp resp = AxiResp::kOkay) {
    if (R().IsFull()) return false;
    LOGCHECK(data && data->size() == cfg.data_bytes,
             "TryPushR: data size must equal data_bytes");
    RPkt p(clk);
    p.id   = id;
    p.resp = static_cast<uint64_t>(static_cast<uint8_t>(resp));
    p.last = last ? 1u : 0u;
    p.data = data;
    R().Push(p);
    return true;
  }

  bool TryPushB(uint64_t id, AxiResp resp = AxiResp::kOkay) {
    if (B().IsFull()) return false;
    BPkt p(clk);
    p.id   = id;
    p.resp = static_cast<uint64_t>(static_cast<uint8_t>(resp));
    B().Push(p);
    return true;
  }

 private:
  std::optional<AddrCmd> PopAddr(Fifo<AddrPkt>& ch) {
    if (ch.IsEmpty()) return std::nullopt;
    auto& h = ch.Front();
    AddrCmd c{
        static_cast<uint64_t>(h.id),
        static_cast<uint64_t>(h.addr),
        static_cast<uint8_t>(static_cast<uint64_t>(h.len)),
        static_cast<uint8_t>(static_cast<uint64_t>(h.size)),
        static_cast<AxiBurst>(static_cast<uint64_t>(h.burst) & 0x3),
    };
    ch.Pop();
    return c;
  }
};

inline void AxiMaster::Bind(AxiSlave& slave) {
  slave.AdoptChannelsFrom(*this);
}
inline void AxiMaster::Bind(std::shared_ptr<AxiSlave> slave) {
  LOGCHECK(slave != nullptr, "AxiMaster::Bind: slave is null");
  slave->AdoptChannelsFrom(*this);
}

}

#endif
