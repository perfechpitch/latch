# Bach 要开发哪些软件

> **文档模式：** 设计。陈述当前的软件范围划分，动机与取舍收在文末。
> **文档层级：** 总览。它讲 Bach 这台机器要配的全部软件，不限于 latch 上的重建；同目录的《Bach 中间文件格式》《模型编译器》《数值与参考实现》是其中“模拟器与工具”一块的详细实现。
> **目的：** 给排软件开发计划的人：一共要开发哪些软件，各自跑在哪、吃什么、吐什么、靠哪些硬件接口。每块一张图加几条短句，产物与接口另有一表。

来源：`Bach软件文档库/01_项目管理/01_软件后续工作梳理20260817.md`（五块工作）、`04_总体设计/01_编译器设计构想.md`（编译器模块划分、开发顺序、CodeGen、Runtime）、`03_需求分析/01_应用软件对SOC及系统需求分析.md`（slave CPU、通信、专家均衡、RV core、Debug、Profiling）、`07_技术讨论会议记录/04_应用软件需求讨论 2026-08-19`（slave CPU 选型）、`Bach/02_二、需求分析/06_第四阶段/05_系统软件需求分析.md`（Boot、launch、RAS、GDB、流控）。软件怎么工作在《软件栈》与《SCP 的工作流程》，本文只列要做出什么。

***

## 总览

```svg
<svg viewBox="0 0 920 430" width="920" height="430" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="Bach 软件总览">
<title>Bach 软件总览</title>
<rect width="920" height="430" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">Bach 的软件按运行位置分五块，编译产物贯穿全部</text>
<rect x="280" y="190" width="620" height="200" rx="8" fill="none" stroke="#9aa1ad" stroke-width="1" stroke-dasharray="5 4"/>
<rect x="20" y="60" width="190" height="80" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="115.0" y="91.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#7c3aed" font-weight="600" text-anchor="middle">编译器（离线）</text>
<text x="115.0" y="104.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">导入 → 路由搜索 → CodeGen</text>
<text x="115.0" y="117.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">→ 打包成一个 model 文件</text>
<rect x="280" y="60" width="230" height="80" rx="7" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="395.0" y="91.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#2563eb" font-weight="600" text-anchor="middle">上位机与框架</text>
<text x="395.0" y="104.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">Host CPU · Node / tray CPU · slave CPU</text>
<text x="395.0" y="117.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">推理框架 · 通信 · runtime 加载</text>
<rect x="745" y="60" width="155" height="80" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4" stroke-dasharray="5 4"/>
<text x="822.5" y="91.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#6b7280" font-weight="600" text-anchor="middle">GPU 侧</text>
<text x="822.5" y="104.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">Attention · Router · top-k</text>
<text x="822.5" y="117.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">不在 Bach 开发范围</text>
<text x="290" y="206" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#9aa1ad" font-weight="600" text-anchor="start"></text>
<text x="890" y="206" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#9aa1ad" font-weight="700" text-anchor="end">一颗 chip（×48）</text>
<rect x="300" y="240" width="190" height="90" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="395.0" y="269.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#be123c" font-weight="600" text-anchor="middle">SCP 系统软件</text>
<text x="395.0" y="282.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">boot · launch</text>
<text x="395.0" y="295.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">状态 · RAS · 热控</text>
<text x="395.0" y="308.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">GDB server · Debug Agent</text>
<rect x="540" y="215" width="340" height="150" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="710.0" y="274.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#0d9488" font-weight="600" text-anchor="middle">Bach core 侧固件与 kernel</text>
<text x="710.0" y="287.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">每 core 三个 RV core，各一份 firmware</text>
<text x="710.0" y="300.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">DTE bootloader · weights loader</text>
<text x="710.0" y="313.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">四类 core 的 task kernel</text>
<rect x="20" y="240" width="190" height="100" rx="7" fill="#fdf1dc" stroke="#d97706" stroke-width="1.4"/>
<text x="115.0" y="274.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#b45309" font-weight="600" text-anchor="middle">模拟器与工具</text>
<text x="115.0" y="287.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">latch 逐拍模型 · .bachir 生成器</text>
<text x="115.0" y="300.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">数值参考实现</text>
<text x="115.0" y="313.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">路由可视化 · profiler · GDB 前端</text>
<path d="M210.0 100.0 L279.0 100.0" stroke="#7c3aed" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#am)"/>
<rect x="212.9" y="84.5" width="64.3" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="245" y="94" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#7c3aed" text-anchor="middle">model 文件</text>
<path d="M511.0 100.0 L744.0 100.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="588.8" y="84.5" width="92.4" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="635" y="94" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" text-anchor="middle">token 包 · 结果</text>
<text x="635" y="112" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#6b7280" font-weight="400" text-anchor="middle">RDMA / SmartNIC · 两层 credit</text>
<path d="M395.0 141.0 L395.0 239.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)" marker-start="url(#abs)"/>
<text x="385" y="190" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#2563eb" font-weight="400" text-anchor="end">PCIe：命令 · kernel 镜像 · 配置 ↓</text>
<text x="385" y="203" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#2563eb" font-weight="400" text-anchor="end">状态 · 故障 ↑</text>
<path d="M464.0 140.0 L464.0 180.0 L710.0 180.0 L710.0 214.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<rect x="480.9" y="166.5" width="238.2" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="600" y="176" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#2563eb" text-anchor="middle">msg 流：weights · token（PCIe → Router）</text>
<path d="M490.0 285.0 L539.0 289.9" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<rect x="487.5" y="268.5" width="55.0" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="515" y="278" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#be123c" text-anchor="middle">ctrl_noc</text>
<path d="M115.0 140.0 L115.0 239.0" stroke="#7c3aed" stroke-width="1.5" fill="none" stroke-linejoin="round" stroke-dasharray="5 4" marker-end="url(#am)"/>
<rect x="119.8" y="185.5" width="60.5" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="150" y="195" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#7c3aed" text-anchor="middle">同一份产物</text>
<text x="460" y="412" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="middle">数据面（token · weights · 结果）只经上位机与 core 内软件；控制面（配置 · 状态 · 故障 · 调试）经 SCP。</text>
</svg>
```

* 五块按运行位置分：编译器在开发机上离线跑；上位机与框架在 Host、Node / tray、slave 三级 CPU 上；SCP 系统软件每 chip 一份；core 侧固件与 kernel 跑在每 core 的三个 RV core 上；模拟器与工具在开发机上
* 编译产物贯穿四块：编译器产出，上位机加载，SCP 写进 core，core 侧执行；模拟器吃同一份产物
* 两条通路把它们接起来
  * PCIe：上位机与 SCP 之间传命令、kernel 镜像与配置数据，反向传状态与故障；上位机与 Router 之间传 weights 与 token 的 msg 流
  * ctrl_noc：SCP 写 core 内的固件、kernel、路由表、任务链

