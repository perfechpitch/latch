#ifndef _LATCH_BACH_IP_EXTERNAL_OUT_STUB_
#define _LATCH_BACH_IP_EXTERNAL_OUT_STUB_

// 出口桩：站在 LPU 出口。
//
// 两级：
//   X1 收 flit，按 (gpu_id, token_id) 开重组缓冲，逐 flit 追加，回一拍 vc_release
//   X2 尾 flit 到 → 与期望输出逐 bit 比对、记完成、向入口桩回报 retired
//
// 这一轮不建 numeric，期望输出由外面直接给字节；没给期望的 token 只记完成、
// 不比对。等 reference/ 建起来，比对的那一半换成它的输出，接口不变。

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/log.h"
#include "bach/common/flit.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class OutStub : public BachModule {
 public:
  using TokenKey = std::pair<uint64_t, uint64_t>;  // (gpu_id, token_id)

  // 收齐一个 token 时回调，入口桩据此记 retired。
  using RetiredFn = std::function<void(uint64_t gpu, uint64_t token)>;

  OutStub(ClockPtr clock, const std::string& name, uint64_t parent = 0,
          bool tick = true)
      : BachModule(clock, name, parent, tick),
        rx(std::make_shared<LinkEnd>(clock)),
        tx(std::make_shared<LinkEnd>(clock)),
        done_cnt(clock),
        mismatch_cnt(clock),
        last_cycle(clock) {}

  // rx 只读：一根线只有一个写者，写它的是上游。vc_release 要往回程那根写，
  // 所以另有一个 tx 口。
  LinkEnd& Rx() { return *rx; }
  LinkEndPtr RxPtr() const { return rx; }
  void AttachRx(LinkEndPtr wire) { rx = std::move(wire); }

  LinkEnd& Tx() { return *tx; }
  LinkEndPtr TxPtr() const { return tx; }
  void AttachTx(LinkEndPtr wire) { tx = std::move(wire); }

  void SetExpect(uint64_t gpu, uint64_t token, std::vector<uint8_t> bytes) {
    expect[{gpu, token}] = std::move(bytes);
  }
  void SetExpectTokens(uint64_t n) { expect_tokens = n; }
  void OnRetired(RetiredFn fn) { retired_fn = std::move(fn); }

  uint64_t DoneCount() const { return done_cnt.Get(); }
  uint64_t MismatchCount() const { return mismatch_cnt.Get(); }
  uint64_t LastCycle() const { return last_cycle.Get(); }
  bool AllDone() const {
    return expect_tokens != 0 && done_pending >= expect_tokens;
  }

  // 主线程在 JoinAll 之后取，判完成集合用。
  std::vector<TokenKey> const& DoneSet() const { return done_set; }

  bool Quiescent() const override { return asm_buf.empty(); }

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    FlitView f = ReadFlit(rx->flit);
    bool release_vc = false;
    uint64_t release_id = 0;

    if (f.valid && f.msg) {
      TokenKey key{f.msg->gpu_id, f.msg->token_id};
      Entry& e = asm_buf[key];
      if (f.head) {
        e.bytes = 0;
        e.data.clear();
        e.want = f.msg->size;
      }
      e.bytes += f.bytes;
      // payload 只有在给了期望输出时才攒，否则不占内存。
      if (!f.msg->payload.empty() && expect.count(key) != 0) {
        e.data.insert(e.data.end(), f.msg->payload.begin(),
                      f.msg->payload.end());
      }
      release_vc = true;
      release_id = f.vc;

      if (f.tail) {
        Finish(key, e, now);
        asm_buf.erase(key);
      }
    }

    tx->flit.Idle();
    tx->release.Drive(release_vc, release_id, false, 0, false, 0);

    done_cnt = done_pending;
    mismatch_cnt = mismatch_pending;
    last_cycle = last_pending;
    TracePerCycle("done", done_pending);
    TracePerCycle("mismatch", mismatch_pending);
  }

 private:
  struct Entry {
    uint64_t bytes = 0;
    uint64_t want = 0;
    std::vector<uint8_t> data;
  };

  void Finish(TokenKey const& key, Entry const& e, uint64_t now) {
    auto it = expect.find(key);
    bool bad = false;
    if (it != expect.end()) {
      bad = (e.data != it->second);
    }
    if (bad) ++mismatch_pending;
    ++done_pending;
    last_pending = now;
    done_set.push_back(key);
    if (retired_fn) retired_fn(key.first, key.second);
  }

  LinkEndPtr rx, tx;

  // Step 独占。
  std::map<TokenKey, Entry> asm_buf;
  std::map<TokenKey, std::vector<uint8_t>> expect;
  std::vector<TokenKey> done_set;
  RetiredFn retired_fn;
  uint64_t expect_tokens = 0;
  uint64_t done_pending = 0, mismatch_pending = 0, last_pending = 0;

  Logic64 done_cnt, mismatch_cnt, last_cycle;
};

}  // namespace bach
}  // namespace latch

#endif
