#include "module/llc.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/clock.h"
#include "base/fifo.h"
#include "base/logic.h"
#include "base/module.h"
#include "base/runtime.h"
#include "gtest/gtest.h"
#include "module/axi.h"

using namespace latch;

namespace {

std::shared_ptr<std::vector<uint8_t>> MakeLine(uint64_t line_bytes,
                                               uint64_t pat) {
  auto v = std::make_shared<std::vector<uint8_t>>(line_bytes, 0);
  std::memcpy(v->data(), &pat, sizeof(pat));
  return v;
}
uint64_t LineHead(const std::vector<uint8_t>& v) {
  uint64_t x = 0;
  std::memcpy(&x, v.data(), sizeof(x));
  return x;
}

class LlcEventObserver : public ClkModule {
 public:
  LlcEventObserver(ClockPtr clock, Llc* l, const std::string& tag = "")
      : ClkModule(clock), llc(l) {
    RegisterId("llc_obs" + tag, 0);
  }
  void Cycle() override {
    DelayCycle(1);
    hit_events.push_back(llc->HitEvent());
    miss_events.push_back(llc->MissEvent());
    wb_events.push_back(llc->WritebackEvent());
    bypass_events.push_back(llc->BypassEvent());
    maint_events.push_back(llc->MaintOpEvent());
  }
  static uint64_t Sum(const std::vector<uint64_t>& v) {
    uint64_t s = 0;
    for (auto x : v) s += x;
    return s;
  }
  static int64_t FirstNonZero(const std::vector<uint64_t>& v) {
    for (size_t i = 0; i < v.size(); ++i)
      if (v[i] > 0) return static_cast<int64_t>(i);
    return -1;
  }
  uint64_t HitSum()    const { return Sum(hit_events); }
  uint64_t MissSum()   const { return Sum(miss_events); }
  uint64_t WbSum()     const { return Sum(wb_events); }
  uint64_t BypassSum() const { return Sum(bypass_events); }
  uint64_t MaintSum()  const { return Sum(maint_events); }

  std::vector<uint64_t> hit_events, miss_events, wb_events,
                        bypass_events, maint_events;

 private:
  Llc* llc;
};

class FakeDram : public AxiSlave {
 public:
  FakeDram(ClockPtr clock, Llc* l, uint64_t lb,
           uint32_t read_latency_cycles = 8, bool v = false)
      : AxiSlave(clock, "fake_dram", Llc::MakeDefaultAxi()),
        line_bytes(lb),
        read_latency(read_latency_cycles),
        verbose(v) {
    RegisterId("fake_dram", 0);
    l->AxiMasterPort()->Bind(*this);
    uint64_t db = Cfg().data_bytes;
    LOGCHECK(line_bytes % db == 0, "FakeDram: line_bytes/data_bytes mismatch");
    beats_per_line = line_bytes / db;
  }

