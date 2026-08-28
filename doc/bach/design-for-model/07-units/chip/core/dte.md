# DTE DSA

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **DTE DSA**

给实现 DTE 的人：八个独立打拍的模块各自做哪些事、端口与存储怎么定。

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

| Route | 用哪对 Lane | 说明 |
| - | - | - |
| Router → MM | RD_CH0 + WR_CH0（inbound） | Header Parser 解析后 Commit 成对建立 |
| Router → CM | RD_CH0 + WR_CH0（inbound） | 同上 |
| MM → Router | RD_CH1 + WR_CH1（outbound） | 出口是 Router TX |
| CM → Router | RD_CH1 + WR_CH1（outbound） | 出口是 Router TX |
| MM → CM | RD_CH1 + WR_CH1（outbound） | 出口切到 DMA WR1，硬件 route mask 只允许 CoreMem |
| CM → MM | — | 本版本不支持，XBar 不提供 CH1 write ctrl 到 Matrix Memory 的连接 |

任务从两个入口来，都在 Commit 边界汇成同一套内部任务模型：Router 入站帧的 Header 经 Header Parser 生成 Descriptor；DTE RV core 经寄存器写加 Doorbell 生成 Descriptor。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1620 1040" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
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
  <rect x="0" y="0" width="1620" height="1040" fill="#ffffff"/>
  <text x="20" y="26" font-size="12" fill="#111827">DTE DSA · 第 0 层（八个独立打拍的模块；两个物理 Channel 拆成四条 Lane）</text>
  <text x="559" y="26" font-size="9.5" fill="#6b7280">一个高层任务被劈成 RD / WR 两个子上下文，各自排队各自推进，完成时按 task_id 合回一次 task_done</text>
  <polygon points="36,132 196,132 187,162 27,162" fill="#f8fafc" stroke="#374151"/>
  <text x="112" y="151" font-size="9" fill="#374151" text-anchor="middle">in_core_data_ch</text>
  <polygon points="36,60 206,60 197,90 27,90" fill="#f8fafc" stroke="#374151"/>
  <text x="117" y="79" font-size="9" fill="#374151" text-anchor="middle">dsa_cfg（RV core）</text>
  <polygon points="36,700 196,700 187,730 27,730" fill="#f8fafc" stroke="#374151"/>
  <text x="112" y="719" font-size="9" fill="#374151" text-anchor="middle">out_core_data_ch</text>
  <rect x="252" y="104" width="320" height="186" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="264" y="125" font-size="11" fill="#111827">Header Parser</text>
  <text x="264" y="142" font-size="8.5" fill="#475569">首拍锁存 Header，检查 opcode / route、长度、身份字段与帧格式</text>
  <text x="264" y="155.5" font-size="8.5" fill="#475569">逻辑字段与检查：version / header_len · packet_type / route</text>
  <text x="264" y="169.0" font-size="8.5" fill="#475569">　dst_addr（在目的端范围内、满足对齐）· byte_count</text>
  <text x="264" y="182.5" font-size="8.5" fill="#475569">　task_id / stream_id（未完成上下文中不得重复占用）· attributes</text>
  <text x="264" y="196.0" font-size="8.5" fill="#475569">生成一个高层 Router 入站 Descriptor 请求 Commit</text>
  <text x="264" y="209.5" font-size="8.5" fill="#475569">一帧一任务：同一 Frame 只属于一个 Router→MM / Router→CM 任务</text>
  <text x="264" y="223.0" font-size="8.5" fill="#475569">首拍固定为 Header：靠“上一帧 TLAST 已接受”判断下一拍是新 Header</text>
  <text x="264" y="236.5" font-size="8.5" fill="#475569">非法 Header 进 Drop Frame：不生成 Descriptor、不发存储器请求，</text>
  <text x="264" y="250.0" font-size="8.5" fill="#475569">　只消费到 TLAST 以恢复帧边界</text>
  <rect x="618" y="104" width="340" height="206" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="630" y="125" font-size="11" fill="#111827">Commit（配对接纳）</text>
  <text x="630" y="142" font-size="8.5" fill="#475569">一个高层任务必须同时拿到三样：</text>
  <text x="630" y="155.5" font-size="8.5" fill="#475569">　1. 目标 RD Lane 的 TaskQueue 项</text>
  <text x="630" y="169.0" font-size="8.5" fill="#475569">　2. WR Lane 的 TaskQueue 项</text>
  <text x="630" y="182.5" font-size="8.5" fill="#475569">　3. Completion RS 项</text>
  <text x="630" y="196.0" font-size="8.5" fill="#475569">任一侧没有空间，Commit 整体保持，Header 入口向 Router 反压</text>
  <text x="630" y="209.5" font-size="8.5" fill="#475569">这条规则挡住“读已经开始、写还没有落脚点”的半任务</text>
  <text x="630" y="223.0" font-size="8.5" fill="#475569">同时完成地址展开：源地址、目的地址、按任务边界切分的元数据</text>
  <text x="630" y="236.5" font-size="8.5" fill="#475569">两个配置 Bank，Bank0 优先于 Bank1：</text>
  <text x="630" y="250.0" font-size="8.5" fill="#475569">　都空闲时 Router 的配置进 Bank0，RV core 的配置进 Bank1</text>
  <text x="630" y="263.5" font-size="8.5" fill="#475569">　只剩一个 Bank 而两者竞争时优先配置 Router 信息</text>
  <rect x="1004" y="104" width="280" height="206" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="1016" y="125" font-size="11" fill="#111827">TaskQueue ×4</text>
  <text x="1016" y="142" font-size="8.5" fill="#475569">每条 Lane 各自一个，深度 16（待评估）</text>
  <text x="1016" y="155.5" font-size="8.5" fill="#475569">按序激活：TaskQueue 按序装载为 Active Context</text>
  <text x="1016" y="169.0" font-size="8.5" fill="#475569">read-ahead 允许 RD / WR 任务序号错位，</text>
  <text x="1016" y="182.5" font-size="8.5" fill="#475569">　但不改变各 Lane 内的顺序</text>
  <text x="1016" y="196.0" font-size="8.5" fill="#475569">四条 Lane 的状态彼此独立：任一 Lane 的</text>
  <text x="1016" y="209.5" font-size="8.5" fill="#475569">　Active Context 释放后就能激活下一个任务，</text>
  <text x="1016" y="223.0" font-size="8.5" fill="#475569">　不等配对的那一条</text>
  <text x="1016" y="236.5" font-size="8.5" fill="#475569">issue_done 就允许该 Lane 提前激活下一任务</text>
  <rect x="252" y="346" width="300" height="150" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="264" y="367" font-size="11" fill="#111827">RD_CH0（inbound 读）</text>
  <text x="264" y="384" font-size="8.5" fill="#475569">从 CoreStation 收帧</text>
  <text x="264" y="397.5" font-size="8.5" fill="#475569">Payload 附带 task_id、有效字节与任务边界</text>
  <text x="264" y="411.0" font-size="8.5" fill="#475569">写入 inbound buffer</text>
  <text x="264" y="424.5" font-size="8.5" fill="#475569">Buffer 满时通过 TREADY 向 Router 反压</text>
  <text x="264" y="438.0" font-size="8.5" fill="#475569">TKEEP 按字节粒度生效，每个 Payload Fire</text>
  <text x="264" y="451.5" font-size="8.5" fill="#475569">　累计 TKEEP 有效字节，TLAST 时与</text>
  <text x="264" y="465.0" font-size="8.5" fill="#475569">　byte_count 比较</text>
  <rect x="600" y="346" width="190" height="150" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="612" y="367" font-size="11" fill="#111827">inbound buffer</text>
  <text x="612" y="384" font-size="8.5" fill="#475569">与 outbound 合计约 8 KB</text>
  <text x="612" y="397.5" font-size="8.5" fill="#475569">256 B × 20～30 拍</text>
  <text x="612" y="411.0" font-size="8.5" fill="#475569">最大可掩盖 32 T 延迟</text>
  <text x="612" y="424.5" font-size="8.5" fill="#475569">read-ahead 的领先量由</text>
  <text x="612" y="438.0" font-size="8.5" fill="#475569">　Buffer credit、读 outstanding</text>
  <text x="612" y="451.5" font-size="8.5" fill="#475569">　限额、可保留的任务边界数</text>
  <text x="612" y="465.0" font-size="8.5" fill="#475569">　共同约束</text>
  <rect x="838" y="346" width="300" height="150" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="850" y="367" font-size="11" fill="#111827">WR_CH0（inbound 写）</text>
  <text x="850" y="384" font-size="8.5" fill="#475569">从 inbound buffer 按任务边界取数</text>
  <text x="850" y="397.5" font-size="8.5" fill="#475569">经 DMA_XBAR 写入目标 Matrix Mem / Core Mem</text>
  <text x="850" y="411.0" font-size="8.5" fill="#475569">Router → MM：dst_addr 直给，不加 stream 偏移</text>
  <text x="850" y="424.5" font-size="8.5" fill="#475569">Router → CM：dst_addr = dst_base_addr</text>
  <text x="850" y="438.0" font-size="8.5" fill="#475569">　　+ stream_id × stream_stride</text>
  <text x="850" y="451.5" font-size="8.5" fill="#475569">写请求与响应 Drain 后进 Completion RS</text>
  <rect x="252" y="540" width="300" height="150" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="264" y="561" font-size="11" fill="#111827">RD_CH1（outbound 读）</text>
  <text x="264" y="578" font-size="8.5" fill="#475569">AGCU 生成源端读地址</text>
  <text x="264" y="591.5" font-size="8.5" fill="#475569">Read Ctrl 经 DMA_XBAR 读 MM / CM</text>
  <text x="264" y="605.0" font-size="8.5" fill="#475569">返回数据连同 task_id 与边界元数据</text>
  <text x="264" y="618.5" font-size="8.5" fill="#475569">　写入 outbound buffer</text>
  <text x="264" y="632.0" font-size="8.5" fill="#475569">MM 侧地址直给，CM 侧加 stream 偏移</text>
  <rect x="600" y="540" width="190" height="150" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="612" y="561" font-size="11" fill="#111827">outbound buffer</text>
  <text x="612" y="578" font-size="8.5" fill="#475569">同上，与 inbound 合计约 8 KB</text>
  <text x="612" y="591.5" font-size="8.5" fill="#475569">出口阻塞只通过 Credit</text>
  <text x="612" y="605.0" font-size="8.5" fill="#475569">　反压限制领先距离</text>
  <text x="612" y="618.5" font-size="8.5" fill="#475569">不要求读写用同一个</text>
  <text x="612" y="632.0" font-size="8.5" fill="#475569">　Active Context</text>
  <rect x="838" y="540" width="300" height="150" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="850" y="561" font-size="11" fill="#111827">WR_CH1（outbound 写）</text>
  <text x="850" y="578" font-size="8.5" fill="#475569">按固化的 Route 选出口：Router TX 或 CoreMem Egress</text>
  <text x="850" y="591.5" font-size="8.5" fill="#475569">Router TX 用 AXI-Stream 的 Valid / Ready / Keep / Last</text>
  <text x="850" y="605.0" font-size="8.5" fill="#475569">CoreMem Egress 用 DMA_XBAR 写握手</text>
  <text x="850" y="618.5" font-size="8.5" fill="#475569">MM → CM 时出口切到 DMA WR1，硬件 route mask</text>
  <text x="850" y="632.0" font-size="8.5" fill="#475569">　只允许 CoreMem，不会把 CH1 数据写回 Matrix Mem</text>
  <text x="850" y="645.5" font-size="8.5" fill="#475569">两种出口的响应与 Drain 条件统一送进 Completion RS</text>
  <rect x="1190" y="346" width="300" height="170" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="1202" y="367" font-size="11" fill="#111827">Completion RS</text>
  <text x="1202" y="384" font-size="8.5" fill="#475569">只在同一 task_id 的 RD 与 WR 两侧条件都满足时</text>
  <text x="1202" y="397.5" font-size="8.5" fill="#475569">　产生 task_done（Join）</text>
  <text x="1202" y="411.0" font-size="8.5" fill="#475569">同一拍多个 Join 命中时全部写入 Done Pending，</text>
  <text x="1202" y="424.5" font-size="8.5" fill="#475569">　不允许覆盖或丢失</text>
  <text x="1202" y="438.0" font-size="8.5" fill="#475569">六个完成层级：queued → active → issue_done</text>
  <text x="1202" y="451.5" font-size="8.5" fill="#475569">　→ drained → join_done → task_done</text>
  <text x="1202" y="465.0" font-size="8.5" fill="#475569">issue_done 只表示请求已发出，真正完成还要等</text>
  <text x="1202" y="478.5" font-size="8.5" fill="#475569">　写响应、读响应排空以及 outstanding 清零</text>
  <rect x="1190" y="556" width="300" height="134" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="1202" y="577" font-size="11" fill="#111827">Done Pending</text>
  <text x="1202" y="594" font-size="8.5" fill="#475569">深度 16（待定）</text>
  <text x="1202" y="607.5" font-size="8.5" fill="#475569">负责多个同拍 Join 的串行化</text>
  <text x="1202" y="621.0" font-size="8.5" fill="#475569">向 TS 的报告是 exactly-once</text>
  <text x="1202" y="634.5" font-size="8.5" fill="#475569">task_last 标记的那一笔完成后才通知 TS</text>
  <text x="1202" y="648.0" font-size="8.5" fill="#475569">no_ack 置位的任务不回 Ack</text>
  <polygon points="1200,730 1380,730 1371,760 1191,760" fill="#f8fafc" stroke="#374151"/>
  <text x="1286" y="749" font-size="9" fill="#374151" text-anchor="middle">dsa_done → TS</text>
  <rect x="252" y="760" width="350" height="200" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="264" y="781" font-size="11" fill="#111827">Hmem 与 LUT</text>
  <text x="264" y="798" font-size="8.5" fill="#475569">Hmem 16 KB + 32 B：</text>
  <text x="264" y="811.5" font-size="8.5" fill="#475569">　sw_header_table 16 stream × 64 task × 16 B = 16 KB</text>
  <text x="264" y="825.0" font-size="8.5" fill="#475569">　core_mask_table 16 项按 stream_id 索引 = 32 B</text>
  <text x="264" y="838.5" font-size="8.5" fill="#475569">硬件包头静态部分另有 64 项按 task_id 索引，boot 阶段配好；</text>
  <text x="264" y="852.0" font-size="8.5" fill="#475569">　动态部分 path_core_mask 2 B 由 DTE core 配</text>
  <text x="264" y="865.5" font-size="8.5" fill="#475569">LUT 192 B：path_id_table + task_len_table</text>
  <text x="264" y="879.0" font-size="8.5" fill="#475569">进核：只在需要分配新 stream_id 时才存包头（hw_header_op=1），</text>
  <text x="264" y="892.5" font-size="8.5" fill="#475569">　中间环节的 reduce 与 concat 任务直接丢弃</text>
  <text x="264" y="906.0" font-size="8.5" fill="#475569">出核：按 task_id 查出 path_id 与 size 改写进硬件包头</text>
  <text x="264" y="919.5" font-size="8.5" fill="#475569">　path_core_mask 在 core 内没有修改接口，软件包头不改</text>
  <text x="264" y="933.0" font-size="8.5" fill="#475569">支持纯包头任务（data_len = 0），进出 core 都可以</text>
  <rect x="640" y="760" width="400" height="200" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="652" y="781" font-size="11" fill="#111827">出核前的资源与流控</text>
  <text x="652" y="798" font-size="8.5" fill="#475569">RouterTable 副本：按 PathID 查到 VC 与资源需求，软件写，三方一致</text>
  <text x="652" y="811.5" font-size="8.5" fill="#475569">本级 Reduce credit 表：每用户一个 entry，flit 粒度</text>
  <text x="652" y="825.0" font-size="8.5" fill="#475569">　用户创建 Stream 资源时分配 credit 数量</text>
  <text x="652" y="838.5" font-size="8.5" fill="#475569">　发 Reduce 包前要求本级 credit 够整包，否则在 PendingTaskQ 等</text>
  <text x="652" y="852.0" font-size="8.5" fill="#475569">　ReduceModule 每完成一次 Reduce 并把 flit 发给下游就还一个</text>
  <text x="652" y="865.5" font-size="8.5" fill="#475569">PendingTaskQ：没申请到下游 Stream 或 Reduce 资源的任务在这里等</text>
  <text x="652" y="879.0" font-size="8.5" fill="#475569">出方向 VC buffer ×4：按 VC0～3 多线程调度，单 VC 阻塞只阻塞该 buffer</text>
  <text x="652" y="892.5" font-size="8.5" fill="#475569">进方向只用单个 VC 调度，多 VC 到单 VC 的映射由 Router 侧硬件固化</text>
  <text x="652" y="906.0" font-size="8.5" fill="#475569">两类业务层 credit 都分方向，先查 routing table 定方向再取 credit</text>
  <text x="652" y="919.5" font-size="8.5" fill="#475569">解析本级 Router 各方向传进来的 core credit release，按 action 决定</text>
  <text x="652" y="933.0" font-size="8.5" fill="#475569">　是否同步更新 core 内的 stream 表状态</text>
  <rect x="1080" y="760" width="220" height="200" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="1092" y="781" font-size="11" fill="#111827">DMA_XBAR</text>
  <text x="1092" y="798" font-size="8.5" fill="#475569">Core Mem 8 bank</text>
  <text x="1092" y="811.5" font-size="8.5" fill="#475569">Matrix Mem 64 bank</text>
  <text x="1092" y="825.0" font-size="8.5" fill="#475569">命中冲突就排队</text>
  <text x="1092" y="838.5" font-size="8.5" fill="#475569">CH0 与 CH1 目的资源</text>
  <text x="1092" y="852.0" font-size="8.5" fill="#475569">　无冲突时独立推进</text>
  <text x="1092" y="865.5" font-size="8.5" fill="#475569">共享端口时按 XBar</text>
  <text x="1092" y="879.0" font-size="8.5" fill="#475569">　仲裁规则</text>
  <text x="1092" y="892.5" font-size="8.5" fill="#475569">Router→CM 与 MM→CM</text>
  <text x="1092" y="906.0" font-size="8.5" fill="#475569">　竞争 CM 写路径，</text>
  <text x="1092" y="919.5" font-size="8.5" fill="#475569">　未获选 Lane 保持</text>
  <text x="1092" y="933.0" font-size="8.5" fill="#475569">　valid 与上下文</text>
  <polygon points="1400,790 1550,790 1541,820 1391,820" fill="#f8fafc" stroke="#374151"/>
  <text x="1471" y="809" font-size="9" fill="#374151" text-anchor="middle">cmem_rd / wr</text>
  <polygon points="1400,850 1550,850 1541,880 1391,880" fill="#f8fafc" stroke="#374151"/>
  <text x="1471" y="869" font-size="9" fill="#374151" text-anchor="middle">mmem_rd / wr</text>
  <polygon points="1400,910 1550,910 1541,940 1391,940" fill="#f8fafc" stroke="#374151"/>
  <text x="1471" y="929" font-size="9" fill="#374151" text-anchor="middle">smem_wr</text>
  <polyline points="196,147 224,147 224,152 252,152" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="206,75 652,75 652,104" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <text x="360" y="54" font-size="8.5" fill="#6b7280" text-anchor="start">RV core 写四个寄存器，最后写 Doorbell → 另一个任务入口</text>
  <polyline points="572,160 595,160 595,166 618,166" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="958,166 1004,166" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="1088,310 1088,326 378,326 378,346" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="1178,310 1178,520 378,520 378,540" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <text x="600" y="320" font-size="8.5" fill="#6b7280" text-anchor="middle">Commit 激活各 Lane 的 Active Context</text>
  <polyline points="552,406 600,406" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="790,406 838,406" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="552,600 600,600" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="790,600 838,600" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="868,690 868,715 196,715" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <text x="240" y="708" font-size="8.5" fill="#6b7280" text-anchor="start">出核数据经 CoreStation 发往 Router</text>
  <polyline points="1138,406 1164,406 1164,397 1190,397" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="1138,570 1164,570 1164,492 1190,492" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="1340,516 1340,556" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="1340,690 1340,710 1286,710 1286,730" fill="none" stroke="#0f766e" marker-end="url(#g)"/>
  <polyline points="1138,475 1160,475 1160,738 1124,738 1124,760" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="510,690 510,734 1102,734 1102,760" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="1300,808 1346,808 1346,805 1391,805" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="1300,868 1346,868 1346,865 1391,865" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="1300,928 1346,928 1346,925 1391,925" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="462,760 462,752 1024,752 1024,690" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#a)"/>
  <polyline points="760,760 760,706 928,706 928,690" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <text x="240" y="748" font-size="8.5" fill="#6b7280" text-anchor="start">Hmem / LUT 供 WR Lane 查表改写包头</text>
  <text x="20" y="996" font-size="10.5" fill="#374151">五种搬运方向：Router → MM、Router → CM 走 RD_CH0 + WR_CH0；MM → Router、CM → Router、MM → CM 走 RD_CH1 + WR_CH1。本版本不支持 CM → MM。</text>
  <text x="20" y="1020" font-size="10.5" fill="#374151">数据布局仅支持连续一维搬运，不支持 stride；单个 DTE 任务的搬运量上限 32 KB（256 B × 128 拍），超过的拆成多个任务包下发。</text>
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
| F4 | 一帧一任务：同一个 AXI-Stream Frame 只属于一个 Router → MM 或 Router → CM 任务，不允许任务间交织 |
| F5 | 首拍固定为 Header，靠“上一帧 TLAST 已接受”判断下一拍是新 Header，不依赖 Start-of-Frame 信号 |
| F6 | Header Commit 成功后才允许 Payload Fire；TLAST 标识最后一个 Payload beat；`byte_count` 为 0 时可由 Header beat 同时携带 TLAST |
| F7 | TKEEP 按字节粒度生效，每个 Payload Fire 累计 TKEEP 有效字节，TLAST 时与 `byte_count` 比较 |
| F8 | 非法 Header 进 Drop Frame 流程：不生成 Descriptor、不发存储器请求，只消费到 TLAST 以恢复帧边界 |

