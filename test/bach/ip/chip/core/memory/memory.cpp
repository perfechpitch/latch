// 三块存储的行为基线：端到端拍数、bank 仲裁、数据存得对不对。
//
// 步 3 的判据：各 master 端口的端到端拍数等于参数表；同 bank 冲突按优先级授予。

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/memory/core_mem.h"
#include "bach/ip/chip/core/memory/matrix_mem.h"
#include "bach/ip/chip/core/memory/share_mem.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

// 在指定拍发一笔，按 valid/ready 握手：没看见 ready 就原地保持。
class MemDriver : public BachModule {
 public:
  MemDriver(ClockPtr c, MemPort& p, uint64_t fire_at, uint64_t address,
            uint64_t nbytes, ByteBlockPtr data = nullptr)
      : BachModule(c, "drv"), port(p), at(fire_at), addr(address),
        bytes(nbytes), wdata(std::move(data)) {}

  uint64_t issue_cycle = 0;   // 请求被接走的那一拍
  uint64_t rsp_cycle = 0;     // 响应回来的那一拍
  ByteBlockPtr got;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    if (port.RspValid() && rsp_cycle == 0) {
      rsp_cycle = now;
      got = port.RspData();
    }
    if (!issued && now >= at) {
      // ready 是存储上一拍锁存的，看见了才算这一笔被接走
      if (asserted && port.Ready()) {
        issued = true;
        issue_cycle = now;
      }
      if (wdata) {
        port.Write(addr, wdata);
      } else {
        port.Read(addr, bytes);
      }
      asserted = true;
    } else {
      port.IdleReq();
    }
  }

 private:
  MemPort& port;
  uint64_t at, addr, bytes;
  ByteBlockPtr wdata;
  bool issued = false, asserted = false;
};

// Logic64 在主线程读到的是 t=0 的值，计数要在协程内抄下来。
class ConflictProbe : public BachModule {
 public:
  ConflictProbe(ClockPtr c, BankedMem& target)
      : BachModule(c, "probe"), mem(target) {}

  uint64_t conflicts = 0;

 protected:
  void Step() override { conflicts = mem.Conflicts(); }

 private:
  BankedMem& mem;
};

ByteBlockPtr Bytes(std::vector<uint8_t> v) {
  return std::make_shared<ByteBlock>(std::move(v));
}

}  // namespace

// 写进去再读出来，字节要一样。
TEST(BachCoreMem, WriteThenRead) {
  ByteBlockPtr got;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreMem cm(clk, "cmem");
    MemDriver wr(clk, cm.Port(kCmemDteWr), 1, 512, 4, Bytes({1, 2, 3, 4}));
    MemDriver rd(clk, cm.Port(kCmemMuRd), 40, 512, 4);
    clk->Continue(120 * kPeriod);
    RT::JoinAll();
    got = rd.got;
  }
  RT::Reset();
  ASSERT_TRUE(got != nullptr);
  ASSERT_EQ(got->size(), 4u);
  EXPECT_EQ((*got)[0], 1u);
  EXPECT_EQ((*got)[3], 4u);
}

// 没写过的地方读出来是全 0，不是未初始化的随机值：比对结果不能依赖没写过的段。
TEST(BachCoreMem, UntouchedReadsZero) {
  ByteBlockPtr got;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreMem cm(clk, "cmem");
    MemDriver rd(clk, cm.Port(kCmemVuRd), 1, 4096, 8);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    got = rd.got;
  }
  RT::Reset();
  ASSERT_TRUE(got != nullptr);
  for (uint8_t b : *got) EXPECT_EQ(b, 0u);
}

// 端到端拍数按各 master 那一侧的口径：DTE 13T、MU 16T、VU 14T。
TEST(BachCoreMem, PerMasterLatency) {
  struct Case {
    uint64_t port;
    uint64_t want;
  };
  std::vector<Case> cases = {{kCmemDteRd, 13}, {kCmemMuRd, 16},
                             {kCmemVuRd, 14}};
  for (auto const& c : cases) {
    uint64_t issue = 0, rsp = 0;
    {
      ClockPtr clk = MakeClock(0, kPeriod);
      CoreMem cm(clk, "cmem");
      MemDriver rd(clk, cm.Port(c.port), 1, 0, 4);
      clk->Continue(120 * kPeriod);
      RT::JoinAll();
      issue = rd.issue_cycle;
      rsp = rd.rsp_cycle;
    }
    RT::Reset();
    ASSERT_GT(rsp, issue) << "port " << c.port;
    EXPECT_EQ(rsp - issue, c.want) << "port " << c.port;
  }
}

