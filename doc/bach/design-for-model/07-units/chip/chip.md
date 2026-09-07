# Chip

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → **chip**（每 LPU 48 颗）

给实现 Chip 装配、SCP 桩、ctrl_noc 端点与 C2C Bridge 的人：这一层有哪些对象、各自做哪些事、端口与存储怎么定。core 内部各单元在 `core/` 下各有一份文档。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《系统与部署》：“Chip 内结构”“Boot 流程”“Chip 内地址划分”
* 《Router 片上交换与归约》：“跳过与跨 chip”的 Skip 与 C2C Bridge
* 《软件栈》：“部署阶段：kernel 与 weights 走两条不同的路”

***

## 1　定位与边界

一颗 chip 是两行的 Core 阵列，加四个 C2C Bridge、一个 SCP、一条 ctrl_noc。48 颗 chip 怎么摆、四个 chip 口接到哪，是 LPU 的事；chip 只把四个口露出来。

**列数有两种**，由这颗 chip 在 LPU 里的列位置定：

| chip 的列位置 | 阵列 | core 数 | 多出来的那一列 |
| - | - | - | - |
| 中间列 | 2 行 × 4 列 | 8，全部是计算 core | —— |
| 第一列 | 2 行 × 5 列 | 10 | 在西侧：左上角是 B core，左下角不派角色 |
| 最后一列 | 2 行 × 5 列 | 10 | 在东侧：右下角是 R core，右上角不派角色 |

三种形状都是 **8 个计算 core**，B core 与 R core 是多出来的那一列带进来的，不占计算 core 的位置。**不派角色**是指那个 core 只构造 Router 的八个模块，不构造 TS、RV core、DSA 与三块存储：它永远不作端点，只按路由表转发。它坐在 chip 接 PCIe Switch 的那个口上（第一列的 W 口、最后一列的 E 口），Switch 与专用 core 之间的一跳就走它。

这一层有四个对象：

