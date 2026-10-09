#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_MOVER_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_MOVER_

// Mover：DTE 的数据通路，合并原来的 Lane / Buffer / DMA_XBAR / OutArb / Commit /
// CompletionRs / HeaderParser。
//
// 原来那些模块各自是独立协程，靠 valid/ready/seq 多拍握手与中间 Buffer、读写两半、
// drain 队列、读 outstanding、credit 这些细节互相顶速度差。这一版把它们收敛成一个
// 模块，按任务顺序推进、每拍搬一拍：
//
//   进核（Router→Cm/Mm）  收下一整帧（Router 一拍一 beat 送到，Message 里已经带
//                         完整 payload），再逐段写存储，写完报完成。
//   出核（Cm/Mm→Router）  建包 → 逐段读存储填 payload（一个在途读，读一拍等一拍）
//                         → 逐拍发 Router（数据段 + scale 段分开发 beat，带 topK 的
//                         补最后一拍），发完报完成。
//   MM→CM               读 Mmem → 写 Cmem，报完成。
//
// 五个通道各一个在途任务槽（in_ch + out_ch[0..3]），存储读/写口与 Router 出核单口
// 在本模块内联仲裁：一个口一拍只服务一个通道。读响应按“一块存储一个在途读”认归属。
//
// 完成上报折叠进这里：done_pend 串行化、shareMem 写在报 TS 之前、ack_ts_en 恰好一
// 次、向 DonePort 一拍报一笔（exactly-once）。
//
// 保留的观测计数：parsed / admitted / start_task / start_user / stalled / joined /
// used / reported / done_task / done_user / sent / occupancy，供 Dte 门面访问器读。

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/log.h"
#include "bach/common/flit.h"
#include "bach/common/message.h"
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/ip/chip/core/dte/dte_types.h"
#include "bach/ip/chip/core/dte/hmem.h"
#include "bach/ip/chip/core/memory/banked_mem.h"
#include "bach/ip/chip/core/mu/gen_ep_info.h"
#include "bach/ip/chip/core/mu/mu_ports.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/chip/core/ts/ts_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class Mover : public BachModule {
 public:
  Mover(ClockPtr clock, const std::string& name, Hmem& tables,
        uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        hmem(tables),
        from_rv(std::make_shared<DescPort>(clock)),
        from_router(std::make_shared<CoreDataPort>(clock)),
        to_router(std::make_shared<CoreDataPort>(clock)),
        cmem_rd(std::make_shared<MemPort>(clock)),
        cmem_wr(std::make_shared<MemPort>(clock)),
        mmem_rd(std::make_shared<MemPort>(clock)),
        mmem_wr(std::make_shared<MemPort>(clock)),
        smem_wr(std::make_shared<MemPort>(clock)),
        mu_topk(std::make_shared<MuTopkPort>(clock)),
        to_ts(std::make_shared<DonePort>(clock)) {}

  // ── 对外端口 ──
  CoreDataPort& FromRouter() { return *from_router; }
  void AttachFromRouter(std::shared_ptr<CoreDataPort> p) {
    from_router = std::move(p);
  }
  CoreDataPort& ToRouter() { return *to_router; }
  void AttachToRouter(std::shared_ptr<CoreDataPort> p) {
    to_router = std::move(p);
  }
  MemPort& CmemRd() { return *cmem_rd; }
  MemPort& CmemWr() { return *cmem_wr; }
  MemPort& MmemRd() { return *mmem_rd; }
  MemPort& MmemWr() { return *mmem_wr; }
  void AttachCmemRd(std::shared_ptr<MemPort> p) { cmem_rd = std::move(p); }
  void AttachCmemWr(std::shared_ptr<MemPort> p) { cmem_wr = std::move(p); }
  void AttachMmemRd(std::shared_ptr<MemPort> p) { mmem_rd = std::move(p); }
  void AttachMmemWr(std::shared_ptr<MemPort> p) { mmem_wr = std::move(p); }
  MemPort& SmemWr() { return *smem_wr; }
  std::shared_ptr<MemPort> SmemWrPtr() const { return smem_wr; }
  void AttachSmemWr(std::shared_ptr<MemPort> p) { smem_wr = std::move(p); }
  void AttachMuTopk(std::shared_ptr<MuTopkPort> p) { mu_topk = std::move(p); }
  DonePort& ToTs() { return *to_ts; }
  void AttachToTs(std::shared_ptr<DonePort> p) { to_ts = std::move(p); }

  // 起任务这一笔从 Regfile 的 DescPort 来。
  DescPort& FromRv() { return *from_rv; }
  void RebindRv(std::shared_ptr<DescPort> p) { from_rv = std::move(p); }

  // 出核造包时从 MU 的 topK_ep_table 读 topK、从 Core Mem 同步读包头上下文。
  void AttachMuTopkEp(GenEpInfo* ep) { mu_topk_ep = ep; }
  void AttachCmemSync(BankedMem* m) { cmem_sync = m; }

  // ── 观测 ──
  uint64_t Parsed() const { return parsed; }
  uint64_t Admitted() const { return admitted; }
  uint64_t RvAdmitted() const { return admitted; }
  uint64_t StartTask() const { return start_task; }
  uint64_t StartUser() const { return start_user; }
  uint64_t Stalled() const { return stalled; }
  uint64_t Joined() const { return joined; }
  uint64_t Used() const { return used; }
  uint64_t Reported() const { return reported; }
  uint64_t DoneTask() const { return done_task; }
  uint64_t DoneUser() const { return done_user; }
  uint64_t Sent() const { return sent; }
  uint64_t Occupancy() const { return occupancy; }
  // 五条 lane 的边沿计数与起点身份，供 core.h 的 EmitDte() 打包成波形信号。
  uint64_t LaneRdSeq(uint64_t lane) const { return lane_rd_seq[lane]; }
  uint64_t LaneWrSeq(uint64_t lane) const { return lane_wr_seq[lane]; }
  uint64_t LaneRdTask(uint64_t lane) const { return lane_rd_task[lane]; }
  uint64_t LaneRdUser(uint64_t lane) const { return lane_rd_user[lane]; }

  bool Quiescent() const override {
    if (!pending.empty() || !rx_frames.empty() || !done_pend.empty()) return false;
    for (auto const& s : slots) {
      if (s) return false;
    }
    return !rx_msg;
  }

 protected:
  void Step() override {
    cmem_rd_used = mmem_rd_used = false;
    cmem_wr_used = mmem_wr_used = false;
    topk_used = false;

    ReceiveFromRouter();
    Dispatch();
    CollectResponses();
    AdvanceAll();
    SendStep();
    Report();

    if (!cmem_rd_used) cmem_rd->IdleReq();
    if (!cmem_wr_used) cmem_wr->IdleReq();
    if (!mmem_rd_used) mmem_rd->IdleReq();
    if (!mmem_wr_used) mmem_wr->IdleReq();
    if (!smem_used) smem_wr->IdleReq();
    if (mu_topk && !topk_used) mu_topk->Idle();

    used = 0;
    for (auto const& s : slots) {
      if (s) ++used;
    }
    occupancy = used + rx_frames.size();
    TracePerCycle("used", used);
    TracePerCycle("pending", pending.size());
  }

 private:
  enum class Phase { kRead, kWrite, kSend };

  struct Task {
    Descriptor desc;
    MessagePtr msg;              // 出核：建好待发的包；进核：收到的整帧
    Phase phase = Phase::kRead;
    uint64_t seg = 0;            // 当前段（读/写/发共用，各相顺序推进）
    uint64_t off = 0;            // 段内已处理字节
    uint64_t filled = 0;         // 出核读回来已经填进 payload 的字节
    bool hdr_stored = false;     // 进核：包头上下文落库没有
    bool sent_first = false;     // 出核：首拍发出去没有
    bool topk_pending = false;   // 出核：最后一拍 payload 发完，还欠 topK 拍
    bool rresp_seen = false;     // 已收到第一笔读响应（lane 轨道的起点）
    uint64_t rd_outstanding = 0; // 已发出、还没回来的读请求数（这个任务）
  };

  struct Pend {
    uint64_t stream_id = 0, task_id = 0, user_id = 0, reduce_seq = 0;
    bool notify = true;
    bool smem = false;
    uint64_t smem_addr = 0, smem_data = 0;
  };

  // 读写这一侧的源/目的存储，由 route 决定（scale 旁带随它的数据落在同一块存储）。
  static bool FromCm(Route r) { return r == Route::kCmToRouter; }
  static bool ToCm(Route r) {
    return r == Route::kRouterToCm || r == Route::kMmToCm;
  }

  // 段是否参与 payload 的正文字节（读侧与发拍侧：数据 + scale，不含 topK）。
  static bool DataOrScale(SegEndpoint k) {
    return k == SegEndpoint::kCmem || k == SegEndpoint::kMmem ||
           k == SegEndpoint::kScale;
  }
  static bool ReadSeg(Segment const& s) {
    return s.valid && !s.is_header && DataOrScale(s.src_kind);
  }
  // 写这一侧：数据 + scale + topK（topK 也占 payload 流的一拍，走专用数据线）。
  static bool WriteSeg(Segment const& s) {
    if (!s.valid || s.is_header) return false;
    return DataOrScale(s.dst_kind) || s.dst_kind == SegEndpoint::kTopk;
  }
  // 段 i 在 msg->payload 里的起点：前面所有 data/scale 段的长度之和。
  static uint64_t PayloadBase(Descriptor const& d, uint64_t seg) {
    uint64_t base = 0;
    for (uint64_t i = 0; i < seg; ++i) {
      Segment const& s = d.seg[i];
      if (s.valid && !s.is_header && DataOrScale(s.dst_kind)) base += s.len;
    }
    return base;
  }
  // 从 seg 起还有没有要发的 data/scale 段。
  static bool HasLaterPayload(Descriptor const& d, uint64_t seg) {
    for (uint64_t i = seg; i < 4; ++i) {
      if (ReadSeg(d.seg[i])) return true;
    }
    return false;
  }

  // 从 Router 收进核帧。一帧一任务、帧不交织，单槽 rx_msg 就够：首拍是 Header，
  // Message 里已带完整 payload；TLAST 判帧边界，收齐推进 rx_frames 等配对。
  void ReceiveFromRouter() {
    CoreDataView d = ReadCoreData(*from_router);
    if (!d.valid) {
      from_router->DriveReady(true);
      return;
    }
    // 同一拍数据会连着两拍出现在端口上，按序号认它。
    if (d.seq == last_router_seq) {
      from_router->DriveReady(rx_frames.size() < kCentralTaskQDepth);
      return;
    }
    // 收新帧（首拍是 Header）：已收齐的帧满中央队列就反压，不记序号，下拍原样重来。
    if (!rx_msg && rx_frames.size() >= kCentralTaskQDepth) {
      from_router->DriveReady(false);
      return;
    }
    last_router_seq = d.seq;
    if (!rx_msg) {
      // 首拍是 Header：包头上下文随 Message 一起进来，这里只做合法性检查。
      LOGCHECK(d.msg && d.msg->size <= kMaxTaskBytes,
               "Mover: 非法包头（没带 Message 或长度超过 32 KB）。");
      rx_msg = d.msg;
      ++parsed;
    }
    if (d.last) {
      rx_frames.push_back(rx_msg);
      rx_msg.reset();
    }
    from_router->DriveReady(rx_frames.size() < kCentralTaskQDepth);
  }

  void TakeFromRv() {
    if (!from_rv->Valid()) {
      from_rv->DriveAccepted(false);
      return;
    }
    if (from_rv->Seq() == last_rv_seq) {
      from_rv->DriveAccepted(true);
      return;
    }
    auto d = from_rv->Desc();
    if (!d) {
      from_rv->DriveAccepted(false);
      return;
    }
    if (pending.size() >= kCentralTaskQDepth) {
      from_rv->DriveAccepted(false);
      return;
    }
    Descriptor nd = *d;
    MarkReducePkt(nd);
    pending.push_back(nd);
    last_rv_seq = from_rv->Seq();
    from_rv->DriveAccepted(true);
  }

  // 走归约路径出核的包，包头的 reduce_seq 打上发方的 task_id。
  void MarkReducePkt(Descriptor& d) {
    if (d.route != Route::kCmToRouter && d.route != Route::kMmToRouter) return;
    if (hmem.Rtab(d.path_id).operation == Operation::kForward) return;
    d.reduce_seq = d.task_id;
  }

  // 把 pending 里的任务填进空闲槽。进核任务要有收到的帧才放（FIFO 配对）。
  void Dispatch() {
    TakeFromRv();
    bool moved = false;
    for (auto it = pending.begin(); it != pending.end();) {
      uint64_t lane = it->Lane();
      if (lane >= kLaneNum || slots[lane]) {
        ++it;
        continue;
      }
      if (IsInbound(it->route) && rx_frames.empty()) {
        ++it;
        continue;
      }
      Task t;
      t.desc = *it;
      if (IsInbound(it->route)) {
        t.msg = rx_frames.front();
        rx_frames.pop_front();
        t.phase = Phase::kWrite;
      } else {
        if (!t.desc.msg) MakeOutboundMsg(t.desc);
        t.msg = t.desc.msg;
        t.phase = Phase::kRead;
      }
      ++admitted;
      start_task = t.desc.task_id;
      start_user = t.desc.user_id;
      slots[lane] = std::move(t);
      if (IsInbound(slots[lane]->desc.route)) {
        LaneRdEvent(lane, *slots[lane]);   // 进核无读：起点取放进槽这一拍
      }
      it = pending.erase(it);
      moved = true;
    }
    if (!pending.empty() && !moved) ++stalled;
  }

  // 出核任务要发出去的那个包在这里造好，读回来的数据往它的 payload 里填。
  void MakeOutboundMsg(Descriptor& d) const {
    auto m = std::make_shared<Message>();
    m->user_id = d.user_id;
    m->path_id = d.path_id;
    m->dst = hmem.Rtab(d.path_id).ext_dst;
    m->dst_addr = d.DataDst();
    HmemEntry h;
    if (d.seg[0].src_kind == SegEndpoint::kHeader) {
      h = hmem.Entry(d.stream_id);
    } else {
      h = BytesToHeader(cmem_sync->Peek(d.seg[0].src_addr, kHeaderCtxBytes));
    }
    m->gpu_id = h.gpu_id;
    m->token_id = h.token_id;
    m->size = d.PayloadBytes();
    m->scale_valid = d.HasScale() ? 1 : 0;
    if (d.HasTopk() && mu_topk_ep) {
      m->topk_valid = 1;
      m->topk = mu_topk_ep->TopkBytes(d.TopkIndex());
    }
    m->vc = d.vc;
    m->stream_id = d.stream_id;
    m->task_id = d.task_id;
    m->reduce_seq = d.reduce_seq;
    m->payload.assign(d.PayloadDataBytes(), 0);
    d.msg = m;
  }

  // 读响应按“一块存储一队按序在途读”认归属：同一存储的响应按发出顺序回来，
  // owner 队列的队头就是这一笔响应属于哪个槽。
  void CollectResponses() {
    CollectOne(*cmem_rd, cmem_rd_owners);
    CollectOne(*mmem_rd, mmem_rd_owners);
  }

  // lane 轨道起点边沿：这一拍 lane 上某笔任务收到第一笔读响应（进核无读，放进槽
  // 那一拍就算起点）。身份取这笔任务自己的，供 core.h 打包进 dte_lane_rd_*。
  void LaneRdEvent(uint64_t lane, Task const& t) {
    ++lane_rd_seq[lane];
    lane_rd_task[lane] = t.desc.task_id;
    lane_rd_user[lane] = t.desc.user_id;
  }

  void CollectOne(MemPort& rd, std::deque<int>& owners) {
    if (owners.empty() || !rd.RspValid()) return;
    int owner = owners.front();
    owners.pop_front();
    Task& t = *slots[owner];
    if (!t.rresp_seen) {
      t.rresp_seen = true;
      LaneRdEvent(owner, t);
    }
    ByteBlockPtr blk = rd.RspData();
    uint64_t n = blk ? blk->size() : kFlitBytes;
    if (t.msg) {
      for (uint64_t i = 0; i < n && t.filled + i < t.msg->payload.size(); ++i) {
        t.msg->payload[t.filled + i] = (*blk)[i];
      }
    }
    t.filled += n;
    --t.rd_outstanding;
  }

  // 推进各槽的读/写。出核发拍走 SendStep（全局一个 Router 口）。
  void AdvanceAll() {
    for (uint64_t i = 0; i < kLaneNum; ++i) {
      if (!slots[i]) continue;
      Task& t = *slots[i];
      if (t.phase == Phase::kRead) {
        ReadStep(t, i);
      } else if (t.phase == Phase::kWrite) {
        WriteStep(t, i);
      }
    }
  }

  void ReadStep(Task& t, uint64_t slot) {
    bool from_cm = FromCm(t.desc.route);
    MemPort& rd = from_cm ? *cmem_rd : *mmem_rd;
    std::deque<int>& owners = from_cm ? cmem_rd_owners : mmem_rd_owners;

    while (t.seg < 4 && !ReadSeg(t.desc.seg[t.seg])) ++t.seg;
    if (t.seg >= 4) {
      // 读段都发出去了，等这笔任务的在途读全部回来再进下一相（读发完不等于读完）。
      if (t.rd_outstanding == 0) {
        if (!t.rresp_seen) {
          t.rresp_seen = true;
          LaneRdEvent(slot, t);   // 没有读段：读相直接结束，起点取这一拍
        }
        t.phase = (t.desc.route == Route::kMmToCm) ? Phase::kWrite : Phase::kSend;
        t.seg = 0;
        t.off = 0;
      }
      return;
    }
    // 一个口一拍只发一笔；这块存储本拍已发过就轮到下一拍。
    if (from_cm ? cmem_rd_used : mmem_rd_used) return;
    if (!rd.Ready()) return;

    Segment const& s = t.desc.seg[t.seg];
    uint64_t left = s.len - t.off;
    uint64_t n = left > kFlitBytes ? kFlitBytes : left;
    if (s.src_kind == SegEndpoint::kScale) {
      rd.ReadScale(s.src_addr + t.off * kScaleGroupBytes, n * kScaleGroupBytes);
    } else {
      rd.Read(s.src_addr + t.off, n);
    }
    owners.push_back(slot);
    ++t.rd_outstanding;
    // 读游标在读发出的当拍就推进，不等响应；响应回来只往 payload 里填。
    t.off += n;
    if (t.off >= s.len) {
      t.off = 0;
      ++t.seg;
    }
    if (from_cm) cmem_rd_used = true;
    else mmem_rd_used = true;
  }

  void WriteStep(Task& t, uint64_t slot) {
    // 进核任务收下第一拍就落包头上下文（出核不落：包头出核时从落库处读回）。
    if (IsInbound(t.desc.route) && !t.hdr_stored) {
      StoreHeader(t);
      t.hdr_stored = true;
    }
    while (t.seg < 4 && !WriteSeg(t.desc.seg[t.seg])) ++t.seg;
    if (t.seg >= 4) {
      Finish(slot);
      return;
    }
    Segment const& s = t.desc.seg[t.seg];
    if (s.dst_kind == SegEndpoint::kTopk) {
      // topK 段不落存储，随包的 topk 字段走，经专用数据线写进 MU 的 topK_ep_table。
      if (mu_topk && !topk_used) {
        std::vector<uint8_t> topk = t.msg ? t.msg->topk : std::vector<uint8_t>();
        mu_topk->Drive(t.desc.TopkIndex(),
                       std::make_shared<ByteBlock>(std::move(topk)));
        topk_used = true;
      }
      ++t.seg;
      t.off = 0;
      return;
    }
    uint64_t left = s.len - t.off;
    uint64_t n = left > kFlitBytes ? kFlitBytes : left;
    MemPort& wr = ToCm(t.desc.route) ? *cmem_wr : *mmem_wr;
    if ((ToCm(t.desc.route) ? cmem_wr_used : mmem_wr_used)) return;
    if (!wr.Ready()) return;
    uint64_t base = PayloadBase(t.desc, t.seg);
    auto data = std::make_shared<ByteBlock>(n, 0);
    if (t.msg) {
      for (uint64_t i = 0; i < n && base + t.off + i < t.msg->payload.size(); ++i) {
        (*data)[i] = t.msg->payload[base + t.off + i];
      }
    }
    if (s.dst_kind == SegEndpoint::kScale) {
      wr.WriteScale(s.dst_addr + t.off * kScaleGroupBytes, data,
                    n * kScaleGroupBytes);
    } else {
      wr.Write(s.dst_addr + t.off, data);
    }
    if (ToCm(t.desc.route)) cmem_wr_used = true;
    else mmem_wr_used = true;
    t.off += n;
    if (t.off == s.len) {
      ++t.seg;
      t.off = 0;
    }
  }

  // 进核：包头上下文落库。计算 core 落 Hmem（按 stream_id），B/R core 落 Core Mem
  // （段 0 目的端地址，48 B 序列化）。
  void StoreHeader(Task& t) {
    MessagePtr const& m = t.msg;
    if (!m) return;
    if (t.desc.seg[0].dst_kind == SegEndpoint::kHeader) {
      HmemEntry& h = hmem.Entry(t.desc.stream_id);
      h.core_mask = m->path_core_mask;
      h.hardware_used = 1;
      h.gpu_id = m->gpu_id;
      h.token_id = m->token_id;
    } else {
      HmemEntry h;
      h.core_mask = m->path_core_mask;
      h.hardware_used = 1;
      h.gpu_id = m->gpu_id;
      h.token_id = m->token_id;
      cmem_sync->Poke(t.desc.seg[0].dst_addr, HeaderToBytes(h));
    }
  }

  // 出核发拍：Router 那一个口，四个出核通道共用。一个包的几拍粘在一个通道上
  // （router_owner 直到发完带 tlast 的拍 / 补完 topK 拍才撒手），别的包插不进来。
  void SendStep() {
    if (router_held) {
      // 上一拍还没被 Router 收下，保持不动。
      if (!to_router->Ready()) return;
      router_held = false;
      if (router_held_last) {
        Finish(router_owner);  // 帧发完了
        router_owner = -1;
      }
      // 帧没发完：router_owner 保持，下一拍继续发同一帧。
      return;
    }
    if (router_owner == -1) {
      for (uint64_t i = 0; i < kLaneNum; ++i) {
        if (slots[i] && slots[i]->phase == Phase::kSend) {
          router_owner = i;
          break;
        }
      }
    }
    if (router_owner == -1) {
      to_router->Idle();
      return;
    }
    Task& t = *slots[router_owner];
    if (t.topk_pending) {
      // 最后一拍 payload 发完，补一笔 topK 拍（不走 payload 流，字节随 topk 字段）。
      to_router->Drive(t.desc.TopkBytes(), /*last=*/true, /*hdr=*/!t.sent_first,
                       t.desc.vc, t.msg);
      t.topk_pending = false;
      ++sent;
      router_held = true;
      router_held_last = true;
      return;
    }
    while (t.seg < 4 && !ReadSeg(t.desc.seg[t.seg])) ++t.seg;
    if (t.seg >= 4) {
      // 没有 payload 段了。
      if (t.desc.TopkBytes() != 0) {
        t.topk_pending = true;
        return;
      }
      Finish(router_owner);
      router_owner = -1;
      return;
    }
    Segment const& s = t.desc.seg[t.seg];
    uint64_t left = s.len - t.off;
    uint64_t n = left > kFlitBytes ? kFlitBytes : left;
    bool has_topk = t.desc.TopkBytes() != 0;
    bool seg_done = (t.off + n == s.len);
    // 带 topK 的包最后一拍 payload 不打 last，留给补的那笔 topK 拍。
    bool last = seg_done && !HasLaterPayload(t.desc, t.seg + 1) && !has_topk;
    to_router->Drive(n, last, !t.sent_first, t.desc.vc, t.msg);
    t.sent_first = true;
    ++sent;
    t.off += n;
    if (seg_done) {
      ++t.seg;
      t.off = 0;
    }
    if (last) {
      router_held_last = true;  // 无 topK 的最后一拍 payload，这一拍就是帧尾
    } else {
      router_held_last = false;
      if (seg_done && !HasLaterPayload(t.desc, t.seg) && has_topk) {
        t.topk_pending = true;  // 最后一拍 payload 发完，欠 topK 拍
      }
    }
    router_held = true;
  }

  void Finish(uint64_t slot) {
    Task const& t = *slots[slot];
    ++lane_wr_seq[slot];   // lane 轨道终点：最后一笔写/发出去，任务离开槽位
    ++joined;
    if (t.desc.ack_ts_en || t.desc.wr_sharemem_flag) {
      done_pend.push_back({t.desc.stream_id, t.desc.task_id, t.desc.user_id,
                           t.desc.reduce_seq, t.desc.ack_ts_en,
                           t.desc.wr_sharemem_flag, t.desc.smem_addr,
                           t.desc.smem_data});
    }
    if (router_owner == static_cast<int>(slot)) router_owner = -1;
    slots[slot].reset();
  }

  // 向 TS 的报告：一拍报一笔，exactly-once。shareMem 写在报 TS 之前。
  void Report() {
    smem_used = false;
    if (done_pend.empty()) {
      to_ts->Idle();
      return;
    }
    Pend& p = done_pend.front();
    if (p.smem) {
      to_ts->Idle();
      if (!smem_wr->Ready()) return;
      auto d = std::make_shared<ByteBlock>(4, 0);
      for (uint64_t k = 0; k < 4; ++k) {
        (*d)[k] = uint8_t((p.smem_data >> (8 * k)) & 0xFFu);
      }
      smem_wr->Write(p.smem_addr, d);
      smem_used = true;
      p.smem = false;
      return;
    }
    if (!p.notify) {
      done_pend.pop_front();
      to_ts->Idle();
      return;
    }
    to_ts->Drive(p.stream_id, p.task_id);
    done_task = p.task_id;
    done_user = p.user_id;
    done_pend.pop_front();
    ++reported;
  }

  Hmem& hmem;
  BankedMem* cmem_sync = nullptr;
  GenEpInfo* mu_topk_ep = nullptr;

  std::shared_ptr<DescPort> from_rv;
  std::shared_ptr<CoreDataPort> from_router, to_router;
  std::shared_ptr<MemPort> cmem_rd, cmem_wr, mmem_rd, mmem_wr, smem_wr;
  std::shared_ptr<MuTopkPort> mu_topk;
  std::shared_ptr<DonePort> to_ts;

  // 中央任务队列：已快照、尚未开始搬的完整 TaskDesc。
  std::deque<Descriptor> pending;
  // 已收齐、待配对的进核帧（Message 里已带完整 payload）。
  std::deque<MessagePtr> rx_frames;
  MessagePtr rx_msg;                    // 正在收的那一帧
  std::array<std::optional<Task>, kLaneNum> slots;
  std::deque<Pend> done_pend;

  // 读响应归属：一块存储一队按序在途读，队头是下一笔响应属于哪个槽。
  std::deque<int> cmem_rd_owners, mmem_rd_owners;
  // 出核发拍状态。
  int router_owner = -1;
  bool router_held = false, router_held_last = false;

  uint64_t last_router_seq = 0, last_rv_seq = 0;

  // 观测计数。
  uint64_t parsed = 0, admitted = 0, start_task = 0, start_user = 0;
  uint64_t stalled = 0, joined = 0, used = 0, reported = 0;
  uint64_t done_task = 0, done_user = 0, sent = 0, occupancy = 0;

  // 五条 lane 的边沿计数与起点身份。lane_rd_seq 起点（第一笔读响应 / 进核放进槽）、
  // lane_wr_seq 终点（最后一笔写/发）。起点身份随事件存一份，供 EmitDte() 读。
  std::array<uint64_t, kLaneNum> lane_rd_seq{};
  std::array<uint64_t, kLaneNum> lane_wr_seq{};
  std::array<uint64_t, kLaneNum> lane_rd_task{};
  std::array<uint64_t, kLaneNum> lane_rd_user{};

  // 每拍端口占用标记。
  bool cmem_rd_used = false, cmem_wr_used = false, mmem_rd_used = false;
  bool mmem_wr_used = false, topk_used = false, smem_used = false;
};

}  // namespace bach
}  // namespace latch

#endif
