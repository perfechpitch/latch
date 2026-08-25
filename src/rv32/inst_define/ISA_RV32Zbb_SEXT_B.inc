#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t v = rs1;
uint8_t  b = v & 0xFF;
WriteSr(_RD, uint32_t(Bits(b).S32()));
#endif

#ifdef TEST_DEFINE
TEST_CASE("positive low byte (sign bit clear)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x0000007F));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_SEXT_B, 7, 5);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x0000007F);
}

TEST_CASE("negative low byte (sign bit set)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x000000A0));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_SEXT_B, 7, 5);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xFFFFFFA0);
}

TEST_CASE("high bytes set but ignored") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0xDEADBEFF));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_SEXT_B, 7, 5);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xFFFFFFFF);
}
#endif