| 对象 | 形态 | 数量 |
| - | - | - |
| Chip | 装配容器，不打拍 | 1 |
| SCP 桩 | 独立打拍的模块 | 1 |
| ctrl_noc 端点 | 独立打拍的模块 | 8 或 10，每 core 一个 |
| C2C Bridge | 独立打拍的模块 | 4，每行左右两端各一个 |

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1900 800" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" role="img" aria-label="Chip 第 0 层">
<title>Chip 第 0 层</title>
<defs><marker id="a" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#475569"/></marker><marker id="as" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#475569"/></marker><marker id="g" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0f766e"/></marker><marker id="gs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0f766e"/></marker><marker id="o" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#b45309"/></marker><marker id="os" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#b45309"/></marker><marker id="p" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#7c3aed"/></marker><marker id="ps" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#7c3aed"/></marker><marker id="i" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#4338ca"/></marker><marker id="is" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#4338ca"/></marker><marker id="t" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0d9488"/></marker><marker id="ts" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0d9488"/></marker><marker id="r" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#be123c"/></marker><marker id="rs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#be123c"/></marker><marker id="b" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#2563eb"/></marker><marker id="bs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#2563eb"/></marker><marker id="m" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#d97706"/></marker><marker id="ms" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#d97706"/></marker><marker id="l" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#9aa1ad"/></marker><marker id="ls" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#9aa1ad"/></marker></defs>
<rect x="0" y="0" width="1900" height="800" fill="#ffffff"/>
<text x="20" y="26" font-size="12" fill="#111827">Chip · 第 0 层（两排 core 的 Router 都朝 chip 中部：第 0 排 TS 在上 Router 在下，第 1 排上下镜像；mid 直连；C2C Bridge 长在每排两端）</text>
<text x="885" y="26" font-size="9.5" fill="#6b7280">紫虚线 = ctrl_noc 配置总线　灰线 = R2R / C2C 数据链路（每方向 256 B/T 双向）</text>
<rect x="150" y="150" width="1700" height="540" rx="14" fill="none" stroke="#374151" stroke-width="1.2"/>
<rect x="340" y="180" width="200" height="170" rx="5" fill="#f8fafc" stroke="#374151" stroke-width="1"/>
<rect x="580" y="180" width="200" height="170" rx="5" fill="#f8fafc" stroke="#374151" stroke-width="1"/>
<rect x="820" y="180" width="200" height="170" rx="5" fill="#f8fafc" stroke="#374151" stroke-width="1"/>
<rect x="1060" y="180" width="200" height="170" rx="5" fill="#f8fafc" stroke="#374151" stroke-width="1"/>
<rect x="1300" y="180" width="200" height="170" rx="5" fill="#fdf6ec" stroke="#374151" stroke-width="1"/>
<rect x="340" y="480" width="200" height="170" rx="5" fill="#f8fafc" stroke="#374151" stroke-width="1"/>
<rect x="580" y="480" width="200" height="170" rx="5" fill="#f8fafc" stroke="#374151" stroke-width="1"/>
<rect x="820" y="480" width="200" height="170" rx="5" fill="#f8fafc" stroke="#374151" stroke-width="1"/>
<rect x="1060" y="480" width="200" height="170" rx="5" fill="#f8fafc" stroke="#374151" stroke-width="1"/>
<rect x="1300" y="480" width="200" height="170" rx="5" fill="#fef2f2" stroke="#be123c" stroke-width="1.4"/>
<text x="160" y="165" font-size="11" fill="#111827" font-weight="600">图上画最后一列 chip：2 行 × 5 列，row-major 编号。中间列 chip 少最右一列，只有 core0～core7；第一列 chip 是它的镜像，core0 是 B core、core5 不派角色</text>
<rect x="348" y="318" width="184" height="24" rx="3" fill="#e0e7ff" stroke="#4338ca"/>
<text x="440.0" y="334" font-size="9.5" fill="#3730a3" font-weight="600" text-anchor="middle">Router</text>
<text x="348" y="196" font-size="10" fill="#111827" font-weight="600">core0</text>
<rect x="348" y="202" width="184" height="16" rx="3" fill="#dcf3f0" stroke="#0d9488"/>
<text x="440.0" y="214" font-size="8.5" fill="#0f766e" font-weight="600" text-anchor="middle">TS</text>
<rect x="348" y="226" width="60" height="54" rx="3" fill="#fef9c3" stroke="#a16207"/>
<text x="378" y="248" font-size="8" fill="#374151" text-anchor="middle">MU</text>
<text x="378" y="262" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="410" y="226" width="60" height="54" rx="3" fill="#d1fae5" stroke="#047857"/>
<text x="440" y="248" font-size="8" fill="#374151" text-anchor="middle">VU</text>
<text x="440" y="262" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="472" y="226" width="60" height="54" rx="3" fill="#e0f2fe" stroke="#0369a1"/>
<text x="502" y="248" font-size="8" fill="#374151" text-anchor="middle">DTE</text>
<text x="502" y="262" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="348" y="288" width="184" height="18" rx="3" fill="#fde8d8" stroke="#c2410c"/>
<text x="440.0" y="301" font-size="8" fill="#7c2d12" text-anchor="middle">Mmem · Cmem · Smem · DTE xbar</text>
<rect x="588" y="318" width="184" height="24" rx="3" fill="#e0e7ff" stroke="#4338ca"/>
<text x="680.0" y="334" font-size="9.5" fill="#3730a3" font-weight="600" text-anchor="middle">Router</text>
<text x="588" y="196" font-size="10" fill="#111827" font-weight="600">core1</text>
<rect x="588" y="202" width="184" height="16" rx="3" fill="#dcf3f0" stroke="#0d9488"/>
<text x="680.0" y="214" font-size="8.5" fill="#0f766e" font-weight="600" text-anchor="middle">TS</text>
<rect x="588" y="226" width="60" height="54" rx="3" fill="#fef9c3" stroke="#a16207"/>
<text x="618" y="248" font-size="8" fill="#374151" text-anchor="middle">MU</text>
<text x="618" y="262" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="650" y="226" width="60" height="54" rx="3" fill="#d1fae5" stroke="#047857"/>
<text x="680" y="248" font-size="8" fill="#374151" text-anchor="middle">VU</text>
<text x="680" y="262" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="712" y="226" width="60" height="54" rx="3" fill="#e0f2fe" stroke="#0369a1"/>
<text x="742" y="248" font-size="8" fill="#374151" text-anchor="middle">DTE</text>
<text x="742" y="262" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="588" y="288" width="184" height="18" rx="3" fill="#fde8d8" stroke="#c2410c"/>
<text x="680.0" y="301" font-size="8" fill="#7c2d12" text-anchor="middle">Mmem · Cmem · Smem · DTE xbar</text>
<rect x="828" y="318" width="184" height="24" rx="3" fill="#e0e7ff" stroke="#4338ca"/>
<text x="920.0" y="334" font-size="9.5" fill="#3730a3" font-weight="600" text-anchor="middle">Router</text>
<text x="828" y="196" font-size="10" fill="#111827" font-weight="600">core2</text>
<rect x="828" y="202" width="184" height="16" rx="3" fill="#dcf3f0" stroke="#0d9488"/>
<text x="920.0" y="214" font-size="8.5" fill="#0f766e" font-weight="600" text-anchor="middle">TS</text>
<rect x="828" y="226" width="60" height="54" rx="3" fill="#fef9c3" stroke="#a16207"/>
<text x="858" y="248" font-size="8" fill="#374151" text-anchor="middle">MU</text>
<text x="858" y="262" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="890" y="226" width="60" height="54" rx="3" fill="#d1fae5" stroke="#047857"/>
<text x="920" y="248" font-size="8" fill="#374151" text-anchor="middle">VU</text>
<text x="920" y="262" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="952" y="226" width="60" height="54" rx="3" fill="#e0f2fe" stroke="#0369a1"/>
<text x="982" y="248" font-size="8" fill="#374151" text-anchor="middle">DTE</text>
<text x="982" y="262" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="828" y="288" width="184" height="18" rx="3" fill="#fde8d8" stroke="#c2410c"/>
<text x="920.0" y="301" font-size="8" fill="#7c2d12" text-anchor="middle">Mmem · Cmem · Smem · DTE xbar</text>
<rect x="1068" y="318" width="184" height="24" rx="3" fill="#e0e7ff" stroke="#4338ca"/>
<text x="1160.0" y="334" font-size="9.5" fill="#3730a3" font-weight="600" text-anchor="middle">Router</text>
<text x="1068" y="196" font-size="10" fill="#111827" font-weight="600">core3</text>
<rect x="1068" y="202" width="184" height="16" rx="3" fill="#dcf3f0" stroke="#0d9488"/>
<text x="1160.0" y="214" font-size="8.5" fill="#0f766e" font-weight="600" text-anchor="middle">TS</text>
<rect x="1068" y="226" width="60" height="54" rx="3" fill="#fef9c3" stroke="#a16207"/>
<text x="1098" y="248" font-size="8" fill="#374151" text-anchor="middle">MU</text>
<text x="1098" y="262" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="1130" y="226" width="60" height="54" rx="3" fill="#d1fae5" stroke="#047857"/>
<text x="1160" y="248" font-size="8" fill="#374151" text-anchor="middle">VU</text>
<text x="1160" y="262" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="1192" y="226" width="60" height="54" rx="3" fill="#e0f2fe" stroke="#0369a1"/>
<text x="1222" y="248" font-size="8" fill="#374151" text-anchor="middle">DTE</text>
<text x="1222" y="262" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="1068" y="288" width="184" height="18" rx="3" fill="#fde8d8" stroke="#c2410c"/>
<text x="1160.0" y="301" font-size="8" fill="#7c2d12" text-anchor="middle">Mmem · Cmem · Smem · DTE xbar</text>
<rect x="1308" y="318" width="184" height="24" rx="3" fill="#e0e7ff" stroke="#4338ca"/>
<text x="1400.0" y="334" font-size="9.5" fill="#3730a3" font-weight="600" text-anchor="middle">Router</text>
<text x="1308" y="196" font-size="10" fill="#111827" font-weight="600">core4</text>
<text x="1312" y="234.0" font-size="8.5" fill="#92400e">不派角色：只构造 Router 八个模块</text>
<text x="1312" y="247.5" font-size="8.5" fill="#92400e">local 侧禁用 · 不接收溢流</text>
<text x="1312" y="261.0" font-size="8.5" fill="#92400e">credit 跨过它透传</text>
<text x="1312" y="274.5" font-size="8.5" fill="#92400e">只按路由表转发</text>
<text x="1492" y="301" font-size="8.5" fill="#9ca3af" text-anchor="end">最后一列 chip 的空位</text>
<rect x="348" y="488" width="184" height="24" rx="3" fill="#e0e7ff" stroke="#4338ca"/>
<text x="440.0" y="504" font-size="9.5" fill="#3730a3" font-weight="600" text-anchor="middle">Router</text>
<text x="348" y="642" font-size="10" fill="#111827" font-weight="600">core5</text>
<rect x="348" y="612" width="184" height="16" rx="3" fill="#dcf3f0" stroke="#0d9488"/>
<text x="440.0" y="624" font-size="8.5" fill="#0f766e" font-weight="600" text-anchor="middle">TS</text>
<rect x="348" y="544" width="60" height="54" rx="3" fill="#fef9c3" stroke="#a16207"/>
<text x="378" y="566" font-size="8" fill="#374151" text-anchor="middle">MU</text>
<text x="378" y="580" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="410" y="544" width="60" height="54" rx="3" fill="#d1fae5" stroke="#047857"/>
<text x="440" y="566" font-size="8" fill="#374151" text-anchor="middle">VU</text>
<text x="440" y="580" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="472" y="544" width="60" height="54" rx="3" fill="#e0f2fe" stroke="#0369a1"/>
<text x="502" y="566" font-size="8" fill="#374151" text-anchor="middle">DTE</text>
<text x="502" y="580" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="348" y="518" width="184" height="18" rx="3" fill="#fde8d8" stroke="#c2410c"/>
<text x="440.0" y="531" font-size="8" fill="#7c2d12" text-anchor="middle">Mmem · Cmem · Smem · DTE xbar</text>
<rect x="588" y="488" width="184" height="24" rx="3" fill="#e0e7ff" stroke="#4338ca"/>
<text x="680.0" y="504" font-size="9.5" fill="#3730a3" font-weight="600" text-anchor="middle">Router</text>
<text x="588" y="642" font-size="10" fill="#111827" font-weight="600">core6</text>
<rect x="588" y="612" width="184" height="16" rx="3" fill="#dcf3f0" stroke="#0d9488"/>
<text x="680.0" y="624" font-size="8.5" fill="#0f766e" font-weight="600" text-anchor="middle">TS</text>
<rect x="588" y="544" width="60" height="54" rx="3" fill="#fef9c3" stroke="#a16207"/>
<text x="618" y="566" font-size="8" fill="#374151" text-anchor="middle">MU</text>
<text x="618" y="580" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="650" y="544" width="60" height="54" rx="3" fill="#d1fae5" stroke="#047857"/>
<text x="680" y="566" font-size="8" fill="#374151" text-anchor="middle">VU</text>
<text x="680" y="580" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="712" y="544" width="60" height="54" rx="3" fill="#e0f2fe" stroke="#0369a1"/>
<text x="742" y="566" font-size="8" fill="#374151" text-anchor="middle">DTE</text>
<text x="742" y="580" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="588" y="518" width="184" height="18" rx="3" fill="#fde8d8" stroke="#c2410c"/>
<text x="680.0" y="531" font-size="8" fill="#7c2d12" text-anchor="middle">Mmem · Cmem · Smem · DTE xbar</text>
<rect x="828" y="488" width="184" height="24" rx="3" fill="#e0e7ff" stroke="#4338ca"/>
<text x="920.0" y="504" font-size="9.5" fill="#3730a3" font-weight="600" text-anchor="middle">Router</text>
<text x="828" y="642" font-size="10" fill="#111827" font-weight="600">core7</text>
<rect x="828" y="612" width="184" height="16" rx="3" fill="#dcf3f0" stroke="#0d9488"/>
<text x="920.0" y="624" font-size="8.5" fill="#0f766e" font-weight="600" text-anchor="middle">TS</text>
<rect x="828" y="544" width="60" height="54" rx="3" fill="#fef9c3" stroke="#a16207"/>
<text x="858" y="566" font-size="8" fill="#374151" text-anchor="middle">MU</text>
<text x="858" y="580" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="890" y="544" width="60" height="54" rx="3" fill="#d1fae5" stroke="#047857"/>
<text x="920" y="566" font-size="8" fill="#374151" text-anchor="middle">VU</text>
<text x="920" y="580" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="952" y="544" width="60" height="54" rx="3" fill="#e0f2fe" stroke="#0369a1"/>
<text x="982" y="566" font-size="8" fill="#374151" text-anchor="middle">DTE</text>
<text x="982" y="580" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="828" y="518" width="184" height="18" rx="3" fill="#fde8d8" stroke="#c2410c"/>
<text x="920.0" y="531" font-size="8" fill="#7c2d12" text-anchor="middle">Mmem · Cmem · Smem · DTE xbar</text>
<rect x="1068" y="488" width="184" height="24" rx="3" fill="#e0e7ff" stroke="#4338ca"/>
<text x="1160.0" y="504" font-size="9.5" fill="#3730a3" font-weight="600" text-anchor="middle">Router</text>
<text x="1068" y="642" font-size="10" fill="#111827" font-weight="600">core8</text>
<rect x="1068" y="612" width="184" height="16" rx="3" fill="#dcf3f0" stroke="#0d9488"/>
<text x="1160.0" y="624" font-size="8.5" fill="#0f766e" font-weight="600" text-anchor="middle">TS</text>
<rect x="1068" y="544" width="60" height="54" rx="3" fill="#fef9c3" stroke="#a16207"/>
<text x="1098" y="566" font-size="8" fill="#374151" text-anchor="middle">MU</text>
<text x="1098" y="580" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="1130" y="544" width="60" height="54" rx="3" fill="#d1fae5" stroke="#047857"/>
<text x="1160" y="566" font-size="8" fill="#374151" text-anchor="middle">VU</text>
<text x="1160" y="580" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="1192" y="544" width="60" height="54" rx="3" fill="#e0f2fe" stroke="#0369a1"/>
<text x="1222" y="566" font-size="8" fill="#374151" text-anchor="middle">DTE</text>
<text x="1222" y="580" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="1068" y="518" width="184" height="18" rx="3" fill="#fde8d8" stroke="#c2410c"/>
<text x="1160.0" y="531" font-size="8" fill="#7c2d12" text-anchor="middle">Mmem · Cmem · Smem · DTE xbar</text>
<rect x="1308" y="488" width="184" height="24" rx="3" fill="#e0e7ff" stroke="#4338ca"/>
<text x="1400.0" y="504" font-size="9.5" fill="#3730a3" font-weight="600" text-anchor="middle">Router</text>
<text x="1308" y="642" font-size="10" fill="#be123c" font-weight="600">core9 · R core</text>
<rect x="1308" y="612" width="184" height="16" rx="3" fill="#dcf3f0" stroke="#0d9488"/>
<text x="1400.0" y="624" font-size="8.5" fill="#0f766e" font-weight="600" text-anchor="middle">TS</text>
<rect x="1308" y="544" width="60" height="54" rx="3" fill="#fef9c3" stroke="#a16207"/>
<text x="1338" y="566" font-size="8" fill="#374151" text-anchor="middle">MU</text>
<text x="1338" y="580" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="1370" y="544" width="60" height="54" rx="3" fill="#d1fae5" stroke="#047857"/>
<text x="1400" y="566" font-size="8" fill="#374151" text-anchor="middle">VU</text>
<text x="1400" y="580" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="1432" y="544" width="60" height="54" rx="3" fill="#e0f2fe" stroke="#0369a1"/>
<text x="1462" y="566" font-size="8" fill="#374151" text-anchor="middle">DTE</text>
<text x="1462" y="580" font-size="7" fill="#6b7280" text-anchor="middle">RV + DSA</text>
<rect x="1308" y="518" width="184" height="18" rx="3" fill="#fde8d8" stroke="#c2410c"/>
<text x="1400.0" y="531" font-size="8" fill="#7c2d12" text-anchor="middle">Mmem · Cmem · Smem · DTE xbar</text>
<rect x="150" y="273.0" width="150" height="114" rx="4" fill="#eef2ff" stroke="#4338ca"/>
<text x="162" y="294.0" font-size="11" fill="#111827" font-weight="600">C2C Bridge</text>
<text x="162.0" y="311.0" font-size="8.5" fill="#475569">简化 Router（RC/VA/SA）</text>
<text x="162.0" y="324.5" font-size="8.5" fill="#475569">TX：4 KB 拆包 + seq_id</text>
<text x="162.0" y="338.0" font-size="8.5" fill="#475569">RX：按 seq_id 拼包</text>
<text x="162.0" y="351.5" font-size="8.5" fill="#475569">AXI Bridge · credit 透传</text>
<text x="288" y="378.0" font-size="8.5" fill="#9ca3af" text-anchor="end">接 core0 左端</text>
<polygon points="29,314 120,314 111,346 20,346" fill="#f8fafc" stroke="#374151"/>
<text x="70.0" y="333.5" font-size="9" fill="#374151" text-anchor="middle">c2c[N]</text>
<rect x="1540" y="273.0" width="150" height="114" rx="4" fill="#eef2ff" stroke="#4338ca"/>
<text x="1552" y="294.0" font-size="11" fill="#111827" font-weight="600">C2C Bridge</text>
<text x="1552.0" y="311.0" font-size="8.5" fill="#475569">简化 Router（RC/VA/SA）</text>
<text x="1552.0" y="324.5" font-size="8.5" fill="#475569">TX：4 KB 拆包 + seq_id</text>
<text x="1552.0" y="338.0" font-size="8.5" fill="#475569">RX：按 seq_id 拼包</text>
<text x="1552.0" y="351.5" font-size="8.5" fill="#475569">AXI Bridge · credit 透传</text>
<text x="1678" y="378.0" font-size="8.5" fill="#9ca3af" text-anchor="end">接 core4 右端</text>
<polygon points="1729,314 1820,314 1811,346 1720,346" fill="#f8fafc" stroke="#374151"/>
<text x="1770.0" y="333.5" font-size="9" fill="#374151" text-anchor="middle">c2c[E]</text>
<rect x="150" y="443.0" width="150" height="114" rx="4" fill="#eef2ff" stroke="#4338ca"/>
<text x="162" y="464.0" font-size="11" fill="#111827" font-weight="600">C2C Bridge</text>
<text x="162.0" y="481.0" font-size="8.5" fill="#475569">简化 Router（RC/VA/SA）</text>
<text x="162.0" y="494.5" font-size="8.5" fill="#475569">TX：4 KB 拆包 + seq_id</text>
<text x="162.0" y="508.0" font-size="8.5" fill="#475569">RX：按 seq_id 拼包</text>
<text x="162.0" y="521.5" font-size="8.5" fill="#475569">AXI Bridge · credit 透传</text>
<text x="288" y="548.0" font-size="8.5" fill="#9ca3af" text-anchor="end">接 core5 左端</text>
<polygon points="29,484 120,484 111,516 20,516" fill="#f8fafc" stroke="#374151"/>
<text x="70.0" y="503.5" font-size="9" fill="#374151" text-anchor="middle">c2c[W]</text>
<rect x="1540" y="443.0" width="150" height="114" rx="4" fill="#eef2ff" stroke="#4338ca"/>
<text x="1552" y="464.0" font-size="11" fill="#111827" font-weight="600">C2C Bridge</text>
<text x="1552.0" y="481.0" font-size="8.5" fill="#475569">简化 Router（RC/VA/SA）</text>
<text x="1552.0" y="494.5" font-size="8.5" fill="#475569">TX：4 KB 拆包 + seq_id</text>
<text x="1552.0" y="508.0" font-size="8.5" fill="#475569">RX：按 seq_id 拼包</text>
<text x="1552.0" y="521.5" font-size="8.5" fill="#475569">AXI Bridge · credit 透传</text>
<text x="1678" y="548.0" font-size="8.5" fill="#9ca3af" text-anchor="end">接 core9 右端</text>
<polygon points="1729,484 1820,484 1811,516 1720,516" fill="#f8fafc" stroke="#374151"/>
<text x="1770.0" y="503.5" font-size="9" fill="#374151" text-anchor="middle">c2c[S]</text>
<path d="M116.5 330.0 L149.0 330.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M116.5 500.0 L149.0 500.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1691.0 330.0 L1723.5 330.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1691.0 500.0 L1723.5 500.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M301.0 330.0 L347.0 330.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M301.0 500.0 L347.0 500.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1493.0 330.0 L1539.0 330.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1493.0 500.0 L1539.0 500.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M533.0 330.0 L587.0 330.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="528.4" y="313.5" width="63.1" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="560.0" y="321" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">left / right</text>
<path d="M533.0 500.0 L587.0 500.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M773.0 330.0 L827.0 330.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M773.0 500.0 L827.0 500.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1013.0 330.0 L1067.0 330.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1013.0 500.0 L1067.0 500.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1253.0 330.0 L1307.0 330.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1253.0 500.0 L1307.0 500.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M440.0 343.0 L440.0 487.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="443.8" y="404.9" width="10.5" height="20.3" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 449.0 415.0)" x="449.0" y="418.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">mid</text>
<path d="M680.0 343.0 L680.0 487.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M920.0 343.0 L920.0 487.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1160.0 343.0 L1160.0 487.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1400.0 343.0 L1400.0 487.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<text x="470.0" y="411.0" font-size="8.5" fill="#6b7280" text-anchor="start">left / right：同行相邻 Router，256 B/T，40T</text>
<text x="470.0" y="425.0" font-size="8.5" fill="#6b7280" text-anchor="start">mid：另一行对称位置的 Router，mid-to-mid 直连</text>
<text x="950.0" y="411.0" font-size="8.5" fill="#6b7280" text-anchor="start">Router 在 core 的内侧边：第 0 排在底边，第 1 排在顶边，</text>
<text x="950.0" y="425.0" font-size="8.5" fill="#6b7280" text-anchor="start">十个 Router 排成中间一条带，C2C Bridge 接在这条带的四个端点上</text>
<rect x="20" y="44" width="240" height="100.0" rx="4" fill="#fbf3df" stroke="#b45309"/>
<text x="32" y="65" font-size="11" fill="#111827" font-weight="600">SCP 桩（每 chip 一个）</text>
<text x="32.0" y="82.0" font-size="8.5" fill="#475569">boot：自启动 → PCIe 训练</text>
<text x="32.0" y="95.5" font-size="8.5" fill="#475569">→ 顺序配 core0～core7</text>
<text x="32.0" y="109.0" font-size="8.5" fill="#475569">初始化六步 · 广播开关</text>
<text x="32.0" y="122.5" font-size="8.5" fill="#475569">weights 模式 ↔ 业务模式</text>
<path d="M260.0 94.0 L300.0 94.0 L300.0 154.0 L1875.0 154.0 L1875.0 676.0 L300.0 676.0" stroke="#7c3aed" stroke-width="1.6" fill="none" stroke-linejoin="round" stroke-dasharray="7 4"/>
<text x="306" y="88.0" font-size="8.5" fill="#7c3aed" text-anchor="start">scp_ctrl</text>
<text x="320" y="147" font-size="8.5" fill="#7c3aed" text-anchor="start">ctrl_noc 配置总线：按地址分发 → 每 core 一个 ctrl_noc 端点 → 各模块的 cfg 口；core id 只读；32 bit/T，每笔事务一拍</text>
<path d="M440.0 154.0 L440.0 179.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<rect x="443.8" y="156.9" width="10.5" height="20.3" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 449.0 167.0)" x="449.0" y="170.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#7c3aed" text-anchor="middle">cfg</text>
<path d="M440.0 676.0 L440.0 651.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<path d="M680.0 154.0 L680.0 179.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<path d="M680.0 676.0 L680.0 651.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<path d="M920.0 154.0 L920.0 179.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<path d="M920.0 676.0 L920.0 651.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<path d="M1160.0 154.0 L1160.0 179.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<path d="M1160.0 676.0 L1160.0 651.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<path d="M1400.0 154.0 L1400.0 179.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<path d="M1400.0 676.0 L1400.0 651.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<text x="1510" y="670" font-size="8.5" fill="#7c3aed" text-anchor="start">每 core 一个 ctrl_noc 端点，不派角色的也配</text>
<polygon points="29,174.0 140,174.0 131,204.0 20,204.0" fill="#f8fafc" stroke="#374151"/>
<text x="80.0" y="188.0" font-size="9" fill="#374151" text-anchor="middle">async_int</text>
<text x="80.0" y="199.0" font-size="7.5" fill="#6b7280" text-anchor="middle">core_status → SCP</text>
<path d="M84.5 174.0 L80.1 145.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#a)"/>
<text x="150" y="194.0" font-size="8.5" fill="#6b7280" text-anchor="start">本轮只留接口名</text>
<text x="20" y="762" font-size="10.5" fill="#374151" text-anchor="start">四个 chip 口由 LPU 接到相邻 chip 或 PCIe Switch；C2C 当作一种长延迟的 R2R，Router 到 Router 400T。</text>
<text x="20" y="784" font-size="10.5" fill="#374151" text-anchor="start">Chip 不打拍，是装配容器：按 chip 形状构造 8 或 10 个 Core、接 mesh、接四个 C2C Bridge、接 ctrl_noc。SCP 桩、ctrl_noc 端点、C2C Bridge 是本层三种独立打拍的模块。</text>
</svg>
```

***

## 2　功能清单

一功能一条，编号供“机制覆盖”一章引用。

### Chip 装配（构造期，不打拍）

| 编号 | 功能 |
| - | - |
| F1 | 按本 chip 的 `chip_shape` 构造 Core：中间列 8 个，第一列与最后一列 10 个。不派角色的那个 core 只构造 Router 的八个模块，不构造 TS、RV core、DSA 与三块存储 |
| F2 | 构造期断言：不派角色的 core 不承担 logical compute core、EP broadcast core、EP reduction core，也不承担任何需要访问 local memory 的源或目的；它只转发 |
| F3 | 同行相邻 core 的 Router `left` 与 `right` 端口用一对 Link 对接，走 R2R 参数 |
| F4 | `core[i]` 与另一行对称位置的 core 的 `mid` 端口对接：4 列 chip 是 `core[i]` 与 `core[i+4]`，5 列 chip 是 `core[i]` 与 `core[i+5]`。mid-to-mid 直连，同样走 R2R 参数 |
| F5 | 每行左右两端的 core 各接一个 C2C Bridge，行 0 左端引到 chip 的 N 口、行 0 右端引到 E 口、行 1 左端引到 W 口、行 1 右端引到 S 口。落到编号上：4 列 chip 是 core0 / core3 / core4 / core7，5 列 chip 是 core0 / core4 / core5 / core9 |
| F6 | 每 core 构造一个 ctrl_noc 端点，按各单元文档声明的 `cfg` 口接线 |
| F7 | 三种形状的角色分配写死在 `logical_map` 里：第一列 chip 的 core0 是 B core、core5 不派角色，最后一列 chip 的 core9 是 R core、core4 不派角色，其余 core 一律是 logical compute core 0～7 |
| F8 | 三种形状的路由表不同，路由表按 chip 从编译侧读入，不写死 |

### SCP 桩

| 编号 | 功能 |
| - | - |
| F9 | boot 序列：自启动 → 完成 PCIe 链路训练 → 给**本 chip 全部 core 的 Router** 配 RouterTable、Skip Mask 与 Credit Bypass Route → 顺序解复位并配置各 core 的 TS、三个 RV core 与三个 DSA |
| F10 | Router 那一段的上电顺序：PMU 退出 Idle 释放 core 时钟域复位 → `stream_credit[port]` 置 0 → RouterTable 处于默认状态（**所有条目 bypass / no-op**），VC Buffer 硬件固定初始化 → SCP 经 ctrl_noc 配 RouterTable 与 VC 使能 mask → 各 core 上电发初始化脉冲，`stream_credit` 逐步初始化 → 就绪 |
| F11 | 不派角色的 core 在 Skip Mask 里标成跳过：经过它的包走完整流水线但不投递 local。它的 Router 的**数据通路可时钟门控，配置通路时钟保持** |
| F12 | 不派角色的 core 只配 Router 那两样，不配 TS、RV core、DSA，也不解复位它们（那几个模块本来就没构造）。Router 的配置排在 TS 与 RV core 之前，全 chip 一个 core 不落；漏掉任何一个 core 的 Router，经过它的 path 就全断 |
| F13 | ctrl_noc 广播开关：关时依次配每个 core，开时只发一次带广播标记的请求给 core0，由 core0 依次广播；默认关 |
| F14 | 每个 core 的初始化五步，按序做完：RV core firmware 写入 ITCM → 配置 Bach core 解复位 → TS 初始化（任务链）→ Router 初始化（路由表）→ kernel 初始化。DTE 的 weights loader 属于 kernel，随 kernel 镜像装入 |
| F15 | RouterTable 的三份副本：等 Router 内部多副本提交完成后，软件才写 DTE 与 ReduceModule 的那两份，硬件不代为同步 |
| F16 | core 内 boot：把启动程序搬进三个 RV core 的 ITCM，启动三个 RV core 进 wait，确认 `ready` 全高后开放业务接收权限，Router 才开始接收业务 |
| F17 | TS 没有控制核，只有寄存器，复位清 0 后等外部启动，不需要装载程序 |
| F18 | 装载拍数按镜像字节数除以 4 B 计，与业务段用同一把尺 |
| F19 | weights 加载模式的配置：Router 路由表配成 weights 专用的 P2P 路径且只用 1 条 path，TS 的 datain 任务 `pc` 指向 weights loader、`trigger_task_chain_en = 0` |
| F20 | 加载一笔 weights 的四步：Router 收到数据通知 TS 触发 datain 任务 → TS 通知 DTE core 执行 → DTE core 跑 weights loader 算出落 Matrix Mem 的地址再发 DTE 指令搬运 → datain 完成通知 TS 释放，不触发任务链 |
| F21 | 切到业务模式改三处：Router 路由表换成业务路径、TS 的 datain `pc` 指向 token 搬移入口且 `trigger_task_chain_en = 1`、各 DSA 写入业务场景的静态配置 |
| F22 | 异常与中断从 `async_int` 收，转报给上层。本轮只留接口名与状态位，不实现行为 |

### ctrl_noc 端点

| 编号 | 功能 |
| - | - |
| F23 | 收 `scp_ctrl` 事务，`cfg_core` 命中本 core 或带广播标记时锁存，按 `addr_map` 查出目的模块 |
| F24 | 下一拍把事务写到目的模块的 `cfg` 口：TS 的 CFG_REG、RouterTable 的 CSR、三个 DSA 的寄存器、Share Mem、RV core 的 ITCM 与 DTCM、Core Mem 与 Matrix Mem 的后门 |
| F25 | 地址空间视野检查：地址不在本 core 视野内时记地址错。三个 RV core 各自看到 ITCM、DTCM、Share Mem、Core Mem 与对应 DSA 的 IO reg；DTE DSA 另可见 Matrix Mem；MU / VU DSA 只读 Matrix Mem；SCP 看到 core 内全部地址空间 |
| F26 | `core_id` 是只读寄存器，SCP 经 ctrl_noc 读 MMIO 取得，软件不可修改；weights 落到哪个 core 全靠它 |
| F27 | 读事务转给目的模块，`rdata` 下一拍回 |

### C2C Bridge

| 编号 | 功能 |
| - | - |
| F28 | 简化 Router：RC / VA / SA 完整流水线，与 core 内 Router 同一套逻辑 |
| F29 | TX Engine 拆包：按 4 KB 边界拆分，加 4-bit `seq_id` 与 tail 标记；位宽 2048 转 1024 |
| F30 | RX Engine 拼包：按 `seq_id` 缓存，tail 到齐后还原原始包；位宽 1024 转 2048 |
| F31 | AXI Bridge 做 credit 与 AXI4 的协议转换 |
| F32 | 同向的数据与 credit release 之间做仲裁，小包优先；反向按类型 demux 分流 |
| F33 | TX 方向的 AXI write 是 posted，写响应可以丢 |
| F34 | RX 方向的 AXI 需要响应，由 AXI Bridge 返回 dummy response，释放 PCIe 的 outstanding 资源 |
| F35 | VC Buffer 按方向分档：TX 是 private 20 flit/VC × 4 = 80 加 shared 约 20，合计 100 flit ≈ 28.8 KB，覆盖本级 R2R 往返约 20 cycle；RX 是 private 20 flit/VC × 4 = 80 加 shared 约 300，合计 380 flit ≈ 109.4 KB，覆盖 PCIe 往返 600 ns @1024-bit。两向合计约 138.7 KB |
| F36 | credit 与这个结构一一对应，记法同 core 内 Router：每 VC 一个 private 计数器加每方向一个 shared 计数器，发送先扣 private 再扣 shared，归还先补 private。一个方向的 credit 总量等于对侧该方向的 buffer 容量，不超发 |
| F37 | 跨 chip 时同步上下游的 Reduce credit，防止上游超发；release 的粒度是 flit，在 C2C 上压缩包数量后再传 |
| F38 | 三类 credit 的 release 一律透传，Bridge 自身不建 stream credit 表，也不参与 Reduce 累加 |
| F39 | 三类 credit 可以共享同一个 AXI 传输包同步组包以提高效率，接收侧按分段还原。VC credit 在 Router 上是 flit 粒度，跨 C2C 要先转换成包粒度；业务层的两类本身就是包或 stream 粒度，不转换 |
| F40 | 反向 AXI write 携带 `{vc_id, vc_type, credit_release_length, credit_release_user}`，用 side band 信息与正常数据包区分，经 Demux 分流后更新本地的 `credit_cnt[vc]` 或 credit user table |
| F41 | 对着 PCIe Switch 或 CPU 的那一侧没有对端的 PCIe Bridge，硬件要能 **bypass 掉 Bridge 的业务层逻辑**，只保留位宽转换与拆包合包 |
| F42 | **只做透明传输**：左侧收到的包默认发到右侧，右侧收到的包默认发到左侧，Bridge 不做路由判断。业务上不对 C2C 使用独立地址编码方式访问，接口处做流式通信封装。所有跨 chip 的路由方案都建立在这个前提上 |

***

## 3　接口

```
port c2c[d] (双向, credit/release, clk)           // d ∈ {N, E, W, S}：chip 对外的四个口，由 LPU 接到相邻 chip 或 PCIe Switch
  in  flit_valid · flit_vc[1:0] · flit_head · flit_tail · flit_bytes[8:0] · flit_msg
  in  stream_release_valid · stream_release_user[15:0]
  in  reduce_release_valid · reduce_release_user[15:0]
  in  vc_release_valid · vc_release_vc[1:0]
  out 同字段
