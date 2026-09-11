#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_COMPLETION_RS_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_COMPLETION_RS_

// Completion RS 与 Done Pending：把劈开的两半合回来。
//
// 只在同一个 task_id 的 RD 与 WR 两侧条件都满足时产生 task_done，这一步叫 Join。
//
// 认哪两半是同一笔，用的是 Commit 准入时分配的内部序号。业务上的 task_id 只在
// 一个 stream 内唯一，同一拍在途的两笔任务可以带同一个值：一笔是 Router 送进来
// 的搬入，另一笔是 RV core 配的搬出。向 TS 上报时用的仍是业务身份。
// 同一拍多个 Join 命中时全部写进 Done Pending，不允许覆盖或丢失，由 Done Pending
// 负责串行化。向 TS 的报告是 exactly-once。
//
// 六个完成层级：
//   queued      已进 TaskQueue 未装载
//   active      由对应 AGCU / Ctrl 执行
//   issue_done  该侧最后一个请求已 Fire
//   drained     相关响应、Buffer 数据和外部副作用均已收敛
//   join_done   同一 task_id 的 RD 与 WR 都满足
//   task_done   进 Done Pending 并与 TS 成功握手
//
// task_last 标记一个 task 拆成几笔搬运时的最后一笔，只有带这个标记的那一笔完成
// 后才通知 TS；no_ack 置位的任务不回 Ack。
//
// shareMem 写落在 Join 之后：任务数据传输完成后按描述符里的地址与数据写一笔
// shareMem，写出去了才通知 TS。这一路只有 B core 与 R core 用。

