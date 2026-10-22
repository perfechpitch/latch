#ifndef _LATCH_BACH_IP_CHIP_CORE_MU_REGFILE_
#define _LATCH_BACH_IP_CHIP_CORE_MU_REGFILE_

// MU 的配置寄存器。
//
// 分静态与动态两档：静态配置基本不随用户变化，初始化阶段配好、业务流阶段快速
// 调用；动态配置随用户变化，跟随任务下发，含静态配置的选择。
//
// streamID / taskID / userID 不再由软件写寄存器：RV 在发出 dsaw 那一拍把 CSR
// 抄进配置请求，写 TASK_TRIGGER 被收下时从这笔请求采样进任务快照。dsa_done 回
// 给 TS 的 stream_id / task_id 就是这一组，与 DTE、VU 同一套做法。
//
// 寄存器偏移照《Matrix Unit DSA》§Register Map Overview（任务配置 0x0000~0x03FF），
// 位域照各寄存器那张位域表。地址模型：token 与结果的物理地址由硬件按
// base + stream_id × stream_stride 算，软件只写 base 与 stride，不再把 stream
// 偏移乘进绝对地址。

#include <array>
#include <deque>
#include <string>
#include <functional>
#include <vector>

#include "base/log.h"
#include "bach/common/numeric/mx.h"
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/ip/chip/core/rv_core/rv_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 任务配置寄存器 0x0000~0x03FF。
constexpr uint64_t kMuTaskTrigger    = 0x000;  // WO，写即把当前配置锁成一笔任务
constexpr uint64_t kMuPrimitiveDim   = 0x004;  // [15:0] Nblock、[31:16] Kblock
constexpr uint64_t kMuAAddr          = 0x008;  // token/A 基址
constexpr uint64_t kMuAStreamStride  = 0x00C;  // token/A 用户间 stride
constexpr uint64_t kMuCAddr          = 0x010;  // output/C 基址
constexpr uint64_t kMuCStreamStride  = 0x014;  // output/C 用户间 stride
constexpr uint64_t kMuAcExpertStride = 0x018;  // [15:0] token、[31:16] output
constexpr uint64_t kMuBAddr          = 0x01C;  // weight/B 的 Matrix Mem 基址
constexpr uint64_t kMuBExpertStride  = 0x020;  // 专家间 weight stride
constexpr uint64_t kMuPrimitiveMode  = 0x024;  // 任务类型、精度、专家信息、task_last

// 控制与状态寄存器 0x0400~0x07FF。
constexpr uint64_t kMuCtrl        = 0x400;
constexpr uint64_t kMuStatus      = 0x404;
constexpr uint64_t kMuTaskqStatus = 0x408;
constexpr uint64_t kMuErrInfo     = 0x40C;

// MU_STATUS 位域。软件轮询它等一笔任务做完；用 task_last 的那一档不再轮询，
// 由 dsa_done 报 TS。
constexpr uint64_t kMuBusy = 1u << 0;

// 物理阵列：32 个站点，每站 2 个 lane，一次出 64 或 128 个结果。
constexpr uint64_t kMuArrayK = 128;
constexpr uint64_t kMuArrayN = 64;

// primitive_mode：type[0]、A[1]、C[2]、共享精度[4:3]、路由精度[6:5]、
// 共享槽数[9:7]、共享使能[10]、共享mask[18:11]、路由数[23:19]、
// 路由使能[24]、reduce[25]、task_last[26]。使能时计数编码为数量减一。
constexpr uint64_t kMuPrimTypeShift = 0;             // [0] 0: 1×K128×N64、1: 1×K64×N128
constexpr uint64_t kMuADataTypeShift = 1;            // [1] token：0 BF16、1 MXFP8
constexpr uint64_t kMuCDataTypeShift = 2;            // [2] out：0 FP32、1 BF16
constexpr uint64_t kMuRouterEpDtypeShift = 5;        // [6:5] weight：0 BF16、1 MXFP8、2 MXFP4、3 NVFP4
constexpr uint64_t kMuRouterExpertCountShift = 19;   // [23:19] 路由专家数减一
constexpr uint64_t kMuRouterEpReduceEn = 1u << 25;
constexpr uint64_t kMuTaskLast = 1u << 26;

