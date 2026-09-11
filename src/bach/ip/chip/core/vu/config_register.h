#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_CONFIG_REGISTER_
#define _LATCH_BACH_IP_CHIP_CORE_VU_CONFIG_REGISTER_

// M1 · config_register 写与 trigger。
//
// 8 组静态配置模板加 12 个动态参数寄存器。macro_inst_trigger 是唯一的启动
// 寄存器，写一次执行一次：两次写之间没有其他配置也启动两次，所以不能按
// 「值变了才算一次」去认，得按写事务本身认。
//
// 静态配置的改写规则：目标组正被未完成的宏指令引用时，把这次配置写阻塞在配置
// 通路上（req_ready 拉低），等引用它的宏指令退休后写入生效。引用计数在这里
// 维护：压 ISQ 时加一，M9 报退休时减一。在飞的宏指令因此始终按改写前的配置
// 执行完毕。
//
// 三条配置通路（VU-Core 的 dsa_cfg、Ctrl-NOC 的 cfg、Debug Module）共享同一份
// 寄存器视图、权限一致，流控彼此独立：VU-Core 这一条因静态组被引用而阻塞时，
// 另外两条仍读得出现场。所以三条各有各的 ready，读路不受阻塞影响。

#include <array>
#include <deque>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/ip/chip/core/vu/regfiles.h"
#include "bach/ip/chip/core/vu/vu_ports.h"
#include "bach/ip/chip/core/rv_core/rv_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 三条配置通路。
enum class VuCfgPath : uint32_t {
  kCore = 0,
  kCtrlNoc = 1,
  kDebug = 2,
};
constexpr uint64_t kVuCfgPathNum = 3;