// 同 bank 撞车时按优先级授予：MU 先走，VU 排在它后面。
TEST(BachCoreMem, SameBankPriority) {
  uint64_t mu_rsp = 0, vu_rsp = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreMem cm(clk, "cmem");
    // 同一个地址，必然同 bank
    MemDriver mu(clk, cm.Port(kCmemMuRd), 1, 0, 4);
    MemDriver vu(clk, cm.Port(kCmemVuRd), 1, 0, 4);
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    mu_rsp = mu.rsp_cycle;
    vu_rsp = vu.rsp_cycle;
  }
  RT::Reset();
  ASSERT_GT(mu_rsp, 0u);
  ASSERT_GT(vu_rsp, 0u);
  // MU 优先级更高，先被授予；VU 晚一拍才轮到
  EXPECT_LT(mu_rsp - 16u, vu_rsp - 14u);
}

// 不同 bank 不互相挡，两个 master 同拍都能走。
TEST(BachCoreMem, DifferentBanksRunInParallel) {
  uint64_t a = 0, b = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreMem cm(clk, "cmem");
    MemDriver d0(clk, cm.Port(kCmemDteRd), 1, 0, 4);        // bank 0
    MemDriver d1(clk, cm.Port(kCmemReissue), 1, 128, 4);  // bank 1
    clk->Continue(120 * kPeriod);
    RT::JoinAll();
    a = d0.issue_cycle;
    b = d1.issue_cycle;
  }
  RT::Reset();
  EXPECT_EQ(a, b);
}

// Matrix Mem 的硬约束：同一 bank 不许两个 master 同时访问。
//
// 默认撞上就停（LOGCHECK 失败），因为真硬件上被让路的那一笔直接丢弃、DTE 没有
// 重传通路，丢一笔结果就错。这里把停下关掉，验证冲突确实被检出、计了数。
TEST(BachMatrixMem, SameBankIsDetected) {
  uint64_t conflicts = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    MatrixMem mm(clk, "mmem");
    mm.RelaxConflictHalt();
    MemDriver mu(clk, mm.Port(kMmemMu), 1, 0, 4);
    MemDriver dte(clk, mm.Port(kMmemDteRd), 1, 0, 4);
    ConflictProbe probe(clk, mm);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    conflicts = probe.conflicts;
  }
  RT::Reset();
  EXPECT_GT(conflicts, 0u);
}

// Matrix Mem 读 8T、写 9T。
TEST(BachMatrixMem, DteReadWriteLatency) {
  uint64_t rd_lat = 0, wr_lat = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    MatrixMem mm(clk, "mmem");
    MemDriver rd(clk, mm.Port(kMmemDteRd), 1, 0, 4);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    rd_lat = rd.rsp_cycle - rd.issue_cycle;
  }
  RT::Reset();
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    MatrixMem mm(clk, "mmem");
    MemDriver wr(clk, mm.Port(kMmemDteWr), 1, 0, 4, Bytes({9, 9, 9, 9}));
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    wr_lat = wr.rsp_cycle - wr.issue_cycle;
  }
  RT::Reset();
  EXPECT_EQ(rd_lat, 8u);
  EXPECT_EQ(wr_lat, 9u);
}

// Share Mem 四个 master 轮询：都在等时轮流走，没有谁被饿死。
TEST(BachShareMem, RoundRobinAcrossMasters) {
  std::vector<uint64_t> issues;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    ShareMem sm(clk, "smem");
    MemDriver a(clk, sm.Port(kSmemDteRv), 1, 0, 4);
    MemDriver b(clk, sm.Port(kSmemMuRv), 1, 0, 4);
    MemDriver c(clk, sm.Port(kSmemVuRv), 1, 0, 4);
    MemDriver d(clk, sm.Port(kSmemDteDsa), 1, 0, 4);
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    issues = {a.issue_cycle, b.issue_cycle, c.issue_cycle, d.issue_cycle};
  }
  RT::Reset();
  // 四个都被服务过，没有谁停在 0
  for (uint64_t v : issues) EXPECT_GT(v, 0u);
}

