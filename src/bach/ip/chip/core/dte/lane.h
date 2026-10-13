#ifndef _LATCH_BACH_IP_CHIP_CORE_DTE_LANE_
#define _LATCH_BACH_IP_CHIP_CORE_DTE_LANE_

#include "bach/ip/chip/core/dte/buffer.h"
#include "bach/ip/chip/core/dte/dte_ports.h"
#include "bach/ip/chip/core/dte/hmem.h"
#include "bach/ip/chip/core/dte/task_queue.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace latch {
namespace bach {
constexpr uint64_t kRdOutstanding = kDteBufFlits;
constexpr uint64_t kDrainSlots = 1;
inline bool PayloadSeg(SegEndpoint ep) {
  return ep != SegEndpoint::kHeader && ep != SegEndpoint::kInvalid;
}
inline bool WrSeg(SegEndpoint ep) { return PayloadSeg(ep); }

// 一份内容（head/data/scale/topk）在波形里标记「从哪读 / 写到哪」的位置编码。与
// Python 侧 spans.py 的 DTE_*_LOC 信号、trace2perfetto.py 的 DTE_LOC_NAME 一一对应。
// 0 = none（这一半不搬这份内容，长度 0）；1 = Hmem（包头上下文表）；2 = Core Mem；
// 3 = Matrix Mem；4 = Router（进核的源端 / 出核的目的端）；5 = MU（topK_ep_table）。
enum DteLoc : uint64_t {
  kDteLocNone = 0,
  kDteLocHmem = 1,
  kDteLocCmem = 2,
  kDteLocMmem = 3,
  kDteLocRouter = 4,
  kDteLocMu = 5,
};

// Each half has one Active and one Hold. A younger task can issue non-last
// requests while Hold is occupied; its last request waits for Hold to drain.
class Lane : public BachModule {
public:
  Lane(ClockPtr clock, const std::string &name, uint64_t lane_idx,
       DteBuffer &buf, uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick), idx(lane_idx), buffer(buf),
        to_router(std::make_shared<CoreDataPort>(clock)),
        payload(std::make_shared<PayloadPort>(clock)),
        admit(std::make_shared<AdmitPort>(clock)),
        rd_done(std::make_shared<HalfDonePort>(clock)),
        wr_done(std::make_shared<HalfDonePort>(clock)) {
    for (auto &p : mem)
      p = std::make_shared<MemPort>(clock);
  }
  std::shared_ptr<AdmitPort> AdmitPtr() const { return admit; }
  bool HasRoom(uint64_t half) const { return !q[half].Full(); }
  MemPort &Cmem() { return *mem[0]; }
  MemPort &Mmem() { return *mem[1]; }
  void AttachCmem(std::shared_ptr<MemPort> p) { mem[0] = std::move(p); }
  void AttachMmem(std::shared_ptr<MemPort> p) { mem[1] = std::move(p); }
  void AttachTopk(std::shared_ptr<MemPort> p) { mem[2] = std::move(p); }
  void AttachHeader(std::shared_ptr<MemPort> p) { mem[3] = std::move(p); }
  CoreDataPort &ToRouter() { return *to_router; }
  void AttachToRouter(std::shared_ptr<CoreDataPort> p) {
    to_router = std::move(p);
  }
  void AttachTailDone(std::shared_ptr<Logic64> p) { tail_done = std::move(p); }
  std::shared_ptr<HalfDonePort> RdDonePtr() const { return rd_done; }
  std::shared_ptr<HalfDonePort> WrDonePtr() const { return wr_done; }
  void AttachPayload(std::shared_ptr<PayloadPort> p) { payload = std::move(p); }
  // Compatibility hooks for test construction; production traffic uses ports.
  void AttachHmem(Hmem &h) { hmem = &h; }
  uint64_t Outstanding(uint64_t half) const {
    return (active[half] ? active[half]->outstanding : 0) +
           (hold[half] ? hold[half]->outstanding : 0);
  }
  uint64_t ContextState(uint64_t half) const {
    return (active[half] ? 1 : 0) | (hold[half] ? 2 : 0) |
           (active[half] ? active[half]->seg << 4 : 0);
  }
  bool Quiescent() const override {
    return q[0].Empty() && q[1].Empty() && !active[0] && !active[1] &&
           !hold[0] && !hold[1] && !deferred && responses.empty() && !tx_stage;
  }

protected:
  void Step() override {
    used.fill(false);
    router_used = false;
    rresp_ev = false;
    wreq_ev = false;
    rresp_task = 0xFFu;
    rresp_user = 0xFFFFu;
    wreq_task = 0xFFu;
    wreq_user = 0xFFFFu;
    // 读写两半各一条轨道：rd_start/wr_start = 这一半激活一个任务，rd_done/wr_done =
    // 这一半把这个任务做完（读全回来 / 最后一拍发出去）。身份统一取 desc 的
    // task_id / user_id，读写两半的起点与终点各自对得上。
    rd_start_ev = false;
    rd_done_ev = false;
    wr_start_ev = false;
    wr_done_ev = false;
    rd_start_task = 0xFFu; rd_start_user = 0xFFFFu;
    rd_done_task = 0xFFu; rd_done_user = 0xFFFFu;
    wr_start_task = 0xFFu; wr_start_user = 0xFFFFu;
    wr_done_task = 0xFFu; wr_done_user = 0xFFFFu;
    rd_start_head = 0; rd_start_data = 0; rd_start_scale = 0; rd_start_topk = 0;
    wr_start_head = 0; wr_start_data = 0; wr_start_scale = 0; wr_start_topk = 0;
    rd_start_head_loc = 0; rd_start_data_loc = 0;
    rd_start_scale_loc = 0; rd_start_topk_loc = 0;
    wr_start_head_loc = 0; wr_start_data_loc = 0;
    wr_start_scale_loc = 0; wr_start_topk_loc = 0;

    rd_done->Idle();
    wr_done->Idle();
    Collect();
    Report();
    ForwardTxStage();
    TakeAdmit();
    Activate();
    TakePayload();
    Write();
    Read();
    for (uint64_t ep = 0; ep < 4; ++ep)
      if (!used[ep])
        mem[ep]->IdleReq();
    if (!router_used)
      to_router->Idle();
    TracePerCycle("rd_q", q[0].Size());
    TracePerCycle("wr_q", q[1].Size());
    // 一笔任务的两个端点：收到第一拍数据（rresp）与发完最后一拍 wreq。
    // 身份出核取 desc 的 STUPV，进核取包头那份 Message —— 两者在各自那条路上
    // 从收到发一路带着，rresp 与 wreq 配对得上。
    TracePerCycle("rresp", rresp_ev ? 1 : 0);
    TracePerCycle("rresp_task", rresp_task);
    TracePerCycle("rresp_user", rresp_user);
    TracePerCycle("wreq", wreq_ev ? 1 : 0);
    TracePerCycle("wreq_task", wreq_task);
    TracePerCycle("wreq_user", wreq_user);
    // 读写两半各自一条轨道：rd_start → rd_done 是读这一半，wr_start → wr_done 是
    // 写这一半，身份取 desc 的 task_id / user_id。
    TracePerCycle("rd_start", rd_start_ev ? 1 : 0);
    TracePerCycle("rd_start_task", rd_start_task);
    TracePerCycle("rd_start_user", rd_start_user);
    TracePerCycle("rd_done", rd_done_ev ? 1 : 0);
    TracePerCycle("rd_done_task", rd_done_task);
    TracePerCycle("rd_done_user", rd_done_user);
    TracePerCycle("wr_start", wr_start_ev ? 1 : 0);
    TracePerCycle("wr_start_task", wr_start_task);
    TracePerCycle("wr_start_user", wr_start_user);
    TracePerCycle("wr_done", wr_done_ev ? 1 : 0);
    TracePerCycle("wr_done_task", wr_done_task);
    TracePerCycle("wr_done_user", wr_done_user);
    // rd_start / wr_start 那一拍的四份内容（head/data/scale/topk）各多少字节，供
    // Perfetto 段上印 bytes 与理想/堵塞拍数。head 是固定的 32 B 包头上下文。
    TracePerCycle("rd_start_head", rd_start_head);
    TracePerCycle("rd_start_data", rd_start_data);
    TracePerCycle("rd_start_scale", rd_start_scale);
    TracePerCycle("rd_start_topk", rd_start_topk);
    TracePerCycle("wr_start_head", wr_start_head);
    TracePerCycle("wr_start_data", wr_start_data);
    TracePerCycle("wr_start_scale", wr_start_scale);
    TracePerCycle("wr_start_topk", wr_start_topk);
    // 同一拍各份内容从哪里读（rd）/写到哪（wr），位置编码见 DteLoc。
    TracePerCycle("rd_start_head_loc", rd_start_head_loc);
    TracePerCycle("rd_start_data_loc", rd_start_data_loc);
    TracePerCycle("rd_start_scale_loc", rd_start_scale_loc);
    TracePerCycle("rd_start_topk_loc", rd_start_topk_loc);
    TracePerCycle("wr_start_head_loc", wr_start_head_loc);
    TracePerCycle("wr_start_data_loc", wr_start_data_loc);
    TracePerCycle("wr_start_scale_loc", wr_start_scale_loc);
    TracePerCycle("wr_start_topk_loc", wr_start_topk_loc);
  }

private:
  struct Context {
    Descriptor desc;
    uint64_t seg = 0, offset = 0, outstanding = 0, filled = 0;
    bool issued = false, first = true, tail = false;
    std::deque<uint8_t> residue;
  };
  struct Flight {
    uint64_t seq = 0, slot = 0, skip = 0, bytes = 0, offset = 0;
  };
  struct BufferedResponse {
    Flight flight;
    ByteBlock bytes;
    uint64_t error = 0;
  };
  struct TxStage {
    uint64_t at = 0, bytes = 0, seq = 0;
    bool last = false, head = false;
    MessagePtr msg;
  };
  bool Outbound() const { return idx != kInCh; }
  static bool FromCm(Route r) { return r == Route::kCmToRouter; }
  static bool ToCm(Route r) {
    return r == Route::kRouterToCm || r == Route::kMmToCm;
  }

