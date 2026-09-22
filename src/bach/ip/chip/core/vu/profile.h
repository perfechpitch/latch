#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_PROFILE_
#define _LATCH_BACH_IP_CHIP_CORE_VU_PROFILE_

// Profile 计数器。
//
// profile_ctrl 在 0x4000，32 个计数器从 0x4008 起，每个拆成 _lo [31:0] 与
// _hi [63:32] 两个 32 位寄存器，共 64 个（0x4008 ～ 0x4104）。0x4004 是未实现
// 地址。计数器的名字与顺序照《VU-DSA 寄存器整理》，按时间基准 → 配置期 → 发射期
// → 执行期的次序排。
//
// profile_ctrl.RUN 置 1 才计数，置 0 暂停并保持当前值；CLEAR 写 1 清零全部
// 计数器后自动归零，RUN=1 时也可以清。软件写计数器本身无效、不报错，与
// error_code 那一档不一样：error_code 是读的时候全部清零并同时清
// status.ERROR_FLAG。计数器按 64-bit 累加，溢出后回绕，不置任何异常。
//
// 发射期的四个 issue_stall_* 是「pipe_ctrl 有空位却没派发」的原因分解，按
// fence > cmfence > dep > eu 的优先级归因、一拍只记一项，可以相加：
//   有空位周期 = 派发周期 + 四项之和 + issue_starve_cycle。
// 取值都从各模块的观测访问器读，所以这里记的是相邻两拍的差。

#include <array>
#include <string>

#include "bach/ip/chip/core/vu/config_register.h"
#include "bach/ip/chip/core/vu/isq.h"
#include "bach/ip/chip/core/vu/lu.h"
#include "bach/ip/chip/core/vu/mexe.h"
#include "bach/ip/chip/core/vu/pipe_ctrl.h"
#include "bach/ip/chip/core/vu/sexe.h"
#include "bach/ip/chip/core/vu/su.h"
#include "bach/ip/chip/core/vu/valu.h"
#include "bach/ip/chip/core/vu/vsfu.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 计数器编号，偏移是 kVuProfileCnt + idx * 8（每个占 _lo 与 _hi 两个寄存器）。
enum class VuCounter : uint32_t {
  // 时间基准
  kRunCycle = 0,            // RUN=1 的采样窗口 Cycle 数
  kTotalBusyCycle = 1,      // VU-DSA 处于 Busy 状态的总 Cycle 数
  // 配置期
  kCfgWrNum = 2,            // 配置通路上生效的寄存器写次数
  kCfgWrStallCycle = 3,     // 配置写因静态配置组在用而阻塞的 Stall Cycle
  // 发射期
  kMacroInstTotalNum = 4,   // 累计发射的宏指令条数
  kMacroInstRetireNum = 5,  // 累计退休的宏指令条数
  kIsqFullCycle = 6,        // ISQ 满的 Cycle 数
  kStallFenceCycle = 7,     // MACRO_INST_FENCE 等前序全部完成
  kStallCmFenceCycle = 8,   // CM_FENCE 等前序的 CM 访问完成
  kStallDepCycle = 9,       // Scoreboard 数据依赖
  kStallEuCycle = 10,       // 执行分组结构冒险
  kIssueStarveCycle = 11,   // 硬件有空位而 ISQ 为空
  // 执行期·访存
  kLuBusyCycle = 12,
  kCmLdReqNum = 13,         // 发往 CM 的读请求拍数（一拍 128 Byte）
  kCmLdStallCycle = 14,
  kSuBusyCycle = 15,
  kCmStReqNum = 16,
  kCmStStallCycle = 17,
  // 执行期·计算
  kValu0BusyCycle = 18,
  kValu1BusyCycle = 19,
  kValu2BusyCycle = 20,
  kVsfu0BusyCycle = 21,     // BF16 拼接时与 VSFU1 同时计入
  kVsfu1BusyCycle = 22,
  kMexeBusyCycle = 23,
  kSexeBusyCycle = 24,      // 三次迭代累加
  // 执行期·端口
  kVrfRdP0BusyCycle = 25,
  kVrfRdP1BusyCycle = 26,
  kVrfWtP0BusyCycle = 27,
  kVrfWtP1BusyCycle = 28,
  kMrfWtBusyCycle = 29,
  kNanReplaceCnt = 30,      // 替换模式下被换成 NAN_REPLACE_VALUE 的 element 数
  kInfReplaceCnt = 31,      // 替换模式下被换成 ±INF_REPLACE_VALUE 的 element 数
};

