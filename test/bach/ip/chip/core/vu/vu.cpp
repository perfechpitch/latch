// VU DSA 的行为基线。
//
// 步 8 的判据：逐条计算原语与参考实现逐 bit 比对。参考实现写在这个文件里，按
// 硬件的顺序算：归约是 LANES 内先加再走树，换成整条顺序加就不是同一个 bit。
//
// 除计算原语外，另外五件事各有用例：trigger 写一次执行一次、静态配置组被引用时
// 配置写阻塞、Scoreboard 挡住有依赖的宏指令、MACRO_INST_FENCE 与 DATA_BROADCAST
// 各自的派发条件、VL 的两个边界。

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include <deque>
#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/vu/vu.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

// VU 十四个模块由装配统一驱动，只占一个协程；驱动台自己另外几个。
void EnsureSlots() { RT::Reset(8, 8); }

// 扮演 Core Mem：一块字节数组，收得下、隔 14 拍回响应。
class MemStub : public BachModule {
 public:
  MemStub(ClockPtr c, const std::string& name, MemPort& p, uint64_t bytes)
      : BachModule(c, name), port(p), mem(bytes, 0) {}

  std::vector<uint8_t> mem;
  uint64_t reads = 0, writes = 0;

  void Poke(uint64_t at, std::vector<uint8_t> const& v) {
    for (uint64_t i = 0; i < v.size() && at + i < mem.size(); ++i) {
      mem[at + i] = v[i];
    }
  }
  std::vector<uint8_t> Peek(uint64_t at, uint64_t n) const {
    std::vector<uint8_t> v;
    for (uint64_t i = 0; i < n; ++i) v.push_back(at + i < mem.size() ? mem[at + i] : 0);
    return v;
  }

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    ByteBlockPtr rsp;
    bool valid = false;
    if (!pipe.empty() && pipe.front().at <= now) {
      rsp = pipe.front().data;
      valid = true;
      pipe.pop_front();
    }
    port.DriveSlave(true, valid, rsp);

    // 不按 (addr, bytes) 去重：master 每拍要么发一笔要么 IdleReq，端口上不会
    // 回落成上一拍的值，所以每一拍的 req_valid 都是一笔真请求。
    if (port.req_valid.Get() == 0) return;
    uint64_t addr = port.req_addr.Get();
    uint64_t n = port.req_bytes.Get();

    if (port.req_we.Get() != 0) {
      ByteBlockPtr d = port.req_wdata.Get();
      if (d) {
        // 只改 [woff, woff + bytes) 那一段：一次请求搬一整块，两头的字节归
        // 相邻的数据。带 scale 的那一档尾巴上另有 4 个字节。
        uint64_t woff = port.req_woff.Get();
        for (uint64_t i = 0; i < n && woff + i < d->size(); ++i) {
          if (addr + woff + i < mem.size()) mem[addr + woff + i] = (*d)[woff + i];
        }
        if (d->size() > 128) {
          // scale 段：一块 128 B 一个字节，摆在正文之后的 scale 区。
          uint64_t at = scale_base + addr / 128;
          if (at < mem.size()) mem[at] = (*d)[128];
        }
      }
      ++writes;
      return;
    }
    auto data = std::make_shared<ByteBlock>(Peek(addr, n));
    if (port.req_scale_en.Get() != 0) {
      uint64_t at = scale_base + addr / 128;
      data->push_back(at < mem.size() ? mem[at] : 0);
      data->resize(n + 4, 0);
    }
    pipe.push_back({now + kVuCmLatency, data});
    ++reads;
  }

 public:
  uint64_t scale_base = 0;

 private:
  struct Pending {
    uint64_t at = 0;
    ByteBlockPtr data;
  };
  MemPort& port;
  std::deque<Pending> pipe;
};

// 按序把配置写发进去，看见 ready 才换下一笔。
class CfgWriter : public BachModule {
 public:
  CfgWriter(ClockPtr c, std::shared_ptr<DsaCfgPort> p)
      : BachModule(c, "cfg_writer"), port(std::move(p)) {}

  void Push(uint64_t addr, uint64_t data) { q.push_back({addr, data}); }
  bool Idle() const { return q.empty() && !driving; }
  uint64_t Sent() const { return sent; }

 protected:
  void Step() override {
    if (driving) {
      if (!port->Ready()) return;   // 阻塞时保持不变
      q.pop_front();
      ++sent;
      driving = false;
    }
    if (q.empty()) {
      port->Idle();
      return;
    }
    port->Drive(q.front().first, q.front().second, ++seq);
    driving = true;
  }

 private:
  std::shared_ptr<DsaCfgPort> port;
  std::deque<std::pair<uint64_t, uint64_t>> q;
  bool driving = false;
  uint64_t sent = 0, seq = 0;
};

// 收 dsa_done。
class DoneSink : public BachModule {
 public:
  DoneSink(ClockPtr c, DonePort& p) : BachModule(c, "done"), port(p) {}
  struct Rec {
    uint64_t stream_id = 0, task_id = 0, event = 0;
  };
  std::vector<Rec> got;

 protected:
  void Step() override {
    if (port.Valid()) {
      got.push_back({port.stream_id.Get(), port.task_id.Get(), port.event.Get()});
    }
  }

 private:
  DonePort& port;
};

// 装配统一驱动：VU 内部各模块 tick=false，由这一个协程按末级先做的次序逐个走。
class VuDriver : public BachModule {
 public:
  VuDriver(ClockPtr c, Vu& target) : BachModule(c, "driver"), vu(target) {}

 protected:
  void Step() override { vu.RunStep(); }

 private:
  Vu& vu;
};

// ── 一个完整的驱动台 ──
struct Rig {
  ClockPtr clk;
  std::unique_ptr<Vu> vu;
  std::shared_ptr<DsaCfgPort> cfg;
  std::shared_ptr<MemPort> ld, st;
  std::unique_ptr<CfgWriter> writer;
  std::unique_ptr<MemStub> ldmem, stmem;
  std::unique_ptr<DoneSink> sink;
  std::unique_ptr<VuDriver> driver;

