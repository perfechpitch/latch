# RV Core（DTE / MU / VU 各一）

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **RV core ×3**

给实现 RV core 的人：一个独立打拍的模块做哪些事、端口与存储怎么定。三个实例硬件相同、接口相同，区别只在绑定的 DSA、ITCM 里的镜像、可见的地址空间。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《Core 内硬件》“RV Core”全节：指令集、自定义指令、task 下发与完成、dsa_iss 的下发规则、LSU 与访存分流
* 《软件栈》：“每个 task 的共同形状”“各类 core 的软件流程”

***

## 1　定位与边界

RV core 是 TS 与 DSA 之间的桥梁：从 TS 收 task，按 `task_pc` 跑 ITCM 里的 kernel，配置 DSA 执行任务。它跑两类指令：

* **custom-0 自定义指令**：配置对应的 DSA、读 DSA 寄存器、结束 task、查映射表、循环
* **普通 load / store**：访问 DTCM、Share Mem，以及只有 DTE core 有的 Core Mem 与 Router I/O reg

指令逐条执行，不建流水线：`src/rv32` 的 `SystemRv32` 提供 RV32IMC、M 态 CSR 与译码，本模块覆盖 `Decode` 接入自定义指令，外面包一层 task_queue、dsa_iss、dsa_rq、lsq 与 gpr 就绪表做逐拍记账。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1380 860" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs>
    <marker id="a" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#475569"/></marker>
    <marker id="as" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#475569"/></marker>
    <marker id="g" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0f766e"/></marker>
    <marker id="gs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0f766e"/></marker>
    <marker id="o" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#b45309"/></marker>
    <marker id="os" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#b45309"/></marker>
    <marker id="p" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#7c3aed"/></marker>
    <marker id="ps" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#7c3aed"/></marker>
    <marker id="i" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#4338ca"/></marker>
    <marker id="is" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#4338ca"/></marker>
  </defs>
  <rect x="0" y="0" width="1380" height="860" fill="#ffffff"/>
  <text x="20" y="26" font-size="12" fill="#111827">RV core · 第 0 层</text>
  <text x="185" y="26" font-size="9.5" fill="#6b7280">DTE / MU / VU 各一个实例，硬件相同、接口相同；黄色虚线框内是折算成每条指令 1 拍、不建流水线的部分</text>
  <polygon points="40,116 190,116 181,146 31,146" fill="#f8fafc" stroke="#374151"/>
  <text x="111" y="135" font-size="9" fill="#374151" text-anchor="middle">task_cmd / task_ack</text>
  <polygon points="40,742 190,742 181,772 31,772" fill="#f8fafc" stroke="#374151"/>
  <text x="111" y="761" font-size="9" fill="#374151" text-anchor="middle">rv_done → TS</text>
  <rect x="250" y="84" width="280" height="142" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="262" y="105" font-size="11" fill="#111827">task_queue</text>
  <text x="262" y="122" font-size="8.5" fill="#475569">提前接收 TS 下发的 task，做到用户之间</text>
  <text x="262" y="135.5" font-size="8.5" fill="#475569">　task 的无 bubble 调度</text>
  <text x="262" y="149.0" font-size="8.5" fill="#475569">按是否有空槽产生 task_ack；未被接收时</text>
  <text x="262" y="162.5" font-size="8.5" fill="#475569">　TS 不能释放该 task 跳到下一个</text>
  <text x="262" y="176.0" font-size="8.5" fill="#475569">队头 task 的 task_pc 驱动取指</text>
  <text x="262" y="189.5" font-size="8.5" fill="#475569">深度 2（待定）</text>
  <rect x="250" y="286" width="560" height="238" fill="#fefce8" stroke="#a16207" stroke-dasharray="5 4" rx="4"/>
  <text x="262" y="307" font-size="11" fill="#111827">指令执行器（src/rv32 的 SystemRv32）</text>
  <text x="262" y="324" font-size="8.5" fill="#475569">RV32IMC，只支持 M 态，实现 M 态 CSR，不支持 S / U / H；fence 实现为 nop</text>
  <text x="262" y="337.5" font-size="8.5" fill="#475569">覆盖 Decode 接入 custom-0 自定义指令：</text>
  <text x="262" y="351.0" font-size="8.5" fill="#475569">　dsar / dsari 读 DSA 寄存器（不会被阻塞）</text>
  <text x="262" y="364.5" font-size="8.5" fill="#475569">　dsaw.s / dsaw.d / dsawi.s / dsawi.d 写 DSA 寄存器</text>
  <text x="262" y="378.0" font-size="8.5" fill="#475569">　task_done（带 TS 标志位）· flag_check · loop</text>
  <text x="262" y="391.5" font-size="8.5" fill="#475569">每条指令 1 拍；访存与 DSA 读的延迟记在 gpr 就绪表上</text>
  <text x="262" y="405.0" font-size="8.5" fill="#475569">不建流水线：pc_gen / loop_bp / decode / dispatch / 双发射 /</text>
  <text x="262" y="418.5" font-size="8.5" fill="#475569">　gpr 端口 / SEU 的乘除多拍 / DTCM 的 bank 冲突都折算成 1 拍</text>
  <text x="262" y="432.0" font-size="8.5" fill="#475569">复位后按 io_reg 的 boot_pc 启动；收到 task 后按 task_pc 起始执行</text>
  <text x="262" y="445.5" font-size="8.5" fill="#475569">task_done：队列有待执行 task 则跳到队头 task 起始 PC，</text>
  <text x="262" y="459.0" font-size="8.5" fill="#475569">　否则阻塞取指等待；带 TS 标志时通知 TS</text>
  <rect x="880" y="286" width="250" height="104" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="892" y="307" font-size="11" fill="#111827">gpr 就绪表</text>
  <text x="892" y="324" font-size="8.5" fill="#475569">32 × 32 bit</text>
  <text x="892" y="337.5" font-size="8.5" fill="#475569">DSA 读返回与访存返回未到时</text>
  <text x="892" y="351.0" font-size="8.5" fill="#475569">　把对应寄存器标为未就绪</text>
  <text x="892" y="364.5" font-size="8.5" fill="#475569">读到未就绪的源就等</text>
  <rect x="880" y="420" width="250" height="104" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="892" y="441" font-size="11" fill="#111827">CSR</text>
  <text x="892" y="458" font-size="8.5" fill="#475569">M 态 CSR + 自定义 CSR</text>
  <text x="892" y="471.5" font-size="8.5" fill="#475569">stream_id 只读（4 bit）</text>
  <text x="892" y="485.0" font-size="8.5" fill="#475569">local_user_id 可读写（12 bit）</text>
  <text x="892" y="498.5" font-size="8.5" fill="#475569">由 ctrl_noc 直接配置，不经流水线</text>
  <rect x="1180" y="84" width="170" height="180" fill="#f5f3ff" stroke="#7c3aed" rx="4"/>
  <text x="1192" y="105" font-size="11" fill="#111827">ITCM / DTCM</text>
  <text x="1192" y="122" font-size="8.5" fill="#475569">ITCM 4 KB，8 B/T，1 拍</text>
  <text x="1192" y="135.5" font-size="8.5" fill="#475569">　firmware · kernel</text>
  <text x="1192" y="149.0" font-size="8.5" fill="#475569">　· bootloader</text>
  <text x="1192" y="162.5" font-size="8.5" fill="#475569">DTCM 8 KB，32 bit × 4 bank</text>
  <text x="1192" y="176.0" font-size="8.5" fill="#475569">　BSS 段 · 寄存器溢出 · 堆栈</text>
  <text x="1192" y="189.5" font-size="8.5" fill="#475569">由 ctrl_noc 装载</text>
  <text x="1192" y="203.0" font-size="8.5" fill="#475569">装载拍数 = 字节数 / 4 B</text>
  <rect x="250" y="584" width="290" height="192" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="262" y="605" font-size="11" fill="#111827">dsa_iss / dsa_rq</text>
  <text x="262" y="622" font-size="8.5" fill="#475569">dsa_iss：DSA 调用指令下发通道</text>
  <text x="262" y="635.5" font-size="8.5" fill="#475569">　每拍最多一条配置或 trigger 指令</text>
  <text x="262" y="649.0" font-size="8.5" fill="#475569">　按下发通道是否反压阻塞判断是否下发成功</text>
  <text x="262" y="662.5" font-size="8.5" fill="#475569">dsa_rq：8 项，按顺序记录已下发的读指令</text>
  <text x="262" y="676.0" font-size="8.5" fill="#475569">　返回数据后按记录的目的寄存器编号写回 gpr</text>
  <text x="262" y="689.5" font-size="8.5" fill="#475569">DSA 读寄存器指令不支持同步读返回，</text>
  <text x="262" y="703.0" font-size="8.5" fill="#475569">　软件要查询状态只能轮询</text>
  <text x="262" y="716.5" font-size="8.5" fill="#475569">任务启动靠写 DSA 的 trigger 寄存器；</text>
  <text x="262" y="730.0" font-size="8.5" fill="#475569">　last 标志包含在 trigger 里</text>
  <rect x="870" y="584" width="300" height="192" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="882" y="605" font-size="11" fill="#111827">lsq（顺序发射）</text>
  <text x="882" y="622" font-size="8.5" fill="#475569">sm_lsq 16 项：Share Mem，5～10 拍</text>
  <text x="882" y="635.5" font-size="8.5" fill="#475569">cm_lsq 16 项：Core Mem，15～25 拍</text>
  <text x="882" y="649.0" font-size="8.5" fill="#475569">　只有 DTE core 有；Router I/O reg 复用它</text>
  <text x="882" y="662.5" font-size="8.5" fill="#475569">DTCM：4 bank 单端口 SRAM，3 拍</text>
  <text x="882" y="676.0" font-size="8.5" fill="#475569">　可同时接收 2 个不冲突 bank 的请求</text>
  <text x="882" y="689.5" font-size="8.5" fill="#475569">　同 bank 冲突则阻塞第二条</text>
  <text x="882" y="703.0" font-size="8.5" fill="#475569">访存带宽 32 bit</text>
  <text x="882" y="716.5" font-size="8.5" fill="#475569">写回优先级：share_mem / core_mem 优先于 DTCM</text>
  <text x="882" y="730.0" font-size="8.5" fill="#475569">DTE core 读 Core Mem 固定回 1056 bit，不 burst</text>
  <polygon points="620,660 780,660 771,690 611,690" fill="#f8fafc" stroke="#374151"/>
  <text x="696" y="679" font-size="9" fill="#374151" text-anchor="middle">dsa_cfg / dsa_rdata</text>
  <polygon points="1256,600 1352,600 1343,630 1247,630" fill="#f8fafc" stroke="#374151"/>
  <text x="1300" y="619" font-size="8.5" fill="#374151" text-anchor="middle">sm_lsq</text>
  <polygon points="1256,660 1352,660 1343,690 1247,690" fill="#f8fafc" stroke="#374151"/>
  <text x="1300" y="679" font-size="8.5" fill="#374151" text-anchor="middle">cm_lsq</text>
  <polygon points="1256,720 1352,720 1343,750 1247,750" fill="#f8fafc" stroke="#374151"/>
  <text x="1300" y="739" font-size="8.5" fill="#374151" text-anchor="middle">io_reg</text>
  <polygon points="1230,430 1350,430 1341,460 1221,460" fill="#f8fafc" stroke="#374151"/>
  <text x="1286" y="449" font-size="8.5" fill="#374151" text-anchor="middle">cfg（ctrl_noc）</text>
  <polyline points="190,131 220,131 220,124 250,124" fill="none" stroke="#0f766e" marker-start="url(#gs)" marker-end="url(#g)"/>
  <polyline points="390,226 390,256 373,256 373,286" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="810,324 845,324 845,338 880,338" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="810,491 840,491 840,615 870,615" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="373,524 373,554 395,554 395,584" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="540,661 576,661 576,675 611,675" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="1180,214 869,214 869,367 810,367" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-start="url(#as)" marker-end="url(#a)"/>
  <text x="970" y="262" font-size="8.5" fill="#6b7280" text-anchor="middle">取指 8 B/T，1 拍</text>
  <polyline points="1221,445 1176,445 1176,457 1130,457" fill="none" stroke="#7c3aed" stroke-dasharray="2 3" marker-end="url(#p)"/>
  <polyline points="1314,430 1314,347 1326,347 1326,264" fill="none" stroke="#7c3aed" stroke-dasharray="2 3" marker-end="url(#p)"/>
  <polyline points="1170,615 1247,615" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="1170,675 1247,675" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="1170,735 1247,735" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="250,457 220,457 220,757 190,757" fill="none" stroke="#0f766e" marker-end="url(#g)"/>
  <text x="200" y="724" font-size="8.5" fill="#0f766e" text-anchor="end">rv_done 由 task_done 指令产生</text>
  <text x="200" y="738" font-size="8.5" fill="#0f766e" text-anchor="end">带 TS 标志时通知 TS</text>
  <text x="560" y="556" font-size="8.5" fill="#6b7280" text-anchor="start">gpr 就绪表承载访存与 DSA 读的延迟</text>
  <text x="20" y="820" font-size="10.5" fill="#374151">三个实例的区别只在绑定的 DSA、ITCM 里的镜像、以及可见的地址空间：只有 DTE core 有 cm_lsq 与 Router I/O reg，Matrix Mem 对三个 RV core 都不可见。</text>
  <text x="20" y="842" font-size="10.5" fill="#374151">firmware 程序结束时要执行一条不通知 TS 的 task_done，等待业务流 task。</text>
