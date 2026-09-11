#ifndef _LATCH_BACH_IP_CHIP_CORE_RV_CORE_LSQ_
#define _LATCH_BACH_IP_CHIP_CORE_RV_CORE_LSQ_

// M5 · lsq 发射与返回。
//
// 按地址范围把访存分到四个目标：DTCM、Share Mem、Core Mem、Router I/O reg。
// 前者是核内的 SRAM，后三个走对外的端口。
//
//   dtcm      8 KB，4 bank 单端口，3 拍；可同时收 2 个不冲突 bank 的请求，
//             同 bank 冲突则阻塞第二条
//   sm_lsq    16 项，访问 Share Mem，5～10 拍，顺序执行，每拍仅发一个
//   cm_lsq    16 项，访问 Core Mem 与 Router I/O reg，15～25 拍，顺序执行，
//             每拍仅发一个；只有 DTE core 有
//
// 写回优先级：DTCM 读出的数据与 Share Mem / Core Mem 的数据同拍要写回时，
// 优先后者，阻塞 DTCM，因为那两条路的延迟长得多，让它们先走总时间更短。
//
// 本级只管时序。数据在功能模型执行那一条指令时就已经落进 gpr 了，这里发的是
// 请求、收的是「什么时候算数」，写回口只带寄存器编号。

#include <deque>
#include <memory>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/chip/core/rv_core/rv_ports.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

class RvLsq : public BachModule {
 public:
  RvLsq(ClockPtr clock, const std::string& name, bool has_cm,
        uint64_t parent = 0, bool tick = true)
      : BachModule(clock, name, parent, tick),
        cm_enabled(has_cm),
        req(std::make_shared<LsqReqPort>(clock)),
        wb(std::make_shared<GprWbPort>(clock)),
        smem(std::make_shared<MemPort>(clock)),
        cmem(std::make_shared<MemPort>(clock)),
        hdr(std::make_shared<MemPort>(clock)),
        sm_depth(clock),
        cm_depth(clock),
        served(clock) {}

  std::shared_ptr<LsqReqPort> ReqPtr() const { return req; }
  std::shared_ptr<GprWbPort> WbPtr() const { return wb; }
  MemPort& Smem() { return *smem; }
  void AttachSmem(std::shared_ptr<MemPort> p) { smem = std::move(p); }
  MemPort& Cmem() { return *cmem; }
  void AttachCmem(std::shared_ptr<MemPort> p) { cmem = std::move(p); }
  // Router CoreStation 的包头读口。DTE RV core 的 cm_lsq 按地址范围分流到
  // Core Mem 与这一条，包头只有这一条读取通路。
  MemPort& Hdr() { return *hdr; }
  void AttachHdr(std::shared_ptr<MemPort> p) { hdr = std::move(p); }

  bool HasRoom(LsqTarget t) const {
    if (t == LsqTarget::kDtcm) return true;
    if (t == LsqTarget::kShareMem) return sm_q.size() < kSmLsqDepth;
    return cm_enabled && cm_q.size() < kCmLsqDepth;
  }
  uint64_t Served() const { return serve_cnt; }
  bool Quiescent() const override {
    return sm_q.empty() && cm_q.empty() && sm_flight.empty() &&
           cm_flight.empty() && dtcm_pipe.empty();
  }

 protected:
  void Step() override {
    sm_used = false;
    cm_used = false;
    hdr_used = false;

    // 末级先做：先写回，再收响应，再发请求，最后收新的一笔。
    Writeback();
    Collect();
    Issue();
    Accept();

    if (!sm_used) smem->IdleReq();
    if (!cm_used) cmem->IdleReq();
    if (!hdr_used) hdr->IdleReq();

    sm_depth = sm_q.size();
    cm_depth = cm_q.size();
    served = serve_cnt;
    TracePerCycle("sm_q", sm_q.size());
    TracePerCycle("cm_q", cm_q.size());
  }

 private:
  struct Entry {
    LsqTarget target = LsqTarget::kDtcm;
    bool we = false;
    uint64_t addr = 0, wdata = 0, be = 0, rd_idx = 0;
  };
  struct Timed {
    uint64_t at = 0, rd_idx = 0;
    bool need_wb = false;
    // Share Mem 与 Core Mem 读回来的那 4 B。功能模型执行 lw 那一拍从这两块拿到
    // 的是占位值，真正的内容随写回一起补进目的寄存器；DTCM 是功能模型自己的
    // 存储，读那一刻就已经是真值，不带这一项。
    bool has_data = false;
    uint64_t data = 0;
  };

  // 写回优先级：Share Mem 与 Core Mem 排在 DTCM 前面。同一拍只写回一个。
  void Writeback() {
    uint64_t now = CycleNow();
    if (Pop(sm_done, now) || Pop(cm_done, now)) return;
    if (Pop(dtcm_pipe, now)) return;
    wb->Idle();
  }

