# 第 8 章　文档缺口与 TBD

**模式**：design（陈述当前设计，动机与取舍收在各节的“取舍”段落里）

登记原始设计文档之间的口径冲突、标注为空缺的内容、引用了但本地没有的文档，以及设计上仍未确定的问题。
动手建模前先扫一遍，凡是本章列出的取值都不要当作已定。

《Bach 硬件设计建模参考》第 8 章，全套目录见 [README](README.md)。

***

## 口径冲突

同一项在不同原始文档里给了不同的值，都需要找设计者确认。“建议”一栏是本文档作者的取法，不是设计结论。

| 项 | 说法 A | 说法 B | 建议 |
| - | - | - | - |
| RV core 特权态 | MAS_TOP：“仅支持 U 态（用户态）。没有 CSR？” | RV Core MAS：“支持 M 态，支持所有 M 态 CSR；不支持 S、U、H” | 按 RV Core MAS（M 态） |
| Router ↔ Router 带宽 | 性能需求规格 V0.5：128 B/T | Router MAS：每方向 256 B、双向各 256 GBps | 按 Router MAS，性能公式需同步换算 |
| VU 访存带宽 | 性能需求规格：B_VU = 64 B/T | VU MAS：每周期 1 Load + 1 Store 各 128 B | 按 VU MAS |
| VU 能否直接读 Matrix Mem | 软件流程梳理：R core 链二由 VU DSA 直接从 Matrix Mem 读两路数据做 reduction（[`02_软件流程梳理/d13.png`](<../../../../perfechpitch/Bach软件文档库/01_Bach软件文档库/04_总体设计/02_软件流程梳理/d13.png>) 画的就是 matrix mem → VU dsa → core mem） | MAS_TOP：“VU 不能直接读 Matrix mem”；EP 组间 Reduction 讨论与 core 内调度机制：DTE 先把两笔从 MM 搬到 CM，VU 再求和（[`04_core内调度机制/d40.png`](<../../../../perfechpitch/Bach/02_二、需求分析/06_第四阶段需求分析（Core Level需求分析）/04_core内调度机制/d40.png>)） | 按 MAS_TOP（多一步 DTE 搬运），R core 链二因此是 4 个 task 而不是 3 个。这一步直接改变 R core 的 CM 容量与带宽需求 |
| PPTP 下 silu·dot·量化 落在哪一段 chip | 软件流程梳理伪代码：FC1/FC3 的 reduce 结果都落到 **FC2 段** chip 的 core 0，dot 在那里做 | 本文档 `pptp_nk` 角色表：dot 在 **FC3 chip** 的 `pptp_fc3_nk_dot_core`；板卡结构和模型映射：Silu 在 **FC1 chip**、dot 在 **FC3 chip** | 三处把 VU 的活摆在不同 chip 上，直接改变每段 chip 的 VU 占用与跨段传输量。**未解**，建模前必须定死一种 |
| B / R core 的 datain 侧是几个 task | 软件流程梳理：两个 task，DTE 搬完再由另一个 task 置 flag / 更新 `arrive_num` | core 内调度机制：一个异步 `datain_task`，要求 DTE DSA 搬完时顺带置标志 | 影响 RV core 的占用拍数，性能模型里差一个 task 的调度开销。建模先按两个 task（软件流程梳理更具体），确认后再收敛 |
| Core Mem 容量 | MAS_TOP 内存结构表：512KB / 1MB | Cmem MAS：1MB + 32KB（8 bank） | 按 Cmem MAS |
| Matrix Mem bank 数 | MU MAS：32 个 Mmem Bank 与 32 个物理 Lane 一对一 | Mmem MAS：按 64 个 lane 分成 64 bank | **未解**，直接影响 8KB/T 的组织方式 |
| MU 计算流水深度 | 参数表：执行拍数 / 流水延时 9T | Matrix exe 章节：单 Lane 内深度 10 级 | 差 1 拍，可能是含 / 不含某一级 |
| loop_bp 项数 | Key features：最多 4 项 | 参数列表：4/8 | 未定 |
| `STALL_COMPUTE_ON_CREDIT_MISS` | Top 模拟器详设正文：默认 `true`（credit 不足时本核停算） | 同文档仿真参数表：默认 `false` | **未解**，影响 credit 阻塞时的吞吐建模，两种模式文档建议做对比仿真 |
| VU 读 CoreMem 带宽 | Top 模拟器：`vu 读/写 core mem = 256 B/T` | 一体化模拟器：`VU_Dsa 访存端口 CoreMem bw=64B`；VU MAS：128 B | 三处不一致，**建模取 VU MAS 的 128 B** |
| chip 内 core 网格 | 硬件 MAS / 需求分析：2×4（Harvest 后 2×5） | 两套模拟器一律按 **2×5** 建模 | 模拟器口径已含 Harvest，按 2×5 |
| map 文件里的 core 网格 | `.map` 示例 meta：`core_cols_per_chip: 4` | 模拟器基准配置：`CORE_COLS_PER_CHIP = 5` | 示例 map 是旧的 2×4 版本 |

## 原始文档里标注为空缺或待定的内容

