#ifndef _LATCH_MODULE_DDR3_
#define _LATCH_MODULE_DDR3_

#include <cstdint>
#include <deque>
#include <memory>
#include <sstream>
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

#include "ramulator/base/config.h"
#include "ramulator/base/factory.h"
#include "ramulator/base/request.h"
#include "ramulator/frontend/i_frontend.h"
#include "ramulator/memory_system/i_memory_system.h"

namespace latch {

class Ddr3 : public ClkModule {
 public:
  static std::string DefaultConfigYaml() {
    return R"YAML(
frontend:
  impl: External
  clock_ratio: 1
memory_system:
  impl: GenericDRAM
  clock_ratio: 1
  channel_mapper:
    impl: CacheLineInterleave
    interleave_bits: 0
  controllers:
    - impl: GenericDDR
      scheduler:
        impl: FRFCFS
      refresh_manager:
        impl: AllBank
      row_policy:
        impl: Open
      addr_mapper:
        impl: RoBaRaCoCh
      dram:
        impl: DDR3
        channel_width: 64
        org:
          dq: 8
          count: [1, 1, 8, 131072, 1024]
        timing: [1600, 4, 11, 11, 11, 28, 39, 12, 6, 8, 4, 5, 6, 24, 280, 6240, 2, 1250]
        read_latency: 15
        timing_constraints:
          - [0, [3, 5], [3, 5], 4]
          - [0, [4, 6], [4, 6], 4]
          - [1, [3, 5], [3, 5], 4]
          - [1, [4, 6], [4, 6], 4]
          - [1, [3, 5], [4, 6], 9]
          - [1, [4, 6], [3, 5], 18]
          - [1, [3, 5], [3, 4, 5, 6], 6, 1, true]
          - [1, [4, 6], [3, 5], 3, 1, true]
          - [1, [3], [2], 6]
          - [1, [4], [2], 24]
          - [1, [0], [0], 5]
          - [1, [0], [0], 24, 4]
          - [1, [0], [2], 28]
          - [1, [2], [0], 11]
          - [1, [0], [7], 39]
          - [1, [1, 2], [7], 11]
          - [1, [5], [7], 17]
          - [1, [6], [7], 35]
          - [1, [7], [0], 280]
          - [2, [0], [0], 39]
          - [2, [0], [3, 4, 5, 6], 11]
          - [2, [0], [1], 28]
          - [2, [1], [0], 11]
          - [2, [3], [1], 6]
          - [2, [4], [1], 24]
          - [2, [5], [0], 17]
          - [2, [6], [0], 35]
)YAML";
  }

  static AxiConfig MakeDefaultAxiConfig() {
    AxiConfig c;
    c.data_bytes = 16;
    c.ar_depth = 4;
    c.r_depth  = 16;
    c.aw_depth = 4;
    c.w_depth  = 16;
    c.b_depth  = 4;
    return c;
  }

  Ddr3(ClockPtr clock, const std::string& name = "ddr3", bool v = false)
      : Ddr3(clock, DefaultConfigYaml(), name, v, MakeDefaultAxiConfig()) {}

  Ddr3(ClockPtr clock, const std::string& config_yaml,
       const std::string& name, bool v, const AxiConfig& axi_cfg)
      : ClkModule(clock),
        verbose(v),
        axi_slave(std::make_shared<AxiSlave>(clock, name + "_axi", axi_cfg)),
        inflight_reads(clock),
        inflight_writes(clock),
        total_reads_done(clock),
        total_writes_done(clock) {
    RegisterId(name, 0);

    auto cfg = Ramulator::Config::parse_config_string(config_yaml);
    frontend.reset(Ramulator::Factory::create_frontend(cfg));
    memory_system.reset(Ramulator::Factory::create_memory_system(cfg));
    LOGCHECK(frontend != nullptr, "ramulator2 frontend creation failed.");
    LOGCHECK(memory_system != nullptr,
             "ramulator2 memory_system creation failed.");
    frontend->connect_memory_system(memory_system.get());
    memory_system->connect_frontend(frontend.get());

    fe_clock_ratio = frontend->get_clock_ratio();
    mem_clock_ratio = memory_system->get_clock_ratio();
    LOGCHECK(fe_clock_ratio > 0 && mem_clock_ratio > 0,
             "ramulator2 clock_ratio must be > 0 on both sides.");
    fe_count = mem_clock_ratio - 1;
    mem_count = fe_clock_ratio - 1;

    tx_bytes = static_cast<uint64_t>(memory_system->get_tx_bytes());
    LOGCHECK(tx_bytes % axi_cfg.data_bytes == 0,
             "Ddr3: tx_bytes must be a multiple of AXI data_bytes");
    beats_per_tx = tx_bytes / axi_cfg.data_bytes;

    if (verbose) {
      RT::LogWithTime(
          "[{}] init  tx_bytes={} tCK_ps={} axi_data_bytes={} beats={}", name,
          tx_bytes, GetTCKps(), axi_cfg.data_bytes, beats_per_tx);
    }
  }

