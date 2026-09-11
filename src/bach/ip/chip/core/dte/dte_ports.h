#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_DTE_PORTS_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_DTE_PORTS_

// DTE 内部各模块之间的端口。对外那几组复用已有的：进出核走 CoreDataPort，
// 访存走 MemPort，向 TS 报完成走 DonePort。

#include <memory>

#include "base/logic.h"
#include "bach/ip/chip/core/dte/dte_types.h"

namespace latch {
namespace bach {

// 一笔 Descriptor 的传递。用 LogicPtr 搬，不把字段摊平。
//
// 同一笔请求会连着两拍出现在端口上（请求方要等 accepted 打一拍才撤 valid），
// 所以带序号，接收方按序号认，不执行两遍。这一条在 Router 与 TS 上各踩过一次。
class DescPort : public Logic {
 public:
  Logic64 valid;
  LogicPtr<Descriptor> desc;
  Logic64 accepted;
  Logic64 seq;

  explicit DescPort(ClockPtr c) : valid(c), desc(c), accepted(c), seq(c) {
    Fields(valid, desc, accepted, seq);
  }

  void Drive(std::shared_ptr<Descriptor> d) {
    valid = 1;
    desc = std::move(d);
    seq = ++issue_seq;
  }
  // 保持重发用这个：等对方收下的那几拍要一直发同一个号，每拍换号的话接收方
  // 按序号去重就把同一笔认成好几笔。
  void Drive(std::shared_ptr<Descriptor> d, uint64_t n) {
    valid = 1;
    desc = std::move(d);
    seq = n;
  }
  uint64_t NextSeq() { return ++issue_seq; }
  void Idle() {
    valid = 0;
    desc = std::shared_ptr<Descriptor>();
  }
  void DriveAccepted(bool ok) { accepted = ok ? 1 : 0; }

  bool Valid() const { return valid.Get() != 0; }
  bool Accepted() const { return accepted.Get() != 0; }
  uint64_t Seq() const { return seq.Get(); }
  std::shared_ptr<Descriptor> Desc() const { return desc.Get(); }

 private:
  uint64_t issue_seq = 0;
};

// Header Parser → 进核通道的 RD 侧：Payload 那些拍。
//
// 不能让 Parser 直接往 Lane 的 buffer 里塞：那是两个模块的协程同时改同一个
// 容器，单线程下看着能跑，多线程下就是段错误。一切跨模块的搬运都走端口。
class PayloadPort : public Logic {
 public:
  // frame 是 Header Parser 给这一帧编的号，进核那一路按它认「这几拍属于哪一
  // 帧」。off 是这一拍的数据在整包 payload 里的起点：一包拆成几拍进来，写存储
  // 时要按这个起点切出本拍那一段。
  Logic64 valid, frame, bytes, off, last, ready, seq;
  LogicPtr<Message> msg;

  explicit PayloadPort(ClockPtr c)
      : valid(c), frame(c), bytes(c), off(c), last(c), ready(c), seq(c),
        msg(c) {
    Fields(valid, frame, bytes, off, last, ready, seq, msg);
  }

  void Drive(uint64_t frame_seq, uint64_t n, uint64_t at, bool is_last,
             MessagePtr const& m) {
    valid = 1;
    frame = frame_seq;
    bytes = n;
    off = at;
    last = is_last ? 1 : 0;
    msg = m;
    seq = ++issue_seq;
  }
  void Idle() {
    valid = 0;
    frame = 0;
    bytes = 0;
    off = 0;
    last = 0;
    msg = MessagePtr();
  }
  void DriveReady(bool ok) { ready = ok ? 1 : 0; }
  bool Valid() const { return valid.Get() != 0; }
  bool Ready() const { return ready.Get() != 0; }
  uint64_t Seq() const { return seq.Get(); }

 private:
  uint64_t issue_seq = 0;
};

// 一侧完成的上报：Lane → Completion RS。
class HalfDonePort : public Logic {
 public:
  Logic64 valid, commit_seq, half, drained;

  explicit HalfDonePort(ClockPtr c)
      : valid(c), commit_seq(c), half(c), drained(c) {
    Fields(valid, commit_seq, half, drained);
  }

