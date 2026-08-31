# DTE 数据搬运引擎

**模式**：design（陈述当前设计，动机与取舍收在各节的“取舍”段落里）

给要实现或建模 DTE 的人：DTE 解决什么问题、靠什么解决、每条机制归在哪一处。

* DTE 在 core 内的位置、它和三级存储的关系：《执行单元与存储》
* 逐级拍数、接口位宽、机制覆盖表：[DTE 建模单元](07-units/chip/core/dte.md)

《Bach 硬件设计建模参考》DTE 专题，全套目录见 [README](README.md)。

***

## DTE 解决的问题

core 里 MU 算矩阵、VU 算向量。数据怎么进来、怎么出去、怎么在两块存储之间倒手，全归 DTE。它只做搬运，不做计算，职责是五件事：

* 接纳任务
* 生成访问命令
* 处理背压
* 追踪在途事务
* 向 TS 报告最终完成

难点在两端的节奏对不上：

* **Router 一侧**：流式到达，按 AXI-Stream 的帧边界走，什么时候来由上游决定
* **存储一侧**：要过 DMA_XBAR 抢 bank，Core Mem 有 8 个 bank、Matrix Mem 有 64 个，命中冲突就得排队
* 把两端硬绑在一起，慢的一端会把快的一端拖住

DTE 的做法是**把一个搬运任务从中间劈开**：

* 读一半、写一半，各自排队各自推进
* 中间用 buffer 顶住速度差
* 完成时按 `task_id` 合回一次 `task_done` 报给 TS

后面两节分别是这个“劈开”和这个“合上”。

支持五种搬运方向：

| Route | 用哪对 Lane | 说明 |
| - | - | - |
| Router → MM | RD_CH0 + WR_CH0（inbound） | Header Parser 解析后 Commit 成对建立 |
| Router → CM | RD_CH0 + WR_CH0（inbound） | 同上 |
| MM → Router | RD_CH1 + WR_CH1（outbound） | 出口是 Router TX |
| CM → Router | RD_CH1 + WR_CH1（outbound） | 出口是 Router TX |
| MM → CM | RD_CH1 + WR_CH1（outbound） | 出口切到 `DMA WR1`，硬件 route mask 只允许 CoreMem |
| ~~CM → MM~~ | — | **本版本不支持**，XBar 不提供 CH1 write ctrl 到 Matrix Memory 的连接 |

数据布局：**仅支持连续一维搬运**，当前不支持 stride。

模块组成与对外通道如下，布局与通道名照 MAS 的 Block Diagram 与 External Connections。