  ~Ddr3() override {
    if (memory_system) memory_system->finalize();
    if (frontend) frontend->finalize();
  }

  std::shared_ptr<AxiSlave> AxiPort() { return axi_slave; }

  int   GetTxBytes() const { return memory_system->get_tx_bytes(); }
  float GetTCKps()   const { return memory_system->get_tCK(); }
  uint64_t TotalReadsDone()  const { return total_reads_done.Get(); }
  uint64_t TotalWritesDone() const { return total_writes_done.Get(); }
  uint64_t InflightReads()   const { return inflight_reads.Get(); }
  uint64_t InflightWrites()  const { return inflight_writes.Get(); }

  std::string DumpStats() {
    std::ostringstream oss;
    frontend->print_stats(oss);
    memory_system->print_stats(oss);
    return oss.str();
  }

  virtual void Cycle() override {
    DelayCycle(1);

    AcceptAR();
    AcceptAW();
    AcceptW();
    TrySubmitWrite();

    if (++fe_count >= mem_clock_ratio) {
      fe_count = 0;
      frontend->tick();
    }
    if (++mem_count >= fe_clock_ratio) {
      mem_count = 0;
      memory_system->tick();
    }

    EmitR();
    EmitB();

    inflight_reads    = inflight_reads_pending;
    inflight_writes   = inflight_writes_pending;
    total_reads_done  = total_reads_done_pending;
    total_writes_done = total_writes_done_pending;

    TraceIfChanged("ar_q_depth", axi_slave->AR().ValidCount());
    TraceIfChanged("aw_q_depth", axi_slave->AW().ValidCount());
    TraceIfChanged("w_q_depth",  axi_slave->W().ValidCount());
    TraceIfChanged("r_q_depth",  axi_slave->R().ValidCount());
    TraceIfChanged("b_q_depth",  axi_slave->B().ValidCount());
    TraceIfChanged("inflight_reads",    inflight_reads.Get());
    TraceIfChanged("inflight_writes",   inflight_writes.Get());
    TraceIfChanged("total_reads_done",  total_reads_done.Get());
    TraceIfChanged("total_writes_done", total_writes_done.Get());
  }

 private:
  struct ReadJob {
    uint64_t seq;
    uint64_t id;
    uint64_t addr;
    uint8_t  len;
    uint8_t  size;
    uint64_t beats_emitted = 0;
  };
  struct WriteJob {
    uint64_t seq;
    uint64_t id;
    uint64_t addr;
    uint8_t  len;
    uint8_t  size;
    uint64_t beats_accepted = 0;
    bool     all_beats_in = false;
    bool     submitted = false;
  };

  void TraceIfChanged(const std::string& name, uint64_t v) {
    auto it = last_traced.find(name);
    if (it == last_traced.end() || it->second != v) {
      Trace(name, v);
      last_traced[name] = v;
    }
  }

  static uint64_t BurstBytes(uint8_t len, uint8_t size) {
    return (uint64_t(len) + 1) << size;
  }

  void AcceptAR() {
    auto& ar = axi_slave->AR();
    if (ar.IsEmpty()) return;
    auto& h = ar.Front();
    uint64_t addr = uint64_t(h.addr);
    uint64_t id   = uint64_t(h.id);
    uint8_t  len  = static_cast<uint8_t>(uint64_t(h.len));
    uint8_t  size = static_cast<uint8_t>(uint64_t(h.size));
    LOGCHECK(BurstBytes(len, size) == tx_bytes,
             "Ddr3: AR burst total bytes must equal tx_bytes");

    uint64_t seq = next_seq;

    read_seq_done[seq] = false;
    ++inflight_reads_pending;
    auto cb = [this, seq](Ramulator::Request&) {
      auto it = read_seq_done.find(seq);
      if (it != read_seq_done.end()) it->second = true;
      --inflight_reads_pending;
      ++total_reads_done_pending;
    };
    bool ok = frontend->receive_external_requests(
        Ramulator::Request::Type::Read,
        static_cast<Ramulator::Addr_t>(addr), 0, std::move(cb),
        static_cast<int>(tx_bytes));
    if (!ok) {
      read_seq_done.erase(seq);
      --inflight_reads_pending;
      return;
    }

    ++next_seq;
    ar.Pop();
    read_jobs.push_back(ReadJob{seq, id, addr, len, size, 0});
    if (verbose) {
      RT::LogWithTime("[{}] AR  id={} addr=0x{:x}", Name(), id, addr);
    }
  }

