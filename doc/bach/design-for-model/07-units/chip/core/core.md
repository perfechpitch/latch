# Core

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → **core**（每 chip 10 个）

给装配 Core 的人：core 内有哪些单元、单元之间用哪些端口组相连、跨单元的那几条约定归谁管。每个单元自身的功能、端口与存储在它自己那份文档里。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《Core 内硬件》：“Bach Core 顶层”“对外通道”“core 内部通路带宽”“Core Mem 的硬件多用户管理”“内存一致性与同步”
* 《执行单元与存储》：三个 DSA 与三块存储的边界
* 《软件栈》：“每个 task 的共同形状”

***

## 1　定位与边界

Core 是 chip 阵列里的一格，对外只有两组连接：

* **三个 R2R 方向**（left / right / mid）各一条 256 B/T 的双向链路，由 chip 接到相邻 core 的 Router 或 chip 边界的 C2C Bridge
* **ctrl_noc** 的配置写事务口

没有第三条路：core 的一切进出都过 Router。

Core 内七个单元，各一份文档：

| 单元 | 独立打拍的模块 | 文档 |
| - | - | - |
| Router | RouterStation ×3、Xbar、CoreStation、ReduceModule、RouterTable / CSR、CoreMem 重发、Retire、CoreMemCreditMonitor | [`router.md`](router.md) |
| TS | User_Match、CFG_REG、DataIn_task_table、Stream_table、Task_ctrl、DTE_Arb、MU_Arb、VU_Arb、Except Check（`Stream_table` 内再展开 user_LUT、ptr_ctrl、stream_id_map、task_state_update、task_rdy_check、retire、except_check） | [`ts.md`](ts.md) |
| RV core ×3 | 每个一个模块：task_queue、指令执行器、dsa_iss、dsa_rq、lsq、CSR | [`rv-core.md`](rv-core.md) |
| DTE DSA | Header Parser、Commit、TaskQueue ×4、Lane ×4、中间 Buffer、Completion RS、Hmem 与 LUT、topK 与 shareMem 写 | [`dte.md`](dte.md) |
| MU DSA | regfile、issue_q、gen_ep_info、agu ×3 与 acu、ldq ×2、matrix exe、stq | [`mu.md`](mu.md) |
| VU DSA | config_register、ISQ、pipe_ctrl 与 Scoreboard、LU、SU、SMUX / DMUX、VALU ×3、VSFU ×2、MEXE、SEXE、寄存器堆与 Profile | [`vu.md`](vu.md) |
| 存储 | Core Mem、Matrix Mem、Share Mem | [`memory.md`](memory.md) |

core MAS 的模块表还列了四个不单独成文档的模块：

