// 一个 core 上那一段 MoE 的数值：按 EP6+TP8 的 KN 拆分，槽位 s 的 FC1、FC3 部分
// 和，silu·dot·量化，FC2 第 s 段，全用 MXFP8 输入，结果与 reference/ 那一份逐 bit
// 相同。
//
// 前两份只走计算通路，不经 TS 与 RV core：MU 任务与门控那几条 VU 宏指令由这里
// 按顺序下发。第三份装 kernel、走任务链：这个 core 当 dot core 用，自己一个 core
// 就是整条 chip 内归约链，部分和归约给自己，FC2 那一段写进 concat 区后沿行链出核。
//
// 一个 core 的权重约 1.2 MiB，比对向量里只给种子，两侧按同一个规则逐 tile 生成。

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/common/numeric/formats.h"
#include "bach/ip/chip/core/core.h"
#include "bach/ip/chip/core/memory/core_mem.h"
#include "bach/ip/chip/core/memory/matrix_mem.h"
#include "bach/ip/chip/core/mu/mu.h"
#include "bach/ip/chip/core/vu/vu.h"
#include "test/bach/ip/chip/kn_data.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

// ── 比对向量 ──

struct MoeCase {
  uint64_t group = 0, chip = 0, slot = 0;
  std::vector<uint8_t> token, token_scale, part, fc2;
  std::vector<std::vector<uint8_t>> act, act_scale;
};

std::vector<uint8_t> HexBytes(std::string const& s) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    v.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
  }
  return v;
}

MoeCase ReadMoeCase() {
  MoeCase c;
  std::ifstream f(std::string(LATCH_SOURCE_DIR) +
                  "/src/bach/compiler/reference/vectors/moe.txt");
  std::string line;
  std::map<std::string, std::string> kv;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    size_t sp = line.find(' ');
    if (sp == std::string::npos) continue;
    kv[line.substr(0, sp)] = line.substr(sp + 1);
  }
  if (kv.count("part") == 0) return c;
  c.group = std::stoull(kv["group"]);
  c.chip = std::stoull(kv["chip"]);
  c.slot = std::stoull(kv["slot"]);
  c.token = HexBytes(kv["token"]);
  c.token_scale = HexBytes(kv["token_scale"]);
  c.part = HexBytes(kv["part"]);
  c.fc2 = HexBytes(kv["fc2"]);
  for (uint64_t e = 0; e < kn::kExperts; ++e) {
    c.act.push_back(HexBytes(kv["act" + std::to_string(e)]));
    c.act_scale.push_back(HexBytes(kv["act_scale" + std::to_string(e)]));
  }
  return c;
}

// topK 表在数据线里的样子：每项 {local_ep_index 2 B, weight 4 B}。
std::vector<uint8_t> TopkBytes(std::vector<TopkEntry> const& t) {
  std::vector<uint8_t> b(kTopkBytesPerStream, 0);
  for (size_t i = 0; i < t.size(); ++i) {
    uint64_t at = i * kTopkEntryBytes;
    b[at] = uint8_t(t[i].local_ep_index & 0xFFu);
    b[at + 1] = uint8_t((t[i].local_ep_index >> 8) & 0xFFu);
    uint32_t w = numeric::BitsOf(t[i].weight);
    for (int k = 0; k < 4; ++k) b[at + 2 + k] = uint8_t((w >> (8 * k)) & 0xFFu);
  }
  return b;
}

// topK 里第 0 个是组内第 1 个专家，第 1 个是组内第 0 个；topK 直接存组内序号。
constexpr uint64_t kLocal[kn::kExperts] = {1, 0};

// 一个 core 要的那几样：token、topK 表、三个矩阵的那一片。
template <typename Cmem, typename Mmem>
void PokeCoreData(Cmem& cm, Mmem& mm, Mu& mu, MoeCase const& want) {
  cm.Poke(kn::kTokenOff, want.token);
  cm.PokeScale(kn::kTokenOff, want.token_scale);
  // topK 表由 DTE 搬运时经专用数据线直接写进 MU 的 topK_ep_table，不落 Core Mem；
  // 直接存组内序号。这几笔 MU 任务都走 stream 0。
  mu.EpInfo().WriteTopk(
      0, TopkBytes({{kLocal[0], kn::kWep[0]}, {kLocal[1], kn::kWep[1]}}));
  kn::PokeCoreWeights(mm, want.group, want.chip, want.slot, kLocal);
}

