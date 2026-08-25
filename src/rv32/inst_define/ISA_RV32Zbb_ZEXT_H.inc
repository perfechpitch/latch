
#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
WriteSr(_RD, uint32_t(rs1 & 0xFFFF));
#endif

#ifdef TEST_DEFINE
TEST_CASE("high half set is zeroed") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0xDEAD1234));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_ZEXT_H, 7, 5);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x00001234);
}

TEST_CASE("only low half set is unchanged") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x0000FFFF));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_ZEXT_H, 7, 5);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x0000FFFF);
}

TEST_CASE("low half zero with high half set yields zero") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0xABCD0000));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_ZEXT_H, 7, 5);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x00000000);
}
#endif
