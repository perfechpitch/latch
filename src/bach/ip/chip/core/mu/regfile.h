#ifndef _LATCH_BACH_IP_CHIP_CORE_MU_REGFILE_
#define _LATCH_BACH_IP_CHIP_CORE_MU_REGFILE_

// MU 的配置寄存器。
//
// 分静态与动态两档：静态配置基本不随用户变化，初始化阶段配好、业务流阶段快速
// 调用；动态配置随用户变化，跟随任务下发，含静态配置的选择。
//
// streamID / taskID / userID 不再由软件写寄存器：三个身份信号从 MU RV core 的
// CSR 直连过来（DsaIdsPort），写 TASK_TRIGGER 那一拍采样进任务快照。dsa_done 回
// 给 TS 的 stream_id / task_id 就是这一组，与 DTE、VU 同一套做法。
//
// 寄存器偏移照《Matrix Unit DSA》§Register Map Overview（任务配置 0x0000~0x03FF），
// 位域照各寄存器那张位域表。地址模型：token 与结果的物理地址由硬件按
// base + stream_id × stream_stride 算，软件只写 base 与 stride，不再把 stream
// 偏移乘进绝对地址。

#include <array>
#include <deque>
#include <string>

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
constexpr uint64_t kMuTopkTableAddr  = 0x024;  // 用户 topK 存储地址（留档）
constexpr uint64_t kMuPrimitiveMode  = 0x028;  // 任务类型、精度、专家信息、task_last

// 控制与状态寄存器 0x0400~0x07FF。
constexpr uint64_t kMuCtrl        = 0x400;
constexpr uint64_t kMuStatus      = 0x404;
constexpr uint64_t kMuTaskqStatus = 0x408;
constexpr uint64_t kMuErrInfo     = 0x40C;

// MU_STATUS 位域。软件轮询它等一笔任务做完；用 task_last 的那一档不再轮询，
// 由 dsa_done 报 TS。
constexpr uint64_t kMuBusy = 1u << 0;

// 物理阵列：32 个 lane，每 lane 128 个 MAC，一次出 64 个结果。
constexpr uint64_t kMuArrayK = 128;
constexpr uint64_t kMuArrayN = 64;

// primitive_mode 位域。宽度 25 位：[0] primitive_type、[2:1] A_data_type、
// [3] C_data_type、[6:4] share_ep_data_type、[9:7] router_ep_data_type、
// [13:10] share_ep_count、[21:14] router_expert_count、[22] share_ep_en、
// [23] router_ep_reduce_en、[24] task_last。
constexpr uint64_t kMuPrimTypeShift = 0;             // [0] 0: 1×K128×N64、1: 1×K64×N128
constexpr uint64_t kMuADataTypeShift = 1;            // [2:1] token：0 BF16、1 MXFP8
constexpr uint64_t kMuCDataTypeShift = 3;            // [3] out：0 FP32、1 BF16
constexpr uint64_t kMuRouterEpDtypeShift = 7;        // [9:7] weight：0 BF16、1 MXFP8、2 MXFP4、3 NVFP4
constexpr uint64_t kMuRouterExpertCountShift = 14;   // [21:14] 路由专家数
constexpr uint64_t kMuRouterEpReduceEn = 1u << 23;
constexpr uint64_t kMuTaskLast = 1u << 24;

// primitive_dim 位域：[15:0] Nblock、[31:16] Kblock。
constexpr uint64_t kMuNblockShift = 0;
constexpr uint64_t kMuKblockShift = 16;

// AC_expert_stride 位域：[15:0] token_expert_stride、[31:16] output_expert_stride。
constexpr uint64_t kMuTokenExpertStrideShift = 0;
constexpr uint64_t kMuOutputExpertStrideShift = 16;

// TASK_TRIGGER 位域：[0] Temp Valid、[2:1] Temp Index（模板 0~3）。
constexpr uint64_t kMuTriggerValid = 1u << 0;

// 一次任务的完整配置。写 TASK_TRIGGER 那一刻锁存成这一份。
struct MuTaskCfg {
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

  // topK_table_addr 只留档：topK 实际经 DTE 那条专用数据线进 topK_ep_table，
  // 不参与寻址。
  uint64_t topk_table_addr = 0;

  // 身份：写 TASK_TRIGGER 那一拍从 RV core CSR 直连采样，不由软件写。
  uint64_t stream_id = 0;
  uint64_t task_id = 0;
  uint64_t user_id = 0;

  // 一条原语的 K 与 N，由 primitive_type 从物理阵列折出。
  uint64_t PrimK() const { return primitive_type == 1 ? kMuArrayK / 2 : kMuArrayK; }
  uint64_t PrimN() const { return primitive_type == 1 ? kMuArrayN * 2 : kMuArrayN; }
  // 这一笔要走几个专家。不带 topK 的那一档按一个走。
  uint64_t Experts() const { return expert_count == 0 ? 1 : expert_count; }
};

class MuRegfile : public BachModule {
 public:
  MuRegfile(ClockPtr clock, const std::string& name, uint64_t parent = 0,
            bool tick = true)
      : BachModule(clock, name, parent, tick),
        cfg_port(std::make_shared<DsaCfgPort>(clock)),
        ids(std::make_shared<DsaIdsPort>(clock)),
        rdata(std::make_shared<DsaRdataPort>(clock)),
        triggers(clock) {}

  // boot 期 ctrl_noc 灌配置走这一条。与 RV core 的 dsa_iss 那一条分开：两条通路
  // 同时往一个端口上写就是两个写者，而它们本来就不在同一段时间里用。
  void CfgWrite(uint64_t offset, uint64_t data) { WriteReg(offset, data); }

