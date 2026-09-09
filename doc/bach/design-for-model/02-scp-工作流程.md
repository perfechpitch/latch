# SCP 的工作流程

**模式**：design（陈述当前设计，动机与取舍收在文末“取舍”一节）

给要实现或建模 SCP 及其配置通路的人：一张总览图定顶点，四个分支各一节，每节一张图加几条短句。

* SCP 在 chip 里的位置、Boot 流程与地址划分：《系统与部署》
* 每笔配置落到哪个寄存器：《TS 任务调度器》《Router 片上交换与归约》与 `07-units/` 各单元文档
* SCP 桩与 ctrl_noc 端点的逐拍行为：[Chip 建模单元](07-units/chip/chip.md)

源文档：`06_第四阶段/05_系统软件需求分析.md`（Boot、模型加载、容错、GDB 调试、状态监控、IPI 与 WFT）、`04_四、MAS/03_Bach_core_MAS_TOP（pending）.md`（core_noc、视野、初始化六步）、`03_第一阶段/01_功能需求规格说明书.md`（boot 通路、复位模式）、`Bach项目文档/02_Architecture/03_HAS/` 下的 MSCP、CTRL_NOC、SOC_TOP 三份 HAS（SoC 层的 SCP 子系统与配置网络）。

《Bach 硬件设计建模参考》SCP 专题，全套目录见 [README](README.md)。

***

## 总览

```svg
<svg viewBox="0 0 920 320" width="920" height="320" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="SCP 的工作流程总览">
<title>SCP 的工作流程总览</title>
<rect width="920" height="320" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">SCP 的工作流程：一个顶点，四个分支</text>
<rect x="200" y="44" width="520" height="58" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="460.0" y="70.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="14" fill="#be123c" font-weight="600" text-anchor="middle">SCP：把 chip 从空白配到能收业务</text>
<text x="460.0" y="85.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#5c6370" text-anchor="middle">运行期只查状态、收故障、做调试；不碰数据面，不碰调度</text>
<rect x="20" y="150" width="205" height="112" rx="7" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="122.5" y="189.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="13" fill="#2563eb" font-weight="600" text-anchor="middle">配置网络 ctrl_noc</text>
<text x="122.5" y="203.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#5c6370" text-anchor="middle">SCP NoC → BCB → CTRL_NOC</text>
<text x="122.5" y="217.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#5c6370" text-anchor="middle">core_noc 端点按 addr_map 分发</text>
<text x="122.5" y="231.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#5c6370" text-anchor="middle">广播 · 视野检查 · 存储后门</text>
<rect x="240" y="150" width="205" height="112" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="342.5" y="189.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="13" fill="#0d9488" font-weight="600" text-anchor="middle">上电到能收业务</text>
<text x="342.5" y="203.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#5c6370" text-anchor="middle">SCP 自启动 → PCIe 训练</text>
<text x="342.5" y="217.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#5c6370" text-anchor="middle">解复位前：ITCM · 固件 · Router 表</text>
<text x="342.5" y="231.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#5c6370" text-anchor="middle">解复位 → firmware → ready → 开放</text>
<rect x="460" y="150" width="205" height="112" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="562.5" y="189.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="13" fill="#7c3aed" font-weight="600" text-anchor="middle">装模型</text>
<text x="562.5" y="203.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#5c6370" text-anchor="middle">kernel：SCP 经 ctrl_noc 写 ITCM</text>
<text x="562.5" y="217.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#5c6370" text-anchor="middle">weights：Host msg 流，不经 SCP</text>
<text x="562.5" y="231.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#5c6370" text-anchor="middle">SCP 配模式 · 读 core_id · 切业务</text>
<rect x="680" y="150" width="205" height="112" rx="7" fill="#fdf1dc" stroke="#d97706" stroke-width="1.4"/>
<text x="782.5" y="189.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="13" fill="#b45309" font-weight="600" text-anchor="middle">运行期</text>
<text x="782.5" y="203.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#5c6370" text-anchor="middle">状态查询</text>
<text x="782.5" y="217.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#5c6370" text-anchor="middle">故障：软件 / 瞬时 / 永久</text>
<text x="782.5" y="231.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#5c6370" text-anchor="middle">调试 · 业务管理动作</text>
<path d="M460.0 102 L460.0 126" stroke="#6b7280" stroke-width="1.5" fill="none"/>
<path d="M122.5 126 L782.5 126" stroke="#6b7280" stroke-width="1.5" fill="none"/>
<path d="M122.5 126.0 L122.5 149.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M342.5 126.0 L342.5 149.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M562.5 126.0 L562.5 149.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M782.5 126.0 L782.5 149.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<text x="460" y="296" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#9aa1ad" font-weight="400" text-anchor="middle">每个分支对应下文一节，框里的三行是那一节的子节点</text>
</svg>
```

* **SCP**（System Control Processor）：每 chip 一个子系统，跑系统控制软件（完整 OS 内核），负责这颗 chip 的启动、控制、监测与调试
  * SoC 级文档把它与负责板级通信的 MCP 合称 MSCP 子系统（`scp_ss`）
  * **上位 CPU**：经 PCIe 与 SCP 通信的上一级处理器，当前是 tray 的 CPU；源文档写作 slave cpu 或 host
  * **IPI 模块**：core 内收集各模块异常与状态、向 SCP 发中断的部件，属于 Core Monitor
* SCP 不做四件事：不搬 weights，不在业务数据面上，运行期不干预 TS 调度，不转发业务包

***

## SCP 的位置与三条通路

