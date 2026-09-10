// Core 与 chip 两层装配的行为基线。
//
// 这两层自己不打拍，判据因此是结构性的：接线对不对、不派角色的 core 少构造了
// 哪些模块、boot 序列走没走完、C2C 上的包拆了又拼回来还是不是原来那一个。

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/chip.h"
#include "bach/ip/link/link.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;
// Router 对外三个 R2R 方向的下标，与 flow_dir 的位序同一套。
constexpr uint64_t kMidDir = 0, kLeftDir = 1, kRightDir = 2;

void EnsureSlots() { RT::Reset(8, 8); }

// 一个 chip 上百个模块，全部由这一个协程驱动。
class ChipDriver : public BachModule {
 public:
  ChipDriver(ClockPtr c, Chip& target) : BachModule(c, "driver"), chip(target) {}

 protected:
  void Step() override { chip.RunOutside(); }

 private:
  Chip& chip;
};

// ── 形状与角色 ──

TEST(BachChip, ShapeAndRoles) {
  // 中间列 8 个 core 全是 compute；第一列 core0 是 B core、core5 不派角色；
  // 最后一列 core9 是 R core、core4 不派角色。
  EXPECT_EQ(CoresOf(ChipShape::kMiddle), 8u);
  EXPECT_EQ(CoresOf(ChipShape::kFirst), 10u);
  EXPECT_EQ(CoresOf(ChipShape::kLast), 10u);

  for (uint64_t i = 0; i < 8; ++i) {
    EXPECT_EQ(RoleOf(ChipShape::kMiddle, i), CoreRole::kCompute) << "i=" << i;
  }
  EXPECT_EQ(RoleOf(ChipShape::kFirst, 0), CoreRole::kBroadcast);
  EXPECT_EQ(RoleOf(ChipShape::kFirst, 5), CoreRole::kSpare);
  EXPECT_EQ(RoleOf(ChipShape::kLast, 9), CoreRole::kReduce);
  EXPECT_EQ(RoleOf(ChipShape::kLast, 4), CoreRole::kSpare);
  // 不派角色的那个只在两种 2×5 形状里有，中间列一个都没有。
  for (uint64_t i = 0; i < 8; ++i) {
    EXPECT_NE(RoleOf(ChipShape::kMiddle, i), CoreRole::kSpare) << "i=" << i;
  }
}

TEST(BachChip, SpareCoreIsRouterOnly) {
  // 不派角色的 core 只构造 Router 的八个模块，不构造 TS、RV core、DSA 与三块
  // 存储。它的 Ready 恒为真 —— 没有 RV core 要等。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  ChipCfg cfg;
  cfg.shape = ChipShape::kLast;
  Chip chip(clk, "chip", cfg);

  EXPECT_TRUE(chip.GetCore(4).Context().router_only);
  EXPECT_TRUE(chip.GetCore(4).Ready());
  EXPECT_FALSE(chip.GetCore(0).Context().router_only);
  // Router 一个都不能少：经过它的 path 全靠这个。
  EXPECT_EQ(chip.GetCore(4).GetRouter().Quiescent(), true);
}

// ── core 的对外只有 Router ──

TEST(BachCore, PortsAreRouterOnly) {
  // core 对外只有三个 R2R 方向与 ctrl_noc，没有第三条路。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreContext ctx;
  ctx.core_id = 3;
  Core core(clk, "core", ctx);

  for (uint64_t d = 0; d < 3; ++d) {
    EXPECT_NE(core.InWire(d), nullptr) << "d=" << d;
    EXPECT_NE(core.OutWire(d), nullptr) << "d=" << d;
    EXPECT_NE(core.BackWire(d), nullptr) << "d=" << d;
    // VC credit 与 stream release 的回程各走各的实例。
    EXPECT_NE(core.UpBackWire(d), core.UpReleaseWire(d)) << "d=" << d;
  }
  EXPECT_EQ(core.Context().core_id, 3u);
}

