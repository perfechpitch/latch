#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t oldCsr = CSR[_CSR].U32();
CSR[_CSR] = oldCsr | rs1;
WriteSr(_RD, oldCsr);
#endif

#ifdef TEST_DEFINE
TEST_CASE("set some bits in CSR") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0b11000011));
  CSR[CSR_MSCRATH].Set<uint32_t>(static_cast<uint32_t>(0b00001111));
  RUN_INST(ISA_RV32I_CSRRS, 7, 6, CSR_MSCRATH);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 0b00001111);
  CHECK_REGISTER_EQ(uint32_t, CSR[CSR_MSCRATH], 0b11001111);
}

TEST_CASE("rs1 = x0 leaves CSR unchanged, rd gets current value") {
  SR[0].Set<uint32_t>(static_cast<uint32_t>(0));
  CSR[CSR_MSCRATH].Set<uint32_t>(static_cast<uint32_t>(0xA5A5A5A5));
  RUN_INST(ISA_RV32I_CSRRS, 8, 0, CSR_MSCRATH);
  CHECK_REGISTER_EQ(uint32_t, SR[8], 0xA5A5A5A5);
  CHECK_REGISTER_EQ(uint32_t, CSR[CSR_MSCRATH], 0xA5A5A5A5);
}
#endif