| 模块 | 缺什么 |
| - | - |
| Bach_core_MAS_TOP | 整篇标 `pending`。Glossary 全空；DSA / Cmem / Matrix mem / Share mem 的异常类型全空；“如何上报”未写；地址空间分配只有白板引用没有正文；“软硬件编程契约”章节大量只有标题（Kernel 三段式代码拆分原理、统一偏移地址编程规范、MOE 场景的 Boot / Launch / 动态描述、动态专家调度、Gdb、jtag） |
| Router MAS | Programming Model 起（Register Map、Interrupts、ISA、Programming Sequence、Locking、Performance、Power、Area）**全部是 eFUSE 模板残留**，与 Bach 无关。Signal List 也是 eFUSE 的 AHB 信号。“业务 Credit 的路由”正文只引用了另一篇未拉取的文档《Harvest下业务级Credit的路由机制》 |
| TS MAS | 标 `编写ing`。Signal List 表格大量字段空白（宽度、方向）；`EXCEPT_CHECK` 和 `IO_REG` 两节的 Interface 与 Timing 是 eFUSE 模板残留；Performance / Power / Area 全部是模板 |
| RV Core MAS | Signal List 空；各子模块的 Interface (LLD) / Timing Diagram 基本空；CSR 具体实现哪些“待讨论”；B core 软件流程标题重复且内容为空；Performance / Power / Area 只有标题 |
| Cmem MAS | Function Description 整章只有标题（regfile / agu / acu / ldq / stq / Matrix exe，且这些标题明显是从 MU 文档复制的，与 Cmem 无关）；Performance Targets 表格空；Memory List 是 eFUSE 模板残留 |
| Mmem MAS | Function Description 同样只有标题 |
| MU MAS | 大量章节是注释形式的写作提纲（以 `> //注释：` 开头）；寄存器描述全部指向内嵌 sheet，本地拉取的 markdown 里只有 `<sheet>` 标签没有内容 |
| ISA 描述表 | `Vector` 和 `DTE` 两个 sheet **完全为空**；`Matrix` sheet 里是通用模板样例（ADD/SUB/LW/SW），不是真实的 MU 自定义指令 |
| 寄存器描述表 | “地址总概”只有分类没有地址范围；`TS` sheet 为空；`Router` sheet 只有三个条目名称（驻留专家掩码、Message id 记录、路由方向表），无地址无位域 |
| Shared DM | 整篇只有一个标题，无任何内容 |

## 引用了但本地没有的文档

正文里 `<cite>` 引用、但不在已拉取范围内的：

* 《Harvest 下业务级 Credit 的路由机制》（Router“业务 Credit 的路由”一节的全部内容）
* 《TS_通信机制》（TS 的 Programming Sequence 指向它的 5.3 节场景映射）
* 《MU/DTE 寄存器配置参数》（DTE Programming Model）
* 《DTE DSA 副本》《DTE DSA》（3 Lane 评估的对比材料）
* 《Mmem(Matrix Mem) 详细设计文档模板》
* 各文档内嵌的 whiteboard 图与 sheet 表格。本地 markdown 里只留了 `<whiteboard token="...">` / `<sheet token="...">` 标签，**图和表的实际内容没有随正文导出**。Router、TS、DTE 的关键框图与时序图都属此列

> 如果建模过程中需要这些内容，可以按 token 单独去飞书拉：whiteboard 用画板导出接口，内嵌 sheet 用表格读接口。这些都在已授权范围内。

## 设计上仍未确定的问题

**Router**

* 整包传输方案下“长包阻塞可能有死锁场景，需要在架构层考虑不会出现死锁”，死锁避免的具体论证未写

**TS**

* concat 的 task 软件配 1 个还是多个（若配 1 个，需在任务链中指明 `exe_num`）
* DTE task 的三种优先级用固定优先级，还是“reissue 最高 + 其余按最老用户”
* 哪些 task 该硬化进 TS。原文的设想是“所有与用户和 core mem 分配无关的 task，都可以采用硬化 task
  在 TS 的方式（包括 broadcast 重发），只有与用户强相关的任务链才会在 stream 表里创建和工作”，
  尚未定下界线

**RV Core**

* loop 分支未到最大次数就退出时，loop_bp 表项无法释放怎么解决
* 普通条件分支是否需要 gshare 类预测器
* `boot_pc` 与异常入口是否为软件可配的 I/O 寄存器
* lsq 是否需要记录 store 类指令
* DTCM 的 2-bit ECC 能否按异步非精确处理

**MU**

* 软件希望访存地址 byte 对齐，但 256B 数据块宽度的地址偏移会增加 buffer 设计复杂度（输入输出要移位拼接）

**VU**

* MXFP8 的块大小与 data / scale 映射关系 TBD
* Vector 数据广播场景是否值得支持乱序调度 TBD
* RS 随机舍入是否实现待确认

**DTE**

* CM → MM 方向后续是否要支持仍是遗留问题，当前倾向不支持
* LUT 的字段展开与转换等软件仿真结果出来后再固化

**Core Mem**

* DTCM 是否要做，取决于 RV core 访问 SM 的延迟是否满足要求
* ITCM 溢出处理，当前结论是“不会有溢出场景”

**专用 core（B core / R core）**

* R core 的 Matrix Mem 要不要额外的 credit 机制。原文的疑问是“Reduction Core 的 Matrix Mem
  与 Broadcast Core 的 Matrix Mem 对应的话，不需要额外 Credit 机制保证？”，并附了一条追问：
  总槽位数是否要按输入 / 输出能支持的**较小**那个来算，否则进得多、出得少仍会缺 credit

***

本套文档的两处来源与原始文档的 wiki 链接见 [README](README.md)。本章内容生成于 2026-08-20。
