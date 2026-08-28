# Router

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **Router**

给实现 Router 的人：八个独立打拍的模块各自做哪些事、端口与存储怎么定。每个 Core 一份，坏核也有。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《Router 片上交换与归约》全篇
* 《归约的完整过程》：“第二层：chip 内 core 间的逐跳累加”“Reduce credit 的闭环”
* 《软件栈》：“Router 软件视角”“阻塞重传四步”“使用示例：C6、C7 双坏核”

***

## 1　定位与边界

Router 是 core 与片上网络之间的交换点，同时承担三件事：包的路由转发、三层流控、Reduce 计算。

对外三组连接：

* **三个 R2R 方向端口**：`left` 与 `right` 连同行相邻的 Router，`mid` 连另一行对称位置的 Router。每个 256 B/T 双向，接相邻 core 的 Router 或 chip 边界的 C2C Bridge。边沿 Router 通过配置禁用不存在的端口
* **进 core 与出 core 各一条通路**，接本 core 的 DTE
* **控制信号**接 TS

三类 credit 互不复用，管的东西、粒度、维护方、扣还时机都不同：

| | VC credit | Stream 资源（CoreMem credit） | Reduce credit |
| - | - | - | - |
| 管什么 | 下游 VC Buffer 有没有空槽 | 目标 core 的 Core Mem 有没有空间容纳这个用户的数据 | 下游 ReduceModule 的上下文有没有空间 |
| 粒度 | 按下游方向加 VC，flit | 按 UserID 加目标方向，一个表项 | 按 UserID，flit |
| 谁维护 | RouterStation 的独立计数器，硬件自动维护 | Router 是唯一有效状态；DTE 持一份 cache；TS 内另有一份本级表，按与 Router 完全一致的逻辑分配空项 | DTE 维护本级的，ReduceModule 维护相邻下游各方向的，Router 不维护 |
| 何时扣 | flit 发出时扣该方向该 VC 一个；经 CoreMem 重注入的包，Output Port 识别到重注入标记才扣 | 新 UserID 的包在本级占一个表项；出核的包由 DTE 先向 Router 申请到授权 | 每发一个 flit 扣一个；DTE 发 Reduce 包前要求本级 credit 够整包 |
| 何时还 | flit 离开下游 VC Buffer 就归还 | 用户在下游 core 跑完任务链、用完 Core Mem 后发携带 UserID 的 release，逐跳传到上游 | 输出 flit 被下游接受后产生携带 UserID 的 release，经静态旁路返回上游 |
| 快慢 | 快，flit 一进一出就还 | 慢，要等那个用户在下游 core 上跑完整条任务链 | 介于两者之间，按 flit 还但要等下游 Reduce 完成 |

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1700 1250" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
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
  <rect x="0" y="0" width="1700" height="1250" fill="#ffffff"/>
  <text x="20" y="26" font-size="12" fill="#111827">Router · 第 0 层（八个独立打拍的模块；每 Core 一份，坏核也有）</text>
  <text x="460" y="26" font-size="9.5" fill="#6b7280">灰线 = flit 数据面　橙线 = 三类 credit 与 release　绿线 = 与 TS 的控制通路　紫虚线 = ctrl_noc 配置</text>
  <polygon points="36,142 160,142 151,174 27,174" fill="#f8fafc" stroke="#374151"/>
  <text x="94" y="162" font-size="9" fill="#374151" text-anchor="middle">left_data_ch</text>
  <rect x="196" y="80" width="340" height="186" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="208" y="101" font-size="11" fill="#111827">RouterStation[left]</text>
  <text x="208" y="118" font-size="8.5" fill="#475569">Header Parser：取 path_id · user_id · size</text>
  <text x="208" y="131.5" font-size="8.5" fill="#475569">　operation · directionMask · vc_id，查 RouterTable</text>
  <text x="208" y="145.0" font-size="8.5" fill="#475569">VC Buffer ×4（private 2 + shared pool 约 20 flit）</text>
  <text x="208" y="158.5" font-size="8.5" fill="#475569">Packet Context：VC · 输出方向 · 剩余长度 · 包边界</text>
  <text x="208" y="172.0" font-size="8.5" fill="#475569">Stream Resource Table：下游各方向的 UserID 占用</text>
  <text x="208" y="185.5" font-size="8.5" fill="#475569">VC Credit 计数器：每下游方向每 VC 一个</text>
  <text x="208" y="199.0" font-size="8.5" fill="#475569">Output Buffer + Packet Shifter（按总线宽度拼接）</text>
  <text x="208" y="212.5" font-size="8.5" fill="#475569">Credit Release 静态旁路：按 CSR 的方向 Mask 转发</text>
  <text x="208" y="226.0" font-size="8.5" fill="#475569">坏核：只透传，不查 credit、不支持阻塞重发</text>
  <polygon points="36,358 160,358 151,390 27,390" fill="#f8fafc" stroke="#374151"/>
  <text x="94" y="378" font-size="9" fill="#374151" text-anchor="middle">right_data_ch</text>
  <rect x="196" y="296" width="340" height="186" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="208" y="317" font-size="11" fill="#111827">RouterStation[right]</text>
  <text x="208" y="334" font-size="8.5" fill="#475569">Header Parser：取 path_id · user_id · size</text>
  <text x="208" y="347.5" font-size="8.5" fill="#475569">　operation · directionMask · vc_id，查 RouterTable</text>
  <text x="208" y="361.0" font-size="8.5" fill="#475569">VC Buffer ×4（private 2 + shared pool 约 20 flit）</text>
  <text x="208" y="374.5" font-size="8.5" fill="#475569">Packet Context：VC · 输出方向 · 剩余长度 · 包边界</text>
  <text x="208" y="388.0" font-size="8.5" fill="#475569">Stream Resource Table：下游各方向的 UserID 占用</text>
  <text x="208" y="401.5" font-size="8.5" fill="#475569">VC Credit 计数器：每下游方向每 VC 一个</text>
  <text x="208" y="415.0" font-size="8.5" fill="#475569">Output Buffer + Packet Shifter（按总线宽度拼接）</text>
  <text x="208" y="428.5" font-size="8.5" fill="#475569">Credit Release 静态旁路：按 CSR 的方向 Mask 转发</text>
  <text x="208" y="442.0" font-size="8.5" fill="#475569">坏核：只透传，不查 credit、不支持阻塞重发</text>
  <polygon points="36,574 160,574 151,606 27,606" fill="#f8fafc" stroke="#374151"/>
  <text x="94" y="594" font-size="9" fill="#374151" text-anchor="middle">mid_data_ch</text>
  <rect x="196" y="512" width="340" height="186" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="208" y="533" font-size="11" fill="#111827">RouterStation[mid]</text>
  <text x="208" y="550" font-size="8.5" fill="#475569">Header Parser：取 path_id · user_id · size</text>
  <text x="208" y="563.5" font-size="8.5" fill="#475569">　operation · directionMask · vc_id，查 RouterTable</text>
  <text x="208" y="577.0" font-size="8.5" fill="#475569">VC Buffer ×4（private 2 + shared pool 约 20 flit）</text>
  <text x="208" y="590.5" font-size="8.5" fill="#475569">Packet Context：VC · 输出方向 · 剩余长度 · 包边界</text>
  <text x="208" y="604.0" font-size="8.5" fill="#475569">Stream Resource Table：下游各方向的 UserID 占用</text>
  <text x="208" y="617.5" font-size="8.5" fill="#475569">VC Credit 计数器：每下游方向每 VC 一个</text>
  <text x="208" y="631.0" font-size="8.5" fill="#475569">Output Buffer + Packet Shifter（按总线宽度拼接）</text>
  <text x="208" y="644.5" font-size="8.5" fill="#475569">Credit Release 静态旁路：按 CSR 的方向 Mask 转发</text>
  <text x="208" y="658.0" font-size="8.5" fill="#475569">坏核：只透传，不查 credit、不支持阻塞重发</text>
  <rect x="596" y="80" width="150" height="618" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="608" y="101" font-size="11" fill="#111827">Xbar</text>
  <text x="608" y="118" font-size="8.5" fill="#475569">5 入 7 出</text>
  <text x="608" y="131.5" font-size="8.5" fill="#475569"></text>
  <text x="608" y="145.0" font-size="8.5" fill="#475569">入：left · right</text>
  <text x="608" y="158.5" font-size="8.5" fill="#475569">　　mid · local</text>
  <text x="608" y="172.0" font-size="8.5" fill="#475569">　　reduce 回注</text>
  <text x="608" y="185.5" font-size="8.5" fill="#475569"></text>
  <text x="608" y="199.0" font-size="8.5" fill="#475569">出：left · right</text>
  <text x="608" y="212.5" font-size="8.5" fill="#475569">　　mid · core</text>
  <text x="608" y="226.0" font-size="8.5" fill="#475569">　　reduce_0/1/2</text>
  <text x="608" y="239.5" font-size="8.5" fill="#475569"></text>
  <text x="608" y="253.0" font-size="8.5" fill="#475569">按输出独立</text>
  <text x="608" y="266.5" font-size="8.5" fill="#475569">RoundRobin</text>
  <text x="608" y="280.0" font-size="8.5" fill="#475569"></text>
  <text x="608" y="293.5" font-size="8.5" fill="#475569">贪婪整包：</text>
  <text x="608" y="307.0" font-size="8.5" fill="#475569">整包 &gt; 上包 body</text>
  <text x="608" y="320.5" font-size="8.5" fill="#475569">&gt; 轮询</text>
  <text x="608" y="334.0" font-size="8.5" fill="#475569"></text>
  <text x="608" y="347.5" font-size="8.5" fill="#475569">多播同拍复制</text>
  <text x="608" y="361.0" font-size="8.5" fill="#475569">全有或全无</text>
  <text x="608" y="374.5" font-size="8.5" fill="#475569"></text>
  <text x="608" y="388.0" font-size="8.5" fill="#475569">入口锁定到尾 flit</text>
  <text x="608" y="401.5" font-size="8.5" fill="#475569">（进 core 或</text>
  <text x="608" y="415.0" font-size="8.5" fill="#475569">ReduceModule）</text>
  <text x="608" y="428.5" font-size="8.5" fill="#475569"></text>
  <text x="608" y="442.0" font-size="8.5" fill="#475569">R2R 可在 flit</text>
  <text x="608" y="455.5" font-size="8.5" fill="#475569">边界切换包</text>
  <polyline points="160,158 196,158" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="536,158 566,158 566,170 596,170" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="160,374 196,374" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="536,374 566,374 566,386 596,386" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="160,590 196,590" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="536,590 566,590 566,602 596,602" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <rect x="806" y="80" width="400" height="254" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="818" y="101" font-size="11" fill="#111827">CoreStation</text>
  <text x="818" y="118" font-size="8.5" fill="#475569">HeaderFIFO：按接收顺序存包头，DTE 读完写 1 弹出</text>
  <text x="818" y="131.5" font-size="8.5" fill="#475569">OutputBuffer（in_core_fifo）：整包写入，不支持包间交织</text>
  <text x="818" y="145.0" font-size="8.5" fill="#475569">三态准入：UserID 已分配 → 直接收；未分配但有空项 → 记录占用；</text>
  <text x="818" y="158.5" font-size="8.5" fill="#475569">　无空项 → 该 VC 不能向 Core 发，但 VC 有空仍可继续收上游</text>
  <text x="818" y="172.0" font-size="8.5" fill="#475569">已过 Stream 检查，进 core 不再查 VC credit</text>
  <text x="818" y="185.5" font-size="8.5" fill="#475569">出 core：Core 方向输入 VC，与 DTE 之间按 VC credit 协议</text>
  <text x="818" y="199.0" font-size="8.5" fill="#475569">反压时 valid / Header / Payload / 首尾标志 / 有效字节保持不变</text>
  <text x="818" y="212.5" font-size="8.5" fill="#475569">收满一个包按顺序经 router2ts_trigger_ch 直接通知 TS</text>
  <text x="818" y="226.0" font-size="8.5" fill="#475569">判定收完：比较已接收数据量与包头里的 payload 大小</text>
  <text x="818" y="239.5" font-size="8.5" fill="#475569">进 core 与出 core 两条路完全并行，不共享仲裁状态</text>
  <rect x="806" y="384" width="400" height="314" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="818" y="405" font-size="11" fill="#111827">ReduceModule</text>
  <text x="818" y="422" font-size="8.5" fill="#475569">三路输入仲裁（Data ×3）：仲裁 SRAM / Bank / 计算资源</text>
  <text x="818" y="435.5" font-size="8.5" fill="#475569">　进入后锁定当前包直至尾 flit；资源不足对输入反压</text>
  <text x="818" y="449.0" font-size="8.5" fill="#475569">输入精度处理：BF16 扩展为 FP32，数据面统一 FP32</text>
  <text x="818" y="462.5" font-size="8.5" fill="#475569">Reduce Context SRAM：16 用户 × 16 KiB，FP32 中间结果</text>
  <text x="818" y="476.0" font-size="8.5" fill="#475569">RMW 管线：首份输入建上下文，后续输入原位累加，80 GFLOPS</text>
  <text x="818" y="489.5" font-size="8.5" fill="#475569">User Context Table：UserID · 包状态 · 输入完成 · 输出状态 · Retire</text>
  <text x="818" y="503.0" font-size="8.5" fill="#475569">RouterTable Copy：输出方向 · 下一跳 VC · operation · 输出精度</text>
  <text x="818" y="516.5" font-size="8.5" fill="#475569">结果生成与发送：全部输入完成 → 输出队列 → 转 FP32 / BF16</text>
  <text x="818" y="530.0" font-size="8.5" fill="#475569">　发送前查目标 VC credit 与该方向的下游 Reduce credit</text>
  <text x="818" y="543.5" font-size="8.5" fill="#475569">Downstream Reduce Credit Map：按 UserID 加方向，逐 flit 扣、</text>
  <text x="818" y="557.0" font-size="8.5" fill="#475569">　按 release 恢复</text>
  <text x="818" y="570.5" font-size="8.5" fill="#475569">三条硬约束：必须执行 Reduce 不许降级 · 上下文保护 ·</text>
  <text x="818" y="584.0" font-size="8.5" fill="#475569">　中间累加固定 FP32</text>
  <text x="818" y="597.5" font-size="8.5" fill="#475569">整包发出后向 core 返回 UserID</text>
  <text x="1194" y="689" font-size="8.5" fill="#9ca3af" text-anchor="end">输入三路各 160 GB/s，输出 160 GB/s</text>
  <polyline points="746,142 776,142 776,141 806,141" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <text x="776" y="110" font-size="8.5" fill="#6b7280" text-anchor="middle">→ core</text>
  <polyline points="806,298 776,298 776,302 746,302" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <text x="776" y="306" font-size="8.5" fill="#6b7280" text-anchor="middle">local ←</text>
  <polyline points="746,451 776,451 776,441 806,441" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <text x="776" y="436" font-size="8.5" fill="#6b7280" text-anchor="middle">→ reduce ×3</text>
  <polyline points="806,673 776,673 776,636 746,636" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <text x="776" y="646" font-size="8.5" fill="#6b7280" text-anchor="middle">结果回注 ←</text>
  <polygon points="1250,150 1410,150 1401,182 1241,182" fill="#f8fafc" stroke="#374151"/>
  <text x="1326" y="170" font-size="9" fill="#374151" text-anchor="middle">in_core_data_ch</text>
  <polygon points="1250,262 1410,262 1401,294 1241,294" fill="#f8fafc" stroke="#374151"/>
  <text x="1326" y="282" font-size="9" fill="#374151" text-anchor="middle">out_core_data_ch</text>
  <polyline points="1206,141 1224,141 1224,166 1241,166" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <text x="1228" y="158" font-size="8.5" fill="#6b7280" text-anchor="middle">AXI-Stream-Like</text>
  <polyline points="1241,278 1224,278 1224,258 1206,258" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <text x="1228" y="300" font-size="8.5" fill="#6b7280" text-anchor="middle">AXI-Stream-Like</text>
  <text x="1252" y="330" font-size="8.5" fill="#6b7280" text-anchor="start">↔ 本 core 的 DTE DSA（VC credit 协议）</text>
  <rect x="60" y="770" width="330" height="232" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="72" y="791" font-size="11" fill="#111827">RouterTable / CSR</text>
  <text x="72" y="808" font-size="8.5" fill="#475569">64 条表项，索引 path_id</text>
  <text x="72" y="821.5" font-size="8.5" fill="#475569">字段：curVC · directionMask · nxtVC · streamNeedMask</text>
  <text x="72" y="835.0" font-size="8.5" fill="#475569">　　　operation · stallWay · reducePrecision</text>
  <text x="72" y="848.5" font-size="8.5" fill="#475569">只描述静态路由与资源需求，不保存包的动态状态</text>
  <text x="72" y="862.0" font-size="8.5" fill="#475569">内部多副本：所有需并行查询的位置各一份</text>
  <text x="72" y="875.5" font-size="8.5" fill="#475569">更新状态机把同一笔写依次写入全部副本并记完成</text>
  <text x="72" y="889.0" font-size="8.5" fill="#475569">全部副本写完才向软件返回完成，禁止部分新部分旧</text>
  <text x="72" y="902.5" font-size="8.5" fill="#475569">外部两份（DTE、ReduceModule）由软件写，硬件不同步</text>
  <text x="72" y="916.0" font-size="8.5" fill="#475569">Credit Bypass Route：每个业务 credit 输入端口一张</text>
  <text x="72" y="929.5" font-size="8.5" fill="#475569">　静态输出方向 Mask，坏核场景靠改它切换 credit 路径</text>
  <text x="378" y="993" font-size="8.5" fill="#9ca3af" text-anchor="end">软件经 R2CU 接口配置</text>
  <rect x="420" y="770" width="330" height="232" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="432" y="791" font-size="11" fill="#111827">CoreMem 重发</text>
  <text x="432" y="808" font-size="8.5" fill="#475569">stallWay = 转存时：整包重定向到本地 Core Mem</text>
  <text x="432" y="821.5" font-size="8.5" fill="#475569">Bypass 被映射成“进 core + 出 core”两段</text>
  <text x="432" y="835.0" font-size="8.5" fill="#475569">CoreMem 中只保存包（含 UserID · PathID · size）</text>
  <text x="432" y="848.5" font-size="8.5" fill="#475569">重发时用 PathID 重查 RouterTable，不重复保存 VC 与路由</text>
  <text x="432" y="862.0" font-size="8.5" fill="#475569">同 VC 保序：该 VC 有未完成的重发包时后续包不得越过</text>
  <text x="432" y="875.5" font-size="8.5" fill="#475569">按 VC 粒度维护 pending_reinject 计数器防超车</text>
  <text x="432" y="889.0" font-size="8.5" fill="#475569">进 core 暂存时改写 overflow_reinject = 1</text>
  <text x="432" y="902.5" font-size="8.5" fill="#475569">出 core 重发时改回 0；Output Port 识别到该标记才扣 credit</text>
  <text x="432" y="916.0" font-size="8.5" fill="#475569">坏核不接收溢流，coremem credit 直接 bypass</text>
  <text x="432" y="929.5" font-size="8.5" fill="#475569">无论直接发还是重发，完成后都向 TS 回 UserID + PathID</text>
  <rect x="780" y="770" width="320" height="232" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="792" y="791" font-size="11" fill="#111827">Retire</text>
  <text x="792" y="808" font-size="8.5" fill="#475569">Core 判定任务链结束后向 Router 与 ReduceModule 广播</text>
  <text x="792" y="821.5" font-size="8.5" fill="#475569">Core 的保证：该 UserID 全部进 core、出 core 搬运完成</text>
  <text x="792" y="835.0" font-size="8.5" fill="#475569">　且不会再发起新搬运之后才发 Retire</text>
  <text x="792" y="848.5" font-size="8.5" fill="#475569">Retire 发出后 Router 上不得再出现以该 Core 为源或</text>
  <text x="792" y="862.0" font-size="8.5" fill="#475569">　目标的该用户包</text>
  <text x="792" y="875.5" font-size="8.5" fill="#475569">Router 的动作：停止该 UserID 的新发送，删除其全部</text>
  <text x="792" y="889.0" font-size="8.5" fill="#475569">　Stream 资源授权表项</text>
  <text x="792" y="902.5" font-size="8.5" fill="#475569">ReduceModule 的动作：延迟回收。先记录 Retire，待相邻</text>
  <text x="792" y="916.0" font-size="8.5" fill="#475569">　下游各方向的 Reduce credit 全恢复到初值才删映射</text>
  <text x="792" y="929.5" font-size="8.5" fill="#475569">本级 core 与所有下级出口的 release 经 core credit crossbar</text>
  <text x="792" y="943.0" font-size="8.5" fill="#475569">　汇总，发往除来向外的另两个 R2R port，逐跳传到上游</text>
  <rect x="1130" y="770" width="340" height="232" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="1142" y="791" font-size="11" fill="#111827">CoreMemCreditMonitor</text>
  <text x="1142" y="808" font-size="8.5" fill="#475569">监听事件队列：16 项全相连</text>
  <text x="1142" y="821.5" font-size="8.5" fill="#475569">TS 注册时带 UserID · StreamID · TaskID · PathID</text>
  <text x="1142" y="835.0" font-size="8.5" fill="#475569">Router 按 PathID 查出要发的下游方向、VC 与 stream 需求</text>
  <text x="1142" y="848.5" font-size="8.5" fill="#475569">申请到 → 经反向控制通路通知 TS</text>
  <text x="1142" y="862.0" font-size="8.5" fill="#475569">　（回 StreamID · TaskID · PathID）</text>
  <text x="1142" y="875.5" font-size="8.5" fill="#475569">申请不到 → 需求记进队列监听，资源满足再通知</text>
  <text x="1142" y="889.0" font-size="8.5" fill="#475569">多个事件同时满足时按 StreamID 仲裁，选最老的通知 TS</text>
  <text x="1142" y="902.5" font-size="8.5" fill="#475569">进 core 重发的任务也注册到该队列</text>
  <text x="1142" y="916.0" font-size="8.5" fill="#475569">同一 VC 有未重发完的包时后续包不能提前发</text>
  <text x="1142" y="929.5" font-size="8.5" fill="#475569">另输出 per-port 的 coremem_credit 给 core 与 DTE</text>
  <text x="1458" y="993" font-size="8.5" fill="#9ca3af" text-anchor="end">出核前的资源监听在 Router，不在 TS</text>
  <polyline points="159,770 159,724 298,724 298,698" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#a)"/>
  <polyline points="350,770 350,738 886,738 886,698" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#a)"/>
  <polyline points="559,770 559,712 671,712 671,698" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#a)"/>
  <polyline points="1184,770 1184,752 1102,752 1102,698" fill="none" stroke="#b45309" stroke-dasharray="4 3" marker-end="url(#o)"/>
  <polyline points="1062,770 1062,734 1014,734 1014,698" fill="none" stroke="#b45309" stroke-dasharray="4 3" marker-end="url(#o)"/>
  <text x="64" y="700" font-size="8.5" fill="#6b7280" text-anchor="start">查表结果 → 各 RouterStation 与 ReduceModule；Reduce credit 与 Retire 的回收 → ReduceModule</text>
  <polygon points="1516,110 1688,110 1679,142 1507,142" fill="#f8fafc" stroke="#374151"/>
  <text x="1598" y="130" font-size="8.5" fill="#374151" text-anchor="middle">router2ts_trigger_ch</text>
  <polygon points="1516,172 1688,172 1679,204 1507,204" fill="#f8fafc" stroke="#374151"/>
  <text x="1598" y="192" font-size="8.5" fill="#374151" text-anchor="middle">router2ts_credit_ch</text>
  <polygon points="1516,234 1688,234 1679,266 1507,266" fill="#f8fafc" stroke="#374151"/>
  <text x="1598" y="254" font-size="8.5" fill="#374151" text-anchor="middle">rmem2ts_done_ch</text>
  <polygon points="1516,296 1688,296 1679,328 1507,328" fill="#f8fafc" stroke="#374151"/>
  <text x="1598" y="316" font-size="8.5" fill="#374151" text-anchor="middle">ts2router 资源注册</text>
  <polygon points="1516,358 1688,358 1679,390 1507,390" fill="#f8fafc" stroke="#374151"/>
  <text x="1598" y="378" font-size="8.5" fill="#374151" text-anchor="middle">ts2router retire / credit 返还</text>
  <polyline points="1166,334 1166,352 1440,352 1440,126 1507,126" fill="none" stroke="#0f766e" marker-end="url(#g)"/>
  <polyline points="1470,835 1492,835 1492,188 1507,188" fill="none" stroke="#0f766e" marker-end="url(#g)"/>
  <polyline points="1206,447 1466,447 1466,250 1507,250" fill="none" stroke="#0f766e" marker-end="url(#g)"/>
  <polyline points="1507,312 1300,312 1300,770" fill="none" stroke="#b45309" marker-end="url(#o)"/>
  <polyline points="1507,374 1240,374 1240,742 1020,742 1020,770" fill="none" stroke="#b45309" marker-end="url(#o)"/>
  <text x="1512" y="472" font-size="8.5" fill="#0f766e" text-anchor="end">user_id · path_id · 重发标记</text>
  <text x="1512" y="486" font-size="8.5" fill="#0f766e" text-anchor="end">stream_id · task_id · path_id</text>
  <text x="1512" y="500" font-size="8.5" fill="#0f766e" text-anchor="end">UserID（reduce 整包完成）</text>
  <text x="1512" y="514" font-size="8.5" fill="#b45309" text-anchor="end">UserID · StreamID · TaskID · PathID</text>
  <polygon points="60,1160 200,1160 191,1190 51,1190" fill="#f8fafc" stroke="#374151"/>
  <text x="126" y="1179" font-size="9" fill="#374151" text-anchor="middle">cfg（ctrl_noc）</text>
  <polyline points="200,1175 222,1175 222,1024 93,1024 93,1002" fill="none" stroke="#7c3aed" stroke-dasharray="2 3" marker-end="url(#p)"/>
  <text x="232" y="1178" font-size="8.5" fill="#7c3aed" text-anchor="start">cfg 写事务 → RouterTable / CSR、Credit Bypass Route、各 Station 的静态配置</text>
  <text x="20" y="1218" font-size="10.5" fill="#374151">三类 credit 互不复用：VC credit 管下游 VC Buffer 的空槽（RouterStation 维护）；Stream 资源管目标 core 的 Core Mem 空间（Router 是唯一有效状态，DTE 持 cache）；</text>
  <text x="20" y="1240" font-size="10.5" fill="#374151">Reduce credit 管下游 ReduceModule 的上下文（DTE 管本级，ReduceModule 管相邻下游，Router 不维护）。</text>
