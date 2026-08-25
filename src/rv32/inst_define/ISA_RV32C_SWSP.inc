#ifdef INST_RUN_DEFINE
uint32_t sp_r = ReadSr(2);
uint32_t rs2 = ReadSr(_RS2);
uint64_t addr = sp_r + CONCAT(_IMM2_7_6, _IMM4_5_2).U32() * 4;
system->StoreMem<uint32_t>(addr, rs2);
#endif

#ifdef TEST_DEFINE
TEST_CASE("store word to sp-relative slot") {
  SR[2].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  SR[7].Set<uint32_t>(static_cast<uint32_t>(0xBADDCAFE));
  RUN_INST(ISA_RV32C_SWSP, 7, 2, 3);
  CHECK_MEMORY_EQ(uint32_t, 0x08001000 + 140, 0xBADDCAFE);
}
#endif
