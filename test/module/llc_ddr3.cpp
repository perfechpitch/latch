#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "gtest/gtest.h"
#include "module/axi.h"
#include "module/ddr3.h"
#include "module/llc.h"

using namespace latch;

namespace {

std::shared_ptr<std::vector<uint8_t>> MakeLine(uint64_t line_bytes,
                                               uint64_t pat) {
  auto v = std::make_shared<std::vector<uint8_t>>(line_bytes, 0);
  std::memcpy(v->data(), &pat, sizeof(pat));
  return v;
}

class TestMaster : public AxiMaster {
 public:
  TestMaster(ClockPtr clock, Llc* l, uint64_t m, const std::string& name)
      : AxiMaster(clock, name, Llc::MakeDefaultAxi()), clk_(clock) {
    Bind(*l->AxiSlavePort(m));
    beats_per_line_ = l->BeatsPerLine();
    burst_len_  = l->BurstLen();
    burst_size_ = l->BurstSize();
    line_bytes_ = beats_per_line_ * Cfg().data_bytes;
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
    for (uint64_t i = 0; i < beats_per_line_; ++i) {
      auto beat = std::make_shared<std::vector<uint8_t>>(db, 0);
      std::memcpy(beat->data(), line->data() + i * db, db);
      bool last = (i + 1 == beats_per_line_);
      while (!TryPushWBeat(beat, last)) clk_->DelayCycle(1);
    }
  }

  std::shared_ptr<std::vector<uint8_t>> PopReadLine() {
    uint64_t db = Cfg().data_bytes;
    auto line = std::make_shared<std::vector<uint8_t>>(line_bytes_, 0);
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
  uint64_t beats_per_line_ = 0;
  uint64_t line_bytes_ = 0;
  uint8_t  burst_len_ = 0;
  uint8_t  burst_size_ = 0;
};

class AxiPortTracer : public ClkModule {
 public:
  AxiPortTracer(ClockPtr clock, const std::string& name, AxiBase* axi)
      : ClkModule(clock), axi_(axi) {
    RegisterId(name, 0);
  }
  void Cycle() override {
    DelayCycle(1);
    if (!axi_->IsBound()) return;

    TraceAddr("ar", axi_->AR());
    TraceAddr("aw", axi_->AW());

    {
      auto& f = axi_->R();
      const uint64_t cnt = f.ValidCount();
      TraceIfChanged("r_cnt", cnt);
      TraceIfChanged("r_id",  cnt > 0 ? static_cast<uint64_t>(f.Front().id) : 0ULL);
    }
    {
      auto& f = axi_->B();
      const uint64_t cnt = f.ValidCount();
      TraceIfChanged("b_cnt", cnt);
      TraceIfChanged("b_id",  cnt > 0 ? static_cast<uint64_t>(f.Front().id) : 0ULL);
    }
    TraceIfChanged("w_cnt", axi_->W().ValidCount());
  }

 private:
  void TraceAddr(const char* tag, Fifo<AxiAddrPkt>& f) {
    const uint64_t cnt = f.ValidCount();
    uint64_t addr = 0, id = 0, len = 0;
    if (cnt > 0) {
      const auto& head = f.Front();
      addr = static_cast<uint64_t>(head.addr);
      id   = static_cast<uint64_t>(head.id);
      len  = static_cast<uint64_t>(head.len);
    }
    TraceIfChanged(std::string(tag) + "_cnt",  cnt);
    TraceIfChanged(std::string(tag) + "_addr", addr);
    TraceIfChanged(std::string(tag) + "_id",   id);
    TraceIfChanged(std::string(tag) + "_len",  len);
  }

  void TraceIfChanged(const std::string& name, uint64_t v) {
    auto it = last_.find(name);
    if (it == last_.end() || it->second != v) {
      Trace(name, v);
      last_[name] = v;
    }
  }

  AxiBase* axi_;
  std::unordered_map<std::string, uint64_t> last_;
};

void RunUntil(ClockPtr clk, std::function<bool()> done_pred,
              uint64_t max_cycles = 20000) {
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

}

class LlcDdr3Test : public ::testing::Test {
 protected:
  void SetUp() override {
    RT::Reset();
    RT::GetRecorder().Reset();
  }
};

TEST_F(LlcDdr3Test, TwoPortsReadThenWriteSweep) {
  ClockPtr clk = MakeClock(0, 10);

  Llc::Config cfg;
  cfg.line_bytes = 64;
  cfg.n_way = 4;
  cfg.n_master = 2;
  cfg.n_llc_bytes = 4 * cfg.n_way * cfg.line_bytes;
  cfg.n_region = 4;
  cfg.n_mshr = 4;
  cfg.n_outstanding_dram = 4;
  cfg.q_maint = 2;
  cfg.ctrl_fifo_depth = 2;
  cfg.axi = Llc::MakeDefaultAxi();

  constexpr uint64_t kAddrBase = 0x1000;
  constexpr uint64_t kLines    = 32;
  const uint64_t kPatBase = 0xC0FFEE00ULL;

  bool done = false;
  uint64_t miss_snap = 0, hit_snap = 0;
  uint64_t id_counter = 0;

  {
    Llc  llc (clk, "llc",  cfg);
    Ddr3 dram(clk, "ddr3", false);
    llc.AxiMasterPort()->Bind(*dram.AxiPort());

    TestMaster tm0(clk, &llc, 0, "tm0");
    TestMaster tm1(clk, &llc, 1, "tm1");
    TestMaster* tms[2] = {&tm0, &tm1};

    AxiPortTracer tr_s0(clk, "llc_axi_s0",     llc.AxiSlavePort(0).get());
    AxiPortTracer tr_s1(clk, "llc_axi_s1",     llc.AxiSlavePort(1).get());
    AxiPortTracer tr_m (clk, "llc_axi_m_dram", llc.AxiMasterPort().get());

    auto AddrOf = [&](uint64_t i) {
      return kAddrBase + i * cfg.line_bytes;
    };

    RT::Launch(
        [&]() {

          for (uint64_t i = 0; i < kLines; ++i) {
            uint64_t reader = i & 1;
            tms[reader]->IssueRead(++id_counter, AddrOf(i));
            (void)tms[reader]->PopReadLine();
          }

          for (uint64_t i = 0; i < kLines; ++i) {
            uint64_t writer = i & 1;
            tms[writer]->WriteLine(++id_counter, AddrOf(i),
                                   MakeLine(cfg.line_bytes, kPatBase + i));
            tms[writer]->WaitB();
          }

          miss_snap = llc.MissCount();
          hit_snap  = llc.HitCount();
          done = true;
        },
        0);

    RunUntil(clk, [&]() { return done; }, 400000);
  }

  RT::FlushRecorder();

  EXPECT_EQ(miss_snap, 2 * kLines);
  EXPECT_EQ(hit_snap,  0u);
}

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