### Commit

| 编号 | 功能 |
| - | - |
| F9 | 一个高层任务必须同时拿到三样才接纳：目标 RD Lane 的 TaskQueue 项、WR Lane 的 TaskQueue 项、Completion RS 项 |
| F10 | 任一侧没有空间时 Commit 整体保持，Header 入口向 Router 反压。这条规则挡住“读已经开始、写还没有落脚点”的半任务 |
| F11 | 同时完成地址展开：源地址、目的地址、按任务边界切分的元数据都在这一步算好 |
| F12 | 两个配置 Bank，Bank0 优先于 Bank1：都空闲时 Router 的配置进 Bank0、RV core 的配置进 Bank1；只剩一个 Bank 而两者竞争时优先配置 Router 信息 |
| F13 | 两个任务入口在 Commit 边界汇成同一套内部任务模型 |
| F14 | RV core 侧的配置序列：用 `dsawi` / `dsaw` 写 `TASK_CFG_ADDR` 与 `TASK_CFG_TD` 两个寄存器，再写 Doorbell（`TASK_CFG_TRG`），`TASK_CFG_PACK` 随之自动写入。必须最后写 Doorbell |

### TaskQueue ×4 与 Lane ×4

| 编号 | 功能 |
| - | - |
| F15 | 两个物理 Channel（`inbound_ch` / ch0、`outbound_ch` / ch1）各拆成读写两半，一共四条 Lane：RD_CH0、WR_CH0、RD_CH1、WR_CH1 |
| F16 | 每条 Lane 各自拥有 TaskQueue（16 项，深度待评估）和 Active Context；一个高层任务落到一对 RD / WR 子上下文上 |
| F17 | 四条 Lane 的状态彼此独立：任一 Lane 的 Active Context 释放后就能从本 Lane 的 TaskQueue 激活下一个任务，不等配对的那一条 |
| F18 | 同一 Lane 内任务不乱序：TaskQueue 按序激活。read-ahead 允许 RD / WR 任务序号错位，但不改变各 Lane 内的顺序 |
| F19 | `issue_done` 就允许该 Lane 提前激活下一任务，不必等全部 drain |
| F20 | RD Lane 允许领先 WR Lane，领先量由三件事共同约束：中间 Buffer 的可用 Credit、读的 outstanding 限额、可保留的任务边界数 |
| F21 | 并发约束：CH0 与 CH1 目的资源无冲突时独立推进，共享 DMA_XBAR 端口时按其仲裁规则；Router → CM 与 MM → CM 竞争 CoreMem 写路径，由 CM 写仲裁器选择，未获选 Lane 保持 valid 和上下文 |
| F22 | 数据布局仅支持连续一维搬运，当前不支持 stride |
| F23 | 单个 DTE 任务的搬运量上限是 32 KB（256 B × 128 拍），比包上限小，超过的要拆成多个任务包下发；一个任务可由多个任务包组成，包之间不保证顺序执行 |