// primitive_dim 位域：[15:0] Nblock、[31:16] Kblock。
constexpr uint64_t kMuNblockShift = 0;
constexpr uint64_t kMuKblockShift = 16;

// AC_expert_stride 位域：[15:0] token_expert_stride、[31:16] output_expert_stride。
constexpr uint64_t kMuTokenExpertStrideShift = 0;
constexpr uint64_t kMuOutputExpertStrideShift = 16;

// TASK_TRIGGER 位域：[0] Temp Valid、[2:1] Temp Index（模板 0~3）。
constexpr uint64_t kMuTriggerTemplateValid = 1u << 0;
constexpr uint64_t kMuTriggerDefault = 0;
constexpr uint64_t kMuShareEpDtypeShift = 3;
constexpr uint64_t kMuShareEpCountShift = 7;
constexpr uint64_t kMuShareEpEn = 1u << 10;
constexpr uint64_t kMuShareEpMaskShift = 11;
constexpr uint64_t kMuRouterEpEn = 1u << 24;

// 一次任务的完整配置。写 TASK_TRIGGER 那一刻锁存成这一份。
struct MuExpert {
  numeric::DataType dtype = numeric::DataType::kBf16;
  uint64_t weight_base = 0, kblock = 0;
  float coefficient = 1.0f;
  bool router = false;
};
struct MuTaskCfg {
  std::array<uint32_t, 9> words{};
  bool decoded_mode = false, router_ep_en = false, share_ep_en = false;
  uint64_t share_slot_count = 0, share_mask = 0, router_limit = 0;
  numeric::DataType share_dtype = numeric::DataType::kBf16;
  numeric::DataType router_dtype = numeric::DataType::kBf16;
  bool numeric_error_en = false;
  std::vector<MuExpert> experts;
  // primitive_mode 位域。
  uint64_t primitive_type = 0;   // 0 = 1×K128×N64、1 = 1×K64×N128
  numeric::DataType a_dtype = numeric::DataType::kBf16;  // token
  numeric::DataType b_dtype = numeric::DataType::kBf16;  // router weight
  bool out_bf16 = false;         // C_data_type：0 FP32、1 BF16
  uint64_t expert_count = 0;     // router_expert_count；0 表示与专家无关（FC0）
  bool ep_reduce = false;        // router_ep_reduce_en：FC2 置 1，FC1/FC3 置 0
  bool task_last = false;        // 任务包里最后一笔，完成后要反馈 TS

  // primitive_dim：[31:16] Kblock、[15:0] Nblock。
  uint64_t kblock = 1;
  uint64_t nblock = 1;

  // 地址。token 与结果在 Core Mem，硬件按 base + stream_id × stride 算；
  // weight 在 Matrix Mem，无 stream 偏移，专家间按 B_expert_stride 隔。
  uint64_t a_addr = 0;
  uint64_t a_stream_stride = 0;
  uint64_t c_addr = 0;
  uint64_t c_stream_stride = 0;
  uint64_t b_addr = 0;
  uint64_t b_expert_stride = 0;
  // AC_expert_stride 拆两侧：token_expert_stride 用于 FC2，output_expert_stride
  // 用于 FC1/FC3。
  uint64_t token_expert_stride = 0;
  uint64_t output_expert_stride = 0;

  // 身份：写 TASK_TRIGGER 那一拍从 RV core CSR 直连采样，不由软件写。
  uint64_t stream_id = 0;
  uint64_t task_id = 0;
  uint64_t user_id = 0;

  // 这一笔任务配置过的寄存器数：从上一笔 trigger 到写 TASK_TRIGGER 之间，软件写
  // 任务配置寄存器（0x004~0x024）的次数。写 trigger 那一刻锁存，供波形带出去。
  uint64_t cfg_count = 0;

