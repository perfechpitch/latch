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
// 队首在四个 VC 之间轮询选一个交给 Xbar。Xbar 每拍按 VC 发布它那个入口还收不收
// 得下，这个 VC 收得下就发，发了当场出队并归还 VC credit。等授予的话一笔要占两
// 拍，一个方向的吞吐就只剩每两拍一个 flit。资源不足的那一笔在 Xbar 那个 VC 的
// 入口缓冲里等，同一个 input port 的其他 VC 不受影响。
//
// 不派角色的 core 上只走直通：数据走完整流水线但不投递本 core，不检查 credit、
// 不支持阻塞重发。
//
// Core 多操作数：一笔 reduce 任务的几份操作数都从本 core 出来时，core 方向这一站
// 把它们依次分到 reduce_in_mask 置位的几路上（《Router》：Rmem支持一个方向来的
// 多个操作数，需要配置特殊模式）。同一用户在这条 path 上的第 k 个包走第 k 个置位
// 的那一路，走满一轮从头计。一个包从首 flit 定下走哪一路，后面的 flit 跟着走。

#include <array>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <utility>

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

  // 透传档，坏 core 的 Router 由 core_bad_mask 打开：不投递本 core、不检查业务
  // credit、不支持阻塞重发。
  void SetPassThrough(bool on) { pass_through = on; }
  // 队头被归约挡住、入口只剩深度余量时，用它判断这一包现在能不能进归约。
  void SetAdmitBypass(std::function<bool(Message const &)> fn) {
    admit_bypass = std::move(fn);
  }

  uint64_t Occupancy() const { return occupancy.Get(); }
  uint64_t Forwarded() const { return forwarded.Get(); }
  // 停钟之后取的那一份：Logic64 只在协程里读得到当拍的值。
  uint64_t FwdCount() const { return forwarded_pending; }
  uint64_t QueueDepth() const {
    uint64_t n = 0;
    for (auto const& q : vc_buf) n += q.size();
    return n;
  }
  // 停钟后看队头那一拍的 path。没有包返回 false。
  bool QueueHead(uint64_t& path, uint64_t& user) const {
    for (auto const& q : vc_buf) {
      if (q.empty() || !q.front().f.msg) continue;
      path = q.front().f.msg->path_id;
      user = q.front().f.msg->user_id;
      return true;
    }
    return false;
  }

  // 停钟后看 VC v 队头那一拍是包头还是包身、包尾。
  bool FrontFlag(uint64_t v, bool& head, bool& tail) const {
    if (v >= vc_buf.size() || vc_buf[v].empty()) return false;
    head = vc_buf[v].front().f.head;
    tail = vc_buf[v].front().f.tail;
    return true;
  }
  uint64_t LastRoom() const { return req ? req->room.Get() : 0; }
  uint64_t LastBlock() const { return req ? req->hold_block.Get() : 0; }
  uint64_t LastPull() const { return req ? req->hold_pull.Get() : 0; }
  // 停钟后按 VC 列出队列里连续的同一包：v<vc>:u<user>p<path>x<个数>，H/T 表示含包头/包尾。
  std::string VcText() const {
    std::string s;
    for (uint64_t v = 0; v < vc_buf.size(); ++v) {
      if (vc_buf[v].empty()) continue;
      s += " v" + std::to_string(v) + ":";
      uint64_t i = 0;
      while (i < vc_buf[v].size()) {
        FlitView const& g = vc_buf[v][i].f;
        uint64_t u = g.msg ? g.msg->user_id : ~0ull, p = g.msg ? g.msg->path_id : ~0ull;
        uint64_t n = 0;
        bool h = false, t = false;
        while (i < vc_buf[v].size()) {
          FlitView const& x = vc_buf[v][i].f;
          uint64_t xu = x.msg ? x.msg->user_id : ~0ull, xp = x.msg ? x.msg->path_id : ~0ull;
          if (xu != u || xp != p || (n > 0 && x.head)) break;
          h |= x.head;
          t |= x.tail;
          ++n;
          ++i;
          if (x.tail) break;
        }
        s += "u" + std::to_string(u) + "p" + std::to_string(p) + "x" + std::to_string(n) +
             (h ? "H" : "") + (t ? "T" : "") + ",";
      }
    }
    return s;
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
    // 逐 flit 的入口观测：Perfetto 的 router 入口轨道按它折「收包头 → 收完包尾」。
    // 无效拍全 0，保证 TracePerCycle 逐拍提交、脉冲不粘连。flit_size 是包 size
    // （数据通道字节数，含 topK），挂在 Message 上、整包共享。
    TracePerCycle("flit_head", f.valid && f.head ? 1u : 0u);
    TracePerCycle("flit_tail", f.valid && f.tail ? 1u : 0u);
    TracePerCycle("flit_user", f.valid && f.msg ? f.msg->user_id : 0u);
    TracePerCycle("flit_path", f.valid && f.msg ? f.msg->path_id : 0u);
    TracePerCycle("flit_size", f.valid && f.msg ? f.msg->size : 0u);
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
    // 包头已经发出、包尾还没到的那一包优先。它的包身可能还排在后到的包头后面，
    // 先抽出来送进交叉开关，这一拍不再把别的包发到同一条 VC 上。
    if (req->hold_pull.Get() != 0 && PullOpen()) return;
    uint64_t blocked = req->hold_block.Get();
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
      uint64_t lane = ReduceLaneOf(e, f, v);
      uint64_t mask = OutMaskOf(e, *f.msg, lane);
      if (mask == 0) {
        // 开了 path_core_mask 的那一档：这条 path 走遍一串 core，落在哪几个由
        // 包头的 mask 挑，走到既没人要又没有下游的那一跳，这一笔就是走到头了，
        // 在这里吃掉。没开的那一档进不进核与包无关，那时候没有出口是表项配错。
        if (!e.path_core_mask_enable) spdlog::error("RouterStation {} pid {} uid {} copy {} flow {} op {} mask {}",Name(),f.msg->path_id,f.msg->user_id,copy,e.flow_dir,uint64_t(e.operation),f.msg->path_core_mask);
        LOGCHECK(e.path_core_mask_enable,
                 "RouterStation: 表项既不进核也没有出方向。");
        ++dropped_pending;
        Drop(v);
        return;
      }
      // Xbar 那一侧这个 VC 的入口缓冲快满了，这个 VC 本拍不发，队列原地不动。
      uint64_t nvc = NextVcOf(e, mask, f.vc);
      // 同一条交叉开关 VC 一包发完才换下一包（F-012）：本级已经发出包头、
      // 还没发出包尾的那一包，只放它自己的 flit。Xbar 挡住的 VC 同样只放它。
      bool mine = OpenMine(v, f);
      if (open_[v].on && !mine) continue;
      if (((blocked >> v) & 1u) != 0 && !mine) continue;
      if (!req->RoomFor(v)) continue;
      req->valid = 1;
      req->out_mask = mask;
      req->vc = nvc;
      req->input_vc = v; req->input_vc_valid = 1;
      req->head = f.head ? 1 : 0;
      req->tail = f.tail ? 1 : 0;
      req->bytes = f.bytes;
      req->path_id = f.msg->path_id;
      req->user_id = f.msg->user_id;
      req->enters_core = ((mask >> kOutCore) & 1u) ? 1 : 0;
      req->stall_way = (!pass_through && e.stall_way) ? 1 : 0;
      req->stream_need = pass_through ? 0 : e.stream_need;
      req->whole_packet = HasWholePacket(v) ? 1 : 0;
      req->credit_require = pass_through ? 0 : RequireOf(e, mask);
      req->msg = f.msg;
      req->seq = ++req_seq;
      rr_last = v;
      NoteOpen(v, f);
      if (CoreOperands(e) && f.head) TakeOperand(e, *f.msg, lane, v);
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
      up_back->release.Drive(true, v, false, 0);
      if (slack_on && s.f.tail && s.f.msg && s.f.msg->user_id == slack_user &&
          s.f.msg->path_id == slack_path &&
          s.f.msg->reduce_seq == slack_seq) {
        slack_on = false;
      }
      return;
    }
    req->IdleReq();
    up_back->flit.Idle();
    up_back->release.Idle();
  }

  // 本级往交叉开关 VC nvc 发出了包头、还没发出包尾的那一包。
  void NoteOpen(uint64_t v, FlitView const& g) {
    if (g.tail) {
      if (open_[v].on && open_[v].user == g.msg->user_id &&
          open_[v].path == g.msg->path_id) {
        open_[v].on = false;
      }
      return;
    }
    if (!g.head) return;
    open_[v].on = true;
    open_[v].user = g.msg->user_id;
    open_[v].path = g.msg->path_id;
  }
  bool OpenMine(uint64_t v, FlitView const& g) const {
    return open_[v].on && !g.head && open_[v].user == g.msg->user_id &&
           open_[v].path == g.msg->path_id;
  }

  // 队头是还不能进归约的下一包时，同 VC 后面若有当前归约还在等的一拍，先发那一拍。
  int FindBypass(uint64_t v) const {
    if (!admit_bypass) return -1;
    constexpr uint64_t reduce_mask = (1ull << kOutReduce0) |
        (1ull << kOutReduce1) | (1ull << kOutReduce2);
    for (uint64_t i = 1; i < vc_buf[v].size(); ++i) {
      FlitView const& g = vc_buf[v][i].f;
      if (!g.msg) continue;
      RouteEntry const& ge = rtab.Lookup(copy, g.msg->path_id);
      if (!ge.valid) continue;
      uint64_t glane = ReduceLaneOf(ge, g, v);
      uint64_t gmask = OutMaskOf(ge, *g.msg, glane);
      if ((gmask & reduce_mask) == 0) continue;
      uint64_t gnvc = NextVcOf(ge, gmask, g.vc);
      if (!req->SlackFor(gnvc)) continue;
      bool same = slack_on && g.msg->user_id == slack_user &&
                  g.msg->path_id == slack_path &&
                  g.msg->reduce_seq == slack_seq;
      if (g.head) {
        if (!admit_bypass(*g.msg)) continue;
      } else if (!same) {
        continue;
      }
      return static_cast<int>(i);
    }
    return -1;
  }

  // 把还没发完那一包的下一拍从队列中间抽出来。找不到就返回假，调用方再走
  // 普通轮询，被挡住的那条 VC 仍不会发出去。
  bool PullOpen() {
    uint64_t hvc = req->hold_vc.Get();
    uint64_t huser = req->hold_user.Get();
    uint64_t hpath = req->hold_path.Get();
    if (hvc >= kVcNum || !req->RoomFor(hvc)) return false;
    for (uint64_t v = hvc; v == hvc && v < kVcNum; ++v) {
      for (uint64_t i = 0; i < std::min<size_t>(1, vc_buf[v].size()); ++i) {
        FlitView const& g = vc_buf[v][i].f;
        if (!g.msg || g.head) continue;
        if (g.msg->user_id != huser || g.msg->path_id != hpath) continue;
        RouteEntry const& ge = rtab.Lookup(copy, g.msg->path_id);
        if (!ge.valid) continue;
        uint64_t glane = ReduceLaneOf(ge, g, v);
        uint64_t gmask = OutMaskOf(ge, *g.msg, glane);
        if (v != hvc) continue;
        SendAt(v, i, false);
        return true;
      }
    }
    return false;
  }

  bool SendAt(uint64_t v, uint64_t at, bool track_slack = true) {
    Slot slot = vc_buf[v][at];
    FlitView const& g = slot.f;
    RouteEntry const& ge = rtab.Lookup(copy, g.msg->path_id);
    uint64_t glane = ReduceLaneOf(ge, g, v);
    uint64_t gmask = OutMaskOf(ge, *g.msg, glane);
    uint64_t gnvc = NextVcOf(ge, gmask, g.vc);
    req->valid = 1;
    req->out_mask = gmask;
    req->vc = gnvc;
    req->input_vc = v; req->input_vc_valid = 1;
    req->head = g.head ? 1 : 0;
    req->tail = g.tail ? 1 : 0;
    req->bytes = g.bytes;
    req->path_id = g.msg->path_id;
    req->user_id = g.msg->user_id;
    req->enters_core = ((gmask >> kOutCore) & 1u) ? 1 : 0;
    req->stall_way = (!pass_through && ge.stall_way) ? 1 : 0;
    req->stream_need = pass_through ? 0 : ge.stream_need;
    req->whole_packet = 0;
    req->credit_require = pass_through ? 0 : RequireOf(ge, gmask);
    req->msg = g.msg;
    req->seq = ++req_seq;
    rr_last = v;
    NoteOpen(v, g);
    if (CoreOperands(ge) && g.head) TakeOperand(ge, *g.msg, glane, v);
    vc_buf[v].erase(std::next(vc_buf[v].begin(), static_cast<std::ptrdiff_t>(at)));
    if (slot.used_shared) {
      --shared_used;
    } else {
      --private_used[v];
    }
    ++forwarded_pending;
    up_back->flit.Idle();
    up_back->release.Drive(true, v, false, 0);
    if (track_slack) {
      slack_on = !g.tail;
      if (!g.tail) {
        slack_user = g.msg->user_id;
        slack_path = g.msg->path_id;
        slack_seq = g.msg->reduce_seq;
      }
    }
    return true;
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
    up_back->release.Drive(true, v, false, 0);
  }

  // 这一笔在下游要占多少 credit。多个出方向时取第一个置位方向那一份，多播
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
  // path 从本级“往哪几个方向发”，是归约算完之后的事；operation 管的是本级在
  // 这条 path 上的角色。两者混用会成环：归约结果回注时用同一个 path_id 查表，
  // 若按 flow_dir 判就会被再次送进 ReduceModule。
  //
  // 本级要做归约时这一笔只进 ReduceModule，不同时往下游发；下游那一段由
  // ReduceModule 收齐后按 flow_dir 自己发。
  uint64_t OutMaskOf(RouteEntry const& e, Message const& m,
                     uint64_t lane) const {
    if (!pass_through && e.operation != Operation::kForward &&
        e.reduce_in_mask != 0) {
      return 1ull << (kOutReduce0 + lane);
    }
    uint64_t mask = 0;
    if (e.flow_dir & kFlowMid) mask |= 1ull << kOutMid;
    if (e.flow_dir & kFlowLeft) mask |= 1ull << kOutLeft;
    if (e.flow_dir & kFlowRight) mask |= 1ull << kOutRight;
    // 本核自己发出去的重发包不再送回本核。表项里的进核位描述的是从 mesh
    // 进来的那一跳；出核数据已经在核里待过，再进一次就是第二个 token。
    if (dir != kInLocal && !pass_through && e.EntersCore(m.path_core_mask)) {
      mask |= 1ull << kOutCore;
    }
    return mask;
  }

  // 三路 reduce 输入对应三个相邻方向，与 reduce_in_mask 的三位同一套编号：
  // bit0 mid、bit1 left、bit2 right。三个数据源是“两个上游 core 加一个本
  // core”，本 core 自己那一份从 core 方向进来，走哪一路由本表项 flow_dir 的
  // reduce1 与 reduce2 两位指定：两位都不置走 bit0，置 reduce1 走 bit1，置
  // reduce2 走 bit2。一个 core 既收上游分量又出自己那一份时，两者不能挤在同
  // 一路上，这两位就是拿来错开的。两位都置是 Core 多操作数：本 core 出的几份
  // 依次占 reduce_in_mask 置位的几路。
  uint64_t ReduceLaneOf(RouteEntry const& e, FlitView const& f,
                        uint64_t v) const {
    if (dir < kR2RNum) return dir;
    if (CoreOperands(e)) {
      if (!f.head) return cur_lane[v];
      auto it = operand_seen.find({f.msg->path_id, f.msg->user_id});
      uint64_t k = it == operand_seen.end() ? 0 : it->second;
      return NthLane(e.reduce_in_mask, k);
    }
    if (e.flow_dir & kFlowReduce2) return 2;
    if (e.flow_dir & kFlowReduce1) return 1;
    return 0;
  }
  bool CoreOperands(RouteEntry const& e) const {
    return dir == kInLocal && !pass_through &&
           e.operation != Operation::kForward &&
           (e.flow_dir & kFlowReduce1) && (e.flow_dir & kFlowReduce2);
  }
  // reduce_in_mask 里第 k 个置位的那一路。
  static uint64_t NthLane(uint64_t in_mask, uint64_t k) {
    for (uint64_t r = 0; r < kR2RNum; ++r) {
      if (((in_mask >> r) & 1u) == 0) continue;
      if (k == 0) return r;
      --k;
    }
    LOGCHECK(false, "RouterStation: Core 多操作数的份数超过 reduce_in_mask 的路数。");
    return 0;
  }
  // 首 flit 发出去才算占了这一路：本拍没发出去的下一拍重算，不能多记一份。
  void TakeOperand(RouteEntry const& e, Message const& m, uint64_t lane,
                   uint64_t v) {
    cur_lane[v] = lane;
    uint64_t n = 0;
    for (uint64_t r = 0; r < kR2RNum; ++r) n += (e.reduce_in_mask >> r) & 1u;
    auto key = std::make_pair(m.path_id, m.user_id);
    uint64_t k = operand_seen[key] + 1;
    if (k >= n) {
      operand_seen.erase(key);
    } else {
      operand_seen[key] = k;
    }
  }

  // 换 VC：本跳读到表里的“下一跳 VC”后改写包头里的那个字段供下一跳读。
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
  std::function<bool(Message const&)> admit_bypass;
  bool slack_on = false;
  // 按交叉开关 VC 记：包头已经发出、包尾还没发出的那一包。
  struct Open {
    bool on = false;
    uint64_t user = 0;
    uint64_t path = 0;
  };
  std::array<Open, kVcNum> open_{};
  uint64_t slack_user = 0, slack_path = 0, slack_seq = 0;

  LinkEndPtr up, up_back;
  std::shared_ptr<XbarReqPort> req;
  std::shared_ptr<CreditLevelPort> level;

  // Step 独占。
  std::array<std::deque<Slot>, kVcNum> vc_buf;
  VcDepth priv_depth = kVcPrivateDepthDefault;
  uint64_t shared_depth = kVcSharedDepth;
  std::array<uint64_t, kVcNum> private_used{};
  uint64_t shared_used = 0;
  // Core 多操作数：各 VC 上正在发的那个包走哪一路；每个 (path, 用户) 这一轮已经
  // 发出几份。
  std::array<uint64_t, kVcNum> cur_lane{};
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> operand_seen;
  uint64_t rr_last = 0;
  uint64_t req_seq = 0;
  uint64_t forwarded_pending = 0, dropped_pending = 0;

  // 走到头被吃掉的 flit 数。
  Logic64 occupancy, forwarded, dropped;
};

}  // namespace bach
}  // namespace latch

#endif
