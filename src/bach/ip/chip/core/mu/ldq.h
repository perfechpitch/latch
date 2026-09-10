#ifndef _LATCH_BACH_IP_CHIP_CORE_MU_LDQ_
#define _LATCH_BACH_IP_CHIP_CORE_MU_LDQ_

// ldq ×2：Token 与 Weight 各一条读队列。
//
// Token ldq 深 16，取决于读延时；Core Mem 读带宽 256 B，读延迟 16（待定）。
// Rd outstanding buffer 16 × 256 B = 4 KB，用来掩盖 latency。
//
// Weight ldq 深 4；Matrix Mem 读带宽 8 KB，bank 与 lane 一对一垂直贴合、无
// crossbar，读延迟 4T（读 sram 2T 加打拍 2T）。各 lane 访存地址相同，只发一个
// 地址然后逐级脉动到各 lane，所以一次读就够整个阵列用。
//
// MAC 入口用乒乓 2 级缓存掩盖 Mmem 读出延迟。
//
// vlane 对 Load token 的影响：从 buffer 只读取 256 B / vlane_num 字节，再 copy
// 扩展到 256 B 输出。

#include <deque>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

constexpr uint64_t kTokenLdqDepth = 16;
constexpr uint64_t kWeightLdqDepth = 4;
constexpr uint64_t kMuRdOutstanding = 16;

class MuLdq : public BachModule {
 public:
  struct Req {
    uint64_t addr = 0;
    uint64_t bytes = 0;
    uint64_t tag = 0;
    // 带 scale 的那一档：block scale 与数据一一映射，存储把它附在响应正文
    // 之后一起回来，不占独立的读通道。
    bool scale_en = false;
    // 这一笔读的是 topK 表不是 token。两样都从 Core Mem 读，走同一条队列，
    // 收方按这个标记分开。
    bool topk = false;
  };
  struct Rsp {
    uint64_t tag = 0;
    bool topk = false;
    std::vector<uint8_t> data;
  };

  MuLdq(ClockPtr clock, const std::string& name, uint64_t slots,
        uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        depth(slots),
        port(std::make_shared<MemPort>(clock)),
        issued(clock),
        returned(clock) {}

  MemPort& Port() { return *port; }
  void AttachPort(std::shared_ptr<MemPort> p) { port = std::move(p); }

  bool HasRoom() const { return q.size() < depth; }
  void Push(Req const& r) {
    LOGCHECK(HasRoom(), "MuLdq: 队列满了。");
    q.push_back(r);
  }

  bool HasData() const { return !done.empty(); }
  // 队头那一笔是不是 topK 表。它没有配对的权重读，收方要单独取走。
  bool HeadIsTopk() const { return !done.empty() && done.front().topk; }
  Rsp TakeData() {
    Rsp r = done.front();
    done.pop_front();
    return r;
  }

  uint64_t Issued() const { return issued.Get(); }
  uint64_t Returned() const { return returned.Get(); }

  bool Quiescent() const override {
    return q.empty() && inflight.empty() && done.empty();
  }

 protected:
  void Step() override {
    Collect();
    Issue();
    issued = issue_pending;
    returned = return_pending;
    TracePerCycle("inflight", inflight.size());
  }

 private:
  void Issue() {
    // outstanding 限额挡住读跑得太超前。
    if (q.empty() || inflight.size() >= kMuRdOutstanding || !port->Ready()) {
      port->IdleReq();
      return;
    }
    Req r = q.front();
    q.pop_front();
    port->Read(r.addr, r.bytes, r.scale_en);
    inflight.push_back(r);
    ++issue_pending;
  }

  void Collect() {
    if (!port->RspValid() || inflight.empty()) return;
    Req r = inflight.front();
    inflight.pop_front();
    Rsp s;
    s.tag = r.tag;
    s.topk = r.topk;
    auto d = port->RspData();
    if (d) s.data = *d;
    done.push_back(s);
    ++return_pending;
  }

  uint64_t depth;
  std::shared_ptr<MemPort> port;
  std::deque<Req> q, inflight;
  std::deque<Rsp> done;
  uint64_t issue_pending = 0, return_pending = 0;

  Logic64 issued, returned;
};

}  // namespace bach
}  // namespace latch

#endif
