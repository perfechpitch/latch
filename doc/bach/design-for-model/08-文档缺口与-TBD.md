# 第 8 章　文档缺口与 TBD

**模式**：design（陈述当前设计，动机与取舍收在各节的“取舍”段落里）

登记四类内容：

* 原始设计文档之间的口径冲突
* 原文档标注为空缺的内容
* 引用了但本地没有的文档
* 设计上仍未确定的问题

动手建模前先扫一遍，凡是本章列出的取值都不要当作已定。

《Bach 硬件设计建模参考》第 8 章，全套目录见 [README](README.md)。

***

## 口径冲突

同一项在不同原始文档里给了不同的值，都需要找设计者确认。“建议”一栏是本文档作者的取法，不是设计结论。

| 项 | 说法 A | 说法 B | 建议 |
| - | - | - | - |
| RV core 特权态 | MAS_TOP：“仅支持 U 态（用户态）。没有 CSR？” | RV Core MAS：“支持 M 态，支持所有 M 态 CSR；不支持 S、U、H” | 按 RV Core MAS（M 态） |
| VU 访存带宽 | 性能需求规格：B_VU = 64 B/T | VU MAS：每周期 1 Load + 1 Store 各 128 B | 按 VU MAS |
| VU 能否直接读 Matrix Mem | 软件流程梳理：R core 链二由 VU DSA 直接从 Matrix Mem 读两路数据做 reduction（[`02_软件流程梳理/d13.png`](<Bach软件文档库/01_Bach软件文档库/04_总体设计/02_软件流程梳理/d13.png>) 画的就是 matrix mem → VU dsa → core mem） | MAS_TOP：“VU 不能直接读 Matrix mem”；EP 组间 Reduction 讨论与 core 内调度机制：DTE 先把两笔从 MM 搬到 CM，VU 再求和（[`04_core内调度机制/d40.png`](<Bach/02_二、需求分析/06_第四阶段需求分析（Core Level需求分析）/04_core内调度机制/d40.png>)） | 按 MAS_TOP（多一步 DTE 搬运），R core 链二因此是 4 个 task 而不是 3 个。这一步直接改变 R core 的 CM 容量与带宽需求 |
| PPTP 下 silu·dot·量化 落在哪一段 chip | 软件流程梳理伪代码：FC1/FC3 的 reduce 结果都落到 **FC2 段** chip 的 core 0，dot 在那里做 | 需求分析的 `pptp_nk` 角色表：dot 在 **FC3 chip** 的 `pptp_fc3_nk_dot_core`；板卡结构和模型映射：Silu 在 **FC1 chip**、dot 在 **FC3 chip** | 三处把 VU 的活摆在不同 chip 上，直接改变每段 chip 的 VU 占用与跨段传输量。**已定**：按软件流程梳理那一档，三步统一落在 FC2 段 chip 的逻辑 core 0 |
| 进核那一笔的目的地址由谁给 | DTE MAS：Header Parser 解析入站包头就生成 Descriptor，`dst_addr` 取自包头，搬运随即开始 | EP 组间 Reduction 讨论与软件流程梳理：R core 的 datain 任务里软件先读 `arrive_num`，结合 `(gpu_id, token_id)` 算出 Matrix Mem 存放位置再配给 DTE DSA。系统软件需求分析的 weights 加载那一段同样：DTE core 跑 weights loader，标量指令算出这一片落 Matrix Mem 的地址，再由 DTE 指令把数据从 Router 搬进去 | **已定：按 DTE MAS**，落点取自包头，发方在出核造包时写进去。后者要收方软件先算再搬，而软件被下发时数据已经在搬了。R core 一个用户的两笔落进同一个槽的哪一半，因此由发方按自己在链上的位置决定；weights 加载阶段落点由 Host 那一侧按 core 与分片算好写进包头，收方的 loader 只数搬进来几笔，数够了中断 SCP |
| B / R core 的 datain 侧是几个 task | 软件流程梳理：两个 task，DTE 搬完再由另一个 task 置 flag / 更新 `arrive_num` | core 内调度机制与软件计算流程详细评估（GLM5 章 B core 伪代码）：一个 `DATAIN_TASK`，DTE DSA 搬完时顺带置标志并推进 head 指针 | 按一个 datain_task（两处较新的文档一致） |
| Core Mem 容量 | MAS_TOP 内存结构表：512KB / 1MB | Cmem MAS：1MB + 32KB（8 bank） | 按 Cmem MAS |
| DTE ↔ Cmem 接口宽度 | DTE MAS 旧版：256B + 8B（Data + scale） | Cmem MAS：256B/T | **已消除**。DTE MAS 现版改成「与 Cmem 接口宽度 256B/T \* 2（双向）」，两边一致 |
| DSA 配置指令一次写几个寄存器 | ISA 描述表：`dsaw.d` / `dsawi.d` 写 2 个；软件计算流程详细评估的伪代码仍在用 `dsawi.d` | RV Core MAS：每条最多配置 1 个 DSA 寄存器；软件计算流程详细评估的**指令表已改成只有 `dsaw` / `dsawi` 单寄存器写** | **已定：单寄存器写**。软件侧的指令表与 MAS 已经对齐，只剩 ISA 描述表的编码和同一篇里的伪代码没同步。模型把 `.d` 展开成两条单寄存器写 |
| `task_done` 的 FC 标志 | ISA 描述表的编码里有 FC 位；RV Core MAS 的“task完成指令”一节仍写着 TS 与 FC 两个标记，FC 带 fence 语义 | 软件计算流程详细评估已改成 `task_done ts`，连同“后续 task 要读本 task 写进 shared_mem 的数据可用 fc 做 fence”那段用法一起删掉；RV Core MAS 的 Features 一节也只列 TS 标志 | 按软件侧与 Features，**不建 FC**。**冲突仍在**：MAS 正文那一节没跟着删，而 `fence` 指令本身实现为 nop，所以 MAS“内存一致性”一节里“用 fence + task 完成通知 TS 隔离两个 task 的数据相关”这句现在没有对应的硬件手段，跨 task 的隔离只剩任务链的全序 |
| `flag_check` 指令 | ISA 描述表有编码；RV Core MAS 的 `user_id` CSR 描述仍提到“R core 执行 flag_check 对应的 task” | RV Core MAS 的 Features 已删除“内存 flag 查询指令” | 保留建模（B core / R core 轮询映射表靠它），待设计者确认 |
| 用户号是一个还是两个 | RV Core MAS 的“task 完成通道”一节把回给 TS 的那一项叫 `local_user_id`，“用户在这个 HBU 流控范围内的编号”，说它用于 R-core 用户映射表和 Matrix Mem 地址计算 | 同文档“自定义 CSR 寄存器”一节把同一个东西叫 `user_id`：“用户在 HBU 流控范围内的 local user_id，12 bit，可读写，传统业务流下由 TS 的 task 开始执行时从 task_queue 硬件写入 CSR，R-core 执行 flag_check 对应的 task 时由软件写入”；Task Scheduler MAS 的 stream_table 表项清单里只有一个 `user_id` | **已定：一个**。RV core 的自定义 CSR 只有 `stream_id`、`user_id`、`task_id` 三个，可读写的那个就是 `user_id`；stream_table 也只有一个 `user_id` 字段。TS 下发时硬件把表项的值写进 CSR，自启动的 core 上软件认出用户后写进同一个 CSR、随 `task_done` 回 TS 更新同一个字段。两条写入路径写的是同一个东西，不存在两个编号 |
| 用户号有几位 | Task Scheduler MAS 的画板与旧版接口：10 bit（`user_id[9:0]`）；TS LLD v0.5 的 Stream Map 表项 `UID_W = 10`，而它的对外接口表是 16 位 | RV Core MAS 的自定义 CSR：“有效位宽暂定为 12bit” | 原始文档里只有这两处给了位宽。Router 的包格式那一节把 `user_id` 列在“硬件相关的其余字段”里，没写宽度。10 bit 那一档在接口表与画板上多处一致，12 bit 那一档原文自带“暂定”。**要问设计者**：用户号的值域多大，一个 LPU 里同时在飞的用户上限是多少。建模先按 16 bit 建，装得下这两种取值 |
| 取指 / 访存不对齐异常的归属 | RV Core MAS decode 优先级表：addr misalign 单列一类 | 同文档 ITCM 与 Exception 两节：不对齐归入 access fault | 按 decode 优先级表 |
| RV core 的 C / A / F / D 扩展 | RV Core MAS Features：C 支持、**A 考虑支持**、F 与 D 不支持；MAS_TOP：RV32IMC；同文档“标准指令集”小节仍是“是否支持 C？是否支持 AFD？”的问句 | 软件计算流程详细评估：**RV Core 目前支持的指令集为 RV32IMAC**，A 已经算进去 | 按 Features 建，**A 先不建**。这条影响 Share Mem 的多核一致性怎么做：三个 RV core 共享 Share Mem，A 一旦确定支持，task 之间的同步就有原子指令可用 |
| DTE 的软件接口 | DTE MAS：Task Descriptor（`TASK_CFG_ADDR` / `TD` / `PACK` / `TRG`）+ Trigger 序列 | 软件计算流程详细评估：一套按字段命名的寄存器；《DTE 寄存器配置参数》：第三套，`DTE_BASE` 起三段地址空间加 `template[0..3]` 寄存器模板，字段名与第二套对得上并多出模板 / 动态的分档 | 软件接口按《DTE 寄存器配置参数》建（地址空间与模板已写进《DTE 数据搬运引擎》），完成机制按 DTE MAS。三套的字段仍未逐条对齐 |
| 入站包头的字段清单 | DTE MAS Header Logical Fields：`version` / `header_len`、`packet_type` / `route`、`dst_addr`、`byte_count`、`task_id` / `stream_id`、`attributes` / `reserved`；软件计算流程详细评估的 MSG 包结构：硬件包头 8 B（包头标记 2 B + Router 信息 4 B + 包长度 2 B） | 《Core 间数据流通信机制》：**硬件包头 16 B + 软件包头 16 B = 32 B**，逐字节给全（Reserved 7 B、Hardware Used 1 B、UserID 2 B、Reserved 1 B、PathID 1 B、CoreMask 2 B、size 2 B），与 DATA_NOC HAS 的 Header 32 B 对上 | **按《Core 间数据流通信机制》建**，它是三份里唯一逐字节给全的，且总长与 HAS 的 32 B 一致。DTE MAS 自己声明“具体 Header 位域仍以 Router 接口规范为准” |
| DTE 的启动方式 | DTE MAS 旧版：有“TS 快速启动流程”（TS 绕过 RV core 直接启动 DTE），标 P1 优先级 | 软件计算流程详细评估：只保留 DTE core 配置任务给 DSA 这一种 | **已定**。DTE MAS 现版把「TS 直接启动 DTE DSA」这一条整条删掉，启动方式只剩 DTE core 配置任务给 DSA |
| 包头与 topK 的存放位置 | DTE MAS：包头可写 DTE 内 Header mem 或 Cmem 独立空间；topK 可写 MU 的 `topK_ep_table` 或 Cmem 独立空间 | 软件计算流程详细评估：计算 core 的包头存 DTE 内独立 mem，B core / R core 存 Core Mem；topK 由 DTE 复制到单独的 mem 供 MU 读 | **已定**。《MU / DTE 需求整理和遗留问题分析》20260825：硬件包头与软件包头合并成一张 288 B 的表按 `stream_id` 索引，存 Hmem 还是存 Core Mem 由 `hw_header_addr` 这个地址本身选，普通计算 core 走 Hmem、B core / R core 走 Core Mem |
| B core 查询 ready 的 task 由谁执行 | 软件计算流程详细评估 GLM5 章：VU 做 check flag | 同文档 TS 章的 B core 示例：task0 为 MU | 按 GLM5 章（VU），待确认 |
| `SELF_START` 的适用范围 | TS MAS 旧版：仅 B core 才会有 | 软件计算流程详细评估 R core 示例：task0 `self_start=1` | **已消除**。TS MAS 现版的 `SELF_START` 是全局寄存器，描述写“当前场景只有B core\R core会出现这种工作模式” |
| Router 的 R2R 方向端口 | Router MAS：五方向互连，上、下、左、右及 Core | DATA_NOC HAS：left / right / mid 三个方向端口，加 local 与 reduce_0/1/2，5 入 7 出 Crossbar | 按 HAS 的三方向。两行的拓扑里邻居就是同行左右加另一行对称位，且与第 3 章顶层节的 data_L / data_UD / data_R 三通道一致 |
| Reduce 的累加做在哪 | Router MAS：ReduceModule 在 Router 内，16 用户 × 32 KiB 上下文，RMW 原位累加，自己维护下游 Reduce Credit | DATA_NOC HAS：Router 内不设 Reduce Buffer，累加由独立的 Rmem 子系统完成，reduce credit 是单独的流控网络 | 按 MAS：ReduceModule 在 Router 内 |
| stream credit 表谁是唯一有效状态 | Router MAS：Router 维护的 User Resource Allocation Table 是唯一有效状态，DTE 持 cache | DATA_NOC HAS：Router 输出单元与 core 内各持一份 credit table，靠 credit release 接口同步 | 按 MAS：Router 唯一有效 |
| 出核前查资源的监听队列在谁那里 | Router MAS 一处：功能已转移到 DTE 中 | 同一份 MAS 另一处：详写 Router 上 16 项全相连监听事件队列与完整申请流程 | 按后者：在 Router |
| Router 的仲裁粒度 | Router MAS 有一节“Interleave 和整包的对比”，只列两案优劣、未给结论 | MAS 正文与 DATA_NOC HAS 都是 flit 级（“Packet 在 VC 间按照 Flit 的粒度传输”“矩阵仲裁以 flit 为基本节拍”） | flit 级，整包只作为贪婪仲裁的优先级偏好。原第 3 章写成“整包粒度（选定）”是误读，已更正 |
| R2R 带宽 | Router MAS：相邻 Router 双向各 256 GB/s @1GHz | DATA_NOC HAS：R2R 210 GB/s、C2C 90 GB/s | 两个都记：256 GB/s 是 256 B/T @1GHz 的接口理论值，210 GB/s 是 HAS 记的有效带宽。是否同一口径待确认 |
| 重发（reissue）任务在不在主任务链上 | TS MAS 与 core 内调度机制：重发 task 不在主线任务链上，可并行执行、优先级最高（TS MAS 里“重发的 task 任务优先级最高”这句自己划了删除线） | Top 模拟器详设的三条【讨论结果】：插在任务链中实现，同一 stream 下即使无依赖也按任务顺序执行，需等它完成才能执行后面的任务；软件计算流程详细评估的 normal core 示例把 broadcast reissue 排成 task 1' | **已定：在链上**。TS MAS 现版把重发的搬入与搬出配成链上的项（`TASK_TYPE` 1～3），“重发的 task 任务优先级最高”一句与 DTE 优先级的方案 1 都划掉了，DTE 发射全部按最老用户仲裁 |
| TS 的时延数字 | TS MAS：task 唤醒延迟 2～3 cycle | Top 模拟器详设：调度间隔 16 T、需与 Router 通信 21 T、retire 5 T；TS LLD v0.5：各模块当拍组合、下一上升沿写入 Stream Map，流水级波形从完成到下一项被 MU 收下是 C0～C3 四拍 | 三者口径不同不是矛盾：MAS 是硬件目标，模拟器是含 RV core 往返的端到端值，LLD 是逐级拍数。《TS 任务调度器》按这三层分别记 |
| 进 core 的数据缓存在哪 | Router MAS：CoreStation 内的 HeaderFIFO 与 OutputBuffer，只给职责不给深度 | DATA_NOC HAS：Router 内 local 端口不设 buffer（容量栏写“—”，注明“buffer 在 DTE-local 桥接”），缓冲在 Router 外的 DTE-local 桥接模块，Router→DTE 60 flits × 288 B ≈ 16.9 KB | 结构按 MAS（不设独立桥接模块，逻辑拆到 core 与 Router 两侧），**容量按 HAS 的 60 flits** |
| Router 的 Credit Bypass Route | Router MAS F-018～F-020：20 bit 的 `RTR_RELEASE_ROUTE`，按四个输入方向分四组，每组 5 位，分别表示转到上、下、左、右与 Self（Core）；Stream 与 Reduce 两类 release 只按它转发 | 软件计算流程详细评估 Router 章：删除了该配置小节，软件只配 RouterTable；但同章检查清单与释放表仍引用 Credit Bypass Route | 按 Router MAS。R2R 端口按“Router 的 R2R 方向端口”那一条定成三个方向，所以模型是三个输入方向各 4 位（mid、left、right、本级） |
| Matrix Mem bank 数 | MU MAS：32 个 Mmem Bank 与 32 个物理 Lane 一对一 | Mmem MAS：按 64 个 lane 分成 64 bank | **未解**，直接影响 8KB/T 的组织方式 |
| MU 计算流水深度 | 参数表：执行拍数 / 流水延时 9T | Matrix exe 章节：单 Lane 内深度 10 级 | 差 1 拍，可能是含 / 不含某一级 |
| loop_bp 项数 | Key features：最多 4 项 | 参数列表：4/8 | 未定 |
| `STALL_COMPUTE_ON_CREDIT_MISS` | Top 模拟器详设正文：默认 `true`（credit 不足时本核停算） | 同文档仿真参数表：默认 `false` | **未解**，影响 credit 阻塞时的吞吐建模，两种模式文档建议做对比仿真 |
| VU 读 CoreMem 带宽 | Top 模拟器：`vu 读/写 core mem = 256 B/T` | 一体化模拟器：`VU_Dsa 访存端口 CoreMem bw=64B`；VU MAS：128 B | 三处不一致，**建模取 VU MAS 的 128 B** |
| chip 内 core 网格 | HAS 汇总版“Harvest 规则”：每 HBU 内 **2×5** 个 Bach Core，按坏核数分 A / B / C 型，坏 >2 个废弃；保证至少 8 个可用，**多于 8 个的富余 core 作特殊功能用**；板级左右两列只能 A / B 型。硬件 MAS / 需求分析同口径 | 两套模拟器一律按 **2×5** 建模；《仿真评估工作》里 Pysim 的基准 map 是 **2×4** 的 chip、B core 与 R core 另加 | **已定：按列位置分两种形状**。中间列 chip 2×4 共 8 个 core，第一列与最后一列 chip 2×5 共 10 个，多出的一列放 B core / R core 与一个不派角色的 core。Harvest 方案作废，没有坏核余量，每颗 chip 一律 8 个计算 core |
| map 文件里的 core 网格 | `.map` 示例 meta：`core_cols_per_chip: 4` | 模拟器基准配置：`CORE_COLS_PER_CHIP = 5` | 两个都对，只是各说一种 chip：中间列是 4 列，两侧是 5 列。这一项要按 chip 逐颗给，不能配成全局常数 |
| VC 数 | DATA_NOC HAS 正文与 VC Buffer 表：每 Input Port V = 4 | 同一份 HAS 的 VC 使能 mask 20-bit、`vc_id` 5-bit、Area 预算按 VC0–19（4×20 + 16×2 + shared 20 = 132 flits/port） | V = 20 是 2026/08/19 缩减 VC 之前的残留，但 Area 与 Architectural Guidelines 两节没同步。按 V = 4 建 |
| VC private 深度 | HAS 3.2.2 与 REQ-ARCH-025：Private per-VC 深度 = 2（防死锁），软件可配 | 同一份 HAS 的 VC Buffer 结构表：Private ~20 flits/VC（覆盖 RTT），总量 4×20 + shared 20 = 100 flits/port = 25 KB | **已定：按 20**。HAS 的 VC Buffer 规格表是缩减到 V=4 之后的正式口径（`V=4`、`Private(VC0–3) ~20 flits/VC`、`总 4×20+20=100 flits/port=25 KB`）；`2` 是 REQ-ARCH-025 的软件可配下限，也是已删除的 VC4–19 那一档的深度，HAS 正文写作「极限情况……保证每 VC 基本传输需求」 |
| R2R 单跳延迟 | DATA_NOC HAS 性能预算：internal 6 ns + wire 10 ns = **16 ns/hop**，mid 无走线延迟 | 第 5 章延迟表与性能需求规格：T_R2R = **40 T** | **倾向 16 ns**。HAS 新版新增 ASM-03「R2R round trip 最大不超过 20 cycle，单向 C2C latency 最大不超过 300ns」，单跳约 10 cycle 以内，与 16 ns @1GHz 一档相符；40 T 对不上这条约束。待与设计者确认 40 T 是不是含 core 侧往返的端到端值 |
| Rmem per-port buffer | HAS ASM-07：per-port **128 flits** | 同一份 HAS 的 Area 预算：Reduce 子系统 3 port × **32 flits** | 未解 |
| ReduceBuffer 容量 | 《通信机制（分析过程）》：单用户最大 reduce 数据量 8K × FP32 = 32 KB，正反双份 = 64 KB；《Core 间数据流通信机制》：16 用户 × 32 KB = **512 KB** | Router MAS F-044：16 用户 × **32 KiB** = 512 KiB，“容量统一按 FP32 驻留数据量计算。一个 32 KiB 分区最多保存 8192 个 FP32 元素，对应 16 KiB 的 BF16 输入数据”；《TS_通信机制》：每用户 **16 KiB** | **已定：每用户 32 KiB，按 FP32 驻留算**（Router MAS F-044）。《TS_通信机制》的 16 KiB 与 MAS 按 BF16 输入量折算的数一致，它给了拆分的理由：单用户 reduce 次数不定，Rmem 装不下全空间，**要求软件把一笔 reduce task 拆成 4 笔 8 KB 的 reduce task** |
| CoreMem credit 粒度 | 《通信机制（分析过程）》：按 **1 KB 粒度**划分，path 按自己需求申请 | DATA_NOC HAS：按 **user 粒度**的资源表格，16 项 | 未解。前者是容量记账，后者是表项记账 |
| `stream_credit`（HAS 旧版叫 `coremem_credit`）初值 | HAS Boot 流程：上电 `stream_credit[port] = 0`，由正常 Core 上电发初始化脉冲逐步初始化 | HAS 4.3.2：`stream_credit` 为 16 个用户的状态表，**默认为全部使能状态** | 按 Boot 流程那一套（上电 0），另一处是描述稳态 |
| Router 与 core 的接口协议 | HAS 正文：五类端口统一 Credit-based，local 也是 credit 流控 | HAS 遗留 action：“目前 router 和 core 通信采用 axi stream，如果可以也建议使用同样的 hflit 和 pflit 协议” | 当前实现是 AXI-Stream-Like，credit-based 是建议方向。按当前实现建，DTE-local 桥接做两侧协议转换 |
| `TASK_EXE_MASK` 的极性 | TS MAS 寄存器表 bit 44：**0 = 按照用户执行，1 = 不按照用户执行**，一位一档 | 同一份 MAS 的 DP+P2P 场景描述：“有些用户只有 P2P 无计算 task、有些是计算无 P2P”，要分出两组就需要两个方向，一位不够 | **已消除**。TS MAS 现版寄存器表不再有 `TASK_EXE_MASK`，功能清单的 DP+P2P 一项整条划掉，trigger 也不再带 `compute` |
| chip 内 mid 接口 | 第 2 章与我们的 Chip 装配：`core[i]` 与另一行对称位置的 core 的 mid 端口全部对接（2×4 是 `core[i+4]`，2×5 是 `core[i+5]`） | HAS 旧版 ASM-01 括号：“中间 router mid 接口不连接” | **已定：连接**。HAS 新版正文改成「简化二维 Mesh（**中间各列连接作为备份通路**）」，并新增 REQ-ARCH-037，用于提供多路径选择 |
| VC Buffer 容量 | 《通信机制（分析过程）》按容量记：reduce 专用 VC3 16 KB、三个共享 VC 各 8 KB、三方向各一套，合计 **120 KB** | DATA_NOC HAS 按 flit 记：private 20 flit/VC × 4 加 shared 20，一个方向 100 flit ≈ **25 KB**，三方向 75 KB | 未解。前者按 reduce 要整包缓冲反推，后者按覆盖 credit 往返反推 |
| ReduceBuffer 容量的第三种口径 | 《通信机制（分析过程）》另一处：Core 必须一次性整包发进 ReduceBuffer，一个 Token 8192 × 2 B = **16 KB** | 同一份文档前文记 64 KB；Router MAS F-044 记 512 KiB | 三个数在同一条链上：16 KB 是单包下界，64 KB 是正反双份，512 KiB 是 16 用户并发。**已定：按 Router MAS F-044**，16 个用户各一个 32 KiB 分区 |
| `TASK_DSA_EN` 位域 | TS MAS 寄存器表 bit 38:37 曾定义 `TASK_DSA_EN`，`0` 只调用 RV core 不调 DSA、`1` 调用 | 同一份 MAS 已把这一整行划上删除线，且没有给替代方案 | **已消除**。TS MAS 现版用 `TASK_RECV_UNIT` 区分：00 只调 RV core，01 调 RV core 与 DSA、两路完成都要。下发字段 `task_dsa_en` 按它取 |
| CM 物理带宽 | 《Core Memory 容量带宽需求推导》：**512 B/cycle @1 GHz**，每用户 50 KiB 写 + 50 KiB 读 | 《通信机制（分析过程）》：**256 B/cycle @1 GHz**，每用户 25 KiB 写 + 25 KiB 读；《Cmem MAS》：**(1 KB + 32 B)/T** | 三份不一致。第 5 章按 512 B/cycle 记，Cmem MAS 的 1 KB/T 是 8 bank 全开的峰值，两者不是同一个口径 |
| Reduce credit 的粒度 | DATA_NOC HAS：core 与 reduce 之间按 user 管，reduce 与 reduce 之间“按照 flit+user 的粒度进行 credit 资源管理”，“reduce 之间 creidt release 粒度为 flit” | Router MAS F-032、F-033 与“Reduce Credit 与传输 Credit”一节：下游 Reduce Credit 以 UserID、目标方向、Reduce 任务为粒度，`reduceNeedMask` 置位时在任务头一次向目标方向提交前取得准入，“后续 Packet 和 flit 复用该准入”；下游做完这笔任务、结果全部交付后还一次 release，“不是逐 flit VC Credit 返还” | **已定：按 Router MAS**。逐 flit 的那一层归 VC credit 管 |
| 逐级 Reduce 的 credit 类型 | 《通信机制（分析过程）》：VC3 专给逐级 reduce，走 VC credit | DATA_NOC HAS：reduce 是独立的 credit 网络，与 VC credit 分离 | **已定：三层各管一段**。HAS 新版设计原则写「三层 credit 流控：Vc credit + stream credit + reduce credit，额外建立单独的逐级流控网络，避免数据超发，减少 vc 使用，避免死锁」。一个 reduce flit 同时受 VC credit（缓冲槽）与 reduce credit（下游上下文）约束 |
| ReduceMemory 的结果按整包还是按 flit 出 | Router MAS F-013：“ReduceMemory 的各输入流和输出流均以完整 Packet 为单位进行仲裁和传输”，“结果也以整包形式输出” | 同一份 MAS“整包输入输出与结果流水”一节：“当某个结果 flit 所需的操作数均已完成累加，且输出端满足接收条件时，该 flit 即可进入输出流水，无需等待整笔任务或整个 Packet 的全部结果生成” | 读作不矛盾：F-013 管的是包与包之间不交织，后一节管的是一个包内部按 flit 流出。模型按后一节，包发出首 flit 后锁定到尾 flit |
| 逐级 reduce 的 N 笔是并行发射还是一笔做完再发下一笔 | 《软件计算流程详细评估》：“一个reduce（32KB）的任务需要拆分到多笔小的reduce（8KB） 任务下发，TS在调度时为了保证reduce的计算延迟，需要并行发射多个任务”；《TS_通信机制》：“根据标识TS可以连续下发多笔reduce 任务？？？” | Router MAS 引 HRD-TS-018：“维护Rmem的credit资源，每个用户只有1个credit资源。TS发射reduce 任务后，credit清零，等到Tmem完成reduce，通知TS，对credit恢复”；同一份 MAS：“TS执行单用户的任务链是串行依赖的，上一个任务完成才能发送下一个任务”；F-030：“当前任务的全部输入处理完成且结果完整发出前，不得接收会覆盖该用户上下文的下一任务” | **已定**。TS MAS 现版把功能清单的 Reduce 一项（`reduce_num`）整条划掉，LLD 写“同一UID最多一笔进行中的Reduce任务”。一笔 reduce 装不下时软件在链上配几项逐级 reduce 任务，一项一包，前一项的 Reduce Done 回来后一项才发 |
| 本级 Rmem credit 是 TS 自己记还是向 Rmem 申请 | Router MAS 引 HRD-TS-019：“TS发现是reduce任务时，请求Rmem的credit，如果满足，reduce可以发射，否则阻塞直到credit满足” | HRD-TS-018：“维护Rmem的credit资源，每个用户只有1个credit资源”；TS MAS 现版的对外通道表里与 Rmem 相关的只有 `rmem2ts_done_ch`（“Rmem完成reduce通知TS finish”），没有向 Rmem 申请的通道 | 按 HRD-TS-018：TS 为每个用户记一份，下发一笔占掉，`rmem2ts_done_ch` 报回来才还。记在哪、复位值是什么原文没写，模型记在 stream 表项上，随表项建、随退休清 |
| `TASK_RECV_UNIT` 的 10 | TS LLD 的 Task Done 一节：“10（Router）等待Router完成，本地ACK不触发完成” | TS MAS 寄存器表：`TASK_RECV_UNIT` 只有 00（只调 RV core）与 01（RV core 加 DSA），“10：无法写入，保持旧值” | 按 MAS。逐级 reduce 任务按 `TASK_TYPE = 4` 认，只收 Router 的 Reduce Done；它的 `TASK_RECV_UNIT` 配 01 |
| TS 任务表项的位域 | TS LLD 参数表：每项 50 bit，“valid1、PC32、send_unit2、recv_unit2、wait_wake1、PID8、Reissue类型1、End1、pid_update1、credit_en1” | TS MAS 寄存器表：`TASK_TYPE[7:5]` 三位六档（PID 更新是第 5 档），另有 `TASK_P2P_REISSUE_TID[13:8]` | 按 MAS |
| TS 的 `ROUTER_TABLE` 位域 | TS LLD 参数表：每项 7 bit，“VCID 2 bit、flow_dir 5 bit” | TS MAS：`TASK_DIR[3:0]`（上下、左、右、本 core）加 `TASK_VCID[5:4]` | 按 MAS |
| TS 下发给 DTE 的 `vcid` 是什么 | TS LLD 接口表 `ts2dtecore_task_vcid`：“DTE任务的VCID操作码：00=Kernel/Weight，01=Transfer，10=Reduce，11=Reduce Twice” | TS MAS 的 `TASK_VCID`：“数据在router上传递时，使用的Virtual channel 通道编号” | 按 MAS：DTE 出核用它选 VC。LLD 另写“credit_en=1的DataOut下发前按Stream Map的实际PID取VCID”“DataIn不查表”，模型对全部 Generated 的 DTE 任务查表，搬入那一格填 0 |
| TS LLD 里留着的旧字段 | TS LLD 的 3.1 与 3.8：Router trigger 带 `task_exe`，Task LUT 有 `task_exe_mask`、`task_B_reissue`、`task_P2P_reissue`、`self_start` 各一列；LUT-FUNC-010 要查“Path匹配唯一性”，3.8.2.3 写“DataIn Path无匹配或多匹配属于配置错误” | 同一份 LLD 的 1.1 功能列表：“按该用户完成位图，选择PID相同且未完成的最低TID作为搬入任务……同PID可对应多个Task”；TS MAS 寄存器表没有这几列，功能清单的 DP+P2P 一项整条划掉 | 按 MAS 与 LLD 的 1.1：同一个 PID 可以对多项，按做完没有依次取 |
| `core_type` 还在不在 | TS MAS 功能清单 1(e)：“支持软件配置本core的类型 core_type：B/R core，普通core”；Block Diagram 的 CFG_REG 一行也列着 `core_type` 与 `B_core_direction` | TS MAS 寄存器表：全局 `SELF_START`（`0x214`）；TS LLD 参数表：“0为Normal，1为Special；替代core_type” | 按寄存器表：`SELF_START` 取代 `core_type`。`B_CORE_DIRECTION` 地址映射里没有，按《TS_通信机制》保留 |
| TS 查不查逐任务的 credit | TS MAS 功能清单 8：“TS识别到任务为 dataout 且需要查credit ，根据任务的stream_id ，path_id，user_id 向Router发起 credit请求”；`TASK_CREDIT_EN`：“表示task 是否需要credit才可以发射，用于做credit问询” | TS MAS 顶层的 `Credit_monitor` 一节整节划掉；对外通道表只剩 `router2ts_credit_release_left/right/up_ch`（描述只写“Router 返回的TS”）与 `ts2router_credit_release_ch`；TS LLD 的 Task Generator 只按 `wait_wake` 置 WAIT 或 READY，Stream Map 一节写“不发起逐任务Credit查询”，`credit_en` 只用来取 VCID | **未解**。模型保留申请与授予两路（`ts2router_req`、`router2ts_credit_ch`），`TASK_CREDIT_EN = 1` 的任务装入时置 WAIT、授予后置 READY；`router2ts_credit_release_left/right/up_ch` 这三路没有对应端口 |
| 重发搬入配对的搬出由谁给 | TS LLD：trigger 带 `r2t_trigger_dataout_tid`，“当前Trigger对应另一半DataOut任务的TID”，由 Router 给出 | TS MAS 寄存器表：搬入那一项的 `TASK_P2P_REISSUE_TID`，软件配 | 按 MAS：Router 不送这一项，Broadcast 重发的搬入也用 `TASK_P2P_REISSUE_TID` 找配对的搬出 |
| 自启动什么时候建表 | TS MAS 功能清单 13：“TS复位后可以直接自启动16个stream_table表项” | TS LLD 的 Stream Map 3.6.2.4：“配置提交只锁存Task0模板，不立即建槽。首笔被DataIn实际接受的Router trigger产生特殊启动事件”，一次建满 N 个 | 按 MAS：写完配置就建 |
| 自启动表项退休走不走 Router | TS MAS 功能清单 2：释放“后通知上游core 的TS credit ++”，不分模式 | TS LLD：SELF_START 模式“不等待Task Done的用户退休ACK，也不等待Router retire ready”，同一个 SID 重装 Task 0 后排到队尾 | 按 MAS：照常向 Router 发退休请求，收下后在队尾补一项 |
| DTE 仲裁时同一个 stream 上谁先 | TS MAS 的 DTE_Arb 一节：“两者属于同一Stream时优先选择Generated任务” | TS LLD 的 DTE-ARB-FUNC-002：“同SID冲突时由DataIn拥有该候选和完整Bundle” | 按 MAS |
| Bypass 那一路怎么和普通候选比 | TS MAS 性能特性 3：“方案2：全部按照最老用户原则仲裁” | TS LLD 的 DTE-ARB-FUNC-004：“normal结果与Special Bypass同时有效时按本地RR偏好选择” | Bypass 那一路不占 stream、没有年龄，模型按最老算 |
| 权重加载怎么派 DTE | TS LLD 的 TS-FUNC-001 与 DTE Arbiter：`weights_loading` 由 0 置 1 时向 DTE 发一笔只有 PC 的搬入请求，身份字段全 0，优先级最高 | 第 10 章“装 weights 的包照常通知 TS，落点取自包头”一条：每个装 weights 的包都 trigger TS，按 `DATAIN_TASK_PC` 派 DTE，loader 数够了中断 SCP | TS MAS 只写“硬件执行搬运weights操作”。模型按后者 |
| VU 的 event 送不送 TS | VU MAS F50：“可向 TS 发 Event 同步信号”（`vu_event`） | TS MAS 与 TS LLD 的 `vu2ts_done_ch` 都只有 uid、tid、sid、valid，没有 event | 模型把 `event` 留在 VU 的完成口上，TS 收下不处理 |
| 《TS_通信机制》的配置示例 | 《TS_通信机制》5.3 节（TS MAS 的 Programming Sequence 引它作任务链配置示例）：`self_start`、`B_reissue`、`P2P_reissue`、`task_reduce_iss`、`exe_mask`、`dsa_en` / `exe_dest` 各一列 | TS MAS 寄存器表：这几列都已不在，换成 `TASK_TYPE`、`TASK_P2P_REISSUE_TID` 与全局 `SELF_START` | 按 MAS。《软件栈》照录的四张表仍是原文的旧位域 |
| TS 找后继的算法 | TS MAS 的 Task Ctrl 一节：`SKIP_MASK = ~END_MASK & ((DATA_IN_MASK & done_bitmap) \| (REISSUE_MASK & ~stream.reissue))`，“End Task 即使已经提前完成也不能被跳过” | TS LLD 的 Task Generator：`search_mask = after_current_mask & through_end_mask & ~completion_bitmap`，“若Future End已经提前完成，其完成位会移除End”；MAS 寄存器表已没有重发标志位，`REISSUE_MASK` 派生不出来 | 按 LLD：跳过的项在 trigger 那一刻就并进完成位图，找后继只看一张位图 |
| DTE 仲裁里重发任务是不是最高 | TS MAS 的 DTE_Arb 一节：“Reissue任务优先级最高，并从head_ptr开始选择最老的Reissue” | 同一份 MAS 的性能特性 3：固定优先级的方案 1 划掉，“方案2：全部按照最老用户原则仲裁” | 按性能特性的方案 2 |
| 用户什么时候退休 | TS MAS 的 Stream_table 一节：“dsa/rv core 返回的任务完成是task_chain中标志为end的task”；Credit Monitor 一节：head 那一项“valid=1、end=1且task_fsm=TASK_FINISH” | TS LLD 的 Stream Map：“through_end_mask非零且覆盖的任务全部完成”，“不是仅End位完成即可” | 按 LLD：第 0 项到 End 的完成位全部置起才退休 |
| 逐级 reduce 的完成要不要等 DTE 那一半 | TS MAS 的 Task_done 一节：“Router Done可以被保持，但必须等匹配的DTE ACK被消费后才提交任务完成” | TS LLD 的 Task Done：“根据Router返回的UID匹配有效Stream，取出该槽位的current_tid，生成Reduce完成事件”，本地 DTE ACK 不参与 | 按 LLD：只认 Router 的 Reduce Done |