***

## 编译器

```svg
<svg viewBox="0 0 920 400" width="920" height="400" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="编译器的模块与产物">
<title>编译器的模块与产物</title>
<rect width="920" height="400" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">编译器：导入六样东西，搜出路径，编出 kernel，打成一个 model 文件</text>
<rect x="20" y="60" width="150" height="74" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="95.0" y="82.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#7c3aed" font-weight="600" text-anchor="middle">导入（六样）</text>
<text x="95.0" y="95.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">harvest mask · placement</text>
<text x="95.0" y="107.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">算子路由需求 · credit 配置</text>
<text x="95.0" y="120.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">任务链与 kernel 源码 · 权重</text>
<rect x="195" y="60" width="125" height="74" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="257.5" y="88.3" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#7c3aed" font-weight="600" text-anchor="middle">Topology Builder</text>
<text x="257.5" y="100.8" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">rack / chip / core 图</text>
<text x="257.5" y="113.3" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">N / S / E / W 边</text>
<rect x="345" y="60" width="130" height="74" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="410.0" y="88.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#7c3aed" font-weight="600" text-anchor="middle">Capability Manager</text>
<text x="410.0" y="100.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">harvest → A / B / C 型</text>
<text x="410.0" y="113.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">compute / EP 能力</text>
<rect x="500" y="60" width="145" height="74" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="572.5" y="88.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#7c3aed" font-weight="600" text-anchor="middle">Operator Normalizer</text>
<text x="572.5" y="100.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">TOML → RouteRequest</text>
<text x="572.5" y="113.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">普通 reduce 自动拆分</text>
<rect x="670" y="60" width="120" height="74" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="730.0" y="88.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#7c3aed" font-weight="600" text-anchor="middle">Joint Solver</text>
<text x="730.0" y="101.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">逻辑 → 物理映射</text>
<text x="730.0" y="113.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">P2P · 广播树 · 归约树</text>
<rect x="815" y="60" width="90" height="74" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="860.0" y="88.3" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#7c3aed" font-weight="600" text-anchor="middle">Route Lowering</text>
<text x="860.0" y="100.3" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" text-anchor="middle">方向 mask</text>
<text x="860.0" y="112.3" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" text-anchor="middle">enter_internal</text>
<path d="M170.0 97.0 L194.0 97.0" stroke="#7c3aed" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#am)"/>
<path d="M320.0 97.0 L344.0 97.0" stroke="#7c3aed" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#am)"/>
<path d="M475.0 97.0 L499.0 97.0" stroke="#7c3aed" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#am)"/>
<path d="M645.0 97.0 L669.0 97.0" stroke="#7c3aed" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#am)"/>
<path d="M790.0 97.0 L814.0 97.0" stroke="#7c3aed" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#am)"/>
<rect x="195" y="220" width="160" height="64" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="275.0" y="243.3" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#7c3aed" font-weight="600" text-anchor="middle">Independent Verifier</text>
<text x="275.0" y="255.8" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">独立于 solver 的规则检查</text>
<text x="275.0" y="268.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">P2P · 广播 · 归约 · EP 五组</text>
<rect x="380" y="220" width="130" height="64" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="445.0" y="243.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#7c3aed" font-weight="600" text-anchor="middle">Visualizer</text>
<text x="445.0" y="256.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">mesh · 坏核 · 映射</text>
<text x="445.0" y="268.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">每条 path 的有向树</text>
<rect x="535" y="220" width="180" height="64" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="625.0" y="243.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#7c3aed" font-weight="600" text-anchor="middle">CodeGen</text>
<text x="625.0" y="256.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">kernel 编译链接（裸机 · PIC）</text>
<text x="625.0" y="268.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">weights 切分重排 · 路由表</text>
<rect x="740" y="220" width="165" height="64" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="822.5" y="243.8" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#7c3aed" font-weight="600" text-anchor="middle">打包 → model 文件</text>
<text x="822.5" y="255.8" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" text-anchor="middle">PPUF：metadata 索引 · mmap</text>
<text x="822.5" y="267.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" text-anchor="middle">四类产物各占一段</text>
<path d="M860 134 L860 180 L275 180" stroke="#7c3aed" stroke-width="1.5" fill="none"/>
<path d="M275.0 180.0 L275.0 219.0" stroke="#7c3aed" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#am)"/>
<path d="M445.0 180.0 L445.0 219.0" stroke="#7c3aed" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#am)"/>
<path d="M625.0 180.0 L625.0 219.0" stroke="#7c3aed" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#am)"/>
<path d="M715.0 252.0 L739.0 252.0" stroke="#7c3aed" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#am)"/>
<text x="460" y="320" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="middle">产物：每 core 的 RouterTable · Skip Mask · Credit Bypass Route · task_chain · datain_task；每类 core 一个 RV32 ELF 与 task_pc 表；每 core 的 weights 分片；一个 model 文件。</text>
<text x="460" y="345" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="middle">推荐开发顺序：① Topology 与输出格式　② P2P 联合 mapping　③ 普通 broadcast　④ 普通 reduce　⑤ EP broadcast / reduce　⑥ 多 rack 与 Fast 模式</text>
<text x="460" y="370" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="middle">Exact 模式全局联合求解；Fast 模式按 chip 分步搜索，用于多 rack。Verifier 与 Visualizer 早做，路由问题的调试全靠它们。</text>
</svg>
```

* 输入六样：harvest disable mask、placement（rack × chip × core）、算子给出的待搜索路径（P2P / broadcast / reduce 的源与目的）、路由 credit 配置、task chain 形式的子 kernel 调用顺序与 kernel 源码、模型权重（Hugging Face 或 GGUF）
* 不导入计算图做图优化：MoE、FC0+MoE+QKV 都是 fuse 后手写的算子，重心是逻辑 core 到物理 core 的映射与路由搜索
* 七个模块：Topology Builder、Capability Manager、Operator Normalizer、Joint Solver、Route Lowering、Independent Verifier、Visualizer；Verifier 不复用 solver 的约束代码
* CodeGen 四件事：按任务链编译链接 kernel（裸机、PIC、无 main，结束后等下一次调度）；weights 按算子切分策略分块重排到 core；对规划好的路径生成路由表；kernel、weights、路由表、任务表分段打成一个 model 文件（参考 GGUF 设计 PPUF，metadata 索引，支持 mmap）
* 编译器输入文件三份：`hardware.toml`（拓扑与全局进出口）、`harvest.toml`（每 chip 坏核 mask）、`operator_routes.toml`（算子路由与 path id）
* 模型支持：优先 MoE 类（DeepSeek、Kimi、GLM），换模型先手动写 ShardedTensor 与任务链，再自动绑定手写算子生成源码