port bridge2core[d] (双向, credit/release, clk)   // C2C Bridge 与边界 core 的 Router 之间，字段同上；ready 的成立条件是对侧该 VC 的 credit 大于 0
port scp_ctrl (master, ctrl_noc 写事务, clk)      // SCP 桩 → ctrl_noc 端点，32 bit/T，每笔事务一拍
  out cfg_valid · cfg_core[3:0] · cfg_addr[23:0] · cfg_we · cfg_wdata[31:0] · cfg_bcast
  in  cfg_rdata[31:0]                               // 下一拍
port async_int (slave, 电平, clk)                 // core_status → SCP：中断信息，本轮只留接口名
  in  int_valid · int_code[7:0] · int_core[3:0]
port core_cfg[i][m] (master, ctrl_noc 写事务, clk) // ctrl_noc 端点 → 第 i 个 core 内模块 m 的 cfg 口，字段由各单元文档给
port core_ready[i] (slave, 电平, clk)             // 第 i 个 core 的三个 RV core 是否已进 wait
  in  ready
```

***

## 4　存储器

```
mem chip_shape     FF        {中间列 2×4, 第一列 2×5, 最后一列 2×5}                1R    编译侧读入      复位由输入给   // 本 chip 的 core 阵列形状
mem logical_map    FF 阵列   8 或 10 × {logical_core[3:0], role[2:0]}               1R    编译侧读入      复位由输入给   // 逻辑 core 编号与角色（compute / B core / R core / 不派角色）
mem addr_map       FF 阵列   N × {base[23:0], size, target_module, target_core}     1R    静态            复位由输入给   // ctrl_noc 地址分发表
mem core_id_reg[N] FF        只读 core id，N 为本 chip 的 core 数                    1R    SCP 经 ctrl_noc 读，不可改     复位固定
mem scp_fsm        FF        {state[3:0], core_idx[3:0], step[2:0], cursor[31:0]}   1RW   boot 序列       复位 自启动
mem scp_img        FF 阵列   配置事务序列（firmware、任务链、路由表、kernel、DSA 静态配置）        1R  编译侧读入  复位由输入给
mem noc_latch[N]   级间 latch {valid, addr[23:0], we, wdata[31:0]}                  —     每拍覆写        —              // 端点 → 目的模块 cfg 口
mem noc_rdata      1-deep 寄存器 {rdata[31:0]}                                      1W1R  每拍覆写        —
mem tx_vc_buf[4]   FIFO      每 VC 20 flit                                          1W1R  满 → 不再准入   复位空         // C2C Bridge TX 的 private buffer
mem tx_shared      FIFO      约 20 flit                                             1W1R  private 满时借用 复位空
mem rx_vc_buf[4]   FIFO      每 VC 80 flit                                          1W1R  满 → 向 PCIe 侧反压  复位空
mem rx_shared      FIFO      约 300 flit                                            1W1R  同上            复位空
mem rx_reasm       FF 阵列   按 seq_id 的重组缓冲，16 项                             1RW   tail 到齐即还原 复位空         // C2C Bridge RX 拼包
mem c2c_credit     FF 阵列   每方向每 VC 一个 private 计数器，加每方向一个 shared 计数器，另加每 UserID 的 Reduce credit  1RW  与对侧 VC Buffer 的占用规则一一对应：先扣 private 再扣 shared，归还先补 private  复位 TX private 20 / shared 20，RX private 80 / shared 300
```

***

## 5　流水线总览

Chip 自己不打拍，这一层的逐拍行为在 SCP 桩、ctrl_noc 端点与四个 C2C Bridge 里。第 1 层图按两段画：配置段是 boot 期的事，C2C 段是业务期的事，两段互不相干。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 678 436" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arcov" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="678" height="436" fill="#ffffff"/>

  <text x="20" y="26" font-size="12" fill="#111827">Chip · 第 1 层流水线总览（SCP 与 ctrl_noc 一段，C2C Bridge 一段，两段互不相干）</text>
  <text x="20" y="42" font-size="9.5" fill="#6b7280">横向是级序，不是拍序；每级的拍数在右上角 Dx。橙色虚线框是变长级，非按比例。</text>
  <path d="M150 52 L150 156" stroke="#e5e7eb" fill="none"/>
<path d="M150 212 L150 242" stroke="#e5e7eb" fill="none"/>
<path d="M150 298 L150 328" stroke="#e5e7eb" fill="none"/>
  <path d="M316 52 L316 70" stroke="#e5e7eb" fill="none"/>
<path d="M316 126 L316 156" stroke="#e5e7eb" fill="none"/>
<path d="M316 212 L316 328" stroke="#e5e7eb" fill="none"/>
  <line x1="482" y1="52" x2="482" y2="328" stroke="#e5e7eb"/>
  <text x="20" y="102" font-size="10.5" fill="#6b7280">配置</text>
  <rect x="150" y="70" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="160" y="84" font-size="8.5" fill="#92400e">M1</text>
  <text x="292" y="84" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="160" y="104" font-size="11" fill="#7c2d12">SCP boot 序列</text>
  <rect x="316" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="84" font-size="8.5" fill="#6b7280">M2</text>
  <text x="458" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="104" font-size="11" fill="#111827">ctrl_noc 端点</text>
  <text x="326" y="118" font-size="11" fill="#111827">按地址分发</text>
  <path d="M300 98 L315 98" stroke="#475569" marker-end="url(#arcov)" fill="none"/>
  <text x="20" y="188" font-size="10.5" fill="#6b7280">C2C 出</text>
  <rect x="150" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="170" font-size="8.5" fill="#6b7280">M3</text>
  <text x="292" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D3</text>
  <text x="160" y="190" font-size="11" fill="#111827">Bridge RC/VA/SA</text>
  <rect x="316" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="170" font-size="8.5" fill="#6b7280">M4</text>
  <text x="458" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="190" font-size="11" fill="#111827">TX Engine 拆包</text>
  <path d="M300 184 L315 184" stroke="#475569" marker-end="url(#arcov)" fill="none"/>
  <rect x="482" y="156" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="492" y="170" font-size="8.5" fill="#92400e">M6</text>
  <text x="624" y="170" font-size="8.5" fill="#92400e" text-anchor="end">D300</text>
  <text x="492" y="190" font-size="11" fill="#7c2d12">AXI Bridge</text>
  <path d="M466 184 L481 184" stroke="#475569" marker-end="url(#arcov)" fill="none"/>
  <text x="20" y="274" font-size="10.5" fill="#6b7280">C2C 入</text>
  <rect x="150" y="242" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="256" font-size="8.5" fill="#6b7280">M5</text>
  <text x="292" y="256" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="276" font-size="11" fill="#111827">RX Engine 拼包</text>
  <text x="20" y="352" font-size="10.5" fill="#374151">M1 每笔配置事务一拍，装载拍数按镜像字节数除以 4 B 计，与业务段用同一把尺。</text>
  <text x="20" y="380" font-size="10.5" fill="#374151">M6 的 300 拍是 PCIe C2C 的 300 ns；Router 到 Router 的 400 T 是这一段加两侧 Bridge 与走线的合计。</text>
  <text x="20" y="408" font-size="10.5" fill="#374151">C2C Bridge 的 RC / VA / SA 与 core 内 Router 同一套逻辑，只是不建 stream credit 表、不参与 Reduce 累加。</text>
</svg>
```

