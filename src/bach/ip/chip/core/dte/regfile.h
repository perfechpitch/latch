#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_REGFILE_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_REGFILE_

// DTE 的软件配置寄存器组：RV core 看得见的那一层。
//
// 对齐飞书《DTE DSA》文档的 16KB 地址空间：
//   0x0000~0x03FF  Config：CFG_TRIGGER + 19 项任务配置寄存器
//   0x0400~0x07FF  Ctrl/Status（本模型不落地）
//   0x0800~0x0BFF  Profile（不落地）
//   0x0C00~0x0FFF  Debug（不落地）
//   0x1000~0x1FFF  Template：8 个 × 128B，每个 19 项（0x00~0x48，无 TRIGGER）
//   0x2000~0x2FFF  Glb Cfg TaskQ（16 项 × 256B，只读回读）
//   0x3000~0x3FFF  Header Table（16 项 × 128B，本模型走 Hmem，此处只读回读）
//
// 起任务的过程：RV core 一条指令写一个寄存器，最后写 CFG_TRIGGER（0x0000）。写
// trigger 那一拍采样 STUPV 身份（streamID/taskID/userID/pathID/vcid 从 RV core 的
// CSR 直连过来），合并 19 项配置组装成一个 4 段位 Descriptor 交给 Commit。
//
// 快速配置（Template）：CFG_TRIGGER[0] temp_valid 置位时，以 temp_index 选中的
// 模板为底，被显式写过的 Cfg Reg File 字段覆盖模板字段；temp_valid 清 0 时全部取
// Cfg Reg File（普通配置）。为此维护一个 19 项「已显式写」dirty 掩码，Trigger Fire
// 时清零。
//
// 反压：Commit 的 PendingTaskQ 满时它不收这一笔 Descriptor，本模块保持着重发，
// 同时拉低 dsa_cfg 的 req_ready 反压 RV core。
//
// 读寄存器隔几拍才回：读不支持同步返回，返回数据走独立的 dsa_rdata 口。同拍
// 回的话 dsa_rq 那边还没把这一笔的目的寄存器记进队列，脉冲就丢了。

#include <array>
#include <deque>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/dte/agcu.h"
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/ip/chip/core/rv_core/rv_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// ── 地址空间（16KB，照文档 §寄存器地址域划分）──
constexpr uint64_t kDteConfigBase = 0x0000;
constexpr uint64_t kDteCtrlBase = 0x0400;     // 不落地 (d)
constexpr uint64_t kDteProfileBase = 0x0800;  // 不落地 (d)
constexpr uint64_t kDteDebugBase = 0x0C00;    // 不落地 (d)
constexpr uint64_t kDteTemplateBase = 0x1000;
constexpr uint64_t kDteTaskqBase = 0x2000;
constexpr uint64_t kDteHmemBase = 0x3000;
constexpr uint64_t kDteTemplateStride = 0x80;
constexpr uint64_t kDteTemplateNum = 8;

// ── Config 区 19 项任务配置寄存器（0x0004~0x004C，全部 R/W）──
// 段 i 一组 {ADDRi_SRC, ADDRi_DST, STRIDEi, DATA_LENi}。
constexpr uint64_t kDteRegTrigger = 0x000;  // CFG_TRIGGER，WO
constexpr uint64_t kDteRegAddr0Src = 0x004;
constexpr uint64_t kDteRegAddr0Dst = 0x008;
constexpr uint64_t kDteRegAddr1Src = 0x00C;
constexpr uint64_t kDteRegAddr1Dst = 0x010;
constexpr uint64_t kDteRegAddr2Src = 0x014;
constexpr uint64_t kDteRegAddr2Dst = 0x018;
constexpr uint64_t kDteRegAddr3Src = 0x01C;
constexpr uint64_t kDteRegAddr3Dst = 0x020;
constexpr uint64_t kDteRegStride0 = 0x024;
constexpr uint64_t kDteRegStride1 = 0x028;
constexpr uint64_t kDteRegStride2 = 0x02C;
constexpr uint64_t kDteRegStride3 = 0x030;
constexpr uint64_t kDteRegDataLen0 = 0x034;
constexpr uint64_t kDteRegDataLen1 = 0x038;
constexpr uint64_t kDteRegDataLen2 = 0x03C;
constexpr uint64_t kDteRegDataLen3 = 0x040;
constexpr uint64_t kDteRegSmWAddr = 0x044;
constexpr uint64_t kDteRegSmWData = 0x048;
constexpr uint64_t kDteRegTransMode = 0x04C;