TEST(BachCore, WiringIsByPortsNotOrder) {
  // 模块之间只通过端口相连：两侧只看到端口束的字段，不持有对方的类型。这里
  // 验证接线的结果 —— 同一根线两端拿到的是同一个对象。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreContext ctx;
  Core core(clk, "core", ctx);

  // TS 下发给 DTE RV core 的那一根。
  EXPECT_EQ(&core.GetTs().DteCmd(), &core.Rv(0).Cmd());
  EXPECT_EQ(&core.GetTs().MuCmd(), &core.Rv(1).Cmd());
  EXPECT_EQ(&core.GetTs().VuCmd(), &core.Rv(2).Cmd());
  // 三个 RV core 各配自己那个 DSA 的寄存器。
  EXPECT_EQ(&core.Rv(0).DsaCfg(), &core.GetDte().Cfg());
  EXPECT_EQ(&core.Rv(1).DsaCfg(), &core.GetMu().Cfg());
  EXPECT_EQ(&core.Rv(2).DsaCfg(), &core.GetVu().Cfg());
  // Router 与 TS 之间的控制线。
  EXPECT_EQ(&core.GetRouter().Trigger(), &core.GetTs().Trigger());
}

TEST(BachCore, NoDsaDirectLink) {
  // 三个 DSA 之间没有任何直连：数据一律经存储交换。这里验证它们只与存储和各自
  // 的 RV core 相连，没有共用的数据端口。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreContext ctx;
  Core core(clk, "core", ctx);

  // MU 与 VU 各占 Core Mem 的一个口，互不相同。
  EXPECT_NE(&core.GetMu().TokenPort(), &core.GetVu().CmemLd());
  // VU 不能直接读 Matrix Mem：它根本没有 Matrix Mem 的口。要 Matrix Mem 里的
  // 数据得先由 DTE 搬到 Core Mem。
  EXPECT_EQ(&core.Cmem().Port(kCmemVuRd), &core.GetVu().CmemLd());
  EXPECT_EQ(&core.Mmem().Port(kMmemMu), &core.GetMu().WeightPort());
}

// ── boot 序列 ──

TEST(BachChip, ScpBootReachesBusiness) {
  // 自启动 → PCIe 训练 → 全 chip 每个 core 的 Router → 逐个 core 的五步 →
  // 三个 RV core 的 ready 全高 → 开放业务接收。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  ChipCfg cfg;
  cfg.shape = ChipShape::kMiddle;
  Chip chip(clk, "chip", cfg);
  ChipDriver drv(clk, chip);

  // Router 那一段：全 chip 一个 core 不落。漏掉任何一个，经过它的 path 就全断。
  for (uint64_t i = 0; i < chip.CoreNum(); ++i) {
    ScpTxn t;
    t.core = i;
    t.addr = kCfgRouterBase;
    t.data = 0x1234;
    chip.Scp().PushRouterTxn(t);
  }
  // 每个 core 的五步之一：firmware 写进 ITCM。
  for (uint64_t i = 0; i < chip.CoreNum(); ++i) {
    ScpTxn t;
    t.core = i;
    t.addr = kCfgItcmBase;
    t.data = 0x13;   // nop
    chip.Scp().PushCoreTxn(t);
  }

  clk->Continue(400 * kPeriod);
  RT::JoinAll();

  EXPECT_EQ(chip.Scp().State(), ScpState::kBusiness);
  EXPECT_TRUE(chip.Scp().BusinessOpen());
  EXPECT_EQ(chip.Scp().Issued(), 2 * chip.CoreNum());
}

TEST(BachChip, CtrlNocRoutesByAddress) {
  // 端点按 addr_map 查目的模块，`cfg_core` 命中本 core 或带广播标记才收。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  ChipCfg cfg;
  Chip chip(clk, "chip", cfg);
  ChipDriver drv(clk, chip);

  // 一笔发给 core2 的、一笔广播的。
  ScpTxn a;
  a.core = 2;
  a.addr = kCfgTsBase + 0x40;
  chip.Scp().PushRouterTxn(a);
  ScpTxn b;
  b.core = 0;
  b.addr = kCfgSmemBase;
  b.bcast = true;
  chip.Scp().PushRouterTxn(b);

  clk->Continue(200 * kPeriod);
  RT::JoinAll();

  // core2 收了两笔（一笔点名、一笔广播），别的 core 只收了广播那一笔。
  EXPECT_EQ(chip.Noc(2).Taken(), 2u);
  EXPECT_EQ(chip.Noc(0).Taken(), 1u);
  EXPECT_EQ(chip.Noc(5).Taken(), 1u);
}

