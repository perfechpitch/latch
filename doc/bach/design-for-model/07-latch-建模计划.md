# 第 7 章　latch 建模计划

**模式**：design（陈述当前设计，动机与取舍收在各节的“取舍”段落里）

规划 latch 侧要建哪些模块、每个模块的功能逻辑是什么。
本轮只做功能正确性：数据落在哪、算出什么、什么条件下阻塞。
时序与性能模型不在本轮范围内，理由与清单见本章的“本轮明确不做的部分”。

《Bach 硬件设计建模参考》第 7 章，全套目录见 [README](README.md)。

***

## 本轮的范围

**功能逻辑**指决定结果正确性的那部分行为：模块持有什么状态、收到什么事件、按什么规则改状态、往外送什么。
**时序**指同一件事花多少拍、谁先谁后被仲裁到。两者在本套建模里分两轮做，本轮只做前者。

| | 本轮做 | 本轮不做 |
| - | - | - |
| 数据 | 每个字节最终落在哪个 core 的哪块存储、值是多少 | 搬运用多少拍、带宽是否打满 |
| 控制 | 任务链按什么顺序推进、什么条件下不能推进 | 调度延时、流水重叠、仲裁的先后 |
| 流控 | credit 够不够、会不会死锁、账目是否守恒 | 阻塞持续多久、反压传播的快慢 |
| 判据 | 与参考实现逐元素比对一致，且不变量不被破坏 | 与解析模型的延迟公式对得上 |

**为什么先功能后时序**：功能骨架决定模块边界和事件接口。骨架没定就建时序，模块一拆分时序模型全部返工。
反过来功能模型建好后，给每条通路挂上延迟参数即可长出性能模型，已有的功能验收用例还能继续用。

第 8 章列出的口径冲突绝大多数是数值层面（带宽多少、几拍、容量多大），不影响功能逻辑，
因此本轮可以在这些冲突未解的情况下推进。**唯一影响功能的是“VU 能否直接读 Matrix Mem”**，
它改变 R core 链二的 task 数，建模前需要定死一种。

## 模块清单

下面 14 个模块本轮全部要建，按依赖顺序排。
“层”一列区分三种模块：仿真开始前静态算好的、跟着时钟跑的、只是一份配置的。

| # | 模块 | 层 | 依赖 |
| - | - | - | - |
| 1 | 拓扑与地址空间 | 静态 | 无 |
| 2 | 切分与权重分配 | 静态 | 1 |
| 3 | 路由表与任务链生成 | 静态 | 1、2 |
| 4 | 参考实现（金标准 FFN） | 静态 | 2 |
| 5 | Core Mem / Matrix Mem / Share Mem | 时钟 | 1 |
| 6 | TS 任务调度器 | 时钟 | 3 |
| 7 | RV Core（DTE / MU / VU 各一） | 时钟 | 6 |
| 8 | DTE DSA | 时钟 | 5、7 |
| 9 | MU DSA | 时钟 | 5、7 |
| 10 | VU DSA | 时钟 | 5、7 |
| 11 | Router | 时钟 | 3、5 |
| 12 | core 角色：计算 / B / R | 配置 | 3、6～11 |
| 13 | Chip 与 C2C 互联 | 时钟 | 11 |
| 14 | 边界桩：GPU / DPU / PCIe Switch | 时钟 | 13 |

第 12 项不是新硬件。三类 core 用的是同一套硬件，角色差别全部来自第 3 项生成的 task_chain 与 RouterTable，
建模时不应为它们各建一个类。

## 各模块的功能逻辑

每个模块先用一句话说清职责，再逐条列出它**具体要实现哪些功能**，最后给验收判据。
凡是与拍数、带宽、仲裁先后有关的都不在这里。

### 1　拓扑与地址空间

把物理布局算成一张可查的表，供后面所有模块定位。

1. 由 chip 阵列尺寸与 core 网格（2×4，Harvest 下 2×5）生成每个 core 的 core id，
   建立 core id ↔ (tray, chip, core) 的双向映射。
2. 应用 harvest disable mask 标出坏核。坏核只保留 Router，不落数据、不开 stream。
3. 建立 chip 内地址划分：Core Mem、Matrix Mem、Share Mem、IO reg 各自的地址区间。
4. 提供“某地址属于哪块存储、某 core 在阵列的什么位置”的查询，路由表生成与落点判定都依赖它。

**验收**：任一 core id 能唯一定位；坏核集合与 mask 一致；地址区间不重叠、不留空洞。

