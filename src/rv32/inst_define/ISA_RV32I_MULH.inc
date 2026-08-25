#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
WriteSr(_RD, uint32_t((int64_t(int32_t(rs1)) * int64_t(int32_t(rs2))) >> 32));
#endif

#ifdef TEST_DEFINE
TEST_CASE("small positive * positive — high half is 0") {
  SR[5].Set<int32_t>(static_cast<int32_t>(10));
  SR[6].Set<int32_t>(static_cast<int32_t>(11));
  RUN_INST(ISA_RV32I_MULH, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0);
}

TEST_CASE("negative * positive — high half sign-extends to 0xFFFFFFFF") {
  SR[5].Set<int32_t>(static_cast<int32_t>(-10));
  SR[6].Set<int32_t>(static_cast<int32_t>(11));
  RUN_INST(ISA_RV32I_MULH, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xFFFFFFFF);
}

TEST_CASE("INT_MIN * INT_MIN — high half is 0x40000000") {
  SR[5].Set<int32_t>(static_cast<int32_t>(INT32_MIN));
  SR[6].Set<int32_t>(static_cast<int32_t>(INT32_MIN));
  RUN_INST(ISA_RV32I_MULH, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0x40000000);
}
#endif
