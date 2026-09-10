#ifndef _LATCH_BACH_IP_CHIP_CORE_RV_CORE_DSA_ISS_
#define _LATCH_BACH_IP_CHIP_CORE_RV_CORE_DSA_ISS_

// M3 · dsa_iss 下发。
//
// 每拍最多下发一条配置或 trigger 指令。写 DSA 寄存器一条写一个 —— 指令表里
// 只有 dsaw / dsawi，与「每条最多配置 1 个 DSA 寄存器」一致。
//
// 写会被反压：DSA 的配置通路满时 req_ready 拉低，这一条阻塞在这里，执行器那边
// pc 保持。读不会被阻塞，而且不支持同步返回 —— 发出去就走，返回数据由 dsa_rq
// 按记录的顺序写回 gpr，软件要查状态只能轮询。

#include <deque>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/ip/chip/core/rv_core/rv_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class DsaIss : public BachModule {
 public:
  DsaIss(ClockPtr clock, const std::string& name, uint64_t parent = 0,
         bool tick = true)
      : BachModule(clock, name, parent, tick),
        req(std::make_shared<DsaReqPort>(clock)),
        cfg(std::make_shared<DsaCfgPort>(clock)),
        rq(std::make_shared<GprWbPort>(clock)),
        issued(clock),
        stalls(clock) {}

  std::shared_ptr<DsaReqPort> ReqPtr() const { return req; }
  DsaCfgPort& Cfg() { return *cfg; }
  void AttachCfg(std::shared_ptr<DsaCfgPort> p) { cfg = std::move(p); }
  // 读指令下发的同时把目的寄存器编号交给 dsa_rq 记着。
  std::shared_ptr<GprWbPort> ReadIssuePtr() const { return rq; }

  uint64_t Issued() const { return issue_cnt; }
  uint64_t Stalls() const { return stall_cnt; }
  bool Quiescent() const override { return !holding; }

 protected:
  void Step() override {
    // 末级先做：先看上一条被收走没有，再收新的。
    Drain();
    Accept();
    Publish();

    issued = issue_cnt;
    stalls = stall_cnt;
    TracePerCycle("holding", holding ? 1 : 0);
  }

 private:
  void Drain() {
    if (!holding || !driven) return;
    // 写要等 DSA 那边给 ready；读不会被阻塞，发一拍就算下发。
    if (held_we && !cfg->Ready()) {
      ++stall_cnt;
      return;
    }
    holding = false;
    driven = false;
    ++issue_cnt;
  }

  void Accept() {
    req->DriveReady(!holding);
    if (holding || !req->Valid() || req->Seq() == last_seq) {
      rq->Idle();
      return;
    }
    last_seq = req->Seq();
    held_we = req->We();
    held_addr = req->addr.Get();
    held_data = req->wdata.Get();
    holding = true;
    // 序号在收下这一笔时定死。等 ready 的那几拍要一直发同一个号 —— 每拍换号
    // 的话，接收方按序号去重就把同一笔认成好几笔，写 trigger 那种「写一次执行
    // 一次」的寄存器会被执行好几遍。
    ++cfg_seq;
    // 读指令：把目的寄存器编号交给 dsa_rq 记着，返回时按序写回。
    if (!held_we) {
      rq->Drive(req->rd_idx.Get(), last_seq);
    } else {
      rq->Idle();
    }
  }

  void Publish() {
    if (!holding) {
      cfg->Idle();
      return;
    }
    // 写与读各有各的驱动：一拍里对同一个 Latch 写两次是静默覆盖，不能先 Drive
    // 再把 req_we 抹掉。
    if (held_we) {
      cfg->Drive(held_addr, held_data, cfg_seq);
    } else {
      cfg->DriveRead(held_addr, cfg_seq);
    }
    driven = true;
  }

  std::shared_ptr<DsaReqPort> req;
  std::shared_ptr<DsaCfgPort> cfg;
  std::shared_ptr<GprWbPort> rq;

  bool holding = false, driven = false, held_we = false;
  uint64_t held_addr = 0, held_data = 0;
  uint64_t last_seq = 0, cfg_seq = 0, issue_cnt = 0, stall_cnt = 0;

  Logic64 issued, stalls;
};

}  // namespace bach
}  // namespace latch

#endif