TEST(BachChip, CtrlNocFlagsAddressOutOfView) {
  // 地址不在本 core 视野内时记地址错，不下发。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  ChipCfg cfg;
  Chip chip(clk, "chip", cfg);
  ChipDriver drv(clk, chip);

  ScpTxn t;
  t.core = 1;
  t.addr = 0xF00000;   // 超出全部段
  chip.Scp().PushRouterTxn(t);

  clk->Continue(200 * kPeriod);
  RT::JoinAll();

  EXPECT_EQ(chip.Noc(1).AddrErrors(), 1u);
  EXPECT_EQ(chip.Noc(1).Taken(), 0u);
}

TEST(BachChip, CfgAddressMap) {
  // 逐段查一遍：每个基址落在自己那个模块上，三个 RV core 的 ITCM 与 DTCM 按
  // core 号分开。
  EXPECT_EQ(LookupCfg(kCfgRouterBase).target, kCfgRouter);
  EXPECT_EQ(LookupCfg(kCfgTsBase).target, kCfgTs);
  EXPECT_EQ(LookupCfg(kCfgDteBase).target, kCfgDte);
  EXPECT_EQ(LookupCfg(kCfgMuBase).target, kCfgMu);
  EXPECT_EQ(LookupCfg(kCfgVuBase).target, kCfgVu);
  EXPECT_EQ(LookupCfg(kCfgSmemBase).target, kCfgSmem);
  EXPECT_EQ(LookupCfg(kCfgCmemBase).target, kCfgCmem);
  EXPECT_EQ(LookupCfg(kCfgMmemBase).target, kCfgMmem);

  CfgRoute rv2 = LookupCfg(kCfgItcmBase + 2 * kCfgRvStride + 0x40);
  EXPECT_EQ(rv2.target, kCfgItcm);
  EXPECT_EQ(rv2.index, 2u);
  EXPECT_EQ(rv2.offset, 0x40u);
  CfgRoute dt1 = LookupCfg(kCfgDtcmBase + kCfgRvStride + 8);
  EXPECT_EQ(dt1.target, kCfgDtcm);
  EXPECT_EQ(dt1.index, 1u);
  // 段与段之间不重叠：VU 的 Profile 到 0x40EC，所以它占两倍宽度。
  EXPECT_EQ(LookupCfg(kCfgVuBase + kCfgDsaSize).target, kCfgVu);
  EXPECT_EQ(LookupCfg(0xF00000).target, kCfgNone);
}

// ── C2C Bridge ──

TEST(BachC2c, CreditIsPrivateThenShared) {
  // 每 VC 一个 private 计数器，加每方向一个 shared 计数器。发送先扣 private
  // 再扣 shared，归还先补 private。
  C2cCredit c(2, 3);
  EXPECT_EQ(c.Private(0), 2u);
  EXPECT_EQ(c.Shared(), 3u);
  c.Take(0);
  c.Take(0);
  EXPECT_EQ(c.Private(0), 0u);
  EXPECT_EQ(c.Shared(), 3u);
  c.Take(0);   // private 用完了，借 shared
  EXPECT_EQ(c.Shared(), 2u);
  c.Give(0);   // 归还先补 private
  EXPECT_EQ(c.Private(0), 1u);
  EXPECT_EQ(c.Shared(), 2u);
  // 一个方向的 credit 总量不超过对侧该方向的 buffer 容量。
  c.Give(0);
  c.Give(0);
  c.Give(0);
  EXPECT_EQ(c.Private(0), 2u);
  EXPECT_EQ(c.Shared(), 3u);
}

TEST(BachC2c, TxSplitsAtSegmentBoundary) {
  // 按 4 KB 边界拆分，加 4 位 seq_id 与 tail 标记，位宽 2048 转 1024 —— 一段
  // 占两拍。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  C2cTxEngine tx(clk, "tx", 0, false);

  C2cBeat b;
  b.seg.vc = 1;
  b.seg.bytes = 3 * kC2cSegBytes + 100;   // 四段
  b.seg.tail = true;
  tx.Push(b);

  std::vector<C2cBeat> got;
  while (tx.HasBeat()) got.push_back(tx.TakeBeat());
  ASSERT_EQ(got.size(), 8u) << "四段各两拍";
  for (uint64_t i = 0; i < 4; ++i) {
    EXPECT_EQ(got[i * 2].seg.seq_id, i);
    EXPECT_EQ(got[i * 2].seg.seq_id, got[i * 2 + 1].seg.seq_id);
    // 只有最后一段带 tail。
    EXPECT_EQ(got[i * 2].seg.tail, i == 3) << "i=" << i;
  }
  EXPECT_EQ(got[0].seg.bytes, kC2cSegBytes);
  EXPECT_EQ(got[6].seg.bytes, 100u);
}

