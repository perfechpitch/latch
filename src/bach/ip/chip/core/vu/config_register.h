#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_CONFIG_REGISTER_
#define _LATCH_BACH_IP_CHIP_CORE_VU_CONFIG_REGISTER_

// M1 · config_register 写与 trigger。
//
// 六个 Block：动态参数 0x0000（12 个）、静态配置组 N×0x100 + 0x1000（8 组各
// 23 个）、全局静态 0x1F00（2 个）、DSA-RF 后门 0x2000（2 个）、状态 0x3000
// （12 个）、Profile 0x4000（65 个）。macro_inst_trigger 是唯一的启动寄存器，
// 写一次执行一次：两次写之间没有其他配置也启动两次，所以不能按「值变了才算
// 一次」去认，得按写事务本身认。
//
// 静态配置的改写规则：目标组正被未完成的宏指令引用时，把这次配置写阻塞在配置
// 通路上（req_ready 拉低），等引用它的宏指令退休后写入生效。引用计数在这里
// 维护：压 ISQ 时加一，M9 报退休时减一。在飞的宏指令因此始终按改写前的配置
// 执行完毕。
//
// 三条配置通路（VU-Core 的 dsa_cfg、Ctrl-NOC 的 cfg、Debug Module）共享同一份
// 寄存器视图、权限一致，流控彼此独立：VU-Core 这一条因静态组被引用而阻塞时，
// 另外两条仍读得出现场。所以三条各有各的 ready，读路不受阻塞影响。
//
// 状态区里 software 写得进去的只有 snapshot_addr 一个：宏指令发射时带下来的
// 那一组动态参数快照按年龄编号摆在那里。其余几个由硬件维护，写访问不报错但也
// 不改变任何东西。
//
// 异常一律经 ReportError 上报：置 error_code 的位，VALID=0 时把首个异常的
// 上下文锁进 error_info 与对应的上下文寄存器。读 error_code 把它们一起清零。

#include <array>
#include <deque>
#include <memory>
#include <set>
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

// 快照窗口的条目来自 ISQ：已发射、尚未退休的宏指令按年龄编号，0 是最老的一条。
// 这个窗口跨两级，所以拆成一个口子由 ISQ 填。
class VuSnapshotWindow {
 public:
  virtual ~VuSnapshotWindow() = default;
  // 在场宏指令条数，也就是有效编号的条数。
  virtual uint64_t SnapCount() const = 0;
  // 该编号上那一条是不是已经派发到执行单元（0 = 还在 ISQ 里排队）。
  virtual bool SnapDispatched(uint64_t age) const = 0;
  virtual uint64_t SnapTag(uint64_t age) const = 0;
  virtual uint64_t SnapParam(uint64_t age, uint64_t idx) const = 0;
};

