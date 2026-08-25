#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t value = rs1;
WriteSr(_RD, uint32_t(value == 0 ? 32 : __builtin_ctz(value)));
#endif

#ifdef TEST_DEFINE
TEST_CASE("zero input yields 32 trailing zeros") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x00000000));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_CTZ, 6, 5);
  CHECK_REGISTER_EQ(uint32_t, SR[6], 32);
}

TEST_CASE("bit 0 set yields 0 trailing zeros") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x00000001));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_CTZ, 6, 5);
  CHECK_REGISTER_EQ(uint32_t, SR[6], 0);
}

TEST_CASE("bit 16 set yields 16 trailing zeros") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x00010000));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_CTZ, 6, 5);
  CHECK_REGISTER_EQ(uint32_t, SR[6], 16);
}
#endif
