# 归约的完整过程

**模式**：design（陈述当前设计，动机与取舍收在文末“取舍”一节）

给要实现或建模归约的人：一个 token 的 FFN 结果在 Bach 里要经过哪几层归约、每一层由谁做加法、数据怎么走、credit 怎么控、完成怎么报、卡住时怎么解。把散在《系统与部署》《Router 片上交换与归约》《TS 任务调度器》《软件栈》里的归约片段，按一个 token 的先后顺序串成一份。

* 三层归约各自落在哪个硬件上：《Core 内硬件》《执行单元与存储》
* Router 里 Reduce 包的准入、仲裁与三类 credit 的通则：《Router 片上交换与归约》
* R core 两条任务链逐 task 的软件流程：《软件栈》

内容来自五份源文档，口径不一致处见“口径与待定”：

* `04_四、MAS/04_Router.md`（Reduce 操作、ReduceModule）
* `04_四、MAS/10_Matrix Unit DSA.md`（MU 的专家间累加形式）
* `Bach项目文档/02_Architecture/03_HAS/02_DATA_NOC_DE_HAS.md`（Rmem 与 reduce credit 网）
* `04_第二阶段/02_EP组间Reduction讨论（过程）.md`、`01_MoE模型部署边界讨论（阶段总结）.md`（EP 组间 reduction 链、死锁与派遣）
* `06_第四阶段/04_core内调度机制.md`、`Bach软件文档库/04_总体设计/02_软件流程梳理.md`（R core 的两条任务链）

《Bach 硬件设计建模参考》归约专题，全套目录见 [README](README.md)。

***

## 归约发生在三层

一个 token 的 FFN 结果在三个层次上各做一次加法，三层做加法的地方、触发方式、等待方式和流控都不同。

```svg
<svg viewBox="0 0 1100 486" width="1100" height="486" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="归约发生在三层：core 内、chip 内、EP 组间">
<title>归约发生在三层：core 内、chip 内、EP 组间</title>
<rect width="1100" height="486" fill="#ffffff"/>
<defs><marker id="raa" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="raas" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker><marker id="raai" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#3f4451"/></marker><marker id="raais" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#3f4451"/></marker><marker id="raab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="raabs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="raar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="raars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="raac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="raacs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="raap" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="raaps" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker></defs>
<text x="30" y="26" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="start">归约发生在三层：core 内、chip 内、EP 组间</text>
<text x="30" y="46" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="start">一个 token 的 FFN 结果要过三层归约才成为最终值；三层做加法的地方、触发方式和等待方式各不相同</text>
<rect x="30" y="62" width="330" height="330" rx="8" fill="#ffffff" stroke="#0d9488" stroke-width="1.4"/>
<rect x="30" y="62" width="330" height="26" rx="8" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.2"/>
<text x="195.0" y="80" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#0d9488" font-weight="700" text-anchor="middle">第一层　core 内：专家间加权求和</text>
<rect x="380" y="62" width="340" height="330" rx="8" fill="#ffffff" stroke="#2563eb" stroke-width="1.4"/>
<rect x="380" y="62" width="340" height="26" rx="8" fill="#dbeafe" stroke="#2563eb" stroke-width="1.2"/>
<text x="550.0" y="80" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#2563eb" font-weight="700" text-anchor="middle">第二层　chip 内：切 K 部分和的逐跳累加</text>
<rect x="740" y="62" width="330" height="330" rx="8" fill="#ffffff" stroke="#7c3aed" stroke-width="1.4"/>
<rect x="740" y="62" width="330" height="26" rx="8" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.2"/>
<text x="905.0" y="80" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#7c3aed" font-weight="700" text-anchor="middle">第三层　EP 组间：沿 R core 链累加</text>
<rect x="50" y="104" width="290" height="74" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="58" y="119" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#0d9488" font-weight="700" text-anchor="start">MU DSA</text>
<text x="58" y="133" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">C = C + (A × B) × W_ep</text>
<text x="58" y="145" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">对本 core 上每个激活的专家：token × 该专家的 FC2 分片，</text>
<text x="58" y="157" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">乘专家权重 W_ep 后累加进 C；不支持初始 C 加载</text>
<text x="195" y="200" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">一个 token 命中 top-8 个专家，落在本 EP 组的那几个</text>
<text x="195" y="214" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">在同一个 core 上逐个算、逐个累加，不出 core</text>
<rect x="50" y="230" width="290" height="58" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="58" y="245" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#16181d" font-weight="700" text-anchor="start">VU DSA（备用路径）</text>
<text x="58" y="259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">VALU1 的跨元素归约原语：求和 / 最大 / 最小</text>
<text x="58" y="271" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">tp-reduce / ep-reduce 也可在 VU 做，代价是 Core Mem 容量与带宽</text>
<text x="195" y="312" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#0d9488" font-weight="600" text-anchor="middle">触发：任务链里的一个 MU task</text>
<text x="195" y="328" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#0d9488" font-weight="600" text-anchor="middle">数据在哪等：Core Mem</text>
<text x="195" y="344" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#0d9488" font-weight="600" text-anchor="middle">流控：无，core 内部</text>
<text x="195" y="372" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">建模默认走 MU 这条路径</text>
<rect x="408" y="110" width="60" height="40" rx="4" fill="#fffbeb" stroke="#d97706" stroke-width="1.1"/>
<text x="438" y="126" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">core 0</text>
<text x="438" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="400" text-anchor="middle">部分和 p0</text>
<rect x="408" y="160" width="60" height="26" rx="4" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.0"/>
<text x="438" y="177" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#7c3aed" font-weight="600" text-anchor="middle">Router</text>
<path d="M438 150 L438 159.3" stroke="#2563eb" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#raab)"/>
<path d="M468 173 L485.3 173" stroke="#2563eb" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#raab)"/>
<rect x="486" y="110" width="60" height="40" rx="4" fill="#fffbeb" stroke="#d97706" stroke-width="1.1"/>
<text x="516" y="126" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">core 1</text>
<text x="516" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="400" text-anchor="middle">部分和 p1</text>
<rect x="486" y="160" width="60" height="26" rx="4" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.0"/>
<text x="516" y="177" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#7c3aed" font-weight="600" text-anchor="middle">Router</text>
<path d="M516 150 L516 159.3" stroke="#2563eb" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#raab)"/>
<path d="M546 173 L563.3 173" stroke="#2563eb" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#raab)"/>
<rect x="564" y="110" width="60" height="40" rx="4" fill="#fffbeb" stroke="#d97706" stroke-width="1.1"/>
<text x="594" y="126" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">core 2</text>
<text x="594" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="400" text-anchor="middle">部分和 p2</text>
<rect x="564" y="160" width="60" height="26" rx="4" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.0"/>
<text x="594" y="177" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#7c3aed" font-weight="600" text-anchor="middle">Router</text>
<path d="M594 150 L594 159.3" stroke="#2563eb" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#raab)"/>
<path d="M624 173 L641.3 173" stroke="#2563eb" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#raab)"/>
<rect x="642" y="110" width="60" height="40" rx="4" fill="#fffbeb" stroke="#d97706" stroke-width="1.1"/>
<text x="672" y="126" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="middle">core 3</text>
<text x="672" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="400" text-anchor="middle">部分和 p3</text>
<rect x="642" y="160" width="60" height="26" rx="4" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.0"/>
<text x="672" y="177" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#7c3aed" font-weight="600" text-anchor="middle">Router</text>
<path d="M672 150 L672 159.3" stroke="#2563eb" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#raab)"/>
<text x="550" y="206" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">每个 core 拿 K 维的一片权重，算出完整长度的部分和</text>
<text x="550" y="220" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">p0 → core1 的 ReduceModule 与 p1 原位相加 → core2 …</text>
<text x="550" y="234" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">流着加，不等齐；中间累加精度 FP32</text>
<rect x="400" y="248" width="300" height="46" rx="5" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.3"/>
<text x="408" y="263" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#7c3aed" font-weight="700" text-anchor="start">Router 的 ReduceModule</text>
<text x="408" y="277" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">operation = Reduce 的包在这里做 Read-Modify-Write，</text>
<text x="408" y="289" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="start">结果回注 CrossBar 送下一跳；最后一跳落 core 或送下一 chip</text>
<text x="550" y="312" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="600" text-anchor="middle">触发：DTE 出核 Reduce 包，Router 查表</text>
<text x="550" y="328" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="600" text-anchor="middle">数据在哪等：ReduceModule 上下文 16 × 16 KiB</text>
<text x="550" y="344" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="600" text-anchor="middle">流控：Reduce credit（DTE 本级、ReduceModule 下游）</text>
<text x="550" y="372" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">切 N 的结果不归约，concat 即可</text>
<rect x="760" y="104" width="200" height="32" rx="4" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.0"/>
<text x="830" y="124" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="middle">EP 0：8 chip TP</text>
<rect x="970" y="104" width="70" height="32" rx="4" fill="#fee2e2" stroke="#dc2626" stroke-width="1.1"/>
<text x="1005" y="124" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#dc2626" font-weight="700" text-anchor="middle">R core</text>
<path d="M960 120 L969.3 120" stroke="#2563eb" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#raab)"/>
<path d="M1005 136 L1005 147.3" stroke="#7c3aed" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#raap)"/>
<rect x="760" y="148" width="200" height="32" rx="4" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.0"/>
<text x="830" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="middle">EP 1：8 chip TP</text>
<rect x="970" y="148" width="70" height="32" rx="4" fill="#fee2e2" stroke="#dc2626" stroke-width="1.1"/>
<text x="1005" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#dc2626" font-weight="700" text-anchor="middle">R core</text>
<path d="M960 164 L969.3 164" stroke="#2563eb" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#raab)"/>
<path d="M1005 180 L1005 191.3" stroke="#7c3aed" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#raap)"/>
<rect x="760" y="192" width="200" height="32" rx="4" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.0"/>
<text x="830" y="212" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="middle">EP 2：8 chip TP</text>
<rect x="970" y="192" width="70" height="32" rx="4" fill="#fee2e2" stroke="#dc2626" stroke-width="1.1"/>
<text x="1005" y="212" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#dc2626" font-weight="700" text-anchor="middle">R core</text>
<path d="M960 208 L969.3 208" stroke="#2563eb" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#raab)"/>
<path d="M1005 224 L1005 235.3" stroke="#7c3aed" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#raap)"/>
<rect x="760" y="236" width="200" height="32" rx="4" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.0"/>
<text x="830" y="256" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="middle">EP 3：8 chip TP</text>
<rect x="970" y="236" width="70" height="32" rx="4" fill="#fee2e2" stroke="#dc2626" stroke-width="1.1"/>
<text x="1005" y="256" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#dc2626" font-weight="700" text-anchor="middle">R core</text>
<path d="M960 252 L969.3 252" stroke="#2563eb" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#raab)"/>
<text x="905" y="292" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">…… 到链尾那组才出核</text>
<text x="905" y="312" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#7c3aed" font-weight="600" text-anchor="middle">触发：Router 收到 reduce / reduction 数据通知 TS</text>
<text x="905" y="328" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#7c3aed" font-weight="600" text-anchor="middle">数据在哪等：R core 的 Matrix Mem 32 MB，约 1300 用户</text>
<text x="905" y="344" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#7c3aed" font-weight="600" text-anchor="middle">流控：派遣前查所有 R core 的余量</text>
<text x="905" y="372" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">等两笔齐了再加，用户之间乱序</text>
<rect x="30" y="410" width="1040" height="62" rx="5" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.2"/>
<text x="46" y="427" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">第一层在 core 内算完就地；第二层在 Router 里流着加，包经过就加，每一跳都不等；第三层在 R core 里等两笔到齐再加。三层各自的 credit：第一层没有，第二层是 Reduce credit，第三层是派遣时的 R core 余量。</text>
<text x="46" y="443" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">第二层与第三层不共用机制：EP 组间若也走 Router 逐跳累加，ReduceModule 的 16 × 16 KiB 上下文盖不住 EP 之间的不均衡，任务少的组会被频繁反压，所以第三层借一个 core 的 32 MB Matrix Mem 做缓冲。</text>
<text x="46" y="459" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">第二层在哪些算子上出现由切分模式定：tp_nn 只有 FC2；tp_nk 与 pptp 的 FC1 / FC3 也要（chip 内切 K），FC2 反而是 concat。</text>
</svg>
```

| 层 | 加什么 | 谁做加法 | 什么时候加 | 数据在哪等 | 流控 |
| - | - | - | - | - | - |
| 第一层　core 内 | 本 core 上几个激活专家的 FC2 结果，乘各自的专家权重 | MU DSA（`C = C + (A × B) × W_ep`） | 算一个专家加一次，就地 | Core Mem | 无 |
| 第二层　chip 内 core 间 | 切 K 后各 core 的部分和 | Router 的 ReduceModule，Read-Modify-Write | 包流经每一跳就加，不等齐 | ReduceModule 上下文，16 用户 × 16 KiB | Reduce credit |
| 第三层　EP 组间 | 各 EP 组的组内结果 | R core 的 VU | 一个用户的两笔到齐才加 | R core 的 Matrix Mem，32 MB | 派遣前预留所有 R core 的余量 |

三层不共用机制。EP 组间若也走 Router 逐跳累加，ReduceModule 的上下文盖不住 EP 之间的不均衡，任务少的组会被频繁反压，所以第三层借一个 core 的 Matrix Mem 做缓冲，等齐再加。

***

## 第一层：core 内的专家间求和

一个 token 命中 top-8 个专家，其中落在本 EP 组的那几个在同一个 core 上逐个算。MU 的第二种运算形式把专家间的累加并进矩阵乘：

```
C = C + (A × B) × W_ep
```

* `A` 是 token，`B` 是该专家的 FC2 分片，`W_ep` 是这个专家的权重，FP32 输入
* 不支持初始 `C` 加载：第一个专家的结果直接写 `C`，后续专家逐个累加
* 输出 FP32 或 BF16

这条路径把 EP reduce 从 VU 挪进 MU，Core Mem 不必为每个用户缓存所有激活专家的中间结果。建模时 MoE 多专家场景默认走它。VU 保留跨元素归约原语（VALU1 的求和 / 最大 / 最小），是备用路径。

在任务链里，第一层要么是 FC2 的那个 MU task 本身，要么是紧跟其后的一个 VU task（《软件栈》里 normal core 的示例链写作 task 5 “Core 内 reduce”）。第一层的输出是本 core 对这个 token 的一份完整长度或一段的结果，交给第二层。

***

## 第二层：chip 内 core 间的逐跳累加

### 切 K 才需要归约

一次 GEMV `y[1, N] = x[1, K] × W[K, N]`，权重按 N 切，每个 core 算出 `y` 的一段，各段首尾相接就是结果，不需要加法；按 K 切，每个 core 算出的是完整长度的部分和，必须逐元素相加。第二层只出现在切 K 的那些算子上：

| 模式 | FC1 / FC3 | FC2 | chip 内第二层出现在 | chip 间 |
| - | - | - | - | - |
| `tp_nn` | chip 间切 N，chip 内切 N | chip 间切 K，chip 内切 K | FC2 逐级 reduce | FC2 全局 reduce |
| `tp_nk` | chip 间切 N，chip 内切 K | chip 间切 K，chip 内切 N | FC1 / FC3 逐级 reduce 汇聚到一个 core 做 dot 再广播；FC2 concat | FC2 reduce |
| `pptp_nk` | 同 `tp_nk`，FC1、FC3 在不同行 chip | 同上 | FC1 / FC3 逐级 reduce；FC2 concat | FC2 reduce |
| `pptp_nn` | chip 间切 N，chip 内切 N | chip 间切 K，chip 内切 K | FC1 / FC3 concat；FC2 reduce | FC2 reduce |

chip 间这一级 FC2 固定切 K，整条 FFN 只做一次 chip 间 reduce：FC1 / FC3 的 N 与 FC2 的 K 是同一个维度，中间的 SiLU · dot · 量化是逐元素运算，每个 chip 手里的三样东西自然对齐，不用中途汇聚再广播。

### 一个 Reduce 包怎么走

