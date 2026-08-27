# Chip

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → **chip**

给实现 Chip 装配、SCP 桩与 ctrl_noc 的人，三个对象各自的接线、端口、存储器、逐拍行为、参数与机制：

* **Chip**：装配容器，不打拍
* **SCP 桩**：独立打拍的模块
* **ctrl_noc 端点**：独立打拍的模块，每 core 一个

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《系统与部署》：“Chip 与 Harvest”“Boot 流程”“Chip 内地址划分”
* 《Core 内硬件》：“Bach Core 顶层”“对外通道”
* 《软件栈》：“部署阶段”“weights 加载模式”

***

## 1　定位与边界

Chip 的组成：2×5 的 Core 阵列，加边界的四个 C2C 端口、一个 SCP 桩、一条 ctrl_noc。48 颗 chip 怎么摆、每颗的四个 C2C 端口接到哪，是 LPU 的事，chip 只把这四个端口露出来。

Chip 只做构造与接线，四件事：

1. 按本 chip 的 harvest mask 构造 10 个 Core，坏核只构造 Router 的模块
2. 把相邻 Core 的 Router 端口用 Link 对接成 mesh
3. 四个边界 core 各露一个 C2C 端口
4. SCP 桩经 ctrl_noc 端点写到每个 core 内模块的 `cfg` 端口

