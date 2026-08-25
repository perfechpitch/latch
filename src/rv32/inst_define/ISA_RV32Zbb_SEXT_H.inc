#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t v = rs1;
uint16_t h = v & 0xFFFF;
WriteSr(_RD, uint32_t(Bits(h).S32()));
#endif

#ifdef TEST_DEFINE
TEST_CASE("positive halfword (sign bit clear)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x00007FFF));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_SEXT_H, 7, 5);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x00007FFF);
}

TEST_CASE("negative halfword (sign bit set)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x000080A0));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_SEXT_H, 7, 5);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xFFFF80A0);
}

TEST_CASE("high halfword set but ignored") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0xDEAD1234));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_SEXT_H, 7, 5);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x00001234);
}
#endif
