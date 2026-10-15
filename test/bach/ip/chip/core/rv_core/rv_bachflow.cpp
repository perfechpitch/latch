// Bounded execution verifies the alias, not just its initial zero contents.
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "bach/ip/chip/core/memory/core_mem.h"
#include "bach/ip/chip/core/memory/share_mem.h"
#include "bach/ip/chip/core/rv_core/rv_core.h"
#include "base/clock.h"
#include "base/runtime.h"

using namespace latch;
using namespace latch::bach;

namespace {
class RvEnvironment : public BachModule {
 public:
  RvEnvironment(ClockPtr c, RvCore& target, uint64_t entry)
      : BachModule(c, "environment"), rv(target), pc(entry) {}
  uint64_t cmem_ops = 0, completions = 0;
  bool sent = false;
 protected:
  void Step() override {
    if (rv.Done().Valid() && !done_prev) ++completions;
    done_prev = rv.Done().Valid();
    auto req = ReadMemReq(rv.Cmem());
    if (req.valid && req.seq != last_cm) { last_cm = req.seq; ++cmem_ops; }
    rv.Cmem().DriveSlave(true, false, nullptr);
    rv.Smem().DriveSlave(true, false, nullptr);
    rv.Hdr().DriveSlave(true, false, nullptr);
    rv.DsaCfg().DriveReady(true);
    if (!sent && CycleNow() >= 2) {
      rv.Cmd().Drive(pc, 1, 2, 101, 255, true, 1);
      if (rv.Cmd().Ready()) sent = true;
    } else {
      rv.Cmd().Idle();
    }
    rv.RunStep();
  }
 private:
  RvCore& rv;
  uint64_t pc, last_cm = 0;
  bool done_prev = false;
};
ByteBlock Words(std::initializer_list<uint32_t> words) {
  ByteBlock bytes;
  for (auto word : words)
    for (int i = 0; i < 4; ++i) bytes.push_back(uint8_t(word >> (8 * i)));
  return bytes;
}
void LoadDataHex(RvCore &rv, uint32_t offset, ByteBlock const &bytes) {
  uint32_t address = kDtcmAliasBase + offset;
  std::ostringstream text;
  auto record = [&](uint16_t low, uint8_t type, ByteBlock const &body) {
    uint8_t sum = body.size() + (low >> 8) + (low & 255) + type;
    text << ':' << std::hex << std::setfill('0') << std::setw(2) << body.size()
         << std::setw(4) << low << std::setw(2) << unsigned(type);
    for (auto b : body) { text << std::setw(2) << unsigned(b); sum += b; }
    text << std::setw(2) << unsigned(uint8_t(-sum)) << '\n';
  };
  record(0, 4, ByteBlock{uint8_t(address >> 24), uint8_t(address >> 16)});
  record(uint16_t(address), 0, bytes);
  record(0, 1, {});
  std::string path = "/tmp/rv-bachflow-data.hex";
  { std::ofstream out(path); out << text.str(); }
  rv.LoadImage(path);
}
}

TEST(RvBachflow, HexBssAndAbi) {
  RT::Reset(8, 8);
  RvCore rv(MakeClock(0, 1), "rv", RvUnit::kDte);
  EXPECT_EQ(rv.Exec().Sp(), kDtcmBase + kDtcmSize);
  rv.Exec().SetBachflowAbi();
  EXPECT_EQ(rv.Exec().Sp(), 0x102000u);
  LoadDataHex(rv, 0, ByteBlock{1, 2, 0, 0});
  EXPECT_EQ(rv.Exec().PeekByte(kDtcmAliasBase), 1u);
  EXPECT_EQ(rv.Exec().PeekByte(kDtcmBase), 1u);
  EXPECT_EQ(rv.Exec().PeekByte(kDtcmAliasBase + 2), 0u);
  EXPECT_DEATH(rv.PokeItcm(4095, ByteBlock{1, 2}), "越界");
}

TEST(RvBachflow, AliasStoreAndLoadExecuteWithoutCoreMemTraffic) {
  RT::Reset(8, 8);
  auto clk = MakeClock(0, 1);
  RvCore rv(clk, "rv", RvUnit::kDte);
  rv.Exec().SetBachflowAbi();
  LoadDataHex(rv, 4, ByteBlock{0xa5, 0xa5, 0xa5, 0xa5});
  // lui a0,0x100; addi a1,zero,0x78; sw a1,4(a0); lw a2,4(a0); task_done.
  rv.PokeItcm(0x100, Words({0x00100537, 0x07800593, 0x00b52223,
                             0x00452603, 0x8000200b}));
  RvEnvironment env(clk, rv, 0x100);
  clk->Continue(100);
  RT::JoinAll();
  ASSERT_TRUE(env.sent);
  EXPECT_EQ(env.completions, 1u) << "pc=" << rv.Exec().Pc()
      << " insts=" << rv.Exec().Insts() << " a2=" << rv.Exec().Gpr(12)
      << " alias=" << unsigned(rv.Exec().PeekByte(kDtcmAliasBase + 4));
  EXPECT_EQ(rv.Exec().Gpr(12), 0x78u);
  EXPECT_EQ(rv.Exec().PeekByte(kDtcmBase + 4), 0x78u);
  EXPECT_EQ(rv.Exec().PeekByte(kDtcmAliasBase + 4), 0x78u);
  EXPECT_EQ(env.cmem_ops, 0u);
}

