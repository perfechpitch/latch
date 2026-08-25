#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
int32_t a = int32_t(rs1);
int32_t b = int32_t(rs2);
WriteSr(_RD, uint32_t((a < b) ? a : b));
#endif

#ifdef TEST_DEFINE
TEST_CASE("rs1 greater than rs2") {
  SR[5].Set<int32_t>(static_cast<int32_t>(100));
  SR[6].Set<int32_t>(static_cast<int32_t>(50));
  SR[7].Set<int32_t>(static_cast<int32_t>(0));
  RUN_INST(ISA_RV32Zbb_MIN, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], 50);
}

TEST_CASE("rs1 less than rs2") {
  SR[5].Set<int32_t>(static_cast<int32_t>(7));
  SR[6].Set<int32_t>(static_cast<int32_t>(42));
  SR[7].Set<int32_t>(static_cast<int32_t>(0));
  RUN_INST(ISA_RV32Zbb_MIN, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], 7);
}

TEST_CASE("rs1 equals rs2") {
  SR[5].Set<int32_t>(static_cast<int32_t>(33));
  SR[6].Set<int32_t>(static_cast<int32_t>(33));
  SR[7].Set<int32_t>(static_cast<int32_t>(0));
  RUN_INST(ISA_RV32Zbb_MIN, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], 33);
}

TEST_CASE("signed comparison: negative vs positive") {
  SR[5].Set<int32_t>(static_cast<int32_t>(-10));
  SR[6].Set<int32_t>(static_cast<int32_t>(5));
  SR[7].Set<int32_t>(static_cast<int32_t>(0));
  RUN_INST(ISA_RV32Zbb_MIN, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], -10);
}
#endif