TEST(BachC2c, ReleasePassesThroughWithoutSplit) {
  // 三类 release 一律透传：Bridge 自身不建 stream credit 表，也不参与 Reduce
  // 累加，release 不拆包。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  C2cTxEngine tx(clk, "tx", 0, false);

  C2cBeat b;
  b.is_release = true;
  b.rel.stream_valid = true;
  b.rel.stream_user = 42;
  tx.Push(b);

  ASSERT_TRUE(tx.HasBeat());
  C2cBeat got = tx.TakeBeat();
  EXPECT_TRUE(got.is_release);
  EXPECT_EQ(got.rel.stream_user, 42u);
  EXPECT_FALSE(tx.HasBeat()) << "release 只占一拍，不按位宽拆";
}

}  // namespace

namespace {

// 两座桥对接：一侧的 TakeOut() 喂给另一侧的 PushIn()。
class BridgePair : public BachModule {
 public:
  BridgePair(ClockPtr c, C2cBridge& l, C2cBridge& r)
      : BachModule(c, "pair"), left(l), right(r) {}

 protected:
  void Step() override {
    left.RunStep();
    right.RunStep();
    while (left.HasOut()) right.PushIn(left.TakeOut());
    while (right.HasOut()) left.PushIn(right.TakeOut());
  }

 private:
  C2cBridge& left;
  C2cBridge& right;
};

// 往桥的 core 侧灌一个整包，再从对侧的 core 侧收。
class C2cFeeder : public BachModule {
 public:
  C2cFeeder(ClockPtr c, LinkEndPtr in_wire, uint64_t bytes, MessagePtr pkt)
      : BachModule(c, "feed"), wire(std::move(in_wire)), size(bytes),
        msg(std::move(pkt)) {}
  bool sent = false;

 protected:
  void Step() override {
    if (sent) {
      wire->Idle();
      return;
    }
    wire->flit.Drive(/*vc=*/1, /*is_head=*/true, /*is_tail=*/true, size, msg);
    wire->release.Idle();
    sent = true;
  }

 private:
  LinkEndPtr wire;
  uint64_t size;
  MessagePtr msg;
};

class C2cSink : public BachModule {
 public:
  C2cSink(ClockPtr c, LinkEndPtr out_wire)
      : BachModule(c, "sink"), wire(std::move(out_wire)) {}
  uint64_t got = 0, bytes = 0, user = 0, at = 0;

 protected:
  void Step() override {
    FlitView f = ReadFlit(wire->flit);
    if (!f.valid) return;
    ++got;
    bytes = f.bytes;
    if (got == 1) at = CycleNow();
    if (f.msg) user = f.msg->user_id;
  }

 private:
  LinkEndPtr wire;
};

TEST(BachC2c, TwoBridgesPassThroughUnchanged) {
  // 只做透明传输：左侧收到的包默认发到右侧，Bridge 不做路由判断。包在 TX 那边
  // 按 4 KB 拆、位宽减半，在 RX 那边按 seq_id 拼回来 —— 出来的还是原来那一个。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  C2cCfg cfg;
  C2cBridge a(clk, "a", cfg);
  C2cBridge b(clk, "b", cfg);
  BridgePair pair(clk, a, b);

  auto msg = std::make_shared<Message>();
  msg->user_id = 314;
  const uint64_t kBytes = 3 * kC2cSegBytes + 512;
  C2cFeeder feed(clk, a.FromCore(), kBytes, msg);
  C2cSink sink(clk, b.ToCore());

  // 一段 AXI 延迟加上三关与拆拼，给足拍数。
  clk->Continue(2000 * kPeriod);
  RT::JoinAll();

  EXPECT_EQ(sink.got, 1u) << "拆开的四段应当拼回成一个包";
  EXPECT_EQ(sink.bytes, kBytes) << "拼回来的字节数要与原包一致";
  EXPECT_EQ(sink.user, 314u) << "包本身透传，内容不变";
}

TEST(BachC2c, LinkLatencyCountsOnce) {
  // 一段线的时间只算一次：出方向那座桥记这一段的延迟，入方向那座只做协议转换。
  // 两侧都记的话，Router 到 Router 就成了 600T，超过规格书给的 400T。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  C2cCfg cfg;
  cfg.axi_latency = 100;
  C2cBridge a(clk, "a", cfg);
  C2cBridge b(clk, "b", cfg);
  BridgePair pair(clk, a, b);

  auto msg = std::make_shared<Message>();
  msg->user_id = 5;
  C2cFeeder feed(clk, a.FromCore(), 256, msg);
  C2cSink sink(clk, b.ToCore());

  clk->Continue(600 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink.got, 1u);
  EXPECT_GE(sink.at, cfg.axi_latency) << "这一段的延迟要记上";
  EXPECT_LT(sink.at, 2 * cfg.axi_latency) << "两侧都记就成了两段";
}

// 每行左右两端各接一座 C2C Bridge，一个 chip 四座。
TEST(BachChip, FourBridgesPerChip) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  ChipCfg cfg;
  Chip chip(clk, "chip", cfg);

