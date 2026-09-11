#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_COMMIT_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_COMMIT_

// Commit：两个任务入口在这里汇成同一套内部任务模型。
//
// 一个高层任务必须同时拿到三样才接纳：目标通道读侧的 TaskQueue 项、写侧的
// TaskQueue 项、Completion RS 项。任一侧没有空间时整体保持，Header 入口向 Router
// 反压。这条规则挡住「读已经开始、写还没有落脚点」的半任务。
//
// 两个配置 Bank，Bank0 优先于 Bank1：都空闲时 Router 的配置进 Bank0、RV core 的
// 配置进 Bank1；只剩一个 Bank 而两者竞争时优先配置 Router 信息。
//
// PendingTaskQ 排在 Commit 之前：RV core 配好一个出核任务后，先按 path_id 查出
// 走哪个 VC 与资源需求，credit 不够的进 PendingTaskQ 等，够了才来 Commit 申请
// 那三样。等 credit 的任务因此不占 TaskQueue 项，也不占 Completion RS 项。
// 这是「资源的持有与等待不成环」在 DTE 上的落点。

#include <deque>
#include <map>
#include <vector>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/common/flit.h"
#include "bach/ip/chip/core/dte/hmem.h"
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
        from_parser(std::make_shared<DescPort>(clock)),
        from_rv(std::make_shared<DescPort>(clock)),
        to_rs(std::make_shared<AdmitPort>(clock)),
        admitted(clock),
        stalled(clock),
        pending_len(clock) {}

  std::shared_ptr<DescPort> ParserPortPtr() const { return from_parser; }
  // 装配层把 Parser 那一侧的口接过来：一根线两端是同一个对象。
  void RebindParser(std::shared_ptr<DescPort> p) { from_parser = std::move(p); }
  DescPort& FromRv() { return *from_rv; }
  std::shared_ptr<DescPort> FromRvPtr() const { return from_rv; }
  void RebindRv(std::shared_ptr<DescPort> p) { from_rv = std::move(p); }

  // Xbar 每拍发布各方向各 VC 还发不发得出，出核任务发数据之前读它。
  void AttachVcLevel(std::shared_ptr<CreditLevelPort> p) {
    vc_level = std::move(p);
  }

  // 准入一笔后往这几个口上发：每个 Lane 一个，Completion RS 一个。
  void AddLanePort(std::shared_ptr<AdmitPort> p) {
    to_lane.push_back(std::move(p));
  }
  std::shared_ptr<AdmitPort> RsPortPtr() const { return to_rs; }

  uint64_t Admitted() const { return admitted.Get(); }
  uint64_t Stalled() const { return stalled.Get(); }
  uint64_t PendingLen() const { return pending_len.Get(); }

  bool Quiescent() const override { return pending.empty(); }

 protected:
  void Step() override {
    // 一拍只准入一笔，所以这三步的先后就是两个 Bank 的优先级：只剩一个 Bank
    // 而两侧都要时先配 Router 的那一笔。TakeFromRv 只是把任务放进 PendingTaskQ
    // 等 credit，不占准入的名额，排在最后。
    TakeFromParser();
    TryAdmitPending();
    TakeFromRv();
    // 准入的那一笔集中在这里发出去：端口每拍必须驱动一次，散在几处写会互相
    // 盖掉。同线程同拍两次写同一个 Latch 不触发断言。
    Deliver();

    admitted = admit_pending;
    stalled = stall_pending;
    pending_len = pending.size();
    TracePerCycle("admitted", admit_pending);
    TracePerCycle("pending", pending.size());
  }

 private:
  void TakeFromParser() {
    if (!from_parser->Valid()) {
      from_parser->DriveAccepted(false);
      return;
    }
    if (from_parser->Seq() == last_parser_seq) {
      from_parser->DriveAccepted(true);
      return;
    }
    auto d = from_parser->Desc();
    if (!d) {
      from_parser->DriveAccepted(false);
      return;
    }
    // Bank0 优先：Router 的配置总是先进。
    if (!TryAdmit(*d)) {
      ++stall_pending;
      from_parser->DriveAccepted(false);
      return;
    }
    last_parser_seq = from_parser->Seq();
    from_parser->DriveAccepted(true);
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
    if (pending.size() >= kPendingTaskQDepth) {
      from_rv->DriveAccepted(false);
      return;
    }
    // 出核任务先进 PendingTaskQ 等 credit，够了才去 Commit 申请那三样。
    Descriptor nd = *d;
    MarkReducePkt(nd);
    pending.push_back(nd);
    last_rv_seq = from_rv->Seq();
    from_rv->DriveAccepted(true);
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

  void TryAdmitPending() {
    if (pending.empty()) return;
    Descriptor const& d = pending.front();
    // 发数据之前实时检查这条 VC 通路上的 flit credit。下游 Stream 资源与本级
    // Rmem 资源不在这里查，TS 下发之前已经申请到；Reduce 包也一样。
    RouteEntry const& e = hmem.Rtab(d.path_id);
    if (!VcOk(d, e)) return;
    if (!TryAdmit(d)) {
      ++stall_pending;
      return;
    }
    pending.pop_front();
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
    // 收方按这一项把包搬进它的存储。软件配 DTE 模板的 dst_addr 时配的就是收方
    // 那一侧的落点，本地这一笔用不着它。
    m->dst_addr = d.dst_addr;
    HmemEntry const& h = hmem.Entry(d.stream_id);
    m->gpu_id = h.gpu_id;
    m->token_id = h.token_id;
    m->size = d.bytes;
    m->vc = d.vc;
    m->stream_id = d.stream_id;
    m->task_id = d.task_id;
    m->reduce_seq = d.reduce_seq;
    m->payload.assign(d.bytes, 0);
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

  // 三样一起拿：读侧 TaskQueue、写侧 TaskQueue、Completion RS。三样都是别的
  // 模块的队列，所以问的是它们上一拍报的 ready，那是「下一拍一定收得下」的
  // 承诺，往它们队列里放东西的只有本模块一家，承诺到下一拍仍然成立。
  bool TryAdmit(Descriptor const& d) {
    if (holding) return false;
    uint64_t lane = d.Lane();
    if (lane >= to_lane.size()) return false;
    // Lane 的 ready 已经把读写两侧的 TaskQueue 都算进去了。
    if (!to_lane[lane]->Ready() || !to_rs->Ready()) return false;
    // 同时完成地址展开：源地址、目的地址、按任务边界切分的元数据都在这一步算好。
    held = std::make_shared<Descriptor>(d);
    held_lane = lane;
    holding = true;
    ++admit_seq;
    // 内部序号：两个入口来的任务在这里汇成同一套编号，Completion RS 按它 Join。
    held->commit_seq = admit_seq;
    // 进核那一笔把 DPU 那一对包头记进 Hmem，出核造包时取回来。
    if (held->msg && IsInbound(held->route)) {
      HmemEntry& h = hmem.Entry(held->stream_id);
      h.gpu_id = held->msg->gpu_id;
      h.token_id = held->msg->token_id;
    }
    if (!held->msg && !IsInbound(held->route)) MakeOutboundMsg(*held);
    ++admit_pending;
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
  std::shared_ptr<CreditLevelPort> vc_level;
  std::shared_ptr<DescPort> from_parser, from_rv;
  std::vector<std::shared_ptr<AdmitPort>> to_lane;
  std::shared_ptr<AdmitPort> to_rs;
  std::shared_ptr<Descriptor> held;
  bool holding = false;
  uint64_t held_lane = 0, admit_seq = 0;

  std::deque<Descriptor> pending;
  // 每个 stream 当前这笔 reduce task 下一包该打几号。
  uint64_t last_parser_seq = 0, last_rv_seq = 0;
  uint64_t admit_pending = 0, stall_pending = 0;

  Logic64 admitted, stalled, pending_len;
};

}  // namespace bach
}  // namespace latch

#endif
