#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_DTE_TYPES_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_DTE_TYPES_

// DTE 的任务模型。
//
// DTE 只做搬运，不做计算。难点在两端的节奏对不上：Router 一侧流式到达、什么时候
// 来由上游决定；存储一侧要过 DMA_XBAR 抢 bank。做法是把一个搬运任务从中间劈开，
// 读一半、写一半各自排队各自推进，中间用 buffer 顶住速度差，完成时按 task_id
// 合回一次 task_done。
//
// 任务统一从 RV core 的配置入口来：DTE RV core 经寄存器写加 Trigger 生成
// Descriptor，进中央 TaskQueue，再在 Commit 边界按通道资源 dispatch 成同一套内部
// 任务模型。
//
// 对齐飞书《DTE DSA》文档后，任务模型是 4 个通用段位：段 0 唯一与用途绑定 =
// 包头（core_mask 2B + Hardware Used 1B + 软件包头 16B），段 1~3 通用，内容由软件
// 约定，DTE 不区分。各段端点属于 Mmem / Cmem / topK_table / header_table 的哪一个，
// 由该段地址所在的地址范围译码决定（同一个 Task 的不同段可以落在不同存储上）。

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

// 每个通道读写两半各一个 TaskQueue，深度 4（《DTE DSA》：WR Lane TaskQ 深度 4，
// 允许 Read Lane 先执行 4 个任务）。一笔任务进通道时两半同时入队，所以两半取同一
// 深度。
constexpr uint64_t kTaskQueueDepth = 4;
// 中央 TaskQueue 16 项，排在 Commit 之前：保存“已快照、尚未 dispatch”的完整
// TaskDesc，不同通道的任务可乱序下发（对齐飞书《DTE DSA》）。
constexpr uint64_t kCentralTaskQDepth = 16;
// Completion RS 与 Done Pending 各 16 项。
constexpr uint64_t kCompRsNum = 16;
constexpr uint64_t kDonePendDepth = 16;
// 一个通道一份中间 Buffer，8 KB，按 256 B 一项算 32 项，最大可掩盖 32 T 的
// 访存延迟。五个通道各一份。
constexpr uint64_t kDteBufFlits = 32;
// 单个 DTE 任务的搬运量上限：256 B × 128 拍 = 32 KB。超过的要拆成多个任务包。
constexpr uint64_t kMaxTaskBytes = 32 * 1024;

// 一个段位的端点，由地址范围译码决定段落在哪块存储。
enum class SegEndpoint : uint32_t {
  kCmem = 0,     // CoreMem 数据
  kMmem = 1,     // MatrixMem 数据
  kScale = 2,    // CoreMem scale 旁带（地址低位给对应数据地址）
  kTopk = 3,     // MU topK_table（按 stream_id 索引写 MU）
  kHeader = 4,   // header_table（DTE 的 Hmem，按 stream_id 索引）
};

// 端点地址范围译码的位约定：地址高 4 bit 作端点 tag，低位为端内偏移。飞书文档只
// 说“由地址范围译码”未给具体数值，这里是建模约定（见 04-dte 建模文档）。段地址
// 存在 Segment::src / dst 里时可能带 tag，展开成实际地址时用 kEpDataMask 剥掉。
constexpr uint64_t kEpShift = 28;
constexpr uint64_t kEpMask = 0xF;
constexpr uint64_t kEpDataMask = (1ull << kEpShift) - 1;

// 一个段位。地址公式 stream_start_i = CFG_ADDRi + SID × CFG_STRIDEi，stride 配 0
// 退化为纯物理地址；route 100（Mmem→Cmem）源端不叠 stride（Mmem 侧软件给物理地址）。
struct Segment {
  bool valid = false;
  // 段 0 恒为包头：永远不占 payload 数据通道。它的端点译码只决定这段包头落哪块
  // 存储——tag 0x4 落 header_table（计算 core 用 Hmem），tag 0x0 落 Core Mem
  // （B/R core 用 Core Mem）。其余段 false。
  bool is_header = false;
  uint64_t src = 0;      // CFG_ADDRi_SRC（软件基址）/ 进核包头落点
  uint64_t dst = 0;      // CFG_ADDRi_DST
  uint64_t stride = 0;   // CFG_STRIDEi
  uint64_t len = 0;      // CFG_DATA_LENi，字节
  SegEndpoint src_kind = SegEndpoint::kCmem;   // 源端地址范围译码
  SegEndpoint dst_kind = SegEndpoint::kCmem;   // 目的端地址范围译码
  // 展开后的实际地址（Fire / 解析时由 agcu 算好，lane 直接用）。
  uint64_t src_addr = 0;
  uint64_t dst_addr = 0;
};

// 一个高层任务。Commit 把它劈成一对 RD / WR 子上下文。
struct Descriptor {
  bool valid = false;
  // Commit 准入时分配的内部序号。Completion RS 按它把劈开的两半合回来。
  // 业务上的 task_id 只在一个 stream 内唯一，同一拍在途的两笔任务可以带同一个
  // 值：一笔是 Router 送进来的搬入，另一笔是 RV core 配的搬出。
  uint64_t commit_seq = 0;
  // 进核任务按到达顺序编的帧号：配置驱动的第 N 个进核任务对应第 N 个到达的数据包
  // （FIFO），Lane 在准入时给进核任务编这个号，与 Header Parser 给每帧编的号对齐，
  // 进核那一路用它认“这几拍属于哪一帧”。出核任务不用（走 commit_seq）。
  uint64_t frame_seq = 0;
  uint64_t task_id = 0;
  uint64_t stream_id = 0;
  uint64_t user_id = 0;
  uint64_t path_id = 0;
  Route route = Route::kRouterToCm;

