# DTE DSA

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **DTE DSA**

给实现 DTE 的人：八个逐拍推进的模块各自做哪些事、端口与存储怎么定。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《DTE 数据搬运引擎》全篇
* 《Router 片上交换与归约》：“进 core”“出 core”“Reduce”的 DTE 侧职责
* 《归约的完整过程》：“TS 与 DTE 侧的配合”

***

## 1　定位与边界

DTE 只做搬运，不做计算，职责五件：接纳任务、生成访问命令、处理背压、追踪在途事务、向 TS 报告最终完成。

难点在两端的节奏对不上：Router 一侧流式到达、什么时候来由上游决定；存储一侧要过 DMA_XBAR 抢 bank。做法是把一个搬运任务从中间劈开，读一半、写一半各自排队各自推进，中间用 buffer 顶住速度差，完成时按 `task_id` 合回一次 `task_done`。

支持五种搬运方向：

| Route | 走哪个通道 | 说明 |
| - | - | - |
| Router → MM | 进核通道 `in_ch` | Header Parser 解析后 Commit 成对建立 |
| Router → CM | 进核通道 `in_ch` | 同上 |
| MM → Router | 出核通道 `out_ch[n]`，n 由这条 path 的 VC 定 | 出口是 Router TX |
| CM → Router | 出核通道 `out_ch[n]`，n 由这条 path 的 VC 定 | 出口是 Router TX |
| MM → CM | 固定走 `out_ch[3]` | 出口切到 DMA WR1，硬件 route mask 只允许 CoreMem |
| CM → MM | — | 本版本不支持，XBar 不提供 WR_CH1 到 Matrix Memory 的连接 |