  DsaCfgPort& CfgPort() { return *cfg_port; }
  std::shared_ptr<DsaCfgPort> CfgPortPtr() const { return cfg_port; }
  void AttachCfg(std::shared_ptr<DsaCfgPort> p) { cfg_port = std::move(p); }
  // 身份信号：写 TASK_TRIGGER 那一拍采样进任务快照。
  DsaIdsPort& Ids() { return *ids; }
  void AttachIds(std::shared_ptr<DsaIdsPort> p) { ids = std::move(p); }
  // 读寄存器隔几拍才回，返回数据走独立的一根线。
  std::shared_ptr<DsaRdataPort> RdataPtr() const { return rdata; }
  // 忙不忙由装配层每拍写进来：这一级看不到 issue_q 与执行通路。
  void SetBusy(bool on) { busy = on; }

  // 写 trigger 的那一拍会把当前寄存器内容锁成一笔任务，放这里等 issue_q 取。
  bool HasPending() const { return pending; }
  MuTaskCfg TakePending() {
    pending = false;
    return latched;
  }

  MuTaskCfg const& Live() const { return live; }
  uint64_t Triggers() const { return triggers.Get(); }

  bool Quiescent() const override { return !pending; }

 protected:
  void Step() override {
    rdata_used = false;
    ReturnRead();
    // 配置通路一律收得下：反压只在 issue_q 满时由外面拉低。
    cfg_port->DriveReady(!pending);
    if (cfg_port->Valid() && !pending) Write();
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
    WriteReg(addr, v);
  }

  void ReturnRead() {
    if (pending_read.empty()) return;
    ReadBack const& r = pending_read.front();
    if (r.at > CycleNow()) return;
    rdata->Drive(r.value, r.seq);
    rdata_used = true;
    pending_read.pop_front();
  }

  uint64_t ReadReg(uint64_t addr) const {
    switch (addr) {
      case kMuStatus: return busy ? kMuBusy : 0;
      case kMuPrimitiveDim: return (live.kblock << kMuKblockShift) |
                                   (live.nblock << kMuNblockShift);
      case kMuAAddr: return live.a_addr;
      case kMuCAddr: return live.c_addr;
      case kMuBAddr: return live.b_addr;
      default: return 0;
    }
  }

  void WriteReg(uint64_t addr, uint64_t v) {
    switch (addr) {
      case kMuPrimitiveDim:
        live.kblock = (v >> kMuKblockShift) & 0xFFFFu;
        live.nblock = (v >> kMuNblockShift) & 0xFFFFu;
        break;
      case kMuAAddr: live.a_addr = v; break;
      case kMuAStreamStride: live.a_stream_stride = v; break;
      case kMuCAddr: live.c_addr = v; break;
      case kMuCStreamStride: live.c_stream_stride = v; break;
      case kMuAcExpertStride:
        live.token_expert_stride = (v >> kMuTokenExpertStrideShift) & 0xFFFFu;
        live.output_expert_stride = (v >> kMuOutputExpertStrideShift) & 0xFFFFu;
        break;
      case kMuBAddr: live.b_addr = v; break;
      case kMuBExpertStride: live.b_expert_stride = v; break;
      case kMuTopkTableAddr: live.topk_table_addr = v; break;
      case kMuPrimitiveMode:
        live.primitive_type = (v >> kMuPrimTypeShift) & 1u;
        live.a_dtype = DecodeADtype((v >> kMuADataTypeShift) & 0x3u);
        live.out_bf16 = ((v >> kMuCDataTypeShift) & 1u) != 0;
        live.b_dtype = DecodeBDtype((v >> kMuRouterEpDtypeShift) & 0x7u);
        live.expert_count = (v >> kMuRouterExpertCountShift) & 0xFFu;
        live.ep_reduce = (v & kMuRouterEpReduceEn) != 0;
        live.task_last = (v & kMuTaskLast) != 0;
        break;
      case kMuTaskTrigger:
        // 写 1 启动，硬件接收后自清零。身份信号在这一拍采样，必须最后写。
        if (v & kMuTriggerValid) {
          live.stream_id = ids->Stream();
          live.task_id = ids->Task();
          live.user_id = ids->User();
          latched = live;
          pending = true;
          ++trigger_pending;
        }
        break;
      default:
        break;
    }
  }

  // A_data_type 只有 BF16 / MXFP8 两档有效。
  static numeric::DataType DecodeADtype(uint64_t v) {
    return v == 1 ? numeric::DataType::kMxfp8 : numeric::DataType::kBf16;
  }
  // router_ep_data_type 多两档：0 BF16、1 MXFP8、2 MXFP4、3 NVFP4。
  static numeric::DataType DecodeBDtype(uint64_t v) {
    if (v == 1) return numeric::DataType::kMxfp8;
    if (v == 2) return numeric::DataType::kMxfp4;
    // NVFP4 本轮不建模，回落 BF16。
    return numeric::DataType::kBf16;
  }

  struct ReadBack {
    uint64_t at = 0, value = 0, seq = 0;
  };

  std::shared_ptr<DsaCfgPort> cfg_port;
  std::shared_ptr<DsaIdsPort> ids;
  std::shared_ptr<DsaRdataPort> rdata;
  std::deque<ReadBack> pending_read;
  bool rdata_used = false, busy = false;
  MuTaskCfg live, latched;
  bool pending = false;
  uint64_t last_seq = 0;
  uint64_t trigger_pending = 0;

  Logic64 triggers;
};

}  // namespace bach
}  // namespace latch

#endif
