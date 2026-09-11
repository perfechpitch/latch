#ifndef _LATCH_BACH_IP_CHIP_CORE_ROUTER_XBAR_
#define _LATCH_BACH_IP_CHIP_CORE_ROUTER_XBAR_

// Xbar：5 入 7 出的交换点，同时持有下游方向的两样资源。
//
// 入是 left / right / mid / local（core 方向的 station）/ ReduceModule 回注；
// 出是 left / right / mid / core / reduce_0 / reduce_1 / reduce_2。
//
// 三级在一拍里做完：
//   M3 VA  查目标方向的 VC credit 与 Stream 授权
//   M4 SA  每个 output port 一个独立的 RoundRobin，每拍独立仲裁，不跨拍锁定
//   M5 ST  发出后统一扣各目标的 credit
//
// 每个入口一个小的输入缓冲：station 那一侧按「收得下就发」交进来，本级从缓冲的
// 队首仲裁。资源不够时这一笔留在缓冲里等，同一个入口后面的不越过它。
//
// 多播全有全无：任一目标没握手就不推进任何分支。只发一半会让同一个 User 的数据
// 在不同分支上错位，已发方向占了资源却完不成整体传输。
//
// 仲裁不是纯 RoundRobin，是贪婪整包。优先级由高到低三档：还在发一个包的入口、
// 队列里攥着一整个包的入口、其余按 RoundRobin。R2R 的通路本身允许在 flit 边界
// 切换包，所以别的包可以占掉本包两个 flit 之间的那一拍，但本包的下一个 flit
// 一提出来就该被授予。
//
// 原文在这三档之前还有一档「接口传输 priority」，接口之间怎么分优先级没有给，
// 这里不猜。
//
// VC credit 两级记账，与下游 VC Buffer 的分配规则一一对应：private[o][v] > 0 就
// 扣 private，否则扣 shared[o]，两者都为 0 时该 VC 不能发。归还按同一条规则回填。
// 两边规则相同，计数因此不会漂移。

#include <array>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 每方向的 Stream Resource Table 项数，建模计划的默认值。
constexpr uint64_t kStreamTabEntries = 16;

// 下游 core 那一侧的 Core Mem credit，单位 1 KB。初值取自 F109 的例子：一个
// 方向 128，一笔广播扣 32 之后剩 96。真实初值由编译侧按 path 与 stream 算好，
// 广播等于目的 core 数。
constexpr uint64_t kCoreCreditKb = 128;

// 每个入口的输入缓冲深度与报「收得下」的门限。
//
// station 读的是上一拍发布的 room，所以从 room 拉低到它停下来隔着一拍，那一拍
// 它还会再交一笔进来。门限之外留两格就是给这一笔加一格余量的。
constexpr uint64_t kXbarInDepth = 4;
constexpr uint64_t kXbarInMark = 2;

class Xbar : public BachModule {
 public:
  Xbar(ClockPtr clock, const std::string& name, uint64_t parent = 0,
       bool tick = true)
      : BachModule(clock, name, parent, tick),
        level(std::make_shared<CreditLevelPort>(clock)),
        granted(clock),
        stalled(clock),
        overflow(clock) {
    for (uint64_t i = 0; i < kXbarInNum; ++i) reqs.push_back(nullptr);
    for (uint64_t o = 0; o < kR2RNum; ++o) {
      out_link.push_back(std::make_shared<LinkEnd>(clock));
      private_cr[o] = priv_depth;
      shared_cr[o] = shared_depth;
      core_credit[o] = kCoreCreditKb;
    }
    core_out = std::make_shared<LinkEnd>(clock);
    core_level = std::make_shared<ReadyLevelPort>(clock);
    overflow_port = std::make_shared<OverflowPort>(clock);
    for (uint64_t r = 0; r < 3; ++r) {
      reduce_out.push_back(std::make_shared<LinkEnd>(clock));
      reduce_level.push_back(std::make_shared<ReadyLevelPort>(clock));
    }
  }

  // 装配层把各 station 的请求口接进来。
  void AttachReq(uint64_t in_idx, std::shared_ptr<XbarReqPort> port) {
    reqs.at(in_idx) = std::move(port);
  }
  std::shared_ptr<CreditLevelPort> LevelPtr() const { return level; }

