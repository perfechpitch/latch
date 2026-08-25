#ifdef INST_RUN_DEFINE
uint32_t rs1 = ReadSr(_RS1);
uint32_t rs2 = ReadSr(_RS2);
uint64_t addr  = rs1;
uint32_t rs2v  = rs2;
int32_t  rs2vs = int32_t(rs2v);
uint32_t old   = system->LoadMem<uint32_t>(addr);
int32_t  olds  = int32_t(old);
(void)rs2vs; (void)olds;
uint32_t newv  = (uint32_t((olds > rs2vs) ? olds : rs2vs));
system->StoreMem<uint32_t>(addr, newv);
WriteSr(_RD, old);
#endif

#ifdef TEST_DEFINE
TEST_CASE("signed max picks positive over negative") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  SR[6].Set<int32_t>(static_cast<int32_t>(5));
  system->SetMemory<int32_t>(0x08001000, -10);
  RUN_INST(ISA_RV32A_AMOMAX_W, 7, 5, 6, 0, 0);
  CHECK_REGISTER_EQ(int32_t, SR[7], -10);
  CHECK_MEMORY_EQ(int32_t, 0x08001000, 5);
}

TEST_CASE("signed max when old larger") {
  SR[5].Set<uint32_t>(static_cast<uint32_t>(0x08001000));
  SR[6].Set<int32_t>(static_cast<int32_t>(-100));
  system->SetMemory<int32_t>(0x08001000, 50);
  RUN_INST(ISA_RV32A_AMOMAX_W, 7, 5, 6, 0, 0);
  CHECK_REGISTER_EQ(int32_t, SR[7], 50);
  CHECK_MEMORY_EQ(int32_t, 0x08001000, 50);
}
#endif