  // 读这一半：head/data/scale/topk 各从哪里读（源端）。
  //   data/scale：进核从 Router 的包来，出核按 route 落 Core Mem（CmToRouter）或
  //               Matrix Mem（其余），scale 旁带随数据落同一块；
  //   topk：进核从 Router 的包来，出核经 MU 的 topK 端口读；
  //   head（包头）：进核从 Router 的包头拍来，出核从落库处读回——计算 core 在
  //                Hmem（段 0 打 header tag），B/R core 在 Core Mem。
  static DteLoc MemLoc(SegEndpoint ep) {
    if (ep == SegEndpoint::kHeader) return kDteLocHmem;
    if (ep == SegEndpoint::kMmem) return kDteLocMmem;
    if (ep == SegEndpoint::kTopk) return kDteLocMu;
    return kDteLocCmem;
  }
  DteLoc RdLocHead(Descriptor const& d) const {
    if (IsInbound(d.route)) return kDteLocRouter;
    return MemLoc(d.seg[0].src_kind);
  }
  DteLoc RdLocData(Descriptor const& d) const {
    if (IsInbound(d.route)) return kDteLocRouter;
    return FromCm(d.route) ? kDteLocCmem : kDteLocMmem;
  }
  DteLoc RdLocScale(Descriptor const& d) const { return RdLocData(d); }
  DteLoc RdLocTopk(Descriptor const& d) const {
    return IsInbound(d.route) ? kDteLocRouter : kDteLocMu;
  }
  // 写这一半：head/data/scale/topk 各写到哪（目的端）。
  //   data/scale：进核按 route 落 Core Mem（RouterToCm）或 Matrix Mem（RouterToMm），
  //               出核发 Router（MM→CM 落 Core Mem），scale 旁带随数据落同一块；
  //   topk：发 Router 的出核（CmToRouter/MmToRouter）随包发出去，进核与 MM→CM 经
  //         旁带写进 MU 的 topK_ep_table；
  //   head（包头）：进核按 hw_header_op 保存到段0目的端或丢弃；出核发 Router，
  //                core内搬运按段0目的端写入。
  DteLoc WrLocHead(Descriptor const& d) const {
    if (IsInbound(d.route))
      return d.hw_header_op ? kDteLocNone : MemLoc(d.seg[0].dst_kind);
    return d.route == Route::kMmToCm ? MemLoc(d.seg[0].dst_kind) : kDteLocRouter;
  }
  DteLoc WrLocData(Descriptor const& d) const {
    if (IsInbound(d.route)) return ToCm(d.route) ? kDteLocCmem : kDteLocMmem;
    return d.route == Route::kMmToCm ? kDteLocCmem : kDteLocRouter;
  }
  DteLoc WrLocScale(Descriptor const& d) const { return WrLocData(d); }
  DteLoc WrLocTopk(Descriptor const& d) const {
    if (d.route == Route::kCmToRouter || d.route == Route::kMmToRouter)
      return kDteLocRouter;
    return kDteLocMu;
  }
  // 这份内容没搬（字节 0）时位置记 none，避免「长度 0 却给了个位置」。
  static uint64_t LocOrNone(DteLoc loc, uint64_t bytes) {
    return bytes ? uint64_t(loc) : uint64_t(kDteLocNone);
  }

