#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_TASK_CTRL_
#define _LATCH_BACH_IP_CHIP_CORE_TS_TASK_CTRL_

// Task_ctrl：当前任务做完之后生成后继，对应详细设计的 Task Generator。
//
// 每个 stream 独立推进，不需要全局 Task Pointer。只选 valid=1 且 task_fsm=FINISH
// 且 end=0 的 stream。
//
// 后继是当前任务之后、THROUGH_END_MASK 以内、done_bitmap 还没置位的最低一项：
//
//   search = after_current & THROUGH_END_MASK & ~done_bitmap
//
// done_bitmap 里既有做完的也有跳过的，异步 datain 提前完成、重发任务被跳过都落
// 在这里。一次优先编码一拍选出，连续跳过的数量不增加周期。搜索不回绕、不越过
// End；End 提前完成、End 以前也没有剩下的，就不生成后继，由退休那一侧收尾。
//
// 后继的初始状态由 wait_wake 与 credit_en 定。PID：上一笔是 PID 更新任务、带回的
// 新 PID 还没被继承，而后继正好紧邻它，就继承那个新值；跳过了紧邻的那一项，就用
// 所选任务自己的 PID。
//
// 新任务的 task_id、task_fsm、end 与全部下发属性必须一起原子写入才算生成成功，
// 所以走的是整项写那个口。

#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/ts/cfg_reg.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class TaskCtrl : public BachModule {
 public:
  TaskCtrl(ClockPtr clock, const std::string& name, CfgReg& reg,
           uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        cfg(reg),
        snap(std::make_shared<SnapshotPort>(clock)),
        install(std::make_shared<StreamWritePortIf>(clock)),
        installed(clock),
        skipped(clock) {}

  void AttachSnapshot(std::shared_ptr<SnapshotPort> p) { snap = std::move(p); }
  std::shared_ptr<StreamWritePortIf> InstallPtr() const { return install; }

  uint64_t Installed() const { return installed.Get(); }
  uint64_t Skipped() const { return skipped.Get(); }

  bool Quiescent() const override { return !pending; }

 protected:
  void Step() override {
    if (pending) {
      if (install->Accepted()) {
        pending = false;
        ++install_pending;
      } else {
        // 整项写失败后要重读最新表内容再来，所以这里不保持旧请求，下一拍重算。
        pending = false;
      }
    }
    if (!pending) {
      install->Idle();
      Generate();
    }
    installed = install_pending;
    skipped = skip_pending;
    TracePerCycle("installed", install_pending);
  }

 private:
  void Generate() {
    StreamSnapshotPtr s = snap->Get();
    if (!s) return;
    for (uint64_t k = 0; k < kStreamNum; ++k) {
      uint64_t i = s->AgeOrder(k);
      StreamEntry const& e = s->entry[i];
      if (!e.valid || e.task_fsm != TaskFsm::kFinish || e.end) continue;

      uint64_t after =
          e.task_id + 1 >= kTaskChainNum ? 0 : ~0ull << (e.task_id + 1);
      uint64_t search = after & cfg.ThroughEndMask() & ~e.done_bitmap;
      if (search == 0) continue;  // End 以前没有剩下的
      uint64_t next = uint64_t(__builtin_ctzll(search));
      skip_pending += next - e.task_id - 1;

      TaskEntry const& t = cfg.Task(next);
      auto w = std::make_shared<StreamWrite>();
      w->valid = true;
      w->stream_id = i;
      w->whole = true;
      w->entry = e;
      w->entry.task_id = next;
      ApplyTaskAttr(w->entry, t);
      if (e.pid_pending && next == e.task_id + 1) {
        w->entry.task_path_id = e.task_path_id;
      }
      w->entry.pid_pending = false;
      w->entry.task_fsm = InitFsmOf(t);
      install->Drive(w);
      pending = true;
      return;  // 一拍只装一个
    }
  }

  CfgReg& cfg;
  std::shared_ptr<SnapshotPort> snap;
  std::shared_ptr<StreamWritePortIf> install;

  bool pending = false;
  uint64_t install_pending = 0, skip_pending = 0;

  Logic64 installed, skipped;
};

}  // namespace bach
}  // namespace latch

#endif