  // 三个 R2R 出方向的线，接下游链路。
  LinkEnd& OutLink(uint64_t o) { return *out_link.at(o); }
  void AttachOutLink(uint64_t o, LinkEndPtr wire) {
    out_link.at(o) = std::move(wire);
  }
  // 下游回来的 vc_release 走的那根线，由装配层接。
  void AttachBackLink(uint64_t o, LinkEndPtr wire) {
    back_link.resize(kR2RNum);
    back_link.at(o) = std::move(wire);
  }

  LinkEnd& CoreOut() { return *core_out; }
  void AttachCoreOut(LinkEndPtr wire) { core_out = std::move(wire); }
  LinkEnd& ReduceOut(uint64_t r) { return *reduce_out.at(r); }
  void AttachReduceOut(uint64_t r, LinkEndPtr wire) {
    reduce_out.at(r) = std::move(wire);
  }

  // core 与 reduce 两侧的准入电平，由 CoreStation 与 ReduceModule 每拍发布。
  // 没接的时候端口读出来是 0，所以装配层不接就等于关着；单模块测试要显式接一根
  // 常高的线，或者用 ForceReady 把这一档关掉。
  void AttachCoreLevel(std::shared_ptr<ReadyLevelPort> port) {
    core_level = std::move(port);
  }
  void AttachReduceLevel(uint64_t r, std::shared_ptr<ReadyLevelPort> port) {
    reduce_level.at(r) = std::move(port);
  }
  // 单模块测试用：不接准入电平时假定对方永远收得下。
  void ForceReady(bool on) { force_ready = on; }

  // 溢流出口，接 CoreMemReissue。
  std::shared_ptr<OverflowPort> OverflowPtr() const { return overflow_port; }
  void AttachOverflow(std::shared_ptr<OverflowPort> port) {
    overflow_port = std::move(port);
  }

  // 只透传的 core：不记账、不占坑，credit 直接 bypass。
  void SetPassThrough(bool on) { pass_through = on; }

  uint64_t Granted() const { return granted.Get(); }
  uint64_t Stalled() const { return stalled.Get(); }
  uint64_t Overflow() const { return overflow.Get(); }

  // 验收要查的 credit 守恒：一轮跑完后所有方向所有类型的 credit 回到初值。
  uint64_t VcCredit(uint64_t o, uint64_t v) const { return private_cr[o][v]; }
  uint64_t SharedCredit(uint64_t o) const { return shared_cr[o]; }
  // 下游 VC Buffer 的两级深度，也就是本级 credit 的初值。跑起来之前配。
  void SetVcDepth(VcDepth const& priv, uint64_t shared) {
    priv_depth = priv;
    shared_depth = shared;
    for (uint64_t o = 0; o < kR2RNum; ++o) {
      private_cr[o] = priv_depth;
      shared_cr[o] = shared_depth;
    }
  }
  // 停钟之后取的那一份。
  uint64_t SentCount(uint64_t o) const { return sent_total[o]; }
  uint64_t StreamUsed(uint64_t o) const { return stream_tab[o].size(); }
  // B core 广播之前查的那一档：mask 指的每个方向都要能给这个 user 留一个 stream
  // 坑，有一个方向留不出来就整笔不发。mask 的位与出口编号同序。
  bool BroadcastRoom(uint64_t mask, uint64_t user) const {
    for (uint64_t o = 0; o < kR2RNum; ++o) {
      if ((mask & (1ull << o)) == 0) continue;
      if (stream_tab[o].count(user) != 0) continue;
      if (stream_tab[o].size() >= kStreamTabEntries) return false;
    }
    return true;
  }
  bool StreamHolds(uint64_t o, uint64_t user) const {
    return stream_tab[o].count(user) != 0;
  }
  // 这个方向还剩多少 Core Mem credit，单位 1 KB。
  uint64_t CoreCredit(uint64_t o) const { return core_credit[o]; }
  void SetCoreCredit(uint64_t o, uint64_t kb) { core_credit[o] = kb; }

  // Retire：停止该 UserID 的新发送，删掉它全部的 stream 授权表项。
  // Retire 从这个口广播退休的 user。表由本模块的协程改，Retire 只送号。
  void AttachRetire(std::shared_ptr<RetireBroadcastPort> p) {
    retire_in = std::move(p);
  }

  // Retire 的动作只有一条：删掉这个用户在各方向上的 stream credit 授权。
  // 已经进了本级、正在等仲裁的包照发。「停止新发送」说的是新包要重新申请
  // 授权，而 Retire 之后本来就不会再有新包。挡住在途的那些包会把出核的最后
  // 一笔卡死在 Router 里。
  void RetireUser(uint64_t user) {
    for (uint64_t o = 0; o < kR2RNum; ++o) {
      if (stream_tab[o].erase(user) == 0) continue;
      // 这个用户占的那些 KB 跟着退休一起还回来。
      auto it = taken_kb[o].find(user);
      if (it == taken_kb[o].end()) continue;
      core_credit[o] += it->second;
      if (core_credit[o] > kCoreCreditKb) core_credit[o] = kCoreCreditKb;
      taken_kb[o].erase(it);
    }
  }

