#ifndef _LATCH_BACH_IP_CHIP_CORE_ROUTER_ROUTER_PORTS_
#define _LATCH_BACH_IP_CHIP_CORE_ROUTER_ROUTER_PORTS_

// Router 内部各模块之间的端口束。
//
// 资源的归属：VC credit 与出方向的 Stream Resource Table 都落在 Xbar，不落在
// RouterStation。文档把 VA 那一级画在 station 上，但这两样管的都是「下游方向」
// 的资源，四个 station 会同时想用；只有让唯一的发送决策点持有它们，才不会出现
// 两个 station 同一拍各自判断够、加起来超扣。station 侧做 M1 收 flit 进 VC
// Buffer 与 M2 查表建上下文，把队首请求交给 Xbar；M3 的资源检查、M4 的通路仲裁、
// M5 的交换都在 Xbar 一拍里做完。
//
// station 读 Xbar 上一拍发布的 credit 电平只是为了少提无望的请求，判不判都不影响
// 正确性：真正的扣减只发生在 Xbar 授予的那一刻。

#include <cstdint>

#include "base/logic.h"
#include "bach/common/flit.h"
#include "bach/ip/chip/core/router/router_table.h"

namespace latch {
namespace bach {

// RouterStation → Xbar：交出去的一笔 flit。
//
// 这一路不是 valid/grant 握手，是「下游有位置就发」：Xbar 每拍发布自己那个入口
// 缓冲还收不收得下（room），station 读上一拍的电平，收得下就把队首交出去并当场
// 出队。等授予的话一笔要占两拍（station 提请求、Xbar 下一拍授予、station 再
// 下一拍才看得到），单个方向的吞吐就只有每两拍一个 flit，而 VC Buffer 队首每拍
// 都能出一个。
//
// room 拉低到 station 停下来隔着一拍，所以 Xbar 那边的入口缓冲要比门限多留余量，
// 多出来的那一笔照样收得下。
class XbarReqPort : public Logic {
 public:
  Logic64 valid, out_mask, vc, head, tail, bytes;
  Logic64 path_id, user_id, enters_core, stall_way, need_stream;
  // 这个入口的队列里从队首起有没有一整个包。仲裁的第二档看它：手里攥着整包的
  // 入口先走，免得几个只来了半个包的入口互相插队，谁都拼不齐。
  Logic64 whole_packet;
  // 这一笔要在下游那个方向占多少 credit，单位 1 KB。取自表项那个方向的
  // nxt_credit_require：广播的量含提前预留的输出结果空间，不只是数据本身。
  Logic64 credit_require;
  LogicPtr<Message> msg;
  // Xbar 写、station 读：入口缓冲还收不收得下。
  Logic64 room;
  // 每交出一笔加一。Xbar 按它认这一笔见没见过：两侧各自打拍，station 一拍
  // 不写端口就会回落成上一拍的值，只看 valid 会把同一笔收两遍。
  Logic64 seq;

  explicit XbarReqPort(ClockPtr c)
      : valid(c), out_mask(c), vc(c), head(c), tail(c), bytes(c), path_id(c),
        user_id(c), enters_core(c), stall_way(c), need_stream(c),
        whole_packet(c), credit_require(c), msg(c), room(c), seq(c) {
    Fields(valid, out_mask, vc, head, tail, bytes, path_id, user_id,
           enters_core, stall_way, need_stream, whole_packet, credit_require,
           msg, room, seq);
  }

  void IdleReq() {
    valid = 0;
    out_mask = 0;
    vc = 0;
    head = 0;
    tail = 0;
    bytes = 0;
    path_id = 0;
    user_id = 0;
    enters_core = 0;
    stall_way = 0;
    need_stream = 0;
    whole_packet = 0;
    credit_require = 0;
    msg = MessagePtr();
  }