  explicit Rig(uint64_t bytes = 64 * 1024) {
    EnsureSlots();
    clk = MakeClock(0, kPeriod);
    vu = std::make_unique<Vu>(clk, "vu");
    cfg = std::make_shared<DsaCfgPort>(clk);
    vu->AttachCfg(cfg);
    ld = std::make_shared<MemPort>(clk);
    st = std::make_shared<MemPort>(clk);
    vu->AttachCmemLd(ld);
    vu->AttachCmemSt(st);
    writer = std::make_unique<CfgWriter>(clk, cfg);
    ldmem = std::make_unique<MemStub>(clk, "ldmem", *ld, bytes);
    stmem = std::make_unique<MemStub>(clk, "stmem", *st, bytes);
    sink = std::make_unique<DoneSink>(clk, vu->Done());
    driver = std::make_unique<VuDriver>(clk, *vu);
    // stream_id 与 task_id 沿用 VU-Core CSR 自带的那一组：TS 下发给这个核的
    // 值，由 VU RV core 在写 trigger 之前配进来。
    vu->ConfigRegister().SetCoreIds(3, 7);
    // profile_ctrl.RUN 的复位值是 0，写 1 才开始计数。用例要读计数器，先开上。
    Write(kVuProfileCtrl, kVuProfileRun);
  }

  void Write(uint64_t addr, uint64_t data) { writer->Push(addr, data); }

  // 写一组静态配置。off 是组内字节偏移。
  void WriteStatic(uint64_t group, uint64_t off, uint64_t data) {
    Write(kVuStaticBase + group * kVuStaticStride + off, data);
  }

  void Run(uint64_t cycles) {
    clk->Continue(cycles * kPeriod);
    RT::JoinAll();
  }
};

// ── 配置助手 ──
//
// 静态组内 23 个寄存器：前 12 个是 *_op / mask_op / PRF_op，后 11 个是动态参数
// 寄存器的静态副本，组内偏移 = 对应动态地址 + 0x2C。

// 一个 op 寄存器：OPCODE 与三路源选择各占一个字节。
uint64_t OpWord(uint64_t opcode, uint64_t src1 = 0, uint64_t src2 = 0,
                uint64_t src3 = 0) {
  return opcode | (src1 << 8) | (src2 << 16) | (src3 << 24);
}

// PRF_op：VRF 两个写端口与 MRF 写端口用「0 不写回、非零指定来源」，SRF 六个
// 虚拟写口是位图。
uint64_t PrfWord(uint64_t vrf_p0, uint64_t vrf_p1, uint64_t mrf,
                 uint64_t srf_en) {
  return vrf_p0 | (vrf_p1 << 8) | (mrf << 16) | (srf_en << 24);
}

// mask_op：VALU0、VALU1、VALU2、VSFU 各两位。
uint64_t MaskWord(uint64_t v0, uint64_t v1, uint64_t v2, uint64_t vsfu) {
  return v0 | (v1 << 2) | (v2 << 4) | (vsfu << 6);
}

uint64_t TypeVlWordOf(uint64_t vl, bool bf16) {
  return (vl & kVuVlMask) | (uint64_t(bf16 ? 1 : 0) << kVuDataTypeShift);
}

uint64_t TypeVlWord(uint64_t vl, bool bf16, numeric::RoundMode mode) {
  return (vl & kVuVlMask) | (uint64_t(bf16 ? 1 : 0) << kVuDataTypeShift) |
         (uint64_t(mode) << kVuRoundModeShift);
}

// 两个 16 位索引打成一个字：p0 在低半，p1 在高半。
uint64_t IndexWord(uint64_t p0, uint64_t p1 = 0) {
  return (p0 & 0xFFFFu) | ((p1 & 0xFFFFu) << 16);
}

// 四个 8 位 SRF 索引打成一个字。
uint64_t SrfWord(uint64_t p0, uint64_t p1 = 0, uint64_t p2 = 0,
                 uint64_t p3 = 0) {
  return (p0 & 0xFFu) | ((p1 & 0xFFu) << 8) | ((p2 & 0xFFu) << 16) |
         ((p3 & 0xFFu) << 24);
}

uint64_t TriggerWord(uint64_t cfg_idx, uint64_t mask = 0, uint64_t extra = 0) {
  return (mask << kVuTrigMaskShift) | (cfg_idx << kVuTrigCfgIdxShift) | extra;
}

std::vector<uint8_t> Fp32Bytes(std::vector<float> const& v) {
  std::vector<uint8_t> b;
  for (float x : v) {
    uint32_t w = numeric::BitsOf(x);
    for (int k = 0; k < 4; ++k) b.push_back(uint8_t((w >> (8 * k)) & 0xFFu));
  }
  return b;
}

std::vector<float> Fp32Of(std::vector<uint8_t> const& b) {
  std::vector<float> v;
  for (size_t i = 0; i + 3 < b.size(); i += 4) {
    uint32_t w = 0;
    for (int k = 0; k < 4; ++k) w |= uint32_t(b[i + k]) << (8 * k);
    v.push_back(numeric::FloatOf(w));
  }
  return v;
}

// 一组温和的输入：值落在 0.25～4，一条 VL=64 的向量加起来不会溢出。
std::vector<float> Tame(uint64_t n, uint64_t seed) {
  std::vector<float> v;
  uint64_t s = seed;
  for (uint64_t i = 0; i < n; ++i) {
    s = s * 1103515245u + 12345u;
    uint32_t man = uint32_t((s >> 8) & 0x7FFFFFu);
    uint32_t exp = 125 + uint32_t((s >> 3) % 3u);
    uint32_t sign = uint32_t((s >> 30) & 1u) << 31;
    v.push_back(numeric::FloatOf(sign | (exp << 23) | man));
  }
  return v;
}

constexpr uint64_t kSrcAddr = 0x1000;
constexpr uint64_t kDstAddr = 0x4000;
constexpr uint64_t kVl = 64;
constexpr uint64_t kStream = 3;
constexpr uint64_t kTask = 7;

// 配一条「LU 读 FP32 → 一个执行单元算 → SU 写回 FP32」的宏指令。各执行单元的
// 用例都在它上面换 op 与源。静态副本区放地址与 VL，动态区不用。
void SetupChain(Rig& rig, uint64_t op_reg, uint64_t op_word, uint64_t su_src,
                uint64_t vl = kVl) {
  rig.WriteStatic(0, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
  rig.WriteStatic(0, kVuSuOp, OpWord(uint64_t(SuOp::kStFp32), su_src));
  if (op_reg != 0) rig.WriteStatic(0, op_reg, op_word);
  rig.WriteStatic(0, kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(vl, false, numeric::RoundMode::kRne));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuLdAddr, kSrcAddr);
  rig.WriteStatic(0, kVuStaticDupOffset + kVuStAddr, kDstAddr);
}

// ── 数据通路 ──

