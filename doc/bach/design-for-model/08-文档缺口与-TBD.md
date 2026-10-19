# 第 8 章　文档缺口与 TBD

**模式**：design（陈述当前设计，动机与取舍收在各节的“取舍”段落里）

登记四类内容：

* 原始设计文档之间的口径冲突
* 原文档标注为空缺的内容
* 引用了但本地没有的文档
* 设计上仍未确定的问题

动手建模前先扫一遍，本章区分已采用的目标基线、原文待统一项和当前模型假设；待统一项不得编码为已冻结硬件行为。

《Bach 硬件设计建模参考》第 8 章，全套目录见 [README](README.md)。

***

## 已确定的跨模块设计合同

本项目按以下约定统一各模块。原始资料中的不同说法保留在本章；硬件物理位域、时钟边界及异常恢复中尚未定义的部分继续单独登记。

| 设计合同 | 项目采用的规定 | 原始资料的分歧 |
| - | - | - |
| 下游Reduce准入 | TS管理本级RMEM许可；网络输出逐flit查VC credit，接收端按用户准入、最早未完成Reduce、任务号、PID及物理上下文放行。RMEM输出完成按UID归还本级许可；业务Stream release按用户退休归还 | Router MAS的Downstream Reduce Credit Map、reduceNeedMask及逐任务Reduce release是另一方案，本项目不采用 |
| TS／Router许可所有权 | TS保存三方向计数、Pending和已用方向，原子授予新增方向；Router保存实体用户表，真实Stream release仅归还TS已持有的方向。重发完成另行通知 | 旧资料的TS注册请求、Router监听并返回SID/TID Grant不接入本项目的业务许可通路 |
| TS／DTE任务边界 | 一个TS任务可提交多条DTE搬运，每条独立占全局Entry并有序退休；仅最后条目使能ack_ts_en。R Core两Packet属于同一个Reduce任务，Core/DSA双ACK完成任务，RMEM返回独立还空间 | 原文一Task一Packet限制不用于本项目；不能把两个操作数直接拆成相互等待的Reduce任务 |
| ERROR后的推进 | 停止新的Channel下发及正常SM／Ack推进；保留异常队头、描述符和在途上下文。已下发任务保留原请求／响应归属，已Fire事务继续收集响应；ERROR接收并丢弃新Trigger、清dirty，普通寄存器访问继续 | 原文“已下发任务完成”在本项目中不表示继续正常SM副作用或TS完成。残包终止、恢复及完整Flush协议仍需补齐 |
| 权重加载启动 | loading置位只切换模式，每加载阶段由一个已接受Trigger发起PC-only请求，身份全零；业务ACK在加载期间忽略。切换业务前由加载控制方确认数据、写响应和DTE队列排空 | 原文将loading置位直接作为请求的说法不用于本项目；整权重分片的加载阶段划分和完成通知仍需定义 |

## 口径冲突

同一项在不同原始文档里给了不同的值，都需要找设计者确认。“建议”一栏是本文档作者的取法，不是设计结论。

