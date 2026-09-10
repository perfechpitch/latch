// LPU 层端到端：一个 token 从入口桩进阵列，穿过阵列走到出口那一颗 chip，从
// 出口桩出来。
//
// 与 chip 层那条跨 chip 的区别：这一条从片外进、从片外出 —— 入口桩封包、PCIe
// Switch 按 dst 查目的端口、chip 边缘口把段折成 flit，回来时再折回去，出口桩
// 按 (gpu_id, token_id) 重组并与期望逐字节比对。
//
// 入口在 global_top_left 的西侧，出口在 global_bottom_right 的东侧，所以路要
// 先沿第 0 列往下走完 12 层，再沿最后一层往右走到最后一列，一共 15 颗 chip。
// 只构造这 15 颗：48 颗全建起来逐拍推进要几十毫秒一拍。坐标、形状、角色、链路
// 参数仍按 12 × 4 的整机算。

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/lpu.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

// 每个挂时钟的对象占一个常驻协程，槽位总数是 sub_thread × co_thread。槽位不够
// 时多出来的协程排在 pending 里永远等不到空位，表现是进程卡住而不是报错，所以
// 建时钟这一侧要数清楚要多少个。
constexpr uint64_t kSubThread = 16;
void EnsureSlots(uint64_t coros = 0) {
  uint64_t need = coros + 16;
  uint64_t co = (need + kSubThread - 1) / kSubThread;
  RT::Reset(kSubThread, co < 8 ? 8 : co);
}

// core 与 chip 各占一个常驻协程。它们之间只经打拍的端口通讯，所以与 LPU 一个
// 协程顺序驱动逐拍结果相同，区别只在能不能并行。
LpuCfg TickingCfg(std::vector<uint64_t> const& want) {
  LpuCfg c = LpuBuildOnly(want);
  c.core_tick = true;
  c.chip_tick = true;
  return c;
}

std::string KernelDir() {
  return std::string(LATCH_SOURCE_DIR) + "/src/bach/compiler/kernel/build/";
}

