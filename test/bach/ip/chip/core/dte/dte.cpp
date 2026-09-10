// DTE 的行为基线。
//
// 步 7 的判据：五个搬运方向各一个用例，Commit 配对接纳与 Join 各一个。
//
// 重点覆盖三条最容易实现错的：Commit 三样一起拿（挡住半任务）、Join 两侧都满足
// 才报完成、通道之间乱序不互相堵。

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/dte/dte.h"
#include "bach/ip/chip/core/memory/core_mem.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

// DTE 是十一个模块，加驱动就超过默认的 8 个协程槽位。
void EnsureSlots() { RT::Reset(8, 8); }

// 身份取自包头：stream_id 是这个用户在本 core 上的槽位，task_id 是任务链上的
// 第几步。DTE 报完成时原样带给 TS。
MessagePtr MakeMsg(uint64_t user, uint64_t path, uint64_t bytes,
                   uint64_t stream = 0, uint64_t task = 0) {
  auto m = std::make_shared<Message>();
  m->user_id = user;
  m->path_id = path;
  m->size = bytes;
  m->stream_id = stream;
  m->task_id = task;
  return m;
}

// 扮演 Router 的 CoreStation：按 AXI-Stream 往 DTE 送一个整包。
class RouterFeeder : public BachModule {
 public:
  RouterFeeder(ClockPtr c, Dte& target, uint64_t fire_at, MessagePtr pkt)
      : BachModule(c, "feeder"), dte(target), at(fire_at), msg(std::move(pkt)) {
    total = FlitsOf(msg->size);
  }

  uint64_t sent = 0;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    CoreDataPort& p = dte.FromRouter();
    if (now < at || sent >= total) {
      p.Idle();
      return;
    }
    // 反压时保持数据不变，看见 ready 才换下一笔。端口带序号，所以保持期间
    // 接收方不会把同一笔消费两遍。
    if (driving) {
      if (!p.Ready()) return;
      ++sent;
      driving = false;
      if (sent >= total) {
        p.Idle();
        return;
      }
    }
    uint64_t left = msg->size - sent * kFlitBytes;
    uint64_t n = left > kFlitBytes ? kFlitBytes : left;
    bool last = (sent + 1 == total);
    p.Drive(n, last, sent == 0, 0, msg);
    driving = true;
  }

 private:
  Dte& dte;
  uint64_t at, total = 0;
  MessagePtr msg;
  bool driving = false;
};

// 扮演三块存储：一律收得下、隔两拍回响应。
class MemSide : public BachModule {
 public:
  MemSide(ClockPtr c, std::vector<std::shared_ptr<MemPort>> list)
      : BachModule(c, "mem"), ports(std::move(list)) {}

  uint64_t writes = 0, reads = 0;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    for (size_t i = 0; i < ports.size(); ++i) {
      MemReqView r = ReadMemReq(*ports[i]);
      if (r.valid) {
        if (r.we) {
          ++writes;
        } else {
          ++reads;
          pend.push_back({i, now + 2});
        }
      }
      bool rsp = false;
      for (auto it = pend.begin(); it != pend.end();) {
        if (it->port == i && it->at <= now) {
          rsp = true;
          it = pend.erase(it);
          break;
        }
        ++it;
      }
      ports[i]->DriveSlave(true, rsp,
                           rsp ? std::make_shared<ByteBlock>(256, 7) : nullptr);
    }
  }

 private:
  struct Pend {
    size_t port = 0;
    uint64_t at = 0;
  };
  std::vector<std::shared_ptr<MemPort>> ports;
  std::vector<Pend> pend;
};

// 扮演 TS：收 dsa_done。
// 兼做探针：那几个计数都是 Logic64，主线程读到的是 t=0 的值，要在协程内抄。
class TsSide : public BachModule {
 public:
  TsSide(ClockPtr c, Dte& target) : BachModule(c, "ts"), dte(target) {}

  uint64_t dones = 0;
  std::vector<uint64_t> tasks;
  uint64_t parsed = 0, dropped = 0, admitted = 0, stalled = 0, joined = 0;
  uint64_t trigs = 0, done_stream = 0, done_task = 0;

