#ifndef _LATCH_MODULE_LLC_
#define _LATCH_MODULE_LLC_

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/clock.h"
#include "base/fifo.h"
#include "base/log.h"
#include "base/logic.h"
#include "base/module.h"
#include "base/runtime.h"

#include "module/axi.h"

namespace latch {

class Llc : public ClkModule {
 public:
  enum class AttrMode : uint8_t { WBWA = 0, Bypass = 1 };

  struct RegionAttr {
    bool valid = false;
    uint64_t base = 0;
    uint64_t mask = 0;
    AttrMode mode = AttrMode::WBWA;
    bool prefetch_friendly = false;
    uint64_t pin_way_mask = 0;
  };

  enum StatId : uint32_t {
    kStatMaintBusy = 0,
    kStatMaintOpCompleted = 1,
    kStatMissCount = 2,
    kStatHitCount = 3,
    kStatWritebackCount = 4,
    kStatBypassCount = 5,
  };

  enum CtrlOp : uint8_t {
    kCtrlSetRegion  = 0x0,
    kCtrlInvalidate = 0x2,
    kCtrlClean      = 0x3,
    kCtrlFlush      = 0x4,
    kCtrlPrefetch   = 0x5,
  };
  enum MaintOpCode : uint8_t {
    kOpInvalidate = kCtrlInvalidate,
    kOpClean      = kCtrlClean,
    kOpFlush      = kCtrlFlush,
    kOpPrefetch   = kCtrlPrefetch,
  };

  struct Config {
    uint64_t n_llc_bytes = 8 * 1024;
    uint64_t line_bytes  = 64;
    uint64_t n_way       = 8;
    uint64_t n_master    = 4;
    uint64_t n_region    = 8;

    uint64_t n_mshr      = 16;
    uint64_t n_outstanding_dram = 16;
    uint64_t q_maint     = 4;
    uint64_t ctrl_fifo_depth = 4;
    uint64_t hit_latency_cyc = 0;

    uint64_t n_bank = 0;

    uint64_t master_req_cap = 0;

    uint64_t n_outstanding_dram_wr = 0;

    bool tag_port_conflict = false;

    uint64_t frontend_pipe_stages = 0;

    bool allow_concurrent_set_fills = false;
    bool write_through = false;

    AxiConfig axi = MakeDefaultAxi();
  };

  static AxiConfig MakeDefaultAxi() {
    AxiConfig c;
    c.data_bytes = 16;
    c.ar_depth = 4;
    c.r_depth  = 16;
    c.aw_depth = 4;
    c.w_depth  = 16;
    c.b_depth  = 4;
    return c;
  }

  class CtrlReqPkt : public Logic {
   public:
    Logic64 op, idx, base, length, region_mask, attr_mode, attr_pin_mask;
    explicit CtrlReqPkt(ClockPtr c)
        : op(c), idx(c), base(c), length(c),
          region_mask(c), attr_mode(c), attr_pin_mask(c) {
      Fields(op, idx, base, length, region_mask, attr_mode, attr_pin_mask);
    }
  };

  Llc(ClockPtr clock, const std::string& name, const Config& c,
      bool v = false, uint64_t adoptOwnerId = 0)
      : ClkModule(clock),
        cfg(c),
        verbose(v),
        hit_event(clock),
        miss_event(clock),
        writeback_event(clock),
        bypass_event(clock),
        maint_op_event(clock),
        hit_count(clock),
        miss_count(clock),
        fill_count(clock),
        mshr_merge_count(clock),
        writeback_count(clock),
        bypass_count(clock),
        wt_count(clock), wt_inval_count(clock), wt_kill_count(clock),
        maint_op_completed(clock),
        mshr_occ(clock),
        dram_out_count(clock),
        maint_busy_latched(clock) {
    if (adoptOwnerId) obj_id = adoptOwnerId;
    else RegisterId(name, 0);

    LOGCHECK(IsPow2(cfg.line_bytes), "line_bytes must be pow2");
    LOGCHECK(IsPow2(cfg.n_way), "n_way must be pow2");
    LOGCHECK(IsPow2(cfg.n_llc_bytes), "n_llc_bytes must be pow2");
    n_set = cfg.n_llc_bytes / (cfg.line_bytes * cfg.n_way);
    LOGCHECK(IsPow2(n_set), "n_set derived non-pow2");
    LOGCHECK(cfg.n_way <= 64, "n_way>64 not supported (mask is uint64_t)");

    log2_line = Log2(cfg.line_bytes);
    log2_set  = Log2(n_set);
    log2_way  = Log2(cfg.n_way);
    full_way_mask = (cfg.n_way == 64) ? ~0ULL : ((1ULL << cfg.n_way) - 1);

    LOGCHECK(cfg.line_bytes % cfg.axi.data_bytes == 0,
             "line_bytes must be a multiple of axi.data_bytes");
    beats_per_line = cfg.line_bytes / cfg.axi.data_bytes;
    burst_len = static_cast<uint8_t>(beats_per_line - 1);
    burst_size = Log2Bytes(cfg.axi.data_bytes);

    tags.assign(n_set * cfg.n_way, TagEntry{});
    plru.assign(n_set * (cfg.n_way - 1), 0);

    regions.assign(cfg.n_region, RegionAttr{});
    default_attr.valid = true;
    default_attr.mode = AttrMode::WBWA;
    default_attr.pin_way_mask = full_way_mask;

    mshr.assign(cfg.n_mshr, MshrEntry{});
    dram_out.assign(cfg.n_outstanding_dram == 0
                        ? 0
                        : cfg.n_outstanding_dram + cfg.n_outstanding_dram_wr,
                    DramOutstanding{});

    master_bridges.reserve(cfg.n_master);
    for (uint64_t m = 0; m < cfg.n_master; ++m) {
      MasterBridge br;
      br.axi = std::make_shared<AxiSlave>(
          clock, name + "_axi_s" + std::to_string(m), cfg.axi, false);
      master_bridges.push_back(std::move(br));
    }
    dram_bridge.axi = std::make_shared<AxiMaster>(
        clock, name + "_axi_m_dram", cfg.axi, false);

    ctrl_fifo = std::make_shared<Fifo<CtrlReqPkt>>(
        cfg.ctrl_fifo_depth, clock);

    fe_.resize(cfg.frontend_pipe_stages);
    feInFlight_.assign(cfg.n_master, 0);
  }

  std::shared_ptr<AxiSlave>  AxiSlavePort(uint64_t m) {
    return master_bridges.at(m).axi;
  }
  std::shared_ptr<AxiMaster> AxiMasterPort() { return dram_bridge.axi; }
  Fifo<CtrlReqPkt>&          CtrlFifo() { return *ctrl_fifo; }