</svg>
```

***

## 2　功能清单

一功能一条，编号供“机制覆盖”一章引用。

### RouterStation（×3：left / right / mid）

| 编号 | 功能 |
| - | - |
| F1 | Header Parser 从包头取出 `path_id`、`user_id`、`size`、`operation`、`directionMask`、`vc_id`，按 `path_id` 查本地 RouterTable 副本，为这个包建立路由与资源上下文：目标方向、下一跳 VC、要不要 Stream 授权、是不是 Reduce、`stallWay` |
| F2 | Payload flit 按 Header 指定的 VC 号写入对应 VC Buffer；四个 VC 各自独立缓存、独立计 credit，一个 VC 堵住不影响其他 VC |
| F3 | VC 头部 flit 检查所有目标方向的资源：下游该 VC 的 credit、目标方向的 Stream 授权、要进 ReduceModule 时的 Reduce 准入 |
| F4 | 多播要所有目标方向的资源同时到手，任一方向不足则整体等待，不允许各方向独立前进 |
| F5 | 拿不到资源时按 `stallWay` 二选一：留在当前 VC 等，或把整包转进本地 Core Mem 由 DTE 重发 |
| F6 | VC credit 计数器：每个下游方向的每个 VC 一个，上电值等于下游 buffer 深度；flit 发出时扣该方向该 VC 一个 |
| F7 | flit 离开下游 VC Buffer 就归还 VC credit，走共享总线（`credit_return_vld` 加 `credit_return_vc_id`），每个 input port 一拍最多一个 VC 被读出，无冲突 |
| F8 | credit 不足的 VC 被跳过，同一个 input port 的其他 VC 不受影响 |
| F9 | Stream Resource Table 维护所有下游方向的 UserID 占用；只有所有需求方向都满足才允许发送 |
| F10 | 出口方向的 Output Buffer 与 Packet Shifter 按总线宽度移位拼接后从 `<方向>_data_out_ch` 发出 |
| F11 | 同 VC 保序：VC Buffer 是 FIFO，同 VC 内 flit 严格按到达顺序读出，资源检查与仲裁都不重排；跨 VC、跨 input port 之间不保证顺序 |
| F12 | Credit Release 静态旁路：Stream 与 Reduce 两类 release 不查 RouterTable、不做动态路径选择、不进 Xbar 仲裁，只按 CSR 配的静态方向 Mask 转发；Mask 含多个方向时同一笔 release 复制到所有指定方向，UserID 与 credit 类型保持不变 |
| F13 | 坏核上的 RouterStation 只走直通：数据走完整流水线但不投递本 core，不检查 credit、不支持阻塞重发；上游要检查的 credit 对应坏核之后那个好核，下游返还的 credit 也跨过坏核直接给上游好核 |
| F14 | 边沿 Router 通过配置禁用不存在的端口，统一规格实现 |

### Xbar

| 编号 | 功能 |
| - | - |
| F15 | 5 入 7 出：入是 `left` / `right` / `mid` / `local` / ReduceModule 回注；出是 `left` / `right` / `mid` / `core` / `reduce_0` / `reduce_1` / `reduce_2` |
| F16 | 五个输入的目标输出方向互不冲突时同周期并行传输，不做不必要的串行化；ReduceModule 一侧要能承接三路并发输入 |
| F17 | 每个 output port 各有一个独立的 RoundRobin 仲裁器，每拍独立仲裁，不跨拍锁定 |
| F18 | 仲裁不是纯 RoundRobin，是贪婪整包。优先级由高到低：接口传输 priority → 当前输入是否有整包 → 当前请求是否为上一包的 body → 正常 RoundRobin |
| F19 | 多播同一拍 1 到 N 复制，每个 output 的 Mux 独立控制，多个 output 可选同一个 input；任一目标没握手就不推进任何分支 |
| F20 | 握手成功后统一扣各目标的 credit、更新包上下文 |
| F21 | 交织粒度分两种：Router 到 Router 的通路允许在 flit 边界切换包，需保存 VC、输出方向、剩余长度和包边界上下文；包一旦开始进入 Core 或 ReduceModule 就锁定到尾 flit。两种粒度不能混用 |

### CoreStation

| 编号 | 功能 |
| - | - |
| F22 | 进 core 的三态准入：UserID 已分配则直接收；未分配但 Stream 资源表有空项则记录 UserID 占用后收；无空项时该 VC 不能向 Core 发数据，但 VC 有空项时仍可继续接收上游数据 |
| F23 | 已通过 Stream 检查，进 core 不再检查对 Core 方向的 VC credit，一定有 Core Mem 空间 |
| F24 | Header 写入 HeaderFIFO，Payload 写入 OutputBuffer（in_core_fifo），两者保持同一包顺序与边界 |
| F25 | Core 入口以整包为单位，不支持包间交织，必须发完一个整包再发下一个 |
| F26 | 收满一个包后按接收包头的顺序经 `router2ts_trigger_ch` 直接通知 TS，请求里带 `user_id`、`path_id` 与这一笔要不要重发的标记。这条通路硬件直连，不经 RV core，也不经任何软件环节 |
| F27 | 判定一个包收完的依据：比较已接收数据量与包头里的 payload 大小 |
| F28 | DTE core 经 AXI-Full 类接口读包头生成搬运任务，读完向指定地址写 1 把包头弹出，CoreStation 映射出下一个包头 |
| F29 | 被反压时保持 valid、当前 Header、Payload、首尾 flit 标志与有效字节信息不变，传输位置不得前移；反压解除后从同一 flit 继续握手，保证包不丢拍、不重拍、不跨包、不串包 |
| F30 | 出 core：DTE 按 PathID 查自己那份 RouterTable 得到 VC 与资源需求，先向 Router 申请到下游 Stream 或 Reduce 资源，再在目标 VC 有空时经 `out_core_data_ch` 发出整包；CoreStation 拆出 Header 与 Payload，按 Header 的 VC 号写入 Core 方向输入 VC，之后与其他方向一样查表、参与仲裁 |
| F31 | 进 core 与出 core 两条数据通路完全并行，互不共享数据通路仲裁状态 |

### ReduceModule

| 编号 | 功能 |
| - | - |
| F32 | 三路输入仲裁 SRAM、Bank 与计算资源；进入后锁定当前包直至尾 flit |
| F33 | 输入精度处理：BF16 扩展为 FP32，FP32 直接进入，数据面统一 FP32 |
| F34 | 同一 User 同一包的第一份输入分配上下文并写入 FP32 数据，后续方向的输入读出当前值、累加、写回，原位 Read-Modify-Write |
| F35 | 上下文保护：当前包的全部输入完成并输出前，同一 User 的下一个包不得覆盖该上下文 |
| F36 | 必须执行 Reduce：SRAM、Bank 或计算单元暂不可用时对输入反压，不允许绕过 Reduce 降级为直接存储或转发 |
| F37 | 精度：中间累加固定 FP32，输出按 RouterTable 的 `reducePrecision` 转成 FP32 或 BF16 |
| F38 | 全部方向输入完成后结果进输出队列，发送前查目标 VC credit 与该方向的下游 Reduce credit，作为 Xbar 的第五路输入重新参与仲裁 |
| F39 | 每发一个 flit 扣一个下游 Reduce credit；下游每发出一个 flit 产生携带 UserID 的 release，经 Router 的静态旁路返回上游 |
| F40 | Downstream Reduce Credit Map 按 UserID 加目标方向维护相邻下游的 Reduce credit，逐 flit 扣减、按 release 恢复 |
| F41 | 整包发出后经 `rmem2ts_done_ch` 向 core 返回 UserID。TS 只认这一路把 reduce task 置 FINISH |
| F42 | 输入侧只有 VC credit 准入，`credit > 0` 即收；Reduce credit 是另一张网，与数据面分离 |
| F43 | 表项的建与删：用户创建 Stream 资源时分配一个 entry 的 credit 数量；收到本级 core 该 UserID 的 Retire 且相邻下游各方向的 credit 全部恢复到分配数量后，才删掉这一项给其他用户用 |
| F44 | 链上没有同步点：上游分量到达时不必等本 core 算完，先存进上下文，本地出核的分量出来时再加 |

### RouterTable / CSR

| 编号 | 功能 |
| - | - |
| F45 | 64 条表项，按 `path_id` 索引。字段：`curVC`、`directionMask`、`nxtVC`、`streamNeedMask`、`operation`、`stallWay`、`reducePrecision` |
| F46 | 只描述静态路由与资源需求，不保存包的动态执行状态 |
| F47 | 内部多副本：所有需要并行查询的位置各持一份，由 Router 的配置入口统一接收写事务 |
| F48 | 更新状态机把同一笔写依次写入全部副本并记录完成状态；全部副本写完才向软件返回完成，禁止暴露部分新部分旧的状态 |
| F49 | 外部两份副本由 DTE 与 ReduceModule 各自维护，软件负责写入相同配置并保证三方一致，Router 硬件不同步外部副本；软件只能在 Router 提交完成后再写它们 |
| F50 | Credit Bypass Route：软件经 CSR 为每个业务 credit 输入端口配置静态输出方向 Mask，可指定一个或多个 R2R 方向与 Core 方向；坏核场景切换 credit 路径靠的就是改这组配置 |
| F51 | 中间节点可以按表改写 VC |

### CoreMem 重发

| 编号 | 功能 |
| - | - |
| F52 | `stallWay` 选转存时把整包重定向到本地 Core Mem 缓存，Router 上的 Bypass 操作被映射成“进 core 加出 core”两段 |
| F53 | Core Mem 中只保存包，包头含 UserID、PathID、size；重发时用 PathID 重新查 RouterTable，不重复保存 VC 和路由信息 |
| F54 | 同 VC 保序：同一 VC 存在未完成的 CoreMem 重发包时，后续包不得越过；按 VC 粒度维护 `pending_reinject` 计数器防超车 |
| F55 | 包进 core 暂存时改写 `overflow_reinject = 1`，出 core 重发时 Router 改回 0；Output Port 识别到重注入标记才扣减 credit |
| F56 | 坏核不接收溢流：Router 对坏核不发起进 core 缓存处理，此时 coremem credit 直接 bypass |
| F57 | 无论直接发送还是经 CoreMem 重发，完成后都向 core 内 TS 返回至少含 UserID 加 PathID 的完成信息 |

### Retire

| 编号 | 功能 |
| - | - |
| F58 | ReduceModule 完成计算并发出全部包后向 Core 返回 UserID；Core 判定任务链结束后向 Router 和 ReduceModule 广播 User Retire |
| F59 | Core 的保证：仅可在该 UserID 的全部进 core、出 core 数据搬运完成，且不会再发起新搬运之后发 Retire。Retire 发出后，Router 上不得再出现以该 Core 为源或目标的该用户包 |
| F60 | Router 的动作：停止该 UserID 的新发送，删除其全部 Stream 资源授权表项 |
| F61 | ReduceModule 的动作是延迟回收，先记录 Retire，待相邻下游各方向的 Reduce credit 全部恢复到初始值后才删除对应用户映射 |
| F62 | Stream credit 的回程：每个 Router 用一个组合逻辑的 core credit crossbar 汇总本级 core 与所有下级出口的 pulse 加 user，发往除来向外的另两个 R2R port，逐跳传到上游；跨 chip 经 C2C Bridge 透传 |

### CoreMemCreditMonitor

| 编号 | 功能 |
| - | - |
| F63 | 监听事件队列 16 项全相连，可同时监听多笔多方向的资源申请 |
| F64 | TS 注册时带 UserID、StreamID、TaskID、PathID；Router 按 PathID 查到需要发送的下游方向、VC 需求与 stream 需求 |
| F65 | 申请到就经反向控制通路通知 TS，回 StreamID、TaskID、PathID；申请不到就把需求记进队列监听，资源满足再通知 |
| F66 | 多个事件同时满足时按 StreamID 仲裁，选最老的任务通知 TS |
| F67 | 进 core 重发的任务也注册到该队列，数据进 Core、资源就绪后通知 TS 重发 |
| F68 | 同一 VC 的数据包要保序，当前 VC 有未重发完的数据时后续包不能提前发送 |
| F69 | 只有 Router 负责真正申请 Stream 表项。DTE 要发数据必须先从 Router 拿到指定 user 的授权，禁止超额分配或重复授权 |
| F70 | Router 的进 core 表和 TS 内部的 Stream 资源表按完全一致的逻辑申请空项，分配因此不会多于实际资源数，这保证了“Router 通知 TS 的包一定能被 TS 接收” |
| F71 | 另输出 per-port 的 `coremem_credit` 同步信息给 core 与 DTE，用于判断重注入 |

***

## 3　接口

```
port link[d] (双向, credit/release, clk)          // d ∈ {left, right, mid}：256 B/T，接相邻 core 的 Router 或 C2C Bridge
  in  flit_valid · flit_vc[1:0] · flit_head · flit_tail · flit_bytes[8:0] · flit_msg
  in  credit_return_vld · credit_return_vc_id[1:0]        // VC credit 归还，共享总线，一拍最多一个 VC
  in  stream_release_vld · stream_release_user[15:0]      // Stream 资源 release
  in  reduce_release_vld · reduce_release_user[15:0]      // Reduce credit release
  out 同字段
