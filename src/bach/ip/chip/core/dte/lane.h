#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_LANE_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_LANE_

// Lane：一个物理通道，含读写两半各自的 TaskQueue、Active Context 与 AGCU。
//
// 五个实例：一个进核通道 in_ch，四个出核通道 out_ch[0..3] 与 Router 的四个 VC
// 一一对应。MM → CM 不另开通道，固定复用 out_ch[3]，它的目的端 MUX 到 Core Mem。
//
// 通道之间可以乱序执行：某个 VC 阻塞只堵住对应的那个出核通道，别的通道照发。
// 通道内顺序执行：TaskQueue 按序激活。同一通道内读写两半的状态彼此独立，一侧的
// Active Context 释放后就能激活下一个任务，不等另一侧：issue_done 就允许提前
// 激活下一任务，不必等全部 drain。
//
// 读这一半允许领先写那一半，领先量由三件事共同约束：中间 Buffer 的可用 Credit、
// 读的 outstanding 限额、可保留的任务边界数。
//
// issue_done 就把当前上下文腾出来给下一个任务，不必等全部 drain：发完请求的那
// 一笔挪进 drain 队列等响应收敛，下一笔当拍就能激活。可保留的任务边界数就是这
// 个队列的深度。
//
// 对齐飞书《DTE DSA》后，任务是 4 个通用段位。两侧按段遍历：
//   进核  RD 侧的数据由 Header Parser 从 AXI-Stream 收下来推进 inbound buffer，
//         这一侧只跟着记账；WR 侧从 buffer 按任务边界取数，逐段写存储（数据段
//         照常写，scale 段走 scale 旁带，topK 段经旁带写 MU，包头段走 Hmem 不在
//         这里）
//   出核  RD 侧逐段读源端（数据段照常读，scale 段只读 scale），数据进 outbound
//         buffer；WR 侧从 buffer 取数发给 Router
//   MM→CM 完全走出核通道，只是 WR 侧的出口从 Router TX 切到 Core Mem
//
// 端点（Cmem/Mmem/scale 旁带/topK_table/header_table）在 Fire / 解析时已由 agcu
// 译码并展开成每段的 src_addr / dst_addr，这一层直接用，不再自己叠 stream 偏移。

#include <deque>
#include <memory>
#include <set>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/dte/agcu.h"
#include "bach/ip/chip/core/dte/buffer.h"
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/ip/chip/core/dte/task_queue.h"
#include "bach/ip/chip/core/mu/mu_ports.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 读的 outstanding 限额。原文只说领先量由 Buffer credit、读 outstanding 与可保留
// 的任务边界数共同约束，没给这一项的值。取与一个通道的 Buffer 项数相同：
// 让 Buffer 成为真正卡住的那一个，读这一半才填得满 256 B/T —— 在飞读数少于访存
// 延迟的拍数时，带宽就是「在飞数 ÷ 延迟」，取 4 只有 4/13。
constexpr uint64_t kRdOutstanding = kDteBufFlits;

// 一侧同时能保留几个已经 issue_done、还在等 drain 的任务边界。对齐飞书《DTE DSA》
// 「WR Lane TaskQ 深度 4，允许 Read Lane 先执行 4 个任务」：读这一半领先写那一半
// 的任务边界数取 4。
constexpr uint64_t kDrainSlots = 4;

// 段是否参与 payload 的字节搬运。topK / header 段走旁带或 Hmem，不占 payload。
inline bool PayloadSeg(SegEndpoint k) {
  return k == SegEndpoint::kCmem || k == SegEndpoint::kMmem ||
         k == SegEndpoint::kScale;
}

// 写这一侧：这一段的字节落在收到的 payload 流里。数据 / scale / topK 都算（topK
// 与数据/scale 同一条数据通道），只有 header 段走 Hmem 不在这里。读那一侧 topK 不
// 占（topK 从 MU 的 topK_ep_table 读，不走存储），所以读用 PayloadSeg、写用这个。
inline bool WrSeg(SegEndpoint k) {
  return k == SegEndpoint::kCmem || k == SegEndpoint::kMmem ||
         k == SegEndpoint::kScale || k == SegEndpoint::kTopk;
}