// 非同组的优先级：MU 最先，VU 与 DTE 同档，重发 / RV core / ctrl_noc 平级垫底。
TEST(BachCoreMem, PriorityOrderAcrossMasters) {
  std::vector<MemMaster> m = CoreMemMasters();
  EXPECT_LT(m[kCmemMuRd].priority, m[kCmemVuRd].priority) << "MU 在最前";
  EXPECT_EQ(m[kCmemVuRd].priority, m[kCmemDteRd].priority) << "VU 与 DTE 同档";
  EXPECT_LT(m[kCmemDteRd].priority, m[kCmemReissue].priority);
  EXPECT_EQ(m[kCmemReissue].priority, m[kCmemRvCore].priority) << "后三档平级";
  EXPECT_EQ(m[kCmemRvCore].priority, m[kCmemCfg].priority);
  // 读与写各占一个端口：合成一根的话同一拍发出的读与写会互相盖掉。
  EXPECT_EQ(m.size(), uint64_t(kCmemPortNum));
  EXPECT_NE(kCmemMuRd, kCmemMuWr);
  EXPECT_NE(kCmemDteRd, kCmemDteWr);
}

// 本模块不丢请求：槽占着时拉低 ready，master 原地保持，后来照样做完。
TEST(BachCoreMem, NoRequestIsDropped) {
  uint64_t low_rsp = 0, high_rsp = 0;
  ByteBlockPtr low_got;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreMem mem(clk, "cmem");
    // 同一个 bank，两笔一起来：优先级低的那一笔要等，但不能丢。
    MemDriver hi(clk, mem.Port(kCmemMuRd), 2, 0, 64);
    MemDriver lo(clk, mem.Port(kCmemCfg), 2, 0, 64);
    clk->Continue(120 * kPeriod);
    RT::JoinAll();
    high_rsp = hi.rsp_cycle;
    low_rsp = lo.rsp_cycle;
    low_got = lo.got;
  }
  RT::Reset();
  EXPECT_GT(high_rsp, 0u) << "优先的那一笔做完了";
  EXPECT_GT(low_rsp, 0u) << "让路的那一笔也要做完，不能丢";
  ASSERT_TRUE(low_got);
  EXPECT_EQ(low_got->size(), 64u);
}

// 部分 bank 冲突只挡住冲突的那个 bank，别的 bank 上的 master 照走。
TEST(BachCoreMem, PartialConflictOnlyStallsThatBank) {
  uint64_t other_rsp = 0, hi_rsp = 0, lo_rsp = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreMem mem(clk, "cmem");
    // MU 与 ctrl_noc 撞在 bank 0，VU 在 bank 1 上自己走自己的。
    MemDriver hi(clk, mem.Port(kCmemMuRd), 2, 0, 64);
    MemDriver lo(clk, mem.Port(kCmemCfg), 2, 0, 64);
    MemDriver other(clk, mem.Port(kCmemVuRd), 2, 128, 64);
    clk->Continue(120 * kPeriod);
    RT::JoinAll();
    hi_rsp = hi.rsp_cycle;
    lo_rsp = lo.rsp_cycle;
    other_rsp = other.rsp_cycle;
  }
  RT::Reset();
  EXPECT_GT(other_rsp, 0u);
  EXPECT_LT(other_rsp, lo_rsp) << "别的 bank 不受这场冲突影响";
  EXPECT_LT(hi_rsp, lo_rsp);
}

