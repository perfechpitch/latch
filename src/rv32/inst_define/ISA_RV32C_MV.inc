#ifdef INST_RUN_DEFINE
uint32_t rs2_r = ReadSr(_RS2);
WriteSr(_RD, rs2_r);
#endif

#ifdef TEST_DEFINE
TEST_CASE("positive value copy") {
  SR[5].Set<int32_t>(static_cast<int32_t>(10));
  SR[7].Set<int32_t>(static_cast<int32_t>(11));
  RUN_INST(ISA_RV32C_MV, 7, 5);
  CHECK_REGISTER_EQ(int32_t, SR[5], 11);
}

TEST_CASE("negative value round-trip via uint32") {
  SR[5].Set<int32_t>(static_cast<int32_t>(-10));
  SR[7].Set<int32_t>(static_cast<int32_t>(-11));
  RUN_INST(ISA_RV32C_MV, 7, 5);
  CHECK_REGISTER_EQ(int32_t, SR[5], -11);
}
#endif
