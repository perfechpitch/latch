#ifdef INST_RUN_DEFINE
uint32_t sp_r = ReadSr(2);
int32_t offset = CONCAT(_IMM1_9, _IMM2_7_8, _IMM1_6, _IMM1_5, _IMM1_4).S32() * 16;
if (offset != 0) {
  WriteSr(2, uint32_t(int32_t(sp_r) + offset));
}
#endif

#ifdef TEST_DEFINE
// 6-bit signed immediate scaled by 16; operates implicitly on sp (x2).

TEST_CASE("negative offset (decrement sp)") {
  SR[2].Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  RUN_INST(ISA_RV32C_ADDI16SP, 1, 2, 0, 1, 1);
  CHECK_REGISTER_EQ(uint32_t, SR[2], 0x00800000u - 208);
}

TEST_CASE("positive offset (increment sp)") {
  SR[2].Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  RUN_INST(ISA_RV32C_ADDI16SP, 0, 0, 0, 1, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[2], 0x00800000u + 16);
}
#endif