```svg
<svg viewBox="0 0 920 360" width="920" height="360" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="SCP 在 chip 里的位置与三条通路">
<title>SCP 在 chip 里的位置与三条通路</title>
<rect width="920" height="360" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">SCP 在 chip 里的位置与三条通路</text>
<rect x="320" y="118" width="560" height="222" rx="8" fill="none" stroke="#9aa1ad" stroke-width="1" stroke-dasharray="5 4"/>
<rect x="160" y="30" width="740" height="320" rx="8" fill="none" stroke="#9aa1ad" stroke-width="1" stroke-dasharray="5 4"/>
<text x="170" y="46" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#9aa1ad" font-weight="600" text-anchor="start">一颗 chip</text>
<text x="870" y="134" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#9aa1ad" font-weight="600" text-anchor="end">Bach core（不派角色的只构造 Router）</text>
<rect x="20" y="150" width="120" height="60" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="80.0" y="170.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#6b7280" font-weight="600" text-anchor="middle">上位 CPU</text>
<text x="80.0" y="184.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">tray 的 CPU</text>
<text x="80.0" y="197.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">经 PCIe 与 SCP 通信</text>
<rect x="170" y="150" width="120" height="60" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="230.0" y="177.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#6b7280" font-weight="600" text-anchor="middle">PCIe</text>
<text x="230.0" y="191.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">四向口之一</text>
<rect x="170" y="54" width="120" height="70" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="230.0" y="79.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#be123c" font-weight="600" text-anchor="middle">SCP 子系统</text>
<text x="230.0" y="93.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">系统控制软件</text>
<text x="230.0" y="106.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">OS 内核 · GDB server</text>
<rect x="320" y="62" width="560" height="26" rx="4" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="600.0" y="79.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#2563eb" font-weight="600" text-anchor="middle">ctrl_noc 配置网络：SCP NoC → BCB → CTRL_NOC</text>
<rect x="340" y="138" width="140" height="40" rx="7" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="410.0" y="155.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#2563eb" font-weight="600" text-anchor="middle">core_noc 端点</text>
<text x="410.0" y="168.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">addr_map 分发</text>
<rect x="340" y="208" width="90" height="46" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="385.0" y="228.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">TS</text>
<text x="385.0" y="241.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">CFG_REG</text>
<rect x="440" y="208" width="100" height="46" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="490.0" y="228.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">Router</text>
<text x="490.0" y="241.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">RouterTable CSR</text>
<rect x="550" y="208" width="110" height="46" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="605.0" y="228.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">RV core ×3</text>
<text x="605.0" y="241.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">ITCM · DTCM · CSR</text>
<rect x="670" y="208" width="86" height="46" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="713.0" y="228.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">DSA ×3</text>
<text x="713.0" y="241.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">静态寄存器</text>
<rect x="766" y="208" width="104" height="46" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="818.0" y="228.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#7c3aed" font-weight="600" text-anchor="middle">存储后门</text>
<text x="818.0" y="241.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">SM · CM · MM</text>
<rect x="340" y="280" width="160" height="44" rx="7" fill="#fdf1dc" stroke="#d97706" stroke-width="1.4"/>
<text x="420.0" y="299.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#b45309" font-weight="600" text-anchor="middle">Core Monitor / IPI</text>
<text x="420.0" y="312.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">状态影子寄存器 · 中断</text>
<path d="M290.0 75.0 L319.0 75.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M410.0 88.0 L410.0 137.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<rect x="415.7" y="108.5" width="52.5" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="442" y="118" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#2563eb" text-anchor="middle">cfg 事务</text>
<path d="M410.0 178 L410.0 194" stroke="#2563eb" stroke-width="1.5" fill="none"/>
<path d="M385.0 194 L818.0 194" stroke="#2563eb" stroke-width="1.5" fill="none"/>
<path d="M385.0 194.0 L385.0 207.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M490.0 194.0 L490.0 207.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M605.0 194.0 L605.0 207.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M713.0 194.0 L713.0 207.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M818.0 194.0 L818.0 207.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M340.0 302.0 L305.0 302.0 L305.0 110.0 L291.0 110.0" stroke="#d97706" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ar)"/>
<rect x="298.2" y="169.5" width="13.5" height="60.9" rx="3" fill="#ffffff" opacity="0.95"/>
<text transform="rotate(-90 305 200)" x="305" y="203.7" font-family="'Noto Sans Mono CJK SC', 'SF Mono', Menlo, Consolas, monospace" font-size="10.5" fill="#d97706" text-anchor="middle">async_int</text>
<path d="M230.0 149.0 L230.0 125.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="126.2" y="132.5" width="67.6" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="160" y="142" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" text-anchor="middle">通知 · 镜像</text>
<path d="M141.0 180.0 L169.0 180.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M230.0 210.0 L230.0 332.0 L319.0 332.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" stroke-dasharray="5 4" marker-end="url(#a)"/>
<rect x="233.0" y="316.5" width="84.0" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="275" y="326" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" text-anchor="middle">业务流不经 SCP</text>
</svg>
```

* 三条通路
  * `ctrl_noc`：SCP → core 内寄存器与 SRAM，也能读回；传路由表、任务链、DSA 静态配置、固件与 kernel、状态查询、调试读写
  * `async_int`：IPI 模块 → SCP；传 boot 完成、异常、故障、调试停机
  * PCIe：SCP ↔ 上位 CPU；传 boot 与 launch 完成、故障通知、kernel 镜像与配置数据
* 地址空间视野：SCP 看到 Bach core 内全部地址空间，其他 master 只看局部

| 模块 | 可见地址空间 |
| - | - |
| SCP | Bach core 内所有地址空间 |
| DTE / MU / VU RV core | 各自 ITCM、DTCM、Share Mem、Core Mem、对应 DSA 的 IO reg |
| DTE DSA | DTE IO reg、Matrix Mem、Core Mem |
| MU / VU DSA | 各自 IO reg、只读 Matrix Mem、Core Mem |

***

## 配置网络：一笔写怎么到目的模块

