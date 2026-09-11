// ReduceModule：三路输入的仲裁与锁定、原位累加、上下文保护、输出前的两道
// credit，以及退休后的延迟回收。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/common/numeric/formats.h"
#include "bach/ip/chip/core/router/reduce_module.h"
#include "bach/ip/wiring.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

// 一个 reduce 包：前 16 B 是软件辅助信息，加法跳过；后面是一个 FP32 元素。
std::vector<uint8_t> Word(float v) {
  std::vector<uint8_t> b(kReduceSwHeaderBytes + 4, 0);
  // 辅助信息随便填个能认出来的值，验它原样留在输出里。
  b[0] = 0xA5;
  uint32_t bits = numeric::BitsOf(v);
  for (int k = 0; k < 4; ++k) {
    b[kReduceSwHeaderBytes + k] = uint8_t((bits >> (8 * k)) & 0xFF);
  }
  return b;
}
float WordOf(std::vector<uint8_t> const& b) {
  if (b.size() < kReduceSwHeaderBytes + 4) return 0.0f;
  uint32_t v = 0;
  for (int k = 0; k < 4; ++k) {
    v |= uint32_t(b[kReduceSwHeaderBytes + k]) << (8 * k);
  }
  return numeric::FloatOf(v);
}
uint8_t SwHeadOf(std::vector<uint8_t> const& b) {
  return b.empty() ? 0 : b[0];
}

// 一份占好几个 flit 的分量：软件辅助信息之后跟 count 个同值的 FP32。
MessagePtr LongPart(uint64_t user, uint64_t path, uint64_t seq, float value,
                    uint64_t count) {
  auto m = std::make_shared<Message>();
  m->user_id = user;
  m->path_id = path;
  m->reduce_seq = seq;
  m->payload.assign(kReduceSwHeaderBytes + count * 4, 0);
  uint32_t bits = numeric::BitsOf(value);
  for (uint64_t j = 0; j < count; ++j) {
    for (int k = 0; k < 4; ++k) {
      m->payload[kReduceSwHeaderBytes + j * 4 + k] =
          uint8_t((bits >> (8 * k)) & 0xFF);
    }
  }
  m->size = m->payload.size();
  return m;
}

MessagePtr Part(uint64_t user, uint64_t path, uint64_t seq, float value) {
  auto m = std::make_shared<Message>();
  m->user_id = user;
  m->path_id = path;
  m->reduce_seq = seq;
  m->payload = Word(value);
  m->size = m->payload.size();
  return m;
}

// 中继累加：等两路分量，算完往右发。
RouteEntry Relay(uint64_t in_mask, uint64_t dtype = kReduceFp32,
                 uint64_t out_dtype = kReduceFp32) {
  RouteEntry e;
  e.op_type = OpType::kReduce;
  e.flow_dir = kFlowRight;
  e.path_core_bypass = true;
  e.reduce_in_mask = in_mask;
  e.operation = Operation::kReduce1;
  e.reduce_need = true;
  e.reduce_data_type = dtype;
  e.reduce_outdata_type = out_dtype;
  return e;
}

// 末端汇聚：算完只交本 core。
RouteEntry Sink(uint64_t in_mask) {
  RouteEntry e;
  e.op_type = OpType::kReduce;
  e.flow_dir = 0;
  e.path_core_bypass = false;
  e.reduce_in_mask = in_mask;
  e.operation = Operation::kReduce2;
  e.reduce_data_type = kReduceFp32;
  e.reduce_outdata_type = kReduceFp32;
  return e;
}

// 灌三路输入、扮演 Xbar 给授予、收 done，全在这一个协程里。
class ReduceHarness : public BachModule {
 public:
  struct Job {
    uint64_t at = 0;
    uint64_t lane = 0;
    MessagePtr msg;
    bool tail = true;
    bool head = true;
  };

  ReduceHarness(ClockPtr c, ReduceModule& target,
                std::vector<LinkEndPtr> wires)
      : BachModule(c, "harness"), rm(target), in(std::move(wires)) {}

  std::vector<Job> jobs;
  // 这一拍之前不给位置，用来把输出堵住。
  uint64_t grant_from = 0;
  // 到点退休、到点还 credit。改 ReduceModule 的表要在推它的这个协程里做。
  uint64_t retire_at = 0, return_at = 0;
  uint64_t retire_user = 0, return_dir = 0;

