// TS 的行为基线。
//
// 步 6 的判据在头两个用例：单 stream 单 task 从 trigger 到 retire 走完，
// 以及六个写口的冲突。
//
// 其余覆盖 SKIP_MASK 一拍跳过、reduce 两半按 reduce_seq 配对、Head-only 退休、
// 表满时反压 trigger 这几条最容易实现错的。

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/ts/ts.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

// TS 是九个模块，加驱动就超过默认的 8 个协程槽位。
void EnsureSlots() { RT::Reset(8, 8); }

TaskEntry MakeTask(SendUnit unit, bool end, bool dsa_en = true) {
  TaskEntry t;
  t.send_unit = unit;
  t.recv_unit = dsa_en ? RecvUnit::kDsa : RecvUnit::kRvOnly;
  t.dsa_en = dsa_en;
  t.end = end;
  t.exe_mask = true;   // 不按用户区分，所有用户都做
  t.task_pc = 0x100;
  return t;
}

// 扮演 Router：在指定拍推一笔 trigger，看见 ready 就撤。
class RouterSide : public BachModule {
 public:
  RouterSide(ClockPtr c, Ts& sched, uint64_t fire_at, uint64_t uid,
             uint64_t pid = 0)
      : BachModule(c, "router"), ts(sched), at(fire_at), user(uid), path(pid) {}

  bool sent = false;
  uint64_t retire_seen = 0;
  uint64_t retire_user = 0;
  uint64_t credit_req_seen = 0;
  bool retire_hold = false;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    if (!sent && now >= at) {
      ts.Trigger().Drive(user, path, /*reissue=*/false, /*compute=*/true);
      if (ts.Trigger().Ready()) sent = true;
    } else {
      ts.Trigger().Idle();
    }
    // Router 侧一律收下退休请求。
    //
    // 一次 valid/ready 握手最少两拍：发送方拉 valid，接收方下一拍拉 accepted，
    // 发送方再下一拍才看得到并撤 valid。所以同一笔会连着两拍出现在端口上，按
    // user_id 认它，别数成两笔。
    if (ts.Credit().RetireReq().Valid()) {
      uint64_t u = ts.Credit().RetireReq().user_id.Get();
      if (!retire_hold || retire_user != u) {
        ++retire_seen;
        retire_user = u;
        retire_hold = true;
      }
    } else {
      retire_hold = false;
    }
    ts.Credit().RetireReq().DriveAccepted(true);
    // credit 申请也一律收下，但不授予（除非测试另接）。
    if (ts.Credit().Req().Valid()) ++credit_req_seen;
    ts.Credit().Req().DriveReady(true);
    ts.Credit().Grant().Idle();
    ts.Done().RdcDone().Idle();
  }

 private:
  Ts& ts;
  uint64_t at, user, path;
};

// 扮演三个 RV core：收 task，隔几拍报完成。
class UnitSide : public BachModule {
 public:
  UnitSide(ClockPtr c, Ts& sched, uint64_t lat, uint64_t at = 0,
           uint64_t which = 0)
      : BachModule(c, "units"), ts(sched), latency(lat), inject_at(at),
        inject_task(which) {}

  uint64_t dte_cmds = 0, mu_cmds = 0, vu_cmds = 0;
  std::vector<uint64_t> dte_tasks;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    Serve(ts.DteCmd(), 0, dte_cmds, now, &dte_tasks);
    Serve(ts.MuCmd(), 1, mu_cmds, now, nullptr);
    Serve(ts.VuCmd(), 2, vu_cmds, now, nullptr);
    for (uint64_t u = 0; u < 3; ++u) {
      ts.RvDone(u).Idle();
      // 注入一笔「这个 task 已经完成」：datain 任务的完成与主线走到哪无关，
      // 数据到了就报，所以它可以在主线还没走到那一项时先回来。
      if (u == 0 && inject_at != 0 && now == inject_at) {
        ts.DsaDone(0).Drive(0, inject_task);
      } else if (fire[u] != 0 && now >= fire[u]) {
        ts.DsaDone(u).Drive(stream[u], task[u]);
        fire[u] = 0;
      } else {
        ts.DsaDone(u).Idle();
      }
    }
  }

 private:
  void Serve(TaskCmdPort& cmd, uint64_t u, uint64_t& cnt, uint64_t now,
             std::vector<uint64_t>* log) {
    // 同一笔会连着两拍出现在端口上（TS 要等 ready 打一拍才撤 valid），按
    // (stream, task) 认它，不重复受理。
    if (cmd.Valid() && fire[u] == 0) {
      uint64_t s = cmd.stream_id.Get();
      uint64_t t = cmd.task_id.Get();
      uint64_t key = s * kTaskChainNum + t + 1;
      if (key != last_key[u]) {
        ++cnt;
        stream[u] = s;
        task[u] = t;
        last_key[u] = key;
        if (log) log->push_back(t);
        fire[u] = now + latency;
      }
    }
    cmd.DriveReady(fire[u] == 0 || fire[u] > now);
  }

  Ts& ts;
  uint64_t latency;
  uint64_t inject_at = 0, inject_task = 0;
  std::array<uint64_t, 3> fire{}, stream{}, task{}, last_key{};
};

}  // namespace

