#include "module/axi.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/clock.h"
#include "base/fifo.h"
#include "base/runtime.h"
#include "gtest/gtest.h"

using namespace latch;

namespace {

std::shared_ptr<std::vector<uint8_t>> PatternBeat(uint64_t addr,
                                                  uint64_t bytes) {
  auto v = std::make_shared<std::vector<uint8_t>>(bytes, 0);
  for (uint64_t i = 0; i < bytes; ++i) {
    (*v)[i] = static_cast<uint8_t>((addr + i) * 0x9E37u);
  }
  return v;
}

class MemSlave : public AxiSlave {
 public:
  enum class ROrder { InOrder, ReverseOrder };

  MemSlave(ClockPtr clock, const std::string& name, const Config& c,
           uint64_t mem_bytes)
      : AxiSlave(clock, name, c), mem(mem_bytes, 0) {}

  void Preload(uint64_t addr, const std::vector<uint8_t>& data) {
    for (uint64_t i = 0; i < data.size(); ++i) {
      if (addr + i < mem.size()) mem[addr + i] = data[i];
    }
  }
  uint8_t Peek(uint64_t a) const {
    return a < mem.size() ? mem[a] : 0;
  }

  void SetReadOrder(ROrder o) { r_order = o; }
  void SetAcceptW(bool v)     { accept_w = v; }

  void SetReadLatency(uint64_t cyc) { min_read_latency_cycles = cyc; }

  uint64_t ARsAccepted() const { return ars_accepted; }
  uint64_t AWsAccepted() const { return aws_accepted; }
  uint64_t WBeatsApplied() const { return w_beats_applied; }
  uint64_t RBeatsEmitted() const { return r_beats_emitted; }
  uint64_t BsEmitted()     const { return bs_emitted; }

  void Cycle() override {
    DelayCycle(1);
    ++cycle;
    PopAR();
    PopAW();
    AcceptW();
    EmitR();
    EmitB();
  }

 private:
  struct ReadJob {
    uint64_t id;
    uint64_t addr;
    uint8_t  len;
    uint8_t  size;
    AxiBurst burst;
    uint64_t arrival_cycle = 0;
    uint64_t beats_emitted = 0;
  };
  struct WriteJob {
    uint64_t id;
    uint64_t addr;
    uint8_t  len;
    uint8_t  size;
    AxiBurst burst;
    uint64_t beats_applied = 0;
  };

  static uint64_t BeatAddr(uint64_t base, uint8_t size, AxiBurst burst,
                           uint64_t beat_idx) {
    if (burst == AxiBurst::kFixed) return base;
    return base + (uint64_t(1) << size) * beat_idx;
  }
  uint64_t BeatBytes(uint8_t size) const {
    uint64_t b = uint64_t(1) << size;
    return b < Cfg().data_bytes ? b : Cfg().data_bytes;
  }

  void PopAR() {
    while (auto c = TryPopAR()) {
      read_q.push_back({c->id, c->addr, c->len, c->size, c->burst,
                        cycle, 0});
      ++ars_accepted;
    }
  }
  void PopAW() {
    while (auto c = TryPopAW()) {
      write_q.push_back({c->id, c->addr, c->len, c->size, c->burst, 0});
      ++aws_accepted;
    }
  }

  void AcceptW() {
    if (!accept_w) return;
    if (write_q.empty()) return;
    auto wb = TryPopW();
    if (!wb) return;
    auto& job = write_q.front();
    uint64_t bytes_per_beat = BeatBytes(job.size);
    uint64_t beat_byte_addr =
        BeatAddr(job.addr, job.size, job.burst, job.beats_applied);
    auto const& data = *wb->data;
    auto const* strb = wb->strb ? wb->strb.get() : nullptr;

    LOGCHECK(data.size() == Cfg().data_bytes,
             "MemSlave: W beat data size mismatch");
    for (uint64_t i = 0; i < bytes_per_beat; ++i) {
      bool valid = !strb || (*strb)[i] != 0;
      if (!valid) continue;
      uint64_t a = beat_byte_addr + i;
      if (a < mem.size()) mem[a] = data[i];
    }
    ++w_beats_applied;
    ++job.beats_applied;
    if (job.beats_applied > job.len) {
      LOGCHECK(wb->last,
               "MemSlave: write burst missing last on the final beat");
      pending_b.push_back(job.id);
      write_q.pop_front();
    }
  }

