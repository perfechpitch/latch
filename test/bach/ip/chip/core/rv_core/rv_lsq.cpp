// 访存的发射与写回。
//
// 按目标分三条路：DTCM 在核内、Share Mem 与 Core Mem 走对外的端口、Router 的
// 包头口复用 cm_lsq。两条队列各自顺序发射、每拍一个；写回时长延迟的那两路排在
// DTCM 前面；DTCM 同 bank 撞了就往后排一拍。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/rv_core/lsq.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

// 灌访存请求、扮演三个出口的存储、收写回。
class LsqHarness : public BachModule {
 public:
  struct Job {
    uint64_t at = 0;
    LsqTarget target = LsqTarget::kDtcm;
    bool we = false;
    uint64_t addr = 0;
    uint64_t rd = 0;
  };

  LsqHarness(ClockPtr c, RvLsq& target) : BachModule(c, "harness"), lsq(target) {}

  std::vector<Job> jobs;
  uint64_t smem_latency = 6, cmem_latency = 15;

  struct Issued {
    uint64_t at = 0, addr = 0;
    bool we = false;
  };
  std::vector<Issued> smem_ops, cmem_ops, hdr_ops;
  struct Wb {
    uint64_t at = 0, rd = 0;
    bool has_data = false;
    uint64_t data = 0;
  };
  std::vector<Wb> wbs;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做：先看上一拍写回的与发出的。
    if (lsq.WbPtr()->Valid() && lsq.WbPtr()->Seq() != last_wb_seq) {
      last_wb_seq = lsq.WbPtr()->Seq();
      wbs.push_back({now, lsq.WbPtr()->RdIdx(), lsq.WbPtr()->HasData(),
                     lsq.WbPtr()->Data()});
    }
    Serve(lsq.Smem(), smem_ops, sm_pend, smem_latency, 0, now);
    Serve(lsq.Cmem(), cmem_ops, cm_pend, cmem_latency, 1, now);
    Serve(lsq.Hdr(), hdr_ops, hdr_pend, cmem_latency, 2, now);

    Feed(now);
    lsq.RunStep();
  }

 private:
  void Serve(MemPort& p, std::vector<Issued>& log, std::vector<uint64_t>& pend,
             uint64_t lat, uint64_t which, uint64_t now) {
    MemReqView r = ReadMemReq(p);
    if (r.valid && r.seq != last_req_seq[which]) {
      last_req_seq[which] = r.seq;
      log.push_back({now, r.addr, r.we});
      pend.push_back(now + lat);
    }
    bool rsp = false;
    for (auto it = pend.begin(); it != pend.end(); ++it) {
      if (*it <= now) {
        rsp = true;
        pend.erase(it);
        break;
      }
    }
    p.DriveSlave(true, rsp,
                 rsp ? std::make_shared<ByteBlock>(4, uint8_t(which + 1))
                     : nullptr);
  }

  void Feed(uint64_t now) {
    LsqReqPort& p = *lsq.ReqPtr();
    if (holding && p.Ready()) holding = false;
    if (!holding) {
      for (uint64_t i = cursor; i < jobs.size(); ++i) {
        if (jobs[i].at > now) break;
        held = jobs[i];
        cursor = i + 1;
        holding = true;
        ++seq;
        break;
      }
    }
    if (holding) {
      p.Drive(held.target, held.we, held.addr, 0, 0xF, held.rd, seq);
    } else {
      p.Idle();
    }
  }

  RvLsq& lsq;
  std::vector<uint64_t> sm_pend, cm_pend, hdr_pend;
  std::array<uint64_t, 3> last_req_seq{};
  uint64_t last_wb_seq = 0, cursor = 0, seq = 0;
  Job held;
  bool holding = false;
};

}  // namespace

// 三个目标各走各的出口：Share Mem 与 Core Mem 分别落到自己那个端口，Router 的
// 包头口复用 cm_lsq 但换出口。
TEST(BachRvLsq, EachTargetGoesToItsOwnPort) {
  uint64_t sm = 0, cm = 0, hdr = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvLsq lsq(clk, "lsq", /*has_cm=*/true, 0, false);
    LsqHarness h(clk, lsq);
    h.jobs = {{2, LsqTarget::kShareMem, false, 0x100, 1},
              {4, LsqTarget::kCoreMem, false, 0x200, 2},
              {6, LsqTarget::kRouterIo, false, 0x300, 3}};
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    sm = h.smem_ops.size();
    cm = h.cmem_ops.size();
    hdr = h.hdr_ops.size();
  }
  RT::Reset();
  EXPECT_EQ(sm, 1u);
  EXPECT_EQ(cm, 1u);
  EXPECT_EQ(hdr, 1u) << "包头只有这一条读取通路";
}

