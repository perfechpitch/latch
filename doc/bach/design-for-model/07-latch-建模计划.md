# 第 7 章　latch 建模计划

**模式**：design（陈述当前设计，动机与取舍收在各节的“取舍”段落里）

给在 latch 上按逐拍 cycle 模型重建 Bach 的实现者，定下六件事：

1. 要建哪些对象
2. 每个对象在模型里是什么
3. 前面几章定义的机制各落在模型的哪里
4. 代码怎么组织
5. 按什么顺序建
6. 怎么验收

三条边界：

* 模型的顶层是一个 LPU，即 48 颗 chip 装模型的一层 MoE。LPU 之外的硬件一律一个桩；chip 之内的对象粒度与《Core 内硬件》《执行单元与存储》的子块一致
* 机制全部实现，数值按 bit 级与参考实现比对
* 《建模参数与性能模型》的参数直接成为各单元的输入

本章不复述前面几章的规则，只写模型怎么承载它们。

《Bach 硬件设计建模参考》第 7 章，全套目录见 [README](README.md)。

***

## 建模范围与对象清单

### 四条总决定

1. **逐拍 cycle 模型**：功能与时序一起建，每个仲裁点、每条通路都有拍数。
2. **机制全覆盖**：第 2 至 4 章定义的每一条机制都落到一个单元的一个函数，并有一个用例；覆盖关系在各单元文档的“机制覆盖”表里逐条登记。
3. **bit 级数值**：MU 的累加顺序、MXFP8 的 scale block、VU 的三处舍入、Router reduce 的 FP32 中间精度都按硬件规则实现；参考实现按同样的顺序计算，逐元素比对不留容差。
4. **RV core 跑真实 RV32 程序，不建流水线**：kernel 编译成 RV32 程序，由 `src/rv32` 的功能模型逐条执行，每条指令 1 拍，访存与 DSA 读的延迟另计；task_queue、dsa_iss、CSR、task_done 这些与 TS 和 DSA 交互的机制照建。

### 对象清单

“形态”一列：模块 = 独立打拍的 `ClkModule`；装配 = 只做构造与接线的容器；静态 = 仿真前算好的表；桩 = 只模仿接口行为的模块。

| 层 | 对象 | 对应设计 | 形态 | 文档 |
| - | - | - | - | - |
| LPU 之外 | 片外桩：入口桩、出口桩 | 第 6 章“GPU → Bach 的两层 credit 反压”“输出包格式”、第 2 章 Node 组成 | 桩 ×2 | [`external-stub.md`](07-units/external-stub.md) |
| LPU | LPU：48 chip 的构造与接线、全局坐标换算、chip 形状与逻辑 core 映射的读入 | 第 2 章“集群与 Node”“机柜内多 tray 互联与 token 派遣”、第 6 章编译器的硬件抽象 | 装配 | [`lpu.md`](07-units/lpu.md) |
| LPU | 链路：R2R、C2C、跨 tray 纵向、PCIe ↔ Router、ETH | 第 2 章互连参数、第 5 章延迟表 | 模块（带宽、延迟、到达时刻） | [`link.md`](07-units/link.md) |
| LPU | PCIe Switch | 第 2 章“tray 组成”、第 6 章两层 credit | 模块 ×12 | [`pcie-switch.md`](07-units/pcie-switch.md) |
| chip | Chip（中间列 2×4、两侧 2×5、四个 C2C 端口）、SCP 桩、ctrl_noc 端点每 core 一个、C2C Bridge ×4 | 第 2 章“Chip 内结构与 Boot”、第 3 章 Router 的“跳过与跨 chip” | 装配 + 模块 ×3 类 | [`chip/chip.md`](07-units/chip/chip.md) |
| core | Core | 第 3 章 Bach Core 顶层 | 装配 | [`chip/core/core.md`](07-units/chip/core/core.md) |
| core | Router：RouterTable 与 CSR、RouterStation ×3、Xbar、CoreStation、CoreMem 重发、ReduceModule、Retire、CoreMemCreditMonitor | 第 3 章 Router | 模块 ×8 | [`chip/core/router.md`](07-units/chip/core/router.md) |
| core | TS：User_Match、CFG_REG、DataIn_task_table、Stream_table、Task_ctrl、DTE_Arb、MU_Arb、VU_Arb、Except Check | 第 3 章 TS 任务调度器 | 模块 ×9 | [`chip/core/ts.md`](07-units/chip/core/ts.md) |
| core | RV core ×3：task_queue、指令执行器（`src/rv32`）、dsa_iss、访存（sm_lsq / cm_lsq）、CSR | 第 3 章 RV Core | 模块 ×3 | [`chip/core/rv-core.md`](07-units/chip/core/rv-core.md) |
| core | DTE：Header Parser、Commit、TaskQueue ×4、Lane ×4（含 AGCU）、中间 Buffer、Completion RS、Hmem 与 LUT、topK 与 shareMem 写 | 第 4 章 DTE DSA | 模块 ×8 | [`chip/core/dte.md`](07-units/chip/core/dte.md) |
| core | MU：regfile、issue_q、gen_ep_info、agu ×3、ldq ×2、matrix exe、stq | 第 4 章 MU DSA | 模块 ×7 | [`chip/core/mu.md`](07-units/chip/core/mu.md) |
| core | VU：config_register、ISQ、pipe_ctrl 与 Scoreboard、LU、SU、SMUX / DMUX、VALU0 / VALU1 / VALU2 / VSFU、MEXE、SEXE、VRF / MRF / SRF、Profile | 第 4 章 VU DSA | 模块 ×11 | [`chip/core/vu.md`](07-units/chip/core/vu.md) |
| core | Core Mem、Matrix Mem、Share Mem 及各自的仲裁器 | 第 4 章存储子系统 | 模块 ×3 | [`chip/core/memory.md`](07-units/chip/core/memory.md) |