***

## 6　逐级行为

### M1 · SCP boot 序列

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 934 218" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arc1" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="934" height="218" fill="#ffffff"/>

  <rect x="20" y="34" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="38" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="55" font-size="10" fill="#374151" text-anchor="middle">scp_img · FF 配置事务序列 · 1R</text>
  <rect x="20" y="88" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="92" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="109" font-size="10" fill="#374151" text-anchor="middle">scp_fsm · FF · 1RW</text>
  <polygon points="30,142 188,142 178,182 20,182" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="161" font-size="10.5" fill="#374151" text-anchor="middle">core_ready[i]</text>
  <text x="104" y="179" font-size="9.5" fill="#6b7280" text-anchor="middle">ready</text>
  <polygon points="748,70 914,70 904,146 738,146" fill="#f8fafc" stroke="#374151"/>
  <text x="826" y="89" font-size="10.5" fill="#374151" text-anchor="middle">scp_ctrl</text>
  <text x="826" y="107" font-size="9.5" fill="#6b7280" text-anchor="middle">cfg_valid · cfg_core[3:0]</text>
  <text x="826" y="125" font-size="9.5" fill="#6b7280" text-anchor="middle">cfg_addr[23:0] · cfg_we</text>
  <text x="826" y="143" font-size="9.5" fill="#6b7280" text-anchor="middle">cfg_wdata[31:0] · cfg_bcast</text>
  <rect x="232" y="20" width="462" height="178" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M1</text>
  <text x="680" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="250" y="56" font-size="12" fill="#111827">SCP 桩 · 六步初始化逐笔发事务</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. 自启动 → PCIe 训练 → 本 chip 全部 core 的 Router 配路由表与 credit 旁路</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. 再顺序解复位各个 core，逐个走六步：RV firmware 进 ITCM → DTE</text>
  <text x="262" y="118" font-size="10.5" fill="#475569">解复位 → TS 任务链 → DSA 静态配置 → kernel</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">3. scp_ctrl = {cfg_core, cfg_addr, cfg_we, cfg_wdata, cfg_bcast}，每笔一拍</text>
  <text x="250" y="158" font-size="10.5" fill="#475569">4. Router 的 commit_done 拉高后才写 DTE 与 ReduceModule 的两份副本</text>
  <text x="250" y="182" font-size="10" fill="#9ca3af">三个 RV core 的 ready 全高后才开放业务接收</text>
  <path d="M188 55 L231 55" stroke="#475569" marker-end="url(#arc1)" fill="none"/>
  <path d="M188 109 L231 109" stroke="#475569" marker-end="url(#arc1)" fill="none"/>
  <path d="M188 162 L231 162" stroke="#475569" marker-end="url(#arc1)" fill="none"/>
  <path d="M694 108 L742 108" stroke="#475569" marker-end="url(#arc1)" fill="none"/>
