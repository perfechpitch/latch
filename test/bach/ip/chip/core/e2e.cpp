// 步 11 · core 层端到端：一个 token 从 Router 进来，走完 TS 建 stream、下发
// task、RV core 跑 kernel 配 DSA、DSA 搬运并报完成、TS 退休这一整条链。
//
// 与各单元自己的测试的区别：那些拿桩喂自己那一段，这一份只在 core 外面接桩 ——
// 从 Router 的一个入方向灌包，从出方向收包，core 里的每一段都是真模块。

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <map>
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
constexpr uint64_t kLeft = 1;

void EnsureSlots() { RT::Reset(8, 8); }

std::string KernelDir() {
  return std::string(LATCH_SOURCE_DIR) + "/src/bach/compiler/kernel/build/";
}

// 从 nm 导出的符号表里取一个 task 的入口地址，task_chain 的 TASK_PC 填的就是它。
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

// 一个 token。payload 逐字节可辨，落到 Core Mem 后按它比对。
//
// 512 B 是一个 K=256 的 BF16 token，与 kernel 里的 TOKEN_BYTES 同一个数：出核
// 那几笔搬运按它配 data_len，进来的与出去的长度对不上就不是同一份数据。
MessagePtr MakeToken(uint64_t path, uint64_t user, uint64_t bytes = 512) {
  auto m = std::make_shared<Message>();
  m->path_id = path;
  m->user_id = user;
  m->size = bytes;
  m->compute = 1;
  // 包头带这个用户在本 core 上的槽位与这一笔是链上的第几步。TS 那边环形分配
  // 出来的第一个槽位也是 0。
  m->stream_id = 0;
  m->task_id = 0;
  m->payload.resize(bytes);
  for (uint64_t i = 0; i < bytes; ++i) {
    m->payload[i] = uint8_t((user * 7 + i * 3) & 0xFFu);
  }
  return m;
}

RouteEntry EnterCore() {
  RouteEntry e;
  e.flow_dir = 0;
  e.path_core_bypass = false;
  e.stream_table_enable = false;
  e.operation = Operation::kForward;
  return e;
}

// core 里上百个模块全由这一个协程驱动，外面那两根线也在这里驱动与读取。
//
// 灌包、驱动、读出口放在同一个协程里，是因为出口上的 flit 是单拍脉冲：分成两个
// 协程的话谁先跑不定，读的那一侧可能整拍都错过。
class CoreHarness : public BachModule {
 public:
  CoreHarness(ClockPtr c, Core& target, uint64_t at, MessagePtr m)
      : BachModule(c, "harness"), core(target), fire_at(at),
        msg(std::move(m)) {}

  uint64_t out_flits = 0;
  std::vector<MessagePtr> out_msgs;

 protected:
  void Step() override {
    LinkEndPtr in = core.InWire(kLeft);
    if (CycleNow() == fire_at && msg) {
      in->flit.Drive(0, true, true, msg->size, msg);
    } else {
      in->flit.Idle();
    }
    // 只驱动上游那两根：数据进来的 in_wire 与下游还 credit 的 back_wire。
    // out_wire、up_back_wire、up_release_wire 是 core 自己写的，外面只读。
    for (uint64_t d = 0; d < 3; ++d) core.BackWire(d)->flit.Idle();

    core.RunStep();

    for (uint64_t d = 0; d < 3; ++d) {
      FlitView f = ReadFlit(core.OutWire(d)->flit);
      if (!f.valid) continue;
      ++out_flits;
      if (f.msg) out_msgs.push_back(f.msg);
    }
  }

 private:
  Core& core;
  uint64_t fire_at;
  MessagePtr msg;
};

// 一路上的关键量在协程里抄下来，用例末尾取。那几个都是 Logic64 或模块内部的
// 计数，主线程直接读会读到 t=0 的值。
class CoreProbe : public BachModule {
 public:
  CoreProbe(ClockPtr c, Core& target) : BachModule(c, "probe"), core(target) {}

  uint64_t triggers = 0, cs_used = 0, streams_valid = 0;
  uint64_t rv_insts = 0, dte_cmds = 0, dsa_writes = 0;
  uint64_t dte_trigs = 0, dte_regwrites = 0, dte_admits = 0, dte_dones = 0;
  uint64_t mu_dones = 0, vu_dones = 0;
  uint64_t cmem_grants = 0, head = 0, tail = 0, out_sent = 0;

