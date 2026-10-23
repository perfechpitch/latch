#ifndef _LATCH_BACH_IP_CHIP_CORE_ROUTER_XBAR_
#define _LATCH_BACH_IP_CHIP_CORE_ROUTER_XBAR_

// Xbar：5 入 7 出的交换点，持有各出口那一侧的两样资源。
//
// 入是 left / right / mid / local（core 方向的 station）/ ReduceModule 回注；
// 出是 left / right / mid / core / reduce_0 / reduce_1 / reduce_2。
//
// 三级在一拍里做完：
//   M3 VA  查目标方向的 VC credit 与 Stream 资源表
//   M4 SA  每个 output port 一个独立的 RoundRobin，每拍独立仲裁，不跨拍锁定
//   M5 ST  发出后统一扣各目标的 credit
//
// 每个入口按 VC 各一个小的输入缓冲：station 那一侧按“这个 VC 收得下就发”交进
// 来，本级从各 VC 的队首仲裁。资源不够时这一笔留在缓冲里等，同一个 VC 后面的不
// 越过它；同一个入口别的 VC 照走，与 station 里四个 VC 互不侵占是同一件事。
//
// 多播全有全无：任一目标没握手就不推进任何分支。只发一半会让同一个 User 的数据
// 在不同分支上错位，已发方向占了资源却完不成整体传输。
//
// 仲裁不是纯 RoundRobin，是贪婪整包。优先级由高到低三档：还在发一个包的入口、
// 队列里攥着一整个包的入口、其余按 RoundRobin。R2R 的通路本身允许在 flit 边界
// 切换包，所以别的包可以占掉本包两个 flit 之间的那一拍，但本包的下一个 flit
// 一提出来就该被授予。
//
// 原文在这三档之前还有一档“接口传输 priority”，接口之间怎么分优先级没有给，
// 这里不猜。
//
// VC credit 两级记账，与下游 VC Buffer 的分配规则一一对应：private[o][v] > 0 就
// 扣 private，否则扣 shared[o]，两者都为 0 时该 VC
// 不能发。归还按同一条规则回填。 两边规则相同，计数因此不会漂移。

#include <algorithm>
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/module_base.h"
#include "base/log.h"

