#ifndef _LATCH_BACH_IP_CHIP_CORE_ROUTER_ROUTER_STATION_
#define _LATCH_BACH_IP_CHIP_CORE_ROUTER_ROUTER_STATION_

// RouterStation：一个输入方向的入口站。四个实例，left / right / mid / core。
//
// 两级：
//   M1 Input VC Buffer  上游来的 flit 按包头的 VC 号写进对应 VC Buffer。该 VC 的
//                       private 未满就占 private，满了就占本方向的 shared pool。
//                       四个 VC 的 private 互不侵占，一个 VC 堵住不影响其他 VC。
//   M2 RC 查表          队首 flit 按 path_id 查本地 RouterTable 副本，建路由与资源
//                       上下文：出方向、下一跳 VC、进不进本 core、要不要查 stream
//                       credit、是不是 reduce、stall_way
//
// 同 VC 保序：VC Buffer 是 FIFO，同 VC 内 flit 严格按到达顺序读出，资源检查与仲裁
// 都不重排。跨 VC、跨 input port 之间不保证顺序。
//
// 队首在四个 VC 之间轮询选一个交给 Xbar。Xbar 每拍发布它那个入口还收不收得下，
// 收得下就发，发了当场出队并归还 VC credit —— 等授予的话一笔要占两拍，一个方向
// 的吞吐就只剩每两拍一个 flit。credit 不足的 VC 被跳过，同一个 input port 的其他
// VC 不受影响。
//
// 不派角色的 core 上只走直通：数据走完整流水线但不投递本 core，不检查 credit、
// 不支持阻塞重发。

