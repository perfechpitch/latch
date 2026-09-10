#ifndef _LATCH_BACH_IP_CHIP_CORE_ROUTER_CORE_STATION_
#define _LATCH_BACH_IP_CHIP_CORE_ROUTER_CORE_STATION_

// CoreStation：Router 与本 core 的 DTE 之间那一段。
//
// 进 core 与出 core 两条数据通路完全并行，互不共享仲裁状态。
//
// 进 core（M7、M8）：
//   三态准入 —— UserID 已分配则直接收；未分配但 stream credit 表有空项则记录
//   占用后收；无空项时该 VC 不能向 Core 发数据，但 VC 还有空位时仍可继续从上游
//   收。已通过 Stream 检查的包进 core 不再查 Core 方向的 VC credit，一定有
//   Core Mem 空间。
//
//   Header 写进 HeaderFIFO，Payload 写进 OutputBuffer，两者保持同一包的顺序与
//   边界。Header 就绪即通知 TS，不等整包收完 —— OutputBuffer 因此是流水缓冲
//   而不是整包缓冲，进 core 的包长不受它的容量约束。
//
//   trigger 请求发出后保持到 TS 拉 ready。TS 入口占满时本笔原地保持，不发下一笔，
//   也不清 HeaderFIFO 的队头：丢一笔 trigger 就等于丢一个 token。
//
// 出 core（M9）：DTE 已经查好 VC 与下游资源，这里只拆出 Header 与 Payload，
// 按 Header 的 VC 号写进 Core 方向的输入 VC，之后与其他方向一样查表、参与仲裁。
//
// 包头读口：DTE RV core 经 cm_lsq 映射到 Router I/O reg 的地址段读队头那个包的
// 包头，读完往弹出地址写 1，队头出队、下一个包头映射上来。core 内不另设第二条
// 读包头的通路。队列满时对进 core 的数据反压 —— 丢一个包头就等于丢一个 token
// 的搬运任务。

#include <deque>
#include <set>
#include <string>

#include "base/log.h"
#include "bach/ip/chip/core/ports.h"
#include "bach/ip/chip/core/router/router_ports.h"
#include "bach/ip/chip/core/router/xbar.h"
#include "bach/ip/module_base.h"

namespace latch {
namespace bach {

// HeaderFIFO 与 OutputBuffer 的深度，建模计划的默认值。60 flit 取自 DATA_NOC
// HAS 的 DTE-local 桥接 Router→DTE 60 flits。
constexpr uint64_t kHeaderFifoDepth = 16;
constexpr uint64_t kOutBufFlits = 60;

// 包头读口的地址布局：队头那个包的包头按 4 B 一项排开，读哪一项由地址的低位定。
// 写 kHdrPopOffset 那一项把队头弹出。硬件的包头位域还没定死，这里按模型里
// Message 的字段排，够 kernel 生成一笔搬运任务。
enum HdrRegOffset : uint64_t {
  kHdrUserId = 0,
  kHdrPathId = 4,
  kHdrSize = 8,
  kHdrCoreMask = 12,
  kHdrStreamId = 16,
  kHdrTaskId = 20,
  kHdrCompute = 24,
  kHdrReissue = 28,
  kHdrPopOffset = 32,
  kHdrRegBytes = 36,
};

// 包头就在 Router 里，读它比读 Core Mem 快。原文没给拍数，取一拍。
constexpr uint64_t kHdrReadLatency = 1;

class CoreStation : public BachModule {
 public:
  CoreStation(ClockPtr clock, const std::string& name, uint64_t parent = 0,
              bool tick = true)
      : BachModule(clock, name, parent, tick),
        from_xbar(std::make_shared<LinkEnd>(clock)),
        to_dte(std::make_shared<CoreDataPort>(clock)),
        from_dte(std::make_shared<CoreDataPort>(clock)),
        trigger(std::make_shared<TriggerPort>(clock)),
        level(std::make_shared<ReadyLevelPort>(clock)),
        to_station(std::make_shared<LinkEnd>(clock)),
        hdr(std::make_shared<MemPort>(clock)),
        admitted(clock),
        rejected(clock),
        triggers(clock) {}

  // Xbar 的 core 出口接这里。
  void AttachFromXbar(LinkEndPtr wire) { from_xbar = std::move(wire); }
  LinkEnd& FromXbar() { return *from_xbar; }

  CoreDataPort& ToDte() { return *to_dte; }
  std::shared_ptr<CoreDataPort> ToDtePtr() const { return to_dte; }
  CoreDataPort& FromDte() { return *from_dte; }
  std::shared_ptr<CoreDataPort> FromDtePtr() const { return from_dte; }
  TriggerPort& Trigger() { return *trigger; }
  std::shared_ptr<TriggerPort> TriggerPtr() const { return trigger; }

