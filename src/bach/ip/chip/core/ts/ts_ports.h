#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_TS_PORTS_
#define _LATCH_BACH_IP_CHIP_CORE_TS_TS_PORTS_

// TS 与 core 内其他单元之间的端口，以及 TS 内部各模块之间的写口。

#include <array>
#include <cstdint>
#include <memory>

#include "base/logic.h"
#include "bach/ip/chip/core/ts/ts_types.h"

namespace latch {
namespace bach {

// TS → RV core：task 下发。
// cmd_ready 是该 RV core 的 task_queue 有没有空槽，也就是 raw ACCEPT。
class TaskCmdPort : public Logic {
 public:
  Logic64 cmd_valid, task_pc, stream_id, task_id, user_id, path_id,
      task_dsa_en, cmd_ready;
  // 一笔命令连着几拍出现在端口上，收方按序号认它。B core 与 R core 的 datain
  // 任务不占 stream 表项，几笔的 stream_id 与 task_id 都是 0，只有序号分得开。
  Logic64 seq;

  explicit TaskCmdPort(ClockPtr c)
      : cmd_valid(c), task_pc(c), stream_id(c), task_id(c), user_id(c),
        path_id(c), task_dsa_en(c), cmd_ready(c), seq(c) {
    Fields(cmd_valid, task_pc, stream_id, task_id, user_id, path_id,
           task_dsa_en, cmd_ready, seq);
  }

  void Drive(uint64_t pc, uint64_t stream, uint64_t task, uint64_t user,
             uint64_t path, bool dsa_en, uint64_t issue_seq) {
    cmd_valid = 1;
    task_pc = pc;
    stream_id = stream;
    task_id = task;
    user_id = user;
    path_id = path;
    task_dsa_en = dsa_en ? 1 : 0;
    seq = issue_seq;
  }
  void Idle() {
    cmd_valid = 0;
    task_pc = 0;
    stream_id = 0;
    task_id = 0;
    user_id = 0;
    path_id = 0;
    task_dsa_en = 0;
  }
  uint64_t Seq() const { return seq.Get(); }
  void DriveReady(bool ok) { cmd_ready = ok ? 1 : 0; }
  bool Ready() const { return cmd_ready.Get() != 0; }
  bool Valid() const { return cmd_valid.Get() != 0; }
};

// RV core / DSA → TS：完成脉冲。
// 七路都是脉冲，Task_done 永远就绪、不向上游反压：完成事件在硬件里没有重发通路，
// 接收方一旦拒收就等于把那个 stream 永远停在当前 task。
class DonePort : public Logic {
 public:
  Logic64 valid, stream_id, user_id, task_id, reduce_seq, event;

  explicit DonePort(ClockPtr c)
      : valid(c), stream_id(c), user_id(c), task_id(c), reduce_seq(c),
        event(c) {
    Fields(valid, stream_id, user_id, task_id, reduce_seq, event);
  }

  void Drive(uint64_t stream, uint64_t task, uint64_t seq = 0,
             uint64_t user = 0) {
    valid = 1;
    stream_id = stream;
    task_id = task;
    reduce_seq = seq;
    user_id = user;
  }
  void Idle() {
    valid = 0;
    stream_id = 0;
    user_id = 0;
    task_id = 0;
    reduce_seq = 0;
    event = 0;
  }
  bool Valid() const { return valid.Get() != 0; }
};

// TS 内部：一笔写 stream_table 的请求。
//
// 写失败分两种：create、install 这类整项写入失败后要重读最新表内容再来；
// issue 这类只改一个字段的失败后只重试这一笔写，不能重新下发已经被 RV core
// 接收的任务。所以请求方要保持到 accepted 才算生效。
struct StreamWrite {
  bool valid = false;
  uint64_t stream_id = 0;

  // 整项写（create / install）用这个。
  bool whole = false;
  StreamEntry entry;

