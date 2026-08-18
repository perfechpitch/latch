// common 三件套的行为基线：封包与身份、参数换算、三种排队语义。
//
// 仲裁器是纯逻辑，直接在主线程上跑；封包要过 Fifo，所以按建模规则用两个
// ClkModule 在同一个 clock 上真并发跑，sub_thread 大于 1。

#include <gtest/gtest.h>

#include <memory>

#include "base/clock.h"
#include "base/fifo.h"
#include "base/module.h"
#include "base/runtime.h"
#include "bach/common/arbiter.h"
#include "bach/common/packet.h"
#include "bach/common/params.h"

using namespace latch;
using namespace latch::bach;

namespace {

// 时钟周期取 1 ns，与模型一致：一拍就是一 ns。
constexpr Time kPeriod = 1;

// ---------------------------------------------------------------- 驱动

class PktProducer : public ClkModule {
 public:
  PktProducer(ClockPtr c, std::shared_ptr<Fifo<RoutedPkt>> f)
      : ClkModule(c), fifo(std::move(f)) {
    RegisterName("bach_pkt_producer");
  }

  void Cycle() override {
    DelayCycle(1);
    ++cyc;
    if (cyc != 1 || fifo->IsFull()) return;

    auto inst = std::make_shared<CommInst>();
    inst->uid = 7;
    inst->tid = 3;
    inst->opcode = Opcode::kReduce;
    inst->tag = 11;
    inst->beat_id = 1;
    inst->total_fragments = 2;
    inst->vol = 512;
    inst->xfer_id = 99;
    inst->hit_map = {2, 5};

    // 与 rv32 各级一样，每次发都现建一个包，不复用成员：没被本拍写过的字段
    // 会回落到上一拍的值，复用会把旧值悄悄带出去。
    RoutedPkt p(clk);
    p.dst = EncodeCoord(Coord{2, -3});
    p.src = EncodeCoord(Coord{0, 0});
    p.size = uint64_t{128};
    p.payload_size = uint64_t{512};
    p.byte_offset = uint64_t{0};
    p.fragment_id = uint64_t{0};
    p.total_frag = uint64_t{4};
    p.is_tail = uint64_t{0};
    p.arrive_cycle = RT::Now() + kPeriod;
    p.hop_count = uint64_t{1};
    p.in_port = static_cast<uint64_t>(Port::kWest);
    p.payload = inst;
    fifo->Push(p);

    push_cycle = RT::Now();
    pushed = 1;
  }

  Time push_cycle = 0;
  uint64_t pushed = 0;

 private:
  std::shared_ptr<Fifo<RoutedPkt>> fifo;
  uint64_t cyc = 0;
};

class PktConsumer : public ClkModule {
 public:
  PktConsumer(ClockPtr c, std::shared_ptr<Fifo<RoutedPkt>> f)
      : ClkModule(c), fifo(std::move(f)) {
    RegisterName("bach_pkt_consumer");
  }

  void Cycle() override {
    DelayCycle(1);
    if (fifo->IsEmpty()) return;

    RoutedPkt& slot = fifo->Front();
    seen_cycle = RT::Now();
    seen_dst = DecodeCoord(slot.dst);
    seen_size = slot.size;
    seen_arrive = slot.arrive_cycle;
    seen_in_port = static_cast<Port>(uint64_t(slot.in_port));

    CommInstPtr inst = slot.payload.Get();
    seen_uid = inst->uid;
    seen_opcode = inst->opcode;
    seen_xfer = inst->xfer_id;
    seen_hits = inst->hit_map.size();
    seen_hits_group5 = inst->HitsGroup(5) ? 1 : 0;
    payload_addr = reinterpret_cast<uint64_t>(inst.get());

    fifo->Pop();
    ++seen;
  }

  uint64_t seen = 0;
  Time seen_cycle = 0;
  Coord seen_dst{0, 0};
  uint64_t seen_size = 0;
  Time seen_arrive = 0;
  Port seen_in_port = Port::kUnknown;
  uint64_t seen_uid = 0;
  Opcode seen_opcode = Opcode::kDontCare;
  uint64_t seen_xfer = 0;
  uint64_t seen_hits = 0;
  uint64_t seen_hits_group5 = 0;
  uint64_t payload_addr = 0;

 private:
  std::shared_ptr<Fifo<RoutedPkt>> fifo;
};

}  // namespace

