#ifndef _LATCH_TEST_BACH_IP_CHIP_MOE_COMMON_
#define _LATCH_TEST_BACH_IP_CHIP_MOE_COMMON_

// 一层 MoE 摊在多个 core 上那几个用例共用的部分：比对向量的读入、chip 内的
// 槽位几何、每个 core 的任务链与权重、广播树与归约链的接线、驱动几颗 chip 的
// 那个协程。三个规模（一颗 chip、一个 EP 组、48 颗 chip）各自的摆放留在各自
// 的用例文件里。

#include <gtest/gtest.h>

#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/common/numeric/formats.h"
#include "bach/ip/bundle_load.h"
#include "bach/ip/chip/chip.h"
#include "bach/ip/lpu_grid.h"

namespace latch {
namespace bach {
namespace moetest {


constexpr Time kPeriod = 1;

// 每个挂时钟的模块占一个常驻协程，槽位总数是 sub_thread × co_thread。槽位不够
// 时多出来的协程排在 pending 里永远等不到空位，表现是进程卡住而不是报错，所以
// 建时钟这一侧要数清楚要多少个。cores 是本次要各占协程的 core 数，另外留出
// harness 与余量。
constexpr uint64_t kSubThread = 16;
// core 各占一个常驻协程。关掉就退回“装配层一个协程顺序调 RunStep()”，两者逐拍
// 结果相同，用来对照跑。
constexpr bool kCoreTick = true;
// 片内链路、ctrl_noc 端点与 SCP 也各占一个协程（每颗 chip 一个）。
constexpr bool kChipTick = true;
inline void EnsureSlots(uint64_t coros = 0) {
  uint64_t need = coros + 16;
  uint64_t co = (need + kSubThread - 1) / kSubThread;
  RT::Reset(kSubThread, co < 8 ? 8 : co);
}

// 编译器产的那些 bundle 的根：一份拓扑描述编出一套，各占一个子目录。
inline std::string BundleRoot() {
  return std::string(LATCH_SOURCE_DIR) + "/src/bach/compiler/bundle";
}

// 波形写成 <name>.trace，落在跑测试时的当前目录。一个进程里几个用例各写各的，
// 所以每个用例给一个自己的名字。
inline void TraceInto(std::string const& name) {
  RT::GetRecorder().StartNew(name);
}

// 趁模块表还在把波形收尾。信号名与层次是从模块表写进波形的，而 RT::Reset() 会
// 把那张表清掉，Recorder 又要等进程退出才析构，那时读到的是空表，波形里一个信
// 号名都没有。调它之前模块要先析构完，段数据才落得齐。
inline void TraceDone() { RT::FlushRecorder(); }

inline std::string KernelDir() {
  return std::string(LATCH_SOURCE_DIR) + "/src/bach/compiler/kernel/build/";
}

inline uint64_t SymbolOf(std::string const& name, std::string const& kind) {
  std::ifstream f(KernelDir() + "kernel_" + kind + ".sym");
  std::string addr, type, sym;
  while (f >> addr >> type >> sym) {
    if (sym == name) return std::stoull(addr, nullptr, 16);
  }
  return 0;
}

inline bool KernelBuilt() {
  std::ifstream f(KernelDir() + "kernel_mu.hex");
  return f.good();
}

// ── 比对向量 ──

struct SpreadCase {
  uint64_t k = 0, inter = 0, out_n = 0, experts = 0, chips = 0, cores = 0;
  uint64_t w1_seed = 0, w3_seed = 0, w2_seed = 0, seed_stride = 0;
  // 归约次序，全局 core 号（chip 号 × 每 chip 的 core 数，加 chip 内号）。
  std::vector<uint64_t> order;
  std::vector<float> w_ep;
  std::vector<uint8_t> token;
  std::vector<std::vector<uint32_t>> core_bits;
  std::vector<uint32_t> out_bits;

  uint64_t Total() const { return chips * cores; }
};

inline std::vector<uint8_t> HexBytes(std::string const& s) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    v.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
  }
  return v;
}

inline std::vector<uint32_t> HexWords(std::string const& s) {
  std::vector<uint32_t> v;
  std::stringstream ss(s);
  std::string one;
  while (std::getline(ss, one, ',')) {
    if (!one.empty()) v.push_back(uint32_t(std::stoul(one, nullptr, 16)));
  }
  return v;
}

inline std::vector<uint64_t> DecList(std::string const& s) {
  std::vector<uint64_t> v;
  std::stringstream ss(s);
  std::string one;
  while (std::getline(ss, one, ',')) {
    if (!one.empty()) v.push_back(std::stoull(one));
  }
  return v;
}

inline SpreadCase ReadCase(std::string const& name) {
  SpreadCase c;
  std::ifstream f(std::string(LATCH_SOURCE_DIR) +
                  "/src/bach/compiler/reference/vectors/" + name);
  std::string line;
  std::map<std::string, std::string> kv;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    size_t sp = line.find(' ');
    if (sp == std::string::npos) continue;
    kv[line.substr(0, sp)] = line.substr(sp + 1);
  }
  if (kv.count("k") == 0) return c;
  c.k = std::stoull(kv["k"]);
  c.inter = std::stoull(kv["inter"]);
  c.out_n = std::stoull(kv["out_n"]);
  c.experts = std::stoull(kv["experts"]);
  c.chips = std::stoull(kv["chips"]);
  c.cores = std::stoull(kv["cores"]);
  c.order = DecList(kv["order"]);
  c.w1_seed = std::stoull(kv["w1_seed"], nullptr, 16);
  c.w3_seed = std::stoull(kv["w3_seed"], nullptr, 16);
  c.w2_seed = std::stoull(kv["w2_seed"], nullptr, 16);
  c.seed_stride = std::stoull(kv["seed_stride"], nullptr, 16);
  for (uint32_t b : HexWords(kv["w_ep"])) c.w_ep.push_back(numeric::FloatOf(b));
  c.token = HexBytes(kv["token"]);
  for (uint64_t g = 0; g < c.Total(); ++g) {
    c.core_bits.push_back(HexWords(kv["core_bits" + std::to_string(g)]));
  }
  c.out_bits = HexWords(kv["out_bits"]);
  return c;
}

