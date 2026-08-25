#include "rv32.h"

#include "isa/decoder.h"

namespace latch::rv32 {

#include "gen/insts_hex_parser.inc"

std::shared_ptr<Instruction> SystemRv32::Decode(
    std::shared_ptr<InstBinary> instPkg) const {
  return DecodeByBitTable(instPkg, table, decode_jump_table, INST_MAX_WIDTH);
}

std::unique_ptr<Instance> SystemRv32::MakeInstance(std::shared_ptr<Instruction> inst) {
  return std::make_unique<InstanceRv32>(inst, this);
}

void SystemRv32::PrepareInstance(Instance& instance, uint32_t thread) {
  auto& inst32 = static_cast<InstanceRv32&>(instance);
  Register& srFile = GetRegFile(REG_SR, thread);
  inst32.SnapshotSr([&](uint32_t i) { return srFile[i].U32(); });
}

void SystemRv32::RetireInstance(Instance& instance, uint32_t thread) {
  auto& inst32 = static_cast<InstanceRv32&>(instance);
  Register& srFile = GetRegFile(REG_SR, thread);
  inst32.CommitSr([&](uint32_t i, uint32_t v) { srFile[i] = v; });
}

#include "gen/insts_factory.inc"

std::unique_ptr<Program> SystemRv32::NewProgram() { return std::make_unique<Rv32Program>(); }

}  // namespace latch::rv32