  void SetRegion(uint32_t idx, uint64_t base, uint64_t mask, AttrMode mode,
                 uint64_t pin_way_mask = 0,
                 bool  = false) {
    LOGCHECK(idx < cfg.n_region, "SetRegion: idx OOB");
    CtrlReqPkt p(clk);
    p.op           = uint64_t{kCtrlSetRegion};
    p.idx          = uint64_t{idx};
    p.base         = base;
    p.region_mask  = mask;
    p.attr_mode    = uint64_t(static_cast<uint8_t>(mode));
    p.attr_pin_mask = pin_way_mask;
    PushCtrlAndSettle(p);
  }
  void Invalidate(uint64_t base, uint64_t length) {
    PushMaintViaFifo(kCtrlInvalidate, base, length);
  }
  void Clean(uint64_t base, uint64_t length) {
    PushMaintViaFifo(kCtrlClean, base, length);
  }
  void Flush(uint64_t base, uint64_t length) {
    PushMaintViaFifo(kCtrlFlush, base, length);
  }
  void Prefetch(uint64_t base, uint64_t length) {
    PushMaintViaFifo(kCtrlPrefetch, base, length);
  }
  uint64_t ReadStat(uint32_t stat_id) const {
    switch (stat_id) {
      case kStatMaintBusy:           return MaintBusy() ? 1u : 0u;
      case kStatMaintOpCompleted:    return maint_op_completed.Get();
      case kStatMissCount:           return miss_count.Get();
      case kStatHitCount:            return hit_count.Get();
      case kStatWritebackCount:      return writeback_count.Get();
      case kStatBypassCount:         return bypass_count.Get();
      default:                       return 0;
    }
  }

  uint64_t HitEvent()         const { return hit_event.Get(); }
  uint64_t MissEvent()        const { return miss_event.Get(); }
  uint64_t WritebackEvent()   const { return writeback_event.Get(); }
  uint64_t BypassEvent()      const { return bypass_event.Get(); }
  uint64_t MaintOpEvent()     const { return maint_op_event.Get(); }
  uint64_t HitCount()         const { return hit_count.Get(); }
  uint64_t MissCount()        const { return miss_count.Get(); }

  uint64_t FillCount()        const { return fill_count.Get(); }
  uint64_t MshrMergeCount()   const { return mshr_merge_count.Get(); }
  uint64_t WritebackCount()   const { return writeback_count.Get(); }
  uint64_t BypassCount()      const { return bypass_count.Get(); }

  uint64_t TagPortStallCount() const { return tag_port_stall_total; }

  uint64_t SetConflictCount()  const { return set_conflict_total; }
  uint64_t MshrFullCount()     const { return mshr_full_total; }
  uint64_t DramOutFullCount()  const { return dram_out_full_total; }

  uint64_t MshrPeak()    const { return mshrPeak_; }
  uint64_t DramOutPeak() const { return dramOutPeak_; }
  uint64_t WriteThroughCount() const { return wt_count.Get(); }
  uint64_t WtInvalCount()      const { return wt_inval_count.Get(); }
  uint64_t WtKillCount()       const { return wt_kill_count.Get(); }
  uint64_t MaintCompleted()   const { return maint_op_completed.Get(); }
  uint64_t MshrOccupancy()        const { return mshr_occ.Get(); }
  uint64_t DramOutstandingCount() const { return dram_out_count.Get(); }
  bool MaintBusy() const {
    if (maint_busy_latched.Get() != 0) return true;
    if (!ctrl_fifo->IsEmpty()) {
      uint64_t op = uint64_t(ctrl_fifo->Front().op);
      if (op == kCtrlInvalidate || op == kCtrlClean ||
          op == kCtrlFlush      || op == kCtrlPrefetch) return true;
    }
    return false;
  }
  uint64_t NSet() const { return n_set; }
  uint64_t BeatsPerLine() const { return beats_per_line; }
  uint8_t  BurstLen() const { return burst_len; }
  uint8_t  BurstSize() const { return burst_size; }

  void Cycle() override {
    DelayCycle(1);
    ++tick_;

    hit_this_cycle             = 0;
    miss_this_cycle            = 0;
    writeback_this_cycle       = 0;
    bypass_this_cycle          = 0;
    wt_this_cycle = wt_inval_this_cycle = wt_kill_this_cycle = 0;
    maint_op_this_cycle        = 0;
    fillBankBusy_.clear();

    for (uint64_t m = 0; m < cfg.n_master; ++m) {
      if (master_bridges[m].axi->IsBound()) AssembleMasterReqs(m);
    }

    if (dram_bridge.axi->IsBound()) AssembleDramRBeats();
    HandleOneDramResp();
    CompleteOneFilledMshr();
    if (dram_bridge.axi->IsBound()) {
      IssueOneDramReq();
      DrainDramOutboundW();
    }
    ProcessCtrlReq();
    ProcessMasterRequests();

    WalkerStep();
    for (uint64_t m = 0; m < cfg.n_master; ++m) {
      if (!master_bridges[m].axi->IsBound()) continue;
      DrainDeferredResp(m);
      EmitMasterR(m);
      EmitMasterB(m);
    }
    UpdateMaintBusyPending();

    hit_event           = hit_this_cycle;
    miss_event          = miss_this_cycle;
    writeback_event     = writeback_this_cycle;
    bypass_event        = bypass_this_cycle;
    maint_op_event      = maint_op_this_cycle;

    hit_total          += hit_this_cycle;
    miss_total         += miss_this_cycle;
    writeback_total    += writeback_this_cycle;
    bypass_total       += bypass_this_cycle;
    wt_total           += wt_this_cycle;
    wt_inval_total     += wt_inval_this_cycle;
    wt_kill_total      += wt_kill_this_cycle;
    maint_op_total     += maint_op_this_cycle;
    hit_count           = hit_total;
    miss_count          = miss_total;
    fill_count          = fill_total;
    mshr_merge_count    = merge_total;
    writeback_count     = writeback_total;
    bypass_count        = bypass_total;
    wt_count            = wt_total;
    wt_inval_count      = wt_inval_total;
    wt_kill_count       = wt_kill_total;
    maint_op_completed  = maint_op_total;
    mshr_occ            = mshr_occ_pending;
    dram_out_count      = dram_out_pending;
    maint_busy_latched  = maint_busy_pending ? 1u : 0u;

    EmitTraces();
  }

 private:
  struct TagEntry {
    bool valid = false;
    bool dirty = false;
    bool lock = false;
    uint64_t tag = 0;
    std::vector<uint8_t> data;
  };

  struct PendingReq {
    uint64_t master;
    uint64_t req_id;
    uint64_t addr;
    bool is_write;
    uint64_t nbytes = 0;
    std::shared_ptr<std::vector<uint8_t>> wr_data;
  };

  struct MshrEntry {
    bool valid = false;
    bool fill_done = false;
    bool array_written = false;
    uint64_t line_addr = 0;
    uint64_t set = 0;
    uint64_t way = 0;
    uint64_t pin_mask = 0;
    uint64_t dram_id = 0;
    std::vector<uint8_t> fill_data;
    std::deque<PendingReq> pending;
    bool victim_was_dirty = false;
    bool killed = false;
  };

  enum class DramKind : uint8_t {
    kFill, kWriteback, kMaintWb, kBypassRead, kBypassWrite,
  };

  struct DramOutstanding {
    bool valid = false;
    DramKind kind;
    uint64_t mshr_idx = 0;
    uint64_t bypass_master = 0;
    uint64_t bypass_req_id = 0;
    uint64_t bypass_addr = 0;
    uint64_t bypass_nbytes = 0;
  };

