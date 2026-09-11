// 完整 Router 装配的行为基线。
//
// 八类模块接成一个 Router，从对外的三个 R2R 方向端口灌数据，看它转发、进核、
// 归约、溢流转存这几条路走不走得通。
//
// 这里验的是接线本身：单模块的行为在 router_basic 与 router_scenarios 里已经
// 各自验过，这一份查的是它们接起来之后端到端还成不成立。

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/common/numeric/formats.h"
#include "bach/ip/chip/core/router/router.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

// 一个 Router 是 11 个模块，加上测试的驱动就超过默认的 8 个协程槽位。槽位不够
// 时多余的协程永远等不到空位，进程卡住而不报错，所以每个用例先把池子开够。
void EnsureSlots() { RT::Reset(8, 8); }
// Router 对外的三个 R2R 方向下标，与 flow_dir 的位序同一套。
constexpr uint64_t kMid = 0, kLeft = 1, kRight = 2;

MessagePtr MakeMsg(uint64_t path, uint64_t user, uint64_t bytes = 256) {
  auto m = std::make_shared<Message>();
  m->path_id = path;
  m->user_id = user;
  m->size = bytes;
  return m;
}

class Pusher : public BachModule {
 public:
  struct Job {
    uint64_t at = 0;
    MessagePtr msg;
    uint64_t vc = 0;
  };

  Pusher(ClockPtr c, LinkEndPtr port, std::vector<Job> list)
      : BachModule(c, "pusher"), sink(std::move(port)), jobs(std::move(list)) {}

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    for (auto const& j : jobs) {
      if (j.at != now) continue;
      sink->flit.Drive(j.vc, true, true, j.msg->size, j.msg);
      return;
    }
    sink->flit.Idle();
  }

 private:
  LinkEndPtr sink;
  std::vector<Job> jobs;
};

// 收下游出口，并按需回 vc_release。
class Downstream : public BachModule {
 public:
  Downstream(ClockPtr c, LinkEndPtr from, LinkEndPtr back, bool release_on)
      : BachModule(c, "down"), src(std::move(from)), rel(std::move(back)),
        on(release_on) {}

  uint64_t got = 0;
  uint64_t first_at = 0;
  std::vector<uint64_t> users, ats;
  std::vector<uint8_t> last_payload;

 protected:
  void Step() override {
    FlitView f = ReadFlit(src->flit);
    if (f.valid) {
      if (got == 0) first_at = CycleNow();
      ++got;
      ats.push_back(CycleNow());
      if (f.msg) {
        users.push_back(f.msg->user_id);
        last_payload = f.msg->payload;
      }
    }
    rel->flit.Idle();
    if (on && f.valid) {
      rel->release.Drive(true, f.vc, false, 0, false, 0);
    } else {
      rel->release.Idle();
    }
  }

 private:
  LinkEndPtr src, rel;
  bool on;
};

// 扮演 DTE 与 TS：一直拉 ready 收进核数据，并把 trigger 收下。
class CoreSide : public BachModule {
 public:
  CoreSide(ClockPtr c, Router& r) : BachModule(c, "core_side"), rt(r) {}

  uint64_t got_flits = 0, triggers = 0;
  std::vector<uint64_t> trigger_users;
  bool reduce_done = false;
  uint64_t done_user = 0, done_seq = 0;

 protected:
  void Step() override {
    // 进核数据：一直收得下。
    CoreDataView d = ReadCoreData(rt.ToDte());
    if (d.valid) ++got_flits;
    rt.ToDte().DriveReady(true);
    rt.FromDte().Idle();

    // trigger：一直拉 ready。
    if (rt.Trigger().Valid()) {
      ++triggers;
      trigger_users.push_back(rt.Trigger().user_id.Get());
    }
    rt.Trigger().DriveReady(true);

    if (rt.ReduceDone().Valid()) {
      reduce_done = true;
      done_user = rt.ReduceDone().user_id.Get();
      done_seq = rt.ReduceDone().reduce_seq.Get();
    }
    rt.CreditReq().Idle();
    rt.RetireReq().Idle();
  }

 private:
  Router& rt;
};

class AsmProbe : public BachModule {
 public:
  AsmProbe(ClockPtr c, Router& r) : BachModule(c, "probe"), rt(r) {}

