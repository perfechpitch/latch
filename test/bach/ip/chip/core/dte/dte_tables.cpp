// DTE 自己持有的那几张表，以及出核那一层的仲裁。
//
// Hmem 按 stream_id 索引，硬件包头与软件包头合并成一项；stream_cache
// 只跟随不分配； 本级 Reduce credit 按用户记。 出核四个通道对 Router
// 那一个口轮转，一个包的几拍不许被别的包插进来。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "bach/ip/chip/core/dte/dma_xbar.h"
#include "bach/ip/chip/core/dte/hmem.h"
#include "bach/ip/chip/core/dte/out_arb.h"
#include "base/clock.h"
#include "base/runtime.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

} // namespace

// Hmem 一项就是硬件包头加软件包头：硬件只改 core_mask，软件改那 16 B。
TEST(BachDteHmem, HeaderTablesAreMergedPerStream) {
  ClockPtr clk = MakeClock(0, kPeriod);
  Hmem h(clk, "hmem", 0, false);

  h.SetCoreMask(3, 0x00FF);
  h.Entry(3).sw_header[0] = 0xAB;
  EXPECT_EQ(h.Entry(3).core_mask, 0x00FFu);
  EXPECT_EQ(h.Entry(3).sw_header[0], 0xAB);
  // 各 stream 各一项，互不影响。
  EXPECT_EQ(h.Entry(4).core_mask, 0u);
  EXPECT_EQ(h.Entry(4).sw_header[0], 0);
  RT::Reset();
}

// path_id 到 task_id 的反查表由 TS 那一侧配好，DTE 只读。
TEST(BachDteHmem, PathTaskMapIsReadOnlyHere) {
  ClockPtr clk = MakeClock(0, kPeriod);
  Hmem h(clk, "hmem", 0, false);
  h.PreloadPathTask(7, 3);
  h.PreloadPathTask(8, 5);
  EXPECT_EQ(h.PathTask(7), 3u);
  EXPECT_EQ(h.PathTask(8), 5u);
  EXPECT_EQ(h.PathTask(9), 0u) << "没配过的回 0";
  RT::Reset();
}

// stream_cache 只跟随不分配：按 Router 送回来的 release 记账，方向各记各的。
TEST(BachDteHmem, StreamCacheOnlyFollows) {
  ClockPtr clk = MakeClock(0, kPeriod);
  Hmem h(clk, "hmem", 0, false);

  EXPECT_FALSE(h.CacheHolds(kDirLeft, 41));
  h.FollowStreamCredit(kDirLeft, 41);
  EXPECT_TRUE(h.CacheHolds(kDirLeft, 41));
  EXPECT_FALSE(h.CacheHolds(kDirRight, 41)) << "方向各记各的";
  // 同一个用户跟随两次不占两项。
  h.FollowStreamCredit(kDirLeft, 41);
  h.DropStreamCredit(kDirLeft, 41);
  EXPECT_FALSE(h.CacheHolds(kDirLeft, 41));
  RT::Reset();
}

namespace {

// 四个通道往仲裁器里放拍，收 Router 那一侧发出来的。
class ArbHarness : public BachModule {
public:
  struct Beat {
    uint64_t at = 0;
    uint64_t lane = 0;
    uint64_t user = 0;
    bool last = false;
  };

  ArbHarness(ClockPtr c, DteOutArb &target)
      : BachModule(c, "harness"), arb(target) {}

  std::vector<Beat> beats;

  struct Got {
    uint64_t at = 0, user = 0;
    bool last = false;
  };
  std::vector<Got> out;

protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做：先看上一拍发出去的。
    CoreDataView d = ReadCoreData(arb.Out());
    if (d.valid && d.seq != last_out_seq) {
      last_out_seq = d.seq;
      out.push_back({now, d.msg ? d.msg->user_id : 0, d.last});
    }
    arb.Out().DriveReady(true);

    std::vector<bool> drove(DteOutArb::kOutNum, false);
    for (auto const &b : beats) {
      if (b.at != now)
        continue;
      auto m = std::make_shared<Message>();
      m->user_id = b.user;
      m->size = 256;
      arb.LanePort(b.lane)->Drive(256, b.last, !b.last, 0, m);
      drove[b.lane] = true;
    }
    for (uint64_t i = 0; i < DteOutArb::kOutNum; ++i) {
      if (!drove[i])
        arb.LanePort(i)->Idle();
    }
    arb.RunStep();
  }

private:
  DteOutArb &arb;
  uint64_t last_out_seq = 0;
};

} // namespace