</svg>
```

### M2 · ctrl_noc 端点分发

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 812 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arc2" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="812" height="198" fill="#ffffff"/>

  <polygon points="30,33 188,33 178,109 20,109" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="52" font-size="10.5" fill="#374151" text-anchor="middle">scp_ctrl</text>
  <text x="104" y="70" font-size="9.5" fill="#6b7280" text-anchor="middle">cfg_valid · cfg_core[3:0]</text>
  <text x="104" y="88" font-size="9.5" fill="#6b7280" text-anchor="middle">cfg_addr[23:0]</text>
  <text x="104" y="106" font-size="9.5" fill="#6b7280" text-anchor="middle">cfg_wdata[31:0] · cfg_bcast</text>
  <rect x="20" y="121" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="125" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="142" font-size="10" fill="#374151" text-anchor="middle">addr_map · FF · 1R</text>
  <polygon points="626,33 792,33 782,109 616,109" fill="#f8fafc" stroke="#374151"/>
  <text x="704" y="52" font-size="10.5" fill="#374151" text-anchor="middle">core_cfg[i][m]</text>
  <text x="704" y="70" font-size="9.5" fill="#6b7280" text-anchor="middle">cfg_valid · cfg_addr</text>
  <text x="704" y="88" font-size="9.5" fill="#6b7280" text-anchor="middle">cfg_we · cfg_wdata</text>
  <text x="704" y="106" font-size="9.5" fill="#6b7280" text-anchor="middle">cfg_rdata</text>
  <rect x="616" y="121" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="620" y="125" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="704" y="142" font-size="10" fill="#374151" text-anchor="middle">noc_latch[10] · 级间 latch · 1W</text>
  <rect x="232" y="20" width="340" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M2</text>
  <text x="558" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">ctrl_noc 端点 · 按地址找目的模块</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. take = (cfg_core == 本 core) || cfg_bcast</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. take → noc_latch = {valid, addr, we, wdata}</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. target = addr_map 查 cfg_addr 落在哪个模块的地址段</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. 地址不在本 core 视野内 → 记地址错，不下发</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">读事务的 rdata 下一拍回</text>
  <path d="M188 71 L231 71" stroke="#475569" marker-end="url(#arc2)" fill="none"/>
  <path d="M188 142 L231 142" stroke="#475569" marker-end="url(#arc2)" fill="none"/>
  <path d="M572 71 L620 71" stroke="#475569" marker-end="url(#arc2)" fill="none"/>
  <path d="M572 142 L615 142" stroke="#475569" marker-end="url(#arc2)" fill="none"/>
</svg>
```