  static uint64_t Tag(Descriptor const &d) {
    return IsInbound(d.route) ? d.frame_seq : d.commit_seq;
  }
  static uint64_t Endpoint(SegEndpoint ep) {
    return ep == SegEndpoint::kMmem     ? 1
           : ep == SegEndpoint::kTopk   ? 2
           : ep == SegEndpoint::kHeader ? 3
                                        : 0;
  }
  static uint64_t Length(Descriptor const &d, uint64_t seg) {
    return seg < 4 && d.seg[seg].valid ? (seg == 0 ? 32 : d.seg[seg].len) : 0;
  }
  static void Advance(Context &c) {
    while (c.seg < 4 &&
           (Length(c.desc, c.seg) == 0 || c.offset == Length(c.desc, c.seg))) {
      ++c.seg;
      c.offset = 0;
    }
  }
  static bool Last(Context const &c, uint64_t n) {
    if (c.offset + n != Length(c.desc, c.seg))
      return false;
    for (uint64_t s = c.seg + 1; s < 4; ++s)
      if (Length(c.desc, s))
        return false;
    return true;
  }
  Context *Owner(uint64_t half, uint64_t seq) {
    if (active[half] && active[half]->desc.commit_seq == seq)
      return &*active[half];
    if (hold[half] && hold[half]->desc.commit_seq == seq)
      return &*hold[half];
    return nullptr;
  }
  // 这笔任务的字节画像：data（Cmem/Mmem 数据段）、scale 旁带、topK 各多少字节。
  // 段长统一按字节计（与 Descriptor::PayloadBytes 一致）；scale/topK 看任一端 tag。
  static void ByteProfile(Descriptor const& d, uint64_t& data, uint64_t& scale,
                          uint64_t& topk) {
    data = 0;
    scale = 0;
    topk = 0;
    for (auto const& s : d.seg) {
      if (!s.valid || s.is_header) continue;
      if (s.src_kind == SegEndpoint::kTopk || s.dst_kind == SegEndpoint::kTopk) {
        topk += s.len;
      } else if (s.src_kind == SegEndpoint::kScale ||
                 s.dst_kind == SegEndpoint::kScale) {
        scale += s.len;
      } else {
        data += s.len;
      }
    }
  }

