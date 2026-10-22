#ifndef _LATCH_BACH_IP_CHIP_CORE_MU_MU_
#define _LATCH_BACH_IP_CHIP_CORE_MU_MU_

// MU DSA 这一组七个模块的装配。
//
// 一笔任务走的路：RV core 写配置到 regfile，最后写 trigger 锁成一笔进 issue_q；
// agu 按“先循环 tile_K 再循环 tile_N”算三组地址；两条 ldq 分别从 Core Mem 读
// token、从 Matrix Mem 读 weight；matrix exe 按 scale block 分组累加；stq 把结果
// 拼成整拍写回 Core Mem，与 issue_q 的 finish 合成 dsa_done。
//
// TaskQ 保存 8 笔任务；读、算、写各自按顺序推进，允许不同任务的阶段重叠。
//
// 七个模块由外层统一驱动：外层每拍调一次 RunStep()，它按末级先做的次序逐个
// 走一遍，整个 MU 只占外层那一个协程。

#include <memory>
#include <string>

#include "bach/ip/chip/core/mu/agu.h"
#include "bach/ip/chip/core/mu/gen_ep_info.h"
#include "bach/ip/chip/core/mu/issue_q.h"
#include "bach/ip/chip/core/mu/ldq.h"
#include "bach/ip/chip/core/mu/matrix_exe.h"
#include "bach/ip/chip/core/mu/mu_ports.h"
#include "bach/ip/chip/core/mu/regfile.h"
#include "bach/ip/chip/core/mu/stq.h"
#include "bach/ip/chip/core/ts/ts_ports.h"

namespace latch {
namespace bach {

struct MuCfg {
  uint64_t cmem_size = 1024 * 1024 + 32 * 1024;
  uint64_t mmem_size = 36ull * 1024 * 1024;
};

class Mu {
 public:
  Mu(ClockPtr clock, const std::string& name, MuCfg const& setting,
     uint64_t parent = 0)
      : clk(clock), cfg(setting) {
    const uint64_t gid = TraceGroup(name, parent);
    // 都不自己挂时钟：下面那一小段控制直接调各模块的方法往它们的队列里放
    // 东西，那些方法改的是普通容器。各模块要是各占一个协程，就成了几个线程改
    // 同一个容器。外层每拍调一次 RunStep()，它按末级先做的次序走一遍。
    reg = std::make_unique<MuRegfile>(clock, "regfile", gid, false);
    iq = std::make_unique<MuIssueQ>(clock, "issue_q", gid, false);
    ep = std::make_unique<GenEpInfo>(clock, "gen_ep", gid, false);
    token_ldq = std::make_unique<MuLdq>(clock, "token_ldq",
                                        kTokenLdqDepth, gid, false);
    weight_ldq = std::make_unique<MuLdq>(clock, "weight_ldq",
                                         kWeightLdqDepth, gid, false);
    exe = std::make_unique<MatrixExe>(clock, "exe", gid, false);
    stq = std::make_unique<MuStq>(clock, "stq", gid, false);
    done = std::make_shared<DonePort>(clock);
    token_ldq->SetBuffer(4096, 128);
    // Per lane: 4KiB of returned B data and 12 outstanding read descriptors.
    weight_ldq->SetBuffer(64 * 4096, 12);
    ctrl = std::make_unique<Ctrl>(clock, "ctrl", gid, *this, false);
    reg->BindQueueRoom([this] { return iq->HasRoom(); });
    reg->BindAccessError([this](uint64_t at, bool alignment) { ReportControl(at, alignment); });
    reg->BindControl([this](uint64_t at) { return ReadControl(at); },
                     [this](uint64_t at, uint64_t value) { WriteControl(at, value); });
  }

  // ── 对外 ──
  DsaCfgPort& Cfg() { return reg->CfgPort(); }
  std::shared_ptr<DsaCfgPort> CfgPtr() const { return reg->CfgPortPtr(); }
  void AttachCfg(std::shared_ptr<DsaCfgPort> p) { reg->AttachCfg(std::move(p)); }
  DonePort& Done() { return *done; }
  void AttachDone(std::shared_ptr<DonePort> p) { done = std::move(p); }
  MemPort& TokenPort() { return token_ldq->Port(); }
  MemPort& WeightPort() { return weight_ldq->Port(); }
  MemPort& OutPort() { return stq->Port(); }
  void AttachCmemRd(std::shared_ptr<MemPort> p) {
    token_ldq->AttachPort(std::move(p));
  }
  void AttachMmemRd(std::shared_ptr<MemPort> p) {
    weight_ldq->AttachPort(std::move(p));
  }
  void BindMmemWindow(std::function<void(bool)> request, std::function<bool()> ready) {
    mmem_window_request = std::move(request); mmem_window_ready = std::move(ready);
  }
  void AttachCmemWr(std::shared_ptr<MemPort> p) { stq->AttachPort(std::move(p)); }
  // DTE 搬运时经这条数据线把 topK 直接写进 topK_ep_table。
  void AttachMuTopk(std::shared_ptr<MuTopkPort> p) { topk_port = std::move(p); }

  // ── 配置面 ──
  GenEpInfo& EpInfo() { return *ep; }
  std::shared_ptr<DsaRdataPort> RdataPtr() const { return reg->RdataPtr(); }

