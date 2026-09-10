#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_HEADER_PARSER_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_HEADER_PARSER_

// Header Parser：Router 入站帧的第一道。
//
// 首拍固定是 Header，靠「上一帧 TLAST 已接受」判断下一拍是新 Header，不依赖
// Start-of-Frame 信号。首拍锁存后检查 opcode / route、长度、身份字段和帧格式，
// 通过了就生成一个 Descriptor 请 Commit 为 RD 与 WR 两侧同时分配。
//
// 一帧一任务：同一个 AXI-Stream Frame 只属于一个搬入任务，不允许任务间交织。
//
// Header Commit 成功后才允许 Payload Fire。TLAST 标识最后一个 Payload beat；
// byte_count 为 0 时可由 Header beat 同时携带 TLAST。
//
// 非法 Header 进 Drop Frame：不生成 Descriptor、不发存储器请求，只消费到 TLAST
// 以恢复帧边界。丢的是这一帧，不是把通道卡死。

#include <functional>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class HeaderParser : public BachModule {
 public:
  HeaderParser(ClockPtr clock, const std::string& name, uint64_t parent = 0,
               bool tick = true)
      : BachModule(clock, name, parent, tick),
        from_router(std::make_shared<CoreDataPort>(clock)),
        to_commit(std::make_shared<DescPort>(clock)),
        payload(std::make_shared<PayloadPort>(clock)),
        parsed(clock),
        dropped(clock),
        beats(clock) {}

  CoreDataPort& FromRouter() { return *from_router; }
  void AttachFromRouter(std::shared_ptr<CoreDataPort> p) {
    from_router = std::move(p);
  }
  std::shared_ptr<DescPort> ToCommitPtr() const { return to_commit; }

  // Payload 那些拍交给进核通道的 RD 侧，由它写进 inbound buffer，附带 task_id、
  // 有效字节与任务边界。走端口，不直接碰对方的容器。
  std::shared_ptr<PayloadPort> PayloadPtr() const { return payload; }

  // 进核数据往哪搬由软件配：Router → MM 还是 Router → CM。
  // path_task_map 的副本由 Hmem 管，装配层把它交进来。
  void AttachPathTask(std::function<uint64_t(uint64_t)> fn) {
    path_task = std::move(fn);
  }
  void SetInboundRoute(Route r) {
    LOGCHECK(IsInbound(r), "HeaderParser: 进核只能是 Router → MM 或 → CM。");
    inbound = r;
  }
  // B core 与 R core 的 token entry valid 标志表。搬完之后由 Completion RS 写
  // 出去，写出去了才通知 TS —— 软件自己写会写在数据落地之前。
  // B core 与 R core 上进来的包不建 stream 表项，进核那一笔的完成没有可报的
  // 对象，不回 Ack。
  void SetInboundNoAck(bool on) { inbound_no_ack = on; }
  void SetInboundFlag(uint64_t base, uint64_t entry_bytes) {
    flag_base = base;
    entry_bytes_of_slot = entry_bytes;
  }

  uint64_t Parsed() const { return parsed.Get(); }
  uint64_t Dropped() const { return dropped.Get(); }
  uint64_t Beats() const { return beats.Get(); }

  bool Quiescent() const override { return !in_frame && !waiting; }

 protected:
  void Step() override {
    payload_driven = false;
    // 上一笔 Descriptor 还没被 Commit 收下就原地保持，同时对 Router 反压：
    // Header Commit 成功之前不许 Payload Fire。
    if (waiting) {
      if (to_commit->Accepted()) {
        waiting = false;
        // 纯包头任务的 Header beat 自己带 TLAST，整帧就这一拍，后面没有 Payload。
        // 这时不能进 in_frame，否则下一帧的 Header 会被当成 Payload 收下去。
        in_frame = !header_only;
        header_only = false;
        ++parse_pending;
      } else {
        from_router->DriveReady(false);
        if (!payload_driven) payload->Idle();
        Commit();
        return;
      }
    }
    to_commit->Idle();
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

    // Buffer 满时通过 TREADY 向 Router 反压，本拍不收 —— 这一步要排在
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

    auto desc = std::make_shared<Descriptor>();
    desc->valid = true;
    desc->route = inbound;
    desc->user_id = d.msg->user_id;
    desc->path_id = d.msg->path_id;
    // stream_id 取自包头：一个用户在各 core 上占的槽位按到达顺序环形分配，
    // 各 core 分出来的号一致。task_id 按 path_id 查本地的 path_task_map ——
    // 那一笔是任务链上的第几步由收方的链定，包头里带的是发方的编号。
    desc->stream_id = d.msg->stream_id;
    desc->task_id = path_task ? path_task(d.msg->path_id) : d.msg->task_id;
    desc->frame_seq = ++frame_cnt;
    desc->bytes = d.msg->size;
    desc->dst_addr = d.msg->dst_addr;
    if (entry_bytes_of_slot != 0) {
      desc->smem_wr = true;
      desc->smem_addr = flag_base + d.msg->dst_addr / entry_bytes_of_slot * 4;
      desc->smem_data = 1;
    }
    desc->msg = d.msg;
    desc->task_last = true;
    desc->no_ack = inbound_no_ack;
    byte_count = d.msg->size;
    keep_sum = d.bytes;

    cur_frame = desc->frame_seq;
    to_commit->Drive(desc);
    waiting = true;
    // byte_count 为 0 的纯包头任务，Header 这一拍就是整帧。
    header_only = d.last;
    if (d.bytes != 0 || d.last) {
      // Header 这一拍带的数据也是包的开头，从 0 起。整包只有这一拍时，边界
      // 标记跟着它走。
      payload->Drive(cur_frame, d.bytes, 0, d.last, d.msg);
      payload_driven = true;
      if (d.last) keep_sum = 0;
    }
    from_router->DriveReady(false);
  }

  // 检查 F2 列的那几项。这一轮只建结构性的几条，位域细节等包格式定死后补。
  bool Legal(CoreDataView const& d) const {
    if (!d.msg) return false;
    // 长度：最短 16 B 的纯包头包，最长实际支持到 (16 K + 32) B。
    if (d.msg->size > kMaxTaskBytes) return false;
    // 目的地址要在范围内且对齐。
    if (d.msg->dst_addr % 128 != 0) return false;
    return true;
  }

  std::shared_ptr<CoreDataPort> from_router;
  std::shared_ptr<DescPort> to_commit;
  std::shared_ptr<PayloadPort> payload;

  Route inbound = Route::kRouterToCm;
  uint64_t flag_base = 0, entry_bytes_of_slot = 0;
  bool inbound_no_ack = false;
  std::function<uint64_t(uint64_t)> path_task;
  bool in_frame = false, dropping = false, waiting = false;
  bool header_only = false;
  uint64_t byte_count = 0, keep_sum = 0, cur_frame = 0, frame_cnt = 0;
  uint64_t last_seq = 0;
  bool payload_driven = false;
  uint64_t parse_pending = 0, drop_pending = 0, beat_pending = 0;

  Logic64 parsed, dropped, beats;
};

}  // namespace bach
}  // namespace latch

#endif