// 步 6 的判据：单 stream 单 task 从 trigger 到 retire 走完。
TEST(BachTs, OneUserRunsFromTriggerToRetire) {
  uint64_t dte_cmds = 0, retire_seen = 0, retire_user = 0;
  uint64_t head = 0, tail = 0;
  bool slot_free = false;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Ts ts(clk, "ts", TsCfg{});
    ts.Cfg().WriteTask(0, MakeTask(SendUnit::kDte, /*end=*/true));
    ts.Cfg().SetInitFinish();

    RouterSide router(clk, ts, /*at=*/2, /*user=*/77);
    UnitSide units(clk, ts, /*latency=*/3);
    clk->Continue(80 * kPeriod);
    RT::JoinAll();

    dte_cmds = units.dte_cmds;
    retire_seen = router.retire_seen;
    retire_user = router.retire_user;
    head = ts.Table().HeadPtr();
    tail = ts.Table().TailPtr();
    slot_free = !ts.Table().Peek(0).valid;
  }
  RT::Reset();
  EXPECT_EQ(dte_cmds, 1u);      // 下发了一笔
  EXPECT_EQ(retire_seen, 1u);   // 走到了退休
  EXPECT_EQ(retire_user, 77u);
  EXPECT_TRUE(slot_free);       // Router 收下后才清 valid
  EXPECT_EQ(head, tail);        // 推了 head_ptr，表空了
}

// 一条三步的链：DTE → MU → VU，逐步推进，最后退休。
TEST(BachTs, WalksTheWholeChain) {
  uint64_t dte = 0, mu = 0, vu = 0, retire = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Ts ts(clk, "ts", TsCfg{});
    ts.Cfg().WriteTask(0, MakeTask(SendUnit::kDte, false));
    ts.Cfg().WriteTask(1, MakeTask(SendUnit::kMu, false));
    ts.Cfg().WriteTask(2, MakeTask(SendUnit::kVu, true));
    ts.Cfg().SetInitFinish();

    RouterSide router(clk, ts, 2, 88);
    UnitSide units(clk, ts, 2);
    clk->Continue(150 * kPeriod);
    RT::JoinAll();
    dte = units.dte_cmds;
    mu = units.mu_cmds;
    vu = units.vu_cmds;
    retire = router.retire_seen;
  }
  RT::Reset();
  EXPECT_EQ(dte, 1u);
  EXPECT_EQ(mu, 1u);
  EXPECT_EQ(vu, 1u);
  EXPECT_EQ(retire, 1u);
}