### 中间 Buffer

| 编号 | 功能 |
| - | - |
| F24 | inbound buffer 与 outbound buffer 合计约 8 KB，按 256 B × 20～30 拍算，最大可掩盖 32 T 的延迟 |
| F25 | Buffer 满时通过 TREADY 向 Router 反压 |
| F26 | 出口阻塞只通过 Credit 反压限制领先距离，不要求读写用同一个 Active Context |

### Completion RS 与 Done Pending

| 编号 | 功能 |
| - | - |
| F27 | 只在同一个 `task_id` 的 RD 与 WR 两侧条件都满足时产生 `task_done`（Join） |
| F28 | 同一拍多个 Join 命中时全部写入 Done Pending，不允许覆盖或丢失，由 Done Pending 负责串行化 |
| F29 | 向 TS 的报告是 exactly-once |
| F30 | 六个完成层级：`queued`（已进 TaskQueue 未装载）→ `active`（由对应 AGCU / Ctrl 执行）→ `issue_done`（该 Lane 最后一个请求已 Fire）→ `drained`（相关响应、Buffer 数据和外部副作用均已收敛）→ `join_done`（同一 task_id 的 RD 与 WR 都满足）→ `task_done`（进 Done Pending 并与 TS 成功握手） |
| F31 | `task_last` 标记一个 task 拆成几笔搬运时的最后一笔，只有带这个标记的那一笔完成后才通知 TS；`no_ack` 置位的任务不回 Ack |

