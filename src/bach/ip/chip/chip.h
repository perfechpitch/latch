#ifndef _LATCH_BACH_IP_CHIP_CHIP_
#define _LATCH_BACH_IP_CHIP_CHIP_

// Chip：一片里 2×5 共 10 个 core 的阵列，加四座 C2C Bridge、每 core 一个 ctrl_noc
// 端点与一个 SCP 桩。
//
// 自己不打拍：这一层的逐拍行为在 SCP 桩、ctrl_noc 端点与四座 Bridge 里，core 内
// 的在各单元的模块里。
//
// 10 个 core 按 row-major 编号：
//   0 1 2 3 4
//   5 6 7 8 9
// 构造时不区分 chip 所在列，也不区分角色。坏 core 由 core_bad_mask 标出，业务开
// 始之前写进每个 core 的 Router；角色由编译器写进各 core 的配置。
//
// 接线三条：
//   同行相邻 core 的 left 与 right 用一对 Link 对接
//   core[i] 与 core[i+5] 的 mid 口直连
//   每行左右两端的 core 各接一座 C2C Bridge：core0 引到 N 口、core4 引到 E 口、
//   core5 引到 W 口、core9 引到 S 口

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/ip/chip/c2c_bridge.h"
#include "bach/ip/chip/chip_ports.h"
#include "bach/ip/chip/core/core.h"
#include "bach/ip/chip/ctrl_noc.h"
#include "bach/ip/chip/scp.h"
#include "bach/ip/link/link.h"

namespace latch {
namespace bach {

// 一颗 chip 2 行 × 5 列共 10 个 core。
constexpr uint64_t kChipCols = 5;
constexpr uint64_t kChipCoreNum = 2 * kChipCols;

// chip 的四个对外口。
enum ChipPort : uint64_t {
  kChipN = 0,
  kChipE = 1,
  kChipW = 2,
  kChipS = 3,
  kChipPortNum = 4,
};

struct ChipCfg {
  uint64_t gx = 0, gy = 0;   // 本 chip 在 LPU 里的坐标
  // 四个 C2C 口各一份参数。同层左右与同列上下用 C2C，跨 tray 那两处换纵向
  // 参数，接 PCIe Switch 的那个口用 PCIe ↔ Router，都由 LPU 按链路表填。
  std::array<C2cCfg, kChipPortNum> port{};
  // core 各自挂时钟（各占一个常驻协程）。core 之间只经打拍的 LinkEnd 通讯，
  // 所以与 chip 顺序调 RunStep() 逐拍结果相同，区别只在能不能并行。
  bool core_tick = false;
  // 本 chip 的片内链路、ctrl_noc 端点与 SCP 也各占一个协程。四座 C2C 桥不在
  // 里面：两片对接是一侧的 TakeOut() 直接喂另一侧的 PushIn()，那一对方法改的
  // 是普通容器，桥与做对接的那一方必须在同一个协程里。
  bool chip_tick = false;
};

class Chip : public BachModule {
 public:
  Chip(ClockPtr clock, const std::string& name, ChipCfg const& setting)
      : BachModule(clock, name, 0, setting.chip_tick), cfg(setting),
        cols(kChipCols) {
    uint64_t n = kChipCoreNum;
    for (uint64_t i = 0; i < n; ++i) {
      CoreContext ctx;
      ctx.core_id = i;
      ctx.gx = cfg.gx;
      ctx.gy = cfg.gy;
      ctx.tick = cfg.core_tick;
      cores.push_back(std::make_unique<Core>(
          clock, "core" + std::to_string(i), ctx, Id()));
      // Ctrl-NOC 端点上跑的是 boot 期的配置写，不是业务数据，不记波形。
      {
        TraceOffScope off;
        noc.push_back(std::make_unique<CtrlNocEndpoint>(
            clock, "noc" + std::to_string(i), i, Id(), false));
      }
    }
    scp = std::make_unique<ScpStub>(clock, "scp", n, Id(), false);
    for (uint64_t i = 0; i < n; ++i) noc[i]->AttachCtrl(scp->CtrlPtr());

    BuildLinks();
    BuildBridges();
  }

