#ifdef INST_RUN_DEFINE
uint32_t rd_r = ReadSr(_RD + 8);
WriteSr(_RD + 8, uint32_t(int32_t(rd_r) >> CONCAT(_IMM1, _IMM5).U32()));
#endif

#ifdef TEST_DEFINE
TEST_CASE("arithmetic shift by 1 of negative preserves sign") {
  SR[6 + 8].Set<uint32_t>(static_cast<uint32_t>(0x80000000));
  RUN_INST(ISA_RV32C_SRAI, 1, 6, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[6 + 8], 0xC0000000);
}

TEST_CASE("arithmetic shift by 4 of negative") {
  SR[6 + 8].Set<uint32_t>(static_cast<uint32_t>(0xFFFF0000));
  RUN_INST(ISA_RV32C_SRAI, 4, 6, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[6 + 8], 0xFFFFF000);
}

TEST_CASE("arithmetic shift by 4 of positive") {
  SR[6 + 8].Set<uint32_t>(static_cast<uint32_t>(0x0FFFF000));
  RUN_INST(ISA_RV32C_SRAI, 4, 6, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[6 + 8], 0x00FFFF00);
}

TEST_CASE("arithmetic shift by 31 of negative yields all-ones") {
  SR[6 + 8].Set<uint32_t>(static_cast<uint32_t>(0x80000000));
  RUN_INST(ISA_RV32C_SRAI, 31, 6, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[6 + 8], 0xFFFFFFFF);
}
#endif
