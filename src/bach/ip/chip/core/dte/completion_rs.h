#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_COMPLETION_RS_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_COMPLETION_RS_

// CompletionRs joins physical completion, issues ordered ShareMem writes, and
// retires the global head only after its response. ACK carries UID/SID/TID and
// is valid-only.

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"
#include "base/log.h"

namespace latch {
namespace bach {

class CompletionRs : public BachModule {
public:
  CompletionRs(ClockPtr clock, const std::string &name, uint64_t parent = 0,
               bool tick = true)
      : BachModule(clock, name, parent, tick),
        rd_done(std::make_shared<HalfDonePort>(clock)),
        wr_done(std::make_shared<HalfDonePort>(clock)),
        to_ts(std::make_shared<DonePort>(clock)),
        admit(std::make_shared<AdmitPort>(clock)),
        smem_wr(std::make_shared<MemPort>(clock)), joined(clock),
        reported(clock), used(clock),
        retired(std::make_shared<HalfDonePort>(clock)),
        exception(std::make_shared<DteExceptionPort>(clock)) {}

  HalfDonePort &RdDone() { return *rd_done; }
  HalfDonePort &WrDone() { return *wr_done; }
  std::shared_ptr<HalfDonePort> RdDonePtr() const { return rd_done; }
  std::shared_ptr<HalfDonePort> WrDonePtr() const { return wr_done; }

  // 五个通道各有两侧，各自写各自的口：一根线只有一个写者，不能让五个 Lane
  // 挤在同一个口上。装配层把它们都登记进来，这里每拍逐个收。
  void AddSource(std::shared_ptr<HalfDonePort> rd,
                 std::shared_ptr<HalfDonePort> wr) {
    sources.push_back({std::move(rd), std::move(wr)});
  }
  DonePort &ToTs() { return *to_ts; }
  void AttachToTs(std::shared_ptr<DonePort> p) { to_ts = std::move(p); }

  // shareMem 表项写。B core 与 R core 之外的 core 上这个口一直空闲。
  MemPort &SmemWr() { return *smem_wr; }
  std::shared_ptr<MemPort> SmemWrPtr() const { return smem_wr; }
  void AttachSmemWr(std::shared_ptr<MemPort> p) { smem_wr = std::move(p); }

  // Commit 占一项。三样资源里的第三样就是它。占用走端口：这张表由本模块的
  // 协程在 Join 里逐项删，让 Commit 的协程直接往里插就是两个线程改同一个容器。
  std::shared_ptr<AdmitPort> AdmitPtr() const { return admit; }
  void AttachAdmit(std::shared_ptr<AdmitPort> p) { admit = std::move(p); }
  bool HasRoom() const { return rs.size() < kCompRsNum; }

  uint64_t Joined() const { return joined.Get(); }
  uint64_t Reported() const { return reported.Get(); }
  uint64_t Used() const { return used.Get(); }
  // 刚报到 TS 的那一笔的身份，供 Core 层发波形。完成脉冲本身只带 stream 与
  // task（Drive 的第三参默认 0），user 取自描述符里 RV core 采下来的那一份。
  uint64_t DoneTask() const { return done_task; }
  uint64_t DoneUser() const { return done_user; }
  // 完成了、但**不报 TS** 的那一批（ack_ts_en = 0）：自启动 core 的 Bypass DataIn、
  // 权重加载阶段的 datain。与 Reported() 互斥，每笔退休任务只走一支。Core 层发
  // dsa_done_noack 用 —— 波形上 DTE-DSA 的终点要靠它，不报不等于没完成。
  uint64_t NoAckCnt() const { return noack_cnt; }
  uint64_t NoAckTask() const { return noack_task; }
  uint64_t NoAckUser() const { return noack_user; }