任务从两个入口来，都在 Commit 边界汇成同一套内部任务模型：Router 入站帧的 Header 经 Header Parser 生成 Descriptor；DTE RV core 经寄存器写加 Trigger 生成 Descriptor。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1760 1330" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" role="img" aria-label="DTE DSA 第 0 层">
<title>DTE DSA 第 0 层</title>
<defs><marker id="a" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#475569"/></marker><marker id="as" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#475569"/></marker><marker id="g" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0f766e"/></marker><marker id="gs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0f766e"/></marker><marker id="o" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#b45309"/></marker><marker id="os" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#b45309"/></marker><marker id="p" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#7c3aed"/></marker><marker id="ps" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#7c3aed"/></marker><marker id="i" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#4338ca"/></marker><marker id="is" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#4338ca"/></marker><marker id="t" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0d9488"/></marker><marker id="ts" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0d9488"/></marker><marker id="r" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#be123c"/></marker><marker id="rs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#be123c"/></marker><marker id="b" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#2563eb"/></marker><marker id="bs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#2563eb"/></marker><marker id="m" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#d97706"/></marker><marker id="ms" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#d97706"/></marker><marker id="l" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#9aa1ad"/></marker><marker id="ls" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#9aa1ad"/></marker></defs>
<rect x="0" y="0" width="1760" height="1330" fill="#ffffff"/>
<text x="20" y="26" font-size="12" fill="#111827">DTE DSA · 第 0 层（八个逐拍推进的模块；一个进核通道加四个出核通道。方位：RV core 与 TS 在上，Core Mem / Matrix Mem 经 DTE xbar 在下，Router 在最下）</text>
<text x="1010" y="26" font-size="9.5" fill="#6b7280">一个高层任务被劈成 RD / WR 两个子上下文，各自排队各自推进，完成时按 task_id 合回一次 task_done</text>
<rect x="160" y="110" width="300" height="154.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="172" y="131" font-size="11" fill="#111827" font-weight="600">Completion RS</text>
<text x="172.0" y="148.0" font-size="8.5" fill="#475569">只在同一 task_id 的 RD 与 WR 两侧条件都满足时</text>
<text x="172.0" y="161.5" font-size="8.5" fill="#475569">　产生 task_done（Join）</text>
<text x="172.0" y="175.0" font-size="8.5" fill="#475569">同一拍多个 Join 命中时全部写入 Done Pending，</text>
<text x="172.0" y="188.5" font-size="8.5" fill="#475569">　不允许覆盖或丢失</text>
<text x="172.0" y="202.0" font-size="8.5" fill="#475569">六个完成层级：queued → active → issue_done</text>
<text x="172.0" y="215.5" font-size="8.5" fill="#475569">　→ drained → join_done → task_done</text>
<text x="172.0" y="229.0" font-size="8.5" fill="#475569">issue_done 只表示请求已发出，真正完成还要等</text>
<text x="172.0" y="242.5" font-size="8.5" fill="#475569">　写响应、读响应排空以及 outstanding 清零</text>
<rect x="500" y="110" width="300" height="113.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="512" y="131" font-size="11" fill="#111827" font-weight="600">Done Pending</text>
<text x="512.0" y="148.0" font-size="8.5" fill="#475569">深度 16（待定）</text>
<text x="512.0" y="161.5" font-size="8.5" fill="#475569">负责多个同拍 Join 的串行化</text>
<text x="512.0" y="175.0" font-size="8.5" fill="#475569">向 TS 的报告是 exactly-once</text>
<text x="512.0" y="188.5" font-size="8.5" fill="#475569">task_last 标记的那一笔完成后才通知 TS</text>
<text x="512.0" y="202.0" font-size="8.5" fill="#475569">no_ack 置位的任务不回 Ack</text>
<rect x="840" y="110" width="300" height="154.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="852" y="131" font-size="11" fill="#111827" font-weight="600">TaskQueue ×4</text>
<text x="852.0" y="148.0" font-size="8.5" fill="#475569">每通道每侧各一个，深度不少于 16</text>
<text x="852.0" y="161.5" font-size="8.5" fill="#475569">通道内按序激活：TaskQueue 按序装载为 Active Context</text>
<text x="852.0" y="175.0" font-size="8.5" fill="#475569">read-ahead 允许 RD / WR 任务序号错位，</text>
<text x="852.0" y="188.5" font-size="8.5" fill="#475569">　但不改变通道内的顺序</text>
<text x="852.0" y="202.0" font-size="8.5" fill="#475569">通道之间可乱序：某个 VC 阻塞只堵住对应的</text>
<text x="852.0" y="215.5" font-size="8.5" fill="#475569">　Active Context 释放后就能激活下一个任务，</text>
<text x="852.0" y="229.0" font-size="8.5" fill="#475569">　不等配对的那一条</text>
<text x="852.0" y="242.5" font-size="8.5" fill="#475569">issue_done 就允许该侧提前激活下一任务</text>
<rect x="1180" y="110" width="320" height="181.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1192" y="131" font-size="11" fill="#111827" font-weight="600">Commit（配对接纳）</text>
<text x="1192.0" y="148.0" font-size="8.5" fill="#475569">一个高层任务必须同时拿到三样：</text>
<text x="1192.0" y="161.5" font-size="8.5" fill="#475569">　1. 目标通道读侧的 TaskQueue 项</text>
<text x="1192.0" y="175.0" font-size="8.5" fill="#475569">　2. 同一通道写侧的 TaskQueue 项</text>
<text x="1192.0" y="188.5" font-size="8.5" fill="#475569">　3. Completion RS 项</text>
<text x="1192.0" y="202.0" font-size="8.5" fill="#475569">任一侧没有空间，Commit 整体保持，Header 入口向 Router 反压</text>
<text x="1192.0" y="215.5" font-size="8.5" fill="#475569">这条规则挡住“读已经开始、写还没有落脚点”的半任务</text>
<text x="1192.0" y="229.0" font-size="8.5" fill="#475569">同时完成地址展开：源地址、目的地址、按任务边界切分的元数据</text>
<text x="1192.0" y="242.5" font-size="8.5" fill="#475569">两个配置 Bank，Bank0 优先于 Bank1：</text>
<text x="1192.0" y="256.0" font-size="8.5" fill="#475569">　都空闲时 Router 的配置进 Bank0，RV core 的配置进 Bank1</text>
<text x="1192.0" y="269.5" font-size="8.5" fill="#475569">　只剩一个 Bank 而两者竞争时优先配置 Router 信息</text>
<polygon points="599,44 710,44 701,74 590,74" fill="#f8fafc" stroke="#374151"/>
<text x="650.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">dsa_done → TS</text>
<polygon points="1289,44 1400,44 1391,74 1280,74" fill="#f8fafc" stroke="#374151"/>
<text x="1340.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">dsa_cfg（RV core）</text>
<rect x="560" y="350" width="300" height="113.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="572" y="371" font-size="11" fill="#111827" font-weight="600">RD_CH1（出核读）· 出核通道 ×4</text>
<text x="572.0" y="388.0" font-size="8.5" fill="#475569">AGCU 生成源端读地址</text>
<text x="572.0" y="401.5" font-size="8.5" fill="#475569">Read Ctrl 经 DMA_XBAR 读 MM / CM</text>
<text x="572.0" y="415.0" font-size="8.5" fill="#475569">返回数据连同 task_id 与边界元数据</text>
<text x="572.0" y="428.5" font-size="8.5" fill="#475569">　写入 outbound buffer</text>
<text x="572.0" y="442.0" font-size="8.5" fill="#475569">MM 侧地址直给，CM 侧加 stream 偏移</text>
<rect x="890" y="350" width="180" height="113.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="902" y="371" font-size="11" fill="#111827" font-weight="600">outbound buffer</text>
<text x="902.0" y="388.0" font-size="8.5" fill="#475569">同上，四条出核通道各一份 8 KB</text>
<text x="902.0" y="401.5" font-size="8.5" fill="#475569">出口阻塞只通过 Credit</text>
<text x="902.0" y="415.0" font-size="8.5" fill="#475569">　反压限制领先距离</text>
<text x="902.0" y="428.5" font-size="8.5" fill="#475569">不要求读写用同一个</text>
<text x="902.0" y="442.0" font-size="8.5" fill="#475569">　Active Context</text>
<rect x="1130" y="350" width="300" height="127.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1142" y="371" font-size="11" fill="#111827" font-weight="600">WR_CH1（出核写）· 出核通道 ×4</text>
<text x="1142.0" y="388.0" font-size="8.5" fill="#475569">按固化的 Route 选出口：Router TX 或 CoreMem Egress</text>
<text x="1142.0" y="401.5" font-size="8.5" fill="#475569">Router TX 用 AXI-Stream 的 Valid / Ready / Keep / Last</text>
<text x="1142.0" y="415.0" font-size="8.5" fill="#475569">CoreMem Egress 用 DMA_XBAR 写握手</text>
<text x="1142.0" y="428.5" font-size="8.5" fill="#475569">MM → CM 时出口切到 DMA WR1，硬件 route mask</text>
<text x="1142.0" y="442.0" font-size="8.5" fill="#475569">　只允许 CoreMem，不会把出核通道数据写回 Matrix Mem</text>
<text x="1142.0" y="455.5" font-size="8.5" fill="#475569">两种出口的响应与 Drain 条件统一送进 Completion RS</text>
<rect x="560" y="560" width="300" height="127.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="572" y="581" font-size="11" fill="#111827" font-weight="600">WR_CH0（进核写）· 进核通道 ×1</text>
<text x="572.0" y="598.0" font-size="8.5" fill="#475569">从 inbound buffer 按任务边界取数</text>
<text x="572.0" y="611.5" font-size="8.5" fill="#475569">经 DMA_XBAR 写入目标 Matrix Mem / Core Mem</text>
<text x="572.0" y="625.0" font-size="8.5" fill="#475569">Router → MM：dst_addr 取自包头，不加 stream 偏移</text>
<text x="572.0" y="638.5" font-size="8.5" fill="#475569">Router → CM：dst_addr 取自包头，再叠 stream 偏移</text>
<text x="572.0" y="652.0" font-size="8.5" fill="#475569">　　+ stream_id × stream_stride</text>
<text x="572.0" y="665.5" font-size="8.5" fill="#475569">写请求与响应 Drain 后进 Completion RS</text>
<rect x="890" y="560" width="180" height="140.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="902" y="581" font-size="11" fill="#111827" font-weight="600">inbound buffer</text>
<text x="902.0" y="598.0" font-size="8.5" fill="#475569">进核这条通道一份 8 KB</text>
<text x="902.0" y="611.5" font-size="8.5" fill="#475569">256 B × 32 项</text>
<text x="902.0" y="625.0" font-size="8.5" fill="#475569">最大可掩盖 32 T 延迟</text>
<text x="902.0" y="638.5" font-size="8.5" fill="#475569">read-ahead 的领先量由</text>
<text x="902.0" y="652.0" font-size="8.5" fill="#475569">　Buffer credit、读 outstanding</text>
<text x="902.0" y="665.5" font-size="8.5" fill="#475569">　限额、可保留的任务边界数</text>
<text x="902.0" y="679.0" font-size="8.5" fill="#475569">　共同约束</text>
<rect x="1130" y="560" width="300" height="140.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1142" y="581" font-size="11" fill="#111827" font-weight="600">RD_CH0（进核读）· 进核通道 ×1</text>
<text x="1142.0" y="598.0" font-size="8.5" fill="#475569">从 CoreStation 收帧</text>
<text x="1142.0" y="611.5" font-size="8.5" fill="#475569">Payload 附带 task_id、有效字节与任务边界</text>
<text x="1142.0" y="625.0" font-size="8.5" fill="#475569">写入 inbound buffer</text>
<text x="1142.0" y="638.5" font-size="8.5" fill="#475569">Buffer 满时通过 TREADY 向 Router 反压</text>
<text x="1142.0" y="652.0" font-size="8.5" fill="#475569">TKEEP 按字节粒度生效，每个 Payload Fire</text>
<text x="1142.0" y="665.5" font-size="8.5" fill="#475569">　累计 TKEEP 有效字节，TLAST 时与</text>
<text x="1142.0" y="679.0" font-size="8.5" fill="#475569">　byte_count 比较</text>
<rect x="160" y="960" width="360" height="194.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="172" y="981" font-size="11" fill="#111827" font-weight="600">DMA_XBAR</text>
<text x="172.0" y="998.0" font-size="8.5" fill="#475569">Core Mem 8 bank</text>
<text x="172.0" y="1011.5" font-size="8.5" fill="#475569">Matrix Mem 64 bank</text>
<text x="172.0" y="1025.0" font-size="8.5" fill="#475569">命中冲突就排队</text>
<text x="172.0" y="1038.5" font-size="8.5" fill="#475569">各通道目的资源</text>
<text x="172.0" y="1052.0" font-size="8.5" fill="#475569">　无冲突时独立推进</text>
<text x="172.0" y="1065.5" font-size="8.5" fill="#475569">共享端口时按 XBar</text>
<text x="172.0" y="1079.0" font-size="8.5" fill="#475569">　仲裁规则</text>
<text x="172.0" y="1092.5" font-size="8.5" fill="#475569">Router→CM 与 MM→CM</text>
<text x="172.0" y="1106.0" font-size="8.5" fill="#475569">　竞争 CM 写路径，</text>
<text x="172.0" y="1119.5" font-size="8.5" fill="#475569">　未获选的一侧保持</text>
<text x="172.0" y="1133.0" font-size="8.5" fill="#475569">　valid 与上下文</text>
<rect x="560" y="960" width="400" height="194.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="572" y="981" font-size="11" fill="#111827" font-weight="600">出核前的资源与流控</text>
<text x="572.0" y="998.0" font-size="8.5" fill="#475569">RouterTable 副本：按 PathID 查到 VC 与资源需求，软件写，三方一致</text>
<text x="572.0" y="1011.5" font-size="8.5" fill="#475569">本级 Reduce credit 表：每用户一个 entry，flit 粒度</text>
<text x="572.0" y="1025.0" font-size="8.5" fill="#475569">　用户建 stream credit 表项时分配 credit 数量</text>
<text x="572.0" y="1038.5" font-size="8.5" fill="#475569">　发 Reduce 包前要求本级 credit 够整包，否则在 PendingTaskQ 等</text>
<text x="572.0" y="1052.0" font-size="8.5" fill="#475569">　ReduceModule 每完成一次 Reduce 并把 flit 发给下游就还一个</text>
<text x="572.0" y="1065.5" font-size="8.5" fill="#475569">PendingTaskQ：没申请到下游 Stream 或 Reduce 资源的任务在这里等</text>
<text x="572.0" y="1079.0" font-size="8.5" fill="#475569">出方向 VC buffer ×4：按 VC0～3 多线程调度，单 VC 阻塞只阻塞该 buffer</text>
<text x="572.0" y="1092.5" font-size="8.5" fill="#475569">进方向只用单个 VC 调度，多 VC 到单 VC 的映射由 Router 侧硬件固化</text>
<text x="572.0" y="1106.0" font-size="8.5" fill="#475569">两类业务层 credit 都分方向，先查 routing table 定方向再取 credit</text>
<text x="572.0" y="1119.5" font-size="8.5" fill="#475569">解析本级 Router 各方向传进来的 core credit release，按 action 决定</text>
<text x="572.0" y="1133.0" font-size="8.5" fill="#475569">　是否同步更新 core 内的 stream 表状态</text>
<rect x="1000" y="960" width="320" height="167.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1012" y="981" font-size="11" fill="#111827" font-weight="600">Header Parser</text>
<text x="1012.0" y="998.0" font-size="8.5" fill="#475569">首拍锁存 Header，检查 opcode / route、长度、身份字段与帧格式</text>
<text x="1012.0" y="1011.5" font-size="8.5" fill="#475569">逻辑字段与检查：version / header_len · packet_type / route</text>
<text x="1012.0" y="1025.0" font-size="8.5" fill="#475569">　dst_addr（在目的端范围内、满足对齐）· byte_count</text>
<text x="1012.0" y="1038.5" font-size="8.5" fill="#475569">　task_id / stream_id（未完成上下文中不得重复占用）· attributes</text>
<text x="1012.0" y="1052.0" font-size="8.5" fill="#475569">生成一个高层 Router 入站 Descriptor 请求 Commit</text>
<text x="1012.0" y="1065.5" font-size="8.5" fill="#475569">一帧一任务：同一 Frame 只属于一个 Router→MM / Router→CM 任务</text>
<text x="1012.0" y="1079.0" font-size="8.5" fill="#475569">首拍固定为 Header：靠“上一帧 TLAST 已接受”判断下一拍是新 Header</text>
<text x="1012.0" y="1092.5" font-size="8.5" fill="#475569">非法 Header 进 Drop Frame：不生成 Descriptor、不发存储器请求，</text>
<text x="1012.0" y="1106.0" font-size="8.5" fill="#475569">　只消费到 TLAST 以恢复帧边界</text>
<rect x="1360" y="960" width="350" height="194.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1372" y="981" font-size="11" fill="#111827" font-weight="600">Hmem</text>
<text x="1372.0" y="998.0" font-size="8.5" fill="#475569">Hmem 288 B = 16 项 × {core_mask 2 B, sw_header 16 B}：</text>
<text x="1372.0" y="1011.5" font-size="8.5" fill="#475569">　硬件包头与软件包头合并成一张表，按 stream_id 索引</text>
<text x="1372.0" y="1025.0" font-size="8.5" fill="#475569">　软件只配一个地址；B core / R core 改存 Core Mem</text>
<text x="1372.0" y="1038.5" font-size="8.5" fill="#475569">path_id 由 TS 直连送来，size 由 RV core 配寄存器；</text>
<text x="1372.0" y="1052.0" font-size="8.5" fill="#475569">　不再有 path_id_table 与 task_len_table</text>
<text x="1372.0" y="1065.5" font-size="8.5" fill="#475569">硬件只改 core_mask，RV core 改软件包头</text>
<text x="1372.0" y="1079.0" font-size="8.5" fill="#475569">进核：只在需要分配新 stream_id 时才存包头（hw_header_op=1），</text>
<text x="1372.0" y="1092.5" font-size="8.5" fill="#475569">　中间环节的 reduce 与 concat 任务直接丢弃</text>
<text x="1372.0" y="1106.0" font-size="8.5" fill="#475569">出核：按 task_id 查出 path_id 与 size 改写进硬件包头</text>
<text x="1372.0" y="1119.5" font-size="8.5" fill="#475569">　path_core_mask 在 core 内没有修改接口，软件包头不改</text>
<text x="1372.0" y="1133.0" font-size="8.5" fill="#475569">支持纯包头任务（data_len = 0），进出 core 都可以</text>
<polygon points="179,1194.5 270,1194.5 261,1224.5 170,1224.5" fill="#f8fafc" stroke="#374151"/>
<text x="220.0" y="1213.0" font-size="9" fill="#374151" text-anchor="middle">cmem_rd / wr</text>
<polygon points="299,1194.5 390,1194.5 381,1224.5 290,1224.5" fill="#f8fafc" stroke="#374151"/>
<text x="340.0" y="1213.0" font-size="9" fill="#374151" text-anchor="middle">mmem_rd / wr</text>
<polygon points="419,1194.5 510,1194.5 501,1224.5 410,1224.5" fill="#f8fafc" stroke="#374151"/>
<text x="460.0" y="1213.0" font-size="9" fill="#374151" text-anchor="middle">smem_wr</text>
<polygon points="694,1194.5 835,1194.5 826,1224.5 685,1224.5" fill="#f8fafc" stroke="#374151"/>
<text x="760.0" y="1213.0" font-size="9" fill="#374151" text-anchor="middle">out_core_data_ch</text>
<polygon points="1094,1194.5 1235,1194.5 1226,1224.5 1085,1224.5" fill="#f8fafc" stroke="#374151"/>
<text x="1160.0" y="1213.0" font-size="9" fill="#374151" text-anchor="middle">in_core_data_ch</text>
<path d="M220.1 1155.5 L224.4 1193.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M340.1 1155.5 L344.4 1193.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M460.1 1155.5 L464.4 1193.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1335.5 74.0 L1339.9 109.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<text x="1410" y="90" font-size="8.5" fill="#6b7280" text-anchor="start">RV core 写四个寄存器，最后写 Trigger</text>
<path d="M1180.0 200.5 L1140.9 187.3" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1113.8" y="187.0" width="92.4" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1160" y="194.5" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">双 Bank，Bank0 优先</text>
<path d="M1164.5 1194.5 L1160.1 1128.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<text x="1250" y="1216.5" font-size="8.5" fill="#6b7280" text-anchor="start">帧：首拍 Header，后续 Payload</text>
<path d="M1200.0 960.0 L1200.0 701.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1202.8" y="786.8" width="10.5" height="86.9" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 1208 830.25)" x="1208" y="833.2" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">Payload → inbound</text>
<path d="M1300.0 960.0 L1300.0 920.0 L1540.0 920.0 L1540.0 325.0 L1330.0 325.0 L1330.0 292.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1542.8" y="557.3" width="10.5" height="130.4" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 1548 622.5)" x="1548" y="625.5" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">Header → 配对（Descriptor）</text>
<path d="M860.0 264.0 L860.0 320.0 L700.0 320.0 L700.0 349.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="693.4" y="308.5" width="173.3" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="780" y="316" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">Commit 激活两侧的 Active Context</text>
<path d="M1100.0 264.0 L1100.0 602.1 L1129.0 602.1" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1086.8" y="381.5" width="10.5" height="61.1" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 1092 412.0)" x="1092" y="415.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">激活读侧</text>
<path d="M1130.0 665.4 L1071.0 665.4" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M890.0 630.2 L861.0 623.7" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="831.5" y="616.8" width="86.9" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="875" y="624.25" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">read-ahead credit</text>
<path d="M700.0 687.0 L700.0 920.0 L480.0 920.0 L480.0 959.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="563.7" y="908.5" width="52.6" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="590" y="916" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">写 MM / CM</text>
<path d="M200.0 960.0 L200.0 406.8 L559.0 406.8" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="186.8" y="657.1" width="10.5" height="52.6" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 192 683.375)" x="192" y="686.4" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">读 MM / CM</text>
<path d="M860.0 406.8 L889.0 406.8" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M1070.0 406.8 L1129.0 413.4" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M1130.0 451.6 L1110.0 451.6 L1110.0 930.0 L760.0 930.0 L760.0 959.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1096.8" y="675.0" width="10.5" height="31.5" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 1102 690.8)" x="1102" y="693.8" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">出核帧</text>
<path d="M1480.0 959.0 L1480.0 400.8 L1431.0 400.8" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="1482.8" y="571.9" width="10.5" height="57.0" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 1488 600.4)" x="1488" y="603.4" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">查表改写包头</text>
<path d="M760.0 1154.5 L764.4 1193.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<text x="850" y="1216.5" font-size="8.5" fill="#6b7280" text-anchor="start">经 CoreStation 发往 Router</text>
<path d="M600.0 350.0 L600.0 322.0 L400.0 322.0 L400.0 265.0" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#g)"/>
<rect x="390.6" y="311.0" width="218.8" height="10" fill="#ffffff" opacity="0.92"/>
<text x="500" y="318" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8" fill="#0f766e" text-anchor="middle">各 Lane 的 issue_done / drained（以 RD_CH1 为例）</text>
<path d="M460.0 187.0 L499.1 167.2" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#g)"/>
<rect x="467.0" y="173.5" width="25.0" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="479.553907198723" y="181.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#0f766e" text-anchor="middle">Join</text>
<path d="M650.0 110.0 L645.6 75.0" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#g)"/>
<text x="20" y="1292" font-size="10.5" fill="#374151" text-anchor="start">五种搬运方向：Router → MM、Router → CM 走 RD_CH0 + WR_CH0；MM → Router、CM → Router、MM → CM 走 RD_CH1 + WR_CH1。本版本不支持 CM → MM。WR_CH1 的完成也进 Completion RS（图中省略连线）。</text>
<text x="20" y="1314" font-size="10.5" fill="#374151" text-anchor="start">数据布局仅支持连续一维搬运，不支持 stride；单个 DTE 任务的搬运量上限 32 KB（256 B × 128 拍），超过的拆成多个任务包下发。</text>
</svg>
```

***

## 2　功能清单

一功能一条，编号供“机制覆盖”一章引用。

### Header Parser

| 编号 | 功能 |
| - | - |
| F1 | 首拍锁存 Header，检查 opcode / route、长度、身份字段和帧格式 |
| F2 | 解析的逻辑字段与各自的检查：`version` / `header_len`（版本受支持、长度不超过首拍有效字节）；`packet_type` / `route`（标识这是 DTE 搬入任务并选 Router → MM 还是 Router → CM，其他 Route 在这里拒绝）；`dst_addr`（在目的端地址范围内、满足对齐）；`byte_count`（与后续 Payload 的 TKEEP 累计值及 TLAST 位置一致）；`task_id` / `stream_id`（未完成上下文中不得重复占用）；`attributes` / `reserved`（未定义位为约定默认值） |
| F3 | 生成一个高层 Router 入站 Descriptor，请求 Commit 为 RD_CH0 与 WR_CH0 同时分配 TaskQueue 项和完成跟踪项 |
| F3a | Descriptor 的 `stream_id` 取自包头：一个用户在各 core 上占的槽位按到达顺序环形分配，各 core 分出来的号一致。`task_id` 按 `path_id` 查本地的 `path_task_map` 副本，与 TS 那一份同源：这一笔是任务链上的第几步由收方的链定，包头里带的是发方的编号 |
| F3c | Descriptor 的 `dst_addr` 取自包头，落 Core Mem 的那一档收方再叠自己的 stream 偏移，落 Matrix Mem 的那一档就是最终地址。`route` 这一项按 core 的角色定死：计算 core 落 Core Mem，R core 落 Matrix Mem。发方那一侧没有指定收方落哪块存储的寄存器，包头里因此只带地址 |
| F3d | B core 与 R core 上进来的包不建 stream 表项，进核那一笔的完成没有可报的对象，因此不回 Ack |
| F3b | 每一帧另编一个帧号，从这里发给进核通道。进核那一路按帧号认「这几拍属于哪一帧」：`task_id` 只说这一笔是链上的第几步，同一个 `path` 上连着来的几个包带的是同一个值 |
| F4 | 一帧一任务：同一个 AXI-Stream Frame 只属于一个 Router → MM 或 Router → CM 任务，不允许任务间交织 |
| F5 | 首拍固定为 Header，靠“上一帧 TLAST 已接受”判断下一拍是新 Header，不依赖 Start-of-Frame 信号 |
| F6 | Header Commit 成功后才允许 Payload Fire；TLAST 标识最后一个 Payload beat；`byte_count` 为 0 时可由 Header beat 同时携带 TLAST |
| F7 | TKEEP 按字节粒度生效，每个 Payload Fire 累计 TKEEP 有效字节，TLAST 时与 `byte_count` 比较 |
| F8 | 非法 Header 进 Drop Frame 流程：不生成 Descriptor、不发存储器请求，只消费到 TLAST 以恢复帧边界 |

### Commit

| 编号 | 功能 |
| - | - |
| F9 | 一个高层任务必须同时拿到三样才接纳：目标通道读侧的 TaskQueue 项、写侧的 TaskQueue 项、Completion RS 项 |
| F10 | 任一侧没有空间时 Commit 整体保持，Header 入口向 Router 反压。这条规则挡住“读已经开始、写还没有落脚点”的半任务 |
| F11 | 同时完成地址展开：源地址、目的地址、按任务边界切分的元数据都在这一步算好 |
| F12 | 两个配置 Bank，Bank0 优先于 Bank1：都空闲时 Router 的配置进 Bank0、RV core 的配置进 Bank1；只剩一个 Bank 而两者竞争时优先配置 Router 信息 |
| F13 | 两个任务入口在 Commit 边界汇成同一套内部任务模型 |
| F14 | RV core 侧的配置序列：用 `dsawi` / `dsaw` 写 `TASK_CFG_ADDR` 与 `TASK_CFG_TD` 两个寄存器，一条指令写一个，再写 Trigger（`TASK_CFG_TRG`），`TASK_CFG_PACK` 随之自动写入。必须最后写 Trigger |
| F14a | 写 Trigger 那一拍把当前模板的十一项与四个直连身份信号一起采下来拼成 Descriptor。四个身份不由软件写：`streamID` / `taskID` / `userID` / `pathID` 从 RV core 的 CSR 直连过来 |
| F14b | 一笔配置写在被收下之前一直保持同一个序号。每拍换号的话 DSA 按序号去重就把同一笔认成好几笔，写一次执行一次的 Trigger 会被执行好几遍 |

**Fast LUT**：从「TS 把任务下发下来」到「总线上出现第一笔搬运请求」这一段叫 DTE Setup Time，目标是压到 10T 以内。办法是常规任务不走 RV core 的配置 kernel：TS 给的 `task_id` 命中 Fast LUT 后，硬件拿表项内容（`length`、控制位）与 `user_id` 索引到的 User Base Register 拼出 task descriptor，直接推进对应通道的 TaskQueue，命中路径 4T；未命中才转发信息、重设 PC、启动 RV core 的 kernel，代价是 Core Latency + 4T。Fast LUT 只加速任务配置，不改路由定义、数据通路和完成条件。

### 五个物理通道与各自的 TaskQueue

| 编号 | 功能 |
| - | - |
| F15 | 五个物理通道：一个进核通道 `in_ch`，四个出核通道 `out_ch[0..3]` **与 Router 的四个 VC 一一对应**。`MM → CM` 不另开通道，固定复用 `out_ch[3]`，它的目的端要能 MUX 到 Core Mem。每个通道再拆成读写两半 |
| F16 | 每个通道的读写两半各自拥有 TaskQueue（**深度不少于 16**，与 TS 的 16 个 stream 对齐）和 Active Context；一个高层任务落到一对 RD / WR 子上下文上 |
| F17 | **通道之间可以乱序执行**：某个 VC 阻塞只堵住对应的那个出核通道，别的通道照发。同一通道内读写两半的状态也彼此独立，一侧的 Active Context 释放后就能激活下一个任务，不等另一侧 |
| F18 | **通道内顺序执行**：TaskQueue 按序激活。read-ahead 允许 RD / WR 任务序号错位，但不改变通道内的顺序。向 TS 反馈完成仍按 TS 下发的顺序，与通道间的乱序无关 |
| F19 | `issue_done` 就允许该侧提前激活下一任务，不必等全部 drain |
| F20 | 读这一半允许领先写那一半，领先量由三件事共同约束：中间 Buffer 的可用 Credit、读的 outstanding 限额、可保留的任务边界数。Buffer 的位置在发读请求那一刻就占下，不是等响应回来再看有没有地方：等回来再看的话，没位置的那些读响应只能丢，而请求已经发出去、outstanding 也已经记上 |
| F21 | 并发约束：各通道目的资源无冲突时独立推进，共享 DMA_XBAR 端口时按其仲裁规则；Router → CM 与 MM → CM 竞争 CoreMem 写路径，由 CM 写仲裁器选择，未获选的一侧保持 valid 和上下文 |
| F21a | 一个通道一拍里两侧都可能用存储端口：MM → CM 那一档读那一半读 Matrix Mem、写那一半写 Core Mem。两块存储各记各的占用，谁都不许替对方把端口置闲 |
| F22 | 数据布局仅支持连续一维搬运，当前不支持 stride |
| F23 | 单个 DTE 任务的搬运量上限是 32 KB（256 B × 128 拍），比包上限小，超过的要拆成多个任务包下发；一个任务可由多个任务包组成，包之间不保证顺序执行 |

### 中间 Buffer

| 编号 | 功能 |
| - | - |
| F24 | 中间 Buffer 每条通道一份，8 KB，按 256 B 一项算 32 项，最大可掩盖 32 T 的访存延迟 |
| F25 | Buffer 满时通过 TREADY 向 Router 反压 |
| F26 | 出口阻塞只通过 Credit 反压限制领先距离，不要求读写用同一个 Active Context |

### Completion RS 与 Done Pending

| 编号 | 功能 |
| - | - |
| F27 | 只在同一笔任务的 RD 与 WR 两侧条件都满足时产生 `task_done`（Join）。认哪两半属于同一笔用的是 Commit 准入时分配的内部序号：`task_id` 只在一个 stream 内唯一，同一拍在途的两笔任务可以带同一个值（一笔是 Router 送进来的搬入，另一笔是 RV core 配的搬出）。向 TS 上报时用的仍是 `stream_id` 与 `task_id` |
| F28 | 同一拍多个 Join 命中时全部写入 Done Pending，不允许覆盖或丢失，由 Done Pending 负责串行化 |
| F29 | 向 TS 的报告是 exactly-once |
| F30 | 六个完成层级：`queued`（已进 TaskQueue 未装载）→ `active`（由对应 AGCU / Ctrl 执行）→ `issue_done`（该侧最后一个请求已 Fire）→ `drained`（相关响应、Buffer 数据和外部副作用均已收敛）→ `join_done`（同一 task_id 的 RD 与 WR 都满足）→ `task_done`（进 Done Pending 并与 TS 成功握手） |
| F31 | `task_last` 标记一个 task 拆成几笔搬运时的最后一笔，只有带这个标记的那一笔完成后才通知 TS；`no_ack` 置位的任务不回 Ack |

### 三条数据流

| 编号 | 功能 |
| - | - |
| F32 | Inbound（Router → MM / CM）：Router 以 AXI-Stream 发 Header → Header Parser 生成 Descriptor → Commit 为 RD_CH0 与 WR_CH0 同时分配 → RD_CH0 控制 Payload 写入 inbound buffer，附带 task_id、有效字节与任务边界 → WR_CH0 从 buffer 按任务边界取数经 DMA_XBAR 写入目标存储 → 两侧按 task_id Join |
| F33 | Outbound（MM / CM → Router）：Commit 原子拆成 RD_CH1 与 WR_CH1 → RD_CH1 的 AGCU 生成源端读地址，Read Ctrl 经 DMA_XBAR 读取，把返回数据连同 task_id 和边界元数据写入 outbound buffer → WR_CH1 按固化的 Route 选 Router TX 或 CoreMem Egress，从 buffer 按任务边界发出 → 同一任务的两侧都完成且 buffer 里该任务的数据已排空后才生成 `task_done` |
| F34 | Inner（MM → CM）：完全走出核通道，固定占 `out_ch[3]`。先锁定 Matrix Mem 为读源、Core Mem 为写目标；数据经 DMA_XBAR RD、`ch1_rd_ctrl` 和 outbound buffer 到达 `ch1_wr_ctrl`；CH1 的出口绑定此时选 DMA WR1 而不是 Router TX；WR1 的硬件 route mask 只允许 CoreMem，因此不会把出核通道的数据写回 Matrix Mem |
| F35 | 存储读、Buffer 搬运和 WR1 写可以流水重叠，但每一级仍各自遵守 valid / ready |

### Hmem 与四类内容的存放

| 编号 | 功能 |
| - | - |
| F36 | 一次搬运的对象是一个 MSG 包，进了 core 就按内容拆成四份、各存各的地方：包头、scale、topK、data |
| F37 | topK 一律走 `cmem_wr` 写进 Core Mem 的独立 topK 区，DTE 与 MU 之间没有直连通路。MU 自己在 task 启动时把这一段读进它的 `topK_ep_table` |
| F38 | 计算 core 上：硬件包头与软件包头**合并成一张表**存 Hmem，16 项按 `stream_id` 索引，每项 `{core_mask 2 B, sw_header 16 B}`，共 `16 × 16 B + 32 B = 288 B`，软件只配一个地址；scale 存 Core Mem 的 scale 区，topK 存 Core Mem 的独立 topK 区，data 存 Core Mem 按 stream 分片 |
| F39 | B core / R core 上：包头存 Core Mem 独立空间，容量由软件分配；走 Hmem 还是走 Core Mem 由 `hw_header_addr` 这个地址本身选，不另设开关。scale 与 topK 与 data 在 Matrix Mem 里连排，由 GPU 侧按 pattern 排好序送来，DTE 不重排顺序 |
| F40 | **DTE 内不再存 `path_id_table` 与 `task_len_table`**：`path_id` 由 TS 直连送过来（TS 配置时就带 `user_id` / `stream_id` / `path_id` / `task_id` 四样），`size` 由 RV core 配寄存器给，或按 `data_len` 算出来 |
| F41 | 包头分工：硬件只改硬件包头（`core_mask`），RV core 改软件包头 |
| F42 | `data_len` 在不同方向盖的范围不同：Router ↔ Matrix Mem 时是 topK + scale + data 的总长；Router ↔ Core Mem 时只是 data 的长度，scale 与 topK 的长度另算 |
| F42a | `CFG_DATA_LEN` 是 16 bit，写进去的数以 8 B 为一格，硬件乘回字节，所以一段最长 64 KB 差 8 B。不足一格的尾巴配不出来 |
| F43 | 进核时计算 core 只在这个 token 需要分配新 `stream_id` 时才存包头（`hw_header_op = 1`），中间环节的 reduce 与 concat 任务直接丢弃（`hw_header_op = 0`）；广播 token 进核必然带 topK，必须存下来 |
| F44 | 出核时改写硬件包头：`path_id` 用 TS 送来的那个，`size` 用 RV core 配的寄存器（Concat 这类算完数据量会变的场景就靠它），`core_mask` 只在 Bach 做 MoE Route 时改；计算结果出核不带 topK |
| F44a | 出核任务要发的那个包在 Commit 准入时就建好，身份与长度按 F44 填；读侧从存储取回的每一块按已填字节数排进它的 payload |
| F44c | 出核造包时把 DTE 模板里的 `dst_addr` 抄进包头：那一项配的是收方的落点，发方这一笔自己用不着它 |
| F44b | 出核不改的包头字段沿用进核那一笔的：DPU 写的 `gpu_id` 与 `token_id` 在进核那一笔记进 Hmem 里这个 `stream_id` 的软件包头，出核造包时取回来填上 |
| F45 | 支持纯包头任务（`data_len = 0`），进出 core 都可以 |
| F46 | 地址的一条规矩：软件只配基址，偏移由硬件用 `stream_id` 算出来。`stream_id` 是 TS 建 stream 表项时定的，随任务一起给到 DTE，软件不需要知道这个 token 落在 Core Mem 的哪一片。进核与出核两个方向都按这条算：进核的落点是 `stream_base + stream_id × stream_stride + dst_addr`，出核的取数点把 `src_addr` 代进同一个式子 |
| F47 | 通用寻址式子是 `PhyAddr = base_addr + stream_id × stride + offset`，**`base_addr` 只对 Core Mem 有效**：Matrix Mem 的地址全由软件管，配任务时 `src_addr` / `dst_addr` 就是最终物理地址，硬件不再叠 `stream_id × stride`。四类地址按这个式子展开：data 在 Core Mem 侧是 `base_addr + stream_id × stream_stride`；包头（硬件加软件合并那一项）是 `header_base_addr + stream_id × 18 B`；scale 是 `scale_base_addr + stream_id × scale_stride`；topK 是 `topk_base_addr + stream_id × 256 B` |
| F48 | 两项搬运长度硬件自己算，不用软件配：scale 是 `data_len / 32`（32 个元素共用一个 scale），topK 是 `router_ep_count × 6 B`（每项 `{expert_id 2 B, weight 4 B}`，每 stream 上限 256 B） |
| F49 | Matrix Mem 一侧不加 stream 偏移，Core Mem 一侧加：Matrix Mem 放的是模型 weight 与按 pattern 排好序送来的 token，位置软件自己算准；Core Mem 按 stream 切成 16 片，谁占哪片由 TS 定，软件配的时候还不知道 |

### shareMem 写

| 编号 | 功能 |
| - | - |
| F50 | 任务数据传输完成后，按 `sharemem_waddr` / `sharemem_data` 写 shareMem，然后通知 TS |
| F51 | 只在 B core 与 R core 使用，存 user_id 与 token entry 的 valid 标志：token 搬入 Matrix Mem 后置 valid，搬出后置 invalid |
| F51a | 搬入那一笔的标志由 Header Parser 建描述符时一并填：表的基址与一个 token 槽位多大由 core 的配置给，第几项按包头的 `dst_addr` 除以槽位大小算，写进去的值是 valid。搬入的描述符不经软件，标志的地址因此不能由软件配（原文只说了写哪里，没说搬入这一笔的地址怎么来） |

### 出核前的资源与流控

**分工**：下游的 Stream 资源与 Rmem 资源由 TS 在下发前查：TS 查 RouterTable 与对应的 stream 资源，有资源才把任务下发下来；**DTE 这一侧只查 VC 通路上的 flit credit**，外加发往本 core ReduceModule 时的本级 Reduce credit（F56、F57）。这样划分是因为业务层资源以 stream 为单位、生命期跨整条任务链，而 flit credit 是逐拍变化的，只有真正要发数据的那一刻才知道够不够。

**`stream_cache`**：DTE 里另存一份 Router 那张 stream 表的副本（`User Resource Cache Table`），3 方向各 16 项 `{valid, user_id}`，形状与 Router 的 `stream_tab[d]` 一致。它是**只跟随、不分配**的：真正建表项只有 Router 能做（F53 的分工），这份副本靠 Router 各方向送回的 `stream_credit_vld` 与 release 里的 action 位同步（F62），用处是包要重发时本地先记账（F63），以及判断某个包该不该重注入。

| 编号 | 功能 |
| - | - |
| F52 | DTE 内维护一份 RouterTable 副本，按 PathID 查到 VC 与资源需求；软件负责写入并保证与 Router、ReduceModule 三方一致 |
| F53 | 发数据之前实时检查该任务所属 VC 通路上的 flit credit，不够就让任务在 `PendingTaskQ` 等；下游 Stream / Rmem 资源不在这里查，TS 下发之前已经申请到 |
| F54 | `PendingTaskQ` 排在 Commit **之前**：RV core 配好一个出核任务后，先按 `path_id` 查出走哪个 VC 与资源需求，credit 不够的进 `PendingTaskQ` 等，够了才去 Commit 申请那三样。等 credit 的任务因此不占 TaskQueue 项，也不占 Completion RS 项 |
| F55 | `PendingTaskQ` 满时拉低 `dsa_cfg` 的 `req_ready`，反压 DTE RV core，该 RV core 不能参与下一个用户的搬运。反压只落到出核这条链上，进核那条链的 Commit 资源不受影响 |
| F55a | 五个通道对每块存储的读与写各只有一个 master 口，由 DMA_XBAR 轮转仲裁。读与写各走各的口、各有各的轮转，一拍可以同时发一读一写。通道在入口各占几格，按序号把请求放进来；`req_ready` 报的是那几格还收不收得下 |
| F55b | 四个出核通道对 Router 只有一个 `out_core_data_ch`，同样轮转仲裁。授权粘在一个通道上直到它把带 `tlast` 的那一拍发完，一个包的几拍中间不会插进别的包 |
| F56 | 本级 Reduce credit 表：每用户一个 entry，flit 粒度。某个用户建 stream credit 表项时给这个用户分配一个 entry 的 credit 数量 |
| F57 | 搬 Reduce 包前先检查本级 Reduce credit 是否够整包，再在 VC credit 满足的前提下发到 ReduceModule |
| F58 | ReduceModule 每完成一次 Reduce 并把 flit 发给下游就释放一个 credit，经独立的释放通道把 Valid 加 UserID 送回 DTE |
| F59 | 出方向按 VC0～3 多线程调度维护多个 VC buffer，某个 VC 阻塞只阻塞对应的那个 buffer；用它吸收整包流量，完成 core 与 Router 之间的协议转换 |
| F60 | 进方向 Router 与 core 之间只用单个 VC 调度，多 VC 到单 VC 的映射由 Router 侧硬件固化完成；DTE 侧感知单 VC buffer 的缓存状态并据此启动搬运，解析包信息，搬完按 flit 释放 VC credit |
| F61 | 两类业务层 credit（下游的 coremem credit 与 reduce credit）都分方向，方向由 routing table 定；这两类由 TS 在下发前查（见 TS 一节 F60～F63、F69）。DTE 只负责 credit 回程：把 Router 各方向送回来的 release 解析出来更新本地的表 |
| F62 | credit 回程：解析本级 Router 各方向传进来的 core credit release，按其中的 action 信息决定是否同步更新 core 内的 stream 表状态。action 三种：1 credit release only（下游发起 release，本级 port 的 credit 更新）、2 bypass only（本级发生 bypass 而下游没有 release，传的是消耗信息，单 port 实现不复用）、3 bypass + credit release（两者同时发生，同步传两个 user_id 与 path_id，只发生在 router → core 场景） |
| F63 | 进 core 缓存的包要重发时，**core 内先同步更新本地的 core credit table**，之后 Router 的 output 检索到该重发包时再更新自己那一份 |

### 软件配置的寄存器

| 编号 | 功能 |
| - | - |
| F64 | `TASK_CFG_TD` 位域：`src_sel`（00=Router / 01=MM / 10=CM）、`dst_sel`、`queue_sel`（b0=ch0 / b1=ch1）、`cnt`（搬运总拍数，单任务最大 256 B × 128）、`mask`（256 B 一拍，按 32 B 粒度，位为 0 表示该 32 B 无效）、`path_id`、`task_pack_id`、`last` |
| F65 | `TASK_CFG_PACK` 位域：`UserID`、`stream_id`（等同 SlotID）、`task_id`（最大 64）、`no_ack`、`last`。打包信息只有在 `dst_sel` 是 Router 时才有效 |
| F66 | 按字段命名的软件寄存器分五组：模式与开关（`transfer_mode` / `scale_valid` / `topK_valid` / `hw_header_op` / `task_last` / `smem_valid` / `router_ep_count`）、数据的地址与长度、包头与 scale 与 topK 的地址、shareMem 表项、任务身份 |
| F67 | 不随任务变的控制与观测寄存器：`SYS_CTRL` / `DTE_CTRL`（时钟门控、软复位、任务启动触发、单步调试使能、清空缓冲）、`SYS_STATUS` / `DTE_STATUS`（Idle / Running / Error / Stop 与忙状态）、`EXCEPT_STATUS` / `EXCEPT_MASK`、`EXCEPT_CFG_*`（出异常时自动抓下当时的三个任务配置寄存器，只读）、`PMU_CTRL` 与 11 个 `PMU_CNT_*` |
| F68 | 异常四类：访存越界、非对齐、ECC 错、搬运异常。中断默认屏蔽，写 0 打开。本轮只留状态位与接口名，不实现行为 |

### 包的边界与读写通路

| 编号 | 功能 |
| - | - |
| F69 | 包长范围：最短 16 B，即只含包头与路由信息的空包；最长允许 64 KB，实际支持到 (16 K + 32) B |
| F70 | 不设包尾。包的结束靠包长度计数；整包校验的做法是物理层按 transfer 校验加软件做整包校验，软件的校验位算进软件 payload，不占硬件字段 |
| F71 | reduce 包的软件辅助信息长度**必须固定 16 B**，Router 做 reduce 加法时固定跳过这 16 B。软件层面 reduce 包只需要传 `user_id` 加操作类型（如 concat idx），16 B 够用。非 reduce 包的软件辅助信息长度由软件自定，Router 完全不感知内容和长度 |
| F72 | Router 把数据送给 DTE 时**不剥离任何数据**，DTE 的软件能看到包头、路由信息在内的全部内容。其中 VC 字段每一跳 Router 都会改写，每个 core 看到的内容不完全一致 |
| F73 | 软件处理一个包用两条通路：包头的路由信息与软件辅助信息用标量 store / load 指令生成和读取，实际的用户 token 数据用 DMA 搬。两条通路对延迟与吞吐的需求不同，而且非 reduce 包的软件辅助信息长度不固定，硬件实现要按两条通路分别优化 |
| F74 | reduce 包出核时 DTE 在硬件字段里打上 `reduce_seq`：一个 stream 的 `reduce_num = N` 笔按 0～N−1 顺序编号，同一笔的 `dsa_done` 带回同一个 `reduce_seq`。Router 的 Reduce Done 原样带这个号回 TS，TS 靠它把两半逐包配对 |

***

## 3　接口

```
port in_core_data_ch (slave, AXI-Stream-Like, clk)    // CoreStation → DTE：进 core 的整包
  in  tvalid · tdata[2047:0] · tkeep[255:0] · tlast · thdr
  out tready                                            // = inbound buffer 有空 且 Commit 三样资源都够