class Lane : public BachModule {
 public:
  Lane(ClockPtr clock, const std::string& name, uint64_t lane_idx,
       DteBuffer& buf, Agcu const& addr_gen, uint64_t parent = 0,
       bool tick = true)
      : BachModule(clock, name, parent, tick),
        idx(lane_idx),
        buffer(buf),
        agcu(addr_gen),
        cmem(std::make_shared<MemPort>(clock)),
        mmem(std::make_shared<MemPort>(clock)),
        to_router(std::make_shared<CoreDataPort>(clock)),
        payload(std::make_shared<PayloadPort>(clock)),
        admit(std::make_shared<AdmitPort>(clock)),
        rd_done(std::make_shared<HalfDonePort>(clock)),
        wr_done(std::make_shared<HalfDonePort>(clock)),
        rd_active(clock),
        wr_active(clock),
        moved(clock) {}

  // Commit 从这个口把准入的任务送进来。读写两侧同时进队，所以 ready 要两侧
  // 都有空位才给。
  std::shared_ptr<AdmitPort> AdmitPtr() const { return admit; }
  bool HasRoom(uint64_t half) const { return !q[half].Full(); }

  MemPort& Cmem() { return *cmem; }
  MemPort& Mmem() { return *mmem; }
  void AttachCmem(std::shared_ptr<MemPort> p) { cmem = std::move(p); }
  void AttachMmem(std::shared_ptr<MemPort> p) { mmem = std::move(p); }
  CoreDataPort& ToRouter() { return *to_router; }
  void AttachToRouter(std::shared_ptr<CoreDataPort> p) {
    to_router = std::move(p);
  }
  std::shared_ptr<HalfDonePort> RdDonePtr() const { return rd_done; }
  std::shared_ptr<HalfDonePort> WrDonePtr() const { return wr_done; }

  // 进核通道的 RD 侧从这个口收 Payload。
  void AttachPayload(std::shared_ptr<PayloadPort> p) { payload = std::move(p); }

  // topK 旁带写进 MU 的那条数据线。只有进核通道收到，出核通道恒为空。
  void AttachMuTopk(std::shared_ptr<MuTopkPort> p) { mu_topk = std::move(p); }

  uint64_t Moved() const { return moved.Get(); }
  uint64_t QueueLen(uint64_t half) const { return q[half].Size(); }

  bool Quiescent() const override {
    return q[kRd].Empty() && q[kWr].Empty() && !ctx[kRd].busy &&
           !ctx[kWr].busy && drain_q[kRd].empty() && drain_q[kWr].empty();
  }

 protected:
  void Step() override {
    cmem_used = false;
    mmem_used = false;
    router_used = false;
    topk_driven = false;

    // 末级先做：先收响应、再发新请求、最后激活下一个任务。
    TakeAdmit();
    TakePayload();
    CollectResponses();
    ReportDrained();
    StepWr();
    StepRd();
    Activate();

    if (!cmem_used) cmem->IdleReq();
    if (!mmem_used) mmem->IdleReq();
    if (!router_used) to_router->Idle();
    if (mu_topk && !topk_driven) mu_topk->Idle();

    rd_active = ctx[kRd].busy ? 1 : 0;
    wr_active = ctx[kWr].busy ? 1 : 0;
    moved = move_pending;
    TracePerCycle("rd_q", q[kRd].Size());
    TracePerCycle("wr_q", q[kWr].Size());
  }

 private:
  bool Outbound() const { return idx != kInCh; }

  // 读这一侧的源端存储：route 决定。CmToRouter 读 Core Mem，其余读 Matrix Mem。
  // scale 旁带随它的数据落在同一块存储。
  bool FromCm(Route r) const { return r == Route::kCmToRouter; }
  // 写这一侧的目的端存储：route 决定。RouterToCm、MmToCm 写 Core Mem，
  // RouterToMm 写 Matrix Mem。scale 旁带随它的数据落在同一块存储。
  bool ToCm(Route r) const {
    return r == Route::kRouterToCm || r == Route::kMmToCm;
  }

