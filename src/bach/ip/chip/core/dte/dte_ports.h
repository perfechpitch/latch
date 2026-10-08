#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_DTE_PORTS_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_DTE_PORTS_

// DTE 内部各模块之间的端口。对外那几组复用已有的：进出核走 CoreDataPort，
// 访存走 MemPort，向 TS 报完成走 DonePort。

#include <memory>

#include "base/logic.h"
#include "bach/ip/chip/core/dte/dte_types.h"
#include "bach/ip/chip/core/rv_core/rv_ports.h"

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

// DTE RV core 的 dsa_iss 配置口。
// 带序号：一次握手最少两拍，请求方在等 ready 那一拍不改端口，接收方读到的
// valid 与数据都还是上一拍那一份。同一个地址连着写两个相同的值是合法的（写
// 启动寄存器就是这样，写一次执行一次），所以不能按 (addr, data) 认重复，只能
// 按序号认。请求方每换一笔就把序号加一。
class DsaCfgPort : public Logic {
 public:
  Logic64 req_valid, req_we, req_addr, req_wdata, req_ready, req_seq;
  Logic64 req_stream, req_task, req_user, req_path, req_vc;

  explicit DsaCfgPort(ClockPtr c)
      : req_valid(c), req_we(c), req_addr(c), req_wdata(c), req_ready(c),
        req_seq(c), req_stream(c), req_task(c), req_user(c), req_path(c),
        req_vc(c) {
    Fields(req_valid, req_we, req_addr, req_wdata, req_ready, req_seq,
           req_stream, req_task, req_user, req_path, req_vc);
  }

  void Drive(uint64_t addr, uint64_t data, uint64_t seq, DsaTaskIds ids = {}) {
    req_valid = 1;
    req_we = 1;
    req_addr = addr;
    req_wdata = data;
    req_seq = seq;
    req_stream = ids.stream;
    req_task = ids.task;
    req_user = ids.user;
    req_path = ids.path;
    req_vc = ids.vc;
  }
  DsaTaskIds TaskIds() const {
    return {req_stream.Get(), req_task.Get(), req_user.Get(), req_path.Get(),
            req_vc.Get()};
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
    req_stream = 0;
    req_task = 0;
    req_user = 0;
    req_path = 0;
    req_vc = 0;
  }
  void DriveReady(bool ok) { req_ready = ok ? 1 : 0; }
  bool Valid() const { return req_valid.Get() != 0; }
  bool Ready() const { return req_ready.Get() != 0; }
  uint64_t Seq() const { return req_seq.Get(); }
};

}  // namespace bach
}  // namespace latch

#endif
