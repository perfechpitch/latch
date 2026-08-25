#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
WriteSr(_RD, uint32_t(int32_t(rs1) ^ int32_t(_IMM12.S32())));
#endif

#ifdef TEST_DEFINE
TEST_CASE("flip selected bits") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x0F));
  RUN_INST(ISA_RV32I_XORI, 7, 5, 0xC3);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xCCu);
}

TEST_CASE("XOR with -1 (sign-extended 0xFFFFFFFF) inverts all bits") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x12345678u));
  RUN_INST(ISA_RV32I_XORI, 7, 5, -1);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xEDCBA987u);
}

TEST_CASE("XOR with zero is identity") {
  SR[5].Set<int32_t>(static_cast<int32_t>(0x55));
  RUN_INST(ISA_RV32I_XORI, 7, 5, 0);
  CHECK_REGISTER_EQ(int32_t, SR[7], 0x55);
}
#endif
