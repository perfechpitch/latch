#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_COMMIT_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_COMMIT_

// Commit owns all 16 logical queue entries until ordered retirement. Each
// channel dispatches independently; CompletionRs is the completion view of the
// same entries.

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bach/common/flit.h"
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/ip/chip/core/dte/hmem.h"
#include "bach/ip/module_base.h"
#include "base/log.h"

namespace latch {
namespace bach {

class Commit : public BachModule {
public:
  Commit(ClockPtr clock, const std::string &name, Hmem &tables,
         uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick), hmem(tables),
        from_rv(std::make_shared<DescPort>(clock)),
        to_rs(std::make_shared<AdmitPort>(clock)),
        retired(std::make_shared<HalfDonePort>(clock)),
        canceled(std::make_shared<HalfDonePort>(clock)), admitted(clock),
        stalled(clock), queued(clock) {}

  DescPort &FromRv() { return *from_rv; }
  std::shared_ptr<DescPort> FromRvPtr() const { return from_rv; }
  void RebindRv(std::shared_ptr<DescPort> p) { from_rv = std::move(p); }

  // dispatch 一笔后往这几个口上发：每个 Lane 一个，Completion RS 一个。
  void AddLanePort(std::shared_ptr<AdmitPort> p) {
    to_lane.push_back(std::move(p));
  }
  std::shared_ptr<AdmitPort> RsPortPtr() const { return to_rs; }

  uint64_t Admitted() const { return admitted.Get(); }
  // 过 dispatch 门槛（真正开始搬）的笔数与刚过那一笔的身份。所有任务都从 RV
  // core 的配置入口来，所以这里数的就是 DTE 的全部任务；之前还要在中央
  // TaskQueue 里等 通道资源 / VC credit。
  uint64_t RvAdmitted() const { return admit_cnt; }
  uint64_t StartTask() const { return start_task; }
  uint64_t StartUser() const { return start_user; }
  uint64_t Stalled() const { return stalled.Get(); }
  uint64_t Queued() const { return queued.Get(); }

  void AttachRetired(std::shared_ptr<HalfDonePort> p) {
    retired = std::move(p);
  }
  uint64_t Occupied() const { return central_q.size(); }
  void AttachEntryState(std::function<uint64_t(uint64_t)> read,
                        std::function<uint64_t(uint64_t)> error) {
    entry_state = std::move(read);
    entry_error = std::move(error);
  }
  void Resume() {
    running = true;
    stepping = false;
  }
  void Stop() { running = false; }
  void SingleStep() {
    running = true;
    stepping = true;
  }
  bool Running() const { return running; }
  void Cancel(uint64_t entry) {
    for (auto &d : central_q)
      if (d.entry_id == entry && !d.dispatched) {
        d.dispatched = true;
        cancellations.push_back(d.commit_seq);
        break;
      }
  }
  void Flush() {
    Stop();
    for (auto &d : central_q)
      if (!d.dispatched)
        Cancel(d.entry_id);
  }
  std::shared_ptr<HalfDonePort> CancelPtr() const { return canceled; }
  uint64_t ReadEntry(uint64_t address) const {
    if (address < 0x2000 || address >= 0x3000 || (address & 3))
      return 0;
    uint64_t entry = (address - 0x2000) / 256, off = address & 255;
    for (auto const &d : central_q)
      if (d.entry_id == entry) {
        if (off < 32) {
          uint64_t seg = off / 8;
          return off & 4 ? d.seg[seg].dst : d.seg[seg].src;
        }
        if (off < 48)
          return d.seg[(off - 32) / 4].stride;
        if (off < 64)
          return d.seg[(off - 48) / 4].len;
        if (off == 64)
          return d.smem_addr;
        if (off == 68)
          return d.smem_data;
        if (off == 72) {
          uint64_t mode = uint64_t(d.route) | (d.hw_header_op ? 128 : 0) |
                          (d.wr_sharemem_flag ? 256 : 0) |
                          (d.ack_ts_en ? 512 : 0);
          for (uint64_t i = 1; i < 4; ++i)
            if (d.seg[i].valid)
              mode |= 1ull << (i + 3);
          return mode;
        }
        if (off == 80)
          return d.stream_id;
        if (off == 84)
          return d.task_id;
        if (off == 88)
          return d.user_id;
        if (off == 92)
          return d.path_id;
        if (off == 96)
          return d.vc;
        if (off == 100)
          return (d.dispatched ? 2 : 1) |
                 (entry_state ? entry_state(d.commit_seq) : 0);
        if (off == 104)
          return entry_error ? entry_error(d.commit_seq) : d.error;
        if (off == 108)
          return d.dispatch_cycle ? d.dispatch_cycle - d.admitted_cycle : 0;
      }
    return 0;
  }

