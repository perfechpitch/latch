// 由 ISA DSL 工具生成 —— 存储器布局(jsonc `memory` 块)。
// #include 进 System 子类 ctor body(regs.inc 之后、InitMemory() 之前)。

MarkMemoryDeclared();
AppendMemRegion(MEM_MAIN, {"MAIN", 0x0, UINT64_MAX, MemKind::ram, (uint8_t)0x55});