 protected:
  void Step() override {
    triggers = core.GetRouter().GetCoreStation().Triggers();
    cs_used = core.GetRouter().GetCoreStation().StreamUsed();
    rv_insts = core.Rv(0).Exec().Insts();
    head = core.GetTs().Table().HeadPtr();
    tail = core.GetTs().Table().TailPtr();
    uint64_t v = 0;
    for (uint64_t i = 0; i < kStreamNum; ++i) {
      if (core.GetTs().Table().Peek(i).valid) ++v;
    }
    streams_valid = v;
    // 一次握手最少两拍，同一笔会连着两拍出现在端口上，按「valid 从低变高」认它。
    Count(core.GetTs().DteCmd().Valid(), &cmd_hold, &dte_cmds);
    Count(core.Rv(0).DsaCfg().Valid(), &cfg_hold, &dsa_writes);
    Count(core.GetTs().DsaDone(0).Valid(), &done_hold, &dte_dones);
    Count(core.GetTs().DsaDone(1).Valid(), &mu_hold, &mu_dones);
    Count(core.GetTs().DsaDone(2).Valid(), &vu_hold, &vu_dones);
    dte_trigs = core.GetDte().Regfile().Triggers();
    dte_regwrites = core.GetDte().Regfile().Writes();
    dte_admits = core.GetDte().Committer().Admitted();
    cmem_grants = core.Cmem().Granted();
    out_sent = core.GetDte().OutArb().Sent();
  }

 private:
  static void Count(bool valid, bool* hold, uint64_t* cnt) {
    if (valid && !*hold) {
      ++*cnt;
      *hold = true;
    } else if (!valid) {
      *hold = false;
    }
  }

  Core& core;
  bool cmd_hold = false, cfg_hold = false, done_hold = false;
  bool mu_hold = false, vu_hold = false;
};

// 一条 datain 链：Router 送来的包由 DTE 自己搬进 Core Mem，RV core 那一笔只
// 处理软件包头。
void WriteDatainChain(Core& core) {
  TaskEntry t;
  t.send_unit = SendUnit::kDte;
  t.recv_unit = RecvUnit::kDsa;
  t.dsa_en = true;
  t.end = true;
  t.exe_mask = true;
  t.path_id = 3;
  t.task_pc = SymbolOf("task_dte_user_init");
  core.GetTs().Cfg().WriteTask(0, t);
  core.GetTs().Cfg().WritePathMap(3, 0);
  core.GetTs().Cfg().SetInitFinish();
  core.GetDte().Tables().PreloadPathTask(3, 0);
}

}  // namespace

// 步 11 的判据（单 core 那一半）：一个 token 进来，走完整条链并退休。
TEST(BachCoreE2e, OneTokenWalksTheWholeCore) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  uint64_t triggers = 0, cmds = 0, insts = 0, regw = 0, trigs = 0;
  uint64_t admits = 0, dones = 0, grants = 0, head = 0, tail = 0;
  uint64_t streams = 0, cs_used = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreContext ctx;
    Core core(clk, "core", ctx);
    core.GetRouter().Preload(3, EnterCore());
    core.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
    WriteDatainChain(core);
    ASSERT_GT(SymbolOf("task_dte_user_init"), 0u);

    CoreHarness harness(clk, core, /*at=*/2, MakeToken(3, 42));
    CoreProbe probe(clk, core);
    clk->Continue(400 * kPeriod);
    RT::JoinAll();
    triggers = probe.triggers;
    cmds = probe.dte_cmds;
    insts = probe.rv_insts;
    regw = probe.dte_regwrites;
    trigs = probe.dte_trigs;
    admits = probe.dte_admits;
    dones = probe.dte_dones;
    grants = probe.cmem_grants;
    head = probe.head;
    tail = probe.tail;
    streams = probe.streams_valid;
    cs_used = probe.cs_used;
  }
  RT::Reset();
  EXPECT_EQ(triggers, 1u);   // Router 收下包并通知了 TS
  EXPECT_EQ(cmds, 1u);       // TS 下发了这一笔给 DTE RV core
  EXPECT_GT(insts, 0u);      // RV core 真的跑了 kernel
  EXPECT_GT(regw, 0u);       // kernel 真的写了 DTE 的寄存器
  EXPECT_EQ(trigs, 0u);      // datain 这一笔不再自己配一趟搬运
  EXPECT_EQ(admits, 1u);     // 搬运只起了一笔，由 Header Parser 起
  EXPECT_GT(grants, 0u);     // 数据真的落进了 Core Mem
  EXPECT_EQ(dones, 1u);      // DTE 向 TS 报了一次完成
  EXPECT_EQ(head, tail);     // 表空了：这个用户退休了
  EXPECT_EQ(streams, 0u);
  EXPECT_EQ(cs_used, 0u);    // Router 那一侧的 stream 表项也释放了
}