| 模块 | MAS 给的描述 | 在本套文档里的位置 |
| - | - | - |
| `Core_noc` | SCP 控制通路访问 Bach core 全局的路由模块 | 即 `ctrl_noc` 的 core 内端点，见《Core 内硬件》 |
| `DTE xbar` | DTE 搬移数据的 xbar | 即 `DMA_XBAR`，接 Core Mem 与 Matrix Mem 各 256 B/T，见《Core 内硬件》与 [`dte.md`](dte.md) |
| `Debug module` | — | 解析 DMI 操作，实现 core 内组件的 debug，见《系统与部署》 |
| `Core_monitor` | — | MAS 未给描述，本套文档未建模 |

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1490 1068" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
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
  <rect x="0" y="0" width="1490" height="1068" fill="#ffffff"/>
  <text x="20" y="26" font-size="12" fill="#111827">Bach Core · 第 0 层</text>
  <text x="207" y="26" font-size="9.5" fill="#6b7280">灰线 = 数据通路　绿线 = 控制与完成　橙线 = credit 与 retire　紫虚线 = ctrl_noc 配置</text>
  <polygon points="60,44 180,44 171,76 51,76" fill="#f8fafc" stroke="#374151"/>
  <text x="116" y="64" font-size="9" fill="#374151" text-anchor="middle">data_L</text>
  <polygon points="250,44 370,44 361,76 241,76" fill="#f8fafc" stroke="#374151"/>
  <text x="306" y="64" font-size="9" fill="#374151" text-anchor="middle">data_UD</text>
  <polygon points="440,44 560,44 551,76 431,76" fill="#f8fafc" stroke="#374151"/>
  <text x="496" y="64" font-size="9" fill="#374151" text-anchor="middle">data_R</text>
  <text x="700" y="60" font-size="8.5" fill="#6b7280" text-anchor="start">三个 R2R 方向各 256 B/T 双向，接相邻 core 的 Router 或 chip 边界的 C2C Bridge</text>
  <text x="700" y="76" font-size="8.5" fill="#6b7280" text-anchor="start">线上跑 flit，另有 vc_release / stream_release / reduce_release 三条独立回程</text>
  <rect x="40" y="110" width="560" height="196" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="52" y="131" font-size="11" fill="#111827">Router（八个独立打拍的模块）</text>
  <text x="52" y="148" font-size="8.5" fill="#475569">RouterStation ×3（left / right / mid）</text>
  <text x="52" y="161.5" font-size="8.5" fill="#475569">　Header Parser · VC Buffer ×4 · Packet Context</text>
  <text x="52" y="175.0" font-size="8.5" fill="#475569">　Stream Resource Table · VC Credit 计数器 · Credit Release 静态旁路</text>
  <text x="52" y="188.5" font-size="8.5" fill="#475569">Xbar 5 入 7 出：按输出 RoundRobin，贪婪整包，多播全有全无</text>
  <text x="52" y="202.0" font-size="8.5" fill="#475569">CoreStation：HeaderFIFO · OutputBuffer · 三态准入 · 进出 core 并行</text>
  <text x="52" y="215.5" font-size="8.5" fill="#475569">ReduceModule：16 用户 × 16 KiB · RMW FP32 累加 · 下游 Reduce credit 表</text>
  <text x="52" y="229.0" font-size="8.5" fill="#475569">RouterTable / CSR（64 项，多副本提交）· CoreMem 重发</text>
  <text x="52" y="242.5" font-size="8.5" fill="#475569">Retire · CoreMemCreditMonitor（监听事件队列 16 项全相连）</text>
  <text x="588" y="297" font-size="8.5" fill="#9ca3af" text-anchor="end">每 Core 一份，坏核也有</text>
  <rect x="740" y="110" width="640" height="196" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="752" y="131" font-size="11" fill="#111827">TS 任务调度器（九个独立打拍的模块）</text>
  <text x="752" y="148" font-size="8.5" fill="#475569">CFG_REG：task_chain 64 项 · datain_task 1 项 · stream_num</text>
  <text x="752" y="161.5" font-size="8.5" fill="#475569">　CORE_TYPE · B_CORE_DIRECTION · TS_INIT_FINISH / TS_STATE</text>
  <text x="752" y="175.0" font-size="8.5" fill="#475569">User_Match · DataIn_task_table（只有 1 项，占住就反压 Router）</text>
  <text x="752" y="188.5" font-size="8.5" fill="#475569">Stream_table：16 项顺序 FIFO · task_fsm · done_bitmap 64 位 · 六个写口</text>
  <text x="752" y="202.0" font-size="8.5" fill="#475569">Task_ctrl：SKIP_MASK 一拍跳过 · 原子安装后继 · End task 不可跳</text>
  <text x="752" y="215.5" font-size="8.5" fill="#475569">DTE_Arb（reissue 最高）· MU_Arb · VU_Arb（从 head_ptr 环形年龄优先）</text>
  <text x="752" y="229.0" font-size="8.5" fill="#475569">credit 子模块：注册 / 唤醒 · retire：Head-only 退休</text>
  <text x="752" y="242.5" font-size="8.5" fill="#475569">Task_done：七路完成合流，reduce 拆成 DTE ack 与 Router Done 两半</text>
  <text x="1368" y="297" font-size="8.5" fill="#9ca3af" text-anchor="end">四种工作模式由 CORE_TYPE 与 WEIGHTS_MODE 选定</text>
  <rect x="40" y="360" width="290" height="100" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="52" y="381" font-size="11" fill="#111827">DTE RV core</text>
  <text x="52" y="398" font-size="8.5" fill="#475569">task_queue · dsa_iss（每拍 ≤ 1 条）· dsa_rq 8 项</text>
  <text x="52" y="411.5" font-size="8.5" fill="#475569">sm_lsq 16 · cm_lsq 16 · Router I/O reg</text>
  <text x="52" y="425.0" font-size="8.5" fill="#475569">ITCM 4 KB · DTCM 8 KB · gpr 就绪表 · 自定义 CSR</text>
  <text x="318" y="451" font-size="8.5" fill="#9ca3af" text-anchor="end">src/rv32 逐条执行，每条 1 拍</text>
  <rect x="360" y="360" width="290" height="100" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="372" y="381" font-size="11" fill="#111827">MU RV core</text>
  <text x="372" y="398" font-size="8.5" fill="#475569">task_queue · dsa_iss（每拍 ≤ 1 条）· dsa_rq 8 项</text>
  <text x="372" y="411.5" font-size="8.5" fill="#475569">sm_lsq 16</text>
  <text x="372" y="425.0" font-size="8.5" fill="#475569">ITCM 4 KB · DTCM 8 KB · gpr 就绪表 · 自定义 CSR</text>
  <text x="638" y="451" font-size="8.5" fill="#9ca3af" text-anchor="end">src/rv32 逐条执行，每条 1 拍</text>
  <rect x="680" y="360" width="290" height="100" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="692" y="381" font-size="11" fill="#111827">VU RV core</text>
  <text x="692" y="398" font-size="8.5" fill="#475569">task_queue · dsa_iss（每拍 ≤ 1 条）· dsa_rq 8 项</text>
  <text x="692" y="411.5" font-size="8.5" fill="#475569">sm_lsq 16</text>
  <text x="692" y="425.0" font-size="8.5" fill="#475569">ITCM 4 KB · DTCM 8 KB · gpr 就绪表 · 自定义 CSR</text>
  <text x="958" y="451" font-size="8.5" fill="#9ca3af" text-anchor="end">src/rv32 逐条执行，每条 1 拍</text>
  <rect x="1150" y="360" width="200" height="100" fill="#f5f3ff" stroke="#7c3aed" rx="4"/>
  <text x="1162" y="381" font-size="11" fill="#111827">ctrl_noc 端点</text>
  <text x="1162" y="398" font-size="8.5" fill="#475569">按 addr_map 分发到 core 内各模块的 cfg 口</text>
  <text x="1162" y="411.5" font-size="8.5" fill="#475569">32 bit/T，每笔事务一拍</text>
  <text x="1162" y="425.0" font-size="8.5" fill="#475569">core id 只读</text>
  <polygon points="1392,378 1480,378 1471,408 1383,408" fill="#f8fafc" stroke="#374151"/>
  <text x="1432" y="397" font-size="8.5" fill="#374151" text-anchor="middle">scp_ctrl</text>
  <rect x="40" y="560" width="310" height="168" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="52" y="581" font-size="11" fill="#111827">DTE DSA（八个模块）</text>
  <text x="52" y="598" font-size="8.5" fill="#475569">Header Parser（非法头 Drop Frame）· Commit（双 Bank，Bank0 优先）</text>
  <text x="52" y="611.5" font-size="8.5" fill="#475569">TaskQueue ×4 · Lane ×4（RD/WR × CH0/CH1，含 AGCU）</text>
  <text x="52" y="625.0" font-size="8.5" fill="#475569">中间 Buffer 约 8 KB（read-ahead credit）</text>
  <text x="52" y="638.5" font-size="8.5" fill="#475569">Completion RS（按 task_id Join）· Done Pending</text>
  <text x="52" y="652.0" font-size="8.5" fill="#475569">Hmem 16 KB + 32 B · LUT 192 B</text>
  <text x="52" y="665.5" font-size="8.5" fill="#475569">RouterTable 副本 · 本级 Reduce credit 表 · PendingTaskQ</text>
  <text x="52" y="679.0" font-size="8.5" fill="#475569">出方向 VC buffer ×4 · shareMem 写</text>
  <rect x="380" y="560" width="310" height="168" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="392" y="581" font-size="11" fill="#111827">MU DSA（七个模块）</text>
  <text x="392" y="598" font-size="8.5" fill="#475569">regfile · issue_q 16 · gen_ep_info（local_ep_table）</text>
  <text x="392" y="611.5" font-size="8.5" fill="#475569">agu ×3 与 acu（越界 / 对齐检查 → Drain &amp; Trap）</text>
  <text x="392" y="625.0" font-size="8.5" fill="#475569">Token ldq 16（Rd outstanding 4 KB）· Weight ldq 4</text>
  <text x="392" y="638.5" font-size="8.5" fill="#475569">matrix exe：32 lane × 10 级 CSA 树，vlane 1 / 2</text>
  <text x="392" y="652.0" font-size="8.5" fill="#475569">stq 16 · Wr concat buffer 1～2 KB</text>
  <text x="392" y="665.5" font-size="8.5" fill="#475569">C = A × B 或 C = C + (A × B) × W_ep</text>
  <text x="392" y="679.0" font-size="8.5" fill="#475569">先循环 tile_K 再循环 tile_N，task 间三段重叠</text>
  <rect x="720" y="560" width="330" height="168" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="732" y="581" font-size="11" fill="#111827">VU DSA（十一个模块）</text>
  <text x="732" y="598" font-size="8.5" fill="#475569">config_register：8 组静态模板 + 12 个动态参数</text>
  <text x="732" y="611.5" font-size="8.5" fill="#475569">ISQ · pipe_ctrl + Scoreboard（VRF / MRF / SRF 的依赖）</text>
  <text x="732" y="625.0" font-size="8.5" fill="#475569">LU · SU：CM 每周期 1 Load + 1 Store，各 128 B，不 burst</text>
  <text x="732" y="638.5" font-size="8.5" fill="#475569">SMUX / DMUX</text>
  <text x="732" y="652.0" font-size="8.5" fill="#475569">VALU0 / VALU1 / VALU2 · VSFU · MEXE · SEXE（3 次迭代）</text>
  <text x="732" y="665.5" font-size="8.5" fill="#475569">VRF 64 KB · MRF 4 KB · SRF 256 B · Profile 计数器</text>
  <text x="732" y="679.0" font-size="8.5" fill="#475569">最多两条相邻宏指令重叠；CM 访存冲突靠 MACRO_INST_FENCE</text>
  <rect x="1090" y="560" width="310" height="168" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="1102" y="581" font-size="11" fill="#111827">Share Mem</text>
  <text x="1102" y="598" font-size="8.5" fill="#475569">smem · SRAM 32 KB</text>
  <text x="1102" y="611.5" font-size="8.5" fill="#475569">访问延迟 5～10 拍，不需要初始化</text>
  <text x="1102" y="625.0" font-size="8.5" fill="#475569">master：三个 RV core 的 sm_lsq</text>
  <text x="1102" y="638.5" font-size="8.5" fill="#475569">　　　　+ DTE DSA 的 shareMem 写</text>
  <text x="1102" y="652.0" font-size="8.5" fill="#475569">用途：task 间共享数据</text>
  <text x="1102" y="665.5" font-size="8.5" fill="#475569">B core 的 head / tail 指针</text>
  <text x="1102" y="679.0" font-size="8.5" fill="#475569">R core 的 arrive_num 与 tmp_info 表</text>
  <text x="1388" y="719" font-size="8.5" fill="#9ca3af" text-anchor="end">四个 master 仲裁</text>
  <rect x="40" y="818" width="620" height="170" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="44" y="822" width="612" height="162" fill="none" stroke="#374151"/>
  <text x="52" y="839" font-size="11" fill="#111827">Core Mem</text>
  <text x="52" y="856" font-size="8.5" fill="#475569">cmem_bank ×8：SRAM 1024 × 128 B + 每 bank 4 KB scale = 1 MB + 32 KB</text>
  <text x="52" y="869.5" font-size="8.5" fill="#475569">最大带宽 (1 KB + 32 B)/T；地址粒度 128 B + 4 B，支持 byte mask</text>
  <text x="52" y="883.0" font-size="8.5" fill="#475569">master：DTE DSA 256 B/T（13T）· MU 132 B/T（11T）· VU 132 B/T（14T）</text>
  <text x="52" y="896.5" font-size="8.5" fill="#475569">　　　　DTE RV core（128 B / 16 B / 2 B）· Router 重发 · ctrl_noc 4 B/T</text>
  <text x="52" y="910.0" font-size="8.5" fill="#475569">仲裁：每 bank 二选一；DTE 端口先判读写各自冲突，再判读写之间</text>
  <text x="52" y="923.5" font-size="8.5" fill="#475569">非同组优先级 MU &gt; VU = DTE；DTE 部分冲突只反压那个 bank</text>
  <text x="52" y="937.0" font-size="8.5" fill="#475569">按 stream_num 均等分片，base(stream_id) 在 master 侧算</text>
  <rect x="700" y="818" width="640" height="170" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="704" y="822" width="632" height="162" fill="none" stroke="#374151"/>
  <text x="712" y="839" font-size="11" fill="#111827">Matrix Mem</text>
  <text x="712" y="856" font-size="8.5" fill="#475569">mmem_bank ×64，每 bank 0.5625 MB，合计 32 + 4 MB（scale : data = 1 : 8）</text>
  <text x="712" y="869.5" font-size="8.5" fill="#475569">最大带宽 (8 + 1) KB/T；地址粒度 128 B，不支持 byte mask</text>
  <text x="712" y="883.0" font-size="8.5" fill="#475569">master：DTE DSA 读写 256 B/T（写 9T 读 8T）· MU 只读 (8+1) KB/T（8T）</text>
  <text x="712" y="896.5" font-size="8.5" fill="#475569">　　　　ctrl_noc 4 B/T（地址对齐 128 B，数据粒度 4 B，burst ≤ 32）</text>
  <text x="712" y="910.0" font-size="8.5" fill="#475569">硬约束：同一 bank 不许两个 master 同时访问，冲突时只执行 MU 并计数报错</text>
  <text x="712" y="923.5" font-size="8.5" fill="#475569">SRAM 单 bit 自纠错，纠错后在 SRAM 空闲时写回覆盖</text>
  <text x="712" y="937.0" font-size="8.5" fill="#475569">三种角色：普通 core 存 weight · B core 存 token · R core 存 reduction</text>
  <polyline points="116,76 116,93 107,93 107,110" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="306,76 306,93 253,93 253,110" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="496,76 496,93 398,93 398,110" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="600,141 740,141" fill="none" stroke="#0f766e" marker-end="url(#g)"/>
  <text x="670" y="138" font-size="8.5" fill="#0f766e" text-anchor="middle">router2ts_trigger_ch</text>
  <polyline points="600,173 740,173" fill="none" stroke="#0f766e" marker-end="url(#g)"/>
  <text x="670" y="169" font-size="8.5" fill="#0f766e" text-anchor="middle">router2ts_credit_ch</text>
  <polyline points="600,204 740,204" fill="none" stroke="#0f766e" marker-end="url(#g)"/>
  <text x="670" y="200" font-size="8.5" fill="#0f766e" text-anchor="middle">rmem2ts_done_ch</text>
  <polyline points="740,243 600,243" fill="none" stroke="#b45309" marker-end="url(#o)"/>
  <text x="670" y="238" font-size="8.5" fill="#b45309" text-anchor="middle">ts2router 资源注册</text>
  <polyline points="740,279 600,279" fill="none" stroke="#b45309" marker-end="url(#o)"/>
  <text x="670" y="290" font-size="8.5" fill="#b45309" text-anchor="middle">retire / stream credit 返还</text>
  <polyline points="823,306 823,333 185,333 185,360" fill="none" stroke="#0f766e" marker-start="url(#gs)" marker-end="url(#g)"/>
  <polyline points="970,306 970,333 505,333 505,360" fill="none" stroke="#0f766e" marker-start="url(#gs)" marker-end="url(#g)"/>
  <polyline points="1124,306 1124,333 825,333 825,360" fill="none" stroke="#0f766e" marker-start="url(#gs)" marker-end="url(#g)"/>
  <text x="560" y="326" font-size="8.5" fill="#0f766e" text-anchor="middle">task_cmd（task_pc · stream_id · local_user_id）/ task_ack；rv_done（stream_id · local_user_id · task_id）</text>
  <polyline points="185,460 185,510 195,510 195,560" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="505,460 505,510 535,510 535,560" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="825,460 825,510 885,510 885,560" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <text x="505" y="478" font-size="8.5" fill="#6b7280" text-anchor="middle">dsa_cfg：dsaw / dsawi 写寄存器 + 写 trigger 启动；dsar / dsari 读不阻塞，按 dsa_rq 顺序写回 gpr</text>
  <polyline points="325,560 325,502 1060,502 1060,306" fill="none" stroke="#0f766e" stroke-dasharray="4 3" marker-end="url(#g)"/>
  <polyline points="665,560 665,502 1079,502 1079,306" fill="none" stroke="#0f766e" stroke-dasharray="4 3" marker-end="url(#g)"/>
  <polyline points="1024,560 1024,502 1098,502 1098,306" fill="none" stroke="#0f766e" stroke-dasharray="4 3" marker-end="url(#g)"/>
  <text x="1140" y="496" font-size="8.5" fill="#0f766e" text-anchor="end">dsa_done ×3 · VU 的 Event 同步信号 → Task_done</text>
  <polyline points="289,460 289,530 1133,530 1133,560" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="621,460 621,530 1161,530 1161,560" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="953,460 953,530 1189,530 1189,560" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <text x="1086" y="524" font-size="8.5" fill="#6b7280" text-anchor="end">sm_lsq ×3（32 bit，顺序发射，5～10 拍）</text>
  <polyline points="40,267 14,267 14,610 40,610" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <text transform="rotate(-90 28 640)" x="28" y="640" font-size="8.5" fill="#6b7280" text-anchor="middle">in_core / out_core_data_ch（AXI-Stream-Like）+ VC credit</text>
  <polyline points="57,306 57,333 81,333 81,360" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-start="url(#as)" marker-end="url(#a)"/>
  <text x="112" y="332" font-size="8.5" fill="#6b7280" text-anchor="start">Router I/O reg</text>
  <polyline points="96,728 96,773 102,773 102,818" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <text x="108" y="776" font-size="8.5" fill="#6b7280" text-anchor="start">cmem_dte 256 B</text>
  <polyline points="263,728 263,773 802,773 802,818" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <text x="540" y="758" font-size="8.5" fill="#6b7280" text-anchor="middle">mmem_dte 256 B</text>
  <polyline points="479,728 479,773 424,773 424,818" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <text x="486" y="776" font-size="8.5" fill="#6b7280" text-anchor="start">cmem_mu 132 B</text>
  <polyline points="653,728 653,773 994,773 994,818" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <text x="920" y="794" font-size="8.5" fill="#6b7280" text-anchor="middle">mmem_mu 只读 (8+1) KB</text>
  <polyline points="793,728 793,773 598,773 598,818" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <text x="686" y="742" font-size="8.5" fill="#6b7280" text-anchor="middle">cmem_vu 1056 bit，不 burst</text>
  <polyline points="330,428 352,428 352,796 365,796 365,818" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-start="url(#as)" marker-end="url(#a)"/>
  <text x="371" y="790" font-size="8.5" fill="#6b7280" text-anchor="start">cm_lsq（固定读回 1056 bit，32 bit / 拍）</text>
  <polyline points="350,721 372,721 372,750 1127,750 1127,728" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#a)"/>
  <text x="1078" y="772" font-size="8.5" fill="#6b7280" text-anchor="end">shareMem 写（B / R core）</text>
  <polyline points="1383,393 1366,393 1366,410 1350,410" fill="none" stroke="#7c3aed" stroke-dasharray="2 3" marker-end="url(#p)"/>
  <polyline points="1250,360 1250,333 1252,333 1252,306" fill="none" stroke="#7c3aed" stroke-dasharray="2 3" marker-end="url(#p)"/>
  <text x="1480" y="336" font-size="8.5" fill="#7c3aed" text-anchor="end">cfg → TS / RouterTable / 三个 DSA / Share Mem / ITCM · DTCM / Cmem · Mmem</text>
  <text x="20" y="1052" font-size="10.5" fill="#374151">Core 不打拍，是装配容器：构造上面全部模块，按各单元文档声明的端口组对接。坏核只构造 Router 的八个模块，其余一律不构造。</text>