  // 这一笔在本通道的 buffer 里用哪个号认。进核那一路的号由 Lane 在 TakeAdmit 时按
  // 到达顺序编（与 Header Parser 发给 payload 口的帧号对齐），所以用它的帧号；
  // 出核那一路用 Commit 的内部序号。两条路各用各的 buffer，号撞不上。
  static uint64_t TagOf(Descriptor const& d) {
    return IsInbound(d.route) ? d.frame_seq : d.commit_seq;
  }

  // 读这一侧第 i 段要搬的字节数：payload 段搬 len，topK / header 段不搬。
  static uint64_t RdLen(Descriptor const& d, uint64_t i) {
    if (i >= 4) return 0;
    Segment const& s = d.seg[i];
    if (!s.valid || !PayloadSeg(s.src_kind)) return 0;
    return s.len;
  }

  // 从 seg 号往后读这一侧还有没有要搬的段。
  static bool RdRemain(Descriptor const& d, uint64_t seg) {
    for (uint64_t i = seg; i < 4; ++i) {
      if (RdLen(d, i) != 0) return true;
    }
    return false;
  }

  // 读回来的一块填进要发出去的那个包，按已填字节数排在后面。
  static void FillOut(ActiveCtx& c, ByteBlockPtr const& blk) {
    if (!c.desc.msg || !blk) return;
    std::vector<uint8_t>& out = c.desc.msg->payload;
    for (uint64_t i = 0; i < blk->size() && c.filled + i < out.size(); ++i) {
      out[c.filled + i] = (*blk)[i];
    }
    c.filled += blk->size();
  }

  // 一块里 [from, from + n) 那一段。
  static ByteBlockPtr SliceBlock(ByteBlockPtr const& blk, uint64_t from,
                                 uint64_t n) {
    auto b = std::make_shared<ByteBlock>(n, 0);
    for (uint64_t i = 0; i < n && from + i < blk->size(); ++i) {
      (*b)[i] = (*blk)[from + i];
    }
    return b;
  }

  // 从整包 payload 里切出这一拍那一段。包没带 payload 时给一块零。
  static ByteBlockPtr SliceOf(MessagePtr const& m, uint64_t off, uint64_t n) {
    auto b = std::make_shared<ByteBlock>(n, 0);
    if (!m) return b;
    for (uint64_t i = 0; i < n && off + i < m->payload.size(); ++i) {
      (*b)[i] = m->payload[off + i];
    }
    return b;
  }

  // 收 Commit 准入的任务。
  void TakeAdmit() {
    admit->DriveReady(!q[kRd].Full() && !q[kWr].Full());
    if (!admit->Valid() || admit->Seq() == last_admit_seq) return;
    auto d = admit->Desc();
    if (!d) return;
    last_admit_seq = admit->Seq();
    // 进核任务按到达顺序编帧号：配置驱动的第 N 个进核任务配第 N 个到达的数据包
    // （FIFO），与 Header Parser 给每帧编的号对齐。读写两半拿同一份号。
    Descriptor nd = *d;
    if (IsInbound(nd.route)) nd.frame_seq = ++inbound_frame_seq;
    q[kRd].Push(nd);
    q[kWr].Push(nd);
  }

  // 从 Header Parser 收 Payload 写进 inbound buffer。
  void TakePayload() {
    if (Outbound()) return;
    // 收下这一拍：valid、序号新、buffer 还有位置才收。收不下就留在端口上重试
    // （不推 last_payload_seq），靠 ready 反压上游。
    if (payload->Valid() && payload->Seq() != last_payload_seq &&
        buffer.HasRoom(idx)) {
      last_payload_seq = payload->Seq();
      uint64_t frame = payload->frame.Get();
      bool last = payload->last.Get() != 0;
      uint64_t n = payload->bytes.Get();
      uint64_t off = payload->off.Get();
      MessagePtr m = payload->msg.Get();
      buffer.Push(idx, {frame, n, last, SliceOf(m, off, n), m});
      if (last) inbound_done.insert(frame);
    }
    // 反压电平：buffer 留两格余量再报收得下，absorb 掉 ready 一拍延迟里已经在途
    // 的那一拍，与 CoreStation 的准入电平（size + 2 <= depth）同一套。
    payload->DriveReady(buffer.Credit(idx) >= 2);
  }