```svg
<svg viewBox="0 0 1200 616" width="1200" height="616" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="chip 内的逐跳累加：一个 Reduce 包从 DTE 出核到落在最后一跳">
<title>chip 内的逐跳累加：一个 Reduce 包从 DTE 出核到落在最后一跳</title>
<rect width="1200" height="616" fill="#ffffff"/>
<defs><marker id="rba" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="rbas" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker><marker id="rbai" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#3f4451"/></marker><marker id="rbais" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#3f4451"/></marker><marker id="rbab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="rbabs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="rbar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="rbars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="rbac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="rbacs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="rbap" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="rbaps" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker></defs>
<text x="30" y="26" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="start">chip 内的逐跳累加：一个 Reduce 包从 DTE 出核到落在最后一跳</text>
<text x="30" y="46" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="start">以 4 个 core 切 K 为例。每个 core 的 DTE 把自己的部分和当 Reduce 包发出，Router 按 RouterTable 的 operation 把它导进 ReduceModule，与已到的上下文相加后送下一跳</text>
<rect x="40" y="70" width="240" height="300" rx="8" fill="#fbfcfd" stroke="#9aa1ad" stroke-width="1.2"/>
<text x="160" y="88" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#16181d" font-weight="700" text-anchor="middle">core 0</text>
<rect x="54" y="100" width="100" height="46" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="62" y="115" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#0d9488" font-weight="700" text-anchor="start">MU / VU</text>
<text x="62" y="129" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">算出部分和</text>
<text x="62" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">p0（完整长度）</text>
<rect x="166" y="100" width="100" height="46" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="174" y="115" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#0d9488" font-weight="700" text-anchor="start">DataOut DTE</text>
<text x="174" y="129" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">查表：Reduce · VC</text>
<text x="174" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">查本级 Reduce credit</text>
<path d="M154 123 L165.3 123" stroke="#16181d" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbai)"/>
<rect x="54" y="170" width="212" height="180" rx="6" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.2"/>
<text x="160" y="186" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#7c3aed" font-weight="700" text-anchor="middle">Router</text>
<rect x="64" y="196" width="92" height="48" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="72" y="211" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">RouterTable</text>
<text x="72" y="225" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">operation=Reduce</text>
<text x="72" y="237" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">方向 · nxt_vc · 精度</text>
<rect x="164" y="196" width="92" height="48" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="172" y="211" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">CrossBar</text>
<text x="172" y="225" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">Data 进 Reduce</text>
<text x="172" y="237" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">结果回注</text>
<rect x="64" y="254" width="192" height="88" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="72" y="269" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="start">ReduceModule</text>
<text x="72" y="283" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">上下文 [UserID]：FP32</text>
<text x="72" y="295" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">首份输入：建上下文写入</text>
<text x="72" y="307" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">全部输入到齐 → 出队 → 查 VC credit</text>
<text x="72" y="319" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">+ 下游 Reduce credit → 发下一跳</text>
<path d="M216 146 L216 195.3" stroke="#2563eb" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbab)"/>
<text x="226" y="170" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#2563eb" font-weight="400" text-anchor="start">出核 Reduce 包</text>
<path d="M210 244 L210 253.3" stroke="#2563eb" stroke-width="1.8" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbab)"/>
<path d="M256 300 L353.3 300" stroke="#2563eb" stroke-width="2.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbab)"/>
<text x="305.0" y="292" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#2563eb" font-weight="600" text-anchor="middle">p0+…+p0</text>
<rect x="330" y="70" width="240" height="300" rx="8" fill="#fbfcfd" stroke="#9aa1ad" stroke-width="1.2"/>
<text x="450" y="88" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#16181d" font-weight="700" text-anchor="middle">core 1</text>
<rect x="344" y="100" width="100" height="46" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="352" y="115" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#0d9488" font-weight="700" text-anchor="start">MU / VU</text>
<text x="352" y="129" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">算出部分和</text>
<text x="352" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">p1（完整长度）</text>
<rect x="456" y="100" width="100" height="46" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="464" y="115" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#0d9488" font-weight="700" text-anchor="start">DataOut DTE</text>
<text x="464" y="129" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">查表：Reduce · VC</text>
<text x="464" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">查本级 Reduce credit</text>
<path d="M444 123 L455.3 123" stroke="#16181d" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbai)"/>
<rect x="344" y="170" width="212" height="180" rx="6" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.2"/>
<text x="450" y="186" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#7c3aed" font-weight="700" text-anchor="middle">Router</text>
<rect x="354" y="196" width="92" height="48" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="362" y="211" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">RouterTable</text>
<text x="362" y="225" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">operation=Reduce</text>
<text x="362" y="237" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">方向 · nxt_vc · 精度</text>
<rect x="454" y="196" width="92" height="48" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="462" y="211" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">CrossBar</text>
<text x="462" y="225" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">Data 进 Reduce</text>
<text x="462" y="237" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">结果回注</text>
<rect x="354" y="254" width="192" height="88" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="362" y="269" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="start">ReduceModule</text>
<text x="362" y="283" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">上下文 [UserID]：FP32</text>
<text x="362" y="295" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">到达的 p0+…+p0 与本地 p1 相加</text>
<text x="362" y="307" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">全部输入到齐 → 出队 → 查 VC credit</text>
<text x="362" y="319" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">+ 下游 Reduce credit → 发下一跳</text>
<path d="M506 146 L506 195.3" stroke="#2563eb" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbab)"/>
<text x="516" y="170" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#2563eb" font-weight="400" text-anchor="start">出核 Reduce 包</text>
<path d="M500 244 L500 253.3" stroke="#2563eb" stroke-width="1.8" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbab)"/>
<path d="M546 300 L643.3 300" stroke="#2563eb" stroke-width="2.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbab)"/>
<text x="595.0" y="292" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#2563eb" font-weight="600" text-anchor="middle">p0+…+p1</text>
<rect x="620" y="70" width="240" height="300" rx="8" fill="#fbfcfd" stroke="#9aa1ad" stroke-width="1.2"/>
<text x="740" y="88" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#16181d" font-weight="700" text-anchor="middle">core 2</text>
<rect x="634" y="100" width="100" height="46" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="642" y="115" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#0d9488" font-weight="700" text-anchor="start">MU / VU</text>
<text x="642" y="129" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">算出部分和</text>
<text x="642" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">p2（完整长度）</text>
<rect x="746" y="100" width="100" height="46" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="754" y="115" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#0d9488" font-weight="700" text-anchor="start">DataOut DTE</text>
<text x="754" y="129" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">查表：Reduce · VC</text>
<text x="754" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">查本级 Reduce credit</text>
<path d="M734 123 L745.3 123" stroke="#16181d" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbai)"/>
<rect x="634" y="170" width="212" height="180" rx="6" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.2"/>
<text x="740" y="186" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#7c3aed" font-weight="700" text-anchor="middle">Router</text>
<rect x="644" y="196" width="92" height="48" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="652" y="211" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">RouterTable</text>
<text x="652" y="225" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">operation=Reduce</text>
<text x="652" y="237" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">方向 · nxt_vc · 精度</text>
<rect x="744" y="196" width="92" height="48" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="752" y="211" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">CrossBar</text>
<text x="752" y="225" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">Data 进 Reduce</text>
<text x="752" y="237" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">结果回注</text>
<rect x="644" y="254" width="192" height="88" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="652" y="269" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="start">ReduceModule</text>
<text x="652" y="283" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">上下文 [UserID]：FP32</text>
<text x="652" y="295" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">到达的 p0+…+p1 与本地 p2 相加</text>
<text x="652" y="307" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">全部输入到齐 → 出队 → 查 VC credit</text>
<text x="652" y="319" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">+ 下游 Reduce credit → 发下一跳</text>
<path d="M796 146 L796 195.3" stroke="#2563eb" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbab)"/>
<text x="806" y="170" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#2563eb" font-weight="400" text-anchor="start">出核 Reduce 包</text>
<path d="M790 244 L790 253.3" stroke="#2563eb" stroke-width="1.8" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbab)"/>
<path d="M836 300 L933.3 300" stroke="#2563eb" stroke-width="2.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbab)"/>
<text x="885.0" y="292" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#2563eb" font-weight="600" text-anchor="middle">p0+…+p2</text>
<rect x="910" y="70" width="240" height="300" rx="8" fill="#fbfcfd" stroke="#9aa1ad" stroke-width="1.2"/>
<text x="1030" y="88" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#16181d" font-weight="700" text-anchor="middle">core 3</text>
<rect x="924" y="100" width="100" height="46" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="932" y="115" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#0d9488" font-weight="700" text-anchor="start">MU / VU</text>
<text x="932" y="129" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">算出部分和</text>
<text x="932" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">p3（完整长度）</text>
<rect x="1036" y="100" width="100" height="46" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="1044" y="115" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#0d9488" font-weight="700" text-anchor="start">DataOut DTE</text>
<text x="1044" y="129" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">查表：Reduce · VC</text>
<text x="1044" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">查本级 Reduce credit</text>
<path d="M1024 123 L1035.3 123" stroke="#16181d" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbai)"/>
<rect x="924" y="170" width="212" height="180" rx="6" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.2"/>
<text x="1030" y="186" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#7c3aed" font-weight="700" text-anchor="middle">Router</text>
<rect x="934" y="196" width="92" height="48" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="942" y="211" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">RouterTable</text>
<text x="942" y="225" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">operation=Reduce</text>
<text x="942" y="237" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">方向 · nxt_vc · 精度</text>
<rect x="1034" y="196" width="92" height="48" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="1042" y="211" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="700" text-anchor="start">CrossBar</text>
<text x="1042" y="225" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">Data 进 Reduce</text>
<text x="1042" y="237" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">结果回注</text>
<rect x="934" y="254" width="192" height="88" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="942" y="269" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#16181d" font-weight="700" text-anchor="start">ReduceModule</text>
<text x="942" y="283" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">上下文 [UserID]：FP32</text>
<text x="942" y="295" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">到达的 p0+…+p2 与本地 p3 相加</text>
<text x="942" y="307" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">全部输入到齐 → 出队 → 查 VC credit</text>
<text x="942" y="319" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">+ 下游 Reduce credit → 发下一跳</text>
<path d="M1086 146 L1086 195.3" stroke="#2563eb" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbab)"/>
<text x="1096" y="170" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#2563eb" font-weight="400" text-anchor="start">出核 Reduce 包</text>
<path d="M1080 244 L1080 253.3" stroke="#2563eb" stroke-width="1.8" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbab)"/>
<path d="M1126 300 L1170 300 L1170 420 L700.7 420" stroke="#2563eb" stroke-width="2.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbab)"/>
<rect x="480" y="400" width="220" height="40" rx="5" fill="#dbeafe" stroke="#2563eb" stroke-width="1.3"/>
<text x="488" y="415" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#2563eb" font-weight="700" text-anchor="start">最后一跳的去向由 RouterTable 定</text>
<text x="488" y="429" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">进本 core（Core Mem）或继续送下一 chip / R core</text>
<path d="M120 340 L120 460 L479.3 460" stroke="#d97706" stroke-width="1.4" fill="none" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rbar)"/>
<rect x="480" y="446" width="300" height="40" rx="5" fill="#fdeed8" stroke="#d97706" stroke-width="1.3"/>
<text x="488" y="461" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#d97706" font-weight="700" text-anchor="start">完成信号</text>
<text x="488" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">ReduceModule 整包发出后向 core 返回 UserID（rmem2ts_done_ch）；只有它能把 reduce task 置 FINISH</text>
<rect x="30" y="500" width="1140" height="78" rx="5" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.2"/>
<text x="46" y="517" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">顺序：每一跳先查 RouterTable 得到 operation / 方向 / 下一跳 VC / 精度 → 包进 ReduceModule 锁定到尾 flit → 首份建上下文、后续 RMW →</text>
<text x="46" y="533" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">到齐后查目标 VC credit 与下游 Reduce credit → 发出 → 逐 flit 给上游 release。跳间是 R2R 通路，flit 可交织。</text>
<text x="46" y="549" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">为什么流着加：ReduceModule 是逐跳的，p0 到 core1 时不必等 core1 算完，先存进上下文；core1 的 p1 出核时再相加。链上没有同步点，这是切 K 放在 FC2 chip 间那一级的理由。</text>
<text x="46" y="565" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">DTE 出核前要保证本级 Reduce credit 够整包，且目标 VC 有 credit；不够就在 PendingTaskQ 等，Router 上不会因为 Reduce 包堵住。</text>
</svg>
```

以一行 core 切 K 为例，编号与图中一致：

1. 每个 core 的 MU / VU 算出本 core 的部分和 `p_k`，完整长度，放在 Core Mem。
2. DataOut DTE 按包头的 PathID 查自己那份 RouterTable，得知这是 Reduce 操作以及走哪个 VC；检查本级 Reduce credit 是否够整包，再在目标 VC 有 credit 的前提下把 `p_k` 作为 Reduce 包发出。不够就在 PendingTaskQ 等。
3. 包进本 core 的 Router，RouterStation 按 PathID 查 RouterTable：`operation = Reduce`、目标方向、下一跳 VC、`reduce_outdata_type`。CrossBar 把它导向 ReduceModule（Data ×3 之一）。进入后锁定到尾 flit。
4. ReduceModule 按 UserID 找上下文：第一份输入分配上下文并写入 FP32 数据；后续方向的输入读出当前值、累加、写回。链上第 k 跳到达的是 `p0 + … + p(k−1)`，与本地出核的 `p_k` 相加。
5. 该包的全部方向输入完成后，结果进输出队列，按 RouterTable 转成 FP32 或 BF16，检查目标 VC credit 与该方向的下游 Reduce credit，作为 CrossBar 的第五路输入重新仲裁后发往下一跳。
6. 每发出一个 flit，向上游产生携带 UserID 的 release；输入侧 flit 离开 VC Buffer 时归还 VC credit。
7. 最后一跳的去向由 RouterTable 定：进本 core 的 Core Mem，或继续送下一 chip、送 R core。
8. 整包发出后 ReduceModule 向 core 返回 UserID（`rmem2ts_done_ch`），TS 只认这一路把 reduce task 置 FINISH。

链上没有同步点：`p0` 到 core 1 时不必等 core 1 算完，先存进上下文，core 1 的 `p1` 出核时再加。这是“流着加”，也是 chip 间 reduce 不做中途汇聚的理由。

### ReduceModule

```svg
<svg viewBox="0 0 1100 520" width="1100" height="520" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="ReduceModule：三路输入、原位累加、下游 credit">
<title>ReduceModule：三路输入、原位累加、下游 credit</title>
<rect width="1100" height="520" fill="#ffffff"/>
<defs><marker id="rca" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="rcas" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker><marker id="rcai" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#3f4451"/></marker><marker id="rcais" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#3f4451"/></marker><marker id="rcab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="rcabs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="rcar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="rcars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="rcac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="rcacs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="rcap" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="rcaps" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker></defs>
<text x="30" y="26" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="start">ReduceModule：三路输入、原位累加、下游 credit</text>
<text x="30" y="46" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="start">照 MAS 的 Reduce Module 数据通路图：三路输入 → 仲裁与整包锁定 → 精度处理 → 16 用户上下文的 RMW 管线 → 输出队列 → 精度转换 → 查 credit 发出</text>
<rect x="40" y="80" width="120" height="36" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="48" y="95" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">输入方向 0</text>
<text x="48" y="109" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">RouterStation 或 Core</text>
<path d="M160 98 L199.3 98" stroke="#2563eb" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rcab)"/>
<rect x="40" y="128" width="120" height="36" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="48" y="143" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">输入方向 1</text>
<text x="48" y="157" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">RouterStation 或 Core</text>
<path d="M160 146 L199.3 146" stroke="#2563eb" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rcab)"/>
<rect x="40" y="176" width="120" height="36" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="48" y="191" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">输入方向 2</text>
<text x="48" y="205" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">RouterStation 或 Core</text>
<path d="M160 194 L199.3 194" stroke="#2563eb" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rcab)"/>
<text x="100" y="236" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="600" text-anchor="middle">3 × 160 GB/s</text>
<rect x="200" y="70" width="700" height="300" rx="8" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.4"/>
<text x="550" y="88" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12.5" fill="#7c3aed" font-weight="700" text-anchor="middle">ReduceModule</text>
<rect x="212" y="100" width="110" height="70" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="220" y="115" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">三路输入仲裁</text>
<text x="220" y="129" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">SRAM / Bank / 计算资源</text>
<text x="220" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">Packet 锁定直至尾 flit</text>
<text x="220" y="153" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">资源不够就反压输入</text>
<rect x="334" y="100" width="110" height="70" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="342" y="115" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">输入精度处理</text>
<text x="342" y="129" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">BF16 → 扩展为 FP32</text>
<text x="342" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">FP32 → 直接进入</text>
<text x="342" y="153" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">统一 FP32 数据面</text>
<path d="M322 135 L333.3 135" stroke="#16181d" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rcai)"/>
<rect x="456" y="100" width="200" height="130" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="464" y="115" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">用户 Reduce 资源与 RMW 管线</text>
<text x="464" y="129" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">Reduce Context SRAM：16 用户 × 16 KiB</text>
<text x="464" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">Read 当前上下文 → FP32 累加 → 原位 Write</text>
<text x="464" y="153" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">首份输入分配上下文并写入</text>
<text x="464" y="165" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">后续方向的输入读改写</text>
<text x="464" y="177" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">当前包全部输入完成并输出前，</text>
<text x="464" y="189" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">同一 User 的下一包不得覆盖</text>
<text x="464" y="201" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">算力 80 GFLOPS</text>
<path d="M444 135 L455.3 135" stroke="#16181d" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rcai)"/>
<rect x="668" y="100" width="220" height="130" rx="5" fill="#ffffff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="676" y="115" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">结果生成与发送</text>
<text x="676" y="129" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">全部方向输入完成 → 进输出队列</text>
<text x="676" y="141" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">输出精度转换：FP32 或 BF16</text>
<text x="676" y="153" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">检查目标 VC credit</text>
<text x="676" y="165" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">+ 该方向的下游 Reduce credit</text>
<text x="676" y="177" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">整包发出 → 向 core 返回 UserID</text>
<text x="676" y="189" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">输出 160 GB/s</text>
<path d="M656 165 L667.3 165" stroke="#16181d" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rcai)"/>
<rect x="212" y="244" width="232" height="56" rx="5" fill="#ffffff" stroke="#c9ced6" stroke-width="1.3"/>
<text x="220" y="259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">RouterTable Copy</text>
<text x="220" y="273" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">operation / 目标方向 / nxt_vc / 输出精度</text>
<text x="220" y="285" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">软件写入，与 Router、DTE 三方一致</text>
<rect x="212" y="308" width="232" height="52" rx="5" fill="#ffffff" stroke="#c9ced6" stroke-width="1.3"/>
<text x="220" y="323" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">User Context Table</text>
<text x="220" y="337" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">UserID · 包状态 · 输入完成 · 输出状态 · Retire 状态</text>
<rect x="456" y="244" width="200" height="56" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="464" y="259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#0d9488" font-weight="700" text-anchor="start">Downstream Reduce Credit Map</text>
<text x="464" y="273" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">按 UserID × 相邻下游方向维护</text>
<text x="464" y="285" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">逐 flit 扣减，按 release 恢复</text>
<rect x="456" y="308" width="200" height="52" rx="5" fill="#ffffff" stroke="#c9ced6" stroke-width="1.3"/>
<text x="464" y="323" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">Retire</text>
<text x="464" y="337" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">收到 User Retire 先停新任务，</text>
<text x="464" y="349" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">下游 credit 全恢复到初始值才删映射</text>
<rect x="668" y="244" width="220" height="56" rx="5" fill="#ffffff" stroke="#c9ced6" stroke-width="1.3"/>
<text x="676" y="259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">CreditBypass XBar</text>
<text x="676" y="273" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">Stream / Reduce release 按 CSR 静态 Mask 旁路，</text>
<text x="676" y="285" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">不经数据面仲裁</text>
<text x="778" y="340" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="middle">三条硬规矩：必须执行 Reduce，不允许降级为转发；</text>
<text x="778" y="353" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#5c6370" font-weight="400" text-anchor="middle">上下文保护；中间累加精度固定 FP32</text>
<rect x="940" y="120" width="130" height="60" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="948" y="135" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">下游 Router</text>
<text x="948" y="149" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">目标方向 / VC</text>
<text x="948" y="161" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">静态路由确定</text>
<path d="M888 150 L939.3 150" stroke="#2563eb" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rcab)"/>
<rect x="940" y="200" width="130" height="60" rx="5" fill="#fdeed8" stroke="#d97706" stroke-width="1.3"/>
<text x="948" y="215" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#d97706" font-weight="700" text-anchor="start">本 core</text>
<text x="948" y="229" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">完成：UserID</text>
<text x="948" y="241" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">Retire 广播</text>
<path d="M888 215 L939.3 215" stroke="#d97706" stroke-width="1.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rcar)"/>
<path d="M940 245 L900 245 L900 330 L656.7 330" stroke="#d97706" stroke-width="1.2" fill="none" stroke-dasharray="4 2" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rcar)"/>
<rect x="30" y="390" width="1040" height="78" rx="5" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.2"/>
<text x="46" y="407" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">MAS 与 HAS 的口径不同：MAS 把累加做在 Router 内的 ReduceModule；HAS 写 Router 内不设 Reduce Buffer，累加由独立的 Rmem 子系统经 reduce_0 / 1 / 2 三个端口完成，</text>
<text x="46" y="423" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">每端口 128 flits buffer，允许 Rmem 改写 vcid。本文按 MAS。</text>
<text x="46" y="439" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">输入侧只有 VC credit 准入（credit &gt; 0 即收），Reduce credit 是另一张网：core 与 ReduceModule 之间按用户粒度，ReduceModule 之间按 flit 加用户双粒度，与数据面分离。</text>
<text x="46" y="455" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">边缘的 ReduceModule 因跨 chip 带宽有限，HAS 要求它自己做精度压缩；跨 chip 时 release 逐 flit 太密，要在 C2C 上压缩后再传。</text>
</svg>
```

