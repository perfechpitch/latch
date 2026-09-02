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

| | VC credit | stream credit（CoreMem credit） | Reduce credit |
| - | - | - | - |
| 管什么 | 下游 VC Buffer 有没有空槽 | 目标 core 的 Core Mem 有没有空间容纳这个用户的数据 | 下游 ReduceModule 的上下文有没有空间 |
| 粒度 | 按下游方向加 VC，flit | 按 UserID 加目标方向，一个表项 | 按 UserID，flit |
| 谁维护 | RouterStation 的独立计数器，硬件自动维护 | Router 是唯一有效状态；DTE 持一份 cache；TS 内另有一份本级表，按与 Router 完全一致的逻辑分配空项 | DTE 维护本级的，ReduceModule 维护相邻下游各方向的，Router 不维护 |
| 何时扣 | flit 发出时扣该方向该 VC 一个；经 CoreMem 重注入的包，Output Port 识别到重注入标记才扣 | 新 UserID 的包在本级占一个表项；出核的包由 DTE 先向 Router 申请到授权 | 每发一个 flit 扣一个；DTE 发 Reduce 包前要求本级 credit 够整包 |
| 何时还 | flit 离开下游 VC Buffer 就归还 | 用户在下游 core 跑完任务链、用完 Core Mem 后发携带 UserID 的 release，逐跳传到上游 | 输出 flit 被下游接受后产生携带 UserID 的 release，经静态旁路返回上游 |
| 快慢 | 快，flit 一进一出就还 | 慢，要等那个用户在下游 core 上跑完整条任务链 | 介于两者之间，按 flit 还但要等下游 Reduce 完成 |

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 2240 1150" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" role="img" aria-label="Router 第 0 层">
<title>Router 第 0 层</title>
<defs><marker id="a" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#475569"/></marker><marker id="as" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#475569"/></marker><marker id="g" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0f766e"/></marker><marker id="gs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0f766e"/></marker><marker id="o" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#b45309"/></marker><marker id="os" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#b45309"/></marker><marker id="p" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#7c3aed"/></marker><marker id="ps" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#7c3aed"/></marker><marker id="i" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#4338ca"/></marker><marker id="is" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#4338ca"/></marker><marker id="t" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0d9488"/></marker><marker id="ts" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0d9488"/></marker><marker id="r" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#be123c"/></marker><marker id="rs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#be123c"/></marker><marker id="b" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#2563eb"/></marker><marker id="bs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#2563eb"/></marker><marker id="m" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#d97706"/></marker><marker id="ms" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#d97706"/></marker><marker id="l" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#9aa1ad"/></marker><marker id="ls" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#9aa1ad"/></marker></defs>
<rect x="0" y="0" width="2240" height="1150" fill="#ffffff"/>
<text x="20" y="26" font-size="12" fill="#111827">Router · 第 0 层（八个独立打拍的模块；每 Core 一份，坏核也有。方位照 MAS 框图：core 侧模块在上，left / right 在两侧，mid 朝另一排在下）</text>
<text x="884" y="26" font-size="9.5" fill="#6b7280">灰线 = flit 数据面　橙线 = 三类 credit 与 release　绿线 = 与 TS 的控制通路　紫虚线 = ctrl_noc 配置</text>
<rect x="40" y="120" width="330" height="195.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="52" y="141" font-size="11" fill="#111827" font-weight="600">RouterTable / CSR</text>
<text x="52.0" y="158.0" font-size="8.5" fill="#475569">64 条表项，索引 path_id</text>
<text x="52.0" y="171.5" font-size="8.5" fill="#475569">字段：cur_vc · flow_dir · nxt_vc · stream_table_enable</text>
<text x="52.0" y="185.0" font-size="8.5" fill="#475569">　　　operation · stall_way · reduce_outdata_type</text>
<text x="52.0" y="198.5" font-size="8.5" fill="#475569">只描述静态路由与资源需求，不保存包的动态状态</text>
<text x="52.0" y="212.0" font-size="8.5" fill="#475569">内部多副本：所有需并行查询的位置各一份</text>
<text x="52.0" y="225.5" font-size="8.5" fill="#475569">更新状态机把同一笔写依次写入全部副本并记完成</text>
<text x="52.0" y="239.0" font-size="8.5" fill="#475569">全部副本写完才向软件返回完成，禁止部分新部分旧</text>
<text x="52.0" y="252.5" font-size="8.5" fill="#475569">外部两份（DTE、ReduceModule）由软件写，硬件不同步</text>
<text x="52.0" y="266.0" font-size="8.5" fill="#475569">Credit Bypass Route：每个业务 credit 输入端口一张</text>
<text x="52.0" y="279.5" font-size="8.5" fill="#475569">　静态输出方向 Mask，坏核场景靠改它切换 credit 路径</text>
<text x="358" y="306.0" font-size="8.5" fill="#9ca3af" text-anchor="end">软件经 R2CU 接口配置</text>
<rect x="400" y="120" width="330" height="181.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="412" y="141" font-size="11" fill="#111827" font-weight="600">CoreMem 重发</text>
<text x="412.0" y="158.0" font-size="8.5" fill="#475569">stall_way = 转存时：整包重定向到本地 Core Mem</text>
<text x="412.0" y="171.5" font-size="8.5" fill="#475569">Bypass 被映射成“进 core + 出 core”两段</text>
<text x="412.0" y="185.0" font-size="8.5" fill="#475569">CoreMem 中只保存包（含 UserID · PathID · size）</text>
<text x="412.0" y="198.5" font-size="8.5" fill="#475569">重发时用 PathID 重查 RouterTable，不重复保存 VC 与路由</text>
<text x="412.0" y="212.0" font-size="8.5" fill="#475569">同 VC 保序：该 VC 有未完成的重发包时后续包不得越过</text>
<text x="412.0" y="225.5" font-size="8.5" fill="#475569">按 VC 粒度维护 pending_reinject 计数器防超车</text>
<text x="412.0" y="239.0" font-size="8.5" fill="#475569">进 core 暂存时改写 overflow_reinject = 1</text>
<text x="412.0" y="252.5" font-size="8.5" fill="#475569">出 core 重发时改回 0；Output Port 识别到该标记才扣 credit</text>
<text x="412.0" y="266.0" font-size="8.5" fill="#475569">坏核不接收溢流，coremem credit 直接 bypass</text>
<text x="412.0" y="279.5" font-size="8.5" fill="#475569">无论直接发还是重发，完成后都向 TS 回 UserID + PathID</text>
<rect x="760" y="120" width="400" height="181.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="772" y="141" font-size="11" fill="#111827" font-weight="600">CoreStation</text>
<text x="772.0" y="158.0" font-size="8.5" fill="#475569">HeaderFIFO：按接收顺序存包头，DTE 读完写 1 弹出</text>
<text x="772.0" y="171.5" font-size="8.5" fill="#475569">OutputBuffer（in_core_fifo）：整包写入，不支持包间交织</text>
<text x="772.0" y="185.0" font-size="8.5" fill="#475569">三态准入：UserID 已分配 → 直接收；未分配但有空项 → 记录占用；</text>
<text x="772.0" y="198.5" font-size="8.5" fill="#475569">　无空项 → 该 VC 不能向 Core 发，但 VC 有空仍可继续收上游</text>
<text x="772.0" y="212.0" font-size="8.5" fill="#475569">已过 Stream 检查，进 core 不再查 VC credit</text>
<text x="772.0" y="225.5" font-size="8.5" fill="#475569">出 core：Core 方向输入 VC，与 DTE 之间按 VC credit 协议</text>
<text x="772.0" y="239.0" font-size="8.5" fill="#475569">反压时 valid / Header / Payload / 首尾标志 / 有效字节保持不变</text>
<text x="772.0" y="252.5" font-size="8.5" fill="#475569">收满一个包按顺序经 router2ts_trigger_ch 直接通知 TS</text>
<text x="772.0" y="266.0" font-size="8.5" fill="#475569">判定收完：比较已接收数据量与包头里的 payload 大小</text>
<text x="772.0" y="279.5" font-size="8.5" fill="#475569">进 core 与出 core 两条路完全并行，不共享仲裁状态</text>
<rect x="1200" y="120" width="300" height="194.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1212" y="141" font-size="11" fill="#111827" font-weight="600">Retire</text>
<text x="1212.0" y="158.0" font-size="8.5" fill="#475569">Core 判定任务链结束后向 Router 与 ReduceModule 广播</text>
<text x="1212.0" y="171.5" font-size="8.5" fill="#475569">Core 的保证：该 UserID 全部进 core、出 core 搬运完成</text>
<text x="1212.0" y="185.0" font-size="8.5" fill="#475569">　且不会再发起新搬运之后才发 Retire</text>
<text x="1212.0" y="198.5" font-size="8.5" fill="#475569">Retire 发出后 Router 上不得再出现以该 Core 为源或</text>
<text x="1212.0" y="212.0" font-size="8.5" fill="#475569">　目标的该用户包</text>
<text x="1212.0" y="225.5" font-size="8.5" fill="#475569">Router 的动作：停止该 UserID 的新发送，删除其全部</text>
<text x="1212.0" y="239.0" font-size="8.5" fill="#475569">　stream credit 授权表项</text>
<text x="1212.0" y="252.5" font-size="8.5" fill="#475569">ReduceModule 的动作：延迟回收。先记录 Retire，待相邻</text>
<text x="1212.0" y="266.0" font-size="8.5" fill="#475569">　下游各方向的 Reduce credit 全恢复到初值才删映射</text>
<text x="1212.0" y="279.5" font-size="8.5" fill="#475569">本级 core 与所有下级出口的 release 经 core credit crossbar</text>
<text x="1212.0" y="293.0" font-size="8.5" fill="#475569">　汇总，发往除来向外的另两个 R2R port，逐跳传到上游</text>
<rect x="1560" y="120" width="400" height="195.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1572" y="141" font-size="11" fill="#111827" font-weight="600">CoreMemCreditMonitor</text>
<text x="1572.0" y="158.0" font-size="8.5" fill="#475569">监听事件队列：16 项全相连</text>
<text x="1572.0" y="171.5" font-size="8.5" fill="#475569">TS 注册时带 UserID · StreamID · TaskID · PathID</text>
<text x="1572.0" y="185.0" font-size="8.5" fill="#475569">Router 按 PathID 查出要发的下游方向、VC 与 stream 需求</text>
<text x="1572.0" y="198.5" font-size="8.5" fill="#475569">申请到 → 经反向控制通路通知 TS</text>
<text x="1572.0" y="212.0" font-size="8.5" fill="#475569">　（回 StreamID · TaskID · PathID）</text>
<text x="1572.0" y="225.5" font-size="8.5" fill="#475569">申请不到 → 需求记进队列监听，资源满足再通知</text>
<text x="1572.0" y="239.0" font-size="8.5" fill="#475569">多个事件同时满足时按 StreamID 仲裁，选最老的通知 TS</text>
<text x="1572.0" y="252.5" font-size="8.5" fill="#475569">进 core 重发的任务也注册到该队列</text>
<text x="1572.0" y="266.0" font-size="8.5" fill="#475569">同一 VC 有未重发完的包时后续包不能提前发</text>
<text x="1572.0" y="279.5" font-size="8.5" fill="#475569">另输出 per-port 的 stream_credit 给 core 与 DTE</text>
<text x="1948" y="306.0" font-size="8.5" fill="#9ca3af" text-anchor="end">出核前的资源监听在 Router，不在 TS</text>
<rect x="170" y="395.0" width="340" height="167.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="182" y="416.0" font-size="11" fill="#111827" font-weight="600">RouterStation[left]</text>
<text x="182.0" y="433.0" font-size="8.5" fill="#475569">Header Parser：取 path_id · path_core_mask · user_id</text>
<text x="182.0" y="446.5" font-size="8.5" fill="#475569">　size · vc_id，查 RouterTable 得出方向与资源</text>
<text x="182.0" y="460.0" font-size="8.5" fill="#475569">VC Buffer ×4（private 20 + shared pool 约 20 flit）</text>
<text x="182.0" y="473.5" font-size="8.5" fill="#475569">Packet Context：VC · 输出方向 · 剩余长度 · 包边界</text>
<text x="182.0" y="487.0" font-size="8.5" fill="#475569">Stream Resource Table：下游各方向的 UserID 占用</text>
<text x="182.0" y="500.5" font-size="8.5" fill="#475569">VC Credit 计数器：每下游方向每 VC 一个</text>
<text x="182.0" y="514.0" font-size="8.5" fill="#475569">Output Buffer + Packet Shifter（按总线宽度拼接）</text>
<text x="182.0" y="527.5" font-size="8.5" fill="#475569">Credit Release 静态旁路：按 CSR 的方向 Mask 转发</text>
<text x="182.0" y="541.0" font-size="8.5" fill="#475569">坏核：只透传，不查 credit、不支持阻塞重发</text>
<rect x="760" y="395.0" width="400" height="221.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="772" y="416.0" font-size="11" fill="#111827" font-weight="600">Xbar</text>
<text x="772.0" y="433.0" font-size="8.5" fill="#475569">5 入 7 出</text>
<text x="772.0" y="446.5" font-size="8.5" fill="#475569"></text>
<text x="772.0" y="460.0" font-size="8.5" fill="#475569">入：left · right</text>
<text x="772.0" y="473.5" font-size="8.5" fill="#475569">　　mid · local</text>
<text x="772.0" y="487.0" font-size="8.5" fill="#475569">　　reduce 回注</text>
<text x="772.0" y="500.5" font-size="8.5" fill="#475569"></text>
<text x="772.0" y="514.0" font-size="8.5" fill="#475569">出：left · right</text>
<text x="772.0" y="527.5" font-size="8.5" fill="#475569">　　mid · core</text>
<text x="772.0" y="541.0" font-size="8.5" fill="#475569">　　reduce_0/1/2</text>
<text x="772.0" y="554.5" font-size="8.5" fill="#475569"></text>
<text x="772.0" y="568.0" font-size="8.5" fill="#475569">按输出独立</text>
<text x="772.0" y="581.5" font-size="8.5" fill="#475569">RoundRobin</text>
<text x="772.0" y="595.0" font-size="8.5" fill="#475569"></text>
<text x="960.0" y="433.0" font-size="8.5" fill="#475569">贪婪整包：</text>
<text x="960.0" y="446.5" font-size="8.5" fill="#475569">整包 &gt; 上包 body</text>
<text x="960.0" y="460.0" font-size="8.5" fill="#475569">&gt; 轮询</text>
<text x="960.0" y="473.5" font-size="8.5" fill="#475569"></text>
<text x="960.0" y="487.0" font-size="8.5" fill="#475569">多播同拍复制</text>
<text x="960.0" y="500.5" font-size="8.5" fill="#475569">全有或全无</text>
<text x="960.0" y="514.0" font-size="8.5" fill="#475569"></text>
<text x="960.0" y="527.5" font-size="8.5" fill="#475569">入口锁定到尾 flit</text>
<text x="960.0" y="541.0" font-size="8.5" fill="#475569">（进 core 或</text>
<text x="960.0" y="554.5" font-size="8.5" fill="#475569">ReduceModule）</text>
<text x="960.0" y="568.0" font-size="8.5" fill="#475569"></text>
<text x="960.0" y="581.5" font-size="8.5" fill="#475569">R2R 可在 flit</text>
<text x="960.0" y="595.0" font-size="8.5" fill="#475569">边界切换包</text>
<rect x="1300" y="395.0" width="400" height="249.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1312" y="416.0" font-size="11" fill="#111827" font-weight="600">ReduceModule</text>
<text x="1312.0" y="433.0" font-size="8.5" fill="#475569">三路输入仲裁（Data ×3）：仲裁 SRAM / Bank / 计算资源</text>
<text x="1312.0" y="446.5" font-size="8.5" fill="#475569">　进入后锁定当前包直至尾 flit；资源不足对输入反压</text>
<text x="1312.0" y="460.0" font-size="8.5" fill="#475569">输入精度处理：BF16 扩展为 FP32，数据面统一 FP32</text>
<text x="1312.0" y="473.5" font-size="8.5" fill="#475569">Reduce Context SRAM：16 用户 × 16 KiB，FP32 中间结果</text>
<text x="1312.0" y="487.0" font-size="8.5" fill="#475569">RMW 管线：首份输入建上下文，后续输入原位累加，80 GFLOPS</text>
<text x="1312.0" y="500.5" font-size="8.5" fill="#475569">User Context Table：UserID · 包状态 · 输入完成 · 输出状态 · Retire</text>
<text x="1312.0" y="514.0" font-size="8.5" fill="#475569">RouterTable Copy：输出方向 · 下一跳 VC · operation · 输出精度</text>
<text x="1312.0" y="527.5" font-size="8.5" fill="#475569">结果生成与发送：全部输入完成 → 输出队列 → 转 FP32 / BF16</text>
<text x="1312.0" y="541.0" font-size="8.5" fill="#475569">　发送前查目标 VC credit 与该方向的下游 Reduce credit</text>
<text x="1312.0" y="554.5" font-size="8.5" fill="#475569">Downstream Reduce Credit Map：按 UserID 加方向，逐 flit 扣、</text>
<text x="1312.0" y="568.0" font-size="8.5" fill="#475569">　按 release 恢复</text>
<text x="1312.0" y="581.5" font-size="8.5" fill="#475569">三条硬约束：必须执行 Reduce 不许降级 · 上下文保护 ·</text>
<text x="1312.0" y="595.0" font-size="8.5" fill="#475569">　中间累加固定 FP32</text>
<text x="1312.0" y="608.5" font-size="8.5" fill="#475569">整包发出后向 core 返回 UserID</text>
<text x="1688" y="635.0" font-size="8.5" fill="#9ca3af" text-anchor="end">输入三路各 160 GB/s，输出 160 GB/s</text>
<rect x="1760" y="395.0" width="340" height="167.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1772" y="416.0" font-size="11" fill="#111827" font-weight="600">RouterStation[right]</text>
<text x="1772.0" y="433.0" font-size="8.5" fill="#475569">Header Parser：取 path_id · path_core_mask · user_id</text>
<text x="1772.0" y="446.5" font-size="8.5" fill="#475569">　size · vc_id，查 RouterTable 得出方向与资源</text>
<text x="1772.0" y="460.0" font-size="8.5" fill="#475569">VC Buffer ×4（private 20 + shared pool 约 20 flit）</text>
<text x="1772.0" y="473.5" font-size="8.5" fill="#475569">Packet Context：VC · 输出方向 · 剩余长度 · 包边界</text>
<text x="1772.0" y="487.0" font-size="8.5" fill="#475569">Stream Resource Table：下游各方向的 UserID 占用</text>
<text x="1772.0" y="500.5" font-size="8.5" fill="#475569">VC Credit 计数器：每下游方向每 VC 一个</text>
<text x="1772.0" y="514.0" font-size="8.5" fill="#475569">Output Buffer + Packet Shifter（按总线宽度拼接）</text>
<text x="1772.0" y="527.5" font-size="8.5" fill="#475569">Credit Release 静态旁路：按 CSR 的方向 Mask 转发</text>
<text x="1772.0" y="541.0" font-size="8.5" fill="#475569">坏核：只透传，不查 credit、不支持阻塞重发</text>
<rect x="790" y="734.0" width="340" height="167.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="802" y="755.0" font-size="11" fill="#111827" font-weight="600">RouterStation[mid]</text>
<text x="802.0" y="772.0" font-size="8.5" fill="#475569">Header Parser：取 path_id · path_core_mask · user_id</text>
<text x="802.0" y="785.5" font-size="8.5" fill="#475569">　size · vc_id，查 RouterTable 得出方向与资源</text>
<text x="802.0" y="799.0" font-size="8.5" fill="#475569">VC Buffer ×4（private 20 + shared pool 约 20 flit）</text>
<text x="802.0" y="812.5" font-size="8.5" fill="#475569">Packet Context：VC · 输出方向 · 剩余长度 · 包边界</text>
<text x="802.0" y="826.0" font-size="8.5" fill="#475569">Stream Resource Table：下游各方向的 UserID 占用</text>
<text x="802.0" y="839.5" font-size="8.5" fill="#475569">VC Credit 计数器：每下游方向每 VC 一个</text>
<text x="802.0" y="853.0" font-size="8.5" fill="#475569">Output Buffer + Packet Shifter（按总线宽度拼接）</text>
<text x="802.0" y="866.5" font-size="8.5" fill="#475569">Credit Release 静态旁路：按 CSR 的方向 Mask 转发</text>
<text x="802.0" y="880.0" font-size="8.5" fill="#475569">坏核：只透传，不查 credit、不支持阻塞重发</text>
<polygon points="49,44 160,44 151,74 40,74" fill="#f8fafc" stroke="#374151"/>
<text x="100.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">cfg（ctrl_noc）</text>
<polygon points="779,44 890,44 881,74 770,74" fill="#f8fafc" stroke="#374151"/>
<text x="830.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">in_core_data_ch</text>
<polygon points="909,44 1020,44 1011,74 900,74" fill="#f8fafc" stroke="#374151"/>
<text x="960.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">out_core_data_ch</text>
<polygon points="1039,44 1150,44 1141,74 1030,74" fill="#f8fafc" stroke="#374151"/>
<text x="1090.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">router2ts_trigger_ch</text>
<polygon points="1209,44 1350,44 1341,74 1200,74" fill="#f8fafc" stroke="#374151"/>
<text x="1275.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">ts2router retire / credit 返还</text>
<polygon points="1469,44 1590,44 1581,74 1460,74" fill="#f8fafc" stroke="#374151"/>
<text x="1525.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">rmem2ts_done_ch</text>
<polygon points="1619,44 1760,44 1751,74 1610,74" fill="#f8fafc" stroke="#374151"/>
<text x="1685.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">router2ts_credit_ch</text>
<polygon points="1799,44 1940,44 1931,74 1790,74" fill="#f8fafc" stroke="#374151"/>
<text x="1865.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">ts2router 资源注册</text>
<polygon points="29,462.75 140,462.75 131,494.75 20,494.75" fill="#f8fafc" stroke="#374151"/>
<text x="80.0" y="482.2" font-size="9" fill="#374151" text-anchor="middle">left_data_ch</text>
<polygon points="2109,462.75 2220,462.75 2211,494.75 2100,494.75" fill="#f8fafc" stroke="#374151"/>
<text x="2160.0" y="482.2" font-size="9" fill="#374151" text-anchor="middle">right_data_ch</text>
<polygon points="909,941.5 1020,941.5 1011,973.5 900,973.5" fill="#f8fafc" stroke="#374151"/>
<text x="960.0" y="961.0" font-size="9" fill="#374151" text-anchor="middle">mid_data_ch</text>
<path d="M136.5 478.8 L169.0 478.8" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M2101.0 478.8 L2103.5 478.8" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M960.1 902.5 L964.4 940.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M510.0 445.2 L759.0 461.4" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="615.4" y="431.8" width="38.3" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="634.5010464655378" y="439.25" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">flit 入</text>
<path d="M760.0 550.0 L511.0 512.4" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="585.9" y="498.8" width="99.2" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="635.4943808056712" y="506.25" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">出口 → output buffer</text>
<path d="M1828.0 562.5 L1828.0 674.0 L1120.0 674.0 L1120.0 617.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1498.0" y="661.5" width="103.9" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1550" y="669.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">right 的 flit 入 Xbar</text>
<path d="M1060.0 616.5 L1060.0 692.0 L1930.0 692.0 L1930.0 563.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1507.6" y="695.5" width="84.9" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1550" y="703.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">Xbar → right 出口</text>
<path d="M892.0 734.0 L840.4 617.4" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="878.8" y="678.5" width="10.5" height="33.5" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 884 695.25)" x="884" y="698.2" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">mid 入</text>
<path d="M940.0 616.5 L1027.4 733.2" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="942.8" y="669.5" width="10.5" height="51.6" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 948 695.25)" x="948" y="698.2" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">→ mid 出口</text>
<path d="M880.0 395.0 L880.0 302.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="866.8" y="295.2" width="10.5" height="105.6" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 872 348.0)" x="872" y="351.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">→ core（进 core 整包）</text>
<path d="M1040.0 301.0 L1040.0 394.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1042.8" y="303.7" width="10.5" height="88.6" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 1048 348.0)" x="1048" y="351.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">local ←（出 core）</text>
<path d="M1160.0 461.4 L1299.0 469.6" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1189.4" y="447.9" width="80.1" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1229.5008658896331" y="455.45" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">→ reduce 输入 ×3</text>
<path d="M1300.0 569.3 L1161.0 550.2" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1177.9" y="536.5" width="105.3" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1230.4953394199226" y="544.05" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">结果回注（第 5 路输入）</text>
<path d="M830.0 120.0 L825.6 75.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M955.5 74.0 L959.9 119.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M1090.0 120.0 L1085.6 75.0" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#g)"/>
<path d="M1270.5 74.0 L1274.9 119.0" stroke="#b45309" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#o)"/>
<path d="M1525.0 395.0 L1520.5 75.0" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#g)"/>
<path d="M1685.0 120.0 L1680.6 75.0" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#g)"/>
<path d="M1860.5 74.0 L1864.9 119.0" stroke="#b45309" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#o)"/>
<path d="M731.0 210.5 L759.0 210.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1200.0 217.2 L1161.0 210.7" stroke="#b45309" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#o)"/>
<path d="M1400.1 314.5 L1400.0 394.0" stroke="#b45309" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#o)"/>
<rect x="1402.8" y="302.4" width="10.5" height="104.6" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 1408 354.75)" x="1408" y="357.7" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#b45309" text-anchor="middle">Retire 广播 → 延迟回收</text>
<text x="20" y="1074" font-size="10.5" fill="#374151" text-anchor="start">通路载荷：in / out_core_data_ch 是 AXI-Stream-Like，对本 core 的 DTE DSA 走 VC credit 协议；router2ts_trigger_ch = user_id · path_id · 重发标记；router2ts_credit_ch = stream_id · task_id · path_id；rmem2ts_done_ch = UserID（reduce 整包完成）；ts2router 资源注册 = UserID · StreamID · TaskID · PathID。</text>
<text x="20" y="1094" font-size="10.5" fill="#374151" text-anchor="start">CoreMem 重发 ↔ CoreStation：stall_way = 转存时整包重定向到本地 Core Mem；Retire → CoreStation：停发该 UserID。</text>
<path d="M271.0 315.0 L272.0 394.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#a)"/>
<rect x="440.7" y="377.5" width="238.6" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="560" y="385.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">查表结果 → 各 RouterStation 与 ReduceModule（副本）</text>
<path d="M95.5 74.0 L105.8 119.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<rect x="230.7" y="88.5" width="338.5" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="400" y="96" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#7c3aed" text-anchor="middle">cfg 写事务 → RouterTable / CSR、Credit Bypass Route、各 Station 静态配置</text>
<text x="20" y="1114" font-size="10.5" fill="#374151" text-anchor="start">三类 credit 互不复用：VC credit 管下游 VC Buffer 的空槽（RouterStation 维护）；stream credit 管目标 core 的 Core Mem 空间（Router 是唯一有效状态，DTE 持 cache）；</text>
<text x="20" y="1134" font-size="10.5" fill="#374151" text-anchor="start">Reduce credit 管下游 ReduceModule 的上下文（DTE 管本级，ReduceModule 管相邻下游，Router 不维护）。ReduceModule 输入三路各 160 GB/s，输出 160 GB/s。</text>
</svg>
```

***

## 2　功能清单

一功能一条，编号供“机制覆盖”一章引用。

### RouterStation（×3：left / right / mid）

| 编号 | 功能 |
| - | - |
| F1 | Header Parser 从包头取出 `path_id`、`path_core_mask`、`user_id`、`size`、`vc_id`，按 `path_id` 查本地 RouterTable 副本，为这个包建立路由与资源上下文：出方向、下一跳 VC、进不进本 core、要不要查 stream credit table、是不是 reduce、`stall_way` |
| F2 | Payload flit 按 Header 指定的 VC 号写入对应 VC Buffer：该 VC 的 private 未满就占 private，满了就占本方向的 shared pool。四个 VC 的 private 互不侵占，一个 VC 堵住不影响其他 VC |
| F3 | VC 头部 flit 检查所有目标方向的资源：下游该 VC 的 credit、目标方向的 Stream 授权、要进 ReduceModule 时的 Reduce 准入 |
| F4 | 多播要所有目标方向的资源同时到手，任一方向不足则整体等待，不允许各方向独立前进 |
| F5 | 拿不到资源时按 `stall_way` 二选一：留在当前 VC 等，或把整包转进本地 Core Mem 由 DTE 重发 |
| F6 | VC credit 分两级记账，与下游 VC Buffer 的分配规则一一对应：每个下游方向的每个 VC 一个 private 计数器上电值 20，另有每个下游方向一个 shared 计数器上电值 20。发送时 `private[o][v] > 0` 就扣 private，否则扣 `shared[o]`；两者都为 0 时该 VC 不能发 |
| F7 | 一个方向的 credit 总量 = 4 × 20 + 20 = 100，正好等于下游该方向的 VC Buffer 容量，任何时刻都不会超发。上游不需要第二道反压信号，链路上也没有 ready |
| F8 | flit 离开下游 VC Buffer 就归还 VC credit，走共享总线（`credit_return_vld` 加 `credit_return_vc_id`），每个 input port 一拍最多一个 VC 被读出，无冲突 |
| F9 | 归还按同一条规则回填：`private[o][v] < 2` 就补 private，否则补 `shared[o]`。下游也是先占 private 再借 shared、离开时对称释放，两边规则相同，计数因此不会漂移 |
| F10 | credit 不足的 VC 被跳过，同一个 input port 的其他 VC 不受影响 |
| F11 | Stream Resource Table 维护所有下游方向的 UserID 占用；只有所有需求方向都满足才允许发送 |
| F12 | 出口方向的 Output Buffer 与 Packet Shifter 按总线宽度移位拼接后从 `<方向>_data_out_ch` 发出 |
| F13 | 同 VC 保序：VC Buffer 是 FIFO，同 VC 内 flit 严格按到达顺序读出，资源检查与仲裁都不重排；跨 VC、跨 input port 之间不保证顺序 |
| F14 | Credit Release 静态旁路：Stream 与 Reduce 两类 release 不查 RouterTable、不做动态路径选择、不进 Xbar 仲裁，只按 CSR 配的静态方向 Mask 转发；Mask 含多个方向时同一笔 release 复制到所有指定方向，UserID 与 credit 类型保持不变 |
| F15 | 坏核上的 RouterStation 只走直通：数据走完整流水线但不投递本 core，不检查 credit、不支持阻塞重发；上游要检查的 credit 对应坏核之后那个好核，下游返还的 credit 也跨过坏核直接给上游好核 |
| F16 | 边沿 Router 通过配置禁用不存在的端口，统一规格实现 |

### Xbar

| 编号 | 功能 |
| - | - |
| F17 | 5 入 7 出：入是 `left` / `right` / `mid` / `local` / ReduceModule 回注；出是 `left` / `right` / `mid` / `core` / `reduce_0` / `reduce_1` / `reduce_2` |
| F18 | 五个输入的目标输出方向互不冲突时同周期并行传输，不做不必要的串行化；ReduceModule 一侧要能承接三路并发输入 |
| F19 | 每个 output port 各有一个独立的 RoundRobin 仲裁器，每拍独立仲裁，不跨拍锁定 |
| F20 | 仲裁不是纯 RoundRobin，是贪婪整包。优先级由高到低：接口传输 priority → 当前输入是否有整包 → 当前请求是否为上一包的 body → 正常 RoundRobin |
| F21 | 多播同一拍 1 到 N 复制，每个 output 的 Mux 独立控制，多个 output 可选同一个 input；任一目标没握手就不推进任何分支 |
| F22 | 握手成功后统一扣各目标的 credit、更新包上下文 |
| F23 | 交织粒度分两种：Router 到 Router 的通路允许在 flit 边界切换包，需保存 VC、输出方向、剩余长度和包边界上下文；包一旦开始进入 Core 或 ReduceModule 就锁定到尾 flit。两种粒度不能混用 |

### CoreStation

| 编号 | 功能 |
| - | - |
| F24 | 进 core 的三态准入：UserID 已分配则直接收；未分配但 stream credit 表有空项则记录 UserID 占用后收；无空项时该 VC 不能向 Core 发数据，但 VC 有空项时仍可继续接收上游数据 |
| F25 | 已通过 Stream 检查，进 core 不再检查对 Core 方向的 VC credit，一定有 Core Mem 空间 |
| F26 | Header 写入 HeaderFIFO，Payload 写入 OutputBuffer（in_core_fifo），两者保持同一包顺序与边界 |
| F27 | Core 入口以整包为单位，不支持包间交织，必须发完一个整包再发下一个 |
| F28 | 收满一个包后按接收包头的顺序经 `router2ts_trigger_ch` 直接通知 TS，请求里带 `user_id`、`path_id`、这一笔要不要重发的标记，以及这个用户要不要做计算的 `compute` 位。这条通路硬件直连，不经 RV core，也不经任何软件环节 |
| F29 | 判定一个包收完的依据：比较已接收数据量与包头里的 payload 大小 |
| F30 | `compute` 取自包头软件 payload 里的一位，CoreStation 原样转给 TS，不做解释。DP+P2P 场景下 TS 用它跳过与本用户无关的那一半任务（**待定**：这一位在包头里的位置等 Router 接口规范定下包头位域后回填） |
| F31 | 请求发出后保持到 TS 拉 `ready`。TS 的 trigger 入口占满时本笔请求原地保持，CoreStation 不发下一笔，也不清 HeaderFIFO 的队头。请求不允许丢弃：丢一笔 trigger 就等于丢一个 token |
| F32 | DTE RV core 经 `cm_lsq` 映射到 Router I/O reg 的地址段读包头生成搬运任务，读完向指定地址写 1 把包头弹出，CoreStation 映射出下一个包头。Router 侧的 `hdr_rd` 是这条通路的从端，core 内不另设第二条读包头的通路 |
| F33 | 被反压时保持 valid、当前 Header、Payload、首尾 flit 标志与有效字节信息不变，传输位置不得前移；反压解除后从同一 flit 继续握手，保证包不丢拍、不重拍、不跨包、不串包 |
| F34 | 出 core：DTE 按 PathID 查自己那份 RouterTable 得到 VC 与资源需求，先向 Router 申请到下游 Stream 或 Reduce 资源，再在目标 VC 有空时经 `out_core_data_ch` 发出整包；CoreStation 拆出 Header 与 Payload，按 Header 的 VC 号写入 Core 方向输入 VC，之后与其他方向一样查表、参与仲裁 |
| F35 | 进 core 与出 core 两条数据通路完全并行，互不共享数据通路仲裁状态 |

### ReduceModule

| 编号 | 功能 |
| - | - |
| F36 | 三路输入仲裁 SRAM、Bank 与计算资源；进入后锁定当前包直至尾 flit |
| F37 | 输入精度处理：BF16 扩展为 FP32，FP32 直接进入，数据面统一 FP32 |
| F38 | 同一 User 同一包的第一份输入分配上下文并写入 FP32 数据，后续方向的输入读出当前值、累加、写回，原位 Read-Modify-Write |
| F39 | 上下文保护：当前包的全部输入完成并输出前，同一 User 的下一个包不得覆盖该上下文 |
| F40 | 必须执行 Reduce：SRAM、Bank 或计算单元暂不可用时对输入反压，不允许绕过 Reduce 降级为直接存储或转发 |
| F41 | 精度：中间累加固定 FP32，输出按 RouterTable 的 `reduce_outdata_type` 转成 FP32 或 BF16 |
| F42 | 全部方向输入完成后结果进输出队列，发送前查目标 VC credit 与该方向的下游 Reduce credit，作为 Xbar 的第五路输入重新参与仲裁 |
| F43 | `op_type` 分 reduce 与 reduce_twice 两档：reduce 第一次收到就写入上下文，reduce_twice 要到第二次或第三次收到才与上下文里的值相加。两个源允许都来自本 core 的 DTE，即用 Router 完成 core 内两个 token 的 reduce 再发出 |
| F44 | “全部方向”取自 RouterTable 的 `reduce_in_mask`：按包头的 `path_id` 查 `rdc_rtab[path_id].reduce_in_mask`，得到这条 path 在本级会有哪几个相邻方向送来分量。`all_in = (in_done_mask == reduce_in_mask)`。首份输入建上下文时把这个集合一并记进 `rdc_user_tab`，本包收齐前不再重查 |
| F45 | `reduce_in_mask` 为 0 表示本级不做累加：坏核与纯透传的中继核都是这一档，包按 `flow_dir` 直接转发，不进 ReduceModule |
| F46 | 每发一个 flit 扣一个下游 Reduce credit；下游每发出一个 flit 产生携带 UserID 的 release，经 Router 的静态旁路返回上游 |
| F47 | Downstream Reduce Credit Map 按 UserID 加目标方向维护相邻下游的 Reduce credit，逐 flit 扣减、按 release 恢复 |
| F48 | 整包发出后经 `rmem2ts_done_ch` 向 core 返回 UserID 与该包包头里的 `reduce_seq`。TS 只认这一路把 reduce task 置 FINISH，并按 `reduce_seq` 与 DTE 的那一半配对 |
| F49 | 输入侧只有 VC credit 准入，`credit > 0` 即收；Reduce credit 是另一张网，与数据面分离 |
| F50 | 表项的建与删：用户建 stream credit 表项时分配一个 entry 的 credit 数量；收到本级 core 该 UserID 的 Retire 且相邻下游各方向的 credit 全部恢复到分配数量后，才删掉这一项给其他用户用 |
| F51 | 16 个用户上下文与进 core 的 stream credit 表项**一一对应**，同为 16 项，同在用户建 stream credit 表项时占用、同在 Retire 时回收。因此不会出现 Stream 已授权而 ReduceModule 没有上下文的情况，反压只覆盖 SRAM、Bank 与计算单元三种暂时不可用，不必覆盖上下文耗尽 |
| F52 | 链上没有同步点：上游分量到达时不必等本 core 算完，先存进上下文，本地出核的分量出来时再加 |

### RouterTable / CSR

| 编号 | 功能 |
| - | - |
| F53 | 64 条表项，按 `path_id` 索引。字段照 DATA_NOC HAS 的 `Routing table field`，VC 与阻塞那几项照 Router MAS 的 `Table Entry`：<br>`op_type` 2 bit（0 = kernel / weight 搬运、1 = transfer、2 = reduce、3 = reduce_twice）<br>`flow_dir` 5 bit 出方向掩码（bit0 上下、bit1 左、bit2 右、bit3 reduce1、bit4 reduce2）<br>`cur_vc` 2 bit、`nxt_vc` 五个出方向各 2 bit<br>`path_core_mask_enable` 1 bit、`path_core_mask_idx` 4 bit、`path_core_bypass` 1 bit（0 进 core、1 bypass）<br>`need_buffer` 1 bit（这条 path 允许进 core 缓存，即溢流使能）<br>`stream_table_enable` 1 bit（这个包要不要查输出端的 stream credit table）<br>`cur_credit_type` 1 bit（0 广播 / 1 P2P）、`cur_credit_require` 6 bit<br>`nxt_credit_type` 3 bit（三个 R2R 方向各一位）、`nxt_credit_require` 三个 R2R 方向各 6 bit<br>`reduce_data_type` 3 bit（输入精度 BF16 / FP32）、`reduce_outdata_type` 1 bit（输出精度）<br>`reduce_in_mask` 3 bit、`operation` 2 bit（0 转发、1 Reduce0、2 Reduce1、3 Reduce2）、`stall_way` 1 bit |
| F54 | 进不进本 core 由两个字段二选一决定：`path_core_mask_enable = 0` 时按 `path_core_bypass` 定，适用于普通广播与 P2P；`= 1` 时取 MSG 里 `path_core_mask` 的第 `path_core_mask_idx` 位，适用于 EP 广播。**位到 core 的对应关系不是固定编码**，每个 core 在自己的表项里指定看哪一位 |
| F55 | `reduce_data_type` 配在表里而不是由 TS 给，是因为 `reduce_twice` 时 Router 可能先收到两个远程的 Reduce Token 而不是本 core 发出的那一份，那时 TS 还没有介入 |
| F56 | `op_type = 0` 的 kernel / weight 搬运包进 core 时**跳过 TS，直接唤醒 DTE**，不走 `router2ts_trigger_ch` 那条建表通路 |
| F57 | 另有一组与 RouterTable 分开配的 **Skip Mask 寄存器**：per-core 一位，标记该 core 是否被跳过。复位释放后 RouterTable 的所有条目为 bypass / no-op，配置写入前不投递任何包 |
| F58 | `flow_dir` 与 `reduce_in_mask` 一个管出一个管进：前者是这条 path 从本级往哪几个方向发，后者是这条 path 在本级要等哪几个相邻方向的分量。两者互相独立，出分量的源核 `reduce_in_mask` 为 0；最终汇聚的核 `flow_dir` 全不置位，它只把结果交给本 core，进核由 `path_core_bypass` 判 |
| F59 | 只描述静态路由与资源需求，不保存包的动态执行状态 |
| F60 | 内部多副本：所有需要并行查询的位置各持一份，由 Router 的配置入口统一接收写事务 |
| F61 | 更新状态机把同一笔写依次写入全部副本并记录完成状态；全部副本写完才向软件返回完成，禁止暴露部分新部分旧的状态 |
| F62 | 外部两份副本由 DTE 与 ReduceModule 各自维护，软件负责写入相同配置并保证三方一致，Router 硬件不同步外部副本；软件只能在 Router 提交完成后再写它们 |
| F63 | Credit Bypass Route：软件经 CSR 为每个业务 credit 输入端口配置静态输出方向 Mask，可指定一个或多个 R2R 方向与 Core 方向；坏核场景切换 credit 路径靠的就是改这组配置 |
| F64 | 中间节点可以按表改写 VC |

### CoreMem 重发

| 编号 | 功能 |
| - | - |
| F65 | `stall_way` 选转存时把整包重定向到本地 Core Mem 缓存，Router 上的 Bypass 操作被映射成“进 core 加出 core”两段 |
| F66 | Core Mem 中只保存包，包头含 UserID、PathID、size；重发时用 PathID 重新查 RouterTable，不重复保存 VC 和路由信息 |
| F67 | 暂存空间由软件在 Core Mem 里预留，容量与项数取自 `cmem_part` 的 `reissue_base` 与 `reissue_pkts_per_vc`，每 VC 至少容得下一个整包。`pending_reinject` 计数到这个上限说明配少了，**模型直接断言失败**：不覆盖已暂存的包，不丢包，也不退回“留在当前 VC 等” —— 那条路会让同一个 `stall_way` 配置在两种容量下走出两种行为，掩盖配置错误 |
| F68 | 同 VC 保序：同一 VC 存在未完成的 CoreMem 重发包时，后续包不得越过；按 VC 粒度维护 `pending_reinject` 计数器防超车 |
| F69 | 包进 core 暂存时改写 `overflow_reinject = 1`，出 core 重发时 Router 改回 0；Output Port 识别到重注入标记才扣减 credit |
| F70 | 坏核不接收溢流：Router 对坏核不发起进 core 缓存处理，此时 coremem credit 直接 bypass |
| F71 | 无论直接发送还是经 CoreMem 重发，完成后都向 core 内 TS 返回至少含 UserID 加 PathID 的完成信息 |

### Retire

| 编号 | 功能 |
| - | - |
| F72 | ReduceModule 完成计算并发出全部包后向 Core 返回 UserID；Core 判定任务链结束后向 Router 和 ReduceModule 广播 User Retire |
| F73 | Core 的保证：仅可在该 UserID 的全部进 core、出 core 数据搬运完成，且不会再发起新搬运之后发 Retire。Retire 发出后，Router 上不得再出现以该 Core 为源或目标的该用户包 |
| F74 | Router 的动作：停止该 UserID 的新发送，删除其全部 stream credit 授权表项 |
| F75 | ReduceModule 的动作是延迟回收，先记录 Retire，待相邻下游各方向的 Reduce credit 全部恢复到初始值后才删除对应用户映射 |
| F76 | Stream credit 的回程：每个 Router 用一个组合逻辑的 core credit crossbar 汇总本级 core 与所有下级出口的 pulse 加 user，发往除来向外的另两个 R2R port，逐跳传到上游；跨 chip 经 C2C Bridge 透传 |

### CoreMemCreditMonitor

| 编号 | 功能 |
| - | - |
| F77 | 监听事件队列 16 项全相连，可同时监听多笔多方向的资源申请 |
| F78 | TS 注册时带 UserID、StreamID、TaskID、PathID；Router 按 PathID 查到需要发送的下游方向、VC 需求与 stream 需求 |
| F79 | 申请到就经反向控制通路通知 TS，回 StreamID、TaskID、PathID；申请不到就把需求记进队列监听，资源满足再通知 |
| F80 | 多个事件同时满足时按 StreamID 仲裁，选最老的任务通知 TS |
| F81 | 进 core 重发的任务也注册到该队列，数据进 Core、资源就绪后通知 TS 重发 |
| F82 | 同一 VC 的数据包要保序，当前 VC 有未重发完的数据时后续包不能提前发送 |
| F83 | 只有 Router 负责真正申请 Stream 表项。DTE 要发数据必须先从 Router 拿到指定 user 的授权，禁止超额分配或重复授权 |
| F84 | Router 的进 core 表和 TS 内部的 stream credit 表按完全一致的逻辑申请空项，分配因此不会多于实际资源数，这保证了“Router 通知 TS 的包一定能被 TS 接收” |
| F85 | 另输出 per-port 的 `stream_credit` 同步信息给 core 与 DTE，用于判断重注入 |

### VC 分配与更换

| 编号 | 功能 |
| - | - |
| F86 | VC 数取 4 的依据：目前最复杂的场景里，一个 Router 最多同时经过 4 条同向数据流 |
| F87 | VC3 专给逐级 reduce，VC0 / VC1 / VC2 支持除 reduce 外的全部操作类型、由软件配。逐级 reduce 单独占一个 VC 的原因有三条：这类数据基本不进 core，在 Router 里直接与 core 内数据求和；绝大多数场景都存在逐级 reduce；reduce 要求整包缓冲，每个 VC 都支持 reduce 的话 buffer 尺寸过大 |
| F88 | 三个传输方向各一套独立的 VC 与 credit，防止不同方向的数据流互相阻塞 |
| F89 | VC Buffer 的容量分配：reduce 专用的 VC3 是 16 KB，三个共享 VC 各 8 KB，三个方向各一套，合计 (16 + 8 × 3) × 3 = 120 KB |
| F90 | 软件分配 VC 的两条原则：可能互相阻塞导致死锁的数据流**必须**分到不同 VC；可能互相阻塞导致性能下降的数据流**尽量**分到不同 VC |
| F91 | 要换 VC 的场景：一条 path 前半段做 reduce、后半段做 bypass，需要在 DTE 的 RV core 软件不介入的前提下由 Router 自己换。必须实现的是 RouterTable 里为每条 `path_id` 指定“下一跳的 VC” |
| F92 | 换 VC 的实现方式已定：下一跳的 VC 编码在包头里，下一跳 Router 解包直接取；本跳 Router 读到表里的“下一跳 VC”后改写包头里的这个字段供下一跳读。被否掉的另一种做法是包里不带 VC、每个 Router 各查各的表得到“本跳 VC”与“下一跳 VC”，它要求上一跳表里的“下一跳 VC”与本跳表里的“本跳 VC”永远相等，配错就死锁 |
| F93 | 逻辑分区不等于物理存储：三个 VC Buffer 可能落在同一块物理 SRAM 上、靠地址划分。建模按逻辑分区建，各分区的容量各自记账 |

### 逐级与非逐级归约

| 编号 | 功能 |
| - | - |
| F94 | 逐级的 reduce / concat 在 Router 里按序执行，只缓存最老那个用户的数据，最多支持 3 个数据源同时参与：两个上游 core 的分量加一个本 core 的分量 |
| F95 | 非逐级的 reduce / concat **必须进 core 内执行**：数据在 Core Mem 里等其他来源到达后再算，支持不同用户之间的数据乱序到达。原因是非逐级汇聚时用户间会乱序 —— chip0 往 chip1 传 usr0 的数据途中，chip1 自己 usr1 的数据可能先到汇聚点 |
| F96 | ReduceBuffer 要装得下一个完整 Token：Core 必须一次性把 Reduce Token 完整发进 Router 的 ReduceBuffer，不能分段传输，否则效率严重下降。按 8192 × 2 B 算，一个 Token 是 16 KB |
| F97 | ReduceBuffer 不得用作流量控制的缓存。“Reduce 完成后不切 `path_id`，直接从 ReduceBuffer 发 P2P”这种把两条 path 合并的做法，只在后续 P2P 能立即发出时才允许；发不出去就让 Reduce 结果先进本 core 的 Core Mem，用 Core Mem 做流控缓冲 |

### path_id 与 path_core_mask 的分配

| 编号 | 功能 |
| - | - |
| F98 | 两个通信事务满足任一条时必须分配新的 `path_id`：一是涉及的 R2R 路径有重叠但后续传播节点不完全一致，例如广播到 core01 与广播到 core0123；二是在同一个 core 上做的操作不同，例如 core0123 上的 broadcast 与 reduce |
| F99 | 两条反例，路径不冲突时可以共用同一个 `path_id`：分别在 core01 和 core23 上广播，路径完全无关；分别在 core03 和 core23 上做 P2P，路径重叠但后半部分完全一致 |
| F100 | `path_core_mask` 16 bit 理论上最大支持 EP16；组合数溢出时由 DTE core 的软件程序换一个新的 `path_id` 解决，硬件不做处理 |
| F101 | `path_core_mask` 的第二种用法：DP 广播固定广播到每个 core，靠 mask 判断该 core 做不做计算。好处是只占 1 项 RouterTable，代价是浪费总线带宽 —— core0 做计算时，core0 之后那段广播传输其实是多余的 |
| F102 | EP 多播两种编码方式的表项数对比：纯 `path_id` 编码要 2^K − 1 项；`path_id` 加 `path_core_mask` 编码只要 K 项 `path_id`，配 2^K 种 mask |

### 包格式与透传

| 编号 | 功能 |
| - | - |
| F103 | 最短包长 16 B，即只含包头与路由信息的空包；最长允许 64 KB，实际支持到 (16 K + 32) B |
| F104 | 不设包尾：包的结束靠包长度计数。整包校验建议物理层按 transfer 做、软件做整包校验，软件的校验位算进软件 payload |
| F105 | reduce 包的软件辅助信息长度**必须固定 16 B**，Router 做 reduce 加法时固定跳过这 16 B。软件层面 reduce 包只需要传 `user_id` 加操作类型（如 concat idx），16 B 够用 |
| F106 | 其他类型的包 Router 完全不感知内容和长度，只做透明传输 |
| F107 | Router 把数据送给 DTE 时**不剥离任何数据**，DTE 的软件能看到包头、路由信息在内的全部内容。其中 VC 字段每一跳 Router 都会改，每个 core 看到的内容不完全一致 |

### credit 的记账单位

| 编号 | 功能 |
| - | - |
| F108 | credit 的单位是 1 KB |
| F109 | 广播扣 credit 时扣掉的不只是广播数据本身的空间，还含提前预留的输出结果空间。原文的例子是一笔广播扣 32，其中 8 KB 是广播数据空间、24 KB 是提前预留的输出结果空间，该方向的 credit 从 128 减到 96 |
| F110 | 广播 credit 的释放时机是计算完成、运算结果写到下游的同时，把这一笔一次性释放回上游的 CreditMonitor |
| F111 | bypass 的扣与还：Core0 的 Router 查表要 bypass 给 Core1 时先查 Core1 的 credit 并扣掉，Core1 算完释放空间后通知 Core0 的 CreditMonitor 加回；反向同理，core2 的数据要经 core1 bypass 时扣 core1 的 credit，core1 把数据发给 core0 后归还 |

***

## 3　接口

```
port link[d] (双向, credit/release, clk)          // d ∈ {left, right, mid}：256 B/T，接相邻 core 的 Router 或 C2C Bridge
  in  flit_valid · flit_vc[1:0] · flit_head · flit_tail · flit_bytes[8:0] · flit_msg
  in  credit_return_vld · credit_return_vc_id[1:0]        // VC credit 归还，共享总线，一拍最多一个 VC
  in  stream_release_vld · stream_release_user[15:0]      // stream credit release
  in  reduce_release_vld · reduce_release_user[15:0]      // Reduce credit release
  out 同字段