### M3 · Bridge RC/VA/SA

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 913 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arc3" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="913" height="198" fill="#ffffff"/>

  <polygon points="30,33 188,33 178,109 20,109" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="52" font-size="10.5" fill="#374151" text-anchor="middle">bridge2core[d]</text>
  <text x="104" y="70" font-size="9.5" fill="#6b7280" text-anchor="middle">flit_valid · flit_vc[1:0]</text>
  <text x="104" y="88" font-size="9.5" fill="#6b7280" text-anchor="middle">flit_head · flit_tail</text>
  <text x="104" y="106" font-size="9.5" fill="#6b7280" text-anchor="middle">flit_msg</text>
  <rect x="20" y="121" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="125" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="142" font-size="10" fill="#374151" text-anchor="middle">c2c_credit · FF 每方向每 VC · 1RW</text>
  <rect x="717" y="53" width="176" height="92" fill="#f1f5f9" stroke="#334155"/>
  <rect x="717" y="53" width="176" height="18" fill="#334155"/>
  <text x="805" y="66" font-size="10.5" fill="#ffffff" text-anchor="middle">BR_ST</text>
  <text x="805" y="93" font-size="10" fill="#334155" text-anchor="middle">out_dir[2:0]</text>
  <text x="805" y="115" font-size="10" fill="#334155" text-anchor="middle">nxt_vc[1:0]</text>
  <text x="805" y="137" font-size="10" fill="#334155" text-anchor="middle">is_release</text>
  <rect x="232" y="20" width="441" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M3</text>
  <text x="659" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D3</text>
  <text x="250" y="56" font-size="12" fill="#111827">C2C Bridge · 与 core 内 Router 同一套三关</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. RC：按 path_id 查得出口方向与下一跳 VC</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. VA：查 c2c_credit[方向][VC] &gt; 0，跨 chip 另同步下游的 Reduce credit</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. SA：同向的数据与 credit release 之间仲裁，小包优先</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. Bridge 不建 stream credit 表，也不参与 Reduce 累加，三类 release 一律透传</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">反向按类型 demux 分流</text>
  <path d="M188 71 L231 71" stroke="#475569" marker-end="url(#arc3)" fill="none"/>
  <path d="M188 142 L231 142" stroke="#475569" marker-end="url(#arc3)" fill="none"/>
  <path d="M673 99 L716 99" stroke="#475569" marker-end="url(#arc3)" fill="none"/>