| 组件 | 职责 |
| - | - |
| 三路输入仲裁 | 仲裁最多三路输入的 SRAM、Bank 与计算资源；进入后锁定当前包直至尾 flit；资源暂不可用时对输入反压 |
| 输入精度处理 | BF16 输入扩展为 FP32，FP32 直接进入；数据面统一 FP32 |
| Reduce Context SRAM | 16 用户 × 16 KiB，保存当前包的 FP32 中间累加结果 |
| RMW 管线 | 首份输入建立上下文，后续方向的输入执行 Read-Modify-Write 原位累加；算力 80 GFLOPS |
| User Context Table | 记录 UserID、当前包状态、输入完成情况、输出状态与 Retire 状态 |
| RouterTable Copy | 确定输出方向、下一跳 VC、operation 和输出精度；由软件写入，与 Router、DTE 三方一致 |
| 结果生成与发送 | 全部方向输入完成后进输出队列；输出精度可配 FP32 或 BF16；发送前检查目标 VC credit 与下游 Reduce credit |
| Downstream Reduce Credit Map | 按 UserID 加相邻下游方向维护 Reduce credit，逐 flit 扣减、按 release 恢复 |

三条硬约束：

* **必须执行 Reduce**：SRAM、Bank 或计算单元暂不可用时对输入反压，不允许绕过 Reduce 降级为直接存储或转发
* **上下文保护**：当前包的全部输入完成并输出前，同一 User 的下一个包不得覆盖该上下文
* **精度**：输入 FP32 或 BF16，中间累加固定 FP32，输出可配 FP32 或 BF16

输入侧只有 VC credit 准入（`credit > 0` 即收）；Reduce credit 是另一张网，与数据面分离。

### Reduce credit 的闭环

```svg
<svg viewBox="0 0 1100 440" width="1100" height="440" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="Reduce credit 的闭环：谁扣、谁还、什么时候删">
<title>Reduce credit 的闭环：谁扣、谁还、什么时候删</title>
<rect width="1100" height="440" fill="#ffffff"/>
<defs><marker id="rda" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="rdas" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker><marker id="rdai" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#3f4451"/></marker><marker id="rdais" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#3f4451"/></marker><marker id="rdab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="rdabs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="rdar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="rdars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="rdac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="rdacs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="rdap" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="rdaps" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker></defs>
<text x="30" y="26" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="start">Reduce credit 的闭环：谁扣、谁还、什么时候删</text>
<text x="30" y="46" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="start">三段各自一份计数：DTE 持本级 ReduceModule 的，每个 ReduceModule 持相邻下游的；Router 不持有，只把 release 按静态 Mask 转发</text>
<rect x="40" y="90" width="170" height="70" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.3"/>
<text x="48" y="105" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#0d9488" font-weight="700" text-anchor="start">DataOut DTE（源 core）</text>
<text x="48" y="119" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">本级 Reduce credit 表</text>
<text x="48" y="131" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">每用户一个 entry，flit 粒度</text>
<text x="48" y="143" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">建 stream credit 表项时分配</text>
<rect x="280" y="90" width="190" height="70" rx="5" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.3"/>
<text x="288" y="105" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#7c3aed" font-weight="700" text-anchor="start">本级 ReduceModule</text>
<text x="288" y="119" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">ResourceMap：UserID × 下游方向</text>
<text x="288" y="131" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">每发一个 flit 扣一个</text>
<text x="288" y="143" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">收 release(UserID) 恢复</text>
<rect x="540" y="90" width="190" height="70" rx="5" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.3"/>
<text x="548" y="105" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#7c3aed" font-weight="700" text-anchor="start">下游 ReduceModule</text>
<text x="548" y="119" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">同上，持再下游的映射</text>
<text x="548" y="131" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">每收一个 flit 累加</text>
<text x="548" y="143" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">每发出一个 flit release 一个</text>
<rect x="800" y="90" width="190" height="70" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="808" y="105" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#16181d" font-weight="700" text-anchor="start">链尾</text>
<text x="808" y="119" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">落 core 或送下一 chip</text>
<text x="808" y="131" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">整包发出 → 完成回 UserID</text>
<text x="808" y="143" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">release 经 Router 静态旁路回上游</text>
<path d="M210 115 L279.3 115" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rdab)"/>
<rect x="236" y="94" width="18" height="18" rx="9" fill="#ffffff" stroke="#2563eb" stroke-width="1.3"/>
<text x="245" y="106.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#2563eb" font-weight="700" text-anchor="middle">扣</text>
<text x="245" y="140" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" font-weight="400" text-anchor="middle">够整包才发</text>
<path d="M470 115 L539.3 115" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rdab)"/>
<rect x="496" y="94" width="18" height="18" rx="9" fill="#ffffff" stroke="#2563eb" stroke-width="1.3"/>
<text x="505" y="106.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#2563eb" font-weight="700" text-anchor="middle">扣</text>
<text x="505" y="140" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#2563eb" font-weight="400" text-anchor="middle">每 flit 扣一个</text>
<path d="M730 115 L799.3 115" stroke="#2563eb" stroke-width="2.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rdab)"/>
<path d="M600 160 L600 200 L496 200" stroke="#d97706" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M478 200 L375 200 L375 160.7" stroke="#d97706" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rdar)"/>
<rect x="478" y="191" width="18" height="18" rx="9" fill="#ffffff" stroke="#d97706" stroke-width="1.3"/>
<text x="487" y="203.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#d97706" font-weight="700" text-anchor="middle">还</text>
<text x="487" y="222" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="middle">每发出一个 flit，release(Valid + UserID)</text>
<path d="M330 160 L330 240 L125 240 L125 160.7" stroke="#d97706" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rdar)"/>
<text x="228" y="258" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="middle">经独立释放通道把 Valid + UserID 送回 DTE</text>
<rect x="40" y="290" width="300" height="60" rx="5" fill="#dbeafe" stroke="#2563eb" stroke-width="1.3"/>
<text x="48" y="305" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#2563eb" font-weight="700" text-anchor="start">TS 侧</text>
<text x="48" y="319" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">软件配 reduce_num = N：TS 连续下发 N 笔 credit 请求（router2ts_credit_ch），</text>
<text x="48" y="331" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">满足即顺序下发，直到收全 N 笔 Reduce Done</text>
<rect x="370" y="290" width="320" height="60" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="378" y="305" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#16181d" font-weight="700" text-anchor="start">Retire 与表项删除</text>
<text x="378" y="319" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">core 判定任务链结束 → 广播 User Retire</text>
<text x="378" y="331" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">ReduceModule 先记录，待相邻下游各方向 credit 全恢复到分配数量才删该用户映射</text>
<rect x="720" y="290" width="340" height="60" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="728" y="305" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#16181d" font-weight="700" text-anchor="start">跨 chip</text>
<text x="728" y="319" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">PCIe 两侧要同步上下游 Reduce credit，防止上游超发；</text>
<text x="728" y="331" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">release 粒度是 flit，在 C2C 上压缩包数量后再传；边缘 ReduceModule 自行精度压缩</text>
<rect x="30" y="370" width="1040" height="46" rx="5" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.2"/>
<text x="46" y="387" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">计数实体只有两处：DTE 的本级表、各 ReduceModule 的 ResourceMap；Router 的 CreditBypass XBar 只转发。用户的 entry 在建 stream credit 表项时分配，Retire 且 credit 恢复到分配数量后删除。</text>
<text x="46" y="403" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">这张 credit 网与 VC credit 分离：VC credit 保证下游 buffer 不溢，Reduce credit 保证 ReduceModule 上下文不被超发；两者都满足才发。</text>
</svg>
```

| | 谁持有计数 | 何时扣 | 何时还 | 表项的建与删 |
| - | - | - | - | - |
| core 与本级 ReduceModule 之间 | DataOut DTE 的本级 Reduce credit 表，每用户一个 entry，flit 粒度 | DTE 发 Reduce 包前要求够整包 | ReduceModule 每完成一次 Reduce 并把 flit 发给下游，经独立释放通道送回 Valid + UserID | 用户建 stream credit 表项时分配一个 entry 的 credit 数量；收到 Retire 且 credit 恢复到分配数量后删除 |
| ReduceModule 与相邻下游之间 | 上游 ReduceModule 的 Downstream Reduce Credit Map，按 UserID 加方向 | 每发一个 flit 扣一个，同时还要查目标 VC credit | 下游每发出一个 flit 产生携带 UserID 的 release，经 Router 的静态旁路返回 | 同上；ReduceModule 收到 User Retire 先记录，待相邻下游各方向 credit 全部恢复到初始值才删该用户映射 |

Router 不维护 Reduce credit，只把 release 按 CSR 配的静态方向 Mask 转发，不查 RouterTable，可多播。跨 chip 时 PCIe 两侧要同步上下游 Reduce credit 防止上游超发；release 粒度是 flit，在 C2C 上压缩包数量后再传；边缘的 ReduceModule 自行做精度压缩以减少跨 chip 带宽。

### TS 与 DTE 侧的配合

任务链里的逐级 reduce 是一个 DTE task，字段与普通 DTE task 的差别：

| 字段 | 取值 | 含义 |
| - | - | - |
| `task_recv_unit` | 2（rmem） | 完成信号由 ReduceModule 回，不是 DTE 的 ack |
| `credit_en` | 1 | 下发前要查 credit |
| `task_reduce_iss` | 1 | 这是逐级 reduce 任务 |
| `reduce_num` | N | 该任务要操作几次 reduce（软件侧属性） |

TS 的动作：

* 发现是 reduce task 后**顺序连续下发 N 笔 credit 请求**（`router2ts_credit_ch`），credit 满足即可顺序下发，直到收全 N 笔 Reduce Done
* 完成判定拆成两半：**DTE ack 只代表搬运完成**，执行 `consume_only`，不改 stream 状态；**只有 Router 的 Reduce Done 才有权把 reduce task 置 FINISH**。两个事件可任意顺序到达，Reduce Done 可以被 Hold，但必须等匹配的 DTE ack 被消费后才提交
* 匹配按包做：包头带 `reduce_seq`（0～N−1），DTE 发出时打上、Router 原样带回，TS 用两张 N 位位图逐位配对，两张全满才提交
* Router 不携带 stream_id，`Task_done` 按 user_id 找对应 stream

DTE 的动作：DTE 中要有一份 RouterTable，按 PathID 查到 VC 与 Reduce 资源需求；本级 Reduce credit 够整包才发，否则任务在 PendingTaskQ 等。

***

## 第三层：EP 组间的 reduction 链

### 链的形状

