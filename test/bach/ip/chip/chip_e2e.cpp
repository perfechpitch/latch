// 跨 chip 的端到端：一个 token 经 C2C 从一颗 chip 走到另一颗。
//
// 与 chip.cpp 里那条两 core 接力的区别：那一条只走 chip 内的 Link，包在链路上
// 原样过去；这一条要过 C2C —— 出方向按 4 KB 拆段、位宽 2048 转 1024，入方向按
// seq_id 拼回来，中间隔着一段 300 拍的 PCIe 链路。
//
// 走的路与 LPU 里同行相邻两颗 chip 的接法一致：前一颗的 E 口对后一颗的 W 口。

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/chip.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

std::string KernelDir() {
  return std::string(LATCH_SOURCE_DIR) + "/src/bach/compiler/kernel/build/";
}

uint64_t SymbolOf(std::string const& name, std::string const& kind = "dte") {
  std::ifstream f(KernelDir() + "kernel_" + kind + ".sym");
  std::string addr, type, sym;
  while (f >> addr >> type >> sym) {
    if (sym == name) return std::stoull(addr, nullptr, 16);
  }
  return 0;
}

bool KernelBuilt() {
  std::ifstream f(KernelDir() + "kernel_dte.hex");
  return f.good();
}

// 512 B 是一个 K=256 的 BF16 token，与 kernel 里的 TOKEN_BYTES 同一个数。
constexpr uint64_t kTokenBytes = 512;

MessagePtr MakeToken(uint64_t path, uint64_t user) {
  auto m = std::make_shared<Message>();
  m->path_id = path;
  m->user_id = user;
  // DPU 写的那一对：出口桩按它认这是哪个 GPU 的第几个 token。两颗 chip 上各
  // 出核造一次新包，这一对要跟着数据过去。
  m->gpu_id = 2;
  m->token_id = user;
  m->size = kTokenBytes;
  m->compute = 1;
  m->stream_id = 0;
  m->task_id = 0;
  m->payload.resize(kTokenBytes);
  for (uint64_t i = 0; i < kTokenBytes; ++i) {
    m->payload[i] = uint8_t((user * 13 + i * 7) & 0xFFu);
  }
  return m;
}

RouteEntry EnterCore() {
  RouteEntry e;
  e.flow_dir = 0;
  e.path_core_bypass = false;
  e.operation = Operation::kForward;
  return e;
}

RouteEntry PassTo(uint64_t flow) {
  RouteEntry e;
  e.flow_dir = flow;
  e.path_core_bypass = true;
  e.operation = Operation::kForward;
  return e;
}

// 一个 core 上的两步链：收进来搬进 Core Mem，再按 out_path 发出去。
void WriteRelayChain(Core& core, uint64_t in_path, uint64_t out_path) {
  TaskEntry in;
  in.send_unit = SendUnit::kDte;
  in.recv_unit = RecvUnit::kDsa;
  in.dsa_en = true;
  in.exe_mask = true;
  in.path_id = in_path;
  in.task_pc = SymbolOf("task_dte_user_init");
  core.GetTs().Cfg().WriteTask(0, in);

  TaskEntry out;
  out.send_unit = SendUnit::kDte;
  out.recv_unit = RecvUnit::kDsa;
  out.dsa_en = true;
  out.end = true;
  out.exe_mask = true;
  out.path_id = out_path;
  out.task_pc = SymbolOf("task_dte_move");
  core.GetTs().Cfg().WriteTask(1, out);

  core.GetTs().Cfg().WritePathMap(in_path, 0);
  core.GetTs().Cfg().SetInitFinish();
  core.GetDte().Tables().PreloadPathTask(in_path, 0);
}

// 中间列 chip 是 2×4：W 口挂在 core4（行 1 左端）的 left，E 口挂在 core3
// （行 0 右端）的 right。从 W 走到 E 的一条路是
//   core4 →right→ core5 →right→ core6 →right→ core7 →mid→ core3 →right→ E
// 落地那一颗 core 是 core4，其余四颗只转发。
constexpr uint64_t kEnterCore = 4;
constexpr uint64_t kRelayCore[4] = {5, 6, 7, 3};
constexpr uint64_t kRelayFlow[4] = {kFlowRight, kFlowRight, kFlowMid,
                                    kFlowRight};

// 把 out_path 这一条从落地 core 一直铺到 E 口。落地 core 自己也要一项：DTE 发
// 出的包查的就是它。
void WireExitPath(Chip& chip, uint64_t out_path) {
  chip.GetCore(kEnterCore).GetRouter().Preload(out_path, PassTo(kFlowRight));
  for (uint64_t i = 0; i < 4; ++i) {
    chip.GetCore(kRelayCore[i])
        .GetRouter()
        .Preload(out_path, PassTo(kRelayFlow[i]));
  }
}

// 一颗 chip 上的一段：从 W 口进来落在 core4，算完从 E 口出去。
void SetUpChip(Chip& chip, uint64_t in_path, uint64_t out_path) {
  Core& landing = chip.GetCore(kEnterCore);
  landing.GetRouter().Preload(in_path, EnterCore());
  landing.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
  WriteRelayChain(landing, in_path, out_path);
  WireExitPath(chip, out_path);
}

// 注入与收取各用一座桥当接头：往它的 core 侧灌整包，拆段的活由 TX 做，与真
// chip 之间的对接走的是同一条通路。这两段不是实际存在的链路，延迟填 0。
C2cCfg StubCfg() {
  C2cCfg c;
  c.axi_latency = 0;
  return c;
}

