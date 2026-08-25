#ifdef INST_RUN_DEFINE
uint32_t rd_r = ReadSr(_RD + 8);
WriteSr(_RD + 8, uint32_t(rd_r & static_cast<uint32_t>(CONCAT(_IMM1, _IMM5).S32())));
#endif

#ifdef TEST_DEFINE
TEST_CASE("negative immediate masks high bits") {
  SR[6 + 8].Set<uint32_t>(static_cast<uint32_t>(0x0000000A));
  RUN_INST(ISA_RV32C_ANDI, 2, 6, 1);
  CHECK_REGISTER_EQ(uint32_t, SR[6 + 8], 0x0000000A & 0xFFFFFFE2);
}

TEST_CASE("positive immediate") {
  SR[6 + 8].Set<uint32_t>(static_cast<uint32_t>(0xFFFFFFFF));
  RUN_INST(ISA_RV32C_ANDI, 0x0F, 6, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[6 + 8], 0x0000000F);
}
#endif
