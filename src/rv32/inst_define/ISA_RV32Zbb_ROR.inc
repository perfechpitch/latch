#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
uint32_t value = rs1;
uint32_t shift = rs2 & 0x1F;
WriteSr(_RD, uint32_t((shift == 0) ? value : ((value >> shift) | (value << (32 - shift)))));
#endif

#ifdef TEST_DEFINE
TEST_CASE("shift = 0 (identity, exercises UB guard)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x12345678));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_ROR, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x12345678);
}

TEST_CASE("shift = 8 rotates one byte right") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x12345678));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(8));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_ROR, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x78123456);
}

TEST_CASE("shift = 31 (edge)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x00000001));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(31));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_ROR, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x00000002);
}

TEST_CASE("shift = 36 verifies & 0x1F masking (36 & 0x1F = 4)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x12345678));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(36));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0));
  RUN_INST(ISA_RV32Zbb_ROR, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x81234567);
}
#endif
