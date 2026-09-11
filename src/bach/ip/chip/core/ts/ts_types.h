#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_TS_TYPES_
#define _LATCH_BACH_IP_CHIP_CORE_TS_TS_TYPES_

// TS 的两张表与它们的表项。
//
// task_chain 是静态的，说清这类 core 的操作流长什么样：一条全序链，位域里没有
// 前驱表也没有后继表，依赖信息只有数组下标。每项占两个 32 位寄存器，
// TASK_CHAIN_n_PC 与 TASK_CHAIN_n_ATTR。
//
// stream_table 是运行时的，说清每个在途用户走到了哪一步。进度由三个字段合起来
// 表示：task_id 是链上的第几步，task_fsm 是这一步的状态，done_bitmap 是 64 位
// 对应 64 个 task 做完没有，跳过的也算做完。异步 datain 提前完成就表现为
// done_bitmap 上某一位先亮而 task_id 还没走到那里。

#include <array>
#include <cstdint>

namespace latch {
namespace bach {

// 一条链最多 64 项。
constexpr uint64_t kTaskChainNum = 64;
// stream_table 16 项。
constexpr uint64_t kStreamNum = 16;
// 自启动 core 上 Bypass 那一路 datain 用的保留身份：不占 stream 表项，它回来的
// 完成 ACK 不更新任何表项。
constexpr uint64_t kBypassSid = 15;
constexpr uint64_t kBypassTid = 63;

// TASK_SEND_UNIT。
enum class SendUnit : uint32_t {
  kDte = 0,
  kMu = 1,
  kVu = 2,
};

// TASK_RECV_UNIT：这一笔要收齐哪几路完成 ACK。
enum class RecvUnit : uint32_t {
  kRvOnly = 0,  // 00：只调 RV core，RV core 的 ACK 到了就算完成
  kDsa = 1,     // 01：调 RV core 与 DSA，两路 ACK 都到才算完成
};

// TASK_TYPE。
enum class TaskType : uint32_t {
  kNormal = 0,
  kReissueOut = 1,      // Broadcast / P2P 重发的搬出任务
  kBcastReissueIn = 2,  // Broadcast 重发的搬入任务
  kP2pReissueIn = 3,    // P2P 重发的搬入任务
  kReduce = 4,          // 逐级 reduce 任务
  kPidUpdate = 5,       // 完成时带回新 PID，交给紧邻的后继任务
};

// task_fsm 的五个状态。
enum class TaskFsm : uint32_t {
  kIdle = 0,
  kWait = 1,     // 等外部数据唤醒，或等 credit
  kReady = 2,    // 前置条件齐了，待发射
  kInfly = 3,    // 已发射
  kFinish = 4,
};

// task_chain 的一项。位域照《Task Scheduler MAS》的 TASK_CHAIN_xx_PC / ATTR。
struct TaskEntry {
  bool valid = false;                      // ATTR[31]：写 ATTR 时硬件置起
  uint64_t task_pc = 0;                    // PC[31:0]
  SendUnit send_unit = SendUnit::kDte;     // ATTR[1:0]
  RecvUnit recv_unit = RecvUnit::kRvOnly;  // ATTR[3:2]
  bool wait_wake = false;                  // ATTR[4]
  TaskType task_type = TaskType::kNormal;  // ATTR[7:5]
  // ATTR[13:8]：重发搬入任务配对的那个搬出任务。
  uint64_t p2p_reissue_tid = 0;
  bool credit_en = false;                  // ATTR[14]
  uint64_t path_id = 0;                    // ATTR[22:15]
  bool end = false;                        // ATTR[23]

  bool IsDataIn() const { return wait_wake; }
  bool IsReduce() const { return task_type == TaskType::kReduce; }
  bool UsesDsa() const { return recv_unit == RecvUnit::kDsa; }
};

// 一项任务装进 stream 时的初始状态：要等外部数据唤醒的置 WAIT；要查 credit 的
// 也置 WAIT，credit 到了再置 READY；其余置 READY。
inline TaskFsm InitFsmOf(TaskEntry const& t) {
  if (t.wait_wake || t.credit_en) return TaskFsm::kWait;
  return TaskFsm::kReady;
}

// stream_table 的一项。
struct StreamEntry {
  // ── 用户级标记，建表写入，整条任务链期间基本不动 ──
  bool valid = false;
  // 用户号只有这一个：Router 与 credit 记账认它，软件读它算 R core 的用户映射
  // 表与 Matrix Mem 地址。普通计算 core 上建表时从 Router 请求里取，自启动的
  // core 上等 Task 0 的 RV core ACK 带回来。
  uint64_t user_id = 0;
  bool user_id_vld = false;
  bool reissue = false;

  // ── 进度三字段 ──
  uint64_t task_id = 0;
  TaskFsm task_fsm = TaskFsm::kIdle;
  uint64_t done_bitmap = 0;

  // ── 当前 task 的属性，每次更新 task_id 时从 task_chain 索引得到 ──
  SendUnit task_unit = SendUnit::kDte;
  RecvUnit task_recv = RecvUnit::kRvOnly;
  TaskType task_type = TaskType::kNormal;
  uint64_t task_pc = 0;
  // 当前任务实际用的 PID：取自 task_chain；上一笔是 PID 更新任务、这一笔又是它
  // 紧邻的后继时，继承它完成时带回的那个值。DTE 出核时包头里的 path 用它。
  uint64_t task_path_id = 0;
  // task_path_id 里存的是等紧邻后继继承的新 PID。
  bool pid_pending = false;
  bool end = false;

  // 本级 Rmem 的 credit，每个用户一份：发出一笔 reduce 就占掉，Rmem 做完那一笔
  // 报回来才还。
  bool rmem_busy = false;
};

// 把 task_chain 的一项摊进表项。PID 由调用方按继承规则再定。
inline void ApplyTaskAttr(StreamEntry& e, TaskEntry const& t) {
  e.task_unit = t.send_unit;
  e.task_recv = t.recv_unit;
  e.task_type = t.task_type;
  e.task_pc = t.task_pc;
  e.task_path_id = t.path_id;
  e.end = t.end;
}

// 八个写口，按来源命名。优先级由高到低就是这个顺序：让表项先腾空再填新的，
// 回收类排在生成类前面，create 排最后，队头卡住时不会因为新用户不断插队
// 而饿死。
// issue 这一类有三个物理实例：三条发射通路各自独立打拍，同一拍可以并行下发
// 3 个 task，各自都要回写 READY → INFLY。三个实例优先级相同，挨在一起排。
enum StreamWritePort : uint32_t {
  kWrRetirement = 0,  // 清 valid 并推 head_ptr
  kWrCompletion = 1,  // 完成事件
  kWrInstall = 2,     // Task_ctrl 生成后继，整项写
  kWrIssueDte = 3,    // 发射通路收到 ACCEPT 后 READY → INFLY
  kWrIssueMu = 4,
  kWrIssueVu = 5,
  kWrCreditWake = 6,  // credit 到了置 READY
  kWrCreate = 7,      // User_Match 建表或补跳过位
  kWrPortNum = 8,
};

}  // namespace bach
}  // namespace latch

#endif