## 原始文档里标注为空缺或待定的内容

| 模块 | 缺什么 |
| - | - |
| Bach_core_MAS_TOP | 整篇标 `pending`。Glossary 全空；DSA / Cmem / Matrix mem / Share mem 的异常类型全空；“如何上报”未写；地址空间分配只有白板引用没有正文；“软硬件编程契约”章节大量只有标题（Kernel 三段式代码拆分原理、统一偏移地址编程规范、MOE 场景的 Boot / Launch / 动态描述、动态专家调度、Gdb、jtag） |
| Router MAS | Programming Model 起（Register Map、Interrupts、ISA、Programming Sequence、Locking、Performance、Power、Area）**全部是 eFUSE 模板残留**，与 Bach 无关。Signal List 也是 eFUSE 的 AHB 信号。“业务 Credit 的路由”正文只引用了另一篇未拉取的文档《Harvest下业务级Credit的路由机制》 |
| TS MAS | 标 `编写ing`。Signal List 表格大量字段空白（宽度、方向）；`EXCEPT_CHECK` 一节的 Interface 与 Timing 是 eFUSE 模板残留，`IO_REG` 一节只剩标题；MISC / Application scenarios / Interrupt Handling Sequence / Programming Sequence 的步骤与 Performance / Power / Area 全部是模板 |
| RV Core MAS | Signal List 空；各子模块的 Interface (LLD) / Timing Diagram 基本空；CSR 具体实现哪些“待讨论”；B core 软件流程标题重复且内容为空；Performance / Power / Area 只有标题 |
| Cmem MAS | Function Description 整章只有标题（regfile / agu / acu / ldq / stq / Matrix exe，且这些标题明显是从 MU 文档复制的，与 Cmem 无关）；Performance Targets 表格空；Memory List 是 eFUSE 模板残留 |
| Mmem MAS | Function Description 同样只有标题 |
| MU MAS | 大量章节是注释形式的写作提纲（以 `> //注释：` 开头）；寄存器描述全部指向内嵌 sheet，本地拉取的 markdown 里只有 `<sheet>` 标签没有内容 |
| ISA 描述表 | `Vector` 和 `DTE` 两个 sheet **完全为空**；`Matrix` sheet 里是通用模板样例（ADD/SUB/LW/SW），不是真实的 MU 自定义指令 |
| 寄存器描述表 | “地址总概”只有分类没有地址范围；`TS` sheet 为空；`Router` sheet 只有三个条目名称（驻留专家掩码、Message id 记录、路由方向表），无地址无位域 |
| Shared DM | 整篇只有一个标题，无任何内容 |

