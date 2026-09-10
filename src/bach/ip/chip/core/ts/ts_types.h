#ifndef _LATCH_BACH_IP_CHIP_CORE_TS_TS_TYPES_
#define _LATCH_BACH_IP_CHIP_CORE_TS_TS_TYPES_

// TS 的两张表与它们的表项。
//
// task_chain 是静态的，说清这类 core 的操作流长什么样：一条全序链，位域里没有
// 前驱表也没有后继表，依赖信息只有数组下标。
//
// stream_table 是运行时的，说清每个在途用户走到了哪一步：进度由三个字段合起来
// 表示 —— task_id 是链上的第几步，task_fsm 是这一步的状态，done_bitmap 是 64 位
// 对应 64 个 task 的数据齐没齐。异步 datain 提前完成就表现为 done_bitmap 上某一位
// 先亮而 task_id 还没走到那里。

#include <array>
#include <cstdint>

namespace latch {
namespace bach {

// 一条链最多 64 项。
constexpr uint64_t kTaskChainNum = 64;
// stream_table 16 项。
constexpr uint64_t kStreamNum = 16;

// task_chain 项的 TASK_SEND_UNIT。
enum class SendUnit : uint32_t {
  kDte = 0,
  kMu = 1,
  kVu = 2,
};

// task_chain 项的 TASK_RECV_UNIT：这一笔的完成由谁收尾。
enum class RecvUnit : uint32_t {
  kRvOnly = 0,     // 只调 RV core，RV core 收尾
  kDsa = 1,        // 调 DSA，DSA 收尾
  kDteDsaRmem = 2, // DTE DSA 加 Router 的 Rmem，两边都要
};

// task_fsm 的五个状态。
enum class TaskFsm : uint32_t {
  kIdle = 0,
  kWait = 1,     // 等前置条件：datain 数据、credit、或前序完成
  kReady = 2,    // 前置条件齐了，待发射
  kInfly = 3,    // 已发射
  kFinish = 4,
};

// task_chain 的一项。位域照《Task Scheduler MAS》的 Task_chain0-63。
struct TaskEntry {
  bool valid = false;
  uint64_t task_pc = 0;
  SendUnit send_unit = SendUnit::kDte;
  RecvUnit recv_unit = RecvUnit::kRvOnly;
  bool self_start = false;
  bool wait_wake = false;
  bool broadcast_reissue = false;
  bool p2p_reissue = false;
  bool reduce = false;
  bool credit_en = false;
  // 极性照寄存器定义：1 = 这个 task 不按用户区分，所有用户都做；
  // 0 = 按用户区分，只有 compute = 1 的用户做。
  bool exe_mask = true;
  uint64_t path_id = 0;
  bool end = false;

  // 硬件位域里没有的软件侧属性，从软件侧任务链表另行读入，与硬件表分开存。
  uint64_t exe_dest = 0;
  uint64_t reduce_num = 0;
  bool dsa_en = false;

  bool IsDataIn() const { return wait_wake; }
  bool IsReissue() const { return broadcast_reissue || p2p_reissue; }
};

// stream_table 的一项。
struct StreamEntry {
  // ── 用户级标记，建表写入，整条任务链期间基本不动 ──
  bool valid = false;
  // 用户号只有这一个：Router 与 credit 记账认它，软件读它算 R core 的用户映射
  // 表与 Matrix Mem 地址。普通计算 core 上建表时从 Router 请求里取，自启动的
  // core 上等软件认出用户之后随完成补进来。
  uint64_t user_id = 0;
  bool user_id_vld = false;
  bool reissue = false;
  bool compute = false;

  // ── 进度三字段 ──
  uint64_t task_id = 0;
  TaskFsm task_fsm = TaskFsm::kIdle;
  uint64_t done_bitmap = 0;

  // ── 当前 task 的属性，每次更新 task_id 时从 task_chain 索引得到 ──
  SendUnit task_unit = SendUnit::kDte;
  RecvUnit task_recv = RecvUnit::kRvOnly;
  bool task_dsa_en = false;
  uint64_t task_pc = 0;
  // DSA 出核时包头里的 path 字段用它。DTE 内不再存 path_id_table：这一路由号
  // 由 TS 随 task 一起送到 DSA。
  uint64_t task_path_id = 0;
  bool is_reissue = false;
  bool end = false;

  // reduce 任务的两半配对：包头带 reduce_seq，DTE 发出时打上、Router 的
  // Reduce Done 原样带回。只有两张位图的低 N 位全满才置 FINISH。
  uint64_t reduce_num = 0;
  uint64_t reduce_issued = 0;
  uint64_t dte_ack_map = 0;
  uint64_t router_done_map = 0;
};

// 六个写口，按来源命名。优先级由高到低就是这个顺序：让表项先腾空再填新的，
// 回收类排在生成类前面，create 排最后 —— 队头卡住时不会因为新用户不断插队
// 而饿死。
// MAS 数的是六类口，其中 issue 这一类有三个物理实例 —— 三条发射通路各自独立
// 打拍，同一拍可以并行下发 3 个 task，各自都要回写 READY → INFLY。三个实例
// 优先级相同，挨在一起排。
enum StreamWritePort : uint32_t {
  kWrRetirement = 0,  // 清 valid 并推 head_ptr
  kWrCompletion = 1,  // 三条 Completion Lane
  kWrInstall = 2,     // Task_ctrl 生成后继，整项写
  kWrIssueDte = 3,    // 发射通路收到 ACCEPT 后 READY → INFLY
  kWrIssueMu = 4,
  kWrIssueVu = 5,
  kWrCreditWake = 6,  // credit 到了置 READY
  kWrCreate = 7,      // User_Match 建表，整项写
  kWrPortNum = 8,
};

}  // namespace bach
}  // namespace latch

#endif
