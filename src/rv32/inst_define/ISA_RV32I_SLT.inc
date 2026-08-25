#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
WriteSr(_RD, uint32_t((int32_t(rs1) < int32_t(rs2)) ? 1 : 0));
#endif

#ifdef TEST_DEFINE
TEST_CASE("signed: -1 < 1 is true (distinguishes from SLTU)") {
  SR[5].Set<int32_t>(static_cast<int32_t>(-1));
  SR[6].Set<int32_t>(static_cast<int32_t>(1));
  RUN_INST(ISA_RV32I_SLT, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], 1);
}

TEST_CASE("signed: 1 < -1 is false") {
  SR[5].Set<int32_t>(static_cast<int32_t>(1));
  SR[6].Set<int32_t>(static_cast<int32_t>(-1));
  RUN_INST(ISA_RV32I_SLT, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], 0);
}

TEST_CASE("equal operands yield 0") {
  SR[5].Set<int32_t>(static_cast<int32_t>(42));
  SR[6].Set<int32_t>(static_cast<int32_t>(42));
  RUN_INST(ISA_RV32I_SLT, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], 0);
}
#endif