```svg
<svg viewBox="0 0 1200 1500" width="1200" height="1500" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="EP 组间的 reduction 链：落本组就累加，不落就透传，链尾才出核">
<title>EP 组间的 reduction 链</title>
<rect width="1200" height="1500" fill="#ffffff"/>
<defs><marker id="rca" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="rcas" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker><marker id="rcai" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#3f4451"/></marker><marker id="rcais" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#3f4451"/></marker><marker id="rcab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="rcabs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="rcar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="rcars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="rcac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="rcacs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="rcap" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="rcaps" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker></defs>
<text x="600.0" y="26" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="middle">EP 组间的 reduction 链：落本组就累加，不落就透传，链尾才出核</text>
<text x="600.0" y="46" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#5c6370" font-weight="400" text-anchor="middle">按机柜的物理排布画到 core 一级；每个 EP 组（相邻两层 8 chip）借最后一列的一个 core 作 R core，各组的 R core 沿最后一列串成一条链</text>
<rect x="138" y="132" width="776" height="370" rx="6" fill="#fffdf7" stroke="#d97706" stroke-width="1.0" stroke-dasharray="5 3"/>
<text x="132" y="321.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#d97706" font-weight="700" text-anchor="end">tray 0</text>
<rect x="138" y="524" width="776" height="370" rx="6" fill="#fffdf7" stroke="#d97706" stroke-width="1.0" stroke-dasharray="5 3"/>
<text x="132" y="713.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#d97706" font-weight="700" text-anchor="end">tray 1</text>
<rect x="138" y="916" width="776" height="370" rx="6" fill="#fffdf7" stroke="#d97706" stroke-width="1.0" stroke-dasharray="5 3"/>
<text x="132" y="1105.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#d97706" font-weight="700" text-anchor="end">tray 2</text>
<rect x="150" y="140" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M178 165 L195 165" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 165 L225 165" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 165 L255 165" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 165 L298 165" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 196 L195 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 196 L225 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 196 L255 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 196 L298 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 165 L178 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 184 L178 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 165 L208 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 184 L208 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 165 L238 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 184 L238 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 165 L268 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 184 L268 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 165 L298 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 184 L298 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="165" y="177" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="320" y="183" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="168" y="183" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 0</text>
<rect x="165.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="195.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="225.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="255.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="285.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="165.0" y="187.0" width="26" height="18" rx="3" fill="#dbeafe" stroke="#2563eb" stroke-width="1.2"/>
<text x="178.0" y="199.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="700" text-anchor="middle">B</text>
<rect x="195.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="225.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="255.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="285.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="342" y="140" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M370 165 L387 165" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 165 L417 165" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 165 L447 165" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 165 L490 165" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 196 L387 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 196 L417 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 196 L447 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 196 L490 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 165 L370 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 184 L370 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 165 L400 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 184 L400 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 165 L430 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 184 L430 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 165 L460 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 184 L460 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 165 L490 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 184 L490 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="357" y="177" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="512" y="183" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="360" y="183" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 1</text>
<rect x="357.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="387.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="417.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="447.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="477.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="357.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="387.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="417.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="447.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="477.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="534" y="140" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M562 165 L579 165" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 165 L609 165" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 165 L639 165" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 165 L682 165" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 196 L579 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 196 L609 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 196 L639 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 196 L682 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 165 L562 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 184 L562 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 165 L592 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 184 L592 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 165 L622 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 184 L622 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 165 L652 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 184 L652 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 165 L682 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 184 L682 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="549" y="177" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="704" y="183" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="552" y="183" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 2</text>
<rect x="549.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="579.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="609.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="639.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="669.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="549.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="579.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="609.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="639.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="669.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="726" y="140" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M754 165 L771 165" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 165 L801 165" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 165 L831 165" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M857 165 L874 165" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 196 L771 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 196 L801 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 196 L831 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M857 196 L874 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 165 L754 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 184 L754 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 165 L784 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 184 L784 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 165 L814 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 184 L814 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 165 L844 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 184 L844 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 165 L874 177" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 184 L874 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="741" y="177" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="896" y="183" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="744" y="183" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 3</text>
<rect x="741.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="771.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="801.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="831.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="861.0" y="156.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="168" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="741.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="771.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="801.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="831.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="861.0" y="187.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="199" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="150" y="232" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M178 257 L195 257" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 257 L225 257" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 257 L255 257" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 257 L298 257" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 288 L195 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 288 L225 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 288 L255 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 288 L298 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 257 L178 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 276 L178 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 257 L208 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 276 L208 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 257 L238 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 276 L238 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 257 L268 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 276 L268 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 257 L298 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 276 L298 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="165" y="269" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="320" y="275" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="168" y="275" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 4</text>
<rect x="165.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="195.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="225.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="255.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="285.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="165.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="195.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="225.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="255.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="285.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="342" y="232" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M370 257 L387 257" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 257 L417 257" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 257 L447 257" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 257 L490 257" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 288 L387 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 288 L417 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 288 L447 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 288 L490 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 257 L370 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 276 L370 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 257 L400 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 276 L400 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 257 L430 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 276 L430 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 257 L460 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 276 L460 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 257 L490 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 276 L490 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="357" y="269" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="512" y="275" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="360" y="275" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 5</text>
<rect x="357.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="387.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="417.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="447.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="477.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="357.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="387.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="417.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="447.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="477.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="534" y="232" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M562 257 L579 257" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 257 L609 257" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 257 L639 257" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 257 L682 257" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 288 L579 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 288 L609 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 288 L639 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 288 L682 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 257 L562 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 276 L562 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 257 L592 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 276 L592 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 257 L622 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 276 L622 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 257 L652 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 276 L652 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 257 L682 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 276 L682 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="549" y="269" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="704" y="275" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="552" y="275" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 6</text>
<rect x="549.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="579.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="609.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="639.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="669.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="549.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="579.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="609.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="639.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="669.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="726" y="232" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M754 257 L771 257" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 257 L801 257" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 257 L831 257" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M857 257 L874 257" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 288 L771 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 288 L801 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 288 L831 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 257 L754 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 276 L754 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 257 L784 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 276 L784 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 257 L814 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 276 L814 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 279 L844 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 257 L874 269" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 276 L874 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="741" y="269" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="896" y="275" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="744" y="275" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 7</text>
<rect x="741.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="771.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="801.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="831.0" y="248.0" width="26" height="18" rx="3" fill="#fee2e2" stroke="#dc2626" stroke-width="1.2"/>
<text x="844.0" y="260.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#dc2626" font-weight="700" text-anchor="middle">R</text>
<rect x="861.0" y="248.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="260" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="741.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="771.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="801.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="831.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="861.0" y="279.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="291" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="150" y="324" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M178 349 L195 349" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 349 L225 349" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 349 L255 349" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 349 L298 349" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 380 L195 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 380 L225 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 380 L255 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 380 L298 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 349 L178 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 368 L178 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 349 L208 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 368 L208 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 349 L238 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 368 L238 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 349 L268 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 368 L268 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 349 L298 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 368 L298 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="165" y="361" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="320" y="367" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="168" y="367" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 8</text>
<rect x="165.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="195.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="225.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="255.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="285.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="165.0" y="371.0" width="26" height="18" rx="3" fill="#dbeafe" stroke="#2563eb" stroke-width="1.2"/>
<text x="178.0" y="383.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="700" text-anchor="middle">B</text>
<rect x="195.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="225.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="255.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="285.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="342" y="324" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M370 349 L387 349" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 349 L417 349" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 349 L447 349" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 349 L490 349" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 380 L387 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 380 L417 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 380 L447 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 380 L490 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 349 L370 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 368 L370 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 349 L400 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 368 L400 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 349 L430 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 368 L430 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 349 L460 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 368 L460 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 349 L490 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 368 L490 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="357" y="361" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="512" y="367" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="360" y="367" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 9</text>
<rect x="357.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="387.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="417.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="447.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="477.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="357.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="387.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="417.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="447.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="477.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="534" y="324" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M562 349 L579 349" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 349 L609 349" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 349 L639 349" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 349 L682 349" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 380 L579 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 380 L609 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 380 L639 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 380 L682 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 349 L562 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 368 L562 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 349 L592 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 368 L592 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 349 L622 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 368 L622 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 349 L652 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 368 L652 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 349 L682 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 368 L682 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="549" y="361" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="704" y="367" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="552" y="367" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 10</text>
<rect x="549.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="579.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="609.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="639.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="669.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="549.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="579.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="609.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="639.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="669.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="726" y="324" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M857 349 L874 349" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 380 L771 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 380 L801 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 380 L831 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 349 L754 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 368 L754 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 349 L784 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 368 L784 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 349 L814 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 368 L814 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 349 L844 358" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 371 L844 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 349 L874 361" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 368 L874 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="741" y="361" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="896" y="367" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="744" y="367" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 11</text>
<rect x="741.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="771.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="801.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="831.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="861.0" y="340.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="352" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="741.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="771.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="801.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="831.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="861.0" y="371.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="383" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="150" y="416" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M178 441 L195 441" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 441 L225 441" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 441 L255 441" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 441 L298 441" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 472 L195 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 472 L225 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 472 L255 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 472 L298 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 441 L178 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 460 L178 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 441 L208 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 460 L208 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 441 L238 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 460 L238 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 441 L268 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 460 L268 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 441 L298 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 460 L298 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="165" y="453" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="320" y="459" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="168" y="459" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 12</text>
<rect x="165.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="195.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="225.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="255.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="285.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="165.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="195.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="225.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="255.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="285.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="342" y="416" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M370 441 L387 441" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 441 L417 441" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 441 L447 441" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 441 L490 441" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 472 L387 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 472 L417 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 472 L447 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 472 L490 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 441 L370 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 460 L370 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 441 L400 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 460 L400 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 441 L430 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 460 L430 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 441 L460 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 460 L460 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 441 L490 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 460 L490 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="357" y="453" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="512" y="459" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="360" y="459" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 13</text>
<rect x="357.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="387.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="417.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="447.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="477.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="357.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="387.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="417.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="447.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="477.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="534" y="416" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M562 441 L579 441" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 441 L609 441" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 441 L639 441" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 441 L682 441" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 472 L579 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 472 L609 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 472 L639 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 472 L682 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 441 L562 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 460 L562 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 441 L592 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 460 L592 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 441 L622 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 460 L622 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 441 L652 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 460 L652 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 441 L682 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 460 L682 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="549" y="453" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="704" y="459" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="552" y="459" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 14</text>
<rect x="549.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="579.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="609.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="639.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="669.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="549.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="579.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="609.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="639.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="669.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="726" y="416" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M857 441 L874 441" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 472 L771 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 472 L801 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 472 L831 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 441 L754 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 460 L754 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 441 L784 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 460 L784 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 441 L814 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 460 L814 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 441 L844 450" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 463 L844 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 441 L874 453" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 460 L874 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="741" y="453" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="896" y="459" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="744" y="459" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 15</text>
<rect x="741.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="771.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="801.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="831.0" y="432.0" width="26" height="18" rx="3" fill="#fee2e2" stroke="#dc2626" stroke-width="1.2"/>
<text x="844.0" y="444.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#dc2626" font-weight="700" text-anchor="middle">R</text>
<rect x="861.0" y="432.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="444" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="741.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="771.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="801.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="831.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="861.0" y="463.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="475" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="150" y="532" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M178 557 L195 557" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 557 L225 557" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 557 L255 557" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 557 L298 557" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 588 L195 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 588 L225 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 588 L255 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 588 L298 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 557 L178 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 576 L178 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 557 L208 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 576 L208 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 557 L238 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 576 L238 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 557 L268 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 576 L268 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 557 L298 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 576 L298 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="165" y="569" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="320" y="575" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="168" y="575" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 0</text>
<rect x="165.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="195.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="225.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="255.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="285.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="165.0" y="579.0" width="26" height="18" rx="3" fill="#dbeafe" stroke="#2563eb" stroke-width="1.2"/>
<text x="178.0" y="591.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="700" text-anchor="middle">B</text>
<rect x="195.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="225.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="255.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="285.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="342" y="532" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M370 557 L387 557" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 557 L417 557" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 557 L447 557" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 557 L490 557" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 588 L387 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 588 L417 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 588 L447 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 588 L490 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 557 L370 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 576 L370 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 557 L400 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 576 L400 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 557 L430 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 576 L430 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 557 L460 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 576 L460 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 557 L490 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 576 L490 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="357" y="569" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="512" y="575" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="360" y="575" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 1</text>
<rect x="357.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="387.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="417.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="447.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="477.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="357.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="387.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="417.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="447.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="477.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="534" y="532" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M562 557 L579 557" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 557 L609 557" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 557 L639 557" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 557 L682 557" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 588 L579 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 588 L609 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 588 L639 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 588 L682 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 557 L562 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 576 L562 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 557 L592 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 576 L592 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 557 L622 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 576 L622 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 557 L652 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 576 L652 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 557 L682 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 576 L682 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="549" y="569" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="704" y="575" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="552" y="575" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 2</text>
<rect x="549.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="579.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="609.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="639.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="669.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="549.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="579.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="609.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="639.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="669.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="726" y="532" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M857 557 L874 557" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 588 L771 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 588 L801 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 588 L831 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 557 L754 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 576 L754 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 557 L784 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 576 L784 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 557 L814 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 576 L814 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 557 L844 566" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 579 L844 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 557 L874 569" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 576 L874 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="741" y="569" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="896" y="575" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="744" y="575" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 3</text>
<rect x="741.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="771.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="801.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="831.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="861.0" y="548.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="560" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="741.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="771.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="801.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="831.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="861.0" y="579.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="591" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="150" y="624" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M178 649 L195 649" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 649 L225 649" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 649 L255 649" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 649 L298 649" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 680 L195 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 680 L225 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 680 L255 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 680 L298 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 649 L178 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 668 L178 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 649 L208 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 668 L208 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 649 L238 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 668 L238 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 649 L268 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 668 L268 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 649 L298 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 668 L298 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="165" y="661" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="320" y="667" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="168" y="667" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 4</text>
<rect x="165.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="195.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="225.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="255.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="285.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="165.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="195.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="225.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="255.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="285.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="342" y="624" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M370 649 L387 649" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 649 L417 649" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 649 L447 649" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 649 L490 649" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 680 L387 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 680 L417 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 680 L447 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 680 L490 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 649 L370 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 668 L370 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 649 L400 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 668 L400 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 649 L430 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 668 L430 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 649 L460 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 668 L460 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 649 L490 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 668 L490 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="357" y="661" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="512" y="667" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="360" y="667" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 5</text>
<rect x="357.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="387.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="417.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="447.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="477.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="357.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="387.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="417.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="447.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="477.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="534" y="624" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M562 649 L579 649" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 649 L609 649" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 649 L639 649" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 649 L682 649" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 680 L579 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 680 L609 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 680 L639 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 680 L682 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 649 L562 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 668 L562 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 649 L592 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 668 L592 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 649 L622 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 668 L622 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 649 L652 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 668 L652 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 649 L682 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 668 L682 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="549" y="661" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="704" y="667" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="552" y="667" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 6</text>
<rect x="549.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="579.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="609.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="639.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="669.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="549.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="579.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="609.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="639.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="669.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="726" y="624" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M857 649 L874 649" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 680 L771 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 680 L801 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 680 L831 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 649 L754 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 668 L754 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 649 L784 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 668 L784 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 649 L814 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 668 L814 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 649 L844 658" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 671 L844 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 649 L874 661" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 668 L874 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="741" y="661" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="896" y="667" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="744" y="667" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 7</text>
<rect x="741.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="771.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="801.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="831.0" y="640.0" width="26" height="18" rx="3" fill="#fee2e2" stroke="#dc2626" stroke-width="1.2"/>
<text x="844.0" y="652.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#dc2626" font-weight="700" text-anchor="middle">R</text>
<rect x="861.0" y="640.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="652" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="741.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="771.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="801.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="831.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="861.0" y="671.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="683" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="150" y="716" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M178 741 L195 741" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 741 L225 741" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 741 L255 741" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 741 L298 741" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 772 L195 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 772 L225 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 772 L255 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 772 L298 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 741 L178 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 760 L178 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 741 L208 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 760 L208 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 741 L238 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 760 L238 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 741 L268 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 760 L268 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 741 L298 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 760 L298 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="165" y="753" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="320" y="759" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="168" y="759" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 8</text>
<rect x="165.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="195.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="225.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="255.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="285.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="165.0" y="763.0" width="26" height="18" rx="3" fill="#dbeafe" stroke="#2563eb" stroke-width="1.2"/>
<text x="178.0" y="775.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="700" text-anchor="middle">B</text>
<rect x="195.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="225.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="255.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="285.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="342" y="716" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M370 741 L387 741" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 741 L417 741" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 741 L447 741" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 741 L490 741" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 772 L387 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 772 L417 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 772 L447 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 772 L490 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 741 L370 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 760 L370 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 741 L400 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 760 L400 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 741 L430 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 760 L430 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 741 L460 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 760 L460 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 741 L490 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 760 L490 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="357" y="753" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="512" y="759" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="360" y="759" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 9</text>
<rect x="357.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="387.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="417.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="447.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="477.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="357.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="387.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="417.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="447.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="477.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="534" y="716" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M562 741 L579 741" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 741 L609 741" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 741 L639 741" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 741 L682 741" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 772 L579 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 772 L609 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 772 L639 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 772 L682 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 741 L562 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 760 L562 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 741 L592 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 760 L592 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 741 L622 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 760 L622 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 741 L652 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 760 L652 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 741 L682 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 760 L682 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="549" y="753" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="704" y="759" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="552" y="759" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 10</text>
<rect x="549.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="579.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="609.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="639.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="669.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="549.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="579.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="609.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="639.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="669.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="726" y="716" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M857 741 L874 741" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 772 L771 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 772 L801 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 772 L831 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 741 L754 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 760 L754 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 741 L784 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 760 L784 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 741 L814 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 760 L814 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 741 L844 750" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 763 L844 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 741 L874 753" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 760 L874 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="741" y="753" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="896" y="759" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="744" y="759" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 11</text>
<rect x="741.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="771.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="801.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="831.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="861.0" y="732.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="744" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="741.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="771.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="801.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="831.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="861.0" y="763.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="775" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="150" y="808" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M178 833 L195 833" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 833 L225 833" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 833 L255 833" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 833 L298 833" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 864 L195 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 864 L225 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 864 L255 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 864 L298 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 833 L178 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 852 L178 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 833 L208 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 852 L208 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 833 L238 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 852 L238 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 833 L268 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 852 L268 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 833 L298 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 852 L298 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="165" y="845" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="320" y="851" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="168" y="851" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 12</text>
<rect x="165.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="195.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="225.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="255.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="285.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="165.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="195.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="225.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="255.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="285.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="342" y="808" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M370 833 L387 833" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 833 L417 833" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 833 L447 833" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 833 L490 833" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 864 L387 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 864 L417 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 864 L447 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 864 L490 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 833 L370 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 852 L370 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 833 L400 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 852 L400 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 833 L430 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 852 L430 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 833 L460 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 852 L460 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 833 L490 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 852 L490 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="357" y="845" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="512" y="851" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="360" y="851" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 13</text>
<rect x="357.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="387.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="417.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="447.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="477.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="357.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="387.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="417.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="447.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="477.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="534" y="808" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M562 833 L579 833" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 833 L609 833" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 833 L639 833" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 833 L682 833" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 864 L579 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 864 L609 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 864 L639 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 864 L682 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 833 L562 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 852 L562 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 833 L592 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 852 L592 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 833 L622 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 852 L622 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 833 L652 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 852 L652 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 833 L682 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 852 L682 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="549" y="845" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="704" y="851" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="552" y="851" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 14</text>
<rect x="549.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="579.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="609.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="639.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="669.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="549.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="579.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="609.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="639.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="669.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="726" y="808" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M857 833 L874 833" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 864 L771 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 864 L801 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 864 L831 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 833 L754 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 852 L754 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 833 L784 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 852 L784 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 833 L814 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 852 L814 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 833 L844 842" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 855 L844 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 833 L874 845" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 852 L874 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="741" y="845" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="896" y="851" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="744" y="851" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 15</text>
<rect x="741.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="771.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="801.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="831.0" y="824.0" width="26" height="18" rx="3" fill="#fee2e2" stroke="#dc2626" stroke-width="1.2"/>
<text x="844.0" y="836.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#dc2626" font-weight="700" text-anchor="middle">R</text>
<rect x="861.0" y="824.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="836" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="741.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="771.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="801.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="831.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="861.0" y="855.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="867" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="150" y="924" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M178 949 L195 949" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 949 L225 949" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 949 L255 949" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 949 L298 949" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 980 L195 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 980 L225 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 980 L255 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 980 L298 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 949 L178 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 968 L178 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 949 L208 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 968 L208 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 949 L238 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 968 L238 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 949 L268 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 968 L268 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 949 L298 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 968 L298 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="165" y="961" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="320" y="967" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="168" y="967" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 0</text>
<rect x="165.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="195.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="225.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="255.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="285.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="165.0" y="971.0" width="26" height="18" rx="3" fill="#dbeafe" stroke="#2563eb" stroke-width="1.2"/>
<text x="178.0" y="983.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="700" text-anchor="middle">B</text>
<rect x="195.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="225.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="255.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="285.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="342" y="924" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M370 949 L387 949" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 949 L417 949" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 949 L447 949" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 949 L490 949" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 980 L387 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 980 L417 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 980 L447 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 980 L490 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 949 L370 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 968 L370 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 949 L400 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 968 L400 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 949 L430 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 968 L430 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 949 L460 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 968 L460 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 949 L490 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 968 L490 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="357" y="961" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="512" y="967" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="360" y="967" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 1</text>
<rect x="357.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="387.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="417.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="447.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="477.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="357.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="387.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="417.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="447.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="477.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="534" y="924" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M562 949 L579 949" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 949 L609 949" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 949 L639 949" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 949 L682 949" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 980 L579 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 980 L609 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 980 L639 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 980 L682 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 949 L562 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 968 L562 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 949 L592 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 968 L592 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 949 L622 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 968 L622 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 949 L652 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 968 L652 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 949 L682 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 968 L682 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="549" y="961" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="704" y="967" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="552" y="967" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 2</text>
<rect x="549.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="579.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="609.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="639.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="669.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="549.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="579.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="609.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="639.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="669.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="726" y="924" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M857 949 L874 949" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 980 L771 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 980 L801 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 980 L831 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 949 L754 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 968 L754 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 949 L784 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 968 L784 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 949 L814 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 968 L814 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 949 L844 958" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 971 L844 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 949 L874 961" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 968 L874 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="741" y="961" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="896" y="967" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="744" y="967" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 3</text>
<rect x="741.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="771.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="801.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="831.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="861.0" y="940.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="952" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="741.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="771.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="801.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="831.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="861.0" y="971.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="983" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="150" y="1016" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M178 1041 L195 1041" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 1041 L225 1041" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 1041 L255 1041" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 1041 L298 1041" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 1072 L195 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 1072 L225 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 1072 L255 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 1072 L298 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 1041 L178 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 1060 L178 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 1041 L208 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 1060 L208 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 1041 L238 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 1060 L238 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 1041 L268 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 1060 L268 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 1041 L298 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 1060 L298 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="165" y="1053" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="320" y="1059" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="168" y="1059" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 4</text>
<rect x="165.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="195.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="225.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="255.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="285.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="165.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="195.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="225.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="255.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="285.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="342" y="1016" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M370 1041 L387 1041" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 1041 L417 1041" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 1041 L447 1041" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 1041 L490 1041" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 1072 L387 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 1072 L417 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 1072 L447 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 1072 L490 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 1041 L370 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 1060 L370 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 1041 L400 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 1060 L400 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 1041 L430 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 1060 L430 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 1041 L460 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 1060 L460 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 1041 L490 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 1060 L490 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="357" y="1053" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="512" y="1059" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="360" y="1059" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 5</text>
<rect x="357.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="387.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="417.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="447.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="477.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="357.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="387.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="417.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="447.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="477.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="534" y="1016" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M562 1041 L579 1041" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 1041 L609 1041" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 1041 L639 1041" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 1041 L682 1041" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 1072 L579 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 1072 L609 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 1072 L639 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 1072 L682 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 1041 L562 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 1060 L562 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 1041 L592 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 1060 L592 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 1041 L622 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 1060 L622 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 1041 L652 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 1060 L652 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 1041 L682 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 1060 L682 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="549" y="1053" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="704" y="1059" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="552" y="1059" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 6</text>
<rect x="549.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="579.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="609.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="639.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="669.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="549.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="579.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="609.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="639.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="669.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="726" y="1016" width="176" height="78" rx="5" fill="#f3f4f6" stroke="#9aa1ad" stroke-width="1.1"/>
<path d="M857 1041 L874 1041" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 1072 L771 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 1072 L801 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 1072 L831 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 1041 L754 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 1060 L754 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 1041 L784 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 1060 L784 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 1041 L814 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 1060 L814 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 1041 L844 1050" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 1063 L844 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 1041 L874 1053" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 1060 L874 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="741" y="1053" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="896" y="1059" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="744" y="1059" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 7</text>
<rect x="741.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="771.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="801.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="831.0" y="1032.0" width="26" height="18" rx="3" fill="#fee2e2" stroke="#dc2626" stroke-width="1.2"/>
<text x="844.0" y="1044.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#dc2626" font-weight="700" text-anchor="middle">R</text>
<rect x="861.0" y="1032.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="1044" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="741.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="771.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="801.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="831.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="861.0" y="1063.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="1075" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="150" y="1108" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M178 1133 L195 1133" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 1133 L225 1133" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 1133 L255 1133" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 1133 L298 1133" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 1164 L195 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 1164 L225 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 1164 L255 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 1164 L298 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 1133 L178 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 1152 L178 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 1133 L208 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 1152 L208 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 1133 L238 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 1152 L238 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 1133 L268 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 1152 L268 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 1133 L298 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 1152 L298 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="165" y="1145" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="320" y="1151" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="168" y="1151" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 8</text>
<rect x="165.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="195.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="225.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="255.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="285.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="165.0" y="1155.0" width="26" height="18" rx="3" fill="#dbeafe" stroke="#2563eb" stroke-width="1.2"/>
<text x="178.0" y="1167.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#2563eb" font-weight="700" text-anchor="middle">B</text>
<rect x="195.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="225.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="255.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="285.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="342" y="1108" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M370 1133 L387 1133" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 1133 L417 1133" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 1133 L447 1133" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 1133 L490 1133" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 1164 L387 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 1164 L417 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 1164 L447 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 1164 L490 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 1133 L370 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 1152 L370 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 1133 L400 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 1152 L400 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 1133 L430 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 1152 L430 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 1133 L460 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 1152 L460 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 1133 L490 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 1152 L490 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="357" y="1145" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="512" y="1151" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="360" y="1151" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 9</text>
<rect x="357.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="387.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="417.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="447.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="477.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="357.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="387.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="417.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="447.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="477.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="534" y="1108" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M562 1133 L579 1133" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 1133 L609 1133" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 1133 L639 1133" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 1133 L682 1133" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 1164 L579 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 1164 L609 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 1164 L639 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 1164 L682 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 1133 L562 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 1152 L562 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 1133 L592 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 1152 L592 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 1133 L622 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 1152 L622 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 1133 L652 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 1152 L652 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 1133 L682 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 1152 L682 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="549" y="1145" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="704" y="1151" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="552" y="1151" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 10</text>
<rect x="549.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="579.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="609.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="639.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="669.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="549.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="579.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="609.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="639.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="669.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="726" y="1108" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M857 1133 L874 1133" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 1164 L771 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 1164 L801 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 1164 L831 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 1133 L754 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 1152 L754 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 1133 L784 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 1152 L784 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 1133 L814 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 1152 L814 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 1133 L844 1142" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 1155 L844 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 1133 L874 1145" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 1152 L874 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="741" y="1145" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="896" y="1151" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="744" y="1151" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 11</text>
<rect x="741.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="771.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="801.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="831.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="861.0" y="1124.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="1136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="741.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="771.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="801.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="831.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="861.0" y="1155.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="1167" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="150" y="1200" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M178 1225 L195 1225" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 1225 L225 1225" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 1225 L255 1225" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 1225 L298 1225" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 1256 L195 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M221 1256 L225 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M251 1256 L255 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M281 1256 L298 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 1225 L178 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M178 1244 L178 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 1225 L208 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M208 1244 L208 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 1225 L238 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M238 1244 L238 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 1225 L268 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M268 1244 L268 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 1225 L298 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298 1244 L298 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="165" y="1237" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="320" y="1243" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="168" y="1243" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 12</text>
<rect x="165.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="195.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="225.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="255.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="285.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="165.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="178.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="195.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="208.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="225.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="238.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="255.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="268.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="285.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="298.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="342" y="1200" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M370 1225 L387 1225" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 1225 L417 1225" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 1225 L447 1225" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 1225 L490 1225" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 1256 L387 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M413 1256 L417 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M443 1256 L447 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M473 1256 L490 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 1225 L370 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M370 1244 L370 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 1225 L400 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M400 1244 L400 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 1225 L430 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M430 1244 L430 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 1225 L460 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M460 1244 L460 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 1225 L490 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490 1244 L490 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="357" y="1237" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="512" y="1243" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="360" y="1243" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 13</text>
<rect x="357.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="387.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="417.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="447.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="477.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="357.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="370.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="387.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="400.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="417.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="430.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="447.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="460.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="477.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="490.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="534" y="1200" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M562 1225 L579 1225" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 1225 L609 1225" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 1225 L639 1225" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 1225 L682 1225" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 1256 L579 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M605 1256 L609 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M635 1256 L639 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M665 1256 L682 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 1225 L562 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M562 1244 L562 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 1225 L592 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M592 1244 L592 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 1225 L622 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M622 1244 L622 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 1225 L652 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M652 1244 L652 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 1225 L682 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682 1244 L682 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="549" y="1237" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="704" y="1243" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="552" y="1243" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 14</text>
<rect x="549.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="579.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="609.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="639.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">3</text>
<rect x="669.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="549.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="562.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="579.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="592.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="609.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="622.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="639.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="652.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="669.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="682.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<rect x="726" y="1200" width="176" height="78" rx="5" fill="#fdf6e3" stroke="#d97706" stroke-width="1.1"/>
<path d="M857 1225 L874 1225" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 1256 L771 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 1256 L801 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 1256 L831 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M857 1256 L874 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 1225 L754 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M754 1244 L754 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 1225 L784 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M784 1244 L784 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 1225 L814 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M814 1244 L814 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 1225 L844 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 1244 L844 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 1225 L874 1237" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874 1244 L874 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="741" y="1237" width="146" height="7" rx="2" fill="#ede9fe" stroke="#7c3aed" stroke-width="0.8"/>
<text x="896" y="1243" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6" fill="#7c3aed" font-weight="400" text-anchor="end">Router</text>
<text x="744" y="1243" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#16181d" font-weight="700" text-anchor="start">chip 15</text>
<rect x="741.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">0</text>
<rect x="771.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">1</text>
<rect x="801.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">2</text>
<rect x="831.0" y="1216.0" width="26" height="18" rx="3" fill="#fee2e2" stroke="#dc2626" stroke-width="1.2"/>
<text x="844.0" y="1228.5" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#dc2626" font-weight="700" text-anchor="middle">R</text>
<rect x="861.0" y="1216.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="1228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">4</text>
<rect x="741.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="754.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">5</text>
<rect x="771.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="784.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">6</text>
<rect x="801.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="814.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">7</text>
<rect x="831.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="844.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">8</text>
<rect x="861.0" y="1247.0" width="26" height="18" rx="3" fill="#ffffff" stroke="#9aa1ad" stroke-width="0.9"/>
<text x="874.0" y="1259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">9</text>
<path d="M311.0 165 L334.0 165 L334.0 196 L357.0 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M503.0 165 L526.0 165 L526.0 196 L549.0 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M695.0 165 L718.0 165 L718.0 196 L741.0 196" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M311.0 257 L334.0 257 L334.0 288 L357.0 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M503.0 257 L526.0 257 L526.0 288 L549.0 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M695.0 257 L718.0 257 L718.0 288 L741.0 288" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M311.0 349 L334.0 349 L334.0 380 L357.0 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M503.0 349 L526.0 349 L526.0 380 L549.0 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M695.0 349 L718.0 349 L718.0 380 L741.0 380" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M311.0 441 L334.0 441 L334.0 472 L357.0 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M503.0 441 L526.0 441 L526.0 472 L549.0 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M695.0 441 L718.0 441 L718.0 472 L741.0 472" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M311.0 557 L334.0 557 L334.0 588 L357.0 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M503.0 557 L526.0 557 L526.0 588 L549.0 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M695.0 557 L718.0 557 L718.0 588 L741.0 588" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M311.0 649 L334.0 649 L334.0 680 L357.0 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M503.0 649 L526.0 649 L526.0 680 L549.0 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M695.0 649 L718.0 649 L718.0 680 L741.0 680" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M311.0 741 L334.0 741 L334.0 772 L357.0 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M503.0 741 L526.0 741 L526.0 772 L549.0 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M695.0 741 L718.0 741 L718.0 772 L741.0 772" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M311.0 833 L334.0 833 L334.0 864 L357.0 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M503.0 833 L526.0 833 L526.0 864 L549.0 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M695.0 833 L718.0 833 L718.0 864 L741.0 864" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M311.0 949 L334.0 949 L334.0 980 L357.0 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M503.0 949 L526.0 949 L526.0 980 L549.0 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M695.0 949 L718.0 949 L718.0 980 L741.0 980" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M311.0 1041 L334.0 1041 L334.0 1072 L357.0 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M503.0 1041 L526.0 1041 L526.0 1072 L549.0 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M695.0 1041 L718.0 1041 L718.0 1072 L741.0 1072" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M311.0 1133 L334.0 1133 L334.0 1164 L357.0 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M503.0 1133 L526.0 1133 L526.0 1164 L549.0 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M695.0 1133 L718.0 1133 L718.0 1164 L741.0 1164" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M311.0 1225 L334.0 1225 L334.0 1256 L357.0 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M503.0 1225 L526.0 1225 L526.0 1256 L549.0 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M695.0 1225 L718.0 1225 L718.0 1256 L741.0 1256" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298.0 205.0 L298.0 225.0 L178.0 225.0 L178.0 248.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298.0 297.0 L298.0 317.0 L178.0 317.0 L178.0 340.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298.0 389.0 L298.0 409.0 L178.0 409.0 L178.0 432.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298.0 481.0 L298.0 513.0 L178.0 513.0 L178.0 548.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298.0 597.0 L298.0 617.0 L178.0 617.0 L178.0 640.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298.0 689.0 L298.0 709.0 L178.0 709.0 L178.0 732.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298.0 781.0 L298.0 801.0 L178.0 801.0 L178.0 824.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298.0 873.0 L298.0 905.0 L178.0 905.0 L178.0 940.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298.0 989.0 L298.0 1009.0 L178.0 1009.0 L178.0 1032.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298.0 1081.0 L298.0 1101.0 L178.0 1101.0 L178.0 1124.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M298.0 1173.0 L298.0 1193.0 L178.0 1193.0 L178.0 1216.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490.0 205.0 L490.0 225.0 L370.0 225.0 L370.0 248.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490.0 297.0 L490.0 317.0 L370.0 317.0 L370.0 340.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490.0 389.0 L490.0 409.0 L370.0 409.0 L370.0 432.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490.0 481.0 L490.0 513.0 L370.0 513.0 L370.0 548.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490.0 597.0 L490.0 617.0 L370.0 617.0 L370.0 640.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490.0 689.0 L490.0 709.0 L370.0 709.0 L370.0 732.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490.0 781.0 L490.0 801.0 L370.0 801.0 L370.0 824.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490.0 873.0 L490.0 905.0 L370.0 905.0 L370.0 940.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490.0 989.0 L490.0 1009.0 L370.0 1009.0 L370.0 1032.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490.0 1081.0 L490.0 1101.0 L370.0 1101.0 L370.0 1124.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M490.0 1173.0 L490.0 1193.0 L370.0 1193.0 L370.0 1216.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682.0 205.0 L682.0 225.0 L562.0 225.0 L562.0 248.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682.0 297.0 L682.0 317.0 L562.0 317.0 L562.0 340.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682.0 389.0 L682.0 409.0 L562.0 409.0 L562.0 432.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682.0 481.0 L682.0 513.0 L562.0 513.0 L562.0 548.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682.0 597.0 L682.0 617.0 L562.0 617.0 L562.0 640.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682.0 689.0 L682.0 709.0 L562.0 709.0 L562.0 732.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682.0 781.0 L682.0 801.0 L562.0 801.0 L562.0 824.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682.0 873.0 L682.0 905.0 L562.0 905.0 L562.0 940.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682.0 989.0 L682.0 1009.0 L562.0 1009.0 L562.0 1032.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682.0 1081.0 L682.0 1101.0 L562.0 1101.0 L562.0 1124.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M682.0 1173.0 L682.0 1193.0 L562.0 1193.0 L562.0 1216.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874.0 205.0 L874.0 225.0 L754.0 225.0 L754.0 248.0" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-opacity="1.0" stroke-linejoin="round" stroke-linecap="round"/>










<text x="72" y="221.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#16181d" font-weight="700" text-anchor="middle">EP 0</text>
<text x="72" y="235.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#0d9488" font-weight="400" text-anchor="middle">有激活专家</text>
<rect x="146" y="136" width="760" height="178" rx="5" fill="none" stroke="#d97706" stroke-width="0.9" stroke-dasharray="2 3"/>
<path d="M165 148 L887 148" stroke="#0d9488" stroke-width="1.3" fill="none" stroke-opacity="1.0" stroke-dasharray="3 2" marker-end="url(#rcac)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M165 240 L887 240" stroke="#0d9488" stroke-width="1.3" fill="none" stroke-opacity="1.0" stroke-dasharray="3 2" marker-end="url(#rcac)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="72" y="405.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#5c6370" font-weight="700" text-anchor="middle">EP 1</text>
<text x="72" y="419.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">该用户不落此组</text>
<rect x="146" y="320" width="760" height="178" rx="5" fill="none" stroke="#9aa1ad" stroke-width="0.9" stroke-dasharray="2 3"/>
<text x="72" y="613.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#16181d" font-weight="700" text-anchor="middle">EP 2</text>
<text x="72" y="627.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#0d9488" font-weight="400" text-anchor="middle">有激活专家</text>
<rect x="146" y="528" width="760" height="178" rx="5" fill="none" stroke="#d97706" stroke-width="0.9" stroke-dasharray="2 3"/>
<path d="M165 540 L887 540" stroke="#0d9488" stroke-width="1.3" fill="none" stroke-opacity="1.0" stroke-dasharray="3 2" marker-end="url(#rcac)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M165 632 L887 632" stroke="#0d9488" stroke-width="1.3" fill="none" stroke-opacity="1.0" stroke-dasharray="3 2" marker-end="url(#rcac)" stroke-linejoin="round" stroke-linecap="round"/>
<text x="72" y="797.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#5c6370" font-weight="700" text-anchor="middle">EP 3</text>
<text x="72" y="811.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">该用户不落此组</text>
<rect x="146" y="712" width="760" height="178" rx="5" fill="none" stroke="#9aa1ad" stroke-width="0.9" stroke-dasharray="2 3"/>
<text x="72" y="1005.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#5c6370" font-weight="700" text-anchor="middle">EP 4</text>
<text x="72" y="1019.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">该用户不落此组</text>
<rect x="146" y="920" width="760" height="178" rx="5" fill="none" stroke="#9aa1ad" stroke-width="0.9" stroke-dasharray="2 3"/>
<text x="72" y="1189.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#16181d" font-weight="700" text-anchor="middle">EP 5</text>
<text x="72" y="1203.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#0d9488" font-weight="400" text-anchor="middle">有激活专家</text>
<rect x="146" y="1104" width="760" height="178" rx="5" fill="none" stroke="#d97706" stroke-width="0.9" stroke-dasharray="2 3"/>
<path d="M165 1116 L887 1116" stroke="#0d9488" stroke-width="1.3" fill="none" stroke-opacity="1.0" stroke-dasharray="3 2" marker-end="url(#rcac)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M165 1208 L887 1208" stroke="#0d9488" stroke-width="1.3" fill="none" stroke-opacity="1.0" stroke-dasharray="3 2" marker-end="url(#rcac)" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 257 L844 269" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 276 L844 279" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M857 288 L874 288" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874.0 297.0 L874.0 317.0 L754.0 317.0 L754.0 340.0" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M750.5 333.0 L757.5 333.0 L754.0 339.0 z" fill="#7c3aed" stroke="none"/>
<path d="M754 349 L771 349" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 349 L801 349" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 349 L831 349" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 358 L844 361" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 368 L844 371" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M857 380 L874 380" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874.0 389.0 L874.0 409.0 L754.0 409.0 L754.0 432.0" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M750.5 425.0 L757.5 425.0 L754.0 431.0 z" fill="#7c3aed" stroke="none"/>
<path d="M754 441 L771 441" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 441 L801 441" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 441 L831 441" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 450 L844 453" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 460 L844 463" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M857 472 L874 472" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874.0 481.0 L874.0 513.0 L754.0 513.0 L754.0 548.0" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M750.5 541.0 L757.5 541.0 L754.0 547.0 z" fill="#7c3aed" stroke="none"/>
<path d="M754 557 L771 557" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 557 L801 557" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 557 L831 557" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 566 L844 569" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 576 L844 579" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M857 588 L874 588" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874.0 597.0 L874.0 617.0 L754.0 617.0 L754.0 640.0" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M750.5 633.0 L757.5 633.0 L754.0 639.0 z" fill="#7c3aed" stroke="none"/>
<path d="M754 649 L771 649" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 649 L801 649" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 649 L831 649" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 658 L844 661" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 668 L844 671" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M857 680 L874 680" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874.0 689.0 L874.0 709.0 L754.0 709.0 L754.0 732.0" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M750.5 725.0 L757.5 725.0 L754.0 731.0 z" fill="#7c3aed" stroke="none"/>
<path d="M754 741 L771 741" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 741 L801 741" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 741 L831 741" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 750 L844 753" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 760 L844 763" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M857 772 L874 772" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874.0 781.0 L874.0 801.0 L754.0 801.0 L754.0 824.0" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M750.5 817.0 L757.5 817.0 L754.0 823.0 z" fill="#7c3aed" stroke="none"/>
<path d="M754 833 L771 833" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 833 L801 833" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 833 L831 833" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 842 L844 845" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 852 L844 855" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M857 864 L874 864" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874.0 873.0 L874.0 905.0 L754.0 905.0 L754.0 940.0" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M750.5 933.0 L757.5 933.0 L754.0 939.0 z" fill="#7c3aed" stroke="none"/>
<path d="M754 949 L771 949" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 949 L801 949" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 949 L831 949" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 958 L844 961" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 968 L844 971" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M857 980 L874 980" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874.0 989.0 L874.0 1009.0 L754.0 1009.0 L754.0 1032.0" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M750.5 1025.0 L757.5 1025.0 L754.0 1031.0 z" fill="#7c3aed" stroke="none"/>
<path d="M754 1041 L771 1041" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 1041 L801 1041" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 1041 L831 1041" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 1050 L844 1053" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 1060 L844 1063" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M857 1072 L874 1072" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874.0 1081.0 L874.0 1101.0 L754.0 1101.0 L754.0 1124.0" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M750.5 1117.0 L757.5 1117.0 L754.0 1123.0 z" fill="#7c3aed" stroke="none"/>
<path d="M754 1133 L771 1133" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 1133 L801 1133" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 1133 L831 1133" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 1142 L844 1145" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M844 1152 L844 1155" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M857 1164 L874 1164" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M874.0 1173.0 L874.0 1193.0 L754.0 1193.0 L754.0 1216.0" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M750.5 1209.0 L757.5 1209.0 L754.0 1215.0 z" fill="#7c3aed" stroke="none"/>
<path d="M754 1225 L771 1225" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M797 1225 L801 1225" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<path d="M827 1225 L844 1225" stroke="#7c3aed" stroke-width="2.6" fill="none" stroke-opacity="0.6" stroke-linejoin="round" stroke-linecap="round"/>
<text x="746.0" y="320.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#7c3aed" font-weight="700" text-anchor="end">第一组，直接发出</text>
<text x="746.0" y="516.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#7c3aed" font-weight="700" text-anchor="end">不落，透传</text>
<text x="746.0" y="712.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#7c3aed" font-weight="700" text-anchor="end">落本组，加后发出</text>
<text x="746.0" y="908.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#7c3aed" font-weight="700" text-anchor="end">不落，透传</text>
<text x="746.0" y="1104.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#7c3aed" font-weight="700" text-anchor="end">不落，透传</text>
<text x="844.0" y="429.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#7c3aed" font-weight="400" text-anchor="middle">过而不加</text>
<text x="844.0" y="821.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#7c3aed" font-weight="400" text-anchor="middle">过而不加</text>
<text x="844.0" y="1029.0" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#7c3aed" font-weight="400" text-anchor="middle">过而不加</text>
<rect x="930" y="160" width="250" height="96" rx="6" fill="#fdeed8" stroke="#d97706" stroke-width="1.3"/>
<text x="942" y="179" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#d97706" font-weight="700" text-anchor="start">Dispatcher</text>
<text x="942" y="198" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">派遣前查所有 R core 余量，</text>
<text x="942" y="213" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">都有余量才派遣</text>
<text x="942" y="228" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">派遣时每个 R core 余量 −1</text>
<text x="942" y="243" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">链尾 R core 反向通知（橙虚线）：</text>
<text x="942" y="258" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">该用户完成，所有 R core 余量 +1</text>
<rect x="930" y="280" width="250" height="128" rx="6" fill="#f6f1ff" stroke="#7c3aed" stroke-width="1.3"/>
<text x="942" y="299" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#7c3aed" font-weight="700" text-anchor="start">每一跳只有两种动作</text>
<text x="942" y="318" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">reduction 数据到 R core 后先判用户是否落本组：</text>
<text x="942" y="333" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">落本组 → 进 Matrix Mem，等与本组</text>
<text x="942" y="348" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">　reduce 数据相加后再发出</text>
<text x="942" y="363" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">不落本组 → 跳过，沿链向下游透传</text>
<text x="942" y="378" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">该用户的第一组：本组结果直接发出</text>
<text x="942" y="393" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">链尾组：累加完从东口出核</text>
<rect x="930" y="432" width="250" height="112" rx="6" fill="#f5f6f8" stroke="#16181d" stroke-width="1.3"/>
<text x="942" y="451" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#16181d" font-weight="700" text-anchor="start">两种数据，名字相近</text>
<text x="942" y="470" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">reduce 数据（青）：本组 TP 逐跳 reduce</text>
<text x="942" y="485" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">　出来的组内结果</text>
<text x="942" y="500" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">reduction 数据（紫）：沿链在 EP 组之间</text>
<text x="942" y="515" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">　流动的累加中间结果</text>
<text x="942" y="530" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">R core 不分配权重，不参与 TP 与 FFN 计算</text>
<rect x="930" y="568" width="250" height="128" rx="6" fill="#ffffff" stroke="#5c6370" stroke-width="1.3"/>
<text x="942" y="587" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11.5" fill="#5c6370" font-weight="700" text-anchor="start">路线</text>
<text x="942" y="606" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">紫粗线：reduction 数据沿最后一列走：</text>
<text x="942" y="621" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">　北口 core0 进 → 上排到 R core（core3）</text>
<text x="942" y="636" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">　→ core8 → 南口 core9 出 → 下一颗 chip</text>
<text x="942" y="651" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">　不落本组的组，R core 过而不加</text>
<text x="942" y="666" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">青虚线：组内 reduce 数据汇到 R core</text>
<text x="942" y="681" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#5c6370" font-weight="400" text-anchor="start">B：B core（core5），token 由此进各组</text>
<path d="M902 1256 L916 1256 L916 208 L929 208" stroke="#d97706" stroke-width="1.4" fill="none" stroke-opacity="1.0" stroke-dasharray="5 3" marker-end="url(#rcar)" stroke-linejoin="round" stroke-linecap="round"/>
<rect x="480" y="1318" width="140" height="40" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="550" y="1343" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="600" text-anchor="middle">DPU DDR</text>
<rect x="300" y="1318" width="140" height="40" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="370" y="1343" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="600" text-anchor="middle">PCIe Switch</text>
<rect x="120" y="1318" width="140" height="40" rx="7" fill="#eceef1" stroke="#6b7280" stroke-width="1.4"/>
<text x="190" y="1343" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="12" fill="#6b7280" font-weight="600" text-anchor="middle">GPU（下一层）</text>
<path d="M480 1338 L440.7 1338" stroke="#6b7280" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rca)"/>
<path d="M300 1338 L260.7 1338" stroke="#6b7280" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rca)"/>
<path d="M887 1225 L928 1225 L928 1338 L620.7 1338" stroke="#d97706" stroke-width="2.0" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rcar)"/>
<text x="934" y="1229" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#d97706" font-weight="400" text-anchor="start">东口出核</text>
<text x="370" y="1372" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#5c6370" font-weight="400" text-anchor="middle">链尾只此一处出核：60 GB/s、2 个 ETH 口</text>
<rect x="30" y="1386" width="1140" height="94" rx="5" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.2"/>
<text x="46" y="1403" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">全图 3 tray × 4 层 × 4 chip = 48 chip；黄底是该用户有激活专家的组，灰底是不落的组。紫字是每一跳的动作：第一组直接发出、落本组加后发出、不落透传。</text>
<text x="46" y="1419" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">chip 内 2×5 core 按 row-major 编号，Router 居中，相邻 core 之间有链路；chip 之间经边界 core 相连：core0 = 北口、core4 = 东口、core5 = 西口、core9 = 南口。坏核未画。</text>
<text x="46" y="1435" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">串成链换来的是出口带宽与 EP 组数解耦：各组各自出核要 60 GB/s × 6 = 360 GB/s、9 个 ETH 口；串成链只有链尾一处 60 GB/s、2 个口。每一跳搬的都是同一份累加中间结果，加 EP 组不会让任何一跳变宽。</text>
<text x="46" y="1451" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">代价是整条链成了一个整体：任何一组把 reduce-buffer 占满，链上游全部停住，所以派遣必须保守，用户不落的组也要确认余量。</text>
<text x="46" y="1467" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">与第二层的差别：这里每一跳要等两笔齐了再加（用户之间乱序），不是流着加；缓冲是 32 MB 的 Matrix Mem，不是 ReduceModule 的 16 × 16 KiB。R 借 core 3 是《系统与部署》的暂定（Core3 / Core7）。</text>
</svg>
```