### 对象的层级与文档归属

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1160 640" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <rect x="0" y="0" width="1160" height="640" fill="#ffffff"/>
  <text x="20" y="30" font-size="12" fill="#111827">建模对象的层级与文档归属</text>
  <rect x="30" y="60" width="186" height="62" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="42" y="80" font-size="11.5" fill="#7c2d12">片外桩</text>
  <text x="42" y="96" font-size="9" fill="#92400e">入口桩 · 出口桩</text>
  <text x="204" y="80" font-size="9" fill="#9ca3af" text-anchor="end">桩 ×2</text>
  <text x="204" y="113" font-size="8.5" fill="#9ca3af" text-anchor="end">external-stub.md</text>
  <rect x="30" y="150" width="186" height="70" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="42" y="170" font-size="11.5" fill="#111827">LPU</text>
  <text x="42" y="186" font-size="9" fill="#475569">48 chip · 12 × 4 网格</text>
  <text x="42" y="200" font-size="9" fill="#475569">坐标换算 · chip 形状</text>
  <text x="204" y="170" font-size="9" fill="#9ca3af" text-anchor="end">装配</text>
  <text x="204" y="211" font-size="8.5" fill="#9ca3af" text-anchor="end">lpu.md</text>
  <line x1="123" y1="122" x2="123" y2="148" stroke="#b45309" stroke-dasharray="4 3"/>
  <text x="131" y="140" font-size="9" fill="#92400e">经 PCIe Switch 接入</text>
  <rect x="230" y="150" width="206" height="44" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="242" y="170" font-size="11.5" fill="#111827">链路</text>
  <text x="424" y="170" font-size="9" fill="#9ca3af" text-anchor="end">模块</text>
  <text x="424" y="185" font-size="8.5" fill="#9ca3af" text-anchor="end">link.md</text>
  <rect x="230" y="206" width="206" height="44" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="242" y="226" font-size="11.5" fill="#111827">PCIe Switch</text>
  <text x="424" y="226" font-size="9" fill="#9ca3af" text-anchor="end">模块 ×12</text>
  <text x="424" y="241" font-size="8.5" fill="#9ca3af" text-anchor="end">pcie-switch.md</text>
  <rect x="230" y="284" width="206" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="242" y="304" font-size="11.5" fill="#111827">chip ×48</text>
  <text x="242" y="320" font-size="9" fill="#475569">2×4 / 2×5 core · SCP · ctrl_noc</text>
  <text x="424" y="304" font-size="9" fill="#9ca3af" text-anchor="end">装配</text>
  <text x="424" y="331" font-size="8.5" fill="#9ca3af" text-anchor="end">chip/chip.md</text>
  <polyline points="216,185 232,185 232,172 230,172" fill="none" stroke="#94a3b8"/>
  <path d="M232 194 L232 228 L230 228" fill="none" stroke="#94a3b8"/>
  <path d="M232 250 L232 306 L230 306" fill="none" stroke="#94a3b8"/>
  <rect x="452" y="278" width="208" height="52" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="464" y="300" font-size="11" fill="#111827">SCP 桩 · ctrl_noc 端点 ×10</text>
  <text x="648" y="300" font-size="9" fill="#9ca3af" text-anchor="end">模块 ×3 类</text>
  <text x="464" y="316" font-size="9" fill="#475569">C2C Bridge ×4</text><text x="648" y="322" font-size="8.5" fill="#9ca3af" text-anchor="end">chip/chip.md</text>
  <rect x="452" y="356" width="208" height="44" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="464" y="376" font-size="11.5" fill="#111827">core ×10</text>
  <text x="648" y="376" font-size="9" fill="#9ca3af" text-anchor="end">装配</text>
  <text x="648" y="391" font-size="8.5" fill="#9ca3af" text-anchor="end">chip/core/core.md</text>
  <polyline points="436,312 452,312 452,306 452,306" fill="none" stroke="#94a3b8"/>
  <path d="M452 330 L452 378 L452 378" fill="none" stroke="#94a3b8"/>
  <rect x="676" y="60" width="460" height="72" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="688" y="80" font-size="11.5" fill="#111827">Router</text>
  <text x="688" y="96" font-size="9" fill="#475569">RouterStation ×3 · Xbar · CoreStation · ReduceModule</text>
  <text x="688" y="110" font-size="9" fill="#475569">RouterTable / CSR · CoreMemCreditMonitor · Retire</text>
  <text x="688" y="124" font-size="9" fill="#475569">CoreMem 重发</text>
  <text x="1124" y="80" font-size="9" fill="#9ca3af" text-anchor="end">模块 ×8</text>
  <text x="1124" y="123" font-size="8.5" fill="#9ca3af" text-anchor="end">chip/core/router.md</text>
  <path d="M660 378 L676 378" fill="none" stroke="#94a3b8"/>
<path d="M676 368 L676 356" fill="none" stroke="#94a3b8"/>
<path d="M676 284 L676 272" fill="none" stroke="#94a3b8"/>
<path d="M676 228 L676 216" fill="none" stroke="#94a3b8"/>
<path d="M676 144 L676 96 L676 96" fill="none" stroke="#94a3b8"/>
  <rect x="676" y="144" width="460" height="72" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="688" y="164" font-size="11.5" fill="#111827">TS 任务调度器</text>
  <text x="688" y="180" font-size="9" fill="#475569">CFG_REG · User_Match · DataIn_task_table · Stream_table</text>
  <text x="688" y="194" font-size="9" fill="#475569">Task_ctrl · DTE_Arb · MU_Arb · VU_Arb · Except Check</text>
  <text x="688" y="208" font-size="9" fill="#475569">Task_done</text>
  <text x="1124" y="164" font-size="9" fill="#9ca3af" text-anchor="end">模块 ×9</text>
  <text x="1124" y="207" font-size="8.5" fill="#9ca3af" text-anchor="end">chip/core/ts.md</text>
  <path d="M676 216 L676 180 L676 180" fill="none" stroke="#94a3b8"/>
  <rect x="676" y="228" width="460" height="44" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="688" y="248" font-size="11.5" fill="#111827">RV core ×3</text>
  <text x="688" y="264" font-size="9" fill="#475569">task_queue · 指令执行器（src/rv32）· dsa_iss · lsq · CSR</text>
  <text x="1124" y="248" font-size="9" fill="#9ca3af" text-anchor="end">模块 ×3</text>
  <text x="1124" y="263" font-size="8.5" fill="#9ca3af" text-anchor="end">chip/core/rv-core.md</text>
  <path d="M676 272 L676 250 L676 250" fill="none" stroke="#94a3b8"/>
  <rect x="676" y="284" width="460" height="72" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="688" y="304" font-size="11.5" fill="#111827">DTE DSA</text>
  <text x="688" y="320" font-size="9" fill="#475569">Header Parser · Commit · TaskQueue ×4</text>
  <text x="688" y="334" font-size="9" fill="#475569">5 个物理通道（AGCU）· 中间 Buffer · Completion RS · Hmem 与 Fast LUT</text>
  <text x="688" y="348" font-size="9" fill="#475569">topK 与 shareMem 写</text>
  <text x="1124" y="304" font-size="9" fill="#9ca3af" text-anchor="end">模块 ×8</text>
  <text x="1124" y="347" font-size="8.5" fill="#9ca3af" text-anchor="end">chip/core/dte.md</text>
  <path d="M676 356 L676 320 L676 320" fill="none" stroke="#94a3b8"/>
  <rect x="676" y="368" width="460" height="58" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="688" y="388" font-size="11.5" fill="#111827">MU DSA</text>
  <text x="688" y="404" font-size="9" fill="#475569">regfile · issue_q · gen_ep_info · agu ×3 · ldq ×2</text>
  <text x="688" y="418" font-size="9" fill="#475569">matrix exe · stq</text>
  <text x="1124" y="388" font-size="9" fill="#9ca3af" text-anchor="end">模块 ×7</text>
  <text x="1124" y="417" font-size="8.5" fill="#9ca3af" text-anchor="end">chip/core/mu.md</text>
  <path d="M676 378 L676 397 L676 397" fill="none" stroke="#94a3b8"/>
  <rect x="676" y="438" width="460" height="72" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="688" y="458" font-size="11.5" fill="#111827">VU DSA</text>
  <text x="688" y="474" font-size="9" fill="#475569">config_register · ISQ · pipe_ctrl · LU · SU</text>
  <text x="688" y="488" font-size="9" fill="#475569">SMUX / DMUX · VALU ×3 · VSFU · MEXE · SEXE · 寄存器堆</text>
  <text x="688" y="502" font-size="9" fill="#475569">Profile</text>
  <text x="1124" y="458" font-size="9" fill="#9ca3af" text-anchor="end">模块 ×11</text>
  <text x="1124" y="501" font-size="8.5" fill="#9ca3af" text-anchor="end">chip/core/vu.md</text>
  <path d="M676 426 L676 474 L676 474" fill="none" stroke="#94a3b8"/>
  <rect x="676" y="522" width="460" height="44" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="688" y="542" font-size="11.5" fill="#111827">存储</text>
  <text x="688" y="558" font-size="9" fill="#475569">Core Mem · Matrix Mem · Share Mem</text>
  <text x="1124" y="542" font-size="9" fill="#9ca3af" text-anchor="end">模块 ×3</text>
  <text x="1124" y="557" font-size="8.5" fill="#9ca3af" text-anchor="end">chip/core/memory.md</text>
  <path d="M676 510 L676 544 L676 544" fill="none" stroke="#94a3b8"/>
  <text x="20" y="600" font-size="10.5" fill="#374151">装配容器只做构造与接线，自身没有 Cycle()；模块各自持有推进它的协程；桩只模仿接口行为。</text>
  <text x="20" y="622" font-size="10.5" fill="#374151">右下角是这一层对应的单元文档，全部在 07-units/ 下，目录层级与本图一致。</text>