// 与 reference/vectors.py 的 tame_cpp 逐字节相同的一批数。
inline std::vector<uint8_t> Pattern(uint64_t n, uint64_t seed) {
  std::vector<uint8_t> v;
  uint64_t s = seed;
  for (uint64_t i = 0; i < n; ++i) {
    s = s * 1103515245u + 12345u;
    v.push_back(uint8_t((s >> 16) & 0xFFu));
  }
  return v;
}

inline std::vector<uint8_t> TameBf16(uint64_t count, uint64_t seed) {
  std::vector<uint8_t> v = Pattern(count * 2, seed);
  for (uint64_t i = 0; i + 1 < v.size(); i += 2) {
    uint8_t sign = uint8_t(v[i + 1] & 0x80u);
    uint8_t exp = uint8_t(126 + (v[i] % 3));
    v[i + 1] = uint8_t(sign | (exp >> 1));
    v[i] = uint8_t(((exp & 1u) << 7) | (v[i] & 0x7Fu));
  }
  return v;
}

// topK 表在 Core Mem 里的样子：每项 {expert_id 2 B, weight 4 B}。
inline std::vector<uint8_t> TopkBytes(std::vector<TopkEntry> const& t) {
  std::vector<uint8_t> b(kTopkBytesPerStream, 0);
  for (size_t i = 0; i < t.size(); ++i) {
    uint64_t at = i * kTopkEntryBytes;
    b[at] = uint8_t(t[i].expert_id & 0xFFu);
    b[at + 1] = uint8_t((t[i].expert_id >> 8) & 0xFFu);
    uint32_t w = numeric::BitsOf(t[i].weight);
    for (int k = 0; k < 4; ++k) b[at + 2 + k] = uint8_t((w >> (8 * k)) & 0xFFu);
  }
  return b;
}

// ── 摆放 ──
//
// 与 compiler/kernel/bach.h 的 MOE_* 同源，改一处要一起改。
constexpr uint64_t kTopkAt = 0x3000;
constexpr uint64_t kSendAt = 0x9800;
constexpr uint64_t kResultAt = kSendAt + kReduceSwHeaderBytes;
constexpr uint64_t kW1At = 0x000000;
constexpr uint64_t kW3At = 0x600000;
constexpr uint64_t kW2At = 0xC00000;
constexpr uint64_t kBStride = 0x300000;

// token 广播进各 core 走这条，FC2 的部分和沿链归约走那条。两条的 VC 由 path_id
// 定：DTE 出核那一笔按 path_id % 4 挑 VC，归约要落在 VC3 上。
constexpr uint64_t kInPath = 4;
constexpr uint64_t kOutPath = 7;
constexpr uint64_t kUserId = 77;
// 与 compiler/kernel/bach.h 的 MOE_SEND_HALF_OFF 同源。
constexpr uint64_t kSendHalfOff = 0x0400;

// weights 加载阶段走的那条 path：这一阶段只用一条，走遍格子里的 8 个计算 core，
// 一个包落哪个 core 由包头的 path_core_mask 挑，位号就是槽位号。
constexpr uint64_t kWeightsPath = 8;
// 与 compiler/kernel/bach.h 的 WEIGHTS_CNT_OFF 同源。
constexpr uint64_t kWeightsCntOff = 0x05D0;
// 一个权重包搬多少。包长上限 32 KB，取一半，一包 64 flit。
constexpr uint64_t kWeightsChunk = 16 * 1024;
// 这一阶段的用户号。权重不进归约，与业务那个用户不相干。
constexpr uint64_t kWeightsUser = 3;

// ── chip 内的拓扑 ──
//
// 三种形状的计算 core 都是 8 个，都摆成 2 行 4 列的格子：同行相邻两个有
// left / right 一对链路，上下同列的两个有 mid 直连。下面把格子里的位置叫槽位，
// 槽位号是 行 × 4 + 列，片内 core 号由形状换算。
//
// 中间列 chip 是 2×4，槽位号就是片内 core 号。第一列与最后一列是 2×5，多出来
// 的那一列放 B core、R core 与不派角色的那个 core，它们不算计算 core，槽位换算
// 因此要跳过它们。
constexpr uint64_t kCols = 4;
constexpr uint64_t kCorePerChip = 8;

// 四个 chip 口用的是所在 core 的哪个方向。
inline uint64_t DirOfPort(uint64_t port) {
  return port == kChipN || port == kChipW ? kDirLeft : kDirRight;
}

// 四个 chip 口各坐在格子的哪个角上：N 是行 0 列 0，E 是行 0 列 3，W 是行 1
// 列 0，S 是行 1 列 3。
inline uint64_t SlotOfPort(uint64_t port) {
  static const uint64_t kSlot[kChipPortNum] = {0, kCols - 1, kCols,
                                               2 * kCols - 1};
  return kSlot[port];
}
// 一个槽位上如果挂着 chip 口，是哪一个。没有就返回 kChipPortNum。
inline uint64_t PortOfSlot(uint64_t slot) {
  for (uint64_t p = 0; p < kChipPortNum; ++p) {
    if (SlotOfPort(p) == slot) return p;
  }
  return kChipPortNum;
}