  // topK 里第 i 个专家在本 EP Group 内的序号。topK 直接存组内序号，读出来就是；
  // 没配 topK 的那一档按 topK 里的先后当序号用。
  uint64_t LocalEpOf(MuInflight const& f, uint64_t i) const {
    if (f.cfg.decoded_mode) return 0;
    std::vector<TopkEntry> const& t = TopkOf(f);
    if (i >= t.size()) return i;
    return t[i].local_ep_index;
  }
  // topK 里第 i 个专家的权重。合并成一份时乘它。
  float WeightOf(MuInflight const& f, uint64_t i) const {
    if (i < f.cfg.experts.size()) return f.cfg.experts[i].coefficient;
    std::vector<TopkEntry> const& t = TopkOf(f);
    if (i >= t.size()) return 1.0f;
    return t[i].weight;
  }
  // 这一笔用哪一份 topK：按 stream_id 读 topK_ep_table。DTE 搬运时写进来，
  // MU 计算时与 token / weight 同时读，不再有前置串行读。
  std::vector<TopkEntry> const& TopkOf(MuInflight const& f) const {
    return ep->Topk(f.cfg.stream_id, f.cfg.Experts());
  }

  bool Plan(MuTaskCfg &task, MuInflight::Fault* fault = nullptr) const {
    if (fault) *fault = {};
    auto fail = [&](uint64_t type = 0, bool mmem = false, uint64_t address = 0, bool valid = false) {
      if (fault) *fault = {type, address, mmem, valid};
      return false;
    };
    if (!task.decoded_mode) return true;
    task.experts.clear();
    if (task.stream_id >= 16) return fail();
    auto const &bytes = ep->TopkBytes(task.stream_id);
    uint64_t routed = task.router_ep_en ? GenEpInfo::CountOf(bytes) : 0;
    if (routed > task.router_limit || routed > 32) return fail();
    uint64_t shared = __builtin_popcountll(task.share_mask);
    task.expert_count = shared + routed;
    if (!task.expert_count) return true;
    if (!task.kblock || !task.nblock) return fail();
    if (task.stream_id && (!task.a_stream_stride || !task.c_stream_stride)) return fail();
    if (task.expert_count > 1 && !(task.ep_reduce ? task.token_expert_stride : task.output_expert_stride)) return fail();
    uint64_t ref_k = task.PrimK(), total_k = ref_k * task.kblock;
    uint64_t ref_bytes = ref_k * task.PrimN() * numeric::ElemBitsOf(task.b_dtype) / 512;
    auto entries = GenEpInfo::Parse(bytes, routed);
    if (entries.size() != routed) return fail();
    bool used_b_stride = (task.share_mask & ~1ull) || (routed && task.share_slot_count);
    for (auto const& entry : entries) used_b_stride |= entry.local_ep_index != 0;
    if (used_b_stride && !task.b_expert_stride) return fail();
    if (used_b_stride && task.b_expert_stride % ref_bytes) return fail(1, true);
    auto stride = [&](numeric::DataType dtype, uint64_t &result) {
      MuTaskCfg shape = task; shape.b_dtype = dtype;
      uint64_t k = shape.PrimK();
      uint64_t data = k * shape.PrimN() * numeric::ElemBitsOf(dtype) / 512;
      uint64_t numerator = task.b_expert_stride * ref_k * data;
      uint64_t denominator = k * ref_bytes;
      if (total_k % k || numerator % denominator) return fail();
      result = numerator / denominator;
      return true;
    };
    uint64_t shared_stride = 0, routed_stride = 0;
    if ((task.share_slot_count && (task.share_mask || routed)) && !stride(task.share_dtype, shared_stride)) return false;
    if (routed && !stride(task.router_dtype, routed_stride)) return false;
    auto span_of = [&](numeric::DataType dtype) {
      return task.nblock * total_k * task.PrimN() * numeric::ElemBitsOf(dtype) / 512;
    };
    if (routed && task.share_slot_count && shared_stride < span_of(task.share_dtype)) return fail();
    auto add = [&](numeric::DataType dtype, uint64_t base, float weight, bool router) {
      MuTaskCfg shape = task; shape.b_dtype = dtype;
      uint64_t k = shape.PrimK();
      if (task.a_dtype == numeric::DataType::kMxfp8 && dtype == numeric::DataType::kBf16) return fail();
      uint64_t lane_bytes = k * shape.PrimN() * numeric::ElemBitsOf(dtype) / 512;
      if (total_k % k || total_k / k > 65535) return fail();
      if (base % lane_bytes) return fail(1, true, base, true);
      uint64_t span = (total_k / k) * task.nblock * lane_bytes;
      uint64_t limit = std::min<uint64_t>(cfg.mmem_size / 64, 512 * 1024);
      if (base > limit || span > limit - base)
        return fail(2, true, base >= limit ? base : base + span - 1, true);
      task.experts.push_back({dtype, base, total_k / k, weight, router});
      return true;
    };
    for (uint64_t i = 0; i < task.share_slot_count; ++i)
      if ((task.share_mask >> i) & 1) {
        if (i && (!shared_stride || shared_stride < span_of(task.share_dtype))) return fail();
        if (!add(task.share_dtype, task.b_addr + i * shared_stride, 1.0f, false)) return false;
      }
    for (auto const &entry : entries) {
      if (entry.local_ep_index && routed_stride < span_of(task.router_dtype)) return fail();
      uint64_t base = task.b_addr + task.share_slot_count * shared_stride + entry.local_ep_index * routed_stride;
      if (!add(task.router_dtype, base, entry.weight, true)) return false;
    }
    uint64_t a_span = total_k * numeric::ElemBitsOf(task.a_dtype) / 8;
    uint64_t c_span = task.nblock * task.PrimN() * (task.out_bf16 ? 2 : 4);
    uint64_t a_extent = a_span + (task.ep_reduce ? (task.expert_count - 1) * task.token_expert_stride : 0);
    uint64_t c_extent = c_span + (!task.ep_reduce ? (task.expert_count - 1) * task.output_expert_stride : 0);
    uint64_t a = task.a_addr + task.stream_id * task.a_stream_stride;
    uint64_t c = task.c_addr + task.stream_id * task.c_stream_stride;
    uint64_t limit = std::min<uint64_t>(cfg.cmem_size, 1024 * 1024);
    uint64_t a_alignment = task.a_dtype == numeric::DataType::kMxfp8 ? 32 : 8;
    if (a % a_alignment) return fail(1, false, a, true);
    if (c % 8) return fail(1, false, c, true);
    if (a > limit || a_extent > limit - a)
      return fail(2, false, a >= limit ? a : a + a_extent - 1, true);
    if (c > limit || c_extent > limit - c)
      return fail(2, false, c >= limit ? c : c + c_extent - 1, true);
    if (task.expert_count > 1 && (task.ep_reduce ? task.token_expert_stride < a_span : task.output_expert_stride < c_span)) return fail();
    if (task.stream_id && (task.a_stream_stride % a_alignment || task.c_stream_stride % 8)) return fail(1);
    if (task.expert_count > 1 && (task.ep_reduce ? task.token_expert_stride % a_alignment : task.output_expert_stride % 8)) return fail(1);
    return true;
  }
  uint64_t Error() const {
    return report_error;
  }
  void ReportControl(uint64_t at, bool) {
    // Unmapped and unaligned CSR accesses both use control address-error type 0.
    if (!control_error) { control_error = true; control_error_addr = uint32_t(at); }
  }
  uint64_t ErrorReports() const { return error_reports; }
  bool DatapathIdle() const {
    return token_ldq->Quiescent() && weight_ldq->Quiescent() &&
           exe->Quiescent() && stq->Quiescent();
  }
  uint64_t ReadControl(uint64_t at) const {
    auto head = iq->Head();
    if (at == kMuCtrl) return clearing && !cancel_debug ? 1 : 0;
    if (at == kMuStatus) {
      if (clearing) return 2;
      if (Error()) return 3;
      if (debug_stop) return DatapathIdle() ? 4 : 5;
      return idle_cycles < 2 ? 1 : 0;
    }
    if (at == kMuTaskqStatus) return iq->Size() | (iq->HeadSlot() << 4) |
        (iq->TailSlot() << 7) | (DatapathIdle() ? 0 : 1u << 10) |
        (reg->Reserved() ? 1u << 14 : 0);
    uint64_t task_fault = report_kind == 2 && head ? 1u | (head->fault.type << 3) |
        (head->fault.mmem ? 4u : 0u) | (head->fault.address_valid ? 1u << 10 : 0u) : 0;
    if (at == kMuErrInfo) return (numeric_error_en ? 1u << 12 : 0) |
        (report_kind ? 1u << 11 : 0) | (report_flags ?
        ((report_flags & 15) << 6) | (1u << 5) : 0) |
        task_fault;
    if (at == 0x410) return issue_id;
    if (at == 0x414) return control_error ? control_error_addr : 0;
    if (at == 0x804) return reg->Triggers();
    if (at == 0x80c) return exe->Prims();
    if (at == 0xc00) return debug_en ? 1 : 0;
    if (at == 0xc04) return debug_command;
    if (at >= 0x2000 && at < 0x2800) {
      auto entry = iq->AtSlot((at - 0x2000) / 256);
      if (!entry) return 0;
      uint64_t offset = at & 255;
      if (offset <= 0x20 && !(offset & 3)) return entry->cfg.words[offset / 4];
      if (offset == 0x24) return entry->cfg.stream_id | (entry->cfg.task_id << 4) |
          (entry->cfg.user_id << 10);
      if (offset == 0x28) return (entry->stage == MuStage::kFinished ? 2 :
          entry->issued ? 1 : 0) | (entry->stage == MuStage::kFinished && entry->error ? 4 : 0);
      if (offset == 0x2c) {
        uint64_t summary = entry->cfg.numeric_error_en ? 1u << 16 : 0;
        if (entry->stage != MuStage::kFinished) return summary;
        return summary | (entry->numeric_flags ? 16 : 0) | (entry->numeric_flags << 5) |
            (entry->error == 1 || entry->error == 3 ? (1u << 17) | (entry->fault.type << 1) |
             (entry->fault.mmem ? 1 : 0) | (entry->fault.address_valid ? 8 : 0) : 0);
      }
      if (offset == 0x30) return uint32_t(entry->error_addr);
    }
    if (at >= 0x3000 && at < 0x4000 && !(at & 3)) {
      auto const& bytes = ep->TopkBytes((at - 0x3000) / 256);
      uint64_t offset = at & 255, word = 0;
      for (uint64_t i = 0; i < 4 && offset + i < bytes.size(); ++i)
        word |= uint64_t(bytes[offset + i]) << (8 * i);
      return word;
    }
    return 0;
  }
  void WriteControl(uint64_t at, uint64_t value) {
    if (at == kMuErrInfo) numeric_error_en = (value & (1u << 12)) != 0;
    if (at == 0x410) issue_id = value & 0x3ffffff;
    if (at == kMuCtrl && (value & 1)) BeginClear(false);
    else if (at == kMuCtrl && (value & 2)) {
      auto head = iq->Head();
      if (report_kind == 2 && head && head->error && head->stage == MuStage::kFinished) {
        head->error = 0; iq->RetireFront();
      }
      if (report_kind == 1) control_error = false;
      report_kind = report_error = report_flags = 0;
    }
    if (at == 0xc00) {
      if (value & 2) { debug_en = debug_stop = step_budget = false; debug_command = 0; }
      else if ((value & 1) && !debug_en) { debug_en = debug_stop = true; }
    }
    if (at == 0xc04 && debug_en) {
      if (value == 1) { debug_stop = true; step_budget = false; debug_command = DatapathIdle() ? 0 : 1; }
      if (value == 2 && !Error() && debug_stop && DatapathIdle()) { step_budget = true; debug_command = 2; }
      if (value == 3) { debug_stop = step_budget = false; debug_command = 0; }
      if (value == 4) BeginClear(true);
    }
    if (at >= 0x3000 && at < 0x4000 && !(at & 3)) {
      uint64_t sid = (at - 0x3000) / 256, offset = at & 255;
      auto bytes = ep->TopkBytes(sid); bytes.resize(256);
      for (uint64_t i = 0; i < 4; ++i) bytes[offset + i] = uint8_t(value >> (8 * i));
      ep->WriteTopk(sid, bytes);
    }
  }
  void BeginClear(bool cancel) {
    clearing = true; cancel_debug = cancel; clear_wait = 0;
    reg->BlockTrigger(true); reg->CancelPending();
    token_ldq->Cancel(); weight_ldq->Cancel(); exe->Cancel(); stq->Cancel();
    if (cancel) { debug_stop = true; step_budget = false; debug_command = 4; }
  }

