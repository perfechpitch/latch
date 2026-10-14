// 装 pcode 导出的 EP1 bundle（BACHIR 11），从 chip 0 的 W 口注入 16 个用户的 token。
//
//   PYTHONPATH=pcode/src:defs:route/src
//   python3 -m bachflow_pcode.latch_export pcode/output/ep1_pp3_tp16
//       --out /tmp/latch_bundle --name pcode_ep1
//
// 根目录 PCODE_BUNDLE_ROOT（默认 /tmp/latch_bundle），名字 PCODE_BUNDLE_NAME
// （默认 pcode_ep1）。没有 bundle 就跳过。
//
// topK 56 B：2 B ep_counter、8×2 B 组内序号、8×4 B FP32 权重，其余填 0。
// 组内序号加上 1 个共享槽之后必须落在前 20 个物理专家里，这样 Matrix Mem
// 只铺这 20 个里被点到的专家，不必把 256 个路由专家都激活。
// 数据段 6336 B 是有限 E4M3 图案。权重按 0x18000 稀疏铺进 Matrix Mem。
// B core 自启动。pcie 搬入把每个用户写进 Matrix Mem 的一份缓冲，check flag
// 轮询 ShareMem 的 head/tail，看到谁 ready 再带上他的 user_id 往下发。
// 缓冲和计算核的 stream 表都是 16 项，所以 16 名隔 1 拍注入，不必等前一名走完。
// 停钟后用与 MatrixExe 相同的累加，按 stream 比较 16 名用户的 FC1 部分和，以及
// chip 47 核 8 上 concat 的 6144 个 BF16。自启动的 B 核不参与停钟判断。

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "bach/common/message.h"
#include "bach/common/numeric/accum.h"
#include "bach/common/numeric/formats.h"
#include "bach/common/numeric/mx.h"
#include "test/bach/ip/chip/moe_common.h"

using namespace latch;
using namespace latch::bach;
using namespace latch::bach::moetest;