***

## 上位机与框架

```svg
<svg viewBox="0 0 920 360" width="920" height="360" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="上位机与框架">
<title>上位机与框架</title>
<rect width="920" height="360" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">上位机与框架：三级 CPU 各管一段，框架与通信把 GPU 和 HBU 接起来</text>
<rect x="20" y="60" width="210" height="112" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="125.0" y="100.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="600" text-anchor="middle">Host CPU（全局管理）</text>
<text x="125.0" y="113.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">业务发起 · 资源编排 · 生命周期</text>
<text x="125.0" y="126.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">专家调度器：生成迁移决策</text>
<text x="125.0" y="139.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">正常退出：停止下发，等逐级完成</text>
<rect x="260" y="60" width="230" height="112" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="375.0" y="100.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="600" text-anchor="middle">Node / tray CPU</text>
<text x="375.0" y="113.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">Node 创建 · 任务准入 · Node 级限流</text>
<text x="375.0" y="126.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">停止排空 · 完成判断 · 资源释放</text>
<text x="375.0" y="139.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">协调专家迁移 · 向 Host 返回状态</text>
<rect x="520" y="60" width="240" height="112" rx="7" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="640.0" y="89.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#2563eb" font-weight="600" text-anchor="middle">slave CPU（HBU 板上）</text>
<text x="640.0" y="101.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">解析 model 文件，按 core 拆分加载</text>
<text x="640.0" y="114.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">路由配置 · 发起传输</text>
<text x="640.0" y="126.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">trace 归集 · profiling 配置</text>
<text x="640.0" y="138.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">热点专家统计与权重重排</text>
<text x="640.0" y="151.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">节点状态监测 · 通信兜底</text>
<rect x="790" y="60" width="110" height="112" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4" stroke-dasharray="5 4"/>
<text x="845.0" y="100.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="600" text-anchor="middle">GPU 侧</text>
<text x="845.0" y="113.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">Attention · Router</text>
<text x="845.0" y="126.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">top-k</text>
<text x="845.0" y="139.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">不在开发范围</text>
<rect x="260" y="230" width="500" height="84" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="510.0" y="256.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#7c3aed" font-weight="600" text-anchor="middle">框架与通信</text>
<text x="510.0" y="269.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">AF 分离推理框架（参考 vLLM 的 AFD 分支）</text>
<text x="510.0" y="282.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">HBU ↔ GPU：RDMA / SmartNIC，或 DPU 转发；GPU → Bach 两层 credit 反压</text>
<text x="510.0" y="295.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">runtime：解析 PPUF，weights / kernel 加载；与 SCP 的 PCIe 命令协议</text>
<path d="M230.0 116.0 L259.0 116.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="200.7" y="40.5" width="88.6" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="245" y="50" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" text-anchor="middle">业务 · 迁移决策</text>
<path d="M490.0 116.0 L519.0 116.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="471.2" y="40.5" width="67.6" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="505" y="50" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" text-anchor="middle">任务 · 限流</text>
<path d="M640.0 173.0 L640.0 229.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)" marker-start="url(#abs)"/>
<path d="M845.0 173.0 L845.0 272.0 L761.0 272.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="798.8" y="195.5" width="92.4" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="845" y="205" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" text-anchor="middle">token 包 · 结果</text>
<text x="460" y="342" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="middle">第一版：低配 slave CPU 主控，数据先落 DDR 再转发到 HBU，先打通与 GPU 的链路；后续评估把 slave CPU 的职责迁到 DPU。</text>
</svg>
```

* Host CPU：业务生命周期的所有者，停止提交、跨 LPU / GPU 协调、资源释放；专家调度器按全局负载生成迁移决策
* Node / tray CPU：Node 创建、任务准入（看 Node buffer、SmartNIC 队列、tray 间拥塞）、限流、停止排空、完成判断、资源释放、状态返回；专家迁移时协调源 tray 与目标 tray
* slave CPU（HBU 板上，第一版）
  * 初始化：解析 model 文件，按 core 拆分，配置路由（或地址路由），发起 kernel 与 weights 传输
  * 观测：配置全节点 profiling，收各 core 的 trace 到 DDR，供上位机取
  * 通信：SmartNIC 或 DPU 做不了协议转换与数据中转时兜底承载
  * 专家均衡：统计各专家负载，按结果重排权重并从 DDR 搬到 HBU
  * 监测：每个 HBU 节点的详细状态，汇总上报
* 框架与通信
  * AF 分离的推理框架，GPU 做 Attention，Bach 做 MoE；参考带 AF 分离的 vLLM 分支
  * HBU 与 GPU 之间：RDMA 经 SmartNIC，或 HBU 写给 DPU 由 DPU 转发；GPU 到 Bach 两层 credit 反压
  * runtime：解析 PPUF，加载 weights 与 kernel；与 SCP 的 PCIe 命令协议
* 流控落点：需要 CPU 介入的 buffer 只有四处 Matrix Mem（输入端残差、Broadcast core、Reduction core、输出端残差），手段是关 PCIe Switch 注入口或叫停 SmartNIC 的 token 下发

***

## 上位机与 HBU 之间的收发软件