// ---------------------------------------------------------------- 封包

TEST(BachPacket, CoordRoundTripKeepsNegative) {
  // 外部节点与 PCIe 交换节点用不落在核阵列内的坐标，可以为负。
  const Coord c{-4, 7};
  EXPECT_EQ(DecodeCoord(EncodeCoord(c)), c);
  EXPECT_EQ(DecodeCoord(EncodeCoord(Coord{0, 0})), (Coord{0, 0}));
  EXPECT_EQ(DecodeCoord(EncodeCoord(Coord{-1, -1})), (Coord{-1, -1}));
}

TEST(BachPacket, BeatKeyDistinguishesOpcodeAndTag) {
  CommInst a;
  a.uid = 1; a.tid = 2; a.tag = 3; a.opcode = Opcode::kReduce;
  CommInst b = a;
  CommInst c = a;
  c.opcode = Opcode::kReduction;   // 大小写两种 reduce 是不同任务语义
  CommInst d = a;
  d.tag = 4;

  EXPECT_TRUE(MakeBeatKey(a) == MakeBeatKey(b));
  EXPECT_FALSE(MakeBeatKey(a) == MakeBeatKey(c));
  EXPECT_FALSE(MakeBeatKey(a) == MakeBeatKey(d));
}

TEST(BachPacket, FragKeySeparatesTwoTransfersOfSameBeat) {
  // 两个 uid、tid、tag、opcode 全同的包同时在一跳上，只有传输实例号能把它们的
  // 片分开。这一维在 Bach 里是 Python 对象身份，C++ 侧由发送侧显式分配。
  CommInst inst;
  inst.uid = 1; inst.tid = 2; inst.tag = 3; inst.opcode = Opcode::kMove;

  FragKey x{MakeBeatKey(inst), 0, 100};
  FragKey y{MakeBeatKey(inst), 0, 101};
  EXPECT_FALSE(x == y);
  EXPECT_NE(FragKeyHash()(x), FragKeyHash()(y));
}

TEST(BachPacket, PortsAreOpposedPairwise) {
  EXPECT_EQ(OppositePort(Port::kNorth), Port::kSouth);
  EXPECT_EQ(OppositePort(Port::kPcieEast), Port::kPcieWest);
  EXPECT_EQ(OppositePort(Port::kLocal), Port::kLocal);
  EXPECT_TRUE(IsPciePort(Port::kPcieUp));
  EXPECT_FALSE(IsPciePort(Port::kEast));
}

TEST(BachPacket, BarrierOpcodesAreTheFourFanInKinds) {
  EXPECT_TRUE(IsBarrierOpcode(Opcode::kReduce));
  EXPECT_TRUE(IsBarrierOpcode(Opcode::kReduction));
  EXPECT_TRUE(IsBarrierOpcode(Opcode::kConcat));
  EXPECT_TRUE(IsBarrierOpcode(Opcode::kRes));
  EXPECT_FALSE(IsBarrierOpcode(Opcode::kMove));
  EXPECT_FALSE(IsBarrierOpcode(Opcode::kUserInit));
}

// ---------------------------------------------------------------- 参数

TEST(BachParams, CycleCountRoundsUpAndFloorsAtOne) {
  EXPECT_EQ(CalcCycles(0, 512), 1u);    // size 不大于 0 仍算一拍
  EXPECT_EQ(CalcCycles(-8, 512), 1u);
  EXPECT_EQ(CalcCycles(1, 512), 1u);
  EXPECT_EQ(CalcCycles(512, 512), 1u);
  EXPECT_EQ(CalcCycles(513, 512), 2u);
  EXPECT_EQ(CalcCycles(1024, 128), 8u);
}

TEST(BachParams, DefaultsMatchBachAndValidate) {
  Params p;
  EXPECT_EQ(p.ts_logic_time, 16u);
  EXPECT_EQ(p.dte_setup_time, 85u);
  EXPECT_EQ(p.dte_reduce_time, 32u);
  EXPECT_EQ(p.dte_bandwidth, 512u);
  EXPECT_EQ(p.stream_count, 8u);
  EXPECT_EQ(p.cross_node_delay, 5000u);
  EXPECT_EQ(p.setup_ahead_depth, 1u);
  p.Validate();
}

// ---------------------------------------------------------------- 独占资源