  void DriveRoom(bool ok) { room = ok ? 1 : 0; }
  bool Room() const { return room.Get() != 0; }
};

struct XbarReqView {
  bool valid = false;
  uint64_t out_mask = 0;
  uint64_t vc = 0;
  bool head = false, tail = false;
  uint64_t bytes = 0;
  uint64_t path_id = 0, user_id = 0;
  bool enters_core = false, stall_way = false, need_stream = false;
  bool whole_packet = false;
  uint64_t credit_require = 0;
  MessagePtr msg;
  uint64_t seq = 0;
};

inline XbarReqView ReadXbarReq(XbarReqPort const& p) {
  XbarReqView v;
  v.valid = p.valid.Get() != 0;
  if (!v.valid) return v;
  v.out_mask = p.out_mask.Get();
  v.vc = p.vc.Get();
  v.head = p.head.Get() != 0;
  v.tail = p.tail.Get() != 0;
  v.bytes = p.bytes.Get();
  v.path_id = p.path_id.Get();
  v.user_id = p.user_id.Get();
  v.enters_core = p.enters_core.Get() != 0;
  v.stall_way = p.stall_way.Get() != 0;
  v.need_stream = p.need_stream.Get() != 0;
  v.whole_packet = p.whole_packet.Get() != 0;
  v.credit_require = p.credit_require.Get();
  v.msg = p.msg.Get();
  v.seq = p.seq.Get();
  return v;
}

// Xbar 每拍发布的 credit 电平，给各 station 读。只是让它们少提无望的请求。
class CreditLevelPort : public Logic {
 public:
  // 三个 R2R 方向 × 4 个 VC 的可发标志，压成一个位图。
  Logic64 vc_ok;
  Logic64 stream_free;  // 每方向 stream 表还有没有空项，按方向压位

  explicit CreditLevelPort(ClockPtr c) : vc_ok(c), stream_free(c) {
    Fields(vc_ok, stream_free);
  }

  static uint64_t Bit(uint64_t dir, uint64_t vc) { return dir * kVcNum + vc; }

  void Drive(uint64_t ok, uint64_t free_mask) {
    vc_ok = ok;
    stream_free = free_mask;
  }
  bool VcOk(uint64_t dir, uint64_t vc) const {
    return ((vc_ok.Get() >> Bit(dir, vc)) & 1u) != 0;
  }
};

// 准入电平：接收方每拍发布自己收不收得下，发送方读上一拍的值。
// 电平类端口，持续有效，接收方任意拍读。
class ReadyLevelPort : public Logic {
 public:
  Logic64 ready;

  explicit ReadyLevelPort(ClockPtr c) : ready(c) { Fields(ready); }

  void Drive(bool ok) { ready = ok ? 1 : 0; }
  bool Ready() const { return ready.Get() != 0; }
};

// Xbar → CoreMemReissue：拿不到下游资源且 stall_way 选转存时，把这一笔交出去。
// station 那一侧照常收到 grant，VC 槽腾出来，包落进本地 Core Mem 等重发。
class OverflowPort : public Logic {
 public:
  Logic64 valid, vc, head, tail, bytes;
  LogicPtr<Message> msg;

  explicit OverflowPort(ClockPtr c)
      : valid(c), vc(c), head(c), tail(c), bytes(c), msg(c) {
    Fields(valid, vc, head, tail, bytes, msg);
  }

  void Drive(FlitView const& f) {
    valid = 1;
    vc = f.vc;
    head = f.head ? 1 : 0;
    tail = f.tail ? 1 : 0;
    bytes = f.bytes;
    msg = f.msg;
  }
  void Idle() {
    valid = 0;
    vc = 0;
    head = 0;
    tail = 0;
    bytes = 0;
    msg = MessagePtr();
  }
  bool Valid() const { return valid.Get() != 0; }

  FlitView View() const {
    FlitView v;
    v.valid = valid.Get() != 0;
    if (!v.valid) return v;
    v.vc = vc.Get();
    v.head = head.Get() != 0;
    v.tail = tail.Get() != 0;
    v.bytes = bytes.Get();
    v.msg = msg.Get();
    return v;
  }
};

// CoreStation ↔ DTE 的进出核数据通路，AXI-Stream-Like：valid/ready 加 last 与
// 有效字节数。反压时数据保持，位置不前移，恢复后从同一 flit 继续。
class CoreDataPort : public Logic {
 public:
  Logic64 tvalid, tbytes, tlast, thdr, vc;
  LogicPtr<Message> tdata;
  Logic64 tready;
  // 一次握手最少两拍：发送方拉 valid，接收方下一拍拉 ready，发送方再下一拍才
  // 看得到并换下一笔。这中间发送方保持数据不变（或干脆不写、让 Latch 回落），
  // 接收方会连着两拍看到同一笔，按序号认它，不消费两遍。
  Logic64 seq;