TEST(Vu, LoadStoreRoundTrip) {
  Rig rig;
  std::vector<float> in = Tame(kVl, 0x100);
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(in));
  // 不经任何执行单元：LU 的结果由 SU_op.SRC_SEL 直接取走。
  SetupChain(rig, 0, 0, kSrcLu);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  std::vector<float> got = Fp32Of(rig.stmem->Peek(kDstAddr, kVl * 4));
  ASSERT_EQ(got.size(), in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    EXPECT_EQ(numeric::BitsOf(got[i]), numeric::BitsOf(in[i])) << "i=" << i;
  }
  ASSERT_EQ(rig.sink->got.size(), 1u);
  EXPECT_EQ(rig.sink->got[0].stream_id, kStream);
  EXPECT_EQ(rig.sink->got[0].task_id, kTask);
  EXPECT_EQ(rig.sink->got[0].event, 0u);
}

// ── 执行单元的计算原语 ──

TEST(Vu, Valu0MulByScalar) {
  // vfmul.vf：vd = src2 × 标量 src1。三个 VALU 只有 src1 能取 SRF，所以标量
  // 操作数一律挂 src1；VALU0 硬连线的是 SRF_rd_p1。
  Rig rig;
  std::vector<float> in = Tame(kVl, 0x201);
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(in));
  float k = 1.375f;
  rig.vu->Regfiles().WriteSrf(5, k);

  SetupChain(rig, kVuValu0Op,
             OpWord(uint64_t(ValuOp::kFmulVf), kSrcSrfP0 + 1, kSrcLu),
             kSrcValu0);
  rig.WriteStatic(0, kVuStaticDupOffset + kVuSrfRdIndex0, SrfWord(0, 5));
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  std::vector<float> got = Fp32Of(rig.stmem->Peek(kDstAddr, kVl * 4));
  ASSERT_EQ(got.size(), in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    float want = numeric::ClampNanInf(in[i] * k);
    EXPECT_EQ(numeric::BitsOf(got[i]), numeric::BitsOf(want)) << "i=" << i;
  }
}

TEST(Vu, Valu0MaccUsesThreeOperands) {
  // vfmacc.vv：vd += src1 × src2，累加器由 src3 提供。三操作数只有 VALU0 有。
  Rig rig;
  std::vector<float> in = Tame(kVl, 0x210);
  std::vector<float> acc = Tame(kVl, 0x211);
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(in));
  rig.vu->Regfiles().WriteVrf(0, acc, false, numeric::RoundMode::kRne);

  SetupChain(rig, kVuValu0Op,
             OpWord(uint64_t(ValuOp::kMaccVv), kSrcLu, kSrcLu, kSrcVrfP0),
             kSrcValu0);
  rig.WriteStatic(0, kVuStaticDupOffset + kVuVrfRdIndex, IndexWord(0));
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  std::vector<float> got = Fp32Of(rig.stmem->Peek(kDstAddr, kVl * 4));
  ASSERT_EQ(got.size(), in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    // 乘积先算出来再加，不写成一句，因为写成一句编译器会合成 FMA，少一次舍入。
    float prod = in[i] * in[i];
    float want = numeric::ClampNanInf(prod + acc[i]);
    EXPECT_EQ(numeric::BitsOf(got[i]), numeric::BitsOf(want)) << "i=" << i;
  }
}

TEST(Vu, VsfuExp) {
  Rig rig;
  std::vector<float> in = Tame(kVl, 0x202);
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(in));

  SetupChain(rig, kVuVsfuOp, OpWord(uint64_t(VsfuOp::kExp), kSrcLu), kSrcVsfu);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  std::vector<float> got = Fp32Of(rig.stmem->Peek(kDstAddr, kVl * 4));
  ASSERT_EQ(got.size(), in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    float want = numeric::ClampNanInf(std::exp(in[i]));
    EXPECT_EQ(numeric::BitsOf(got[i]), numeric::BitsOf(want)) << "i=" << i;
  }
}

TEST(Vu, VsfuSigmoid) {
  Rig rig;
  std::vector<float> in = Tame(kVl, 0x203);
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(in));

  SetupChain(rig, kVuVsfuOp, OpWord(uint64_t(VsfuOp::kSigmoid), kSrcLu),
             kSrcVsfu);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  std::vector<float> got = Fp32Of(rig.stmem->Peek(kDstAddr, kVl * 4));
  for (size_t i = 0; i < in.size(); ++i) {
    float want = numeric::ClampNanInf(1.0f / (1.0f + std::exp(-in[i])));
    EXPECT_EQ(numeric::BitsOf(got[i]), numeric::BitsOf(want)) << "i=" << i;
  }
}

TEST(Vu, Valu1ReduceSumFollowsTreeOrder) {
  // vfredusum.vs：标量初值在 src1，结果走 SRF 虚拟写口 p1。参考实现按 LANES
  // 内先加再走树，换成整条顺序加就不是同一个 bit。
  Rig rig;
  std::vector<float> in = Tame(kVl, 0x204);
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(in));
  rig.vu->Regfiles().WriteSrf(2, 0.0f);

  rig.WriteStatic(0, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
  rig.WriteStatic(0, kVuValu1Op,
                  OpWord(uint64_t(ValuOp::kRedusum), kSrcSrfP0 + 2, kSrcLu));
  rig.WriteStatic(0, kVuPrfOp, PrfWord(0, 0, 0, 1u << 1));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(kVl, false, numeric::RoundMode::kRne));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuLdAddr, kSrcAddr);
  rig.WriteStatic(0, kVuStaticDupOffset + kVuSrfRdIndex0, SrfWord(0, 0, 2));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuSrfWtIndex0, SrfWord(0, 9));
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  float got = rig.vu->Regfiles().ReadSrf(9);
  float want = numeric::ClampNanInf(
      0.0f + numeric::ReduceTree(in, kVuLanes));
  EXPECT_EQ(numeric::BitsOf(got), numeric::BitsOf(want));

  float serial = numeric::AccumInOrder(in);
  EXPECT_NE(numeric::BitsOf(want), numeric::BitsOf(serial))
      << "这一组输入分不出树序与顺序加的差别，换一组";
}

