#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
uint64_t addr  = rs1;
uint32_t rs2v  = rs2;
int32_t  rs2vs = int32_t(rs2v);
uint32_t old   = system->LoadMem<uint32_t>(addr);
int32_t  olds  = int32_t(old);
(void)rs2vs; (void)olds;
uint32_t newv  = ((old > rs2v) ? old : rs2v);
system->StoreMem<uint32_t>(addr, newv);
WriteSr(_RD, old);
#endif

#ifdef TEST_DEFINE
TEST_CASE("unsigned max: 1 vs 0xFFFFFFFF picks 0xFFFFFFFF (distinguishes from signed)") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  SR[6].Set<uint32_t>(static_cast<uint32_t>(0xFFFFFFFF));
  system->SetMemory<uint32_t>(0x08001000, 1);
  RUN_INST(ISA_RV32A_AMOMAXU_W, 7, 5, 6, 0, 0);
  CHECK_REGISTER_EQ(uint32_t, SR[7], 1);
  CHECK_MEMORY_EQ(uint32_t, 0x08001000, 0xFFFFFFFF);
}
#endif
