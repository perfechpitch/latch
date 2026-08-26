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
| VU 能否直接读 Matrix Mem | 软件流程梳理：R core 链二由 VU DSA 直接从 Matrix Mem 读两路数据做 reduction（[`02_软件流程梳理/d13.png`](<../../../../perfechpitch/Bach软件文档库/01_Bach软件文档库/04_总体设计/02_软件流程梳理/d13.png>) 画的就是 matrix mem → VU dsa → core mem） | MAS_TOP：“VU 不能直接读 Matrix mem”；EP 组间 Reduction 讨论与 core 内调度机制：DTE 先把两笔从 MM 搬到 CM，VU 再求和（[`04_core内调度机制/d40.png`](<../../../../perfechpitch/Bach/02_二、需求分析/06_第四阶段需求分析（Core Level需求分析）/04_core内调度机制/d40.png>)） | 按 MAS_TOP（多一步 DTE 搬运），R core 链二因此是 4 个 task 而不是 3 个。这一步直接改变 R core 的 CM 容量与带宽需求 |
| PPTP 下 silu·dot·量化 落在哪一段 chip | 软件流程梳理伪代码：FC1/FC3 的 reduce 结果都落到 **FC2 段** chip 的 core 0，dot 在那里做 | 本文档 `pptp_nk` 角色表：dot 在 **FC3 chip** 的 `pptp_fc3_nk_dot_core`；板卡结构和模型映射：Silu 在 **FC1 chip**、dot 在 **FC3 chip** | 三处把 VU 的活摆在不同 chip 上，直接改变每段 chip 的 VU 占用与跨段传输量。**未解**，建模前必须定死一种 |
| B / R core 的 datain 侧是几个 task | 软件流程梳理：两个 task，DTE 搬完再由另一个 task 置 flag / 更新 `arrive_num` | core 内调度机制与软件计算流程详细评估（GLM5 章 B core 伪代码）：一个 `DATAIN_TASK`，DTE DSA 搬完时顺带置标志并推进 head 指针 | 按一个 datain_task（两处较新的文档一致） |
| Core Mem 容量 | MAS_TOP 内存结构表：512KB / 1MB | Cmem MAS：1MB + 32KB（8 bank） | 按 Cmem MAS |
| DTE ↔ Cmem 接口宽度 | DTE MAS：256B + 8B（Data + scale） | Cmem MAS：256B/T | 按 Cmem MAS |
| DSA 配置指令一次写几个寄存器 | ISA 描述表：`dsaw.d` / `dsawi.d` 写 2 个；软件计算流程详细评估的伪代码大量使用 `dsawi.d` | RV Core MAS：每条最多配置 1 个 DSA 寄存器 | 按 RV Core MAS，`dsawi.d` 先当两条 `dsawi.s` 建 |
| `task_done` 的 FC 标志 | ISA 描述表与软件计算流程详细评估：`task_done ts, fc`，fc 带 fence 语义且需 ts 有效 | RV Core MAS：已删除 FC 标志 | 按 RV Core MAS，不建 FC |
| `flag_check` 指令 | ISA 描述表有编码；RV Core MAS 的 `user_id` CSR 描述仍提到“R core 执行 flag_check 对应的 task” | RV Core MAS 的 Features 已删除“内存 flag 查询指令” | 保留建模（B core / R core 轮询映射表靠它），待设计者确认 |
| 取指 / 访存不对齐异常的归属 | RV Core MAS decode 优先级表：addr misalign 单列一类 | 同文档 ITCM 与 Exception 两节：不对齐归入 access fault | 按 decode 优先级表 |
| RV core 的 C / A / F / D 扩展 | RV Core MAS Features：C 支持、A 考虑支持、F 与 D 不支持；MAS_TOP：RV32IMC | 同文档“标准指令集”小节仍以“是否支持 C？是否支持 AFD？”的问句形式 | 按 Features |
| DTE 的软件接口 | DTE MAS：Task Descriptor（`TASK_CFG_ADDR` / `TD` / `PACK` / `TRG`）+ Doorbell 序列 | 软件计算流程详细评估：`transfer_mode` / 基地址与 `stream_stride` / `data_len` / 包头与 scale 地址 / `sharemem_*` 一套按字段命名的寄存器，与 Task Descriptor 字段无对应关系 | 软件接口按软件计算流程详细评估建，Lane 与完成机制按 DTE MAS |
| 入站包头的字段清单 | DTE MAS Header Logical Fields：`version` / `header_len`、`packet_type` / `route`、`dst_addr`、`byte_count`、`task_id` / `stream_id`、`attributes` / `reserved` | 软件计算流程详细评估的 MSG 包结构：包头标记 2 B、Router 信息 4 B（`path_id` + `path_core_mask` + rsv）、包长度 2 B | 两份给的是同一个 Header 的两种写法，字段对不上。DTE MAS 自己声明“具体 Header 位域仍以 Router 接口规范为准”，等那一份 |
| DTE 的启动方式 | DTE MAS：有“TS 快速启动流程”（TS 绕过 RV core 直接启动 DTE） | 软件计算流程详细评估：只保留 DTE core 配置任务给 DSA 这一种，TS 直接启动的方案待定 | 按软件计算流程详细评估 |
| 包头与 topK 的存放位置 | DTE MAS：包头可写 DTE 内 Header mem 或 Cmem 独立空间；topK 可写 MU 的 `topK_ep_table` 或 Cmem 独立空间 | 软件计算流程详细评估：计算 core 的包头存 DTE 内独立 mem，B core / R core 存 Core Mem；topK 由 DTE 复制到单独的 mem 供 MU 读 | 按软件计算流程详细评估 |
| B core 查询 ready 的 task 由谁执行 | 软件计算流程详细评估 GLM5 章：VU 做 check flag | 同文档 TS 章的 B core 示例：task0 为 MU | 按 GLM5 章（VU），待确认 |
| `SELF_START` 的适用范围 | TS MAS：仅 B core 才会有 | 软件计算流程详细评估 R core 示例：task0 `self_start=1` | 按软件计算流程详细评估，B core 与 R core 都有 |
| Router 的 R2R 方向端口 | Router MAS：五方向互连，上、下、左、右及 Core | DATA_NOC HAS：left / right / mid 三个方向端口，加 local 与 reduce_0/1/2，5 入 7 出 Crossbar | 按 HAS 的三方向。2×5 拓扑里邻居就是同行左右加另一行对称位，且与第 3 章顶层节的 data_L / data_UD / data_R 三通道一致 |
| Reduce 的累加做在哪 | Router MAS：ReduceModule 在 Router 内，16 用户 × 16 KiB 上下文，RMW 原位累加，自己维护下游 Reduce Credit | DATA_NOC HAS：Router 内不设 Reduce Buffer，累加由独立的 Rmem 子系统完成，reduce credit 是单独的流控网络 | 按 MAS：ReduceModule 在 Router 内 |
| Stream 资源表谁是唯一有效状态 | Router MAS：Router 维护的 User Resource Allocation Table 是唯一有效状态，DTE 持 cache | DATA_NOC HAS：Router 输出单元与 core 内各持一份 credit table，靠 credit release 接口同步 | 按 MAS：Router 唯一有效 |
| 出核前查资源的监听队列在谁那里 | Router MAS 一处：功能已转移到 DTE 中 | 同一份 MAS 另一处：详写 Router 上 16 项全相连监听事件队列与完整申请流程 | 按后者：在 Router |
| Router 的仲裁粒度 | Router MAS 有一节“Interleave 和整包的对比”，只列两案优劣、未给结论 | MAS 正文与 DATA_NOC HAS 都是 flit 级（“Packet 在 VC 间按照 Flit 的粒度传输”“矩阵仲裁以 flit 为基本节拍”） | flit 级，整包只作为贪婪仲裁的优先级偏好。原第 3 章写成“整包粒度（选定）”是误读，已更正 |
| R2R 带宽 | Router MAS：相邻 Router 双向各 256 GB/s @1GHz | DATA_NOC HAS：R2R 210 GB/s、C2C 90 GB/s | 两个都记：256 GB/s 是 256 B/T @1GHz 的接口理论值，210 GB/s 是 HAS 记的有效带宽。是否同一口径待确认 |
| 重发（reissue）任务在不在主任务链上 | TS MAS 与 core 内调度机制：重发 task 不在主线任务链上，可并行执行、优先级最高（TS MAS 里“重发的 task 任务优先级最高”这句自己划了删除线） | Top 模拟器详设的三条【讨论结果】：插在任务链中实现，同一 stream 下即使无依赖也按任务顺序执行，需等它完成才能执行后面的任务；软件计算流程详细评估的 normal core 示例把 broadcast reissue 排成 task 1' | 按 TS MAS 与 core 内调度机制：不在链上、并行、优先级最高 |
| TS 的时延数字 | TS MAS：task 唤醒延迟 2～3 cycle | Top 模拟器详设：调度间隔 16 T、需与 Router 通信 21 T、retire 5 T；TS LLD 时序图：CREATE 3 / WAKE 2 / DONE 3 / INSTALL 4 / RETIRE 6 | 三者口径不同不是矛盾：MAS 是硬件目标，模拟器是含 RV core 往返的端到端值，LLD 是逐级拍数。《TS 任务调度器》按这三层分别记 |
| Router 的 Credit Bypass Route | Router MAS：软件通过 CSR 为每个业务 Credit 输入端口配置静态输出方向 Mask | 软件计算流程详细评估 Router 章：删除了该配置小节，软件只配 RouterTable；但同章检查清单与释放表仍引用 Credit Bypass Route | 按 Router MAS |
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