  // wreq 发完那一拍记一笔。身份取这一拍带的那份 Message：进核是包头那份（从收
  // payload 一路带到发 wreq），出核是 MakeOutboundMsg 造的那份（task_id / user_id
  // 都与 desc 一致）。拿不到 Message 就退回 desc 的 STUPV。
  void MarkWreq(MessagePtr const& m, Descriptor const& d) {
    wreq_ev = true;
    wreq_task = m ? (m->task_id & 0xFFu) : (d.task_id & 0xFFu);
    wreq_user = m ? (m->user_id & 0xFFFFu) : (d.user_id & 0xFFFFu);
    // 写这一半的终点（最后一拍发出），身份取 desc，与 wr_start 对得上。
    wr_done_ev = true;
    wr_done_task = d.task_id & 0xFFu;
    wr_done_user = d.user_id & 0xFFFFu;
  }

  void TakeAdmit() {
    if (admit->Valid() && admit->Seq() != last_admit) {
      auto d = admit->Desc();
      if (d) {
        LOGCHECK(!q[0].Full() && !q[1].Full(),
                 "Lane: admission exceeded reserved queue space");
        Descriptor copy = *d;
        if (IsInbound(copy.route))
          copy.frame_seq = ++frames;
        q[0].Push(copy);
        q[1].Push(copy);
        last_admit = admit->Seq();
      }
    }
    admit->DriveReady(!q[0].Full() && !q[1].Full());
  }
  void Activate() {
    for (uint64_t h = 0; h < 2; ++h)
      if (!active[h] && !q[h].Empty()) {
        Context c;
        c.desc = q[h].Front();
        if (IsInbound(c.desc.route) && c.desc.hw_header_op)
          c.seg = 1;
        uint64_t data_b, scale_b, topk_b;
        ByteProfile(c.desc, data_b, scale_b, topk_b);
        if (h == kRd) {
          rd_start_ev = true;
          rd_start_task = c.desc.task_id & 0xFFu;
          rd_start_user = c.desc.user_id & 0xFFFFu;
          rd_start_data = data_b;
          rd_start_scale = scale_b;
          rd_start_topk = topk_b;
          rd_start_head = kHeaderCtxBytes;
          rd_start_head_loc = uint64_t(RdLocHead(c.desc));
          rd_start_data_loc = LocOrNone(RdLocData(c.desc), data_b);
          rd_start_scale_loc = LocOrNone(RdLocScale(c.desc), scale_b);
          rd_start_topk_loc = LocOrNone(RdLocTopk(c.desc), topk_b);
        } else {
          wr_start_ev = true;
          wr_start_task = c.desc.task_id & 0xFFu;
          wr_start_user = c.desc.user_id & 0xFFFFu;
          wr_start_data = data_b;
          wr_start_scale = scale_b;
          wr_start_topk = topk_b;
          wr_start_head = WrLocHead(c.desc) == kDteLocNone ? 0 : kHeaderCtxBytes;
          wr_start_head_loc = uint64_t(WrLocHead(c.desc));
          wr_start_data_loc = LocOrNone(WrLocData(c.desc), data_b);
          wr_start_scale_loc = LocOrNone(WrLocScale(c.desc), scale_b);
          wr_start_topk_loc = LocOrNone(WrLocTopk(c.desc), topk_b);
        }
        q[h].Pop();
        active[h] = std::move(c);
      }
  }
  static ByteBlockPtr Slice(MessagePtr const &m, uint64_t off, uint64_t n) {
    auto b = std::make_shared<ByteBlock>(n, 0);
    if (m)
      for (uint64_t k = 0; k < n; ++k)
        (*b)[k] = m->WireByte(off + k);
    return b;
  }
  void TakePayload() {
    if (Outbound())
      return;
    bool buffer_written = false;
    if (deferred && buffer.HasRoom(idx)) {
      buffer.Push(idx, *deferred);
      deferred.reset();
      buffer_written = true;
    }
    if (payload->Valid() && payload->Seq() != last_payload && !deferred &&
        !buffer_written &&
        buffer.Credit(idx) >= 2) {
      // The header may precede the task by one beat. Payload acceptance waits
      // for the matching RD context; it never depends on descriptor dispatch.
      uint64_t frame = payload->frame.Get();
      auto m = payload->msg.Get();
      if (!active[0] || Tag(active[0]->desc) != frame) {
        payload->DriveReady(false);
        return;
      }
      bool last = payload->last.Get() != 0;
      uint64_t n = payload->bytes.Get(), off = payload->off.Get();
      if (off == 0) {
        rresp_ev = true;
        rresp_task = m ? (m->task_id & 0xFFu) : 0xFFu;
        rresp_user = m ? (m->user_id & 0xFFFFu) : 0xFFFFu;
      }
      if (off == 0 && header_frame != frame) {
        header_frame = frame;
        uint64_t expected = 0;
        for (uint64_t seg = 1; seg < 4; ++seg)
          expected += Length(active[0]->desc, seg);
        if (!m || expected != m->TotalBytes()) {
          active[0]->desc.error = 8;
          if (auto wr = Owner(1, active[0]->desc.commit_seq))
            wr->desc.error = 8;
        }
        if (!active[0]->desc.hw_header_op)
          buffer.Push(idx, {frame, 32, last && n == 0,
                            std::make_shared<ByteBlock>(HeaderOfMessage(*m)), m,
                            true});
        else
          buffer.Push(idx, {frame, n, last, Slice(m, off, n), m});
        if (hmem) {
          auto const &s = active[0]->desc.seg[0];
          hmem->SaveIdentity(s.dst_kind, s.dst_addr, *m);
        }
        if (n && !active[0]->desc.hw_header_op)
          deferred = BufBeat{frame, n, last, Slice(m, off, n), m};
      } else
        buffer.Push(idx, {frame, n, last, Slice(m, off, n), m});
      if (payload->error.Get()) {
        active[0]->desc.error = payload->error.Get();
        if (auto wr = Owner(1, active[0]->desc.commit_seq))
          wr->desc.error = payload->error.Get();
      }
      last_payload = payload->Seq();
      payload->accepted_seq = last_payload;
      if (last)
        inbound_done.insert(frame);
    }
    payload->DriveReady(!deferred && buffer.Credit(idx) >= 3 &&
                        active[0].has_value());
  }
  void Collect() {
    for (uint64_t ep = 0; ep < 4; ++ep) {
      MemPort &p = *mem[ep];
      bool read_response = p.RspValid() && !reads[ep].empty();
      if (read_response) {
        Flight f = reads[ep].front();
        reads[ep].pop_front();
        auto c = Owner(0, f.seq);
        LOGCHECK(c && c->outstanding, "Lane: read response without owner");
        if (c->desc.commit_seq != rd_seen_commit) {
          rd_seen_commit = c->desc.commit_seq;
          rresp_ev = true;
          rresp_task = c->desc.task_id & 0xFFu;
          rresp_user = c->desc.user_id & 0xFFFFu;
        }
        if (p.RspError() && !c->desc.error)
          c->desc.error = 7;
        ByteBlock bytes(f.bytes, 0);
        auto got = p.RspData();
        if (got)
          for (uint64_t k = 0; k < f.bytes && f.skip + k < got->size(); ++k)
            bytes[k] = (*got)[f.skip + k];
        responses.push_back({f, std::move(bytes), p.RspError() ? 7u : 0u});
      }
      // Dedicated memory backends report writes through rsp_valid. XBar
      // separates read data and write completion on its aggregate lane ports.
      bool write_response =
          p.WriteRspValid() ||
          (!read_response && p.RspValid() && !writes[ep].empty());
      if (write_response) {
        LOGCHECK(!writes[ep].empty(), "Lane: write response without flight");
        auto c = Owner(1, writes[ep].front());
        writes[ep].pop_front();
        LOGCHECK(c && c->outstanding, "Lane: write response without owner");
        --c->outstanding;
        if (!c->desc.error &&
            (p.WriteRspValid() ? p.WriteRspError() : p.RspError()))
          c->desc.error = 7;
      }
    }
    // Returned data waits in its reserved context until the single Buffer
    // write port accepts it. Outstanding includes these waiting responses.
    if (!responses.empty()) {
      auto &response = responses.front();
      auto owner = Owner(kRd, response.flight.seq);
      LOGCHECK(owner && owner->outstanding,
               "Lane: buffered read response without owner");
      buffer.Complete(idx, response.flight.slot, response.bytes,
                      response.flight.offset, response.error);
      --owner->outstanding;
      responses.pop_front();
    }
  }
  void Report() {
    if (hold[0] && hold[0]->outstanding == 0) {
      auto &c = *hold[0];
      rd_done->Drive(c.desc.commit_seq, kRd, true, c.desc.error);
      rd_done_ev = true;
      rd_done_task = c.desc.task_id & 0xFFu;
      rd_done_user = c.desc.user_id & 0xFFFFu;
      rd_complete.insert(c.desc.commit_seq);
      hold[0].reset();
    }
    if (hold[1] && hold[1]->outstanding == 0 &&
        rd_complete.count(hold[1]->desc.commit_seq)) {
      auto &c = *hold[1];
      if (c.tail && tail_done && tail_done->Get() < c.desc.commit_seq)
        return;
      if (!buffer.TaskDrained(idx, Tag(c.desc)))
        return;
      wr_done->Drive(c.desc.commit_seq, kWr, true, c.desc.error);
      rd_complete.erase(c.desc.commit_seq);
      hold[1].reset();
    }
  }
  void Finish(uint64_t h) {
    if (!active[h] || !active[h]->issued || hold[h])
      return;
    if (h == kWr)
      MarkWreq(active[h]->desc.msg, active[h]->desc);
    hold[h] = std::move(active[h]);
    active[h].reset();
  }
  void Read() {
    if (!active[0])
      return;
    auto &c = *active[0];
    if (c.desc.error && !Outbound()) {
      if (!inbound_done.erase(Tag(c.desc)))
        return;
      c.issued = true;
      Finish(0);
      return;
    }
    if (!Outbound()) {
      if (inbound_done.erase(Tag(c.desc)))
        c.issued = true;
      Finish(0);
      return;
    }
    Advance(c);
    if (c.seg >= 4) {
      c.issued = true;
      Finish(0);
      return;
    }
    auto const &s = c.desc.seg[c.seg];
    uint64_t ep = Endpoint(s.src_kind);
    uint64_t addr = s.src_addr + c.offset, skip = 0, n = 0;
    if (ep == 3)
      n = 32;
    else if (ep == 2)
      n = 128;
    else {
      skip = addr % 256;
      n = std::min(256 - skip, Length(c.desc, c.seg) - c.offset);
    }
    if (Last(c, n) && hold[0])
      return;
    if (c.outstanding + (hold[0] ? hold[0]->outstanding : 0) >= kRdOutstanding)
      return;
    if (used[ep] || !mem[ep]->Ready() || !buffer.HasRoom(idx))
      return;
    if (ep < 2 && !mem[ep]->PairReady())
      return;
    uint64_t slot;
    if (ep == 2 && c.offset % 256 == 128) {
      slot = topk_slot;
    } else {
      uint64_t reserve = ep == 2 ? 256 : n;
      slot =
          buffer.Reserve(idx, {Tag(c.desc), reserve, Last(c, ep == 2 ? 256 : n),
                               nullptr, c.desc.msg, c.seg == 0});
      if (ep == 2)
        topk_slot = slot;
    }
    if (ep < 2) {
      mem[ep]->ReadPair(addr - skip);
      if (s.src_kind == SegEndpoint::kScale) {
        mem[ep]->req_scale_en = kScalePhysical;
        mem[ep]->req_hw_addr0 = (addr - skip) / 128 | (1ull << 13);
        mem[ep]->req_hw_addr1 = ((addr - skip) / 128 + 1) | (1ull << 13);
      }
    } else
      mem[ep]->Read(addr, n);
    if (ep == 2) {
      mem[ep]->req_topk_sel = 1;
      mem[ep]->req_lane_mask = 1;
      mem[ep]->req_hw_addr0 = addr / 128;
    }
    if (ep == 3)
      mem[ep]->req_hw_addr0 = addr;
    reads[ep].push_back(
        {c.desc.commit_seq, slot, skip, n, ep == 2 ? c.offset % 256 : 0});
    used[ep] = true;
    ++c.outstanding;
    c.offset += n;
    if (LastAfter(c)) {
      c.issued = true;
      Finish(0);
    }
  }
  static bool LastAfter(Context &c) {
    Advance(c);
    return c.seg >= 4;
  }
  void Write() {
    if (!active[1])
      return;
    auto &c = *active[1];
    if (c.desc.error && !(Outbound() && c.desc.route != Route::kMmToCm)) {
      if (!buffer.Empty(idx) && buffer.Front(idx).tag == Tag(c.desc))
        buffer.Pop(idx);
      c.residue.clear();
      if (!rd_complete.count(c.desc.commit_seq) ||
          !buffer.TaskDrained(idx, Tag(c.desc)))
        return;
      c.issued = true;
      Finish(1);
      return;
    }
    if (c.issued) {
      Finish(1);
      return;
    }
    bool network = Outbound() && c.desc.route != Route::kMmToCm;
    if (network) {
      WriteNetwork(c);
      return;
    }
    Advance(c);
    if (c.seg >= 4) {
      // Zero-length marker belongs to the same input frame.
      if (!buffer.Empty(idx) && buffer.Front(idx).tag == Tag(c.desc) &&
          buffer.Front(idx).bytes == 0)
        buffer.Pop(idx);
      c.issued = true;
      Finish(1);
      return;
    }
    if (c.residue.empty()) {
      if (buffer.Empty(idx) || buffer.Front(idx).tag != Tag(c.desc))
        return;
      auto b = buffer.Front(idx);
      buffer.Pop(idx);
      if (b.error)
        c.desc.error = b.error;
      if (b.data)
        c.residue.insert(c.residue.end(), b.data->begin(), b.data->end());
    }
    if (c.residue.empty())
      return;
    auto const &s = c.desc.seg[c.seg];
    uint64_t ep = Endpoint(s.dst_kind);
    uint64_t addr = s.dst_addr + c.offset, skip = ep < 2 ? addr % 256 : 0;
    uint64_t cap = ep == 3 ? 32 : ep == 2 ? 128 : 256 - skip;
    uint64_t n = std::min(cap, Length(c.desc, c.seg) - c.offset);
    if (c.residue.size() < n) {
      if (buffer.Empty(idx) || buffer.Front(idx).tag != Tag(c.desc))
        return;
      auto beat = buffer.Front(idx);
      buffer.Pop(idx);
      if (beat.error)
        c.desc.error = beat.error;
      if (beat.data)
        c.residue.insert(c.residue.end(), beat.data->begin(), beat.data->end());
      if (c.residue.size() < n)
        return;
    }
    if (Last(c, n) && hold[1])
      return;
    if (used[ep] || !mem[ep]->Ready() || (ep < 2 && !mem[ep]->PairReady()))
      return;
    auto bytes = std::make_shared<ByteBlock>(ep < 2 ? 256 : n, 0);
    for (uint64_t k = 0; k < n; ++k) {
      (*bytes)[skip + k] = c.residue.front();
      c.residue.pop_front();
    }
    if (ep < 2) {
      mem[ep]->WritePair(addr - skip, bytes, skip, n);
      if (s.dst_kind == SegEndpoint::kScale) {
        mem[ep]->req_scale_en = kScalePhysical;
        mem[ep]->req_hw_addr0 = (addr - skip) / 128 | (1ull << 13);
        mem[ep]->req_hw_addr1 = ((addr - skip) / 128 + 1) | (1ull << 13);
      }
    } else
      mem[ep]->Write(addr, bytes);
    if (ep == 2) {
      mem[ep]->req_topk_sel = 1;
      mem[ep]->req_lane_mask = 1;
      mem[ep]->req_hw_addr0 = addr / 128;
    }
    if (ep == 3)
      mem[ep]->req_hw_addr0 = addr;
    writes[ep].push_back(c.desc.commit_seq);
    ++c.outstanding;
    used[ep] = true;
    c.offset += n;
    if (LastAfter(c)) {
      c.issued = true;
      Finish(1);
    }
  }
  void WriteNetwork(Context &c) {
    if (c.residue.size() < 256 && !c.tail && !buffer.Empty(idx) &&
        buffer.Front(idx).tag == Tag(c.desc)) {
      auto b = buffer.Front(idx);
      if (b.is_header) {
        if (!c.first)
          return;
        if (b.data && c.desc.msg) {
          auto &m = *c.desc.msg;
          ApplyHeader(m, *b.data);
          if (hmem)
            hmem->RestoreIdentity(c.desc.seg[0].src_kind,
                                  c.desc.seg[0].src_addr, m);
          if (!c.desc.HasScale() && !c.desc.seg[2].valid)
            m.scale_valid = 0;
          if (!c.desc.HasTopk() && !c.desc.seg[3].valid)
            m.topk_valid = 0;
          if (m.scale_valid)
            m.logical_data_bytes = c.desc.seg[1].len;
          {
            m.user_id = c.desc.user_id;
            m.path_id = c.desc.path_id;
            m.vc = c.desc.vc;
            m.loopback = (m.loopback & ~12) | (m.vc << 2);
          }
        }
      } else if (b.data) {
        c.residue.insert(c.residue.end(), b.data->begin(), b.data->end());
        if (c.desc.msg) {
          auto &data = c.desc.msg->payload;
          for (auto byte : *b.data) {
            if (c.filled < data.size())
              data[c.filled] = byte;
            ++c.filled;
          }
        }
      }
      if (b.error)
        c.desc.error = b.error;
      c.tail = b.last;
      buffer.Pop(idx);
    }
    bool staged = idx == kInnerLane;
    if ((c.residue.size() < 256 && !c.tail) ||
        (staged ? tx_stage.has_value() : !to_router->Ready()))
      return;
    uint64_t n = std::min<uint64_t>(256, c.residue.size());
    bool last = c.tail && c.residue.size() <= 256;
    if (last && hold[1])
      return;
    if (staged)
      tx_stage = TxStage{CycleNow() + 1, n, c.desc.commit_seq, last, c.first,
                         c.desc.msg};
    else {
      to_router->Drive(n, last, c.first, c.desc.vc, c.desc.msg);
      router_used = true;
    }
    c.first = false;
    for (uint64_t k = 0; k < n; ++k)
      c.residue.pop_front();
    if (last && !staged)
      FinishNetwork(c);
  }
  void ForwardTxStage() {
    if (!tx_stage || tx_stage->at > CycleNow() || !to_router->Ready())
      return;
    LOGCHECK(active[kWr] && active[kWr]->desc.commit_seq == tx_stage->seq,
             "Lane: staged Router flit lost its context");
    auto &context = *active[kWr];
    to_router->Drive(tx_stage->bytes, tx_stage->last, tx_stage->head,
                     context.desc.vc, tx_stage->msg);
    router_used = true;
    bool last = tx_stage->last;
    tx_stage.reset();
    if (last)
      FinishNetwork(context);
  }
  void FinishNetwork(Context &c) {
    if (c.desc.msg && c.desc.msg->topk_valid &&
        c.desc.msg->payload.size() >= 256)
      c.desc.msg->topk.assign(c.desc.msg->payload.end() - 256,
                              c.desc.msg->payload.end());
    c.tail = true;
    c.issued = true;
    Finish(kWr);
  }
  uint64_t idx;
  DteBuffer &buffer;
  std::array<std::shared_ptr<MemPort>, 4> mem;
  std::array<bool, 4> used{};
  std::array<std::deque<Flight>, 4> reads;
  std::deque<BufferedResponse> responses;
  std::array<std::deque<uint64_t>, 4> writes;
  std::array<TaskQueue, 2> q{{TaskQueue(1), TaskQueue(4)}};
  std::array<std::optional<Context>, 2> active, hold;
  std::set<uint64_t> inbound_done, rd_complete;
  std::optional<BufBeat> deferred;
  std::optional<TxStage> tx_stage;
  std::shared_ptr<CoreDataPort> to_router;
  std::shared_ptr<PayloadPort> payload;
  std::shared_ptr<AdmitPort> admit;
  std::shared_ptr<HalfDonePort> rd_done, wr_done;
  std::shared_ptr<Logic64> tail_done;
  Hmem *hmem = nullptr;
  uint64_t frames = 0, last_admit = 0, last_payload = 0, header_frame = 0,
           topk_slot = 0;
  bool router_used = false;
  // 一笔任务两端的波形事件：rresp（收到第一拍数据）与 wreq（发完最后一拍）。
  // 本拍没有就填 0xFF / 0xFFFF，Python 侧按 (user, task) 配对。
  bool rresp_ev = false, wreq_ev = false;
  uint64_t rresp_task = 0xFFu, rresp_user = 0xFFFFu;
  uint64_t wreq_task = 0xFFu, wreq_user = 0xFFFFu;
  // 读/写拆分：读轨道 = RD 半激活(rd_start) → RD 半读完(rd_done)，
  // 写轨道 = WR 半激活(wr_start) → 发完最后一拍(wr_done)。都用 desc 的身份
  // (task_id/user_id)，进核/出核两侧一致，Python 侧按 (user, task) 配对。
  bool rd_start_ev = false, rd_done_ev = false;
  bool wr_start_ev = false, wr_done_ev = false;
  uint64_t rd_start_task = 0xFFu, rd_start_user = 0xFFFFu;
  uint64_t rd_done_task = 0xFFu, rd_done_user = 0xFFFFu;
  uint64_t wr_start_task = 0xFFu, wr_start_user = 0xFFFFu;
  uint64_t wr_done_task = 0xFFu, wr_done_user = 0xFFFFu;
  // rd_start / wr_start 那一拍带上这笔任务四份内容（head/data/scale/topk）的字节数。
  // head 是固定 32 B 的包头上下文，进核丢头时写这一半记 0。
  uint64_t rd_start_head = 0, rd_start_data = 0, rd_start_scale = 0, rd_start_topk = 0;
  uint64_t wr_start_head = 0, wr_start_data = 0, wr_start_scale = 0, wr_start_topk = 0;
  // 同一拍各份内容从哪里读（rd）/写到哪（wr）的位置编码，见 DteLoc。
  uint64_t rd_start_head_loc = 0, rd_start_data_loc = 0;
  uint64_t rd_start_scale_loc = 0, rd_start_topk_loc = 0;
  uint64_t wr_start_head_loc = 0, wr_start_data_loc = 0;
  uint64_t wr_start_scale_loc = 0, wr_start_topk_loc = 0;
  // 出核：上一次报过 rresp 起点的 commit_seq，用它认“新任务的第一笔响应”。
  uint64_t rd_seen_commit = ~0ull;
};
} // namespace bach
} // namespace latch
#endif