  struct DramReqInternal {
    bool is_write;
    uint64_t addr;
    uint64_t id;
    std::shared_ptr<std::vector<uint8_t>> data;
  };

  struct MaintOpRec {
    uint8_t op;
    uint64_t base;
    uint64_t length;
    uint64_t cursor;
  };

  struct DeferredWb {
    uint64_t addr;
    std::shared_ptr<std::vector<uint8_t>> data;
  };

  struct MasterBridge {
    std::shared_ptr<AxiSlave> axi;

    struct WriteAsm {
      uint64_t axi_id = 0;
      uint64_t addr = 0;
      std::shared_ptr<std::vector<uint8_t>> buf;
      uint64_t beats_so_far = 0;
    };
    std::deque<WriteAsm> write_asm;

    struct LineReq {
      bool is_write = false;
      uint64_t addr = 0;
      uint64_t axi_id = 0;
      uint64_t nbytes = 0;
      std::shared_ptr<std::vector<uint8_t>> data;
    };
    std::deque<LineReq> req_q;

    struct ReadBurstOut {
      uint64_t axi_id = 0;
      std::shared_ptr<std::vector<uint8_t>> line;
      uint64_t beats_emitted = 0;
      uint64_t beat0 = 0;
      uint64_t nbeats = 0;
    };
    std::deque<ReadBurstOut> r_out;
    std::deque<uint64_t>     b_out;

    struct DeferredR {
      uint64_t axi_id = 0;
      std::shared_ptr<std::vector<uint8_t>> line;
      uint64_t ready_at = 0;
      uint64_t beat0 = 0;
      uint64_t nbeats = 0;
    };
    struct DeferredB {
      uint64_t req_id = 0;
      uint64_t ready_at = 0;
    };
    std::deque<DeferredR> deferred_r;
    std::deque<DeferredB> deferred_b;
  };

  struct DramBridge {
    std::shared_ptr<AxiMaster> axi;

    struct WriteOut {
      uint64_t axi_id = 0;
      std::shared_ptr<std::vector<uint8_t>> line;
      uint64_t beats_pushed = 0;
    };
    std::deque<WriteOut> w_out;

    std::unordered_map<uint64_t, std::shared_ptr<std::vector<uint8_t>>> r_asm;
    std::unordered_map<uint64_t, uint64_t> r_asm_beats;

    struct ReadDone {
      uint64_t axi_id = 0;
      std::shared_ptr<std::vector<uint8_t>> line;
    };
    std::deque<ReadDone> r_done;
  };

  static bool IsPow2(uint64_t x) { return x && ((x & (x - 1)) == 0); }
  static uint32_t Log2(uint64_t x) {
    uint32_t l = 0;
    while ((1ULL << l) < x) ++l;
    return l;
  }
  static uint8_t Log2Bytes(uint64_t b) {
    uint8_t s = 0;
    while ((uint64_t(1) << s) < b) ++s;
    return s;
  }
  uint64_t LineAddr(uint64_t a) const { return a & ~(cfg.line_bytes - 1); }

  void ReadBeatsOf(uint64_t addr, uint64_t nbytes, uint64_t& beat0, uint64_t& nbeats) const {
    uint64_t off = addr & (cfg.line_bytes - 1);
    if (nbytes == 0 || off + nbytes > cfg.line_bytes) nbytes = cfg.line_bytes - off;
    beat0 = off / cfg.axi.data_bytes;
    nbeats = (off + nbytes + cfg.axi.data_bytes - 1) / cfg.axi.data_bytes - beat0;
  }
  uint64_t SetIdx (uint64_t a) const { return (a >> log2_line) & (n_set - 1); }

  uint64_t BankOf(uint64_t set) const { return cfg.n_bank > 0 ? (set % cfg.n_bank) : set; }
  uint64_t TagBits(uint64_t a) const { return a >> (log2_line + log2_set); }
  TagEntry&       TagAt(uint64_t s, uint64_t w)       { return tags[s * cfg.n_way + w]; }
  TagEntry const& TagAt(uint64_t s, uint64_t w) const { return tags[s * cfg.n_way + w]; }
  uint8_t& PlruBit(uint64_t s, uint32_t b)            { return plru[s * (cfg.n_way - 1) + b]; }

  void PushCtrlAndSettle(const CtrlReqPkt& p) {
    while (ctrl_fifo->IsFull()) clk->DelayCycle(1);
    ctrl_fifo->Push(p);
  }
  void PushMaintViaFifo(uint8_t op, uint64_t base, uint64_t length) {
    CtrlReqPkt p(clk);
    p.op     = uint64_t{op};
    p.base   = base;
    p.length = length;
    PushCtrlAndSettle(p);
  }

  RegionAttr LookupRegion(uint64_t addr) const {
    for (int i = static_cast<int>(cfg.n_region) - 1; i >= 0; --i) {
      auto const& r = regions[i];
      if (!r.valid) continue;
      if (((addr ^ r.base) & r.mask) == 0) return r;
    }
    return default_attr;
  }

  void ProcessCtrlReq() {
    if (ctrl_fifo->IsEmpty()) return;
    auto& head = ctrl_fifo->Front();
    uint64_t op = head.op;
    if (op == kCtrlSetRegion) {
      uint64_t idx = head.idx;
      LOGCHECK(idx < cfg.n_region, "ctrl SET_REGION: idx OOB");
      auto& r = regions[idx];
      r.valid = true;
      r.base  = uint64_t(head.base) & ~(cfg.line_bytes - 1);
      r.mask  = head.region_mask;
      r.mode  = static_cast<AttrMode>(uint64_t(head.attr_mode) & 0x1);
      uint64_t pin = head.attr_pin_mask;
      r.pin_way_mask = pin ? (pin & full_way_mask) : full_way_mask;
      r.prefetch_friendly = false;
      ctrl_fifo->Pop();
      if (verbose) {
        RT::LogWithTime("[{}] SET_REGION idx={} base=0x{:x} mask=0x{:x}",
                        Name(), idx, r.base, r.mask);
      }
    } else if (op == kCtrlInvalidate || op == kCtrlClean ||
               op == kCtrlFlush      || op == kCtrlPrefetch) {
      if (maint_q.size() >= cfg.q_maint) return;
      MaintOpRec r;
      r.op = static_cast<uint8_t>(op);
      r.base = uint64_t(head.base) & ~(cfg.line_bytes - 1);
      uint64_t end = uint64_t(head.base) + uint64_t(head.length);
      r.length = ((end + cfg.line_bytes - 1) & ~(cfg.line_bytes - 1)) - r.base;
      r.cursor = 0;
      maint_q.push_back(r);
      ctrl_fifo->Pop();
      if (verbose) {
        RT::LogWithTime("[{}] Maint enqueued op=0x{:x} base=0x{:x} len=0x{:x}",
                        Name(), r.op, r.base, r.length);
      }
    } else {
      LOGCHECK(false, "ctrl: unknown opcode");
    }
  }

  void UpdateMaintBusyPending() {
    bool busy = !maint_q.empty() || maint_busy ||
                !maint_wb_dram_q.empty() || !maint_wb_pending.empty();
    if (!busy && !ctrl_fifo->IsEmpty()) {
      uint64_t op = ctrl_fifo->Front().op;
      if (op == kCtrlInvalidate || op == kCtrlClean ||
          op == kCtrlFlush      || op == kCtrlPrefetch) {
        busy = true;
      }
    }
    maint_busy_pending = busy;
  }