## 引用了但本地没有的文档

正文里 `<cite>` 引用、但不在已拉取范围内的：

* 《Rmem HAS 及 core 通信机制》（DATA_NOC HAS 指向它说明 Rmem 的内部架构：容量、算力、操作类型）
* 《DTE DSA》正本（只有《DTE DSA 副本》在库里）
* 各文档内嵌的 `<readonly-block type="diagram">` 绘图与 `type="isv"` 集成块，飞书没有开放读取接口。画板与内嵌表格已按 token 拉到 `perfechpitch/` 对应目录，索引见该库的 `FIGURES.md`

以下几篇原先缺，现已拉进 `perfechpitch/_refs/`：《Harvest 下业务级 Credit 的路由机制》《TS_通信机制》《MU / DTE 寄存器配置参数》《MU / DTE 需求整理和遗留问题分析》《DTE DSA 副本》《Mmem(Matrix Mem) 详细设计文档模板》《BachCore 输出并行数据流需求分析》《多个上游竞争一个下游资源的需求分析》《不同业务场景下的拆包方式》《RV core 指令集与自定义指令讨论》《MU 评估》《Bach Pysim 技术文档 v0.5.0》《仿真评估工作》。

## 设计上仍未确定的问题

**Router**

* 整包传输方案下“长包阻塞可能有死锁场景，需要在架构层考虑不会出现死锁”，死锁避免的具体论证未写
* Router 表项示例里一处原文未定：C0 在 Path2 上要不要查输出端的 stream credit table。原文另一处“C8 在 Path1 上进 CoreMem 重发时 Core 位是否也要置位”已不成立：出方向掩码里没有 Core 位，进不进本 core 由 `path_core_bypass` 单独判定
* 一笔 Reduce 任务含几个 Packet、任务边界靠什么标出来，Router MAS 没写。MAS 只说“同一任务在每个目标方向只取得一次用户级准入，后续 Packet 和 flit 复用该准入”；《TS_通信机制》把一笔 32 KB 的 reduce 拆成 4 笔 8 KB 的 reduce task。模型按一个 `reduce_seq` 的包算一笔任务
* 本级 ReduceMemory 做完一笔任务后“向相关上游发送一次”的 release 从哪一组进静态路由，Router MAS 没写：`RTR_RELEASE_ROUTE` 只按四个输入方向分组，这一笔不从任何输入方向进来。模型把它直接写到这笔任务每一路上游来源所在的方向，经过纯透传的 core 时才按静态路由转。哪一路是本 core 那一份按表项 `flow_dir` 的 reduce1、reduce2 两位定，默认本 core 出了自己那一份
* `RTR_RELEASE_ROUTE` 的复位值，以及没配路由的口收到 release 怎么处理，Router MAS 没写。模型里这种口收到 Reduce release 就报错
* Rmem 16 个分区都占着时新用户的第一笔怎么办，Router MAS 没写：反压那一段只列了“Rmem Bank、读改写通道、输入缓冲或输出空间暂时不可用”。模型对这一路输入反压，等有用户 Retire 放出一个分区。分区用时才分配之后，它与 16 项 stream 表不再一一对应；分区被上游先到的用户占满、本 core 的用户推进不下去时会不会卡死，原文没有论证
* 只合并上游分量、本 core 不出分量的那一级，本 core 的 TS 里可能没有这个用户，谁对它发 User Retire 放 Rmem 分区，Router MAS 没写：原文只说“TS 发送 User Retire 触发Rmem释放”。模型里分区只等本 core 的 TS 对这个用户发 Retire
* 结果流里后面的结果还没算好时，同一出口别的 VC 能不能先走，Router MAS 没写：F-012 只说“只有当前 Packet 因下游条件不满足而阻塞时，才允许在合法的 flit 边界切换到其他已就绪的 VC”，结果没算好不是下游条件。模型只锁同一出口同一 VC，别的 VC 照走
* **VC 机制到底实不实现**。原文的原话是“实现 VC 机制需要很大的额外面积、设计复杂度和验证空间，成本极高。具体是否实现需要模拟器介入，综合判断开发复杂度和效果收益”。这是本次建模要回答的问题之一，不是文档缺口
* “Broadcast 过快引起空泡”这一档的定量结论，原文明确写了“需要模拟器介入协助确认”。前提是同一个用户在 core0 与 core2 上的处理速度不同，而计算量分布均匀时差距主要来自逐级 Reduce
* P2P 流量控制的“流量控制使能”配在哪一张表，原文只写“在一个 Core 配置了流量控制使能”，没有指明是 TS 的 CFG_REG 还是 RouterTable。本套文档按配在 TS 建模
* **走发送就绪门控的那些报文靠什么发现下游收不下**。同一个 user 在某方向已持有资源后，后续报文不再查 stream credit，最终用哪个硬件信号表达“下游收不下”原文没有定，把它列为需要硬件与产品拍板的开放问题。Top 模拟器做成可切换的桩：`SEND_READY_MODE` 默认 `backpressure`（链路与 Core Mem 的反压），另有 `per_user_ready` 与 `ack` 两档

