#ifndef _LATCH_TEST_BACH_IP_CHIP_EP1_EXACT_COMMON_
#define _LATCH_TEST_BACH_IP_CHIP_EP1_EXACT_COMMON_

// EP1 exact-token 传输骨架。
//
// 只借 moe_common 的 MakeLpuChips / GridLinks / 两座桥。不铺合成权重，
// 不做 max-cos-over-streams，不读 golden。进核负荷是调用方给的字节，
// 落点与物理 core 也由调用方给，不从场景文件猜。
//
// 用户号、SID、allocation 要在 slot 被复用或退休之前抠下来。完成只认
// done_bitmap 覆盖 ThroughEndMask，不认“表空了”。快照走 StreamTable::SnapPtr()，
// 那是表每拍末发给其余 TS 模块的同一份公开口。

#include <gtest/gtest.h>

#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "bach/common/flit.h"
#include "bach/ip/chip/chip.h"
#include "bach/ip/chip/core/memory/core_mem.h"
#include "bach/ip/chip/core/ts/cfg_reg.h"
#include "bach/ip/chip/core/ts/stream_table.h"
#include "bach/ip/lpu_grid.h"
#include "base/clock.h"
#include "base/runtime.h"

namespace latch {
namespace bach {
namespace ep1exact {

constexpr Time kPeriod = 1;
constexpr uint64_t kSubThread = 16;

inline void EnsureSlots(uint64_t coros = 0) {
  uint64_t need = coros + 16;
  uint64_t co = (need + kSubThread - 1) / kSubThread;
  RT::Reset(kSubThread, co < 8 ? 8 : co);
}

// 两颗 chip 对接的一条 C2C。与 moe_common 的 ChipPair 同形。
// 这里自己写一份，免得带上旧测试的输入生成与 golden。
struct ChipPair {
  uint64_t a = 0, a_port = 0, b = 0, b_port = 0;
};

inline std::vector<ChipPair> GridLinks(uint64_t rows, uint64_t cols) {
  std::vector<ChipPair> v;
  for (uint64_t gy = 0; gy < rows; ++gy) {
    for (uint64_t gx = 0; gx < cols; ++gx) {
      uint64_t a = gy * cols + gx;
      if (gx + 1 < cols)
        v.push_back({a, kChipE, a + 1, kChipW});
      if (gy + 1 < rows)
        v.push_back({a, kChipS, a + cols, kChipN});
    }
  }
  return v;
}

inline C2cCfg StubCfg() {
  C2cCfg c;
  c.axi_latency = 0;
  return c;
}

// 保存的 token 文件：56 B topK + 6144 B 数据 + 192 B scale。
constexpr uint64_t kTopkBytes = 56;
constexpr uint64_t kDataBytes = 6144;
constexpr uint64_t kScaleBytes = 192;
constexpr uint64_t kSavedBytes = kTopkBytes + kDataBytes + kScaleBytes; // 6392
constexpr uint64_t kFlitBytesLocal = 256;

// 外部进核只用这一个 PathID。装完 bundle 之后先查被占用的表项，
// 冲突就失败，不覆盖 63 或任何已有项。
constexpr uint64_t kIngressPath = 62;

struct Endpoint {
  uint64_t chip = 0;
  uint64_t physical = 0;
  uint64_t logical = 0;
  uint64_t addr = 0;
};

struct Capture {
  uint64_t chip = 0;
  uint64_t physical = 0;
  uint64_t user_id = 0;
  uint64_t sid = 0;
  uint64_t allocation = 0;
  uint64_t cycle = 0;
  bool seen = false;
  bool user_stamped = false;
  bool completed = false;
  uint64_t completed_cycle = 0;
};

// 存放位置。topK / 数据 / scale / 包头各自一段，地址由调用方给出。
// 没给的段不读。这是落地后的字节，不是注入时那一份 Message。
struct StoreView {
  Core *core = nullptr;
  uint64_t data_addr = ~uint64_t{0};
  uint64_t scale_addr = ~uint64_t{0};
  uint64_t topk_addr = ~uint64_t{0};
  uint64_t header_addr = ~uint64_t{0};
  bool scale_on_cmem = true;
  bool topk_on_cmem = true;
  bool header_on_cmem = true;
};

struct StoreProbe {
  uint64_t topk = 0, data = 0, scale = 0, header = 0, mismatch = 0;
  uint64_t users = 0;
  std::string note;