| 项 | 说法 A | 说法 B | 建议 |
| - | - | - | - |
| RV core 特权态 | MAS_TOP：“仅支持 U 态（用户态）。没有 CSR？” | RV Core MAS：“支持 M 态，支持所有 M 态 CSR；不支持 S、U、H” | 按 RV Core MAS（M 态） |
| VU 访存带宽 | 性能需求规格：B_VU = 64 B/T | VU MAS：每周期 1 Load + 1 Store 各 128 B | 按 VU MAS |
| VU 能否直接读 Matrix Mem | 软件流程梳理：R core 链二由 VU DSA 直接从 Matrix Mem 读两路数据做 reduction（[`02_软件流程梳理/d13.png`](</home/colin/develop/perfechpitch/Bach软件文档库/01_Bach软件文档库/04_总体设计/02_软件流程梳理/d13.png>) 画的就是 matrix mem → VU dsa → core mem） | MAS_TOP：“VU 不能直接读 Matrix mem”；EP 组间 Reduction 讨论与 core 内调度机制：DTE 先把两笔从 MM 搬到 CM，VU 再求和（[`04_core内调度机制/d40.png`](</home/colin/develop/perfechpitch/Bach/02_二、需求分析/06_第四阶段需求分析（Core Level需求分析）/04_core内调度机制/d40.png>)） | 按 MAS_TOP：VU 不直接读 Matrix Mem。R core 的两笔也不经 VU，由链二的 DTE 从 Matrix Mem 送进本级 Rmem 相加，见下一行。R core 每行一个：链一收本行结果与上一行 R core 送来的累加结果两半，每半一包 12288 B；链二从软件用户 FIFO 取出一个 ready 用户，把两半送进本级 Rmem 相加，结果直接发往下一行 R core |
| R core 两行间的求和做在哪 | 《软件计算流程详细评估》“各行间FC2 psum reduction”：“使用VU完成两行间redcution”；《core内调度机制》[`d40.png`](</home/colin/develop/perfechpitch/Bach/02_二、需求分析/06_第四阶段需求分析（Core Level需求分析）/04_core内调度机制/d40.png>) 与同文 R core 示例链是四步：MU 查、DTE 从 Matrix Mem 搬进 Core Mem、VU 求和、DTE 送出 | 《EP组间Reduction讨论（过程）》“Reduction计算通路”末尾：“可以选择不在VU做reduction计算，在Router进行计算，数据先来在Matrix Mem存储，用户的另一个累加数据到达后通知DTE将数据搬运到Router计算后直接向下游输出” | **已定（用户决定）：按说法 B**。缓冲仍在 Matrix Mem；链二的 DTE 把两笔作为本 core 的两个操作数送进本级 Rmem，相加的结果直接发往下一行。同文“复用TP Reduction通路做EP Reduction（X）”否掉的是不经 Matrix Mem、直接在 Router 逐跳累加的做法，与这一取法不冲突 |
| PPTP 下 silu·dot·量化 落在哪一段 chip | 软件流程梳理伪代码：FC1/FC3 的 reduce 结果都落到 **FC2 段** chip 的 core 0，dot 在那里做 | 需求分析的 `pptp_nk` 角色表：dot 在 **FC3 chip** 的 `pptp_fc3_nk_dot_core`；板卡结构和模型映射：Silu 在 **FC1 chip**、dot 在 **FC3 chip** | 三处把 VU 的活摆在不同 chip 上，直接改变每段 chip 的 VU 占用与跨段传输量。**已定**：按软件流程梳理那一档，三步统一落在 FC2 段 chip 的逻辑 core 0 |
| B / R core 的 datain 侧是几个 task | 软件流程梳理：两个 task，DTE 搬完再由另一个 task 置 flag / 更新 `arrive_num` | core 内调度机制与软件计算流程详细评估（GLM5 章 B core 伪代码）：一个 `DATAIN_TASK`，DTE DSA 搬完时顺带置标志并推进 head 指针 | 按一个 datain_task（两处较新的文档一致）。R core 这一个 datain task 配好进核搬运后按 `user_id` 更新软件映射表，不等数据落地，两笔都到了就把该用户写入软件用户 FIFO；链二弹出后再等标志置齐 |
| Core Mem 容量 | MAS_TOP 内存结构表：512KB / 1MB | Cmem MAS：1MB + 32KB（8 bank） | 按 Cmem MAS |
| DSA 配置指令一次写几个寄存器 | ISA 描述表：`dsaw.d` / `dsawi.d` 写 2 个；软件计算流程详细评估的伪代码仍在用 `dsawi.d` | RV Core MAS：每条最多配置 1 个 DSA 寄存器；软件计算流程详细评估的**指令表已改成只有 `dsaw` / `dsawi` 单寄存器写** | **已定：单寄存器写，`.d` 那一档不再进设计**。《RV Core自定义指令详细设计》定稿的 6 条里只有 `dsaw` / `dsawi`，`bit31` 改用来区分寄存器寻址与立即数寻址。ISA 描述表那一侧的旧编码与伪代码不再跟 |
| `task_done` 的 FC 标志 | ISA 描述表的编码里有 FC 位；RV Core MAS 的“task完成指令”一节仍写着 TS 与 FC 两个标记，FC 带 fence 语义 | 软件计算流程详细评估已改成 `task_done ts`，连同“后续 task 要读本 task 写进 shared_mem 的数据可用 fc 做 fence”那段用法一起删掉；RV Core MAS 的 Features 一节也只列 TS 标志 | 按软件侧与 Features，**不建 FC**。**冲突仍在**：MAS 正文那一节没跟着删，而 `fence` 指令本身实现为 nop，所以 MAS“内存一致性”一节里“用 fence + task 完成通知 TS 隔离两个 task 的数据相关”这句现在没有对应的硬件手段，跨 task 的隔离只剩任务链的全序 |
| `flag_check` 指令 | ISA 描述表有编码；RV Core MAS 的 `user_id` CSR 描述仍提到“R core 执行 flag_check 对应的 task” | RV Core MAS 的 Features 已删除“内存 flag 查询指令” | **已定：不建**。《RV Core自定义指令详细设计》定稿的 6 条里没有它，B core / R core 的轮询改用普通 Share Mem 读：B core 比较 `head` 与 `tail`，R core 比较软件用户 FIFO 的头尾。随之取消的还有“唯一一条多拍自定义指令”这一档。旧稿里见到 `flag_check` 的说法，按“软件用普通 Share Mem 读轮询”读 |
| 用户号是一个还是两个 | RV Core MAS 的“task 完成通道”一节把回给 TS 的那一项叫 `local_user_id`，“用户在这个 HBU 流控范围内的编号”，说它用于 R-core 用户映射表和 Matrix Mem 地址计算 | 同文档“自定义 CSR 寄存器”一节把同一个东西叫 `user_id`：“用户在 HBU 流控范围内的 local user_id，12 bit，可读写，传统业务流下由 TS 的 task 开始执行时从 task_queue 硬件写入 CSR，R-core 执行 flag_check 对应的 task 时由软件写入”；Task Scheduler MAS 的 stream_table 表项清单里只有一个 `user_id` | **已定：一个**。RV core 的自定义 CSR 只有 `stream_id`、`user_id`、`task_id` 三个，可读写的那个就是 `user_id`；stream_table 也只有一个 `user_id` 字段。TS 下发时硬件把表项的值写进 CSR，自启动的 core 上软件认出用户后写进同一个 CSR、随 `task_done` 回 TS 更新同一个字段。两条写入路径写的是同一个东西，不存在两个编号 |
| 用户号有几位 | Task Scheduler MAS 的画板与旧版接口：10 bit（`user_id[9:0]`）；TS LLD v0.5 的 Stream Map 表项 `UID_W = 10`，而它的对外接口表是 16 位 | RV Core MAS 的自定义 CSR：“有效位宽暂定为 12bit” | 原始文档里只有这两处给了位宽。Router 的包格式那一节把 `user_id` 列在“硬件相关的其余字段”里，没写宽度。10 bit 那一档在接口表与画板上多处一致，12 bit 那一档原文自带“暂定”。**要问设计者**：用户号的值域多大，一个 LPU 里同时在飞的用户上限是多少。建模先按 16 bit 建，装得下这两种取值 |
| 取指 / 访存不对齐异常的归属 | RV Core MAS decode 优先级表：addr misalign 单列一类 | 同文档 ITCM 与 Exception 两节：不对齐归入 access fault | 按 decode 优先级表 |
| RV core 的 C / A / F / D 扩展 | RV Core MAS Features：C 支持、**A 考虑支持**、F 与 D 不支持；MAS_TOP：RV32IMC；同文档“标准指令集”小节仍是“是否支持 C？是否支持 AFD？”的问句 | 软件计算流程详细评估：**RV Core 目前支持的指令集为 RV32IMAC**，A 已经算进去 | 按 Features 建，**A 先不建**。这条影响 Share Mem 的多核一致性怎么做：三个 RV core 共享 Share Mem，A 一旦确定支持，task 之间的同步就有原子指令可用 |
| 逐级归约要不要单独的 credit | Router MAS F-032、F-033：下游 Reduce credit 按 UserID 加目标方向、以一笔 reduce 任务为粒度，表项的 `reduceNeedMask` 置位时头一个 flit 前取得准入，下游交付完结果还一次 Reduce release | 《Core 间数据流通信机制》：“Rmem跟Cmem维护的逻辑一致，也是按照用户数量平均划分，容量的维护机制跟Stream资源的维护保持一致，这样不需要额外的Credit机制”，一项 Stream 资源同时分配下游的 CoreMem 与 ReduceMem | **已定**：网络输出逐flit使用VC credit，业务准入沿用Stream；TS另管本级RMEM许可，接收端按用户、任务及物理上下文放行。代价是同一个用户在同一个 core 上的两笔 reduce 之间没有跨 core 的挡手：上游第二笔的数据早到时，下游那个用户的分区还被第一笔占着，包只能在 VC 上等。Router MAS 那一路是靠下游 Rmem 空项挡的 |
| B core 查询 ready 的 task 由谁执行 | 软件计算流程详细评估 GLM5 章：VU 做 check flag | 同文档 TS 章的 B core 示例：task0 为 MU | **已定：MU**。查询的 RV core 约定为 MU RV core，由它检查 Share Mem 里的表项判断有没有有效数据 |
| `SELF_START` 的适用范围 | TS MAS 旧版：仅 B core 才会有 | 软件计算流程详细评估 R core 示例：task0 `self_start=1` | **已消除**。TS MAS 现版的 `SELF_START` 是全局寄存器，描述写“当前场景只有B core\R core会出现这种工作模式” |
| Router 的 R2R 方向端口 | Router MAS：五方向互连，上、下、左、右及 Core | DATA_NOC HAS：left / right / mid 三个方向端口，加 local 与 reduce_0/1/2，5 入 7 出 Crossbar | 按 HAS 的三方向。两行的拓扑里邻居就是同行左右加另一行对称位，且与第 3 章顶层节的 data_L / data_UD / data_R 三通道一致 |
| Reduce 的累加做在哪 | Router MAS：ReduceModule 在 Router 内，16 用户 × 32 KiB 上下文，RMW 原位累加，自己维护下游 Reduce Credit | DATA_NOC HAS：Router 内不设 Reduce Buffer，累加由独立的 Rmem 子系统完成，reduce credit 是单独的流控网络 | 按 MAS：ReduceModule 在 Router 内 |
| Router 的仲裁粒度 | Router MAS 有一节“Interleave 和整包的对比”，只列两案优劣、未给结论 | MAS 正文与 DATA_NOC HAS 都是 flit 级（“Packet 在 VC 间按照 Flit 的粒度传输”“矩阵仲裁以 flit 为基本节拍”） | flit 级，整包只作为贪婪仲裁的优先级偏好。原第 3 章写成“整包粒度（选定）”是误读，已更正 |
| R2R 带宽 | Router MAS：相邻 Router 双向各 256 GB/s @1GHz | DATA_NOC HAS：R2R 210 GB/s、C2C 90 GB/s | 两个都记：256 GB/s 是 256 B/T @1GHz 的接口理论值，210 GB/s 是 HAS 记的有效带宽。是否同一口径待确认 |
| 重发（reissue）任务在不在主任务链上 | TS MAS 与 core 内调度机制：重发 task 不在主线任务链上，可并行执行、优先级最高（TS MAS 里“重发的 task 任务优先级最高”这句自己划了删除线） | Top 模拟器详设的三条【讨论结果】：插在任务链中实现，同一 stream 下即使无依赖也按任务顺序执行，需等它完成才能执行后面的任务；软件计算流程详细评估的 normal core 示例把 broadcast reissue 排成 task 1' | **已定：在链上**。TS MAS 现版把重发的搬入与搬出配成链上的项（`TASK_TYPE` 1～3），“重发的 task 任务优先级最高”一句与 DTE 优先级的方案 1 都划掉了，DTE 发射全部按最老用户仲裁 |
| 进 core 的数据缓存在哪 | Router MAS：CoreStation 内的 HeaderFIFO 与 OutputBuffer，只给职责不给深度 | DATA_NOC HAS：Router 内 local 端口不设 buffer（容量栏写“—”，注明“buffer 在 DTE-local 桥接”），缓冲在 Router 外的 DTE-local 桥接模块，Router→DTE 60 flits × 288 B ≈ 16.9 KB | 结构按 MAS（不设独立桥接模块，逻辑拆到 core 与 Router 两侧），**容量按 HAS 的 60 flits** |
| Router 的 Credit Bypass Route | Router MAS F-018～F-020：20 bit 的 `RTR_RELEASE_ROUTE`，按四个输入方向分四组，每组 5 位，分别表示转到上、下、左、右与 Self（Core）；Stream 与 Reduce 两类 release 只按它转发 | 软件计算流程详细评估 Router 章：删除了该配置小节，软件只配 RouterTable；但同章检查清单与释放表仍引用 Credit Bypass Route | 按 Router MAS。R2R 端口按“Router 的 R2R 方向端口”那一条定成三个方向，所以模型是三个输入方向各 4 位（mid、left、right、本级） |
| Matrix Mem bank 数 | MU MAS：32 个 Mmem Bank 与 32 个物理 Lane 一对一 | Mmem MAS：按 64 个 lane 分成 64 bank | **未解**，直接影响 8KB/T 的组织方式 |
| MU 计算流水深度 | 参数表：执行拍数 / 流水延时 9T | Matrix exe 章节：单 Lane 内深度 10 级 | 差 1 拍，可能是含 / 不含某一级 |
| loop_bp 项数 | Key features：最多 4 项 | 参数列表：4/8 | 未定 |
| `STALL_COMPUTE_ON_CREDIT_MISS` | Top 模拟器详设正文：默认 `true`（credit 不足时本核停算） | 同文档仿真参数表：默认 `false` | **未解**，影响 credit 阻塞时的吞吐建模，两种模式文档建议做对比仿真 |
| VU 读 CoreMem 带宽 | Top 模拟器：`vu 读/写 core mem = 256 B/T` | 一体化模拟器：`VU_Dsa 访存端口 CoreMem bw=64B`；VU MAS：128 B | 三处不一致，**建模取 VU MAS 的 128 B** |
| chip 内 core 网格 | HAS 汇总版“Harvest 规则”：每 HBU 内 **2×5** 个 Bach Core，按坏核数分 A / B / C 型，坏 >2 个废弃；保证至少 8 个可用，**多于 8 个的富余 core 作特殊功能用**；板级左右两列只能 A / B 型。硬件 MAS / 需求分析同口径；《DATA_NOC DE HAS》REQ-ARCH-030 与 `core_bad_mask` 一节：每颗 chip 至多 2 个坏 core，一行至多 1 个，坏 core 的 Router 数据通路正常 | 两套模拟器一律按 **2×5** 建模；《仿真评估工作》里 Pysim 的基准 map 是 **2×4** 的 chip、B core 与 R core 另加；《编译器设计构想》：软件把可用的计算 core 当逻辑 2×4 用 | **已定：统一 2×5，中间列坏 core2、core7，两侧全好**。每颗 chip 10 个 core；`core_bad_mask` 每颗 chip 10 bit，中间两列 `0x084`，第一列与最后一列 `0x000`，由软件配置，编译器只实现这一种坏 core 布局。坏 core 上只有 Router 工作，数据经它透传。每颗 chip 8 个计算 core，编译器当逻辑 2×4 用；第一列组头 chip 的 core0 是 B core，最后一列 chip 的 core9 是 R core，其余不是计算 core 的好 core 不派角色 |
| map 文件里的 core 网格 | `.map` 示例 meta：`core_cols_per_chip: 4` | 模拟器基准配置：`CORE_COLS_PER_CHIP = 5` | **已定：统一 5 列**。chip 一律 2×5，列数是全局常数。编译器把每颗 chip 的 8 个计算 core 当逻辑 2×4 用，这个逻辑格子是 4 列，与 `.map` 示例的 4 相同 |
| VC 数 | DATA_NOC HAS 正文与 VC Buffer 表：每 Input Port V = 4 | 同一份 HAS 的 VC 使能 mask 20-bit、`vc_id` 5-bit、Area 预算按 VC0–19（4×20 + 16×2 + shared 20 = 132 flits/port） | V = 20 是 2026/08/19 缩减 VC 之前的残留，但 Area 与 Architectural Guidelines 两节没同步。按 V = 4 建 |
| VC private 深度 | HAS 3.2.2 与 REQ-ARCH-025：Private per-VC 深度 = 2（防死锁），软件可配 | 同一份 HAS 的 VC Buffer 结构表：Private ~20 flits/VC（覆盖 RTT），总量 4×20 + shared 20 = 100 flits/port = 25 KB | **已定：按 20**。HAS 的 VC Buffer 规格表是缩减到 V=4 之后的正式口径（`V=4`、`Private(VC0–3) ~20 flits/VC`、`总 4×20+20=100 flits/port=25 KB`）；`2` 是 REQ-ARCH-025 的软件可配下限，也是已删除的 VC4–19 那一档的深度，HAS 正文写作“极限情况……保证每 VC 基本传输需求” |
| R2R 单跳延迟 | DATA_NOC HAS 性能预算：internal 6 ns + wire 10 ns = **16 ns/hop**，mid 无走线延迟 | 第 5 章延迟表与性能需求规格：T_R2R = **40 T** | **已定：按 HAS 那一档拆开建**。Router 内部那 6 ns 由 Router 自己的流水级走掉，链路只担走线，左右填 10 T、mid 填 0 T，两段加起来才是一跳。ASM-03“R2R round trip 最大不超过 20 cycle”也与这一档相符。40 T 是把两段并在一处的口径，逐级建模的模型再填 40 就把 Router 内部那一段算了两遍。**仍待与设计者确认** 40 T 是不是含 core 侧往返的端到端值 |
| Rmem per-port buffer | HAS ASM-07：per-port **128 flits** | 同一份 HAS 的 Area 预算：Reduce 子系统 3 port × **32 flits** | 未解 |
| ReduceBuffer 容量 | 《通信机制（分析过程）》：单用户最大 reduce 数据量 8K × FP32 = 32 KB，正反双份 = 64 KB；《Core 间数据流通信机制》：16 用户 × 32 KB = **512 KB** | Router MAS F-044：16 用户 × **32 KiB** = 512 KiB，“容量统一按 FP32 驻留数据量计算。一个 32 KiB 分区最多保存 8192 个 FP32 元素，对应 16 KiB 的 BF16 输入数据”；《TS_通信机制》：每用户 **16 KiB** | **已定：每用户 32 KiB，按 FP32 驻留算**（Router MAS F-044）。《TS_通信机制》的 16 KiB 与 MAS 按 BF16 输入量折算的数一致，它给了拆分的理由：单用户 reduce 次数不定，Rmem 装不下全空间，**要求软件把一笔 reduce task 拆成 4 笔 8 KB 的 reduce task** |
| CoreMem credit 粒度 | 《通信机制（分析过程）》：按 **1 KB 粒度**划分，path 按自己需求申请 | DATA_NOC HAS：按 **user 粒度**的资源表格，16 项 | 未解。前者是容量记账，后者是表项记账 |
| `stream_credit`（HAS 旧版叫 `coremem_credit`）初值 | HAS Boot 流程：上电 `stream_credit[port] = 0`，由正常 Core 上电发初始化脉冲逐步初始化 | HAS 4.3.2：`stream_credit` 为 16 个用户的状态表，**默认为全部使能状态** | 按 Boot 流程那一套（上电 0），另一处是描述稳态 |
| chip 内 mid 接口 | 第 2 章与我们的 Chip 装配：`core[i]` 与另一行对称位置的 core 的 mid 端口全部对接（`core[i+5]`） | HAS 旧版 ASM-01 括号：“中间 router mid 接口不连接” | **已定：连接**。HAS 新版正文改成“简化二维 Mesh（**中间各列连接作为备份通路**）”，并新增 REQ-ARCH-037，用于提供多路径选择 |
| VC Buffer 容量 | 《通信机制（分析过程）》按容量记：reduce 专用 VC3 16 KB、三个共享 VC 各 8 KB、三方向各一套，合计 **120 KB** | DATA_NOC HAS 按 flit 记：private 20 flit/VC × 4 加 shared 20，一个方向 100 flit ≈ **25 KB**，三方向 75 KB | 未解。前者按 reduce 要整包缓冲反推，后者按覆盖 credit 往返反推 |
| ReduceBuffer 容量的第三种口径 | 《通信机制（分析过程）》另一处：Core 必须一次性整包发进 ReduceBuffer，一个 Token 8192 × 2 B = **16 KB** | 同一份文档前文记 64 KB；Router MAS F-044 记 512 KiB | 三个数在同一条链上：16 KB 是单包下界，64 KB 是正反双份，512 KiB 是 16 用户并发。**已定：按 Router MAS F-044**，16 个用户各一个 32 KiB 分区 |
| CM 物理带宽 | 《Core Memory 容量带宽需求推导》：**512 B/cycle @1 GHz**，每用户 50 KiB 写 + 50 KiB 读 | 《通信机制（分析过程）》：**256 B/cycle @1 GHz**，每用户 25 KiB 写 + 25 KiB 读；《Cmem MAS》：**(1 KB + 32 B)/T** | 三份不一致。第 5 章按 512 B/cycle 记，Cmem MAS 的 1 KB/T 是 8 bank 全开的峰值，两者不是同一个口径 |
| Reduce credit 的粒度 | DATA_NOC HAS：core 与 reduce 之间按 user 管，reduce 与 reduce 之间“按照 flit+user 的粒度进行 credit 资源管理”，“reduce 之间 creidt release 粒度为 flit” | Router MAS F-032、F-033 与“Reduce Credit 与传输 Credit”一节：下游 Reduce Credit 以 UserID、目标方向、Reduce 任务为粒度，`reduceNeedMask` 置位时在任务头一次向目标方向提交前取得准入，“后续 Packet 和 flit 复用该准入”；下游做完这笔任务、结果全部交付后还一次 release，“不是逐 flit VC Credit 返还” | Router MAS 的任务级准入与逐 flit VC credit 是两层机制。当前模型采用上表“逐级归约要不要单独的 credit”的选择：输出只查 VC credit，接收端按用户及 RMEM 物理上下文反压；独立下游 Reduce credit 未实现 |
| ReduceMemory 的结果按整包还是按 flit 出 | Router MAS F-013：“ReduceMemory 的各输入流和输出流均以完整 Packet 为单位进行仲裁和传输”，“结果也以整包形式输出” | 同一份 MAS“整包输入输出与结果流水”一节：“当某个结果 flit 所需的操作数均已完成累加，且输出端满足接收条件时，该 flit 即可进入输出流水，无需等待整笔任务或整个 Packet 的全部结果生成” | 读作不矛盾：F-013 管的是包与包之间不交织，后一节管的是一个包内部按 flit 流出。模型按后一节，包发出首 flit 后锁定到尾 flit |
| `core_type` 还在不在 | TS MAS 功能清单 1(e)：“支持软件配置本core的类型 core_type：B/R core，普通core”；Block Diagram 的 CFG_REG 一行也列着 `core_type` 与 `B_core_direction` | TS MAS 寄存器表：全局 `SELF_START`（`0x214`）；TS LLD 参数表：“0为Normal，1为Special；替代core_type” | 按寄存器表：`SELF_START` 取代 `core_type`。`B_CORE_DIRECTION` 地址映射里没有，按《TS_通信机制》保留 |
| VU 的 event 送不送 TS | VU MAS F50：“可向 TS 发 Event 同步信号”（`vu_event`） | TS MAS 与 TS LLD 的 `vu2ts_done_ch` 都只有 uid、tid、sid、valid，没有 event | **已定：只有 `EVENT_EN` 才发 `dsa_done`**，同拍拉高 `event`。TS 把这一路当普通 DSA ACK，不另处理 `event` |