</svg>
```

### 本轮不建的部分

* Host CPU、Node CPU、tray CPU（源文档叫 Board CPU）的业务流控与退出、动态专家调度（第 2 章“业务流控与退出”）。LPU Dispatch 的派遣规则（所有 R core 有余量才派遣）放在入口桩里。
* Debug Module、DTM、GDB。
* 异常、ECC、看门狗、功耗类机制（RV core 九类异常、MU Drain & Trap、VU error_code、TS Except_Check、DTE 首错保留、DIDT 分级、零输入门控、MU 与 VU 错峰）。各单元给这些机制留出状态位与接口名，本轮不实现其行为。
* RV core 的流水线细节：pc_gen、loop_bp、decode、dispatch、双发射、gpr 端口、SEU 的乘除多拍、DTCM 的 bank 冲突。它们折算成每条指令 1 拍。

***

## 建模方式

### 逐拍 cycle 模型

latch 的建模方法分两段：功能模型（`src/rv32`：逐条指令执行，无时序）与逐拍 cycle 模型（手写模块，直接用 runtime 原语）。

Bach core 里的 TS 按 stream 年龄仲裁发射、三块存储按 bank 仲裁多 master、Router 有 credit 闭环与多播原子准入、VU 有 Scoreboard，这些都要逐拍表达优先级排队与原子准入。因此 Bach 建成逐拍 cycle 模型，范本是 `src/module/llc.h` 与 `src/module/rv32_pipeline.h`，遵循 `module_writing_guide.md` 的全部规则。RV core 是唯一的例外：它把 `src/rv32` 的功能模型当指令执行器，外面包一层逐拍的 task_queue、dsa_iss、lsq 与延迟记账。

### 挂时钟粒度：图上的每个模块独立打拍

硬件框图里的每个模块都是一个 `tick=true` 的 `ClkModule`，各自持有推进它的协程：Core 内的 Router 八个模块、TS 九个模块、三个 RV core、DTE / MU / VU 的各模块、三块存储；Core 外的 PCIe Switch、链路、SCP 桩、ctrl_noc 端点，以及 LPU 之外的入口桩与出口桩。LPU、Chip 与 Core 不是模块，是装配容器：构造各模块、按各单元文档的接口把端口对接起来，自身没有 `Cycle()`。

一个模块一拍做的事全部写在它的 `Step()` 里：读入口端口上一拍锁存的值，算本拍的组合逻辑，把结果写到出口端口，下拍对方才看得到。`Cycle()` 只做起手的 `DelayCycle(1)` 再调 `Step()`，`Step()` 里一次也不许让出。模块之间没有调用关系，同一拍里各模块的执行顺序不影响结果。这条分工写在 `ip/module_base.h` 的基类里。

波形层次是 `chip<i>.core<j>.<单元>.<模块>.<信号>`。常驻协程数等于模块数，48 chip 的规模见“风险”。模块单测时直接拉起该模块和驱动它的测试桩，不需要装配整个 Core。

### 等待改写成跨拍状态机

`Cycle()` 体内除起手的 `DelayCycle(1)` 外不得再 yield，否则该协程挂起会卡住 OldestStamp。硬件里的每一处等待都写成跨拍状态机，每拍判断一次：

| 单元 | 等待点 | 状态机 |
| - | - | - |
| TS | 等前序 task 完成、等 datain、等 credit、等 RV core 空闲、等 `raw ACCEPT` | 每个 stream 表项一个 `task_fsm`：IDLE → WAIT → RDY → INFLY → FINISH |
| RV core | 等 task、等源 gpr 就绪（DSA 读、访存返回）、等 lsq 与 dsa_iss 有空 | 执行状态加 gpr 就绪表 |
| DTE | 等 Commit 配对资源、等 Lane 激活、等访存返回、逐拍搬运、等 Router 反压解除、等 Completion RS 的 Join | 每条 Lane 一个 Active Context 状态机，每个任务一个完成状态（queued → active → issue_done → drained → join_done → task_done） |
| MU | 等 issue_q 出队、等 Matrix Mem / Core Mem 返回、流水推进、等 stq 拼满 | 原语状态机，按 lane 深度与 outstanding 计拍 |
| VU | ISQ 排队、pipe_ctrl 合法性检查、Scoreboard 依赖、等 CM Load / Store 返回、宏指令重叠 | 宏指令状态机，最多两条相邻在飞 |
| Router | 等 VC credit、等 Stream 坑、等 Reduce credit、多播原子准入、Reduce 累加、AbsorbReissue | Message 状态机：Idle → Lookup → DeliverLocal / Forward / ReduceAccum / AbsorbReissue |
| 存储 | bank 冲突 | 仲裁器每拍 `TryGrant` 一次，被拒的请求下一拍再来 |

每个 Core 的状态机数量有界：stream 表项 16、DTE TaskQueue 4 × 16、VU 在飞宏指令 2、MU issue_q 16、Router 每方向 4 个 VC 各一个队首上下文。

`common/arbiter.h` 提供四种排队语义：

| 硬件里的资源 | 仲裁器 |
| - | - |
| 独占且先到先得（bank 端口、Lane、Xbar 出口） | 独占仲裁器，等待队列先进先出 |
| 按年龄或固定优先级（TS 三条发射通路、Cmem 的 MU > VU = DTE、DTE Commit 的 Bank0 > Bank1） | 优先级仲裁器，等待队列按优先级、请求拍、插入序排 |
| 计数额度（Stream 坑、VC credit、Reduce credit、TaskQueue 项数、DTE Buffer credit、CH1 outstanding） | 计数信号量，get 队列队首阻塞 |
| 多方向一次性扣减（多播原子准入、广播任务一次扣掉全部目的数、Commit 的三项配对接纳） | 全部方向都够才一起扣，任一不足整体不发 |

### 跨模块按硬件的握手协议与打拍规则

模块之间只通过端口相连。端口上的每个信号都是上拍写、下拍读的寄存器（`Logic64` / `LogicCell`，在端口束里 `Fields(...)` 注册）；在 latch 里一个端口组实现成一组 `Logic` 字段或一个深度 1 的 `Fifo`，两者都是上拍写下拍读，选哪个是实现细节，不出现在接口声明里。一个端口组用什么协议，照该接口在第 2 至 4 章里的定义：

| 协议 | 用在 | 规则 |
| - | - | - |
| valid/ready | TS ↔ RV core 的 task 下发、RV core → DSA 的配置下发、DSA / RV core ↔ 存储的请求、Xbar 的出口授予 | 发送方拉 valid 并保持数据不变直到看见 ready；接收方的 ready 是它上一拍锁存的值（等价于硬件里带 skid buffer 的注册 ready），一次握手最少两拍 |
| credit / release | Router 链路的 VC credit、Stream credit、Reduce credit；CoreStation ↔ DTE 的 VC credit；DTE 中间 Buffer 的 credit | 发送方本地扣 credit 后才发，不等对方应答；release 经独立通道回传，回传也打一拍 |
| AXI-Stream-like | CoreStation ↔ DTE 的 DataIn / DataOut | valid/ready 加 last 与有效字节数；反压时数据保持 |
| 脉冲 | credit_issue_pulse、reduce_done、trigger、Retire 广播、task_done | 单拍有效，接收方当拍锁存 |
| 电平 | ready 全高、busy、状态寄存器 | 持续有效，接收方任意拍读 |
| ctrl_noc 写事务 | SCP → core 内寄存器 | 每笔事务一拍，按地址分发到目的模块的 `cfg` 端口 |

链路上的封包按 flit 传，flit 是 `Logic` 的派生类，`LogicPtr<Message>` 让同一 Message 的多个 flit 共享 payload 字节（bit 级比对的依据）；Message 的 Header 与 Payload 合成一个 flit 序列，首 flit 携带 Header。发包时在 `Step()` 体内现建一个 flit，写完字段再送进端口，不复用成员：`Latch::Get()` 对本拍未写的字段回落到上一拍的值，复用会把旧字段带出去。

`Fifo` 是模块内部的存储结构（VC Buffer、TaskQueue、ISQ、链路的在途队列），不是模块的接口。

### 可观测量与禁用清单

跨拍可读的计数、占用与标志一律用 `Logic64`，配一个 cycle 内的裸 `uint64_t` 累加器，在 `Step()` 末尾一次性 commit。`Logic64` 一拍只能 Set 一次，同拍多次 Set 触发断言。

`Logic64::Get()` 在主线程读到的是 t=0 的值。因此完成集合、结束时刻、credit 余额这些验收产物必须在协程内 snapshot 到普通变量，不能在 `JoinAll()` 之后直接读。

不出现 `std::mutex`、`std::lock_guard`、`std::condition_variable`、`std::atomic`。

***

## 模型结构

### 目录按硬件层级

`ip/` 的目录层级与对象清单的层级一致：一层硬件一个目录，目录里每个头文件对应清单里的一个模块，与 `07-units/` 下那一层的文档一一对上。Python 版模拟器遗留的单元（`credit_unit.h`、`moe_bitmap.h`、`compute/`、`eth_switch/`、`external/phase*`、`dispatcher.h`）保留不动，新单元不依赖它们。

```
src/bach/
  ip/
    module_base.h                  模块基类：Cycle() = DelayCycle(1) + Step()，Step() 内不让出
    node_context.h                 每个节点都有的编号、参数表与记录器
    wiring.h                       接一条双向物理链路（数据端口对 + 三种 release 端口）
    lpu.h                          LPU 装配：构造 48 个 Chip、按 12 × 4 网格接 C2C、接 PCIe Switch 与片外桩
    lpu_grid.h                     全局坐标换算：(tray, 层, 列) ↔ (gx, gy)，以及片外节点与跨 chip 网关的坐标
    link/
      link.h                       链路模型：带宽、延迟、arrive_cycle 计算
    pcie_switch.h                  双路 x16、组播复制、按最慢收端反压
    external/
      in_stub.h                    入口桩：注入表、两层 credit、自定义包头、LPU Dispatch 派遣
      out_stub.h                   出口桩：收结果、按 (gpu_id, token_id) 与参考实现比对
    chip/
      chip.h                       2×4 / 2×5 阵列、chip 形状、四个 C2C 端口、ctrl_noc
      scp.h                        SCP 桩：boot 序列、ctrl_noc 配置事务
      ctrl_noc_endpoint.h          ctrl_noc 在 core 内的落点：寄存器写入分发
      core/
        core.h                     装配容器：构造 core 内全部模块，按各单元文档的接口对接端口
        core_context.h             各模块共用的只读上下文（core id、角色、参数表）
        ports.h                    端口束：valid/ready、credit/release、AXI-Stream-like、脉冲、电平各一种字段结构
        router/
          router_table.h           RouterTable 与 CSR、Credit Bypass Route、多副本提交状态机
          router_station.h         ×4：Header Parser、VC Buffer、Packet Context、Stream Resource Table、VC Credit
          xbar.h                   按输出仲裁、多播全有全无、入口锁定
          core_station.h           HeaderFIFO、OutputBuffer、三态准入、TS 通知、出 core VC 流控
          coremem_reissue.h        stall_way、Bypass 映射成进核加出核、同 VC 保序
          reduce_module.h          16 用户上下文、RMW 累加、Reduce credit、输出队列
          retire.h                 Retire 广播与两处回收
          credit_monitor.h         监听事件队列 16 项全相连
        ts/
          cfg_reg.h                task_chain 64 项、datain_task、stream_num、Task LUT
          user_match.h
          datain_task_table.h      Depth-1 Hold
          stream_table.h           16 项 FIFO、task_fsm、六个写端口
          task_ctrl.h              SKIP_MASK 一拍跳过、原子安装
          dte_arb.h  mu_vu_arb.h
          credit_monitor.h         Reissue 唤醒、Head-only 退休、credit 申请
          task_done.h              七路完成合流
        rv_core/
          rv_core.h                驱动 src/rv32 的 SystemRv32 逐条执行；task_queue、dsa_iss、dsa_rq、lsq、gpr 就绪表、自定义 CSR
          bach_insts.h             custom-0 自定义指令（dsar、dsari、dsaw、dsawi、task_done、flag_check、loop）
          rv_ports.h               RV core 地址空间：ITCM、DTCM、Share Mem、Core Mem、Router I/O reg 各一个 MemoryPort
          kernel/
            kernel_api.h           kernel 源码侧头文件：自定义指令的 inline asm 封装、DSA 寄存器地址、自定义 CSR 编号
            *.c  link.ld  Makefile 各 kernel（bcore_datain、check_flag、broadcast、weights loader、计算 core 各 task），riscv gcc 编译成每类 core 一个 ELF
        dte/
          header_parser.h  commit.h  task_queue.h  lane.h  agcu.h  buffer.h  completion_rs.h  hmem.h
        mu/
          regfile.h  issue_q.h  gen_ep_info.h  agu.h  ldq.h  matrix_exe.h  stq.h
        vu/
          config_register.h  isq.h  pipe_ctrl.h  lu.h  su.h  mux.h  valu.h  vsfu.h  mexe.h  sexe.h  regfiles.h  profile.h
        memory/
          core_mem.h  matrix_mem.h  share_mem.h  bank_arbiter.h
  common/
    flit.h  message.h              封包与 Message
    params.h                       全部参数的唯一出处
    regmap.h                       DSA 寄存器地址映射（VU 按 MAS；MU、DTE 为临时映射）
    arbiter.h                      四种仲裁器
    numeric/                       FP8_e4m3 / MXFP8 / MXFP4 / NVFP4 / BF16 / FP32 的编解码、舍入、CSA 累加顺序
  tables/
    router_table.h  task_chain.h  header_tables.h  loader.h      编译侧产物的读入
  observer/
    span_recorder.h  jsonl_writer.h
  sim/
    build.h                        从编译侧产物装配出整套硬件
    completion.h                   完成判据与全局生命周期看门狗
  reference/
    ffn_reference.py               参考实现（与 numeric/ 同一套累加顺序）