  // ── 对外：四个 C2C 口 ──
  //
  // 两片对接就是把一侧的 TakeOut() 喂给另一侧的 PushIn()。C2C 上传的是段与
  // release，不是 flit。位宽转换与拆包合包在桥里做完了。
  C2cBridge& Port(uint64_t d) { return *bridges.at(d); }

  ScpStub& Scp() { return *scp; }
  Core& GetCore(uint64_t i) { return *cores.at(i); }
  CtrlNocEndpoint& Noc(uint64_t i) { return *noc.at(i); }
  uint64_t CoreNum() const { return cores.size(); }
  uint64_t Gx() const { return cfg.gx; }
  uint64_t Gy() const { return cfg.gy; }
  // core_bad_mask 写进每个 core 的 Router，与 SCP 在 Router 配置阶段逐 core 写同
  // 一个值等价。业务开始之前写完。
  void SetCoreBadMask(uint64_t mask) {
    for (auto& c : cores) c->SetCoreBadMask(mask);
  }
  uint64_t CoreBadMask() { return cores[0]->GetRouter().Table().CoreBadMask(); }

  // 装配层每拍调这个。四座桥永远在调用方那个协程里跑，因为两片对接靠的是
  // TakeOut() 与 PushIn() 这一对直接改容器的方法。其余按 chip_tick：不挂时钟
  // 时就在这里接着跑，挂了就由本 chip 自己那个协程跑。
  void RunOutside() {
    for (auto& b : bridges) b->RunStep();
    if (!cfg.chip_tick) Step();
  }

  // 末级先做：链路在最外，SCP 在最里。
  void Step() override {
    for (auto& l : links) l->RunStep();
    // core 自己挂时钟时不在这里推：它在自己那个协程里跑，读它的 Ready() 就是
    // 跨协程读普通成员。boot 期那两条通路（Deliver 与 SetCoreReady）只在
    // 由本层顺序驱动时走。
    if (cfg.core_tick) {
      scp->RunStep();
      return;
    }
    for (auto& c : cores) c->RunStep();
    for (uint64_t i = 0; i < noc.size(); ++i) {
      noc[i]->RunStep();
      Deliver(i);
      scp->SetCoreReady(i, cores[i]->Ready());
    }
    scp->RunStep();
  }

  // 这个数要在停钟之后取：它翻的是各 core 内部的普通容器，core 各占协程时在
  // 协程外读就是跨协程读非 atomic 的量。
  bool Quiescent() const override {
    for (auto const& c : cores) {
      if (!c->Quiescent()) return false;
    }
    for (auto const& b : bridges) {
      if (!b->Quiescent()) return false;
    }
    return scp->Quiescent();
  }

 private:
  // 同行相邻 core 的 left 与 right 用一对 Link 对接；core[i] 与 core[i+5] 的
  // mid 口直连。左右那一档担 10 T 的走线，mid 两端上下相对、没有走线段。
  void BuildLinks() {
    for (uint64_t row = 0; row < 2; ++row) {
      for (uint64_t c = 0; c + 1 < cols; ++c) {
        uint64_t a = row * cols + c;
        uint64_t b = a + 1;
        Connect("lr" + std::to_string(a), a, uint64_t(Dir::kDirRight), b,
                uint64_t(Dir::kDirLeft), LinkR2R());
      }
    }
    for (uint64_t c = 0; c < cols; ++c) {
      Connect("mid" + std::to_string(c), c, uint64_t(Dir::kDirMid),
              c + cols, uint64_t(Dir::kDirMid), LinkR2RMid());
    }
  }

