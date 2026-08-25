#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1 + 8);
uint32_t rs2 = ReadSr(_RD + 8);
uint64_t addr = rs1 + (CONCAT(_IMM1_6, _IMM3_3_5, _IMM1_2).U32() << 2);
system->StoreMem<uint32_t>(addr, rs2);
#endif

#ifdef TEST_DEFINE
// The _RD encoding field is the store-source register here (mem written from SR[_RD + 8]).

TEST_CASE("store word with high-bit data") {
  SR[6 + 8].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  SR[7 + 8].Set<uint32_t>(static_cast<uint32_t>(0xCAFEBABE));
  RUN_INST(ISA_RV32C_SW, 7, 0, 1, 6, 2);
  CHECK_MEMORY_EQ(uint32_t, 0x08001000 + 20, 0xCAFEBABE);
}
#endif