uint64_t SymbolOf(std::string const& name) {
  std::ifstream f(KernelDir() + "kernel_dte.sym");
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

// 一笔搬运的字节数，与 kernel 里的 TOKEN_BYTES 同一个数。入口桩那边一个 token
// 的级联包是 6368 B，要等各段的 shape 都配齐才搬得动，先按 kernel 现在搬的这
// 一段来。
constexpr uint64_t kMoveBytes = 512;

LpuTables Tables() {
  SplitParam split;
  split.ep = 6;
  split.tp = 8;
  split.mode = SplitMode::kEptpNn;
  return BuildLpuTables(split);
}

// 从入口那颗到出口那颗的一条路，每一步记这颗 chip 与它的进出口。
struct Hop {
  uint64_t gx = 0, gy = 0;
  uint64_t in_port = kChipW, out_port = kChipE;
};

// 先沿第 0 列往下走完 12 层，再沿最后一层往右走到最后一列。
std::vector<Hop> RoutePlan() {
  std::vector<Hop> plan;
  for (uint64_t gy = 0; gy < kGridY; ++gy) {
    Hop h;
    h.gx = 0;
    h.gy = gy;
    h.in_port = gy == 0 ? kChipW : kChipN;
    h.out_port = gy + 1 == kGridY ? kChipE : kChipS;
    plan.push_back(h);
  }
  for (uint64_t gx = 1; gx < kGridX; ++gx) {
    Hop h;
    h.gx = gx;
    h.gy = kGridY - 1;
    h.in_port = kChipW;
    h.out_port = kChipE;
    plan.push_back(h);
  }
  return plan;
}

// 一颗 chip 上从进口那颗 core 到出口那颗 core 的一条路。
//
// 四个口各挂在一颗 core 上：N 在行 0 左端、E 在行 0 右端、W 在行 1 左端、S 在
// 行 1 右端。两个出口都在各自那行的最右，所以走法只有一种 —— 先沿进口那一行
// 一直往右，要换行就在最右那颗转 mid 过去。
std::vector<uint64_t> CorePath(ChipShape s, uint64_t in_port,
                               uint64_t out_port) {
  uint64_t cols = ColsOf(s);
  uint64_t in_row = in_port == kChipN ? 0 : 1;
  uint64_t out_row = out_port == kChipE ? 0 : 1;
  std::vector<uint64_t> p;
  for (uint64_t c = 0; c < cols; ++c) p.push_back(in_row * cols + c);
  if (in_row != out_row) p.push_back(out_row * cols + cols - 1);
  return p;
}

// 路上第 i 颗往下一颗走哪个方向。要换行时倒数第二颗走 mid，其余一直往右；最后
// 那颗往右就是出 E 口或 S 口，两个口都挂在各自那行的右端。
uint64_t FlowAt(uint64_t i, uint64_t n, bool cross) {
  return cross && i + 2 == n ? kFlowMid : kFlowRight;
}

bool CrossRow(Hop const& h) {
  return (h.in_port == kChipN) != (h.out_port == kChipE);
}

RouteEntry EnterCore() {
  RouteEntry e;
  e.flow_dir = 0;
  e.path_core_bypass = false;
  e.operation = Operation::kForward;
  return e;
}

RouteEntry PassTo(uint64_t flow, uint64_t ext_dst = 0) {
  RouteEntry e;
  e.flow_dir = flow;
  e.path_core_bypass = true;
  e.operation = Operation::kForward;
  e.ext_dst = ext_dst;
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

// 只转发的一颗：路上每颗 core 都给这条 path 配一项，包不进核。
void WirePassChip(Chip& chip, Hop const& h, uint64_t path) {
  std::vector<uint64_t> p = CorePath(chip.Shape(), h.in_port, h.out_port);
  for (uint64_t i = 0; i < p.size(); ++i) {
    chip.GetCore(p[i]).GetRouter().Preload(
        path, PassTo(FlowAt(i, p.size(), CrossRow(h))));
  }
}

// 落地的一颗：包在路上第一颗派了角色的 core 进核，搬进 Core Mem 再发出去。
// 不派角色的那颗只转发，落不了任务。
// ext_dst 只有出口那颗填：它的出核包要经 PCIe Switch 才到得了出口桩。
uint64_t WireLandingChip(Chip& chip, Hop const& h, uint64_t in_path,
                         uint64_t out_path, uint64_t ext_dst) {
  std::vector<uint64_t> p = CorePath(chip.Shape(), h.in_port, h.out_port);
  uint64_t n = p.size();

  uint64_t landing = n;
  for (uint64_t i = 0; i < n; ++i) {
    Core& c = chip.GetCore(p[i]);
    uint64_t flow = FlowAt(i, n, CrossRow(h));
    if (landing == n && !c.Context().router_only) {
      landing = i;
      c.GetRouter().Preload(in_path, EnterCore());
      c.GetRouter().Preload(out_path, PassTo(flow, ext_dst));
      // DTE 手上另有一份按 path_id 查的副本，出核造包时读的是它。
      c.GetDte().Tables().PreloadRtab(out_path, PassTo(flow, ext_dst));
      c.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
      WriteRelayChain(c, in_path, out_path);
      continue;
    }
    // 落地那颗之前的转发进来的，之后的转发出去的。
    c.GetRouter().Preload(i < landing ? in_path : out_path, PassTo(flow));
  }
  LOGCHECK(landing < n, "WireLandingChip: 这颗 chip 上没有能落任务的 core。");
  return p[landing];
}

// 一个 LPU 上的模块全部由这一个协程驱动。表的读数在协程里抄下来，主线程读不到
// 协程里的 Logic。
class LpuDriver : public BachModule {
 public:
  LpuDriver(ClockPtr c, Lpu& target, std::vector<Core*> watch,
            std::vector<LinkEndPtr> probe = {})
      : BachModule(c, "driver"), lpu(target), cores(std::move(watch)),
        wires(std::move(probe)) {
    wire_cnt.assign(wires.size(), 0);
    triggers.assign(cores.size(), 0);
  }

  std::vector<uint64_t> triggers;
  uint64_t done = 0, mismatch = 0, done_at = 0;
  // 一路上的几个计数：注入了几个 token、入口那座 Switch 转发了几个 flit、
  // 入口那颗 chip 的桥拼回了几个包。
  uint64_t injected = 0, sw_in = 0, chip_in = 0;
  std::vector<uint64_t> wire_cnt;
  bool stop_when_done = false;

 protected:
  void Step() override {
    // head_ptr 是 Stream_table 自己 Step() 里改的普通成员，别的协程不读它；
    // 停钟之后再取。Triggers() 是 Logic64，只在协程里读得到当拍的值。
    for (uint64_t i = 0; i < cores.size(); ++i) {
      triggers[i] = cores[i]->GetRouter().GetCoreStation().Triggers();
    }
    lpu.RunStep();
    uint64_t now = lpu.Out().DoneCount();
    if (now != done && done_at == 0) {
      done_at = CycleNow();
      // 收齐了就停钟：这一层跑满上限要几分钟，尾巴上全是空拍。
      if (stop_when_done) clk->Stop();
    }
    done = now;
    mismatch = lpu.Out().MismatchCount();
    for (uint64_t i = 0; i < wires.size(); ++i) {
      if (ReadFlit(wires[i]->flit).valid) ++wire_cnt[i];
    }
    injected = lpu.In().Injected();
    sw_in = lpu.Switch(0).Forwarded();
    chip_in = lpu.GetChip(0, 0).Port(kChipW).Rx().Assembled();
  }

 private:
  Lpu& lpu;
  std::vector<Core*> cores;
  std::vector<LinkEndPtr> wires;
};

std::vector<uint8_t> TokenBytes(uint64_t token) {
  std::vector<uint8_t> b(kMoveBytes);
  for (uint64_t i = 0; i < kMoveBytes; ++i) {
    b[i] = uint8_t((token * 17 + i * 3) & 0xFFu);
  }
  return b;
}

}  // namespace

// 一个 token 从入口桩进来，在入口那颗 chip 上搬进 Core Mem 再发出去，穿过中间
// 十三颗只转发的 chip，在出口那颗上再搬一次，从出口桩出来。收到的字节与注入的
// 相等。
TEST(BachLpuE2e, TokenCrossesTheArray) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  std::vector<uint8_t> payload = TokenBytes(0);
  std::vector<uint64_t> heads;
  uint64_t done = 0, mismatch = 0, done_at = 0;
  {
    std::vector<Hop> plan = RoutePlan();
    std::vector<uint64_t> want;
    for (Hop const& h : plan) want.push_back(ChipIdOf(h.gx, h.gy));
    EnsureSlots(want.size() * (kMaxCorePerChip + 1));
    ClockPtr clk = MakeClock(0, kPeriod);
    Lpu lpu(clk, "lpu", Tables(), TickingCfg(want));

    // 三个 path：3 号进入口那颗，4 号穿过中间那十三颗，5 号从出口那颗出到片外。
    // 一颗 core 上进核与出核是两条不同的 path —— 同一个号只能配一种走法。
    constexpr uint64_t kInPath = 3, kMidPath = 4, kOutPath = 5;
    std::vector<Core*> landing;
    for (uint64_t k = 0; k < plan.size(); ++k) {
      Hop const& h = plan[k];
      Chip& chip = lpu.GetChip(h.gx, h.gy);
      if (k == 0) {
        landing.push_back(
            &chip.GetCore(WireLandingChip(chip, h, kInPath, kMidPath, 0)));
      } else if (k + 1 == plan.size()) {
        landing.push_back(&chip.GetCore(
            WireLandingChip(chip, h, kMidPath, kOutPath, kNodeOutStub)));
      } else {
        WirePassChip(chip, h, kMidPath);
      }
    }

    InjectItem it;
    it.inject_cycle = 2;
    it.gpu_id = 0;
    it.token_id = 0;
    it.dst = ChipIdOf(0, 0);
    it.path_id = 3;
    it.bytes = kMoveBytes;
    it.payload = payload;
    lpu.In().SetInjectTable({it});
    lpu.In().SetGpuBuffer(0, 8);
    lpu.Out().SetExpect(0, 0, payload);
    lpu.Out().SetExpectTokens(1);

    LpuDriver driver(clk, lpu, landing);
    driver.stop_when_done = true;
    // 两头各一段以太网，3000 T 一段；中间十四段 C2C 加十五颗 chip 的转发。
    clk->Continue(25000 * kPeriod);
    RT::JoinAll();
    for (Core* c : landing) heads.push_back(c->GetTs().Table().HeadPtr());
    done = driver.done;
    mismatch = driver.mismatch;
    done_at = driver.done_at;
  }
  RT::Reset();

  EXPECT_EQ(done, 1u) << "出口桩没收到这个 token";
  // 两头各一段以太网，一段 3000 T。到得比这早说明有一段没走。
  EXPECT_GT(done_at, 2 * 3000u) << "两段以太网的时间没记上";
  EXPECT_EQ(mismatch, 0u) << "出口桩收到的字节与注入的不一样";
  for (uint64_t i = 0; i < heads.size(); ++i) {
    EXPECT_EQ(heads[i], 1u) << "第 " << i << " 颗落地的 chip 上这个用户没退休";
  }
}

