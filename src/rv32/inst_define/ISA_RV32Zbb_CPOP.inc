#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t value = rs1;
WriteSr(_RD, uint32_t(__builtin_popcount(value)));
#endif

#ifdef TEST_DEFINE
TEST_CASE("zero input yields population count 0") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x00000000));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_CPOP, 6, 5);
  CHECK_REGISTER_EQ(uint32_t, SR[6], 0);
}

TEST_CASE("all ones yields population count 32") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0xFFFFFFFF));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_CPOP, 6, 5);
  CHECK_REGISTER_EQ(uint32_t, SR[6], 32);
}

TEST_CASE("alternating nibble pattern 0x0F0F0F0F yields 16") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x0F0F0F0F));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_CPOP, 6, 5);
  CHECK_REGISTER_EQ(uint32_t, SR[6], 16);
}
#endif
