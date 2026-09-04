# Router 片上交换与归约

**模式**：design（陈述当前设计，动机与取舍收在各节的“取舍”段落里）

给要实现或建模 Router 的人：Router 解决什么问题、靠什么解决、每条机制归在哪一处。

* Router 在 core 内的位置与对外通道：《Core 内硬件》
* 逐级拍数、接口位宽、机制覆盖表：[Router 建模单元](07-units/chip/core/router.md)

内容来自两份源文档，在若干点上口径不同，已裁定的取值见文末“两份源文档的口径”：

* `04_四、MAS/04_Router.md`
* `Bach项目文档/02_Architecture/03_HAS/02_DATA_NOC_DE_HAS.md`

《Bach 硬件设计建模参考》Router 专题，全套目录见 [README](README.md)。

***

## Router 解决的问题

* 一个 chip 上是两行 Bach core：中间列 chip 4 列共 8 个，第一列与最后一列 chip 5 列共 10 个，都是 8 个计算 core
* core 之间要互相传业务数据，还要与相邻 chip 交换数据
* Router 是这张片上网络的节点，每个 core 一个，物理上位于 chip 中部

传输模式有三种，都不是简单的点到点：

* **广播**：一个 core 把 token 发给一批 core，每个途经 core 既要收一份进核，又要继续往下传
* **点对点**：一个 core 发给另一个 core，中间的 core 只透传
* **归约**：多个 core 的数据沿传输方向逐级汇合累加，最后落到一个 core

难点不在转发本身，在**下游随时可能收不下**。收不下分两个层次：

* **下游 Router 的 VC buffer 满了**：网络内部的拥塞
* **下游 core 的 Core Mem 没有空间接这个用户**：业务侧的资源不足

两者的时间尺度差着数量级，Router 的应对：

* 分成两层 credit 分别管
* 给业务侧的不足留第三条出路：把包暂存进本地 Core Mem，等资源恢复

因此 Router 同时承担三件事：包的路由转发、两级流控、Reduce 计算。

### 拓扑与端口

每个 Router 有三个 R2R 方向端口：

* `left` 与 `right`：连同行相邻的 Router
* `mid`：连另一行对称位置的 Router（两排南北对称，mid-to-mid 直连）
* 边沿 Router 通过配置禁用不存在的端口，统一规格实现
* 每行左右两端的端口接 C2C Bridge，全 chip 共 4 个

加上接本 core 的 `local` 端口和 ReduceModule，Crossbar 的输入共五路：

* **五个输入的目标输出方向互不冲突时，Crossbar 必须支持五路输入同周期并行传输**，不得做不必要的串行化
* ReduceModule 一侧要能承接三路并发输入

> **取舍**：不用标准 AXI4 而自研 flit 级协议，因为 AXI4 的 header 开销在纯数据通路场景里是冗余的，
> Router 不需要地址路由。定制协议把路由信息压缩到最小，payload 带宽利用率最高，
> 代价是要自研协议栈和配套验证环境。

模块组成与对外通道如下，布局与通道名照 MAS 的 Block Diagram。

