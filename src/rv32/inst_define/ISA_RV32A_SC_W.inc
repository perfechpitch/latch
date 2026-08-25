#ifdef INST_RUN_DEFINE
// Single-hart: SC.W always succeeds (rd = 0); no reservation is tracked.
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
uint64_t addr = rs1;
system->StoreMem<uint32_t>(addr, rs2);
WriteSr(_RD, uint32_t(0));
#endif

#ifdef TEST_DEFINE
TEST_CASE("store-conditional always succeeds") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0xCAFEBABE));
  system->SetMemory<uint32_t>(0x08001000, 0x00000000);
  RUN_INST(ISA_RV32A_SC_W, 7, 5, 6, 0, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0u);
  CHECK_MEMORY_EQ(uint32_t, 0x08001000, 0xCAFEBABE);
}
#endif