```

模块之间只有端口。装配容器把生产者的出口端口和消费者的入口端口对接，两侧都只看到端口束的字段，不持有对方的类型，装配顺序不受构造顺序牵制。

`test/bach/ip/` 按 `ip/` 的层级镜像分目录，每个模块旁边有它自己的测试。

### 物理归属

**Router 属于 Core。**

* Core 构造时创建各模块，Router 的八个模块是其中一组，每个 Core 一份
* chip 内 mesh 的连线动作在 Chip 里做，但被连的端口长在 RouterStation 上
* 不派角色的 core 只构造 Router 的模块，不构造 TS、RV core、DSA 与存储

**链路与 PCIe Switch 属于 LPU。**

* 每条物理链路每方向一个链路实例，实例本身不属于任何一颗 chip，两端各是一个模块的端口
* PCIe Switch 的坐标是不与核阵列重叠的锚点，端口由拓扑起名；核那一侧仍是 chip 边界四个 C2C 端口之一
* 两侧各用各的端口标识，接线时对接

**SCP 桩与 ctrl_noc 端点属于 Chip。**

* SCP 桩每 chip 一个，只接 ctrl_noc
* ctrl_noc 端点每 core 一个，落点是 core 内各模块的 `cfg` 口

**入口桩与出口桩在 LPU 之外。**

* 它们的坐标不在核阵列内，登记在片外节点表里
* 两个桩挂在 PCIe Switch 上，各自带一个路由器
  * 包先进自己那个路由器的本地口，由它按链路的带宽与延迟送到网关核
  * 回来的包也在这个路由器上落地重组，再交给桩本身

### 接口的声明方式

每个模块的接口按《硬件电路设计描述规范》的“接口”模板声明在该单元的文档里：每个端口组一个声明块，注明 master / slave、协议、时钟域，valid/ready 组注明 ready 的成立条件；信号名与代码一致。本章不汇总各口。

***

## 各单元的建模规格

每个单元一份文档，放在 `07-units/` 下，目录层级与对象清单一致。文档按九章写，前六章取自《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`），后三章是本章的要求：