## TS 与 DTE 详设内部不一致

本次对齐以TS revision6452、DTE revision24200为依据。以下每项保留原文分歧及影响。模型中已选定的位域和时序用于 kernel 与回归；其余未统一内容保留为硬件待定项。

| 编号 | 原文位置和分歧 | 文档处理与实现边界 |
| - | - | - |
| U-DTE-MODE | DTE“复杂配置寄存器说明”写mode[9:7]、seg[6:4]、header[2]、SM[1]、ACK[0]；模板说明/hex例以mode[2:0]、header[7]、SM[8]、ACK[9]，且保留bit3；审查时的旧代码seg[5:3]/保留bit6又不同 | 用户已确认模型采用 mode[2:0]、bit3保留、seg_valid[6:4]、header[7]、SM[8]、ACK[9]。kernel、模板与模型使用这一位序；原文仍需统一 |
| U-DTE-STOP | AddrDecode、RVCore IF与全局Q段规定RUN/STOP满队列均反压；Status Ctrl段规定STOP恒ready并丢弃满队列Trigger | 用户已确认模型采用STOP队满反压全部RV访问。控制网络可恢复运行；原文的另一种STOP行为仍待统一 |
| U-TS-CREDIT | Credit Ctrl数据流/时序表/画板E2扣账并READY；场景5.4前后8槽两阶段E3提交 | 用户已确认模型采用E2提交，方向扣账与READY在同一次Map更新中生效。原文E3场景仍待统一 |
| U-TS-CACHE | Stream Map字段表不保存PC/VCID、Arbiter按winner查LUT；场景与内嵌表要求Map安装时缓存PC/VCID、Arbiter直接取 | 命令必须持有PC/实际PID/VCID；缓存模块和寄存边界待统一，面积与精确时序不能混算 |
| U-TS-WEIGHT | TS-FUNC-001将loading 0→1描述成发PC请求；后续权重场景规定置位只进模式、仍等Trigger | 本项目loading只切模式，已接受Trigger生成PC-only零身份请求；每加载阶段一次Trigger，切业务须等数据、响应和队列排空 |
| U-TS-RETIRE | 2.2 valid描述仍写保持到Router接受；功能、顶层接口和生命周期规定无外部Ready、单拍UID/valid | 采用多处一致的valid-only目标合同，内部Retire Hold ready顶层固定1；保留原文陈旧语句的来源分歧 |
| U-TS-CHECK | LUT-FUNC-010说不再检查旧Reissue组合；配置错误表仍检查ReissueOut credit、P2P wait_wake和partner类型 | 软件继续配置合法组合；硬件保留哪几项检查待统一。Reduce不依赖credit_en是明确规则 |
| U-DTE-QTABLE | DTE参数同步引用仍称Q只保存尚未dispatch任务；主体结构、状态机及验证规定持有至Ack退休 | 采用多处一致的16项全生命周期Entry；参数引用旧说明不定义另一个派发FIFO |
| U-TS-RECV | TS ATTR/软件约束/接口/5.2为00 Core、01 DSA、10双ACK；Task Done旧段为01双ACK、10 Router | 采用寄存器及后续场景一致的00/01/10目标；六输入FIFO说明须同步分类，禁止把RMEM返回归为TID完成 |
| U-TS-RTAB | TS 2.4旧表方向5bit（上下分开）及VCID[6:5]；功能、Credit Ctrl和LUT表方向4bit（南北合并）及VCID[5:4] | 逻辑方向按南北/西/东/Local，物理bit未统一前不提供可执行RouterTable打包例 |
| U-TS-CREATE | DataIn文字说Create/Skip/claim内部无独立ready；内嵌接口表GzIkmi仍将Create列为valid-ready | 原子分配和Candidate提交一致；具体Create握手及容量返回边界待统一，不能用通用accepted写口替代所有Map更新 |
| U-TS-LISTEN | 旧Router MAS提出TS注册四身份、Router监听16事件并返回Grant；新TS详设使用三方向Credit Ctrl、每SID Pending和方向返回 | TS提交三方向许可，Router保存实体用户表并按真实release返回已持有方向；旧注册／SID/TID Grant不接入业务通路。旧Router MAS另有“监听功能移到DTE”的表述，均不改变本项目的TS许可所有权 |
| U-TS-VCID | TS旧接口表将VCID写成Kernel/Transfer/Reduce/ReduceTwice操作码；RouterTable和DTE Channel选择把VCID作为实际0～3通道 | 当前模型按任务实际PID查询路由VCID并随Trigger锁存；旧操作码表与物理VC的对应由Owner统一，不用未冻结操作码改现有Channel选择 |
| U-DTE-LEN-EXAMPLE | DTE总原则和CFG_DATA_LEN寄存器表规定字节单位、8B为软件对齐；分拍示例仍有data_len_i×8的旧公式 | 模型与配置示例按字节长度计算，内部拍数为ceil((起址mod256+长度)/256)；旧乘8公式需要原Owner同步修正 |
| U-TS-WINDOW | 通信机制提出超前窗口，旧衍生稿的“阻塞UID≥M+N”与“允许发到M+N”例子边界不同，窗口寄存器位置未定义 | 保留应用场景、推算假设、容量及候选过程；不冻结等号边界，也不据旧模型path_flowctl选择硬件存储 |
| U-DTE-TASK | DTE“软件限制”要求一TS Task一Packet一个传输任务；其它章节和现有软件用最后一笔ack_ts_en表示一个Task拆多搬运 | 本项目允许一个TS任务包含多个DTE搬运条目，最后条目反馈ACK；R Core两操作数沿用这一方式。原文一Task一Packet条款需同步修订 |
| U-DTE-ERROR | Status Ctrl ERROR文字允许已下发任务完成；后续控制接口限制ERROR推进，Flush和残包终止仍TBD | ERROR停止新Channel下发及正常SM/Ack，保留Except队头并收集已Fire响应；残包终止、恢复和cancel指针修复仍待定义 |

