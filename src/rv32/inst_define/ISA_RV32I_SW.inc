#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
uint64_t addr = rs1 + CONCAT(_IMM7, _IMM5).S32();
system->StoreMem<uint32_t>(addr, rs2);
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic 32-bit store") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x12345678));
  RUN_INST(ISA_RV32I_SW, 0, 5, 6, 0b0100);
  CHECK_MEMORY_EQ(uint32_t, 0x08001080, 0x12345678);
}

TEST_CASE("store value with high bit set (full 32 bits written)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0xDEADBEEF));
  RUN_INST(ISA_RV32I_SW, 0, 5, 6, 0b0100);
  CHECK_MEMORY_EQ(uint32_t, 0x08001080, 0xDEADBEEF);
}

TEST_CASE("negative-offset store") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x08001100));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0xCAFEBABE));
  RUN_INST(ISA_RV32I_SW, 0x10, 5, 6, 0x7F);
  CHECK_MEMORY_EQ(uint32_t, 0x080010F0, 0xCAFEBABE);
}
#endif