  // ── 观测 ──
  MuRegfile& Regfile() { return *reg; }
  MuIssueQ& IssueQ() { return *iq; }
  MatrixExe& Exe() { return *exe; }
  MuStq& Stq() { return *stq; }
  // 上一笔宣告完成时的笔数与身份，供 Core 层发波形。完成脉冲本身只带 stream 与
  // task（Drive 的第三参默认 0），user 取自 trigger 写带进来的那一份。
  uint64_t DoneCnt() const { return ctrl->DoneCnt(); }
  uint64_t DoneTask() const { return ctrl->DoneTask(); }
  uint64_t DoneUser() const { return ctrl->DoneUser(); }
  // 真正发行进执行通路那一拍起的笔数与身份，Core 层发 dsa_task_dispatch 用。
  uint64_t DispatchCnt() const { return ctrl->DispatchCnt(); }
  uint64_t DispatchTask() const { return ctrl->DispatchTask(); }
  uint64_t DispatchUser() const { return ctrl->DispatchUser(); }
  uint64_t DispatchCfgCount() const { return ctrl->DispatchCfgCount(); }
  // 这一笔在矩阵执行单元里进出所跨那一段的两端，Core 层发 dsa_calc_start /
  // dsa_calc_done 用。
  uint64_t CalcStartCnt() const { return ctrl->CalcStartCnt(); }
  uint64_t CalcStartTask() const { return ctrl->CalcStartTask(); }
  uint64_t CalcStartUser() const { return ctrl->CalcStartUser(); }
  uint64_t CalcStartExpert() const { return ctrl->CalcStartExpert(); }
  uint64_t CalcStartK() const { return ctrl->CalcStartK(); }
  uint64_t CalcStartN() const { return ctrl->CalcStartN(); }
  uint64_t CalcStartKblock() const { return ctrl->CalcStartKblock(); }
  uint64_t CalcStartNblock() const { return ctrl->CalcStartNblock(); }
  uint64_t CalcStartPrimK() const { return ctrl->CalcStartPrimK(); }
  uint64_t CalcStartPrimN() const { return ctrl->CalcStartPrimN(); }
  uint64_t CalcStartADtype() const { return ctrl->CalcStartADtype(); }
  uint64_t CalcStartBDtype() const { return ctrl->CalcStartBDtype(); }
  uint64_t CalcStartOutBf16() const { return ctrl->CalcStartOutBf16(); }
  uint64_t CalcDoneCnt() const { return ctrl->CalcDoneCnt(); }
  uint64_t CalcDoneTask() const { return ctrl->CalcDoneTask(); }
  uint64_t CalcDoneUser() const { return ctrl->CalcDoneUser(); }