// 四个出核通道轮转用 Router 那一个口。
TEST(BachDteOutArb, FourLanesTakeTurns) {
  std::vector<uint64_t> users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteOutArb arb(clk, "arb", 0, false);
    ArbHarness h(clk, arb);
    // 四个通道各放一个单拍的包。
    for (uint64_t i = 0; i < DteOutArb::kOutNum; ++i) {
      h.beats.push_back({2, i, 100 + i, /*last=*/true});
    }
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    for (auto const &g : h.out)
      users.push_back(g.user);
  }
  RT::Reset();
  ASSERT_EQ(users.size(), 4u) << "四个通道的包都要发出去";
  std::vector<uint64_t> sorted = users;
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(sorted[0], 100u);
  EXPECT_EQ(sorted[3], 103u);
}

// 一个包的几拍不许被别的包插进来：授权粘在一个通道上直到它发完带 tlast 的那拍。
TEST(BachDteOutArb, OnePacketIsNotInterleaved) {
  std::vector<uint64_t> users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteOutArb arb(clk, "arb", 0, false);
    ArbHarness h(clk, arb);
    // 通道 0 是一个三拍的包，通道 1 同时也想发。
    h.beats = {{2, 0, 100, false},
               {3, 0, 100, false},
               {4, 0, 100, true},
               {2, 1, 200, true},
               {6, 1, 201, true}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    for (auto const &g : h.out)
      users.push_back(g.user);
  }
  RT::Reset();
  ASSERT_GE(users.size(), 4u);
  // 通道 0 那个包的三拍要连着走完，中间不夹别人的。
  EXPECT_EQ(users[0], 100u);
  EXPECT_EQ(users[1], 100u);
  EXPECT_EQ(users[2], 100u);
  EXPECT_EQ(users[3], 200u);
}

TEST(BachDteHmem, AllGroupsAndItemsPreserveIndependentHardwareBytes) {
  auto clock = MakeClock(0, kPeriod);
  Hmem table(clock, "table", 0, false);
  HmemEntry first;
  first.core_mask = 0x1234;
  first.hardware_used = 0xa5;
  first.sw_header.fill(0x11);
  table.WriteIndexed(15 * 32, HeaderToBytes(first));
  auto second = first;
  second.hardware_used = 0x5a;
  second.core_mask = 0xabcd;
  second.sw_header.fill(0x22);
  table.WriteIndexed(15 * 32 + 31, HeaderToBytes(second));
  EXPECT_EQ(table.Indexed(15 * 32).hardware_used, 0xa5u);
  EXPECT_EQ(table.Indexed(511).hardware_used, 0x5au);
  EXPECT_EQ(table.Indexed(15 * 32).core_mask, 0xabcdu);
  EXPECT_EQ(table.Indexed(15 * 32).sw_header, second.sw_header);
  table.WriteSoftware(0x3000 + 15 * 32 + 28, 0x01020304);
  EXPECT_EQ(table.ReadSoftware(0x3000 + 15 * 32 + 28), 0x01020304u);
  EXPECT_EQ(table.Indexed(511).hardware_used, 1u);
  table.WriteSoftware(0x33fc, 0xffffffff);
  EXPECT_EQ(table.Indexed(0).hardware_used, 0u);
  RT::Reset();
}

TEST(BachDteHmem, SoftwareLayoutMatchesHardwareIndicesAndReservedWords) {
  auto clock = MakeClock(0, kPeriod);
  Hmem table(clock, "table", 0, false);
  for (uint64_t g = 0; g < 16; ++g) {
    for (uint64_t k = 0; k < 32; k += 4) {
      const uint32_t word = uint32_t(g * 32 + k) * 0x01010101u;
      table.WriteSoftware(0x3000 + g * 32 + k, word);
      EXPECT_EQ(table.ReadSoftware(0x3000 + g * 32 + k), word);
      for (uint64_t byte = 0; byte < 4; ++byte)
        EXPECT_EQ(table.Indexed(g * 32 + k + byte).hardware_used,
                  uint8_t(word >> (8 * byte)));
    }
    table.WriteSoftware(0x3200 + g * 32, 0xffff0000u | (g + 1));
    EXPECT_EQ(table.ReadSoftware(0x3200 + g * 32), g + 1);
    for (uint64_t n = 0; n < 4; ++n) {
      uint32_t word = 0x12345678u + uint32_t(g * 4 + n);
      table.WriteSoftware(0x3204 + g * 32 + n * 4, word);
      EXPECT_EQ(table.ReadSoftware(0x3204 + g * 32 + n * 4), word);
    }
    for (uint64_t off = 20; off < 32; off += 4) {
      table.WriteSoftware(0x3200 + g * 32 + off, 0xffffffffu);
      EXPECT_EQ(table.ReadSoftware(0x3200 + g * 32 + off), 0u);
    }
  }
  for (uint64_t address = 0x3400; address < 0x4000; address += 4) {
    table.WriteSoftware(address, 0xffffffffu);
    EXPECT_EQ(table.ReadSoftware(address), 0u);
  }
  RT::Reset();
}