### 三条数据流

| 编号 | 功能 |
| - | - |
| F32 | Inbound（Router → MM / CM）：Router 以 AXI-Stream 发 Header → Header Parser 生成 Descriptor → Commit 为 RD_CH0 与 WR_CH0 同时分配 → RD_CH0 控制 Payload 写入 inbound buffer，附带 task_id、有效字节与任务边界 → WR_CH0 从 buffer 按任务边界取数经 DMA_XBAR 写入目标存储 → 两侧按 task_id Join |
| F33 | Outbound（MM / CM → Router）：Commit 原子拆成 RD_CH1 与 WR_CH1 → RD_CH1 的 AGCU 生成源端读地址，Read Ctrl 经 DMA_XBAR 读取，把返回数据连同 task_id 和边界元数据写入 outbound buffer → WR_CH1 按固化的 Route 选 Router TX 或 CoreMem Egress，从 buffer 按任务边界发出 → 同一任务的两侧都完成且 buffer 里该任务的数据已排空后才生成 `task_done` |
| F34 | Inner（MM → CM）：完全使用 CH1。先锁定 Matrix Mem 为读源、Core Mem 为写目标；数据经 DMA_XBAR RD、`ch1_rd_ctrl` 和 outbound buffer 到达 `ch1_wr_ctrl`；CH1 的出口绑定此时选 DMA WR1 而不是 Router TX；WR1 的硬件 route mask 只允许 CoreMem，因此不会把 CH1 的数据写回 Matrix Mem |
| F35 | 存储读、Buffer 搬运和 WR1 写可以流水重叠，但每一级仍各自遵守 valid / ready |

