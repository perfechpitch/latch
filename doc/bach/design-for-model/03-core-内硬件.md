# 第 3 章　Core 内硬件

**模式**：design（陈述当前设计，动机与取舍收在各节的“取舍”段落里）

定义 Router、Bach Core 顶层、TS 任务调度器、RV Core 四个模块的结构与行为。
这四者构成 core 内的控制通路，决定多用户如何在三条执行链上流水，是性能建模要精确复现的部分。

《Bach 硬件设计建模参考》第 3 章，全套目录见 [README](README.md)。

***

## Router：片上交换与归约中心

Router 是 chip 内 2×4 core 阵列的数据交换与**片上归约**中心，物理上位于 chip 中部，同时承担三件事：

* 包的路由转发
* Stream 与 VC 两级流控
* Reduce 计算

### 内部组成

| 模块 | 职责 |
| - | - |
| **RouterStation**（×4，对应上下左右） | 收发与相邻 Router 通信的 Packet，按包头查本级路由信息参与仲裁；持有多个 VC 缓存；输出通路可把多个 Packet 按总线宽度移位拼接 |
| **CoreStation**（×1） | 收发与本 core 通信的 Packet，**不设 VC 缓存**；进 core 通路把拼接的 Packet 恢复，出 core 通路按包头查路由参与仲裁 |
| **ReduceModule** | 接收 Reduce 输入、做归约计算、发送结果，也按包头查路由参与仲裁 |
| **CoreMemCreditMonitor** | 维护各方向 CoreMemCredit，支持对 credit 需求的监听，条件满足后通知 TS；credit 信息在 Router 内广播参与仲裁 |
| **InterconnectMatrix**（Xbar） | 五方向仲裁与转发，内部维护下游所有方向的 credit |
| **CSR & perf & debug** | CSR 寄存器、性能数据、debug 信息的统一寄存器界面，可经 AXI 读写 |

### 性能与容量指标

| 指标 | 值 |
| - | - |
| 每方向数据宽度 | 256 B |
| 相邻 Router 双向各 | 256 GB/s @1GHz |
| Reduce 输入 | 三路各 160 GB/s |
| Reduce 输出 | 160 GB/s |
| Reduce 算力 | 80 GFLOPS（FP32/BF16） |
| ReduceModule 上下文 | 16 用户 × 16 KiB |
| VC | 每输入方向 4 类；输出方向不设 VC Buffer |

* 进 core 与出 core 数据通路**完全并行**，各支持 256 GB/s @1GHz
* VC Buffer 深度需覆盖对应路径的往返延迟，并满足该 VC 的峰值带宽需求
* 五个输入的目标输出方向互不冲突时，**Xbar 必须支持五路输入同周期并行传输**，不得做不必要的串行化

### RouterTable

Packet 的路径解析与资源判定全靠这张表。Router Station 以 Header 中的 `PathID` 为索引查本地副本。**RouterTable 只描述静态路由与资源需求，不保存 Packet 的动态执行状态。**

| 字段 | 含义 | 使用位置 |
| - | - | - |
| `PathID` | 表项索引，标识一条软件预先规划的业务路径 | Header Parser、重发查询 |
| `curVC` | Packet 进入当前 Router 时使用的 VC 类型 | 输入 VC 分配 |
| `directionMask` | 上下左右 + Core 五个目标方向的有效位；单位有效=单播，多位有效=多播 | 输出仲裁、Xbar |
| `nxtVC` | 各目标方向下一跳使用的 VC 类型 | 输出 Header、下游 VC credit 查询 |
| `streamNeedMask` | 各目标方向是否需要 Stream 授权；**Core 方向的需求必须在本级检查** | Stream Resource Table |
| `operation` | 普通转发 or Reduce 操作类型，选择 Bypass / Core / ReduceModule 路径 | 路径选择、ReduceModule |
| `stallWay` | 资源不足时：留在当前 VC 等待，还是转入 CoreMem 暂存由 DTE 重发 | 阻塞处理 |
| `reducePrecision` | Reduce 输入 / 输出精度配置；**中间累加精度固定 FP32** | ReduceModule |

> **三份副本的一致性是建模必须体现的约束**
>
> * Router 内部：所有需要并行查询的位置各持一份副本，由 Router 的配置入口统一接收写事务。更新状态机把同一笔写依次写入全部副本并记录完成状态，**全部副本写完才向软件返回完成**（原子提交，禁止暴露部分新部分旧的状态）。
> * DTE 与 ReduceModule 各自维护自己的 RouterTable，**由软件负责写入相同配置并保证三方一致，Router 硬件不同步外部副本**。软件只能在 Router 提交完成后再写 DTE 和 ReduceModule。

### 两级流控：Stream 与 VC Credit

这是 Router 最核心的机制，两级职责完全分开：

| <br /> | Stream 资源 | VC Credit | Reduce Credit |
| - | - | - | - |
| 粒度 | 按 UserID + 目标方向的一个表项 | 按下游方向 + VC，flit 粒度 | flit 粒度，按 UserID |
| 保证什么 | 目标 Core 的 CM 有空间容纳该用户的数据 | 下游 VC Buffer 有空间 | 下游 ReduceModule 上下文有空间 |
| 谁维护 | **Router 是唯一有效状态**（User Resource Allocation Table）；DTE 持一份 cache（User Resource Cache Table） | 每个下游方向的每个 VC 一个独立 Credit Counter | **DTE 维护本级；ReduceModule 维护相邻下游各方向；Router 不维护** |
| 怎么释放 | 下游或 Core 通过携带 UserID 的 release 通道通知 Router 回收表项 | flit 离开下游 VC 后经独立 release 通道返还 | 输出 flit 被下游接受后产生携带 UserID 的 release |

#### Stream 授权的关键规则

* 只有 **Router 负责真正申请表项**；DTE 要发数据必须先从 Router 获得指定 user 的授权，禁止超额分配或重复授权。
* Router 的进 core 表和 TS 内部的 Stream 资源表**按完全一致的逻辑申请空项**，因此分配不会多于实际资源数量，这保证“Router 通知 TS 的 Packet 一定能被 TS 接收”。
* DTE 内也要维护一份下游 Stream 资源表，Router 为了快速 Bypass 也维护一份下游映射表，**这两个表的行为必须保持一致**。

#### 业务 Credit 的静态 Bypass

Stream Credit 和 Reduce Credit 的 release 走一条特殊路径：

* 软件通过 CSR 为每个业务 Credit 输入端口配置**静态输出方向 Mask**
* 转发时**不查询 Packet RouterTable**，也不做动态路径选择
* Mask 含多个方向时，同一笔 release 复制到所有指定方向，UserID 与 Credit 类型保持不变

### Packet 传输的四条路径

#### (a) Router → Router（Bypass）

