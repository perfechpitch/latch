#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
uint32_t a = rs1;
uint32_t b = rs2;
if (b == 0) {
  WriteSr(_RD, uint32_t(a));
} else {
  WriteSr(_RD, uint32_t(a % b));
}
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic unsigned remainder") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(10));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(3));
  RUN_INST(ISA_RV32I_REMU, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 1);
}

TEST_CASE("rem by zero returns dividend") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0xDEADBEEF));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32I_REMU, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xDEADBEEF);
}
#endif