### Hmem、LUT 与四类内容的存放

| 编号 | 功能 |
| - | - |
| F36 | 一次搬运的对象是一个 MSG 包，进了 core 就按内容拆成四份、各存各的地方：包头、scale、topK、data |
| F37 | 计算 core 上：硬件包头存 Hmem 的 `core_mask_table`（16 项按 stream_id 索引，共 32 B），软件包头存 `sw_header_table`（16 stream × 64 task × 16 B = 16 KB），scale 存 Core Mem 的 scale 区，topK 存 MU 内的 `topK_ep_table` 或 Core Mem 独立空间，data 存 Core Mem 按 stream 分片 |
| F38 | B core / R core 上：包头存 Core Mem 独立空间，scale 与 topK 与 data 在 Matrix Mem 里连排，由 GPU 侧按 pattern 排好序送来，DTE 不重排顺序 |
| F39 | 硬件包头的静态部分（`path_id` + `size`）另有 64 项按 `task_id` 索引，boot 阶段配好；动态部分 `path_core_mask` 2 B 由 DTE core 配 |
| F40 | LUT 192 B：`path_id_table` + `task_len_table` |
| F41 | `data_len` 在不同方向盖的范围不同：Router ↔ Matrix Mem 时是 topK + scale + data 的总长；Router ↔ Core Mem 时只是 data 的长度，scale 与 topK 的长度另算 |
| F42 | 进核时计算 core 只在这个 token 需要分配新 `stream_id` 时才存包头（`hw_header_op = 1`），中间环节的 reduce 与 concat 任务直接丢弃（`hw_header_op = 0`）；广播 token 进核必然带 topK，必须存下来 |
| F43 | 出核时 DTE 按 `task_id` 查出 `path_id` 与 `size` 改写进硬件包头；`path_core_mask` 在 core 内没有修改接口，软件包头不改；计算结果出核不带 topK |
| F44 | 支持纯包头任务（`data_len = 0`），进出 core 都可以 |
| F45 | 地址的一条规矩：软件只配基址，偏移由硬件用 `stream_id` 算出来。`stream_id` 是 TS 建 stream 表项时定的，随任务一起给到 DTE，软件不需要知道这个 token 落在 Core Mem 的哪一片 |
| F46 | 五类地址的算法：data 在 Core Mem 侧是 `base_addr + stream_id × stream_stride`、在 Matrix Mem 侧直接用 `src_addr` / `dst_addr` 不加偏移；硬件包头是 `header_base_addr + stream_id × 包头长度`；软件包头是基址 + `stream_id × 1 KB + task_id × 16 B`；scale 是 `scale_base_addr + stream_id × scale_stride`；topK 是 `topk_base_addr + stream_id × 256 B` |
| F47 | 两项搬运长度硬件自己算，不用软件配：scale 是 `data_len / 32`（32 个元素共用一个 scale），topK 是 `router_ep_count × 6 B`（每项 `{expert_id 2 B, weight 4 B}`，每 stream 上限 256 B） |
| F48 | Matrix Mem 一侧不加 stream 偏移，Core Mem 一侧加：Matrix Mem 放的是模型 weight 与按 pattern 排好序送来的 token，位置软件自己算准；Core Mem 按 stream 切成 16 片，谁占哪片由 TS 定，软件配的时候还不知道 |

