#ifndef _LATCH_BACH_IP_LPU_
#define _LATCH_BACH_IP_LPU_

// LPU：48 颗 chip 装模型的一层 MoE，也就是当前部署下的一个机柜。
//
// 自己不打拍，只做构造与接线五件事：
//   1. 按每颗 chip 的列位置定形状，构造 48 个 Chip
//   2. 按 12 × 4 网格接 chip 之间的 C2C：同层左右直连、同列上下直连，都不经
//      PCIe Switch
//   3. 每层最左最右两颗 chip 的边缘口接本 tray 的 PCIe Switch
//   4. 片外桩挂到 PCIe Switch 上
//   5. 读入编译侧给的坐标换算表与逻辑 core 映射，做跨表的自洽检查
//
// 链路的时间落在哪：chip 与 chip 之间没有 Link 实例 —— 两座桥之间传的是段，
// Link 搬的是 flit，段号只在一对桥之间有意义。这一段的带宽与延迟落在发送侧那
// 座桥的 AXI 段上，跨 tray 的两处在那里换成纵向参数。chip 与 PCIe Switch 之间
// 传的是 flit，那一段由一对 Link 实例承担，两侧桥的 AXI 段因此计 0。

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/log.h"
#include "bach/common/params.h"
#include "bach/ip/chip/chip.h"
#include "bach/ip/external/in_stub.h"
#include "bach/ip/external/out_stub.h"
#include "bach/ip/link/link.h"
#include "bach/ip/lpu_grid.h"
#include "bach/ip/pcie_switch.h"
#include "bach/ip/wiring.h"

namespace latch {
namespace bach {

// ── chip 的边缘口与 PCIe Switch 之间 ──
//
// 这个口的对面没有另一座桥。桥收发的是段：一个 flit 在 TX 那边发两拍、在 RX
// 那边收两拍合一，段号只在一对桥之间有意义。Switch 收发的是 flit，所以这一侧
// 由装配层折算 —— 出去的两拍合成一个 flit，进来的一个 flit 摊成两拍，段号由
// 本侧自己给。
class SwitchPort {
 public:
  SwitchPort(C2cBridge& bridge, LinkEndPtr out_wire, LinkEndPtr in_wire)
      : br(&bridge), to_sw(std::move(out_wire)), from_sw(std::move(in_wire)) {}

  void Move() {
    Collect();
    Drive();
    Receive();
  }

  uint64_t OutFlits() const { return out_cnt; }
  uint64_t InFlits() const { return in_cnt; }

 private:
  // 出方向：一段两拍，第二拍才算凑齐。release 不拆，来一笔发一笔。
  void Collect() {
    while (br->HasOut()) {
      C2cBeat b = br->TakeOut();
      if (b.is_release) {
        rel_q.push_back(b.rel);
        continue;
      }
      if (!half) {
        half = true;
        continue;
      }
      half = false;
      flit_q.push_back(b);
    }
  }

  // 每拍在 Drive 与 Idle 里二选一调一次：一拍都不写，下一拍读到的还是这一拍
  // 的值，同一个 flit 就发了两遍。
  void Drive() {
    if (flit_q.empty()) {
      to_sw->flit.Idle();
    } else {
      C2cBeat const& b = flit_q.front();
      to_sw->flit.Drive(b.seg.vc, /*is_head=*/true, b.seg.tail, b.seg.bytes,
                        b.seg.msg);
      flit_q.pop_front();
      ++out_cnt;
    }
    if (rel_q.empty()) {
      to_sw->release.Idle();
    } else {
      ReleaseView const& r = rel_q.front();
      to_sw->release.Drive(r.vc_valid, r.vc_id, r.stream_valid, r.stream_user,
                           r.reduce_valid, r.reduce_user);
      rel_q.pop_front();
    }
  }

  // 入方向：一个 flit 摊成两拍，两拍同一个段号，桥的 RX 按段号把它们合回去。
  void Receive() {
    FlitView f = ReadFlit(from_sw->flit);
    if (f.valid) {
      C2cBeat b;
      b.seg.vc = f.vc;
      b.seg.seq_id = seq;
      b.seg.tail = f.tail;
      b.seg.bytes = f.bytes;
      b.seg.msg = f.msg;
      seq = (seq + 1) % kC2cSeqNum;
      br->PushIn(b);
      br->PushIn(b);
      ++in_cnt;
    }
    ReleaseView r = ReadRelease(from_sw->release);
    if (r.Any()) {
      C2cBeat b;
      b.is_release = true;
      b.rel = r;
      br->PushIn(b);
    }
  }

