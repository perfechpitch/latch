// 一个 core 上那一段 MoE 的数值：MU 算 FC1 与 FC3、门控、MU 算 FC2 并按 topK
// 权重合并几个专家，结果与 reference/ 那一份逐 bit 相同。
//
// 这一份只走计算通路，不经 TS 与 RV core：三笔 MU 任务与门控那几条 VU 宏指令
// 由这里按顺序下发。链路那一层由 e2e 那一份验。
//
// 权重几百 KB，比对向量里只给种子，两侧按同一个规则各自生成。

#include <gtest/gtest.h>

#include <algorithm>

#include <array>

#include <cmath>
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
#include "bach/ip/chip/core/mu/mu.h"
#include "bach/ip/chip/core/core.h"
#include "bach/ip/chip/core/vu/vu.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

// 波形写成 <name>.trace，落在跑测试时的当前目录。收尾要趁模块表还在：信号名与
// 层次是从模块表写进波形的，RT::Reset() 会把那张表清掉。

// ── 比对向量 ──

struct MoeCase {
  uint64_t k = 0, inter = 0, out_n = 0, experts = 0;
  uint64_t token_seed = 0, w1_seed = 0, w3_seed = 0, w2_seed = 0;
  std::vector<float> w_ep;
  std::vector<uint8_t> token;
  std::vector<std::vector<uint8_t>> fc1_bytes, fc3_bytes, act_bytes;
  std::vector<uint32_t> out_bits;
};

std::vector<uint8_t> HexBytes(std::string const& s) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    v.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
  }
  return v;
}

std::vector<uint32_t> HexWords(std::string const& s) {
  std::vector<uint32_t> v;
  std::stringstream ss(s);
  std::string one;
  while (std::getline(ss, one, ',')) {
    if (!one.empty()) v.push_back(uint32_t(std::stoul(one, nullptr, 16)));
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
  if (kv.count("k") == 0) return c;
  c.k = std::stoull(kv["k"]);
  c.inter = std::stoull(kv["inter"]);
  c.out_n = std::stoull(kv["out_n"]);
  c.experts = std::stoull(kv["experts"]);
  c.token_seed = std::stoull(kv["token_seed"], nullptr, 16);
  c.w1_seed = std::stoull(kv["w1_seed"], nullptr, 16);
  c.w3_seed = std::stoull(kv["w3_seed"], nullptr, 16);
  c.w2_seed = std::stoull(kv["w2_seed"], nullptr, 16);
  for (uint32_t b : HexWords(kv["w_ep"])) c.w_ep.push_back(numeric::FloatOf(b));
  c.token = HexBytes(kv["token"]);
  for (uint64_t e = 0; e < c.experts; ++e) {
    std::string t = std::to_string(e);
    c.fc1_bytes.push_back(HexBytes(kv["fc1_bytes" + t]));
    c.fc3_bytes.push_back(HexBytes(kv["fc3_bytes" + t]));
    c.act_bytes.push_back(HexBytes(kv["act_bytes" + t]));
  }
  c.out_bits = HexWords(kv["out_bits"]);
  return c;
}

// 与 reference/vectors.py 的 tame_cpp 逐字节相同的一批数。
std::vector<uint8_t> Pattern(uint64_t n, uint64_t seed) {
  std::vector<uint8_t> v;
  uint64_t s = seed;
  for (uint64_t i = 0; i < n; ++i) {
    s = s * 1103515245u + 12345u;
    v.push_back(uint8_t((s >> 16) & 0xFFu));
  }
  return v;
}

std::vector<uint8_t> TameBf16(uint64_t count, uint64_t seed) {
  std::vector<uint8_t> v = Pattern(count * 2, seed);
  for (uint64_t i = 0; i + 1 < v.size(); i += 2) {
    uint8_t sign = uint8_t(v[i + 1] & 0x80u);
    uint8_t exp = uint8_t(126 + (v[i] % 3));
    v[i + 1] = uint8_t(sign | (exp >> 1));
    v[i] = uint8_t(((exp & 1u) << 7) | (v[i] & 0x7Fu));
  }
  return v;
}

// ── 存储桩 ──
//
// 由外面统一驱动：门控那一步要在同一个协程里读出中间结果再写回去，各占一个
// 协程的话就是两个线程碰同一个容器。
class MoeMem : public BachModule {
 public:
  // 一块存储几个口。MU 与 VU 都读写 Core Mem，写回去的中间结果下一步要读回来，
  // 所以几个口对着同一份内容。第一个口回读数据，其余只收写。
  MoeMem(ClockPtr c, const std::string& name, MemPort& rd,
         std::vector<MemPort*> more, uint64_t bytes, uint64_t delay)
      : BachModule(c, name, 0, false), port(rd), others(std::move(more)),
        mem(bytes, 0), latency(delay) {}

  std::vector<uint8_t> mem;

  void Poke(uint64_t at, std::vector<uint8_t> const& v) {
    for (uint64_t i = 0; i < v.size() && at + i < mem.size(); ++i) {
      mem[at + i] = v[i];
    }
  }
  std::vector<uint8_t> Peek(uint64_t at, uint64_t n) const {
    std::vector<uint8_t> v;
    for (uint64_t i = 0; i < n; ++i) {
      v.push_back(at + i < mem.size() ? mem[at + i] : 0);
    }
    return v;
  }

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    if (pipes.size() != others.size() + 1) pipes.resize(others.size() + 1);
    Serve(0, port, now);
    for (uint64_t i = 0; i < others.size(); ++i) {
      if (others[i] != nullptr) Serve(i + 1, *others[i], now);
    }
  }

 private:
  // 一个口一条在途队列：几个口同时读时，响应各回各的。
  void Serve(uint64_t idx, MemPort& p, uint64_t now) {
    ByteBlockPtr rsp;
    bool valid = false;
    if (!pipes[idx].empty() && pipes[idx].front().first <= now) {
      rsp = pipes[idx].front().second;
      valid = true;
      pipes[idx].pop_front();
    }
    p.DriveSlave(true, valid, rsp);

    if (p.req_valid.Get() == 0) return;
    uint64_t addr = p.req_addr.Get();
    uint64_t n = p.req_bytes.Get();
    if (p.req_we.Get() != 0) {
      ByteBlockPtr d = p.req_wdata.Get();
      if (d) {
        uint64_t woff = p.req_woff.Get();
        std::vector<uint8_t> seg;
        for (uint64_t i = 0; i < n && woff + i < d->size(); ++i) {
          seg.push_back((*d)[woff + i]);
        }
        Poke(addr + woff, seg);
      }
      return;
    }
    pipes[idx].push_back(
        {now + latency, std::make_shared<ByteBlock>(Peek(addr, n))});
  }

  MemPort& port;
  std::vector<MemPort*> others;
  std::vector<std::deque<std::pair<uint64_t, ByteBlockPtr>>> pipes;
  uint64_t latency;
};

