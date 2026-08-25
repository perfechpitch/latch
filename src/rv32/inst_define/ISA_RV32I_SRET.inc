#ifdef INST_RUN_DEFINE
PC = CSR[CSR_SEPC].U32();
BRANCH;
#endif

#ifdef TEST_DEFINE
TEST_CASE("restores PC from sepc") {
  CSR[CSR_SEPC].Set<uint32_t>(static_cast<uint32_t>(0x00880000));
  PC.Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  RunOne(std::make_shared<Rv32::ISA_RV32I_SRET>());
  CHECK_REGISTER_EQ(uint32_t, PC, 0x00880000);
}
#endif