port in_core_data_ch (master, AXI-Stream-Like, clk)   // CoreStation → DTE：进 core 的整包
  out tvalid · tdata[2047:0] · tkeep[255:0] · tlast · thdr
  in  tready                                              // = DTE 的 inbound buffer 与 Commit 资源都够
port out_core_data_ch (slave, AXI-Stream-Like, clk)   // DTE → CoreStation：出 core 的整包
  in  tvalid · tdata[2047:0] · tkeep[255:0] · tlast · thdr · vc_id[1:0]
  out tready                                              // = 该 VC 的 Core 方向输入 VC 有空
port hdr_rd (slave, AXI-Full 类, clk)                 // DTE core 读 HeaderFIFO 的包头，读完写 1 弹出
  in  araddr[31:0] · arvalid · awaddr[31:0] · wvalid · wdata[31:0]
  out arready · rvalid · rdata[255:0] · awready · wready
port router2ts_trigger_ch (master, 脉冲, clk)         // CoreStation → TS：收满一个包
  out valid · user_id[15:0] · path_id[7:0] · reissue
port router2ts_credit_ch (master, 脉冲, clk)          // CoreMemCreditMonitor → TS：资源到手
  out valid · stream_id[3:0] · task_id[5:0] · path_id[7:0]
port rmem2ts_done_ch (master, 脉冲, clk)              // ReduceModule → TS：一个整包 reduce 完成
  out valid · user_id[15:0]