</svg>
```

### M4 · TX Engine 拆包

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 812 218" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arc4" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="812" height="218" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">BR_ST</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">out_dir[2:0]</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">nxt_vc[1:0]</text>
  <rect x="20" y="102" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="106" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="123" font-size="10" fill="#374151" text-anchor="middle">tx_vc_buf[4] · FIFO 20 flit · 1W1R</text>
  <rect x="20" y="156" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="160" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="177" font-size="10" fill="#374151" text-anchor="middle">tx_shared · FIFO 20 flit · 1W1R</text>
  <polygon points="626,79 792,79 782,137 616,137" fill="#f8fafc" stroke="#374151"/>
  <text x="704" y="98" font-size="10.5" fill="#374151" text-anchor="middle">c2c[d]</text>
  <text x="704" y="116" font-size="9.5" fill="#6b7280" text-anchor="middle">flit_valid · flit_msg</text>
  <text x="704" y="134" font-size="9.5" fill="#6b7280" text-anchor="middle">seq_id[3:0] · tail</text>
  <rect x="232" y="30" width="340" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="46" font-size="8.5" fill="#6b7280">M4</text>
  <text x="558" y="46" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="66" font-size="12" fill="#111827">TX Engine · 按 4 KB 边界拆并降位宽</text>
  <text x="250" y="88" font-size="10.5" fill="#475569">1. seg = 按 4 KB 边界切分；seq_id = 段号（4 bit）</text>
  <text x="250" y="108" font-size="10.5" fill="#475569">2. 最后一段置 tail 标记</text>
  <text x="250" y="128" font-size="10.5" fill="#475569">3. 位宽 2048 转 1024，一拍拆成两拍发出</text>
  <text x="250" y="148" font-size="10.5" fill="#475569">4. AXI write 是 posted，写响应可以丢</text>
  <text x="250" y="172" font-size="10" fill="#9ca3af">private 20 flit 覆盖本级 R2R 往返约 20 拍</text>
  <path d="M188 55 L231 55" stroke="#475569" marker-end="url(#arc4)" fill="none"/>
  <path d="M188 123 L231 123" stroke="#475569" marker-end="url(#arc4)" fill="none"/>
  <path d="M188 177 L231 177" stroke="#475569" marker-end="url(#arc4)" fill="none"/>
  <path d="M572 108 L620 108" stroke="#475569" marker-end="url(#arc4)" fill="none"/>
</svg>
```

### M5 · RX Engine 拼包

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 862 208" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arc5" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="862" height="208" fill="#ffffff"/>

  <polygon points="30,20 188,20 178,78 20,78" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="39" font-size="10.5" fill="#374151" text-anchor="middle">c2c[d]</text>
  <text x="104" y="57" font-size="9.5" fill="#6b7280" text-anchor="middle">flit_valid · flit_msg</text>
  <text x="104" y="75" font-size="9.5" fill="#6b7280" text-anchor="middle">seq_id[3:0] · tail</text>
  <rect x="20" y="90" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="94" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="111" font-size="10" fill="#374151" text-anchor="middle">rx_vc_buf[4] · FIFO 80 flit · 1W1R</text>
  <rect x="20" y="144" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="148" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="165" font-size="10" fill="#374151" text-anchor="middle">rx_reasm · FF 16 项 · 1RW</text>
  <polygon points="676,74 842,74 832,132 666,132" fill="#f8fafc" stroke="#374151"/>
  <text x="754" y="93" font-size="10.5" fill="#374151" text-anchor="middle">bridge2core[d]</text>
  <text x="754" y="111" font-size="9.5" fill="#6b7280" text-anchor="middle">flit_valid · flit_msg</text>
  <text x="754" y="129" font-size="9.5" fill="#6b7280" text-anchor="middle">flit_head · flit_tail</text>
  <rect x="232" y="25" width="390" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="41" font-size="8.5" fill="#6b7280">M5</text>
  <text x="608" y="41" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="61" font-size="12" fill="#111827">RX Engine · 按 seq_id 还原原始包</text>
  <text x="250" y="83" font-size="10.5" fill="#475569">1. rx_reasm[seq_id] 缓存收到的段</text>
  <text x="250" y="103" font-size="10.5" fill="#475569">2. tail 到齐 → 按 seq_id 顺序拼回原始包</text>
  <text x="250" y="123" font-size="10.5" fill="#475569">3. 位宽 1024 转 2048</text>
  <text x="250" y="143" font-size="10.5" fill="#475569">4. AXI Bridge 回 dummy response，释放 PCIe 的 outstanding 资源</text>
  <text x="250" y="167" font-size="10" fill="#9ca3af">RX private 80 加 shared 约 300 flit，覆盖 PCIe 往返 600 ns</text>
  <path d="M188 49 L231 49" stroke="#475569" marker-end="url(#arc5)" fill="none"/>
  <path d="M188 111 L231 111" stroke="#475569" marker-end="url(#arc5)" fill="none"/>
  <path d="M188 165 L231 165" stroke="#475569" marker-end="url(#arc5)" fill="none"/>
  <path d="M622 103 L670 103" stroke="#475569" marker-end="url(#arc5)" fill="none"/>
