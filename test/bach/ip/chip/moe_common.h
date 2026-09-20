#ifndef _LATCH_TEST_BACH_IP_CHIP_MOE_COMMON_
#define _LATCH_TEST_BACH_IP_CHIP_MOE_COMMON_

// 一层 MoE 按 EP6+TP8 的 KN 拆分摊在多颗 chip 上那几个用例共用的部分：比对向量
// 的读入、每个 core 的数据、weights 加载阶段、驱动几颗 chip 的那个协程，以及
// 按 chip、按 R core 核对中间量。配置表与 kernel 一律从编译器产的 bundle 装，
// 这里不铺路由表也不写任务链；只有 weights 加载那一条 path 不在 bundle 里，在
// 这里铺。
//
// Core Mem、Matrix Mem 上的摆放与数据的生成照 kn_data.h，那一份与
// compiler/kernel/bach.h、compiler/reference/vectors.py 同源。

#include <gtest/gtest.h>

#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/runtime.h"
#include "bach/common/numeric/formats.h"
#include "bach/ip/bundle_load.h"
#include "bach/ip/chip/chip.h"
#include "bach/ip/lpu_grid.h"
#include "test/bach/ip/chip/kn_data.h"

namespace latch {
namespace bach {
namespace moetest {

constexpr Time kPeriod = 1;

// 每个挂时钟的模块占一个常驻协程，槽位总数是 sub_thread × co_thread。槽位不够
// 时多出来的协程排在 pending 里永远等不到空位，表现是进程卡住而不是报错，所以
// 建时钟这一侧要数清楚要多少个。coros 是本次要各占协程的模块数，另外留出
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
//
// 一行一条“键 值”，值是十六进制字节串或十进制数。

struct Vectors {
  std::map<std::string, std::string> kv;