**TS**

* concat 的 task 软件配 1 个还是多个（若配 1 个，需在任务链中指明 `exe_num`）
* ~~DTE task 的三种优先级用固定优先级，还是“reissue 最高 + 其余按最老用户”~~ 已定：TS MAS 现版划掉方案 1，全部按最老用户仲裁
* 哪些 task 该硬化进 TS，界线尚未定下
  * 原文的设想：“所有与用户和 core mem 分配无关的 task，都可以采用硬化 task 在 TS 的方式（包括 broadcast 重发），只有与用户强相关的任务链才会在 stream 表里创建和工作”
* ~~reduce 任务（32 KB）拆成多笔 8 KB 由 TS 并行发射，方案可能改到 DTE 内做多笔，届时 TS 不再需要 `TASK_REDUCE_ISS`~~ 已定：TS MAS 现版划掉 `reduce_num`，一笔 reduce 在链上拆成几项逐级 reduce 任务
* ~~dataout 任务后续可能由 DTE 直接与 Router 交互检查 credit，不经 TS~~ 已定反向：TS 查 RouterTable 与 stream 资源、有资源才下发，DTE 只查 VC 通路上的 flit credit
* TS 直接配置启动 DTE DSA 的方案待定
* ~~`TASK_DSA_EN` 位域在 MAS 里已划删除线，取消之后 TS 靠什么区分「只调 RV core」与「调 DSA」的 task，MAS 没写~~ 已定：由 `TASK_RECV_UNIT` 区分
* Core Mem 里给 P2P 阻塞缓冲留多大、开哪几个方向（最多 3 个），与给 broadcast 留的空间怎么分
* 逐级 reduce 任务的 `TASK_CREDIT_EN` 查的是什么，原文没写清：TS MAS 的配置检查要求 `TASK_TYPE = 4` 必须标它，而它的描述是“用于做credit问询……只有dataout任务需要”，reduce 那一项的下游却是本级 Rmem。模型把它读成要本级 Rmem 的 credit，TS 不为这一项向 Router 发问询
* ~~TS 逐笔下发 reduce 的 N 笔时，kernel 怎么知道这一次是第几笔~~ 已不成立：每一包是链上单独一项，各有各的 PC
* 运行时 PID 更新时 RV core 怎样把新 PID 带回，TS MAS 与《寄存器描述》RV Core 页都没写，TS LLD 只说 Core ACK 携带新 PID。模型给 RV core 加一个可读写的自定义 CSR `0x18`（当前任务的 PID），kernel 写它，`task_done` 时随完成带回
* Task Generator 的并行度：TS LLD 是 16 个槽位各一份组合逻辑，同一拍各装各的；模型每拍按年龄只装一个
* 两路完成配对的记录项数：TS LLD 每个执行单元一项，由系统保证同组不交叠；模型按 `{执行单元, stream_id, task_id}` 记，不限项数
* 有 valid 项却没有 End 的链算不算配置错误，TS MAS 的检查清单没列（只列了多个 End 与 End 后面还有 valid）。模型算错，因为 `THROUGH_END_MASK` 要以 End 为界
* `TASK_VCID` 怎么填，TS MAS 只说是 VC 通道编号：编译侧按 PID 对 VC 数取模填，与 DTE 原先按 `path_id` 挑 VC 的取法一致
* Reduce Done 带回的 `reduce_seq`：模型里 DTE 把发这一包的 `task_id` 打进包头的这一位，Router 原样带回；TS 按 `user_id` 找 stream，不看它
* DP + P2P 重发的 core 上，只做透传的用户怎样越过本 core 的搬入与计算任务：TS MAS 功能清单写着“P2P/计算task 任务按照用户可以直接越过”，但按用户分组（`exe_mask` 与 trigger 带的计算标志）已整条划掉，寄存器表与 TS LLD 都没有替代的机制。模型没有这种 core