#include <deque>
#include <map>
#include <vector>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class CompletionRs : public BachModule {
 public:
  CompletionRs(ClockPtr clock, const std::string& name, uint64_t parent = 0,
               bool tick = true)
      : BachModule(clock, name, parent, tick),
        rd_done(std::make_shared<HalfDonePort>(clock)),
        wr_done(std::make_shared<HalfDonePort>(clock)),
        to_ts(std::make_shared<DonePort>(clock)),
        admit(std::make_shared<AdmitPort>(clock)),
        smem_wr(std::make_shared<MemPort>(clock)),
        joined(clock),
        reported(clock),
        used(clock) {}

  HalfDonePort& RdDone() { return *rd_done; }
  HalfDonePort& WrDone() { return *wr_done; }
  std::shared_ptr<HalfDonePort> RdDonePtr() const { return rd_done; }
  std::shared_ptr<HalfDonePort> WrDonePtr() const { return wr_done; }

  // 五个通道各有两侧，各自写各自的口：一根线只有一个写者，不能让五个 Lane
  // 挤在同一个口上。装配层把它们都登记进来，这里每拍逐个收。
  void AddSource(std::shared_ptr<HalfDonePort> rd,
                 std::shared_ptr<HalfDonePort> wr) {
    sources.push_back({std::move(rd), std::move(wr)});
  }
  DonePort& ToTs() { return *to_ts; }
  void AttachToTs(std::shared_ptr<DonePort> p) { to_ts = std::move(p); }

  // shareMem 表项写。B core 与 R core 之外的 core 上这个口一直空闲。
  MemPort& SmemWr() { return *smem_wr; }
  std::shared_ptr<MemPort> SmemWrPtr() const { return smem_wr; }
  void AttachSmemWr(std::shared_ptr<MemPort> p) { smem_wr = std::move(p); }

  // Commit 占一项。三样资源里的第三样就是它。占用走端口：这张表由本模块的
  // 协程在 Join 里逐项删，让 Commit 的协程直接往里插就是两个线程改同一个容器。
  std::shared_ptr<AdmitPort> AdmitPtr() const { return admit; }
  void AttachAdmit(std::shared_ptr<AdmitPort> p) { admit = std::move(p); }
  bool HasRoom() const { return rs.size() < kCompRsNum; }

  uint64_t SmemWrites() const { return smem_cnt; }
  uint64_t Joined() const { return joined.Get(); }
  uint64_t Reported() const { return reported.Get(); }
  uint64_t Used() const { return used.Get(); }

  bool Quiescent() const override { return rs.empty() && pend.empty(); }

 protected:
  void Step() override {
    // 末级先做：先把 Done Pending 里的报给 TS，腾出位置。
    Report();
    Collect();
    Join();
    TakeAdmit();

    if (!smem_used) smem_wr->IdleReq();
    joined = join_pending;
    reported = report_pending;
    used = rs.size();
    TracePerCycle("rs_used", rs.size());
    TracePerCycle("done_pend", pend.size());
  }

 private:
  struct Entry {
    Descriptor desc;
    bool rd_ok = false, wr_ok = false;
    bool rd_drained = false, wr_drained = false;
  };
  struct Pend {
    uint64_t stream_id = 0, task_id = 0, reduce_seq = 0;
    bool notify = true;   // task_last 且非 no_ack 的那一笔才通知 TS
    bool smem = false;
    uint64_t smem_addr = 0, smem_data = 0;
  };

  // 收 Commit 准入的任务，占一项。Join 排在它前面，所以这一拍腾出来的位置
  // 当拍就能用上。
  void TakeAdmit() {
    admit->DriveReady(HasRoom());
    if (!admit->Valid() || admit->Seq() == last_admit_seq) return;
    auto d = admit->Desc();
    if (!d) return;
    last_admit_seq = admit->Seq();
    LOGCHECK(HasRoom(), "CompletionRs: 满了。Commit 应当先确认有空位。");
    LOGCHECK(rs.count(d->commit_seq) == 0,
             "CompletionRs: 同一个内部序号在未完成的上下文里重复占用。");
    Entry e;
    e.desc = *d;
    rs[d->commit_seq] = e;
  }

  void Collect() {
    Take(*rd_done, kRd);
    Take(*wr_done, kWr);
    for (auto const& s : sources) {
      Take(*s.rd, kRd);
      Take(*s.wr, kWr);
    }
  }

  void Take(HalfDonePort const& p, uint64_t which) {
    if (!p.Valid()) return;
    auto it = rs.find(p.commit_seq.Get());
    if (it == rs.end()) return;
    bool drained = p.drained.Get() != 0;
    if (which == kRd) {
      it->second.rd_ok = true;
      it->second.rd_drained = drained;
    } else {
      it->second.wr_ok = true;
      it->second.wr_drained = drained;
    }
  }

  // 两侧条件都满足才 Join。同一拍多个命中全部写进 Done Pending。
  void Join() {
    for (auto it = rs.begin(); it != rs.end();) {
      Entry const& e = it->second;
      if (!(e.rd_ok && e.wr_ok && e.rd_drained && e.wr_drained)) {
        ++it;
        continue;
      }
      if (pend.size() >= kDonePendDepth) break;  // 串行化，不丢
      ++join_pending;
      // 只有带 task_last 的那一笔完成后才通知 TS；no_ack 的不回 Ack。
      bool notify = e.desc.task_last && !e.desc.no_ack;
      if (notify || e.desc.smem_wr) {
        pend.push_back({e.desc.stream_id, e.desc.task_id, e.desc.reduce_seq,
                        notify, e.desc.smem_wr, e.desc.smem_addr,
                        e.desc.smem_data});
      }
      it = rs.erase(it);
    }
  }

  // 向 TS 的报告是 exactly-once：这里一拍报一笔，报完就出队。带 shareMem 写的
  // 那一笔先把它写出去，写出去了下一拍再报 TS。
  void Report() {
    smem_used = false;
    if (pend.empty()) {
      to_ts->Idle();
      return;
    }
    Pend& p = pend.front();
    if (p.smem) {
      to_ts->Idle();
      if (!smem_wr->Ready()) return;
      auto d = std::make_shared<ByteBlock>(4, 0);
      for (uint64_t k = 0; k < 4; ++k) {
        (*d)[k] = uint8_t((p.smem_data >> (8 * k)) & 0xFFu);
      }
      smem_wr->Write(p.smem_addr, d);
      smem_used = true;
      p.smem = false;
      ++smem_cnt;
      return;
    }
    if (!p.notify) {
      pend.pop_front();
      to_ts->Idle();
      return;
    }
    to_ts->Drive(p.stream_id, p.task_id);
    pend.pop_front();
    ++report_pending;
  }

  struct Source {
    std::shared_ptr<HalfDonePort> rd, wr;
  };
  std::vector<Source> sources;
  std::shared_ptr<HalfDonePort> rd_done, wr_done;
  std::shared_ptr<MemPort> smem_wr;
  bool smem_used = false;
  uint64_t smem_cnt = 0;
  std::shared_ptr<DonePort> to_ts;
  std::shared_ptr<AdmitPort> admit;
  uint64_t last_admit_seq = 0;

  std::map<uint64_t, Entry> rs;
  std::deque<Pend> pend;
  uint64_t join_pending = 0, report_pending = 0;

  Logic64 joined, reported, used;
};

}  // namespace bach
}  // namespace latch

#endif