每个 EP 组借出口处的一个 Bach core 作 R core（暂定 Core3 / Core7，图上画在组内最后一列下层 chip 的 core 3）。R core 不分配权重，不参与 TP 与 FFN 计算。各 EP 组的 R core 串成一条链，一个用户的结果沿链逐组累加，只在链尾出核一次。

这条通路上有两种数据，名字相近但不是一回事：

* **reduce 数据**：本组 TP 内逐级 reduce 出来的组内结果，从相邻上游 core 的 Router 进来
* **reduction 数据**：沿链在 EP 组之间流动的累加中间结果，从上一组的 R core 进来

每一跳只有两种动作：

1. R core 的 Router 收下相邻上游 core 的 reduce 数据，存进本 core 的 Matrix Mem
2. 当前 EP 组是该用户的第一组：累加后经 reduction 通路发往下一组 R core
3. reduction 数据到达 R core 后判断该用户是否落在当前组：**落在本组就进 Matrix Mem，等与本组的 reduce 数据累加后再发出；不落在本组就跳过，沿通路继续向下游透传**
4. 走到链尾节点累加完成，经 result 输出通路送到 PCIe Switch，进 SNIC DPU 的 DDR，回 GPU

串成链换来的是出口带宽与 EP 组数解耦：

| | 各组各自出核 | 串成一条链 |
| - | - | - |
| 出口在哪 | 每个激活组各出一份 | 只有链尾一处 |
| 出口带宽 | 随组数线性涨，EP6 下 60 GB/s × 6 = 360 GB/s | 60 GB/s |
| ETH 口数 | LPU 侧 9 个，GPU 侧也要 9 个 | 2 个 |

