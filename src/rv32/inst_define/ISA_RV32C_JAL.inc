#ifdef INST_RUN_DEFINE
WriteSr(1, uint32_t(PC + 2));
int32_t offset = CONCAT(_IMM1_11, _IMM1_10, _IMM2_9_8, _IMM1_7, _IMM1_6, _IMM1_5, _IMM1_4, _IMM3_3_1).S32() * 2;
PC = PC + offset;
BRANCH;
#endif

#ifdef TEST_DEFINE
TEST_CASE("forward jump records return address in x1") {
  PC.Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  SR[1].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32C_JAL, 0, 1, 0, 0, 0, 0, 0, 0);
  CHECK_REGISTER_EQ(uint32_t, PC, 0x00800000u + 2);
  CHECK_REGISTER_EQ(uint32_t, SR[1], 0x00800002);
}

TEST_CASE("backward jump still records pc+2 in x1") {
  PC.Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  SR[1].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32C_JAL, 0, 0, 0, 0, 0, 0, 0, 1);
  CHECK_REGISTER_EQ(uint32_t, PC, 0x00800000u - 2048);
  CHECK_REGISTER_EQ(uint32_t, SR[1], 0x00800002);
}
#endif