TEST(Vu, Valu1SortMax16) {
  // vsortmax16.v：Top-16 最大值排序，同时输出 16 个 INT16 索引。
  Rig rig;
  std::vector<float> in = Tame(kVl, 0x205);
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(in));

  SetupChain(rig, kVuValu1Op,
             OpWord(uint64_t(ValuOp::kSortmax16), kSrcNone, kSrcLu),
             kSrcValu1);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  std::vector<float> got = Fp32Of(rig.stmem->Peek(kDstAddr, kVuTopK * 4));
  std::vector<float> sorted = in;
  std::sort(sorted.begin(), sorted.end(), std::greater<float>());
  ASSERT_GE(got.size(), kVuTopK);
  for (uint64_t i = 0; i < kVuTopK; ++i) {
    EXPECT_EQ(numeric::BitsOf(got[i]), numeric::BitsOf(sorted[i])) << "i=" << i;
  }
}

TEST(Vu, SexeThreeSerialIterations) {
  // SEXE0 取 SRF_rd_p4/p5，SEXE1 取 SEXE0 的输出与 SRF_rd_p6，SEXE2 取 SEXE1
  // 的输出：三次迭代链式依赖，是同一物理单元在一条宏指令内跑三遍。
  Rig rig;
  rig.vu->Regfiles().WriteSrf(4, 2.0f);
  rig.vu->Regfiles().WriteSrf(5, 3.0f);
  rig.vu->Regfiles().WriteSrf(6, 4.0f);

  rig.WriteStatic(0, kVuSexe0Op,
                  OpWord(uint64_t(SexeOp::kFadd), kSrcSrfP0 + 4, kSrcSrfP0 + 5));
  rig.WriteStatic(0, kVuSexe1Op,
                  OpWord(uint64_t(SexeOp::kFmul), kSrcSexe0, kSrcSrfP0 + 6));
  rig.WriteStatic(0, kVuSexe2Op,
                  OpWord(uint64_t(SexeOp::kFsqrt), kSrcSexe1, kSrcNone));
  rig.WriteStatic(0, kVuPrfOp, PrfWord(0, 0, 0, (1u << 3) | (1u << 4) | (1u << 5)));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(1, false, numeric::RoundMode::kRne));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuSrfRdIndex1, SrfWord(4, 5, 6));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuSrfWtIndex0,
                  SrfWord(0, 0, 0, 20));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuSrfWtIndex1, SrfWord(21, 22));
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  // SEXE0 = 2 + 3 = 5；SEXE1 = 5 × 4 = 20；SEXE2 = sqrt(20)。
  EXPECT_EQ(rig.vu->Regfiles().ReadSrf(20), 5.0f);
  EXPECT_EQ(rig.vu->Regfiles().ReadSrf(21), 20.0f);
  EXPECT_EQ(numeric::BitsOf(rig.vu->Regfiles().ReadSrf(22)),
            numeric::BitsOf(std::sqrt(20.0f)));
}

TEST(Vu, LoadMaskThenMexeLogic) {
  // 两条宏指令：第一条 ld.vm_mask 写 MRF，第二条读它做掩码逻辑运算再写回 MRF。
  Rig rig;
  std::vector<uint8_t> bits = {0xA5, 0x3C, 0xFF, 0x00, 0x0F, 0xF0, 0x55, 0xAA};
  rig.ldmem->Poke(kSrcAddr, bits);

  // 组 0：ld.vm_mask → MRF entry 0。
  rig.WriteStatic(0, kVuLuOp, OpWord(uint64_t(LuOp::kLdVmMask)));
  rig.WriteStatic(0, kVuPrfOp, PrfWord(0, 0, kSrcLu, 0));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(kVl, false, numeric::RoundMode::kRne));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuLdAddr, kSrcAddr);
  rig.WriteStatic(0, kVuStaticDupOffset + kVuMrfWtIndex, IndexWord(0));
  // 组 1：MEXE 取 MRF_rd_p0 做 vmsof.m（只留首个 1），写回 MRF entry 4。
  // 两条读写同一段 MRF，第二条由 Scoreboard 自动等第一条退休。
  rig.WriteStatic(1, kVuMexeOp, OpWord(uint64_t(MexeOp::kSof), kSrcMrfP0));
  rig.WriteStatic(1, kVuPrfOp, PrfWord(0, 0, kSrcMexe, 0));
  rig.WriteStatic(1, kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(kVl, false, numeric::RoundMode::kRne));
  rig.WriteStatic(1, kVuStaticDupOffset + kVuMrfRdIndex, IndexWord(0));
  rig.WriteStatic(1, kVuStaticDupOffset + kVuMrfWtIndex, IndexWord(4));
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Write(kVuMacroInstTrigger, TriggerWord(1));
  rig.Run(800);

  // CM 里的掩码是连续位流，第 i 个元素在字节 i/8 的第 i%8 位；MRF 里才按 entry
  // 排（FP32 下一个 entry 只用低 32 bit）。ReadMrf 按 entry 排读回来，所以读到
  // 的第 i 个就是写进去的第 i 个。
  std::vector<bool> loaded = rig.vu->Regfiles().ReadMrf(0, kVl, false);
  ASSERT_EQ(loaded.size(), kVl);
  for (uint64_t i = 0; i < kVl; ++i) {
    bool want = ((bits[i / 8] >> (i % 8)) & 1u) != 0;
    EXPECT_EQ(loaded[i], want) << "i=" << i;
  }

  std::vector<bool> got = rig.vu->Regfiles().ReadMrf(4, kVl, false);
  uint64_t first = kVl;
  for (uint64_t i = 0; i < kVl; ++i) {
    if (loaded[i]) {
      first = i;
      break;
    }
  }
  for (uint64_t i = 0; i < kVl; ++i) EXPECT_EQ(got[i], i == first) << "i=" << i;
}

TEST(Vu, MexeCountPopulation) {
  // vcpop.m 的标量结果走 SRF 虚拟写口 p2。
  Rig rig;
  std::vector<uint8_t> bits(16, 0);
  bits[0] = 0xFF;
  bits[1] = 0x0F;
  rig.ldmem->Poke(kSrcAddr, bits);

  rig.WriteStatic(0, kVuLuOp, OpWord(uint64_t(LuOp::kLdVmMask)));
  rig.WriteStatic(0, kVuPrfOp, PrfWord(0, 0, kSrcLu, 0));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(kVl, false, numeric::RoundMode::kRne));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuLdAddr, kSrcAddr);
  rig.WriteStatic(0, kVuStaticDupOffset + kVuMrfWtIndex, IndexWord(0));
  rig.WriteStatic(1, kVuMexeOp, OpWord(uint64_t(MexeOp::kCpop), kSrcMrfP0));
  rig.WriteStatic(1, kVuPrfOp, PrfWord(0, 0, 0, 1u << 2));
  rig.WriteStatic(1, kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(kVl, false, numeric::RoundMode::kRne));
  rig.WriteStatic(1, kVuStaticDupOffset + kVuMrfRdIndex, IndexWord(0));
  rig.WriteStatic(1, kVuStaticDupOffset + kVuSrfWtIndex0, SrfWord(0, 0, 30));
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Write(kVuMacroInstTrigger, TriggerWord(1));
  rig.Run(800);

  std::vector<bool> m = rig.vu->Regfiles().ReadMrf(0, kVl, false);
  uint64_t want = 0;
  for (bool b : m) {
    if (b) ++want;
  }
  EXPECT_EQ(rig.vu->Regfiles().ReadSrf(30), float(want));
}