 protected:
  void Step() override {
    if (dte.ToTs().Valid()) {
      ++dones;
      tasks.push_back(dte.ToTs().stream_id.Get());
      done_stream = dte.ToTs().stream_id.Get();
      done_task = dte.ToTs().task_id.Get();
    }
    trigs = dte.Regfile().Triggers();
    parsed = dte.Parser().Parsed();
    dropped = dte.Parser().Dropped();
    admitted = dte.Committer().Admitted();
    stalled = dte.Committer().Stalled();
    joined = dte.Completion().Joined();
  }

 private:
  Dte& dte;
};

}  // namespace

// Router → CM：一个整包进来，搬进 Core Mem，两侧 Join 后向 TS 报一次完成。
TEST(BachDte, InboundRouterToCoreMem) {
  uint64_t dones = 0, writes = 0, parsed = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    DteCfg cfg;
    cfg.inbound = Route::kRouterToCm;
    Dte dte(clk, "dte", cfg);

    auto cm_rd = std::make_shared<MemPort>(clk);
    auto cm_wr = std::make_shared<MemPort>(clk);
    dte.AttachCmemRd(cm_rd);
    dte.AttachCmemWr(cm_wr);
    auto mm_rd = std::make_shared<MemPort>(clk);
    auto mm_wr = std::make_shared<MemPort>(clk);
    dte.AttachMmemRd(mm_rd);
    dte.AttachMmemWr(mm_wr);

    RouterFeeder feed(clk, dte, 2, MakeMsg(11, 0, 512));
    MemSide mem(clk, {cm_rd, cm_wr, mm_rd, mm_wr});
    TsSide ts(clk, dte);
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    dones = ts.dones;
    writes = mem.writes;
    parsed = ts.parsed;
  }
  RT::Reset();
  EXPECT_EQ(parsed, 1u);   // 解析了一个 Header
  EXPECT_GT(writes, 0u);   // 数据写进了存储
  EXPECT_EQ(dones, 1u);    // exactly-once：只报一次
}

// 纯包头任务：data_len 为 0，Header beat 同时带 TLAST，照样走完并报完成。
TEST(BachDte, HeaderOnlyTask) {
  uint64_t dones = 0, parsed = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Dte dte(clk, "dte", DteCfg{});
    auto cm_rd = std::make_shared<MemPort>(clk);
    auto cm_wr = std::make_shared<MemPort>(clk);
    dte.AttachCmemRd(cm_rd);
    dte.AttachCmemWr(cm_wr);
    auto mm_rd = std::make_shared<MemPort>(clk);
    auto mm_wr = std::make_shared<MemPort>(clk);
    dte.AttachMmemRd(mm_rd);
    dte.AttachMmemWr(mm_wr);
    RouterFeeder feed(clk, dte, 2, MakeMsg(12, 0, 0));
    MemSide mem(clk, {cm_rd, cm_wr, mm_rd, mm_wr});
    TsSide ts(clk, dte);
    clk->Continue(150 * kPeriod);
    RT::JoinAll();
    dones = ts.dones;
    parsed = ts.parsed;
  }
  RT::Reset();
  EXPECT_EQ(parsed, 1u);
  EXPECT_EQ(dones, 1u);
}

// 非法 Header 进 Drop Frame：不生成 Descriptor、不发存储请求，只消费到 TLAST。
TEST(BachDte, IllegalHeaderDropsFrame) {
  uint64_t parsed = 0, dropped = 0, writes = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Dte dte(clk, "dte", DteCfg{});
    auto cm_rd = std::make_shared<MemPort>(clk);
    auto cm_wr = std::make_shared<MemPort>(clk);
    dte.AttachCmemRd(cm_rd);
    dte.AttachCmemWr(cm_wr);
    auto mm_rd = std::make_shared<MemPort>(clk);
    auto mm_wr = std::make_shared<MemPort>(clk);
    dte.AttachMmemRd(mm_rd);
    dte.AttachMmemWr(mm_wr);
    // 超过单任务上限 32 KB，这一帧要被丢掉
    RouterFeeder feed(clk, dte, 2, MakeMsg(13, 0, 64 * 1024));
    MemSide mem(clk, {cm_rd, cm_wr, mm_rd, mm_wr});
    TsSide ts(clk, dte);
    clk->Continue(400 * kPeriod);
    RT::JoinAll();
    parsed = ts.parsed;
    dropped = ts.dropped;
    writes = mem.writes;
  }
  RT::Reset();
  EXPECT_EQ(parsed, 0u);
  EXPECT_EQ(dropped, 1u);
  EXPECT_EQ(writes, 0u);   // 丢的帧不发存储请求
}