  void Cycle() override {
    DelayCycle(1);
    ++cycle;

    if (auto cmd = TryPopAR()) {
      LOGCHECK(BurstBytes(cmd->len, cmd->size) == line_bytes,
               "FakeDram: AR burst must equal line_bytes");
      ReadJob j;
      j.id = cmd->id;
      j.addr = cmd->addr;
      j.due_cycle = cycle + read_latency;
      j.data = std::make_shared<std::vector<uint8_t>>(Read(j.addr));
      read_q.push_back(std::move(j));
      ++reads_seen;
      if (verbose) {
        RT::LogWithTime("[fake_dram] AR id={} addr=0x{:x}", cmd->id, cmd->addr);
      }
    }

    if (auto cmd = TryPopAW()) {
      LOGCHECK(BurstBytes(cmd->len, cmd->size) == line_bytes,
               "FakeDram: AW burst must equal line_bytes");
      WriteJob j;
      j.id = cmd->id;
      j.addr = cmd->addr;
      j.buf = std::make_shared<std::vector<uint8_t>>(line_bytes, 0);
      write_q.push_back(std::move(j));
    }

    if (auto wb = TryPopW()) {
      LOGCHECK(!write_q.empty(), "FakeDram: W beat without AW");
      auto& j = write_q.front();
      uint64_t db = Cfg().data_bytes;
      LOGCHECK(wb->data && wb->data->size() == db,
               "FakeDram: W beat data size mismatch");
      uint64_t off = j.beats_accepted * db;
      std::memcpy(j.buf->data() + off, wb->data->data(), db);
      ++j.beats_accepted;
      if (j.beats_accepted == beats_per_line) {
        LOGCHECK(wb->last,
                 "FakeDram: write burst missing last on final beat");
        Write(j.addr, *j.buf);
        ++writes_seen;
        pending_b.push_back(j.id);
        if (verbose) {
          RT::LogWithTime("[fake_dram] WR addr=0x{:x} id={}", j.addr, j.id);
        }
        write_q.pop_front();
      }
    }

    if (!read_q.empty() && !R().IsFull()) {
      auto& j = read_q.front();
      if (j.due_cycle <= cycle) {
        uint64_t db = Cfg().data_bytes;
        auto beat = std::make_shared<std::vector<uint8_t>>(db, 0);
        uint64_t off = j.beats_emitted * db;
        std::memcpy(beat->data(), j.data->data() + off, db);
        bool last = (j.beats_emitted + 1 == beats_per_line);
        TryPushR(j.id, beat, last);
        ++j.beats_emitted;
        if (last) {
          if (verbose) {
            RT::LogWithTime("[fake_dram] R last id={} addr=0x{:x}", j.id,
                            j.addr);
          }
          read_q.pop_front();
        }
      }
    }

    if (!pending_b.empty() && !B().IsFull()) {
      TryPushB(pending_b.front());
      pending_b.pop_front();
    }
  }

  void Write(uint64_t line_addr, const std::vector<uint8_t>& data) {
    store[line_addr] = data;
  }
  std::vector<uint8_t> Read(uint64_t line_addr) const {
    auto it = store.find(line_addr);
    if (it == store.end()) return std::vector<uint8_t>(line_bytes, 0);
    return it->second;
  }
  bool Has(uint64_t line_addr) const { return store.count(line_addr) > 0; }
  uint64_t ReadsSeen()  const { return reads_seen; }
  uint64_t WritesSeen() const { return writes_seen; }

 private:
  struct ReadJob {
    uint64_t id;
    uint64_t addr;
    uint64_t due_cycle;
    std::shared_ptr<std::vector<uint8_t>> data;
    uint64_t beats_emitted = 0;
  };
  struct WriteJob {
    uint64_t id;
    uint64_t addr;
    std::shared_ptr<std::vector<uint8_t>> buf;
    uint64_t beats_accepted = 0;
  };
  static uint64_t BurstBytes(uint8_t len, uint8_t size) {
    return (uint64_t(len) + 1) << size;
  }

  uint64_t line_bytes;
  uint32_t read_latency;
  bool verbose;
  uint64_t cycle = 0;
  uint64_t beats_per_line = 0;
  std::unordered_map<uint64_t, std::vector<uint8_t>> store;
  std::deque<ReadJob> read_q;
  std::deque<WriteJob> write_q;
  std::deque<uint64_t> pending_b;
  uint64_t reads_seen = 0, writes_seen = 0;
};

class TestMaster : public AxiMaster {
 public:
  TestMaster(ClockPtr clock, Llc* l, uint64_t m,
             const std::string& name)
      : AxiMaster(clock, name, Llc::MakeDefaultAxi()),
        clk_(clock),
        line_bytes_(l->NSet() ? l->BurstSize() : 0) {

    Bind(*l->AxiSlavePort(m));
    beats_per_line_ = l->BeatsPerLine();
    burst_len_ = l->BurstLen();
    burst_size_ = l->BurstSize();
  }

  void IssueRead(uint64_t id, uint64_t addr) {
    while (!TryIssueRead(id, addr, burst_len_, burst_size_))
      clk_->DelayCycle(1);
  }

