#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t target = rs1 + _IMM12.S32();
WriteSr(_RD, uint32_t(PC + 4));
PC = target & 0xfffffffeu;
BRANCH;
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic indirect jump, low bit clear") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x00880000));
  RUN_INST(ISA_RV32I_JALR, 7, 6, 8);
  CHECK_REGISTER_EQ(uint32_t, SR[7], (uint32_t)InitPC + 4);
  CHECK_REGISTER_EQ(uint32_t, PC, 0x00880008);
}

TEST_CASE("target with bit 0 set -> low bit cleared in PC") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x00880000));
  RUN_INST(ISA_RV32I_JALR, 7, 6, 9);
  CHECK_REGISTER_EQ(uint32_t, SR[7], (uint32_t)InitPC + 4);
  CHECK_REGISTER_EQ(uint32_t, PC, 0x00880008);
}

TEST_CASE("negative immediate") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x00880100));
  RUN_INST(ISA_RV32I_JALR, 7, 6, -0x10);
  CHECK_REGISTER_EQ(uint32_t, SR[7], (uint32_t)InitPC + 4);
  CHECK_REGISTER_EQ(uint32_t, PC, 0x008800F0);
}
#endif
