// B core：组内广播的发起点。
//
// 上游把 token 一笔笔送进来，B core 留一份在自己的 Matrix Mem，再广播给本组
// 各 core。两条链：
//
//   链一  由 Router 触发。落点与 valid 标志都由硬件按包头办，datain 那一段只
//         把这一格是哪个用户记进 Share Mem
//   链二  自启动。VU 查 tail 那一格的 valid，置起来了就把 tail 推一格；head 与
//         tail 不相等就说明有还没发的，DTE 把 head 那一笔从 Matrix Mem 广播
//         出去，发完清那一格的 valid 并把 head 推一格
//
// 这一份验的是两条链接起来之后走不走得通：一笔都没有时链二在 task 0 上等着，
// 连着几笔时按进来的次序发出去，发出去的那一份与送进来的逐字节相同。

#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
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
  std::ifstream f(KernelDir() + "kernel_vu.hex");
  return f.good();
}

// ── 摆放 ──
//
// 与 compiler/kernel/bach.h 的 BC_* 同源，改一处要一起改。
constexpr uint64_t kSlots = 16;
constexpr uint64_t kTokenBytes = 6144 * 2;
constexpr uint64_t kMmBase = 0x000000;
constexpr uint64_t kFlagOff = 0x0500;

constexpr uint64_t kInPath = 3;
constexpr uint64_t kOutPath = 0;

// 第几笔落在 Matrix Mem 的哪里。发方按自己送出的笔数算，B core 那边按收下的
// 笔数算，同一条规则。
uint64_t Land(uint64_t seq) {
  return kMmBase + (seq % kSlots) * kTokenBytes;
}

// 一笔 token 的内容：认得出是哪一笔就行，B core 不算它，只搬。
std::vector<uint8_t> TokenBytes(uint64_t seed) {
  std::vector<uint8_t> b(kTokenBytes, 0);
  for (uint64_t j = 0; j < kTokenBytes; ++j) {
    b[j] = uint8_t((seed * 131 + j * 7 + (j >> 8)) & 0xFFu);
  }
  return b;
}

MessagePtr MakeToken(uint64_t seq, uint64_t user,
                     std::vector<uint8_t> const& body) {
  auto m = std::make_shared<Message>();
  m->path_id = kInPath;
  m->user_id = user;
  m->gpu_id = 2;
  m->token_id = user;
  m->compute = 1;
  m->stream_id = 0;
  m->task_id = 0;
  m->dst_addr = Land(seq);
  m->payload = body;
  m->size = m->payload.size();
  return m;
}

// B core 的两条链。链二从 task 0 起，链一是单独配的 datain_task。
void WriteBcoreChains(Core& core) {
  TaskEntry wait;
  wait.send_unit = SendUnit::kVu;
  wait.recv_unit = RecvUnit::kRvOnly;
  wait.self_start = true;
  wait.exe_mask = true;
  wait.task_pc = SymbolOf("task_bc_wait", "vu");
  core.GetTs().Cfg().WriteTask(0, wait);

  TaskEntry send;
  send.send_unit = SendUnit::kDte;
  send.recv_unit = RecvUnit::kDsa;
  send.dsa_en = true;
  send.end = true;
  send.exe_mask = true;
  send.path_id = kOutPath;
  send.credit_en = true;
  send.task_pc = SymbolOf("task_dte_bc_send", "dte");
  core.GetTs().Cfg().WriteTask(1, send);

  // 链一：只有一个 datain 任务，手动配，不进 task_chain。
  core.GetTs().Cfg().WriteDatainTask(SymbolOf("task_dte_bc_datain", "dte"),
                                     /*weights_mode=*/false);
  core.GetTs().Cfg().SetCoreType(CoreType::kBroadcast);
  // 广播只往中间那一路发，TS 下发搬出之前查的就是这个方向的下游资源。
  core.GetTs().Cfg().SetBCoreDirection(kFlowMid);
  core.GetTs().Cfg().SetStreamNum(kStreamNum);
  core.GetTs().Cfg().SetInitFinish();
  core.GetTs().SelfStart();
}

RouteEntry EnterCore() {
  RouteEntry e;
  e.flow_dir = 0;
  e.path_core_bypass = false;
  e.stream_table_enable = false;
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
class BcoreHarness : public BachModule {
 public:
  struct Job {
    uint64_t at = 0;
    MessagePtr msg;
  };

  BcoreHarness(ClockPtr c, Core& target, std::vector<Job> list)
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
        core.BackWire(d)->release.Drive(true, took_vc[d], false, 0, false, 0);
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

CoreContext BcoreContext() {
  CoreContext ctx;
  ctx.role = CoreRole::kBroadcast;
  ctx.inbound_flag_base = kFlagOff;
  ctx.inbound_entry_bytes = kTokenBytes;
  return ctx;
}

void LoadKernels(Core& core) {
  core.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
  core.Rv(1).LoadImage(KernelDir() + "kernel_mu.hex");
  core.Rv(2).LoadImage(KernelDir() + "kernel_vu.hex");
}

}  // namespace

// 一笔都没进来：head 与 tail 一直相等，链二在 task 0 上等着，出口是空的。
TEST(BachBcore, NothingGoesOutWithoutAToken) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  std::vector<MessagePtr> got;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreContext ctx = BcoreContext();
    Core core(clk, "bcore", ctx);
    LoadKernels(core);
    core.GetRouter().Preload(kOutPath, LeaveCore());
    core.GetDte().Tables().PreloadRtab(kOutPath, LeaveCore());
    WriteBcoreChains(core);

    BcoreHarness h(clk, core, {});
    clk->Continue(20000 * kPeriod);
    RT::JoinAll();
    got = h.out_msgs;
  }
  RT::Reset();

  EXPECT_TRUE(got.empty()) << "没有数据也发了 " << got.size() << " 个包";
}

// 连着三笔：按进来的次序发出去。三笔的 user 各不相同也不连号，落点只看第几笔。
TEST(BachBcore, TokensGoOutInArrivalOrder) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  std::vector<std::vector<uint8_t>> bodies;
  for (uint64_t u = 0; u < 3; ++u) bodies.push_back(TokenBytes(u + 1));

  std::vector<MessagePtr> got;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreContext ctx = BcoreContext();
    Core core(clk, "bcore", ctx);
    LoadKernels(core);
    core.GetRouter().Preload(kInPath, EnterCore());
    core.GetRouter().Preload(kOutPath, LeaveCore());
    core.GetDte().Tables().PreloadRtab(kOutPath, LeaveCore());
    WriteBcoreChains(core);

    BcoreHarness h(clk, core,
                   {{2, MakeToken(0, 41, bodies[0])},
                    {3000, MakeToken(1, 77, bodies[1])},
                    {6000, MakeToken(2, 5, bodies[2])}});
    clk->Continue(40000 * kPeriod);
    RT::JoinAll();
    got = h.out_msgs;
  }
  RT::Reset();

  ASSERT_EQ(got.size(), 3u) << "三笔都要发出去";
  for (uint64_t i = 0; i < 3; ++i) {
    EXPECT_EQ(got[i]->payload, bodies[i]) << "第 " << i << " 笔";
  }
}