| 章 | 写什么 |
| - | - |
| 1 定位与边界 | 一句话职责加第 0 层图：本单元的全部模块与邻居、每条对外通路的端口组名 |
| 2 功能清单 | 本单元做的每一件事一条，按模块分组，编号 F1、F2…。这一章是实现的清单，也是第 8 章“机制覆盖”指向的落点 |
| 3 接口 | 每个端口组一个声明块，注明 master / slave、协议、时钟域；valid/ready 组注明 ready 的成立条件；信号名与代码一致 |
| 4 存储器 | 全模块的存储清单，含级间 latch，每行给类型、形状、读写口、写规则、复位 |
| 5 流水线总览 | 第 1 层图：本单元全部模块的级、级间 latch、存储与端口的连接关系，每级标 Mx 与 Dx。图下不超过三行短句，只写图画不出的规则 |
| 6 逐级行为 | 第 2 层图：每级一张三栏小图（入口存储器 → 组合逻辑 → 出口存储器），过四要素检查（信号连接带位宽、读写的存储、实现功能的赋值表达式、右上角 Dx），级编号与第 1 层图一致 |
| 7 参数汇总 | latency / 深度 / 位宽常量，注明权威出处；设计未给值的写默认值并标“待定”，全部待定值汇总在本章末尾 |
| 8 机制覆盖 | 三列：机制、落在哪条功能（第 2 章的编号）、用例（`test/bach/ip/` 下的测试名） |
| 9 取舍 | 这一单元为什么这么设计，动机与被否掉的方案收在这里 |

LPU 与 Core 是纯装配容器，没有自己的一拍工作，第 5、6 两章对它们永远是空的，构造期的接线步骤写在第 2 章的功能清单里。Chip 那一份除装配外还带 SCP 桩、ctrl_noc 端点与 C2C Bridge 三类模块，第 5、6 两章写这三类。编译侧算好的那些静态表不单独立文档：每张表跟着用它的那个单元，写在该单元第 4 章“存储器”的末尾；跨表的自洽检查没有单一持有者，收在本章“输入”一节。

图一律手写 SVG，用该规范的 stencil；上层盒子名 = 下层图标题，上层箭头上的信号名 = 下层图的端口组名。逐层的缝合关系就是文档的目录层级：LPU 第 0 层图里的一颗 chip 盒子，展开是 Chip 的第 0 层图；Chip 图里的一个 core 盒子，展开是 Core 的第 0 层图；Core 图里的一个单元盒子，展开是该单元文档的第 0 层图。第 0 层图要画全：本单元的每个模块、每条对外通路、每类 credit 的走向都要出现在图上。

***

## 输入

模型读入四类东西，本章只规定内容要求。文件格式见《Bach 中间文件格式》，它的 `cycle` 档就是本章这十张表；数值格式、舍入与累加顺序见《数值与参考实现》。

| 类 | 内容 | 来源 |
| - | - | - |
| 拓扑与部署 | chip 数 48、tray 数 3（编译器叫 rack）、tray 形状 4 层 × 4 chip、chip 形状（中间列 2×4、两侧 2×5）、全局进出口位置（`global_top_left` / `global_bottom_right`）、逻辑 ↔ 物理 core 映射、切分参数（EP / TP / PP / DP 与四种模式之一）、GPU 数与每 GPU 的 batch | 编译侧 |
| 每 core 配置 | RouterTable（每 path 一表项、三份副本一致）、Credit Bypass Route、task_chain（≤ 64 项，含软件属性 `exe_dest` / `reduce_num`）、datain_task、`stream_num`、`CORE_TYPE`、`B_core_direction`、`trigger_task_chain_en`、DTE 包头表（硬件包头静态表 64 项、软件包头 16 × 64 项）、MU `local_ep_table`、VU 8 组静态配置、Core Mem 的 reissue 预留空间 | 编译侧 |
| kernel 镜像 | 每类 core 一个 RV32 ELF（代码段进 ITCM、数据段进 DTCM），与 task_pc → kernel 入口地址表 | 编译侧 |
| 数据 | 每 core 27 MiB 权重分片（含共享专家）与落 Matrix Mem 的地址；注入表（每 token 的 6368 B 级联包与注入拍）；参考实现的期望输出 | 编译侧 + `reference/` |

编译侧产物里必须有、不能反推的几样：每个 core 的 RouterTable（同一 path_id 在不同 core 上表项不同）、每个 core 的 task_chain 与 datain_task、每 core 的权重分片与角色、每 chip 的形状与角色分配、`CreditCounter[path_id][stream_id]` 初值（广播 = 目的 core 数，P2P = 1）。

参数表 `common/params.h` 是全部拍数、带宽、深度的唯一出处，每个值标注来历：MAS 给的、性能需求规格说明书给的、第 8 章冲突项按“建议”取的、本章“待定”默认值。

### 十张只读表

四类东西在模型里落成十张只读的普通内存，不打拍，不进波形。每张表由哪个单元持有，写在该单元文档的“存储器”一章。

```
grid          48 × {tray, layer, col, gx, gy}                                   LPU
chip_shape    48 × {中间列 2×4, 第一列 2×5, 最后一列 2×5}                        LPU
logical_map   48 × 10 × {logical_core, role}                                    LPU
split_param   {ep, tp, pp, dp, mode, gpu_num, batch}                            LPU
core_cfg      48 × 10 × {rtab 64 项, skip_mask, credit_bypass, task_chain 64 项,
                         sw_attr 64 项, path_task_map 64 项, datain_task, cfg_misc,
                         cmem_part, lut 64 项, local_ep_table, vu_static 8 组}   各单元
credit_init   48 × 10 × 每 {path_id, stream_id} 一个初值                        TS
kernel_img    每类 core 一个 {itcm 字节流, dtcm 字节流, task_pc 表 64 项}        RV core
weight_shard  48 × 10 × {字节流, 落 Matrix Mem 的地址}                          入口桩
inject_tbl    N_token × {inject_cycle, gpu_id, token_id, payload 6368 B}         入口桩
expect_out    N_token × 12 KiB                                                  出口桩
```

### 读入时的跨表自洽检查

这些断言在构造期做完，不逐拍。它们查的是**跨表**的一致性 —— 单张表内部的合规性由持有它的单元自己查（例如 `task_chain` 的四项检查在 TS 写 `TS_INIT_FINISH` 时做）。跨表这一层没有哪个单元能独自看到，因此收在这里：

