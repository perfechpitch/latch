#ifndef _LATCH_ISA_RV32_
#define _LATCH_ISA_RV32_

#include <array>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <variant>
#include <vector>

#include "isa/instance.h"
#include "isa/instruction.h"
#include "isa/isa.h"
#include "isa/register.h"
#include "isa/system.h"
#include "isa/utils.h"

namespace latch::rv32 {
using namespace latch;

// ======== ISA：Rv32 / Rv32::Inst / 各枚举 ========

#include "gen/insts_def.inc"

#define INST_MAX_WIDTH 4

#include "gen/reg_enums.inc"

#include "gen/mem_enums.inc"

// kSystemThreadNum —— 默认核数，由 codegen 从 rv32.jsonc config 的 `thread` 生成（见 SystemRv32 ctor 默认值）。
#include "gen/config.inc"

#define Rv32Instruction Rv32::Inst

enum CSR_REG_STS { CSR_STS_NONE = 0x0, CSR_STS_RUN = 0x1, CSR_STS_HALT = 0x2 };

class Rv32 : public ISA {
 public:
  Rv32(){};
  ~Rv32() {}

#include "gen/insts_api.inc"

  class Inst : public Instruction {
   public:
    Inst(std::string name, uint64_t width) : Instruction(name, width) {}
    ~Inst() {}
  };

#include "gen/insts_api.h"

  uint64_t GetInstMaxByteWidth() override { return INST_MAX_WIDTH; }

 private:
};

// ======== System：SystemRv32 / Rv32Program ========

class SystemRv32 : public System {
 public:
  SystemRv32(std::shared_ptr<ISA> isa, uint64_t pc = 0x80000000, uint32_t thread = kSystemThreadNum) : System(isa, pc, thread) {
#include "gen/regs.inc"
    InitRegister();
#include "gen/mem.inc"
    InitMemory();
    ResetRegister();
    for (int i = 0; i < thread; i++) SetPC(initPc, i);
  }
  ~SystemRv32() {}

  void Reset() override {
    registerSystem->Reset();
    memorySystem->Clear();
    ResetInflight();
    ResetRegister();
  }

  void ResetRegister() {
    for (int i = 0; i < maxThread; i++) {
      GetRegFile(REG_SR, i).Reset();
      GetRegFile(REG_SPR, i).Reset();
      GetRegFile(REG_CSR, i).Reset();
      SetPC(-1u, i);
    }
  }

  uint64_t GetPC(uint32_t thread) override { return GetRegFile(REG_SPR, thread)[SPR_PC].U64(); }
  void SetPC(uint64_t pc = -1u, uint32_t thread = 0) override {
    uint32_t v = (pc == -1u) ? static_cast<uint32_t>(initPc) : static_cast<uint32_t>(pc);
    GetRegFile(REG_SPR, thread)[SPR_PC] = v;
  }

  bool GetHaltState(uint32_t thread) override { return CSR(thread)[CSR_STS_NONE].U32() == CSR_STS_HALT; }
  void SetHaltState(bool halt, uint32_t thread) override {
    if (halt)
      CSR(thread)[CSR_STS_NONE] = (uint32_t)CSR_STS_HALT;
    else
      CSR(thread)[CSR_STS_NONE] = (uint32_t)CSR_STS_RUN;
  }

  virtual std::shared_ptr<Instruction> Decode(std::shared_ptr<InstBinary> instPkg) const override;

  // RV32 用 SR 影子寄存器：造 InstanceRv32，跑前把 SR 快照进 localSr、退休时按写掩码提交增量。
  // 单拍推进由 Engine 驱动（StepContext，engine.h），此处只填寄存器相关的三个钩子。
  std::unique_ptr<Instance> MakeInstance(std::shared_ptr<Instruction> inst) override;
  void PrepareInstance(Instance& instance, uint32_t thread) override;
  void RetireInstance(Instance& instance, uint32_t thread) override;

  std::unique_ptr<Program> NewProgram() override;

#include "gen/reg_accessors.inc"

  void PcIncrease(uint32_t thread, uint32_t inst_byte_width) {
    GetRegFile(REG_SPR, thread)[SPR_PC] = GetRegFile(REG_SPR, thread)[SPR_PC].U32() + inst_byte_width;
  }

  void Step(uint32_t thread) { registerSystem->GetRegFile(REG_SR, thread)[0].Retype(uint32) = uint32_t(0); }
};

class Rv32Program : public Program {
 public:
  std::shared_ptr<Instruction> MakeInst(const std::string& name, const std::vector<uint64_t>& args) override;
};

// ======== Instance：InstanceRv32（单条指令的执行上下文） ========

// 单条指令的执行上下文。SR 读写作用在本 Instance 的 localSr 而非共享 SystemRv32：驱动方 body 跑前
// SnapshotSr 填入、退休后 CommitSr 提交增量。
class InstanceRv32 : public Instance {
 public:
  InstanceRv32(std::shared_ptr<Instruction> instruction, System* sys)
      : Instance(instruction, sys),
        localSr{},
        srWriteMask(0) {
    auto* sysRv32 = dynamic_cast<SystemRv32*>(sys);
    if (sysRv32) SetLocalPc(static_cast<uint32_t>(sysRv32->GetPC(0)));
  }

  ~InstanceRv32() {}

  uint32_t LocalSr(uint32_t idx) const { return localSr[idx & 31u]; }
  void SetLocalSr(uint32_t idx, uint32_t v) {
    idx &= 31u;
    if (idx == 0) return;  // x0 恒零
    localSr[idx] = v;
    srWriteMask |= (1u << idx);
  }
  uint32_t SrWriteMask() const { return srWriteMask; }
  bool SrWritten(uint32_t idx) const { return (srWriteMask >> (idx & 31u)) & 1u; }

  template <typename Reader>
  void SnapshotSr(Reader&& src) {
    for (uint32_t i = 0; i < 32; ++i) localSr[i] = src(i);
    localSr[0] = 0;
    srWriteMask = 0;
  }

  // 只提交 body 实际写过的项（按 srWriteMask）；x0 永不提交。
  template <typename Writer>
  void CommitSr(Writer&& dst) const {
    uint32_t m = srWriteMask;
    while (m) {
      uint32_t i = __builtin_ctz(m);
      m &= m - 1;
      dst(i, localSr[i]);
    }
  }

 private:
  std::array<uint32_t, 32> localSr;
  uint32_t srWriteMask;
};

}  // namespace latch::rv32

#endif

#ifdef START_INST_CODE_DEFINE

#include "gen/reg_macros.inc"

// 指令体访问 SR 的两个原语（instance 在作用域）：读快照 localSr、写快照并记入写掩码，退休时 CommitSr 提交。
// 内存访问直接用 system->LoadMem<T>(addr) / system->StoreMem<T>(addr, v)（isa/system.h）。
#define ReadSr(index)         instance->LocalSr(static_cast<uint32_t>(index))
#define WriteSr(index, value) instance->SetLocalSr(static_cast<uint32_t>(index), static_cast<uint32_t>(value))

#endif
