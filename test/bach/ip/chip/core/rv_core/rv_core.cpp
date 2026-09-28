// RV core 的行为基线：跑真实的 RV32 kernel。
//
// 步 5 的判据：一个只做标量活的 kernel 跑完并向 TS 报完成。
//
// 指令逐条执行、每条 1 拍，功能由 src/rv32 的功能模型算；task_queue、dsa_iss、
// 完成上报这些与 TS 和 DSA 交互的机制在外面这一层。

#include <gtest/gtest.h>

#include <deque>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/rv_core/rv_core.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

std::string KernelDir() {
  return std::string(LATCH_SOURCE_DIR) + "/src/bach/compiler/kernel/build/";
}

// 从 nm 导出的符号表里取一个 task 的入口地址。task_chain 的 TASK_PC 填的就是它。
uint64_t SymbolOf(std::string const& kind, std::string const& name) {
  std::ifstream f(KernelDir() + "kernel_" + kind + ".sym");
  std::string addr, type, sym;
  while (f >> addr >> type >> sym) {
    if (sym == name) return std::stoull(addr, nullptr, 16);
  }
  return 0;
}

bool KernelBuilt(std::string const& kind) {
  std::ifstream f(KernelDir() + "kernel_" + kind + ".hex");
  return f.good();
}

// 扮演 Share Mem / Core Mem / Router 包头口：一律收得下，隔几拍回响应。
//
// 现在 RV core 真的会往这几个口上发请求了。没有从端时 req_ready 一直是 0，
// 请求发不出去，gpr 的就绪位也就永远立不起来，核会停在那条指令上。
class MemStub : public BachModule {
 public:
  MemStub(ClockPtr c, const std::string& name, MemPort& p, uint64_t delay)
      : BachModule(c, name), port(p), latency(delay) {}

  uint64_t reads = 0, writes = 0;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    ByteBlockPtr rsp;
    bool valid = false;
    if (!pipe.empty() && pipe.front().first <= now) {
      rsp = pipe.front().second;
      valid = true;
      pipe.pop_front();
    }
    port.DriveSlave(true, valid, rsp);

    if (port.req_valid.Get() == 0) return;
    if (port.req_we.Get() != 0) {
      ++writes;
      return;
    }
    auto d = std::make_shared<ByteBlock>(port.req_bytes.Get(), 0);
    pipe.push_back({now + latency, d});
    ++reads;
  }

 private:
  MemPort& port;
  std::deque<std::pair<uint64_t, ByteBlockPtr>> pipe;
  uint64_t latency;
};

// 把一个 RV core 的三个存储口都接上从端。
struct MemSides {
  MemSides(ClockPtr c, RvCore& rv)
      : smem(c, "smem", rv.Smem(), kShareMemLatency),
        cmem(c, "cmem", rv.Cmem(), kCoreMemLatency),
        hdr(c, "hdr", rv.Hdr(), kCoreMemLatency) {}
  MemStub smem, cmem, hdr;
};

// RV core 的五个模块由装配统一驱动，装配自己不挂时钟。
class RvDriver : public BachModule {
 public:
  RvDriver(ClockPtr c, RvCore& target) : BachModule(c, "driver"), rv(target) {}

 protected:
  void Step() override { rv.RunStep(); }

 private:
  RvCore& rv;
};

// 扮演 TS：在指定拍下发一个 task，收完成。
class TsSide : public BachModule {
 public:
  TsSide(ClockPtr c, RvCore& core, uint64_t fire_at, uint64_t entry,
         uint64_t sid, uint64_t tid)
      : BachModule(c, "ts"), rv(core), at(fire_at), pc(entry), stream(sid),
        task(tid) {}

  uint64_t dones = 0;
  uint64_t done_stream = 0, done_task = 0;
  uint64_t insts = 0, started = 0;
  bool sent = false;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    if (!sent && now >= at) {
      rv.Cmd().Drive(pc, stream, task, /*user=*/77, /*path=*/3,
                     /*dsa_en=*/true, /*issue_seq=*/1);
      if (rv.Cmd().Ready()) sent = true;
    } else {
      rv.Cmd().Idle();
    }
    if (rv.Done().Valid()) {
      ++dones;
      done_stream = rv.Done().stream_id.Get();
      done_task = rv.Done().task_id.Get();
    }
    // DSA 那一侧一律收得下配置。
    rv.DsaCfg().DriveReady(true);
    insts = rv.Exec().Insts();
    started = rv.TaskQueue().Started();
  }

 private:
  RvCore& rv;
  uint64_t at, pc, stream, task;
};

