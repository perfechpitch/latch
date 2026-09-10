// 进出口桩与整条片外通路的行为基线。
//
// 步 2 的判据在最后一个用例：一个包从入口桩发出，经链路与 Switch 回到出口桩，
// 到达拍与手算一致。

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/external/in_stub.h"
#include "bach/ip/external/out_stub.h"
#include "bach/ip/link/link.h"
#include "bach/ip/pcie_switch.h"
#include "bach/ip/wiring.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

// 在协程内抄计数，主线程读不到 Logic64 的当前值。
class Probe : public BachModule {
 public:
  Probe(ClockPtr c, InStub& src, OutStub& dst)
      : BachModule(c, "probe"), in(src), out(dst) {}

  uint64_t injected = 0, pool_avail = 0, done = 0, mismatch = 0, last = 0;

 protected:
  void Step() override {
    injected = in.Injected();
    pool_avail = in.PoolAvail();
    done = out.DoneCount();
    mismatch = out.MismatchCount();
    if (out.DoneCount() != 0) last = out.LastCycle();
  }

 private:
  InStub& in;
  OutStub& out;
};

InjectItem Token(uint64_t at, uint64_t gpu, uint64_t id, uint64_t bytes = 256) {
  InjectItem it;
  it.inject_cycle = at;
  it.gpu_id = gpu;
  it.token_id = id;
  it.dst = 1;
  it.bytes = bytes;
  return it;
}

}  // namespace

// 第一层闸门：GPU 本地缓冲占满就不再放行。深度 2 时前两笔走，第三笔卡住。
TEST(BachInStub, LocalCreditGate) {
  uint64_t injected = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    InStub stub(clk, "in", /*pool_total=*/100);
    stub.SetGpuBuffer(0, 2);
    stub.SetInjectTable({Token(1, 0, 0), Token(1, 0, 1), Token(1, 0, 2)});
    OutStub sink(clk, "out");
    Probe probe(clk, stub, sink);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    injected = probe.injected;
  }
  RT::Reset();
  EXPECT_EQ(injected, 2u);
}

// 第二层闸门：全局池子。池子 1 时只放行一笔，与本地缓冲多深无关。
TEST(BachInStub, PoolGate) {
  uint64_t injected = 0, pool = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    InStub stub(clk, "in", /*pool_total=*/1);
    stub.SetGpuBuffer(0, 16);
    stub.SetInjectTable({Token(1, 0, 0), Token(1, 0, 1), Token(1, 0, 2)});
    OutStub sink(clk, "out");
    Probe probe(clk, stub, sink);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    injected = probe.injected;
    pool = probe.pool_avail;
  }
  RT::Reset();
  EXPECT_EQ(injected, 1u);
  EXPECT_EQ(pool, 0u);
}

// retired 回来腾出位置，后面的才继续走。
TEST(BachInStub, RetiredFreesCredit) {
  uint64_t injected = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    InStub stub(clk, "in", /*pool_total=*/1);
    stub.SetGpuBuffer(0, 1);
    stub.SetInjectTable({Token(1, 0, 0), Token(1, 0, 1), Token(1, 0, 2)});
    OutStub sink(clk, "out");
    // 出口桩收齐一个就回报，入口桩据此放行下一个
    sink.OnRetired([&](uint64_t gpu, uint64_t token) {
      stub.ReportRetired(gpu, /*src=*/0, token + 1);
    });

    LinkEndPtr wire = MakeWire(clk);
    stub.AttachTx(wire);
    sink.AttachRx(wire);

    Probe probe(clk, stub, sink);
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    injected = probe.injected;
  }
  RT::Reset();
  EXPECT_EQ(injected, 3u);
}