  void RunStep() {
    // DTE 搬运时写进来的 topK 这一拍先收进 topK_ep_table，下面才算得到。
    StepTopk();
    // 软件轮询 SYS_STATUS 等一笔任务做完，忙不忙由这里每拍写进去。空的判据与
    // dsa_done 一样要算上写回落地：离开 stq 的写请求还要经端口到存储。
    if (Quiescent()) {
      ++idle_cycles;
    } else {
      idle_cycles = 0;
    }
    reg->SetBusy(idle_cycles < 2);
    reg->RunStep();
    ep->RunStep();
    stq->RunStep();
    exe->RunStep();
    token_ldq->RunStep();
    weight_ldq->RunStep();
    iq->RunStep();
    ctrl->RunStep();
  }

  bool Quiescent() const {
    return reg->Quiescent() && iq->Quiescent() && exe->Quiescent() &&
           stq->Quiescent() && token_ldq->Quiescent() &&
           weight_ldq->Quiescent();
  }

 private:
  // DTE 那条 topK 数据线：每拍读一次，valid 且序号没见过的写进 topK_ep_table。
  // 同一拍数据会连着两拍出现在端口上，按序号认它。
  void StepTopk() {
    if (!topk_port || !topk_port->Valid()) return;
    if (topk_port->Seq() == last_topk_seq) return;
    last_topk_seq = topk_port->Seq();
    auto d = topk_port->Data();
    if (d) ep->WriteTopk(topk_port->StreamId(), *d);
  }
  // 把七个模块串起来的那一小段控制。放在装配里，因为它读的是各模块的接口，
  // 不属于任何一个模块自己。
  class Ctrl : public BachModule {
   public:
    Ctrl(ClockPtr c, const std::string& name, uint64_t parent, Mu& owner,
         bool tick)
        : BachModule(c, name, parent, tick), mu(owner) {}