```svg
<svg viewBox="-150 0 1350 990" width="1350" height="990" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="Router 的模块组成与对外通道">
<title>Router 的模块组成与对外通道</title>
<rect x="-150" width="1350" height="990" fill="#ffffff"/>
<defs><marker id="kga" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="kgas" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker><marker id="kgai" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#3f4451"/></marker><marker id="kgais" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#3f4451"/></marker><marker id="kgab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="kgabs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="kgar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="kgars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="kgac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="kgacs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="kgap" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="kgaps" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker></defs>
<text x="30" y="26" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="start">Router 的模块组成与对外通道</text>
<text x="30" y="46" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="start">布局照 MAS 的 Block Diagram：5×7 CrossBar 居中，三个 RouterStation 各接一个方向（left 展开画，right / mid 同构），CoreStation 在上，ReduceModule 在右下；粗蓝线是 Packet，细橙线是 credit / release，灰虚线是配置与调试</text>
<rect x="290" y="62" width="130" height="40" rx="5" fill="#dbeafe" stroke="#2563eb" stroke-width="1.3"/>
<text x="355.0" y="86.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#2563eb" font-weight="700" text-anchor="middle">TaskScheduler</text>
<rect x="440" y="62" width="120" height="40" rx="5" fill="#fffbeb" stroke="#d97706" stroke-width="1.3"/>
<text x="500.0" y="86.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#16181d" font-weight="700" text-anchor="middle">DTE Core</text>
<rect x="590" y="62" width="220" height="40" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="700.0" y="86.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#0d9488" font-weight="700" text-anchor="middle">DTE（DataIn / DataOut）</text>
<rect x="840" y="62" width="110" height="40" rx="5" fill="#fdeed8" stroke="#d97706" stroke-width="1.3"/>
<text x="895.0" y="86.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#d97706" font-weight="700" text-anchor="middle">Core Mem</text>
<path d="M810.7 82 L839.3 82" stroke="#d97706" stroke-width="1.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#kgars)" marker-end="url(#kgar)"/>
<rect x="985" y="62" width="90" height="40" rx="5" fill="#eceef1" stroke="#6b7280" stroke-width="1.3"/>
<text x="1030.0" y="86.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" font-weight="700" text-anchor="middle">ctrl_bus</text>
<rect x="1085" y="62" width="95" height="40" rx="5" fill="#eceef1" stroke="#6b7280" stroke-width="1.3"/>
<text x="1132.5" y="86.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#6b7280" font-weight="700" text-anchor="middle">debug module</text>
<rect x="40" y="130" width="1140" height="720" rx="12" fill="#fbfcfd" stroke="#3f4451" stroke-width="1.6"/>
<text x="56" y="150" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#16181d" font-weight="700" text-anchor="start">Router</text>
<rect x="440" y="165" width="400" height="190" rx="8" fill="#eef4ff" stroke="#2563eb" stroke-width="1.3"/>
<text x="830" y="181" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#2563eb" font-weight="700" text-anchor="end">CoreStation</text>
<rect x="460" y="210" width="90" height="34" rx="5" fill="#fef3c7" stroke="#d97706" stroke-width="1.3"/>
<text x="505.0" y="231.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#16181d" font-weight="700" text-anchor="middle">Header FIFO</text>
<rect x="460" y="256" width="90" height="34" rx="5" fill="#fef3c7" stroke="#d97706" stroke-width="1.3"/>
<text x="468" y="271" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#16181d" font-weight="700" text-anchor="start">in_core_fifo</text>
<text x="468" y="285" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="start">OutputBuffer</text>
<text x="452" y="322" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">Packet Join：Header + Payload 拼成 core 可见的整包，不交织</text>
<rect x="700" y="178" width="70" height="20" rx="5" fill="#fee2e2" stroke="#dc2626" stroke-width="1.3"/>
<text x="735.0" y="192.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">allocate</text>
<rect x="688" y="206" width="94" height="11" rx="2" fill="#fef3c7" stroke="#d97706" stroke-width="0.9"/>
<rect x="688" y="220" width="94" height="11" rx="2" fill="#fef3c7" stroke="#d97706" stroke-width="0.9"/>
<rect x="688" y="234" width="94" height="11" rx="2" fill="#fef3c7" stroke="#d97706" stroke-width="0.9"/>
<rect x="688" y="248" width="94" height="11" rx="2" fill="#fef3c7" stroke="#d97706" stroke-width="0.9"/>
<text x="735" y="236" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="600" text-anchor="middle">Virtual Channel ×4</text>
<rect x="700" y="270" width="70" height="20" rx="5" fill="#fee2e2" stroke="#dc2626" stroke-width="1.3"/>
<text x="735.0" y="284.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">Arbiter</text>
<rect x="790" y="200" width="44" height="62" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="798" y="215" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#16181d" font-weight="700" text-anchor="start">Header</text>
<text x="798" y="229" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">Check</text>
<text x="798" y="241" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">Router</text>
<text x="798" y="253" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">Table</text>
<rect x="625" y="200" width="55" height="62" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="633" y="215" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">Credit</text>
<text x="633" y="229" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">Release</text>
<text x="725" y="335" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="end">出 core：按 Header 的 VC 号入 VC，VC 有空才准 DTE 发；再查表参与仲裁</text>
<text x="450" y="347" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">Packet Split：出 core 时拆 Header / Payload</text>
<path d="M735 198 L735 205.3" stroke="#16181d" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgai)"/>
<path d="M735 262 L735 269.3" stroke="#16181d" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgai)"/>
<path d="M770 188 L812 188 L812 199.3" stroke="#dc2626" stroke-width="1.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgar)"/>
<path d="M812 262 L812 280 L770.7 280" stroke="#dc2626" stroke-width="1.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgar)"/>
<path d="M735 102 L735 177.3" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgab)"/>
<text transform="translate(745 140) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" text-anchor="middle">out_core_data_ch</text>
<path d="M652 200 L652 102.7" stroke="#d97706" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgar)"/>
<text transform="translate(662 150) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" text-anchor="middle">router2DTE_release</text>
<path d="M500 210 L500 102.7" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgab)"/>
<text transform="translate(510 150) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" text-anchor="middle">Header 读取 / 弹出（AXI-Full 类）</text>
<path d="M550 273 L600 273 L600 102.7" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgab)"/>
<text transform="translate(610 165) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#2563eb" text-anchor="middle">in_core_data_ch（AXI-Stream-Like）</text>
<path d="M460 227 L430 227 L430 82 L420.7 82" stroke="#d97706" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgar)"/>
<text transform="translate(422 150) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" text-anchor="middle">notify_ch：包头就绪 → TS</text>
<rect x="70" y="165" width="200" height="78" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="78" y="180" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#7c3aed" font-weight="700" text-anchor="start">Stream Resource Map</text>
<text x="78" y="194" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">本级 stream credit 表：按 UserID 分配 / 占用 / 释放</text>
<text x="78" y="206" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">Router 是唯一有效状态，DTE 只持 cache</text>
<text x="78" y="218" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">决定哪些 VC 可以进 core、可以通知 TS</text>
<path d="M40 195 L69.3 195" stroke="#d97706" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgar)"/>
<text x="34" y="191" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="400" text-anchor="end">coremem credit release ×4（入）</text>
<path d="M70 225 L40 225" stroke="#d97706" stroke-width="1.3" fill="none" marker-end="url(#kgar)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="34" y="221" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="400" text-anchor="end">coremem credit release ×3（出）</text>
<path d="M170 165 L170 120 L300 120 L300 102.7" stroke="#d97706" stroke-width="1.3" fill="none" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgar)"/>
<text x="235" y="116" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="400" text-anchor="middle">notify_ch：资源就绪 → TS</text>
<path d="M270 204 L440 204" stroke="#7c3aed" stroke-width="1.0" fill="none" stroke-dasharray="4 3" stroke-linejoin="round" stroke-linecap="round"/>
<text x="305" y="200" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#7c3aed" font-weight="400" text-anchor="middle">准入查表</text>
<rect x="70" y="262" width="200" height="92" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="78" y="277" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#7c3aed" font-weight="700" text-anchor="start">CoreMemCreditMonitor</text>
<text x="78" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">出核前的资源监听：队列 16 项全相连</text>
<text x="78" y="303" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">TS 注册（UserID · StreamID · TaskID · PathID）</text>
<text x="78" y="315" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">→ 按 PathID 查下游方向的 VC 与 Stream 需求</text>
<text x="78" y="327" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">→ 申请到就通知 TS 调度搬运，否则挂队列监听</text>
<text x="78" y="339" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">各方向 credit 信息在 Router 内广播参与仲裁</text>
<path d="M355 102.7 L355 300 L270.7 300" stroke="#d97706" stroke-width="1.3" fill="none" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#kgars)" marker-end="url(#kgar)"/>
<text transform="translate(365 200) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" text-anchor="middle">注册事件 / 通知（资源就绪 → TS）</text>
<path d="M170 262 L170 243.7" stroke="#7c3aed" stroke-width="1.0" fill="none" stroke-dasharray="4 3" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgap)"/>
<text x="178" y="256" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#7c3aed" font-weight="400" text-anchor="start">查 Stream 表</text>
<path d="M170 354 L170 399.3" stroke="#7c3aed" stroke-width="1.0" fill="none" stroke-dasharray="4 3" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgap)"/>
<text x="178" y="380" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#7c3aed" font-weight="400" text-anchor="start">查各方向 VC credit</text>
<rect x="985" y="165" width="90" height="78" rx="5" fill="#eceef1" stroke="#6b7280" stroke-width="1.3"/>
<text x="993" y="180" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#6b7280" font-weight="700" text-anchor="start">CSR &amp;</text>
<text x="993" y="194" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">perf &amp; debug</text>
<text x="993" y="206" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">RouterTable 更新</text>
<text x="993" y="218" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">Credit 静态 Mask</text>
<rect x="1085" y="165" width="95" height="78" rx="5" fill="#eceef1" stroke="#6b7280" stroke-width="1.3"/>
<text x="1093" y="180" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#6b7280" font-weight="700" text-anchor="start">debug</text>
<text x="1093" y="194" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">dm 访问</text>
<text x="1093" y="206" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">Router 内状态</text>
<path d="M1015 102 L1015 164.3" stroke="#6b7280" stroke-width="1.2" fill="none" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kga)"/>
<path d="M1045 165 L1045 102.7" stroke="#6b7280" stroke-width="1.2" fill="none" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kga)"/>
<text transform="translate(1008 134) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#6b7280" text-anchor="middle">ctrl_bus_req_ch</text>
<text transform="translate(1054 134) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#6b7280" text-anchor="middle">ctrl_bus_resp_ch</text>
<path d="M1112 102 L1112 164.3" stroke="#6b7280" stroke-width="1.2" fill="none" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kga)"/>
<path d="M1150 165 L1150 102.7" stroke="#6b7280" stroke-width="1.2" fill="none" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kga)"/>
<text transform="translate(1105 134) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#6b7280" text-anchor="middle">dm_req_ch</text>
<text transform="translate(1159 134) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#6b7280" text-anchor="middle">dm_resp_ch</text>
<rect x="500" y="400" width="280" height="120" rx="8" fill="#dbeafe" stroke="#2563eb" stroke-width="1.5"/>
<text x="640" y="422" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#2563eb" font-weight="600" text-anchor="middle">Interconnect Matrix</text>
<text x="640" y="446" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="16" fill="#2563eb" font-weight="700" text-anchor="middle">5×7 CrossBar</text>
<text x="640" y="466" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">5 入：left / right / mid / Core / Reduce</text>
<text x="640" y="479" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">7 出：left / right / mid / Core（Header + Payload）/ Reduce Data ×3</text>
<text x="640" y="492" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">按输出独立仲裁，无冲突的输出同拍并行；多播全有或全无</text>
<text x="640" y="505" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">维护下游各方向的 credit；输出侧不设 VC Buffer</text>
<path d="M735 290 L735 399.3" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgab)"/>
<path d="M540 400 L540 290.7" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgab)"/>
<path d="M505 400 L505 290" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M505 256 L505 244.7" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgab)"/>
<text x="560" y="365" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#2563eb" font-weight="400" text-anchor="start">进 core 出口</text>
<text x="560" y="377" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#2563eb" font-weight="400" text-anchor="start">Header / Payload</text>
<text x="745" y="365" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#2563eb" font-weight="400" text-anchor="start">Core 方向输入</text>
<rect x="70" y="400" width="380" height="340" rx="8" fill="#eef4ff" stroke="#2563eb" stroke-width="1.3"/>
<text x="80" y="416" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#2563eb" font-weight="700" text-anchor="start">RouterStation[left]</text>
<text x="440" y="416" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="end">MAS 图里记作 E</text>
<rect x="150" y="428" width="62" height="30" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="158" y="443" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">Header</text>
<text x="158" y="457" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">Check</text>
<rect x="220" y="428" width="62" height="30" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="228" y="443" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">Router</text>
<text x="228" y="457" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">Table 副本</text>
<text x="216" y="476" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="middle">Header Parser：PathID · UserID · size · operation · 方向 Mask · VC</text>
<rect x="92" y="490" width="52" height="60" rx="5" fill="#fee2e2" stroke="#dc2626" stroke-width="1.3"/>
<text x="100" y="505" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="start">allo-</text>
<text x="100" y="519" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="start">cate</text>
<rect x="160" y="490" width="150" height="12" rx="2" fill="#fef3c7" stroke="#d97706" stroke-width="0.9"/>
<text x="235" y="499" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#16181d" font-weight="400" text-anchor="middle">VC0</text>
<rect x="160" y="505" width="150" height="12" rx="2" fill="#fef3c7" stroke="#d97706" stroke-width="0.9"/>
<text x="235" y="514" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#16181d" font-weight="400" text-anchor="middle">VC1</text>
<rect x="160" y="520" width="150" height="12" rx="2" fill="#fef3c7" stroke="#d97706" stroke-width="0.9"/>
<text x="235" y="529" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#16181d" font-weight="400" text-anchor="middle">VC2</text>
<rect x="160" y="535" width="150" height="12" rx="2" fill="#fef3c7" stroke="#d97706" stroke-width="0.9"/>
<text x="235" y="544" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#16181d" font-weight="400" text-anchor="middle">VC3</text>
<text x="235" y="562" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="middle">Virtual Channel ×4，各自独立 credit</text>
<rect x="326" y="490" width="52" height="60" rx="5" fill="#fee2e2" stroke="#dc2626" stroke-width="1.3"/>
<text x="334" y="505" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="start">Arbi-</text>
<text x="334" y="519" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="start">ter</text>
<path d="M144 520 L159.3 520" stroke="#16181d" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgai)"/>
<path d="M310 520 L325.3 520" stroke="#16181d" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgai)"/>
<path d="M378 520 L440 520 L440 480 L499.3 480" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgab)"/>
<text x="470" y="474" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" font-weight="400" text-anchor="middle">请求：方向 Mask · 下一跳 VC</text>
<path d="M118 490 L118 443 L149.3 443" stroke="#dc2626" stroke-width="1.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgar)"/>
<path d="M282 443 L352 443 L352 489.3" stroke="#dc2626" stroke-width="1.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgar)"/>
<rect x="300" y="575" width="100" height="34" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="308" y="590" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">Credit Release</text>
<text x="308" y="604" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">flit 离开 VC 即还</text>
<rect x="92" y="575" width="90" height="34" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="100" y="590" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">VC Credit</text>
<text x="100" y="604" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">下游各 VC 计数</text>
<rect x="92" y="630" width="100" height="34" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="100" y="645" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">Packet Shifter</text>
<text x="100" y="659" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">按总线宽度移位拼接</text>
<rect x="220" y="630" width="110" height="34" rx="5" fill="#fef3c7" stroke="#d97706" stroke-width="1.3"/>
<text x="228" y="645" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="start">output buffer</text>
<text x="275" y="655" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">R2R 允许 flit 交织</text>
<path d="M500 505 L480 505 L480 647 L330.7 647" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgab)"/>
<path d="M220 647 L192.7 647" stroke="#16181d" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgai)"/>
<text x="240" y="690" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">资源不足时按 stall_way：留在 VC 等，或转入 CoreMem 由 DTE 重发</text>
<text x="240" y="704" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">多播原子准入：所有目标方向的 VC / Stream / Reduce 资源同时到手才发</text>
<text x="240" y="718" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">Stream / Reduce release 按 CSR 静态 Mask 旁路转发（不查 RouterTable）</text>
<path d="M40 520 L91.3 520" stroke="#2563eb" stroke-width="2.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgab)"/>
<text x="34" y="516" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="600" text-anchor="end">left_data_in_ch</text>
<path d="M92 647 L40 647" stroke="#2563eb" stroke-width="2.6" fill="none" marker-end="url(#kgab)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="34" y="643" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="600" text-anchor="end">left_data_out_ch</text>
<path d="M350 609 L350 620 L291 620" stroke="#d97706" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M195 620 L184 620" stroke="#d97706" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M92 620 L60 620 L60 608 L40 608" stroke="#d97706" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgar)"/>
<text x="34" y="608" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="end">left_credit_release_ch_out</text>
<path d="M40 575 L70 575 L70 592 L91.3 592" stroke="#d97706" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgar)"/>
<text x="34" y="571" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="end">left_credit_release_ch_in</text>
<rect x="860" y="400" width="200" height="120" rx="5" fill="#eef4ff" stroke="#2563eb" stroke-width="1.3"/>
<text x="868" y="415" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#2563eb" font-weight="700" text-anchor="start">RouterStation[right]</text>
<text x="868" y="429" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">结构同 left：allocate → VC ×4 → Arbiter，</text>
<text x="868" y="441" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">Header Check + RouterTable 副本，</text>
<text x="868" y="453" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">Credit Release / VC Credit，</text>
<text x="868" y="465" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">output buffer → Packet Shifter</text>
<text x="868" y="477" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">行两端的 Router 这一侧接 C2C Bridge</text>
<text x="868" y="489" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">MAS 图里记作 W</text>
<path d="M780 440 L859.3 440" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgab)"/>
<path d="M860 470 L780.7 470" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgab)"/>
<path d="M1060 430 L1180 430" stroke="#2563eb" stroke-width="2.6" fill="none" marker-end="url(#kgab)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="1178" y="424" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="600" text-anchor="end">right_data_out_ch</text>
<path d="M1180 460 L1060.7 460" stroke="#2563eb" stroke-width="2.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgab)"/>
<text x="1178" y="454" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="600" text-anchor="end">right_data_in_ch</text>
<path d="M1060 490 L1180 490" stroke="#d97706" stroke-width="1.3" fill="none" marker-end="url(#kgar)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="1178" y="485" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="end">right_credit_release_ch_out</text>
<path d="M1180 510 L1060.7 510" stroke="#d97706" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgar)"/>
<text x="1178" y="505" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="end">right_credit_release_ch_in</text>
<rect x="500" y="600" width="220" height="90" rx="5" fill="#eef4ff" stroke="#2563eb" stroke-width="1.3"/>
<text x="508" y="615" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#2563eb" font-weight="700" text-anchor="start">RouterStation[mid]</text>
<text x="508" y="629" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">结构同 left；接另一行对称位的 Router（mid-to-mid 直连）</text>
<text x="508" y="641" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">MAS 图里记作 S/N，是一个端口</text>
<text x="508" y="653" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">边沿 Router 不存在的端口由配置禁用</text>
<path d="M600 520 L600 599.3" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgab)"/>
<path d="M660 600 L660 520.7" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgab)"/>
<path d="M560 690 L560 850" stroke="#2563eb" stroke-width="2.6" fill="none" marker-end="url(#kgab)" stroke-linejoin="round" stroke-linecap="round"/>
<text transform="translate(550 770) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" text-anchor="middle">mid_data_out_ch</text>
<path d="M620 850 L620 690.7" stroke="#2563eb" stroke-width="2.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgab)"/>
<text transform="translate(630 770) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" text-anchor="middle">mid_data_in_ch</text>
<path d="M680 690 L680 850" stroke="#d97706" stroke-width="1.3" fill="none" marker-end="url(#kgar)" stroke-linejoin="round" stroke-linecap="round"/>
<text transform="translate(690 770) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" text-anchor="middle">mid_credit_release_ch_out</text>
<path d="M740 850 L740 690" stroke="#d97706" stroke-width="1.3" fill="none" marker-end="url(#kgar)" stroke-linejoin="round" stroke-linecap="round"/>
<text transform="translate(750 770) rotate(-90)" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" text-anchor="middle">mid_credit_release_ch_in</text>
<rect x="820" y="560" width="340" height="260" rx="8" fill="#eef4ff" stroke="#2563eb" stroke-width="1.3"/>
<text x="990.0" y="578" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#2563eb" font-weight="700" text-anchor="middle">ReduceModule</text>
<rect x="835" y="590" width="150" height="48" rx="5" fill="#fef3c7" stroke="#d97706" stroke-width="1.3"/>
<text x="843" y="605" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">ReduceMemory</text>
<text x="843" y="619" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">16 KB × 16 entry（用户上下文）</text>
<text x="843" y="631" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">FP32 中间累加结果</text>
<rect x="995" y="590" width="150" height="48" rx="5" fill="#fed7aa" stroke="#d97706" stroke-width="1.3"/>
<text x="1003" y="605" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">Adder + Convert</text>
<text x="1003" y="619" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">RMW 原位累加</text>
<text x="1003" y="631" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">BF16 → FP32；输出 FP32 / BF16</text>
<rect x="835" y="650" width="150" height="48" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="843" y="665" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">CreditBypass XBar</text>
<text x="843" y="679" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">Stream / Reduce release</text>
<text x="843" y="691" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">按静态 Mask 旁路</text>
<rect x="995" y="650" width="150" height="48" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="1003" y="665" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">ResourceMap</text>
<text x="1003" y="679" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">按 UserID + 方向维护</text>
<text x="1003" y="691" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">相邻下游 Reduce Credit</text>
<text x="990.0" y="730" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">三路输入 3 × 160 GB/s，输出 160 GB/s，80 GFLOPS；输入后锁定到尾 flit</text>
<text x="990.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">资源不可用时反压输入，不允许绕过 Reduce 降级为转发</text>
<text x="990.0" y="758" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">完成后向 core 返回 UserID；收到 Retire 后待下游 credit 全部恢复才删表项</text>
<path d="M735 520 L735 600 L819.3 600" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgab)"/>
<text x="768" y="594" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="600" text-anchor="middle">Data ×3</text>
<path d="M820 632 L800 632 L800 520" stroke="#2563eb" stroke-width="1.8" fill="none" marker-end="url(#kgab)" stroke-dasharray="6 3" stroke-linejoin="round" stroke-linecap="round"/>
<text x="768" y="647" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" font-weight="400" text-anchor="middle">结果回注</text>
<text x="768" y="658" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" font-weight="400" text-anchor="middle">（第 5 路输入）</text>
<path d="M1180 780 L1160.7 780" stroke="#d97706" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgar)"/>
<text x="1178" y="774" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="end">ts_router_user_retire_ch</text>
<path d="M1180 800 L1160.7 800" stroke="#d97706" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kgar)"/>
<text x="1178" y="794" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="end">reduce credit release ×3（入）</text>
<path d="M1160 815 L1180 815" stroke="#d97706" stroke-width="1.3" fill="none" marker-end="url(#kgar)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="1178" y="828" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="end">reduce credit release ×4（出）</text>
<rect x="92" y="613" width="92" height="13" rx="6" fill="#dbeafe" stroke="#2563eb" stroke-width="1.2"/>
<text x="138.0" y="623" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#2563eb" font-weight="700" text-anchor="middle">VC credit 计数实体</text>
<rect x="950" y="504" width="92" height="13" rx="6" fill="#dbeafe" stroke="#2563eb" stroke-width="1.2"/>
<text x="996.0" y="514" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#2563eb" font-weight="700" text-anchor="middle">VC credit 计数实体</text>
<rect x="610" y="674" width="92" height="13" rx="6" fill="#dbeafe" stroke="#2563eb" stroke-width="1.2"/>
<text x="656.0" y="684" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#2563eb" font-weight="700" text-anchor="middle">VC credit 计数实体</text>
<rect x="404" y="596" width="72" height="13" rx="6" fill="#ffffff" stroke="#2563eb" stroke-width="0.9" stroke-dasharray="2 2"/>
<text x="440.0" y="606" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" font-weight="400" text-anchor="middle">只还，无计数</text>
<rect x="560" y="296" width="160" height="13" rx="6" fill="#ffffff" stroke="#2563eb" stroke-width="0.9" stroke-dasharray="2 2"/>
<text x="640.0" y="306" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" font-weight="400" text-anchor="middle">只还 VC credit，计数在 DataOut DTE</text>
<rect x="110" y="226" width="150" height="13" rx="6" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.2"/>
<text x="185.0" y="236" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#7c3aed" font-weight="700" text-anchor="middle">stream credit 计数实体（唯一有效）</text>
<rect x="195" y="575" width="96" height="48" rx="5" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.3"/>
<text x="203" y="590" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#7c3aed" font-weight="700" text-anchor="start">Stream 映射</text>
<text x="203" y="604" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">下游各方向的授权</text>
<rect x="200" y="607" width="86" height="13" rx="6" fill="#ffffff" stroke="#7c3aed" stroke-width="0.9" stroke-dasharray="2 2"/>
<text x="243.0" y="617" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#7c3aed" font-weight="400" text-anchor="middle">查询副本，不计数</text>
<rect x="1046" y="702" width="150" height="13" rx="6" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.2"/>
<text x="1121.0" y="712" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#0d9488" font-weight="700" text-anchor="middle">Reduce credit 计数实体（相邻下游）</text>
<rect x="835" y="702" width="112" height="13" rx="6" fill="#ffffff" stroke="#0d9488" stroke-width="0.9" stroke-dasharray="2 2"/>
<text x="891.0" y="712" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#0d9488" font-weight="400" text-anchor="middle">只转发 release，不计数</text>
<rect x="590" y="49" width="236" height="13" rx="6" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.2"/>
<text x="708.0" y="59" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#0d9488" font-weight="700" text-anchor="middle">Reduce credit 计数实体（本级）·stream credit cache</text>
<rect x="30" y="866" width="1140" height="110" rx="5" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.2"/>
<text x="46" y="883" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">口径：三个 R2R 方向本文档记 left / right / mid，MAS Block Diagram 记 E / W / S/N，S/N 是一个端口；通道名照 MAS 的写法 &lt;方向&gt;_data_in_ch / _data_out_ch / _credit_release_ch_in / _out。</text>
<text x="46" y="899" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">带宽：每方向 256 B 宽，相邻 Router 双向各 256 GB/s @1GHz；进 core 与出 core 各 256 GB/s，完全并行。Header 与 Payload 走两根独立总线（hflit 256-bit，pflit 2048-bit）。</text>
<text x="46" y="915" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">CoreMemCreditMonitor 在 MAS 的模块说明里列为独立模块，Block Diagram 里没有单独画，本图按模块说明补画在 Stream Resource Map 下方，细节见“出核前的资源监听”。</text>
<text x="46" y="931" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">credit 的计数实体（实心标签）：VC credit 在发送侧各 RouterStation 的计数器（每个下游方向 × VC 一个）；stream credit在 Stream Resource Map 的表（唯一有效状态，DTE 另持一份 cache）；</text>
<text x="46" y="947" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">Reduce credit 在 ReduceModule 的 ResourceMap（相邻下游）与 DTE（本级）。</text>
<text x="46" y="963" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">虚线标签的位置只做加减或转发，本地没有计数：接收侧的 Credit Release 只归还，RouterStation 的 Stream 映射只查询，CreditBypass XBar 只按静态 Mask 转发 release。</text>
</svg>
```

***