原文依据：[TS详细设计](</home/colin/develop/perfechpitch/_refs/Task_Scheduler_详细设计文档模板.md>)的配置、Task Done、Credit Ctrl、Stream Map与场景；[DTE详细设计](</home/colin/develop/perfechpitch/Bach/05_五、详细设计/02_DTE DSA详细设计文档.md>)的Config、Status Ctrl、Glb Cfg TaskQ和接口。原始画板部分已经注明旧图，项目图只表达上述一致合同。

### 关联软件与 Router 接口仍需统一

DTE的32B包头已包含16B sw_aux，Packet长度为32加使能数据段总长。当前kernel／Router模型又在payload前面保存16B软件辅助信息并跳过加法。该兼容前缀是否继续作为业务数据、怎样与sw_aux对应及怎样计算长度，须与软件／Router协议统一；DTE不自动插入这段数据。归约示例的有效元素数与模型前缀占用分别记录。

R Core的两操作数归约算法沿用已选定方案。模型按用户确认的基线，将两Packet放在一个TS任务内，最后一笔开启ack_ts_en。本地Core与DTE双ACK完成该任务，Router返回独立释放RMEM。本项目采用多条搬运、最后ACK的任务边界。控制数据槽复用仍须等待执行完成与RMEM归还两类条件。

