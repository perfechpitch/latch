#include "module/ddr3.h"

#include <iostream>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/clock.h"
#include "base/fifo.h"
#include "base/module.h"
#include "base/recorder.h"
#include "base/runtime.h"
#include "gtest/gtest.h"
#include "module/axi.h"
#include "utils/trace_reader.h"

using namespace latch;

namespace {

class Ddr3Driver : public AxiMaster {
 public:
  Ddr3Driver(ClockPtr clock, Ddr3* d, std::vector<uint64_t> rs,
             std::vector<uint64_t> ws)
      : AxiMaster(clock, "ddr3_driver", Ddr3::MakeDefaultAxiConfig()),
        dram(d),
        reads(std::move(rs)),
        writes(std::move(ws)) {
    Bind(*dram->AxiPort());
    uint64_t tx = static_cast<uint64_t>(dram->GetTxBytes());
    uint64_t db = Cfg().data_bytes;
    LOGCHECK(tx % db == 0, "Ddr3Driver: tx_bytes must be multiple of "
                           "axi data_bytes");
    beats_per_tx = tx / db;
    burst_len  = static_cast<uint8_t>(beats_per_tx - 1);
    burst_size = LogBytes(db);
  }

  void Cycle() override {
    DelayCycle(1);

    if (read_idx < reads.size()) {
      uint64_t id = read_idx;
      if (TryIssueRead(id, reads[read_idx], burst_len, burst_size)) {
        read_id_addr[id] = reads[read_idx];
        ++read_idx;
      }
    }

    while (pending_w_beats_left > 0) {
      auto pad =
          std::make_shared<std::vector<uint8_t>>(Cfg().data_bytes, 0);
      bool last = (pending_w_beats_left == 1);
      if (!TryPushWBeat(pad, last)) break;
      --pending_w_beats_left;
    }
    if (pending_w_beats_left == 0 && write_idx < writes.size()) {
      uint64_t id = write_idx;
      if (TryIssueWriteAddr(id, writes[write_idx], burst_len, burst_size)) {
        write_id_addr[id] = writes[write_idx];
        pending_w_beats_left = beats_per_tx;
        ++write_idx;
      }
    }

    while (auto r = TryPopR()) {
      if (r->last) {
        auto it = read_id_addr.find(r->id);
        LOGCHECK(it != read_id_addr.end(),
                 "Ddr3Driver: R for unknown read id");
        read_done.insert(it->second);
        read_id_addr.erase(it);
      }
    }

    while (auto b = TryPopB()) {
      auto it = write_id_addr.find(b->id);
      LOGCHECK(it != write_id_addr.end(),
               "Ddr3Driver: B for unknown write id");
      write_done.insert(it->second);
      write_id_addr.erase(it);
    }

    total_reads_done_snap  = dram->TotalReadsDone();
    total_writes_done_snap = dram->TotalWritesDone();
    inflight_reads_snap    = dram->InflightReads();
    inflight_writes_snap   = dram->InflightWrites();
  }

  size_t sent_reads()  const { return read_idx; }
  size_t sent_writes() const { return write_idx; }

  Ddr3* dram;
  std::vector<uint64_t> reads;
  std::vector<uint64_t> writes;
  size_t read_idx = 0;
  size_t write_idx = 0;
  uint64_t beats_per_tx = 0;
  uint8_t  burst_len = 0;
  uint8_t  burst_size = 0;
  uint64_t pending_w_beats_left = 0;
  std::unordered_map<uint64_t, uint64_t> read_id_addr;
  std::unordered_map<uint64_t, uint64_t> write_id_addr;
  std::unordered_set<uint64_t> read_done;
  std::unordered_set<uint64_t> write_done;
  uint64_t total_reads_done_snap  = 0;
  uint64_t total_writes_done_snap = 0;
  uint64_t inflight_reads_snap    = 0;
  uint64_t inflight_writes_snap   = 0;