  uint64_t granted = 0;
  // 结果首 flit 与最后一个尾 flit 出现在哪一拍。
  uint64_t first_head_at = 0, last_tail_at = 0;
  // ReduceModule 累计挡住了几拍输入。计数是 Logic，要在协程里读。
  uint64_t stalled = 0;
  std::vector<uint64_t> out_users;
  std::vector<float> out_values;
  std::vector<uint8_t> out_heads;
  std::vector<MessagePtr> out_msgs;
  std::vector<uint64_t> done_users, done_seqs;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做：先看上一拍摆出来的请求与完成信号。
    XbarReqView v = ReadXbarReq(*rm.ReqPtr());
    if (v.valid && v.seq != last_seq) {
      last_seq = v.seq;
      ++granted;
      if (v.head && first_head_at == 0) first_head_at = now;
    }
    // 结果按 flit 发，一个包在尾 flit 那一拍记一条。
    if (v.valid && v.seq == last_seq && v.tail && v.seq != last_tail_seq) {
      last_tail_seq = v.seq;
      last_tail_at = now;
      out_users.push_back(v.user_id);
      if (v.msg) {
        out_values.push_back(WordOf(v.msg->payload));
        out_heads.push_back(SwHeadOf(v.msg->payload));
        out_msgs.push_back(v.msg);
      }
    }
    rm.ReqPtr()->DriveRoom(now >= grant_from);
    if (rm.Done().Valid()) {
      done_users.push_back(rm.Done().user_id.Get());
      done_seqs.push_back(rm.Done().reduce_seq.Get());
    }

    Feed(now);
    if (retire_at != 0 && now == retire_at) rm.RetireUser(retire_user);
    if (return_at != 0 && now == return_at) rm.ReturnCredit(retire_user, return_dir);
    rm.RunStep();
    stalled = rm.Stalled();
  }

 private:
  void Feed(uint64_t now) {
    std::vector<bool> drove(in.size(), false);
    for (auto const& j : jobs) {
      if (j.at != now) continue;
      in[j.lane]->flit.Drive(0, j.head, j.tail, j.msg->size, j.msg);
      drove[j.lane] = true;
    }
    for (uint64_t r = 0; r < in.size(); ++r) {
      if (!drove[r]) in[r]->flit.Idle();
      in[r]->release.Idle();
    }
  }

  ReduceModule& rm;
  std::vector<LinkEndPtr> in;
  uint64_t last_seq = 0, last_tail_seq = 0;
};

struct Bench {
  ClockPtr clk;
  std::unique_ptr<RouterTable> tab;
  std::unique_ptr<ReduceModule> rm;
  std::vector<LinkEndPtr> wires;

  explicit Bench(ClockPtr c) : clk(c) {
    tab = std::make_unique<RouterTable>(c, "rtab", 0, false);
    rm = std::make_unique<ReduceModule>(c, "reduce", *tab, 0, 0, false);
    for (uint64_t r = 0; r < 3; ++r) {
      wires.push_back(MakeWire(c));
      rm->AttachIn(r, wires.back());
    }
  }
};

}  // namespace

// 首份输入建上下文，后续方向读出当前值累加再写回：出来的是两份分量的和。
TEST(BachReduce, RmwAccumulatesInPlace) {
  std::vector<float> values;
  std::vector<uint64_t> users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));  // bit0 mid、bit1 left
    ReduceHarness h(clk, *b.rm, b.wires);
    h.jobs = {{2, 0, Part(77, 9, 5, 10.0f)}, {6, 1, Part(77, 9, 5, 32.0f)}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    values = h.out_values;
    users = h.out_users;
  }
  RT::Reset();
  ASSERT_EQ(values.size(), 1u) << "两份分量合成一个结果";
  EXPECT_FLOAT_EQ(values[0], 42.0f);
  EXPECT_EQ(users[0], 77u);
}