  void ResetCounts() {
    topk = data = scale = header = mismatch = 0;
    note.clear();
  }

  void Compare(std::vector<uint8_t> const &got,
               std::vector<uint8_t> const &want, char const *what,
               uint64_t *count) {
    if (got.size() != want.size()) {
      ++mismatch;
      if (note.empty())
        note = std::string(what) + " 长度 " + std::to_string(got.size());
      return;
    }
    for (uint64_t i = 0; i < got.size(); ++i) {
      if (got[i] != want[i]) {
        ++mismatch;
        if (note.empty()) {
          std::ostringstream o;
          o << what << " 第 " << i << " 字节 0x" << std::hex << int(got[i])
            << " 该是 0x" << int(want[i]);
          note = o.str();
        }
        return;
      }
    }
    *count += want.size();
  }

  // 读这块 core 上已经落下去的字节。对不上就记 mismatch，不改期望。
  void Read(StoreView const &view, std::vector<uint8_t> const &saved) {
    if (!view.core || saved.size() != kSavedBytes)
      return;
    ++users;
    std::vector<uint8_t> want_topk(saved.begin(), saved.begin() + kTopkBytes);
    std::vector<uint8_t> want_data(saved.begin() + kTopkBytes,
                                  saved.begin() + kTopkBytes + kDataBytes);
    std::vector<uint8_t> want_scale(saved.end() - kScaleBytes, saved.end());
    if (view.data_addr != ~uint64_t{0}) {
      auto got = view.core->Cmem().Peek(view.data_addr, kDataBytes);
      Compare(got, want_data, "data", &this->data);
    }
    if (view.scale_addr != ~uint64_t{0}) {
      ByteBlock got = view.scale_on_cmem
                          ? view.core->Cmem().PeekScale(view.scale_addr,
                                                        kScaleBytes)
                          : view.core->Mmem().PeekScale(view.scale_addr,
                                                        kScaleBytes);
      Compare(got, want_scale, "scale", &this->scale);
    }
    if (view.topk_addr != ~uint64_t{0}) {
      ByteBlock got = view.topk_on_cmem
                          ? view.core->Cmem().Peek(view.topk_addr, kTopkBytes)
                          : view.core->Mmem().Peek(view.topk_addr, kTopkBytes);
      Compare(got, want_topk, "topk", &this->topk);
    }
    if (view.header_addr != ~uint64_t{0}) {
      ByteBlock got =
          view.header_on_cmem
              ? view.core->Cmem().Peek(view.header_addr, 32)
              : view.core->Mmem().Peek(view.header_addr, 32);
      if (got.size() == 32)
        header += 32;
      else {
        ++mismatch;
        if (note.empty())
          note = "header 读不到 32 B";
      }
    }
  }
};

struct StoredOutput {
  Capture owner;
  ByteBlock bytes;
};

struct RunResult {
  std::map<uint64_t, StoredOutput> stored_outputs;
  std::vector<MessagePtr> out_msgs;
  std::vector<uint64_t> out_at;
  uint64_t stopped_at = 0;
  bool completed_by_end = false;
  bool still_occupied_at_end = false;
  uint64_t expected = 0;
  std::vector<Capture> captures;
  uint64_t dte_error = 0;
  uint64_t probes_topk = 0;
  uint64_t probes_data = 0;
  uint64_t probes_scale = 0;
  uint64_t probes_header = 0;
  uint64_t probes_mismatch = 0;
  uint64_t probes_users = 0;
  std::string probe_note;
  bool store_checked = false;
};

inline std::string EnvOr(char const *name, std::string const &fallback) {
  char const *v = std::getenv(name);
  return v && *v ? std::string(v) : fallback;
}

inline uint64_t EnvU64(char const *name, uint64_t fallback) {
  char const *v = std::getenv(name);
  if (!v || !*v)
    return fallback;
  return std::stoull(v, nullptr, 0);
}

// 进核期望逻辑 8，出核期望逻辑 7。地址必须由环境给出，没有默认的 0xb00。
inline Endpoint IngressFromEnv() {
  Endpoint e;
  e.chip = EnvU64("BACHFLOW_INGRESS_CHIP", ~uint64_t{0});
  e.physical = EnvU64("BACHFLOW_INGRESS_PHYS", ~uint64_t{0});
  e.addr = EnvU64("BACHFLOW_INGRESS_ADDR", ~uint64_t{0});
  return e;
}

inline Endpoint EgressFromEnv() {
  Endpoint e;
  e.chip = EnvU64("BACHFLOW_EGRESS_CHIP", ~uint64_t{0});
  e.physical = EnvU64("BACHFLOW_EGRESS_PHYS", ~uint64_t{0});
  e.addr = EnvU64("BACHFLOW_EGRESS_ADDR", ~uint64_t{0});
  return e;
}

inline bool EndpointSet(Endpoint const &e) {
  return e.chip != ~uint64_t{0} && e.physical != ~uint64_t{0} &&
         e.addr != ~uint64_t{0};
}

// 装载后的逻辑图。LoadBundle 的 BundleStat.logical 是同一形状：
// (chip, physical) → 逻辑号，-1 表示不映射。这里不包含 bundle_load.h。
using LogicalMap = std::map<std::pair<uint64_t, uint64_t>, int64_t>;

inline bool LogicalOf(LogicalMap const &logical, uint64_t chip,
                      uint64_t physical, int64_t *out) {
  auto it = logical.find({chip, physical});
  if (it == logical.end())
    return false;
  *out = it->second;
  return true;
}

inline std::vector<std::pair<uint64_t, uint64_t>>
FindLogical(LogicalMap const &logical, int64_t want) {
  std::vector<std::pair<uint64_t, uint64_t>> out;
  for (auto const &one : logical) {
    if (one.second == want)
      out.push_back(one.first);
  }
  return out;
}

// 进核 PathID 必须空着。valid 的表项算占用，不论是不是 63。
// 表还是 64 项时，250 越界，返回 false，不进 Lookup 的断言。
inline bool PathFree(Core &core, uint64_t path) {
  if (path >= kPathNum || path == 63)
    return false;
  RouteEntry const &e = core.GetRouter().Table().Lookup(0, path);
  return !e.valid;
}

// 进核表项：从指定的片外口进来，只进这一个物理 core，不转发。
// 这是 harness 自己的进口，不是 bundle 里的任何一条 path。
inline RouteEntry IngressEntry() {
  RouteEntry e;
  e.valid = true;
  e.op_type = OpType::kTransfer;
  e.flow_dir = 0;
  e.path_core_bypass = false;
  e.path_core_mask_enable = false;
  e.stream_need = 0;
  e.operation = Operation::kForward;
  e.stall_way = false;
  return e;
}

// 现有输入流 stride 是 0x10000，数据 K 是 6144。scale 旁带是 6144/32 = 192 B。
// 传输本身不写这份旁带时，允许在 token 跑起来之前按流 PokeScale。
// 这只是临时的输入预填：不写权重 scale，也不覆盖 FC2 的 VU 产出。
// 线上 scale 段仍按 exact token 的 192 B 比对，预填不算进那一次比对。
constexpr uint64_t kInputStride = 0x10000;
constexpr uint64_t kScaleGroups = kDataBytes / 32; // 192
constexpr uint64_t kInputStreams = 16;
constexpr uint8_t kSavedScaleByte = 0x7f;

inline void PokeInputScale(CoreMem &mem, std::vector<uint8_t> const &saved,
                           uint64_t base) {
  LOGCHECK(saved.size() == kSavedBytes, "scale 预填要整份 6392 B。");
  std::vector<uint8_t> scale(saved.begin() + kTopkBytes + kDataBytes,
                             saved.end());
  LOGCHECK(scale.size() == kScaleGroups, "输入 scale 不是 192 B。");
  for (uint64_t sid = 0; sid < kInputStreams; ++sid)
    mem.PokeScale(base + sid * kInputStride, scale);
}

// 编译入口段1是 CoreMem topK，段2是连续 MatrixMem 数据+SF。
// 线上顺序严格按保存文件：56 B topK、6144 B 数据、192 B scale。
inline MessagePtr ExactToken(uint64_t user, uint64_t path, uint64_t dst,
                             std::vector<uint8_t> const &saved,
                             uint64_t token_id) {
  LOGCHECK(saved.size() == kSavedBytes, "exact token 文件不是 6392 B。");
  auto m = std::make_shared<Message>();
  m->path_id = path;
  m->user_id = user;
  m->token_id = token_id;
  m->gpu_id = 1;
  m->dst_addr = dst;
  m->scale_valid = 1;
  m->topk_valid = 1;
  m->payload_contains_topk = true;
  m->payload = saved;
  m->topk.assign(saved.begin(), saved.begin() + kTopkBytes);
  m->size = kSavedBytes;
  LOGCHECK(m->size == kSavedBytes, "exact token 线上长度不是 6392 B。");
  return m;
}

inline std::vector<uint8_t> DeterministicSaved(uint64_t user) {
  std::vector<uint8_t> b(kSavedBytes);
  for (uint64_t i = 0; i < kSavedBytes; ++i)
    b[i] = uint8_t((user * 131u + i * 17u + 3u) & 0xFFu);
  // topK 前两字节是 ep_counter。固定成 8，后面的字节仍由用户号决定，
  // 两个 token 不会碰上同一份。
  b[0] = 8;
  b[1] = 0;
  return b;
}

inline bool LoadSavedFile(std::string const &path, std::vector<uint8_t> *out,
                          std::string *why) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    *why = "打不开 " + path;
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  if (out->size() != kSavedBytes) {
    *why = path + " 长度是 " + std::to_string(out->size()) + "，要 6392";
    return false;
  }
  return true;
}

using SlotKey = std::tuple<uint64_t, uint64_t, uint64_t, uint64_t>;

// 完成：这一项的 done_bitmap 覆盖任务链的 end 位。不看 Occupied。
// through 为 0（还没 InitFinish）不算完成。
inline bool EndCovered(StreamEntry const &e, uint64_t through) {
  return e.valid && e.user_id_vld && through != 0 &&
         (e.done_bitmap & through) == through;
}

// 一拍里抄下还活着的表项。键是 chip、物理 core、SID、allocation。
// allocation 只在这一颗 core 上有意义，不能和别的 core 混。
// 用户号只在 RV ACK 已经把它写进表（user_id_vld）之后才记。
// harness 不往表里写用户号。
inline void NoteLive(uint64_t chip, uint64_t physical,
                     StreamSnapshot const &snap, uint64_t cycle,
                     std::map<SlotKey, Capture> *held) {
  for (uint64_t sid = 0; sid < snap.slot_num && sid < kStreamNum; ++sid) {
    StreamEntry const &e = snap.entry[sid];
    if (!e.valid)
      continue;
    Capture &c = (*held)[SlotKey{chip, physical, sid, e.allocation}];
    if (!c.seen) {
      c.seen = true;
      c.chip = chip;
      c.physical = physical;
      c.sid = sid;
      c.allocation = e.allocation;
      c.cycle = cycle;
    }
    if (e.user_id_vld) {
      c.user_id = e.user_id;
      c.user_stamped = true;
    }
  }
}

// 这一拍 end 位已经盖住，就在同一颗 core、同一个 allocation 上锁存。
// 下一拍槽可能已经退休，不能要求所有用户在同一拍都还活着。
inline void LatchDone(uint64_t chip, uint64_t physical,
                      StreamSnapshot const &snap, uint64_t through,
                      uint64_t cycle, std::map<SlotKey, Capture> *held) {
  if (through == 0)
    return;
  for (uint64_t sid = 0; sid < snap.slot_num && sid < kStreamNum; ++sid) {
    StreamEntry const &e = snap.entry[sid];
    if (!EndCovered(e, through))
      continue;
    auto it = held->find(SlotKey{chip, physical, sid, e.allocation});
    if (it == held->end() || it->second.completed)
      continue;
    it->second.completed = true;
    it->second.completed_cycle = cycle;
    if (e.user_id_vld) {
      it->second.user_id = e.user_id;
      it->second.user_stamped = true;
    }
  }
}

// 落地后的字节比对。注入时那一份 Message 跟自己比不算数。
// 每个用户单独计，不把上一份的 done 留到下一份。
class ExactHarness : public BachModule {
public:
  ExactHarness(ClockPtr c, std::vector<Chip *> target,
               std::vector<ChipPair> pairs, C2cBridge &feed, C2cBridge &sink,
               uint64_t feed_chip, uint64_t feed_port, uint64_t sink_chip,
               uint64_t sink_port, Core &watch, uint64_t watch_physical,
               uint64_t cycle_limit)
      : BachModule(c, "exact"), chips(std::move(target)),
        links(std::move(pairs)), in_stub(feed), out_stub(sink),
        in_chip(feed_chip), in_port(feed_port), out_chip(sink_chip),
        out_port(sink_port), watch_core(watch), watch_chip(sink_chip),
        watch_phys(watch_physical), limit(cycle_limit) {}