    // 与别的模块一样由 Mu::RunStep() 驱动，不自己挂时钟。

    uint64_t DoneCnt() const { return done_cnt; }
    uint64_t DoneTask() const { return done_task; }
    uint64_t DoneUser() const { return done_user; }
    // 真正发行进执行通路那一拍起的笔数与身份，Core 层发 dsa_task_dispatch 用。
    // 判据见 Load()：这一笔的第一个 tile 开始发起访存。越界那一路也计 —— 它一样
    // 是从这儿起的，不计的话 MU-DSA-ISQ 那一行就没有终点。
    uint64_t DispatchCnt() const { return dispatch_cnt; }
    uint64_t DispatchTask() const { return dispatch_task; }
    uint64_t DispatchUser() const { return dispatch_user; }
    // 这笔任务配置过的寄存器数，随发行那一拍带出去（Perfetto 的 MU-DSA args）。
    uint64_t DispatchCfgCount() const { return dispatch_cfg_count; }
    // 这一笔在矩阵执行单元里进出所跨那一段的两端，Core 层发 dsa_calc_start /
    // dsa_calc_done 用。起点是第一个 tile 进 exe、终点是最后一个 tile 的后一拍，量的是
    // 这笔任务的真实 MAC 时间（kblock × 专家数 × nblock 拍），不把 10 级流水深度算进去。
    // 越界那一笔不经过正常计算，两端都不计：它没有计算阶段。
    uint64_t CalcStartCnt() const { return calc_start_cnt; }
    uint64_t CalcStartTask() const { return calc_start_task; }
    uint64_t CalcStartUser() const { return calc_start_user; }
    // 这笔任务的 GEMM 规模：专家数、K 与 N（M 恒为 1，不单列）。Core 层发
    // dsa_calc_start_expert / k / n 用，好让 Perfetto 里 MU-DSA-CALC 的 args 带上尺寸。
    uint64_t CalcStartExpert() const { return calc_start_expert; }
    uint64_t CalcStartK() const { return calc_start_k; }
    uint64_t CalcStartN() const { return calc_start_n; }
    uint64_t CalcStartKblock() const { return calc_start_kblock; }
    uint64_t CalcStartNblock() const { return calc_start_nblock; }
    uint64_t CalcStartPrimK() const { return calc_start_prim_k; }
    uint64_t CalcStartPrimN() const { return calc_start_prim_n; }
    uint64_t CalcStartADtype() const { return calc_start_adtype; }
    uint64_t CalcStartBDtype() const { return calc_start_bdtype; }
    uint64_t CalcStartOutBf16() const { return calc_start_outbf16; }
    uint64_t CalcDoneCnt() const { return calc_done_cnt; }
    uint64_t CalcDoneTask() const { return calc_done_task; }
    uint64_t CalcDoneUser() const { return calc_done_user; }

