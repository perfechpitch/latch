#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
WriteSr(_RD, uint32_t((int32_t(rs1) < _IMM12.S32()) ? 1 : 0));
#endif

#ifdef TEST_DEFINE
TEST_CASE("positive: 10 < 50 is true") {
  SR[6].Set<int32_t>(static_cast<int32_t>(10));
  RUN_INST(ISA_RV32I_SLTI, 7, 6, 50);
  CHECK_REGISTER_EQ(int32_t, SR[7], 1);
}

TEST_CASE("signed: -10 < -50 is false (negative immediate)") {
  SR[6].Set<int32_t>(static_cast<int32_t>(-10));
  RUN_INST(ISA_RV32I_SLTI, 7, 6, -50);
  CHECK_REGISTER_EQ(int32_t, SR[7], 0);
}

TEST_CASE("signed: -100 < -50 is true (negative immediate sign-extended)") {
  SR[6].Set<int32_t>(static_cast<int32_t>(-100));
  RUN_INST(ISA_RV32I_SLTI, 7, 6, -50);
  CHECK_REGISTER_EQ(int32_t, SR[7], 1);
}
#endif