  std::vector<MessagePtr> queue;
  RunResult result;
  // 观察点：chip、物理 core。空则只看 watch_core。
  std::vector<std::tuple<uint64_t, uint64_t, Core *>> watches;
  // 落地后要比的位置。空则不宣称收到了字节。
  std::vector<StoreView> stores;
  // 每个用户一份保存的 6392 B，与 queue 对齐。落地探针用它，不用线上副本。
  std::vector<std::vector<uint8_t>> saved;
  uint64_t expected = 0;
  std::set<uint64_t> requested_users;
  std::function<void(uint64_t)> observe;
  uint64_t output_addr = ~uint64_t{0};
  uint64_t output_stride = kInputStride;
  uint64_t output_bytes = 0;
  bool halted = false;

protected:
  void Step() override {
    if (halted) return;
    if (ReadRelease(in_stub.RcVaSa().ToCoreBack()->release).vc_valid)
      ++feed_room;

    for (auto const &w : WatchList()) {
      Core *core = std::get<2>(w);
      auto snap = core->GetTs().Table().SnapPtr()->Get();
      if (!snap)
        continue;
      uint64_t chip = std::get<0>(w), physical = std::get<1>(w);
      NoteLive(chip, physical, *snap, CycleNow(), &held);
      LatchDone(chip, physical, *snap, core->GetTs().Cfg().ThroughEndMask(),
                CycleNow(), &held);
      if (chip == watch_chip && physical == watch_phys && output_bytes) {
        for (uint64_t sid = 0; sid < snap->slot_num; ++sid) {
          auto const &entry = snap->entry[sid];
          if (!requested_users.count(entry.user_id) ||
              !EndCovered(entry, core->GetTs().Cfg().ThroughEndMask()))
            continue;
          auto found = result.stored_outputs.find(entry.user_id);
          if (found != result.stored_outputs.end()) {
            LOGCHECK(found->second.owner.sid == sid &&
                         found->second.owner.allocation == entry.allocation,
                     "同一用户不能由两个 allocation 输出。");
            continue;
          }
          Capture owner = held.at(SlotKey{chip, physical, sid, entry.allocation});
          result.stored_outputs.emplace(entry.user_id, StoredOutput{
              owner, core->Cmem().Peek(output_addr + sid * output_stride, output_bytes)});
        }
      }
    }

    LinkEndPtr feed_in = in_stub.FromCore();
    if (input_packets.empty() && !queue.empty() && CycleNow() >= 2) {
      input_packets.push_back(queue.front());
      queue.erase(queue.begin());
    }
    if (!input_packets.empty() && feed_room) {
      auto packet = input_packets.front();
      uint64_t total = packet->TotalBytes();
      uint64_t bytes = std::min<uint64_t>(kFlitBytesLocal, total - input_offset);
      bool last = input_offset + bytes == total;
      bool head = input_offset == 0;
      feed_in->flit.Drive(0, head, last, bytes, packet);
      --feed_room;
      input_offset += bytes;
      if (last) {
        input_packets.pop_front();
        input_offset = 0;
      }
    } else {
      feed_in->flit.Idle();
    }
    feed_in->release.Idle();
    out_stub.FromCore()->flit.Idle();
    out_stub.FromCore()->release.Idle();

    out_stub.RunStep();
    for (uint64_t i = chips.size(); i > 0; --i)
      chips[i - 1]->RunOutside();
    in_stub.RunStep();

    Move(in_stub, chips[in_chip]->Port(in_port));
    for (ChipPair const &p : links)
      Move(chips[p.a]->Port(p.a_port), chips[p.b]->Port(p.b_port));
    Move(chips[out_chip]->Port(out_port), out_stub);

    FlitView got = ReadFlit(out_stub.ToCore()->flit);
    if (got.valid && got.msg && got.tail) {
      result.out_msgs.push_back(got.msg);
      result.out_at.push_back(CycleNow());
    }

    if (observe) observe(CycleNow());
    bool all_done = !queue.empty() || !input_packets.empty() ? false : EndsMet();
    bool have_outputs = output_bytes ? result.stored_outputs.size() == expected
                                     : result.out_msgs.size() >= expected;
    if (halted || (all_done && have_outputs) || CycleNow() >= limit) {
      result.stopped_at = CycleNow();
      result.completed_by_end = all_done;
      result.still_occupied_at_end = AnyOccupied();
      result.expected = expected;
      result.dte_error = watch_core.GetDte().Completion().Error();
      StoreProbe store;
      if (!stores.empty() && saved.size() == expected && expected != 0) {
        for (uint64_t i = 0; i < expected && i < saved.size(); ++i) {
          for (StoreView const &view : stores)
            store.Read(view, saved[i]);
        }
        result.store_checked = true;
      }
      result.probes_topk = store.topk;
      result.probes_data = store.data;
      result.probes_scale = store.scale;
      result.probes_header = store.header;
      result.probes_mismatch = store.mismatch;
      result.probes_users = store.users;
      result.probe_note = store.note;
      for (auto const &one : held)
        result.captures.push_back(one.second);
      halted = true;
      clk->Stop();
    }
  }

private:
  std::vector<std::tuple<uint64_t, uint64_t, Core *>> WatchList() {
    if (!watches.empty())
      return watches;
    return {{watch_chip, watch_phys, &watch_core}};
  }

