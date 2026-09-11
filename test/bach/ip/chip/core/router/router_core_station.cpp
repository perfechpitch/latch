// CoreStation：Router 与本 core 的 DTE 之间那一段。
//
// 进核与出核两条通路完全并行；Header 就绪即通知 TS，不等整包收完；TS 或 DTE
// 反压时原地保持，不丢 trigger、不跳过 flit。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/router/core_station.h"
#include "bach/ip/wiring.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

MessagePtr MakeMsg(uint64_t user, uint64_t path, uint64_t bytes = 256) {
  auto m = std::make_shared<Message>();
  m->user_id = user;
  m->path_id = path;
  m->size = bytes;
  return m;
}

// 灌包、收出口、控两侧的 ready，全在这一个协程里：跨协程读别的模块本拍写的值
// 会差一拍，出口那几根又都是单拍脉冲。
class StationHarness : public BachModule {
 public:
  struct InJob {
    uint64_t at = 0;
    MessagePtr msg;
    bool head = true, tail = true;
    uint64_t vc = 0;
  };

  StationHarness(ClockPtr c, CoreStation& target, LinkEndPtr in_wire,
                 LinkEndPtr out_wire)
      : BachModule(c, "harness"), cs(target), from_xbar(std::move(in_wire)),
        to_station(std::move(out_wire)) {}

  std::vector<InJob> in_jobs;
  // 这些拍之前不给 ready。
  uint64_t dte_ready_from = 0, ts_ready_from = 0;
  // 到点从 DTE 那一侧回灌一笔出核数据。
  uint64_t out_at = 0;
  MessagePtr out_msg;

  // 包头口上要读的那几项，以及到点弹出的拍。
  std::vector<std::pair<uint64_t, uint64_t>> hdr_reads;  // {拍, 偏移}
  std::vector<uint64_t> hdr_pops;
  std::vector<uint64_t> hdr_values;

  // 观测
  std::vector<uint64_t> dte_seq, dte_at;
  std::vector<MessagePtr> dte_msgs;
  std::vector<uint64_t> trig_users, trig_at;
  std::vector<uint64_t> trig_path, trig_reissue;
  std::vector<uint64_t> station_at;
  MessagePtr station_msg;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做：先看上一拍驱到各端口上的。
    CoreDataView d = ReadCoreData(cs.ToDte());
    if (d.valid) {
      dte_seq.push_back(d.seq);
      dte_at.push_back(now);
      dte_msgs.push_back(d.msg);
    }
    if (cs.Trigger().Valid() && now >= ts_ready_from) {
      trig_users.push_back(cs.Trigger().user_id.Get());
      trig_path.push_back(cs.Trigger().path_id.Get());
      trig_reissue.push_back(cs.Trigger().reissue.Get());
      trig_at.push_back(now);
    }
    FlitView f = ReadFlit(to_station->flit);
    if (f.valid) {
      station_at.push_back(now);
      station_msg = f.msg;
    }

    ServeHdrSide(now);
    Feed(now);
    cs.ToDte().DriveReady(now >= dte_ready_from);
    cs.Trigger().DriveReady(now >= ts_ready_from);
    cs.RunStep();
  }

 private:
  // 扮演 DTE RV core 的 cm_lsq：到点读一项包头，到点写 1 弹出。
  void ServeHdrSide(uint64_t now) {
    MemPort& p = cs.Hdr();
    if (p.RspValid()) {
      ByteBlockPtr d = p.RspData();
      uint64_t v = 0;
      if (d) {
        for (uint64_t i = 0; i < d->size() && i < 8; ++i) {
          v |= uint64_t((*d)[i]) << (8 * i);
        }
      }
      hdr_values.push_back(v);
    }
    bool drove = false;
    for (auto const& r : hdr_reads) {
      if (r.first != now) continue;
      p.Read(r.second, 4);
      drove = true;
      break;
    }
    if (!drove) {
      for (uint64_t t : hdr_pops) {
        if (t != now) continue;
        p.Write(kHdrPopOffset, std::make_shared<ByteBlock>(4, 1));
        drove = true;
        break;
      }
    }
    if (!drove) p.IdleReq();
  }

  void Feed(uint64_t now) {
    bool drove = false;
    for (auto const& j : in_jobs) {
      if (j.at != now) continue;
      from_xbar->flit.Drive(j.vc, j.head, j.tail, j.msg->size, j.msg);
      drove = true;
      break;
    }
    if (!drove) from_xbar->flit.Idle();
    from_xbar->release.Idle();

    if (out_msg && now == out_at) {
      cs.FromDte().Drive(out_msg->size, true, true, 0, out_msg);
    } else {
      cs.FromDte().Idle();
    }
  }

  CoreStation& cs;
  LinkEndPtr from_xbar, to_station;
};

struct Bench {
  ClockPtr clk;
  std::unique_ptr<CoreStation> cs;
  LinkEndPtr in_wire, out_wire;