// 收 DSA 配置写，看 kernel 到底配了哪些寄存器。
class DsaSide : public BachModule {
 public:
  DsaSide(ClockPtr c, RvCore& target) : BachModule(c, "dsa"), rv(target) {}

  uint64_t writes = 0;
  std::vector<uint64_t> addrs;

 protected:
  void Step() override {
    if (rv.DsaCfg().Valid()) {
      ++writes;
      addrs.push_back(rv.DsaCfg().req_addr.Get());
    }
  }

 private:
  RvCore& rv;
};

}  // namespace

// 步 5 的判据：TS 下发一个只做标量活的 task，RV core 按 task_pc 跑完 kernel
// 并向 TS 报完成。
TEST(BachRvCore, RunsOneTaskAndReportsDone) {
  if (!KernelBuilt("dte")) {
    GTEST_SKIP() << "kernel 还没编，先在 src/bach/compiler/kernel 下 make";
  }
  uint64_t dones = 0, insts = 0, stream = 0, task = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvCore rv(clk, "rv_dte", RvUnit::kDte);
    RvDriver drv(clk, rv);
    MemSides mem(clk, rv);
    rv.LoadImage(KernelDir() + "kernel_dte.hex");

    uint64_t pc = SymbolOf("dte", "task_rv_nop");
    ASSERT_GT(pc, 0u) << "符号表里没有 task_rv_nop";

    TsSide ts(clk, rv, /*at=*/2, pc, /*stream=*/3, /*task=*/5);
    clk->Continue(2000 * kPeriod);
    RT::JoinAll();
    dones = ts.dones;
    insts = ts.insts;
    stream = ts.done_stream;
    task = ts.done_task;
  }
  RT::Reset();
  EXPECT_EQ(dones, 1u);       // 报了一次完成
  EXPECT_EQ(stream, 3u);      // 带回 TS 下发的身份
  EXPECT_EQ(task, 5u);
  EXPECT_GT(insts, 0u);       // 真的跑了指令
}

// 配了 DSA 的 task：RV core 配完寄存器用 task_done(0) 交还自己，不向 TS 报；这一笔
// 的完成由 DSA 报，TASK_RECV_UNIT 配只等 DSA。
TEST(BachRvCore, DsaTaskHandsBackWithoutNotifyingTs) {
  if (!KernelBuilt("dte")) GTEST_SKIP() << "kernel 还没编";
  uint64_t dones = 0, started = 0, dsa_writes = 0, finishes = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvCore rv(clk, "rv_dte", RvUnit::kDte);
    RvDriver drv(clk, rv);
    MemSides mem(clk, rv);
    rv.LoadImage(KernelDir() + "kernel_dte.hex");

    uint64_t pc = SymbolOf("dte", "task_dte_move");
    ASSERT_GT(pc, 0u);

    TsSide ts(clk, rv, /*at=*/2, pc, /*stream=*/3, /*task=*/5);
    DsaSide dsa(clk, rv);
    clk->Continue(2000 * kPeriod);
    RT::JoinAll();
    dones = ts.dones;
    started = ts.started;
    dsa_writes = dsa.writes;
    finishes = rv.TaskQueue().Finishes();
  }
  RT::Reset();
  EXPECT_EQ(started, 1u);     // 起了这一个 task
  EXPECT_GT(dsa_writes, 0u);  // 真的配了 DSA 寄存器
  EXPECT_EQ(finishes, 1u);    // RV core 交还了自己
  EXPECT_EQ(dones, 0u);       // 但不向 TS 报完成
}