TEST(RvBachflow, CompressedTimingDecodesIntegerDependenciesAndOffsets) {
  // c.lw a1,0(a0); c.lw a2,4(a0); c.sw a1,4(a0).
  EXPECT_EQ(RvTimingInst(0x410c), 0x00052583u);
  EXPECT_EQ(RvTimingInst(0x4150), 0x00452603u);
  EXPECT_EQ(RvTimingInst(0xc14c), 0x00b52223u);
  EXPECT_EQ(RvIssueResOf(0x410c), kResMem);
  auto u = RvDecodeRegs(0x410c);
  EXPECT_TRUE(u.has_rs1); EXPECT_EQ(u.rs1, 10u);
  EXPECT_TRUE(u.has_rd); EXPECT_EQ(u.rd, 11u);
  EXPECT_FALSE(u.has_rs2);
  // c.lwsp a0,12(sp); c.swsp a1,12(sp).
  EXPECT_EQ(RvTimingInst(0x4532), 0x00c12503u);
  EXPECT_EQ(RvTimingInst(0xc62e), 0x00b12623u);
  // c.addi a1,1; c.slli a1,5; c.add a1,a2; c.mv a1,a2.
  EXPECT_EQ(RvTimingInst(0x0585), 0x00158593u);
  EXPECT_EQ(RvTimingInst(0x0596), 0x00559593u);
  EXPECT_EQ(RvTimingInst(0x95b2), 0x00c585b3u);
  EXPECT_EQ(RvTimingInst(0x85b2), 0x00c005b3u);
  EXPECT_EQ(RvIssueResOf(0x8082), kResCtrl);
  EXPECT_EQ(RvDecodeRegs(0x8082).rs1, 1u);
}

TEST(RvBachflow, CompressedAliasLoadsAndStoresAreLocalAndComplete) {
  RT::Reset(8, 8);
  auto clk = MakeClock(0, 1);
  RvCore rv(clk, "rv", RvUnit::kDte);
  rv.Exec().SetBachflowAbi();
  LoadDataHex(rv, 4, ByteBlock{0xa5, 0xa5, 0xa5, 0xa5});
  // lui a0,0x100; addi a1,zero,120; c.sw a1,4(a0); c.lw a2,4(a0); task_done 1.
  ByteBlock image = Words({0x00100537, 0x07800593});
  image.insert(image.end(), {0x4c, 0xc1, 0x50, 0x41});
  auto done = Words({0x8000200b});
  image.insert(image.end(), done.begin(), done.end());
  rv.PokeItcm(0x100, image);
  RvEnvironment env(clk, rv, 0x100);
  clk->Continue(100);
  RT::JoinAll();
  EXPECT_EQ(env.completions, 1u);
  EXPECT_EQ(rv.Exec().Gpr(12), 120u);
  EXPECT_EQ(rv.Exec().PeekByte(kDtcmBase + 4), 120u);
  EXPECT_EQ(env.cmem_ops, 0u);
}

TEST(RvBachflow, LaterCompressedWriteWaitsForOlderAsyncLoad) {
  RT::Reset(8, 8);
  auto clk = MakeClock(0, 1);
  RvCore rv(clk, "rv", RvUnit::kMu);
  // lui a1,0x48300; c.lw a0,0(a1); c.li a2,1; c.li a0,7;
  // c.addi a0,0 waits for the old read, then task_done 1.
  ByteBlock image = Words({0x483005b7});
  image.insert(image.end(), {0x88, 0x41, 0x05, 0x46, 0x1d, 0x45, 0x01, 0x05});
  auto done = Words({0x8000200b});
  image.insert(image.end(), done.begin(), done.end());
  rv.PokeItcm(0x100, image);
  ShareMem sm(clk, "sm");
  rv.AttachSmem(sm.PortPtr(kSmemMuRv));
  sm.Poke(0, Words({99}));
  class WriteEnvironment : public BachModule {
   public:
    WriteEnvironment(ClockPtr clock, RvCore& target)
        : BachModule(clock, "write_environment"), rv(target) {}
    uint64_t dones = 0;
   protected:
    void Step() override {
      if (rv.Done().Valid()) ++dones;
      if (CycleNow() >= 2 && !sent) {
        rv.Cmd().Drive(0x100, 0, 0, 101, 255, false, 1);
        if (rv.Cmd().Ready()) sent = true;
      } else { rv.Cmd().Idle(); }
      rv.DsaCfg().DriveReady(true);
      rv.RunStep();
    }
   private:
    RvCore& rv;
    bool sent = false;
  } env(clk, rv);
  clk->Continue(100);
  RT::JoinAll();
  ASSERT_EQ(env.dones, 1u);
  EXPECT_EQ(rv.Exec().Gpr(10), 7u);
}

