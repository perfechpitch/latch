#ifdef INST_RUN_DEFINE
if (_RS1 != 0) {
  uint32_t rs1 = ReadSr(_RS1);
  uint32_t newpc = rs1;
  PC = newpc;
  BRANCH;
}
#endif

#ifdef TEST_DEFINE
TEST_CASE("jump to address in rs1") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x00880000));
  PC.Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  RUN_INST(ISA_RV32C_JR, 6);
  CHECK_REGISTER_EQ(uint32_t, PC, 0x00880000);
}

TEST_CASE("rs1 == x0 is reserved; pc falls through by 2") {
  PC.Set<uint32_t>(static_cast<uint32_t>(0x00880010));
  RUN_INST(ISA_RV32C_JR, 0);
  CHECK_REGISTER_EQ(uint32_t, PC, 0x00880012);
}
#endif
