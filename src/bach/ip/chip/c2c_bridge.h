#ifndef _LATCH_BACH_IP_CHIP_C2C_BRIDGE_
#define _LATCH_BACH_IP_CHIP_C2C_BRIDGE_

// C2C Bridge：chip 的四个对外口，每个一座桥。
//
// 只做透明传输：左侧收到的包默认发到右侧，右侧收到的包默认发到左侧，Bridge 不
// 做路由判断。跨 chip 的路由方案都建立在这个前提上：业务上不对 C2C 使用独立
// 的地址编码，接口处做的是流式通信封装。
//
// 四段，各是一个独立打拍的模块：
//   RC / VA / SA   与 core 内 Router 同一套三关，D3。查出口方向与下一跳 VC、
//                  查 credit、同向的数据与 release 之间仲裁（小包优先）
//   TX Engine      按 4 KB 边界拆包，加 4 位 seq_id 与 tail 标记，位宽 2048
//                  转 1024，一拍拆两拍发
//   RX Engine      按 seq_id 缓存，tail 到齐后还原原始包，位宽 1024 转 2048
//   AXI Bridge     credit 与 AXI4 的协议转换。出方向按这一段物理链路的延迟
//                  计时（PCIe C2C 300 ns，按 1 T = 1 ns 折算），入方向只做转
//                  换不再计时，一段线的时间算在发送侧。Router 到 Router 的
//                  400 T 含这一段与两侧 Bridge
//
// 三类 credit 一律透传：Bridge 自身不建 stream credit 表，也不参与 Reduce 累加。
// VC credit 在 Router 上是 flit 粒度，跨 C2C 攒够一个包的量再传一笔，笔上带着
// 它代表几个 flit，收方按这个数展开；业务层那两类本身就是包或 stream 粒度，
// 来一笔传一笔。攒着的余量在本拍没有别的东西要发时一并发掉，不会一直压着。
//
// 对着 PCIe Switch 或 CPU 的那一侧没有对端的 PCIe Bridge，业务层逻辑要能 bypass
// 掉，只保留位宽转换与拆包合包。

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/common/flit.h"
#include "bach/common/params.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// VC Buffer 按方向分档，credit 与它一一对应。
constexpr uint64_t kC2cTxPrivate = 20;    // 每 VC，覆盖本级 R2R 往返约 20 拍
constexpr uint64_t kC2cTxShared = 20;
constexpr uint64_t kC2cRxPrivate = 80;    // 每 VC
constexpr uint64_t kC2cRxShared = 300;    // 覆盖 PCIe 往返 600 ns @1024-bit
constexpr uint64_t kC2cSegBytes = 4096;   // 拆包的边界
constexpr uint64_t kC2cSeqNum = 16;       // seq_id 4 位
constexpr uint64_t kC2cReasmNum = 16;
constexpr uint64_t kC2cAxiLatency = 300;  // PCIe C2C 300 ns，1 T = 1 ns
constexpr uint64_t kC2cRcStages = 3;      // RC / VA / SA 三关
// 从本片 core 收 flit 的缓冲容量。桥收下就把 VC 位置还给 core，所以这个数要与
// Router 给这个方向的 VC credit 总量一致，否则 core 那一侧按自己的额度发，桥这
// 一侧收不下，flit 就丢在线上而 credit 已经扣掉了。
constexpr uint64_t kC2cInCap = kC2cTxPrivate + kC2cTxShared;
// VC credit 攒到几个 flit 打成一笔。原文只说「转成包粒度」，没给数，按 4 KB 的
// 分段边界除以一拍 256 B 取 16 偏大，这里取 4：攒太多会让上游等得久。
constexpr uint64_t kC2cCreditPackFlits = 4;

// 一段：TX 拆出来、RX 拼回去的单位。
struct C2cSeg {
  uint64_t vc = 0;
  uint64_t seq_id = 0;
  bool tail = false;
  uint64_t bytes = 0;
  MessagePtr msg;
};

// 跨 C2C 的一笔，可能是数据段也可能是三类 release 之一。它们共享同一个 AXI
// 传输包同步组包，接收侧按分段还原。
struct C2cBeat {
  bool is_release = false;
  // 链路层自己的 credit 归还：收方收下一段之后发回给发方，让它的发送额度恢复。
  // 与上面那三类 release 不同：那三类是 core 之间的事，跨片透传给对侧 core；
  // 这一类到对侧的桥为止，不往 core 送。
  bool is_c2c_credit = false;
  C2cSeg seg;
  ReleaseView rel;
  // 这一笔 VC release 代表几个 flit。业务层那两类恒为 1。
  uint64_t vc_len = 1;
};

