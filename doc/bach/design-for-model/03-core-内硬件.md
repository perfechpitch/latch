# 第 3 章　Core 内硬件

**模式**：design（陈述当前设计，动机与取舍收在各节的“取舍”段落里）

定义 Router、Bach Core 顶层、TS 任务调度器、RV Core 四个模块的结构与行为。

* 这四者构成 core 内的控制通路，决定多用户如何在三条执行链上流水
* 性能建模要精确复现的就是这一部分

《Bach 硬件设计建模参考》第 3 章，全套目录见 [README](README.md)。

***

## Router：片上交换与归约中心

Router 是 chip 内 core 阵列的数据交换与**片上归约**中心，物理上位于 chip 中部，每个 core 一个。

* Crossbar 五路输入：三个 R2R 方向端口 `left` / `right` / `mid`，接本 core 的 `local` 端口，ReduceModule
* 同时承担三件事：包的路由转发、Stream 与 VC 两级流控、Reduce 计算

下游收不下有三种不同的原因，Router 用三层互不复用的 credit 分别管：

* **VC Credit**：下游 VC Buffer 的空槽
* **stream credit**：目标 core 的 Core Mem 空间
* **Reduce Credit**：下游 ReduceModule 的上下文

展开在[《Router 片上交换与归约》](03-router-片上交换与归约.md)：六级流水线与单跳延迟、RouterTable 的字段与三份副本、按任务类型分的走法、三类 credit 的管理方式、ReduceModule 与用户退休、跳过与 C2C Bridge。

***

## Bach Core 顶层

### 组成