</svg>
```

***

## 2　功能清单

一功能一条，编号供“机制覆盖”一章引用。Core 自身不打拍，功能分两类：构造期的接线，以及跨单元、不归任何单个单元的那几条约定。

### 装配（构造期，不打拍）

| 编号 | 功能 |
| - | - |
| F1 | `good` 为真时构造七个单元的全部模块：Router 八个、TS 九个、三个 RV core、三个 DSA 的各模块、三块存储与 ctrl_noc 端点 |
| F2 | `good` 为假时只构造 Router 的八个模块。CoreStation 永远不准入，ReduceModule 不累加，CoreMemCreditMonitor 空转，`stream_credit` 上电默认 0 |
| F3 | 按各单元文档声明的端口组把生产者的出口端口与消费者的入口端口对接；两侧只看到端口束的字段，不持有对方的类型，装配顺序不受构造顺序牵制 |
| F4 | 把 Router 三个 RouterStation 的对外端口引到 `data_L` / `data_UD` / `data_R` |
| F5 | 把 ctrl_noc 端点的入口引到 `cfg`，出口按 `addr_map` 接到各模块的 `cfg` 口 |
| F6 | 建立只读的 `core_context`（`core_id`、全局坐标、角色、`good`），core 内各模块共用 |

### 跨单元的约定

| 编号 | 功能 |
| - | - |
| F7 | 一个 task 的共同形状：TS 把 `task_pc` 与 `stream_id` 配给一个 RV core → RV core 向自己的 DSA 发一条异步任务指令后立刻向 TS 交还自己，不等 DSA 执行完 → DSA 干完向 TS 回 ack，TS 据此推进同一 stream 的下一个 task |
| F8 | 只做标量活的 task（check flag、改指针、改映射表）不调 DSA，由 RV core 自己向 TS 报完成 |
| F9 | 两处会阻塞：RV core 正忙时该 task 等待；DSA 的指令 buffer 满时反压 RV core，该 RV core 不能参与下一个用户的搬运 |
| F10 | 进 core 与出 core 两条数据通路完全并行，互不共享数据通路仲裁状态 |
| F11 | Core Mem 的硬件多用户管理：软件配 `stream_num`（1～16），硬件按它把 Core Mem 均等切分，TS 给新用户分配唯一 `stream_id` 绑定一份独立物理分片，软件用统一偏移地址读写，硬件路由到当前用户的分片，用户结束后硬件自动回收 |
| F12 | Matrix Mem 不做这套分片，每个用户看到的是相同的权重 |
| F13 | 内存一致性靠任务隔离，不做显式维护：不同用户之间地址空间独立；MU、VU、DTE 的 RV core 在任意时刻不执行同一个用户的 task；同一用户的 task 按任务链顺序执行，前一个完成后才下发后一个 |
| F14 | 同一用户跨 task 的数据经 Share Mem 传，前一个 task 的 RV core 写完 Share Mem 后用 `fence` 加 task 完成通知 TS 的方式隔离两个 task 的数据相关 |
| F15 | 生产者与消费者按五对 Release / Acquire 配对：Router 写内部 Buffer 配 Data Ready；RV 写 DMA Command 配 Doorbell；DMA 写目标 Memory 配 DMA Task Done；MU / VU 写 Task 输出配 Task Done；DSA 写输出配 Chain Done |
| F16 | VU 不能直接读 Matrix Mem。需要 Matrix Mem 里的数据时先由 DTE 搬到 Core Mem |
| F17 | 特权级只支持 M 态，不实现 MMU，中断异常上报 SCP。本轮只留状态位与接口名 |
| F18 | DTE RV core 的 `cm_lsq` 按地址范围分流到两个从端：Core Mem 的 `cmem_rv`，与 Router CoreStation 的 `hdr_rd`。包头只有这一条读取通路，DTE DSA 不另接一条 |
| F19 | 三个 DSA 的任务身份一律由软件写入各自的配置寄存器，core 内**不存在**从 RV core 到 DSA 的身份专用通路：DTE 写 `TASK_CFG_PACK`，MU 与 VU 写各自的动态配置寄存器 |
| F20 | **三个 DSA 之间没有任何直连**：DTE 进核时把 topK 写进 Core Mem 的 topK 区，MU 自己从那里读回来。DSA 之间的数据一律经存储交换，控制一律经 TS 与各自的 RV core |

***

## 3　接口

Core 对外只有 Router 的三个方向与 ctrl_noc；core 内单元之间的端口组在两侧单元文档的“接口”一章里各有一个声明块。

```
port data[d] (双向, credit/release, clk)         // d ∈ {L, UD, R}：Router 的一个 R2R 方向，由 chip 接线
  in  flit_valid · flit_vc[1:0] · flit_head · flit_tail · flit_bytes[8:0] · flit_msg
  in  stream_release_valid · stream_release_user[15:0]
  in  reduce_release_valid · reduce_release_user[15:0]
  in  vc_release_valid · vc_release_vc[1:0]
  out 同字段