  void UpdatePlruAccess(uint64_t set, uint64_t way) {
    uint64_t pos = 0;
    for (uint32_t lvl = 0; lvl < log2_way; ++lvl) {
      uint32_t idx = (1u << lvl) - 1u + static_cast<uint32_t>(pos);
      uint32_t dir = (way >> (log2_way - 1u - lvl)) & 1u;
      PlruBit(set, idx) = static_cast<uint8_t>(dir ^ 1u);
      pos = pos * 2 + dir;
    }
  }
  uint64_t PlruVictim(uint64_t set) const {
    uint64_t pos = 0, way = 0;
    for (uint32_t lvl = 0; lvl < log2_way; ++lvl) {
      uint32_t idx = (1u << lvl) - 1u + static_cast<uint32_t>(pos);
      uint8_t dir = plru[set * (cfg.n_way - 1) + idx];
      way = (way << 1) | dir;
      pos = pos * 2 + dir;
    }
    return way;
  }
  uint64_t TagLookup(uint64_t set, uint64_t tag) const {
    for (uint64_t w = 0; w < cfg.n_way; ++w) {
      auto const& e = TagAt(set, w);
      if (e.valid && e.tag == tag) return w;
    }
    return cfg.n_way;
  }
  uint64_t ChooseVictim(uint64_t set, uint64_t pin_mask) const {
    if (pin_mask == 0) pin_mask = full_way_mask;

    const bool blk = !cfg.allow_concurrent_set_fills;
    for (uint64_t w = 0; w < cfg.n_way; ++w) {
      if (!(pin_mask & (1ULL << w))) continue;
      auto const& e = TagAt(set, w);
      if (!e.valid && !(blk && e.lock)) return w;
    }
    uint64_t v = PlruVictim(set);
    if ((pin_mask & (1ULL << v)) && !(blk && TagAt(set, v).lock)) return v;
    for (uint64_t w = 0; w < cfg.n_way; ++w) {
      if (!(pin_mask & (1ULL << w))) continue;
      if (!(blk && TagAt(set, w).lock)) return w;
    }
    return cfg.n_way;
  }

  int AllocDramSlot(bool is_write = false) {

    if (cfg.n_outstanding_dram == 0) {
      for (size_t i = 0; i < dram_out.size(); ++i)
        if (!dram_out[i].valid) return static_cast<int>(i);
      dram_out.push_back(DramOutstanding{});
      if (dram_out.size() > dramOutPeak_) dramOutPeak_ = dram_out.size();
      return static_cast<int>(dram_out.size() - 1);
    }
    size_t lo = 0, hi = dram_out.size();
    if (cfg.n_outstanding_dram_wr > 0) {
      if (is_write) lo = cfg.n_outstanding_dram;
      else          hi = cfg.n_outstanding_dram;
    }
    for (size_t i = lo; i < hi; ++i) {
      if (!dram_out[i].valid) return static_cast<int>(i);
    }
    return -1;
  }
  int FindMshrFreeSlot() {
    for (size_t i = 0; i < mshr.size(); ++i) {
      if (!mshr[i].valid) return static_cast<int>(i);
    }
    if (cfg.n_mshr != 0) return -1;
    mshr.push_back(MshrEntry{});
    if (mshr.size() > mshrPeak_) mshrPeak_ = mshr.size();
    return static_cast<int>(mshr.size() - 1);
  }
  void SetDramSlotValid(uint64_t i, bool v) {
    bool prev = dram_out[i].valid;
    dram_out[i].valid = v;
    if (v && !prev) ++dram_out_pending;
    else if (!v && prev) --dram_out_pending;
  }
  void SetMshrValid(uint64_t i, bool v) {
    bool prev = mshr[i].valid;
    mshr[i].valid = v;
    if (v && !prev) ++mshr_occ_pending;
    else if (!v && prev) --mshr_occ_pending;
  }
  int FindMshrByAddr(uint64_t line_addr) {
    for (size_t i = 0; i < mshr.size(); ++i) {
      if (mshr[i].valid && mshr[i].line_addr == line_addr)
        return static_cast<int>(i);
    }
    return -1;
  }

  void AssembleMasterReqs(uint64_t m) {
    auto& br = master_bridges[m];
    auto& ax = *br.axi;

    bool underCap = cfg.master_req_cap == 0 ||
                    br.req_q.size() + br.write_asm.size() + feInFlight_[m]
                        < cfg.master_req_cap;

    if (underCap && !ax.AR().IsEmpty()) {
      auto cmd = ax.TryPopAR();
      LOGCHECK(cmd.has_value(), "AssembleMasterReqs: AR pop failed");

      uint64_t bb = BurstBytes(cmd->len, cmd->size);
      LOGCHECK(bb <= cfg.line_bytes &&
               (cmd->addr & (cfg.line_bytes - 1)) + bb <= cfg.line_bytes,
               "Llc AR burst must fit within one line");
      MasterBridge::LineReq r;
      r.is_write = false;
      r.addr     = cmd->addr;
      r.axi_id   = cmd->id;
      r.nbytes   = bb;
      br.req_q.push_back(std::move(r));
    }

    if (underCap && !ax.AW().IsEmpty()) {
      auto cmd = ax.TryPopAW();
      LOGCHECK(cmd.has_value(), "AssembleMasterReqs: AW pop failed");
      LOGCHECK(BurstBytes(cmd->len, cmd->size) == cfg.line_bytes,
               "Llc AW burst must equal line_bytes");
      MasterBridge::WriteAsm wa;
      wa.axi_id = cmd->id;
      wa.addr   = cmd->addr;
      wa.buf = std::make_shared<std::vector<uint8_t>>(cfg.line_bytes, 0);
      wa.beats_so_far = 0;
      br.write_asm.push_back(std::move(wa));
    }

    if (!ax.W().IsEmpty() && !br.write_asm.empty()) {
      auto wb = ax.TryPopW();
      LOGCHECK(wb.has_value(), "AssembleMasterReqs: W pop failed");
      auto& asm0 = br.write_asm.front();
      LOGCHECK(wb->data && wb->data->size() == cfg.axi.data_bytes,
               "Llc W beat size mismatch");
      uint64_t off = asm0.beats_so_far * cfg.axi.data_bytes;
      std::memcpy(asm0.buf->data() + off, wb->data->data(),
                  cfg.axi.data_bytes);
      ++asm0.beats_so_far;
      if (asm0.beats_so_far == beats_per_line) {
        LOGCHECK(wb->last, "Llc: write burst missing last on final beat");
        MasterBridge::LineReq r;
        r.is_write = true;
        r.addr     = asm0.addr;
        r.axi_id   = asm0.axi_id;
        r.data     = asm0.buf;
        br.req_q.push_back(std::move(r));
        br.write_asm.pop_front();
      }
    }
  }

