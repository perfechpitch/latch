#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
int32_t a = int32_t(rs1);
int32_t b = int32_t(rs2);
if (b == 0) {
  WriteSr(_RD, uint32_t(a));
} else if (a == int32_t(0x80000000) && b == -1) {
  WriteSr(_RD, uint32_t(0));
} else {
  WriteSr(_RD, uint32_t(a % b));
}
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic positive % positive") {
  SR[5].Set<int32_t>(static_cast<int32_t>(10));
  SR[6].Set<int32_t>(static_cast<int32_t>(3));
  RUN_INST(ISA_RV32I_REM, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], 1);
}

TEST_CASE("rem by zero returns dividend") {
  SR[5].Set<int32_t>(static_cast<int32_t>(12345));
  SR[6].Set<int32_t>(static_cast<int32_t>(0));
  RUN_INST(ISA_RV32I_REM, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], 12345);
}

TEST_CASE("overflow INT_MIN % -1 returns 0") {
  SR[5].Set<int32_t>(static_cast<int32_t>(INT32_MIN));
  SR[6].Set<int32_t>(static_cast<int32_t>(-1));
  RUN_INST(ISA_RV32I_REM, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], 0);
}
#endif