### 2　切分与权重分配

把切分参数变成“每个 core 拿哪一片权重、扮演什么角色”。

1. 接收切分参数（EP / TP / PP / DP 各多少），按第 2 章的切分限制算出需要的 chip 数与实际部署数。
2. 按 `tp_nn` / `tp_nk` / `pptp_nk` / `pptp_nn` 之一，决定 FC1、FC3、FC2 各自在 chip 间与 core 间切哪一维。
3. 为每个 core 算出它持有哪些专家的哪一片，给出分片的形状与字节数。
4. 给每个 core 标角色：计算 core、B core、R core；计算 core 再分子角色
   （normal / concat / chip reduce / dot / result 等，随切分模式而变）。
5. 校验每 core 权重量装得进 32 MB Matrix Mem。

**验收**：所有分片拼起来等于完整权重，不重不漏；每 core 容量不超限；
EP6+TP8 与 EP1+PP3+TP16 都应算出每 core 27 MiB，与第 6 章的账对得上。

### 3　路由表与任务链生成

把角色划分变成硬件真正吃的两张配置表。

1. 由角色推出每条 path 的 `directionMask`（五个方向哪些转发、Core 位是否落核）与 `streamNeedMask`。
2. 分配 `path_id`，为每个 core 生成它那一份 RouterTable。**同一个 path_id 在不同 core 上表项不同**，
   广播路径就是靠这一点逐跳展开的。
3. 生成每个 core 的 task_chain：unit 序列、每个 task 的 `fetch_pc`、`self_start`、`end`、
   `TASK_CREDIT_EN`、`TASK_PATH_ID`。
4. 生成两套配置：weights 加载模式（`trigger_task_chain_en = 0`）与业务模式（`= 1`）。
5. 把同一份 RouterTable 同时写给 Router、DTE、ReduceModule 三方。

**验收**：把生成的表喂给 Router 模型，一次广播的落点集合等于该 path 的目的集合；三方副本内容一致；
两套模式切换后 core 行为随之改变。

### 4　参考实现

与硬件无关的金标准，后面所有验收都以它为准，**必须最先做**。

1. 按模型定义直接算一层 FFN：FC1、FC3、SiLU、dot、量化、FC2、专家加权求和。
2. 精度按 FP8 / BF16 / FP32 的实际规则处理，包括缩放因子的应用与窄化时的舍入。
3. 按 `(gpu_id, token_id)` 给出逐 token 的期望输出。
4. 提供逐元素比对接口，能指出第一个不一致的位置。

**验收**：它本身是基准。用小规模参数（少数专家、小维度）与手算结果核对一次。

### 5　三级存储

三块存储职责不同，要分别建。

**Core Mem**（8 bank，1 MB + 32 KB）

1. 按 `stream_num` 均等切分，硬件完成“相同虚拟地址映射到不同物理分片”，多用户互不覆盖。
2. 数据带 scale 段，MXFP8 时一拍 132 B。
3. 五个 master：DTE 读写、MU 读写、VU 读写。本轮只需保证并发访问后数据正确，不建仲裁时序。

**Matrix Mem**（64 bank，32 + 4 MB）

4. 三种角色都要支持：存专家权重、作 R core 时存 reduction 数据、作 B core 时存 token。
5. **同一 bank 不允许两个 master 同时访问**，冲突时只执行 MU 的访问并让错误计数器增长。
6. 地址粒度 128 B，不支持按 byte mask 读写。

**Share Mem**（32 KB）

7. 只被 DTE / MU / VU 三个 RV core 的访存指令读写。
8. 存 B core 的头尾指针、R core 的 `arrive_num` 映射表、task 之间传递的标量。
9. 不需要初始化。

**验收**：多用户并发写同一虚拟地址后各自读回自己的值；触发同 bank 冲突时计数器增长且只有 MU 的数据生效；
向 CM → MM 方向搬运被拒绝。

### 6　TS 任务调度器

core 内唯一的调度者，本轮功能项最多的一个模块。

1. 维护 stream_table 16 项：`valid` / `user_id` / `task_id` / `task_unit` / `task_dsa_en` / `task_pc` /
   `task_fsm` / `done_bitmap` / `reissue` / `end`。
2. 用 `tail_ptr` 注册新用户、`head_ptr` 顺序释放，每次粒度为 1。
3. 按 `task_fsm` 推进每个 stream：IDLE → WAIT → RDY → INFLY → FINISH。
4. **发射判定**：该 stream 最老（从 `head_ptr` 开始环形扫描，不按 stream_id 数值排序）、
   对应 RV core 空闲、前序 task 已完成、credit 条件满足，四条同时成立才发。