  void AssembleDramRBeats() {
    auto& ax = *dram_bridge.axi;
    if (ax.R().IsEmpty()) return;
    auto rb = ax.TryPopR();
    LOGCHECK(rb.has_value(), "AssembleDramRBeats: R pop failed");
    LOGCHECK(rb->data && rb->data->size() == cfg.axi.data_bytes,
             "Llc DRAM R beat size mismatch");
    uint64_t id = rb->id;
    auto& buf = dram_bridge.r_asm[id];
    if (!buf) buf = std::make_shared<std::vector<uint8_t>>(cfg.line_bytes, 0);
    uint64_t beats = dram_bridge.r_asm_beats[id];
    uint64_t off = beats * cfg.axi.data_bytes;
    std::memcpy(buf->data() + off, rb->data->data(), cfg.axi.data_bytes);
    ++beats;
    dram_bridge.r_asm_beats[id] = beats;
    if (beats == beats_per_line) {
      LOGCHECK(rb->last, "Llc: DRAM R burst missing last on final beat");
      DramBridge::ReadDone rd;
      rd.axi_id = id;
      rd.line = buf;
      dram_bridge.r_done.push_back(std::move(rd));
      dram_bridge.r_asm.erase(id);
      dram_bridge.r_asm_beats.erase(id);
    }

    while (auto wr = dram_bridge.axi->TryPopB()) { (void)wr; }
  }

  void HandleOneDramResp() {
    if (dram_bridge.r_done.empty()) return;
    auto& head = dram_bridge.r_done.front();
    uint64_t id = head.axi_id;
    auto data_sp = head.line;
    LOGCHECK(id < dram_out.size(), "DRAM resp id OOB");
    DramOutstanding& slot = dram_out[id];
    LOGCHECK(slot.valid, "DRAM resp to free slot");

    if (slot.kind == DramKind::kFill) {
      auto& e = mshr[slot.mshr_idx];
      LOGCHECK(e.valid, "fill resp to invalid mshr");
      LOGCHECK(data_sp && data_sp->size() == cfg.line_bytes,
               "DRAM fill response data size mismatch");
      e.fill_data = *data_sp;
      e.fill_done = true;
      if (verbose) {
        RT::LogWithTime("[{}] fill_complete  mshr={} addr=0x{:x}", Name(),
                        slot.mshr_idx, e.line_addr);
      }
      SetDramSlotValid(id, false);
      dram_bridge.r_done.pop_front();
    } else if (slot.kind == DramKind::kBypassRead) {
      auto& br = master_bridges[slot.bypass_master];
      if (br.r_out.size() >= cfg.axi.r_depth) return;
      MasterBridge::ReadBurstOut burst;
      burst.axi_id = slot.bypass_req_id;
      burst.line = data_sp;
      ReadBeatsOf(slot.bypass_addr, slot.bypass_nbytes, burst.beat0, burst.nbeats);
      br.r_out.push_back(std::move(burst));
      ++bypass_this_cycle;
      if (verbose) {
        RT::LogWithTime("[{}] bypass_rd done m{} addr=0x{:x} id={}", Name(),
                        slot.bypass_master, slot.bypass_addr,
                        slot.bypass_req_id);
      }
      SetDramSlotValid(id, false);
      dram_bridge.r_done.pop_front();
    } else {
      LOGCHECK(false, "unexpected DRAM resp routed to non-read slot");
    }
  }

  void CompleteOneFilledMshr() {
    for (size_t i = 0; i < mshr.size(); ++i) {
      auto& e = mshr[i];
      if (!e.valid || !e.fill_done) continue;

      if (!e.array_written) {

        if (cfg.tag_port_conflict) fillBankBusy_[BankOf(e.set)] = true;
        auto& tag = TagAt(e.set, e.way);
        if (e.killed) {

          tag.valid = false;
          tag.dirty = false;
          tag.lock  = false;
        } else {
          tag.valid = true;
          tag.dirty = false;
          tag.lock  = false;
          tag.tag   = TagBits(e.line_addr);
          tag.data  = e.fill_data;
          UpdatePlruAccess(e.set, e.way);
        }
        e.array_written = true;
      }
      auto& tag = TagAt(e.set, e.way);
      const std::vector<uint8_t>& fill_src = e.killed ? e.fill_data : tag.data;

      bool blocked = false;
      while (!e.pending.empty()) {
        auto const& pr = e.pending.front();
        auto& br = master_bridges[pr.master];
        bool fits = pr.is_write ? br.deferred_b.size() < cfg.axi.b_depth
                                : br.deferred_r.size() < cfg.axi.r_depth;
        if (!fits) { blocked = true; break; }

        PendingReq p = std::move(e.pending.front());
        e.pending.pop_front();

        if (p.is_write) {
          LOGCHECK(p.wr_data && p.wr_data->size() == cfg.line_bytes,
                   "mshr_wake: write data size mismatch");
          tag.data  = *p.wr_data;
          tag.dirty = true;
          br.deferred_b.push_back({p.req_id, tick_ + cfg.hit_latency_cyc});
          if (verbose) {
            RT::LogWithTime("[{}] mshr_wake WR m{} addr=0x{:x} id={}", Name(),
                            p.master, p.addr, p.req_id);
          }
        } else {
          uint64_t b0, nb;
          ReadBeatsOf(p.addr, p.nbytes, b0, nb);
          br.deferred_r.push_back({p.req_id, std::make_shared<std::vector<uint8_t>>(fill_src),
                                   tick_ + cfg.hit_latency_cyc, b0, nb});
          if (verbose) {
            RT::LogWithTime("[{}] mshr_wake RD m{} addr=0x{:x} id={}", Name(),
                            p.master, p.addr, p.req_id);
          }
        }
      }
      if (blocked) continue;
      SetMshrValid(i, false);
      e.fill_done = false;
      e.array_written = false;
      e.killed = false;
      e.fill_data.clear();
      return;
    }
  }

  void WalkerStep() {
    maint_busy = !maint_q.empty();
    if (maint_q.empty()) return;
    MaintOpRec cur = maint_q.front();

    uint64_t cur_addr = cur.base + cur.cursor;
    uint64_t set = SetIdx(cur_addr);
    uint64_t tag = TagBits(cur_addr);

    if (masterBankBusy_.count(BankOf(set))) return;
    if (cfg.tag_port_conflict && fillBankBusy_.count(BankOf(set))) return;

    if (cur.op == kOpPrefetch) {
      if (TagLookup(set, tag) == cfg.n_way) {
        TryStartFill(cur_addr, false, 0, 0, full_way_mask,
                     false, nullptr);
      }
    } else {
      uint64_t w = TagLookup(set, tag);
      if (w != cfg.n_way) {
        auto& e = TagAt(set, w);
        if (cur.op == kOpInvalidate) {
          e.valid = false;
          e.dirty = false;
        } else if (cur.op == kOpClean) {
          if (e.dirty) {
            EnqueueWriteback(cur_addr,
                             std::make_shared<std::vector<uint8_t>>(e.data),
                             true);
            e.dirty = false;
          }
        } else if (cur.op == kOpFlush) {
          if (e.dirty) {
            EnqueueWriteback(cur_addr,
                             std::make_shared<std::vector<uint8_t>>(e.data),
                             true);
          }
          e.valid = false;
          e.dirty = false;
        }
      }
    }

    auto& m = maint_q.front();
    m.cursor += cfg.line_bytes;
    if (m.cursor >= m.length) {
      ++maint_op_this_cycle;
      if (verbose) {
        RT::LogWithTime("[{}] Maint done op=0x{:x} base=0x{:x} len=0x{:x}",
                        Name(), m.op, m.base, m.length);
      }
      maint_q.pop_front();
    }
  }

