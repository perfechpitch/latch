// R core：一行一行往下累加的那一层归约。
//
// 一个用户的两笔从两个方向来：本行结果与上一行 R core 送来的累加结果，在本 core
// 上等齐了再相加送下一行。两条链：
//
//   链一  由 Router 触发。落点与 valid 标志都由硬件按包头办，datain 那一段只
//         把这个槽是哪个用户记进 Share Mem
//   链二  自启动。MU 扫标志表找齐了的槽 → DTE 把整槽从 Matrix Mem 搬到 Core
//         Mem → VU 两笔求和 → DTE 把结果送下一行
//
// 一半是一包：16 B 软件辅助信息后面接 6144 个 BF16。这一份验的是两条链接起来
// 之后走不走得通、加出来的数对不对。用户之间乱序到达那一档由“三个用户交叉着来”
// 那个用例覆盖。

#include <gtest/gtest.h>

#include <initializer_list>

#include <array>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/common/numeric/formats.h"
#include "bach/ip/chip/core/core.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

std::string KernelDir() {
  return std::string(LATCH_SOURCE_DIR) + "/src/bach/compiler/kernel/build/";
}

uint64_t SymbolOf(std::string const& name, std::string const& kind) {
  std::ifstream f(KernelDir() + "kernel_" + kind + ".sym");
  std::string addr, type, sym;
  while (f >> addr >> type >> sym) {
    if (sym == name) return std::stoull(addr, nullptr, 16);
  }
  return 0;
}

bool KernelBuilt() {
  std::ifstream f(KernelDir() + "kernel_mu.hex");
  return f.good();
}

// ── 摆放 ──
//
// 与 compiler/kernel/bach.h 的 RC_* 同源，改一处要一起改。
constexpr uint64_t kSlots = 16;
// 一笔是一包，包首 16 B 是软件辅助信息，后面是 kOutN 个 BF16；一半装一笔。
constexpr uint64_t kOutN = 6144;
constexpr uint64_t kPacketBytes = kReduceSwHeaderBytes + kOutN * 2;
constexpr uint64_t kHalfBytes = 0x3080;
constexpr uint64_t kSlotBytes = 2 * kHalfBytes;
constexpr uint64_t kMmBase = 0x000000;
constexpr uint64_t kFlagOff = 0x0000;
constexpr uint64_t kSumOff = 0x0000;

constexpr uint64_t kInPath = 3;
constexpr uint64_t kOutPath = 0;

// 一个用户在 R core 上的落点。half 为 0 是本行结果，为 1 是上一行送来的累加结果。
uint64_t Land(uint64_t user, uint64_t half) {
  return kMmBase + (user % kSlots) * kSlotBytes + half * kHalfBytes;
}

// 一笔的 payload：16 B 头，后面是 kOutN 个 BF16。
std::vector<uint8_t> HalfPayload(std::vector<float> const& v) {
  std::vector<uint8_t> b(kPacketBytes, 0);
  for (uint64_t j = 0; j < kOutN; ++j) {
    uint16_t bits = numeric::ToBf16(v[j]);
    b[kReduceSwHeaderBytes + j * 2] = uint8_t(bits & 0xFFu);
    b[kReduceSwHeaderBytes + j * 2 + 1] = uint8_t(bits >> 8);
  }
  return b;
}

// 连着的一段字节按 BF16 读出来。
std::vector<float> FloatsOf(std::vector<uint8_t> const& b) {
  std::vector<float> v;
  for (uint64_t j = 0; j + 2 <= b.size(); j += 2) {
    v.push_back(numeric::FromBf16(uint16_t(b[j] | (b[j + 1] << 8))));
  }
  return v;
}

// 一个数按 BF16 存下再读回来。
float Bf16Of(float v) { return numeric::FromBf16(numeric::ToBf16(v)); }

// 两笔按 BF16 读出来、按 FP32 相加、再按 BF16 写回，就是 R core 该算出来的那一份。
float SumOf(float a, float b) { return Bf16Of(Bf16Of(a) + Bf16Of(b)); }

// 一笔，落那一半的起点。
MessagePtr MakeHalf(uint64_t user, uint64_t half, std::vector<float> const& v) {
  auto m = std::make_shared<Message>();
  m->path_id = kInPath;
  m->user_id = user;
  m->gpu_id = 2;
  m->token_id = user;
  m->stream_id = 0;
  m->task_id = 0;
  m->dst_addr = Land(user, half);
  m->payload = HalfPayload(v);
  m->size = m->payload.size();
  return m;
}

