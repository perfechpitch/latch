#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_LANE_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_LANE_

// Lane：一个物理通道，含读写两半各自的 TaskQueue、Active Context 与 AGCU。
//
// 五个实例：一个进核通道 in_ch，四个出核通道 out_ch[0..3] 与 Router 的四个 VC
// 一一对应。MM → CM 不另开通道，固定复用 out_ch[3]，它的目的端 MUX 到 Core Mem。
//
// 通道之间可以乱序执行：某个 VC 阻塞只堵住对应的那个出核通道，别的通道照发。
// 通道内顺序执行：TaskQueue 按序激活。同一通道内读写两半的状态彼此独立，一侧的
// Active Context 释放后就能激活下一个任务，不等另一侧 —— issue_done 就允许提前
// 激活下一任务，不必等全部 drain。
//
// 读这一半允许领先写那一半，领先量由三件事共同约束：中间 Buffer 的可用 Credit、
// 读的 outstanding 限额、可保留的任务边界数。
//
// issue_done 就把当前上下文腾出来给下一个任务，不必等全部 drain：发完请求的那
// 一笔挪进 drain 队列等响应收敛，下一笔当拍就能激活。可保留的任务边界数就是这
// 个队列的深度。
//
// 两半的分工按方向变：
//   进核  RD 侧的数据由 Header Parser 从 AXI-Stream 收下来推进 inbound buffer，
//         这一侧只跟着记账；WR 侧从 buffer 按任务边界取数写存储
//   出核  RD 侧的 AGCU 生成源端读地址、经 DMA_XBAR 读存储，数据进 outbound
//         buffer；WR 侧从 buffer 取数发给 Router
//   MM→CM 完全走出核通道，只是 WR 侧的出口从 Router TX 切到 Core Mem

#include <deque>
#include <memory>
#include <set>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/dte/agcu.h"
#include "bach/ip/chip/core/dte/buffer.h"
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/ip/chip/core/dte/task_queue.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// 读的 outstanding 限额，建模计划的默认值。
constexpr uint64_t kRdOutstanding = 4;

