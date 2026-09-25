#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_COMMIT_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_COMMIT_

// Commit：中央 TaskQueue + 按通道资源 dispatch。
//
// 对齐飞书《DTE DSA》：所有任务（进核 + 出核）统一从 RV core 的配置入口来。RV core
// 配好寄存器写 CFG_TRIGGER 后，Regfile 把快照出的 Descriptor 送进中央 TaskQueue
// （深度 16，保存「已快照、尚未 dispatch」的完整 TaskDesc，不同通道的任务可乱序
// 下发）；Commit 再按目标通道的读/写 TaskQueue 槽、Completion RS 槽、出核的 VC
// credit 从队头 dispatch。
//
// 「三样一起拿」的语义从 Fire 时刻移到 dispatch 时刻：dispatch 时检查目标 Lane 的
// 读侧 + 写侧 TaskQueue 项与 Completion RS 项，任一侧没有空间就整体保持（挡住
// 「读已开始、写没落脚点」的半任务）；出核任务还要先看这条 VC 通路发不发得出。
//
// 反压：中央 TaskQueue 满（16）就不收 Regfile 送来的 Descriptor，Regfile 据此拉低
// dsa_cfg 的 req_ready 反压 RV core。

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/common/flit.h"
#include "bach/ip/chip/core/dte/hmem.h"
#include "bach/ip/chip/core/mu/gen_ep_info.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class Commit : public BachModule {
 public:
  Commit(ClockPtr clock, const std::string& name, Hmem& tables,
         uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        hmem(tables),
        from_rv(std::make_shared<DescPort>(clock)),
        to_rs(std::make_shared<AdmitPort>(clock)),
        admitted(clock),
        stalled(clock),
        queued(clock) {}

  DescPort& FromRv() { return *from_rv; }
  std::shared_ptr<DescPort> FromRvPtr() const { return from_rv; }
  void RebindRv(std::shared_ptr<DescPort> p) { from_rv = std::move(p); }

  // Xbar 每拍发布各方向各 VC 还发不发得出，出核任务 dispatch 之前读它。
  void AttachVcLevel(std::shared_ptr<CreditLevelPort> p) {
    vc_level = std::move(p);
  }

  // 出核造包时从 MU 的 topK_ep_table 把 topK 读出来附回要发的包。装配层把 MU 的
  // GenEpInfo 指过来。
  void AttachMuTopkEp(GenEpInfo* ep) { mu_topk_ep = ep; }

  // dispatch 一笔后往这几个口上发：每个 Lane 一个，Completion RS 一个。
  void AddLanePort(std::shared_ptr<AdmitPort> p) {
    to_lane.push_back(std::move(p));
  }
  std::shared_ptr<AdmitPort> RsPortPtr() const { return to_rs; }

  uint64_t Admitted() const { return admitted.Get(); }
  // 过 dispatch 门槛（真正开始搬）的笔数与刚过那一笔的身份。所有任务都从 RV core
  // 的配置入口来，所以这里数的就是 DTE 的全部任务；之前还要在中央 TaskQueue 里等
  // 通道资源 / VC credit。
  uint64_t RvAdmitted() const { return admit_cnt; }
  uint64_t StartTask() const { return start_task; }
  uint64_t StartUser() const { return start_user; }
  uint64_t Stalled() const { return stalled.Get(); }
  uint64_t Queued() const { return queued.Get(); }

  bool Quiescent() const override { return central_q.empty() && !holding; }

 protected:
  void Step() override {
    // 先收新任务进中央 TaskQueue，再从队头 dispatch 一笔，最后把 dispatch 出的那
    // 一笔集中发出去（端口每拍必须驱动一次，散在几处写会互相盖掉）。
    TakeFromRv();
    TryDispatch();
    Deliver();

    admitted = admit_pending;
    stalled = stall_pending;
    queued = central_q.size();
    TracePerCycle("admitted", admit_pending);
    TracePerCycle("queued", central_q.size());
  }

 private:
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
    if (central_q.size() >= kCentralTaskQDepth) {
      from_rv->DriveAccepted(false);
      return;
    }
    // 快照一份进中央 TaskQueue，走归约路径的出核任务在这里标成 reduce 包。
    Descriptor nd = *d;
    MarkReducePkt(nd);
    central_q.push_back(nd);
    last_rv_seq = from_rv->Seq();
    from_rv->DriveAccepted(true);
  }

  // 从队头往后扫，dispatch 第一个目标通道就绪的任务。不同通道的任务可乱序下发：
  // 队头那个出核任务还堵在 VC credit 上时，后面别的通道的任务可以先行。同一通道
  // 内按序（F18）：一个通道有任务没发出去，这一拍排在它后面、同一通道的任务都
  // 不发。三样一起拿：读侧 TaskQueue、写侧 TaskQueue、Completion RS；出核任务再
  // 查 VC credit。
  void TryDispatch() {
    if (holding) return;
    if (central_q.empty()) return;
    uint64_t blocked = 0;  // 这一拍已经有任务没发出去的通道，按位记
    for (auto it = central_q.begin(); it != central_q.end(); ++it) {
      Descriptor const& d = *it;
      uint64_t lane = d.Lane();
      if (lane >= to_lane.size()) continue;
      if (blocked & (1ull << lane)) continue;

      // 出核任务 dispatch 之前实时检查这条 VC 通路上的 flit credit。下游 Stream
      // 资源与本级 Rmem 资源不在这里查，TS 下发之前已经申请到；Reduce 包也一样。
      // 进核任务不走这条通路，不查。
      if (!IsInbound(d.route)) {
        RouteEntry const& e = hmem.Rtab(d.path_id);
        if (!VcOk(d, e)) {
          blocked |= 1ull << lane;
          continue;
        }
      }

      // Lane 的 ready 已经把读写两侧的 TaskQueue 都算进去了。
      if (!to_lane[lane]->Ready() || !to_rs->Ready()) {
        blocked |= 1ull << lane;
        continue;
      }

      // 三样齐了才真正 dispatch：地址展开已在 Regfile 的 Fire 里算好。
      held = std::make_shared<Descriptor>(d);
      held_lane = lane;
      holding = true;
      ++admit_seq;
      // 内部序号：所有任务在这里汇成同一套编号，Completion RS 按它 Join。
      held->commit_seq = admit_seq;
      // 出核任务要发出去的那个包在这里造好（进核任务的包头已由 Header Parser 记进
      // Hmem，这里不再写）。身份要在 erase 之前取，d 就指着要 dispatch 的那一项。
      start_task = d.task_id;
      start_user = d.user_id;
      if (!held->msg && !IsInbound(held->route)) MakeOutboundMsg(*held);
      ++admit_cnt;
      ++admit_pending;
      central_q.erase(it);
      return;
    }
    // 走到这里：队列非空但一个都 dispatch 不出去，记一次 stall。
    ++stall_pending;
  }

  // 走归约路径出核的包标成 reduce 包（F74），包头的 reduce_seq 打上发方的
  // task_id：ReduceModule 靠它分开同一个用户前后两笔 reduce 任务。一条归约链上
  // 各 core 的任务链一样，同一笔任务的 task_id 也一样。
  void MarkReducePkt(Descriptor& d) {
    if (d.route != Route::kCmToRouter && d.route != Route::kMmToRouter) return;
    if (hmem.Rtab(d.path_id).operation == Operation::kForward) return;
    d.reduce_pkt = true;
    d.reduce_seq = d.task_id;
  }

  // 出核任务要发出去的那个包在这里造好，读回来的数据往它的 payload 里填。
  // 出核改写的三个包头字段（F44）：path_id 用 TS 送来的那个，size 用 RV core
  // 配的寄存器，core_mask 只在做 MoE Route 时改。片外那一段的目的标识跟着
  // path_id 的表项走。
  void MakeOutboundMsg(Descriptor& d) const {
    auto m = std::make_shared<Message>();
    m->user_id = d.user_id;
    m->path_id = d.path_id;
    m->dst = hmem.Rtab(d.path_id).ext_dst;
    // 收方按这一项把包搬进它的存储。软件配 DTE 数据段的 CFG_ADDRi_DST 时配的就是
    // 收方那一侧的落点，本地这一笔用不着它。
    m->dst_addr = d.DataDst();
    HmemEntry const& h = hmem.Entry(d.stream_id);
    m->gpu_id = h.gpu_id;
    m->token_id = h.token_id;
    // 带 scale 的包：数据后面接 scale，包长把 payload 各段都算上。
    m->size = d.PayloadBytes();
    m->scale_valid = d.HasScale() ? 1 : 0;
    // 带 topK 的包（B core 广播那一笔）：从 MU 的 topK_ep_table 按段内偏移取这一
    // 份原样附回，收方 DTE 再按 stream_id 写进它的 MU。
    if (d.HasTopk() && mu_topk_ep) {
      m->topk_valid = 1;
      m->topk = mu_topk_ep->TopkBytes(d.TopkIndex());
    }
    m->vc = d.vc;
    m->stream_id = d.stream_id;
    m->task_id = d.task_id;
    m->reduce_seq = d.reduce_seq;
    m->payload.assign(m->size, 0);
    d.msg = m;
  }

  // 这一笔要往哪几个方向发，那几个方向的这个 VC 都还发得出才算够。
  bool VcOk(Descriptor const& d, RouteEntry const& e) const {
    if (!vc_level) return true;
    for (uint64_t o = 0; o < kR2RNum; ++o) {
      if (((e.flow_dir >> o) & 1u) == 0) continue;
      if (!vc_level->VcOk(o, d.vc)) return false;
    }
    return true;
  }

  // 发出去，直到两边都收下。一次握手最少两拍，接收方按序号认。
  void Deliver() {
    for (uint64_t i = 0; i < to_lane.size(); ++i) {
      if (holding && i == held_lane) {
        to_lane[i]->Drive(held, admit_seq);
      } else {
        to_lane[i]->Idle();
      }
    }
    if (holding) {
      to_rs->Drive(held, admit_seq);
      // 两边都是「下一拍一定收得下」，所以发一拍就算送到。
      holding = false;
      held = std::shared_ptr<Descriptor>();
      return;
    }
    to_rs->Idle();
  }

  Hmem& hmem;
  GenEpInfo* mu_topk_ep = nullptr;
  std::shared_ptr<CreditLevelPort> vc_level;
  std::shared_ptr<DescPort> from_rv;
  std::vector<std::shared_ptr<AdmitPort>> to_lane;
  std::shared_ptr<AdmitPort> to_rs;
  std::shared_ptr<Descriptor> held;
  bool holding = false;
  uint64_t held_lane = 0, admit_seq = 0;

  // 中央 TaskQueue：保存「已快照、尚未 dispatch」的完整 TaskDesc。
  std::deque<Descriptor> central_q;
  uint64_t last_rv_seq = 0;
  uint64_t admit_pending = 0, stall_pending = 0;
  // 过 dispatch 门槛的笔数与刚过的那一笔的身份，供 Core 层发波形。
  uint64_t admit_cnt = 0, start_task = 0, start_user = 0;

  Logic64 admitted, stalled, queued;
};

}  // namespace bach
}  // namespace latch

#endif
