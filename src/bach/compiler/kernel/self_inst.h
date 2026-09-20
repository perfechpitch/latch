/* self_inst.h - RISC-V 自定义指令 (opcode = 0x0B, custom-0) 编码宏
 * 宏名与汇编助记符一致, 参数顺序与汇编语法一致(6 条指令):
 *   dsaw  rs, addr                  寄存器寻址写 DSA 单寄存器  (funct3=001, bit31=0)
 *   dsawi rs, reg_i_addr            立即数寻址写 DSA 单寄存器  (funct3=001, bit31=1)
 *   dsar   d, addr                  寄存器寻址读 DSA 单寄存器  (funct3=000, bit31=0)
 *   dsari  d, reg_i_addr            立即数寻址读 DSA 单寄存器  (funct3=000, bit31=1)
 *   loop  max_cnt, cur_cnt, offset  自定义循环分支             (funct3=110)
 *   task_done ts                    task 完成标志               (funct3=010, bit31=TS)
 *
 * 编码实现说明:
 * 全部用 gas 的类型化 `.insn r opcode, funct3, funct7, rd, rs1, rs2` 形式,
 * 让源/目的寄存器经 "%0" 等 asm 操作数由编译器自由分配 (不写死 a0/a1/a3,
 * 也不再需要先 mv 到固定寄存器、不 clobber 调用者寄存器)。
 * 立即数指令 (dsawi/dsari/loop) 中被立即数占用的 rd/rs2/funct7 字段是常量,
 * 经 "n" 约束做文本替换拼成 x<编号> 寄存器名 / funct7 数值注入编码,
 * 故立即数必须是编译期常量; 运行期地址请用寄存器寻址的 dsaw/dsar。
 *
 * 《RV Core自定义指令详细设计》的代码实现。模型侧译码见
 * src/bach/ip/chip/core/rv_core/custom0.h，两侧字段布局须保持一致。
 */

#ifndef SELF_INST_H
#define SELF_INST_H

/* 立即数占着指令字里的字段, 只能由编译器在编译期拼进编码; 传运行期值时报的是
 * gas 那句没有指向性的 "impossible constraint in 'asm'", 看不出该改什么。
 *
 * 只有 task_done 挂得住这个校验: 它的参数按定义就是一个字面常量 (0 / 1), 不存在
 * 误报。dsawi / dsari / loop 的立即数不能用同样办法校验——__builtin_constant_p
 * 在 gimplify 阶段折叠, 比 gas 的 "i" / "n" 约束早得多, 像
 * dte_template(0) + DTE_SRC_ADDR 这种「static inline 函数 + 常量」的写法 gas 收
 * 得下 (约束在 RTL 展开时才折叠), 这个校验却会误报。所以那两个只留注释说明:
 * 立即数要写成编译器 fold 得动的常量, 运行期地址改用 dsaw / dsar。
 *
 * 只声明不定义: 常量能折叠掉这个调用, 真被调用说明传的不是常量。 */
extern void self_inst_ts_must_be_constant(void)
    __attribute__((error("task_done(ts): ts 必须是编译期常量 0 或 1, 它编码在指令的 bit31 里")));

/* ---- 寄存器寻址写 DSA 单寄存器: dsaw rs1, rs2 (funct3=001, bit31=0) ----
 * rs1 = 写数据, rs2 = DSA 字节地址, 均由编译器分配 */
#define dsaw(rs, addr) \
    __asm__ volatile( \
        ".insn r 0x0B, 1, 0, x0, %0, %1" \
        : : "r"(rs), "r"(addr))

/* ---- 立即数寻址写 DSA 单寄存器: dsawi rs1, reg_addr (funct3=001, bit31=1) ----
 * rs1 = 写数据 (编译器分配); imm[15:5] -> [30:20], imm[4:0] -> rd 字段。
 * 按 R-type 字段拆分: funct7 = bit31|imm[15:10], rs2 = imm[9:5], rd = imm[4:0],
 * 三者均为常量, 经 "n" 文本替换注入; rs1 经 "%0" 由编译器分配。
 * reg_addr 要写成编译器 fold 得动的常量 (字面量、宏、static inline 函数加常量都
 * 算), 否则报 "impossible constraint in 'asm'"; 运行期地址用 dsaw。 */
