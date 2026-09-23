// DTE 的行为基线。
//
// 对齐飞书《DTE DSA》后，任务统一从 RV core 的配置入口来（写 CFG 寄存器 + trigger），
// 进核不再是包驱动：Header Parser 只存包头 + 转发 payload，数据落点由 CFG_ADDRx_DST
// 配。进核任务与数据包按到达顺序 FIFO 配对（第 N 个进核任务配第 N 个到达的包）。
//
// 重点覆盖：进核搬进 Core Mem、scale 段落 scale 旁带、纯包头任务、非法帧丢弃、
// Commit 三样一起拿（挡住半任务）、Join 两侧都满足才报完成、Buffer 满反压 Router。

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

// 一笔任务的 CFG_TRANS_MODE：低 3 位 route，[6:3] addr_valid，[9] ack_ts_en。
uint64_t TransMode(Route r, uint64_t addr_valid, bool ack = true) {
  return uint64_t(r) | (addr_valid << kDteAddrValidShift) |
         (ack ? kDteAckTsEn : 0);
}

// 扮演 Router 的 CoreStation：按脚本在指定拍送一串整包（每个包可多拍）。反压时
// 保持数据不变，看见 ready 才换下一拍。端口带序号，保持期间接收方不会消费两遍。
class FrameFeeder : public BachModule {
 public:
  struct Item {
    uint64_t at = 0;
    MessagePtr msg;
  };

  FrameFeeder(ClockPtr c, Dte& target, std::vector<Item> seq)
      : BachModule(c, "feeder"), dte(target), items(std::move(seq)) {}

  uint64_t sent = 0;  // 已送出的拍数

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    CoreDataPort& p = dte.FromRouter();

    if (driving) {
      if (!p.Ready()) return;  // 反压：这一拍保持，不换
      ++sent;
      ++beat;
      driving = false;
    }

    // 没有包在发：找下一个到点的包。
    if (!msg) {
      while (cursor < items.size() && items[cursor].at <= now) {
        msg = items[cursor].msg;
        total = FlitsOf(msg->size);
        beat = 0;
        ++cursor;
        break;
      }
      if (!msg) {
        p.Idle();
        return;
      }
    }

    if (beat >= total) {  // 这一包发完了
      msg.reset();
      p.Idle();
      return;
    }
    uint64_t left = msg->size - beat * kFlitBytes;
    uint64_t n = left > kFlitBytes ? kFlitBytes : left;
    bool last = (beat + 1 == total);
    p.Drive(n, last, beat == 0, 0, msg);
    driving = true;
  }

 private:
  Dte& dte;
  std::vector<Item> items;
  uint64_t cursor = 0, beat = 0, total = 0;
  bool driving = false;
  MessagePtr msg;
};

// 扮演 DTE RV core：按脚本逐笔写寄存器（每笔保持到被收下），每个任务最后一笔是
// 写 CFG_TRIGGER；每拍驱动四个直连身份信号。配置驱动进核/出核的统一入口。
class RvCfgDriver : public BachModule {
 public:
  struct Wr {
    uint64_t addr = 0, data = 0;
  };
  struct Task {
    uint64_t stream = 0, task = 0, user = 0, path = 0;
    std::vector<Wr> wr;
  };

  RvCfgDriver(ClockPtr c, Dte& target, std::shared_ptr<DsaIdsPort> p,
              std::vector<Task> cfg, uint64_t start = 0)
      : BachModule(c, "rv"), dte(target), ids(std::move(p)),
        tasks(std::move(cfg)), start_at(start) {}

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    DsaCfgPort& cfg = dte.Cfg();
    if (now < start_at || task_cursor >= tasks.size()) {
      ids->Drive(0, 0, 0, 0);
      cfg.Idle();
      return;
    }
    Task const& t = tasks[task_cursor];
    ids->Drive(t.stream, t.task, t.user, t.path);

    if (driving) {
      if (!cfg.Ready()) {  // 没被收下就保持同一笔
        cfg.Drive(held_addr, held_data, held_seq);
        return;
      }
      driving = false;
    }
    if (wr_cursor < t.wr.size()) {
      held_addr = t.wr[wr_cursor].addr;
      held_data = t.wr[wr_cursor].data;
      held_seq = ++next_seq;
      ++wr_cursor;
      driving = true;
      cfg.Drive(held_addr, held_data, held_seq);
      return;
    }
    // 这一笔任务写完，切下一个。
    ++task_cursor;
    wr_cursor = 0;
    cfg.Idle();
  }

 private:
  Dte& dte;
  std::shared_ptr<DsaIdsPort> ids;
  std::vector<Task> tasks;
  uint64_t start_at;
  uint64_t task_cursor = 0, wr_cursor = 0;
  uint64_t next_seq = 0;
  bool driving = false;
  uint64_t held_addr = 0, held_data = 0, held_seq = 0;
};