port cfg (slave, ctrl_noc 写事务, clk)           // ctrl_noc 端点 → core 内各模块的 cfg 口
  in  cfg_valid · cfg_addr[23:0] · cfg_we · cfg_wdata[31:0]
  out cfg_rdata[31:0]                             // 下一拍
port ready (master, 电平, clk)                   // 三个 RV core 都进 wait 后拉高，SCP 据此开放业务接收
  out ready
```

***

## 4　存储器

Core 自己只有一份只读上下文，各单元的存储在各自文档的“存储器”一章。

```
mem core_context   FF   {core_id[3:0], gx[1:0], gy[3:0], role[2:0], good}   1R   构造期写入   复位由输入给   // 各模块共用的只读上下文
```

***

## 5　流水线总览

Core 不打拍，没有第 1 层图。各单元的第 1 层图在各自文档里。

***

## 6　逐级行为

Core 只有构造期的接线，没有逐级行为。各单元的第 2 层图待各自的第 1 层图完成后补。

***

## 7　参数汇总

```
R2R 方向        3 个（left / right / mid），各 256 B/T 双向，进 core 与出 core 并行
CORE_PER_CHIP   10（2×5，row-major）
stream_num      1～16，软件配；决定 Core Mem 的分片数
core 内通路带宽  ctrl_noc 32 bit/T · Router ↔ DTE 256 B/T ×2 · MU ← Matrix Mem 8 KB/T
                MU ↔ Core Mem 132 B/T · VU ↔ Core Mem 132 B/T · DTE ↔ Core / Matrix Mem 各 256 B/T