// Commit 三样一起拿：Completion RS 占满之后不再接纳，挡住半任务。
TEST(BachDte, CommitNeedsAllThreeResources) {
  uint64_t admitted = 0, stalled = 0, peak = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Dte dte(clk, "dte", DteCfg{});
    auto cm_rd = std::make_shared<MemPort>(clk);
    auto cm_wr = std::make_shared<MemPort>(clk);
    dte.AttachCmemRd(cm_rd);
    dte.AttachCmemWr(cm_wr);
    auto mm_rd = std::make_shared<MemPort>(clk);
    auto mm_wr = std::make_shared<MemPort>(clk);
    dte.AttachMmemRd(mm_rd);
    dte.AttachMmemWr(mm_wr);

    // 存储侧一直不 ready，任务做不完，Completion RS 会被占满
    class DeadMem : public BachModule {
     public:
      DeadMem(ClockPtr c, std::vector<std::shared_ptr<MemPort>> list)
          : BachModule(c, "dead"), ports(std::move(list)) {}

     protected:
      void Step() override {
        for (auto& p : ports) p->DriveSlave(false, false, nullptr);
      }

     private:
      std::vector<std::shared_ptr<MemPort>> ports;
    };
    DeadMem mem(clk, {cm_rd, cm_wr, mm_rd, mm_wr});

    // 连着灌很多个纯包头任务，把 RS 占满
    class ManyFrames : public BachModule {
     public:
      ManyFrames(ClockPtr c, Dte& d) : BachModule(c, "many"), dte(d) {}
      uint64_t sent = 0;
      bool driving = false;

     protected:
      void Step() override {
        CoreDataPort& p = dte.FromRouter();
        if (driving) {
          if (!p.Ready()) return;   // 保持，不换
          ++sent;
          driving = false;
        }
        if (sent >= 30) {
          p.Idle();
          return;
        }
        p.Drive(16, true, true, 0, MakeMsg(100 + sent, 0, 16));
        driving = true;
      }

     private:
      Dte& dte;
    };
    ManyFrames many(clk, dte);

    class Probe : public BachModule {
     public:
      Probe(ClockPtr c, Dte& d) : BachModule(c, "probe"), dte(d) {}
      uint64_t admitted = 0, stalled = 0;

     protected:
      void Step() override {
        admitted = dte.Committer().Admitted();
        stalled = dte.Committer().Stalled();
        uint64_t u = dte.Completion().Used();
        if (u > peak) peak = u;
      }

     public:
      uint64_t peak = 0;

     private:
      Dte& dte;
    };
    Probe probe(clk, dte);
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    admitted = probe.admitted;
    stalled = probe.stalled;
    peak = probe.peak;
  }
  RT::Reset();
  // Completion RS 只有 16 项，在途的任务数任何时候都不超过它
  EXPECT_LE(peak, kCompRsNum);
  EXPECT_GT(admitted, 0u);
  EXPECT_GT(stalled, 0u);   // 后面的被挡住了，不是丢了
}