  bool Empty() const { return kv.empty(); }
  bool Has(std::string const& key) const { return kv.count(key) != 0; }
  std::vector<uint8_t> Bytes(std::string const& key) const {
    std::vector<uint8_t> v;
    auto it = kv.find(key);
    if (it == kv.end()) {
      ADD_FAILURE() << "比对向量里没有 " << key;
      return v;
    }
    std::string const& s = it->second;
    v.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
      v.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
    }
    return v;
  }
};

inline Vectors ReadVectors(std::string const& name) {
  Vectors c;
  std::ifstream f(std::string(LATCH_SOURCE_DIR) +
                  "/src/bach/compiler/reference/vectors/" + name);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    size_t sp = line.find(' ');
    if (sp == std::string::npos) continue;
    c.kv[line.substr(0, sp)] = line.substr(sp + 1);
  }
  return c;
}

// 两段字节逐个比，报出第一处不同。整段打印一万多个字节看不出东西。
inline void ExpectSame(std::vector<uint8_t> const& got,
                       std::vector<uint8_t> const& want,
                       std::string const& what) {
  if (got.size() != want.size()) {
    ADD_FAILURE() << what << "：长度是 " << got.size() << "，该是 "
                  << want.size();
    return;
  }
  for (size_t i = 0; i < got.size(); ++i) {
    if (got[i] == want[i]) continue;
    ADD_FAILURE() << what << "：第 " << i << " 个字节是 0x" << std::hex
                  << int(got[i]) << "，该是 0x" << int(want[i]) << std::dec;
    return;
  }
}

// ── 每个 core 的数据 ──

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

// topK 里第 0 个是全局 17 号专家、组内第 1 个，第 1 个是全局 5 号、组内第 0 个。
constexpr uint64_t kLocal[kn::kExperts] = {1, 0};

// boot 期铺进一个计算 core 的数据：本组专家表、topK 表与本 core 分到的那一片
// 权重。group 是 EP 组号，chip 是这颗 chip 在组里的序号，slot 是这个 core 的
// 逻辑槽位。
inline void SetUpCoreData(Core& core, uint64_t group, uint64_t chip,
                          uint64_t slot) {
  core.GetMu().EpInfo().SetLocalEpTable({5, 17});
  // topK 表按 stream 放：MU 按 topk_addr + stream_id × stride 读，每个 stream 各
  // 占一段。多 token 各自落在自己的 stream 上，得各有一份，否则只有 0 号 stream
  // 有专家、别的 stream 算出全 0。
  auto topk = TopkBytes({{17, kn::kWep[0]}, {5, kn::kWep[1]}});
  for (uint64_t s = 0; s < kn::kStreamNum; ++s) {
    core.Cmem().Poke(kn::kTopkOff + s * kn::kStreamStride, topk);
  }
  kn::PokeCoreWeights(core.Mmem(), group, chip, slot, kLocal);
}

// ── chip 内的几何 ──
//
// 每颗 chip 2×5 共 10 个 core，8 个计算 core 当 2 行 4 列的格子用，槽位号是
// 行 × 4 + 逻辑列。逻辑列到物理列按 chip 所在列换算：第一列 chip 是 1、2、3、4，
// 中间两列是 0、1、3、4，最后一列是 0、1、2、3。与 compiler/hwconfig/moe.py 同源。
constexpr uint64_t kCols = 4;
constexpr uint64_t kCorePerChip = 8;

inline uint64_t PhysColOf(uint64_t gx, uint64_t col) {
  static const uint64_t kFirst[kCols] = {1, 2, 3, 4};
  static const uint64_t kMiddle[kCols] = {0, 1, 3, 4};
  static const uint64_t kLast[kCols] = {0, 1, 2, 3};
  if (gx == 0) return kFirst[col];
  if (gx + 1 == kGridX) return kLast[col];
  return kMiddle[col];
}

// 槽位换成片内 core 号。gx 是 chip 所在列。
inline uint64_t CoreOfSlot(uint64_t gx, uint64_t slot) {
  return slot / kCols * kChipCols + PhysColOf(gx, slot % kCols);
}

// R core 坐在最后一列 chip 的 core9 上。
constexpr uint64_t kRcoreId = kChipCoreNum - 1;

// ── 几条 path 与 token ──
//
// 与 compiler/hwconfig/moe.py 同源。
constexpr uint64_t kInPath = 4;        // token 广播进各计算 core
constexpr uint64_t kBcastInPath = 5;   // 送进 B core 那一段
constexpr uint64_t kUserId = 77;

// 第几笔落在 B core 的 Matrix Mem 哪里。发方按送出的笔数算，B core 按收下的笔
// 数算，同一条规则。与 compiler/kernel/bach.h 的 BC_SLOTS 与 bc_land 同源。
constexpr uint64_t kBcSlots = 16;
inline uint64_t BcoreLand(uint64_t seq) {
  return (seq % kBcSlots) * kn::kBcTokenBytes;
}

// 一个 token：6144 个 MXFP8，192 个 scale 接在后面。path 与落点由调用方给：从
// B core 进来的落它的 Matrix Mem，直接进计算 core 的落 Core Mem。
inline MessagePtr MakeToken(uint64_t path, uint64_t dst) {
  auto m = std::make_shared<Message>();
  m->path_id = path;
  m->user_id = kUserId;
  m->gpu_id = 2;
  m->token_id = 1;
  m->stream_id = 0;
  m->task_id = 0;
  m->dst_addr = dst;
  m->scale_valid = 1;
  m->payload = kn::TokenData();
  std::vector<uint8_t> scale = kn::TokenScale();
  m->payload.insert(m->payload.end(), scale.begin(), scale.end());
  m->size = m->payload.size();
  return m;
}

// 第 seq 个 token：与单 token 版差在 user_id / token_id / stream_id。三者按 seq
// 错开，让每个 token 各占一条 stream 片（Core Mem 落点、Hmem、topK）与一个 R
// core 槽（RcLand 按 user_id % 16），结果互不覆盖；token_id 经 Hmem 原样带进
// 出口包，用来按 token 区分结果。seq 从 0 起，同飞上限 kn::kStreamNum。
inline MessagePtr MakeToken(uint64_t path, uint64_t dst, uint64_t seq) {
  MessagePtr m = MakeToken(path, dst);
  m->user_id = kUserId + seq;
  m->token_id = seq + 1;
  m->stream_id = seq % kn::kStreamNum;
  return m;
}

// ── weights 加载阶段 ──
//
// 装模型时权重不经 SCP，走的是 Host 那条 msg 流：包从 Router 进来，Router 通知
// TS，TS 派 datain 任务给 DTE RV core 跑 weights loader，搬运把数据从 Router
// 落进 Matrix Mem，scale 随包头的标记落进 scale 旁带。这一阶段不建 stream 表项、
// 不启动任务链；各 core 数够了 SCP 才把 core 切到业务模式。

// weights 加载阶段走的那条 path：这一阶段只用一条，走遍格子里的 8 个计算 core，
// 一个包落哪个 core 由包头的 path_core_mask 挑，位号就是槽位号。bundle 里不用这个号。
constexpr uint64_t kWeightsPath = 12;
// 与 compiler/kernel/bach.h 的 WEIGHTS_CNT_OFF 同源。
constexpr uint64_t kWeightsCntOff = 0x05D0;
// 一个权重包搬多少数据：两个 tile，一包的 scale 另占 512 B。
constexpr uint64_t kWeightsChunk = 2 * kn::kTileBytes;
constexpr uint64_t kWeightsScale = kWeightsChunk / 32;
// 这一阶段的用户号。权重不进归约，与业务那个用户不相干。
constexpr uint64_t kWeightsUser = 3;

// 把一颗 core 配进 weights 加载阶段。任务链在这之前就由 bundle 配好了，切模式
// 只动 datain 那一项与进核那一档。
inline void EnterWeightsMode(Core& core) {
  core.GetTs().Cfg().WriteDatainTask(SymbolOf("task_dte_weights_loader", "dte"),
                                     /*weights_mode=*/true);
  core.GetTs().Cfg().SetTriggerChainEn(false);
  core.GetTs().InitFinish();
  core.GetDte().Tables().PreloadPathTask(kWeightsPath, 0);
  core.SetWeightsInbound();
}

// 切回业务：datain 改指 token 搬移那一笔，任务链放行，进核那一笔按 bundle 写入
// 的业务模式配置落回 Core Mem。
inline void EnterBusinessMode(Core& core) {
  core.GetTs().Cfg().WriteDatainTask(SymbolOf("task_dte_user_init", "dte"),
                                     /*weights_mode=*/false);
  core.GetTs().Cfg().SetTriggerChainEn(true);
  core.GetTs().InitFinish();
  core.SetBusinessInbound();
}

// weights 那条 path 的路由表，只铺第一列 chip：从 W 口进来，坐在口上的 core5
// 不是计算 core，往右转一跳；格子第 1 行往右铺满，行首那个槽位再经 mid 到第 0
// 行往右铺满。每一跳都开 path_core_mask，包走遍整棵树，只在自己那一位是 1 的
// core 上落地。
inline void WireWeightsPath(Chip& chip) {
  LOGCHECK(chip.Gx() == 0, "WireWeightsPath: 只铺第一列 chip。");
  RouteEntry in;
  in.flow_dir = kFlowRight;
  in.path_core_bypass = true;
  chip.GetCore(kChipCols).GetRouter().Preload(kWeightsPath, in);
  for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
    RouteEntry e;
    e.flow_dir = slot % kCols + 1 < kCols ? uint64_t(kFlowRight) : 0;
    if (slot == kCols) e.flow_dir |= kFlowMid;
    e.path_core_bypass = false;
    e.path_core_mask_enable = true;
    e.path_core_mask_idx = slot;
    chip.GetCore(CoreOfSlot(0, slot)).GetRouter().Preload(kWeightsPath, e);
  }
}