port in_core_data_ch (master, AXI-Stream-Like, clk)   // CoreStation → DTE：进 core 的整包
  out tvalid · tdata[2047:0] · tkeep[255:0] · tlast · thdr
  in  tready                                              // = DTE 的 inbound buffer 与 Commit 资源都够
port out_core_data_ch (slave, AXI-Stream-Like, clk)   // DTE → CoreStation：出 core 的整包
  in  tvalid · tdata[2047:0] · tkeep[255:0] · tlast · thdr · vc_id[1:0]
  out tready                                              // = 该 VC 的 Core 方向输入 VC 有空
port hdr_rd (slave, AXI-Full 类, clk)                 // DTE RV core 的 cm_lsq 读 HeaderFIFO 的包头，读完写 1 弹出
  in  araddr[31:0] · arvalid · awaddr[31:0] · wvalid · wdata[31:0]
  out arready · rvalid · rdata[255:0] · awready · wready
port router2ts_trigger_ch (master, valid/ready, clk)  // CoreStation → TS：收满一个包
  out valid · user_id[15:0] · path_id[7:0] · reissue · compute
  in  ready                                               // = TS 的 trigger 入口队列有空项；拉低时 CoreStation 保持本笔请求，不发下一笔
port router2ts_credit_ch (master, 脉冲, clk)          // CoreMemCreditMonitor → TS：资源到手
  out valid · stream_id[3:0] · task_id[5:0] · path_id[7:0]