</svg>
```

***

## 2　功能清单

一功能一条，编号供“机制覆盖”一章引用。

### task_queue 与 task 下发

| 编号 | 功能 |
| - | - |
| F1 | 提前接收 TS 下发的 task，前一个 task 完成后立刻执行队头缓存的那个，做到用户之间 task 的无 bubble 调度 |
| F2 | 按 task_queue 是否有空槽产生 `task_ack`；未被接收时 TS 不能释放该 task 跳到下一个 |
| F3 | 下发信息四个字段：`task_pc` 是起始取指 PC；`stream_id` 4 bit，用于算该用户的 Core Mem 与 Share Mem 区域基址，硬件写入自定义 CSR 且只读；`local_user_id` 12 bit，用于 R core 用户映射表和 Matrix Mem 地址计算，可读写，R core 执行 `flag_check` 后由软件写入 |
| F4 | 完成信息三个字段：`stream_id`、`local_user_id`、`task_id`。`task_id` 6 bit 只读，异步 datain 任务由软件识别包头后写入，用于告诉 TS 是任务链中哪一步完成 |

### 指令执行

| 编号 | 功能 |
| - | - |
| F5 | 指令集 RV32IMC：I 基本指令集、M 整型乘除法、C 压缩指令集；不支持 F 与 D，A 考虑支持 |
| F6 | 特权级只支持 M 态，实现 M 态 CSR，不支持 S / U / H；`fence` 指令实现为 nop |
| F7 | 每条指令 1 拍。不建流水线：pc_gen、loop_bp、decode、dispatch、双发射、gpr 端口、SEU 的乘除多拍、DTCM 的 bank 冲突都折算进这 1 拍 |
| F8 | 复位后按 io_reg 的 `boot_pc` 启动；收到 TS 下发的 task 后按 `task_pc` 起始执行 |
| F9 | 自定义指令 `dsar` / `dsari`：读 DSA 寄存器，地址分别来自 rs1 与立即数 `reg_addr1[4:0]` |
| F10 | 自定义指令 `dsaw.s` / `dsawi.s`：写 1 个 DSA 寄存器；`dsaw.d` / `dsawi.d` 一次写 2 个。按 RV Core MAS 的“每条最多配置 1 个 DSA 寄存器”建模，`dsawi.d` 先当两条 `dsawi.s` |
| F11 | 自定义指令 `task_done`：通知当前 task 完成。队列有待执行 task 则跳转到队头 task 起始 PC，否则阻塞取指等待；带 `TS` 标志时通知 TS。firmware 程序结束时要执行一条不通知 TS 的 `task_done`，等待业务流 task |
| F12 | 自定义指令 `flag_check`：从 Share Mem 的起始地址查到结束地址，找第一个 1 并把位置偏移量写回 rd，查到结束地址仍没找到则返回全 1。B core 与 R core 轮询软件映射表靠它 |
| F13 | 自定义指令 `loop`：rs1 是最大循环次数、rs2 是当前循环次数，rs2 ≥ rs1 时退出循环，imm 是分支偏移 |
| F14 | 寄存器分静态配置与动态配置：静态配置基本不随用户变化，初始化阶段配好、业务流阶段快速调用；动态配置随用户变化，跟随任务下发，含静态配置的选择 |
| F15 | 任务的启动靠写 DSA 的 trigger 寄存器；last 标志（该任务包是 task 的最后一个，DSA 执行完后通知 TS task 完成）包含在 trigger 寄存器里 |
| F16 | 性能约束：单个用户各 DSA 对应的软件调度程序在 RV core 上执行时间不超过 200 cycle |

### dsa_iss 与 dsa_rq

| 编号 | 功能 |
| - | - |
| F17 | dsa_iss 每拍最多下发一条配置或 trigger 指令 |
| F18 | DSA 任务配置下发指令按下发通道是否反压阻塞判断是否下发成功 |
| F19 | DSA 读寄存器指令不会被阻塞 |
| F20 | dsa_rq 8 项，按顺序记录已下发的读指令信息，返回数据后按记录的目的寄存器编号写回 gpr |
| F21 | DSA 读寄存器指令不支持同步读返回，软件要查询状态只能轮询 |

### gpr 就绪表

| 编号 | 功能 |
| - | - |
| F22 | 32 × 32 bit 的通用寄存器；DSA 读返回与访存返回未到时把对应寄存器标为未就绪 |
| F23 | 指令读到未就绪的源寄存器就等，等到写回才继续。访存与 DSA 读的延迟就记在这张表上 |

### lsq 与访存分流

| 编号 | 功能 |
| - | - |
| F24 | 按地址范围把访存分到四个目标：DTCM、Share Mem、Core Mem、Router I/O reg |
| F25 | DTCM 是 4 bank 单端口 SRAM、8 KB、32 bit × 4 bank，3 拍；可同时接收 2 个不冲突 bank 的请求，同 bank 冲突则阻塞第二条 |
| F26 | `sm_lsq` 16 项，访问 Share Mem，5～10 拍，顺序执行，每拍仅发一个读或写请求 |
| F27 | `cm_lsq` 16 项，访问 Core Mem，15～25 拍，顺序执行，每拍仅发一个请求；只有 DTE core 有 |
| F28 | Router I/O reg 复用 `cm_lsq`，仅 DTE core 需要 |
| F29 | 访存带宽 32 bit |
| F30 | 写回优先级：DTCM 读出数据与 Share Mem / Core Mem 数据同时需写回时，优先写回 Share Mem / Core Mem，阻塞 DTCM |
| F31 | DTE core 访问 Core Mem 的接口与其余通路不同：一次读请求固定读回 1056 bit，不支持 burst，按 32 bit / 拍返回；地址 18 bit，4 B 粒度；写请求带 4 bit 字节使能 |

### 存储与配置

| 编号 | 功能 |
| - | - |
| F32 | ITCM 4 KB，8 B/T，1 拍，存 firmware、kernel、DTE core 的 bootloader |
| F33 | DTCM 8 KB，存初始化 BSS 数据段、寄存器溢出与堆栈 |
| F34 | ITCM 与 DTCM 由 ctrl_noc 装载，装载拍数按镜像字节数除以 4 B 计 |
| F35 | CSR 由 ctrl_noc 直接配置，不经流水线 |
| F36 | 三个实例可见的地址空间：各自的 ITCM、DTCM、Share Mem、Core Mem 与对应 DSA 的 IO reg；Matrix Mem 对三个 RV core 都不可见 |
| F37 | 异常九类（Load / Store ECC Error、Load / Store Address Misaligned、Load / Store Access Fault、Fetch Address Misaligned、Fetch Access Fault、Fetch ECC Error、Illegal Instruction、Environment Call、Breakpoint），本轮只留状态位与接口名，不实现行为 |

***

## 3　接口

```
port task_cmd (slave, valid/ready, clk)           // TS → RV core：task 下发
  in  cmd_valid · task_pc[31:0] · stream_id[3:0] · local_user_id[11:0] · task_dsa_en
  out cmd_ready                                     // = task_queue 有空槽（raw ACCEPT）
