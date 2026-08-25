/**
**/
//
// 由 ISA DSL 工具生成。
//
// clang-format off
#pragma GCC diagnostic ignored "-Wunused-parameter"

enum class InstructionEnum {
    ISA_RV32I_ADD = 0,
    ISA_RV32I_SUB = 1,
    ISA_RV32I_SLL = 2,
    ISA_RV32I_SLT = 3,
    ISA_RV32I_SLTU = 4,
    ISA_RV32I_XOR = 5,
    ISA_RV32I_SRL = 6,
    ISA_RV32I_SRA = 7,
    ISA_RV32I_OR = 8,
    ISA_RV32I_AND = 9,
    ISA_RV32I_ADDI = 10,
    ISA_RV32I_SLTI = 11,
    ISA_RV32I_SLTIU = 12,
    ISA_RV32I_XORI = 13,
    ISA_RV32I_ORI = 14,
    ISA_RV32I_ANDI = 15,
    ISA_RV32I_SLLI = 16,
    ISA_RV32I_SRLI = 17,
    ISA_RV32I_SRAI = 18,
    ISA_RV32I_LB = 19,
    ISA_RV32I_LH = 20,
    ISA_RV32I_LW = 21,
    ISA_RV32I_LBU = 22,
    ISA_RV32I_LHU = 23,
    ISA_RV32I_SB = 24,
    ISA_RV32I_SH = 25,
    ISA_RV32I_SW = 26,
    ISA_RV32I_BEQ = 27,
    ISA_RV32I_BNE = 28,
    ISA_RV32I_BLT = 29,
    ISA_RV32I_BGE = 30,
    ISA_RV32I_BLTU = 31,
    ISA_RV32I_BGEU = 32,
    ISA_RV32I_LUI = 33,
    ISA_RV32I_AUIPC = 34,
    ISA_RV32I_JAL = 35,
    ISA_RV32I_JALR = 36,
    ISA_RV32I_FENCE = 37,
    ISA_RV32I_FENCE_I = 38,
    ISA_RV32I_ECALL = 39,
    ISA_RV32I_EBREAK = 40,
    ISA_RV32I_CSRRW = 41,
    ISA_RV32I_CSRRS = 42,
    ISA_RV32I_CSRRC = 43,
    ISA_RV32I_CSRRWI = 44,
    ISA_RV32I_CSRRSI = 45,
    ISA_RV32I_CSRRCI = 46,
    ISA_RV32I_MUL = 47,
    ISA_RV32I_MULH = 48,
    ISA_RV32I_MULHSU = 49,
    ISA_RV32I_MULHU = 50,
    ISA_RV32I_DIV = 51,
    ISA_RV32I_DIVU = 52,
    ISA_RV32I_REM = 53,
    ISA_RV32I_REMU = 54,
    ISA_RV32C_NOP = 55,
    ISA_RV32C_ADDI = 56,
    ISA_RV32C_JAL = 57,
    ISA_RV32C_LI = 58,
    ISA_RV32C_ADDI16SP = 59,
    ISA_RV32C_LUI = 60,
    ISA_RV32C_SRLI = 61,
    ISA_RV32C_SRAI = 62,
    ISA_RV32C_ANDI = 63,
    ISA_RV32C_SUB = 64,
    ISA_RV32C_XOR = 65,
    ISA_RV32C_OR = 66,
    ISA_RV32C_AND = 67,
    ISA_RV32C_J = 68,
    ISA_RV32C_BEQZ = 69,
    ISA_RV32C_BNEZ = 70,
    ISA_RV32C_ADDI4SPN = 71,
    ISA_RV32C_FLD = 72,
    ISA_RV32C_LW = 73,
    ISA_RV32C_FLW = 74,
    ISA_RV32C_FSD = 75,
    ISA_RV32C_SW = 76,
    ISA_RV32C_FSW = 77,
    ISA_RV32C_SLLI = 78,
    ISA_RV32C_FLDSP = 79,
    ISA_RV32C_LWSP = 80,
    ISA_RV32C_FLWSP = 81,
    ISA_RV32C_JR = 82,
    ISA_RV32C_MV = 83,
    ISA_RV32C_EBREAK = 84,
    ISA_RV32C_JALR = 85,
    ISA_RV32C_ADD = 86,
    ISA_RV32C_FSDSP = 87,
    ISA_RV32C_SWSP = 88,
    ISA_RV32C_FSWSP = 89,
    ISA_RV32I_SRET = 90,
    ISA_RV32I_MRET = 91,
    ISA_RV32A_LR_W = 92,
    ISA_RV32A_SC_W = 93,
    ISA_RV32A_AMOSWAP_W = 94,
    ISA_RV32A_AMOADD_W = 95,
    ISA_RV32A_AMOXOR_W = 96,
    ISA_RV32A_AMOAND_W = 97,
    ISA_RV32A_AMOOR_W = 98,
    ISA_RV32A_AMOMIN_W = 99,
    ISA_RV32A_AMOMAX_W = 100,
    ISA_RV32A_AMOMINU_W = 101,
    ISA_RV32A_AMOMAXU_W = 102,
    ISA_RV32Zbb_CLZ = 103,
    ISA_RV32Zbb_CTZ = 104,
    ISA_RV32Zbb_CPOP = 105,
    ISA_RV32Zbb_MIN = 106,
    ISA_RV32Zbb_MAX = 107,
    ISA_RV32Zbb_MINU = 108,
    ISA_RV32Zbb_MAXU = 109,
    ISA_RV32Zbb_SEXT_B = 110,
    ISA_RV32Zbb_SEXT_H = 111,
    ISA_RV32Zbb_ZEXT_H = 112,
    ISA_RV32Zbb_ANDN = 113,
    ISA_RV32Zbb_ORN = 114,
    ISA_RV32Zbb_XNOR = 115,
    ISA_RV32Zbb_ROL = 116,
    ISA_RV32Zbb_ROR = 117,
    ISA_RV32Zbb_RORI = 118,
ENUM_END
};