port out_core_data_ch (master, AXI-Stream-Like, clk)  // DTE → CoreStation：出 core 的整包
  out tvalid · tdata[2047:0] · tkeep[255:0] · tlast · thdr · vc_id[1:0]
  in  tready                                            // = 该 VC 的 Core 方向输入 VC 有空
port router_credit (slave, 电平 + 脉冲, clk)          // Router 侧回来的三类信息
  in  stream_credit_vld[2:0] · stream_credit_user[2:0][15:0]
  in  reduce_release_vld · reduce_release_user[15:0]
  in  vc_credit[3:0][7:0]
port dsa_cfg (slave, valid/ready, clk)                // DTE RV core 的 dsa_iss
  in  req_valid · req_we · req_addr[11:0] · req_wdata[31:0]
  out req_ready                                         // = 配置通路未反压；Commit Bank 满或 PendingTaskQ 满时拉低
port dsa_rdata (master, 脉冲, clk)                    // 读寄存器的异步返回
  out valid · rdata[31:0]
port dsa_ids (slave, 电平, clk)                       // DTE RV core 的 CSR 直连；写 task_trigger 那一拍采样
  in  stream_id[3:0] · task_id[5:0] · user_id[15:0] · path_id[7:0]
port dsa_done (master, 脉冲, clk)                     // → TS：task_last 的那一笔完成时报
  out valid · stream_id[3:0] · task_id[5:0] · reduce_seq[5:0]   // reduce 任务才有效，供 TS 逐包配对