TEST(BachArbiter, ExclusiveGrantsInArrivalOrder) {
  ExclusiveArbiter arb;
  Ticket a = arb.Enqueue(10);
  Ticket b = arb.Enqueue(10);
  Ticket c = arb.Enqueue(11);

  // 空闲时当刻授予，与 request() 落在空资源上立即成功一致
  EXPECT_TRUE(arb.Granted(a));
  EXPECT_FALSE(arb.Granted(b));
  EXPECT_EQ(arb.QueueLen(), 2u);

  arb.Release(a, 12);
  EXPECT_TRUE(arb.Granted(b));   // 一次释放只放行队首一个
  EXPECT_FALSE(arb.Granted(c));
  EXPECT_EQ(arb.GrantCycle(b), 12u);
  EXPECT_EQ(arb.WaitCycles(b), 2u);

  arb.Release(b, 20);
  EXPECT_TRUE(arb.Granted(c));
  EXPECT_EQ(arb.WaitCycles(c), 9u);
  arb.Release(c, 21);
  EXPECT_FALSE(arb.Busy());
}

TEST(BachArbiter, ExclusiveCancelRemovesWaiter) {
  ExclusiveArbiter arb;
  Ticket a = arb.Enqueue(0);
  Ticket b = arb.Enqueue(0);
  Ticket c = arb.Enqueue(0);

  arb.Cancel(b);
  EXPECT_EQ(arb.QueueLen(), 1u);
  arb.Release(a, 1);
  EXPECT_TRUE(arb.Granted(c));
}

// ---------------------------------------------------------------- 优先级资源

TEST(BachArbiter, PriorityLowerValueGoesFirst) {
  PriorityArbiter arb(1);
  Ticket hold = arb.Enqueue(0, /*priority=*/9);
  EXPECT_TRUE(arb.Granted(hold));

  Ticket late_high = arb.Enqueue(1, /*priority=*/1);
  Ticket early_low = arb.Enqueue(2, /*priority=*/5);

  arb.Release(hold, 3);
  // 优先级 1 后到，仍然先于优先级 5
  EXPECT_TRUE(arb.Granted(late_high));
  EXPECT_FALSE(arb.Granted(early_low));

  arb.Release(late_high, 4);
  EXPECT_TRUE(arb.Granted(early_low));
}

TEST(BachArbiter, PrioritySameLevelKeepsArrivalOrder) {
  PriorityArbiter arb(1);
  Ticket hold = arb.Enqueue(0, 0);
  Ticket first = arb.Enqueue(1, 3);
  Ticket second = arb.Enqueue(1, 3);

  arb.Release(hold, 2);
  EXPECT_TRUE(arb.Granted(first));
  EXPECT_FALSE(arb.Granted(second));
}

TEST(BachArbiter, PriorityCapacityIsTheAdmissionWindow) {
  // 准入令牌的容量是 setup_ahead_depth 加 1
  Params p;
  PriorityArbiter arb(p.setup_ahead_depth + 1);
  Ticket a = arb.Enqueue(0, 0);
  Ticket b = arb.Enqueue(0, 1);
  Ticket c = arb.Enqueue(0, 2);

  EXPECT_TRUE(arb.Granted(a));
  EXPECT_TRUE(arb.Granted(b));
  EXPECT_FALSE(arb.Granted(c));
  EXPECT_EQ(arb.Held(), 2u);

  arb.Release(a, 5);
  EXPECT_TRUE(arb.Granted(c));
}

// ---------------------------------------------------------------- 计数信号量

TEST(BachArbiter, CreditDeductsAtGrantAndRefillsOnPut) {
  CreditCounter credits(2, 2);
  Ticket a = credits.Get(0);
  Ticket b = credits.Get(0);
  Ticket c = credits.Get(0);

  EXPECT_TRUE(credits.Granted(a));
  EXPECT_TRUE(credits.Granted(b));
  EXPECT_FALSE(credits.Granted(c));
  EXPECT_EQ(credits.Level(), 0u);

  credits.Drop(a);
  credits.Put(7);                  // 下游 RETIRE 回来
  EXPECT_TRUE(credits.Granted(c));
  EXPECT_EQ(credits.GrantCycle(c), 7u);
  EXPECT_EQ(credits.Level(), 0u);
}