**RV Core**

* loop 分支未到最大次数就退出时，loop_bp 表项无法释放怎么解决
* 普通条件分支是否需要 gshare 类预测器
* `boot_pc` 与异常入口是否为软件可配的 I/O 寄存器
* lsq 是否需要记录 store 类指令
* DTCM 的 2-bit ECC 能否按异步非精确处理

**MU**

* 软件希望访存地址 byte 对齐，但 256B 数据块宽度的地址偏移会增加 buffer 设计复杂度（输入输出要移位拼接）
* FC1 与 FC3 融合时在 N 方向拼接，输出为 `ep_num×(64+64)`、`ep_num×(128+128)`、`ep_num×(256+256)` 这类布局，对 VU 的 silu 与 dot 不友好，待讨论

**VU**

* Vector 数据广播场景是否值得支持乱序调度 TBD
* RS 随机舍入是否实现待确认

**DTE**

* CM → MM 方向后续是否要支持仍是遗留问题，当前倾向不支持
* Fast LUT 的字段展开与转换等软件仿真结果出来后再固化
* 目的地址的生成是否全部交给 DTE core（B core 按指针循环累加，R core 的指针方案未定）
* reissue 任务目前硬件只按 `path_id` 判断，是否合适
* scale 与 data 在 Core Mem 里的存储形式
* Matrix Mem → Core Mem 搬运的源与目的是否用同一个 `stream_id`
* Fast LUT 表项里的 `length` 与「`task_len` 由 RV core 配寄存器」这两条出自不同时间的源文档
  * 《DTE DSA》给的 Fast LUT 表项是 `{valid, length, ctrl_flags}`
  * 《MU / DTE 需求整理和遗留问题分析》20260825 的结论是去掉 `task_len_table`、`task_len` 改由 RV core 配寄存器
  * 本文按「命中 Fast LUT 的常规任务用表里的 `length`，未命中才走 RV core 配寄存器」理解，未经源文档确认