// 双发射：每拍最多发 2 条，跑完一个 task 花的拍数不少于它指令数的一半；没有
// 依赖的相邻两条确实同拍发出。
TEST(BachRvCore, AtMostTwoInstructionsPerCycle) {
  if (!KernelBuilt("mu")) GTEST_SKIP() << "kernel 还没编";
  uint64_t insts = 0, done_at = 0, dual = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvCore rv(clk, "rv_mu", RvUnit::kMu);
    RvDriver drv(clk, rv);
    MemSides mem(clk, rv);
    rv.LoadImage(KernelDir() + "kernel_mu.hex");
    uint64_t pc = SymbolOf("mu", "task_mu_compute");
    ASSERT_GT(pc, 0u);

    class Watch : public BachModule {
     public:
      Watch(ClockPtr c, RvCore& core, uint64_t entry)
          : BachModule(c, "w"), rv(core), pc(entry) {}
      uint64_t insts = 0, done_at = 0;
      bool sent = false;

     protected:
      void Step() override {
        uint64_t now = CycleNow();
        if (!sent && now >= 2) {
          rv.Cmd().Drive(pc, 1, 2, 88, 3, true, 1);
          if (rv.Cmd().Ready()) sent = true;
        } else {
          rv.Cmd().Idle();
        }
        // 这一笔配了 DSA，收尾是交还自己而不是报 TS，所以看 task_queue 收到
        // 交还的那一拍。
        if (rv.TaskQueue().Finishes() > 0 && done_at == 0) done_at = now;
        rv.DsaCfg().DriveReady(true);
        insts = rv.Exec().Insts();
      }

     private:
      RvCore& rv;
      uint64_t pc;
    };
    Watch w(clk, rv, pc);
    clk->Continue(2000 * kPeriod);
    RT::JoinAll();
    insts = w.insts;
    done_at = w.done_at;
    dual = rv.Exec().DualIssued();
  }
  RT::Reset();
  ASSERT_GT(done_at, 0u);
  ASSERT_GT(insts, 0u);
  // 每拍最多 2 条，另加访存与 DSA 读的停拍，所以拍数不少于指令数的一半
  EXPECT_GE(2 * done_at, insts);
  EXPECT_GT(dual, 0u) << "没有依赖的相邻两条要同拍发出";
}

// 身份在 RV 发出 dsaw 那一拍抄进请求。trigger 后面紧跟 task_done 的 task
// （send_part 就是写一笔 trigger 再交还），背靠背下发两个，DSA 那一侧先压着不收：
// trigger 还在下发队列里排着时核已经换成下一个 task，DSA 收下时带的仍是发出它的
// 那个 task 的身份。
TEST(BachRvCore, TriggerKeepsIdsUntilDsaTakesIt) {
  if (!KernelBuilt("dte")) GTEST_SKIP() << "kernel 还没编";
  constexpr uint64_t kSendAt = 2;
  constexpr uint64_t kOpenAt = 60;  // DSA 这一拍起才收
  std::vector<std::pair<uint64_t, uint64_t>> ids;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvCore rv(clk, "rv_dte", RvUnit::kDte);
    RvDriver drv(clk, rv);
    MemSides mem(clk, rv);
    rv.LoadImage(KernelDir() + "kernel_dte.hex");
    uint64_t pc = SymbolOf("dte", "task_dte_send_part");
    ASSERT_GT(pc, 0u);

    class Side : public BachModule {
     public:
      Side(ClockPtr c, RvCore& core, uint64_t entry)
          : BachModule(c, "side"), rv(core), pc(entry) {}
      std::vector<std::pair<uint64_t, uint64_t>> ids;

     protected:
      void Step() override {
        uint64_t now = CycleNow();
        // 两个 task 一个接一个塞进 task_queue：stream 3 / task 5，stream 4 / task 6。
        if (sent < 2 && now >= kSendAt) {
          rv.Cmd().Drive(pc, 3 + sent, 5 + sent, 77, 3, true, sent + 1);
          if (rv.Cmd().Ready()) ++sent;
        } else {
          rv.Cmd().Idle();
        }
        DsaCfgPort& cfg = rv.DsaCfg();
        bool open = now >= kOpenAt;
        cfg.DriveReady(open);
        if (open && cfg.Valid() && cfg.Seq() != last_seq) {
          last_seq = cfg.Seq();
          if (cfg.req_we.Get() != 0 && cfg.req_addr.Get() == 0) {
            DsaTaskIds got = cfg.TaskIds();
            ids.push_back({got.stream, got.task});
          }
        }
      }

     private:
      RvCore& rv;
      uint64_t pc;
      uint64_t sent = 0, last_seq = 0;
    };
    Side side(clk, rv, pc);
    clk->Continue(600 * kPeriod);
    RT::JoinAll();
    ids = side.ids;
  }
  RT::Reset();
  ASSERT_EQ(ids.size(), 2u);
  EXPECT_EQ(ids[0], std::make_pair(uint64_t(3), uint64_t(5)));
  EXPECT_EQ(ids[1], std::make_pair(uint64_t(4), uint64_t(6)));
}

