#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
if (rs1 < rs2) {
  PC = PC + CONCAT(_IMM1_U0, _IMM1, _IMM6, _IMM4).S32() * 2;
  BRANCH;
}
#endif

#ifdef TEST_DEFINE
TEST_CASE("branch taken forward: 11 <u 12") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(11));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(12));
  RUN_INST(ISA_RV32I_BLTU, 0, 4, 5, 6, 0, 0);
  CHECK_REGISTER_EQ(uint32_t, PC, (uint32_t)InitPC + 8);
}

TEST_CASE("branch taken backward (offset = -4)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(1));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(2));
  RUN_INST(ISA_RV32I_BLTU, 0x01, -2, 5, 6, 0x3f, 0x01);
  CHECK_REGISTER_EQ(uint32_t, PC, (uint32_t)InitPC - 4);
}

TEST_CASE("branch NOT taken: 0xFFFFFFFF is NOT <u 1 (would be true if signed)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0xFFFFFFFF));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(1));
  RUN_INST(ISA_RV32I_BLTU, 0, 4, 5, 6, 0, 0);
  CHECK_REGISTER_EQ(uint32_t, PC, (uint32_t)InitPC + 4);
}
#endif