## 一个包穿过 Router 的工作过程

Router 上跑的不止一种包：进本 core 的、直通到下一个 Router 的、多播的、要归约的、被阻塞后转进 Core Mem 再重发的，还有不是数据的 credit 旁路。先按进本 core 的包走一遍主线，再按任务类型分别说走法，最后单独说三类 credit 各自怎么管。

### 主线：一个进本 core 的包

编号与图中一致，每一步只说谁在什么时候做什么，判定规则在后面各节。

```svg
<svg viewBox="0 0 1200 640" width="1200" height="640" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="一个包穿过 Router 的工作过程">
<title>一个包穿过 Router 的工作过程</title>
<rect width="1200" height="640" fill="#ffffff"/>
<defs><marker id="kha" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="khas" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker><marker id="khai" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#3f4451"/></marker><marker id="khais" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#3f4451"/></marker><marker id="khab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="khabs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="khar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="khars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="khac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="khacs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="khap" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="khaps" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker></defs>
<text x="30" y="26" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="start">一个包穿过 Router 的工作过程</text>
<text x="30" y="46" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="start">编号是正文里的步骤号：①～⑤ 是 RouterStation 到 CrossBar 的正向路，⑥～⑧ 是进 core，⑨ 是出 core，⑩ 是 Reduce；橙色是回程的 credit / release</text>
<rect x="30" y="250" width="90" height="60" rx="5" fill="#eceef1" stroke="#6b7280" stroke-width="1.3"/>
<text x="38" y="265" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#6b7280" font-weight="700" text-anchor="start">上游</text>
<text x="38" y="279" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="start">Router</text>
<text x="38" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="start">或 C2C</text>
<rect x="1080" y="250" width="90" height="60" rx="5" fill="#eceef1" stroke="#6b7280" stroke-width="1.3"/>
<text x="1088" y="265" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#6b7280" font-weight="700" text-anchor="start">下游</text>
<text x="1088" y="279" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="start">Router</text>
<text x="1088" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="start">或 C2C</text>
<rect x="140" y="90" width="920" height="500" rx="12" fill="#fbfcfd" stroke="#3f4451" stroke-width="1.5"/>
<text x="156" y="110" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#16181d" font-weight="700" text-anchor="start">Router</text>
<rect x="160" y="200" width="300" height="180" rx="8" fill="#eef4ff" stroke="#2563eb" stroke-width="1.2"/>
<text x="170" y="218" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#2563eb" font-weight="700" text-anchor="start">RouterStation[left]</text>
<rect x="175" y="232" width="120" height="40" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="183" y="247" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">Header Parser</text>
<text x="183" y="261" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">查 RouterTable 副本</text>
<rect x="175" y="285" width="120" height="11" rx="2" fill="#fef3c7" stroke="#d97706" stroke-width="0.9"/>
<rect x="175" y="299" width="120" height="11" rx="2" fill="#fef3c7" stroke="#d97706" stroke-width="0.9"/>
<rect x="175" y="313" width="120" height="11" rx="2" fill="#fef3c7" stroke="#d97706" stroke-width="0.9"/>
<rect x="175" y="327" width="120" height="11" rx="2" fill="#fef3c7" stroke="#d97706" stroke-width="0.9"/>
<text x="235" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">VC Buffer ×4</text>
<rect x="320" y="285" width="60" height="56" rx="5" fill="#fee2e2" stroke="#dc2626" stroke-width="1.3"/>
<text x="328" y="300" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="start">资源</text>
<text x="328" y="314" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">检查</text>
<text x="328" y="326" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">+ 仲裁</text>
<text x="320" y="372" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">VC credit · Stream · Reduce 准入</text>
<rect x="395" y="232" width="55" height="40" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="403" y="247" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">Credit</text>
<text x="403" y="261" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">Release</text>
<rect x="490" y="250" width="160" height="90" rx="8" fill="#dbeafe" stroke="#2563eb" stroke-width="1.4"/>
<text x="570" y="285" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="13" fill="#2563eb" font-weight="700" text-anchor="middle">5×7 CrossBar</text>
<text x="570" y="303" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">按输出独立仲裁</text>
<text x="570" y="316" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">多播全有或全无</text>
<text x="570" y="329" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">进 core / Reduce 锁定整包</text>
<rect x="830" y="250" width="200" height="62" rx="5" fill="#eef4ff" stroke="#2563eb" stroke-width="1.3"/>
<text x="838" y="265" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#2563eb" font-weight="700" text-anchor="start">RouterStation[right] 出口</text>
<text x="838" y="279" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">output buffer → Packet Shifter</text>
<text x="838" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">扣下游 VC credit，逐 flit 发</text>
<rect x="490" y="120" width="330" height="110" rx="8" fill="#eef4ff" stroke="#2563eb" stroke-width="1.2"/>
<text x="500" y="138" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#2563eb" font-weight="700" text-anchor="start">CoreStation</text>
<rect x="505" y="150" width="110" height="34" rx="5" fill="#fef3c7" stroke="#d97706" stroke-width="1.3"/>
<text x="513" y="165" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">Header FIFO</text>
<rect x="505" y="190" width="110" height="34" rx="5" fill="#fef3c7" stroke="#d97706" stroke-width="1.3"/>
<text x="513" y="205" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">in_core_fifo</text>
<rect x="660" y="150" width="145" height="74" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="668" y="165" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">出 core 输入 VC</text>
<text x="668" y="179" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">按 Header 的 VC 号入 VC</text>
<text x="668" y="191" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">VC 有空才准 DTE 发</text>
<text x="668" y="203" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">再查表参与仲裁</text>
<rect x="160" y="120" width="100" height="40" rx="5" fill="#dbeafe" stroke="#2563eb" stroke-width="1.3"/>
<text x="168" y="135" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#2563eb" font-weight="700" text-anchor="start">TS</text>
<text x="168" y="149" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">收 notify，调 DTE</text>
<rect x="280" y="120" width="90" height="40" rx="5" fill="#fffbeb" stroke="#d97706" stroke-width="1.3"/>
<text x="288" y="135" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#16181d" font-weight="700" text-anchor="start">DTE Core</text>
<text x="288" y="149" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">读包头建任务</text>
<rect x="380" y="120" width="95" height="40" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="388" y="135" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#0d9488" font-weight="700" text-anchor="start">DataIn DTE</text>
<text x="388" y="149" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">收整包 → CM</text>
<rect x="860" y="120" width="100" height="40" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="868" y="135" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#0d9488" font-weight="700" text-anchor="start">DataOut DTE</text>
<text x="868" y="149" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">查表、拿资源</text>
<rect x="160" y="420" width="170" height="60" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="168" y="435" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#7c3aed" font-weight="700" text-anchor="start">Stream Resource Map</text>
<text x="168" y="449" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">本级 Stream 表</text>
<text x="168" y="461" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">release 携 UserID 回收</text>
<rect x="720" y="400" width="300" height="100" rx="5" fill="#eef4ff" stroke="#2563eb" stroke-width="1.3"/>
<text x="728" y="415" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#2563eb" font-weight="700" text-anchor="start">ReduceModule</text>
<text x="728" y="429" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">首份输入建上下文，后续 RMW 原位累加</text>
<text x="728" y="441" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">输出前查目标 VC credit + 下游 Reduce credit</text>
<text x="728" y="453" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">完成回 UserID；Retire 后待 credit 恢复再删表项</text>
<path d="M120 280 L175 280" stroke="#2563eb" stroke-width="2.4" fill="none" marker-end="url(#khab)" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="131" y="253" width="18" height="18" rx="9" fill="#ffffff" stroke="#2563eb" stroke-width="1.4"/>
<text x="140" y="266" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#2563eb" font-weight="700" text-anchor="middle">①</text>
<text x="140" y="300" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" font-weight="400" text-anchor="middle">Header 先到</text>
<path d="M235 272 L235 284.3" stroke="#2563eb" stroke-width="1.8" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khab)"/>
<rect x="249" y="270" width="18" height="18" rx="9" fill="#ffffff" stroke="#2563eb" stroke-width="1.4"/>
<text x="258" y="283" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#2563eb" font-weight="700" text-anchor="middle">②</text>
<path d="M295 313 L319.3 313" stroke="#2563eb" stroke-width="1.8" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khab)"/>
<rect x="298" y="291" width="18" height="18" rx="9" fill="#ffffff" stroke="#2563eb" stroke-width="1.4"/>
<text x="307" y="304" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#2563eb" font-weight="700" text-anchor="middle">③</text>
<path d="M380 313 L489.3 313" stroke="#2563eb" stroke-width="2.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khab)"/>
<rect x="426" y="291" width="18" height="18" rx="9" fill="#ffffff" stroke="#2563eb" stroke-width="1.4"/>
<text x="435" y="304" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#2563eb" font-weight="700" text-anchor="middle">④</text>
<path d="M650 281 L829.3 281" stroke="#2563eb" stroke-width="2.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khab)"/>
<rect x="731" y="259" width="18" height="18" rx="9" fill="#ffffff" stroke="#2563eb" stroke-width="1.4"/>
<text x="740" y="272" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#2563eb" font-weight="700" text-anchor="middle">⑤</text>
<text x="740" y="298" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" font-weight="400" text-anchor="middle">R2R：flit 边界可交织</text>
<path d="M1030 281 L1079.3 281" stroke="#2563eb" stroke-width="2.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khab)"/>
<path d="M540 250 L540 224.7" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khab)"/>
<path d="M560 250 L560 236 L575 236 L575 224" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M575 190 L575 184.7" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khab)"/>
<rect x="601" y="231" width="18" height="18" rx="9" fill="#ffffff" stroke="#2563eb" stroke-width="1.4"/>
<text x="610" y="244" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#2563eb" font-weight="700" text-anchor="middle">⑥</text>
<text x="622" y="244" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" font-weight="400" text-anchor="start">Stream 准入后进</text>
<path d="M505 167 L325 167 L325 160.7" stroke="#d97706" stroke-width="1.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khar)"/>
<path d="M505 174 L215 174 L215 160.7" stroke="#d97706" stroke-width="1.4" fill="none" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khar)"/>
<rect x="619" y="158" width="18" height="18" rx="9" fill="#ffffff" stroke="#d97706" stroke-width="1.4"/>
<text x="628" y="171" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#d97706" font-weight="700" text-anchor="middle">⑦</text>
<text x="330" y="186" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="middle">notify_ch → TS；DTE Core 读包头、弹出</text>
<path d="M505 207 L455 207 L455 160.7" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khab)"/>
<rect x="469" y="187" width="18" height="18" rx="9" fill="#ffffff" stroke="#2563eb" stroke-width="1.4"/>
<text x="478" y="200" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#2563eb" font-weight="700" text-anchor="middle">⑧</text>
<path d="M900 160 L900 187 L805.7 187" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khab)"/>
<rect x="871" y="191" width="18" height="18" rx="9" fill="#ffffff" stroke="#2563eb" stroke-width="1.4"/>
<text x="880" y="204" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#2563eb" font-weight="700" text-anchor="middle">⑨</text>
<path d="M700 224 L700 238 L620 238 L620 249" stroke="#2563eb" stroke-width="2.2" fill="none" marker-end="url(#khab)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M620 340 L620 366" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M620 384 L620 450 L719.3 450" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khab)"/>
<text x="670" y="441" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" font-weight="400" text-anchor="middle">Data ×3</text>
<rect x="611" y="366" width="18" height="18" rx="9" fill="#ffffff" stroke="#2563eb" stroke-width="1.4"/>
<text x="620" y="379" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#2563eb" font-weight="700" text-anchor="middle">⑩</text>
<path d="M720 470 L660 470 L660 340 L651 340" stroke="#2563eb" stroke-width="1.6" fill="none" marker-end="url(#khab)" stroke-dasharray="6 3" stroke-linejoin="round" stroke-linecap="round"/>
<text x="670" y="483" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" font-weight="400" text-anchor="start">结果回注</text>
<path d="M395 252 L295 252" stroke="#d97706" stroke-width="1.2" fill="none" stroke-dasharray="3 2" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M422 232 L422 194 L100 194 L100 249.3" stroke="#d97706" stroke-width="1.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khar)"/>
<text x="75" y="330" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="middle">VC credit 回程：flit 离开</text>
<text x="75" y="342" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="middle">VC Buffer 就还上游一个</text>
<path d="M1000 250 L1000 108 L210 108 L210 119.3" stroke="#d97706" stroke-width="1.4" fill="none" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khar)"/>
<text x="600" y="104" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="middle">下游 release 回来按 UserID 恢复 Stream / Reduce credit；包发完（直发或经 CM 重发）把完成信息（UserID + PathID）回 TS</text>
<path d="M470 160 L470 187" stroke="#d97706" stroke-width="1.4" fill="none" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M470 205 L470 450 L330.7 450" stroke="#d97706" stroke-width="1.4" fill="none" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khar)"/>
<text x="340" y="441" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="start">core 用完 CM → release（UserID）→ 删本级 Stream 表项</text>
<path d="M245 420 L245 380.7" stroke="#d97706" stroke-width="1.4" fill="none" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khar)"/>
<text x="255" y="402" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="start">授权给 ③ 的准入</text>
<path d="M720 490 L245 490 L245 480.7" stroke="#d97706" stroke-width="1.2" fill="none" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khar)"/>
<text x="500" y="504" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="middle">Stream / Reduce release 按 CSR 静态 Mask 旁路，不查 RouterTable</text>
<path d="M1060 430 L1020.7 430" stroke="#d97706" stroke-width="1.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#khar)"/>
<text x="1064" y="426" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="start">Retire</text>
<rect x="30" y="534" width="1140" height="62" rx="5" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.2"/>
<text x="46" y="551" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">阻塞时的两条岔路：③ 拿不到资源，按 RouterTable 的 stall_way 留在 VC 等，或把包转进 Core Mem（Bypass 变成“进 core + 出 core”），资源就绪后由 DTE 重发，同 VC 内不许越过；</text>
<text x="46" y="567" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">⑥ 本级 Stream 表无空项时，该 VC 不能向 core 发（VC 有空项仍可收）。进 core 与出 core 两条路完全并行，互不共享仲裁状态。</text>
<text x="46" y="583" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">这是进本 core 的主线；直通、多播、Reduce、CoreMem 重发、credit 旁路各自的走法见“按任务类型分的走法”，三类 credit 谁维护、何时扣还见“三类 credit 的管理方式”。</text>
</svg>
```

**正向路：RouterStation 到 CrossBar**

1. Header 先于或与首个 Payload flit 一起到达 `left_data_in_ch`。RouterStation 的 Header Parser 从包头取出 PathID、UserID、size、operation、方向 Mask 与 VC，按 PathID 查本地 RouterTable 副本，为这个包建立路由与资源上下文（目标方向、下一跳 VC、需不需要 Stream 授权、是不是 Reduce、stall_way）。
2. Payload flit 按 Header 指定的 VC 号写入对应的 VC Buffer。四个 VC 各自独立缓存、独立计 credit，一个 VC 堵住不影响其他 VC。
3. VC 头部 flit 检查所有目标方向的资源：下游该 VC 的 credit、目标方向的 Stream 授权、要进 ReduceModule 时的 Reduce 准入。多播要所有目标方向同时到手，任一方向不足则整体等待。拿不到资源时按 stall_way 处理：留在 VC 里等，或把包转进本地 Core Mem 由 DTE 重发。
4. 满足条件的请求带着方向 Mask、当前 flit 与下一跳 VC 进 CrossBar 仲裁。CrossBar 按输出独立仲裁，只有竞争同一输出的请求互斥；多播在同一拍向全部目标复制，任一目标没握手就不推进任何分支。握手成功后统一扣各目标的 credit、更新包上下文。
5. 目标是相邻 Router 时，flit 经出口方向的 RouterStation 的 output buffer 与 Packet Shifter 按总线宽度移位拼接后从 `<方向>_data_out_ch` 发出。Router 到 Router 的通路可在 flit 边界切换包；一旦进入 Core 或 ReduceModule 就锁定到尾 flit。

