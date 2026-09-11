#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_REGFILE_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_REGFILE_

// DTE 的软件配置寄存器组：RV core 看得见的那一层。
//
// 《DTE寄存器配置参数》的地址空间七段。本模块只实现 task 模板那一段与 issue
// 段的 trigger。其余几段是状态、性能计数与 debug dump，不参与起任务。
//
// 起任务的过程（F14）：RV core 一条指令写一个寄存器，最后写 transfer_mode/
// task_trigger。写 trigger 那一拍把当前模板的十一项与四个直连身份信号一起采
// 下来，拼成一个 Descriptor 交给 Commit。
//
// 四个身份不由软件写：streamID、taskID、userID、pathID 从 RV core 的 CSR 直连
// 过来，trigger 写时采样。
//
// 反压：Commit 的 PendingTaskQ 满时它不收这一笔 Descriptor，本模块保持着重发，
// 同时拉低 dsa_cfg 的 req_ready 反压 RV core（F55）。
//
// 读寄存器隔几拍才回：读不支持同步返回，返回数据走独立的 dsa_rdata 口。同拍
// 回的话 dsa_rq 那边还没把这一笔的目的寄存器记进队列，脉冲就丢了。

#include <array>
#include <deque>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/ip/chip/core/rv_core/rv_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 地址空间七段的基址，照《DTE寄存器配置参数》§寄存器地址空间分配。
constexpr uint64_t kDteCtrlBase = 0x000;
constexpr uint64_t kDteIssueBase = 0x100;
constexpr uint64_t kDteTemplateBase = 0x200;
constexpr uint64_t kDteTaskqBase = 0x300;
constexpr uint64_t kDteLutBase = 0x600;
constexpr uint64_t kDteHmemBase = 0x800;
// 3 套有效模板加 1 套 header-only，每套 64 B 对齐。
constexpr uint64_t kDteTemplateStride = 0x40;
constexpr uint64_t kDteTemplateNum = 4;

// 一套模板内十一项的偏移，照 §DTE task寄存器配置信息。
constexpr uint64_t kDteRegSrcAddr = 0x00;
constexpr uint64_t kDteRegDstAddr = 0x04;
constexpr uint64_t kDteRegStreamStride = 0x08;
constexpr uint64_t kDteRegScaleAddr = 0x0C;
constexpr uint64_t kDteRegTopkAddr = 0x10;
constexpr uint64_t kDteRegHwHeaderAddr = 0x14;
constexpr uint64_t kDteRegSwHeaderAddr = 0x18;
constexpr uint64_t kDteRegSharememWaddr = 0x1C;
constexpr uint64_t kDteRegSharememWdata = 0x20;
constexpr uint64_t kDteRegDataLen = 0x24;
constexpr uint64_t kDteRegTrigger = 0x28;

// transfer_mode/task_trigger 的字段，照 §`transfer_mode/task_trigger` 字段建议。
constexpr uint64_t kDteModeMask = 0x7;          // transfer_mode 3 bit
constexpr uint64_t kDteEpCountShift = 3;        // router_ep_count 8 bit
constexpr uint64_t kDteEpCountMask = 0xFF;
constexpr uint64_t kDteScaleValid = 1ull << 11;
constexpr uint64_t kDteTopkValid = 1ull << 12;
constexpr uint64_t kDteHwHeaderOp = 1ull << 13;
constexpr uint64_t kDteWrSharememFlag = 1ull << 14;
constexpr uint64_t kDteTaskLast = 1ull << 15;

// 一套模板。
struct DteTemplate {
  uint64_t src_addr = 0;
  uint64_t dst_addr = 0;
  uint64_t stream_stride = 0;
  uint64_t scale_addr = 0;
  uint64_t topk_addr = 0;
  uint64_t hw_header_addr = 0;
  uint64_t sw_header_addr = 0;
  uint64_t sharemem_waddr = 0;
  uint64_t sharemem_wdata = 0;
  // CFG_DATA_LEN 的原值，单位是 8 B 一格，不是字节。
  uint64_t data_len = 0;
  uint64_t trigger = 0;
};

