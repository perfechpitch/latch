#ifdef INST_RUN_DEFINE
uint32_t oldCsr = CSR[_CSR].U32();
CSR[_CSR] = oldCsr & (~_IMM5.U32());
WriteSr(_RD, oldCsr);
#endif

#ifdef TEST_DEFINE
TEST_CASE("clear bits via immediate") {
  CSR[CSR_MSCRATH].Set<uint32_t>(static_cast<uint32_t>(0b00001111));
  RUN_INST(ISA_RV32I_CSRRCI, 7, 0b00000011, CSR_MSCRATH);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0b00001111);
  CHECK_REGISTER_EQ(uint32_t, CSR[CSR_MSCRATH], 0b00001100);
}

TEST_CASE("imm = 0 leaves CSR unchanged") {
  CSR[CSR_MSCRATH].Set<uint32_t>(static_cast<uint32_t>(0xA5A5A5A5));
  RUN_INST(ISA_RV32I_CSRRCI, 8, 0, CSR_MSCRATH);
  CHECK_REGISTER_EQ(uint32_t, SR[8], 0xA5A5A5A5);
  CHECK_REGISTER_EQ(uint32_t, CSR[CSR_MSCRATH], 0xA5A5A5A5);
}
#endif