port rmem2ts_done_ch (master, 脉冲, clk)              // ReduceModule → TS：一个整包 reduce 完成
  out valid · user_id[15:0] · reduce_seq[5:0]           // reduce_seq 原样取自包头，供 TS 逐包配对
port ts2router_req (slave, valid/ready, clk)          // TS → CoreMemCreditMonitor：注册资源申请
  in  req_valid · user_id[15:0] · stream_id[3:0] · task_id[5:0] · path_id[7:0]
  out req_ready                                           // = 监听事件队列有空项
port ts2router_retire (slave, valid/ready, clk)        // TS → Retire：用户退休与 credit 返还
  in  valid · user_id[15:0]
  out accepted                                            // 即 ready：Router 接收后 TS 才清 valid 并推进 head_ptr
port stream_credit (master, 电平, clk)               // per-port 的 stream credit 同步信息，给 core 与 DTE
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
mem rtab[k]           FF 阵列   64 × {op_type[1:0], flow_dir[4:0], cur_vc[1:0], nxt_vc[4:0][1:0], path_core_mask_enable, path_core_mask_idx[3:0], path_core_bypass, need_buffer, stream_table_enable, cur_credit_type, cur_credit_require[5:0], nxt_credit_type[2:0], nxt_credit_require[2:0][5:0], reduce_data_type[2:0], reduce_outdata_type, reduce_in_mask[2:0], operation[1:0], stall_way}  1R1W  多副本，逐份写  复位 全条目 bypass / no-op
mem skip_mask         FF        10 b，per-core 一位                                          1R1W  与 RouterTable 分开配     复位 0
mem rtab_commit       FF        {副本写入游标, 完成状态}                                    1RW   更新状态机                复位 空闲
mem credit_bypass     FF 阵列   每个业务 credit 输入端口一个 {out_mask[6:0]}                1R1W  CSR 配置                  复位 0
mem vc_buf[d][v]      FIFO      private 每 VC 2 flit                                        1W1R  满且 shared 也满 → 不再收上游  复位空    // d ∈ {left,right,mid,core}，v ∈ 0..3
mem vc_shared[d]      FIFO      每方向 20 flit，四个 VC 先到先得                             1W1R  private 满时借用           复位空
mem pkt_ctx[d][v]     FF 阵列   {vc[1:0], out_mask[6:0], remain_len[15:0], head, tail, locked}  1RW  队首上下文             复位空
mem vc_credit[d][v]   FF        private 计数器，每下游方向每 VC 一个                          1RW   private > 0 时扣它；release 回来时优先补它  复位 2
mem vc_shared_cr[d]   FF        shared 计数器，每下游方向一个                                1RW   private 为 0 时扣它；private 已满时 release 补它  复位 20
mem stream_tab[d]     FF 阵列   每方向 16 项 × {valid, user_id[15:0]}                        1RW   建：新 UserID 首次到达；删：release 或 Retire  复位空
mem hdr_fifo          FIFO      16 × 256 B 包头                                             1W1R  DTE 读完写 1 弹出          复位空
mem out_buf           FIFO      32 flit（in_core_fifo）                                     1W1R  整包写入，不交织          复位空
mem rdc_ctx           SRAM      16 用户 × 16 KiB，FP32 中间累加结果                          1R1W  RMW 原位累加              复位未定义
mem rdc_user_tab      FF 阵列   16 × {user_id[15:0], path_id[7:0], pkt_state[2:0], in_done_mask[2:0], expect_mask[2:0], out_state[1:0], retired}  1RW  建上下文时记下 expect_mask = rdc_rtab[path_id].reduce_in_mask  复位空
mem rdc_down_credit   FF 阵列   16 用户 × 3 方向 × 计数器                                    1RW   发 flit 扣 1，release 加 1  复位 分配值
mem rdc_out_q         FIFO      8 flit                                                      1W1R  满 → 停止 RMW 输出         复位空
mem rdc_rtab          FF 阵列   64 项，RouterTable 的外部副本                                1R1W  软件写                    复位 0
mem reissue_tab       FF 阵列   每 VC 一项 {pending_reinject[7:0]}                           1RW   进 core 暂存加 1，重发减 1  复位 0
mem monitor_q         FF 阵列   16 项全相连 × {user_id[15:0], stream_id[3:0], task_id[5:0], path_id[7:0], need_mask[6:0]}  1RW  —  复位空
mem retire_pend       FF 阵列   16 × {user_id[15:0], from_core, to_rdc}                      1RW   广播后逐项回收            复位空
mem 级间 latch         级间 latch 各级之间的包上下文与 flit                                   —     每拍覆写                  —
```

### 编译侧怎么填这三张表

`rtab`（64 项按 `path_id` 索引）、`skip_mask`（每 chip 10 bit）、`credit_bypass`（每个业务 credit 输入端口一个 `out_mask`）都由编译侧算好，boot 期经 ctrl_noc 写入。同一个 `path_id` 在不同 core 上表项不同，`rtab` 因此**每 core 一份**，不能从拓扑反推。`CreditCounter[path_id][stream_id]` 的初值同样每 core 一份：广播任务等于目的 core 数量，P2P 任务是 1。

各字段的填法：

| 字段 | 填法 |
| - | - |
| `op_type` | 0 kernel / weight 搬运、1 transfer、2 reduce、3 reduce_twice。0 那一档的包进 core 时跳过 TS 直接唤醒 DTE |
| `flow_dir` | 出方向掩码，bit0 上下、bit1 左、bit2 右、bit3 reduce1、bit4 reduce2。单个有效位是单播，多个有效位是多播。**进本 core 不占这里的位**，末端核这一项全不置位 |
| `cur_vc` | 给上一级无法指定 VC 的入口用 |
| `nxt_vc` | VC 随 path 静态绑定，**相互依赖的流不排进同一个 VC** |
| `path_core_mask_enable` / `path_core_mask_idx` / `path_core_bypass` | 进不进本 core 由前者二选一：0 时按 `path_core_bypass` 定（**0 进 core、1 bypass**），适用于普通广播与 P2P；1 时取 MSG 里 `path_core_mask` 的第 `path_core_mask_idx` 位，适用于 EP 广播。**位到 core 的对应由每个 core 自己指定**，不是固定编码 |
| `need_buffer` | 这条 path 允许进 core 缓存。选它就等于选了 `stall_way` 的转存那一档，必须预留 Core Mem 空间并在任务链里安排 reissue 任务 |
| `stream_table_enable` | 这个包出核前要不要查对应输出端的 stream credit table。纯 bypass 的包不查 |
| `cur_credit_type` / `cur_credit_require` | 进核占用的 credit 池类型与额度。额度的语义是上游已拨给本核的量，不是再向下游申请；不进核的表项填 0 |
| `nxt_credit_type` / `nxt_credit_require` | 三个 R2R 方向各一份。某方向 `nxt_credit_require` 为 0 表示该方向不查 credit，只受链路反压；`flow_dir` 置位而 require 为 0 的方向随原子发送一起放行 |
| `reduce_data_type` / `reduce_outdata_type` | reduce 加法的输入精度与输出精度，各自取 BF16 或 FP32。中间累加固定 FP32，不受这两项影响 |
| `reduce_in_mask` | 这条 path 在本级会有哪几个相邻方向送来分量。取法是纯拓扑推导：在这条 path 的图上，把本核作为下一跳、且操作是 reduce 的那些相邻核，它们所在的方向就置位。出分量的源核填 0，坏核与纯透传的中继核也填 0 |
| `operation` | 本级在这条 path 上的角色：0 普通转发、1 Reduce0、2 Reduce1、3 Reduce2 |
| `stall_way` | 留在当前 VC 等，或转 Core Mem 暂存由 DTE 重发。选后者必须为它预留 Core Mem 空间并在任务链里安排 reissue 任务 |

`reduce_in_mask` 与 `operation` 的三档 reduce 取值，Router MAS 的 `Table Entry` 与 DATA_NOC HAS 的 `Routing table field` 都没有（**待确认**）。前者是逐级归约判断收齐所必需的，后者的 Reduce0 / Reduce1 / Reduce2 含义原始文档未定义，本文档按源分量 / 中继累加 / 最终汇聚给。

### 一套可直接照抄的实例：C6、C7 双坏核

chip 内 2×5 里 C6、C7 是坏核，三条 path 都经过它们。这套表项在模型里当 Router 单测的输入。

| Core | Path | flow_dir | path_core_bypass | stream_table_enable | operation | stall_way |
| - | - | - | - | - | - | - |
| C0 | Path0 广播 | 右 + 上下 | 1 不进核 | 1 | 转发 | 留在 VC |
| C0 | Path2 reduce | 右 | 1 不进核 | 0 | Reduce0 | 留在 VC |
| C5 | Path0 | 右 | 0 进核 | 0 | 转发 | 进 Core Mem 重发 |
| C5 | Path1 P2P | 右 | 1 不进核 | 0 | 转发 | 留在 VC |
| C5 | Path2 | 右 | 1 不进核 | 0 | Reduce1 | 留在 VC |
| C6 坏核 | Path0 / Path1 / Path2 | 右 | 1 不进核 | 0 | 转发（Path2 也只透传，不累加） | 只能留在 VC |
| C8 | Path0 | 右 | 0 进核 | 1 | 转发 | 进 Core Mem 重发 |
| C8 | Path1 | 右 | 1 不进核 | 1 | 转发 | 进 Core Mem 重发 |
| C8 | Path2 | 右 | 1 不进核 | 0 | Reduce1 | 留在 VC |
| C9 | Path0 / Path1 | 全不置位 | 0 进核 | 0 | 转发 | 末端 |
| C9 | Path2 | 全不置位 | 0 进核 | 0 | Reduce2 | 末端 |

同一个 core 上不同 path 的表项互相独立：C6 三条填法相同，仍然要三个表项。

两列各看各的：`flow_dir` 只管往外发哪几个方向，进不进本核看 `path_core_bypass`，所以 C5 与 C8 的 Path0 是“往右发同时进本核”，C9 是“不再外发只进本核”。`stream_table_enable` 看的是下一跳会不会把数据落进它的 core：C5 的 Path0 为 0 是因为下游 C6 是坏核不落地，C8 的 Path1 为 1 是因为下一跳 C9 要进核。

这张表没列 `reduce_in_mask`：它按上面那条纯拓扑规则从 Path2 的图推出来，C6 与 C7 是坏核填 0，C9 是两路汇入所以置 bit0 与 bit1。逐核的完整取值等 `operation` 的 Reduce0 / Reduce1 / Reduce2 含义定下来后一并回填（**待定**）。

### 一层里 path 怎么分

按数据流各分一条：

| path_id | 数据流 |
| - | - |
| 0 | 入口桩 → 第一个 B core 的 datain |
| 1 | B core 广播 token，目的含本组计算 core 与下一个 B core |
| 2 | FC1 reduce |
| 3 | FC3 reduce |
| 4 | FC2 input 广播 |
| 5～12 | FC2 reduce，共 8 条 |

本例共 13 条（path 0～12）。按 dp10 / ep16 / pp3 试算的上界是 55 条，64 项的表装得下。

***

## 5　流水线总览

数据面六级，每级 1 拍，单跳 ≤ 6 拍。进出 core、Reduce、溢流重发、控制四组各自成段，与数据面并行推进。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1176 636" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arrov" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="1176" height="636" fill="#ffffff"/>

  <text x="20" y="26" font-size="12" fill="#111827">Router · 第 1 层流水线总览（数据面六级每级 1 拍，单跳 ≤ 6 拍）</text>
  <text x="20" y="42" font-size="9.5" fill="#6b7280">横向是级序，不是拍序；每级的拍数在右上角 Dx。橙色虚线框是变长级，非按比例。</text>
  <path d="M150 52 L150 70" stroke="#e5e7eb" fill="none"/>
<path d="M150 126 L150 156" stroke="#e5e7eb" fill="none"/>
<path d="M150 212 L150 242" stroke="#e5e7eb" fill="none"/>
<path d="M150 298 L150 414" stroke="#e5e7eb" fill="none"/>
<path d="M150 470 L150 500" stroke="#e5e7eb" fill="none"/>
  <path d="M316 52 L316 70" stroke="#e5e7eb" fill="none"/>
<path d="M316 126 L316 156" stroke="#e5e7eb" fill="none"/>
<path d="M316 212 L316 242" stroke="#e5e7eb" fill="none"/>
<path d="M316 298 L316 414" stroke="#e5e7eb" fill="none"/>
<path d="M316 470 L316 500" stroke="#e5e7eb" fill="none"/>
  <path d="M482 52 L482 70" stroke="#e5e7eb" fill="none"/>
<path d="M482 126 L482 156" stroke="#e5e7eb" fill="none"/>
<path d="M482 212 L482 242" stroke="#e5e7eb" fill="none"/>
<path d="M482 298 L482 414" stroke="#e5e7eb" fill="none"/>
<path d="M482 470 L482 500" stroke="#e5e7eb" fill="none"/>
  <path d="M648 52 L648 70" stroke="#e5e7eb" fill="none"/>
<path d="M648 126 L648 500" stroke="#e5e7eb" fill="none"/>
  <path d="M814 52 L814 70" stroke="#e5e7eb" fill="none"/>
<path d="M814 126 L814 500" stroke="#e5e7eb" fill="none"/>
  <path d="M980 52 L980 70" stroke="#e5e7eb" fill="none"/>
<path d="M980 126 L980 500" stroke="#e5e7eb" fill="none"/>
  <text x="20" y="102" font-size="10.5" fill="#6b7280">数据面</text>
  <rect x="150" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="84" font-size="8.5" fill="#6b7280">M1</text>
  <text x="292" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="104" font-size="11" fill="#111827">Input VC Buffer</text>
  <rect x="316" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="84" font-size="8.5" fill="#6b7280">M2</text>
  <text x="458" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="104" font-size="11" fill="#111827">RC 查表</text>
  <path d="M300 98 L315 98" stroke="#475569" marker-end="url(#arrov)" fill="none"/>
  <rect x="482" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="492" y="84" font-size="8.5" fill="#6b7280">M3</text>
  <text x="624" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="492" y="104" font-size="11" fill="#111827">VA 拿资源</text>
  <path d="M466 98 L481 98" stroke="#475569" marker-end="url(#arrov)" fill="none"/>
  <rect x="648" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="658" y="84" font-size="8.5" fill="#6b7280">M4</text>
  <text x="790" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="658" y="104" font-size="11" fill="#111827">SA 抢通路</text>
  <path d="M632 98 L647 98" stroke="#475569" marker-end="url(#arrov)" fill="none"/>
  <rect x="814" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="824" y="84" font-size="8.5" fill="#6b7280">M5</text>
  <text x="956" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="824" y="104" font-size="11" fill="#111827">ST 交换</text>
  <path d="M798 98 L813 98" stroke="#475569" marker-end="url(#arrov)" fill="none"/>
  <rect x="980" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="990" y="84" font-size="8.5" fill="#6b7280">M6</text>
  <text x="1122" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="990" y="104" font-size="11" fill="#111827">Output Pipe</text>
  <path d="M964 98 L979 98" stroke="#475569" marker-end="url(#arrov)" fill="none"/>
  <text x="20" y="188" font-size="10.5" fill="#6b7280">进出 core</text>
  <rect x="150" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="170" font-size="8.5" fill="#6b7280">M7</text>
  <text x="292" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="190" font-size="11" fill="#111827">进 core 准入</text>
  <rect x="316" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="170" font-size="8.5" fill="#6b7280">M8</text>
  <text x="458" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="190" font-size="11" fill="#111827">收满通知 TS</text>
  <path d="M300 184 L315 184" stroke="#475569" marker-end="url(#arrov)" fill="none"/>
  <rect x="482" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="492" y="170" font-size="8.5" fill="#6b7280">M9</text>
  <text x="624" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="492" y="190" font-size="11" fill="#111827">出 core 拆包</text>
  <path d="M466 184 L481 184" stroke="#475569" marker-end="url(#arrov)" fill="none"/>
  <text x="20" y="274" font-size="10.5" fill="#6b7280">Reduce</text>
  <rect x="150" y="242" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="256" font-size="8.5" fill="#6b7280">M10</text>
  <text x="292" y="256" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="276" font-size="11" fill="#111827">三路输入仲裁</text>
  <rect x="316" y="242" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="256" font-size="8.5" fill="#6b7280">M11</text>
  <text x="458" y="256" font-size="8.5" fill="#6b7280" text-anchor="end">D2</text>
  <text x="326" y="276" font-size="11" fill="#111827">RMW 累加</text>
  <path d="M300 270 L315 270" stroke="#475569" marker-end="url(#arrov)" fill="none"/>
  <rect x="482" y="242" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="492" y="256" font-size="8.5" fill="#6b7280">M12</text>
  <text x="624" y="256" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="492" y="276" font-size="11" fill="#111827">输出准入与发送</text>
  <path d="M466 270 L481 270" stroke="#475569" marker-end="url(#arrov)" fill="none"/>
  <text x="20" y="360" font-size="10.5" fill="#6b7280">溢流重发</text>
  <rect x="150" y="328" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="160" y="342" font-size="8.5" fill="#92400e">M13</text>
  <text x="292" y="342" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="160" y="362" font-size="11" fill="#7c2d12">进 core 暂存</text>
  <rect x="316" y="328" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="326" y="342" font-size="8.5" fill="#92400e">M14</text>
  <text x="458" y="342" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="326" y="362" font-size="11" fill="#7c2d12">取出重发</text>
  <path d="M300 356 L315 356" stroke="#475569" marker-end="url(#arrov)" fill="none"/>
  <text x="20" y="446" font-size="10.5" fill="#6b7280">控制</text>
  <rect x="150" y="414" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="428" font-size="8.5" fill="#6b7280">M15</text>
  <text x="292" y="428" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="448" font-size="11" fill="#111827">监听队列</text>
  <rect x="316" y="414" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="428" font-size="8.5" fill="#6b7280">M16</text>
  <text x="458" y="428" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="448" font-size="11" fill="#111827">Retire 广播</text>
  <path d="M300 442 L315 442" stroke="#475569" marker-end="url(#arrov)" fill="none"/>
  <rect x="482" y="414" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="492" y="428" font-size="8.5" fill="#6b7280">M17</text>
  <text x="624" y="428" font-size="8.5" fill="#6b7280" text-anchor="end">D5</text>
  <text x="492" y="448" font-size="11" fill="#111827">RouterTable 提交</text>
  <path d="M466 442 L481 442" stroke="#475569" marker-end="url(#arrov)" fill="none"/>
  <text x="20" y="524" font-size="10.5" fill="#374151">M3 的准入对多播是全有全无：任一目标方向的资源不足，本拍所有分支都不推进。</text>
  <text x="20" y="552" font-size="10.5" fill="#374151">M4 每拍重新仲裁，不跨拍锁定；M5 起进 core 与进 ReduceModule 的包锁定到尾 flit，R2R 方向仍可在 flit 边界换包。</text>
  <text x="20" y="580" font-size="10.5" fill="#374151">M13 / M14 的拍数由 Core Mem 的访问延迟与该 VC 前面还压着几个未重发的包决定，不计入单跳延迟。</text>
  <text x="20" y="608" font-size="10.5" fill="#374151">VC credit 分 private 与 shared 两级，与下游 VC Buffer 的占用规则一一对应，一个方向的总量 28 等于下游容量，因此链路上没有 ready。</text>
</svg>
```