```svg
<svg viewBox="0 0 1200 900" width="1200" height="900" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="Bach Core 顶层结构">
<title>Bach Core 顶层结构</title>
<rect width="1200" height="900" fill="#ffffff"/>
<defs><marker id="kea" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="keas" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker><marker id="keai" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#3f4451"/></marker><marker id="keais" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#3f4451"/></marker><marker id="keab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="keabs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="kear" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="kears" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="keac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="keacs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="keap" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="keaps" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker></defs>
<text x="30" y="26" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="start">Bach Core 顶层</text>
<text x="30" y="46" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">布局照 MAS TOP 与系统软件需求分析里的画法：TS 在最上，Router 在最下，core_noc + debug_noc 绕一圈；Core Mem 容量按 Cmem MAS 记 1 MB + 32 KB</text>
<rect x="560" y="56" width="140" height="36" rx="6" fill="#eceef1" stroke="#6b7280" stroke-width="1.3"/>
<text x="630" y="79" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="700" text-anchor="middle">SCP</text>
<rect x="980" y="56" width="100" height="36" rx="6" fill="#eceef1" stroke="#6b7280" stroke-width="1.3"/>
<text x="1030" y="79" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="700" text-anchor="middle">DTM</text>
<rect x="120" y="120" width="960" height="680" rx="14" fill="#ffffff" stroke="#3f4451" stroke-width="1.6"/>
<text x="138" y="140" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#5c6370" font-weight="600" text-anchor="start">Bach Core</text>
<rect x="225" y="150" width="750" height="610" rx="8" fill="none" stroke="#ede9fe" stroke-width="9"/>
<rect x="225" y="150" width="750" height="610" rx="8" fill="none" stroke="#7c3aed" stroke-width="1.0" stroke-dasharray="3 3"/>
<text x="600" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#7c3aed" font-weight="400" text-anchor="middle">core_noc + debug_noc　32 bit/T　SCP 的控制通路，core 内全部配置寄存器（Router 路由表 · TS task chain · DSA 配置 · Core status）都挂在这条总线上</text>
<text x="600" y="752" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#7c3aed" font-weight="400" text-anchor="middle">core_noc + debug_noc</text>
<rect x="140" y="150" width="66" height="490" rx="6" fill="#eceef1" stroke="#6b7280" stroke-width="1.3"/>
<text x="173" y="172" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#6b7280" font-weight="700" text-anchor="middle">Core</text>
<text x="173" y="186" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#6b7280" font-weight="700" text-anchor="middle">Monitor</text>
<text transform="translate(177 420) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">IPI（异常中断）· 各模块状态影子寄存器 · clk / rst 控制</text>
<rect x="1000" y="150" width="60" height="490" rx="6" fill="#eceef1" stroke="#6b7280" stroke-width="1.3"/>
<text x="1030" y="172" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#6b7280" font-weight="700" text-anchor="middle">Debug</text>
<text x="1030" y="186" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#6b7280" font-weight="700" text-anchor="middle">Module</text>
<text transform="translate(1034 420) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">解析 DMI · core_ctrl（halt / resume / reset / halt_on_reset）· abstract_cmd · SBA</text>
<rect x="260" y="180" width="520" height="58" rx="6" fill="#dbeafe" stroke="#2563eb" stroke-width="1.3"/>
<text x="270" y="197" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#2563eb" font-weight="700" text-anchor="start">TS　任务调度器</text>
<text x="270" y="212" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">task_chain 64 项 · stream_table 16 项顺序 FIFO · 三条发射通路 DTE_Arb / MU_Arb / VU_Arb，年龄优先</text>
<text x="270" y="225" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">固定硬件逻辑，上电配定四种工作模式 · 调度延时 2～3 T · 七路完成事件合流（RV ack ×3 · DSA ack ×3 · Reduce Done）</text>
<rect x="810" y="180" width="140" height="58" rx="6" fill="#fdeed8" stroke="#d97706" stroke-width="1.3"/>
<text x="820" y="197" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#d97706" font-weight="700" text-anchor="start">Share Mem</text>
<text x="820" y="212" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">32 KB · 3 个 RV core 共享</text>
<text x="820" y="225" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">延时 5～10 T · 传 task 间数据</text>
<rect x="260" y="268" width="210" height="70" rx="6" fill="#fffbeb" stroke="#d97706" stroke-width="1.3"/>
<text x="270" y="285" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#16181d" font-weight="700" text-anchor="start">MU RV Core</text>
<text x="270" y="300" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">RV32IMC · 仅 M 态 · 无 MMU</text>
<text x="270" y="313" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">ITCM 4 KB · DTCM 8 KB</text>
<text x="270" y="326" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">自定义指令读写 DSA 寄存器</text>
<rect x="260" y="368" width="210" height="70" rx="6" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="270" y="385" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#0d9488" font-weight="700" text-anchor="start">MU DSA</text>
<text x="270" y="400" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">8 K MAC · 64 lane</text>
<text x="270" y="413" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">token × weights 的 GEMV</text>
<text x="270" y="426" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">MXFP8 / MXFP4</text>
<rect x="500" y="268" width="210" height="70" rx="6" fill="#fffbeb" stroke="#d97706" stroke-width="1.3"/>
<text x="510" y="285" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#16181d" font-weight="700" text-anchor="start">VU RV Core</text>
<text x="510" y="300" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">RV32IMC · 仅 M 态 · 无 MMU</text>
<text x="510" y="313" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">ITCM 4 KB · DTCM 8 KB</text>
<text x="510" y="326" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">自定义指令读写 DSA 寄存器</text>
<rect x="500" y="368" width="210" height="70" rx="6" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="510" y="385" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#0d9488" font-weight="700" text-anchor="start">VU DSA</text>
<text x="510" y="400" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">1024 bit/T</text>
<text x="510" y="413" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">VALU0/1/2 · VSFU ×2 · LU / SU</text>
<text x="510" y="426" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">VRF / MRF / SRF</text>
<rect x="740" y="268" width="210" height="70" rx="6" fill="#fffbeb" stroke="#d97706" stroke-width="1.3"/>
<text x="750" y="285" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#16181d" font-weight="700" text-anchor="start">DTE RV Core</text>
<text x="750" y="300" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">RV32IMC · 仅 M 态 · 无 MMU</text>
<text x="750" y="313" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">ITCM 4 KB · DTCM 8 KB</text>
<text x="750" y="326" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">自定义指令读写 DSA 寄存器</text>
<rect x="740" y="368" width="210" height="70" rx="6" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="750" y="385" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#0d9488" font-weight="700" text-anchor="start">DTE DSA</text>
<text x="750" y="400" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">2 ch / 4 lane</text>
<text x="750" y="413" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">Header Parser · TaskQueue ×4</text>
<text x="750" y="426" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">AGCU · Hmem 与 Fast LUT</text>
<rect x="260" y="478" width="340" height="80" rx="6" fill="#fdeed8" stroke="#d97706" stroke-width="1.3"/>
<text x="270" y="495" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#d97706" font-weight="700" text-anchor="start">Matrix Mem</text>
<text x="270" y="510" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">32 MB + 4 MB scale · 64 bank</text>
<text x="270" y="523" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">(8 + 1 KB)/T，访问延迟 50 T 以内</text>
<text x="270" y="536" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">weight · B core 存 token · R core 存 reduction 数据</text>
<rect x="630" y="478" width="250" height="80" rx="6" fill="#fdeed8" stroke="#d97706" stroke-width="1.3"/>
<text x="640" y="495" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#d97706" font-weight="700" text-anchor="start">Core Mem</text>
<text x="640" y="510" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">1 MB + 32 KB · 8 bank · 地址粒度 128 B + 4 B</text>
<text x="640" y="523" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">(1 KB + 32 B)/T，访问延迟 15 T 以内</text>
<text x="640" y="536" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">token（message + data）· MU 结果 · VU 结果</text>
<text x="640" y="549" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">按 stream_num 均分给并发用户，硬件做地址映射</text>
<rect x="260" y="592" width="660" height="26" rx="4" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="590" y="609" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#0d9488" font-weight="700" text-anchor="middle">DTE Xbar（DMA_XBAR）　到 Core Mem / Matrix Mem 各 256 B/T</text>
<rect x="260" y="660" width="690" height="70" rx="6" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.3"/>
<text x="270" y="677" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#7c3aed" font-weight="700" text-anchor="start">Router　片上交换与归约中心</text>
<text x="270" y="692" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">RouterStation ×3（left / right / mid，每方向 VC ×4）· CoreStation ×1 · Xbar 5 入 7 出 · ReduceModule · CoreMemCreditMonitor</text>
<text x="270" y="705" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">每方向 256 B/T，进 core 与出 core 通路完全并行 · Reduce 输入 3 路各 160 GB/s，算力 80 GFLOPS，上下文 16 用户 × 16 KiB</text>
<text x="270" y="718" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">三层 credit：VC 按 flit · stream 按 UserID + 方向 · reduce 按 UserID　VC Buffer 100 flit/port ≈ 25 KB ×3，flit 级仲裁</text>
<path d="M935 438.7 L935 659.3" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#keabs)" marker-end="url(#keab)"/>
<text x="944" y="470" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#2563eb" font-weight="400" text-anchor="start">256 B/T ×2</text>
<text x="944" y="482" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#2563eb" font-weight="400" text-anchor="start">进 / 出 core</text>
<path d="M900 438.7 L900 591.3" stroke="#2563eb" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#keabs)" marker-end="url(#keab)"/>
<text x="908" y="575" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#2563eb" font-weight="400" text-anchor="start">256 B/T</text>
<path d="M430 558.7 L430 591.3" stroke="#2563eb" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#keabs)" marker-end="url(#keab)"/>
<path d="M755 558.7 L755 591.3" stroke="#2563eb" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#keabs)" marker-end="url(#keab)"/>
<path d="M365 478 L365 438.7" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#keab)"/>
<text x="372" y="462" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#2563eb" font-weight="400" text-anchor="start">8 KB/T 只读</text>
<path d="M440 438.7 L440 458 L645 458 L645 477.3" stroke="#2563eb" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#keabs)" marker-end="url(#keab)"/>
<text x="470" y="470" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#2563eb" font-weight="400" text-anchor="start">512 B/T 或 1 KB/T</text>
<path d="M690 438.7 L690 477.3" stroke="#2563eb" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#keabs)" marker-end="url(#keab)"/>
<text x="698" y="462" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#2563eb" font-weight="400" text-anchor="start">512 B/T 或 1 KB/T</text>
<rect x="20" y="672" width="80" height="46" rx="6" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.2"/>
<text x="60" y="691" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#16181d" font-weight="600" text-anchor="middle">data_L_ch</text>
<text x="60" y="706" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="middle">256 B/T</text>
<path d="M100.7 695 L259.3 695" stroke="#2563eb" stroke-width="2.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#keabs)" marker-end="url(#keab)"/>
<rect x="1100" y="672" width="80" height="46" rx="6" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.2"/>
<text x="1140" y="691" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#16181d" font-weight="600" text-anchor="middle">data_R_ch</text>
<text x="1140" y="706" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="middle">256 B/T</text>
<path d="M950.7 695 L1099.3 695" stroke="#2563eb" stroke-width="2.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#keabs)" marker-end="url(#keab)"/>
<rect x="540" y="830" width="180" height="46" rx="6" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.2"/>
<text x="630" y="849" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#16181d" font-weight="600" text-anchor="middle">data_UD_ch</text>
<text x="630" y="864" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="middle">另一行对称位（mid）· 256 B/T</text>
<path d="M630 730.7 L630 829.3" stroke="#2563eb" stroke-width="2.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#keabs)" marker-end="url(#keab)"/>
<text x="640" y="774" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="start">左右两边可以是 chip 间 PCIe，也可以是相邻 core 的 Router；</text>
<text x="640" y="787" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="start">上下方向是同一个通道，来自另一行 core 的 Router</text>
<path d="M259.3 205 L246 205 L246 700 L259.3 700" stroke="#d97706" stroke-width="1.8" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#kears)" marker-end="url(#kear)"/>
<text transform="translate(240 450) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#d97706" font-weight="600" text-anchor="middle">Router ↔ TS：trigger · credit · complete · reduce_done · retire</text>
<path d="M365 238.7 L365 267.3" stroke="#d97706" stroke-width="1.8" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#kears)" marker-end="url(#kear)"/>
<path d="M365 338.7 L365 367.3" stroke="#d97706" stroke-width="1.8" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#kears)" marker-end="url(#kear)"/>
<path d="M480 368 L480 238.7" stroke="#d97706" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kear)"/>
<path d="M605 238.7 L605 267.3" stroke="#d97706" stroke-width="1.8" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#kears)" marker-end="url(#kear)"/>
<path d="M605 338.7 L605 367.3" stroke="#d97706" stroke-width="1.8" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#kears)" marker-end="url(#kear)"/>
<path d="M720 368 L720 238.7" stroke="#d97706" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kear)"/>
<path d="M845 238.7 L845 267.3" stroke="#d97706" stroke-width="1.8" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#kears)" marker-end="url(#kear)"/>
<path d="M845 338.7 L845 367.3" stroke="#d97706" stroke-width="1.8" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#kears)" marker-end="url(#kear)"/>
<path d="M962 368 L962 238" stroke="#d97706" stroke-width="1.2" fill="none" marker-end="url(#kear)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="372" y="258" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#d97706" font-weight="400" text-anchor="start">task 下发 / RV ack</text>
<text x="372" y="358" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#d97706" font-weight="400" text-anchor="start">配置 · dsa_iss</text>
<text transform="translate(475 303) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#d97706" text-anchor="middle">DSA ack → TS</text>
<path d="M630 92 L630 149.3" stroke="#6b7280" stroke-width="1.4" fill="none" stroke-dasharray="5 3" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kea)"/>
<text x="640" y="118" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#6b7280" font-weight="400" text-anchor="start">scp_ctrl_ch　32 bit/T</text>
<path d="M630 154 L630 179.3" stroke="#6b7280" stroke-width="1.2" fill="none" stroke-dasharray="4 3" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kea)"/>
<path d="M880 154 L880 179.3" stroke="#6b7280" stroke-width="1.2" fill="none" stroke-dasharray="4 3" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kea)"/>
<path d="M600 756 L600 730.7" stroke="#6b7280" stroke-width="1.2" fill="none" stroke-dasharray="4 3" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kea)"/>
<path d="M1030 92 L1030 149.3" stroke="#6b7280" stroke-width="1.4" fill="none" stroke-dasharray="5 3" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kea)"/>
<text x="1040" y="118" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#6b7280" font-weight="400" text-anchor="start">dmi_ch</text>
<text x="1040" y="131" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#6b7280" font-weight="400" text-anchor="start">32 bit/T</text>
<path d="M975.7 300 L999.3 300" stroke="#6b7280" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#keas)" marker-end="url(#kea)"/>
<path d="M206.7 300 L224.3 300" stroke="#6b7280" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#keas)" marker-end="url(#kea)"/>
<path d="M173 150 L173 74 L559.3 74" stroke="#6b7280" stroke-width="1.4" fill="none" stroke-dasharray="5 3" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kea)"/>
<text x="330" y="68" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#6b7280" font-weight="400" text-anchor="middle">async_int_ch　core 的中断异常信息返回 SCP</text>
<text x="30" y="888" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="start">连线：</text>
<path d="M70 884 L110 884" stroke="#2563eb" stroke-width="2.2" fill="none" marker-end="url(#keab)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="118" y="888" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="start">数据通路</text>
<path d="M190 884 L230 884" stroke="#d97706" stroke-width="1.8" fill="none" marker-end="url(#kear)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="238" y="888" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="start">调度与完成</text>
<path d="M320 884 L360 884" stroke="#6b7280" stroke-width="1.4" fill="none" marker-end="url(#kea)" stroke-dasharray="5 3" stroke-linejoin="round" stroke-linecap="round"/>
<text x="368" y="888" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="start">配置 / 调试 / 状态</text>
<text x="520" y="888" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="start">VU DSA 不直接读 Matrix Mem，两笔数据由 DTE 先搬到 Core Mem；DTE Xbar 是 DTE 搬数据用的 xbar，不接 MU / VU</text>
</svg>
```