  C2cBridge* br;
  LinkEndPtr to_sw, from_sw;
  std::deque<C2cBeat> flit_q;
  std::deque<ReleaseView> rel_q;
  bool half = false;
  uint64_t seq = 0, out_cnt = 0, in_cnt = 0;
};

// 一条 chip 到 chip 的 C2C，两端各是一颗 chip 的一个口。
struct C2cPair {
  uint64_t a_chip = 0, a_port = 0;
  uint64_t b_chip = 0, b_port = 0;
  LinkParams params;
};

// 一条 chip 边缘口到 PCIe Switch 的链路。
struct EdgePair {
  uint64_t chip = 0, chip_port = 0;
  uint64_t sw = 0, sw_port = 0;
  LinkParams params;
};

// 哪几颗 chip 真的构造出来。
//
// 一颗 chip 是 8 或 10 个 core、每个 core 上百个模块，48 颗全建起来逐拍推进要
// 几十毫秒一拍，走完一层的几万拍跑不出结果。一轮只用到其中几颗时，其余的不
// 构造：坐标、形状、角色、链路参数仍按 12 × 4 的整机算，没建的那几颗身上的
// 链路不登记，Switch 与两个片外桩照旧全建。
struct LpuCfg {
  std::array<bool, kChipNum> build;
  // 每个 core、每颗 chip 各占一个常驻协程。开着才有并行，槽位（sub_thread ×
  // co_thread）要由建时钟那一侧按数量反推。
  bool core_tick = false;
  bool chip_tick = false;

  LpuCfg() { build.fill(true); }
};

// 只建这几颗，坐标按整机算。
inline LpuCfg LpuBuildOnly(std::vector<uint64_t> const& want) {
  LpuCfg c;
  c.build.fill(false);
  for (uint64_t i : want) {
    LOGCHECK(i < kChipNum, "LpuBuildOnly: chip 号越界。");
    c.build[i] = true;
  }
  return c;
}

class Lpu {
 public:
  Lpu(ClockPtr clock, const std::string& name, LpuTables const& tables,
      LpuCfg const& setting = LpuCfg())
      : clk(clock), tbl(tables), cfg(setting) {
    CheckLpuTables(tbl);
    Build(name);
    WireRow();
    WireCol();
    WireEdge(name);
    WireExt(name);
    Route();
  }

  // ── 观测 ──
  Chip& GetChip(uint64_t gx, uint64_t gy) { return ChipAt(ChipIdOf(gx, gy)); }
  Chip& ChipAt(uint64_t chip) {
    LOGCHECK(chips.at(chip) != nullptr, "Lpu: 这一颗 chip 没有构造。");
    return *chips[chip];
  }
  bool HasChip(uint64_t chip) const { return chips.at(chip) != nullptr; }
  PcieSwitch& Switch(uint64_t i) { return *switches.at(i); }
  InStub& In() { return *in_stub; }
  OutStub& Out() { return *out_stub; }
  LpuTables const& Tables() const { return tbl; }

  std::vector<C2cPair> const& C2cLinks() const { return c2c; }
  std::vector<EdgePair> const& EdgeLinks() const { return edge; }
  std::vector<EdgePair> const& ExtLinks() const { return ext; }

  uint64_t ChipCount() const {
    uint64_t n = 0;
    for (auto const& c : chips) {
      if (c) ++n;
    }
    return n;
  }
  uint64_t CoreCount() const {
    uint64_t n = 0;
    for (auto const& c : chips) {
      if (c) n += c->CoreNum();
    }
    return n;
  }
  uint64_t ComputeCoreCount() const {
    uint64_t n = 0;
    for (uint64_t i = 0; i < kChipNum; ++i) {
      if (!chips[i]) continue;
      for (uint64_t c = 0; c < CoresOf(tbl.chip_shape[i]); ++c) {
        if (tbl.logical_map[i][c].role == CoreRole::kCompute) ++n;
      }
    }
    return n;
  }

  // 末级先做：链路与 Switch 在最外，chip 在里面，跨模块的搬运放在最后。
  void RunStep() {
    for (auto& l : links) l->RunStep();
    for (auto& s : switches) s->RunStep();
    in_stub->RunStep();
    out_stub->RunStep();
    for (auto& c : chips) {
      if (c) c->RunOutside();
    }

    for (C2cPair const& p : c2c) {
      C2cBridge& a = chips[p.a_chip]->Port(p.a_port);
      C2cBridge& b = chips[p.b_chip]->Port(p.b_port);
      while (a.HasOut()) b.PushIn(a.TakeOut());
      while (b.HasOut()) a.PushIn(b.TakeOut());
    }
    for (auto& p : sw_ports) p->Move();
  }