// 槽位换成片内 core 号。
inline uint64_t CoreOfSlot(ChipShape shape, uint64_t slot) {
  uint64_t row = slot / kCols;
  uint64_t col = slot % kCols;
  if (shape == ChipShape::kMiddle) return row * kCols + col;
  // 2×5：第一列 chip 的 core0 是 B core、core5 不派角色，计算 core 从每行第
  // 二个起；最后一列 chip 的 core4 不派角色、core9 是 R core，计算 core 是每
  // 行前四个。
  uint64_t base = shape == ChipShape::kFirst ? 1 : 0;
  return row * (kCols + 1) + base + col;
}

// 坐在某个 chip 口上的那个 core。它不一定是计算 core：2×5 的四个角里有两个是
// B core / R core / 不派角色的那个，那种 core 在这两条 path 上只做转发。
inline uint64_t CoreOfPort(ChipShape shape, uint64_t port) {
  uint64_t cols = shape == ChipShape::kMiddle ? kCols : kCols + 1;
  static const uint64_t kRow[kChipPortNum] = {0, 0, 1, 1};
  static const bool kAtTail[kChipPortNum] = {false, true, false, true};
  return kRow[port] * cols + (kAtTail[port] ? cols - 1 : 0);
}
inline bool PortSitsOnCompute(ChipShape shape, uint64_t port) {
  return CoreOfPort(shape, port) == CoreOfSlot(shape, SlotOfPort(port));
}

// 槽位 a 往槽位 b 发要走哪个方向。两个槽位不相邻时返回 kDirNum。
inline uint64_t DirBetween(uint64_t a, uint64_t b) {
  if (a / kCols == b / kCols) {
    if (b == a + 1) return kDirRight;
    if (a == b + 1) return kDirLeft;
    return kDirNum;
  }
  return a % kCols == b % kCols ? kDirMid : kDirNum;
}

inline uint64_t FlowOf(uint64_t dir) {
  if (dir == kDirMid) return kFlowMid;
  if (dir == kDirLeft) return kFlowLeft;
  return kFlowRight;
}

// 从 dir 方向发出去的包，落在对面那个 core 的哪个方向上。mid 是 mid 对 mid，
// left 与 right 互为对面。
inline uint64_t OppositeOf(uint64_t dir) {
  if (dir == kDirMid) return kDirMid;
  return dir == kDirLeft ? kDirRight : kDirLeft;
}

// ── 每个 core 的配置 ──

// EPTP-NN 的五步。第 2 步与第 3 步各在一个 task 里发几笔 DSA 任务，收 RV core
// 那一路的完成；最后一步是 reduce 任务，DTE 的搬运与 Router 的归约两半配齐才
// 算完。
inline void WriteMoeChain(Core& core, uint64_t out_path) {
  TaskEntry in;
  in.send_unit = SendUnit::kDte;
  in.recv_unit = RecvUnit::kDsa;
  in.dsa_en = true;
  in.exe_mask = true;
  in.path_id = kInPath;
  in.task_pc = SymbolOf("task_dte_user_init", "dte");
  core.GetTs().Cfg().WriteTask(0, in);

  TaskEntry fc13;
  fc13.send_unit = SendUnit::kMu;
  fc13.recv_unit = RecvUnit::kRvOnly;
  fc13.exe_mask = true;
  fc13.task_pc = SymbolOf("task_mu_fc13", "mu");
  core.GetTs().Cfg().WriteTask(1, fc13);

  TaskEntry gate;
  gate.send_unit = SendUnit::kVu;
  gate.recv_unit = RecvUnit::kRvOnly;
  gate.exe_mask = true;
  gate.task_pc = SymbolOf("task_vu_gate", "vu");
  core.GetTs().Cfg().WriteTask(2, gate);

  TaskEntry fc2;
  fc2.send_unit = SendUnit::kMu;
  fc2.recv_unit = RecvUnit::kDsa;
  fc2.dsa_en = true;
  fc2.exe_mask = true;
  fc2.task_pc = SymbolOf("task_mu_fc2", "mu");
  core.GetTs().Cfg().WriteTask(3, fc2);

  TaskEntry out;
  out.send_unit = SendUnit::kDte;
  out.recv_unit = RecvUnit::kDsa;
  out.dsa_en = true;
  out.reduce = true;
  out.reduce_num = 1;
  out.end = true;
  out.exe_mask = true;
  out.path_id = out_path;
  out.task_pc = SymbolOf("task_dte_send_moe", "dte");
  core.GetTs().Cfg().WriteTask(4, out);

  core.GetTs().Cfg().WritePathMap(kInPath, 0);
  core.GetTs().Cfg().SetInitFinish();
  core.GetDte().Tables().PreloadPathTask(kInPath, 0);
}

// 广播那一条：落进本 core，再按 flow 往下游复制。
inline RouteEntry EnterAndSpread(uint64_t flow) {
  RouteEntry e;
  e.flow_dir = flow;
  e.path_core_bypass = false;
  e.stream_table_enable = false;
  e.operation = Operation::kForward;
  return e;
}

// 归约那一条：本级收哪几路、算完往哪个方向发。结果不进任何 core，链尾那一份
// 直接出 chip。
inline RouteEntry ReduceHop(uint64_t in_mask, uint64_t flow, bool first) {
  RouteEntry e;
  e.op_type = OpType::kReduce;
  e.flow_dir = flow;
  e.path_core_bypass = true;
  e.reduce_in_mask = in_mask;
  e.operation = first ? Operation::kReduce0 : Operation::kReduce1;
  e.reduce_data_type = kReduceFp32;
  e.reduce_outdata_type = kReduceFp32;
  return e;
}