// ── VU 那一段：silu·dot·量化 ──
//
// 三条宏指令，经 VRF 中转：VU 的执行链是 VALU 在 VSFU 前面，一条里做不完「先取
// sigmoid 再乘回去」；而且一条宏指令只有一路 LU，fc1 与 fc3 两个向量也进不来。
//
//   一  LU 读 fc1（BF16）→ VSFU 出 sigmoid → 写 VRF 第 0 项
//   二  LU 读 fc1（BF16）→ VALU0 乘 VRF 第 0 项 → 写 VRF 第 8 项（silu）
//   三  LU 读 fc3（BF16）→ VALU0 乘 VRF 第 8 项 → SU 量化成 MXFP8 写回
//
// 第 8 项不是第 1 项：一条 VL=256 的 FP32 向量占 8 个 entry，索引是 entry 号。
constexpr uint64_t kVrfSig = 0;
constexpr uint64_t kVrfGate = 8;

uint64_t OpWord(uint64_t opcode, uint64_t src1 = 0, uint64_t src2 = 0) {
  return opcode | (src1 << 8) | (src2 << 16);
}

uint64_t TypeVlWord(uint64_t vl) {
  return (vl & kVuVlMask) | (uint64_t(numeric::RoundMode::kRne)
                             << kVuRoundModeShift);
}

// 三组静态配置写进一个队列。几个专家共用，地址每条走动态副本。
void GateSetup(std::deque<std::pair<uint64_t, uint64_t>>& q) {
  auto st = [&](uint64_t group, uint64_t off, uint64_t data) {
    q.push_back({kVuStaticBase + group * kVuStaticStride + off, data});
  };
  uint64_t vl = kn::kSegInter;
  st(0, kVuLuOp, OpWord(uint64_t(LuOp::kLdBf16)));
  st(0, kVuVsfuOp, OpWord(uint64_t(VsfuOp::kSigmoid), kSrcLu));
  st(0, kVuSuOp, OpWord(uint64_t(SuOp::kNop)));
  st(0, kVuPrfOp, kSrcVsfu0);
  st(0, kVuStaticDupOffset + kVuVrfWtIndex, kVrfSig);
  st(0, kVuStaticDupOffset + kVuTypeVl, TypeVlWord(vl));

  st(1, kVuLuOp, OpWord(uint64_t(LuOp::kLdBf16)));
  st(1, kVuValu0Op, OpWord(uint64_t(ValuOp::kFmulVv), kSrcLu, kSrcVrfP0));
  st(1, kVuSuOp, OpWord(uint64_t(SuOp::kNop)));
  st(1, kVuPrfOp, kSrcValu0);
  st(1, kVuStaticDupOffset + kVuVrfRdIndex, kVrfSig);
  st(1, kVuStaticDupOffset + kVuVrfWtIndex, kVrfGate);
  st(1, kVuStaticDupOffset + kVuTypeVl, TypeVlWord(vl));

  st(2, kVuLuOp, OpWord(uint64_t(LuOp::kLdBf16)));
  st(2, kVuValu0Op, OpWord(uint64_t(ValuOp::kFmulVv), kSrcLu, kSrcVrfP0));
  st(2, kVuSuOp, OpWord(uint64_t(SuOp::kStMxfp8), kSrcValu0));
  st(2, kVuPrfOp, 0);
  st(2, kVuStaticDupOffset + kVuVrfRdIndex, kVrfGate);
  st(2, kVuStaticDupOffset + kVuTypeVl, TypeVlWord(vl));
}

// 一个专家那三条：fc1 与 fc3 从 red 那一包的数据段读，act 写 FC2 输入那一处。
// 三条之间是 VRF 上的先后，VU 只查 Core Mem 的地址重叠，所以逐条置 fence。
void GateFire(std::deque<std::pair<uint64_t, uint64_t>>& q, uint64_t red,
              uint64_t e) {
  uint64_t fc1 = red + e * kn::kPartStride;
  uint64_t fc3 = red + (kn::kExperts + e) * kn::kPartStride;
  uint64_t act = kn::kActOff + e * kn::kActStride;
  uint64_t const rd[3] = {fc1, fc1, fc3};
  for (uint64_t g = 0; g < 3; ++g) {
    q.push_back({kVuLdAddr, rd[g]});
    q.push_back({kVuStAddr, act});
    q.push_back({kVuMacroInstTrigger, kVuMaskLdAddr | kVuMaskStAddr |
                                          (g << kVuTrigCfgIdxShift) |
                                          kVuTrigFence});
  }
}

