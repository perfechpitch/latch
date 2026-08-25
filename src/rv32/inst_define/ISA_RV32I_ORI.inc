#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
WriteSr(_RD, uint32_t(int32_t(rs1) | int32_t(_IMM12.S32())));
#endif

#ifdef TEST_DEFINE
TEST_CASE("set low bits") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x03));
  RUN_INST(ISA_RV32I_ORI, 7, 6, 0x0C);
  CHECK_REGISTER_EQ(int32_t, SR[7], 0x0F);
}

TEST_CASE("negative immediate sign-extends, ORs ones into upper bits") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x00000001u));
  RUN_INST(ISA_RV32I_ORI, 7, 6, -1);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xFFFFFFFFu);
}

TEST_CASE("OR with zero is identity") {
  SR[6].Set<int32_t>(static_cast<int32_t>(0x12345678));
  RUN_INST(ISA_RV32I_ORI, 7, 6, 0);
  CHECK_REGISTER_EQ(int32_t, SR[7], 0x12345678);
}
#endif