## 6　逐级行为

### M1 · Input VC Buffer

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 877 226" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arr1" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="877" height="226" fill="#ffffff"/>

  <polygon points="30,20 188,20 178,96 20,96" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="39" font-size="10.5" fill="#374151" text-anchor="middle">link[d]</text>
  <text x="104" y="57" font-size="9.5" fill="#6b7280" text-anchor="middle">flit_valid · flit_vc[1:0]</text>
  <text x="104" y="75" font-size="9.5" fill="#6b7280" text-anchor="middle">flit_head · flit_tail</text>
  <text x="104" y="93" font-size="9.5" fill="#6b7280" text-anchor="middle">flit_bytes[8:0] · flit_msg</text>
  <rect x="20" y="108" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="112" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="129" font-size="10" fill="#374151" text-anchor="middle">vc_buf[d][v] · FIFO 2 flit · 1W1R</text>
  <rect x="20" y="162" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="166" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="183" font-size="10" fill="#374151" text-anchor="middle">vc_shared[d] · FIFO 20 flit · 1W1R</text>
  <rect x="681" y="56" width="176" height="114" fill="#f1f5f9" stroke="#334155"/>
  <rect x="681" y="56" width="176" height="18" fill="#334155"/>
  <text x="769" y="69" font-size="10.5" fill="#ffffff" text-anchor="middle">IN_RC</text>
  <text x="769" y="96" font-size="10" fill="#334155" text-anchor="middle">vc[1:0]</text>
  <text x="769" y="118" font-size="10" fill="#334155" text-anchor="middle">path_id[7:0]</text>
  <text x="769" y="140" font-size="10" fill="#334155" text-anchor="middle">user_id[15:0]</text>
  <text x="769" y="162" font-size="10" fill="#334155" text-anchor="middle">head · tail</text>
  <rect x="232" y="34" width="405" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="50" font-size="8.5" fill="#6b7280">M1</text>
  <text x="623" y="50" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="70" font-size="12" fill="#111827">Input VC Buffer · 收 flit 入队</text>
  <text x="250" y="92" font-size="10.5" fill="#475569">1. vc = link[d].flit_vc；head = link[d].flit_head</text>
  <text x="250" y="112" font-size="10.5" fill="#475569">2. 该 VC 的 private 未满就占 private，满了就占本方向的 shared pool</text>
  <text x="250" y="132" font-size="10.5" fill="#475569">3. head ? hdr_field = Parse(flit_msg) : 追加到该 VC 队尾</text>
  <text x="250" y="152" font-size="10.5" fill="#475569">4. 上游按同一条规则记 credit，因此这里不会出现两边都满还收到 flit</text>
  <text x="250" y="176" font-size="10" fill="#9ca3af">private 20 flit 只归本 VC，shared 先到先得</text>
  <path d="M188 58 L231 58" stroke="#475569" marker-end="url(#arr1)" fill="none"/>
  <path d="M188 129 L231 129" stroke="#475569" marker-end="url(#arr1)" fill="none"/>
  <path d="M188 183 L231 183" stroke="#475569" marker-end="url(#arr1)" fill="none"/>
  <path d="M637 113 L680 113" stroke="#475569" marker-end="url(#arr1)" fill="none"/>
