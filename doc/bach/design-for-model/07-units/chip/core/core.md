# Core

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → **core**（每 chip 10 个）

给装配 Core 的人：core 内有哪些单元、单元之间用哪些端口组相连、坏核构造成什么样。每个单元自身的端口、存储器、流水线与逐级行为在它自己那份文档里。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《Core 内硬件》：“Bach Core 顶层”“对外通道”“core 内部通路带宽”
* 《执行单元与存储》：三个 DSA 与三块存储的边界

***

## 1　定位与边界

Core 是 chip 阵列里的一格，对外三组连接：

* **三个 R2R 方向**（left / right / mid）各一条 256 B/T 的双向链路，由 chip 接到相邻 core 的 Router，或引到 chip 边界的 C2C 端口
* **ctrl_noc** 的配置写事务口
* 没有别的对外通路：core 的一切进出都过 Router

Core 内七个单元，各一份文档：

| 单元 | 独立打拍的模块 | 文档 |
| - | - | - |
| Router | RouterStation ×3、Xbar、CoreStation、ReduceModule、RouterTable / CSR、CoreMemCreditMonitor、Retire、CoreMem 重发 | [`router.md`](router.md) |
| TS | CFG_REG、User_Match、DataIn_task_table、Stream_table、Task_ctrl、DTE_Arb、MU_Arb / VU_Arb、Credit_monitor、Task_done | [`ts.md`](ts.md) |
| RV core ×3 | 每个一个模块：task_queue、指令执行器、dsa_iss、lsq、CSR | [`rv-core.md`](rv-core.md) |
| DTE DSA | Header Parser、Commit、TaskQueue ×4、Lane ×4、中间 Buffer、Completion RS、Hmem 与 LUT、topK 与 shareMem 写 | [`dte.md`](dte.md) |
| MU DSA | regfile、issue_q、gen_ep_info、agu ×3、ldq ×2、matrix exe、stq | [`mu.md`](mu.md) |
| VU DSA | config_register、ISQ、pipe_ctrl、LU、SU、SMUX / DMUX、VALU ×3、VSFU、MEXE、SEXE、寄存器堆与 Profile | [`vu.md`](vu.md) |
| 存储 | Core Mem、Matrix Mem、Share Mem | [`memory.md`](memory.md) |

