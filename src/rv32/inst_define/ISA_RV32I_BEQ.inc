#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
if (rs1 == rs2) {
  PC = PC + CONCAT(_IMM1_U0, _IMM1, _IMM6, _IMM4).S32() * 2;
  BRANCH;
}
#endif

#ifdef TEST_DEFINE
TEST_CASE("branch taken forward (rs1 == rs2, offset = +8)") {
  SR[5].Set<int32_t>(static_cast<int32_t>(0b00001111));
  SR[6].Set<int32_t>(static_cast<int32_t>(0b00001111));
  RUN_INST(ISA_RV32I_BEQ, 0, 4, 5, 6, 0, 0);
  CHECK_REGISTER_EQ(uint32_t, PC, (uint32_t)InitPC + 8);
}

TEST_CASE("branch taken backward (offset = -4)") {
  SR[5].Set<int32_t>(static_cast<int32_t>(0b00001111));
  SR[6].Set<int32_t>(static_cast<int32_t>(0b00001111));
  RUN_INST(ISA_RV32I_BEQ, 0x01, -2, 5, 6, 0x3f, 0x01);
  CHECK_REGISTER_EQ(uint32_t, PC, (uint32_t)InitPC - 4);
}

TEST_CASE("branch NOT taken (rs1 != rs2) -> PC += 4") {
  SR[5].Set<int32_t>(static_cast<int32_t>(0b00001111));
  SR[6].Set<int32_t>(static_cast<int32_t>(0b00001110));
  RUN_INST(ISA_RV32I_BEQ, 0, 2, 5, 6, 0, 0);
  CHECK_REGISTER_EQ(uint32_t, PC, (uint32_t)InitPC + 4);
}
#endif