  // 改字段用这几个。
  bool set_fsm = false;
  TaskFsm fsm = TaskFsm::kIdle;
  bool set_done_bit = false;
  uint64_t done_bit = 0;
  bool set_reissue = false;
  // 自启动 core：rv_done 带回来的 user_id 补进表项。建表那一刻没有用户信息，
  // 软件认出这一笔属于哪个用户之后才有。
  bool set_user_id = false;
  uint64_t user_id = 0;
  // Router 的 CoreMem 重发完成后清掉这一项的重发标记。
  bool clear_reissue = false;
  bool clear_valid = false;
  // completion 口专用：只有这一笔的 task_id 等于该 stream 当前的 task_id 时
  // 才改 task_fsm，否则只亮 done_bitmap 一位。
  bool fsm_if_current = false;
  uint64_t from_task_id = 0;
};

// 一个写口。请求方每拍写 valid 与 req，Stream_table 读上一拍的值，处理完回
// accepted。请求要一直保持到看见 accepted 才算生效。
//
// 用 LogicPtr 搬请求而不是把字段摊平：一笔整项写含整个 StreamEntry，摊成几十个
// Logic64 既啰嗦又慢，而 LogicPtr 只搬一个 shared_ptr。
class StreamWritePortIf : public Logic {
 public:
  Logic64 valid;
  LogicPtr<StreamWrite> req;
  Logic64 accepted;
  // 一次握手最少两拍：请求方拉 valid，Stream_table 下一拍写 accepted，请求方
  // 再下一拍才看得到并撤 valid。这中间请求方那一拍不重写端口，Latch 就回落成
  // 上一拍的值，Stream_table 会把同一笔再执行一遍 —— 建表就变成推两次 tail。
  // 按序号认它。
  Logic64 seq;

  explicit StreamWritePortIf(ClockPtr c)
      : valid(c), req(c), accepted(c), seq(c) {
    Fields(valid, req, accepted, seq);
  }

  void Drive(std::shared_ptr<StreamWrite> w) {
    valid = 1;
    req = std::move(w);
    seq = ++issue_seq;
  }
  void Idle() {
    valid = 0;
    req = std::shared_ptr<StreamWrite>();
  }
  void DriveAccepted(bool ok) { accepted = ok ? 1 : 0; }
  bool Accepted() const { return accepted.Get() != 0; }
  bool Valid() const { return valid.Get() != 0; }
  uint64_t Seq() const { return seq.Get(); }
  std::shared_ptr<StreamWrite> Req() const { return req.Get(); }

 private:
  // 请求方自己的计数，不跨模块读，所以是普通成员。
  uint64_t issue_seq = 0;
};

// Stream_table 每拍末发布的整表快照。别的模块读上一拍的它来做判断，不去碰
// Stream_table 的内部容器 —— 那些只由它自己的 Step() 触碰。
struct StreamSnapshot {
  std::array<StreamEntry, kStreamNum> entry;
  uint64_t head_ptr = 0;
  uint64_t tail_ptr = 0;
  // 在途几项，装满时就是 kStreamNum。两个指针在快照里都对 kStreamNum 取过
  // 模，满与空都是两者相等，差值分不开，所以这个数单独带出来。上限是 CFG_REG
  // 里软件配的 stream_num，读的人自己去取。
  uint64_t in_flight = 0;

  uint64_t InFlight() const { return in_flight; }
  // 从 head_ptr 开始的环形年龄序：越靠近 head 越老。
  uint64_t AgeOrder(uint64_t k) const { return (head_ptr + k) % kStreamNum; }
};

using StreamSnapshotPtr = std::shared_ptr<StreamSnapshot>;

class SnapshotPort : public Logic {
 public:
  LogicPtr<StreamSnapshot> snap;

  explicit SnapshotPort(ClockPtr c) : snap(c) { Fields(snap); }

  void Drive(StreamSnapshotPtr s) { snap = std::move(s); }
  StreamSnapshotPtr Get() const { return snap.Get(); }
};

}  // namespace bach
}  // namespace latch

#endif
