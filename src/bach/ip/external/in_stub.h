#ifndef _LATCH_BACH_IP_EXTERNAL_IN_STUB_
#define _LATCH_BACH_IP_EXTERNAL_IN_STUB_

// 入口桩：站在 LPU 入口，一个 GPU 一个实例。
//
// 三级：
//   G1 注入判定  两道闸门都开才放行，1 credit = 1 token
//   G2 封包      写 DPU 自定义包头，切成 flit 进 tx_q
//   G3 retired   收端回来的 retired 更新 credit
//
// 两道闸门的分工：第一层是 GPU 自己的发送缓冲有没有位置，第二层是 Bach 全局的
// 池子还剩多少。两层都是 1 credit = 1 token，都开才流。
//
// 组播下 retired 按最慢收端取 min：一份数据复制给多个收端，只有全部收端都退了
// 这一笔才算真的腾出空间。
//
// 注入顺序有一条规矩由 inject_tbl 自己保证：weights 加载阶段先发最远路径的数据。
// 先发近的会让不进本核的 weights 卡在 Router 里，要等 DTE 把当前数据搬走才能继续收。

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/common/flit.h"
#include "bach/common/seq.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

constexpr uint64_t kTxQueueDepth = 32;

// 一个 token 的级联包：激活 6144 B + scale 192 B + expert 16 B + weight 16 B。
constexpr uint64_t kTokenBytes = 6368;

struct InjectItem {
  uint64_t inject_cycle = 0;
  uint64_t gpu_id = 0;
  uint64_t token_id = 0;
  uint64_t dst = 0;              // PCIe Switch 按它查目的端口
  // 进了阵列之后按它走：每一跳的 Router 拿它查自己的 RouterTable。dst 只管到
  // 第一颗 chip 的边缘口为止。
  uint64_t path_id = 0;
  // 这一份数据要不要算。取自软件 payload 的一位，Router 解析包头时原样转给 TS。
  bool compute = true;
  uint64_t bytes = kTokenBytes;
  std::vector<uint8_t> payload;  // 空表示这一轮不关心内容
};

// 一个 GPU 那一份本地 credit。
struct GpuCredit {
  uint64_t buffer_depth_tokens = 0;
  uint64_t buffer_used = 0;
  uint64_t inflight_bach = 0;
  uint64_t retired_min = 0;
  // 各收端各自上报到哪了，取 min 才是真的退了。
  std::map<uint64_t, uint64_t> retired_by_src;
};

class InStub : public BachModule {
 public:
  InStub(ClockPtr clock, const std::string& name, uint64_t pool_size,
         uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        pool_total(pool_size),
        tx(std::make_shared<LinkEnd>(clock)),
        injected(clock),
        sent_flits(clock),
        pool_avail(clock) {}

  LinkEnd& Tx() { return *tx; }
  LinkEndPtr TxPtr() const { return tx; }
  void AttachTx(LinkEndPtr wire) { tx = std::move(wire); }

  // 构造期读入。
  void SetInjectTable(std::vector<InjectItem> tbl) { inject_tbl = std::move(tbl); }
  void SetGpuBuffer(uint64_t gpu, uint64_t depth_tokens) {
    gpu_credit[gpu].buffer_depth_tokens = depth_tokens;
  }
  // EP6+TP8 的 LPU Dispatch：所有 R core 都有余量才派遣，派时各减一。
  void SetDispatchSlots(std::vector<uint64_t> slots) {
    dispatch_slots = std::move(slots);
  }

  // 收端上报：这个 GPU 的 token 退到哪了。src 区分是哪个收端。
  void ReportRetired(uint64_t gpu, uint64_t src, uint64_t retired_token) {
    pending_retired.push_back({gpu, src, retired_token});
  }
  // 链尾返回，R core 的余量加回来。
  void ReturnDispatchSlot(uint64_t r) {
    if (r < dispatch_slots.size()) ++dispatch_slots[r];
  }

  uint64_t Injected() const { return injected.Get(); }
  uint64_t SentFlits() const { return sent_flits.Get(); }
  uint64_t PoolAvail() const { return pool_avail.Get(); }

  bool Quiescent() const override {
    return cursor >= inject_tbl.size() && tx_q.empty() &&
           pending_retired.empty();
  }

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做。
    Transmit();
    Retire();
    Inject(now);

    injected = injected_pending;
    sent_flits = sent_pending;
    pool_avail = PoolLeft();
    TracePerCycle("injected", injected_pending);
    TracePerCycle("tx_q", tx_q.size());
  }

