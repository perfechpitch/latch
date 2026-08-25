#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
WriteSr(_RD, uint32_t(int32_t(rs1) * int32_t(rs2)));
#endif

#ifdef TEST_DEFINE
TEST_CASE("positive * positive") {
  SR[5].Set<int32_t>(static_cast<int32_t>(10));
  SR[6].Set<int32_t>(static_cast<int32_t>(11));
  RUN_INST(ISA_RV32I_MUL, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], 110);
}

TEST_CASE("signed mul yielding negative") {
  SR[5].Set<int32_t>(static_cast<int32_t>(-7));
  SR[6].Set<int32_t>(static_cast<int32_t>(13));
  RUN_INST(ISA_RV32I_MUL, 7, 5, 6);
  CHECK_REGISTER_EQ(int32_t, SR[7], -91);
}

TEST_CASE("overflow wraps to low 32 bits") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x10000));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x10000));
  RUN_INST(ISA_RV32I_MUL, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0);
}
#endif