  bool Quiescent() const override {
    return central_q.empty() && cancellations.empty();
  }

protected:
  void Step() override {
    // 先收新任务进中央 TaskQueue，再从队头 dispatch 一笔，最后把 dispatch
    // 出的那 一笔集中发出去（端口每拍必须驱动一次，散在几处写会互相盖掉）。
    to_rs->Idle();
    canceled->Idle();
    if (!cancellations.empty()) {
      canceled->Drive(cancellations.front(), kWr, true);
      cancellations.pop_front();
    }
    RetireEntry();
    TakeFromRv();
    TryDispatch();
    Deliver();

    admitted = admit_pending;
    stalled = stall_pending;
    from_rv->DriveCredits(kCentralTaskQDepth - central_q.size());
    queued = central_q.size();
    TracePerCycle("admitted", admit_pending);
    TracePerCycle("queued", central_q.size());
  }

private:
  void RetireEntry() {
    if (!retired->Valid() || retired->commit_seq.Get() == last_retire_seq)
      return;
    last_retire_seq = retired->commit_seq.Get();
    LOGCHECK(!central_q.empty() &&
                 central_q.front().commit_seq == last_retire_seq,
             "Commit: 退休必须命中全局队头。");
    central_q.pop_front();
  }

  void TakeFromRv() {
    if (!from_rv->Valid()) {
      from_rv->DriveAccepted(false);
      return;
    }
    if (from_rv->Seq() == last_rv_seq) {
      from_rv->DriveAccepted(true);
      return;
    }
    auto d = from_rv->Desc();
    if (!d) {
      from_rv->DriveAccepted(false);
      return;
    }
    if (central_q.size() >= kCentralTaskQDepth || !to_rs->Ready()) {
      from_rv->DriveAccepted(false);
      return;
    }
    // 快照一份进中央 TaskQueue，走归约路径的出核任务在这里标成 reduce 包。
    Descriptor nd = *d;
    nd.commit_seq = ++admit_seq;
    nd.admitted_cycle = CycleNow();
    nd.entry_id = (nd.commit_seq - 1) % kCentralTaskQDepth;
    MarkReducePkt(nd);
    central_q.push_back(nd);
    to_rs->Drive(std::make_shared<Descriptor>(nd), nd.commit_seq);
    last_rv_seq = from_rv->Seq();
    from_rv->DriveAccepted(true);
  }

  // 从队头往后扫，dispatch 第一个目标通道就绪的任务。不同通道的任务可乱序下发：
  // 队头那个任务堵在通道资源上时，后面别的通道的任务可以先行。同一通道内按序：
  // 一个通道有任务没发出去，这一拍排在它后面、同一通道的任务都不发。三样一起拿：
  // 读侧 TaskQueue、写侧 TaskQueue、Completion RS。
  void TryDispatch() {
    if (!running)
      return;
    auto skip = just_sent;
    just_sent.fill(false);
    uint64_t blocked = 0;
    bool waiting = false, progress = false;
    for (auto &d : central_q) {
      if (d.dispatched)
        continue;
      if (d.error) {
        d.dispatched = true;
        continue;
      }
      uint64_t lane = d.Lane();
      if (lane >= to_lane.size() || (blocked & (1ull << lane)))
        continue;
      waiting = true;
      if (skip[lane] || held[lane] || !to_lane[lane]->Ready()) {
        blocked |= 1ull << lane;
        continue;
      }
      d.dispatch_cycle = CycleNow();
      auto desc = std::make_shared<Descriptor>(d);
      start_task = d.task_id;
      start_user = d.user_id;
      if (!desc->msg && !IsInbound(desc->route))
        MakeOutboundMsg(*desc);
      held[lane] = Held{std::move(desc), lane};
      blocked |= 1ull << lane;
      d.dispatched = true;
      ++admit_cnt;
      ++admit_pending;
      progress = true;
      if (stepping) {
        Stop();
        break;
      }
    }
    if (waiting && !progress)
      ++stall_pending;
  }

