#ifdef INST_RUN_DEFINE
uint32_t rd_r = ReadSr(_RD);
uint32_t rs2_r = ReadSr(_RS2);
WriteSr(_RD, uint32_t(int32_t(rd_r) + int32_t(rs2_r)));
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic positive add") {
  SR[5].Set<int32_t>(static_cast<int32_t>(10));
  SR[7].Set<int32_t>(static_cast<int32_t>(11));
  RUN_INST(ISA_RV32C_ADD, 5, 7);
  CHECK_REGISTER_EQ(int32_t, SR[7], 21);
}

TEST_CASE("mixed-sign operands") {
  SR[5].Set<int32_t>(static_cast<int32_t>(-3));
  SR[7].Set<int32_t>(static_cast<int32_t>(5));
  RUN_INST(ISA_RV32C_ADD, 5, 7);
  CHECK_REGISTER_EQ(int32_t, SR[7], 2);
}

TEST_CASE("overflow wraps to INT32_MIN") {
  SR[5].Set<int32_t>(static_cast<int32_t>(1));
  SR[7].Set<int32_t>(static_cast<int32_t>(INT32_MAX));
  RUN_INST(ISA_RV32C_ADD, 5, 7);
  CHECK_REGISTER_EQ(int32_t, SR[7], INT32_MIN);
}
#endif