```svg
<svg viewBox="0 0 920 340" width="920" height="340" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="配置网络的两段">
<title>配置网络的两段</title>
<rect width="920" height="340" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">配置网络：chip 级一段，core 级一段</text>
<rect x="20" y="60" width="80" height="60" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="60.0" y="94.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="13" fill="#be123c" font-weight="600" text-anchor="middle">SCP</text>
<rect x="120" y="60" width="90" height="60" rx="7" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="165.0" y="94.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="13" fill="#2563eb" font-weight="600" text-anchor="middle">SCP NoC</text>
<rect x="230" y="60" width="150" height="60" rx="7" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="305.0" y="81.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#2563eb" font-weight="600" text-anchor="middle">BCB</text>
<text x="305.0" y="94.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">只广播写请求，按地址段分组</text>
<text x="305.0" y="106.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">early / non-early 两种模式</text>
<rect x="400" y="60" width="150" height="60" rx="7" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="475.0" y="81.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#2563eb" font-weight="600" text-anchor="middle">CTRL_NOC</text>
<text x="475.0" y="94.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">AXI4 / APB · 三段地址</text>
<text x="475.0" y="107.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">chip id 5 bit，跨 32 chip</text>
<rect x="610" y="20" width="170" height="44" rx="7" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="695.0" y="46.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#2563eb" font-weight="600" text-anchor="middle">core_noc 端点 ×10</text>
<rect x="610" y="76" width="170" height="44" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="695.0" y="102.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="600" text-anchor="middle">PCIe 配置空间 · 其他 chip</text>
<rect x="610" y="132" width="170" height="44" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="695.0" y="158.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="600" text-anchor="middle">CRG · Debug · Security</text>
<rect x="400" y="164" width="150" height="44" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="475.0" y="183.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="600" text-anchor="middle">PCIe NoC</text>
<text x="475.0" y="196.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">boot 时 BIST 访问 core SRAM</text>
<path d="M100.0 90.0 L119.0 90.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M210.0 90.0 L229.0 90.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M380.0 90.0 L399.0 90.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M550 90 L580 90 L580 154 M580 42 L580 90" stroke="#2563eb" stroke-width="1.5" fill="none"/>
<path d="M580.0 42.0 L609.0 42.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M580.0 98.0 L609.0 98.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M580.0 154.0 L609.0 154.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M475.0 164.0 L475.0 121.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" stroke-dasharray="5 4" marker-end="url(#a)"/>
<rect x="491.0" y="137.5" width="50.0" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="516" y="147" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" text-anchor="middle">配置通路</text>
<rect x="20" y="240" width="220" height="80" rx="7" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="130.0" y="264.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#2563eb" font-weight="600" text-anchor="middle">core_noc 端点（每 core 一个）</text>
<text x="130.0" y="277.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">命中本 core 或广播标记 → 锁存</text>
<text x="130.0" y="290.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">addr_map 查目的模块，下一拍写 cfg 口</text>
<text x="130.0" y="303.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">视野外记地址错；读下一拍回</text>
<rect x="290" y="250" width="96" height="54" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="338.0" y="274.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">TS</text>
<text x="338.0" y="287.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">CFG_REG</text>
<rect x="394" y="250" width="96" height="54" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="442.0" y="274.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">Router</text>
<text x="442.0" y="287.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">RouterTable CSR</text>
<rect x="498" y="250" width="96" height="54" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="546.0" y="274.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">DSA ×3</text>
<text x="546.0" y="287.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">静态寄存器</text>
<rect x="602" y="250" width="96" height="54" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="650.0" y="274.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#7c3aed" font-weight="600" text-anchor="middle">ITCM / DTCM</text>
<text x="650.0" y="287.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">固件 · kernel</text>
<rect x="706" y="250" width="96" height="54" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="754.0" y="274.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#7c3aed" font-weight="600" text-anchor="middle">Share Mem</text>
<text x="754.0" y="287.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">软件表</text>
<rect x="810" y="250" width="96" height="54" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="858.0" y="274.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#7c3aed" font-weight="600" text-anchor="middle">CM / MM 后门</text>
<text x="858.0" y="287.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">4 B/T</text>
<path d="M240 280 L265 280 L265 228 L858 228" stroke="#2563eb" stroke-width="1.5" fill="none"/>
<path d="M338.0 228.0 L338.0 249.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M442.0 228.0 L442.0 249.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M546.0 228.0 L546.0 249.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M650.0 228.0 L650.0 249.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M754.0 228.0 L754.0 249.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M858.0 228.0 L858.0 249.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M780.0 42.0 L800.0 42.0 L800.0 214.0 L130.0 214.0 L130.0 239.0" stroke="#9aa1ad" stroke-width="1.5" fill="none" stroke-linejoin="round" stroke-dasharray="5 4" marker-end="url(#an)"/>
<rect x="285.5" y="199.5" width="29.0" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="300" y="209" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#9aa1ad" text-anchor="middle">放大</text>
</svg>
```

* chip 级（SoC 里的 CTRL_NOC）
  * 路径：SCP → SCP NoC → BCB → CTRL_NOC → 目标子系统
  * 协议：AXI4（只有 increment burst）与 APB3 / 4
  * 地址三段：pcie 段跨 chip；core 段是 core 内存储与配置空间；soc cfg 段是各子系统寄存器；另有 5 bit chip id，最多 32 颗 chip 互访
  * PCIe 经 PCIe NoC → CTRL_NOC → core_noc 访问 core 内 SRAM，只用于 boot 时的 BIST；业务流由 PCIe NoC 直连 core
  * **BCB**（Broadcast Bridge）：把 SCP 的一笔写复制给多个 slave
    * 只广播写请求；按地址段分组，软件逐组使能，命中哪些组就发哪些 slave
    * early 模式收到地址即回 OKAY，随后异步完成，用 fence 防读写乱序；non-early 模式等全部 BRESP 才回
    * 当前单出口，复制包背靠背发出；命中未使能地址段转到 error target，由 NoC 回错误响应
    * 复位后 bypass，SCP 配置后才广播
* core 级（`core_noc` 端点，每 core 一个）
  * 收 `scp_ctrl` 事务，`cfg_core` 命中本 core 或带广播标记时锁存
  * 按 `addr_map` 查目的模块，下一拍写到它的 `cfg` 口；读事务的 `rdata` 下一拍回
  * 地址不在本 core 视野内：记地址错，不下发
  * 广播开关默认关：关时逐 core 写；开时发一次带广播标记的请求给 core0，由 core0 依次广播
  * `core_id` 只读，SCP 经 MMIO 读取，软件不可改
  * 存储后门：Core Mem 与 Matrix Mem 各一个口，4 B/T；Matrix Mem 侧 128 B 对齐、4 B 粒度、burst ≤ 32
    * Core Mem 仲裁：`ctrl_noc` 与 Router 的 CoreMem 重发、DTE RV core 平级，排在三个 DSA 之后
    * Matrix Mem：DTE、`ctrl_noc`、MU 不能同时访问同一 bank，冲突时只执行 MU 并计数报错

***

## 上电到能收业务