| 模块 | 功能 |
| - | - |
| `core_noc` | SCP 控制通路访问 Bach core 全局的路由模块 |
| `TS` | Bach Core 的控制单元，负责用户以及用户间在 core 内多任务（DTE/MU/VU）的调度 |
| `Router` | 数据通路的中转站，接收不同方向数据并路由到不同方向输出 |
| `MU/VU/DTE RV Core` | 接收 TS 调度，给对应 DSA 下发任务。ITCM 存 firmware 与 kernel，DTCM 存初始化数据、BSS 段 |
| `MU DSA` | token 与 weights 的 GEMV 运算 |
| `VU DSA` | SiLU & dot、tp-reduce、ep-reduce、concat 等向量运算 |
| `DTE DSA` | DMA 搬移任务 |
| `Matrix Mem` | ① weight 存放 ② 充当 Broadcast core 缓存 ③ 充当 Reduction core 缓存 |
| `Core Mem` | ① token 存放位置 ② MU 计算结果 ③ VU 计算结果 |
| `Share Mem` | 3 个 RV core 的共享 mem，存 task 间共享数据 |
| `DTE Xbar` | DTE 搬移数据的 xbar |
| `Core Monitor` | core 内部状态的影子寄存器：IPI 模块（异常中断信息）、各模块状态、clk/rst 控制（单独模块） |
| `Debug Module` | 解析 DMI 操作，实现对 core 内组件的 debug：core_ctrl（3 个 RV core / MU / VU / DTE 的 halt/resume/reset/halt_on_reset）、abstract_cmd、SBA 访问 |