class VuConfigRegister : public BachModule {
 public:
  VuConfigRegister(ClockPtr clock, const std::string& name, VuRegfiles& rf,
                   uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        regs(rf),
        out(std::make_shared<VuInstPort>(clock)),
        rdata(std::make_shared<DsaRdataPort>(clock)),
        triggers(clock),
        blocked(clock) {
    for (uint64_t i = 0; i < kVuCfgPathNum; ++i) {
      path[i] = std::make_shared<DsaCfgPort>(clock);
    }

  }

  DsaCfgPort& Path(VuCfgPath p) { return *path[uint64_t(p)]; }
  std::shared_ptr<DsaCfgPort> PathPtr(VuCfgPath p) const {
    return path[uint64_t(p)];
  }
  void AttachPath(VuCfgPath p, std::shared_ptr<DsaCfgPort> q) {
    path[uint64_t(p)] = std::move(q);
  }
  VuInstPort& Out() { return *out; }
  void AttachOut(std::shared_ptr<VuInstPort> p) { out = std::move(p); }

  // ── ISQ 与 M9 用这两个报引用的增减 ──
  void HoldCfg(uint64_t idx) {
    if (idx < kVuCfgGroups) ++ref[idx];
  }
  void ReleaseCfg(uint64_t idx) {
    if (idx < kVuCfgGroups && ref[idx] > 0) --ref[idx];
  }

  // VU-Core CSR 里的 stream_id 与 task_id：TS 下发给这个核的那一组，从 VU RV
  // core 直连过来，每拍有效。STREAM_ID_OVERRIDE 只改 stream_id。
  void AttachIds(std::shared_ptr<DsaIdsPort> p) { ids = std::move(p); }
  // 单模块测试里没接直连线时用这个直接给。
  void SetCoreIds(uint64_t stream, uint64_t task) {
    fixed_stream = stream;
    fixed_task = task;
    has_fixed = true;
  }

  // boot 期把一组静态配置直接写进来，不走三条配置通路。装配层照 Router 的
  // Preload 那一套用它：那几笔在业务开始之前就写完了，不占运行时的通路。
  void Preload(uint64_t addr, uint64_t data) { Write(addr, data); }

  // ── 状态区由 ISQ 与 M9 维护，软件写无效不报错（F10）──
  void SetStatus(uint64_t v) { status = v; }
  void SetMacroInstLeft(uint64_t v) { macro_inst_left = v; }
  void RaiseError(uint64_t bits) { error_code |= bits; }
  std::shared_ptr<DsaRdataPort> RdataPtr() const { return rdata; }
  // 在飞的宏指令条数，装配层每拍写进来。
  void SetLeft(uint64_t n) { left = n; }

  VuStaticCfg const& StaticCfg(uint64_t idx) const {
    return static_cfg[idx < kVuCfgGroups ? idx : 0];
  }
  VuDynParam const& Dyn() const { return dyn; }
  uint64_t Triggers() const { return trigger_cnt; }
  uint64_t BlockedWrites() const { return blocked_cnt; }
  // 配置通路上生效的寄存器写次数。
  uint64_t Writes() const { return write_cnt; }
  uint64_t ErrorCode() const { return error_code; }
  uint64_t Status() const { return status; }
  uint64_t MacroInstLeft() const { return macro_inst_left; }
  uint64_t ProfileCtrl() const { return profile_ctrl; }
  // 读 error_code 时其全部异常位清零并同时清 status.ERROR_FLAG。
  uint64_t TakeErrorCode() {
    uint64_t v = error_code;
    error_code = 0;
    status &= ~kVuStatusErrorFlag;
    return v;
  }

  bool Quiescent() const override { return !pending; }

 protected:
  void Step() override {
    rdata_used = false;
    ReturnRead();
    // 末级先做：先看下游收没收走上一条，再收这一拍的配置写。
    Drain();
    for (uint64_t i = 0; i < kVuCfgPathNum; ++i) Serve(i);
    if (!rdata_used) rdata->Idle();
    // 端口的驱动集中在这里。Serve 里写 trigger 的那一拍才刚把 pending 立起来，
    // 若在 Drain 那一步驱动，这一条就一拍也没出现在端口上，下一拍的 Drain 又会
    // 把它当成「已发出且被收走」清掉，宏指令会静默丢掉。
    Publish();

    triggers = trigger_cnt;
    blocked = blocked_cnt;
    TracePerCycle("pending", pending ? 1 : 0);
  }

 private:
  uint64_t CoreStreamId() const {
    if (has_fixed) return fixed_stream;
    return ids ? ids->Stream() : 0;
  }
  uint64_t CoreTaskId() const {
    if (has_fixed) return fixed_task;
    return ids ? ids->Task() : 0;
  }

  void Drain() {
    if (pending && driving && out->Ready()) {
      pending = false;
      driving = false;
    }
  }

  void Publish() {
    if (pending) {
      out->Drive(held, out_seq);
      driving = true;
      return;
    }
    out->Idle();
  }

  void Serve(uint64_t i) {
    DsaCfgPort& p = *path[i];
    if (!p.Valid()) {
      // 没有请求时 ready 照给：ready 是接收方上一拍锁存的值，一直拉高才不会
      // 让请求方白等一拍。
      p.DriveReady(!pending);
      return;
    }
    uint64_t addr = p.req_addr.Get();
    uint64_t v = p.req_wdata.Get();
    bool we = p.req_we.Get() != 0;

    // 目标静态组正被在飞的宏指令引用 → 这次写阻塞在通路上。读不受影响。
    if (we && InStatic(addr) && ref[GroupOf(addr)] > 0) {
      p.DriveReady(false);
      ++blocked_cnt;
      return;
    }
    // 上一条宏指令还没被 ISQ 收走时不能再锁一条：trigger 会覆盖 held。
    if (we && addr == kVuMacroInstTrigger && pending) {
      p.DriveReady(false);
      return;
    }
    p.DriveReady(true);

    // 同一笔写会连着两拍出现在端口上，按序号认它。不能按 (addr, data) 认：
    // macro_inst_trigger 是写一次执行一次，两次写之间没有其他配置也启动两次，
    // 那两笔的地址与数据完全一样。
    if (p.Seq() == last_seq[i]) return;
    last_seq[i] = p.Seq();
    if (we) {
      Write(addr, v);
      ++write_cnt;
      return;
    }
    pending_read.push_back({CycleNow() + kDsaReadLatency, ReadReg(addr),
                            p.Seq()});
  }

  void ReturnRead() {
    if (pending_read.empty()) return;
    ReadBack const& r = pending_read.front();
    if (r.at > CycleNow()) return;
    rdata->Drive(r.value, r.seq);
    rdata_used = true;
    pending_read.pop_front();
  }

  // 软件读得到的几项。macro_inst_left 是在飞的宏指令还剩几条：一个 task 发了
  // 几条时，软件轮询它到 0 再通知 TS。
  uint64_t ReadReg(uint64_t addr) const {
    switch (addr) {
      case kVuMacroInstLeft: return left;
      case kVuErrorCode: return error_code;
      default: return 0;
    }
  }

  static bool InStatic(uint64_t addr) {
    return addr >= kVuStaticBase &&
           addr < kVuStaticBase + kVuCfgGroups * kVuStaticStride;
  }
  static uint64_t GroupOf(uint64_t addr) {
    return (addr - kVuStaticBase) / kVuStaticStride;
  }

  void Write(uint64_t addr, uint64_t v) {
    if (addr < kVuDynamicEnd) {
      if (addr == kVuMacroInstTrigger) {
        Trigger(v);
        return;
      }
      WriteParam(dyn, addr, v);
      return;
    }
    if (InStatic(addr)) {
      WriteStatic(GroupOf(addr), (addr - kVuStaticBase) % kVuStaticStride, v);
      return;
    }
    if (addr == kVuRegFileAddr) {
      rf_addr = v;
      return;
    }
    if (addr == kVuRegFileData) {
      regs.WriteBackdoor(rf_addr, v);
      return;
    }
    if (addr == kVuProfileCtrl) {
      profile_ctrl = v;
      return;
    }
    if (addr >= kVuStatusBase && addr < kVuProfileBase) {
      // macro_inst_left、status 与 error_code 由硬件维护，软件写入无效、不报错。
      return;
    }
    if (addr >= kVuProfileCnt && addr <= kVuProfileBase + 0xEC) {
      // Profile 计数器同样只能由硬件写，清零走 profile_ctrl.CLEAR。
      return;
    }
    error_code |= kVuErrUnimplReg;
  }

  // 12 个动态参数寄存器与 11 个静态副本共用这一段：静态副本与动态版本逐位相同，
  // 组内偏移 = 对应动态地址 + 0x2C。
  static void WriteParam(VuDynParam& p, uint64_t addr, uint64_t v) {
    switch (addr) {
      case kVuTypeVl: p.type_vl = uint32_t(v); break;
      case kVuLdAddr: p.ld_addr = uint32_t(v); break;
      case kVuStAddr: p.st_addr = uint32_t(v); break;
      case kVuVrfRdIndex: p.vrf_rd_index = uint32_t(v); break;
      case kVuVrfWtIndex: p.vrf_wt_index = uint32_t(v); break;
      case kVuMrfRdIndex: p.mrf_rd_index = uint32_t(v); break;
      case kVuMrfWtIndex: p.mrf_wt_index = uint32_t(v); break;
      case kVuSrfRdIndex0: p.srf_rd_index_0 = uint32_t(v); break;
      case kVuSrfRdIndex1: p.srf_rd_index_1 = uint32_t(v); break;
      case kVuSrfWtIndex0: p.srf_wt_index_0 = uint32_t(v); break;
      case kVuSrfWtIndex1: p.srf_wt_index_1 = uint32_t(v); break;
      default: break;
    }
  }

  // 写 trigger：锁存当前 12 个动态参数为一份快照，与所选静态组的 11 个副本一起
  // 打包。取哪一份逐参数由 STATIC_DYNAMIC_MASK 决定，那件事收在 VuMacroInst 里。
  void Trigger(uint64_t v) {
    dyn.trigger = uint32_t(v);
    auto inst = std::make_shared<VuMacroInst>();
    inst->cfg_idx = (v >> kVuTrigCfgIdxShift) & 0x7u;
    inst->dyn = dyn;
    inst->dup = static_cfg[inst->cfg_idx].dup;
    inst->mask = (v >> kVuTrigMaskShift) & 0xFFu;
    inst->event_en = (v & kVuTrigEventEn) != 0;
    inst->sid_override = (v & kVuTrigSidOverride) != 0;
    inst->fence = (v & kVuTrigFence) != 0;
    inst->broadcast = (v & kVuTrigBroadcast) != 0;
    inst->seq = ++inst_seq;

    // STREAM_ID_OVERRIDE = 0 时沿用 VU-Core CSR 自带的 Stream ID，那一份由
    // VU RV core 经 dsa_ids 直连给进来；= 1 时改用 trigger 里的 STREAM_ID
    // 字段。task_id 不在这个寄存器里，始终取 VU-Core 那一份。
    inst->stream_id = inst->sid_override ? ((v >> kVuTrigSidShift) & 0xFu)
                                         : CoreStreamId();
    inst->task_id = CoreTaskId();

    held = inst;
    pending = true;
    out_seq = inst->seq;
    ++trigger_cnt;
  }

  // 一组静态模板 23 个寄存器：前 12 个是没有动态副本的 *_op / mask_op /
  // PRF_op，后 11 个是动态参数寄存器的静态副本，组内偏移 = 动态地址 + 0x2C。
  void WriteStatic(uint64_t g, uint64_t off, uint64_t v) {
    VuStaticCfg& c = static_cfg[g];
    switch (off) {
      case kVuLuOp: c.lu.Set(v); return;
      case kVuSuOp: c.su.Set(v); return;
      case kVuValu0Op: c.valu[0].Set(v); return;
      case kVuValu1Op: c.valu[1].Set(v); return;
      case kVuValu2Op: c.valu[2].Set(v); return;
      case kVuVsfuOp: c.vsfu.Set(v); return;
      case kVuMexeOp: c.mexe.Set(v); return;
      case kVuSexe0Op: c.sexe[0].Set(v); return;
      case kVuSexe1Op: c.sexe[1].Set(v); return;
      case kVuSexe2Op: c.sexe[2].Set(v); return;
      case kVuMaskOp: c.mask_op = uint32_t(v); return;
      case kVuPrfOp: c.prf_op = uint32_t(v); return;
      default: break;
    }
    if (off >= kVuStaticDupOffset + kVuTypeVl &&
        off < kVuStaticDupOffset + kVuDynamicEnd) {
      WriteParam(c.dup, off - kVuStaticDupOffset, v);
      return;
    }
    error_code |= kVuErrUnimplReg;
  }

  VuRegfiles& regs;
  std::array<std::shared_ptr<DsaCfgPort>, kVuCfgPathNum> path;
  std::shared_ptr<VuInstPort> out;

  std::array<VuStaticCfg, kVuCfgGroups> static_cfg{};
  VuDynParam dyn;
  std::array<uint64_t, kVuCfgGroups> ref{};

  std::shared_ptr<VuMacroInst> held;
  bool pending = false, driving = false;
  uint64_t out_seq = 0, inst_seq = 0;
  uint64_t trigger_cnt = 0, blocked_cnt = 0, write_cnt = 0;
  uint64_t status = 0, macro_inst_left = 0, error_code = 0, profile_ctrl = 0;
  uint64_t rf_addr = 0;
  std::shared_ptr<DsaIdsPort> ids;
  uint64_t fixed_stream = 0, fixed_task = 0;
  bool has_fixed = false;
  std::array<uint64_t, kVuCfgPathNum> last_seq{};

  struct ReadBack {
    uint64_t at = 0, value = 0, seq = 0;
  };
  std::shared_ptr<DsaRdataPort> rdata;
  std::deque<ReadBack> pending_read;
  bool rdata_used = false;
  // 在飞的宏指令条数，由装配层每拍写进来：这一级看不到 ISQ 与执行通路。
  uint64_t left = 0;

  Logic64 triggers, blocked;
};

}  // namespace bach
}  // namespace latch

#endif