</svg>
```

### M2 · RC 查表

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 945 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arr2" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="945" height="198" fill="#ffffff"/>

  <rect x="20" y="26" width="168" height="92" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="26" width="168" height="18" fill="#334155"/>
  <text x="104" y="39" font-size="10.5" fill="#ffffff" text-anchor="middle">IN_RC</text>
  <text x="104" y="66" font-size="10" fill="#334155" text-anchor="middle">vc[1:0]</text>
  <text x="104" y="88" font-size="10" fill="#334155" text-anchor="middle">path_id[7:0]</text>
  <text x="104" y="110" font-size="10" fill="#334155" text-anchor="middle">user_id[15:0]</text>
  <rect x="20" y="130" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="134" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="151" font-size="10" fill="#374151" text-anchor="middle">rtab[k] · FF 64 项 · 1R</text>
  <rect x="749" y="42" width="176" height="114" fill="#f1f5f9" stroke="#334155"/>
  <rect x="749" y="42" width="176" height="18" fill="#334155"/>
  <text x="837" y="55" font-size="10.5" fill="#ffffff" text-anchor="middle">RC_VA</text>
  <text x="837" y="82" font-size="10" fill="#334155" text-anchor="middle">out_mask[6:0]</text>
  <text x="837" y="104" font-size="10" fill="#334155" text-anchor="middle">nxt_vc[6:0][1:0]</text>
  <text x="837" y="126" font-size="10" fill="#334155" text-anchor="middle">need_stream · enter_core</text>
  <text x="837" y="148" font-size="10" fill="#334155" text-anchor="middle">operation[1:0] · stall_way</text>
  <rect x="232" y="20" width="473" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M2</text>
  <text x="691" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">RC · 按 path_id 定去向</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. e = rtab[path_id]</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. out_mask = e.flow_dir；nxt_vc = e.nxt_vc</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. need_stream = e.stream_table_enable；enter_core = e.path_core_mask_enable ? core_mask[e.path_core_mask_idx] : !e.path_core_bypass</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. pkt_ctx[d][vc] = {nxt_vc, out_mask, enter_core, e.operation, e.stall_way, remain_len}</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">坏核只置转发位，need_stream 与 enter_core 恒为 0</text>
  <path d="M188 72 L231 72" stroke="#475569" marker-end="url(#arr2)" fill="none"/>
  <path d="M188 151 L231 151" stroke="#475569" marker-end="url(#arr2)" fill="none"/>
  <path d="M705 99 L748 99" stroke="#475569" marker-end="url(#arr2)" fill="none"/>
</svg>
```

