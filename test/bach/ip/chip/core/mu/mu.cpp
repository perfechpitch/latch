// MU DSA 的行为基线。
//
// 步 8 的判据：逐条计算原语与参考实现逐 bit 比对。参考实现就写在这个文件里，
// 按硬件的累加顺序算 —— 浮点加法不结合，顺序是结果的一部分，换一个顺序比对就
// 过不去，所以参考实现不能图省事写成 std::inner_product。
//
// 三种输入格式各一条：BF16 无 block scale 走顺序加，MXFP8 与 MXFP4 走按块分组
// 累加。再加 AGU 的地址序、结果写回的编码，和整条装配跑一笔任务。

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include <deque>
#include <memory>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/ip/chip/core/mu/mu.h"

using namespace latch;
using namespace latch::bach;

namespace {

constexpr Time kPeriod = 1;

void EnsureSlots() { RT::Reset(8, 8); }

// ── 参考实现 ──
//
// 按硬件的顺序算一次原语：块内先把乘积加完再乘 scale，块间顺序加。
std::vector<float> Reference(MuTaskCfg const& cfg,
                             std::vector<uint8_t> const& token,
                             std::vector<uint8_t> const& weight,
                             std::vector<uint8_t> const& scale) {
  uint64_t k = cfg.PrimK();
  uint64_t n = cfg.PrimN();
  uint64_t block = numeric::ScaleBlockOf(cfg.dtype_ab);
  uint64_t elem_bits = numeric::ElemBitsOf(cfg.dtype_ab);

  std::vector<float> a = numeric::Decode(cfg.dtype_ab, token, k);
  std::vector<float> sc;
  if (block != 0) {
    sc = numeric::DecodeScale(cfg.dtype_ab, scale, k / block);
  }

  std::vector<float> out;
  for (uint64_t j = 0; j < n; ++j) {
    uint64_t col_bytes = k * elem_bits / 8;
    std::vector<uint8_t> col(weight.begin() + j * col_bytes,
                             weight.begin() + (j + 1) * col_bytes);
    std::vector<float> b = numeric::Decode(cfg.dtype_ab, col, k);

    // 乘积先逐个算出来存下，再加。写成 acc += a[i] * b[i] 的话编译器会合成
    // FMA，中间那一次舍入就没了，与硬件先乘后加的结果差一个 bit —— 逐 bit
    // 比对下这不是等价变形。
    std::vector<float> prod(k, 0.0f);
    for (uint64_t i = 0; i < k; ++i) prod[i] = a[i] * b[i];

    float acc = 0.0f;
    if (block == 0) {
      for (uint64_t i = 0; i < k; ++i) acc += prod[i];
    } else {
      for (uint64_t blk = 0; blk * block < k; ++blk) {
        float part = 0.0f;
        for (uint64_t i = blk * block; i < (blk + 1) * block; ++i) {
          part += prod[i];
        }
        acc += part * sc[blk];
      }
    }
    float r = numeric::ClampNanInf(acc);
    if (cfg.out_bf16) r = numeric::FromBf16(numeric::ToBf16(r));
    out.push_back(r);
  }
  return out;
}

// 造一组可复现的字节：线性同余，不用随机数，跑两次结果一样。
std::vector<uint8_t> Pattern(uint64_t n, uint64_t seed) {
  std::vector<uint8_t> v;
  uint64_t s = seed;
  for (uint64_t i = 0; i < n; ++i) {
    s = s * 1103515245u + 12345u;
    v.push_back(uint8_t((s >> 16) & 0xFFu));
  }
  return v;
}

// 一条 K=256 的累加链上，随便取的 BF16 会把结果推到 ±Inf，比对到最后只剩
// Clamp 那一档，看不出累加顺序对不对。所以指数按格式收进一个温和的区间：
// BF16 与 FP32 的值落在 0.25～4，E8M0 的 scale 落在 2^−7～2^7。
std::vector<uint8_t> TamePattern(numeric::DataType t, uint64_t count,
                                 uint64_t seed) {
  uint64_t bits = numeric::ElemBitsOf(t);
  std::vector<uint8_t> v = Pattern(count * bits / 8, seed);
  if (t == numeric::DataType::kMxfp8) {
    // E4M3 里指数与尾数都到顶那一格是 NaN。NaN 一进累加链，结果就只剩 Clamp
    // 后的那个最大值，符号还随传播路径变 —— 比对的是位，得先把它排除掉。
    for (uint8_t& b : v) {
      if ((b & 0x7Fu) == 0x7Fu) b = uint8_t(b & 0xFEu);
    }
    return v;
  }
  if (t != numeric::DataType::kBf16) return v;   // FP4 本来就有界，也没有 NaN
  for (uint64_t i = 0; i + 1 < v.size(); i += 2) {
    // BF16 的高字节装符号与指数的高 7 位，把指数压到 126～128。
    uint8_t sign = uint8_t(v[i + 1] & 0x80u);
    uint8_t exp = uint8_t(126 + (v[i] % 3));
    v[i + 1] = uint8_t(sign | (exp >> 1));
    v[i] = uint8_t(((exp & 1u) << 7) | (v[i] & 0x7Fu));
  }
  return v;
}

std::vector<uint8_t> TameScale(numeric::DataType t, uint64_t nblock,
                               uint64_t seed) {
  std::vector<uint8_t> v = Pattern(nblock, seed);
  for (uint8_t& b : v) {
    // E8M0：127 是 2^0，把它收进 2^−7～2^7。
    b = t == numeric::DataType::kMxfp8 ? uint8_t(120 + (b % 15)) : b;
  }
  return v;
}

// 按序把配置写发进去，看见 ready 才换下一笔。
class CfgWriter : public BachModule {
 public:
  CfgWriter(ClockPtr c, std::shared_ptr<DsaCfgPort> p)
      : BachModule(c, "cfg_writer"), port(std::move(p)) {}