* R core 的 shareMem 怎么索引，原文留了问句未答
* DTE 的 `path_task_map` 按 `path_id` 只记一个 `task_id`：TS 允许链上几项搬入任务配同一个 PID，每次找还没做完的最低一项（ts.md F11），DTE 给进核那一笔的 DSA 完成填的却固定是其中一项，与 RV core 那一路带回的 `task_id` 对不上。模型的三类 core 每个 PID 只对应一项搬入，没有碰到

**Core Mem**

* DTCM 是否要做，取决于 RV core 访问 SM 的延迟是否满足要求
* ITCM 溢出处理，当前结论是“不会有溢出场景”

**Data_NOC（DATA_NOC HAS 的 Open Issues）**

* OPEN-04 Routing Table 在线更新机制：是否需要额外的数据通路模式支持运行时在线更新而不中断数据流
* OPEN-05 拓扑可扩展性：当前的两行 Mesh 需要 mid 接口，若后续采用单行结构 mid 将删除，Router 是否预留 mid 接口的可配置删除能力
* OPEN-06 Chip to chip 是否支持 VC：支持则两侧要单独例化 SRAM 吸收 600 ns 往返；不支持则要靠软件在业务上单独实现跨 chip 的进 core 流控
* hflit 与 pflit 并行传输的协议多约 10% 信号线，是否改成统一协议
* Router 与 core 之间是否从 AXI-Stream 改成同一套 hflit / pflit 协议
* 会议记录《通信机制》2026.7.16 第 10 问“EP 多播 path_core_mask 的含义是什么”当场没有答复，答案在《通信机制（分析过程）》的 Router Table 一节