namespace {

constexpr uint64_t kPciePath = 63;
constexpr uint64_t kUserCount = 16;
constexpr uint64_t kUserBase = 1;           // 用户号 1..16
constexpr uint64_t kStreamStride = 0x10000;
constexpr uint64_t kTopkBytes = 56;
constexpr uint64_t kDataBytes = 6336;
constexpr uint64_t kRouted = 8;
constexpr uint64_t kIdleCycles = 3000;  // PCODE_IDLE_CYCLES 可覆盖
constexpr uint64_t kUserGap = 1;          // 下一名隔 1 拍注入
constexpr uint64_t kConcatN = 6144;
constexpr uint64_t kExpertStride = 0x18000;
constexpr uint64_t kSharedSlots = 1;        // 共享专家占物理序号 0
constexpr uint64_t kExpertPool = 20;        // 只允许前 20 个物理专家
constexpr uint64_t kFc1Core = 1;            // chip 0 上跑 FC1 的物理核
constexpr uint64_t kFc1Out = 0x18c0;        // 0x484018c0 剥掉 Core Mem 基址
constexpr uint64_t kConcatAt = 0xB00;
constexpr uint64_t kFc1N = 128;
constexpr uint64_t kTokenK = 6144;
constexpr double kMinCosine = 0.99;

std::string EnvOr(char const* key, std::string const& dflt) {
  char const* v = std::getenv(key);
  return v && *v ? std::string(v) : dflt;
}

void StoreU16(std::vector<uint8_t>& b, uint64_t at, uint16_t v) {
  b[at] = uint8_t(v & 0xFF);
  b[at + 1] = uint8_t((v >> 8) & 0xFF);
}

void StoreF32(std::vector<uint8_t>& b, uint64_t at, float v) {
  uint32_t bits = numeric::BitsOf(v);
  for (int k = 0; k < 4; ++k) b[at + k] = uint8_t((bits >> (8 * k)) & 0xFF);
}

float LoadF32(std::vector<uint8_t> const& b, uint64_t at) {
  uint32_t bits = 0;
  for (int k = 0; k < 4; ++k) bits |= uint32_t(b[at + k]) << (8 * k);
  return numeric::FloatOf(bits);
}

struct TopkSlot {
  uint64_t local = 0;
  float weight = 0;
};

TopkSlot TopkAt(MessagePtr const& msg, uint64_t e) {
  uint64_t local = uint64_t(msg->payload[2 + e * 2]) |
                   (uint64_t(msg->payload[3 + e * 2]) << 8);
  return {local, LoadF32(msg->payload, 2 + kRouted * 2 + e * 4)};
}

// 物理专家号 = 共享槽数 + 组内序号。源只从还能落进前 20 个的组内序号里取。
uint64_t PhysicalExpert(uint64_t local) {
  return kSharedSlots + local;
}

MessagePtr MakeEp1Token(uint64_t user, uint64_t local_base) {
  auto m = std::make_shared<Message>();
  m->path_id = kPciePath;
  m->user_id = user;
  m->path_core_mask = 0x3F;
  m->gpu_id = 2;
  m->token_id = user;
  m->payload_aux = DtePayloadAuxBytes();
  m->payload.assign(kTopkBytes + kDataBytes, 0);
  StoreU16(m->payload, 0, kRouted);
  for (uint64_t e = 0; e < kRouted; ++e) {
    uint64_t local = local_base + e;
    // 越出前 20 个就折回去，不让路由专家号铺到 256。
    if (PhysicalExpert(local) >= kExpertPool) {
      local = local % (kExpertPool - kSharedSlots);
    }
    StoreU16(m->payload, 2 + e * 2, uint16_t(local));
    StoreF32(m->payload, 2 + 16 + e * 4, 0.125f * float(e + 1));
  }
  // 有限 E4M3（0x30..0x37）。0x7F / 0xFF 是 NaN，进累加器后会被夹成最大值，
  // 再和反号的 NaN 抵消成 0，参考和仿真都对不上。
  for (uint64_t i = kTopkBytes; i < m->payload.size(); ++i) {
    m->payload[i] = uint8_t(0x30 + ((i - kTopkBytes) & 7u));
  }
  m->size = m->payload.size();
  return m;
}

uint64_t InstsOf(Core& c) {
  uint64_t n = 0;
  for (uint64_t i = 0; i < 3; ++i) n += c.Rv(i).Exec().Insts();
  return n;
}

uint64_t ProgressOf(Core& c) {
  return c.GetDte().Regfile().Triggers() + c.GetTs().Table().Writes();
}

// 专家 e 的第 i 个 MXFP8 字节。落在 0x30..0x37，是有限的正 E4M3。
uint8_t ExpertByte(uint64_t e, uint64_t i) {
  return uint8_t(0x30 + ((e + i) & 7u));
}

// 一个 K64×N128 的 tile 累进一列。顺序与 MatrixExe 相同：列优先取权重，
// 块内乘积加完再乘两个 scale。
void AccTile(std::vector<float>& col, bool k_first, ByteBlock const& token,
             ByteBlock const& tscale, ByteBlock const& weight,
             ByteBlock const& wscale) {
  constexpr uint64_t pk = 64, pn = 128, block = 32;
  auto a = numeric::Decode(numeric::DataType::kMxfp8, token, pk);
  auto sc = numeric::DecodeScale(numeric::DataType::kMxfp8, tscale, pk / block);
  if (k_first) col.assign(pn, 0.0f);
  for (uint64_t j = 0; j < pn; ++j) {
    ByteBlock colb(weight.begin() + j * pk, weight.begin() + (j + 1) * pk);
    auto b = numeric::Decode(numeric::DataType::kMxfp8, colb, pk);
    std::vector<float> prod(pk, 0.0f);
    for (uint64_t i = 0; i < pk; ++i) prod[i] = a[i] * b[i];
    ByteBlock ws;
    uint64_t nb = pk / block;
    for (uint64_t t = 0; t < nb; ++t) ws.push_back(wscale[j * nb + t]);
    float acc = numeric::AccumByScaleBlock2(
        prod, sc, numeric::DecodeScale(numeric::DataType::kMxfp8, ws, nb), block);
    float clamped = numeric::ClampNanInf(acc);
    col[j] = k_first ? clamped : numeric::ClampNanInf(col[j] + clamped);
  }
}

// 一个专家的 GEMV。token_at(k_off, n) 给出这一段 token 的字节；权重按
// (n_idx * kblock + k_idx) 排 tile，与 AGU 一致。
template <typename Tok>
std::vector<float> GemvExpert(uint64_t expert, uint64_t kblock, uint64_t nblock,
                              bool round_bf16, uint8_t scale, Tok token_at) {
  constexpr uint64_t pk = 64, pn = 128;
  std::vector<float> out(nblock * pn, 0.0f);
  ByteBlock ones(pk / 32, scale);
  for (uint64_t n_idx = 0; n_idx < nblock; ++n_idx) {
    std::vector<float> col(pn, 0.0f);
    for (uint64_t k_idx = 0; k_idx < kblock; ++k_idx) {
      ByteBlock tok = token_at(k_idx * pk, pk);
      uint64_t woff = (n_idx * kblock + k_idx) * pk * pn;
      ByteBlock w(pk * pn), ws((pk * pn) / 32, scale);
      for (uint64_t i = 0; i < w.size(); ++i) w[i] = ExpertByte(expert, woff + i);
      AccTile(col, k_idx == 0, tok, ones, w, ws);
    }
    for (uint64_t j = 0; j < pn; ++j) {
      float v = col[j];
      if (round_bf16) v = numeric::FromBf16(numeric::ToBf16(v));
      out[n_idx * pn + j] = v;
    }
  }
  return out;
}

// FC1 共享专家那一列：K=768、N=128，K64×N128，12 个 K tile。芯片 0 核 1 把
// 结果写在 0x18c0，和 MatrixExe 同一条累加顺序。
std::vector<float> GoldenFc1(MessagePtr const& msg) {
  uint8_t sc = numeric::ToE8m0(1.0f);
  auto tok = [&](uint64_t off, uint64_t n) {
    ByteBlock b(n, 0);
    for (uint64_t i = 0; i < n; ++i) b[i] = msg->payload[kTopkBytes + off + i];
    return b;
  };
  return GemvExpert(0, /*kblock=*/12, /*nblock=*/1, /*round_bf16=*/false, sc,
                    tok);
}

// 权重 scale 固定为 1，token 的 scale 按元素偏移另给。
template <typename Tok, typename Sc>
std::vector<float> GemvExpertScales(uint64_t expert, uint64_t kblock, uint64_t nblock,
                                    Tok token_at, Sc scale_at) {
  constexpr uint64_t pk = 64, pn = 128;
  std::vector<float> out(nblock * pn, 0.0f);
  uint8_t one = numeric::ToE8m0(1.0f);
  for (uint64_t n_idx = 0; n_idx < nblock; ++n_idx) {
    std::vector<float> col(pn, 0.0f);
    for (uint64_t k_idx = 0; k_idx < kblock; ++k_idx) {
      ByteBlock tok = token_at(k_idx * pk, pk);
      ByteBlock tsc = scale_at(k_idx * pk, pk / 32);
      uint64_t woff = (n_idx * kblock + k_idx) * pk * pn;
      ByteBlock w(pk * pn), ws((pk * pn) / 32, one);
      for (uint64_t i = 0; i < w.size(); ++i) w[i] = ExpertByte(expert, woff + i);
      AccTile(col, k_idx == 0, tok, tsc, w, ws);
    }
    for (uint64_t j = 0; j < pn; ++j) out[n_idx * pn + j] = col[j];
  }
  return out;
}

// concat 目的地是 6144 个 BF16：8 个逻辑核各 768，再经 16 个 chip 的 bf16 求和。
// 各核灌进的权重相同，token 周期为 8，K 片又是 8 的倍数，所以每一片一样。
// 共享专家走量化后的 silu；组内序号 0..3 按 stride 0x500 读到的仍是 token。
std::vector<float> GoldenConcat(MessagePtr const& msg) {
  uint8_t one = numeric::ToE8m0(1.0f);
  auto tok0 = [&](uint64_t off, uint64_t n) {
    ByteBlock b(n, 0);
    for (uint64_t i = 0; i < n; ++i) {
      uint64_t at = kTopkBytes + off + i;
      b[i] = at < msg->payload.size() ? msg->payload[at] : 0;
    }
    return b;
  };
  auto part = GemvExpert(0, /*kblock=*/12, /*nblock=*/1, /*round_bf16=*/false, one, tok0);
  std::vector<float> col(part.size(), 0.0f);
  for (uint64_t j = 0; j < part.size(); ++j) {
    col[j] = numeric::FromBf16(numeric::ToBf16(part[j] * 8.0f));
  }
  std::vector<float> act(col.size(), 0.0f);
  for (uint64_t j = 0; j < col.size(); ++j) {
    float sig = 1.0f / (1.0f + std::exp(-col[j]));
    float silu = numeric::ClampNanInf(col[j] * sig);
    act[j] = numeric::ClampNanInf(silu * col[j]);
  }
  auto block_scale = numeric::MakeScale(numeric::DataType::kMxfp8, act, true);
  auto body = numeric::Encode(numeric::DataType::kMxfp8, act, block_scale,
                              numeric::RoundMode::kRne);
  auto scb = numeric::EncodeScale(numeric::DataType::kMxfp8, block_scale);
  auto act_at = [&](uint64_t off, uint64_t n) {
    ByteBlock b(n, 0);
    for (uint64_t i = 0; i < n; ++i) {
      if (off + i < body.size()) b[i] = body[off + i];
    }
    return b;
  };
  auto sc_at = [&](uint64_t off, uint64_t n) {
    ByteBlock b(n, one);
    for (uint64_t i = 0; i < n; ++i) {
      uint64_t bi = off / 32 + i;
      if (bi < scb.size()) b[i] = scb[bi];
    }
    return b;
  };
  auto token_at = [&](uint64_t off, uint64_t n) {
    ByteBlock b(n, 0);
    for (uint64_t i = 0; i < n; ++i) b[i] = uint8_t(0x30 + ((off + i) & 7u));
    return b;
  };
  auto ones = [&](uint64_t, uint64_t n) { return ByteBlock(n, one); };

  std::vector<float> acc;
  auto accumulate = [&](std::vector<float> const& g, float w, bool ep_first) {
    if (ep_first) acc.assign(g.size(), 0.0f);
    for (uint64_t j = 0; j < g.size(); ++j) {
      float t = numeric::ClampNanInf(g[j] * w);
      acc[j] = ep_first ? t : numeric::ClampNanInf(acc[j] + t);
    }
  };
  accumulate(GemvExpertScales(0, /*kblock=*/2, /*nblock=*/6, act_at, sc_at), 1.0f, true);
  // 共享专家走量化后的 silu。组内序号的前 4 个按 stride 读到的仍是 token，
  // 权重专家号是共享槽加上这条 topK 里的组内序号。
  for (uint64_t cnt = 0; cnt < 4; ++cnt) {
    TopkSlot s = TopkAt(msg, cnt);
    accumulate(GemvExpertScales(PhysicalExpert(s.local), 2, 6, token_at, ones),
               s.weight, false);
  }
  for (uint64_t j = 0; j < acc.size(); ++j) {
    acc[j] = numeric::FromBf16(numeric::ToBf16(acc[j]));
  }
  std::vector<float> slice(acc.size(), 0.0f);
  for (uint64_t j = 0; j < acc.size(); ++j) {
    float s = 0.0f;
    for (int c = 0; c < 16; ++c) s = numeric::ClampNanInf(s + acc[j]);
    slice[j] = numeric::FromBf16(numeric::ToBf16(s));
  }
  std::vector<float> full;
  full.reserve(slice.size() * 8);
  for (int core_i = 0; core_i < 8; ++core_i) {
    full.insert(full.end(), slice.begin(), slice.end());
  }
  return full;
}

double Cosine(std::vector<float> const& got, std::vector<float> const& want) {
  double dot = 0, na = 0, nb = 0;
  for (uint64_t i = 0; i < got.size() && i < want.size(); ++i) {
    dot += double(got[i]) * double(want[i]);
    na += double(got[i]) * double(got[i]);
    nb += double(want[i]) * double(want[i]);
  }
  if (na == 0.0 || nb == 0.0) return 0.0;
  return dot / std::sqrt(na * nb);
}

void PokeExperts(std::vector<Chip*> const& all,
                 std::vector<MessagePtr> const& tokens) {
  uint8_t sc = numeric::ToE8m0(1.0f);
  ByteBlock data(kExpertStride, 0);
  ByteBlock scale(kExpertStride / 32, sc);
  ByteBlock tok_scale(kTokenK / 32, sc);
  // 共享专家，加上各用户 topK 点到的路由专家。都在前 20 个物理序号里。
  std::vector<uint64_t> used(kExpertPool, 0);
  used[0] = 1;
  for (MessagePtr const& m : tokens) {
    for (uint64_t e = 0; e < kRouted; ++e) {
      uint64_t phys = PhysicalExpert(TopkAt(m, e).local);
      if (phys < kExpertPool) used[phys] = 1;
    }
  }
  for (Chip* chip : all) {
    for (uint64_t k = 0; k < kChipCoreNum; ++k) {
      Core& core = chip->GetCore(k);
      if (core.Bad()) continue;
      for (uint64_t e = 0; e < kExpertPool; ++e) {
        if (!used[e]) continue;
        for (uint64_t i = 0; i < data.size(); ++i) data[i] = ExpertByte(e, i);
        core.Mmem().Poke(e * kExpertStride, data);
        core.Mmem().PokeScale(e * kExpertStride, scale);
      }
      // token 的 scale 在旁带。DTE 只写正文，不盖掉这里。
      core.Cmem().PokeScale(0, tok_scale);
    }
  }
}

class PcodeHarness : public BachModule {
 public:
  PcodeHarness(ClockPtr c, std::vector<Chip*> target, std::vector<ChipPair> pairs,
               C2cBridge& feed, C2cBridge& sink, uint64_t cap,
               std::vector<MessagePtr> m)
      : BachModule(c, "harness"), chips(std::move(target)), links(std::move(pairs)),
        in_stub(feed), out_stub(sink), limit(cap), msgs(std::move(m)) {}

