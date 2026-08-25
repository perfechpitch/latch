#ifndef _LATCH_ISA_ENGINE_
#define _LATCH_ISA_ENGINE_

#include <cstdint>
#include <memory>
#include <vector>

#include "isa/instance.h"   // StepContext 解引用 Instance，需其完整定义
#include "isa/system.h"

namespace latch {

// Engine：功能执行驱动者。**非 ClkModule、不挂 Clock、不用协程**——cycle 推进就是普通循环，每拍每个上下文
// 同步跑完一条指令。纯功能、无时序。
//
// Engine 持有若干 (System, thread) **执行上下文**：多核同构 = 一个 System + N thread，各注册一个上下文；
// 多个 System 也可各注册。串行 step → 无竞争、确定。
//
// 只有两个跑指令入口：
//   - Cycle()    跑一拍：顺序 step 各上下文一拍。
//   - Continue() 跑到收工：循环 Cycle 到停机（带 maxCyc 安全上限）。
class Engine {
 public:
  // stopOnPrimary：true → 首个注册的上下文（primary / master）停机即收工（加速器模型，slave 被动从属、
  // 不会自己停）。false → 等所有上下文都停机。maxCyc：安全上限。
  explicit Engine(uint64_t maxCycles = 100000, bool stopWhenPrimaryHalts = false)
      : maxCyc(maxCycles), stopOnPrimary(stopWhenPrimaryHalts) {}

  // 注册一个执行上下文：在 System 的哪个 thread 上跑。
  void Add(System* sys, uint32_t thread = 0) {
    contexts.push_back(Context{sys, thread});
  }

  bool AllHalted() const {
    for (auto const& c : contexts)
      if (!c.sys->GetHaltState(c.thread)) return false;
    return true;
  }

  bool PrimaryHalted() const {
    return !contexts.empty() &&
           contexts[0].sys->GetHaltState(contexts[0].thread);
  }

  // 跑一拍：顺序 step 各上下文（串行 → 无竞争、确定）。
  void Cycle() {
    for (auto& c : contexts) StepContext(c);
    ++cyc;
  }

  // 跑到收工，返回总拍数。stopOnPrimary → primary 停机即收工；否则等所有上下文停机；都带 maxCyc 上限。
  uint64_t Continue() {
    while (cyc < maxCyc) {
      Cycle();
      if (stopOnPrimary ? PrimaryHalted() : AllHalted()) break;
    }
    return cyc;
  }

  uint64_t Cycles() const { return cyc; }
  uint64_t Retired() const {
    uint64_t n = 0;
    for (auto const& c : contexts) n += c.sys->Retired(c.thread);
    return n;
  }

 private:
  struct Context {
    System* sys;
    uint32_t thread;
  };

  // 单拍推进一个上下文：按 PC 取一条（FetchInst）→ 造 Instance、快照 → 同步跑完指令体 → 提交、按控制点定
  // PC、停机、退休计数（退休数住 System 的 inflight，经 friend 访问）。
  void StepContext(Context& c) {
    System* sys = c.sys;
    uint32_t thread = c.thread;
    if (sys->GetHaltState(thread)) return;
    uint64_t pc = sys->GetPC(thread);
    auto inst = sys->FetchInst(pc);
    auto cur = sys->MakeInstance(inst);
    cur->SetLocalPc(pc);                 // 本条初始 PC（按本 thread；AUIPC / 分支相对用）
    sys->PrepareInstance(*cur, thread);  // ISA 子类：寄存器快照
    cur->Run();
    sys->RetireInstance(*cur, thread);   // ISA 子类：寄存器提交
    if (cur->IsBranched()) {
      sys->SetPC(cur->LocalPc(), thread);
    } else {
      sys->SetPC(sys->GetPC(thread) + sys->EffectiveWidth(cur->GetInst()), thread);
    }
    if (cur->Halts()) sys->SetHaltState(true, thread);
    ++sys->inflight[thread].retired;
  }

  std::vector<Context> contexts;
  uint64_t maxCyc;
  bool stopOnPrimary;
  uint64_t cyc = 0;
};

}  // namespace latch

#endif
