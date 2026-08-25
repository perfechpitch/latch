#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
WriteSr(_RD, uint32_t((uint64_t(rs1) * uint64_t(rs2)) >> 32));
#endif

#ifdef TEST_DEFINE
TEST_CASE("0xFFFFFFFF * 2 — high half = 1") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0xFFFFFFFF));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(2));
  RUN_INST(ISA_RV32I_MULHU, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 1);
}

TEST_CASE("MAX_U * MAX_U — high half = 0xFFFFFFFE") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0xFFFFFFFF));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0xFFFFFFFF));
  RUN_INST(ISA_RV32I_MULHU, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xFFFFFFFE);
}
#endif