// 只转发：坐在 chip 口上但不是计算 core 的那几个（B core、R core、不派角色的
// 那个）在这两条 path 上都只做转发，不落地也不累加。
inline RouteEntry PassThrough(uint64_t flow) {
  RouteEntry e;
  e.flow_dir = flow;
  e.path_core_bypass = true;
  e.stream_table_enable = false;
  e.operation = Operation::kForward;
  return e;
}

// chip 内的广播树：token 从 enter_port 进来落在格子的第 0 列，沿本行往右铺满，
// 再经 mid 到另一行的同一列，同样往右铺满。行末那个槽位坐在 chip 口上，要不要
// 往外发由 out_port 给出。口上那个 core 不是计算 core 时，进出各多一跳转发。
inline void WireBroadcast(Chip& chip, ChipShape shape, uint64_t enter_port,
                   std::vector<uint64_t> const& out_port) {
  if (!PortSitsOnCompute(shape, enter_port)) {
    chip.GetCore(CoreOfPort(shape, enter_port))
        .GetRouter()
        .Preload(kInPath,
                 PassThrough(FlowOf(OppositeOf(DirOfPort(enter_port)))));
  }
  uint64_t head = SlotOfPort(enter_port) / kCols * kCols;
  uint64_t rows[2] = {head, head ^ kCols};
  for (uint64_t r = 0; r < 2; ++r) {
    for (uint64_t j = 0; j < kCols; ++j) {
      uint64_t slot = rows[r] + j;
      uint64_t flow = 0;
      if (j + 1 < kCols) {
        flow |= kFlowRight;
      } else {
        for (uint64_t p : out_port) {
          if (SlotOfPort(p) == slot) flow |= FlowOf(DirOfPort(p));
        }
      }
      // 进来那个槽位还要往另一行发一份。
      if (r == 0 && j == 0) flow |= kFlowMid;
      chip.GetCore(CoreOfSlot(shape, slot))
          .GetRouter()
          .Preload(kInPath, EnterAndSpread(flow));
    }
  }
  for (uint64_t p : out_port) {
    if (PortSitsOnCompute(shape, p)) continue;
    chip.GetCore(CoreOfPort(shape, p))
        .GetRouter()
        .Preload(kInPath, PassThrough(FlowOf(DirOfPort(p))));
  }
}

// 归约链：按 order 逐跳铺。order 是全局槽位号（chip 号 × 8 加 chip 内槽位号），
// 同一颗 chip 内相邻两跳走 core 之间的链路，跨 chip 那一跳走两颗 chip 对接的
// 那个口。口上那个 core 不是计算 core 时，那一跳多一次转发。
//
// 本 core 自己那一份分量从 core 方向进 ReduceModule，默认落在 bit0 那一路，与
// mid 同一路；上游正好从 mid 来时两者会挤在一起，所以那种 core 的表项置
// reduce1，把自己那一份挪到 bit1。
//
// tail_flow 是链尾那一份往哪个方向发，它出 chip 之后不再有下一跳。
inline void WireReduceChain(std::vector<Chip*> const& chips,
                     std::vector<uint64_t> const& order, uint64_t tail_flow,
                     uint64_t path = kOutPath) {
  for (uint64_t i = 0; i < order.size(); ++i) {
    uint64_t chip = order[i] / kCorePerChip;
    uint64_t self = order[i] % kCorePerChip;
    ChipShape shape = chips[chip]->Shape();

    uint64_t in_lane = kDirNum;
    if (i > 0) {
      uint64_t prev_chip = order[i - 1] / kCorePerChip;
      uint64_t prev = order[i - 1] % kCorePerChip;
      if (prev_chip == chip) {
        in_lane = OppositeOf(DirBetween(prev, self));
        ASSERT_NE(in_lane, kDirNum) << "第 " << i << " 跳的两个槽位不相邻";
      } else {
        // 跨 chip 那一跳落在本槽位那个 chip 口上。
        uint64_t port = PortOfSlot(self);
        ASSERT_LT(port, kChipPortNum) << "第 " << i << " 跳的入口槽位上没有口";
        ASSERT_LT(PortOfSlot(prev), kChipPortNum)
            << "第 " << i << " 跳的出口槽位上没有口";
        in_lane = DirOfPort(port);
        if (!PortSitsOnCompute(shape, port)) {
          chips[chip]
              ->GetCore(CoreOfPort(shape, port))
              .GetRouter()
              .Preload(path, PassThrough(FlowOf(OppositeOf(in_lane))));
        }
      }
    }

    uint64_t flow = tail_flow;
    uint64_t out_port = kChipPortNum;
    if (i + 1 < order.size()) {
      uint64_t next_chip = order[i + 1] / kCorePerChip;
      uint64_t next = order[i + 1] % kCorePerChip;
      if (next_chip == chip) {
        uint64_t d = DirBetween(self, next);
        ASSERT_NE(d, kDirNum) << "第 " << i << " 跳往下走的两个槽位不相邻";
        flow = FlowOf(d);
      } else {
        out_port = PortOfSlot(self);
        ASSERT_LT(out_port, kChipPortNum) << "第 " << i << " 跳的出口槽位上没有口";
        flow = FlowOf(DirOfPort(out_port));
      }
    } else {
      out_port = PortOfSlot(self);
    }
    if (out_port < kChipPortNum && !PortSitsOnCompute(shape, out_port)) {
      chips[chip]
          ->GetCore(CoreOfPort(shape, out_port))
          .GetRouter()
          .Preload(path, PassThrough(FlowOf(DirOfPort(out_port))));
    }

    uint64_t own_lane = in_lane == kDirMid ? 1 : 0;
    if (own_lane == 1) flow |= kFlowReduce1;
    uint64_t mask = 1ull << own_lane;
    if (in_lane != kDirNum) mask |= 1ull << in_lane;

    RouteEntry e = ReduceHop(mask, flow, i == 0);
    Core& core = chips[chip]->GetCore(CoreOfSlot(shape, self));
    core.GetRouter().Preload(path, e);
    core.GetDte().Tables().PreloadRtab(path, e);
  }
}

