#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
WriteSr(_RD, uint32_t(int32_t(rs1) >> _IMM5));
#endif

#ifdef TEST_DEFINE
TEST_CASE("arithmetic shift of negative by 1") {
  SR[6].Set<int32_t>(static_cast<int32_t>(-4));
  RUN_INST(ISA_RV32I_SRAI, 7, 6, 1);
  CHECK_REGISTER_EQ(int32_t, SR[7], -2);
}

TEST_CASE("arithmetic shift of negative by 31 yields -1") {
  SR[6].Set<int32_t>(static_cast<int32_t>(-1));
  RUN_INST(ISA_RV32I_SRAI, 7, 6, 31);
  CHECK_REGISTER_EQ(int32_t, SR[7], -1);
}

TEST_CASE("arithmetic shift of positive by 1") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x03));
  RUN_INST(ISA_RV32I_SRAI, 7, 6, 1);
  CHECK_REGISTER_EQ(int32_t, SR[7], 0x01);
}
#endif
