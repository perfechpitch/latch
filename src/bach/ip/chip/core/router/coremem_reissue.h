#ifndef _LATCH_BACH_IP_CHIP_CORE_ROUTER_COREMEM_REISSUE_
#define _LATCH_BACH_IP_CHIP_CORE_ROUTER_COREMEM_REISSUE_

// CoreMem 重发：stall_way 选转存那一档的落地。
//
// 两级：
//   M13 进 core 暂存  拿不到下游资源且 stall_way 选转存时，把整包重定向到本地
//                     Core Mem。Router 上的 Bypass 操作因此被映射成「进 core 加
//                     出 core」两段。存的时候改写 overflow_reinject = 1
//   M14 取出重发      资源回来后取出，用 PathID 重新查 RouterTable，不重复保存
//                     VC 与路由信息；Router 把 overflow_reinject 改回 0，
//                     Output Port 识别到这个标记才扣 credit
//
// 同 VC 保序：同一 VC 存在未完成的重发包时，后续包不得越过。按 VC 粒度维护
// pending_reinject 计数器防超车。
//
// 容量由软件在 Core Mem 里预留，取自 cmem_part 的 reissue_base 与
// reissue_pkts_per_vc，每 VC 至少容得下一个整包。计数到上限说明配少了，模型
// 直接断言失败：不覆盖已暂存的包，不丢包，也不退回「留在当前 VC 等」，因为那条路
// 会让同一个 stall_way 配置在两种容量下走出两种行为，把配置错误掩盖过去。
//
// 不派角色的 core 不接收溢流：Router 对它不发起进 core 缓存处理，此时 coremem
// credit 直接 bypass。
//
// 重发出去之后向本 core 的 TS 报一笔，至少带 UserID 与 PathID：直接发送那一路
// 由 CoreStation 的 trigger 通知 TS，走了暂存的这一路要在这里补上，否则 TS 那边
// 的重发标记一直挂着。

#include <array>
#include <deque>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// Router 的 CoreMem 重发 → TS：这一笔重发完了。
class ReissueDonePort : public Logic {
 public:
  Logic64 valid, user_id, path_id, seq;

  explicit ReissueDonePort(ClockPtr c)
      : valid(c), user_id(c), path_id(c), seq(c) {
    Fields(valid, user_id, path_id, seq);
  }

  void Drive(uint64_t user, uint64_t path, uint64_t n) {
    valid = 1;
    user_id = user;
    path_id = path;
    seq = n;
  }
  void Idle() {
    valid = 0;
    user_id = 0;
    path_id = 0;
    seq = seq.Get();
  }
  bool Valid() const { return valid.Get() != 0; }
  uint64_t User() const { return user_id.Get(); }
  uint64_t Path() const { return path_id.Get(); }
  uint64_t Seq() const { return seq.Get(); }
};

class CoreMemReissue : public BachModule {
 public:
  CoreMemReissue(ClockPtr clock, const std::string& name,
                 uint64_t pkts_per_vc, uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        cap_per_vc(pkts_per_vc),
        out_wire(std::make_shared<LinkEnd>(clock)),
        overflow(std::make_shared<OverflowPort>(clock)),
        done(std::make_shared<ReissueDonePort>(clock)),
        stored(clock),
        reissued(clock),
        pending(clock) {}

  // 重发出去的那一路，接 core 方向的 RouterStation。
  LinkEnd& Out() { return *out_wire; }
  void AttachOut(LinkEndPtr wire) { out_wire = std::move(wire); }

  // Xbar 判定转存后从这个口把 flit 交过来。
  void AttachOverflow(std::shared_ptr<OverflowPort> port) {
    overflow = std::move(port);
  }

  // 重发完成后往这个口报一笔，接本 core 的 TS。
  ReissueDonePort& Done() { return *done; }
  std::shared_ptr<ReissueDonePort> DonePtr() const { return done; }

  // 只透传的 core 不接收溢流：Router 对它不发起进 core 缓存处理，此时 coremem
  // credit 直接 bypass。
  void SetPassThrough(bool on) { pass_through = on; }

  // Xbar 判定要转存时调这里。
  void Store(FlitView const& f) {
    LOGCHECK(!pass_through,
             "CoreMemReissue: 不派角色的 core 不接收溢流，这一笔不该转存。");
    LOGCHECK(f.vc < kVcNum, "CoreMemReissue: VC 号越界。");
    LOGCHECK(pending_cnt[f.vc] < cap_per_vc,
             "CoreMemReissue: 这个 VC 的暂存区满了。容量由 cmem_part 的 "
             "reissue_pkts_per_vc 定，满了说明配少了。");
    FlitView s = f;
    if (s.msg) s.msg->reissue = 1;  // overflow_reinject = 1
    buf[f.vc].push_back(s);
    ++pending_cnt[f.vc];
    ++stored_pending;
  }

  // 资源回来后由 CreditMonitor 唤醒：把这个 VC 的队头放出去重发。
  void Wake(uint64_t vc) {
    LOGCHECK(vc < kVcNum, "CoreMemReissue: VC 号越界。");
    wake[vc] = true;
  }

  uint64_t Pending(uint64_t vc) const { return pending_cnt.at(vc); }
  uint64_t Stored() const { return stored.Get(); }
  uint64_t Reissued() const { return reissued.Get(); }

  bool Quiescent() const override {
    for (auto const& q : buf) {
      if (!q.empty()) return false;
    }
    return true;
  }

 protected:
  void Step() override {
    // 末级先做：先把该重发的放出去，腾出的位置本拍就能收新的转存。
    Reissue();
    TakeOverflow();
    stored = stored_pending;
    reissued = reissued_pending;
    uint64_t tot = 0;
    for (uint64_t c : pending_cnt) tot += c;
    pending = tot;
    TracePerCycle("pending", tot);
  }

 private:
  // 从 Xbar 收一笔转存。
  void TakeOverflow() {
    FlitView f = overflow->View();
    if (!f.valid) return;
    Store(f);
  }

  void Reissue() {
    for (uint64_t v = 0; v < kVcNum; ++v) {
      if (!wake[v] || buf[v].empty()) continue;
      // 同 VC 保序：只放队头，后面的不得越过。
      FlitView f = buf[v].front();
      buf[v].pop_front();
      --pending_cnt[v];
      wake[v] = false;
      // 重发时 Router 把标记改回 0，Output Port 识别到它才扣 credit。
      if (f.msg) f.msg->reissue = 0;
      out_wire->flit.Drive(f.vc, f.head, f.tail, f.bytes, f.msg);
      out_wire->release.Idle();
      ++reissued_pending;
      // 整包的最后一拍发出去才算这一笔重发完了，这时候报给 TS。
      if (f.tail && f.msg) {
        done->Drive(f.msg->user_id, f.msg->path_id, ++done_seq);
      } else {
        done->Idle();
      }
      return;  // 每拍最多重发一个
    }
    out_wire->flit.Idle();
    out_wire->release.Idle();
    done->Idle();
  }

  uint64_t cap_per_vc;
  bool pass_through = false;
  LinkEndPtr out_wire;
  std::shared_ptr<OverflowPort> overflow;
  std::shared_ptr<ReissueDonePort> done;

  // Step 独占。
  std::array<std::deque<FlitView>, kVcNum> buf;
  std::array<uint64_t, kVcNum> pending_cnt{};
  std::array<bool, kVcNum> wake{};
  uint64_t stored_pending = 0, reissued_pending = 0, done_seq = 0;

  Logic64 stored, reissued, pending;
};

}  // namespace bach
}  // namespace latch

#endif