每一跳搬的都是同一份累加中间结果，加 EP 组不会让任何一跳变宽。代价是整条链成了一个整体：任何一组把 reduce-buffer 占满，链上游全部停住。

### R core 内部

```svg
<svg viewBox="0 0 1100 640" width="1100" height="640" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="R core 内部：两条任务链，两笔数据按用户对齐">
<title>R core 内部：两条任务链，两笔数据按用户对齐</title>
<rect width="1100" height="640" fill="#ffffff"/>
<defs><marker id="rfa" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="rfas" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker><marker id="rfai" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#3f4451"/></marker><marker id="rfais" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#3f4451"/></marker><marker id="rfab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="rfabs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="rfar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="rfars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="rfac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="rfacs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="rfap" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="rfaps" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker></defs>
<text x="30" y="26" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="start">R core 内部：两条任务链，两笔数据按用户对齐</text>
<text x="30" y="46" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="start">链一由 Router 触发，只负责把到达的一笔存进 Matrix Mem 并记账；链二自启动，扫到哪个用户两笔齐了就把它算完送走。谁先集齐谁先走，与到达顺序无关</text>
<rect x="40" y="90" width="150" height="90" rx="5" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.3"/>
<text x="48" y="105" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#7c3aed" font-weight="700" text-anchor="start">Router</text>
<text x="48" y="119" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">两个来向的数据：</text>
<text x="48" y="131" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">① 本组 TP 的 reduce 数据（相邻上游 core）</text>
<text x="48" y="143" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">② 上游 EP 组的 reduction 数据</text>
<text x="48" y="155" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">收满一个包 → trigger TS</text>
<text x="115" y="304" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#7c3aed" font-weight="400" text-anchor="middle">要求搬运原子化：一个方向一个用户</text>
<text x="115" y="316" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#7c3aed" font-weight="400" text-anchor="middle">全部搬进 Matrix Mem 才搬别的</text>
<rect x="40" y="230" width="150" height="60" rx="5" fill="#dbeafe" stroke="#2563eb" stroke-width="1.3"/>
<text x="48" y="245" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#2563eb" font-weight="700" text-anchor="start">TS</text>
<text x="48" y="259" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">链一每来一包激活一次</text>
<text x="48" y="271" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">链二自启动，stream 复位后占满</text>
<path d="M115 180 L115 229.3" stroke="#d97706" stroke-width="1.4" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rfar)"/>
<text x="122" y="210" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#d97706" font-weight="400" text-anchor="start">trigger</text>
<rect x="220" y="80" width="400" height="130" rx="8" fill="#fffbeb" stroke="#d97706" stroke-width="1.2"/>
<text x="420" y="98" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#d97706" font-weight="700" text-anchor="middle">链一：Router → Matrix Mem（Router 触发）</text>
<rect x="232" y="110" width="180" height="88" rx="5" fill="#ffffff" stroke="#d97706" stroke-width="1.3"/>
<text x="240" y="125" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">task 0　DTE</text>
<text x="240" y="139" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">软件读 arrive_num[gpu_id][token_id]</text>
<text x="240" y="151" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">第一笔：新分配 Matrix Mem 空间、建映射表项</text>
<text x="240" y="163" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">第二笔：写进已开好的空间</text>
<text x="240" y="175" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">记 tmp_info0[stream_id] = (gpu_id, token_id)</text>
<text x="240" y="187" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">把地址配给 DTE DSA，Router → Matrix Mem</text>
<rect x="428" y="110" width="180" height="40" rx="5" fill="#ffffff" stroke="#d97706" stroke-width="1.3"/>
<text x="436" y="125" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">task 1　DTE</text>
<text x="436" y="139" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">arrive_num[gpu_id][token_id] ++</text>
<path d="M412 130 L427.3 130" stroke="#16181d" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rfai)"/>
<text x="518" y="175" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">两个方向对应两个不同的 stream</text>
<text x="518" y="188" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">链一与链二不共用 stream_id</text>
<rect x="220" y="230" width="860" height="150" rx="8" fill="#eef4ff" stroke="#2563eb" stroke-width="1.2"/>
<text x="650" y="248" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#2563eb" font-weight="700" text-anchor="middle">链二：求和并送出（自启动）</text>
<rect x="232" y="260" width="190" height="100" rx="5" fill="#ffffff" stroke="#2563eb" stroke-width="1.3"/>
<text x="240" y="275" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">task 0　MU</text>
<text x="240" y="289" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">循环扫 arrive_num 找 == 2 的项</text>
<text x="240" y="301" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">找不到就继续等</text>
<text x="240" y="313" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">找到：清零，把 (gpu_id, token_id)</text>
<text x="240" y="325" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">写进 tmp_info1[stream_id]，报完成</text>
<text x="240" y="337" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">（可顺手再创建一个新 stream）</text>
<rect x="436" y="260" width="190" height="100" rx="5" fill="#ffffff" stroke="#2563eb" stroke-width="1.3"/>
<text x="444" y="275" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">task 1　DTE</text>
<text x="444" y="289" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">按 tmp_info1[stream_id] 把两笔</text>
<text x="444" y="301" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">从 Matrix Mem 搬到 Core Mem</text>
<text x="444" y="313" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start"></text>
<text x="444" y="325" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">备选：不在 VU 加，DTE 把第二笔</text>
<text x="444" y="337" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">搬到 Router 的 ReduceModule 加完直接送出</text>
<rect x="640" y="260" width="190" height="100" rx="5" fill="#ffffff" stroke="#2563eb" stroke-width="1.3"/>
<text x="648" y="275" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">task 2　VU</text>
<text x="648" y="289" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">两笔求和，结果写回 Core Mem</text>
<text x="648" y="301" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start"></text>
<text x="648" y="313" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">MU 已在计算 core 把本组</text>
<text x="648" y="325" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">各专家的结果合成一笔，</text>
<text x="648" y="337" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">这里只加两笔</text>
<rect x="844" y="260" width="224" height="100" rx="5" fill="#ffffff" stroke="#2563eb" stroke-width="1.3"/>
<text x="852" y="275" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">task 3　DTE</text>
<text x="852" y="289" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">结果 Core Mem → Router</text>
<text x="852" y="301" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">不是链尾组：送下一组 EP 的 R core</text>
<text x="852" y="313" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">链尾组：经 PCIe Switch 进 SNIC DPU DDR，</text>
<text x="852" y="325" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">地址由 (gpu_id, token_id) 算出</text>
<text x="852" y="337" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="start">链结束：释放 stream 与 Matrix Mem，通知 Router 更新 credit</text>
<path d="M422 310 L435.3 310" stroke="#16181d" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rfai)"/>
<path d="M626 310 L639.3 310" stroke="#16181d" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rfai)"/>
<path d="M830 310 L843.3 310" stroke="#16181d" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rfai)"/>
<rect x="220" y="400" width="300" height="90" rx="5" fill="#fdeed8" stroke="#d97706" stroke-width="1.3"/>
<text x="228" y="415" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#d97706" font-weight="700" text-anchor="start">Share Mem 里的三张表</text>
<text x="228" y="429" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">arrive_num[gpu_id][token_id]：已到几笔，0 → 1 → 2</text>
<text x="228" y="441" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">tmp_info0[stream_id]：链一本次搬运对应的 (gpu_id, token_id)</text>
<text x="228" y="453" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">tmp_info1[stream_id]：链二本次要算的 (gpu_id, token_id)</text>
<rect x="540" y="400" width="300" height="90" rx="5" fill="#fdeed8" stroke="#d97706" stroke-width="1.3"/>
<text x="548" y="415" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#d97706" font-weight="700" text-anchor="start">Matrix Mem 32 MB</text>
<text x="548" y="429" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">按 (gpu_id, token_id) 用软件映射表定位，乱序分配、乱序释放</text>
<text x="548" y="441" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">不是 B core 那种一对 head / tail 指针的顺序 FIFO</text>
<text x="548" y="453" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">每笔 12 KiB（6144 个 BF16）→ 约 1300 个用户的半成品同时挂着</text>
<rect x="860" y="400" width="208" height="90" rx="5" fill="#ffffff" stroke="#c9ced6" stroke-width="1.3"/>
<text x="868" y="415" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#16181d" font-weight="700" text-anchor="start">为什么两条链</text>
<text x="868" y="429" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">拆开“在 TS 占一个 stream 项”</text>
<text x="868" y="441" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">与“数据在本核停留”两段时间：</text>
<text x="868" y="453" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">在途用户上限从 16 项换成 Matrix Mem 容量，</text>
<text x="868" y="465" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">推进顺序由链二自己挑，不受到达顺序约束</text>
<rect x="30" y="510" width="1040" height="46" rx="5" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.2"/>
<text x="46" y="527" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">两笔的来向：本组 TP 逐跳 reduce 的结果从相邻上游 core 的 Router 进来；上游 EP 组的 reduction 数据从行间通路进来。到达顺序完全不定，所以 (g0, t8) 的第一笔比 (g0, t7) 晚到，却可能先集齐、先被链二取走。</text>
<text x="46" y="543" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">对 TS 的要求：token 与 reduction 结果都能注册 stream；支持乱序，谁先集齐谁先算；R core 的 stream 映射到 Matrix Mem 而不是 16 项的 Core Mem。对 DTE 的要求：新增 Matrix Mem → Core Mem（或 → Router）的搬运。</text>
</svg>
```