SCP 桩按 boot 序列与初始化六步发配置事务，自身不计算。每 chip 一个。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1100 520" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs>
    <marker id="c0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker>
    <marker id="c0s" markerWidth="9" markerHeight="9" refX="1" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#475569"/></marker>
  </defs>
  <rect x="0" y="0" width="1100" height="520" fill="#ffffff"/>
  <text x="20" y="28" font-size="12" fill="#111827">Chip · 第 0 层（2×5 阵列，row-major 编号）</text>

  <!-- 阵列 -->
  <g font-size="10" fill="#111827">
    <rect x="200" y="80" width="110" height="70" fill="#f8fafc" stroke="#374151" rx="4"/><text x="212" y="104">core0（N）</text>
    <rect x="340" y="80" width="110" height="70" fill="#f8fafc" stroke="#374151" rx="4"/><text x="352" y="104">core1</text>
    <rect x="480" y="80" width="110" height="70" fill="#f8fafc" stroke="#374151" rx="4"/><text x="492" y="104">core2</text>
    <rect x="620" y="80" width="110" height="70" fill="#f8fafc" stroke="#374151" rx="4"/><text x="632" y="104">core3</text>
    <rect x="760" y="80" width="110" height="70" fill="#f8fafc" stroke="#374151" rx="4"/><text x="772" y="104">core4（E）</text>
    <rect x="200" y="200" width="110" height="70" fill="#f8fafc" stroke="#374151" rx="4"/><text x="212" y="224">core5（W）</text>
    <rect x="340" y="200" width="110" height="70" fill="#f8fafc" stroke="#374151" rx="4"/><text x="352" y="224">core6</text>
    <rect x="480" y="200" width="110" height="70" fill="#f8fafc" stroke="#374151" rx="4"/><text x="492" y="224">core7</text>
    <rect x="620" y="200" width="110" height="70" fill="#f8fafc" stroke="#374151" rx="4"/><text x="632" y="224">core8</text>
    <rect x="760" y="200" width="110" height="70" fill="#f8fafc" stroke="#374151" rx="4"/><text x="772" y="224">core9（S）</text>
  </g>
  <g font-size="9" fill="#475569">
    <text x="212" y="122">Router · TS · RV ×3</text><text x="212" y="136">DSA ×3 · Mem ×3</text>
    <text x="352" y="122">同左</text><text x="492" y="122">同左</text><text x="632" y="122">同左</text><text x="772" y="122">同左</text>
    <text x="212" y="242">同上</text><text x="352" y="242">同上</text><text x="492" y="242">同上</text><text x="632" y="242">坏核：只 Router</text><text x="772" y="242">同上</text>
  </g>
  <!-- mesh 链路 -->
  <g stroke="#475569">
    <line x1="312" y1="115" x2="338" y2="115" marker-start="url(#c0s)" marker-end="url(#c0)"/><line x1="452" y1="115" x2="478" y2="115" marker-start="url(#c0s)" marker-end="url(#c0)"/><line x1="592" y1="115" x2="618" y2="115" marker-start="url(#c0s)" marker-end="url(#c0)"/><line x1="732" y1="115" x2="758" y2="115" marker-start="url(#c0s)" marker-end="url(#c0)"/>
    <line x1="312" y1="235" x2="338" y2="235" marker-start="url(#c0s)" marker-end="url(#c0)"/><line x1="452" y1="235" x2="478" y2="235" marker-start="url(#c0s)" marker-end="url(#c0)"/><line x1="592" y1="235" x2="618" y2="235" marker-start="url(#c0s)" marker-end="url(#c0)"/><line x1="732" y1="235" x2="758" y2="235" marker-start="url(#c0s)" marker-end="url(#c0)"/>
    <line x1="255" y1="152" x2="255" y2="198" marker-start="url(#c0s)" marker-end="url(#c0)"/><line x1="395" y1="152" x2="395" y2="198" marker-start="url(#c0s)" marker-end="url(#c0)"/><line x1="535" y1="152" x2="535" y2="198" marker-start="url(#c0s)" marker-end="url(#c0)"/><line x1="675" y1="152" x2="675" y2="198" marker-start="url(#c0s)" marker-end="url(#c0)"/><line x1="815" y1="152" x2="815" y2="198" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  </g>
  <text x="535" y="72" font-size="9" fill="#6b7280" text-anchor="middle">相邻 Router 之间：Link 对（R2R 256 B/T，40T）</text>
  <!-- C2C -->
  <polygon points="30,96 120,96 112,132 22,132" fill="#f8fafc" stroke="#374151"/>
  <text x="71" y="118" font-size="10.5" fill="#374151" text-anchor="middle">c2c[N]</text>
  <line x1="122" y1="115" x2="198" y2="115" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <polygon points="30,216 120,216 112,252 22,252" fill="#f8fafc" stroke="#374151"/>
  <text x="71" y="238" font-size="10.5" fill="#374151" text-anchor="middle">c2c[W]</text>
  <line x1="122" y1="235" x2="198" y2="235" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <polygon points="920,96 1010,96 1002,132 912,132" fill="#f8fafc" stroke="#374151"/>
  <text x="961" y="118" font-size="10.5" fill="#374151" text-anchor="middle">c2c[E]</text>
  <line x1="872" y1="115" x2="912" y2="115" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <polygon points="920,216 1010,216 1002,252 912,252" fill="#f8fafc" stroke="#374151"/>
  <text x="961" y="238" font-size="10.5" fill="#374151" text-anchor="middle">c2c[S]</text>
  <line x1="872" y1="235" x2="912" y2="235" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="961" y="270" font-size="9" fill="#6b7280" text-anchor="middle">边界 core 各接一条 C2C（PCIe / 相邻 chip）</text>

  <!-- SCP / ctrl_noc -->
  <rect x="200" y="340" width="220" height="90" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="212" y="364" font-size="12" fill="#7c2d12">SCP 桩</text>
  <text x="212" y="384" font-size="10" fill="#92400e">boot 序列状态机 · 初始化六步</text>
  <text x="212" y="400" font-size="10" fill="#92400e">32 bit/T 发事务，每笔一拍</text>
  <text x="212" y="416" font-size="10" fill="#92400e">不计算</text>
  <rect x="480" y="340" width="390" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="492" y="364" font-size="12" fill="#111827">ctrl_noc 端点（每 core 一个）</text>
  <text x="492" y="384" font-size="10" fill="#475569">按地址分发到 TS / RouterTable / DSA / Share Mem / RV core ITCM · DTCM / Core Mem · Matrix Mem 的 cfg 口</text>
  <text x="492" y="400" font-size="10" fill="#475569">只读寄存器：core id · ready · async_int</text>
  <text x="492" y="416" font-size="10" fill="#475569">广播开关，默认关</text>
  <line x1="422" y1="385" x2="478" y2="385" stroke="#475569" stroke-dasharray="2 3" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="450" y="378" font-size="9" fill="#6b7280" text-anchor="middle">scp_ctrl</text>
  <polyline points="675,338 675,300 535,300 535,272" fill="none" stroke="#475569" stroke-dasharray="2 3" marker-end="url(#c0)"/>
  <text x="600" y="296" font-size="9" fill="#6b7280">cfg 写事务 → 各 core 各模块</text>

  <text x="20" y="470" font-size="10.5" fill="#374151">Chip 不是模块，是装配容器：构造 10 个 Core 与它们的模块、接 mesh 与 C2C、接 ctrl_noc。坏核只构造 Router 的模块。</text>
  <text x="20" y="490" font-size="10.5" fill="#374151">四个 C2C 端口只引到 chip 边界，接相邻 chip 还是 PCIe Switch 由 LPU 决定。</text>