```svg
<svg viewBox="0 0 920 440" width="920" height="440" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="上电到能收业务的时间线">
<title>上电到能收业务的时间线</title>
<rect width="920" height="440" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">上电到能收业务：SCP 与各方的时间线</text>
<rect x="10" y="330" width="900" height="90" rx="8" fill="none" stroke="#9aa1ad" stroke-width="1" stroke-dasharray="5 4"/>
<rect x="10" y="230" width="900" height="90" rx="8" fill="none" stroke="#9aa1ad" stroke-width="1" stroke-dasharray="5 4"/>
<rect x="10" y="130" width="900" height="90" rx="8" fill="none" stroke="#9aa1ad" stroke-width="1" stroke-dasharray="5 4"/>
<rect x="10" y="30" width="900" height="90" rx="8" fill="none" stroke="#9aa1ad" stroke-width="1" stroke-dasharray="5 4"/>
<text x="20" y="46" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#9aa1ad" font-weight="600" text-anchor="start">SCP</text>
<text x="20" y="146" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#9aa1ad" font-weight="600" text-anchor="start">PCIe</text>
<text x="20" y="246" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#9aa1ad" font-weight="600" text-anchor="start">Bach core：Router · RV core · TS · DSA</text>
<text x="20" y="346" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#9aa1ad" font-weight="600" text-anchor="start">上位 CPU</text>
<rect x="20" y="50" width="138" height="64" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="89.0" y="73.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#be123c" font-weight="600" text-anchor="middle">① SCP 自启动</text>
<text x="89.0" y="86.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">BOOTROM → BL1 → 切时钟</text>
<text x="89.0" y="99.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">初始化 CTRL_NOC</text>
<rect x="166" y="50" width="138" height="64" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="235.0" y="73.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#be123c" font-weight="600" text-anchor="middle">② 配 PCIe</text>
<text x="235.0" y="86.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">PCIE_PLL · 释放复位</text>
<text x="235.0" y="99.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">配寄存器</text>
<rect x="312" y="50" width="138" height="64" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="381.0" y="73.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#be123c" font-weight="600" text-anchor="middle">③ 解复位前配置</text>
<text x="381.0" y="86.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">ITCM 扫描 · 写固件</text>
<text x="381.0" y="99.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">10 个 Router 配表</text>
<rect x="458" y="50" width="138" height="64" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="527.0" y="79.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#be123c" font-weight="600" text-anchor="middle">④ 解复位</text>
<text x="527.0" y="92.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">写 clk / reset 模块</text>
<rect x="604" y="50" width="138" height="64" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="673.0" y="73.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#be123c" font-weight="600" text-anchor="middle">⑤ 收 boot done</text>
<text x="673.0" y="86.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">轮询 status 或收 IPI</text>
<text x="673.0" y="99.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">ready 全高 → 开放业务</text>
<rect x="750" y="50" width="138" height="64" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="819.0" y="79.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#be123c" font-weight="600" text-anchor="middle">⑥ 通知上位 CPU</text>
<text x="819.0" y="92.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">经 PCIe 报 boot 完成</text>
<rect x="166" y="150" width="138" height="64" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="235.0" y="173.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="600" text-anchor="middle">链路训练完成</text>
<text x="235.0" y="186.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">之后 SCP 才能</text>
<text x="235.0" y="199.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">经 PCIe 通信</text>
<rect x="312" y="250" width="138" height="64" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="381.0" y="273.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">Router 配表</text>
<text x="381.0" y="286.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">多副本写完才回完成</text>
<text x="381.0" y="299.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">再写 DTE / RM 副本</text>
<rect x="458" y="250" width="138" height="64" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="527.0" y="273.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">firmware → WFT</text>
<text x="527.0" y="286.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">TS / Router wait_cfg</text>
<text x="527.0" y="299.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">DSA idle</text>
<rect x="604" y="250" width="138" height="64" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="673.0" y="273.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">三个 boot done</text>
<text x="673.0" y="286.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">自定义指令置 Boot done</text>
<text x="673.0" y="299.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">IPI 中断或 status</text>
<rect x="750" y="350" width="138" height="64" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="819.0" y="379.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="600" text-anchor="middle">收到 boot 完成</text>
<text x="819.0" y="392.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">等下一步 launch</text>
<path d="M158.0 82.0 L165.0 82.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<path d="M304.0 82.0 L311.0 82.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<path d="M450.0 82.0 L457.0 82.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<path d="M596.0 82.0 L603.0 82.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<path d="M742.0 82.0 L749.0 82.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<path d="M235.0 114.0 L235.0 149.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M381.0 114.0 L381.0 249.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M527.0 114.0 L527.0 249.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M596.0 282.0 L603.0 282.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<path d="M673.0 250.0 L673.0 115.0" stroke="#d97706" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ar)"/>
<rect x="676.7" y="130.5" width="78.6" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="716" y="140" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#d97706" text-anchor="middle">IPI / status</text>
<path d="M819.0 114.0 L819.0 349.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
</svg>
```

* 解复位链按 SCP → NoC → PCIe → Bach core 传播，最终顺序由 SoC 设计给出；chip 之间的 SCP 顺序还是并行启动由设计决定
* ① SCP 自启动
  * BOOTROM 取第一条指令跑 BL0；配 SPI-Flash 读 BL1、验签、搬进 SCP-ITCM 后跳转
  * 时钟源从晶振切到 MSCP_PLL，切完 ROM 不再可访问；再配 TOP_PLL 让顶层总线工作
  * CTRL_NOC 默认可用：PMU 退出总线 idle，BCB bypass
* ② 配 PCIe：配 PCIE_PLL、经 CRG 释放复位、配寄存器、完成链路训练；之后才能与上位 CPU 通信
* ③ 解复位前配置（此时 core 内模块全部未解复位，`ctrl_noc` 已连通）
  * 初始化 ITCM：RV core 有分支预测、无预取，只扫 ITCM，避免预测取到未初始化内容触发异常
  * 写固件：RV firmware 写到三个 RV core 的 ITCM reset pc 位置（固定位置）；DTCM 写静态参数
  * 配本 chip 全部 core 的 Router：写 RouterTable、Skip Mask、Credit Bypass Route；不派角色的 core 也写这几项，其他不配
    * Router 上电顺序：PMU 释放 core 时钟域复位 → `stream_credit` 置 0 → RouterTable 全 bypass / no-op → 等 SCP 配表与 VC 使能 → 各 core 发初始化脉冲 → 就绪
    * RouterTable 多副本全部写完 Router 才回完成；SCP 拿到完成后再写 DTE 与 ReduceModule 各自的那一份，硬件不代为同步
* ④ 解复位：SCP 写 clk / reset 模块
  * RV core 从 `boot_pc` 跑 firmware：配 CSR、初始化 gp / sp，执行 **WFT 指令**进 wait
  * WFT 指令：通知 TS 允许下发新任务；可选通知 IPI 向 SCP 上报状态（boot 阶段用）
  * DTE 要能解析 MSG 并执行，解析程序是 DTE kernel 里的一段，随 kernel 镜像装入；MU、VU 不需要
  * TS 无控制核，复位清 0 进 wait_cfg；Router wait_cfg；DSA idle
* ⑤ 收 boot done
  * 三个 RV core 都 boot done → 自定义指令置 Bach core 状态为 Boot done
  * SCP 轮询状态寄存器，或收 IPI 中断
  * 三个 RV core 的 ready 全高 → SCP 开放该 core 的业务接收权限，Router 才收业务
* ⑥ 通知上位 CPU：SCP 经 PCIe 报 boot 完成；此时路由表里还没有业务路径，TS 也没有任务链
* 每个 core 的初始化五步：RV firmware 进 ITCM → 解复位 → TS 初始化（任务链）→ Router 初始化（路由表）→ kernel 初始化；后三步在装模型时做

***

## 装模型：kernel 走 SCP，weights 走 msg 流