5. 三条发射通路，DTE / MU / VU 各一条，互不干扰。
6. **异步 DataIn**：数据提前到达时只更新 `done_bitmap`，不推进 `task_id`。
   这是任务链能乱序完成又保持顺序语义的关键。
7. **自发创建任务链**：`self_start` 的 task 复位后自动占用 stream 表项，不等用户数据；
   一条链跑完在同一表项自发创建新链。B core 复位后可直接自启动 16 个表项，
   此时没有用户信息，等 RV core 返回 `user_id` 再回填。
8. **credit 判定**：按 task 上的 `TASK_CREDIT_EN` 与 `TASK_PATH_ID` 查 `CreditCounter[path_id][stream_id]`。
   广播任务初值等于目的 core 数量，够就一次扣掉全部目的数，不够整体不发；P2P 任务初值为 1。
9. **重发**：`reissue` 有效但尚未重发成功时，该用户的原始数据不能被覆盖，用户也不能释放。
   重发 task 不在主线任务链上，可与主线并行。
10. **完成事件合流**：七路通道，用 `task_completion_src` 区分 RV / DSA / Router 三种来源。
    DTE ACK 只代表搬运完成；**reduce 任务只有 Router 的 `reduce_done` 才能置 FINISH**；两者可任意顺序到达。
    task 完成有 RV 收尾、DSA 收尾、两者都上报三种场景。
11. **任务生成与跳过**：按 core 角色跳过任务链里不需要的 task。
12. **异常检测**：异步任务长时间没等到 trigger、用户长时间未 retire、Router 请求携带的 `path_id`
    在 task_chain 里匹配不上。

**验收**：同一个 `task_id` 在不同 user 之间时序无交叠；乱序完成后顺序语义不被破坏；
`self_start` 链能持续自发创建；缺 `reduce_done` 时任务链停在 reduce 处不前进。

### 7　RV Core

本轮建**行为级**：能执行一段 kernel 的功能语义即可，不建双发射流水线、不建分支预测。

1. 接收 TS 下发的 `{task_pc, stream_id, user_id, task_id}`。
2. 按 `task_pc` 执行该角色对应的 kernel：算 Matrix Mem / Core Mem 地址、
   读写 Share Mem 里的软件映射表。
3. 发 DSA 任务配置指令：`dsawi` / `dsaw` 写 ADDR 与 TD 两组寄存器，**Doorbell 必须最后写**，
   写 Doorbell 时 PACK 寄存器自动写入。
4. 发完异步指令**立刻**向 TS 交还自己，不等 DSA 完成。
5. 纯标量 task 不调度 DSA，由 RV core 自己向 TS 报 `task_done`；
   `task_done` 指令要能带上 `task_id`（异步 datain 任务由软件识别包头后写入）。
6. `flag_check`：给起止地址，在 Share Mem 里找第一个置位并返回偏移量，找不到返回全 1。
   B core 与 R core 的映射表轮询靠它。
7. `dsa_iss` credit：DSA 任务队列 16 项，credit 为 0 时阻塞配置指令发射。

**验收**：多用户并发下 B core 的头尾指针、R core 的 `arrive_num` 不串；
`flag_check` 的结果与逐位扫描一致；Doorbell 未最后写时任务不应启动。

### 8　DTE DSA

只搬运不计算。

1. **五种搬运方向**：Router→MM、Router→CM、MM→Router、CM→Router、MM→CM。
   **CM→MM 不支持**，XBar 不提供这条连接。
2. 只支持**连续一维**搬运，不支持 stride。
3. **四 Lane 模型**：RD_CH0 / WR_CH0 / RD_CH1 / WR_CH1，一个高层任务由一对 RD/WR 子上下文组成。
   * 配对接纳：必须同时拿到目标 RD/WR TaskQueue 项与 Completion RS 项，任一侧无空间则整体不接。
   * 独立激活：任一 Lane 的上下文释放后即可激活本 Lane 的下一任务，不等配对 Lane。
   * 按任务 Join：同一 `task_id` 的 RD 与 WR 都满足才产生 `task_done`，且 **Exactly-once**。
4. **Task Descriptor** 的字段都要吃进去：`src_sel` / `dst_sel` / `queue_sel` /
   `cnt`（单任务最大 256B × 128 = 32 KB）/ `mask`（256B 一拍，按 32B 粒度）/ `path_id`。