class DteRegfile : public BachModule {
 public:
  DteRegfile(ClockPtr clock, const std::string& name, uint64_t parent = 0,
             bool tick = true)
      : BachModule(clock, name, parent, tick),
        cfg(std::make_shared<DsaCfgPort>(clock)),
        ids(std::make_shared<DsaIdsPort>(clock)),
        out(std::make_shared<DescPort>(clock)),
        rdata(std::make_shared<DsaRdataPort>(clock)),
        triggers(clock),
        writes(clock) {}

  // ── 对外 ──
  DsaCfgPort& Cfg() { return *cfg; }
  std::shared_ptr<DsaCfgPort> CfgPtr() const { return cfg; }
  DsaIdsPort& Ids() { return *ids; }
  void AttachIds(std::shared_ptr<DsaIdsPort> p) { ids = std::move(p); }
  std::shared_ptr<DsaRdataPort> RdataPtr() const { return rdata; }
  // 起任务这一笔交给 Commit 的 from_rv 口：一根线两端是同一个对象。
  std::shared_ptr<DescPort> OutPtr() const { return out; }
  void AttachOut(std::shared_ptr<DescPort> p) { out = std::move(p); }

  // ── 观测 ──
  DteTemplate const& Template(uint64_t i) const { return tpl.at(i); }
  uint64_t Triggers() const { return trig_cnt; }
  uint64_t Writes() const { return write_cnt; }
  uint64_t Reads() const { return read_cnt; }
  bool Quiescent() const override { return !holding && pending_read.empty(); }

 protected:
  void Step() override {
    rdata_used = false;
    // 末级先做：先看上一笔 Descriptor 被 Commit 收下没有，再收新的寄存器写。
    Drain();
    ReturnRead();
    TakeCfg();
    Publish();
    if (!rdata_used) rdata->Idle();

    // PendingTaskQ 满时本模块手上压着一笔发不出去，此时不再收新的配置写。
    cfg->DriveReady(!holding);
    triggers = trig_cnt;
    writes = write_cnt;
    TracePerCycle("holding", holding ? 1 : 0);
  }

 private:
  void Drain() {
    if (!holding) return;
    if (!out->Accepted()) return;
    holding = false;
    held = std::shared_ptr<Descriptor>();
  }

  void TakeCfg() {
    if (holding) return;
    if (!cfg->Valid() || cfg->Seq() == last_seq) return;
    last_seq = cfg->Seq();
    uint64_t at = cfg->req_addr.Get();
    if (cfg->req_we.Get() == 0) {
      pending_read.push_back({CycleNow() + kDsaReadLatency, ReadReg(at),
                              last_seq});
      ++read_cnt;
      return;
    }
    ++write_cnt;
    WriteReg(at, cfg->req_wdata.Get());
  }

  void ReturnRead() {
    if (pending_read.empty()) return;
    ReadBack const& r = pending_read.front();
    if (r.at > CycleNow()) return;
    rdata->Drive(r.value, r.seq);
    rdata_used = true;
    pending_read.pop_front();
  }

  void Publish() {
    if (holding) {
      out->Drive(held, held_seq);
      return;
    }
    out->Idle();
  }

