#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
WriteSr(_RD, uint32_t(int32_t(rs1) + int32_t(rs2)));
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic positive add") {
  SR[5].Set<int32_t>(static_cast<int32_t>(10));
  SR[6].Set<int32_t>(static_cast<int32_t>(11));
  RUN_INST(ISA_RV32I_ADD, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], 21);
}

TEST_CASE("mixed-sign operands") {
  SR[5].Set<int32_t>(static_cast<int32_t>(-7));
  SR[6].Set<int32_t>(static_cast<int32_t>(3));
  RUN_INST(ISA_RV32I_ADD, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], -4);
}

TEST_CASE("overflow wraps to INT32_MIN") {
  SR[5].Set<int32_t>(static_cast<int32_t>(INT32_MAX));
  SR[6].Set<int32_t>(static_cast<int32_t>(1));
  RUN_INST(ISA_RV32I_ADD, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], INT32_MIN);
}
#endif