port cmem_rd / cmem_wr (master, valid/ready, clk)     // 经 DMA_XBAR，256 B
  out req_valid · req_addr[17:0] · req_wdata[2047:0] · req_be[255:0]
  in  req_ready · rsp_valid · rsp_rdata[2047:0] · rsp_scale[63:0]
port mmem_rd / mmem_wr (master, valid/ready, clk)     // 经 DMA_XBAR，256 B
  out req_valid · req_addr[24:0] · req_wdata[2047:0]
  in  req_ready · rsp_valid · rsp_rdata[2047:0]
port smem_wr (master, valid/ready, clk)               // shareMem 表项写，只有 B core / R core 用
  out req_valid · req_addr[14:0] · req_wdata[31:0]
  in  req_ready
  out req_valid · req_stream_id[3:0] · req_off[7:0] · req_wdata[47:0]   // 每项 {expert_id 2 B, weight 4 B}
  in  req_ready
port cfg (slave, ctrl_noc 写事务, clk)                // 静态寄存器、Hmem、Fast LUT、RouterTable 副本
  in  cfg_valid · cfg_addr[23:0] · cfg_we · cfg_wdata[31:0]
  out cfg_rdata[31:0]
```

***

## 4　存储器

```
mem task_q[c][h]     FIFO      每通道每侧 ≥16 项 × Descriptor（c ∈ {in_ch, out_ch0..3}，h ∈ {RD, WR}）  1W1R  按序激活  复位空
mem active_ctx[c][h] FF        每通道每侧一份 {task_id[5:0], stream_id[3:0], user_id[15:0], cur_addr, remain, boundary}  1RW  issue_done 后释放  复位空
mem in_buf           FIFO      进核那条通道，8 KB（256 B × 32）                      1W1R  满 → TREADY 拉低       复位空
mem out_buf[c]       FIFO      出核四条通道各一份，8 KB（256 B × 32）                 1W1R  满 → 停止 RD 侧发请求    复位空
mem comp_rs          FF 阵列   16 项 × {task_id[5:0], rd_done, wr_done, drained}      1RW   Commit 时占，Join 时消  复位空
mem done_pend        FIFO      16 × {stream_id[3:0], task_id[5:0]}                    1W1R  串行化同拍多个 Join    复位空
mem hmem             FF 阵列   16 项 × {core_mask 2 B, sw_header 16 B} = 288 B         1R1W  按 stream_id 索引；硬件写 core_mask，RV core 写 sw_header  复位 0
mem fast_lut         FF 阵列   64 × {valid, length[15:0], ctrl_flags}                 1R    boot 期经 ctrl_noc 配好，按 task_id 索引  复位 valid=0   // 只加速任务配置，不改路由定义、数据通路和完成条件
mem rtab_copy        FF 阵列   64 项，RouterTable 的外部副本                           1R1W  软件写，三方一致        复位 0
mem path_task_copy   FF 阵列   64 × task_id[5:0]，TS 那张 path_task_map 的副本         1R1W  boot 期配成与 TS 一致    复位 0
mem reduce_credit    FF 阵列   16 用户 × 计数器（flit 粒度）                           1RW   建 stream credit时分配，release 恢复  复位 0
mem stream_cache     FF 阵列   3 方向 × 16 项 × {valid, user_id[15:0]}                 1R1W  Router 的 User Resource Allocation Table 的 cache，只跟随不分配  复位空
mem pending_taskq    FIFO      16 × Descriptor                                        1W1R  排在 Commit 之前，资源没申请到的出核任务在这里等；满则拉低 dsa_cfg 的 req_ready  复位空
mem out_vc_buf[4]    FIFO      每 VC 一个，深度按整包容量                              1W1R  某 VC 阻塞只阻塞该 buffer  复位空
mem cfg_bank[2]      FF 阵列   两个配置 Bank × {TASK_CFG_ADDR, TASK_CFG_TD, TASK_CFG_PACK}  1RW  Bank0 优先  复位空
mem template[4]      FF 阵列   3 套有效 + 1 套 header-only，每套 64 B 对齐 × 十一项    1RW   RV core 写；写 Trigger 那一项时按当前内容起一笔任务  复位 0
mem xbar_slot[m][c]  FIFO      每块存储每通道 4 格 × 请求（m ∈ {CM, MM}）             1W1R  DMA_XBAR 入口；占到 2 格就拉低 req_ready  复位空
mem out_slot[v]      FIFO      每出核通道 4 格 × 一拍（v ∈ 0..3）                     1W1R  出核仲裁入口；占到 2 格就拉低 tready  复位空
mem pmu_cnt          FF 阵列   11 个计数器                                            1RW   搬运原语数、进核 / 出核各自的执行周期与数据量等  复位 0
```

***

## 5　流水线总览

一个搬运任务从中间劈开，读一半与写一半各自排队推进，按 `task_id` 合回一次完成。性能剖析按下面这套口径分段，每段对上第 1 层图的哪几级：

```
setup_cycles  = first_issue_fire - task_accept_fire      // M1 到 M3
issue_cycles  = issue_done       - first_issue_fire      // M4 与 M6
drain_cycles  = task_done        - issue_done            // M5 排空与 M7
report_cycles = done_fire        - task_done             // M8
total_latency = setup + issue + drain + report
active_cycles = cycles(any_data_fire)
stall_cycles  = cycles(valid && !ready)
```

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 678 608" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="areov" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="678" height="608" fill="#ffffff"/>

  <text x="20" y="26" font-size="12" fill="#111827">DTE · 第 1 层流水线总览（读写两半各自推进，按 task_id 合回一次完成）</text>
  <text x="20" y="42" font-size="9.5" fill="#6b7280">横向是级序，不是拍序；每级的拍数在右上角 Dx。橙色虚线框是变长级，非按比例。</text>
  <path d="M150 52 L150 70" stroke="#e5e7eb" fill="none"/>
<path d="M150 126 L150 328" stroke="#e5e7eb" fill="none"/>
<path d="M150 384 L150 500" stroke="#e5e7eb" fill="none"/>
  <path d="M316 52 L316 70" stroke="#e5e7eb" fill="none"/>
<path d="M316 126 L316 328" stroke="#e5e7eb" fill="none"/>
<path d="M316 384 L316 500" stroke="#e5e7eb" fill="none"/>
  <path d="M482 52 L482 70" stroke="#e5e7eb" fill="none"/>
<path d="M482 126 L482 500" stroke="#e5e7eb" fill="none"/>
  <text x="20" y="102" font-size="10.5" fill="#6b7280">接纳</text>
  <rect x="150" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="84" font-size="8.5" fill="#6b7280">M1</text>
  <text x="292" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="104" font-size="11" fill="#111827">Header Parser</text>
  <rect x="316" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="84" font-size="8.5" fill="#6b7280">M2</text>
  <text x="458" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="104" font-size="11" fill="#111827">Commit 配对接纳</text>
  <path d="M300 98 L315 98" stroke="#475569" marker-end="url(#areov)" fill="none"/>
  <rect x="482" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="492" y="84" font-size="8.5" fill="#6b7280">M3</text>
  <text x="624" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="492" y="104" font-size="11" fill="#111827">通道激活</text>
  <path d="M466 98 L481 98" stroke="#475569" marker-end="url(#areov)" fill="none"/>
  <text x="20" y="188" font-size="10.5" fill="#6b7280">读一半</text>
  <rect x="150" y="156" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="160" y="170" font-size="8.5" fill="#92400e">M4</text>
  <text x="292" y="170" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="160" y="190" font-size="11" fill="#7c2d12">读侧发请求</text>
  <rect x="316" y="156" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="326" y="170" font-size="8.5" fill="#92400e">M5</text>
  <text x="458" y="170" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="326" y="190" font-size="11" fill="#7c2d12">中间 Buffer</text>
  <path d="M300 184 L315 184" stroke="#475569" marker-end="url(#areov)" fill="none"/>
  <text x="20" y="274" font-size="10.5" fill="#6b7280">写一半</text>
  <rect x="150" y="242" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="160" y="256" font-size="8.5" fill="#92400e">M6</text>
  <text x="292" y="256" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="160" y="276" font-size="11" fill="#7c2d12">写侧取数写出</text>
  <text x="20" y="360" font-size="10.5" fill="#6b7280">合上</text>
  <rect x="150" y="328" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="342" font-size="8.5" fill="#6b7280">M7</text>
  <text x="292" y="342" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="362" font-size="11" fill="#111827">Completion RS Join</text>
  <rect x="316" y="328" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="342" font-size="8.5" fill="#6b7280">M8</text>
  <text x="458" y="342" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="362" font-size="11" fill="#111827">Done Pending</text>
  <text x="326" y="376" font-size="11" fill="#111827">与 TS 握手</text>
  <path d="M300 356 L315 356" stroke="#475569" marker-end="url(#areov)" fill="none"/>
  <text x="20" y="446" font-size="10.5" fill="#6b7280">出核等资源</text>
  <rect x="150" y="414" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="160" y="428" font-size="8.5" fill="#92400e">M9</text>
  <text x="292" y="428" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="160" y="448" font-size="11" fill="#7c2d12">PendingTaskQ</text>
  <text x="20" y="524" font-size="10.5" fill="#374151">各通道的 Active Context 彼此独立：任一条到 issue_done 就能激活下一个任务，不等配对的那一条。</text>
  <text x="20" y="552" font-size="10.5" fill="#374151">内部启动延迟 85 T 里，75 T 是 DTE RV core 上那 50 条算地址的指令，M1 到 M3 三拍加两级端口打拍构成“流水启动 5 T”。</text>
  <text x="20" y="580" font-size="10.5" fill="#374151">M9 排在 M2 之前：等资源的出核任务不占 TaskQueue 项，也不占 Completion RS 项。</text>
</svg>
```

