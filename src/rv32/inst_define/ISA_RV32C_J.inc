#ifdef INST_RUN_DEFINE
int32_t offset = CONCAT(_IMM1_11, _IMM1_10, _IMM2_9_8, _IMM1_7, _IMM1_6, _IMM1_5, _IMM1_4, _IMM3_3_1).S32() * 2;
PC = PC + offset;
BRANCH;
#endif

#ifdef TEST_DEFINE
TEST_CASE("forward jump (small positive)") {
  PC.Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  RUN_INST(ISA_RV32C_J, 0, 1, 0, 0, 0, 0, 0, 0);
  CHECK_REGISTER_EQ(uint32_t, PC, 0x00800000u + 2);
}

TEST_CASE("backward jump (minimum negative)") {
  PC.Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  RUN_INST(ISA_RV32C_J, 0, 0, 0, 0, 0, 0, 0, 1);
  CHECK_REGISTER_EQ(uint32_t, PC, 0x00800000u - 2048);
}
#endif