// ── 一个方向的 credit ──
//
// 每 VC 一个 private 计数器，加每方向一个 shared 计数器。发送先扣 private 再扣
// shared，归还先补 private。一个方向的 credit 总量等于对侧该方向的 buffer 容量，
// 不超发。
class C2cCredit {
 public:
  C2cCredit(uint64_t per_vc, uint64_t shared_cap)
      : cap(per_vc), shared_cap_(shared_cap), shared_(shared_cap) {
    priv.fill(0);
    for (uint64_t v = 0; v < kVcNum; ++v) priv[v] = per_vc;
  }

  bool Has(uint64_t vc) const { return priv[vc] > 0 || shared_ > 0; }
  void Take(uint64_t vc) {
    if (priv[vc] > 0) {
      --priv[vc];
      return;
    }
    LOGCHECK(shared_ > 0, "C2cCredit: 超发了。发送前应当先看有没有 credit。");
    --shared_;
  }
  void Give(uint64_t vc) {
    if (priv[vc] < cap) {
      ++priv[vc];
      return;
    }
    if (shared_ < shared_cap_) ++shared_;
  }
  uint64_t Private(uint64_t vc) const { return priv[vc]; }
  uint64_t Shared() const { return shared_; }

 private:
  uint64_t cap, shared_cap_, shared_;
  std::array<uint64_t, kVcNum> priv{};
};

// ── M3 · RC / VA / SA ──
//
// 从 core 侧收 flit，查出口与下一跳 VC，查 credit，与同向的 release 仲裁，
// 交给 TX Engine。三关合起来 D3。
class C2cRcVaSa : public BachModule {
 public:
  C2cRcVaSa(ClockPtr clock, const std::string& name, uint64_t parent = 0,
            bool tick = true)
      : BachModule(clock, name, parent, tick),
        from_core(std::make_shared<LinkEnd>(clock)),
        to_core_back(std::make_shared<LinkEnd>(clock)),
        credit(kC2cTxPrivate, kC2cTxShared),
        granted(clock),
        blocked(clock) {}

  // core 侧的链路端：数据进来、三类 release 也从这里进来。
  LinkEndPtr FromCore() const { return from_core; }
  void AttachFromCore(LinkEndPtr p) { from_core = std::move(p); }
  // 回给本片 core 的 VC credit：收下一个 flit 就还一个。不还的话 core 那一侧
  // 的 credit 只减不加，一个大包发满一个方向的额度就再也发不动。
  LinkEndPtr ToCoreBack() const { return to_core_back; }
  void AttachToCoreBack(LinkEndPtr p) { to_core_back = std::move(p); }

  // 往线上发要看对侧还有没有位置。release 不占 credit：它是让对侧腾出位置的那
  // 一笔，被数据堵住就两边互等。
  bool HasBeat() const {
    if (out.empty()) return false;
    C2cBeat const& b = out.front();
    return b.is_release || credit.Has(b.seg.vc);
  }
  C2cBeat TakeBeat() {
    C2cBeat b = out.front();
    if (!b.is_release) {
      credit.Take(b.seg.vc);
      // 离开本级缓冲才把位置还给 core。收下就还的话，core 拿回额度又发，而本级
      // 还没腾空，那些 flit 只能挡回去，一个包就缺了一截。
      back_pend.push_back(b.seg.vc);
    }
    out.pop_front();
    return b;
  }
  // 对侧还回来的 credit。
  void GiveCredit(uint64_t vc) { credit.Give(vc); }
  // 本级收下一段之后要还给对侧的那一笔，排队发出去。
  void PushC2cCredit(uint64_t vc) { c2c_pend.push_back(vc); }
  C2cCredit const& Credit() const { return credit; }

  uint64_t Granted() const { return grant_cnt; }
  uint64_t Blocked() const { return block_cnt; }
  // 停钟之后取的那几个。
  uint64_t GrantCount() const { return grant_cnt; }
  uint64_t BlockCount() const { return block_cnt; }
  uint64_t PendingC2cCredit() const { return c2c_pend.size(); }
  bool Quiescent() const override { return pipe.empty() && out.empty(); }