// 一份分量占好几个 flit 时只算一次：整包的字节挂在同一个 Message 上，每个
// flit 读到的 payload 都是整包，跟着 flit 数累加就会把一份算成好几份。
TEST(BachReduce, MultiFlitPartIsAccumulatedOnce) {
  std::vector<float> values;
  std::vector<MessagePtr> msgs;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    ReduceHarness h(clk, *b.rm, b.wires);
    // 528 B 一份，按 256 B 一个 flit 分三个。
    MessagePtr mid = LongPart(77, 9, 5, 10.0f, 128);
    MessagePtr left = LongPart(77, 9, 5, 32.0f, 128);
    h.jobs = {{2, 0, mid, false, true},
              {3, 0, mid, false, false},
              {4, 0, mid, true, false},
              {8, 1, left, false, true},
              {9, 1, left, false, false},
              {10, 1, left, true, false}};
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    values = h.out_values;
    msgs = h.out_msgs;
  }
  RT::Reset();
  ASSERT_EQ(values.size(), 1u) << "两份分量合成一个结果";
  EXPECT_FLOAT_EQ(values[0], 42.0f) << "三个 flit 的那一份只该算一遍";
  ASSERT_EQ(msgs.size(), 1u);
  ASSERT_EQ(msgs[0]->payload.size(), kReduceSwHeaderBytes + 128u * 4);
  // 末尾那个元素与开头一样，整包每一个都只加了一遍。
  uint32_t last = 0;
  for (int k = 0; k < 4; ++k) {
    last |= uint32_t(msgs[0]->payload[kReduceSwHeaderBytes + 127 * 4 + k])
            << (8 * k);
  }
  EXPECT_FLOAT_EQ(numeric::FloatOf(last), 42.0f) << "包尾那一个也只加一遍";
}

// 收齐判据取自表里的 reduce_in_mask：只到一路时不输出，等齐了才发。
TEST(BachReduce, WaitsForEveryDirectionInTheMask) {
  uint64_t out_before = 0, out_after = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b111));  // 三路都要
    ReduceHarness h(clk, *b.rm, b.wires);
    h.jobs = {{2, 0, Part(77, 9, 5, 1.0f)},
              {4, 1, Part(77, 9, 5, 2.0f)},
              {40, 2, Part(77, 9, 5, 4.0f)}};
    clk->Continue(30 * kPeriod);
    RT::JoinAll();
    out_before = h.out_values.size();
  }
  RT::Reset();
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b111));
    ReduceHarness h(clk, *b.rm, b.wires);
    h.jobs = {{2, 0, Part(77, 9, 5, 1.0f)},
              {4, 1, Part(77, 9, 5, 2.0f)},
              {40, 2, Part(77, 9, 5, 4.0f)}};
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    out_after = h.out_values.size();
  }
  RT::Reset();
  EXPECT_EQ(out_before, 0u) << "第三路没来之前不输出";
  EXPECT_EQ(out_after, 1u) << "三路齐了才发一次";
}

// 链上没有同步点：上游分量先到就先存进上下文，本地那一份晚到几十拍也不阻塞。
TEST(BachReduce, UpstreamPartWaitsInTheContext) {
  std::vector<float> values;
  uint64_t emit_at = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    ReduceHarness h(clk, *b.rm, b.wires);
    h.jobs = {{2, 0, Part(77, 9, 5, 100.0f)}, {50, 1, Part(77, 9, 5, 23.0f)}};
    clk->Continue(100 * kPeriod);
    RT::JoinAll();
    values = h.out_values;
    if (!h.out_users.empty()) emit_at = 1;
  }
  RT::Reset();
  ASSERT_EQ(values.size(), 1u);
  EXPECT_FLOAT_EQ(values[0], 123.0f);
  EXPECT_EQ(emit_at, 1u);
}

// 上下文保护：当前任务的结果还没全部发出时，同一个 user 的下一笔不得覆盖它。
// 挡住的那个包留在输入缓冲里，前一笔的结果发完之后再进来。
TEST(BachReduce, NextPacketOfTheSameUserDoesNotOverwrite) {
  std::vector<float> values;
  std::vector<uint64_t> seqs;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    ReduceHarness h(clk, *b.rm, b.wires);
    // 第一个包只到了一路，同一路上第二个包（reduce_seq 不同）就来了。
    h.jobs = {{2, 0, Part(77, 9, 5, 10.0f)},
              {4, 0, Part(77, 9, 6, 999.0f)},
              {20, 1, Part(77, 9, 5, 32.0f)},
              {30, 1, Part(77, 9, 6, 1.0f)}};
    // 第一个包往右发出去之后，右边还回 release，第二个包才发得出去。
    h.retire_user = 77;
    h.return_at = 40;
    h.return_dir = 2;
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    values = h.out_values;
    seqs = h.done_seqs;
  }
  RT::Reset();
  ASSERT_EQ(values.size(), 2u) << "两个包都要出来，挡住的那个不能丢";
  EXPECT_FLOAT_EQ(values[0], 42.0f) << "第二个包被挡在外面，没有把 10 覆盖掉";
  EXPECT_FLOAT_EQ(values[1], 1000.0f) << "第一个包出去之后第二个包再进来";
  ASSERT_EQ(seqs.size(), 2u);
  EXPECT_EQ(seqs[0], 5u);
  EXPECT_EQ(seqs[1], 6u);
}

