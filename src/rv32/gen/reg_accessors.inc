// Generated —— 每个寄存器堆的统一访问器(jsonc `registers` 块)。
// #include 进 System 子类 body;PC/Branch/Halt 等语义 override 仍手写。

  Register& SR(uint32_t thread = 0) { return GetRegFile(REG_SR, thread); }
  uint64_t GetSR(uint32_t index, uint32_t thread) { return GetRegFile(REG_SR, thread)[index].U64(); }
  void SetSR(uint32_t index, uint64_t v, uint32_t thread) { GetRegFile(REG_SR, thread)[index].Set<uint32_t>(static_cast<uint32_t>(v)); }

  Register& SPR(uint32_t thread = 0) { return GetRegFile(REG_SPR, thread); }
  uint64_t GetSPR(uint32_t index, uint32_t thread) { return GetRegFile(REG_SPR, thread)[index].U64(); }
  void SetSPR(uint32_t index, uint64_t v, uint32_t thread) { GetRegFile(REG_SPR, thread)[index].Set<uint32_t>(static_cast<uint32_t>(v)); }

  Register& CSR(uint32_t thread = 0) { return GetRegFile(REG_CSR, thread); }
  uint64_t GetCSR(uint32_t index, uint32_t thread) { return GetRegFile(REG_CSR, thread)[index].U64(); }
  void SetCSR(uint32_t index, uint64_t v, uint32_t thread) { GetRegFile(REG_CSR, thread)[index].Set<uint32_t>(static_cast<uint32_t>(v)); }

  Register& SCC(uint32_t thread = 0) { return GetRegFile(REG_SCC, thread); }
  uint64_t GetSCC(uint32_t index, uint32_t thread) { return GetRegFile(REG_SCC, thread)[index].U64(); }
  void SetSCC(uint32_t index, uint64_t v, uint32_t thread) { GetRegFile(REG_SCC, thread)[index].Set<uint32_t>(static_cast<uint32_t>(v)); }