R core 每个用户要等两笔数据，两笔从两个方向来，用户之间到达顺序完全不定。B core 那种一对 head / tail 指针的顺序 FIFO 在这里会把先集齐的用户堵在后面，所以 R core 用软件映射表按 `(gpu_id, token_id)` 定位 Matrix Mem 空间，乱序分配、乱序释放，谁先集齐谁先走。

Share Mem 里的三张表：

| 结构 | 作用 |
| - | - |
| `arrive_num[gpu_id][token_id]` | 该用户已到几笔数据，初值 0，到 2 表示齐了 |
| `tmp_info0[stream_id]` | 链一用：本次搬运对应的 `(gpu_id, token_id)`，从 task 0 传给 task 1 |
| `tmp_info1[stream_id]` | 链二用：本次要算的 `(gpu_id, token_id)`，从 task 0 传给后面的搬运与计算 |

**链一：Router → Matrix Mem**，由 Router 触发，每来一包激活一次。

| task | 执行单元 | 内容 |
| - | - | - |
| 0 | DTE | 软件读 `arrive_num`，结合 `(gpu_id, token_id)` 算出 Matrix Mem 存放位置配给 DTE DSA：是这个用户的第一笔就新分配空间并建映射表项，是第二笔就写进已开好的空间。同时记下 `tmp_info0[stream_id]` |
| 1 | DTE | `arrive_num[gpu_id][token_id]++` |

**链二：求和并送出**，自启动，stream 复位后就占满。

| task | 执行单元 | 内容 |
| - | - | - |
| 0 | MU | 循环扫 `arrive_num` 找 == 2 的项。找不到就继续等；找到则把该项清零、把 `(gpu_id, token_id)` 写进 `tmp_info1[stream_id]`，向 TS 报完成（可以顺手再创建一个新 stream） |
| 1 | DTE | 按 `tmp_info1[stream_id]` 把两笔数据从 Matrix Mem 搬到 Core Mem |
| 2 | VU | 两笔求和，结果写回 Core Mem |
| 3 | DTE | 结果从 Core Mem 搬到 Router。不是链尾组就送往下一组 EP 的 R core；是链尾组就经 PCIe Switch 送进 SNIC DPU 的 DDR，目的地址由 `(gpu_id, token_id)` 算出。任务链结束，释放 stream 与 Matrix Mem 空间，并通知 Router 更新本 core 的 credit |

两条链用的不是同一套 stream_id；链一的两个方向也分别对应两个不同的 stream。两条链拆开的是“在 TS 里占一个 stream 项”与“数据在本核停留”两段时间：在途用户上限从 stream_table 的 16 项换成 Matrix Mem 的容量，按每笔 12 KiB（6144 个 BF16）算，32 MB 能同时挂着约 1300 个用户的半成品；推进顺序由链二自己挑，不受到达顺序约束。

链二的 task 1 与 task 2 有一个备选：不在 VU 加，两笔先到的存 Matrix Mem，另一笔到达后 DTE 把它搬到 Router 的 ReduceModule 加完直接向下游输出。

### 对 Router、TS、DTE 的要求

| 模块 | 要求 |
| - | - |
| Router | 收到 reduce / reduction 数据后通知 TS 下发 DTE 搬运任务；**搬运要原子化**，某个方向的用户数据全部搬进 Matrix Mem 后才能搬别的方向或别的用户。这与第二层要求交织（每个方向进来 128 B 就往后传）正好相反 |
| TS | R core 配与普通 core 不同的任务链；新增要 DTE 激活才能下发的 task 类型；stream 映射到 Matrix Mem 而不是 16 项的 Core Mem；支持乱序，哪个用户的数据先集齐就先算 |
| DTE | 新增 Matrix Mem → Core Mem（或 → Router）的搬运方向 |

### 死锁与派遣

```svg
<svg viewBox="0 0 1100 440" width="1100" height="440" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="EP reduction 的死锁与解法：派遣前预留所有 R core 的余量">
<title>EP reduction 的死锁与解法：派遣前预留所有 R core 的余量</title>
<rect width="1100" height="440" fill="#ffffff"/>
<defs><marker id="rga" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="rgas" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker><marker id="rgai" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#3f4451"/></marker><marker id="rgais" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#3f4451"/></marker><marker id="rgab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="rgabs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="rgar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="rgars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="rgac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="rgacs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="rgap" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="rgaps" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker></defs>
<text x="30" y="26" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="start">EP reduction 的死锁与解法：派遣前预留所有 R core 的余量</text>
<text x="30" y="46" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="start">若每批用户都能占满 R core 的 reduce-buffer，逐级 reduction 会死锁；选定的解法把保序放在调度侧，硬件只需上报释放</text>
<rect x="40" y="70" width="330" height="220" rx="8" fill="#fff1f2" stroke="#dc2626" stroke-width="1.2"/>
<text x="205" y="90" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#dc2626" font-weight="700" text-anchor="middle">卡死场景</text>
<rect x="60" y="106" width="200" height="36" rx="4" fill="#ffffff" stroke="#dc2626" stroke-width="1.0"/>
<text x="160" y="121" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#dc2626" font-weight="700" text-anchor="middle">R core 0</text>
<text x="160" y="135" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="middle">buffer 被批次 A 占满，等批次 B 的另一笔</text>
<path d="M160 142 L160 157.3" stroke="#dc2626" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rgar)"/>
<rect x="60" y="158" width="200" height="36" rx="4" fill="#ffffff" stroke="#dc2626" stroke-width="1.0"/>
<text x="160" y="173" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#dc2626" font-weight="700" text-anchor="middle">R core 1</text>
<text x="160" y="187" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="middle">buffer 被批次 A 占满，等批次 B 的另一笔</text>
<path d="M160 194 L160 209.3" stroke="#dc2626" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rgar)"/>
<rect x="60" y="210" width="200" height="36" rx="4" fill="#ffffff" stroke="#dc2626" stroke-width="1.0"/>
<text x="160" y="225" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#dc2626" font-weight="700" text-anchor="middle">R core 2</text>
<text x="160" y="239" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7" fill="#5c6370" font-weight="400" text-anchor="middle">批次 B 的这一笔等上游放行</text>
<text x="310" y="160" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9" fill="#dc2626" font-weight="700" text-anchor="middle">互相等</text>
<text x="205" y="278" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">每个色块是同一批用户，且能占满一个 R core 的资源</text>
<rect x="400" y="70" width="320" height="100" rx="5" fill="#f5f6f8" stroke="#c9ced6" stroke-width="1.3"/>
<text x="408" y="85" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#16181d" font-weight="700" text-anchor="start">方案 1　硬同步（通信保序）</text>
<text x="408" y="99" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">上级 R core 必须等同一用户到达下级、收到下级通知后才向下传播</text>
<text x="408" y="111" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">对顶层调度无要求；最差要首尾 R core 直接通信，同步机制复杂</text>
<text x="560" y="160" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="middle">未选</text>
<rect x="400" y="190" width="320" height="100" rx="5" fill="#fdeed8" stroke="#d97706" stroke-width="1.3"/>
<text x="408" y="205" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#d97706" font-weight="700" text-anchor="start">方案 2　动态调度（调度保序）　选定</text>
<text x="408" y="219" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">调度核派遣前检查所有 EP 的 R core 余量都充足才派遣</text>
<text x="408" y="231" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">硬件只需实现 reduce-buffer 释放的反馈通路，R core 无反压逻辑</text>
<text x="408" y="243" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="start">代价：调度核压力大，只能顺序调度</text>
<rect x="750" y="70" width="320" height="220" rx="8" fill="#fffbeb" stroke="#d97706" stroke-width="1.2"/>
<text x="910" y="90" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="11" fill="#d97706" font-weight="700" text-anchor="middle">LPU Dispatch 派遣规则</text>
<text x="762" y="114" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="400" text-anchor="start">① 派遣一个用户时统一分配所有 R core 的资源，不区分 EP 组</text>
<text x="762" y="136" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="400" text-anchor="start">② 所有 R core 都有余量时才派遣</text>
<text x="762" y="158" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="400" text-anchor="start">③ 派遣时每个 R core 余量 −1</text>
<text x="762" y="180" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#16181d" font-weight="400" text-anchor="start">④ 链尾 R core 返回后，所有 R core 余量 +1</text>
<text x="762" y="202" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8.5" fill="#5c6370" font-weight="400" text-anchor="start">用户不需要的 EP 也要求余量充足，因此只会顺序调度</text>
<rect x="770" y="232" width="40" height="22" rx="3" fill="#ffffff" stroke="#d97706" stroke-width="1.0"/>
<text x="790" y="247" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="600" text-anchor="middle">R0: n</text>
<rect x="820" y="232" width="40" height="22" rx="3" fill="#ffffff" stroke="#d97706" stroke-width="1.0"/>
<text x="840" y="247" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="600" text-anchor="middle">R1: n</text>
<rect x="870" y="232" width="40" height="22" rx="3" fill="#ffffff" stroke="#d97706" stroke-width="1.0"/>
<text x="890" y="247" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="600" text-anchor="middle">R2: n</text>
<rect x="920" y="232" width="40" height="22" rx="3" fill="#ffffff" stroke="#d97706" stroke-width="1.0"/>
<text x="940" y="247" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="600" text-anchor="middle">R3: n</text>
<rect x="970" y="232" width="40" height="22" rx="3" fill="#ffffff" stroke="#d97706" stroke-width="1.0"/>
<text x="990" y="247" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="600" text-anchor="middle">R4: n</text>
<rect x="1020" y="232" width="40" height="22" rx="3" fill="#ffffff" stroke="#d97706" stroke-width="1.0"/>
<text x="1040" y="247" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="8" fill="#d97706" font-weight="600" text-anchor="middle">R5: n</text>
<text x="910" y="278" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.5" fill="#5c6370" font-weight="400" text-anchor="middle">dispatcher 里每个 R core 一个余量计数</text>
<rect x="30" y="310" width="1040" height="78" rx="5" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.2"/>
<text x="46" y="327" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">完成的通知走反向：最后一个执行 reduction 的 R core 反向传播通知 dispatcher（选定），不经 output_cpu / input_cpu 转手。</text>
<text x="46" y="343" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">调度核是谁：bach core 做 dispatcher 时用 mesh 逐点传播，每次派遣统一分配所有 EP 的 R core 空间，只能顺序调度；</text>
<text x="46" y="359" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">若 slave CPU 做 dispatcher，可实现多播加乱序调度，需要 CPU 对每个 EP 组的 reduction 资源做管理并有 credit 回报。</text>
<text x="46" y="375" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">R core 的 buffer 是 32 MB Matrix Mem，大概率已避免卡死；上面的机制处理仍然卡死的情况。</text>
</svg>
```

若每批用户都能占满 R core 的 reduce-buffer，逐级 reduction 会死锁。R core 的 buffer 用 32 MB 的 Matrix Mem，大概率已避免；仍然卡死时由调度侧解：

| 方案 | 做法 | 优缺点 |
| - | - | - |
| 硬同步（通信保序） | 上级 R core 必须等同一用户到达下级、收到下级通知后才向下传播 | 对顶层调度无要求；最差要首尾 R core 直接通信，同步机制复杂 |
| **动态调度（调度保序），选定** | 调度核在派遣前检查所有 EP 的 R core 余量都充足才派遣 | 硬件只需实现 reduce-buffer 释放的反馈通路，R core 无反压逻辑；调度核压力大，只能顺序调度 |

LPU Dispatch 的派遣规则：

1. 作为 dispatcher 的 Bach core 派遣一个用户时，**统一分配所有 R core 的资源，不区分 EP 组**
2. 所有 R core 都有余量时才派遣
3. 派遣时每个 R core 余量减 1
4. 等链尾 R core 返回后，所有 R core 余量加 1

即使用户不需要的 EP 也要求余量充足，因此只会顺序调度。完成的通知走反向：最后一个执行 reduction 的 R core 反向传播通知 dispatcher，不经 output_cpu / input_cpu 转手。若改由 slave CPU 做 dispatcher，可实现多播加乱序调度，代价是 CPU 要对每个 EP 组的 reduction 资源做管理并有 credit 回报。

***

## 一个 token 的三层归约时间线