// Profile 计数器由 VuProfile 维护，读走这一个口子。
class VuCounterWindow {
 public:
  virtual ~VuCounterWindow() = default;
  virtual uint64_t CounterLo(uint64_t rank) const = 0;
  virtual uint64_t CounterHi(uint64_t rank) const = 0;
};

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
  void AttachSnapshotWindow(VuSnapshotWindow* w) { snap_win = w; }
  void AttachCounterWindow(VuCounterWindow* w) { cnt_win = w; }

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
  void SetCoreIds(uint64_t stream, uint64_t task, uint64_t user = 0) {
    fixed_stream = stream;
    fixed_task = task;
    fixed_user = user;
    has_fixed = true;
  }

  // boot 期把一组静态配置直接写进来，不走三条配置通路。装配层照 Router 的
  // Preload 那一套用它：那几笔在业务开始之前就写完了，不占运行时的通路。
  void Preload(uint64_t addr, uint64_t data) { Write(addr, data); }

  // ── 状态区由 ISQ 与 M9 维护，软件写无效不报错（snapshot_addr 除外）──
  void SetStatus(uint64_t v) { status = v; }
  void SetMacroInstLeft(uint64_t v) { macro_inst_left = v; }
  // 被调度阶段拦下的那几条（CFG_ERROR）：它们出了 ISQ 但没进执行单元，快照窗口
  // 里按「仍在排队」算。条数极少，退休时由 ISQ 摘掉。
  void MarkNotDispatched(uint64_t seq) { not_dispatched.insert(seq); }
  void ClearNotDispatched(uint64_t seq) { not_dispatched.erase(seq); }
  bool NotDispatched(uint64_t seq) const {
    return not_dispatched.count(seq) != 0;
  }
  // 替换模式下被换掉的 element 数，M9 每退休一段累进来，Profile 计数器读它。
  void AddReplaces(uint64_t nan, uint64_t inf) {
    nan_replaced += nan;
    inf_replaced += inf;
  }
  uint64_t NanReplaced() const { return nan_replaced; }
  uint64_t InfReplaced() const { return inf_replaced; }
  std::shared_ptr<DsaRdataPort> RdataPtr() const { return rdata; }
  // 在飞的宏指令条数，装配层每拍写进来：这一级看不到 ISQ 与执行通路。
  void SetLeft(uint64_t n) { left = n; }

  // 上报一个异常。unit 填 ERR_UNIT 编码，inst 给出该宏指令的配置上下文：
  // 首个异常（error_info.VALID 还是 0）时把 USER_ID / STREAM_ID / CONFIG_IDX /
  // ERR_UNIT / FIRST_ERR 一起锁下来，并把这条宏指令存成 sticky 快照。
  void ReportError(uint64_t bits, uint64_t unit, VuMacroInst const* inst = nullptr,
                   uint64_t ctx_idx = 0) {
    if (bits == 0) return;
    error_code |= bits;
    // NAN_ERROR 另有一份上下文寄存器，与 error_info 一同锁存、一同清零。
    if ((bits & kVuErrNan) != 0 && (nan_err_info & kVuErrCtxValid) == 0) {
      uint64_t user = inst ? inst->user_id : 0;
      nan_err_info = (user & 0xFFFFu) | kVuErrCtxValid;
    }
    // CM / RF 的 ECC 各有出错位置，模型里没有产生源，接到时照实锁。
    if ((bits & kVuErrVrfEcc) != 0) LatchRfErr(vrf_err_info, ctx_idx, inst);
    if ((bits & kVuErrMrfEcc) != 0) LatchRfErr(mrf_err_info, ctx_idx, inst);
    if ((bits & kVuErrSrfEcc) != 0) LatchRfErr(srf_err_info, ctx_idx, inst);
    if ((bits & kVuErrCmEcc) != 0 && (cm_err_info & kVuErrCtxValid) == 0) {
      uint64_t user = inst ? inst->user_id : 0;
      cm_err_info = (user & 0xFFFFu) | kVuErrCtxValid;
      cm_err_addr = inst ? inst->LdAddr() : 0;
    }

    if ((error_info & kVuErrInfoValid) != 0) return;   // 首错已经锁过
    uint64_t first = 0;
    while (first < 9 && (bits & (1u << first)) == 0) ++first;
    error_info = ((inst ? inst->user_id : 0) & 0xFFFFu) |
                 (((inst ? inst->stream_id : 0) & 0xFu)
                  << kVuErrInfoStreamShift) |
                 (((inst ? inst->cfg_idx : 0) & 0x7u)
                  << kVuErrInfoCfgIdxShift) |
                 ((unit & 0xFu) << kVuErrInfoUnitShift) |
                 ((first & 0xFu) << kVuErrInfoFirstShift) | kVuErrInfoValid;
    sticky = inst ? std::make_shared<VuMacroInst>(*inst) : nullptr;
    sticky_tag = inst ? inst->tag : 0;
    // 派发前的静态合法性检查与配置总线访问拦下的那两条还没派发，其余都已派出。
    sticky_dispatched = (bits & (kVuErrRegAddr | kVuErrCfg)) == 0;
  }
  void RaiseError(uint64_t bits) { ReportError(bits, kVuErrUnitNone); }

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
  uint64_t ErrorInfo() const { return error_info; }
  uint64_t NanErrInfo() const { return nan_err_info; }
  uint64_t SnapshotAddr() const { return snap_addr; }
  uint64_t SnapshotData() const { return SnapshotValue(); }
  // 全局静态寄存器：所有宏指令共享，上电写一次。
  uint64_t InfReplaceValue() const { return inf_replace_value; }
  uint64_t NanReplaceValue() const { return nan_replace_value; }
  // 读 error_code 时其全部异常位清零，同时清 status.ERROR_FLAG、error_info、
  // sticky 快照与全部错误上下文寄存器。
  uint64_t TakeErrorCode() {
    uint64_t v = error_code;
    error_code = 0;
    error_info = 0;
    vrf_err_info = 0;
    mrf_err_info = 0;
    srf_err_info = 0;
    cm_err_info = 0;
    cm_err_addr = 0;
    nan_err_info = 0;
    sticky.reset();
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
  // 用户号也走这根直连线。宏指令本身不带它，写 trigger 那一拍采下来存进指令里，
  // 随这条指令一起进 ISQ、一起退休，波形与 dsa_done 才认得是哪一笔 task 的。
  uint64_t CoreUserId() const {
    if (has_fixed) return fixed_user;
    return ids ? ids->User() : 0;
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

  // 软件读得到的那几项。register 视图是三条通路共享的那一份，读一个未实现的
  // 地址不产生副作用，只置位 error_code.REG_ADDR_ERROR。
  uint64_t ReadReg(uint64_t addr) {
    if (addr < kVuDynamicEnd) return ParamOf(dyn, addr);
    if (InStatic(addr)) {
      return StaticOf(StaticCfg(GroupOf(addr)), (addr - kVuStaticBase) %
                                                     kVuStaticStride);
    }
    switch (addr) {
      case kVuInfReplaceValue: return inf_replace_value;
      case kVuNanReplaceValue: return nan_replace_value;
      case kVuRegFileAddr: return rf_addr;
      case kVuRegFileData: return BackdoorRead();
      case kVuMacroInstLeft: return left;
      case kVuStatus: return status;
      case kVuErrorCode: return TakeErrorCode();
      case kVuErrorInfo: return error_info;
      case kVuSnapshotAddr: return snap_addr;
      case kVuSnapshotData: return SnapshotValue();
      case kVuVrfErrInfo: return vrf_err_info;
      case kVuMrfErrInfo: return mrf_err_info;
      case kVuSrfErrInfo: return srf_err_info;
      case kVuCmErrInfo: return cm_err_info;
      case kVuCmErrAddr: return cm_err_addr;
      case kVuNanErrInfo: return nan_err_info;
      case kVuProfileCtrl: return profile_ctrl;
      default: break;
    }
    if (addr >= kVuProfileCnt && addr <= kVuProfileCnt + kVuCounterNum * 8 - 4) {
      if (!cnt_win) return 0;
      uint64_t rank = (addr - kVuProfileCnt) / 8;
      return ((addr - kVuProfileCnt) % 8 == 0) ? cnt_win->CounterLo(rank)
                                               : cnt_win->CounterHi(rank);
    }
    ReportError(kVuErrRegAddr, kVuErrUnitNone);
    return 0;
  }

  static bool InStatic(uint64_t addr) {
    return addr >= kVuStaticBase &&
           addr < kVuStaticBase + kVuCfgGroups * kVuStaticStride;
  }
  static bool InGlobalStatic(uint64_t addr) {
    return addr == kVuInfReplaceValue || addr == kVuNanReplaceValue;
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
    if (InGlobalStatic(addr)) {
      // 全局静态：所有宏指令共享，不随 CONFIG_IDX 切换，也不参与
      // STATIC_DYNAMIC_MASK。
      if (addr == kVuInfReplaceValue) {
        inf_replace_value = uint32_t(v);
      } else {
        nan_replace_value = uint32_t(v);
      }
      return;
    }
    if (addr == kVuRegFileAddr) {
      rf_addr = v;
      return;
    }
    if (addr == kVuRegFileData) {
      BackdoorWrite(v);
      return;
    }
    if (addr == kVuProfileCtrl) {
      profile_ctrl = v;
      return;
    }
    if (addr == kVuSnapshotAddr) {
      // 状态区里只有这一个软件写进去是生效的：它是个索引，不是硬件维护的值。
      snap_addr = v;
      return;
    }
    if (addr >= kVuStatusBase && addr < kVuStatusEnd) {
      // macro_inst_left、status、error_code、error_info、snapshot_data 与 6 个
      // 错误上下文寄存器由硬件维护，软件写入不生效、也不报错。
      return;
    }
    if (addr >= kVuProfileCnt && addr <= kVuProfileCnt + kVuCounterNum * 8 - 4) {
      // Profile 计数器同样只能由硬件写，清零走 profile_ctrl.CLEAR。0x4004 是
      // 未实现地址，落到下面那一条。
      return;
    }
    ReportError(kVuErrRegAddr, kVuErrUnitNone);
  }

  // reg_file_addr：RF_SEL 在 [17:16] 选 VRF / MRF / SRF 之一，RF_ADDR 在
  // [15:0] 是这个 RF 内的字节地址，低 2 位被忽略。选到没有的 RF（11）或地址
  // 越过该 RF 的容量时置 RF_IDX_ERROR，访问在容量内回绕。
  uint64_t RfOffset() {
    uint64_t sel = (rf_addr >> kVuRfSelShift) & 0x3u;
    uint64_t cap = VuRegfiles::RfBytes(sel);
    if (cap == 0) {
      ReportError(kVuErrRfIndex, kVuErrUnitRfDebug);
      rf_sel_ok = false;
      return 0;
    }
    uint64_t a = rf_addr & 0xFFFFu;
    if (a >= cap) ReportError(kVuErrRfIndex, kVuErrUnitRfDebug);
    rf_sel_ok = true;
    return a;
  }

  uint64_t BackdoorRead() {
    uint64_t a = RfOffset();
    if (!rf_sel_ok) return 0;
    return regs.ReadBackdoor((rf_addr >> kVuRfSelShift) & 0x3u, a);
  }

  void BackdoorWrite(uint64_t v) {
    uint64_t a = RfOffset();
    if (!rf_sel_ok) return;
    regs.WriteBackdoor((rf_addr >> kVuRfSelShift) & 0x3u, a, v);
  }

  // 一个动态参数寄存器或它的静态副本的值。0x0000 是 trigger，读出的是最近
  // 一次写入的值。
  static uint64_t ParamOf(VuDynParam const& p, uint64_t addr) {
    switch (addr) {
      case kVuMacroInstTrigger: return p.trigger;
      case kVuTypeVl: return p.type_vl;
      case kVuLdAddr: return p.ld_addr;
      case kVuStAddr: return p.st_addr;
      case kVuVrfRdIndex: return p.vrf_rd_index;
      case kVuVrfWtIndex: return p.vrf_wt_index;
      case kVuMrfRdIndex: return p.mrf_rd_index;
      case kVuMrfWtIndex: return p.mrf_wt_index;
      case kVuSrfRdIndex0: return p.srf_rd_index_0;
      case kVuSrfRdIndex1: return p.srf_rd_index_1;
      case kVuSrfWtIndex0: return p.srf_wt_index_0;
      case kVuSrfWtIndex1: return p.srf_wt_index_1;
      default: return 0;
    }
  }

  static uint64_t StaticOf(VuStaticCfg const& c, uint64_t off) {
    switch (off) {
      case kVuLuOp: return c.lu.raw;
      case kVuSuOp: return c.su.raw;
      case kVuValu0Op: return c.valu[0].raw;
      case kVuValu1Op: return c.valu[1].raw;
      case kVuValu2Op: return c.valu[2].raw;
      case kVuVsfuOp: return c.vsfu.raw;
      case kVuMexeOp: return c.mexe.raw;
      case kVuSexe0Op: return c.sexe[0].raw;
      case kVuSexe1Op: return c.sexe[1].raw;
      case kVuSexe2Op: return c.sexe[2].raw;
      case kVuMaskOp: return c.mask_op;
      case kVuPrfOp: return c.prf_op;
      default: break;
    }
    if (off >= kVuStaticDupOffset + kVuTypeVl &&
        off < kVuStaticDupOffset + kVuDynamicEnd) {
      return ParamOf(c.dup, off - kVuStaticDupOffset);
    }
    return 0;
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
    inst->cm_fence = (v & kVuTrigCmFence) != 0;
    inst->seq = ++inst_seq;
    inst->tag = ++tag_seq & 0xFFu;

    // STREAM_ID_OVERRIDE = 0 时沿用 VU-Core CSR 自带的 Stream ID，那一份由
    // VU RV core 经 dsa_ids 直连给进来；= 1 时改用 trigger 里的 STREAM_ID
    // 字段。task_id 不在这个寄存器里，始终取 VU-Core 那一份。
    inst->stream_id = inst->sid_override ? ((v >> kVuTrigSidShift) & 0xFu)
                                         : CoreStreamId();
    inst->task_id = CoreTaskId();
    inst->user_id = CoreUserId();

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
    ReportError(kVuErrRegAddr, kVuErrUnitNone);
  }

  static void LatchRfErr(uint64_t& reg, uint64_t idx, VuMacroInst const* inst) {
    if ((reg & kVuErrCtxValid) != 0) return;
    uint64_t user = inst ? inst->user_id : 0;
    reg = (user & 0xFFFFu) | (((idx & kVuRfIdxMask) << kVuErrCtxIdxShift)) |
          kVuErrCtxValid;
  }

  // snapshot_addr 选中的那一条快照。SNAP_IDX=0 是状态字，1～0xC 是 12 个动态
  // 参数寄存器；SNAP_SEL 按年龄选，0xFF 选 sticky（error_info 锁存的那条）。
  uint64_t SnapshotValue() const {
    uint64_t idx = (snap_addr >> kVuSnapIdxShift) & 0xFu;
    uint64_t sel = (snap_addr >> kVuSnapSelShift) & 0xFFu;
    if (sel == kVuSnapSelSticky) {
      if (!sticky || (error_info & kVuErrInfoValid) == 0) return 0;
      if (idx == kVuSnapStatusWord) {
        uint64_t w = kVuSnapValid | (sticky_tag << kVuSnapTagShift);
        if (sticky_dispatched) w |= kVuSnapDispatched;
        return w;
      }
      return sticky->SnapParam(idx);
    }
    if (!snap_win || sel >= snap_win->SnapCount()) return 0;
    if (idx == kVuSnapStatusWord) {
      uint64_t w = kVuSnapValid | (snap_win->SnapTag(sel) << kVuSnapTagShift);
      if (snap_win->SnapDispatched(sel)) w |= kVuSnapDispatched;
      return w;
    }
    return snap_win->SnapParam(sel, idx);
  }

  VuRegfiles& regs;
  std::array<std::shared_ptr<DsaCfgPort>, kVuCfgPathNum> path;
  std::shared_ptr<VuInstPort> out;
  VuSnapshotWindow* snap_win = nullptr;
  VuCounterWindow* cnt_win = nullptr;

  std::array<VuStaticCfg, kVuCfgGroups> static_cfg{};
  VuDynParam dyn;
  std::array<uint64_t, kVuCfgGroups> ref{};

  std::shared_ptr<VuMacroInst> held;
  bool pending = false, driving = false;
  uint64_t out_seq = 0, inst_seq = 0, tag_seq = 0;
  uint64_t trigger_cnt = 0, blocked_cnt = 0, write_cnt = 0;
  uint64_t status = 0, macro_inst_left = 0, error_code = 0, profile_ctrl = 0;
  uint64_t error_info = 0, snap_addr = 0;
  uint64_t vrf_err_info = 0, mrf_err_info = 0, srf_err_info = 0;
  uint64_t cm_err_info = 0, cm_err_addr = 0, nan_err_info = 0;
  uint64_t inf_replace_value = 0, nan_replace_value = 0;
  uint64_t nan_replaced = 0, inf_replaced = 0;
  // sticky 快照：error_info 锁存的那一条宏指令。
  std::shared_ptr<VuMacroInst> sticky;
  uint64_t sticky_tag = 0;
  bool sticky_dispatched = false;
  uint64_t rf_addr = 0;
  bool rf_sel_ok = true;
  std::set<uint64_t> not_dispatched;
  std::shared_ptr<DsaIdsPort> ids;
  uint64_t fixed_stream = 0, fixed_task = 0, fixed_user = 0;
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