enum class FIELD {
    _OPCODE = 0,
    _RD = 1,
    _FUNCT3 = 2,
    _RS1 = 3,
    _RS2 = 4,
    _FUNCT7 = 5,
    _IMM12 = 6,
    _IMM5 = 7,
    _CONST0 = 8,
    _CONST32 = 9,
    _IMM7 = 10,
    _IMM1 = 11,
    _IMM4 = 12,
    _IMM6 = 13,
    _IMM1_U0 = 14,
    _IMM20 = 15,
    _IMM8 = 16,
    _IMM10 = 17,
    _SUCC = 18,
    _PRED = 19,
    _FUNCT12 = 20,
    _CSR = 21,
    _FUNCT5 = 22,
    _IMM1_5 = 23,
    _IMM3_3_1 = 24,
    _IMM1_7 = 25,
    _IMM1_6 = 26,
    _IMM1_10 = 27,
    _IMM2_9_8 = 28,
    _IMM1_4 = 29,
    _IMM1_11 = 30,
    _IMM2_7_8 = 31,
    _IMM1_9 = 32,
    _FUNCT2 = 33,
    _FUNCT2_1 = 34,
    _FUNCT1 = 35,
    _IMM2_2_1 = 36,
    _IMM2_6_7 = 37,
    _IMM2_3_4 = 38,
    _IMM1_8 = 39,
    _IMM1_3 = 40,
    _IMM1_2 = 41,
    _IMM4_9_6 = 42,
    _IMM2_5_4 = 43,
    _IMM2 = 44,
    _IMM3 = 45,
    _IMM3_3_5 = 46,
    _IMM2_7_6 = 47,
    _IMM3_4_2 = 48,
    _FUNCT5_1 = 49,
    _IMM3_8_6 = 50,
    _IMM3_5_3 = 51,
    _IMM4_5_2 = 52,
    _OPCODE1 = 53,
    _OPCODE2 = 54,
    _OPCODE3 = 55,
    _OPCODE4 = 56,
    _OPCODE5 = 57,
    _OPCODE6 = 58,
    _RL = 59,
    _AQ = 60,
    _FUNCT5_2 = 61,
    _IMM = 62,
ENUM_END
};