// 按序把配置写发进去，看见 ready 才换下一笔。
void DriveOne(std::deque<std::pair<uint64_t, uint64_t>>& que, DsaCfgPort& port,
              bool& busy, uint64_t& sq) {
  if (busy) {
    if (!port.Ready()) return;
    que.pop_front();
    busy = false;
  }
  if (que.empty()) {
    port.Idle();
    return;
  }
  port.Drive(que.front().first, que.front().second, ++sq);
  busy = true;
}

// 一笔 MU 任务要写的那几个寄存器。
struct MuJob {
  uint64_t mode = 0;   // primitive_mode 的精度与类型位；专家数、reduce、task_last 由 Submit 拼
  uint64_t kblock = 1, nblock = 1;
  uint64_t a_addr = 0, b_addr = 0, c_addr = 0, ac_stride = 0;
  bool ep_reduce = false;
};

void Submit(std::deque<std::pair<uint64_t, uint64_t>>& q, MuJob const& j) {
  uint64_t mode = j.mode | (kn::kExperts << kMuRouterExpertCountShift);
  if (j.ep_reduce) mode |= kMuRouterEpReduceEn;
  mode |= kMuTaskLast;   // 单 core 用例三笔各自报一次 dsa_done，全置 task_last
  q.push_back({kMuPrimitiveMode, mode});
  q.push_back({kMuPrimitiveDim, j.nblock | (j.kblock << 16)});
  q.push_back({kMuAAddr, j.a_addr});
  q.push_back({kMuBAddr, j.b_addr});
  q.push_back({kMuCAddr, j.c_addr});
  // AC_expert_stride 拆两侧：FC2 用 token 那侧，FC1/FC3 用 output 那侧，不用的写 0。
  q.push_back({kMuAcExpertStride,
               j.ep_reduce ? (j.ac_stride << kMuTokenExpertStrideShift)
                           : (j.ac_stride << kMuOutputExpertStrideShift)});
  q.push_back({kMuBExpertStride, kn::kMmStride});
  q.push_back({kMuTaskTrigger, kMuTriggerValid});
}

constexpr uint64_t kFc13Mode =
    (1u << kMuADataTypeShift) |        // A=MXFP8
    (1u << kMuCDataTypeShift) |        // C=BF16
    (1u << kMuRouterEpDtypeShift);     // B=MXFP8，primitive_type = 0
constexpr uint64_t kFc2Mode = kFc13Mode | (1u << kMuPrimTypeShift);  // 1×K64×N128

// FC1、门控、FC2 按顺序下发。MU、VU、两块存储与两条配置通路都由这一个协程驱动。
class MoeRig : public BachModule {
 public:
  MoeRig(ClockPtr c, Mu& m, Vu& v, CoreMem& cm, MatrixMem& mm, uint64_t slot,
         std::shared_ptr<DsaCfgPort> mu_cfg, std::shared_ptr<DsaCfgPort> vu_cfg)
      : BachModule(c, "rig"), mu(m), vu(v), cmem(cm), mmem(mm), s(slot),
        cfg(std::move(mu_cfg)), vcfg(std::move(vu_cfg)) {}

  uint64_t dones = 0, vdones = 0;
  bool finished = false;

 protected:
  void Step() override {
    if (mu.Done().Valid()) ++dones;
    if (vu.Done().Valid()) ++vdones;
    Advance();
    DriveOne(q, *cfg, driving, seq);
    DriveOne(vq, *vcfg, vdriving, vseq);
    mu.RunStep();
    vu.RunStep();
    cmem.RunStep();
    mmem.RunStep();
  }