  EXPECT_EQ(uint64_t(kChipPortNum), 4u);
  // 四个口各是一座桥，互不相同。
  std::set<C2cBridge*> seen;
  for (uint64_t d = 0; d < kChipPortNum; ++d) {
    seen.insert(&chip.Port(d));
  }
  EXPECT_EQ(seen.size(), 4u) << "四个口不能共用同一座桥";
  RT::Reset();
}

// 不派角色的 core 只能转发，不承担 compute 与两个专用角色。
TEST(BachChip, SpareCoreTakesNoRole) {
  for (ChipShape shape : {ChipShape::kFirst, ChipShape::kLast}) {
    for (uint64_t i = 0; i < CoresOf(shape); ++i) {
      CoreRole r = RoleOf(shape, i);
      if (r != CoreRole::kSpare) continue;
      // 不派角色的那个既不是 compute，也不是 B core 或 R core。
      EXPECT_NE(r, CoreRole::kCompute);
      EXPECT_NE(r, CoreRole::kBroadcast);
      EXPECT_NE(r, CoreRole::kReduce);
    }
  }
  // 中间列一个不派角色的都没有。
  for (uint64_t i = 0; i < CoresOf(ChipShape::kMiddle); ++i) {
    EXPECT_NE(RoleOf(ChipShape::kMiddle, i), CoreRole::kSpare);
  }
}

// 同向的数据与 credit release 之间仲裁，小包优先：release 是最小的那种。
// 用业务层那一类来验 —— VC 那一类是 flit 粒度，跨 C2C 要先攒成包粒度。
TEST(BachC2c, ReleaseGoesBeforeData) {
  bool first_is_release = false;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    C2cCfg cfg;
    cfg.axi_latency = 0;
    C2cBridge br(clk, "br", cfg);

    // 同一拍在 core 侧既有数据又有 release。
    class BothFeeder : public BachModule {
     public:
      BothFeeder(ClockPtr c, LinkEndPtr w) : BachModule(c, "feed"), wire(std::move(w)) {}

     protected:
      void Step() override {
        if (sent) {
          wire->flit.Idle();
          wire->release.Idle();
          return;
        }
        auto m = std::make_shared<Message>();
        m->user_id = 7;
        m->size = 256;
        wire->flit.Drive(1, true, true, 256, m);
        wire->release.Drive(false, 0, /*stream_rel=*/true, 42, false, 0);
        sent = true;
      }

     private:
      LinkEndPtr wire;
      bool sent = false;
    };
    class Drain : public BachModule {
     public:
      Drain(ClockPtr c, C2cBridge& b) : BachModule(c, "drain"), br(b) {}
      bool first_release = false;
      bool got_any = false;

     protected:
      void Step() override {
        br.RunStep();
        while (br.HasOut()) {
          C2cBeat beat = br.TakeOut();
          if (!got_any) {
            got_any = true;
            first_release = beat.is_release;
          }
        }
      }

     private:
      C2cBridge& br;
    };
    BothFeeder feed(clk, br.FromCore());
    Drain drain(clk, br);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    first_is_release = drain.first_release;
  }
  RT::Reset();
  EXPECT_TRUE(first_is_release) << "release 是小包，排在数据前面";
}