5. **包头**：起点的 DTE 配置或 DPU 注入，中途不可修改。Hmem 存软件包头表与硬件 `core_mask_table`。
6. **R core 场景下搬运要原子化**：某个方向的用户数据全部搬进 Matrix Mem 后，
   才能搬别的方向或别的用户。这与普通 reduce 要求交织正好相反。

**验收**：搬运字节数与落点地址正确；`mask` 置 0 的 32B 确实没被写；
`task_done` 不重不漏；原子性被破坏时用例能检出。

### 9　MU DSA

矩阵乘，外加专家间的加权累加。

1. **两种运算形式**：`C = A × B`；`C = C + (A × B) × W_ep`。
   后者把 EP reduce 从 VU 挪进了 MU，**不支持初始 C 加载**。
2. **8 种计算原语**，覆盖四种精度组合：MXFP8×MXFP8（`1×K128×N64`、`1×K64×N128`）、
   BF16×BF16（`1×K64×N64`、`1×K32×N128`）、W4A8（`1×K256×N64`、`1×K128×N128`）、
   W4A16（`1×K128×N64`、`1×K64×N128`）。
3. 完整式子 `D = sf_A × sf_B × A × B + C`：权重 B 与其缩放因子来自 Matrix Mem，
   输入 A 与其缩放因子、累加值 C 来自 Core Mem，结果写回 Core Mem。
4. **vlane**：Load token 时只读 `256B / vlane_num` 字节再复制扩展到 256B；
   Store 时按 vlane 数决定横切拼装还是纵向整块。
5. 本轮不建 32 个物理 lane 的并行结构，只需数值结果正确。

**验收**：八种原语各跑一遍，与参考实现逐元素比对；
专家加权求和的结果与“先加权后求和”一致；不给初始 C 时结果正确。

### 10　VU DSA

向量算子，与 MU、DTE 最不一样的地方在于交互抽象是宏指令。

1. **宏指令模型**：一条宏指令 = 一组静态配置（计算图通路模板）+ 一组动态参数（本次的地址与索引）。
   写 `macro_inst_trigger` 后硬件锁存动态参数、与静态配置打包压入执行队列。
2. **8 组静态配置**，软件可随时改写；若该组正被未完成的宏指令引用，这次写要阻塞到引用它的宏指令退休，
   in-flight 宏指令按改写前的配置执行完毕。
3. **执行单元**：LU、SU、VALU0、VALU1、VALU2、VSFU、MEXE、SEXE，各自的指令能力见第 4 章。
   SEXE 物理上只有一组，SEXE0/1/2 是同一单元在一条宏指令内的三次串行迭代。
4. **单条宏指令的资源上限**要体现出来，它决定一个算子拆成几条：
   CM 1 Load + 1 Store、VRF 2R2W、MRF 2R1W、每个执行单元各 1 次。
5. **本轮至少实现这些算子**：SiLU·dot·量化、专家间 reduce、Softmax、Top-K、Vector ADD/MULT、Weighted SUM。
6. Scoreboard 追踪 VRF / MRF / SRF 的 RAW / WAR / WAW。
7. **CM 访存冲突硬件不追踪**，靠软件置 `MACRO_INST_FENCE`。

**验收**：各算子结果与参考实现一致；
**缺 FENCE 的用例必须能复现出错**，模型若默认挡住冲突就比硬件乐观了；
静态配置被 in-flight 宏指令引用时改写确实被阻塞。

### 11　Router

功能最密集、也最容易实现错的一个模块。

1. 按 `path_id` 查 RouterTable 得 `directionMask`：**Core 位有效就落一份进本核，其余方向位有效就继续转发**。
   广播的复制就是这样逐跳展开的，发起方只发一次。
2. **多播原子准入**：所有置位方向的资源都拿到才发，任一方向不足则所有分支统一等待，
   不允许分支各自先发。
3. **Stream 授权**：Router 是唯一有效状态（User Resource Allocation Table），进 core 必须先有 Stream 表项。
   DTE 持一份 cache，两者行为必须一致。
4. **两级流控**：Stream 资源按 UserID + 目标方向占一项，保证目标 core 的 Core Mem 有空间；
   VC Credit 按下游方向 + VC、flit 粒度；Reduce Credit 保证下游 ReduceModule 上下文有空间。
5. **传输粒度**：**进 core 后锁定到尾 flit**，router 到 router 可以在 flit 边界切换 Packet。
   两种粒度不能混用。
