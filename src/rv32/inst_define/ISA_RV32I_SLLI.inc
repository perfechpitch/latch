#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
WriteSr(_RD, uint32_t(rs1 << _IMM5));
#endif

#ifdef TEST_DEFINE
TEST_CASE("shift left by 1") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x03));
  RUN_INST(ISA_RV32I_SLLI, 7, 6, 1);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x06u);
}

TEST_CASE("shift left by 31 moves LSB to MSB") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(1));
  RUN_INST(ISA_RV32I_SLLI, 7, 6, 31);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x80000000u);
}
#endif
