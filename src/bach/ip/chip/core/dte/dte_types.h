#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_DTE_TYPES_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_DTE_TYPES_

// DTE 的任务模型。
//
// DTE 只做搬运，不做计算。难点在两端的节奏对不上：Router 一侧流式到达、什么时候
// 来由上游决定；存储一侧要过 DMA_XBAR 抢 bank。做法是把一个搬运任务从中间劈开，
// 读一半、写一半各自排队各自推进，中间用 buffer 顶住速度差，完成时按 task_id
// 合回一次 task_done。
//
// 任务从两个入口来，都在 Commit 边界汇成同一套内部任务模型：Router 入站帧的
// Header 经 Header Parser 生成 Descriptor；DTE RV core 经寄存器写加 Trigger
// 生成 Descriptor。

#include <cstdint>
#include <memory>

#include "bach/common/message.h"

namespace latch {
namespace bach {

// 五种搬运方向。CM → MM 本版本不支持：XBar 不提供 WR_CH1 到 Matrix Memory 的连接。
// 编码与 transfer_mode 那三位一样：000 router→Cmem、001 router→Mmem、
// 010 Cmem→router、011 Mmem→router、100 Mmem→Cmem。
enum class Route : uint32_t {
  kRouterToCm = 0,
  kRouterToMm = 1,
  kCmToRouter = 2,
  kMmToRouter = 3,
  kMmToCm = 4,     // 固定复用 out_ch[3]
};

inline bool IsInbound(Route r) {
  return r == Route::kRouterToMm || r == Route::kRouterToCm;
}

// 五个物理通道：一个进核通道，四个出核通道与 Router 的四个 VC 一一对应。
constexpr uint64_t kInCh = 0;
constexpr uint64_t kOutCh0 = 1;
constexpr uint64_t kLaneNum = 5;
// MM → CM 不另开通道，固定占 out_ch[3]，它的目的端 MUX 到 Core Mem。
constexpr uint64_t kInnerLane = kOutCh0 + 3;

// 通道的读写两半。
enum LaneHalf : uint64_t {
  kRd = 0,
  kWr = 1,
  kHalfNum = 2,
};

// TaskQueue 深度不少于 16，与 TS 的 16 个 stream 对齐。
constexpr uint64_t kTaskQueueDepth = 16;
// Completion RS 与 Done Pending 各 16 项。
constexpr uint64_t kCompRsNum = 16;
constexpr uint64_t kDonePendDepth = 16;
// PendingTaskQ 16 项，排在 Commit 之前。
constexpr uint64_t kPendingTaskQDepth = 16;
// 一个通道一份中间 Buffer，8 KB，按 256 B 一项算 32 项，最大可掩盖 32 T 的
// 访存延迟。五个通道各一份。
constexpr uint64_t kDteBufFlits = 32;
// 单个 DTE 任务的搬运量上限：256 B × 128 拍 = 32 KB。超过的要拆成多个任务包。
constexpr uint64_t kMaxTaskBytes = 32 * 1024;

// CFG_DATA_LEN 是 16 bit，写进去的数以 8 B 为一格，所以一段最长 64 KB 差 8 B。
// 软件配的是格数，硬件乘回字节。
constexpr uint64_t kDteDataLenGrain = 8;
constexpr uint64_t kDteDataLenMax = 0xFFFFu;

// 一个高层任务。Commit 把它劈成一对 RD / WR 子上下文。
struct Descriptor {
  bool valid = false;
  // Commit 准入时分配的内部序号。Completion RS 按它把劈开的两半合回来 ——
  // 业务上的 task_id 只在一个 stream 内唯一，同一拍在途的两笔任务可以带同一个
  // 值：一笔是 Router 送进来的搬入，另一笔是 RV core 配的搬出。
  uint64_t commit_seq = 0;
  // Header Parser 给每一帧编的号。进核那一路用它认「这几拍属于哪一帧」：
  // task_id 只说这一笔是任务链上的第几步，同一个 path 上连着来的几个包带的
  // 是同一个值。
  uint64_t frame_seq = 0;
  uint64_t task_id = 0;
  uint64_t stream_id = 0;
  uint64_t user_id = 0;
  uint64_t path_id = 0;
  Route route = Route::kRouterToCm;

  uint64_t src_addr = 0;
  uint64_t dst_addr = 0;
  uint64_t bytes = 0;
  uint64_t vc = 0;           // 出核走哪个 VC，决定落在哪个 out_ch

  // task_last 标记一个 task 拆成几笔搬运时的最后一笔，只有带这个标记的那一笔
  // 完成后才通知 TS；no_ack 置位的任务不回 Ack。
  bool task_last = true;
  bool no_ack = false;
  uint64_t reduce_seq = 0;   // reduce 包出核时打上，供 TS 逐包配对

  // shareMem 写：数据搬完之后按这一对写一笔，写出去了才通知 TS。只有 B core
  // 与 R core 用，存的是 user_id 与 token entry 的 valid 标志。
  bool smem_wr = false;
  uint64_t smem_addr = 0, smem_data = 0;

  // 进核任务带着原包，出核任务带着要发出去的包。
  MessagePtr msg;

  // 这个任务落在哪个通道上。
  uint64_t Lane() const {
    if (IsInbound(route)) return kInCh;
    if (route == Route::kMmToCm) return kInnerLane;
    return kOutCh0 + (vc % 4);
  }
};

// 完成的六个层级。向 TS 的报告是 exactly-once。
enum class CompState : uint32_t {
  kQueued = 0,     // 已进 TaskQueue 未装载
  kActive = 1,     // 由对应 AGCU / Ctrl 执行
  kIssueDone = 2,  // 该侧最后一个请求已 Fire
  kDrained = 3,    // 相关响应、Buffer 数据和外部副作用均已收敛
  kJoinDone = 4,   // 同一 task_id 的 RD 与 WR 都满足
  kTaskDone = 5,   // 进 Done Pending 并与 TS 成功握手
};

}  // namespace bach
}  // namespace latch

#endif
