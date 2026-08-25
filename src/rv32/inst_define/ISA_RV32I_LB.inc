#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint64_t addr = rs1 + _IMM12.S32();
uint32_t v = system->LoadMem<uint8_t>(addr);
v |= (v & 0x80) ? 0xFFFFFF00u : 0u;
WriteSr(_RD, v);
#endif

#ifdef TEST_DEFINE
TEST_CASE("byte with bit 7 set -> sign-extended negative") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001080, 0x000000A0);
  RUN_INST(ISA_RV32I_LB, 7, 6, 0x0080);
  CHECK_REGISTER_EQ(int32_t, SR[7], static_cast<int32_t>(0xFFFFFFA0));
}

TEST_CASE("positive byte (bit 7 clear) -> zero-extended") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001080, 0x00000042);
  RUN_INST(ISA_RV32I_LB, 7, 6, 0x0080);
  CHECK_REGISTER_EQ(int32_t, SR[7], 0x42);
}

TEST_CASE("max positive byte = 0x7F") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001080, 0x0000007F);
  RUN_INST(ISA_RV32I_LB, 7, 6, 0x0080);
  CHECK_REGISTER_EQ(int32_t, SR[7], 0x7F);
}

TEST_CASE("max negative byte = 0x80 -> -128") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001080, 0x00000080);
  RUN_INST(ISA_RV32I_LB, 7, 6, 0x0080);
  CHECK_REGISTER_EQ(int32_t, SR[7], -128);
}
#endif