namespace latch {
namespace bach {

// 每方向的 Stream Resource Table 项数，建模计划的默认值。
constexpr uint64_t kStreamTabEntries = 16;

// 下游 core 那一侧的 Core Mem credit，单位 1 KB。初值取自 F109 的例子：一个
// 方向 128，一笔广播扣 32 之后剩 96。真实初值由编译侧按 path 与 stream 算好，
// 广播等于目的 core 数。
constexpr uint64_t kCoreCreditKb = 128;

// 每个入口每个 VC 的输入缓冲深度与报“收得下”的门限。
//
// station 读的是上一拍发布的 room，所以从 room 拉低到它停下来隔着一拍，那一拍
// 它还会再交一笔进来。门限之外留两格就是给这一笔加一格余量的。
constexpr uint64_t kXbarInDepth = 4;
constexpr uint64_t kXbarInMark = 2;

class Xbar : public BachModule {
public:
  Xbar(ClockPtr clock, const std::string &name, uint64_t parent = 0,
       bool tick = true)
      : BachModule(clock, name, parent, tick),
        level(std::make_shared<CreditLevelPort>(clock)), granted(clock),
        stalled(clock), overflow(clock) {
    ts_returns = std::make_shared<DirectionReturnPort>(clock);
    for (uint64_t i = 0; i < kXbarInNum; ++i)
      reqs.push_back(nullptr);
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
  LinkEnd &OutLink(uint64_t o) { return *out_link.at(o); }
  void AttachOutLink(uint64_t o, LinkEndPtr wire) {
    out_link.at(o) = std::move(wire);
  }
  // 下游回来的 vc_release 走的那根线，由装配层接。
  void AttachBackLink(uint64_t o, LinkEndPtr wire) {
    back_link.resize(kR2RNum);
    back_link.at(o) = std::move(wire);
  }

  LinkEnd &CoreOut() { return *core_out; }
  void AttachCoreOut(LinkEndPtr wire) { core_out = std::move(wire); }
  LinkEnd &ReduceOut(uint64_t r) { return *reduce_out.at(r); }
  void AttachReduceOut(uint64_t r, LinkEndPtr wire) {
    reduce_out.at(r) = std::move(wire);
  }

  // core 与 reduce 两侧的准入电平，由 CoreStation 与 ReduceModule 每拍发布。
  // 没接的时候端口读出来是 0，所以装配层不接就等于关着；单模块测试要显式接一根
  // 常高的线，或者用 ForceReady 把这一档关掉。
  void AttachCoreLevel(std::shared_ptr<ReadyLevelPort> port) {
    core_level = std::move(port);
  }
  // 数据缓冲还有空。包头队列满的时候，正在进核的那个包的后续 flit 仍看这一路。
  void AttachDataLevel(std::shared_ptr<ReadyLevelPort> port) {
    data_level = std::move(port);
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

  // 透传档，坏 core 的 Router 由 core_bad_mask 打开：不记账、不占坑，业务
  // credit 直接 bypass。
  void SetPassThrough(bool on) { pass_through = on; }

  // 下一跳 VC credit 用完时整包改送进核（保底）。规划器给出无环的 VC 分配时
  // 可以关掉，credit 不够只等不改送。
  void SetCreditDivert(bool on) { credit_divert = on; }

  uint64_t Granted() const { return granted.Get(); }
  uint64_t Stalled() const { return stalled.Get(); }
  uint64_t Overflow() const { return overflow.Get(); }
  uint64_t InDepth(uint64_t in) const {
    uint64_t n = 0;
    for (auto const& q : in_q.at(in)) n += q.size();
    return n;
  }
  bool InFront(uint64_t in, uint64_t& path, uint64_t& mask, bool& head) const {
    for (uint64_t v = 0; v < kVcNum; ++v) {
      if (in_q.at(in)[v].empty()) continue;
      XbarReqView const& f = in_q.at(in)[v].front();
      path = f.path_id;
      mask = f.out_mask;
      head = f.head;
      return true;
    }
    return false;
  }
  uint64_t InFrontUser(uint64_t in) const {
    for (uint64_t v = 0; v < kVcNum; ++v) {
      if (in_q.at(in)[v].empty()) continue;
      return in_q.at(in)[v].front().user_id;
    }
    return ~0ull;
  }
  // 停钟后看这个入口最后一次没发出去的原因。0 没试过，1 准入，2 出口被占，
  // 3 资源，4 发出去了。
  int HoldWhy(uint64_t in) const { return hold_why.at(in); }
  uint64_t HoldPath(uint64_t in) const { return hold_path.at(in); }
  // 停钟后按 VC 列出入口缓冲与 aside 里的 flit。
  std::string QueueText(uint64_t in) const {
    std::string s;
    for (uint64_t v = 0; v < kVcNum; ++v) {
      for (XbarReqView const& f : in_q.at(in)[v]) {
        s += " v" + std::to_string(v) + "u" + std::to_string(f.user_id) + "p" +
             std::to_string(f.path_id) + (f.head ? "H" : "") + (f.tail ? "T" : "") +
             "m" + std::to_string(f.out_mask);
      }
      for (auto const& pkt : aside.at(in)[v]) {
        s += " aside" + std::to_string(v) + "x" + std::to_string(pkt.flits.size()) +
             (pkt.open ? "open" : "");
      }
    }
    return s;
  }
  // 停钟后看这个入口上挡着 station 的几把锁。
  std::string LockText(uint64_t in) const {
    std::string s;
    for (uint64_t v = 0; v < kVcNum; ++v) {
      if (flight_.at(in)[v].on) {
        s += " fl" + std::to_string(v) + ":u" + std::to_string(flight_[in][v].user) +
             "p" + std::to_string(flight_[in][v].path);
      }
    }
    if (divert_.at(in).on) {
      s += " div" + std::to_string(divert_[in].vc) + ":u" +
           std::to_string(divert_[in].user) + "p" + std::to_string(divert_[in].path);
    }
    if (core_pkt_.at(in).on) {
      s += " core" + std::to_string(core_pkt_[in].vc) + ":u" +
           std::to_string(core_pkt_[in].user) + "p" + std::to_string(core_pkt_[in].path) +
           (core_pkt_[in].divert ? "d" : "");
    }
    return s;
  }
  bool ReduceReady(uint64_t r) const {
    return reduce_level.at(r)->Ready();
  }
  uint64_t OwnerOf(uint64_t o, uint64_t vc) const {
    return vc_owner.at(o).at(vc);
  }

  // 验收要查的 credit 守恒：一轮跑完后所有方向所有类型的 credit 回到初值。
  uint64_t VcCredit(uint64_t o, uint64_t v) const { return private_cr[o][v]; }
  uint64_t SharedCredit(uint64_t o) const { return shared_cr[o]; }
  // 下游 VC Buffer 的两级深度，也就是本级 credit 的初值。跑起来之前配。
  void SetVcDepth(VcDepth const &priv, uint64_t shared) {
    priv_depth = priv;
    shared_depth = shared;
    for (uint64_t o = 0; o < kR2RNum; ++o) {
      private_cr[o] = priv_depth;
      shared_cr[o] = shared_depth;
    }
  }
  // 停钟之后取的那一份。
  uint64_t SentCount(uint64_t o) const { return sent_total[o]; }
  uint64_t StreamUsed(uint64_t o) const { return stream_tab.at(o).size(); }
  // B core 广播之前查的那一档：mask 指的每个方向都要能给这个 user 留一个 stream
  // 坑，有一个方向留不出来就整笔不发。mask 的位与出口编号同序。
  bool BroadcastRoom(uint64_t mask, uint64_t user) const {
    for (uint64_t o = 0; o < kR2RNum; ++o) {
      if ((mask & (1ull << o)) == 0)
        continue;
      if (stream_tab[o].count(user) != 0)
        continue;
      if (stream_tab[o].size() >= kStreamTabEntries)
        return false;
    }
    return true;
  }
  bool StreamHolds(uint64_t o, uint64_t user) const {
    return stream_tab.at(o).count(user) != 0;
  }
  void SetReduceAdmission(std::function<bool(Message const &)> check,
                          std::function<void(Message const &)> reserve) {
    reduce_admission = std::move(check);
    reduce_reserve = std::move(reserve);
  }
  // station 在入口已经到门限、队头又被归约挡住时，问这一包现在进不进得了归约。
  bool ReduceHeadOk(Message const &m) const {
    if (!reduce_admission)
      return true;
    return reduce_admission(m);
  }
  // 首 flit 只往网格方向转走、不进本核也不进归约时通知。中间核上的 P2P
  // reissue 是给 stall_way 转存准备的；这一包已经转走，那一对任务要跳过，
  // 否则任务链停在 wait_wake 上，后面的本核归约发不出去。
  void SetForwardSkip(std::function<void(uint64_t, uint64_t)> fn) {
    forward_skip = std::move(fn);
  }
  // 这个方向还剩多少 Core Mem credit，单位 1 KB。
  uint64_t CoreCredit(uint64_t o) const { return core_credit[o]; }
  void SetCoreCredit(uint64_t o, uint64_t kb) { core_credit[o] = kb; }

  // 下游还回来的 release 从这个口进来，Retire 只送方向与 user，表由本模块的协程
  // 改。
  void AttachRelease(std::shared_ptr<StreamReleasePort> p) {
    release_in = std::move(p);
  }

  // 本级退休从这个口来：只放进本 core 那一项（F-036）。各下游方向那几项不动，
  // 它们等那些下游各自退休时还回来。
  void AttachRetire(std::shared_ptr<RetireBroadcastPort> p) {
    retire_in = std::move(p);
  }
  void RetireUser(uint64_t user) { stream_tab[kOutCore].erase(user); }

  // 方向 o 的下游上这个用户跑完了：删那个方向这一项，它占的那些 KB 一起还回来。
  // 没有这一项说明本 core 没向那个方向申请过，丢掉。
  void AttachTsDirections(std::shared_ptr<DirectionOwnerPort> p) {
    ts_owners = std::move(p);
  }
  std::shared_ptr<DirectionReturnPort> DirectionReturnsPtr() const {
    return ts_returns;
  }

  uint64_t TsOwned(uint64_t dir) const {
    return owned_directions.at(dir).size();
  }
  uint64_t TsReturned(uint64_t dir) const { return ts_return_count.at(dir); }
  void ReleaseUser(uint64_t o, uint64_t user) {
    LOGCHECK(o < kR2RNum, "Xbar: release 的方向越界。");
    // TS owns direction permits even when this route does not require the
    // Router's legacy stream table. Return the TS loan on the real release.
    if (owned_directions[o].erase(user)) {
      direction_returns[o].push_back(user);
      ++ts_return_count[o];
    }
    if (stream_tab[o].erase(user) == 0)
      return;
    auto it = taken_kb[o].find(user);
    if (it == taken_kb[o].end())
      return;
    core_credit[o] += it->second;
    if (core_credit[o] > kCoreCreditKb)
      core_credit[o] = kCoreCreditKb;
    taken_kb[o].erase(it);
  }

  // CreditMonitor 授予时就占坑：TS 拿到的授权与这里记的是同一项，不然授了之后
  // 到发包之间还会被别的用户抢走。
  void TakeStream(uint64_t mask, uint64_t user) {
    for (uint64_t o = 0; o < kR2RNum; ++o) {
      if ((mask & (1ull << o)) == 0)
        continue;
      if (!stream_tab[o].insert(user).second)
        continue;
      LOGCHECK(stream_tab[o].size() <= kStreamTabEntries,
               "Xbar: stream 表满了还授权。");
    }
  }

  // mask 里有没有一个出口正有包改送进核等重发（不分 VC）。TS 据此在这些方向
  // 上按用户年龄授权。
  uint64_t ReissueBusy(uint64_t mask) const {
    uint64_t busy = 0;
    for (uint64_t o = 0; o < kR2RNum; ++o)
      if (((mask >> o) & 1u) && !reissue_wait_[o].empty())
        busy |= 1ull << o;
    return busy;
  }
  bool InStreamTab(uint64_t mask, uint64_t user) const {
    for (uint64_t o = 0; o < kR2RNum; ++o)
      if (((mask >> o) & 1u) && stream_tab[o].count(user) == 0)
        return false;
    return true;
  }

  bool Quiescent() const override {
    for (auto const &per_vc : in_q) {
      for (auto const &q : per_vc) {
        if (!q.empty())
          return false;
      }
    }
    for (auto const &per_vc : aside) {
      for (auto const &pkts : per_vc) {
        for (auto const &pkt : pkts) {
          if (pkt.open || !pkt.flits.empty())
            return false;
        }
      }
    }
    return true;
  }

protected:
  void Step() override {
    uint64_t mask = 0;
    if (ts_owners) {
      auto owners = ts_owners->owners.Get();
      if (owners)
        for (auto const &owner : *owners)
          for (uint64_t dir = 0; dir < 3; ++dir)
            if (owner.dirs & (1ull << dir))
              owned_directions[dir].insert(owner.user);
    }
    for (uint64_t dir = 0; dir < 3; ++dir)
      if (!direction_returns[dir].empty()) {
        direction_returns[dir].pop_front();
        mask |= 1ull << dir;
      }
    ts_returns->valid_mask = mask;
    TakeRelease();
    TakeRetire();
    ReturnCredit();
    // 先收再仲裁：本拍交进来的这一笔，没有冲突时本拍就发出去，入口缓冲上不多
    // 占一拍。缓冲是给“资源不够、留着等”的那些用的，不是流水线的一级。
    Collect();
    Arbitrate();
    PublishRoom();
    PublishHold();
    PublishLevel();

    granted = granted_pending;
    stalled = stalled_pending;
    overflow = overflow_pending;
    TracePerCycle("granted", granted_pending);
    TracePerCycle("stalled", stalled_pending);
  }

private:
  // 收 Retire 转过来的下游 release。同一笔会连着两拍出现在端口上，按序号认它。
  void TakeRelease() {
    if (!release_in || !release_in->Valid())
      return;
    if (release_in->Seq() == last_release_seq)
      return;
    last_release_seq = release_in->Seq();
    ReleaseUser(release_in->Dir(), release_in->User());
  }

  // 收 Retire 的广播。同一笔会连着两拍出现在端口上，按序号认它。
  void TakeRetire() {
    if (!retire_in || !retire_in->Valid())
      return;
    if (retire_in->Seq() == last_retire_seq)
      return;
    last_retire_seq = retire_in->Seq();
    RetireUser(retire_in->User());
  }

  std::shared_ptr<DirectionOwnerPort> ts_owners;
  std::shared_ptr<DirectionReturnPort> ts_returns;
  std::array<std::set<uint64_t>, 3> owned_directions;
  std::array<uint64_t, 3> ts_return_count{};
  std::array<std::deque<uint64_t>, 3> direction_returns;
  std::shared_ptr<StreamReleasePort> release_in;
  std::shared_ptr<RetireBroadcastPort> retire_in;
  uint64_t last_release_seq = 0, last_retire_seq = 0;

  // 下游 flit 离开它的 VC Buffer 就还一个 credit。回填规则与占用规则对称：
  // private 没满补 private，满了补 shared。
  void ReturnCredit() {
    for (uint64_t o = 0; o < back_link.size(); ++o) {
      if (!back_link[o])
        continue;
      ReleaseView r = ReadRelease(back_link[o]->release);
      if (!r.vc_valid)
        continue;
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
      if (!reqs[in])
        continue;
      XbarReqView v = ReadXbarReq(*reqs[in]);
      if (!v.valid || v.seq == last_seq[in])
        continue;
      last_seq[in] = v.seq;
      v.from_mesh = in < kInLocal;
      v.from_local = in == kInLocal;
      LOGCHECK(v.vc < kVcNum, "Xbar: VC 号越界。");
      LOGCHECK(in_q[in][v.vc].size() < kXbarInDepth,
               "Xbar: 入口缓冲满了。上游按 room 发，满就说明门限留少了。");
      in_q[in][v.vc].push_back(v);
    }
  }

  // 每拍发布各入口的各个 VC 还收不收得下。
  void PublishRoom() {
    for (uint64_t in = 0; in < kXbarInNum; ++in) {
      if (!reqs[in])
        continue;
      uint64_t room = 0;
      for (uint64_t v = 0; v < kVcNum; ++v) {
        if (in_q[in][v].size() < kXbarInMark) room |= 1ull << v;
        // 包头已经发出、包尾还在 station 里的那一包，可能被后到的包挡在交叉
        // 开关外面。水位已经到了也不挡这一包：给它留一个位置，station 把包身
        // 抽过来。后到的包仍然不发。
        uint64_t ouser = 0, opath = 0;
        if (OpenUser(in, v, ouser, opath) &&
            !ContinuationQueued(in, v, ouser, opath) &&
            in_q[in][v].size() < kXbarInDepth - 1) {
          room |= 1ull << v;
        }
      }
      reqs[in]->DriveRoom(room);
    }
  }

  // 正在进核的那一包，下一拍是不是已经在入口队列里。
  bool ContinuationQueued(uint64_t in, uint64_t vc, uint64_t user,
                          uint64_t path) const {
    for (auto const& f : in_q[in][vc]) {
      if (f.user_id == user && f.path_id == path) return true;
    }
    return false;
  }

  // 这个入口有没有一包已经进了核、包尾还没到。改送进核优先于普通进核。
  bool OpenOf(uint64_t in, uint64_t& vc, uint64_t& user, uint64_t& path,
              bool& divert) const {
    if (divert_[in].on) {
      vc = divert_[in].vc;
      user = divert_[in].user;
      path = divert_[in].path;
      divert = true;
      return true;
    }
    if (core_pkt_[in].on) {
      vc = core_pkt_[in].vc;
      user = core_pkt_[in].user;
      path = core_pkt_[in].path;
      divert = false;
      return true;
    }
    return false;
  }

  // 这一跳被挡住，是不是只因为下游 mesh 的 Stream 不够，或下游连包头这一个
  // flit 都收不下。口被占、Core 或 Reduce 反压都不算。
  bool StreamShortOnly(uint64_t in, XbarReqView const& v,
                       std::array<bool, kXbarOutNum> const& taken) const {
    bool stream_short = false;
    bool credit_short = false;
    bool locked = false;
    bool other = false;
    for (uint64_t o = 0; o < kXbarOutNum; ++o) {
      if (((v.out_mask >> o) & 1u) == 0) continue;
      if (!OwnerOk(o, v.vc, in)) {
        locked = true;
        continue;
      }
      if (taken[o]) {
        other = true;
        continue;
      }
      if (ResourceOk(o, v)) continue;
      if (MeshStreamShort(o, v)) stream_short = true;
      else if (credit_divert && MeshCreditShort(o, v)) credit_short = true;
      else other = true;
    }
    // 下游 credit 用完时整包进核，否则多播 wormhole 会成环；出口被别的包锁着
    // 只是等它发完，不改送。
    if (credit_short && locked) return false;
    return (stream_short || credit_short) && !other;
  }

  bool MeshCreditShort(uint64_t o, XbarReqView const& v) const {
    return o < kR2RNum && private_cr[o][v.vc] == 0 && shared_cr[o] == 0;
  }

  bool MeshStreamShort(uint64_t o, XbarReqView const& v) const {
    if (o >= kR2RNum || pass_through || !Needs(v, o)) return false;
    if (!StreamOk(o, v.user_id) || WaitAhead(o, v)) return true;
    return stream_tab[o].count(v.user_id) == 0 &&
           core_credit[o] < v.credit_require;
  }

  // 这个入口这条 VC 上，有没有一包已经发出包头、包尾还没发出。
  bool OpenUser(uint64_t in, uint64_t vc, uint64_t& user, uint64_t& path) const {
    if (divert_[in].on && divert_[in].vc == vc) {
      user = divert_[in].user;
      path = divert_[in].path;
      return true;
    }
    if (core_pkt_[in].on && core_pkt_[in].vc == vc) {
      user = core_pkt_[in].user;
      path = core_pkt_[in].path;
      return true;
    }
    if (flight_[in][vc].on) {
      user = flight_[in][vc].user;
      path = flight_[in][vc].path;
      return true;
    }
    return false;
  }

  // 纯转发（不进核、不进归约）的包，包尾发出去才告诉本核这一笔走完。包头一发
  // 就退休违反 F-036：release 回到上游时包身还在上游 Xbar，后面的 flit 又把
  // 这个 user 占回上游的 stream 坑和 credit，再也没有 release 来还。
  // 本核 TS 自己发出的包和归约结果不算转发：它们在本核已有自己的任务，包尾
  // 晚于 TS 退休才离开时再报一次，会给已退休的用户另起一项永远等不到数据的槽。
  void NoteForwardDone(uint64_t in, XbarReqView const& v) {
    constexpr uint64_t kStayMask = (1ull << kOutCore) | (1ull << kOutReduce0) |
                                   (1ull << kOutReduce1) | (1ull << kOutReduce2);
    if (in >= kInLocal) return;
    if (v.tail && v.msg && (v.out_mask & kStayMask) == 0 && forward_skip) {
      forward_skip(v.user_id, v.path_id);
    }
  }

  // 包头发出去就占住这条 VC，包尾发出去才放开。单 flit 整包当拍放开。
  void NoteFlight(uint64_t in, XbarReqView const& v) {
    if (v.tail) {
      if (flight_[in][v.vc].on && flight_[in][v.vc].user == v.user_id &&
          flight_[in][v.vc].path == v.path_id) {
        flight_[in][v.vc].on = false;
      }
      return;
    }
    if (!v.head) return;
    flight_[in][v.vc].on = true;
    flight_[in][v.vc].user = v.user_id;
    flight_[in][v.vc].path = v.path_id;
  }

  // 记下送进本核的那一包。包尾之前，同一条 VC 上别的包不能再进核。
  void NoteCore(uint64_t in, XbarReqView const& v, bool divert) {
    if (v.tail) {
      core_pkt_[in].on = false;
      return;
    }
    if (!v.head) return;
    core_pkt_[in].on = true;
    core_pkt_[in].vc = v.vc;
    core_pkt_[in].user = v.user_id;
    core_pkt_[in].path = v.path_id;
    core_pkt_[in].divert = divert;
  }

  // 告诉 station：哪条 VC 先别发后面的包，以及要不要把还没发完的包身抽出来。
  void PublishHold() {
    for (uint64_t in = 0; in < kXbarInNum; ++in) {
      if (!reqs[in]) continue;
      uint64_t block = 0;
      // F-012 与第 505–506 行：包头已经发出、包尾还没发出的那一包独占这条
      // VC，包身还在上游时同样不让后面的包插进来，它自己的包身由 hold_pull
      // 抽出来先发。整包进核等重发的那一包不再占着这条 VC（F-027 的保序只管
      // 重发包之间的先后，见 DivertToCore）。
      for (uint64_t v = 0; v < kVcNum; ++v) {
        if (flight_[in][v].on) block |= 1ull << v;
      }
      if (divert_[in].on) block |= 1ull << divert_[in].vc;
      if (core_pkt_[in].on) block |= 1ull << core_pkt_[in].vc;
      reqs[in]->hold_block = block;
      uint64_t pvc = 0, puser = 0, ppath = 0;
      bool pdiv = false;
      bool pull = false;
      if (OpenOf(in, pvc, puser, ppath, pdiv) &&
          !ContinuationQueued(in, pvc, puser, ppath)) {
        pull = true;
      } else {
        for (uint64_t v = 0; v < kVcNum; ++v) {
          if (!flight_[in][v].on) continue;
          if (ContinuationQueued(in, v, flight_[in][v].user, flight_[in][v].path)) {
            continue;
          }
          pvc = v;
          puser = flight_[in][v].user;
          ppath = flight_[in][v].path;
          pull = true;
          break;
        }
      }
      if (pull) {
        reqs[in]->hold_pull = 1;
        reqs[in]->hold_vc = pvc;
        reqs[in]->hold_user = puser;
        reqs[in]->hold_path = ppath;
      } else {
        reqs[in]->hold_pull = 0;
      }
    }
  }

  // 一笔授予之后都走这里。in_packet 记在入口上而不是记“上一拍发的是谁”：一个包
  // 的两个 flit 之间可能隔着一拍，那一拍会被别的入口占掉。
  void AfterGrant(uint64_t in, XbarReqView const& v) {
    in_packet[in] = !v.tail;
  }

  // 一个入口各 VC 的队首按交进来的先后排：station 的序号一笔加一，小的先到。
  std::vector<uint64_t> VcsByAge(uint64_t in) const {
    std::vector<uint64_t> vcs;
    for (uint64_t v = 0; v < kVcNum; ++v) {
      if (!in_q[in][v].empty())
        vcs.push_back(v);
    }
    std::sort(vcs.begin(), vcs.end(), [&](uint64_t a, uint64_t b) {
      return in_q[in][a].front().seq < in_q[in][b].front().seq;
    });
    return vcs;
  }

  void Arbitrate() {
    // 先把各输出口置空，本拍没人用就是 idle。
    std::array<bool, kXbarOutNum> taken{};
    overflow_used = false;
    std::vector<uint64_t> order = IssueOrder();

    for (uint64_t in : order) {
      // 各 VC 的队首按先后逐个试，一个入口一拍最多授予一笔。最早那一笔卡住时
      // 别的 VC 照样能走。
      std::vector<uint64_t> vcs = VcsByAge(in);
      if (!vcs.empty() && !TryInput(in, vcs, taken))
        ++stalled_pending;
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
    if (!overflow_used)
      overflow_port->Idle();
  }

  // 本核这一拍接不接得下这一笔：口没被占、整包锁没被别人拿着、缓冲和
  // stream 槽都够。改送进核走的就是这条，不再看原目标。
  bool CoreCanTake(uint64_t in, XbarReqView const& v,
                   std::array<bool, kXbarOutNum> const& taken) const {
    if (taken[kOutCore] || !OwnerOk(kOutCore, v.vc, in)) return false;
    if (!ResourceOk(kOutCore, v)) return false;
    // 改送进核的包要在本核起一笔重发任务，占本核一项 stream；原表项多半没置
    // 进核那一位，不在这里查，满了的核也会被塞进新用户。
    return pass_through || StreamOk(kOutCore, v.user_id);
  }

  // 把这一 flit 送进本核，并标上重发。包尾之前同一条 VC 上的后续包不能越过它；
  // 包尾进核之后这条 VC 就让出来，后面的包只要资源够就照常走。F-027 的保序只管
  // 重发包自己：重发时仍用原来的 VC，彼此按先后发；它不让别的包等这条 VC。
  void DivertToCore(uint64_t in, std::deque<XbarReqView>& q,
                    std::deque<XbarReqView>::iterator it,
                    std::array<bool, kXbarOutNum>& taken) {
    XbarReqView v = *it;
    // 改送那一刻起，这个用户在缺 stream 的方向上排队等重发；同 VC 后到的新用户
    // 看到有人在等也要改送进核，由 TS 按用户年龄排出先后，不能直通抢走下游
    // 归还的那一项。
    if (v.head) {
      for (uint64_t o = 0; o < kR2RNum; ++o)
        if (((v.out_mask >> o) & 1u) &&
            (MeshStreamShort(o, v) ||
             (credit_divert && MeshCreditShort(o, v))))
          reissue_wait_[o].emplace(std::make_pair(v.user_id, v.path_id), v.vc);
    }
    // 多播的几个方向共享同一个 Message。重发标记只打在送进本核的这一份上，
    // 不能改共享对象，否则还在别的路口上的副本也会被当成已经改道。
    if (v.head && v.msg) {
      auto copied = std::make_shared<Message>(*v.msg);
      copied->reissue = 1;
      v.msg = copied;
    }
    taken[kOutCore] = true;
    Consume(kOutCore, v);
    if (!pass_through)
      stream_tab[kOutCore].insert(v.user_id);
    Emit(kOutCore, v);
    vc_owner[kOutCore][v.vc] = v.tail ? 0 : in + 1;
    NoteCore(in, v, true);
    NoteFlight(in, v);
    AfterGrant(in, v);
    ++granted_pending;
    q.erase(it);
    if (v.tail) {
      divert_[in].on = false;
    } else {
      divert_[in].on = true;
      divert_[in].vc = v.vc;
      divert_[in].user = v.user_id;
      divert_[in].path = v.path_id;
    }
  }

  // 进核那一包的下一拍如果已经在队列里，这一拍只发它。改送的只进核；普通进核
  // 的仍按原目标整组发出。后面的包留在队列里。
  bool ContinueOpen(uint64_t in, std::array<bool, kXbarOutNum>& taken) {
    uint64_t vc = 0, user = 0, path = 0;
    bool divert = false;
    if (!OpenOf(in, vc, user, path, divert)) return false;
    std::deque<XbarReqView>& q = in_q[in][vc];
    auto it = q.begin();
    for (; it != q.end(); ++it) {
      if (it->user_id == user && it->path_id == path) break;
    }
    if (it == q.end()) return false;
    LOGCHECK(!it->head,
             "Xbar: 进核的包还没到尾，同用户同 path 又来了新包头。");
    if (divert) {
      if (!CoreCanTake(in, *it, taken)) return false;
      DivertToCore(in, q, it, taken);
      return true;
    }
    bool ok = true;
    for (uint64_t o = 0; o < kXbarOutNum; ++o) {
      if (((it->out_mask >> o) & 1u) == 0) continue;
      if (!OwnerOk(o, it->vc, in) || taken[o] || !ResourceOk(o, *it)) {
        ok = false;
        break;
      }
    }
    if (!ok) return false;
    XbarReqView v = *it;
    q.erase(it);
    for (uint64_t o = 0; o < kXbarOutNum; ++o) {
      if (((v.out_mask >> o) & 1u) == 0) continue;
      taken[o] = true;
      Consume(o, v);
      Emit(o, v);
      vc_owner[o][v.vc] = v.tail ? 0 : in + 1;
      if (o == kOutCore) NoteCore(in, v, false);
    }
    NoteFlight(in, v);
    ++granted_pending;
    AfterGrant(in, v);
    return true;
  }

  // 任一方向：包头已经发出、包尾还没发出的，这一拍只续这一包。改送进核和普通
  // 进核仍由 ContinueOpen 处理。这里补上不进核的那一包，比如只往东的 path。
  // 这一拍发不走就换别的 VC，同一条 VC 上后面的包留着。
  bool ContinueFlight(uint64_t in, std::array<bool, kXbarOutNum>& taken) {
    for (uint64_t vc = 0; vc < kVcNum; ++vc) {
      if (!flight_[in][vc].on) continue;
      if (divert_[in].on && divert_[in].vc == vc) continue;
      if (core_pkt_[in].on && core_pkt_[in].vc == vc) continue;
      std::deque<XbarReqView>& q = in_q[in][vc];
      auto it = q.begin();
      for (; it != q.end(); ++it) {
        if (it->user_id == flight_[in][vc].user &&
            it->path_id == flight_[in][vc].path) {
          break;
        }
      }
      if (it == q.end()) continue;
      LOGCHECK(!it->head,
               "Xbar: 这一包还没到尾，同用户同 path 又来了新包头。");
      bool ok = true;
      for (uint64_t o = 0; o < kXbarOutNum; ++o) {
        if (((it->out_mask >> o) & 1u) == 0) continue;
        if (!OwnerOk(o, it->vc, in) || taken[o] || !ResourceOk(o, *it)) {
          ok = false;
          break;
        }
      }
      if (!ok) continue;
      XbarReqView v = *it;
      q.erase(it);
      NoteForwardDone(in, v);
      for (uint64_t o = 0; o < kXbarOutNum; ++o) {
        if (((v.out_mask >> o) & 1u) == 0) continue;
        taken[o] = true;
        Consume(o, v);
        Emit(o, v);
        vc_owner[o][v.vc] = v.tail ? 0 : in + 1;
        if (o == kOutCore) NoteCore(in, v, false);
      }
      NoteFlight(in, v);
      ++granted_pending;
      AfterGrant(in, v);
      return true;
    }
    return false;
  }

  // 一个入口这一拍：vcs 是它各 VC 的队首按先后排好的次序，授予了一笔返回真。
  bool TryInput(uint64_t in, std::vector<uint64_t> const& vcs,
                std::array<bool, kXbarOutNum>& taken) {
    // 包头已经发出、包尾还没到的，同一条 VC 上只续这一包。改送的只进核，普通
    // 进核和只走 mesh 的仍走原目标。后面的包先不发。包身还没进队列就让别的 VC 走。
    if (divert_[in].on || core_pkt_[in].on) {
      if (ContinueOpen(in, taken)) return true;
    }
    if (ContinueFlight(in, taken)) return true;
    for (uint64_t k = 0; k < vcs.size(); ++k) {
      if (divert_[in].on && vcs[k] == divert_[in].vc) continue;
      if (core_pkt_[in].on && vcs[k] == core_pkt_[in].vc) continue;
      // 未发完的那一包独占这条 VC：包尾之前不授予别的包，包身还在上游也等。
      if (flight_[in][vcs[k]].on) continue;
      std::deque<XbarReqView>& q = in_q[in][vcs[k]];
      if (q.empty()) continue;
      XbarReqView const& v = q.front();
      constexpr uint64_t reduce_mask = (1ull << kOutReduce0) |
          (1ull << kOutReduce1) | (1ull << kOutReduce2);
      if (v.head && (v.out_mask & reduce_mask) && reduce_admission && v.msg &&
          !reduce_admission(*v.msg)) {
        hold_why[in] = 1;
        hold_path[in] = v.path_id;
        continue;
      }

      // 多播全有全无：先看所有目标出口是不是都空着、资源都够。另一个入口的包
      // 正占着这个出口的这个 VC 时只能等，不算资源不够，不走转存。
      bool ok = true, owner_busy = false;
      for (uint64_t o = 0; o < kXbarOutNum; ++o) {
        if (((v.out_mask >> o) & 1u) == 0)
          continue;
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
        if (v.head && (v.out_mask & reduce_mask)) {
          hold_why[in] = owner_busy ? 2 : 3;
          hold_path[in] = v.path_id;
        }
        // F-025：整包改送进核只发生在下游 Stream 不够、stallWay=1、还没向原目标
        // 交过 flit。下一跳 VC credit 不够、口被占、Core/Reduce 反压都不改送，
        // 包留在当前 VC 等。credit 也同时不够时先等 credit，避免原目标已经能收
        // 之前就把整包改道。另一个入口正占着出口时不算资源不够。本核这一拍收不下
        // 就留在 VC 里等。本核发出去的重发包不再改送回本核。
        if (in != kInLocal && k == 0 && !core_pkt_[in].on && !divert_[in].on &&
            v.stall_way && v.head && !owner_busy &&
            StreamShortOnly(in, v, taken) && CoreCanTake(in, v, taken)) {
          DivertToCore(in, q, q.begin(), taken);
          return true;
        }
        continue;
      }

      if (v.head && (v.out_mask & reduce_mask) && reduce_reserve && v.msg) {
        reduce_reserve(*v.msg);
      }
      // 改送进核的那一档已经在上面返回，不会到这里。
      NoteForwardDone(in, v);
      for (uint64_t o = 0; o < kXbarOutNum; ++o) {
        if (((v.out_mask >> o) & 1u) == 0)
          continue;
        taken[o] = true;
        Consume(o, v);
        Emit(o, v);
        vc_owner[o][v.vc] = v.tail ? 0 : in + 1;
        if (o == kOutCore) NoteCore(in, v, false);
      }
      NoteFlight(in, v);

      if (v.head && (v.out_mask & reduce_mask)) {
        hold_why[in] = 4;
        hold_path[in] = v.path_id;
      }
      ++granted_pending;
      AfterGrant(in, v);
      q.pop_front();
      return true;
    }
    return false;
  }

  // 队头那包进不了归约时，从它后面挑一拍已经能进的发出去。队头不动。
  bool GrantPastHold(uint64_t in, std::deque<XbarReqView>& q,
                     std::array<bool, kXbarOutNum>& taken) {
    (void)in;
    (void)q;
    (void)taken;
    return false;
#if 0
    constexpr uint64_t reduce_mask = (1ull << kOutReduce0) |
        (1ull << kOutReduce1) | (1ull << kOutReduce2);
    for (auto it = std::next(q.begin()); it != q.end(); ++it) {
      XbarReqView const& cand = *it;
      uint64_t seq = cand.msg ? cand.msg->reduce_seq : 0;
      bool mid = bypass_on[in] && cand.user_id == bypass_user[in] &&
                 cand.path_id == bypass_path[in] && seq == bypass_seq[in];
      bool fresh = !bypass_on[in] && cand.head && cand.msg &&
                   (cand.out_mask & reduce_mask) && WaitingInput(*cand.msg);
      if (!mid && !fresh) continue;
      bool ok = true;
      for (uint64_t o = 0; o < kXbarOutNum; ++o) {
        if (((cand.out_mask >> o) & 1u) == 0) continue;
        if (!OwnerOk(o, cand.vc, in) || taken[o] || !ResourceOk(o, cand)) {
          ok = false;
          break;
        }
      }
      if (!ok) return false;
      XbarReqView v = cand;
      q.erase(it);
      if (v.head && (v.out_mask & reduce_mask) && reduce_reserve && v.msg) {
        reduce_reserve(*v.msg);
      }
      for (uint64_t o = 0; o < kXbarOutNum; ++o) {
        if (((v.out_mask >> o) & 1u) == 0) continue;
        taken[o] = true;
        Consume(o, v);
        Emit(o, v);
        vc_owner[o][v.vc] = v.tail ? 0 : in + 1;
      }
      if (v.head && (v.out_mask & reduce_mask)) {
        hold_why[in] = 4;
        hold_path[in] = v.path_id;
      }
      ++granted_pending;
      AfterGrant(in, v);
      bypass_on[in] = !v.tail;
      if (!v.tail) {
        bypass_user[in] = v.user_id;
        bypass_path[in] = v.path_id;
        bypass_seq[in] = seq;
      }
      return true;
    }
#endif
    return false;
  }

  // 归约准入没过的一包。open 时尾 flit 还没到，后续 flit 在 Collect 里接上。
  struct Aside {
    bool open = false;
    uint64_t user = 0, path = 0, seq = 0;
    std::deque<XbarReqView> flits;
  };

  static bool SameAside(Aside const& a, XbarReqView const& v) {
    uint64_t seq = v.msg ? v.msg->reduce_seq : 0;
    return v.user_id == a.user && v.path_id == a.path && seq == a.seq;
  }

  // 把这个 VC 队头那一整包（队列里已经到的部分）挪到旁边。
  void PeelAside(uint64_t in, uint64_t vc) {
    std::deque<XbarReqView>& q = in_q[in][vc];
    if (q.empty() || !q.front().head) return;
    Aside a;
    a.open = true;
    a.user = q.front().user_id;
    a.path = q.front().path_id;
    a.seq = q.front().msg ? q.front().msg->reduce_seq : 0;
    while (!q.empty()) {
      XbarReqView const& f = q.front();
      if (!a.flits.empty() && !SameAside(a, f)) break;
      bool tail = f.tail;
      a.flits.push_back(f);
      q.pop_front();
      if (tail) {
        a.open = false;
        break;
      }
    }
    aside[in][vc].push_back(std::move(a));
  }

  // 旁边最早那一包现在发得走就发一个 flit。1 发出去了，2 包已经开头但这一拍
  // 发不走（不能让别的包插进这个 VC），0 没有可发的。
  int TryAside(uint64_t in, uint64_t vc, std::array<bool, kXbarOutNum>& taken) {
    auto& dq = aside[in][vc];
    while (!dq.empty() && dq.front().flits.empty() && !dq.front().open) {
      dq.pop_front();
    }
    if (dq.empty()) return 0;
    if (dq.front().flits.empty()) return dq.front().open ? 2 : 0;
    XbarReqView v = dq.front().flits.front();
    constexpr uint64_t reduce_mask = (1ull << kOutReduce0) |
        (1ull << kOutReduce1) | (1ull << kOutReduce2);
    bool started = !v.head;
    if (v.head && (v.out_mask & reduce_mask) && reduce_admission && v.msg &&
        !reduce_admission(*v.msg)) {
      return 0;
    }
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
      if (v.head && (v.out_mask & reduce_mask)) {
        hold_why[in] = owner_busy ? 2 : 3;
        hold_path[in] = v.path_id;
      }
      return started ? 2 : 0;
    }
    if (v.head && (v.out_mask & reduce_mask) && reduce_reserve && v.msg) {
      reduce_reserve(*v.msg);
    }
    NoteForwardDone(in, v);
    for (uint64_t o = 0; o < kXbarOutNum; ++o) {
      if (((v.out_mask >> o) & 1u) == 0) continue;
      taken[o] = true;
      Consume(o, v);
      Emit(o, v);
      vc_owner[o][v.vc] = v.tail ? 0 : in + 1;
    }
    if (v.head && (v.out_mask & reduce_mask)) {
      hold_why[in] = 4;
      hold_path[in] = v.path_id;
    }
    ++granted_pending;
    AfterGrant(in, v);
    dq.front().flits.pop_front();
    if (dq.front().flits.empty() && !dq.front().open) dq.pop_front();
    return 1;
  }

  // 这个入口最早交进来、还在等的那一笔；没有返回空。
  XbarReqView const *Oldest(uint64_t in) const {
    XbarReqView const *p = nullptr;
    for (uint64_t v = 0; v < kVcNum; ++v) {
      if (in_q[in][v].empty())
        continue;
      if (!p || in_q[in][v].front().seq < p->seq)
        p = &in_q[in][v].front();
    }
    return p;
  }

  // 贪婪整包，三档：还没发完一个包的入口排在最前，其次是队列里攥着一整个包的，
  // 其余按 RoundRobin。每一档内部都按 RoundRobin 的顺序走，所以同一档里的几个
  // 入口仍然轮流。
  std::vector<uint64_t> IssueOrder() {
    std::vector<uint64_t> order;
    for (uint64_t k = 1; k <= kXbarInNum; ++k) {
      uint64_t in = (rr_last + k) % kXbarInNum;
      if (in_packet[in])
        order.push_back(in);
    }
    for (uint64_t k = 1; k <= kXbarInNum; ++k) {
      uint64_t in = (rr_last + k) % kXbarInNum;
      XbarReqView const *p = Oldest(in);
      if (in_packet[in] || !p)
        continue;
      if (p->whole_packet)
        order.push_back(in);
    }
    for (uint64_t k = 1; k <= kXbarInNum; ++k) {
      uint64_t in = (rr_last + k) % kXbarInNum;
      if (in_packet[in])
        continue;
      XbarReqView const *p = Oldest(in);
      if (p && p->whole_packet)
        continue;
      order.push_back(in);
    }
    rr_last = (rr_last + 1) % kXbarInNum;
    return order;
  }

  // 同一出口同一 VC 上一个包从首 flit 到尾 flit 独占：包中间隔了几拍，别的入口
  // 也不能在这个 VC 上插进来，否则下游按首尾拼包会拼乱。
  //
  // 进 core 那个出口整个独占，不分 VC：core 内的输入不支持几个包交织，发完一
  // 个包才调度下一个。下游 CoreStation 到 DTE 只有一条通路，不按 VC 分开缓冲。
  bool OwnerOk(uint64_t o, uint64_t vc, uint64_t in) const {
    LOGCHECK(vc < kVcNum, "Xbar: VC 号越界。");
    if (o == kOutCore) {
      for (uint64_t v = 0; v < kVcNum; ++v) {
        if (v != vc && vc_owner[o][v] != 0)
          return false;
      }
    }
    return vc_owner[o][vc] == 0 || vc_owner[o][vc] == in + 1;
  }

  bool ResourceOk(uint64_t o, XbarReqView const &v) const {
    if (o == kOutCore) {
      if (!force_ready) {
        // 新包头要包头队列和数据缓冲都有空。已经发出头的那个包，后续 flit
        // 不进包头队列；队列满了还挡它，reduce 结果就交不完，读包头的任务
        // 又在等这份结果。
        bool head_ok = core_level->Ready();
        bool data_ok = data_level ? data_level->Ready() : head_ok;
        if (v.head ? !head_ok : !data_ok)
          return false;
      }
      // 进本 core 的三态准入在这一级做（F-021）：已占的直接过，没占但有空项也
      // 过，满了就不往 core 发，这一笔留在它那个 VC 上等，别的 VC 照走。落
      // Matrix Mem 的 B core 与 R core 不占这一项，表项那一位不置。
      return !Needs(v, kOutCore) || StreamOk(kOutCore, v.user_id);
    }
    if (o >= kOutReduce0) {
      return force_ready || reduce_level[o - kOutReduce0]->Ready();
    }
    // 链路级的 VC credit 与本级派不派角色无关：它记的是下一跳 VC Buffer 还有
    // 几个位置，不看就发会把下一跳撑爆。只透传的 core 不投递本 core、不占
    // stream 坑、不扣 Core Mem 的量，所以只查这一项。
    if (private_cr[o][v.vc] == 0 && shared_cr[o] == 0)
      return false;
    if (pass_through)
      return true;
    if (!Needs(v, o))
      return true;
    if (!StreamOk(o, v.user_id) || WaitAhead(o, v))
      return false;
    // 已经占过坑的用户不再扣量：坑按 user 记，量也跟着那一次记。
    return stream_tab[o].count(v.user_id) != 0 ||
           core_credit[o] >= v.credit_require;
  }

  // 这一笔往出口 o 发之前查不查那一侧的 stream 资源，看表项的 streamNeedMask。
  static bool Needs(XbarReqView const &v, uint64_t o) {
    return ((v.stream_need >> o) & 1u) != 0;
  }

  // 每个 (出口, VC) 一个计数：改送进核 +1，那一包重发的包头从本核发出 -1。
  // 计数不为 0 时网上来的包不直通，一律改送进核排在后面，占没占下游一项都
  // 一样。本核发出的不在这里挡：重发包本身不能挡，普通发送由 TS 按用户年龄
  // 授权。reduce 结果不能改送，已占下游一项的照走，没占的等。
  bool WaitAhead(uint64_t o, XbarReqView const &v) const {
    if (v.from_local)
      return false;
    if (!v.from_mesh && stream_tab[o].count(v.user_id) != 0)
      return false;
    for (auto const& [up, vc] : reissue_wait_[o])
      if (vc == v.vc && (v.from_mesh || up.first != v.user_id))
        return true;
    return false;
  }

  // 三态准入：已占的直接过；没占但有空项则过（占用在 Consume 里做）；满了不过。
  bool StreamOk(uint64_t o, uint64_t user) const {
    if (stream_tab[o].count(user) != 0)
      return true;
    return stream_tab[o].size() < kStreamTabEntries;
  }

  void Consume(uint64_t o, XbarReqView const &v) {
    ++sent_total[o];
    if (o == kOutCore) {
      if (!pass_through && Needs(v, o))
        stream_tab[o].insert(v.user_id);
      return;
    }
    if (o >= kOutReduce0)
      return;
    if (private_cr[o][v.vc] > 0) {
      --private_cr[o][v.vc];
    } else {
      --shared_cr[o];
    }
    // 等重发的用户包头发出去才不再挡后来者：许可到真正发出之间 DTE 还要读 CM，
    // 这段时间放直通包过去，到下游就排在它前面了。包头占住这一出口的 VC 之后，
    // 同 VC 的后来者只能跟在包尾后面。按 (user, path) 清：同一用户别的 path
    // 先从这一出口发出，不代表等重发的那个包已经发出。
    if (v.head && v.from_local) {
      auto rec = reissue_wait_[o].find({v.user_id, v.path_id});
      if (rec != reissue_wait_[o].end())
        reissue_wait_[o].erase(rec);
    }
    if (pass_through)
      return;
    // 坑按 user 记，不按包记：同一个 user 第二次发送余额不变。credit 的量也
    // 只在第一次占坑时扣，单位 1 KB。
    if (Needs(v, o) && stream_tab[o].insert(v.user_id).second) {
      LOGCHECK(core_credit[o] >= v.credit_require,
               "Xbar: Core Mem credit 不够就发了。");
      core_credit[o] -= v.credit_require;
      taken_kb[o][v.user_id] += v.credit_require;
    }
  }

  void Emit(uint64_t o, XbarReqView const &v) {
    // 本跳把包头里的 VC 改成表里的“下一跳 VC”，供下一跳直接取。
    if (v.msg)
      v.msg->vc = v.vc;
    LinkEnd *dst = nullptr;
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
      if (stream_tab[o].size() < kStreamTabEntries)
        free_mask |= 1ull << o;
    }
    level->Drive(ok, free_mask);
  }

  std::vector<std::shared_ptr<XbarReqPort>> reqs;
  std::shared_ptr<CreditLevelPort> level;
  std::vector<LinkEndPtr> out_link, back_link, reduce_out;
  LinkEndPtr core_out;
  std::shared_ptr<ReadyLevelPort> core_level, data_level;
  std::vector<std::shared_ptr<ReadyLevelPort>> reduce_level;
  std::shared_ptr<OverflowPort> overflow_port;

  // Step 独占。
  VcDepth priv_depth = kVcPrivateDepthDefault;
  uint64_t shared_depth = kVcSharedDepth;
  std::array<VcDepth, kR2RNum> private_cr{};
  std::array<uint64_t, kR2RNum> shared_cr{};
  // 四张 Stream Resource Table：三个下游方向各一张，进本 core 那一侧一张。
  std::array<std::set<uint64_t>, kOutCore + 1> stream_tab;
  std::array<uint64_t, kR2RNum> core_credit{};
  std::array<std::map<uint64_t, uint64_t>, kR2RNum> taken_kb;
  // 硬件只记每个 (出口, VC) 的个数；(user, path) 只用来认出从本核发出的
  // 是不是那一包重发，代替硬件包头上的重发标记。
  std::array<std::multimap<std::pair<uint64_t, uint64_t>, uint64_t>, kR2RNum>
      reissue_wait_;
  bool force_ready = false;
  bool credit_divert = true;
  std::function<bool(Message const &)> reduce_admission;
  std::function<void(uint64_t, uint64_t)> forward_skip;
  std::function<void(Message const &)> reduce_reserve;
  bool overflow_used = false;
  bool pass_through = false;
  std::array<uint64_t, kXbarOutNum> sent_total{};
  std::array<std::array<std::deque<XbarReqView>, kVcNum>, kXbarInNum> in_q;
  // 归约准入没过的包，按 VC 分开，不占 in_q 的队头。
  std::array<std::array<std::deque<Aside>, kVcNum>, kXbarInNum> aside{};
  std::array<uint64_t, kXbarInNum> last_seq{};
  std::array<bool, kXbarInNum> in_packet{};
  // 这个入口正在把一整包改送进核。包尾之前不再试原目标。
  struct Divert {
    bool on = false;
    uint64_t vc = 0;
    uint64_t user = 0;
    uint64_t path = 0;
  };
  std::array<Divert, kXbarInNum> divert_{};
  // 已经向本核交出包头、包尾还没到的那一包。核内不接受别的包插进来。
  struct CorePkt {
    bool on = false;
    bool divert = false;
    uint64_t vc = 0;
    uint64_t user = 0;
    uint64_t path = 0;
  };
  std::array<CorePkt, kXbarInNum> core_pkt_{};
  // 包头已经从这入口这条 VC 发出，包尾还没有。包尾之前这条 VC 只发这一包，
  // 包身还在上游也不让后面的包插进来。不因此登记 Reissue。
  struct Flight {
    bool on = false;
    uint64_t user = 0;
    uint64_t path = 0;
  };
  std::array<std::array<Flight, kVcNum>, kXbarInNum> flight_{};
  // 从被挡住的队头后面抽出来、还没发完的那一包。
  std::array<bool, kXbarInNum> bypass_on{};
  std::array<uint64_t, kXbarInNum> bypass_user{}, bypass_path{}, bypass_seq{};
  // 各出口各 VC 正被哪个入口的包占着：入口号加一，0 表示空着。
  std::array<std::array<uint64_t, kVcNum>, kXbarOutNum> vc_owner{};
  std::array<int, kXbarInNum> hold_why{};
  std::array<uint64_t, kXbarInNum> hold_path{};
  uint64_t rr_last = 0;
  uint64_t granted_pending = 0, stalled_pending = 0, overflow_pending = 0;

  Logic64 granted, stalled, overflow;
};

} // namespace bach
} // namespace latch

#endif