### M3 · VA 拿资源

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 934 294" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arr3" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="934" height="294" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="92" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">RC_VA</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">out_mask[6:0]</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">nxt_vc[6:0][1:0]</text>
  <text x="104" y="104" font-size="10" fill="#334155" text-anchor="middle">need_stream[6:0]</text>
  <rect x="20" y="124" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="128" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="145" font-size="10" fill="#374151" text-anchor="middle">vc_credit[d][v] · FF private · 1RW</text>
  <rect x="20" y="178" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="182" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="199" font-size="10" fill="#374151" text-anchor="middle">vc_shared_cr[d] · FF shared · 1RW</text>
  <rect x="20" y="232" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="236" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="253" font-size="10" fill="#374151" text-anchor="middle">stream_tab[d] · FF 16 项 · 1RW</text>
  <rect x="738" y="101" width="176" height="92" fill="#f1f5f9" stroke="#334155"/>
  <rect x="738" y="101" width="176" height="18" fill="#334155"/>
  <text x="826" y="114" font-size="10.5" fill="#ffffff" text-anchor="middle">VA_SA</text>
  <text x="826" y="141" font-size="10" fill="#334155" text-anchor="middle">grant</text>
  <text x="826" y="163" font-size="10" fill="#334155" text-anchor="middle">out_mask[6:0]</text>
  <text x="826" y="185" font-size="10" fill="#334155" text-anchor="middle">nxt_vc[6:0][1:0]</text>
  <rect x="232" y="68" width="462" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="84" font-size="8.5" fill="#6b7280">M3</text>
  <text x="680" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="104" font-size="12" fill="#111827">VA · 三类 credit 一起判</text>
  <text x="250" y="126" font-size="10.5" fill="#475569">1. vc_ok = ∀o∈out_mask: private[o][nxt_vc[o]] &gt; 0 || shared_cr[o] &gt; 0</text>
  <text x="250" y="146" font-size="10.5" fill="#475569">2. st_ok = ∀o∈need_stream: stream_tab[o] 命中 user_id 或有空项</text>
  <text x="250" y="166" font-size="10.5" fill="#475569">3. rd_ok = operation≠Reduce || rdc_down_credit[user_id][o] ≥ 整包 flit 数</text>
  <text x="250" y="186" font-size="10.5" fill="#475569">4. grant = vc_ok &amp;&amp; st_ok &amp;&amp; rd_ok；!grant → stall_way ? 走 M13 : 留在本 VC</text>
  <text x="250" y="210" font-size="10" fill="#9ca3af">多播全有全无：grant 为假时所有分支一起等</text>
  <line x1="188" y1="66" x2="228" y2="66" stroke="#475569" marker-end="url(#arr3)"/>
  <path d="M188 145 L231 145" stroke="#475569" marker-end="url(#arr3)" fill="none"/>
  <path d="M188 199 L231 199" stroke="#475569" marker-end="url(#arr3)" fill="none"/>
  <line x1="188" y1="253" x2="228" y2="253" stroke="#475569" marker-end="url(#arr3)"/>
  <path d="M694 147 L737 147" stroke="#475569" marker-end="url(#arr3)" fill="none"/>
</svg>
```

### M4 · SA 抢通路

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 880 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arr4" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="880" height="198" fill="#ffffff"/>

  <rect x="20" y="37" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="37" width="168" height="18" fill="#334155"/>
  <text x="104" y="50" font-size="10.5" fill="#ffffff" text-anchor="middle">VA_SA</text>
  <text x="104" y="77" font-size="10" fill="#334155" text-anchor="middle">grant</text>
  <text x="104" y="99" font-size="10" fill="#334155" text-anchor="middle">out_mask[6:0]</text>
  <rect x="20" y="119" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="123" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="140" font-size="10" fill="#374151" text-anchor="middle">rr_ptr[o] · FF 7 项 · 1RW</text>
  <rect x="684" y="64" width="176" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="684" y="64" width="176" height="18" fill="#334155"/>
  <text x="772" y="77" font-size="10.5" fill="#ffffff" text-anchor="middle">SA_ST</text>
  <text x="772" y="104" font-size="10" fill="#334155" text-anchor="middle">win[6:0][2:0]</text>
  <text x="772" y="126" font-size="10" fill="#334155" text-anchor="middle">mux_sel[6:0]</text>
  <rect x="232" y="20" width="408" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M4</text>
  <text x="626" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">SA · 每个 output 独立仲裁</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. req[o] = {i | grant[i] &amp;&amp; o∈out_mask[i]}</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. pri = {接口 priority, 有整包, 是上一包的 body, RoundRobin 指针}</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. win[o] = ArgMax(req[o], pri)</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. rr_ptr[o] = win[o] + 1</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">每拍重新仲裁，不跨拍锁定</text>
  <path d="M188 72 L231 72" stroke="#475569" marker-end="url(#arr4)" fill="none"/>
  <path d="M188 140 L231 140" stroke="#475569" marker-end="url(#arr4)" fill="none"/>
  <path d="M640 99 L683 99" stroke="#475569" marker-end="url(#arr4)" fill="none"/>
</svg>
```

### M5 · ST 交换

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 898 272" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arr5" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="898" height="272" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">SA_ST</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">win[6:0][2:0]</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">mux_sel[6:0]</text>
  <rect x="20" y="102" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="106" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="123" font-size="10" fill="#374151" text-anchor="middle">vc_buf[d][v] · FIFO · 1R</text>
  <rect x="20" y="156" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="160" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="177" font-size="10" fill="#374151" text-anchor="middle">vc_credit[d][v] · FF private · 1RW</text>
  <rect x="20" y="210" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="214" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="231" font-size="10" fill="#374151" text-anchor="middle">vc_shared_cr[d] · FF shared · 1RW</text>
  <rect x="702" y="101" width="176" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="702" y="101" width="176" height="18" fill="#334155"/>
  <text x="790" y="114" font-size="10.5" fill="#ffffff" text-anchor="middle">ST_OUT</text>
  <text x="790" y="141" font-size="10" fill="#334155" text-anchor="middle">out_flit[6:0]</text>
  <text x="790" y="163" font-size="10" fill="#334155" text-anchor="middle">locked</text>
  <rect x="232" y="57" width="426" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="73" font-size="8.5" fill="#6b7280">M5</text>
  <text x="644" y="73" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="93" font-size="12" fill="#111827">ST · Crossbar 复制并扣 credit</text>
  <text x="250" y="115" font-size="10.5" fill="#475569">1. out_flit[o] = vc_buf[win[o]].pop()（多播时同一 flit 送多个 o）</text>
  <text x="250" y="135" font-size="10.5" fill="#475569">2. ∀o: private[o][nxt_vc[o]] &gt; 0 ? private −= 1 : shared_cr[o] −= 1</text>
  <text x="250" y="155" font-size="10.5" fill="#475569">3. 首次占用方向时 stream_tab[o] 写入 user_id</text>
  <text x="250" y="175" font-size="10.5" fill="#475569">4. tail ? 解锁 pkt_ctx : locked = 1</text>
  <text x="250" y="199" font-size="10" fill="#9ca3af">进 core 与进 ReduceModule 的包锁定到尾 flit</text>
  <line x1="188" y1="55" x2="228" y2="55" stroke="#475569" marker-end="url(#arr5)"/>
  <path d="M188 123 L231 123" stroke="#475569" marker-end="url(#arr5)" fill="none"/>
  <path d="M188 177 L231 177" stroke="#475569" marker-end="url(#arr5)" fill="none"/>
  <line x1="188" y1="231" x2="228" y2="231" stroke="#475569" marker-end="url(#arr5)"/>
  <path d="M658 136 L701 136" stroke="#475569" marker-end="url(#arr5)" fill="none"/>
</svg>
```

### M6 · Output Pipe

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 950 326" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arr6" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="950" height="326" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">ST_OUT</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">out_flit[6:0]</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">locked</text>
  <rect x="20" y="102" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="106" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="123" font-size="10" fill="#374151" text-anchor="middle">out_buf · FIFO 32 flit · 1W1R</text>
  <rect x="20" y="156" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="160" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="177" font-size="10" fill="#374151" text-anchor="middle">vc_credit[d][v] · FF private · 1RW</text>
  <rect x="20" y="210" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="214" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="231" font-size="10" fill="#374151" text-anchor="middle">vc_shared_cr[d] · FF shared · 1RW</text>
  <rect x="20" y="264" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="268" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="285" font-size="10" fill="#374151" text-anchor="middle">credit_bypass · FF · 1R</text>
  <polygon points="764,124 930,124 920,200 754,200" fill="#f8fafc" stroke="#374151"/>
  <text x="842" y="143" font-size="10.5" fill="#374151" text-anchor="middle">link[o]</text>
  <text x="842" y="161" font-size="9.5" fill="#6b7280" text-anchor="middle">flit_valid · flit_msg</text>
  <text x="842" y="179" font-size="9.5" fill="#6b7280" text-anchor="middle">stream_release_vld</text>
  <text x="842" y="197" font-size="9.5" fill="#6b7280" text-anchor="middle">reduce_release_vld</text>
  <rect x="232" y="84" width="478" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="100" font-size="8.5" fill="#6b7280">M6</text>
  <text x="696" y="100" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="120" font-size="12" fill="#111827">Output Pipe · 拼接后发出</text>
  <text x="250" y="142" font-size="10.5" fill="#475569">1. out_buf[o].push(out_flit[o])</text>
  <text x="250" y="162" font-size="10.5" fill="#475569">2. link[o].flit_* = Shift(out_buf[o], 总线宽度 256 B)</text>
  <text x="250" y="182" font-size="10.5" fill="#475569">3. credit_return_vld → private[d][vc] &lt; 2 ? private += 1 : shared_cr[d] += 1</text>
  <text x="250" y="202" font-size="10.5" fill="#475569">4. release 按 credit_bypass[in_port].out_mask 复制转发，不进仲裁</text>
  <text x="250" y="226" font-size="10" fill="#9ca3af">一个 input port 一拍最多归还一个 VC 的 credit</text>
  <line x1="188" y1="55" x2="228" y2="55" stroke="#475569" marker-end="url(#arr6)"/>
  <path d="M188 123 L231 123" stroke="#475569" marker-end="url(#arr6)" fill="none"/>
  <path d="M188 177 L231 177" stroke="#475569" marker-end="url(#arr6)" fill="none"/>
  <path d="M188 231 L231 231" stroke="#475569" marker-end="url(#arr6)" fill="none"/>
  <line x1="188" y1="285" x2="228" y2="285" stroke="#475569" marker-end="url(#arr6)"/>
  <path d="M710 162 L758 162" stroke="#475569" marker-end="url(#arr6)" fill="none"/>
</svg>
```

### M7 · 进 core 准入

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 958 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arr7" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="958" height="198" fill="#ffffff"/>

  <rect x="20" y="48" width="168" height="48" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="48" width="168" height="18" fill="#334155"/>
  <text x="104" y="61" font-size="10.5" fill="#ffffff" text-anchor="middle">ST_OUT</text>
  <text x="104" y="88" font-size="10" fill="#334155" text-anchor="middle">out_flit[core]</text>
  <rect x="20" y="108" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="112" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="129" font-size="10" fill="#374151" text-anchor="middle">stream_tab[core] · FF 16 项 · 1RW</text>
  <rect x="762" y="51" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="766" y="55" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="850" y="72" font-size="10" fill="#374151" text-anchor="middle">hdr_fifo · FIFO 16 × 256 B · 1W</text>
  <rect x="762" y="105" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="766" y="109" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="850" y="126" font-size="10" fill="#374151" text-anchor="middle">out_buf · FIFO 32 flit · 1W</text>
  <rect x="232" y="20" width="486" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M7</text>
  <text x="704" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">CoreStation · 三态准入并写入 FIFO</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. hit = stream_tab[core] 命中 user_id</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. free = stream_tab[core] 有空项</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. admit = hit || free；free &amp;&amp; !hit → stream_tab[core] 写入 user_id</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. admit ? {hdr_fifo.push(header); out_buf.push(payload)} : 该 VC 不向 Core 发</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">进 core 不再查 VC credit，Stream 已保证有 Core Mem 空间</text>
  <path d="M188 72 L231 72" stroke="#475569" marker-end="url(#arr7)" fill="none"/>
  <path d="M188 129 L231 129" stroke="#475569" marker-end="url(#arr7)" fill="none"/>
  <path d="M718 72 L761 72" stroke="#475569" marker-end="url(#arr7)" fill="none"/>
  <path d="M718 126 L761 126" stroke="#475569" marker-end="url(#arr7)" fill="none"/>
</svg>
```

### M8 · 收满通知 TS

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 903 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arr8" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="903" height="198" fill="#ffffff"/>

  <rect x="20" y="48" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="52" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="69" font-size="10" fill="#374151" text-anchor="middle">hdr_fifo · FIFO 16 项 · 1R</text>
  <rect x="20" y="102" width="168" height="48" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="102" width="168" height="18" fill="#334155"/>
  <text x="104" y="115" font-size="10.5" fill="#ffffff" text-anchor="middle">RECV_CNT</text>
  <text x="104" y="142" font-size="10" fill="#334155" text-anchor="middle">recv_bytes[15:0]</text>
  <polygon points="717,60 883,60 873,136 707,136" fill="#f8fafc" stroke="#374151"/>
  <text x="795" y="79" font-size="10.5" fill="#374151" text-anchor="middle">router2ts_trigger_ch</text>
  <text x="795" y="97" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · user_id[15:0]</text>
  <text x="795" y="115" font-size="9.5" fill="#6b7280" text-anchor="middle">path_id[7:0] · reissue</text>
  <text x="795" y="133" font-size="9.5" fill="#6b7280" text-anchor="middle">compute · ready</text>
  <rect x="232" y="20" width="431" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M8</text>
  <text x="649" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">CoreStation · 判收完并 trigger TS</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. recv_bytes += flit_bytes；done = (recv_bytes == hdr.payload_size)</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. done → trigger = {user_id, path_id, reissue, compute}</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. router2ts_trigger_ch.valid = trigger 有效</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. !ready → 保持 trigger 与 hdr_fifo 队头，不发下一笔</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">trigger 与 token 一一对应，只许反压不许丢</text>
  <path d="M188 69 L231 69" stroke="#475569" marker-end="url(#arr8)" fill="none"/>
  <path d="M188 126 L231 126" stroke="#475569" marker-end="url(#arr8)" fill="none"/>
  <path d="M663 98 L711 98" stroke="#475569" marker-end="url(#arr8)" fill="none"/>
</svg>
```

### M9 · 出 core 拆包

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 856 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arr9" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="856" height="198" fill="#ffffff"/>

  <polygon points="30,60 188,60 178,136 20,136" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="79" font-size="10.5" fill="#374151" text-anchor="middle">out_core_data_ch</text>
  <text x="104" y="97" font-size="9.5" fill="#6b7280" text-anchor="middle">tvalid · tdata[2047:0]</text>
  <text x="104" y="115" font-size="9.5" fill="#6b7280" text-anchor="middle">tkeep · tlast · thdr</text>
  <text x="104" y="133" font-size="9.5" fill="#6b7280" text-anchor="middle">vc_id[1:0] · tready</text>
  <rect x="660" y="78" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="664" y="82" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="748" y="99" font-size="10" fill="#374151" text-anchor="middle">vc_buf[core][v] · FIFO · 1W</text>
  <rect x="232" y="20" width="384" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M9</text>
  <text x="602" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">CoreStation · 拆 Header 与 Payload 进 Core 方向 VC</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. tready = vc_buf[core][vc_id].free &gt; 0</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. tvalid &amp;&amp; tready → {hdr, payload} = Split(tdata, thdr)</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. vc_buf[core][vc_id].push(hdr 首 flit 加后续 payload flit)</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. 之后与其他方向一样走 M2 到 M6</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">进 core 与出 core 不共享仲裁状态，两条通路并行</text>
  <path d="M188 98 L231 98" stroke="#475569" marker-end="url(#arr9)" fill="none"/>
  <path d="M616 99 L659 99" stroke="#475569" marker-end="url(#arr9)" fill="none"/>