// 一个 core 上要装的三样：kernel、任务链、这一片权重与 topK 表。
// 三个 RV core 的镜像。每个 core 装的都是同一份，差别只在 TS 的任务链配置。
inline void LoadKernels(Core& core) {
  core.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
  core.Rv(1).LoadImage(KernelDir() + "kernel_mu.hex");
  core.Rv(2).LoadImage(KernelDir() + "kernel_vu.hex");
}

// 铺数据与运行期那两处资源：权重、topK 表、本组专家表、Share Mem 的初值，以及
// 归约那一条要的用户上下文与 credit。配置表不在这里，由 bundle 装。
inline void SetUpCoreData(Core& core, SpreadCase const& want, uint64_t shard,
                          uint64_t send_half = 0) {
  // 本组把结果送到 R core 的哪一半，boot 期写进 Share Mem。
  core.Smem().Poke(kSendHalfOff, {uint8_t(send_half), 0, 0, 0});

  // topK 里第 0 个是全局 17 号专家、组内第 1 个，第 1 个是全局 5 号、组内第 0 个。
  core.GetMu().EpInfo().SetLocalEpTable({5, 17});
  core.Cmem().Poke(kTopkAt,
                   TopkBytes({{17, want.w_ep[0]}, {5, want.w_ep[1]}}));
  for (uint64_t e = 0; e < want.experts; ++e) {
    uint64_t local = e == 0 ? 1 : 0;
    uint64_t off = shard * want.seed_stride + e;
    core.Mmem().Poke(kW1At + local * kBStride,
                     TameBf16(want.k * want.inter, want.w1_seed + off));
    core.Mmem().Poke(kW3At + local * kBStride,
                     TameBf16(want.k * want.inter, want.w3_seed + off));
    core.Mmem().Poke(kW2At + local * kBStride,
                     TameBf16(want.inter * want.out_n, want.w2_seed + off));
  }

  // 归约那一条的两处占用：Router 里给这个用户开一个 ReduceModule 上下文，
  // DTE 侧给它拨一份本级 Reduce credit。真机上这两样由建表那一步一起做。
  core.GetRouter().AllocUser(kUserId);
  // credit 按 flit 记，要够一整个 reduce 包。
  core.GetDte().Tables().AllocReduceCredit(
      kUserId, FlitsOf(kReduceSwHeaderBytes + want.out_n * 4) + 4);
}

// 手工配表那一档：kernel 与任务链自己写，数据自己铺。单元测试用这个。
inline void SetUpCore(Core& core, SpreadCase const& want, uint64_t shard,
                      uint64_t out_path = kOutPath, uint64_t send_half = 0) {
  LoadKernels(core);
  WriteMoeChain(core, out_path);
  SetUpCoreData(core, want, shard, send_half);
}

// ── weights 加载阶段 ──
//
// 装模型时权重不经 SCP，走的是 Host 那条 msg 流：包从 Router 进来，Router 通知
// TS，TS 派 datain 任务给 DTE RV core 跑 weights loader，搬运把数据从 Router
// 落进 Matrix Mem。这一阶段不建 stream 表项、不启动任务链；各 core 数够了 SCP
// 才把 core 切到业务模式。

// 把一颗 core 配进 weights 加载阶段。任务链在这之前就配好了，切模式只动
// datain 那一项与进核那一档。
inline void EnterWeightsMode(Core& core) {
  core.GetTs().Cfg().WriteDatainTask(SymbolOf("task_dte_weights_loader", "dte"),
                                     /*weights_mode=*/true);
  core.GetTs().Cfg().SetTriggerChainEn(false);
  core.GetTs().Cfg().SetInitFinish();
  core.GetDte().Tables().PreloadPathTask(kWeightsPath, 0);
  core.SetWeightsInbound();
}

// 切回业务：datain 改指 token 搬移那一笔，任务链放行，进核那一笔落回 Core Mem。
inline void EnterBusinessMode(Core& core) {
  core.GetTs().Cfg().WriteDatainTask(SymbolOf("task_dte_user_init", "dte"),
                                     /*weights_mode=*/false);
  core.GetTs().Cfg().SetTriggerChainEn(true);
  core.GetTs().Cfg().SetInitFinish();
  core.SetBusinessInbound();
}

// weights 那条 path 的路由表：树形与 token 那条一样，从 enter_port 进来沿本行
// 往右铺满，再经 mid 到另一行往右铺满；差别在每一跳都开 path_core_mask，包走遍
// 整棵树，只在自己那一位是 1 的 core 上落地。
inline void WireWeightsPath(Chip& chip, ChipShape shape, uint64_t enter_port) {
  if (!PortSitsOnCompute(shape, enter_port)) {
    chip.GetCore(CoreOfPort(shape, enter_port))
        .GetRouter()
        .Preload(kWeightsPath,
                 PassThrough(FlowOf(OppositeOf(DirOfPort(enter_port)))));
  }
  uint64_t head = SlotOfPort(enter_port) / kCols * kCols;
  uint64_t rows[2] = {head, head ^ kCols};
  for (uint64_t r = 0; r < 2; ++r) {
    for (uint64_t j = 0; j < kCols; ++j) {
      uint64_t slot = rows[r] + j;
      uint64_t flow = j + 1 < kCols ? uint64_t(kFlowRight) : 0;
      // 进来那个槽位还要往另一行发一份。
      if (r == 0 && j == 0) flow |= kFlowMid;
      RouteEntry e = EnterAndSpread(flow);
      e.path_core_mask_enable = true;
      e.path_core_mask_idx = slot;
      chip.GetCore(CoreOfSlot(shape, slot))
          .GetRouter()
          .Preload(kWeightsPath, e);
    }
  }
}