### 对外通道

| channel | 源 → 目的 | 内容 | 带宽 |
| - | - | - | - |
| `scp_ctrl_ch` | SCP → ctrl_noc | SCP 访问 core 内资源的控制总线 | 32 bit/T（待定） |
| `async_int_ch` | core_status → SCP | core 返回给 SCP 的中断信息 | — |
| `data_L_ch` | PCIe / router → router | 数据通道：weight、kernel、token、中间结果 | 256 B/T |
| `data_UD_ch` | router → router | 同上 | 256 B/T |
| `data_R_ch` | PCIe / router → router | 同上 | 256 B/T |
| `dmi_ch` | DTM → debug_module | DM 与 DTM 间传输数据和命令 | APB? 32 bit/T（待定） |

Bach core 对外有三个方向的数据通道：左右两边可以是 chip 间 PCIe 传输，也可以是 bach core 之间经 router 的传输；上下方向为同一个通道，来自 bach core 的 router。

### core 内部通路带宽

| 通路 | 带宽 |
| - | - |
| core_noc | 32 bit/T |
| router / PCIe ↔ router | 256 B/T |
| MU DSA ← Matrix Mem | 8 KB/T |
| MU DSA ↔ Core Mem | 512 B/T 或 1 KB/T |
| VU DSA ↔ Core Mem | 512 B/T 或 1 KB/T |
| router ↔ DTE DSA、DTE Xbar ↔ Core/Matrix Mem | 原文空缺，按 Cmem MAS 与 DTE 文档为 256B ×2（Cmem）、256B ×2（router） |

> **VU 不能直接读 Matrix Mem**；需要支持 Matrix Mem → Core Mem 的搬移。

### Core Mem 的硬件多用户管理

这是 Bach 支持多用户并发的基础机制：**无页表、硬件地址映射 + 软硬件分层管理**。

映射必须由硬件做，理由是多用户复用同一套 kernel 代码：

* 软件只能用统一固定的虚拟偏移地址，没法为每个用户单独改地址、单独编译
* 纯软件管理会让相同虚拟地址落到同一块物理内存，多用户互相覆盖
* “**相同虚拟地址 ⇒ 不同物理地址**”这层动态映射只能交给硬件