***

## 6　逐级行为

### M1 · Header Parser

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 887 218" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="are1" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="887" height="218" fill="#ffffff"/>

  <polygon points="30,43 188,43 178,119 20,119" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="62" font-size="10.5" fill="#374151" text-anchor="middle">in_core_data_ch</text>
  <text x="104" y="80" font-size="9.5" fill="#6b7280" text-anchor="middle">tvalid · tdata[2047:0]</text>
  <text x="104" y="98" font-size="9.5" fill="#6b7280" text-anchor="middle">tkeep[255:0] · tlast</text>
  <text x="104" y="116" font-size="9.5" fill="#6b7280" text-anchor="middle">thdr · tready</text>
  <rect x="20" y="131" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="135" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="152" font-size="10" fill="#374151" text-anchor="middle">fast_lut · FF 64 项 · 1R</text>
  <rect x="691" y="52" width="176" height="114" fill="#f1f5f9" stroke="#334155"/>
  <rect x="691" y="52" width="176" height="18" fill="#334155"/>
  <text x="779" y="65" font-size="10.5" fill="#ffffff" text-anchor="middle">HDR_CMT</text>
  <text x="779" y="92" font-size="10" fill="#334155" text-anchor="middle">route[1:0]</text>
  <text x="779" y="114" font-size="10" fill="#334155" text-anchor="middle">dst_addr[24:0]</text>
  <text x="779" y="136" font-size="10" fill="#334155" text-anchor="middle">byte_count[15:0]</text>
  <text x="779" y="158" font-size="10" fill="#334155" text-anchor="middle">task_id · stream_id</text>
  <rect x="232" y="20" width="415" height="178" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M1</text>
  <text x="633" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">Header Parser · 首拍锁存并检查</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. 上一帧 tlast 已接受 → 本拍的 beat 判为新 Header</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. f = Parse(tdata)；ok = 版本受支持 &amp;&amp; header_len ≤ 首拍有效字节</text>
  <text x="262" y="118" font-size="10.5" fill="#475569">&amp;&amp; dst_addr 在范围内且对齐 &amp;&amp; task_id/stream_id 未被在飞上下文占用</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">3. ok → desc = {route, dst_addr, byte_count, task_id, stream_id}</text>
  <text x="250" y="158" font-size="10.5" fill="#475569">4. !ok → Drop Frame：不生成 Descriptor、不发存储请求，只消费到 tlast</text>
  <text x="250" y="182" font-size="10" fill="#9ca3af">TKEEP 逐 beat 累计，tlast 时与 byte_count 比对</text>
  <path d="M188 81 L231 81" stroke="#475569" marker-end="url(#are1)" fill="none"/>
  <path d="M188 152 L231 152" stroke="#475569" marker-end="url(#are1)" fill="none"/>
  <path d="M647 109 L690 109" stroke="#475569" marker-end="url(#are1)" fill="none"/>