### shareMem 写

| 编号 | 功能 |
| - | - |
| F49 | 任务数据传输完成后，按 `sharemem_waddr` / `sharemem_data` 写 shareMem，然后通知 TS |
| F50 | 只在 B core 与 R core 使用，存 user_id 与 token entry 的 valid 标志：token 搬入 Matrix Mem 后置 valid，搬出后置 invalid |

### 出核前的资源与流控

| 编号 | 功能 |
| - | - |
| F51 | DTE 内维护一份 RouterTable 副本，按 PathID 查到 VC 与资源需求；软件负责写入并保证与 Router、ReduceModule 三方一致 |
| F52 | 包对下游 Stream 或 Rmem 资源有需求时，DTE 必须先向 Router 申请到才能发，否则任务在 `PendingTaskQ` 等待 |
| F53 | 本级 Reduce credit 表：每用户一个 entry，flit 粒度。某个用户创建 Stream 资源时给这个用户分配一个 entry 的 credit 数量 |
| F54 | 搬 Reduce 包前先检查本级 Reduce credit 是否够整包，再在 VC credit 满足的前提下发到 ReduceModule |
| F55 | ReduceModule 每完成一次 Reduce 并把 flit 发给下游就释放一个 credit，经独立的释放通道把 Valid 加 UserID 送回 DTE |
| F56 | 出方向按 VC0～3 多线程调度维护多个 VC buffer，某个 VC 阻塞只阻塞对应的那个 buffer；用它吸收整包流量，完成 core 与 Router 之间的协议转换 |
| F57 | 进方向 Router 与 core 之间只用单个 VC 调度，多 VC 到单 VC 的映射由 Router 侧硬件固化完成；DTE 侧感知单 VC buffer 的缓存状态并据此启动搬运，解析包信息，搬完按 flit 释放 VC credit |
| F58 | 出去之前查两类业务层 credit：下游的 coremem credit 与 reduce credit。两类都分方向，要先查 routing table 确定方向，再取对应方向的 credit |
| F59 | credit 回程：解析本级 Router 各方向传进来的 core credit release，按其中的 action 信息决定是否同步更新 core 内的 stream 表状态 |

### 软件配置的寄存器

| 编号 | 功能 |
| - | - |
| F60 | `TASK_CFG_TD` 位域：`src_sel`（00=Router / 01=MM / 10=CM）、`dst_sel`、`queue_sel`（b0=ch0 / b1=ch1）、`cnt`（搬运总拍数，单任务最大 256 B × 128）、`mask`（256 B 一拍，按 32 B 粒度，位为 0 表示该 32 B 无效）、`path_id`、`task_pack_id`、`last` |
| F61 | `TASK_CFG_PACK` 位域：`UserID`、`stream_id`（等同 SlotID）、`task_id`（最大 64）、`no_ack`、`last`。打包信息只有在 `dst_sel` 是 Router 时才有效 |
| F62 | 按字段命名的软件寄存器分五组：模式与开关（`transfer_mode` / `scale_valid` / `topK_valid` / `hw_header_op` / `task_last` / `smem_valid` / `router_ep_count`）、数据的地址与长度、包头与 scale 与 topK 的地址、shareMem 表项、任务身份 |
| F63 | 不随任务变的控制与观测寄存器：`SYS_CTRL` / `DTE_CTRL`（时钟门控、软复位、任务启动触发、单步调试使能、清空缓冲）、`SYS_STATUS` / `DTE_STATUS`（Idle / Running / Error / Stop 与忙状态）、`EXCEPT_STATUS` / `EXCEPT_MASK`、`EXCEPT_CFG_*`（出异常时自动抓下当时的三个任务配置寄存器，只读）、`PMU_CTRL` 与 11 个 `PMU_CNT_*` |
| F64 | 异常四类：访存越界、非对齐、ECC 错、搬运异常。中断默认屏蔽，写 0 打开。本轮只留状态位与接口名，不实现行为 |

***

## 3　接口