 private:
  void Advance() {
    uint64_t token = kn::kTokenOff + s * kn::kSegEmbed;
    uint64_t kb = kn::kSegEmbed / kn::kFc13K, nb = kn::kSegInter / kn::kFc13N;
    if (stage == 0) {
      GateSetup(vq);
      Submit(q, {kFc13Mode, kb, nb, token, kn::kMmW1, kn::kFc1Off,
                 kn::kPartStride, false});
      stage = 1;
      return;
    }
    if (stage == 1 && dones >= 1) {
      Submit(q, {kFc13Mode, kb, nb, token, kn::kMmW3, kn::kFc3Off,
                 kn::kPartStride, false});
      stage = 2;
      return;
    }
    if (stage == 2 && dones >= 2) {
      // 这里没有归约：门控直接读本 core 自己的部分和。
      for (uint64_t e = 0; e < kn::kExperts; ++e) GateFire(vq, kn::kFc1Off, e);
      stage = 3;
      return;
    }
    // 每个专家三条宏指令各报一次完成。真实链路里由 RV core 轮询
    // macro_inst_left 收尾，这一份直接数。
    if (stage == 3 && vdones >= 3 * kn::kExperts) {
      Submit(q, {kFc2Mode, kn::kSegInter / kn::kFc2K,
                 kn::kSegEmbed / kn::kFc2N, kn::kActOff, kn::kMmW2,
                 kn::ConcatOf(s), kn::kActStride, true});
      stage = 4;
      return;
    }
    if (stage == 4 && dones >= 3) finished = true;
  }

  Mu& mu;
  Vu& vu;
  CoreMem& cmem;
  MatrixMem& mmem;
  uint64_t s;
  std::shared_ptr<DsaCfgPort> cfg, vcfg;
  std::deque<std::pair<uint64_t, uint64_t>> q, vq;
  bool driving = false, vdriving = false;
  uint64_t seq = 0, vseq = 0, stage = 0;
};

}  // namespace

// 一个 core 上的整段：MU 算槽位 s 的 FC1 与 FC3 部分和（两个专家各出一份），
// VU 做 silu·dot·量化，MU 算 FC2 第 s 段并把两个专家按 topK 权重合并成一份。三处
// 中间量都与参考实现逐 bit 相同。
TEST(BachMoe, OneCoreMatchesReference) {
  MoeCase want = ReadMoeCase();
  ASSERT_FALSE(want.part.empty())
      << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";

  std::vector<uint8_t> part, fc2;
  std::vector<std::vector<uint8_t>> act, act_scale;
  uint64_t dones = 0, vdones = 0;
  bool finished = false;
  {
    EnsureSlots();
    RT::GetRecorder().StartNew("moe_one_core");
    ClockPtr clk = MakeClock(0, kPeriod);
    Mu mu(clk, "mu", MuCfg{});
    Vu vu(clk, "vu");
    CoreMem cmem(clk, "cmem", 0, false);
    MatrixMem mmem(clk, "mmem", 0, false);

    auto mu_cfg = std::make_shared<DsaCfgPort>(clk);
    mu.AttachCfg(mu_cfg);
    mu.AttachCmemRd(cmem.PortPtr(kCmemMuRd));
    mu.AttachCmemWr(cmem.PortPtr(kCmemMuWr));
    mu.AttachMmemRd(mmem.PortPtr(kMmemMu));
    auto vu_cfg = std::make_shared<DsaCfgPort>(clk);
    vu.AttachCfg(vu_cfg);
    vu.AttachCmemLd(cmem.PortPtr(kCmemVuRd));
    vu.AttachCmemSt(cmem.PortPtr(kCmemVuWr));
    vu.ConfigRegister().SetCoreIds(0, 4);

    PokeCoreData(cmem, mmem, mu, want);

    MoeRig rig(clk, mu, vu, cmem, mmem, want.slot, mu_cfg, vu_cfg);
    clk->Continue(200000 * kPeriod);
    RT::JoinAll();
    dones = rig.dones;
    vdones = rig.vdones;
    finished = rig.finished;
    part = cmem.Peek(kn::kFc1Off, kn::kPartBytes - kn::kSwHead);
    for (uint64_t e = 0; e < kn::kExperts; ++e) {
      act.push_back(cmem.Peek(kn::kActOff + e * kn::kActStride, kn::kSegInter));
      act_scale.push_back(cmem.PeekScale(kn::kActOff + e * kn::kActStride,
                                         kn::kSegInter / 32));
    }
    fc2 = cmem.Peek(kn::ConcatOf(want.slot), kn::kFc2Bytes);
  }
  RT::FlushRecorder();
  RT::Reset();

  EXPECT_EQ(dones, 3u) << "三笔 MU 任务应当各报一次完成";
  EXPECT_EQ(vdones, 3u * kn::kExperts) << "门控每条宏指令各报一次完成";
  EXPECT_TRUE(finished);
  EXPECT_EQ(part, want.part) << "FC1、FC3 部分和";
  for (uint64_t e = 0; e < kn::kExperts; ++e) {
    EXPECT_EQ(act[e], want.act[e]) << "第 " << e << " 个专家的 FC2 输入";
    EXPECT_EQ(act_scale[e], want.act_scale[e])
        << "第 " << e << " 个专家的 FC2 输入的 scale";
  }
  EXPECT_EQ(fc2, want.fc2) << "FC2 那一段";
}

