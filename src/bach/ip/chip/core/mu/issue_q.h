#ifndef _LATCH_BACH_IP_CHIP_CORE_MU_ISSUE_Q_
#define _LATCH_BACH_IP_CHIP_CORE_MU_ISSUE_Q_

// issue_q：深度 16，顺序执行与 finish。
//
// 任务切换无 bubble，并且支持 task 间在执行通路上不同操作类型的重叠：
//   task0 load → {task0 计算 ‖ task1 load} → {task0 写回 ‖ task1 计算} → …
// 所以队列里同时可以有几笔在不同阶段，只是各阶段各自只有一笔。

#include <deque>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/ip/chip/core/mu/agu.h"
#include "bach/ip/chip/core/mu/gen_ep_info.h"
#include "bach/ip/chip/core/mu/regfile.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

constexpr uint64_t kMuIssueQDepth = 16;

// 一笔在飞的任务走过的几个阶段。三段重叠就是这几段各自有一笔。
enum class MuStage : uint32_t {
  kQueued = 0,
  kLoading = 1,
  kComputing = 2,
  kStoring = 3,
  kFinished = 4,
};

// 一笔任务不是一次原语：物理阵列一次只算 K × N 那么大，一笔任务要按 tile 走
// kblock × 专家数 × nblock 遍。三个计数记的是这一笔走到哪了：发了几个 tile
// 的读、算完几个、写回几个。三段重叠就落在这三个计数的差上：写回第 i 个 tile
// 的同时可以在算第 i+1 个、读第 i+2 个。
//
// 读与算按 tile 数走，写回按输出的列数走：K 那一层的几段与合并成一份的那几个
// 专家都只产出一个结果。
struct MuInflight {
  MuTaskCfg cfg;
  MuStage stage = MuStage::kQueued;
  uint64_t seq = 0;
  MuAgu agu;
  uint64_t total = 1;
  uint64_t issued = 0, computed = 0, stored = 0;
  // acu 查出越界：这一笔余下的数据全部作废，已发出的回复收回来也不算。
  bool dropped = false;
  // 这一笔自己的那一份 topK。任务启动时从 Core Mem 读进来，权重地址与 W_ep
  // 都从它取。每笔各存一份：几笔任务在执行通路上重叠时，共用一份会读到别人的
  // 那一组专家。
  std::vector<TopkEntry> topk;
  bool topk_asked = false;
  bool topk_ready = false;
  // 各 tile 写回哪。发读的时候由 AGU 算出来记下，算完那一拍再取，因为 AGU 的
  // 迭代这时已经走到后面的 tile 了。
  std::vector<uint64_t> out_addr;

  explicit MuInflight(MuTaskCfg const& task)
      : cfg(task), agu(task),
        total(task.kblock * task.Experts() * task.nblock) {
    if (total == 0) total = 1;
  }
  uint64_t Kblock() const { return cfg.kblock == 0 ? 1 : cfg.kblock; }
  // 写回的次数。一列不管切成几段都只写一次；几个专家合并成一份时也只写一次，
  // 各出一份时每个专家写一次。
  uint64_t OutTotal() const {
    uint64_t n = cfg.nblock;
    if (!cfg.ep_reduce) n *= cfg.Experts();
    return n == 0 ? 1 : n;
  }
  bool Done() const { return stored >= OutTotal(); }
};

class MuIssueQ : public BachModule {
 public:
  MuIssueQ(ClockPtr clock, const std::string& name, uint64_t parent = 0,
           bool tick = true)
      : BachModule(clock, name, parent, tick), depth(clock), issued(clock) {}

  bool HasRoom() const { return q.size() < kMuIssueQDepth; }
  void Push(MuTaskCfg const& cfg) {
    LOGCHECK(HasRoom(), "MuIssueQ: 满了。写 trigger 前应当先看有没有空位。");
    MuInflight f(cfg);
    f.seq = next_seq++;
    q.push_back(f);
    ++issue_pending;
  }

  // 取第一笔还停在某个阶段的任务。顺序执行：只看队头那几笔。
  MuInflight* FirstAt(MuStage s) {
    for (auto& f : q) {
      if (f.stage == s) return &f;
    }
    return nullptr;
  }
  // 取第一笔还有 tile 没发读的、没算的、没写回的。三段各自找各自的那一笔，
  // 于是同一笔任务的相邻 tile 与相邻两笔任务都能重叠。
  MuInflight* FirstToLoad() {
    for (auto& f : q) {
      if (f.issued < f.total) return &f;
    }
    return nullptr;
  }
  MuInflight* FirstToCompute() {
    for (auto& f : q) {
      if (f.computed < f.issued) return &f;
    }
    return nullptr;
  }
  // 写回那一段找的是「还有列没写回」的那一笔。一列切成几段时，段与段之间没有
  // 结果产出，所以这里不按已算的段数比。
  MuInflight* FirstToStore() {
    for (auto& f : q) {
      if (f.stored < f.OutTotal()) return &f;
    }
    return nullptr;
  }
  MuInflight* Head() { return q.empty() ? nullptr : &q.front(); }
  // 按下发序号找。读回来的响应带着这个号，据此记给对应那一笔。
  MuInflight* BySeq(uint64_t s) {
    for (auto& f : q) {
      if (f.seq == s) return &f;
    }
    return nullptr;
  }
  // 队头做完就出队：finish 按顺序，与执行通路上的重叠无关。
  void RetireFront() {
    if (!q.empty() && q.front().stage == MuStage::kFinished) q.pop_front();
  }
  bool Empty() const { return q.empty(); }
  uint64_t Size() const { return q.size(); }
  uint64_t Issued() const { return issued.Get(); }

  bool Quiescent() const override { return q.empty(); }

 protected:
  void Step() override {
    depth = q.size();
    issued = issue_pending;
    TracePerCycle("depth", q.size());
  }

 private:
  std::deque<MuInflight> q;
  uint64_t next_seq = 0, issue_pending = 0;

  Logic64 depth, issued;
};

}  // namespace bach
}  // namespace latch

#endif