  // 出核数据交给 core 方向的 RouterStation。
  void AttachToStation(LinkEndPtr wire) { to_station = std::move(wire); }
  LinkEnd& ToStation() { return *to_station; }

  // 包头读口，接 DTE RV core 的 cm_lsq。本模块是从端。
  MemPort& Hdr() { return *hdr; }
  std::shared_ptr<MemPort> HdrPtr() const { return hdr; }
  void AttachHdr(std::shared_ptr<MemPort> p) { hdr = std::move(p); }
  uint64_t HeaderDepth() const { return hdr_fifo.size(); }
  // 进核那一段还压着几个 flit。
  uint64_t OutBufDepth() const { return out_buf.size(); }
  uint64_t Popped() const { return pop_cnt; }

  // 进 core 的 stream credit 表：16 项，与 ReduceModule 的用户上下文一一对应。
  bool HoldsUser(uint64_t user) const { return stream_tab.count(user) != 0; }
  uint64_t StreamUsed() const { return stream_tab.size(); }
  // Retire 从这个口广播退休的 user。表由本模块的协程改，Retire 只送号。
  void AttachRetire(std::shared_ptr<RetireBroadcastPort> p) {
    retire_in = std::move(p);
  }

  void RetireUser(uint64_t user) { stream_tab.erase(user); }

  uint64_t Admitted() const { return admitted.Get(); }
  uint64_t Rejected() const { return rejected.Get(); }
  uint64_t Triggers() const { return triggers.Get(); }

  // Xbar 读它决定 core 方向能不能收。电平每拍发布，Xbar 读上一拍的值。
  bool CoreReady() const { return out_buf.size() < kOutBufFlits; }
  std::shared_ptr<ReadyLevelPort> LevelPtr() const { return level; }

  bool Quiescent() const override {
    return out_buf.empty() && !trig_hold;
  }

 protected:
  void Step() override {
    TakeRetire();
    // 末级先做。
    ServeHdr();
    DrainToDte();
    PushTrigger();
    AcceptFromXbar();
    AcceptFromDte();

    // 准入电平：本拍末算出来，Xbar 下一拍读到。
    level->Drive(CoreReady());
    admitted = admitted_pending;
    rejected = rejected_pending;
    triggers = trigger_pending;
    TracePerCycle("out_buf", out_buf.size());
    TracePerCycle("hdr_fifo", hdr_fifo.size());
  }

 private:
  // 收 Retire 的广播。同一笔会连着两拍出现在端口上，按序号认它。
  void TakeRetire() {
    if (!retire_in || !retire_in->Valid()) return;
    if (retire_in->Seq() == last_retire_seq) return;
    last_retire_seq = retire_in->Seq();
    RetireUser(retire_in->User());
  }

  std::shared_ptr<RetireBroadcastPort> retire_in;
  uint64_t last_retire_seq = 0;

  struct Pending {
    uint64_t user_id = 0, path_id = 0;
    bool reissue = false, compute = false;
  };

  // 包头读口的从端：读队头那个包的包头，写 kHdrPopOffset 把它弹出。
  void ServeHdr() {
    ByteBlockPtr data;
    bool rsp = false;
    // 上一笔读的响应到点了就回。
    if (hdr_rsp_at != 0 && CycleNow() >= hdr_rsp_at) {
      data = hdr_rsp;
      hdr_rsp = ByteBlockPtr();
      hdr_rsp_at = 0;
      rsp = true;
    }
    MemReqView r = ReadMemReq(*hdr);
    if (r.valid && r.seq != last_hdr_seq) {
      last_hdr_seq = r.seq;
      if (r.we) {
        // 读完写 1 弹出，下一个包头映射上来。队列空时写进来是软件配错了。
        LOGCHECK(!hdr_fifo.empty(),
                 "CoreStation: 包头队列是空的，没有可弹出的。");
        hdr_fifo.pop_front();
        ++pop_cnt;
      } else {
        hdr_rsp = std::make_shared<ByteBlock>(HeaderBytes(r.addr, r.bytes));
        hdr_rsp_at = CycleNow() + kHdrReadLatency;
      }
    }
    hdr->DriveSlave(true, rsp, data);
  }

