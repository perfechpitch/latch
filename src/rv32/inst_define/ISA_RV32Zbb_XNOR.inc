#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
WriteSr(_RD, uint32_t(~(rs2 ^ rs1)));
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic disjoint bits") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x0000FFFF));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0xFFFF0000));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_XNOR, 7, 6, 5);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x00000000);
}

TEST_CASE("rs2 all zeros -> ~(rs1 ^ 0) = ~rs1") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x12345678));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x00000000));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_XNOR, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xEDCBA987);
}

TEST_CASE("rs2 all ones -> ~(rs1 ^ 0xFFFFFFFF) = rs1") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0xDEADBEEF));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0xFFFFFFFF));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_XNOR, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xDEADBEEF);
}
#endif