1. **软件配置并发规格**：按 Core Mem 总容量与单用户所需空间，配置 TS 的最大并发用户数 `stream_num`（1～16）
2. **硬件全局分片**：按 `stream_num` 均等切分 Core Mem，单用户独占空间 = 总容量 / stream_num
3. **用户接入绑定**：TS 收到新用户任务后硬件自动分配唯一 `stream_id`，绑定一份独立物理分片
4. **软件分片内偏移使用**：软件用统一偏移地址读写，硬件自动路由到当前用户的物理分片
5. **硬件自动回收**：用户任务结束后硬件自动释放对应 stream_id 的分片

Matrix Mem 不需要这套机制，每个用户看到的是相同的权重。

### 内存结构与容量

| 存储 | 容量 | 带宽 | 内容 |
| - | - | - | - |
| DTE core ITCM | <mde-comment id="jzpft2">4 KB</mde-comment> | 8B/T，延迟 1T | ① 初始化代码（用于后续 Kernel/Weight 搬运）② kernel ③ Firmware |
| MU core ITCM | 4 KB | 同上 | kernel、Firmware |
| VU core ITCM | 4 KB | 同上 | kernel、Firmware |
| DTE / MU / VU core DTCM | 8 KB | 32bit × 4 bank | 初始化 BSS 数据段、寄存器溢出与堆栈 |
| Matrix mem | 32 MB（+4MB scale） | 8 KB/clk（+1KB scale） | weight |
| Core mem | 512 KB / 1 MB（+32KB scale） | 512 B/clk 或 1 KB/clk | ① 业务流 token：message + data ② MU 计算结果 ③ VU 计算结果 |
| Share mem | 32 KB | — | 共享的 message 信息、标量数据 |
| IO reg | — | — | Router 路由表、TS task chain、DTE/MU/VU DSA 配置寄存器、Core status |

#### <mde-comment id="agp9rj">Share Mem 的必要性</mde-comment>

* Core Mem 容量大、物理距离远，访问延时 15～25 拍，顺序执行的 RV core 掩盖不了
* 因此单独做一块容量小、物理距离近、延时 5～10 拍的 SRAM，作为三个 RV core 的共享标量存储
* 用途是加速 DTE / MU / VU 的 task 之间传数据。**不需要初始化，只存 task 间的共享数据**

Core mem → Share mem 的 message 交互选定的方案是“**初始跟随数据搬移到 core mem，再由第一个 RV core 拿到 share mem**”，另一个候选是初始化时直接进 share mem。

### 异常与中断

特权级设计极简：**仅支持 M 态**（MAS_TOP 里写“仅支持 U 态”，与 RV Core MAS 的“支持 M 态、不支持 S/U/H”矛盾，见《文档缺口与 TBD》），**不实现 MMU**，中断异常上报给 SCP 处理。

| 模块 | 异常 | 影响 |
| - | - | - |
| RV core | 非法指令、地址错误 | 通过中断上报 SCP，<mde-comment id="4h38ow">RV core 进 firmware 保存现场</mde-comment>，等待 SCP 处理；期间不接受 TS 调度、不下发新的 DSA 任务 |
| MU / VU / DTE DSA | 原文空缺 | DSA 各自文档有定义（见《执行单元与存储》） |
| Core / Matrix / Share mem | 原文空缺 | ECC 相关见《执行单元与存储》的“存储子系统” |

### 内存一致性与同步

Bach 用**任务隔离**代替显式一致性维护，建模时这一块可以大幅简化：

1. 不同 token 用户之间有独立地址空间，不存在数据共享
2. MU、VU、DTE 的 RV core **在任意时刻不会执行同一个用户的 task**，不会访问同一地址空间，不需要维护一致性
3. 同一用户在任务链不同 task 之间共享数据，默认靠 TS 把不同步骤的任务隔开来解决：
   * 同一用户的 task 按任务链顺序执行，前一个完成后才下发后一个
   * task 间通过 share mem 传数据 / context，两个 task 的数据相关就靠这条全序隔开：前一个 task 的 RV core 写完 share mem，再通知 TS 完成。RV Core MAS 这里写的是“fence + task 完成通知 TS”，但 `fence` 指令实现为 nop、`task_done` 的 FC 标志不建，所以真正起隔离作用的只有任务链的先后

生产者与消费者按下面的顺序配对（每一步是一个 Release / Acquire 对）：

| # | 生产者写什么 | 配对的事件 |
| - | - | - |
| 1 | Router 写内部 Buffer | Release + Data Ready |
| 2 | RV 写 DMA Command | Release + Trigger |
| 3 | DMA 写目标 Memory | Release + DMA Task Done |
| 4 | MU / VU 写 Task 输出 | Release + Task Done |
| 5 | DSA 写输出 | Release + Chain Done |

配图：[Bach_core_MAS_TOP 3 张画板](<Bach/04_四、MAS（Micro Architecture SPEC）/03_Bach_core_MAS_TOP（pending）>)（core 内部连接图、地址空间分配、DTE RV core 调度 DTE）+ 2 张内嵌绘图（`d01` core 外部连接图、`d02` 内存管理流程）

内嵌表格：[Bach_core_MAS_TOP 7 子表](_sheets/_JUGvs3)（Task 类型与对应资源 / 典型操作 / 切分边界、模型各阶段的 RV 配置内容与 DSA 计算、三个平面的分工、约束定义、三张地址结构说明）

来源：`04_四、MAS/03_Bach_core_MAS_TOP（pending）.md`、`07_RV Core.md`、`06_第四阶段/04_core内调度机制.md`

***

## TS 任务调度器