```svg
<svg viewBox="0 0 1240 830" width="1240" height="830" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="DTE 的模块组成与对外通道">
<title>DTE 的模块组成与对外通道</title>
<rect width="1240" height="830" fill="#ffffff"/>
<defs><marker id="kka" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="kkas" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker><marker id="kkai" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#3f4451"/></marker><marker id="kkais" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#3f4451"/></marker><marker id="kkab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="kkabs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="kkar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="kkars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="kkac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="kkacs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="kkap" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="kkaps" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker></defs>
<text x="30" y="26" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="start">DTE 的模块组成与对外通道</text>
<text x="30" y="44" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">布局与通道名照 MAS 的 Block Diagram：左列是任务入口（config reg / LUT / MUX / taskQ），中间上下两条 Channel（inbound_ch/ch0、outbound_ch/ch1），右侧 DMA XBar 接 MatrixMem / CoreMem；</text>
<text x="30" y="58" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">每条 Channel 都是 taskq → agcu → ctrl 的读写两行，中间一个 buffer；Hmem 收进核包头、出核时被读；对外通道名照 External Connections 图</text>
<rect x="40" y="140" width="120" height="46" rx="5" fill="#fffbeb" stroke="#d97706" stroke-width="1.3"/>
<text x="48" y="155" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#16181d" font-weight="700" text-anchor="start">DTE RVCore</text>
<text x="48" y="169" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">用 dsawi 写四个寄存器</text>
<text x="48" y="181" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">core_cmd / status</text>
<rect x="40" y="246" width="120" height="46" rx="5" fill="#dbeafe" stroke="#2563eb" stroke-width="1.3"/>
<text x="48" y="261" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#2563eb" font-weight="700" text-anchor="start">TS</text>
<text x="48" y="275" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">trigger：TID · SID · UID</text>
<text x="48" y="287" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">收 done</text>
<rect x="40" y="352" width="120" height="46" rx="5" fill="#eceef1" stroke="#6b7280" stroke-width="1.3"/>
<text x="48" y="367" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" font-weight="700" text-anchor="start">ctrl_NOC</text>
<text x="48" y="381" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">SCP 经 ctrl_ch 配</text>
<text x="48" y="393" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">LUT 与静态寄存器</text>
<rect x="40" y="458" width="120" height="46" rx="5" fill="#eceef1" stroke="#6b7280" stroke-width="1.3"/>
<text x="48" y="473" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" font-weight="700" text-anchor="start">Debug</text>
<text x="48" y="487" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">debug_ch</text>
<rect x="190" y="96" width="830" height="650" rx="12" fill="#eef4ff" stroke="#2563eb" stroke-width="1.6"/>
<text x="204" y="116" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="13" fill="#2563eb" font-weight="700" text-anchor="start">DTE</text>
<rect x="215" y="140" width="90" height="46" rx="5" fill="#ffffff" stroke="#c9ced6" stroke-width="1.3"/>
<text x="223" y="155" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">config reg</text>
<text x="223" y="169" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">ADDR · TD · PACK</text>
<text x="223" y="181" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">Doorbell 启动</text>
<rect x="215" y="246" width="90" height="46" rx="5" fill="#fef3c7" stroke="#d97706" stroke-width="1.3"/>
<text x="223" y="261" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">LUT</text>
<text x="223" y="275" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">path_id_table</text>
<text x="223" y="287" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">task_len_table 192 B</text>
<path d="M330 150 L352 162 L352 282 L330 294 Z" fill="#ffffff" stroke="#3f4451" stroke-width="1.2"/>
<text transform="translate(342 222) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" text-anchor="middle">MUX</text>
<rect x="380" y="192" width="80" height="62" rx="5" fill="#dcfce7" stroke="#16a34a" stroke-width="1.3"/>
<text x="388" y="207" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">taskQ</text>
<text x="388" y="221" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">TaskQueue 16 项</text>
<text x="388" y="233" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">Commit：读写两侧</text>
<text x="388" y="245" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">同时拿到项才成立</text>
<rect x="215" y="458" width="90" height="46" rx="5" fill="#ffffff" stroke="#c9ced6" stroke-width="1.3"/>
<text x="223" y="473" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">Debug ctrl</text>
<text x="223" y="487" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">调试通路</text>
<path d="M160 163 L215 163" stroke="#16181d" stroke-width="1.4" fill="none" marker-end="url(#kkai)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M160 269 L215 269" stroke="#2563eb" stroke-width="1.6" fill="none" marker-end="url(#kkab)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M160 375 L200 375 L200 300 L215 300" stroke="#6b7280" stroke-width="1.2" fill="none" marker-end="url(#kka)" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M200 375 L200 190 L215 190" stroke="#6b7280" stroke-width="1.2" fill="none" marker-end="url(#kka)" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M160 481 L215 481" stroke="#6b7280" stroke-width="1.2" fill="none" marker-end="url(#kka)" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M305 163 L330 163" stroke="#16181d" stroke-width="1.4" fill="none" marker-end="url(#kkai)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M305 269 L330 269" stroke="#16181d" stroke-width="1.4" fill="none" marker-end="url(#kkai)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M352 222 L380 222" stroke="#16181d" stroke-width="1.6" fill="none" marker-end="url(#kkai)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M305 481 L420 481 L420 254" stroke="#6b7280" stroke-width="1.2" fill="none" marker-end="url(#kka)" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="470" y="140" width="440" height="200" rx="8" fill="#ffffff" stroke="#2563eb" stroke-width="1.2"/>
<text x="690" y="156" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#2563eb" font-weight="700" text-anchor="middle">inbound_ch / ch0　Router → MM / CM</text>
<rect x="485" y="170" width="80" height="32" rx="5" fill="#dcfce7" stroke="#16a34a" stroke-width="1.3"/>
<text x="525.0" y="190.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">ch0_taskq</text>
<rect x="600" y="170" width="90" height="32" rx="5" fill="#dcfce7" stroke="#16a34a" stroke-width="1.3"/>
<text x="645.0" y="190.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">rd_ch0_agcu</text>
<rect x="720" y="170" width="90" height="32" rx="5" fill="#dcfce7" stroke="#16a34a" stroke-width="1.3"/>
<text x="765.0" y="190.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">ch0_rd_ctrl</text>
<rect x="720" y="226" width="90" height="28" rx="5" fill="#fef3c7" stroke="#d97706" stroke-width="1.3"/>
<text x="765.0" y="244.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">inbound buffer</text>
<rect x="485" y="280" width="80" height="32" rx="5" fill="#f3e8ff" stroke="#a21caf" stroke-width="1.3"/>
<text x="525.0" y="300.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">ch0_wr_taskq</text>
<rect x="600" y="280" width="90" height="32" rx="5" fill="#f3e8ff" stroke="#a21caf" stroke-width="1.3"/>
<text x="645.0" y="300.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">wr_ch0_agcu</text>
<rect x="720" y="280" width="90" height="32" rx="5" fill="#f3e8ff" stroke="#a21caf" stroke-width="1.3"/>
<text x="765.0" y="300.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">ch0_wr_ctrl</text>
<path d="M565 186 L600 186" stroke="#16181d" stroke-width="1.4" fill="none" marker-end="url(#kkai)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M690 186 L720 186" stroke="#16181d" stroke-width="1.4" fill="none" marker-end="url(#kkai)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M525 202 L525 280" stroke="#16181d" stroke-width="1.4" fill="none" marker-end="url(#kkai)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M565 296 L600 296" stroke="#16181d" stroke-width="1.4" fill="none" marker-end="url(#kkai)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M690 296 L720 296" stroke="#16181d" stroke-width="1.4" fill="none" marker-end="url(#kkai)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M765 202 L765 226" stroke="#2563eb" stroke-width="2.2" fill="none" marker-end="url(#kkab)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M765 254 L765 280" stroke="#2563eb" stroke-width="2.2" fill="none" marker-end="url(#kkab)" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="470" y="380" width="440" height="200" rx="8" fill="#ffffff" stroke="#0d9488" stroke-width="1.2"/>
<text x="690" y="396" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#0d9488" font-weight="700" text-anchor="middle">outbound_ch / ch1　MM / CM → Router，MM → CM</text>
<rect x="485" y="410" width="80" height="32" rx="5" fill="#dcfce7" stroke="#16a34a" stroke-width="1.3"/>
<text x="525.0" y="430.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">ch1_taskq</text>
<rect x="600" y="410" width="90" height="32" rx="5" fill="#dcfce7" stroke="#16a34a" stroke-width="1.3"/>
<text x="645.0" y="430.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">rd_ch1_agcu</text>
<rect x="720" y="410" width="90" height="32" rx="5" fill="#dcfce7" stroke="#16a34a" stroke-width="1.3"/>
<text x="765.0" y="430.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">ch1_rd_ctrl</text>
<rect x="720" y="466" width="90" height="28" rx="5" fill="#fef3c7" stroke="#d97706" stroke-width="1.3"/>
<text x="765.0" y="484.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">outbound buffer</text>
<rect x="485" y="520" width="80" height="32" rx="5" fill="#f3e8ff" stroke="#a21caf" stroke-width="1.3"/>
<text x="525.0" y="540.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">ch1_wr_taskq</text>
<rect x="600" y="520" width="90" height="32" rx="5" fill="#f3e8ff" stroke="#a21caf" stroke-width="1.3"/>
<text x="645.0" y="540.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">wr_ch1_agcu</text>
<rect x="720" y="520" width="90" height="32" rx="5" fill="#f3e8ff" stroke="#a21caf" stroke-width="1.3"/>
<text x="765.0" y="540.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">ch1_wr_ctrl</text>
<path d="M565 426 L600 426" stroke="#16181d" stroke-width="1.4" fill="none" marker-end="url(#kkai)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M690 426 L720 426" stroke="#16181d" stroke-width="1.4" fill="none" marker-end="url(#kkai)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M525 442 L525 520" stroke="#16181d" stroke-width="1.4" fill="none" marker-end="url(#kkai)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M565 536 L600 536" stroke="#16181d" stroke-width="1.4" fill="none" marker-end="url(#kkai)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M690 536 L720 536" stroke="#16181d" stroke-width="1.4" fill="none" marker-end="url(#kkai)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M765 442 L765 466" stroke="#2563eb" stroke-width="2.2" fill="none" marker-end="url(#kkab)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M765 494 L765 520" stroke="#2563eb" stroke-width="2.2" fill="none" marker-end="url(#kkab)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="690" y="348" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">RD 可领先 WR，领先量由 buffer credit 约束；buffer 满经 TREADY 向 Router 反压</text>
<text x="690" y="588" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">出口由 Route 固化：Router TX 或 CoreMem（CH1 write 只允许 CoreMem，所以不支持 CM → MM）</text>
<path d="M460 222 L470 222 L470 186 L485 186" stroke="#16181d" stroke-width="1.6" fill="none" marker-end="url(#kkai)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M470 222 L470 426 L485 426" stroke="#16181d" stroke-width="1.6" fill="none" marker-end="url(#kkai)" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="800" y="108" width="180" height="24" rx="5" fill="#fef3c7" stroke="#d97706" stroke-width="1.3"/>
<text x="890.0" y="124.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="middle">Hmem 16 KB + 32 B</text>
<text x="890" y="142" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="middle">sw_harder_table 16×64 项 · core_mask_table</text>
<path d="M780 170 L780 150 L820 150 L820 132" stroke="#d97706" stroke-width="1.4" fill="none" marker-end="url(#kkar)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="826" y="154" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#d97706" font-weight="400" text-anchor="start">2 B / 16 B 包头写入</text>
<path d="M990 132 L1000 132 L1000 576 L765 576 L765 552" stroke="#dc2626" stroke-width="1.2" fill="none" marker-end="url(#kkar)" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round"/>
<text transform="translate(1009 360) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#dc2626" text-anchor="middle">出核时读包头（sw / hw 包头）</text>
<rect x="1030" y="200" width="44" height="380" rx="6" fill="#eceef1" stroke="#3f4451" stroke-width="1.3"/>
<text transform="translate(1052 390) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#16181d" text-anchor="middle">DMA XBar</text>
<path d="M810 296 L1030 296" stroke="#2563eb" stroke-width="2.4" fill="none" marker-end="url(#kkab)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="920" y="290" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" font-weight="400" text-anchor="middle">dsa2xbar_ch0_wr　256 B</text>
<path d="M1030 426 L810 426" stroke="#2563eb" stroke-width="2.4" fill="none" marker-end="url(#kkab)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="920" y="420" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" font-weight="400" text-anchor="middle">xbar2dsa_ch　256 B</text>
<path d="M840 526 L862 536 L862 556 L840 566 Z" fill="#ffffff" stroke="#3f4451" stroke-width="1.2"/>
<path d="M810 536 L840 536" stroke="#2563eb" stroke-width="2.2" fill="none" marker-end="url(#kkab)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M862 546 L900 546 L900 536 L1030 536" stroke="#2563eb" stroke-width="2.2" fill="none" marker-end="url(#kkab)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="965" y="530" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" font-weight="400" text-anchor="middle">dsa2xbar_ch1_wr　CoreMem Only</text>
<path d="M900 546 L900 700 L1120 700 L1120 606" stroke="#2563eb" stroke-width="2.4" fill="none" marker-end="url(#kkab)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="1010" y="694" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" font-weight="400" text-anchor="middle">dsa2router_ch　256 B</text>
<rect x="1110" y="108" width="110" height="40" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="1118" y="123" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#0d9488" font-weight="700" text-anchor="start">MU</text>
<text x="1118" y="137" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">local_ep_table</text>
<text x="1118" y="149" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">128 B / 256 B</text>
<path d="M810 178 L1090 178 L1090 138 L1110 138" stroke="#0d9488" stroke-width="1.4" fill="none" marker-end="url(#kkac)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="950" y="174" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#0d9488" font-weight="400" text-anchor="middle">topK 复制给 MU　128 B / 256 B</text>
<rect x="1110" y="170" width="110" height="40" rx="5" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.3"/>
<text x="1118" y="185" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#7c3aed" font-weight="700" text-anchor="start">Router RX</text>
<text x="1118" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">router2dsa_ch</text>
<text x="1118" y="211" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">read_ch0 · 256 B</text>
<path d="M1110 196 L810 196" stroke="#2563eb" stroke-width="2.4" fill="none" marker-end="url(#kkab)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="960" y="210" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#2563eb" font-weight="400" text-anchor="middle">AXI-Stream：首拍 Header，Header Parser 在 ch0_rd_ctrl 解析</text>
<rect x="1110" y="276" width="110" height="40" rx="5" fill="#fdeed8" stroke="#d97706" stroke-width="1.3"/>
<text x="1118" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#d97706" font-weight="700" text-anchor="start">MatrixMem</text>
<text x="1118" y="305" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">to_mm_ch CH0</text>
<text x="1118" y="317" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">mm_out_ch</text>
<path d="M1074 296 L1110 296" stroke="#d97706" stroke-width="1.8" fill="none" marker-end="url(#kkar)" marker-start="url(#kkars)" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="1110" y="406" width="110" height="40" rx="5" fill="#fdeed8" stroke="#d97706" stroke-width="1.3"/>
<text x="1118" y="421" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#d97706" font-weight="700" text-anchor="start">CoreMem</text>
<text x="1118" y="435" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">to_cm_ch CH0 / CH1</text>
<text x="1118" y="447" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">cm_out_ch</text>
<path d="M1074 426 L1110 426" stroke="#d97706" stroke-width="1.8" fill="none" marker-end="url(#kkar)" marker-start="url(#kkars)" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="1110" y="566" width="110" height="40" rx="5" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.3"/>
<text x="1118" y="581" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#7c3aed" font-weight="700" text-anchor="start">Router TX</text>
<text x="1118" y="595" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">write_ch1 · 256 B</text>
<text x="1118" y="607" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">AXI-Stream Valid/Ready</text>
<rect x="485" y="620" width="325" height="40" rx="5" fill="#dbeafe" stroke="#2563eb" stroke-width="1.3"/>
<text x="493" y="635" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#2563eb" font-weight="700" text-anchor="start">Completion</text>
<text x="493" y="649" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">RD 与 WR 两侧按 task_id Join，buffer 排空后才生成一次 task_done；同一任务只报一次</text>
<path d="M765 552 L765 620" stroke="#d97706" stroke-width="1.2" fill="none" marker-end="url(#kkar)" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M830 312 L830 366 L680 366 L680 620" stroke="#d97706" stroke-width="1.2" fill="none" marker-end="url(#kkar)" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M485 640 L175 640 L175 292" stroke="#d97706" stroke-width="1.6" fill="none" marker-end="url(#kkar)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="330" y="634" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="middle">done → TS（dte2ts_done_ch）</text>
<rect x="30" y="770" width="1180" height="40" rx="5" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.2"/>
<text x="46" y="787" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">外部通道名照 External Connections：ctrl_ch（SCP）、debug_ch、core_cmd / status（DTE Core）、trigger_dsa 与 router2dsa_ch / dsa2router_ch（Router）、dsa2xbar_ch0_wr / ch1_wr 与 xbar2dsa_ch（DMA_XBAR）、done（TS）。</text>
<text x="46" y="803" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">MAS 图里 inbound_ch 的写控制标成 ch1_wr_ctrl，按上下文应为 ch0_wr_ctrl，本图按后者；参数：TaskQueue 深度 16（待定）、中间 buffer 约 8 KB、Cmem 口 256 B + 8 B scale、Router 口 256 B。</text>
</svg>
```