// 19 项配置字段数。
constexpr uint64_t kDteFieldNum = 19;

// ── CFG_TRIGGER（WO，4 bit，0x0000）位域 ──
constexpr uint64_t kDteTempValid = 1ull << 0;       // [0] 启用 Config Template
constexpr uint64_t kDteTempIndexShift = 1;          // [3:1] 配置表 1~8 号
constexpr uint64_t kDteTempIndexMask = 0x7;

// ── CFG_TRANS_MODE（R/W，10 bit，0x004C）位域 ──
constexpr uint64_t kDteModeMask = 0x7;              // [2:0] transfer_mode
constexpr uint64_t kDteAddrValidShift = 3;          // [6:3] addr_valid[3:0]
constexpr uint64_t kDteAddrValidMask = 0xF;
constexpr uint64_t kDteHwHeaderOp = 1ull << 7;      // [7] 包头 保存/丢弃/修改
constexpr uint64_t kDteWrSharememFlag = 1ull << 8;  // [8] 完成后写 ShareMem
constexpr uint64_t kDteAckTsEn = 1ull << 9;         // [9] 完成后通知 TS（原 task_last）

// 一套 19 项配置。既是 Cfg Reg File 的内容，也是 8 套模板里每一套的内容。
struct DteConfig {
  Segment seg[4]{};
  uint64_t smem_addr = 0;   // CFG_SM_W_ADDR
  uint64_t smem_data = 0;   // CFG_SM_W_DATA
  uint64_t trans_mode = 0;  // CFG_TRANS_MODE

  // 19 项字段按模板内的顺序读/写：0~7 是 ADDR0..3 的 SRC/DST，8~11 是 STRIDE0..3，
  // 12~15 是 DATA_LEN0..3，16/17 是 SM_W_ADDR/DATA，18 是 TRANS_MODE。
  uint64_t Field(uint64_t i) const {
    switch (i) {
      case 0: return seg[0].src;  case 1: return seg[0].dst;
      case 2: return seg[1].src;  case 3: return seg[1].dst;
      case 4: return seg[2].src;  case 5: return seg[2].dst;
      case 6: return seg[3].src;  case 7: return seg[3].dst;
      case 8: return seg[0].stride;  case 9: return seg[1].stride;
      case 10: return seg[2].stride; case 11: return seg[3].stride;
      case 12: return seg[0].len;  case 13: return seg[1].len;
      case 14: return seg[2].len;  case 15: return seg[3].len;
      case 16: return smem_addr;   case 17: return smem_data;
      case 18: return trans_mode;
      default: return 0;
    }
  }
  void SetField(uint64_t i, uint64_t v) {
    switch (i) {
      case 0: seg[0].src = v; return;  case 1: seg[0].dst = v; return;
      case 2: seg[1].src = v; return;  case 3: seg[1].dst = v; return;
      case 4: seg[2].src = v; return;  case 5: seg[2].dst = v; return;
      case 6: seg[3].src = v; return;  case 7: seg[3].dst = v; return;
      case 8: seg[0].stride = v; return;  case 9: seg[1].stride = v; return;
      case 10: seg[2].stride = v; return; case 11: seg[3].stride = v; return;
      case 12: seg[0].len = v & 0xFFFF; return;
      case 13: seg[1].len = v & 0xFFFF; return;
      case 14: seg[2].len = v & 0xFFFF; return;
      case 15: seg[3].len = v & 0xFFFF; return;
      case 16: smem_addr = v; return;   case 17: smem_data = v; return;
      case 18: trans_mode = v; return;
      default: return;
    }
  }
};