// 更小的一条：只建入口那颗 chip。token 从入口桩进来，过 PCIe Switch 与边缘口
// 的段与 flit 折算，落在这颗 chip 上搬进 Core Mem，用户退休。发出去那一笔停在
// 没有对端的 E 口上。
TEST(BachLpuE2e, TokenEntersTheFirstChip) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  std::vector<uint8_t> payload = TokenBytes(0);
  uint64_t head = 0, injected = 0, sw_in = 0, chip_in = 0;
  uint64_t trigger = 0, relayed = 0;
  {
    EnsureSlots(kMaxCorePerChip + 1);
    ClockPtr clk = MakeClock(0, kPeriod);
    Lpu lpu(clk, "lpu", Tables(), TickingCfg({ChipIdOf(0, 0)}));

    Hop h;
    h.gx = 0;
    h.gy = 0;
    h.in_port = kChipW;
    h.out_port = kChipE;
    Chip& chip = lpu.GetChip(0, 0);
    Core& core = chip.GetCore(WireLandingChip(chip, h, 3, 4, 0));

    InjectItem it;
    it.inject_cycle = 2;
    it.dst = ChipIdOf(0, 0);
    it.path_id = 3;
    it.bytes = kMoveBytes;
    it.payload = payload;
    lpu.In().SetInjectTable({it});
    lpu.In().SetGpuBuffer(0, 8);

    // 入口那一段是以太网，3000 T 的线上延迟。
    // 第一列 chip 的 core5 不派角色，包从 W 口进来先经它转发。kDirRight 是 2。
    LpuDriver driver(clk, lpu, {&core}, {chip.GetCore(5).OutWire(2)});
    clk->Continue(6000 * kPeriod);
    RT::JoinAll();
    head = core.GetTs().Table().HeadPtr();
    trigger = driver.triggers[0];
    injected = driver.injected;
    sw_in = driver.sw_in;
    chip_in = driver.chip_in;
    relayed = driver.wire_cnt[0];
  }
  RT::Reset();

  EXPECT_EQ(injected, 1u) << "入口桩没把 token 发出来";
  EXPECT_GT(sw_in, 0u) << "入口那座 PCIe Switch 一个 flit 都没转发";
  EXPECT_EQ(chip_in, 1u) << "入口那颗 chip 的桥没拼回这个包";
  EXPECT_GT(relayed, 0u) << "不派角色的那颗 core 没把包转出去";
  EXPECT_EQ(trigger, 1u) << "落地那颗 core 没收到这个包";
  EXPECT_EQ(head, 1u) << "这颗 chip 上这个用户没退休";
}