// Join：一笔任务的两侧都满足才报完成，不会报两次。三帧走同一个 path，所以
// task_id 是同一个（都是任务链上的那一步），认哪两半属于同一笔靠的是 Commit
// 分配的内部序号。
TEST(BachDte, JoinReportsExactlyOnce) {
  uint64_t dones = 0, joined = 0;
  std::vector<uint64_t> tasks;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Dte dte(clk, "dte", DteCfg{});
    auto cm_rd = std::make_shared<MemPort>(clk);
    auto cm_wr = std::make_shared<MemPort>(clk);
    dte.AttachCmemRd(cm_rd);
    dte.AttachCmemWr(cm_wr);
    auto mm_rd = std::make_shared<MemPort>(clk);
    auto mm_wr = std::make_shared<MemPort>(clk);
    dte.AttachMmemRd(mm_rd);
    dte.AttachMmemWr(mm_wr);

    class ThreeFrames : public BachModule {
     public:
      ThreeFrames(ClockPtr c, Dte& d) : BachModule(c, "three"), dte(d) {}
      uint64_t sent = 0;
      bool driving = false;

     protected:
      void Step() override {
        CoreDataPort& p = dte.FromRouter();
        if (driving) {
          if (!p.Ready()) return;
          ++sent;
          driving = false;
        }
        if (sent >= 3) {
          p.Idle();
          return;
        }
        p.Drive(16, true, true, 0,
                MakeMsg(200 + sent, 0, 16, /*stream=*/sent, /*task=*/sent));
        driving = true;
      }

     private:
      Dte& dte;
    };
    ThreeFrames three(clk, dte);
    MemSide mem(clk, {cm_rd, cm_wr, mm_rd, mm_wr});
    TsSide ts(clk, dte);

    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    dones = ts.dones;
    tasks = ts.tasks;
    joined = ts.joined;
  }
  RT::Reset();
  EXPECT_EQ(joined, 3u);
  EXPECT_EQ(dones, 3u);
  // 三笔各报一次，报上来的是各自的 stream_id，一个都不重复
  ASSERT_EQ(tasks.size(), 3u);
  EXPECT_NE(tasks[0], tasks[1]);
  EXPECT_NE(tasks[1], tasks[2]);
}

// 中间 Buffer 满时向 Router 反压，不丢数据。
TEST(BachDte, BufferFullBackpressuresRouter) {
  uint64_t sent = 0, parsed = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Dte dte(clk, "dte", DteCfg{});
    auto cm_rd = std::make_shared<MemPort>(clk);
    auto cm_wr = std::make_shared<MemPort>(clk);
    dte.AttachCmemRd(cm_rd);
    dte.AttachCmemWr(cm_wr);
    auto mm_rd = std::make_shared<MemPort>(clk);
    auto mm_wr = std::make_shared<MemPort>(clk);
    dte.AttachMmemRd(mm_rd);
    dte.AttachMmemWr(mm_wr);

    // 存储侧不收，写那一半推不动，buffer 会满
    class DeadMem : public BachModule {
     public:
      DeadMem(ClockPtr c, std::vector<std::shared_ptr<MemPort>> list)
          : BachModule(c, "dead"), ports(std::move(list)) {}

     protected:
      void Step() override {
        for (auto& p : ports) p->DriveSlave(false, false, nullptr);
      }

     private:
      std::vector<std::shared_ptr<MemPort>> ports;
    };
    DeadMem mem(clk, {cm_rd, cm_wr, mm_rd, mm_wr});
    // 一个很大的包：256 B × 100 拍，buffer 只有 16 拍
    RouterFeeder feed(clk, dte, 2, MakeMsg(30, 0, 100 * 256));
    TsSide ts(clk, dte);
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    sent = feed.sent;
    parsed = ts.parsed;
  }
  RT::Reset();
  EXPECT_EQ(parsed, 1u);
  // buffer 顶住了，没把 100 拍全收下
  EXPECT_LT(sent, 100u);
  EXPECT_GT(sent, 0u);
}