 protected:
  void Step() override {
    uint64_t now = CycleNow();
    // 末级先做：先把到点的放出去，再收新的。
    while (!pipe.empty() && pipe.front().first <= now) {
      out.push_back(pipe.front().second);
      pipe.pop_front();
    }
    Take(now);

    // 本拍收下的那个 flit，把它占的 VC 位置还给 core。一拍最多收一个，也就
    // 一拍最多还一个。
    to_core_back->flit.Idle();
    if (back_pend.empty()) {
      to_core_back->release.Idle();
    } else {
      to_core_back->release.Drive(true, back_pend.front(), false, 0, false, 0);
      back_pend.pop_front();
    }

    granted = grant_cnt;
    blocked = block_cnt;
    TracePerCycle("inflight", pipe.size());
  }

 private:
  // 链路层的 credit 归还先发：它不占对侧 credit，也不能被数据堵住，否则两侧
  // 互等。
  bool SendC2cCredit(uint64_t now) {
    if (c2c_pend.empty()) return false;
    C2cBeat b;
    b.is_release = true;
    b.is_c2c_credit = true;
    b.rel.vc_id = c2c_pend.front();
    c2c_pend.pop_front();
    pipe.push_back({now + kC2cRcStages, b});
    ++grant_cnt;
    return true;
  }

  // 一拍里链路层 credit、core 那一侧的 release 与数据可以同时来，三样都当拍收
  // 下：release 是 core 那一侧的单拍脉冲，数据按 VC credit 发、没有反压，哪一样
  // 留到下一拍都会丢。同向的数据与 release 之间小包优先，按进 pipe 的先后排，
  // TX Engine 一拍取一笔。
  void Take(uint64_t now) {
    SendC2cCredit(now);
    ReleaseView r = ReadRelease(from_core->release);
    // VC credit 是 flit 粒度，先攒着；业务层那两类照原样走。
    if (r.vc_valid) {
      LOGCHECK(r.vc_id < kVcNum, "C2cRcVaSa: 归还的 VC 号越界。");
      ++vc_pend[r.vc_id];
    }
    if (r.stream_valid || r.reduce_valid) {
      C2cBeat b;
      b.is_release = true;
      b.rel = r;
      b.rel.vc_valid = false;   // VC 那一份走打包的通路
      pipe.push_back({now + kC2cRcStages, b});
      ++grant_cnt;
    }
    FlitView f = ReadFlit(from_core->flit);
    // 攒够一个包的量就发一笔。本拍既没有数据要发、也没有新的 credit 进来时，
    // 不足一个包的余量一并发掉。只看有没有数据的话，credit 一拍一个连着来
    // 时每一拍都算闲着，就永远攒不成一笔。
    SendPackedCredit(now, !f.valid && !r.vc_valid);
    if (!f.valid) return;
    // VA：本级还收不收得下。位置在 flit 离开本级缓冲时才还给 core，所以这里的
    // 额度与 core 那一侧的 VC credit 是同一个账，只数数据：release 与链路层
    // credit 不占那一份额度。对侧还有没有位置是往线上发那一步的事。
    if (DataHeld() >= kC2cInCap) {
      ++block_cnt;
      return;
    }
    C2cBeat b;
    b.seg.vc = f.vc;
    b.seg.bytes = f.bytes;
    b.seg.tail = f.tail;
    b.seg.msg = f.msg;
    pipe.push_back({now + kC2cRcStages, b});
    ++grant_cnt;
  }

  // 本级缓冲里压着几个数据段。
  uint64_t DataHeld() const {
    uint64_t n = 0;
    for (auto const& p : pipe) {
      if (!p.second.is_release) ++n;
    }
    for (auto const& b : out) {
      if (!b.is_release) ++n;
    }
    return n;
  }

  // 攒够 kC2cCreditPackFlits 就打成一笔，或者本拍闲着时把余量发掉。
  bool SendPackedCredit(uint64_t now, bool idle) {
    for (uint64_t v = 0; v < kVcNum; ++v) {
      if (vc_pend[v] == 0) continue;
      if (vc_pend[v] < kC2cCreditPackFlits && !idle) continue;
      uint64_t n = vc_pend[v] >= kC2cCreditPackFlits ? kC2cCreditPackFlits
                                                     : vc_pend[v];
      vc_pend[v] -= n;
      C2cBeat b;
      b.is_release = true;
      b.rel.vc_valid = true;
      b.rel.vc_id = v;
      b.vc_len = n;
      pipe.push_back({now + kC2cRcStages, b});
      ++grant_cnt;
      return true;
    }
    return false;
  }

