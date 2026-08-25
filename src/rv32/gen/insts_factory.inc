/**
**/
//
// 由 ISA DSL 工具生成。
//
// clang-format off
#pragma GCC diagnostic ignored "-Wunused-parameter"

// 按名字构造指令：item key / mnemonic + 整型参数 → ctor（编码指令与虚拟指令通用）。
// Rv32Program::MakeInst 的定义体（Program 虚函数的每架构覆盖，见 isa/program.h）。
// 在该架构的 .cpp 的 namespace latch::rv32 内 #include（放法同 insts_hex_parser.inc）。

std::shared_ptr<Instruction> Rv32Program::MakeInst(
    const std::string& name, const std::vector<uint64_t>& args) {
  (void)args;
  if (name == "ISA_RV32I_ADD" || name == "ADD") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_ADD expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_ADD>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_SUB" || name == "SUB") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_SUB expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_SUB>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_SLL" || name == "SLL") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_SLL expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_SLL>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_SLT" || name == "SLT") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_SLT expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_SLT>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_SLTU" || name == "SLTU") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_SLTU expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_SLTU>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_XOR" || name == "XOR") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_XOR expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_XOR>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_SRL" || name == "SRL") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_SRL expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_SRL>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_SRA" || name == "SRA") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_SRA expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_SRA>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_OR" || name == "OR") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_OR expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_OR>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_AND" || name == "AND") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_AND expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_AND>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_ADDI" || name == "ADDI") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_ADDI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_ADDI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_SLTI" || name == "SLTI") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_SLTI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_SLTI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_SLTIU" || name == "SLTIU") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_SLTIU expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_SLTIU>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_XORI" || name == "XORI") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_XORI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_XORI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_ORI" || name == "ORI") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_ORI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_ORI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_ANDI" || name == "ANDI") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_ANDI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_ANDI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_SLLI" || name == "SLLI") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_SLLI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_SLLI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_SRLI" || name == "SRLI") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_SRLI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_SRLI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_SRAI" || name == "SRAI") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_SRAI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_SRAI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_LB" || name == "LB") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_LB expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_LB>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_LH" || name == "LH") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_LH expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_LH>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_LW" || name == "LW") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_LW expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_LW>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_LBU" || name == "LBU") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_LBU expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_LBU>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_LHU" || name == "LHU") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_LHU expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_LHU>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_SB" || name == "SB") {
    LOGCHECK(args.size() == 4, "ISA_RV32I_SB expects 4 args");
    return std::make_shared<Rv32::ISA_RV32I_SB>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]));
  }
  if (name == "ISA_RV32I_SH" || name == "SH") {
    LOGCHECK(args.size() == 4, "ISA_RV32I_SH expects 4 args");
    return std::make_shared<Rv32::ISA_RV32I_SH>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]));
  }
  if (name == "ISA_RV32I_SW" || name == "SW") {
    LOGCHECK(args.size() == 4, "ISA_RV32I_SW expects 4 args");
    return std::make_shared<Rv32::ISA_RV32I_SW>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]));
  }
  if (name == "ISA_RV32I_BEQ" || name == "BEQ") {
    LOGCHECK(args.size() == 6, "ISA_RV32I_BEQ expects 6 args");
    return std::make_shared<Rv32::ISA_RV32I_BEQ>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]), static_cast<uint64_t>(args[5]));
  }
  if (name == "ISA_RV32I_BNE" || name == "BNE") {
    LOGCHECK(args.size() == 6, "ISA_RV32I_BNE expects 6 args");
    return std::make_shared<Rv32::ISA_RV32I_BNE>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]), static_cast<uint64_t>(args[5]));
  }
  if (name == "ISA_RV32I_BLT" || name == "BLT") {
    LOGCHECK(args.size() == 6, "ISA_RV32I_BLT expects 6 args");
    return std::make_shared<Rv32::ISA_RV32I_BLT>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]), static_cast<uint64_t>(args[5]));
  }
  if (name == "ISA_RV32I_BGE" || name == "BGE") {
    LOGCHECK(args.size() == 6, "ISA_RV32I_BGE expects 6 args");
    return std::make_shared<Rv32::ISA_RV32I_BGE>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]), static_cast<uint64_t>(args[5]));
  }
  if (name == "ISA_RV32I_BLTU" || name == "BLTU") {
    LOGCHECK(args.size() == 6, "ISA_RV32I_BLTU expects 6 args");
    return std::make_shared<Rv32::ISA_RV32I_BLTU>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]), static_cast<uint64_t>(args[5]));
  }
  if (name == "ISA_RV32I_BGEU" || name == "BGEU") {
    LOGCHECK(args.size() == 6, "ISA_RV32I_BGEU expects 6 args");
    return std::make_shared<Rv32::ISA_RV32I_BGEU>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]), static_cast<uint64_t>(args[5]));
  }
  if (name == "ISA_RV32I_LUI" || name == "LUI") {
    LOGCHECK(args.size() == 2, "ISA_RV32I_LUI expects 2 args");
    return std::make_shared<Rv32::ISA_RV32I_LUI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32I_AUIPC" || name == "AUIPC") {
    LOGCHECK(args.size() == 2, "ISA_RV32I_AUIPC expects 2 args");
    return std::make_shared<Rv32::ISA_RV32I_AUIPC>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32I_JAL" || name == "JAL") {
    LOGCHECK(args.size() == 5, "ISA_RV32I_JAL expects 5 args");
    return std::make_shared<Rv32::ISA_RV32I_JAL>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32I_JALR" || name == "JALR") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_JALR expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_JALR>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_FENCE" || name == "FENCE") {
    LOGCHECK(args.size() == 4, "ISA_RV32I_FENCE expects 4 args");
    return std::make_shared<Rv32::ISA_RV32I_FENCE>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]));
  }
  if (name == "ISA_RV32I_FENCE_I" || name == "FENCE_I") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_FENCE_I expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_FENCE_I>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_ECALL" || name == "ECALL") {
    LOGCHECK(args.size() == 2, "ISA_RV32I_ECALL expects 2 args");
    return std::make_shared<Rv32::ISA_RV32I_ECALL>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32I_EBREAK" || name == "EBREAK") {
    LOGCHECK(args.size() == 2, "ISA_RV32I_EBREAK expects 2 args");
    return std::make_shared<Rv32::ISA_RV32I_EBREAK>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32I_CSRRW" || name == "CSRRW") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_CSRRW expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_CSRRW>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_CSRRS" || name == "CSRRS") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_CSRRS expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_CSRRS>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_CSRRC" || name == "CSRRC") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_CSRRC expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_CSRRC>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_CSRRWI" || name == "CSRRWI") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_CSRRWI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_CSRRWI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_CSRRSI" || name == "CSRRSI") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_CSRRSI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_CSRRSI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_CSRRCI" || name == "CSRRCI") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_CSRRCI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_CSRRCI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_MUL" || name == "MUL") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_MUL expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_MUL>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_MULH" || name == "MULH") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_MULH expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_MULH>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_MULHSU" || name == "MULHSU") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_MULHSU expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_MULHSU>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_MULHU" || name == "MULHU") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_MULHU expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_MULHU>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_DIV" || name == "DIV") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_DIV expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_DIV>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_DIVU" || name == "DIVU") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_DIVU expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_DIVU>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_REM" || name == "REM") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_REM expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_REM>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_REMU" || name == "REMU") {
    LOGCHECK(args.size() == 3, "ISA_RV32I_REMU expects 3 args");
    return std::make_shared<Rv32::ISA_RV32I_REMU>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32C_NOP" || name == "C.NOP") {
    LOGCHECK(args.size() == 2, "ISA_RV32C_NOP expects 2 args");
    return std::make_shared<Rv32::ISA_RV32C_NOP>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32C_ADDI" || name == "C.ADDI") {
    LOGCHECK(args.size() == 3, "ISA_RV32C_ADDI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32C_ADDI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32C_JAL" || name == "C.JAL") {
    LOGCHECK(args.size() == 8, "ISA_RV32C_JAL expects 8 args");
    return std::make_shared<Rv32::ISA_RV32C_JAL>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]), static_cast<uint64_t>(args[5]), static_cast<uint64_t>(args[6]), static_cast<uint64_t>(args[7]));
  }
  if (name == "ISA_RV32C_LI" || name == "C.LI") {
    LOGCHECK(args.size() == 3, "ISA_RV32C_LI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32C_LI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32C_ADDI16SP" || name == "C.ADDI16SP") {
    LOGCHECK(args.size() == 5, "ISA_RV32C_ADDI16SP expects 5 args");
    return std::make_shared<Rv32::ISA_RV32C_ADDI16SP>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32C_LUI" || name == "C.LUI") {
    LOGCHECK(args.size() == 3, "ISA_RV32C_LUI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32C_LUI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32C_SRLI" || name == "C.SRLI") {
    LOGCHECK(args.size() == 3, "ISA_RV32C_SRLI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32C_SRLI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32C_SRAI" || name == "C.SRAI") {
    LOGCHECK(args.size() == 3, "ISA_RV32C_SRAI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32C_SRAI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32C_ANDI" || name == "C.ANDI") {
    LOGCHECK(args.size() == 3, "ISA_RV32C_ANDI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32C_ANDI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32C_SUB" || name == "C.SUB") {
    LOGCHECK(args.size() == 2, "ISA_RV32C_SUB expects 2 args");
    return std::make_shared<Rv32::ISA_RV32C_SUB>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32C_XOR" || name == "C.XOR") {
    LOGCHECK(args.size() == 2, "ISA_RV32C_XOR expects 2 args");
    return std::make_shared<Rv32::ISA_RV32C_XOR>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32C_OR" || name == "C.OR") {
    LOGCHECK(args.size() == 2, "ISA_RV32C_OR expects 2 args");
    return std::make_shared<Rv32::ISA_RV32C_OR>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32C_AND" || name == "C.AND") {
    LOGCHECK(args.size() == 2, "ISA_RV32C_AND expects 2 args");
    return std::make_shared<Rv32::ISA_RV32C_AND>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32C_J" || name == "C.J") {
    LOGCHECK(args.size() == 8, "ISA_RV32C_J expects 8 args");
    return std::make_shared<Rv32::ISA_RV32C_J>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]), static_cast<uint64_t>(args[5]), static_cast<uint64_t>(args[6]), static_cast<uint64_t>(args[7]));
  }
  if (name == "ISA_RV32C_BEQZ" || name == "C.BEQZ") {
    LOGCHECK(args.size() == 6, "ISA_RV32C_BEQZ expects 6 args");
    return std::make_shared<Rv32::ISA_RV32C_BEQZ>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]), static_cast<uint64_t>(args[5]));
  }
  if (name == "ISA_RV32C_BNEZ" || name == "C.BNEZ") {
    LOGCHECK(args.size() == 6, "ISA_RV32C_BNEZ expects 6 args");
    return std::make_shared<Rv32::ISA_RV32C_BNEZ>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]), static_cast<uint64_t>(args[5]));
  }
  if (name == "ISA_RV32C_ADDI4SPN" || name == "C.ADDI4SPN") {
    LOGCHECK(args.size() == 5, "ISA_RV32C_ADDI4SPN expects 5 args");
    return std::make_shared<Rv32::ISA_RV32C_ADDI4SPN>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32C_FLD" || name == "C.FLD") {
    LOGCHECK(args.size() == 4, "ISA_RV32C_FLD expects 4 args");
    return std::make_shared<Rv32::ISA_RV32C_FLD>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]));
  }
  if (name == "ISA_RV32C_LW" || name == "C.LW") {
    LOGCHECK(args.size() == 5, "ISA_RV32C_LW expects 5 args");
    return std::make_shared<Rv32::ISA_RV32C_LW>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32C_FLW" || name == "C.FLW") {
    LOGCHECK(args.size() == 5, "ISA_RV32C_FLW expects 5 args");
    return std::make_shared<Rv32::ISA_RV32C_FLW>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32C_FSD" || name == "C.FSD") {
    LOGCHECK(args.size() == 4, "ISA_RV32C_FSD expects 4 args");
    return std::make_shared<Rv32::ISA_RV32C_FSD>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]));
  }
  if (name == "ISA_RV32C_SW" || name == "C.SW") {
    LOGCHECK(args.size() == 5, "ISA_RV32C_SW expects 5 args");
    return std::make_shared<Rv32::ISA_RV32C_SW>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32C_FSW" || name == "C.FSW") {
    LOGCHECK(args.size() == 5, "ISA_RV32C_FSW expects 5 args");
    return std::make_shared<Rv32::ISA_RV32C_FSW>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32C_SLLI" || name == "C.SLLI") {
    LOGCHECK(args.size() == 3, "ISA_RV32C_SLLI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32C_SLLI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32C_FLDSP" || name == "C.FLDSP") {
    LOGCHECK(args.size() == 4, "ISA_RV32C_FLDSP expects 4 args");
    return std::make_shared<Rv32::ISA_RV32C_FLDSP>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]));
  }
  if (name == "ISA_RV32C_LWSP" || name == "C.LWSP") {
    LOGCHECK(args.size() == 4, "ISA_RV32C_LWSP expects 4 args");
    return std::make_shared<Rv32::ISA_RV32C_LWSP>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]));
  }
  if (name == "ISA_RV32C_FLWSP" || name == "C.FLWSP") {
    LOGCHECK(args.size() == 4, "ISA_RV32C_FLWSP expects 4 args");
    return std::make_shared<Rv32::ISA_RV32C_FLWSP>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]));
  }
  if (name == "ISA_RV32C_JR" || name == "C.JR") {
    LOGCHECK(args.size() == 1, "ISA_RV32C_JR expects 1 args");
    return std::make_shared<Rv32::ISA_RV32C_JR>(static_cast<uint64_t>(args[0]));
  }
  if (name == "ISA_RV32C_MV" || name == "C.MV") {
    LOGCHECK(args.size() == 2, "ISA_RV32C_MV expects 2 args");
    return std::make_shared<Rv32::ISA_RV32C_MV>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32C_EBREAK" || name == "C.EBREAK") {
    LOGCHECK(args.size() == 0, "ISA_RV32C_EBREAK expects 0 args");
    return std::make_shared<Rv32::ISA_RV32C_EBREAK>();
  }
  if (name == "ISA_RV32C_JALR" || name == "C.JALR") {
    LOGCHECK(args.size() == 1, "ISA_RV32C_JALR expects 1 args");
    return std::make_shared<Rv32::ISA_RV32C_JALR>(static_cast<uint64_t>(args[0]));
  }
  if (name == "ISA_RV32C_ADD" || name == "C.ADD") {
    LOGCHECK(args.size() == 2, "ISA_RV32C_ADD expects 2 args");
    return std::make_shared<Rv32::ISA_RV32C_ADD>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32C_FSDSP" || name == "C.FSDSP") {
    LOGCHECK(args.size() == 3, "ISA_RV32C_FSDSP expects 3 args");
    return std::make_shared<Rv32::ISA_RV32C_FSDSP>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32C_SWSP" || name == "C.SWSP") {
    LOGCHECK(args.size() == 3, "ISA_RV32C_SWSP expects 3 args");
    return std::make_shared<Rv32::ISA_RV32C_SWSP>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32C_FSWSP" || name == "C.FSWSP") {
    LOGCHECK(args.size() == 3, "ISA_RV32C_FSWSP expects 3 args");
    return std::make_shared<Rv32::ISA_RV32C_FSWSP>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32I_SRET" || name == "SRET") {
    LOGCHECK(args.size() == 0, "ISA_RV32I_SRET expects 0 args");
    return std::make_shared<Rv32::ISA_RV32I_SRET>();
  }
  if (name == "ISA_RV32I_MRET" || name == "MRET") {
    LOGCHECK(args.size() == 0, "ISA_RV32I_MRET expects 0 args");
    return std::make_shared<Rv32::ISA_RV32I_MRET>();
  }
  if (name == "ISA_RV32A_LR_W" || name == "LR.W") {
    LOGCHECK(args.size() == 4, "ISA_RV32A_LR_W expects 4 args");
    return std::make_shared<Rv32::ISA_RV32A_LR_W>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]));
  }
  if (name == "ISA_RV32A_SC_W" || name == "SC.W") {
    LOGCHECK(args.size() == 5, "ISA_RV32A_SC_W expects 5 args");
    return std::make_shared<Rv32::ISA_RV32A_SC_W>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32A_AMOSWAP_W" || name == "AMOSWAP.W") {
    LOGCHECK(args.size() == 5, "ISA_RV32A_AMOSWAP_W expects 5 args");
    return std::make_shared<Rv32::ISA_RV32A_AMOSWAP_W>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32A_AMOADD_W" || name == "AMOADD.W") {
    LOGCHECK(args.size() == 5, "ISA_RV32A_AMOADD_W expects 5 args");
    return std::make_shared<Rv32::ISA_RV32A_AMOADD_W>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32A_AMOXOR_W" || name == "AMOXOR.W") {
    LOGCHECK(args.size() == 5, "ISA_RV32A_AMOXOR_W expects 5 args");
    return std::make_shared<Rv32::ISA_RV32A_AMOXOR_W>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32A_AMOAND_W" || name == "AMOAND.W") {
    LOGCHECK(args.size() == 5, "ISA_RV32A_AMOAND_W expects 5 args");
    return std::make_shared<Rv32::ISA_RV32A_AMOAND_W>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32A_AMOOR_W" || name == "AMOOR.W") {
    LOGCHECK(args.size() == 5, "ISA_RV32A_AMOOR_W expects 5 args");
    return std::make_shared<Rv32::ISA_RV32A_AMOOR_W>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32A_AMOMIN_W" || name == "AMOMIN.W") {
    LOGCHECK(args.size() == 5, "ISA_RV32A_AMOMIN_W expects 5 args");
    return std::make_shared<Rv32::ISA_RV32A_AMOMIN_W>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32A_AMOMAX_W" || name == "AMOMAX.W") {
    LOGCHECK(args.size() == 5, "ISA_RV32A_AMOMAX_W expects 5 args");
    return std::make_shared<Rv32::ISA_RV32A_AMOMAX_W>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32A_AMOMINU_W" || name == "AMOMINU.W") {
    LOGCHECK(args.size() == 5, "ISA_RV32A_AMOMINU_W expects 5 args");
    return std::make_shared<Rv32::ISA_RV32A_AMOMINU_W>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32A_AMOMAXU_W" || name == "AMOMAXU.W") {
    LOGCHECK(args.size() == 5, "ISA_RV32A_AMOMAXU_W expects 5 args");
    return std::make_shared<Rv32::ISA_RV32A_AMOMAXU_W>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]), static_cast<uint64_t>(args[3]), static_cast<uint64_t>(args[4]));
  }
  if (name == "ISA_RV32Zbb_CLZ" || name == "CLZ") {
    LOGCHECK(args.size() == 2, "ISA_RV32Zbb_CLZ expects 2 args");
    return std::make_shared<Rv32::ISA_RV32Zbb_CLZ>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32Zbb_CTZ" || name == "CTZ") {
    LOGCHECK(args.size() == 2, "ISA_RV32Zbb_CTZ expects 2 args");
    return std::make_shared<Rv32::ISA_RV32Zbb_CTZ>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32Zbb_CPOP" || name == "CPOP") {
    LOGCHECK(args.size() == 2, "ISA_RV32Zbb_CPOP expects 2 args");
    return std::make_shared<Rv32::ISA_RV32Zbb_CPOP>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32Zbb_MIN" || name == "MIN") {
    LOGCHECK(args.size() == 3, "ISA_RV32Zbb_MIN expects 3 args");
    return std::make_shared<Rv32::ISA_RV32Zbb_MIN>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32Zbb_MAX" || name == "MAX") {
    LOGCHECK(args.size() == 3, "ISA_RV32Zbb_MAX expects 3 args");
    return std::make_shared<Rv32::ISA_RV32Zbb_MAX>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32Zbb_MINU" || name == "MINU") {
    LOGCHECK(args.size() == 3, "ISA_RV32Zbb_MINU expects 3 args");
    return std::make_shared<Rv32::ISA_RV32Zbb_MINU>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32Zbb_MAXU" || name == "MAXU") {
    LOGCHECK(args.size() == 3, "ISA_RV32Zbb_MAXU expects 3 args");
    return std::make_shared<Rv32::ISA_RV32Zbb_MAXU>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32Zbb_SEXT_B" || name == "SEXT.B") {
    LOGCHECK(args.size() == 2, "ISA_RV32Zbb_SEXT_B expects 2 args");
    return std::make_shared<Rv32::ISA_RV32Zbb_SEXT_B>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32Zbb_SEXT_H" || name == "SEXT.H") {
    LOGCHECK(args.size() == 2, "ISA_RV32Zbb_SEXT_H expects 2 args");
    return std::make_shared<Rv32::ISA_RV32Zbb_SEXT_H>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32Zbb_ZEXT_H" || name == "ZEXT.H") {
    LOGCHECK(args.size() == 2, "ISA_RV32Zbb_ZEXT_H expects 2 args");
    return std::make_shared<Rv32::ISA_RV32Zbb_ZEXT_H>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]));
  }
  if (name == "ISA_RV32Zbb_ANDN" || name == "ANDN") {
    LOGCHECK(args.size() == 3, "ISA_RV32Zbb_ANDN expects 3 args");
    return std::make_shared<Rv32::ISA_RV32Zbb_ANDN>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32Zbb_ORN" || name == "ORN") {
    LOGCHECK(args.size() == 3, "ISA_RV32Zbb_ORN expects 3 args");
    return std::make_shared<Rv32::ISA_RV32Zbb_ORN>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32Zbb_XNOR" || name == "XNOR") {
    LOGCHECK(args.size() == 3, "ISA_RV32Zbb_XNOR expects 3 args");
    return std::make_shared<Rv32::ISA_RV32Zbb_XNOR>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32Zbb_ROL" || name == "ROL") {
    LOGCHECK(args.size() == 3, "ISA_RV32Zbb_ROL expects 3 args");
    return std::make_shared<Rv32::ISA_RV32Zbb_ROL>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32Zbb_ROR" || name == "ROR") {
    LOGCHECK(args.size() == 3, "ISA_RV32Zbb_ROR expects 3 args");
    return std::make_shared<Rv32::ISA_RV32Zbb_ROR>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  if (name == "ISA_RV32Zbb_RORI" || name == "RORI") {
    LOGCHECK(args.size() == 3, "ISA_RV32Zbb_RORI expects 3 args");
    return std::make_shared<Rv32::ISA_RV32Zbb_RORI>(static_cast<uint64_t>(args[0]), static_cast<uint64_t>(args[1]), static_cast<uint64_t>(args[2]));
  }
  spdlog::error("MakeInst: unknown instruction '{}'", name);
  LOGCHECK(false, "unknown instruction");
  return nullptr;
}