```svg
<svg viewBox="0 0 920 330" width="920" height="330" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="装模型的两条路">
<title>装模型的两条路</title>
<rect width="920" height="330" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">装模型：kernel 与 weights 走两条路，SCP 只管切模式</text>
<rect x="740" y="30" width="170" height="290" rx="8" fill="none" stroke="#be123c" stroke-width="1" stroke-dasharray="5 4"/>
<text x="20" y="32" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#2563eb" font-weight="700" text-anchor="start">kernel：KB 级，若干 core 同一份，可广播</text>
<rect x="20" y="42" width="120" height="56" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="80.0" y="68.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="600" text-anchor="middle">上位 CPU</text>
<text x="80.0" y="80.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">kernel 镜像 · 配置数据</text>
<rect x="160" y="42" width="90" height="56" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="205.0" y="74.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="600" text-anchor="middle">PCIe</text>
<rect x="280" y="42" width="100" height="56" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="330.0" y="74.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#be123c" font-weight="600" text-anchor="middle">SCP</text>
<rect x="410" y="42" width="110" height="56" rx="7" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="465.0" y="67.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#2563eb" font-weight="600" text-anchor="middle">ctrl_noc</text>
<text x="465.0" y="80.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">可广播</text>
<rect x="550" y="42" width="170" height="56" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="635.0" y="67.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#7c3aed" font-weight="600" text-anchor="middle">ITCM / DTCM / Share Mem</text>
<text x="635.0" y="80.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">.insn 与 .data 按地址分</text>
<path d="M140.0 70.0 L159.0 70.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M250.0 70.0 L279.0 70.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M380.0 70.0 L409.0 70.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<path d="M520.0 70.0 L549.0 70.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<text x="20" y="142" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#0d9488" font-weight="700" text-anchor="start">weights：MB 级，每 core 不同，不经 SCP</text>
<rect x="20" y="152" width="120" height="56" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="80.0" y="177.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="600" text-anchor="middle">Host</text>
<text x="80.0" y="190.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">msg 流，先发最远</text>
<rect x="160" y="152" width="90" height="56" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="205.0" y="184.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="600" text-anchor="middle">PCIe</text>
<rect x="280" y="152" width="100" height="56" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="330.0" y="177.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">Router</text>
<text x="330.0" y="190.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">单 path P2P</text>
<rect x="410" y="152" width="110" height="56" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="465.0" y="177.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">DTE</text>
<text x="465.0" y="190.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">weights loader</text>
<rect x="550" y="152" width="170" height="56" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="635.0" y="177.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#7c3aed" font-weight="600" text-anchor="middle">Matrix Mem</text>
<text x="635.0" y="190.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">每 core 27 MiB</text>
<path d="M140.0 180.0 L159.0 180.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<path d="M250.0 180.0 L279.0 180.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<path d="M380.0 180.0 L409.0 180.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<path d="M520.0 180.0 L549.0 180.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<rect x="280" y="244" width="100" height="44" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="330.0" y="263.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">TS</text>
<text x="330.0" y="276.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">datain_task</text>
<path d="M330.0 208.0 L330.0 243.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<rect x="335.4" y="222.5" width="49.2" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="360" y="232" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#0d9488" text-anchor="middle">trigger</text>
<path d="M380.0 261.6 L465.0 261.6 L465.0 209.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<rect x="388.3" y="247.5" width="71.4" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="424" y="257" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#0d9488" text-anchor="middle">派 DTE core</text>
<text x="750" y="46" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#be123c" font-weight="600" text-anchor="start">SCP 的配置动作（按时间）</text>
<rect x="755" y="52" width="140" height="62" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="825.0" y="68.3" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#be123c" font-weight="600" text-anchor="middle">① 配 weights 加载模式</text>
<text x="825.0" y="80.8" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">Router 1 条 path</text>
<text x="825.0" y="93.3" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">TS：loader，en = 0</text>
<text x="825.0" y="105.8" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">DTE 六步初始化</text>
<rect x="755" y="128" width="140" height="40" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="825.0" y="145.8" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#be123c" font-weight="600" text-anchor="middle">② 读 core_id</text>
<text x="825.0" y="158.3" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">物理 id 定路由表</text>
<rect x="755" y="182" width="140" height="62" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="825.0" y="198.3" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#be123c" font-weight="600" text-anchor="middle">③ 切业务模式</text>
<text x="825.0" y="210.8" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">Router 业务 path</text>
<text x="825.0" y="223.3" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">TS：token，en = 1，任务链</text>
<text x="825.0" y="235.8" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">DSA 静态配置</text>
<rect x="755" y="258" width="140" height="44" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="825.0" y="277.8" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#be123c" font-weight="600" text-anchor="middle">④ 经 PCIe 通知</text>
<text x="825.0" y="290.3" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">launch 完成</text>
<path d="M825.0 114.0 L825.0 127.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<path d="M825.0 168.0 L825.0 181.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<path d="M825.0 244.0 L825.0 257.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<path d="M720.0 180.0 L728.0 180.0 L728.0 213.0 L754.0 213.0" stroke="#d97706" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ar)"/>
<rect x="685.7" y="222.5" width="52.5" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="712" y="232" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#d97706" text-anchor="middle">中断 SCP</text>
</svg>
```

* 装 kernel（KB 级，若干 core 同一份）
  * 上位 CPU 经 PCIe 把 kernel 镜像与配置数据下发到 SCP 子系统，通知 SCP 开始 launch
  * SCP 经 `ctrl_noc` 写标量缓存：代码段进 ITCM，数据段进 DTCM，按地址区分 `.insn` 与 `.data`；Share Mem 也可写
  * 每类 core 一个 RV32 ELF，配套 `task_pc` 表，把 `task_chain` 每一项的 `TASK_PC` 指到 kernel 的入口
  * 内容：三个 RV core 的固件，计算 core 的 datain 与计算 task，B core 的 `bcore_datain`、`check_flag`、`broadcast`，R core 的映射表维护与求和，weights loader
* 装 weights（MB 级，每 core 不同，不经 SCP）
  * ① SCP 先配 weights 加载模式（三处配置见下表），再通知上位 CPU 可以下发
  * ② SCP 读每个 core 的 `core_id`：编译器按物理 id 配路由表，用户 kernel 只见逻辑 id
  * 数据路径：Host msg 流 → PCIe → Router → Router 通知 TS 触发 datain → TS 派 DTE RV core 跑 weights loader → 算出 Matrix Mem 地址 → DTE 指令搬 Router → Matrix Mem → 完成后通知 TS 释放，不触发任务链；先发最远路径的数据
  * 完成：weights loader 计数满 → 自定义指令中断 SCP → 执行 WFT 进入正常模式