TEST(Vu, ValuMaskKeepsUnselectedElements) {
  // 掩码位为 0 的 element 不更新目的寄存器。掩码来源由 mask_op 给，不占
  // SRC*_SEL。
  Rig rig;
  std::vector<float> in = Tame(kVl, 0x220);
  std::vector<float> base = Tame(kVl, 0x221);
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(in));
  rig.vu->Regfiles().WriteVrf(8, base, false, numeric::RoundMode::kRne);
  // 偶数位置置 1。
  std::vector<bool> m(kVl);
  for (uint64_t i = 0; i < kVl; ++i) m[i] = (i % 2) == 0;
  rig.vu->Regfiles().WriteMrf(0, m, false);

  rig.WriteStatic(0, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
  rig.WriteStatic(0, kVuValu0Op,
                  OpWord(uint64_t(ValuOp::kFaddVv), kSrcLu, kSrcVrfP0));
  rig.WriteStatic(0, kVuMaskOp, MaskWord(kVuMaskP0, 0, 0, 0));
  rig.WriteStatic(0, kVuPrfOp, PrfWord(kSrcValu0, 0, 0, 0));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(kVl, false, numeric::RoundMode::kRne));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuLdAddr, kSrcAddr);
  rig.WriteStatic(0, kVuStaticDupOffset + kVuVrfRdIndex, IndexWord(8));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuVrfWtIndex, IndexWord(16));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuMrfRdIndex, IndexWord(0));
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  std::vector<float> got = rig.vu->Regfiles().ReadVrf(16, kVl, false);
  for (uint64_t i = 0; i < kVl; ++i) {
    if (m[i]) {
      float want = numeric::ClampNanInf(base[i] + in[i]);
      EXPECT_EQ(numeric::BitsOf(got[i]), numeric::BitsOf(want)) << "i=" << i;
    } else {
      // 未选中的位置保持写目标原有的值。这一轮写目标是空的 VRF 段，所以是 0。
      EXPECT_EQ(numeric::BitsOf(got[i]), 0u) << "i=" << i;
    }
  }
}

// ── 发射与身份 ──

TEST(Vu, TriggerRunsOncePerWrite) {
  // 任何一次写入都会向 ISQ 压入一条宏指令，即使写入值与上次相同。
  Rig rig;
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(Tame(kVl, 0x301)));
  SetupChain(rig, 0, 0, kSrcLu);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(600);

  EXPECT_EQ(rig.sink->got.size(), 2u);
}

TEST(Vu, StreamIdOverrideDoesNotTouchTaskId) {
  // STREAM_ID_OVERRIDE = 0 沿用 VU-Core CSR 自带的 Stream ID，= 1 改用 trigger
  // 的 STREAM_ID 字段。task_id 不在这个寄存器里，始终取 VU-Core 那一份。
  Rig rig;
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(Tame(kVl, 0x302)));
  rig.vu->ConfigRegister().SetCoreIds(2, 41);
  SetupChain(rig, 0, 0, kSrcLu);
  rig.Write(kVuMacroInstTrigger,
            TriggerWord(0, 0, kVuTrigSidOverride | (9u << kVuTrigSidShift)));
  rig.Run(400);

  ASSERT_EQ(rig.sink->got.size(), 1u);
  EXPECT_EQ(rig.sink->got[0].stream_id, 9u);
  EXPECT_EQ(rig.sink->got[0].task_id, 41u);
}

TEST(Vu, EventEnRaisesEvent) {
  Rig rig;
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(Tame(kVl, 0x303)));
  SetupChain(rig, 0, 0, kSrcLu);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0, 0, kVuTrigEventEn));
  rig.Run(400);

  ASSERT_EQ(rig.sink->got.size(), 1u);
  EXPECT_EQ(rig.sink->got[0].event, 1u);
}

TEST(Vu, TypeVlSwitchesBetweenStaticAndDynamic) {
  // STATIC_DYNAMIC_MASK.bit[0] 决定 TYPE_VL 取静态副本还是动态寄存器。两条
  // 宏指令的 VL 不一样，写回的字节数就不一样。
  Rig rig;
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(Tame(256, 0x304)));
  SetupChain(rig, 0, 0, kSrcLu, 32);          // 静态副本里 VL = 32
  rig.Write(kVuTypeVl, TypeVlWord(64, false, numeric::RoundMode::kRne));
  rig.Write(kVuMacroInstTrigger, TriggerWord(0, kVuMaskTypeVl));
  rig.Run(400);

  std::vector<uint8_t> got = rig.stmem->Peek(kDstAddr, 256);
  bool tail_written = false;
  for (uint64_t i = 128; i < 256; ++i) {
    if (got[i] != 0) tail_written = true;
  }
  EXPECT_TRUE(tail_written) << "取的是静态那一档的 VL=32，只写了前 128 B";
}

TEST(Vu, AddrSwitchesBetweenStaticAndDynamic) {
  // bit[1] / bit[2] 各控制 Load 与 Store 地址。这里只把 Store 地址走动态。
  Rig rig;
  std::vector<float> in = Tame(kVl, 0x310);
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(in));
  SetupChain(rig, 0, 0, kSrcLu);
  rig.Write(kVuStAddr, kDstAddr + 0x800);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0, kVuMaskStAddr));
  rig.Run(400);

  std::vector<float> got = Fp32Of(rig.stmem->Peek(kDstAddr + 0x800, kVl * 4));
  ASSERT_EQ(got.size(), in.size());
  EXPECT_EQ(numeric::BitsOf(got[0]), numeric::BitsOf(in[0]));
  // 静态副本给的那个地址不该被写。
  std::vector<uint8_t> stale = rig.stmem->Peek(kDstAddr, 16);
  for (uint8_t b : stale) EXPECT_EQ(b, 0u);
}