// R core 的两条链。链二从 task 0 起，链一是单独配的 datain_task。
void WriteRcoreChains(Core& core) {
  TaskEntry find;
  find.send_unit = SendUnit::kMu;
  find.recv_unit = RecvUnit::kRvOnly;
  find.task_pc = SymbolOf("task_rc_find", "mu");
  core.GetTs().Cfg().WriteTask(0, find);

  TaskEntry load;
  load.send_unit = SendUnit::kDte;
  load.recv_unit = RecvUnit::kDsa;
  load.task_pc = SymbolOf("task_dte_rc_load", "dte");
  core.GetTs().Cfg().WriteTask(1, load);

  TaskEntry add;
  add.send_unit = SendUnit::kVu;
  add.recv_unit = RecvUnit::kRvOnly;
  add.task_pc = SymbolOf("task_vu_add", "vu");
  core.GetTs().Cfg().WriteTask(2, add);

  TaskEntry send;
  send.send_unit = SendUnit::kDte;
  send.recv_unit = RecvUnit::kDsa;
  send.end = true;
  send.path_id = kOutPath;
  send.task_pc = SymbolOf("task_dte_rc_send", "dte");
  core.GetTs().Cfg().WriteTask(3, send);
  core.GetTs().Cfg().WriteRouterTable(kOutPath, 0, kOutPath % kVcNum);

  // 链一：只有一个 datain 任务，手动配，不进 task_chain。
  core.GetTs().Cfg().WriteDatainTask(SymbolOf("task_dte_rc_datain", "dte"),
                                     /*weights_mode=*/false);
  core.GetTs().Cfg().SetSelfStart(true);
  // B core 与 R core 的 stream_num 配 16，自启动数因此也是 16（ts.md F72）。
  core.GetTs().Cfg().SetStreamNum(kStreamNum);
  core.GetTs().InitFinish();
}

RouteEntry EnterCore() {
  RouteEntry e;
  e.flow_dir = 0;
  e.path_core_bypass = false;
  e.operation = Operation::kForward;
  return e;
}

RouteEntry LeaveCore() {
  RouteEntry e;
  e.flow_dir = kFlowMid;
  e.path_core_bypass = true;
  e.operation = Operation::kForward;
  return e;
}

// core 里上百个模块与外面那两根线都由这一个协程驱动。
class RcoreHarness : public BachModule {
 public:
  struct Job {
    uint64_t at = 0;
    MessagePtr msg;
  };

  RcoreHarness(ClockPtr c, Core& target, std::vector<Job> list)
      : BachModule(c, "harness"), core(target), jobs(std::move(list)) {}

  std::vector<MessagePtr> out_msgs;
  std::array<bool, 3> took{};
  std::array<uint64_t, 3> took_vc{};

 protected:
  void Step() override {
    LinkEndPtr in = core.InWire(1);
    bool drove = false;
    for (auto const& j : jobs) {
      if (j.at != CycleNow()) continue;
      in->flit.Drive(0, true, true, j.msg->size, j.msg);
      drove = true;
      break;
    }
    if (!drove) in->flit.Idle();
    // 出核那一路的下游：上一拍收下的 flit，这一拍把 VC 位置还回去。不还的话
    // core 那一侧的 credit 只减不加，一个多 flit 的包发满额度就再也发不动。
    for (uint64_t d = 0; d < 3; ++d) {
      core.BackWire(d)->flit.Idle();
      if (took[d]) {
        core.BackWire(d)->release.Drive(true, took_vc[d], false, 0);
        took[d] = false;
      } else {
        core.BackWire(d)->release.Idle();
      }
    }

    core.RunStep();

    for (uint64_t d = 0; d < 3; ++d) {
      FlitView f = ReadFlit(core.OutWire(d)->flit);
      if (!f.valid) continue;
      took[d] = true;
      took_vc[d] = f.vc;
      // 一个包只记一条：多 flit 的包在尾 flit 那一拍才算收完。
      if (f.tail && f.msg) out_msgs.push_back(f.msg);
    }
  }

 private:
  Core& core;
  std::vector<Job> jobs;
};

// 一笔，第 at 拍注进去。
std::vector<RcoreHarness::Job> Parts(uint64_t at, uint64_t user, uint64_t half,
                                     std::vector<float> const& v) {
  return {{at, MakeHalf(user, half, v)}};
}

// 几笔的包并成一张注入表。
std::vector<RcoreHarness::Job> Jobs(
    std::initializer_list<std::vector<RcoreHarness::Job>> parts) {
  std::vector<RcoreHarness::Job> all;
  for (auto const& p : parts) all.insert(all.end(), p.begin(), p.end());
  return all;
}

// R core 的 Core Mem 里求和那一段：跳过开头的 16 B 头。
std::vector<uint8_t> GatherSum(Core& core) {
  return core.Cmem().Peek(kSumOff + kReduceSwHeaderBytes, kOutN * 2);
}

// R core 在业务模式下进核那一笔：落 Matrix Mem、不回 Ack，搬完置到齐标志。
InboundCfg RcoreInbound() {
  InboundCfg in;
  in.route = Route::kRouterToMm;
  in.no_ack = true;
  in.flag_base = kFlagOff;
  // 到齐标志每半一项：硬件按落点除以这个格子大小找项。
  in.flag_entry_bytes = kHalfBytes;
  return in;
}

void LoadKernels(Core& core) {
  core.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
  core.Rv(1).LoadImage(KernelDir() + "kernel_mu.hex");
  core.Rv(2).LoadImage(KernelDir() + "kernel_vu.hex");
}

