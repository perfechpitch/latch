#ifdef INST_RUN_DEFINE
if (_RD != 0) {
  uint32_t sp_r = ReadSr(2);
  uint64_t addr = sp_r + CONCAT(_IMM2_7_6, _IMM1_5, _IMM3_4_2).U32() * 4;
  WriteSr(_RD, system->LoadMem<uint32_t>(addr));
}
#endif

#ifdef TEST_DEFINE
TEST_CASE("load word from sp-relative slot") {
  SR[2].Set<uint32_t>(static_cast<uint32_t>(0x08000000));
  system->SetMemory<uint32_t>(0x08000000 + 72, 0xFEEDFACE);
  RUN_INST(ISA_RV32C_LWSP, 1, 2, 6, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[6], 0xFEEDFACE);
}
#endif