  uint64_t stopped_at = 0;
  // 16 名用户隔 1 拍注入。MM 里各占一份，check flag 看到 ready 再往下发。
  // 计算核按 stream 把结果隔开 kStreamStride。全体空闲后一次把 16 份留下。
  struct WaveSnap {
    ByteBlock fc1;
    ByteBlock concat;
  };
  std::vector<WaveSnap> snaps;

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    LinkEndPtr feed_in = in_stub.FromCore();
    // 第 1 名在第 2 拍开始注入，一拍一个 flit（每个最多 kFlitBytes，Header
    // Parser 按 flit 收）；发完一名再隔 kUserGap 拍发下一名。
    if (!cur && sent < msgs.size() && now >= next_at) {
      cur = msgs[sent];
      beat = 0;
      std::cerr << "  用户 " << cur->user_id << " 注入，第 " << now << " 拍\n";
    }
    if (cur) {
      uint64_t total = cur->TotalBytes();
      uint64_t flits = (total + kFlitBytes - 1) / kFlitBytes;
      if (flits == 0) flits = 1;
      uint64_t left = total - beat * kFlitBytes;
      uint64_t n = left > kFlitBytes ? kFlitBytes : left;
      bool last = beat + 1 == flits;
      feed_in->flit.Drive(/*vc=*/0, /*is_head=*/beat == 0, /*is_tail=*/last, n,
                          cur);
      ++beat;
      if (last) {
        cur.reset();
        ++sent;
        next_at = now + kUserGap;
      }
    } else {
      feed_in->flit.Idle();
    }
    feed_in->release.Idle();
    out_stub.FromCore()->flit.Idle();
    out_stub.FromCore()->release.Idle();

