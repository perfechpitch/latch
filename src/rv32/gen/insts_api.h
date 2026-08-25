/**
**/
//
// 由 ISA DSL 工具生成。
//
// clang-format off
#pragma GCC diagnostic ignored "-Wunused-parameter"

class ISA_RV32I_ADD : public Rv32Instruction {
 public:
  explicit ISA_RV32I_ADD(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_ADD(InstBinary const& binAddr);
  ~ISA_RV32I_ADD() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_SUB : public Rv32Instruction {
 public:
  explicit ISA_RV32I_SUB(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_SUB(InstBinary const& binAddr);
  ~ISA_RV32I_SUB() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_SLL : public Rv32Instruction {
 public:
  explicit ISA_RV32I_SLL(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_SLL(InstBinary const& binAddr);
  ~ISA_RV32I_SLL() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_SLT : public Rv32Instruction {
 public:
  explicit ISA_RV32I_SLT(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_SLT(InstBinary const& binAddr);
  ~ISA_RV32I_SLT() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_SLTU : public Rv32Instruction {
 public:
  explicit ISA_RV32I_SLTU(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_SLTU(InstBinary const& binAddr);
  ~ISA_RV32I_SLTU() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_XOR : public Rv32Instruction {
 public:
  explicit ISA_RV32I_XOR(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_XOR(InstBinary const& binAddr);
  ~ISA_RV32I_XOR() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_SRL : public Rv32Instruction {
 public:
  explicit ISA_RV32I_SRL(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_SRL(InstBinary const& binAddr);
  ~ISA_RV32I_SRL() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_SRA : public Rv32Instruction {
 public:
  explicit ISA_RV32I_SRA(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_SRA(InstBinary const& binAddr);
  ~ISA_RV32I_SRA() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_OR : public Rv32Instruction {
 public:
  explicit ISA_RV32I_OR(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_OR(InstBinary const& binAddr);
  ~ISA_RV32I_OR() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_AND : public Rv32Instruction {
 public:
  explicit ISA_RV32I_AND(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_AND(InstBinary const& binAddr);
  ~ISA_RV32I_AND() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_ADDI : public Rv32Instruction {
 public:
  explicit ISA_RV32I_ADDI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ );
  explicit ISA_RV32I_ADDI(InstBinary const& binAddr);
  ~ISA_RV32I_ADDI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM12;
};

class ISA_RV32I_SLTI : public Rv32Instruction {
 public:
  explicit ISA_RV32I_SLTI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ );
  explicit ISA_RV32I_SLTI(InstBinary const& binAddr);
  ~ISA_RV32I_SLTI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM12;
};

class ISA_RV32I_SLTIU : public Rv32Instruction {
 public:
  explicit ISA_RV32I_SLTIU(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ );
  explicit ISA_RV32I_SLTIU(InstBinary const& binAddr);
  ~ISA_RV32I_SLTIU() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM12;
};

class ISA_RV32I_XORI : public Rv32Instruction {
 public:
  explicit ISA_RV32I_XORI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ );
  explicit ISA_RV32I_XORI(InstBinary const& binAddr);
  ~ISA_RV32I_XORI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM12;
};

class ISA_RV32I_ORI : public Rv32Instruction {
 public:
  explicit ISA_RV32I_ORI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ );
  explicit ISA_RV32I_ORI(InstBinary const& binAddr);
  ~ISA_RV32I_ORI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM12;
};

class ISA_RV32I_ANDI : public Rv32Instruction {
 public:
  explicit ISA_RV32I_ANDI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ );
  explicit ISA_RV32I_ANDI(InstBinary const& binAddr);
  ~ISA_RV32I_ANDI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM12;
};

class ISA_RV32I_SLLI : public Rv32Instruction {
 public:
  explicit ISA_RV32I_SLLI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM5_ );
  explicit ISA_RV32I_SLLI(InstBinary const& binAddr);
  ~ISA_RV32I_SLLI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM5;
  ConstField _CONST0;
};

class ISA_RV32I_SRLI : public Rv32Instruction {
 public:
  explicit ISA_RV32I_SRLI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM5_ );
  explicit ISA_RV32I_SRLI(InstBinary const& binAddr);
  ~ISA_RV32I_SRLI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM5;
  ConstField _CONST0;
};

class ISA_RV32I_SRAI : public Rv32Instruction {
 public:
  explicit ISA_RV32I_SRAI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM5_ );
  explicit ISA_RV32I_SRAI(InstBinary const& binAddr);
  ~ISA_RV32I_SRAI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM5;
  ConstField _CONST32;
};

class ISA_RV32I_LB : public Rv32Instruction {
 public:
  explicit ISA_RV32I_LB(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ );
  explicit ISA_RV32I_LB(InstBinary const& binAddr);
  ~ISA_RV32I_LB() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM12;
};

class ISA_RV32I_LH : public Rv32Instruction {
 public:
  explicit ISA_RV32I_LH(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ );
  explicit ISA_RV32I_LH(InstBinary const& binAddr);
  ~ISA_RV32I_LH() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM12;
};

class ISA_RV32I_LW : public Rv32Instruction {
 public:
  explicit ISA_RV32I_LW(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ );
  explicit ISA_RV32I_LW(InstBinary const& binAddr);
  ~ISA_RV32I_LW() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM12;
};

class ISA_RV32I_LBU : public Rv32Instruction {
 public:
  explicit ISA_RV32I_LBU(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ );
  explicit ISA_RV32I_LBU(InstBinary const& binAddr);
  ~ISA_RV32I_LBU() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM12;
};

class ISA_RV32I_LHU : public Rv32Instruction {
 public:
  explicit ISA_RV32I_LHU(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ );
  explicit ISA_RV32I_LHU(InstBinary const& binAddr);
  ~ISA_RV32I_LHU() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM12;
};

class ISA_RV32I_SB : public Rv32Instruction {
 public:
  explicit ISA_RV32I_SB(uint64_t _IMM5_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM7_ );
  explicit ISA_RV32I_SB(InstBinary const& binAddr);
  ~ISA_RV32I_SB() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM5;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _IMM7;
};

class ISA_RV32I_SH : public Rv32Instruction {
 public:
  explicit ISA_RV32I_SH(uint64_t _IMM5_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM7_ );
  explicit ISA_RV32I_SH(InstBinary const& binAddr);
  ~ISA_RV32I_SH() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM5;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _IMM7;
};

class ISA_RV32I_SW : public Rv32Instruction {
 public:
  explicit ISA_RV32I_SW(uint64_t _IMM5_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM7_ );
  explicit ISA_RV32I_SW(InstBinary const& binAddr);
  ~ISA_RV32I_SW() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM5;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _IMM7;
};

class ISA_RV32I_BEQ : public Rv32Instruction {
 public:
  explicit ISA_RV32I_BEQ(uint64_t _IMM1_, uint64_t _IMM4_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM6_, uint64_t _IMM1_U0_ );
  explicit ISA_RV32I_BEQ(InstBinary const& binAddr);
  ~ISA_RV32I_BEQ() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM1;
  Config _IMM4;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _IMM6;
  Config _IMM1_U0;
};

class ISA_RV32I_BNE : public Rv32Instruction {
 public:
  explicit ISA_RV32I_BNE(uint64_t _IMM1_, uint64_t _IMM4_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM6_, uint64_t _IMM1_U0_ );
  explicit ISA_RV32I_BNE(InstBinary const& binAddr);
  ~ISA_RV32I_BNE() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM1;
  Config _IMM4;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _IMM6;
  Config _IMM1_U0;
};

class ISA_RV32I_BLT : public Rv32Instruction {
 public:
  explicit ISA_RV32I_BLT(uint64_t _IMM1_, uint64_t _IMM4_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM6_, uint64_t _IMM1_U0_ );
  explicit ISA_RV32I_BLT(InstBinary const& binAddr);
  ~ISA_RV32I_BLT() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM1;
  Config _IMM4;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _IMM6;
  Config _IMM1_U0;
};

class ISA_RV32I_BGE : public Rv32Instruction {
 public:
  explicit ISA_RV32I_BGE(uint64_t _IMM1_, uint64_t _IMM4_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM6_, uint64_t _IMM1_U0_ );
  explicit ISA_RV32I_BGE(InstBinary const& binAddr);
  ~ISA_RV32I_BGE() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM1;
  Config _IMM4;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _IMM6;
  Config _IMM1_U0;
};

class ISA_RV32I_BLTU : public Rv32Instruction {
 public:
  explicit ISA_RV32I_BLTU(uint64_t _IMM1_, uint64_t _IMM4_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM6_, uint64_t _IMM1_U0_ );
  explicit ISA_RV32I_BLTU(InstBinary const& binAddr);
  ~ISA_RV32I_BLTU() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM1;
  Config _IMM4;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _IMM6;
  Config _IMM1_U0;
};

class ISA_RV32I_BGEU : public Rv32Instruction {
 public:
  explicit ISA_RV32I_BGEU(uint64_t _IMM1_, uint64_t _IMM4_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _IMM6_, uint64_t _IMM1_U0_ );
  explicit ISA_RV32I_BGEU(InstBinary const& binAddr);
  ~ISA_RV32I_BGEU() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM1;
  Config _IMM4;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _IMM6;
  Config _IMM1_U0;
};

class ISA_RV32I_LUI : public Rv32Instruction {
 public:
  explicit ISA_RV32I_LUI(uint64_t _RD_, uint64_t _IMM20_ );
  explicit ISA_RV32I_LUI(InstBinary const& binAddr);
  ~ISA_RV32I_LUI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  Config _IMM20;
};

class ISA_RV32I_AUIPC : public Rv32Instruction {
 public:
  explicit ISA_RV32I_AUIPC(uint64_t _RD_, uint64_t _IMM20_ );
  explicit ISA_RV32I_AUIPC(InstBinary const& binAddr);
  ~ISA_RV32I_AUIPC() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  Config _IMM20;
};

class ISA_RV32I_JAL : public Rv32Instruction {
 public:
  explicit ISA_RV32I_JAL(uint64_t _RD_, uint64_t _IMM8_, uint64_t _IMM1_, uint64_t _IMM10_, uint64_t _IMM1_U0_ );
  explicit ISA_RV32I_JAL(InstBinary const& binAddr);
  ~ISA_RV32I_JAL() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  Config _IMM8;
  Config _IMM1;
  Config _IMM10;
  Config _IMM1_U0;
};

class ISA_RV32I_JALR : public Rv32Instruction {
 public:
  explicit ISA_RV32I_JALR(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ );
  explicit ISA_RV32I_JALR(InstBinary const& binAddr);
  ~ISA_RV32I_JALR() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM12;
};

class ISA_RV32I_FENCE : public Rv32Instruction {
 public:
  explicit ISA_RV32I_FENCE(uint64_t _RD_, uint64_t _RS1_, uint64_t _SUCC_, uint64_t _PRED_ );
  explicit ISA_RV32I_FENCE(InstBinary const& binAddr);
  ~ISA_RV32I_FENCE() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _SUCC;
  Config _PRED;
  ConstField _CONST0;
};

class ISA_RV32I_FENCE_I : public Rv32Instruction {
 public:
  explicit ISA_RV32I_FENCE_I(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM12_ );
  explicit ISA_RV32I_FENCE_I(InstBinary const& binAddr);
  ~ISA_RV32I_FENCE_I() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM12;
};

class ISA_RV32I_ECALL : public Rv32Instruction {
 public:
  explicit ISA_RV32I_ECALL(uint64_t _RD_, uint64_t _RS1_ );
  explicit ISA_RV32I_ECALL(InstBinary const& binAddr);
  ~ISA_RV32I_ECALL() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  ConstField _FUNCT12;
};

class ISA_RV32I_EBREAK : public Rv32Instruction {
 public:
  explicit ISA_RV32I_EBREAK(uint64_t _RD_, uint64_t _RS1_ );
  explicit ISA_RV32I_EBREAK(InstBinary const& binAddr);
  ~ISA_RV32I_EBREAK() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  ConstField _FUNCT12;
};

class ISA_RV32I_CSRRW : public Rv32Instruction {
 public:
  explicit ISA_RV32I_CSRRW(uint64_t _RD_, uint64_t _RS1_, uint64_t _CSR_ );
  explicit ISA_RV32I_CSRRW(InstBinary const& binAddr);
  ~ISA_RV32I_CSRRW() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _CSR;
};

class ISA_RV32I_CSRRS : public Rv32Instruction {
 public:
  explicit ISA_RV32I_CSRRS(uint64_t _RD_, uint64_t _RS1_, uint64_t _CSR_ );
  explicit ISA_RV32I_CSRRS(InstBinary const& binAddr);
  ~ISA_RV32I_CSRRS() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _CSR;
};

class ISA_RV32I_CSRRC : public Rv32Instruction {
 public:
  explicit ISA_RV32I_CSRRC(uint64_t _RD_, uint64_t _RS1_, uint64_t _CSR_ );
  explicit ISA_RV32I_CSRRC(InstBinary const& binAddr);
  ~ISA_RV32I_CSRRC() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _CSR;
};

class ISA_RV32I_CSRRWI : public Rv32Instruction {
 public:
  explicit ISA_RV32I_CSRRWI(uint64_t _RD_, uint64_t _IMM5_, uint64_t _CSR_ );
  explicit ISA_RV32I_CSRRWI(InstBinary const& binAddr);
  ~ISA_RV32I_CSRRWI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _IMM5;
  Config _CSR;
};

class ISA_RV32I_CSRRSI : public Rv32Instruction {
 public:
  explicit ISA_RV32I_CSRRSI(uint64_t _RD_, uint64_t _IMM5_, uint64_t _CSR_ );
  explicit ISA_RV32I_CSRRSI(InstBinary const& binAddr);
  ~ISA_RV32I_CSRRSI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _IMM5;
  Config _CSR;
};

class ISA_RV32I_CSRRCI : public Rv32Instruction {
 public:
  explicit ISA_RV32I_CSRRCI(uint64_t _RD_, uint64_t _IMM5_, uint64_t _CSR_ );
  explicit ISA_RV32I_CSRRCI(InstBinary const& binAddr);
  ~ISA_RV32I_CSRRCI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _IMM5;
  Config _CSR;
};

class ISA_RV32I_MUL : public Rv32Instruction {
 public:
  explicit ISA_RV32I_MUL(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_MUL(InstBinary const& binAddr);
  ~ISA_RV32I_MUL() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_MULH : public Rv32Instruction {
 public:
  explicit ISA_RV32I_MULH(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_MULH(InstBinary const& binAddr);
  ~ISA_RV32I_MULH() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_MULHSU : public Rv32Instruction {
 public:
  explicit ISA_RV32I_MULHSU(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_MULHSU(InstBinary const& binAddr);
  ~ISA_RV32I_MULHSU() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_MULHU : public Rv32Instruction {
 public:
  explicit ISA_RV32I_MULHU(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_MULHU(InstBinary const& binAddr);
  ~ISA_RV32I_MULHU() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_DIV : public Rv32Instruction {
 public:
  explicit ISA_RV32I_DIV(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_DIV(InstBinary const& binAddr);
  ~ISA_RV32I_DIV() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_DIVU : public Rv32Instruction {
 public:
  explicit ISA_RV32I_DIVU(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_DIVU(InstBinary const& binAddr);
  ~ISA_RV32I_DIVU() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_REM : public Rv32Instruction {
 public:
  explicit ISA_RV32I_REM(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_REM(InstBinary const& binAddr);
  ~ISA_RV32I_REM() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32I_REMU : public Rv32Instruction {
 public:
  explicit ISA_RV32I_REMU(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32I_REMU(InstBinary const& binAddr);
  ~ISA_RV32I_REMU() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32C_NOP : public Rv32Instruction {
 public:
  explicit ISA_RV32C_NOP(uint64_t _IMM5_, uint64_t _IMM1_ );
  explicit ISA_RV32C_NOP(InstBinary const& binAddr);
  ~ISA_RV32C_NOP() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM5;
  ConstField _FUNCT5;
  Config _IMM1;
  ConstField _FUNCT3;
};

class ISA_RV32C_ADDI : public Rv32Instruction {
 public:
  explicit ISA_RV32C_ADDI(uint64_t _IMM5_, uint64_t _RD_, uint64_t _IMM1_ );
  explicit ISA_RV32C_ADDI(InstBinary const& binAddr);
  ~ISA_RV32C_ADDI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM5;
  Config _RD;
  Config _IMM1;
  ConstField _FUNCT3;
};

class ISA_RV32C_JAL : public Rv32Instruction {
 public:
  explicit ISA_RV32C_JAL(uint64_t _IMM1_5_, uint64_t _IMM3_3_1_, uint64_t _IMM1_7_, uint64_t _IMM1_6_, uint64_t _IMM1_10_, uint64_t _IMM2_9_8_, uint64_t _IMM1_4_, uint64_t _IMM1_11_ );
  explicit ISA_RV32C_JAL(InstBinary const& binAddr);
  ~ISA_RV32C_JAL() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM1_5;
  Config _IMM3_3_1;
  Config _IMM1_7;
  Config _IMM1_6;
  Config _IMM1_10;
  Config _IMM2_9_8;
  Config _IMM1_4;
  Config _IMM1_11;
  ConstField _FUNCT3;
};

class ISA_RV32C_LI : public Rv32Instruction {
 public:
  explicit ISA_RV32C_LI(uint64_t _IMM5_, uint64_t _RD_, uint64_t _IMM1_ );
  explicit ISA_RV32C_LI(InstBinary const& binAddr);
  ~ISA_RV32C_LI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM5;
  Config _RD;
  Config _IMM1;
  ConstField _FUNCT3;
};

class ISA_RV32C_ADDI16SP : public Rv32Instruction {
 public:
  explicit ISA_RV32C_ADDI16SP(uint64_t _IMM1_5_, uint64_t _IMM2_7_8_, uint64_t _IMM1_6_, uint64_t _IMM1_4_, uint64_t _IMM1_9_ );
  explicit ISA_RV32C_ADDI16SP(InstBinary const& binAddr);
  ~ISA_RV32C_ADDI16SP() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM1_5;
  Config _IMM2_7_8;
  Config _IMM1_6;
  Config _IMM1_4;
  ConstField _FUNCT5;
  Config _IMM1_9;
  ConstField _FUNCT3;
};

class ISA_RV32C_LUI : public Rv32Instruction {
 public:
  explicit ISA_RV32C_LUI(uint64_t _IMM5_, uint64_t _RD_, uint64_t _IMM1_ );
  explicit ISA_RV32C_LUI(InstBinary const& binAddr);
  ~ISA_RV32C_LUI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM5;
  Config _RD;
  Config _IMM1;
  ConstField _FUNCT3;
};

class ISA_RV32C_SRLI : public Rv32Instruction {
 public:
  explicit ISA_RV32C_SRLI(uint64_t _IMM5_, uint64_t _RD_, uint64_t _IMM1_ );
  explicit ISA_RV32C_SRLI(InstBinary const& binAddr);
  ~ISA_RV32C_SRLI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM5;
  Config _RD;
  ConstField _FUNCT2;
  Config _IMM1;
  ConstField _FUNCT3;
};

class ISA_RV32C_SRAI : public Rv32Instruction {
 public:
  explicit ISA_RV32C_SRAI(uint64_t _IMM5_, uint64_t _RD_, uint64_t _IMM1_ );
  explicit ISA_RV32C_SRAI(InstBinary const& binAddr);
  ~ISA_RV32C_SRAI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM5;
  Config _RD;
  ConstField _FUNCT2;
  Config _IMM1;
  ConstField _FUNCT3;
};

class ISA_RV32C_ANDI : public Rv32Instruction {
 public:
  explicit ISA_RV32C_ANDI(uint64_t _IMM5_, uint64_t _RD_, uint64_t _IMM1_ );
  explicit ISA_RV32C_ANDI(InstBinary const& binAddr);
  ~ISA_RV32C_ANDI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM5;
  Config _RD;
  ConstField _FUNCT2;
  Config _IMM1;
  ConstField _FUNCT3;
};

class ISA_RV32C_SUB : public Rv32Instruction {
 public:
  explicit ISA_RV32C_SUB(uint64_t _RS2_, uint64_t _RD_ );
  explicit ISA_RV32C_SUB(InstBinary const& binAddr);
  ~ISA_RV32C_SUB() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RS2;
  ConstField _FUNCT2;
  Config _RD;
  ConstField _FUNCT2_1;
  ConstField _FUNCT1;
  ConstField _FUNCT3;
};

class ISA_RV32C_XOR : public Rv32Instruction {
 public:
  explicit ISA_RV32C_XOR(uint64_t _RS2_, uint64_t _RD_ );
  explicit ISA_RV32C_XOR(InstBinary const& binAddr);
  ~ISA_RV32C_XOR() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RS2;
  ConstField _FUNCT2;
  Config _RD;
  ConstField _FUNCT2_1;
  ConstField _FUNCT1;
  ConstField _FUNCT3;
};

class ISA_RV32C_OR : public Rv32Instruction {
 public:
  explicit ISA_RV32C_OR(uint64_t _RS2_, uint64_t _RD_ );
  explicit ISA_RV32C_OR(InstBinary const& binAddr);
  ~ISA_RV32C_OR() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RS2;
  ConstField _FUNCT2;
  Config _RD;
  ConstField _FUNCT2_1;
  ConstField _FUNCT1;
  ConstField _FUNCT3;
};

class ISA_RV32C_AND : public Rv32Instruction {
 public:
  explicit ISA_RV32C_AND(uint64_t _RS2_, uint64_t _RD_ );
  explicit ISA_RV32C_AND(InstBinary const& binAddr);
  ~ISA_RV32C_AND() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RS2;
  ConstField _FUNCT2;
  Config _RD;
  ConstField _FUNCT2_1;
  ConstField _FUNCT1;
  ConstField _FUNCT3;
};

class ISA_RV32C_J : public Rv32Instruction {
 public:
  explicit ISA_RV32C_J(uint64_t _IMM1_5_, uint64_t _IMM3_3_1_, uint64_t _IMM1_7_, uint64_t _IMM1_6_, uint64_t _IMM1_10_, uint64_t _IMM2_9_8_, uint64_t _IMM1_4_, uint64_t _IMM1_11_ );
  explicit ISA_RV32C_J(InstBinary const& binAddr);
  ~ISA_RV32C_J() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM1_5;
  Config _IMM3_3_1;
  Config _IMM1_7;
  Config _IMM1_6;
  Config _IMM1_10;
  Config _IMM2_9_8;
  Config _IMM1_4;
  Config _IMM1_11;
  ConstField _FUNCT3;
};

class ISA_RV32C_BEQZ : public Rv32Instruction {
 public:
  explicit ISA_RV32C_BEQZ(uint64_t _IMM1_5_, uint64_t _IMM2_2_1_, uint64_t _IMM2_6_7_, uint64_t _RS1_, uint64_t _IMM2_3_4_, uint64_t _IMM1_8_ );
  explicit ISA_RV32C_BEQZ(InstBinary const& binAddr);
  ~ISA_RV32C_BEQZ() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM1_5;
  Config _IMM2_2_1;
  Config _IMM2_6_7;
  Config _RS1;
  Config _IMM2_3_4;
  Config _IMM1_8;
  ConstField _FUNCT3;
};

class ISA_RV32C_BNEZ : public Rv32Instruction {
 public:
  explicit ISA_RV32C_BNEZ(uint64_t _IMM1_5_, uint64_t _IMM2_2_1_, uint64_t _IMM2_6_7_, uint64_t _RS1_, uint64_t _IMM2_3_4_, uint64_t _IMM1_8_ );
  explicit ISA_RV32C_BNEZ(InstBinary const& binAddr);
  ~ISA_RV32C_BNEZ() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM1_5;
  Config _IMM2_2_1;
  Config _IMM2_6_7;
  Config _RS1;
  Config _IMM2_3_4;
  Config _IMM1_8;
  ConstField _FUNCT3;
};

class ISA_RV32C_ADDI4SPN : public Rv32Instruction {
 public:
  explicit ISA_RV32C_ADDI4SPN(uint64_t _RD_, uint64_t _IMM1_3_, uint64_t _IMM1_2_, uint64_t _IMM4_9_6_, uint64_t _IMM2_5_4_ );
  explicit ISA_RV32C_ADDI4SPN(InstBinary const& binAddr);
  ~ISA_RV32C_ADDI4SPN() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  Config _IMM1_3;
  Config _IMM1_2;
  Config _IMM4_9_6;
  Config _IMM2_5_4;
  ConstField _FUNCT3;
};

class ISA_RV32C_FLD : public Rv32Instruction {
 public:
  explicit ISA_RV32C_FLD(uint64_t _RD_, uint64_t _IMM2_, uint64_t _RS1_, uint64_t _IMM3_ );
  explicit ISA_RV32C_FLD(InstBinary const& binAddr);
  ~ISA_RV32C_FLD() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  Config _IMM2;
  Config _RS1;
  Config _IMM3;
  ConstField _FUNCT3;
};

class ISA_RV32C_LW : public Rv32Instruction {
 public:
  explicit ISA_RV32C_LW(uint64_t _RD_, uint64_t _IMM1_6_, uint64_t _IMM1_2_, uint64_t _RS1_, uint64_t _IMM3_3_5_ );
  explicit ISA_RV32C_LW(InstBinary const& binAddr);
  ~ISA_RV32C_LW() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  Config _IMM1_6;
  Config _IMM1_2;
  Config _RS1;
  Config _IMM3_3_5;
  ConstField _FUNCT3;
};

class ISA_RV32C_FLW : public Rv32Instruction {
 public:
  explicit ISA_RV32C_FLW(uint64_t _RD_, uint64_t _IMM1_6_, uint64_t _IMM1_2_, uint64_t _RS1_, uint64_t _IMM3_3_5_ );
  explicit ISA_RV32C_FLW(InstBinary const& binAddr);
  ~ISA_RV32C_FLW() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  Config _IMM1_6;
  Config _IMM1_2;
  Config _RS1;
  Config _IMM3_3_5;
  ConstField _FUNCT3;
};

class ISA_RV32C_FSD : public Rv32Instruction {
 public:
  explicit ISA_RV32C_FSD(uint64_t _RD_, uint64_t _IMM2_, uint64_t _RS1_, uint64_t _IMM3_ );
  explicit ISA_RV32C_FSD(InstBinary const& binAddr);
  ~ISA_RV32C_FSD() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  Config _IMM2;
  Config _RS1;
  Config _IMM3;
  ConstField _FUNCT3;
};

class ISA_RV32C_SW : public Rv32Instruction {
 public:
  explicit ISA_RV32C_SW(uint64_t _RD_, uint64_t _IMM1_6_, uint64_t _IMM1_2_, uint64_t _RS1_, uint64_t _IMM3_3_5_ );
  explicit ISA_RV32C_SW(InstBinary const& binAddr);
  ~ISA_RV32C_SW() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  Config _IMM1_6;
  Config _IMM1_2;
  Config _RS1;
  Config _IMM3_3_5;
  ConstField _FUNCT3;
};

class ISA_RV32C_FSW : public Rv32Instruction {
 public:
  explicit ISA_RV32C_FSW(uint64_t _RD_, uint64_t _IMM1_6_, uint64_t _IMM1_2_, uint64_t _RS1_, uint64_t _IMM3_3_5_ );
  explicit ISA_RV32C_FSW(InstBinary const& binAddr);
  ~ISA_RV32C_FSW() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  Config _IMM1_6;
  Config _IMM1_2;
  Config _RS1;
  Config _IMM3_3_5;
  ConstField _FUNCT3;
};

class ISA_RV32C_SLLI : public Rv32Instruction {
 public:
  explicit ISA_RV32C_SLLI(uint64_t _IMM5_, uint64_t _RD_, uint64_t _IMM1_ );
  explicit ISA_RV32C_SLLI(InstBinary const& binAddr);
  ~ISA_RV32C_SLLI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM5;
  Config _RD;
  Config _IMM1;
  ConstField _FUNCT3;
};

class ISA_RV32C_FLDSP : public Rv32Instruction {
 public:
  explicit ISA_RV32C_FLDSP(uint64_t _IMM3_, uint64_t _IMM2_, uint64_t _RD_, uint64_t _IMM1_ );
  explicit ISA_RV32C_FLDSP(InstBinary const& binAddr);
  ~ISA_RV32C_FLDSP() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM3;
  Config _IMM2;
  Config _RD;
  Config _IMM1;
  ConstField _FUNCT3;
};

class ISA_RV32C_LWSP : public Rv32Instruction {
 public:
  explicit ISA_RV32C_LWSP(uint64_t _IMM2_7_6_, uint64_t _IMM3_4_2_, uint64_t _RD_, uint64_t _IMM1_5_ );
  explicit ISA_RV32C_LWSP(InstBinary const& binAddr);
  ~ISA_RV32C_LWSP() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM2_7_6;
  Config _IMM3_4_2;
  Config _RD;
  Config _IMM1_5;
  ConstField _FUNCT3;
};

class ISA_RV32C_FLWSP : public Rv32Instruction {
 public:
  explicit ISA_RV32C_FLWSP(uint64_t _IMM3_, uint64_t _IMM2_, uint64_t _RD_, uint64_t _IMM1_ );
  explicit ISA_RV32C_FLWSP(InstBinary const& binAddr);
  ~ISA_RV32C_FLWSP() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _IMM3;
  Config _IMM2;
  Config _RD;
  Config _IMM1;
  ConstField _FUNCT3;
};

class ISA_RV32C_JR : public Rv32Instruction {
 public:
  explicit ISA_RV32C_JR(uint64_t _RS1_ );
  explicit ISA_RV32C_JR(InstBinary const& binAddr);
  ~ISA_RV32C_JR() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  ConstField _FUNCT5;
  Config _RS1;
  ConstField _FUNCT1;
  ConstField _FUNCT3;
};

class ISA_RV32C_MV : public Rv32Instruction {
 public:
  explicit ISA_RV32C_MV(uint64_t _RS2_, uint64_t _RD_ );
  explicit ISA_RV32C_MV(InstBinary const& binAddr);
  ~ISA_RV32C_MV() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RS2;
  Config _RD;
  ConstField _FUNCT1;
  ConstField _FUNCT3;
};

class ISA_RV32C_EBREAK : public Rv32Instruction {
 public:
  explicit ISA_RV32C_EBREAK();
  explicit ISA_RV32C_EBREAK(InstBinary const& binAddr);
  ~ISA_RV32C_EBREAK() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  ConstField _FUNCT5;
  ConstField _FUNCT5_1;
  ConstField _FUNCT1;
  ConstField _FUNCT3;
};

class ISA_RV32C_JALR : public Rv32Instruction {
 public:
  explicit ISA_RV32C_JALR(uint64_t _RS1_ );
  explicit ISA_RV32C_JALR(InstBinary const& binAddr);
  ~ISA_RV32C_JALR() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  ConstField _FUNCT5;
  Config _RS1;
  ConstField _FUNCT1;
  ConstField _FUNCT3;
};

class ISA_RV32C_ADD : public Rv32Instruction {
 public:
  explicit ISA_RV32C_ADD(uint64_t _RS2_, uint64_t _RD_ );
  explicit ISA_RV32C_ADD(InstBinary const& binAddr);
  ~ISA_RV32C_ADD() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RS2;
  Config _RD;
  ConstField _FUNCT1;
  ConstField _FUNCT3;
};

class ISA_RV32C_FSDSP : public Rv32Instruction {
 public:
  explicit ISA_RV32C_FSDSP(uint64_t _RS2_, uint64_t _IMM3_8_6_, uint64_t _IMM3_5_3_ );
  explicit ISA_RV32C_FSDSP(InstBinary const& binAddr);
  ~ISA_RV32C_FSDSP() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RS2;
  Config _IMM3_8_6;
  Config _IMM3_5_3;
  ConstField _FUNCT3;
};

class ISA_RV32C_SWSP : public Rv32Instruction {
 public:
  explicit ISA_RV32C_SWSP(uint64_t _RS2_, uint64_t _IMM2_7_6_, uint64_t _IMM4_5_2_ );
  explicit ISA_RV32C_SWSP(InstBinary const& binAddr);
  ~ISA_RV32C_SWSP() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RS2;
  Config _IMM2_7_6;
  Config _IMM4_5_2;
  ConstField _FUNCT3;
};

class ISA_RV32C_FSWSP : public Rv32Instruction {
 public:
  explicit ISA_RV32C_FSWSP(uint64_t _RS2_, uint64_t _IMM2_7_6_, uint64_t _IMM4_5_2_ );
  explicit ISA_RV32C_FSWSP(InstBinary const& binAddr);
  ~ISA_RV32C_FSWSP() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RS2;
  Config _IMM2_7_6;
  Config _IMM4_5_2;
  ConstField _FUNCT3;
};

class ISA_RV32I_SRET : public Rv32Instruction {
 public:
  explicit ISA_RV32I_SRET();
  explicit ISA_RV32I_SRET(InstBinary const& binAddr);
  ~ISA_RV32I_SRET() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE1;
  ConstField _OPCODE2;
  ConstField _OPCODE3;
  ConstField _OPCODE4;
  ConstField _OPCODE5;
  ConstField _OPCODE6;
};

class ISA_RV32I_MRET : public Rv32Instruction {
 public:
  explicit ISA_RV32I_MRET();
  explicit ISA_RV32I_MRET(InstBinary const& binAddr);
  ~ISA_RV32I_MRET() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE1;
  ConstField _OPCODE2;
  ConstField _OPCODE3;
  ConstField _OPCODE4;
  ConstField _OPCODE5;
  ConstField _OPCODE6;
};

class ISA_RV32A_LR_W : public Rv32Instruction {
 public:
  explicit ISA_RV32A_LR_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RL_, uint64_t _AQ_ );
  explicit ISA_RV32A_LR_W(InstBinary const& binAddr);
  ~ISA_RV32A_LR_W() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  ConstField _FUNCT5_1;
  Config _RL;
  Config _AQ;
  ConstField _FUNCT5_2;
};

class ISA_RV32A_SC_W : public Rv32Instruction {
 public:
  explicit ISA_RV32A_SC_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ );
  explicit ISA_RV32A_SC_W(InstBinary const& binAddr);
  ~ISA_RV32A_SC_W() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _RL;
  Config _AQ;
  ConstField _FUNCT5;
};

class ISA_RV32A_AMOSWAP_W : public Rv32Instruction {
 public:
  explicit ISA_RV32A_AMOSWAP_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ );
  explicit ISA_RV32A_AMOSWAP_W(InstBinary const& binAddr);
  ~ISA_RV32A_AMOSWAP_W() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _RL;
  Config _AQ;
  ConstField _FUNCT5;
};

class ISA_RV32A_AMOADD_W : public Rv32Instruction {
 public:
  explicit ISA_RV32A_AMOADD_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ );
  explicit ISA_RV32A_AMOADD_W(InstBinary const& binAddr);
  ~ISA_RV32A_AMOADD_W() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _RL;
  Config _AQ;
  ConstField _FUNCT5;
};

class ISA_RV32A_AMOXOR_W : public Rv32Instruction {
 public:
  explicit ISA_RV32A_AMOXOR_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ );
  explicit ISA_RV32A_AMOXOR_W(InstBinary const& binAddr);
  ~ISA_RV32A_AMOXOR_W() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _RL;
  Config _AQ;
  ConstField _FUNCT5;
};

class ISA_RV32A_AMOAND_W : public Rv32Instruction {
 public:
  explicit ISA_RV32A_AMOAND_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ );
  explicit ISA_RV32A_AMOAND_W(InstBinary const& binAddr);
  ~ISA_RV32A_AMOAND_W() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _RL;
  Config _AQ;
  ConstField _FUNCT5;
};

class ISA_RV32A_AMOOR_W : public Rv32Instruction {
 public:
  explicit ISA_RV32A_AMOOR_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ );
  explicit ISA_RV32A_AMOOR_W(InstBinary const& binAddr);
  ~ISA_RV32A_AMOOR_W() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _RL;
  Config _AQ;
  ConstField _FUNCT5;
};

class ISA_RV32A_AMOMIN_W : public Rv32Instruction {
 public:
  explicit ISA_RV32A_AMOMIN_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ );
  explicit ISA_RV32A_AMOMIN_W(InstBinary const& binAddr);
  ~ISA_RV32A_AMOMIN_W() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _RL;
  Config _AQ;
  ConstField _FUNCT5;
};

class ISA_RV32A_AMOMAX_W : public Rv32Instruction {
 public:
  explicit ISA_RV32A_AMOMAX_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ );
  explicit ISA_RV32A_AMOMAX_W(InstBinary const& binAddr);
  ~ISA_RV32A_AMOMAX_W() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _RL;
  Config _AQ;
  ConstField _FUNCT5;
};

class ISA_RV32A_AMOMINU_W : public Rv32Instruction {
 public:
  explicit ISA_RV32A_AMOMINU_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ );
  explicit ISA_RV32A_AMOMINU_W(InstBinary const& binAddr);
  ~ISA_RV32A_AMOMINU_W() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _RL;
  Config _AQ;
  ConstField _FUNCT5;
};

class ISA_RV32A_AMOMAXU_W : public Rv32Instruction {
 public:
  explicit ISA_RV32A_AMOMAXU_W(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_, uint64_t _RL_, uint64_t _AQ_ );
  explicit ISA_RV32A_AMOMAXU_W(InstBinary const& binAddr);
  ~ISA_RV32A_AMOMAXU_W() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  Config _RL;
  Config _AQ;
  ConstField _FUNCT5;
};

class ISA_RV32Zbb_CLZ : public Rv32Instruction {
 public:
  explicit ISA_RV32Zbb_CLZ(uint64_t _RD_, uint64_t _RS1_ );
  explicit ISA_RV32Zbb_CLZ(InstBinary const& binAddr);
  ~ISA_RV32Zbb_CLZ() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  ConstField _FUNCT5;
  ConstField _FUNCT7;
};

class ISA_RV32Zbb_CTZ : public Rv32Instruction {
 public:
  explicit ISA_RV32Zbb_CTZ(uint64_t _RD_, uint64_t _RS1_ );
  explicit ISA_RV32Zbb_CTZ(InstBinary const& binAddr);
  ~ISA_RV32Zbb_CTZ() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  ConstField _FUNCT5;
  ConstField _FUNCT7;
};

class ISA_RV32Zbb_CPOP : public Rv32Instruction {
 public:
  explicit ISA_RV32Zbb_CPOP(uint64_t _RD_, uint64_t _RS1_ );
  explicit ISA_RV32Zbb_CPOP(InstBinary const& binAddr);
  ~ISA_RV32Zbb_CPOP() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  ConstField _FUNCT5;
  ConstField _FUNCT7;
};

class ISA_RV32Zbb_MIN : public Rv32Instruction {
 public:
  explicit ISA_RV32Zbb_MIN(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32Zbb_MIN(InstBinary const& binAddr);
  ~ISA_RV32Zbb_MIN() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32Zbb_MAX : public Rv32Instruction {
 public:
  explicit ISA_RV32Zbb_MAX(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32Zbb_MAX(InstBinary const& binAddr);
  ~ISA_RV32Zbb_MAX() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32Zbb_MINU : public Rv32Instruction {
 public:
  explicit ISA_RV32Zbb_MINU(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32Zbb_MINU(InstBinary const& binAddr);
  ~ISA_RV32Zbb_MINU() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32Zbb_MAXU : public Rv32Instruction {
 public:
  explicit ISA_RV32Zbb_MAXU(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32Zbb_MAXU(InstBinary const& binAddr);
  ~ISA_RV32Zbb_MAXU() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32Zbb_SEXT_B : public Rv32Instruction {
 public:
  explicit ISA_RV32Zbb_SEXT_B(uint64_t _RD_, uint64_t _RS1_ );
  explicit ISA_RV32Zbb_SEXT_B(InstBinary const& binAddr);
  ~ISA_RV32Zbb_SEXT_B() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  ConstField _FUNCT5;
  ConstField _FUNCT7;
};

class ISA_RV32Zbb_SEXT_H : public Rv32Instruction {
 public:
  explicit ISA_RV32Zbb_SEXT_H(uint64_t _RD_, uint64_t _RS1_ );
  explicit ISA_RV32Zbb_SEXT_H(InstBinary const& binAddr);
  ~ISA_RV32Zbb_SEXT_H() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  ConstField _FUNCT5;
  ConstField _FUNCT7;
};

class ISA_RV32Zbb_ZEXT_H : public Rv32Instruction {
 public:
  explicit ISA_RV32Zbb_ZEXT_H(uint64_t _RD_, uint64_t _RS1_ );
  explicit ISA_RV32Zbb_ZEXT_H(InstBinary const& binAddr);
  ~ISA_RV32Zbb_ZEXT_H() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  ConstField _FUNCT5;
  ConstField _FUNCT7;
};

class ISA_RV32Zbb_ANDN : public Rv32Instruction {
 public:
  explicit ISA_RV32Zbb_ANDN(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32Zbb_ANDN(InstBinary const& binAddr);
  ~ISA_RV32Zbb_ANDN() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32Zbb_ORN : public Rv32Instruction {
 public:
  explicit ISA_RV32Zbb_ORN(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32Zbb_ORN(InstBinary const& binAddr);
  ~ISA_RV32Zbb_ORN() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32Zbb_XNOR : public Rv32Instruction {
 public:
  explicit ISA_RV32Zbb_XNOR(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32Zbb_XNOR(InstBinary const& binAddr);
  ~ISA_RV32Zbb_XNOR() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32Zbb_ROL : public Rv32Instruction {
 public:
  explicit ISA_RV32Zbb_ROL(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32Zbb_ROL(InstBinary const& binAddr);
  ~ISA_RV32Zbb_ROL() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32Zbb_ROR : public Rv32Instruction {
 public:
  explicit ISA_RV32Zbb_ROR(uint64_t _RD_, uint64_t _RS1_, uint64_t _RS2_ );
  explicit ISA_RV32Zbb_ROR(InstBinary const& binAddr);
  ~ISA_RV32Zbb_ROR() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _RS2;
  ConstField _FUNCT7;
};

class ISA_RV32Zbb_RORI : public Rv32Instruction {
 public:
  explicit ISA_RV32Zbb_RORI(uint64_t _RD_, uint64_t _RS1_, uint64_t _IMM_ );
  explicit ISA_RV32Zbb_RORI(InstBinary const& binAddr);
  ~ISA_RV32Zbb_RORI() {}
  virtual void RunOnInstance(Instance* instance) override;
  void InstInit();


 private:
  ConstField _OPCODE;
  Config _RD;
  ConstField _FUNCT3;
  Config _RS1;
  Config _IMM;
  ConstField _FUNCT7;
};