1. Packet 经数据总线传到下一级 Router，Router 自动检测包头，按 `PathID` 查到路由信息和资源需求
2. 数据总线每个 flit 携带 VC 通道号，flit 到达后自动找到对应 VC 存放位置
3. 输入方向的 RouterStation 维护所有下游方向的 Stream 资源映射表，**只有所有需求方向都满足才允许发送**
4. VC 间用 Credit 机制，按 flit 粒度传输，申请到下游 credit 才能发 flit

> **交织规则**：Router-to-Router 通路**允许在 flit 边界切换 Packet**，需保存 VC、输出方向、剩余长度和包边界上下文；但 Packet 一旦开始进入 Core 或 ReduceModule 就**锁定到尾 flit**。这条区分直接决定建模时 buffer 的组织方式。

#### (b) 进 Core

1. 进 Core 对 Router 而言也是一个输出方向，需维护本级 Core 的 Stream 资源
2. 因为已通过 Stream 检查，**不再检查对 Core 的 VC credit**，一定有 CM 空间
3. 按 PathID 查到需进 Core 后，检查本级 Stream 资源表，按三种结果分别处理：
   * **已分配**：包头进 HeaderFIFO、数据进 OutputBuffer
   * **未分配但有空项**：记录 UserID 占用
   * **无空项**：该 VC 不能发数据到 Core，但 VC 有空项时仍可接收数据
4. CoreStation 按接收包头的顺序通知 TS 调度 DTE 搬运：
   * DTE Core 用 **AXI-Full 类接口**读包头生成 DTE 任务，读完向指定地址写 1 把包头弹出，CoreStation 映射出下一个包头
   * CoreStation 与 DTE 之间用 **AXI-Stream-Like** 协议传数据（Header+Payload）
   * **Core 内输入不支持多 Packet 交织**，Router 必须保证发完一个整包再发下一个

#### (c) 出 Core

1. 由 DataOut DTE 发起。DTE 内有（Router 一个方向的 VC 数个）Buffer，某 VC 阻塞只阻塞 DTE 中对应 Buffer，不影响其他
2. DTE 发数据到 Router 时与 CoreStation 有 Credit 协议，保证 VC 有容量才发
3. 若 Packet 对下游 Stream / Rmem 资源有需求，DTE 必须先申请到才能发，否则任务在 `PendingTaskQ` 等待
4. DTE 中需有一份 RouterTable，按 PathID 查到 VC 和资源需求

#### (d) Packet 重发（CoreMem 暂存）

* 下游资源不满足时，Router 可把 Packet 重定向到 CoreMem 缓存，等资源就绪后重发。此时 **Router 上的 Bypass 操作被映射成“进 core + 出 core”**
* CoreMem 中**只保存 Packet**，包头含 UserID、PathID、size；重发时用 PathID 重新查 RouterTable，不重复保存 VC 和路由信息
* **同 VC 保序**：同一 VC 存在未完成的 CoreMem 重发 Packet 时，后续 Packet 不得越过
* 无论直接发送还是经 CoreMem 重发，完成后都向 Core 内 TS 返回至少含 UserID + PathID 的完成信息

### ReduceModule

| 组件 | 职责 |
| - | - |
| User Context Table | 记录 UserID、当前 Packet 状态、输入完成情况、输出状态与 Retire 状态 |
| Reduce Context SRAM | 16 用户 × 16 KiB，保存当前 Packet 的 FP32 中间累加结果 |
| RMW Pipeline | 首份输入建立上下文，后续方向输入执行 Read-Modify-Write **原位**累加 |
| Precision Convert | BF16 输入扩展为 FP32；输出按 RouterTable 配置转 FP32 或 BF16 |
| Downstream Reduce Credit Map | 按 UserID + 目标方向维护相邻下游 Reduce Credit，逐 flit 扣减、按 release 恢复 |
| Input/Output Arbiter | 仲裁最多三路输入的 SRAM / Bank / 计算资源 |

关键约束：

* **上下文保护**：当前 Packet 的全部输入完成并输出前，同一 User 的下一 Packet 不得覆盖该上下文
* **必须执行 Reduce**：SRAM / Bank / 计算单元暂不可用时对输入反压，**不允许绕过 Reduce 降级为直接存储或转发**
* 精度：输入 FP32 / BF16，BF16 转 FP32 后参与计算，中间累加统一 FP32，输出可配 FP32 或 BF16

### 用户退休（Retire）

资源回收的时序契约，建模时是一个明确的状态机：

1. ReduceModule 完成计算并发出全部 Packet 后向 Core 返回 UserID
2. Core 判定任务链结束后**向 Router 和 ReduceModule 广播 User Retire**
3. **Core 的保证**：仅可在该 UserID 的全部进 core、出 core 数据搬运完成、且不会再发起新搬运后发 Retire。Retire 发出后，Router 上不得再出现以该 Core 为源或目标的该用户 Packet
4. **Router 的动作**：收到 Retire 后停止该 UserID 的新发送，删除其全部 Stream 资源授权表项
5. **ReduceModule 的动作**：**延迟回收**，先记录 Retire，待相邻下游各方向 Reduce Credit 全部恢复到初始值后才删除对应用户映射

### 数据包监听机制

Core 对外发数据要同时满足 VC 资源与 Stream 资源。监听这两项资源的职责在 **Router**，
一次监听走三步：TS 发送注册事件 → Router 查资源 → 满足后通知 TS 调度搬运任务。该功能在 DTE 中实现。

> **取舍**：若改由 TS 监听，大量通信信息要塞进 TS 任务链，计算与通信不再解耦。

* 监听事件队列：**16 项全相连**，可同时监听多笔多方向的资源申请
* 多个事件同时满足时按 StreamID 仲裁，选最老的任务通知 TS
* 进 core 重发的任务也注册到该队列，数据进 Core、资源就绪后通知 TS 重发
* 同一 VC 的数据包要保序，当前 VC 有未重发完的数据时后续包不能提前发送

### 传输粒度

Router **按 Packet 粒度仲裁，不允许 interleave**。总缓存 72KB × 3 = 216KB。

> **取舍**：整包粒度的功能与时序都更简单，只需对包头仲裁，验证复杂度低；token 天然很大，8KB 能传 32 拍，不靠多流 interleave 也能把 R2R 利用率喂满。代价是长包阻塞可能引入死锁场景，须在架构层保证不出现。

两个方案的对比：

| <br /> | Packet Interleave | 整包粒度（选定） |
| - | - | - |
| 容量 | 32KB×3 + intf 16×4 = 160KB | 8+32+16+16KB = 72KB ×3 = 216KB |
| 设计 | 更贴近标准 NoC，设计人员认为更好收敛 | 功能和时序简单，验证复杂度低，只需对包头仲裁 |
| 适配性 | R2R 利用率不够时扩展粒度更一致 | 适合大数据流，token 天然很大，8KB 能传 32T，不需要多流拼 interleave 提升利用率 |
| 风险 | — | **长包阻塞可能有死锁场景，需要在架构层保证不出现** |