// task_queue 提前收下一个 task，前一个完成后立刻接上，不留 bubble。
TEST(BachRvCore, QueuesNextTaskWhileBusy) {
  if (!KernelBuilt("dte")) GTEST_SKIP() << "kernel 还没编";
  uint64_t dones = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvCore rv(clk, "rv_dte", RvUnit::kDte);
    RvDriver drv(clk, rv);
    MemSides mem(clk, rv);
    rv.LoadImage(KernelDir() + "kernel_dte.hex");
    uint64_t pc = SymbolOf("dte", "task_dte_move");
    ASSERT_GT(pc, 0u);

    class TwoTasks : public BachModule {
     public:
      TwoTasks(ClockPtr c, RvCore& core, uint64_t entry)
          : BachModule(c, "tt"), rv(core), pc(entry) {}
      uint64_t sent = 0, started = 0;

     protected:
      void Step() override {
        if (sent < 2) {
          rv.Cmd().Drive(pc, sent, sent, 90 + sent, 3, true, sent + 1);
          if (rv.Cmd().Ready()) ++sent;
        } else {
          rv.Cmd().Idle();
        }
        started = rv.TaskQueue().Started();
        rv.DsaCfg().DriveReady(true);
      }

     private:
      RvCore& rv;
      uint64_t pc;
    };
    TwoTasks tt(clk, rv, pc);
    clk->Continue(4000 * kPeriod);
    RT::JoinAll();
    dones = tt.started;
  }
  RT::Reset();
  EXPECT_EQ(dones, 2u);   // 两个 task 都起了
}

// 自定义 CSR 读它当场拿到当前这一笔 task 的身份：kernel 读出 stream_id 再写给
// DSA，读回上一次的值就把身份配错了。
TEST(BachRvCore, CsrReadsCurrentTaskIds) {
  if (!KernelBuilt("mu")) GTEST_SKIP() << "kernel 还没编";
  uint64_t stream_seen = 0, task_seen = 0, user_seen = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvCore rv(clk, "rv_mu", RvUnit::kMu);
    RvDriver drv(clk, rv);
    MemSides mem(clk, rv);
    rv.LoadImage(KernelDir() + "kernel_mu.hex");
    uint64_t pc = SymbolOf("mu", "task_mu_compute");
    ASSERT_GT(pc, 0u);

    // 身份三项不再由 kernel 写进 MU 的寄存器：写 trigger 那一拍，DSA 从 dsa_ids
    // 直连线上采样当前 task 的身份。看 trigger（0x000）写出去那一刻 dsa_ids 上
    // 是什么。
    class Watch : public BachModule {
     public:
      Watch(ClockPtr c, RvCore& core, uint64_t entry)
          : BachModule(c, "w"), rv(core), pc(entry) {}
      uint64_t stream_seen = 0, task_seen = 0, user_seen = 0;
      bool sent = false;

     protected:
      void Step() override {
        if (!sent && CycleNow() >= 2) {
          rv.Cmd().Drive(pc, /*stream=*/5, /*task=*/7, /*user=*/61, 3, true, 1);
          if (rv.Cmd().Ready()) sent = true;
        } else {
          rv.Cmd().Idle();
        }
        if (rv.DsaCfg().Valid() && rv.DsaCfg().req_we.Get() != 0) {
          if (rv.DsaCfg().req_addr.Get() == 0x000) {   // MU_TASK_TRIGGER
            stream_seen = rv.DsaIdsPtr()->Stream();
            task_seen = rv.DsaIdsPtr()->Task();
            user_seen = rv.DsaIdsPtr()->User();
          }
        }
        rv.DsaCfg().DriveReady(true);
      }

     private:
      RvCore& rv;
      uint64_t pc;
    };
    Watch w(clk, rv, pc);
    clk->Continue(2000 * kPeriod);
    RT::JoinAll();
    stream_seen = w.stream_seen;
    task_seen = w.task_seen;
    user_seen = w.user_seen;
  }
  RT::Reset();
  EXPECT_EQ(stream_seen, 5u);   // trigger 那一拍 dsa_ids 上就是这一笔 task 的身份
  EXPECT_EQ(task_seen, 7u);
  EXPECT_EQ(user_seen, 61u);
}