// 搬进 Core Mem 的字节与注入的 payload 逐字节相等。
TEST(BachCoreE2e, PayloadLandsByteForByte) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  constexpr uint64_t kBytes = 512;
  MessagePtr token = MakeToken(3, 42, kBytes);
  std::vector<uint8_t> landed;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreContext ctx;
    Core core(clk, "core", ctx);
    core.GetRouter().Preload(3, EnterCore());
    core.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
    WriteDatainChain(core);

    CoreHarness harness(clk, core, /*at=*/2, token);
    clk->Continue(400 * kPeriod);
    RT::JoinAll();
    // 进核搬运的落点取自包头的 dst_addr，注入那一包没填，就是 0。
    landed = core.Cmem().Peek(0, kBytes);
  }
  RT::Reset();
  ASSERT_EQ(landed.size(), kBytes);
  for (uint64_t i = 0; i < kBytes; ++i) {
    ASSERT_EQ(landed[i], token->payload[i]) << "第 " << i << " 个字节";
  }
}

// 前一个用户退休之后，下一个用户接着走完整条链。
//
// 两个 token 属于两个用户：Retire 发出后 Router 上不得再出现该用户的包，所以
// 同一个用户不会在退休之后又来一笔。第二个用户拿到的是环形分配的下一个槽位。
TEST(BachCoreE2e, NextUserRunsAfterRetire) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  uint64_t triggers = 0, dones = 0, head = 0, tail = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreContext ctx;
    Core core(clk, "core", ctx);
    core.GetRouter().Preload(3, EnterCore());
    core.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
    WriteDatainChain(core);

    class TwoTokens : public BachModule {
     public:
      TwoTokens(ClockPtr c, Core& t) : BachModule(c, "harness"), core(t) {}

     protected:
      void Step() override {
        uint64_t now = CycleNow();
        LinkEndPtr in = core.InWire(kLeft);
        if (now == 2 || now == 200) {
          MessagePtr m = MakeToken(3, now == 2 ? 42 : 43, 512);
          // 前一个退休后 head 推进一格，下一个用户落在槽位 1。
          m->stream_id = now == 2 ? 0 : 1;
          in->flit.Drive(0, true, true, m->size, m);
        } else {
          in->flit.Idle();
        }
        for (uint64_t d = 0; d < 3; ++d) core.BackWire(d)->flit.Idle();
        core.RunStep();
      }

     private:
      Core& core;
    };
    TwoTokens harness(clk, core);
    CoreProbe probe(clk, core);
    clk->Continue(600 * kPeriod);
    RT::JoinAll();
    triggers = probe.triggers;
    dones = probe.dte_dones;
    head = probe.head;
    tail = probe.tail;
  }
  RT::Reset();
  EXPECT_EQ(triggers, 2u);
  EXPECT_EQ(dones, 2u);
  EXPECT_EQ(head, tail);   // 两笔都退休了
}

namespace {

// 两步链：第一步 datain 把包搬进 Core Mem，第二步把它从 Core Mem 发回 Router。
void WriteTwoStepChain(Core& core) {
  TaskEntry in;
  in.send_unit = SendUnit::kDte;
  in.recv_unit = RecvUnit::kDsa;
  in.dsa_en = true;
  in.exe_mask = true;
  in.path_id = 3;
  in.task_pc = SymbolOf("task_dte_user_init");
  core.GetTs().Cfg().WriteTask(0, in);

  TaskEntry out;
  out.send_unit = SendUnit::kDte;
  out.recv_unit = RecvUnit::kDsa;
  out.dsa_en = true;
  out.end = true;
  out.exe_mask = true;
  out.path_id = 0;
  out.task_pc = SymbolOf("task_dte_move");
  core.GetTs().Cfg().WriteTask(1, out);

  core.GetTs().Cfg().WritePathMap(3, 0);
  core.GetTs().Cfg().SetInitFinish();
  core.GetDte().Tables().PreloadPathTask(3, 0);
}

}  // namespace

// 出核那一半：数据从 Core Mem 发回 Router，出口上收到的包与注入的逐字节相等。
TEST(BachCoreE2e, TokenLeavesTheCore) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  constexpr uint64_t kBytes = 512;
  MessagePtr token = MakeToken(3, 42, kBytes);
  std::vector<MessagePtr> got;
  uint64_t dones = 0, head = 0, tail = 0, sent_beats = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreContext ctx;
    Core core(clk, "core", ctx);
    // path 3 进核；path 0 从本 core 往 mid 方向发出去。
    core.GetRouter().Preload(3, EnterCore());
    RouteEntry leave;
    leave.flow_dir = kFlowMid;
    leave.path_core_bypass = true;
    leave.operation = Operation::kForward;
    core.GetRouter().Preload(0, leave);
    core.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
    WriteTwoStepChain(core);
    ASSERT_GT(SymbolOf("task_dte_move"), 0u);

    CoreHarness harness(clk, core, /*at=*/2, token);
    CoreProbe probe(clk, core);
    clk->Continue(800 * kPeriod);
    RT::JoinAll();
    got = harness.out_msgs;
    dones = probe.dte_dones;
    head = probe.head;
    tail = probe.tail;
    sent_beats = probe.out_sent;
  }
  RT::Reset();
  EXPECT_EQ(sent_beats, 2u); // 512 B 分两拍发，一拍 256 B
  EXPECT_EQ(dones, 2u);      // 两步各报一次完成
  EXPECT_EQ(head, tail);     // 走到链尾退休了
  ASSERT_FALSE(got.empty()) << "出口上一个包都没有";
  MessagePtr sent = got.front();
  EXPECT_EQ(sent->user_id, token->user_id);
  ASSERT_EQ(sent->payload.size(), kBytes);
  for (uint64_t i = 0; i < kBytes; ++i) {
    ASSERT_EQ(sent->payload[i], token->payload[i]) << "第 " << i << " 个字节";
  }
}