  void EmitR() {
    if (read_q.empty()) return;
    if (R().IsFull()) return;
    auto eligible = [&](const ReadJob& j) {
      return cycle - j.arrival_cycle >= min_read_latency_cycles;
    };
    ReadJob* job = nullptr;
    size_t job_idx = 0;
    if (r_order == ROrder::InOrder) {
      for (size_t i = 0; i < read_q.size(); ++i) {
        if (eligible(read_q[i])) { job = &read_q[i]; job_idx = i; break; }
      }
    } else {
      for (size_t i = read_q.size(); i > 0; --i) {
        if (eligible(read_q[i - 1])) {
          job = &read_q[i - 1]; job_idx = i - 1; break;
        }
      }
    }
    if (!job) return;
    uint64_t beat_byte_addr =
        BeatAddr(job->addr, job->size, job->burst, job->beats_emitted);
    auto data = std::make_shared<std::vector<uint8_t>>(Cfg().data_bytes, 0);
    uint64_t bytes_per_beat = BeatBytes(job->size);
    for (uint64_t i = 0; i < bytes_per_beat; ++i) {
      uint64_t a = beat_byte_addr + i;
      (*data)[i] = (a < mem.size()) ? mem[a] : 0;
    }
    bool is_last = (job->beats_emitted == job->len);
    TryPushR(job->id, data, is_last);
    ++r_beats_emitted;
    ++job->beats_emitted;
    if (is_last) {
      read_q.erase(read_q.begin() + job_idx);
    }
  }

  void EmitB() {
    if (pending_b.empty()) return;
    if (B().IsFull()) return;
    TryPushB(pending_b.front());
    pending_b.pop_front();
    ++bs_emitted;
  }

  std::vector<uint8_t> mem;
  std::deque<ReadJob>  read_q;
  std::deque<WriteJob> write_q;
  std::deque<uint64_t> pending_b;
  ROrder r_order = ROrder::InOrder;
  bool   accept_w = true;
  uint64_t cycle = 0;
  uint64_t min_read_latency_cycles = 0;

  uint64_t ars_accepted    = 0;
  uint64_t aws_accepted    = 0;
  uint64_t w_beats_applied = 0;
  uint64_t r_beats_emitted = 0;
  uint64_t bs_emitted      = 0;
};

class Axi_Test : public ::testing::Test {
 protected:
  void SetUp() override {
    RT::Reset();
    RT::GetRecorder().Reset();
  }

  static AxiConfig MakeCfg(uint64_t data_bytes = 8) {
    AxiConfig c;
    c.data_bytes = data_bytes;
    c.ar_depth = 4;
    c.r_depth  = 8;
    c.aw_depth = 4;
    c.w_depth  = 8;
    c.b_depth  = 4;
    return c;
  }

