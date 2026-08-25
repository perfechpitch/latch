#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint64_t addr = rs1 + _IMM12.S32();
WriteSr(_RD, system->LoadMem<uint32_t>(addr));
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic positive 32-bit load") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001080, 0x00111111);
  RUN_INST(ISA_RV32I_LW, 7, 6, 0x0080);
  CHECK_REGISTER_EQ(int32_t, SR[7], 0x00111111);
}

TEST_CASE("value with high bit set loads verbatim (no sign-extension issue)") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001080, 0x80ABCDEF);
  RUN_INST(ISA_RV32I_LW, 7, 6, 0x0080);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x80ABCDEF);
}

TEST_CASE("negative-offset addressing") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x08001100));
  system->SetMemory<uint32_t>(0x080010F0, 0xCAFEBABE);
  RUN_INST(ISA_RV32I_LW, 7, 6, -0x10);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xCAFEBABE);
}
#endif