  LinkEndPtr from_core;
  LinkEndPtr to_core_back;
  std::deque<uint64_t> back_pend;
  C2cCredit credit;
  std::array<uint64_t, kVcNum> vc_pend{};
  std::deque<uint64_t> c2c_pend;
  std::deque<std::pair<uint64_t, C2cBeat>> pipe;
  std::deque<C2cBeat> out;
  uint64_t grant_cnt = 0, block_cnt = 0;

  Logic64 granted, blocked;
};

// ── M4 · TX Engine ──
//
// 按 4 KB 边界拆分，加 seq_id 与 tail 标记，位宽 2048 转 1024（一拍拆两拍发）。
// AXI write 是 posted，写响应可以丢。
class C2cTxEngine : public BachModule {
 public:
  C2cTxEngine(ClockPtr clock, const std::string& name, uint64_t parent = 0,
              bool tick = true)
      : BachModule(clock, name, parent, tick), sent(clock), depth(clock) {}

  void Push(C2cBeat const& b) {
    if (b.is_release) {
      // 三类 release 一律透传，不拆包。
      out.push_back(b);
      return;
    }
    // 按 4 KB 边界切分，段号 4 位。位宽减半，所以一段再拆成两拍发出。
    uint64_t left = b.seg.bytes == 0 ? 1 : b.seg.bytes;
    uint64_t nseg = (left + kC2cSegBytes - 1) / kC2cSegBytes;
    for (uint64_t i = 0; i < nseg; ++i) {
      C2cBeat s = b;
      s.seg.seq_id = (next_seq + i) % kC2cSeqNum;
      s.seg.tail = (i + 1 == nseg) && b.seg.tail;
      uint64_t take = left > kC2cSegBytes ? kC2cSegBytes : left;
      s.seg.bytes = take;
      left -= take;
      // 2048 → 1024：一段占两拍。
      out.push_back(s);
      out.push_back(s);
    }
    next_seq = (next_seq + nseg) % kC2cSeqNum;
  }

  bool HasBeat() const { return !out.empty(); }
  C2cBeat TakeBeat() {
    C2cBeat b = out.front();
    out.pop_front();
    ++send_cnt;
    return b;
  }
  uint64_t Sent() const { return send_cnt; }
  bool Quiescent() const override { return out.empty(); }

 protected:
  void Step() override {
    sent = send_cnt;
    depth = out.size();
    TracePerCycle("depth", out.size());
  }

 private:
  std::deque<C2cBeat> out;
  uint64_t next_seq = 0, send_cnt = 0;

  Logic64 sent, depth;
};

// ── M6 · AXI Bridge ──
//
// 出方向把 credit 语义转成 AXI write burst，入方向转回 flit 与三类 release。
// 延迟由构造时给的参数定：出方向是这一段线的延迟，入方向是 0。TX 方向的 write
// 是 posted，RX 方向由本级回 dummy response 释放 PCIe 的 outstanding 资源。
class C2cAxiBridge : public BachModule {
 public:
  C2cAxiBridge(ClockPtr clock, const std::string& name, uint64_t latency,
               uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick), lat(latency), moved(clock) {}

  void Push(C2cBeat const& b) { pipe.push_back({CycleNow() + lat, b}); }
  bool HasBeat() const {
    return !pipe.empty() && pipe.front().first <= last_cycle;
  }
  C2cBeat TakeBeat() {
    C2cBeat b = pipe.front().second;
    pipe.pop_front();
    ++move_cnt;
    return b;
  }
  uint64_t Moved() const { return move_cnt; }
  bool Quiescent() const override { return pipe.empty(); }

 protected:
  void Step() override {
    last_cycle = CycleNow();
    moved = move_cnt;
    TracePerCycle("inflight", pipe.size());
  }

 private:
  uint64_t lat;
  std::deque<std::pair<uint64_t, C2cBeat>> pipe;
  uint64_t last_cycle = 0, move_cnt = 0;

  Logic64 moved;
};

// ── M5 · RX Engine ──
//
// 按 seq_id 缓存收到的段，tail 到齐后按 seq_id 顺序拼回原始包，位宽 1024 转
// 2048。拼好的包连同透传的 release 一起交给 core 侧的链路端。
class C2cRxEngine : public BachModule {
 public:
  C2cRxEngine(ClockPtr clock, const std::string& name, uint64_t parent = 0,
              bool tick = true)
      : BachModule(clock, name, parent, tick),
        to_core(std::make_shared<LinkEnd>(clock)),
        assembled(clock),
        depth(clock) {}