port ts2router_req (slave, valid/ready, clk)          // TS → CoreMemCreditMonitor：注册资源申请
  in  req_valid · user_id[15:0] · stream_id[3:0] · task_id[5:0] · path_id[7:0]
  out req_ready                                           // = 监听事件队列有空项
port ts2router_retire (slave, 脉冲, clk)              // TS → Retire：用户退休与 credit 返还
  in  valid · user_id[15:0]
  out accepted                                            // Router 接收后 TS 才清 valid 并推进 head_ptr
port coremem_credit (master, 电平, clk)               // per-port 的 Stream 资源同步信息，给 core 与 DTE
  out credit_vld[2:0] · credit_user[2:0][15:0]
port cmem_reissue (master, valid/ready, clk)          // CoreMem 重发与 Core Mem 之间的暂存读写，256 B
  out req_valid · req_we · req_addr[17:0] · req_wdata[2047:0]
  in  req_ready · rsp_valid · rsp_rdata[2047:0]
port cfg (slave, ctrl_noc 写事务, clk)                // RouterTable、CSR、Credit Bypass Route 的配置口
  in  cfg_valid · cfg_addr[23:0] · cfg_we · cfg_wdata[31:0]
  out cfg_rdata[31:0] · commit_done                       // 全部副本写完才拉 commit_done