#define dsawi(rs, reg_i_addr) \
    __asm__ volatile( \
        ".insn r 0x0B, 1, %1, x%2, %0, x%3" \
        : : "r"(rs), \
            "n"(0x40u | ((((reg_i_addr) & 0xFFFFu) >> 10) & 0x3Fu)), \
            "n"((reg_i_addr) & 0x1Fu), \
            "n"((((reg_i_addr) & 0xFFFFu) >> 5) & 0x1Fu))

/* ---- 寄存器寻址读 DSA 单寄存器: dsar rs1, rd (funct3=000, bit31=0) ----
 * rs1 = DSA 字节地址, rd = GPR 写回目标, 均由编译器分配 */
#define dsar(d, addr) \
    __asm__ volatile( \
        ".insn r 0x0B, 0, 0, %0, %1, x0" \
        : "=r"(d) : "r"(addr))

/* ---- 立即数寻址读 DSA 单寄存器: dsari rd, reg_addr (funct3=000, bit31=1) ----
 * rd = GPR 写回目标 (编译器分配); imm[15:0] -> [30:15]。
 * 按 R-type 字段拆分: funct7 = bit31|imm[15:10], rs2 = imm[9:5], rs1 = imm[4:0],
 * 三者均为常量, 经 "n" 文本替换注入; rd 经 "%0" 由编译器分配。
 * reg_addr 同 dsawi: 要 fold 得动, 运行期地址用 dsar。 */
#define dsari(d, reg_i_addr) \
    __asm__ volatile( \
        ".insn r 0x0B, 0, %1, %0, x%2, x%3" \
        : "=r"(d) \
        : "n"(0x40u | ((((reg_i_addr) & 0xFFFFu) >> 10) & 0x3Fu)), \
          "n"((reg_i_addr) & 0x1Fu), \
          "n"((((reg_i_addr) & 0xFFFFu) >> 5) & 0x1Fu))

/* ---- 自定义循环分支: loop rs1, rs2, offset (funct3=110, B 型立即数) ----
 * rs1 = 最大循环次数, rs2 = 当前循环次数 (均由编译器分配)。
 * rs2 >= rs1 时顺序执行下一条指令, 否则跳转到 PC + offset。
 * offset 是编译期常量**有符号字节偏移**, 直接按标准 B 型布局编码
 * (funct7/rd 两个字段承载偏移位, 在宏内联展开, 无需中间宏):
 *   imm[12] -> funct7 bit6(bit31), imm[10:5] -> funct7 bits[5:0]([30:25]),
 *   imm[4:1] -> rd bits[4:1]([11:8]), imm[11] -> rd bit0(bit7),
 * 字节偏移 = sext(imm[12:1]) << 1 (offset 低 bit 隐含为 0)。
 * 有效范围 +-4094, 低 1 bit 必须为 0 (目标 2 字节对齐, 否则取指
 * fetch_access_fault)。
 *
 * 注意: 该形式编译器不感知分支跳转, 适合编码验证 / 手工排布的代码;
 * 真实 C 循环请用下方 loop_goto 标签形式。 */
#define loop(max_cnt, cur_cnt, offset) \
    __asm__ volatile( \
        ".insn r 0x0B, 6, %2, x%3, %0, %1" \
        : : "r"(max_cnt), "r"(cur_cnt), \
            "n"(((((offset) >> 12) & 1u) << 6) | (((offset) >> 5) & 0x3Fu)), \
            "n"(((((offset) >> 1) & 0xFu) << 1) | (((offset) >> 11) & 1u)))

/* ---- 自定义循环分支 (C 标签形式): loop_goto rs1, rs2, target ----
 * 语义同 loop, 但跳转目标是当前函数内的 C 标签 (asm goto)。
 * 编译器感知该回边, 可正确分配寄存器 / 收紧循环体, 是 C 代码的推荐用法:
 *
 *     uint32_t s = 0, i = 0, max = 10;
 *     top:
 *         s += i;
 *         i++;
 *         loop_goto(max, i, top);   // i < max ? 回 top : 顺序执行
 */
#define loop_goto(max_cnt, cur_cnt, target) \
    __asm__ volatile goto(".insn b 0x0B, 6, %0, %1, %l2" \
                          : : "r"(max_cnt), "r"(cur_cnt) : : target)

/* ---- task 完成标志: task_done ts (funct3=010, bit31=TS) ---- */
#define task_done(ts) \
    do { \
        if (!__builtin_constant_p(ts)) self_inst_ts_must_be_constant(); \
        __asm__ volatile(".word %0" \
            : : "i"(0x0000000Bu | (0b010<<12) | (((ts)&1u)<<31))); \
    } while (0)

#endif /* SELF_INST_H */