**进 core**

6. 目标含本级 Core 时，先查本级 stream credit 表：UserID 已命中直接收；未命中且有空项则分配后收；无空项时该 VC 不能向 Core 发，但 VC 有空项仍可继续收上游数据。准入后不再检查 Core 方向的 VC credit，Header 写入 Header FIFO，Payload 写入 in_core_fifo（OutputBuffer），两者保持同一包顺序与边界。
7. CoreStation 收满一个包，按包头顺序经 `notify_ch` 通知 TS，请求里带 UserID、PathID 与重发标记。DTE Core 经 AXI-Full 类接口读包头生成搬运任务，读完向指定地址写 1 把包头弹出，CoreStation 映射出下一个包头。
8. DataIn DTE 按 TS 的调度经 `in_core_data_ch`（AXI-Stream-Like）收拼接好的整包写进 Core Mem。Core 入口以整包为单位，不支持包间交织。被反压时 valid、Header、Payload、首尾标志与有效字节保持不变，解除后从同一 flit 继续。

**出 core**

9. DataOut DTE 按 PathID 查自己那份 RouterTable 得到 VC 与资源需求，先向 Router 申请到下游 Stream 或 Reduce 资源（申请不到就在 PendingTaskQ 等），再在目标 VC 有空时经 `out_core_data_ch` 发出整包。CoreStation 拆出 Header 与 Payload，按 Header 的 VC 号写入 Core 方向输入 VC，之后与其他方向一样查表、参与仲裁。进 core 与出 core 两条路完全并行，互不共享仲裁状态。

**Reduce**

10. operation 是 Reduce 的包由 CrossBar 导向 ReduceModule（Data ×3）。同一 User 同一包的第一份输入分配上下文并写入 FP32 数据，后续方向的输入读出当前值、累加、写回；全部输入完成后结果进输出队列，发送前查目标 VC credit 与该方向的下游 Reduce credit，结果作为 CrossBar 的第五路输入重新参与仲裁。整包发出后向 Core 返回 UserID 作为完成信息。

### 按任务类型分的走法

| 任务 | 从哪来 | 经过的模块 | 放行前查什么 | 到哪去 / 完成信号 |
| - | - | - | - | - |
| 直通（Bypass） | 上游 Router 的 `<方向>_data_in_ch` | RouterStation → CrossBar → 出口 RouterStation 的 output buffer / Packet Shifter | 下游该 VC 的 credit；目标方向需要 Stream 时查本级的下游 Stream 映射表 | 下一个 Router；flit 离开本级 VC 即还上游 VC credit |
| 多播 | 同上，flow_dir 多位有效 | 同上，CrossBar 同拍复制到全部目标 | 全部目标方向的 VC credit 与 Stream 授权同时到手 | 各目标方向；任一方向没握手则整体不推进 |
| 进 core | 上游 Router | RouterStation → CrossBar → CoreStation 的 Header FIFO 与 in_core_fifo | 本级 stream credit 表准入（不查 Core 方向 VC credit） | Core Mem；CoreStation 经 `notify_ch` 通知 TS，DTE 搬完后 TS 收完成信息 |
| 出 core | DataOut DTE 的 `out_core_data_ch` | CoreStation 的 Core 方向输入 VC → CrossBar → 出口 RouterStation | 目标 VC 有空才准 DTE 发；下游 Stream / Reduce 资源由 DTE 先向 Router 申请到 | 下一个 Router；发完向 TS 返回 UserID + PathID |
| Reduce | 本 core 的 DataOut DTE，或上游 Router 的 Reduce 包 | CrossBar → ReduceModule（Data ×3）→ 结果回注 CrossBar | 本级 Reduce credit 够整包（DTE 查）；输出时查目标 VC credit 与下游 Reduce credit | 下游 Router 或本 core；整包发出后向 core 返回 UserID |
| 进 CoreMem 暂存与重发 | 直通或多播的包在本级拿不到资源，stall_way 选了转存 | 走一遍进 core，再由 DTE 走一遍出 core | 重发时按 PathID 重查 RouterTable，同 VC 内不许越过未重发的包 | 原目标；完成后同样向 TS 返回 UserID + PathID |
| 业务 credit 的旁路 | 下游或本 core 的 Stream / Reduce release | RouterStation 的 Credit Release，CrossBar 不参与仲裁 | 不查 RouterTable，只看 CSR 里该输入端口的静态方向 Mask | Mask 指定的一个或多个方向 |

不派角色的 core 上的 Router 只走直通：数据走完整流水线但不投递本 core，credit 也跨过它直接给两侧落地的 core，见“跳过与跨 chip”。

包落进哪块存储由数据类型决定，不由路由决定：Router 解析包头把数据类型交给 TS，TS 据此配置 DTE 的 `dst_sel`。**残差数据、EP 间的 reduction 数据、EP 间的 broadcast 数据都是 P2P 传输，落点都是 core 的 Matrix Mem**；业务流的 token 落 Core Mem。kernel 与 weight 走的是另一档，`op_type = 0` 时跳过 TS 直接唤醒 DTE，落 Matrix Mem。

#### 直通与多播

* 包经数据总线传到下一级 Router，自动检测包头，按 `PathID` 查到路由信息和资源需求
* 数据总线每个 flit 携带 VC 通道号，到达后自动找到对应 VC 存放位置
* 输入方向的 RouterStation 维护所有下游方向的 stream credit映射表，**只有所有需求方向都满足才允许发送**

**交织规则**（直接决定建模时 buffer 的组织方式）：

* Router 到 Router 的通路**允许在 flit 边界切换包**，需保存 VC、输出方向、剩余长度和包边界上下文
* 包一旦开始进入 Core 或 ReduceModule 就**锁定到尾 flit**

多播是 flow_dir 多位有效的直通，走法相同，只是 CrossBar 在 ST 阶段同拍复制、准入要全部目标方向一起满足。

#### 进 core

* 进 Core 对 Router 而言也是一个输出方向，要维护本级 Core 的 stream credit
* 已通过 Stream 检查，因此**不再检查对 Core 的 VC credit**，一定有 CM 空间

按 `PathID` 查到需进 Core 后，检查本级 stream credit 表，按三种结果分别处理：

* **已分配**：包头进 HeaderFIFO、数据进 OutputBuffer
* **未分配但有空项**：记录 UserID 占用
* **无空项**：该 VC 不能发数据到 Core，但 VC 有空项时仍可接收数据

通知 TS 这一步：

* CoreStation 收满一个包，按接收包头的顺序经 `router2ts_trigger_ch` **直接通知 TS**
* 请求里带 `user_id`、`path_id` 与这一笔要不要重发的标记
* 这条通路硬件直连，不经过 RV core，也不经过任何软件环节
* TS 据此调度 DTE 搬运

DTE 取数这一步：

* DTE core 用 **AXI-Full 类接口**读包头生成 DTE 任务
* 读完向指定地址写 1 把包头弹出，CoreStation 映射出下一个包头
* CoreStation 与 DTE 之间用 **AXI-Stream-Like** 协议传数据
* **Core 内输入不支持多包交织**，Router 必须发完一个整包再发下一个

被反压时：

* 保持 valid、当前 Header、Payload、首尾 flit 标志和有效字节信息不变，传输位置不得前移
* 反压解除后从同一 flit 继续握手，确保包不丢拍、不重拍、不跨包、不串包

#### 出 core

由 DataOut DTE 发起：

* DTE 内按 Router 一个方向的 VC 数各有一个 Buffer，某个 VC 阻塞只阻塞对应的那个 Buffer
* DTE 发数据到 Router 时与 CoreStation 有 credit 协议，保证 VC 有容量才发
* 包对下游 Stream 或 Rmem 资源有需求时，DTE 必须先申请到才能发，否则任务在 `PendingTaskQ` 等待
* DTE 中也要有一份 RouterTable，按 PathID 查到 VC 和资源需求
* 进 Core 与出 Core 的数据通路**完全并行**，互不共享数据通路仲裁状态

#### Reduce

Reduce 包从 core 出发这一段由 DTE 管 credit，进了 ReduceModule 之后由 ReduceModule 管：

* DTE 持有本级 ReduceModule 每一项的 credit，粒度是一个 flit。某个用户建 stream credit 表项时，给这个用户分配一个 entry 的 credit 数量
* DTE 按包头的 PathID 查自己那份 RouterTable，得知这是 Reduce 操作以及走哪个 VC
* DTE 搬 Reduce 包前先检查本级 Reduce credit 是否够整包，再在 VC credit 满足的前提下发到 ReduceModule
* ReduceModule 每完成一次 Reduce 并把 flit 发给下游，就释放一个 credit，经独立的释放通道把 Valid + UserID 送回 DTE
* ReduceModule 之间同样按 credit 走：每完成一个 flit 就可以向下级发一个 flit，发前要同时查下游的 Reduce credit 与 VC credit；上游 ReduceModule 维护所有下游 UserID 与 credit 数量的映射表，按 flit 申请、按 UserID 释放
* 表项的删除：收到本级 core 某个 UserID 的 Retire，且 credit 恢复到分配数量后，删掉这一项给其他用户用

累加本身怎么做、上下文怎么保护、精度怎么定，在“归约”一节。

#### 进 CoreMem 暂存与重发

**判断在 RC 阶段做，按包为单位**：

```
if need_buffer && !core_bad_mask[本 core] &&
   (stream_credit[目标方向] 不可用 || pending_reinject[vc][目标方向] > 0):
       目标端口改为 local          // 重定向，不派角色的 core 不接收溢流
       overflow_reinject = 1
       pending_reinject[vc][原方向] += 1
       // 此时暂不更新 stream_credit
```

重注入时才结账：Core 与 DTE 保证 `stream_credit` 可用后才发起，包重新走 RC 查表得到目的 port（包头里不再单独保存路由信息），Output Port 识别到 `overflow_reinject = 1` 时才占位更新 `stream_credit`，并把 `pending_reinject` 减一。

`overflow_enable` 与 `original_target_port` 曾经是包头字段，现已删除：前者移进 RouterTable 的 `need_buffer`，后者由重发时重查路由表得到。

**core credit 更新信号分三种 action**，Router 给 core 的三条输出方向各一根：

| action | 场景 |
| - | - |
| credit release only | 下游 core 发起了 release，本级 port 的 credit 更新 |
| bypass only | 本级发生 bypass 行为而下游 core 没有 release，传的是 credit 消耗信息，单 port 实现不复用 |
| bypass + credit release | 两者同时发生，同步传两个 user_id 与 path_id；只发生在 router → core 场景 |

进 core 缓存的包要重发时，**core 内先同步更新本地的 core credit table，之后 router 的 output 检索到该重发包时再更新自己那一份**。Core credit release 还要向多个上游广播，因为多个上游可能在竞争同一个下游的 CoreMem 资源。

下游资源不满足时，`stall_way` 二选一：

* 留在当前 VC 等
* 把包重定向到本地 Core Mem 缓存，等资源就绪后重发。此时**Router 上的 Bypass 操作被映射成“进 core 加出 core”**

* CoreMem 中**只保存包**，包头含 UserID、PathID、size
  * 重发时用 PathID 重新查 RouterTable，不重复保存 VC 和路由信息
* **同 VC 保序**：同一 VC 存在未完成的 CoreMem 重发包时，后续包不得越过
  * 按 VC 粒度维护 `pending_reinject` 计数器防超车
  * 包进 core 暂存时改写 `overflow_reinject=1`，出 core 重发时 Router 改回 0
  * Output Port 识别到重注入标记才扣减 credit
* 无论直接发送还是经 CoreMem 重发，完成后都向 core 内 TS 返回至少含 UserID 加 PathID 的完成信息
* **不派角色的 core 不接收溢流**：Router 对它不发起进 core 缓存处理，此时 coremem credit 直接 bypass

#### 业务 credit 的旁路

Stream 与 Reduce 两类 release 不是数据包，但也经 Router 转发，走的是另一条规则：

* 软件通过 CSR 为每个业务 credit 输入端口配置**静态输出方向 Mask**，可指定一个或多个 R2R 方向与 Core 方向
* 转发时不查 RouterTable，也不做动态路径选择，不进 CrossBar 的仲裁
* Mask 含多个方向时，同一笔 release 复制到所有指定方向，UserID 与 credit 类型保持不变
* 跨过不落地的 core 时切换 credit 路径，靠的就是改这组静态配置

### 三类 credit 的管理方式

下游收不下有三种不同的原因，对应三类互不复用的 credit。它们管的东西、粒度、维护方、扣还时机都不同：

