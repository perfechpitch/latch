#ifdef INST_RUN_DEFINE
// Single-hart: LR.W degenerates to a plain word load (no reservation tracked).
uint32_t rs1 = ReadSr(_RS1);
uint64_t addr = rs1;
WriteSr(_RD, system->LoadMem<uint32_t>(addr));
#endif

#ifdef TEST_DEFINE
TEST_CASE("loads aligned word from memory") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001000, 0x12345678);
  RUN_INST(ISA_RV32A_LR_W, 7, 5, 0, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x12345678);
}

TEST_CASE("loads high-bit value") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001000, 0xDEADBEEF);
  RUN_INST(ISA_RV32A_LR_W, 7, 5, 0, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xDEADBEEF);
}
#endif