// 一个权重包：payload 是某一片权重里的一段，落点是 Matrix Mem 上的最终地址，
// mask 挑收它的那个 core。
inline MessagePtr MakeWeightsMsg(uint64_t slot, uint64_t at,
                                 std::vector<uint8_t> bytes) {
  auto m = std::make_shared<Message>();
  m->path_id = kWeightsPath;
  m->path_core_mask = 1ull << slot;
  m->user_id = kWeightsUser;
  m->gpu_id = 2;
  m->size = bytes.size();
  m->dst_addr = at;
  m->payload = std::move(bytes);
  return m;
}

inline MessagePtr MakeToken(SpreadCase const& want) {
  auto m = std::make_shared<Message>();
  m->path_id = kInPath;
  m->user_id = kUserId;
  m->gpu_id = 2;
  m->token_id = 1;
  m->size = want.token.size();
  m->compute = 1;
  m->stream_id = 0;
  m->task_id = 0;
  m->payload = want.token;
  return m;
}

// 注入与收取各用一座桥当接头。这两段不是实际存在的链路，延迟填 0。
inline C2cCfg StubCfg() {
  C2cCfg c;
  c.axi_latency = 0;
  return c;
}

// 两颗 chip 对接的一条 C2C：一侧的 TakeOut 喂给另一侧的 PushIn，两个方向都要。
struct ChipPair {
  uint64_t a = 0, a_port = 0, b = 0, b_port = 0;
};

// 几颗 chip、两座接头桥与它们之间的段搬运，全由这一个协程驱动。出口上的 flit
// 是单拍脉冲，分到两个协程里谁先跑不定，会整拍错过。
class SpreadHarness : public BachModule {
 public:
  SpreadHarness(ClockPtr c, std::vector<Chip*> target,
                std::vector<ChipPair> pairs, C2cBridge& feed, C2cBridge& sink,
                uint64_t feed_chip, uint64_t feed_port, uint64_t sink_chip,
                uint64_t sink_port, std::vector<uint64_t> run_order,
                uint64_t at, uint64_t cap, MessagePtr m)
      : BachModule(c, "harness"), chips(std::move(target)),
        links(std::move(pairs)), in_stub(feed), out_stub(sink),
        in_chip(feed_chip), in_port(feed_port), out_chip(sink_chip),
        out_port(sink_port), order(std::move(run_order)), fire_at(at),
        limit(cap), msg(std::move(m)) {}

  std::vector<MessagePtr> out_msgs;
  // 停钟那一拍。一个 token 从进入口到结果出口一共走了多少拍。
  uint64_t stopped_at = 0;
  // ── weights 加载那一段。留空就只跑业务那一段 ──
  // 包按 weights_gap 拍一个注进去，注完等 weights_done() 说各 core 都收够，调
  // to_business() 切模式，再过一个 gap 发 token。
  std::vector<MessagePtr> weights;
  uint64_t weights_gap = 0;
  std::function<bool()> weights_done;
  std::function<void()> to_business;
  // 切进业务模式、同时把 token 发出去的那一拍。
  uint64_t switched_at = 0;
  // 每个 core 的任务链都走空了才算这一笔真的走完。core 各占协程时这个数只能读
  // 打拍的那一份：head_ptr 与 tail_ptr 是 stream_table 自己 Step() 里改的普通
  // 成员，别的协程读它就是跨协程读非 atomic 的量。
  std::vector<uint64_t> inflight;

 protected:
  void Step() override {
    // 结果还没出来就不必挨个问：判停要两件事同时成立，先看便宜的那件。
    bool all_empty = false;
    if (!out_msgs.empty()) {
      inflight.clear();
      all_empty = true;
      for (Chip* c : chips) {
        ChipShape shape = c->Shape();
        for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
          uint64_t n = c->GetCore(CoreOfSlot(shape, slot)).GetTs().Table()
                           .InFlight();
          inflight.push_back(n);
          if (n != 0) all_empty = false;
        }
      }
    }

    LinkEndPtr feed_in = in_stub.FromCore();
    MessagePtr fire = Pick();
    if (fire) {
      feed_in->flit.Drive(/*vc=*/0, /*is_head=*/true, /*is_tail=*/true,
                          fire->size, fire);
    } else {
      feed_in->flit.Idle();
    }
    feed_in->release.Idle();
    out_stub.FromCore()->flit.Idle();
    out_stub.FromCore()->release.Idle();

    // 末级先做：收取那一头在最外，注入那一头在最里。
    out_stub.RunStep();
    for (uint64_t i : order) chips[i]->RunOutside();
    in_stub.RunStep();

    Move(in_stub, chips[in_chip]->Port(in_port));
    for (ChipPair const& p : links) {
      Move(chips[p.a]->Port(p.a_port), chips[p.b]->Port(p.b_port));
    }
    Move(chips[out_chip]->Port(out_port), out_stub);

    FlitView f = ReadFlit(out_stub.ToCore()->flit);
    if (f.valid && f.msg) out_msgs.push_back(f.msg);