  void Push(uint64_t addr, uint64_t data) { q.push_back({addr, data}); }

 protected:
  void Step() override {
    if (driving) {
      if (!port->Ready()) return;
      q.pop_front();
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
  uint64_t seq = 0;
};

// 探针：Logic64 在主线程读到的是 t=0 的值，要在协程里 snapshot。
class Probe : public BachModule {
 public:
  Probe(ClockPtr c, MatrixExe& unit) : BachModule(c, "probe"), exe(unit) {}
  uint64_t prims = 0;

 protected:
  void Step() override { prims = exe.Prims(); }

 private:
  MatrixExe& exe;
};

// 跑一次原语，把 matrix exe 的结果与参考实现逐 bit 比。
void CheckPrimitive(MuTaskCfg const& cfg, uint64_t seed) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  MatrixExe exe(clk, "exe");
  Probe probe(clk, exe);

  uint64_t k = cfg.PrimK(), n = cfg.PrimN();
  uint64_t elem_bits = numeric::ElemBitsOf(cfg.dtype_ab);
  uint64_t block = numeric::ScaleBlockOf(cfg.dtype_ab);
  std::vector<uint8_t> token = TamePattern(cfg.dtype_ab, k, seed);
  std::vector<uint8_t> weight = TamePattern(cfg.dtype_ab, k * n, seed + 7);
  std::vector<uint8_t> scale =
      block == 0 ? std::vector<uint8_t>()
                 : TameScale(cfg.dtype_ab, k / block, seed + 13);
  (void)elem_bits;

  exe.Issue(cfg, token, weight, scale);
  clk->Continue((kMuLaneDepth + 4) * kPeriod);
  RT::JoinAll();

  ASSERT_TRUE(exe.HasResult());
  MatrixExe::Result r = exe.TakeResult();
  std::vector<float> want = Reference(cfg, token, weight, scale);
  ASSERT_EQ(r.out.size(), want.size());
  for (size_t i = 0; i < want.size(); ++i) {
    EXPECT_EQ(numeric::BitsOf(r.out[i]), numeric::BitsOf(want[i]))
        << "第 " << i << " 个输出：得 " << r.out[i] << "，应当是 " << want[i];
  }
  EXPECT_EQ(probe.prims, 1u);
}

// ── 计算原语 ──

TEST(Mu, PrimitiveBf16) {
  MuTaskCfg cfg;
  cfg.dtype_ab = numeric::DataType::kBf16;
  CheckPrimitive(cfg, 0x1234);
}

TEST(Mu, PrimitiveMxfp8) {
  MuTaskCfg cfg;
  cfg.dtype_ab = numeric::DataType::kMxfp8;
  CheckPrimitive(cfg, 0x2345);
}

TEST(Mu, PrimitiveMxfp4) {
  MuTaskCfg cfg;
  cfg.dtype_ab = numeric::DataType::kMxfp4;
  CheckPrimitive(cfg, 0x3456);
}

TEST(Mu, PrimitiveOutBf16) {
  // DTYPE_C = 1：结果原位舍入截断成 BF16。
  MuTaskCfg cfg;
  cfg.dtype_ab = numeric::DataType::kBf16;
  cfg.out_bf16 = true;
  CheckPrimitive(cfg, 0x4567);
}

TEST(Mu, PrimitiveK128N64) {
  // 另一档物理阵列规格：1×K128×N64，输出带宽 256 B。
  MuTaskCfg cfg;
  cfg.prim_k128_n64 = true;
  cfg.dtype_ab = numeric::DataType::kMxfp8;
  EXPECT_EQ(cfg.PrimK(), 128u);
  EXPECT_EQ(cfg.PrimN(), 64u);
  CheckPrimitive(cfg, 0x5678);
}

TEST(Mu, AccumOrderIsPartOfResult) {
  // 换一个累加顺序，同一份输入算出来就不是同一个 bit。这条是「参考实现必须照抄
  // 硬件顺序」那句话的凭据。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  MatrixExe exe(clk, "exe");

  MuTaskCfg cfg;
  cfg.dtype_ab = numeric::DataType::kMxfp8;
  uint64_t k = cfg.PrimK(), n = cfg.PrimN();
  std::vector<uint8_t> token = TamePattern(cfg.dtype_ab, k, 0x9001);
  std::vector<uint8_t> weight = TamePattern(cfg.dtype_ab, k * n, 0x9002);
  std::vector<uint8_t> scale = TameScale(cfg.dtype_ab, k / 32, 0x9003);

  exe.Issue(cfg, token, weight, scale);
  clk->Continue((kMuLaneDepth + 4) * kPeriod);
  RT::JoinAll();
  MatrixExe::Result r = exe.TakeResult();

  // 逐个乘 scale 再顺序加：与按块分组累加算出来的位不一样。
  std::vector<float> a = numeric::Decode(cfg.dtype_ab, token, k);
  std::vector<float> sc = numeric::DecodeScale(cfg.dtype_ab, scale, k / 32);
  uint64_t differ = 0;
  for (uint64_t j = 0; j < n; ++j) {
    std::vector<uint8_t> col(weight.begin() + j * k, weight.begin() + (j + 1) * k);
    std::vector<float> b = numeric::Decode(cfg.dtype_ab, col, k);
    float other = 0.0f;
    for (uint64_t i = 0; i < k; ++i) other += a[i] * b[i] * sc[i / 32];
    if (numeric::BitsOf(other) != numeric::BitsOf(r.out[j])) ++differ;
  }
  EXPECT_GT(differ, 0u) << "换顺序算出来处处相同，说明这组输入挑不出差别";
}

// ── AGU ──

TEST(Mu, AguIteratesKThenN) {
  MuTaskCfg cfg;
  cfg.kblock = 3;
  cfg.nblock = 2;
  MuAgu agu(cfg);
  std::vector<std::pair<uint64_t, uint64_t>> got;
  for (int i = 0; i < 6; ++i) {
    MuStep s = agu.Next();
    got.push_back({s.k_idx, s.n_idx});
  }
  std::vector<std::pair<uint64_t, uint64_t>> want = {
      {0, 0}, {1, 0}, {2, 0}, {0, 1}, {1, 1}, {2, 1}};
  EXPECT_EQ(got, want);
  EXPECT_TRUE(agu.Done());
}

TEST(Mu, AguAddresses) {
  MuTaskCfg cfg;
  cfg.dtype_ab = numeric::DataType::kBf16;
  cfg.kblock = 2;
  cfg.nblock = 2;
  cfg.addr_token = 0x1000;
  cfg.addr_weight = 0x20000;
  cfg.addr_out = 0x8000;
  MuAgu agu(cfg);

  // token 按 tile_K 走，weight 按 (n × kblock + k) 走，结果按 tile_N 走。
  MuStep s0 = agu.Next();
  MuStep s1 = agu.Next();
  MuStep s2 = agu.Next();
  EXPECT_EQ(agu.TokenAddr(s0), 0x1000u);
  EXPECT_EQ(agu.TokenAddr(s1), 0x1000u + agu.TokenBytes());
  EXPECT_EQ(agu.TokenAddr(s2), 0x1000u);            // 第二个 tile_N，K 归零
  EXPECT_EQ(agu.WeightAddr(s2), 0x20000u + 2 * agu.WeightBytes());
  EXPECT_EQ(agu.OutAddr(s0), 0x8000u);
  EXPECT_EQ(agu.OutAddr(s2), 0x8000u + agu.OutBytes());
}

TEST(Mu, AguRejectsOutOfRange) {
  MuTaskCfg cfg;
  cfg.addr_token = 0x100000;
  MuAgu agu(cfg);
  MuStep s = agu.Next();
  EXPECT_FALSE(agu.CheckStep(s, 0x1000, 0x100000));
  // 没对齐也拒。
  MuTaskCfg odd;
  odd.addr_token = 0x1004;
  MuAgu agu2(odd);
  MuStep s2 = agu2.Next();
  EXPECT_FALSE(agu2.CheckStep(s2, 0x100000, 0x100000));
}

// ── 配置寄存器 ──

TEST(Mu, TriggerLatchesConfig) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  MuRegfile reg(clk, "regfile");
  auto port = std::make_shared<DsaCfgPort>(clk);
  reg.AttachCfg(port);