TS 决定多用户如何在 DTE / MU / VU 三条执行链上流水。它是**按任务链走的硬化调度器**，调度延时 2～3 cycle。

**TS 是一块固定的硬件逻辑，不是可编程的调度器**：

* 上电时由 SCP 经 `ctrl_noc` 把 64 项的 `task_chain` 与几个全局寄存器配好
* 写 `TS_INIT_FINISH` 之后就按固定逻辑跑，运行时不接受软件干预
* `CORE_TYPE` 与 `WEIGHTS_MODE` 两个配置项选定四种工作模式：weights 加载、普通计算 core、B core、R core
* **数据进来由 Router 经 `router2ts_trigger_ch` 直接通知它**，这是它被激活的入口

运行时用 16 项的 `stream_table` 记录每个在途用户走到哪一步，做四件事：

1. 新用户到了建表
2. 判当前 task 的前置条件齐了没有
3. 同一个执行单元有多个候选时选最老的
4. 收到完成事件推进度，并在链尾退休

展开在[《TS 任务调度器》](03-ts-任务调度器.md)：两张表的字段、四个动作的规则、三处复杂性（异步 datain、下游反压、B / R core 双链），以及配置检查、异常和时延口径。

***

## RV Core

三个 RV Core（DTE core / MU core / VU core）是 TS 与 DSA 之间的桥梁：接收 TS 下发的 task，执行 task 对应的 kernel 程序，配置 DSA 执行任务。三者**硬件相同，接口相同**，只是 task 信息和 DSA 配置指令内容有别。

### 指令集

| 扩展 | 支持 | 说明 |
| - | - | - |
| I | 是 | 基本指令集 |
| M | 是 | 整型乘除法 |
| A | 考虑支持 | 原子指令 |
| F | 否 | 单精度浮点 |
| D | 否 | 双精度浮点 |
| C | 是 | 压缩指令集 |

位宽：**RV32 就足够**（文档结论）。特权：**只支持 M 态**，实现 M 态 CSR，不支持 S/U/H。`fence` 指令实现为 nop。

### <mde-comment id="nou6r0">自定义指令</mde-comment>（custom-0 编码空间）

下面八条自定义指令建模时必须实现，编码取自 ISA 描述表：

| 助记符 | funct3 | opcode | 作用 |
| - | - | - | - |
| `dsar` | 000 | custom-0 | 读 DSA 寄存器，地址来自 rs1 |
| `dsari` | 000 | custom-0 | 读 DSA 寄存器，地址为立即数 |
| `dsaw.s` | 001 | custom-0 | 写 1 个 DSA 寄存器 |
| `dsaw.d` | 001 | custom-0 | 写 2 个 DSA 寄存器（rd2/rs2 + rd1/rs1） |
| `dsawi.s` / `dsawi.d` | 001 | custom-0 | 同上，寄存器地址用立即数编码 |
| `task_done` | 010 | custom-0 | 带 `TS` 标志位 |
| `flag_check` | 010 | custom-0 | 映射表快速查找，rs1/rs2 给起止地址，rd1 返回偏移 |
| `loop` | 110 | custom-0 | 自定义循环分支，rs1=最大循环次数，rs2=当前循环次数，rs2 ≥ rs1 时退出循环，imm 为分支偏移；它替代的是 `blt`，偏移量要按循环体的指令长度算 |

**写指令按单寄存器写建模**：《软件计算流程详细评估》的指令表已经只剩 `dsaw` / `dsawi` 两条，一次写 1 个 DSA 寄存器，与 RV Core MAS 的“每条最多配置 1 个”一致；上表的 `.s` / `.d` 两档编码取自 ISA 描述表，那一侧尚未同步。**立即数是 16 bit 的字节地址，覆盖 0～64K**，超出这个范围的寄存器只能用 `dsar` / `dsaw` 走寄存器寻址。

#### DSA 任务配置指令的语义

* 每条最多配置 1 个 DSA 寄存器
* 任务的启动靠写 DSA 的 **trigger 寄存器**；**last 标志**（该任务包为 task 的最后一个，DSA 执行完后通知 TS task 完成）包含在 trigger 寄存器里
* 寄存器分**静态配置**（基本不随用户变化，初始化阶段配好，业务流阶段快速调用）与**动态配置**（随用户变化，跟随任务下发，含静态配置选择）
* DSA 寄存器读指令**不支持同步读返回**，软件要查询状态需轮询

#### task_done 指令

* 通知 `pc_gen`：当前 task 完成，若 task_queue 有待执行 task 则跳转到队头 task 起始 PC，否则阻塞取指等待
* `TS` 标志有效 → 通知 TS 当前 task 完成
* firmware 程序结束时需执行一条**不通知 TS** 的 task_done，等待业务流 task

#### flag_check（映射表快速查找）

该指令只见于 ISA 描述表，RV Core MAS 不再列出。

* 输入：起始地址与结束地址
* 行为：share mem 从起始地址开始查找第一个 1，把位置偏移量写回 rd
* 查到结束地址仍没找到则返回全 1
* 用处：“自发创建任务链”一节里 R core / B core 轮询软件映射表靠的就是这条指令

### 流水线微架构

**双发射顺序流水，6～9 级**。各级：