namespace {

// 三步链：datain 搬进 Core Mem、MU 算一笔、把结果发回 Router。
void WriteComputeChain(Core& core) {
  TaskEntry in;
  in.send_unit = SendUnit::kDte;
  in.recv_unit = RecvUnit::kDsa;
  in.dsa_en = true;
  in.exe_mask = true;
  in.path_id = 3;
  in.task_pc = SymbolOf("task_dte_user_init");
  core.GetTs().Cfg().WriteTask(0, in);

  TaskEntry mul;
  mul.send_unit = SendUnit::kMu;
  mul.recv_unit = RecvUnit::kDsa;
  mul.dsa_en = true;
  mul.exe_mask = true;
  mul.task_pc = SymbolOf("task_mu_compute", "mu");
  core.GetTs().Cfg().WriteTask(1, mul);

  TaskEntry out;
  out.send_unit = SendUnit::kDte;
  out.recv_unit = RecvUnit::kDsa;
  out.dsa_en = true;
  out.end = true;
  out.exe_mask = true;
  out.path_id = 0;
  out.task_pc = SymbolOf("task_dte_move");
  core.GetTs().Cfg().WriteTask(2, out);

  core.GetTs().Cfg().WritePathMap(3, 0);
  core.GetTs().Cfg().SetInitFinish();
  core.GetDte().Tables().PreloadPathTask(3, 0);
}

}  // namespace

// 三个单元接力：DTE 搬进来、MU 算、DTE 再搬出去，三步各报一次完成后退休。
TEST(BachCoreE2e, ThreeUnitsRelayOneToken) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  uint64_t dte_dones = 0, mu_dones = 0, head = 0, tail = 0, out_beats = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreContext ctx;
    Core core(clk, "core", ctx);
    core.GetRouter().Preload(3, EnterCore());
    RouteEntry leave;
    leave.flow_dir = kFlowMid;
    leave.path_core_bypass = true;
    leave.operation = Operation::kForward;
    core.GetRouter().Preload(0, leave);
    core.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
    core.Rv(1).LoadImage(KernelDir() + "kernel_mu.hex");
    WriteComputeChain(core);
    ASSERT_GT(SymbolOf("task_mu_compute", "mu"), 0u);

    CoreHarness harness(clk, core, /*at=*/2, MakeToken(3, 42));
    CoreProbe probe(clk, core);
    clk->Continue(4000 * kPeriod);
    RT::JoinAll();
    dte_dones = probe.dte_dones;
    mu_dones = probe.mu_dones;
    head = probe.head;
    tail = probe.tail;
    out_beats = harness.out_flits;
  }
  RT::Reset();
  EXPECT_EQ(dte_dones, 2u);   // 搬进来一次、搬出去一次
  EXPECT_EQ(mu_dones, 1u);    // MU 算了一笔
  EXPECT_GT(out_beats, 0u);   // 结果出了 core
  EXPECT_EQ(head, tail);      // 走到链尾退休了
}

namespace {

// 四步链：datain 搬进 Core Mem、MU 算、VU 算、把结果发回 Router。
void WriteFullChain(Core& core) {
  TaskEntry in;
  in.send_unit = SendUnit::kDte;
  in.recv_unit = RecvUnit::kDsa;
  in.dsa_en = true;
  in.exe_mask = true;
  in.path_id = 3;
  in.task_pc = SymbolOf("task_dte_user_init");
  core.GetTs().Cfg().WriteTask(0, in);

  TaskEntry mul;
  mul.send_unit = SendUnit::kMu;
  mul.recv_unit = RecvUnit::kDsa;
  mul.dsa_en = true;
  mul.exe_mask = true;
  mul.task_pc = SymbolOf("task_mu_compute", "mu");
  core.GetTs().Cfg().WriteTask(1, mul);

  TaskEntry vec;
  vec.send_unit = SendUnit::kVu;
  vec.recv_unit = RecvUnit::kDsa;
  vec.dsa_en = true;
  vec.exe_mask = true;
  vec.task_pc = SymbolOf("task_vu_compute", "vu");
  core.GetTs().Cfg().WriteTask(2, vec);

  TaskEntry out;
  out.send_unit = SendUnit::kDte;
  out.recv_unit = RecvUnit::kDsa;
  out.dsa_en = true;
  out.end = true;
  out.exe_mask = true;
  out.path_id = 0;
  out.task_pc = SymbolOf("task_dte_move");
  core.GetTs().Cfg().WriteTask(3, out);

  core.GetTs().Cfg().WritePathMap(3, 0);
  core.GetTs().Cfg().SetInitFinish();
  core.GetDte().Tables().PreloadPathTask(3, 0);
}

}  // namespace

