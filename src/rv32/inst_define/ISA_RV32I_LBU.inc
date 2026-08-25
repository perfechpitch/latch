#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint64_t addr = rs1 + _IMM12.S32();
uint32_t v = system->LoadMem<uint8_t>(addr);
WriteSr(_RD, v);
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic byte load") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001080, 0x00111111);
  RUN_INST(ISA_RV32I_LBU, 7, 6, 0x0080);
  CHECK_REGISTER_EQ(int32_t, SR[7], 0x00000011);
}

TEST_CASE("byte with bit 7 set -> zero-extended (NOT sign-extended)") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001080, 0x000000A0);
  RUN_INST(ISA_RV32I_LBU, 7, 6, 0x0080);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x000000A0);
}

TEST_CASE("0xFF byte -> 255, not -1") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x08001000));
  system->SetMemory<uint32_t>(0x08001080, 0x000000FF);
  RUN_INST(ISA_RV32I_LBU, 7, 6, 0x0080);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x000000FF);
}
#endif
