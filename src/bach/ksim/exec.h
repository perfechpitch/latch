#ifndef _LATCH_BACH_KSIM_EXEC_
#define _LATCH_BACH_KSIM_EXEC_

// 核里的执行体。它是从属单元，由 Core 在自己的 Cycle 体内调 Step。
//
// 四条互不相干的通道：矩阵核、向量核、搬运的出方向、搬运的入方向。指令按 kernel 里的
// 先后顺序发射，发射出去之后各走各的通道，所以这一层把结果推出去的同时，下一层的输入
// 可以正在收进来。
//
// 一条指令发得出去要两个条件都成立：它要占的那条通道空着，而且它要读的每一块数据都已
// 经写好了。数据依赖按地址认：编译器把每一块排在各自的地址上，写的那条没做完，读它的
// 那条就发不出去。顺序发射，一条卡住后面的都等着。
//
// 四种指令各自怎么占时间：
//
//   Gemm  Elem   占 cycles 拍。这个数来自实测表，查不到时由编译器按权重加载量估
//   Send        逐拍把这一包推到链路上，占一次 dte_setup 加按带宽算的传输时间。推完
//               就算完，不等对端收到。发出去也意味着这一层的缓冲腾出来了，额度这时还
//   Recv        占住入方向，等收够 length 字节。收齐才算写好了那一块
//
// 占用与等待都记进观测：算的记在矩阵核或向量核名下，推数据记在搬运名下，停在 Recv 上
// 那一段记成等对端数据到达。
//
// 包上带的那个 tag 认的是这是第几份输入，它跟着数据走：收进来的 tag 排在队里，这一层
// 的 Send 取队头带出去。一份输入从进阵列到出阵列的延迟就是靠它配对的，核在中间换了个
// 号的话就配不上了。

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

#include "base/log.h"
#include "base/runtime.h"
#include "base/time_stamp.h"
#include "bach/ip/sub_unit.h"
#include "bach/ksim/kernel.h"
#include "bach/ksim/router.h"
#include "bach/observer/span_recorder.h"

namespace latch {
namespace bach {
namespace ksim {

struct CoreParams {
  uint64_t dte_setup = 4;
  // 本核推数据出去的带宽，字节每拍。与 Router 那一跳的带宽是同一条线。
  uint64_t link_bw = 32;
};

// 核做完一层之后把额度还给上游。
class CreditSink {
 public:
  virtual ~CreditSink() = default;
  virtual void ReturnCredit(uint64_t core) = 0;
};

class Exec : public SubUnit, public FlitSink {
 public:
  Exec(ClockPtr clock, uint64_t core_id, CoreKernel const* kernel,
       Router* router, CoreParams const& params, SpanRecorder* recorder,
       std::string const& name, uint64_t parent, bool standalone = false)
      : SubUnit(clock, standalone),
        id(core_id),
        prog(kernel),
        rt(router),
        par(params),
        rec(recorder) {
    LOGCHECK(rt != nullptr, "Exec: router is null.");
    RegisterId(name, parent);
  }

  void ConnectCredit(CreditSink* s) { credit_sink = s; }

  void Deliver(Flit const& f, Time now) override {
    recv_bytes += f.bytes;
    arriving.push_back(f.tag);
  }

  void Step() override {
    const Time now = RT::Now();
    Retire(now);
    while (Issue(now)) Retire(now);
    if (!HasProgram()) return;
    TraceIf("pc", pc, &last_pc);
    TraceIf("waiting", wait_since == kNever ? 0 : 1, &last_waiting);
  }

  bool HasProgram() const { return prog != nullptr && !prog->ops.empty(); }
  bool Done() const { return HasProgram() && pc >= prog->ops.size() && AllIdle(); }

  uint64_t Pc() const { return pc; }
  Time ComputeCycles() const { return compute_cycles; }
  Time WaitCycles() const { return wait_cycles; }
  Time WaitCyclesAt(Time now) const {
    return wait_since == kNever ? wait_cycles : wait_cycles + (now - wait_since);
  }
  uint64_t SentBytes() const { return sent_bytes; }
  uint64_t RecvBytes() const { return recv_total; }
  Time FinishedAt() const { return finished_at; }

 private:
  static constexpr Time kNever = ~Time{0};
  // 通道上没活时的取值。用一个不会跟真实时刻撞上的数。
  static constexpr Time kDone = ~Time{0} - 1;
  static constexpr uint32_t kChannels = 4;
  enum Chan : uint32_t { kMc = 0, kVc = 1, kDteOut = 2, kDteIn = 3 };

  void TraceIf(const char* name, uint64_t v, uint64_t* last) {
    if (v == *last) return;
    *last = v;
    Trace(name, v);
  }