```
port in_core_data_ch (slave, AXI-Stream-Like, clk)    // CoreStation → DTE：进 core 的整包
  in  tvalid · tdata[2047:0] · tkeep[255:0] · tlast · thdr
  out tready                                            // = inbound buffer 有空 且 Commit 三样资源都够
port out_core_data_ch (master, AXI-Stream-Like, clk)  // DTE → CoreStation：出 core 的整包
  out tvalid · tdata[2047:0] · tkeep[255:0] · tlast · thdr · vc_id[1:0]
  in  tready                                            // = 该 VC 的 Core 方向输入 VC 有空
port hdr_rd (master, AXI-Full 类, clk)                // DTE core 读 CoreStation 的包头，读完写 1 弹出
  out araddr[31:0] · arvalid · awaddr[31:0] · wvalid · wdata[31:0]
  in  arready · rvalid · rdata[255:0] · awready · wready
port router_credit (slave, 电平 + 脉冲, clk)          // Router 侧回来的三类信息
  in  coremem_credit_vld[2:0] · coremem_credit_user[2:0][15:0]
  in  reduce_release_vld · reduce_release_user[15:0]
  in  vc_credit[3:0][7:0]
port dsa_cfg (slave, valid/ready, clk)                // DTE RV core 的 dsa_iss
  in  req_valid · req_we · req_addr[11:0] · req_wdata[31:0]
  out req_ready                                         // = 配置通路未反压；Commit Bank 满时拉低
port dsa_rdata (master, 脉冲, clk)                    // 读寄存器的异步返回
  out valid · rdata[31:0]
port dsa_done (master, 脉冲, clk)                     // → TS：task_last 的那一笔完成时报
  out valid · stream_id[3:0] · task_id[5:0]
port cmem_rd / cmem_wr (master, valid/ready, clk)     // 经 DMA_XBAR，256 B
  out req_valid · req_addr[17:0] · req_wdata[2047:0] · req_be[255:0]
  in  req_ready · rsp_valid · rsp_rdata[2047:0] · rsp_scale[63:0]
port mmem_rd / mmem_wr (master, valid/ready, clk)     // 经 DMA_XBAR，256 B
  out req_valid · req_addr[24:0] · req_wdata[2047:0]
  in  req_ready · rsp_valid · rsp_rdata[2047:0]
port smem_wr (master, valid/ready, clk)               // shareMem 表项写，只有 B core / R core 用
  out req_valid · req_addr[14:0] · req_wdata[31:0]
  in  req_ready
port cfg (slave, ctrl_noc 写事务, clk)                // 静态寄存器、Hmem 静态部分、LUT、RouterTable 副本
  in  cfg_valid · cfg_addr[23:0] · cfg_we · cfg_wdata[31:0]
  out cfg_rdata[31:0]
```

***

## 4　存储器

```
mem task_q[l]        FIFO      每 Lane 16 项 × Descriptor（l ∈ {RD_CH0, WR_CH0, RD_CH1, WR_CH1}）  1W1R  按序激活  复位空
mem active_ctx[l]    FF        每 Lane 一份 {task_id[5:0], stream_id[3:0], user_id[15:0], cur_addr, remain, boundary}  1RW  issue_done 后释放  复位空
mem in_buf           FIFO      inbound，约 4 KB（256 B × 16）                        1W1R  满 → TREADY 拉低       复位空
mem out_buf          FIFO      outbound，约 4 KB（256 B × 16）                       1W1R  满 → 停止 RD_CH1 发请求  复位空
mem comp_rs          FF 阵列   16 项 × {task_id[5:0], rd_done, wr_done, drained}      1RW   Commit 时占，Join 时消  复位空
mem done_pend        FIFO      16 × {stream_id[3:0], task_id[5:0]}                    1W1R  串行化同拍多个 Join    复位空
mem hmem_sw          SRAM      sw_header_table 16 stream × 64 task × 16 B = 16 KB     1R1W  按 stream_id 与 task_id 两级索引  复位未定义
mem hmem_mask        FF 阵列   core_mask_table 16 × 2 B = 32 B                        1R1W  按 stream_id 索引      复位 0
mem hdr_static       FF 阵列   64 × {path_id[7:0], size[15:0]}                        1R    boot 阶段配好          复位由输入给
mem lut              FF 阵列   path_id_table + task_len_table，共 192 B               1R    ctrl_noc 配置          复位由输入给
mem rtab_copy        FF 阵列   64 项，RouterTable 的外部副本                           1R1W  软件写，三方一致        复位 0
mem reduce_credit    FF 阵列   16 用户 × 计数器（flit 粒度）                           1RW   建 Stream 资源时分配，release 恢复  复位 0
mem pending_taskq    FIFO      16 × Descriptor                                        1W1R  资源没申请到的任务在这里等  复位空
mem out_vc_buf[4]    FIFO      每 VC 一个，深度按整包容量                              1W1R  某 VC 阻塞只阻塞该 buffer  复位空
mem cfg_bank[2]      FF 阵列   两个配置 Bank × {TASK_CFG_ADDR, TASK_CFG_TD, TASK_CFG_PACK}  1RW  Bank0 优先  复位空
mem pmu_cnt          FF 阵列   11 个计数器                                            1RW   搬运原语数、CH0 / CH1 各自的执行周期与数据量等  复位 0
```

***

## 5　流水线总览

第 1 层图待四条 Lane 与 Commit、Completion RS 的逐级拍数定下来后补。届时按下面这套性能剖析口径把各段对齐：

```
setup_cycles  = first_issue_fire - task_accept_fire
issue_cycles  = issue_done       - first_issue_fire
drain_cycles  = task_done        - issue_done
report_cycles = done_fire        - task_done
total_latency = setup + issue + drain + report
active_cycles = cycles(any_data_fire)
stall_cycles  = cycles(valid && !ready)
```

***

## 6　逐级行为

第 2 层图与每级的四要素待第 1 层图完成后补，级编号回标到第 1 层图。

***

## 7　参数汇总