  // 4 个通用段位。段 0 与包头绑定，段 1~3 通用。
  Segment seg[4];

  uint64_t vc = 0;           // 出核走哪个 VC，决定落在哪个 out_ch

  // ack_ts_en 对应 CFG_TRANS_MODE[9]，飞书《DTE DSA》详细设计里就叫 ack_ts_en
  // （MAS 旧命名 task_last）：标记一个 task 拆成几笔搬运时的最后一笔，只有带这个
  // 标记的那一笔完成后才通知 TS；不通知 TS 的任务把这一位清 0。
  bool ack_ts_en = true;
  // reduce 包的任务边界：发方的 task_id，ReduceModule 靠它分开同一个用户前后
  // 两笔 reduce 任务。
  uint64_t reduce_seq = 0;

  // shareMem 写：数据搬完之后按这一对写一笔，写出去了才通知 TS。只有 B core
  // 与 R core 用，存的是 user_id 与 token entry 的 valid 标志。对应 CFG_TRANS_MODE[8]
  // 的 wr_sharemem_flag + CFG_SM_W_ADDR / CFG_SM_W_DATA。
  bool wr_sharemem_flag = false;
  uint64_t smem_addr = 0, smem_data = 0;

  // 进核任务带着原包，出核任务带着要发出去的包。
  MessagePtr msg;

  // 出核包里接进 size 的段长之和：数据段（Cmem/Mmem）+ scale 段 + topK 段。
  // topK 也走数据通道（飞书《DTE DSA》里它是包里的 k 数据段，与数据/scale 同一条
  // 256B/T 通道），所以算进 size 的 flit 换算；但它的字节不走 payload 正文，随包的
  // topk 字段走，由 DTE 经专用数据线写进 MU 的 topK_ep_table。
  uint64_t PayloadBytes() const {
    uint64_t n = 0;
    for (auto const& s : seg) {
      if (!s.valid || s.is_header) continue;
      if (s.src_kind == SegEndpoint::kCmem ||
          s.src_kind == SegEndpoint::kMmem ||
          s.src_kind == SegEndpoint::kScale ||
          s.src_kind == SegEndpoint::kTopk) {
        n += s.len;
      }
    }
    return n;
  }
  // 出核包 payload 正文的字节数：数据段（Cmem/Mmem）+ scale 段，不含 topK。topK 的
  // 字节随包的 topk 字段走，不占 payload 正文，但仍算进 PayloadBytes()（size）。
  uint64_t PayloadDataBytes() const {
    uint64_t n = 0;
    for (auto const& s : seg) {
      if (!s.valid || s.is_header) continue;
      if (s.src_kind == SegEndpoint::kCmem ||
          s.src_kind == SegEndpoint::kMmem ||
          s.src_kind == SegEndpoint::kScale) {
        n += s.len;
      }
    }
    return n;
  }
  // topK 段的字节数：进核按 dst、出核按 src 取，同一段只有一端打 TOPK tag。
  uint64_t TopkBytes() const {
    for (auto const& s : seg) {
      if (s.valid && (s.src_kind == SegEndpoint::kTopk ||
                      s.dst_kind == SegEndpoint::kTopk)) {
        return s.len;
      }
    }
    return 0;
  }
  // 是否有 scale 段（出核造包据此打 scale_valid）。
  bool HasScale() const {
    for (auto const& s : seg) {
      if (s.valid && (s.src_kind == SegEndpoint::kScale ||
                      s.dst_kind == SegEndpoint::kScale)) {
        return true;
      }
    }
    return false;
  }

  // 是否有 topK 段（进核按 dst 写 MU、出核按 src 读 MU，两边都算）。
  bool HasTopk() const {
    for (auto const& s : seg) {
      if (s.valid && (s.src_kind == SegEndpoint::kTopk ||
                      s.dst_kind == SegEndpoint::kTopk)) {
        return true;
      }
    }
    return false;
  }

  // topK 段落在 MU topK_ep_table 的哪一项：取该段地址的端内偏移（低位）。进核取
  // dst 段、出核取 src 段，同一段只有一端打 TOPK tag。计算 core 写 stream_id，
  // B core 写 user_id，都是表里 1024 份中的一个下标。
  uint64_t TopkIndex() const {
    for (auto const& s : seg) {
      if (s.valid && s.dst_kind == SegEndpoint::kTopk) {
        return s.dst & kEpDataMask;
      }
      if (s.valid && s.src_kind == SegEndpoint::kTopk) {
        return s.src & kEpDataMask;
      }
    }
    return 0;
  }

  // 出核包要带的目的地址：数据段（Cmem/Mmem）的 CFG_ADDRi_DST，收方据此落点。
  // dst 存的是带端点 tag 的软件值，剥掉 tag 才是收方认识的落点（收方按包头的
  // dst_addr 当纯偏移用，tag 是本地存储译码的约定，不进包）。
  uint64_t DataDst() const {
    for (auto const& s : seg) {
      if (s.valid && !s.is_header &&
          (s.src_kind == SegEndpoint::kCmem ||
           s.src_kind == SegEndpoint::kMmem)) {
        return s.dst & kEpDataMask;
      }
    }
    return 0;
  }

  // 这个任务落在哪个通道上。
  uint64_t Lane() const {
    if (IsInbound(route)) return kInCh;
    if (route == Route::kMmToCm) return kInnerLane;
    return kOutCh0 + (vc % 4);
  }
};

}  // namespace bach
}  // namespace latch

#endif
