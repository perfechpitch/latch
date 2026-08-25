#ifdef INST_RUN_DEFINE
WriteSr(_RD, uint32_t(PC + 4));
PC = PC + CONCAT(_IMM1_U0, _IMM8, _IMM1, _IMM10).S32() * 2;
BRANCH;
#endif

#ifdef TEST_DEFINE
TEST_CASE("forward branch (offset = +16)") {
  RUN_INST(ISA_RV32I_JAL, 7, 0, 0, 0x08, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[7], (uint32_t)InitPC + 4);
  CHECK_REGISTER_EQ(uint32_t, PC, (uint32_t)InitPC + 16);
}

TEST_CASE("backward branch (offset = -4)") {
  RUN_INST(ISA_RV32I_JAL, 7, 0xFF, 1, 0x3FE, 1);
  CHECK_REGISTER_EQ(uint32_t, SR[7], (uint32_t)InitPC + 4);
  CHECK_REGISTER_EQ(uint32_t, PC, (uint32_t)InitPC - 4);
}

TEST_CASE("rd captures return address (PC + 4) before jump") {
  RUN_INST(ISA_RV32I_JAL, 3, 0, 0, 0x10, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[3], (uint32_t)InitPC + 4);
  CHECK_REGISTER_EQ(uint32_t, PC, (uint32_t)InitPC + 32);
}
#endif