配图：[Router 12 张](<../../../../perfechpitch/Bach/04_四、MAS（Micro Architecture SPEC）/04_Router>)

| 编号 | 内容 |
| - | - |
| `01` | Block Diagram（5×7 CrossBar / Router Station / Interconnect Matrix / Stream Resource Map） |
| `02` | Packet 在 Router 上传输 |
| `03` | 进 core 机制 |
| `04` | 出 core 机制 |
| `05` | Core 出 Reduce |
| `06` | ReduceModule 之间 |
| `07~12` | RouterTable / CSR / 各 Station 与 Xbar 细节 |

另有 [Router OLD 1 张](<../../../../perfechpitch/Bach/04_四、MAS（Micro Architecture SPEC）/05_Router OLD>)。

内嵌表格：

* [通信机制 6 子表](../../../../perfechpitch/_sheets/_II1Rs6)（每个 path_id 在各 core 上的进出方向（✅ 落核 / ➡️ ⬇️ 转发）：broadcast、reduce、p2p、EP 多播各一张；`N8MvQ8` 是 path_id + path_core_mask 的编码对照）
* [通信机制分析过程 52 子表](../../../../perfechpitch/_sheets/_BTlDsC)（36 张同构的 path_id 路径表覆盖各切分场景，另有 message 动态字段、logic op 说明、合并前后对照）

来源：`04_四、MAS/04_Router.md`（前 400 行为有效内容，Programming Model 之后为 eFUSE 模板残留）

***

## Bach Core 顶层

### 组成

```
┌─────────────────────────────────────────────────────────────────────┐
│ Bach Core                                                           │
│  ┌──────────┐              ┌──────────────────┐                     │
│  │  Router  │──────────────│ TS 任务调度器    │                     │
│  │ 256B/T×5 │              │ chain 64/stream16│                     │
│  └────┬─────┘              └────────┬─────────┘                     │
│       │                             │                               │
│       │      ┌──────────┬───────────┼───────────┐    ┌────────────┐ │
│       │      │DTE RVCore│ MU RVCore │ VU RVCore │────│ Share Mem  │ │
│       │      │ITCM 4KB  │ ITCM 4KB  │ ITCM 4KB  │    │   32KB     │ │
│       │      │DTCM 8KB  │ DTCM 8KB  │ DTCM 8KB  │    └────────────┘ │
│       │      └────┬─────┴─────┬─────┴─────┬─────┘    ┌────────────┐ │
│       │           │           │           │          │Core Monitor│ │
│       └──────│ DTE DSA  │  MU DSA   │  VU DSA   │    │IPI/状态/clk│ │
│              │2ch/4lane │ 8K MAC    │1024bit/T  │    └────────────┘ │
│              └────┬─────┴────┬──────┴────┬──────┘    ┌────────────┐ │
│                   │          │           │           │Debug Module│ │
│              ┌────▼──────────▼───────────▼────┐      └────────────┘ │
│              │   DTE Xbar (DMA_XBAR)          │                     │
│              └────┬────────────────────┬──────┘                     │
│         ┌─────────▼────────┐  ┌────────▼──────────┐                 │
│         │ Core Mem 1MB+32KB│  │Matrix Mem 32+4MB  │                 │
│         │(1KB+32B)/T·8bank │  │ (8+1KB)/T·64bank  │                 │
│         └──────────────────┘  └───────────────────┘                 │
└─────────────────────────────────────────────────────────────────────┘
```

| 模块 | 功能 |
| - | - |
| `core_noc` | SCP 控制通路访问 Bach core 全局的路由模块 |
| `TS` | Bach Core 的控制单元，负责用户以及用户间在 core 内多任务（DTE/MU/VU）的调度 |
| `Router` | 数据通路的中转站，接收不同方向数据并路由到不同方向输出 |
| `MU/VU/DTE RV Core` | 接收 TS 调度，给对应 DSA 下发任务。ITCM 存 firmware / kernel / bootloader，DTCM 存初始化数据、BSS 段 |
| `MU DSA` | token 与 weights 的 GEMV 运算 |
| `VU DSA` | SiLU & dot、tp-reduce、ep-reduce、concat 等向量运算 |
| `DTE DSA` | DMA 搬移任务 |
| `Matrix Mem` | ① weight 存放 ② 充当 Broadcast core 缓存 ③ 充当 Reduction core 缓存 |
| `Core Mem` | ① token 存放位置 ② MU 计算结果 ③ VU 计算结果 |
| `Share Mem` | 3 个 RV core 的共享 mem，存 task 间共享数据 |
| `DTE Xbar` | DTE 搬移数据的 xbar |
| `Core Monitor` | core 内部状态的影子寄存器：IPI 模块（异常中断信息）、各模块状态、clk/rst 控制（单独模块） |
| `Debug Module` | 解析 DMI 操作，实现对 core 内组件的 debug：core_ctrl（3 个 RV core / MU / VU / DTE 的 halt/resume/reset/halt_on_reset）、abstract_cmd、SBA 访问 |

### 对外通道

| channel | 源 → 目的 | 内容 | 带宽 |
| - | - | - | - |
| `scp_ctrl_ch` | SCP → ctrl_noc | SCP 访问 core 内资源的控制总线 | 32 bit/T（待定） |
| `async_int_ch` | core_status → SCP | core 返回给 SCP 的中断信息 | — |
| `data_L_ch` | PCIe / router → router | 数据通道：weight、kernel、token、中间结果 | 256 B/T |
| `data_UD_ch` | router → router | 同上 | 256 B/T |
| `data_R_ch` | PCIe / router → router | 同上 | 256 B/T |
| `dmi_ch` | DTM → debug_module | DM 与 DTM 间传输数据和命令 | APB? 32 bit/T（待定） |

Bach core 对外有三个方向的数据通道：左右两边可以是 chip 间 PCIe 传输，也可以是 bach core 之间经 router 的传输；上下方向为同一个通道，来自 bach core 的 router。

### core 内部通路带宽

| 通路 | 带宽 |
| - | - |
| core_noc | 32 bit/T |
| router / PCIe ↔ router | 256 B/T |
| MU DSA ← Matrix Mem | 8 KB/T |
| MU DSA ↔ Core Mem | 512 B/T 或 1 KB/T |
| VU DSA ↔ Core Mem | 512 B/T 或 1 KB/T |
| router ↔ DTE DSA、DTE Xbar ↔ Core/Matrix Mem | 原文空缺，按 DTE 文档为 256B+8B ×2（Cmem）、256B ×2（router） |

> **VU 不能直接读 Matrix Mem**；需要支持 Matrix Mem → Core Mem 的搬移。

### Core Mem 的硬件多用户管理

这是 Bach 支持多用户并发的基础机制：**无页表、硬件地址映射 + 软硬件分层管理**。