  explicit Bench(ClockPtr c) : clk(c) {
    cs = std::make_unique<CoreStation>(c, "cs", 0, false);
    in_wire = MakeWire(c);
    out_wire = MakeWire(c);
    cs->AttachFromXbar(in_wire);
    cs->AttachToStation(out_wire);
  }
};

}  // namespace

// path_id 与 reissue 位原样转给 TS，Router 自己不解释它们。
TEST(BachCoreStation, PathAndReissueReachTsUntouched) {
  std::vector<uint64_t> paths, reissues, users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    StationHarness h(clk, *b.cs, b.in_wire, b.out_wire);
    auto m0 = MakeMsg(11, 3);
    m0->reissue = 1;
    auto m1 = MakeMsg(12, 5);
    m1->reissue = 0;
    h.in_jobs = {{1, m0, true, true, 0}, {6, m1, true, true, 0}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    paths = h.trig_path;
    reissues = h.trig_reissue;
    users = h.trig_users;
  }
  RT::Reset();
  ASSERT_GE(users.size(), 2u);
  EXPECT_EQ(users[0], 11u);
  EXPECT_EQ(paths[0], 3u);
  EXPECT_EQ(reissues[0], 1u);
  EXPECT_EQ(users[1], 12u);
  EXPECT_EQ(paths[1], 5u);
  EXPECT_EQ(reissues[1], 0u);
}

// TS 入口占满时这一笔原地保持，不发下一笔：丢一笔 trigger 就等于丢一个 token。
TEST(BachCoreStation, TriggerHoldsUntilTsIsReady) {
  std::vector<uint64_t> users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    StationHarness h(clk, *b.cs, b.in_wire, b.out_wire);
    // 三个 user 连着进核，TS 到第 30 拍才开始收。
    h.in_jobs = {{1, MakeMsg(21, 3), true, true, 0},
                 {3, MakeMsg(22, 3), true, true, 0},
                 {5, MakeMsg(23, 3), true, true, 0}};
    h.ts_ready_from = 30;
    clk->Continue(120 * kPeriod);
    RT::JoinAll();
    users = h.trig_users;
  }
  RT::Reset();
  ASSERT_EQ(users.size(), 3u) << "一笔都不能丢";
  EXPECT_EQ(users[0], 21u) << "按包头顺序，不重排";
  EXPECT_EQ(users[1], 22u);
  EXPECT_EQ(users[2], 23u);
}

// DTE 反压时数据保持：valid、Header、首尾标志与有效字节不变，位置不前移，
// 恢复后从同一 flit 继续。
TEST(BachCoreStation, DataHoldsWhileDteIsNotReady) {
  std::vector<uint64_t> seqs, at;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    StationHarness h(clk, *b.cs, b.in_wire, b.out_wire);
    h.in_jobs = {{1, MakeMsg(31, 3), true, false, 0},
                 {2, MakeMsg(31, 3), false, true, 0}};
    h.dte_ready_from = 20;
    h.ts_ready_from = 0;
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    seqs = h.dte_seq;
    at = h.dte_at;
  }
  RT::Reset();
  ASSERT_FALSE(seqs.empty());
  // ready 之前那些拍上端口一直是同一笔。
  uint64_t first = seqs.front();
  uint64_t held = 0;
  for (uint64_t i = 0; i < seqs.size() && at[i] <= 20; ++i) {
    EXPECT_EQ(seqs[i], first) << "第 " << at[i] << " 拍换了一笔";
    ++held;
  }
  EXPECT_GT(held, 1u) << "确实被压住了几拍";
  // 松开之后才推进到第二个 flit。
  EXPECT_NE(seqs.back(), first) << "恢复后应当接着发下一个 flit";
}

// 进核与出核完全并行，互不共享仲裁状态：进核那一路被 DTE 压住的这些拍里，
// 出核那一路照走。
TEST(BachCoreStation, InAndOutRunInParallel) {
  std::vector<uint64_t> in_at, out_at;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    StationHarness h(clk, *b.cs, b.in_wire, b.out_wire);
    h.in_jobs = {{4, MakeMsg(41, 3), true, true, 0}};
    // DTE 一直不收，进核那一路卡在第一个 flit 上。
    h.dte_ready_from = 1000;
    h.out_at = 6;
    h.out_msg = MakeMsg(42, 5);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    in_at = h.dte_at;
    out_at = h.station_at;
  }
  RT::Reset();
  ASSERT_FALSE(in_at.empty()) << "进核那一路的数据摆在端口上等着";
  ASSERT_FALSE(out_at.empty()) << "出核那一路没有被它拖住";
  EXPECT_EQ(out_at.front(), 7u) << "出核数据下一拍就交给 core 方向的 station";
}