// 六个写口的冲突：同一个 stream 同一拍来两笔，高优先级的先写，低的下一拍再来。
TEST(BachTs, WritePortPriorityOnSameStream) {
  uint64_t writes = 0, conflicts = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    StreamTable table(clk, "table", kStreamNum);

    // 手动接两个口，同一拍写同一个 stream。install 的优先级高于 create。
    class TwoWriters : public BachModule {
     public:
      TwoWriters(ClockPtr c, StreamTable& target)
          : BachModule(c, "tw"), tab(target) {}
      uint64_t install_ok = 0, create_ok = 0;

     protected:
      void Step() override {
        uint64_t now = CycleNow();
        if (now == 1) {
          auto a = std::make_shared<StreamWrite>();
          a->valid = true;
          a->stream_id = 0;
          a->whole = true;
          a->entry.valid = true;
          a->entry.task_id = 5;
          tab.Port(kWrInstall).Drive(a);

          auto b = std::make_shared<StreamWrite>();
          b->valid = true;
          b->stream_id = 0;
          b->whole = true;
          b->entry.valid = true;
          b->entry.task_id = 9;
          tab.Port(kWrCreate).Drive(b);
        } else if (now == 3) {
          // accepted 是 Stream_table 上一拍写的，所以要晚一拍读
          if (tab.Port(kWrInstall).Accepted()) ++install_ok;
          if (tab.Port(kWrCreate).Accepted()) ++create_ok;
          tab.Port(kWrInstall).Idle();
        } else if (now > 3) {
          if (tab.Port(kWrCreate).Accepted()) ++create_ok;
          tab.Port(kWrCreate).Idle();
          tab.Port(kWrInstall).Idle();
        }
      }

     private:
      StreamTable& tab;
    };
    TwoWriters tw(clk, table);
    clk->Continue(20 * kPeriod);
    RT::JoinAll();
    writes = tw.install_ok;
    conflicts = tw.create_ok;
  }
  RT::Reset();
  // install 优先级高，第一拍就写进去；create 被挡，下一拍才成
  EXPECT_EQ(writes, 1u);
  EXPECT_EQ(conflicts, 1u);
}

// SKIP_MASK：异步 datain 已经提前完成时，主线走到那一项直接跳过。
TEST(BachTs, SkipsCompletedDatainTask) {
  std::vector<uint64_t> tasks;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Ts ts(clk, "ts", TsCfg{});
    // task0 普通、task1 是 datain、task2 普通收尾
    ts.Cfg().WriteTask(0, MakeTask(SendUnit::kDte, false));
    TaskEntry din = MakeTask(SendUnit::kDte, false);
    din.wait_wake = true;   // 这一项是 datain
    ts.Cfg().WriteTask(1, din);
    ts.Cfg().WriteTask(2, MakeTask(SendUnit::kDte, true));
    ts.Cfg().SetInitFinish();

    RouterSide router(clk, ts, 2, 99);
    // 第 8 拍注入一笔 task1 的完成：datain 提前到了，done_bitmap 的第 1 位先亮，
    // 而主线的 task_id 还停在 0。
    UnitSide units(clk, ts, 2, /*inject_at=*/8, /*inject_task=*/1);

    clk->Continue(150 * kPeriod);
    RT::JoinAll();
    tasks = units.dte_tasks;
  }
  RT::Reset();
  // 三项都是 DTE，但 task1 被跳过，只发了 0 与 2
  ASSERT_GE(tasks.size(), 2u);
  EXPECT_EQ(tasks[0], 0u);
  EXPECT_EQ(tasks[1], 2u);
}

// 表满时拉低 trigger 的 ready，不丢请求。
TEST(BachTs, BackpressuresTriggerWhenTableIsFull) {
  uint64_t created = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    TsCfg cfg;
    Ts ts(clk, "ts", cfg);
    ts.Cfg().SetStreamNum(2);   // 只给两个坑
    // 一条永远不完成的链：没有 RV core 来 ack，表项一直占着
    ts.Cfg().WriteTask(0, MakeTask(SendUnit::kDte, true));
    ts.Cfg().SetInitFinish();

    // 连着灌四个不同的用户
    class ManyUsers : public BachModule {
     public:
      ManyUsers(ClockPtr c, Ts& sched) : BachModule(c, "mu"), ts(sched) {}
      uint64_t sent = 0;

     protected:
      void Step() override {
        if (sent < 4) {
          ts.Trigger().Drive(100 + sent, 0, false, true);
          if (ts.Trigger().Ready()) ++sent;
        } else {
          ts.Trigger().Idle();
        }
        ts.Credit().RetireReq().DriveAccepted(true);
        ts.Credit().Req().DriveReady(true);
        ts.Credit().Grant().Idle();
        ts.Done().RdcDone().Idle();
        ts.DteCmd().DriveReady(false);   // RV core 一直不收
        ts.MuCmd().DriveReady(false);
        ts.VuCmd().DriveReady(false);
        for (uint64_t u = 0; u < 3; ++u) {
          ts.RvDone(u).Idle();
          ts.DsaDone(u).Idle();
        }
      }

     private:
      Ts& ts;
    };
    ManyUsers mu(clk, ts);
    clk->Continue(100 * kPeriod);
    RT::JoinAll();
    uint64_t used = 0;
    for (uint64_t i = 0; i < kStreamNum; ++i) {
      if (ts.Table().Peek(i).valid) ++used;
    }
    created = used;
  }
  RT::Reset();
  // 只有两个坑，第三个用户在上游等
  EXPECT_EQ(created, 2u);
}