// 组播下 retired 取 min：两个收端，只有慢的那个也退了才腾位置。
TEST(BachInStub, RetiredTakesMinAcrossSources) {
  uint64_t injected = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    InStub stub(clk, "in", /*pool_total=*/8);
    stub.SetGpuBuffer(0, 1);
    stub.SetInjectTable({Token(1, 0, 0), Token(1, 0, 1)});
    OutStub sink(clk, "out");
    Probe probe(clk, stub, sink);
    // 只有 src=0 报了退到 1，src=1 还停在 0，min 仍是 0，不腾位置
    stub.ReportRetired(0, 0, 1);
    stub.ReportRetired(0, 1, 0);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    injected = probe.injected;
  }
  RT::Reset();
  EXPECT_EQ(injected, 1u);
}

// LPU Dispatch：任一 R core 没余量就不派遣。
TEST(BachInStub, DispatchNeedsAllReduceSlots) {
  uint64_t injected = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    InStub stub(clk, "in", /*pool_total=*/8);
    stub.SetGpuBuffer(0, 8);
    stub.SetDispatchSlots({2, 0});  // 第二个 R core 没余量
    stub.SetInjectTable({Token(1, 0, 0), Token(1, 0, 1)});
    OutStub sink(clk, "out");
    Probe probe(clk, stub, sink);
    clk->Continue(60 * kPeriod);
    RT::JoinAll();
    injected = probe.injected;
  }
  RT::Reset();
  EXPECT_EQ(injected, 0u);
}

// 一个包切成多个 flit，出口桩按尾 flit 判收齐，一个 token 只算一次。
TEST(BachOutStub, ReassemblesMultiFlitToken) {
  uint64_t done = 0;
  std::vector<OutStub::TokenKey> set;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    InStub stub(clk, "in", /*pool_total=*/8);
    stub.SetGpuBuffer(0, 8);
    // 6368 B 要 25 个 flit
    stub.SetInjectTable({Token(1, 0, 5, kTokenBytes)});
    OutStub sink(clk, "out");
    LinkEndPtr wire = MakeWire(clk);
    stub.AttachTx(wire);
    sink.AttachRx(wire);
    Probe probe(clk, stub, sink);
    clk->Continue(200 * kPeriod);
    RT::JoinAll();
    done = probe.done;
    set = sink.DoneSet();
  }
  RT::Reset();
  EXPECT_EQ(done, 1u);
  ASSERT_EQ(set.size(), 1u);
  EXPECT_EQ(set[0].first, 0u);
  EXPECT_EQ(set[0].second, 5u);
}

// 步 2 的判据：入口桩 → ETH 链路 → Switch → PCIe 链路 → 出口桩，
// 到达拍与手算一致。
TEST(BachExternal, EndToEndThroughSwitch) {
  uint64_t done = 0, last = 0;
  {
    ClockPtr clk = MakeClock(0, kPeriod);
    InStub stub(clk, "in", /*pool_total=*/8);
    stub.SetGpuBuffer(0, 8);
    stub.SetInjectTable({Token(1, 0, 3, 256)});

    // 桩到 Switch 走 ETH 参数，Switch 到桩走 PCIe 出口参数
    Link eth(clk, "eth", LinkEthIn());
    PcieSwitch sw(clk, "sw", 2);
    sw.SetRoute(1, {1});
    Link pcie(clk, "pcie", LinkPcieOut());
    OutStub sink(clk, "out");

    // 四根线，每根一个对象
    LinkEndPtr w1 = MakeWire(clk);  // 桩 → eth
    LinkEndPtr w2 = MakeWire(clk);  // eth → sw
    LinkEndPtr w3 = MakeWire(clk);  // sw → pcie
    LinkEndPtr w4 = MakeWire(clk);  // pcie → 桩
    stub.AttachTx(w1);
    eth.AttachIn(w1);
    eth.AttachOut(w2);
    sw.AttachIn(0, w2);
    sw.AttachOut(1, w3);
    pcie.AttachIn(w3);
    pcie.AttachOut(w4);
    sink.AttachRx(w4);

    Probe probe(clk, stub, sink);
    clk->Continue(8000 * kPeriod);
    RT::JoinAll();
    done = probe.done;
    last = probe.last;
  }
  RT::Reset();
  ASSERT_EQ(done, 1u);
  // 手算，每一段都能在参数表里查到：
  //   拍 1     入口桩注入并封包进 tx_q。Transmit 排在 Inject 之前（末级先做），
  //            所以本拍还发不出去
  //   拍 2     发第一个 flit 上 w1
  //   拍 3     eth 收下：arrive = 3 + ceil(256/50) + 3000 = 3009
  //   拍 3009  eth 出到 w2
  //   拍 3010  sw 收下进出口队列。Drain 也排在 Accept 之前，本拍出不去
  //   拍 3011  sw 出到 w3
  //   拍 3012  pcie 收下：arrive = 3012 + ceil(256/108) + 300 = 3315
  //   拍 3315  pcie 出到 w4
  //   拍 3316  出口桩收到尾 flit（单 flit 包，头即尾）
  //
  // 每过一个模块多一拍，是「上拍写、下拍读」的必然结果，与 RTL 的寄存器语义
  // 一致。链路那两段的传输与延迟拍数与 link.md 的参数逐项对得上。
  EXPECT_EQ(last, 3316u);
}

