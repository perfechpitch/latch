#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1 + 8);
uint64_t addr = rs1 + (CONCAT(_IMM1_6, _IMM3_3_5, _IMM1_2).U32() << 2);
WriteSr(_RD + 8, system->LoadMem<uint32_t>(addr));
#endif

#ifdef TEST_DEFINE
TEST_CASE("load word with non-trivial high bits") {
  SR[6 + 8].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001000 + 20, 0xDEADBEEF);
  RUN_INST(ISA_RV32C_LW, 7, 0, 1, 6, 2);
  CHECK_REGISTER_EQ(uint32_t, SR[7 + 8], 0xDEADBEEF);
}
#endif
