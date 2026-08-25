#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
uint64_t addr = rs1 + CONCAT(_IMM7, _IMM5).S32();
system->StoreMem<uint8_t>(addr, uint8_t(rs2));
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic store of low byte") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x12345678));
  RUN_INST(ISA_RV32I_SB, 0, 5, 6, 0b0100);
  CHECK_MEMORY_EQ(uint8_t, 0x08001080, 0x78);
}

TEST_CASE("upper bits of rs2 are ignored (only low byte stored)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0xDEADBEEF));
  RUN_INST(ISA_RV32I_SB, 0, 5, 6, 0b0100);
  CHECK_MEMORY_EQ(uint8_t, 0x08001080, 0xEF);
}

TEST_CASE("store byte 0x00") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001080, 0xFFFFFFFF);
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0xAAAAAA00));
  RUN_INST(ISA_RV32I_SB, 0, 5, 6, 0b0100);
  CHECK_MEMORY_EQ(uint8_t, 0x08001080, 0x00);
}
#endif