// Share Mem 与 Core Mem 读回来的那 4 B 随写回一起补进目的寄存器。功能模型执行
// lw 那一拍从这两块拿到的是占位值，不带回来就永远读不到真内容；DTCM 是功能模型
// 自己的存储，读那一刻已经是真值，写回因此不带数据。
TEST(BachRvLsq, ReadDataComesBackWithTheWriteback) {
  std::vector<LsqHarness::Wb> wbs;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvLsq lsq(clk, "lsq", /*has_cm=*/true, 0, false);
    LsqHarness h(clk, lsq);
    h.jobs = {{2, LsqTarget::kShareMem, false, 0x100, 1},
              {4, LsqTarget::kCoreMem, false, 0x200, 2},
              {6, LsqTarget::kDtcm, false, 0x300, 3}};
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    wbs = h.wbs;
  }
  RT::Reset();
  ASSERT_EQ(wbs.size(), 3u);
  for (auto const& w : wbs) {
    if (w.rd == 1) {
      EXPECT_TRUE(w.has_data);
      EXPECT_EQ(w.data, 0x01010101u) << "Share Mem 读回来的那 4 B";
    } else if (w.rd == 2) {
      EXPECT_TRUE(w.has_data);
      EXPECT_EQ(w.data, 0x02020202u) << "Core Mem 读回来的那 4 B";
    } else {
      EXPECT_FALSE(w.has_data) << "DTCM 那一路不带数据";
    }
  }
}

// 队列顺序发射，每拍最多一个：连着来的几笔按到达顺序出去。
TEST(BachRvLsq, QueueIssuesInOrderOneEachCycle) {
  std::vector<uint64_t> addrs, ats;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvLsq lsq(clk, "lsq", true, 0, false);
    LsqHarness h(clk, lsq);
    for (uint64_t i = 0; i < 4; ++i) {
      h.jobs.push_back({2 + i * 2, LsqTarget::kShareMem, false,
                        0x100 + i * 4, i + 1});
    }
    clk->Continue(120 * kPeriod);
    RT::JoinAll();
    for (auto const& o : h.smem_ops) {
      addrs.push_back(o.addr);
      ats.push_back(o.at);
    }
  }
  RT::Reset();
  ASSERT_EQ(addrs.size(), 4u);
  for (uint64_t i = 0; i < 4; ++i) {
    EXPECT_EQ(addrs[i], 0x100u + i * 4) << "第 " << i << " 笔顺序不对";
  }
  for (uint64_t i = 1; i < ats.size(); ++i) {
    EXPECT_GT(ats[i], ats[i - 1]) << "一拍只发一个";
  }
}

// DTCM 在核内，读出来的数据三拍后写回。
//
// 「同 bank 冲突阻塞第二条」这一条在这一层看不到：访存请求口一拍只收一笔，
// 而每拍最多执行一条指令，两笔请求本来就落在相邻的两拍上，撞不到一起。
TEST(BachRvLsq, DtcmTakesThreeCycles) {
  std::vector<uint64_t> at;
  uint64_t ops = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvLsq lsq(clk, "lsq", true, 0, false);
    LsqHarness h(clk, lsq);
    h.jobs = {{2, LsqTarget::kDtcm, false, 0x0, 1},
              {6, LsqTarget::kDtcm, false, 0x4, 2}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    for (auto const& w : h.wbs) at.push_back(w.at);
    ops = h.smem_ops.size() + h.cmem_ops.size() + h.hdr_ops.size();
  }
  RT::Reset();
  ASSERT_EQ(at.size(), 2u) << "两笔都要写回";
  EXPECT_GT(at[1], at[0]) << "按发出的顺序写回";
  EXPECT_EQ(ops, 0u) << "DTCM 在核内，不走对外的端口";
}

// 写回优先长延迟的那两路：Share Mem 与 DTCM 同拍要写回时先写 Share Mem。
TEST(BachRvLsq, LongLatencyPathsWriteBackFirst) {
  std::vector<uint64_t> order;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvLsq lsq(clk, "lsq", true, 0, false);
    LsqHarness h(clk, lsq);
    h.smem_latency = 6;
    // Share Mem 那一笔早发，DTCM 那一笔晚发，让两者的写回撞在同一拍附近。
    h.jobs = {{2, LsqTarget::kShareMem, false, 0x100, 7},
              {5, LsqTarget::kDtcm, false, 0x0, 9}};
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    for (auto const& w : h.wbs) order.push_back(w.rd);
  }
  RT::Reset();
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], 7u) << "Share Mem 那一笔先写回";
  EXPECT_EQ(order[1], 9u);
}

// 写请求不写回：只有读才占写回口。
TEST(BachRvLsq, WritesDoNotWriteBack) {
  uint64_t wbs = 0, ops = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvLsq lsq(clk, "lsq", true, 0, false);
    LsqHarness h(clk, lsq);
    h.jobs = {{2, LsqTarget::kShareMem, /*we=*/true, 0x100, 0}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    wbs = h.wbs.size();
    ops = h.smem_ops.size();
  }
  RT::Reset();
  EXPECT_EQ(ops, 1u) << "请求要发出去";
  EXPECT_EQ(wbs, 0u) << "写不占写回口";
}

// 没有 cm_lsq 的那两个核不许发 Core Mem 的访存。
TEST(BachRvLsq, CoreWithoutCmLsqRejectsCoreMemAccess) {
  bool has_room_with = false, has_room_without = true;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RvLsq with_cm(clk, "with", /*has_cm=*/true, 0, false);
    RvLsq without(clk, "without", /*has_cm=*/false, 0, false);
    has_room_with = with_cm.HasRoom(LsqTarget::kCoreMem);
    has_room_without = without.HasRoom(LsqTarget::kCoreMem);
  }
  RT::Reset();
  EXPECT_TRUE(has_room_with) << "DTE core 有 cm_lsq";
  EXPECT_FALSE(has_room_without) << "另外两个核没有";
}