// 分区在用户第一笔任务真正进来时才分配（F-029）：16 个都占着时，第 17 个用户
// 的分量留在输入缓冲里，等有用户 Retire 放出一个分区再进来。
uint64_t RunSeventeenUsers(uint64_t retire_at) {
  uint64_t out_num = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    ReduceHarness h(clk, *b.rm, b.wires);
    for (uint64_t u = 0; u <= kReduceCtxNum; ++u) {
      h.jobs.push_back({2 + u * 4, 0, Part(100 + u, 9, 1, 1)});
      h.jobs.push_back({3 + u * 4, 1, Part(100 + u, 9, 1, 1)});
    }
    h.retire_at = retire_at;
    h.retire_user = 100;
    clk->Continue(300 * kPeriod);
    RT::JoinAll();
    out_num = h.out_values.size();
  }
  RT::Reset();
  return out_num;
}

TEST(BachReduce, SeventeenthUserWaitsForAFreePartition) {
  EXPECT_EQ(RunSeventeenUsers(0), kReduceCtxNum)
      << "16 个分区都占着，第 17 个用户进不来";
  EXPECT_EQ(RunSeventeenUsers(150), kReduceCtxNum + 1)
      << "有用户 Retire 放出分区，第 17 个接着做";
}

// 分区不预先占：用户第一笔任务进来之前一个都不占，进来了占一个。
TEST(BachReduce, PartitionIsAllocatedWhenTheFirstTaskEnters) {
  uint64_t before = 1, after = 0;
  bool holds = false;
  for (uint64_t run : {10u, 60u}) {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    ReduceHarness h(clk, *b.rm, b.wires);
    h.jobs = {{20, 0, Part(77, 9, 5, 1.0f)}, {24, 1, Part(77, 9, 5, 2.0f)}};
    clk->Continue(run * kPeriod);
    RT::JoinAll();
    if (run == 10) {
      before = b.rm->ContextUsed();
    } else {
      after = b.rm->ContextUsed();
      holds = b.rm->HoldsUser(77);
    }
    RT::Reset();
  }
  EXPECT_EQ(before, 0u) << "第一笔任务进来之前不占分区";
  EXPECT_EQ(after, 1u);
  EXPECT_TRUE(holds) << "任务做完分区还留着，等 Retire 才放";
}

// 结果按 flit 出（Router MAS「整包输入输出与结果流水」）：某个结果 flit 要的
// 操作数都累加完了就发，不等整包。左路后面几个 flit 晚到，结果的首 flit 先出去，
// 包锁定到尾 flit，后面的等数据到了再接着发。
TEST(BachReduce, ResultFlitLeavesBeforeTheWholePacketArrives) {
  uint64_t first_head = 0, tail = 0;
  std::vector<float> values;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    ReduceHarness h(clk, *b.rm, b.wires);
    // 1040 B 一份：16 B 软件辅助信息加 256 个 FP32，按 256 B 一个 flit 分五个。
    MessagePtr mid = LongPart(77, 9, 5, 10.0f, 256);
    MessagePtr left = LongPart(77, 9, 5, 32.0f, 256);
    h.jobs = {{2, 0, mid, false, true},   {3, 0, mid, false, false},
              {4, 0, mid, false, false},  {5, 0, mid, false, false},
              {6, 0, mid, true, false},   {3, 1, left, false, true},
              {40, 1, left, false, false}, {41, 1, left, false, false},
              {42, 1, left, false, false}, {43, 1, left, true, false}};
    clk->Continue(100 * kPeriod);
    RT::JoinAll();
    first_head = h.first_head_at;
    tail = h.last_tail_at;
    values = h.out_values;
  }
  RT::Reset();
  EXPECT_GT(first_head, 0u);
  EXPECT_LT(first_head, 40u) << "两路的首 flit 到了，结果的首 flit 就发";
  EXPECT_GT(tail, 43u) << "尾 flit 要等左路的数据都到了";
  ASSERT_EQ(values.size(), 1u);
  EXPECT_FLOAT_EQ(values[0], 42.0f);
}