  // 扮演 RV core：逐拍写配置，最后写 SYS_CTRL 的 TASK_START 位。
  class Writer : public BachModule {
   public:
    Writer(ClockPtr c, std::shared_ptr<DsaCfgPort> p)
        : BachModule(c, "writer"), port(std::move(p)) {}

   protected:
    void Step() override {
      static const std::pair<uint64_t, uint64_t> kSeq[] = {
          {kMuTaskCfg, 0x9u},          // PRIM_TYPE=1、DTYPE_AB=01（MXFP8）
          {kMuTaskBlock, 0x00020003u}, // KBLOCK=3、NBLOCK=2
          {kMuAddrToken, 0x1000u},
          {kMuAddrOut, 0x8000u},
          {kMuStreamId, 5u},
          {kMuTaskId, 9u},
          {kMuSysCtrl, kMuTaskStart},
      };
      uint64_t now = CycleNow();
      if (now == 0 || now > 7) {
        port->Idle();
        return;
      }
      auto const& kv = kSeq[now - 1];
      port->Drive(kv.first, kv.second, now);
    }

   private:
    std::shared_ptr<DsaCfgPort> port;
  };
  Writer w(clk, port);

  clk->Continue((12) * kPeriod);
  RT::JoinAll();

  ASSERT_TRUE(reg.HasPending());
  MuTaskCfg cfg = reg.TakePending();
  EXPECT_TRUE(cfg.prim_k128_n64);
  EXPECT_EQ(cfg.dtype_ab, numeric::DataType::kMxfp8);
  EXPECT_EQ(cfg.kblock, 3u);
  EXPECT_EQ(cfg.nblock, 2u);
  EXPECT_EQ(cfg.addr_token, 0x1000u);
  EXPECT_EQ(cfg.addr_out, 0x8000u);
  EXPECT_EQ(cfg.stream_id, 5u);
  EXPECT_EQ(cfg.task_id, 9u);
  EXPECT_FALSE(reg.HasPending());
}

// ── 结果写回 ──

TEST(Mu, StqEncodesByDtypeC) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  MuStq stq(clk, "stq");