  bool TryStartFill(uint64_t addr, bool from_master, uint64_t master,
                    uint64_t req_id, uint64_t pin_mask, bool is_write,
                    std::shared_ptr<std::vector<uint8_t>> wr_data,
                    uint64_t nbytes = 0) {
    uint64_t line = LineAddr(addr);
    int existing = FindMshrByAddr(line);
    if (existing >= 0) {
      if (from_master) {
        PendingReq pr;
        pr.master = master;
        pr.req_id = req_id;
        pr.addr   = addr;
        pr.is_write = is_write;
        pr.nbytes   = nbytes;
        pr.wr_data  = std::move(wr_data);
        mshr[existing].pending.push_back(std::move(pr));
        ++merge_total;
      }
      return true;
    }

    int mshr_slot = FindMshrFreeSlot();
    if (mshr_slot < 0) { ++mshr_full_total; return false; }
    int dram_slot = AllocDramSlot();
    if (dram_slot < 0) { ++dram_out_full_total; return false; }

    uint64_t set = SetIdx(line);
    uint64_t victim = ChooseVictim(set, pin_mask);
    if (victim == cfg.n_way) { ++set_conflict_total; return false; }

    dram_out[dram_slot].kind  = DramKind::kFill;
    dram_out[dram_slot].mshr_idx = static_cast<uint64_t>(mshr_slot);
    SetDramSlotValid(dram_slot, true);

    auto& vt = TagAt(set, victim);
    bool victim_was_dirty = (vt.valid && vt.dirty);
    if (victim_was_dirty) {
      uint64_t victim_addr =
          (vt.tag << (log2_line + log2_set)) | (set << log2_line);
      EnqueueWriteback(victim_addr,
                       std::make_shared<std::vector<uint8_t>>(vt.data),
                       false);
    }
    vt.valid = false;
    vt.dirty = false;
    vt.lock  = true;

    auto& e = mshr[mshr_slot];
    SetMshrValid(mshr_slot, true);
    e.fill_done = false;
    e.array_written = false;
    e.line_addr = line;
    e.set = set;
    e.way = victim;
    e.pin_mask = pin_mask;
    e.victim_was_dirty = victim_was_dirty;
    e.dram_id = static_cast<uint64_t>(dram_slot);
    e.pending.clear();
    e.fill_data.clear();

    if (from_master) {
      PendingReq pr;
      pr.master = master;
      pr.req_id = req_id;
      pr.addr   = addr;
      pr.is_write = is_write;
      pr.nbytes   = nbytes;
      pr.wr_data  = std::move(wr_data);
      e.pending.push_back(std::move(pr));
    }

    DramReqInternal req;
    req.is_write = false;
    req.addr = line;
    req.id   = static_cast<uint64_t>(dram_slot);
    req.data = nullptr;
    fill_dram_q.push_back(std::move(req));
    ++fill_total;
    if (verbose) {
      RT::LogWithTime("[{}] mshr_alloc  slot={} addr=0x{:x} victim_way={} "
                      "dram_id={}",
                      Name(), mshr_slot, line, victim, dram_slot);
    }
    return true;
  }

  void EnqueueWriteback(uint64_t addr,
                        std::shared_ptr<std::vector<uint8_t>> data,
                        bool from_maint) {
    int slot = AllocDramSlot(true);
    if (slot < 0) {
      if (from_maint) maint_wb_pending.push_back({addr, std::move(data)});
      else            wb_pending.push_back({addr, std::move(data)});
      return;
    }
    dram_out[slot].kind  =
        from_maint ? DramKind::kMaintWb : DramKind::kWriteback;
    SetDramSlotValid(slot, true);
    DramReqInternal req;
    req.is_write = true;
    req.addr = addr;
    req.id   = static_cast<uint64_t>(slot);
    req.data = std::move(data);
    if (from_maint) maint_wb_dram_q.push_back(std::move(req));
    else            wb_dram_q.push_back(std::move(req));
    ++writeback_this_cycle;
    if (verbose) {
      RT::LogWithTime("[{}] wb_enqueue  addr=0x{:x} from_maint={} dram_id={}",
                      Name(), addr, from_maint ? 1 : 0, slot);
    }
  }

  void RetryDeferredWb() {
    auto retry = [&](std::deque<DeferredWb>& q, bool from_maint) {
      if (q.empty()) return;
      int slot = AllocDramSlot(true);
      if (slot < 0) return;
      auto head = std::move(q.front());
      q.pop_front();
      dram_out[slot].kind  =
          from_maint ? DramKind::kMaintWb : DramKind::kWriteback;
      SetDramSlotValid(slot, true);
      DramReqInternal req;
      req.is_write = true;
      req.addr = head.addr;
      req.id   = static_cast<uint64_t>(slot);
      req.data = std::move(head.data);
      if (from_maint) maint_wb_dram_q.push_back(std::move(req));
      else            wb_dram_q.push_back(std::move(req));
      ++writeback_this_cycle;
    };
    retry(wb_pending,       false);
    retry(maint_wb_pending, true);
  }

  void IssueOneDramReq() {
    RetryDeferredWb();

    std::deque<DramReqInternal>* qs[4] = {
        &fill_dram_q, &wb_dram_q, &maint_wb_dram_q, &bypass_dram_q};
    for (int i = 0; i < 4; ++i) {
      int idx = (dram_rr + i) & 3;
      auto& q = *qs[idx];
      if (q.empty()) continue;
      auto& r = q.front();

      auto& ax = *dram_bridge.axi;
      if (r.is_write) {
        if (ax.AW().IsFull()) continue;
        if (dram_bridge.w_out.size() >= cfg.axi.aw_depth) continue;
        ax.TryIssueWriteAddr(r.id, r.addr, burst_len, burst_size);
        DramBridge::WriteOut wo;
        wo.axi_id = r.id;
        wo.line = r.data;
        dram_bridge.w_out.push_back(std::move(wo));
      } else {
        if (ax.AR().IsFull()) continue;
        ax.TryIssueRead(r.id, r.addr, burst_len, burst_size);
      }
      q.pop_front();
      dram_rr = (idx + 1) & 3;
      return;
    }
  }

  void DrainDramOutboundW() {
    if (dram_bridge.w_out.empty()) return;
    auto& ax = *dram_bridge.axi;
    if (ax.W().IsFull()) return;
    auto& wo = dram_bridge.w_out.front();
    auto beat = std::make_shared<std::vector<uint8_t>>(
        cfg.axi.data_bytes, 0);
    uint64_t off = wo.beats_pushed * cfg.axi.data_bytes;
    LOGCHECK(wo.line && wo.line->size() == cfg.line_bytes,
             "DrainDramOutboundW: write line size mismatch");
    std::memcpy(beat->data(), wo.line->data() + off, cfg.axi.data_bytes);
    bool last = (wo.beats_pushed + 1 == beats_per_line);
    ax.TryPushWBeat(beat, last);
    ++wo.beats_pushed;
    if (last) {
      SetDramSlotValid(wo.axi_id, false);
      dram_bridge.w_out.pop_front();
    }
  }