  // 一条原语的 K 与 N，由 primitive_type 从物理阵列折出。
  uint64_t PrimK() const {
    uint64_t k = b_dtype == numeric::DataType::kBf16 ? 64 :
        numeric::ElemBitsOf(b_dtype) == 4 && a_dtype == numeric::DataType::kMxfp8 ? 256 : 128;
    return primitive_type == 1 ? k / 2 : k;
  }
  uint64_t PrimN() const { return primitive_type == 1 ? kMuArrayN * 2 : kMuArrayN; }
  // 这一笔要走几个专家。不带 topK 的那一档按一个走。
  uint64_t Experts() const { return decoded_mode ? expert_count : expert_count == 0 ? 1 : expert_count; }
  MuTaskCfg ExpertCfg(uint64_t index) const {
    MuTaskCfg result = *this;
    if (index < experts.size()) {
      result.b_dtype = experts[index].dtype;
      result.b_addr = experts[index].weight_base;
      result.kblock = experts[index].kblock;
    }
    return result;
  }
  uint64_t Tiles() const {
    if (experts.empty()) return kblock * Experts() * nblock;
    uint64_t blocks = 0;
    for (auto const &expert : experts) blocks += expert.kblock;
    return blocks * nblock;
  }
};

class MuRegfile : public BachModule {
 public:
  MuRegfile(ClockPtr clock, const std::string& name, uint64_t parent = 0,
            bool tick = true)
      : BachModule(clock, name, parent, tick),
        cfg_port(std::make_shared<DsaCfgPort>(clock)),
        rdata(std::make_shared<DsaRdataPort>(clock)),
        triggers(clock) { live = Decode(raw); }
  void BindQueueRoom(std::function<bool()> room) { queue_room = std::move(room); }
  void BindAccessError(std::function<void(uint64_t, bool)> error) { access_error = std::move(error); }
  void BlockTrigger(bool on) { block_trigger = on; }
  bool Reserved() const { return pending; }
  void CancelPending() { pending = pending_committed = false; dirty = cfg_write_cnt = 0; }
  void BindControl(std::function<uint64_t(uint64_t)> read,
                   std::function<void(uint64_t, uint64_t)> write) {
    control_read = std::move(read); control_write = std::move(write);
  }
  uint64_t ConfigRead(uint64_t offset) const { return ReadReg(offset); }

  // boot 期 ctrl_noc 灌配置走这一条。与 RV core 的 dsa_iss 那一条分开：两条通路
  // 同时往一个端口上写就是两个写者，而它们本来就不在同一段时间里用。
  void CfgWrite(uint64_t offset, uint64_t data) { WriteReg(offset, data); }

  DsaCfgPort& CfgPort() { return *cfg_port; }
  std::shared_ptr<DsaCfgPort> CfgPortPtr() const { return cfg_port; }
  void AttachCfg(std::shared_ptr<DsaCfgPort> p) { cfg_port = std::move(p); }
  // 读寄存器隔几拍才回，返回数据走独立的一根线。
  std::shared_ptr<DsaRdataPort> RdataPtr() const { return rdata; }
  // 忙不忙由装配层每拍写进来：这一级看不到 issue_q 与执行通路。
  void SetBusy(bool on) { busy = on; }

  // 写 trigger 的那一拍会把当前寄存器内容锁成一笔任务，放这里等 issue_q 取。
  bool HasPending() const { return pending && pending_committed; }
  MuTaskCfg TakePending() {
    pending = false; pending_committed = false;
    return latched;
  }

  MuTaskCfg const& Live() const { return live; }
  uint64_t Triggers() const { return triggers.Get(); }
  // 收下的那一笔的身份，Core 层发 dsa_task_trigger 用。笔数 Triggers() 已经有 ——
  // 每次写 trigger 锁成一笔就自增一次，反压（DriveReady(!pending)）保证不会丢。
  uint64_t AcceptedTask() const { return latched.task_id; }
  uint64_t AcceptedUser() const { return latched.user_id; }