* ③ 切业务模式（SCP 收到中断后改三处）
  * Router 路由表换业务路径：token 广播、逐级 reduce 等
  * TS：`datain_task` 的 pc 指向 token 搬移，`trigger_task_chain_en = 1`；`task_chain` 配成本 core 角色的业务任务链（也可在 weights 模式就配好）
  * DSA 写业务场景的静态配置
  * TS 配置的写入顺序：全局项 `CORE_TYPE`、`STREAM_NUM`、`B_CORE_DIRECTION` → 逐项 `task_chain[i]`（硬件自动置 `TASK_VALID`）→ `DATAIN_TASK` → `TS_INIT_FINISH`；硬件随即查五项合规性写 `TS_STATE`，B / R core 自启动 16 项
* ④ SCP 经 PCIe 通知 launch 完成；此后 token 进来才会算

weights 加载模式的三处配置：

| 配置对象 | 配什么 |
| - | - |
| Router | 路由表只用 1 条 path，是 weights 专用的 P2P 路径；path 与物理 core id 解耦，软件在 path 里指定 core index、在 msg 里标记落在哪些 core；不派角色的 core 数据不进核，仍按位置转发 |
| TS | `WEIGHTS_MODE = 1`；`datain_task` 的 pc 指向 weights loader，`trigger_task_chain_en = 0`，搬完不启动任务链 |
| DTE | SCP 复位 DTE → 全局静态寄存器 → task LUT → stream 表 → header / topK 参数 → 使能 TS 直接触发；DTE 回 `init_done` |

```svg
<svg viewBox="0 0 920 180" width="920" height="180" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="Bach core 状态机">
<title>Bach core 状态机</title>
<rect width="920" height="180" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">Bach core 的状态随 SCP 的配置推进</text>
<rect x="20" y="70" width="130" height="50" rx="7" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.4"/>
<text x="85.0" y="92.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#16181d" font-weight="600" text-anchor="middle">unreset</text>
<text x="85.0" y="105.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">解复位前</text>
<rect x="170" y="70" width="130" height="50" rx="7" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.4"/>
<text x="235.0" y="92.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#16181d" font-weight="600" text-anchor="middle">booting</text>
<text x="235.0" y="105.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">跑 firmware</text>
<rect x="320" y="70" width="130" height="50" rx="7" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.4"/>
<text x="385.0" y="92.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#16181d" font-weight="600" text-anchor="middle">wait_cfg</text>
<text x="385.0" y="105.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">等 datain_task</text>
<rect x="470" y="70" width="130" height="50" rx="7" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.4"/>
<text x="535.0" y="92.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#16181d" font-weight="600" text-anchor="middle">wait_task_chain_cfg</text>
<text x="535.0" y="105.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">等 task_chain</text>
<rect x="620" y="70" width="130" height="50" rx="7" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.4"/>
<text x="685.0" y="92.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#16181d" font-weight="600" text-anchor="middle">idle</text>
<text x="685.0" y="105.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">配完，可调度</text>
<rect x="790" y="70" width="110" height="50" rx="7" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.4"/>
<text x="845.0" y="92.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#16181d" font-weight="600" text-anchor="middle">busy</text>
<text x="845.0" y="105.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">有任务在跑</text>
<path d="M150.0 95.0 L169.0 95.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="139.8" y="54.5" width="39.5" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="159.5" y="64" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" text-anchor="middle">解复位</text>
<path d="M300.0 95.0 L319.0 95.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="296.7" y="124.5" width="25.6" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="309.5" y="134" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" text-anchor="middle">WFT</text>
<path d="M450.0 95.0 L469.0 95.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="415.0" y="54.5" width="89.1" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="459.5" y="64" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" text-anchor="middle">配 datain_task</text>
<path d="M600.0 95.0 L619.0 95.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="567.9" y="124.5" width="83.2" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="609.5" y="134" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" text-anchor="middle">配 task_chain</text>
<path d="M750.0 85.0 L789.0 85.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<rect x="749.8" y="54.5" width="39.5" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="769.5" y="64" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#0d9488" text-anchor="middle">有任务</text>
<path d="M790.0 105.0 L751.0 105.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="750.8" y="124.5" width="39.5" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="770.5" y="134" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" text-anchor="middle">处理完</text>
</svg>
```

* 状态转移的触发者
  * unreset → booting：SCP 解复位；RV core 进 busy，其他模块进 idle
  * booting → wait_cfg：firmware 执行完，RV core 执行 WFT 进 idle
  * wait_cfg → wait_task_chain_cfg → idle：SCP 先配 `datain_task`，再配 `task_chain`
  * idle ⇄ busy：装 kernel 时 `ctrl_noc` 与 ITCM 忙；装 weights 与业务流时 TS 只要有任务在调度就是 busy，处理完回 idle

***

## 运行期：SCP 在数据面之外做的事

```svg
<svg viewBox="0 0 920 300" width="920" height="300" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="运行期的四类事">
<title>运行期的四类事</title>
<rect width="920" height="300" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">运行期：SCP 在数据面之外做的四类事</text>
<rect x="230" y="44" width="460" height="56" rx="7" fill="#fdf1dc" stroke="#d97706" stroke-width="1.4"/>
<text x="460.0" y="69.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="14" fill="#b45309" font-weight="600" text-anchor="middle">运行期的 SCP</text>
<text x="460.0" y="83.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#5c6370" text-anchor="middle">token · 权重 · 结果 · credit 不经 SCP；TS 不接受软件干预</text>
<rect x="20" y="148" width="205" height="120" rx="7" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="122.5" y="193.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="13" fill="#2563eb" font-weight="600" text-anchor="middle">状态查询</text>
<text x="122.5" y="206.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">Core Monitor 影子寄存器 · IPI</text>
<text x="122.5" y="219.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">TS_STATE · Router · DSA 状态</text>
<text x="122.5" y="232.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">上位 CPU 经 PCIe 从 SCP 取</text>
<rect x="240" y="148" width="205" height="120" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="342.5" y="193.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="13" fill="#be123c" font-weight="600" text-anchor="middle">异常与故障</text>
<text x="342.5" y="206.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">软件故障：停整核 → 复位重 boot</text>
<text x="342.5" y="219.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">瞬时故障：poison msg，不经 SCP</text>
<text x="342.5" y="232.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">永久故障：按 SCP 配的阈值上报</text>
<rect x="460" y="148" width="205" height="120" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="562.5" y="193.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="13" fill="#0d9488" font-weight="600" text-anchor="middle">调试</text>
<text x="562.5" y="206.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">SCP 跑 GDB server 与 Debug Agent</text>
<text x="562.5" y="219.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">Stop · 断点 · 单步 · 寄存器与内存读写</text>
<text x="562.5" y="232.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">停机粒度：整个 Bach core</text>
<rect x="680" y="148" width="205" height="120" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="782.5" y="193.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="13" fill="#7c3aed" font-weight="600" text-anchor="middle">业务管理动作</text>
<text x="782.5" y="206.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">专家迁移：源排空 · 目标建 Context</text>
<text x="782.5" y="219.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">热控制：限流 · DVFS · 关机</text>
<text x="782.5" y="232.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">经 PCIe 通知上位 CPU</text>
<path d="M460.0 100 L460.0 124" stroke="#6b7280" stroke-width="1.5" fill="none"/>
<path d="M122.5 124 L782.5 124" stroke="#6b7280" stroke-width="1.5" fill="none"/>
<path d="M122.5 124.0 L122.5 147.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M342.5 124.0 L342.5 147.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M562.5 124.0 L562.5 147.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M782.5 124.0 L782.5 147.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
</svg>
```

