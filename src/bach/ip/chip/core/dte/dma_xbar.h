#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_DMA_XBAR_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_DMA_XBAR_

// DMA_XBAR：五个通道与两块存储之间的那一层。
//
// DTE 对每块存储的读与写各有一个 master 口，而五个通道各自独立打拍、各自发
// 请求。没有这一层的话五个通道会驱动同一根线，后跑的那个把先跑的请求盖掉 ——
// 同线程同拍两次写同一个 Latch 是静默覆盖，不触发断言。
//
// 读与写各走各的口，一拍可以同时发一读一写：存储那一侧只在 bank 冲突时才在
// 读写之间二选一。
//
// 每个通道在入口各留几格：通道按序号把请求放进来，本模块从这几格里轮转取。
//
// ready 拉低到通道真的停下来，中间隔着几拍：通道读的是上一拍的 ready，而两边
// 各自打拍时谁先跑还是不定的，所以拉低那一拍之后还会再来一两笔。格数因此比
// ready 的门限多留两格，多来的那几笔有地方放。
//
// 按序号收而不是只看 req_valid：两侧各自打拍时谁先跑是不定的，读到的可能是当拍
// 也可能是上一拍的值，只看 valid 就会把同一笔收两次或漏掉一次。
//
// 响应按发出顺序回：读请求转发时把发起的通道记进队列，数据回来时按队头分发。

#include <array>
#include <deque>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/dte/dte_types.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class DteXbar : public BachModule {
 public:
  DteXbar(ClockPtr clock, const std::string& name, uint64_t parent = 0,
          bool tick = true)
      : BachModule(clock, name, parent, tick),
        grants(clock) {
    for (uint64_t w = 0; w < kWhichNum; ++w) {
      out[w][kRead] = std::make_shared<MemPort>(clock);
      out[w][kWrite] = std::make_shared<MemPort>(clock);
      for (uint64_t i = 0; i < kLaneNum; ++i) {
        side[w][i] = std::make_shared<MemPort>(clock);
      }
    }
  }

  // ── 对内：每个通道各拿自己那一份 ──
  std::shared_ptr<MemPort> LaneCmem(uint64_t lane) const {
    return side[kToCmem].at(lane);
  }
  std::shared_ptr<MemPort> LaneMmem(uint64_t lane) const {
    return side[kToMmem].at(lane);
  }

  // ── 对外：每块存储的读与写各一个口 ──
  MemPort& CmemRd() { return *out[kToCmem][kRead]; }
  MemPort& CmemWr() { return *out[kToCmem][kWrite]; }
  MemPort& MmemRd() { return *out[kToMmem][kRead]; }
  MemPort& MmemWr() { return *out[kToMmem][kWrite]; }
  void AttachCmemRd(std::shared_ptr<MemPort> p) {
    out[kToCmem][kRead] = std::move(p);
  }
  void AttachCmemWr(std::shared_ptr<MemPort> p) {
    out[kToCmem][kWrite] = std::move(p);
  }
  void AttachMmemRd(std::shared_ptr<MemPort> p) {
    out[kToMmem][kRead] = std::move(p);
  }
  void AttachMmemWr(std::shared_ptr<MemPort> p) {
    out[kToMmem][kWrite] = std::move(p);
  }

  uint64_t Grants() const { return grant_cnt; }
  bool Quiescent() const override {
    for (uint64_t w = 0; w < kWhichNum; ++w) {
      if (!pend[w].empty()) return false;
      for (auto const& q : slot[w]) {
        if (!q.empty()) return false;
      }
    }
    return true;
  }

 protected:
  void Step() override {
    Serve(kToCmem);
    Serve(kToMmem);
    grants = grant_cnt;
    TracePerCycle("grants", grant_cnt);
  }

 private:
  enum Which : uint64_t { kToCmem = 0, kToMmem = 1, kWhichNum = 2 };
  enum Rw : uint64_t { kRead = 0, kWrite = 1, kRwNum = 2 };

  // 入口每个通道的格数，与报 ready 的门限。
  static constexpr uint64_t kSlotDepth = 4;
  static constexpr uint64_t kSlotMark = 2;

  void Serve(uint64_t which) {
    uint64_t owner = kLaneNum;
    ByteBlockPtr data;
    Respond(which, &owner, &data);
    Collect(which);
    Forward(which, kRead);
    Forward(which, kWrite);
    // ready 报的是这一拍收完、发完之后还剩几格。放在最前算的话报的是收这一笔
    // 之前的占用，比实际少一笔，通道就会多发一笔进来。
    for (uint64_t i = 0; i < kLaneNum; ++i) {
      bool mine = (i == owner);
      side[which][i]->DriveSlave(slot[which][i].size() < kSlotMark, mine,
                                 mine ? data : ByteBlockPtr());
    }
  }

  // 读响应按发出顺序回到发起的通道。存储对同一个口是按序返回的。
  void Respond(uint64_t which, uint64_t* owner, ByteBlockPtr* data) {
    MemPort& rd = *out[which][kRead];
    if (!rd.RspValid() || pend[which].empty()) return;
    *owner = pend[which].front();
    *data = rd.RspData();
    pend[which].pop_front();
  }

  // 按序号把新请求收进各自那一格。
  void Collect(uint64_t which) {
    for (uint64_t i = 0; i < kLaneNum; ++i) {
      MemPort const& in = *side[which][i];
      MemReqView r = ReadMemReq(in);
      if (!r.valid || r.seq == seen[which][i]) continue;
      LOGCHECK(slot[which][i].size() < kSlotDepth,
               "DteXbar: 那几格满了。通道应当先看 ready 再发。");
      seen[which][i] = r.seq;
      slot[which][i].push_back(r);
    }
  }

  // 轮转取一笔发给存储。读与写各走各的口，各有各的轮转指针。
  void Forward(uint64_t which, uint64_t rw) {
    MemPort& port = *out[which][rw];
    if (!port.Ready()) {
      port.IdleReq();
      return;
    }
    uint64_t& turn = grant[which][rw];
    for (uint64_t k = 0; k < kLaneNum; ++k) {
      uint64_t i = (turn + k) % kLaneNum;
      // 一个通道的读与写之间保持先来后到：队头不是这一档就轮下一个通道。
      if (slot[which][i].empty()) continue;
      MemReqView const& r = slot[which][i].front();
      if ((r.we ? kWrite : kRead) != rw) continue;
      if (r.we) {
        port.Write(r.addr, r.wdata, r.scale_en, r.woff, r.bytes);
      } else {
        port.Read(r.addr, r.bytes, r.scale_en);
        pend[which].push_back(i);
      }
      slot[which][i].pop_front();
      turn = (i + 1) % kLaneNum;
      ++grant_cnt;
      return;
    }
    port.IdleReq();
  }

  std::array<std::array<std::shared_ptr<MemPort>, kLaneNum>, kWhichNum> side;
  std::array<std::array<std::deque<MemReqView>, kLaneNum>, kWhichNum> slot;
  std::array<std::array<uint64_t, kLaneNum>, kWhichNum> seen{};
  std::array<std::array<std::shared_ptr<MemPort>, kRwNum>, kWhichNum> out;
  std::array<std::array<uint64_t, kRwNum>, kWhichNum> grant{};
  std::array<std::deque<uint64_t>, kWhichNum> pend;
  uint64_t grant_cnt = 0;

  Logic64 grants;
};

}  // namespace bach
}  // namespace latch

#endif