  uint64_t stream_right = 0, overflow = 0, stored = 0, cs_used = 0;
  uint64_t cr_right = 0, shared_right = 0;

 protected:
  void Step() override {
    stream_right = rt.GetXbar().StreamUsed(kRight);
    overflow = rt.GetXbar().Overflow();
    stored = rt.GetReissue().Stored();
    cs_used = rt.GetCoreStation().StreamUsed();
    cr_right = rt.GetXbar().VcCredit(kRight, 0);
    shared_right = rt.GetXbar().SharedCredit(kRight);
  }

 private:
  Router& rt;
};

RouteEntry Forward(uint64_t flow, bool enters_core, bool stall_way = false) {
  RouteEntry e;
  e.flow_dir = flow;
  e.path_core_bypass = !enters_core;
  e.stream_table_enable = false;
  e.operation = Operation::kForward;
  e.stall_way = stall_way;
  return e;
}

// 一个 reduce 包的 payload：前 16 B 软件辅助信息，后面一个 FP32 元素。
std::vector<uint8_t> ReducePayload(float v) {
  std::vector<uint8_t> b(kReduceSwHeaderBytes + 4, 0);
  uint32_t bits = numeric::BitsOf(v);
  for (int k = 0; k < 4; ++k) {
    b[kReduceSwHeaderBytes + k] = uint8_t((bits >> (8 * k)) & 0xFF);
  }
  return b;
}
float ReduceValueOf(std::vector<uint8_t> const& b) {
  if (b.size() < kReduceSwHeaderBytes + 4) return 0.0f;
  uint32_t v = 0;
  for (int k = 0; k < 4; ++k) {
    v |= uint32_t(b[kReduceSwHeaderBytes + k]) << (8 * k);
  }
  return numeric::FloatOf(v);
}

// 末端汇聚：等 mid 与 left 两个方向的分量，算完交本 core。
RouteEntry ReduceSink(uint64_t in_mask) {
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

// 中继累加：等两个方向的分量，算完往右发。
RouteEntry ReduceRelay(uint64_t in_mask) {
  RouteEntry e;
  e.op_type = OpType::kReduce;
  e.flow_dir = kFlowRight;
  e.path_core_bypass = true;
  e.reduce_in_mask = in_mask;
  e.operation = Operation::kReduce1;
  e.reduce_need = true;
  e.reduce_data_type = kReduceFp32;
  e.reduce_outdata_type = kReduceFp32;
  return e;
}

}  // namespace

// 从 left 进、往 right 出，整条链走通。
TEST(BachRouterAsm, ForwardsAcrossAssembly) {
  uint64_t got = 0;
  std::vector<uint64_t> users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Router rt(clk, "router", RouterCfg{});
    rt.Preload(3, Forward(kFlowRight, /*enters_core=*/false));

    Pusher push(clk, rt.InWire(kLeft), {{1, MakeMsg(3, 42)}});
    Downstream down(clk, rt.OutWire(kRight), rt.BackWire(kRight), true);
    CoreSide core(clk, rt);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    got = down.got;
    users = down.users;
  }
  RT::Reset();
  EXPECT_EQ(got, 1u);
  ASSERT_EQ(users.size(), 1u);
  EXPECT_EQ(users[0], 42u);
}

// A9：进核由包头 path_core_mask 的第 idx 位定，该位为 1 时进核，为 0 时
// 只转发。位到 core 的对应不是固定编码，每个 core 在自己的表项里指定看哪一位。
TEST(BachRouterAsm, CoreMaskBitDecidesEntry) {
  uint64_t flits = 0, forwarded = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Router rt(clk, "router", RouterCfg{});
    RouteEntry e = Forward(kFlowRight, /*enters_core=*/false);
    e.path_core_mask_enable = true;
    e.path_core_mask_idx = 5;
    rt.Preload(3, e);

    // 第一个包第 5 位是 1，进核也往右转；第二个包该位是 0，只往右转。
    auto m0 = MakeMsg(3, 42);
    m0->path_core_mask = 1ull << 5;
    auto m1 = MakeMsg(3, 43);
    m1->path_core_mask = 1ull << 4;
    Pusher p0(clk, rt.InWire(kLeft), {{2, m0}, {12, m1}});
    Downstream down(clk, rt.OutWire(kRight), rt.BackWire(kRight), true);
    CoreSide core(clk, rt);
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    flits = core.got_flits;
    forwarded = down.got;
  }
  RT::Reset();
  EXPECT_EQ(forwarded, 2u) << "两个包都往右转";
  EXPECT_EQ(flits, 1u) << "只有那一位是 1 的进核";
}


