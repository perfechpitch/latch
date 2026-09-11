#ifndef _LATCH_BACH_IP_CHIP_CORE_MU_REGFILE_
#define _LATCH_BACH_IP_CHIP_CORE_MU_REGFILE_

// MU 的配置寄存器。
//
// 分静态与动态两档：静态配置基本不随用户变化，初始化阶段配好、业务流阶段快速
// 调用；动态配置随用户变化，跟随任务下发，含静态配置的选择。
//
// streamID / taskID / userID 由软件写进动态配置寄存器，不来自硬件通路：MU RV
// core 从自定义 CSR 读出 TS 下发的这三个值，在写 trigger 之前配给 MU。
// dsa_done 回给 TS 的 stream_id 与 task_id 就是寄存器里的这一组。
//
// 寄存器偏移照《Matrix Unit DSA》§Register Map Overview，位域照各寄存器那张
// 位域表。地址映射本轮按那一份，与 compiler/hwconfig/layout.py 同源。

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

// 偏移照《Matrix Unit DSA》。
constexpr uint64_t kMuSysCtrl = 0x000;
constexpr uint64_t kMuSysStatus = 0x004;
constexpr uint64_t kMuTaskCfg = 0x008;
constexpr uint64_t kMuTaskBlock = 0x00C;
constexpr uint64_t kMuAddrToken = 0x010;
constexpr uint64_t kMuAddrWeight = 0x014;
constexpr uint64_t kMuAddrScale = 0x018;
constexpr uint64_t kMuAddrOut = 0x01C;
constexpr uint64_t kMuExceptStatus = 0x020;
constexpr uint64_t kMuExceptMask = 0x024;
// 这三个原文没有：软件要把 TS 下发的身份配进来，dsa_done 才填得出。
// 《MU/DTE 寄存器配置参数》改名后只剩 DTE 那一半，MU 侧的地址映射仍无着落，
// 这几个按建模计划的临时映射排在异常那一组之后。
constexpr uint64_t kMuStreamId = 0x050;
constexpr uint64_t kMuTaskId = 0x054;
constexpr uint64_t kMuUserId = 0x058;
constexpr uint64_t kMuTopkStride = 0x05C;
constexpr uint64_t kMuTopkAddr = 0x048;
// 多专家那一组。原文给的是名字（AC_expert_stride、B_expert_stride、
// primitive_mode 里的 router_expert_count 与 router_ep_reduce_en），地址同样
// 无着落，按同一条临时映射往后排。
constexpr uint64_t kMuAcExpertStride = 0x060;
constexpr uint64_t kMuBExpertStride = 0x064;
constexpr uint64_t kMuEpCtrl = 0x068;

// EP_CTRL 位域：[7:0] router_expert_count、[8] router_ep_reduce_en。
constexpr uint64_t kMuEpReduceEn = 1u << 8;

// SYS_CTRL 位域。
constexpr uint64_t kMuTaskStart = 1u << 0;

// SYS_STATUS 位域。软件轮询它等一笔任务做完：一个 task 里发几笔时，TS 那边只
// 等一次完成，收尾由 RV core 报。
constexpr uint64_t kMuBusy = 1u << 0;

// 一次任务的完整配置。写 trigger 那一刻锁存成这一份。
struct MuTaskCfg {
  // TASK_CFG：[0] PRIM_TYPE、[2:1] VLANE_MODE、[4:3] DTYPE_AB、[5] DTYPE_C
  bool prim_k128_n64 = false;   // 0 = 1×K256×N32，1 = 1×K128×N64
  uint64_t vlane = 1;           // 1 或 2
  numeric::DataType dtype_ab = numeric::DataType::kBf16;
  bool out_bf16 = false;        // 0 = FP32 全精度写回，1 = BF16 原位舍入截断

  // TASK_BLOCK：[15:0] KBLOCK、[31:16] NBLOCK
  uint64_t kblock = 1;
  uint64_t nblock = 1;

  uint64_t addr_token = 0;
  uint64_t addr_weight = 0;
  uint64_t addr_scale = 0;
  uint64_t addr_out = 0;

  // ── 多专家 ──
  //
  // 本次任务要算 topK 里的前几个专家。0 表示这一笔与专家无关（FC0 那一档），
  // 按单专家走。
  uint64_t expert_count = 0;
  // 使能专家间 reduce：FC2 置 1，几个专家的结果按 topK 权重合并成一份；
  // FC1 与 FC3 置 0，每个专家各输出一份。
  bool ep_reduce = false;
  // 激活与结果这一侧每个专家隔多远。ep_reduce 为 1 时算在读的地址上（每个
  // 专家一份激活），为 0 时算在写的地址上（每个专家一份结果）。
  uint64_t ac_expert_stride = 0;
  // 权重这一侧每个专家隔多远。
  uint64_t b_expert_stride = 0;