```
物理数据 Channel   2（inbound_ch / ch0 与 outbound_ch / ch1），拆成四条 Lane
TaskQueue          每 Lane 16 项（深度待评估）
中间 Buffer        inbound + outbound 约 8 KB，256 B × (20～30) T，最大掩盖 32 T 延迟
Completion RS      16 项（待定）；Done Pending 16 项（待定）
与 Cmem 接口宽度    256 B + 8 B（Data + scale），双向；按 Cmem MAS 取 256 B
与 Mmem 接口宽度    256 B，双向；写 9T、读 8T
与 Router 接口宽度  256 B，双向（看不到 scale）
Hmem               16 KB + 32 B（sw_header_table + core_mask_table）
LUT                192 B（path_id_table + task_len_table）
单任务最大搬运量    32 KB（256 B × 128 拍）
内部启动延迟        85T（流水启动 5T + 50 条指令算地址 75T + core 发射 5T）
MSG 包结构          包头标记 2 B + Router 信息 4 B（path_id 1 B + path_core_mask 2 B + rsv 1 B）+ 包长度 2 B + 软件辅助信息 0～16 B + 业务数据 0～(64 K − 24) B
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
| 双 Bank，Bank0 优先，竞争时优先 Router | F12 | `commit_bank_priority` |
| 必须最后写 Doorbell，PACK 随之自动写入 | F14 | `doorbell_order` |
| 四条 Lane 状态彼此独立 | F17 | `lane_independent` |
| 同一 Lane 内任务不乱序 | F18 | `lane_inorder` |
| issue_done 就允许 Lane 走下一个任务 | F19 | `issue_done_release` |
| read-ahead 领先量的三项约束 | F20 | `read_ahead_limit` |
| Router→CM 与 MM→CM 竞争 CM 写路径 | F21 | `cm_write_contend` |
| 单任务上限 32 KB，超过拆成多个任务包 | F23 | `task_split_32k` |
| Buffer 满时经 TREADY 反压 Router | F25 | `buffer_backpressure` |
| Completion RS 按 task_id Join | F27 | `completion_join` |
| 同拍多个 Join 全部写入 Done Pending，不丢失 | F28 | `join_serialize` |
| 向 TS exactly-once | F29 | `exactly_once` |
| 六个完成层级 | F30 | `completion_levels` |
| task_last 才通知 TS，no_ack 不回 Ack | F31 | `task_last_ack` |
| 三条数据流各自的通路与出口绑定 | F32～F34 | `three_flows` |
| MM → CM 的 route mask 只允许 CoreMem | F34 | `wr1_route_mask` |
| 一个包进核拆成四份分开存 | F36～F38 | `packet_split_four` |
| data_len 在不同方向盖的范围不同 | F41 | `data_len_scope` |
| hw_header_op 决定存不存包头 | F42 | `hw_header_op` |
| 出核按 task_id 改写 path_id 与 size | F43 | `header_rewrite` |
| 纯包头任务 data_len = 0 | F44 | `header_only_task` |
| 软件只配基址，硬件用 stream_id 算偏移 | F45、F46 | `stream_offset` |
| scale 与 topK 的长度硬件自己算 | F47 | `derived_length` |
| Matrix Mem 侧不加偏移，Core Mem 侧加 | F48 | `mm_no_offset` |
| shareMem 写：搬入置 valid、搬出置 invalid | F49、F50 | `sharemem_flag` |
| 三份 RouterTable 副本一致 | F51 | `dte_rtab_copy` |
| 出核前置申请，没拿到就在 PendingTaskQ 等 | F52 | `dte_pending_taskq` |
| Reduce credit 够整包才发 | F53、F54 | `dte_reduce_credit` |
| ReduceModule 每完成一次 Reduce 还一个 credit | F55 | `reduce_credit_release` |
| 出方向 4 个 VC buffer，单 VC 阻塞不影响其他 | F56 | `dte_vc_buffers` |
| 进方向单 VC，搬完按 flit 释放 VC credit | F57 | `dte_single_vc_in` |
| 两类业务层 credit 都分方向，先查 routing table | F58 | `credit_by_direction` |
| credit release 按 action 决定是否更新 stream 表 | F59 | `credit_release_action` |

***

## 9　取舍

* **为什么一个任务要拆成读写两半**
  * 搬运的两端节奏不同：Router 侧什么时候来数据由上游决定，存储侧要抢 bank
  * 绑成一体，任一端卡住另一端就空转；拆开之后两端各自按自己的节奏排队，中间 buffer 吸收速度差
  * 代价是要额外维护配对关系和 Join 逻辑
* **为什么 Commit 必须两侧同时拿到资源**
  * 只拿到读侧就开始收数据，数据进了 buffer 却没有写侧上下文可以落地，buffer 会被一个无法推进的任务占住
  * 两侧同时拿到才接纳，把这种半途卡死挡在入口
* **为什么单向 2 通道而不是 3 Lane**
  * 把 Matrix Mem、Core Mem、Router 各看作一个读写 Resource 时理想情况是 3 Lane，代价是 XBar 面积近似 `N_in × N_out × W`，偏大
  * 带宽不是约束：需求 190～320 GB/s，而 Matrix Mem 8192 GB/s、Core Mem 512 GB/s、Router 双向各 256 GB/s
  * 物理布局上 DTE 位于 Router 与 Cmem 之间、Mmem 在 Cmem 上方，按物理相邻关系合并通路的代价最小
* **为什么 `issue_done` 就允许 Lane 走下一个任务**
  * Lane 的资源是 Active Context，不是在途事务。最后一个请求发出后 Lane 本身已经空出来，剩下的响应排空由 Completion RS 按 `task_id` 跟踪
  * 若等到全部 drain 才放行，外部响应延迟会直接算进 Lane 的占用时间
* **为什么一个包进核要拆成四份分开存**
  * 四份的读者不同：data 与 scale 给 MU 和 VU 算，topK 给 MU 查专家，包头只在这个 token 再出核时用来重写路由