// 一条 FFN 形状的链走完：DTE 搬进来、MU 与 VU 各算一笔、DTE 搬出去，四步各报
// 一次完成后退休。
TEST(BachCoreE2e, FullChainRunsFourSteps) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  uint64_t dte_dones = 0, mu_dones = 0, vu_dones = 0;
  uint64_t head = 0, tail = 0, out_beats = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreContext ctx;
    Core core(clk, "core", ctx);
    core.GetRouter().Preload(3, EnterCore());
    RouteEntry leave;
    leave.flow_dir = kFlowMid;
    leave.path_core_bypass = true;
    leave.operation = Operation::kForward;
    core.GetRouter().Preload(0, leave);
    core.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
    core.Rv(1).LoadImage(KernelDir() + "kernel_mu.hex");
    core.Rv(2).LoadImage(KernelDir() + "kernel_vu.hex");
    WriteFullChain(core);
    ASSERT_GT(SymbolOf("task_vu_compute", "vu"), 0u);

    CoreHarness harness(clk, core, /*at=*/2, MakeToken(3, 42));
    CoreProbe probe(clk, core);
    clk->Continue(8000 * kPeriod);
    RT::JoinAll();
    dte_dones = probe.dte_dones;
    mu_dones = probe.mu_dones;
    vu_dones = probe.vu_dones;
    head = probe.head;
    tail = probe.tail;
    out_beats = harness.out_flits;
  }
  RT::Reset();
  EXPECT_EQ(dte_dones, 2u);
  EXPECT_EQ(mu_dones, 1u);
  EXPECT_EQ(vu_dones, 1u);
  EXPECT_GT(out_beats, 0u);
  EXPECT_EQ(head, tail);
}

// ── 完整的一条：注入一个 token，收到 MU 算出来的结果 ──

namespace {

// reference/ 那一份算出来的期望：注入什么、应当收到什么。
struct E2eCase {
  std::vector<uint8_t> token, weight, out_bytes, act_bytes;
  std::vector<float> out;
};

std::vector<uint8_t> HexBytes(std::string const& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    out.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
  }
  return out;
}

E2eCase ReadE2eCase() {
  E2eCase c;
  std::ifstream f(std::string(LATCH_SOURCE_DIR) +
                  "/src/bach/compiler/reference/vectors/e2e.txt");
  std::string line;
  while (std::getline(f, line)) {
    // 按行读：注释行里的中文按空白拆出来是奇数个词，成对读会把后面的键值错位。
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    std::string key, val;
    if (!(ls >> key >> val)) continue;
    if (key == "token") c.token = HexBytes(val);
    else if (key == "weight") c.weight = HexBytes(val);
    else if (key == "out_bytes") c.out_bytes = HexBytes(val);
    else if (key == "act_bytes") c.act_bytes = HexBytes(val);
    else if (key == "out_bits") {
      std::istringstream is(val);
      std::string tok;
      while (std::getline(is, tok, ',')) {
        c.out.push_back(numeric::FloatOf(uint32_t(std::stoul(tok, nullptr, 16))));
      }
    }
  }
  return c;
}

// 三步链：datain 把 token 搬进 Core Mem、MU 算一条原语、把结果搬出去。
void WriteGemmChain(Core& core) {
  TaskEntry in;
  in.send_unit = SendUnit::kDte;
  in.recv_unit = RecvUnit::kDsa;
  in.dsa_en = true;
  in.exe_mask = true;
  in.path_id = 3;
  in.task_pc = SymbolOf("task_dte_user_init");
  core.GetTs().Cfg().WriteTask(0, in);

  TaskEntry mul;
  mul.send_unit = SendUnit::kMu;
  mul.recv_unit = RecvUnit::kDsa;
  mul.dsa_en = true;
  mul.exe_mask = true;
  mul.task_pc = SymbolOf("task_mu_compute", "mu");
  core.GetTs().Cfg().WriteTask(1, mul);

  TaskEntry out;
  out.send_unit = SendUnit::kDte;
  out.recv_unit = RecvUnit::kDsa;
  out.dsa_en = true;
  out.end = true;
  out.exe_mask = true;
  out.path_id = 0;
  out.task_pc = SymbolOf("task_dte_send_fc1");
  core.GetTs().Cfg().WriteTask(2, out);

  core.GetTs().Cfg().WritePathMap(3, 0);
  core.GetTs().Cfg().SetInitFinish();
  core.GetDte().Tables().PreloadPathTask(3, 0);
}

}  // namespace