  // 已经 issue_done、在等响应收敛的那些：队头收敛了就报完成、出队。
  void ReportDrained() {
    rd_reported = false;
    wr_reported = false;
    for (uint64_t h = 0; h < kHalfNum; ++h) {
      if (drain_q[h].empty()) continue;
      ActiveCtx& c = drain_q[h].front();
      if (h == kRd) {
        if (c.outstanding != 0) continue;
        rd_done->Drive(c.desc.commit_seq, kRd, true);
        rd_reported = true;
      } else {
        if (!buffer.TaskDrained(idx, TagOf(c.desc))) continue;
        wr_done->Drive(c.desc.commit_seq, kWr, true);
        wr_reported = true;
      }
      drain_q[h].pop_front();
    }
  }

  // 一侧的 Active Context 空了就从 TaskQueue 按序激活下一个。
  void Activate() {
    for (uint64_t h = 0; h < kHalfNum; ++h) {
      if (ctx[h].busy || q[h].Empty()) continue;
      if (drain_q[h].size() >= kDrainSlots) continue;
      Descriptor const& d = q[h].Front();
      ctx[h].busy = true;
      ctx[h].desc = d;
      ctx[h].seg = 0;
      ctx[h].cur_addr = 0;
      ctx[h].remain = 0;
      ctx[h].off = 0;
      ctx[h].part = 0;
      ctx[h].outstanding = 0;
      ctx[h].issue_done = false;
      ctx[h].drained = false;
      ctx[h].filled = 0;
      ctx[h].sent_first = false;
      ctx[h].topk_pending = false;
      q[h].Pop();
    }
  }

  void StepRd() {
    ActiveCtx& c = ctx[kRd];
    if (!c.busy) return;

    if (!Outbound()) {
      // 进核：数据由 Header Parser 推进 buffer，这一侧只跟着记账。
      if (!c.issue_done && inbound_done.count(TagOf(c.desc)) != 0) {
        inbound_done.erase(TagOf(c.desc));
        c.issue_done = true;
        c.drained = true;
        if (!rd_reported) {
          rd_done->Drive(c.desc.commit_seq, kRd, true);
          rd_reported = true;
        } else {
          drain_q[kRd].push_back(c);
        }
        c.busy = false;
        return;
      }
      if (!rd_reported) rd_done->Idle();
      return;
    }

    // 出核：逐段读源端。领先量受三件事约束。
    if (c.issue_done) {
      drain_q[kRd].push_back(c);
      c.busy = false;
      if (!rd_reported) rd_done->Idle();
      return;
    }
    if (!rd_reported) rd_done->Idle();

    // 当前段读完了就推进到下一个有数据的段；都读完了就 issue_done。段 0 恒为
    // 包头、不是 payload 段，所以先从段 1 起看。
    if (c.remain == 0) {
      ++c.seg;
      while (c.seg < 4 && RdLen(c.desc, c.seg) == 0) ++c.seg;
      if (c.seg >= 4) {
        c.issue_done = true;
        return;
      }
      c.remain = RdLen(c.desc, c.seg);
      c.cur_addr = c.desc.seg[c.seg].src_addr;
    }

    uint64_t flying = c.outstanding;
    for (ActiveCtx const& d : drain_q[kRd]) flying += d.outstanding;
    bool room = buffer.Credit(idx) > flying;
    bool slot = flying < kRdOutstanding;
    if (!room || !slot) return;
    Segment const& s = c.desc.seg[c.seg];
    MemPort& port = FromCm(c.desc.route) ? *cmem : *mmem;
    if (!port.Ready()) return;
    if (s.src_kind == SegEndpoint::kScale) {
      // scale 段：len 是 scale 字节数，地址给的是对应数据地址。
      uint64_t g = c.remain > kFlitBytes ? kFlitBytes : c.remain;
      port.ReadScale(c.cur_addr, g * kScaleGroupBytes);
      c.cur_addr += g * kScaleGroupBytes;
      c.remain -= g;
    } else {
      uint64_t n = c.remain > kFlitBytes ? kFlitBytes : c.remain;
      port.Read(c.cur_addr, n);
      c.cur_addr += n;
      c.remain -= n;
    }
    (FromCm(c.desc.route) ? cmem_used : mmem_used) = true;
    ++c.outstanding;
  }