</svg>
```

### M2 · Commit 配对接纳

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 878 240" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="are2" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="878" height="240" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="92" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">HDR_CMT</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">route[1:0]</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">dst_addr[24:0]</text>
  <text x="104" y="104" font-size="10" fill="#334155" text-anchor="middle">task_id · stream_id</text>
  <rect x="20" y="124" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="128" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="145" font-size="10" fill="#374151" text-anchor="middle">cfg_bank[2] · FF 2 组 · 1RW</text>
  <rect x="20" y="178" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="182" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="199" font-size="10" fill="#374151" text-anchor="middle">comp_rs · FF 16 项 · 1RW</text>
  <rect x="682" y="72" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="686" y="76" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="770" y="93" font-size="10" fill="#374151" text-anchor="middle">task_q[RD] · FIFO 16 项 · 1W</text>
  <rect x="682" y="126" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="686" y="130" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="770" y="147" font-size="10" fill="#374151" text-anchor="middle">task_q[WR] · FIFO 16 项 · 1W</text>
  <rect x="232" y="41" width="406" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="57" font-size="8.5" fill="#6b7280">M2</text>
  <text x="624" y="57" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="77" font-size="12" fill="#111827">Commit · 三样同时拿到才收</text>
  <text x="250" y="99" font-size="10.5" fill="#475569">1. ok = task_q[RD].free &amp;&amp; task_q[WR].free &amp;&amp; comp_rs.free</text>
  <text x="250" y="119" font-size="10.5" fill="#475569">2. !ok → 整体保持，tready = 0，向 Router 反压</text>
  <text x="250" y="139" font-size="10.5" fill="#475569">3. ok → 地址展开：src/dst 基址加 stream_id × stride，切出任务边界</text>
  <text x="250" y="159" font-size="10.5" fill="#475569">4. ok → {task_q[RD].push(d), task_q[WR].push(d), comp_rs 占一项}</text>
  <text x="250" y="183" font-size="10" fill="#9ca3af">两个 Bank 都空闲时 Router 的配置进 Bank0，竞争时优先 Router</text>
  <path d="M188 66 L231 66" stroke="#475569" marker-end="url(#are2)" fill="none"/>
  <path d="M188 145 L231 145" stroke="#475569" marker-end="url(#are2)" fill="none"/>
  <path d="M188 199 L231 199" stroke="#475569" marker-end="url(#are2)" fill="none"/>
  <path d="M638 93 L681 93" stroke="#475569" marker-end="url(#are2)" fill="none"/>
  <path d="M638 147 L681 147" stroke="#475569" marker-end="url(#are2)" fill="none"/>
</svg>
```

### M3 · 通道激活

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 841 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="are3" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="841" height="198" fill="#ffffff"/>

  <rect x="20" y="51" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="55" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="72" font-size="10" fill="#374151" text-anchor="middle">task_q[l] · FIFO 16 项 · 1R</text>
  <rect x="20" y="105" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="109" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="126" font-size="10" fill="#374151" text-anchor="middle">active_ctx · FF 每通道每侧 1 份 · 1RW</text>
  <rect x="645" y="42" width="176" height="114" fill="#f1f5f9" stroke="#334155"/>
  <rect x="645" y="42" width="176" height="18" fill="#334155"/>
  <text x="733" y="55" font-size="10.5" fill="#ffffff" text-anchor="middle">LANE_CTX</text>
  <text x="733" y="82" font-size="10" fill="#334155" text-anchor="middle">task_id[5:0]</text>
  <text x="733" y="104" font-size="10" fill="#334155" text-anchor="middle">cur_addr</text>
  <text x="733" y="126" font-size="10" fill="#334155" text-anchor="middle">remain[15:0]</text>
  <text x="733" y="148" font-size="10" fill="#334155" text-anchor="middle">boundary</text>
  <rect x="232" y="20" width="369" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M3</text>
  <text x="587" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">TaskQueue · 装载 Active Context</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. active_ctx[l].free → active_ctx[l] = task_q[l].pop()</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. cur_addr = 起始地址；remain = cnt；boundary = 任务边界表</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. 同一侧内按序激活，不乱序</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. issue_done → active_ctx[l] 释放，本拍即可装下一个</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">每通道每侧各一份，彼此不等</text>
  <path d="M188 72 L231 72" stroke="#475569" marker-end="url(#are3)" fill="none"/>
  <path d="M188 126 L231 126" stroke="#475569" marker-end="url(#are3)" fill="none"/>
  <path d="M601 99 L644 99" stroke="#475569" marker-end="url(#are3)" fill="none"/>
</svg>
```

### M4 · 读侧发请求

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 913 218" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="are4" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="913" height="218" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">LANE_CTX</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">cur_addr</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">remain[15:0]</text>
  <rect x="20" y="102" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="106" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="123" font-size="10" fill="#374151" text-anchor="middle">in_buf · FIFO 8 KB · 1W</text>
  <rect x="20" y="156" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="160" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="177" font-size="10" fill="#374151" text-anchor="middle">out_buf · FIFO 8 KB · 1W</text>
  <polygon points="727,38 893,38 883,96 717,96" fill="#f8fafc" stroke="#374151"/>
  <text x="805" y="57" font-size="10.5" fill="#374151" text-anchor="middle">cmem_rd / mmem_rd</text>
  <text x="805" y="75" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · req_addr</text>
  <text x="805" y="93" font-size="9.5" fill="#6b7280" text-anchor="middle">rsp_valid · rsp_rdata</text>
  <rect x="717" y="108" width="176" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="717" y="108" width="176" height="18" fill="#334155"/>
  <text x="805" y="121" font-size="10.5" fill="#ffffff" text-anchor="middle">RD_DONE</text>
  <text x="805" y="148" font-size="10" fill="#334155" text-anchor="middle">issue_done</text>
  <text x="805" y="170" font-size="10" fill="#334155" text-anchor="middle">task_id[5:0]</text>
  <rect x="232" y="20" width="441" height="178" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M4</text>
  <text x="659" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="250" y="56" font-size="12" fill="#111827">读侧 · AGCU 生成地址并发读</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. lead_ok = in_buf/out_buf 的 credit 够 &amp;&amp; 在飞读数 &lt; outstanding 上限</text>
  <text x="262" y="98" font-size="10.5" fill="#475569">&amp;&amp; 已保留的任务边界数未满</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">2. lead_ok → {cmem_rd|mmem_rd}.req = {addr=cur_addr, 256 B}</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">3. cur_addr += 256；remain −= 256</text>
  <text x="250" y="158" font-size="10.5" fill="#475569">4. remain == 0 → issue_done = 1</text>
  <text x="250" y="182" font-size="10" fill="#9ca3af">拍数 = ceil(byte_count / 256) 加存储读延迟</text>
  <path d="M188 55 L231 55" stroke="#475569" marker-end="url(#are4)" fill="none"/>
  <path d="M188 123 L231 123" stroke="#475569" marker-end="url(#are4)" fill="none"/>
  <path d="M188 177 L231 177" stroke="#475569" marker-end="url(#are4)" fill="none"/>
  <path d="M673 67 L721 67" stroke="#475569" marker-end="url(#are4)" fill="none"/>
  <path d="M673 143 L716 143" stroke="#475569" marker-end="url(#are4)" fill="none"/>
</svg>
```

