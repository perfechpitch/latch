#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_RETIRE_
#define _LATCH_BACH_IP_CHIP_CORE_VU_RETIRE_

// M9 · 退休与 dsa_done。
//
// 一条宏指令的全部微指令都完成、store 也落地了，这一条就退休：macro_inst_left
// 减一、把它从 Scoreboard 上摘掉、释放对这一组静态配置的引用。被阻塞的配置写
// 这时生效并解除阻塞。
//
// dsa_done 报给 TS 的 stream_id 与 task_id 就是写 trigger 那一拍从 dsa_ids 采下
// 来的那一组；STREAM_ID_OVERRIDE 置位时 stream_id 用的是 trigger 里显式给定的值。
// EVENT_EN 置位时同拍另发一个 Event 硬件同步信号。

#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/chip/core/vu/config_register.h"
#include "bach/ip/chip/core/vu/isq.h"
#include "bach/ip/chip/core/vu/pipe_ctrl.h"
#include "bach/ip/chip/core/vu/vu_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class VuRetire : public BachModule {
 public:
  VuRetire(ClockPtr clock, const std::string& name, VuConfigRegister& cr,
           VuIsq& queue, VuPipeCtrl& ctrl, uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        cfg_reg(cr),
        isq(queue),
        pipe(ctrl),
        in(std::make_shared<VuFlowPort>(clock)),
        done(std::make_shared<DonePort>(clock)),
        retired(clock) {}

  VuFlowPort& In() { return *in; }
  void AttachIn(std::shared_ptr<VuFlowPort> p) { in = std::move(p); }
  DonePort& Done() { return *done; }
  void AttachDone(std::shared_ptr<DonePort> p) { done = std::move(p); }

  uint64_t Retired() const { return retired.Get(); }
  // 刚退休那一条宏指令的笔数与身份。一条宏指令退休就报一次 dsa_done，这与硬件
  // 发给 TS 的是同一拍、同一笔。Core 层发波形要用：这一层自己的信号在 chip 级被
  // TraceOffScope 关掉了，只能由 Core 统一发。
  //
  // 不按「在飞归零」记 —— VU 不知道 task 的边界（那是软件的事），实测同一笔 task
  // 里 ISQ 会真的空好几次，按归零记会漏掉终点、段一路拉到波形末尾。一笔 task 的
  // 「最后一条宏指令完成」由波形那一侧把这些段并起来表达。
  //
  // 注意：配了 kRvOnly 的档上，TS 收到的 task_done 不是这一路，而是 RV core 轮询
  // macro_inst_left 到 0 之后的 ACK —— 那个已经由 rv_done 记着。
  uint64_t MacroDoneCnt() const { return retire_cnt; }
  uint64_t MacroDoneTask() const { return done_task; }
  uint64_t MacroDoneStream() const { return done_stream; }
  uint64_t MacroDoneUser() const { return done_user; }
  bool Quiescent() const override { return true; }

 protected:
  void Step() override {
    in->DriveReady(true);
    Take();
    retired = retire_cnt;
    TracePerCycle("retired", retire_cnt);
  }

 private:
  void Take() {
    if (!in->Valid() || in->Seq() == last_seq) {
      done->Idle();
      return;
    }
    VuFlowPtr f = in->Flow();
    if (!f) {
      done->Idle();
      return;
    }
    last_seq = in->Seq();

    VuMacroInst const& inst = f->uops.inst;
    // 通路上一路带下来的转换异常这时候记进 error_code。
    if (f->error != 0) cfg_reg.RaiseError(f->error);

    // 一条宏指令拆成几段流过时，前几段只是它的一截，走完最后一段这一条才算完。
    if (!f->seg_last) {
      done->Idle();
      return;
    }

    isq.Retire();
    pipe.Retire(inst.seq);
    cfg_reg.ReleaseCfg(inst.cfg_idx);

    // 一条宏指令退休就报一次。一个 task 发了几条时，最后由 RV core 轮询
    // macro_inst_left 到 0 再报 TS，那一档的 TASK_RECV_UNIT 配 00，只收 RV core
    // 的 ACK。
    done->Drive(inst.stream_id, inst.task_id);
    if (inst.event_en) done->event = 1;
    ++retire_cnt;
    // 报给 TS 的那一拍就是它，身份从指令上取。
    done_stream = inst.stream_id;
    done_task = inst.task_id;
    done_user = inst.user_id;
  }

  VuConfigRegister& cfg_reg;
  VuIsq& isq;
  VuPipeCtrl& pipe;
  std::shared_ptr<VuFlowPort> in;
  std::shared_ptr<DonePort> done;
  uint64_t last_seq = 0, retire_cnt = 0;
  // 最后一条宏指令退休的笔数与身份，供 Core 层发波形。
  uint64_t done_stream = 0, done_task = 0, done_user = 0;

  Logic64 retired;
};

}  // namespace bach
}  // namespace latch

#endif