```

***

## 4　存储器

```
mem rtab[k]           FF 阵列   64 × {curVC[1:0], directionMask[6:0], nxtVC[6:0][1:0], streamNeedMask[6:0], operation[2:0], stallWay, reducePrecision[1:0]}  1R1W  多副本，逐份写  复位 0   // k 份内部副本
mem rtab_commit       FF        {副本写入游标, 完成状态}                                    1RW   更新状态机                复位 空闲
mem credit_bypass     FF 阵列   每个业务 credit 输入端口一个 {out_mask[6:0]}                1R1W  CSR 配置                  复位 0
mem vc_buf[d][v]      FIFO      private 每 VC 2 flit + shared pool 约 20 flit               1W1R  满 → 不再收上游           复位空    // d ∈ {left,right,mid,core}，v ∈ 0..3
mem pkt_ctx[d][v]     FF 阵列   {vc[1:0], out_mask[6:0], remain_len[15:0], head, tail, locked}  1RW  队首上下文             复位空
mem vc_credit[d][v]   FF        计数器                                                      1RW   发出扣 1，release 加 1     复位 下游 buffer 深度
mem stream_tab[d]     FF 阵列   每方向 16 项 × {valid, user_id[15:0]}                        1RW   建：新 UserID 首次到达；删：release 或 Retire  复位空
mem hdr_fifo          FIFO      16 × 256 B 包头                                             1W1R  DTE 读完写 1 弹出          复位空
mem out_buf           FIFO      32 flit（in_core_fifo）                                     1W1R  整包写入，不交织          复位空
mem rdc_ctx           SRAM      16 用户 × 16 KiB，FP32 中间累加结果                          1R1W  RMW 原位累加              复位未定义
mem rdc_user_tab      FF 阵列   16 × {user_id[15:0], pkt_state[2:0], in_done_mask[2:0], out_state[1:0], retired}  1RW  —   复位空
mem rdc_down_credit   FF 阵列   16 用户 × 3 方向 × 计数器                                    1RW   发 flit 扣 1，release 加 1  复位 分配值
mem rdc_out_q         FIFO      8 flit                                                      1W1R  满 → 停止 RMW 输出         复位空
mem rdc_rtab          FF 阵列   64 项，RouterTable 的外部副本                                1R1W  软件写                    复位 0
mem reissue_tab       FF 阵列   每 VC 一项 {pending_reinject[7:0]}                           1RW   进 core 暂存加 1，重发减 1  复位 0
mem monitor_q         FF 阵列   16 项全相连 × {user_id[15:0], stream_id[3:0], task_id[5:0], path_id[7:0], need_mask[6:0]}  1RW  —  复位空
mem retire_pend       FF 阵列   16 × {user_id[15:0], from_core, to_rdc}                      1RW   广播后逐项回收            复位空
mem 级间 latch         级间 latch 各级之间的包上下文与 flit                                   —     每拍覆写                  —
```

***

## 5　流水线总览

数据面是六级流水线，每级 1 cycle，单跳延迟 ≤ 6 cycles：

```
Input VC Buffer ──→ RC ──→ VA ──→ SA ──→ ST ──→ Output Pipe
   (head flit)      查表    拿资源   抢通路   交换