***

## 拆开与合上

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1340 790" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" role="img" aria-label="一个搬运任务在 DTE 里被拆成读写两半又合回一次完成">
<title>一个搬运任务怎么被拆开又合上</title>
<defs>
<marker id="ad" markerWidth="9" markerHeight="9" refX="7.5" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#2563eb"/></marker>
<marker id="ac" markerWidth="9" markerHeight="9" refX="7.5" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#d97706"/></marker>
<marker id="at" markerWidth="9" markerHeight="9" refX="7.5" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#0d9488"/></marker>
</defs>
<rect width="1340" height="790" fill="#ffffff"/>
<text x="24" y="30" font-size="15" fill="#111827" font-weight="600">一个搬运任务怎么被拆开又合上</text>
<text x="24" y="50" font-size="10.5" fill="#475569">读一半、写一半各自排队推进，中间 buffer 顶住两端速度差，完成时按 task_id 合回一次 task_done</text>
<rect x="210" y="96" width="730" height="60" rx="4" fill="#f8fafc" stroke="#374151" stroke-width="1.25"/>
<text x="221" y="118" font-size="12" fill="#111827" font-weight="600">Commit：配对接纳，不产生半任务</text>
<text x="221" y="134" font-size="9.5" fill="#475569">一个高层任务必须同时拿到 RD Lane 的 TaskQueue 项、WR Lane 的 TaskQueue 项和 Completion RS 项；任一侧没有空间就整体保持，Header 入口向 Router 反压。同一步完成地址展开</text>
<rect x="30" y="200" width="160" height="110" rx="4" fill="#eef2f7" stroke="#374151" stroke-width="1.25"/>
<text x="41" y="220" font-size="12" fill="#111827" font-weight="600">Router RX</text>
<text x="41" y="236" font-size="9.5" fill="#475569">AXI-Stream</text>
<text x="41" y="249" font-size="9.5" fill="#475569">一帧一任务</text>
<text x="41" y="262" font-size="9.5" fill="#475569">首拍固定 Header</text>
<rect x="210" y="200" width="170" height="110" rx="4" fill="#f8fafc" stroke="#374151" stroke-width="1.25"/>
<text x="221" y="220" font-size="12" fill="#111827" font-weight="600">RD_CH0</text>
<text x="221" y="236" font-size="9.5" fill="#475569">收 Payload 写 buffer</text>
<text x="221" y="249" font-size="9.5" fill="#475569">带 task_id、有效字节</text>
<text x="221" y="262" font-size="9.5" fill="#475569">与任务边界</text>
<text x="221" y="275" font-size="9.5" fill="#475569">buffer 满则 TREADY 反压</text>
<rect x="400" y="200" width="140" height="110" rx="4" fill="#eef2f6" stroke="#374151" stroke-width="1.25"/>
<text x="411" y="220" font-size="12" fill="#111827" font-weight="600">inbound buffer</text>
<text x="411" y="236" font-size="9.5" fill="#475569">与 outbound</text>
<text x="411" y="249" font-size="9.5" fill="#475569">合计约 8 KB</text>
<text x="411" y="262" font-size="9.5" fill="#475569">256B × 20～30 T</text>
<text x="411" y="275" font-size="9.5" fill="#475569">掩盖 32 T 延迟</text>
<rect x="560" y="200" width="170" height="110" rx="4" fill="#f8fafc" stroke="#374151" stroke-width="1.25"/>
<text x="571" y="220" font-size="12" fill="#111827" font-weight="600">WR_CH0</text>
<text x="571" y="236" font-size="9.5" fill="#475569">按任务边界取数</text>
<text x="571" y="249" font-size="9.5" fill="#475569">经 DMA_XBAR</text>
<text x="571" y="262" font-size="9.5" fill="#475569">写 MM / CM</text>
<rect x="750" y="200" width="170" height="110" rx="4" fill="#eef2f7" stroke="#374151" stroke-width="1.25"/>
<text x="761" y="220" font-size="12" fill="#111827" font-weight="600">Matrix Mem / Core Mem</text>
<text x="761" y="236" font-size="9.5" fill="#475569">inbound 的落点</text>
<rect x="30" y="350" width="160" height="110" rx="4" fill="#eef2f7" stroke="#374151" stroke-width="1.25"/>
<text x="41" y="370" font-size="12" fill="#111827" font-weight="600">Matrix Mem / Core Mem</text>
<text x="41" y="386" font-size="9.5" fill="#475569">outbound 的源</text>
<rect x="210" y="350" width="170" height="110" rx="4" fill="#f8fafc" stroke="#374151" stroke-width="1.25"/>
<text x="221" y="370" font-size="12" fill="#111827" font-weight="600">RD_CH1</text>
<text x="221" y="386" font-size="9.5" fill="#475569">AGCU 生成源端读地址</text>
<text x="221" y="399" font-size="9.5" fill="#475569">经 DMA_XBAR 读</text>
<text x="221" y="412" font-size="9.5" fill="#475569">数据连同 task_id</text>
<text x="221" y="425" font-size="9.5" fill="#475569">与边界元数据入 buffer</text>
<rect x="400" y="350" width="140" height="110" rx="4" fill="#eef2f6" stroke="#374151" stroke-width="1.25"/>
<text x="411" y="370" font-size="12" fill="#111827" font-weight="600">outbound buffer</text>
<text x="411" y="386" font-size="9.5" fill="#475569">read-ahead 的</text>
<text x="411" y="399" font-size="9.5" fill="#475569">领先量由它的</text>
<text x="411" y="412" font-size="9.5" fill="#475569">credit 约束</text>
<rect x="560" y="350" width="170" height="110" rx="4" fill="#f8fafc" stroke="#374151" stroke-width="1.25"/>
<text x="571" y="370" font-size="12" fill="#111827" font-weight="600">WR_CH1</text>
<text x="571" y="386" font-size="9.5" fill="#475569">按固化的 Route 选出口</text>
<text x="571" y="399" font-size="9.5" fill="#475569">Router TX 或</text>
<text x="571" y="412" font-size="9.5" fill="#475569">CoreMem Egress</text>
<rect x="750" y="350" width="170" height="110" rx="4" fill="#eef2f7" stroke="#374151" stroke-width="1.25"/>
<text x="761" y="370" font-size="12" fill="#111827" font-weight="600">Router TX / Core Mem</text>
<text x="761" y="386" font-size="9.5" fill="#475569">MM → CM 时出口</text>
<text x="761" y="399" font-size="9.5" fill="#475569">切到 DMA WR1</text>
<rect x="210" y="510" width="730" height="60" rx="4" fill="#f8fafc" stroke="#374151" stroke-width="1.25"/>
<text x="221" y="532" font-size="12" fill="#111827" font-weight="600">Completion RS：按 task_id Join</text>
<text x="221" y="548" font-size="9.5" fill="#475569">同一 task_id 的 RD 与 WR 两侧条件都满足才产生 task_done。同一拍多个 Join 命中时全部写入 Done Pending，不允许覆盖或丢失</text>
<rect x="980" y="510" width="170" height="60" rx="4" fill="#f8fafc" stroke="#374151" stroke-width="1.25"/>
<text x="991" y="532" font-size="12" fill="#111827" font-weight="600">Done Pending</text>
<text x="991" y="548" font-size="9.5" fill="#475569">串行化后报 TS</text>
<path d="M192 255 L206 255" fill="none" stroke="#2563eb" stroke-width="1.6" marker-end="url(#ad)"/>
<path d="M382 255 L396 255" fill="none" stroke="#2563eb" stroke-width="1.6" marker-end="url(#ad)"/>
<path d="M542 255 L556 255" fill="none" stroke="#2563eb" stroke-width="1.6" marker-end="url(#ad)"/>
<path d="M732 255 L746 255" fill="none" stroke="#2563eb" stroke-width="1.6" marker-end="url(#ad)"/>
<path d="M192 405 L206 405" fill="none" stroke="#2563eb" stroke-width="1.6" marker-end="url(#ad)"/>
<path d="M382 405 L396 405" fill="none" stroke="#2563eb" stroke-width="1.6" marker-end="url(#ad)"/>
<path d="M542 405 L556 405" fill="none" stroke="#2563eb" stroke-width="1.6" marker-end="url(#ad)"/>
<path d="M732 405 L746 405" fill="none" stroke="#2563eb" stroke-width="1.6" marker-end="url(#ad)"/>
<rect x="36.32000000000001" y="181.5" width="147.35999999999999" height="15.5" fill="#ffffff" opacity="0.95"/>
<text x="110" y="192" font-size="9.5" fill="#2563eb" text-anchor="middle">inbound：Router → MM / CM</text>
<rect x="18.650000000000006" y="331.5" width="182.7" height="15.5" fill="#ffffff" opacity="0.95"/>
<text x="110" y="342" font-size="9.5" fill="#2563eb" text-anchor="middle">outbound：MM / CM → Router / CM</text>
<path d="M295 158 L295 198" fill="none" stroke="#d97706" stroke-width="1.6" marker-end="url(#ac)"/>
<path d="M645 158 L645 198" fill="none" stroke="#d97706" stroke-width="1.6" marker-end="url(#ac)"/>
<path d="M390 158 L390 405 L384 405" fill="none" stroke="#d97706" stroke-width="1.6" marker-end="url(#ac)"/>
<path d="M740 158 L740 405 L734 405" fill="none" stroke="#d97706" stroke-width="1.6" marker-end="url(#ac)"/>
<rect x="368.98" y="166" width="218.04" height="15" fill="#ffffff" opacity="0.95"/>
<text x="478" y="176" font-size="9" fill="#d97706" text-anchor="middle">四条 Lane 各自的 TaskQueue 与 Active Context</text>
<path d="M922 255 L960 255 L960 508" fill="none" stroke="#0d9488" stroke-width="1.6" marker-end="url(#at)"/>
<path d="M922 405 L930 405 L930 508" fill="none" stroke="#0d9488" stroke-width="1.6" marker-end="url(#at)"/>
<rect x="965" y="320" width="61.8" height="15" fill="#ffffff" opacity="0.95"/>
<text x="968" y="330" font-size="9" fill="#0d9488" text-anchor="start">RD / WR 两侧</text>
<rect x="965" y="334" width="61.8" height="15" fill="#ffffff" opacity="0.95"/>
<text x="968" y="344" font-size="9" fill="#0d9488" text-anchor="start">各自 drained</text>
<path d="M942 540 L976 540" fill="none" stroke="#0d9488" stroke-width="1.6" marker-end="url(#at)"/>
<path d="M1065 572 L1065 606" fill="none" stroke="#0d9488" stroke-width="1.6" marker-end="url(#at)"/>
<rect x="979.54" y="613.5" width="170.92" height="15.5" fill="#ffffff" opacity="0.95"/>
<text x="1065" y="624" font-size="9.5" fill="#0d9488" text-anchor="middle">task_done → TS（exactly-once）</text>
<text x="24" y="690" font-size="12" fill="#111827" font-weight="600">完成的六个层级</text>
<rect x="160" y="672" width="175" height="44" rx="5" fill="#fdf5e8" stroke="#c2823a" stroke-width="1.1"/>
<text x="247" y="688" font-size="10.5" fill="#111827" text-anchor="middle" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">queued</text>
<text x="247" y="705" font-size="9" fill="#475569" text-anchor="middle">进 TaskQueue</text>
<path d="M337 694 L353 694" fill="none" stroke="#d97706" stroke-width="1.4" marker-end="url(#ac)"/>
<rect x="355" y="672" width="175" height="44" rx="5" fill="#fdf5e8" stroke="#c2823a" stroke-width="1.1"/>
<text x="442" y="688" font-size="10.5" fill="#111827" text-anchor="middle" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">active</text>
<text x="442" y="705" font-size="9" fill="#475569" text-anchor="middle">装载为 Active Context</text>
<path d="M532 694 L548 694" fill="none" stroke="#d97706" stroke-width="1.4" marker-end="url(#ac)"/>
<rect x="550" y="672" width="175" height="44" rx="5" fill="#fdf5e8" stroke="#c2823a" stroke-width="1.1"/>
<text x="637" y="688" font-size="10.5" fill="#111827" text-anchor="middle" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">issue_done</text>
<text x="637" y="705" font-size="9" fill="#475569" text-anchor="middle">最后一个请求已 Fire</text>
<path d="M727 694 L743 694" fill="none" stroke="#d97706" stroke-width="1.4" marker-end="url(#ac)"/>
<rect x="745" y="672" width="175" height="44" rx="5" fill="#fdf5e8" stroke="#c2823a" stroke-width="1.1"/>
<text x="832" y="688" font-size="10.5" fill="#111827" text-anchor="middle" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">drained</text>
<text x="832" y="705" font-size="9" fill="#475569" text-anchor="middle">响应与 Buffer 收敛</text>
<path d="M922 694 L938 694" fill="none" stroke="#d97706" stroke-width="1.4" marker-end="url(#ac)"/>
<rect x="940" y="672" width="175" height="44" rx="5" fill="#fdf5e8" stroke="#c2823a" stroke-width="1.1"/>
<text x="1027" y="688" font-size="10.5" fill="#111827" text-anchor="middle" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">join_done</text>
<text x="1027" y="705" font-size="9" fill="#475569" text-anchor="middle">RD 与 WR 都满足</text>
<path d="M1117 694 L1133 694" fill="none" stroke="#d97706" stroke-width="1.4" marker-end="url(#ac)"/>
<rect x="1135" y="672" width="175" height="44" rx="5" fill="#fdf5e8" stroke="#c2823a" stroke-width="1.1"/>
<text x="1222" y="688" font-size="10.5" fill="#111827" text-anchor="middle" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">task_done</text>
<text x="1222" y="705" font-size="9" fill="#475569" text-anchor="middle">与 TS 握手成功</text>
<text x="160" y="736" font-size="9.5" fill="#475569">前三层是 Lane 子上下文的粒度，后三层是高层任务的粒度。issue_done 一到，该 Lane 就能去装下一个任务，剩下的排空由 Completion RS 按 task_id 跟踪</text>
<text x="24" y="762" font-size="10.5" fill="#475569">连线：</text>
<path d="M70 758 L104 758" stroke="#2563eb" stroke-width="1.6" marker-end="url(#ad)"/>
<text x="112" y="762" font-size="10.5" fill="#475569">数据通路</text>
<path d="M220 758 L254 758" stroke="#d97706" stroke-width="1.6" marker-end="url(#ac)"/>
<text x="262" y="762" font-size="10.5" fill="#475569">任务分配</text>
<path d="M370 758 L404 758" stroke="#0d9488" stroke-width="1.6" marker-end="url(#at)"/>
<text x="412" y="762" font-size="10.5" fill="#475569">完成事件</text>
<text x="540" y="762" font-size="10.5" fill="#475569">CM → MM 本版本不支持：XBar 不提供 CH1 write ctrl 到 Matrix Memory 的连接</text>
</svg>
```

### 四条 Lane

数据面上是两个物理 Channel：

* `inbound_ch` / ch0：Router 进来的方向
* `outbound_ch` / ch1：出去，以及 MM → CM

每个 Channel 再拆成读写两半，一共四条 Lane：RD_CH0、WR_CH0、RD_CH1、WR_CH1。

* 每条 Lane **各自拥有 TaskQueue 和 Active Context**
* 一个高层任务落到一对 RD/WR 子上下文上
* 四条 Lane 的状态彼此独立：任一 Lane 的 Active Context 释放后就能从本 Lane 的 TaskQueue 激活下一个任务，不等配对的那一条

### Commit：配对接纳，不产生半任务

一个高层任务必须**同时**拿到三样：

1. 目标 RD Lane 的 TaskQueue 项
2. WR Lane 的 TaskQueue 项
3. Completion RS 项

* 任一侧没有空间，Commit 整体保持，Header 入口向 Router 反压
* 这条规则挡住“读已经开始、写还没有落脚点”的半任务
* Commit 同时完成地址展开：源地址、目的地址、按任务边界切分的元数据都在这一步算好

### 中间 Buffer 与 read-ahead

inbound buffer 与 outbound buffer 合计约 8 KB，按 256 B × 20～30 拍算，最大可掩盖 32 T 的延迟。

RD Lane 允许领先 WR Lane，领先量由三件事共同约束：

* 中间 Buffer 的可用 Credit
* 读的 outstanding 限额
* 可保留的任务边界数

也就是说：

* RD_CH0 可以在 WR_CH0 还没排空任务 N 时就开始接收任务 N+1
* 出口阻塞只通过 Credit 反压限制领先距离
* 不要求读写用同一个 Active Context

### Completion RS：按 task_id 合上

* 只在同一个 `task_id` 的 RD 与 WR 两侧条件都满足时产生 `task_done`
* 同一拍多个 Join 命中时**全部写入 Done Pending，不允许覆盖或丢失**，由 Done Pending 负责串行化
* 向 TS 的报告是 **exactly-once**

### 完成的六个层级

| 状态 | 粒度 | 定义 |
| - | - | - |
| `queued` | Lane 子上下文 | 已进 TaskQueue，尚未装载为 Active Context |
| `active` | Lane 子上下文 | 由对应 AGCU / Ctrl 执行，四条 Lane 状态彼此独立 |
| `issue_done` | Lane 子上下文 | 该 Lane 最后一个请求已 Fire，**允许该 Lane 提前激活下一任务** |
| `drained` | Lane / 任务边界 | 相关响应、Buffer 数据和外部副作用均已收敛 |
| `join_done` | 高层任务 | 同一 `task_id` 的 RD 与 WR 子上下文均满足完成条件 |
| `task_done` | 高层任务 | 完成结果进 Done Pending 并与 TS 成功握手 |

分这么多层，是因为“请求发完”和“事情办完”不是一回事：

* `issue_done` 只表示所有写请求已经发出，Lane 可以去干下一个任务
* 真正的完成还要等存储的写响应、读响应排空，以及 outstanding 清零

### 并发约束

| 场景 | 允许 | 约束 |
| - | - | - |
| RD_CH0 领先 WR_CH0 | 是 | 受 inbound buffer Credit、任务边界容量、Router AXI-Stream 背压约束 |
| RD_CH1 领先 WR_CH1 | 是 | 受 outbound buffer Credit、读 outstanding、出口背压约束 |
| CH0 与 CH1 同时执行 | 是 | 目的资源无冲突时独立推进，共享 DMA_XBAR 端口时按其仲裁规则 |
| Router→MM 与 MM→CM | 是 | 分别用 CH0 与 CH1，端口映射无冲突时可并行 |
| Router→CM 与 MM→CM | 受限 | **竞争 CoreMem 写路径**，由 CM 写仲裁器选择，未获选 Lane 保持 valid 和上下文 |
| 同一 Lane 内任务乱序 | 否 | TaskQueue 按序激活。read-ahead 允许 RD / WR 任务序号错位，但不改变各 Lane 内顺序 |
| 多个任务同拍 Join 完成 | 是 | Completion RS 捕获所有命中，Done Pending 负责序列化 |

***

## 三条数据流

### Inbound：Router → MM / CM

1. Router 以 AXI-Stream 发送 Header，Header Parser 在首拍锁存并检查 opcode / route、长度、身份字段和帧格式。
2. Header Parser 生成一个高层 Router 入站 Descriptor，请求 Commit 为 RD_CH0 与 WR_CH0 同时分配
   TaskQueue 项和完成跟踪项。
3. 两侧资源全部可用时 Commit 原子成功，RD_CH0 建立 Router 接收上下文，WR_CH0 建立 MM / CM 写入上下文。
   否则 Header 入口保持背压。
4. 后续 Payload 由 RD_CH0 控制写入 inbound buffer，附带 `task_id`、有效字节与任务边界信息。
   Buffer 满时通过 TREADY 向 Router 反压。
5. WR_CH0 从 inbound buffer 按任务边界取数，经 DMA_XBAR 写入目标 MM / CM。
6. RD_CH0 的帧接收结束和 WR_CH0 的写请求与响应 Drain 分别进入 Completion RS，
   二者按 `task_id` Join 后才向 TS 产生一次 `task_done`。

### Outbound：MM / CM → Router

选定的 Matrix Mem 或 Core Mem 是数据源，Router TX 是最终的流式接收端。

1. Commit 把 MM / CM → Router 或 MM → CM 的高层任务原子拆成 RD_CH1 与 WR_CH1 两个子上下文。
2. RD_CH1 的 AGCU 生成源端读地址，Read Ctrl 经 DMA_XBAR 读取 MM / CM，
   把返回数据连同 `task_id` 和边界元数据写入 outbound buffer。
3. WR_CH1 按固化的 Route 选择 Router TX 或 CoreMem Egress，从 outbound buffer 按任务边界发出。
4. Router TX 用 AXI-Stream 的 Valid / Ready / Keep / Last，CoreMem Egress 用 DMA_XBAR 写握手，
   两种出口的响应与 Drain 条件统一送进 Completion RS。
5. 同一任务的 RD_CH1 与 WR_CH1 都完成、且 outbound buffer 里该任务的数据已排空后，才生成 `task_done`。

### Inner：MM → CM

完全使用 CH1。

1. 任务先锁定 Matrix Mem 为读源、Core Mem 为写目标
2. Matrix 返回的数据经 DMA_XBAR RD、`ch1_rd_ctrl` 和 outbound buffer 到达 `ch1_wr_ctrl`
3. CH1 的出口绑定此时选 DMA WR1 而不是 Router TX
4. WR1 的硬件 route mask 只允许 CoreMem，因此不会把 CH1 的数据写回 Matrix Mem

存储读、Buffer 搬运和 WR1 写可以流水重叠，但每一级仍各自遵守 valid / ready。

***

## 一个任务搬的是什么

一次搬运的对象是一个 MSG 包。包在 Router 线上连成一条，进了 core 就**按内容拆成四份、各存各的地方**。

拆开存是因为读者不同：

* **data 与 scale**：给 MU 和 VU 算
* **topK**：给 MU 查专家
* **包头**：只在这个 token 再出核时用来重写路由

四份各有各的地址，这是下一节地址算法要分五类的根源。拆到哪里去、地址怎么来，一张图看完。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1360 790" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" role="img" aria-label="一个 DTE 任务搬的四类内容与它们各自的地址">
<title>一个任务搬的四类内容与它们各自的地址</title><defs>
<marker id="d" markerWidth="9" markerHeight="9" refX="7.5" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#2563eb"/></marker>
<marker id="c" markerWidth="9" markerHeight="9" refX="7.5" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#d97706"/></marker>
<marker id="g" markerWidth="9" markerHeight="9" refX="7.5" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#6b7280"/></marker>
</defs>
<rect width="1360" height="790" fill="#ffffff"/>
<text x="24" y="28" font-size="14.5" fill="#111827" font-weight="600">一个任务搬的四类内容与它们各自的地址</text>
<text x="24" y="47" font-size="10" fill="#475569">MSG 包在 Router 线上连成一条，进核后按内容拆成四份分开存。蓝色是硬件自己算出来的偏移，其余由软件配</text>
<rect x="24" y="62" width="1312" height="108" rx="6" fill="#eef6fb" stroke="#b7d4e6" stroke-width="1.1"/>
<text x="36" y="80" font-size="10.5" fill="#6b7280" font-weight="600">Router 线上：一个 MSG 包</text>
<rect x="44" y="92" width="100" height="46" rx="3" fill="#ffffff" stroke="#374151" stroke-width="1.1"/>
<text x="94.0" y="110" font-size="10" fill="#111827" text-anchor="middle">包头标记</text>
<text x="94.0" y="126" font-size="8.5" fill="#6b7280" text-anchor="middle">2 B</text>
<rect x="144" y="92" width="300" height="46" rx="3" fill="#ffffff" stroke="#374151" stroke-width="1.1"/>
<text x="294.0" y="110" font-size="10" fill="#111827" text-anchor="middle">Router 信息  4 B</text>
<text x="294.0" y="126" font-size="8.5" fill="#6b7280" text-anchor="middle">path_id 1 B · path_core_mask 2 B · rsv 1 B</text>
<rect x="444" y="92" width="100" height="46" rx="3" fill="#ffffff" stroke="#374151" stroke-width="1.1"/>
<text x="494.0" y="110" font-size="10" fill="#111827" text-anchor="middle">包长度</text>
<text x="494.0" y="126" font-size="8.5" fill="#6b7280" text-anchor="middle">2 B</text>
<rect x="544" y="92" width="190" height="46" rx="3" fill="#f8fafc" stroke="#374151" stroke-width="1.1"/>
<text x="639.0" y="110" font-size="10" fill="#111827" text-anchor="middle">软件辅助信息</text>
<text x="639.0" y="126" font-size="8.5" fill="#6b7280" text-anchor="middle">0～16 B</text>
<rect x="734" y="92" width="460" height="46" rx="3" fill="#f8fafc" stroke="#374151" stroke-width="1.1"/>
<text x="964.0" y="110" font-size="10" fill="#111827" text-anchor="middle">业务数据</text>
<text x="964.0" y="126" font-size="8.5" fill="#6b7280" text-anchor="middle">0～(64 K − 24) B</text>
<path d="M44 148 L544 148" stroke="#6b7280" stroke-width="1"/>
<text x="294" y="161" font-size="8.5" fill="#6b7280" text-anchor="middle">前三段合起来是包头，24 B 封顶</text>
<text x="1140" y="161" font-size="8.5" fill="#6b7280" text-anchor="middle">单个 DTE 任务的搬运量上限 32 KB = 256 B × 128 拍</text>
<path d="M680 170 L680 194" fill="none" stroke="#6b7280" stroke-width="1.8" marker-end="url(#g)"/>
<rect x="500" y="196" width="360" height="62" rx="4" fill="#ffffff" stroke="#374151" stroke-width="1.2"/>
<text x="510" y="215" font-size="11.5" fill="#111827" font-weight="600">DTE：按内容拆成四份</text>
<text x="510" y="231.0" font-size="9" fill="#475569">每份的目的地址各算各的，规则见下面两栏</text>
<rect x="248" y="206" width="228" height="42" rx="4" fill="#ffffff" stroke="#c9bade" stroke-width="1.2"/>
<text x="362" y="223" font-size="10" fill="#111827" text-anchor="middle">TS 给的 stream_id / task_id</text>
<text x="362" y="238" font-size="8.5" fill="#6b7280" text-anchor="middle">五类地址的 stream 偏移都用它算</text>
<path d="M476 227 L498 227" fill="none" stroke="#2563eb" stroke-width="1.6" marker-end="url(#d)"/>
<path d="M560 258 L560 278" fill="none" stroke="#d97706" stroke-width="1.6" marker-end="url(#c)"/>
<path d="M800 258 L800 278" fill="none" stroke="#d97706" stroke-width="1.6" marker-end="url(#c)"/>
<rect x="24" y="280" width="656" height="398" rx="6" fill="#fdf6ec" stroke="#e4c99b" stroke-width="1.1"/>
<text x="36" y="298" font-size="10.5" fill="#6b7280" font-weight="600">计算 core：包头进 Hmem，data 与 scale 进 Core Mem，topK 进 MU</text>
<rect x="40" y="306" width="624" height="88" rx="4" fill="#ffffff" stroke="#374151" stroke-width="1.2"/>
<text x="50" y="325" font-size="11" fill="#111827" font-weight="600">Hmem · sw_header_table（软件包头）</text>
<text x="50" y="341.0" font-size="8.8" fill="#475569">16 stream × 64 task 项，每项 16 B，合 16 KB</text>
<text x="50" y="353.5" font-size="8.8" fill="#2563eb">→ 基址 + stream_id × 1 KB + task_id × 16 B</text>
<text x="50" y="366.0" font-size="8.8" fill="#475569">两级偏移都是硬件固定步长，软件只配基址</text>
<rect x="40" y="404" width="624" height="88" rx="4" fill="#ffffff" stroke="#374151" stroke-width="1.2"/>
<text x="50" y="423" font-size="11" fill="#111827" font-weight="600">Hmem · core_mask_table（硬件包头）</text>
<text x="50" y="439.0" font-size="8.8" fill="#475569">动态部分 path_core_mask 16 项 × 2 B = 32 B，DTE core 配</text>
<text x="50" y="451.5" font-size="8.8" fill="#475569">静态部分 {path_id, size} 64 项按 task_id 索引，boot 配</text>
<text x="50" y="464.0" font-size="8.8" fill="#2563eb">→ header_base_addr + stream_id × 包头长度</text>
<rect x="40" y="502" width="624" height="88" rx="4" fill="#ffffff" stroke="#374151" stroke-width="1.2"/>
<text x="50" y="521" font-size="11" fill="#111827" font-weight="600">Core Mem · data 区与 scale 区</text>
<text x="50" y="537.0" font-size="8.8" fill="#2563eb">→ data：dst_base_addr + stream_id × stream_stride</text>
<text x="50" y="549.5" font-size="8.8" fill="#2563eb">→ scale：scale_base_addr + stream_id × scale_stride</text>
<text x="50" y="562.0" font-size="8.8" fill="#475569">data_len 只算 data；scale 长度硬件按 data_len / 32 算</text>
<rect x="40" y="600" width="624" height="72" rx="4" fill="#ffffff" stroke="#374151" stroke-width="1.2"/>
<text x="50" y="619" font-size="11" fill="#111827" font-weight="600">MU · topK_ep_table（或 Core Mem 独立空间）</text>
<text x="50" y="635.0" font-size="8.8" fill="#2563eb">→ topk_base_addr + stream_id × 256 B，每 stream 上限 256 B</text>
<text x="50" y="647.5" font-size="8.8" fill="#475569">长度硬件按 router_ep_count × 6 B 算，每项 {expert_id 2 B, weight 4 B}</text>
<rect x="696" y="280" width="640" height="398" rx="6" fill="#eef8f4" stroke="#a8d8c6" stroke-width="1.1"/>
<text x="708" y="298" font-size="10.5" fill="#6b7280" font-weight="600">B core / R core：三类连排进 Matrix Mem，包头与 shareMem 单放</text>
<rect x="712" y="306" width="608" height="100" rx="4" fill="#ffffff" stroke="#374151" stroke-width="1.2"/>
<text x="722" y="325" font-size="11" fill="#111827" font-weight="600">Matrix Mem · topK + scale + data 连排</text>
<text x="722" y="341.0" font-size="8.8" fill="#475569">src_addr / dst_addr 直接当最终地址用，不加 stream 偏移</text>
<text x="722" y="353.5" font-size="8.8" fill="#475569">data_len 盖住 topK + scale + data 三类的总长</text>
<text x="722" y="366.0" font-size="8.8" fill="#475569">顺序由 GPU 侧按 pattern 排好，DTE 不重排</text>
<text x="722" y="378.5" font-size="8.8" fill="#475569">位置由软件算准，配任务时就已经确定</text>
<rect x="712" y="418" width="608" height="76" rx="4" fill="#ffffff" stroke="#374151" stroke-width="1.2"/>
<text x="722" y="437" font-size="11" fill="#111827" font-weight="600">Core Mem · 包头独立空间</text>
<text x="722" y="453.0" font-size="8.8" fill="#475569">软件包头与硬件包头都存在这里，不进 Hmem</text>
<text x="722" y="465.5" font-size="8.8" fill="#475569">出核时同样按 task_id 改写 path_id 与 size</text>
<rect x="712" y="506" width="608" height="88" rx="4" fill="#ffffff" stroke="#374151" stroke-width="1.2"/>
<text x="722" y="525" font-size="11" fill="#111827" font-weight="600">shareMem</text>
<text x="722" y="541.0" font-size="8.8" fill="#475569">sharemem_waddr 直接给表项地址，不加偏移</text>
<text x="722" y="553.5" font-size="8.8" fill="#475569">表项内容 sharemem_data：user_id 加 valid / invalid 标志</text>
<text x="722" y="566.0" font-size="8.8" fill="#475569">token 搬进 Matrix Mem 后置 valid，搬出后置 invalid，软件维护</text>
<text x="1016" y="628" font-size="9" fill="#6b7280" text-anchor="middle">两边的差别只在存到哪里。包在 Router 线上的格式是同一个，</text>
<text x="1016" y="646" font-size="9" fill="#6b7280" text-anchor="middle">DTE 按 core 的类型决定拆到哪几处去</text>
<rect x="24" y="692" width="1312" height="92" rx="6" fill="#f5f1fa" stroke="#c9bade" stroke-width="1.1"/>
<text x="36" y="710" font-size="10.5" fill="#6b7280" font-weight="600">谁配基址，谁算偏移</text>
<rect x="40" y="720" width="640" height="58" rx="4" fill="#ffffff" stroke="#374151" stroke-width="1.2"/>
<text x="50" y="739" font-size="10.5" fill="#111827" font-weight="600">软件配（RV core 写 DSA 寄存器，或 boot 阶段配）</text>
<text x="50" y="755.0" font-size="8.8" fill="#475569">五类基址、stream_stride、scale_stride、data_len、router_ep_count、</text>
<text x="50" y="767.5" font-size="8.8" fill="#475569">以及 transfer_mode 与几个开关；同一段代码对所有用户配同一套基址</text>
<rect x="696" y="720" width="640" height="58" rx="4" fill="#ffffff" stroke="#2563eb" stroke-width="1.2"/>
<text x="706" y="739" font-size="10.5" fill="#111827" font-weight="600">硬件算（用 TS 给的 stream_id / task_id）</text>
<text x="706" y="755.0" font-size="8.8" fill="#475569">五类地址的 stream 偏移、软件包头的 task 级偏移、</text>
<text x="706" y="767.5" font-size="8.8" fill="#475569">scale 长度 data_len / 32、topK 长度 router_ep_count × 6 B</text>
</svg>
```