// 一个方向每拍出一个 flit：station 那一侧按「Xbar 收得下就发」交，不等授予。
// 等授予的话一笔要占两拍，一个方向的带宽就只剩一半。
TEST(BachRouterAsm, OneFlitPerCycleThroughOneInput) {
  std::vector<uint64_t> at;
  uint64_t got = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Router rt(clk, "router", RouterCfg{});
    rt.Preload(3, Forward(kFlowRight, /*enters_core=*/false));

    // 连着十拍，每拍从同一个方向推一个 flit 进来。
    std::vector<Pusher::Job> jobs;
    for (uint64_t i = 0; i < 10; ++i) {
      jobs.push_back({1 + i, MakeMsg(3, 100 + i)});
    }
    Pusher push(clk, rt.InWire(kLeft), jobs);
    Downstream down(clk, rt.OutWire(kRight), rt.BackWire(kRight), true);
    CoreSide core(clk, rt);
    clk->Continue(120 * kPeriod);
    RT::JoinAll();
    at = down.ats;
    got = down.got;
  }
  RT::Reset();
  EXPECT_EQ(got, 10u) << "十个 flit 都要出去";
  ASSERT_EQ(at.size(), 10u);
  for (uint64_t i = 1; i < at.size(); ++i) {
    EXPECT_EQ(at[i], at[i - 1] + 1)
        << "第 " << i << " 个 flit 与上一个之间空了拍";
  }
}

// 进核：包落到 CoreStation，占一个坑，并通知 TS。
TEST(BachRouterAsm, EntersCoreAndNotifiesTs) {
  uint64_t flits = 0, triggers = 0, used = 0;
  std::vector<uint64_t> trig_users;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Router rt(clk, "router", RouterCfg{});
    rt.Preload(3, Forward(/*flow=*/0, /*enters_core=*/true));

    Pusher push(clk, rt.InWire(kLeft), {{2, MakeMsg(3, 42)}});
    CoreSide core(clk, rt);
    AsmProbe probe(clk, rt);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    flits = core.got_flits;
    triggers = core.triggers;
    trig_users = core.trigger_users;
    used = probe.cs_used;
  }
  RT::Reset();
  EXPECT_EQ(flits, 1u);
  EXPECT_EQ(triggers, 1u);
  ASSERT_EQ(trig_users.size(), 1u);
  EXPECT_EQ(trig_users[0], 42u);
  EXPECT_EQ(used, 1u);  // 占了一个进核的坑
}

// A17 的完整闭环：两个方向的分量进 ReduceModule，收齐后回注 Xbar，
// 结果交本 core，同时向 TS 发 reduce_done。
TEST(BachRouterAsm, ReduceClosesTheLoop) {
  uint64_t flits = 0;
  bool done = false;
  uint64_t done_user = 0, done_seq = 0;
  std::vector<uint8_t> payload;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Router rt(clk, "router", RouterCfg{});
    // bit0 mid、bit1 left
    rt.Preload(9, ReduceSink(0b011));

    auto m0 = MakeMsg(9, 77);
    m0->reduce_seq = 5;
    m0->payload = ReducePayload(10.0f);
    auto m1 = MakeMsg(9, 77);
    m1->reduce_seq = 5;
    m1->payload = ReducePayload(32.0f);

    Pusher p0(clk, rt.InWire(kMid), {{2, m0}});
    Pusher p1(clk, rt.InWire(kLeft), {{6, m1}});

    // 收进核那一路的 payload
    class CorePayload : public BachModule {
     public:
      CorePayload(ClockPtr c, Router& r) : BachModule(c, "cp"), rt(r) {}
      uint64_t flits = 0;
      std::vector<uint8_t> payload;
      bool done = false;
      uint64_t user = 0, seq = 0;

     protected:
      void Step() override {
        CoreDataView d = ReadCoreData(rt.ToDte());
        if (d.valid) {
          ++flits;
          if (d.msg) payload = d.msg->payload;
        }
        rt.ToDte().DriveReady(true);
        rt.FromDte().Idle();
        rt.Trigger().DriveReady(true);
        if (rt.ReduceDone().Valid()) {
          done = true;
          user = rt.ReduceDone().user_id.Get();
          seq = rt.ReduceDone().reduce_seq.Get();
        }
        rt.CreditReq().Idle();
        rt.RetireReq().Idle();
      }

     private:
      Router& rt;
    };
    CorePayload cp(clk, rt);
    clk->Continue(100 * kPeriod);
    RT::JoinAll();
    flits = cp.flits;
    payload = cp.payload;
    done = cp.done;
    done_user = cp.user;
    done_seq = cp.seq;
  }
  RT::Reset();
  EXPECT_EQ(flits, 1u);
  EXPECT_TRUE(done);
  EXPECT_EQ(done_user, 77u);
  EXPECT_EQ(done_seq, 5u);
  EXPECT_FLOAT_EQ(ReduceValueOf(payload), 42.0f)
      << "10 + 32，累加发生在 ReduceModule 里";
}