Core 自身不打拍，只做构造与接线：构造这些模块，按各单元文档声明的端口把它们对接起来。坏核只构造 Router 的模块。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1240 900" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs>
    <marker id="c0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker>
    <marker id="c0s" markerWidth="9" markerHeight="9" refX="1" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#475569"/></marker>
  </defs>
  <rect x="0" y="0" width="1240" height="900" fill="#ffffff"/>
  <text x="20" y="30" font-size="12" fill="#111827">Bach Core · 第 0 层</text>

  <!-- 对外端口 -->
  <polygon points="560,50 660,50 650,90 550,90" fill="#f8fafc" stroke="#374151"/>
  <text x="605" y="74" font-size="10.5" fill="#374151" text-anchor="middle">data_UD</text>
  <polygon points="30,180 130,180 120,220 20,220" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="204" font-size="10.5" fill="#374151" text-anchor="middle">data_L</text>
  <polygon points="1120,180 1220,180 1210,220 1110,220" fill="#f8fafc" stroke="#374151"/>
  <text x="1165" y="204" font-size="10.5" fill="#374151" text-anchor="middle">data_R</text>
  <polygon points="30,800 130,800 120,840 20,840" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="824" font-size="10.5" fill="#374151" text-anchor="middle">scp_ctrl</text>

  <!-- Router -->
  <rect x="200" y="120" width="380" height="150" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="216" y="144" font-size="12" fill="#111827">Router</text>
  <text x="216" y="166" font-size="10" fill="#475569">RouterStation ×3 · Xbar · CoreStation</text>
  <text x="216" y="182" font-size="10" fill="#475569">ReduceModule · RouterTable / CSR</text>
  <text x="216" y="198" font-size="10" fill="#475569">CoreMemCreditMonitor · Retire · CoreMem 重发</text>
  <text x="216" y="222" font-size="9.5" fill="#9ca3af">三个方向各 256 B/T，进 core 与出 core 并行</text>
  <line x1="605" y1="92" x2="520" y2="118" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <line x1="132" y1="200" x2="198" y2="200" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <polyline points="1108,200 1000,200 1000,100 590,100 590,118" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="640" y="96" font-size="9" fill="#6b7280">flit + vc_release / stream_release / reduce_release</text>

  <!-- TS -->
  <rect x="760" y="120" width="380" height="150" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="776" y="144" font-size="12" fill="#111827">TS 任务调度器</text>
  <text x="776" y="166" font-size="10" fill="#475569">CFG_REG · User_Match · DataIn_task_table</text>
  <text x="776" y="182" font-size="10" fill="#475569">Stream_table · Task_ctrl · DTE_Arb / MU_Arb / VU_Arb</text>
  <text x="776" y="198" font-size="10" fill="#475569">Credit_monitor · Task_done</text>
  <text x="776" y="222" font-size="9.5" fill="#9ca3af">stream 16 项，任务链 64 项</text>
  <line x1="582" y1="170" x2="758" y2="170" stroke="#475569" marker-end="url(#c0)"/>
  <text x="670" y="164" font-size="9" fill="#6b7280" text-anchor="middle">trigger · credit_pulse · reduce_done</text>
  <line x1="758" y1="240" x2="582" y2="240" stroke="#475569" marker-end="url(#c0)"/>
  <text x="670" y="256" font-size="9" fill="#6b7280" text-anchor="middle">credit_req · stream_credit_return · retire</text>

  <!-- RV core ×3 -->
  <rect x="200" y="340" width="220" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="216" y="364" font-size="12" fill="#111827">DTE RV core</text>
  <text x="216" y="384" font-size="10" fill="#475569">src/rv32 · ITCM 4 KB · DTCM 8 KB</text>
  <text x="216" y="400" font-size="10" fill="#475569">task_queue · dsa_iss · sm_lsq · cm_lsq</text>
  <rect x="480" y="340" width="220" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="496" y="364" font-size="12" fill="#111827">MU RV core</text>
  <text x="496" y="384" font-size="10" fill="#475569">src/rv32 · ITCM 4 KB · DTCM 8 KB</text>
  <text x="496" y="400" font-size="10" fill="#475569">task_queue · dsa_iss · sm_lsq</text>
  <rect x="760" y="340" width="220" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="776" y="364" font-size="12" fill="#111827">VU RV core</text>
  <text x="776" y="384" font-size="10" fill="#475569">src/rv32 · ITCM 4 KB · DTCM 8 KB</text>
  <text x="776" y="400" font-size="10" fill="#475569">task_queue · dsa_iss · sm_lsq</text>

  <!-- TS ↔ RV core -->
  <polyline points="850,272 850,300 310,300 310,338" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <polyline points="870,272 870,310 590,310 590,338" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <line x1="890" y1="272" x2="890" y2="338" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="600" y="296" font-size="9" fill="#6b7280" text-anchor="middle">task_cmd / task_ack · task_done</text>

  <!-- Share Mem -->
  <rect x="1040" y="340" width="170" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="1044" y="344" width="162" height="82" fill="none" stroke="#374151"/>
  <text x="1056" y="366" font-size="12" fill="#111827">Share Mem</text>
  <text x="1056" y="386" font-size="10" fill="#475569">smem · SRAM 32 KB</text>
  <text x="1056" y="402" font-size="10" fill="#475569">RV ×3 + DTE 写 · 仲裁</text>
  <line x1="982" y1="385" x2="1038" y2="385" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <polyline points="700,385 730,385 730,320 1010,320 1010,395 1038,395" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <polyline points="420,385 450,385 450,325 1020,325 1020,405 1038,405" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="1010" y="380" font-size="9" fill="#6b7280" text-anchor="end">sm_lsq ×3</text>

  <!-- DSA ×3 -->
  <rect x="200" y="500" width="220" height="110" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="216" y="524" font-size="12" fill="#111827">DTE DSA</text>
  <text x="216" y="544" font-size="10" fill="#475569">Header Parser · Commit · TaskQueue ×4</text>
  <text x="216" y="560" font-size="10" fill="#475569">Lane ×4（AGCU）· Buffer · Completion RS</text>
  <text x="216" y="576" font-size="10" fill="#475569">Hmem / LUT · topK / shareMem 写</text>
  <rect x="480" y="500" width="220" height="110" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="496" y="524" font-size="12" fill="#111827">MU DSA</text>
  <text x="496" y="544" font-size="10" fill="#475569">regfile · issue_q · gen_ep_info</text>
  <text x="496" y="560" font-size="10" fill="#475569">agu ×3 · ldq ×2 · matrix exe · stq</text>
  <text x="496" y="576" font-size="10" fill="#475569">32 lane × 10 级</text>
  <rect x="760" y="500" width="220" height="110" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="776" y="524" font-size="12" fill="#111827">VU DSA</text>
  <text x="776" y="544" font-size="10" fill="#475569">config_register · ISQ · pipe_ctrl</text>
  <text x="776" y="560" font-size="10" fill="#475569">LU · SU · MUX · VALU ×3 · VSFU</text>
  <text x="776" y="576" font-size="10" fill="#475569">MEXE · SEXE · VRF / MRF / SRF · Profile</text>

  <!-- RV → DSA, DSA → TS -->
  <line x1="310" y1="432" x2="310" y2="498" stroke="#475569" marker-end="url(#c0)"/>
  <line x1="590" y1="432" x2="590" y2="498" stroke="#475569" marker-end="url(#c0)"/>
  <line x1="870" y1="432" x2="870" y2="498" stroke="#475569" marker-end="url(#c0)"/>
  <text x="600" y="470" font-size="9" fill="#6b7280" text-anchor="middle">dsa_cfg（dsaw / dsar）</text>
  <polyline points="422,530 460,530 460,455 1005,455 1005,272" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#c0)"/>
  <polyline points="702,530 740,530 740,455" fill="none" stroke="#475569" stroke-dasharray="4 3"/>
  <polyline points="982,530 1005,530 1005,455" fill="none" stroke="#475569" stroke-dasharray="4 3"/>
  <text x="1010" y="450" font-size="9" fill="#6b7280">dsa_done ×3（脉冲）</text>

  <!-- Router ↔ DTE DSA / DTE RV -->
  <polyline points="230,272 160,272 160,555 198,555" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="165" y="420" font-size="9" fill="#6b7280" transform="rotate(-90 165 420)" text-anchor="middle">hdr · datain · dataout（+ vc credit）</text>
  <polyline points="250,272 180,272 180,400 198,400" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="185" y="300" font-size="9" fill="#6b7280" transform="rotate(-90 185 300)" text-anchor="middle">io_reg</text>

  <!-- Core Mem / Matrix Mem -->
  <rect x="200" y="700" width="380" height="110" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="204" y="704" width="372" height="102" fill="none" stroke="#374151"/>
  <text x="216" y="726" font-size="12" fill="#111827">Core Mem</text>
  <text x="216" y="746" font-size="10" fill="#475569">cmem_bank ×8 · SRAM 1024×128 B + scale 1024×4 B</text>
  <text x="216" y="762" font-size="10" fill="#475569">6 个 master 口 · 每 bank 独占仲裁 · (1 KB + 32 B)/T</text>
  <text x="216" y="784" font-size="9.5" fill="#9ca3af">stream_id 分片由地址计算侧完成</text>
  <rect x="620" y="700" width="360" height="110" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="624" y="704" width="352" height="102" fill="none" stroke="#374151"/>
  <text x="636" y="726" font-size="12" fill="#111827">Matrix Mem</text>
  <text x="636" y="746" font-size="10" fill="#475569">mmem_bank ×32 · SRAM 1 MB + scale 128 KB / bank</text>
  <text x="636" y="762" font-size="10" fill="#475569">DTE 读写 · MU 只读（lane 一对一）· ctrl_noc · (8 + 1 KB)/T</text>

  <!-- DSA ↔ Mem -->
  <line x1="290" y1="612" x2="290" y2="698" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="296" y="660" font-size="9" fill="#6b7280">cmem_dte_rd / wr 256 B</text>
  <polyline points="380,612 380,650 700,650 700,698" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="560" y="646" font-size="9" fill="#6b7280" text-anchor="middle">mmem_dte_rd / wr 256 B</text>
  <polyline points="540,612 540,630 500,630 500,698" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="504" y="690" font-size="9" fill="#6b7280">cmem_mu_rd / wr 132 B</text>
  <line x1="640" y1="612" x2="760" y2="698" stroke="#475569" marker-start="url(#c0s)"/>
  <text x="716" y="640" font-size="9" fill="#6b7280">mmem_mu_rd 8 KB + 1 KB</text>
  <polyline points="820,612 820,670 560,670 560,698" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="700" y="684" font-size="9" fill="#6b7280" text-anchor="middle">cmem_vu_ld / st 1056 bit</text>
  <polyline points="240,432 240,470 140,470 140,720 198,720" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="145" y="600" font-size="9" fill="#6b7280" transform="rotate(-90 145 600)" text-anchor="middle">cm_lsq</text>
  <polyline points="422,590 1000,590 1000,432" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#c0)"/>
  <text x="900" y="586" font-size="9" fill="#6b7280">sm_wr</text>

  <!-- ctrl_noc -->
  <rect x="30" y="700" width="90" height="70" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="40" y="722" font-size="11" fill="#111827">ctrl_noc</text>
  <text x="40" y="740" font-size="10" fill="#475569">端点</text>
  <text x="40" y="756" font-size="10" fill="#475569">32 bit/T</text>
  <line x1="75" y1="798" x2="75" y2="772" stroke="#475569" marker-end="url(#c0)"/>
  <polyline points="75,698 75,640 100,640 100,120 200,120" fill="none" stroke="#475569" stroke-dasharray="2 3" marker-end="url(#c0)"/>
  <text x="104" y="135" font-size="9" fill="#6b7280">cfg 写事务 → 各模块的 cfg 端口</text>