// 下游 Reduce credit 按 UserID 与目标方向记，粒度是一笔任务：发出去之后那个
// 方向这个用户就忙着，release 回来才空。
TEST(BachReduce, DownCreditIsPerUserAndDirection) {
  bool right = false, mid = true, after_return = true;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));  // 往右发
    ReduceHarness h(clk, *b.rm, b.wires);
    h.jobs = {{2, 0, Part(77, 9, 5, 1.0f)}, {4, 1, Part(77, 9, 5, 2.0f)}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    right = b.rm->DownBusy(77, 2);
    mid = b.rm->DownBusy(77, 0);
    b.rm->ReturnCredit(77, 2);
    after_return = b.rm->DownBusy(77, 2);
  }
  RT::Reset();
  EXPECT_TRUE(right) << "往右发过一笔，右边这个用户就忙着";
  EXPECT_FALSE(mid) << "没往那个方向发就不占";
  EXPECT_FALSE(after_return) << "release 回来就空了";
}

// 同一个用户的下一笔任务要等下游把上一笔的 release 还回来才发。
std::vector<float> RunTwoTasks(uint64_t return_at) {
  std::vector<float> values;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    ReduceHarness h(clk, *b.rm, b.wires);
    h.jobs = {{2, 0, Part(77, 9, 5, 1.0f)},
              {4, 1, Part(77, 9, 5, 2.0f)},
              {10, 0, Part(77, 9, 6, 10.0f)},
              {12, 1, Part(77, 9, 6, 20.0f)}};
    h.retire_user = 77;
    h.return_at = return_at;
    h.return_dir = 2;
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    values = h.out_values;
  }
  RT::Reset();
  return values;
}

TEST(BachReduce, NextTaskWaitsForDownstreamRelease) {
  std::vector<float> held = RunTwoTasks(0);
  ASSERT_EQ(held.size(), 1u) << "release 没回来，第二笔发不出去";
  EXPECT_FLOAT_EQ(held[0], 3.0f);
  std::vector<float> freed = RunTwoTasks(40);
  ASSERT_EQ(freed.size(), 2u) << "release 回来之后第二笔接着发";
  EXPECT_FLOAT_EQ(freed[1], 30.0f);
}

// User Retire 只放本地分区（F-037）：往右发过的那一笔 release 还没回来，下游
// 映射留着；回来了才清。
TEST(BachReduce, RetireFreesThePartitionButKeepsTheDownstreamMap) {
  bool held = true, right_busy = false;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    ReduceHarness h(clk, *b.rm, b.wires);
    h.jobs = {{2, 0, Part(77, 9, 5, 1.0f)}, {4, 1, Part(77, 9, 5, 2.0f)}};
    h.retire_at = 30;
    h.retire_user = 77;
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    held = b.rm->HoldsUser(77);
    right_busy = b.rm->DownBusy(77, 2);
  }
  RT::Reset();
  EXPECT_FALSE(held) << "Retire 之后本地分区当场放";
  EXPECT_TRUE(right_busy) << "往右那一笔的 release 没回来，下游映射还记着";

  bool right_after = true;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    ReduceHarness h(clk, *b.rm, b.wires);
    h.jobs = {{2, 0, Part(77, 9, 5, 1.0f)}, {4, 1, Part(77, 9, 5, 2.0f)}};
    h.retire_at = 30;
    h.retire_user = 77;
    h.return_at = 32;
    h.return_dir = 2;
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    right_after = b.rm->DownBusy(77, 2);
  }
  RT::Reset();
  EXPECT_FALSE(right_after) << "release 回来就清了";
}

// 只收一路的中继：灌一份 count 个 FP32 的分量，数出来几个包。
uint64_t RunOnePart(uint64_t count) {
  uint64_t out_num = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b001));
    ReduceHarness h(clk, *b.rm, b.wires);
    h.jobs = {{2, 0, LongPart(77, 9, 5, 1.0f, count)}};
    // 结果按 flit 发，装满时 129 个 flit，留够拍数。
    clk->Continue(400 * kPeriod);
    RT::JoinAll();
    out_num = h.out_values.size();
  }
  RT::Reset();
  return out_num;
}

// 上下文容量：一个包的 FP32 累加结果正好装满一个用户的上下文照常累加，多一个
// 元素就报错，软件要先按容量拆包。
TEST(BachReduce, PacketLargerThanTheContextIsAnError) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  uint64_t full = kReduceCtxBytes / 4;
  EXPECT_EQ(RunOnePart(full), 1u) << "正好装满照常出结果";
  EXPECT_DEATH(RunOnePart(full + 1), "");
}