  bool AllIdle() const {
    for (uint32_t i = 0; i < kChannels; ++i) {
      if (chan[i] != kDone) return false;
    }
    return true;
  }

  void Retire(Time now) {
    for (uint32_t i = 0; i < kChannels; ++i) {
      if (chan[i] != kDone && chan[i] <= now) chan[i] = kDone;
    }
    // 发射完最后一条不算做完，还要等各通道上的活都收尾。
    if (HasProgram() && pc >= prog->ops.size() && AllIdle() && finished_at == 0) {
      finished_at = now;
    }
  }

  bool Free(Chan c) const { return chan[c] == kDone; }

  // 这一块数据写好了没有。没人写过的当成一开始就在，权重就是这样。
  bool ReadyAt(uint64_t addr, Time now) const {
    auto it = ready.find(addr);
    return it == ready.end() || it->second <= now;
  }

  void Occupy(Chan c, Time now, Time len, Unit unit, SpanState st,
              uint64_t vol) {
    chan[c] = now + len;
    if (rec != nullptr) rec->Span(id, unit, pc, pc, now, chan[c], st, vol);
  }

  bool Issue(Time now) {
    if (prog == nullptr || pc >= prog->ops.size()) return false;
    KernelOp const& op = prog->ops[pc];
    switch (op.kind) {
      case OpKind::kGemm: {
        if (!Free(kMc) || !ReadyAt(op.a, now) || !ReadyAt(op.b, now)) return false;
        Occupy(kMc, now, op.cycles, Unit::kMatrix, SpanState::kCompute,
               op.m * op.n);
        compute_cycles += op.cycles;
        ready[op.c] = chan[kMc];
        ++pc;
        return true;
      }
      case OpKind::kElem: {
        if (!Free(kVc)) return false;
        for (uint64_t s : op.srcs) {
          if (!ReadyAt(s, now)) return false;
        }
        Occupy(kVc, now, op.cycles, Unit::kVector, SpanState::kCompute, op.n);
        compute_cycles += op.cycles;
        ready[op.dst] = chan[kVc];
        ++pc;
        return true;
      }
      case OpKind::kSend: {
        if (!Free(kDteOut) || !ReadyAt(op.src, now)) return false;
        Flit f;
        f.src = id;
        f.dst = op.dst_core < 0 ? kHostTarget
                                : static_cast<uint64_t>(op.dst_core);
        f.bytes = op.length;
        // 带上这一层收进来的那个号，收回去的一端才认得出是哪一份。
        f.tag = carried.empty() ? pc : carried.front();
        if (!carried.empty()) carried.pop_front();
        rt->Inject(f, now);
        sent_bytes += op.length;
        LOGCHECK(par.link_bw > 0, "Exec: link bandwidth must be positive.");
        Occupy(kDteOut, now,
               par.dte_setup + (op.length + par.link_bw - 1) / par.link_bw,
               Unit::kDte, SpanState::kTransfer, op.length);
        if (credit_sink != nullptr) credit_sink->ReturnCredit(id);
        ++pc;
        return true;
      }
      case OpKind::kRecv: {
        if (!Free(kDteIn)) return false;
        if (recv_bytes < op.length) {
          if (wait_since == kNever) wait_since = now;
          return false;
        }
        recv_bytes -= op.length;
        recv_total += op.length;
        if (!arriving.empty()) {
          carried.push_back(arriving.front());
          arriving.pop_front();
        }
        if (wait_since != kNever) {
          wait_cycles += now - wait_since;
          if (rec != nullptr) {
            rec->Wait(id, Unit::kDte, pc, pc, wait_since, now,
                      WaitReason::kIncomingBarrier);
          }
          wait_since = kNever;
        }
        ready[op.dst] = now;
        ++pc;
        return true;
      }
    }
    return false;
  }

  uint64_t id;
  CoreKernel const* prog;
  Router* rt;
  CoreParams par;
  SpanRecorder* rec;
  CreditSink* credit_sink = nullptr;

  Time chan[kChannels] = {kDone, kDone, kDone, kDone};
  std::unordered_map<uint64_t, Time> ready;
  uint64_t pc = 0;

  std::deque<uint64_t> arriving;
  std::deque<uint64_t> carried;
  uint64_t recv_bytes = 0;
  uint64_t recv_total = 0;
  uint64_t sent_bytes = 0;
  Time compute_cycles = 0;
  Time wait_cycles = 0;
  Time wait_since = kNever;
  Time finished_at = 0;
  uint64_t last_pc = ~uint64_t{0};
  uint64_t last_waiting = ~uint64_t{0};
};

}
}
}

#endif