```svg
<svg viewBox="0 0 920 370" width="920" height="370" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="上位机与 HBU 之间的收发">
<title>上位机与 HBU 之间的收发</title>
<rect width="920" height="370" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">上位机与 HBU 之间：收、发、credit 账本三条软件流程</text>
<rect x="20" y="60" width="120" height="70" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="80.0" y="86.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#6b7280" font-weight="600" text-anchor="middle">GPU[g]</text>
<text x="80.0" y="99.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">HBM buffer</text>
<text x="80.0" y="112.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">window_remaining</text>
<rect x="200" y="60" width="220" height="90" rx="7" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="310.0" y="90.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#2563eb" font-weight="600" text-anchor="middle">SNIC / DPU：收</text>
<text x="310.0" y="103.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">解 batch（或放 GPU 侧）</text>
<text x="310.0" y="115.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">加 user_id：gpu_id · SNIC 信息 · 自增号</text>
<text x="310.0" y="128.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">加 path_id（编译器定）· 协议转换</text>
<rect x="470" y="60" width="120" height="70" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="530.0" y="92.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#6b7280" font-weight="600" text-anchor="middle">PCIe Switch</text>
<text x="530.0" y="105.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">组播为可选项</text>
<rect x="640" y="60" width="260" height="90" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="770.0" y="90.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#0d9488" font-weight="600" text-anchor="middle">HBU</text>
<text x="770.0" y="103.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">Router → B core 的 Matrix Mem</text>
<text x="770.0" y="115.6" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">R core 算完，Dataout DTE 发结果</text>
<text x="770.0" y="128.1" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">写 {g, n_retired} 到 CQ shard</text>
<rect x="200" y="210" width="220" height="100" rx="7" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="310.0" y="245.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#2563eb" font-weight="600" text-anchor="middle">SNIC / DPU：发（方案 B）</text>
<text x="310.0" y="257.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">HBU 写到 SNIC 可见空间，有反压</text>
<text x="310.0" y="270.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">解析 HBU 包，按 gpu_id 构 RDMA Write WQE</text>
<text x="310.0" y="282.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">写远端 GPU 后释放 credit</text>
<rect x="470" y="210" width="430" height="100" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="685.0" y="239.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#7c3aed" font-weight="600" text-anchor="middle">slave CPU：credit 账本</text>
<text x="685.0" y="251.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">Step0 冷启动：granted_pending[g] = K[g]，Σ K ≤ pool_total</text>
<text x="685.0" y="264.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">Step2 收数：inflight_bach[g] += n，granted_pending[g] −= n</text>
<text x="685.0" y="276.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">Step3 retire：轮询 CQ shard，inflight_bach[g] −= n_retired</text>
<text x="685.0" y="289.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">Step4 refill：写 GPU 的 window_remaining（MMIO 或中断）</text>
<path d="M140.0 95.0 L199.0 104.8" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="136.6" y="42.5" width="66.8" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="170" y="52" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" text-anchor="middle">RDMA Write</text>
<path d="M420.0 105.0 L469.0 95.2" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M590.0 95.0 L639.0 104.8" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="588.1" y="78.5" width="53.8" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="615" y="88" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" text-anchor="middle">token 包</text>
<path d="M666.0 150.0 L666.0 180.0 L440.0 180.0 L440.0 255.0 L421.0 255.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<rect x="492.7" y="166.5" width="120.6" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="553" y="176" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#0d9488" text-anchor="middle">结果 12 KiB，经 PCIe</text>
<path d="M200.0 255.0 L80.0 255.0 L80.0 131.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="107.2" y="240.5" width="65.5" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="140" y="250" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" text-anchor="middle">写 GPU HBM</text>
<path d="M801.2 151.0 L801.0 209.0" stroke="#7c3aed" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#am)" marker-start="url(#ams)"/>
<rect x="636.8" y="175.5" width="152.5" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="713" y="185" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#7c3aed" text-anchor="middle">CQ shard（DDR）· doorbell</text>
<path d="M420.0 290.0 L469.0 290.0" stroke="#2563eb" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ab)"/>
<rect x="409.4" y="274.5" width="70.2" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="444.5" y="284" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#2563eb" text-anchor="middle">释放 credit</text>
<path d="M599.0 310.0 L599.0 335.0 L40.0 335.0 L40.4 131.0" stroke="#7c3aed" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#am)"/>
<rect x="277.8" y="321.5" width="84.4" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="320" y="331" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#7c3aed" text-anchor="middle">refill window</text>
<text x="460" y="360" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="middle">两层 credit 的变量与门控在《软件栈》：1 credit = 1 token；PCIe Switch 组播时按最慢收端聚合后再回报 GPU。</text>
</svg>
```

* 收（GPU → HBU）：DPU 的可编程流水线解 batch、加 user_id（gpu_id 由 QP 映射表查，带 SNIC 信息与自增号）、加编译器定的 path_id，协议转换后进 HBU 内部总线；CPU 不参与
* 发（HBU → GPU）：选方案 B，DPU 转发。HBU 的 DTE 把结果写到 SNIC 的可见空间（不是 DDR，带宽不够），HBU 与 SNIC 之间要有反压；DPU 解析 HBU 包、按 gpu_id 构 RDMA Write WQE 写远端 GPU，完成后释放 credit。方案 A 由 SNIC 读 HBU 内数据，代价大，不做
* credit 账本（slave CPU）：维护 pool_total、inflight_bach[g]、granted_pending[g]、pool_avail、每 GPU 的窗口 K[g]；retire 由 R core 的 Dataout DTE 写 CQ shard 并 doorbell，CPU 轮询 head / tail
* 其他收发都要过协议转换
  * CPU 给 HBU 发数据：CPU 加 HBU 包头作为 payload，发到 HBU 接 PCIe Switch 的 port 地址；不做性能要求
  * HBU 给 DDR 发：软件包头带目的地址，协议转换模块丢掉包头写 DDR
  * HBU 经 PCIe Switch 写另一个 HBU：R core 与 B core 之间传 credit，包头带目的 HBU 的 port 地址与专用帧格式指示符
  * x32 是两个 x16 port：发送时软件包头地址高位 bit 选 port，其余地址位透传；接收侧硬件合并成一路
* 本节待定：Bach 到 GPU 是否组 micro batch 后批量发、在哪组；path_id 路由到 PCIe 域地址的转换，软件包头里 PCIe offset 的位宽；DDR 带宽是否够（SNIC 50 GB/s 时 DDR 一写一读要 100 GB/s 以上）

***

## SCP 系统软件