### M5 · 中间 Buffer

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 900 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="are5" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="900" height="198" fill="#ffffff"/>

  <polygon points="30,24 188,24 178,64 20,64" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="43" font-size="10.5" fill="#374151" text-anchor="middle">cmem_rd / mmem_rd</text>
  <text x="104" y="61" font-size="9.5" fill="#6b7280" text-anchor="middle">rsp_valid · rsp_rdata</text>
  <rect x="20" y="76" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="80" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="97" font-size="10" fill="#374151" text-anchor="middle">in_buf · FIFO 8 KB · 1W1R</text>
  <rect x="20" y="130" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="134" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="151" font-size="10" fill="#374151" text-anchor="middle">out_buf · FIFO 8 KB · 1W1R</text>
  <rect x="704" y="53" width="176" height="92" fill="#f1f5f9" stroke="#334155"/>
  <rect x="704" y="53" width="176" height="18" fill="#334155"/>
  <text x="792" y="66" font-size="10.5" fill="#ffffff" text-anchor="middle">BUF_ST</text>
  <text x="792" y="93" font-size="10" fill="#334155" text-anchor="middle">data[2047:0]</text>
  <text x="792" y="115" font-size="10" fill="#334155" text-anchor="middle">task_id[5:0]</text>
  <text x="792" y="137" font-size="10" fill="#334155" text-anchor="middle">boundary · drained</text>
  <rect x="232" y="20" width="428" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M5</text>
  <text x="646" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="250" y="56" font-size="12" fill="#111827">中间 Buffer · 吸收两端速度差</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. rsp_valid → buf.push({data, task_id, 有效字节, 是否任务边界})</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. buf.full → 进方向拉低 tready 反压 Router；出方向停止 RD 发请求</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. RD 领先 WR 的距离只由 buffer credit 限制，不要求同一 Active Context</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. 该 task 的数据全部被 WR 取走 → drained = 1</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">每条通道一份 8 KB，最大掩盖 32 T 延迟</text>
  <path d="M188 44 L231 44" stroke="#475569" marker-end="url(#are5)" fill="none"/>
  <path d="M188 97 L231 97" stroke="#475569" marker-end="url(#are5)" fill="none"/>
  <path d="M188 151 L231 151" stroke="#475569" marker-end="url(#are5)" fill="none"/>
  <path d="M660 99 L703 99" stroke="#475569" marker-end="url(#are5)" fill="none"/>
</svg>
```

### M6 · 写侧取数写出

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 904 254" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="are6" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="904" height="254" fill="#ffffff"/>

  <rect x="20" y="81" width="168" height="92" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="81" width="168" height="18" fill="#334155"/>
  <text x="104" y="94" font-size="10.5" fill="#ffffff" text-anchor="middle">BUF_ST</text>
  <text x="104" y="121" font-size="10" fill="#334155" text-anchor="middle">data[2047:0]</text>
  <text x="104" y="143" font-size="10" fill="#334155" text-anchor="middle">task_id[5:0]</text>
  <text x="104" y="165" font-size="10" fill="#334155" text-anchor="middle">boundary</text>
  <polygon points="718,20 884,20 874,78 708,78" fill="#f8fafc" stroke="#374151"/>
  <text x="796" y="39" font-size="10.5" fill="#374151" text-anchor="middle">out_core_data_ch</text>
  <text x="796" y="57" font-size="9.5" fill="#6b7280" text-anchor="middle">tvalid · tdata · tlast</text>
  <text x="796" y="75" font-size="9.5" fill="#6b7280" text-anchor="middle">vc_id[1:0] · tready</text>
  <polygon points="718,90 884,90 874,148 708,148" fill="#f8fafc" stroke="#374151"/>
  <text x="796" y="109" font-size="10.5" fill="#374151" text-anchor="middle">cmem_wr / mmem_wr</text>
  <text x="796" y="127" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · req_wdata</text>
  <text x="796" y="145" font-size="9.5" fill="#6b7280" text-anchor="middle">req_ready</text>
  <rect x="708" y="160" width="176" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="708" y="160" width="176" height="18" fill="#334155"/>
  <text x="796" y="173" font-size="10.5" fill="#ffffff" text-anchor="middle">WR_DONE</text>
  <text x="796" y="200" font-size="10" fill="#334155" text-anchor="middle">issue_done · drained</text>
  <text x="796" y="222" font-size="10" fill="#334155" text-anchor="middle">task_id[5:0]</text>
  <rect x="232" y="48" width="432" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="64" font-size="8.5" fill="#6b7280">M6</text>
  <text x="650" y="64" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="250" y="84" font-size="12" fill="#111827">写侧 · 按任务边界写目标</text>
  <text x="250" y="106" font-size="10.5" fill="#475569">1. 出口按 route 固化：Router TX、CoreMem Egress、DMA WR1 三选一</text>
  <text x="250" y="126" font-size="10.5" fill="#475569">2. buf 非空 &amp;&amp; 目标 ready → 发一拍 256 B</text>
  <text x="250" y="146" font-size="10.5" fill="#475569">3. Router→CM 与 MM→CM 争 CM 写路径时，未获选的一侧保持 valid 与上下文</text>
  <text x="250" y="166" font-size="10.5" fill="#475569">4. 该 task 最后一拍发出 → issue_done = 1；写响应回齐 → drained = 1</text>
  <text x="250" y="190" font-size="10" fill="#9ca3af">WR1 的 route mask 只允许 CoreMem</text>
  <path d="M188 127 L231 127" stroke="#475569" marker-end="url(#are6)" fill="none"/>
  <path d="M664 49 L712 49" stroke="#475569" marker-end="url(#are6)" fill="none"/>
  <path d="M664 119 L712 119" stroke="#475569" marker-end="url(#are6)" fill="none"/>
  <path d="M664 195 L707 195" stroke="#475569" marker-end="url(#are6)" fill="none"/>
</svg>
```

### M7 · Completion RS Join

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 918 246" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="are7" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="918" height="246" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">RD_DONE</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">issue_done</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">task_id[5:0]</text>
  <rect x="20" y="102" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="102" width="168" height="18" fill="#334155"/>
  <text x="104" y="115" font-size="10.5" fill="#ffffff" text-anchor="middle">WR_DONE</text>
  <text x="104" y="142" font-size="10" fill="#334155" text-anchor="middle">drained</text>
  <text x="104" y="164" font-size="10" fill="#334155" text-anchor="middle">task_id[5:0]</text>
  <rect x="20" y="184" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="188" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="205" font-size="10" fill="#374151" text-anchor="middle">comp_rs · FF 16 项 · 1RW</text>
  <rect x="722" y="102" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="726" y="106" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="810" y="123" font-size="10" fill="#374151" text-anchor="middle">done_pend · FIFO 16 项 · 1W</text>
  <rect x="232" y="44" width="446" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="60" font-size="8.5" fill="#6b7280">M7</text>
  <text x="664" y="60" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="80" font-size="12" fill="#111827">Completion RS · 两侧都齐才算完</text>
  <text x="250" y="102" font-size="10.5" fill="#475569">1. rd_done[task_id] |= RD 侧 drained；wr_done[task_id] |= WR 侧 drained</text>
  <text x="250" y="122" font-size="10.5" fill="#475569">2. join = rd_done &amp;&amp; wr_done</text>
  <text x="250" y="142" font-size="10.5" fill="#475569">3. join &amp;&amp; task_last → done_pend.push({stream_id, task_id})</text>
  <text x="250" y="162" font-size="10.5" fill="#475569">4. 同一拍多个 join 全部写入 done_pend，不覆盖不丢失</text>
  <text x="250" y="186" font-size="10" fill="#9ca3af">task_last 之外的分片完成后不通知 TS</text>
  <path d="M188 55 L231 55" stroke="#475569" marker-end="url(#are7)" fill="none"/>
  <path d="M188 137 L231 137" stroke="#475569" marker-end="url(#are7)" fill="none"/>
  <line x1="188" y1="205" x2="228" y2="205" stroke="#475569" marker-end="url(#are7)"/>
  <path d="M678 123 L721 123" stroke="#475569" marker-end="url(#are7)" fill="none"/>
</svg>
```

### M8 · Done Pending 与 TS 握手

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 919 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="are8" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="919" height="198" fill="#ffffff"/>

  <rect x="20" y="78" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="82" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="99" font-size="10" fill="#374151" text-anchor="middle">done_pend · FIFO 16 项 · 1R</text>
  <polygon points="733,69 899,69 889,127 723,127" fill="#f8fafc" stroke="#374151"/>
  <text x="811" y="88" font-size="10.5" fill="#374151" text-anchor="middle">dsa_done</text>
  <text x="811" y="106" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · stream_id[3:0]</text>
  <text x="811" y="124" font-size="9.5" fill="#6b7280" text-anchor="middle">task_id[5:0]</text>
  <rect x="232" y="20" width="447" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M8</text>
  <text x="665" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">Done Pending · exactly-once 上报</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. done_pend 非空 &amp;&amp; !no_ack → dsa_done = {valid=1, stream_id, task_id}</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. 发出后 done_pend.pop()，同一 task_id 不再上报第二次</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. no_ack 置位的任务直接出队，不回 Ack</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. reduce task：本路只代表搬运完成，TS 侧执行 consume_only</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">把任务置 FINISH 的权力在 Router 的 Reduce Done</text>
  <path d="M188 99 L231 99" stroke="#475569" marker-end="url(#are8)" fill="none"/>
  <path d="M679 98 L727 98" stroke="#475569" marker-end="url(#are8)" fill="none"/>
</svg>
```