// 进核一笔纯数据任务的配置脚本：段 1 = 数据，落 dst，长 len，完成后通知 TS。
RvCfgDriver::Task InboundTask(uint64_t stream, uint64_t task, uint64_t user,
                              uint64_t dst, uint64_t len) {
  RvCfgDriver::Task t;
  t.stream = stream;
  t.task = task;
  t.user = user;
  t.path = 0;
  t.wr = {{kDteRegAddr1Dst, dst},
          {kDteRegDataLen1, len},
          {kDteRegTransMode, TransMode(Route::kRouterToCm, 1u << 1)},
          {kDteRegTrigger, 0}};
  return t;
}

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

// 挂四块存储口（不接 CoreMem，只计数）。
void AttachDummyMem(Dte& dte, ClockPtr clk, std::vector<std::shared_ptr<MemPort>>& out) {
  out.push_back(std::make_shared<MemPort>(clk));
  out.push_back(std::make_shared<MemPort>(clk));
  out.push_back(std::make_shared<MemPort>(clk));
  out.push_back(std::make_shared<MemPort>(clk));
  dte.AttachCmemRd(out[0]);
  dte.AttachCmemWr(out[1]);
  dte.AttachMmemRd(out[2]);
  dte.AttachMmemWr(out[3]);
}

}  // namespace

// Router → CM：配置起一笔进核任务，一个整包进来，搬进 Core Mem，两侧 Join 后向 TS
// 报一次完成。
TEST(BachDte, InboundRouterToCoreMem) {
  uint64_t dones = 0, writes = 0, parsed = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Dte dte(clk, "dte", DteCfg{});
    std::vector<std::shared_ptr<MemPort>> mem;
    AttachDummyMem(dte, clk, mem);
    auto ids = std::make_shared<DsaIdsPort>(clk);
    dte.AttachIds(ids);

    RvCfgDriver rv(clk, dte, ids,
                   {InboundTask(/*stream=*/0, /*task=*/0, /*user=*/11,
                                /*dst=*/0x100, /*len=*/512)},
                   2);
    FrameFeeder feed(clk, dte, {{20, MakeMsg(11, 0, 512)}});
    MemSide m(clk, mem);
    TsSide ts(clk, dte);
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    dones = ts.dones;
    writes = m.writes;
    parsed = ts.parsed;
  }
  RT::Reset();
  EXPECT_EQ(parsed, 1u);   // 解析了一个 Header
  EXPECT_GT(writes, 0u);   // 数据写进了存储
  EXPECT_EQ(dones, 1u);    // exactly-once：只报一次
}

// 带 scale 的包：payload 是 MXFP8 数据后面接 scale。数据段落 Core Mem，scale 段
// 落同一段地址的 scale 旁带，每 32 B 数据一个。分段由 CFG_ADDR2_DST 的端点 tag
// （0x2 = scale）决定，不再看包头的 scale_valid。
TEST(BachDte, InboundScaleLandsInScaleSideband) {
  constexpr uint64_t kData = 512;
  constexpr uint64_t kAt = 0x80;
  std::vector<uint8_t> data(kData), scale(kData / 32);
  for (uint64_t i = 0; i < kData; ++i) data[i] = uint8_t(i * 7 + 1);
  for (uint64_t i = 0; i < scale.size(); ++i) scale[i] = uint8_t(120 + i);
  uint64_t dones = 0;
  std::vector<uint8_t> got_data, got_scale;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Dte dte(clk, "dte", DteCfg{});
    CoreMem cmem(clk, "cmem");
    dte.AttachCmemRd(cmem.PortPtr(kCmemDteRd));
    dte.AttachCmemWr(cmem.PortPtr(kCmemDteWr));
    auto mm_rd = std::make_shared<MemPort>(clk);
    auto mm_wr = std::make_shared<MemPort>(clk);
    dte.AttachMmemRd(mm_rd);
    dte.AttachMmemWr(mm_wr);
    auto ids = std::make_shared<DsaIdsPort>(clk);
    dte.AttachIds(ids);

    // 段 1 = 数据（512 B），段 2 = scale（16 组，端点 tag 0x2，落点同数据地址）。
    RvCfgDriver::Task t;
    t.stream = 0;
    t.task = 0;
    t.user = 11;
    t.path = 0;
    t.wr = {{kDteRegAddr1Dst, kAt},
            {kDteRegDataLen1, kData},
            {kDteRegAddr2Dst, (uint64_t(SegEndpoint::kScale) << kEpShift) | kAt},
            {kDteRegDataLen2, scale.size()},
            {kDteRegTransMode, TransMode(Route::kRouterToCm, (1u << 1) | (1u << 2))},
            {kDteRegTrigger, 0}};
    RvCfgDriver rv(clk, dte, ids, {t}, 2);

    MessagePtr m = MakeMsg(11, 0, kData + scale.size());
    m->payload = data;
    m->payload.insert(m->payload.end(), scale.begin(), scale.end());
    FrameFeeder feed(clk, dte, {{20, m}});
    MemSide mem(clk, {mm_rd, mm_wr});
    TsSide ts(clk, dte);
    clk->Continue(300 * kPeriod);
    RT::JoinAll();
    dones = ts.dones;
    got_data = cmem.Peek(kAt, kData);
    got_scale = cmem.PeekScale(kAt, scale.size());
  }
  RT::Reset();
  EXPECT_EQ(dones, 1u);
  EXPECT_EQ(got_data, data) << "数据那一段";
  EXPECT_EQ(got_scale, scale) << "包尾那一段进 scale 旁带";
}