```svg
<svg viewBox="0 0 920 300" width="920" height="300" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="SCP 系统软件">
<title>SCP 系统软件</title>
<rect width="920" height="300" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">SCP 系统软件：三段流程跑在同一套公共层上</text>
<rect x="20" y="60" width="280" height="124" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="160.0" y="100.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#be123c" font-weight="600" text-anchor="middle">boot</text>
<text x="160.0" y="113.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">BL0 / BL1 引导 · PLL 切换</text>
<text x="160.0" y="126.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">CTRL_NOC · PCIe 初始化与链路训练</text>
<text x="160.0" y="139.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">ITCM 初始化 · 写固件 · 配全部 10 个 Router</text>
<text x="160.0" y="152.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">解复位 · 收 boot done · 经 PCIe 通知上位机</text>
<rect x="320" y="60" width="280" height="124" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="460.0" y="100.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#be123c" font-weight="600" text-anchor="middle">launch</text>
<text x="460.0" y="113.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">解析上位机下发的 kernel 与配置</text>
<text x="460.0" y="126.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">经 ctrl_noc 写 ITCM / DTCM / Share Mem</text>
<text x="460.0" y="139.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">配 weights 加载模式 · 读 core_id</text>
<text x="460.0" y="152.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">收 loader 完成中断 · 切业务模式 · 通知上位机</text>
<rect x="620" y="60" width="280" height="124" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="760.0" y="100.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#be123c" font-weight="600" text-anchor="middle">运行期</text>
<text x="760.0" y="113.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">状态查询 agent（影子寄存器 · IPI）</text>
<text x="760.0" y="126.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">RAS：软件 / 瞬时 / 永久故障处理</text>
<text x="760.0" y="139.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">GDB server + Debug Agent（翻成 ctrl_noc 操作）</text>
<text x="760.0" y="152.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">热控制 · DVFS · 专家 Context 关闭与创建</text>
<path d="M300.0 122.0 L319.0 122.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<path d="M600.0 122.0 L619.0 122.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<rect x="20" y="230" width="880" height="50" rx="7" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.4"/>
<text x="460.0" y="259.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#16181d" font-weight="600" text-anchor="middle">公共层：OS 内核 · PCIe 与上位机的命令协议 · ctrl_noc 读写 · async_int 中断处理 · 日志与 RAS 记录</text>
<path d="M160.0 184.0 L160.0 229.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#a)"/>
<path d="M460.0 184.0 L460.0 229.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#a)"/>
<path d="M760.0 184.0 L760.0 229.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#a)"/>
</svg>
```

* boot：BOOTROM 起 BL0，读 BL1 验签跳转，切时钟；初始化 CTRL_NOC 与 PCIe；解复位前初始化 ITCM、写固件、配全部 10 个 core 的 Router；解复位后收三个 RV core 的 boot done，经 PCIe 通知上位机
* launch：接收 kernel 镜像与配置数据，经 ctrl_noc 写 ITCM / DTCM / Share Mem；配 weights 加载模式（Router 单 path、TS 指向 weights loader）；读每个 core 的 core_id；收 loader 完成中断后切业务模式并通知上位机
* 运行期：状态查询 agent；RAS 三类故障处理与 RAS 日志；GDB server 与 Debug Agent 把 GDB 语义翻成 ctrl_noc 操作，停机粒度是整个 core；热控制与 DVFS；专家迁移时关闭源 Context、创建目标 Context
* 公共层：完整 OS 内核、PCIe 与上位机的命令协议、ctrl_noc 读写、async_int 中断处理、日志

***

## core 侧固件与 kernel

```svg
<svg viewBox="0 0 920 410" width="920" height="410" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="core 侧固件与 kernel">
<title>core 侧固件与 kernel</title>
<rect width="920" height="410" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">core 侧软件：每个 RV core 一份 firmware，上面按 core 角色装 kernel</text>
<rect x="20" y="60" width="280" height="130" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="160.0" y="96.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">DTE RV core 的 kernel</text>
<text x="160.0" y="109.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">DTE bootloader · weights loader</text>
<text x="160.0" y="122.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">datain：Router → Matrix Mem / Core Mem</text>
<text x="160.0" y="135.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">dataout：Matrix Mem / Core Mem → Router</text>
<text x="160.0" y="148.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">逐级 reduce 的 DTE 任务</text>
<text x="160.0" y="161.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">broadcast / P2P reissue</text>
<rect x="320" y="60" width="280" height="130" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="460.0" y="103.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">MU RV core 的 kernel</text>
<text x="460.0" y="116.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">FC1 / FC3 GEMV（多专家输出）</text>
<text x="460.0" y="129.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">FC2 GEMV（加权合并输出）</text>
<text x="460.0" y="142.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">Norm（DP 场景）</text>
<text x="460.0" y="155.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">R core：check flag（纯标量，不调 DSA）</text>
<rect x="620" y="60" width="280" height="130" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="760.0" y="103.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">VU RV core 的 kernel</text>
<text x="760.0" y="116.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">SiLU · 点乘 · MXFP8 量化</text>
<text x="760.0" y="129.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">core 内 reduce</text>
<text x="760.0" y="142.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">B core：check_flag（纯标量）</text>
<text x="760.0" y="155.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">R core：两笔数据求和</text>
<rect x="20" y="240" width="880" height="70" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="460.0" y="266.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">RV core firmware（BSP，DTE / MU / VU 各一份镜像）</text>
<text x="460.0" y="279.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">CSR · 中断与异常 · CLINT / PLIC · 复位与启动 · PMP · 看门狗 · BSS / 栈 · 主循环与 WFT</text>
<text x="460.0" y="292.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">printf · 故障 dump · 日志导出 · trace 上报；task_done 后回主循环等下一条 task_cmd</text>
<rect x="20" y="350" width="880" height="40" rx="7" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.4"/>
<text x="460.0" y="373.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#16181d" font-weight="600" text-anchor="middle">硬件接口：TS 的 task_cmd / task_done · DSA 寄存器（dsaw / dsawi 写，dsar / dsari 读）· loop 指令 · Router I/O reg · Share Mem</text>
<path d="M160.0 190.0 L160.0 239.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<path d="M460.0 190.0 L460.0 239.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<rect x="382.1" y="204.5" width="155.8" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="460" y="214" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#0d9488" text-anchor="middle">收到 task_cmd 跳到 task_pc</text>
<path d="M760.0 190.0 L760.0 239.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<path d="M460.0 310.0 L460.0 349.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#a)"/>
<text x="460" y="402" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="1" fill="#5c6370" font-weight="400" text-anchor="start"></text>
</svg>
```

* firmware（BSP）每 RV core 一份镜像，DTE / MU / VU 各不同：CSR 读写（芯片标识、硬线程 id、中断使能、定时器）、中断与异常处理、CLINT / PLIC 适配、复位与启动、PMP、看门狗、BSS 清零与栈初始化、主循环与 WFT；printf、故障 dump、日志导出；程序结束时执行一条不通知 TS 的 task_done 等业务 task
* DTE RV core 另装 DTE bootloader 与 weights loader：先用标量指令算出这一片落 Matrix Mem 的地址，再发 DTE 指令搬运，计数满后自定义指令中断 SCP
* task kernel 按 core 角色装配，四类：normal core、B core、R core、DP + P2P reissue core；每类 core 一个 RV32 ELF，配 task_pc 表把 task_chain 每一项指到入口
  * DTE：datain、dataout、逐级 reduce、reissue
  * MU：FC1 / FC3 GEMV、FC2 GEMV、Norm；R core 上只做 check flag
  * VU：SiLU · 点乘 · MXFP8 量化、core 内 reduce、R core 的求和；B core 上只做 check_flag