    out_stub.RunStep();
    for (uint64_t i = chips.size(); i > 0; --i) chips[i - 1]->RunOutside();
    in_stub.RunStep();

    Move(in_stub, chips[0]->Port(kChipW));
    for (ChipPair const& p : links) {
      Move(chips[p.a]->Port(p.a_port), chips[p.b]->Port(p.b_port));
    }
    Move(chips[chips.size() - 1]->Port(kChipE), out_stub);

    if (now % 64 == 0) {
      uint64_t sig = 0;
      bool busy = false;
      // 默认不把自启动核算进空闲：B 核的 check-flag 转完还会空转。
      // PCODE_WATCH_B=1 时把它算上，用来看只有 B 核在干活的前缀。
      bool watch_b = std::getenv("PCODE_WATCH_B") != nullptr;
      for (Chip* c : chips) {
        for (uint64_t k = 0; k < kChipCoreNum; ++k) {
          Core& core = c->GetCore(k);
          if (core.Bad()) continue;
          if (!watch_b && core.GetTs().Cfg().SelfStartCore()) continue;
          // 自启动的 B 核会一直往网上发包，别的核的 router 因此也不空。
          // 只看任务表：计算核的 stream 还占着，或 DTE/TS 计数还在变，就没停。
          if (core.GetTs().Table().InFlight() != 0) busy = true;
          sig += ProgressOf(core) * 3 + core.GetTs().Table().InFlight();
        }
      }
      if (busy || sig != last_sig) {
        last_sig = sig;
        last_change = now;
      }
      if (now % 8192 == 0) {
        uint64_t insts = 0, inflight = 0;
        for (Chip* c : chips) {
          for (uint64_t k = 0; k < kChipCoreNum; ++k) {
            Core& core = c->GetCore(k);
            if (core.Bad()) continue;
            insts += InstsOf(core);
            inflight += core.GetTs().Table().InFlight();
          }
        }
        ByteBlock ht = chips[0]->GetCore(5).Smem().Peek(1024 * 4, 8);
        uint32_t head = uint32_t(ht[0]) | (uint32_t(ht[1]) << 8) |
                        (uint32_t(ht[2]) << 16) | (uint32_t(ht[3]) << 24);
        uint32_t tail = uint32_t(ht[4]) | (uint32_t(ht[5]) << 8) |
                        (uint32_t(ht[6]) << 16) | (uint32_t(ht[7]) << 24);
        std::cerr << "  [cycle " << now << "] insts " << insts << " inflight "
                  << inflight << " head " << head << " tail " << tail << "\n";
      }
    }
    uint64_t idle_cycles = kIdleCycles;
    if (char const* e = std::getenv("PCODE_IDLE_CYCLES")) idle_cycles = std::strtoull(e, nullptr, 10);
    bool idle = now > 2 + idle_cycles && now - last_change > idle_cycles;
    if (idle && sent == msgs.size() && snaps.empty()) {
      for (uint64_t u = 0; u < msgs.size(); ++u) {
        WaveSnap w;
        w.fc1 = chips[0]->GetCore(kFc1Core).Cmem().Peek(
            kFc1Out + u * kStreamStride, kFc1N * 4);
        w.concat = chips[47]->GetCore(8).Cmem().Peek(
            kConcatAt + u * kStreamStride, kConcatN * 2);
        snaps.push_back(std::move(w));
      }
      std::cerr << "  " << snaps.size() << " 个用户的结果已留下，第 " << now
                << " 拍\n";
    }
    if ((snaps.size() == msgs.size() && idle) || now >= limit) {
      stopped_at = now;
      clk->Stop();
    }
  }