// 纯包头任务：配置只有 route + ack_ts_en、没有数据段，Header beat 同时带 TLAST，
// 照样走完并报完成。
TEST(BachDte, HeaderOnlyTask) {
  uint64_t dones = 0, parsed = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Dte dte(clk, "dte", DteCfg{});
    std::vector<std::shared_ptr<MemPort>> mem;
    AttachDummyMem(dte, clk, mem);
    auto ids = std::make_shared<DsaIdsPort>(clk);
    dte.AttachIds(ids);

    RvCfgDriver::Task t;
    t.stream = 0;
    t.task = 0;
    t.user = 12;
    t.path = 0;
    t.wr = {{kDteRegTransMode, TransMode(Route::kRouterToCm, 0)},
            {kDteRegTrigger, 0}};
    RvCfgDriver rv(clk, dte, ids, {t}, 2);
    FrameFeeder feed(clk, dte, {{20, MakeMsg(12, 0, 0)}});
    MemSide m(clk, mem);
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

// 非法 Header 进 Drop Frame：不存包头、不发存储请求，只消费到 TLAST。
TEST(BachDte, IllegalHeaderDropsFrame) {
  uint64_t parsed = 0, dropped = 0, writes = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Dte dte(clk, "dte", DteCfg{});
    std::vector<std::shared_ptr<MemPort>> mem;
    AttachDummyMem(dte, clk, mem);
    // 超过单任务上限 32 KB，这一帧要被丢掉
    FrameFeeder feed(clk, dte, {{2, MakeMsg(13, 0, 64 * 1024)}});
    MemSide m(clk, mem);
    TsSide ts(clk, dte);
    clk->Continue(400 * kPeriod);
    RT::JoinAll();
    parsed = ts.parsed;
    dropped = ts.dropped;
    writes = m.writes;
  }
  RT::Reset();
  EXPECT_EQ(parsed, 0u);
  EXPECT_EQ(dropped, 1u);
  EXPECT_EQ(writes, 0u);   // 丢的帧不发存储请求
}

