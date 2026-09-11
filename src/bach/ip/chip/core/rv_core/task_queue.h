#ifndef _LATCH_BACH_IP_CHIP_CORE_RV_CORE_TASK_QUEUE_
#define _LATCH_BACH_IP_CHIP_CORE_RV_CORE_TASK_QUEUE_

// M1 · task_queue 出队。
//
// 提前接收 TS 下发的 task，前一个 task 完成后立刻执行队头缓存的那个。用户
// 之间的切换因此没有 bubble。队列深 2：一个在跑，一个等着。
//
// cmd_ready 就是「队列还有空槽」。拉低时 TS 不能释放这个 task 跳到下一个，所以
// 这一根线是 TS 侧那条链的闸门。
//
// 下发信息里 stream_id、task_id、user_id 与 path_id 由硬件写进自定义 CSR 供软件
// 读。
// user_id 那一个可读写：自启动的 B core 与 R core 上 TS 下发时还没有用户身份，
// 软件在 flag_check 认出这一笔属于哪个用户之后自己写进来。

#include <deque>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/rv_core/rv_ports.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 一笔从 TS 收下的 task。
struct RvTask {
  uint64_t task_pc = 0;
  uint64_t stream_id = 0;
  uint64_t task_id = 0;
  uint64_t user_id = 0;
  // DSA 出核时包头里的 path 字段用它。TS 随 task 一起送来。
  uint64_t path_id = 0;
  bool dsa_en = false;
  // DTE 任务的 VCID，经身份口带给 DTE。
  uint64_t vcid = 0;
};

class RvTaskQueue : public BachModule {
 public:
  RvTaskQueue(ClockPtr clock, const std::string& name, uint64_t parent = 0,
              bool tick = true)
      : BachModule(clock, name, parent, tick),
        cmd(std::make_shared<TaskCmdPort>(clock)),
        start(std::make_shared<TaskStartPort>(clock)),
        finish(std::make_shared<TaskDonePort>(clock)),
        depth(clock),
        started(clock) {}

  TaskCmdPort& Cmd() { return *cmd; }
  void AttachCmd(std::shared_ptr<TaskCmdPort> p) { cmd = std::move(p); }
  TaskStartPort& Start() { return *start; }
  std::shared_ptr<TaskStartPort> StartPtr() const { return start; }
  // 指令执行器执行 task_done 后从这个口报回来，队列据此起下一个。
  std::shared_ptr<TaskDonePort> FinishPtr() const { return finish; }

  uint64_t QueueLen() const { return q.size(); }
  uint64_t Started() const { return start_cnt; }
  uint64_t Finishes() const { return finish_cnt; }
  bool Busy() const { return running; }
  bool Quiescent() const override { return q.empty() && !running; }

 protected:
  void Step() override {
    // 末级先做：先看执行器交还了没有，再起下一个，最后收新的。
    TakeFinish();
    Launch();
    TakeCmd();

    depth = q.size();
    started = start_cnt;
    TracePerCycle("depth", q.size());
  }

 private:
  void TakeFinish() {
    if (!finish->Valid() || finish->Seq() == last_finish_seq) return;
    last_finish_seq = finish->Seq();
    running = false;
    ++finish_cnt;
  }

  // 前一个做完了就把队头那个交给执行器。ready 是执行器上一拍报的。
  //
  // 握手那一支排在最前：这一笔已经从队列里取出来放在 held 上了，队列这时是空的。
  // 若先按「队列空就 Idle」判，正在握手的那一笔会被覆盖成 Idle，执行器再也
  // 收不到它。
  void Launch() {
    if (driving) {
      if (!start->Ready()) {
        start->Drive(held.task_pc, held.stream_id, held.task_id, held.user_id,
                     held.path_id, held.dsa_en, start_seq, held.vcid);
        return;
      }
      driving = false;
      running = true;
      ++start_cnt;
      start->Idle();
      return;
    }
    if (running || q.empty()) {
      start->Idle();
      return;
    }
    held = q.front();
    q.pop_front();
    driving = true;
    ++start_seq;
    start->Drive(held.task_pc, held.stream_id, held.task_id, held.user_id,
                 held.path_id, held.dsa_en, start_seq, held.vcid);
  }

  // cmd_ready = 队列有空槽。未被接收时 TS 不能释放该 task。
  void TakeCmd() {
    cmd->DriveReady(q.size() < kRvTaskQueueDepth);
    if (!cmd->Valid() || q.size() >= kRvTaskQueueDepth) return;
    // 同一笔会连着几拍出现在端口上，按序号认它。不能按 (stream, task) 认：
    // B core 与 R core 的 datain 任务不占 stream 表项，几笔的这两项都是 0。
    if (cmd->Seq() == last_cmd_seq) return;
    last_cmd_seq = cmd->Seq();
    RvTask t;
    t.task_pc = cmd->task_pc.Get();
    t.stream_id = cmd->stream_id.Get();
    t.task_id = cmd->task_id.Get();
    t.user_id = cmd->user_id.Get();
    t.path_id = cmd->path_id.Get();
    t.dsa_en = cmd->task_dsa_en.Get() != 0;
    t.vcid = cmd->vcid.Get();
    q.push_back(t);
  }

  std::shared_ptr<TaskCmdPort> cmd;
  std::shared_ptr<TaskStartPort> start;
  std::shared_ptr<TaskDonePort> finish;

  std::deque<RvTask> q;
  RvTask held;
  bool running = false, driving = false;
  uint64_t last_cmd_seq = 0, last_finish_seq = 0;
  uint64_t start_seq = 0, start_cnt = 0, finish_cnt = 0;

  Logic64 depth, started;
};

}  // namespace bach
}  // namespace latch

#endif