// 一次请求搬的是一整块，只改指定的那一段：两头的字节归相邻的数据。
TEST(BachCoreMem, PartialWriteTouchesOnlyItsRange) {
  ByteBlock before, after;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreMem mem(clk, "cmem");
    // 先铺一整行 0xAA。
    mem.Poke(0, ByteBlock(128, 0xAA));
    before = mem.Peek(0, 128);
    // 只写中间 8 个字节。
    auto blk = std::make_shared<ByteBlock>(128, 0x55);
    class PartialDriver : public BachModule {
     public:
      PartialDriver(ClockPtr c, MemPort& p, ByteBlockPtr d)
          : BachModule(c, "pd"), port(p), data(std::move(d)) {}

     protected:
      void Step() override {
        if (done) {
          port.IdleReq();
          return;
        }
        if (asserted && port.Ready()) {
          done = true;
          port.IdleReq();
          return;
        }
        port.Write(0, data, /*scale_en=*/false, /*woff=*/16, /*bytes=*/8);
        asserted = true;
      }

     private:
      MemPort& port;
      ByteBlockPtr data;
      bool asserted = false, done = false;
    };
    PartialDriver drv(clk, mem.Port(kCmemDteWr), blk);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    after = mem.Peek(0, 128);
  }
  RT::Reset();
  ASSERT_EQ(after.size(), 128u);
  for (uint64_t i = 0; i < 16; ++i) {
    EXPECT_EQ(after[i], 0xAA) << "第 " << i << " 个字节不该被动";
  }
  for (uint64_t i = 16; i < 24; ++i) {
    EXPECT_EQ(after[i], 0x55) << "第 " << i << " 个字节该被改";
  }
  for (uint64_t i = 24; i < 128; ++i) {
    EXPECT_EQ(after[i], 0xAA) << "第 " << i << " 个字节不该被动";
  }
}

// Matrix Mem 的形状：64 bank，四个 master，MU 只读且优先。
TEST(BachMatrixMem, ShapeAndMasters) {
  std::vector<MemMaster> m = MatrixMemMasters();
  EXPECT_EQ(m.size(), uint64_t(kMmemPortNum));
  EXPECT_LT(m[kMmemMu].priority, m[kMmemDteRd].priority) << "撞了只执行 MU";
  EXPECT_EQ(m[kMmemDteRd].read_latency, 8u);
  EXPECT_EQ(m[kMmemDteWr].write_latency, 9u) << "DTE 的读 8T、写 9T";
  EXPECT_EQ(kMatrixMemBytes, 36ull * 1024 * 1024);
}


// Share Mem：32 KB，只被三个 RV core 与 DTE 读写，四家轮询。
TEST(BachShareMem, ShapeAndMasters) {
  EXPECT_EQ(kShareMemBytes, 32u * 1024);
  std::vector<MemMaster> m = ShareMemMasters();
  EXPECT_EQ(m.size(), uint64_t(kSmemPortNum));
  EXPECT_EQ(m.size(), 4u) << "三个 RV core 加 DTE";
  for (auto const& one : m) {
    EXPECT_GE(one.read_latency, 5u) << "5 到 10 拍";
    EXPECT_LE(one.read_latency, 10u);
  }
}

// scale 使能时同时读写对应地址的 scale 寄存器：MU 与 VU 按 132 B 读写，块尾
// 那 4 B 就是 scale。
TEST(BachCoreMem, ScaleGoesWithTheDataWhenEnabled) {
  ByteBlock got_with, got_without;
  ByteBlock scale_after;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreMem mem(clk, "cmem");
    mem.Poke(0, ByteBlock(128, 0x11));
    mem.PokeScale(0, ByteBlock{0xAA, 0xBB, 0xCC, 0xDD});

    // 拉高 scale 使能读 128 B：回来的是 132 B，尾巴上是那一份 scale。
    class ScaleReader : public BachModule {
     public:
      ScaleReader(ClockPtr c, MemPort& p, bool en)
          : BachModule(c, "sr"), port(p), scale_en(en) {}
      ByteBlock got;

     protected:
      void Step() override {
        if (port.RspValid() && got.empty()) {
          ByteBlockPtr d = port.RspData();
          if (d) got = *d;
        }
        if (done) {
          port.IdleReq();
          return;
        }
        if (asserted && port.Ready()) {
          done = true;
          port.IdleReq();
          return;
        }
        port.Read(0, 128, scale_en);
        asserted = true;
      }

     private:
      MemPort& port;
      bool scale_en;
      bool asserted = false, done = false;
    };
    ScaleReader with(clk, mem.Port(kCmemMuRd), /*scale_en=*/true);
    ScaleReader without(clk, mem.Port(kCmemVuRd), /*scale_en=*/false);
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    got_with = with.got;
    got_without = without.got;
    scale_after = mem.PeekScale(0);
  }
  RT::Reset();
  ASSERT_EQ(got_with.size(), 132u) << "128 B 数据加 4 B scale";
  EXPECT_EQ(got_with[0], 0x11);
  EXPECT_EQ(got_with[128], 0xAA);
  EXPECT_EQ(got_with[131], 0xDD);
  EXPECT_EQ(got_without.size(), 128u) << "不使能就只走 SRAM 那一段";
  EXPECT_EQ(scale_after[0], 0xAA);
}

