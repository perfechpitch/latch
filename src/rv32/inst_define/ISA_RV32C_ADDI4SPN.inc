#ifdef INST_RUN_DEFINE
uint32_t sp_r = ReadSr(2);
uint32_t offset = CONCAT(_IMM4_9_6, _IMM2_5_4, _IMM1_3, _IMM1_2).U32() * 4;
if (offset != 0) {
  WriteSr(_RD + 8, uint32_t(sp_r + offset));
}
#endif

#ifdef TEST_DEFINE
// 8-bit unsigned immediate scaled by 4, added to implicit sp; dest is rd'+8 (x8..x15).

TEST_CASE("typical offset") {
  SR[2].Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  SR[5 + 8].Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  RUN_INST(ISA_RV32C_ADDI4SPN, 5, 1, 0, 5, 1);
  CHECK_REGISTER_EQ(uint32_t, SR[5 + 8], static_cast<uint32_t>(0x00800000u + 344));
}

TEST_CASE("small offset") {
  SR[2].Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  SR[7 + 8].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32C_ADDI4SPN, 7, 0, 1, 0, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[7 + 8], static_cast<uint32_t>(0x00800000u + 4));
}
#endif
