#ifndef _LATCH_MODULE_RV32_ISA_
#define _LATCH_MODULE_RV32_ISA_

// RV32IM 的指令编码、译码与 ALU
//
// 这里全是纯函数，不依赖运行时也不依赖任何模块，五级流水线的各级模块共用它。
// 译码结果 Rv32Decoded 里的每个字段都对应 RTL 里 ID 级吐出的一根控制线。

#include <cstdint>

namespace latch {
namespace rv32 {

inline uint32_t Opcode(uint32_t inst) { return inst & 0x7Fu; }
inline uint32_t Rd(uint32_t inst) { return (inst >> 7) & 0x1Fu; }
inline uint32_t Funct3(uint32_t inst) { return (inst >> 12) & 0x7u; }
inline uint32_t Rs1(uint32_t inst) { return (inst >> 15) & 0x1Fu; }
inline uint32_t Rs2(uint32_t inst) { return (inst >> 20) & 0x1Fu; }
inline uint32_t Funct7(uint32_t inst) { return (inst >> 25) & 0x7Fu; }

inline uint32_t SignExtend(uint32_t v, uint32_t bits) {
  uint32_t sh = 32u - bits;
  return static_cast<uint32_t>(static_cast<int32_t>(v << sh) >> sh);
}

inline uint32_t ImmI(uint32_t inst) { return SignExtend(inst >> 20, 12); }

inline uint32_t ImmS(uint32_t inst) {
  return SignExtend(((inst >> 25) << 5) | ((inst >> 7) & 0x1Fu), 12);
}

inline uint32_t ImmB(uint32_t inst) {
  uint32_t v = (((inst >> 31) & 0x1u) << 12) | (((inst >> 7) & 0x1u) << 11) |
               (((inst >> 25) & 0x3Fu) << 5) | (((inst >> 8) & 0xFu) << 1);
  return SignExtend(v, 13);
}

inline uint32_t ImmU(uint32_t inst) { return inst & 0xFFFFF000u; }

inline uint32_t ImmJ(uint32_t inst) {
  uint32_t v = (((inst >> 31) & 0x1u) << 20) | (((inst >> 12) & 0xFFu) << 12) |
               (((inst >> 20) & 0x1u) << 11) | (((inst >> 21) & 0x3FFu) << 1);
  return SignExtend(v, 21);
}

enum Op : uint32_t {
  kOpLoad = 0x03u,
  kOpMiscMem = 0x0Fu,
  kOpImm = 0x13u,
  kOpAuipc = 0x17u,
  kOpStore = 0x23u,
  kOpReg = 0x33u,
  kOpLui = 0x37u,
  kOpBranch = 0x63u,
  kOpJalr = 0x67u,
  kOpJal = 0x6Fu,
  kOpSystem = 0x73u,
};

// ALU 功能编码，与 RTL 里 alu_op 这根线的取值一一对应
enum AluOp : uint32_t {
  kAluAdd = 0,
  kAluSub = 1,
  kAluSll = 2,
  kAluSlt = 3,
  kAluSltu = 4,
  kAluXor = 5,
  kAluSrl = 6,
  kAluSra = 7,
  kAluOr = 8,
  kAluAnd = 9,
  kAluPassB = 10,  // LUI：直接把立即数送出去
  kAluMul = 11,
  kAluMulh = 12,
  kAluMulhsu = 13,
  kAluMulhu = 14,
  kAluDiv = 15,
  kAluDivu = 16,
  kAluRem = 17,
  kAluRemu = 18,
};

// alu_src_a 这根线：ALU 的 A 口选谁
enum AluSrcA : uint32_t { kSrcARs1 = 0, kSrcAPc = 1, kSrcAZero = 2 };
// alu_src_b 这根线：ALU 的 B 口选谁
enum AluSrcB : uint32_t { kSrcBRs2 = 0, kSrcBImm = 1 };
// wb_sel 这根线：写回数据选谁
enum WbSel : uint32_t { kWbAlu = 0, kWbMem = 1, kWbPc4 = 2 };

inline const char* RegAbi(uint32_t i) {
  static const char* kName[32] = {
      "zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0",
      "a1",   "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
      "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
  return kName[i & 31u];
}

// 译码结果：这一组就是 ID 级组合逻辑吐出的全部控制线
struct Rv32Decoded {
  uint32_t inst = 0;
  uint32_t opcode = 0;
  uint32_t rd = 0;
  uint32_t rs1 = 0;
  uint32_t rs2 = 0;
  uint32_t funct3 = 0;
  uint32_t funct7 = 0;
  uint32_t imm = 0;

  uint32_t alu_op = kAluAdd;
  uint32_t alu_src_a = kSrcARs1;
  uint32_t alu_src_b = kSrcBRs2;
  uint32_t wb_sel = kWbAlu;

  uint32_t reg_write = 0;
  uint32_t mem_read = 0;
  uint32_t mem_write = 0;
  uint32_t branch = 0;
  uint32_t jump = 0;  // JAL / JALR
  uint32_t jalr = 0;  // 目标由 rs1 给出
  uint32_t uses_rs1 = 0;
  uint32_t uses_rs2 = 0;
  uint32_t illegal = 0;
  uint32_t ebreak = 0;
  uint32_t ecall = 0;
};

inline Rv32Decoded Decode(uint32_t inst) {
  Rv32Decoded d;
  d.inst = inst;
  d.opcode = Opcode(inst);
  d.rd = Rd(inst);
  d.rs1 = Rs1(inst);
  d.rs2 = Rs2(inst);
  d.funct3 = Funct3(inst);
  d.funct7 = Funct7(inst);

  switch (d.opcode) {
    case kOpLui:
      d.imm = ImmU(inst);
      d.alu_op = kAluPassB;
      d.alu_src_a = kSrcAZero;
      d.alu_src_b = kSrcBImm;
      d.reg_write = 1;
      break;

    case kOpAuipc:
      d.imm = ImmU(inst);
      d.alu_op = kAluAdd;
      d.alu_src_a = kSrcAPc;
      d.alu_src_b = kSrcBImm;
      d.reg_write = 1;
      break;

    case kOpJal:
      d.imm = ImmJ(inst);
      d.alu_op = kAluAdd;
      d.alu_src_a = kSrcAPc;
      d.alu_src_b = kSrcBImm;
      d.wb_sel = kWbPc4;
      d.reg_write = 1;
      d.jump = 1;
      break;

    case kOpJalr:
      d.imm = ImmI(inst);
      d.alu_op = kAluAdd;
      d.alu_src_a = kSrcARs1;
      d.alu_src_b = kSrcBImm;
      d.wb_sel = kWbPc4;
      d.reg_write = 1;
      d.jump = 1;
      d.jalr = 1;
      d.uses_rs1 = 1;
      d.illegal = (d.funct3 != 0) ? 1u : 0u;
      break;

    case kOpBranch:
      d.imm = ImmB(inst);
      d.alu_op = kAluSub;
      d.branch = 1;
      d.uses_rs1 = 1;
      d.uses_rs2 = 1;
      d.illegal = (d.funct3 == 2 || d.funct3 == 3) ? 1u : 0u;
      break;

    case kOpLoad:
      d.imm = ImmI(inst);
      d.alu_op = kAluAdd;
      d.alu_src_b = kSrcBImm;
      d.wb_sel = kWbMem;
      d.reg_write = 1;
      d.mem_read = 1;
      d.uses_rs1 = 1;
      d.illegal = (d.funct3 == 3 || d.funct3 == 6 || d.funct3 == 7) ? 1u : 0u;
      break;

    case kOpStore:
      d.imm = ImmS(inst);
      d.alu_op = kAluAdd;
      d.alu_src_b = kSrcBImm;
      d.mem_write = 1;
      d.uses_rs1 = 1;
      d.uses_rs2 = 1;
      d.illegal = (d.funct3 > 2) ? 1u : 0u;
      break;

    case kOpImm: {
      d.imm = ImmI(inst);
      d.alu_src_b = kSrcBImm;
      d.reg_write = 1;
      d.uses_rs1 = 1;
      switch (d.funct3) {
        case 0: d.alu_op = kAluAdd; break;                        // ADDI
        case 1:                                                   // SLLI
          d.alu_op = kAluSll;
          d.imm = (inst >> 20) & 0x1Fu;
          d.illegal = (d.funct7 != 0) ? 1u : 0u;
          break;
        case 2: d.alu_op = kAluSlt; break;                        // SLTI
        case 3: d.alu_op = kAluSltu; break;                       // SLTIU
        case 4: d.alu_op = kAluXor; break;                        // XORI
        case 5:                                                   // SRLI/SRAI
          d.alu_op = (d.funct7 == 0x20u) ? kAluSra : kAluSrl;
          d.imm = (inst >> 20) & 0x1Fu;
          d.illegal = (d.funct7 != 0 && d.funct7 != 0x20u) ? 1u : 0u;
          break;
        case 6: d.alu_op = kAluOr; break;                         // ORI
        default: d.alu_op = kAluAnd; break;                       // ANDI
      }
      break;
    }

    case kOpReg: {
      d.reg_write = 1;
      d.uses_rs1 = 1;
      d.uses_rs2 = 1;
      if (d.funct7 == 0x01u) {  // M 扩展
        switch (d.funct3) {
          case 0: d.alu_op = kAluMul; break;
          case 1: d.alu_op = kAluMulh; break;
          case 2: d.alu_op = kAluMulhsu; break;
          case 3: d.alu_op = kAluMulhu; break;
          case 4: d.alu_op = kAluDiv; break;
          case 5: d.alu_op = kAluDivu; break;
          case 6: d.alu_op = kAluRem; break;
          default: d.alu_op = kAluRemu; break;
        }
      } else if (d.funct7 == 0x00u || d.funct7 == 0x20u) {
        const bool alt = (d.funct7 == 0x20u);
        switch (d.funct3) {
          case 0: d.alu_op = alt ? kAluSub : kAluAdd; break;
          case 1: d.alu_op = kAluSll; d.illegal = alt; break;
          case 2: d.alu_op = kAluSlt; d.illegal = alt; break;
          case 3: d.alu_op = kAluSltu; d.illegal = alt; break;
          case 4: d.alu_op = kAluXor; d.illegal = alt; break;
          case 5: d.alu_op = alt ? kAluSra : kAluSrl; break;
          case 6: d.alu_op = kAluOr; d.illegal = alt; break;
          default: d.alu_op = kAluAnd; d.illegal = alt; break;
        }
      } else {
        d.illegal = 1;
      }
      break;
    }

    case kOpMiscMem:
      // FENCE / FENCE.I：这个模型里访存是顺序的，当空操作处理
      break;

    case kOpSystem:
      // 只认 ecall 与 ebreak，没有 CSR 文件，csrrw 之类一律当非法指令
      if (inst == 0x00000073u) {
        d.ecall = 1;
      } else if (inst == 0x00100073u) {
        d.ebreak = 1;
      } else {
        d.illegal = 1;
      }
      break;

    default:
      d.illegal = 1;
      break;
  }

  if (d.rd == 0) d.reg_write = 0;  // x0 恒为 0，写口直接封掉
  return d;
}

inline uint32_t AluCompute(uint32_t op, uint32_t a, uint32_t b) {
  const int32_t sa = static_cast<int32_t>(a);
  const int32_t sb = static_cast<int32_t>(b);
  switch (op) {
    case kAluAdd: return a + b;
    case kAluSub: return a - b;
    case kAluSll: return a << (b & 31u);
    case kAluSlt: return (sa < sb) ? 1u : 0u;
    case kAluSltu: return (a < b) ? 1u : 0u;
    case kAluXor: return a ^ b;
    case kAluSrl: return a >> (b & 31u);
    case kAluSra: return static_cast<uint32_t>(sa >> (b & 31u));
    case kAluOr: return a | b;
    case kAluAnd: return a & b;
    case kAluPassB: return b;
    case kAluMul:
      return static_cast<uint32_t>(static_cast<uint64_t>(a) *
                                   static_cast<uint64_t>(b));
    case kAluMulh:
      return static_cast<uint32_t>(
          (static_cast<int64_t>(sa) * static_cast<int64_t>(sb)) >> 32);
    case kAluMulhsu:
      return static_cast<uint32_t>(
          (static_cast<int64_t>(sa) *
           static_cast<int64_t>(static_cast<uint64_t>(b))) >> 32);
    case kAluMulhu:
      return static_cast<uint32_t>(
          (static_cast<uint64_t>(a) * static_cast<uint64_t>(b)) >> 32);
    case kAluDiv:
      if (b == 0) return 0xFFFFFFFFu;
      if (a == 0x80000000u && b == 0xFFFFFFFFu) return 0x80000000u;
      return static_cast<uint32_t>(sa / sb);
    case kAluDivu:
      if (b == 0) return 0xFFFFFFFFu;
      return a / b;
    case kAluRem:
      if (b == 0) return a;
      if (a == 0x80000000u && b == 0xFFFFFFFFu) return 0;
      return static_cast<uint32_t>(sa % sb);
    case kAluRemu:
      if (b == 0) return a;
      return a % b;
    default: return 0;
  }
}

inline bool BranchTaken(uint32_t funct3, uint32_t a, uint32_t b) {
  const int32_t sa = static_cast<int32_t>(a);
  const int32_t sb = static_cast<int32_t>(b);
  switch (funct3) {
    case 0: return a == b;    // BEQ
    case 1: return a != b;    // BNE
    case 4: return sa < sb;   // BLT
    case 5: return sa >= sb;  // BGE
    case 6: return a < b;     // BLTU
    case 7: return a >= b;    // BGEU
    default: return false;
  }
}

// 访存宽度（字节），由 funct3 低两位给出
inline uint32_t AccessSize(uint32_t funct3) {
  switch (funct3 & 3u) {
    case 0: return 1;
    case 1: return 2;
    default: return 4;
  }
}

// 字节使能：4 位，bit i 对应对齐字里的第 i 个字节
inline uint32_t ByteEnable(uint32_t funct3, uint32_t addr) {
  const uint32_t off = addr & 3u;
  switch (funct3 & 3u) {
    case 0: return 0x1u << off;
    case 1: return 0x3u << off;
    default: return 0xFu;
  }
}

// store 数据按字节偏移对齐到字里
inline uint32_t AlignStore(uint32_t data, uint32_t addr) {
  return data << ((addr & 3u) * 8u);
}

// load 数据从字里取出并按 funct3 做符号或零扩展
inline uint32_t ExtractLoad(uint32_t word_data, uint32_t funct3,
                            uint32_t addr) {
  const uint32_t shifted = word_data >> ((addr & 3u) * 8u);
  switch (funct3) {
    case 0: return SignExtend(shifted & 0xFFu, 8);     // LB
    case 1: return SignExtend(shifted & 0xFFFFu, 16);  // LH
    case 2: return word_data;                          // LW
    case 4: return shifted & 0xFFu;                    // LBU
    case 5: return shifted & 0xFFFFu;                  // LHU
    default: return word_data;
  }
}

}  // namespace rv32
}  // namespace latch

#endif