* 状态查询：经 `ctrl_noc` 读 Core Monitor 的影子寄存器与 IPI 中断信息、`TS_STATE`、Router 的 perf / debug 寄存器、各 DSA 状态；SCP 能访问 TS 全部寄存器；上位 CPU 经 PCIe 从 SCP 取
* 异常与故障
  * RV core 非法指令、地址错误：中断上报 SCP，RV core 进 firmware 保存现场等 SCP 处理，期间不接受 TS 调度
  * 软件故障（用法或配置违反规范，无比特损坏）：所有用户共用一套 kernel，一处故障影响全部，所以即时检测、上报、停整核，流程见下图
  * 硬件瞬时故障：不经 SCP；硬件中断 DTE core，firmware 读 TS 里的 stream id，把 Core Mem 包头标为 poison msg 后返回，poison msg 传回 Host 由 Host 重发
  * 硬件永久故障：SCP 事先配识别条件（如某故障计数达阈值）；触发后 core 记 RAS 日志、停止、经 IPI 上报；SCP 读 RAS 日志，后续同软件故障或上报换硬件
  * 复位：整 chip 复位（含 SCP 与控制通路、功能单元在跑的任务与队列、PCIe 与 Router 里残留的传输）或只复位 AI Core；以 Group 为单位，Group 内每颗 chip 各收一个复位信号

```svg
<svg viewBox="0 0 920 250" width="920" height="250" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="软件故障流程">
<title>软件故障流程</title>
<rect width="920" height="250" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">软件故障：停整核，修复后复位重 boot</text>
<rect x="20" y="44" width="205" height="70" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="122.5" y="69.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#0d9488" font-weight="600" text-anchor="middle">① core 停机、存状态</text>
<text x="122.5" y="83.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">硬件识别软件故障</text>
<text x="122.5" y="96.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">经 IPI 中断 SCP</text>
<rect x="245" y="44" width="205" height="70" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="347.5" y="69.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#be123c" font-weight="600" text-anchor="middle">② SCP 读日志</text>
<text x="347.5" y="83.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">经 ctrl_noc 读中断日志</text>
<text x="347.5" y="96.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">与各组件异常信息</text>
<rect x="470" y="44" width="205" height="70" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="572.5" y="69.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#be123c" font-weight="600" text-anchor="middle">③ 通知上位 CPU</text>
<text x="572.5" y="83.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">经 PCIe：出了软件故障</text>
<text x="572.5" y="96.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">停止下发业务</text>
<rect x="695" y="44" width="205" height="70" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="797.5" y="69.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#6b7280" font-weight="600" text-anchor="middle">④ 上位 CPU 修代码</text>
<text x="797.5" y="83.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">读 SCP 里的故障信息</text>
<text x="797.5" y="96.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">下发修复后的代码</text>
<rect x="20" y="154" width="205" height="70" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="122.5" y="179.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#be123c" font-weight="600" text-anchor="middle">⑤ 复位 Bach core</text>
<text x="122.5" y="193.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">清空已有业务</text>
<text x="122.5" y="206.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">重走 boot，等各模块反馈</text>
<rect x="245" y="154" width="205" height="70" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="347.5" y="179.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#be123c" font-weight="600" text-anchor="middle">⑥ 更新配置与代码</text>
<text x="347.5" y="193.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">所有相同配置的 core</text>
<text x="347.5" y="206.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">不只是故障 core</text>
<rect x="470" y="154" width="205" height="70" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="572.5" y="186.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#be123c" font-weight="600" text-anchor="middle">⑦ 通知可下发业务</text>
<text x="572.5" y="200.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">经 PCIe</text>
<path d="M225.0 79.0 L244.0 79.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<path d="M450.0 79.0 L469.0 79.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<path d="M675.0 79.0 L694.0 79.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<path d="M225.0 189.0 L244.0 189.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<path d="M450.0 189.0 L469.0 189.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<path d="M797.5 114.0 L797.5 134.0 L122.5 134.0 L122.5 153.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
</svg>
```

* 调试：Bach core 不跑 GDB server；SCP 跑 OS、GDB server 与 Debug Agent，把 GDB 语义翻成 `ctrl_noc` 操作
  * 停机粒度是整个 Bach core：现场分布在多个模块、没有快照机制，只停一部分会不一致；用户级、task 级、指令级只作为触发与观察粒度
  * 每次停机由 core 内 `gdb_ctrl` 收齐各模块状态后经 IPI 通知 SCP；SCP 可以顺带停下协同计算的其他 core

| 手段 | SCP 经 `ctrl_noc` 做什么 | core 内怎么响应 |
| - | - | - |
| Stop / Resume | 写选中 core 的 `gdb_ctrl` | 全局 stop 或 resume TS、RV core、DSA |
| 软件断点 | 配 `EBREAK_ROUTE_SEL`；写 ITCM 把目标指令换成 ebreak | RV core 执行到 ebreak 进 wait，`gdb_ctrl` 进全局调试 |
| 硬件指令断点 | 写 RV core 的 trigger 模块 | 取指地址匹配触发 |
| 内存访问断点 | 写 RV core / DSA 的地址匹配条件 | 读、写或读写地址匹配触发 |
| task 完成断点 | 写 TS 的 trigger 寄存器 | TS 完成指定 task 后停发新 task，`gdb_ctrl` 立即停 RV core 与 DSA |
| 指令级 / task 级单步 | trigger `icount = 1` / 按 task 完成断点配 | 执行完一条指令或一个 task 进 wait |
| 寄存器、内存读写 | 直接读写 GPR、CSR、IO reg 与各类存储 | 无 |

* 业务管理动作
  * 动态专家调度：源 HBU 的 SCP 关闭并排空源专家 Context；目标 HBU 的 SCP 创建目标 Context 并配置 Bach core
  * 功耗与热控制：SCP 读温度传感器；高于紧急门限关机，保护区间内对所有 core 限流，低于正常门限按 DVFS 调频；过温经 GPIO 或 MCP 上报 BMC
  * 经 PCIe 通知上位 CPU：boot 完成、launch 完成、软件故障、可以下发业务

***

## 在 latch 模型里