// 两颗 chip、两座接头桥与它们之间的段搬运，全由这一个协程驱动。出口上的 flit
// 是单拍脉冲，分到两个协程里谁先跑不定，会整拍错过。
class ChipPairHarness : public BachModule {
 public:
  ChipPairHarness(ClockPtr c, Chip& first, Chip& second, C2cBridge& feed,
                  C2cBridge& sink, uint64_t at, MessagePtr m)
      : BachModule(c, "harness"), a(first), b(second), in_stub(feed),
        out_stub(sink), fire_at(at), msg(std::move(m)) {}

  std::vector<MessagePtr> out_msgs;
  // 两颗 chip 上落地那个 core 的表都走空了才算这一笔真的走完。
  uint64_t head_a = 0, head_b = 0;
  uint64_t done_at = 0;

 protected:
  void Step() override {
    head_a = a.GetCore(kEnterCore).GetTs().Table().HeadPtr();
    head_b = b.GetCore(kEnterCore).GetTs().Table().HeadPtr();

    LinkEndPtr feed_in = in_stub.FromCore();
    if (CycleNow() == fire_at && msg) {
      feed_in->flit.Drive(/*vc=*/0, /*is_head=*/true, /*is_tail=*/true,
                          msg->size, msg);
    } else {
      feed_in->flit.Idle();
    }
    feed_in->release.Idle();
    out_stub.FromCore()->flit.Idle();
    out_stub.FromCore()->release.Idle();

    // 末级先做：收取那一头在最外。
    out_stub.RunStep();
    b.RunOutside();
    a.RunOutside();
    in_stub.RunStep();

    // 段的搬运。三段对接各两个方向：反方向走的是 credit 与 release。
    Move(in_stub, a.Port(kChipW));
    Move(a.Port(kChipE), b.Port(kChipW));
    Move(b.Port(kChipE), out_stub);

    FlitView f = ReadFlit(out_stub.ToCore()->flit);
    if (f.valid && f.msg) {
      if (out_msgs.empty()) done_at = CycleNow();
      out_msgs.push_back(f.msg);
    }
  }

 private:
  static void Move(C2cBridge& x, C2cBridge& y) {
    while (x.HasOut()) y.PushIn(x.TakeOut());
    while (y.HasOut()) x.PushIn(y.TakeOut());
  }

  Chip& a;
  Chip& b;
  C2cBridge& in_stub;
  C2cBridge& out_stub;
  uint64_t fire_at;
  MessagePtr msg;
};

}  // namespace

// 一个 token 走过两颗 chip：进第一颗落在 core4 搬进 Core Mem，再经四颗只转发
// 的 core 从 E 口出去，过 C2C 进第二颗的 W 口，在那边走一遍同样的路，从第二颗
// 的 E 口出来。收到的字节与注入的相等。
TEST(BachChipE2e, TokenCrossesTwoChips) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  MessagePtr token = MakeToken(3, 42);
  std::vector<MessagePtr> got;
  uint64_t head_a = 0, head_b = 0, done_at = 0, crossed = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    ChipCfg ca, cb;
    ca.shape = ChipShape::kMiddle;
    ca.gx = 1;
    ca.gy = 0;
    cb.shape = ChipShape::kMiddle;
    cb.gx = 2;
    cb.gy = 0;
    Chip a(clk, "chip_a", ca);
    Chip b(clk, "chip_b", cb);
    C2cBridge feed(clk, "feed", StubCfg());
    C2cBridge sink(clk, "sink", StubCfg());

    // path 3 进第一颗，出来走 path 4；path 4 进第二颗，出来走 path 5。
    SetUpChip(a, /*in_path=*/3, /*out_path=*/4);
    SetUpChip(b, /*in_path=*/4, /*out_path=*/5);

    ChipPairHarness harness(clk, a, b, feed, sink, /*at=*/2, token);
    clk->Continue(1500 * kPeriod);
    RT::JoinAll();
    got = harness.out_msgs;
    head_a = harness.head_a;
    head_b = harness.head_b;
    done_at = harness.done_at;
    // 第一颗 chip 的 E 口上发出去的段数。这一路走的确实是 C2C。
    crossed = a.Port(kChipE).Tx().Sent();
  }
  RT::Reset();

  EXPECT_GT(crossed, 0u) << "第一颗 chip 的 E 口上一段都没发出去";
  EXPECT_GE(done_at, kC2cAxiLatency) << "这一段 C2C 的延迟要记上";
  EXPECT_EQ(head_a, 1u) << "第一颗 chip 上这个用户没退休";
  EXPECT_EQ(head_b, 1u) << "第二颗 chip 上这个用户没退休";
  ASSERT_FALSE(got.empty()) << "第二颗 chip 的出口上一个包都没有";
  MessagePtr sent = got.front();
  EXPECT_EQ(sent->user_id, token->user_id);
  EXPECT_EQ(sent->gpu_id, token->gpu_id) << "DPU 那一对包头没跟着出核";
  EXPECT_EQ(sent->token_id, token->token_id) << "DPU 那一对包头没跟着出核";
  ASSERT_EQ(sent->payload.size(), kTokenBytes);
  for (uint64_t i = 0; i < kTokenBytes; ++i) {
    ASSERT_EQ(sent->payload[i], token->payload[i]) << "第 " << i << " 个字节";
  }
}
