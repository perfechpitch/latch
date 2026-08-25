#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
WriteSr(_RD, uint32_t((int64_t(int32_t(rs1)) * uint64_t(rs2)) >> 32));
#endif

#ifdef TEST_DEFINE
TEST_CASE("positive_signed * positive_unsigned — high half is 0") {
  SR[5].Set<int32_t>(static_cast<int32_t>(10));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(11));
  RUN_INST(ISA_RV32I_MULHSU, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0);
}

TEST_CASE("negative_signed * positive_unsigned — high half sign-extends") {
  SR[5].Set<int32_t>(static_cast<int32_t>(-10));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(11));
  RUN_INST(ISA_RV32I_MULHSU, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xFFFFFFFF);
}

TEST_CASE("negative_signed * large_unsigned — distinguishes from MULHU/MULH") {
  SR[5].Set<int32_t>(static_cast<int32_t>(-1));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0xFFFFFFFF));
  RUN_INST(ISA_RV32I_MULHSU, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xFFFFFFFF);
}
#endif
