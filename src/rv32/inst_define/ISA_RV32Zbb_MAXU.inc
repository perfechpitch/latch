#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
uint32_t a = rs1;
uint32_t b = rs2;
WriteSr(_RD, uint32_t((a > b) ? a : b));
#endif

#ifdef TEST_DEFINE
TEST_CASE("rs1 greater than rs2") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(100));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(50));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_MAXU, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 100);
}

TEST_CASE("rs1 less than rs2") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(7));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(42));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_MAXU, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 42);
}

TEST_CASE("rs1 equals rs2") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(33));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(33));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_MAXU, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 33);
}

TEST_CASE("unsigned comparison: 0xFFFFFFFF vs 1") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0xFFFFFFFF));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(1));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_MAXU, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xFFFFFFFF);
}
#endif