  void WriteLine(uint64_t id, uint64_t addr,
                 std::shared_ptr<std::vector<uint8_t>> line) {
    while (!TryIssueWriteAddr(id, addr, burst_len_, burst_size_))
      clk_->DelayCycle(1);
    uint64_t db = Cfg().data_bytes;
    LOGCHECK(line && line->size() == beats_per_line_ * db,
             "TestMaster::WriteLine: line size mismatch");
    for (uint64_t i = 0; i < beats_per_line_; ++i) {
      auto beat = std::make_shared<std::vector<uint8_t>>(db, 0);
      std::memcpy(beat->data(), line->data() + i * db, db);
      bool last = (i + 1 == beats_per_line_);
      while (!TryPushWBeat(beat, last)) clk_->DelayCycle(1);
    }
  }

  std::shared_ptr<std::vector<uint8_t>> PopReadLine() {
    uint64_t db = Cfg().data_bytes;
    auto line = std::make_shared<std::vector<uint8_t>>(beats_per_line_ * db, 0);
    for (uint64_t i = 0; i < beats_per_line_; ++i) {
      while (true) {
        auto r = TryPopR();
        if (r) {
          std::memcpy(line->data() + i * db, r->data->data(), db);
          break;
        }
        clk_->DelayCycle(1);
      }
    }
    return line;
  }

  void WaitB() {
    while (true) {
      auto b = TryPopB();
      if (b) return;
      clk_->DelayCycle(1);
    }
  }

 private:
  ClockPtr clk_;
  uint64_t line_bytes_;
  uint64_t beats_per_line_;
  uint8_t burst_len_;
  uint8_t burst_size_;
};

struct ScriptOp {
  bool is_write;
  uint64_t addr;
  uint64_t req_id;
  std::shared_ptr<std::vector<uint8_t>> wr_data;
  uint64_t wait_resp_min = 0;
  std::function<bool()> gate;
};

class ScriptDriver : public AxiMaster {
 public:
  ScriptDriver(ClockPtr clock, Llc* l, uint64_t m,
               std::vector<ScriptOp> ops)
      : AxiMaster(clock, "script_drv_m" + std::to_string(m),
                  Llc::MakeDefaultAxi()),
        line_bytes(l->BurstSize() ? (1ULL << l->BurstSize()) *
                                     (l->BurstLen() + 1)
                                  : 0),
        master(m),
        script(std::move(ops)) {
    Bind(*l->AxiSlavePort(m));
    beats_per_line = l->BeatsPerLine();
    burst_len = l->BurstLen();
    burst_size = l->BurstSize();
  }

  void Cycle() override {
    DelayCycle(1);

    if (pending_w_beats == 0 && idx < script.size()) {
      auto const& op = script[idx];
      bool resp_ok = rd_drained + wr_drained >= op.wait_resp_min;
      bool gate_ok = !op.gate || op.gate();
      if (resp_ok && gate_ok) {
        if (op.is_write) {
          if (TryIssueWriteAddr(op.req_id, op.addr, burst_len, burst_size)) {
            LOGCHECK(op.wr_data && op.wr_data->size() == line_bytes,
                     "ScriptDriver: write op missing wr_data of correct size");
            pending_w_line = op.wr_data;
            pending_w_beats = beats_per_line;
            pending_w_beat_idx = 0;
            ++idx;
          }
        } else {
          if (TryIssueRead(op.req_id, op.addr, burst_len, burst_size)) {
            ++idx;
          }
        }
      }
    }

    while (pending_w_beats > 0) {
      uint64_t db = Cfg().data_bytes;
      auto beat = std::make_shared<std::vector<uint8_t>>(db, 0);
      std::memcpy(beat->data(),
                  pending_w_line->data() + pending_w_beat_idx * db, db);
      bool last = (pending_w_beats == 1);
      if (!TryPushWBeat(beat, last)) break;
      --pending_w_beats;
      ++pending_w_beat_idx;
      if (pending_w_beats == 0) {
        pending_w_line.reset();
      }
    }

    while (auto r = TryPopR()) {
      auto& a = read_asm[r->id];
      uint64_t db = Cfg().data_bytes;
      if (!a.buf) a.buf = std::make_shared<std::vector<uint8_t>>(line_bytes, 0);
      uint64_t off = a.beats_so_far * db;
      std::memcpy(a.buf->data() + off, r->data->data(), db);
      ++a.beats_so_far;
      if (r->last) {
        LOGCHECK(a.beats_so_far == beats_per_line,
                 "ScriptDriver: R last with wrong beat count");
        rd_results[r->id] = LineHead(*a.buf);
        read_asm.erase(r->id);
        ++rd_drained;
      }
    }

    while (auto b = TryPopB()) {
      (void)b;
      ++wr_drained;
    }
  }