映射必须由硬件做，理由是多用户复用同一套 kernel 代码：软件只能用统一固定的虚拟偏移地址，
没法为每个用户单独改地址、单独编译。纯软件管理会让相同虚拟地址落到同一块物理内存，多用户互相覆盖。
因此“**相同虚拟地址 ⇒ 不同物理地址**”这层动态映射只能交给硬件。

1. **软件配置并发规格**：按 Core Mem 总容量与单用户所需空间，配置 TS 的最大并发用户数 `stream_num`（1～16）
2. **硬件全局分片**：按 `stream_num` 均等切分 Core Mem，单用户独占空间 = 总容量 / stream_num
3. **用户接入绑定**：TS 收到新用户任务后硬件自动分配唯一 `stream_id`，绑定一份独立物理分片
4. **软件分片内偏移使用**：软件用统一偏移地址读写，硬件自动路由到当前用户的物理分片
5. **硬件自动回收**：用户任务结束后硬件自动释放对应 stream_id 的分片

Matrix Mem 不需要这套机制，每个用户看到的是相同的权重。

### 内存结构与容量

| 存储 | 容量 | 带宽 | 内容 |
| - | - | - | - |
| DTE core ITCM | <mde-comment id="jzpft2">4 KB</mde-comment> | 8B/T，延迟 1T | ① 初始化代码（用于后续 Kernel/Weight 搬运）② kernel ③ Firmware |
| MU core ITCM | 4 KB | 同上 | kernel、Firmware |
| VU core ITCM | 4 KB | 同上 | kernel、Firmware |
| DTE / MU / VU core DTCM | 8 KB | 32bit × 4 bank | 初始化 BSS 数据段、寄存器溢出与堆栈 |
| Matrix mem | 32 MB（+4MB scale） | 8 KB/clk（+1KB scale） | weight |
| Core mem | 512 KB / 1 MB（+32KB scale） | 512 B/clk 或 1 KB/clk | ① 业务流 token：message + data ② MU 计算结果 ③ VU 计算结果 |
| Share mem | 32 KB | — | 共享的 message 信息、标量数据 |
| IO reg | — | — | Router 路由表、TS task chain、DTE/MU/VU DSA 配置寄存器、Core status |

#### <mde-comment id="agp9rj">Share Mem 的必要性</mde-comment>

Core Mem 容量大、物理距离远，访问延时 15～25 拍，顺序执行的 RV core 掩盖不了。
因此单独做一块容量小、物理距离近、延时 5～10 拍的 SRAM，作为三个 RV core 的共享标量存储，
用来加速 DTE / MU / VU 的 task 之间传数据。**它不需要初始化，只存 task 间的共享数据。**

Core mem → Share mem 的 message 交互采用“**初始跟随数据搬移到 core mem，再由第一个 RV core
拿到 share mem**”的方案，另一个候选是初始化时直接进 share mem。

### 异常与中断

特权级设计极简：**仅支持 M 态**（MAS_TOP 里写“仅支持 U 态”，与 RV Core MAS 的“支持 M 态、不支持 S/U/H”矛盾，见《文档缺口与 TBD》），**不实现 MMU**，中断异常上报给 SCP 处理。

| 模块 | 异常 | 影响 |
| - | - | - |
| RV core | 非法指令、地址错误 | 通过中断上报 SCP，<mde-comment id="4h38ow">RV core 进 firmware 保存现场</mde-comment>，等待 SCP 处理；期间不接受 TS 调度、不下发新的 DSA 任务 |
| MU / VU / DTE DSA | 原文空缺 | DSA 各自文档有定义（见《执行单元与存储》） |
| Core / Matrix / Share mem | 原文空缺 | ECC 相关见《执行单元与存储》的“存储子系统” |

### 内存一致性与同步

Bach 用**任务隔离**代替显式一致性维护，建模时这一块可以大幅简化：

1. 不同 token 用户之间有独立地址空间，不存在数据共享
2. MU、VU、DTE 的 RV core **在任意时刻不会执行同一个用户的 task**，不会访问同一地址空间，不需要维护一致性
3. 同一用户在任务链不同 task 之间共享数据，默认通过 TS 隔离不同步骤任务来解决：
   * 同一用户的 task 按任务链顺序执行，前一个完成后才下发后一个
   * task 间通过 share mem 传数据 / context，前一个 task 的 RV core 写完 share mem 后，用 `fence + task 完成通知 TS` 的方式隔离两个 task 的数据相关

生产者与消费者按下面的顺序配对（每一步是一个 Release / Acquire 对）：

| # | 生产者写什么 | 配对的事件 |
| - | - | - |
| 1 | Router 写内部 Buffer | Release + Data Ready |
| 2 | RV 写 DMA Command | Release + Doorbell |
| 3 | DMA 写目标 Memory | Release + DMA Task Done |
| 4 | MU / VU 写 Task 输出 | Release + Task Done |
| 5 | DSA 写输出 | Release + Chain Done |

配图：[Bach_core_MAS_TOP 3 张画板](<../../../../perfechpitch/Bach/04_四、MAS（Micro Architecture SPEC）/03_Bach_core_MAS_TOP（pending）>)（core 内部连接图、地址空间分配、DTE RV core 调度 DTE）+ 2 张内嵌绘图（`d01` core 外部连接图、`d02` 内存管理流程）

内嵌表格：[Bach_core_MAS_TOP 7 子表](../../../../perfechpitch/_sheets/_JUGvs3)（Task 类型与对应资源 / 典型操作 / 切分边界、模型各阶段的 RV 配置内容与 DSA 计算、三个平面的分工、约束定义、三张地址结构说明）

来源：`04_四、MAS/03_Bach_core_MAS_TOP（pending）.md`、`07_RV Core.md`、`06_第四阶段/04_core内调度机制.md`

***

## TS 任务调度器

TS 决定多用户如何在 DTE / MU / VU 三条执行链上流水。它是**基于任务链的硬化调度器**，调度延时 2～3 cycle。

> **取舍**：不同场景下 core 内任务流固定、任务类型不多，硬化调度器足够覆盖，因此不做软件调度。

### 两个核心数据结构

#### task_chain（任务链）

最多 **64 个 task**，软件初始化时通过 `core_noc` 配置。每个表项 64 bit：