class VuProfile : public BachModule, public VuCounterWindow {
 public:
  VuProfile(ClockPtr clock, const std::string& name, VuConfigRegister& cr,
            VuIsq& queue, VuPipeCtrl& ctrl, VuLu& load, VuSu& store,
            std::array<VuValu*, 3> const& valu_list, VuVsfu& sfu, VuMexe& mask,
            VuSexe& scalar, uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        cfg_reg(cr),
        isq(queue),
        pipe(ctrl),
        lu(load),
        su(store),
        valu(valu_list),
        vsfu(sfu),
        mexe(mask),
        sexe(scalar),
        busy(clock) {}

  uint64_t Counter(VuCounter c) const { return cnt[uint64_t(c)]; }
  // 配置总线上读计数器：每个拆成 _lo 与 _hi 两个 32 位寄存器。
  uint64_t CounterLo(uint64_t rank) const override {
    return rank < kVuCounterNum ? (cnt[rank] & 0xFFFFFFFFull) : 0;
  }
  uint64_t CounterHi(uint64_t rank) const override {
    return rank < kVuCounterNum ? (cnt[rank] >> 32) : 0;
  }
  bool Quiescent() const override { return true; }

 protected:
  void Step() override {
    uint64_t ctrl_word = cfg_reg.ProfileCtrl();
    if ((ctrl_word & kVuProfileClear) != 0) Rebase();
    // RUN 置 0 时暂停计数并保持当前值。
    if ((ctrl_word & kVuProfileRun) == 0 && started) {
      busy = cnt[uint64_t(VuCounter::kTotalBusyCycle)];
      return;
    }
    started = true;

    ++cnt[uint64_t(VuCounter::kRunCycle)];
    bool vu_busy = !isq.Empty() || isq.Inflight() > 0;
    if (vu_busy) ++cnt[uint64_t(VuCounter::kTotalBusyCycle)];
    if (isq.Full()) ++cnt[uint64_t(VuCounter::kIsqFullCycle)];
    // 硬件有空位而 ISQ 为空：发射被饿着。
    if (isq.Empty() && isq.Inflight() < kVuOverlap) {
      ++cnt[uint64_t(VuCounter::kIssueStarveCycle)];
    }

    Delta(VuCounter::kCfgWrNum, cfg_reg.Writes(), base_cfg_wr);
    Delta(VuCounter::kCfgWrStallCycle, cfg_reg.BlockedWrites(), base_cfg_stall);
    Delta(VuCounter::kMacroInstTotalNum, cfg_reg.Triggers(), base_macro_in);
    Delta(VuCounter::kMacroInstRetireNum, isq.Retired(), base_macro_out);
    Delta(VuCounter::kStallFenceCycle, pipe.FenceStalls(), base_fence);
    Delta(VuCounter::kStallCmFenceCycle, pipe.CmFenceStalls(), base_cmfence);
    Delta(VuCounter::kStallDepCycle, pipe.DepStalls(), base_dep);
    Delta(VuCounter::kStallEuCycle, pipe.EuStalls(), base_eu);
    Delta(VuCounter::kLuBusyCycle, lu.BusyCycles(), base_lu_busy);
    Delta(VuCounter::kCmLdReqNum, lu.Beats(), base_ld);
    Delta(VuCounter::kCmLdStallCycle, lu.StallCycles(), base_ld_stall);
    Delta(VuCounter::kSuBusyCycle, su.BusyCycles(), base_su_busy);
    Delta(VuCounter::kCmStReqNum, su.Beats(), base_st);
    Delta(VuCounter::kCmStStallCycle, su.StallCycles(), base_st_stall);
    for (uint64_t i = 0; i < 3; ++i) {
      Delta(VuCounter(uint64_t(VuCounter::kValu0BusyCycle) + i),
            valu[i]->BusyCycles(), base_valu[i]);
    }
    // FP32 下两个 VSFU 各自计数；BF16 拼接时同一拍同时计入两者。
    Delta(VuCounter::kVsfu0BusyCycle, vsfu.Busy0(), base_vsfu);
    Delta(VuCounter::kVsfu1BusyCycle, vsfu.Busy1(), base_vsfu1);
    Delta(VuCounter::kMexeBusyCycle, mexe.BusyCycles(), base_mexe);
    Delta(VuCounter::kSexeBusyCycle, sexe.BusyCycles(), base_sexe);

    // RF 端口的 busy 拍数：一条宏指令派发时按它要用的端口各记一拍。
    Delta(VuCounter::kVrfRdP0BusyCycle, pipe.VrfRdBusy(0), base_vrf_rd[0]);
    Delta(VuCounter::kVrfRdP1BusyCycle, pipe.VrfRdBusy(1), base_vrf_rd[1]);
    Delta(VuCounter::kVrfWtP0BusyCycle, pipe.VrfWtBusy(0), base_vrf_wt[0]);
    Delta(VuCounter::kVrfWtP1BusyCycle, pipe.VrfWtBusy(1), base_vrf_wt[1]);
    Delta(VuCounter::kMrfWtBusyCycle, pipe.MrfWtBusy(), base_mrf_wt);

    Delta(VuCounter::kNanReplaceCnt, cfg_reg.NanReplaced(), base_nan_rep);
    Delta(VuCounter::kInfReplaceCnt, cfg_reg.InfReplaced(), base_inf_rep);

    busy = cnt[uint64_t(VuCounter::kTotalBusyCycle)];
  }

 private:
  void Delta(VuCounter c, uint64_t now, uint64_t& base) {
    cnt[uint64_t(c)] = now - base;
  }

  // CLEAR：全部计数器清零，各来源的基准重新取当前值。
  void Rebase() {
    cnt.fill(0);
    base_cfg_wr = cfg_reg.Writes();
    base_cfg_stall = cfg_reg.BlockedWrites();
    base_macro_in = cfg_reg.Triggers();
    base_macro_out = isq.Retired();
    base_fence = pipe.FenceStalls();
    base_cmfence = pipe.CmFenceStalls();
    base_dep = pipe.DepStalls();
    base_eu = pipe.EuStalls();
    base_lu_busy = lu.BusyCycles();
    base_ld = lu.Beats();
    base_ld_stall = lu.StallCycles();
    base_su_busy = su.BusyCycles();
    base_st = su.Beats();
    base_st_stall = su.StallCycles();
    for (uint64_t i = 0; i < 3; ++i) base_valu[i] = valu[i]->BusyCycles();
    base_vsfu = vsfu.Busy0();
    base_vsfu1 = vsfu.Busy1();
    base_mexe = mexe.BusyCycles();
    base_sexe = sexe.BusyCycles();
    for (uint64_t i = 0; i < 2; ++i) {
      base_vrf_rd[i] = pipe.VrfRdBusy(i);
      base_vrf_wt[i] = pipe.VrfWtBusy(i);
    }
    base_mrf_wt = pipe.MrfWtBusy();
    base_nan_rep = cfg_reg.NanReplaced();
    base_inf_rep = cfg_reg.InfReplaced();
  }

  VuConfigRegister& cfg_reg;
  VuIsq& isq;
  VuPipeCtrl& pipe;
  VuLu& lu;
  VuSu& su;
  std::array<VuValu*, 3> valu;
  VuVsfu& vsfu;
  VuMexe& mexe;
  VuSexe& sexe;

  std::array<uint64_t, kVuCounterNum> cnt{};
  bool started = false;
  uint64_t base_cfg_wr = 0, base_cfg_stall = 0;
  uint64_t base_macro_in = 0, base_macro_out = 0;
  uint64_t base_fence = 0, base_cmfence = 0, base_dep = 0, base_eu = 0;
  uint64_t base_lu_busy = 0, base_ld = 0, base_ld_stall = 0;
  uint64_t base_su_busy = 0, base_st = 0, base_st_stall = 0;
  std::array<uint64_t, 3> base_valu{};
  uint64_t base_vsfu = 0, base_vsfu1 = 0, base_mexe = 0, base_sexe = 0;
  std::array<uint64_t, 2> base_vrf_rd{}, base_vrf_wt{};
  uint64_t base_mrf_wt = 0;
  uint64_t base_nan_rep = 0, base_inf_rep = 0;

  Logic64 busy;
};

}  // namespace bach
}  // namespace latch

#endif