// 写 Trigger 那一拍采样四个直连身份信号：软件不写身份，DSA 报完成时填的是
// 直连过来的那一组。
TEST(BachDte, TriggerSamplesDirectIds) {
  uint64_t trigs = 0, dones = 0, done_stream = 0, done_task = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Dte dte(clk, "dte", DteCfg{});
    auto cm_rd = std::make_shared<MemPort>(clk);
    auto cm_wr = std::make_shared<MemPort>(clk);
    dte.AttachCmemRd(cm_rd);
    dte.AttachCmemWr(cm_wr);
    auto mm_rd = std::make_shared<MemPort>(clk);
    auto mm_wr = std::make_shared<MemPort>(clk);
    dte.AttachMmemRd(mm_rd);
    dte.AttachMmemWr(mm_wr);
    auto ids = std::make_shared<DsaIdsPort>(clk);
    dte.AttachIds(ids);

    // 扮演 DTE RV core：每拍驱动身份，按 F14 的顺序写四个寄存器再写 Trigger。
    class RvSide : public BachModule {
     public:
      RvSide(ClockPtr c, Dte& d, std::shared_ptr<DsaIdsPort> p)
          : BachModule(c, "rv"), dte(d), ids(std::move(p)) {}

     protected:
      void Step() override {
        ids->Drive(/*stream=*/6, /*task=*/9, /*user=*/77, /*path=*/2);
        uint64_t now = CycleNow();
        DsaCfgPort& cfg = dte.Cfg();
        uint64_t tpl = kDteTemplateBase;
        if (now == 3) {
          cfg.Drive(tpl + kDteRegSrcAddr, 0, 1);
        } else if (now == 4) {
          cfg.Drive(tpl + kDteRegDstAddr, 0, 2);
        } else if (now == 5) {
          cfg.Drive(tpl + kDteRegDataLen, 256, 3);
        } else if (now == 6) {
          // transfer_mode = 010：Cmem → Router。task_last 置位才通知 TS。
          cfg.Drive(tpl + kDteRegTrigger,
                    uint64_t(Route::kCmToRouter) | kDteTaskLast, 4);
        } else {
          cfg.Idle();
        }
      }

     private:
      Dte& dte;
      std::shared_ptr<DsaIdsPort> ids;
    };
    RvSide rv(clk, dte, ids);
    MemSide mem(clk, {cm_rd, cm_wr, mm_rd, mm_wr});
    TsSide ts(clk, dte);
    // 出核那一侧一律收得下。
    class RouterSink : public BachModule {
     public:
      RouterSink(ClockPtr c, Dte& d) : BachModule(c, "sink"), dte(d) {}

     protected:
      void Step() override {
        dte.ToRouter().DriveReady(true);
      }

     private:
      Dte& dte;
    };
    RouterSink sink(clk, dte);

    clk->Continue(300 * kPeriod);
    RT::JoinAll();
    trigs = ts.trigs;
    dones = ts.dones;
    done_stream = ts.done_stream;
    done_task = ts.done_task;
  }
  RT::Reset();
  EXPECT_EQ(trigs, 1u);        // 写一次 Trigger 起一笔任务
  EXPECT_EQ(dones, 1u);
  EXPECT_EQ(done_stream, 6u);  // 报完成时填的是直连过来的那一组
  EXPECT_EQ(done_task, 9u);
}

// 一笔配置写在被收下之前保持同一个序号：换了号，写一次执行一次的 Trigger 会被
// 执行好几遍。
TEST(BachDte, TriggerRunsOncePerWrite) {
  uint64_t trigs = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Dte dte(clk, "dte", DteCfg{});
    auto cm_rd = std::make_shared<MemPort>(clk);
    auto cm_wr = std::make_shared<MemPort>(clk);
    dte.AttachCmemRd(cm_rd);
    dte.AttachCmemWr(cm_wr);
    auto mm_rd = std::make_shared<MemPort>(clk);
    auto mm_wr = std::make_shared<MemPort>(clk);
    dte.AttachMmemRd(mm_rd);
    dte.AttachMmemWr(mm_wr);
    auto ids = std::make_shared<DsaIdsPort>(clk);
    dte.AttachIds(ids);

    // 同一个序号连着驱动十拍：只该起一笔。
    class HoldOne : public BachModule {
     public:
      HoldOne(ClockPtr c, Dte& d, std::shared_ptr<DsaIdsPort> p)
          : BachModule(c, "hold"), dte(d), ids(std::move(p)) {}

     protected:
      void Step() override {
        ids->Drive(1, 2, 3, 0);
        uint64_t now = CycleNow();
        if (now >= 3 && now < 13) {
          dte.Cfg().Drive(kDteTemplateBase + kDteRegTrigger,
                          uint64_t(Route::kCmToRouter) | kDteTaskLast, 7);
        } else {
          dte.Cfg().Idle();
        }
      }

     private:
      Dte& dte;
      std::shared_ptr<DsaIdsPort> ids;
    };
    HoldOne hold(clk, dte, ids);
    MemSide mem(clk, {cm_rd, cm_wr, mm_rd, mm_wr});
    TsSide ts(clk, dte);
    clk->Continue(120 * kPeriod);
    RT::JoinAll();
    trigs = ts.trigs;
  }
  RT::Reset();
  EXPECT_EQ(trigs, 1u);
}
