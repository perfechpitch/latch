// 由 ISA DSL 工具生成 —— 寄存器布局(jsonc `registers` 块)。
// #include 进 System 子类 ctor body。

AppendRegInfo(REG_NONE, {"NONE", true, {32}, DataType::uint32, -1, 0});
AppendRegInfo(REG_SR, {"SR", true, {64, 1}, DataType::uint32, -1, 0});
AppendRegInfo(REG_SPR, {"SPR", true, {SPR_ENUM, 1}, DataType::uint32, -1, 0});
AppendRegInfo(REG_CSR, {"CSR", true, {CSR_ENUM, 1}, DataType::uint32, -1, 0});
AppendRegInfo(REG_SCC, {"SCC", true, {4, 1}, DataType::uint8, REG_SPR, SPR_SCC});