    // 结果收到、所有 core 的任务链都走空就停表。拍数上限只作兜底：卡住时要
    // 停得下来，跑通时不必空转。
    if ((!out_msgs.empty() && all_empty) || CycleNow() >= limit) {
      stopped_at = CycleNow();
      clk->Stop();
    }
  }

 private:
  // 这一拍往里注哪个包。weights 那一段一个一个来，注完等各 core 收够，切模式，
  // 再发 token。
  MessagePtr Pick() {
    uint64_t now = CycleNow();
    if (weights.empty()) return now == fire_at ? msg : MessagePtr();
    LOGCHECK(weights_gap != 0, "SpreadHarness: weights 那一段要给注入间隔。");
    if (now < fire_at) return MessagePtr();
    if (sent < weights.size()) {
      if ((now - fire_at) % weights_gap != 0) return MessagePtr();
      return weights[sent++];
    }
    if (switched_at == 0) {
      // 各 core 都收够了 loader 才中断 SCP。计数是 loader 写的，那一笔搬运还
      // 在路上，所以再留一个 gap 给它落完，也是 SCP 改配置的那段时间。
      if (weights_done && !weights_done()) return MessagePtr();
      switched_at = now + weights_gap;
      return MessagePtr();
    }
    if (now != switched_at) return MessagePtr();
    if (to_business) to_business();
    return msg;
  }

  static void Move(C2cBridge& x, C2cBridge& y) {
    while (x.HasOut()) y.PushIn(x.TakeOut());
    while (y.HasOut()) x.PushIn(y.TakeOut());
  }

  std::vector<Chip*> chips;
  std::vector<ChipPair> links;
  C2cBridge& in_stub;
  C2cBridge& out_stub;
  uint64_t in_chip, in_port, out_chip, out_port;
  std::vector<uint64_t> order;
  uint64_t fire_at, limit;
  MessagePtr msg;
  uint64_t sent = 0;
};

// 收到的那一份与参考实现逐 bit 对。
inline void CheckResult(SpreadCase const& want,
                 std::vector<std::vector<uint8_t>> const& landed,
                 std::vector<uint64_t> const& inflight,
                 std::vector<MessagePtr> const& got) {
  // 先看各 core 自己算出来的那一份部分和：错在这一层就不是归约的事。
  for (uint64_t g = 0; g < want.Total(); ++g) {
    ASSERT_EQ(landed[g].size(), want.core_bits[g].size() * 4);
    for (uint64_t j = 0; j < want.core_bits[g].size(); ++j) {
      uint32_t b = 0;
      for (int t = 0; t < 4; ++t) b |= uint32_t(landed[g][j * 4 + t]) << (8 * t);
      EXPECT_EQ(b, want.core_bits[g][j])
          << "第 " << g << " 个 core 的部分和第 " << j << " 个";
    }
  }
  for (uint64_t g = 0; g < inflight.size(); ++g) {
    EXPECT_EQ(inflight[g], 0u) << "第 " << g << " 个 core 的任务链没走到头";
  }
  ASSERT_FALSE(got.empty()) << "出口上一个包都没有";
  MessagePtr result = got.front();
  ASSERT_EQ(result->payload.size(),
            kReduceSwHeaderBytes + want.out_bits.size() * 4);
  for (uint64_t j = 0; j < want.out_bits.size(); ++j) {
    uint32_t b = 0;
    for (int t = 0; t < 4; ++t) {
      b |= uint32_t(result->payload[kReduceSwHeaderBytes + j * 4 + t])
           << (8 * t);
    }
    EXPECT_EQ(b, want.out_bits[j]) << "归约结果第 " << j << " 个";
  }
}

// ── R core：EP 组之间那一层 ──

// 广播树：从 chip0 的 W 口进来，第 0 层往右铺满，同时从 chip0 往下到第 1 层的
// 头一颗，那一层再往右铺满。每颗 chip 的进口与还要往外发的口。
struct BcastPlan {
  uint64_t enter_port;
  std::vector<uint64_t> out_port;
};

// 与 compiler/kernel/bach.h 的 RC_* 同源。
constexpr uint64_t kRcHalfBytes = 0x6100;
constexpr uint64_t kRcFlagOff = 0x0000;
constexpr uint64_t kRcSumOff = 0x0000;

// R core 的两条链。链二从 task 0 起，链一是单独配的 datain_task。
inline void WriteRcoreChains(Core& core, uint64_t out_path) {
  TaskEntry find;
  find.send_unit = SendUnit::kMu;
  find.recv_unit = RecvUnit::kRvOnly;
  find.self_start = true;
  find.exe_mask = true;
  find.task_pc = SymbolOf("task_rc_find", "mu");
  core.GetTs().Cfg().WriteTask(0, find);

  TaskEntry load;
  load.send_unit = SendUnit::kDte;
  load.recv_unit = RecvUnit::kDsa;
  load.dsa_en = true;
  load.exe_mask = true;
  load.task_pc = SymbolOf("task_dte_rc_load", "dte");
  core.GetTs().Cfg().WriteTask(1, load);

  TaskEntry add;
  add.send_unit = SendUnit::kVu;
  add.recv_unit = RecvUnit::kRvOnly;
  add.exe_mask = true;
  add.task_pc = SymbolOf("task_vu_add", "vu");
  core.GetTs().Cfg().WriteTask(2, add);

  TaskEntry send;
  send.send_unit = SendUnit::kDte;
  send.recv_unit = RecvUnit::kDsa;
  send.dsa_en = true;
  send.end = true;
  send.exe_mask = true;
  send.path_id = out_path;
  send.task_pc = SymbolOf("task_dte_rc_send", "dte");
  core.GetTs().Cfg().WriteTask(3, send);

  core.GetTs().Cfg().WriteDatainTask(SymbolOf("task_dte_rc_datain", "dte"),
                                     /*weights_mode=*/false);
  core.GetTs().Cfg().SetCoreType(CoreType::kReduction);
  // B core 与 R core 的 stream_num 配 16，自启动数因此也是 16（ts.md F84）。
  core.GetTs().Cfg().SetStreamNum(kStreamNum);
  core.GetTs().Cfg().SetInitFinish();
  core.GetTs().SelfStart();
}

