#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
uint64_t addr = rs1 + CONCAT(_IMM7, _IMM5).S32();
system->StoreMem<uint16_t>(addr, uint16_t(rs2));
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic store of low halfword") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x12345678));
  RUN_INST(ISA_RV32I_SH, 0, 5, 6, 0b0100);
  CHECK_MEMORY_EQ(uint16_t, 0x08001080, 0x5678);
}

TEST_CASE("upper bits of rs2 are ignored (only low halfword stored)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0xDEADBEEF));
  RUN_INST(ISA_RV32I_SH, 0, 5, 6, 0b0100);
  CHECK_MEMORY_EQ(uint16_t, 0x08001080, 0xBEEF);
}

TEST_CASE("store halfword with bit 15 set") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x00008000));
  RUN_INST(ISA_RV32I_SH, 0, 5, 6, 0b0100);
  CHECK_MEMORY_EQ(uint16_t, 0x08001080, 0x8000);
}
#endif
