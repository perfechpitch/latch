#ifndef _LATCH_INST_INSTANCE_
#define _LATCH_INST_INSTANCE_

#include <memory>

#include "isa/instruction.h"
#include "isa/system.h"

namespace latch {

// 单条指令的执行上下文（纯功能、同步执行）：Run() 直接调用 Instruction::RunOnInstance 把指令体一次跑完；
// 指令体在体内直接读写寄存器 / 内存，并通过 LocalPc / MarkBranched / MarkHalts 报告控制流。
class Instance {
 public:
  Instance(std::shared_ptr<Instruction> instruction, System* sys)
      : inst(instruction), system(sys), done(false) {}

  virtual ~Instance() = default;

  // 跑指令体；只跑一次，之后调用为空操作。
  void Run() {
    if (done) return;
    inst->RunOnInstance(this);
    done = true;
  }
  bool IsDone() const { return done; }

  System* GetSystem() const { return system; }
  std::shared_ptr<Instruction> GetInst() const { return inst; }

  uint64_t& LocalPc() { return localPc; }
  void      SetLocalPc(uint64_t pc) { localPc = pc; }
  void      MarkBranched() { branched = true; }
  bool      IsBranched() const { return branched; }
  void      MarkHalts() { halts = true; }
  bool      Halts() const { return halts; }

 protected:
  std::shared_ptr<Instruction> inst;
  System* system;
  bool done;

  uint64_t localPc = 0;
  bool     branched = false;
  bool     halts = false;
};

// System 的执行骨架在此 out-of-line 定义：system.h 仅前置声明 Instance（instance.h 反向
// include 了 system.h），故这个解引用 Instance 的函数无法在 system.h 内联。inline 防多 TU 重定义。
inline std::unique_ptr<Instance> System::MakeInstance(std::shared_ptr<Instruction> inst) {
  return std::make_unique<Instance>(inst, this);
}

}  // namespace latch

#endif

#ifdef START_INST_CODE_DEFINE
#define PC                       instance->LocalPc()
#define BRANCH                   instance->MarkBranched()
#define HALT()                   instance->MarkHalts();
#endif