namespace {

// VU 与存储、配置通路都由这一个协程驱动。
class GateRig : public BachModule {
 public:
  GateRig(ClockPtr c, Vu& target, CoreMem& mem,
          std::shared_ptr<DsaCfgPort> cfg_port)
      : BachModule(c, "gate"), vu(target), cmem(mem), cfg(std::move(cfg_port)) {}

  uint64_t dones = 0;
  std::deque<std::pair<uint64_t, uint64_t>> q;

 protected:
  void Step() override {
    if (vu.Done().Valid()) ++dones;
    DriveOne(q, *cfg, driving, seq);
    vu.RunStep();
    cmem.RunStep();
  }

 private:
  Vu& vu;
  CoreMem& cmem;
  std::shared_ptr<DsaCfgPort> cfg;
  bool driving = false;
  uint64_t seq = 0;
};

}  // namespace

// silu·dot·量化这一步交给 VU 真的算一遍：部分和按 dot core 上归约结果那一包的
// 摆法铺好，三条宏指令走完，写回 Core Mem 的那一份与它的 scale 与参考实现逐 bit
// 相同。
TEST(BachMoe, VuGateMatchesReference) {
  MoeCase want = ReadMoeCase();
  ASSERT_FALSE(want.part.empty())
      << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";

  std::vector<std::vector<uint8_t>> act, act_scale;
  uint64_t dones = 0;
  {
    EnsureSlots();
    RT::GetRecorder().StartNew("moe_vu_gate");
    ClockPtr clk = MakeClock(0, kPeriod);
    Vu vu(clk, "vu");
    CoreMem cmem(clk, "cmem", 0, false);
    auto cfg_port = std::make_shared<DsaCfgPort>(clk);
    vu.AttachCfg(cfg_port);
    vu.AttachCmemLd(cmem.PortPtr(kCmemVuRd));
    vu.AttachCmemSt(cmem.PortPtr(kCmemVuWr));
    vu.ConfigRegister().SetCoreIds(0, 4);

    // 只有自己一份分量的归约原样出来，所以归约结果那一包的数据段就是部分和。
    uint64_t red = kn::kRedOff + kn::kSwHead;
    cmem.Poke(red, want.part);

    GateRig rig(clk, vu, cmem, cfg_port);
    GateSetup(rig.q);
    for (uint64_t e = 0; e < kn::kExperts; ++e) GateFire(rig.q, red, e);
    clk->Continue(40000 * kPeriod);
    RT::JoinAll();
    dones = rig.dones;
    for (uint64_t e = 0; e < kn::kExperts; ++e) {
      act.push_back(cmem.Peek(kn::kActOff + e * kn::kActStride, kn::kSegInter));
      act_scale.push_back(cmem.PeekScale(kn::kActOff + e * kn::kActStride,
                                         kn::kSegInter / 32));
    }
  }
  RT::FlushRecorder();
  RT::Reset();

  EXPECT_EQ(dones, 3u * kn::kExperts) << "每条宏指令各报一次完成";
  for (uint64_t e = 0; e < kn::kExperts; ++e) {
    EXPECT_EQ(act[e], want.act[e]) << "第 " << e << " 个专家";
    EXPECT_EQ(act_scale[e], want.act_scale[e]) << "第 " << e << " 个专家的 scale";
  }
}