port rv_done (master, 脉冲, clk)                  // RV core → TS：task_done 指令带 TS 标志时产生
  out valid · stream_id[3:0] · local_user_id[11:0] · task_id[5:0]
port dsa_cfg (master, valid/ready, clk)           // dsa_iss → 对应 DSA：配置写与 trigger
  out req_valid · req_we · req_addr[11:0] · req_wdata[31:0]
  in  req_ready                                     // = DSA 的配置通路未反压
port dsa_rdata (slave, 脉冲, clk)                 // DSA → dsa_rq：读寄存器的返回，异步
  in  valid · rdata[31:0]
port sm_lsq (master, valid/ready, clk)            // → Share Mem，32 bit
  out req_valid · req_we · req_addr[14:0] · req_wdata[31:0] · req_be[3:0]
  in  req_ready · rsp_valid · rsp_rdata[31:0]
port cm_lsq (master, valid/ready, clk)            // → Core Mem 与 Router I/O reg，只有 DTE core 有
  out req_valid · req_we · req_addr[17:0] · req_wdata[31:0] · req_be[3:0]
  in  req_ready · rsp_valid · rsp_rdata[1055:0]     // 读固定回 1056 bit，按 32 bit / 拍取用
port cfg (slave, ctrl_noc 写事务, clk)            // ITCM / DTCM 装载与 CSR 配置
  in  cfg_valid · cfg_addr[23:0] · cfg_we · cfg_wdata[31:0]
  out cfg_rdata[31:0]