// 一个权重包：payload 是某一片权重里的一段数据接上它的 scale，落点是 Matrix Mem
// 上的最终地址，mask 挑收它的那个 core。
inline MessagePtr MakeWeightsMsg(uint64_t slot, uint64_t at,
                                 std::vector<uint8_t> data,
                                 std::vector<uint8_t> const& scale) {
  auto m = std::make_shared<Message>();
  m->path_id = kWeightsPath;
  m->path_core_mask = 1ull << slot;
  m->user_id = kWeightsUser;
  m->gpu_id = 2;
  m->dst_addr = at;
  m->scale_valid = 1;
  m->payload = std::move(data);
  m->payload.insert(m->payload.end(), scale.begin(), scale.end());
  m->size = m->payload.size();
  return m;
}

// ── 驱动 ──

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

// 几颗 chip 摆成 rows × cols，层内左右相接（E 对 W）、层间上下相接（S 对 N），
// 与 LPU 的 WireRow / WireCol 同一套接法。
inline std::vector<ChipPair> GridLinks(uint64_t rows, uint64_t cols) {
  std::vector<ChipPair> v;
  for (uint64_t gy = 0; gy < rows; ++gy) {
    for (uint64_t gx = 0; gx < cols; ++gx) {
      uint64_t a = gy * cols + gx;
      if (gx + 1 < cols) v.push_back({a, kChipE, a + 1, kChipW});
      if (gy + 1 < rows) v.push_back({a, kChipS, a + cols, kChipN});
    }
  }
  return v;
}