 private:
  static uint8_t LogBytes(uint64_t b) {
    uint8_t s = 0;
    while ((uint64_t(1) << s) < b) ++s;
    return s;
  }
};

}

class Ddr3Test : public ::testing::Test {
 protected:
  void SetUp() override {
    RT::Reset();
    RT::GetRecorder().Reset();
  }
};

TEST_F(Ddr3Test, ReadRequestsRespondWithMatchingAddresses) {
  ClockPtr clk = MakeClock(0, 10);
  Ddr3 dram(clk, "ddr3_rd", true);
  const int tx = dram.GetTxBytes();
  ASSERT_EQ(tx, 64);

  std::vector<uint64_t> reads;
  for (int i = 0; i < 8; ++i) {
    reads.push_back(static_cast<uint64_t>(i) * 0x10000 + i * tx);
  }

  Ddr3Driver drv(clk, &dram, reads, {});
  clk->Continue(40000);
  RT::JoinAll();

  EXPECT_EQ(drv.sent_reads(), reads.size());
  EXPECT_EQ(drv.read_done.size(), reads.size());
  for (uint64_t a : reads) {
    EXPECT_TRUE(drv.read_done.count(a))
        << "missing read response for 0x" << std::hex << a;
  }
  EXPECT_EQ(drv.total_reads_done_snap, reads.size());
  EXPECT_EQ(drv.inflight_reads_snap, 0u);

  std::cout << "\n=== ramulator stats (reads) ===\n"
            << dram.DumpStats() << std::endl;
}

TEST_F(Ddr3Test, WriteRequestsRespondWithMatchingAddresses) {
  ClockPtr clk = MakeClock(0, 10);
  Ddr3 dram(clk, "ddr3_wr", true);
  const int tx = dram.GetTxBytes();

  std::vector<uint64_t> writes;
  for (int i = 0; i < 4; ++i) {
    writes.push_back(0x80000000ULL + static_cast<uint64_t>(i) * tx);
  }

  Ddr3Driver drv(clk, &dram, {}, writes);
  clk->Continue(40000);
  RT::JoinAll();

  EXPECT_EQ(drv.sent_writes(), writes.size());
  EXPECT_EQ(drv.write_done.size(), writes.size());
  for (uint64_t a : writes) {
    EXPECT_TRUE(drv.write_done.count(a))
        << "missing write response for 0x" << std::hex << a;
  }
  EXPECT_EQ(drv.total_writes_done_snap, writes.size());
  EXPECT_EQ(drv.inflight_writes_snap, 0u);

  std::cout << "\n=== ramulator stats (writes) ===\n"
            << dram.DumpStats() << std::endl;
}

TEST_F(Ddr3Test, MixedReadWriteStream) {
  ClockPtr clk = MakeClock(0, 10);
  Ddr3 dram(clk, "ddr3_mix", false);
  const int tx = dram.GetTxBytes();

  std::vector<uint64_t> reads, writes;
  for (int i = 0; i < 4; ++i) {
    reads.push_back(0x20000ULL + static_cast<uint64_t>(i) * tx);
    writes.push_back(0x40000ULL + static_cast<uint64_t>(i) * tx);
  }

  Ddr3Driver drv(clk, &dram, reads, writes);
  clk->Continue(40000);
  RT::JoinAll();

  EXPECT_EQ(drv.read_done.size(),  reads.size());
  EXPECT_EQ(drv.write_done.size(), writes.size());
}

