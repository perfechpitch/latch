#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
WriteSr(_RD, uint32_t((rs1 < static_cast<uint32_t>(_IMM12.S32())) ? 1 : 0));
#endif

#ifdef TEST_DEFINE
TEST_CASE("unsigned: 10 < 50 is true") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(10));
  RUN_INST(ISA_RV32I_SLTIU, 7, 6, 50);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 1u);
}

TEST_CASE("unsigned: 60 < 50 is false") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(60));
  RUN_INST(ISA_RV32I_SLTIU, 7, 6, 50);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0u);
}

TEST_CASE("imm=-1 sign-extends to 0xFFFFFFFF; any non-max rs1 is less") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(1));
  RUN_INST(ISA_RV32I_SLTIU, 7, 6, -1);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 1u);
}
#endif
