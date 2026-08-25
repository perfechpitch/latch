#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
if (int32_t(rs1) < int32_t(rs2)) {
  PC = PC + CONCAT(_IMM1_U0, _IMM1, _IMM6, _IMM4).S32() * 2;
  BRANCH;
}
#endif

#ifdef TEST_DEFINE
TEST_CASE("branch taken forward: -1 < 1 (signed)") {
  SR[5].Set<int32_t>(static_cast<int32_t>(-1));
  SR[6].Set<int32_t>(static_cast<int32_t>(1));
  RUN_INST(ISA_RV32I_BLT, 0, 4, 5, 6, 0, 0);
  CHECK_REGISTER_EQ(uint32_t, PC, (uint32_t)InitPC + 8);
}

TEST_CASE("branch taken backward: -6 < -4 (signed)") {
  SR[5].Set<int32_t>(static_cast<int32_t>(-6));
  SR[6].Set<int32_t>(static_cast<int32_t>(-4));
  RUN_INST(ISA_RV32I_BLT, 0x01, -2, 5, 6, 0x3f, 0x01);
  CHECK_REGISTER_EQ(uint32_t, PC, (uint32_t)InitPC - 4);
}

TEST_CASE("branch NOT taken: 1 is NOT < -1 (signed)") {
  SR[5].Set<int32_t>(static_cast<int32_t>(1));
  SR[6].Set<int32_t>(static_cast<int32_t>(-1));
  RUN_INST(ISA_RV32I_BLT, 0, 4, 5, 6, 0, 0);
  CHECK_REGISTER_EQ(uint32_t, PC, (uint32_t)InitPC + 4);
}
#endif
