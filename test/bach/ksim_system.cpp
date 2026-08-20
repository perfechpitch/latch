// 装起来跑：核阵列、外围模块与停机判据一起挂在同一个时钟上。
//
// 覆盖四种指令各自怎么占时间、Recv 收不齐就停着等、跨 chip 到得了、额度顶得住，以及
// 观测产物记下来的东西对不对得上。

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "base/signal_tracer.h"
#include "bach/ksim/loader.h"
#include "bach/ksim/system.h"
#include "gtest/gtest.h"

using namespace latch;
using namespace latch::bach;
using namespace latch::bach::ksim;

namespace {

KernelOp Gemm(uint64_t cycles, uint64_t a = 0x100, uint64_t c = 0x200) {
  KernelOp op;
  op.kind = OpKind::kGemm;
  op.m = 8; op.k = 64; op.n = 128;
  op.a = a; op.c = c;
  op.cycles = cycles;
  return op;
}

KernelOp Elem(uint64_t cycles) {
  KernelOp op;
  op.kind = OpKind::kElem;
  op.n = 1024;
  op.cycles = cycles;
  return op;
}

KernelOp Send(uint64_t bytes, int64_t dst, uint64_t src = 0x200) {
  KernelOp op;
  op.kind = OpKind::kSend;
  op.length = bytes;
  op.dst_core = dst;
  op.src = src;
  return op;
}

KernelOp Recv(uint64_t bytes, uint64_t dst = 0x100) {
  KernelOp op;
  op.kind = OpKind::kRecv;
  op.length = bytes;
  op.dst = dst;
  return op;
}

// 一轮跑完之后留下的东西。
struct Outcome {
  bool finished = false;
  Time end_time = 0;
  Time compute = 0;
  Time wait = 0;
  uint64_t fed = 0;
  uint64_t drained = 0;
  uint64_t expect = 0;
  uint64_t spans = 0;
  uint64_t waits = 0;
  uint64_t completed = 0;
  std::vector<Time> finished_at;
  std::vector<bool> done;
};

Outcome RunOnce(KernelImage const& img, Topology topo, Time limit = 200000,
            uint64_t credit = 8, RouterParams rp = RouterParams{32, 1, 32, 40},
            CoreParams cp = CoreParams{4, 32}) {
  RT::Reset(4, topo.CoreCount() + 8);
  SetTraceDisabled(true);
  Outcome out;
  RunRecorder run;
  {
    ClockPtr clk = MakeClock(0, 1);
    Array arr(clk, img, topo, rp, cp);
    Host host(clk, img, &arr, topo, rp.pcie_latency, credit, "host", 0);
    arr.ConnectHost(&host);
    Control ctl(clk, &arr, &host, limit);

    clk->Continue();
    RT::JoinAll();

    for (uint64_t i = 0; i < arr.Size(); ++i) {
      Core* c = arr.At(i);
      run.Collect(c->Recorder());
      Exec& e = c->Unit();
      out.done.push_back(e.Done());
      out.finished_at.push_back(e.FinishedAt());
      if (!e.HasProgram()) continue;
      out.compute += e.ComputeCycles();
      out.wait += e.WaitCyclesAt(ctl.EndTime());
    }
    run.Collect(host.Recorder());
    run.Finalize(host.FedCount());
    out.finished = ctl.Finished();
    out.end_time = ctl.EndTime();
    out.fed = host.FedBytes();
    out.drained = host.DrainedBytes();
    out.expect = host.ExpectBytes();
  }
  out.spans = run.Spans().size();
  out.waits = run.Waits().size();
  out.completed = run.Result().completed_uids.size();
  return out;
}

KernelImage Tiny() {
  return LoadKernels(std::string(PROJECT_TEST_DIR) + "/bach/fixture/tiny.bachk");
}

}  // namespace

TEST(KsimSystem, RunsTheFixtureToCompletion) {
  const Outcome o = RunOnce(Tiny(), Topology{1, 1, 2, 4});
  EXPECT_TRUE(o.finished) << "没跑完，end_time=" << o.end_time;
  // core 0 四条 GEMM 各 96 加一条 ELEM 的 4，core 1 一条 GEMM 96。
  EXPECT_EQ(o.compute, 96u * 5 + 4);
  EXPECT_EQ(o.drained, o.expect);
  EXPECT_GT(o.end_time, 0u);
}

TEST(KsimSystem, ObservationsAreRecorded) {
  const Outcome o = RunOnce(Tiny(), Topology{1, 1, 2, 4});
  // 每条算的、每条搬的都留下一段占用，转发的每一跳也留一段。
  EXPECT_GT(o.spans, 10u);
  // 停在 Recv 上等对端那几段记成等待。
  EXPECT_GT(o.waits, 0u);
  // 喂出去两份，两份都配上了完成。
  EXPECT_EQ(o.completed, 2u);
}

TEST(KsimSystem, ComputeTakesTheCyclesInTheKernel) {
  KernelImage img;
  CoreKernel k;
  k.core = 0;
  k.ops = {Gemm(100), Elem(20), Gemm(30, 0x200, 0x300)};
  img.cores = {k};
  const Outcome o = RunOnce(img, Topology{1, 1, 2, 4});
  EXPECT_TRUE(o.done[0]);
  EXPECT_EQ(o.compute, 150u);
  EXPECT_EQ(o.wait, 0u);
}