  bool Quiescent() const {
    for (auto const& c : chips) {
      if (c && !c->Quiescent()) return false;
    }
    for (auto const& s : switches) {
      if (!s->Quiescent()) return false;
    }
    for (auto const& l : links) {
      if (!l->Quiescent()) return false;
    }
    return true;
  }

 private:
  // ── L1 第 1 条：按列位置定形状，构造 48 个 Chip ──
  //
  // 每个口的参数在这里定死：同层左右与 tray 内上下用 PCIe C2C，跨 tray 的两处
  // 换纵向参数，接 Switch 的那个口计 0（那一段由 Link 实例承担）。
  void Build(const std::string& name) {
    for (uint64_t gy = 0; gy < kGridY; ++gy) {
      for (uint64_t gx = 0; gx < kGridX; ++gx) {
        uint64_t i = ChipIdOf(gx, gy);
        ChipCfg c;
        c.shape = tbl.chip_shape[i];
        c.gx = gx;
        c.gy = gy;
        c.port[kChipE].axi_latency =
            gx + 1 < kGridX ? LinkPcieC2C().latency : 0;
        c.port[kChipW].axi_latency = gx > 0 ? LinkPcieC2C().latency : 0;
        c.port[kChipS].axi_latency = VertParams(gy).latency;
        c.port[kChipN].axi_latency = gy > 0 ? VertParams(gy - 1).latency : 0;
        c.core_tick = cfg.core_tick;
        c.chip_tick = cfg.chip_tick;
        if (!cfg.build[i]) continue;
        chips[i] = std::make_unique<Chip>(
            clk, name + ".chip" + std::to_string(i), c);
      }
    }
  }

  // 同列上下那一段的参数：跨 tray 的两处换纵向链路参数。
  static LinkParams VertParams(uint64_t gy) {
    if (gy + 1 >= kGridY) return LinkParams{0, 0, 0};
    return CrossTray(gy) ? LinkTrayVertical() : LinkPcieC2C();
  }

  // ── L1 第 2 条：同层左右直连 ──
  void WireRow() {
    for (uint64_t gy = 0; gy < kGridY; ++gy) {
      for (uint64_t gx = 0; gx + 1 < kGridX; ++gx) {
        C2cPair p;
        p.a_chip = ChipIdOf(gx, gy);
        p.a_port = kChipE;
        p.b_chip = ChipIdOf(gx + 1, gy);
        p.b_port = kChipW;
        p.params = LinkPcieC2C();
        if (chips[p.a_chip] && chips[p.b_chip]) c2c.push_back(p);
      }
    }
  }

  // ── L1 第 3 条：同列上下直连，跨 tray 换参数 ──
  void WireCol() {
    for (uint64_t gy = 0; gy + 1 < kGridY; ++gy) {
      for (uint64_t gx = 0; gx < kGridX; ++gx) {
        C2cPair p;
        p.a_chip = ChipIdOf(gx, gy);
        p.a_port = kChipS;
        p.b_chip = ChipIdOf(gx, gy + 1);
        p.b_port = kChipN;
        p.params = VertParams(gy);
        if (chips[p.a_chip] && chips[p.b_chip]) c2c.push_back(p);
      }
    }
  }

  // ── L1 第 4 条：每层最左最右两颗 chip 的边缘口接本 tray 的 PCIe Switch ──
  void WireEdge(const std::string& name) {
    for (uint64_t s = 0; s < kSwitchNum; ++s) {
      switches.push_back(std::make_unique<PcieSwitch>(
          clk, name + ".sw" + std::to_string(s), kSwitchPortNum, 0, false));
    }
    for (uint64_t gy = 0; gy < kGridY; ++gy) {
      for (uint64_t gx : {uint64_t(0), kGridX - 1}) {
        EdgePair e;
        e.chip = ChipIdOf(gx, gy);
        e.chip_port = gx == 0 ? kChipW : kChipE;
        e.sw = SwitchOfEdge(gx, gy);
        e.sw_port = SwitchPortOfEdge(gy);
        e.params = LinkPcieRouterLr();
        if (!chips[e.chip]) continue;
        edge.push_back(e);
        MakeSwitchPort(name + ".edge" + std::to_string(e.chip), e);
      }
    }
  }