### MSG 包在 Router 线上的样子

| 字段 | 长度 |
| - | - |
| 包头标记 | 2 B |
| Router 信息 | 4 B = `path_id` 1 B + `path_core_mask` 2 B + reserved 1 B |
| 包长度 | 2 B |
| 软件辅助信息 | 0～16 B |
| 业务数据 | 0～(64 K − 24) B |

* 前四项合起来是包头，24 B 封顶，业务数据的上限由此得来
* **单个 DTE 任务的搬运量上限是 32 KB**（256 B × 128 拍），比包上限小，超过的要拆成多个任务包下发

### 四类内容的长度与去处

| 内容 | 长度 | 计算 core 存哪 | B core / R core 存哪 |
| - | - | - | - |
| 硬件包头 | 静态部分 `path_id` + `size`；动态部分 `path_core_mask` 2 B | DTE 内 Hmem 的 `core_mask_table`，16 项按 `stream_id` 索引，共 32 B | Core Mem 独立空间 |
| 软件包头 | 每项 16 B | DTE 内 Hmem 的 `sw_header_table`，16 stream × 64 task 项，共 16 KB | Core Mem 独立空间 |
| scale | `data_len / 32`，只有 MXFP8 有 | Core Mem 的 scale 区 | Matrix Mem，与 data 连排 |
| topK | `router_ep_count × 6 B`，每项 `{expert_id 2 B, weight 4 B}`，每 stream 上限 256 B | MU 内 `topK_ep_table`，或 Core Mem 独立空间，由软件配 | Matrix Mem，与 data 连排 |
| data | `data_len` | Core Mem 按 stream 分片 | Matrix Mem |