  explicit CoreDataPort(ClockPtr c)
      : tvalid(c), tbytes(c), tlast(c), thdr(c), vc(c), tdata(c), tready(c),
        seq(c) {
    Fields(tvalid, tbytes, tlast, thdr, vc, tdata, tready, seq);
  }

  void Drive(uint64_t bytes, bool last, bool hdr, uint64_t vc_id,
             MessagePtr const& m) {
    tvalid = 1;
    tbytes = bytes;
    tlast = last ? 1 : 0;
    thdr = hdr ? 1 : 0;
    vc = vc_id;
    tdata = m;
    seq = ++issue_seq;
  }
  void Idle() {
    tvalid = 0;
    tbytes = 0;
    tlast = 0;
    thdr = 0;
    vc = 0;
    tdata = MessagePtr();
  }
  void DriveReady(bool ok) { tready = ok ? 1 : 0; }
  bool Ready() const { return tready.Get() != 0; }
  bool Valid() const { return tvalid.Get() != 0; }
  uint64_t Seq() const { return seq.Get(); }

 private:
  uint64_t issue_seq = 0;
};

struct CoreDataView {
  bool valid = false;
  uint64_t bytes = 0;
  bool last = false, hdr = false;
  uint64_t vc = 0;
  uint64_t seq = 0;
  MessagePtr msg;
};

inline CoreDataView ReadCoreData(CoreDataPort const& p) {
  CoreDataView v;
  v.valid = p.tvalid.Get() != 0;
  if (!v.valid) return v;
  v.bytes = p.tbytes.Get();
  v.last = p.tlast.Get() != 0;
  v.hdr = p.thdr.Get() != 0;
  v.vc = p.vc.Get();
  v.seq = p.Seq();
  v.msg = p.tdata.Get();
  return v;
}

// CoreStation → TS 的 trigger：Header 就绪即通知，不等整包收完。
// 请求发出后保持到 TS 拉 ready；丢一笔 trigger 就等于丢一个 token。
class TriggerPort : public Logic {
 public:
  Logic64 valid, user_id, path_id, reissue;
  Logic64 ready;
  // 一笔请求保持到看见 ready，TS 那一侧会连着几拍看到同一笔，按序号认它。
  Logic64 seq;

  explicit TriggerPort(ClockPtr c)
      : valid(c), user_id(c), path_id(c), reissue(c), ready(c), seq(c) {
    Fields(valid, user_id, path_id, reissue, ready, seq);
  }

  void Drive(uint64_t user, uint64_t path, bool re) {
    valid = 1;
    user_id = user;
    path_id = path;
    reissue = re ? 1 : 0;
    seq = ++issue_seq;
  }
  void Idle() {
    valid = 0;
    user_id = 0;
    path_id = 0;
    reissue = 0;
  }
  void DriveReady(bool ok) { ready = ok ? 1 : 0; }
  bool Ready() const { return ready.Get() != 0; }
  bool Valid() const { return valid.Get() != 0; }
  uint64_t Seq() const { return seq.Get(); }

 private:
  uint64_t issue_seq = 0;
};

// Retire 认定一个 user 退休后，广播给要抹表的那几个模块。
//
// 早先是回调：Retire 的协程直接去改 Xbar 与 Core Station 的 Stream 表、给
// Reduce Module 的上下文打标记。那是一个模块的协程改另一个模块的容器，各自
// 占一个协程时就是几个线程改同一张表。跨模块的动作一律走端口，各自的协程在
// 自己的 Step 里改自己的表。
//
// 电平口：valid 拉起一拍，接收方按序号认。
class RetireBroadcastPort : public Logic {
 public:
  Logic64 valid, user_id, seq;

  explicit RetireBroadcastPort(ClockPtr c) : valid(c), user_id(c), seq(c) {
    Fields(valid, user_id, seq);
  }

  void Drive(uint64_t user, uint64_t n) {
    valid = 1;
    user_id = user;
    seq = n;
  }
  void Idle() {
    valid = 0;
    user_id = 0;
    seq = seq.Get();
  }
  bool Valid() const { return valid.Get() != 0; }
  uint64_t User() const { return user_id.Get(); }
  uint64_t Seq() const { return seq.Get(); }
};

}  // namespace bach
}  // namespace latch

#endif