* 每个 task 的形状相同：TS 配 task_pc 与 stream_id 给 RV core，RV core 发一条异步 DSA 指令后立刻交还，DSA 完成后向 TS 回 Ack；纯标量 task 由 RV core 自己报完成
* 自定义指令：`dsaw / dsawi` 写 DSA 寄存器、`dsar / dsari` 读、`loop` 循环分支、`task_done` 带 TS 标志

***

## 模拟器与工具

```svg
<svg viewBox="0 0 920 250" width="920" height="250" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="模拟器与工具">
<title>模拟器与工具</title>
<rect width="920" height="250" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">模拟器与工具：产品编译器未就位前，用替身产物驱动 latch 模型</text>
<rect x="20" y="60" width="210" height="84" rx="7" fill="#fdf1dc" stroke="#d97706" stroke-width="1.4"/>
<text x="125.0" y="93.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#b45309" font-weight="600" text-anchor="middle">latch 逐拍模型</text>
<text x="125.0" y="106.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">48 chip，每个模块独立打拍</text>
<text x="125.0" y="119.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">读十张只读表</text>
<rect x="270" y="60" width="200" height="84" rx="7" fill="#fdf1dc" stroke="#d97706" stroke-width="1.4"/>
<text x="370.0" y="93.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#b45309" font-weight="600" text-anchor="middle">模型编译器（.bachir）</text>
<text x="370.0" y="106.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">Python 描述 → 中间文件</text>
<text x="370.0" y="119.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">产品编译器的替身</text>
<rect x="510" y="60" width="180" height="84" rx="7" fill="#fdf1dc" stroke="#d97706" stroke-width="1.4"/>
<text x="600.0" y="93.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#b45309" font-weight="600" text-anchor="middle">数值与参考实现</text>
<text x="600.0" y="106.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">编解码 · 舍入 · 累加顺序</text>
<text x="600.0" y="119.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">与模型共用一份实现</text>
<rect x="730" y="60" width="170" height="84" rx="7" fill="#fdf1dc" stroke="#d97706" stroke-width="1.4"/>
<text x="815.0" y="86.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#b45309" font-weight="600" text-anchor="middle">调试与性能工具</text>
<text x="815.0" y="99.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">路由 Visualizer / Verifier</text>
<text x="815.0" y="112.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">System / Kernel Profiler</text>
<text x="815.0" y="125.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">GDB 前端</text>
<path d="M270.0 102.0 L231.0 102.0" stroke="#d97706" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ar)"/>
<rect x="225.4" y="86.5" width="49.2" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="250" y="96" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#d97706" text-anchor="middle">.bachir</text>
<path d="M600.0 144.0 L600.0 180.0 L125.0 180.0 L125.0 145.0" stroke="#d97706" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ar)"/>
<rect x="292.9" y="166.5" width="138.2" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="362" y="176" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#d97706" text-anchor="middle">expect_out，逐 bit 比对</text>
<text x="460" y="232" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="middle">四样的规格见同目录的《Bach 中间文件格式》《模型编译器》《数值与参考实现》，以及《latch 建模计划》。</text>
</svg>
```

* latch 逐拍模型：48 chip，每个模块独立打拍，输入十张只读表；建模范围与顺序在《latch 建模计划》
* 模型编译器（.bachir）：把一个模型在一台机器上怎么跑写成一段 Python，跑出中间文件；在产品编译器就位前当它的替身
* 数值与参考实现：数据格式编码、转换舍入、累加顺序，模型与参考实现共用一份，逐 bit 比对
* 调试与性能工具：路由 Visualizer 与 Verifier（编译器早期就要有）；System Profiler（task 粒度全 trace，类 nsys）与 Kernel Profiler（PMU 加 PC 采样，类 ncu）；GDB 前端接 SCP 上的 GDB server
* Profiling 对硬件的要求：每 core 2～4 MB SRAM 存 trace，per-core PMU 32 个以上可编程 counter，Router、link、DMA、功耗各有 PMU，core 间与 chip 间时间对齐

***

## 产物与接口一览

| 产物 | 谁产出 | 谁消费 | 走哪条路 |
| - | - | - | - |
| RouterTable · Skip Mask · Credit Bypass Route | 编译器 Route Lowering | SCP 写到每 core 的 Router，坏核也写 | ctrl_noc |
| task_chain · datain_task · TS 全局项 | 编译器（算子任务链） | SCP 写到 TS | ctrl_noc |
| kernel 镜像（每类 core 一个 RV32 ELF）与 task_pc 表 | 编译器 CodeGen | SCP 写到 ITCM / DTCM | ctrl_noc |
| RV firmware · DTE bootloader · weights loader | 固件开发 | SCP 在 boot 期写到 ITCM | ctrl_noc |
| weights 分片（每 core 27 MiB） | 编译器 CodeGen | 上位机发出，Router 转发，DTE 搬进 Matrix Mem | PCIe msg 流 |
| model 文件（PPUF） | 编译器打包 | runtime 解析，按 core 拆分 | 网络到 DDR |
| token 包 6368 B · 结果 12 KiB | GPU 侧 · core kernel | Router | PCIe msg 流 |
| boot / launch 完成 · 故障 · 状态 | SCP | 上位机 | PCIe |
| trace · PMU 计数 | core 内 SRAM | slave CPU 的 DDR，再到上位机 | 独立 noc 通道或复用 ctrl_noc，待定 |

***

## path_id 与其他编号