几处对得上的地方：

* Hmem 的 16 KB + 32 B 就是这两张表加起来：软件包头 16 × 64 × 16 B = 16 KB，硬件包头动态部分 16 × 2 B = 32 B
* 硬件包头的静态部分另有 64 项，按 `task_id` 索引，boot 阶段配好；动态部分由 DTE core 配
* B core 与 R core 的 scale、topK、data 由 GPU 侧按 pattern 排好序送来，DTE 不重排顺序

`data_len` 这个寄存器在不同方向上盖的范围不一样：

* **Router ↔ Matrix Mem**：topK + scale + data 的总长
* **Router ↔ Core Mem**：只是 data 的长度，scale 与 topK 的长度另算

原因就在上面那张表：B core / R core 侧三类内容在 Matrix Mem 里连排、一次搬完；计算 core 侧三类内容各去各的地方，长度得分开给。

### 什么时候存，什么时候丢

* **进核**
  * 计算 core 只在这个 token 需要分配新 `stream_id` 时才存包头（`hw_header_op = 1`）
  * 中间环节的 reduce 与 concat 任务直接丢弃（`hw_header_op = 0`）
  * 广播 token 进核必然带 topK，必须存下来
* **出核**
  * DTE 按 `task_id` 查出 `path_id` 与 `size` 改写进硬件包头
  * `path_core_mask` 在 core 内没有修改接口，软件包头不改
  * 计算结果出核不带 topK