## 当前模型对待统一项的处理

以下约定用于运行模型和软件回归，硬件文档仍需逐项统一。

| 待统一范围 | 模型采用的约定 | 后续需要确认的内容 |
| - | - | - |
| 配置位序和许可提交 | TRANS_MODE使用已确认的低位mode布局；方向许可在E2提交；STOP队满反压；多搬运任务最后一条ACK | 原文的其它位序、E3场景、STOP丢Trigger说明和一Task一Packet限制 |
| RMEM本地复用 | 已准入用户可提前接收最早尚未完成的归约分量，本地计算与上游接收可以重叠。RMEM占用期间只接收同一任务与同一路径的分量。SELF_START重生用内部通知清除旧UID的空闲RMEM上下文，不产生对外release | 提前准入与RMEM许可的硬件寄存边界 |
| TS许可与返回 | 编译侧按stream_need生成需要许可的方向，数据转发方向仍由RouterTable决定。真实release归还TS已持有的许可；转发流量不产生重复归还 | 方向返回的物理寄存边界和缓存实现位置 |
| TS寄存边界 | Map保存当前PC；仲裁器按实际PID读取VCID，并保持选中的命令。Create与其它Map更新均提交带分配代次的请求 | PC缓存位置、Create的物理握手和面积统计边界 |
| 特殊模式与配置检查 | 权重模式等待Trigger，只发PC和零身份；SELF_START在首Trigger激活。Reduce不检查credit_en，Reissue仍检查配对组合 | 整分片加载阶段划分及Reissue检查清单 |
| DTE控制地址 | `DteModelCfg.control_base`默认0x400。基址加0控制STOP/RUN/step；加4读Idle；加8读及清首错；加12读异常身份；加16读占用；加20取消未下发条目；加24请求Flush；加28读首错entry_id；加32读异常身份高位；加36读中断状态；加40读写模型中断mask | 控制字段、异常编码与清除规则的硬件冻结值 |
| DTE观测地址 | Profile默认0x800/0x804读取下发/ACK计数；Debug默认0xc00，每通道64B，读取两半上下文和在途数。TaskQ每条256B，保存完整配置和分开的身份字段 | PMU、Debug和TaskQ状态的硬件偏移、编码及统计项 |
| 存储端口抽象 | CM/MM逻辑256B请求占用两个128B bank单元，两个ready同时有效才发；分拍响应合并后返回原通道。topK使用两个实际128B请求 | 端口各路延迟、MM cache边界及周期模型的精度 |
| 软件数据布局 | 段长按8B补齐。Message分别保留有效数据长度、线上总长度和诊断身份；出口桩在尾拍读取完整数据并去除补齐字节。B Core将数据、scale、topK连续保存到MM，逻辑长度6592B、槽位跨度6656B，槽位按256B对齐；权重scale使用MM中32～33MB的软件保留区，并映射到MU的scale视图 | MM保留区与cache布局；线上长度与软件包头读取视图的最终编程契约 |
| 错误与Flush | 错误归原条目，按队头记录首错并停止新下发；已发出的访存等待响应。Flush取消未下发条目，已下发条目排空后回收，抑制正常ACK及尚未发出的SM写 | 残包终止、上游继续送帧、ERROR恢复、复位与Flush之间的协议。模型不定义新的错误flit格式 |

