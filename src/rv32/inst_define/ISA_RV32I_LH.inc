#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint64_t addr = rs1 + _IMM12.S32();
uint32_t v = system->LoadMem<uint16_t>(addr);
v |= (v & 0x8000) ? 0xFFFF0000u : 0u;
WriteSr(_RD, v);
#endif

#ifdef TEST_DEFINE
TEST_CASE("halfword with bit 15 set -> sign-extended negative") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001080, 0x000080A0);
  RUN_INST(ISA_RV32I_LH, 7, 6, 0x0080);
  CHECK_REGISTER_EQ(int32_t, SR[7], static_cast<int32_t>(0xFFFF80A0));
}

TEST_CASE("positive halfword (bit 15 clear) -> zero-extended") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001080, 0x00001234);
  RUN_INST(ISA_RV32I_LH, 7, 6, 0x0080);
  CHECK_REGISTER_EQ(int32_t, SR[7], 0x1234);
}

TEST_CASE("max positive halfword = 0x7FFF") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001080, 0x00007FFF);
  RUN_INST(ISA_RV32I_LH, 7, 6, 0x0080);
  CHECK_REGISTER_EQ(int32_t, SR[7], 0x7FFF);
}

TEST_CASE("max negative halfword = 0x8000 -> -32768") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001080, 0x00008000);
  RUN_INST(ISA_RV32I_LH, 7, 6, 0x0080);
  CHECK_REGISTER_EQ(int32_t, SR[7], -32768);
}
#endif