  struct FeEntry {
    uint64_t master = 0, req_id = 0, addr = 0, nbytes = 0;
    bool     is_write = false;
    std::shared_ptr<std::vector<uint8_t>> wr_data;
    uint64_t line = 0, set = 0, bank = 0;
  };

  bool ExecuteRequest(const FeEntry& e) {
    auto& br = master_bridges[e.master];
    if (!e.is_write && br.deferred_r.size() >= cfg.axi.r_depth) return false;
    if ( e.is_write && br.deferred_b.size() >= cfg.axi.b_depth) return false;

    RegionAttr attr = LookupRegion(e.addr);
    uint64_t defer = DeferredExtraCyc();

    if (attr.mode == AttrMode::Bypass) {
      return TryEnqueueBypass(e.master, e.addr, e.req_id, e.is_write, e.wr_data, e.nbytes);
    }

    if (cfg.write_through && e.is_write) {

      uint64_t wt_tag = TagBits(e.addr);
      uint64_t wt_w = TagLookup(e.set, wt_tag);
      int wt_mshr = FindMshrByAddr(e.line);
      if (!TryEnqueueBypass(e.master, e.addr, e.req_id, true, e.wr_data, e.nbytes))
        return false;
      if (wt_w != cfg.n_way) {
        auto& te = TagAt(e.set, wt_w);
        te.valid = false;
        te.dirty = false;
        ++wt_inval_this_cycle;
      }
      if (wt_mshr >= 0 && !mshr[wt_mshr].killed) {
        mshr[wt_mshr].killed = true;
        ++wt_kill_this_cycle;
      }
      ++wt_this_cycle;
      return true;
    }

    uint64_t tag = TagBits(e.addr);
    uint64_t w = TagLookup(e.set, tag);
    if (w != cfg.n_way) {
      auto& te = TagAt(e.set, w);

      if (e.is_write) {
        LOGCHECK(e.wr_data && e.wr_data->size() == cfg.line_bytes,
                 "write hit: wr_data size mismatch");
        te.data = *e.wr_data;
        te.dirty = true;
        br.deferred_b.push_back({e.req_id, tick_ + defer});
      } else {
        uint64_t b0, nb;
        ReadBeatsOf(e.addr, e.nbytes, b0, nb);
        br.deferred_r.push_back({e.req_id, std::make_shared<std::vector<uint8_t>>(te.data),
                                 tick_ + defer, b0, nb});
      }
      UpdatePlruAccess(e.set, w);
      ++hit_this_cycle;
      if (verbose) {
        RT::LogWithTime("[{}] HIT  m{} {} addr=0x{:x} way={} id={}", Name(),
                        e.master, e.is_write ? "WR" : "RD", e.addr, w, e.req_id);
      }
      return true;
    }
    bool ok = TryStartFill(e.addr, true, e.master, e.req_id,
                           attr.pin_way_mask, e.is_write, e.wr_data, e.nbytes);
    if (!ok) return false;
    ++miss_this_cycle;
    if (verbose) {
      RT::LogWithTime("[{}] MISS m{} {} addr=0x{:x} id={}", Name(), e.master,
                      e.is_write ? "WR" : "RD", e.addr, e.req_id);
    }
    return true;
  }

  uint64_t DeferredExtraCyc() const {
    uint64_t st = cfg.frontend_pipe_stages;
    return st >= cfg.hit_latency_cyc ? 0 : cfg.hit_latency_cyc - st;
  }

  void ProcessMasterRequests() {

    masterBankBusy_.clear();
    if (cfg.frontend_pipe_stages > 0) { PipelinedFrontend(); return; }
    auto& bank_busy = masterBankBusy_;

    for (uint64_t k = 0; k < cfg.n_master; ++k) {
      uint64_t m = (master_rr + k) % cfg.n_master;
      auto& br = master_bridges[m];
      if (br.req_q.empty()) continue;
      auto const& head = br.req_q.front();

      FeEntry e;
      e.master = m; e.req_id = head.axi_id; e.addr = head.addr;
      e.nbytes = head.nbytes; e.is_write = head.is_write; e.wr_data = head.data;
      e.line = LineAddr(e.addr); e.set = SetIdx(e.line); e.bank = BankOf(e.set);

      if (bank_busy.count(e.bank)) continue;
      if (cfg.tag_port_conflict && fillBankBusy_.count(e.bank)) {

        ++tag_port_stall_total;
        continue;
      }
      if (!ExecuteRequest(e)) continue;
      br.req_q.pop_front();
      bank_busy[e.bank] = true;
    }
    master_rr = (master_rr + 1) % cfg.n_master;
  }

  void PipelinedFrontend() {
    const uint64_t NS = cfg.frontend_pipe_stages;

    for (auto& lane : fe_[NS - 1]) {
      if (!lane.second.has_value()) continue;
      if (ExecuteRequest(*lane.second)) {
        masterBankBusy_[lane.first] = true;
        --feInFlight_[lane.second->master];
        lane.second.reset();
      }
    }

    for (uint64_t s = NS - 1; s >= 1; --s) {
      for (auto& lane : fe_[s - 1]) {
        if (!lane.second.has_value()) continue;
        auto& next = fe_[s][lane.first];
        if (next.has_value()) continue;

        if (s == 1 && cfg.tag_port_conflict && fillBankBusy_.count(lane.first)) {
          ++tag_port_stall_total;
          continue;
        }
        if (s == 1) masterBankBusy_[lane.first] = true;
        next = std::move(lane.second);
        lane.second.reset();
      }
    }

    for (uint64_t k = 0; k < cfg.n_master; ++k) {
      uint64_t m = (master_rr + k) % cfg.n_master;
      auto& br = master_bridges[m];
      if (br.req_q.empty()) continue;
      auto const& head = br.req_q.front();

      FeEntry e;
      e.master = m; e.req_id = head.axi_id; e.addr = head.addr;
      e.nbytes = head.nbytes; e.is_write = head.is_write; e.wr_data = head.data;
      e.line = LineAddr(e.addr); e.set = SetIdx(e.line); e.bank = BankOf(e.set);

      auto& slot = fe_[0][e.bank];
      if (slot.has_value()) continue;
      ++feInFlight_[m];
      slot = std::move(e);
      br.req_q.pop_front();
    }
    master_rr = (master_rr + 1) % cfg.n_master;
  }