```svg
<svg viewBox="0 0 1100 560" width="1100" height="560" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="一个 token 的三层归约时间线（EP6 + TP8，chip 内切 K 的 nk 模式）">
<title>一个 token 的三层归约时间线（EP6 + TP8，chip 内切 K 的 nk 模式）</title>
<rect width="1100" height="560" fill="#ffffff"/>
<defs><marker id="rha" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#9aa1ad"/></marker><marker id="rhas" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#9aa1ad"/></marker><marker id="rhai" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#3f4451"/></marker><marker id="rhais" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#3f4451"/></marker><marker id="rhab" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#2563eb"/></marker><marker id="rhabs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#2563eb"/></marker><marker id="rhar" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#d97706"/></marker><marker id="rhars" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#d97706"/></marker><marker id="rhac" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#0d9488"/></marker><marker id="rhacs" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#0d9488"/></marker><marker id="rhap" viewBox="0 0 10 8" refX="9" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M0 0 L10 4 L0 8 z" fill="#7c3aed"/></marker><marker id="rhaps" viewBox="0 0 10 8" refX="1" refY="4" markerWidth="7" markerHeight="6" orient="auto"><path d="M10 0 L0 4 L10 8 z" fill="#7c3aed"/></marker></defs>
<text x="30" y="26" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="15" fill="#16181d" font-weight="700" text-anchor="start">一个 token 的三层归约时间线（EP6 + TP8，chip 内切 K 的 nk 模式）</text>
<text x="30" y="46" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10" fill="#5c6370" font-weight="400" text-anchor="start">从进 EP 组到链尾出核，归约出现在 ③ ⑤ ⑦ ⑨ 四处；蓝色是第二层（Router 流着加），青色是第一层（MU 就地加），紫色是第三层（R core 等齐再加）</text>
<rect x="30" y="70" width="1040" height="52" rx="4" fill="#fbfcfd" stroke="#9aa1ad" stroke-width="0.8"/>
<text x="40" y="100" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">GPU / DPU</text>
<rect x="30" y="130" width="1040" height="52" rx="4" fill="#fbfcfd" stroke="#9aa1ad" stroke-width="0.8"/>
<text x="40" y="160" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">B core</text>
<rect x="30" y="190" width="1040" height="52" rx="4" fill="#fbfcfd" stroke="#9aa1ad" stroke-width="0.8"/>
<text x="40" y="220" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">计算 core（64 个）</text>
<rect x="30" y="250" width="1040" height="52" rx="4" fill="#fbfcfd" stroke="#9aa1ad" stroke-width="0.8"/>
<text x="40" y="280" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">Router ReduceModule</text>
<rect x="30" y="310" width="1040" height="52" rx="4" fill="#fbfcfd" stroke="#9aa1ad" stroke-width="0.8"/>
<text x="40" y="340" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">R core</text>
<rect x="30" y="370" width="1040" height="52" rx="4" fill="#fbfcfd" stroke="#9aa1ad" stroke-width="0.8"/>
<text x="40" y="400" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="9.5" fill="#16181d" font-weight="700" text-anchor="start">EP 链</text>
<rect x="108" y="76" width="124" height="40" rx="5" fill="#eceef1" stroke="#6b7280" stroke-width="1.1"/>
<text x="170" y="91" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.8" fill="#6b7280" font-weight="700" text-anchor="middle">① token 进 Node</text>
<text x="170" y="106" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#5c6370" font-weight="400" text-anchor="middle">DPU 加 gpu_id / token_id 包头</text>
<rect x="238" y="136" width="124" height="40" rx="5" fill="#fdeed8" stroke="#d97706" stroke-width="1.1"/>
<text x="300" y="151" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.8" fill="#d97706" font-weight="700" text-anchor="middle">② 广播进本组</text>
<text x="300" y="166" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#5c6370" font-weight="400" text-anchor="middle">B core 查所有目的 core 余量后发一次</text>
<rect x="368" y="196" width="124" height="40" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.1"/>
<text x="430" y="211" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.8" fill="#0d9488" font-weight="700" text-anchor="middle">③ FC1 / FC3</text>
<text x="430" y="226" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#5c6370" font-weight="400" text-anchor="middle">chip 内切 K → 每 core 一份部分和</text>
<rect x="498" y="256" width="124" height="40" rx="5" fill="#dbeafe" stroke="#2563eb" stroke-width="1.1"/>
<text x="560" y="271" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.8" fill="#2563eb" font-weight="700" text-anchor="middle">③′ 逐跳 reduce 汇到 dot core</text>
<text x="560" y="286" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#5c6370" font-weight="400" text-anchor="middle">ReduceModule 流着加，FP32</text>
<rect x="628" y="196" width="124" height="40" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.1"/>
<text x="690" y="211" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.8" fill="#0d9488" font-weight="700" text-anchor="middle">④ silu · dot · 量化 → 广播回</text>
<text x="690" y="226" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#5c6370" font-weight="400" text-anchor="middle">dot core 做完再广播给 8 个 core</text>
<rect x="758" y="196" width="124" height="40" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.1"/>
<text x="820" y="211" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.8" fill="#0d9488" font-weight="700" text-anchor="middle">⑤ FC2 + 专家间求和</text>
<text x="820" y="226" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#5c6370" font-weight="400" text-anchor="middle">MU：C += (A×B)×W_ep，就地加</text>
<rect x="888" y="196" width="124" height="40" rx="5" fill="#ccfbf1" stroke="#0d9488" stroke-width="1.1"/>
<text x="950" y="211" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.8" fill="#0d9488" font-weight="700" text-anchor="middle">⑥ chip 内 concat</text>
<text x="950" y="226" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#5c6370" font-weight="400" text-anchor="middle">FC2 chip 内切 N，拼接不归约</text>
<rect x="238" y="256" width="124" height="40" rx="5" fill="#dbeafe" stroke="#2563eb" stroke-width="1.1"/>
<text x="300" y="271" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.8" fill="#2563eb" font-weight="700" text-anchor="middle">⑦ chip 间 FC2 切 K 的逐跳 reduce</text>
<text x="300" y="286" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#5c6370" font-weight="400" text-anchor="middle">8 个 chip 沿组流着加 → 本组结果</text>
<rect x="498" y="316" width="124" height="40" rx="5" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.1"/>
<text x="560" y="331" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.8" fill="#7c3aed" font-weight="700" text-anchor="middle">⑧ 进 R core（reduce 数据）</text>
<text x="560" y="346" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#5c6370" font-weight="400" text-anchor="middle">链一：存 Matrix Mem，arrive_num++</text>
<rect x="758" y="316" width="124" height="40" rx="5" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.1"/>
<text x="820" y="331" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.8" fill="#7c3aed" font-weight="700" text-anchor="middle">⑨ 与上游 reduction 数据相加</text>
<text x="820" y="346" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#5c6370" font-weight="400" text-anchor="middle">链二：两笔齐 → VU 求和</text>
<rect x="888" y="376" width="124" height="40" rx="5" fill="#ede9fe" stroke="#7c3aed" stroke-width="1.1"/>
<text x="950" y="391" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="7.8" fill="#7c3aed" font-weight="700" text-anchor="middle">⑩ 送下一组 / 链尾出核</text>
<text x="950" y="406" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="6.5" fill="#5c6370" font-weight="400" text-anchor="middle">落本组累加，不落透传；链尾 → PCIe → DPU</text>
<path d="M232 96 L237.93 155.3" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rha)"/>
<path d="M362 156 L367.93 215.3" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rha)"/>
<path d="M492 216 L497.93 275.3" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rha)"/>
<path d="M622 276 L627.93 216.7" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rha)"/>
<path d="M752 216 L757.3 216" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rha)"/>
<path d="M882 216 L887.3 216" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rha)"/>
<path d="M950 236 L950 246 L300 246 L300 255.3" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rha)"/>
<path d="M362 276 L497.36 335.72" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rha)"/>
<path d="M622 336 L757.3 336" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rha)"/>
<path d="M882 336 L887.93 395.3" stroke="#9aa1ad" stroke-width="1.2" fill="none" stroke-linejoin="round" stroke-linecap="round" marker-end="url(#rha)"/>
<rect x="30" y="440" width="1040" height="62" rx="5" fill="#f5f6f8" stroke="#9aa1ad" stroke-width="1.2"/>
<text x="46" y="457" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">④ 之前的 ③′ 是 nk 模式独有的：FC1 / FC3 在 chip 内切 K，部分和必须先归约成完整的 FC1、FC3 结果才能做 dot，所以汇聚到一个 core 做完再广播回去；tp_nn 模式没有这一步，chip 内只剩 FC2 的逐级 reduce。</text>
<text x="46" y="473" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">⑦ 是 FFN 唯一一次 chip 间 reduce：FC2 chip 间切 K，每个 chip 出的是完整长度的部分和，沿本行 8 个 chip 流着加，不需要中途汇聚再广播。</text>
<text x="46" y="489" font-family="'Noto Sans CJK SC', 'PingFang SC', 'Microsoft YaHei', -apple-system, sans-serif" font-size="10.5" fill="#5c6370" font-weight="400" text-anchor="start">⑧ ⑨ ⑩ 在链上每个有激活专家的组各发生一次；用户没有激活专家的组只透传。</text>
</svg>
```

按 EP6 + TP8、chip 内切 K 的 `nk` 模式，一个 token 从进 EP 组到链尾出核：

1. token 进 Node，DPU 加 `gpu_id` / `token_id` 包头
2. B core 确认本组所有目的 core 有余量后发一次搬运，包沿 path 逐跳复制进 64 个 core 的 Core Mem
3. FC1 / FC3：chip 内切 K，每个 core 一份部分和；**第二层**把它们逐跳 reduce 汇聚到 dot core
4. dot core 做 SiLU · dot · 量化，再广播给 8 个 core
5. FC2：MU 算本 core 的分片并做**第一层**专家间累加
6. FC2 chip 内切 N，各 core 的段 concat，不归约
7. FC2 chip 间切 K：**第二层**沿本行 8 个 chip 逐跳 reduce，得到本组结果，这是 FFN 唯一一次 chip 间 reduce
8. 本组结果作为 reduce 数据进 R core，链一存进 Matrix Mem、`arrive_num++`
9. 上游 EP 组的 reduction 数据到达，两笔齐了，链二做**第三层**求和
10. 送下一组 R core；链尾组经 PCIe Switch 进 DPU DDR，回 GPU

`tp_nn` 模式没有第 3 步的汇聚：FC1 / FC3 两级都切 N，chip 内只剩 FC2 的逐级 reduce。第 8 到 10 步在链上每个有激活专家的组各发生一次，用户没有激活专家的组只透传。

***

## 参数与时延

| 项 | 值 | 出处 |
| - | - | - |
| ReduceModule 输入 | 三路各 160 GB/s | MAS |
| ReduceModule 输出 | 160 GB/s | MAS |
| ReduceModule 算力 | 80 GFLOPS（FP32 / BF16） | MAS |
| ReduceModule 上下文 | 16 用户 × 16 KiB | MAS |
| 中间累加精度 | FP32 固定；输入 FP32 / BF16，输出可配 | MAS |
| R core 缓冲 | 32 MB Matrix Mem，每笔 12 KiB，约 1300 个用户 | 《软件栈》 |
| EP 组间出口带宽 | 链尾 60 GB/s，2 个 ETH 口（MoE + Reduction 模式） | 《系统与部署》 |
| ReduceModule Entry credit、bank 数、RMW 拍数、输出队列深度 | 64 flit、4、2、8 | 《latch 建模计划》的建模取值，设计未给 |
| Xbar 与 ReduceModule 三路输入的仲裁 | 轮询 | 同上 |

第二层的关键路径随部署规模增长：

```
部署 chip 数 ≤ 行 chip 数：
  Node 关键路径 = 行 chip 数 × 跨 chip 传输延时 + 部署 chip 数 × 单 chip Reduce 延时

部署 chip 数 > 行 chip 数：
  Node 关键路径 = (行 chip 数 + 部署 chip 数 / 行 chip 数 − 1) × (跨 chip 传输延时 + 单 chip Reduce 延时)

T_chip(reduce) = d_embedding / B_R2R + T_R2R × 5 跳
T_reduce = TP_chipNumber × (T_C2C + T_R2R × 5 跳) + d_embedding / B_R2R + T_C2C
```

bit 级一致性：Router reduce 按到达顺序 FP32 累加，MU 的 CSA 树按 scale block 分组累加，VU 归约按 LANES 内归约再 ⌈log2 SEG⌉ 级累加；参考实现与模型按同一顺序计算才能逐元素对上。

***

## 口径与待定

| 项 | 本篇取值 | 另一份怎么说 / 待定 |
| - | - | - |
| 第二层的累加做在哪 | Router 内的 ReduceModule（MAS） | HAS：Router 内不设 Reduce Buffer，累加由独立的 Rmem 子系统经 `reduce_0 / 1 / 2` 三端口完成，每端口 128 flits buffer，允许 Rmem 改写 vcid |
| Reduce credit 谁维护 | DTE 维护本级，ReduceModule 维护相邻下游，Router 不维护（MAS） | HAS：core 与 reduce 之间按用户粒度、reduce 之间按 flit 加用户双粒度，单独的流控网络，user stream 的释放由 core 集中管理 |
| 第一层做在哪 | MU 的 `C = C + (A × B) × W_ep`（建模默认） | 《软件栈》示例链里是一个 VU task；MAS 的备注是把它挪进 MU 以省 Core Mem |
| R core 求和做在哪 | 链二的 VU task | 备选：DTE 把第二笔搬进 Router 的 ReduceModule 加完直接送出 |
| R core 借哪个 core | 出口处的 Core3 / Core7，每行一个 | 暂定；借行尾是否导致 EP 组切分不均衡未定 |
| RouterTable `operation` 的 Reduce0 / Reduce1 / Reduce2 | 建模按源分量 / 中继累加 / 最终汇聚 | 原文未定义 |
| reduce 任务的拆分 | 一个 32 KB 的 reduce 任务拆成多笔 8 KB 由 TS 并行发射 | 方案可能改到 DTE 内做多笔，届时 TS 不再需要 `TASK_REDUCE_ISS` |
| dispatcher 是谁 | Bach core，顺序调度 | slave CPU 做 dispatcher 可乱序，需 CPU 管理每组 reduction 资源 |
| PPTP 下 silu · dot · 量化落在哪段 chip | `pptp_fc3_nk_dot_core` 在 FC3 chip | 软件流程梳理：FC1 / FC3 的 reduce 结果都落到 FC2 段 chip 的 core 0 |

***

## 取舍

* **为什么第三层不复用第二层的 Router 逐跳累加**：EP 覆盖的范围远大于 TP，且 EP 之间不均衡很大，逐跳 reduce 的 buffer 要大到能掩盖不均衡，否则任务少的 EP 被频繁反压。借一个 core 拿它的 32 MB Matrix Mem 当 buffer，另外四个候选各有硬伤：经 Ethernet 送 GPU 算要 7 个 ETH 通道；外挂 CPU 受限于内存通道与主板功耗；PCIe Switch 挂 FPGA 成本过高；chip 内新增小 core 会让不需要 reduction 的 chip 浪费 3/4 到 7/8 的新增面积。
* **为什么 chip 间只在 FC2 切 K**：切 K 的 reduce 在 Router 里逐跳流水累加，数据流过就加，不必等齐；FC2 若切 N，各 chip 的段要先汇聚成完整向量再广播回所有 chip，是关键路径上的一次串行往返，省的是等待不是带宽。
* **为什么 ReduceModule 不允许降级**：若允许绕过 Reduce 直接存储或转发，下游收到的是未归约的原始数据，且不知道这件事，没有补做的机会，宁可反压。
* **为什么退休时 ReduceModule 要延迟回收**：Retire 只说明 core 侧的搬运结束，ReduceModule 发往相邻下游的 flit 可能还在路上，等下游各方向的 Reduce credit 全部恢复到初始值才能确认都已被接收。
* **为什么 R core 要两条链**：单链下在途用户数被 stream_table 的 16 项卡住，推进顺序只能按到达顺序；R core 必须按“谁先集齐”推进，这一点单链做不到，与缓冲大小无关。
* **为什么死锁交给调度侧**：硬同步要 R core 之间互相通知，最差首尾 R core 直接通信；调度侧预留只要求硬件上报释放，R core 不带反压逻辑，代价是只能顺序调度。

***

配图：

* [EP 组间 Reduction 讨论 10 张](<Bach/02_二、需求分析/04_第二阶段需求分析（功能扩展）/02_EP组间Reduction讨论（过程）>)（`d08` Reduction 传输通路、`d09` core 内计算流程、卡死场景、解卡死机制、派遣机制）
* [Router 12 张](<Bach/04_四、MAS（Micro Architecture SPEC）/04_Router>)（`05` Core 出 Reduce、`06` ReduceModule 之间、`11` Reduce Module 数据通路与 Credit 职责边界）
* [core 内调度机制](<Bach/02_二、需求分析/06_第四阶段需求分析（Core Level需求分析）/04_core内调度机制>)（`d40` R core 的 task_chain 四步）

来源：

* `04_四、MAS/04_Router.md`：Reduce 操作（Core 出、ReduceModule 之间）、Reduce 和 TS 的任务链释放逻辑、Reduce Module
* `04_四、MAS/10_Matrix Unit DSA.md`：支持的矩阵运算形式
* `Bach项目文档/02_Architecture/03_HAS/02_DATA_NOC_DE_HAS.md`：3.2.4 归约数据流的 VC 传输、3.2.5 Rmem
* `04_第二阶段/02_EP组间Reduction讨论（过程）.md`、`04_第二阶段/01_MoE模型部署边界讨论（阶段总结）.md`
* `06_第四阶段/04_core内调度机制.md`：reduction_core
* `Bach软件文档库/04_总体设计/02_软件流程梳理.md`：Reduction Core 流程、FC1/FC3 Reduce、FC2 reduce