  bool EndsMet() {
    if (expected == 0 || held.empty())
      return false;
    std::set<uint64_t> completed;
    for (auto const &one : held) {
      Capture const &c = one.second;
      if (c.chip == watch_chip && c.physical == watch_phys &&
          c.completed && c.user_stamped && requested_users.count(c.user_id))
        completed.insert(c.user_id);
    }
    return requested_users.size() == expected && completed == requested_users;
  }

  bool AnyOccupied() {
    for (auto const &w : WatchList()) {
      if (std::get<2>(w)->GetTs().Table().InFlight() != 0)
        return true;
    }
    return false;
  }

  static void Move(C2cBridge &x, C2cBridge &y) {
    while (x.HasOut())
      y.PushIn(x.TakeOut());
    while (y.HasOut())
      x.PushIn(y.TakeOut());
  }

  std::vector<Chip *> chips;
  std::vector<ChipPair> links;
  C2cBridge &in_stub;
  C2cBridge &out_stub;
  uint64_t in_chip, in_port, out_chip, out_port;
  Core &watch_core;
  uint64_t watch_chip = 0;
  uint64_t watch_phys = 0;
  uint64_t limit;
  std::deque<MessagePtr> input_packets;
  uint64_t input_offset = 0;
  uint64_t feed_room = kC2cInCap;
  std::map<SlotKey, Capture> held;
};

// 写成文本记录，不引 JSON 库。一行一条，键与值用空格分开。
inline void WriteRecords(std::string const &path, RunResult const &r,
                         std::vector<uint64_t> const &users) {
  std::ofstream o(path);
  LOGCHECK(o.good(), "写不出记录文件。");
  o << "stopped_at " << r.stopped_at << "\n";
  o << "completed_by_end " << (r.completed_by_end ? 1 : 0) << "\n";
  o << "still_occupied " << (r.still_occupied_at_end ? 1 : 0) << "\n";
  o << "expected " << r.expected << "\n";
  o << "store_checked " << (r.store_checked ? 1 : 0) << "\n";
  o << "probe_users " << r.probes_users << "\n";
  o << "probe_topk " << r.probes_topk << "\n";
  o << "probe_data " << r.probes_data << "\n";
  o << "probe_scale " << r.probes_scale << "\n";
  o << "probe_header " << r.probes_header << "\n";
  o << "probe_mismatch " << r.probes_mismatch << "\n";
  o << "dte_error " << r.dte_error << "\n";
  if (!r.probe_note.empty())
    o << "probe_note " << r.probe_note << "\n";
  o << "stored_output_count " << r.stored_outputs.size() << "\n";
  for (auto const &entry : r.stored_outputs) {
    auto const &value = entry.second;
    o << "stored_output user " << entry.first << " sid " << value.owner.sid
      << " allocation " << value.owner.allocation << " bytes " << value.bytes.size()
      << " completed_cycle " << value.owner.completed_cycle << "\n";
  }
  o << "out_count " << r.out_msgs.size() << "\n";
  for (uint64_t i = 0; i < r.out_msgs.size(); ++i) {
    Message const &m = *r.out_msgs[i];
    o << "out " << i << " user " << m.user_id << " bytes " << m.payload.size()
      << " at " << r.out_at[i] << "\n";
  }
  for (Capture const &c : r.captures) {
    o << "capture chip " << c.chip << " phys " << c.physical << " user "
      << c.user_id << " sid " << c.sid << " allocation " << c.allocation
      << " cycle " << c.cycle << " completed " << (c.completed ? 1 : 0)
      << " completed_cycle " << c.completed_cycle << "\n";
  }
  for (uint64_t u : users)
    o << "user " << u << "\n";
}

// BF16 原样按实际用户命名落盘。重复用户不能覆盖上一份输出。
inline void WriteBf16(std::string const &dir, RunResult const &r) {
  if (!r.stored_outputs.empty()) {
    for (auto const &entry : r.stored_outputs) {
      auto const &bytes = entry.second.bytes;
      std::ofstream out(dir + "/user" + std::to_string(entry.first) + ".bf16",
                        std::ios::binary);
      LOGCHECK(out.good(), "写不出锁存的 BF16 文件。");
      out.write(reinterpret_cast<char const *>(bytes.data()), std::streamsize(bytes.size()));
    }
    return;
  }
  std::set<uint64_t> written;
  for (uint64_t i = 0; i < r.out_msgs.size(); ++i) {
    Message const &m = *r.out_msgs[i];
    LOGCHECK(written.insert(m.user_id).second, "重复用户输出不能覆盖。");
    std::ostringstream name;
    name << dir << "/user" << m.user_id << ".bf16";
    std::ofstream o(name.str(), std::ios::binary);
    LOGCHECK(o.good(), "写不出 BF16 文件。");
    o.write(reinterpret_cast<char const *>(m.payload.data()),
            std::streamsize(m.payload.size()));
  }
}

} // namespace ep1exact
} // namespace bach
} // namespace latch

#endif