### M9 · PendingTaskQ

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 927 262" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="are9" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="927" height="262" fill="#ffffff"/>

  <polygon points="30,20 188,20 178,78 20,78" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="39" font-size="10.5" fill="#374151" text-anchor="middle">dsa_cfg</text>
  <text x="104" y="57" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · req_addr[11:0]</text>
  <text x="104" y="75" font-size="9.5" fill="#6b7280" text-anchor="middle">req_wdata[31:0] · req_ready</text>
  <rect x="20" y="90" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="94" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="111" font-size="10" fill="#374151" text-anchor="middle">rtab_copy · FF 64 项 · 1R</text>
  <rect x="20" y="144" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="148" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="165" font-size="10" fill="#374151" text-anchor="middle">reduce_credit · FF 16 项 · 1RW</text>
  <rect x="20" y="198" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="202" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="219" font-size="10" fill="#374151" text-anchor="middle">stream_cache · FF 3×16 项 · 1R1W</text>
  <rect x="731" y="42" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="735" y="46" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="819" y="63" font-size="10" fill="#374151" text-anchor="middle">pending_taskq · FIFO 16 项 · 1W1R</text>
  <rect x="731" y="96" width="176" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="731" y="96" width="176" height="18" fill="#334155"/>
  <text x="819" y="109" font-size="10.5" fill="#ffffff" text-anchor="middle">HDR_CMT</text>
  <text x="819" y="136" font-size="10" fill="#334155" text-anchor="middle">route[1:0]</text>
  <text x="819" y="158" font-size="10" fill="#334155" text-anchor="middle">task_id · stream_id</text>
  <rect x="232" y="25" width="455" height="212" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="41" font-size="8.5" fill="#6b7280">M9</text>
  <text x="673" y="41" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="250" y="61" font-size="12" fill="#111827">PendingTaskQ · 出核任务先拿授权</text>
  <text x="250" y="83" font-size="10.5" fill="#475569">1. e = rtab_copy[path_id]，取这条 path 走哪个 VC、是不是 Reduce</text>
  <text x="250" y="103" font-size="10.5" fill="#475569">2. 查 vc_credit[e.vc] 够不够整包；下游 stream / Rmem 资源不查，TS 下发前已拿到</text>
  <text x="250" y="123" font-size="10.5" fill="#475569">3. reduce 包另要求 reduce_credit[user] ≥ 整包 flit 数</text>
  <text x="250" y="143" font-size="10.5" fill="#475569">4. 不够 → pending_taskq.push(desc)，不进 M2；router_credit 到 → 出队进 M2</text>
  <text x="250" y="163" font-size="10.5" fill="#475569">5. stream_credit_vld 回来时按 action 更新 stream_cache；重发前本地先记账</text>
  <text x="250" y="187" font-size="10" fill="#9ca3af">反压只落在出核这条链，进核的 Commit 资源不受影响</text>
  <text x="250" y="205" font-size="10" fill="#9ca3af">pending_taskq 满 → dsa_cfg.req_ready = 0；stream_cache 只跟随，不分配</text>
  <path d="M188 49 L231 49" stroke="#475569" marker-end="url(#are9)" fill="none"/>
  <path d="M188 111 L231 111" stroke="#475569" marker-end="url(#are9)" fill="none"/>
  <path d="M188 165 L231 165" stroke="#475569" marker-end="url(#are9)" fill="none"/>
  <path d="M188 219 L231 219" stroke="#475569" marker-end="url(#are9)" fill="none"/>
  <path d="M687 63 L730 63" stroke="#475569" marker-end="url(#are9)" fill="none"/>
  <path d="M687 131 L730 131" stroke="#475569" marker-end="url(#are9)" fill="none"/>
</svg>
```

***

## 7　参数汇总

```
物理通道           5（进核 `in_ch` 1 条 + 出核 `out_ch[0..3]` 4 条，与 4 个 VC 一一对应）；每条再拆读、写两侧
TaskQueue          每通道每侧不少于 16 项
中间 Buffer        每条通道一份 8 KB，256 B × 32 项，最大掩盖 32 T 延迟
Completion RS      16 项（待定）；Done Pending 16 项（待定）
与 Cmem 接口宽度    256 B/T，双向（DTE MAS 与 Cmem MAS 一致）
与 Mmem 接口宽度    256 B，双向；写 9T、读 8T
与 Router 接口宽度  256 B，双向（看不到 scale）
Hmem               288 B = 16 项 × {core_mask 2 B, sw_header 16 B}，按 stream_id 索引（B core / R core 改存 Core Mem）
stream_cache       3 方向 × 16 项 × {valid, user_id}，Router 那张 stream 表的只读副本
Fast LUT           64 项 × {valid, length, ctrl_flags}，按 task_id 索引；配合 User Base Register 直接拼出 task descriptor
DTE Setup Time     目标 < 10T：Fast LUT 命中 4T，未命中 Core Latency + 4T
单任务最大搬运量    32 KB（256 B × 128 拍）
内部启动延迟        85T（流水启动 5T + 50 条指令算地址 75T + core 发射 5T）
MSG 包结构          包头标记 2 B + Router 信息 4 B（path_id 1 B + path_core_mask 2 B + rsv 1 B）+ 包长度 2 B + 软件辅助信息 0～16 B + 业务数据 0～(64 K − 24) B
                   reduce 包另在硬件字段里带 reduce_seq 6 bit，由 DTE 发出时按 stream 内的第几笔 reduce 打上
包长范围            最短 16 B，最长 64 KB、实际支持到 (16 K + 32) B；不设包尾，结束靠包长度计数
reduce 包           软件辅助信息固定 16 B，Router 做加法时跳过这 16 B
scale 长度          data_len / 32；topK 长度 router_ep_count × 6 B，每 stream 上限 256 B
```

***

## 8　机制覆盖

| 机制 | 功能 | 用例 |
| - | - | - |
| Header 首拍锁存与六类字段检查 | F1、F2 | `header_check` |
| 一帧一任务，不允许任务间交织 | F4 | `one_frame_one_task` |
| 靠上一帧 TLAST 判断下一拍是新 Header | F5 | `frame_boundary` |
| TKEEP 累计与 byte_count 比较 | F7 | `tkeep_count` |
| 非法 Header 进 Drop Frame，只消费到 TLAST | F8 | `drop_frame` |
| Commit 配对接纳：三样同时拿到才接纳 | F9、F10 | `commit_pairing` |
| 进核任务的 stream_id 取自包头，task_id 按 path_id 查表 | F3a | `inbound_ids` |
| 进核那一路按帧号认帧，同 path 的几个包不串 | F3b | `frame_seq_tag` |
| 写 Trigger 那一拍采样四个直连身份信号 | F14a | `trigger_samples_ids` |
| 一笔配置写在被收下之前保持同一个序号 | F14b | `cfg_seq_stable` |
| DMA_XBAR 轮转仲裁五个通道对一块存储的访问 | F55a | `dma_xbar_arbitration` |
| 四个出核通道轮转仲裁 Router 那一个口，一个包不被插断 | F55b | `out_arb_frame` |
| 出核包在 Commit 建好，读回的数据排进它的 payload | F44a | `outbound_packing` |
| 双 Bank，Bank0 优先，竞争时优先 Router | F12 | `commit_bank_priority` |
| 必须最后写 Trigger，PACK 随之自动写入 | F14 | `trigger_order` |
| 通道之间乱序，通道内读写两半独立 | F17 | `channel_ooo` |
| 通道内顺序激活，向 TS 反馈按下发顺序 | F18 | `channel_inorder` |
| issue_done 就允许该侧走下一个任务 | F19 | `issue_done_release` |
| reduce 包软件辅助信息固定 16 B | F71 | `reduce_hdr_fixed_16b` |
| reduce 包出核打 reduce_seq，ack 原样带回 | F74 | `reduce_seq_stamp` |
| Router 送 DTE 时不剥离任何数据 | F72 | `no_strip_from_router` |
| 包头走标量 load / store，数据走 DMA | F73 | `header_vs_data_path` |
| read-ahead 领先量的三项约束 | F20 | `read_ahead_limit` |
| Router→CM 与 MM→CM 竞争 CM 写路径 | F21 | `cm_write_contend` |
| 单任务上限 32 KB，超过拆成多个任务包 | F23 | `task_split_32k` |
| Buffer 满时经 TREADY 反压 Router | F25 | `buffer_backpressure` |
| Completion RS 按 Commit 分配的内部序号 Join | F27 | `completion_join` |
| 同拍多个 Join 全部写入 Done Pending，不丢失 | F28 | `join_serialize` |
| 向 TS exactly-once | F29 | `exactly_once` |
| 六个完成层级 | F30 | `completion_levels` |
| task_last 才通知 TS，no_ack 不回 Ack | F31 | `task_last_ack` |
| 三条数据流各自的通路与出口绑定 | F32～F34 | `three_flows` |
| MM → CM 的 route mask 只允许 CoreMem | F34 | `wr1_route_mask` |
| topK 走 cmem_wr 写进 Core Mem 的 topK 区 | F37 | `topk_to_cmem` |
| 一个包进核拆成四份分开存 | F36、F39 | `packet_split_four` |
| 包头两张表合并成 288 B，按 stream_id 索引 | F38 | `hmem_merged` |
| path_id 由 TS 直连、size 由 RV core 配，不再有查找表 | F40 | `no_lut_table` |
| data_len 在不同方向盖的范围不同 | F42 | `data_len_scope` |
| hw_header_op 决定存不存包头 | F43 | `hw_header_op` |
| 出核改写 path_id / size / core_mask | F41、F44 | `header_rewrite` |
| 进核搬运的落点取自包头，发方在出核造包时写进去 | F3c、F44c | `dst_from_header` |
| B core 与 R core 上进核那一笔不回 Ack | F3d | `inbound_no_ack` |
| DPU 的 gpu_id 与 token_id 随数据出核 | F44b | `dpu_header_relay` |
| 纯包头任务 data_len = 0 | F45 | `header_only_task` |
| 软件只配基址，硬件用 stream_id 算偏移，进核出核都按这条算 | F46、F47 | `stream_offset` |
| scale 与 topK 的长度硬件自己算 | F48 | `derived_length` |
| Matrix Mem 侧不加偏移，Core Mem 侧加 | F49 | `mm_no_offset` |
| shareMem 写：搬入置 valid、搬出置 invalid | F50、F51 | `sharemem_flag` |
| 搬入那一笔的标志项按落点算，不由软件配 | F51a | `inbound_flag_index` |
| 三份 RouterTable 副本一致 | F52 | `dte_rtab_copy` |
| 出核前置申请，没拿到就在 PendingTaskQ 等 | F53 | `dte_pending_taskq` |
| PendingTaskQ 排在 Commit 之前，等资源的任务不占 Completion RS | F54 | `pending_before_commit` |
| PendingTaskQ 满只反压出核这条链，不影响进核 | F55 | `pending_full_backpressure` |
| Reduce credit 够整包才发 | F56、F57 | `dte_reduce_credit` |
| ReduceModule 每完成一次 Reduce 还一个 credit | F58 | `reduce_credit_release` |
| 出方向 4 个 VC buffer，单 VC 阻塞不影响其他 | F59 | `dte_vc_buffers` |
| 进方向单 VC，搬完按 flit 释放 VC credit | F60 | `dte_single_vc_in` |
| 两类业务层 credit 都分方向，先查 routing table | F61 | `credit_by_direction` |
| stream_cache 只跟随不分配，按 action 更新 | F62、F63 | `credit_release_action` |

***

## 9　取舍

* **为什么一个任务要拆成读写两半**
  * 搬运的两端节奏不同：Router 侧什么时候来数据由上游决定，存储侧要抢 bank
  * 绑成一体，任一端卡住另一端就空转；拆开之后两端各自按自己的节奏排队，中间 buffer 吸收速度差
  * 代价是要额外维护配对关系和 Join 逻辑
* **为什么 Commit 必须两侧同时拿到资源**
  * 只拿到读侧就开始收数据，数据进了 buffer 却没有写侧上下文可以落地，buffer 会被一个无法推进的任务占住
  * 两侧同时拿到才接纳，把这种半途卡死挡在入口
* **为什么通道按 VC 切、出核四份进核一份**
  * 早期方案是按存储资源切：Matrix Mem、Core Mem、Router 各看作一个读写 Resource，理想情况 3 条通路，代价是 XBar 面积近似 `N_in × N_out × W`，偏大；带宽从来不是约束（需求 190～320 GB/s，而 Matrix Mem 8192 GB/s、Core Mem 512 GB/s、Router 双向各 256 GB/s），所以按物理相邻关系并成进核、出核两条
  * 但两条挡不住死锁：出核任务共用一条出口，某个 VC 的 credit 耗尽会把排在后面、走别的 VC 的任务一起堵死
  * 现在把出核那条按 VC 复制成四份，一份对一个 VC，通道之间可以乱序推进，一个 VC 阻塞只影响自己那条；进核方向没有这个问题，仍是一条
  * `MM → CM` 不占 VC，固定复用 `out_ch[3]`，在目的端 MUX 到 Core Mem
* **为什么 `issue_done` 就允许该侧走下一个任务**
  * 一侧占的资源是 Active Context，不是在途事务。最后一个请求发出后这一侧本身已经空出来，剩下的响应排空由 Completion RS 按 `task_id` 跟踪
  * 若等到全部 drain 才放行，外部响应延迟会直接算进通道的占用时间
* **为什么一个包进核要拆成四份分开存**
  * 四份的读者不同：data 与 scale 给 MU 和 VU 算，topK 由 MU 自己从 Core Mem 读回来查专家，包头只在这个 token 再出核时用来重写路由
