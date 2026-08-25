#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1 + 8);
if (rs1 != 0) {
  PC = PC + CONCAT(_IMM1_8, _IMM2_6_7, _IMM1_5, _IMM2_3_4, _IMM2_2_1).S32() * 2;
  BRANCH;
}
#endif

#ifdef TEST_DEFINE
TEST_CASE("taken with positive offset") {
  PC.Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  SR[6 + 8].Set<uint32_t>(static_cast<uint32_t>(0x0000000F));
  RUN_INST(ISA_RV32C_BNEZ, 1, 2, 0, 6, 1, 0);
  CHECK_REGISTER_EQ(uint32_t, PC, 0x00800000u + 44);
}

TEST_CASE("taken with negative offset") {
  PC.Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  SR[6 + 8].Set<uint32_t>(static_cast<uint32_t>(0x0000000F));
  RUN_INST(ISA_RV32C_BNEZ, 1, 2, 0, 6, 1, 1);
  CHECK_REGISTER_EQ(uint32_t, PC, 0x00800000u - 212);
}

TEST_CASE("not taken falls through (pc += 2)") {
  PC.Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  SR[6 + 8].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32C_BNEZ, 1, 2, 0, 6, 1, 1);
  CHECK_REGISTER_EQ(uint32_t, PC, 0x00800000u + 2);
}
#endif