* 支持纯包头任务（`data_len = 0`），进出 core 都可以

### shareMem 写

* 任务数据传输完成后，按 `sharemem_waddr` / `sharemem_data` 写 shareMem，然后通知 TS
* 只在 B core 与 R core 使用
* 存 user_id 与 token entry 的 valid 标志，由软件维护：token 搬入 Matrix Mem 后置 valid，搬出后置 invalid

***

## 软件怎么配一个任务

DTE 有两个任务入口：

* **Router 搬入的任务**：Descriptor 编码在 AXI-Stream 帧的 Header 里，由 Header Parser 直接解析后提交
* **其余任务**：由 RV core 配
* 两个入口在 Commit 边界汇成同一套内部任务模型

这一节讲 RV core 这一侧：写哪些寄存器、每个参数管什么、地址怎么算出来、五个方向各自怎么配。

### 四个寄存器与 Doorbell

软件用 `dsawi` / `dsaw` 指令写四个寄存器，**必须最后写 Doorbell**：

```asm
# 先配 ADDR 和 TD 寄存器（两个都必须配）
dsawi.d TASK_CFG_ADDR(addr0) TASK_CFG_TD(addr1) data0 data1
# 再写 Doorbell，PACK 寄存器随之自动写入
dsawi.s TASK_CFG_TRG data0
```

`TASK_CFG_TD` 位域：

| 位域 | 名称 | 含义 |
| - | - | - |
| 31:30 | `src_sel` | 00=Router，01=MM，10=CM，11=Reserve |
| 29:28 | `dst_sel` | 同上 |
| 27:26 | `queue_sel` | b0=ch0，b1=ch1 |
| 25:19 | `cnt` | 搬运总拍数，**单任务最大 256B × 128 = 32 KB** |
| 18:11 | `mask` | 256B 一拍，按 32B 粒度 mask，位为 0 表示该 32B 无效 |
| 10:5 | `path_id` | 当前任务打包需要的 PathID |
| 4:1 | `task_pack_id` | 一个任务可由多个任务包组成，从 0 计数，**包之间不保证顺序执行** |
| 0 | `last` | 是否当前 TaskID 的最后一个任务包 |

`TASK_CFG_PACK` 位域：