  uint64_t Sent()       const { return idx; }
  uint64_t RdDrained()  const { return rd_drained; }
  uint64_t WrDrained()  const { return wr_drained; }
  const std::unordered_map<uint64_t, uint64_t>& Reads() const {
    return rd_results;
  }

 private:
  struct ReadAsm {
    std::shared_ptr<std::vector<uint8_t>> buf;
    uint64_t beats_so_far = 0;
  };
  uint64_t line_bytes;
  uint64_t master;
  std::vector<ScriptOp> script;
  uint64_t idx = 0, rd_drained = 0, wr_drained = 0;
  uint64_t beats_per_line = 0;
  uint8_t burst_len = 0, burst_size = 0;

  std::shared_ptr<std::vector<uint8_t>> pending_w_line;
  uint64_t pending_w_beats = 0;
  uint64_t pending_w_beat_idx = 0;

  std::unordered_map<uint64_t, ReadAsm> read_asm;
  std::unordered_map<uint64_t, uint64_t> rd_results;
};

class Llc_Test : public ::testing::Test {
 protected:
  static constexpr uint64_t kLine = 64;

  void SetUp() override {
    RT::Reset();
    RT::GetRecorder().Reset();
  }

  Llc::Config MakeCfg(uint64_t n_master = 2, uint64_t n_set = 4,
                      uint64_t n_way = 4) {
    Llc::Config c;
    c.line_bytes = kLine;
    c.n_way = n_way;
    c.n_master = n_master;
    c.n_llc_bytes = n_set * n_way * c.line_bytes;
    c.n_region = 4;
    c.n_mshr = 8;
    c.n_outstanding_dram = 8;
    c.q_maint = 4;
    c.ctrl_fifo_depth = 4;
    c.axi = Llc::MakeDefaultAxi();
    return c;
  }

