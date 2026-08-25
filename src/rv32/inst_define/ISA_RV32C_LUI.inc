#ifdef INST_RUN_DEFINE
uint32_t imm = static_cast<uint32_t>(CONCAT(_IMM1, _IMM5).S32()) << 12;
if (imm != 0) WriteSr(_RD, imm);
#endif

#ifdef TEST_DEFINE
TEST_CASE("negative immediate writes sign-extended upper") {
  SR[6].Set<int32_t>(static_cast<int32_t>(10));
  RUN_INST(ISA_RV32C_LUI, 2, 6, 1);
  CHECK_REGISTER_EQ(uint32_t, SR[6], 0xFFFE2000);
}

TEST_CASE("positive immediate") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0));
  RUN_INST(ISA_RV32C_LUI, 1, 6, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[6], 0x00001000);
}

TEST_CASE("zero immediate leaves rd unchanged (reserved encoding)") {
  SR[6].Set<int32_t>(static_cast<int32_t>(11));
  RUN_INST(ISA_RV32C_LUI, 0, 6, 0);
  CHECK_REGISTER_EQ(int32_t, SR[6], 11);
}
#endif