</svg>
```

***

## 2　接口

```
port c2c[d] (双向, credit/release, clk)           // d ∈ {N, E, W, S}：边界 core（core0 / core4 / core5 / core9）Router 的一个方向，由 LPU 接到相邻 chip 或 PCIe Switch
port scp_ctrl (master, ctrl_noc 写事务, clk)      // SCP 桩 → ctrl_noc 端点，32 bit/T
  out cfg_valid · cfg_core[3:0] · cfg_addr[23:0] · cfg_we · cfg_wdata[31:0] · cfg_bcast
  in  cfg_rdata[31:0]                               // 下一拍
port async_int (slave, 电平, clk)                 // core_status → SCP：中断信息（本轮只留接口名）
  in  int_valid · int_code[7:0]
port core.cfg[m] (master, ctrl_noc 写事务, clk)   // ctrl_noc 端点 → core 内模块 m 的 cfg 口，字段同各单元文档
```

***

## 3　存储器

```
mem harvest_mask     FF        10 b                                   1R      输入                                 复位由输入给   // 坏核位图
mem logical_map      FF 阵列   10 × {logical_core, role}               1R      编译侧读入                           复位由输入给   // 本 chip 的逻辑 core 编号与角色
mem addr_map         FF 阵列   N × {base[23:0], size, target_module, target_core}  1R  静态                        复位由输入给   // ctrl_noc 地址分发表
mem core_id_reg[10]  FF        只读 core id                            1R      SCP 经 ctrl_noc 读，不可改            复位固定
mem scp_fsm          FF        {state[3:0], core_idx[3:0], step[2:0], cursor[31:0]}  1RW  boot 序列                复位 自启动
mem scp_img          FF 阵列   配置事务序列（firmware、bootloader、任务链、路由表、kernel、DSA 静态配置）  1R  编译侧读入  复位由输入给
mem noc_latch[10]    级间 latch {valid, addr[23:0], we, wdata[31:0]}   —       每拍覆写                             —              // 端点 → 目的模块 cfg 口
mem noc_rdata        1-deep 寄存器 {rdata[31:0]}                       1W1R    每拍覆写                             —
```

***

## 4　流水线总览

* **SCP 桩**：单级。B1 每拍发一笔配置事务，按 `scp_img` 顺序与 boot 状态机
* **ctrl_noc 端点**：单级。N1 收事务、查 `addr_map`、下拍写到目的模块的 `cfg` 口

不另画第 1 层图。

***

## 5　逐级行为

### A1 · Chip 装配（构造期，不逐拍）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `harvest_mask`、`logical_map`、参数表 | 1. `Build`：对 10 个 core，`harvest_mask[i]` 为坏则只构造 Router 的模块（CoreStation 永远不准入、ReduceModule 不累加、CreditMonitor 空转），否则构造全部模块<br>2. 断言：坏核不能承担 compute / B core / R core（编译侧校验，此处再查一次）<br>3. `Wire`：2×5 row-major，相邻 core 的 Router `link[d]` 用一对 Link 对接（R2R 参数）；core0 = N、core4 = E、core5 = W、core9 = S 的对外方向各引出到 `c2c[d]`，接谁由 LPU 定<br>4. 每 core 一个 ctrl_noc 端点，按各单元文档的 `cfg` 口接线 | 模块实例与端口连接 | — |

### B1 · SCP 桩 boot 序列

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `scp_fsm`、`scp_img`、`async_int`、各 core 的 `ready` | 1. 自启动 → PCIe 训练（固定拍数，参数）→ 顺序配 core0～core7（有广播开关时发一次带 `cfg_bcast` 的请求）<br>2. 每 core 初始化六步：firmware 写 ITCM → DTE bootloader 写 DTE RV core ITCM → 解复位 → TS 任务链 → router 路由表（等 RouterTable Commit Status 完成后再写 DTE 与 ReduceModule 副本）→ kernel<br>3. 每拍发 1 笔 `scp_ctrl` 事务（32 bit/T）；ITCM / DTCM 装载按镜像字节数 / 4 B 计拍<br>4. 等三个 RV core 进 wait、`ready` 全高 → 开放业务接收（Router 开始接收业务）<br>5. weights 加载模式：1 条 P2P path、datain pc 指向 weights loader、`trigger_task_chain_en = 0`，四步加载；切业务模式：换路由表、datain pc、`trigger_task_chain_en = 1`、DSA 静态配置 | `scp_ctrl`、`scp_fsm` | D1 每笔；序列 D变长 |

### N1 · ctrl_noc 端点

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `scp_ctrl`、`addr_map`、`core_id_reg`、目的模块的 `cfg_rdata` | 1. `cfg_valid ∧ cfg_core == 本 core`（或 `cfg_bcast`）→ `noc_latch = {addr, we, wdata}`，查 `addr_map` 得目的模块<br>2. 下拍写目的模块 `cfg` 口（TS CFG_REG、RouterTable CSR、DSA 寄存器、Share Mem、RV core ITCM / DTCM、Core Mem / Matrix Mem 后门）；地址不在视野内 → 报地址错（异常不建，只记）<br>3. 读：`core_id_reg` 只读；其他读转目的模块，`noc_rdata` 下一拍回 | `core.cfg[m]`、`noc_rdata` | D1 |

***

## 6　参数汇总

```
ARRAY               2×5，row-major；边界 core0 = N、core4 = E、core5 = W、core9 = S
HARVEST             每 chip 允许 1～2 个坏核（暂定方案 3）
CTRL_NOC_BW         32 bit/T（待定）；每笔事务一拍
CTRL_NOC_BCAST      开关，默认关
PCIE_TRAIN_CYCLES   参数，待定
ITCM 装载拍数        镜像字节数 / 4 B
地址空间视野         第 2 章“Chip 内地址划分”：RV core 各自 ITCM / DTCM / Share Mem / Core Mem / 对应 DSA IO reg；DTE DSA 含 Matrix Mem；MU / VU DSA 只读 Matrix Mem
```

***

## 7　机制覆盖

| 机制 | 落点 | 用例 |
| - | - | - |
| 2×5 阵列与边界 core 连接规则 | A1 第 3 条 | `chip_mesh_2x5` |
| Harvest disable mask 只关 EngineNode，RouterNode 仍可用 | A1 第 1 条 | `harvest_router_only` |
| 坏核不能承担 compute / B core / R core；可承担转发、多播、router reduce | A1 第 2 条 + Router 文档坏核一节 | `harvest_roles` |
| core id 由 SCP 经 ctrl_noc 读 MMIO，不可修改 | N1 第 3 条 | `core_id_readonly` |
| SCP boot 序列：自启动 → PCIe 训练 → 顺序配 core0～core7 | B1 第 1 条 | `boot_sequence` |
| Core 内 boot：ITCM 装载（kernel 镜像）→ 三个 RV core 进 wait → ready 全高 → 开放业务接收 | B1 第 3、4 条 + RV core 文档 G1 | `core_boot` |
| 初始化六步 | B1 第 2 条 | `init_six_steps` |
| Router 多副本提交完成后软件再写 DTE 与 ReduceModule 副本 | B1 第 2 条 | `router_table_three_copies` |
| weights 加载模式：四步加载 | B1 第 5 条 + 真实 datain 任务 | `weights_load` |
| 切到业务模式 | B1 第 5 条 | `switch_to_business` |
| 地址空间视野 | N1 第 2 条的 `addr_map` | `address_map` |

***

## 8　取舍

* **Chip 为什么做成装配容器而不是模块**
  * 它没有自己的一拍工作，全部逐拍行为在 Core 内各模块和 SCP 桩里
* **ctrl_noc 端点为什么每 core 一个**
  * 让配置事务与每个模块的 `cfg` 口一一对应
* **装载拍数为什么按字节数计而不是同拍写入**
  * 这样 boot 段的拍数与业务段同一把尺