   protected:
    void Step() override {
      // 上一拍算到最后一个 tile 时只记了身份、置 pending，这一拍开头才补 calc_done：
      // 段长按 tile 数闭合（最后一个 tile 的后一拍 − 第一个 tile = kblock × 专家数 ×
      // nblock），而不是少一拍。
      if (calc_done_pending) {
        ++calc_done_cnt;
        calc_done_pending = false;
      }
      if (mu.clearing) {
        mu.done->Idle();
        if (mu.DatapathIdle() && ++mu.clear_wait >= 2) {
          mu.iq->Clear(); active_seq = ~0ull; window_requested = false; mu.clearing = false;
          mu.report_kind = mu.report_error = mu.report_flags = 0; mu.control_error = false;
          mu.token_ldq->Resume(); mu.weight_ldq->Resume(); mu.reg->BlockTrigger(false);
          if (mu.mmem_window_request) mu.mmem_window_request(false);
          mu.debug_command = 0;
        }
        return;
      }
      if (mu.debug_command == 1 && mu.DatapathIdle()) mu.debug_command = 0;
      // Drain 的推进排在最前：这一拍先把上一拍进的那一步走掉，外面才看得到
      // 每一步各占一拍。
      Finish();
      if (mu.control_error && !mu.report_kind) {
        mu.report_kind = 1; mu.report_error = 4; ++mu.error_reports;
      }
      Retire();
      Store();
      Compute();
      Load();
      Accept();
    }

   private:
    void Accept() {
      // 第一步就是阻塞任务下发。
      if (!mu.reg->HasPending() || !mu.iq->HasRoom()) return;
      auto task = mu.reg->TakePending();
      mu.iq->Push(task);
    }

    // 每笔读请求携带任务号，下一任务可在前一任务计算和写回时预取。
    void Load() {
      MuInflight* f = mu.iq->FirstToLoad();
      if (f == nullptr) return;
      if (f->seq != active_seq) {
        if (mu.debug_stop && !mu.step_budget) return;
        if (mu.debug_stop && !mu.DatapathIdle()) return;
        if (!f->planned) {
          bool valid = mu.Plan(f->cfg, &f->fault);
          f->planned = true; f->total = f->cfg.Tiles(); f->agu = MuAgu(f->cfg);
          if (!valid) { f->error = 1; f->error_addr = f->fault.address; f->dropped = true; f->total = 0; }
        }
        if (f->total && mu.mmem_window_request) {
          mu.mmem_window_request(true);
          window_requested = true;
          if (mu.mmem_window_ready && !mu.mmem_window_ready()) return;
        }
        if (mu.step_budget) { mu.step_budget = false; step_seq = f->seq; }
        active_seq = f->seq;
      }
      if (!f->total) {
        f->stage = MuStage::kFinished;
        if (f->seq == step_seq) { mu.debug_command = 0; step_seq = ~0ull; }
        return;
      }
      if (!mu.token_ldq->HasRoom() || !mu.weight_ldq->HasRoom()) return;
      // 这一笔的头一个 tile 真正开始发起访存，就是它「发行」那一拍。写在越界那一路
      // 之前：越界的任务也是从这儿起的（它只是余下 tile 作废），不计的话
      // MU-DSA-ISQ 那一行就没有终点。
      if (f->issued == 0) {
        ++dispatch_cnt;
        dispatch_task = f->cfg.task_id;
        dispatch_user = f->cfg.user_id;
        // 这笔任务配置过的寄存器数，随发行那一拍带出去，Perfetto 的 MU-DSA args 靠它
        // 带上「这一笔任务配置了多少个寄存器」。
        dispatch_cfg_count = f->cfg.cfg_count;
      }
      MuStep s = f->agu.Peek();
      LOGCHECK(f->agu.CheckStep(s, mu.cfg.cmem_size, mu.cfg.mmem_size),
               "MU: task passed decode with an invalid primitive address");
      // token 与权重的请求都带上 scale：block scale 与数据一一映射，存储把它
      // 从 scale 旁带里取出来接在正文后面，不占独立的读通道。权重只有 MXFP8
      // 那一档带。
      auto tile_cfg = f->cfg.ExpertCfg(s.e_idx);
      MuAgu tile_shape(tile_cfg);
      bool has_scale = tile_shape.ScaleBytes() != 0;
      if (!mu.token_ldq->HasRoomFor(tile_shape.TokenBytes()) ||
          !mu.weight_ldq->HasRoomFor(tile_shape.WeightBytes())) return;
      MuLdq::Req token_request{f->agu.TokenAddr(s), tile_shape.TokenBytes(), f->seq, has_scale};
      token_request.pair = f->cfg.decoded_mode;
      token_request.memory_limit = std::min<uint64_t>(mu.cfg.cmem_size, 1024 * 1024);
      mu.token_ldq->Push(token_request);
      MuLdq::Req weight_request{f->agu.WeightAddr(s, mu.LocalEpOf(*f, s.e_idx)),
                               tile_shape.WeightBytes(), f->seq, tile_shape.WeightScaleBytes() != 0};
      weight_request.lane_memory = f->cfg.decoded_mode;
      weight_request.lane_bytes = tile_shape.WeightBytes() / 64;
      uint64_t block = numeric::ScaleBlockOf(tile_cfg.b_dtype);
      weight_request.scale_group = block ? block * numeric::ElemBitsOf(tile_cfg.b_dtype) / 8 : 32;
      mu.weight_ldq->Push(weight_request);
      f->agu.Next();
      f->out_addr.push_back(f->agu.OutAddr(s));
      f->steps.push_back(s);
      ++f->issued;
      f->stage = MuStage::kLoading;
    }

