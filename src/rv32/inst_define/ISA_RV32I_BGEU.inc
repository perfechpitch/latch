#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
if (rs1 >= rs2) {
  PC = PC + CONCAT(_IMM1_U0, _IMM1, _IMM6, _IMM4).S32() * 2;
  BRANCH;
}
#endif

#ifdef TEST_DEFINE
TEST_CASE("branch taken forward: 0xFFFFFFFF >=u 1 (false if signed)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0xFFFFFFFF));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(1));
  RUN_INST(ISA_RV32I_BGEU, 0, 4, 5, 6, 0, 0);
  CHECK_REGISTER_EQ(uint32_t, PC, (uint32_t)InitPC + 8);
}

TEST_CASE("branch taken on equal (rs1 == rs2 satisfies >=)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0xDEAD));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0xDEAD));
  RUN_INST(ISA_RV32I_BGEU, 0, 4, 5, 6, 0, 0);
  CHECK_REGISTER_EQ(uint32_t, PC, (uint32_t)InitPC + 8);
}

TEST_CASE("branch taken backward (offset = -4)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0b00001111));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0b00001110));
  RUN_INST(ISA_RV32I_BGEU, 0x01, -2, 5, 6, 0x3f, 0x01);
  CHECK_REGISTER_EQ(uint32_t, PC, (uint32_t)InitPC - 4);
}

TEST_CASE("branch NOT taken: 1 is NOT >=u 0xFFFFFFFF") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(1));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0xFFFFFFFF));
  RUN_INST(ISA_RV32I_BGEU, 0, 4, 5, 6, 0, 0);
  CHECK_REGISTER_EQ(uint32_t, PC, (uint32_t)InitPC + 4);
}
#endif
