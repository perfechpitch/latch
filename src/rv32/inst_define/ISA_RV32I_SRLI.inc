#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
WriteSr(_RD, uint32_t(rs1 >> _IMM5));
#endif

#ifdef TEST_DEFINE
TEST_CASE("shift right by 1") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x06));
  RUN_INST(ISA_RV32I_SRLI, 7, 6, 1);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x03u);
}

TEST_CASE("shift right by 31 of sign-bit value fills with zero") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x80000000u));
  RUN_INST(ISA_RV32I_SRLI, 7, 6, 31);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 1u);
}
#endif