```svg
<svg viewBox="0 0 920 400" width="920" height="400" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="path_id 的一生">
<title>path_id 的一生</title>
<rect width="920" height="400" fill="#ffffff"/>
<defs><marker id="a" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#6b7280"/></marker><marker id="as" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#6b7280"/></marker><marker id="ab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="abs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="ac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="acs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="ar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="ars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="am" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="ams" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker><marker id="ak" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#be123c"/></marker><marker id="aks" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#be123c"/></marker><marker id="an" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="ans" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="8" markerHeight="7" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker></defs>
<text x="460.0" y="24" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">path_id：编译器分配，SCP 填表，之后全靠硬件按它查表</text>
<rect x="20" y="60" width="200" height="70" rx="7" fill="#ede5fd" stroke="#7c3aed" stroke-width="1.4"/>
<text x="120.0" y="86.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#7c3aed" font-weight="600" text-anchor="middle">编译器：分配 path_id</text>
<text x="120.0" y="99.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">路由搜索，一条路一个号</text>
<text x="120.0" y="112.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">写进 operator_routes.toml</text>
<rect x="270" y="60" width="190" height="70" rx="7" fill="#fde4ea" stroke="#be123c" stroke-width="1.4"/>
<text x="365.0" y="86.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#be123c" font-weight="600" text-anchor="middle">SCP：boot 期填表</text>
<text x="365.0" y="99.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">三份 RouterTable 逐项写</text>
<text x="365.0" y="112.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">坏核也写，不置 Core 位</text>
<rect x="510" y="60" width="390" height="70" rx="7" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.4"/>
<text x="705.0" y="86.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#16181d" font-weight="600" text-anchor="middle">每个 core 的 RouterTable[path_id]</text>
<text x="705.0" y="99.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">同一个号在不同 core 的这一行内容不同</text>
<text x="705.0" y="112.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">Router · DTE · ReduceModule 各持一份副本</text>
<path d="M220.0 95.0 L269.0 95.0" stroke="#7c3aed" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#am)"/>
<rect x="225.2" y="78.5" width="39.5" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="245" y="88" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#7c3aed" text-anchor="middle">路由表</text>
<path d="M460.0 95.0 L509.0 95.0" stroke="#be123c" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ak)"/>
<rect x="457.5" y="78.5" width="55.0" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="485" y="88" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#be123c" text-anchor="middle">ctrl_noc</text>
<rect x="20" y="200" width="130" height="80" rx="7" fill="#e4ecfd" stroke="#2563eb" stroke-width="1.4"/>
<text x="85.0" y="231.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#2563eb" font-weight="600" text-anchor="middle">DPU：写包头</text>
<text x="85.0" y="244.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">path_id</text>
<text x="85.0" y="257.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">path_core_mask</text>
<rect x="180" y="200" width="120" height="80" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="240.0" y="231.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="600" text-anchor="middle">边界桥接</text>
<text x="240.0" y="244.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">PCIe 地址 ↔</text>
<text x="240.0" y="257.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">path_id 路由</text>
<rect x="330" y="200" width="170" height="80" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="415.0" y="231.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">Router</text>
<text x="415.0" y="244.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">Header Parser 按它查表</text>
<text x="415.0" y="257.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">分 VC · 选出口 · 进不进核</text>
<rect x="530" y="200" width="170" height="80" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="615.0" y="225.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">TS</text>
<text x="615.0" y="237.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">path_task_map 找到任务</text>
<text x="615.0" y="250.4" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">CreditCounter 按它扣</text>
<text x="615.0" y="262.9" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" text-anchor="middle">资源注册带 PathID</text>
<rect x="730" y="200" width="170" height="80" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="815.0" y="231.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">DTE</text>
<text x="815.0" y="244.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">lut 按 task_id 改写包头</text>
<text x="815.0" y="257.2" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">PendingTaskQ 按它查资源</text>
<rect x="330" y="320" width="170" height="54" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="415.0" y="344.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">ReduceModule</text>
<text x="415.0" y="357.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">reduce_in_mask 判收齐 · 定出口</text>
<rect x="530" y="320" width="170" height="54" rx="7" fill="#dcf3f0" stroke="#0d9488" stroke-width="1.4"/>
<text x="615.0" y="344.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#0d9488" font-weight="600" text-anchor="middle">CoreMem 重发</text>
<text x="615.0" y="357.7" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" text-anchor="middle">只存 UserID · PathID · size</text>
<path d="M150.0 240.0 L179.0 240.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M300.0 240.0 L329.0 240.0" stroke="#6b7280" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="271.7" y="182.5" width="86.5" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="315" y="192" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#6b7280" text-anchor="middle">包头带 path_id</text>
<path d="M500.0 240.0 L529.0 240.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<rect x="479.9" y="184.5" width="70.2" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="515" y="194" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#0d9488" text-anchor="middle">datain 触发</text>
<path d="M700.0 240.0 L729.0 240.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<rect x="690.4" y="184.5" width="49.2" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="715" y="194" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#0d9488" text-anchor="middle">task_pc</text>
<path d="M781.0 200.0 L781.0 165.0 L432.0 165.0 L432.0 199.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<rect x="540.0" y="151.5" width="131.9" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="606" y="161" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#0d9488" text-anchor="middle">出核包，包头由 DTE 改写</text>
<path d="M381.0 280.0 L381.0 319.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<rect x="359.2" y="290.5" width="59.7" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="389" y="300" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#0d9488" text-anchor="middle">reduce 包</text>
<path d="M466.0 280.0 L466.0 300.0 L581.0 300.0 L581.0 319.0" stroke="#0d9488" stroke-width="1.5" fill="none" stroke-linejoin="round" marker-end="url(#ac)"/>
<rect x="567.8" y="286.5" width="144.5" height="13.5" rx="3" fill="#ffffff" opacity="0.95"/>
<text x="640" y="296" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#0d9488" text-anchor="middle">资源不够转存，重发时重查表</text>
<text x="460" y="392" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="middle">包里没有目的地址，硬件全靠 path_id 查表；软件只在两头碰它：编译器分配和填表，DTE 的 RV core 改包头。</text>
</svg>
```

* path_id 是数据面唯一的路由键：包里没有目的地址，硬件全靠它查表。编号本身 8 bit 用 6 bit，不编码任何含义
* 谁分配：编译器路由搜索时，一条 P2P、一棵广播树、一棵归约树各占一个；weights 加载专用 1 条；写进 `operator_routes.toml`。两个通信事务满足任一条就要换新号：R2R 路径有重叠但后续传播的节点不完全一致；在同一个 core 上做的操作不同
* EP 广播用 path_id 配包头里 16 bit 的 path_core_mask：K 条 path_id 配 2^K 种 mask，每个 core 在自己的表项里用 `path_core_mask_idx` 指定看哪一位；按 dp10 / ep16 / pp3 试算共 55 条，64 项的表装得下
* 它背后的信息在两侧
  * 硬件侧是每个 core 的 RouterTable 里 path_id 那一行，同一个号在不同 core 内容不同：去向（`flow_dir`、`op_type`、`operation`）、VC（`cur_vc`、`nxt_vc`）、进不进核（`path_core_bypass` 或 mask 位）、流控（`stream_table_enable`、进核与各出方向的 credit 池与额度）、缓存与阻塞（`need_buffer`、`stall_way`）、归约（`reduce_in_mask`、输入输出精度）。三份副本，SCP 先写 Router，commit 完成后再写 DTE 与 ReduceModule 的
  * 软件侧是编译器的一条路由请求：`kind`、`weight`、`sources`、`destinations`、`paired_path_id`、`is_synthetic`、`storage_destination`、`allowed_ep_merge_chips`；普通 reduce 自动拆成归约到 merge core 加 P2P 到存放 core 两条，后一条取 10000 加原号；求解后落成每个 core 的 `recv_mask / send_mask / local_inject / local_consume / enter_internal`