```svg
<svg viewBox="0 0 1200 790" width="1200" height="790" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="三类 credit 的管理方式">
<title>三类 credit 的管理方式</title>
<rect width="1200" height="790" fill="#ffffff"/>
<defs><marker id="kia" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="kias" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker><marker id="kiai" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#3f4451"/></marker><marker id="kiais" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#3f4451"/></marker><marker id="kiab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="kiabs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="kiar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="kiars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="kiac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="kiacs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="kiap" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="kiaps" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker></defs>
<text x="30" y="26" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="start">三类 credit 的管理方式</text>
<text x="30" y="46" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="start">每一行一类 credit：蓝色是数据的正向路和扣 credit 的时点，橙色是 release 的回程和走法，右侧是表项归谁、怎么建怎么删</text>
<rect x="30" y="70" width="1140" height="226" rx="8" fill="#fbfcfd" stroke="#9aa1ad" stroke-width="1.0"/>
<rect x="30" y="70" width="150" height="226" rx="8" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.0"/>
<text x="105" y="100" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="13" fill="#2563eb" font-weight="700" text-anchor="middle">VC credit</text>
<text x="105" y="122" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">下游 VC Buffer 有没有空槽</text>
<text x="105" y="136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">按下游方向 + VC，flit 粒度</text>
<text x="105" y="162" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="600" text-anchor="middle">快：flit 一进一出就还</text>
<rect x="30" y="306" width="1140" height="226" rx="8" fill="#fbfcfd" stroke="#9aa1ad" stroke-width="1.0"/>
<rect x="30" y="306" width="150" height="226" rx="8" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.0"/>
<text x="105" y="336" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="13" fill="#7c3aed" font-weight="700" text-anchor="middle">stream credit</text>
<text x="105" y="358" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">（CoreMem credit）目标 core 的</text>
<text x="105" y="372" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">Core Mem 有没有空间容纳这个用户</text>
<text x="105" y="398" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#7c3aed" font-weight="600" text-anchor="middle">慢：等用户跑完整条任务链</text>
<rect x="30" y="542" width="1140" height="226" rx="8" fill="#fbfcfd" stroke="#9aa1ad" stroke-width="1.0"/>
<rect x="30" y="542" width="150" height="226" rx="8" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.0"/>
<text x="105" y="572" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="13" fill="#0d9488" font-weight="700" text-anchor="middle">Reduce credit</text>
<text x="105" y="594" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">下游 ReduceModule 有没有上下文空间</text>
<text x="105" y="608" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">按 UserID，flit 粒度</text>
<text x="105" y="634" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#0d9488" font-weight="600" text-anchor="middle">中：按 flit 还，但要等 Reduce 完成</text>
<rect x="210" y="100" width="150" height="60" rx="5" fill="#eef4ff" stroke="#2563eb" stroke-width="1.3"/>
<text x="218" y="115" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#2563eb" font-weight="700" text-anchor="start">本级 Router</text>
<text x="218" y="129" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">RouterStation[方向]</text>
<text x="218" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">VC Credit 计数器</text>
<rect x="220" y="170" width="130" height="48" rx="5" fill="#fef3c7" stroke="#d97706" stroke-width="1.3"/>
<text x="228" y="185" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="start">计数器</text>
<text x="228" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">每个下游方向 × 每个 VC 一个</text>
<text x="228" y="211" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">上电 = 下游 buffer 深度</text>
<rect x="520" y="100" width="170" height="60" rx="5" fill="#eef4ff" stroke="#2563eb" stroke-width="1.3"/>
<text x="528" y="115" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#2563eb" font-weight="700" text-anchor="start">下游 Router</text>
<text x="528" y="129" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">RouterStation 的 VC Buffer ×4</text>
<text x="528" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">flit 按 Header 的 VC 号入槽</text>
<rect x="520" y="170" width="170" height="48" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="528" y="185" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="start">读出</text>
<text x="528" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">VC 头部 flit 经 VA / SA / ST 发走</text>
<text x="528" y="211" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">同拍每个 input port 最多读一个 VC</text>
<path d="M360 125 L519.3 125" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kiab)"/>
<rect x="370" y="107" width="16" height="16" rx="8" fill="#ffffff" stroke="#2563eb" stroke-width="1.2"/>
<text x="378" y="118.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="700" text-anchor="middle">扣</text>
<text x="392" y="118" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#2563eb" font-weight="400" text-anchor="start">发一个 flit，该方向该 VC 减 1</text>
<text x="392" y="140" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#2563eb" font-weight="400" text-anchor="start">多播：所有目标方向都 &gt; 0 才发</text>
<path d="M520 194 L460 194 L460 214" stroke="#d97706" stroke-width="1.4" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 230 L460 246 L300 246 L300 218.7" stroke="#d97706" stroke-width="1.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kiar)"/>
<rect x="452" y="214" width="16" height="16" rx="8" fill="#ffffff" stroke="#d97706" stroke-width="1.2"/>
<text x="460" y="225.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#d97706" font-weight="700" text-anchor="middle">还</text>
<text x="200" y="262" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="400" text-anchor="start">还：flit 离开下游 VC Buffer 就经共享总线 credit_return_vld + credit_return_vc_id 归还一个，逐 flit、不经 CrossBar</text>
<text x="200" y="276" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">重注入的包：Output Port 识别到 overflow_reinject 标记才扣　进 core 这一段不查 VC credit（Stream 已保证 CM 空间）</text>
<rect x="740" y="100" width="420" height="60" rx="5" fill="#ffffff" stroke="#2563eb" stroke-width="1.3"/>
<text x="748" y="115" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#2563eb" font-weight="700" text-anchor="start">谁维护 · 表项</text>
<text x="748" y="129" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">RouterStation 硬件自动维护，没有表项，只有计数器</text>
<text x="748" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">上游只看自己的计数器，不知道下游用哪一格</text>
<rect x="740" y="170" width="420" height="48" rx="5" fill="#ffffff" stroke="#c9ced6" stroke-width="1.3"/>
<text x="748" y="185" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">跨 chip</text>
<text x="748" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">经 C2C Bridge 时同向数据与 credit release 仲裁（小包优先），反向 demux 分流</text>
<rect x="195" y="328" width="120" height="66" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="203" y="343" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#0d9488" font-weight="700" text-anchor="start">源 core</text>
<text x="203" y="357" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">DataOut DTE 要出核</text>
<text x="203" y="369" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">先向 Router 申请授权</text>
<rect x="195" y="402" width="120" height="48" rx="5" fill="#fef3c7" stroke="#d97706" stroke-width="1.3"/>
<text x="203" y="417" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">DTE cache</text>
<text x="203" y="431" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">User Resource</text>
<text x="203" y="443" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">Cache Table</text>
<rect x="345" y="328" width="170" height="66" rx="5" fill="#f5f0ff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="353" y="343" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#7c3aed" font-weight="700" text-anchor="start">本级 Router</text>
<text x="353" y="357" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">Stream Resource Map</text>
<text x="353" y="369" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">User Resource Allocation Table</text>
<text x="353" y="381" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">按 UserID + 目标方向一项</text>
<rect x="345" y="402" width="170" height="48" rx="5" fill="#fef3c7" stroke="#d97706" stroke-width="1.3"/>
<text x="353" y="417" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">唯一有效状态</text>
<text x="353" y="431" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">只有 Router 负责真正申请表项</text>
<text x="353" y="443" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">禁止超额分配或重复授权</text>
<rect x="545" y="328" width="170" height="66" rx="5" fill="#f5f0ff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="553" y="343" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#7c3aed" font-weight="700" text-anchor="start">目标 core 的 Router</text>
<text x="553" y="357" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">进 core 表准入：命中直接收，</text>
<text x="553" y="369" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">有空项则分配，无空项该 VC 停发</text>
<text x="553" y="381" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">TS 内另一份表，同逻辑分配</text>
<rect x="545" y="402" width="170" height="48" rx="5" fill="#fdeed8" stroke="#d97706" stroke-width="1.3"/>
<text x="553" y="417" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#d97706" font-weight="700" text-anchor="start">目标 core</text>
<text x="553" y="431" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">Core Mem 收整包；用户跑完任务链、</text>
<text x="553" y="443" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">用完 CM 后发 release（UserID）</text>
<path d="M315.7 351 L344.3 351" stroke="#d97706" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-start="url(#kiars)" marker-end="url(#kiar)"/>
<text x="330" y="344" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#d97706" font-weight="400" text-anchor="middle">授权</text>
<path d="M515 361 L544.3 361" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kiab)"/>
<rect x="522" y="370" width="16" height="16" rx="8" fill="#ffffff" stroke="#2563eb" stroke-width="1.2"/>
<text x="530" y="381.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="700" text-anchor="middle">扣</text>
<text x="200" y="466" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#2563eb" font-weight="400" text-anchor="start">扣：新 UserID 的包在本级占一个表项；出核由 DTE 拿到授权才发；进 core 由本级表准入，之后不查 VC credit</text>
<path d="M700 450 L700 458" stroke="#d97706" stroke-width="1.4" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M700 474 L700 482 L430 482" stroke="#d97706" stroke-width="1.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kiar)"/>
<rect x="692" y="458" width="16" height="16" rx="8" fill="#ffffff" stroke="#d97706" stroke-width="1.2"/>
<text x="700" y="469.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#d97706" font-weight="700" text-anchor="middle">还</text>
<text x="200" y="498" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="400" text-anchor="start">还：release 是脉冲加 user：每个 Router 一个组合逻辑的 core credit crossbar，汇总本级 core 与所有下级出口的 pulse + user，</text>
<text x="200" y="512" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="400" text-anchor="start">发往除来向外的另两个 R2R port 逐跳上传（left 来就发 right 和 mid），跨 chip 经 C2C Bridge 透传；转发路径由寄存器静态配置</text>
<path d="M430 482 L400 482 L400 450.7" stroke="#d97706" stroke-width="1.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kiar)"/>
<path d="M400 482 L255 482 L255 450.7" stroke="#d97706" stroke-width="1.2" fill="none" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kiar)"/>
<text x="328" y="478" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#d97706" font-weight="400" text-anchor="middle">上游删表项 / DTE cache 同步</text>
<rect x="740" y="328" width="420" height="66" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="748" y="343" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#7c3aed" font-weight="700" text-anchor="start">三份表的关系</text>
<text x="748" y="357" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">Router 的表是唯一有效状态；DTE 持 cache，发数据前必须从 Router 拿授权</text>
<text x="748" y="369" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">TS 内的本级 Stream 表与 Router 的进 core 表按完全一致的逻辑申请空项，</text>
<text x="748" y="381" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">所以“Router 通知 TS 的包一定能被 TS 接收”</text>
<rect x="740" y="402" width="420" height="48" rx="5" fill="#ffffff" stroke="#c9ced6" stroke-width="1.3"/>
<text x="748" y="417" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">建与删</text>
<text x="748" y="431" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">建：新 UserID 首次到达　删：收到 release；或收到 User Retire 后删该用户全部授权表项</text>
<text x="748" y="443" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">Router 另输出 per-port 的 stream_credit 同步信息给 core 与 DTE，用于判断重注入</text>
<rect x="195" y="564" width="120" height="66" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="203" y="579" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#0d9488" font-weight="700" text-anchor="start">本 core 的 DTE</text>
<text x="203" y="593" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">查本级 Reduce credit</text>
<text x="203" y="605" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">够整包才发，还要有 VC credit</text>
<rect x="195" y="638" width="120" height="48" rx="5" fill="#fef3c7" stroke="#d97706" stroke-width="1.3"/>
<text x="203" y="653" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">DTE 持本级表</text>
<text x="203" y="667" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">每用户一个 entry 的 credit</text>
<text x="203" y="679" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">建 stream credit 表项时分配</text>
<rect x="345" y="564" width="170" height="66" rx="5" fill="#eef4ff" stroke="#2563eb" stroke-width="1.3"/>
<text x="353" y="579" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#2563eb" font-weight="700" text-anchor="start">本级 ReduceModule</text>
<text x="353" y="593" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">首份输入建上下文，后续 RMW</text>
<text x="353" y="605" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">输出前查目标 VC credit</text>
<text x="353" y="617" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">和该方向的下游 Reduce credit</text>
<rect x="345" y="638" width="170" height="48" rx="5" fill="#fef3c7" stroke="#d97706" stroke-width="1.3"/>
<text x="353" y="653" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">ResourceMap</text>
<text x="353" y="667" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">按 UserID + 方向维护相邻下游 credit</text>
<text x="353" y="679" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">Router 不维护 Reduce credit</text>
<rect x="545" y="564" width="170" height="66" rx="5" fill="#eef4ff" stroke="#2563eb" stroke-width="1.3"/>
<text x="553" y="579" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#2563eb" font-weight="700" text-anchor="start">下游 ReduceModule</text>
<text x="553" y="593" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">每收一个 flit 累加一次</text>
<text x="553" y="605" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">每发出一个 flit 就 release 一个</text>
<path d="M315 597 L344.3 597" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kiab)"/>
<rect x="322" y="606" width="16" height="16" rx="8" fill="#ffffff" stroke="#2563eb" stroke-width="1.2"/>
<text x="330" y="617.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="700" text-anchor="middle">扣</text>
<path d="M515 597 L544.3 597" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kiab)"/>
<rect x="522" y="606" width="16" height="16" rx="8" fill="#ffffff" stroke="#2563eb" stroke-width="1.2"/>
<text x="530" y="617.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="700" text-anchor="middle">扣</text>
<text x="200" y="702" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#2563eb" font-weight="400" text-anchor="start">扣：每发一个 flit 扣一个。DTE 发往本级 ReduceModule 扣 DTE 持有的；ReduceModule 发往下游扣 ResourceMap 里该 UserID 该方向的</text>
<path d="M700 630 L700 664" stroke="#d97706" stroke-width="1.4" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M700 680 L700 718 L430 718" stroke="#d97706" stroke-width="1.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kiar)"/>
<rect x="692" y="664" width="16" height="16" rx="8" fill="#ffffff" stroke="#d97706" stroke-width="1.2"/>
<text x="700" y="675.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#d97706" font-weight="700" text-anchor="middle">还</text>
<path d="M430 718 L400 718 L400 686.7" stroke="#d97706" stroke-width="1.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kiar)"/>
<path d="M400 718 L255 718 L255 686.7" stroke="#d97706" stroke-width="1.2" fill="none" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#kiar)"/>
<text x="328" y="714" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#d97706" font-weight="400" text-anchor="middle">Valid + UserID，独立释放通道</text>
<text x="200" y="734" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="400" text-anchor="start">还：release 携带 UserID，走业务 credit 的静态旁路：按 CSR 配的方向 Mask 转发，不查 RouterTable，可多播</text>
<text x="200" y="748" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="400" text-anchor="start">上游按 UserID 恢复指定用户的 credit；收到 Retire 且 credit 恢复到分配数量后才删该用户的表项</text>
<rect x="740" y="564" width="420" height="66" rx="5" fill="#ffffff" stroke="#0d9488" stroke-width="1.3"/>
<text x="748" y="579" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#0d9488" font-weight="700" text-anchor="start">谁维护 · 表项</text>
<text x="748" y="593" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">DTE 维护本级 ReduceModule 的 credit（按 user 分配 entry，粒度 flit）</text>
<text x="748" y="605" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">ReduceModule 维护相邻下游各方向的 credit 映射（UserID + 方向）</text>
<text x="748" y="617" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">Router 只负责把 release 按静态 Mask 转发</text>
<rect x="740" y="638" width="420" height="48" rx="5" fill="#ffffff" stroke="#c9ced6" stroke-width="1.3"/>
<text x="748" y="653" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">建与删</text>
<text x="748" y="667" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">建：用户建 stream credit 表项时分配一个 entry 的 credit 数量</text>
<text x="748" y="679" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">删：延迟回收，收到 Retire 后待相邻下游各方向 credit 全部恢复到初始值</text>
</svg>
```

| | VC credit | stream credit（CoreMem credit） | Reduce credit |
| - | - | - | - |
| 管什么 | 下游 VC Buffer 有没有空槽 | 目标 core 的 Core Mem 有没有空间容纳这个用户的数据 | 下游 ReduceModule 的上下文有没有空间 |
| 粒度 | 按下游方向加 VC，flit | 按 UserID 加目标方向，一个表项 | **core ↔ Rmem 按 UserID；Rmem ↔ Rmem 按 flit 加 UserID 双粒度** |
| 谁维护 | 每个下游方向的每个 VC 一个独立计数器，RouterStation 硬件自动维护 | Router 是唯一有效状态（User Resource Allocation Table）；DTE 持一份 cache（User Resource Cache Table）；TS 内另有一份本级表，按与 Router 完全一致的逻辑分配空项 | DTE 维护本级 ReduceModule 的；ReduceModule 维护相邻下游各方向的；Router 不维护 |
| 何时扣 | flit 发出时扣该方向该 VC 一个；经 CoreMem 重注入的包，Output Port 识别到重注入标记才扣 | 新 UserID 的包在本级占一个表项；出核的包由 DTE 先向 Router 申请到授权 | 每发一个 flit 扣一个；DTE 发 Reduce 包前要求本级 credit 够整包 |
| 何时还，走哪条路 | flit 离开下游 VC Buffer 就归还，走共享总线（`credit_return_vld` 加 `credit_return_vc_id`），每个 input port 一拍最多一个 VC 被读出，无冲突 | 用户在下游 core 跑完任务链、用完 Core Mem 后发携带 UserID 的 release；每个 Router 用一个组合逻辑的 core credit crossbar 汇总本级 core 与所有下级出口的 pulse 加 user，发往除来向外的另两个 R2R port，逐跳传到上游；跨 chip 经 C2C Bridge 透传 | 输出 flit 被下游接受后产生携带 UserID 的 release，经业务 credit 的静态旁路路径返回上游 |
| 表项的建与删 | 没有表项，计数器上电等于下游 buffer 深度 | 建：新 UserID 首次到达；删：收到 release，或收到 Retire 后删该用户全部授权表项 | 建：用户建 stream credit 表项时分配一个 entry 的 credit；删：收到 Retire 且 credit 恢复到分配数量 |
| 快慢 | 快，flit 一进一出就还 | 慢，要等那个用户在下游 core 上跑完整条任务链 | 介于两者之间，按 flit 还但要等下游 Reduce 完成 |