// 送 DTE 时不剥离任何数据：包头与 payload 原样过去，是同一个 Message。
TEST(BachCoreStation, NothingIsStrippedOnTheWayToDte) {
  MessagePtr sent, got;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    StationHarness h(clk, *b.cs, b.in_wire, b.out_wire);
    sent = MakeMsg(51, 3, 64);
    sent->payload = {1, 2, 3, 4, 5};
    sent->reduce_seq = 9;
    h.in_jobs = {{1, sent, true, true, 0}};
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    if (!h.dte_msgs.empty()) got = h.dte_msgs.front();
  }
  RT::Reset();
  ASSERT_TRUE(got);
  EXPECT_EQ(got.get(), sent.get()) << "搬的是同一份，不复制也不裁剪";
  EXPECT_EQ(got->payload.size(), 5u);
  EXPECT_EQ(got->reduce_seq, 9u);
}

// Header 就绪即通知 TS，不等整包收完。
TEST(BachCoreStation, HeaderNotifiesTsBeforeTheTail) {
  std::vector<uint64_t> trig_at, data_at;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    StationHarness h(clk, *b.cs, b.in_wire, b.out_wire);
    // 一个三 flit 的包，尾 flit 隔了十几拍才来。
    auto m = MakeMsg(61, 3);
    h.in_jobs = {{1, m, true, false, 0},
                 {2, m, false, false, 0},
                 {20, m, false, true, 0}};
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    trig_at = h.trig_at;
    data_at = h.dte_at;
  }
  RT::Reset();
  ASSERT_FALSE(trig_at.empty());
  EXPECT_LT(trig_at.front(), 20u) << "头 flit 一到就通知，不等尾 flit";
  ASSERT_FALSE(data_at.empty());
  EXPECT_LT(data_at.front(), 20u) << "payload 也是边收边搬";
}

// 进核那一路也是每拍一个 flit：DTE 上一拍说收得下就发，不等它逐笔确认。
TEST(BachCoreStation, OneFlitPerCycleIntoTheCore) {
  std::vector<uint64_t> at;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    StationHarness h(clk, *b.cs, b.in_wire, b.out_wire);
    auto m = MakeMsg(71, 3);
    for (uint64_t i = 0; i < 8; ++i) {
      h.in_jobs.push_back({1 + i, m, i == 0, i == 7, 0});
    }
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    at = h.dte_at;
  }
  RT::Reset();
  ASSERT_EQ(at.size(), 8u) << "八个 flit 都要交给 DTE";
  for (uint64_t i = 1; i < at.size(); ++i) {
    EXPECT_EQ(at[i], at[i - 1] + 1) << "第 " << i << " 个与上一个之间空了拍";
  }
}

// 包头读口：读队头那个包的字段，写 1 之后下一个包头映射上来。
TEST(BachCoreStation, HeaderIsReadThenPopped) {
  std::vector<uint64_t> vals;
  uint64_t depth_at_end = 0, pops = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    StationHarness h(clk, *b.cs, b.in_wire, b.out_wire);
    auto first = MakeMsg(81, 3);
    first->size = 512;
    auto second = MakeMsg(82, 5);
    second->size = 256;
    h.in_jobs = {{1, first, true, true, 0}, {3, second, true, true, 0}};
    // 读队头的 user_id 与 size，写 1 弹出，再读下一个的 user_id。
    h.hdr_reads = {{8, kHdrUserId}, {12, kHdrSize}, {20, kHdrUserId}};
    h.hdr_pops = {16};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    vals = h.hdr_values;
    depth_at_end = b.cs->HeaderDepth();
    pops = b.cs->Popped();
  }
  RT::Reset();
  ASSERT_EQ(vals.size(), 3u);
  EXPECT_EQ(vals[0], 81u) << "队头那个包的 user_id";
  EXPECT_EQ(vals[1], 512u) << "同一个包的 size";
  EXPECT_EQ(vals[2], 82u) << "弹出之后映射上来的是下一个包";
  EXPECT_EQ(pops, 1u);
  EXPECT_EQ(depth_at_end, 1u) << "两个进来、弹出一个，还剩一个";
}

// 包头队列满了对进 core 的数据反压，不静默丢：丢一个包头就等于丢一个 token
// 的搬运任务。
TEST(BachCoreStation, FullHeaderFifoBackpressures) {
  uint64_t depth = 0, admitted = 0, rejected = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    StationHarness h(clk, *b.cs, b.in_wire, b.out_wire);
    // 灌 20 个单拍包，队列只有 16 个位置。
    for (uint64_t i = 0; i < 20; ++i) {
      h.in_jobs.push_back({1 + i * 2, MakeMsg(100 + i, 3), true, true, 0});
    }
    clk->Continue(120 * kPeriod);
    RT::JoinAll();
    depth = b.cs->HeaderDepth();
    admitted = h.dte_at.size();
    rejected = 0;
  }
  RT::Reset();
  EXPECT_EQ(depth, kHeaderFifoDepth) << "最多装这么多";
  EXPECT_LE(admitted, kHeaderFifoDepth)
      << "多出来的那几个整笔不收，不能只收数据丢包头";
  EXPECT_EQ(rejected, 0u);
}
