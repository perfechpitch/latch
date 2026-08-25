#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
WriteSr(_RD, uint32_t((rs1 < rs2) ? 1 : 0));
#endif

#ifdef TEST_DEFINE
TEST_CASE("unsigned: 1 < 2 is true") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(1));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(2));
  RUN_INST(ISA_RV32I_SLTU, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 1u);
}

TEST_CASE("unsigned: 0xFFFFFFFF < 1 is false (distinguishes from SLT)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0xFFFFFFFFu));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(1));
  RUN_INST(ISA_RV32I_SLTU, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0u);
}

TEST_CASE("equal operands yield 0") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(42));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(42));
  RUN_INST(ISA_RV32I_SLTU, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0u);
}
#endif
