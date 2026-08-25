#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
WriteSr(_RD, uint32_t(int32_t(rs1) & int32_t(_IMM12.S32())));
#endif

#ifdef TEST_DEFINE
TEST_CASE("mask lower bits") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x0F));
  RUN_INST(ISA_RV32I_ANDI, 7, 6, 0x3C);
  CHECK_REGISTER_EQ(int32_t, SR[7], 0x0C);
}

TEST_CASE("negative immediate sign-extends to all-ones in upper bits") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0xDEADBEEFu));
  RUN_INST(ISA_RV32I_ANDI, 7, 6, -1);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xDEADBEEFu);
}

TEST_CASE("AND with zero clears all bits") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x12345678));
  RUN_INST(ISA_RV32I_ANDI, 7, 6, 0);
  CHECK_REGISTER_EQ(int32_t, SR[7], 0);
}
#endif