```svg
<svg viewBox="0 0 920 160" width="920" height="160" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="模型里的 SCP 桩">
<title>模型里的 SCP 桩</title>
<rect width="920" height="160" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">latch 模型里的 SCP：一个桩，一串配置事务</text>
<rect x="20" y="50" width="190" height="66" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="115.0" y="73.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#6b7280" font-weight="600" text-anchor="middle">编译侧输入</text>
<text x="115.0" y="87.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">core_cfg · credit_init</text>
<text x="115.0" y="100.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">kernel_img</text>
<rect x="240" y="50" width="150" height="66" rx="7" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="315.0" y="73.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#2563eb" font-weight="600" text-anchor="middle">scp_img</text>
<text x="315.0" y="87.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">配置事务序列</text>
<text x="315.0" y="100.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">按六步排好</text>
<rect x="420" y="50" width="170" height="66" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="505.0" y="73.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#be123c" font-weight="600" text-anchor="middle">SCP 桩</text>
<text x="505.0" y="87.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">scp_fsm：core_idx · step</text>
<text x="505.0" y="100.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">每笔一拍</text>
<rect x="620" y="50" width="130" height="66" rx="7" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="685.0" y="80.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#2563eb" font-weight="600" text-anchor="middle">ctrl_noc 端点 ×10</text>
<text x="685.0" y="94.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" text-anchor="middle">addr_map</text>
<rect x="780" y="50" width="120" height="66" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="840.0" y="80.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">各模块 cfg 口</text>
<text x="840.0" y="93.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">装载拍数 = 字节 / 4</text>
<path d="M210.0 83.0 L239.0 83.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M390.0 83.0 L419.0 83.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M750.0 83.0 L779.0 83.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M590.0 83.0 L619.0 83.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<rect x="577.5" y="34.5" width="55.0" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="605" y="44" font-family="'Noto Sans Mono CJK SC', 'SF Mono', Menlo, Consolas, monospace" font-size="10.5" fill="#2563eb" text-anchor="middle">scp_ctrl</text>
<text x="460" y="145" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#5c6370" font-weight="400" text-anchor="middle">Router 的 commit_done 拉高后才写 DTE / ReduceModule 的 RouterTable 副本；三个 RV core 的 ready 全高后才开放业务接收</text>
</svg>
```

* SCP 桩每 chip 一个，只接 `ctrl_noc`；`ctrl_noc` 端点每 core 一个；都是每拍推进一次的模块，归属 Chip
* 事务顺序：先给本 chip 全部 core 的 Router 配表，再顺序解复位并配置各个 core；每笔一拍
* 编译侧的 `core_cfg`、`credit_init`、`kernel_img` 在 boot 期变成 `scp_img`；`scp_fsm` 记当前 core 与步骤
* 三份 RouterTable 一致不在输入自洽检查里查，由 SCP 桩的写入顺序保证
* 本轮不建：`async_int` 与 IPI 的行为（留接口名与状态位）；Debug Module、DTM、GDB；Host / Node / tray CPU 的流控与退出；RV core 异常与 TS Except Check 的行为

***

## 参数与口径

| 项 | 取值 | 出处 |
| - | - | - |
| `scp_ctrl` 与 `core_noc` 带宽 | 32 bit/T，每笔事务一拍 | MAS_TOP（待定） |
| CTRL_NOC 配置时钟 | 800 MHz，R2CU 接口，APB / AXI-lite，32 bit | 《建模参数与性能模型》 |
| SCP master 接口 | AXI4，256 bit @ 800 MHz，256 outstanding | CTRL_NOC HAS；CTRL_NOC MAS 写作 128 bit |
| BCB 广播聚合有效带宽 | ≥ 23 GB/s | CTRL_NOC HAS |
| SCP 到同频 slave 端到端延迟 | ≤ 20 cycles | CTRL_NOC HAS |
| 配置寄存器读往返延迟 | ≤ 40 cycles | CTRL_NOC HAS |
| `ctrl_noc` 到 Core Mem / Matrix Mem 后门 | 读写各 4 B/T；Matrix Mem 侧 128 B 对齐、4 B 粒度、burst ≤ 32 | 《执行单元与存储》 |
| `ctrl_noc` 广播开关 | 默认关 | chip.md |
| 跨 chip 配置访问 | 5 bit chip id，最多 32 chip | CTRL_NOC HAS |

设计上未定、建模按前一种取法：

* 解复位链顺序：SCP → NoC → PCIe → Bach core，待 SoC 确认
* RV core 的 reset PC：硬件固定值，还是软件可配且不受复位影响
* Boot 完成通知用的 mailbox 在 core 内还是 core 外
* 广播的实现位置：SoC 级文档放在 SCP 侧的 BCB，core 级文档写的是 core0 转发；建模按 chip.md 的开关
* Router 初始化在六步里的位置：MAS_TOP 排在 TS 之后，《latch 建模计划》提前到解复位前并覆盖不派角色的 core
* SCP 的初始化程序来自 flash 还是经 PCIe 搬入；SCP 与哪个 PCIe 相连
* ITCM 溢出：通知 SCP 重搬 kernel，还是调 DTE DSA 重搬
* kernel 是否也支持经 msg 流搬运
* 装 weights 时是否开写后读做 ECC 提前检查
* 是否用 PMP 给 DTCM 与 Share Mem 划 RV core 只读、SCP 可读写的区域
* GDB 是否一定经 SCP，还是 Host CPU 也可经 PCIe 直接调试
* 软件故障复位前是否先停所有待修 core 并清在途总线交易
* 应用软件的需求：SCP 低频重读或重写全部 weights 而不影响业务

***

## 取舍

* **kernel 走 SCP，weights 走 msg 流**：kernel 重复、KB 级，配置总线够用且能广播；weights 每 core 不同、MB 级，配置总线带宽不够
* **不派角色的 core 的 Router 先配，一个不落**：漏配会让经过它的 path 全断
* **解复位前先初始化 ITCM**：分支预测会取到未初始化内容；没有预取，所以只扫 ITCM
* **运行期 SCP 不碰调度**：TS 写完 `TS_INIT_FINISH` 后按固定逻辑跑，计算与通信的推进不依赖 SCP 的响应时间
* **调试经 SCP，停机停整核**：core 不跑 GDB server；现场分布在多个模块又没有快照，只停一部分会不一致
* **广播放在 SCP 侧的 BCB**：master 侧一次复制比逐个写省延迟，能一次处理去往不同类型 slave 的广播；代价是单出口下某个 slave 卡死会锁住 outstanding，靠 NoC 的错误响应自恢复
* **软件故障停整核、修全部同配置 core**：所有用户共用一套 kernel，继续跑只会破坏现场；修复后所有装了相同配置的 core 都要更新