  uint64_t stream_id = 0;
  uint64_t task_id = 0;
  uint64_t user_id = 0;
  // topK 表在 Core Mem 里的位置。MU 在任务启动时按 topk_addr + stream_id ×
  // topk_stride 把这一份读进来。
  uint64_t topk_addr = 0;
  uint64_t topk_stride = 0;

  // 物理矩阵原语的 K 与 N。
  uint64_t PrimK() const { return prim_k128_n64 ? 128 : 256; }
  uint64_t PrimN() const { return prim_k128_n64 ? 64 : 32; }
  // 这一笔要走几个专家。不带 topK 的那一档按一个走。
  uint64_t Experts() const { return expert_count == 0 ? 1 : expert_count; }
};

class MuRegfile : public BachModule {
 public:
  MuRegfile(ClockPtr clock, const std::string& name, uint64_t parent = 0,
            bool tick = true)
      : BachModule(clock, name, parent, tick),
        cfg_port(std::make_shared<DsaCfgPort>(clock)),
        rdata(std::make_shared<DsaRdataPort>(clock)),
        triggers(clock) {}

  // boot 期 ctrl_noc 灌配置走这一条。与 RV core 的 dsa_iss 那一条分开：
  // 两条通路同时往一个端口上写就是两个写者，而它们本来就不在同一段时间里用
  // （ctrl_noc 是 boot 期，dsa_iss 是业务期）。
  void CfgWrite(uint64_t offset, uint64_t data) { WriteReg(offset, data); }

  DsaCfgPort& CfgPort() { return *cfg_port; }
  std::shared_ptr<DsaCfgPort> CfgPortPtr() const { return cfg_port; }
  void AttachCfg(std::shared_ptr<DsaCfgPort> p) { cfg_port = std::move(p); }
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
    // SYS_CTRL 的 TASK_START 位是写一次启动一次，连着写两个相同的值是两笔。
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
      case kMuSysStatus: return busy ? kMuBusy : 0;
      case kMuTaskBlock: return live.kblock | (live.nblock << 16);
      case kMuAddrToken: return live.addr_token;
      case kMuAddrWeight: return live.addr_weight;
      case kMuAddrOut: return live.addr_out;
      case kMuStreamId: return live.stream_id;
      case kMuTaskId: return live.task_id;
      case kMuUserId: return live.user_id;
      default: return 0;
    }
  }

  void WriteReg(uint64_t addr, uint64_t v) {
    switch (addr) {
      case kMuTaskCfg:
        live.prim_k128_n64 = (v & 1u) != 0;
        live.vlane = ((v >> 1) & 0x3u) == 0 ? 1 : 2;
        live.dtype_ab = DecodeDtype((v >> 3) & 0x3u);
        live.out_bf16 = ((v >> 5) & 1u) != 0;
        break;
      case kMuTaskBlock:
        live.kblock = v & 0xFFFFu;
        live.nblock = (v >> 16) & 0xFFFFu;
        break;
      case kMuAddrToken: live.addr_token = v; break;
      case kMuAddrWeight: live.addr_weight = v; break;
      case kMuAddrScale: live.addr_scale = v; break;
      case kMuAddrOut: live.addr_out = v; break;
      case kMuStreamId: live.stream_id = v; break;
      case kMuTaskId: live.task_id = v; break;
      case kMuUserId: live.user_id = v; break;
      case kMuTopkStride: live.topk_stride = v; break;
      case kMuTopkAddr: live.topk_addr = v; break;
      case kMuAcExpertStride: live.ac_expert_stride = v; break;
      case kMuBExpertStride: live.b_expert_stride = v; break;
      case kMuEpCtrl:
        live.expert_count = v & 0xFFu;
        live.ep_reduce = (v & kMuEpReduceEn) != 0;
        break;
      case kMuSysCtrl:
        // 写 1 启动，硬件接收后自清零。必须最后写。
        if (v & kMuTaskStart) {
          latched = live;
          pending = true;
          ++trigger_pending;
        }
        break;
      default:
        break;
    }
  }

  static numeric::DataType DecodeDtype(uint64_t v) {
    // 00 BF16、01 MXFP8、10 MXFP4、11 保留
    if (v == 1) return numeric::DataType::kMxfp8;
    if (v == 2) return numeric::DataType::kMxfp4;
    return numeric::DataType::kBf16;
  }

  struct ReadBack {
    uint64_t at = 0, value = 0, seq = 0;
  };

  std::shared_ptr<DsaCfgPort> cfg_port;
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