TEST(BachDteHmem, HeaderMemoryLayoutIsExactly32Bytes) {
  HmemEntry header;
  header.raw.fill(0x7e);
  header.sw_header.fill(0x55);
  header.pid = 0xa1;
  header.uid = 0x2345;
  header.packet_length = 0x6789;
  header.core_mask = 0xabcd;
  header.hardware_used = 0x5a;
  auto bytes = HeaderToBytes(header);
  ASSERT_EQ(bytes.size(), 32u);
  EXPECT_EQ(bytes[31], 0xa1);
  EXPECT_EQ(bytes[22], 0x45);
  EXPECT_EQ(bytes[23], 0x23);
  EXPECT_EQ(bytes[24], 0x89);
  EXPECT_EQ(bytes[25], 0x67);
  EXPECT_EQ(bytes[26], 0x5a);
  EXPECT_EQ(bytes[28], 0xcd);
  EXPECT_EQ(bytes[29], 0xab);
  EXPECT_EQ(bytes[30], 0x7e);
  EXPECT_EQ(bytes[0], 0x55);
  EXPECT_EQ(HeaderToBytes(BytesToHeader(bytes)), bytes);
}

TEST(BachDteOut, ExhaustedVcDoesNotBlockOtherVcOrRequireReady) {
  EnsureSlots();
  auto clock = MakeClock(0, kPeriod);
  DteOutArb arb(clock, "arb", 0, false);
  auto credit = std::make_shared<DteTxCreditPort>(clock);
  arb.AttachCredits(credit, VcDepth{1, 1, 0, 0}, 0);
  struct Driver : BachModule {
    DteOutArb &arb;
    std::shared_ptr<DteTxCreditPort> credit;
    std::vector<uint64_t> sent;
    Driver(ClockPtr c, DteOutArb &a, std::shared_ptr<DteTxCreditPort> p)
        : BachModule(c, "driver"), arb(a), credit(p) {}
    void Step() override {
      auto output = ReadCoreData(arb.Out());
      if (output.valid)
        sent.push_back(output.vc);
      credit->returned_mask = CycleNow() == 10 ? 1 : 0;
      arb.Out().DriveReady(false);
      for (uint64_t lane = 0; lane < 4; ++lane)
        arb.LanePort(lane)->Idle();
      if (CycleNow() == 2 || CycleNow() == 4) {
        auto m = std::make_shared<Message>();
        m->seq = CycleNow();
        arb.LanePort(0)->Drive(8, true, true, 0, m);
      }
      if (CycleNow() == 5) {
        auto m = std::make_shared<Message>();
        m->seq = 5;
        arb.LanePort(1)->Drive(8, true, true, 1, m);
      }
      arb.RunStep();
    }
  } driver(clock, arb, credit);
  clock->Continue(20);
  RT::JoinAll();
  EXPECT_EQ(driver.sent, (std::vector<uint64_t>{0, 1, 0}));
  EXPECT_EQ(arb.Sent(), 3u);
  EXPECT_TRUE(arb.Quiescent());
  RT::Reset();
}