TEST(KsimSystem, RecvBlocksUntilTheBytesArrive) {
  KernelImage img;
  CoreKernel a;
  a.core = 0;
  a.ops = {Gemm(50), Send(320, 1)};
  CoreKernel b;
  b.core = 1;
  b.ops = {Recv(320), Gemm(10, 0x100, 0x300)};
  img.cores = {a, b};
  const Outcome o = RunOnce(img, Topology{1, 1, 2, 4});
  EXPECT_TRUE(o.done[1]);
  // core 1 一直停在 Recv 上等，等的拍数记在等待里而不是计算里。
  EXPECT_GT(o.wait, 50u);
  EXPECT_EQ(o.compute, 60u);
  EXPECT_GT(o.finished_at[1], o.finished_at[0]);
}

TEST(KsimSystem, NeverFinishesWhenTheBytesNeverCome) {
  KernelImage img;
  CoreKernel b;
  b.core = 1;
  b.ops = {Recv(320)};
  img.cores = {b};
  const Outcome o = RunOnce(img, Topology{1, 1, 2, 4}, 500);
  EXPECT_FALSE(o.finished);
  EXPECT_EQ(o.end_time, 500u);
  EXPECT_GT(o.wait, 400u);
}

TEST(KsimSystem, AcrossChipsStillArrives) {
  KernelImage img;
  CoreKernel a;
  a.core = 0;
  a.ops = {Send(512, 8)};
  CoreKernel b;
  b.core = 8;
  b.ops = {Recv(512)};
  img.cores = {a, b};
  const Outcome o = RunOnce(img, Topology{1, 2, 2, 4}, 20000);
  EXPECT_TRUE(o.done[8]);
}

TEST(KsimSystem, SendAndNextRecvOverlap) {
  // 一个核连着做两层：这一层把结果推出去的同时，下一层的输入可以正在收进来。
  KernelImage img;
  CoreKernel k;
  k.core = 1;
  for (int i = 0; i < 2; ++i) {
    k.ops.push_back(Recv(3200));
    k.ops.push_back(Gemm(50));
    k.ops.push_back(Send(3200, -1));
  }
  CoreKernel up;
  up.core = 0;
  up.ops = {Send(3200, 1), Send(3200, 1)};
  img.cores = {up, k};
  img.drains = {Drain{1, 0, 3200}, Drain{1, 0, 3200}};
  const Outcome o = RunOnce(img, Topology{1, 1, 2, 4});
  EXPECT_TRUE(o.done[1]);
  // 一层是收 100 拍、算 50 拍、发 4 加 100 拍。两层全串起来要 508 拍；收发跟计算各走
  // 各的通道，所以实际比串起来短。
  EXPECT_LT(o.finished_at[1], 508u);
  EXPECT_EQ(o.compute, 100u);
}

TEST(KsimSystem, ReadWaitsForTheWriteThatFeedsIt) {
  // 后一条读的正是前一条写的那一块，必须等它写完。
  KernelImage img;
  CoreKernel k;
  k.core = 0;
  k.ops = {Gemm(200, 0x100, 0x9000), Gemm(30, 0x9000, 0xA000)};
  img.cores = {k};
  const Outcome o = RunOnce(img, Topology{1, 1, 2, 4});
  EXPECT_TRUE(o.done[0]);
  // 两条都在矩阵核上且有依赖，只能一前一后。
  EXPECT_GE(o.finished_at[0], 230u);
}

TEST(KsimSystem, CreditHoldsBackTheHost) {
  // 一个核连着要三份输入。每份 6400 字节按 32 字节每拍是 200 拍，跟算它的 200 拍相当，
  // 所以搬运能不能跟计算重叠看得出来。时序表说三份一起喂，额度只有一份的话后两份得等
  // 前一份的结果推出去才动得了。
  KernelImage img;
  CoreKernel k;
  k.core = 0;
  for (int i = 0; i < 3; ++i) {
    k.ops.push_back(Recv(6400));
    k.ops.push_back(Gemm(200));
    k.ops.push_back(Send(6400, -1));
  }
  img.cores = {k};
  for (int i = 0; i < 3; ++i) {
    img.feeds.push_back(Feed{0, 0, 0, 6400});
    img.drains.push_back(Drain{0, 0, 6400});
  }
  const Outcome tight = RunOnce(img, Topology{1, 1, 2, 4}, 200000, 1);
  const Outcome loose = RunOnce(img, Topology{1, 1, 2, 4}, 200000, 8);
  ASSERT_TRUE(tight.finished);
  ASSERT_TRUE(loose.finished);
  // 额度放开之后三份可以一起在路上，所以早一些跑完；两边算的量是一样的。
  EXPECT_LT(loose.end_time, tight.end_time);
  EXPECT_EQ(tight.compute, loose.compute);
  // 被额度挡住的那几段记成等额度。
  EXPECT_GT(tight.waits, loose.waits);
}

TEST(KsimSystem, CreditIsReturnedSoTheRunStillFinishes) {
  const Outcome o = RunOnce(Tiny(), Topology{1, 1, 2, 4}, 200000, 1);
  EXPECT_TRUE(o.finished) << "额度收紧之后卡住了，end_time=" << o.end_time;
  EXPECT_EQ(o.drained, o.expect);
}