* 《Harvest 下业务级 Credit 的路由机制》（Router“业务 Credit 的路由”一节的全部内容）
* 《TS_通信机制》（TS 的 Programming Sequence 指向它的 5.3 节场景映射）
* 《Rmem HAS 及 core 通信机制》（DATA_NOC HAS 指向它说明 Rmem 的内部架构：容量、算力、操作类型）
* 《MU/DTE 寄存器配置参数》（DTE Programming Model、软件计算流程详细评估 Matrix 章）
* 《MU/DTE 需求整理和遗留问题分析》（软件计算流程详细评估 DTE 章与 Matrix 章）
* 《DTE DSA 副本》《DTE DSA》（3 Lane 评估的对比材料）
* 《Mmem(Matrix Mem) 详细设计文档模板》
* 各文档内嵌的 `<readonly-block type="diagram">` 绘图与 `type="isv"` 集成块，飞书没有开放读取接口。画板与内嵌表格已按 token 拉到 `perfechpitch/` 对应目录，索引见该库的 `FIGURES.md`

## 设计上仍未确定的问题

**Router**

* 整包传输方案下“长包阻塞可能有死锁场景，需要在架构层考虑不会出现死锁”，死锁避免的具体论证未写
* 双坏核示例里两处表项原文未定：C0 在 Path2 上的 `streamNeedMask` 是不置位还是右向置位；C8 在 Path1 上进 CoreMem 重发时 Core 位是否也要置位。`operation` 列的 Reduce0 / Reduce1 / Reduce2 含义原文未定义