  // 模板段以外的几段本模型不落地：它们是状态、性能计数与 debug dump，不参与
  // 起任务。
  void WriteReg(uint64_t at, uint64_t data) {
    if (at < kDteTemplateBase || at >= kDteTaskqBase) return;
    uint64_t idx = (at - kDteTemplateBase) / kDteTemplateStride;
    if (idx >= kDteTemplateNum) return;
    uint64_t off = (at - kDteTemplateBase) % kDteTemplateStride;
    DteTemplate& t = tpl[idx];
    switch (off) {
      case kDteRegSrcAddr: t.src_addr = data; return;
      case kDteRegDstAddr: t.dst_addr = data; return;
      case kDteRegStreamStride: t.stream_stride = data; return;
      case kDteRegScaleAddr: t.scale_addr = data; return;
      case kDteRegTopkAddr: t.topk_addr = data; return;
      case kDteRegHwHeaderAddr: t.hw_header_addr = data; return;
      case kDteRegSwHeaderAddr: t.sw_header_addr = data; return;
      case kDteRegSharememWaddr: t.sharemem_waddr = data; return;
      case kDteRegSharememWdata: t.sharemem_wdata = data; return;
      case kDteRegDataLen:
        LOGCHECK(data <= kDteDataLenMax, "DteRegfile: CFG_DATA_LEN 只有 16 bit。");
        t.data_len = data;
        return;
      case kDteRegTrigger:
        t.trigger = data;
        Fire(idx);
        return;
      default:
        return;
    }
  }

  uint64_t ReadReg(uint64_t at) const {
    if (at < kDteTemplateBase || at >= kDteTaskqBase) return 0;
    uint64_t idx = (at - kDteTemplateBase) / kDteTemplateStride;
    if (idx >= kDteTemplateNum) return 0;
    uint64_t off = (at - kDteTemplateBase) % kDteTemplateStride;
    DteTemplate const& t = tpl[idx];
    switch (off) {
      case kDteRegSrcAddr: return t.src_addr;
      case kDteRegDstAddr: return t.dst_addr;
      case kDteRegStreamStride: return t.stream_stride;
      case kDteRegScaleAddr: return t.scale_addr;
      case kDteRegTopkAddr: return t.topk_addr;
      case kDteRegHwHeaderAddr: return t.hw_header_addr;
      case kDteRegSwHeaderAddr: return t.sw_header_addr;
      case kDteRegSharememWaddr: return t.sharemem_waddr;
      case kDteRegSharememWdata: return t.sharemem_wdata;
      case kDteRegDataLen: return t.data_len;
      case kDteRegTrigger: return t.trigger;
      default: return 0;
    }
  }

  // 写 trigger 那一拍拼出一个 Descriptor。
  void Fire(uint64_t idx) {
    DteTemplate const& t = tpl[idx];
    auto d = std::make_shared<Descriptor>();
    d->valid = true;
    // 身份直连采样。出核走哪个 VC 取 TS 随任务下发的 VCID。
    d->stream_id = ids->Stream();
    d->task_id = ids->Task();
    d->user_id = ids->User();
    d->path_id = ids->Path();
    d->vc = ids->Vc();
    d->route = Route(t.trigger & kDteModeMask);
    // 地址公式 base + streamID * stride + offset。router 作为源或目的时那一端
    // 的地址无意义，硬件也照算，取出来不用。
    uint64_t shift = d->stream_id * t.stream_stride;
    d->src_addr = t.src_addr + shift;
    d->dst_addr = t.dst_addr + shift;
    d->bytes = t.data_len * kDteDataLenGrain;
    d->task_last = (t.trigger & kDteTaskLast) != 0;
    d->smem_wr = (t.trigger & kDteWrSharememFlag) != 0;
    d->smem_addr = t.sharemem_waddr;
    d->smem_data = t.sharemem_wdata;
    held = d;
    held_seq = out->NextSeq();
    holding = true;
    ++trig_cnt;
  }

  std::shared_ptr<DsaCfgPort> cfg;
  std::shared_ptr<DsaIdsPort> ids;
  std::shared_ptr<DescPort> out;
  std::shared_ptr<DsaRdataPort> rdata;

  struct ReadBack {
    uint64_t at = 0, value = 0, seq = 0;
  };
  std::deque<ReadBack> pending_read;

  std::array<DteTemplate, kDteTemplateNum> tpl{};
  std::shared_ptr<Descriptor> held;
  bool holding = false, rdata_used = false;
  uint64_t last_seq = 0, held_seq = 0, trig_cnt = 0, write_cnt = 0, read_cnt = 0;

  Logic64 triggers, writes;
};

}  // namespace bach
}  // namespace latch

#endif