// reduce 的两半按 reduce_seq 一一配对：N=2 时两张位图的低 2 位都满才 FINISH。
TEST(BachTs, ReducePairsBySeq) {
  uint64_t finished_early = 0, finished_late = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Ts ts(clk, "ts", TsCfg{});
    TaskEntry red = MakeTask(SendUnit::kDte, true);
    red.reduce = true;
    red.reduce_num = 2;
    red.recv_unit = RecvUnit::kDteDsaRmem;
    ts.Cfg().WriteTask(0, red);
    ts.Cfg().SetInitFinish();

    class ReduceDriver : public BachModule {
     public:
      ReduceDriver(ClockPtr c, Ts& sched)
          : BachModule(c, "rd"), ts(sched) {}
      uint64_t at_15 = 0, at_60 = 0;

     protected:
      void Step() override {
        uint64_t now = CycleNow();
        // 先建表
        if (now < 4) {
          ts.Trigger().Drive(55, 0, false, true);
        } else {
          ts.Trigger().Idle();
        }
        ts.Credit().RetireReq().DriveAccepted(true);
        ts.Credit().Req().DriveReady(true);
        ts.Credit().Grant().Idle();
        ts.DteCmd().DriveReady(true);
        ts.MuCmd().DriveReady(true);
        ts.VuCmd().DriveReady(true);
        for (uint64_t u = 0; u < 3; ++u) ts.RvDone(u).Idle();

        // 第 10 拍：DTE ack seq0；第 12 拍：Router done seq0
        // 第 40 拍：DTE ack seq1；第 45 拍：Router done seq1
        ts.DsaDone(0).Idle();
        ts.Done().RdcDone().Idle();
        if (now == 10) ts.DsaDone(0).Drive(0, 0, /*seq=*/0);
        if (now == 12) ts.Done().RdcDone().Drive(55, 0);
        if (now == 40) ts.DsaDone(0).Drive(0, 0, /*seq=*/1);
        if (now == 45) ts.Done().RdcDone().Drive(55, 1);

        if (now == 30) at_15 = ts.Done().Finished();
        if (now == 70) at_60 = ts.Done().Finished();
      }

     private:
      Ts& ts;
    };
    ReduceDriver rd(clk, ts);
    clk->Continue(100 * kPeriod);
    RT::JoinAll();
    finished_early = rd.at_15;
    finished_late = rd.at_60;
  }
  RT::Reset();
  // 只配上第 0 笔时还不算完成
  EXPECT_EQ(finished_early, 0u);
  // 第 1 笔也配上之后才 FINISH
  EXPECT_EQ(finished_late, 1u);
}

// 配置检查：一条链上出现两个 TASK_END 是错的。
TEST(BachTs, RejectsTwoEndTasks) {
  uint64_t state = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CfgReg cfg(clk, "cfg");
    cfg.WriteTask(0, MakeTask(SendUnit::kDte, true));
    cfg.WriteTask(1, MakeTask(SendUnit::kMu, true));
    cfg.SetInitFinish();
    clk->Continue(10 * kPeriod);
    RT::JoinAll();
    state = cfg.TsState();
  }
  RT::Reset();
  EXPECT_TRUE((state & kStateChainError) != 0);
}

// 配置检查：valid 的项中间有空洞也是错的。
TEST(BachTs, RejectsHoleInChain) {
  uint64_t state = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CfgReg cfg(clk, "cfg");
    cfg.WriteTask(0, MakeTask(SendUnit::kDte, false));
    cfg.WriteTask(2, MakeTask(SendUnit::kMu, true));  // 跳过了 1
    cfg.SetInitFinish();
    clk->Continue(10 * kPeriod);
    RT::JoinAll();
    state = cfg.TsState();
  }
  RT::Reset();
  EXPECT_TRUE((state & kStateChainError) != 0);
}
