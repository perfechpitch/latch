#ifndef _LATCH_BACH_IP_CHIP_CORE_ROUTER_CREDIT_MONITOR_
#define _LATCH_BACH_IP_CHIP_CORE_ROUTER_CREDIT_MONITOR_

// CoreMemCreditMonitor：TS 的资源申请在 Router 这一侧的落点。
//
// 只有 Router 负责真正申请 Stream 表项，禁止超额分配或重复授权。TS 在下发任务前
// 向 Router 查到指定 user 的授权，DTE 拿到任务时这份授权已经到手。
//
// Router 的进 core 表与 TS 内部的 stream credit 表按完全一致的逻辑申请空项，
// 分配因此不会多于实际资源数，这就是「Router 通知 TS 的包一定能被 TS 接收」
// 的依据。
//
// M15 监听队列：16 项全相连，可同时监听多笔多方向的申请。申请到就经反向控制通路
// 通知 TS，回 StreamID、TaskID、PathID；申请不到就把需求记进队列，资源满足再通知。
// 多个事件同时满足时按 StreamID 仲裁，选最老的任务通知 TS。
//
// 进 core 重发的任务也注册到这个队列：数据进 Core、资源就绪后通知 TS 重发。
// 同一 VC 的数据包要保序，当前 VC 有未重发完的数据时后续包不能提前发送。

#include <deque>
#include <functional>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 监听事件队列项数。
constexpr uint64_t kMonitorQDepth = 16;

// TS → Monitor：注册资源申请。
class CreditReqPort : public Logic {
 public:
  Logic64 req_valid, user_id, stream_id, task_id, path_id, req_ready;

  explicit CreditReqPort(ClockPtr c)
      : req_valid(c), user_id(c), stream_id(c), task_id(c), path_id(c),
        req_ready(c) {
    Fields(req_valid, user_id, stream_id, task_id, path_id, req_ready);
  }

  void Drive(uint64_t user, uint64_t stream, uint64_t task, uint64_t path) {
    req_valid = 1;
    user_id = user;
    stream_id = stream;
    task_id = task;
    path_id = path;
  }
  void Idle() {
    req_valid = 0;
    user_id = 0;
    stream_id = 0;
    task_id = 0;
    path_id = 0;
  }
  void DriveReady(bool ok) { req_ready = ok ? 1 : 0; }
  bool Ready() const { return req_ready.Get() != 0; }
  bool Valid() const { return req_valid.Get() != 0; }
};

// Monitor → TS：资源到手（脉冲）。
class CreditGrantPort : public Logic {
 public:
  Logic64 valid, stream_id, task_id, path_id;

  explicit CreditGrantPort(ClockPtr c)
      : valid(c), stream_id(c), task_id(c), path_id(c) {
    Fields(valid, stream_id, task_id, path_id);
  }

  void Drive(uint64_t stream, uint64_t task, uint64_t path) {
    valid = 1;
    stream_id = stream;
    task_id = task;
    path_id = path;
  }
  void Idle() {
    valid = 0;
    stream_id = 0;
    task_id = 0;
    path_id = 0;
  }
  bool Valid() const { return valid.Get() != 0; }
};

class CreditMonitor : public BachModule {
 public:
  // 查一条 path 的资源够不够。装配层接到 Xbar 与 CoreStation 上。
  using ResourceCheck = std::function<bool(uint64_t user, uint64_t path)>;
  // 真正占坑。
  using ResourceTake = std::function<void(uint64_t user, uint64_t path)>;

  CreditMonitor(ClockPtr clock, const std::string& name, RouterTable& table,
                uint64_t copy_idx, uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        rtab(table),
        copy(copy_idx),
        req(std::make_shared<CreditReqPort>(clock)),
        grant(std::make_shared<CreditGrantPort>(clock)),
        queued(clock),
        notified(clock) {}

  CreditReqPort& Req() { return *req; }
  std::shared_ptr<CreditReqPort> ReqPtr() const { return req; }
  CreditGrantPort& Grant() { return *grant; }
  std::shared_ptr<CreditGrantPort> GrantPtr() const { return grant; }

  void SetCheck(ResourceCheck fn) { check = std::move(fn); }
  void SetTake(ResourceTake fn) { take = std::move(fn); }

  uint64_t QueueLen() const { return q.size(); }
  uint64_t Notified() const { return notified.Get(); }

  bool Quiescent() const override { return q.empty(); }

 protected:
  void Step() override {
    // 末级先做：先看队列里有没有等到资源的，再收新的申请。
    NotifyOne();
    TakeRequest();

    queued = q.size();
    notified = notified_pending;
    TracePerCycle("queued", q.size());
    TracePerCycle("notified", notified_pending);
  }

 private:
  struct Event {
    uint64_t user_id = 0, stream_id = 0, task_id = 0, path_id = 0;
    uint64_t enqueue_seq = 0;
  };

  void TakeRequest() {
    bool room = q.size() < kMonitorQDepth;
    req->DriveReady(room);
    if (!req->Valid() || !room) return;
    uint64_t user = req->user_id.Get();
    uint64_t task = req->task_id.Get();
    // 同一笔会连着两拍出现在端口上，按 (user, task) 认它。
    if (seen_once && user == last_user && task == last_task) return;
    Event e;
    e.user_id = user;
    e.stream_id = req->stream_id.Get();
    e.task_id = task;
    e.path_id = req->path_id.Get();
    e.enqueue_seq = next_seq++;
    q.push_back(e);
    last_user = user;
    last_task = task;
    seen_once = true;
  }

  // 多个事件同时满足时按 StreamID 仲裁，选最老的任务通知 TS。
  void NotifyOne() {
    auto pick = q.end();
    for (auto it = q.begin(); it != q.end(); ++it) {
      if (check && !check(it->user_id, it->path_id)) continue;
      if (pick == q.end() || it->stream_id < pick->stream_id ||
          (it->stream_id == pick->stream_id &&
           it->enqueue_seq < pick->enqueue_seq)) {
        pick = it;
      }
    }
    if (pick == q.end()) {
      grant->Idle();
      return;
    }
    if (take) take(pick->user_id, pick->path_id);
    grant->Drive(pick->stream_id, pick->task_id, pick->path_id);
    q.erase(pick);
    ++notified_pending;
  }

  RouterTable& rtab;
  uint64_t copy;
  std::shared_ptr<CreditReqPort> req;
  std::shared_ptr<CreditGrantPort> grant;
  ResourceCheck check;
  ResourceTake take;

  // Step 独占。
  std::deque<Event> q;
  uint64_t next_seq = 0;
  uint64_t last_user = 0, last_task = 0;
  bool seen_once = false;
  uint64_t notified_pending = 0;

  Logic64 queued, notified;
};

}  // namespace bach
}  // namespace latch

#endif