namespace {

// ── dot core 的整条链 ──

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

// token 广播进来走 path 3；chip 内归约走 path 7，只有自己一个 core，链尾就是
// 自己；行链走 path 11，结果往 mid 出核。两条归约 path 都落在 VC3 上。
constexpr uint64_t kTokenPath = 3;
constexpr uint64_t kChipReducePath = 7;
constexpr uint64_t kRowPath = 11;

TaskEntry Task(SendUnit unit, RecvUnit recv, char const* name, char const* kind) {
  TaskEntry t;
  t.send_unit = unit;
  t.recv_unit = recv;
  t.task_pc = SymbolOf(name, kind);
  return t;
}

// dot core 那条链去掉广播 FC2 输入与收 concat 那几项：只有自己一个 core，没有
// 要广播给的、也没有要收的。
void WriteDotChain(Core& core, uint64_t slot) {
  std::string s = std::to_string(slot);
  TaskEntry in = Task(SendUnit::kDte, RecvUnit::kDsa, "task_dte_user_init", "dte");
  in.path_id = kTokenPath;
  in.wait_wake = true;
  core.GetTs().Cfg().WriteTask(0, in);

  core.GetTs().Cfg().WriteTask(
      1, Task(SendUnit::kMu, RecvUnit::kDsa,
              ("task_mu_part_s" + s).c_str(), "mu"));

  TaskEntry part = Task(SendUnit::kDte, RecvUnit::kDsa, "task_dte_send_part", "dte");
  part.path_id = kChipReducePath;
  part.task_type = TaskType::kReduce;
  part.credit_en = true;
  core.GetTs().Cfg().WriteTask(2, part);

  TaskEntry red = Task(SendUnit::kDte, RecvUnit::kDsa, "task_dte_user_init", "dte");
  red.path_id = kChipReducePath;
  red.wait_wake = true;
  core.GetTs().Cfg().WriteTask(3, red);

  core.GetTs().Cfg().WriteTask(
      4, Task(SendUnit::kVu, RecvUnit::kRvOnly, "task_vu_gate", "vu"));
  core.GetTs().Cfg().WriteTask(
      5, Task(SendUnit::kMu, RecvUnit::kDsa,
              ("task_mu_fc2_s" + s).c_str(), "mu"));

  TaskEntry row = Task(SendUnit::kDte, RecvUnit::kDsa, "task_dte_send_row", "dte");
  row.path_id = kRowPath;
  row.task_type = TaskType::kReduce;
  row.credit_en = true;
  row.end = true;
  core.GetTs().Cfg().WriteTask(6, row);

  core.GetTs().Cfg().WriteRouterTable(kChipReducePath, 0, kChipReducePath % kVcNum);
  core.GetTs().Cfg().WriteRouterTable(kRowPath, 0, kRowPath % kVcNum);
  core.GetTs().InitFinish();
  core.GetDte().Tables().PreloadPathTask(kTokenPath, 0);
  core.GetDte().Tables().PreloadPathTask(kChipReducePath, 3);
}

RouteEntry EnterCore() {
  RouteEntry e;
  e.flow_dir = 0;
  e.path_core_bypass = false;
  e.operation = Operation::kForward;
  return e;
}

// 只有自己一份分量的归约：flow_dir 给出往哪发，不给就交回本 core。
RouteEntry ReduceAlone(uint64_t flow) {
  RouteEntry e;
  e.op_type = OpType::kReduce;
  e.flow_dir = flow;
  e.path_core_bypass = true;
  e.reduce_in_mask = 1;
  e.operation = Operation::kReduce0;
  return e;
}

// core 里上百个模块与外面那两根线都由这一个协程驱动。出口上的 flit 是单拍脉冲，
// 分到两个协程里谁先跑不定，会整拍错过。
class ChainHarness : public BachModule {
 public:
  ChainHarness(ClockPtr c, Core& target, uint64_t at, MessagePtr m)
      : BachModule(c, "chain"), core(target), fire_at(at), msg(std::move(m)) {}

  std::vector<MessagePtr> out_msgs;
  std::array<bool, 3> took{};
  std::array<uint64_t, 3> took_vc{};
  uint64_t head = 0, tail = 0;

 protected:
  void Step() override {
    head = core.GetTs().Table().HeadPtr();
    tail = core.GetTs().Table().TailPtr();
    LinkEndPtr in = core.InWire(1);
    if (CycleNow() == fire_at && msg) {
      in->flit.Drive(0, true, true, msg->size, msg);
    } else {
      in->flit.Idle();
    }
    // 出核那一路的下游：上一拍收下的 flit，这一拍把 VC 位置还回去。
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
      if (f.tail && f.msg) out_msgs.push_back(f.msg);
    }
  }

 private:
  Core& core;
  uint64_t fire_at;
  MessagePtr msg;
};

}  // namespace

