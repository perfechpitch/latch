#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
WriteSr(_RD, uint32_t(rs1 | rs2));
#endif

#ifdef TEST_DEFINE
TEST_CASE("disjoint bit patterns combine to all-ones") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x55555555u));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0xAAAAAAAAu));
  RUN_INST(ISA_RV32I_OR, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xFFFFFFFFu);
}

TEST_CASE("OR with all-zeros is identity") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0xDEADBEEFu));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0x00000000u));
  RUN_INST(ISA_RV32I_OR, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xDEADBEEFu);
}

TEST_CASE("OR with all-ones saturates") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x12345678u));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0xFFFFFFFFu));
  RUN_INST(ISA_RV32I_OR, 7, 5, 6);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0xFFFFFFFFu);
}
#endif