| 位域 | 名称 | 含义 |
| - | - | - |
| 31:0 | `TASK_PC` | 任务执行的起始 PC |
| 33:32 | `TASK_SEND_UNIT` | 执行单元：00=DTE，01=MU，10=VU |
| 35:34 | `TASK_RECV_UNIT` | 完成类型：00=只调 RV core 不调 DSA；01=调 DSA；10=调 DTE DSA + Router 上的 Rmem 模块 |
| 36 | `SELF_START` | 自启动（仅 B core 才会有） |
| 39 | `WAIT_WAKE` | 需要外部数据唤醒 |
| 40 | `TASK_Broadcast_REISSUE` | 标识该 task 是 Broadcast Reissue 任务 |
| 41 | `TASK_P2P_REISSUE` | 标识该 task 是 P2P Reissue 任务 |
| 42 | `TASK_REDUCE` | 逐级 reduce 任务 |
| 43 | `TASK_CREDIT_EN` | 是否需要 credit 才可发射（core 内 DTE 从 MM 搬到 CM 就不需要） |
| 44 | `TASK_EXE_MASK` | 按 user 区分执行 / 不区分 |
| 50:45 | `TASK_PATH_ID` | 任务匹配的 path_id，用于 TS 判断该任务的 credit 条件 |
| 51 | `TASK_END` | 任务结束标识 |
| 63 | `TASK_VALID` | 软件配置后有效 |

另有一个独立的 `DATAIN_TASK` 寄存器，只有 1 项且只能绑定 DTE，四个字段：`TASK_PC`、
`TASK_UNIT`（固定 DTE）、`WEIGHTS_MODE`（是否处于 weights 加载阶段，此时不启动 task_chain）、`TASK_VALID`。

其余配置寄存器：`STREAM_NUM`（1～16）、`TS_INIT_FINISH`、`TS_STATE`、`CORE_TYPE`（B core / R core / 普通 core）。

#### stream_table（多用户调度队列）

**16 项顺序 FIFO**，每项对应一条完整用户业务流：

| 字段 | 说明 |
| - | - |
| `valid` | 槽位有效，建表时置位、用户退出时清除 |
| `user_id` | 用户全局 ID，跟随 router 请求写入 |
| `task_id` | 当前执行的 task 编号（task_chain 的某一步） |
| `task_unit` / `task_dsa_en` / `task_pc` | 更新 task_id 时索引 task_chain 得到 |
| `task_fsm` | IDLE / WAIT / RDY / INFLY / FINISH |
| `done_bitmap`（64 bit） | 记录该用户所有 task 的完成标识，**异步 datain 提前完成就体现在这里** |
| `reissue` | 该用户某个任务需监测下游 credit 满足后再搬移到 router |
| `end` | 当前 task 是否为链尾 |

**指针维护**：注册用 `tail_ptr`，释放用 `head_ptr`，每次粒度为 1，上限受 `stream_num` 限制。**年龄优先**的仲裁都是从 `head_ptr` 开始环形扫描，不按 stream_id 数值排序。

### task_fsm 状态机

```
TASK_IDLE ──第一笔是 datain/reissue──→ TASK_WAIT
          └─第一笔是 self_start─────→ TASK_RDY

TASK_WAIT ──done_bitmap 对应位拉高──→ TASK_RDY
          └─credit 条件满足─────────→ TASK_RDY

TASK_RDY  ──成功下发到 RV core─────→ TASK_INFLY
TASK_INFLY──收到 DSA / RV core 完成─→ TASK_FINISH

TASK_FINISH──是最后一笔────────────→ TASK_IDLE（退休）
            ├─后续是已完成的 datain→ 该 task 直接 TASK_RDY
            └─后续是未完成的 datain→ 该 task 进 TASK_WAIT
```

### 任务生成与跳过

Task Generator 里 **每个 Stream 独立推进，不需要全局 Task Pointer**。
只有当前任务进入 `TASK_FINISH` 后才生成后继任务，用一次 64-bit 优先编码**一拍**跳过所有可跳过的 task：

```
SKIP_MASK = ~END_MASK
          & ( (DATA_IN_MASK & done_bitmap)
            | (REISSUE_MASK & ~stream.reissue) )
```

连续 skip 的数量**不增加周期**。可跳过的四种场景：

* 异步 datain_task 已提前完成
* B reissue 任务不需要重发
* P2P 任务不需要重发（Router 通过匹配 `path_id` 告知哪个 P2P task 已完成）
* DP+P2P 场景：有些用户只有 P2P 无计算 task，有些是计算无 P2P，router 请求携带用户是否计算，对应匹配任务链中计算任务，其余 P2P 任务可越过

**End Task 即使已提前完成也不能被跳过**，并且不会再生成后继任务。

### 三条发射通路

| Arbiter | 候选 | 优先级规则 |
| - | - | - |
| `MU_Arb` / `VU_Arb` | `valid=1 && task_fsm=TASK_READY && task_unit=MU/VU` | 从 head_ptr 开始环形年龄优先，选最老 Stream；非抢占保持，直到 RV Core 返回 raw ACCEPT |
| `DTE_Arb` | Reissue 任务 | 优先级最高，从 head_ptr 选最老的 Reissue |
| `DTE_Arb` | DataIn 任务 vs 普通 Generated 任务 | 没有 Reissue 时，两者按相对 head_ptr 的 Stream 年龄比较，较老者优先；**同一 Stream 时优先选 Generated** |

性能特性：**并行支持 3 个 task 的下发**（DTE / MU / VU 各一），task 唤醒延迟在时序满足前提下最短 2～3 cycle。
为保证单用户 TPOT、防止其他用户争抢某个 DSA，支持**对单个 DSA 的 lock**。

两类任务收到 ACCEPT 后改的状态不同，这条边界要划清：

* **Generated 任务**：收到 raw ACCEPT 后向 Stream Map 提交 `TASK_READY → TASK_INFLY`。
* **DataIn 任务**：收到 ACCEPT 后**只通知 User DataIn State 释放 Depth-1 Hold，不改 Stream Map
  里的当前任务状态**。

### 异步 DataIn 机制

在 `tp_nk` 这类切分下，任务链里 task 0/3/7/8 都是“router trigger TS → 调度 DTE 从 router 搬数据进 core”。
但**这些 task 不一定按任务链顺序执行**，谁先谁后取决于对应数据什么时候到 router，
所以没法等链序执行到该 task 再去调度 DTE。

解法：<mde-comment id="0z2frq">TS 支持**不受任务链约束的 datain task**</mde-comment>。每当 router trigger TS，就激活这个 task 下发到 DTE；DTE core 的程序解析数据包头判断这是任务链中哪一步的数据，搬运完成后通知对应 user 的 stream 项，只更新 `done_bitmap` 对应位，不推进 `task_id`。后续任务查询前序所有完成 flag 都有效才能下发。

一个典型的 `tp_nk` 任务链（11 步）：