* 硬件里谁用它
  * 入口：DPU 把编译器定好的 path_id 写进包头；chip 边界的桥接模块在 PCIe 域地址与 path_id 之间转换
  * Router：Header Parser 按它查表，RouterStation 按它分 VC，Xbar 按它选出口；C2C Bridge 里的简化 Router 同一套
  * ReduceModule：按它查副本，拿 `reduce_in_mask` 判收齐，拿输出方向、VC 与精度发结果
  * CoreMem 重发：转存只记 UserID、PathID、size，重发时重查表
  * TS：Router 的 datain 触发带 path_id，`path_task_map[path_id]` 找到任务链哪一项；`CreditCounter[path_id][stream_id]` 扣 credit；向 Router 注册资源带 UserID、StreamID、TaskID、PathID；写 `TS_INIT_FINISH` 时查任务链里每个 path_id 在表里有有效项
  * DTE：出核按 task_id 查 `lut` 得 `{path_id, size}` 改写包头；配好出核任务后先按 path_id 查资源，要 Stream 或 Reduce 资源的进 PendingTaskQ 等授权。DTE 只能改 path_id 与长度，改不了 path_core_mask
  * SCP：boot 期逐项写三份表，坏核也写，坏核表项不置 Core 位，只按位置转发
* 其他编号分三层记

| 层 | 编号 | 谁分配 | 作用域 | 用在哪 |
| - | - | - | - | - |
| 全局身份 | user_id | 入口 DPU 按到达顺序编 | 全局，跟着包走 | Router 建 stream 表、三类 credit 记账、TS User_Match 认新老用户 |
| 全局身份 | gpu_id · token_id | DPU 加包头 | 全局 | 结果发回哪个 GPU；token_id 每 GPU 自增；R core 按这两个定位 Matrix Mem |
| 全局身份 | expert_id | 模型 | 全局专家号 | topK 表每项 {expert_id, weight}，MU 经 local_ep_table 换成本地序号 |
| core 内身份 | stream_id | TS，新用户建表时占一个槽 | core 内 1～16 | 算该用户的 Core Mem 与 Share Mem 分区基址；DTE 文档叫 SlotID |
| core 内身份 | local_user_id | core 内软件自己编 | core 内 | R core 的用户映射表与 Matrix Mem 地址；与 user_id 无换算关系 |
| core 内身份 | task_id | 编译器排任务链时定 | 一条 task_chain 内，最多 64 | TS 推进任务、done_bitmap 对位、完成回报、DTE 的 lut 索引 |
| core 内身份 | task_pack_id | 软件拆任务包时编 | 一个 DTE 任务内 | 包之间不保证顺序 |
| 路由与链路 | path_id | 编译器 | 全局，每 core 一行表项 | 本节 |
| 路由与链路 | vc_id | 路由表给下一跳 | 每条 link | 进哪个 VC buffer，credit 归还按它 |
| 路由与链路 | core_id | 硬件固定，只读 | chip 内 0～9，row-major | weights 往哪落；编译器按物理号配路由表 |
| 路由与链路 | 逻辑 core 号 | 编译器映射 | chip 内 0～7，8 是 EP 专用核 | kernel 里只见逻辑号 |
| 路由与链路 | chip 坐标 | 拓扑文件 | (rack, chip_y, chip_x) | harvest mask、路由搜索；CTRL_NOC 另有 5 bit chip id 跨 chip 配置 |
| 路由与链路 | seq_id | C2C Bridge 拆包时加 | 一个跨 chip 的包内，4 bit | 收端按它拼回原包 |

***

## 开发顺序

编译器有源文档给出的六阶段顺序；其余按依赖排，是本文的建议：

1. 先行：编译器阶段 1（拓扑、输出格式、Verifier、Visualizer）与 firmware BSP。boot 要靠固件，launch 要靠路由表格式
2. 单 chip 打通：SCP boot → 固件 → P2P 路由表（阶段 2）→ weights loader → launch 切业务模式
3. 业务链：阶段 3、4 的 broadcast 与 reduce → 四类 core 的 kernel → EP6+TP8 完整任务链 → 阶段 5 的 EP special core
4. 上位机：runtime 加载 → AF 分离框架与 HBU ↔ GPU 通信 → 流控与退出 → 动态专家调度
5. 工具：Visualizer 随阶段 1；profiler 与 GDB 随硬件的 PMU 与 Debug Module 定型

***

## 待定

* slave CPU 还是 DPU 承载协议转换、数据中转、初始化、profiling、trace、热点重排；成本与人力评估中，DPU 做不了 PCIe RC 则用低配 slave CPU 加开源驱动
* 静态路由与坏核绑定：坏核组合成千上万，无法全量预编译；候选是按 chip 型（A / B / C）分步搜索，536 种组合预编译路由表，kernel 与 weights 各共享一份，上电读 eFuse 后选表
* runtime 用地址路由还是路由表路由加载 kernel；kernel 是否也支持经 msg 流搬运；ITCM 溢出由 SCP 重搬还是 DTE 重搬
* 专家激活统计放 kernel 里还是 slave CPU 上
* trace 搬运通道：独立 noc 通道还是复用 SCP 配置通道；trace 上报的通信机制
* GDB 是否一定经 SCP，还是上位机也可经 PCIe 直接调试
* RV core 与 TS 的交互细节（固件需求里标待定）
* PCIe Switch 组播、跨 chip P2P、双向传输是否可用，都是可选优化，由硬件评估

***

## 取舍

* **kernel 裸机、PIC、无 main，结束后等下一次调度**：省掉 OS，加载地址自由，一个 kernel 一个二进制
* **全部产物打成一个 model 文件**：kernel 与 weights 都和 core 一一映射，不打包加载前会很分散；metadata 索引、mmap 加载
* **编译器不做图优化**：算子是 fuse 后手写实现，价值在映射与路由搜索
* **kernel 走 SCP，weights 走 msg 流**：kernel 重复、KB 级；weights 每 core 不同、MB 级，配置总线带宽不够
* **第一版低配 slave CPU 主控、数据先进 DDR**：先打通与 GPU 的通信链路，性能优化再往 DPU 迁
* **静态路由换硬件简单**：代价是换卡或换排布要重编译，用分步预编译缓解
* **专家重排放 CPU**：LPU 内部做不了统计与重排；重排期间所有 Bach 节点同时更新 weights，更新完再重启服务
