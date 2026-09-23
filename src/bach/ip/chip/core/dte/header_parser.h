#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_HEADER_PARSER_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_HEADER_PARSER_

// Header Parser：Router 入站帧的数据通路单元。
//
// 对齐飞书《DTE DSA》：进核是配置驱动——任务由 RV core 配 CFG 寄存器 + trigger 起，
// 落点由软件配 CFG_ADDRx_DST；Header Parser 只在包头阶段使能，负责帧边界判定、
// 合法性检查，把包头上下文（core_mask / Hardware Used / 软件包头 / gpu_id /
// token_id）存进 Header Table，再逐拍转发 payload 给进核通道的读侧。它不再生成
// Descriptor。
//
// 首拍固定是 Header，靠「上一帧 TLAST 已接受」判断下一拍是新 Header，不依赖
// Start-of-Frame 信号。一帧一任务，不允许任务间交织。TLAST 标识最后一个 Payload
// beat；byte_count 为 0 时可由 Header beat 同时携带 TLAST。
//
// 非法 Header 进 Drop Frame：只消费到 TLAST 以恢复帧边界。丢的是这一帧，不是把
// 通道卡死。

#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/ip/chip/core/dte/hmem.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class HeaderParser : public BachModule {
 public:
  HeaderParser(ClockPtr clock, const std::string& name, Hmem& tables,
               uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        hmem(tables),
        from_router(std::make_shared<CoreDataPort>(clock)),
        payload(std::make_shared<PayloadPort>(clock)),
        parsed(clock),
        dropped(clock),
        beats(clock) {}

  CoreDataPort& FromRouter() { return *from_router; }
  void AttachFromRouter(std::shared_ptr<CoreDataPort> p) {
    from_router = std::move(p);
  }

  // Payload 那些拍交给进核通道的 RD 侧，由它写进 inbound buffer。走端口，不直接碰
  // 对方的容器。
  std::shared_ptr<PayloadPort> PayloadPtr() const { return payload; }

  uint64_t Parsed() const { return parsed.Get(); }
  uint64_t Dropped() const { return dropped.Get(); }
  uint64_t Beats() const { return beats.Get(); }

  bool Quiescent() const override { return !in_frame && !dropping; }

 protected:
  void Step() override {
    payload_driven = false;
    Handle();
    if (!payload_driven) payload->Idle();
    Commit();
  }

 private:
  void Commit() {
    parsed = parse_pending;
    dropped = drop_pending;
    beats = beat_pending;
    TracePerCycle("parsed", parse_pending);
  }

  void Handle() {
    CoreDataView d = ReadCoreData(*from_router);
    if (!d.valid) {
      from_router->DriveReady(true);
      return;
    }
    // 同一拍数据会连着两拍出现在端口上，按序号认它。
    if (d.seq == last_seq) {
      from_router->DriveReady(true);
      return;
    }

    // Buffer 满时通过 TREADY 向 Router 反压，本拍不收。这一步要排在
    // 「记下已见过这一笔」之前，否则这一笔会被当成收过了，上游换下一笔，
    // 数据就丢了。下游收不下由 payload 口的 ready 反映。
    if (in_frame && !payload->Ready()) {
      from_router->DriveReady(false);
      return;
    }

    last_seq = d.seq;
    ++beat_pending;

    if (dropping) {
      // Drop Frame：只消费到 TLAST 以恢复帧边界。
      if (d.last) dropping = false;
      from_router->DriveReady(true);
      return;
    }

    if (in_frame) {
      // Payload beat：累计 TKEEP 的有效字节，TLAST 时与 byte_count 比较。
      payload->Drive(cur_frame, d.bytes, keep_sum, d.last, d.msg);
      keep_sum += d.bytes;
      payload_driven = true;
      if (d.last) {
        LOGCHECK(keep_sum >= byte_count,
                 "HeaderParser: TLAST 时累计的有效字节少于包头写的 byte_count。");
        in_frame = false;
        keep_sum = 0;
      }
      from_router->DriveReady(true);
      return;
    }

    // 首拍是 Header。
    if (!Legal(d)) {
      ++drop_pending;
      // byte_count 为 0 时 Header beat 可以同时带 TLAST，那就没有要丢的了。
      dropping = !d.last;
      from_router->DriveReady(true);
      return;
    }

    // 存包头上下文进 Header Table：硬件改的 core_mask 与 Hardware Used，加上 DPU
    // 写的那一对自定义包头 gpu_id / token_id（进核那一笔记在这里，出核造包时原样
    // 带回）。软件包头 sw_header 由 RV core 通过配置改，这里不碰。
    HmemEntry& h = hmem.Entry(d.msg->stream_id);
    h.core_mask = d.msg->path_core_mask;
    h.hardware_used = 1;
    h.gpu_id = d.msg->gpu_id;
    h.token_id = d.msg->token_id;

    ++parse_pending;
    byte_count = d.msg->size;
    keep_sum = d.bytes;
    cur_frame = ++frame_cnt;
    in_frame = !d.last;
    if (d.bytes != 0 || d.last) {
      // Header 这一拍带的数据也是包的开头，从 0 起。整包只有这一拍时，边界
      // 标记跟着它走。
      payload->Drive(cur_frame, d.bytes, 0, d.last, d.msg);
      payload_driven = true;
      if (d.last) keep_sum = 0;
    }
    from_router->DriveReady(true);
  }

  // 检查 F2 列的那几项。这一轮只建结构性的几条，位域细节等包格式定死后补。
  // 落点改由配置给（CFG_ADDRx_DST），包头 dst_addr 不再参与，也不做对齐检查。
  bool Legal(CoreDataView const& d) const {
    if (!d.msg) return false;
    // 长度：最长实际支持到 32 KB。
    if (d.msg->size > kMaxTaskBytes) return false;
    return true;
  }

  Hmem& hmem;
  std::shared_ptr<CoreDataPort> from_router;
  std::shared_ptr<PayloadPort> payload;

  bool in_frame = false, dropping = false;
  uint64_t byte_count = 0, keep_sum = 0, cur_frame = 0, frame_cnt = 0;
  uint64_t last_seq = 0;
  bool payload_driven = false;
  uint64_t parse_pending = 0, drop_pending = 0, beat_pending = 0;

  Logic64 parsed, dropped, beats;
};

}  // namespace bach
}  // namespace latch

#endif