// dot core 的链跑一遍：token 带着 scale 进核，MU 算部分和，DTE 把它发进 Router
// 归约、链尾交回自己，VU 做 silu·dot·量化，MU 算 FC2 那一段写进 concat 区，行链
// 出核。Core Mem 里的中间量与出口上收到的那一包都与参考实现逐 bit 相同。
TEST(BachMoe, DotCoreChainMatchesReference) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  MoeCase want = ReadMoeCase();
  ASSERT_FALSE(want.part.empty())
      << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";
  ASSERT_EQ(want.slot, kn::kDotSlot);

  std::vector<MessagePtr> got;
  std::vector<uint8_t> red, fc2;
  std::vector<std::vector<uint8_t>> act, act_scale;
  uint64_t head = 0, tail = 0;
  {
    EnsureSlots();
    RT::GetRecorder().StartNew("moe_dot_chain");
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreContext ctx;
    Core core(clk, "core", ctx);
    core.GetRouter().Preload(kTokenPath, EnterCore());
    RouteEntry to_self = ReduceAlone(0);
    core.GetRouter().Preload(kChipReducePath, to_self);
    core.GetDte().Tables().PreloadRtab(kChipReducePath, to_self);
    RouteEntry out = ReduceAlone(kFlowMid);
    core.GetRouter().Preload(kRowPath, out);
    core.GetDte().Tables().PreloadRtab(kRowPath, out);
    core.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
    core.Rv(1).LoadImage(KernelDir() + "kernel_mu.hex");
    core.Rv(2).LoadImage(KernelDir() + "kernel_vu.hex");
    WriteDotChain(core, want.slot);

    // boot 期装进去的那几样：权重按专家在本组内的序号摆，topK 表直接存组内序号。
    // topK 由 DTE 搬运时经专用数据线写进 MU 的 topK_ep_table，这里直接注入同一份
    // （本 core 的 MU 任务走 stream 0）。token 由 Router 送进来。
    core.GetMu().EpInfo().WriteTopk(
        0, TopkBytes({{kLocal[0], kn::kWep[0]}, {kLocal[1], kn::kWep[1]}}));
    kn::PokeCoreWeights(core.Mmem(), want.group, want.chip, want.slot, kLocal);

    auto token = std::make_shared<Message>();
    token->path_id = kTokenPath;
    token->user_id = 77;
    token->scale_valid = 1;
    token->payload = want.token;
    token->payload.insert(token->payload.end(), want.token_scale.begin(),
                          want.token_scale.end());
    token->size = token->payload.size();
    token->dst_addr = kn::kTokenOff;

    ChainHarness harness(clk, core, /*at=*/2, token);
    clk->Continue(200000 * kPeriod);
    RT::JoinAll();
    got = harness.out_msgs;
    head = harness.head;
    tail = harness.tail;
    red = core.Cmem().Peek(kn::kRedOff + kn::kSwHead,
                           kn::kPartBytes - kn::kSwHead);
    for (uint64_t e = 0; e < kn::kExperts; ++e) {
      act.push_back(
          core.Cmem().Peek(kn::kActOff + e * kn::kActStride, kn::kSegInter));
      act_scale.push_back(core.Cmem().PeekScale(
          kn::kActOff + e * kn::kActStride, kn::kSegInter / 32));
    }
    fc2 = core.Cmem().Peek(kn::ConcatOf(want.slot), kn::kFc2Bytes);
  }
  RT::FlushRecorder();
  RT::Reset();

  EXPECT_EQ(head, tail) << "任务链没走到头，这个用户没退休";
  EXPECT_EQ(red, want.part) << "只有自己一份的归约应当原样交回";
  for (uint64_t e = 0; e < kn::kExperts; ++e) {
    EXPECT_EQ(act[e], want.act[e]) << "第 " << e << " 个专家的 FC2 输入";
    EXPECT_EQ(act_scale[e], want.act_scale[e]) << "第 " << e << " 个专家的 scale";
  }
  EXPECT_EQ(fc2, want.fc2) << "concat 区里自己那一段";
  ASSERT_EQ(got.size(), 1u) << "出口上应当收到行链那一包";
  ASSERT_EQ(got[0]->payload.size(), kn::kRowBytes);
  std::vector<uint8_t> want_row(kn::kRowBytes, 0);
  std::copy(want.fc2.begin(), want.fc2.end(),
            want_row.begin() + kn::kSwHead + want.slot * kn::kFc2Bytes);
  EXPECT_EQ(got[0]->payload, want_row) << "行链那一包：只有自己那一段有数";
}