// 几颗 chip、两座接头桥与它们之间的段搬运，全由这一个协程驱动。出口上的 flit
// 是单拍脉冲，分到两个协程里谁先跑不定，会整拍错过。
class SpreadHarness : public BachModule {
 public:
  SpreadHarness(ClockPtr c, std::vector<Chip*> target,
                std::vector<ChipPair> pairs, C2cBridge& feed, C2cBridge& sink,
                uint64_t feed_chip, uint64_t feed_port, uint64_t sink_chip,
                uint64_t sink_port, uint64_t at, uint64_t cap, MessagePtr m)
      : BachModule(c, "harness"), chips(std::move(target)),
        links(std::move(pairs)), in_stub(feed), out_stub(sink),
        in_chip(feed_chip), in_port(feed_port), out_chip(sink_chip),
        out_port(sink_port), fire_at(at), limit(cap), msg(std::move(m)) {}

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
  // 业务 token 留空时用构造时传进来的那一笔；非空时按 token_gap 从注入那一拍起
  // 连续注入，每个 token 各占一条 stream 片。
  std::vector<MessagePtr> tokens;
  uint64_t token_gap = 0;
  // 切进业务模式、同时把 token 发出去的那一拍。
  uint64_t switched_at = 0;
  // 每个计算 core 的任务链都走空了才算这一笔真的走完。core 各占协程时这个数只
  // 能读打拍的那一份：head_ptr 与 tail_ptr 是 stream_table 自己 Step() 里改的普
  // 通成员，别的协程读它就是跨协程读非 atomic 的量。
  std::vector<uint64_t> inflight;