stream credit的两条硬规则：

* **只有 Router 负责真正申请表项**。DTE 要发数据必须先从 Router 拿到指定 user 的授权，禁止超额分配或重复授权
* **Router 的进 core 表和 TS 内部的 stream credit 表按完全一致的逻辑申请空项**。分配因此不会多于实际资源数，这保证了“Router 通知 TS 的包一定能被 TS 接收”

Router 另外输出 per-port 的 `stream_credit` 同步信息给 core 与 DTE，用于判断重注入。进 core 这一段不查 VC credit，因为 Stream 已经保证了 Core Mem 有空间。

#### stream credit按 1 KB 记账，广播一次扣的量含预留的输出空间

stream credit这一类 credit 的单位是 **1 KB**。《通信机制（分析过程）》给了三个走完整流程的例子（数值都是示意）：

| 场景 | 扣 | 什么时候还 |
| - | - | - |
| 广播进 core | 该方向的 credit 128 → 96，扣 32（其中 8 KB 是广播数据本身的空间，24 KB 是**提前预留的输出结果空间**） | 广播包计算完成、运算结果写到下游的同时，一次性还回上游 Monitor，96 → 128 |
| Core0 → Core1 的 bypass | Core0 的 Router 查表要 bypass 给 Core1，先查 Core1 的 credit 并扣掉，128 → 112 | Core1 算完释放空间后通知 Core0 的 CreditMonitor，112 → 128 |
| Core2 → Core1 → Core0 的反向 bypass | 扣 Core1 的 credit，128 → 120 | Core1 把数据发给 Core0 后归还给 Core1，120 → 128 |

第一行是关键：广播扣 credit 时扣的不只是广播数据本身，还含这个用户后续输出结果要占的空间。少扣这一份，广播能进但结果写不下，就要在计算完成的那一刻卡住。

***

## 一个包要过的三关

数据面是六级流水线，每级 1 cycle：

```
Input VC Buffer ──→ RC ──→ VA ──→ SA ──→ ST ──→ Output Pipe
   (head flit)      查表    拿资源   抢通路   交换
```

* 单跳延迟 **≤6 cycles**
* 优化空间：RC 与 VA 合并到 5 cycles，再把 SA 与 ST 合并到 4 cycles
* 被 mask 掉的 core 走 **Skip 直通**：数据走完整流水线但不投递 local，延迟与正常跳一致

### 第一关：查表定去向

* RC 阶段只对 head flit 执行
* 从 Header 里取出 `path_id`、`core_mask`、包长、`vc_id`
* 以 `path_id` 为索引查 RouterTable，得到这个包在本级的全部去向与资源需求

RouterTable 是路径解析与资源判定的唯一依据，**只描述静态路由与资源需求，不保存包的动态执行状态**：

| 字段 | 含义 | 用在哪 |
| - | - | - |
| `PathID` | 表项索引，标识一条软件预先规划的业务路径 | Header Parser、重发查询 |
| `op_type` | 2 bit：0 kernel / weight 搬运、1 transfer、2 reduce、3 **reduce_twice** | 路径选择、ReduceModule |
| `flow_dir` | 5 bit 出方向掩码：bit0 上下、bit1 左、bit2 右、bit3 reduce1、bit4 reduce2。**进本 core 不占这里的位** | 输出仲裁、Crossbar |
| `cur_vc` | 包进入当前 Router 时使用的 VC 类型 | 输入 VC 分配 |
| `nxt_vc` | 五个出方向各 2 bit，下一跳使用的 VC 类型 | 输出 Header、下游 VC credit 查询 |
| `path_core_mask_enable` | 0 按 `path_core_bypass` 定是否进 core，1 按 MSG 的 `path_core_mask` 定 | 进 core 判定 |
| `path_core_mask_idx` | 4 bit，看 `path_core_mask` 的哪一位 | 进 core 判定 |
| `path_core_bypass` | 0 进 core，1 bypass | 进 core 判定 |
| `need_buffer` | 这条 path 允许进 core 缓存，即溢流使能 | RC 的溢流判断 |
| `stream_table_enable` | 1 bit：这个包出核前要不要查对应输出端的 stream credit table | stream credit 表 |
| `cur_credit_type` / `cur_credit_require` | 1 / 6 bit：进核占用的 credit 池类型与额度，额度是上游已拨给本核的量 | CoreMem credit |
| `nxt_credit_type` / `nxt_credit_require` | 3 bit 加三个 R2R 方向各 6 bit：各出方向的 credit 池类型与需求，某方向为 0 表示不查 credit | CoreMem credit |
| `reduce_data_type` / `reduce_outdata_type` | 3 / 1 bit：Reduce 的输入精度与输出精度，各取 BF16 或 FP32，**中间累加固定 FP32** | ReduceModule |
| `reduce_in_mask` | 3 bit：这条 path 在本级会有哪几个相邻方向送来分量 | ReduceModule 收齐判据 |
| `operation` | 2 bit：0 普通转发、1 Reduce0、2 Reduce1、3 Reduce2 | 路径选择、ReduceModule |
| `stall_way` | 资源不足时留在当前 VC 等待，还是转入 CoreMem 暂存由 DTE 重发 | 阻塞处理 |

字段照 DATA_NOC HAS 的 `Routing table field`，VC 与阻塞那几项照 Router MAS 的 `Table Entry`，两个 credit require 照《Top 模拟器详设》。`reduce_in_mask` 与 `operation` 的三档 reduce 取值这三份都没有（**待确认**）。

RouterTable 支持 64 条表项，软件通过 R2CU 接口配置，中间节点可以按表改写 VC。复位释放后所有条目为 bypass / no-op，配置写入前不投递任何包。另有一组与 RouterTable 分开配的 **Skip Mask 寄存器**，per-core 一位。

**`reduce_data_type` 为什么配在表里而不是由 TS 给**：`reduce_twice` 时 Router 可能先收到两个远程的 Reduce Token，而不是本 core 发出的那一份，那时 TS 还没有介入。

**`op_type = 0` 的 kernel 与 weight 搬运包进 core 时跳过 TS，直接唤醒 DTE**，不走 `router2ts_trigger_ch` 那条建表通路。

#### path_core_mask：用一个动态位图压掉 path 数

`flow_dir` 是静态的、一条 path 上所有用户共用，它定这个包往哪几个方向发；`path_core_mask` 是动态的、每个包各带一份 16 bit，它只决定这个包进不进本 core。两者各管一半，合起来是这一个包在本级的实际去向。

* **一位对应一个 EP 组的 B core**，理论上最大支持 EP16；溢出时由 DTE core 的软件程序换一个新的 `path_id`
* **位到 core 的对应不是固定编码**：每个 core 在自己的 RouterTable 表项里用 `path_core_mask_idx` 指定看哪一位。同一份 mask 在不同 core 上被解释成不同的位
* 除 EP 广播 path 外，其余 path 可以不用 mask，按 `path_core_bypass` 固定转发或不转发
* **谁填**：Broadcast 来源的包由 SNIC 的 DPU 经 P4 可编程逻辑自动加入；其余来源由 DTE core 配置包头。DTE 只能改 `path_id` 与 `size`，**不提供 `path_core_mask` 的修改接口**，因此这一位图进 LPU 之前就要填好，全程不变

它解决的是 path 数爆炸：纯 `path_id` 编码要 2^K − 1 条，加上 mask 之后只要 K 条 `path_id` 配 2^K 种 mask。按 dp10 / ep16 / pp3 试算，MoE 优化后 47 条、kernel 与 weight 各 4 条，合计 55 条，64 项的表装得下。

mask 还有第二种用法，与压 path 数无关：**DP 广播固定广播到每一个 core，靠 mask 判断这个 core 做不做计算**。好处是整条 DP 广播只占 1 项 RouterTable，代价是浪费总线带宽。如果只有 core0 做计算，core0 之后那一段广播传输其实是多余的。用不用这一档由软件按场景权衡。

#### 什么时候必须换一个新的 path_id

两个通信事务满足任一条时必须分配新的 `path_id`：

1. 涉及的 R2R 路径有重叠，但后续传播的节点不完全一致。例：一个广播到 core01、另一个广播到 core0123
2. 在同一个 core 上做的操作不同。例：core0123 上的 broadcast 与 reduce

反过来，路径不冲突时可以共用同一个 `path_id`：

* 分别在 core01 和 core23 上广播 —— 路径完全无关
* 分别在 core03 和 core23 上做 P2P：路径重叠但后半部分完全一致（可以共用，但没必要，原文只作为辅助理解的例子）

### 第二关：拿资源才放行

VA 阶段做两件事：检查资源，然后在本 input port 内的多个 VC 之间选一个。

**VA 与 SA 的分工**：VA 做 credit 检查加 per-input-port 的 VC 选择，按 **Age-based 最老优先**（比 head flit 的到达时间），**以整包为仲裁粒度**并锁定该 packet；SA 做跨 input port 的 per-output-port 竞争，是 flit 级 RoundRobin。credit 不足的 VC 在 VA 就被跳过，进不到 SA。

目标端口的那个 VC 上有本 core 未发完的溢流缓存时，该 VC 同样被 VA 跳过，同 input 的其他 VC 不受影响。

