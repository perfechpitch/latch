#ifndef _LATCH_BACH_IP_CHIP_CORE_MU_STQ_
#define _LATCH_BACH_IP_CHIP_CORE_MU_STQ_

// stq：把各 lane 的结果拼成整拍再写回 Core Mem。
//
// 队列深 16；Core Mem 写带宽 256 B（接口 1 KB），不足 1 KB 按实际传输并标记
// mask；写延迟 16（待定）。
//
// Wr concat buffer 1～2 KB：各 lane 的 buffer 深度不同，取决于物理距离，最远
// 16 拍、最近 1 拍，越近 buffer 越大，最大深度 16。
//
// Store concat 按 vlane 分两种拼装：
//   vlane=1  步进横切，所有 lane buffer 并行 128 B 截面，连续取 8 次攒满 1 KB
//   vlane=2  纵向整块，每 lane 一次取 8 B、共 256 B 截面，连续取 4 次攒满 1 KB
//
// 结果写回后与 issue_q 的 finish 合成 dsa_done；trigger 里的 last 标志决定这一笔
// 要不要报 TS。

#include <deque>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/common/numeric/formats.h"
#include "bach/ip/chip/core/mu/regfile.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

constexpr uint64_t kMuStqDepth = 16;
// 攒满一拍要多少字节。
constexpr uint64_t kMuConcatBytes = 1024;

class MuStq : public BachModule {
 public:
  MuStq(ClockPtr clock, const std::string& name, uint64_t parent = 0,
        bool tick = true)
      : BachModule(clock, name, parent, tick),
        port(std::make_shared<MemPort>(clock)),
        stores(clock),
        drained(clock) {}

  MemPort& Port() { return *port; }
  void AttachPort(std::shared_ptr<MemPort> p) { port = std::move(p); }

  bool HasRoom() const { return q.size() < kMuStqDepth; }

  // 一次原语的结果进来。按 DTYPE_C 编成字节，攒够一拍再发。
  void Push(uint64_t addr, std::vector<float> const& vals, bool out_bf16) {
    LOGCHECK(HasRoom(), "MuStq: 队列满了。");
    Entry e;
    e.addr = addr;
    for (float v : vals) {
      if (out_bf16) {
        uint16_t h = numeric::ToBf16(v);
        e.bytes.push_back(uint8_t(h & 0xFF));
        e.bytes.push_back(uint8_t(h >> 8));
      } else {
        uint32_t b = numeric::BitsOf(v);
        for (int k = 0; k < 4; ++k) e.bytes.push_back(uint8_t((b >> (8 * k)) & 0xFF));
      }
    }
    q.push_back(e);
  }

  uint64_t Stores() const { return stores.Get(); }
  uint64_t Drained() const { return drained.Get(); }

  bool Quiescent() const override { return q.empty(); }

 protected:
  void Step() override {
    Issue();
    stores = store_pending;
    drained = q.empty() ? 1 : 0;
    TracePerCycle("depth", q.size());
  }

 private:
  struct Entry {
    uint64_t addr = 0;
    std::vector<uint8_t> bytes;
  };

  void Issue() {
    if (q.empty() || !port->Ready()) {
      port->IdleReq();
      return;
    }
    Entry e = q.front();
    q.pop_front();
    // 不足 1 KB 按实际传输并标记 mask。这里就是按实际长度发。
    auto data = std::make_shared<ByteBlock>(e.bytes);
    port->Write(e.addr, data);
    ++store_pending;
  }

  std::shared_ptr<MemPort> port;
  std::deque<Entry> q;
  uint64_t store_pending = 0;

  Logic64 stores, drained;
};

}  // namespace bach
}  // namespace latch

#endif
