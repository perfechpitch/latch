#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
int32_t a = int32_t(rs1);
int32_t b = int32_t(rs2);
if (b == 0) {
  WriteSr(_RD, uint32_t(-1));
} else if (a == int32_t(0x80000000) && b == -1) {
  WriteSr(_RD, uint32_t(0x80000000));
} else {
  WriteSr(_RD, uint32_t(a / b));
}
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic positive / positive") {
  SR[5].Set<int32_t>(static_cast<int32_t>(100));
  SR[6].Set<int32_t>(static_cast<int32_t>(2));
  RUN_INST(ISA_RV32I_DIV, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], 50);
}

TEST_CASE("divide by zero returns -1") {
  SR[5].Set<int32_t>(static_cast<int32_t>(123));
  SR[6].Set<int32_t>(static_cast<int32_t>(0));
  RUN_INST(ISA_RV32I_DIV, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], -1);
}

TEST_CASE("overflow INT_MIN / -1 returns INT_MIN") {
  SR[5].Set<int32_t>(static_cast<int32_t>(INT32_MIN));
  SR[6].Set<int32_t>(static_cast<int32_t>(-1));
  RUN_INST(ISA_RV32I_DIV, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], INT32_MIN);
}

TEST_CASE("negative / negative") {
  SR[5].Set<int32_t>(static_cast<int32_t>(-42));
  SR[6].Set<int32_t>(static_cast<int32_t>(-7));
  RUN_INST(ISA_RV32I_DIV, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], 6);
}
#endif