  LinkEndPtr ToCore() const { return to_core; }
  void AttachToCore(LinkEndPtr p) { to_core = std::move(p); }

  void Push(C2cBeat const& b) { in.push_back(b); }
  // 本级收下一段之后要还给对侧的那一笔。
  bool HasRelease() const { return !credit_back.empty(); }
  uint64_t TakeRelease() {
    uint64_t vc = credit_back.front();
    credit_back.pop_front();
    return vc;
  }
  // 对侧还回来的那一笔，加回本桥的发送额度。
  bool HasPeerCredit() const { return !peer_credit.empty(); }
  uint64_t TakePeerCredit() {
    uint64_t vc = peer_credit.front();
    peer_credit.pop_front();
    return vc;
  }

  uint64_t Assembled() const { return asm_cnt; }
  uint64_t AsmCount() const { return asm_cnt; }
  uint64_t PendingRelease() const { return credit_back.size(); }
  uint64_t PendingPeerCredit() const { return peer_credit.size(); }
  bool Quiescent() const override { return in.empty() && ready.empty(); }

 protected:
  void Step() override {
    // 末级先做：先把拼好的发出去，再收新的段。
    Emit();
    Reassemble();

    assembled = asm_cnt;
    depth = in.size();
    TracePerCycle("depth", in.size());
  }

 private:
  void Emit() {
    if (ready.empty()) {
      to_core->Idle();
      return;
    }
    C2cBeat b = ready.front();
    if (b.is_release && b.vc_len > 1) {
      // 收方按这一笔代表的 flit 数展开：下游的 VC credit 一拍只加一个。
      --ready.front().vc_len;
      to_core->flit.Idle();
      to_core->release.Drive(b.rel.vc_valid, b.rel.vc_id, false, 0, false, 0);
      return;
    }
    ready.pop_front();
    if (b.is_release) {
      to_core->flit.Idle();
      to_core->release.Drive(b.rel.vc_valid, b.rel.vc_id, b.rel.stream_valid,
                             b.rel.stream_user, b.rel.reduce_valid,
                             b.rel.reduce_user);
      return;
    }
    to_core->release.Idle();
    to_core->flit.Drive(b.seg.vc, /*is_head=*/true, b.seg.tail, b.seg.bytes,
                        b.seg.msg);
  }

  void Reassemble() {
    if (in.empty()) return;
    C2cBeat b = in.front();
    in.pop_front();
    if (b.is_c2c_credit) {
      // 链路层的 credit 到本桥为止，不往 core 送。
      peer_credit.push_back(b.rel.vc_id);
      return;
    }
    if (b.is_release) {
      // 三类 release 一律透传，不进重组缓冲。
      ready.push_back(b);
      return;
    }
    // 1024 → 2048：两拍合成一段，第二拍才算收齐。
    uint64_t key = b.seg.seq_id;
    if (half.count(key) == 0) {
      half[key] = b;
      return;
    }
    C2cBeat whole = half[key];
    half.erase(key);
    credit_back.push_back(whole.seg.vc);

    reasm[whole.seg.vc].push_back(whole);
    if (!whole.seg.tail) return;
    // tail 到齐：按 seq_id 顺序拼回原始包。段的字节数合起来就是原始包的长度。
    auto& segs = reasm[whole.seg.vc];
    C2cBeat out = segs.front();
    uint64_t total = 0;
    for (auto const& s : segs) total += s.seg.bytes;
    out.seg.bytes = total;
    out.seg.tail = true;
    segs.clear();
    ready.push_back(out);
    ++asm_cnt;
  }

  LinkEndPtr to_core;
  std::deque<C2cBeat> in, ready;
  std::map<uint64_t, C2cBeat> half;
  std::map<uint64_t, std::deque<C2cBeat>> reasm;
  std::deque<uint64_t> credit_back, peer_credit;
  uint64_t asm_cnt = 0;

  Logic64 assembled, depth;
};

