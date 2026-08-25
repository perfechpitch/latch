#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
WriteSr(_RD, uint32_t(rs1 >> (rs2 & 0x1F)));
#endif

#ifdef TEST_DEFINE
TEST_CASE("shift right by 1") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(10));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(1));
  RUN_INST(ISA_RV32I_SRL, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 5u);
}

TEST_CASE("shift right by 31 of sign-bit value") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x80000000u));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(31));
  RUN_INST(ISA_RV32I_SRL, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 1u);
}

TEST_CASE("shift amount masked to low 5 bits (35 -> 3)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x80u));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(35));
  RUN_INST(ISA_RV32I_SRL, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x10u);
}
#endif