  bool Quiescent() const override { return !pending; }

 protected:
  void Step() override {
    rdata_used = false;
    if (pending && CycleNow() >= pending_at) pending_committed = true;
    ReturnRead();
    // 配置通路一律收得下：反压只在 issue_q 满时由外面拉低。
    bool trigger = cfg_port->req_we.Get() && cfg_port->req_addr.Get() == kMuTaskTrigger;
    bool ready = !trigger || (!block_trigger && !pending && (!queue_room || queue_room()));
    if (cfg_port->Valid() && ready) Write();
    cfg_port->DriveReady(!trigger || (!block_trigger && !pending && (!queue_room || queue_room())));
    if (!rdata_used) rdata->Idle();
    triggers = trigger_pending;
    TracePerCycle("triggers", trigger_pending);
  }

 private:
  void Write() {
    uint64_t addr = cfg_port->req_addr.Get();
    uint64_t v = cfg_port->req_wdata.Get();
    // 同一笔会连着两拍出现在端口上，按序号认它。不能按 (addr, data) 认：写
    // TASK_TRIGGER 是写一次启动一次，连着写两个相同的值是两笔。
    if (cfg_port->Seq() == last_seq) return;
    last_seq = cfg_port->Seq();
    if (cfg_port->req_we.Get() == 0) {
      pending_read.push_back({CycleNow() + kDsaReadLatency, ReadReg(addr),
                              last_seq});
      return;
    }
    WriteReg(addr, v, true);
  }

  void ReturnRead() {
    if (pending_read.empty()) return;
    ReadBack const& r = pending_read.front();
    if (r.at > CycleNow()) return;
    rdata->Drive(r.value, r.seq);
    rdata_used = true;
    pending_read.pop_front();
  }

