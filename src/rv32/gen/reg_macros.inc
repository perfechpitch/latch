// Generated —— 寄存器堆 DSL 宏(jsonc `registers` 块);body/.inc 里 N[idx] 的来源。
// 每个有访问器的堆(name=N、key=REG_X)→ `#define N system->GetRegFile(REG_X)`。改堆请改 jsonc 后重建。
// 只在 START_INST_CODE_DEFINE 内由 instance 头 #include(system 在作用域);accessor:false 跳过。

#define SR system->GetRegFile(REG_SR)
#define SPR system->GetRegFile(REG_SPR)
#define CSR system->GetRegFile(REG_CSR)
#define SCC system->GetRegFile(REG_SCC)
