// TS 的行为基线。
//
// 头两个用例看整条通路：单 stream 单 task 从 trigger 走到退休，三步的链逐步推进。
// 其余覆盖写口冲突、提前完成的 datain 被跳过、表满时反压 trigger、逐级 reduce
// 任务一项一项下发、配置检查这几条最容易实现错的。

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

TaskEntry MakeTask(SendUnit unit, bool end, bool dsa = true) {
  TaskEntry t;
  t.send_unit = unit;
  t.recv_unit = dsa ? RecvUnit::kDsa : RecvUnit::kRvOnly;
  t.end = end;
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
  // 同一个 user 在这一拍再推一笔 trigger，0 表示不推。
  uint64_t again_at = 0;
  bool sent_again = false;
  // Router 侧收不收退休请求。不收的话那个 stream 做完了也一直留在表里。
  bool accept_retire = true;
  uint64_t retire_seen = 0;
  uint64_t retire_user = 0;
  uint64_t credit_req_seen = 0;
  bool retire_hold = false;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    if (!sent && now >= at) {
      ts.Trigger().Drive(user, path, /*reissue=*/false);
      if (ts.Trigger().Ready()) sent = true;
    } else if (again_at != 0 && !sent_again && now >= again_at) {
      ts.Trigger().Drive(user, path, /*reissue=*/false);
      if (ts.Trigger().Ready()) sent_again = true;
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
    ts.Credit().RetireReq().DriveAccepted(accept_retire);
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

// 扮演三个 RV core 与它们的 DSA：收 task，隔几拍两路 ACK 一起报。
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
      // 注入一笔「这个 task 已经完成」：datain 任务的完成与主线走到哪无关，
      // 数据到了就报，所以它可以在主线还没走到那一项时先回来。
      if (u == 0 && inject_at != 0 && now == inject_at) {
        ts.RvDone(0).Drive(0, inject_task);
        ts.DsaDone(0).Drive(0, inject_task);
      } else if (fire[u] != 0 && now >= fire[u]) {
        ts.RvDone(u).Drive(stream[u], task[u]);
        ts.DsaDone(u).Drive(stream[u], task[u]);
        fire[u] = 0;
      } else {
        ts.RvDone(u).Idle();
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
    ts.InitFinish();

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
    ts.InitFinish();

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

// 找后继只看完成位图：异步 datain 已经提前完成时，主线走到那一项直接跳过。
TEST(BachTs, SkipsCompletedDatainTask) {
  std::vector<uint64_t> tasks;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Ts ts(clk, "ts", TsCfg{});
    // task0 普通、task1 是 datain、task2 普通收尾
    ts.Cfg().WriteTask(0, MakeTask(SendUnit::kDte, false));
    TaskEntry din = MakeTask(SendUnit::kDte, false);
    din.wait_wake = true;   // 这一项是 datain，PID 与 trigger 带的不同
    din.path_id = 9;
    ts.Cfg().WriteTask(1, din);
    ts.Cfg().WriteTask(2, MakeTask(SendUnit::kDte, true));
    ts.InitFinish();

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

// 同一个 user 先后两个 token：第一个的链走完，第 60 拍第二个进来。retire 为真时
// 第一个的 stream 已经退休，第二个建新表项把链再走一遍，返回 DTE 收到的 task 序列。
std::vector<uint64_t> RunSameUserTwice(bool retire) {
  std::vector<uint64_t> tasks;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Ts ts(clk, "ts", TsCfg{});
    TaskEntry din = MakeTask(SendUnit::kDte, false);
    din.wait_wake = true;
    din.path_id = 4;
    ts.Cfg().WriteTask(0, din);
    ts.Cfg().WriteTask(1, MakeTask(SendUnit::kDte, true));
    ts.InitFinish();

    RouterSide router(clk, ts, 2, 99, /*pid=*/4);
    router.again_at = 60;
    router.accept_retire = retire;
    UnitSide units(clk, ts, 2);
    clk->Continue(150 * kPeriod);
    RT::JoinAll();
    tasks = units.dte_tasks;
  }
  RT::Reset();
  return tasks;
}

// 系统保证有效的 user_id 唯一。一个 user 的 stream 退休了，同一个号再进来就是新
// 用户，链从头走一遍；stream 做完了还没退休时同一个号又进来，任务链上没有这条
// path 还没做的搬入任务，这一笔没有去处，断言失败。
TEST(BachTs, SameUserBeforeRetireIsAnError) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EQ(RunSameUserTwice(true), std::vector<uint64_t>({0, 1, 0, 1}));
  EXPECT_DEATH(RunSameUserTwice(false), "");
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
    ts.InitFinish();

    // 连着灌四个不同的用户
    class ManyUsers : public BachModule {
     public:
      ManyUsers(ClockPtr c, Ts& sched) : BachModule(c, "mu"), ts(sched) {}
      uint64_t sent = 0;

     protected:
      void Step() override {
        if (sent < 4) {
          ts.Trigger().Drive(100 + sent, 0, false);
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

// 逐级 reduce 任务一项一项下发：本级 Rmem 的 credit 每个用户一份，发出一项占
// 掉，Router 按用户号报回这一项做完才还，下一项才发。本地两路 ACK 只算搬完。
TEST(BachTs, ReduceIssuesOneTaskAtATime) {
  std::vector<uint64_t> tasks, at;
  uint64_t finished = 0, admitted = 0, finished_at_25 = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Ts ts(clk, "ts", TsCfg{});
    for (uint64_t k = 0; k < 3; ++k) {
      TaskEntry red = MakeTask(SendUnit::kDte, k == 2);
      red.task_type = TaskType::kReduce;
      red.credit_en = true;
      ts.Cfg().WriteTask(k, red);
    }
    ts.InitFinish();

    class ReduceDriver : public BachModule {
     public:
      ReduceDriver(ClockPtr c, Ts& sched) : BachModule(c, "rd"), ts(sched) {}
      std::vector<uint64_t> tasks, at;
      uint64_t finished = 0, admitted = 0, finished_at_25 = 0;

     protected:
      void Step() override {
        uint64_t now = CycleNow();
        // trigger 驱一次，按序号认，保持几拍再撤。
        if (now == 2) ts.Trigger().Drive(55, 0, false);
        if (now == 8) ts.Trigger().Idle();
        ts.Credit().RetireReq().DriveAccepted(true);
        ts.Credit().Req().DriveReady(true);
        ts.Credit().Grant().Idle();
        ts.DteCmd().DriveReady(true);
        ts.MuCmd().DriveReady(true);
        ts.VuCmd().DriveReady(true);

        TaskCmdPort& cmd = ts.DteCmd();
        if (cmd.Valid() && cmd.Seq() != last_seq) {
          last_seq = cmd.Seq();
          tasks.push_back(cmd.task_id.Get());
          at.push_back(now);
        }

        // 第 k 项：本地两路 ACK 在 20 + 30k 拍，Router 的 Reduce Done 在
        // 30 + 30k 拍。
        for (uint64_t u = 0; u < 3; ++u) {
          ts.RvDone(u).Idle();
          ts.DsaDone(u).Idle();
        }
        ts.Done().RdcDone().Idle();
        for (uint64_t k = 0; k < 3; ++k) {
          if (now == 20 + 30 * k) {
            ts.RvDone(0).Drive(0, k);
            ts.DsaDone(0).Drive(0, k);
          }
          if (now == 30 + 30 * k) ts.Done().RdcDone().Drive(55, k);
        }
        if (now == 25) finished_at_25 = ts.Done().Finished();
        finished = ts.Done().Finished();
        admitted = ts.Credit().RmemAdmitted();
      }

     private:
      Ts& ts;
      uint64_t last_seq = 0;
    };
    ReduceDriver rd(clk, ts);
    clk->Continue(140 * kPeriod);
    RT::JoinAll();
    tasks = rd.tasks;
    at = rd.at;
    finished = rd.finished;
    admitted = rd.admitted;
    finished_at_25 = rd.finished_at_25;
  }
  RT::Reset();
  ASSERT_EQ(tasks.size(), 3u) << "三项各下发一次";
  EXPECT_EQ(tasks, (std::vector<uint64_t>{0, 1, 2}));
  EXPECT_GT(at[1], 30u) << "第 1 项等第 0 项的 Reduce Done 之后才发";
  EXPECT_GT(at[2], 60u) << "第 2 项等第 1 项的 Reduce Done 之后才发";
  EXPECT_EQ(finished_at_25, 0u) << "本地两路 ACK 只算搬完";
  EXPECT_EQ(finished, 3u);
  EXPECT_EQ(admitted, 3u);
}

namespace {

constexpr uint64_t kNever = ~0ull;

// 自启动 core 的配置：Task 0 发 MU、只收 RV core 那一路，第 2 个任务发 DTE 并收尾。
void WriteSelfStartChain(Ts& ts, uint64_t stream_num) {
  ts.Cfg().WriteTask(0, MakeTask(SendUnit::kMu, /*end=*/false, /*dsa=*/false));
  ts.Cfg().WriteTask(1, MakeTask(SendUnit::kDte, /*end=*/true, /*dsa=*/false));
  ts.Cfg().WriteDatainTask(0x300, /*weights_mode=*/false);
  ts.Cfg().SetSelfStart(true);
  ts.Cfg().SetStreamNum(stream_num);
  ts.InitFinish();
}

// 扮演自启动 core 上的三个 RV core：命令来一笔收一笔，隔几拍报 RV core 那一路
// 完成。几笔可以同时在做，一拍报一笔。完成带回用户号，自启动的表项靠它补。
class SelfStartUnits : public BachModule {
 public:
  struct Got {
    uint64_t at = 0, unit = 0, stream = 0, task = 0;
  };

  SelfStartUnits(ClockPtr c, Ts& sched, std::array<uint64_t, 3> lat)
      : BachModule(c, "units"), ts(sched), latency(lat) {}

  std::vector<Got> issued, done;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    TaskCmdPort* cmd[3] = {&ts.DteCmd(), &ts.MuCmd(), &ts.VuCmd()};
    for (uint64_t u = 0; u < 3; ++u) {
      // 同一笔会连着几拍出现在端口上，按序号认它。
      if (cmd[u]->Valid() && cmd[u]->Seq() != last_seq[u]) {
        last_seq[u] = cmd[u]->Seq();
        Got g{now, u, cmd[u]->stream_id.Get(), cmd[u]->task_id.Get()};
        issued.push_back(g);
        pending[u].push_back({now + latency[u], g});
      }
      cmd[u]->DriveReady(true);
      if (head[u] < pending[u].size() && pending[u][head[u]].first <= now) {
        Got g = pending[u][head[u]++].second;
        ts.RvDone(u).Drive(g.stream, g.task, /*user=*/100 + g.stream);
        g.at = now;
        done.push_back(g);
      } else {
        ts.RvDone(u).Idle();
      }
      ts.DsaDone(u).Idle();
    }
  }

 private:
  Ts& ts;
  std::array<uint64_t, 3> latency;
  std::array<uint64_t, 3> last_seq{}, head{};
  std::array<std::vector<std::pair<uint64_t, Got>>, 3> pending;
};

}  // namespace

// 自启动 core：上电建满 stream_num 条链，Task 0 一次只下发一笔。taskchain0 的
// Task 0 做完才发 taskchain1 的，这时 taskchain0 已经在做第 2 个任务，两条链并行。
TEST(BachTs, SelfStartIssuesOneTask0AtATime) {
  std::vector<SelfStartUnits::Got> issued, done;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Ts ts(clk, "ts", TsCfg{});
    WriteSelfStartChain(ts, 4);
    RouterSide router(clk, ts, kNever, 0);
    // MU 做 Task 0 用 10 拍，DTE 做第 2 个任务用 40 拍。
    SelfStartUnits units(clk, ts, {40, 10, 10});
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    issued = units.issued;
    done = units.done;
  }
  RT::Reset();
  std::vector<SelfStartUnits::Got> t0_issue, t0_done;
  uint64_t chain0_second_done = 0;
  for (auto const& g : issued) {
    if (g.task == 0) t0_issue.push_back(g);
  }
  for (auto const& g : done) {
    if (g.task == 0) t0_done.push_back(g);
    if (g.stream == 0 && g.task == 1 && chain0_second_done == 0) {
      chain0_second_done = g.at;
    }
  }
  ASSERT_GE(t0_issue.size(), 4u);
  for (uint64_t k = 0; k < 4; ++k) {
    EXPECT_EQ(t0_issue[k].stream, k) << "按链的次序下发";
  }
  for (uint64_t k = 0; k + 1 < t0_issue.size(); ++k) {
    ASSERT_LT(k, t0_done.size());
    EXPECT_GT(t0_issue[k + 1].at, t0_done[k].at)
        << "第 " << k + 1 << " 笔 Task 0 要等上一笔做完";
  }
  EXPECT_LT(t0_issue[1].at, chain0_second_done)
      << "taskchain0 的第 2 个任务还在做时 taskchain1 的 Task 0 已经下发";
}

// 自启动 core：一条链的全部任务做完，照常向 Router 退休；Router 收下之后这条链
// 在原来的 stream 上重新激活成 Task 0，排到队尾，等前面那条链的 Task 0 做完才轮
// 到它。表一直是满的。
TEST(BachTs, SelfStartChainRestartsInPlace) {
  std::vector<uint64_t> t0_streams;
  uint64_t retire_seen = 0, in_flight = 0;
  bool both_valid = false, third_valid = true;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Ts ts(clk, "ts", TsCfg{});
    WriteSelfStartChain(ts, 2);
    RouterSide router(clk, ts, kNever, 0);
    SelfStartUnits units(clk, ts, {5, 5, 5});
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    for (auto const& g : units.issued) {
      if (g.task == 0) t0_streams.push_back(g.stream);
    }
    retire_seen = router.retire_seen;
    in_flight = ts.Table().InFlight();
    both_valid = ts.Table().Peek(0).valid && ts.Table().Peek(1).valid;
    third_valid = ts.Table().Peek(2).valid;
  }
  RT::Reset();
  ASSERT_GE(t0_streams.size(), 6u);
  for (uint64_t k = 0; k < 6; ++k) {
    EXPECT_EQ(t0_streams[k], k % 2) << "重新激活的链排到队尾，两条链轮流";
  }
  EXPECT_GE(retire_seen, 2u) << "退休照常先交给 Router";
  EXPECT_EQ(in_flight, 2u) << "表一直是满的";
  EXPECT_TRUE(both_valid);
  EXPECT_FALSE(third_valid) << "只在原来的 stream 上重新激活";
}

// 自启动 core 在权重加载模式下写 TS_INIT_FINISH 不建表：这一阶段不启动任务链。
// 切回业务模式再写一次，才建满 stream_num 个表项。
TEST(BachTs, SelfStartBuildsNothingInWeightsMode) {
  bool built_in_weights = true, built_in_business = false;
  uint64_t in_flight = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Ts ts(clk, "ts", TsCfg{});
    ts.Cfg().WriteTask(0, MakeTask(SendUnit::kMu, /*end=*/false, /*dsa=*/false));
    ts.Cfg().WriteTask(1, MakeTask(SendUnit::kDte, /*end=*/true, /*dsa=*/false));
    ts.Cfg().SetSelfStart(true);
    ts.Cfg().SetStreamNum(4);
    ts.Cfg().WriteDatainTask(0x300, /*weights_mode=*/true);
    ts.InitFinish();
    built_in_weights = ts.Table().Peek(0).valid;
    ts.Cfg().WriteDatainTask(0x300, /*weights_mode=*/false);
    ts.InitFinish();
    built_in_business = ts.Table().Peek(0).valid && ts.Table().Peek(3).valid;
    in_flight = ts.Table().InFlight();
  }
  RT::Reset();
  EXPECT_FALSE(built_in_weights) << "权重加载模式不启动任务链";
  EXPECT_TRUE(built_in_business);
  EXPECT_EQ(in_flight, 4u);
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