  // 响应按发出的顺序回来：先给还在等收敛的那一笔，它收满了再给当前这一笔。
  void CollectResponses() {
    if (!Outbound()) return;
    ActiveCtx* p = nullptr;
    if (!drain_q[kRd].empty() && drain_q[kRd].front().outstanding != 0) {
      p = &drain_q[kRd].front();
    } else if (ctx[kRd].busy) {
      p = &ctx[kRd];
    }
    if (p == nullptr) return;
    ActiveCtx& c = *p;
    bool from_cm = FromCm(c.desc.route);
    MemPort& port = from_cm ? *cmem : *mmem;
    if (!port.RspValid()) return;
    LOGCHECK(c.outstanding > 0, "Lane: 收到没发过的读响应。");
    --c.outstanding;
    // 这一笔的所有段都读完、没有在途请求了，才是任务边界（包的最后一拍）。
    bool last = c.remain == 0 && !RdRemain(c.desc, c.seg + 1) &&
                c.outstanding == 0;
    LOGCHECK(buffer.HasRoom(idx), "Lane: 读回来的数据没地方放，发请求那一步的"
                                  "Buffer credit 记错了。");
    ByteBlockPtr blk = port.RspData();
    FillOut(c, blk);
    uint64_t n = blk ? blk->size() : kFlitBytes;
    buffer.Push(idx, {TagOf(c.desc), n, last, blk, c.desc.msg});
  }