// 一侧同时能保留几个已经 issue_done、还在等 drain 的任务边界。原文只说「可保留
// 的任务边界数」是领先量的三项约束之一，没给数，取 2：一笔在等收敛，一笔在发。
constexpr uint64_t kDrainSlots = 2;

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
  //
  // 早先是 Header Parser 直接调进来往 buffer 里塞的，那是两个模块的协程同时改
  // 同一个容器：单线程下看着能跑，多线程下 25 轮里段错误了一次。一切跨模块的
  // 搬运都走端口。
  void AttachPayload(std::shared_ptr<PayloadPort> p) { payload = std::move(p); }

  uint64_t Moved() const { return moved.Get(); }
  uint64_t QueueLen(uint64_t half) const { return q[half].Size(); }

  bool Quiescent() const override {
    return q[kRd].Empty() && q[kWr].Empty() && !ctx[kRd].busy &&
           !ctx[kWr].busy && drain_q[kRd].empty() && drain_q[kWr].empty();
  }

 protected:
  void Step() override {
    // MM → CM 那一档一拍里两侧都用存储：RD 侧读 Matrix Mem，WR 侧写 Core Mem。
    // 谁都不许替对方调 IdleReq —— 同线程同拍两次写 Latch 不触发断言，盖掉的请
    // 求是静默丢的。所以两块存储各记各的，本拍末尾只把没人用的那一个置闲。
    cmem_used = false;
    mmem_used = false;
    router_used = false;

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

    rd_active = ctx[kRd].busy ? 1 : 0;
    wr_active = ctx[kWr].busy ? 1 : 0;
    moved = move_pending;
    TracePerCycle("rd_q", q[kRd].Size());
    TracePerCycle("wr_q", q[kWr].Size());
  }

 private:
  bool Outbound() const { return idx != kInCh; }

  // 一侧的起始地址。Core Mem 那一侧软件只配段内偏移，落在哪一片由硬件按
  // stream_id 算；Matrix Mem 那一侧软件配的就是最终物理地址。
  uint64_t StartAddr(Descriptor const& d, uint64_t half) const {
    if (half == kRd) {
      bool from_cm = d.route == Route::kCmToRouter;
      return agcu.DataAddr(d.stream_id, from_cm, d.src_addr);
    }
    bool to_cm = d.route == Route::kRouterToCm || d.route == Route::kMmToCm;
    return agcu.DataAddr(d.stream_id, to_cm, d.dst_addr);
  }

  // 这一笔在本通道的 buffer 里用哪个号认。进核那一路的号由 Header Parser 在
  // Commit 之前就发给了 payload 口，所以用它的帧号；出核那一路用 Commit 的
  // 内部序号。两条路各用各的 buffer，号撞不上。
  static uint64_t TagOf(Descriptor const& d) {
    return IsInbound(d.route) ? d.frame_seq : d.commit_seq;
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

  // 从整包 payload 里切出这一拍那一段。包没带 payload 时给一块零。
  static ByteBlockPtr SliceOf(MessagePtr const& m, uint64_t off, uint64_t n) {
    auto b = std::make_shared<ByteBlock>(n, 0);
    if (!m) return b;
    for (uint64_t i = 0; i < n && off + i < m->payload.size(); ++i) {
      (*b)[i] = m->payload[off + i];
    }
    return b;
  }

  // 收 Commit 准入的任务。ready 是「下一拍一定收得下」的承诺：往这两个队列里
  // 放东西的只有 Commit 一家，所以这一拍报的空位到下一拍还在。
  void TakeAdmit() {
    admit->DriveReady(!q[kRd].Full() && !q[kWr].Full());
    if (!admit->Valid() || admit->Seq() == last_admit_seq) return;
    auto d = admit->Desc();
    if (!d) return;
    last_admit_seq = admit->Seq();
    q[kRd].Push(*d);
    q[kWr].Push(*d);
  }

  // 从 Header Parser 收 Payload 写进 inbound buffer。ready 反映 buffer 还收不
  // 收得下，上游据此对 Router 反压。
  void TakePayload() {
    if (Outbound()) return;
    payload->DriveReady(buffer.HasRoom(idx));
    if (!payload->Valid()) return;
    // 同一拍数据会连着两拍出现在端口上，按序号认它。
    if (payload->Seq() == last_payload_seq) return;
    if (!buffer.HasRoom(idx)) return;
    last_payload_seq = payload->Seq();
    uint64_t frame = payload->frame.Get();
    bool last = payload->last.Get() != 0;
    uint64_t n = payload->bytes.Get();
    MessagePtr m = payload->msg.Get();
    buffer.Push(idx, {frame, n, last, SliceOf(m, payload->off.Get(), n), m});
    if (last) inbound_done.insert(frame);
  }

  // 已经 issue_done、在等响应收敛的那些：队头收敛了就报完成、出队。一侧一拍
  // 报一笔，报的口一拍只能驱动一次。
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
      // drain 队列满了就先不激活：可保留的任务边界数是领先量的一项约束。
      if (drain_q[h].size() >= kDrainSlots) continue;
      Descriptor const& d = q[h].Front();
      ctx[h].busy = true;
      ctx[h].desc = d;
      ctx[h].remain = d.bytes == 0 ? 0 : d.bytes;
      ctx[h].cur_addr = StartAddr(d, h);
      ctx[h].outstanding = 0;
      ctx[h].issue_done = false;
      ctx[h].drained = false;
      ctx[h].filled = 0;
      q[h].Pop();
    }
  }

  void StepRd() {
    ActiveCtx& c = ctx[kRd];
    if (!c.busy) return;

    if (!Outbound()) {
      // 进核：数据由 Header Parser 推进 buffer，这一侧只跟着记账。整包收完
      // （inbound_last 追上本任务）就算这一侧做完，没有在途的读请求要等。
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

    // 出核：AGCU 生成源端读地址，经 DMA_XBAR 读存储。领先量受三件事约束。
    if (c.issue_done) {
      // 请求发完了就把上下文腾出来，这一笔挪去等响应收敛。
      drain_q[kRd].push_back(c);
      c.busy = false;
      if (!rd_reported) rd_done->Idle();
      return;
    }
    if (!rd_reported) rd_done->Idle();

    // Buffer 的位置在发请求这一刻就要占下：响应回来时没位置，那一块数据就只能
    // 丢，而请求已经发出去、outstanding 也已经记上。所以余量要够本笔已经在飞
    // 的那些，加上这一笔自己。
    bool room = buffer.Credit(idx) > c.outstanding;
    bool slot = c.outstanding < kRdOutstanding;
    if (!room || !slot) return;
    uint64_t n = c.remain > kFlitBytes ? kFlitBytes : c.remain;
    bool from_cm = c.desc.route == Route::kCmToRouter;
    MemPort& port = from_cm ? *cmem : *mmem;
    if (!port.Ready()) return;
    port.Read(c.cur_addr, n);
    (from_cm ? cmem_used : mmem_used) = true;
    c.cur_addr += n;
    c.remain -= n;
    ++c.outstanding;
    if (c.remain == 0) c.issue_done = true;
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
    bool from_cm = c.desc.route == Route::kCmToRouter;
    MemPort& port = from_cm ? *cmem : *mmem;
    if (!port.RspValid()) return;
    LOGCHECK(c.outstanding > 0, "Lane: 收到没发过的读响应。");
    --c.outstanding;
    bool last = (c.remain == 0 && c.outstanding == 0);
    // 位置在发请求时就占下了，这里一定放得进。
    LOGCHECK(buffer.HasRoom(idx), "Lane: 读回来的数据没地方放，发请求那一步的"
                                  "Buffer credit 记错了。");
    ByteBlockPtr blk = port.RspData();
    FillOut(c, blk);
    buffer.Push(idx, {TagOf(c.desc), kFlitBytes, last, blk, c.desc.msg});
  }

  void StepWr() {
    ActiveCtx& c = ctx[kWr];
    if (!c.busy) {
      if (!wr_reported) wr_done->Idle();
      return;
    }
    if (c.issue_done) {
      // 请求发完了就把上下文腾出来，这一笔挪去等 Buffer 里的数据收敛。
      drain_q[kWr].push_back(c);
      c.busy = false;
      if (!wr_reported) wr_done->Idle();
      return;
    }
    if (!wr_reported) wr_done->Idle();

    if (buffer.Empty(idx) || buffer.Front(idx).tag != TagOf(c.desc)) return;
    BufBeat const& b = buffer.Front(idx);

    if (Outbound() && c.desc.route != Route::kMmToCm) {
      // 出核：发给 Router。这一档的出口是 Router TX。
      if (!to_router->Ready()) return;
      to_router->Drive(b.bytes, b.last, b.last, c.desc.vc, b.msg);
      router_used = true;
      buffer.Pop(idx);
      ++move_pending;
      if (b.last) c.issue_done = true;
      return;
    }

    // 进核写存储，或 MM → CM 那一档写 Core Mem。WR1 的硬件 route mask 只允许
    // CoreMem，所以出核通道的数据不会被写回 Matrix Mem。
    bool to_cm = c.desc.route == Route::kRouterToCm ||
                 c.desc.route == Route::kMmToCm;
    MemPort& port = to_cm ? *cmem : *mmem;
    if (!port.Ready()) return;
    ByteBlockPtr data = b.data ? b.data : std::make_shared<ByteBlock>(b.bytes, 0);
    port.Write(c.cur_addr, data);
    (to_cm ? cmem_used : mmem_used) = true;
    c.cur_addr += b.bytes;
    buffer.Pop(idx);
    ++move_pending;
    if (b.last) c.issue_done = true;
  }

  uint64_t idx;
  DteBuffer& buffer;
  Agcu agcu;
  std::shared_ptr<MemPort> cmem, mmem;
  std::shared_ptr<CoreDataPort> to_router;
  std::shared_ptr<PayloadPort> payload;
  std::shared_ptr<AdmitPort> admit;
  std::shared_ptr<HalfDonePort> rd_done, wr_done;

  std::array<TaskQueue, kHalfNum> q;
  std::array<ActiveCtx, kHalfNum> ctx;
  // 已经 issue_done、在等响应或 Buffer 收敛的那些。
  std::array<std::deque<ActiveCtx>, kHalfNum> drain_q;
  bool rd_reported = false, wr_reported = false;
  std::set<uint64_t> inbound_done;
  uint64_t last_payload_seq = 0, last_admit_seq = 0;
  bool cmem_used = false, mmem_used = false, router_used = false;
  uint64_t move_pending = 0;

  Logic64 rd_active, wr_active, moved;
};

}  // namespace bach
}  // namespace latch

#endif
