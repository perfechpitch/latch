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
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 2058 1312" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" role="img" aria-label="Bach Core 第 0 层">
<title>Bach Core 第 0 层</title>
<defs><marker id="a" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#475569"/></marker><marker id="as" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#475569"/></marker><marker id="g" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0f766e"/></marker><marker id="gs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0f766e"/></marker><marker id="o" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#b45309"/></marker><marker id="os" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#b45309"/></marker><marker id="p" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#7c3aed"/></marker><marker id="ps" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#7c3aed"/></marker><marker id="i" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#4338ca"/></marker><marker id="is" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#4338ca"/></marker><marker id="t" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0d9488"/></marker><marker id="ts" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0d9488"/></marker><marker id="r" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#be123c"/></marker><marker id="rs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#be123c"/></marker><marker id="b" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#2563eb"/></marker><marker id="bs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#2563eb"/></marker><marker id="m" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#d97706"/></marker><marker id="ms" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#d97706"/></marker><marker id="l" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#9aa1ad"/></marker><marker id="ls" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#9aa1ad"/></marker></defs>
<rect x="0" y="0" width="2058" height="1312" fill="#ffffff"/>
<text x="20" y="26" font-size="12" fill="#111827">Bach Core · 第 0 层（TS 在上，Router 在下；MU / VU / DTE 三列各自 RV core 在上、DSA 在下；存储一行在 DSA 之下，DTE xbar 再往下；core_noc 环绕一圈）</text>
<text x="978" y="26" font-size="9.5" fill="#6b7280">灰线 = 数据通路　绿线 = 控制与完成　橙线 = credit 与 retire　紫虚线 = ctrl_noc 配置</text>
<rect x="130" y="92" width="1798" height="1100" rx="14" fill="none" stroke="#374151" stroke-width="1.2"/>
<rect x="216" y="112" width="1626" height="1066" rx="8" fill="none" stroke="#7c3aed" stroke-width="1.4" stroke-dasharray="6 4"/>
<text x="140" y="107" font-size="11" fill="#111827" font-weight="600">Bach Core</text>
<text x="226" y="1172" font-size="9.5" fill="#7c3aed" font-weight="600">core_noc + debug_noc 环：ctrl_noc 端点按 addr_map 把 cfg 写到环上各模块（TS / RouterTable / 三个 DSA / Share Mem / ITCM · DTCM / Cmem · Mmem），32 bit/T</text>
<rect x="144" y="130" width="56" height="880" rx="4" fill="#fdf6ec" stroke="#b45309" stroke-dasharray="4 3"/>
<text transform="rotate(-90 172.0 570.0)" x="172.0" y="573.7" font-size="10.5" fill="#92400e" font-weight="600" text-anchor="middle">Core Monitor：状态影子寄存器 · IPI（未建模，只留 async_int / ts2corestatus 接口名）</text>
<rect x="1858" y="130" width="56" height="880" rx="4" fill="#f3f4f6" stroke="#6b7280" stroke-dasharray="4 3"/>
<text transform="rotate(-90 1886.0 570.0)" x="1886.0" y="573.7" font-size="10.5" fill="#4b5563" font-weight="600" text-anchor="middle">Debug Module：解析 DMI，实现 core 内组件的 debug（未建模）</text>
<rect x="234" y="130" width="1200" height="114" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="246" y="151" font-size="11" fill="#111827" font-weight="600">TS 任务调度器（九个独立打拍的模块）</text>
<text x="246.0" y="168.0" font-size="8.5" fill="#475569">CFG_REG：task_chain 64 项 · datain_task 1 项 · stream_num</text>
<text x="246.0" y="181.5" font-size="8.5" fill="#475569">　CORE_TYPE · B_CORE_DIRECTION · TS_INIT_FINISH / TS_STATE</text>
<text x="246.0" y="195.0" font-size="8.5" fill="#475569">User_Match · DataIn_task_table（只有 1 项，占住就反压 Router）</text>
<text x="246.0" y="208.5" font-size="8.5" fill="#475569">Stream_table：16 项顺序 FIFO · task_fsm · done_bitmap 64 位 · 六个写口</text>
<text x="834.0" y="168.0" font-size="8.5" fill="#475569">Task_ctrl：SKIP_MASK 一拍跳过 · 原子安装后继 · End task 不可跳</text>
<text x="834.0" y="181.5" font-size="8.5" fill="#475569">DTE_Arb（reissue 最高）· MU_Arb · VU_Arb（从 head_ptr 环形年龄优先）</text>
<text x="834.0" y="195.0" font-size="8.5" fill="#475569">credit 子模块：注册 / 唤醒 · retire：Head-only 退休</text>
<text x="834.0" y="208.5" font-size="8.5" fill="#475569">Task_done：七路完成合流，reduce 拆成 DTE ack 与 Router Done 两半</text>
<text x="1422" y="235" font-size="8.5" fill="#9ca3af" text-anchor="end">四种工作模式由 CORE_TYPE 与 WEIGHTS_MODE 选定</text>
<rect x="1524" y="130" width="300" height="86.5" rx="4" fill="#f5f3ff" stroke="#7c3aed"/>
<text x="1536" y="151" font-size="11" fill="#111827" font-weight="600">ctrl_noc 端点</text>
<text x="1536.0" y="168.0" font-size="8.5" fill="#475569">按 addr_map 分发到 core 内各模块的 cfg 口</text>
<text x="1536.0" y="181.5" font-size="8.5" fill="#475569">32 bit/T，每笔事务一拍</text>
<text x="1536.0" y="195.0" font-size="8.5" fill="#475569">core id 只读</text>
<rect x="1524" y="256.5" width="300" height="154.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1536" y="277.5" font-size="11" fill="#111827" font-weight="600">Share Mem</text>
<text x="1536.0" y="294.5" font-size="8.5" fill="#475569">smem · SRAM 32 KB</text>
<text x="1536.0" y="308.0" font-size="8.5" fill="#475569">访问延迟 5～10 拍，不需要初始化</text>
<text x="1536.0" y="321.5" font-size="8.5" fill="#475569">master：三个 RV core 的 sm_lsq</text>
<text x="1536.0" y="335.0" font-size="8.5" fill="#475569">　　　　+ DTE DSA 的 shareMem 写</text>
<text x="1536.0" y="348.5" font-size="8.5" fill="#475569">用途：task 间共享数据</text>
<text x="1536.0" y="362.0" font-size="8.5" fill="#475569">B core 的 head / tail 指针</text>
<text x="1536.0" y="375.5" font-size="8.5" fill="#475569">R core 的 arrive_num 与 tmp_info 表</text>
<text x="1812" y="402.0" font-size="8.5" fill="#9ca3af" text-anchor="end">四个 master 仲裁</text>
<rect x="324" y="364" width="330" height="106" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="336" y="385" font-size="11" fill="#111827" font-weight="600">MU RV core</text>
<text x="336.0" y="402.0" font-size="8.5" fill="#475569">task_queue · dsa_iss（每拍 ≤ 1 条）· dsa_rq 8 项</text>
<text x="336.0" y="415.5" font-size="8.5" fill="#475569">sm_lsq 16</text>
<text x="336.0" y="429.0" font-size="8.5" fill="#475569">ITCM 4 KB · DTCM 8 KB · gpr 就绪表 · 自定义 CSR</text>
<text x="642" y="461" font-size="8.5" fill="#9ca3af" text-anchor="end">src/rv32 逐条执行，每条 1 拍</text>
<rect x="324" y="530" width="330" height="150" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="336" y="551" font-size="11" fill="#111827" font-weight="600">MU DSA（七个模块）</text>
<text x="336.0" y="568.0" font-size="8.5" fill="#475569">regfile · issue_q 16 · gen_ep_info（local_ep_table）</text>
<text x="336.0" y="581.5" font-size="8.5" fill="#475569">agu ×3 与 acu（越界 / 对齐检查 → Drain &amp; Trap）</text>
<text x="336.0" y="595.0" font-size="8.5" fill="#475569">Token ldq 16（Rd outstanding 4 KB）· Weight ldq 4</text>
<text x="336.0" y="608.5" font-size="8.5" fill="#475569">matrix exe：32 lane × 10 级 CSA 树，vlane 1 / 2</text>
<text x="336.0" y="622.0" font-size="8.5" fill="#475569">stq 16 · Wr concat buffer 1～2 KB</text>
<text x="336.0" y="635.5" font-size="8.5" fill="#475569">C = A × B 或 C = C + (A × B) × W_ep</text>
<text x="336.0" y="649.0" font-size="8.5" fill="#475569">先循环 tile_K 再循环 tile_N，task 间三段重叠</text>
<rect x="714" y="364" width="330" height="106" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="726" y="385" font-size="11" fill="#111827" font-weight="600">VU RV core</text>
<text x="726.0" y="402.0" font-size="8.5" fill="#475569">task_queue · dsa_iss（每拍 ≤ 1 条）· dsa_rq 8 项</text>
<text x="726.0" y="415.5" font-size="8.5" fill="#475569">sm_lsq 16</text>
<text x="726.0" y="429.0" font-size="8.5" fill="#475569">ITCM 4 KB · DTCM 8 KB · gpr 就绪表 · 自定义 CSR</text>
<text x="1032" y="461" font-size="8.5" fill="#9ca3af" text-anchor="end">src/rv32 逐条执行，每条 1 拍</text>
<rect x="714" y="530" width="330" height="150" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="726" y="551" font-size="11" fill="#111827" font-weight="600">VU DSA（十一个模块）</text>
<text x="726.0" y="568.0" font-size="8.5" fill="#475569">config_register：8 组静态模板 + 12 个动态参数</text>
<text x="726.0" y="581.5" font-size="8.5" fill="#475569">ISQ · pipe_ctrl + Scoreboard（VRF / MRF / SRF 的依赖）</text>
<text x="726.0" y="595.0" font-size="8.5" fill="#475569">LU · SU：CM 每周期 1 Load + 1 Store，各 128 B，不 burst</text>
<text x="726.0" y="608.5" font-size="8.5" fill="#475569">SMUX / DMUX</text>
<text x="726.0" y="622.0" font-size="8.5" fill="#475569">VALU0 / VALU1 / VALU2 · VSFU · MEXE · SEXE（3 次迭代）</text>
<text x="726.0" y="635.5" font-size="8.5" fill="#475569">VRF 64 KB · MRF 4 KB · SRF 256 B · Profile 计数器</text>
<text x="726.0" y="649.0" font-size="8.5" fill="#475569">最多两条相邻宏指令重叠；CM 访存冲突靠 MACRO_INST_FENCE</text>
<rect x="1104" y="364" width="330" height="106" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1116" y="385" font-size="11" fill="#111827" font-weight="600">DTE RV core</text>
<text x="1116.0" y="402.0" font-size="8.5" fill="#475569">task_queue · dsa_iss（每拍 ≤ 1 条）· dsa_rq 8 项</text>
<text x="1116.0" y="415.5" font-size="8.5" fill="#475569">sm_lsq 16 · cm_lsq 16 · Router I/O reg</text>
<text x="1116.0" y="429.0" font-size="8.5" fill="#475569">ITCM 4 KB · DTCM 8 KB · gpr 就绪表 · 自定义 CSR</text>
<text x="1422" y="461" font-size="8.5" fill="#9ca3af" text-anchor="end">src/rv32 逐条执行，每条 1 拍</text>
<rect x="1104" y="530" width="330" height="150" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1116" y="551" font-size="11" fill="#111827" font-weight="600">DTE DSA（八个模块）</text>
<text x="1116.0" y="568.0" font-size="8.5" fill="#475569">Header Parser（非法头 Drop Frame）· Commit（双 Bank，Bank0 优先）</text>
<text x="1116.0" y="581.5" font-size="8.5" fill="#475569">TaskQueue ×4 · Lane ×4（RD/WR × CH0/CH1，含 AGCU）</text>
<text x="1116.0" y="595.0" font-size="8.5" fill="#475569">中间 Buffer 约 8 KB（read-ahead credit）</text>
<text x="1116.0" y="608.5" font-size="8.5" fill="#475569">Completion RS（按 task_id Join）· Done Pending</text>
<text x="1116.0" y="622.0" font-size="8.5" fill="#475569">Hmem 16 KB + 32 B · LUT 192 B</text>
<text x="1116.0" y="635.5" font-size="8.5" fill="#475569">RouterTable 副本 · 本级 Reduce credit 表 · PendingTaskQ</text>
<text x="1116.0" y="649.0" font-size="8.5" fill="#475569">出方向 VC buffer ×4 · shareMem 写</text>
<rect x="324" y="730" width="361.58000000000004" height="150" rx="4" fill="#fdf6ec" stroke="#b45309"/>
<text x="336" y="751" font-size="11" fill="#111827" font-weight="600">Matrix Mem</text>
<text x="336.0" y="768.0" font-size="8.5" fill="#475569">mmem_bank ×64，每 bank 0.5625 MB，合计 32 + 4 MB（scale : data = 1 : 8）</text>
<text x="336.0" y="781.5" font-size="8.5" fill="#475569">最大带宽 (8 + 1) KB/T；地址粒度 128 B，不支持 byte mask</text>
<text x="336.0" y="795.0" font-size="8.5" fill="#475569">master：DTE DSA 读写 256 B/T（写 9T 读 8T）· MU 只读 (8+1) KB/T（8T）</text>
<text x="336.0" y="808.5" font-size="8.5" fill="#475569">　　　　ctrl_noc 4 B/T（地址对齐 128 B，数据粒度 4 B，burst ≤ 32）</text>
<text x="336.0" y="822.0" font-size="8.5" fill="#475569">硬约束：同一 bank 不许两个 master 同时访问，冲突时只执行 MU 并计数报错</text>
<text x="336.0" y="835.5" font-size="8.5" fill="#475569">SRAM 单 bit 自纠错，纠错后在 SRAM 空闲时写回覆盖</text>
<text x="336.0" y="849.0" font-size="8.5" fill="#475569">三种角色：普通 core 存 weight · B core 存 token · R core 存 reduction</text>
<rect x="725.58" y="730" width="355.8" height="150" rx="4" fill="#fdf6ec" stroke="#b45309"/>
<text x="737.58" y="751" font-size="11" fill="#111827" font-weight="600">Core Mem</text>
<text x="737.6" y="768.0" font-size="8.5" fill="#475569">cmem_bank ×8：SRAM 1024 × 128 B + 每 bank 4 KB scale = 1 MB + 32 KB</text>
<text x="737.6" y="781.5" font-size="8.5" fill="#475569">最大带宽 (1 KB + 32 B)/T；地址粒度 128 B + 4 B，支持 byte mask</text>
<text x="737.6" y="795.0" font-size="8.5" fill="#475569">master：DTE DSA 256 B/T（13T）· MU 132 B/T（11T）· VU 132 B/T（14T）</text>
<text x="737.6" y="808.5" font-size="8.5" fill="#475569">　　　　DTE RV core（128 B / 16 B / 2 B）· Router 重发 · ctrl_noc 4 B/T</text>
<text x="737.6" y="822.0" font-size="8.5" fill="#475569">仲裁：每 bank 二选一；DTE 端口先判读写各自冲突，再判读写之间</text>
<text x="737.6" y="835.5" font-size="8.5" fill="#475569">非同组优先级 MU &gt; VU = DTE；DTE 部分冲突只反压那个 bank</text>
<text x="737.6" y="849.0" font-size="8.5" fill="#475569">按 stream_num 均等分片，base(stream_id) 在 master 侧算</text>
<rect x="324" y="910" width="869.5" height="60" rx="4" fill="#ecfeff" stroke="#0e7490"/>
<text x="336" y="931" font-size="11" fill="#111827" font-weight="600">DTE xbar（DMA_XBAR）</text>
<text x="336.0" y="948.0" font-size="8.5" fill="#475569">DTE DSA 到 Core Mem 与 Matrix Mem 各 256 B/T；Core Mem 8 bank · Matrix Mem 64 bank，命中冲突就排队；MM → CM 的搬移也走这里，VU 不直接读 Matrix Mem</text>
<rect x="234" y="1040" width="1590" height="114" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="246" y="1061" font-size="11" fill="#111827" font-weight="600">Router（八个独立打拍的模块）</text>
<text x="246.0" y="1078.0" font-size="8.5" fill="#475569">RouterStation ×3（left / right / mid）</text>
<text x="246.0" y="1091.5" font-size="8.5" fill="#475569">　Header Parser · VC Buffer ×4 · Packet Context</text>
<text x="246.0" y="1105.0" font-size="8.5" fill="#475569">　Stream Resource Table · VC Credit 计数器 · Credit Release 静态旁路</text>
<text x="246.0" y="1118.5" font-size="8.5" fill="#475569">Xbar 5 入 7 出：按输出 RoundRobin，贪婪整包，多播全有全无</text>
<text x="1029.0" y="1078.0" font-size="8.5" fill="#475569">CoreStation：HeaderFIFO · OutputBuffer · 三态准入 · 进出 core 并行</text>
<text x="1029.0" y="1091.5" font-size="8.5" fill="#475569">ReduceModule：16 用户 × 16 KiB · RMW FP32 累加 · 下游 Reduce credit 表</text>
<text x="1029.0" y="1105.0" font-size="8.5" fill="#475569">RouterTable / CSR（64 项，多副本提交）· CoreMem 重发</text>
<text x="1029.0" y="1118.5" font-size="8.5" fill="#475569">Retire · CoreMemCreditMonitor（监听事件队列 16 项全相连）</text>
<text x="1812" y="1145" font-size="8.5" fill="#9ca3af" text-anchor="end">每 Core 一份，不派角色的 core 也有</text>
<polygon points="125.0,44 228.0,44 219.0,74 116.0,74" fill="#f8fafc" stroke="#374151"/>
<text x="172.0" y="58.0" font-size="9" fill="#374151" text-anchor="middle">async_int → SCP</text>
<text x="172.0" y="69.0" font-size="7.5" fill="#6b7280" text-anchor="middle">core 的中断异常信息</text>
<polygon points="1623.0,44 1734.0,44 1725.0,74 1614.0,74" fill="#f8fafc" stroke="#374151"/>
<text x="1674.0" y="58.0" font-size="9" fill="#374151" text-anchor="middle">scp_ctrl</text>
<text x="1674.0" y="69.0" font-size="7.5" fill="#6b7280" text-anchor="middle">SCP → ctrl_noc，32 bit/T</text>
<polygon points="1845.0,44 1936.0,44 1927.0,74 1836.0,74" fill="#f8fafc" stroke="#374151"/>
<text x="1886.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">dmi_ch ← DTM</text>
<polygon points="21,1081.0 116,1081.0 107,1113.0 12,1113.0" fill="#f8fafc" stroke="#374151"/>
<text x="64.0" y="1096.0" font-size="9" fill="#374151" text-anchor="middle">data_L</text>
<text x="64.0" y="1107.0" font-size="7.5" fill="#6b7280" text-anchor="middle">left · 256 B/T</text>
<polygon points="1951,1081.0 2046,1081.0 2037,1113.0 1942,1113.0" fill="#f8fafc" stroke="#374151"/>
<text x="1994.0" y="1096.0" font-size="9" fill="#374151" text-anchor="middle">data_R</text>
<text x="1994.0" y="1107.0" font-size="7.5" fill="#6b7280" text-anchor="middle">right · 256 B/T</text>
<polygon points="978.0,1214 1089.0,1214 1080.0,1246 969.0,1246" fill="#f8fafc" stroke="#374151"/>
<text x="1029.0" y="1229.0" font-size="9" fill="#374151" text-anchor="middle">data_UD</text>
<text x="1029.0" y="1240.0" font-size="7.5" fill="#6b7280" text-anchor="middle">mid · 256 B/T</text>
<path d="M246.0 1040.0 L246.0 245.0" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#g)"/>
<rect x="241.0" y="376.2" width="10" height="95.6" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 246 424)" x="246" y="426.8" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8" fill="#0f766e" text-anchor="middle">router2ts_trigger_ch</text>
<path d="M262.0 1040.0 L262.0 245.0" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#g)"/>
<rect x="257.0" y="498.4" width="10" height="91.1" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 262 544)" x="262" y="546.8" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8" fill="#0f766e" text-anchor="middle">router2ts_credit_ch</text>
<path d="M278.0 1040.0 L278.0 245.0" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#g)"/>
<rect x="273.0" y="627.4" width="10" height="73.2" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 278 664)" x="278" y="666.8" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8" fill="#0f766e" text-anchor="middle">rmem2ts_done_ch</text>
<path d="M294.0 244.0 L294.0 1039.0" stroke="#b45309" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#o)"/>
<rect x="289.0" y="742.6" width="10" height="82.8" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 294 784)" x="294" y="786.8" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8" fill="#b45309" text-anchor="middle">ts2router 资源注册</text>
<path d="M310.0 244.0 L310.0 1039.0" stroke="#b45309" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#o)"/>
<rect x="305.0" y="857.2" width="10" height="93.7" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 310 904)" x="310" y="906.8" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8" fill="#b45309" text-anchor="middle">retire · credit 返还</text>
<path d="M489.0 245.0 L489.0 363.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="491.8" y="232.0" width="10.5" height="144.0" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 497.0 304.0)" x="497.0" y="307.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">task_cmd / task_ack · rv_done</text>
<path d="M489.0 471.0 L489.0 529.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="491.8" y="480.3" width="10.5" height="39.3" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 497.0 500.0)" x="497.0" y="503.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">dsa_cfg</text>
<path d="M879.0 245.0 L879.0 363.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M879.0 471.0 L879.0 529.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1269.0 245.0 L1269.0 363.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1269.0 471.0 L1269.0 529.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M621.0 530.0 L621.0 510.0" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round"/>
<path d="M1011.0 530.0 L1011.0 510.0" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round"/>
<path d="M1401.0 530.0 L1401.0 510.0" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round"/>
<path d="M621.0 510.0 L1474.0 510.0 L1474.0 274.0 L1417.5 274.0" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round"/>
<path d="M1417.5 274.0 L1417.5 245.0" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#g)"/>
<text x="637.0" y="522" font-size="8.5" fill="#0f766e" text-anchor="start">dsa_done ×3 · VU 的 Event 同步信号 → Task_done（绿，DSA 顶边汇成一路上行）</text>
<path d="M555.0 490.0 L555.0 471.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M945.0 490.0 L945.0 471.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M1335.0 490.0 L1335.0 471.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M555.0 490.0 L1832.0 490.0 L1832.0 349.2 L1825.0 349.2" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<text x="571.0" y="485" font-size="8.5" fill="#6b7280" text-anchor="start">sm_lsq ×3（32 bit，顺序发射，5～10 拍）</text>
<path d="M489.0 680.0 L489.0 729.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="491.8" y="653.0" width="10.5" height="103.9" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 497.0 705.0)" x="497.0" y="708.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">mmem_mu 只读 (8+1) KB</text>
<path d="M879.0 681.0 L879.0 729.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="881.8" y="641.1" width="10.5" height="127.7" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 887.0 705.0)" x="887.0" y="708.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">cmem_vu 1056 bit，不 burst</text>
<path d="M621.0 681.0 L621.0 705.0 L765.6 705.0 L765.6 729.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="659.3" y="693.5" width="67.9" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="693.29" y="701" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">cmem_mu 132 B</text>
<path d="M1153.5 681.0 L1153.5 909.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="1156.2" y="750.7" width="10.5" height="88.6" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 1161.5 795.0)" x="1161.5" y="798.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">DMA 读写各 256 B/T</text>
<path d="M468.6 909.0 L468.6 881.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="440.3" y="887.5" width="72.6" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="476.63200000000006" y="895.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">mmem_dte 256 B</text>
<path d="M939.1 909.0 L939.1 881.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="910.7" y="887.5" width="72.6" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="947.0600000000001" y="895.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">cmem_dte 256 B</text>
<path d="M1302.0 1040.0 L1302.0 681.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1288.8" y="821.3" width="10.5" height="77.4" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 1294.0 860.0)" x="1294.0" y="863.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">in_core_data_ch</text>
<path d="M1368.0 680.0 L1368.0 1039.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1370.8" y="818.9" width="10.5" height="82.2" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 1376.0 860.0)" x="1376.0" y="863.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">out_core_data_ch</text>
<text transform="rotate(-90 1398.0 920.0)" x="1398.0" y="920.0" font-size="8" fill="#6b7280" text-anchor="middle">AXI-Stream-Like + VC credit</text>
<path d="M172.0 130.0 L167.6 75.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#a)"/>
<path d="M1669.5 74.0 L1673.9 129.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<path d="M1794.0 216.5 L1794.0 255.5" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<rect x="1796.9" y="232.0" width="54.3" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1824.0" y="239.5" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#7c3aed" text-anchor="middle">cfg（示例）</text>
<path d="M1881.5 74.0 L1885.9 129.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#a)"/>
<path d="M234.0 187.0 L201.0 187.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#a)"/>
<rect x="218.2" y="196.7" width="9.5" height="60.6" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 223.0 227.0)" x="223.0" y="229.6" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="7.5" fill="#475569" text-anchor="middle">ts2corestatus</text>
<path d="M112.5 1097.0 L233.0 1097.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1825.0 1097.0 L1945.5 1097.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1029.1 1155.0 L1033.4 1213.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<text x="1524" y="546" font-size="8.5" fill="#4b5563" text-anchor="start">连线说明（灰双向箭头两端都是主动方）：</text>
<text x="1524" y="559.5" font-size="8.5" fill="#4b5563" text-anchor="start">TS ↔ RV core：task_cmd（task_pc · stream_id ·</text>
<text x="1524" y="573.0" font-size="8.5" fill="#4b5563" text-anchor="start">　local_user_id）/ task_ack；rv_done（stream_id ·</text>
<text x="1524" y="586.5" font-size="8.5" fill="#4b5563" text-anchor="start">　local_user_id · task_id）</text>
<text x="1524" y="600.0" font-size="8.5" fill="#4b5563" text-anchor="start">RV core ↔ DSA：dsa_cfg：dsaw / dsawi 写寄存器 +</text>
<text x="1524" y="613.5" font-size="8.5" fill="#4b5563" text-anchor="start">　写 trigger 启动；dsar / dsari 读不阻塞，按 dsa_rq</text>
<text x="1524" y="627.0" font-size="8.5" fill="#4b5563" text-anchor="start">　顺序写回 gpr</text>
<text x="1524" y="640.5" font-size="8.5" fill="#4b5563" text-anchor="start">DTE RV core ↔ Core Mem：cm_lsq（固定读回 1056 bit，</text>
<text x="1524" y="654.0" font-size="8.5" fill="#4b5563" text-anchor="start">　32 bit / 拍）；DTE RV core ↔ Router：Router I/O reg。</text>
<text x="1524" y="667.5" font-size="8.5" fill="#4b5563" text-anchor="start">　这两条只有 DTE 列有，图中不单画</text>
<text x="1524" y="681.0" font-size="8.5" fill="#4b5563" text-anchor="start">三个 R2R 方向各 256 B/T 双向，接相邻 core 的 Router</text>
<text x="1524" y="694.5" font-size="8.5" fill="#4b5563" text-anchor="start">　或 chip 边界的 C2C Bridge；线上跑 flit，另有</text>
<text x="1524" y="708.0" font-size="8.5" fill="#4b5563" text-anchor="start">　vc_release / stream_release / reduce_release 回程</text>
<text x="20" y="1298" font-size="10.5" fill="#374151" text-anchor="start">Core 不打拍，是装配容器：构造上面全部模块，按各单元文档声明的端口组对接。不派角色的 core 只构造 Router 的八个模块，其余一律不构造。Router 贴底边朝 chip 中部：data_L / data_R 走左右，data_UD 走底边。</text>
</svg>
```

***

## 2　功能清单

一功能一条，编号供“机制覆盖”一章引用。Core 自身不打拍，功能分两类：构造期的接线，以及跨单元、不归任何单个单元的那几条约定。

### 装配（构造期，不打拍）

| 编号 | 功能 |
| - | - |
| F1 | `router_only` 为假时构造七个单元的全部模块：Router 八个、TS 九个、三个 RV core、三个 DSA 的各模块、三块存储与 ctrl_noc 端点 |
| F2 | `router_only` 为真时只构造 Router 的八个模块，这一档只有边界 chip 里不派角色的那个 core 用。CoreStation 永远不准入，ReduceModule 不累加，CoreMemCreditMonitor 空转，`stream_credit` 上电默认 0 |
| F3 | 按各单元文档声明的端口组把生产者的出口端口与消费者的入口端口对接；两侧只看到端口束的字段，不持有对方的类型，装配顺序不受构造顺序牵制 |
| F4 | 把 Router 三个 RouterStation 的对外端口引到 `data_L` / `data_UD` / `data_R` |
| F5 | 把 ctrl_noc 端点的入口引到 `cfg`，出口按 `addr_map` 接到各模块的 `cfg` 口 |
| F6 | 建立只读的 `core_context`（`core_id`、全局坐标、角色、`router_only`），core 内各模块共用 |

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
mem core_context   FF   {core_id[3:0], gx[1:0], gy[3:0], role[2:0], router_only}   1R   构造期写入   复位由输入给   // 各模块共用的只读上下文
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
CORE_PER_CHIP   中间列 chip 8（2×4），第一列与最后一列 chip 10（2×5）；都按 row-major 编号
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
| 不派角色的 core 只构造 Router 的八个模块 | F2 | `spare_core_router_only` |
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
* **不派角色的 core 为什么仍然构造 Router**
  * 它要承担单向转发、router multicast、router-level reduce，以及三类 credit 的透传，还坐在 chip 接 PCIe Switch 的那个口上
* **为什么地址映射交给硬件而不是软件**
  * 多用户复用同一套 kernel 代码，软件只能用统一固定的虚拟偏移地址，没法为每个用户单独改地址、单独编译
  * 纯软件管理会让相同虚拟地址落到同一块物理内存，多用户互相覆盖
* **为什么单独做一块 Share Mem**
  * Core Mem 容量大、物理距离远，访问延时 15～25 拍，顺序执行的 RV core 掩盖不了
  * Share Mem 容量小、距离近，延时 5～10 拍，用来加速三个 RV core 的 task 之间传数据
