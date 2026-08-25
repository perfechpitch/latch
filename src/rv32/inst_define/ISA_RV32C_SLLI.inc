#ifdef INST_RUN_DEFINE
uint32_t rd_r = ReadSr(_RD);
WriteSr(_RD, uint32_t(rd_r << CONCAT(_IMM1, _IMM5).U32()));
#endif

#ifdef TEST_DEFINE
TEST_CASE("small shift") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0b00000011));
  RUN_INST(ISA_RV32C_SLLI, 1, 6, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[6], 0b00000011u << 1);
}

TEST_CASE("shift by 18") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0b00000011));
  RUN_INST(ISA_RV32C_SLLI, 18, 6, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[6], 0b00000011u << 18);
}

TEST_CASE("shift by 31 leaves only top bit of low bit") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x00000001));
  RUN_INST(ISA_RV32C_SLLI, 31, 6, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[6], 0x80000000);
}
#endif