 private:
  struct RetiredMsg {
    uint64_t gpu = 0, src = 0, token = 0;
  };

  uint64_t PoolLeft() const {
    uint64_t used = 0;
    for (auto const& kv : gpu_credit) used += kv.second.inflight_bach;
    return pool_total > used ? pool_total - used : 0;
  }

  // G1：两道闸门。
  void Inject(uint64_t now) {
    if (cursor >= inject_tbl.size()) return;
    InjectItem const& it = inject_tbl[cursor];
    if (it.inject_cycle > now) return;
    if (tx_q.size() + FlitsOf(it.bytes) > kTxQueueDepth) return;

    GpuCredit& g = gpu_credit[it.gpu_id];
    // 第一层：GPU 本地缓冲
    if (g.buffer_depth_tokens != 0 && g.buffer_used >= g.buffer_depth_tokens) {
      return;
    }
    // 第二层：Bach 全局池
    if (PoolLeft() == 0) return;
    // LPU Dispatch：所有 R core 都有余量才派遣
    for (uint64_t s : dispatch_slots) {
      if (s == 0) return;
    }
    for (uint64_t& s : dispatch_slots) --s;

    ++g.buffer_used;
    ++g.inflight_bach;
    seq_w = SeqAdd(seq_w, 1);
    Encapsulate(it);
    ++cursor;
    ++injected_pending;
  }

  // G2：封包。DPU 写 gpu_id + token_id，切成 flit。
  void Encapsulate(InjectItem const& it) {
    auto m = std::make_shared<Message>();
    m->gpu_id = it.gpu_id;
    m->token_id = it.token_id;
    m->dst = it.dst;
    m->path_id = it.path_id;
    m->compute = it.compute ? 1 : 0;
    m->size = it.bytes;
    m->user_id = it.token_id;
    m->seq = hdr_cnt[it.gpu_id]++;
    m->payload = it.payload;

    uint64_t n = FlitsOf(it.bytes);
    uint64_t left = it.bytes;
    for (uint64_t i = 0; i < n; ++i) {
      FlitView f;
      f.valid = true;
      f.vc = 0;
      f.head = (i == 0);
      f.tail = (i + 1 == n);
      f.bytes = left > kFlitBytes ? kFlitBytes : left;
      f.msg = m;
      tx_q.push_back(f);
      left = left > kFlitBytes ? left - kFlitBytes : 0;
    }
  }

  void Transmit() {
    if (tx_q.empty()) {
      tx->flit.Idle();
    } else {
      FlitView const& f = tx_q.front();
      tx->flit.Drive(f.vc, f.head, f.tail, f.bytes, f.msg);
      tx_q.pop_front();
      ++sent_pending;
    }
    tx->release.Idle();
  }

  // G3：retired 取 min 再记账。
  void Retire() {
    for (auto const& r : pending_retired) {
      GpuCredit& g = gpu_credit[r.gpu];
      g.retired_by_src[r.src] = r.token;
      uint64_t lo = 0;
      bool first = true;
      for (auto const& kv : g.retired_by_src) {
        if (first || SeqAfter(kv.second, lo)) {
          lo = kv.second;
          first = false;
        }
      }
      uint64_t k = SeqDiff(g.retired_min, lo);
      if (k == 0) continue;
      g.retired_min = lo;
      g.buffer_used = g.buffer_used > k ? g.buffer_used - k : 0;
      g.inflight_bach = g.inflight_bach > k ? g.inflight_bach - k : 0;
    }
    pending_retired.clear();
  }

  uint64_t pool_total;
  LinkEndPtr tx;

  // Step 独占。
  std::vector<InjectItem> inject_tbl;
  uint64_t cursor = 0;
  std::map<uint64_t, GpuCredit> gpu_credit;
  std::map<uint64_t, uint64_t> hdr_cnt;
  std::vector<uint64_t> dispatch_slots;
  std::deque<FlitView> tx_q;
  std::vector<RetiredMsg> pending_retired;
  uint64_t seq_w = 0;
  uint64_t injected_pending = 0, sent_pending = 0;

  Logic64 injected, sent_flits, pool_avail;
};

}  // namespace bach
}  // namespace latch

#endif
