#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
WriteSr(_RD, uint32_t(int32_t(rs1) + int32_t(_IMM12.S32())));
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic positive immediate") {
  SR[6].Set<int32_t>(static_cast<int32_t>(10));
  RUN_INST(ISA_RV32I_ADDI, 7, 6, 2);
  CHECK_REGISTER_EQ(int32_t, SR[7], 12);
}

TEST_CASE("negative immediate is sign-extended") {
  SR[6].Set<int32_t>(static_cast<int32_t>(100));
  RUN_INST(ISA_RV32I_ADDI, 7, 6, -30);
  CHECK_REGISTER_EQ(int32_t, SR[7], 70);
}

TEST_CASE("add into negative result") {
  SR[6].Set<int32_t>(static_cast<int32_t>(5));
  RUN_INST(ISA_RV32I_ADDI, 7, 6, -10);
  CHECK_REGISTER_EQ(int32_t, SR[7], -5);
}
#endif