  bool TryEnqueueBypass(uint64_t m, uint64_t addr, uint64_t req_id,
                        bool is_write,
                        std::shared_ptr<std::vector<uint8_t>> wr_sp,
                        uint64_t nbytes = 0) {
    int slot = AllocDramSlot(is_write);
    if (slot < 0) return false;
    if (bypass_dram_q.size() >= cfg.axi.aw_depth) return false;

    DramReqInternal req;
    req.is_write = is_write;
    req.addr = LineAddr(addr);
    req.id   = static_cast<uint64_t>(slot);
    if (is_write) {
      LOGCHECK(wr_sp && wr_sp->size() == cfg.line_bytes,
               "bypass_wr: wr_data size mismatch");
      req.data = wr_sp;
      dram_out[slot].kind = DramKind::kBypassWrite;
      master_bridges[m].b_out.push_back(req_id);
      ++bypass_this_cycle;
    } else {
      dram_out[slot].kind = DramKind::kBypassRead;
      dram_out[slot].bypass_master = m;
      dram_out[slot].bypass_req_id = req_id;
      dram_out[slot].bypass_addr   = addr;
      dram_out[slot].bypass_nbytes = nbytes;
    }
    SetDramSlotValid(slot, true);
    bypass_dram_q.push_back(std::move(req));
    return true;
  }

  void DrainDeferredResp(uint64_t m) {
    auto& br = master_bridges[m];
    while (!br.deferred_r.empty() && br.deferred_r.front().ready_at <= tick_) {
      if (br.r_out.size() >= cfg.axi.r_depth) break;
      auto& d = br.deferred_r.front();
      MasterBridge::ReadBurstOut burst;
      burst.axi_id = d.axi_id;
      burst.line = d.line;
      burst.beat0 = d.beat0;
      burst.nbeats = d.nbeats;
      br.r_out.push_back(std::move(burst));
      br.deferred_r.pop_front();
    }
    while (!br.deferred_b.empty() && br.deferred_b.front().ready_at <= tick_) {
      if (br.b_out.size() >= cfg.axi.b_depth) break;
      br.b_out.push_back(br.deferred_b.front().req_id);
      br.deferred_b.pop_front();
    }
  }

  void EmitMasterR(uint64_t m) {
    auto& br = master_bridges[m];
    if (br.r_out.empty()) return;
    auto& ax = *br.axi;
    if (ax.R().IsFull()) return;
    auto& burst = br.r_out.front();
    auto beat = std::make_shared<std::vector<uint8_t>>(
        cfg.axi.data_bytes, 0);
    LOGCHECK(burst.line && burst.line->size() == cfg.line_bytes,
             "EmitMasterR: line size mismatch");
    uint64_t nbeats = burst.nbeats ? burst.nbeats : beats_per_line;
    uint64_t off = (burst.beat0 + burst.beats_emitted) * cfg.axi.data_bytes;
    std::memcpy(beat->data(), burst.line->data() + off, cfg.axi.data_bytes);
    bool last = (burst.beats_emitted + 1 == nbeats);
    ax.TryPushR(burst.axi_id, beat, last);
    ++burst.beats_emitted;
    if (last) br.r_out.pop_front();
  }
  void EmitMasterB(uint64_t m) {
    auto& br = master_bridges[m];
    if (br.b_out.empty()) return;
    auto& ax = *br.axi;
    if (ax.B().IsFull()) return;
    ax.TryPushB(br.b_out.front());
    br.b_out.pop_front();
  }

  static uint64_t BurstBytes(uint8_t len, uint8_t size) {
    return (uint64_t(len) + 1) << size;
  }

  void EmitTraces() {
    TraceIfChanged("hit_event",        hit_event.Get());
    TraceIfChanged("miss_event",       miss_event.Get());
    TraceIfChanged("writeback_event",  writeback_event.Get());
    TraceIfChanged("bypass_event",     bypass_event.Get());
    TraceIfChanged("maint_op_event",   maint_op_event.Get());
    TraceIfChanged("mshr_occ",         mshr_occ.Get());
    TraceIfChanged("dram_outstanding", dram_out_count.Get());
    TraceIfChanged("maint_busy",       maint_busy_latched.Get());
  }
  void TraceIfChanged(const std::string& name, uint64_t v) {
    auto it = last_traced.find(name);
    if (it == last_traced.end() || it->second != v) {
      Trace(name, v);
      last_traced[name] = v;
    }
  }

  Config cfg;
  bool verbose;
  uint64_t n_set = 0;
  uint32_t log2_line = 0;
  uint32_t log2_set  = 0;
  uint32_t log2_way  = 0;
  uint64_t full_way_mask = 0;
  uint64_t beats_per_line = 0;
  uint8_t  burst_len = 0;
  uint8_t  burst_size = 0;

  std::vector<MasterBridge> master_bridges;
  DramBridge dram_bridge;
  std::shared_ptr<Fifo<CtrlReqPkt>> ctrl_fifo;

  std::vector<TagEntry> tags;
  std::vector<uint8_t>  plru;

  std::vector<RegionAttr> regions;
  RegionAttr default_attr;
  std::deque<MaintOpRec> maint_q;
  bool maint_busy = false;

  std::vector<MshrEntry> mshr;
  std::vector<DramOutstanding> dram_out;

  std::deque<DramReqInternal> fill_dram_q;
  std::deque<DramReqInternal> wb_dram_q;
  std::deque<DramReqInternal> maint_wb_dram_q;
  std::deque<DramReqInternal> bypass_dram_q;
  uint32_t dram_rr = 0;

  std::deque<DeferredWb> wb_pending;
  std::deque<DeferredWb> maint_wb_pending;

  uint64_t master_rr = 0;
  uint64_t tick_ = 0;

  std::unordered_map<uint64_t, bool> masterBankBusy_;

  std::unordered_map<uint64_t, bool> fillBankBusy_;

  std::vector<std::unordered_map<uint64_t, std::optional<FeEntry>>> fe_;

  std::vector<uint64_t> feInFlight_;

  Logic64 hit_event;
  Logic64 miss_event;
  Logic64 writeback_event;
  Logic64 bypass_event;
  Logic64 maint_op_event;
  Logic64 hit_count;
  Logic64 miss_count;
  Logic64 fill_count;
  Logic64 mshr_merge_count;
  Logic64 writeback_count;
  Logic64 bypass_count;
  Logic64 wt_count, wt_inval_count, wt_kill_count;
  Logic64 maint_op_completed;
  Logic64 mshr_occ;
  Logic64 dram_out_count;
  Logic64 maint_busy_latched;

  uint64_t hit_this_cycle             = 0;
  uint64_t miss_this_cycle            = 0;
  uint64_t writeback_this_cycle       = 0;
  uint64_t bypass_this_cycle          = 0;
  uint64_t wt_this_cycle = 0, wt_inval_this_cycle = 0, wt_kill_this_cycle = 0;
  uint64_t maint_op_this_cycle        = 0;
  uint64_t hit_total                  = 0;
  uint64_t miss_total                 = 0;
  uint64_t fill_total                 = 0;
  uint64_t merge_total                = 0;
  uint64_t writeback_total            = 0;
  uint64_t bypass_total               = 0;
  uint64_t wt_total = 0, wt_inval_total = 0, wt_kill_total = 0;
  uint64_t maint_op_total             = 0;
  uint64_t tag_port_stall_total       = 0;

  uint64_t mshrPeak_ = 0, dramOutPeak_ = 0;
  uint64_t set_conflict_total         = 0;
  uint64_t mshr_full_total            = 0;
  uint64_t dram_out_full_total        = 0;
  uint64_t mshr_occ_pending            = 0;
  uint64_t dram_out_pending            = 0;
  bool     maint_busy_pending          = false;

  std::unordered_map<std::string, uint64_t> last_traced;
};

}

#endif