**三层 credit 各管一段**，互不复用：

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1340 690" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" role="img" aria-label="Router 的三层 credit 各管一段">
<title>三层 credit 各管一段</title>
<defs>
<marker id="av" markerWidth="9" markerHeight="9" refX="7.5" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#2563eb"/></marker>
<marker id="avs" markerWidth="9" markerHeight="9" refX="0.5" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#2563eb"/></marker>
<marker id="as" markerWidth="9" markerHeight="9" refX="7.5" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#d97706"/></marker>
<marker id="ass" markerWidth="9" markerHeight="9" refX="0.5" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#d97706"/></marker>
<marker id="ar" markerWidth="9" markerHeight="9" refX="7.5" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#0d9488"/></marker>
<marker id="ars" markerWidth="9" markerHeight="9" refX="0.5" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#0d9488"/></marker>
<marker id="ag" markerWidth="9" markerHeight="9" refX="7.5" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#6b7280"/></marker>
</defs>
<rect width="1340" height="690" fill="#ffffff"/>
<text x="24" y="30" font-size="15" fill="#111827" font-weight="600">三层 credit 各管一段</text>
<text x="24" y="50" font-size="10.5" fill="#475569">下游收不下有三种不同的原因，Router 用三层互不复用的 credit 分别管，各自的粒度、维护方和释放时机都不同</text>
<rect x="40" y="92" width="180" height="74" rx="4" fill="#eef2f7" stroke="#374151" stroke-width="1.25"/>
<text x="130.0" y="113" font-size="12" fill="#111827" font-weight="600" text-anchor="middle">源 core</text>
<text x="130.0" y="130" font-size="9.5" fill="#475569" text-anchor="middle">DTE 出核</text>
<rect x="280" y="92" width="200" height="74" rx="4" fill="#eef2f7" stroke="#374151" stroke-width="1.25"/>
<text x="380.0" y="113" font-size="12" fill="#111827" font-weight="600" text-anchor="middle">Router A</text>
<text x="380.0" y="130" font-size="9.5" fill="#475569" text-anchor="middle">RC → VA → SA → ST</text>
<rect x="560" y="92" width="200" height="74" rx="4" fill="#eef2f7" stroke="#374151" stroke-width="1.25"/>
<text x="660.0" y="113" font-size="12" fill="#111827" font-weight="600" text-anchor="middle">Router B</text>
<text x="660.0" y="130" font-size="9.5" fill="#475569" text-anchor="middle">RC → VA → SA → ST</text>
<rect x="840" y="92" width="180" height="74" rx="4" fill="#eef2f7" stroke="#374151" stroke-width="1.25"/>
<text x="930.0" y="113" font-size="12" fill="#111827" font-weight="600" text-anchor="middle">目标 core</text>
<text x="930.0" y="130" font-size="9.5" fill="#475569" text-anchor="middle">Core Mem</text>
<rect x="1080" y="92" width="220" height="74" rx="4" fill="#eef2f7" stroke="#374151" stroke-width="1.25"/>
<text x="1190.0" y="113" font-size="12" fill="#111827" font-weight="600" text-anchor="middle">ReduceModule</text>
<text x="1190.0" y="130" font-size="9.5" fill="#475569" text-anchor="middle">Router B 内</text>
<text x="1190.0" y="143" font-size="9.5" fill="#475569" text-anchor="middle">16 用户 × 16 KiB</text>
<path d="M224 129 L279.5 129" fill="none" stroke="#6b7280" stroke-width="1.6" marker-end="url(#ag)"/>
<path d="M484 129 L559.5 129" fill="none" stroke="#6b7280" stroke-width="1.6" marker-end="url(#ag)"/>
<path d="M764 129 L839.5 129" fill="none" stroke="#6b7280" stroke-width="1.6" marker-end="url(#ag)"/>
<path d="M660 90 L660 74 L1180 74 L1180 91.5" fill="none" stroke="#6b7280" stroke-width="1.4" stroke-dasharray="5 3" marker-end="url(#ag)"/>
<text x="920" y="68" font-size="9" fill="#475569" text-anchor="middle">operation = Reduce 时走这一路</text>
<rect x="24" y="200" width="1292" height="128" rx="5" fill="#fbfcfd" stroke="#dbe2ea" stroke-width="1"/>
<text x="40" y="226" font-size="12.5" fill="#2563eb" font-weight="600">VC Credit</text>
<rect x="130.0" y="238" width="530.0" height="14" rx="7" fill="#2563eb" opacity="0.18"/>
<path d="M130.0 230 L130.0 260" stroke="#2563eb" stroke-width="1.6"/>
<path d="M660.0 230 L660.0 260" stroke="#2563eb" stroke-width="1.6"/>
<path d="M136.0 245 L654.0 245" fill="none" stroke="#2563eb" stroke-width="1.5" marker-end="url(#av)" marker-start="url(#avs)"/>
<text x="395.0" y="234" font-size="9" fill="#2563eb" text-anchor="middle">管住的这一段</text>
<text x="40" y="276" font-size="9.5" fill="#475569">管什么：下游 VC Buffer 有没有空槽</text>
<text x="40" y="289" font-size="9.5" fill="#475569">粒度：按下游方向加 VC，flit 粒度</text>
<text x="40" y="302" font-size="9.5" fill="#475569">谁维护：每个下游方向的每个 VC 一个独立计数器，硬件自动</text>
<text x="40" y="315" font-size="9.5" fill="#475569">怎么释放：flit 离开下游 VC 后经共享总线归还，每个 input port 一拍最多一个 VC 被读出</text>
<text x="40" y="328" font-size="9.5" fill="#475569">快：flit 一进一出就还。进 Core 这一段不查它，因为 Stream 已经保证了 CM 空间</text>
<rect x="24" y="348" width="1292" height="128" rx="5" fill="#fbfcfd" stroke="#dbe2ea" stroke-width="1"/>
<text x="40" y="374" font-size="12.5" fill="#d97706" font-weight="600">stream credit</text>
<rect x="130.0" y="386" width="800.0" height="14" rx="7" fill="#d97706" opacity="0.18"/>
<path d="M130.0 378 L130.0 408" stroke="#d97706" stroke-width="1.6"/>
<path d="M930.0 378 L930.0 408" stroke="#d97706" stroke-width="1.6"/>
<path d="M136.0 393 L924.0 393" fill="none" stroke="#d97706" stroke-width="1.5" marker-end="url(#as)" marker-start="url(#ass)"/>
<text x="530.0" y="382" font-size="9" fill="#d97706" text-anchor="middle">管住的这一段</text>
<text x="40" y="424" font-size="9.5" fill="#475569">管什么：目标 core 的 Core Mem 有没有空间容纳这个用户的数据</text>
<text x="40" y="437" font-size="9.5" fill="#475569">粒度：按 UserID 加目标方向的一个表项</text>
<text x="40" y="450" font-size="9.5" fill="#475569">谁维护：Router 是唯一有效状态（User Resource Allocation Table），DTE 持一份 cache</text>
<text x="40" y="463" font-size="9.5" fill="#475569">怎么释放：下游或 Core 通过携带 UserID 的 release 通道通知 Router 回收表项</text>
<text x="40" y="476" font-size="9.5" fill="#475569">慢：要等那个用户在下游 core 上跑完整条任务链。两层不合并，就是因为快慢差着数量级</text>
<rect x="24" y="496" width="1292" height="128" rx="5" fill="#fbfcfd" stroke="#dbe2ea" stroke-width="1"/>
<text x="40" y="522" font-size="12.5" fill="#0d9488" font-weight="600">Reduce Credit</text>
<rect x="660.0" y="534" width="530.0" height="14" rx="7" fill="#0d9488" opacity="0.18"/>
<path d="M660.0 526 L660.0 556" stroke="#0d9488" stroke-width="1.6"/>
<path d="M1190.0 526 L1190.0 556" stroke="#0d9488" stroke-width="1.6"/>
<path d="M666.0 541 L1184.0 541" fill="none" stroke="#0d9488" stroke-width="1.5" marker-end="url(#ar)" marker-start="url(#ars)"/>
<text x="925.0" y="530" font-size="9" fill="#0d9488" text-anchor="middle">管住的这一段</text>
<text x="40" y="572" font-size="9.5" fill="#475569">管什么：下游 ReduceModule 的上下文有没有空间</text>
<text x="40" y="585" font-size="9.5" fill="#475569">粒度：flit 粒度，按 UserID</text>
<text x="40" y="598" font-size="9.5" fill="#475569">谁维护：ReduceModule 维护相邻下游各方向，Router 不维护</text>
<text x="40" y="611" font-size="9.5" fill="#475569">怎么释放：输出 flit 被下游接受后产生携带 UserID 的 release</text>
<text x="40" y="624" font-size="9.5" fill="#475569">用户退休时延迟回收：等相邻下游各方向的 credit 全部恢复到初始值，才删对应用户映射</text>
<text x="24" y="672" font-size="10.5" fill="#475569">三者互不复用。业务层的 Stream 与 Reduce credit 的 release 走静态 Bypass：软件为每个输入端口配好输出方向 Mask，转发时不查 RouterTable，也不做动态路径选择</text>
</svg>
```

准入条件对每个 flit 统一，没有 head 与 body 之分：

* 单播：`credit_cnt[output_port][target_vc] > 0`
* 组播：所有目标方向都满足。**全有或全无**，任一方向不足则所有分支一起等，不允许各方向独立前进

credit 不足的 VC 被 VA 跳过，同一个 input port 的其他 VC 不受影响。多 VC 的三个用处：

1. credit 不足的 VC 不牵连同 port 的其他 VC
2. 把有依赖关系的 path 分到不同 VC，避免循环等待
3. 一个 VC 耗尽不牵连其他 VC

### 第三关：抢通路

**SA 阶段**：

* 每个 output port 各有一个独立的 RoundRobin 仲裁器，每拍独立仲裁，**不跨拍锁定**
* 各 input 的 VA 每拍选出一个 flit 候选送 SA，SA 按目标 port 分发到对应的 output 仲裁器
* 仲裁不是纯 RoundRobin，是**贪婪整包**：多个 flit 竞争时优先让能凑成整包的那个命中
* 优先级由高到低：接口传输 priority → 当前输入是否有整包 → 当前请求是否为上一包的 body → 正常 RoundRobin

**ST 阶段**：

* 通过 Crossbar 把 flit 从 input port 交换到目标 output port
* 广播时同一拍 1 到 N 复制，每个 output 的 Mux 独立控制，多个 output 可选同一个 input

**同 VC 保序**：

* VC Buffer 是 FIFO，同 VC 内 flit 严格按到达顺序读出，VA 与 SA 都不重排
* 跨 VC、跨 input port 之间不保证顺序

***

***

## 死锁

Data_NOC 的死锁面全部落在 VC 上，Router 硬件不做检查，靠软件在编译期分配 VC 时避开。

### 哪些场景会死锁

| 场景 | 死锁 | 原因 |
| - | - | - |
| 单 Router 内，任意 input / output 组合 | 否 | Crossbar 是全连接交换，不存在跨节点的循环依赖 |
| 跨 Router，同 VC 无环路 | 否 | credit 沿树状或递增路径传递，VC 之间独立，不成循环等待 |
| **跨 Router，同 VC 形成物理环路** | **是** | A 等 B 的 credit、B 等 A 的 credit |
| **Reduce 归约回路同 VC 形成物理环路** | **是** | 同上，单 VC 约束 |
| Reduce 单 Router 内，多操作数汇入加结果输出 | 否 | 操作数来自不同 input port，输出结果经 reduce 端口是独立的 SA 请求 |

**唯一的约束**：同一个 VC 在物理拓扑上不能形成依赖环路，广播、归约、P2P 所有场景都算在内。业务的依赖深度超过 VC 数时，拆到不同 VC。Router 硬件不检查这一条，软件编译期保证。RouterTable 提供 VC 改写能力，事后发现死锁可以靠改 VC 超车，代价是牺牲原有单 VC 内的保序。

### 组播不引入新的死锁

Broadcast 是单向传播树，物理上天然无环：同 Router 内是 input 到多个 output，output 之间无依赖；跨 Router 逐跳发散，credit 沿反向归还。整条广播链用同一个 VC 也不成环。组播的阻塞是性能问题（等所有目标 output 空闲），不是死锁。

### 归约不会在网络上堵死

因为有业务层 reduce 的单独 credit 网络，R2R 之间所有请求都能被下游接收（跨 chip 除外），所以 R2R 之间以及 R2Reduce 之间不会形成死锁。

### VC 怎么分才不死锁

| 规则 | 理由 |
| - | - |
| 同一个 VC 尽量只承载单一方向的流量 | 同 VC 内多方向混存会造成 intra-VC 的队头阻塞 |
| Path 之间没有 VC 使用限制，不同 path 可以用同一个 VC | 只要软件保证单 VC 内无物理环路 |
| DP 场景下进 core 的流与 bypass 的流分不同 VC | 避免队头阻塞 |
| 不同 EP 组的流分不同 VC | 一组 credit 耗尽不牵连其他组 |
| 有计算依赖的数据流分不同 VC | 这是死锁约束，不只是性能 |

四个 VC 的分工是固定的一档加三档可配：

| VC | 承载 | 容量 |
| - | - | - |
| VC3 | 只做逐级 reduce | 16 KB |
| VC0 / VC1 / VC2 | 除 reduce 外的全部操作类型，软件按数据流分配 | 各 8 KB |

VC 数取 4，是因为目前最复杂的场景里一个 Router 最多同时经过 4 条同向数据流。逐级 reduce 单独占一个 VC 有三条理由：这类数据基本不进 core，在 Router 里直接与 core 内的数据求和；绝大多数场景都存在逐级 reduce；reduce 要求整包缓冲，每个 VC 都支持 reduce 的话 buffer 尺寸过大。三个传输方向各一套，VC Buffer 合计 (16 + 8 × 3) × 3 = 120 KB。

软件分配 VC 时两条原则的力度不同：可能互相阻塞导致**死锁**的数据流必须分到不同 VC，可能互相阻塞导致**性能下降**的数据流尽量分到不同 VC。

### 换 VC 怎么换

一条 path 前半段做 reduce、后半段做 bypass 时，要在 DTE 的 RV core 软件不介入的前提下由 Router 自己换 VC。必须实现的是 RouterTable 里为每条 `path_id` 指定“下一跳的 VC”。在此之上两种做法比过，结论已定：

| 做法 | 结论 | 说明 |
| - | - | - |
| 下一跳的 VC 编码在包头里，下一跳 Router 解包直接取 | **选中** | 本跳 Router 读到表里的“下一跳 VC”后改写包头里的这个字段，供下一跳读 |
| 包里不带 VC，每个 Router 各查各的表 | 否决 | 表里要同时存“本跳 VC”与“下一跳 VC”，并强制上一跳的“下一跳 VC”等于本跳的“本跳 VC”，配错就死锁 |

VC 机制本身的实现成本极高：额外面积、设计复杂度、验证空间都很大。原始文档把“到底实不实现”留成待定，写明需要模拟器介入综合判断开发复杂度与效果收益。


### 一个具体的死锁实例

EP + PPTP 切分下，FC3 那一行最右侧的 chip：

1. broadcast 把 core 填满
2. user0 的 FC1 silu result 的 P2P 传输被挡住，EP reduction 的 P2P 传输也被挡住
3. core7 上 user0 的任务链因此完不成，用户释放不了
4. 从中间 core 一直到 core7，TS 的表项都释放不了，死锁

两种改法在原始文档里被逐条比过，结论是选第二种：

| 改法 | 缺点 | 优点 |
| - | - | - |
| 任何数据流阻塞都进 core 暂存，core 内为 P2P 单独预留缓冲 | 三个方向都要单独的缓冲空间，挤占用户计算可用的 Core Mem；一旦某个 core 的 TS 满，后续连续数据流都要进一遍 Core Mem，传输延时大幅增加；进出 core 都要 DTE 搬，DTE 可能成为瓶颈；TS 要增加按包类型分流的额外任务链，软件要专门配 | Core Mem 够用时不需要额外存储，省面积；Router 设计简单，不需要复杂的数据 mux |
| **Router 内加 VC Buffer，不同数据流走不同 VC，阻塞只挡同 VC 的流** | 需要额外存储，三个方向的 VC Buffer 各自独立实现；Router 上的数据 mux 走线复杂；设计与验证复杂 | 避免数据流不必要地进出 core，大幅降低传输延时；大幅降低 DTE 的任务负载；credit 只记 Router VC Buffer 的容量，回收链路短、需要的缓冲更少；TS 任务链与 Core Mem 更干净，不必关心不属于本 core 的数据流 |

### 另一类不死锁的前提：Core Mem 容量单调

EP 内 LPU 多播加 PPTP 切分的简化场景里，Path0 是“广播 + P2P + 广播”，会路过但不进入 Core1；Path1 与 Path3 也走 Core1 的通路，会与 Path0 的 P2P 段竞争。这一带不死锁的前提是：**对于 Core1 相关的那两条路径，User N 的 Path1 / Path2 必须排在 User N+4 的 Path0 之前**。

只要每个 Core 的 Core Memory 能容纳的 User 数量一致，或者沿数据流方向前窄后宽，这个次序就永远成立，即不会死锁。

这个场景仍然会出空泡，前提是同一个 User 在 core0 和 core2 上的处理速度不同。一个 TP 组内处理时间差距一般不大，计算量分布均匀时差距主要来自逐级 Reduce。**原始文档在这里明确写了“需要模拟器介入协助确认”**，是本次建模要回答的问题之一。

### 队头阻塞（不是死锁，但影响性能）

| 场景 | 结果 |
| - | - |
| 同 VC 内多方向 flit 混存、多 VC 内同方向 flit 竞争 | 队头阻塞：VC FIFO 头部 flit 的目标 credit = 0 会挡住后面目标 credit > 0 的 flit |
| 不同 input 到同一个 output | 排队。per-output 的 RoundRobin 加 aging，无饥饿 |
| 不同 input 到不同 output | 无阻塞，SA 并行处理 |

***

## 归约

### ReduceModule

Reduce 在 Router 内部完成，不占用 core 的计算单元。

| 组件 | 职责 |
| - | - |
| User Context Table | 记录 UserID、当前包状态、输入完成情况、输出状态与 Retire 状态 |
| Reduce Context SRAM | 16 用户 × 16 KiB，保存当前包的 FP32 中间累加结果 |
| RMW Pipeline | 首份输入建立上下文，后续方向输入执行 Read-Modify-Write **原位**累加 |
| Precision Convert | BF16 输入扩展为 FP32；输出按 RouterTable 配置转 FP32 或 BF16 |
| Downstream Reduce Credit Map | 按 UserID 加目标方向维护相邻下游 Reduce Credit，逐 flit 扣减、按 release 恢复 |
| 循环队列 | ReduceBuffer 本质是一个循环队列：算完的数据从队头搬走，待算的从队尾进；多个用户的数据可以在传输与计算过程中同时存在 |
| 两种 action | ReduceBuffer ⇒ ReduceBuffer，以及 Core ⇒ 本级 ReduceBuffer |
| 分段传输 | ReduceBuffer 之间把 packet 拆成更小的 segment，一个 segment 够 credit 就能发，用来掩盖 R2R 延迟。**Core 到 ReduceBuffer 相反，要等整包备齐再发**，因为这一段延迟本来就小 |
| release 时机 | ReduceBuffer 发出一笔就向上游返回一笔 credit release |
| 输出 VC | Rmem 允许改写输出的 `vc_id`。改写的落点是包头里那个“下一跳 VC”字段，下一跳 Router 解包直接取，不再查表 |
| 边缘压缩 | 跨 chip 的边缘 Rmem 自己做精度压缩，减少 PCIe 带宽 |
| Input/Output Arbiter | 仲裁最多三路输入的 SRAM、Bank 与计算资源 |

三条硬约束：

* **上下文保护**：当前包的全部输入完成并输出前，同一 User 的下一个包不得覆盖该上下文
* **必须执行 Reduce**：SRAM、Bank 或计算单元暂不可用时对输入反压，
  **不允许绕过 Reduce 降级为直接存储或转发**
* **精度**：输入 FP32 或 BF16，BF16 转 FP32 后参与计算，中间累加统一 FP32，输出可配 FP32 或 BF16

性能指标：Reduce 输入三路各 160 GB/s，输出 160 GB/s，算力 80 GFLOPS（FP32 / BF16）。

再加两条来自《通信机制（分析过程）》的边界：

* **整包不分段的那一段决定了容量下界**。Core 必须一次性把 Reduce Token 完整发进 Router 的 ReduceBuffer，不能分段传输，否则效率严重下降。按 8192 × 2 B 算，一个 Token 就是 16 KB，ReduceBuffer 至少要装得下这一个整包。
* **ReduceBuffer 不得用作流量控制的缓存**。“Reduce 完成后不切 `path_id`，直接从 ReduceBuffer 发起 P2P”这种把两条 path 合并的做法能省一次进出 core，但只在后续 P2P 能立即发出时才允许：发不出去就得把数据压在 ReduceBuffer 里，而 ReduceBuffer 只有一项，一压就挡住后续的 Reduce 事务。发不出去的正确做法是让 Reduce 结果先进本 core 的 Core Memory，用 Core Memory 做流控缓冲。

### 逐级与非逐级的分界

同样是 reduce 和 concat，逐级和非逐级走的是两条完全不同的路：

| | 逐级 | 非逐级 |
| - | - | - |
| 在哪算 | Router 里 | 必须进 core，在 Core Mem 里算 |
| 次序 | 按序执行 | 允许不同用户之间乱序到达 |
| 缓冲 | 只缓存最老那个用户的数据 | 每个用户在 Core Mem 里等其他来源到齐 |
| 源的数量 | 最多 3 个：两个上游 core 的分量加一个本 core 的分量 | 不限于 3 个 |

非逐级必须进 core，原因是汇聚过程中用户之间会乱序：chip0 往 chip1 传 usr0 的数据途中，chip1 自己 usr1 的数据可能先到两个 chip 的汇聚点。Router 里只有一个最老用户的上下文，接不住这种乱序。

### 用户退休

资源回收是一份时序契约，建模时是一个明确的状态机：

1. ReduceModule 完成计算并发出全部包后向 Core 返回 UserID
2. Core 判定任务链结束后**向 Router 和 ReduceModule 广播 User Retire**
3. **Core 的保证**：仅可在该 UserID 的全部进 core、出 core 数据搬运完成、
   且不会再发起新搬运后发 Retire。Retire 发出后，Router 上不得再出现以该 Core 为源或目标的该用户包
4. **Router 的动作**：停止该 UserID 的新发送，删除其全部 stream credit 授权表项
5. **ReduceModule 的动作**：**延迟回收**，先记录 Retire，
   待相邻下游各方向 Reduce Credit 全部恢复到初始值后才删除对应用户映射

***

## 跳过与跨 chip

### Skip

边界 chip 多出来的那一列里有一个 core 不派角色：第一列 chip 的 `core5`、最后一列 chip 的 `core4`。它坐在 chip 接 PCIe Switch 的那个口上，只构造 Router，永远不作端点。Router 对它按 Skip 处理：

* 由 Skip Mask 寄存器标记，per-core 一位
* **Router 数据通路照常工作，正常 R2R 转发**
* 它的 local 侧禁用，不接收溢流，credit pulse 无效，`stream_credit` 上电默认 0
* credit 跨过它走：
  * 上游要检查的 credit 对应的是它之后那个落地的 core
  * 下游返还 credit 也跨过它直接给上游
  * 它只按路由表透传，不检查 credit、不支持阻塞重发
* 三种 chip 形状的路由表不同，**路由表必须作为建模输入参数，不能写死**

### C2C Bridge

全 chip 共 4 个，分布在 Mesh 两侧（每行左右两端），不是每 core 一个。四个子模块：

| 子模块 | 功能 |
| - | - |
| 简化 Router | RC / VA / SA 完整流水线，RTL 复用 |
| TX Engine | 拆包：按 4 KB 边界拆分加 4-bit `seq_id` 加 tail 标记；位宽 2048 转 1024 |
| RX Engine | 拼包：按 `seq_id` 缓存，tail 到齐还原原始包；位宽 1024 转 2048 |
| AXI Bridge | Credit 与 AXI4 协议转换；同向数据与 credit release 仲裁（小包优先）；反向 demux 分流 |

VC Buffer 规格，合计约 138.7 KB：

* TX 方向：Private 20 flits/VC × 4，加 Shared 约 20 flits，覆盖本级 R2R 往返约 20 cycles
* RX 方向：Private 80 flits，加 Shared 约 300 flits，覆盖 PCIe 往返 600 ns @1024-bit

AXI 侧的两处特殊处理：

* TX 方向 AXI write 是 posted，写响应可以丢
* RX 方向 AXI 需要响应，由 AXI Bridge 返回 dummy response 以释放 PCIe 的 outstanding 资源

**C2C 只做透明传输**：左侧收到的包默认发到右侧，右侧收到的包默认发到左侧，不做路由判断。业务上不对 C2C 使用独立地址编码方式访问，接口处做流式通信封装。上面所有的路由方案都建立在这个前提上。

***

## 配置与一致性

### RouterTable 的三份副本

**Router 内部**：

* 所有需要并行查询的位置各持一份副本，由 Router 的配置入口统一接收写事务
* 更新状态机把同一笔写依次写入全部副本并记录完成状态
* **全部副本写完才向软件返回完成**，禁止暴露部分新部分旧的状态

**Router 外部**：

* DTE 与 ReduceModule 各自维护自己的 RouterTable
* **由软件负责写入相同配置并保证三方一致，Router 硬件不同步外部副本**
* 软件只能在 Router 提交完成后再写 DTE 和 ReduceModule

### 出核前的资源监听

core 对外发数据要同时满足 VC 资源与 stream credit。监听这两项资源的职责在 Router，一次监听走三步：

1. TS 发送注册事件
2. Router 查资源
3. 满足后通知 TS 调度搬运任务

* 监听事件队列：**16 项全相连**，可同时监听多笔多方向的资源申请
* TS 注册时带 UserID、StreamID、TaskID、PathID；Router 按 PathID 查到需要发送的下游方向、
  VC 需求与 stream 需求，申请到就通过反向控制通路通知 TS（回 StreamID、TaskID、PathID）
* 申请不到就把需求记进队列监听，资源满足再通知
* 多个事件同时满足时按 StreamID 仲裁，选最老的任务通知 TS
* 进 core 重发的任务也注册到该队列，数据进 Core、资源就绪后通知 TS 重发
* 同一 VC 的数据包要保序，当前 VC 有未重发完的数据时后续包不能提前发送

> **取舍**：若改由 TS 监听，大量通信信息要塞进 TS 任务链，计算与通信不再解耦。

***

## 参数

| 项 | 值 |
| - | - |
| 每方向数据宽度 | 256 B |
| 相邻 Router 双向各 | 256 GB/s @1GHz（接口理论值）；HAS 记 R2R 有效带宽 210 GB/s、C2C 90 GB/s |
| 进 core 与出 core | 各 256 GB/s @1GHz，完全并行 |
| 单跳延迟 | ≤6 cycles（六级流水线），优化后 4～5 |
| RouterTable | 64 条表项 |
| VC | 每输入方向 4 类（VC0～3），输出方向不设 VC Buffer |
| VC Buffer | Private 每 VC 20 flits（覆盖 RTT，软件可配，防死锁下限 2）加每方向 Shared Pool 约 20 flits，合计 100 flits/port ≈ 25 KB；SRAM 实现 |
| flit 存储位宽 | 2048 bit payload + 256 bit header = 2304 bit = 288 B |
| R2R 往返 | ≤ 20 cycle（shared pool 深度的依据） |
| Crossbar | 5 入 7 出，每 cycle 最多 7 组 input → output 交换 |
| 拓扑 | 两行的简化二维 Mesh。left / right 连同行相邻 Router，mid 连另一行对称位置那一个；**中间各列的 mid 也连**，作为备份通路（HAS REQ-ARCH-037：提供多路径选择） |
| 单跳延迟拆分 | 横向 R2R 每跳 = internal 6 ns + 走线 10 ns = 16 ns；mid 无走线延迟；PCIe 出入口只有 internal 6 ns。HAS 新增 ASM-03「R2R round trip 最大不超过 20 cycle，单向 C2C latency 最大不超过 300 ns」，单跳约 10 cycle 以内，与这一档相符；第 5 章的 T_R2R = 40 T 对不上，待确认是不是含 core 侧往返的端到端值 |
| 全 chip 广播延迟 | 82 ns（两行并行，Row 0 七跳 82 ns 是关键路径） |
| 单 VC 传输效率 | 每包额外开销 2 cycles（RC 与 VA 不传 flit）：8 KB 包 94%、16 KB 97%、32 KB 98.5%；多输入竞争时按 80% 折算 |
| Rmem per-port buffer | ASM-07 记 128 flits，Area 预算记 3 port × 32 flits，**未解** |
| ReduceBuffer 容量 | 通信机制记 8K × FP32 = 32 KB，正反双份 64 KB；Router MAS 记 16 用户 × 16 KiB，**未解** |
| Reduce 加法器 | 256 B × 1 GHz × 2 输入 = 512 GB/s |
| DTE-local 桥接 buffer | Router → DTE 方向 60 flits ≈ 16.9 KB |
| CTRL_NOC 配置时钟 | 800 MHz（R2CU 接口，APB / AXI-lite，32 bit） |
| 错误四类 | Link 错误、包长度不匹配（VA 阶段查 pkt_length 是否超过目标端口 buffer 能力）、Credit Overflow、Credit Underflow；后两类硬件自动把该 VC 的 credit 复位到固定初值 |
| Reduce 输入 | 三路各 160 GB/s |
| Reduce 输出 | 160 GB/s |
| Reduce 算力 | 80 GFLOPS（FP32 / BF16） |
| ReduceModule 上下文 | 16 用户 × 16 KiB |
| 监听事件队列 | 16 项全相连 |
| C2C Bridge | 全 chip 4 个，VC Buffer 合计约 138.7 KB |
| 不派角色的 core | 边界 chip 各 1 个，中间列没有 |

包结构。硬件包头 16 B、软件包头 16 B，合计 32 B：

| 字段 | 宽度 | 说明 |
| - | - | - |
| Reserved | 7 B | 保留 |
| Hardware Used | 1 B | `Bypass` 1 bit：Router 为 reissue 做保序；`Bad packet` 1 bit：计算途中发现 ECC 之类的问题。**软件不可改** |
| `UserID` | 2 B | 这个包属于哪个用户 |
| Reserved | 1 B | 保留 |
| `PathID` | 1 B | 每级 RouterTable 按它查路由信息，有效 6 bit |
| `CoreMask` | 2 B | Router 按它决定进不进本 core |
| `size` | 2 B | 包的大小，**含包头** |
| 软件包头 | 16 B | 软件自己读写，硬件不解析也不修改 |
| 业务数据 payload | 0 B～(64 KB − 32 B) | |

包里没有 `task_id`，也没有 `vc_id`：

* `task_id` 是 core 内的东西。出核时 `path_id` 由 TS 直连给 DTE、`size` 由 RV core 配寄存器，DTE 拿这两样改写包头；入核时异步 datain 任务由软件识别包头后把 `task_id` 写进 CSR
* **每一级用哪个 VC 记在 RouterTable 里**，包按 `PathID` 索引到表项，从表项拿 VC，不靠包头带

本文其余各节用到的两个包头位，落在上表的哪里：

* `overflow_reinject` 就是 `Hardware Used` 里的 `Bypass` 位。包进 core 暂存时置 1，出 core 重发时改回 0，Router 靠它给 reissue 保序
* `reduce_seq`（6 bit，逐包配对用）占 Reserved 那 8 B 里的位，源文档的字段表没有单列

Header 与 Payload 走**两根独立并行总线**：

* hflit 256-bit 与 pflit 2048-bit，按同一包边界保持对应关系
* 进 core 时拼接成完整包，出 core 时自动拆分

***

## 两份源文档的口径

| 项 | 本篇取值 | 另一份怎么说 |
| - | - | - |
| R2R 方向端口 | 三个：left / right / mid（HAS） | MAS 写“上、下、左、右及 Core 五个方向” |
| Reduce 做在哪 | Router 内的 ReduceModule（MAS） | HAS 写 Router 内不设 Reduce Buffer，累加由独立的 Rmem 子系统完成，经 reduce_0/1/2 三端口接入 |
| Reduce credit 谁维护 | ReduceModule 维护相邻下游各方向，Router 不维护（MAS） | HAS 写 reduce credit 是单独的流控网络，core 与 reduce 之间按 user 粒度、reduce 之间按 flit 加 user 双粒度 |
| stream credit 表 | Router 是唯一有效状态，DTE 持 cache（MAS） | HAS 写 Router 输出单元与 core 内各持一份 credit table，靠 credit release 接口同步 |
| 资源监听队列 | 在 Router，16 项全相连（MAS） | 同一份 MAS 另一处写“功能已转移到 DTE 中” |
| 仲裁粒度 | flit 级，整包只作为贪婪仲裁的优先级偏好 | MAS 有一节“Interleave 和整包的对比”只列两案优劣、未给结论；MAS 正文与 HAS 都是 flit 级 |
| R2R 带宽 | 256 GB/s（接口理论值）与 210 GB/s（HAS 的有效带宽）两个都记 | 两份分别只给其中一个 |
| `vc_id` 位宽 | 按 VC0～3 | HAS 的包格式表里 `vc_id 5-bit（V≤24）` 是 2026/08/19 缩减 VC 之前的残留 |

***

## 取舍

* **为什么把 VC credit 和 stream credit分成两层**
  * 两者管的东西时间尺度差着数量级
  * VC credit 管下游 Router 的 buffer 槽位，flit 一进一出就归还，快
  * stream credit管下游 core 的 Core Mem 空间，要等那个用户在下游 core 上跑完整条任务链才释放，慢
  * 合成一层，快的那层会被慢的拖成一样慢
* **为什么进 Core 之后不再查 VC credit**
  * Stream 检查已经保证目标 core 有 CM 空间，再查一次是重复的资源判定
  * 这一路也没有下游 Router 的 buffer 需要保护
* **为什么组播要全有或全无**
  * 若允许各方向独立前进，一个包的不同分支会走到不同的进度
  * Router 要为每个分支单独维护剩余长度和包边界上下文，状态量按方向数翻倍
  * 一起等的代价是性能，各自走的代价是状态爆炸
* **为什么 Reduce 不允许降级**
  * 若允许绕过 Reduce 直接存储或转发，下游收到的是未归约的原始数据
  * 下游并不知道这件事，也没有补做归约的机会。宁可反压
* **为什么退休时 ReduceModule 要延迟回收**
  * Retire 只说明 core 侧的搬运结束了，而 ReduceModule 发往相邻下游的 flit 可能还在路上
  * 等下游各方向的 Reduce Credit 全部恢复到初始值，才能确认这些 flit 都已被接收

***

配图：[Router 12 张](<Bach/04_四、MAS（Micro Architecture SPEC）/04_Router>)

| 编号 | 内容 |
| - | - |
| `01` | Block Diagram（5×7 CrossBar / Router Station / Interconnect Matrix / Stream Resource Map） |
| `02` | Packet 在 Router 上传输 |
| `03` | 进 core 机制 |
| `04` | 出 core 机制 |
| `05` | Core 出 Reduce |
| `06` | ReduceModule 之间 |
| `07~12` | RouterTable / CSR / 各 Station 与 Xbar 细节 |

另有 [Router OLD 1 张](<Bach/04_四、MAS（Micro Architecture SPEC）/05_Router OLD>)。

内嵌表格：

* [通信机制 6 子表](_sheets/_II1Rs6)（每个 path_id 在各 core 上的进出方向：broadcast、reduce、p2p、EP 多播各一张；`N8MvQ8` 是 path_id 加 path_core_mask 的编码对照）
* [通信机制分析过程 52 子表](_sheets/_BTlDsC)（36 张同构的 path_id 路径表覆盖各切分场景，另有 message 动态字段、logic op 说明、合并前后对照）

来源：

* `04_四、MAS/04_Router.md`：前 400 行为有效内容，Programming Model 之后为 eFUSE 模板残留
* `Bach项目文档/02_Architecture/03_HAS/02_DATA_NOC_DE_HAS.md`
* `06_第四阶段/02_通信机制（分析过程）.md`