// 三路输入进入后锁定到尾 flit：一个包收到一半时别的方向插不进来，那一路的
// flit 留在它的输入缓冲里，尾 flit 过了再收。
TEST(BachReduce, LockedToTailAcrossInputs) {
  std::vector<float> values;
  uint64_t stalled = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    ReduceHarness h(clk, *b.rm, b.wires);
    // lane0 的包分两拍：head 之后隔十拍才来 tail。中间 lane1 想插一笔。
    h.jobs = {{2, 0, Part(77, 9, 5, 10.0f), false},
              {4, 1, Part(77, 9, 5, 1000.0f), true},
              {12, 0, Part(77, 9, 5, 5.0f), true}};
    clk->Continue(100 * kPeriod);
    RT::JoinAll();
    values = h.out_values;
    stalled = h.stalled;
  }
  RT::Reset();
  EXPECT_GT(stalled, 0u) << "锁定期间 lane1 那一笔要被挡住";
  ASSERT_EQ(values.size(), 1u);
  // 10 + 5 是 lane0 那个包的两拍，1000 是 lane1 那一份，锁定解除后才收进来。
  EXPECT_FLOAT_EQ(values[0], 1015.0f) << "挡住的那一份不能丢";
}

// 两个分量任意顺序到齐都算数：先左后中与先中后左结果一样。
TEST(BachReduce, PartsArriveInEitherOrder) {
  float first_order = 0, other_order = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    ReduceHarness h(clk, *b.rm, b.wires);
    h.jobs = {{2, 0, Part(77, 9, 5, 7.0f)}, {6, 1, Part(77, 9, 5, 35.0f)}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    if (!h.out_values.empty()) first_order = h.out_values.front();
  }
  RT::Reset();
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    ReduceHarness h(clk, *b.rm, b.wires);
    h.jobs = {{2, 1, Part(77, 9, 5, 35.0f)}, {6, 0, Part(77, 9, 5, 7.0f)}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    if (!h.out_values.empty()) other_order = h.out_values.front();
  }
  RT::Reset();
  EXPECT_FLOAT_EQ(first_order, 42.0f);
  EXPECT_FLOAT_EQ(other_order, 42.0f) << "两个分量任意顺序到齐，结果一样";
}

// BF16 输入扩成 FP32 累加，输出按 reduce_outdata_type 转回 BF16。
TEST(BachReduce, Bf16InFp32AccumBf16Out) {
  float value = 0.0f;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011, kReduceBf16, kReduceBf16));
    ReduceHarness h(clk, *b.rm, b.wires);

    // 两份 BF16 分量：0.5 与 0.25，都在 BF16 能精确表示的档上。
    auto make = [](uint64_t user, float v) {
      auto m = std::make_shared<Message>();
      m->user_id = user;
      m->path_id = 9;
      m->reduce_seq = 5;
      m->payload.assign(kReduceSwHeaderBytes + 2, 0);
      uint16_t bits = numeric::ToBf16(v);
      m->payload[kReduceSwHeaderBytes] = uint8_t(bits & 0xFF);
      m->payload[kReduceSwHeaderBytes + 1] = uint8_t((bits >> 8) & 0xFF);
      m->size = m->payload.size();
      return m;
    };
    h.jobs = {{2, 0, make(77, 0.5f)}, {6, 1, make(77, 0.25f)}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    // 输出也是 BF16，取回来看值。
    if (!h.out_msgs.empty()) {
      std::vector<uint8_t> const& p = h.out_msgs.front()->payload;
      if (p.size() >= kReduceSwHeaderBytes + 2) {
        uint16_t bits = uint16_t(uint16_t(p[kReduceSwHeaderBytes]) |
                                 (uint16_t(p[kReduceSwHeaderBytes + 1]) << 8));
        value = numeric::FromBf16(bits);
      }
    }
  }
  RT::Reset();
  EXPECT_FLOAT_EQ(value, 0.75f) << "扩成 FP32 加完再转回 BF16";
}

// 包最前面 16 B 是软件辅助信息，加法跳过它，输出里原样留着。
TEST(BachReduce, SoftwareHeaderIsSkippedAndKept) {
  uint8_t head = 0;
  float value = 0.0f;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    ReduceHarness h(clk, *b.rm, b.wires);
    h.jobs = {{2, 0, Part(77, 9, 5, 3.0f)}, {6, 1, Part(77, 9, 5, 4.0f)}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    if (!h.out_heads.empty()) head = h.out_heads.front();
    if (!h.out_values.empty()) value = h.out_values.front();
  }
  RT::Reset();
  EXPECT_FLOAT_EQ(value, 7.0f) << "只有数据段参与加法";
  EXPECT_EQ(head, 0xA5) << "那 16 B 原样留在输出里，没被加进去";
}