// 步 11 的判据：注入一个 token，收到一个结果，与参考实现逐 bit 相等。
//
// 这一条链上没有桩：token 由 Router 送进来，DTE 搬进 Core Mem，MU 从 Core Mem
// 读它、从 Matrix Mem 读权重、算完写回 Core Mem，DTE 再把结果搬出 core。收到的
// 字节要与 reference/ 那一份 Python 参考实现算出来的逐 bit 相同。
TEST(BachCoreE2e, TokenInResultOutMatchesReference) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  E2eCase want = ReadE2eCase();
  ASSERT_FALSE(want.token.empty())
      << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";
  ASSERT_EQ(want.out_bytes.size(), 128u);

  std::vector<MessagePtr> got;
  std::vector<uint8_t> landed;
  uint64_t mu_dones = 0, head = 0, tail = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreContext ctx;
    Core core(clk, "core", ctx);
    core.GetRouter().Preload(3, EnterCore());
    RouteEntry leave;
    leave.flow_dir = kFlowMid;
    leave.path_core_bypass = true;
    leave.operation = Operation::kForward;
    core.GetRouter().Preload(0, leave);
    core.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
    core.Rv(1).LoadImage(KernelDir() + "kernel_mu.hex");
    WriteGemmChain(core);
    ASSERT_GT(SymbolOf("task_dte_send_fc1"), 0u);

    // 权重是 boot 期装进 Matrix Mem 的，不随 token 走。
    core.Mmem().Poke(0, want.weight);

    // 注入的包带的就是参考实现用的那份 token。
    auto token = std::make_shared<Message>();
    token->path_id = 3;
    token->user_id = 42;
    token->size = want.token.size();
    token->compute = 1;
    token->stream_id = 0;
    token->task_id = 0;
    token->payload = want.token;

    CoreHarness harness(clk, core, /*at=*/2, token);
    CoreProbe probe(clk, core);
    clk->Continue(6000 * kPeriod);
    RT::JoinAll();
    got = harness.out_msgs;
    mu_dones = probe.mu_dones;
    head = probe.head;
    tail = probe.tail;
    // MU 写回 Core Mem 的那一段，出核搬走的就是它。
    landed = core.Cmem().Peek(0x8000, want.out_bytes.size());
  }
  RT::Reset();
  EXPECT_EQ(mu_dones, 1u);   // MU 算了一笔
  EXPECT_EQ(head, tail);     // 走到链尾退休了
  // MU 写回 Core Mem 的那一段与参考实现逐字节相同。
  ASSERT_EQ(landed.size(), want.out_bytes.size());
  for (size_t i = 0; i < landed.size(); ++i) {
    ASSERT_EQ(landed[i], want.out_bytes[i])
        << "Core Mem 第 " << i << " 个字节";
  }
  ASSERT_FALSE(got.empty()) << "出口上一个包都没有";
  MessagePtr result = got.front();
  ASSERT_EQ(result->payload.size(), want.out_bytes.size());
  for (size_t i = 0; i < want.out_bytes.size(); ++i) {
    ASSERT_EQ(result->payload[i], want.out_bytes[i])
        << "第 " << i << " 个字节，第 " << (i / 4) << " 个结果";
  }
}