</svg>
```

### M10 · 三路输入仲裁

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 926 216" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arra" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="926" height="216" fill="#ffffff"/>

  <polygon points="30,24 188,24 178,82 20,82" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="43" font-size="10.5" fill="#374151" text-anchor="middle">reduce_i</text>
  <text x="104" y="61" font-size="9.5" fill="#6b7280" text-anchor="middle">flit_valid[2:0]</text>
  <text x="104" y="79" font-size="9.5" fill="#6b7280" text-anchor="middle">flit_msg · path_id[7:0]</text>
  <rect x="20" y="94" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="98" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="115" font-size="10" fill="#374151" text-anchor="middle">rdc_rtab · FF 64 项 · 1R</text>
  <rect x="20" y="148" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="152" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="169" font-size="10" fill="#374151" text-anchor="middle">rdc_user_tab · FF 16 项 · 1RW</text>
  <rect x="730" y="51" width="176" height="114" fill="#f1f5f9" stroke="#334155"/>
  <rect x="730" y="51" width="176" height="18" fill="#334155"/>
  <text x="818" y="64" font-size="10.5" fill="#ffffff" text-anchor="middle">RDC_IN</text>
  <text x="818" y="91" font-size="10" fill="#334155" text-anchor="middle">src[1:0]</text>
  <text x="818" y="113" font-size="10" fill="#334155" text-anchor="middle">user_id[15:0]</text>
  <text x="818" y="135" font-size="10" fill="#334155" text-anchor="middle">offset[13:0]</text>
  <text x="818" y="157" font-size="10" fill="#334155" text-anchor="middle">first_in · expect_mask[2:0]</text>
  <rect x="232" y="20" width="454" height="176" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M10</text>
  <text x="672" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">ReduceModule · 选一路并锁定到尾 flit</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. req = {i∈{reduce_0,1,2} | flit_valid[i]}</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. win = locked ? cur_src : RoundRobin(req)</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. bank_ok = rdc_ctx 的目标 bank 与计算单元空闲；!bank_ok → 反压该输入</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. first_in → expect_mask = rdc_rtab[path_id].reduce_in_mask 一并记进上下文</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">locked = !tail，进入后锁定当前包直至尾 flit</text>
  <text x="250" y="180" font-size="10" fill="#9ca3af">必须执行 Reduce，不允许绕过降级为直接转发</text>
  <path d="M188 53 L231 53" stroke="#475569" marker-end="url(#arra)" fill="none"/>
  <path d="M188 115 L231 115" stroke="#475569" marker-end="url(#arra)" fill="none"/>
  <path d="M188 169 L231 169" stroke="#475569" marker-end="url(#arra)" fill="none"/>
  <path d="M686 108 L729 108" stroke="#475569" marker-end="url(#arra)" fill="none"/>
</svg>
```

### M11 · RMW 累加

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 850 218" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arrb" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="850" height="218" fill="#ffffff"/>

  <rect x="20" y="25" width="168" height="114" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="25" width="168" height="18" fill="#334155"/>
  <text x="104" y="38" font-size="10.5" fill="#ffffff" text-anchor="middle">RDC_IN</text>
  <text x="104" y="65" font-size="10" fill="#334155" text-anchor="middle">src[1:0]</text>
  <text x="104" y="87" font-size="10" fill="#334155" text-anchor="middle">user_id[15:0]</text>
  <text x="104" y="109" font-size="10" fill="#334155" text-anchor="middle">offset[13:0]</text>
  <text x="104" y="131" font-size="10" fill="#334155" text-anchor="middle">first_in · expect_mask[2:0]</text>
  <rect x="20" y="151" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="155" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="172" font-size="10" fill="#374151" text-anchor="middle">rdc_ctx · SRAM 16×16 KiB · 1R1W</text>
  <rect x="654" y="47" width="176" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="654" y="47" width="176" height="18" fill="#334155"/>
  <text x="742" y="60" font-size="10.5" fill="#ffffff" text-anchor="middle">RDC_OUT</text>
  <text x="742" y="87" font-size="10" fill="#334155" text-anchor="middle">all_in</text>
  <text x="742" y="109" font-size="10" fill="#334155" text-anchor="middle">user_id[15:0]</text>
  <rect x="654" y="129" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="658" y="133" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="742" y="150" font-size="10" fill="#374151" text-anchor="middle">rdc_ctx · SRAM · 1W</text>
  <rect x="232" y="20" width="378" height="178" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M11</text>
  <text x="596" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D2</text>
  <text x="250" y="56" font-size="12" fill="#111827">ReduceModule · 原位读改写</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. a = (dtype==BF16) ? BF16toFP32(flit_msg) : flit_msg</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. first_in ? rdc_ctx[user][offset] = a</text>
  <text x="262" y="118" font-size="10.5" fill="#475569">: rdc_ctx[user][offset] = rdc_ctx[user][offset] + a（FP32）</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">3. in_done_mask |= 1&lt;&lt;src</text>
  <text x="250" y="158" font-size="10.5" fill="#475569">4. all_in = (in_done_mask == expect_mask)</text>
  <text x="250" y="182" font-size="10" fill="#9ca3af">同一 User 的下一个包在 all_in 输出前不得覆盖该上下文</text>
  <path d="M188 82 L231 82" stroke="#475569" marker-end="url(#arrb)" fill="none"/>
  <path d="M188 172 L231 172" stroke="#475569" marker-end="url(#arrb)" fill="none"/>
  <path d="M610 82 L653 82" stroke="#475569" marker-end="url(#arrb)" fill="none"/>
  <path d="M610 150 L653 150" stroke="#475569" marker-end="url(#arrb)" fill="none"/>
</svg>
```

### M12 · 输出准入与发送

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 950 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arrc" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="950" height="198" fill="#ffffff"/>

  <rect x="20" y="37" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="37" width="168" height="18" fill="#334155"/>
  <text x="104" y="50" font-size="10.5" fill="#ffffff" text-anchor="middle">RDC_OUT</text>
  <text x="104" y="77" font-size="10" fill="#334155" text-anchor="middle">all_in</text>
  <text x="104" y="99" font-size="10" fill="#334155" text-anchor="middle">user_id[15:0]</text>
  <rect x="20" y="119" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="123" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="140" font-size="10" fill="#374151" text-anchor="middle">rdc_down_credit · FF 16×3 · 1RW</text>
  <rect x="754" y="51" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="758" y="55" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="842" y="72" font-size="10" fill="#374151" text-anchor="middle">rdc_out_q · FIFO 8 flit · 1W</text>
  <polygon points="764,105 930,105 920,145 754,145" fill="#f8fafc" stroke="#374151"/>
  <text x="842" y="124" font-size="10.5" fill="#374151" text-anchor="middle">rmem2ts_done_ch</text>
  <text x="842" y="142" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · user_id[15:0]</text>
  <rect x="232" y="20" width="478" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M12</text>
  <text x="696" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">ReduceModule · 转精度后回注 Xbar</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. out = (reduce_outdata_type==BF16) ? FP32toBF16(rdc_ctx[user]) : rdc_ctx[user]</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. send_ok = vc_credit[o][nxt_vc] &gt; 0 &amp;&amp; rdc_down_credit[user][o] &gt; 0</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. send_ok → {rdc_out_q.push(out); rdc_down_credit[user][o] −= 1}</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. 整包发完 → rmem2ts_done_ch.valid = 1, user_id = user</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">输出队列满时停止 RMW 输出，回压到 M10</text>
  <path d="M188 72 L231 72" stroke="#475569" marker-end="url(#arrc)" fill="none"/>
  <path d="M188 140 L231 140" stroke="#475569" marker-end="url(#arrc)" fill="none"/>
  <path d="M710 72 L753 72" stroke="#475569" marker-end="url(#arrc)" fill="none"/>
  <path d="M710 125 L758 125" stroke="#475569" marker-end="url(#arrc)" fill="none"/>
</svg>
```

### M13 · 进 core 暂存

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 894 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arrd" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="894" height="198" fill="#ffffff"/>

  <rect x="20" y="37" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="37" width="168" height="18" fill="#334155"/>
  <text x="104" y="50" font-size="10.5" fill="#ffffff" text-anchor="middle">VA_SA</text>
  <text x="104" y="77" font-size="10" fill="#334155" text-anchor="middle">grant=0</text>
  <text x="104" y="99" font-size="10" fill="#334155" text-anchor="middle">stall_way=转存</text>
  <rect x="20" y="119" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="123" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="140" font-size="10" fill="#374151" text-anchor="middle">reissue_tab · FF 每 VC 1 项 · 1RW</text>
  <polygon points="708,60 874,60 864,136 698,136" fill="#f8fafc" stroke="#374151"/>
  <text x="786" y="79" font-size="10.5" fill="#374151" text-anchor="middle">cmem_reissue</text>
  <text x="786" y="97" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · req_we</text>
  <text x="786" y="115" font-size="9.5" fill="#6b7280" text-anchor="middle">req_addr[17:0]</text>
  <text x="786" y="133" font-size="9.5" fill="#6b7280" text-anchor="middle">req_wdata[2047:0]</text>
  <rect x="232" y="20" width="422" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M13</text>
  <text x="640" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="250" y="56" font-size="12" fill="#111827">CoreMem 重发 · 整包落 Core Mem</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. 进入条件：M3 的 grant 为假且 rtab.stall_way = 转存</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. reissue_tab[vc].pending_reinject &lt; 上限 ? 收 : 改按留在 VC 等处理</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. cmem_reissue.req = {we=1, addr=预留区基址+槽号, wdata=整包}</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. overflow_reinject = 1；reissue_tab[vc].pending_reinject += 1</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">拍数由 Core Mem 写延迟决定，不计入单跳延迟</text>
  <path d="M188 72 L231 72" stroke="#475569" marker-end="url(#arrd)" fill="none"/>
  <path d="M188 140 L231 140" stroke="#475569" marker-end="url(#arrd)" fill="none"/>
  <path d="M654 98 L702 98" stroke="#475569" marker-end="url(#arrd)" fill="none"/>
</svg>
```

### M14 · 取出重发

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 874 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arre" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="874" height="198" fill="#ffffff"/>

  <rect x="20" y="42" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="46" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="63" font-size="10" fill="#374151" text-anchor="middle">reissue_tab · FF · 1RW</text>
  <polygon points="30,96 188,96 178,154 20,154" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="115" font-size="10.5" fill="#374151" text-anchor="middle">cmem_reissue</text>
  <text x="104" y="133" font-size="9.5" fill="#6b7280" text-anchor="middle">rsp_valid</text>
  <text x="104" y="151" font-size="9.5" fill="#6b7280" text-anchor="middle">rsp_rdata[2047:0]</text>
  <rect x="678" y="53" width="176" height="92" fill="#f1f5f9" stroke="#334155"/>
  <rect x="678" y="53" width="176" height="18" fill="#334155"/>
  <text x="766" y="66" font-size="10.5" fill="#ffffff" text-anchor="middle">IN_RC</text>
  <text x="766" y="93" font-size="10" fill="#334155" text-anchor="middle">vc[1:0]</text>
  <text x="766" y="115" font-size="10" fill="#334155" text-anchor="middle">path_id[7:0]</text>
  <text x="766" y="137" font-size="10" fill="#334155" text-anchor="middle">user_id[15:0]</text>
  <rect x="232" y="20" width="402" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M14</text>
  <text x="620" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="250" y="56" font-size="12" fill="#111827">CoreMem 重发 · 重查表后再进 M2</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. 唤醒条件：CoreMemCreditMonitor 通知该 user 的资源到手</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. cmem_reissue.req = {we=0, addr=槽号}；rsp_rdata → 整包</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. e = rtab[包头.path_id]（不复用暂存前的 VC 与路由）</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. overflow_reinject = 0；reissue_tab[vc].pending_reinject −= 1</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">同 VC 有未重发完的包时后续包不得越过</text>
  <path d="M188 63 L231 63" stroke="#475569" marker-end="url(#arre)" fill="none"/>
  <path d="M188 125 L231 125" stroke="#475569" marker-end="url(#arre)" fill="none"/>
  <path d="M634 99 L677 99" stroke="#475569" marker-end="url(#arre)" fill="none"/>
</svg>
```

### M15 · 监听队列

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 965 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arrf" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="965" height="198" fill="#ffffff"/>

  <polygon points="30,33 188,33 178,109 20,109" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="52" font-size="10.5" fill="#374151" text-anchor="middle">ts2router_req</text>
  <text x="104" y="70" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · user_id[15:0]</text>
  <text x="104" y="88" font-size="9.5" fill="#6b7280" text-anchor="middle">stream_id[3:0] · task_id[5:0]</text>
  <text x="104" y="106" font-size="9.5" fill="#6b7280" text-anchor="middle">path_id[7:0]</text>
  <rect x="20" y="121" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="125" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="142" font-size="10" fill="#374151" text-anchor="middle">monitor_q · FF 16 项全相连 · 1RW</text>
  <polygon points="779,69 945,69 935,127 769,127" fill="#f8fafc" stroke="#374151"/>
  <text x="857" y="88" font-size="10.5" fill="#374151" text-anchor="middle">router2ts_credit_ch</text>
  <text x="857" y="106" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · stream_id[3:0]</text>
  <text x="857" y="124" font-size="9.5" fill="#6b7280" text-anchor="middle">task_id[5:0] · path_id[7:0]</text>
  <rect x="232" y="20" width="493" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M15</text>
  <text x="711" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">CoreMemCreditMonitor · 注册与匹配</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. ts2router_req.req_ready = monitor_q 有空项</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. 注册：monitor_q.push({user_id, stream_id, task_id, need_mask=rtab[path_id]})</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. hit = {q | ∀o∈q.need_mask: 资源已满足}</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. 多个 hit 时按 stream_id 选最老，发 router2ts_credit_ch</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">进 core 重发的任务也注册在这里</text>
  <path d="M188 71 L231 71" stroke="#475569" marker-end="url(#arrf)" fill="none"/>
  <path d="M188 142 L231 142" stroke="#475569" marker-end="url(#arrf)" fill="none"/>
  <path d="M725 98 L773 98" stroke="#475569" marker-end="url(#arrf)" fill="none"/>
</svg>
```

### M16 · Retire 广播

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 884 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arrg" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="884" height="198" fill="#ffffff"/>

  <polygon points="30,42 188,42 178,100 20,100" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="61" font-size="10.5" fill="#374151" text-anchor="middle">ts2router_retire</text>
  <text x="104" y="79" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · user_id[15:0]</text>
  <text x="104" y="97" font-size="9.5" fill="#6b7280" text-anchor="middle">accepted</text>
  <rect x="20" y="112" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="116" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="133" font-size="10" fill="#374151" text-anchor="middle">retire_pend · FF 16 项 · 1RW</text>
  <rect x="688" y="51" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="692" y="55" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="776" y="72" font-size="10" fill="#374151" text-anchor="middle">stream_tab[d] · FF · 1W</text>
  <rect x="688" y="105" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="692" y="109" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="776" y="126" font-size="10" fill="#374151" text-anchor="middle">rdc_user_tab · FF · 1W</text>
  <rect x="232" y="20" width="412" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M16</text>
  <text x="630" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">Retire · 立即删表项，延迟回收上下文</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. ts2router_retire.accepted = retire_pend 有空项</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. 立即：∀d: stream_tab[d] 删除该 user_id 的全部授权</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. 记录 retire_pend[user].to_rdc = 1，不立即删 ReduceModule 映射</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. rdc_down_credit[user][*] 全部回到分配值 → 删 rdc_user_tab[user]</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">Core 保证发 Retire 前该 user 不再有进出 core 的搬运</text>
  <path d="M188 71 L231 71" stroke="#475569" marker-end="url(#arrg)" fill="none"/>
  <path d="M188 133 L231 133" stroke="#475569" marker-end="url(#arrg)" fill="none"/>
  <path d="M644 72 L687 72" stroke="#475569" marker-end="url(#arrg)" fill="none"/>
  <path d="M644 126 L687 126" stroke="#475569" marker-end="url(#arrg)" fill="none"/>
</svg>
```

