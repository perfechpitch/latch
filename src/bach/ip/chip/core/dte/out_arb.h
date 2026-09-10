#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_OUT_ARB_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_OUT_ARB_

// 出核通道与 Router 之间那一层。
//
// DTE 对 Router 只有一个 out_core_data_ch，而四个出核通道各自独立打拍、各自发
// 数据。没有这一层的话四个通道会驱动同一根线，后跑的那个把先跑的那一拍盖掉。
//
// 仲裁按轮转。每个通道在入口留几格：通道按序号把一拍放进来，本模块每拍从这几
// 格里取一拍发给 Router。ready 报的是那几格还收不收得下，通道读的是上一拍的
// ready，所以格数比门限多留两格。
//
// 一个包的几拍不能被别的包插进来：Router 那一侧按 thdr 与 tlast 认帧边界。所以
// 授权粘在一个通道上，直到它把带 tlast 的那一拍发完才让给下一个。

#include <array>
#include <deque>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/dte/dte_types.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class DteOutArb : public BachModule {
 public:
  // 四个出核通道，与 Router 的四个 VC 一一对应。
  static constexpr uint64_t kOutNum = kVcNum;

  DteOutArb(ClockPtr clock, const std::string& name, uint64_t parent = 0,
            bool tick = true)
      : BachModule(clock, name, parent, tick),
        out(std::make_shared<CoreDataPort>(clock)),
        sent(clock) {
    for (uint64_t i = 0; i < kOutNum; ++i) {
      side[i] = std::make_shared<CoreDataPort>(clock);
    }
  }

  std::shared_ptr<CoreDataPort> LanePort(uint64_t i) const {
    return side.at(i);
  }
  CoreDataPort& Out() { return *out; }
  void AttachOut(std::shared_ptr<CoreDataPort> p) { out = std::move(p); }

  uint64_t Sent() const { return sent_cnt; }
  bool Quiescent() const override {
    for (auto const& q : slot) {
      if (!q.empty()) return false;
    }
    return !holding;
  }

 protected:
  void Step() override {
    Drain();
    Collect();
    Forward();
    for (uint64_t i = 0; i < kOutNum; ++i) {
      side[i]->DriveReady(slot[i].size() < kSlotMark);
    }
    sent = sent_cnt;
    TracePerCycle("sent", sent_cnt);
  }

 private:
  static constexpr uint64_t kSlotDepth = 4;
  static constexpr uint64_t kSlotMark = 2;

  struct Beat {
    uint64_t bytes = 0;
    bool last = false, hdr = false;
    uint64_t vc = 0;
    MessagePtr msg;
  };

  // 上一拍发出去的被 Router 收下了没有。
  void Drain() {
    if (!holding) return;
    if (!out->Ready()) return;
    holding = false;
    ++sent_cnt;
    // 带 tlast 的那一拍发完才让出授权。
    if (held.last) grant = (grant + 1) % kOutNum;
  }

  void Collect() {
    for (uint64_t i = 0; i < kOutNum; ++i) {
      CoreDataView d = ReadCoreData(*side[i]);
      if (!d.valid || d.seq == seen[i]) continue;
      LOGCHECK(slot[i].size() < kSlotDepth,
               "DteOutArb: 那几格满了。通道应当先看 ready 再发。");
      seen[i] = d.seq;
      slot[i].push_back({d.bytes, d.last, d.hdr, d.vc, d.msg});
    }
  }

  void Forward() {
    if (holding) {
      out->Drive(held.bytes, held.last, held.hdr, held.vc, held.msg);
      return;
    }
    for (uint64_t k = 0; k < kOutNum; ++k) {
      uint64_t i = (grant + k) % kOutNum;
      if (slot[i].empty()) continue;
      // 一个包发到一半时不换通道。
      if (in_frame && i != grant) continue;
      held = slot[i].front();
      slot[i].pop_front();
      grant = i;
      in_frame = !held.last;
      holding = true;
      out->Drive(held.bytes, held.last, held.hdr, held.vc, held.msg);
      return;
    }
    out->Idle();
  }

  std::array<std::shared_ptr<CoreDataPort>, kOutNum> side;
  std::array<std::deque<Beat>, kOutNum> slot;
  std::array<uint64_t, kOutNum> seen{};
  std::shared_ptr<CoreDataPort> out;

  Beat held;
  bool holding = false, in_frame = false;
  uint64_t grant = 0, sent_cnt = 0;

  Logic64 sent;
};

}  // namespace bach
}  // namespace latch

#endif
