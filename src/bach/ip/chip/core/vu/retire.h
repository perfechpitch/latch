#ifndef _LATCH_BACH_IP_CHIP_CORE_VU_RETIRE_
#define _LATCH_BACH_IP_CHIP_CORE_VU_RETIRE_

// M9 · 退休与 dsa_done。
//
// 一条宏指令的全部微指令都完成、store 也落地了，这一条就退休：macro_inst_left
// 减一、把它从 Scoreboard 上摘掉、释放对这一组静态配置的引用。被阻塞的配置写
// 这时生效并解除阻塞。
//
// dsa_done 只在 EVENT_EN 置位时发给 TS：stream_id 与 task_id 是写 trigger 那一拍
// 从 dsa_ids 采下来的那一组（STREAM_ID_OVERRIDE 置位时 stream_id 用 trigger 里
// 的值），同拍把 event 拉高。未置 EVENT_EN 的宏指令照常退休，不打完成口。

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
  // 刚发给 TS 的那一笔 dsa_done 的笔数与身份。只在 EVENT_EN 置位的宏指令退休时
  // 加一，与打到完成口上的是同一拍、同一笔。Core 层发波形要用。
  //
  // 未置 EVENT_EN 的宏指令仍退休（减 left、摘记分板、放静态组），只是不报 TS。
  // 多宏任务只在最后一条置 EVENT_EN，TS 配 kDsa 等这一笔；kRvOnly 仍只看 rv_done。
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
    // 通路上一路带下来的异常这时候记进 error_code，上下文按首错锁存。
    if (f->error != 0) cfg_reg.ReportError(f->error, f->err_unit, &inst);
    if (f->nan_replaced != 0 || f->inf_replaced != 0) {
      cfg_reg.AddReplaces(f->nan_replaced, f->inf_replaced);
    }

    // 一条宏指令拆成几段流过时，前几段只是它的一截，走完最后一段这一条才算完。
    if (!f->seg_last) {
      done->Idle();
      return;
    }

    isq.Retire();
    pipe.Retire(inst.seq);
    cfg_reg.ReleaseCfg(inst.cfg_idx);

    // 只有 EVENT_EN 才把完成打给 TS。一个 task 发了几条时，只在最后一条置位，
    // TASK_RECV_UNIT 配 DSA，TS 收齐 rv_done 与这一笔 dsa_done。
    if (!inst.event_en) {
      done->Idle();
      return;
    }
    done->Drive(inst.stream_id, inst.task_id);
    done->event = 1;
    ++retire_cnt;
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