 protected:
  void Step() override {
    // 结果还没出来就不必挨个问：判停要两件事同时成立，先看便宜的那件。
    bool all_empty = false;
    if (out_msgs.size() >= ExpectedOutMsgs()) {
      inflight.clear();
      all_empty = true;
      for (Chip* c : chips) {
        for (uint64_t slot = 0; slot < kCorePerChip; ++slot) {
          uint64_t n = c->GetCore(CoreOfSlot(c->Gx(), slot)).GetTs().Table()
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

    // 收取那一头在最外，注入那一头在最里；chip 按编号倒着推。
    out_stub.RunStep();
    for (uint64_t i = chips.size(); i > 0; --i) chips[i - 1]->RunOutside();
    in_stub.RunStep();

    Move(in_stub, chips[in_chip]->Port(in_port));
    for (ChipPair const& p : links) {
      Move(chips[p.a]->Port(p.a_port), chips[p.b]->Port(p.b_port));
    }
    Move(chips[out_chip]->Port(out_port), out_stub);

    FlitView f = ReadFlit(out_stub.ToCore()->flit);
    if (f.valid && f.msg) out_msgs.push_back(f.msg);

    // 每个 token 的结果包都收到、所有计算 core 的任务链都走空就停表。拍数上限
    // 只作兜底：卡住时要停得下来，跑通时不必空转。
    if ((out_msgs.size() >= ExpectedOutMsgs() && all_empty) ||
        CycleNow() >= limit) {
      stopped_at = CycleNow();
      clk->Stop();
    }
  }

 private:
  // 这一拍往里注哪个包。weights 那一段一个一个来，注完等各 core 收够，切模式，
  // 再发 token。
  MessagePtr Pick() {
    uint64_t now = CycleNow();
    if (weights.empty()) return PickToken(now, fire_at);
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
    if (now == switched_at && to_business) to_business();
    return PickToken(now, switched_at);
  }

  // 业务 token：留空时在 base 那一拍发构造时传进来的那一笔；非空时按 token_gap
  // 从 base 起连续注入，每个 token 各占一条 stream 片。
  MessagePtr PickToken(uint64_t now, uint64_t base) {
    if (tokens.empty()) return now == base ? msg : MessagePtr();
    LOGCHECK(token_gap != 0, "SpreadHarness: 多 token 要给注入间隔。");
    if (now < base) return MessagePtr();
    uint64_t off = now - base;
    if (off % token_gap != 0) return MessagePtr();
    uint64_t i = off / token_gap;
    return i < tokens.size() ? tokens[i] : MessagePtr();
  }

  // 出口上应收到几个结果包：单 token 一包，多 token 每个 token 一包。
  uint64_t ExpectedOutMsgs() const {
    return tokens.empty() ? 1 : tokens.size();
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
  uint64_t fire_at, limit;
  MessagePtr msg;
  uint64_t sent = 0;
};

// ── 核对 ──
//
// 读的都是 stream 0 那一片：一个 token 在各 core 上占的是同一个 stream。

// 一颗 chip 上 dot core 拼好的 concat 区。stream 是 token 占的槽位，落点按
// stream 切片偏移。
inline std::vector<uint8_t> ConcatOf(Chip& chip, uint64_t stream = 0) {
  return chip.GetCore(CoreOfSlot(chip.Gx(), kn::kDotSlot))
      .Cmem()
      .Peek(kn::kConcatOff + stream * kn::kStreamStride,
            kn::kSlots * kn::kFc2Bytes);
}

// 一颗 chip 的中间量逐项与参考实现对：各 core 的部分和、FC2 输入与 FC2 那一段，
// dot core 上的归约结果与 concat。tag 是向量里这颗 chip 那几行的前缀。
inline void CheckChip(Chip& chip, Vectors const& want, std::string const& tag) {
  std::vector<uint8_t> concat;
  for (uint64_t s = 0; s < kn::kSlots; ++s) {
    Core& core = chip.GetCore(CoreOfSlot(chip.Gx(), s));
    std::string who = tag + "槽位 " + std::to_string(s) + " ";
    ExpectSame(core.Cmem().Peek(kn::kPartOff + kn::kSwHead,
                                kn::kPartBytes - kn::kSwHead),
               want.Bytes(tag + "part" + std::to_string(s)), who + "的部分和");
    for (uint64_t e = 0; e < kn::kExperts; ++e) {
      uint64_t at = kn::kActOff + e * kn::kActStride;
      ExpectSame(core.Cmem().Peek(at, kn::kSegInter),
                 want.Bytes(tag + "act" + std::to_string(e)),
                 who + "专家 " + std::to_string(e) + " 的 FC2 输入");
      ExpectSame(core.Cmem().PeekScale(at, kn::kSegInter / 32),
                 want.Bytes(tag + "act_scale" + std::to_string(e)),
                 who + "专家 " + std::to_string(e) + " 的 FC2 输入 scale");
    }
    std::vector<uint8_t> fc2 = want.Bytes(tag + "fc2_" + std::to_string(s));
    ExpectSame(core.Cmem().Peek(kn::ConcatOf(s), kn::kFc2Bytes), fc2,
               who + "的 FC2 那一段");
    concat.insert(concat.end(), fc2.begin(), fc2.end());
  }
  Core& dot = chip.GetCore(CoreOfSlot(chip.Gx(), kn::kDotSlot));
  ExpectSame(dot.Cmem().Peek(kn::kRedOff + kn::kSwHead,
                             kn::kPartBytes - kn::kSwHead),
             want.Bytes(tag + "red"), tag + "dot core 上的归约结果");
  ExpectSame(ConcatOf(chip), concat, tag + "dot core 上的 concat");
}

// R core 的 Matrix Mem 里这个用户那一槽的一半：跳过 16 B 头。half 为 0 是本行
// 结果，为 1 是上一行 R core 送来的累加结果。user 默认 kUserId，多 token 时每个
// token 用 kUserId + seq。
inline std::vector<uint8_t> RcoreHalf(Chip& chip, uint64_t half,
                                      uint64_t user = kUserId) {
  return chip.GetCore(kRcoreId).Mmem().Peek(
      kn::RcLand(user, half) + kn::kSwHead, kn::kRowBytes - kn::kSwHead);
}

// 出口上收到的结果包：单 token 一包，多 token 每个 token 一包，按 token_id 区分。
// dst 是第 0 个 token 的落点，落在哪一半看发这一包的是链尾哪一笔：出的是本行结果
// 就落 half 0（dot core 的 task_dte_send_row），出的是上一行累加过来的结果就落
// half 1（R core 的 task_dte_rc_send）。每个 token 各占一个 R core 槽
// （kUserId + i），落点按槽长递推。所有 token 输入数据相同，所以每一包的正文都该
// 等于同一份 want。
inline void CheckOut(std::vector<MessagePtr> const& got,
                     std::vector<uint8_t> const& want,
                     uint64_t dst, uint64_t token_count = 1) {
  ASSERT_EQ(got.size(), token_count) << "出口上要收到每个 token 一包结果";
  std::map<uint64_t, MessagePtr> by_token;
  for (MessagePtr const& m : got) by_token[m->token_id] = m;
  ASSERT_EQ(by_token.size(), token_count) << "每个 token 都要有一组输出";
  for (uint64_t i = 0; i < token_count; ++i) {
    auto it = by_token.find(i + 1);
    ASSERT_NE(it, by_token.end()) << "token " << i << " 的结果没收到";
    MessagePtr const& m = it->second;
    ASSERT_EQ(m->payload.size(), kn::kRowBytes);
    EXPECT_EQ(m->dst_addr, dst + i * kn::kRcSlotBytes);
    ExpectSame(std::vector<uint8_t>(m->payload.begin() + kn::kSwHead,
                                    m->payload.end()),
               want, "token " + std::to_string(i) + " 出口上收到的结果");
  }
}

inline void CheckInflight(std::vector<uint64_t> const& inflight) {
  for (uint64_t g = 0; g < inflight.size(); ++g) {
    EXPECT_EQ(inflight[g], 0u) << "第 " << g << " 个计算 core 的任务链没走到头";
  }
}

}  // namespace moetest
}  // namespace bach
}  // namespace latch

#endif