    void Compute() {
      MuInflight* f = mu.iq->FirstToCompute();
      if (f == nullptr) return;
      if (!mu.token_ldq->HasData() || !mu.weight_ldq->HasData() || !mu.exe->HasRoom()) return;
      MuLdq::Rsp t = mu.token_ldq->TakeData();
      MuLdq::Rsp w = mu.weight_ldq->TakeData();
      LOGCHECK(t.tag == f->seq && w.tag == f->seq, "MU: read response belongs to another task");
      if (!f->computed) mu.exe->ResetFlags();
      if (f->dropped) {
        ++f->computed;
        return;
      }
      // scale 段跟在 token 响应正文之后。
      auto step = f->steps.at(f->computed);
      auto tile_cfg = f->cfg.ExpertCfg(step.e_idx);
      MuAgu tile_shape(tile_cfg);
      if ((t.error || w.error) && !f->error) {
        f->error = 3;
        f->error_addr = t.error ? f->agu.TokenAddr(step) :
            tile_cfg.b_addr + (step.n_idx * tile_cfg.kblock + step.k_idx) * tile_shape.WeightBytes() / 64;
        f->fault = {3, f->error_addr, !t.error, true};
      }
      uint64_t body = tile_shape.TokenBytes();
      std::vector<uint8_t> token(t.data.begin(),
                                 t.data.begin() + (t.data.size() < body
                                                       ? t.data.size()
                                                       : body));
      std::vector<uint8_t> scale;
      if (t.data.size() > body) {
        scale.assign(t.data.begin() + body, t.data.end());
      }
      // 权重那一块同样是正文后面接 scale。
      uint64_t wbody = tile_shape.WeightBytes();
      std::vector<uint8_t> weight(w.data.begin(),
                                  w.data.begin() + (w.data.size() < wbody
                                                        ? w.data.size()
                                                        : wbody));
      std::vector<uint8_t> wscale;
      if (w.data.size() > wbody) {
        wscale.assign(w.data.begin() + wbody, w.data.end());
      }
      uint64_t at = f->computed < f->out_addr.size() ? f->out_addr[f->computed]
                                                     : 0;
      // tile 的循环顺序由内往外是 K、专家、N，所以同一列的几段与这一列的几个
      // 专家都连着算。算到最后才产出这一列的结果。
      uint64_t kb = tile_cfg.kblock;
      uint64_t ne = f->cfg.Experts();
      uint64_t ki = step.k_idx;
      uint64_t ei = step.e_idx;
      mu.exe->Issue(tile_cfg, token, weight, scale, wscale, at, ki == 0, ki + 1 == kb,
                    ei == 0, ei + 1 == ne, mu.WeightOf(*f, ei), f->seq);
      // 这一笔的头一个 tile 进执行单元，就是它「算起来了」那一拍。真实的 MAC 时间
      // 按 tile 数走（每个 tile 一拍），不把 10 级流水深度算进去：起点取第一个 tile，
      // 终点取最后一个 tile 的后一拍，段长就是 kblock × 专家数 × nblock 拍。
      if (f->computed == 0) {
        ++calc_start_cnt;
        calc_start_task = f->cfg.task_id;
        calc_start_user = f->cfg.user_id;
        // 尺寸一并锁存，供波形带出去：专家数、K = PrimK × kblock、N = PrimN × nblock，
        // M 恒为 1（一条原语就是 1×K×N）。另带原语本身的 K/N 与三处数据类型。
        calc_start_expert = f->cfg.Experts();
        calc_start_k = f->cfg.PrimK() * f->cfg.kblock;
        calc_start_n = f->cfg.PrimN() * f->cfg.nblock;
        calc_start_kblock = f->cfg.kblock;
        calc_start_nblock = f->cfg.nblock;
        calc_start_prim_k = f->cfg.PrimK();
        calc_start_prim_n = f->cfg.PrimN();
        calc_start_adtype = static_cast<uint64_t>(f->cfg.a_dtype);
        calc_start_bdtype = static_cast<uint64_t>(f->cfg.b_dtype);
        calc_start_outbf16 = f->cfg.out_bf16 ? 1 : 0;
      }
      // 最后一个 tile：这一拍只记身份、置 pending，下一个 Step() 开头再补 calc_done，
      // 好让段长按 tile 数闭合而不是少一拍。
      if (f->computed + 1 == f->total) {
        calc_done_task = f->cfg.task_id;
        calc_done_user = f->cfg.user_id;
        calc_done_pending = true;
      }
      f->numeric_flags |= mu.exe->Flags();
      if (!f->error && f->cfg.numeric_error_en && (f->numeric_flags & (numeric::kFpNv | numeric::kFpOf)))
        f->error = 2;
      ++f->computed;
      f->stage = MuStage::kComputing;
    }

    void Store() {
      if (!mu.exe->HasResult()) return;
      MuInflight* f = mu.iq->BySeq(mu.exe->NextResult().task);
      if (f == nullptr || !mu.stq->HasRoom(mu.exe->NextResult().out.size() == 128 &&
                                             !mu.exe->NextResult().cfg.out_bf16)) return;
      MatrixExe::Result r = mu.exe->TakeResult();
      mu.stq->Push(r.addr, r.out, r.cfg.out_bf16, f->stored + 1 == f->OutTotal(), f->seq);
      ++f->stored;
      f->stage = MuStage::kStoring;
    }

