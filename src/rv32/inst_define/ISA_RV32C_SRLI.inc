#ifdef INST_RUN_DEFINE
uint32_t rd_r = ReadSr(_RD + 8);
WriteSr(_RD + 8, uint32_t(rd_r >> CONCAT(_IMM1, _IMM5).U32()));
#endif

#ifdef TEST_DEFINE
TEST_CASE("shift by 1") {
  SR[6 + 8].Set<uint32_t>(static_cast<uint32_t>(0b10000011));
  RUN_INST(ISA_RV32C_SRLI, 1, 6, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[6 + 8], 0b10000011u >> 1);
}

TEST_CASE("shift by 5 drops low bits") {
  SR[6 + 8].Set<uint32_t>(static_cast<uint32_t>(0b10000011));
  RUN_INST(ISA_RV32C_SRLI, 5, 6, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[6 + 8], 0b10000011u >> 5);
}

TEST_CASE("logical shift by 31 yields only top bit") {
  SR[6 + 8].Set<uint32_t>(static_cast<uint32_t>(0x80000000));
  RUN_INST(ISA_RV32C_SRLI, 31, 6, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[6 + 8], 0x00000001);
}
#endif
