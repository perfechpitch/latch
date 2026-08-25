/**
**/
//
// 由 ISA DSL 工具生成。
//
// clang-format off
#pragma GCC diagnostic ignored "-Wunused-parameter"


#define START_INST_CODE_DEFINE


#include "rv32.h"

namespace latch::rv32 {
Rv32::ISA_RV32I_ADD::ISA_RV32I_ADD(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_ADD", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 0, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 0, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_ADD::ISA_RV32I_ADD(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_ADD", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_ADD::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "ADD";
}
void Rv32::ISA_RV32I_ADD::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_ADD run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_ADD.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_ADD run on system END!"));
}
Rv32::ISA_RV32I_SUB::ISA_RV32I_SUB(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_SUB", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 0, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 32, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_SUB::ISA_RV32I_SUB(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_SUB", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_SUB::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "SUB";
}
void Rv32::ISA_RV32I_SUB::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_SUB run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_SUB.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_SUB run on system END!"));
}
Rv32::ISA_RV32I_SLL::ISA_RV32I_SLL(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_SLL", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 1, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 0, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_SLL::ISA_RV32I_SLL(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_SLL", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_SLL::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "SLL";
}
void Rv32::ISA_RV32I_SLL::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_SLL run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_SLL.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_SLL run on system END!"));
}
Rv32::ISA_RV32I_SLT::ISA_RV32I_SLT(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_SLT", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 0, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_SLT::ISA_RV32I_SLT(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_SLT", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_SLT::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "SLT";
}
void Rv32::ISA_RV32I_SLT::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_SLT run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_SLT.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_SLT run on system END!"));
}
Rv32::ISA_RV32I_SLTU::ISA_RV32I_SLTU(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_SLTU", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 3, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 0, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_SLTU::ISA_RV32I_SLTU(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_SLTU", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_SLTU::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "SLTU";
}
void Rv32::ISA_RV32I_SLTU::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_SLTU run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_SLTU.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_SLTU run on system END!"));
}
Rv32::ISA_RV32I_XOR::ISA_RV32I_XOR(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_XOR", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 4, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 0, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_XOR::ISA_RV32I_XOR(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_XOR", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_XOR::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "XOR";
}
void Rv32::ISA_RV32I_XOR::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_XOR run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_XOR.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_XOR run on system END!"));
}
Rv32::ISA_RV32I_SRL::ISA_RV32I_SRL(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_SRL", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 5, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 0, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_SRL::ISA_RV32I_SRL(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_SRL", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_SRL::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "SRL";
}
void Rv32::ISA_RV32I_SRL::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_SRL run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_SRL.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_SRL run on system END!"));
}
Rv32::ISA_RV32I_SRA::ISA_RV32I_SRA(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_SRA", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 5, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 32, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_SRA::ISA_RV32I_SRA(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_SRA", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_SRA::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "SRA";
}
void Rv32::ISA_RV32I_SRA::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_SRA run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_SRA.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_SRA run on system END!"));
}
Rv32::ISA_RV32I_OR::ISA_RV32I_OR(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_OR", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 6, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 0, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_OR::ISA_RV32I_OR(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_OR", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_OR::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "OR";
}
void Rv32::ISA_RV32I_OR::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_OR run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_OR.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_OR run on system END!"));
}
Rv32::ISA_RV32I_AND::ISA_RV32I_AND(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_AND", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 7, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 0, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_AND::ISA_RV32I_AND(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_AND", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_AND::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "AND";
}
void Rv32::ISA_RV32I_AND::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_AND run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_AND.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_AND run on system END!"));
}
Rv32::ISA_RV32I_ADDI::ISA_RV32I_ADDI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ ):
  Rv32Instruction("ISA_RV32I_ADDI", 32), _OPCODE(7, 19, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 0, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM12(12, _IMM12_, "_IMM12")  {
  InstInit();
}
Rv32::ISA_RV32I_ADDI::ISA_RV32I_ADDI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_ADDI", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM12(12, binAddr.SliceBit(20, 12), "_IMM12")  {
  InstInit();
}
void Rv32::ISA_RV32I_ADDI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM12);
  attribute[Rv32KEYS::mnemonic] = "ADDI";
}
void Rv32::ISA_RV32I_ADDI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_ADDI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_ADDI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_ADDI run on system END!"));
}
Rv32::ISA_RV32I_SLTI::ISA_RV32I_SLTI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ ):
  Rv32Instruction("ISA_RV32I_SLTI", 32), _OPCODE(7, 19, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM12(12, _IMM12_, "_IMM12")  {
  InstInit();
}
Rv32::ISA_RV32I_SLTI::ISA_RV32I_SLTI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_SLTI", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM12(12, binAddr.SliceBit(20, 12), "_IMM12")  {
  InstInit();
}
void Rv32::ISA_RV32I_SLTI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM12);
  attribute[Rv32KEYS::mnemonic] = "SLTI";
}
void Rv32::ISA_RV32I_SLTI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_SLTI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_SLTI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_SLTI run on system END!"));
}
Rv32::ISA_RV32I_SLTIU::ISA_RV32I_SLTIU(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ ):
  Rv32Instruction("ISA_RV32I_SLTIU", 32), _OPCODE(7, 19, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 3, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM12(12, _IMM12_, "_IMM12")  {
  InstInit();
}
Rv32::ISA_RV32I_SLTIU::ISA_RV32I_SLTIU(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_SLTIU", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM12(12, binAddr.SliceBit(20, 12), "_IMM12")  {
  InstInit();
}
void Rv32::ISA_RV32I_SLTIU::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM12);
  attribute[Rv32KEYS::mnemonic] = "SLTIU";
}
void Rv32::ISA_RV32I_SLTIU::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_SLTIU run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_SLTIU.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_SLTIU run on system END!"));
}
Rv32::ISA_RV32I_XORI::ISA_RV32I_XORI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ ):
  Rv32Instruction("ISA_RV32I_XORI", 32), _OPCODE(7, 19, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 4, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM12(12, _IMM12_, "_IMM12")  {
  InstInit();
}
Rv32::ISA_RV32I_XORI::ISA_RV32I_XORI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_XORI", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM12(12, binAddr.SliceBit(20, 12), "_IMM12")  {
  InstInit();
}
void Rv32::ISA_RV32I_XORI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM12);
  attribute[Rv32KEYS::mnemonic] = "XORI";
}
void Rv32::ISA_RV32I_XORI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_XORI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_XORI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_XORI run on system END!"));
}
Rv32::ISA_RV32I_ORI::ISA_RV32I_ORI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ ):
  Rv32Instruction("ISA_RV32I_ORI", 32), _OPCODE(7, 19, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 6, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM12(12, _IMM12_, "_IMM12")  {
  InstInit();
}
Rv32::ISA_RV32I_ORI::ISA_RV32I_ORI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_ORI", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM12(12, binAddr.SliceBit(20, 12), "_IMM12")  {
  InstInit();
}
void Rv32::ISA_RV32I_ORI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM12);
  attribute[Rv32KEYS::mnemonic] = "ORI";
}
void Rv32::ISA_RV32I_ORI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_ORI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_ORI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_ORI run on system END!"));
}
Rv32::ISA_RV32I_ANDI::ISA_RV32I_ANDI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ ):
  Rv32Instruction("ISA_RV32I_ANDI", 32), _OPCODE(7, 19, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 7, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM12(12, _IMM12_, "_IMM12")  {
  InstInit();
}
Rv32::ISA_RV32I_ANDI::ISA_RV32I_ANDI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_ANDI", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM12(12, binAddr.SliceBit(20, 12), "_IMM12")  {
  InstInit();
}
void Rv32::ISA_RV32I_ANDI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM12);
  attribute[Rv32KEYS::mnemonic] = "ANDI";
}
void Rv32::ISA_RV32I_ANDI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_ANDI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_ANDI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_ANDI run on system END!"));
}
Rv32::ISA_RV32I_SLLI::ISA_RV32I_SLLI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM5_ ):
  Rv32Instruction("ISA_RV32I_SLLI", 32), _OPCODE(7, 19, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 1, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM5(5, _IMM5_, "_IMM5"), _CONST0(7, 0, "_CONST0")  {
  InstInit();
}
Rv32::ISA_RV32I_SLLI::ISA_RV32I_SLLI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_SLLI", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM5(5, binAddr.SliceBit(20, 5), "_IMM5"), _CONST0(7, binAddr.SliceBit(25, 7), "_CONST0")  {
  InstInit();
}
void Rv32::ISA_RV32I_SLLI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM5);
  AppendField(_CONST0);
  attribute[Rv32KEYS::mnemonic] = "SLLI";
}
void Rv32::ISA_RV32I_SLLI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_SLLI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_SLLI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_SLLI run on system END!"));
}
Rv32::ISA_RV32I_SRLI::ISA_RV32I_SRLI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM5_ ):
  Rv32Instruction("ISA_RV32I_SRLI", 32), _OPCODE(7, 19, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 5, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM5(5, _IMM5_, "_IMM5"), _CONST0(7, 0, "_CONST0")  {
  InstInit();
}
Rv32::ISA_RV32I_SRLI::ISA_RV32I_SRLI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_SRLI", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM5(5, binAddr.SliceBit(20, 5), "_IMM5"), _CONST0(7, binAddr.SliceBit(25, 7), "_CONST0")  {
  InstInit();
}
void Rv32::ISA_RV32I_SRLI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM5);
  AppendField(_CONST0);
  attribute[Rv32KEYS::mnemonic] = "SRLI";
}
void Rv32::ISA_RV32I_SRLI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_SRLI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_SRLI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_SRLI run on system END!"));
}
Rv32::ISA_RV32I_SRAI::ISA_RV32I_SRAI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM5_ ):
  Rv32Instruction("ISA_RV32I_SRAI", 32), _OPCODE(7, 19, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 5, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM5(5, _IMM5_, "_IMM5"), _CONST32(7, 32, "_CONST32")  {
  InstInit();
}
Rv32::ISA_RV32I_SRAI::ISA_RV32I_SRAI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_SRAI", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM5(5, binAddr.SliceBit(20, 5), "_IMM5"), _CONST32(7, binAddr.SliceBit(25, 7), "_CONST32")  {
  InstInit();
}
void Rv32::ISA_RV32I_SRAI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM5);
  AppendField(_CONST32);
  attribute[Rv32KEYS::mnemonic] = "SRAI";
}
void Rv32::ISA_RV32I_SRAI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_SRAI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_SRAI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_SRAI run on system END!"));
}
Rv32::ISA_RV32I_LB::ISA_RV32I_LB(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ ):
  Rv32Instruction("ISA_RV32I_LB", 32), _OPCODE(7, 3, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 0, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM12(12, _IMM12_, "_IMM12")  {
  InstInit();
}
Rv32::ISA_RV32I_LB::ISA_RV32I_LB(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_LB", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM12(12, binAddr.SliceBit(20, 12), "_IMM12")  {
  InstInit();
}
void Rv32::ISA_RV32I_LB::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM12);
  attribute[Rv32KEYS::mnemonic] = "LB";
}
void Rv32::ISA_RV32I_LB::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_LB run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_LB.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_LB run on system END!"));
}
Rv32::ISA_RV32I_LH::ISA_RV32I_LH(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ ):
  Rv32Instruction("ISA_RV32I_LH", 32), _OPCODE(7, 3, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 1, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM12(12, _IMM12_, "_IMM12")  {
  InstInit();
}
Rv32::ISA_RV32I_LH::ISA_RV32I_LH(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_LH", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM12(12, binAddr.SliceBit(20, 12), "_IMM12")  {
  InstInit();
}
void Rv32::ISA_RV32I_LH::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM12);
  attribute[Rv32KEYS::mnemonic] = "LH";
}
void Rv32::ISA_RV32I_LH::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_LH run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_LH.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_LH run on system END!"));
}
Rv32::ISA_RV32I_LW::ISA_RV32I_LW(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ ):
  Rv32Instruction("ISA_RV32I_LW", 32), _OPCODE(7, 3, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM12(12, _IMM12_, "_IMM12")  {
  InstInit();
}
Rv32::ISA_RV32I_LW::ISA_RV32I_LW(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_LW", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM12(12, binAddr.SliceBit(20, 12), "_IMM12")  {
  InstInit();
}
void Rv32::ISA_RV32I_LW::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM12);
  attribute[Rv32KEYS::mnemonic] = "LW";
}
void Rv32::ISA_RV32I_LW::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_LW run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_LW.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_LW run on system END!"));
}
Rv32::ISA_RV32I_LBU::ISA_RV32I_LBU(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ ):
  Rv32Instruction("ISA_RV32I_LBU", 32), _OPCODE(7, 3, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 4, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM12(12, _IMM12_, "_IMM12")  {
  InstInit();
}
Rv32::ISA_RV32I_LBU::ISA_RV32I_LBU(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_LBU", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM12(12, binAddr.SliceBit(20, 12), "_IMM12")  {
  InstInit();
}
void Rv32::ISA_RV32I_LBU::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM12);
  attribute[Rv32KEYS::mnemonic] = "LBU";
}
void Rv32::ISA_RV32I_LBU::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_LBU run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_LBU.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_LBU run on system END!"));
}
Rv32::ISA_RV32I_LHU::ISA_RV32I_LHU(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ ):
  Rv32Instruction("ISA_RV32I_LHU", 32), _OPCODE(7, 3, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 5, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM12(12, _IMM12_, "_IMM12")  {
  InstInit();
}
Rv32::ISA_RV32I_LHU::ISA_RV32I_LHU(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_LHU", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM12(12, binAddr.SliceBit(20, 12), "_IMM12")  {
  InstInit();
}
void Rv32::ISA_RV32I_LHU::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM12);
  attribute[Rv32KEYS::mnemonic] = "LHU";
}
void Rv32::ISA_RV32I_LHU::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_LHU run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_LHU.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_LHU run on system END!"));
}
Rv32::ISA_RV32I_SB::ISA_RV32I_SB(uint64_t _IMM5_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM7_ ):
  Rv32Instruction("ISA_RV32I_SB", 32), _OPCODE(7, 35, "_OPCODE"), _IMM5(5, _IMM5_, "_IMM5"), _FUNCT3(3, 0, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _IMM7(7, _IMM7_, "_IMM7")  {
  InstInit();
}
Rv32::ISA_RV32I_SB::ISA_RV32I_SB(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_SB", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _IMM5(5, binAddr.SliceBit(7, 5), "_IMM5"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _IMM7(7, binAddr.SliceBit(25, 7), "_IMM7")  {
  InstInit();
}
void Rv32::ISA_RV32I_SB::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM5);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_IMM7);
  attribute[Rv32KEYS::mnemonic] = "SB";
}
void Rv32::ISA_RV32I_SB::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_SB run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_SB.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_SB run on system END!"));
}
Rv32::ISA_RV32I_SH::ISA_RV32I_SH(uint64_t _IMM5_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM7_ ):
  Rv32Instruction("ISA_RV32I_SH", 32), _OPCODE(7, 35, "_OPCODE"), _IMM5(5, _IMM5_, "_IMM5"), _FUNCT3(3, 1, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _IMM7(7, _IMM7_, "_IMM7")  {
  InstInit();
}
Rv32::ISA_RV32I_SH::ISA_RV32I_SH(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_SH", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _IMM5(5, binAddr.SliceBit(7, 5), "_IMM5"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _IMM7(7, binAddr.SliceBit(25, 7), "_IMM7")  {
  InstInit();
}
void Rv32::ISA_RV32I_SH::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM5);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_IMM7);
  attribute[Rv32KEYS::mnemonic] = "SH";
}
void Rv32::ISA_RV32I_SH::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_SH run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_SH.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_SH run on system END!"));
}
Rv32::ISA_RV32I_SW::ISA_RV32I_SW(uint64_t _IMM5_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM7_ ):
  Rv32Instruction("ISA_RV32I_SW", 32), _OPCODE(7, 35, "_OPCODE"), _IMM5(5, _IMM5_, "_IMM5"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _IMM7(7, _IMM7_, "_IMM7")  {
  InstInit();
}
Rv32::ISA_RV32I_SW::ISA_RV32I_SW(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_SW", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _IMM5(5, binAddr.SliceBit(7, 5), "_IMM5"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _IMM7(7, binAddr.SliceBit(25, 7), "_IMM7")  {
  InstInit();
}
void Rv32::ISA_RV32I_SW::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM5);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_IMM7);
  attribute[Rv32KEYS::mnemonic] = "SW";
}
void Rv32::ISA_RV32I_SW::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_SW run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_SW.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_SW run on system END!"));
}
Rv32::ISA_RV32I_BEQ::ISA_RV32I_BEQ(uint64_t _IMM1_, uint64_t _IMM4_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM6_, uint64_t _IMM1_U0_ ):
  Rv32Instruction("ISA_RV32I_BEQ", 32), _OPCODE(7, 99, "_OPCODE"), _IMM1(1, _IMM1_, "_IMM1"), _IMM4(4, _IMM4_, "_IMM4"), _FUNCT3(3, 0, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _IMM6(6, _IMM6_, "_IMM6"), _IMM1_U0(1, _IMM1_U0_, "_IMM1_U0")  {
  InstInit();
}
Rv32::ISA_RV32I_BEQ::ISA_RV32I_BEQ(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_BEQ", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _IMM1(1, binAddr.SliceBit(7, 1), "_IMM1"), _IMM4(4, binAddr.SliceBit(8, 4), "_IMM4"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _IMM6(6, binAddr.SliceBit(25, 6), "_IMM6"), _IMM1_U0(1, binAddr.SliceBit(31, 1), "_IMM1_U0")  {
  InstInit();
}
void Rv32::ISA_RV32I_BEQ::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM1);
  AppendField(_IMM4);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_IMM6);
  AppendField(_IMM1_U0);
  attribute[Rv32KEYS::mnemonic] = "BEQ";
}
void Rv32::ISA_RV32I_BEQ::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_BEQ run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_BEQ.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_BEQ run on system END!"));
}
Rv32::ISA_RV32I_BNE::ISA_RV32I_BNE(uint64_t _IMM1_, uint64_t _IMM4_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM6_, uint64_t _IMM1_U0_ ):
  Rv32Instruction("ISA_RV32I_BNE", 32), _OPCODE(7, 99, "_OPCODE"), _IMM1(1, _IMM1_, "_IMM1"), _IMM4(4, _IMM4_, "_IMM4"), _FUNCT3(3, 1, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _IMM6(6, _IMM6_, "_IMM6"), _IMM1_U0(1, _IMM1_U0_, "_IMM1_U0")  {
  InstInit();
}
Rv32::ISA_RV32I_BNE::ISA_RV32I_BNE(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_BNE", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _IMM1(1, binAddr.SliceBit(7, 1), "_IMM1"), _IMM4(4, binAddr.SliceBit(8, 4), "_IMM4"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _IMM6(6, binAddr.SliceBit(25, 6), "_IMM6"), _IMM1_U0(1, binAddr.SliceBit(31, 1), "_IMM1_U0")  {
  InstInit();
}
void Rv32::ISA_RV32I_BNE::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM1);
  AppendField(_IMM4);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_IMM6);
  AppendField(_IMM1_U0);
  attribute[Rv32KEYS::mnemonic] = "BNE";
}
void Rv32::ISA_RV32I_BNE::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_BNE run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_BNE.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_BNE run on system END!"));
}
Rv32::ISA_RV32I_BLT::ISA_RV32I_BLT(uint64_t _IMM1_, uint64_t _IMM4_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM6_, uint64_t _IMM1_U0_ ):
  Rv32Instruction("ISA_RV32I_BLT", 32), _OPCODE(7, 99, "_OPCODE"), _IMM1(1, _IMM1_, "_IMM1"), _IMM4(4, _IMM4_, "_IMM4"), _FUNCT3(3, 4, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _IMM6(6, _IMM6_, "_IMM6"), _IMM1_U0(1, _IMM1_U0_, "_IMM1_U0")  {
  InstInit();
}
Rv32::ISA_RV32I_BLT::ISA_RV32I_BLT(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_BLT", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _IMM1(1, binAddr.SliceBit(7, 1), "_IMM1"), _IMM4(4, binAddr.SliceBit(8, 4), "_IMM4"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _IMM6(6, binAddr.SliceBit(25, 6), "_IMM6"), _IMM1_U0(1, binAddr.SliceBit(31, 1), "_IMM1_U0")  {
  InstInit();
}
void Rv32::ISA_RV32I_BLT::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM1);
  AppendField(_IMM4);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_IMM6);
  AppendField(_IMM1_U0);
  attribute[Rv32KEYS::mnemonic] = "BLT";
}
void Rv32::ISA_RV32I_BLT::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_BLT run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_BLT.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_BLT run on system END!"));
}
Rv32::ISA_RV32I_BGE::ISA_RV32I_BGE(uint64_t _IMM1_, uint64_t _IMM4_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM6_, uint64_t _IMM1_U0_ ):
  Rv32Instruction("ISA_RV32I_BGE", 32), _OPCODE(7, 99, "_OPCODE"), _IMM1(1, _IMM1_, "_IMM1"), _IMM4(4, _IMM4_, "_IMM4"), _FUNCT3(3, 5, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _IMM6(6, _IMM6_, "_IMM6"), _IMM1_U0(1, _IMM1_U0_, "_IMM1_U0")  {
  InstInit();
}
Rv32::ISA_RV32I_BGE::ISA_RV32I_BGE(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_BGE", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _IMM1(1, binAddr.SliceBit(7, 1), "_IMM1"), _IMM4(4, binAddr.SliceBit(8, 4), "_IMM4"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _IMM6(6, binAddr.SliceBit(25, 6), "_IMM6"), _IMM1_U0(1, binAddr.SliceBit(31, 1), "_IMM1_U0")  {
  InstInit();
}
void Rv32::ISA_RV32I_BGE::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM1);
  AppendField(_IMM4);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_IMM6);
  AppendField(_IMM1_U0);
  attribute[Rv32KEYS::mnemonic] = "BGE";
}
void Rv32::ISA_RV32I_BGE::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_BGE run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_BGE.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_BGE run on system END!"));
}
Rv32::ISA_RV32I_BLTU::ISA_RV32I_BLTU(uint64_t _IMM1_, uint64_t _IMM4_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM6_, uint64_t _IMM1_U0_ ):
  Rv32Instruction("ISA_RV32I_BLTU", 32), _OPCODE(7, 99, "_OPCODE"), _IMM1(1, _IMM1_, "_IMM1"), _IMM4(4, _IMM4_, "_IMM4"), _FUNCT3(3, 6, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _IMM6(6, _IMM6_, "_IMM6"), _IMM1_U0(1, _IMM1_U0_, "_IMM1_U0")  {
  InstInit();
}
Rv32::ISA_RV32I_BLTU::ISA_RV32I_BLTU(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_BLTU", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _IMM1(1, binAddr.SliceBit(7, 1), "_IMM1"), _IMM4(4, binAddr.SliceBit(8, 4), "_IMM4"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _IMM6(6, binAddr.SliceBit(25, 6), "_IMM6"), _IMM1_U0(1, binAddr.SliceBit(31, 1), "_IMM1_U0")  {
  InstInit();
}
void Rv32::ISA_RV32I_BLTU::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM1);
  AppendField(_IMM4);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_IMM6);
  AppendField(_IMM1_U0);
  attribute[Rv32KEYS::mnemonic] = "BLTU";
}
void Rv32::ISA_RV32I_BLTU::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_BLTU run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_BLTU.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_BLTU run on system END!"));
}
Rv32::ISA_RV32I_BGEU::ISA_RV32I_BGEU(uint64_t _IMM1_, uint64_t _IMM4_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM6_, uint64_t _IMM1_U0_ ):
  Rv32Instruction("ISA_RV32I_BGEU", 32), _OPCODE(7, 99, "_OPCODE"), _IMM1(1, _IMM1_, "_IMM1"), _IMM4(4, _IMM4_, "_IMM4"), _FUNCT3(3, 7, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _IMM6(6, _IMM6_, "_IMM6"), _IMM1_U0(1, _IMM1_U0_, "_IMM1_U0")  {
  InstInit();
}
Rv32::ISA_RV32I_BGEU::ISA_RV32I_BGEU(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_BGEU", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _IMM1(1, binAddr.SliceBit(7, 1), "_IMM1"), _IMM4(4, binAddr.SliceBit(8, 4), "_IMM4"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _IMM6(6, binAddr.SliceBit(25, 6), "_IMM6"), _IMM1_U0(1, binAddr.SliceBit(31, 1), "_IMM1_U0")  {
  InstInit();
}
void Rv32::ISA_RV32I_BGEU::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM1);
  AppendField(_IMM4);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_IMM6);
  AppendField(_IMM1_U0);
  attribute[Rv32KEYS::mnemonic] = "BGEU";
}
void Rv32::ISA_RV32I_BGEU::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_BGEU run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_BGEU.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_BGEU run on system END!"));
}
Rv32::ISA_RV32I_LUI::ISA_RV32I_LUI(uint64_t _RD_, uint64_t _IMM20_ ):
  Rv32Instruction("ISA_RV32I_LUI", 32), _OPCODE(7, 55, "_OPCODE"), _RD(5, _RD_, "_RD"), _IMM20(20, _IMM20_, "_IMM20")  {
  InstInit();
}
Rv32::ISA_RV32I_LUI::ISA_RV32I_LUI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_LUI", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _IMM20(20, binAddr.SliceBit(12, 20), "_IMM20")  {
  InstInit();
}
void Rv32::ISA_RV32I_LUI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_IMM20);
  attribute[Rv32KEYS::mnemonic] = "LUI";
}
void Rv32::ISA_RV32I_LUI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_LUI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_LUI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_LUI run on system END!"));
}
Rv32::ISA_RV32I_AUIPC::ISA_RV32I_AUIPC(uint64_t _RD_, uint64_t _IMM20_ ):
  Rv32Instruction("ISA_RV32I_AUIPC", 32), _OPCODE(7, 23, "_OPCODE"), _RD(5, _RD_, "_RD"), _IMM20(20, _IMM20_, "_IMM20")  {
  InstInit();
}
Rv32::ISA_RV32I_AUIPC::ISA_RV32I_AUIPC(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_AUIPC", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _IMM20(20, binAddr.SliceBit(12, 20), "_IMM20")  {
  InstInit();
}
void Rv32::ISA_RV32I_AUIPC::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_IMM20);
  attribute[Rv32KEYS::mnemonic] = "AUIPC";
}
void Rv32::ISA_RV32I_AUIPC::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_AUIPC run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_AUIPC.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_AUIPC run on system END!"));
}
Rv32::ISA_RV32I_JAL::ISA_RV32I_JAL(uint64_t _RD_, uint64_t _IMM8_, uint64_t _IMM1_, uint64_t _IMM10_, uint64_t _IMM1_U0_ ):
  Rv32Instruction("ISA_RV32I_JAL", 32), _OPCODE(7, 111, "_OPCODE"), _RD(5, _RD_, "_RD"), _IMM8(8, _IMM8_, "_IMM8"), _IMM1(1, _IMM1_, "_IMM1"), _IMM10(10, _IMM10_, "_IMM10"), _IMM1_U0(1, _IMM1_U0_, "_IMM1_U0")  {
  InstInit();
}
Rv32::ISA_RV32I_JAL::ISA_RV32I_JAL(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_JAL", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _IMM8(8, binAddr.SliceBit(12, 8), "_IMM8"), _IMM1(1, binAddr.SliceBit(20, 1), "_IMM1"), _IMM10(10, binAddr.SliceBit(21, 10), "_IMM10"), _IMM1_U0(1, binAddr.SliceBit(31, 1), "_IMM1_U0")  {
  InstInit();
}
void Rv32::ISA_RV32I_JAL::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_IMM8);
  AppendField(_IMM1);
  AppendField(_IMM10);
  AppendField(_IMM1_U0);
  attribute[Rv32KEYS::mnemonic] = "JAL";
}
void Rv32::ISA_RV32I_JAL::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_JAL run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_JAL.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_JAL run on system END!"));
}
Rv32::ISA_RV32I_JALR::ISA_RV32I_JALR(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ ):
  Rv32Instruction("ISA_RV32I_JALR", 32), _OPCODE(7, 103, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 0, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM12(12, _IMM12_, "_IMM12")  {
  InstInit();
}
Rv32::ISA_RV32I_JALR::ISA_RV32I_JALR(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_JALR", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM12(12, binAddr.SliceBit(20, 12), "_IMM12")  {
  InstInit();
}
void Rv32::ISA_RV32I_JALR::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM12);
  attribute[Rv32KEYS::mnemonic] = "JALR";
}
void Rv32::ISA_RV32I_JALR::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_JALR run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_JALR.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_JALR run on system END!"));
}
Rv32::ISA_RV32I_FENCE::ISA_RV32I_FENCE(uint64_t _RD_, uint64_t _RS1_, uint64_t _SUCC_, uint64_t _PRED_ ):
  Rv32Instruction("ISA_RV32I_FENCE", 32), _OPCODE(7, 15, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 0, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _SUCC(4, _SUCC_, "_SUCC"), _PRED(4, _PRED_, "_PRED"), _CONST0(4, 0, "_CONST0")  {
  InstInit();
}
Rv32::ISA_RV32I_FENCE::ISA_RV32I_FENCE(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_FENCE", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _SUCC(4, binAddr.SliceBit(20, 4), "_SUCC"), _PRED(4, binAddr.SliceBit(24, 4), "_PRED"), _CONST0(4, binAddr.SliceBit(28, 4), "_CONST0")  {
  InstInit();
}
void Rv32::ISA_RV32I_FENCE::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_SUCC);
  AppendField(_PRED);
  AppendField(_CONST0);
  attribute[Rv32KEYS::mnemonic] = "FENCE";
}
void Rv32::ISA_RV32I_FENCE::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_FENCE run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_FENCE.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_FENCE run on system END!"));
}
Rv32::ISA_RV32I_FENCE_I::ISA_RV32I_FENCE_I(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ ):
  Rv32Instruction("ISA_RV32I_FENCE_I", 32), _OPCODE(7, 15, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 1, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM12(12, _IMM12_, "_IMM12")  {
  InstInit();
}
Rv32::ISA_RV32I_FENCE_I::ISA_RV32I_FENCE_I(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_FENCE_I", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM12(12, binAddr.SliceBit(20, 12), "_IMM12")  {
  InstInit();
}
void Rv32::ISA_RV32I_FENCE_I::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM12);
  attribute[Rv32KEYS::mnemonic] = "FENCE_I";
}
void Rv32::ISA_RV32I_FENCE_I::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_FENCE_I run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_FENCE_I.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_FENCE_I run on system END!"));
}
Rv32::ISA_RV32I_ECALL::ISA_RV32I_ECALL(uint64_t _RD_, uint64_t _RS1_ ):
  Rv32Instruction("ISA_RV32I_ECALL", 32), _OPCODE(7, 115, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 0, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _FUNCT12(12, 0, "_FUNCT12")  {
  InstInit();
}
Rv32::ISA_RV32I_ECALL::ISA_RV32I_ECALL(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_ECALL", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _FUNCT12(12, binAddr.SliceBit(20, 12), "_FUNCT12")  {
  InstInit();
}
void Rv32::ISA_RV32I_ECALL::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_FUNCT12);
  attribute[Rv32KEYS::mnemonic] = "ECALL";
}
void Rv32::ISA_RV32I_ECALL::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_ECALL run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_ECALL.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_ECALL run on system END!"));
}
Rv32::ISA_RV32I_EBREAK::ISA_RV32I_EBREAK(uint64_t _RD_, uint64_t _RS1_ ):
  Rv32Instruction("ISA_RV32I_EBREAK", 32), _OPCODE(7, 115, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 0, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _FUNCT12(12, 1, "_FUNCT12")  {
  InstInit();
}
Rv32::ISA_RV32I_EBREAK::ISA_RV32I_EBREAK(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_EBREAK", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _FUNCT12(12, binAddr.SliceBit(20, 12), "_FUNCT12")  {
  InstInit();
}
void Rv32::ISA_RV32I_EBREAK::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_FUNCT12);
  attribute[Rv32KEYS::mnemonic] = "EBREAK";
}
void Rv32::ISA_RV32I_EBREAK::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_EBREAK run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_EBREAK.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_EBREAK run on system END!"));
}
Rv32::ISA_RV32I_CSRRW::ISA_RV32I_CSRRW(uint64_t _RD_, uint64_t _RS1_, uint64_t _CSR_ ):
  Rv32Instruction("ISA_RV32I_CSRRW", 32), _OPCODE(7, 115, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 1, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _CSR(12, _CSR_, "_CSR")  {
  InstInit();
}
Rv32::ISA_RV32I_CSRRW::ISA_RV32I_CSRRW(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_CSRRW", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _CSR(12, binAddr.SliceBit(20, 12), "_CSR")  {
  InstInit();
}
void Rv32::ISA_RV32I_CSRRW::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_CSR);
  attribute[Rv32KEYS::mnemonic] = "CSRRW";
}
void Rv32::ISA_RV32I_CSRRW::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_CSRRW run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_CSRRW.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_CSRRW run on system END!"));
}
Rv32::ISA_RV32I_CSRRS::ISA_RV32I_CSRRS(uint64_t _RD_, uint64_t _RS1_, uint64_t _CSR_ ):
  Rv32Instruction("ISA_RV32I_CSRRS", 32), _OPCODE(7, 115, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _CSR(12, _CSR_, "_CSR")  {
  InstInit();
}
Rv32::ISA_RV32I_CSRRS::ISA_RV32I_CSRRS(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_CSRRS", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _CSR(12, binAddr.SliceBit(20, 12), "_CSR")  {
  InstInit();
}
void Rv32::ISA_RV32I_CSRRS::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_CSR);
  attribute[Rv32KEYS::mnemonic] = "CSRRS";
}
void Rv32::ISA_RV32I_CSRRS::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_CSRRS run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_CSRRS.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_CSRRS run on system END!"));
}
Rv32::ISA_RV32I_CSRRC::ISA_RV32I_CSRRC(uint64_t _RD_, uint64_t _RS1_, uint64_t _CSR_ ):
  Rv32Instruction("ISA_RV32I_CSRRC", 32), _OPCODE(7, 115, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 3, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _CSR(12, _CSR_, "_CSR")  {
  InstInit();
}
Rv32::ISA_RV32I_CSRRC::ISA_RV32I_CSRRC(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_CSRRC", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _CSR(12, binAddr.SliceBit(20, 12), "_CSR")  {
  InstInit();
}
void Rv32::ISA_RV32I_CSRRC::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_CSR);
  attribute[Rv32KEYS::mnemonic] = "CSRRC";
}
void Rv32::ISA_RV32I_CSRRC::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_CSRRC run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_CSRRC.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_CSRRC run on system END!"));
}
Rv32::ISA_RV32I_CSRRWI::ISA_RV32I_CSRRWI(uint64_t _RD_, uint64_t _IMM5_, uint64_t _CSR_ ):
  Rv32Instruction("ISA_RV32I_CSRRWI", 32), _OPCODE(7, 115, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 5, "_FUNCT3"), _IMM5(5, _IMM5_, "_IMM5"), _CSR(12, _CSR_, "_CSR")  {
  InstInit();
}
Rv32::ISA_RV32I_CSRRWI::ISA_RV32I_CSRRWI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_CSRRWI", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _IMM5(5, binAddr.SliceBit(15, 5), "_IMM5"), _CSR(12, binAddr.SliceBit(20, 12), "_CSR")  {
  InstInit();
}
void Rv32::ISA_RV32I_CSRRWI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_IMM5);
  AppendField(_CSR);
  attribute[Rv32KEYS::mnemonic] = "CSRRWI";
}
void Rv32::ISA_RV32I_CSRRWI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_CSRRWI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_CSRRWI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_CSRRWI run on system END!"));
}
Rv32::ISA_RV32I_CSRRSI::ISA_RV32I_CSRRSI(uint64_t _RD_, uint64_t _IMM5_, uint64_t _CSR_ ):
  Rv32Instruction("ISA_RV32I_CSRRSI", 32), _OPCODE(7, 115, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 6, "_FUNCT3"), _IMM5(5, _IMM5_, "_IMM5"), _CSR(12, _CSR_, "_CSR")  {
  InstInit();
}
Rv32::ISA_RV32I_CSRRSI::ISA_RV32I_CSRRSI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_CSRRSI", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _IMM5(5, binAddr.SliceBit(15, 5), "_IMM5"), _CSR(12, binAddr.SliceBit(20, 12), "_CSR")  {
  InstInit();
}
void Rv32::ISA_RV32I_CSRRSI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_IMM5);
  AppendField(_CSR);
  attribute[Rv32KEYS::mnemonic] = "CSRRSI";
}
void Rv32::ISA_RV32I_CSRRSI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_CSRRSI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_CSRRSI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_CSRRSI run on system END!"));
}
Rv32::ISA_RV32I_CSRRCI::ISA_RV32I_CSRRCI(uint64_t _RD_, uint64_t _IMM5_, uint64_t _CSR_ ):
  Rv32Instruction("ISA_RV32I_CSRRCI", 32), _OPCODE(7, 115, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 7, "_FUNCT3"), _IMM5(5, _IMM5_, "_IMM5"), _CSR(12, _CSR_, "_CSR")  {
  InstInit();
}
Rv32::ISA_RV32I_CSRRCI::ISA_RV32I_CSRRCI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_CSRRCI", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _IMM5(5, binAddr.SliceBit(15, 5), "_IMM5"), _CSR(12, binAddr.SliceBit(20, 12), "_CSR")  {
  InstInit();
}
void Rv32::ISA_RV32I_CSRRCI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_IMM5);
  AppendField(_CSR);
  attribute[Rv32KEYS::mnemonic] = "CSRRCI";
}
void Rv32::ISA_RV32I_CSRRCI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_CSRRCI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_CSRRCI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_CSRRCI run on system END!"));
}
Rv32::ISA_RV32I_MUL::ISA_RV32I_MUL(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_MUL", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 0, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 1, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_MUL::ISA_RV32I_MUL(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_MUL", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_MUL::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "MUL";
}
void Rv32::ISA_RV32I_MUL::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_MUL run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_MUL.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_MUL run on system END!"));
}
Rv32::ISA_RV32I_MULH::ISA_RV32I_MULH(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_MULH", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 1, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 1, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_MULH::ISA_RV32I_MULH(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_MULH", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_MULH::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "MULH";
}
void Rv32::ISA_RV32I_MULH::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_MULH run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_MULH.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_MULH run on system END!"));
}
Rv32::ISA_RV32I_MULHSU::ISA_RV32I_MULHSU(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_MULHSU", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 1, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_MULHSU::ISA_RV32I_MULHSU(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_MULHSU", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_MULHSU::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "MULHSU";
}
void Rv32::ISA_RV32I_MULHSU::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_MULHSU run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_MULHSU.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_MULHSU run on system END!"));
}
Rv32::ISA_RV32I_MULHU::ISA_RV32I_MULHU(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_MULHU", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 3, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 1, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_MULHU::ISA_RV32I_MULHU(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_MULHU", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_MULHU::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "MULHU";
}
void Rv32::ISA_RV32I_MULHU::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_MULHU run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_MULHU.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_MULHU run on system END!"));
}
Rv32::ISA_RV32I_DIV::ISA_RV32I_DIV(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_DIV", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 4, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 1, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_DIV::ISA_RV32I_DIV(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_DIV", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_DIV::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "DIV";
}
void Rv32::ISA_RV32I_DIV::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_DIV run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_DIV.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_DIV run on system END!"));
}
Rv32::ISA_RV32I_DIVU::ISA_RV32I_DIVU(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_DIVU", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 5, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 1, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_DIVU::ISA_RV32I_DIVU(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_DIVU", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_DIVU::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "DIVU";
}
void Rv32::ISA_RV32I_DIVU::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_DIVU run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_DIVU.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_DIVU run on system END!"));
}
Rv32::ISA_RV32I_REM::ISA_RV32I_REM(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_REM", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 6, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 1, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_REM::ISA_RV32I_REM(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_REM", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_REM::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "REM";
}
void Rv32::ISA_RV32I_REM::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_REM run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_REM.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_REM run on system END!"));
}
Rv32::ISA_RV32I_REMU::ISA_RV32I_REMU(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32I_REMU", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 7, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 1, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32I_REMU::ISA_RV32I_REMU(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_REMU", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32I_REMU::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "REMU";
}
void Rv32::ISA_RV32I_REMU::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_REMU run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_REMU.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_REMU run on system END!"));
}
Rv32::ISA_RV32C_NOP::ISA_RV32C_NOP(uint64_t _IMM5_, uint64_t _IMM1_ ):
  Rv32Instruction("ISA_RV32C_NOP", 16), _OPCODE(2, 1, "_OPCODE"), _IMM5(5, _IMM5_, "_IMM5"), _FUNCT5(5, 0, "_FUNCT5"), _IMM1(1, _IMM1_, "_IMM1"), _FUNCT3(3, 0, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_NOP::ISA_RV32C_NOP(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_NOP", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _IMM5(5, binAddr.SliceBit(2, 5), "_IMM5"), _FUNCT5(5, binAddr.SliceBit(7, 5), "_FUNCT5"), _IMM1(1, binAddr.SliceBit(12, 1), "_IMM1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_NOP::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM5);
  AppendField(_FUNCT5);
  AppendField(_IMM1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.NOP";
}
void Rv32::ISA_RV32C_NOP::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_NOP run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_NOP.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_NOP run on system END!"));
}
Rv32::ISA_RV32C_ADDI::ISA_RV32C_ADDI(uint64_t _IMM5_, uint64_t _RD_, uint64_t _IMM1_ ):
  Rv32Instruction("ISA_RV32C_ADDI", 16), _OPCODE(2, 1, "_OPCODE"), _IMM5(5, _IMM5_, "_IMM5"), _RD(5, _RD_, "_RD"), _IMM1(1, _IMM1_, "_IMM1"), _FUNCT3(3, 0, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_ADDI::ISA_RV32C_ADDI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_ADDI", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _IMM5(5, binAddr.SliceBit(2, 5), "_IMM5"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _IMM1(1, binAddr.SliceBit(12, 1), "_IMM1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_ADDI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM5);
  AppendField(_RD);
  AppendField(_IMM1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.ADDI";
}
void Rv32::ISA_RV32C_ADDI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_ADDI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_ADDI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_ADDI run on system END!"));
}
Rv32::ISA_RV32C_JAL::ISA_RV32C_JAL(uint64_t _IMM1_5_, uint64_t _IMM3_3_1_, uint64_t _IMM1_7_, uint64_t _IMM1_6_, uint64_t _IMM1_10_, uint64_t _IMM2_9_8_, uint64_t _IMM1_4_, uint64_t _IMM1_11_ ):
  Rv32Instruction("ISA_RV32C_JAL", 16), _OPCODE(2, 1, "_OPCODE"), _IMM1_5(1, _IMM1_5_, "_IMM1_5"), _IMM3_3_1(3, _IMM3_3_1_, "_IMM3_3_1"), _IMM1_7(1, _IMM1_7_, "_IMM1_7"), _IMM1_6(1, _IMM1_6_, "_IMM1_6"), _IMM1_10(1, _IMM1_10_, "_IMM1_10"), _IMM2_9_8(2, _IMM2_9_8_, "_IMM2_9_8"), _IMM1_4(1, _IMM1_4_, "_IMM1_4"), _IMM1_11(1, _IMM1_11_, "_IMM1_11"), _FUNCT3(3, 1, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_JAL::ISA_RV32C_JAL(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_JAL", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _IMM1_5(1, binAddr.SliceBit(2, 1), "_IMM1_5"), _IMM3_3_1(3, binAddr.SliceBit(3, 3), "_IMM3_3_1"), _IMM1_7(1, binAddr.SliceBit(6, 1), "_IMM1_7"), _IMM1_6(1, binAddr.SliceBit(7, 1), "_IMM1_6"), _IMM1_10(1, binAddr.SliceBit(8, 1), "_IMM1_10"), _IMM2_9_8(2, binAddr.SliceBit(9, 2), "_IMM2_9_8"), _IMM1_4(1, binAddr.SliceBit(11, 1), "_IMM1_4"), _IMM1_11(1, binAddr.SliceBit(12, 1), "_IMM1_11"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_JAL::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM1_5);
  AppendField(_IMM3_3_1);
  AppendField(_IMM1_7);
  AppendField(_IMM1_6);
  AppendField(_IMM1_10);
  AppendField(_IMM2_9_8);
  AppendField(_IMM1_4);
  AppendField(_IMM1_11);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.JAL";
}
void Rv32::ISA_RV32C_JAL::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_JAL run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_JAL.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_JAL run on system END!"));
}
Rv32::ISA_RV32C_LI::ISA_RV32C_LI(uint64_t _IMM5_, uint64_t _RD_, uint64_t _IMM1_ ):
  Rv32Instruction("ISA_RV32C_LI", 16), _OPCODE(2, 1, "_OPCODE"), _IMM5(5, _IMM5_, "_IMM5"), _RD(5, _RD_, "_RD"), _IMM1(1, _IMM1_, "_IMM1"), _FUNCT3(3, 2, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_LI::ISA_RV32C_LI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_LI", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _IMM5(5, binAddr.SliceBit(2, 5), "_IMM5"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _IMM1(1, binAddr.SliceBit(12, 1), "_IMM1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_LI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM5);
  AppendField(_RD);
  AppendField(_IMM1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.LI";
}
void Rv32::ISA_RV32C_LI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_LI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_LI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_LI run on system END!"));
}
Rv32::ISA_RV32C_ADDI16SP::ISA_RV32C_ADDI16SP(uint64_t _IMM1_5_, uint64_t _IMM2_7_8_, uint64_t _IMM1_6_, uint64_t _IMM1_4_, uint64_t _IMM1_9_ ):
  Rv32Instruction("ISA_RV32C_ADDI16SP", 16), _OPCODE(2, 1, "_OPCODE"), _IMM1_5(1, _IMM1_5_, "_IMM1_5"), _IMM2_7_8(2, _IMM2_7_8_, "_IMM2_7_8"), _IMM1_6(1, _IMM1_6_, "_IMM1_6"), _IMM1_4(1, _IMM1_4_, "_IMM1_4"), _FUNCT5(5, 2, "_FUNCT5"), _IMM1_9(1, _IMM1_9_, "_IMM1_9"), _FUNCT3(3, 3, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_ADDI16SP::ISA_RV32C_ADDI16SP(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_ADDI16SP", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _IMM1_5(1, binAddr.SliceBit(2, 1), "_IMM1_5"), _IMM2_7_8(2, binAddr.SliceBit(3, 2), "_IMM2_7_8"), _IMM1_6(1, binAddr.SliceBit(5, 1), "_IMM1_6"), _IMM1_4(1, binAddr.SliceBit(6, 1), "_IMM1_4"), _FUNCT5(5, binAddr.SliceBit(7, 5), "_FUNCT5"), _IMM1_9(1, binAddr.SliceBit(12, 1), "_IMM1_9"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_ADDI16SP::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM1_5);
  AppendField(_IMM2_7_8);
  AppendField(_IMM1_6);
  AppendField(_IMM1_4);
  AppendField(_FUNCT5);
  AppendField(_IMM1_9);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.ADDI16SP";
}
void Rv32::ISA_RV32C_ADDI16SP::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_ADDI16SP run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_ADDI16SP.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_ADDI16SP run on system END!"));
}
Rv32::ISA_RV32C_LUI::ISA_RV32C_LUI(uint64_t _IMM5_, uint64_t _RD_, uint64_t _IMM1_ ):
  Rv32Instruction("ISA_RV32C_LUI", 16), _OPCODE(2, 1, "_OPCODE"), _IMM5(5, _IMM5_, "_IMM5"), _RD(5, _RD_, "_RD"), _IMM1(1, _IMM1_, "_IMM1"), _FUNCT3(3, 3, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_LUI::ISA_RV32C_LUI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_LUI", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _IMM5(5, binAddr.SliceBit(2, 5), "_IMM5"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _IMM1(1, binAddr.SliceBit(12, 1), "_IMM1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_LUI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM5);
  AppendField(_RD);
  AppendField(_IMM1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.LUI";
}
void Rv32::ISA_RV32C_LUI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_LUI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_LUI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_LUI run on system END!"));
}
Rv32::ISA_RV32C_SRLI::ISA_RV32C_SRLI(uint64_t _IMM5_, uint64_t _RD_, uint64_t _IMM1_ ):
  Rv32Instruction("ISA_RV32C_SRLI", 16), _OPCODE(2, 1, "_OPCODE"), _IMM5(5, _IMM5_, "_IMM5"), _RD(3, _RD_, "_RD"), _FUNCT2(2, 0, "_FUNCT2"), _IMM1(1, _IMM1_, "_IMM1"), _FUNCT3(3, 4, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_SRLI::ISA_RV32C_SRLI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_SRLI", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _IMM5(5, binAddr.SliceBit(2, 5), "_IMM5"), _RD(3, binAddr.SliceBit(7, 3), "_RD"), _FUNCT2(2, binAddr.SliceBit(10, 2), "_FUNCT2"), _IMM1(1, binAddr.SliceBit(12, 1), "_IMM1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_SRLI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM5);
  AppendField(_RD);
  AppendField(_FUNCT2);
  AppendField(_IMM1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.SRLI";
}
void Rv32::ISA_RV32C_SRLI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_SRLI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_SRLI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_SRLI run on system END!"));
}
Rv32::ISA_RV32C_SRAI::ISA_RV32C_SRAI(uint64_t _IMM5_, uint64_t _RD_, uint64_t _IMM1_ ):
  Rv32Instruction("ISA_RV32C_SRAI", 16), _OPCODE(2, 1, "_OPCODE"), _IMM5(5, _IMM5_, "_IMM5"), _RD(3, _RD_, "_RD"), _FUNCT2(2, 1, "_FUNCT2"), _IMM1(1, _IMM1_, "_IMM1"), _FUNCT3(3, 4, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_SRAI::ISA_RV32C_SRAI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_SRAI", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _IMM5(5, binAddr.SliceBit(2, 5), "_IMM5"), _RD(3, binAddr.SliceBit(7, 3), "_RD"), _FUNCT2(2, binAddr.SliceBit(10, 2), "_FUNCT2"), _IMM1(1, binAddr.SliceBit(12, 1), "_IMM1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_SRAI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM5);
  AppendField(_RD);
  AppendField(_FUNCT2);
  AppendField(_IMM1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.SRAI";
}
void Rv32::ISA_RV32C_SRAI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_SRAI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_SRAI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_SRAI run on system END!"));
}
Rv32::ISA_RV32C_ANDI::ISA_RV32C_ANDI(uint64_t _IMM5_, uint64_t _RD_, uint64_t _IMM1_ ):
  Rv32Instruction("ISA_RV32C_ANDI", 16), _OPCODE(2, 1, "_OPCODE"), _IMM5(5, _IMM5_, "_IMM5"), _RD(3, _RD_, "_RD"), _FUNCT2(2, 2, "_FUNCT2"), _IMM1(1, _IMM1_, "_IMM1"), _FUNCT3(3, 4, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_ANDI::ISA_RV32C_ANDI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_ANDI", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _IMM5(5, binAddr.SliceBit(2, 5), "_IMM5"), _RD(3, binAddr.SliceBit(7, 3), "_RD"), _FUNCT2(2, binAddr.SliceBit(10, 2), "_FUNCT2"), _IMM1(1, binAddr.SliceBit(12, 1), "_IMM1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_ANDI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM5);
  AppendField(_RD);
  AppendField(_FUNCT2);
  AppendField(_IMM1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.ANDI";
}
void Rv32::ISA_RV32C_ANDI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_ANDI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_ANDI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_ANDI run on system END!"));
}
Rv32::ISA_RV32C_SUB::ISA_RV32C_SUB(uint64_t _RS2_, uint64_t _RD_ ):
  Rv32Instruction("ISA_RV32C_SUB", 16), _OPCODE(2, 1, "_OPCODE"), _RS2(3, _RS2_, "_RS2"), _FUNCT2(2, 0, "_FUNCT2"), _RD(3, _RD_, "_RD"), _FUNCT2_1(2, 3, "_FUNCT2_1"), _FUNCT1(1, 0, "_FUNCT1"), _FUNCT3(3, 4, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_SUB::ISA_RV32C_SUB(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_SUB", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _RS2(3, binAddr.SliceBit(2, 3), "_RS2"), _FUNCT2(2, binAddr.SliceBit(5, 2), "_FUNCT2"), _RD(3, binAddr.SliceBit(7, 3), "_RD"), _FUNCT2_1(2, binAddr.SliceBit(10, 2), "_FUNCT2_1"), _FUNCT1(1, binAddr.SliceBit(12, 1), "_FUNCT1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_SUB::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RS2);
  AppendField(_FUNCT2);
  AppendField(_RD);
  AppendField(_FUNCT2_1);
  AppendField(_FUNCT1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.SUB";
}
void Rv32::ISA_RV32C_SUB::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_SUB run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_SUB.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_SUB run on system END!"));
}
Rv32::ISA_RV32C_XOR::ISA_RV32C_XOR(uint64_t _RS2_, uint64_t _RD_ ):
  Rv32Instruction("ISA_RV32C_XOR", 16), _OPCODE(2, 1, "_OPCODE"), _RS2(3, _RS2_, "_RS2"), _FUNCT2(2, 1, "_FUNCT2"), _RD(3, _RD_, "_RD"), _FUNCT2_1(2, 3, "_FUNCT2_1"), _FUNCT1(1, 0, "_FUNCT1"), _FUNCT3(3, 4, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_XOR::ISA_RV32C_XOR(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_XOR", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _RS2(3, binAddr.SliceBit(2, 3), "_RS2"), _FUNCT2(2, binAddr.SliceBit(5, 2), "_FUNCT2"), _RD(3, binAddr.SliceBit(7, 3), "_RD"), _FUNCT2_1(2, binAddr.SliceBit(10, 2), "_FUNCT2_1"), _FUNCT1(1, binAddr.SliceBit(12, 1), "_FUNCT1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_XOR::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RS2);
  AppendField(_FUNCT2);
  AppendField(_RD);
  AppendField(_FUNCT2_1);
  AppendField(_FUNCT1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.XOR";
}
void Rv32::ISA_RV32C_XOR::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_XOR run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_XOR.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_XOR run on system END!"));
}
Rv32::ISA_RV32C_OR::ISA_RV32C_OR(uint64_t _RS2_, uint64_t _RD_ ):
  Rv32Instruction("ISA_RV32C_OR", 16), _OPCODE(2, 1, "_OPCODE"), _RS2(3, _RS2_, "_RS2"), _FUNCT2(2, 2, "_FUNCT2"), _RD(3, _RD_, "_RD"), _FUNCT2_1(2, 3, "_FUNCT2_1"), _FUNCT1(1, 0, "_FUNCT1"), _FUNCT3(3, 4, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_OR::ISA_RV32C_OR(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_OR", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _RS2(3, binAddr.SliceBit(2, 3), "_RS2"), _FUNCT2(2, binAddr.SliceBit(5, 2), "_FUNCT2"), _RD(3, binAddr.SliceBit(7, 3), "_RD"), _FUNCT2_1(2, binAddr.SliceBit(10, 2), "_FUNCT2_1"), _FUNCT1(1, binAddr.SliceBit(12, 1), "_FUNCT1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_OR::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RS2);
  AppendField(_FUNCT2);
  AppendField(_RD);
  AppendField(_FUNCT2_1);
  AppendField(_FUNCT1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.OR";
}
void Rv32::ISA_RV32C_OR::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_OR run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_OR.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_OR run on system END!"));
}
Rv32::ISA_RV32C_AND::ISA_RV32C_AND(uint64_t _RS2_, uint64_t _RD_ ):
  Rv32Instruction("ISA_RV32C_AND", 16), _OPCODE(2, 1, "_OPCODE"), _RS2(3, _RS2_, "_RS2"), _FUNCT2(2, 3, "_FUNCT2"), _RD(3, _RD_, "_RD"), _FUNCT2_1(2, 3, "_FUNCT2_1"), _FUNCT1(1, 0, "_FUNCT1"), _FUNCT3(3, 4, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_AND::ISA_RV32C_AND(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_AND", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _RS2(3, binAddr.SliceBit(2, 3), "_RS2"), _FUNCT2(2, binAddr.SliceBit(5, 2), "_FUNCT2"), _RD(3, binAddr.SliceBit(7, 3), "_RD"), _FUNCT2_1(2, binAddr.SliceBit(10, 2), "_FUNCT2_1"), _FUNCT1(1, binAddr.SliceBit(12, 1), "_FUNCT1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_AND::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RS2);
  AppendField(_FUNCT2);
  AppendField(_RD);
  AppendField(_FUNCT2_1);
  AppendField(_FUNCT1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.AND";
}
void Rv32::ISA_RV32C_AND::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_AND run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_AND.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_AND run on system END!"));
}
Rv32::ISA_RV32C_J::ISA_RV32C_J(uint64_t _IMM1_5_, uint64_t _IMM3_3_1_, uint64_t _IMM1_7_, uint64_t _IMM1_6_, uint64_t _IMM1_10_, uint64_t _IMM2_9_8_, uint64_t _IMM1_4_, uint64_t _IMM1_11_ ):
  Rv32Instruction("ISA_RV32C_J", 16), _OPCODE(2, 1, "_OPCODE"), _IMM1_5(1, _IMM1_5_, "_IMM1_5"), _IMM3_3_1(3, _IMM3_3_1_, "_IMM3_3_1"), _IMM1_7(1, _IMM1_7_, "_IMM1_7"), _IMM1_6(1, _IMM1_6_, "_IMM1_6"), _IMM1_10(1, _IMM1_10_, "_IMM1_10"), _IMM2_9_8(2, _IMM2_9_8_, "_IMM2_9_8"), _IMM1_4(1, _IMM1_4_, "_IMM1_4"), _IMM1_11(1, _IMM1_11_, "_IMM1_11"), _FUNCT3(3, 5, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_J::ISA_RV32C_J(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_J", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _IMM1_5(1, binAddr.SliceBit(2, 1), "_IMM1_5"), _IMM3_3_1(3, binAddr.SliceBit(3, 3), "_IMM3_3_1"), _IMM1_7(1, binAddr.SliceBit(6, 1), "_IMM1_7"), _IMM1_6(1, binAddr.SliceBit(7, 1), "_IMM1_6"), _IMM1_10(1, binAddr.SliceBit(8, 1), "_IMM1_10"), _IMM2_9_8(2, binAddr.SliceBit(9, 2), "_IMM2_9_8"), _IMM1_4(1, binAddr.SliceBit(11, 1), "_IMM1_4"), _IMM1_11(1, binAddr.SliceBit(12, 1), "_IMM1_11"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_J::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM1_5);
  AppendField(_IMM3_3_1);
  AppendField(_IMM1_7);
  AppendField(_IMM1_6);
  AppendField(_IMM1_10);
  AppendField(_IMM2_9_8);
  AppendField(_IMM1_4);
  AppendField(_IMM1_11);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.J";
}
void Rv32::ISA_RV32C_J::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_J run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_J.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_J run on system END!"));
}
Rv32::ISA_RV32C_BEQZ::ISA_RV32C_BEQZ(uint64_t _IMM1_5_, uint64_t _IMM2_2_1_, uint64_t _IMM2_6_7_, uint64_t _RS1_, uint64_t _IMM2_3_4_, uint64_t _IMM1_8_ ):
  Rv32Instruction("ISA_RV32C_BEQZ", 16), _OPCODE(2, 1, "_OPCODE"), _IMM1_5(1, _IMM1_5_, "_IMM1_5"), _IMM2_2_1(2, _IMM2_2_1_, "_IMM2_2_1"), _IMM2_6_7(2, _IMM2_6_7_, "_IMM2_6_7"), _RS1(3, _RS1_, "_RS1"), _IMM2_3_4(2, _IMM2_3_4_, "_IMM2_3_4"), _IMM1_8(1, _IMM1_8_, "_IMM1_8"), _FUNCT3(3, 6, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_BEQZ::ISA_RV32C_BEQZ(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_BEQZ", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _IMM1_5(1, binAddr.SliceBit(2, 1), "_IMM1_5"), _IMM2_2_1(2, binAddr.SliceBit(3, 2), "_IMM2_2_1"), _IMM2_6_7(2, binAddr.SliceBit(5, 2), "_IMM2_6_7"), _RS1(3, binAddr.SliceBit(7, 3), "_RS1"), _IMM2_3_4(2, binAddr.SliceBit(10, 2), "_IMM2_3_4"), _IMM1_8(1, binAddr.SliceBit(12, 1), "_IMM1_8"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_BEQZ::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM1_5);
  AppendField(_IMM2_2_1);
  AppendField(_IMM2_6_7);
  AppendField(_RS1);
  AppendField(_IMM2_3_4);
  AppendField(_IMM1_8);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.BEQZ";
}
void Rv32::ISA_RV32C_BEQZ::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_BEQZ run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_BEQZ.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_BEQZ run on system END!"));
}
Rv32::ISA_RV32C_BNEZ::ISA_RV32C_BNEZ(uint64_t _IMM1_5_, uint64_t _IMM2_2_1_, uint64_t _IMM2_6_7_, uint64_t _RS1_, uint64_t _IMM2_3_4_, uint64_t _IMM1_8_ ):
  Rv32Instruction("ISA_RV32C_BNEZ", 16), _OPCODE(2, 1, "_OPCODE"), _IMM1_5(1, _IMM1_5_, "_IMM1_5"), _IMM2_2_1(2, _IMM2_2_1_, "_IMM2_2_1"), _IMM2_6_7(2, _IMM2_6_7_, "_IMM2_6_7"), _RS1(3, _RS1_, "_RS1"), _IMM2_3_4(2, _IMM2_3_4_, "_IMM2_3_4"), _IMM1_8(1, _IMM1_8_, "_IMM1_8"), _FUNCT3(3, 7, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_BNEZ::ISA_RV32C_BNEZ(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_BNEZ", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _IMM1_5(1, binAddr.SliceBit(2, 1), "_IMM1_5"), _IMM2_2_1(2, binAddr.SliceBit(3, 2), "_IMM2_2_1"), _IMM2_6_7(2, binAddr.SliceBit(5, 2), "_IMM2_6_7"), _RS1(3, binAddr.SliceBit(7, 3), "_RS1"), _IMM2_3_4(2, binAddr.SliceBit(10, 2), "_IMM2_3_4"), _IMM1_8(1, binAddr.SliceBit(12, 1), "_IMM1_8"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_BNEZ::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM1_5);
  AppendField(_IMM2_2_1);
  AppendField(_IMM2_6_7);
  AppendField(_RS1);
  AppendField(_IMM2_3_4);
  AppendField(_IMM1_8);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.BNEZ";
}
void Rv32::ISA_RV32C_BNEZ::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_BNEZ run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_BNEZ.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_BNEZ run on system END!"));
}
Rv32::ISA_RV32C_ADDI4SPN::ISA_RV32C_ADDI4SPN(uint64_t _RD_, uint64_t _IMM1_3_, uint64_t _IMM1_2_, uint64_t _IMM4_9_6_, uint64_t _IMM2_5_4_ ):
  Rv32Instruction("ISA_RV32C_ADDI4SPN", 16), _OPCODE(2, 0, "_OPCODE"), _RD(3, _RD_, "_RD"), _IMM1_3(1, _IMM1_3_, "_IMM1_3"), _IMM1_2(1, _IMM1_2_, "_IMM1_2"), _IMM4_9_6(4, _IMM4_9_6_, "_IMM4_9_6"), _IMM2_5_4(2, _IMM2_5_4_, "_IMM2_5_4"), _FUNCT3(3, 0, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_ADDI4SPN::ISA_RV32C_ADDI4SPN(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_ADDI4SPN", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _RD(3, binAddr.SliceBit(2, 3), "_RD"), _IMM1_3(1, binAddr.SliceBit(5, 1), "_IMM1_3"), _IMM1_2(1, binAddr.SliceBit(6, 1), "_IMM1_2"), _IMM4_9_6(4, binAddr.SliceBit(7, 4), "_IMM4_9_6"), _IMM2_5_4(2, binAddr.SliceBit(11, 2), "_IMM2_5_4"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_ADDI4SPN::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_IMM1_3);
  AppendField(_IMM1_2);
  AppendField(_IMM4_9_6);
  AppendField(_IMM2_5_4);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.ADDI4SPN";
}
void Rv32::ISA_RV32C_ADDI4SPN::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_ADDI4SPN run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_ADDI4SPN.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_ADDI4SPN run on system END!"));
}
Rv32::ISA_RV32C_FLD::ISA_RV32C_FLD(uint64_t _RD_, uint64_t _IMM2_, uint64_t _RS1_, uint64_t _IMM3_ ):
  Rv32Instruction("ISA_RV32C_FLD", 16), _OPCODE(2, 0, "_OPCODE"), _RD(3, _RD_, "_RD"), _IMM2(2, _IMM2_, "_IMM2"), _RS1(3, _RS1_, "_RS1"), _IMM3(3, _IMM3_, "_IMM3"), _FUNCT3(3, 1, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_FLD::ISA_RV32C_FLD(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_FLD", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _RD(3, binAddr.SliceBit(2, 3), "_RD"), _IMM2(2, binAddr.SliceBit(5, 2), "_IMM2"), _RS1(3, binAddr.SliceBit(7, 3), "_RS1"), _IMM3(3, binAddr.SliceBit(10, 3), "_IMM3"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_FLD::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_IMM2);
  AppendField(_RS1);
  AppendField(_IMM3);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.FLD";
}
void Rv32::ISA_RV32C_FLD::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_FLD run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_FLD.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_FLD run on system END!"));
}
Rv32::ISA_RV32C_LW::ISA_RV32C_LW(uint64_t _RD_, uint64_t _IMM1_6_, uint64_t _IMM1_2_, uint64_t _RS1_, uint64_t _IMM3_3_5_ ):
  Rv32Instruction("ISA_RV32C_LW", 16), _OPCODE(2, 0, "_OPCODE"), _RD(3, _RD_, "_RD"), _IMM1_6(1, _IMM1_6_, "_IMM1_6"), _IMM1_2(1, _IMM1_2_, "_IMM1_2"), _RS1(3, _RS1_, "_RS1"), _IMM3_3_5(3, _IMM3_3_5_, "_IMM3_3_5"), _FUNCT3(3, 2, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_LW::ISA_RV32C_LW(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_LW", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _RD(3, binAddr.SliceBit(2, 3), "_RD"), _IMM1_6(1, binAddr.SliceBit(5, 1), "_IMM1_6"), _IMM1_2(1, binAddr.SliceBit(6, 1), "_IMM1_2"), _RS1(3, binAddr.SliceBit(7, 3), "_RS1"), _IMM3_3_5(3, binAddr.SliceBit(10, 3), "_IMM3_3_5"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_LW::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_IMM1_6);
  AppendField(_IMM1_2);
  AppendField(_RS1);
  AppendField(_IMM3_3_5);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.LW";
}
void Rv32::ISA_RV32C_LW::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_LW run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_LW.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_LW run on system END!"));
}
Rv32::ISA_RV32C_FLW::ISA_RV32C_FLW(uint64_t _RD_, uint64_t _IMM1_6_, uint64_t _IMM1_2_, uint64_t _RS1_, uint64_t _IMM3_3_5_ ):
  Rv32Instruction("ISA_RV32C_FLW", 16), _OPCODE(2, 0, "_OPCODE"), _RD(3, _RD_, "_RD"), _IMM1_6(1, _IMM1_6_, "_IMM1_6"), _IMM1_2(1, _IMM1_2_, "_IMM1_2"), _RS1(3, _RS1_, "_RS1"), _IMM3_3_5(3, _IMM3_3_5_, "_IMM3_3_5"), _FUNCT3(3, 3, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_FLW::ISA_RV32C_FLW(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_FLW", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _RD(3, binAddr.SliceBit(2, 3), "_RD"), _IMM1_6(1, binAddr.SliceBit(5, 1), "_IMM1_6"), _IMM1_2(1, binAddr.SliceBit(6, 1), "_IMM1_2"), _RS1(3, binAddr.SliceBit(7, 3), "_RS1"), _IMM3_3_5(3, binAddr.SliceBit(10, 3), "_IMM3_3_5"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_FLW::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_IMM1_6);
  AppendField(_IMM1_2);
  AppendField(_RS1);
  AppendField(_IMM3_3_5);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.FLW";
}
void Rv32::ISA_RV32C_FLW::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_FLW run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_FLW.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_FLW run on system END!"));
}
Rv32::ISA_RV32C_FSD::ISA_RV32C_FSD(uint64_t _RD_, uint64_t _IMM2_, uint64_t _RS1_, uint64_t _IMM3_ ):
  Rv32Instruction("ISA_RV32C_FSD", 16), _OPCODE(2, 0, "_OPCODE"), _RD(3, _RD_, "_RD"), _IMM2(2, _IMM2_, "_IMM2"), _RS1(3, _RS1_, "_RS1"), _IMM3(3, _IMM3_, "_IMM3"), _FUNCT3(3, 5, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_FSD::ISA_RV32C_FSD(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_FSD", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _RD(3, binAddr.SliceBit(2, 3), "_RD"), _IMM2(2, binAddr.SliceBit(5, 2), "_IMM2"), _RS1(3, binAddr.SliceBit(7, 3), "_RS1"), _IMM3(3, binAddr.SliceBit(10, 3), "_IMM3"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_FSD::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_IMM2);
  AppendField(_RS1);
  AppendField(_IMM3);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.FSD";
}
void Rv32::ISA_RV32C_FSD::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_FSD run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_FSD.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_FSD run on system END!"));
}
Rv32::ISA_RV32C_SW::ISA_RV32C_SW(uint64_t _RD_, uint64_t _IMM1_6_, uint64_t _IMM1_2_, uint64_t _RS1_, uint64_t _IMM3_3_5_ ):
  Rv32Instruction("ISA_RV32C_SW", 16), _OPCODE(2, 0, "_OPCODE"), _RD(3, _RD_, "_RD"), _IMM1_6(1, _IMM1_6_, "_IMM1_6"), _IMM1_2(1, _IMM1_2_, "_IMM1_2"), _RS1(3, _RS1_, "_RS1"), _IMM3_3_5(3, _IMM3_3_5_, "_IMM3_3_5"), _FUNCT3(3, 6, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_SW::ISA_RV32C_SW(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_SW", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _RD(3, binAddr.SliceBit(2, 3), "_RD"), _IMM1_6(1, binAddr.SliceBit(5, 1), "_IMM1_6"), _IMM1_2(1, binAddr.SliceBit(6, 1), "_IMM1_2"), _RS1(3, binAddr.SliceBit(7, 3), "_RS1"), _IMM3_3_5(3, binAddr.SliceBit(10, 3), "_IMM3_3_5"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_SW::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_IMM1_6);
  AppendField(_IMM1_2);
  AppendField(_RS1);
  AppendField(_IMM3_3_5);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.SW";
}
void Rv32::ISA_RV32C_SW::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_SW run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_SW.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_SW run on system END!"));
}
Rv32::ISA_RV32C_FSW::ISA_RV32C_FSW(uint64_t _RD_, uint64_t _IMM1_6_, uint64_t _IMM1_2_, uint64_t _RS1_, uint64_t _IMM3_3_5_ ):
  Rv32Instruction("ISA_RV32C_FSW", 16), _OPCODE(2, 0, "_OPCODE"), _RD(3, _RD_, "_RD"), _IMM1_6(1, _IMM1_6_, "_IMM1_6"), _IMM1_2(1, _IMM1_2_, "_IMM1_2"), _RS1(3, _RS1_, "_RS1"), _IMM3_3_5(3, _IMM3_3_5_, "_IMM3_3_5"), _FUNCT3(3, 6, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_FSW::ISA_RV32C_FSW(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_FSW", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _RD(3, binAddr.SliceBit(2, 3), "_RD"), _IMM1_6(1, binAddr.SliceBit(5, 1), "_IMM1_6"), _IMM1_2(1, binAddr.SliceBit(6, 1), "_IMM1_2"), _RS1(3, binAddr.SliceBit(7, 3), "_RS1"), _IMM3_3_5(3, binAddr.SliceBit(10, 3), "_IMM3_3_5"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_FSW::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_IMM1_6);
  AppendField(_IMM1_2);
  AppendField(_RS1);
  AppendField(_IMM3_3_5);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.FSW";
}
void Rv32::ISA_RV32C_FSW::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_FSW run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_FSW.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_FSW run on system END!"));
}
Rv32::ISA_RV32C_SLLI::ISA_RV32C_SLLI(uint64_t _IMM5_, uint64_t _RD_, uint64_t _IMM1_ ):
  Rv32Instruction("ISA_RV32C_SLLI", 16), _OPCODE(2, 2, "_OPCODE"), _IMM5(5, _IMM5_, "_IMM5"), _RD(5, _RD_, "_RD"), _IMM1(1, _IMM1_, "_IMM1"), _FUNCT3(3, 0, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_SLLI::ISA_RV32C_SLLI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_SLLI", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _IMM5(5, binAddr.SliceBit(2, 5), "_IMM5"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _IMM1(1, binAddr.SliceBit(12, 1), "_IMM1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_SLLI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM5);
  AppendField(_RD);
  AppendField(_IMM1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.SLLI";
}
void Rv32::ISA_RV32C_SLLI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_SLLI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_SLLI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_SLLI run on system END!"));
}
Rv32::ISA_RV32C_FLDSP::ISA_RV32C_FLDSP(uint64_t _IMM3_, uint64_t _IMM2_, uint64_t _RD_, uint64_t _IMM1_ ):
  Rv32Instruction("ISA_RV32C_FLDSP", 16), _OPCODE(2, 2, "_OPCODE"), _IMM3(3, _IMM3_, "_IMM3"), _IMM2(2, _IMM2_, "_IMM2"), _RD(5, _RD_, "_RD"), _IMM1(1, _IMM1_, "_IMM1"), _FUNCT3(3, 1, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_FLDSP::ISA_RV32C_FLDSP(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_FLDSP", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _IMM3(3, binAddr.SliceBit(2, 3), "_IMM3"), _IMM2(2, binAddr.SliceBit(5, 2), "_IMM2"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _IMM1(1, binAddr.SliceBit(12, 1), "_IMM1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_FLDSP::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM3);
  AppendField(_IMM2);
  AppendField(_RD);
  AppendField(_IMM1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.FLDSP";
}
void Rv32::ISA_RV32C_FLDSP::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_FLDSP run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_FLDSP.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_FLDSP run on system END!"));
}
Rv32::ISA_RV32C_LWSP::ISA_RV32C_LWSP(uint64_t _IMM2_7_6_, uint64_t _IMM3_4_2_, uint64_t _RD_, uint64_t _IMM1_5_ ):
  Rv32Instruction("ISA_RV32C_LWSP", 16), _OPCODE(2, 2, "_OPCODE"), _IMM2_7_6(2, _IMM2_7_6_, "_IMM2_7_6"), _IMM3_4_2(3, _IMM3_4_2_, "_IMM3_4_2"), _RD(5, _RD_, "_RD"), _IMM1_5(1, _IMM1_5_, "_IMM1_5"), _FUNCT3(3, 2, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_LWSP::ISA_RV32C_LWSP(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_LWSP", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _IMM2_7_6(2, binAddr.SliceBit(2, 2), "_IMM2_7_6"), _IMM3_4_2(3, binAddr.SliceBit(4, 3), "_IMM3_4_2"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _IMM1_5(1, binAddr.SliceBit(12, 1), "_IMM1_5"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_LWSP::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM2_7_6);
  AppendField(_IMM3_4_2);
  AppendField(_RD);
  AppendField(_IMM1_5);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.LWSP";
}
void Rv32::ISA_RV32C_LWSP::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_LWSP run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_LWSP.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_LWSP run on system END!"));
}
Rv32::ISA_RV32C_FLWSP::ISA_RV32C_FLWSP(uint64_t _IMM3_, uint64_t _IMM2_, uint64_t _RD_, uint64_t _IMM1_ ):
  Rv32Instruction("ISA_RV32C_FLWSP", 16), _OPCODE(2, 2, "_OPCODE"), _IMM3(3, _IMM3_, "_IMM3"), _IMM2(2, _IMM2_, "_IMM2"), _RD(5, _RD_, "_RD"), _IMM1(1, _IMM1_, "_IMM1"), _FUNCT3(3, 3, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_FLWSP::ISA_RV32C_FLWSP(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_FLWSP", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _IMM3(3, binAddr.SliceBit(2, 3), "_IMM3"), _IMM2(2, binAddr.SliceBit(5, 2), "_IMM2"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _IMM1(1, binAddr.SliceBit(12, 1), "_IMM1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_FLWSP::InstInit() {
  AppendField(_OPCODE);
  AppendField(_IMM3);
  AppendField(_IMM2);
  AppendField(_RD);
  AppendField(_IMM1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.FLWSP";
}
void Rv32::ISA_RV32C_FLWSP::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_FLWSP run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_FLWSP.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_FLWSP run on system END!"));
}
Rv32::ISA_RV32C_JR::ISA_RV32C_JR(uint64_t _RS1_ ):
  Rv32Instruction("ISA_RV32C_JR", 16), _OPCODE(2, 2, "_OPCODE"), _FUNCT5(5, 0, "_FUNCT5"), _RS1(5, _RS1_, "_RS1"), _FUNCT1(1, 0, "_FUNCT1"), _FUNCT3(3, 4, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_JR::ISA_RV32C_JR(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_JR", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _FUNCT5(5, binAddr.SliceBit(2, 5), "_FUNCT5"), _RS1(5, binAddr.SliceBit(7, 5), "_RS1"), _FUNCT1(1, binAddr.SliceBit(12, 1), "_FUNCT1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_JR::InstInit() {
  AppendField(_OPCODE);
  AppendField(_FUNCT5);
  AppendField(_RS1);
  AppendField(_FUNCT1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.JR";
}
void Rv32::ISA_RV32C_JR::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_JR run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_JR.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_JR run on system END!"));
}
Rv32::ISA_RV32C_MV::ISA_RV32C_MV(uint64_t _RS2_, uint64_t _RD_ ):
  Rv32Instruction("ISA_RV32C_MV", 16), _OPCODE(2, 2, "_OPCODE"), _RS2(5, _RS2_, "_RS2"), _RD(5, _RD_, "_RD"), _FUNCT1(1, 0, "_FUNCT1"), _FUNCT3(3, 4, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_MV::ISA_RV32C_MV(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_MV", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _RS2(5, binAddr.SliceBit(2, 5), "_RS2"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT1(1, binAddr.SliceBit(12, 1), "_FUNCT1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_MV::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RS2);
  AppendField(_RD);
  AppendField(_FUNCT1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.MV";
}
void Rv32::ISA_RV32C_MV::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_MV run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_MV.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_MV run on system END!"));
}
Rv32::ISA_RV32C_EBREAK::ISA_RV32C_EBREAK():
  Rv32Instruction("ISA_RV32C_EBREAK", 16), _OPCODE(2, 2, "_OPCODE"), _FUNCT5(5, 0, "_FUNCT5"), _FUNCT5_1(5, 0, "_FUNCT5_1"), _FUNCT1(1, 1, "_FUNCT1"), _FUNCT3(3, 4, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_EBREAK::ISA_RV32C_EBREAK(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_EBREAK", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _FUNCT5(5, binAddr.SliceBit(2, 5), "_FUNCT5"), _FUNCT5_1(5, binAddr.SliceBit(7, 5), "_FUNCT5_1"), _FUNCT1(1, binAddr.SliceBit(12, 1), "_FUNCT1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_EBREAK::InstInit() {
  AppendField(_OPCODE);
  AppendField(_FUNCT5);
  AppendField(_FUNCT5_1);
  AppendField(_FUNCT1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.EBREAK";
}
void Rv32::ISA_RV32C_EBREAK::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_EBREAK run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_EBREAK.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_EBREAK run on system END!"));
}
Rv32::ISA_RV32C_JALR::ISA_RV32C_JALR(uint64_t _RS1_ ):
  Rv32Instruction("ISA_RV32C_JALR", 16), _OPCODE(2, 2, "_OPCODE"), _FUNCT5(5, 0, "_FUNCT5"), _RS1(5, _RS1_, "_RS1"), _FUNCT1(1, 1, "_FUNCT1"), _FUNCT3(3, 4, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_JALR::ISA_RV32C_JALR(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_JALR", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _FUNCT5(5, binAddr.SliceBit(2, 5), "_FUNCT5"), _RS1(5, binAddr.SliceBit(7, 5), "_RS1"), _FUNCT1(1, binAddr.SliceBit(12, 1), "_FUNCT1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_JALR::InstInit() {
  AppendField(_OPCODE);
  AppendField(_FUNCT5);
  AppendField(_RS1);
  AppendField(_FUNCT1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.JALR";
}
void Rv32::ISA_RV32C_JALR::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_JALR run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_JALR.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_JALR run on system END!"));
}
Rv32::ISA_RV32C_ADD::ISA_RV32C_ADD(uint64_t _RS2_, uint64_t _RD_ ):
  Rv32Instruction("ISA_RV32C_ADD", 16), _OPCODE(2, 2, "_OPCODE"), _RS2(5, _RS2_, "_RS2"), _RD(5, _RD_, "_RD"), _FUNCT1(1, 1, "_FUNCT1"), _FUNCT3(3, 4, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_ADD::ISA_RV32C_ADD(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_ADD", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _RS2(5, binAddr.SliceBit(2, 5), "_RS2"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT1(1, binAddr.SliceBit(12, 1), "_FUNCT1"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_ADD::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RS2);
  AppendField(_RD);
  AppendField(_FUNCT1);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.ADD";
}
void Rv32::ISA_RV32C_ADD::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_ADD run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_ADD.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_ADD run on system END!"));
}
Rv32::ISA_RV32C_FSDSP::ISA_RV32C_FSDSP(uint64_t _RS2_, uint64_t _IMM3_8_6_, uint64_t _IMM3_5_3_ ):
  Rv32Instruction("ISA_RV32C_FSDSP", 16), _OPCODE(2, 2, "_OPCODE"), _RS2(5, _RS2_, "_RS2"), _IMM3_8_6(3, _IMM3_8_6_, "_IMM3_8_6"), _IMM3_5_3(3, _IMM3_5_3_, "_IMM3_5_3"), _FUNCT3(3, 5, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_FSDSP::ISA_RV32C_FSDSP(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_FSDSP", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _RS2(5, binAddr.SliceBit(2, 5), "_RS2"), _IMM3_8_6(3, binAddr.SliceBit(7, 3), "_IMM3_8_6"), _IMM3_5_3(3, binAddr.SliceBit(10, 3), "_IMM3_5_3"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_FSDSP::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RS2);
  AppendField(_IMM3_8_6);
  AppendField(_IMM3_5_3);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.FSDSP";
}
void Rv32::ISA_RV32C_FSDSP::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_FSDSP run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_FSDSP.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_FSDSP run on system END!"));
}
Rv32::ISA_RV32C_SWSP::ISA_RV32C_SWSP(uint64_t _RS2_, uint64_t _IMM2_7_6_, uint64_t _IMM4_5_2_ ):
  Rv32Instruction("ISA_RV32C_SWSP", 16), _OPCODE(2, 2, "_OPCODE"), _RS2(5, _RS2_, "_RS2"), _IMM2_7_6(2, _IMM2_7_6_, "_IMM2_7_6"), _IMM4_5_2(4, _IMM4_5_2_, "_IMM4_5_2"), _FUNCT3(3, 6, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_SWSP::ISA_RV32C_SWSP(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_SWSP", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _RS2(5, binAddr.SliceBit(2, 5), "_RS2"), _IMM2_7_6(2, binAddr.SliceBit(7, 2), "_IMM2_7_6"), _IMM4_5_2(4, binAddr.SliceBit(9, 4), "_IMM4_5_2"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_SWSP::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RS2);
  AppendField(_IMM2_7_6);
  AppendField(_IMM4_5_2);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.SWSP";
}
void Rv32::ISA_RV32C_SWSP::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_SWSP run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_SWSP.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_SWSP run on system END!"));
}
Rv32::ISA_RV32C_FSWSP::ISA_RV32C_FSWSP(uint64_t _RS2_, uint64_t _IMM2_7_6_, uint64_t _IMM4_5_2_ ):
  Rv32Instruction("ISA_RV32C_FSWSP", 16), _OPCODE(2, 2, "_OPCODE"), _RS2(5, _RS2_, "_RS2"), _IMM2_7_6(2, _IMM2_7_6_, "_IMM2_7_6"), _IMM4_5_2(4, _IMM4_5_2_, "_IMM4_5_2"), _FUNCT3(3, 7, "_FUNCT3")  {
  InstInit();
}
Rv32::ISA_RV32C_FSWSP::ISA_RV32C_FSWSP(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32C_FSWSP", 16), _OPCODE(2, binAddr.SliceBit(0, 2), "_OPCODE"), _RS2(5, binAddr.SliceBit(2, 5), "_RS2"), _IMM2_7_6(2, binAddr.SliceBit(7, 2), "_IMM2_7_6"), _IMM4_5_2(4, binAddr.SliceBit(9, 4), "_IMM4_5_2"), _FUNCT3(3, binAddr.SliceBit(13, 3), "_FUNCT3")  {
  InstInit();
}
void Rv32::ISA_RV32C_FSWSP::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RS2);
  AppendField(_IMM2_7_6);
  AppendField(_IMM4_5_2);
  AppendField(_FUNCT3);
  attribute[Rv32KEYS::mnemonic] = "C.FSWSP";
}
void Rv32::ISA_RV32C_FSWSP::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32C_FSWSP run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32C_FSWSP.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32C_FSWSP run on system END!"));
}
Rv32::ISA_RV32I_SRET::ISA_RV32I_SRET():
  Rv32Instruction("ISA_RV32I_SRET", 32), _OPCODE1(7, 115, "_OPCODE1"), _OPCODE2(5, 0, "_OPCODE2"), _OPCODE3(3, 0, "_OPCODE3"), _OPCODE4(5, 0, "_OPCODE4"), _OPCODE5(5, 2, "_OPCODE5"), _OPCODE6(7, 8, "_OPCODE6")  {
  InstInit();
}
Rv32::ISA_RV32I_SRET::ISA_RV32I_SRET(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_SRET", 32), _OPCODE1(7, binAddr.SliceBit(0, 7), "_OPCODE1"), _OPCODE2(5, binAddr.SliceBit(7, 5), "_OPCODE2"), _OPCODE3(3, binAddr.SliceBit(12, 3), "_OPCODE3"), _OPCODE4(5, binAddr.SliceBit(15, 5), "_OPCODE4"), _OPCODE5(5, binAddr.SliceBit(20, 5), "_OPCODE5"), _OPCODE6(7, binAddr.SliceBit(25, 7), "_OPCODE6")  {
  InstInit();
}
void Rv32::ISA_RV32I_SRET::InstInit() {
  AppendField(_OPCODE1);
  AppendField(_OPCODE2);
  AppendField(_OPCODE3);
  AppendField(_OPCODE4);
  AppendField(_OPCODE5);
  AppendField(_OPCODE6);
  attribute[Rv32KEYS::mnemonic] = "SRET";
}
void Rv32::ISA_RV32I_SRET::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_SRET run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_SRET.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_SRET run on system END!"));
}
Rv32::ISA_RV32I_MRET::ISA_RV32I_MRET():
  Rv32Instruction("ISA_RV32I_MRET", 32), _OPCODE1(7, 115, "_OPCODE1"), _OPCODE2(5, 0, "_OPCODE2"), _OPCODE3(3, 0, "_OPCODE3"), _OPCODE4(5, 0, "_OPCODE4"), _OPCODE5(5, 2, "_OPCODE5"), _OPCODE6(7, 24, "_OPCODE6")  {
  InstInit();
}
Rv32::ISA_RV32I_MRET::ISA_RV32I_MRET(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32I_MRET", 32), _OPCODE1(7, binAddr.SliceBit(0, 7), "_OPCODE1"), _OPCODE2(5, binAddr.SliceBit(7, 5), "_OPCODE2"), _OPCODE3(3, binAddr.SliceBit(12, 3), "_OPCODE3"), _OPCODE4(5, binAddr.SliceBit(15, 5), "_OPCODE4"), _OPCODE5(5, binAddr.SliceBit(20, 5), "_OPCODE5"), _OPCODE6(7, binAddr.SliceBit(25, 7), "_OPCODE6")  {
  InstInit();
}
void Rv32::ISA_RV32I_MRET::InstInit() {
  AppendField(_OPCODE1);
  AppendField(_OPCODE2);
  AppendField(_OPCODE3);
  AppendField(_OPCODE4);
  AppendField(_OPCODE5);
  AppendField(_OPCODE6);
  attribute[Rv32KEYS::mnemonic] = "MRET";
}
void Rv32::ISA_RV32I_MRET::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32I_MRET run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32I_MRET.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32I_MRET run on system END!"));
}
Rv32::ISA_RV32A_LR_W::ISA_RV32A_LR_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RL_, uint64_t _AQ_ ):
  Rv32Instruction("ISA_RV32A_LR_W", 32), _OPCODE(7, 47, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _FUNCT5_1(5, 0, "_FUNCT5_1"), _RL(1, _RL_, "_RL"), _AQ(1, _AQ_, "_AQ"), _FUNCT5_2(5, 2, "_FUNCT5_2")  {
  InstInit();
}
Rv32::ISA_RV32A_LR_W::ISA_RV32A_LR_W(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32A_LR_W", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _FUNCT5_1(5, binAddr.SliceBit(20, 5), "_FUNCT5_1"), _RL(1, binAddr.SliceBit(25, 1), "_RL"), _AQ(1, binAddr.SliceBit(26, 1), "_AQ"), _FUNCT5_2(5, binAddr.SliceBit(27, 5), "_FUNCT5_2")  {
  InstInit();
}
void Rv32::ISA_RV32A_LR_W::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_FUNCT5_1);
  AppendField(_RL);
  AppendField(_AQ);
  AppendField(_FUNCT5_2);
  attribute[Rv32KEYS::mnemonic] = "LR.W";
}
void Rv32::ISA_RV32A_LR_W::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32A_LR_W run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32A_LR_W.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32A_LR_W run on system END!"));
}
Rv32::ISA_RV32A_SC_W::ISA_RV32A_SC_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ ):
  Rv32Instruction("ISA_RV32A_SC_W", 32), _OPCODE(7, 47, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _RL(1, _RL_, "_RL"), _AQ(1, _AQ_, "_AQ"), _FUNCT5(5, 3, "_FUNCT5")  {
  InstInit();
}
Rv32::ISA_RV32A_SC_W::ISA_RV32A_SC_W(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32A_SC_W", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _RL(1, binAddr.SliceBit(25, 1), "_RL"), _AQ(1, binAddr.SliceBit(26, 1), "_AQ"), _FUNCT5(5, binAddr.SliceBit(27, 5), "_FUNCT5")  {
  InstInit();
}
void Rv32::ISA_RV32A_SC_W::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_RL);
  AppendField(_AQ);
  AppendField(_FUNCT5);
  attribute[Rv32KEYS::mnemonic] = "SC.W";
}
void Rv32::ISA_RV32A_SC_W::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32A_SC_W run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32A_SC_W.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32A_SC_W run on system END!"));
}
Rv32::ISA_RV32A_AMOSWAP_W::ISA_RV32A_AMOSWAP_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ ):
  Rv32Instruction("ISA_RV32A_AMOSWAP_W", 32), _OPCODE(7, 47, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _RL(1, _RL_, "_RL"), _AQ(1, _AQ_, "_AQ"), _FUNCT5(5, 1, "_FUNCT5")  {
  InstInit();
}
Rv32::ISA_RV32A_AMOSWAP_W::ISA_RV32A_AMOSWAP_W(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32A_AMOSWAP_W", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _RL(1, binAddr.SliceBit(25, 1), "_RL"), _AQ(1, binAddr.SliceBit(26, 1), "_AQ"), _FUNCT5(5, binAddr.SliceBit(27, 5), "_FUNCT5")  {
  InstInit();
}
void Rv32::ISA_RV32A_AMOSWAP_W::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_RL);
  AppendField(_AQ);
  AppendField(_FUNCT5);
  attribute[Rv32KEYS::mnemonic] = "AMOSWAP.W";
}
void Rv32::ISA_RV32A_AMOSWAP_W::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32A_AMOSWAP_W run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32A_AMOSWAP_W.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32A_AMOSWAP_W run on system END!"));
}
Rv32::ISA_RV32A_AMOADD_W::ISA_RV32A_AMOADD_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ ):
  Rv32Instruction("ISA_RV32A_AMOADD_W", 32), _OPCODE(7, 47, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _RL(1, _RL_, "_RL"), _AQ(1, _AQ_, "_AQ"), _FUNCT5(5, 0, "_FUNCT5")  {
  InstInit();
}
Rv32::ISA_RV32A_AMOADD_W::ISA_RV32A_AMOADD_W(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32A_AMOADD_W", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _RL(1, binAddr.SliceBit(25, 1), "_RL"), _AQ(1, binAddr.SliceBit(26, 1), "_AQ"), _FUNCT5(5, binAddr.SliceBit(27, 5), "_FUNCT5")  {
  InstInit();
}
void Rv32::ISA_RV32A_AMOADD_W::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_RL);
  AppendField(_AQ);
  AppendField(_FUNCT5);
  attribute[Rv32KEYS::mnemonic] = "AMOADD.W";
}
void Rv32::ISA_RV32A_AMOADD_W::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32A_AMOADD_W run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32A_AMOADD_W.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32A_AMOADD_W run on system END!"));
}
Rv32::ISA_RV32A_AMOXOR_W::ISA_RV32A_AMOXOR_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ ):
  Rv32Instruction("ISA_RV32A_AMOXOR_W", 32), _OPCODE(7, 47, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _RL(1, _RL_, "_RL"), _AQ(1, _AQ_, "_AQ"), _FUNCT5(5, 4, "_FUNCT5")  {
  InstInit();
}
Rv32::ISA_RV32A_AMOXOR_W::ISA_RV32A_AMOXOR_W(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32A_AMOXOR_W", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _RL(1, binAddr.SliceBit(25, 1), "_RL"), _AQ(1, binAddr.SliceBit(26, 1), "_AQ"), _FUNCT5(5, binAddr.SliceBit(27, 5), "_FUNCT5")  {
  InstInit();
}
void Rv32::ISA_RV32A_AMOXOR_W::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_RL);
  AppendField(_AQ);
  AppendField(_FUNCT5);
  attribute[Rv32KEYS::mnemonic] = "AMOXOR.W";
}
void Rv32::ISA_RV32A_AMOXOR_W::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32A_AMOXOR_W run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32A_AMOXOR_W.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32A_AMOXOR_W run on system END!"));
}
Rv32::ISA_RV32A_AMOAND_W::ISA_RV32A_AMOAND_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ ):
  Rv32Instruction("ISA_RV32A_AMOAND_W", 32), _OPCODE(7, 47, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _RL(1, _RL_, "_RL"), _AQ(1, _AQ_, "_AQ"), _FUNCT5(5, 12, "_FUNCT5")  {
  InstInit();
}
Rv32::ISA_RV32A_AMOAND_W::ISA_RV32A_AMOAND_W(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32A_AMOAND_W", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _RL(1, binAddr.SliceBit(25, 1), "_RL"), _AQ(1, binAddr.SliceBit(26, 1), "_AQ"), _FUNCT5(5, binAddr.SliceBit(27, 5), "_FUNCT5")  {
  InstInit();
}
void Rv32::ISA_RV32A_AMOAND_W::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_RL);
  AppendField(_AQ);
  AppendField(_FUNCT5);
  attribute[Rv32KEYS::mnemonic] = "AMOAND.W";
}
void Rv32::ISA_RV32A_AMOAND_W::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32A_AMOAND_W run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32A_AMOAND_W.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32A_AMOAND_W run on system END!"));
}
Rv32::ISA_RV32A_AMOOR_W::ISA_RV32A_AMOOR_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ ):
  Rv32Instruction("ISA_RV32A_AMOOR_W", 32), _OPCODE(7, 47, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _RL(1, _RL_, "_RL"), _AQ(1, _AQ_, "_AQ"), _FUNCT5(5, 8, "_FUNCT5")  {
  InstInit();
}
Rv32::ISA_RV32A_AMOOR_W::ISA_RV32A_AMOOR_W(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32A_AMOOR_W", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _RL(1, binAddr.SliceBit(25, 1), "_RL"), _AQ(1, binAddr.SliceBit(26, 1), "_AQ"), _FUNCT5(5, binAddr.SliceBit(27, 5), "_FUNCT5")  {
  InstInit();
}
void Rv32::ISA_RV32A_AMOOR_W::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_RL);
  AppendField(_AQ);
  AppendField(_FUNCT5);
  attribute[Rv32KEYS::mnemonic] = "AMOOR.W";
}
void Rv32::ISA_RV32A_AMOOR_W::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32A_AMOOR_W run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32A_AMOOR_W.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32A_AMOOR_W run on system END!"));
}
Rv32::ISA_RV32A_AMOMIN_W::ISA_RV32A_AMOMIN_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ ):
  Rv32Instruction("ISA_RV32A_AMOMIN_W", 32), _OPCODE(7, 47, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _RL(1, _RL_, "_RL"), _AQ(1, _AQ_, "_AQ"), _FUNCT5(5, 16, "_FUNCT5")  {
  InstInit();
}
Rv32::ISA_RV32A_AMOMIN_W::ISA_RV32A_AMOMIN_W(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32A_AMOMIN_W", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _RL(1, binAddr.SliceBit(25, 1), "_RL"), _AQ(1, binAddr.SliceBit(26, 1), "_AQ"), _FUNCT5(5, binAddr.SliceBit(27, 5), "_FUNCT5")  {
  InstInit();
}
void Rv32::ISA_RV32A_AMOMIN_W::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_RL);
  AppendField(_AQ);
  AppendField(_FUNCT5);
  attribute[Rv32KEYS::mnemonic] = "AMOMIN.W";
}
void Rv32::ISA_RV32A_AMOMIN_W::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32A_AMOMIN_W run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32A_AMOMIN_W.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32A_AMOMIN_W run on system END!"));
}
Rv32::ISA_RV32A_AMOMAX_W::ISA_RV32A_AMOMAX_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ ):
  Rv32Instruction("ISA_RV32A_AMOMAX_W", 32), _OPCODE(7, 47, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _RL(1, _RL_, "_RL"), _AQ(1, _AQ_, "_AQ"), _FUNCT5(5, 20, "_FUNCT5")  {
  InstInit();
}
Rv32::ISA_RV32A_AMOMAX_W::ISA_RV32A_AMOMAX_W(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32A_AMOMAX_W", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _RL(1, binAddr.SliceBit(25, 1), "_RL"), _AQ(1, binAddr.SliceBit(26, 1), "_AQ"), _FUNCT5(5, binAddr.SliceBit(27, 5), "_FUNCT5")  {
  InstInit();
}
void Rv32::ISA_RV32A_AMOMAX_W::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_RL);
  AppendField(_AQ);
  AppendField(_FUNCT5);
  attribute[Rv32KEYS::mnemonic] = "AMOMAX.W";
}
void Rv32::ISA_RV32A_AMOMAX_W::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32A_AMOMAX_W run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32A_AMOMAX_W.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32A_AMOMAX_W run on system END!"));
}
Rv32::ISA_RV32A_AMOMINU_W::ISA_RV32A_AMOMINU_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ ):
  Rv32Instruction("ISA_RV32A_AMOMINU_W", 32), _OPCODE(7, 47, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _RL(1, _RL_, "_RL"), _AQ(1, _AQ_, "_AQ"), _FUNCT5(5, 24, "_FUNCT5")  {
  InstInit();
}
Rv32::ISA_RV32A_AMOMINU_W::ISA_RV32A_AMOMINU_W(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32A_AMOMINU_W", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _RL(1, binAddr.SliceBit(25, 1), "_RL"), _AQ(1, binAddr.SliceBit(26, 1), "_AQ"), _FUNCT5(5, binAddr.SliceBit(27, 5), "_FUNCT5")  {
  InstInit();
}
void Rv32::ISA_RV32A_AMOMINU_W::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_RL);
  AppendField(_AQ);
  AppendField(_FUNCT5);
  attribute[Rv32KEYS::mnemonic] = "AMOMINU.W";
}
void Rv32::ISA_RV32A_AMOMINU_W::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32A_AMOMINU_W run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32A_AMOMINU_W.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32A_AMOMINU_W run on system END!"));
}
Rv32::ISA_RV32A_AMOMAXU_W::ISA_RV32A_AMOMAXU_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ ):
  Rv32Instruction("ISA_RV32A_AMOMAXU_W", 32), _OPCODE(7, 47, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 2, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _RL(1, _RL_, "_RL"), _AQ(1, _AQ_, "_AQ"), _FUNCT5(5, 28, "_FUNCT5")  {
  InstInit();
}
Rv32::ISA_RV32A_AMOMAXU_W::ISA_RV32A_AMOMAXU_W(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32A_AMOMAXU_W", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _RL(1, binAddr.SliceBit(25, 1), "_RL"), _AQ(1, binAddr.SliceBit(26, 1), "_AQ"), _FUNCT5(5, binAddr.SliceBit(27, 5), "_FUNCT5")  {
  InstInit();
}
void Rv32::ISA_RV32A_AMOMAXU_W::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_RL);
  AppendField(_AQ);
  AppendField(_FUNCT5);
  attribute[Rv32KEYS::mnemonic] = "AMOMAXU.W";
}
void Rv32::ISA_RV32A_AMOMAXU_W::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32A_AMOMAXU_W run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32A_AMOMAXU_W.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32A_AMOMAXU_W run on system END!"));
}
Rv32::ISA_RV32Zbb_CLZ::ISA_RV32Zbb_CLZ(uint64_t _RD_, uint64_t _RS1_ ):
  Rv32Instruction("ISA_RV32Zbb_CLZ", 32), _OPCODE(7, 19, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 1, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _FUNCT5(5, 0, "_FUNCT5"), _FUNCT7(7, 48, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32Zbb_CLZ::ISA_RV32Zbb_CLZ(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32Zbb_CLZ", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _FUNCT5(5, binAddr.SliceBit(20, 5), "_FUNCT5"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32Zbb_CLZ::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_FUNCT5);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "CLZ";
}
void Rv32::ISA_RV32Zbb_CLZ::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32Zbb_CLZ run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32Zbb_CLZ.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32Zbb_CLZ run on system END!"));
}
Rv32::ISA_RV32Zbb_CTZ::ISA_RV32Zbb_CTZ(uint64_t _RD_, uint64_t _RS1_ ):
  Rv32Instruction("ISA_RV32Zbb_CTZ", 32), _OPCODE(7, 19, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 1, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _FUNCT5(5, 1, "_FUNCT5"), _FUNCT7(7, 48, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32Zbb_CTZ::ISA_RV32Zbb_CTZ(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32Zbb_CTZ", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _FUNCT5(5, binAddr.SliceBit(20, 5), "_FUNCT5"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32Zbb_CTZ::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_FUNCT5);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "CTZ";
}
void Rv32::ISA_RV32Zbb_CTZ::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32Zbb_CTZ run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32Zbb_CTZ.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32Zbb_CTZ run on system END!"));
}
Rv32::ISA_RV32Zbb_CPOP::ISA_RV32Zbb_CPOP(uint64_t _RD_, uint64_t _RS1_ ):
  Rv32Instruction("ISA_RV32Zbb_CPOP", 32), _OPCODE(7, 19, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 1, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _FUNCT5(5, 2, "_FUNCT5"), _FUNCT7(7, 48, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32Zbb_CPOP::ISA_RV32Zbb_CPOP(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32Zbb_CPOP", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _FUNCT5(5, binAddr.SliceBit(20, 5), "_FUNCT5"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32Zbb_CPOP::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_FUNCT5);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "CPOP";
}
void Rv32::ISA_RV32Zbb_CPOP::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32Zbb_CPOP run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32Zbb_CPOP.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32Zbb_CPOP run on system END!"));
}
Rv32::ISA_RV32Zbb_MIN::ISA_RV32Zbb_MIN(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32Zbb_MIN", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 4, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 5, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32Zbb_MIN::ISA_RV32Zbb_MIN(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32Zbb_MIN", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32Zbb_MIN::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "MIN";
}
void Rv32::ISA_RV32Zbb_MIN::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32Zbb_MIN run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32Zbb_MIN.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32Zbb_MIN run on system END!"));
}
Rv32::ISA_RV32Zbb_MAX::ISA_RV32Zbb_MAX(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32Zbb_MAX", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 6, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 5, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32Zbb_MAX::ISA_RV32Zbb_MAX(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32Zbb_MAX", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32Zbb_MAX::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "MAX";
}
void Rv32::ISA_RV32Zbb_MAX::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32Zbb_MAX run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32Zbb_MAX.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32Zbb_MAX run on system END!"));
}
Rv32::ISA_RV32Zbb_MINU::ISA_RV32Zbb_MINU(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32Zbb_MINU", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 5, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 5, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32Zbb_MINU::ISA_RV32Zbb_MINU(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32Zbb_MINU", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32Zbb_MINU::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "MINU";
}
void Rv32::ISA_RV32Zbb_MINU::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32Zbb_MINU run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32Zbb_MINU.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32Zbb_MINU run on system END!"));
}
Rv32::ISA_RV32Zbb_MAXU::ISA_RV32Zbb_MAXU(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32Zbb_MAXU", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 7, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 5, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32Zbb_MAXU::ISA_RV32Zbb_MAXU(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32Zbb_MAXU", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32Zbb_MAXU::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "MAXU";
}
void Rv32::ISA_RV32Zbb_MAXU::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32Zbb_MAXU run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32Zbb_MAXU.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32Zbb_MAXU run on system END!"));
}
Rv32::ISA_RV32Zbb_SEXT_B::ISA_RV32Zbb_SEXT_B(uint64_t _RD_, uint64_t _RS1_ ):
  Rv32Instruction("ISA_RV32Zbb_SEXT_B", 32), _OPCODE(7, 19, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 1, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _FUNCT5(5, 4, "_FUNCT5"), _FUNCT7(7, 48, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32Zbb_SEXT_B::ISA_RV32Zbb_SEXT_B(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32Zbb_SEXT_B", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _FUNCT5(5, binAddr.SliceBit(20, 5), "_FUNCT5"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32Zbb_SEXT_B::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_FUNCT5);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "SEXT.B";
}
void Rv32::ISA_RV32Zbb_SEXT_B::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32Zbb_SEXT_B run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32Zbb_SEXT_B.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32Zbb_SEXT_B run on system END!"));
}
Rv32::ISA_RV32Zbb_SEXT_H::ISA_RV32Zbb_SEXT_H(uint64_t _RD_, uint64_t _RS1_ ):
  Rv32Instruction("ISA_RV32Zbb_SEXT_H", 32), _OPCODE(7, 19, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 1, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _FUNCT5(5, 5, "_FUNCT5"), _FUNCT7(7, 48, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32Zbb_SEXT_H::ISA_RV32Zbb_SEXT_H(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32Zbb_SEXT_H", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _FUNCT5(5, binAddr.SliceBit(20, 5), "_FUNCT5"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32Zbb_SEXT_H::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_FUNCT5);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "SEXT.H";
}
void Rv32::ISA_RV32Zbb_SEXT_H::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32Zbb_SEXT_H run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32Zbb_SEXT_H.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32Zbb_SEXT_H run on system END!"));
}
Rv32::ISA_RV32Zbb_ZEXT_H::ISA_RV32Zbb_ZEXT_H(uint64_t _RD_, uint64_t _RS1_ ):
  Rv32Instruction("ISA_RV32Zbb_ZEXT_H", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 4, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _FUNCT5(5, 0, "_FUNCT5"), _FUNCT7(7, 4, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32Zbb_ZEXT_H::ISA_RV32Zbb_ZEXT_H(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32Zbb_ZEXT_H", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _FUNCT5(5, binAddr.SliceBit(20, 5), "_FUNCT5"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32Zbb_ZEXT_H::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_FUNCT5);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "ZEXT.H";
}
void Rv32::ISA_RV32Zbb_ZEXT_H::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32Zbb_ZEXT_H run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32Zbb_ZEXT_H.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32Zbb_ZEXT_H run on system END!"));
}
Rv32::ISA_RV32Zbb_ANDN::ISA_RV32Zbb_ANDN(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32Zbb_ANDN", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 7, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 32, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32Zbb_ANDN::ISA_RV32Zbb_ANDN(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32Zbb_ANDN", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32Zbb_ANDN::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "ANDN";
}
void Rv32::ISA_RV32Zbb_ANDN::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32Zbb_ANDN run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32Zbb_ANDN.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32Zbb_ANDN run on system END!"));
}
Rv32::ISA_RV32Zbb_ORN::ISA_RV32Zbb_ORN(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32Zbb_ORN", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 6, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 32, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32Zbb_ORN::ISA_RV32Zbb_ORN(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32Zbb_ORN", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32Zbb_ORN::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "ORN";
}
void Rv32::ISA_RV32Zbb_ORN::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32Zbb_ORN run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32Zbb_ORN.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32Zbb_ORN run on system END!"));
}
Rv32::ISA_RV32Zbb_XNOR::ISA_RV32Zbb_XNOR(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32Zbb_XNOR", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 4, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 32, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32Zbb_XNOR::ISA_RV32Zbb_XNOR(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32Zbb_XNOR", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32Zbb_XNOR::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "XNOR";
}
void Rv32::ISA_RV32Zbb_XNOR::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32Zbb_XNOR run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32Zbb_XNOR.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32Zbb_XNOR run on system END!"));
}
Rv32::ISA_RV32Zbb_ROL::ISA_RV32Zbb_ROL(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32Zbb_ROL", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 1, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 48, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32Zbb_ROL::ISA_RV32Zbb_ROL(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32Zbb_ROL", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32Zbb_ROL::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "ROL";
}
void Rv32::ISA_RV32Zbb_ROL::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32Zbb_ROL run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32Zbb_ROL.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32Zbb_ROL run on system END!"));
}
Rv32::ISA_RV32Zbb_ROR::ISA_RV32Zbb_ROR(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ ):
  Rv32Instruction("ISA_RV32Zbb_ROR", 32), _OPCODE(7, 51, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 5, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _RS2(5, _RS2_, "_RS2"), _FUNCT7(7, 48, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32Zbb_ROR::ISA_RV32Zbb_ROR(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32Zbb_ROR", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _RS2(5, binAddr.SliceBit(20, 5), "_RS2"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32Zbb_ROR::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_RS2);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "ROR";
}
void Rv32::ISA_RV32Zbb_ROR::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32Zbb_ROR run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32Zbb_ROR.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32Zbb_ROR run on system END!"));
}
Rv32::ISA_RV32Zbb_RORI::ISA_RV32Zbb_RORI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM_ ):
  Rv32Instruction("ISA_RV32Zbb_RORI", 32), _OPCODE(7, 19, "_OPCODE"), _RD(5, _RD_, "_RD"), _FUNCT3(3, 5, "_FUNCT3"), _RS1(5, _RS1_, "_RS1"), _IMM(5, _IMM_, "_IMM"), _FUNCT7(7, 48, "_FUNCT7")  {
  InstInit();
}
Rv32::ISA_RV32Zbb_RORI::ISA_RV32Zbb_RORI(InstBinary const& binAddr): 
  Rv32Instruction("ISA_RV32Zbb_RORI", 32), _OPCODE(7, binAddr.SliceBit(0, 7), "_OPCODE"), _RD(5, binAddr.SliceBit(7, 5), "_RD"), _FUNCT3(3, binAddr.SliceBit(12, 3), "_FUNCT3"), _RS1(5, binAddr.SliceBit(15, 5), "_RS1"), _IMM(5, binAddr.SliceBit(20, 5), "_IMM"), _FUNCT7(7, binAddr.SliceBit(25, 7), "_FUNCT7")  {
  InstInit();
}
void Rv32::ISA_RV32Zbb_RORI::InstInit() {
  AppendField(_OPCODE);
  AppendField(_RD);
  AppendField(_FUNCT3);
  AppendField(_RS1);
  AppendField(_IMM);
  AppendField(_FUNCT7);
  attribute[Rv32KEYS::mnemonic] = "RORI";
}
void Rv32::ISA_RV32Zbb_RORI::RunOnInstance(Instance* instInstance) {
  InstanceRv32 * instance = static_cast<InstanceRv32 *>(instInstance);
  SystemRv32 * system = dynamic_cast<SystemRv32 *>(instInstance->GetSystem());
  system->LogStr(std::string("-------------------ISA_RV32Zbb_RORI run on system START!"));
  #define INST_RUN_DEFINE
  #include "ISA_RV32Zbb_RORI.inc"
  #undef INST_RUN_DEFINE
  system->LogStr(std::string("-------------------ISA_RV32Zbb_RORI run on system END!"));
}

}  // namespace latch::rv32

#undef START_INST_CODE_DEFINE