GPU/token身份和scale/topK格式信息保存在模型诊断上下文中，HeaderTable的800B及CM/MM的32B包头按硬件字段计算容量。归约payload中的16B兼容前缀仍作为软件数据保留，不自动并入sw_aux。

## 已实施的 TS／DTE 基线

| 范围 | 已实现行为 | 实现位置 |
| - | - | - |
| TS入口 | Lookup/Prepare/Candidate，completion与claimed排除，Future接受旁带 | UserMatch、DTE Arbiter与TG/Map生命周期 |
| TS完成 | 顺序双ACK FIFO、仅合法Core-only PID更新，RMEM只还空间 | TaskDone、FIFO与PID资格、旧测试断言 |
| TS许可 | 三方向计数初N、每用户新增方向原子授权、RMEM每SID独立占用 | CreditMonitor、Router返回接线和N内指针 |
| TS特殊模式 | 首Trigger激活SELF_START，本地下一拍重生；权重零身份PC-only | InitFinish、Bypass/加载Hold、软件启动流程 |
| DTE接纳/完成 | 全局16Entry持有至退休、5Ch Ptr、五WR Finish、SM/Ack按序 | Commit/CompletionRS架构、写bresp与SM响应 |
| DTE存储/包头 | 窗口译码、HeaderTable16×32项800B、32B包头 | AGCU、Hmem、lane、Message及kernel地址 |
| DTE数据流 | 双128B、8B重对齐、M Top两笔topK、每VC credit/flit交织 | XBar/存储/MU端口、Buffer、OutArb/CoreStation |
| DTE控制 | 下一拍读、RUN/STOP/ERROR、状态/TaskQ/HT回读及Except有序 | Regfile/Status、软件可观察错误、控制验收 |