// ── B core：组内广播的发起点 ──
//
// 与 compiler/kernel/bach.h 的 BC_* 同源，改一处要一起改。
constexpr uint64_t kBcSlots = 16;
constexpr uint64_t kBcTokenBytes = 6144 * 2;
constexpr uint64_t kBcMmBase = 0x000000;
constexpr uint64_t kBcFlagOff = 0x0500;
// token 进 chip 走这一条，与广播出去那一条分开：坐在进来那个口上的 core 只把它
// 往 B core 转，不能与广播树上“往右铺”的那一条撞在同一个 path 上。
constexpr uint64_t kBcastInPath = 5;

// 第几笔落在 B core 的 Matrix Mem 哪里。发方按送出的笔数算，B core 按收下的笔
// 数算，同一条规则。
inline uint64_t BcoreLand(uint64_t seq) {
  return kBcMmBase + (seq % kBcSlots) * kBcTokenBytes;
}

// B core 的两条链。链二从 task 0 起，链一是单独配的 datain_task。out_path 是广
// 播出去用的 path，下游 core 按同一个号查自己的路由表；dirs 是往哪几个方向广
// 播，TS 下发搬出之前查的就是这几个方向的下游资源。next_path 给的是往下一个
// EP 组的 B core 转那一笔用的 path，链尾那一组不转，传 0。
inline void WriteBcoreChains(Core& core, uint64_t out_path, uint64_t dirs,
                             uint64_t next_path = 0) {
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
  send.end = next_path == 0;
  send.exe_mask = true;
  send.credit_en = true;
  send.path_id = out_path;
  send.task_pc = SymbolOf("task_dte_bc_send", "dte");
  core.GetTs().Cfg().WriteTask(1, send);

  if (next_path != 0) {
    TaskEntry relay;
    relay.send_unit = SendUnit::kDte;
    relay.recv_unit = RecvUnit::kDsa;
    relay.dsa_en = true;
    relay.end = true;
    relay.exe_mask = true;
    relay.path_id = next_path;
    relay.task_pc = SymbolOf("task_dte_bc_relay", "dte");
    core.GetTs().Cfg().WriteTask(2, relay);
  }

  core.GetTs().Cfg().WriteDatainTask(SymbolOf("task_dte_bc_datain", "dte"),
                                     /*weights_mode=*/false);
  core.GetTs().Cfg().SetCoreType(CoreType::kBroadcast);
  core.GetTs().Cfg().SetBCoreDirection(dirs);
  core.GetTs().Cfg().SetStreamNum(kStreamNum);
  core.GetTs().Cfg().SetInitFinish();
  core.GetTs().SelfStart();
}

// B core 起头的广播树：token 从 enter_port 进来，坐在那个口上的 core 不派角色，
// 只把它转给同一列的 B core；B core 留一份在 Matrix Mem，再往右与往下各发一
// 路，两行各自往右铺满。第一列 chip 的 B core 是 core0，不派角色的那个是 core5，
// 两者同在第 0 列。
inline void WireBcoreBroadcast(Chip& chip, uint64_t enter_port,
                               std::vector<uint64_t> const& out_port = {}) {
  constexpr ChipShape kShape = ChipShape::kFirst;
  constexpr uint64_t kBcore = 0;
  constexpr uint64_t kSpare = 5;
  ASSERT_EQ(chip.Shape(), kShape) << "B core 只在第一列形状的 chip 上";
  // 进来那一路：口上那个 core 往 B core 的方向转一跳，B core 收下不再往下传。
  chip.GetCore(CoreOfPort(kShape, enter_port))
      .GetRouter()
      .Preload(kBcastInPath, PassThrough(kFlowMid));
  chip.GetCore(kBcore).GetRouter().Preload(kBcastInPath, EnterAndSpread(0));

  // 广播出去那一路：B core 自己不收，往右给本行第一个计算 core，往下给另一行
  // 那个只转发的 core。
  chip.GetCore(kBcore)
      .GetRouter()
      .Preload(kInPath, PassThrough(kFlowRight | kFlowMid));
  chip.GetCore(kBcore).GetDte().Tables().PreloadRtab(
      kInPath, PassThrough(kFlowRight | kFlowMid));
  chip.GetCore(kSpare).GetRouter().Preload(kInPath, PassThrough(kFlowRight));

  // 两行的计算 core：收下并往右接着传。行末那个往哪个 chip 口发由 out_port 给
  // 出，没给就到此为止。
  for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
    uint64_t flow = 0;
    if ((slot % kCols) + 1 < kCols) {
      flow = kFlowRight;
    } else {
      for (uint64_t p : out_port) {
        if (SlotOfPort(p) == slot) flow |= FlowOf(DirOfPort(p));
      }
    }
    chip.GetCore(CoreOfSlot(kShape, slot))
        .GetRouter()
        .Preload(kInPath, EnterAndSpread(flow));
  }
  // 口上那个 core 不是计算 core 时，出去还要多一跳转发。
  for (uint64_t p : out_port) {
    if (PortSitsOnCompute(kShape, p)) continue;
    chip.GetCore(CoreOfPort(kShape, p))
        .GetRouter()
        .Preload(kInPath, PassThrough(FlowOf(DirOfPort(p))));
  }
}

// 落进 R core 的那一条：不归约，只交给本 core。
inline RouteEntry LandInRcore() {
  RouteEntry e;
  e.flow_dir = 0;
  e.path_core_bypass = false;
  e.stream_table_enable = false;
  e.operation = Operation::kForward;
  return e;
}
}  // namespace moetest
}  // namespace bach
}  // namespace latch

#endif