  // ── L1 第 5 条：入口桩与出口桩各挂一个 Switch 端口，走 ETH 参数的 Link ──
  //
  // 外部数据从 global_top_left 西侧进，结果从 global_bottom_right 东侧出，
  // 所以两个桩挂的是那两颗 chip 的边缘口所在的那两个 Switch。
  void WireExt(const std::string& name) {
    in_stub = std::make_unique<InStub>(clk, name + ".in_stub", kInPoolTokens, 0,
                                       false);
    out_stub = std::make_unique<OutStub>(clk, name + ".out_stub", 0, false);

    EntryExit const& ee = tbl.entry_exit;
    EdgePair in_e;
    in_e.chip = ChipIdOf(ee.in_gx, ee.in_gy);
    in_e.chip_port = kChipW;
    in_e.sw = SwitchOfEdge(ee.in_gx, ee.in_gy);
    in_e.sw_port = kSwitchExtPort;
    in_e.params = LinkEthIn();
    ext.push_back(in_e);
    // 入口桩发出去，进 Switch 的第三个端口。
    MakeLink(name + ".eth_in", LinkEthIn(), in_stub->TxPtr(),
             switches[in_e.sw]->InPtr(kSwitchExtPort));

    EdgePair out_e;
    out_e.chip = ChipIdOf(ee.out_gx, ee.out_gy);
    out_e.chip_port = kChipE;
    out_e.sw = SwitchOfEdge(ee.out_gx, ee.out_gy);
    out_e.sw_port = kSwitchExtPort;
    out_e.params = LinkEthIn();
    ext.push_back(out_e);
    // Switch 的第三个端口出去，进出口桩。
    MakeLink(name + ".eth_out", LinkEthIn(),
             switches[out_e.sw]->OutPtr(kSwitchExtPort), out_stub->RxPtr());
  }

  // ── L2 第 4 条：Switch 按包头的 dst 查目的端口 ──
  //
  // 阵列内部靠 path_id 走，dst 只在片外那一段有意义：0～47 是 chip，48 是出口
  // 桩。一个 Switch 只认它自己接的那几个目的，Switch 之间不互联。
  void Route() {
    for (EdgePair const& e : edge) {
      switches[e.sw]->SetRoute(e.chip, {e.sw_port});
    }
    // 结果只从 global_bottom_right 那一侧出去，所以只有出口桩挂着的那个
    // Switch 认得出口桩这个目的。
    EntryExit const& ee = tbl.entry_exit;
    uint64_t out_sw = SwitchOfEdge(ee.out_gx, ee.out_gy);
    switches[out_sw]->SetRoute(kNodeOutStub, {kSwitchExtPort});
  }

  // chip 的边缘口与 Switch 之间：一对 Link 加一个折算段与 flit 的口。
  void MakeSwitchPort(const std::string& name, EdgePair const& e) {
    LinkEndPtr to_sw = MakeWire(clk);
    LinkEndPtr from_sw = MakeWire(clk);
    MakeLink(name + ".up", e.params, to_sw, switches[e.sw]->InPtr(e.sw_port));
    MakeLink(name + ".down", e.params, switches[e.sw]->OutPtr(e.sw_port),
             from_sw);
    sw_ports.push_back(std::make_unique<SwitchPort>(
        chips[e.chip]->Port(e.chip_port), to_sw, from_sw));
  }

  void MakeLink(const std::string& name, LinkParams const& params,
                LinkEndPtr from, LinkEndPtr to) {
    auto l = std::make_unique<Link>(clk, name, params, 0, false);
    l->AttachIn(std::move(from));
    l->AttachOut(std::move(to));
    links.push_back(std::move(l));
  }

  // 入口桩那一层全局池的大小，待定，先按每个 GPU 一批取。
  static constexpr uint64_t kInPoolTokens = 64;

  ClockPtr clk;
  LpuTables tbl;
  LpuCfg cfg;
  std::array<std::unique_ptr<Chip>, kChipNum> chips;
  std::vector<std::unique_ptr<PcieSwitch>> switches;
  std::vector<std::unique_ptr<Link>> links;
  std::vector<std::unique_ptr<SwitchPort>> sw_ports;
  std::unique_ptr<InStub> in_stub;
  std::unique_ptr<OutStub> out_stub;
  std::vector<C2cPair> c2c;
  std::vector<EdgePair> edge, ext;
};

}  // namespace bach
}  // namespace latch

#endif