  // 扮演 Core Mem：一律收得下。
  class MemSide : public BachModule {
   public:
    MemSide(ClockPtr c, MemPort& p) : BachModule(c, "mem"), port(p) {}
    std::vector<uint8_t> got;
    uint64_t at = 0;

   protected:
    void Step() override {
      if (port.req_valid.Get() != 0 && port.req_we.Get() != 0) {
        ByteBlockPtr d = port.req_wdata.Get();
        if (d && got.empty()) {
          at = port.req_addr.Get();
          got = *d;
        }
      }
      port.DriveSlave(true, false, ByteBlockPtr());
    }

   private:
    MemPort& port;
  };
  MemSide mem(clk, stq.Port());

  std::vector<float> vals = {1.0f, -2.5f, 0.125f};
  stq.Push(0x8000, vals, false);
  clk->Continue((2100) * kPeriod);
  RT::JoinAll();

  // FP32 小端逐字节。
  ASSERT_GE(mem.got.size(), vals.size() * 4);
  EXPECT_EQ(mem.at, 0x8000u);
  for (size_t i = 0; i < vals.size(); ++i) {
    uint32_t b = 0;
    for (int k = 0; k < 4; ++k) b |= uint32_t(mem.got[i * 4 + k]) << (8 * k);
    EXPECT_EQ(b, numeric::BitsOf(vals[i])) << "i=" << i;
  }
}

}  // namespace

namespace {

// ── 整条装配 ──

// 扮演 Core Mem 或 Matrix Mem：一块字节数组，收得下、隔若干拍回响应。
// scale_en 置位时把 scale 段附在正文之后一起回。
class MuMem : public BachModule {
 public:
  MuMem(ClockPtr c, const std::string& name, MemPort& p, uint64_t bytes,
        uint64_t delay)
      : BachModule(c, name), port(p), mem(bytes, 0), latency(delay) {}

  std::vector<uint8_t> mem;
  // scale 与 token 一一映射：第 i 段 token 配第 i 段 scale。段号要从 token 的
  // 基址算起，直接拿绝对地址除会落到别处去。
  uint64_t token_base = 0, scale_base = 0, scale_stride = 0;
  uint64_t reads = 0, writes = 0;

  void Poke(uint64_t at, std::vector<uint8_t> const& v) {
    for (uint64_t i = 0; i < v.size() && at + i < mem.size(); ++i) mem[at + i] = v[i];
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
    if (!pipe.empty() && pipe.front().first <= now) {
      rsp = pipe.front().second;
      valid = true;
      pipe.pop_front();
    }
    port.DriveSlave(true, valid, rsp);

    // 不按 (addr, bytes) 去重：master 每拍要么发一笔要么 IdleReq，端口上不会
    // 回落成上一拍的值，所以每一拍的 req_valid 都是一笔真请求。同一个地址连着
    // 写两拍是两笔 —— 同一个 tile_N 的几个 tile 就是这样叠写同一段的。
    if (port.req_valid.Get() == 0) return;
    uint64_t addr = port.req_addr.Get();
    uint64_t n = port.req_bytes.Get();
    bool we = port.req_we.Get() != 0;

    if (we) {
      ByteBlockPtr d = port.req_wdata.Get();
      if (d) {
        uint64_t woff = port.req_woff.Get();
        std::vector<uint8_t> seg;
        for (uint64_t i = 0; i < n && woff + i < d->size(); ++i) {
          seg.push_back((*d)[woff + i]);
        }
        Poke(addr + woff, seg);
      }
      ++writes;
      return;
    }
    auto data = std::make_shared<ByteBlock>(Peek(addr, n));
    if (port.req_scale_en.Get() != 0 && scale_stride != 0) {
      // scale 与数据一一映射：一段 token 对应一段 scale。
      uint64_t at = scale_base + ((addr - token_base) / n) * scale_stride;
      std::vector<uint8_t> sc = Peek(at, scale_stride);
      data->insert(data->end(), sc.begin(), sc.end());
    }
    pipe.push_back({now + latency, data});
    ++reads;
  }

 private:
  MemPort& port;
  std::deque<std::pair<uint64_t, ByteBlockPtr>> pipe;
  uint64_t latency;
};

class MuDoneSink : public BachModule {
 public:
  MuDoneSink(ClockPtr c, DonePort& p) : BachModule(c, "done"), port(p) {}
  std::vector<std::pair<uint64_t, uint64_t>> got;

 protected:
  void Step() override {
    if (port.Valid()) got.push_back({port.stream_id.Get(), port.task_id.Get()});
  }

 private:
  DonePort& port;
};

class MuDriver : public BachModule {
 public:
  MuDriver(ClockPtr c, Mu& target) : BachModule(c, "driver"), mu(target) {}

 protected:
  void Step() override { mu.RunStep(); }