// VC credit 跨 C2C 从 flit 粒度转成包粒度：攒够一个包的量才传一笔，笔上带着
// 它代表几个 flit。
TEST(BachC2c, VcCreditIsPackedAcrossC2c) {
  uint64_t beats = 0, total_len = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    C2cCfg cfg;
    cfg.axi_latency = 0;
    C2cBridge br(clk, "br", cfg);

    // core 侧连着还八个 flit 的 VC credit。
    class CreditFeeder : public BachModule {
     public:
      CreditFeeder(ClockPtr c, LinkEndPtr w)
          : BachModule(c, "feed"), wire(std::move(w)) {}

     protected:
      void Step() override {
        wire->flit.Idle();
        if (sent < 8) {
          wire->release.Drive(true, 1, false, 0, false, 0);
          ++sent;
        } else {
          wire->release.Idle();
        }
      }

     private:
      LinkEndPtr wire;
      uint64_t sent = 0;
    };
    class Drain : public BachModule {
     public:
      Drain(ClockPtr c, C2cBridge& b) : BachModule(c, "drain"), br(b) {}
      uint64_t beats = 0, total = 0;

     protected:
      void Step() override {
        br.RunStep();
        while (br.HasOut()) {
          C2cBeat beat = br.TakeOut();
          if (!beat.is_release || !beat.rel.vc_valid) continue;
          ++beats;
          total += beat.vc_len;
        }
      }

     private:
      C2cBridge& br;
    };
    CreditFeeder feed(clk, br.FromCore());
    Drain drain(clk, br);
    clk->Continue(80 * kPeriod);
    RT::JoinAll();
    beats = drain.beats;
    total_len = drain.total;
  }
  RT::Reset();
  EXPECT_EQ(total_len, 8u) << "八个 flit 的量一个不少";
  EXPECT_LT(beats, 8u) << "打成了几笔，不是一 flit 一笔";
  EXPECT_GE(beats, 2u) << "四个一笔，八个至少两笔";
}

// ── 空转 ──

class CoreDriver : public BachModule {
 public:
  CoreDriver(ClockPtr c, Core& target) : BachModule(c, "driver"), core(target) {}

 protected:
  void Step() override { core.RunStep(); }

 private:
  Core& core;
};

TEST(BachCore, IdleCyclesDoNotClash) {
  // core 内近百个模块每拍都在驱动自己的端口。任何一处两个模块写同一根线，
  // Latch 当场断言 —— 空转本身就是一条判据，接线错了跑不过去。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  CoreContext ctx;
  Core core(clk, "core", ctx);
  CoreDriver drv(clk, core);

  clk->Continue(500 * kPeriod);
  RT::JoinAll();

  // 没有活干，各单元应当都是静的。
  EXPECT_TRUE(core.Quiescent());
  // 三个 RV core 都在 wait 上，SCP 据此开放业务接收。
  EXPECT_TRUE(core.Ready());
}

TEST(BachChip, IdleCyclesDoNotClash) {
  // 整片一起空转：加上 core 之间的 Link、四座桥与 ctrl_noc 端点。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  ChipCfg cfg;
  cfg.shape = ChipShape::kFirst;   // 2×5，带一个不派角色的 core
  Chip chip(clk, "chip", cfg);
  ChipDriver drv(clk, chip);

  clk->Continue(500 * kPeriod);
  RT::JoinAll();

  EXPECT_TRUE(chip.Quiescent());
  EXPECT_EQ(chip.CoreNum(), 10u);
}

}  // namespace

