#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
WriteSr(_RD, uint32_t(int32_t(rs1) >> (rs2 & 0x1F)));
#endif

#ifdef TEST_DEFINE
TEST_CASE("arithmetic shift of negative by 1") {
  SR[5].Set<int32_t>(static_cast<int32_t>(-10));
  SR[6].Set<int32_t>(static_cast<int32_t>(1));
  RUN_INST(ISA_RV32I_SRA, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], -5);
}

TEST_CASE("arithmetic shift of negative by 31 fills with sign bit") {
  SR[5].Set<int32_t>(static_cast<int32_t>(-1));
  SR[6].Set<int32_t>(static_cast<int32_t>(31));
  RUN_INST(ISA_RV32I_SRA, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], -1);
}

TEST_CASE("shift amount masked to low 5 bits (35 -> 3)") {
  SR[5].Set<int32_t>(static_cast<int32_t>(-64));
  SR[6].Set<int32_t>(static_cast<int32_t>(35));
  RUN_INST(ISA_RV32I_SRA, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], -8);
}
#endif
