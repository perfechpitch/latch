#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
WriteSr(_RD, uint32_t(int32_t(rs1) - int32_t(rs2)));
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic subtract") {
  SR[5].Set<int32_t>(static_cast<int32_t>(11));
  SR[6].Set<int32_t>(static_cast<int32_t>(10));
  RUN_INST(ISA_RV32I_SUB, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], 1);
}

TEST_CASE("negative result") {
  SR[5].Set<int32_t>(static_cast<int32_t>(10));
  SR[6].Set<int32_t>(static_cast<int32_t>(11));
  RUN_INST(ISA_RV32I_SUB, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], -1);
}

TEST_CASE("mixed-sign subtract") {
  SR[5].Set<int32_t>(static_cast<int32_t>(-5));
  SR[6].Set<int32_t>(static_cast<int32_t>(3));
  RUN_INST(ISA_RV32I_SUB, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], -8);
}

TEST_CASE("zero minus one wraps to -1") {
  SR[5].Set<int32_t>(static_cast<int32_t>(0));
  SR[6].Set<int32_t>(static_cast<int32_t>(1));
  RUN_INST(ISA_RV32I_SUB, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], -1);
}
#endif
