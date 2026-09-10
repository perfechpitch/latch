#ifndef _LATCH_BACH_IP_CHIP_CORE_RV_CORE_DSA_RQ_
#define _LATCH_BACH_IP_CHIP_CORE_RV_CORE_DSA_RQ_

// M4 · dsa_rq 写回。
//
// 8 项，按顺序记录已下发的读指令的目的寄存器编号；DSA 把数据送回来时按记录的
// 顺序写回 gpr 并把就绪位立起来。拍数由对应的 DSA 决定，本级不设上限。
//
// 记的只有寄存器编号：数据由 DSA 那一侧带回来。读寄存器不支持同步返回，软件
// 要查询状态只能轮询。

#include <deque>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/ip/chip/core/rv_core/rv_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class DsaRq : public BachModule {
 public:
  DsaRq(ClockPtr clock, const std::string& name, uint64_t parent = 0,
        bool tick = true)
      : BachModule(clock, name, parent, tick),
        issue(std::make_shared<GprWbPort>(clock)),
        rdata(std::make_shared<DsaRdataPort>(clock)),
        wb(std::make_shared<GprWbPort>(clock)),
        depth(clock),
        returned(clock) {}

  // dsa_iss 下发读指令时从这个口把目的寄存器编号送进来。
  std::shared_ptr<GprWbPort> IssuePtr() const { return issue; }
  void AttachIssue(std::shared_ptr<GprWbPort> p) { issue = std::move(p); }
  DsaRdataPort& Rdata() { return *rdata; }
  void AttachRdata(std::shared_ptr<DsaRdataPort> p) { rdata = std::move(p); }
  // 写回 gpr 并置就绪。
  std::shared_ptr<GprWbPort> WbPtr() const { return wb; }

  uint64_t Depth() const { return q.size(); }
  uint64_t Returned() const { return return_cnt; }
  bool Quiescent() const override { return q.empty(); }

 protected:
  void Step() override {
    // 末级先做：先把回来的写回去，腾出位置，再记新下发的。
    Writeback();
    TakeIssue();

    depth = q.size();
    returned = return_cnt;
    TracePerCycle("depth", q.size());
  }

 private:
  void Writeback() {
    if (!rdata->Valid() || rdata->Seq() == last_rdata_seq || q.empty()) {
      wb->Idle();
      return;
    }
    last_rdata_seq = rdata->Seq();
    uint64_t rd = q.front();
    q.pop_front();
    wb->DriveData(rd, rdata->rdata.Get(), ++wb_seq);
    ++return_cnt;
  }

  void TakeIssue() {
    if (!issue->Valid() || issue->Seq() == last_issue_seq) return;
    last_issue_seq = issue->Seq();
    LOGCHECK(q.size() < kDsaRqDepth,
             "DsaRq: 满了。dsa_iss 应当先看还有没有位置。");
    q.push_back(issue->RdIdx());
  }

  std::shared_ptr<GprWbPort> issue;
  std::shared_ptr<DsaRdataPort> rdata;
  std::shared_ptr<GprWbPort> wb;

  std::deque<uint64_t> q;
  uint64_t last_issue_seq = 0, last_rdata_seq = 0, wb_seq = 0, return_cnt = 0;

  Logic64 depth, returned;
};

}  // namespace bach
}  // namespace latch

#endif
