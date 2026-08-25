#ifdef INST_RUN_DEFINE
WriteSr(_RD, uint32_t(PC + (_IMM20.U32() << 12)));
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic positive immediate") {
  RUN_INST(ISA_RV32I_AUIPC, 7, 0x0002);
  CHECK_REGISTER_EQ(uint32_t, SR[7], (uint32_t)InitPC + 0x2000);
}

TEST_CASE("imm = 0x12345 -> PC + 0x12345000") {
  RUN_INST(ISA_RV32I_AUIPC, 7, 0x12345);
  CHECK_REGISTER_EQ(uint32_t, SR[7], (uint32_t)InitPC + 0x12345000);
}

TEST_CASE("imm high bit set (0x80000) -> PC + 0x80000000") {
  RUN_INST(ISA_RV32I_AUIPC, 7, 0x80000);
  CHECK_REGISTER_EQ(uint32_t, SR[7], (uint32_t)InitPC + 0x80000000u);
}
#endif