std::vector<float> Ramp(float base, float step) {
  std::vector<float> v;
  for (uint64_t j = 0; j < kOutN; ++j) v.push_back(base + step * float(j));
  return v;
}

}  // namespace

// 起一个 R core：装 kernel、配路由与两条链。
#define RCORE_SETUP()                                                   \
  EnsureSlots();                                                        \
  ClockPtr clk = MakeClock(0, kPeriod);                                 \
  Core core(clk, "rcore", CoreContext{});                               \
  core.SetBusinessInboundCfg(RcoreInbound());                           \
  LoadKernels(core);                                                    \
  core.GetRouter().Preload(kInPath, EnterCore());                       \
  core.GetRouter().Preload(kOutPath, LeaveCore());                      \
  core.GetDte().Tables().PreloadRtab(kOutPath, LeaveCore());            \
  WriteRcoreChains(core)

// 一个用户的两笔到齐之后相加送出去：两半逐元素相加，送出去的还是一包，落点按同
// 一条规则算出来的是下一行 R core 那个槽的后一半。
TEST(BachRcore, TwoPartsAreSummedAndSentOn) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  constexpr uint64_t kUser = 5;
  std::vector<float> a = Ramp(1.0f, 1.0f);
  std::vector<float> b = Ramp(1000.0f, 16.0f);

  std::vector<MessagePtr> got;
  std::vector<uint8_t> landed;
  {
    RCORE_SETUP();
    RcoreHarness h(clk, core,
                   Jobs({Parts(2, kUser, 0, a), Parts(200, kUser, 1, b)}));
    clk->Continue(20000 * kPeriod);
    RT::JoinAll();
    got = h.out_msgs;
    landed = GatherSum(core);
  }
  RT::Reset();

  // 先看留在 Core Mem 里那一份：出核那一步之前算完的就是它。
  std::vector<float> sum = FloatsOf(landed);
  ASSERT_EQ(sum.size(), kOutN);
  for (uint64_t j = 0; j < kOutN; ++j) {
    ASSERT_EQ(sum[j], SumOf(a[j], b[j])) << "Core Mem 里第 " << j << " 个";
  }
  ASSERT_EQ(got.size(), 1u) << "送出去的是一包";
  ASSERT_EQ(got[0]->payload.size(), kPacketBytes);
  EXPECT_EQ(got[0]->dst_addr, Land(kUser, 1)) << "送下一行时落它的后一半";
  std::vector<uint8_t> sent_bytes(
      got[0]->payload.begin() + kReduceSwHeaderBytes, got[0]->payload.end());
  std::vector<float> sent = FloatsOf(sent_bytes);
  ASSERT_EQ(sent.size(), kOutN);
  for (uint64_t j = 0; j < kOutN; ++j) {
    ASSERT_EQ(sent[j], SumOf(a[j], b[j])) << "送出去那一份第 " << j << " 个";
  }
}

// 用户之间乱序：谁先集齐谁先走，不按到达顺序。三个用户交叉着来，第二个先集
// 齐，它就先算完出去。三个都要走完。
TEST(BachRcore, WhoeverIsCompleteFirstGoesFirst) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  std::vector<uint64_t> order;
  uint64_t total = 0;
  {
    RCORE_SETUP();
    RcoreHarness h(clk, core,
                   Jobs({Parts(2, 5, 0, Ramp(1.0f, 1.0f)),
                         Parts(200, 6, 0, Ramp(2.0f, 1.0f)),
                         Parts(400, 7, 0, Ramp(3.0f, 1.0f)),
                         Parts(600, 6, 1, Ramp(200.0f, 1.0f)),
                         Parts(2000, 7, 1, Ramp(300.0f, 1.0f)),
                         Parts(4000, 5, 1, Ramp(100.0f, 1.0f))}));
    clk->Continue(40000 * kPeriod);
    RT::JoinAll();
    total = h.out_msgs.size();
    for (MessagePtr const& m : h.out_msgs) {
      // 一个用户送出去的那一包落在那一槽后一半的起点。
      if (m->dst_addr % kSlotBytes == kHalfBytes) {
        order.push_back(m->dst_addr / kSlotBytes);
      }
    }
  }
  RT::Reset();
  EXPECT_EQ(total, 3u) << "三个用户的包都要走完";
  ASSERT_EQ(order.size(), 3u) << "三个用户都要走完";
  EXPECT_EQ(order[0], 6u);
  EXPECT_EQ(order[1], 7u);
  EXPECT_EQ(order[2], 5u);
}

// 只到一笔就不动：链二扫不到两半都齐的槽，一直等。
TEST(BachRcore, OnePartAloneWaits) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  std::vector<MessagePtr> got;
  {
    RCORE_SETUP();
    RcoreHarness h(clk, core, Jobs({Parts(2, 5, 0, Ramp(1.0f, 1.0f))}));
    clk->Continue(20000 * kPeriod);
    RT::JoinAll();
    got = h.out_msgs;
  }
  RT::Reset();
  EXPECT_TRUE(got.empty()) << "只到一笔不该往下走";
}