  // 队头那个包的包头，按 4 B 一项排开。队列空时读出来是全 0。
  ByteBlock HeaderBytes(uint64_t addr, uint64_t bytes) const {
    ByteBlock out(bytes == 0 ? 4 : bytes, 0);
    if (hdr_fifo.empty()) return out;
    MessagePtr const& m = hdr_fifo.front();
    uint64_t off = addr % kHdrRegBytes;
    uint64_t v = 0;
    switch (off) {
      case kHdrUserId: v = m->user_id; break;
      case kHdrPathId: v = m->path_id; break;
      case kHdrSize: v = m->size; break;
      case kHdrCoreMask: v = m->path_core_mask; break;
      case kHdrStreamId: v = m->stream_id; break;
      case kHdrTaskId: v = m->task_id; break;
      case kHdrCompute: v = m->compute; break;
      case kHdrReissue: v = m->reissue; break;
      default: v = 0; break;
    }
    for (uint64_t i = 0; i < out.size() && i < 8; ++i) {
      out[i] = uint8_t((v >> (8 * i)) & 0xFF);
    }
    return out;
  }

  // M7：三态准入。
  void AcceptFromXbar() {
    FlitView f = ReadFlit(from_xbar->flit);
    if (!f.valid || !f.msg) return;
    uint64_t user = f.msg->user_id;
    if (stream_tab.count(user) == 0) {
      if (stream_tab.size() >= kStreamTabEntries) {
        // 无空项：该 VC 不能向 Core 发数据。上游的 VC 还有空位时照样能继续收，
        // 所以这里只是不收本笔，不影响别的方向。
        ++rejected_pending;
        return;
      }
      stream_tab.insert(user);
    }
    if (out_buf.size() >= kOutBufFlits) {
      ++rejected_pending;
      return;
    }
    // 头 flit 要占一个包头槽。队列满就整笔不收 —— 丢一个包头就等于丢一个
    // token 的搬运任务，DTE RV core 再也拿不到它。
    if (f.head && hdr_fifo.size() >= kHeaderFifoDepth) {
      ++rejected_pending;
      return;
    }
    out_buf.push_back(f);
    ++admitted_pending;
    // 头 flit 同时进 HeaderFIFO，并排一笔 trigger。
    if (f.head) {
      hdr_fifo.push_back(f.msg);
      trig_q.push_back({user, f.msg->path_id, f.msg->reissue != 0,
                        f.msg->compute != 0});
    }
  }

  // M8：Header 就绪即通知 TS，请求保持到 TS 拉 ready。
  void PushTrigger() {
    if (trig_hold) {
      if (trigger->Ready()) {
        trig_hold = false;
        ++trigger_pending;
      } else {
        // 原地保持，不发下一笔，也不清 HeaderFIFO 的队头。
        return;
      }
    }
    if (trig_q.empty()) {
      trigger->Idle();
      return;
    }
    Pending p = trig_q.front();
    trig_q.pop_front();
    trigger->Drive(p.user_id, p.path_id, p.reissue, p.compute);
    trig_hold = true;
  }

  // DTE 被调度后按 AXI-Stream-Like 的 valid 边收边搬，数据没到就停在原拍等。
  void DrainToDte() {
    if (out_buf.empty()) {
      to_dte->Idle();
      return;
    }
    if (sent_hold && !to_dte->Ready()) {
      // 反压时保持 valid、当前 Header、首尾标志与有效字节不变，位置不前移。
      return;
    }
    if (sent_hold && to_dte->Ready()) {
      out_buf.pop_front();
      sent_hold = false;
      if (out_buf.empty()) {
        to_dte->Idle();
        return;
      }
    }
    FlitView const& f = out_buf.front();
    to_dte->Drive(f.bytes, f.tail, f.head, f.vc, f.msg);
    sent_hold = true;
  }

  // M9：出 core 拆包，按 Header 的 VC 号写进 core 方向的输入 VC。
  void AcceptFromDte() {
    CoreDataView d = ReadCoreData(*from_dte);
    if (!d.valid) {
      to_station->flit.Idle();
      to_station->release.Idle();
      from_dte->DriveReady(true);
      return;
    }
    to_station->flit.Drive(d.vc, d.hdr, d.last, d.bytes, d.msg);
    to_station->release.Idle();
    from_dte->DriveReady(true);
  }

  LinkEndPtr from_xbar, to_station;
  std::shared_ptr<MemPort> hdr;
  std::shared_ptr<CoreDataPort> to_dte, from_dte;
  std::shared_ptr<TriggerPort> trigger;
  std::shared_ptr<ReadyLevelPort> level;

  // Step 独占。
  std::deque<FlitView> out_buf;
  std::deque<MessagePtr> hdr_fifo;
  std::deque<Pending> trig_q;
  std::set<uint64_t> stream_tab;
  bool trig_hold = false, sent_hold = false;
  ByteBlockPtr hdr_rsp;
  uint64_t hdr_rsp_at = 0, last_hdr_seq = 0, pop_cnt = 0;
  uint64_t admitted_pending = 0, rejected_pending = 0, trigger_pending = 0;

  Logic64 admitted, rejected, triggers;
};

}  // namespace bach
}  // namespace latch

#endif
