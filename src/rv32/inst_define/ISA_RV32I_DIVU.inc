#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
uint32_t a = rs1;
uint32_t b = rs2;
if (b == 0) {
  WriteSr(_RD, uint32_t(-1));
} else {
  WriteSr(_RD, uint32_t(a / b));
}
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic unsigned divide") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(100));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(2));
  RUN_INST(ISA_RV32I_DIVU, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 50);
}

TEST_CASE("divide by zero returns 0xFFFFFFFF") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(12345));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32I_DIVU, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xFFFFFFFF);
}
#endif