// 序号按模 2^16 比较：绕回 0 之后仍然分得清谁在前。
TEST(BachSeq, WrapAroundComparison) {
  EXPECT_EQ(SeqAdd(kSeqMod - 1, 1), 0u) << "加满一圈回到 0";
  EXPECT_EQ(SeqAdd(kSeqMod - 1, 3), 2u);
  // 绕回前后的差值仍然是正确的正数。
  EXPECT_EQ(SeqDiff(kSeqMod - 2, 1), 3u);
  EXPECT_TRUE(SeqAfter(kSeqMod - 2, 1)) << "绕过去的那一个更新";
  EXPECT_FALSE(SeqAfter(1, kSeqMod - 2)) << "反过来就不是";
  // 相等不算「在之后」。
  EXPECT_FALSE(SeqAfter(5, 5));
  // 差值超过半个模时算落在过去。
  EXPECT_FALSE(SeqAfter(0, kSeqHalf + 1));
}

// DPU 的自定义包头：gpu_id 与 token_id 随包走，出口桩按这一对认 token。
TEST(BachInStub, HeaderCarriesGpuAndTokenId) {
  std::vector<MessagePtr> got;
  {
    RT::Reset(8, 8);
    ClockPtr clk = MakeClock(0, kPeriod);
    InStub in(clk, "in", /*pool_size=*/8);
    in.SetGpuBuffer(3, 4);
    std::vector<InjectItem> tbl;
    InjectItem it;
    it.inject_cycle = 2;
    it.gpu_id = 3;
    it.token_id = 77;
    it.dst = 5;
    it.bytes = 256;
    tbl.push_back(it);
    in.SetInjectTable(tbl);

    // 收 tx 那一路，看包头里写了什么。
    class TxTap : public BachModule {
     public:
      TxTap(ClockPtr c, LinkEndPtr w) : BachModule(c, "tap"), wire(std::move(w)) {}
      std::vector<MessagePtr> msgs;

     protected:
      void Step() override {
        FlitView f = ReadFlit(wire->flit);
        if (f.valid && f.msg) msgs.push_back(f.msg);
      }

     private:
      LinkEndPtr wire;
    };
    TxTap tap(clk, in.TxPtr());
    clk->Continue(40 * kPeriod);
    RT::JoinAll();
    got = tap.msgs;
  }
  RT::Reset();
  ASSERT_FALSE(got.empty());
  EXPECT_EQ(got[0]->gpu_id, 3u);
  EXPECT_EQ(got[0]->token_id, 77u);
  EXPECT_EQ(got[0]->dst, 5u) << "PCIe Switch 按它查目的端口";
  EXPECT_EQ(got[0]->size, 256u);
}