namespace {

// 四步链：datain、MU 算一条原语、VU 逐元素平方、把结果搬出去。
void WriteGemmActChain(Core& core) {
  TaskEntry in;
  in.send_unit = SendUnit::kDte;
  in.recv_unit = RecvUnit::kDsa;
  in.dsa_en = true;
  in.exe_mask = true;
  in.path_id = 3;
  in.task_pc = SymbolOf("task_dte_user_init");
  core.GetTs().Cfg().WriteTask(0, in);

  TaskEntry mul;
  mul.send_unit = SendUnit::kMu;
  mul.recv_unit = RecvUnit::kDsa;
  mul.dsa_en = true;
  mul.exe_mask = true;
  mul.task_pc = SymbolOf("task_mu_compute", "mu");
  core.GetTs().Cfg().WriteTask(1, mul);

  TaskEntry act;
  act.send_unit = SendUnit::kVu;
  act.recv_unit = RecvUnit::kDsa;
  act.dsa_en = true;
  act.exe_mask = true;
  act.task_pc = SymbolOf("task_vu_compute", "vu");
  core.GetTs().Cfg().WriteTask(2, act);

  TaskEntry out;
  out.send_unit = SendUnit::kDte;
  out.recv_unit = RecvUnit::kDsa;
  out.dsa_en = true;
  out.end = true;
  out.exe_mask = true;
  out.path_id = 0;
  out.task_pc = SymbolOf("task_dte_send_act");
  core.GetTs().Cfg().WriteTask(3, out);

  core.GetTs().Cfg().WritePathMap(3, 0);
  core.GetTs().Cfg().SetInitFinish();
  core.GetDte().Tables().PreloadPathTask(3, 0);
}

// VU 的第 0 组静态配置：LU 读 32 个 FP32，VALU0 逐元素平方，SU 写回。
//
// 地址与长度由 kernel 写进动态参数寄存器，这里只配算什么。
void PreloadVuSquare(Core& core, uint64_t vl) {
  Vu& vu = core.GetVu();
  auto st = [&](uint64_t off, uint64_t data) {
    vu.Preload(kVuStaticBase + 0 * kVuStaticStride + off, data);
  };
  st(kVuLuOp, uint64_t(LuOp::kLdFp32));
  // vfmul.vv：两个源都取 LU 的输出。
  st(kVuValu0Op, uint64_t(ValuOp::kFmulVv) | (uint64_t(kSrcLu) << 8) |
                     (uint64_t(kSrcLu) << 16));
  st(kVuSuOp, uint64_t(SuOp::kStFp32) | (uint64_t(kSrcValu0) << 8));
  st(kVuStaticDupOffset + kVuTypeVl,
     vl | (uint64_t(numeric::RoundMode::kRne) << 17));
}

}  // namespace

// 步 11 的判据加一步计算：注入一个 token，MU 与 VU 各算一遍，收到的结果与参考
// 实现逐 bit 相等。
//
// VU 那一步是逐元素平方而不是 silu：超越函数硬件用查表加插值，拟合方式设计
// 未给，两侧各用各的标准库算出来会差一个 ulp，拿它做逐 bit 判据立不住。
TEST(BachCoreE2e, GemmThenActivationMatchesReference) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  E2eCase want = ReadE2eCase();
  ASSERT_FALSE(want.act_bytes.empty())
      << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";

  std::vector<MessagePtr> got;
  std::vector<uint8_t> landed;
  uint64_t mu_dones = 0, vu_dones = 0, head = 0, tail = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreContext ctx;
    Core core(clk, "core", ctx);
    core.GetRouter().Preload(3, EnterCore());
    RouteEntry leave;
    leave.flow_dir = kFlowMid;
    leave.path_core_bypass = true;
    leave.operation = Operation::kForward;
    core.GetRouter().Preload(0, leave);
    core.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
    core.Rv(1).LoadImage(KernelDir() + "kernel_mu.hex");
    core.Rv(2).LoadImage(KernelDir() + "kernel_vu.hex");
    WriteGemmActChain(core);
    PreloadVuSquare(core, want.out.size());
    core.Mmem().Poke(0, want.weight);

    auto token = std::make_shared<Message>();
    token->path_id = 3;
    token->user_id = 42;
    token->size = want.token.size();
    token->compute = 1;
    token->stream_id = 0;
    token->task_id = 0;
    token->payload = want.token;

    CoreHarness harness(clk, core, /*at=*/2, token);
    CoreProbe probe(clk, core);
    clk->Continue(8000 * kPeriod);
    RT::JoinAll();
    got = harness.out_msgs;
    mu_dones = probe.mu_dones;
    vu_dones = probe.vu_dones;
    head = probe.head;
    tail = probe.tail;
    landed = core.Cmem().Peek(0xC000, want.act_bytes.size());
  }
  RT::Reset();
  EXPECT_EQ(mu_dones, 1u);
  EXPECT_EQ(vu_dones, 1u);
  EXPECT_EQ(head, tail);
  // VU 写回 Core Mem 的那一段与参考实现逐字节相同。
  ASSERT_EQ(landed.size(), want.act_bytes.size());
  for (size_t i = 0; i < landed.size(); ++i) {
    ASSERT_EQ(landed[i], want.act_bytes[i]) << "Core Mem 第 " << i << " 个字节";
  }
  ASSERT_FALSE(got.empty()) << "出口上一个包都没有";
  MessagePtr result = got.front();
  ASSERT_EQ(result->payload.size(), want.act_bytes.size());
  for (size_t i = 0; i < want.act_bytes.size(); ++i) {
    ASSERT_EQ(result->payload[i], want.act_bytes[i])
        << "第 " << i << " 个字节，第 " << (i / 4) << " 个结果";
  }
}