TEST(BachDteXbar, PairedReadyAndSeparatedResponsesRemainOneTransaction) {
  EnsureSlots();
  auto clock = MakeClock(0, kPeriod);
  DteXbar xbar(clock, "xbar", 0, false);
  struct Driver : BachModule {
    DteXbar &xbar;
    uint64_t sent_at = 0, requests = 0, responses = 0;
    ByteBlock result;
    Driver(ClockPtr c, DteXbar &x) : BachModule(c, "driver"), xbar(x) {}
    void Step() override {
      uint64_t now = CycleNow();
      auto lane = xbar.LaneCmem(0);
      if (lane->RspValid()) {
        ++responses;
        if (lane->RspData())
          result = *lane->RspData();
      }
      if (now == 2)
        lane->ReadPair(0);
      else
        lane->IdleReq();
      auto request = ReadMemReq(xbar.CmemRd());
      if (request.valid) {
        ++requests;
        sent_at = now;
        EXPECT_GE(now, 9u);
      }
      uint64_t mask = sent_at && now == sent_at + 2   ? 1
                      : sent_at && now == sent_at + 7 ? 2
                                                      : 0;
      xbar.CmemRd().DrivePairResponse(
          true, mask,
          mask ? std::make_shared<ByteBlock>(128, mask == 1 ? 0x11 : 0x22)
               : nullptr);
      xbar.CmemRd().DrivePairReady(now < 8 ? 1 : 3);
      xbar.CmemWr().DriveSlave(true, false, nullptr);
      xbar.MmemRd().DriveSlave(true, false, nullptr);
      xbar.MmemWr().DriveSlave(true, false, nullptr);
      xbar.RunStep();
    }
  } driver(clock, xbar);
  clock->Continue(30);
  RT::JoinAll();
  EXPECT_EQ(driver.requests, 1u);
  EXPECT_EQ(driver.responses, 1u);
  ASSERT_EQ(driver.result.size(), 256u);
  EXPECT_EQ(driver.result[0], 0x11);
  EXPECT_EQ(driver.result[127], 0x11);
  EXPECT_EQ(driver.result[128], 0x22);
  EXPECT_EQ(driver.result[255], 0x22);
  EXPECT_TRUE(xbar.Quiescent());
  RT::Reset();
}


class HeaderBudgetHarness : public BachModule {
public:
  HeaderBudgetHarness(ClockPtr clock, Hmem &table, bool writing)
      : BachModule(clock, "driver"), target(table), writes(writing) {}
  uint64_t peak_responses = 0, total_responses = 0;
  bool read_data_correct = true;
protected:
  void Step() override {
    uint64_t returned = 0;
    for (uint64_t lane = 0; lane < 5; ++lane) {
      auto port = target.LanePort(lane);
      if (port->RspValid()) {
        ++returned;
        if (!writes) {
          auto data = port->RspData();
          read_data_correct &= data && data->size() == 32 &&
                               (*data)[26] == 0x20 + lane;
        }
      }
      if (CycleNow() == 2 &&
          (writes ? (lane == 0 || lane == 4) : lane != 0)) {
        if (writes)
          port->Write(lane, std::make_shared<ByteBlock>(32, uint8_t(0x40 + lane)));
        else
          port->Read(lane, 32);
      } else port->IdleReq();
    }
    peak_responses = std::max(peak_responses, returned);
    total_responses += returned;
    target.RunStep();
  }
private:
  Hmem &target;
  bool writes;
};
TEST(BachDteHmem, HeaderFourToOneReadPortHasOneGrantPerCycle) {
  EnsureSlots();
  auto clock = MakeClock(0, kPeriod);
  Hmem table(clock, "header", 0, false);
  for (uint64_t lane = 1; lane < 5; ++lane) {
    ByteBlock bytes(32, 0);
    bytes[26] = uint8_t(0x20 + lane);
    table.WriteIndexed(lane, bytes);
  }
  HeaderBudgetHarness driver(clock, table, false);
  clock->Continue(30);
  RT::JoinAll();
  EXPECT_EQ(driver.peak_responses, 1u);
  EXPECT_EQ(driver.total_responses, 4u);
  EXPECT_TRUE(driver.read_data_correct);
  RT::Reset();
}
TEST(BachDteHmem, HeaderTwoToOneWritePortHasOneGrantPerCycle) {
  EnsureSlots();
  auto clock = MakeClock(0, kPeriod);
  Hmem table(clock, "header", 0, false);
  HeaderBudgetHarness driver(clock, table, true);
  clock->Continue(30);
  RT::JoinAll();
  EXPECT_EQ(driver.peak_responses, 1u);
  EXPECT_EQ(driver.total_responses, 2u);
  EXPECT_EQ(table.Indexed(0).hardware_used, 0x40u);
  EXPECT_EQ(table.Indexed(4).hardware_used, 0x44u);
  RT::Reset();
}