DTE PMU/Debug物理偏移、异常mask位序、cfg_noc/RV同目标访问次序、RD完成保留站容量、Flush／残包恢复及方向返回的硬件寄存边界继续保留TBD。已实施功能与验证位置保存在各单元文档；历史回归结果保存在修订记录中。

## 当前实现仍有的缺口

| 范围 | 实际状态 | 影响 |
| - | - | - |
| DTE 波形 | data/scale 位置按 Route 推断，topK 位置按内容类别推断；尚未逐段保存端点 | 多端点任务的轨道位置可能不准确；wr_done 表示请求发完，不能作为物理完成或退休时刻 |
| DTE PMU | 当前仅有下发及 ACK 等既有观测，新增详设计数不纳入当前建模范围 | 参数文档中的 PMU 逻辑项是目标统计口径 |
| 权重加载完成 | kernel计数并提交搬运后yield；每阶段由Trigger启动，整分片分阶段及完成通知仍待定义 | 计数增加或 RV 返回不能替代写响应完成；切换业务模式须确认数据与 DTE 均已排空 |
| VU舍入 | 原始寄存器的101为RNO；`numeric/round.h`只实现000～100，101/110回落RNE，111被配置检查拒绝。VALU／VSFU的FP32算术使用宿主float | RNO和FP32算术的逐模式舍入仍未实现；RS可用性另属硬件待定 |
| VU异常数值与地址 | VALU／VSFU逐元素运算使用ClampNanInf；当前kernel含16B软件前缀的向量地址会触发CM_ADDR_ERROR，模型继续执行 | 与硬件非收口位置透传NaN/Inf及地址错误处理存在已记录的建模差异；参考实现相同处理不能替代异常语义验证 |
| MU NVFP4 | `mu/regfile.h`的权重类型译码将NVFP4回落BF16 | 硬件算力表中的NVFP4不代表模型已有该格式的数值实现 |
| PCIe双路x16 | Switch的lane_rr只计数；LPU每端口每方向装配一条Link | 双路独立时序和乱序未建模，现有路由／组播测试不覆盖该目标 |
| GPU入口与数据格式 | 通用InStub默认6368B；MoE的SpreadHarness使用6592B业务布局，直接驱动入口桥 | 专家信息编码转换、跨GPU公平grant及完整GPU/DPU协议不能由MoE多token回归推断 |
| 装载与Boot时序 | `LoadBundle`开钟前Preload路由表、镜像与配置；SCP桩按另一套事务序列推进 | bundle装载成功不包含真实SCP、ctrl_noc及MB级权重传输的启动耗时 |
| RV任务身份兼容 | 现行DataIn TID由TS匹配；exec.h仍接受写TID，VCID保存在任务上下文而非独立软件CSR | 写TID旧兼容入口与硬件只读约束仍需清理或明确边界 |

## 原始文档里标注为空缺或待定的内容

| 模块 | 缺什么 |
| - | - |
| Bach_core_MAS_TOP | 整篇标 `pending`。Glossary 全空；DSA / Cmem / Matrix mem / Share mem 的异常类型全空；“如何上报”未写；地址空间分配只有白板引用没有正文；“软硬件编程契约”章节大量只有标题（Kernel 三段式代码拆分原理、统一偏移地址编程规范、MOE 场景的 Boot / Launch / 动态描述、动态专家调度、Gdb、jtag） |
| Router MAS | Programming Model 起（Register Map、Interrupts、ISA、Programming Sequence、Locking、Performance、Power、Area）**全部是 eFUSE 模板残留**，与 Bach 无关。Signal List 也是 eFUSE 的 AHB 信号。“业务Credit的路由”引用的《Harvest下业务级Credit的路由机制》已在原始库同步 |
| TS MAS | 标 `编写ing`。Signal List 表格大量字段空白（宽度、方向）；`EXCEPT_CHECK` 一节的 Interface 与 Timing 是 eFUSE 模板残留，`IO_REG` 一节只剩标题；MISC / Application scenarios / Interrupt Handling Sequence / Programming Sequence 的步骤与 Performance / Power / Area 全部是模板 |
| RV Core MAS | Signal List 空；各子模块的 Interface (LLD) / Timing Diagram 基本空；CSR 具体实现哪些“待讨论”；B core 软件流程标题重复且内容为空；Performance / Power / Area 只有标题 |
| Cmem MAS | Function Description 整章只有标题（regfile / agu / acu / ldq / stq / Matrix exe，且这些标题明显是从 MU 文档复制的，与 Cmem 无关）；Performance Targets 表格空；Memory List 是 eFUSE 模板残留 |
| Mmem MAS | Function Description 同样只有标题 |
| MU MAS | 大量章节是注释形式的写作提纲（以 `> //注释：` 开头）；寄存器描述全部指向内嵌 sheet，原始库已同步可读取sheet为CSV；仍须核对字段的冻结状态 |
| ISA 描述表 | `Vector` 和 `DTE` 两个 sheet **完全为空**；`Matrix` sheet 里是通用模板样例（ADD/SUB/LW/SW），不是真实的 MU 自定义指令 |
| 寄存器描述表 | “地址总概”只有分类没有地址范围；`TS` sheet 为空；`Router` sheet 只有三个条目名称（驻留专家掩码、Message id 记录、路由方向表），无地址无位域 |
| Shared DM | 整篇只有一个标题，无任何内容 |

## 原始文档与资源可用性

TS详细设计正本位于原始库`_refs/Task_Scheduler_详细设计文档模板.md`；DTE详细设计正本位于`Bach/05_五、详细设计/02_DTE DSA详细设计文档.md`。两者的正文、可读取表格和画板均已本地同步，不能再以“只有DTE副本”作为未对齐原因。

原始库的`MANIFEST.tsv`记录来源身份，`FIGURES.md`与同步状态记录附件资源。个别云文档的导出限制属于资源访问问题，不能推断其设计没有定义；原始引用使用该库路径，项目不复制新的原始设计文档。

## 设计上仍未确定的问题

**Router**