  bool Quiescent() const override {
    for (auto const& q : in_q) {
      if (!q.empty()) return false;
    }
    return true;
  }

 protected:
  void Step() override {
    TakeRetire();
    ReturnCredit();
    // 先收再仲裁：本拍交进来的这一笔，没有冲突时本拍就发出去，入口缓冲上不多
    // 占一拍。缓冲是给「资源不够、留着等」的那些用的，不是流水线的一级。
    Collect();
    Arbitrate();
    PublishRoom();
    PublishLevel();

    granted = granted_pending;
    stalled = stalled_pending;
    overflow = overflow_pending;
    TracePerCycle("granted", granted_pending);
    TracePerCycle("stalled", stalled_pending);
  }

 private:
  // 收 Retire 的广播。同一笔会连着两拍出现在端口上，按序号认它。
  void TakeRetire() {
    if (!retire_in || !retire_in->Valid()) return;
    if (retire_in->Seq() == last_retire_seq) return;
    last_retire_seq = retire_in->Seq();
    RetireUser(retire_in->User());
  }

  std::shared_ptr<RetireBroadcastPort> retire_in;
  uint64_t last_retire_seq = 0;

  // 下游 flit 离开它的 VC Buffer 就还一个 credit。回填规则与占用规则对称：
  // private 没满补 private，满了补 shared。
  void ReturnCredit() {
    for (uint64_t o = 0; o < back_link.size(); ++o) {
      if (!back_link[o]) continue;
      ReleaseView r = ReadRelease(back_link[o]->release);
      if (!r.vc_valid) continue;
      uint64_t v = r.vc_id;
      LOGCHECK(v < kVcNum, "Xbar: 归还的 VC 号越界。");
      if (private_cr[o][v] < priv_depth[v]) {
        ++private_cr[o][v];
      } else {
        LOGCHECK(shared_cr[o] < shared_depth,
                 "Xbar: credit 归还超过初值，两边规则漂了。");
        ++shared_cr[o];
      }
    }
  }

  // 从各入口的请求端口收进输入缓冲。按序号认，一笔只收一次。
  void Collect() {
    for (uint64_t in = 0; in < kXbarInNum; ++in) {
      if (!reqs[in]) continue;
      XbarReqView v = ReadXbarReq(*reqs[in]);
      if (!v.valid || v.seq == last_seq[in]) continue;
      last_seq[in] = v.seq;
      LOGCHECK(in_q[in].size() < kXbarInDepth,
               "Xbar: 入口缓冲满了。上游按 room 发，满就说明门限留少了。");
      in_q[in].push_back(v);
    }
  }

  // 每拍发布各入口还收不收得下。
  void PublishRoom() {
    for (uint64_t in = 0; in < kXbarInNum; ++in) {
      if (!reqs[in]) continue;
      reqs[in]->DriveRoom(in_q[in].size() < kXbarInMark);
    }
  }