TEST(Vu, VlClampsAtBothEnds) {
  // 0 等效 1，大于 16384 等效 16384，都不报错。
  VuMacroInst a;
  a.mask = kVuMaskTypeVl;
  a.dyn.type_vl = 0;
  EXPECT_EQ(a.Vl(), 1u);
  VuMacroInst b;
  b.mask = kVuMaskTypeVl;
  b.dyn.type_vl = 20000;
  EXPECT_EQ(b.Vl(), kVuVlMax);
  VuMacroInst c;
  c.mask = kVuMaskTypeVl;
  c.dyn.type_vl = 16384;
  EXPECT_EQ(c.Vl(), 16384u);
}

TEST(Vu, EntriesFollowPrecision) {
  // VRF / MRF 的 entry 占用是 ⌈VL ÷ 32⌉（FP32）或 ⌈VL ÷ 64⌉（BF16）。
  VuMacroInst a;
  a.mask = kVuMaskTypeVl;
  a.dyn.type_vl = TypeVlWordOf(100, false);
  EXPECT_EQ(a.Entries(), 4u);
  VuMacroInst b;
  b.mask = kVuMaskTypeVl;
  b.dyn.type_vl = TypeVlWordOf(100, true);
  EXPECT_EQ(b.Entries(), 2u);
}

// ── 配置写阻塞 ──

TEST(Vu, StaticCfgWriteBlocksWhileReferenced) {
  // 目标静态配置组正被在飞的宏指令引用时，这次配置写阻塞在通路上，等它退休后
  // 才生效。所以在飞的那一条仍按改写前跑完。
  Rig rig;
  std::vector<float> in = Tame(kVl, 0x305);
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(in));
  SetupChain(rig, 0, 0, kSrcLu);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  // 紧接着改同一组的 SU_op：这一笔会被挡住，直到上一条退休。
  rig.WriteStatic(0, kVuSuOp, OpWord(uint64_t(SuOp::kNop)));
  rig.Run(600);

  std::vector<float> got = Fp32Of(rig.stmem->Peek(kDstAddr, kVl * 4));
  ASSERT_EQ(got.size(), in.size());
  EXPECT_EQ(numeric::BitsOf(got[0]), numeric::BitsOf(in[0]));
  EXPECT_TRUE(rig.writer->Idle());
  EXPECT_GT(rig.vu->Profile().Counter(VuCounter::kCfgWrStallCycle), 0u);
}

// ── Scoreboard 与派发 ──

TEST(Vu, ScoreboardStallsOnOverlap) {
  // 两条宏指令写同一段 VRF：后一条要等前一条退休。
  Rig rig;
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(Tame(kVl, 0x306)));
  rig.WriteStatic(0, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
  rig.WriteStatic(0, kVuPrfOp, PrfWord(kSrcLu, 0, 0, 0));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(kVl, false, numeric::RoundMode::kRne));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuLdAddr, kSrcAddr);
  rig.WriteStatic(0, kVuStaticDupOffset + kVuVrfWtIndex, IndexWord(0));
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(800);

  EXPECT_EQ(rig.sink->got.size(), 2u);
  EXPECT_GT(rig.vu->Profile().Counter(VuCounter::kStallDepCycle), 0u)
      << "两条写同一段 VRF 却一拍没等过";
}

TEST(Vu, FenceWaitsForAllPrior) {
  // MACRO_INST_FENCE 的那一条等此前全部宏指令完成才派发，哪怕两条读写的段
  // 完全不相干。CM 访存冲突硬件不追踪，靠的就是这一位。
  Rig rig;
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(Tame(kVl, 0x307)));
  SetupChain(rig, 0, 0, kSrcLu);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Write(kVuMacroInstTrigger, TriggerWord(0, 0, kVuTrigFence));
  rig.Run(800);

  EXPECT_EQ(rig.sink->got.size(), 2u);
  EXPECT_GT(rig.vu->Profile().Counter(VuCounter::kStallFenceCycle), 0u);
}

TEST(Vu, BroadcastWaitsForNonLoadPredecessors) {
  // DATA_BROADCAST 的那一条等除 CM-Load 外的前序宏指令全部完成才派发。前一条
  // 只读 CM，所以这一条不必等。
  Rig rig;
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(Tame(kVl, 0x308)));
  rig.WriteStatic(0, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(kVl, false, numeric::RoundMode::kRne));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuLdAddr, kSrcAddr);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Write(kVuMacroInstTrigger, TriggerWord(0, 0, kVuTrigBroadcast));
  rig.Run(800);

  EXPECT_EQ(rig.sink->got.size(), 2u);
  EXPECT_EQ(rig.vu->Profile().Counter(VuCounter::kStallBcastCycle), 0u)
      << "前一条只有 CM-Load，DATA_BROADCAST 不该等它";
}

// ── OPCODE 与异常 ──

TEST(Vu, UnassignedOpcodeIsNop) {
  // 未分配与本单元不支持的编码按无操作处理，与 0x00 等效，不置任何异常。
  // VALU2 只有 vmv.v.v 一条独有的，给它 VALU0 才有的除法就该当没写。
  Rig rig;
  std::vector<float> in = Tame(kVl, 0x309);
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(in));
  SetupChain(rig, kVuValu2Op, OpWord(uint64_t(ValuOp::kFdivVv), kSrcLu, kSrcLu),
             kSrcLu);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  ASSERT_EQ(rig.sink->got.size(), 1u);
  EXPECT_EQ(rig.vu->ConfigRegister().ErrorCode(), 0u);
  std::vector<float> got = Fp32Of(rig.stmem->Peek(kDstAddr, kVl * 4));
  for (size_t i = 0; i < in.size(); ++i) {
    EXPECT_EQ(numeric::BitsOf(got[i]), numeric::BitsOf(in[i])) << "i=" << i;
  }
}

TEST(Vu, VrfWritePortsMustDifferOrCfgError) {
  // 两个 VRF 写端口指向同一个执行单元 → CFG_ERROR。
  Rig rig;
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(Tame(kVl, 0x30A)));
  rig.WriteStatic(0, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
  rig.WriteStatic(0, kVuPrfOp, PrfWord(kSrcLu, kSrcLu, 0, 0));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(kVl, false, numeric::RoundMode::kRne));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuLdAddr, kSrcAddr);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  EXPECT_NE(rig.vu->ConfigRegister().ErrorCode() & kVuErrCfg, 0u);
}