TEST_F(Ddr3Test, LongInterleavedReadWriteStream) {
  ClockPtr clk = MakeClock(0, 10);
  Ddr3 dram(clk, "ddr3_stream", true);
  const int tx = dram.GetTxBytes();

  constexpr int kN = 24;
  std::vector<uint64_t> reads, writes;
  for (int i = 0; i < kN; ++i) {
    reads.push_back(0x0000ULL + static_cast<uint64_t>(i) * 0x800 + i * tx);
    writes.push_back(0x80000000ULL + static_cast<uint64_t>(i) * 0x800 +
                     i * tx);
  }

  Ddr3Driver drv(clk, &dram, reads, writes);
  clk->Continue(80000);
  RT::JoinAll();

  EXPECT_EQ(drv.sent_reads(),  reads.size());
  EXPECT_EQ(drv.sent_writes(), writes.size());
  EXPECT_EQ(drv.read_done.size(),  reads.size());
  EXPECT_EQ(drv.write_done.size(), writes.size());
  EXPECT_EQ(drv.inflight_reads_snap,  0u);
  EXPECT_EQ(drv.inflight_writes_snap, 0u);

  std::cout << "\n=== ramulator stats (interleaved stream) ===\n"
            << dram.DumpStats() << std::endl;
}

TEST_F(Ddr3Test, BinaryTraceCapturesSignals) {
  const std::string prefix = RT::GetRecorder().PathPrefix();
  constexpr int kReads = 32;
  constexpr int kWrites = 16;

  {
    ClockPtr clk = MakeClock(0, 10);
    Ddr3 dram(clk, "ddr3_trace", false);
    const int tx = dram.GetTxBytes();

    std::vector<uint64_t> reads, writes;
    for (int i = 0; i < kReads; ++i) {
      reads.push_back(static_cast<uint64_t>(i) * 0x1000 + i * tx);
    }
    for (int i = 0; i < kWrites; ++i) {
      writes.push_back(0x40000000ULL + static_cast<uint64_t>(i) * 0x800 +
                       i * tx);
    }

    Ddr3Driver drv(clk, &dram, reads, writes);
    clk->Continue(80000);
    RT::JoinAll();

    EXPECT_EQ(drv.read_done.size(),  reads.size());
    EXPECT_EQ(drv.write_done.size(), writes.size());

    std::cout << "\n=== ramulator stats (trace test) ===\n"
              << dram.DumpStats() << std::endl;
  }
  RT::FlushRecorder();

  const std::string trace_path = prefix + ".trace";
  std::cout << "\n=== trace artifacts ===\n  file: " << trace_path << "\n"
            << std::endl;

  auto data = TraceSlurp(trace_path);
  ASSERT_GT(data.size(), 16u);

  auto meta = TraceParseMeta(data);
  uint64_t module_id = 0;
  for (auto const& r : meta) {
    if (r.name == "ddr3_trace" && r.parent_id == 0) {
      module_id = r.id;
      break;
    }
  }
  ASSERT_NE(module_id, 0u) << "expected 'ddr3_trace' module in meta";

  const std::vector<std::string> expected_signals = {
      "ar_q_depth",       "aw_q_depth",     "w_q_depth",
      "r_q_depth",        "b_q_depth",      "inflight_reads",
      "inflight_writes",  "total_reads_done", "total_writes_done",
  };
  std::map<std::string, uint64_t> sig_id;
  for (auto const& r : meta) {
    if (r.parent_id != module_id) continue;
    sig_id[r.name] = r.id;
  }
  for (auto const& s : expected_signals) {
    EXPECT_TRUE(sig_id.count(s)) << "missing trace signal: " << s;
  }

  auto idx = TraceParseDataIndex(data);
  for (auto const& s : expected_signals) {
    if (!sig_id.count(s)) continue;
    auto it = idx.find(sig_id[s]);
    ASSERT_NE(it, idx.end()) << "no segments for " << s;
    ASSERT_FALSE(it->second.empty()) << s;
  }

  auto& reads_done_segs = idx[sig_id["total_reads_done"]];
  ASSERT_FALSE(reads_done_segs.empty());
  EXPECT_EQ(reads_done_segs.back().v_max, static_cast<uint64_t>(kReads));

  auto& writes_done_segs = idx[sig_id["total_writes_done"]];
  ASSERT_FALSE(writes_done_segs.empty());
  EXPECT_EQ(writes_done_segs.back().v_max, static_cast<uint64_t>(kWrites));

  EXPECT_GT(idx[sig_id["inflight_reads"]].back().v_max, 0u);
}

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