TEST(BachArbiter, CreditQueueBlocksAtHead) {
  CreditCounter credits(1, 4);
  Ticket big = credits.Get(0, 3);
  Ticket small = credits.Get(0, 1);

  // 余额 1 已经够队尾那个要 1 的了，但队头要 3 拿不到，后面就不许越过
  EXPECT_FALSE(credits.Granted(big));
  EXPECT_FALSE(credits.Granted(small));
  EXPECT_EQ(credits.Level(), 1u);

  credits.Put(1, 2);
  EXPECT_TRUE(credits.Granted(big));    // 余额到 3，队头一次拿光
  EXPECT_FALSE(credits.Granted(small));
  EXPECT_EQ(credits.Level(), 0u);

  credits.Put(2, 1);
  EXPECT_TRUE(credits.Granted(small));
  EXPECT_EQ(credits.GrantCycle(small), 2u);
}

TEST(BachArbiter, CreditGroupHoldsWhatItAlreadyTook) {
  // 一条 CU 任务向两个下游验资：先到手的先扣住，再继续等其余
  CreditCounter ds0(1, 1);
  CreditCounter ds1(0, 1);

  CreditGetGroup group;
  group.Add(&ds0, ds0.Get(0));
  group.Add(&ds1, ds1.Get(0));

  EXPECT_FALSE(group.AllGranted());
  EXPECT_EQ(ds0.Level(), 0u);   // 已经扣住了，堵在另一个下游上

  ds1.Put(9);
  EXPECT_TRUE(group.AllGranted());
  EXPECT_EQ(group.GrantCycle(), 9u);   // 全部到手的那一拍
  group.Drop();
  EXPECT_TRUE(group.Empty());
}

// ---------------------------------------------------------------- 跨模块

TEST(BachPacket, RoutedPktCrossesFifoInOneCycle) {
  RT::Reset(4, 4);
  RT::GetRecorder().Reset();

  Time push_cycle = 0;
  uint64_t pushed = 0;
  uint64_t seen = 0;
  Time seen_cycle = 0;
  Coord seen_dst{0, 0};
  uint64_t seen_size = 0;
  Time seen_arrive = 0;
  Port seen_in_port = Port::kUnknown;
  uint64_t seen_uid = 0;
  Opcode seen_opcode = Opcode::kDontCare;
  uint64_t seen_xfer = 0;
  uint64_t seen_hits = 0;
  uint64_t seen_hits_group5 = 0;

  {
    ClockPtr clk = MakeClock(0, kPeriod);
    auto link = std::make_shared<Fifo<RoutedPkt>>(8, clk);
    PktProducer prod(clk, link);
    PktConsumer cons(clk, link);

    clk->Continue(16 * kPeriod);
    RT::JoinAll();

    // 协程内写、主线程读的普通成员，JoinAll 之后取是安全的
    push_cycle = prod.push_cycle;
    pushed = prod.pushed;
    seen = cons.seen;
    seen_cycle = cons.seen_cycle;
    seen_dst = cons.seen_dst;
    seen_size = cons.seen_size;
    seen_arrive = cons.seen_arrive;
    seen_in_port = cons.seen_in_port;
    seen_uid = cons.seen_uid;
    seen_opcode = cons.seen_opcode;
    seen_xfer = cons.seen_xfer;
    seen_hits = cons.seen_hits;
    seen_hits_group5 = cons.seen_hits_group5;
  }

  ASSERT_EQ(pushed, 1u);
  ASSERT_EQ(seen, 1u);

  // Push 到读到正好隔一拍，这就是链路上那一拍的来历
  EXPECT_EQ(seen_cycle - push_cycle, kPeriod);

  EXPECT_EQ(seen_dst, (Coord{2, -3}));
  EXPECT_EQ(seen_size, 128u);
  EXPECT_EQ(seen_arrive, push_cycle + kPeriod);
  EXPECT_EQ(seen_in_port, Port::kWest);

  // 载荷挂在 LogicPtr 下，跨模块只搬 shared_ptr，变长字段原样可见
  EXPECT_EQ(seen_uid, 7u);
  EXPECT_EQ(seen_opcode, Opcode::kReduce);
  EXPECT_EQ(seen_xfer, 99u);
  EXPECT_EQ(seen_hits, 2u);
  EXPECT_EQ(seen_hits_group5, 1u);
}