* 整包传输方案下“长包阻塞可能有死锁场景，需要在架构层考虑不会出现死锁”，死锁避免的具体论证未写
* Router 表项示例里一处原文未定：C0 在 Path2 上要不要查输出端的 stream credit table。原文另一处“C8 在 Path1 上进 CoreMem 重发时 Core 位是否也要置位”已不成立：出方向掩码里没有 Core 位，进不进本 core 由 `path_core_bypass` 单独判定
* 一笔 Reduce 任务含几个 Packet、任务边界靠什么标出来，Router MAS 没写。MAS 只说“同一任务在每个目标方向只取得一次用户级准入，后续 Packet 和 flit 复用该准入”；《TS_通信机制》把一笔 32 KB 的 reduce 拆成 4 笔 8 KB 的 reduce task。模型按一个 `reduce_seq` 的包算一笔任务
* `RTR_RELEASE_ROUTE` 的复位值，以及没配路由的口收到 release 怎么处理，Router MAS 没写。模型里复位值是一个方向都不转，收到的 release 一律先交本级：好 core 删来向那个方向这个用户的表项，坏 core 没有表、删不到就算了，配了掩码的口再按掩码往上游转
* 本级退休发出的 release 从哪一组进静态路由，Router MAS 没写：`RTR_RELEASE_ROUTE` 只按四个 R2R 输入方向分组，这一笔不从任何输入方向进来。模型把它往三个 R2R 方向都发一笔，没向本 core 申请过的上游查无此项，丢掉
* Rmem 16 个分区都占着时新用户的第一笔怎么办，Router MAS 没写：反压那一段只列了“Rmem Bank、读改写通道、输入缓冲或输出空间暂时不可用”。模型对这一路输入反压，等有用户 Retire 放出一个分区。分区用时才分配之后，它与 16 项 stream 表不再一一对应；分区被上游先到的用户占满、本 core 的用户推进不下去时会不会卡死，原文没有论证
* 只合并上游分量、本 core 不出分量的那一级，本 core 的 TS 里可能没有这个用户，谁对它发 User Retire 放 Rmem 分区，Router MAS 没写：原文只说“TS 发送 User Retire 触发Rmem释放”。模型里分区只等本 core 的 TS 对这个用户发 Retire。**量过一次**：把逐行归约那条链改成全部在 Router 上做、沿途的 core9 不派角色（因而没有 TS）之后，连续跑 32 个 token 在第 17 个用户上停住——16 个分区被前 16 个用户占满，没有 TS 就没人发 Retire，再也放不出来。所以做归约的那一级必须是有 TS、会对这个用户发 Retire 的好 core
* 结果流里后面的结果还没算好时，同一出口别的 VC 能不能先走，Router MAS 没写：F-012 只说“只有当前 Packet 因下游条件不满足而阻塞时，才允许在合法的 flit 边界切换到其他已就绪的 VC”，结果没算好不是下游条件。模型只锁同一出口同一 VC，别的 VC 照走
* Core 多操作数模式怎么配，Router MAS 没给编码。线上版 Router MAS 只写了“Rmem支持一个方向来的多个操作数（需要配置特殊模式），但是不支持操作数精度不一致”，本地 perfechpitch 里的旧版没有这一句。模型的取法：表项 `flow_dir` 的 reduce1、reduce2 两位都置时，core 方向把同一用户在这条 path 上先后送出的几包依次分到 `reduce_in_mask` 置位的几路，第 k 包走第 k 个置位的那一路
* 同一个 core 的 Rmem 先后给同一用户做两笔 reduce、其中一笔的上游分量来自别的 chip 时，怎么保证到达顺序，Router MAS 没写：F-030 只说“同一 UserID 的 Reduce 任务在各输入流中的到达顺序必须一致……输入顺序由上游保证”。dot core 的 Rmem 对同一用户先做 chip 内归约、后做行链，上一颗 chip 的行链分量什么时候到与本 chip 的进度无关。当前模型直接接入 RMEM：已准入用户只可接收最早未完成 Reduce 的分量；占用期间按任务号及 PID 保护，首 flit 获准时预留物理上下文。本地 task14 发送一份行结果，dot core 统一为 15 项链。此前未加任务身份保护的直接接入曾造成互等；Core Mem 暂存方案是历史替代方案
* **VC 机制到底实不实现**。原文的原话是“实现 VC 机制需要很大的额外面积、设计复杂度和验证空间，成本极高。具体是否实现需要模拟器介入，综合判断开发复杂度和效果收益”。这是本次建模要回答的问题之一，不是文档缺口
* “Broadcast 过快引起空泡”这一档的定量结论，原文明确写了“需要模拟器介入协助确认”。前提是同一个用户在 core0 与 core2 上的处理速度不同，而计算量分布均匀时差距主要来自逐级 Reduce
* P2P 流量控制的“流量控制使能”配在哪一张表，原文只写“在一个 Core 配置了流量控制使能”，没有指明是 TS 的 CFG_REG 还是 RouterTable。本套文档按配在 TS 建模
* **走发送就绪门控的那些报文靠什么发现下游收不下**。同一个 user 在某方向已持有资源后，后续报文不再查 stream credit，最终用哪个硬件信号表达“下游收不下”原文没有定，把它列为需要硬件与产品拍板的开放问题。Top 模拟器做成可切换的桩：`SEND_READY_MODE` 默认 `backpressure`（链路与 Core Mem 的反压），另有 `per_user_ready` 与 `ack` 两档

**RV core**：task_queue 深度、dsa_iss 收请求的队列深度
* **寄存器地址映射**：MU未冻结字段；DTE的Config/Template地址已明确，TRANS_MODE位序、Status/PMU/Debug具体偏移仍待统一
* **超前发送窗口 N**：软件按 path 配的值，编译期确定；使能位与 N 存在哪一张表未定

**专用 core（B core / R core）**

* DTE 的读口与写口落到 Matrix Mem 同一个 bank 上怎么办，Mmem MAS 没写
  * Mmem MAS 只规定 DTE、ctrl noc、MU 三方不能同时访问相同 bank；B core、R core 上 DTE 一边把进来的数据写进 Matrix Mem、一边把存着的读出去，两个口会撞在同一个 bank 上
  * 模型的取法：照 Cmem MAS 对 DTE 端口的规定，读写两口之间 bank 冲突时仲裁二选一、只反压冲突的那个 bank，不算违反硬约束
* R core 的 Matrix Mem 要不要额外的 credit 机制
  * 原文的疑问：“Reduction Core 的 Matrix Mem 与 Broadcast Core 的 Matrix Mem 对应的话，不需要额外 Credit 机制保证？”
  * 附带的追问：总槽位数是否要按输入 / 输出能支持的**较小**那个来算，否则进得多、出得少仍会缺 credit
* R core 怎么知道本行所在的 EP 组对这个用户没有激活专家
  * 已定的行为：这一行的 R core 不相加，只转发上一行的结果
  * 未定的是判断方式。这一版 R core 固定等两笔再相加，先不看 `core_mask`
  * 附带的追问：第 0 行是链首，前面没有上一行；它所在的 EP 组没有激活专家时，这一行发什么、发不发

***

本套文档的两处来源与原始文档的 wiki 链接见 [README](README.md)。TS/DTE相关问题按2026-10-09本地同步的详设重新核对，原文矛盾与现有实现差异分开登记。