  bool Pop(std::deque<Timed>& q, uint64_t now) {
    if (q.empty() || q.front().at > now) return false;
    Timed t = q.front();
    q.pop_front();
    if (!t.need_wb) return false;   // 写请求不写回，让下一路用这一拍
    if (t.has_data) {
      wb->DriveData(t.rd_idx, t.data, ++wb_seq);
    } else {
      wb->Drive(t.rd_idx, ++wb_seq);
    }
    ++serve_cnt;
    return true;
  }

  void Collect() {
    Take(*smem, sm_flight, sm_done);
    Take(*cmem, cm_flight, cm_done);
    Take(*hdr, hdr_flight, cm_done);
  }

  void Take(MemPort& port, std::deque<Entry>& flight,
            std::deque<Timed>& done) {
    if (!port.RspValid() || flight.empty()) return;
    Entry e = flight.front();
    flight.pop_front();
    done.push_back({CycleNow(), e.rd_idx, !e.we, !e.we, WordOf(port.RspData())});
  }

  static uint64_t WordOf(ByteBlockPtr const& d) {
    if (!d) return 0;
    uint64_t v = 0;
    for (uint64_t k = 0; k < 4 && k < d->size(); ++k) {
      v |= uint64_t((*d)[k]) << (8 * k);
    }
    return v;
  }

  // 两条队列各自顺序发射，每拍各发一个。
  void Issue() {
    IssueOne(sm_q, sm_flight, *smem, sm_used);
    if (!cm_enabled) return;
    if (cm_q.empty()) return;
    // Router I/O reg 复用 cm_lsq，只是出口换成 CoreStation 的包头读口。
    bool to_hdr = cm_q.front().target == LsqTarget::kRouterIo;
    if (to_hdr) {
      IssueOne(cm_q, hdr_flight, *hdr, hdr_used);
    } else {
      IssueOne(cm_q, cm_flight, *cmem, cm_used);
    }
  }

  void IssueOne(std::deque<Entry>& q, std::deque<Entry>& flight, MemPort& port,
                bool& used) {
    if (q.empty() || used || !port.Ready()) return;
    Entry e = q.front();
    if (e.we) {
      auto d = std::make_shared<ByteBlock>(4, 0);
      for (int k = 0; k < 4; ++k) (*d)[k] = uint8_t((e.wdata >> (8 * k)) & 0xFFu);
      port.Write(e.addr, d);
    } else {
      port.Read(e.addr, 4);
    }
    used = true;
    q.pop_front();
    flight.push_back(e);
  }

  void Accept() {
    // ready 是「下一拍一定收得下」：往这几个队列里放东西的只有指令执行器。
    bool room = sm_q.size() < kSmLsqDepth &&
                (!cm_enabled || cm_q.size() < kCmLsqDepth);
    req->DriveReady(room);
    if (!req->Valid() || req->Seq() == last_req_seq || !room) return;
    last_req_seq = req->Seq();

    Entry e;
    e.target = req->Target();
    e.we = req->We();
    e.addr = req->addr.Get();
    e.wdata = req->wdata.Get();
    e.be = req->be.Get();
    e.rd_idx = req->rd_idx.Get();

    switch (e.target) {
      case LsqTarget::kDtcm:
        // 核内 SRAM，不排队：4 bank 单端口 3 拍，同 bank 冲突阻塞第二条。
        dtcm_pipe.push_back({CycleNow() + DtcmDelay(e.addr), e.rd_idx, !e.we});
        break;
      case LsqTarget::kShareMem:
        sm_q.push_back(e);
        break;
      default:
        LOGCHECK(cm_enabled, "RvLsq: 这个实例没有 cm_lsq。");
        cm_q.push_back(e);
        break;
    }
  }

  // 同 bank 冲突则阻塞第二条：本拍这个 bank 已经被占，就往后排一拍。
  uint64_t DtcmDelay(uint64_t addr) {
    uint64_t now = CycleNow();
    uint64_t bank = (addr / 4) % kDtcmBanks;
    uint64_t at = now;
    if (bank_free[bank] > at) at = bank_free[bank];
    bank_free[bank] = at + 1;
    return at + kDtcmLatency;
  }

  bool cm_enabled;
  std::shared_ptr<LsqReqPort> req;
  std::shared_ptr<GprWbPort> wb;
  std::shared_ptr<MemPort> smem, cmem, hdr;

  std::deque<Entry> sm_q, cm_q;
  std::deque<Entry> sm_flight, cm_flight, hdr_flight;
  std::deque<Timed> sm_done, cm_done, dtcm_pipe;
  std::array<uint64_t, kDtcmBanks> bank_free{};
  bool sm_used = false, cm_used = false, hdr_used = false;
  uint64_t last_req_seq = 0, wb_seq = 0, serve_cnt = 0;

  Logic64 sm_depth, cm_depth, served;
};

}  // namespace bach
}  // namespace latch

#endif