  void AcceptAW() {
    auto& aw = axi_slave->AW();
    if (aw.IsEmpty()) return;
    auto& h = aw.Front();
    uint64_t addr = uint64_t(h.addr);
    uint64_t id   = uint64_t(h.id);
    uint8_t  len  = static_cast<uint8_t>(uint64_t(h.len));
    uint8_t  size = static_cast<uint8_t>(uint64_t(h.size));
    LOGCHECK(BurstBytes(len, size) == tx_bytes,
             "Ddr3: AW burst total bytes must equal tx_bytes");
    aw.Pop();
    uint64_t seq = next_seq++;
    write_jobs.push_back(
        WriteJob{seq, id, addr, len, size, 0, false, false});
    if (verbose) {
      RT::LogWithTime("[{}] AW  id={} addr=0x{:x}", Name(), id, addr);
    }
  }

  void AcceptW() {
    auto& w = axi_slave->W();
    if (w.IsEmpty()) return;
    WriteJob* target = nullptr;
    for (auto& j : write_jobs) {
      if (!j.all_beats_in) { target = &j; break; }
    }
    if (!target) return;
    auto& h = w.Front();
    bool last = (uint64_t(h.last) != 0);
    w.Pop();
    ++target->beats_accepted;
    if (target->beats_accepted == uint64_t(target->len) + 1) {
      LOGCHECK(last, "Ddr3: write burst missing last on the final beat");
      target->all_beats_in = true;
    }
  }

  void TrySubmitWrite() {
    for (auto& job : write_jobs) {
      if (!job.all_beats_in) break;
      if (job.submitted) continue;

      write_seq_done[job.seq] = false;
      ++inflight_writes_pending;
      auto cb = [this, seq = job.seq](Ramulator::Request&) {
        auto it = write_seq_done.find(seq);
        if (it != write_seq_done.end()) it->second = true;
        --inflight_writes_pending;
        ++total_writes_done_pending;
      };
      bool ok = frontend->receive_external_requests(
          Ramulator::Request::Type::Write,
          static_cast<Ramulator::Addr_t>(job.addr), 0,
          std::move(cb), static_cast<int>(tx_bytes));
      if (!ok) {
        write_seq_done.erase(job.seq);
        --inflight_writes_pending;
        return;
      }
      job.submitted = true;
      if (verbose) {
        RT::LogWithTime("[{}] submit WR id={} addr=0x{:x}", Name(), job.id,
                        job.addr);
      }
    }
  }

  void EmitR() {
    if (read_jobs.empty()) return;
    if (axi_slave->R().IsFull()) return;
    auto& job = read_jobs.front();
    auto it = read_seq_done.find(job.seq);
    if (it == read_seq_done.end() || !it->second) return;
    auto data = std::make_shared<std::vector<uint8_t>>(
        axi_slave->Cfg().data_bytes, 0);
    bool last = (job.beats_emitted + 1 >= uint64_t(job.len) + 1);
    axi_slave->TryPushR(job.id, data, last);
    ++job.beats_emitted;
    if (last) {
      if (verbose) {
        RT::LogWithTime("[{}] R last id={} addr=0x{:x}", Name(), job.id,
                        job.addr);
      }
      read_seq_done.erase(job.seq);
      read_jobs.pop_front();
    }
  }

  void EmitB() {
    if (write_jobs.empty()) return;
    auto& job = write_jobs.front();
    if (!job.submitted) return;
    auto it = write_seq_done.find(job.seq);
    if (it == write_seq_done.end() || !it->second) return;
    if (!axi_slave->TryPushB(job.id)) return;
    if (verbose) {
      RT::LogWithTime("[{}] B id={} addr=0x{:x}", Name(), job.id, job.addr);
    }
    write_seq_done.erase(job.seq);
    write_jobs.pop_front();
  }

  std::unique_ptr<Ramulator::IFrontEnd> frontend;
  std::unique_ptr<Ramulator::IMemorySystem> memory_system;
  int fe_clock_ratio = 1;
  int mem_clock_ratio = 1;
  int fe_count = 0;
  int mem_count = 0;
  bool verbose = false;
  uint64_t tx_bytes = 0;
  uint64_t beats_per_tx = 0;

  std::shared_ptr<AxiSlave> axi_slave;

  std::deque<ReadJob>  read_jobs;
  std::deque<WriteJob> write_jobs;
  std::unordered_map<uint64_t, bool> read_seq_done;
  std::unordered_map<uint64_t, bool> write_seq_done;
  uint64_t next_seq = 0;

  Logic64 inflight_reads;
  Logic64 inflight_writes;
  Logic64 total_reads_done;
  Logic64 total_writes_done;

  uint64_t inflight_reads_pending    = 0;
  uint64_t inflight_writes_pending   = 0;
  uint64_t total_reads_done_pending  = 0;
  uint64_t total_writes_done_pending = 0;

  std::unordered_map<std::string, uint64_t> last_traced;
};

}
#endif