  using Words = std::array<uint32_t, 9>;
  static MuTaskCfg Decode(Words const &words) {
    MuTaskCfg cfg;
    cfg.words = words;
    cfg.decoded_mode = true;
    cfg.nblock = words[0] & 0xffffu; cfg.kblock = words[0] >> 16;
    cfg.a_addr = words[1]; cfg.a_stream_stride = words[2];
    cfg.c_addr = words[3]; cfg.c_stream_stride = words[4];
    cfg.token_expert_stride = words[5] & 0xffffu;
    cfg.output_expert_stride = words[5] >> 16;
    cfg.b_addr = words[6]; cfg.b_expert_stride = words[7];
    uint32_t mode = words[8];
    cfg.primitive_type = mode & 1;
    cfg.a_dtype = (mode & 2) ? numeric::DataType::kMxfp8 : numeric::DataType::kBf16;
    cfg.out_bf16 = (mode & 4) != 0;
    cfg.share_dtype = numeric::DataType((mode >> 3) & 3);
    cfg.router_dtype = numeric::DataType((mode >> 5) & 3);
    cfg.share_ep_en = (mode & kMuShareEpEn) != 0;
    cfg.share_slot_count = cfg.share_ep_en ? ((mode >> 7) & 7) + 1 : 0;
    cfg.share_mask = (mode >> 11) & ((1u << cfg.share_slot_count) - 1);
    cfg.router_ep_en = (mode & kMuRouterEpEn) != 0;
    cfg.router_limit = cfg.router_ep_en ? ((mode >> 19) & 31) + 1 : 0;
    cfg.expert_count = __builtin_popcountll(cfg.share_mask);
    cfg.b_dtype = cfg.expert_count ? cfg.share_dtype : cfg.router_dtype;
    cfg.ep_reduce = (mode & kMuRouterEpReduceEn) != 0;
    cfg.task_last = (mode & kMuTaskLast) != 0;
    return cfg;
  }
  uint64_t ReadReg(uint64_t addr) const {
    if (!LegalAddress(addr)) return 0;
    if (addr == kMuTaskTrigger) return trigger_value;
    if (addr >= 4 && addr <= kMuPrimitiveMode && !(addr & 3)) return raw[addr / 4 - 1];
    if (addr >= 0x1000 && addr < 0x1200) {
      uint64_t index = (addr - 0x1000) / 0x80, offset = addr % 0x80;
      return offset < 36 && !(offset & 3) ? templates[index][offset / 4] : 0;
    }
    if (control_read && addr >= 0x400) return control_read(addr);
    return addr == kMuStatus && busy ? kMuBusy : 0;
  }
  bool LegalAddress(uint64_t addr) const {
    bool legal = !(addr & 3) && (addr < 0x1200 ||
        (addr >= 0x2000 && addr < 0x2800) || (addr >= 0x3000 && addr < 0x4000));
    if (!legal && access_error) access_error(addr, (addr & 3) != 0);
    return legal;
  }
  void WriteReg(uint64_t addr, uint64_t value, bool rv = false) {
    if (!LegalAddress(addr)) return;
    if (addr >= 4 && addr <= kMuPrimitiveMode) {
      uint64_t index = addr / 4 - 1;
      raw[index] = uint32_t(value) & (index == 8 ? 0x07ffffffu : 0xffffffffu);
      dirty |= 1u << index;
      ++cfg_write_cnt;
      live = Decode(raw);
      return;
    }
    if (addr >= 0x1000 && addr < 0x1200) {
      uint64_t index = (addr - 0x1000) / 0x80, offset = addr % 0x80;
      if (offset < 36) templates[index][offset / 4] = uint32_t(value) &
          (offset == 32 ? 0x07ffffffu : 0xffffffffu);
      return;
    }
    if (addr != kMuTaskTrigger) {
      if (control_write && addr >= 0x400) control_write(addr, value);
      return;
    }
    if (block_trigger || pending || (queue_room && !queue_room())) return;
    trigger_value = value & 7;
    Words snapshot = raw;
    if (value & kMuTriggerTemplateValid) {
      auto const &base = templates[(value >> 1) & 3];
      for (uint64_t i = 0; i < 9; ++i) if (!(dirty & (1u << i))) snapshot[i] = base[i];
    }
    latched = Decode(snapshot);
    auto ids = cfg_port->TaskIds();
    if (!rv && control_read) {
      uint64_t issue = control_read(0x410);
      ids.stream = issue & 15; ids.task = (issue >> 4) & 63; ids.user = (issue >> 10) & 65535;
    }
    latched.stream_id = ids.stream; latched.task_id = ids.task; latched.user_id = ids.user;
    latched.numeric_error_en = control_read && (control_read(kMuErrInfo) & (1u << 12));
    latched.cfg_count = cfg_write_cnt;
    pending = true; pending_committed = false; pending_at = CycleNow() + 1;
    dirty = cfg_write_cnt = 0;
    ++trigger_pending;
  }
  Words raw{};
  std::array<Words, 4> templates{};
  uint64_t dirty = 0, pending_at = 0, trigger_value = 0;
  bool block_trigger = false;
  std::function<bool()> queue_room;
  std::function<void(uint64_t, bool)> access_error;
  std::function<uint64_t(uint64_t)> control_read;
  std::function<void(uint64_t, uint64_t)> control_write;

  struct ReadBack {
    uint64_t at = 0, value = 0, seq = 0;
  };

  std::shared_ptr<DsaCfgPort> cfg_port;
  std::shared_ptr<DsaRdataPort> rdata;
  std::deque<ReadBack> pending_read;
  bool rdata_used = false, busy = false;
  MuTaskCfg live, latched;
  bool pending = false, pending_committed = false;
  uint64_t last_seq = 0;
  uint64_t trigger_pending = 0;
  // 从上一笔 trigger 到这一拍，任务配置寄存器（0x004~0x024）被写了几次。写 trigger
  // 那一刻锁进 latched.cfg_count 再清零，好让下一笔任务重新数。
  uint64_t cfg_write_cnt = 0;

  Logic64 triggers;
};

}  // namespace bach
}  // namespace latch

#endif