TEST(Vu, MaskPortCannotBeSharedOrCfgError) {
  // MRF 只有两个读端口，且一个端口的数据不能广播给多个消费者：两个 VEXE 同选
  // p0 就是把一个端口的数据分给两处 → CFG_ERROR。
  Rig rig;
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(Tame(kVl, 0x30E)));
  rig.WriteStatic(0, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
  rig.WriteStatic(0, kVuValu0Op, OpWord(uint64_t(ValuOp::kFaddVv), kSrcLu, kSrcLu));
  rig.WriteStatic(0, kVuValu1Op, OpWord(uint64_t(ValuOp::kFaddVv), kSrcLu, kSrcLu));
  rig.WriteStatic(0, kVuMaskOp, MaskWord(kVuMaskP0, kVuMaskP0, 0, 0));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(kVl, false, numeric::RoundMode::kRne));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuLdAddr, kSrcAddr);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  EXPECT_NE(rig.vu->ConfigRegister().ErrorCode() & kVuErrCfg, 0u);
}

// ── LU / SU 的转换与舍入 ──

TEST(Vu, LoadFp32NarrowsToBf16UnderRoundMode) {
  // ld.fp32.vm 在 DATA_TYPE=BF16 下按 ROUND_MODE 把 FP32 窄化成 BF16。这是 LU
  // 唯一的高转低。
  Rig rig;
  std::vector<float> in = Tame(kVl, 0x30B);
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(in));

  rig.WriteStatic(0, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
  rig.WriteStatic(0, kVuSuOp, OpWord(uint64_t(SuOp::kStBf16), kSrcLu));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(kVl, true, numeric::RoundMode::kRtz));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuLdAddr, kSrcAddr);
  rig.WriteStatic(0, kVuStaticDupOffset + kVuStAddr, kDstAddr);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  std::vector<uint8_t> got = rig.stmem->Peek(kDstAddr, kVl * 2);
  for (uint64_t i = 0; i < kVl; ++i) {
    uint16_t want = numeric::NarrowBf16(in[i], numeric::RoundMode::kRtz);
    uint16_t have = uint16_t(got[i * 2]) | (uint16_t(got[i * 2 + 1]) << 8);
    EXPECT_EQ(have, want) << "i=" << i;
  }
}

TEST(Vu, CrossesBlockBoundary) {
  // 向量按 32 B 对齐，CM 请求按 128 B 对齐：起始地址落在块中间时，两端多读的
  // 部分由 LU 截掉，写回同理。
  Rig rig;
  const uint64_t off = kSrcAddr + 32;
  std::vector<float> in = Tame(kVl, 0x30C);
  rig.ldmem->Poke(off, Fp32Bytes(in));

  rig.WriteStatic(0, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
  rig.WriteStatic(0, kVuSuOp, OpWord(uint64_t(SuOp::kStFp32), kSrcLu));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(kVl, false, numeric::RoundMode::kRne));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuLdAddr, off);
  rig.WriteStatic(0, kVuStaticDupOffset + kVuStAddr, kDstAddr + 32);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  std::vector<float> got = Fp32Of(rig.stmem->Peek(kDstAddr + 32, kVl * 4));
  ASSERT_EQ(got.size(), in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    EXPECT_EQ(numeric::BitsOf(got[i]), numeric::BitsOf(in[i])) << "i=" << i;
  }
  std::vector<uint8_t> before = rig.stmem->Peek(kDstAddr, 32);
  for (uint8_t b : before) EXPECT_EQ(b, 0u);
}

TEST(Vu, MisalignedAddrRaisesCmAddrError) {
  // 向量按 32 B 对齐，违反置 CM_ADDR_ERROR。硬件只自检对齐、不做长度检查，
  // 本条照走不阻塞流水。
  Rig rig;
  rig.ldmem->Poke(kSrcAddr + 4, Fp32Bytes(Tame(kVl, 0x30F)));
  rig.WriteStatic(0, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(kVl, false, numeric::RoundMode::kRne));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuLdAddr, kSrcAddr + 4);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  EXPECT_EQ(rig.sink->got.size(), 1u) << "对齐不合规不该挡住这一条";
  EXPECT_NE(rig.vu->ConfigRegister().ErrorCode() & kVuErrCmAddr, 0u);
}

// ── 三条配置通路与后门 ──

TEST(Vu, ThreeCfgPathsShareOneRegisterView) {
  // VU-Core、Ctrl-NOC 与 Debug Module 共享同一份寄存器视图，流控彼此独立。
  Rig rig;
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(Tame(kVl, 0x30D)));

  auto noc = std::make_shared<DsaCfgPort>(rig.clk);
  rig.vu->ConfigRegister().AttachPath(VuCfgPath::kCtrlNoc, noc);
  CfgWriter noc_writer(rig.clk, noc);
  noc_writer.Push(kVuStaticBase + kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
  noc_writer.Push(kVuStaticBase + kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(kVl, false, numeric::RoundMode::kRne));
  noc_writer.Push(kVuStaticBase + kVuStaticDupOffset + kVuLdAddr, kSrcAddr);

  rig.vu->ConfigRegister().SetCoreIds(6, 12);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(500);

  ASSERT_EQ(rig.sink->got.size(), 1u);
  EXPECT_EQ(rig.sink->got[0].stream_id, 6u);
  EXPECT_EQ(rig.sink->got[0].task_id, 12u);
  EXPECT_EQ(noc_writer.Sent(), 3u);
}

TEST(Vu, RegFileBackdoor) {
  // reg_file_addr 选 RF 与 RF 内字节地址，reg_file_data 读写。这条通路与宏指令
  // 异步，由软件保证访问期间目标 RF 不被在飞的宏指令读写。
  Rig rig;
  rig.Write(kVuRegFileAddr, 0x40);       // VRF 字节地址 0x40
  rig.Write(kVuRegFileData, 0x3F800000); // 1.0f
  rig.Run(60);

  std::vector<float> got = rig.vu->Regfiles().ReadVrf(0, 32, false);
  ASSERT_EQ(got.size(), 32u);
  EXPECT_EQ(numeric::BitsOf(got[16]), 0x3F800000u);
}

}  // namespace

