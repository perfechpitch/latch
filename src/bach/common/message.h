#ifndef _LATCH_BACH_COMMON_MESSAGE_
#define _LATCH_BACH_COMMON_MESSAGE_

// 一个整包。链路上按 flit 传，同一个 Message 的多个 flit 共享这一份，flit 里挂
// 的是 LogicPtr<Message>，Push 只搬 shared_ptr，不深拷 payload 字节。
//
// 片内包头字段照 Router《DATA_NOC_DE_HAS》「消息格式 (Header field)」节（DTE 详
// 细设计的 3.14 包头格式同）：path_id(Byte0)、core_mask(Byte2-3)、loopback(Byte5,
// [3:2]=vcid)、pkt_length(Byte6-7)、user_id(Byte8-9)、sw_aux(Byte16-31)。Header
// Parser 取 path_id、path_core_mask、user_id、size、vc，按 path_id 查 RouterTable。
// vc 每一跳都被改写（下一跳的 VC 编码在包头 loopback[3:2] 里，本跳 Router 读表后
// 改写它），所以它是 Message 的可变字段，不是 flit 上的只读副本。
//
// Router 不感知 payload 内容，只有 reduce 包例外：软件辅助信息固定 16 B，做加法
// 时跳过这一段（kReduceSwHeaderBytes）。

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

// reduce 包最前面这一段是软件辅助信息，Router 加法时跳过。
constexpr uint64_t kReduceSwHeaderBytes = 16;

// 最短包长：只含包头与路由信息的空包。
constexpr uint64_t kMinPacketBytes = 16;

struct Message {
  // ── 包头 ──
  uint64_t user_id = 0;
  uint64_t path_id = 0;
  // 2 B 位图，最多给 16 个核分位。core 在自己的表项里指定看哪一位。
  uint64_t path_core_mask = 0;
  uint64_t size = 0;           // 数据通道上的字节数（含 topK），收齐判定按它
  uint64_t vc = 0;             // 包头 loopback[3:2]，每一跳被改写
  uint64_t reduce_seq = 0;     // TS 按它给 reduce task 配对
  uint64_t reissue = 0;        // 这一笔是不是 Core Mem 重发出来的
  // 这个用户在目的 core 上占的槽位。各 core 按到达顺序环形分配，分出来的号
  // 因此一致，收方的 DTE 解析出来直接用。
  uint64_t stream_id = 0;
  // 发方那一笔是它自己任务链上的第几步。收方不用它：收方按 path_id 查自己
  // 的 path_task_map 得出这一笔在本地链上的位置。
  uint64_t task_id = 0;

  // ── 片外（DPU/PCIe）自定义包头 ──
  // 不在片内 Router/DTE 的 32B 包头（3.14）里。入口桩封包时写 gpu_id(8) +
  // token_id(16)，出口桩按这一对开重组缓冲、与参考实现比对，入口桩按它记 retired。
  uint64_t gpu_id = 0;
  uint64_t token_id = 0;

  // 片外寻址：PCIe Switch 按 (gpu_id, dst) 查目的端口集合。dst 是 LPU 侧的目的
  // 标识，阵列内部靠 path_id 走，这一项只在片外那一段有意义，不进片内 32B 包头。
  uint64_t dst = 0;

  // 片外进核那一笔的落点。出核造包时从任务的 CFG_ADDRi_DST 抄过来（commit.h 的
  // MakeOutboundMsg）；片内 DTE 的实际落点已改由 CFG_ADDRx_DST 配置寄存器给，包
  // 头这个字段只作片外入站寻址辅助信息，不参与对齐检查（见 header_parser.h）。
  uint64_t dst_addr = 0;

  // payload 是 MXFP8 数据后面接它的 scale（每 32 B 一个 E8M0）。发方 DTE 的任务
  // 置了 scale_valid 时打上，收方据此把包尾那一段写进存储的 scale 旁带。
  uint64_t scale_valid = 0;

  // MoE 进核包自带的 topK 表（256 B）。topk_valid 置位时收方 DTE 不落 Core Mem，
  // 而是经专用数据线按 stream_id 写进 MU 的 topK_ep_table。与数据/scale 同一条
  // 数据通道，算进 size 的 flit 换算；字节不走 payload 正文，单独放在 topk 字段里。
  uint64_t topk_valid = 0;
  std::vector<uint8_t> topk;

  // 诊断用：从哪个 core 发出、第几笔。模型不拿它当主键，硬件包头里也没有。
  uint64_t src_core = 0;
  uint64_t seq = 0;

  // ── payload ──
  // 这一轮不建 numeric，payload 当不透明字节搬运。
  std::vector<uint8_t> payload;

  uint64_t TotalBytes() const {
    return size > kMinPacketBytes ? size : kMinPacketBytes;
  }
  // payload 里数据那一段的长度。带 scale 时 size = D + ceil(D / 32)，由此反推 D。
  uint64_t DataBytes() const {
    return scale_valid != 0 ? size - (size + 32) / 33 : size;
  }
};

using MessagePtr = std::shared_ptr<Message>;

}  // namespace bach
}  // namespace latch

#endif