static std::string GetInstructionUName(InstructionEnum eid) {
  switch(eid) {
    case InstructionEnum::ISA_RV32I_ADD: return "ISA_RV32I_ADD";
    case InstructionEnum::ISA_RV32I_SUB: return "ISA_RV32I_SUB";
    case InstructionEnum::ISA_RV32I_SLL: return "ISA_RV32I_SLL";
    case InstructionEnum::ISA_RV32I_SLT: return "ISA_RV32I_SLT";
    case InstructionEnum::ISA_RV32I_SLTU: return "ISA_RV32I_SLTU";
    case InstructionEnum::ISA_RV32I_XOR: return "ISA_RV32I_XOR";
    case InstructionEnum::ISA_RV32I_SRL: return "ISA_RV32I_SRL";
    case InstructionEnum::ISA_RV32I_SRA: return "ISA_RV32I_SRA";
    case InstructionEnum::ISA_RV32I_OR: return "ISA_RV32I_OR";
    case InstructionEnum::ISA_RV32I_AND: return "ISA_RV32I_AND";
    case InstructionEnum::ISA_RV32I_ADDI: return "ISA_RV32I_ADDI";
    case InstructionEnum::ISA_RV32I_SLTI: return "ISA_RV32I_SLTI";
    case InstructionEnum::ISA_RV32I_SLTIU: return "ISA_RV32I_SLTIU";
    case InstructionEnum::ISA_RV32I_XORI: return "ISA_RV32I_XORI";
    case InstructionEnum::ISA_RV32I_ORI: return "ISA_RV32I_ORI";
    case InstructionEnum::ISA_RV32I_ANDI: return "ISA_RV32I_ANDI";
    case InstructionEnum::ISA_RV32I_SLLI: return "ISA_RV32I_SLLI";
    case InstructionEnum::ISA_RV32I_SRLI: return "ISA_RV32I_SRLI";
    case InstructionEnum::ISA_RV32I_SRAI: return "ISA_RV32I_SRAI";
    case InstructionEnum::ISA_RV32I_LB: return "ISA_RV32I_LB";
    case InstructionEnum::ISA_RV32I_LH: return "ISA_RV32I_LH";
    case InstructionEnum::ISA_RV32I_LW: return "ISA_RV32I_LW";
    case InstructionEnum::ISA_RV32I_LBU: return "ISA_RV32I_LBU";
    case InstructionEnum::ISA_RV32I_LHU: return "ISA_RV32I_LHU";
    case InstructionEnum::ISA_RV32I_SB: return "ISA_RV32I_SB";
    case InstructionEnum::ISA_RV32I_SH: return "ISA_RV32I_SH";
    case InstructionEnum::ISA_RV32I_SW: return "ISA_RV32I_SW";
    case InstructionEnum::ISA_RV32I_BEQ: return "ISA_RV32I_BEQ";
    case InstructionEnum::ISA_RV32I_BNE: return "ISA_RV32I_BNE";
    case InstructionEnum::ISA_RV32I_BLT: return "ISA_RV32I_BLT";
    case InstructionEnum::ISA_RV32I_BGE: return "ISA_RV32I_BGE";
    case InstructionEnum::ISA_RV32I_BLTU: return "ISA_RV32I_BLTU";
    case InstructionEnum::ISA_RV32I_BGEU: return "ISA_RV32I_BGEU";
    case InstructionEnum::ISA_RV32I_LUI: return "ISA_RV32I_LUI";
    case InstructionEnum::ISA_RV32I_AUIPC: return "ISA_RV32I_AUIPC";
    case InstructionEnum::ISA_RV32I_JAL: return "ISA_RV32I_JAL";
    case InstructionEnum::ISA_RV32I_JALR: return "ISA_RV32I_JALR";
    case InstructionEnum::ISA_RV32I_FENCE: return "ISA_RV32I_FENCE";
    case InstructionEnum::ISA_RV32I_FENCE_I: return "ISA_RV32I_FENCE_I";
    case InstructionEnum::ISA_RV32I_ECALL: return "ISA_RV32I_ECALL";
    case InstructionEnum::ISA_RV32I_EBREAK: return "ISA_RV32I_EBREAK";
    case InstructionEnum::ISA_RV32I_CSRRW: return "ISA_RV32I_CSRRW";
    case InstructionEnum::ISA_RV32I_CSRRS: return "ISA_RV32I_CSRRS";
    case InstructionEnum::ISA_RV32I_CSRRC: return "ISA_RV32I_CSRRC";
    case InstructionEnum::ISA_RV32I_CSRRWI: return "ISA_RV32I_CSRRWI";
    case InstructionEnum::ISA_RV32I_CSRRSI: return "ISA_RV32I_CSRRSI";
    case InstructionEnum::ISA_RV32I_CSRRCI: return "ISA_RV32I_CSRRCI";
    case InstructionEnum::ISA_RV32I_MUL: return "ISA_RV32I_MUL";
    case InstructionEnum::ISA_RV32I_MULH: return "ISA_RV32I_MULH";
    case InstructionEnum::ISA_RV32I_MULHSU: return "ISA_RV32I_MULHSU";
    case InstructionEnum::ISA_RV32I_MULHU: return "ISA_RV32I_MULHU";
    case InstructionEnum::ISA_RV32I_DIV: return "ISA_RV32I_DIV";
    case InstructionEnum::ISA_RV32I_DIVU: return "ISA_RV32I_DIVU";
    case InstructionEnum::ISA_RV32I_REM: return "ISA_RV32I_REM";
    case InstructionEnum::ISA_RV32I_REMU: return "ISA_RV32I_REMU";
    case InstructionEnum::ISA_RV32C_NOP: return "ISA_RV32C_NOP";
    case InstructionEnum::ISA_RV32C_ADDI: return "ISA_RV32C_ADDI";
    case InstructionEnum::ISA_RV32C_JAL: return "ISA_RV32C_JAL";
    case InstructionEnum::ISA_RV32C_LI: return "ISA_RV32C_LI";
    case InstructionEnum::ISA_RV32C_ADDI16SP: return "ISA_RV32C_ADDI16SP";
    case InstructionEnum::ISA_RV32C_LUI: return "ISA_RV32C_LUI";
    case InstructionEnum::ISA_RV32C_SRLI: return "ISA_RV32C_SRLI";
    case InstructionEnum::ISA_RV32C_SRAI: return "ISA_RV32C_SRAI";
    case InstructionEnum::ISA_RV32C_ANDI: return "ISA_RV32C_ANDI";
    case InstructionEnum::ISA_RV32C_SUB: return "ISA_RV32C_SUB";
    case InstructionEnum::ISA_RV32C_XOR: return "ISA_RV32C_XOR";
    case InstructionEnum::ISA_RV32C_OR: return "ISA_RV32C_OR";
    case InstructionEnum::ISA_RV32C_AND: return "ISA_RV32C_AND";
    case InstructionEnum::ISA_RV32C_J: return "ISA_RV32C_J";
    case InstructionEnum::ISA_RV32C_BEQZ: return "ISA_RV32C_BEQZ";
    case InstructionEnum::ISA_RV32C_BNEZ: return "ISA_RV32C_BNEZ";
    case InstructionEnum::ISA_RV32C_ADDI4SPN: return "ISA_RV32C_ADDI4SPN";
    case InstructionEnum::ISA_RV32C_FLD: return "ISA_RV32C_FLD";
    case InstructionEnum::ISA_RV32C_LW: return "ISA_RV32C_LW";
    case InstructionEnum::ISA_RV32C_FLW: return "ISA_RV32C_FLW";
    case InstructionEnum::ISA_RV32C_FSD: return "ISA_RV32C_FSD";
    case InstructionEnum::ISA_RV32C_SW: return "ISA_RV32C_SW";
    case InstructionEnum::ISA_RV32C_FSW: return "ISA_RV32C_FSW";
    case InstructionEnum::ISA_RV32C_SLLI: return "ISA_RV32C_SLLI";
    case InstructionEnum::ISA_RV32C_FLDSP: return "ISA_RV32C_FLDSP";
    case InstructionEnum::ISA_RV32C_LWSP: return "ISA_RV32C_LWSP";
    case InstructionEnum::ISA_RV32C_FLWSP: return "ISA_RV32C_FLWSP";
    case InstructionEnum::ISA_RV32C_JR: return "ISA_RV32C_JR";
    case InstructionEnum::ISA_RV32C_MV: return "ISA_RV32C_MV";
    case InstructionEnum::ISA_RV32C_EBREAK: return "ISA_RV32C_EBREAK";
    case InstructionEnum::ISA_RV32C_JALR: return "ISA_RV32C_JALR";
    case InstructionEnum::ISA_RV32C_ADD: return "ISA_RV32C_ADD";
    case InstructionEnum::ISA_RV32C_FSDSP: return "ISA_RV32C_FSDSP";
    case InstructionEnum::ISA_RV32C_SWSP: return "ISA_RV32C_SWSP";
    case InstructionEnum::ISA_RV32C_FSWSP: return "ISA_RV32C_FSWSP";
    case InstructionEnum::ISA_RV32I_SRET: return "ISA_RV32I_SRET";
    case InstructionEnum::ISA_RV32I_MRET: return "ISA_RV32I_MRET";
    case InstructionEnum::ISA_RV32A_LR_W: return "ISA_RV32A_LR_W";
    case InstructionEnum::ISA_RV32A_SC_W: return "ISA_RV32A_SC_W";
    case InstructionEnum::ISA_RV32A_AMOSWAP_W: return "ISA_RV32A_AMOSWAP_W";
    case InstructionEnum::ISA_RV32A_AMOADD_W: return "ISA_RV32A_AMOADD_W";
    case InstructionEnum::ISA_RV32A_AMOXOR_W: return "ISA_RV32A_AMOXOR_W";
    case InstructionEnum::ISA_RV32A_AMOAND_W: return "ISA_RV32A_AMOAND_W";
    case InstructionEnum::ISA_RV32A_AMOOR_W: return "ISA_RV32A_AMOOR_W";
    case InstructionEnum::ISA_RV32A_AMOMIN_W: return "ISA_RV32A_AMOMIN_W";
    case InstructionEnum::ISA_RV32A_AMOMAX_W: return "ISA_RV32A_AMOMAX_W";
    case InstructionEnum::ISA_RV32A_AMOMINU_W: return "ISA_RV32A_AMOMINU_W";
    case InstructionEnum::ISA_RV32A_AMOMAXU_W: return "ISA_RV32A_AMOMAXU_W";
    case InstructionEnum::ISA_RV32Zbb_CLZ: return "ISA_RV32Zbb_CLZ";
    case InstructionEnum::ISA_RV32Zbb_CTZ: return "ISA_RV32Zbb_CTZ";
    case InstructionEnum::ISA_RV32Zbb_CPOP: return "ISA_RV32Zbb_CPOP";
    case InstructionEnum::ISA_RV32Zbb_MIN: return "ISA_RV32Zbb_MIN";
    case InstructionEnum::ISA_RV32Zbb_MAX: return "ISA_RV32Zbb_MAX";
    case InstructionEnum::ISA_RV32Zbb_MINU: return "ISA_RV32Zbb_MINU";
    case InstructionEnum::ISA_RV32Zbb_MAXU: return "ISA_RV32Zbb_MAXU";
    case InstructionEnum::ISA_RV32Zbb_SEXT_B: return "ISA_RV32Zbb_SEXT_B";
    case InstructionEnum::ISA_RV32Zbb_SEXT_H: return "ISA_RV32Zbb_SEXT_H";
    case InstructionEnum::ISA_RV32Zbb_ZEXT_H: return "ISA_RV32Zbb_ZEXT_H";
    case InstructionEnum::ISA_RV32Zbb_ANDN: return "ISA_RV32Zbb_ANDN";
    case InstructionEnum::ISA_RV32Zbb_ORN: return "ISA_RV32Zbb_ORN";
    case InstructionEnum::ISA_RV32Zbb_XNOR: return "ISA_RV32Zbb_XNOR";
    case InstructionEnum::ISA_RV32Zbb_ROL: return "ISA_RV32Zbb_ROL";
    case InstructionEnum::ISA_RV32Zbb_ROR: return "ISA_RV32Zbb_ROR";
    case InstructionEnum::ISA_RV32Zbb_RORI: return "ISA_RV32Zbb_RORI";
    case InstructionEnum::ENUM_END: return "ENUM_END";
   default: LOGCHECK(0, "");
  }
};