namespace {

TEST(Vu, StoreLeavesNeighbourBytesAlone) {
  // 起点落在 128 B 块中间时，这一块前面那一截属于相邻的数据，写回不该动它。
  Rig rig;
  std::vector<float> in = Tame(kVl, 0x400);
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(in));
  // 目的地址前面先摆一段可辨认的字节。
  std::vector<uint8_t> guard(32);
  for (uint64_t i = 0; i < guard.size(); ++i) guard[i] = uint8_t(0xA0 + i);
  rig.stmem->Poke(kDstAddr, guard);
  // 尾巴后面也摆一段。
  std::vector<uint8_t> tail(32, 0x5A);
  rig.stmem->Poke(kDstAddr + 32 + kVl * 4, tail);

  rig.WriteStatic(0, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
  rig.WriteStatic(0, kVuSuOp, OpWord(uint64_t(SuOp::kStFp32), kSrcLu));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuTypeVl,
                  TypeVlWord(kVl, false, numeric::RoundMode::kRne));
  rig.WriteStatic(0, kVuStaticDupOffset + kVuLdAddr, kSrcAddr);
  rig.WriteStatic(0, kVuStaticDupOffset + kVuStAddr, kDstAddr + 32);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  std::vector<float> got = Fp32Of(rig.stmem->Peek(kDstAddr + 32, kVl * 4));
  ASSERT_EQ(got.size(), in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    EXPECT_EQ(numeric::BitsOf(got[i]), numeric::BitsOf(in[i])) << "i=" << i;
  }
  EXPECT_EQ(rig.stmem->Peek(kDstAddr, 32), guard) << "起点前面那一截被写坏了";
  EXPECT_EQ(rig.stmem->Peek(kDstAddr + 32 + kVl * 4, 32), tail)
      << "末尾后面那一截被写坏了";
}

}  // namespace

namespace {

TEST(Vu, ProfileRunAndClear) {
  // RUN 置 0 时暂停计数并保持当前值，CLEAR 写 1 清零全部计数器。
  Rig rig;
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(Tame(kVl, 0x500)));
  SetupChain(rig, 0, 0, kSrcLu);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  rig.Run(400);

  uint64_t ran = rig.vu->Profile().Counter(VuCounter::kRunCycle);
  EXPECT_GT(ran, 0u);
  EXPECT_EQ(rig.vu->Profile().Counter(VuCounter::kMacroInstTotalNum), 1u);
  EXPECT_EQ(rig.vu->Profile().Counter(VuCounter::kMacroInstRetireNum), 1u);
  EXPECT_GT(rig.vu->Profile().Counter(VuCounter::kCmLdReqNum), 0u);
  EXPECT_GT(rig.vu->Profile().Counter(VuCounter::kCmStReqNum), 0u);
  EXPECT_GT(rig.vu->Profile().Counter(VuCounter::kLuBusyCycle), 0u);
}

TEST(Vu, ProfileStopsWhenRunCleared) {
  Rig rig;
  rig.ldmem->Poke(kSrcAddr, Fp32Bytes(Tame(kVl, 0x501)));
  SetupChain(rig, 0, 0, kSrcLu);
  rig.Write(kVuMacroInstTrigger, TriggerWord(0));
  // 跑一段之后关掉 RUN，此后计数保持不变。
  rig.Write(kVuProfileCtrl, 0);
  rig.Run(400);

  uint64_t ran = rig.vu->Profile().Counter(VuCounter::kRunCycle);
  EXPECT_LT(ran, 50u) << "RUN 关掉之后还在计数";
}

}  // namespace

// ── 与 reference/ 那一份的逐 bit 比对 ──

namespace {

// ffn.txt 里的一条 vu_reduce：LANES、VL、输入、期望。
struct ReduceCase {
  uint64_t lanes = 0, vl = 0;
  std::vector<float> in;
  float want = 0.0f;
};

std::vector<ReduceCase> ReadReduceCases() {
  std::vector<ReduceCase> out;
  std::ifstream f(std::string(LATCH_SOURCE_DIR) +
                  "/src/bach/compiler/reference/vectors/ffn.txt");
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream is(line);
    std::vector<std::string> col;
    std::string tok;
    while (is >> tok) col.push_back(tok);
    if (col.size() != 5 || col[0] != "vu_reduce") continue;
    ReduceCase c;
    c.lanes = std::stoull(col[1]);
    c.vl = std::stoull(col[2]);
    std::istringstream vs(col[3]);
    while (std::getline(vs, tok, ',')) {
      c.in.push_back(numeric::FloatOf(uint32_t(std::stoul(tok, nullptr, 16))));
    }
    c.want = numeric::FloatOf(uint32_t(std::stoul(col[4], nullptr, 16)));
    out.push_back(c);
  }
  return out;
}

}  // namespace

// 步 8 的判据换一个来源验一遍：期望值由 reference/ 那一份 Python 参考实现算，
// 不再由本文件里的 numeric:: 算。归约的顺序是结果的一部分，两份实现独立写出来
// 还能对上，这个顺序才算立住。
TEST(Vu, ReduceMatchesPythonReference) {
  std::vector<ReduceCase> cases = ReadReduceCases();
  ASSERT_FALSE(cases.empty())
      << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";
  for (ReduceCase const& c : cases) {
    ASSERT_EQ(c.lanes, kVuLanes) << "向量里的 LANES 与模型的对不上";
    Rig rig;
    rig.ldmem->Poke(kSrcAddr, Fp32Bytes(c.in));
    rig.vu->Regfiles().WriteSrf(2, 0.0f);

    rig.WriteStatic(0, kVuLuOp, OpWord(uint64_t(LuOp::kLdFp32)));
    rig.WriteStatic(0, kVuValu1Op,
                    OpWord(uint64_t(ValuOp::kRedusum), kSrcSrfP0 + 2, kSrcLu));
    rig.WriteStatic(0, kVuPrfOp, PrfWord(0, 0, 0, 1u << 1));
    rig.WriteStatic(0, kVuStaticDupOffset + kVuTypeVl,
                    TypeVlWord(c.vl, false, numeric::RoundMode::kRne));
    rig.WriteStatic(0, kVuStaticDupOffset + kVuLdAddr, kSrcAddr);
    rig.WriteStatic(0, kVuStaticDupOffset + kVuSrfRdIndex0, SrfWord(0, 0, 2));
    rig.WriteStatic(0, kVuStaticDupOffset + kVuSrfWtIndex0, SrfWord(0, 9));
    rig.Write(kVuMacroInstTrigger, TriggerWord(0));
    rig.Run(600);

    EXPECT_EQ(numeric::BitsOf(rig.vu->Regfiles().ReadSrf(9)),
              numeric::BitsOf(c.want))
        << "VL=" << c.vl;
  }
}