| 位域 | 名称 | 含义 |
| - | - | - |
| 31:16 | `UserID` | 用户 ID |
| 15:12 | `stream_id` | 等同 SlotID，用户在 core 内的 ID |
| 11:6 | `task_id` | 最大 64 |
| 1 | `no_ack` | 标记是否需要返回 Ack |
| 0 | `last` | 当前 Task 的最后一个宏指令 |

打包信息只有在 `dst_sel` 是 Router 时才有效。

### 一次配置要写的参数

**两套软件接口并存、尚未对齐**：

* 上面四个寄存器出自 DTE MAS，是硬件那一侧的接口
* 软件这一侧另有一组按名字寻址的 DSA 寄存器，出自软件计算流程详细评估，字段与 `TASK_CFG_*` 没有对应关系

下面按后一套讲，用 `dsawi.s` 写一个、`dsawi.d` 写两个，分五组。

**第一组，模式与开关**，或进同一个寄存器一次写下：

| 参数 | 作用 |
| - | - |
| `transfer_mode` | 选五个搬运方向之一。它决定用 CH0 还是 CH1、哪一对 Lane、出口接 Router TX 还是 DMA WR1 |
| `scale_valid` | 这个任务带不带 scale。只有 MXFP8 的数据有 |
| `topK_valid` | 带不带 topK |
| `hw_header_op` | 1 = 把包头存进 Hmem，0 = 丢弃 |
| `task_last` | 一个 task 拆成几笔搬运时最后一笔置 1，只有带这个标记的那一笔完成后才通知 TS |
| `smem_valid` | 完成后要不要写 shareMem |
| `router_ep_count` | 本 token 激活的专家数。topK 的搬运长度等于它乘 6 B |

**第二组，数据的地址与长度**：

| 参数 | 作用 |
| - | - |
| `src_addr` / `dst_addr` | Matrix Mem 一侧的地址，直接当最终地址用，不加偏移 |
| `src_base_addr` / `dst_base_addr` | Core Mem 一侧的基址，硬件在它上面加 stream 偏移 |
| `stream_stride` | 每个 stream 在 Core Mem 里占多少字节。MM → CM 方向两端各有一个，写 `src_stream_stride` 与 `dst_stream_stride` |
| `data_len` | 搬运长度，单位字节。它盖住哪几类内容随方向变 |

**第三组，包头、scale 与 topK 的地址**：

| 参数 | 作用 |
| - | - |
| `header_addr` | Matrix Mem 一侧的包头地址。包头长度固定，不用配，也不加偏移 |
| `header_base_addr` | Core Mem 一侧的包头基址，硬件加 stream 偏移 |
| `header_src_addr` / `header_dst_base_addr` | MM → CM 方向的包头两端，源侧直给、目的侧给基址 |
| `scale_base_addr` / `scale_stride` | scale 的基址与每 stream 步长。搬运长度硬件按 `data_len / 32` 算，不用配 |
| `topk_base_addr` | topK 的基址。步长硬件固定 256 B，搬运长度硬件按 `router_ep_count × 6 B` 算 |

**第四组，shareMem 表项**：

* `sharemem_waddr`：表项地址
* `sharemem_data`：表项内容，user_id 加 valid / invalid 标志

**第五组，任务身份**：`UserID`、`stream_id`、`task_id`、`no_ack`、`last`。写 Doorbell 时由硬件一并写进 `TASK_CFG_PACK`，软件不单独配。

另有一组不随任务变的控制与观测寄存器：

| 寄存器 | 作用 |
| - | - |
| `SYS_CTRL` / `DTE_CTRL` | 全局时钟门控、软复位、任务启动触发、单步调试使能；清空缓冲 |
| `SYS_STATUS` / `DTE_STATUS` | 主状态机反馈（Idle / Running / Error / Stop）与忙状态 |
| `EXCEPT_STATUS` / `EXCEPT_MASK` | 访存越界、非对齐、ECC 错、搬运异常；中断默认屏蔽，写 0 打开 |
| `EXCEPT_CFG_ADDR` / `EXCEPT_CFG_TD` / `EXCEPT_CFG_PACK` | 出异常时硬件自动抓下当时的三个任务配置寄存器，只读 |
| `PMU_CTRL` 与 11 个 `PMU_CNT_*` | 搬运原语数；CH0 / CH1 各自的执行周期、数据量、XBar 单次传输时间、完成交易数、完成任务数 |

### 地址怎么算

一条规矩贯穿五类地址：**软件只配基址，偏移由硬件用 `stream_id` 算出来**。

* `stream_id` 是 TS 建 stream 表项时定的，随任务一起给到 DTE
* 软件不需要知道这个 token 落在 Core Mem 的哪一片
* 同一段代码配同一个基址，不同用户自动落到各自的片上

| 内容 | 地址 | 基址谁配 | 偏移用什么索引 | 步长 |
| - | - | - | - | - |
| data，Core Mem 侧 | `base_addr + stream_id × stream_stride` | RV core | `stream_id` | 软件配 |
| data，Matrix Mem 侧 | `src_addr` / `dst_addr` | RV core | 不加偏移 | — |
| 硬件包头 | `header_base_addr + stream_id × 包头长度` | RV core | `stream_id` | 硬件固定 |
| 软件包头 | 基址 + `stream_id × 1 KB + task_id × 16 B` | boot 阶段 | `stream_id` 与 `task_id` 两级 | 硬件固定 |
| scale | `scale_base_addr + stream_id × scale_stride` | RV core | `stream_id` | 软件配 |
| topK | `topk_base_addr + stream_id × 256 B` | RV core | `stream_id` | 硬件固定 256 B |
| shareMem 表项 | `sharemem_waddr` | RV core | 不加偏移 | — |

**Matrix Mem 一侧不加 stream 偏移，Core Mem 一侧加**：

* Matrix Mem 放的是模型 weight，以及从 GPU 按 pattern 排好序送来的 token。位置软件自己算准，DTE 不重排
* Core Mem 按 stream 切成 16 片，一片对应一个在飞的用户。谁占哪片由 TS 定，软件配的时候还不知道

**软件包头的两级索引**：

* `stream_id × 1 KB` 里的 1 KB 等于 64 × 16 B，一个 stream 名下 64 个 task 各占 16 B
* 16 个 stream 乘 1 KB 正是 `sw_header_table` 的 16 KB
* 这一级按 Hmem 容量与包头存储图推出，寄存器序列本身只给到 stream 一级

**搬运长度有两项不用软件配，硬件自己算**：

* scale：`data_len / 32`，32 个元素共用一个 scale
* topK：`router_ep_count × 6 B`

### 五个方向的配置例子

五个方向配置项不同，差别集中在四处：

1. 数据地址加不加 stream 偏移
2. `data_len` 盖不盖 scale 与 topK
3. scale 与 topK 要不要单独配地址
4. 要不要写 shareMem

| 方向 | 数据地址 | `data_len` 含义 | scale / topK 单独配 | shareMem | 典型场景 |
| - | - | - | - | - | - |
| Router → MM | `dst_addr` 直给 | topK + scale + data 总长 | 否 | 写，置 valid | CPU 发来的 token 进 B core；EP 间 token 进 R core |
| MM → Router | `src_addr` 直给 | topK + scale + data 总长 | 否 | 写，置 invalid | B core 把 token 广播到别的 core |
| Router → CM | `dst_base_addr` 加偏移 | 只有 data | 是 | 否 | init token 进核；reissue；EPTP-NK 切分时进 concat & reduce core |
| CM → Router | `src_base_addr` 加偏移 | 只有 data | 是 | 否 | reissue；EPTP-NK 切分时 FC1 / FC3 在 Router 上做 reduce、FC2 搬到 concat core |
| MM → CM | 源直给，目的加偏移 | 只有 data | 否 | 写，置 invalid | R core 里 EP 间的 reduction |

**Router → MM**

```c
void data_in_config() {
  // 模式与开关：方向 router → matrix mem
  //   hw_header_op  是否把包头存进 Hmem
  //   task_last     一个 task 拆成多笔搬运时只有最后一笔置 1，由它给 TS 报 done
  //   smem_valid    结束后更新 shareMem
  dsawi.s transfer_mode | scale_valid | topK_valid | hw_header_op | task_last | smem_valid, X

  // 数据：Matrix Mem 侧地址直给，不加 stream 偏移
  dsawi.s dst_addr, X
  // 长度，单位字节，这个方向上盖住 topK + scale + data
  dsawi.s data_len, X

  // 包头：长度固定，地址直给
  dsawi.s header_addr, X

  // shareMem：表项记这个 token 在 Matrix Mem 里的信息，置 user_id 与 valid
  dsawi.s sharemem_waddr, X
  dsawi.s sharemem_data, X
}
```

**MM → Router**

```c
void data_out_config() {
  // 模式与开关：方向 matrix mem → router
  dsawi.s transfer_mode | scale_valid | topK_valid | hw_header_op | task_last | smem_valid, X

  // 数据：Matrix Mem 侧地址直给
  dsawi.s src_addr, X
  // 长度盖住 topK + scale + data
  dsawi.s data_len, X

  // 包头：path_id 在 boot 阶段按 task_id 初始化进 DTE，出核时由硬件填进包头
  dsawi.s header_addr, X

  // shareMem：token 搬出后把表项置 invalid
  dsawi.s sharemem_waddr, X
  dsawi.s sharemem_data, X
}
```

**Router → CM**

```c
void data_in_config() {
  // 模式与开关：方向 router → core mem
  //   router_ep_count  本 token 激活的专家数，决定 topK 搬多少字节
  dsawi.s transfer_mode | scale_valid | topK_valid | hw_header_op | task_last | smem_valid | router_ep_count, X

  // 数据：Core Mem 侧给基址，硬件算 dst_addr = dst_base_addr + stream_id × stream_stride
  dsawi.s dst_base_addr, X
  // 长度只算 data，不含 topK 与 scale
  dsawi.s data_len, X
  dsawi.s stream_stride, X

  // 包头：dst_addr = header_base_addr + stream_id × 包头长度
  dsawi.s header_base_addr, X

  // scale：只有 MXFP8 有，存在 Core Mem
  //   dst_addr = scale_base_addr + stream_id × scale_stride
  //   搬运长度 = data_len / 32，硬件算
  dsawi.s scale_base_addr, X
  dsawi.s scale_stride, X

  // topK：dst_addr = topk_base_addr + stream_id × 256B，每个 stream 上限 256B
  //   搬运长度 = router_ep_count × 6B，每项 {expert_id 2B, weight 4B}
  dsawi.s topk_base_addr, X
}
```