| task_id | unit | end | 任务描述 |
| - | - | - | - |
| 0 | DTE | 0 | Router trigger TS 后创建任务链，配置 DTE 搬 token 到 Core Mem |
| 1 | MU | 0 | 循环执行多个激活专家的 FC1 和 FC3 运算 |
| 2 | DTE | 0 | 搬多个激活专家的 FC1、FC3 数据到 router，等待逐级 reduce |
| 3 | DTE | 0 | router 逐级 reduce 完 trigger TS 后，搬回 Core Mem |
| 4 | VU | 0 | 循环执行多个激活专家的 FC1 SiLU dot FC3，生成 FC2 token |
| 5 | DTE | 0 | 搬 FC2 token 到 router 做 broadcast |
| 6 | MU | 0 | 循环执行多个激活专家的 FC2 运算和专家间 reduce |
| 7 | DTE | 0 | 本 chip 其他 core 数据到达 router 后 trigger TS，搬到 Core Mem 与本 core 数据 concat |
| 8 | DTE | 0 | 上游 chip 的 FC2 result 到达后 trigger TS，搬到 Core Mem |
| 9 | VU | 0 | 把 concat 完的数据和上游 chip 数据 reduce 后写回 Core Mem |
| 10 | DTE | 1 | 从 Core Mem 搬 result 到 router 并 P2P 传到下游 chip |

### 自发创建任务链（B core / R core）

专用 core 的用户数量远超 TS 的 16 项 stream_table，且数据搬入与后续操作之间的时间窗口很大，TS 覆盖不了。解法是**双任务链 + 软件映射表**：

1. 数据接收类 datain 任务由 datain task 支持，但**完成 flag 由软件设置维护，不在 TS 里更新**
2. 后续任务配置为任务链，调度 RV core 循环 check 软件映射表的 flag，check 通过后执行后续计算 / 搬运 / 搬出：
   * check flag 的 RV core 需要长期工作，该 task **不调度对应的 DSA**
   * 任务链创建不受用户数据 trigger 影响，**自动在每个 stream 表项创建起点 task**（`self_start`），按 stream 顺序激活执行 check flag task
   * 前一个 stream check 通过后需清除 ready 用户的 flag，下一个 stream 才能继续 check
   * 一个 stream 的任务链完成后，会在对应 stream 项**自发创建新的任务链**

B core 复位后可直接自启动 16 个 stream_table 表项，此时没有用户信息，等 task 的 RV core 返回 user_id 再更新 stream_table。B core 的搬出 task 还需 check 下游 core 的 TS credit。

### Credit 与重发

#### 用户 token 重发（Broadcast Reissue）

1. broadcast 数据到达 router trigger TS 时，若下游无法接收则把**重发标记置为有效**
2. TS 查询下游 credit，可下发时按用户顺序选**最老的重发用户**，发起重发 task 到 DTE 去 Core Mem 搬 token 到 router
3. 重发 task **不在用户主线任务链上**，可与用户任务链并行执行，优先级高于其他主线 task
4. 重发标记有效但未重发成功时，<mde-comment id="xw28ej">该用户的原始 token 数据**不能被覆盖，也不能释放该用户**</mde-comment>

#### Reduce 任务的 credit

软件配置任务为逐级 reduce 时需同时配置该任务会操作几次 reduce（`reduce_num = N`）。TS 发现是 reduce task 后**顺序连续下发 N 笔 credit 请求**，credit 满足即可顺序下发，直到收全 N 笔 Rmem finish。

#### P2P 阻塞缓冲

为防止 P2P 传输阻塞导致死锁或性能下降，设一个保底的“P2P 阻塞进 core mem”机制。软件可配四项：
开关、分配给 P2P 缓存的 core mem 容量、开启的方向（最多 **3 个**）、每方向的容量与项数。

TS 要为每个方向各维护一张 P2P 阻塞缓冲映射表（`p2p vld | User id | Down direction | Data addr`），
以及对应下游 core 的 credit 计数器。

### 完成事件的合流（Completion Decoder）

七路独立的完成事件通道：

* **七路独立处理**：DTE、MU、VU 各有独立 Completion Lane（RV core ACK + DSA ACK 共 6 路），DTE Lane 额外接收 Router Reduce Done
* **完成来源判定**：用 `task_completion_src` 区分 RV / DSA / Router 三种来源，避免错误 ACK 提前结束任务
* **Reduce 完成分离**：DTE ACK **只代表搬运完成**，执行 `consume_only` 不修改 Stream 状态；**只有 Router Reduce Done 才有权把 Reduce 任务置为 TASK_FINISH**
* **无序汇合**：DTE ACK 与 Router Done 可任意顺序到达，Router Done 可被 Hold，但必须等匹配的 DTE ACK 被消费后才提交任务完成
* **Router UID 匹配**：Router 不携带 SID，Completion Decoder 内部按 `user_id` 找对应 Stream
* **Future DataIn**：固定接受 DSA ACK，只更新对应 `done_bitmap`，不使用当前任务的完成来源属性

#### task 完成的三种场景

| 场景 | 典型情况 | 谁上报 TS |
| - | - | - |
| RV core 收尾 | 纯 RV core task 不调 DSA，或调了 DSA 但 RV core 会等 DSA 完成后再执行一段程序 | RV core |
| DSA 收尾 | 异步配置调度 DSA 后 RV core 立刻结束 task 程序 | DSA |
| 不确定谁收尾 | RV core 异步配置 DSA 后不查询完成状态，但还要再执行一段程序 | **两者都上报，TS 等二者都完成才算 task 真的完成** |

### 异常检测

* 异步 task 本该执行 1 次完成，但 RV core / DSA 对同一 task 返回多次完成
* RV core、DSA 返回非法 `task_id` / `stream_id`
* 超时检测：异步任务长时间（软件配置，如 1 μs）没收到外部 trigger；用户长时间未 retire；router 请求携带的 `path_id` 在 task_chain 无法匹配
* 配置合规检查：软件配完 `ts_init_finish` 后，硬件检查 task_chain 与 datain_task 的合法性，
  查五项：是否配置、多笔 end 标识、任务不连续设置 valid、多笔 task self_start、非 B core 出现 self_start

配图：[Task Scheduler 19 张](<../../../../perfechpitch/Bach/04_四、MAS（Micro Architecture SPEC）/06_Task Scheduler（编写ing）>)：stream_id_map 结构与时序、Task Generator 流程与 SKIP_MASK、DTE/MU/VU Arbiter、Credit Monitor、Completion Decoder、User DataIn State 的模块图与 LLD 时序图

来源：`04_四、MAS/06_Task Scheduler（编写ing）.md`（前 1200 行有效，Programming Model 后半及 Performance/Power/Area 为 eFUSE 模板残留）、`06_第四阶段/04_core内调度机制.md`

***

## RV Core

三个 RV Core（DTE core / MU core / VU core）是 TS 与 DSA 之间的桥梁：接收 TS 下发的 task，执行 task 对应的 kernel 程序，配置 DSA 执行任务。三者**硬件相同，接口相同**，只是 task 信息和 DSA 配置指令内容有别。

### 指令集