  bool Quiescent() const override { return rs.empty() && smem_flight.empty(); }
  std::shared_ptr<HalfDonePort> RetiredPtr() const { return retired; }
  void AttachCanceled(std::shared_ptr<HalfDonePort> p) {
    canceled = std::move(p);
  }
  uint64_t EntryState(uint64_t seq) const {
    auto it = rs.find(seq);
    if (it == rs.end())
      return 0;
    auto const &entry = it->second;
    return (entry.rd_drained ? 4 : 0) | (entry.wr_drained ? 8 : 0) |
           (entry.joined ? 16 : 0) | (entry.smem_issued ? 32 : 0) |
           (entry.smem_complete ? 64 : 0) | (entry.desc.error ? 128 : 0);
  }
  uint64_t Error() const { return first_error; }
  std::shared_ptr<DteExceptionPort> ExceptionPtr() const { return exception; }
  uint64_t ErrorEntry() const { return first_entry; }
  uint64_t EntryError(uint64_t seq) const {
    auto it = rs.find(seq);
    return it == rs.end() ? 0 : it->second.desc.error;
  }
  uint64_t Interrupt() const { return first_error && !interrupt_mask; }
  uint64_t InterruptMask() const { return interrupt_mask; }
  void SetInterruptMask(uint64_t value) { interrupt_mask = value & 1; }
  uint64_t ErrorIds() const { return error_ids; }
  void SuppressAcks() {
    for (auto &item : rs) {
      item.second.desc.ack_ts_en = false;
      if (!item.second.smem_issued)
        item.second.desc.wr_sharemem_flag = false;
    }
  }
  void ClearError(uint64_t mask) {
    if (mask & 1)
      first_error = error_ids = first_entry = 0;
  }

protected:
  void Step() override {
    // 末级先做：先退休已完成的全局队头，腾出表项。
    to_ts->Idle();
    retired->Idle();
    exception->valid = 0;
    CollectSmem();
    Report();
    Collect();
    Join();
    TakeAdmit();
    if (canceled && canceled->Valid()) {
      auto it = rs.find(canceled->commit_seq.Get());
      if (it != rs.end()) {
        it->second.joined = true;
        it->second.desc.ack_ts_en = false;
        it->second.desc.wr_sharemem_flag = false;
      }
    }

    if (!smem_used)
      smem_wr->IdleReq();
    joined = join_pending;
    reported = report_pending;
    used = rs.size();
    TracePerCycle("rs_used", rs.size());
    TracePerCycle("smem_outstanding", smem_flight.size());
  }

private:
  struct Entry {
    Descriptor desc;
    bool rd_ok = false, wr_ok = false;
    bool rd_drained = false, wr_drained = false;
    bool joined = false, smem_issued = false, smem_complete = false;
  };
  // 收 Commit 准入的任务，占一项。Join 排在它前面，所以这一拍腾出来的位置
  // 当拍就能用上。
  // ready 在收下这一拍的任务之后算，Commit 读到它时已经算上了这一笔。
  void TakeAdmit() {
    Admit();
    admit->DriveReady(HasRoom());
  }
  void Admit() {
    if (!admit->Valid() || admit->Seq() == last_admit_seq)
      return;
    auto d = admit->Desc();
    if (!d)
      return;
    last_admit_seq = admit->Seq();
    LOGCHECK(HasRoom(), "CompletionRs: 满了。Commit 应当先确认有空位。");
    LOGCHECK(rs.count(d->commit_seq) == 0,
             "CompletionRs: 同一个内部序号在未完成的上下文里重复占用。");
    Entry e;
    e.desc = *d;
    if (d->error)
      e.joined = true;
    rs[d->commit_seq] = e;
  }

  void Collect() {
    Take(*rd_done, kRd);
    Take(*wr_done, kWr);
    for (auto const &s : sources) {
      Take(*s.rd, kRd);
      Take(*s.wr, kWr);
    }
  }

  void Take(HalfDonePort const &p, uint64_t which) {
    if (!p.Valid())
      return;
    auto it = rs.find(p.commit_seq.Get());
    if (it == rs.end())
      return;
    if (p.error.Get() && !it->second.desc.error)
      it->second.desc.error = p.error.Get();
    bool drained = p.drained.Get() != 0;
    if (which == kRd) {
      it->second.rd_ok = true;
      it->second.rd_drained = drained;
    } else {
      it->second.wr_ok = true;
      it->second.wr_drained = drained;
    }
  }