**TS**

* concat 的 task 软件配 1 个还是多个（若配 1 个，需在任务链中指明 `exe_num`）
* DTE task 的三种优先级用固定优先级，还是“reissue 最高 + 其余按最老用户”
* 哪些 task 该硬化进 TS，界线尚未定下
  * 原文的设想：“所有与用户和 core mem 分配无关的 task，都可以采用硬化 task 在 TS 的方式（包括 broadcast 重发），只有与用户强相关的任务链才会在 stream 表里创建和工作”
* reduce 任务（32 KB）拆成多笔 8 KB 由 TS 并行发射，方案可能改到 DTE 内做多笔，届时 TS 不再需要 `TASK_REDUCE_ISS`
* dataout 任务后续可能由 DTE 直接与 Router 交互检查 credit，不经 TS
* TS 直接配置启动 DTE DSA 的方案待定
* Core Mem 里给 P2P 阻塞缓冲留多大、开哪几个方向（最多 3 个），与给 broadcast 留的空间怎么分

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
* LUT 的字段展开与转换等软件仿真结果出来后再固化
* 目的地址的生成是否全部交给 DTE core（B core 按指针循环累加，R core 的指针方案未定）
* `path_core_mask` 由谁生成（GPU、slave CPU 还是 SmartNIC）
* reissue 任务目前硬件只按 `path_id` 判断，是否合适
* scale 与 data 在 Core Mem 里的存储形式
* Matrix Mem → Core Mem 搬运的源与目的是否用同一个 `stream_id`
* 软件包头在 Hmem 里的 task 级偏移
  * 寄存器序列只给到 `header_base_addr + stream_id × 包头长度` 这一级
  * `+ task_id × 16 B` 这一层是按 Hmem 的 16 KB 容量与包头存储图（16 stream × 64 task × 16 B）推出来的
* R core 的 shareMem 怎么索引，原文留了问句未答

**Core Mem**

* DTCM 是否要做，取决于 RV core 访问 SM 的延迟是否满足要求
* ITCM 溢出处理，当前结论是“不会有溢出场景”

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
  * ReduceModule 的 Entry credit 与 bank 数
  * CoreStation 的 HeaderFIFO 与 OutputBuffer 深度
  * `operation` 的 Reduce0 / Reduce1 / Reduce2 含义
* **DTE**：TaskQueue、Buffer、Completion RS、Done Pending 四处深度
* **VU**：ISQ 深度
* **RV core**：task_queue 深度
* **寄存器地址映射**：MU 与 DTE 两处

**专用 core（B core / R core）**

* R core 的 Matrix Mem 要不要额外的 credit 机制
  * 原文的疑问：“Reduction Core 的 Matrix Mem 与 Broadcast Core 的 Matrix Mem 对应的话，不需要额外 Credit 机制保证？”
  * 附带的追问：总槽位数是否要按输入 / 输出能支持的**较小**那个来算，否则进得多、出得少仍会缺 credit

***

本套文档的两处来源与原始文档的 wiki 链接见 [README](README.md)。本章内容生成于 2026-08-20，2026-08-25 按原始文档更新。