存储容量        Core Mem 1 MB + 32 KB · Matrix Mem 32 + 4 MB · Share Mem 32 KB
                ITCM 4 KB ×3 · DTCM 8 KB ×3
访存延迟        Core Mem 15T 以内 · Matrix Mem 50T 以内 · Share Mem 5～10 拍 · ITCM 1 拍 · DTCM 3 拍
性能约束        单个用户各 DSA 对应的软件调度程序在 RV core 上执行时间不超过 200 cycle
```

***

## 8　机制覆盖

| 机制 | 功能 | 用例 |
| - | - | - |
| core 的一切进出都过 Router，没有旁路 | F4 | `core_ports` |
| 坏核只构造 Router 的八个模块 | F2 | `harvest_router_only` |
| 模块之间只通过端口相连，装配顺序不受构造顺序牵制 | F3 | `core_wiring` |
| 一个 task 的共同形状：RV core 发完异步指令立刻交还自己 | F7 | `task_shape` |
| 只做标量活的 task 不调 DSA，RV core 自己报完成 | F8 | `scalar_only_task` |
| DSA 指令 buffer 满时反压 RV core | F9 | `dsa_backpressure` |
| 进 core 与出 core 完全并行 | F10 | `core_in_out_parallel` |
| Core Mem 的硬件多用户管理：相同虚拟地址映射到不同物理分片 | F11 | `stream_slice` |
| Matrix Mem 不分片 | F12 | `mmem_no_slice` |
| 任务隔离代替一致性维护 | F13、F14 | `task_isolation` |
| 五对 Release / Acquire 配对 | F15 | `release_acquire_pairs` |
| VU 不能直接读 Matrix Mem | F16 | `vu_no_mmem` |
| cm_lsq 按地址分流到 Core Mem 与 Router 包头口，包头只有一条通路 | F18 | `cm_lsq_split` |
| 三个 DSA 的身份都由软件写寄存器，无专用硬件通路 | F19 | `dsa_id_by_software` |
| 三个 DSA 之间没有直连，数据一律经存储交换 | F20 | `no_dsa_direct_link` |

***

## 9　取舍

* **Core 为什么是装配容器而不是模块**
  * 它没有自己的一拍工作，全部逐拍行为在七个单元的模块里
* **为什么坏核仍然构造 Router**
  * 坏核要承担单向转发、router multicast、router-level reduce，以及三类 credit 的透传
* **为什么地址映射交给硬件而不是软件**
  * 多用户复用同一套 kernel 代码，软件只能用统一固定的虚拟偏移地址，没法为每个用户单独改地址、单独编译
  * 纯软件管理会让相同虚拟地址落到同一块物理内存，多用户互相覆盖
* **为什么单独做一块 Share Mem**
  * Core Mem 容量大、物理距离远，访问延时 15～25 拍，顺序执行的 RV core 掩盖不了
  * Share Mem 容量小、距离近，延时 5～10 拍，用来加速三个 RV core 的 task 之间传数据