**系统软件与业务流控**

* Board CPU 是否存在
* Bach core 的任务完成信息返回上层走 PCIe 直接传输，还是走内部数据通路反向传输
* 完成信息由 Router 硬件解析包头发起，还是由软件调用 RV core 发起；软件需要配置哪个 Bach core 返回完成信息
* Node 任务准入依据的 buffer 状态、SmartNIC 队列状态、板间网络拥塞状态缓存在哪里
* GLM5 结果往 GPU 发送：R core 的 Matrix Mem 能否被读、是否先到 DDR（DDR 带宽需网卡的 2～3 倍）、是否组 batch
* GLM5 的 GPU 侧：R core 的 credit 退休信号由谁返回、跨 tray 是否需要 NTB、GPU 能否发到第一列全部 B core 以扩大 Matrix Mem 缓存

**建模所需而设计未给值的参数**

下面这些设计文档没给值，《latch 建模计划》的“边界与风险”一节给了默认值：

* **Router**
  * VC Buffer 深度、VC credit 初值
  * Stream Resource Table 项数
  * RouterTable 表项数与副本数
  * Xbar 与 ReduceModule 的仲裁算法
  * ReduceModule 的 bank 数
  * CoreStation 的 HeaderFIFO 深度与进 core 的包长上限（OutputBuffer 已取 60 flit，这两个值按它匹配）
* **DTE**：Buffer、Completion RS、Done Pending 三处深度（TaskQueue 已定「每通道每侧不少于 16」，具体值仍待评估）
* **VU**：ISQ 深度
* **RV core**：task_queue 深度、dsa_iss 收请求的队列深度
* **寄存器地址映射**：MU 与 DTE 两处
* **超前发送窗口 N**：软件按 path 配的值，编译期确定；使能位与 N 存在哪一张表未定

**专用 core（B core / R core）**

* R core 的 Matrix Mem 要不要额外的 credit 机制
  * 原文的疑问：“Reduction Core 的 Matrix Mem 与 Broadcast Core 的 Matrix Mem 对应的话，不需要额外 Credit 机制保证？”
  * 附带的追问：总槽位数是否要按输入 / 输出能支持的**较小**那个来算，否则进得多、出得少仍会缺 credit

***

本套文档的两处来源与原始文档的 wiki 链接见 [README](README.md)。本章内容生成于 2026-08-20，2026-08-25、2026-08-28 与 2026-09-10 三次按原始文档更新。