namespace {

// 扮演 DTE 与 TS，另外在某一拍把本 core 自己那一份分量发出核。valid 保持到
// 对面拉起 ready 那一拍为止。
class SelfPartSide : public BachModule {
 public:
  SelfPartSide(ClockPtr c, Router& r, uint64_t at, MessagePtr m)
      : BachModule(c, "self_part"), rt(r), fire_at(at), msg(std::move(m)) {}

 protected:
  void Step() override {
    CoreDataView d = ReadCoreData(rt.ToDte());
    (void)d;
    rt.ToDte().DriveReady(true);
    if (msg && CycleNow() >= fire_at) {
      if (drove && rt.FromDte().Ready()) {
        rt.FromDte().Idle();
        msg.reset();
      } else {
        rt.FromDte().Drive(msg->size, true, true, kReduceVc, msg);
        drove = true;
      }
    } else {
      rt.FromDte().Idle();
    }
    rt.Trigger().DriveReady(true);
    rt.CreditReq().Idle();
    rt.RetireReq().Idle();
  }

 private:
  Router& rt;
  uint64_t fire_at;
  MessagePtr msg;
  bool drove = false;
};

}  // namespace

// 本 core 自己那一份分量走哪一路输入由 flow_dir 的 reduce1 指定：上游从 mid
// 来占了 bit0，自己那一份挪到 bit1，两路收齐后往右发。
TEST(BachRouterAsm, SelfPartTakesTheLaneFlowDirNames) {
  uint64_t got = 0;
  std::vector<uint8_t> payload;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Router rt(clk, "router", RouterCfg{});
    RouteEntry e = ReduceRelay(0b011);
    e.flow_dir |= kFlowReduce1;
    rt.Preload(9, e);

    auto up = MakeMsg(9, 77);
    up->payload = ReducePayload(7.0f);
    up->size = up->payload.size();
    auto own = MakeMsg(9, 77);
    own->payload = ReducePayload(5.0f);
    own->size = own->payload.size();

    Pusher p0(clk, rt.InWire(kMid), {{2, up, kReduceVc}});
    SelfPartSide side(clk, rt, 6, own);
    Downstream down(clk, rt.OutWire(kRight), rt.BackWire(kRight), true);
    clk->Continue(100 * kPeriod);
    RT::JoinAll();
    got = down.got;
    payload = down.last_payload;
  }
  RT::Reset();
  EXPECT_EQ(got, 1u);
  EXPECT_FLOAT_EQ(ReduceValueOf(payload), 12.0f)
      << "上游那一份与本 core 那一份都进了 ReduceModule";
}