| 扩展 | 支持 | 说明 |
| - | - | - |
| I | 是 | 基本指令集 |
| M | 是 | 整型乘除法 |
| A | 否 | 原子指令，暂不支持 |
| F | 待定 | 单精度浮点 |
| D | 否 | 双精度浮点 |
| C | 考虑支持 | 压缩指令集 |

位宽：**RV32 就足够**（文档结论）。特权：**只支持 M 态**，实现 M 态 CSR，不支持 S/U/H。`fence` 指令实现为 nop。

### <mde-comment id="nou6r0">自定义指令</mde-comment>（custom-0 编码空间）

下面八条自定义指令建模时必须实现，编码取自 ISA 描述表：

| 助记符 | funct3 | opcode | 作用 |
| - | - | - | - |
| `dsar` | 000 | custom-0 | 读 DSA 寄存器，地址来自 rs1 |
| `dsari` | 000 | custom-0 | 读 DSA 寄存器，地址为立即数 `reg_addr1[4:0]` |
| `dsaw.s` | 001 | custom-0 | 写 1 个 DSA 寄存器 |
| `dsaw.d` | 001 | custom-0 | 写 2 个 DSA 寄存器（rd2/rs2 + rd1/rs1） |
| `dsawi.s` / `dsawi.d` | 001 | custom-0 | 同上，寄存器地址用立即数编码 |
| `task_done` | 010 | custom-0 | 带 `TS` 和 `FC` 两个标志位 |
| `flag_check` | 010 | custom-0 | 映射表快速查找，rs1/rs2 给起止地址，rd1 返回偏移 |
| `loop` | 110 | custom-0 | 自定义循环分支，rs1=当前次数，rs2=最大次数，imm 为分支偏移 |

#### DSA 任务配置指令的语义

* 每条最多配置 2 个 DSA 寄存器
* 可带 **trigger 标志**：标识任务包配置完成可以启动
* 可带 **last 标志**：标识该任务包为 task 的最后一个，DSA 执行完后通知 TS task 完成
* 寄存器分**静态配置**（基本不随用户变化，初始化阶段配好，业务流阶段快速调用）与**动态配置**（随用户变化，跟随任务下发，含静态配置选择）
* DSA 寄存器读指令**不支持同步读返回**，软件要查询状态需轮询

#### task_done 指令

* 通知 `pc_gen`：当前 task 完成，若 task_queue 有待执行 task 则跳转到队头 task 起始 PC，否则阻塞取指等待
* `TS` 标志有效 → 通知 TS 当前 task 完成
* `FC` 标志有效 → 附带 fence 功能，等前序所有访存指令完成才通知 TS；否则执行到 decode 阶段即可通知
* firmware 程序结束时需执行一条**不通知 TS** 的 task_done，等待业务流 task

#### flag_check（映射表快速查找）

给起始地址与结束地址，share mem 从起始地址开始查找第一个 1，把位置偏移量写回 rd；
查到结束地址仍没找到则返回全 1。“自发创建任务链”一节里 R core / B core 轮询软件映射表，靠的就是这条指令。

### 流水线微架构

**双发射顺序流水，6～9 级**。各级：

| 模块 | 职责 |
| - | - |
| `pc_gen` | 复位后按 io_reg 的 `boot_pc` 启动；接收 TS 下发的 task 按起始 PC 执行；每拍按分支预测 / 异常 / 顺序自增产生取指 PC。优先级：异常入口 > 分支预测错误纠正 > 分支预测跳转 > PC+8B |
| `loop_bp` | 循环分支预测器，与自定义 loop 指令配合实现**循环退出 100% 正确预测** |
| `ITCM` | 取指 PC 访问 SRAM 读 8B 指令，ECC 校验（1bit 纠正，2bit 报异常），判断越界与不对齐 |
| `decode` | 解码 2 条指令，判断保留指令，汇总后段流水异常并产生清空与异常跳转 |
| `dispatch` | 8 项指令队列（2 或 4 进 2 出），维护通用寄存器状态表，按序派遣到执行单元 |
| `gpr` | 32×32bit，**4 读端口**（双发射每条最多 2 源）、**5 写端口**（ALU×2、MAC/DIV/DSA 共用、LSU×2） |
| `SEU` | 标量执行：ALU0（算逻+分支）、ALU1（算逻+CSR）各 1 拍；MDU 乘法 3 拍流水、除法多周期阻塞 |
| `LSU` | 双通道 3 级流水，都能执行 load/store，按地址范围分配访问通道 |
| `dsa_iss` | DSA 调用指令下发通道，每拍最多一条配置 / trigger 指令 |
| `CSR` | M 态 CSR + 自定义 CSR |

`d03.png` 那张内部流水图给出了几条正文没写的连线。图里红色虚线是流水级边界，把流水切成 7 段，
与“6～9 级”的说法吻合：

* **`boot_pc`** **来自** **`ioreg`**：pc_gen 的一路输入直接连 ioreg。正文把“boot_pc 与异常入口是否为软件可配的
  I/O 寄存器”列为待定，图上已经按可配画了
* **CSR 由 CTRL NOC 直接配置**，不经过流水线；CSR 与 dsa_iss 之间有双向连线（stream_id / user_id / task_id 随 DSA 指令下发）
* **DTCM 画在 LSU 内部**，与 LSU 共一个框；CTRL NOC 有单独一路直连 DTCM 做初始化
* **LSU 有三个外部出口**：实线连 Shared DM，虚线连 Core Mem 和 Router I/O reg。虚线表示只有 DTE core 才接这两路，与“core 内部通路带宽”一节的地址空间视野一致
* **task_queue 在 pc_gen 之前**，TS 的 task 先进队列再驱动取指

#### loop_bp 细节

最多 **4 项**，每项记五个字段：分支指令 PC、分支目的 PC、最大循环次数、当前循环次数、正常循环方向。

* **查表**：每周期用取指 PC 并行比较 4 个表项判断命中；命中则按“当前次数 + 1 是否等于最大次数”
  判断是否循环退出，据此产生预测。
* **新分配表项**：要**查询 SEXE 与 loop_bp 之间的流水级里有没有相同的 loop 分支指令**，
  有则当前循环次数要加上中间流水的数量，防止次数丢失。
* **嵌套深度**由表项数决定，超出的部分不参与预测。

#### LSU 与访存分流

| 目标 | 组织 | 延迟 | 并发 |
| - | - | - | - |
| DTCM | 4 bank 单端口 SRAM，8KB，32bit×4bank | 3 拍 | 可同时接收 2 个不冲突 bank 的请求；同 bank 冲突则阻塞第二条 |
| Share mem | `sm_lsq` 16 项 | 5～10 拍 | 顺序执行，每拍仅发一个读 / 写请求 |
| Core mem | `cm_lsq` 16 项 | 15～25 拍 | 顺序执行，每拍仅发一个请求 |
| Router I/O reg | 复用 `cm_lsq` | — | 仅 DTE core 需要 |