 private:
  Mu& mu;
};

// topK 表在 Core Mem 里的样子：每项 {expert_id 2 B, weight 4 B}。
std::vector<uint8_t> TopkBytes(std::vector<TopkEntry> const& t) {
  std::vector<uint8_t> b(t.size() * kTopkEntryBytes, 0);
  for (size_t i = 0; i < t.size(); ++i) {
    uint64_t at = i * kTopkEntryBytes;
    b[at] = uint8_t(t[i].expert_id & 0xFFu);
    b[at + 1] = uint8_t((t[i].expert_id >> 8) & 0xFFu);
    uint32_t w = numeric::BitsOf(t[i].weight);
    for (int k = 0; k < 4; ++k) b[at + 2 + k] = uint8_t((w >> (8 * k)) & 0xFFu);
  }
  return b;
}

// 一个 tile 的最小用例：装配跑通的最短路径，出了差别先在这里定位。
TEST(Mu, AssemblySingleTile) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  MuCfg setting;
  Mu mu(clk, "mu", setting);

  auto cfg_port = std::make_shared<DsaCfgPort>(clk);
  auto cmem_rd = std::make_shared<MemPort>(clk);
  auto mmem_rd = std::make_shared<MemPort>(clk);
  auto cmem_wr = std::make_shared<MemPort>(clk);
  mu.AttachCfg(cfg_port);
  mu.AttachCmemRd(cmem_rd);
  mu.AttachMmemRd(mmem_rd);
  mu.AttachCmemWr(cmem_wr);

  MuTaskCfg want;
  want.dtype_ab = numeric::DataType::kBf16;   // 无 block scale，先排除 scale
  want.kblock = 1;
  want.nblock = 1;
  want.addr_token = 0x1000;
  want.addr_weight = 0x0;
  want.addr_out = 0x20000;
  want.stream_id = 2;
  want.task_id = 3;

  uint64_t k = want.PrimK(), n = want.PrimN();
  MuMem cmem(clk, "cmem", *cmem_rd, 0x40000, 13);
  MuMem mmem(clk, "mmem", *mmem_rd, 0x80000, 16);
  MuMem outmem(clk, "outmem", *cmem_wr, 0x40000, 13);

  std::vector<uint8_t> token = TamePattern(want.dtype_ab, k, 0x901);
  std::vector<uint8_t> weight = TamePattern(want.dtype_ab, k * n, 0x902);
  cmem.Poke(want.addr_token, token);
  mmem.Poke(want.addr_weight, weight);

  CfgWriter writer(clk, cfg_port);
  writer.Push(kMuTaskCfg, 0);
  writer.Push(kMuTaskBlock, 1u | (1u << 16));
  writer.Push(kMuAddrToken, want.addr_token);
  writer.Push(kMuAddrWeight, want.addr_weight);
  writer.Push(kMuAddrOut, want.addr_out);
  writer.Push(kMuStreamId, want.stream_id);
  writer.Push(kMuTaskId, want.task_id);
  writer.Push(kMuSysCtrl, kMuTaskStart);

  MuDoneSink sink(clk, mu.Done());
  MuDriver driver(clk, mu);
  clk->Continue(2000 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink.got.size(), 1u);
  std::vector<float> ref = Reference(want, token, weight, {});
  std::vector<uint8_t> raw = outmem.Peek(want.addr_out, n * 4);
  for (uint64_t j = 0; j < n; ++j) {
    uint32_t b = 0;
    for (int t = 0; t < 4; ++t) b |= uint32_t(raw[j * 4 + t]) << (8 * t);
    EXPECT_EQ(b, numeric::BitsOf(ref[j])) << "j=" << j;
  }
}