// ── 地址分配 ──
//
// Core Mem 里分四段：token、FC1 的结果、FC3 的结果、门控之后的激活。每一段按
// 专家隔开。Matrix Mem 里三个矩阵各一段，段内按专家隔开。
constexpr uint64_t kTokenAt = 0x1000;
constexpr uint64_t kFc1At = 0x4000;
constexpr uint64_t kFc3At = 0x8000;
constexpr uint64_t kActAt = 0xC000;
constexpr uint64_t kOutAt = 0x10000;
constexpr uint64_t kTopkAt = 0x18000;
constexpr uint64_t kAcStride = 0x1000;

constexpr uint64_t kW1At = 0x000000;
constexpr uint64_t kW3At = 0x600000;
constexpr uint64_t kW2At = 0xC00000;
constexpr uint64_t kBStride = 0x300000;

// topK 表在 Core Mem 里的样子：每项 {expert_id 2 B, weight 4 B}。
std::vector<uint8_t> TopkBytes(std::vector<TopkEntry> const& t) {
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

// ── VU 那一段：门控 ──
//
// SwiGLU 的门控要三条宏指令。VU 的执行链是 VALU 在 VSFU 前面，一条里做不完
// 「先取 sigmoid 再乘回去」；而且一条宏指令只有一路 LU，fc1 与 fc3 两个向量
// 也进不来。所以经 VRF 中转：
//
//   一  LU 读 fc1 → VSFU 出 sigmoid → 写 VRF 第 0 项
//   二  LU 读 fc1 → VALU0 乘 VRF 第 0 项 → 写 VRF 第 8 项（silu）
//   三  LU 读 fc3 → VALU0 乘 VRF 第 8 项 → SU 量化成 BF16 写回
//
// 第 8 项不是第 1 项：一条 VL=256 的 FP32 向量占 8 个 entry，索引是 entry 号。
constexpr uint64_t kVrfSig = 0;
constexpr uint64_t kVrfGate = 8;

uint64_t OpWord(uint64_t opcode, uint64_t src1 = 0, uint64_t src2 = 0) {
  return opcode | (src1 << 8) | (src2 << 16);
}

uint64_t IndexWord(uint64_t p0) { return p0 & 0xFFFFu; }

uint64_t TypeVlWord(uint64_t vl) {
  return (vl & kVuVlMask) | (uint64_t(numeric::RoundMode::kRne)
                             << kVuRoundModeShift);
}

// 一笔 MU 任务要写的那几个寄存器。
struct MuJob {
  uint64_t kblock = 1, nblock = 1;
  uint64_t addr_token = 0, addr_weight = 0, addr_out = 0;
  uint64_t experts = 1;
  bool ep_reduce = false;
  uint64_t task_id = 0;
};

// 三笔任务按顺序下发，中间那一步的门控在这里算。MU、两个存储桩与配置通路都由
// 这一个协程驱动。
class MoeRig : public BachModule {
 public:
  MoeRig(ClockPtr c, Mu& m, Vu& v, MoeMem& cm, MoeMem& mm, MoeCase const& want,
         std::shared_ptr<DsaCfgPort> mu_cfg, std::shared_ptr<DsaCfgPort> vu_cfg)
      : BachModule(c, "rig"), mu(m), vu(v), cmem(cm), mmem(mm), plan(want),
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
    if (stage == 0) {
      SetupGate();
      Submit({plan.k / 256, plan.inter / 32, kTokenAt, kW1At, kFc1At,
              plan.experts, false, 1});
      stage = 1;
      return;
    }
    if (stage == 1 && dones >= 1) {
      Submit({plan.k / 256, plan.inter / 32, kTokenAt, kW3At, kFc3At,
              plan.experts, false, 2});
      stage = 2;
      return;
    }
    if (stage == 2 && dones >= 2) {
      // 每个专家三条宏指令，地址走动态副本。
      for (uint64_t e = 0; e < plan.experts; ++e) FireGate(e);
      stage = 3;
      return;
    }
    // 六条宏指令各报一次完成。真实链路里由 RV core 轮询 macro_inst_left 收尾，
    // 这一份直接数。
    if (stage == 3 && vdones >= 3 * plan.experts) {
      Submit({plan.inter / 256, plan.out_n / 32, kActAt, kW2At, kOutAt,
              plan.experts, true, 3});
      stage = 4;
      return;
    }
    if (stage == 4 && dones >= 3) finished = true;
  }

  // 门控那三条宏指令的静态配置。三组各一条，地址走动态副本，所以几个专家共用
  // 这三组。
  void SetupGate() {
    uint64_t vl = plan.inter;
    VStatic(0, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
    VStatic(0, kVuVsfuOp, OpWord(uint64_t(VsfuOp::kSigmoid), kSrcLu));
    VStatic(0, kVuSuOp, OpWord(uint64_t(SuOp::kNop)));
    VStatic(0, kVuPrfOp, kSrcVsfu);
    VStatic(0, kVuStaticDupOffset + kVuVrfWtIndex, IndexWord(kVrfSig));
    VStatic(0, kVuStaticDupOffset + kVuTypeVl, TypeVlWord(vl));

    VStatic(1, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
    VStatic(1, kVuValu0Op,
            OpWord(uint64_t(ValuOp::kFmulVv), kSrcLu, kSrcVrfP0));
    VStatic(1, kVuSuOp, OpWord(uint64_t(SuOp::kNop)));
    VStatic(1, kVuPrfOp, kSrcValu0);
    VStatic(1, kVuStaticDupOffset + kVuVrfRdIndex, IndexWord(kVrfSig));
    VStatic(1, kVuStaticDupOffset + kVuVrfWtIndex, IndexWord(kVrfGate));
    VStatic(1, kVuStaticDupOffset + kVuTypeVl, TypeVlWord(vl));

    VStatic(2, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
    VStatic(2, kVuValu0Op,
            OpWord(uint64_t(ValuOp::kFmulVv), kSrcLu, kSrcVrfP0));
    VStatic(2, kVuSuOp, OpWord(uint64_t(SuOp::kStBf16), kSrcValu0));
    VStatic(2, kVuPrfOp, 0);
    VStatic(2, kVuStaticDupOffset + kVuVrfRdIndex, IndexWord(kVrfGate));
    VStatic(2, kVuStaticDupOffset + kVuTypeVl, TypeVlWord(vl));
  }

  // 一个专家那三条：地址每条写一遍，trigger 带上「地址取动态」的掩码。
  void FireGate(uint64_t e) {
    uint64_t fc1 = kFc1At + e * kAcStride;
    uint64_t fc3 = kFc3At + e * kAcStride;
    uint64_t act = kActAt + e * kAcStride;
    uint64_t mask = kVuMaskLdAddr | kVuMaskStAddr;
    uint64_t const rd[3] = {fc1, fc1, fc3};
    for (uint64_t g = 0; g < 3; ++g) {
      VPush(kVuLdAddr, rd[g]);
      VPush(kVuStAddr, act);
      // 三条之间是 VRF 上的先后，VU 只查 Core Mem 的地址重叠，所以逐条置 fence。
      VPush(kVuMacroInstTrigger,
            mask | (g << kVuTrigCfgIdxShift) | kVuTrigFence);
    }
  }

  void VStatic(uint64_t group, uint64_t off, uint64_t data) {
    VPush(kVuStaticBase + group * kVuStaticStride + off, data);
  }
  void VPush(uint64_t addr, uint64_t data) { vq.push_back({addr, data}); }

  void Submit(MuJob const& j) {
    Push(kMuTaskCfg, 0);                       // BF16 进、FP32 出
    Push(kMuTaskBlock, j.kblock | (j.nblock << 16));
    Push(kMuAddrToken, j.addr_token);
    Push(kMuAddrWeight, j.addr_weight);
    Push(kMuAddrOut, j.addr_out);
    Push(kMuAcExpertStride, kAcStride);
    Push(kMuBExpertStride, kBStride);
    Push(kMuEpCtrl, j.experts | (j.ep_reduce ? kMuEpReduceEn : 0));
    Push(kMuTopkAddr, kTopkAt);
    Push(kMuTopkStride, kTopkBytesPerStream);
    Push(kMuStreamId, 0);
    Push(kMuTaskId, j.task_id);
    Push(kMuSysCtrl, kMuTaskStart);
  }

  void Push(uint64_t addr, uint64_t data) { q.push_back({addr, data}); }

  static void DriveOne(std::deque<std::pair<uint64_t, uint64_t>>& que,
                       DsaCfgPort& port, bool& busy, uint64_t& sq) {
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

  Mu& mu;
  Vu& vu;
  MoeMem& cmem;
  MoeMem& mmem;
  MoeCase plan;
  std::shared_ptr<DsaCfgPort> cfg, vcfg;
  std::deque<std::pair<uint64_t, uint64_t>> q, vq;
  bool driving = false, vdriving = false;
  uint64_t seq = 0, vseq = 0, stage = 0;
};

}  // namespace

// 一个 core 上的整段：MU 算 FC1 与 FC3（两个专家各出一份），VU 做门控与量化，
// MU 算 FC2 并把两个专家按 topK 权重合并成一份。中间那一份激活与最后的 32 个
// FP32 都与参考实现逐 bit 相同。
TEST(BachMoe, OneCoreMatchesReference) {
  MoeCase want = ReadMoeCase();
  ASSERT_GT(want.k, 0u) << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";
  ASSERT_EQ(want.experts, 2u);

  std::vector<uint8_t> got;
  std::vector<std::vector<uint8_t>> act;
  uint64_t dones = 0, vdones = 0;
  bool finished = false;
  {
    EnsureSlots();
    RT::GetRecorder().StartNew("moe_one_core");
    ClockPtr clk = MakeClock(0, kPeriod);
    MuCfg setting;
    Mu mu(clk, "mu", setting);

    Vu vu(clk, "vu");

    auto cfg_port = std::make_shared<DsaCfgPort>(clk);
    auto cmem_rd = std::make_shared<MemPort>(clk);
    auto mmem_rd = std::make_shared<MemPort>(clk);
    auto cmem_wr = std::make_shared<MemPort>(clk);
    mu.AttachCfg(cfg_port);
    mu.AttachCmemRd(cmem_rd);
    mu.AttachMmemRd(mmem_rd);
    mu.AttachCmemWr(cmem_wr);

    auto vu_cfg = std::make_shared<DsaCfgPort>(clk);
    auto vu_ld = std::make_shared<MemPort>(clk);
    auto vu_st = std::make_shared<MemPort>(clk);
    vu.AttachCfg(vu_cfg);
    vu.AttachCmemLd(vu_ld);
    vu.AttachCmemSt(vu_st);
    vu.ConfigRegister().SetCoreIds(0, 4);

    // Core Mem 一块两个口，Matrix Mem 只读。
    MoeMem cmem(clk, "cmem", *cmem_rd,
                {cmem_wr.get(), vu_ld.get(), vu_st.get()}, 0x20000, 13);
    // 三个矩阵各两个专家：W1 与 W3 每个专家 MOE_K × MOE_INTER 个 BF16，合 3 MB。
    MoeMem mmem(clk, "mmem", *mmem_rd, {}, 0x1200000, 16);

    cmem.Poke(kTokenAt, want.token);
    // topK 里第 0 个是全局 17 号专家、组内第 1 个，第 1 个是全局 5 号、组内第 0 个。
    mu.EpInfo().SetLocalEpTable({5, 17});
    cmem.Poke(kTopkAt, TopkBytes({{17, want.w_ep[0]}, {5, want.w_ep[1]}}));

    for (uint64_t e = 0; e < want.experts; ++e) {
      uint64_t local = e == 0 ? 1 : 0;
      mmem.Poke(kW1At + local * kBStride,
                TameBf16(want.k * want.inter, want.w1_seed + e));
      mmem.Poke(kW3At + local * kBStride,
                TameBf16(want.k * want.inter, want.w3_seed + e));
      mmem.Poke(kW2At + local * kBStride,
                TameBf16(want.inter * want.out_n, want.w2_seed + e));
    }

    MoeRig rig(clk, mu, vu, cmem, mmem, want, cfg_port, vu_cfg);
    clk->Continue(200000 * kPeriod);
    RT::JoinAll();
    dones = rig.dones;
    vdones = rig.vdones;
    finished = rig.finished;
    for (uint64_t e = 0; e < want.experts; ++e) {
      act.push_back(cmem.Peek(kActAt + e * kAcStride, want.inter * 2));
    }
    got = cmem.Peek(kOutAt, want.out_n * 4);
  }
  RT::FlushRecorder();
  RT::Reset();

  EXPECT_EQ(dones, 3u) << "三笔 MU 任务应当各报一次完成";
  EXPECT_EQ(vdones, 3u * want.experts) << "门控每条宏指令各报一次完成";
  EXPECT_TRUE(finished);
  ASSERT_EQ(act.size(), want.act_bytes.size());
  for (uint64_t e = 0; e < act.size(); ++e) {
    EXPECT_EQ(act[e], want.act_bytes[e]) << "第 " << e << " 个专家的激活";
  }
  ASSERT_EQ(got.size(), want.out_bits.size() * 4);
  for (uint64_t j = 0; j < want.out_bits.size(); ++j) {
    uint32_t b = 0;
    for (int t = 0; t < 4; ++t) b |= uint32_t(got[j * 4 + t]) << (8 * t);
    EXPECT_EQ(b, want.out_bits[j]) << "第 " << j << " 个结果";
  }
}

namespace {

// VU 与它的两个存储桩、配置通路，都由这一个协程驱动。
class GateRig : public BachModule {
 public:
  GateRig(ClockPtr c, Vu& target, MoeMem& ld, MoeMem& st,
          std::shared_ptr<DsaCfgPort> cfg_port)
      : BachModule(c, "gate"), vu(target), ldmem(ld), stmem(st),
        cfg(std::move(cfg_port)) {}

  uint64_t dones = 0;
  void Push(uint64_t addr, uint64_t data) { q.push_back({addr, data}); }
  bool Idle() const { return q.empty() && !driving; }

 protected:
  void Step() override {
    if (vu.Done().Valid()) ++dones;
    DriveCfg();
    vu.RunStep();
    ldmem.RunStep();
    stmem.RunStep();
  }

 private:
  void DriveCfg() {
    if (driving) {
      if (!cfg->Ready()) return;
      q.pop_front();
      driving = false;
    }
    if (q.empty()) {
      cfg->Idle();
      return;
    }
    cfg->Drive(q.front().first, q.front().second, ++seq);
    driving = true;
  }

  Vu& vu;
  MoeMem& ldmem;
  MoeMem& stmem;
  std::shared_ptr<DsaCfgPort> cfg;
  std::deque<std::pair<uint64_t, uint64_t>> q;
  bool driving = false;
  uint64_t seq = 0;
};

// 一组静态配置的一个寄存器。
void Static(GateRig& rig, uint64_t group, uint64_t off, uint64_t data) {
  rig.Push(kVuStaticBase + group * kVuStaticStride + off, data);
}

}  // namespace

// 门控这一步交给 VU 真的算一遍：三条宏指令走完，写回 Core Mem 的那一份与参考
// 实现逐 bit 相同。
TEST(BachMoe, VuGateMatchesReference) {
  MoeCase want = ReadMoeCase();
  ASSERT_GT(want.k, 0u) << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";

  constexpr uint64_t kFc1 = 0x1000, kFc3 = 0x3000, kAct = 0x5000;
  std::vector<uint8_t> got;
  uint64_t dones = 0;
  {
    EnsureSlots();
    RT::GetRecorder().StartNew("moe_vu_gate");
    ClockPtr clk = MakeClock(0, kPeriod);
    Vu vu(clk, "vu");
    auto cfg_port = std::make_shared<DsaCfgPort>(clk);
    auto ld = std::make_shared<MemPort>(clk);
    auto st = std::make_shared<MemPort>(clk);
    vu.AttachCfg(cfg_port);
    vu.AttachCmemLd(ld);
    vu.AttachCmemSt(st);
    vu.ConfigRegister().SetCoreIds(0, 4);

    MoeMem ldmem(clk, "ldmem", *ld, {}, 0x8000, 8);
    MoeMem stmem(clk, "stmem", *st, {}, 0x8000, 8);
    // 只验第 0 个专家那一份：三条宏指令与专家数无关。
    ldmem.Poke(kFc1, want.fc1_bytes[0]);
    ldmem.Poke(kFc3, want.fc3_bytes[0]);

    GateRig rig(clk, vu, ldmem, stmem, cfg_port);
    uint64_t vl = want.inter;

    // 一：sigmoid 进 VRF。SU 不写回。
    Static(rig, 0, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
    Static(rig, 0, kVuVsfuOp, OpWord(uint64_t(VsfuOp::kSigmoid), kSrcLu));
    Static(rig, 0, kVuSuOp, OpWord(uint64_t(SuOp::kNop)));
    Static(rig, 0, kVuPrfOp, kSrcVsfu);
    Static(rig, 0, kVuStaticDupOffset + kVuVrfWtIndex, IndexWord(kVrfSig));
    Static(rig, 0, kVuStaticDupOffset + kVuTypeVl, TypeVlWord(vl));
    Static(rig, 0, kVuStaticDupOffset + kVuLdAddr, kFc1);

    // 二：fc1 乘上刚才那一份，得 silu，仍留在 VRF 里。
    Static(rig, 1, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
    Static(rig, 1, kVuValu0Op,
           OpWord(uint64_t(ValuOp::kFmulVv), kSrcLu, kSrcVrfP0));
    Static(rig, 1, kVuSuOp, OpWord(uint64_t(SuOp::kNop)));
    Static(rig, 1, kVuPrfOp, kSrcValu0);
    Static(rig, 1, kVuStaticDupOffset + kVuVrfRdIndex, IndexWord(kVrfSig));
    Static(rig, 1, kVuStaticDupOffset + kVuVrfWtIndex, IndexWord(kVrfGate));
    Static(rig, 1, kVuStaticDupOffset + kVuTypeVl, TypeVlWord(vl));
    Static(rig, 1, kVuStaticDupOffset + kVuLdAddr, kFc1);

    // 三：fc3 乘上 silu，量化成 BF16 写回 Core Mem。
    Static(rig, 2, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
    Static(rig, 2, kVuValu0Op,
           OpWord(uint64_t(ValuOp::kFmulVv), kSrcLu, kSrcVrfP0));
    Static(rig, 2, kVuSuOp, OpWord(uint64_t(SuOp::kStBf16), kSrcValu0));
    Static(rig, 2, kVuPrfOp, 0);
    Static(rig, 2, kVuStaticDupOffset + kVuVrfRdIndex, IndexWord(kVrfGate));
    Static(rig, 2, kVuStaticDupOffset + kVuTypeVl, TypeVlWord(vl));
    Static(rig, 2, kVuStaticDupOffset + kVuLdAddr, kFc3);
    Static(rig, 2, kVuStaticDupOffset + kVuStAddr, kAct);

    // 三条之间是 VRF 上的先后，VU 只查 Core Mem 的地址重叠，所以逐条置 fence。
    for (uint64_t g = 0; g < 3; ++g) {
      rig.Push(kVuMacroInstTrigger,
               (g << kVuTrigCfgIdxShift) | kVuTrigFence);
    }

    clk->Continue(20000 * kPeriod);
    RT::JoinAll();
    dones = rig.dones;
    got = stmem.Peek(kAct, want.inter * 2);
  }
  RT::FlushRecorder();
  RT::Reset();

  EXPECT_EQ(dones, 3u) << "三条宏指令各报一次完成";
  EXPECT_EQ(got, want.act_bytes[0]) << "门控之后的那一份与参考实现不一样";
}

namespace {

// ── 整条五步链 ──
//
// 摆放与 compiler/kernel/bach.h 的 MOE_* 同源，改一处要一起改。
constexpr uint64_t kChainTopkAt = 0x3000;
// 出核那几包在 Core Mem 里的起点。结果拆成 kChainPieceNum 包，一包一格，格首
// 16 B 是软件辅助信息，Router 做加法时跳过这一段，MU 的结果因此写在它之后。
constexpr uint64_t kChainSendAt = 0x9800;
constexpr uint64_t kChainPieceData = 8192;
constexpr uint64_t kChainPieceStride = 0x2080;
constexpr uint64_t kChainPieceNum = 3;
constexpr uint64_t kChainW1At = 0x000000;
constexpr uint64_t kChainW3At = 0x600000;
constexpr uint64_t kChainW2At = 0xC00000;
constexpr uint64_t kChainBStride = 0x300000;

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

// EPTP-NN 的五步。FC1 与 FC3、门控、FC2 三步各在一个 task 里发几笔 DSA 任务，每
// 笔都会报一次完成，报几次会让 TS 把任务链推过头，所以这三步收 RV core 那一路的
// 完成。
void WriteMoeChain(Core& core) {
  TaskEntry in;
  in.send_unit = SendUnit::kDte;
  in.recv_unit = RecvUnit::kDsa;
  in.path_id = 3;
  in.wait_wake = true;
  in.task_pc = SymbolOf("task_dte_user_init", "dte");
  core.GetTs().Cfg().WriteTask(0, in);

  TaskEntry fc13;
  fc13.send_unit = SendUnit::kMu;
  fc13.recv_unit = RecvUnit::kRvOnly;
  fc13.task_pc = SymbolOf("task_mu_fc13", "mu");
  core.GetTs().Cfg().WriteTask(1, fc13);

  TaskEntry gate;
  gate.send_unit = SendUnit::kVu;
  gate.recv_unit = RecvUnit::kRvOnly;
  gate.task_pc = SymbolOf("task_vu_gate", "vu");
  core.GetTs().Cfg().WriteTask(2, gate);

  TaskEntry fc2;
  fc2.send_unit = SendUnit::kMu;
  fc2.recv_unit = RecvUnit::kRvOnly;
  fc2.task_pc = SymbolOf("task_mu_fc2", "mu");
  core.GetTs().Cfg().WriteTask(3, fc2);

  TaskEntry out;
  out.send_unit = SendUnit::kDte;
  out.recv_unit = RecvUnit::kDsa;
  out.end = true;
  out.path_id = 0;
  out.task_pc = SymbolOf("task_dte_send_moe", "dte");
  core.GetTs().Cfg().WriteTask(4, out);

  core.GetTs().Cfg().SetInitFinish();
  core.GetDte().Tables().PreloadPathTask(3, 0);
}

RouteEntry MoeEnterCore() {
  RouteEntry e;
  e.flow_dir = 0;
  e.path_core_bypass = false;
  e.stream_table_enable = false;
  e.operation = Operation::kForward;
  return e;
}

RouteEntry MoeLeaveCore() {
  RouteEntry e;
  e.flow_dir = kFlowMid;
  e.path_core_bypass = true;
  e.operation = Operation::kForward;
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
  // 三个 DSA 各报了几次完成。
  uint64_t dte_dones = 0, mu_dones = 0, vu_dones = 0;

 protected:
  void Step() override {
    head = core.GetTs().Table().HeadPtr();
    tail = core.GetTs().Table().TailPtr();
    Count(core.GetTs().DsaDone(0).Valid(), &dte_hold, &dte_dones);
    Count(core.GetTs().DsaDone(1).Valid(), &mu_hold, &mu_dones);
    Count(core.GetTs().DsaDone(2).Valid(), &vu_hold, &vu_dones);
    LinkEndPtr in = core.InWire(1);
    if (CycleNow() == fire_at && msg) {
      in->flit.Drive(0, true, true, msg->size, msg);
    } else {
      in->flit.Idle();
    }
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
  static void Count(bool valid, bool* hold, uint64_t* cnt) {
    if (valid && !*hold) {
      ++*cnt;
      *hold = true;
    } else if (!valid) {
      *hold = false;
    }
  }

  Core& core;
  uint64_t fire_at;
  MessagePtr msg;
  bool dte_hold = false, mu_hold = false, vu_hold = false;
};

}  // namespace

// EPTP-NN 的五步链跑一遍：一个 token 进核，MU 算 FC1 与 FC3，VU 做门控与量化，
// MU 算 FC2 并合并两个专家，结果出核。收到的字节与参考实现逐 bit 相同。
TEST(BachMoe, FiveStepChainMatchesReference) {
  if (!KernelBuilt()) GTEST_SKIP() << "kernel 还没编";
  MoeCase want = ReadMoeCase();
  ASSERT_GT(want.k, 0u) << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";

  std::vector<MessagePtr> got;
  std::vector<uint8_t> landed;
  uint64_t head = 0, tail = 0;
  uint64_t dte_dones = 0, mu_dones = 0, vu_dones = 0;
  {
    EnsureSlots();
    RT::GetRecorder().StartNew("moe_five_step");
    ClockPtr clk = MakeClock(0, kPeriod);
    CoreContext ctx;
    Core core(clk, "core", ctx);
    core.GetRouter().Preload(3, MoeEnterCore());
    core.GetRouter().Preload(0, MoeLeaveCore());
    core.Rv(0).LoadImage(KernelDir() + "kernel_dte.hex");
    core.Rv(1).LoadImage(KernelDir() + "kernel_mu.hex");
    core.Rv(2).LoadImage(KernelDir() + "kernel_vu.hex");
    WriteMoeChain(core);

    // boot 期装进去的那几样：权重按专家在本组内的序号摆，topK 表与本组的专家
    // 名单一起给。topK 里第 0 个是全局 17 号专家、组内第 1 个。
    core.GetMu().EpInfo().SetLocalEpTable({5, 17});
    core.Cmem().Poke(kChainTopkAt,
                     TopkBytes({{17, want.w_ep[0]}, {5, want.w_ep[1]}}));
    for (uint64_t e = 0; e < want.experts; ++e) {
      uint64_t local = e == 0 ? 1 : 0;
      core.Mmem().Poke(kChainW1At + local * kChainBStride,
                       TameBf16(want.k * want.inter, want.w1_seed + e));
      core.Mmem().Poke(kChainW3At + local * kChainBStride,
                       TameBf16(want.k * want.inter, want.w3_seed + e));
      core.Mmem().Poke(kChainW2At + local * kChainBStride,
                       TameBf16(want.inter * want.out_n, want.w2_seed + e));
    }

    auto token = std::make_shared<Message>();
    token->path_id = 3;
    token->user_id = 77;
    token->size = want.token.size();
    token->stream_id = 0;
    token->task_id = 0;
    token->payload = want.token;

    ChainHarness harness(clk, core, /*at=*/2, token);
    clk->Continue(200000 * kPeriod);
    RT::JoinAll();
    got = harness.out_msgs;
    head = harness.head;
    tail = harness.tail;
    dte_dones = harness.dte_dones;
    mu_dones = harness.mu_dones;
    vu_dones = harness.vu_dones;
    for (uint64_t k = 0; k < kChainPieceNum; ++k) {
      std::vector<uint8_t> b = core.Cmem().Peek(
          kChainSendAt + k * kChainPieceStride + kReduceSwHeaderBytes,
          kChainPieceData);
      landed.insert(landed.end(), b.begin(), b.end());
    }
  }
  RT::FlushRecorder();
  RT::Reset();

  EXPECT_EQ(head, tail) << "任务链没走到头，这个用户没退休";
  // 五步各自报了几次 DSA 完成：进核与出核各一次（出核拆成几包，这条路不归约，
  // 只有最后一包带 task_last），FC1 与 FC3 两笔加 FC2 按包分成的几笔，门控每个
  // 专家三条宏指令。
  EXPECT_EQ(dte_dones, 2u);
  EXPECT_EQ(mu_dones, 2u + kChainPieceNum);
  EXPECT_EQ(vu_dones, 3u * want.experts);
  // 先看留在 Core Mem 里的那一份：出核那一步之前算完的就是它。
  ASSERT_EQ(landed.size(), want.out_bits.size() * 4);
  for (uint64_t j = 0; j < want.out_bits.size(); ++j) {
    uint32_t b = 0;
    for (int t = 0; t < 4; ++t) b |= uint32_t(landed[j * 4 + t]) << (8 * t);
    EXPECT_EQ(b, want.out_bits[j]) << "Core Mem 里第 " << j << " 个结果";
  }
  // 出口上收到的几包按落点排好拼回一整份，每包跳过格首的 16 B 头。
  ASSERT_EQ(got.size(), kChainPieceNum) << "出口上要收齐拆开的那几包";
  std::sort(got.begin(), got.end(),
            [](MessagePtr const& a, MessagePtr const& b) {
              return a->dst_addr < b->dst_addr;
            });
  std::vector<uint8_t> result;
  for (MessagePtr const& m : got) {
    ASSERT_EQ(m->payload.size(), kReduceSwHeaderBytes + kChainPieceData);
    result.insert(result.end(), m->payload.begin() + kReduceSwHeaderBytes,
                  m->payload.end());
  }
  ASSERT_EQ(result.size(), want.out_bits.size() * 4);
  for (uint64_t j = 0; j < want.out_bits.size(); ++j) {
    uint32_t b = 0;
    for (int t = 0; t < 4; ++t) b |= uint32_t(result[j * 4 + t]) << (8 * t);
    EXPECT_EQ(b, want.out_bits[j]) << "出核那一份第 " << j << " 个结果";
  }
}
