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
  uint64_t last_seq = 0;
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
    b.rm->AllocContext(77);
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
    b.rm->AllocContext(77);
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
    b.rm->AllocContext(77);
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
    b.rm->AllocContext(77);
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
    b.rm->AllocContext(77);
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

// 上下文保护：当前包还没输出时，同一个 user 的下一个包不得覆盖它。
TEST(BachReduce, NextPacketOfTheSameUserDoesNotOverwrite) {
  std::vector<float> values;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    b.rm->AllocContext(77);
    ReduceHarness h(clk, *b.rm, b.wires);
    // 第一个包只到了一路，第二个包（reduce_seq 不同）就来了。
    h.jobs = {{2, 0, Part(77, 9, 5, 10.0f)},
              {4, 1, Part(77, 9, 6, 999.0f)},
              {20, 1, Part(77, 9, 5, 32.0f)}};
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    values = h.out_values;
  }
  RT::Reset();
  ASSERT_FALSE(values.empty());
  EXPECT_FLOAT_EQ(values[0], 42.0f) << "第二个包被挡在外面，没有把 10 覆盖掉";
}

// 输出队列满时对输入反压，不绕过 Reduce 降级成直接转发。
TEST(BachReduce, FullOutputQueueBackpressuresInput) {
  uint64_t out_num = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    for (uint64_t u = 0; u < 12; ++u) b.rm->AllocContext(100 + u);
    ReduceHarness h(clk, *b.rm, b.wires);
    // 十二个 user 各两路分量，Xbar 一直不给授予，输出队列只有 8 个位置。
    for (uint64_t u = 0; u < 12; ++u) {
      h.jobs.push_back({2 + u * 4, 0, Part(100 + u, 9, 1, 1)});
      h.jobs.push_back({3 + u * 4, 1, Part(100 + u, 9, 1, 1)});
    }
    h.grant_from = 100000;  // 永远不授予
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    out_num = h.out_values.size();
  }
  RT::Reset();
  EXPECT_EQ(out_num, 0u) << "发不出去就压着，不绕过 Reduce 直接转发";
}

// 下游 Reduce credit 按 UserID 与目标方向记账：每发一个扣一个，release 回来
// 才恢复。
TEST(BachReduce, DownCreditIsPerUserAndDirection) {
  uint64_t right = 0, mid = 0, after_return = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));  // 往右发
    b.rm->AllocContext(77);
    ReduceHarness h(clk, *b.rm, b.wires);
    h.jobs = {{2, 0, Part(77, 9, 5, 1.0f)}, {4, 1, Part(77, 9, 5, 2.0f)}};
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    right = b.rm->DownCredit(77, 2);
    mid = b.rm->DownCredit(77, 0);
    b.rm->ReturnCredit(77, 2);
    after_return = b.rm->DownCredit(77, 2);
  }
  RT::Reset();
  EXPECT_EQ(right, kReduceCreditInit - 1) << "往右发一个扣一个";
  EXPECT_EQ(mid, kReduceCreditInit) << "没往那个方向发就不扣";
  EXPECT_EQ(after_return, kReduceCreditInit) << "release 回来就补上";
}

// 上下文与进 core 的 stream 表一一对应：16 项，用光了就是两边分配逻辑不一致。
TEST(BachReduce, ContextCountMatchesTheStreamTable) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  Bench b(clk);
  for (uint64_t u = 0; u < kReduceCtxNum; ++u) b.rm->AllocContext(u);
  EXPECT_EQ(b.rm->ContextUsed(), kReduceCtxNum);
  // 同一个 user 再来一次不占新的。
  b.rm->AllocContext(0);
  EXPECT_EQ(b.rm->ContextUsed(), kReduceCtxNum);
  EXPECT_DEATH(b.rm->AllocContext(999), "");
  RT::Reset();
}

// 退休延迟回收：credit 没全回来之前不删上下文，回来了才删。
TEST(BachReduce, RetiredContextWaitsForCreditToComeBack) {
  bool held_before = false;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    b.rm->AllocContext(77);
    ReduceHarness h(clk, *b.rm, b.wires);
    h.jobs = {{2, 0, Part(77, 9, 5, 1.0f)}, {4, 1, Part(77, 9, 5, 2.0f)}};
    h.retire_at = 30;
    h.retire_user = 77;
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    held_before = b.rm->HoldsUser(77);
  }
  RT::Reset();
  EXPECT_TRUE(held_before) << "往右发过一个 flit，credit 还差一个，先记着不删";

  bool held_after = true;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    b.rm->AllocContext(77);
    ReduceHarness h(clk, *b.rm, b.wires);
    h.jobs = {{2, 0, Part(77, 9, 5, 1.0f)}, {4, 1, Part(77, 9, 5, 2.0f)}};
    h.retire_at = 30;
    h.retire_user = 77;
    h.return_at = 32;
    h.return_dir = 2;
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    held_after = b.rm->HoldsUser(77);
  }
  RT::Reset();
  EXPECT_FALSE(held_after) << "credit 全回来了才删";
}

// 三路输入进入后锁定到尾 flit：一个包收到一半时别的方向插不进来。
TEST(BachReduce, LockedToTailAcrossInputs) {
  std::vector<float> values;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    b.rm->AllocContext(77);
    ReduceHarness h(clk, *b.rm, b.wires);
    // lane0 的包分两拍：head 之后隔十拍才来 tail。中间 lane1 想插一笔。
    h.jobs = {{2, 0, Part(77, 9, 5, 10.0f), false},
              {4, 1, Part(77, 9, 5, 1000.0f), true},
              {12, 0, Part(77, 9, 5, 5.0f), true},
              {30, 1, Part(77, 9, 5, 32.0f), true}};
    clk->Continue(100 * kPeriod);
    RT::JoinAll();
    values = h.out_values;
  }
  RT::Reset();
  ASSERT_EQ(values.size(), 1u);
  // 10 + 5 是 lane0 那个包的两拍，32 是 lane1 后来补的那一份。中间被挡住的
  // 那笔 1000 没有混进来。
  EXPECT_FLOAT_EQ(values[0], 47.0f) << "锁定期间别的方向插不进来";
}

// 两个分量任意顺序到齐都算数：先左后中与先中后左结果一样。
TEST(BachReduce, PartsArriveInEitherOrder) {
  float first_order = 0, other_order = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Bench b(clk);
    b.tab->Preload(9, Relay(0b011));
    b.rm->AllocContext(77);
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
    b.rm->AllocContext(77);
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
    b.rm->AllocContext(77);
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
    b.rm->AllocContext(77);
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