| 模块 | 职责 |
| - | - |
| `pc_gen` | 复位后按 io_reg 的 `boot_pc` 启动；接收 TS 下发的 task 按起始 PC 执行；每拍按分支预测 / 异常 / 顺序自增产生取指 PC。优先级：异常入口 > 分支预测错误纠正 > 分支预测跳转 > PC+8B |
| `loop_bp` | 循环分支预测器，与自定义 loop 指令配合实现**循环退出 100% 正确预测** |
| `ITCM` | 取指 PC 访问 SRAM 读 8B 指令，ECC 校验（1bit 纠正，2bit 报异常） |
| `decode` | 解码 ITCM 读出的指令，判断保留指令，汇总后段流水异常并产生清空与异常跳转。解析压缩指令：8B 取指数据最多译出 4 条压缩指令、最少 2 条非压缩指令；压缩与非压缩混合时可能剩半条非压缩指令，暂存后与下一拍取到的指令拼接译码 |
| `dispatch` | 8 项指令队列：每拍最多接收 decode 的 4 条指令，队列剩余 ≥4 项才接收，否则阻塞前端；每拍按序读队头 2 条派遣到执行单元。维护通用寄存器状态表 |
| `gpr` | 32×32bit，**4 读端口**（双发射每条最多 2 源）、**5 写端口**（ALU×2、MAC/DIV/DSA 共用、LSU×2） |
| `SEU` | 标量执行：ALU0（算逻+分支）、ALU1（算逻+CSR）各 1 拍；MDU 乘法 3 拍流水、除法多周期阻塞 |
| `LSU` | 双通道 3 级流水，都能执行 load/store，按地址范围分配访问通道 |
| `dsa_iss` | DSA 调用指令下发通道，每拍最多一条配置 / trigger 指令 |
| `CSR` | M 态 CSR + 自定义 CSR |

`d03.png` 那张内部流水图给出了几条正文没写的连线。图里红色虚线是流水级边界，把流水切成 7 段，
与“6～9 级”的说法吻合：

* **`boot_pc`** **来自** **`ioreg`**：pc_gen 的一路输入直接连 ioreg。正文把“boot_pc 与异常入口是否为软件可配的
  I/O 寄存器”列为待定，图上已经按可配画了
* **CSR 由 CTRL NOC 直接配置**，不经过流水线；CSR 与 dsa_iss 之间有双向连线（stream_id / user_id / task_id 随 DSA 指令下发）
* **DTCM 画在 LSU 内部**，与 LSU 共一个框；CTRL NOC 有单独一路直连 DTCM 做初始化
* **LSU 有三个外部出口**：实线连 Shared DM，虚线连 Core Mem 和 Router I/O reg。虚线表示只有 DTE core 才接这两路，与“core 内部通路带宽”一节的地址空间视野一致
* **task_queue 在 pc_gen 之前**，TS 的 task 先进队列再驱动取指

#### loop_bp 细节

最多 **4 项**，每项记五个字段：分支指令 PC、分支目的 PC、最大循环次数、当前循环次数、正常循环方向。

* **查表**：每周期用取指 PC 并行比较 4 个表项判断命中；命中则按“当前次数 + 1 是否等于最大次数”
  判断是否循环退出，据此产生预测。
* **新分配表项**：要**查询 SEXE 与 loop_bp 之间的流水级里有没有相同的 loop 分支指令**，
  有则当前循环次数要加上中间流水的数量，防止次数丢失。
* **嵌套深度**由表项数决定，超出的部分不参与预测。

#### LSU 与访存分流

| 目标 | 组织 | 延迟 | 并发 |
| - | - | - | - |
| DTCM | 4 bank 单端口 SRAM，8KB，32bit×4bank | 3 拍 | 可同时接收 2 个不冲突 bank 的请求；同 bank 冲突则阻塞第二条 |
| Share mem | `sm_lsq` 16 项 | 5～10 拍 | 顺序执行，每拍仅发一个读 / 写请求 |
| Core mem | `cm_lsq` 16 项 | 15～25 拍 | 顺序执行，每拍仅发一个请求 |
| Router I/O reg | 复用 `cm_lsq` | — | 仅 DTE core 需要 |

访存带宽 32-bit。写回优先级：DTCM 读出数据与 share_mem / core_mem 数据同时需写回时，**优先写回 share_mem / core_mem**，阻塞 DTCM。

DTE core 访问 Core Mem 的接口与其余通路不同：

* 一次读请求**固定读回 1056 bit**，不支持 burst，按 32 bit / 拍返回
* 地址 18 bit，4B 粒度
* 写请求带 4 bit 字节使能

### task 下发与完成

#### task_queue

TS 与 RV core 之间有物理路径延时，“前一个 task 完成再通知 TS 下发下一个”会产生很长延迟。
<mde-comment id="p2nntp">RV core 因此设</mde-comment> **<mde-comment id="p2nntp">task_queue 提前接收 TS 下发的</mde-comment>** **task**，前一个 task 完成后立刻执行队头缓存的那个，
做到用户之间 task 的**无 bubble 调度**。

握手规则：RV core 按 task_queue 是否有空槽产生 `task_ack`；未被接收时，TS 不能释放该 task 跳到下一个。

#### 下发与完成信息

| 字段 | 下发（TS→RV） | 完成（RV→TS） | 用途 |
| - | - | - | - |
| `task_pc` | 有 | — | 起始取指 PC |
| `stream_id` | 有 | 有 | 4 bit，用于计算该用户的 Core Mem 与 share_mem 区域基址；硬件写入自定义 CSR，只读 |
| `user_id` | 有 | 有 | 用户号，软件读它算 R-core 的用户映射表和 Matrix Mem 地址，Router 与 credit 记账认的也是它；**可读写**，普通计算 core 上 TS 下发 task 时硬件写入，B core 与 R core 上由软件在 flag_check 认出用户后写入 |
| `task_id` | — | 有 | 6 bit，只读；**异步 datain 任务由软件识别包头后写入**，用于告诉 TS 是任务链中哪一步完成 |

