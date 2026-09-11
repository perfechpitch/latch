#ifndef _LATCH_BACH_COMMON_MESSAGE_
#define _LATCH_BACH_COMMON_MESSAGE_

// 一个整包。链路上按 flit 传，同一个 Message 的多个 flit 共享这一份，flit 里挂
// 的是 LogicPtr<Message>，Push 只搬 shared_ptr，不深拷 payload 字节。
//
// 包头字段照 Router 那份文档的 F1、F53、F103、F105：Header Parser 取 path_id、
// path_core_mask、user_id、size、vc_id，按 path_id 查 RouterTable。vc 每一跳都
// 会被改写（F92：下一跳的 VC 编码在包头里，本跳 Router 读表后改写它），所以它
// 是 Message 的可变字段，不是 flit 上的只读副本。
//
// Router 不感知 payload 内容（F106），只有 reduce 包例外：软件辅助信息固定 16 B，
// 做加法时跳过这一段（F105）。

#include <cstdint>
#include <memory>
#include <vector>

namespace latch {
namespace bach {

// RouterTable 的 operation 字段：0 转发、1 Reduce0、2 Reduce1、3 Reduce2。
// 三档 reduce 的含义按建模计划的待定项取：源分量 / 中继累加 / 最终汇聚。
enum class Operation : uint32_t {
  kForward = 0,
  kReduce0 = 1,
  kReduce1 = 2,
  kReduce2 = 3,
};

// RouterTable 的 op_type 字段。
enum class OpType : uint32_t {
  kWeight = 0,     // kernel 与 weight 搬运
  kTransfer = 1,
  kReduce = 2,
  kReduceTwice = 3,
};

// reduce 包最前面这一段是软件辅助信息，Router 加法时跳过（F105）。
constexpr uint64_t kReduceSwHeaderBytes = 16;

// 最短包长：只含包头与路由信息的空包（F103）。
constexpr uint64_t kMinPacketBytes = 16;

struct Message {
  // ── 包头 ──
  uint64_t user_id = 0;
  uint64_t path_id = 0;
  // 2 B 位图，最多给 16 个核分位（F100）。core 在自己的表项里指定看哪一位。
  uint64_t path_core_mask = 0;
  uint64_t size = 0;           // payload 字节数，收齐判定按它（F29）
  uint64_t vc = 0;             // 每一跳被改写（F92）
  uint64_t reduce_seq = 0;     // TS 按它给 reduce task 配对（F48）
  uint64_t reissue = 0;        // 这一笔是不是 Core Mem 重发出来的
  // 这个用户在目的 core 上占的槽位。各 core 按到达顺序环形分配，分出来的号
  // 因此一致，收方的 DTE 解析出来直接用。
  uint64_t stream_id = 0;
  // 发方那一笔是它自己任务链上的第几步。收方不用它：收方按 path_id 查自己
  // 的 path_task_map 得出这一笔在本地链上的位置。
  uint64_t task_id = 0;

  // ── DPU 写的自定义包头 ──
  // 入口桩封包时写 gpu_id(8) + token_id(16)。出口桩按这一对与参考实现比对，
  // 入口桩按它记 retired。
  uint64_t gpu_id = 0;
  uint64_t token_id = 0;

  // PCIe Switch 按 (gpu_id, dst) 查目的端口集合。dst 是 LPU 侧的目的标识，
  // 阵列内部靠 path_id 走，这一项只在片外那一段有意义。
  uint64_t dst = 0;

  // 收方把这一包搬进哪里。DTE 的 Header Parser 解析包头就按它建搬运描述符，
  // 落 Core Mem 时收方再叠自己的 stream 偏移，落 Matrix Mem 时直接用。发方
  // 出核造包时从 DTE 模板的 dst_addr 抄过来（F2 的入站包头字段）。
  uint64_t dst_addr = 0;

  // 诊断用：从哪个 core 发出、第几笔。模型不拿它当主键，硬件包头里也没有。
  uint64_t src_core = 0;
  uint64_t seq = 0;

  // ── payload ──
  // 这一轮不建 numeric，payload 当不透明字节搬运。
  std::vector<uint8_t> payload;

  uint64_t TotalBytes() const {
    return size > kMinPacketBytes ? size : kMinPacketBytes;
  }
};

using MessagePtr = std::shared_ptr<Message>;

}  // namespace bach
}  // namespace latch

#endif