// 几个专家合并成一份：C = C + (A × B) × W_ep。第一个专家直接写 C，后面的按
// topK 权重逐个累加。FC2 走的就是这一档。
TEST(Mu, ExpertReduceWeightsAndSums) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  MuCfg setting;
  Mu mu(clk, "mu", setting);

  auto cfg_port = std::make_shared<DsaCfgPort>(clk);
  auto cmem_rd = std::make_shared<MemPort>(clk);
  auto mmem_rd = std::make_shared<MemPort>(clk);
  auto cmem_wr = std::make_shared<MemPort>(clk);
  mu.AttachCfg(cfg_port);
  mu.AttachCmemRd(cmem_rd);
  mu.AttachMmemRd(mmem_rd);
  mu.AttachCmemWr(cmem_wr);

  MuTaskCfg want;
  want.dtype_ab = numeric::DataType::kBf16;
  want.kblock = 1;
  want.nblock = 1;
  want.expert_count = 2;
  want.ep_reduce = true;
  want.ac_expert_stride = 0x800;
  want.b_expert_stride = 0x10000;
  want.addr_token = 0x1000;
  want.addr_weight = 0x0;
  want.addr_out = 0x20000;
  want.stream_id = 5;
  want.task_id = 7;

  uint64_t k = want.PrimK(), n = want.PrimN();
  MuMem cmem(clk, "cmem", *cmem_rd, 0x40000, 13);
  MuMem mmem(clk, "mmem", *mmem_rd, 0x80000, 16);
  MuMem outmem(clk, "outmem", *cmem_wr, 0x40000, 13);

  // 每个专家一份激活、一份权重。合并那一档的激活按 topK 的先后排，权重按专家
  // 在本组内的序号排。
  std::vector<std::vector<uint8_t>> tok, wgt;
  for (uint64_t e = 0; e < want.expert_count; ++e) {
    tok.push_back(TamePattern(want.dtype_ab, k, 0xA10 + e));
    wgt.push_back(TamePattern(want.dtype_ab, k * n, 0xA20 + e));
    cmem.Poke(want.addr_token + e * want.ac_expert_stride, tok.back());
  }
  // topK 里第 0 个是全局 17 号专家、组内第 1 个，第 1 个是全局 5 号、组内第 0 个。
  // 表本身放在 Core Mem 的 topK 区，MU 在任务启动时自己读进来。
  mu.EpInfo().SetLocalEpTable({5, 17});
  const float w0 = 0.75f, w1 = 0.25f;
  want.topk_addr = 0x30000;
  want.topk_stride = kTopkBytesPerStream;
  cmem.Poke(want.topk_addr + want.stream_id * want.topk_stride,
            TopkBytes({{17, w0}, {5, w1}}));
  mmem.Poke(want.addr_weight + 1 * want.b_expert_stride, wgt[0]);
  mmem.Poke(want.addr_weight + 0 * want.b_expert_stride, wgt[1]);

  CfgWriter writer(clk, cfg_port);
  writer.Push(kMuTaskCfg, 0);
  writer.Push(kMuTaskBlock, 1u | (1u << 16));
  writer.Push(kMuAddrToken, want.addr_token);
  writer.Push(kMuAddrWeight, want.addr_weight);
  writer.Push(kMuAddrOut, want.addr_out);
  writer.Push(kMuAcExpertStride, want.ac_expert_stride);
  writer.Push(kMuBExpertStride, want.b_expert_stride);
  writer.Push(kMuEpCtrl, want.expert_count | kMuEpReduceEn);
  writer.Push(kMuTopkAddr, want.topk_addr);
  writer.Push(kMuTopkStride, want.topk_stride);
  writer.Push(kMuStreamId, want.stream_id);
  writer.Push(kMuTaskId, want.task_id);
  writer.Push(kMuSysCtrl, kMuTaskStart);

  MuDoneSink sink(clk, mu.Done());
  MuDriver driver(clk, mu);
  clk->Continue(2000 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink.got.size(), 1u) << "两个专家合成一笔，只报一次 dsa_done";

  // 先各自算完一列，各乘自己的权重，再顺序相加。
  std::vector<float> p0 = Reference(want, tok[0], wgt[0], {});
  std::vector<float> p1 = Reference(want, tok[1], wgt[1], {});
  std::vector<uint8_t> raw = outmem.Peek(want.addr_out, n * 4);
  for (uint64_t j = 0; j < n; ++j) {
    float a = numeric::ClampNanInf(p0[j] * w0);
    float b = numeric::ClampNanInf(p1[j] * w1);
    float ref = numeric::ClampNanInf(a + b);
    uint32_t got = 0;
    for (int t = 0; t < 4; ++t) got |= uint32_t(raw[j * 4 + t]) << (8 * t);
    EXPECT_EQ(got, numeric::BitsOf(ref)) << "j=" << j;
  }
  // 合并成一份，所以只写了一列。
  std::vector<uint8_t> beyond = outmem.Peek(want.addr_out + n * 4, 4);
  EXPECT_EQ(beyond[0], 0u) << "第二个专家不该另写一份";
}

// 每个专家各出一份：FC1 与 FC3 走这一档，结果地址按 topK 的先后隔开。
TEST(Mu, ExpertsWriteSeparateResults) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  MuCfg setting;
  Mu mu(clk, "mu", setting);

  auto cfg_port = std::make_shared<DsaCfgPort>(clk);
  auto cmem_rd = std::make_shared<MemPort>(clk);
  auto mmem_rd = std::make_shared<MemPort>(clk);
  auto cmem_wr = std::make_shared<MemPort>(clk);
  mu.AttachCfg(cfg_port);
  mu.AttachCmemRd(cmem_rd);
  mu.AttachMmemRd(mmem_rd);
  mu.AttachCmemWr(cmem_wr);

  MuTaskCfg want;
  want.dtype_ab = numeric::DataType::kBf16;
  want.kblock = 1;
  want.nblock = 1;
  want.expert_count = 2;
  want.ep_reduce = false;
  want.ac_expert_stride = 0x800;
  want.b_expert_stride = 0x10000;
  want.addr_token = 0x1000;
  want.addr_weight = 0x0;
  want.addr_out = 0x20000;
  want.stream_id = 6;
  want.task_id = 8;

  uint64_t k = want.PrimK(), n = want.PrimN();
  MuMem cmem(clk, "cmem", *cmem_rd, 0x40000, 13);
  MuMem mmem(clk, "mmem", *mmem_rd, 0x80000, 16);
  MuMem outmem(clk, "outmem", *cmem_wr, 0x40000, 13);

  // 各出一份那一档，几个专家共用同一份激活。
  std::vector<uint8_t> token = TamePattern(want.dtype_ab, k, 0xB01);
  cmem.Poke(want.addr_token, token);
  std::vector<std::vector<uint8_t>> wgt;
  for (uint64_t e = 0; e < want.expert_count; ++e) {
    wgt.push_back(TamePattern(want.dtype_ab, k * n, 0xB10 + e));
  }
  mu.EpInfo().SetLocalEpTable({5, 17});
  want.topk_addr = 0x30000;
  want.topk_stride = kTopkBytesPerStream;
  cmem.Poke(want.topk_addr + want.stream_id * want.topk_stride,
            TopkBytes({{17, 1.0f}, {5, 1.0f}}));
  mmem.Poke(want.addr_weight + 1 * want.b_expert_stride, wgt[0]);
  mmem.Poke(want.addr_weight + 0 * want.b_expert_stride, wgt[1]);

  CfgWriter writer(clk, cfg_port);
  writer.Push(kMuTaskCfg, 0);
  writer.Push(kMuTaskBlock, 1u | (1u << 16));
  writer.Push(kMuAddrToken, want.addr_token);
  writer.Push(kMuAddrWeight, want.addr_weight);
  writer.Push(kMuAddrOut, want.addr_out);
  writer.Push(kMuAcExpertStride, want.ac_expert_stride);
  writer.Push(kMuBExpertStride, want.b_expert_stride);
  writer.Push(kMuEpCtrl, want.expert_count);
  writer.Push(kMuTopkAddr, want.topk_addr);
  writer.Push(kMuTopkStride, want.topk_stride);
  writer.Push(kMuStreamId, want.stream_id);
  writer.Push(kMuTaskId, want.task_id);
  writer.Push(kMuSysCtrl, kMuTaskStart);

  MuDoneSink sink(clk, mu.Done());
  MuDriver driver(clk, mu);
  clk->Continue(2000 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink.got.size(), 1u);
  for (uint64_t e = 0; e < want.expert_count; ++e) {
    std::vector<float> ref = Reference(want, token, wgt[e], {});
    std::vector<uint8_t> raw =
        outmem.Peek(want.addr_out + e * want.ac_expert_stride, n * 4);
    for (uint64_t j = 0; j < n; ++j) {
      uint32_t got = 0;
      for (int t = 0; t < 4; ++t) got |= uint32_t(raw[j * 4 + t]) << (8 * t);
      EXPECT_EQ(got, numeric::BitsOf(ref[j])) << "e=" << e << " j=" << j;
    }
  }
}