### M17 · RouterTable 提交

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 862 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arrh" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="862" height="198" fill="#ffffff"/>

  <polygon points="30,42 188,42 178,100 20,100" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="61" font-size="10.5" fill="#374151" text-anchor="middle">cfg</text>
  <text x="104" y="79" font-size="9.5" fill="#6b7280" text-anchor="middle">cfg_valid · cfg_addr[23:0]</text>
  <text x="104" y="97" font-size="9.5" fill="#6b7280" text-anchor="middle">cfg_we · cfg_wdata[31:0]</text>
  <rect x="20" y="112" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="116" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="133" font-size="10" fill="#374151" text-anchor="middle">rtab_commit · FF · 1RW</text>
  <rect x="666" y="51" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="670" y="55" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="754" y="72" font-size="10" fill="#374151" text-anchor="middle">rtab[k] · FF 64 项 × 5 副本 · 1W</text>
  <polygon points="676,105 842,105 832,145 666,145" fill="#f8fafc" stroke="#374151"/>
  <text x="754" y="124" font-size="10.5" fill="#374151" text-anchor="middle">cfg</text>
  <text x="754" y="142" font-size="9.5" fill="#6b7280" text-anchor="middle">commit_done</text>
  <rect x="232" y="20" width="390" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M17</text>
  <text x="608" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D5</text>
  <text x="250" y="56" font-size="12" fill="#111827">RouterTable / CSR · 多副本逐份写</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. cfg_valid &amp;&amp; cfg_we → rtab_commit = {cursor=0, busy=1}</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. 每拍写一份：rtab[cursor][cfg_addr] = cfg_wdata；cursor += 1</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. cursor == 副本数 → {busy=0, commit_done=1}</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. busy 期间不向软件返回完成，禁止暴露部分新部分旧的状态</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">DTE 与 ReduceModule 的两份外部副本由软件在 commit_done 之后再写</text>
  <path d="M188 71 L231 71" stroke="#475569" marker-end="url(#arrh)" fill="none"/>
  <path d="M188 133 L231 133" stroke="#475569" marker-end="url(#arrh)" fill="none"/>
  <path d="M622 72 L665 72" stroke="#475569" marker-end="url(#arrh)" fill="none"/>
  <path d="M622 125 L670 125" stroke="#475569" marker-end="url(#arrh)" fill="none"/>
</svg>
```

***

## 7　参数汇总

```
每方向数据宽度      256 B；相邻 Router 双向各 256 GB/s @1GHz（接口理论值），HAS 记 R2R 有效带宽 210 GB/s、C2C 90 GB/s
进 core 与出 core   各 256 GB/s @1GHz，完全并行
单跳延迟            ≤ 6 cycles（六级流水线），优化后 4～5；坏核走 Skip 直通，延迟与正常跳一致
RouterTable         64 条表项；字段含 flow_dir（出）与 reduce_in_mask（进）两个方向集合；内部副本数与每副本写入拍数 5、1（待定）
VC                  每输入方向 4 类（VC0～3），输出方向不设 VC Buffer
                    VC3 专给逐级 reduce，VC0～2 支持除 reduce 外的操作、软件可配；4 这个数来自“最复杂场景下一个 Router 最多经过 4 条同向数据流”
credit 记账单位      1 KB；广播一次扣的量含提前预留的输出结果空间（原文例：8 KB 广播 + 24 KB 输出 = 扣 32）
VC Buffer           private 每 VC 深度 20（覆盖 RTT，软件可配，防死锁下限 2）加每方向一个 shared pool 20 flit（覆盖 credit 往返），一个方向合计 100 flit ≈ 25 KB
                    private 那 2 flit 任何时候都只归本 VC，shared pool 先到先得。建模按这个结构建，不摊平成每 VC 一个独立深度
                    另一份口径：《通信机制（分析过程）》按容量记 —— reduce 专用 VC3 是 16 KB、三个共享 VC 各 8 KB、三方向各一套，
                    合计 (16 + 8×3) × 3 = 120 KB。两份口径未对齐，见第 8 章
Stream Resource Table  每方向 16 项（待定）
VC credit 初值      private 每 VC 2，shared 每方向 20；发送先扣 private 再扣 shared，归还先补 private 再补 shared
                    一个方向的 credit 总量 4 × 20 + 20 = 100，等于下游该方向的 VC Buffer 容量，不超发，链路上不需要 ready
Reduce 输入 / 输出   三路各 160 GB/s / 160 GB/s；算力 80 GFLOPS（FP32 / BF16）
ReduceModule 上下文  16 用户 × 16 KiB；单个 Token 16 KB 这个下界来自“Core 必须一次性整包发进 ReduceBuffer，不能分段”
ReduceModule Entry credit、bank 数、RMW 拍数、输出队列深度   64 flit、4、2、8（待定）
CoreStation HeaderFIFO / OutputBuffer 深度   16 / 32 flit（待定）
监听事件队列        16 项全相连
Xbar 与 ReduceModule 三路输入的仲裁算法      轮询（待定）
operation 的 Reduce0 / Reduce1 / Reduce2     源分量 / 中继累加 / 最终汇聚（待定，原文未定义）
包结构              path_id 8 bit（有效 6 bit）· path_core_mask 16 bit · user_id 加 task_id 10 bit · vc_id · overflow_reinject 1 bit · reduce_seq 6 bit · 包长 16 bit · 软件 payload 0～16 B · 业务数据 0 B～64 KB
包长范围            最短 16 B（只含包头与路由信息的空包），最长 64 KB、实际支持到 (16K + 32) B；不设包尾，结束靠包长度计数
reduce 包           软件辅助信息固定 16 B，Router 做加法时固定跳过这 16 B
总线                Header 与 Payload 走两根独立并行总线，hflit 256 bit 与 pflit 2048 bit，按同一包边界保持对应；进 core 拼接成完整包，出 core 自动拆分
```

***

## 8　机制覆盖

| 机制 | 功能 | 用例 |
| - | - | - |
| 按 path_id 查表得到全部去向与资源需求 | F1、F53 | `router_lookup` |
| 进 core 由 path_core_mask_enable 二选一，位由 path_core_mask_idx 指定 | F54 | `core_mask_index` |
| reduce_data_type 配在表里，reduce_twice 时 TS 还没介入 | F55 | `reduce_twice_dtype` |
| kernel / weight 搬运包跳过 TS 直接唤醒 DTE | F56 | `kernel_skip_ts` |
| Skip Mask 与 RouterTable 分开配；复位后全条目 bypass / no-op | F57 | `skip_mask_reset` |
| reduce 与 reduce_twice 两档，两个源可都来自本 core 的 DTE | F43 | `reduce_twice` |
| 四个 VC 独立缓存独立计 credit，一个堵住不影响其他 | F2、F10 | `vc_isolation` |
| 单播准入：private 或 shared 有一个大于 0 | F3、F6 | `vc_credit_single` |
| credit 总量等于下游 buffer 容量，不超发 | F9 | `vc_credit_no_overrun` |
| 归还按同一条规则回填，两边计数不漂移 | F11 | `vc_credit_return_rule` |
| 多播全有或全无，任一方向不足则所有分支一起等 | F4、F21 | `multicast_atomic` |
| stall_way 二选一：留在 VC 等 / 转 Core Mem 重发 | F5、F65 | `stall_way` |
| VC credit 走共享总线归还，一拍最多一个 VC | F8 | `vc_credit_return` |
| 只有所有需求方向都满足才允许发送 | F11 | `stream_all_or_none` |
| 同 VC 保序，跨 VC 与跨 port 不保证 | F13 | `same_vc_order` |
| 业务 credit 旁路：不查表、不进仲裁、按静态 Mask 复制 | F14、F63 | `credit_bypass_route` |
| 坏核只透传，credit 跨过它 | F15、F70 | `harvest_skip` |
| Xbar 5 入 7 出，无冲突时五路并行 | F17、F18 | `xbar_parallel` |
| 每 output 独立 RoundRobin，不跨拍锁定 | F19 | `xbar_rr` |
| 贪婪整包的四级优先级 | F20 | `xbar_greedy_packet` |
| 进 core 后锁定到尾 flit，R2R 可在 flit 边界切换包 | F23 | `interleave_grain` |
| 进 core 三态准入 | F24 | `corestation_admit` |
| 进 core 不再查 VC credit | F25 | `no_vc_check_into_core` |
| Core 入口整包，不支持包间交织 | F27 | `no_packet_interleave` |
| CoreStation 按包头顺序直接通知 TS，不经软件 | F28 | `trigger_to_ts` |
| 按已接收字节数与包头 payload 大小判 token 收完 | F29 | `token_complete` |
| compute 位原样转给 TS，Router 不解释 | F30 | `trigger_compute_bit` |
| TS 的 trigger 入口占满时请求原地保持，不丢 trigger | F31 | `trigger_backpressure` |
| DTE RV core 经 cm_lsq 读包头后写 1 弹出，映射出下一个包头 | F32 | `header_pop` |
| 反压时数据保持，解除后从同一 flit 继续 | F33 | `backpressure_hold` |
| 出 core 前置申请：先拿到下游 Stream 或 Reduce 资源才发 | F34、F83 | `out_core_pre_grant` |
| 进 core 与出 core 完全并行 | F35 | `in_out_parallel` |
| ReduceModule 三路输入仲裁，进入后锁定到尾 flit | F36 | `reduce_input_arb` |
| BF16 扩 FP32，中间累加固定 FP32，输出可配 | F37、F41 | `reduce_precision` |
| 首份输入建上下文，后续原位 RMW 累加 | F38 | `reduce_rmw` |
| 收齐判据取自 RouterTable 的 reduce_in_mask，建上下文时记进 expect_mask | F46 | `reduce_in_mask` |
| reduce_in_mask 为 0 的核不累加，只按 flow_dir 转发 | F47 | `reduce_passthrough` |
| 上下文保护：同一 User 的下一个包不得覆盖 | F39 | `reduce_ctx_protect` |
| 必须执行 Reduce，不允许降级 | F40 | `reduce_no_bypass` |
| Reduce 上下文与 Stream 表项一一对应，不会出现上下文耗尽 | F51 | `reduce_ctx_pairing` |
| 输出前查目标 VC credit 与下游 Reduce credit | F42、F47 | `reduce_out_credit` |
| 每发一 flit 扣一个，按 UserID release 恢复 | F46 | `reduce_credit_flit` |
| 只有 Router Reduce Done 才能把 reduce task 置 FINISH | F48 | `reduce_done_owner` |
| 链上没有同步点，流着加 | F52 | `reduce_streaming` |
| RouterTable 内部多副本，全部写完才返回完成 | F60、F61 | `router_table_commit` |
| 三份副本由软件保证一致，硬件不同步外部两份 | F62 | `router_table_three_copies` |
| CoreMem 只存包，重发时按 PathID 重查表 | F66 | `reissue_repath` |
| 暂存空间用尽是配置错误，模型直接断言失败 | F67 | `reissue_space_assert` |
| 同 VC 有未重发完的包时后续包不得越过 | F68、F82 | `reissue_order` |
| overflow_reinject 标记，Output Port 识别到才扣 credit | F69 | `reinject_flag` |
| 完成后向 TS 回 UserID + PathID | F71 | `reissue_done` |
| Retire 的三方时序：Core 保证 → Router 立即删表项 → ReduceModule 延迟回收 | F72～F75 | `retire_three_party` |
| Stream credit 经 core credit crossbar 逐跳传到上游 | F76 | `stream_credit_hop` |
| 监听事件队列 16 项全相连，按 StreamID 选最老通知 TS | F77～F80 | `credit_monitor_q` |
| 进 core 重发的任务也注册到监听队列 | F81 | `reissue_register` |
| Router 与 TS 的 Stream 表按一致逻辑分配，通知的包一定能被接收 | F84 | `stream_tab_consistent` |
| per-port stream_credit 同步给 core 与 DTE | F85 | `stream_credit_sync` |
| VC3 专给逐级 reduce，VC0～2 软件可配 | F86、F87 | `vc_class_split` |
| 三方向各一套独立 VC 与 credit | F88 | `vc_per_direction` |
| 换 VC 靠改写包头，本跳写下一跳读 | F91、F92 | `vc_swap_in_header` |
| 逐级归约按序、只缓最老用户、最多 3 源 | F94 | `reduce_inorder_3src` |
| 非逐级归约必须进 core，支持用户间乱序 | F95 | `reduce_nonlocal_in_core` |
| ReduceBuffer 不得用作流控缓存 | F97 | `redbuf_not_for_flowctrl` |
| path_id 必须换新的两条规则 | F98、F99 | `path_id_alloc_rule` |
| path_core_mask 溢出由软件换 path_id | F100 | `core_mask_overflow` |
| reduce 包软件辅助信息固定 16 B，加法跳过 | F105 | `reduce_hdr_skip_16b` |
| 送 DTE 时不剥离任何数据 | F107 | `no_strip_to_dte` |
| credit 单位 1 KB，广播一次扣含预留的输出空间 | F108、F109 | `credit_unit_1kb` |

### 验收场景

上面那张表逐条查一个机制，下面这些场景各跑一条完整通路，编号沿用原始设计文档的 A 编号。

| 场景 | 跑什么 | 要看到什么 |
| - | - | - |
| A1 | 一个 user 首次进核 | 上游按方向扣 1 个 stream 坑，本核不给自己扣；任务链最后一项完成后 Retire 释放 |
| A2 | 中间核只做 bypass | 不占该核的用户坑，`cur_credit_require` 为 0 |
| A3 | 下游某方向 credit 为 0 | 该方向的包转 Core Mem 暂存，credit 回来后重发 |
| A4 | stream 坑打满 | 第 `stream_num + 1` 个 user 首次占坑失败，在上游等 |
| A4b | 重发闭环 | 下游释放 credit 唤醒上游，最老的 user 先重发，重发后清掉暂存标记，同一个包不会重发两次 |
| A5 | 同一个 user 第二次发送 | 用户坑余额不变：坑按 user 记，不按包记 |
| A5b | 多播的两个方向只有一侧要占坑 | 只向那一侧申请 |
| A5c | 多播的两个方向都要占坑，一侧为 0 | 两个方向都不发，整体转暂存；两侧都够了才一起发 |
| A5d | 多播的两个方向，其中一侧本 user 已持有坑 | 只查没持有的那一侧；两侧都已持有时不查 credit 直接发 |
| A7 | 逐级 reduce | reduce 与进核数据共用同一份 CoreMem credit，只记一次账；分量不额外占坑 |
| A7c | 一条 path 的 reduce 输入方向超过 3 个 | 构造期报错：`reduce_in_mask` 只有三位 |
| A8 | 查表与带宽计量 | 查表拍数与链路占用拍数与参数表一致 |
| A9 | `path_core_mask` 决定进不进核的两个分支 | 该位为 1 时进核，为 0 时只转发 |
| A13 | 跨 chip 的两级 credit | PCIe 入口满时拒绝新的 user 进核；本核任务链走完后归还 PCIe 那一段 |
| A14 | 非逐级归约的分量乱序到达 | 两个分量任意顺序到齐后才推进下一项，不按到达顺序推进 |
| A15 | 单个坏核透传 | 坏核只转发不记账，它的 stream 表全空、不发 trigger、不参与重发；上游记的是坏核下游那个核的坑，下游认的上游是坏核上游那个核；释放经坏核透传回上游 |
| A16 | 多个坏核串联透传 | 逐跳链式透传，两端仍互认为直接上下游，释放沿坏核链逐跳回传，透传时延是逐跳 R2R 累加而不是单跳 |
| A17 | reduce 完成的 Ack 归属 | 累加完成由 Router 发 `reduce_done` 给 TS，DTE 在 reduce 任务下不返 Ack 只释放资源；缺这个 Ack 时任务链停在 reduce 那一项不前进 |

先跑 A2、A5、A15、A16、A17 五个：它们直接覆盖最容易实现错的语义。

***

## 9　取舍

* **为什么把 VC credit 和 stream credit分成两层**
  * 两者管的东西时间尺度差着数量级：VC credit 管下游 Router 的 buffer 槽位，flit 一进一出就归还；stream credit要等那个用户在下游 core 上跑完整条任务链才释放
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