### dsa_iss 的下发规则

* DSA 任务配置下发指令<mde-comment id="uk6dnm">根据下发通道是否反压阻塞判断是否下发成功</mde-comment>
* DSA 读寄存器指令**不会被阻塞**；`dsa_rq`（8 项）按顺序记录已下发的读指令信息，返回数据后按记录的目的寄存器编号写回 gpr

### 异常

| 异常 | 优先级 | 触发点 | 清空范围 |
| - | - | - | - |
| Load / Store ECC Error（DTCM） | 1（最高） | 访问 DTCM 读出后，**异步非精确** | 只清空 decode 及其前序流水 |
| Load / Store Address Misaligned | 2 | LSU 第一级流水 | 清空 dispatch 及之前所有流水，若同拍两条指令的第一条则还要清后一条 |
| Load / Store Access Fault | 3 | 同上 | 同上 |
| Fetch Address Misaligned | 4 | 流水到 decode 阶段触发 | 清空 decode 前序取指流水，decode 后续流水不受影响 |
| Fetch Access Fault | 5 | 同上 | 同上 |
| Fetch ECC Error（ITCM） | 6 | 同上，可**异步非精确** | 同上 |
| Illegal Instruction | 7 | 同上 | 同上 |
| Environment Call（ecall） | 8 | — | — |
| Breakpoint | 9 | ebreak 指令 / 指令断点 / 访存断点 | 按断点类型对应上面几类 |

LSU 的两条指令同拍都检测出异常时，只把前序那条的异常上报 decode。

Fetch Access Fault 的判定：取指地址超出 ITCM 区间，或取指地址最低 1 bit 不为 0。

异常编码（`mcause`）：标准异常沿用 RISC-V 的 0～15；自定义区 24 = Fetch ECC Error、25 = Load ECC Error、26 = Store/AMO ECC Error，`mtval` 记录出错地址。

### 性能要求

* **单个用户各 DSA 对应的软件调度程序在 RV core 上执行时间不超过 200 cycle**
* 通过 task_queue 提前缓存 task，实现用户与用户之间 task 的无 bubble 调度

配图：[RV Core 15 张内嵌绘图](<Bach/04_四、MAS（Micro Architecture SPEC）/07_RV Core>)：`d01` core 内整体框图、`d02` 外部连接、`d03` **内部流水图**、`d04~d08` pc_gen / ITCM / loop_bp / decode / dispatch 各级、`d09~d11` SEU / LSU / dsa_iss、`d12~d15` 四类异常的流水清空范围

来源：`04_四、MAS/07_RV Core.md`、`_sheets/_XT24ss/1qdnTs.csv`（异常类型表）、`01_ISA描述.xlsx`（RV Core sheet）

***

<!--mde-comments
[
  {
    "id": "0z2frq",
    "body": "这个相当于中断，要考虑死锁风险\n增加软件管理复杂度",
    "createdAt": "2026-08-24T05:44:57.157Z",
    "updatedAt": "2026-08-24T05:44:57.157Z",
    "author": "Colin"
  },
  {
    "id": "4h38ow",
    "body": "core 进 中断处理的firmware，也应该需要是中断跳转的吧？ 也就意味着，软件要提前配置好中断环境(中断寄存器）",
    "createdAt": "2026-08-24T05:42:07.828Z",
    "updatedAt": "2026-08-24T05:42:19.715Z",
    "author": "Colin"
  },
  {
    "id": "agp9rj",
    "body": "怎么保证原子性？在ShareMem支持原子指令？ 需要软件怎么用？",
    "createdAt": "2026-08-24T05:37:49.486Z",
    "updatedAt": "2026-08-24T05:38:36.901Z",
    "author": "Colin"
  },
  {
    "id": "jzpft2",
    "body": "ITCM 4KB 这个是不是有点少？\n需要软件评估一下",
    "createdAt": "2026-08-24T05:35:22.399Z",
    "updatedAt": "2026-08-24T05:35:34.728Z",
    "author": "Colin"
  },
  {
    "id": "nou6r0",
    "body": "这些指令的原子性是怎么保证的？硬件支持？",
    "createdAt": "2026-08-24T06:33:08.888Z",
    "updatedAt": "2026-08-24T06:33:08.888Z",
    "author": "Colin"
  },
  {
    "id": "p2nntp",
    "body": "相当于多一级inflight，这个会不会和前面的 中断的DTE传输造成冲突？",
    "createdAt": "2026-08-24T06:58:15.736Z",
    "updatedAt": "2026-08-24T06:59:02.530Z",
    "author": "Colin"
  },
  {
    "id": "uk6dnm",
    "body": "这里credit是在rv 里面？\n阻塞 dsa指令发射，会直接把core 死锁吧？",
    "createdAt": "2026-08-24T07:08:26.042Z",
    "updatedAt": "2026-08-24T07:09:28.272Z",
    "author": "Colin"
  },
  {
    "id": "xw28ej",
    "body": "这个感觉，就需要TS来感知，各个任务之间的数据依赖避免一个数据没用搬出去，就被其他的任务复用了这片存储器空间，造成踩踏",
    "createdAt": "2026-08-24T05:48:25.860Z",
    "updatedAt": "2026-08-24T05:48:25.860Z",
    "author": "Colin"
  }
]
-->