  void Drive(uint64_t seq, uint64_t which, bool is_drained) {
    valid = 1;
    commit_seq = seq;
    half = which;
    drained = is_drained ? 1 : 0;
  }
  void Idle() {
    valid = 0;
    commit_seq = 0;
    half = 0;
    drained = 0;
  }
  bool Valid() const { return valid.Get() != 0; }
};

// DSA → RV core 的 dsa_rq：读寄存器的返回，异步、脉冲。
//
// 读不支持同步返回，所以下发与返回是两条线：dsa_iss 发出去时把目的寄存器编号
// 记进 dsa_rq，数据回来时按记录的顺序写回 gpr。
class DsaRdataPort : public Logic {
 public:
  Logic64 valid, rdata, seq;

  explicit DsaRdataPort(ClockPtr c) : valid(c), rdata(c), seq(c) {
    Fields(valid, rdata, seq);
  }

  void Drive(uint64_t v, uint64_t n) {
    valid = 1;
    rdata = v;
    seq = n;
  }
  void Idle() {
    valid = 0;
    rdata = 0;
    seq = seq.Get();
  }
  bool Valid() const { return valid.Get() != 0; }
  uint64_t Rdata() const { return rdata.Get(); }
  uint64_t Seq() const { return seq.Get(); }
};

// Commit 准入一笔任务后，把它分发给这一笔归属的 Lane 与 Completion RS。
//
// 早先 Commit 是直接调它们的方法往对方的队列里放的，那是一个模块的协程去改另
// 一个模块的容器：单线程下看着能跑，多线程下几百轮里会段错误一次。跨模块的
// 搬运一律走端口。
//
// ready 是「下一拍一定收得下」的承诺：接收方按当前空位算，而往它队列里放东西的
// 只有 Commit 一家，所以承诺在下一拍仍然成立。「三样一起拿」因此还是原样：
// 三个口的 ready 都为真才发，谁没准备好就一起等。
class AdmitPort : public Logic {
 public:
  Logic64 valid, ready, seq;
  LogicPtr<Descriptor> desc;

  explicit AdmitPort(ClockPtr c) : valid(c), ready(c), seq(c), desc(c) {
    Fields(valid, ready, seq, desc);
  }

  void Drive(std::shared_ptr<Descriptor> d, uint64_t n) {
    valid = 1;
    desc = std::move(d);
    seq = n;
  }
  void Idle() {
    valid = 0;
    desc = std::shared_ptr<Descriptor>();
    seq = seq.Get();
  }
  void DriveReady(bool ok) { ready = ok ? 1 : 0; }

  bool Valid() const { return valid.Get() != 0; }
  bool Ready() const { return ready.Get() != 0; }
  uint64_t Seq() const { return seq.Get(); }
  std::shared_ptr<Descriptor> Desc() const { return desc.Get(); }
};

// DTE RV core 的 dsa_iss 配置口。
// 带序号：一次握手最少两拍，请求方在等 ready 那一拍不改端口，接收方读到的
// valid 与数据都还是上一拍那一份。同一个地址连着写两个相同的值是合法的（写
// 启动寄存器就是这样，写一次执行一次），所以不能按 (addr, data) 认重复，只能
// 按序号认。请求方每换一笔就把序号加一。
class DsaCfgPort : public Logic {
 public:
  Logic64 req_valid, req_we, req_addr, req_wdata, req_ready, req_seq;

  explicit DsaCfgPort(ClockPtr c)
      : req_valid(c), req_we(c), req_addr(c), req_wdata(c), req_ready(c),
        req_seq(c) {
    Fields(req_valid, req_we, req_addr, req_wdata, req_ready, req_seq);
  }

  void Drive(uint64_t addr, uint64_t data, uint64_t seq) {
    req_valid = 1;
    req_we = 1;
    req_addr = addr;
    req_wdata = data;
    req_seq = seq;
  }
  // 读寄存器：不会被阻塞，返回数据走独立的 dsa_rdata 口异步回来。
  void DriveRead(uint64_t addr, uint64_t seq) {
    req_valid = 1;
    req_we = 0;
    req_addr = addr;
    req_wdata = 0;
    req_seq = seq;
  }
  void Idle() {
    req_valid = 0;
    req_we = 0;
    req_addr = 0;
    req_wdata = 0;
    req_seq = req_seq.Get();
  }
  void DriveReady(bool ok) { req_ready = ok ? 1 : 0; }
  bool Valid() const { return req_valid.Get() != 0; }
  bool Ready() const { return req_ready.Get() != 0; }
  uint64_t Seq() const { return req_seq.Get(); }
};

}  // namespace bach
}  // namespace latch

#endif