6. **ReduceModule**：`operation` 为 reduce 时走这条路，中间累加精度固定 FP32，
   输入输出精度按 `reducePrecision` 配置；上下文 16 用户 × 16 KiB。
   逐级 reduce 完成后由 Router 主动向 TS 发 `reduce_done`。
7. **阻塞处理**：资源不足时按 `stallWay` 决定留在当前 VC 等待，还是转入 Core Mem 由 DTE 重发。
8. **坏核透传**：坏核只做 router→router 转发与 credit 透传，
   不落核、不占坑、不登记 holders、不参与 reissue、不产生 token_trigger。
9. **用户退休**：Core 保证不再有该 user 的搬运 → Router 立即删表项 →
   ReduceModule 延迟到 credit 全恢复才删。
10. **监听事件队列** 16 项全相连：TS 注册事件，Router 查资源，满足后通知 TS 调度搬运。

**验收**：一次广播的落点集合等于目的集合且每个目的核只落一份；bypass 的中间核不占用户坑；
同用户二次发送不再扣 credit；单坏核与多坏核串联时上下游账本一致；
**跑完一轮 credit 回到初值**。

#### Router credit 的可实现规范（stream_slot 方案）

上面第 3、4 条只说了 credit 要保证什么。真正落到实现，credit 的账记在哪、谁扣谁还，按下面这套规则做。

##### 两条地基

1. **坑账模型**：下游 CoreMem 用户坑的 credit 记在**上游 Router** 的账本 `slot_ledger[dir]` 里，
   上游“能否向某方向发送”就是查它给该 dir 的坑账。**进核不为自己 acquire**，
   `cur_credit_require` 仅占位记账。
2. **fork 原子性**：`flow_dir` 方向固定不可改，多方向 fork **全发或全等**，所有置位方向坑都 acquire 成功才一起发，任一不足则整体 AbsorbReissue，**不做“能发先发”**（否则算子变动态）。

##### 核心规则

| 规则 | 内容 |
| - | - |
| 单位 | 1 credit = 1 用户/stream 坑；每用户 CM = `CoreMem / N_stream`（编译期静态） |
| Init MSG | 该核上某用户任务链的第一个 Message，初始化该用户 CM。**规则 α**：该核该 user 尚无 stream 时收到的第一包即为 Init（类型不限，含 Bypass-as-Init） |
| 两类门控 | **用户资源门控**（首次占坑，查 credit）与**发送就绪门控**（同 user 后续报文不再查 credit，只受链路反压）。**Init 覆盖后，后续 packet 不再单独查 credit** |
| 多方向 | 先按 `held(dir,user)` 过滤掉已持有的方向，**只对剩余未持有且 require>0 的方向做 credit 检查**；fork 原子性只作用于这个待申请子集 |
| 释放 | chain end 产生**一次**释放事件，对 `holders[dir]` 中该 user **逐方向** release，`dst` 由 `dir` 唯一确定；**禁止无方向信息的广播式 +1** |
| Release 字段 | `{user_id, dir, src_core, epoch}`；`epoch` = 该 user 在本核一次占坑生命周期的代次，上游只认当前代次、丢弃过期重复释放 |
| 账本唯一性 | 余额权威**仅在 Router**；TS 只存 `held[dir]` 与 reissue 派生标志，**禁止双写余额** |
| 记账分工 | 上游 `slot_ledger[dir]` 记“我朝下游某向占了几格”，下游 `stream_origin[user]` 记“我从哪个入方向被开的坑”，两边各一半、拓扑一对一闭合 |

##### 坏核 credit 透传

坏核只有 Router 可用（无 TS/DTE），**不能落 CM、不能阻塞重发、不能开坑**。因此坏核 Router 要实现 credit 透传，作为上下游健康核之间的桥梁：

* **数据面**：只按 Router Table 做 router→router 转发，不查 credit、不占坑、不登记 `holders`、不参与 reissue、不产生 `token_trigger`
* **控制面**：下游坑查询/授予、Release 归还**透明转发、不记账、不消费**，只做转发与时延建模

以 `CoreA → CoreB → CoreX(坏) → CoreC` 为例：

* **CoreB 的 `slot_ledger[dir]` 记的是 CoreC 的坑**，不是 CoreX 的。
* CoreC 的 `stream_origin[user]` 记的上游是 CoreB。

多坏核串联时**逐跳链式透传**，健康核两侧看到的始终是“对面第一个健康核”。