1. `grid` 覆盖 48 项且 `(gx, gy)` 无重复；`gx ∈ {0, 3}` 的 chip 是 2×5、其余是 2×4；每 chip 都是 8 个计算 core
2. 每 chip 的 `logical_map` 里逻辑 0～7 各出现一次，逻辑 8 只在 `gx ∈ {0, 3}` 出现，且 special 与 compute 的物理 core 集合不相交
3. `cmem_part` 的各分区互不重叠，且都落在 Core Mem 的 1 MB 之内
4. `task_chain` 里出现的每个 `path_id`，在本 core 的 `rtab` 与 `path_task_map` 里都有 valid 表项，且 `path_task_map[path_id].task_id` 指回配它的那一项
5. `rtab` 里 `stall_way` 选转存的表项，本 core 的 `task_chain` 里必须有对应的 reissue 任务，且 `cmem_part` 里 `reissue_pkts_per_vc` 不为 0
6. 不派角色的 core 的 `rtab` 表项一律不置 Core 位、`stream_table_enable` 全不置位、`stall_way` 只能是留在 VC

第 4、5 两条是软件检查清单里“选进 Core Mem 重发的 path 必须预留空间并安排 reissue 任务”“不派角色的 core 只能选留在 VC 等待”的机器化形式。三份 RouterTable 一致这一条不在这里查，由 SCP 桩的写入顺序保证。

### 表怎么分发

| 阶段 | 做什么 |
| - | - |
| 构造期 | `grid` / `chip_shape` / `logical_map` / `split_param` 交给 LPU 与各 Chip，决定构造出什么 |
| boot 期 | `core_cfg` / `credit_init` / `kernel_img` 变成 SCP 桩的配置事务序列，按初始化六步的顺序发出，每笔一拍 |
| weights 加载模式 | `weight_shard` 由入口桩按“最远路径优先”的顺序注入，走 Router 的 weights path |
| 业务模式 | `inject_tbl` 按 `inject_cycle` 注入，`expect_out` 交给出口桩 |

***

## 时间轴与链路

Clock 周期取 1 T，即 1 GHz 下的 1 ns，一拍就是一 T。第 5 章的参数凡是按 T 给的直接变成拍数；按 ns 或 μs 给的（PCIe C2C 300 ns、ETH 注入 3 μs）按 1 T = 1 ns 折算。

拍数换算统一为 `ceil(size / bw)`，且 size 小于等于 0 时算一拍。

latch 的 `Time` 有效范围是 32 位，1 T 一拍下约 4.29e9 拍。一层 FFN 的端到端延迟在 1e4 到 1e5 拍量级，余量充足。

发送侧按链路带宽与延迟算出到达时刻写进 flit 的 `arrive_cycle`，链路模块把 flit 放进自己的在途队列，只有当前拍不早于 `arrive_cycle` 才把它送到出口端口。端口固有的一拍延迟被吸收，时序 owner 仍然唯一是发送侧的链路模型，不额外扣时。R2R 40T、C2C 400T 都远大于一拍，吸收得下。同一条链路上到达时刻单调递增，因为发送侧的占用时刻单调，所以队首不会阻塞更早到达的包。这一条加断言。

***

## 输出与观测

波形走 latch 原生的 Trace 与 insight，层次为 `chip<i>.core<j>.<单元>.<模块>.<信号>`。

另出一个目录，三个事件桶加一份来源信息：

| 文件 | 内容 |
| - | - |
| `unit_spans.jsonl` | 单元占用区间：node_id、unit、user_id、task_id、start、end、state、volume |
| `unit_waits.jsonl` | 等待区间、归因 reason，以及它属于依赖还是资源 |
| `global_latency.jsonl` | 每个 `(gpu_id, token_id)` 的端到端延迟、经过的 core 序列与每段时刻 |
| `run_meta.json` | 编译侧产物的来源、参数快照、结束时刻、完成与丢失的 token、credit 终态 |

`run_meta.json` 不是可选的。没有它，一份观测产物没法追回是哪套切分、哪套参数跑出来的，结论就没有适用范围。

等待归因按模块细化，分依赖与资源两类：

| 类 | reason |
| - | - |
| 依赖 | 等前序 task 完成、等 datain 数据到达、等 Router `reduce_done`、等 DSA 读寄存器返回、等 Completion RS 的 Join、等 Scoreboard 依赖、等 B core 的 head ≠ tail、等 R core 的 arrive_num == 2 |
| 资源 | 等 stream 坑、等 VC credit、等 Reduce credit、等入口桩的 grant、等 TaskQueue / ISQ / issue_q 项、等 Lane、等 bank 端口、等 RV core 空闲、等 dsa_iss 通道、等 DTE Buffer credit、等 Xbar 出口、等 ReduceModule 上下文 |

瓶颈由此能定位到具体资源而不只是慢，而且只有资源那一半是改参数动得了的。

### 原始文档指名要模拟器回答的三个问题

这三处在原始设计文档里写明了“需要模拟器介入”，是本次建模的交付目标，不是可选的观测项：

| 问题 | 出处的原话 | 要给出的结论 |
| - | - | - |
| VC 机制到底实不实现 | “实现 VC 机制需要很大的额外面积、设计复杂度和验证空间，成本极高。具体是否实现需要模拟器介入，综合判断开发复杂度和效果收益” | 分别跑“阻塞就进 core 暂存”与“Router 内加 VC Buffer”两套配置，比端到端吞吐、DTE 占用率与 Core Mem 占用 |
| Broadcast 过快引起的计算空泡有多大 | “此处产生空泡的前提是第 N 个用户在 core0 和 core2 的处理速度不同……在计算量分布均匀的前提下，时间差距主要来自逐级 Reduce。需要模拟器介入协助确认” | 量出同一 TP 组内各 core 的进度差，判断超前发送窗口 N 该取多大 |
| 包效率与总线对齐效率的实际影响 | “为了面积和效率折中，可以考虑支持 32 B 对齐传输（初步结论是不实现，再算子切分时考虑尽可能不要切出来这么小的包）” | 按激活专家数的实际分布跑，量出只支持 256 B 对齐时损失多少有效带宽 |

前两个都要求模型能跑到稳态并看得见 core 之间的进度差，第三个只要求链路上的字节数按“业务数据 + 32 B 包头、再按对齐规则向上取整”记账。

两条发射端的过滤规则：占用区间丢弃 end 小于 start 的，以及 end 等于 start 且没有显式允许零长的；等待区间丢弃 end 不大于 start 的。被丢的条数单独计数，用来区分“没记到”与“没发生”。

事件在仿真期记在各模块自己的裸 vector 里，`JoinAll()` 之后由主线程汇总落盘。`Cycle()` 里不做文件 IO。汇总时按节点与时刻排全序，否则收集顺序取决于线程调度，产物无法逐行 diff。

***

## 建模顺序

按依赖自下而上建，每一步都能单独跑一个用例再进下一步。

| 步 | 建什么 | 跑通的判据 |
| - | - | - |
| 1 | `common/`（flit、message、params、arbiter、numeric）与 `ip/module_base.h` | 四种仲裁器的单测；`numeric/` 与 `reference/` 的编解码逐 bit 对齐 |
| 2 | 链路、PCIe Switch、入口桩与出口桩 | 一个 flit 从入口桩发出、经链路与 Switch 回到出口桩，到达拍与手算一致 |
| 3 | 三块存储与它们的 bank 仲裁器 | 各 master 端口的端到端拍数等于参数表；同 bank 冲突按优先级授予 |
| 4 | Router 的八个模块 | Router 单测跑 A2、A5、A15、A16、A17 五个场景；credit 守恒 |
| 5 | RV core（接 `src/rv32`）与三块存储、Router I/O reg 的连接 | 一个只做标量活的 kernel 跑完并向 TS 报完成 |
| 6 | TS 的九个模块 | 单 stream 单 task 从 trigger 到 retire 走完；六个写口的冲突用例 |
| 7 | DTE 的八个模块 | 五个搬运方向各一个用例；Commit 配对接纳与 Join 的用例 |
| 8 | MU 与 VU | 逐条计算原语与参考实现逐 bit 比对 |
| 9 | Core 装配、Chip 装配（含 SCP 桩、ctrl_noc 端点、C2C Bridge） | 单 chip 上 boot 走完六步，`ready` 全高 |
| 10 | LPU 装配与静态表读入 | 48 chip 构造出来，拓扑结构那一类检查全过 |
| 11 | 端到端 | 注入一个 token 收到一个结果并逐 bit 相等，再放大到 N 个 |
| 12 | 四种切分模式各跑一遍 | EPTP-NN 先通（每个 core 任务链相同），再 EPTP-NK 的三种角色，再 PPTP-NK 与 PPTP-NN 的三段 chip。四种模式的任务链见 TS 那一份文档 |