  // 两侧排空后标记物理完成；全局队头按接纳顺序在 Report 中退休。
  void Join() {
    for (auto &kv : rs) {
      Entry &e = kv.second;
      if (!e.joined && e.rd_ok && e.wr_ok && e.rd_drained && e.wr_drained) {
        e.joined = true;
        ++join_pending;
      }
    }
  }

  void CollectSmem() {
    if (!(smem_wr->RspValid() || smem_wr->WriteRspValid()))
      return;
    LOGCHECK(!smem_flight.empty(), "CompletionRs: 无主ShareMem写响应。");
    uint64_t seq = smem_flight.front();
    smem_flight.pop_front();
    auto it = rs.find(seq);
    LOGCHECK(it != rs.end(), "CompletionRs: SM响应命中已释放Entry。");
    it->second.smem_complete = true;
    if (!it->second.desc.error &&
        (smem_wr->WriteRspValid() ? smem_wr->WriteRspError()
                                  : smem_wr->RspError()))
      it->second.desc.error = 7;
  }

  void Report() {
    smem_used = false;
    if (first_error || rs.empty())
      return;
    auto head = rs.begin();
    Entry const &head_entry = head->second;
    if (head_entry.joined && head_entry.desc.error) {
      first_error = head_entry.desc.error;
      first_entry = head_entry.desc.entry_id;
      if (!interrupt_mask)
        exception->Drive(head_entry.desc);
      error_ids = head_entry.desc.stream_id | (head_entry.desc.task_id << 4) |
                  (head_entry.desc.user_id << 10) | (head_entry.desc.path_id << 26);
      return;
    }
    // SM Ptr按接纳序扫描，允许旧写等待响应时继续发年轻的SM写。
    for (auto &kv : rs) {
      Entry &e = kv.second;
      if (!e.joined || e.desc.error)
        break;
      if (!e.desc.wr_sharemem_flag) {
        e.smem_complete = true;
        continue;
      }
      if (e.smem_issued)
        continue;
      if (!smem_wr->Ready())
        break;
      auto bytes = std::make_shared<ByteBlock>(128, 0);
      for (uint64_t k = 0; k < 4; ++k) {
        (*bytes)[k] = uint8_t(e.desc.smem_data >> (8 * k));
      }
      smem_wr->Write(e.desc.smem_addr, bytes, false, 0, 4);
      e.smem_issued = true;
      smem_flight.push_back(kv.first);
      smem_used = true;
      break;
    }
    Entry const &e = head->second;
    if (!e.joined || !e.smem_complete)
      return;
    if (e.desc.ack_ts_en && !e.desc.error) {
      to_ts->Drive(e.desc.stream_id, e.desc.task_id, e.desc.user_id);
      done_task = e.desc.task_id;
      done_user = e.desc.user_id;
      ++report_pending;
    } else {
      // 全局队头退休但不发 ACK 时，仍记录 DTE 任务的完成端点。
      ++noack_cnt;
      noack_task = e.desc.task_id;
      noack_user = e.desc.user_id;
    }
    retired->Drive(head->first, kWr, true);
    rs.erase(head);
  }

  struct Source {
    std::shared_ptr<HalfDonePort> rd, wr;
  };
  std::vector<Source> sources;
  std::shared_ptr<HalfDonePort> rd_done, wr_done;
  std::shared_ptr<MemPort> smem_wr;
  bool smem_used = false;
  std::shared_ptr<DonePort> to_ts;
  std::shared_ptr<AdmitPort> admit;
  uint64_t last_admit_seq = 0;

  std::map<uint64_t, Entry> rs;
  std::deque<uint64_t> smem_flight;
  std::shared_ptr<HalfDonePort> retired, canceled;
  uint64_t first_error = 0, error_ids = 0, first_entry = 0, interrupt_mask = 0;
  std::shared_ptr<DteExceptionPort> exception;
  uint64_t join_pending = 0, report_pending = 0;
  // 完成了但不报 TS 的笔数与最近一笔的身份，供 Core 层发 dsa_done_noack。
  uint64_t noack_cnt = 0, noack_task = 0, noack_user = 0;

  // 刚报到 TS 那一笔的身份，供 Core 层发波形。
  uint64_t done_task = 0, done_user = 0;

  Logic64 joined, reported, used;
};

} // namespace bach
} // namespace latch

#endif