    void Finish() {
      // 最后一次CM写经过端口后逐任务完成；年轻任务的返回和写回不阻塞旧任务退休。
      while (mu.stq->HasCompletedTask()) {
        auto task = mu.iq->BySeq(mu.stq->TakeCompletedTask());
        LOGCHECK(task && task->Done(), "MU: write completion without a completed task");
        task->stage = MuStage::kFinished;
        if (task->seq == step_seq) { mu.debug_command = 0; step_seq = ~0ull; }
      }
      // MM只由读阶段持有，计算和CM回程不延长MM独占窗口。
      if (window_requested && !mu.iq->FirstToLoad() && mu.weight_ldq->Quiescent()) {
        mu.mmem_window_request(false);
        window_requested = false;
      }
    }

    void Retire() {
      MuInflight* f = mu.iq->Head();
      if (f == nullptr || f->stage != MuStage::kFinished) {
        mu.done->Idle();
        return;
      }
      if (f->error) {
        f->stage = MuStage::kFinished;
        if (!f->reported_error && !mu.report_kind) {
          f->reported_error = true; ++mu.error_reports;
          mu.report_kind = 2; mu.report_error = f->error; mu.report_flags = f->numeric_flags;
        }
        mu.done->Idle();
        if (f->seq == step_seq) { mu.debug_command = 0; step_seq = ~0ull; }
        return;
      }
      // 全部 tile 都写回后与 issue_q 的 finish 合成 dsa_done。波形那一路每笔完成
      // 都记一笔（spans 靠 dsa_start / dsa_done 配对出“忙”段）；报 TS 的 dsa_done
      // 只有 task_last 那一笔，其余任务完成只推进 issue_q，不通知 TS。
      done_task = f->cfg.task_id;
      done_user = f->cfg.user_id;
      ++done_cnt;
      if (f->cfg.task_last) {
        mu.done->Drive(f->cfg.stream_id, f->cfg.task_id);
      } else {
        mu.done->Idle();
      }
      f->stage = MuStage::kFinished;
      if (f->seq == step_seq) { mu.debug_command = 0; step_seq = ~0ull; }
      mu.iq->RetireFront();
    }

    uint64_t active_seq = ~0ull;
    uint64_t step_seq = ~0ull;
    Mu& mu;
    bool window_requested = false;
    // 宣告完成的笔数与身份，供 Core 层发波形。
    uint64_t done_cnt = 0, done_task = 0, done_user = 0;
    // 真正发行（第一个 tile 开始发起访存）的笔数与身份，同上。
    uint64_t dispatch_cnt = 0, dispatch_task = 0, dispatch_user = 0;
    // 这笔任务配置过的寄存器数，随发行那一拍带出去。
    uint64_t dispatch_cfg_count = 0;
    // 这一笔的第一个 tile 进矩阵执行单元、最后一个 tile 后一拍那一拍的笔数与身份。
    uint64_t calc_start_cnt = 0, calc_start_task = 0, calc_start_user = 0;
    uint64_t calc_start_expert = 0, calc_start_k = 0, calc_start_n = 0;
    uint64_t calc_start_kblock = 0, calc_start_nblock = 0;
    uint64_t calc_start_prim_k = 0, calc_start_prim_n = 0;
    uint64_t calc_start_adtype = 0, calc_start_bdtype = 0, calc_start_outbf16 = 0;
    uint64_t calc_done_cnt = 0, calc_done_task = 0, calc_done_user = 0;
    // 最后一个 tile 已算完、等下一拍补 calc_done 的标志。
    bool calc_done_pending = false;
  };

  ClockPtr clk;
  MuCfg cfg;
  // 执行通路连着空了几拍，用来判 SYS_STATUS.BUSY 落下去。
  uint64_t idle_cycles = 0, error_reports = 0;
  bool numeric_error_en = false;
  bool clearing = false, cancel_debug = false, debug_en = false;
  bool debug_stop = false, step_budget = false;
  uint64_t clear_wait = 0, debug_command = 0;
  uint64_t issue_id = 0, report_kind = 0, report_error = 0, report_flags = 0;
  bool control_error = false;
  uint64_t control_error_addr = 0;
  std::function<void(bool)> mmem_window_request;
  std::function<bool()> mmem_window_ready;
  // topK 数据线最近一笔写进来的序号，按它去重。
  uint64_t last_topk_seq = 0;
  std::unique_ptr<MuRegfile> reg;
  std::unique_ptr<MuIssueQ> iq;
  std::unique_ptr<GenEpInfo> ep;
  std::unique_ptr<MuLdq> token_ldq, weight_ldq;
  std::unique_ptr<MatrixExe> exe;
  std::unique_ptr<MuStq> stq;
  std::shared_ptr<MuTopkPort> topk_port;
  std::shared_ptr<DonePort> done;
  std::unique_ptr<Ctrl> ctrl;
};

}  // namespace bach
}  // namespace latch

#endif