class DteRegfile : public BachModule {
 public:
  DteRegfile(ClockPtr clock, const std::string& name, Agcu const& addr_gen,
             uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        agcu(addr_gen),
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
  DteConfig const& Template(uint64_t i) const { return tpl.at(i); }
  DteConfig const& CfgFile() const { return cfg_file; }
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

  // 地址空间三段落地的只有 Config 与 Template 两段；Ctrl/Status、Profile、Debug
  // 与 TaskQ 回读都不参与起任务（TaskQ/HeaderTable 只读回读返回 0）。
  void WriteReg(uint64_t at, uint64_t data) {
    // CFG_TRIGGER：写 0x0000 就是提交任务。
    if (at == kDteRegTrigger) {
      Fire(data);
      return;
    }
    // Config 区 19 项（0x0004~0x004C）。
    if (at >= kDteConfigBase + 0x004 && at <= kDteRegTransMode) {
      int64_t idx = (at - 0x004) / 4;
      cfg_file.SetField(idx, data);
      dirty |= 1ull << idx;
      return;
    }
    // Template 区 8 × 128B，每套 19 项（0x00~0x48，无 TRIGGER）。
    if (at >= kDteTemplateBase && at < kDteTaskqBase) {
      uint64_t rel = at - kDteTemplateBase;
      uint64_t idx = rel / kDteTemplateStride;
      uint64_t off = rel % kDteTemplateStride;
      if (idx >= kDteTemplateNum || off >= kDteFieldNum * 4) return;
      tpl[idx].SetField(off / 4, data);
      return;
    }
    // 其余几段不落地。
  }

  uint64_t ReadReg(uint64_t at) const {
    if (at >= kDteConfigBase + 0x004 && at <= kDteRegTransMode) {
      return cfg_file.Field((at - 0x004) / 4);
    }
    if (at >= kDteTemplateBase && at < kDteTaskqBase) {
      uint64_t rel = at - kDteTemplateBase;
      uint64_t idx = rel / kDteTemplateStride;
      uint64_t off = rel % kDteTemplateStride;
      if (idx >= kDteTemplateNum || off >= kDteFieldNum * 4) return 0;
      return tpl[idx].Field(off / 4);
    }
    return 0;
  }

  // 写 CFG_TRIGGER 那一拍：合并配置 → 采样 STUPV → 组装 4 段位 Descriptor。
  void Fire(uint64_t trigger) {
    bool temp_valid = (trigger & kDteTempValid) != 0;
    uint64_t temp_idx = (trigger >> kDteTempIndexShift) & kDteTempIndexMask;

    // 合并：temp_valid 时以模板为底、显式写过的 Cfg Reg File 字段覆盖；否则全取
    // Cfg Reg File。合并结果放进一份快照，触发后清 dirty。
    DteConfig merged;
    if (temp_valid) {
      merged = tpl[temp_idx];
      for (uint64_t i = 0; i < kDteFieldNum; ++i) {
        if (dirty & (1ull << i)) merged.SetField(i, cfg_file.Field(i));
      }
    } else {
      merged = cfg_file;
    }

    auto d = std::make_shared<Descriptor>();
    d->valid = true;
    // 身份直连采样。出核走哪个 VC 取 TS 随任务下发的 VCID。
    d->stream_id = ids->Stream();
    d->task_id = ids->Task();
    d->user_id = ids->User();
    d->path_id = ids->Path();
    d->vc = ids->Vc();
    d->route = Route(merged.trans_mode & kDteModeMask);
    uint64_t addr_valid = (merged.trans_mode >> kDteAddrValidShift) &
                          kDteAddrValidMask;
    d->hw_header_op = (merged.trans_mode & kDteHwHeaderOp) != 0;
    d->wr_sharemem_flag = (merged.trans_mode & kDteWrSharememFlag) != 0;
    d->ack_ts_en = (merged.trans_mode & kDteAckTsEn) != 0;
    d->smem_addr = merged.smem_addr;
    d->smem_data = merged.smem_data;

    // 逐段拷贝配置，按 addr_valid 决定段是否参与，再展开地址/译码端点。
    for (uint64_t i = 0; i < 4; ++i) {
      Segment s;
      s.valid = (addr_valid >> i) & 1u;
      s.src = merged.seg[i].src;
      s.dst = merged.seg[i].dst;
      s.stride = merged.seg[i].stride;
      s.len = merged.seg[i].len;
      if (s.valid) agcu.ExpandOut(s, d->stream_id, d->route);
      d->seg[i] = s;
    }

    held = d;
    held_seq = out->NextSeq();
    holding = true;
    ++trig_cnt;
    dirty = 0;
  }

  Agcu agcu;
  std::shared_ptr<DsaCfgPort> cfg;
  std::shared_ptr<DsaIdsPort> ids;
  std::shared_ptr<DescPort> out;
  std::shared_ptr<DsaRdataPort> rdata;

  struct ReadBack {
    uint64_t at = 0, value = 0, seq = 0;
  };
  std::deque<ReadBack> pending_read;

  // Cfg Reg File（普通配置路径）+ 19 项 dirty 掩码 + 8 套模板。
  DteConfig cfg_file{};
  uint64_t dirty = 0;
  std::array<DteConfig, kDteTemplateNum> tpl{};

  std::shared_ptr<Descriptor> held;
  bool holding = false, rdata_used = false;
  uint64_t last_seq = 0, held_seq = 0, trig_cnt = 0, write_cnt = 0, read_cnt = 0;

  Logic64 triggers, writes;
};

}  // namespace bach
}  // namespace latch

#endif