// Commit 三样一起拿：Completion RS 占满之后不再 dispatch，挡住半任务。
TEST(BachDte, CommitNeedsAllThreeResources) {
  uint64_t admitted = 0, stalled = 0, peak = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Dte dte(clk, "dte", DteCfg{});
    std::vector<std::shared_ptr<MemPort>> mem;
    AttachDummyMem(dte, clk, mem);

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
    DeadMem mem_dead(clk, mem);
    auto ids = std::make_shared<DsaIdsPort>(clk);
    dte.AttachIds(ids);

    // 连着配 30 个纯包头任务，把 RS 占满（RS 只有 16 项）。
    std::vector<RvCfgDriver::Task> tasks;
    for (uint64_t i = 0; i < 30; ++i) {
      RvCfgDriver::Task t;
      t.stream = i % 8;
      t.task = i;
      t.user = 100 + i;
      t.path = 0;
      t.wr = {{kDteRegTransMode, TransMode(Route::kRouterToCm, 0)},
              {kDteRegTrigger, 0}};
      tasks.push_back(t);
    }
    RvCfgDriver rv(clk, dte, ids, tasks, 2);

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
    clk->Continue(400 * kPeriod);
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

// Join：一笔任务的两侧都满足才报完成，不会报两次。三笔走同一个 path，认哪两半
// 属于同一笔靠的是 Commit 分配的内部序号。三个进核任务与三个包按到达顺序 FIFO
// 配对。
TEST(BachDte, JoinReportsExactlyOnce) {
  uint64_t dones = 0, joined = 0;
  std::vector<uint64_t> tasks;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Dte dte(clk, "dte", DteCfg{});
    std::vector<std::shared_ptr<MemPort>> mem;
    AttachDummyMem(dte, clk, mem);
    auto ids = std::make_shared<DsaIdsPort>(clk);
    dte.AttachIds(ids);

    std::vector<RvCfgDriver::Task> cfg;
    std::vector<FrameFeeder::Item> pkts;
    for (uint64_t s = 0; s < 3; ++s) {
      cfg.push_back(InboundTask(s, s, 200 + s, 0x200 + s * 0x1000, 16));
      pkts.push_back({40 + s * 10, MakeMsg(200 + s, 0, 16, /*stream=*/s, /*task=*/s)});
    }
    RvCfgDriver rv(clk, dte, ids, cfg, 2);
    FrameFeeder feed(clk, dte, pkts);
    MemSide m(clk, mem);
    TsSide ts(clk, dte);

    clk->Continue(400 * kPeriod);
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
    std::vector<std::shared_ptr<MemPort>> mem;
    AttachDummyMem(dte, clk, mem);

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
    DeadMem mem_dead(clk, mem);
    auto ids = std::make_shared<DsaIdsPort>(clk);
    dte.AttachIds(ids);

    // 一个很大的包：256 B × 100 拍，buffer 只有 32 拍
    constexpr uint64_t kBig = 100 * 256;
    RvCfgDriver rv(clk, dte, ids,
                   {InboundTask(0, 0, 30, 0x400, kBig)}, 2);
    FrameFeeder feed(clk, dte, {{20, MakeMsg(30, 0, kBig)}});
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
    std::vector<std::shared_ptr<MemPort>> mem;
    AttachDummyMem(dte, clk, mem);
    auto ids = std::make_shared<DsaIdsPort>(clk);
    dte.AttachIds(ids);

    // 扮演 DTE RV core：每拍驱动身份，照《DTE DSA》的顺序写段 1 配置、CFG_TRANS_MODE
    // 再写 CFG_TRIGGER 提交。
    class RvSide : public BachModule {
     public:
      RvSide(ClockPtr c, Dte& d, std::shared_ptr<DsaIdsPort> p)
          : BachModule(c, "rv"), dte(d), ids(std::move(p)) {}

     protected:
      void Step() override {
        ids->Drive(/*stream=*/6, /*task=*/9, /*user=*/77, /*path=*/2);
        uint64_t now = CycleNow();
        DsaCfgPort& cfg = dte.Cfg();
        if (now == 3) {
          cfg.Drive(kDteRegAddr1Src, 0, 1);    // 段 1 源地址
        } else if (now == 4) {
          cfg.Drive(kDteRegAddr1Dst, 0, 2);    // 段 1 目的地址
        } else if (now == 5) {
          cfg.Drive(kDteRegDataLen1, 256, 3);  // 段 1 长度，字节
        } else if (now == 6) {
          // transfer_mode = 010（Cmem → Router）+ addr_valid[1]（段 1 参与）
          // + ack_ts_en（完成后通知 TS）。
          uint64_t trans = uint64_t(Route::kCmToRouter) |
                           (1u << (kDteAddrValidShift + 1)) | kDteAckTsEn;
          cfg.Drive(kDteRegTransMode, trans, 4);
        } else if (now == 7) {
          cfg.Drive(kDteRegTrigger, 0, 5);     // 写 CFG_TRIGGER 提交任务
        } else {
          cfg.Idle();
        }
      }

     private:
      Dte& dte;
      std::shared_ptr<DsaIdsPort> ids;
    };
    RvSide rv(clk, dte, ids);
    MemSide m(clk, mem);
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
    std::vector<std::shared_ptr<MemPort>> mem;
    AttachDummyMem(dte, clk, mem);
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
          dte.Cfg().Drive(kDteRegTrigger, 0, 7);
        } else {
          dte.Cfg().Idle();
        }
      }

     private:
      Dte& dte;
      std::shared_ptr<DsaIdsPort> ids;
    };
    HoldOne hold(clk, dte, ids);
    MemSide m(clk, mem);
    TsSide ts(clk, dte);
    clk->Continue(120 * kPeriod);
    RT::JoinAll();
    trigs = ts.trigs;
  }
  RT::Reset();
  EXPECT_EQ(trigs, 1u);
}