#include <array>
#include <deque>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class RouterStation : public BachModule {
 public:
  RouterStation(ClockPtr clock, const std::string& name, uint64_t in_dir,
                RouterTable& table, uint64_t copy_idx, uint64_t parent = 0,
                bool tick = true)
      : BachModule(clock, name, parent, tick),
        tag(name),
        dir(in_dir),
        rtab(table),
        copy(copy_idx),
        up(std::make_shared<LinkEnd>(clock)),
        up_back(std::make_shared<LinkEnd>(clock)),
        req(std::make_shared<XbarReqPort>(clock)),
        level(std::make_shared<CreditLevelPort>(clock)),
        occupancy(clock),
        forwarded(clock),
        dropped(clock) {}

  // 上游来的数据与 release。
  LinkEnd& Up() { return *up; }
  void AttachUp(LinkEndPtr wire) { up = std::move(wire); }
  // 回给上游的那根线，本站只往它写 vc_release。
  LinkEnd& UpBack() { return *up_back; }
  void AttachUpBack(LinkEndPtr wire) { up_back = std::move(wire); }
  // 本方向 VC Buffer 的两级深度：每个 VC 的 private，与四个 VC 共用的 shared
  // pool。跑起来之前配。
  void SetVcDepth(VcDepth const& priv, uint64_t shared) {
    priv_depth = priv;
    shared_depth = shared;
  }

  XbarReqPort& Req() { return *req; }
  std::shared_ptr<XbarReqPort> ReqPtr() const { return req; }
  CreditLevelPort& Level() { return *level; }
  void AttachLevel(std::shared_ptr<CreditLevelPort> wire) {
    level = std::move(wire);
  }

  // 只透传的 core：不投递本 core、不检查 credit、不支持阻塞重发。
  void SetPassThrough(bool on) { pass_through = on; }

  uint64_t Occupancy() const { return occupancy.Get(); }
  uint64_t Forwarded() const { return forwarded.Get(); }
  // 停钟之后取的那一份：Logic64 只在协程里读得到当拍的值。
  uint64_t FwdCount() const { return forwarded_pending; }
  uint64_t QueueDepth() const {
    uint64_t n = 0;
    for (auto const& q : vc_buf) n += q.size();
    return n;
  }

  bool Quiescent() const override {
    for (auto const& q : vc_buf) {
      if (!q.empty()) return false;
    }
    return true;
  }

 protected:
  void Step() override {
    // 末级先做：先把队首交出去、把腾出的 VC 槽还给上游，再收本拍新到的。
    IssueRequest();
    AcceptFlit();

    uint64_t occ = 0;
    for (auto const& q : vc_buf) occ += q.size();
    occupancy = occ;
    forwarded = forwarded_pending;
    dropped = dropped_pending;
    TracePerCycle("occupancy", occ);
    TracePerCycle("forwarded", forwarded_pending);
  }

 private:
  // 报错时说清是谁：波形层次那一串名字。
  std::string tag;

  struct Slot {
    FlitView f;
    bool used_shared = false;
  };

  // M1：按包头的 VC 号写进对应 VC Buffer，private 满了借 shared。
  void AcceptFlit() {
    FlitView f = ReadFlit(up->flit);
    if (!f.valid) return;
    uint64_t v = f.vc;
    LOGCHECK(v < kVcNum, "RouterStation: 包头的 VC 号越界。");
    bool priv_full = private_used[v] >= priv_depth[v];
    if (priv_full && shared_used >= shared_depth) {
      // private 与 shared 都满。上游按 credit 发，一个方向的 credit 总量正好
      // 等于这里的容量，所以到这一步说明 credit 记错了。
      spdlog::error("{} 入方向 {} 的 VC{} 收不下了：private {} shared {}", tag,
                    dir, v, private_used[v], shared_used);
      LOGCHECK(false, "RouterStation: VC Buffer 满了，上游超发。");
    }
    Slot s;
    s.f = f;
    s.used_shared = priv_full;
    if (priv_full) {
      ++shared_used;
    } else {
      ++private_used[v];
    }
    vc_buf[v].push_back(s);
  }

  // M2：队首查表建上下文，在四个 VC 之间轮询挑一个交给 Xbar。
  //
  // Xbar 那一侧上一拍说收得下就发，发了当场出队并把 VC 槽还给上游：等授予的话
  // 一笔要占两拍，一个方向的吞吐就砍掉一半。
  void IssueRequest() {
    if (!req->Room()) {
      // 下游入口缓冲快满了，本拍不发，队列原地不动。
      req->IdleReq();
      up_back->flit.Idle();
      up_back->release.Idle();
      return;
    }
    for (uint64_t k = 1; k <= kVcNum; ++k) {
      uint64_t v = (rr_last + k) % kVcNum;
      if (vc_buf[v].empty()) continue;
      FlitView const& f = vc_buf[v].front().f;
      if (!f.msg) continue;
      RouteEntry const& e = rtab.Lookup(copy, f.msg->path_id);
      if (!e.valid) {
        // 配置写入前不投递任何包。
        continue;
      }
      uint64_t mask = OutMaskOf(e, *f.msg);
      if (mask == 0) {
        // 开了 path_core_mask 的那一档：这条 path 走遍一串 core，落在哪几个由
        // 包头的 mask 挑，走到既没人要又没有下游的那一跳，这一笔就是走到头了，
        // 在这里吃掉。没开的那一档进不进核与包无关，那时候没有出口是表项配错。
        LOGCHECK(e.path_core_mask_enable,
                 "RouterStation: 表项既不进核也没有出方向。");
        ++dropped_pending;
        Drop(v);
        return;
      }
      req->valid = 1;
      req->out_mask = mask;
      req->vc = NextVcOf(e, mask, f.vc);
      req->head = f.head ? 1 : 0;
      req->tail = f.tail ? 1 : 0;
      req->bytes = f.bytes;
      req->path_id = f.msg->path_id;
      req->user_id = f.msg->user_id;
      req->enters_core = (!pass_through && e.EntersCore(f.msg->path_core_mask))
                             ? 1 : 0;
      req->stall_way = (!pass_through && e.stall_way) ? 1 : 0;
      req->need_stream = (!pass_through && e.stream_table_enable) ? 1 : 0;
      req->whole_packet = HasWholePacket(v) ? 1 : 0;
      req->credit_require = pass_through ? 0 : RequireOf(e, mask);
      req->msg = f.msg;
      req->seq = ++req_seq;
      rr_last = v;
      // flit 离开本级 VC Buffer 就归还 VC credit，走共享总线，一拍最多一个 VC。
      Slot s = vc_buf[v].front();
      vc_buf[v].pop_front();
      if (s.used_shared) {
        --shared_used;
      } else {
        --private_used[v];
      }
      ++forwarded_pending;
      up_back->flit.Idle();
      up_back->release.Drive(true, v, false, 0, false, 0);
      return;
    }
    req->IdleReq();
    up_back->flit.Idle();
    up_back->release.Idle();
  }

  // 走到头的那一笔：出队，把 VC 槽还给上游，这一拍不发请求。一拍吃一个 flit。
  void Drop(uint64_t v) {
    Slot s = vc_buf[v].front();
    vc_buf[v].pop_front();
    if (s.used_shared) {
      --shared_used;
    } else {
      --private_used[v];
    }
    rr_last = v;
    req->IdleReq();
    up_back->flit.Idle();
    up_back->release.Drive(true, v, false, 0, false, 0);
  }

  // 这一笔在下游要占多少 credit。多个出方向时取第一个置位方向那一份 —— 多播
  // 在各方向上的额度由编译侧填成一样的。
  static uint64_t RequireOf(RouteEntry const& e, uint64_t mask) {
    if (mask & (1ull << kOutMid)) return e.nxt_credit_require[0];
    if (mask & (1ull << kOutLeft)) return e.nxt_credit_require[1];
    if (mask & (1ull << kOutRight)) return e.nxt_credit_require[2];
    return 0;
  }

  // 这个 VC 的队列里从队首起有没有一整个包：能一直读到带 tail 的那一拍就算有。
  // 交给 Xbar 做仲裁的第二档。
  bool HasWholePacket(uint64_t v) const {
    for (auto const& s : vc_buf[v]) {
      if (s.f.tail) return true;
    }
    return false;
  }

  // 出方向掩码。
  //
  // 进不进 ReduceModule 由 operation 决定，不由 flow_dir：flow_dir 管的是这条
  // path 从本级「往哪几个方向发」，是归约算完之后的事；operation 管的是本级在
  // 这条 path 上的角色。两者混用会成环 —— 归约结果回注时用同一个 path_id 查表，
  // 若按 flow_dir 判就会被再次送进 ReduceModule。
  //
  // 本级要做归约时这一笔只进 ReduceModule，不同时往下游发；下游那一段由
  // ReduceModule 收齐后按 flow_dir 自己发。
  uint64_t OutMaskOf(RouteEntry const& e, Message const& m) const {
    if (!pass_through && e.operation != Operation::kForward &&
        e.reduce_in_mask != 0) {
      return 1ull << (kOutReduce0 + ReduceLaneOf(e));
    }
    uint64_t mask = 0;
    if (e.flow_dir & kFlowMid) mask |= 1ull << kOutMid;
    if (e.flow_dir & kFlowLeft) mask |= 1ull << kOutLeft;
    if (e.flow_dir & kFlowRight) mask |= 1ull << kOutRight;
    if (!pass_through && e.EntersCore(m.path_core_mask)) {
      mask |= 1ull << kOutCore;
    }
    return mask;
  }

  // 三路 reduce 输入对应三个相邻方向，与 reduce_in_mask 的三位同一套编号：
  // bit0 mid、bit1 left、bit2 right。三个数据源是「两个上游 core 加一个本
  // core」，本 core 自己那一份从 core 方向进来，走哪一路由本表项 flow_dir 的
  // reduce1 与 reduce2 两位指定：两位都不置走 bit0，置 reduce1 走 bit1，置
  // reduce2 走 bit2。一个 core 既收上游分量又出自己那一份时，两者不能挤在同
  // 一路上，这两位就是拿来错开的。
  uint64_t ReduceLaneOf(RouteEntry const& e) const {
    if (dir < kR2RNum) return dir;
    if (e.flow_dir & kFlowReduce2) return 2;
    if (e.flow_dir & kFlowReduce1) return 1;
    return 0;
  }

  // 换 VC：本跳读到表里的「下一跳 VC」后改写包头里的那个字段供下一跳读。
  // 多个出方向时取第一个置位方向的配置。
  uint64_t NextVcOf(RouteEntry const& e, uint64_t mask, uint64_t cur) const {
    if (mask & (1ull << kOutMid)) return e.nxt_vc[0];
    if (mask & (1ull << kOutLeft)) return e.nxt_vc[1];
    if (mask & (1ull << kOutRight)) return e.nxt_vc[2];
    return cur;
  }

  uint64_t dir;
  RouterTable& rtab;
  uint64_t copy;
  bool pass_through = false;

  LinkEndPtr up, up_back;
  std::shared_ptr<XbarReqPort> req;
  std::shared_ptr<CreditLevelPort> level;

  // Step 独占。
  std::array<std::deque<Slot>, kVcNum> vc_buf;
  VcDepth priv_depth = kVcPrivateDepthDefault;
  uint64_t shared_depth = kVcSharedDepth;
  std::array<uint64_t, kVcNum> private_used{};
  uint64_t shared_used = 0;
  uint64_t rr_last = 0;
  uint64_t req_seq = 0;
  uint64_t forwarded_pending = 0, dropped_pending = 0;

  // 走到头被吃掉的 flit 数。
  Logic64 occupancy, forwarded, dropped;
};

}  // namespace bach
}  // namespace latch

#endif