  void RunUntil(ClockPtr clk, std::function<bool()> done_pred,
                uint64_t max_cycles = 4000) {
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

TEST_F(Axi_Test, SingleBeatRead) {
  ClockPtr clk = MakeClock(0, 10);
  AxiConfig cfg = MakeCfg(8);

  AxiMaster m(clk, "axi_m", cfg);
  MemSlave  s(clk, "axi_s", cfg, 256);
  m.Bind(s);

  auto expected = PatternBeat(0x40, cfg.data_bytes);
  s.Preload(0x40, *expected);

  bool done = false;
  std::shared_ptr<std::vector<uint8_t>> got;
  uint64_t got_id = 0, got_resp = ~0u;
  bool got_last = false;

  RT::Launch(
      [&]() {
        while (!m.TryIssueRead(7, 0x40, 0,
                               3))
          clk->DelayCycle(1);
        while (true) {
          auto r = m.TryPopR();
          if (r) {
            got      = r->data;
            got_id   = r->id;
            got_resp = r->resp;
            got_last = r->last;
            done = true;
            break;
          }
          clk->DelayCycle(1);
        }
      },
      0);

  RunUntil(clk, [&]() { return done; });

  ASSERT_TRUE(got != nullptr);
  EXPECT_EQ(got_id,   7u);
  EXPECT_EQ(got_resp, static_cast<uint64_t>(AxiResp::kOkay));
  EXPECT_TRUE(got_last);
  EXPECT_EQ(*got, *expected);
  EXPECT_EQ(s.ARsAccepted(), 1u);
  EXPECT_EQ(s.RBeatsEmitted(), 1u);
}

TEST_F(Axi_Test, BurstFourBeatsRead) {
  ClockPtr clk = MakeClock(0, 10);
  AxiConfig cfg = MakeCfg(8);

  AxiMaster m(clk, "axi_m", cfg);
  MemSlave  s(clk, "axi_s", cfg, 512);
  m.Bind(s);

  uint64_t base = 0x80;
  std::vector<std::shared_ptr<std::vector<uint8_t>>> expected(4);
  for (uint64_t i = 0; i < 4; ++i) {
    expected[i] = PatternBeat(base + i * cfg.data_bytes, cfg.data_bytes);
    s.Preload(base + i * cfg.data_bytes, *expected[i]);
  }

  bool done = false;
  std::vector<std::shared_ptr<std::vector<uint8_t>>> got;
  std::vector<bool> got_last;
  uint64_t got_count = 0;

  RT::Launch(
      [&]() {
        while (!m.TryIssueRead(3, base, 3, 3))
          clk->DelayCycle(1);
        while (got_count < 4) {
          auto r = m.TryPopR();
          if (r) {
            EXPECT_EQ(r->id, 3u);
            got.push_back(r->data);
            got_last.push_back(r->last);
            ++got_count;
          } else {
            clk->DelayCycle(1);
          }
        }
        done = true;
      },
      0);

  RunUntil(clk, [&]() { return done; });

  ASSERT_EQ(got.size(), 4u);
  for (uint64_t i = 0; i < 4; ++i) {
    EXPECT_EQ(*got[i], *expected[i]) << "beat " << i;
    EXPECT_EQ(got_last[i], i == 3) << "last bit on beat " << i;
  }
  EXPECT_EQ(s.RBeatsEmitted(), 4u);
}

TEST_F(Axi_Test, BurstWriteThenRead) {
  ClockPtr clk = MakeClock(0, 10);
  AxiConfig cfg = MakeCfg(8);

  AxiMaster m(clk, "axi_m", cfg);
  MemSlave  s(clk, "axi_s", cfg, 512);
  m.Bind(s);

  uint64_t base = 0xC0;
  std::vector<std::shared_ptr<std::vector<uint8_t>>> wbeats(4);
  for (uint64_t i = 0; i < 4; ++i) {
    wbeats[i] = PatternBeat(base + i * cfg.data_bytes, cfg.data_bytes);
  }

  bool done = false;
  bool b_seen = false;
  uint64_t b_id = ~0u, b_resp = ~0u;
  std::vector<std::shared_ptr<std::vector<uint8_t>>> rbeats;

  RT::Launch(
      [&]() {

        while (!m.TryIssueWriteAddr(5, base, 3, 3))
          clk->DelayCycle(1);

        for (uint64_t i = 0; i < 4; ++i) {
          while (!m.TryPushWBeat(wbeats[i], i == 3))
            clk->DelayCycle(1);
        }

        while (true) {
          auto b = m.TryPopB();
          if (b) {
            b_id   = b->id;
            b_resp = b->resp;
            b_seen = true;
            break;
          }
          clk->DelayCycle(1);
        }

        while (!m.TryIssueRead(6, base, 3, 3))
          clk->DelayCycle(1);
        while (rbeats.size() < 4) {
          auto r = m.TryPopR();
          if (r) {
            EXPECT_EQ(r->id, 6u);
            rbeats.push_back(r->data);
          } else {
            clk->DelayCycle(1);
          }
        }
        done = true;
      },
      0);

  RunUntil(clk, [&]() { return done; });

  EXPECT_TRUE(b_seen);
  EXPECT_EQ(b_id,   5u);
  EXPECT_EQ(b_resp, static_cast<uint64_t>(AxiResp::kOkay));
  ASSERT_EQ(rbeats.size(), 4u);
  for (uint64_t i = 0; i < 4; ++i) {
    EXPECT_EQ(*rbeats[i], *wbeats[i]) << "readback beat " << i;
  }
  EXPECT_EQ(s.BsEmitted(), 1u);
  EXPECT_EQ(s.WBeatsApplied(), 4u);
}

TEST_F(Axi_Test, OutOfOrderReadByID) {
  ClockPtr clk = MakeClock(0, 10);
  AxiConfig cfg = MakeCfg(8);

  AxiMaster m(clk, "axi_m", cfg);
  MemSlave  s(clk, "axi_s", cfg, 1024);
  m.Bind(s);
  s.SetReadOrder(MemSlave::ROrder::ReverseOrder);

  s.SetReadLatency(4);

  auto data_a = PatternBeat(0x100, cfg.data_bytes);
  auto data_b = PatternBeat(0x200, cfg.data_bytes);
  s.Preload(0x100, *data_a);
  s.Preload(0x200, *data_b);

  std::unordered_map<uint64_t, std::shared_ptr<std::vector<uint8_t>>> got;
  std::vector<uint64_t> arrival_order;
  bool done = false;

  RT::Launch(
      [&]() {

        while (!m.TryIssueRead(10, 0x100, 0, 3))
          clk->DelayCycle(1);
        while (!m.TryIssueRead(11, 0x200, 0, 3))
          clk->DelayCycle(1);

        clk->DelayCycle(2);
        while (got.size() < 2) {
          auto r = m.TryPopR();
          if (r) {
            arrival_order.push_back(r->id);
            got[r->id] = r->data;
          } else {
            clk->DelayCycle(1);
          }
        }
        done = true;
      },
      0);

  RunUntil(clk, [&]() { return done; });

  ASSERT_EQ(got.size(), 2u);
  ASSERT_EQ(arrival_order.size(), 2u);
  EXPECT_EQ(arrival_order[0], 11u) << "expected R(id=11) first under "
                                      "ReverseOrder mode";
  EXPECT_EQ(arrival_order[1], 10u);
  EXPECT_EQ(*got[10], *data_a);
  EXPECT_EQ(*got[11], *data_b);
}

TEST_F(Axi_Test, WriteBackpressureDoesNotStallReads) {
  ClockPtr clk = MakeClock(0, 10);
  AxiConfig cfg = MakeCfg(8);

  AxiMaster m(clk, "axi_m", cfg);
  MemSlave  s(clk, "axi_s", cfg, 256);
  m.Bind(s);
  s.SetAcceptW(false);

  auto expected = PatternBeat(0x10, cfg.data_bytes);
  s.Preload(0x10, *expected);

  bool done = false;
  bool got_r = false;
  std::shared_ptr<std::vector<uint8_t>> got_data;
  uint64_t w_pushed = 0;
  uint64_t w_full_observed = 0;

  RT::Launch(
      [&]() {

        auto pad = std::make_shared<std::vector<uint8_t>>(cfg.data_bytes, 0);
        for (uint64_t i = 0; i < cfg.w_depth; ++i) {
          if (m.TryPushWBeat(pad, false)) ++w_pushed;
        }

        EXPECT_EQ(w_pushed, cfg.w_depth);
        if (m.W().IsFull()) ++w_full_observed;

        while (!m.TryIssueRead(1, 0x10, 0, 3))
          clk->DelayCycle(1);
        while (true) {
          auto r = m.TryPopR();
          if (r) {
            got_data = r->data;
            got_r = true;
            break;
          }

          if (m.W().IsFull()) ++w_full_observed;
          clk->DelayCycle(1);
        }
        done = true;
      },
      0);

  RunUntil(clk, [&]() { return done; });

  EXPECT_TRUE(got_r);
  ASSERT_TRUE(got_data != nullptr);
  EXPECT_EQ(*got_data, *expected);
  EXPECT_GE(w_full_observed, 1u)
      << "W fifo should have remained full during the read";
  EXPECT_EQ(s.WBeatsApplied(), 0u)
      << "slave was configured to refuse W beats";
}

}
