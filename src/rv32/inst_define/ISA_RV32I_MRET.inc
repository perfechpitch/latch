#ifdef INST_RUN_DEFINE
PC = CSR[CSR_MEPC].U32();
BRANCH;
#endif

#ifdef TEST_DEFINE
TEST_CASE("restores PC from mepc") {
  CSR[CSR_MEPC].Set<uint32_t>(static_cast<uint32_t>(0x00880000));
  PC.Set<uint32_t>(static_cast<uint32_t>(0x00800000));
  RunOne(std::make_shared<Rv32::ISA_RV32I_MRET>());
  CHECK_REGISTER_EQ(uint32_t, PC, 0x00880000);
}
#endif