TEST(Mu, AssemblyRunsAllTiles) {
  // 一笔任务是 kblock × nblock 次原语。整条装配跑完，每个 tile 的结果都要与
  // 参考实现逐 bit 相同，dsa_done 只报一次。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  MuCfg setting;
  Mu mu(clk, "mu", setting);

  auto cfg_port = std::make_shared<DsaCfgPort>(clk);
  auto cmem_rd = std::make_shared<MemPort>(clk);
  auto mmem_rd = std::make_shared<MemPort>(clk);
  auto cmem_wr = std::make_shared<MemPort>(clk);
  mu.AttachCfg(cfg_port);
  mu.AttachCmemRd(cmem_rd);
  mu.AttachMmemRd(mmem_rd);
  mu.AttachCmemWr(cmem_wr);

  MuTaskCfg want;
  want.dtype_ab = numeric::DataType::kMxfp8;
  want.kblock = 2;
  want.nblock = 2;
  want.addr_token = 0x1000;
  want.addr_weight = 0x0;
  want.addr_scale = 0x8000;
  want.addr_out = 0x20000;
  want.stream_id = 4;
  want.task_id = 11;

  uint64_t k = want.PrimK(), n = want.PrimN();
  uint64_t tok_bytes = k;                 // MXFP8 一个元素一字节
  uint64_t wgt_bytes = k * n;
  uint64_t sc_bytes = k / 32;

  MuMem cmem(clk, "cmem", *cmem_rd, 0x40000, 13);
  MuMem mmem(clk, "mmem", *mmem_rd, 0x40000, 16);
  MuMem outmem(clk, "outmem", *cmem_wr, 0x40000, 13);
  cmem.token_base = want.addr_token;
  cmem.scale_base = want.addr_scale;
  cmem.scale_stride = sc_bytes;

  // 每个 tile_K 一份 token 与一份 scale，每个 (n, k) 一份 weight。
  for (uint64_t ki = 0; ki < want.kblock; ++ki) {
    cmem.Poke(want.addr_token + ki * tok_bytes,
              TamePattern(want.dtype_ab, k, 0x600 + ki));
    cmem.Poke(want.addr_scale + ki * sc_bytes,
              TameScale(want.dtype_ab, sc_bytes, 0x700 + ki));
  }
  for (uint64_t i = 0; i < want.kblock * want.nblock; ++i) {
    mmem.Poke(want.addr_weight + i * wgt_bytes,
              TamePattern(want.dtype_ab, k * n, 0x800 + i));
  }

  CfgWriter writer(clk, cfg_port);
  writer.Push(kMuTaskCfg, uint64_t(1) << 3);            // DTYPE_AB = 01（MXFP8）
  writer.Push(kMuTaskBlock, want.kblock | (want.nblock << 16));
  writer.Push(kMuAddrToken, want.addr_token);
  writer.Push(kMuAddrWeight, want.addr_weight);
  writer.Push(kMuAddrScale, want.addr_scale);
  writer.Push(kMuAddrOut, want.addr_out);
  writer.Push(kMuStreamId, want.stream_id);
  writer.Push(kMuTaskId, want.task_id);
  writer.Push(kMuSysCtrl, kMuTaskStart);

  MuDoneSink sink(clk, mu.Done());
  MuDriver driver(clk, mu);
  clk->Continue(3000 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink.got.size(), 1u) << "一笔任务只报一次 dsa_done";
  EXPECT_EQ(sink.got[0].first, want.stream_id);
  EXPECT_EQ(sink.got[0].second, want.task_id);

  // 逐个 tile_N 比对。AGU 先循环 tile_K 再循环 tile_N，同一个 tile_N 的
  // kblock 个 tile 是这一列切出来的几段，各段的部分和顺序相加才是这一列的值。
  for (uint64_t ni = 0; ni < want.nblock; ++ni) {
    std::vector<float> ref;
    for (uint64_t ki = 0; ki < want.kblock; ++ki) {
      std::vector<uint8_t> token = TamePattern(want.dtype_ab, k, 0x600 + ki);
      std::vector<uint8_t> scale =
          TameScale(want.dtype_ab, sc_bytes, 0x700 + ki);
      std::vector<uint8_t> weight =
          TamePattern(want.dtype_ab, k * n, 0x800 + (ni * want.kblock + ki));
      std::vector<float> part = Reference(want, token, weight, scale);
      if (ki == 0) {
        ref = part;
        continue;
      }
      for (uint64_t j = 0; j < n; ++j) {
        ref[j] = numeric::ClampNanInf(ref[j] + part[j]);
      }
    }

    std::vector<uint8_t> raw = outmem.Peek(want.addr_out + ni * n * 4, n * 4);
    for (uint64_t j = 0; j < n; ++j) {
      uint32_t b = 0;
      for (int t = 0; t < 4; ++t) b |= uint32_t(raw[j * 4 + t]) << (8 * t);
      EXPECT_EQ(b, numeric::BitsOf(ref[j])) << "tile_N=" << ni << " j=" << j;
    }
  }
}

}  // namespace

