#ifndef _LATCH_BACH_KSIM_SYSTEM_
#define _LATCH_BACH_KSIM_SYSTEM_

// 把核阵列、外围模块与停机判据装在一起。
//
// 每个核是一个挂时钟的节点，各有自己的协程；核之间只通过 Router 的直接调用往来，与
// Bach 里同刻的函数调用一致。片内按行列接线，片间靠四个角上的核接 PCIe。阵列两侧各
// 接一个外围模块的口：最左那一列的左出口收输入，最右那一列的右出口送结果。
//
// 停机由 Control 判：外围模块该喂的喂完了、该收的收齐了、各核的 kernel 也跑完了，才让
// 时钟停下来。给一个上限拍数兜底，免得一份接不上的时序表把仿真挂死。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/log.h"
#include "base/module.h"
#include "base/runtime.h"
#include "bach/ksim/core.h"
#include "bach/ksim/host.h"
#include "bach/ksim/kernel.h"
#include "bach/ksim/topology.h"
#include "bach/observer/span_recorder.h"

namespace latch {
namespace bach {
namespace ksim {

class Array : public Ingress {
 public:
  Array(ClockPtr clock, KernelImage const& image, Topology const& topology,
        RouterParams const& rp, CoreParams const& cp)
      : topo(topology) {
    root = RT::GetModulePool().CreateID("array");
    const uint64_t n = topo.CoreCount();
    cores.reserve(n);
    for (uint64_t i = 0; i < n; ++i) {
      CoreKernel const* prog = image.Find(i);
      cores.push_back(std::make_unique<Core>(
          clock, i, prog, topo, rp, cp, "core" + std::to_string(i), root));
    }
    WireInChip();
    WireBetweenChips();
  }

  // 一份输入从目标核那一行最左边那个 chip 的左出口进阵列，走西边那个 PCIe Switch。
  Router* EntryFor(uint64_t core) override {
    const uint64_t chip = topo.ChipRowOf(core) * topo.chip_cols;
    return &cores[topo.CoreAt(chip, topo.LeftExit())]->Rt();
  }

  // 结果往东边那个 PCIe Switch 出，所以接的是每一行最右边那个 chip 的右出口。进来的
  // 那一头不用接：外围模块是直接往入口 Router 的队列里推，不经这个口。
  void ConnectHost(Host* host) {
    for (uint64_t cr = 0; cr < topo.chip_rows; ++cr) {
      const uint64_t chip = cr * topo.chip_cols + topo.chip_cols - 1;
      Core* c = cores[topo.CoreAt(chip, topo.RightExit())].get();
      c->Rt().ConnectHostPort(host);
    }
    for (auto& c : cores) c->Unit().ConnectCredit(host);
  }

  bool AllDone() const {
    for (auto const& c : cores) {
      Exec& e = const_cast<Core&>(*c).Unit();
      if (e.HasProgram() && !e.Done()) return false;
    }
    return true;
  }

  Core* At(uint64_t core) { return cores.at(core).get(); }
  uint64_t Size() const { return topo.CoreCount(); }
  Topology const& Topo() const { return topo; }

 private:
  void WireInChip() {
    for (uint64_t i = 0; i < topo.CoreCount(); ++i) {
      for (Port p : {Port::kLeft, Port::kRight, Port::kVert}) {
        const int64_t n = topo.NeighborOf(i, p);
        if (n >= 0) {
          cores[i]->Rt().ConnectNeighbor(
              p, &cores[static_cast<uint64_t>(n)]->Rt());
        }
      }
    }
  }

  // 一个 chip 的某个角核，它的 PCIe 口接到邻居 chip 对应的角核上。
  void Link(uint64_t chip, uint64_t local, int64_t drow, int64_t dcol,
            uint64_t peer_local) {
    const int64_t r = static_cast<int64_t>(chip / topo.chip_cols) + drow;
    const int64_t c = static_cast<int64_t>(chip % topo.chip_cols) + dcol;
    if (r < 0 || c < 0 || r >= static_cast<int64_t>(topo.chip_rows) ||
        c >= static_cast<int64_t>(topo.chip_cols)) {
      return;
    }
    const uint64_t peer_chip =
        static_cast<uint64_t>(r) * topo.chip_cols + static_cast<uint64_t>(c);
    cores[topo.CoreAt(chip, local)]->Rt().ConnectNeighbor(
        Port::kPcie, &cores[topo.CoreAt(peer_chip, peer_local)]->Rt());
  }

  void WireBetweenChips() {
    for (uint64_t chip = 0; chip < topo.ChipCount(); ++chip) {
      Link(chip, topo.RightExit(), 0, 1, topo.LeftExit());
      Link(chip, topo.LeftExit(), 0, -1, topo.RightExit());
      Link(chip, topo.BottomExit(), 1, 0, topo.TopExit());
      Link(chip, topo.TopExit(), -1, 0, topo.BottomExit());
    }
  }

  Topology topo;
  uint64_t root = 0;
  std::vector<std::unique_ptr<Core>> cores;
};

// 停机判据。它自己也挂时钟，每拍看一眼够不够收工。
class Control : public ClkModule {
 public:
  Control(ClockPtr clock, Array* array, Host* host, Time limit)
      : ClkModule(clock), clk_ref(clock), arr(array), hst(host), cap(limit) {
    RegisterName("control");
  }

  void Cycle() override {
    DelayCycle(1);
    const Time now = RT::Now();
    if (hst->FedAll() && hst->DrainedAll() && arr->AllDone()) {
      finished = true;
      end_time = now;
      clk_ref->Stop();
      return;
    }
    if (now >= cap) {
      end_time = now;
      clk_ref->Stop();
    }
  }

  bool Finished() const { return finished; }
  Time EndTime() const { return end_time; }

 private:
  ClockPtr clk_ref;
  Array* arr;
  Host* hst;
  Time cap;
  bool finished = false;
  Time end_time = 0;
};

struct RunStat {
  Time end_time = 0;
  bool finished = false;
  uint64_t fed_bytes = 0;
  uint64_t drained_bytes = 0;
  uint64_t expect_bytes = 0;
  Time compute_cycles = 0;
  Time wait_cycles = 0;
  uint64_t sent_bytes = 0;
  uint64_t recv_bytes = 0;
  uint64_t active_cores = 0;
};

}
}
}

#endif
