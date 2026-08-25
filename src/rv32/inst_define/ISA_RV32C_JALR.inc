#ifdef INST_RUN_DEFINE
if (_RS1 != 0) {
  uint32_t rs1 = ReadSr(_RS1);
  uint32_t newpc = rs1 & ~1u;
  WriteSr(1, uint32_t(PC + 2));
  PC = newpc;
  BRANCH;
} else {
  // C.EBREAK
}
#endif

#ifdef TEST_DEFINE
TEST_CASE("jump to rs1 and link") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x00880000));
  PC.Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  RUN_INST(ISA_RV32C_JALR, 6);
  CHECK_REGISTER_EQ(uint32_t, PC, 0x00880000);
  CHECK_REGISTER_EQ(uint32_t, SR[1], 0x00800002);
}

TEST_CASE("target low bit is cleared") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x00880003));
  PC.Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  RUN_INST(ISA_RV32C_JALR, 6);
  CHECK_REGISTER_EQ(uint32_t, PC, 0x00880002);
  CHECK_REGISTER_EQ(uint32_t, SR[1], 0x00800002);
}

TEST_CASE("rs1 == x0 is C.EBREAK (pc falls through by 2)") {
  PC.Set<uint32_t>(static_cast<uint32_t>(0x00880010));
  RUN_INST(ISA_RV32C_JALR, 0);
  CHECK_REGISTER_EQ(uint32_t, PC, 0x00880012);
}
#endif