// scale 使能的写把块尾那 4 B 写进 scale 寄存器，数据段照常落 SRAM。
TEST(BachCoreMem, ScaleIsWrittenAlongWithTheData) {
  ByteBlock data_after, scale_after;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreMem mem(clk, "cmem");
    auto blk = std::make_shared<ByteBlock>(132, 0);
    for (uint64_t i = 0; i < 128; ++i) (*blk)[i] = 0x22;
    (*blk)[128] = 0x01;
    (*blk)[129] = 0x02;
    (*blk)[130] = 0x03;
    (*blk)[131] = 0x04;

    class ScaleWriter : public BachModule {
     public:
      ScaleWriter(ClockPtr c, MemPort& p, ByteBlockPtr d)
          : BachModule(c, "sw"), port(p), data(std::move(d)) {}

     protected:
      void Step() override {
        if (done) {
          port.IdleReq();
          return;
        }
        if (asserted && port.Ready()) {
          done = true;
          port.IdleReq();
          return;
        }
        port.Write(0, data, /*scale_en=*/true, /*woff=*/0, /*bytes=*/128);
        asserted = true;
      }

     private:
      MemPort& port;
      ByteBlockPtr data;
      bool asserted = false, done = false;
    };
    ScaleWriter w(clk, mem.Port(kCmemMuWr), blk);
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    data_after = mem.Peek(0, 128);
    scale_after = mem.PeekScale(0);
  }
  RT::Reset();
  EXPECT_EQ(data_after[0], 0x22) << "数据段落 SRAM";
  ASSERT_EQ(scale_after.size(), 4u);
  EXPECT_EQ(scale_after[0], 0x01) << "块尾那 4 B 落 scale 寄存器";
  EXPECT_EQ(scale_after[3], 0x04);
}

// 按 Byte mask 写过的行读时不做 ECC 检测：那一行留个记录。
TEST(BachCoreMem, MaskedRowSkipsEcc) {
  bool masked_row = false, whole_row = true;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreMem mem(clk, "cmem");
    auto blk = std::make_shared<ByteBlock>(128, 0x33);

    class Writer : public BachModule {
     public:
      Writer(ClockPtr c, MemPort& p, ByteBlockPtr d, uint64_t at, uint64_t n)
          : BachModule(c, "w"), port(p), data(std::move(d)), addr(at), bytes(n) {}

     protected:
      void Step() override {
        if (done) {
          port.IdleReq();
          return;
        }
        if (asserted && port.Ready()) {
          done = true;
          port.IdleReq();
          return;
        }
        port.Write(addr, data, false, 0, bytes);
        asserted = true;
      }

     private:
      MemPort& port;
      ByteBlockPtr data;
      uint64_t addr, bytes;
      bool asserted = false, done = false;
    };
    // 第 0 行只写 8 个字节，第 1 行整行写。
    Writer part(clk, mem.Port(kCmemDteWr), blk, 0, 8);
    Writer whole(clk, mem.Port(kCmemVuWr), blk, 128, 128);
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    masked_row = mem.MaskedRow(0);
    whole_row = mem.MaskedRow(128);
  }
  RT::Reset();
  EXPECT_TRUE(masked_row) << "非全 1 的那一行留了记录";
  EXPECT_FALSE(whole_row) << "整行写的那一行照常做 ECC 检测";
}

// ECC 的计数器每个 master 一个，这一轮不注错，读出来是 0。
TEST(BachCoreMem, EccCountersAreReadable) {
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreMem mem(clk, "cmem");
  for (uint64_t m = 0; m < mem.MasterNum(); ++m) {
    EXPECT_EQ(mem.EccSingleBit(m), 0u) << "m=" << m;
    EXPECT_EQ(mem.EccDoubleBit(m), 0u) << "m=" << m;
  }
  RT::Reset();
}