</svg>
```

三条读图的提示：

* Router 的三个 R2R 方向端口接 mesh 上相邻 core 的 Router，或 chip 边界上的 C2C 链路
* Core Mem 与 Matrix Mem 只有 DTE、MU、VU 与 ctrl_noc 这几个 master
* ctrl_noc 的配置写事务按地址分发到每个模块的 `cfg` 端口，图上只画到端点

***

## 2　接口

Core 对外只有 Router 的三个方向与 ctrl_noc；core 内单元之间的端口组在两侧单元文档的“接口”章里各有一个声明块。

```
port data[d] (双向, credit/release, clk)         // d ∈ {L, R, UD}：Router 的一个 R2R 方向，由 chip 接线
  in  flit_valid · flit_vc[1:0] · flit_head · flit_tail · flit_bytes[8:0] · flit_msg
  in  stream_release_valid · reduce_release_valid · vc_release_valid
  out 同字段
port cfg (slave, ctrl_noc 写事务, clk)           // ctrl_noc 端点 → core 内各模块的 cfg 口
  in  cfg_valid · cfg_addr[23:0] · cfg_we · cfg_wdata[31:0]
  out cfg_rdata[31:0]                             // 下一拍
```

***

## 3　存储器

Core 自己只有一份只读上下文，各单元的存储在各自文档的“存储器”章。

```
mem core_context   FF   {core_id[3:0], gx[1:0], gy[3:0], role[2:0], good}   1R   构造期写入   复位由输入给   // 各模块共用的只读上下文
```

***

## 4　流水线总览

Core 没有自己的一拍工作。构造期一级：A1 接线。不画第 1 层图。

***

## 5　逐级行为

### A1 · Core 装配（构造期，不逐拍）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `core_context`、参数表 | 1. `good` 为假 → 只构造 Router 的八个模块，CoreStation 永远不准入、ReduceModule 不累加、CreditMonitor 空转；下面第 2、3 条跳过<br>2. `Build`：构造 TS 九个模块、三个 RV core、三个 DSA 的各模块、三块存储与 ctrl_noc 端点<br>3. `Wire`：按各单元文档声明的端口组把生产者的出口端口与消费者的入口端口对接；两侧都只看到端口束的字段，不持有对方的类型<br>4. 把 Router 的三个方向端口引到 `data[d]`，ctrl_noc 端点的入口引到 `cfg` | 模块实例与端口连接 | — |

***

## 6　参数汇总

```
R2R 方向        3 个（left / right / mid），各 256 B/T 双向，进 core 与出 core 并行
CORE_PER_CHIP   10（2×5，row-major）
core 内通路带宽  cmem_dte 256 B、mmem_dte 256 B、cmem_mu 132 B、mmem_mu 8 KB + 1 KB、cmem_vu 1056 bit、ctrl_noc 32 bit/T
存储容量        Core Mem 1024×128 B ×8 bank、Matrix Mem 1 MB ×32 bank、Share Mem 32 KB
```

***

## 7　机制覆盖

| 机制 | 落点 | 用例 |
| - | - | - |
| core 的一切进出都过 Router，没有旁路 | A1 第 4 条 | `core_ports` |
| 坏核只构造 Router 的模块 | A1 第 1 条 | `harvest_router_only` |
| 模块之间只通过端口相连，装配顺序不受构造顺序牵制 | A1 第 3 条 | `core_wiring` |
| 三个 R2R 方向各 256 B/T，进 core 与出 core 并行 | 参数 | `core_r2r_bw` |

***

## 8　取舍

* **Core 为什么是装配容器而不是模块**
  * 它没有自己的一拍工作，全部逐拍行为在七个单元的模块里
* **为什么坏核仍然构造 Router**
  * 坏核要承担单向转发、router multicast、router-level reduce，以及 credit 透传