**CM → Router**

```c
void data_out_config() {
  // 模式与开关：方向 core mem → router
  dsawi.s transfer_mode | scale_valid | topK_valid | hw_header_op | task_last | smem_valid | router_ep_count, X

  // 数据：src_addr = src_base_addr + stream_id × stream_stride
  dsawi.s src_base_addr, X
  // 长度只算 data
  dsawi.s data_len, X
  dsawi.s stream_stride, X

  // 包头：src_addr = header_base_addr + stream_id × 包头长度
  dsawi.s header_base_addr, X

  // scale：两个寄存器一条指令写完
  dsawi.d scale_base_addr, scale_stride, X1, X2

  // topK
  dsawi.s topk_base_addr, X
}
```

**MM → CM**

```c
void data_inner_config() {
  // 模式与开关：方向 matrix mem → core mem，不过 Router，走 CH1 内部通路
  dsawi.s transfer_mode | scale_valid | topK_valid | hw_header_op | task_last | smem_valid | router_ep_count, X

  // 源：Matrix Mem 侧地址直给
  dsawi.s src_addr, X
  dsawi.s data_len, X

  // 目的：Core Mem 侧给基址，dst_addr = dst_base_addr + stream_id × stream_stride
  dsawi.s dst_base_addr, X
  dsawi.s stream_stride, X

  // 包头：R core 的包头存在 Core Mem，这一笔顺带把它搬进 Hmem
  //   源侧地址直给，目的侧 dst_addr = header_dst_base_addr + stream_id × 包头长度
  dsawi.s header_src_addr, X
  dsawi.s header_dst_base_addr, X

  // shareMem：token 从 Matrix Mem 搬走后置 invalid
  dsawi.s sharemem_waddr, X
  dsawi.s sharemem_data, X
}
```

### TS 直接启动

**MAS 已删掉这一条**（原第 2 条，标 P1 优先级）。下面记的是它被删之前的内容，建模不按它做。

它它绕过 RV core，由 TS 直接触发 DTE DSA，DSA 用 `stream_id` / `task_id` 索引内部的静态参数查找表取参数。

初始化要走完七步：

1. SCP 复位 DTE
2. 配全局静态寄存器
3. 配 task LUT
4. 配 stream 相关表
5. 配 header 与 topK 参数
6. 使能 TS 直接触发模式
7. DTE 返回 `init_done`，TS 才允许直接启动

触发时：

1. 检查 `stream_id` / `task_id` 合法性
2. 按 `task_id` 查 task_mode_table，按 `stream_id` 查 stream 相关表
3. 取得 length 与 path_id 等动态字段
4. 算出源与目的地址，生成内部搬运描述符
5. 选 CH0 / CH1 或内部通道启动

启动方式只有“DTE core 配置任务给 DSA”这一种。

### 双 Bank 与优先级

Commit 提供两个配置 Bank，**Bank0 优先于 Bank1**：

* 都空闲时：Router 的配置进 Bank0，RV core 的配置进 Bank1
* 只剩一个 Bank 而两者竞争时：**优先配置 Router 信息**

***

## 与 Router 的边界

### 入站帧格式

* **一帧一任务**：同一个 AXI-Stream Frame 只属于一个 Router→MM 或 Router→CM 任务，不允许任务间交织
* **首拍固定为 Header**：DTE 靠“上一帧 TLAST 已接受”判断下一拍是新 Header，不依赖 Start-of-Frame 信号；
  建议 Header 限定在一个 256 B beat 内
* Header Commit 成功后才允许 Payload Fire，TLAST 标识最后一个 Payload beat；
  `byte_count` 为 0 时可由 Header beat 同时携带 TLAST
* TKEEP 按字节粒度生效，每个 Payload Fire 累计 TKEEP 有效字节，TLAST 时与 `byte_count` 比较
* **非法 Header 进入 Drop Frame 流程**：不生成 Descriptor、不发存储器请求，只消费到 TLAST 以恢复帧边界

DTE 解析 Header 时用到的逻辑字段与各自的检查：

| 逻辑字段 | 用途 | 检查 |
| - | - | - |
| `version` / `header_len` | Header 格式版本与有效长度 | 版本受支持，`header_len` 不超过首拍有效字节 |
| `packet_type` / `route` | 标识这是 DTE 搬入任务，并选 Router → MM 还是 Router → CM | 其他 Route 在 Header Parser 阶段拒绝，不产生外部请求 |
| `dst_addr` | 目的存储的基地址 | 在目的端地址范围内，满足对齐 |
| `byte_count` | Payload 总有效字节数 | 与后续 Payload 的 TKEEP 累计值及 TLAST 位置一致 |
| `task_id` / `stream_id` | 建立任务身份与完成归属 | 未完成上下文中不得重复占用 |
| `attributes` / `reserved` | 后续控制属性与格式扩展 | 未定义位为约定默认值，当前不据此改变基线 Route 行为 |

这张表与 MSG 包的字节布局是同一个 Header 的两种写法，字段对不上：

* 这里有 `version` / `header_len` / `packet_type` / `attributes`
* MSG 包结构里有 `path_id` / `path_core_mask`
* DTE MAS 声明具体位域以 Router 接口规范为准，最终以那一份为准

### core 侧承担的桥接职责

Router 与 core 之间**不做独立的桥接模块**，按耦合关系把逻辑拆到 core 与 Router 两侧分布实现。落到 DTE 这一侧的是：

* **进来的方向**：Router 与 core 之间只用单个 VC 调度，多 VC 到单 VC 的映射由 Router 侧硬件固化完成
  * DTE 侧感知单 VC buffer 的缓存状态并据此启动搬运
  * 解析包信息，搬完按 flit 释放 VC credit
* **出去的方向**：DTE 侧按 VC0～3 多线程调度维护多个 VC buffer
  * 用它吸收整包流量，完成 core 与 Router 之间的协议转换
  * 出去之前查两类业务层 credit：下游的 coremem credit 与 reduce credit
  * 两类 credit 都分方向，要先查 routing table 确定方向，再取对应方向的 credit
* **credit 回程**：解析本级 Router 各方向传进来的 core credit release，按其中的 action 信息决定是否同步更新 core 内的 stream 表状态

***

## 参数

| 项目 | 数量 / 容量 | 说明 |
| - | - | - |
| 物理数据 Channel | 2 | `inbound_ch` / ch0 与 `outbound_ch` / ch1 |
| TaskQueue | 16 | 深度待评估 |
| 中间 Buffer | 约 8 KB | inbound + outbound，约 256B × (20～30) T，最大可掩盖 32 T 延迟 |
| 与 Cmem 接口宽度 | 256 B/T | 双向；DTE MAS 与 Cmem MAS 口径一致 |
| 与 router 接口宽度 | 256 B | Data（看不到 scale），双向 |
| Hmem | 16 KB + 32 B | `sw_header_table` + `core_mask_table` |
| LUT | 192 B | `path_id_table` + `task_len_table` |
| 单任务最大搬运量 | 32 KB | 256 B × 128 拍 |

### 性能剖析口径

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

## 取舍

* **为什么一个任务要拆成读写两半**
  * 搬运的两端节奏不同：Router 侧什么时候来数据由上游决定，存储侧要抢 bank
  * 绑成一体，任一端卡住另一端就空转
  * 拆开之后两端各自按自己的节奏排队，中间 buffer 吸收速度差
  * 代价是要额外维护配对关系和 Join 逻辑
* **为什么 Commit 必须两侧同时拿到资源**
  * 只拿到读侧就开始收数据，数据进了 buffer 却没有写侧上下文可以落地
  * buffer 会被一个无法推进的任务占住
  * 两侧同时拿到才接纳，把这种半途卡死挡在入口
* **为什么单向 2 通道而不是 3 Lane**
  * 把 Matrix Mem、Core Mem、Router 各看作一个读写 Resource 时，理想情况是 3 Lane，定性上会带来性能提升
  * 代价是 XBar 面积近似 *N_in × N_out × W*，偏大
  * 带宽不是约束：需求 190～320 GB/s，而 Matrix Mem 8192 GB/s、Core Mem 512 GB/s、Router 双向各 256 GB/s
  * 物理布局上 DTE 位于 Router 与 Cmem 之间、Mmem 在 Cmem 上方，按物理相邻关系合并通路的代价最小
  * 3 Lane 方案仍在评估，收益要靠建模与仿真定量给出
* **为什么 `issue_done` 就允许 Lane 走下一个任务**
  * Lane 的资源是 Active Context，不是在途事务
  * 最后一个请求发出后 Lane 本身已经空出来，剩下的响应排空由 Completion RS 按 `task_id` 跟踪
  * 若等到全部 drain 才放行，外部响应延迟会直接算进 Lane 的占用时间

***

配图：[DTE DSA 12 张](<Bach/04_四、MAS（Micro Architecture SPEC）/08_DTE DSA>)：顶层框图、内部结构、包头存储与访存、TS 快速启动流程、Inbound/Outbound/Inner Flow、Concurrency、Commit、通路方案评估

内嵌表格：[DTE DSA 4 子表](_sheets/_Dx7Ns6)（Reduction only 场景下各通路（R→M 搬入 / C→R 搬出 / M→C 线上归约）的带宽占比、DTE 参数含义、寄存器偏移与位宽）

来源：`04_四、MAS/08_DTE DSA.md`、`Bach软件文档库/04_总体设计/05_软件计算流程详细评估.md`（DTE 章）