namespace {

TEST(BachChip, CtrlNocWritesReachTargets) {
  // 端点查出目的模块之后，下一拍真的把值写进去了。这里挑三处能直接读回来的：
  // MU 的配置寄存器、Share Mem 与 Core Mem 的后门。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  ChipCfg cfg;
  Chip chip(clk, "chip", cfg);
  ChipDriver drv(clk, chip);

  ScpTxn mu_cfg;
  mu_cfg.core = 1;
  mu_cfg.addr = kCfgMuBase + kMuTaskBlock;
  mu_cfg.data = 3u | (5u << 16);        // KBLOCK=3、NBLOCK=5
  chip.Scp().PushRouterTxn(mu_cfg);

  ScpTxn sm;
  sm.core = 1;
  sm.addr = kCfgSmemBase + 0x40;
  sm.data = 0xDEADBEEF;
  chip.Scp().PushRouterTxn(sm);

  ScpTxn cm;
  cm.core = 1;
  cm.addr = kCfgCmemBase + 0x80;
  cm.data = 0x12345678;
  chip.Scp().PushRouterTxn(cm);

  clk->Continue(300 * kPeriod);
  RT::JoinAll();

  Core& c = chip.GetCore(1);
  EXPECT_EQ(c.GetMu().Regfile().Live().kblock, 3u);
  EXPECT_EQ(c.GetMu().Regfile().Live().nblock, 5u);

  auto read = [](BankedMem& mem, uint64_t at) {
    std::vector<uint8_t> b = mem.Peek(at, 4);
    uint64_t v = 0;
    for (int k = 0; k < 4; ++k) v |= uint64_t(b[k]) << (8 * k);
    return v;
  };
  EXPECT_EQ(read(c.Smem(), 0x40), 0xDEADBEEFu);
  EXPECT_EQ(read(c.Cmem(), 0x80), 0x12345678u);
}

}  // namespace

// ── 跨 core 的端到端 ──

namespace {

std::string ChipKernelDir() {
  return std::string(LATCH_SOURCE_DIR) + "/src/bach/compiler/kernel/build/";
}

uint64_t ChipSymbolOf(std::string const& name, std::string const& kind) {
  std::ifstream f(ChipKernelDir() + "kernel_" + kind + ".sym");
  std::string addr, type, sym;
  while (f >> addr >> type >> sym) {
    if (sym == name) return std::stoull(addr, nullptr, 16);
  }
  return 0;
}

bool ChipKernelBuilt() {
  std::ifstream f(ChipKernelDir() + "kernel_dte.hex");
  return f.good();
}

MessagePtr ChipToken(uint64_t path, uint64_t user, uint64_t bytes) {
  auto m = std::make_shared<Message>();
  m->path_id = path;
  m->user_id = user;
  m->size = bytes;
  m->compute = 1;
  m->stream_id = 0;
  m->task_id = 0;
  m->payload.resize(bytes);
  for (uint64_t i = 0; i < bytes; ++i) {
    m->payload[i] = uint8_t((user * 11 + i * 5) & 0xFFu);
  }
  return m;
}

// 一个 core 上的两步链：收进来搬进 Core Mem，再按 out_path 发出去。
void WriteRelayChain(Core& core, uint64_t in_path, uint64_t out_path) {
  TaskEntry in;
  in.send_unit = SendUnit::kDte;
  in.recv_unit = RecvUnit::kDsa;
  in.dsa_en = true;
  in.exe_mask = true;
  in.path_id = in_path;
  in.task_pc = ChipSymbolOf("task_dte_user_init", "dte");
  core.GetTs().Cfg().WriteTask(0, in);

  TaskEntry out;
  out.send_unit = SendUnit::kDte;
  out.recv_unit = RecvUnit::kDsa;
  out.dsa_en = true;
  out.end = true;
  out.exe_mask = true;
  out.path_id = out_path;
  out.task_pc = ChipSymbolOf("task_dte_move", "dte");
  core.GetTs().Cfg().WriteTask(1, out);

  core.GetTs().Cfg().WritePathMap(in_path, 0);
  core.GetTs().Cfg().SetInitFinish();
  // DTE 那一份 path_task_map 与 TS 配成一样：进核搬运报完成时按它填 task_id。
  core.GetDte().Tables().PreloadPathTask(in_path, 0);
}

RouteEntry ChipEnterCore() {
  RouteEntry e;
  e.flow_dir = 0;
  e.path_core_bypass = false;
  e.operation = Operation::kForward;
  return e;
}

RouteEntry ChipLeaveTo(uint64_t flow) {
  RouteEntry e;
  e.flow_dir = flow;
  e.path_core_bypass = true;
  e.operation = Operation::kForward;
  return e;
}

// 两个 core 与它们之间那条链路由这一个协程驱动，外面那两根线也在这里驱动与
// 读取。出口上的 flit 是单拍脉冲，分成两个协程谁先跑不定，会整拍错过。
class TwoCoreHarness : public BachModule {
 public:
  TwoCoreHarness(ClockPtr c, Core& first, Core& second,
                 std::vector<Link*> wires, uint64_t at, MessagePtr m)
      : BachModule(c, "harness"), c0(first), c1(second),
        links(std::move(wires)), fire_at(at), msg(std::move(m)) {}

