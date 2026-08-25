#ifdef INST_RUN_DEFINE
uint32_t rd_r = ReadSr(_RD + 8);
uint32_t rs2_r = ReadSr(_RS2 + 8);
WriteSr(_RD + 8, uint32_t(rd_r | rs2_r));
#endif

#ifdef TEST_DEFINE
SR[5 + 8].Set<uint32_t>(static_cast<uint32_t>(0x55555555));
SR[7 + 8].Set<uint32_t>(static_cast<uint32_t>(0xAAAAAAAA));

RUN_INST(ISA_RV32C_OR, 5, 7);

CHECK_REGISTER_EQ(uint32_t, SR[7 + 8], 0xFFFFFFFF);
#endif