  // 一对 Link：a 的出口到 b 的入口，b 的出口到 a 的入口。credit 的回程各走
  // 各的实例，与数据同参数。
  void Connect(const std::string& name, uint64_t a, uint64_t da, uint64_t b,
               uint64_t db, LinkParams const& params) {
    // 片内这几十条链路不记波形：一条链路上走了多少、堵没堵，在它两端那两个
    // RouterStation 的 forwarded 与 occupancy 上都读得出来，一颗 chip 五十多条
    // 链路各记三个信号，翻起来全是重复的。
    TraceOffScope off;
    const uint64_t gid = TraceGroup(name, Id());
    MakeLink("fwd", gid, params, cores[a]->OutWire(da), cores[b]->InWire(db));
    MakeLink("rev", gid, params, cores[b]->OutWire(db), cores[a]->InWire(da));
    // VC credit 的回程。stream release 那两根还没接：收它的那一级（把本级
    // core 与各下级出口的 release 汇总再发往另两个 R2R 口的那个 crossbar）还
    // 没建，接到 back_wire 上就成了两根线写同一个端口，VC credit 会被盖掉。
    MakeLink("back_a", gid, params, cores[b]->UpBackWire(db),
             cores[a]->BackWire(da));
    MakeLink("back_b", gid, params, cores[a]->UpBackWire(da),
             cores[b]->BackWire(db));
  }

  void MakeLink(const std::string& name, uint64_t parent,
                LinkParams const& params, LinkEndPtr from, LinkEndPtr to) {
    auto l = std::make_unique<Link>(clk, name, params, parent, false);
    l->AttachIn(std::move(from));
    l->AttachOut(std::move(to));
    links.push_back(std::move(l));
  }

  // 每行左右两端的 core 各接一座 Bridge：core0 → N，core4 → E，core5 → W，
  // core9 → S。
  void BuildBridges() {
    static const char* kNames[kChipPortNum] = {"n", "e", "w", "s"};
    uint64_t edge[kChipPortNum] = {0, cols - 1, cols, 2 * cols - 1};
    uint64_t dir[kChipPortNum] = {
        uint64_t(Dir::kDirLeft), uint64_t(Dir::kDirRight),
        uint64_t(Dir::kDirLeft), uint64_t(Dir::kDirRight)};
    for (uint64_t d = 0; d < kChipPortNum; ++d) {
      bridges.push_back(std::make_unique<C2cBridge>(
          clk, std::string("c2c_") + kNames[d], cfg.port[d], Id()));
      uint64_t i = edge[d];
      // 边界 core 那一侧的 R2R 口没有相邻 core，接到桥上。
      bridges[d]->AttachFromCore(cores[i]->OutWire(dir[d]));
      bridges[d]->AttachToCore(cores[i]->InWire(dir[d]));
      // 桥收下一个 flit 就把 VC 位置还给这个 core：边界那一跳的反压与 core 之
      // 间那几跳同一套账，不还就是只减不加。
      bridges[d]->AttachToCoreBack(cores[i]->BackWire(dir[d]));
    }
  }

  // 把 ctrl_noc 端点这一拍锁存的那笔事务交给目的模块。
  //
  // 走方法调用而不是端口：端点与目的模块都在装配这一个协程里，一拍里的次序由
  // RunStep 的调用顺序定死。
  void Deliver(uint64_t i) {
    CfgEvent const& e = noc[i]->Event();
    if (!e.valid) return;
    cores[i]->CfgWrite(e.route, e.we, e.wdata);
  }

  ChipCfg cfg;
  uint64_t cols;
  std::vector<std::unique_ptr<Core>> cores;
  std::vector<std::unique_ptr<CtrlNocEndpoint>> noc;
  std::vector<std::unique_ptr<Link>> links;
  std::vector<std::unique_ptr<C2cBridge>> bridges;
  std::unique_ptr<ScpStub> scp;
};

}  // namespace bach
}  // namespace latch

#endif
