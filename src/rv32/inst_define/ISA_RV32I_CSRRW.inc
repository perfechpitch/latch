#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t oldCsr = CSR[_CSR].U32();
CSR[_CSR] = rs1;
WriteSr(_RD, oldCsr);
#endif

#ifdef TEST_DEFINE
TEST_CASE("basic swap: old CSR -> rd, rs1 -> CSR") {
  SR[6].Set<uint32_t>(static_cast<uint32_t>(10));
  CSR[CSR_MSCRATH].Set<uint32_t>(static_cast<uint32_t>(12));
  RUN_INST(ISA_RV32I_CSRRW, 7, 6, CSR_MSCRATH);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 12);
  CHECK_REGISTER_EQ(uint32_t, CSR[CSR_MSCRATH], 10);
}

TEST_CASE("read-then-write with distinct rd and rs1") {
  SR[8].Set<uint32_t>(static_cast<uint32_t>(0xDEADBEEF));
  CSR[CSR_MSCRATH].Set<uint32_t>(static_cast<uint32_t>(0xCAFEBABE));
  RUN_INST(ISA_RV32I_CSRRW, 9, 8, CSR_MSCRATH);
  CHECK_REGISTER_EQ(uint32_t, SR[9], 0xCAFEBABE);
  CHECK_REGISTER_EQ(uint32_t, CSR[CSR_MSCRATH], 0xDEADBEEF);
}

TEST_CASE("rs1 = x0 writes 0 to CSR, rd gets old value") {
  SR[0].Set<uint32_t>(static_cast<uint32_t>(0));
  CSR[CSR_MSCRATH].Set<uint32_t>(static_cast<uint32_t>(0x12345678));
  RUN_INST(ISA_RV32I_CSRRW, 10, 0, CSR_MSCRATH);
  CHECK_REGISTER_EQ(uint32_t, SR[10], 0x12345678);
  CHECK_REGISTER_EQ(uint32_t, CSR[CSR_MSCRATH], 0);
}
#endif
