#ifdef INST_RUN_DEFINE
uint32_t oldCsr = CSR[_CSR].U32();
CSR[_CSR] = _IMM5.U32();
WriteSr(_RD, oldCsr);
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic immediate write") {
  CSR[CSR_MSCRATH].Set<uint32_t>(static_cast<uint32_t>(12));
  RUN_INST(ISA_RV32I_CSRRWI, 7, 20, CSR_MSCRATH);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 12);
  CHECK_REGISTER_EQ(uint32_t, CSR[CSR_MSCRATH], 20);
}

TEST_CASE("imm = 0 clears CSR (zero-extended)") {
  CSR[CSR_MSCRATH].Set<uint32_t>(static_cast<uint32_t>(0xFFFFFFFF));
  RUN_INST(ISA_RV32I_CSRRWI, 8, 0, CSR_MSCRATH);
  CHECK_REGISTER_EQ(uint32_t, SR[8], 0xFFFFFFFF);
  CHECK_REGISTER_EQ(uint32_t, CSR[CSR_MSCRATH], 0);
}
#endif