第 4 步之前不碰 TS 与 DSA：Router 是唯一一个不派角色的 core 上也要构造的单元，它先立住，后面每一步都能拿它当数据源与数据汇。

***

## 死锁避免

模型的第一要求是跑得完：注入 N 个 token，出口收到 N 个结果。设计里挡死锁的机制分散在各章，这里按“一个包从进来到出去要过的几道资源”排成一列，每一条都落到某个单元的功能编号上。

### 十二条硬规矩

| # | 规矩 | 落在哪 |
| - | - | - |
| 1 | 每个 VC 有 private 20 flit，队头永远能前进一步，不靠共享池。credit 也按 private 与 shared 两级记，与下游 buffer 的占用规则一一对应，一个方向的总量等于下游容量，不超发 | RouterStation 的 VC Buffer 与两级 credit |
| 2 | 相互依赖的数据流分到不同 VC，避免循环等待 | `RouterTable.nxt_vc` 的填法，编译侧保证 |
| 3 | credit 不足的 VC 被跳过，同一 input port 的其他 VC 不受影响 | RouterStation 的 VA |
| 4 | 多播全有或全无。只发一半会让同一 User 的数据在不同分支上错位，已发方向占了资源却完不成整体传输 | RouterStation 与 Xbar |
| 5 | Router 的进 core 表与 TS 内部的 Stream 表按完全一致的逻辑分配空项，因此“Router 通知 TS 的包一定能被 TS 接收” | Router 的 CoreMemCreditMonitor 与 TS 的 `task_state_update.credit` |
| 6 | 拿不到下游资源时二选一：留在 VC 等，或转 Core Mem 重发。选后者必须为它预留 Core Mem 空间并在任务链里安排 reissue 任务；不派角色的 core 没有 Core Mem，只能留在 VC，因此 path 规划要保证这一段不会长期阻塞 | `RouterTable.stall_way` 与 CoreMem 重发 |
| 7 | P2P 传输阻塞时把数据落进 Core Mem 的 P2P 阻塞缓冲，下游 credit 释放后再续传 | TS 的 P2P 阻塞缓冲映射表 |
| 8 | DTE 的 Commit 配对接纳：RD、WR 两个 TaskQueue 项与 Completion RS 项同时拿到才收，不产生读已开始、写没有落脚点的半任务 | DTE 的 Commit |
| 9 | DTE 的出核任务先在 `PendingTaskQ` 等到资源授权，再去 Commit 申请那三样，等资源的任务不占 Completion RS | DTE 的 PendingTaskQ |
| 10 | EP 组间派遣：所有 R core 都有余量才派遣一个用户，派时各减一，链尾返回后各加一 | 入口桩的 LPU Dispatch |
| 11 | 一条“广播 + P2P + 广播”的路径上，各 core 的 Core Mem 能容纳的用户数**沿数据流方向不能变少**：一致或前窄后宽。满足这一条时 User N 的回程一定排在 User N+4 的去程之前，不会成环 | 编译侧的 `cmem_part` 分配，模型在 boot 期校验 |
| 12 | ReduceBuffer 不得当流控缓存用。Reduce 结果发不出去时进本 core 的 Core Mem，不许压在 ReduceBuffer 里 | ReduceModule 的输出准入 |

### 反压不许变成丢弃

上面十条都靠反压兜底，反压的前提是没有任何一段通路把请求丢掉。三处特别容易写成丢弃：

* Router 收满一个包后通知 TS 的 trigger 与 token 一一对应，TS 的入口占满时 CoreStation 保持本笔请求，不丢
* Matrix Mem 同 bank 冲突只执行 MU，被让路的那一笔丢弃并计数，**模型直接断言失败**。这是硬约束被违反的表现，不是正常工作点：DTE 没有重传机制，丢一笔就少一段数据，用重试掩盖会让配置错误一直查不出来
* MU 的 Drain 只丢越界任务的数据，已进入脉动通路的合法数据照常算完写回

### 运行期要守的三条

跑的过程中每拍都成立，破坏了就是死锁的前兆，比跑完之后查 credit 守恒早得多：

1. **资源的持有与等待不成环**：任何一个模块在等某个资源时，不得同时持有该资源的上游还要用的资源。Commit 的配对接纳与 PendingTaskQ 排在 Commit 之前，是这一条在 DTE 上的两个落点
2. **每个等待都有唤醒源**：`unit_waits` 里的每条等待区间都能配上一个把它唤醒的事件。等待归因表里的 reason 分依赖与资源两类，资源类的唤醒源是对应的 release 或 grant，依赖类的唤醒源是对应的完成事件
3. **没有任何一路请求被无限期饿死**：同优先级按先到先得排队，stream_table 的六个写口把回收类排在生成类之前，Xbar 每拍重新 RoundRobin

### 怎么查

三样自动检查，跟在四条不变量后面一起跑：

| 检查 | 做法 |
| - | - |
| 全局看门狗 | 连续 K 拍没有任何 flit 前进、没有任何 task 状态变化，就判定卡死，打印各单元当前在等什么。K 取端到端延迟上界的十倍 |
| 等待时长上界 | `unit_waits` 里任何一条等待区间超过阈值就单独列出，阈值按该资源的最长合法往返定 |
| 环检测 | 卡死时按“谁在等谁的资源”建一张图，找环。图的边由等待归因的 reason 与该资源的持有者给出 |

***

## 验收

### 四层判据

由粗到细：

1. 结果一致。注入 N 个 token，出口收到 N 个结果，逐 bit 等于参考实现。
2. 不变量成立。四条不变量在每个阶段都查，任何一条被破坏都说明模型有结构性错误。
3. 逐段时间手算。单 user 单 stream 下，入口桩注入加链路加 TS 派发加 RV core 配置加 DSA 启动加访存加逐跳链路时间加计算时间，与模型输出逐段对上，每一段的取值都能在第 5 章或参数表找到。
4. 等待归因。`unit_waits` 的 reason 分布能解释吞吐与延迟的差距，资源类等待能通过改参数消掉。

### 检查项

验收分五类，每一类都要有能自动跑的检查，跑完一轮全部通过才算这一阶段结束。

| 类别 | 检查项举例 |
| - | - |
| 拓扑结构 | chip 数 = 48，摆成全局 12 × 4；`gx ∈ {0, 3}` 的 chip 10 个 core、其余 8 个，每 chip 都是 8 个计算 core；同层左右与同列上下直连，跨 tray 的两处换纵向链路参数；每层两端 chip 接 PCIe Switch；边界 core 的 C2C 连接与各自形状的规则一致；第一列 chip 的 `core0` 是 B core、`core5` 不派角色，最后一列 chip 的 `core9` 是 R core、`core4` 不派角色 |
| 数据流正确性 | **Credit 守恒**（初始 + 归还 = 消费 + 余额，无泄漏）；flit 组装正确；topK 正确传递；MU / VU 计算次数与预期一致；每 core 的 user init 数量符合预期 |
| 边界与异常 | Stream 耗尽正确排队；Credit 耗尽正确阻塞上游；ready 拉低正确背压；无效 path_id / 重复 User ID 被拒 |
| 时序行为 | 各启动延迟等于配置值；R2R 单跳 40T，跨 chip 400T；同一 user 的 task 之间先后与任务链一致；bank 冲突时按优先级授予；VU 两条宏指令重叠、MU 三段重叠可在波形上读出 |
| 性能指标 | 多用户吞吐与第 5 章“多用户吞吐”公式同量级；单 user 端到端延迟不低于理论下界；各单元占用率不超过 100%；RV core 每 task 的标量拍数不超过预算 |

