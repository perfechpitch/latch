#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_TASK_CTRL_
#define _LATCH_BACH_IP_CHIP_CORE_TS_TASK_CTRL_

// Task_ctrl：当前任务做完之后生成后继，对应详细设计的 Task Generator。
//
// 每个 stream 独立推进，不需要全局 Task Pointer。只选 valid=1 且
// task_fsm=FINISH 且 end=0 的 stream。
//
// 后继是当前任务之后、THROUGH_END_MASK 以内、done_bitmap 还没置位的最低一项：
//
//   search = after_current & THROUGH_END_MASK & ~done_bitmap
//
// done_bitmap 里既有做完的也有跳过的，异步 datain 提前完成、重发任务被跳过都落
// 在这里。一次优先编码一拍选出，连续跳过的数量不增加周期。搜索不回绕、不越过
// End；End 提前完成、End 以前也没有剩下的，就不生成后继，由退休那一侧收尾。
//
// 后继的初始状态由 wait_wake 与 credit_en 定。PID：上一笔是 PID
// 更新任务、带回的 新 PID
// 还没被继承，而后继正好紧邻它，就继承那个新值；跳过了紧邻的那一项，就用
// 所选任务自己的 PID。
//
// 新任务的 task_id、task_fsm、end 与全部下发属性必须一起原子写入才算生成成功，
// 所以走的是整项写那个口。

#include <array>
#include <memory>
#include <optional>
#include <string>

#include "bach/ip/chip/core/ts/cfg_reg.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"
#include "base/log.h"

namespace latch {
namespace bach {

class TaskCtrl : public BachModule {
public:
  TaskCtrl(ClockPtr clock, const std::string &name, CfgReg &reg,
           uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick), cfg(reg),
        snap(std::make_shared<SnapshotPort>(clock)),
        install(std::make_shared<StreamWritePortIf>(clock)), installed(clock),
        skipped(clock) {}

  void AttachSnapshot(std::shared_ptr<SnapshotPort> p) { snap = std::move(p); }
  std::shared_ptr<StreamWritePortIf> InstallPtr() const { return install; }

  uint64_t Installed() const { return installed.Get(); }
  uint64_t Skipped() const { return skipped.Get(); }

  // 刚装进去的那一步的身份。Core 层拿 Installed() 认“本拍装了一笔后继”，与
  // 它下发那一拍对起来就是这一步在 TS 里等的时间。
  uint64_t NextTask() const { return next_task; }
  uint64_t NextUser() const { return next_user; }
  bool NextUserValid() const { return next_user_vld; }

  // 已被 StreamTable 收下的后继身份，避免下一批候选覆盖完成事件。
  uint64_t AcceptedTask() const { return accepted_task; }
  uint64_t AcceptedUser() const { return accepted_user; }
  bool AcceptedUserValid() const { return accepted_user_vld; }

  bool Quiescent() const override {
    if (pending)
      return false;
    for (auto const &token : tokens)
      if (token)
        return false;
    return true;
  }

protected:
  void Step() override {
    if (pending) {
      if (!install->Accepted())
        return;
      // accepted 表示批请求已处理，部分后继可能被同拍完成或退休取消。
      // 与表同拍发布的快照核对，身份只取实际安装的后继。
      auto accepted_view = snap->Get();
      auto request = install->Req();
      if (install->AcceptedCount() && accepted_view && request) {
        for (auto const &w : request->updates) {
          auto const &e = accepted_view->entry[w->stream_id];
          if (e.valid && e.allocation == w->allocation &&
              e.task_id == w->entry.task_id) {
            accepted_task = e.task_id;
            accepted_user = e.user_id;
            accepted_user_vld = e.user_id_vld;
          }
        }
      }
      install_pending += install->AcceptedCount();
      pending = false;
    }
    install->Idle();
    auto view = snap->Get();
    if (!view)
      return;
    auto batch = std::make_shared<StreamWrite>();
    batch->valid = true;
    for (uint64_t sid = 0; sid < view->slot_num; ++sid) {
      auto const &e = view->entry[sid];
      if (!e.valid) {
        tokens[sid].reset();
        waiting[sid].reset();
        continue;
      }
      if (waiting[sid]) {
        auto const &old = *waiting[sid];
        if (e.allocation != old.allocation || e.task_id != old.current ||
            (e.done_bitmap & (1ull << old.target)))
          waiting[sid].reset();
        else
          continue;
      }
      if (tokens[sid]) {
        Token token = *tokens[sid];
        tokens[sid].reset();
        if (e.allocation != token.allocation || e.task_id != token.current ||
            e.task_fsm != TaskFsm::kFinish ||
            (e.done_bitmap & (1ull << token.target)))
          continue;
        auto const &task = cfg.Task(token.target);
        if (!task.valid)
          continue;
        auto w = std::make_shared<StreamWrite>();
        w->valid = w->install = w->check_allocation = true;
        w->stream_id = sid;
        w->allocation = token.allocation;
        w->from_task_id = token.current;
        w->entry = e;
        w->entry.task_id = token.target;
        ApplyTaskAttr(w->entry, task);
        if (e.pid_pending && token.target == e.task_id + 1)
          w->entry.task_path_id = e.task_path_id;
        w->entry.pid_pending = false;
        w->entry.credit_drct = task.credit_en && !task.IsReduce()
                                   ? cfg.Route(w->entry.task_path_id).dir & 7
                                   : 0;
        w->entry.task_fsm = InitFsmOf(task);
        if (task.IsDataIn() &&
            (e.datain_accepted_mask & (1ull << token.target)))
          w->entry.task_fsm = TaskFsm::kInfly;
        if (w->entry.task_fsm == TaskFsm::kWaitCredit &&
            !(w->entry.credit_drct & ~e.drct_used))
          w->entry.task_fsm = TaskFsm::kReady;
        batch->updates.push_back(w);
        waiting[sid] = token;
        skip_pending += token.target - token.current - 1;
        next_task = token.target;
        next_user = e.user_id;
        next_user_vld = e.user_id_vld;
      } else if (e.task_fsm == TaskFsm::kFinish && !e.end) {
        uint64_t after = e.task_id == 63 ? 0 : ~0ull << (e.task_id + 1);
        uint64_t search = after & cfg.ThroughEndMask() & ~e.done_bitmap;
        if (search)
          tokens[sid] =
              Token{e.allocation, e.task_id, uint64_t(__builtin_ctzll(search))};
      }
    }
    if (!batch->updates.empty()) {
      install->Drive(batch);
      pending = true;
    }
    installed = install_pending;
    skipped = skip_pending;
    TracePerCycle("installed", install_pending);
  }

private:
  struct Token {
    uint64_t allocation, current, target;
  };
  std::array<std::optional<Token>, kStreamNum> tokens{}, waiting{};

  CfgReg &cfg;
  std::shared_ptr<SnapshotPort> snap;
  std::shared_ptr<StreamWritePortIf> install;

  bool pending = false;
  uint64_t install_pending = 0, skip_pending = 0;

  // 刚装进去的那一步的身份，供 Core 层发波形。自启动 core 上表项可能还没有用户
  // 身份，user 由 next_user_vld 说是真值还是占位。
  uint64_t next_task = 0, next_user = 0;
  bool next_user_vld = false;

  uint64_t accepted_task = 0, accepted_user = 0;
  bool accepted_user_vld = false;

  Logic64 installed, skipped;
};

} // namespace bach
} // namespace latch

#endif