 private:
  static void Move(C2cBridge& x, C2cBridge& y) {
    while (x.HasOut()) y.PushIn(x.TakeOut());
    while (y.HasOut()) x.PushIn(y.TakeOut());
  }

  std::vector<Chip*> chips;
  std::vector<ChipPair> links;
  C2cBridge& in_stub;
  C2cBridge& out_stub;
  uint64_t limit;
  std::vector<MessagePtr> msgs;
  uint64_t sent = 0;
  uint64_t next_at = 2;
  MessagePtr cur;
  uint64_t beat = 0;
  uint64_t last_sig = 0, last_change = 0;
};

}  // namespace

TEST(PcodeBundle, Ep1OneToken) {
  std::string root = EnvOr("PCODE_BUNDLE_ROOT", "/tmp/latch_bundle");
  std::string name = EnvOr("PCODE_BUNDLE_NAME", "pcode_ep1");
  {
    std::ifstream probe(root + "/" + name + "/" + name + ".bachir");
    if (!probe.good()) {
      GTEST_SKIP() << "没有 pcode bundle：" << root << "/" << name;
    }
  }
  const uint64_t kMaxCycles = std::stoull(EnvOr("PCODE_MAX_CYCLES", "400000"));
  // PCODE_TRACE 给出波形前缀时，48 颗 chip 都记 .trace，跑完再转 Perfetto。
  std::string trace_name = EnvOr("PCODE_TRACE", "");
  uint64_t trace_chips = trace_name.empty() ? 0 : kLpuChips;

  uint64_t stopped = 0, b_insts = 0, compute_insts = 0, inflight = 0;
  double cosine = 0, concat_cosine = 0;
  uint32_t sm_head = 0, sm_tail = 0;
  uint64_t snap_n = 0;
  // full：FC1 和 concat 都要 ≥ 0.99。fc1：只查 FC1。idle：只看有没有在周期上限前停钟。
  std::string check = EnvOr("PCODE_CHECK", "full");
  if (trace_chips != 0) TraceInto(trace_name);
  {
    EnsureSlots(kLpuChips * (kMaxCorePerChip + 1));
    ClockPtr clk = MakeClock(0, kPeriod);
    std::vector<std::unique_ptr<Chip>> owned = MakeLpuChips(clk, trace_chips);
    std::vector<Chip*> all;
    for (auto const& c : owned) all.push_back(c.get());
    C2cBridge feed(clk, "feed", StubCfg());
    C2cBridge sink(clk, "sink", StubCfg());
    BundleStat st = LoadBundle(all, root, name);
    ASSERT_EQ(st.chips, kLpuChips);
    std::cerr << "  bundle: entries " << st.entries << " tasks " << st.tasks
              << " aux " << DtePayloadAuxBytes() << "\n";
    if (std::getenv("PCODE_DUMP_CFG") != nullptr) {
      Core& c = all[0]->GetCore(1);
      std::cerr << "  through 0x" << std::hex << c.GetTs().Cfg().ThroughEndMask()
                << std::dec << "\n";
      for (uint64_t t = 0; t < 8; ++t) {
        TaskEntry const& e = c.GetTs().Cfg().Task(t);
        if (!e.valid) break;
        std::cerr << "  task " << t << " type " << int(e.task_type)
                  << " wake " << e.wait_wake << " credit " << e.credit_en
                  << " path " << e.path_id << " end " << e.end << "\n";
      }
      RouteEntry const& re = c.GetRouter().Table().Lookup(0, 1);
      std::cerr << "  path1 op " << int(re.operation) << " mask " << re.reduce_in_mask
                << " flow " << re.flow_dir << " valid " << re.valid << "\n";
      return;
    }
    // 第 u 个用户的组内序号从 8u 起，共 8 个。越出前 20 个物理专家就折回。
    uint64_t user_count = std::stoull(EnvOr("PCODE_USERS", std::to_string(kUserCount)));
    std::vector<MessagePtr> tokens;
    for (uint64_t u = 0; u < user_count; ++u) {
      tokens.push_back(MakeEp1Token(kUserBase + u, u * kRouted));
    }
    std::cerr << "  灌入前 " << kExpertPool << " 个专家里被点到的权重\n";
    PokeExperts(all, tokens);
    PcodeHarness harness(clk, all, GridLinks(kGridY, kGridX), feed, sink, kMaxCycles,
                         tokens);
    clk->Continue();
    RT::JoinAll();
    stopped = harness.stopped_at;
    Core& b = all[0]->GetCore(5);
    b_insts = InstsOf(b);
    for (uint64_t i = 0; i < kLpuChips; ++i) {
      for (uint64_t k = 0; k < kChipCoreNum; ++k) {
        Core& core = all[i]->GetCore(k);
        if (core.Bad()) continue;
        compute_insts += InstsOf(core);
        inflight += core.GetTs().Table().Occupied();
      }
    }
    auto hex8 = [](Core& c, uint64_t off) {
      ByteBlock bytes = c.Cmem().Peek(off, 8);
      std::cerr << std::hex;
      for (uint64_t i = 0; i < bytes.size(); ++i) {
        std::cerr << (i ? " " : "") << int(bytes[i]);
      }
      std::cerr << std::dec;
    };
    for (uint64_t i = 0; i < kLpuChips; ++i) {
      for (uint64_t k = 0; k < kChipCoreNum; ++k) {
        Core& c = all[i]->GetCore(k);
        if (c.Bad() || c.GetTs().Table().Occupied() == 0) continue;
        bool only0 = true;
        uint64_t nvalid = 0;
        for (uint64_t s = 0; s < kStreamNum; ++s) {
          StreamEntry const& e = c.GetTs().Table().Peek(s);
          if (!e.valid) continue;
          ++nvalid;
          if (e.task_id != 0) only0 = false;
        }
        if (only0 && nvalid > 4) continue;
        std::cerr << "  stuck chip " << i << " p" << k;
        for (uint64_t s = 0; s < kStreamNum; ++s) {
          StreamEntry const& e = c.GetTs().Table().Peek(s);
          if (!e.valid) continue;
          std::cerr << " t" << e.task_id << " u" << e.user_id
                    << (e.user_id_vld ? "" : "?")
                    << " fsm " << int(e.task_fsm)
                    << " done 0x" << std::hex << e.done_bitmap << std::dec
                    << " path " << e.task_path_id
                    << " type " << int(e.task_type)
                    << " rmem " << e.rmem_busy;
        }
        std::cerr << " q";
        for (uint64_t d = 0; d < 4; ++d) {
          RouterStation& rs = c.GetRouter().Station(d);
          std::cerr << " " << rs.QueueDepth();
          uint64_t hp = 0, hu = 0;
          if (rs.QueueHead(hp, hu)) std::cerr << "(p" << hp << "u" << hu << ")";
        }
        auto red = c.GetRouter().GetReduce().Inspect();
        Xbar& xb = c.GetRouter().GetXbar();
        std::cerr << " xb";
        for (uint64_t d = 0; d < kXbarInNum; ++d) {
          std::cerr << " " << xb.InDepth(d);
          uint64_t xp = 0, xm = 0;
          bool xh = false;
          if (xb.InFront(d, xp, xm, xh)) {
            std::cerr << (xh ? "h" : "b") << "u" << xb.InFrontUser(d) << "p"
                      << xp << ":" << std::hex << xm << std::dec
                      << "w" << xb.HoldWhy(d);
          }
        }
        std::cerr << " own";
        for (uint64_t v = 0; v < kVcNum; ++v) {
          std::cerr << " " << xb.OwnerOf(kOutCore, v);
        }
        CoreStation& cs = c.GetRouter().GetCoreStation();
        std::cerr << " cbuf " << cs.OutBufDepth() << " hdr " << cs.HeaderDepth()
                  << " trig " << cs.TriggerDepth();
        for (auto const& id : cs.HeaderIds()) {
          std::cerr << " h" << id.first << ":" << id.second;
        }
        uint64_t ou = 0, os = 0, oz = 0;
        if (c.GetRouter().GetReduce().OutFront(ou, os, oz)) {
          std::cerr << " out u" << ou << " " << os << "/" << oz;
        }
        std::cerr << " lv " << xb.ReduceReady(0) << xb.ReduceReady(1)
                  << xb.ReduceReady(2);
        if (red.input_depth[0] || red.input_depth[1] || red.input_depth[2]) {
          std::cerr << " in " << red.input_depth[0] << " " << red.input_depth[1]
                    << " " << red.input_depth[2];
        }
        for (auto const& t : red.tasks) {
          std::cerr << " red u" << t.user_id << " p" << t.path_id
                    << " seq " << t.reduce_seq
                    << (t.busy ? " busy" : "") << (t.reserved ? " rsv" : "")
                    << " exp 0x" << std::hex << t.expect_mask << " got 0x"
                    << t.in_done_mask << std::dec;
        }
        if (red.deny_why != 0) {
          std::cerr << " deny " << red.deny_why << " u" << red.deny_user
                    << " p" << red.deny_path << " held " << red.deny_held;
        }
        std::cerr << "\n";
      }
    }
    // 空闲停钟只看任务表。包还在 VC 里、任务表已经空时，上面那段不会打印。
    for (uint64_t i = 0; i < kLpuChips; ++i) {
      for (uint64_t k = 0; k < kChipCoreNum; ++k) {
        Core& c = all[i]->GetCore(k);
        if (c.Bad()) continue;
        Router& r = c.GetRouter();
        uint64_t qd[4] = {};
        bool any = false;
        for (uint64_t d = 0; d < 4; ++d) {
          qd[d] = r.Station(d).QueueDepth();
          if (qd[d] != 0) any = true;
        }
        Xbar& xb = r.GetXbar();
        uint64_t xd[8] = {};
        for (uint64_t d = 0; d < kXbarInNum && d < 8; ++d) {
          xd[d] = xb.InDepth(d);
          if (xd[d] != 0) any = true;
        }
        CoreStation& cs = r.GetCoreStation();
        if (cs.OutBufDepth() != 0 || cs.HeaderDepth() != 0) any = true;
        if (!any) continue;
        std::cerr << "  queued chip " << i << " p" << k << " q";
        for (uint64_t d = 0; d < 4; ++d) {
          std::cerr << " " << qd[d];
          uint64_t hp = 0, hu = 0;
          if (qd[d] && r.Station(d).QueueHead(hp, hu)) {
            std::cerr << "(p" << hp << "u" << hu << ")";
          }
        }
        std::cerr << " xb";
        for (uint64_t d = 0; d < kXbarInNum && d < 8; ++d) {
          if (xd[d] == 0) continue;
          std::cerr << " " << d << ":" << xd[d];
          uint64_t xp = 0, xm = 0;
          bool xh = false;
          if (xb.InFront(d, xp, xm, xh)) {
            std::cerr << (xh ? "h" : "b") << "u" << xb.InFrontUser(d) << "p"
                      << xp << ":" << std::hex << xm << std::dec
                      << "w" << xb.HoldWhy(d);
          }
        }
        std::cerr << " cbuf " << cs.OutBufDepth() << " hdr " << cs.HeaderDepth();
        uint64_t shown = 0;
        for (auto const& id : cs.HeaderIds()) {
          if (shown >= 8) break;
          std::cerr << " h" << id.first << ":" << id.second;
          ++shown;
        }
        if (cs.HeaderDepth() > shown) std::cerr << " ...";
        std::cerr << "\n";
      }
    }
    std::cerr << "  silu@0 ";
    hex8(all[4]->GetCore(9), 0);
    std::cerr << "\n  silu@900 ";
    hex8(all[4]->GetCore(9), 0x900);
    std::cerr << "\n  silu@1200 ";
    hex8(all[4]->GetCore(9), 0x1200);
    std::cerr << "\n  fc3@18c0 ";
    hex8(all[4]->GetCore(9), 0x18c0);
    std::cerr << "\n  fc2@500 ";
    hex8(all[8]->GetCore(1), 0x500);
    std::cerr << "\n  concat@B00 ";
    hex8(all[47]->GetCore(8), 0xB00);
    std::cerr << "\n";
    cosine = 1;
    concat_cosine = 1;
    snap_n = harness.snaps.size();
    EXPECT_EQ(harness.snaps.size(), tokens.size());
    // idle 只判断有没有跑完。16 路 stream 上逐用户搜余弦，128 用户要扫两千多次整段 concat。
    bool skip_cosine = check == "idle";
    if (harness.snaps.size() >= 2) {
      bool same = harness.snaps[0].fc1 == harness.snaps[1].fc1 &&
                  harness.snaps[0].concat == harness.snaps[1].concat;
      std::cerr << "  两份快照" << (same ? "相同" : "不同") << "\n";
    }
    // pcie_sw_data_in 把 stream 计数写进 ShareMem head（1024 份流表之后）。
    // tail 由 check flag 推进。head 不到用户数就是还有包没落进 MM。
    {
      ByteBlock ht = all[0]->GetCore(5).Smem().Peek(1024 * 4, 8);
      uint32_t head = uint32_t(ht[0]) | (uint32_t(ht[1]) << 8) |
                      (uint32_t(ht[2]) << 16) | (uint32_t(ht[3]) << 24);
      uint32_t tail = uint32_t(ht[4]) | (uint32_t(ht[5]) << 8) |
                      (uint32_t(ht[6]) << 16) | (uint32_t(ht[7]) << 24);
      sm_head = head;
      sm_tail = tail;
      std::cerr << "  B core ShareMem head " << head << " tail " << tail;
      ByteBlock map = all[0]->GetCore(5).Smem().Peek(0, 16);
      std::cerr << " stream_map";
      for (int i = 0; i < 4; ++i) {
        uint32_t w = uint32_t(map[i * 4]) | (uint32_t(map[i * 4 + 1]) << 8) |
                     (uint32_t(map[i * 4 + 2]) << 16) |
                     (uint32_t(map[i * 4 + 3]) << 24);
        std::cerr << " [" << i << "]=" << w;
      }
      Core& bcore = all[0]->GetCore(5);
      std::cerr << " xbar";
      for (uint64_t d = 0; d < 4; ++d) {
        std::cerr << " " << bcore.GetRouter().GetXbar().StreamUsed(d);
      }
      std::cerr << " credit_q " << bcore.GetRouter().GetMonitor().QueueLen()
                << "\n";
    }
    std::cerr << std::setprecision(9);
    // 计算核按 stream 把结果隔开 kStreamStride。16 名叠在一起跑，停钟后
    // 每人一份，按余弦对上参考。
    auto load_f32 = [](ByteBlock const& raw) {
      std::vector<float> got(kFc1N, 0.0f);
      for (uint64_t i = 0; i < kFc1N && i * 4 + 3 < raw.size(); ++i) {
        uint32_t w = uint32_t(raw[i * 4]) | (uint32_t(raw[i * 4 + 1]) << 8) |
                     (uint32_t(raw[i * 4 + 2]) << 16) | (uint32_t(raw[i * 4 + 3]) << 24);
        got[i] = numeric::FloatOf(w);
      }
      return got;
    };
    auto load_bf16 = [](ByteBlock const& raw, uint64_t n) {
      std::vector<float> got(n, 0.0f);
      for (uint64_t i = 0; i < n && i * 2 + 1 < raw.size(); ++i) {
        uint16_t h = uint16_t(raw[i * 2]) | (uint16_t(raw[i * 2 + 1]) << 8);
        got[i] = numeric::FromBf16(h);
      }
      return got;
    };
    if (skip_cosine) {
      cosine = -1;
      concat_cosine = -1;
    }
    for (uint64_t u = 0; !skip_cosine && u < tokens.size(); ++u) {
      std::vector<float> want = GoldenFc1(tokens[u]);
      std::vector<float> want_concat = GoldenConcat(tokens[u]);
      double best_fc1 = -1, best_cat = -1;
      uint64_t sid_fc1 = 0, sid_cat = 0;
      for (uint64_t sid = 0; sid < 16; ++sid) {
        ByteBlock raw = all[0]->GetCore(kFc1Core).Cmem().Peek(
            kFc1Out + sid * kStreamStride, kFc1N * 4);
        double c = Cosine(load_f32(raw), want);
        if (c > best_fc1) {
          best_fc1 = c;
          sid_fc1 = sid;
        }
        ByteBlock cat = all[47]->GetCore(8).Cmem().Peek(
            kConcatAt + sid * kStreamStride, kConcatN * 2);
        double cc = Cosine(load_bf16(cat, want_concat.size()), want_concat);
        if (cc > best_cat) {
          best_cat = cc;
          sid_cat = sid;
        }
      }
      std::cerr << "  用户 " << tokens[u]->user_id << " FC1 余弦 " << best_fc1
                << " stream " << sid_fc1 << "，concat 余弦 " << best_cat
                << " stream " << sid_cat << "\n";
      if (best_fc1 < cosine) cosine = best_fc1;
      if (best_cat < concat_cosine) concat_cosine = best_cat;
    }
    if (tokens.size() >= 2) {
      double g = Cosine(GoldenConcat(tokens[0]), GoldenConcat(tokens[1]));
      std::cerr << "  两份 concat 参考之间的余弦 " << g << "\n";
    }
  }
  if (trace_chips != 0) TraceDone();
  std::cerr << "  停钟在第 " << stopped << " 拍，B core 指令 " << b_insts
            << "，全部指令 " << compute_insts << "，在途 " << inflight << "\n";
  std::cerr << "EP1_RESULT check " << check
            << " stop " << stopped
            << " limit " << kMaxCycles
            << " snaps " << snap_n
            << " inflight " << inflight
            << " insts " << compute_insts
            << " fc1 " << std::setprecision(6) << cosine
            << " concat " << concat_cosine
            << " head " << sm_head
            << " tail " << sm_tail
            << "\n";
  EXPECT_GT(stopped, 2u);
  EXPECT_LT(stopped, kMaxCycles);
  EXPECT_GT(b_insts, 0u);
  if (check == "full" || check == "fc1") {
    EXPECT_GE(cosine, kMinCosine);
  }
  if (check == "full") {
    EXPECT_GE(concat_cosine, kMinCosine);
  }
}