  void RunUntil(ClockPtr clk, std::function<bool()> done_pred,
                uint64_t max_cycles = 8000) {
    bool* finished = new bool(false);
    RT::Launch(
        [clk, done_pred, max_cycles, finished]() {
          for (uint64_t i = 0; i < max_cycles; ++i) {
            clk->DelayCycle(1);
            if (done_pred()) { *finished = true; break; }
          }
          clk->Stop();
        },
        0);
    clk->Continue();
    RT::JoinAll();
    EXPECT_TRUE(*finished) << "RunUntil timed out after " << max_cycles;
    delete finished;
  }
};

TEST_F(Llc_Test, ColdMissThenHit) {
  ClockPtr clk = MakeClock(0, 10);
  auto cfg = MakeCfg();
  Llc llc(clk, "llc1", cfg);
  FakeDram dram(clk, &llc, cfg.line_bytes);
  LlcEventObserver obs(clk, &llc);

  dram.Write(0x1000, *MakeLine(cfg.line_bytes, 0xDEADBEEFULL));

  std::vector<ScriptOp> s = {
      {false, 0x1000, 1, nullptr, 0},
      {false, 0x1000, 2, nullptr, 1},
  };
  ScriptDriver drv(clk, &llc, 0, s);

  RunUntil(clk, [&]() { return drv.RdDrained() == 2; });

  EXPECT_EQ(drv.Reads().at(1), 0xDEADBEEFULL);
  EXPECT_EQ(drv.Reads().at(2), 0xDEADBEEFULL);
  EXPECT_EQ(dram.ReadsSeen(), 1u);

  EXPECT_EQ(obs.MissSum(), 1u);
  EXPECT_EQ(obs.HitSum(),  1u);
  int64_t miss_at = LlcEventObserver::FirstNonZero(obs.miss_events);
  int64_t hit_at  = LlcEventObserver::FirstNonZero(obs.hit_events);
  EXPECT_GE(miss_at, 0);
  EXPECT_GE(hit_at,  0);
  EXPECT_LT(miss_at, hit_at) << "hit must follow miss";
  EXPECT_EQ(obs.miss_events[miss_at], 1u);
  EXPECT_EQ(obs.hit_events[hit_at],   1u);
}

TEST_F(Llc_Test, WriteAllocateThenRead) {
  ClockPtr clk = MakeClock(0, 10);
  auto cfg = MakeCfg();
  Llc llc(clk, "llc2", cfg);
  FakeDram dram(clk, &llc, cfg.line_bytes);
  LlcEventObserver obs(clk, &llc);

  std::vector<ScriptOp> s = {
      {true,  0x2000, 10, MakeLine(cfg.line_bytes, 0xCAFEBABEULL), 0},
      {false, 0x2000, 11, nullptr, 1},
  };
  ScriptDriver drv(clk, &llc, 0, s);

  RunUntil(clk, [&]() {
    return drv.WrDrained() == 1 && drv.RdDrained() == 1;
  });

  EXPECT_EQ(drv.Reads().at(11), 0xCAFEBABEULL);
  EXPECT_EQ(obs.MissSum(), 1u);
  EXPECT_EQ(obs.HitSum(),  1u);
  EXPECT_LT(LlcEventObserver::FirstNonZero(obs.miss_events),
            LlcEventObserver::FirstNonZero(obs.hit_events));
}

TEST_F(Llc_Test, EvictionWritesBackDirtyLine) {
  ClockPtr clk = MakeClock(0, 10);
  auto cfg = MakeCfg(1, 4, 4);
  Llc llc(clk, "llc3", cfg);
  FakeDram dram(clk, &llc, cfg.line_bytes);
  LlcEventObserver obs(clk, &llc);

  uint64_t stride = cfg.line_bytes * cfg.n_llc_bytes /
                    (cfg.line_bytes * cfg.n_way);
  ASSERT_EQ(stride, 256u);
  uint64_t base = 0x10000;
  uint64_t evict_value = 0xA5A5A5A5ULL;

  std::vector<ScriptOp> s;
  for (uint64_t i = 0; i < cfg.n_way; ++i) {
    s.push_back({true, base + i * stride, 100 + i,
                 MakeLine(cfg.line_bytes, evict_value + i)});
  }
  s.push_back({false, base + cfg.n_way * stride, 200, nullptr});
  ScriptDriver drv(clk, &llc, 0, s);

  RunUntil(clk, [&]() {
    return drv.WrDrained() == cfg.n_way && drv.RdDrained() == 1 &&
           llc.DramOutstandingCount() == 0;
  });

  EXPECT_EQ(obs.MissSum(), cfg.n_way + 1);
  EXPECT_EQ(obs.HitSum(),  0u);
  EXPECT_GE(obs.WbSum(),   1u);
  int64_t first_wb = LlcEventObserver::FirstNonZero(obs.wb_events);
  EXPECT_GE(first_wb, LlcEventObserver::FirstNonZero(obs.miss_events));
  ASSERT_TRUE(dram.Has(base));
  EXPECT_EQ(LineHead(dram.Read(base)), evict_value);
}

TEST_F(Llc_Test, BypassRegionGoesDirectToDram) {
  ClockPtr clk = MakeClock(0, 10);
  auto cfg = MakeCfg();
  Llc llc(clk, "llc4", cfg);
  FakeDram dram(clk, &llc, cfg.line_bytes);
  LlcEventObserver obs(clk, &llc);

  llc.SetRegion(0, 0x80000000ULL, ~uint64_t(0xfffff), Llc::AttrMode::Bypass);
  dram.Write(0x80000000ULL, *MakeLine(cfg.line_bytes, 0xDEADULL));

  std::vector<ScriptOp> s = {
      {false, 0x80000000ULL, 1, nullptr},
      {true,  0x80000040ULL, 2, MakeLine(cfg.line_bytes, 0xBEEFULL)},
  };
  ScriptDriver drv(clk, &llc, 0, s);

  RunUntil(clk, [&]() {
    return drv.RdDrained() == 1 && drv.WrDrained() == 1 &&
           llc.DramOutstandingCount() == 0;
  });

  EXPECT_EQ(drv.Reads().at(1), 0xDEADULL);
  EXPECT_EQ(obs.HitSum(),    0u);
  EXPECT_EQ(obs.MissSum(),   0u);
  EXPECT_GE(obs.BypassSum(), 2u);
  ASSERT_TRUE(dram.Has(0x80000040ULL));
  EXPECT_EQ(LineHead(dram.Read(0x80000040ULL)), 0xBEEFULL);
}

TEST_F(Llc_Test, PinWayMaskRestrictsVictimChoice) {
  ClockPtr clk = MakeClock(0, 10);
  auto cfg = MakeCfg(1, 4, 4);
  Llc llc(clk, "llc5", cfg);
  FakeDram dram(clk, &llc, cfg.line_bytes);
  LlcEventObserver obs(clk, &llc);

  llc.SetRegion(0, 0x40000000ULL, ~uint64_t(0xfffff), Llc::AttrMode::WBWA,
                0x1);

  std::vector<ScriptOp> s;
  for (int i = 0; i < 4; ++i) {
    s.push_back({false, 0x40000000ULL + i * 256, uint64_t(i), nullptr});
  }
  ScriptDriver drv(clk, &llc, 0, s);

  RunUntil(clk, [&]() { return drv.RdDrained() == 4; });

  EXPECT_EQ(obs.MissSum(), 4u);
  EXPECT_EQ(obs.HitSum(),  0u);
  EXPECT_EQ(dram.ReadsSeen(), 4u);
}

TEST_F(Llc_Test, MshrMergeOnConcurrentMiss) {
  ClockPtr clk = MakeClock(0, 10);
  auto cfg = MakeCfg(2);
  Llc llc(clk, "llc6", cfg);
  FakeDram dram(clk, &llc, cfg.line_bytes, 16);
  LlcEventObserver obs(clk, &llc);
  dram.Write(0x3000, *MakeLine(cfg.line_bytes, 0xFEEDULL));

  std::vector<ScriptOp> s0 = {{false, 0x3000, 1, nullptr}};
  std::vector<ScriptOp> s1 = {{false, 0x3000, 2, nullptr}};
  ScriptDriver d0(clk, &llc, 0, s0);
  ScriptDriver d1(clk, &llc, 1, s1);

  RunUntil(clk, [&]() {
    return d0.RdDrained() == 1 && d1.RdDrained() == 1;
  });

  EXPECT_EQ(d0.Reads().at(1), 0xFEEDULL);
  EXPECT_EQ(d1.Reads().at(2), 0xFEEDULL);
  EXPECT_EQ(obs.MissSum(), 2u);
  EXPECT_EQ(obs.HitSum(),  0u);
  EXPECT_EQ(dram.ReadsSeen(), 1u);
}

TEST_F(Llc_Test, MaintFlushWritesBackThenInvalidates) {
  ClockPtr clk = MakeClock(0, 10);
  auto cfg = MakeCfg();
  Llc llc(clk, "llc7", cfg);
  FakeDram dram(clk, &llc, cfg.line_bytes);
  LlcEventObserver obs(clk, &llc);
  TestMaster tm(clk, &llc, 0, "tm7");

  bool done = false;
  uint64_t got_after_flush = 0;
  RT::Launch(
      [&]() {
        clk->DelayCycle(1);
        tm.WriteLine(1, 0x5000,
                     MakeLine(cfg.line_bytes, 0x1234ULL));
        tm.WaitB();

        uint64_t writes_before = dram.WritesSeen();
        llc.Flush(0x5000, cfg.line_bytes);
        while (dram.WritesSeen() == writes_before) clk->DelayCycle(1);

        ASSERT_TRUE(dram.Has(0x5000));
        EXPECT_EQ(LineHead(dram.Read(0x5000)), 0x1234ULL);

        uint64_t miss_before = llc.MissCount();
        tm.IssueRead(2, 0x5000);
        auto line = tm.PopReadLine();
        got_after_flush = LineHead(*line);
        EXPECT_GT(llc.MissCount(), miss_before);
        done = true;
      },
      0);
  RunUntil(clk, [&]() { return done; });

  EXPECT_EQ(got_after_flush, 0x1234ULL);
  EXPECT_EQ(obs.MaintSum(), 1u);
  EXPECT_GE(obs.WbSum(),    1u);
  int64_t wb_at    = LlcEventObserver::FirstNonZero(obs.wb_events);
  int64_t maint_at = LlcEventObserver::FirstNonZero(obs.maint_events);
  EXPECT_GE(wb_at, 0);
  EXPECT_GE(maint_at, 0);
  EXPECT_LE(wb_at, maint_at) << "writeback must enqueue before flush completes";
}

TEST_F(Llc_Test, MaintCleanWritesBackKeepsLineValid) {
  ClockPtr clk = MakeClock(0, 10);
  auto cfg = MakeCfg();
  Llc llc(clk, "llc8", cfg);
  FakeDram dram(clk, &llc, cfg.line_bytes);
  LlcEventObserver obs(clk, &llc);
  TestMaster tm(clk, &llc, 0, "tm8");

  bool done = false;
  RT::Launch(
      [&]() {
        clk->DelayCycle(1);
        tm.WriteLine(1, 0x6000,
                     MakeLine(cfg.line_bytes, 0xABCDULL));
        tm.WaitB();

        uint64_t writes_before = dram.WritesSeen();
        llc.Clean(0x6000, cfg.line_bytes);
        while (dram.WritesSeen() == writes_before) clk->DelayCycle(1);

        ASSERT_TRUE(dram.Has(0x6000));
        EXPECT_EQ(LineHead(dram.Read(0x6000)), 0xABCDULL);

        uint64_t hit_before = llc.HitCount();
        tm.IssueRead(2, 0x6000);
        (void)tm.PopReadLine();
        EXPECT_GT(llc.HitCount(), hit_before);
        done = true;
      },
      0);
  RunUntil(clk, [&]() { return done; });

  EXPECT_EQ(obs.MaintSum(), 1u);
  EXPECT_GE(obs.WbSum(),    1u);
  EXPECT_GE(obs.HitSum(),   1u);
}

TEST_F(Llc_Test, MaintPrefetchPullsLineInToCache) {
  ClockPtr clk = MakeClock(0, 10);
  auto cfg = MakeCfg();
  Llc llc(clk, "llc9", cfg);
  FakeDram dram(clk, &llc, cfg.line_bytes);
  LlcEventObserver obs(clk, &llc);
  TestMaster tm(clk, &llc, 0, "tm9");
  dram.Write(0x7000, *MakeLine(cfg.line_bytes, 0xC0DEULL));

  bool done = false;
  uint64_t got = 0;
  RT::Launch(
      [&]() {
        clk->DelayCycle(1);
        llc.Prefetch(0x7000, cfg.line_bytes);
        while (llc.ReadStat(Llc::kStatMaintBusy) != 0 ||
               llc.MshrOccupancy() != 0) {
          clk->DelayCycle(1);
        }

        uint64_t reads_before = dram.ReadsSeen();
        tm.IssueRead(1, 0x7000);
        auto line = tm.PopReadLine();
        got = LineHead(*line);
        EXPECT_EQ(dram.ReadsSeen(), reads_before);
        done = true;
      },
      0);
  RunUntil(clk, [&]() { return done; });

  EXPECT_EQ(got, 0xC0DEULL);
  EXPECT_EQ(obs.MaintSum(), 1u);
  EXPECT_EQ(obs.MissSum(),  0u);
  EXPECT_EQ(obs.HitSum(),   1u);
}

TEST_F(Llc_Test, MaintInvalidateDropsDirtyWithoutWriteback) {
  ClockPtr clk = MakeClock(0, 10);
  auto cfg = MakeCfg();
  Llc llc(clk, "llc10", cfg);
  FakeDram dram(clk, &llc, cfg.line_bytes);
  LlcEventObserver obs(clk, &llc);
  TestMaster tm(clk, &llc, 0, "tm10");

  constexpr uint64_t kOrig  = 0xAA00CC00ULL;
  constexpr uint64_t kDirty = 0xDD11EE22ULL;
  dram.Write(0x8000, *MakeLine(cfg.line_bytes, kOrig));

  bool done = false;
  uint64_t got = 0;
  RT::Launch(
      [&]() {
        clk->DelayCycle(1);
        tm.WriteLine(1, 0x8000,
                     MakeLine(cfg.line_bytes, kDirty));
        tm.WaitB();

        uint64_t wb_total_before = llc.WritebackCount();
        llc.Invalidate(0x8000, cfg.line_bytes);
        while (llc.ReadStat(Llc::kStatMaintBusy) != 0) clk->DelayCycle(1);
        EXPECT_EQ(llc.WritebackCount(), wb_total_before);
        EXPECT_EQ(LineHead(dram.Read(0x8000)), kOrig);

        tm.IssueRead(2, 0x8000);
        auto line = tm.PopReadLine();
        got = LineHead(*line);
        done = true;
      },
      0);
  RunUntil(clk, [&]() { return done; });
  EXPECT_EQ(got, kOrig);

  EXPECT_EQ(obs.MaintSum(), 1u);
  EXPECT_EQ(obs.WbSum(),    0u);
  EXPECT_EQ(obs.MissSum(),  2u);
}

TEST_F(Llc_Test, EndToEndStreamMultiDriver) {
  ClockPtr clk = MakeClock(0, 10);
  auto cfg = MakeCfg(2, 4, 4);
  Llc llc(clk, "llc_stream", cfg);
  FakeDram dram(clk, &llc, cfg.line_bytes, 6);

  constexpr int kN = 16;

  std::vector<uint64_t> a0(kN), a1(kN), p0(kN), p1(kN);
  for (int i = 0; i < kN; ++i) {
    a0[i] = 0x100000ULL + i * cfg.line_bytes;
    a1[i] = 0x200000ULL + i * cfg.line_bytes;
    p0[i] = 0xA0B0ULL + i;
    p1[i] = 0xC0D0ULL + i;
  }

  std::vector<ScriptOp> s0, s1;
  std::shared_ptr<ScriptDriver> d0_ptr, d1_ptr;
  auto& llc_ref = llc;

  auto idle_gate = [&]() {
    return llc_ref.DramOutstandingCount() == 0 &&
           llc_ref.MshrOccupancy() == 0 &&
           d0_ptr && d1_ptr &&
           d0_ptr->WrDrained() == uint64_t(kN) &&
           d1_ptr->WrDrained() == uint64_t(kN);
  };

  for (int i = 0; i < kN; ++i) {
    s0.push_back({true,  a0[i], uint64_t(i), MakeLine(cfg.line_bytes, p0[i]),
                  0, nullptr});
    s1.push_back({true,  a1[i], uint64_t(i), MakeLine(cfg.line_bytes, p1[i]),
                  0, nullptr});
  }
  for (int i = 0; i < kN; ++i) {
    s0.push_back({false, a0[i], uint64_t(kN + i), nullptr, 0, idle_gate});
    s1.push_back({false, a1[i], uint64_t(kN + i), nullptr, 0, idle_gate});
  }

  d0_ptr = std::make_shared<ScriptDriver>(clk, &llc, 0, std::move(s0));
  d1_ptr = std::make_shared<ScriptDriver>(clk, &llc, 1, std::move(s1));

  RunUntil(clk,
           [&]() {
             return d0_ptr->RdDrained() == kN && d1_ptr->RdDrained() == kN;
           },
           16000);

  int mismatches = 0;
  for (int i = 0; i < kN; ++i) {
    auto it0 = d0_ptr->Reads().find(kN + i);
    auto it1 = d1_ptr->Reads().find(kN + i);
    if (it0 == d0_ptr->Reads().end() || it0->second != p0[i]) ++mismatches;
    if (it1 == d1_ptr->Reads().end() || it1->second != p1[i]) ++mismatches;
  }
  EXPECT_EQ(mismatches, 0);
  EXPECT_EQ(d0_ptr->WrDrained(), uint64_t(kN));
  EXPECT_EQ(d1_ptr->WrDrained(), uint64_t(kN));
}

}

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