// ── 与 reference/ 那一份的逐 bit 比对 ──

namespace {

std::string VectorPath() {
  return std::string(LATCH_SOURCE_DIR) + "/src/bach/compiler/reference/vectors/ffn.txt";
}

std::vector<uint8_t> HexBytes(std::string const& s) {
  std::vector<uint8_t> out;
  if (s == "-") return out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    out.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
  }
  return out;
}

std::vector<float> HexFloats(std::string const& s) {
  std::vector<float> out;
  std::istringstream is(s);
  std::string tok;
  while (std::getline(is, tok, ',')) {
    out.push_back(numeric::FloatOf(uint32_t(std::stoul(tok, nullptr, 16))));
  }
  return out;
}

numeric::DataType TypeByName(std::string const& name) {
  if (name == "bf16") return numeric::DataType::kBf16;
  if (name == "mxfp8") return numeric::DataType::kMxfp8;
  if (name == "mxfp4") return numeric::DataType::kMxfp4;
  if (name == "nvfp4") return numeric::DataType::kNvfp4;
  return numeric::DataType::kFp32;
}

// ffn.txt 里的一条 gemm。
struct GemmCase {
  numeric::DataType dtype = numeric::DataType::kBf16;
  uint64_t k = 0, n = 0;
  bool out_bf16 = false;
  std::vector<uint8_t> token, weight, scale;
  std::vector<float> want;
};

// 只取 MU 那两条原语形状的：K=256×N=32 与 K=128×N=64。别的形状 matrix exe
// 发不出去。
std::vector<GemmCase> ReadPrimCases() {
  std::vector<GemmCase> out;
  std::ifstream f(VectorPath());
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream is(line);
    std::vector<std::string> col;
    std::string tok;
    while (is >> tok) col.push_back(tok);
    if (col.size() != 9 || col[0] != "gemm") continue;
    GemmCase c;
    c.k = std::stoull(col[2]);
    c.n = std::stoull(col[3]);
    if (!((c.k == 256 && c.n == 32) || (c.k == 128 && c.n == 64))) continue;
    c.dtype = TypeByName(col[1]);
    c.out_bf16 = col[4] == "1";
    c.token = HexBytes(col[5]);
    c.weight = HexBytes(col[6]);
    c.scale = HexBytes(col[7]);
    c.want = HexFloats(col[8]);
    out.push_back(c);
  }
  return out;
}

}  // namespace

// 步 8 的判据换一个来源验一遍：期望值不再由本文件里的 Reference 算，而是读
// reference/ 那一份 Python 参考实现产出的向量。两份实现独立写出来，模型算的
// 结果要与它逐 bit 相同。
TEST(Mu, MatchesPythonReference) {
  std::vector<GemmCase> cases = ReadPrimCases();
  ASSERT_FALSE(cases.empty())
      << "比对向量没生成，先跑 src/bach/compiler/reference/vectors.py";
  for (GemmCase const& c : cases) {
    MuTaskCfg cfg;
    cfg.dtype_ab = c.dtype;
    cfg.out_bf16 = c.out_bf16;
    cfg.prim_k128_n64 = (c.k == 128);
    ASSERT_EQ(cfg.PrimK(), c.k);
    ASSERT_EQ(cfg.PrimN(), c.n);

    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    MatrixExe exe(clk, "exe");
    exe.Issue(cfg, c.token, c.weight, c.scale);
    clk->Continue((kMuLaneDepth + 4) * kPeriod);
    RT::JoinAll();

    ASSERT_TRUE(exe.HasResult());
    MatrixExe::Result r = exe.TakeResult();
    ASSERT_EQ(r.out.size(), c.want.size());
    for (size_t i = 0; i < c.want.size(); ++i) {
      EXPECT_EQ(numeric::BitsOf(r.out[i]), numeric::BitsOf(c.want[i]))
          << "K=" << c.k << " N=" << c.n << " 第 " << i << " 个输出";
    }
    RT::Reset();
  }
}