  void Arbitrate() {
    // 先把各输出口置空，本拍没人用就是 idle。
    std::array<bool, kXbarOutNum> taken{};
    overflow_used = false;
    std::vector<uint64_t> order = IssueOrder();

    for (uint64_t in : order) {
      if (in_q[in].empty()) continue;
      XbarReqView const& v = in_q[in].front();

      // 多播全有全无：先看所有目标出口是不是都空着、资源都够。另一个入口的包
      // 正占着这个出口的这个 VC 时只能等，不算资源不够，不走转存。
      bool ok = true, owner_busy = false;
      for (uint64_t o = 0; o < kXbarOutNum; ++o) {
        if (((v.out_mask >> o) & 1u) == 0) continue;
        if (!OwnerOk(o, v.vc, in)) {
          owner_busy = true;
          ok = false;
          break;
        }
        if (taken[o] || !ResourceOk(o, v)) {
          ok = false;
          break;
        }
      }
      if (!ok) {
        ++stalled_pending;
        // 拿不到资源时按 stall_way 二选一：留在当前 VC 等（什么都不做，下一拍
        // 再来），或转进本地 Core Mem 由 DTE 重发。
        //
        // 走转存这一档时这一笔就算处理完了：交给 CoreMemReissue，同时给 station
        // 发 grant 把 VC 槽腾出来，不腾的话这一笔既在暂存区里、又占着 VC，
        // 同一份数据记了两处。Router 上的 Bypass 因此被映射成「进 core 加出 core」
        // 两段。每拍最多转存一笔，因为端口一拍只搬一个 flit。
        if (v.stall_way && !owner_busy && !overflow_used) {
          FlitView f;
          f.valid = true;
          f.vc = v.vc;
          f.head = v.head;
          f.tail = v.tail;
          f.bytes = v.bytes;
          f.msg = v.msg;
          overflow_port->Drive(f);
          overflow_used = true;
          in_q[in].pop_front();
          ++overflow_pending;
        }
        continue;
      }

      for (uint64_t o = 0; o < kXbarOutNum; ++o) {
        if (((v.out_mask >> o) & 1u) == 0) continue;
        taken[o] = true;
        Consume(o, v);
        Emit(o, v);
        vc_owner[o][v.vc] = v.tail ? 0 : in + 1;
      }

      ++granted_pending;
      // 这个入口上的包发完没有。记在入口上而不是记「上一拍发的是谁」：一个包
      // 的两个 flit 之间可能隔着一拍，那一拍会被别的入口占掉，记后者的话本包
      // 剩下的 flit 就丢了优先级，包被拆散在多拍里交织出去。
      in_packet[in] = !v.tail;
      in_q[in].pop_front();
      continue;
    }
    // 没人用的出口置 idle。
    // 出线的 release 上只写 VC 那一类：Reduce 那一类由 ReduceModule 写。
    for (uint64_t o = 0; o < kR2RNum; ++o) {
      if (!taken[o]) {
        out_link[o]->flit.Idle();
        out_link[o]->release.DriveVc(false, 0);
      }
    }
    if (!taken[kOutCore]) {
      core_out->flit.Idle();
      core_out->release.Idle();
    }
    for (uint64_t r = 0; r < 3; ++r) {
      if (!taken[kOutReduce0 + r]) {
        reduce_out[r]->flit.Idle();
        reduce_out[r]->release.Idle();
      }
    }
    if (!overflow_used) overflow_port->Idle();
  }

  // 贪婪整包，三档：还没发完一个包的入口排在最前，其次是队列里攥着一整个包的，
  // 其余按 RoundRobin。每一档内部都按 RoundRobin 的顺序走，所以同一档里的几个
  // 入口仍然轮流。
  std::vector<uint64_t> IssueOrder() {
    std::vector<uint64_t> order;
    for (uint64_t k = 1; k <= kXbarInNum; ++k) {
      uint64_t in = (rr_last + k) % kXbarInNum;
      if (in_packet[in]) order.push_back(in);
    }
    for (uint64_t k = 1; k <= kXbarInNum; ++k) {
      uint64_t in = (rr_last + k) % kXbarInNum;
      if (in_packet[in] || in_q[in].empty()) continue;
      if (in_q[in].front().whole_packet) order.push_back(in);
    }
    for (uint64_t k = 1; k <= kXbarInNum; ++k) {
      uint64_t in = (rr_last + k) % kXbarInNum;
      if (in_packet[in]) continue;
      if (!in_q[in].empty() && in_q[in].front().whole_packet) continue;
      order.push_back(in);
    }
    rr_last = (rr_last + 1) % kXbarInNum;
    return order;
  }

  // 同一出口同一 VC 上一个包从首 flit 到尾 flit 独占：包中间隔了几拍，别的入口
  // 也不能在这个 VC 上插进来，否则下游按首尾拼包会拼乱。
  bool OwnerOk(uint64_t o, uint64_t vc, uint64_t in) const {
    LOGCHECK(vc < kVcNum, "Xbar: VC 号越界。");
    return vc_owner[o][vc] == 0 || vc_owner[o][vc] == in + 1;
  }

  bool ResourceOk(uint64_t o, XbarReqView const& v) const {
    if (o == kOutCore) return force_ready || core_level->Ready();
    if (o >= kOutReduce0) {
      return force_ready || reduce_level[o - kOutReduce0]->Ready();
    }
    // 链路级的 VC credit 与本级派不派角色无关：它记的是下一跳 VC Buffer 还有
    // 几个位置，不看就发会把下一跳撑爆。只透传的 core 不投递本 core、不占
    // stream 坑、不扣 Core Mem 的量，所以只查这一项。
    if (private_cr[o][v.vc] == 0 && shared_cr[o] == 0) return false;
    if (pass_through) return true;
    if (v.need_stream && !StreamOk(o, v.user_id)) return false;
    // 已经占过坑的用户不再扣量：坑按 user 记，量也跟着那一次记。
    if (v.need_stream && stream_tab[o].count(v.user_id) == 0 &&
        core_credit[o] < v.credit_require) {
      return false;
    }
    return true;
  }