  void StepWr() {
    ActiveCtx& c = ctx[kWr];
    if (!c.busy) {
      if (!wr_reported) wr_done->Idle();
      return;
    }
    if (c.issue_done) {
      drain_q[kWr].push_back(c);
      c.busy = false;
      if (!wr_reported) wr_done->Idle();
      return;
    }
    if (!wr_reported) wr_done->Idle();

    // 出核：最后一拍 payload 发完还欠一笔 topK 拍。它不走 buffer（发完最后一拍
    // buffer 可能已经空了，且 topK 不占读回来的那一份 Credit），直接从这一拍发，
    // 发出去才置 issue_done。
    if (Outbound() && c.topk_pending) {
      if (!to_router->Ready()) return;
      uint64_t n = c.desc.TopkBytes();
      to_router->Drive(n, /*last=*/true, /*head=*/false, c.desc.vc, c.desc.msg);
      c.topk_pending = false;
      router_used = true;
      ++move_pending;
      c.issue_done = true;
      return;
    }

    if (buffer.Empty(idx) || buffer.Front(idx).tag != TagOf(c.desc)) return;
    BufBeat const& b = buffer.Front(idx);

    if (Outbound() && c.desc.route != Route::kMmToCm) {
      // 出核：发给 Router。这一档的出口是 Router TX。带 topK 的包，最后一拍 payload
      // 之后还要补一笔 256 B 的 topK 拍，所以最后一拍 payload 不打 last，留着给
      // topK 拍。
      if (!to_router->Ready()) return;
      bool has_topk = c.desc.TopkBytes() != 0;
      to_router->Drive(b.bytes, b.last && !has_topk, !c.sent_first, c.desc.vc,
                       b.msg);
      c.sent_first = true;
      router_used = true;
      buffer.Pop(idx);
      ++move_pending;
      if (b.last) {
        if (has_topk) c.topk_pending = true;
        else c.issue_done = true;
      }
      return;
    }

    // 进核写存储，或 MM → CM 那一档写 Core Mem。逐段拆分写入：payload 字节按段
    // 长顺序分到各个目的段。
    bool to_cm = ToCm(c.desc.route);
    MemPort& port = to_cm ? *cmem : *mmem;
    ByteBlockPtr data = b.data ? b.data : std::make_shared<ByteBlock>(b.bytes, 0);
    uint64_t n = b.bytes;
    uint64_t pos = c.off + c.part;           // 这一拍从整包的第几个字节起
    // 找到这个字节落在哪个 payload 段，以及段内偏移。topK 也占 payload 流的一拍，
    // 一并走这个循环（写这一侧用 WrSeg，读那一侧才用 PayloadSeg）。
    uint64_t seg = 0, seg_off = 0;
    while (seg < 4) {
      Segment const& s = c.desc.seg[seg];
      if (s.valid && WrSeg(s.dst_kind)) {
        if (pos < seg_off + s.len) break;
        seg_off += s.len;
      }
      ++seg;
    }
    Segment const& s = c.desc.seg[seg];
    uint64_t take = n - c.part;
    uint64_t seg_left = s.len - (pos - seg_off);
    if (take > seg_left) take = seg_left;    // 一拍跨进下一段
    bool whole = c.part == 0 && take == n;
    if (s.dst_kind == SegEndpoint::kTopk) {
      // topK 段：不落存储，随包的 topk 字段走，经专用数据线写进 MU 的 topK_ep_table。
      // 写进哪一项由 topK 段的端内偏移给：计算 core 是 stream_id，B core 是环形槽号。
      std::vector<uint8_t> topk = b.msg ? b.msg->topk : std::vector<uint8_t>();
      if (mu_topk) {
        mu_topk->Drive(c.desc.TopkIndex(),
                       std::make_shared<ByteBlock>(std::move(topk)));
        topk_driven = true;
      }
    } else {
      if (!port.Ready()) return;
      if (s.dst_kind == SegEndpoint::kScale) {
        // scale 段：dst_addr 是对应数据地址，整包里第 k 个 scale 管数据地址
        // dst_addr + k × 32 那一组。
        uint64_t k = pos - seg_off;
        port.WriteScale(s.dst_addr + k * kScaleGroupBytes,
                        whole ? data : SliceBlock(data, c.part, take),
                        take * kScaleGroupBytes);
      } else {
        port.Write(s.dst_addr + (pos - seg_off),
                   whole ? data : SliceBlock(data, c.part, take));
      }
      (to_cm ? cmem_used : mmem_used) = true;
    }
    c.part += take;
    if (c.part < n) return;
    bool last = b.last;
    c.off += n;
    c.part = 0;
    buffer.Pop(idx);
    ++move_pending;
    if (last) c.issue_done = true;
  }

  uint64_t idx;
  DteBuffer& buffer;
  Agcu agcu;
  std::shared_ptr<MemPort> cmem, mmem;
  std::shared_ptr<CoreDataPort> to_router;
  std::shared_ptr<PayloadPort> payload;
  std::shared_ptr<AdmitPort> admit;
  std::shared_ptr<MuTopkPort> mu_topk;
  std::shared_ptr<HalfDonePort> rd_done, wr_done;

  std::array<TaskQueue, kHalfNum> q;
  std::array<ActiveCtx, kHalfNum> ctx;
  std::array<std::deque<ActiveCtx>, kHalfNum> drain_q;
  bool rd_reported = false, wr_reported = false;
  std::set<uint64_t> inbound_done;
  // 进核任务按到达顺序编的帧号，与 Header Parser 给每帧编的号对齐。
  uint64_t inbound_frame_seq = 0;
  uint64_t last_payload_seq = 0, last_admit_seq = 0;
  bool cmem_used = false, mmem_used = false, router_used = false;
  bool topk_driven = false;
  uint64_t move_pending = 0;

  Logic64 rd_active, wr_active, moved;
};

}  // namespace bach
}  // namespace latch

#endif