TEST(RvBachflow, CompilerProducedRvcFlagTaskSetsUserAndCompletes) {
  char const* image = std::getenv("BACHFLOW_RVC_IMAGE");
  if (!image) GTEST_SKIP() << "Set BACHFLOW_RVC_IMAGE to the emitted MU flag task image";
  RT::Reset(8, 8);
  auto clk = MakeClock(0, 1);
  RvCore rv(clk, "rv", RvUnit::kMu);
  rv.Exec().SetBachflowAbi();
  std::ifstream input(image, std::ios::binary);
  ASSERT_TRUE(input.good());
  ByteBlock bytes(std::istreambuf_iterator<char>{input}, {});
  rv.PokeItcm(0, bytes);
  ShareMem sm(clk, "sm");
  CoreMem cm(clk, "cm");
  rv.AttachSmem(sm.PortPtr(kSmemMuRv));
  rv.AttachCmem(cm.PortPtr(kCmemMuRd));
  sm.Poke(0x1000, Words({1, 0}));
  cm.Poke(8, ByteBlock{101, 0});
  class FlagEnvironment : public BachModule {
   public:
    FlagEnvironment(ClockPtr clock, RvCore& target)
        : BachModule(clock, "flag_environment"), rv(target) {}
    uint64_t dones = 0, user = 0;
   protected:
    void Step() override {
      if (rv.Done().Valid()) { ++dones; user = rv.Done().user_id.Get(); }
      if (CycleNow() >= 2 && !sent) {
        rv.Cmd().Drive(0x12, 0, 0, 0, 255, false, 1);
        if (rv.Cmd().Ready()) sent = true;
      } else { rv.Cmd().Idle(); }
      rv.DsaCfg().DriveReady(true);
      rv.RunStep();
    }
   private:
    RvCore& rv;
    bool sent = false;
  } env(clk, rv);
  clk->Continue(400);
  RT::JoinAll();
  EXPECT_EQ(env.dones, 1u) << "pc=" << rv.Exec().Pc() << " insts=" << rv.Exec().Insts();
  EXPECT_EQ(env.user, 101u);
  EXPECT_EQ(sm.Peek(0x1004, 4), (ByteBlock{1, 0, 0, 0}));
}

TEST(RvBachflow, LegacyCoreMemInsideAliasRequiresExplicitBachflowAbi) {
  for (int selection = 0; selection < 3; ++selection) {
    for (bool store : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "selection=" << selection << " store=" << store);
      RT::Reset(8, 8);
      auto clk = MakeClock(0, 1);
      RvCore rv(clk, "rv", RvUnit::kDte);
      if (selection != 0) rv.Exec().SetBachflowAbi();
      if (selection == 2) rv.Exec().SetAbi(RvAbi::kLegacy);
      uint8_t old_dtcm = rv.Exec().PeekByte(kDtcmBase);
      // lui a0,0x100; addi a1,zero,120; sw OR lw at 0x100000; task_done.
      rv.PokeItcm(0x100, Words({0x00100537, 0x07800593,
          store ? 0x00b52023u : 0x00052603u, 0x8000200b}));
      RvEnvironment env(clk, rv, 0x100);
      clk->Continue(100);
      RT::JoinAll();
      ASSERT_TRUE(env.sent);
      EXPECT_EQ(env.cmem_ops, selection == 1 ? 0u : 1u);
      if (store) {
        EXPECT_EQ(rv.Exec().PeekByte(kDtcmBase),
                  selection == 1 ? uint8_t{120} : old_dtcm);
      }
    }
  }
}

TEST(RvBachflow, AddressImmediatelyOutsideAliasStillUsesCoreMem) {
  RT::Reset(8, 8);
  auto clk = MakeClock(0, 1);
  RvCore rv(clk, "rv", RvUnit::kDte);
  rv.PokeItcm(0x100, Words({0x00102537, 0x00052023, 0x0000200b}));
  RvEnvironment env(clk, rv, 0x100);
  clk->Continue(100);
  RT::JoinAll();
  ASSERT_TRUE(env.sent);
  EXPECT_EQ(env.cmem_ops, 1u);
}
