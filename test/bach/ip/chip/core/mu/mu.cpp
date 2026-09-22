// MU DSA 的行为基线。
//
// 步 8 的判据：逐条计算原语与参考实现逐 bit 比对。参考实现就写在这个文件里，
// 按硬件的累加顺序算：浮点加法不结合，顺序是结果的一部分，换一个顺序比对就
// 过不去，所以参考实现不能图省事写成 std::inner_product。
//
// 三种输入格式各一条：BF16 无 block scale 走顺序加，MXFP8 与 MXFP4 走按块分组
// 累加，MXFP8 × MXFP8 再加权重的 scale。物理阵列是 1×K128×N64，vlane = 2 时是
// 1×K64×N128。再加 AGU 的地址序、结果写回的编码，和整条装配跑一笔任务。

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include <deque>
#include <map>
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
// 按硬件的顺序算一次原语：块内先把乘积加完再乘 scale，块间顺序加。权重也带
// scale 时（按列排，一列 K / block 个）块内部分和乘两个 scale 之积。
std::vector<float> Reference(MuTaskCfg const& cfg,
                             std::vector<uint8_t> const& token,
                             std::vector<uint8_t> const& weight,
                             std::vector<uint8_t> const& scale,
                             std::vector<uint8_t> const& wscale = {}) {
  uint64_t k = cfg.PrimK();
  uint64_t n = cfg.PrimN();
  uint64_t block = numeric::ScaleBlockOf(cfg.a_dtype);
  uint64_t elem_bits = numeric::ElemBitsOf(cfg.b_dtype);

  std::vector<float> a = numeric::Decode(cfg.a_dtype, token, k);
  std::vector<float> sc;
  if (block != 0) {
    sc = numeric::DecodeScale(cfg.a_dtype, scale, k / block);
  }

  std::vector<float> out;
  for (uint64_t j = 0; j < n; ++j) {
    uint64_t col_bytes = k * elem_bits / 8;
    std::vector<uint8_t> col(weight.begin() + j * col_bytes,
                             weight.begin() + (j + 1) * col_bytes);
    std::vector<float> b = numeric::Decode(cfg.b_dtype, col, k);

    // 乘积先逐个算出来存下，再加。写成 acc += a[i] * b[i] 的话编译器会合成
    // FMA，中间那一次舍入就没了，与硬件先乘后加的结果差一个 bit，逐 bit
    // 比对下这不是等价变形。
    std::vector<float> prod(k, 0.0f);
    for (uint64_t i = 0; i < k; ++i) prod[i] = a[i] * b[i];

    float acc = 0.0f;
    if (block == 0) {
      for (uint64_t i = 0; i < k; ++i) acc += prod[i];
    } else {
      uint64_t nb = k / block;
      for (uint64_t blk = 0; blk * block < k; ++blk) {
        float part = 0.0f;
        for (uint64_t i = blk * block; i < (blk + 1) * block; ++i) {
          part += prod[i];
        }
        float s = sc[blk];
        if (!wscale.empty()) {
          std::vector<uint8_t> one = {wscale[j * nb + blk]};
          s = s * numeric::DecodeScale(cfg.b_dtype, one, 1)[0];
        }
        acc += part * s;
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
    // 后的那个最大值，符号还随传播路径变。比对的是位，得先把它排除掉。
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

// 跑一次原语，把 matrix exe 的结果与参考实现逐 bit 比。with_wscale 为真时权重
// 也带 scale（MXFP8 × MXFP8）。
void CheckPrimitive(MuTaskCfg const& cfg, uint64_t seed,
                    bool with_wscale = false) {
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  MatrixExe exe(clk, "exe");
  Probe probe(clk, exe);

  uint64_t k = cfg.PrimK(), n = cfg.PrimN();
  uint64_t elem_bits = numeric::ElemBitsOf(cfg.b_dtype);
  uint64_t block = numeric::ScaleBlockOf(cfg.a_dtype);
  std::vector<uint8_t> token = TamePattern(cfg.a_dtype, k, seed);
  std::vector<uint8_t> weight = TamePattern(cfg.b_dtype, k * n, seed + 7);
  std::vector<uint8_t> scale =
      block == 0 ? std::vector<uint8_t>()
                 : TameScale(cfg.a_dtype, k / block, seed + 13);
  std::vector<uint8_t> wscale =
      with_wscale ? TameScale(cfg.b_dtype, n * (k / block), seed + 17)
                  : std::vector<uint8_t>();
  (void)elem_bits;

  exe.Issue(cfg, token, weight, scale, wscale);
  clk->Continue((kMuLaneDepth + 4) * kPeriod);
  RT::JoinAll();

  ASSERT_TRUE(exe.HasResult());
  MatrixExe::Result r = exe.TakeResult();
  std::vector<float> want = Reference(cfg, token, weight, scale, wscale);
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
  cfg.a_dtype = numeric::DataType::kBf16;
  cfg.b_dtype = numeric::DataType::kBf16;
  CheckPrimitive(cfg, 0x1234);
}

TEST(Mu, PrimitiveMxfp8) {
  MuTaskCfg cfg;
  cfg.a_dtype = numeric::DataType::kMxfp8;
  cfg.b_dtype = numeric::DataType::kMxfp8;
  CheckPrimitive(cfg, 0x2345);
}

TEST(Mu, PrimitiveMxfp4) {
  MuTaskCfg cfg;
  cfg.a_dtype = numeric::DataType::kMxfp4;
  cfg.b_dtype = numeric::DataType::kMxfp4;
  CheckPrimitive(cfg, 0x3456);
}

TEST(Mu, PrimitiveOutBf16) {
  // DTYPE_C = 1：结果原位舍入截断成 BF16。
  MuTaskCfg cfg;
  cfg.a_dtype = numeric::DataType::kBf16;
  cfg.b_dtype = numeric::DataType::kBf16;
  cfg.out_bf16 = true;
  CheckPrimitive(cfg, 0x4567);
}

TEST(Mu, PrimitiveMxfp8TimesMxfp8) {
  // 权重也带 scale：块内部分和乘 token 与权重两个 scale 之积。
  MuTaskCfg cfg;
  cfg.a_dtype = numeric::DataType::kMxfp8;
  cfg.b_dtype = numeric::DataType::kMxfp8;
  EXPECT_EQ(cfg.PrimK(), 128u);
  EXPECT_EQ(cfg.PrimN(), 64u);
  CheckPrimitive(cfg, 0x5678, /*with_wscale=*/true);
}

TEST(Mu, PrimitiveVlane2IsK64N128) {
  // 物理阵列 K128×N64 开 vlane = 2：每个 lane 在第 64 个输入处断开，一次出两个
  // 半长的点积，原语就是 1×K64×N128。
  MuTaskCfg cfg;
  cfg.primitive_type = 1;
  cfg.a_dtype = numeric::DataType::kMxfp8;
  cfg.b_dtype = numeric::DataType::kMxfp8;
  EXPECT_EQ(cfg.PrimK(), 64u);
  EXPECT_EQ(cfg.PrimN(), 128u);
  CheckPrimitive(cfg, 0x6789, /*with_wscale=*/true);
}

TEST(Mu, AccumOrderIsPartOfResult) {
  // 换一个累加顺序，同一份输入算出来就不是同一个 bit。这条是「参考实现必须照抄
  // 硬件顺序」那句话的凭据。
  EnsureSlots();
  ClockPtr clk = MakeClock(0, kPeriod);
  MatrixExe exe(clk, "exe");

  MuTaskCfg cfg;
  cfg.a_dtype = numeric::DataType::kMxfp8;
  cfg.b_dtype = numeric::DataType::kMxfp8;
  uint64_t k = cfg.PrimK(), n = cfg.PrimN();
  std::vector<uint8_t> token = TamePattern(cfg.a_dtype, k, 0x9001);
  std::vector<uint8_t> weight = TamePattern(cfg.b_dtype, k * n, 0x9002);
  std::vector<uint8_t> scale = TameScale(cfg.a_dtype, k / 32, 0x9003);

  exe.Issue(cfg, token, weight, scale);
  clk->Continue((kMuLaneDepth + 4) * kPeriod);
  RT::JoinAll();
  MatrixExe::Result r = exe.TakeResult();

  // 逐个乘 scale 再顺序加：与按块分组累加算出来的位不一样。
  std::vector<float> a = numeric::Decode(cfg.a_dtype, token, k);
  std::vector<float> sc = numeric::DecodeScale(cfg.a_dtype, scale, k / 32);
  uint64_t differ = 0;
  for (uint64_t j = 0; j < n; ++j) {
    std::vector<uint8_t> col(weight.begin() + j * k, weight.begin() + (j + 1) * k);
    std::vector<float> b = numeric::Decode(cfg.b_dtype, col, k);
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
  cfg.a_dtype = numeric::DataType::kBf16;
  cfg.b_dtype = numeric::DataType::kBf16;
  cfg.kblock = 2;
  cfg.nblock = 2;
  cfg.a_addr = 0x1000;
  cfg.b_addr = 0x20000;
  cfg.c_addr = 0x8000;
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
  cfg.a_addr = 0x100000;
  MuAgu agu(cfg);
  MuStep s = agu.Next();
  EXPECT_FALSE(agu.CheckStep(s, 0x1000, 0x100000));
  // 没对齐也拒。
  MuTaskCfg odd;
  odd.a_addr = 0x1004;
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
  auto ids = std::make_shared<DsaIdsPort>(clk);
  reg.AttachCfg(port);
  reg.AttachIds(ids);

  // 扮演 RV core：逐拍写配置，最后写 TASK_TRIGGER。身份三项在 trigger 那一拍从
  // CSR 直连采样，不在寄存器里。
  class Writer : public BachModule {
   public:
    Writer(ClockPtr c, std::shared_ptr<DsaCfgPort> p,
           std::shared_ptr<DsaIdsPort> i)
        : BachModule(c, "writer"), port(std::move(p)), ids(std::move(i)) {}

   protected:
    void Step() override {
      ids->Drive(/*stream=*/5, /*task=*/9, /*user=*/77, /*path=*/0);
      static const std::pair<uint64_t, uint64_t> kSeq[] = {
          {kMuPrimitiveDim, (3u << kMuKblockShift) | 2u},  // KBLOCK=3、NBLOCK=2
          {kMuPrimitiveMode, (1u << kMuPrimTypeShift) |        // 1×K64×N128
                                 (1u << kMuADataTypeShift) |   // A=MXFP8
                                 (1u << kMuRouterEpDtypeShift) |  // B=MXFP8
                                 kMuTaskLast},
          {kMuAAddr, 0x1000u},
          {kMuCAddr, 0x8000u},
          {kMuTaskTrigger, kMuTriggerValid},
      };
      uint64_t now = CycleNow();
      if (now == 0 || now > 5) {
        port->Idle();
        return;
      }
      auto const& kv = kSeq[now - 1];
      port->Drive(kv.first, kv.second, now);
    }

   private:
    std::shared_ptr<DsaCfgPort> port;
    std::shared_ptr<DsaIdsPort> ids;
  };
  Writer w(clk, port, ids);

  clk->Continue((12) * kPeriod);
  RT::JoinAll();

  ASSERT_TRUE(reg.HasPending());
  MuTaskCfg cfg = reg.TakePending();
  EXPECT_EQ(cfg.primitive_type, 1u);
  EXPECT_EQ(cfg.PrimK(), 64u);
  EXPECT_EQ(cfg.PrimN(), 128u);
  EXPECT_EQ(cfg.a_dtype, numeric::DataType::kMxfp8);
  EXPECT_EQ(cfg.b_dtype, numeric::DataType::kMxfp8);
  EXPECT_TRUE(cfg.task_last);
  EXPECT_EQ(cfg.kblock, 3u);
  EXPECT_EQ(cfg.nblock, 2u);
  EXPECT_EQ(cfg.a_addr, 0x1000u);
  EXPECT_EQ(cfg.c_addr, 0x8000u);
  EXPECT_EQ(cfg.stream_id, 5u);
  EXPECT_EQ(cfg.task_id, 9u);
  EXPECT_EQ(cfg.user_id, 77u);
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
// scale 与数据地址一一映射，每 32 B 一个；正文接 scale 的读把 [addr, addr + bytes)
// 覆盖到的那几组接在正文后面，与 BankedMem 的 scale 旁带同一个口径。
class MuMem : public BachModule {
 public:
  MuMem(ClockPtr c, const std::string& name, MemPort& p, uint64_t bytes,
        uint64_t delay)
      : BachModule(c, name), port(p), mem(bytes, 0), latency(delay) {}

  std::vector<uint8_t> mem;
  std::map<uint64_t, uint8_t> scale;   // 按组号存
  uint64_t reads = 0, writes = 0;

  void Poke(uint64_t at, std::vector<uint8_t> const& v) {
    for (uint64_t i = 0; i < v.size() && at + i < mem.size(); ++i) mem[at + i] = v[i];
  }
  // 从 at 那一组起铺几组 scale。
  void PokeScale(uint64_t at, std::vector<uint8_t> const& v) {
    for (uint64_t k = 0; k < v.size(); ++k) {
      scale[at / kScaleGroupBytes + k] = v[k];
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
    if (!pipe.empty() && pipe.front().first <= now) {
      rsp = pipe.front().second;
      valid = true;
      pipe.pop_front();
    }
    port.DriveSlave(true, valid, rsp);

    // 不按 (addr, bytes) 去重：master 每拍要么发一笔要么 IdleReq，端口上不会
    // 回落成上一拍的值，所以每一拍的 req_valid 都是一笔真请求。同一个地址连着
    // 写两拍是两笔，同一个 tile_N 的几个 tile 就是这样叠写同一段的。
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
    if (port.req_scale_en.Get() == kScaleWithData) {
      uint64_t groups = (n + kScaleGroupBytes - 1) / kScaleGroupBytes;
      for (uint64_t k = 0; k < groups; ++k) {
        auto it = scale.find(addr / kScaleGroupBytes + k);
        data->push_back(it == scale.end() ? 0 : it->second);
      }
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

// 扮演 RV core 的身份 CSR：每拍驱动 stream / task / user，写 TASK_TRIGGER 那一拍
// 由 regfile 采样进任务快照。
class IdsHolder : public BachModule {
 public:
  IdsHolder(ClockPtr c, std::shared_ptr<DsaIdsPort> p, uint64_t stream,
            uint64_t task, uint64_t user)
      : BachModule(c, "ids"), ids(std::move(p)),
        stream_(stream), task_(task), user_(user) {}

 protected:
  void Step() override { ids->Drive(stream_, task_, user_, /*path=*/0); }

 private:
  std::shared_ptr<DsaIdsPort> ids;
  uint64_t stream_, task_, user_;
};

// topK 表在数据线里的样子：每项 {local_ep_index 2 B, weight 4 B}。
std::vector<uint8_t> TopkBytes(std::vector<TopkEntry> const& t) {
  std::vector<uint8_t> b(t.size() * kTopkEntryBytes, 0);
  for (size_t i = 0; i < t.size(); ++i) {
    uint64_t at = i * kTopkEntryBytes;
    b[at] = uint8_t(t[i].local_ep_index & 0xFFu);
    b[at + 1] = uint8_t((t[i].local_ep_index >> 8) & 0xFFu);
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

  auto ids = std::make_shared<DsaIdsPort>(clk);
  mu.AttachIds(ids);

  MuTaskCfg want;
  want.a_dtype = numeric::DataType::kBf16;
  want.b_dtype = numeric::DataType::kBf16;   // 无 block scale，先排除 scale
  want.kblock = 1;
  want.nblock = 1;
  want.a_addr = 0x1000;
  want.b_addr = 0x0;
  want.c_addr = 0x20000;
  want.stream_id = 2;
  want.task_id = 3;
  want.task_last = true;   // 单笔任务要置 task_last，才会报一次 dsa_done

  uint64_t k = want.PrimK(), n = want.PrimN();
  MuMem cmem(clk, "cmem", *cmem_rd, 0x40000, 13);
  MuMem mmem(clk, "mmem", *mmem_rd, 0x80000, 16);
  MuMem outmem(clk, "outmem", *cmem_wr, 0x40000, 13);

  std::vector<uint8_t> token = TamePattern(want.a_dtype, k, 0x901);
  std::vector<uint8_t> weight = TamePattern(want.b_dtype, k * n, 0x902);
  cmem.Poke(want.a_addr, token);
  mmem.Poke(want.b_addr, weight);

  CfgWriter writer(clk, cfg_port);
  writer.Push(kMuPrimitiveMode, kMuTaskLast);   // BF16×BF16、1×K128×N64
  writer.Push(kMuPrimitiveDim, 1u | (1u << 16));
  writer.Push(kMuAAddr, want.a_addr);
  writer.Push(kMuBAddr, want.b_addr);
  writer.Push(kMuCAddr, want.c_addr);
  writer.Push(kMuTaskTrigger, kMuTriggerValid);

  IdsHolder ids_holder(clk, ids, want.stream_id, want.task_id, /*user=*/0);
  MuDoneSink sink(clk, mu.Done());
  MuDriver driver(clk, mu);
  clk->Continue(2000 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink.got.size(), 1u);
  std::vector<float> ref = Reference(want, token, weight, {});
  std::vector<uint8_t> raw = outmem.Peek(want.c_addr, n * 4);
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

  auto ids = std::make_shared<DsaIdsPort>(clk);
  mu.AttachIds(ids);

  MuTaskCfg want;
  want.a_dtype = numeric::DataType::kBf16;
  want.b_dtype = numeric::DataType::kBf16;
  want.kblock = 1;
  want.nblock = 1;
  want.expert_count = 2;
  want.ep_reduce = true;
  want.token_expert_stride = 0x800;   // FC2 用 token 那侧
  want.b_expert_stride = 0x10000;
  want.a_addr = 0x1000;
  want.b_addr = 0x0;
  want.c_addr = 0x20000;
  want.stream_id = 5;
  want.task_id = 7;
  want.task_last = true;

  uint64_t k = want.PrimK(), n = want.PrimN();
  MuMem cmem(clk, "cmem", *cmem_rd, 0x40000, 13);
  MuMem mmem(clk, "mmem", *mmem_rd, 0x80000, 16);
  MuMem outmem(clk, "outmem", *cmem_wr, 0x40000, 13);

  // 每个专家一份激活、一份权重。合并那一档的激活按 topK 的先后排，权重按专家
  // 在本组内的序号排。
  std::vector<std::vector<uint8_t>> tok, wgt;
  for (uint64_t e = 0; e < want.expert_count; ++e) {
    tok.push_back(TamePattern(want.a_dtype, k, 0xA10 + e));
    wgt.push_back(TamePattern(want.b_dtype, k * n, 0xA20 + e));
    cmem.Poke(want.a_addr + e * want.token_expert_stride, tok.back());
  }
  // topK 里第 0 个是组内第 1 个专家，第 1 个是组内第 0 个；topK 直接存组内序号。
  // 表由 DTE 搬运时经专用数据线按 stream_id 直接写进 MU 的 topK_ep_table。
  const float w0 = 0.75f, w1 = 0.25f;
  mu.EpInfo().WriteTopk(want.stream_id, TopkBytes({{1, w0}, {0, w1}}));
  mmem.Poke(want.b_addr + 1 * want.b_expert_stride, wgt[0]);
  mmem.Poke(want.b_addr + 0 * want.b_expert_stride, wgt[1]);

  CfgWriter writer(clk, cfg_port);
  writer.Push(kMuPrimitiveMode, (want.expert_count << kMuRouterExpertCountShift) |
                                    kMuRouterEpReduceEn | kMuTaskLast);
  writer.Push(kMuPrimitiveDim, 1u | (1u << 16));
  writer.Push(kMuAAddr, want.a_addr);
  writer.Push(kMuBAddr, want.b_addr);
  writer.Push(kMuCAddr, want.c_addr);
  writer.Push(kMuAcExpertStride,
              want.token_expert_stride << kMuTokenExpertStrideShift);
  writer.Push(kMuBExpertStride, want.b_expert_stride);
  writer.Push(kMuTaskTrigger, kMuTriggerValid);

  IdsHolder ids_holder(clk, ids, want.stream_id, want.task_id, /*user=*/0);
  MuDoneSink sink(clk, mu.Done());
  MuDriver driver(clk, mu);
  clk->Continue(2000 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink.got.size(), 1u) << "两个专家合成一笔，只报一次 dsa_done";

  // 先各自算完一列，各乘自己的权重，再顺序相加。
  std::vector<float> p0 = Reference(want, tok[0], wgt[0], {});
  std::vector<float> p1 = Reference(want, tok[1], wgt[1], {});
  std::vector<uint8_t> raw = outmem.Peek(want.c_addr, n * 4);
  for (uint64_t j = 0; j < n; ++j) {
    float a = numeric::ClampNanInf(p0[j] * w0);
    float b = numeric::ClampNanInf(p1[j] * w1);
    float ref = numeric::ClampNanInf(a + b);
    uint32_t got = 0;
    for (int t = 0; t < 4; ++t) got |= uint32_t(raw[j * 4 + t]) << (8 * t);
    EXPECT_EQ(got, numeric::BitsOf(ref)) << "j=" << j;
  }
  // 合并成一份，所以只写了一列。
  std::vector<uint8_t> beyond = outmem.Peek(want.c_addr + n * 4, 4);
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

  auto ids = std::make_shared<DsaIdsPort>(clk);
  mu.AttachIds(ids);

  MuTaskCfg want;
  want.a_dtype = numeric::DataType::kBf16;
  want.b_dtype = numeric::DataType::kBf16;
  want.kblock = 1;
  want.nblock = 1;
  want.expert_count = 2;
  want.ep_reduce = false;
  want.output_expert_stride = 0x800;   // FC1/FC3 用 output 那侧
  want.b_expert_stride = 0x10000;
  want.a_addr = 0x1000;
  want.b_addr = 0x0;
  want.c_addr = 0x20000;
  want.stream_id = 6;
  want.task_id = 8;
  want.task_last = true;

  uint64_t k = want.PrimK(), n = want.PrimN();
  MuMem cmem(clk, "cmem", *cmem_rd, 0x40000, 13);
  MuMem mmem(clk, "mmem", *mmem_rd, 0x80000, 16);
  MuMem outmem(clk, "outmem", *cmem_wr, 0x40000, 13);

  // 各出一份那一档，几个专家共用同一份激活。
  std::vector<uint8_t> token = TamePattern(want.a_dtype, k, 0xB01);
  cmem.Poke(want.a_addr, token);
  std::vector<std::vector<uint8_t>> wgt;
  for (uint64_t e = 0; e < want.expert_count; ++e) {
    wgt.push_back(TamePattern(want.b_dtype, k * n, 0xB10 + e));
  }
  // topK 直接存组内序号：第 0 个是组内第 1 个，第 1 个是组内第 0 个。表由 DTE
  // 搬运时经专用数据线按 stream_id 直接写进 MU 的 topK_ep_table。
  mu.EpInfo().WriteTopk(want.stream_id, TopkBytes({{1, 1.0f}, {0, 1.0f}}));
  mmem.Poke(want.b_addr + 1 * want.b_expert_stride, wgt[0]);
  mmem.Poke(want.b_addr + 0 * want.b_expert_stride, wgt[1]);

  CfgWriter writer(clk, cfg_port);
  writer.Push(kMuPrimitiveMode,
              (want.expert_count << kMuRouterExpertCountShift) | kMuTaskLast);
  writer.Push(kMuPrimitiveDim, 1u | (1u << 16));
  writer.Push(kMuAAddr, want.a_addr);
  writer.Push(kMuBAddr, want.b_addr);
  writer.Push(kMuCAddr, want.c_addr);
  writer.Push(kMuAcExpertStride,
              want.output_expert_stride << kMuOutputExpertStrideShift);
  writer.Push(kMuBExpertStride, want.b_expert_stride);
  writer.Push(kMuTaskTrigger, kMuTriggerValid);

  IdsHolder ids_holder(clk, ids, want.stream_id, want.task_id, /*user=*/0);
  MuDoneSink sink(clk, mu.Done());
  MuDriver driver(clk, mu);
  clk->Continue(2000 * kPeriod);
  RT::JoinAll();

  ASSERT_EQ(sink.got.size(), 1u);
  for (uint64_t e = 0; e < want.expert_count; ++e) {
    std::vector<float> ref = Reference(want, token, wgt[e], {});
    std::vector<uint8_t> raw =
        outmem.Peek(want.c_addr + e * want.output_expert_stride, n * 4);
    for (uint64_t j = 0; j < n; ++j) {
      uint32_t got = 0;
      for (int t = 0; t < 4; ++t) got |= uint32_t(raw[j * 4 + t]) << (8 * t);
      EXPECT_EQ(got, numeric::BitsOf(ref[j])) << "e=" << e << " j=" << j;
    }
  }
}

// 一笔任务是 kblock × nblock 次原语。整条装配跑完，每个 tile 的结果都要与参考
// 实现逐 bit 相同，dsa_done 只报一次。token 与权重都是 MXFP8、都带 scale，scale 由
// 存储按数据地址一一对应地接在正文后面。
void RunAllTiles(uint64_t prim_type) {
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

  auto ids = std::make_shared<DsaIdsPort>(clk);
  mu.AttachIds(ids);

  MuTaskCfg want;
  want.a_dtype = numeric::DataType::kMxfp8;
  want.b_dtype = numeric::DataType::kMxfp8;
  want.primitive_type = prim_type;
  want.kblock = 2;
  want.nblock = 2;
  want.a_addr = 0x1000;
  want.b_addr = 0x0;
  want.c_addr = 0x20000;
  want.stream_id = 4;
  want.task_id = 11;
  want.task_last = true;

  uint64_t k = want.PrimK(), n = want.PrimN();
  uint64_t tok_bytes = k;                 // MXFP8 一个元素一字节
  uint64_t wgt_bytes = k * n;
  uint64_t sc_bytes = k / 32;

  MuMem cmem(clk, "cmem", *cmem_rd, 0x40000, 13);
  MuMem mmem(clk, "mmem", *mmem_rd, 0x40000, 16);
  MuMem outmem(clk, "outmem", *cmem_wr, 0x40000, 13);

  // 每个 tile_K 一段 token 与它那几个 scale，每个 (n, k) 一块权重与它那几个
  // scale。primitive_type = 1 时一段 token 只有 64 B，第二段从一行的中间开始。
  for (uint64_t ki = 0; ki < want.kblock; ++ki) {
    cmem.Poke(want.a_addr + ki * tok_bytes,
              TamePattern(want.a_dtype, k, 0x600 + ki));
    cmem.PokeScale(want.a_addr + ki * tok_bytes,
                   TameScale(want.a_dtype, sc_bytes, 0x700 + ki));
  }
  for (uint64_t i = 0; i < want.kblock * want.nblock; ++i) {
    mmem.Poke(want.b_addr + i * wgt_bytes,
              TamePattern(want.b_dtype, k * n, 0x800 + i));
    mmem.PokeScale(want.b_addr + i * wgt_bytes,
                   TameScale(want.b_dtype, n * sc_bytes, 0x900 + i));
  }

  CfgWriter writer(clk, cfg_port);
  writer.Push(kMuPrimitiveMode, (prim_type << kMuPrimTypeShift) |
                                    (1u << kMuADataTypeShift) |
                                    (1u << kMuRouterEpDtypeShift) | kMuTaskLast);
  writer.Push(kMuPrimitiveDim, want.kblock | (want.nblock << 16));
  writer.Push(kMuAAddr, want.a_addr);
  writer.Push(kMuBAddr, want.b_addr);
  writer.Push(kMuCAddr, want.c_addr);
  writer.Push(kMuTaskTrigger, kMuTriggerValid);

  IdsHolder ids_holder(clk, ids, want.stream_id, want.task_id, /*user=*/0);
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
      uint64_t i = ni * want.kblock + ki;
      std::vector<uint8_t> token = TamePattern(want.a_dtype, k, 0x600 + ki);
      std::vector<uint8_t> scale =
          TameScale(want.a_dtype, sc_bytes, 0x700 + ki);
      std::vector<uint8_t> weight =
          TamePattern(want.b_dtype, k * n, 0x800 + i);
      std::vector<uint8_t> wscale =
          TameScale(want.b_dtype, n * sc_bytes, 0x900 + i);
      std::vector<float> part = Reference(want, token, weight, scale, wscale);
      if (ki == 0) {
        ref = part;
        continue;
      }
      for (uint64_t j = 0; j < n; ++j) {
        ref[j] = numeric::ClampNanInf(ref[j] + part[j]);
      }
    }

    std::vector<uint8_t> raw = outmem.Peek(want.c_addr + ni * n * 4, n * 4);
    for (uint64_t j = 0; j < n; ++j) {
      uint32_t b = 0;
      for (int t = 0; t < 4; ++t) b |= uint32_t(raw[j * 4 + t]) << (8 * t);
      EXPECT_EQ(b, numeric::BitsOf(ref[j])) << "tile_N=" << ni << " j=" << j;
    }
  }
}

TEST(Mu, AssemblyRunsAllTiles) { RunAllTiles(0); }

TEST(Mu, AssemblyRunsAllTilesK64N128) { RunAllTiles(1); }

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

// ffn.txt 里的一条 gemm 或 gemm_ws。后者多一栏权重的 scale。
struct GemmCase {
  numeric::DataType dtype = numeric::DataType::kBf16;
  uint64_t k = 0, n = 0;
  bool out_bf16 = false;
  std::vector<uint8_t> token, weight, scale, wscale;
  std::vector<float> want;
};

// 只取 MU 那两条原语形状的：K=128×N=64 与 vlane = 2 的 K=64×N=128。别的形状
// matrix exe 发不出去。
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
    bool ws = col.size() == 10 && col[0] == "gemm_ws";
    if (!ws && (col.size() != 9 || col[0] != "gemm")) continue;
    GemmCase c;
    c.k = std::stoull(col[2]);
    c.n = std::stoull(col[3]);
    if (!((c.k == 64 && c.n == 128) || (c.k == 128 && c.n == 64))) continue;
    c.dtype = TypeByName(col[1]);
    c.out_bf16 = col[4] == "1";
    c.token = HexBytes(col[5]);
    c.weight = HexBytes(col[6]);
    c.scale = HexBytes(col[7]);
    if (ws) c.wscale = HexBytes(col[8]);
    c.want = HexFloats(col[ws ? 9 : 8]);
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
    cfg.a_dtype = c.dtype;
    cfg.b_dtype = c.dtype;
    cfg.out_bf16 = c.out_bf16;
    cfg.primitive_type = c.k == 64 ? 1 : 0;
    ASSERT_EQ(cfg.PrimK(), c.k);
    ASSERT_EQ(cfg.PrimN(), c.n);

    EnsureSlots();
    ClockPtr clk = MakeClock(0, kPeriod);
    MatrixExe exe(clk, "exe");
    exe.Issue(cfg, c.token, c.weight, c.scale, c.wscale);
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