  // 三态准入：已占的直接过；没占但有空项则过（占用在 Consume 里做）；满了不过。
  bool StreamOk(uint64_t o, uint64_t user) const {
    if (stream_tab[o].count(user) != 0) return true;
    return stream_tab[o].size() < kStreamTabEntries;
  }

  void Consume(uint64_t o, XbarReqView const& v) {
    ++sent_total[o];
    if (o == kOutCore || o >= kOutReduce0) return;
    if (private_cr[o][v.vc] > 0) {
      --private_cr[o][v.vc];
    } else {
      --shared_cr[o];
    }
    if (pass_through) return;
    // 坑按 user 记，不按包记：同一个 user 第二次发送余额不变。credit 的量也
    // 只在第一次占坑时扣，单位 1 KB。
    if (v.need_stream && stream_tab[o].insert(v.user_id).second) {
      LOGCHECK(core_credit[o] >= v.credit_require,
               "Xbar: Core Mem credit 不够就发了。");
      core_credit[o] -= v.credit_require;
      taken_kb[o][v.user_id] += v.credit_require;
    }
  }

  void Emit(uint64_t o, XbarReqView const& v) {
    // 本跳把包头里的 VC 改成表里的「下一跳 VC」，供下一跳直接取。
    if (v.msg) v.msg->vc = v.vc;
    LinkEnd* dst = nullptr;
    if (o < kR2RNum) {
      dst = out_link[o].get();
    } else if (o == kOutCore) {
      dst = core_out.get();
    } else {
      dst = reduce_out[o - kOutReduce0].get();
    }
    dst->flit.Drive(v.vc, v.head, v.tail, v.bytes, v.msg);
    if (o < kR2RNum) {
      dst->release.DriveVc(false, 0);
    } else {
      dst->release.Idle();
    }
  }

  void PublishLevel() {
    uint64_t ok = 0, free_mask = 0;
    for (uint64_t o = 0; o < kR2RNum; ++o) {
      for (uint64_t v = 0; v < kVcNum; ++v) {
        if (private_cr[o][v] > 0 || shared_cr[o] > 0) {
          ok |= 1ull << CreditLevelPort::Bit(o, v);
        }
      }
      if (stream_tab[o].size() < kStreamTabEntries) free_mask |= 1ull << o;
    }
    level->Drive(ok, free_mask);
  }

  std::vector<std::shared_ptr<XbarReqPort>> reqs;
  std::shared_ptr<CreditLevelPort> level;
  std::vector<LinkEndPtr> out_link, back_link, reduce_out;
  LinkEndPtr core_out;
  std::shared_ptr<ReadyLevelPort> core_level;
  std::vector<std::shared_ptr<ReadyLevelPort>> reduce_level;
  std::shared_ptr<OverflowPort> overflow_port;

  // Step 独占。
  VcDepth priv_depth = kVcPrivateDepthDefault;
  uint64_t shared_depth = kVcSharedDepth;
  std::array<VcDepth, kR2RNum> private_cr{};
  std::array<uint64_t, kR2RNum> shared_cr{};
  std::array<std::set<uint64_t>, kR2RNum> stream_tab;
  std::array<uint64_t, kR2RNum> core_credit{};
  std::array<std::map<uint64_t, uint64_t>, kR2RNum> taken_kb;
  bool force_ready = false;
  bool overflow_used = false;
  bool pass_through = false;
  std::array<uint64_t, kXbarOutNum> sent_total{};
  std::array<std::deque<XbarReqView>, kXbarInNum> in_q;
  std::array<uint64_t, kXbarInNum> last_seq{};
  std::array<bool, kXbarInNum> in_packet{};
  // 各出口各 VC 正被哪个入口的包占着：入口号加一，0 表示空着。
  std::array<std::array<uint64_t, kVcNum>, kXbarOutNum> vc_owner{};
  uint64_t rr_last = 0;
  uint64_t granted_pending = 0, stalled_pending = 0, overflow_pending = 0;

  Logic64 granted, stalled, overflow;
};

}  // namespace bach
}  // namespace latch

#endif