namespace {

// 连着来的几个 token：各自的输入与期望。
struct BatchCase {
  std::vector<uint8_t> weight;
  std::vector<std::vector<uint8_t>> token, out;
};

BatchCase ReadBatchCase() {
  BatchCase c;
  std::ifstream f(std::string(LATCH_SOURCE_DIR) +
                  "/src/bach/compiler/reference/vectors/e2e.txt");
  std::string line;
  std::map<uint64_t, std::vector<uint8_t>> tok, out;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    std::string key, val;
    if (!(ls >> key >> val)) continue;
    if (key == "weight") c.weight = HexBytes(val);
    else if (key.rfind("token", 0) == 0 && key.size() > 5) {
      tok[std::stoull(key.substr(5))] = HexBytes(val);
    } else if (key.rfind("out_bytes", 0) == 0 && key.size() > 9) {
      out[std::stoull(key.substr(9))] = HexBytes(val);
    }
  }
  for (auto const& kv : tok) c.token.push_back(kv.second);
  for (auto const& kv : out) c.out.push_back(kv.second);
  return c;
}

}  // namespace

// 步 11 的后半：放大到 N 个。几个 token 连着进来各占一个 stream 槽位，各自走完
// 整条链，出来的 N 个结果各自与参考实现逐 bit 相等。
//
// 一个 token 一个用户：Retire 发出后 Router 上不得再出现该用户的包，所以同一个
// 用户不会在退休之后又来一笔。
TEST(BachCoreE2e, FourTokensEachMatchReference) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  BatchCase want = ReadBatchCase();
  ASSERT_EQ(want.token.size(), 4u)
      << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";
  ASSERT_EQ(want.out.size(), 4u);

  std::vector<MessagePtr> got;
  uint64_t mu_dones = 0, head = 0, tail = 0;
  {
    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreContext ctx;
    Core core(clk, "core", ctx);
    core.GetRouter().Preload(3, EnterCore());
    RouteEntry leave;
    leave.flow_dir = kFlowMid;
    leave.path_core_bypass = true;
    leave.operation = Operation::kForward;
    core.GetRouter().Preload(0, leave);
    core.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
    core.Rv(1).LoadImage(KernelDir() + "kernel_mu.hex");
    WriteGemmChain(core);
    core.Mmem().Poke(0, want.weight);

    // 四个 token 隔几拍一个地灌进来，各占一个 stream 槽位。
    class Feeder : public BachModule {
     public:
      Feeder(ClockPtr c, Core& t, BatchCase const& w)
          : BachModule(c, "harness"), core(t), want(w) {}

      std::vector<MessagePtr> out_msgs;

     protected:
      void Step() override {
        uint64_t now = CycleNow();
        LinkEndPtr in = core.InWire(kLeft);
        uint64_t idx = (now - 2) / 40;
        if (now >= 2 && (now - 2) % 40 == 0 && idx < want.token.size()) {
          auto m = std::make_shared<Message>();
          m->path_id = 3;
          m->user_id = 100 + idx;
          m->size = want.token[idx].size();
          m->compute = 1;
          // 槽位按到达顺序环形分配，第 i 个用户落在第 i 格。
          m->stream_id = idx;
          m->task_id = 0;
          m->payload = want.token[idx];
          in->flit.Drive(0, true, true, m->size, m);
        } else {
          in->flit.Idle();
        }
        for (uint64_t d = 0; d < 3; ++d) core.BackWire(d)->flit.Idle();

        core.RunStep();

        for (uint64_t d = 0; d < 3; ++d) {
          FlitView f = ReadFlit(core.OutWire(d)->flit);
          if (f.valid && f.msg) out_msgs.push_back(f.msg);
        }
      }

     private:
      Core& core;
      BatchCase const& want;
    };
    Feeder feeder(clk, core, want);
    CoreProbe probe(clk, core);
    clk->Continue(20000 * kPeriod);
    RT::JoinAll();
    got = feeder.out_msgs;
    mu_dones = probe.mu_dones;
    head = probe.head;
    tail = probe.tail;
  }
  RT::Reset();
  EXPECT_EQ(mu_dones, 4u);   // MU 算了四笔
  EXPECT_EQ(head, tail);     // 四个用户都退休了
  ASSERT_EQ(got.size(), want.out.size()) << "出口上收到的包数不对";
  for (size_t i = 0; i < got.size(); ++i) {
    uint64_t idx = got[i]->user_id - 100;
    ASSERT_LT(idx, want.out.size()) << "第 " << i << " 个包的 user_id 不认识";
    ASSERT_EQ(got[i]->payload.size(), want.out[idx].size()) << "user=" << idx;
    for (size_t b = 0; b < want.out[idx].size(); ++b) {
      ASSERT_EQ(got[i]->payload[b], want.out[idx][b])
          << "user=" << idx << " 第 " << b << " 个字节";
    }
  }
}