> 因此“入方向唯一确定上游核”这条规则要修正为：**入方向确定的是穿过坏核链后的第一个健康核，而非物理邻核**。

##### Reduce 完成 Ack 的归属

逐级 reduce 在 Router 内累加完成后，**由 Router 主动向 TS 发 `reduce_done(user_id, path_id)`**。
两侧的分工是：

* **Router**：负责发这个 Ack。
* **DTE DSA**：reduce 任务下只做资源释放，**不向 TS 返 Ack**。

缺该 Ack 时任务链应停在 reduce 处不前进，可作反向用例。

##### RouterTable 表项（比硬件 MAS 更具体的一版）

| 字段 | 宽度 | 含义 |
| - | - | - |
| `op` | 2 bit | `transfer` / `reduce` / `reduce_twice` / `kernel_move` |
| `flow_dir` | 4 bit | 出方向掩码（上/下/左/右），bit=0 表示无流；**进核判定单独走，不占此掩码** |
| `path_core_mask_enable` | 1 bit | 0 用 `path_core_bypass`，1 用 Message mask |
| `path_core_mask_index` | 4 bit | mask 位选择 |
| `path_core_bypass` | 1 bit | enable=0 时是否进核 |
| `cur_credit_type` / `cur_credit_require` | 1 / 6 bit | 进核占用 credit 池类型与额度（**占用记账，非向下游申请**） |
| `nxt_credit_type` / `nxt_credit_require` | 4 bit / 6bit×4 | 各出方向的池类型与需求；0 = 该方向不检查 credit |
| `reduce_data_type` | enum | BF16 / FP32 |

进核判定：

```
enter_core = table.path_core_mask_enable
           ? bit(msg.path_core_mask, table.path_core_mask_index)
           : not table.path_core_bypass
```

**V1 最小可用字段集**：credit 全由 Init MSG 控制、每次 ±1，`nxt_credit_require` 退化为 0/1 门控位。V1 实际参与逻辑的只有 `op` / `flow_dir` / `path_core_bypass` / `path_core_mask_enable` / `nxt_credit_require`。

##### Message 状态机

| 状态 | 含义 | 退出条件 |
| - | - | - |
| Idle | 可接受新头 | 输入有效 |
| Lookup | 读 Router Table + mask | 得到 op / 方向 / 进核 |
| DeliverLocal | 交付本地 / trigger TS | DTE 反压解除 |
| Forward | 写出到邻接端口（可多方向） | 链路缓冲接受 |
| ReduceAccum | 逐级 reduce 累加（≤3 输入） | 可发 |
| AbsorbReissue | 本核已吸收，等待下行条件 | TS+DTE reissue 再注入 |

四条不变量：① 不得把完整用户数据长期堵在 Router 上，可落本核 CM ② reissue 按最老用户优先 ③ 优先级 `reissue 广播 > 普通主链 DTE` ④ reissue 未完成前不得覆盖该用户 CM 暂存，也不得提前 Release 已占用的用户坑。

##### Rmem（Router 内 reduce 缓冲）

* 容量 `REDUCE_BUFFER_BYTES`，量级 512 KiB（= N × 32KB）
* **与 CoreMem 共用同一套 credit**（单账）：reduce 不另开 Init，Init 分配该 user 坑的同一 credit 即同时授权其 Rmem 空间
* Router 依 `userid → streamid` 映射把 packet 映射到对应 Rmem 地址；**非 reduce 用户不映射 Rmem 地址**
* 生命周期随 user：Init 开、chain end 一并释放
* **reduce 加法跳过软件辅助信息 16B**（非业务数据，不参与计算）
* 吞吐：`REDUCE_BUS_WIDTH=256 B/beat`、`REDUCE_ADD_BW=256 B/cycle`、`REDUCE_ADD_PORTS=1`；处理 N 字节有效 payload 耗时 `ceil(N / REDUCE_ADD_BW)`，多输入累加按读改写计，同地址冲突时串行化

### 12　三类 core 角色

同一套硬件，行为差别全部来自第 3 项生成的配置。建模时用同一个 core 类，配不同的 task_chain 与 RouterTable，
**不应为三种角色各建一个类**。

