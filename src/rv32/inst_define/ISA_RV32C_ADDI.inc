#ifdef INST_RUN_DEFINE
uint32_t rd_r = ReadSr(_RD);
WriteSr(_RD, uint32_t(int32_t(rd_r) + CONCAT(_IMM1, _IMM5).S32()));
#endif

#ifdef TEST_DEFINE
TEST_CASE("negative immediate") {
  SR[6].Set<int32_t>(static_cast<int32_t>(10));
  RUN_INST(ISA_RV32C_ADDI, 2, 6, 1);
  CHECK_REGISTER_EQ(int32_t, SR[6], 10 + (-30));
}

TEST_CASE("positive immediate") {
  SR[6].Set<int32_t>(static_cast<int32_t>(100));
  RUN_INST(ISA_RV32C_ADDI, 5, 6, 0);
  CHECK_REGISTER_EQ(int32_t, SR[6], 105);
}

TEST_CASE("negative imm produces negative result") {
  SR[6].Set<int32_t>(static_cast<int32_t>(5));
  RUN_INST(ISA_RV32C_ADDI, 0x1F, 6, 1);
  CHECK_REGISTER_EQ(int32_t, SR[6], 4);
}
#endif