</svg>
```

### M6 · AXI Bridge

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 812 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arc6" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="812" height="198" fill="#ffffff"/>

  <polygon points="30,60 188,60 178,136 20,136" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="79" font-size="10.5" fill="#374151" text-anchor="middle">c2c[d]</text>
  <text x="104" y="97" font-size="9.5" fill="#6b7280" text-anchor="middle">flit_valid · flit_msg</text>
  <text x="104" y="115" font-size="9.5" fill="#6b7280" text-anchor="middle">stream_release_valid</text>
  <text x="104" y="133" font-size="9.5" fill="#6b7280" text-anchor="middle">reduce_release_valid</text>
  <polygon points="626,69 792,69 782,127 616,127" fill="#f8fafc" stroke="#374151"/>
  <text x="704" y="88" font-size="10.5" fill="#374151" text-anchor="middle">c2c[d]</text>
  <text x="704" y="106" font-size="9.5" fill="#6b7280" text-anchor="middle">flit_valid · flit_msg</text>
  <text x="704" y="124" font-size="9.5" fill="#6b7280" text-anchor="middle">vc_release_valid</text>
  <rect x="232" y="20" width="340" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M6</text>
  <text x="558" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D300</text>
  <text x="250" y="56" font-size="12" fill="#111827">AXI Bridge · credit 与 AXI4 的协议转换</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. 出方向：credit 语义转成 AXI write burst</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. 入方向：AXI read/write 转回 flit 与三类 release</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. release 的粒度是 flit，在 C2C 上压缩包数量后再传</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. 300 拍是 PCIe C2C 的 300 ns，按 1 T = 1 ns 折算</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">Router 到 Router 的 400 T 含这一段与两侧 Bridge</text>
  <path d="M188 98 L231 98" stroke="#475569" marker-end="url(#arc6)" fill="none"/>
  <path d="M572 98 L620 98" stroke="#475569" marker-end="url(#arc6)" fill="none"/>
</svg>
```

***

## 7　参数汇总

```
ARRAY             中间列 2×4、第一列与最后一列 2×5，row-major
                  4 列：core0 = 行 0 左端 → chip 的 N 口，core3 = 行 0 右端 → E，core4 = 行 1 左端 → W，core7 = 行 1 右端 → S
                  5 列：core0 → N，core4 → E，core5 → W，core9 → S
ROLES             一律 8 个计算 core；第一列 chip 的 core0 是 B core、core5 不派角色，最后一列 chip 的 core9 是 R core、core4 不派角色
MESH              同行 left / right 相邻，跨行 mid 接另一行对称位置；每方向 256 B/T、40T
CTRL_NOC_BW       32 bit/T（待定）；每笔事务一拍
CTRL_NOC_BCAST    开关，默认关
PCIE_TRAIN_CYCLES 待定
ITCM 装载拍数      镜像字节数 / 4 B
C2C_BRIDGE        每 chip 4 个，分布在 mesh 两侧，不是每 core 一个
C2C_SPLIT         4 KB 边界拆包，seq_id 4 bit
C2C_WIDTH         TX 2048 → 1024，RX 1024 → 2048
C2C_VC_BUF        合计约 138.7 KB；TX private 20 flit/VC ×4 + shared 约 20，RX private 80 + shared 约 300
C2C_LATENCY       Router 到 Router 400T，PCIe C2C 64 GB/s；HAS 记跨 chip 单向 ≤ 200～300 ns
C2C_VC_BUF_DETAIL TX private 20 flit/VC × 4 + shared 20 = 100 flit ≈ 28.8 KB；RX private 80 + shared 300 = 380 flit ≈ 109.4 KB
```

***

## 8　机制覆盖

| 机制 | 功能 | 用例 |
| - | - | - |
| 两种阵列形状与三个 R2R 方向的连接规则 | F3、F4 | `chip_mesh_shape` |
| 不派角色的 core 只构造 Router 的八个模块，只转发不落数据 | F1、F11 | `spare_core_router_only` |
| 不派角色的 core 不能承担 compute / B core / R core；可承担转发、多播、router reduce | F2 | `spare_core_roles` |
| 三种形状的角色分配与路由表按 chip 读入，不写死 | F7、F8 | `chip_shape_roles` |
| 每行左右两端接 C2C Bridge，全 chip 共 4 个 | F5 | `c2c_bridge_four` |
| core id 由 SCP 经 ctrl_noc 读 MMIO，不可修改 | F26 | `core_id_readonly` |
| SCP boot 序列：自启动 → PCIe 训练 → 全部 Router → 各 core | F9 | `boot_sequence` |
| ctrl_noc 广播开关 | F13 | `ctrl_noc_bcast` |
| 初始化六步 | F14 | `init_six_steps` |
| Router 多副本提交完成后软件再写 DTE 与 ReduceModule 副本 | F15 | `router_table_three_copies` |
| Core 内 boot：ITCM 装载 → 三个 RV core 进 wait → ready 全高 → 开放业务接收 | F16、F17 | `core_boot` |
| weights 加载模式：只用 1 条 P2P path，不启动任务链 | F19、F20 | `weights_load` |
| 切到业务模式 | F21 | `switch_to_business` |
| 地址空间视野 | F25 | `address_map` |
| credit 分 private 与 shared 两级，总量等于对侧 buffer 容量 | F37 | `c2c_credit_two_level` |
| C2C 拆包：4 KB 边界 + seq_id + tail | F29 | `c2c_split` |
| C2C 拼包：按 seq_id 缓存，tail 到齐还原 | F30 | `c2c_reassemble` |
| 同向数据与 credit release 仲裁，小包优先 | F32 | `c2c_arb_small_first` |
| TX posted write 丢响应，RX 返回 dummy response | F33、F34 | `c2c_axi_response` |
| 跨 chip 同步上下游 Reduce credit，release 按 flit 压缩后再传 | F37 | `c2c_reduce_credit` |
| 三类 credit 共享一个 AXI 包，VC credit 由 flit 粒度转包粒度 | F39 | `c2c_credit_pack` |
| 对 PCIe Switch 一侧 bypass 掉 Bridge 的业务层逻辑 | F41 | `c2c_bridge_bypass` |
| C2C 只做透明传输，左收右发、右收左发 | F42 | `c2c_transparent` |
| Router 段上电六步，RouterTable 默认 bypass / no-op | F10、F12 | `router_boot_reset` |
| 不派角色的 core 的 Router 数据通路时钟门控，配置通路时钟保持 | F11 | `spare_core_clock_gate` |

***

## 9　取舍

* **Chip 为什么做成装配容器而不是模块**
  * 它没有自己的一拍工作，全部逐拍行为在 Core 内各模块、SCP 桩、ctrl_noc 端点与 C2C Bridge 里
* **C2C Bridge 为什么归 chip 而不是归 core**
  * 全 chip 只有 4 个，长在 mesh 两侧，不是每 core 一个
  * 它做的拆包、拼包、协议转换与 core 内的 Router 无关
* **ctrl_noc 端点为什么每 core 一个**
  * 让配置事务与每个模块的 `cfg` 口一一对应
* **装载拍数为什么按字节数计而不是同拍写入**
  * 这样 boot 段的拍数与业务段用同一把尺