| 角色 | 配置差别 | 这个配置换来什么 | 功能验收 |
| - | - | - | - |
| 计算 core | 单条链，Router 触发 | 一个用户从收到发的完整处理流水；stream 项占用等于处理时长 | 一个 token 的 FFN 结果与参考实现一致 |
| B core | 两条链，链二 `self_start`；Share Mem 里一对头尾指针 | 在途用户数不再受 stream_table 的 16 项限制，改由 Matrix Mem 容量决定 | 链二被 credit 卡住时链一仍能继续收；指针回绕不出错 |
| R core | 两条链，链二 `self_start`；Share Mem 里 `arrive_num` 映射表 | **推进顺序改由软件表决定**，可以按“谁先集齐”而不是按到达顺序推进 | 两笔数据乱序到达时按集齐顺序取走；表项能乱序分配与释放 |

链二 `self_start` 是这两条的共同前提：它不等数据触发，主动去软件表里挑，
因此挑谁不受 TS 年龄优先的约束。建模时若把链二也做成数据驱动，R core 的乱序推进就实现不出来。

### 13　Chip 与 C2C 互联

1. 建 chip 内 core 阵列的 Router 邻接关系（2×4，Harvest 下 2×5）。
2. chip 间 C2C 本轮建成一条无损、保序的链路，功能上等价于一段更长的 router 到 router 通路。
3. **PCIe 两路 x16 之间不保序**，这一点影响功能：软件必须把数据拆成两路独立数据流，
   模型要能体现乱序到达，而不是假定保序。

**验收**：跨 chip 的广播与 reduce 落点正确；两路 x16 乱序到达时结果仍正确。

### 14　边界桩

1. **GPU / DPU**：按 microbatch 注入 token；注入前加 `gpu_id`（8 bit）与 `token_id`（16 bit）包头，
   per-GPU 自增计数器 256 项；实现两层 credit 的额度判定（本地 buffer 门控与 Bach 全局 grant），
   只建“够不够、放不放行”，不建其时序。
2. **PCIe Switch**：组播时把一份数据复制到多个收端；反压按最慢收端聚合，
   各收端的 `retired_token` 取 min 后再向 GPU 回报。
3. **出口**：收下结果，按 `(gpu_id, token_id)` 与参考实现比对。

**验收**：注入 N 个 token 后出口收到 N 个结果且全部正确；组播的每个收端都拿到完整副本；
序号回绕时比较仍正确（一律用模 2^W 差值）。

## 建模顺序

每个阶段结束时都应有一个能跑、能验收的东西，而不是等到最后才第一次跑通。

| 阶段 | 建什么 | 跑通的标志 |
| - | - | - |
| 0 | 模块 1～4：静态配置生成器与参考实现 | 给定切分参数能生成完整的角色、分片、RouterTable、task_chain；参考实现能算出一层的正确结果 |
| 1 | 模块 5～10：单个 core 的功能闭环，Router 用直连桩代替 | 一个 token 进单 core，走完任务链，结果与参考实现一致 |
| 2 | 模块 11：Router 与 core 间互联，扩到 chip 内 2×4 | 单播、多播、逐级 reduce 三种 path 落点正确，credit 守恒 |
| 3 | 模块 12：三类 core 角色 | B core 在下游阻塞时缓冲能涨落；R core 能乱序对齐；两者都不死锁 |
| 4 | 模块 13～14：多 chip、EP 组间 reduction 链、边界桩 | EP6+TP8 与 EP1+PP3+TP16 两种切分下，整层结果与参考实现一致 |

阶段 1 的关键取舍：**Router 先用直连桩**，即 DTE 发出的搬运直接送到目的 core，不查 credit、不排队。
这样能把 TS 与三条执行链的功能先验干净，再把 Router 换进来时，出的问题一定在 Router 侧。

## 验收清单

本轮的验收分三类，每一类都要有能自动跑的检查，跑完一轮全部通过才算这一阶段结束。原始验收清单里另有时序行为与性能指标两类，依赖时序，留到性能模型那一轮，见本章末尾。

| 类别 | 检查项举例 |
| - | - |
| 拓扑结构 | core 数量 = `chip_rows × chip_cols × core_rows × core_cols`；芯片间连接与 `chip_rules` 一致；Host/Out target 坐标不越界 |
| 数据流正确性 | **Credit 守恒**（初始 + 归还 = 消费 + 余额，无泄漏）；Beat 组装正确；MoE BitMap 正确传递；MU/VU 计算次数与预期一致；每 core 的 user init 数量符合预期 |
| 边界与异常 | Stream 耗尽正确排队；Credit 耗尽正确阻塞上游；FIFO 满正确背压；看门狗超时触发；无效 Opcode / 重复 User ID 被拒 |