  // 走归约路径出核的包，包头的 reduce_seq 打上发方的 task_id，Reduce Done
  // 原样带回。各核任务链各自编号（pcode），这个号不要求相同。归约按 user_id 与
  // path 把各路输入收成一笔，不看这个号。
  void MarkReducePkt(Descriptor &d) {
    if (d.route != Route::kCmToRouter && d.route != Route::kMmToRouter)
      return;
    if (hmem.Rtab(d.path_id).operation == Operation::kForward)
      return;
    d.reduce_seq = DtePcodeFormat() ? d.path_id : d.task_id;
  }

  // 出核任务要发出去的那个包在这里造好，读回来的数据往它的 payload 里填。
  // 出核按文档“重组包头”改写 path_id、user_id、loopback[3:2](vc)、pkt_length
  // 四个字段（详细设计 3.15/66），size 由各使能数据段长度算出。片外那一段的
  // 目的标识（dst）跟着 path_id 的表项走。
  void MakeOutboundMsg(Descriptor &d) const {
    auto m = std::make_shared<Message>();
    m->user_id = d.user_id;
    m->path_id = d.path_id;
    m->dst = hmem.Rtab(d.path_id).ext_dst;
    // 收方按这一项把包搬进它的存储。软件配 DTE 数据段的 CFG_ADDRi_DST
    // 时配的就是 收方那一侧的落点，本地这一笔用不着它。
    m->dst_addr = d.DataDst();
    m->size = d.PayloadBytes();
    m->payload_aux = DtePayloadAuxBytes();
    m->scale_valid = d.HasScale();
    m->topk_valid = d.HasTopk();
    m->payload_contains_topk = true;
    m->vc = d.vc;
    m->stream_id = d.stream_id;
    m->task_id = d.task_id;
    m->seq = d.commit_seq;
    m->reduce_seq = d.reduce_seq;
    m->payload.assign(d.PayloadBytes(), 0);
    d.msg = m;
  }

  // 发出去，直到两边都收下。一次握手最少两拍，接收方按序号认。两边的 ready 是
  // 收下这一拍的请求之后算的，TryDispatch 发一笔之后又空一拍，所以读到 ready 时
  // 发过的每一笔都已算进去，发一拍就算送到。
  void Deliver() {
    for (uint64_t lane = 0; lane < to_lane.size(); ++lane) {
      if (held[lane]) {
        to_lane[lane]->Drive(held[lane]->desc, held[lane]->desc->commit_seq);
        held[lane].reset();
        just_sent[lane] = true;
      } else
        to_lane[lane]->Idle();
    }
  }

  std::function<uint64_t(uint64_t)> entry_state, entry_error;
  Hmem &hmem;
  std::shared_ptr<DescPort> from_rv;
  std::vector<std::shared_ptr<AdmitPort>> to_lane;
  std::shared_ptr<AdmitPort> to_rs;
  std::shared_ptr<HalfDonePort> retired, canceled;
  std::deque<uint64_t> cancellations;
  bool running = false, stepping = false;
  uint64_t last_retire_seq = 0;
  // 一笔已 dispatch、正在交付的任务：发给目标 Lane 与 Completion
  // RS，等双方收下。
  struct Held {
    std::shared_ptr<Descriptor> desc;
    uint64_t lane = 0;
  };
  std::array<std::optional<Held>, kLaneNum> held;
  std::array<bool, kLaneNum> just_sent{}; // 上一拍发过一笔
  uint64_t admit_seq = 0;

  // Full lifecycle descriptor view; completion never adds queue capacity.
  std::deque<Descriptor> central_q;
  uint64_t last_rv_seq = 0;
  uint64_t admit_pending = 0, stall_pending = 0;
  // 过 dispatch 门槛的笔数与刚过的那一笔的身份，供 Core 层发波形。
  uint64_t admit_cnt = 0, start_task = 0, start_user = 0;

  Logic64 admitted, stalled, queued;
};

} // namespace bach
} // namespace latch

#endif