```

第 1 层图待逐级拍数定下来后补，届时把上面这五级与 ReduceModule 的 RMW 管线按拍对齐画在一张图上。

***

## 6　逐级行为

第 2 层图与每级的四要素待第 1 层图完成后补，级编号回标到第 1 层图。

***

## 7　参数汇总

```
每方向数据宽度      256 B；相邻 Router 双向各 256 GB/s @1GHz（接口理论值），HAS 记 R2R 有效带宽 210 GB/s、C2C 90 GB/s
进 core 与出 core   各 256 GB/s @1GHz，完全并行
单跳延迟            ≤ 6 cycles（六级流水线），优化后 4～5；坏核走 Skip 直通，延迟与正常跳一致
RouterTable         64 条表项；内部副本数与每副本写入拍数 5、1（待定）
VC                  每输入方向 4 类（VC0～3），输出方向不设 VC Buffer
VC Buffer           private 每 VC 深度 2（防死锁），shared pool 约 20 flit（覆盖 credit 往返）；建模默认 32 flit / VC（待定）
Stream Resource Table  每方向 16 项（待定）
VC credit 初值      下游 VC Buffer 深度
Reduce 输入 / 输出   三路各 160 GB/s / 160 GB/s；算力 80 GFLOPS（FP32 / BF16）
ReduceModule 上下文  16 用户 × 16 KiB
ReduceModule Entry credit、bank 数、RMW 拍数、输出队列深度   64 flit、4、2、8（待定）
CoreStation HeaderFIFO / OutputBuffer 深度   16 / 32 flit（待定）
监听事件队列        16 项全相连
Xbar 与 ReduceModule 三路输入的仲裁算法      轮询（待定）
operation 的 Reduce0 / Reduce1 / Reduce2     源分量 / 中继累加 / 最终汇聚（待定，原文未定义）
包结构              path_id 8 bit（有效 6 bit）· path_core_mask 16 bit · user_id 加 task_id 10 bit · vc_id · overflow_reinject 1 bit · 包长 16 bit · 软件 payload 0～16 B · 业务数据 0 B～64 KB
总线                Header 与 Payload 走两根独立并行总线，hflit 256 bit 与 pflit 2048 bit，按同一包边界保持对应；进 core 拼接成完整包，出 core 自动拆分
```

***

## 8　机制覆盖

| 机制 | 功能 | 用例 |
| - | - | - |
| 按 path_id 查表得到全部去向与资源需求 | F1、F45、F46 | `router_lookup` |
| 四个 VC 独立缓存独立计 credit，一个堵住不影响其他 | F2、F8 | `vc_isolation` |
| 单播准入 `credit_cnt[out][vc] > 0` | F3、F6 | `vc_credit_single` |
| 多播全有或全无，任一方向不足则所有分支一起等 | F4、F19 | `multicast_atomic` |
| stallWay 二选一：留在 VC 等 / 转 Core Mem 重发 | F5、F52 | `stall_way` |
| VC credit 走共享总线归还，一拍最多一个 VC | F7 | `vc_credit_return` |
| 只有所有需求方向都满足才允许发送 | F9 | `stream_all_or_none` |
| 同 VC 保序，跨 VC 与跨 port 不保证 | F11 | `same_vc_order` |
| 业务 credit 旁路：不查表、不进仲裁、按静态 Mask 复制 | F12、F50 | `credit_bypass_route` |
| 坏核只透传，credit 跨过它 | F13、F56 | `harvest_skip` |
| Xbar 5 入 7 出，无冲突时五路并行 | F15、F16 | `xbar_parallel` |
| 每 output 独立 RoundRobin，不跨拍锁定 | F17 | `xbar_rr` |
| 贪婪整包的四级优先级 | F18 | `xbar_greedy_packet` |
| 进 core 后锁定到尾 flit，R2R 可在 flit 边界切换包 | F21 | `interleave_grain` |
| 进 core 三态准入 | F22 | `corestation_admit` |
| 进 core 不再查 VC credit | F23 | `no_vc_check_into_core` |
| Core 入口整包，不支持包间交织 | F25 | `no_packet_interleave` |
| CoreStation 按包头顺序直接通知 TS，不经软件 | F26 | `trigger_to_ts` |
| 按已接收字节数与包头 payload 大小判 token 收完 | F27 | `token_complete` |
| DTE 读包头后写 1 弹出，映射出下一个包头 | F28 | `header_pop` |
| 反压时数据保持，解除后从同一 flit 继续 | F29 | `backpressure_hold` |
| 出 core 前置申请：先拿到下游 Stream 或 Reduce 资源才发 | F30、F69 | `out_core_pre_grant` |
| 进 core 与出 core 完全并行 | F31 | `in_out_parallel` |
| ReduceModule 三路输入仲裁，进入后锁定到尾 flit | F32 | `reduce_input_arb` |
| BF16 扩 FP32，中间累加固定 FP32，输出可配 | F33、F37 | `reduce_precision` |
| 首份输入建上下文，后续原位 RMW 累加 | F34 | `reduce_rmw` |
| 上下文保护：同一 User 的下一个包不得覆盖 | F35 | `reduce_ctx_protect` |
| 必须执行 Reduce，不允许降级 | F36 | `reduce_no_bypass` |
| 输出前查目标 VC credit 与下游 Reduce credit | F38、F40 | `reduce_out_credit` |
| 每发一 flit 扣一个，按 UserID release 恢复 | F39 | `reduce_credit_flit` |
| 只有 Router Reduce Done 才能把 reduce task 置 FINISH | F41 | `reduce_done_owner` |
| 链上没有同步点，流着加 | F44 | `reduce_streaming` |
| RouterTable 内部多副本，全部写完才返回完成 | F47、F48 | `router_table_commit` |
| 三份副本由软件保证一致，硬件不同步外部两份 | F49 | `router_table_three_copies` |
| CoreMem 只存包，重发时按 PathID 重查表 | F53 | `reissue_repath` |
| 同 VC 有未重发完的包时后续包不得越过 | F54、F68 | `reissue_order` |
| overflow_reinject 标记，Output Port 识别到才扣 credit | F55 | `reinject_flag` |
| 完成后向 TS 回 UserID + PathID | F57 | `reissue_done` |
| Retire 的三方时序：Core 保证 → Router 立即删表项 → ReduceModule 延迟回收 | F58～F61 | `retire_three_party` |
| Stream credit 经 core credit crossbar 逐跳传到上游 | F62 | `stream_credit_hop` |
| 监听事件队列 16 项全相连，按 StreamID 选最老通知 TS | F63～F66 | `credit_monitor_q` |
| 进 core 重发的任务也注册到监听队列 | F67 | `reissue_register` |
| Router 与 TS 的 Stream 表按一致逻辑分配，通知的包一定能被接收 | F70 | `stream_tab_consistent` |
| per-port coremem_credit 同步给 core 与 DTE | F71 | `coremem_credit_sync` |

***

## 9　取舍

* **为什么把 VC credit 和 Stream 资源分成两层**
  * 两者管的东西时间尺度差着数量级：VC credit 管下游 Router 的 buffer 槽位，flit 一进一出就归还；Stream 资源要等那个用户在下游 core 上跑完整条任务链才释放
  * 合成一层，快的那层会被慢的拖成一样慢
* **为什么进 Core 之后不再查 VC credit**
  * Stream 检查已经保证目标 core 有 Core Mem 空间，再查一次是重复的资源判定
  * 这一路也没有下游 Router 的 buffer 需要保护
* **为什么组播要全有或全无**
  * 若允许各方向独立前进，一个包的不同分支会走到不同的进度，Router 要为每个分支单独维护剩余长度和包边界上下文，状态量按方向数翻倍
  * 一起等的代价是性能，各自走的代价是状态爆炸
* **为什么 Reduce 不允许降级**
  * 若允许绕过 Reduce 直接存储或转发，下游收到的是未归约的原始数据，且并不知道这件事，也没有补做归约的机会。宁可反压
* **为什么退休时 ReduceModule 要延迟回收**
  * Retire 只说明 core 侧的搬运结束了，而 ReduceModule 发往相邻下游的 flit 可能还在路上
  * 等下游各方向的 Reduce credit 全部恢复到初始值，才能确认这些 flit 都已被接收
* **为什么出核前的资源监听放在 Router 而不是 TS**
  * 若改由 TS 监听，大量通信信息要塞进 TS 任务链，计算与通信不再解耦
* **为什么不用标准 AXI4 而自研 flit 级协议**
  * AXI4 的 header 开销在纯数据通路场景里是冗余的，Router 不需要地址路由
  * 定制协议把路由信息压缩到最小，payload 带宽利用率最高，代价是要自研协议栈和配套验证环境