访存带宽 32-bit。写回优先级：DTCM 读出数据与 share_mem / core_mem 数据同时需写回时，**优先写回 share_mem / core_mem**，阻塞 DTCM。

DTE core 访问 Core Mem 的接口与其余通路不同：一次读请求**固定读回 1056 bit**，不支持 burst，
按 32 bit / 拍返回；地址 18 bit、4B 粒度；写请求带 4 bit 字节使能。

### task 下发与完成

#### task_queue

TS 与 RV core 之间有物理路径延时，“前一个 task 完成再通知 TS 下发下一个”会产生很长延迟。
<mde-comment id="p2nntp">RV core 因此设</mde-comment> **<mde-comment id="p2nntp">task_queue 提前接收 TS 下发的</mde-comment>** **task**，前一个 task 完成后立刻执行队头缓存的那个，
做到用户之间 task 的**无 bubble 调度**。

握手规则：RV core 按 task_queue 是否有空槽产生 `task_ack`；未被接收时，TS 不能释放该 task 跳到下一个。

#### 下发与完成信息

| 字段 | 下发（TS→RV） | 完成（RV→TS） | 用途 |
| - | - | - | - |
| `task_pc` | 有 | — | 起始取指 PC |
| `stream_id` | 有 | 有 | 4 bit，用于计算该用户的 Core Mem 与 share_mem 区域基址；硬件写入自定义 CSR，只读 |
| `local_user_id` | 有 | 有 | 12 bit，HBU 流控范围内的编号，用于 R-core 用户映射表和 Matrix Mem 地址计算；**可读写**，R-core 执行 flag_check 后由软件写入 |
| `task_id` | — | 有 | 6 bit，只读；**异步 datain 任务由软件识别包头后写入**，用于告诉 TS 是任务链中哪一步完成 |

### dsa_iss 的 credit 机制

* DSA 任务配置指令下发前检测 DSA 是否可接收新任务，**通过 credit 机制确保配置指令必定可被 DSA 接收并写入**，保证下发通路不被阻塞（否则中断、debug 无法正常工作和记录信息）
* credit 记录 DSA 任务队列项数，容量暂定 16；每下发一条带 trigger 的指令 credit +1，<mde-comment id="uk6dnm">credit 为 0 时阻塞 dispatch 的 DSA 配置指令发射</mde-comment>
* DSA 任务队列释放一项时通知 RV Core 释放 credit
* DSA 读寄存器指令**不受 credit 影响**；`dsa_rq`（8 项）按顺序记录已下发的读指令信息，返回数据后按记录的目的寄存器编号写回 gpr

### 异常

| 异常 | 优先级 | 触发点 | 清空范围 |
| - | - | - | - |
| DTCM Ecc Error | 1（最高） | 访问 DTCM 读出后，**异步非精确** | 只清空 decode 及其前序流水 |
| Load / Store Access Fault | 2 | LSU 第一级流水 | 清空 dispatch 及之前所有流水，若同拍两条指令的第一条则还要清后一条 |
| Illegal Instruction | 3 | 流水到 decode 阶段触发 | 清空 decode 前序取指流水，decode 后续流水不受影响 |
| ITCM Ecc Error | 4 | 同上 | 同上 |
| Fetch Access Fault | 5 | 同上 | 同上 |
| Breakpoint | — | ebreak 指令 / 指令断点 / 访存断点 | 按断点类型对应上面三类 |

Fetch Access Fault 的判定：取指地址超出 ITCM 区间范围，或取指地址低 2 bit 不为全 0（支持 C 扩展时为最低 bit 不为 0）。

### 性能要求

* **单个用户各 DSA 对应的软件调度程序在 RV core 上执行时间不超过 200 cycle**
* 通过 task_queue 提前缓存 task，实现用户与用户之间 task 的无 bubble 调度

配图：[RV Core 15 张内嵌绘图](<../../../../perfechpitch/Bach/04_四、MAS（Micro Architecture SPEC）/07_RV Core>)：`d01` core 内整体框图、`d02` 外部连接、`d03` **内部流水图**、`d04~d08` pc_gen / ITCM / loop_bp / decode / dispatch 各级、`d09~d11` SEU / LSU / dsa_iss、`d12~d15` 四类异常的流水清空范围

来源：`04_四、MAS/07_RV Core.md`、`01_ISA描述.xlsx`（RV Core sheet）

***

<!--mde-comments
[
  {
    "id": "0z2frq",
    "body": "这个相当于中断，要考虑死锁风险\n增加软件管理复杂度",
    "createdAt": "2026-08-24T05:44:57.157Z",
    "updatedAt": "2026-08-24T05:44:57.157Z",
    "author": "Colin"
  },
  {
    "id": "4h38ow",
    "body": "core 进 中断处理的firmware，也应该需要是中断跳转的吧？ 也就意味着，软件要提前配置好中断环境(中断寄存器）",
    "createdAt": "2026-08-24T05:42:07.828Z",
    "updatedAt": "2026-08-24T05:42:19.715Z",
    "author": "Colin"
  },
  {
    "id": "agp9rj",
    "body": "怎么保证原子性？在ShareMem支持原子指令？ 需要软件怎么用？",
    "createdAt": "2026-08-24T05:37:49.486Z",
    "updatedAt": "2026-08-24T05:38:36.901Z",
    "author": "Colin"
  },
  {
    "id": "jzpft2",
    "body": "ITCM 4KB 这个是不是有点少？\n需要软件评估一下",
    "createdAt": "2026-08-24T05:35:22.399Z",
    "updatedAt": "2026-08-24T05:35:34.728Z",
    "author": "Colin"
  },
  {
    "id": "nou6r0",
    "body": "这些指令的原子性是怎么保证的？硬件支持？",
    "createdAt": "2026-08-24T06:33:08.888Z",
    "updatedAt": "2026-08-24T06:33:08.888Z",
    "author": "Colin"
  },
  {
    "id": "p2nntp",
    "body": "相当于多一级inflight，这个会不会和前面的 中断的DTE传输造成冲突？",
    "createdAt": "2026-08-24T06:58:15.736Z",
    "updatedAt": "2026-08-24T06:59:02.530Z",
    "author": "Colin"
  },
  {
    "id": "uk6dnm",
    "body": "这里credit是在rv 里面？\n阻塞 dsa指令发射，会直接把core 死锁吧？",
    "createdAt": "2026-08-24T07:08:26.042Z",
    "updatedAt": "2026-08-24T07:09:28.272Z",
    "author": "Colin"
  },
  {
    "id": "xw28ej",
    "body": "这个感觉，就需要TS来感知，各个任务之间的数据依赖避免一个数据没用搬出去，就被其他的任务复用了这片存储器空间，造成踩踏",
    "createdAt": "2026-08-24T05:48:25.860Z",
    "updatedAt": "2026-08-24T05:48:25.860Z",
    "author": "Colin"
  }
]
-->