四条不变量：

1. **credit 守恒**：一轮跑完后，所有方向、所有类型的 credit 计数回到初值。
2. **相同 `task_id` 在不同 user 之间时序无交叠**。
3. **每个目的 core 对同一个 user 的同一份数据只落一次**。
4. **注入 N 个 token，出口收到 N 个结果**，且逐 bit 等于参考实现。

Router 的验收场景 A1～A17 逐条列在 Router 那一份文档的“验收场景”一节，下面四个直接覆盖了最易实现错的语义，先跑这四个：

| 场景 | 查什么 |
| - | - |
| A2 | bypass 中间核不占用户坑 |
| A5 | 同用户二次发送余额不减 |
| A15 / A16 | 单个与多个只透传的 core 串联时的 credit 透传 |
| A17 | reduce 完成 Ack 归 Router；缺 Ack 时任务链停在 reduce 处不前进 |

### 最容易实现错的几条

1. **Reduce 任务的完成判定**：DTE ACK 只代表搬运完成，**只有 Router Reduce Done 才能把任务置 FINISH**，且两者可任意顺序到达。判错会让 reduce 链提前推进。
2. **异步 datain 只更新 done_bitmap，不推进 task_id**。这是任务链能乱序完成又保持顺序语义的关键。
3. **Retire 的三方时序**：Core 保证不再有该 user 的搬运 → Router 立即删表项 → ReduceModule 延迟到 credit 全恢复才删。
4. **多播原子准入**：多播的每个 flit 必须同时取得所有目标方向资源才能发，任一方向不足则所有分支统一等待。不能让分支独立前进。
5. **进 core 后锁定到尾 flit**，而 Router-to-Router 可以在 flit 边界切换 Packet。两种粒度不能混用。
6. **Mmem 同 bank 不能有两个 master**，冲突时只执行 MU。
7. **VU 的 CM 访存冲突硬件不追踪**，靠软件的 `MACRO_INST_FENCE`。建模时若默认硬件会挡，结果会偏乐观。
8. **DTE 的配对接纳**：RD、WR 两个 TaskQueue 与 Completion RS 三项同时可用才接纳，不产生半任务。
9. **bit 级的累加顺序**：MU 的 CSA 树按 scale block 分组累加、Router reduce 按到达顺序 FP32 累加、VU 归约按 LANES 内归约再 ⌈log2 SEG⌉ 级累加。参考实现必须用同一顺序。

### 测试写法

驱动本身是 `ClkModule`，通过对方声明的端口交互；至少一个 `sub_thread` 大于 1 的并发用例；每个用例连跑二十次无 flake；断言的量在协程内 snapshot，不在 `JoinAll()` 之后读 `Logic64`。机制覆盖表里的每个用例名对应一个测试函数，覆盖率以“机制覆盖表里没有用例为空的行”为准。

***

## 边界与风险

### 简化

* **RV core 不建流水线**：kernel 是真实 RV32 程序，每条指令 1 拍，访存与 DSA 读的延迟记在 gpr 就绪表上；双发射、分支预测、流水冲刷、乘除多拍、DTCM bank 冲突、gpr 端口竞争不体现。
* **异常、ECC、看门狗、功耗类机制不建**：各单元留状态位与接口名。
* **TS 直接启动 DTE、DTE 的 3 Lane 方案、DSA-RF 调试通路不建**。
* **Router 的输出移位拼接不建**：flit 定长 256 B，尾 flit 带有效字节数。
* **时间常数未校准**：第 5 章标“偏小”“带问号”的值与第 8 章的冲突项都是配置值，支持同参数下的相对比较，不是绝对性能预测。

### 设计未给值、本章填了默认值的参数

全部标“待定”，在 `params.h` 里集中登记，向设计方要到值后只改参数表：

| 参数 | 默认值 |
| - | - |
| Router VC Buffer 深度 | private 20 flit / VC 加每方向 shared pool 20 flit（防死锁下限 2，软件可配）|
| Stream Resource Table 项数（每方向） | 16 |
| VC credit 初值 | private 每 VC 20，shared 每方向 20，先扣 private 再扣 shared |
| RouterTable 表项数、副本数、每副本写入拍数 | 64、5、1 |
| Xbar 与 ReduceModule 三路输入的仲裁算法 | 轮询 |
| ReduceModule Entry credit、bank 数、RMW 拍数、输出队列深度 | 64 flit、4、2、8 |
| CoreStation HeaderFIFO、OutputBuffer 深度 | 16、32 flit |
| DTE TaskQueue、Buffer、Completion RS、Done Pending 深度 | 16、16 × 256 B × 2、16、16 |
| VU ISQ 深度 | 8 |
| Share Mem 四个 master 的仲裁算法 | 轮询 |
| RV core task_queue 深度 | 2 |
| TS stream_table 六个写口的优先级 | retire > done > install > issue > wake > create |
| `reduce_in_mask` 的逐核取值 | 按 path 图推导；`core4` 不派角色那个例子里的值等 Reduce0 / 1 / 2 含义定下后回填 |
| Core Mem 后三个 master 的优先级 | 三者平级，先到先得（前两档 MU > VU = DTE 由设计给定） |
| trigger 请求里 `compute` 位在包头中的位置 | 等 Router 接口规范定下包头位域后回填 |
| `dsar` 与 `dsari` 的区分位 | 《ISA 描述表》给了九条自定义指令的完整编码，逐条见 RV core 那一份文档。只有这两条的编码在表里完全相同，模型按其余四条的规律用 bit31 区分 |
| MU、DTE 的寄存器地址映射 | `regmap.h` 临时映射 |
| Mmem MU 读延迟 | 8T |
| Cmem 的 MU 写延迟 | 16T |
| `operation` 的 Reduce0 / Reduce1 / Reduce2 含义 | 源分量 / 中继累加 / 最终汇聚 |
| `exe_dest`、`reduce_num` 的承载 | task_chain 的软件侧属性 |

### 风险

**规模。** 每拍一次全局 barrier，同步点数量等于模块数。48 chip 按每 core 约五十个模块是两万多个协程（槽位上限 65535），每拍开销未实测。应对写在 `module_base.h`：先在单 chip 上测出每拍开销，过大就让 Core 或 Chip 挂时钟、在它的 `Cycle()` 里顺序调各模块的 `Step()`，模块代码不动；因为跨模块信号全部打拍，两种驱动方式的结果逐拍相同。

**状态机改写的语义等价。** 把硬件里的等待写成跨拍状态机是工作量最大、也最容易引入语义差异的一块。每个状态机要有对照 MAS 时序图的单测，尤其是 Reduce 完成的无序汇合、多播原子准入、Retire 的三方时序、DTE 的配对接纳与 Join 这四条跨阶段持有的语义。

**bit 级一致的累加顺序。** MU 的 CSA 树、VSFU 的查表拟合、VU 的归约树顺序在 MAS 里只给了原则没给细节。《数值与参考实现》为这几处各定了一个默认取法并标了待定，参考实现与模型按同一份 `numeric/` 对齐，与真实硬件是否一致要等 RTL 出来核对。

**握手多出的拍数。** 跨模块信号全部打拍，硬件里同拍完成的组合握手在模型里最少两拍。第 2 至 4 章给了时序图的接口按时序图核对拍数；没给的按注册 ready 建，并在等待归因里单列，便于校准。

***

本章依据第 2 至 6 章整理；建模方式、建模顺序与验收判据以本章为准。