// 中继累加：收齐后按 flow_dir 往右发，占的是右方向这个用户的下游 Reduce 资源。
TEST(BachRouterAsm, ReduceRelayForwardsDownstream) {
  uint64_t got = 0;
  std::vector<uint8_t> payload;
  bool right_busy = false;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    Router rt(clk, "router", RouterCfg{});
    rt.Preload(9, ReduceRelay(0b011));

    auto m0 = MakeMsg(9, 77);
    m0->payload = ReducePayload(7.0f);
    auto m1 = MakeMsg(9, 77);
    m1->payload = ReducePayload(5.0f);
    Pusher p0(clk, rt.InWire(kMid), {{2, m0}});
    Pusher p1(clk, rt.InWire(kLeft), {{6, m1}});
    Downstream down(clk, rt.OutWire(kRight), rt.BackWire(kRight), true);
    CoreSide core(clk, rt);
    clk->Continue(100 * kPeriod);
    RT::JoinAll();
    got = down.got;
    payload = down.last_payload;
    right_busy = rt.GetReduce().DownBusy(77, 2);
  }
  RT::Reset();
  EXPECT_EQ(got, 1u);
  EXPECT_FLOAT_EQ(ReduceValueOf(payload), 12.0f);
  EXPECT_TRUE(right_busy) << "右方向这个用户忙着，等下游还 release";
}

// 溢流：下游 credit 耗光且 stall_way 选转存，包落进 CoreMemReissue，
// VC 槽同时腾出来，同一份数据不会既在暂存区又占着 VC。
TEST(BachRouterAsm, OverflowGoesToReissue) {
  uint64_t overflow = 0, stored = 0, got = 0, occ = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RouterCfg cfg;
    cfg.reissue_pkts_per_vc = 8;
    Router rt(clk, "router", cfg);
    rt.Preload(3, Forward(kFlowRight, /*enters_core=*/false,
                          /*stall_way=*/true));

    // 灌 24 笔：private 20 加 shared 20 共 40 个 credit 用不完，所以先把
    // 下游堵死，Downstream 不回 release，20 笔之后 private 见底。
    std::vector<Pusher::Job> jobs;
    for (uint64_t i = 0; i < 24; ++i) {
      jobs.push_back({1 + i, MakeMsg(3, 500 + i)});
    }
    Pusher push(clk, rt.InWire(kLeft), jobs);
    Downstream down(clk, rt.OutWire(kRight), rt.BackWire(kRight), false);
    CoreSide core(clk, rt);
    AsmProbe probe(clk, rt);
    clk->Continue(300 * kPeriod);
    RT::JoinAll();
    overflow = probe.overflow;
    stored = probe.stored;
    got = down.got;
    occ = rt.Station(kLeft).Occupancy();
  }
  RT::Reset();
  // private 20 加 shared 20 共 40，24 笔全发得出去，不该有溢流
  EXPECT_EQ(got, 24u);
  EXPECT_EQ(overflow, 0u);
  EXPECT_EQ(stored, 0u);
  EXPECT_EQ(occ, 0u);
}

// credit 真耗光时才转存：把 40 个 credit 全用掉，第 41 笔起走 stall_way。
TEST(BachRouterAsm, StallWayStoresWhenCreditIsGone) {
  uint64_t overflow = 0, stored = 0, got = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RouterCfg cfg;
    // 要转存 10 笔，暂存区就得配得下 10 笔。配少了 CoreMemReissue 会直接停。
    // 那是设计要的行为：不覆盖已暂存的包，也不退回「留在当前 VC 等」，免得同一个
    // stall_way 配置在两种容量下走出两种行为。
    cfg.reissue_pkts_per_vc = 16;
    Router rt(clk, "router", cfg);
    rt.Preload(3, Forward(kFlowRight, false, /*stall_way=*/true));

    // 一个方向的 credit 是 private 20 加 shared 20 共 40。前 40 笔把它用光，
    // 之后的没资源可拿，stall_way 选转存就落进 Core Mem。发出去的 flit 会把
    // VC Buffer 的槽腾出来，所以灌 50 笔不会把 buffer 撑爆。
    std::vector<Pusher::Job> jobs;
    for (uint64_t i = 0; i < 50; ++i) {
      jobs.push_back({1 + i, MakeMsg(3, 500 + i)});
    }
    Pusher push(clk, rt.InWire(kLeft), jobs);
    Downstream down(clk, rt.OutWire(kRight), rt.BackWire(kRight), false);
    CoreSide core(clk, rt);
    AsmProbe probe(clk, rt);
    clk->Continue(300 * kPeriod);
    RT::JoinAll();
    overflow = probe.overflow;
    stored = probe.stored;
    got = down.got;
  }
  RT::Reset();
  // 40 个 credit 用完，剩下的 10 笔都转存
  EXPECT_EQ(got, 40u);
  EXPECT_EQ(overflow, 10u);
  EXPECT_EQ(stored, overflow);
}