除上表外，下面四条不变量在每个阶段都要查，任何一条被破坏都说明模型有结构性错误：

1. **credit 守恒**：一轮跑完后，所有方向、所有类型的 credit 计数回到初值。
2. **相同 `task_id` 在不同 user 之间时序无交叠**。
3. **每个目的 core 对同一个 user 的同一份数据只落一次**。
4. **注入 N 个 token，出口收到 N 个结果**，且逐元素等于参考实现。

Router 的验收场景 A1～A17 中，下面四个直接覆盖了最易实现错的语义，先跑这四个：

| 场景 | 查什么 |
| - | - |
| A2 | bypass 中间核不占用户坑 |
| A5 | 同用户二次发送余额不减 |
| A15 / A16 | 单坏核与多坏核串联时的 credit 透传 |
| A17 | reduce 完成 Ack 归 Router；缺 Ack 时任务链停在 reduce 处不前进 |

## 功能上最容易实现错的几条

1. **Reduce 任务的完成判定**：DTE ACK 只代表搬运完成，**只有 Router Reduce Done 才能把任务置 FINISH**，且两者可任意顺序到达。判错会让 reduce 链提前推进。
2. **异步 datain 只更新 done_bitmap，不推进 task_id**。这是任务链能乱序完成又保持顺序语义的关键。
3. **Retire 的三方时序**：Core 保证不再有该 user 的搬运 → Router 立即删表项 → ReduceModule 延迟到 credit 全恢复才删。
4. **多播原子准入**：多播的每个 flit 必须同时取得所有目标方向资源才能发，任一方向不足则所有分支统一等待。不能让分支独立前进。
5. **进 core 后锁定到尾 flit**，而 Router-to-Router 可以在 flit 边界切换 Packet。两种粒度不能混用。
6. **Mmem 同 bank 不能有两个 master**，冲突时只执行 MU。
7. **VU 的 CM 访存冲突硬件不追踪**，靠软件的 `MACRO_INST_FENCE`。建模时若默认硬件会挡，结果会偏乐观。

## 本轮明确不做的部分

下面这些全部依赖时序，留到性能模型那一轮。**现在把它们建进来，模块边界一变就得重做。**

* 第 5 章的全部延迟参数：core 内各通路的拍数、DSA 启动延迟、访存延迟、C2C 与 R2R 延迟
* 带宽与仲裁：各通路的 B/T、bank 冲突时的仲裁顺序、多 master 的优先级
* 流水与重叠：多用户在三条执行链上的重叠、宏指令之间的重叠、MU 的执行流水
* 第 5 章的解析延迟与吞吐模型，以及与它对拍
* 功耗特性
* 验收清单里依赖时序的两类：
  * **时序行为**：各 setup 时间等于配置值，NoC 单跳 = `wire_delay + access_delay`，
    跨芯片 = `CROSS_CHIP_DELAY`，以及 task 之间的时序关系
  * **性能指标**：吞吐量在合理范围，单 user 端到端延迟符合理论下界，各单元占用率不超过 100%

下面这些是原始设计文档里写明“需要建模 / 仿真评估”的点，它们全部依赖时序，同样留到性能模型那一轮作为验收目标：

* **DTE 的 3 Lane 必要性**：假定 Matrix Mem、CoreMem、Router 各是一个读写 Resource，3 Lane 理论上更好，但 XBar 面积正比于 N_in × N_out × W。需要定量评估收益。
* **Broadcast 过快引起的空泡**：EP 内 LPU 多播 + PPTP 切分时，若第 N 个用户在 core2 和 core4 的
  处理速度不同，第 N+1 个用户的广播会在物理通路上排队，导致计算空泡。原文档的判断是“一般在一个
  TP 组内处理时间差距不会很大，时间差主要来自逐级 Reduce”，并写明**需要模拟器介入协助确认**。
* **Core 负载不均衡是否会被多用户掩盖**：纯 TP 和 PP+TP 方式都存在 core 负载不均衡导致单用户 TPOT 延长，需要分析多用户之间能否掩盖，否则整体吞吐降低。
* **切分策略的最优点**：《建模参数与性能模型》“单用户总延时”那条有极小值的曲线，对每个目标模型求解。
* **Core Mem 容量与 stream_num 的取值**：16 用户 × 单用户 30～50KB 是否够，取决于实际切分方案。
* **多用户 T_core_delay 到底是多少**：文档写“50T？”带问号，这个数直接决定多用户吞吐公式的分母。