port ready (master, 电平, clk)                    // 进 wait 状态后拉高，SCP 据此开放业务接收
  out ready
```

***

## 4　存储器

```
mem itcm        SRAM        4 KB，8 B/T，1 拍                                  1R1W  ctrl_noc 装载        复位未定义   // firmware · kernel · bootloader
mem dtcm        SRAM        8 KB，4 bank × 32 bit，3 拍                        1R1W  同 bank 冲突阻塞第二条  复位未定义   // BSS · 寄存器溢出 · 堆栈
mem gpr         FF 阵列     32 × 32 bit                                        —     由指令执行器读写      复位 0
mem gpr_ready   FF          32 b 就绪位图                                       1RW   发出访存 / DSA 读时清，写回时置  复位 全 1
mem task_q      FIFO        深 2 × {task_pc[31:0], stream_id[3:0], local_user_id[11:0], task_dsa_en}  1W1R  满 → task_ack 拉低  复位空
mem dsa_rq      FIFO        8 × {rd_idx[4:0]}                                  1W1R  顺序记录，返回时按序写回 gpr  复位空
mem sm_lsq      FIFO        16 × {we, addr[14:0], wdata[31:0], rd_idx[4:0]}     1W1R  顺序发射，每拍一个    复位空
mem cm_lsq      FIFO        16 × {we, addr[17:0], wdata[31:0], be[3:0], rd_idx[4:0]}  1W1R  顺序发射，每拍一个；只有 DTE core 有  复位空
mem csr         FF 阵列     M 态 CSR + 自定义 CSR                              1R1W  stream_id 只读，local_user_id 可读写  复位 0
mem pc          FF          {pc[31:0], state[2:0]}                             1RW   复位取 boot_pc；收 task 取 task_pc  复位 boot_pc
```

***

## 5　流水线总览

本模块不建流水线，第 1 层图只画 task_queue、指令执行器、dsa_iss / dsa_rq、lsq 四段的拍数记账关系，待这几处的拍数定下来后补。

***

## 6　逐级行为

第 2 层图与每级的四要素待第 1 层图完成后补，级编号回标到第 1 层图。

***

## 7　参数汇总

```
指令集          RV32IMC，只支持 M 态；fence = nop
每条指令        1 拍
ITCM / DTCM     4 KB / 8 KB
gpr             32 × 32 bit
task_queue      深度 2（待定）
dsa_rq          8 项
sm_lsq / cm_lsq 16 / 16 项
访存延迟        ITCM 1 拍 · DTCM 3 拍 · Share Mem 5～10 拍 · Core Mem 15～25 拍
访存带宽        32 bit；DTE core 读 Core Mem 固定回 1056 bit，按 32 bit / 拍
dsa_iss         每拍最多一条配置或 trigger 指令
内部启动延迟     MU 40T · VU 40T · DTE 85T（含 core 发射 5T）
性能约束        单个用户各 DSA 对应的软件调度程序执行时间不超过 200 cycle
custom-0 字段布局   funct3 按第 3 章表；rd / rs1 / rs2 / imm 按 R 型与 I 型标准布局（待定，等 ISA 描述表）
```

***

## 8　机制覆盖

| 机制 | 功能 | 用例 |
| - | - | - |
| task_queue 提前接收，做到用户之间无 bubble 调度 | F1 | `task_queue_prefetch` |
| 按空槽产生 task_ack，未接收时 TS 不能跳到下一个 | F2 | `task_ack_handshake` |
| 下发四字段与完成三字段，stream_id 只读、local_user_id 可写 | F3、F4 | `task_fields` |
| RV32IMC + 只支持 M 态 + fence 为 nop | F5、F6 | `isa_scope` |
| 每条指令 1 拍，流水线细节折算进这 1 拍 | F7 | `one_cycle_per_inst` |
| 八条 custom-0 自定义指令 | F9～F13 | `custom0_insts` |
| dsawi.d 按两条 dsawi.s 建 | F10 | `dsaw_double` |
| task_done 的三种行为：跳队头 / 阻塞等待 / 通知 TS | F11 | `task_done_inst` |
| flag_check 查第一个 1，查不到返回全 1 | F12 | `flag_check` |
| loop 指令按 rs1 / rs2 比较退出 | F13 | `loop_inst` |
| 任务启动靠写 trigger 寄存器，last 标志在 trigger 里 | F15 | `dsa_trigger` |
| dsa_iss 每拍最多一条，按反压判断是否下发成功 | F17、F18 | `dsa_iss_rate` |
| DSA 读不阻塞，dsa_rq 8 项按序写回 gpr | F19、F20 | `dsa_read_async` |
| 读寄存器不支持同步返回，软件轮询 | F21 | `dsa_poll` |
| gpr 就绪表承载访存与 DSA 读的延迟 | F22、F23 | `gpr_ready` |
| 访存按地址范围分流到四个目标 | F24 | `lsu_routing` |
| DTCM 可同时收 2 个不冲突 bank，同 bank 阻塞第二条 | F25 | `dtcm_bank` |
| sm_lsq / cm_lsq 顺序发射，每拍一个请求 | F26、F27 | `lsq_inorder` |
| Router I/O reg 复用 cm_lsq，仅 DTE core 有 | F28 | `io_reg_access` |
| 写回优先 share_mem / core_mem，阻塞 DTCM | F30 | `writeback_priority` |
| DTE core 读 Core Mem 固定 1056 bit，不 burst，32 bit / 拍 | F31 | `rv_cm_read` |
| ITCM / DTCM 由 ctrl_noc 装载，拍数按字节数 / 4 B | F34 | `tcm_load` |
| Matrix Mem 对三个 RV core 都不可见 | F36 | `no_mmem_visibility` |
| 复位取 boot_pc，收 task 取 task_pc | F8 | `boot_and_task_pc` |

***

## 9　取舍

* **为什么设 task_queue**
  * TS 与 RV core 之间有物理路径延时，“前一个 task 完成再通知 TS 下发下一个”会产生很长延迟
  * 提前接收把这段延迟藏在前一个 task 的执行里
* **为什么不建流水线**
  * 本轮关心的是 core 内的调度与访存排队，不是标量核自身的 IPC
  * kernel 是真实 RV32 程序，指令条数是真的；每条 1 拍加上访存与 DSA 读的真实延迟，已经能反映“配置耗时是否小于计算耗时”这一条约束
  * 代价是双发射、分支预测、流水冲刷、乘除多拍、DTCM bank 冲突、gpr 端口竞争都不体现
* **为什么 DSA 读寄存器不阻塞而配置写会阻塞**
  * 配置写要占 DSA 的配置通路，通路满了只能等
  * 读只是取一个状态，用 dsa_rq 记下目的寄存器就能异步返回，不必占住发射口
