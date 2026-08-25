/**
**/
//
// 由 ISA DSL 工具生成。
//
// clang-format off
#pragma GCC diagnostic ignored "-Wunused-parameter"

#define START_INST_CODE_DEFINE

#include "isa/isa_unittest.h"
#include "rv32/rv32.h"

using namespace latch;
using namespace latch::rv32;

// TEST_DEFINE 里 PC 读架构状态（仅 system 在作用域）；覆盖 instance 头的默认，使测试看到执行后的 System 值。
#undef PC
#define PC      system->GetRegFile(REG_SPR)[SPR_PC]

using ISAUnittest = ::latch::ISAUnittestT<Rv32, SystemRv32>;

// RUN_INST(name, ...)：构造并执行一条本架构指令（指令类前缀 = class_prefix，故 ISA 特有）。
// System 不对外 run 指令——经夹具 RunOne 由 Engine 驱动跑完这一条（isa/isa_unittest.h）。
#define RUN_INST(name, ...) \
  RunOne(std::make_shared<Rv32::name>(__VA_ARGS__));

TEST_F(ISAUnittest, ISA_RV32I_ADD) {
    #define TEST_DEFINE
    #include "ISA_RV32I_ADD.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_SUB) {
    #define TEST_DEFINE
    #include "ISA_RV32I_SUB.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_SLL) {
    #define TEST_DEFINE
    #include "ISA_RV32I_SLL.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_SLT) {
    #define TEST_DEFINE
    #include "ISA_RV32I_SLT.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_SLTU) {
    #define TEST_DEFINE
    #include "ISA_RV32I_SLTU.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_XOR) {
    #define TEST_DEFINE
    #include "ISA_RV32I_XOR.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_SRL) {
    #define TEST_DEFINE
    #include "ISA_RV32I_SRL.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_SRA) {
    #define TEST_DEFINE
    #include "ISA_RV32I_SRA.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_OR) {
    #define TEST_DEFINE
    #include "ISA_RV32I_OR.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_AND) {
    #define TEST_DEFINE
    #include "ISA_RV32I_AND.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_ADDI) {
    #define TEST_DEFINE
    #include "ISA_RV32I_ADDI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_SLTI) {
    #define TEST_DEFINE
    #include "ISA_RV32I_SLTI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_SLTIU) {
    #define TEST_DEFINE
    #include "ISA_RV32I_SLTIU.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_XORI) {
    #define TEST_DEFINE
    #include "ISA_RV32I_XORI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_ORI) {
    #define TEST_DEFINE
    #include "ISA_RV32I_ORI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_ANDI) {
    #define TEST_DEFINE
    #include "ISA_RV32I_ANDI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_SLLI) {
    #define TEST_DEFINE
    #include "ISA_RV32I_SLLI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_SRLI) {
    #define TEST_DEFINE
    #include "ISA_RV32I_SRLI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_SRAI) {
    #define TEST_DEFINE
    #include "ISA_RV32I_SRAI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_LB) {
    #define TEST_DEFINE
    #include "ISA_RV32I_LB.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_LH) {
    #define TEST_DEFINE
    #include "ISA_RV32I_LH.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_LW) {
    #define TEST_DEFINE
    #include "ISA_RV32I_LW.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_LBU) {
    #define TEST_DEFINE
    #include "ISA_RV32I_LBU.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_LHU) {
    #define TEST_DEFINE
    #include "ISA_RV32I_LHU.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_SB) {
    #define TEST_DEFINE
    #include "ISA_RV32I_SB.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_SH) {
    #define TEST_DEFINE
    #include "ISA_RV32I_SH.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_SW) {
    #define TEST_DEFINE
    #include "ISA_RV32I_SW.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_BEQ) {
    #define TEST_DEFINE
    #include "ISA_RV32I_BEQ.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_BNE) {
    #define TEST_DEFINE
    #include "ISA_RV32I_BNE.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_BLT) {
    #define TEST_DEFINE
    #include "ISA_RV32I_BLT.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_BGE) {
    #define TEST_DEFINE
    #include "ISA_RV32I_BGE.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_BLTU) {
    #define TEST_DEFINE
    #include "ISA_RV32I_BLTU.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_BGEU) {
    #define TEST_DEFINE
    #include "ISA_RV32I_BGEU.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_LUI) {
    #define TEST_DEFINE
    #include "ISA_RV32I_LUI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_AUIPC) {
    #define TEST_DEFINE
    #include "ISA_RV32I_AUIPC.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_JAL) {
    #define TEST_DEFINE
    #include "ISA_RV32I_JAL.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_JALR) {
    #define TEST_DEFINE
    #include "ISA_RV32I_JALR.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_FENCE) {
    #define TEST_DEFINE
    #include "ISA_RV32I_FENCE.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_FENCE_I) {
    #define TEST_DEFINE
    #include "ISA_RV32I_FENCE_I.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_ECALL) {
    #define TEST_DEFINE
    #include "ISA_RV32I_ECALL.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_EBREAK) {
    #define TEST_DEFINE
    #include "ISA_RV32I_EBREAK.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_CSRRW) {
    #define TEST_DEFINE
    #include "ISA_RV32I_CSRRW.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_CSRRS) {
    #define TEST_DEFINE
    #include "ISA_RV32I_CSRRS.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_CSRRC) {
    #define TEST_DEFINE
    #include "ISA_RV32I_CSRRC.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_CSRRWI) {
    #define TEST_DEFINE
    #include "ISA_RV32I_CSRRWI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_CSRRSI) {
    #define TEST_DEFINE
    #include "ISA_RV32I_CSRRSI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_CSRRCI) {
    #define TEST_DEFINE
    #include "ISA_RV32I_CSRRCI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_MUL) {
    #define TEST_DEFINE
    #include "ISA_RV32I_MUL.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_MULH) {
    #define TEST_DEFINE
    #include "ISA_RV32I_MULH.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_MULHSU) {
    #define TEST_DEFINE
    #include "ISA_RV32I_MULHSU.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_MULHU) {
    #define TEST_DEFINE
    #include "ISA_RV32I_MULHU.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_DIV) {
    #define TEST_DEFINE
    #include "ISA_RV32I_DIV.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_DIVU) {
    #define TEST_DEFINE
    #include "ISA_RV32I_DIVU.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_REM) {
    #define TEST_DEFINE
    #include "ISA_RV32I_REM.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_REMU) {
    #define TEST_DEFINE
    #include "ISA_RV32I_REMU.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_NOP) {
    #define TEST_DEFINE
    #include "ISA_RV32C_NOP.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_ADDI) {
    #define TEST_DEFINE
    #include "ISA_RV32C_ADDI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_JAL) {
    #define TEST_DEFINE
    #include "ISA_RV32C_JAL.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_LI) {
    #define TEST_DEFINE
    #include "ISA_RV32C_LI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_ADDI16SP) {
    #define TEST_DEFINE
    #include "ISA_RV32C_ADDI16SP.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_LUI) {
    #define TEST_DEFINE
    #include "ISA_RV32C_LUI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_SRLI) {
    #define TEST_DEFINE
    #include "ISA_RV32C_SRLI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_SRAI) {
    #define TEST_DEFINE
    #include "ISA_RV32C_SRAI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_ANDI) {
    #define TEST_DEFINE
    #include "ISA_RV32C_ANDI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_SUB) {
    #define TEST_DEFINE
    #include "ISA_RV32C_SUB.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_XOR) {
    #define TEST_DEFINE
    #include "ISA_RV32C_XOR.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_OR) {
    #define TEST_DEFINE
    #include "ISA_RV32C_OR.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_AND) {
    #define TEST_DEFINE
    #include "ISA_RV32C_AND.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_J) {
    #define TEST_DEFINE
    #include "ISA_RV32C_J.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_BEQZ) {
    #define TEST_DEFINE
    #include "ISA_RV32C_BEQZ.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_BNEZ) {
    #define TEST_DEFINE
    #include "ISA_RV32C_BNEZ.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_ADDI4SPN) {
    #define TEST_DEFINE
    #include "ISA_RV32C_ADDI4SPN.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_FLD) {
    #define TEST_DEFINE
    #include "ISA_RV32C_FLD.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_LW) {
    #define TEST_DEFINE
    #include "ISA_RV32C_LW.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_FLW) {
    #define TEST_DEFINE
    #include "ISA_RV32C_FLW.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_FSD) {
    #define TEST_DEFINE
    #include "ISA_RV32C_FSD.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_SW) {
    #define TEST_DEFINE
    #include "ISA_RV32C_SW.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_FSW) {
    #define TEST_DEFINE
    #include "ISA_RV32C_FSW.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_SLLI) {
    #define TEST_DEFINE
    #include "ISA_RV32C_SLLI.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_FLDSP) {
    #define TEST_DEFINE
    #include "ISA_RV32C_FLDSP.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_LWSP) {
    #define TEST_DEFINE
    #include "ISA_RV32C_LWSP.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_FLWSP) {
    #define TEST_DEFINE
    #include "ISA_RV32C_FLWSP.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_JR) {
    #define TEST_DEFINE
    #include "ISA_RV32C_JR.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_MV) {
    #define TEST_DEFINE
    #include "ISA_RV32C_MV.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_EBREAK) {
    #define TEST_DEFINE
    #include "ISA_RV32C_EBREAK.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_JALR) {
    #define TEST_DEFINE
    #include "ISA_RV32C_JALR.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_ADD) {
    #define TEST_DEFINE
    #include "ISA_RV32C_ADD.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_FSDSP) {
    #define TEST_DEFINE
    #include "ISA_RV32C_FSDSP.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_SWSP) {
    #define TEST_DEFINE
    #include "ISA_RV32C_SWSP.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32C_FSWSP) {
    #define TEST_DEFINE
    #include "ISA_RV32C_FSWSP.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_SRET) {
    #define TEST_DEFINE
    #include "ISA_RV32I_SRET.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32I_MRET) {
    #define TEST_DEFINE
    #include "ISA_RV32I_MRET.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32A_LR_W) {
    #define TEST_DEFINE
    #include "ISA_RV32A_LR_W.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32A_SC_W) {
    #define TEST_DEFINE
    #include "ISA_RV32A_SC_W.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32A_AMOSWAP_W) {
    #define TEST_DEFINE
    #include "ISA_RV32A_AMOSWAP_W.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32A_AMOADD_W) {
    #define TEST_DEFINE
    #include "ISA_RV32A_AMOADD_W.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32A_AMOXOR_W) {
    #define TEST_DEFINE
    #include "ISA_RV32A_AMOXOR_W.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32A_AMOAND_W) {
    #define TEST_DEFINE
    #include "ISA_RV32A_AMOAND_W.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32A_AMOOR_W) {
    #define TEST_DEFINE
    #include "ISA_RV32A_AMOOR_W.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32A_AMOMIN_W) {
    #define TEST_DEFINE
    #include "ISA_RV32A_AMOMIN_W.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32A_AMOMAX_W) {
    #define TEST_DEFINE
    #include "ISA_RV32A_AMOMAX_W.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32A_AMOMINU_W) {
    #define TEST_DEFINE
    #include "ISA_RV32A_AMOMINU_W.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32A_AMOMAXU_W) {
    #define TEST_DEFINE
    #include "ISA_RV32A_AMOMAXU_W.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32Zbb_CLZ) {
    #define TEST_DEFINE
    #include "ISA_RV32Zbb_CLZ.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32Zbb_CTZ) {
    #define TEST_DEFINE
    #include "ISA_RV32Zbb_CTZ.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32Zbb_CPOP) {
    #define TEST_DEFINE
    #include "ISA_RV32Zbb_CPOP.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32Zbb_MIN) {
    #define TEST_DEFINE
    #include "ISA_RV32Zbb_MIN.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32Zbb_MAX) {
    #define TEST_DEFINE
    #include "ISA_RV32Zbb_MAX.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32Zbb_MINU) {
    #define TEST_DEFINE
    #include "ISA_RV32Zbb_MINU.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32Zbb_MAXU) {
    #define TEST_DEFINE
    #include "ISA_RV32Zbb_MAXU.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32Zbb_SEXT_B) {
    #define TEST_DEFINE
    #include "ISA_RV32Zbb_SEXT_B.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32Zbb_SEXT_H) {
    #define TEST_DEFINE
    #include "ISA_RV32Zbb_SEXT_H.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32Zbb_ZEXT_H) {
    #define TEST_DEFINE
    #include "ISA_RV32Zbb_ZEXT_H.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32Zbb_ANDN) {
    #define TEST_DEFINE
    #include "ISA_RV32Zbb_ANDN.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32Zbb_ORN) {
    #define TEST_DEFINE
    #include "ISA_RV32Zbb_ORN.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32Zbb_XNOR) {
    #define TEST_DEFINE
    #include "ISA_RV32Zbb_XNOR.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32Zbb_ROL) {
    #define TEST_DEFINE
    #include "ISA_RV32Zbb_ROL.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32Zbb_ROR) {
    #define TEST_DEFINE
    #include "ISA_RV32Zbb_ROR.inc"
    #undef TEST_DEFINE
}

TEST_F(ISAUnittest, ISA_RV32Zbb_RORI) {
    #define TEST_DEFINE
    #include "ISA_RV32Zbb_RORI.inc"
    #undef TEST_DEFINE
}

#undef START_INST_CODE_DEFINE