// ── 一座桥的装配 ──
//
// core 侧 ↔ RC/VA/SA → TX Engine → AXI Bridge → 对外；对外 → AXI Bridge →
// RX Engine → core 侧。两个方向各一套，互不共享。
struct C2cCfg {
  // 对着 PCIe Switch 或 CPU 的那一侧没有对端的 PCIe Bridge：业务层逻辑 bypass
  // 掉，只保留位宽转换与拆包合包。
  bool bypass_business = false;
  // 这一段物理链路一个方向的延迟。到达拍算在发送侧，与链路模型同一条规矩：
  // 一段线的占用只有一个 owner，对接的两座桥因此不会把同一段各计一次，计
  // 两次的话 Router 到 Router 就是 600T，超过规格书给的 400T。
  uint64_t axi_latency = kC2cAxiLatency;
};

class C2cBridge {
 public:
  C2cBridge(ClockPtr clock, const std::string& name, C2cCfg const& setting,
            uint64_t parent = 0)
      : clk(clock), cfg(setting) {
    const uint64_t gid = TraceGroup(name, parent);
    rcvasa = std::make_unique<C2cRcVaSa>(clock, "rcvasa", gid, false);
    tx = std::make_unique<C2cTxEngine>(clock, "tx", gid, false);
    out_axi = std::make_unique<C2cAxiBridge>(clock, "axi_out",
                                             setting.axi_latency, gid, false);
    // 入方向只做协议转换，线上的时间已经由发送侧那一座桥计过了。
    in_axi = std::make_unique<C2cAxiBridge>(clock, "axi_in",
                                            /*latency=*/0, gid, false);
    rx = std::make_unique<C2cRxEngine>(clock, "rx", gid, false);
  }

  // ── 对内：接边界 core 的 Router ──
  LinkEndPtr FromCore() const { return rcvasa->FromCore(); }
  void AttachFromCore(LinkEndPtr p) { rcvasa->AttachFromCore(std::move(p)); }
  LinkEndPtr ToCore() const { return rx->ToCore(); }
  void AttachToCore(LinkEndPtr p) { rx->AttachToCore(std::move(p)); }

  // ── 对外：chip 的一个 C2C 口 ──
  //
  // 出方向由对端调 TakeOut()，入方向由对端调 PushIn()。两座桥对接就是这两个
  // 方法互相喂：C2C 上传的是段与 release，不是 flit。
  void AttachToCoreBack(LinkEndPtr p) { rcvasa->AttachToCoreBack(std::move(p)); }

  bool HasOut() const { return out_axi->HasBeat(); }
  C2cBeat TakeOut() { return out_axi->TakeBeat(); }
  void PushIn(C2cBeat const& b) { in_axi->Push(b); }

  // ── 观测 ──
  C2cRcVaSa& RcVaSa() { return *rcvasa; }
  C2cRxEngine& RxEngine() { return *rx; }
  C2cAxiBridge& AxiOut() { return *out_axi; }
  C2cTxEngine& Tx() { return *tx; }
  C2cRxEngine& Rx() { return *rx; }

  // 末级先做：一笔在一拍里最多前进一级。
  void RunStep() {
    rx->RunStep();
    in_axi->RunStep();
    out_axi->RunStep();
    tx->RunStep();
    rcvasa->RunStep();

    // 各级之间的搬运。走方法调用而不是端口：这几级都在同一个协程里，一拍里
    // 的次序由上面这个调用顺序定死。
    while (rcvasa->HasBeat()) tx->Push(rcvasa->TakeBeat());
    while (tx->HasBeat()) out_axi->Push(tx->TakeBeat());
    while (in_axi->HasBeat()) rx->Push(in_axi->TakeBeat());
    // RX 收下一段，就排一笔链路层 credit 发回对侧；对侧还回来的那些，加回
    // 本桥的发送额度。
    while (rx->HasRelease()) rcvasa->PushC2cCredit(rx->TakeRelease());
    while (rx->HasPeerCredit()) rcvasa->GiveCredit(rx->TakePeerCredit());
  }

  bool Quiescent() const {
    return rcvasa->Quiescent() && tx->Quiescent() && out_axi->Quiescent() &&
           in_axi->Quiescent() && rx->Quiescent();
  }

 private:
  ClockPtr clk;
  C2cCfg cfg;
  std::unique_ptr<C2cRcVaSa> rcvasa;
  std::unique_ptr<C2cTxEngine> tx;
  std::unique_ptr<C2cAxiBridge> out_axi, in_axi;
  std::unique_ptr<C2cRxEngine> rx;
};

}  // namespace bach
}  // namespace latch

#endif