// 只透传的 core：整个 Router 打开 pass_through 后不投递本 core、不记账。
TEST(BachRouterAsm, PassThroughRouterKeepsNoState) {
  uint64_t got = 0, cs_used = 0, stream = 0, flits = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    RouterCfg cfg;
    cfg.pass_through = true;
    Router rt(clk, "router", cfg);
    // 表里写的是进核加查坑，透传核要把这两样都抹掉
    RouteEntry e = Forward(kFlowRight, /*enters_core=*/true);
    e.stream_table_enable = true;
    rt.Preload(3, e);

    Pusher push(clk, rt.InWire(kLeft), {{2, MakeMsg(3, 42)}});
    Downstream down(clk, rt.OutWire(kRight), rt.BackWire(kRight), true);
    CoreSide core(clk, rt);
    AsmProbe probe(clk, rt);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    got = down.got;
    cs_used = probe.cs_used;
    stream = probe.stream_right;
    flits = core.got_flits;
  }
  RT::Reset();
  EXPECT_EQ(got, 1u);      // 照转
  EXPECT_EQ(flits, 0u);    // 不投递本 core
  EXPECT_EQ(cs_used, 0u);  // 不占进核的坑
  EXPECT_EQ(stream, 0u);   // 不记出方向的账
}

// tick 关掉之后由外层统一驱动，结果要与各模块自己挂时钟逐拍相同。
//
// 这是建模计划里对规模那条风险的应对：48 chip × 10 core 每个 core 五十来个模块
// 是两万多个协程，槽位不够就卡死。因为跨模块信号全部打拍，谁来推这一拍不影响
// 各模块读到什么，所以两种驱动方式必须给出同一个答案。这个用例就是守这条。
TEST(BachRouterAsm, DrivenByParentMatchesSelfTicked) {
  auto run = [](bool tick) {
    struct Result {
      uint64_t got = 0;
      uint64_t arrive = 0;
      uint64_t credit = 0;
    } r;
    {
      EnsureSlots();
      ClockPtr clk = MakeClock(0, kPeriod);
      RouterCfg cfg;
      cfg.tick = tick;
      Router rt(clk, "router", cfg);
      rt.Preload(3, Forward(kFlowRight, /*enters_core=*/false));

      // tick 关掉时，Router 只占一个协程：这个驱动每拍调一次 RunStep()。
      class ParentDriver : public BachModule {
       public:
        ParentDriver(ClockPtr c, Router& r) : BachModule(c, "parent"), rt(r) {}

       protected:
        void Step() override { rt.RunStep(); }

       private:
        Router& rt;
      };
      std::unique_ptr<ParentDriver> driver;
      if (!tick) driver = std::make_unique<ParentDriver>(clk, rt);

      Pusher push(clk, rt.InWire(kLeft), {{2, MakeMsg(3, 42)}});
      Downstream down(clk, rt.OutWire(kRight), rt.BackWire(kRight), true);
      CoreSide core(clk, rt);

      clk->Continue(60 * kPeriod);
      RT::JoinAll();
      // 这三样都是协程写、主线程读的普通成员。JoinAll 之后所有协程都退出了，
      // 主线程读它们是安全的。
      //
      // 不能在协程里读别的模块的裸成员：两个模块同处一拍并发跑，读到的是「本拍
      // 已跑过的给本拍末的值 + 还没跑的给上一拍末的值」的混合，同一份输入两次跑
      // 会差一拍。第一版就是让探针在自己的 Step 里读 down.got，40 轮抖出 3 次。
      r.got = down.got;
      r.arrive = down.first_at;
      r.credit = rt.GetXbar().VcCredit(kRight, 0);
    }
    RT::Reset();
    return r;
  };

  auto self = run(true);
  auto parent = run(false);
  EXPECT_EQ(self.got, 1u);
  EXPECT_EQ(parent.got, self.got);
  EXPECT_EQ(parent.arrive, self.arrive);
  EXPECT_EQ(parent.credit, self.credit);
}
