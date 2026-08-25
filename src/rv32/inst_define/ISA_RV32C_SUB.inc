#ifdef INST_RUN_DEFINE
uint32_t rd_r = ReadSr(_RD + 8);
uint32_t rs2_r = ReadSr(_RS2 + 8);
WriteSr(_RD + 8, uint32_t(int32_t(rd_r) - int32_t(rs2_r)));
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic positive subtraction") {
  SR[5 + 8].Set<int32_t>(static_cast<int32_t>(10));
  SR[7 + 8].Set<int32_t>(static_cast<int32_t>(11));
  RUN_INST(ISA_RV32C_SUB, 5, 7);
  CHECK_REGISTER_EQ(int32_t, SR[7 + 8], 1);
}

TEST_CASE("underflow wraps to 0xFFFFFFFF") {
  SR[5 + 8].Set<uint32_t>(static_cast<uint32_t>(1));
  SR[7 + 8].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32C_SUB, 5, 7);
  CHECK_REGISTER_EQ(uint32_t, SR[7 + 8], 0xFFFFFFFF);
}
#endif