  uint64_t out_flits = 0;
  std::vector<MessagePtr> out_msgs;
  // 两个 core 各自的表都走空了才算这一笔真的走完。
  uint64_t head0 = 0, head1 = 0;

 protected:
  void Step() override {
    head0 = c0.GetTs().Table().HeadPtr();
    head1 = c1.GetTs().Table().HeadPtr();
    LinkEndPtr in = c0.InWire(kLeftDir);
    if (CycleNow() == fire_at && msg) {
      in->flit.Drive(0, true, true, msg->size, msg);
    } else {
      in->flit.Idle();
    }
    // 两个 core 各自没接上的那几根回程线由这里拉平。
    c0.BackWire(kLeftDir)->flit.Idle();
    c1.BackWire(kRightDir)->flit.Idle();

    for (Link* l : links) l->RunStep();
    c0.RunStep();
    c1.RunStep();

    FlitView f = ReadFlit(c1.OutWire(kRightDir)->flit);
    if (!f.valid) return;
    ++out_flits;
    if (f.msg) out_msgs.push_back(f.msg);
  }

 private:
  Core& c0;
  Core& c1;
  std::vector<Link*> links;
  uint64_t fire_at;
  MessagePtr msg;
};

}  // namespace

// 一个 token 走过两个 core：前一个收下搬进 Core Mem 再往右发，后一个收下搬进
// 自己的 Core Mem 再往右发出去，出来的字节与注入的相等。
TEST(BachChip, TokenRelaysAcrossTwoCores) {
  if (!ChipKernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  // 512 B 是一个 K=256 的 BF16 token，与 kernel 里的 TOKEN_BYTES 同一个数。
  constexpr uint64_t kBytes = 512;
  MessagePtr token = ChipToken(3, 42, kBytes);
  std::vector<MessagePtr> got;
  std::pair<uint64_t, uint64_t> heads{0, 0};
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreContext ctx0, ctx1;
    ctx0.core_id = 0;
    ctx1.core_id = 1;
    Core c0(clk, "core0", ctx0);
    Core c1(clk, "core1", ctx1);

    // core0 的右出口接 core1 的左入口，VC credit 沿反方向回来。
    Link fwd(clk, "l.fwd", LinkR2R(), 0, false);
    fwd.AttachIn(c0.OutWire(kRightDir));
    fwd.AttachOut(c1.InWire(kLeftDir));
    Link back(clk, "l.back", LinkR2R(), 0, false);
    back.AttachIn(c1.UpBackWire(kLeftDir));
    back.AttachOut(c0.BackWire(kRightDir));

    // path 3 进 core0，出 core0 后往右；path 4 进 core1，出 core1 后往右。
    c0.GetRouter().Preload(3, ChipEnterCore());
    c0.GetRouter().Preload(4, ChipLeaveTo(kFlowRight));
    c1.GetRouter().Preload(4, ChipEnterCore());
    c1.GetRouter().Preload(5, ChipLeaveTo(kFlowRight));
    c0.Rv(0).LoadImage(ChipKernelDir() + "kernel_dte.hex");
    c1.Rv(0).LoadImage(ChipKernelDir() + "kernel_dte.hex");
    WriteRelayChain(c0, /*in_path=*/3, /*out_path=*/4);
    WriteRelayChain(c1, /*in_path=*/4, /*out_path=*/5);

    TwoCoreHarness harness(clk, c0, c1, {&fwd, &back}, /*at=*/2, token);
    clk->Continue(2000 * kPeriod);
    RT::JoinAll();
    got = harness.out_msgs;
    heads = {harness.head0, harness.head1};
  }
  RT::Reset();
  EXPECT_EQ(heads.first, 1u);    // 两个 core 上这个用户都退休了
  EXPECT_EQ(heads.second, 1u);
  ASSERT_FALSE(got.empty()) << "第二个 core 的出口上一个包都没有";
  MessagePtr sent = got.front();
  EXPECT_EQ(sent->user_id, token->user_id);
  ASSERT_EQ(sent->payload.size(), kBytes);
  for (uint64_t i = 0; i < kBytes; ++i) {
    ASSERT_EQ(sent->payload[i], token->payload[i]) << "第 " << i << " 个字节";
  }
}
