#ifdef INST_RUN_DEFINE
WriteSr(_RD, uint32_t(CONCAT(_IMM1, _IMM5).S32()));
#endif

#ifdef TEST_DEFINE
TEST_CASE("load negative immediate") {
  SR[6].Set<int32_t>(static_cast<int32_t>(10));
  RUN_INST(ISA_RV32C_LI, 2, 6, 1);
  CHECK_REGISTER_EQ(int32_t, SR[6], -30);
}

TEST_CASE("load positive immediate") {
  SR[6].Set<int32_t>(static_cast<int32_t>(-1));
  RUN_INST(ISA_RV32C_LI, 0x1F, 6, 0);
  CHECK_REGISTER_EQ(int32_t, SR[6], 31);
}
#endif
