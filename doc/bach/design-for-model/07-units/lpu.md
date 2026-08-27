# LPU

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：**LPU**（模型的顶层）→ chip ×48

给实现 LPU 顶层装配的人：48 颗 chip 怎么摆、彼此怎么接、全局坐标怎么算、片外桩挂在哪、Harvest 与逻辑 core 映射怎么读进来。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《系统与部署》：“集群与 Node”“tray 组成：4 层 × 4 chip”“机柜内多 tray 互联与 token 派遣”“Harvest（良率方案）”
* 《软件栈》：“编译器的硬件抽象”“编译器输入文件”

***

## 1　定位与边界

本轮建模的顶层是一个 LPU：48 颗 chip 装模型的一层 MoE。按《系统与部署》的物理组成，这 48 颗 chip 是一个机柜，也就是当前部署下的一个 LPU Node。

LPU 的组成：

| 组成 | 数量 | 在模型里是什么 |
| - | - | - |
| chip | 48，摆成全局 12 × 4 的网格 | 装配容器，各自 2×5 个 core |
| tray | 3，每个 4 层 × 4 chip | 不是对象。它决定 chip 的全局坐标，以及哪两处纵向链路用不同参数 |
| PCIe Switch | 12，每 tray 4 个，左右各 2，一个接两层 chip 的边缘口 | 独立打拍的模块 |
| 链路 | chip 之间、chip 到 Switch、Switch 到片外桩 | 独立打拍的模块，一条物理链路每方向一个实例 |

LPU 只做构造与接线，不打拍，五件事：

1. 按每颗 chip 自己的 harvest mask 构造 48 个 Chip
2. 按 12 × 4 网格接 chip 之间的 C2C 链路：同层左右直连、同列上下直连，都不经 Switch
3. 每层最左最右两颗 chip 的边缘口接本 tray 的 PCIe Switch
4. 片外桩挂到 PCIe Switch 上
5. 读入编译侧给的全局坐标换算表与逻辑 core 映射

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1250 880" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs>
    <marker id="p0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker>
    <marker id="p0s" markerWidth="9" markerHeight="9" refX="1" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#475569"/></marker>
    <marker id="p1" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#b45309"/></marker>
    <marker id="p1s" markerWidth="9" markerHeight="9" refX="1" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#b45309"/></marker>
  </defs>
  <rect x="0" y="0" width="1250" height="880" fill="#ffffff"/>
  <text x="20" y="30" font-size="12" fill="#111827">LPU · 第 0 层（48 chip，全局 12 × 4 网格）</text>
  <rect x="356" y="88" width="460" height="228" fill="none" stroke="#94a3b8" stroke-dasharray="5 4" rx="6"/>
  <text x="350" y="104" font-size="10" fill="#64748b" text-anchor="end">tray0</text>
  <rect x="356" y="312" width="460" height="228" fill="none" stroke="#94a3b8" stroke-dasharray="5 4" rx="6"/>
  <text x="350" y="328" font-size="10" fill="#64748b" text-anchor="end">tray1</text>
  <rect x="356" y="536" width="460" height="228" fill="none" stroke="#94a3b8" stroke-dasharray="5 4" rx="6"/>
  <text x="350" y="552" font-size="10" fill="#64748b" text-anchor="end">tray2</text>
  <rect x="380" y="100" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="424" y="117" font-size="9.5" fill="#111827" text-anchor="middle">chip(0,0)</text>
  <text x="424" y="130" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="488" y="100" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="532" y="117" font-size="9.5" fill="#111827" text-anchor="middle">chip(1,0)</text>
  <text x="532" y="130" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="596" y="100" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="640" y="117" font-size="9.5" fill="#111827" text-anchor="middle">chip(2,0)</text>
  <text x="640" y="130" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="704" y="100" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="748" y="117" font-size="9.5" fill="#111827" text-anchor="middle">chip(3,0)</text>
  <text x="748" y="130" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="380" y="156" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="424" y="173" font-size="9.5" fill="#111827" text-anchor="middle">chip(0,1)</text>
  <text x="424" y="186" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="488" y="156" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="532" y="173" font-size="9.5" fill="#111827" text-anchor="middle">chip(1,1)</text>
  <text x="532" y="186" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="596" y="156" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="640" y="173" font-size="9.5" fill="#111827" text-anchor="middle">chip(2,1)</text>
  <text x="640" y="186" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="704" y="156" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="748" y="173" font-size="9.5" fill="#111827" text-anchor="middle">chip(3,1)</text>
  <text x="748" y="186" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="380" y="212" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="424" y="229" font-size="9.5" fill="#111827" text-anchor="middle">chip(0,2)</text>
  <text x="424" y="242" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="488" y="212" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="532" y="229" font-size="9.5" fill="#111827" text-anchor="middle">chip(1,2)</text>
  <text x="532" y="242" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="596" y="212" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="640" y="229" font-size="9.5" fill="#111827" text-anchor="middle">chip(2,2)</text>
  <text x="640" y="242" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="704" y="212" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="748" y="229" font-size="9.5" fill="#111827" text-anchor="middle">chip(3,2)</text>
  <text x="748" y="242" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="380" y="268" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="424" y="285" font-size="9.5" fill="#111827" text-anchor="middle">chip(0,3)</text>
  <text x="424" y="298" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="488" y="268" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="532" y="285" font-size="9.5" fill="#111827" text-anchor="middle">chip(1,3)</text>
  <text x="532" y="298" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="596" y="268" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="640" y="285" font-size="9.5" fill="#111827" text-anchor="middle">chip(2,3)</text>
  <text x="640" y="298" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="704" y="268" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="748" y="285" font-size="9.5" fill="#111827" text-anchor="middle">chip(3,3)</text>
  <text x="748" y="298" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="380" y="324" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="424" y="341" font-size="9.5" fill="#111827" text-anchor="middle">chip(0,4)</text>
  <text x="424" y="354" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="488" y="324" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="532" y="341" font-size="9.5" fill="#111827" text-anchor="middle">chip(1,4)</text>
  <text x="532" y="354" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="596" y="324" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="640" y="341" font-size="9.5" fill="#111827" text-anchor="middle">chip(2,4)</text>
  <text x="640" y="354" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="704" y="324" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="748" y="341" font-size="9.5" fill="#111827" text-anchor="middle">chip(3,4)</text>
  <text x="748" y="354" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="380" y="380" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="424" y="397" font-size="9.5" fill="#111827" text-anchor="middle">chip(0,5)</text>
  <text x="424" y="410" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="488" y="380" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="532" y="397" font-size="9.5" fill="#111827" text-anchor="middle">chip(1,5)</text>
  <text x="532" y="410" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="596" y="380" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="640" y="397" font-size="9.5" fill="#111827" text-anchor="middle">chip(2,5)</text>
  <text x="640" y="410" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="704" y="380" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="748" y="397" font-size="9.5" fill="#111827" text-anchor="middle">chip(3,5)</text>
  <text x="748" y="410" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="380" y="436" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="424" y="453" font-size="9.5" fill="#111827" text-anchor="middle">chip(0,6)</text>
  <text x="424" y="466" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="488" y="436" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="532" y="453" font-size="9.5" fill="#111827" text-anchor="middle">chip(1,6)</text>
  <text x="532" y="466" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="596" y="436" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="640" y="453" font-size="9.5" fill="#111827" text-anchor="middle">chip(2,6)</text>
  <text x="640" y="466" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="704" y="436" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="748" y="453" font-size="9.5" fill="#111827" text-anchor="middle">chip(3,6)</text>
  <text x="748" y="466" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="380" y="492" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="424" y="509" font-size="9.5" fill="#111827" text-anchor="middle">chip(0,7)</text>
  <text x="424" y="522" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="488" y="492" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="532" y="509" font-size="9.5" fill="#111827" text-anchor="middle">chip(1,7)</text>
  <text x="532" y="522" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="596" y="492" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="640" y="509" font-size="9.5" fill="#111827" text-anchor="middle">chip(2,7)</text>
  <text x="640" y="522" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="704" y="492" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="748" y="509" font-size="9.5" fill="#111827" text-anchor="middle">chip(3,7)</text>
  <text x="748" y="522" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="380" y="548" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="424" y="565" font-size="9.5" fill="#111827" text-anchor="middle">chip(0,8)</text>
  <text x="424" y="578" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="488" y="548" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="532" y="565" font-size="9.5" fill="#111827" text-anchor="middle">chip(1,8)</text>
  <text x="532" y="578" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="596" y="548" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="640" y="565" font-size="9.5" fill="#111827" text-anchor="middle">chip(2,8)</text>
  <text x="640" y="578" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="704" y="548" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="748" y="565" font-size="9.5" fill="#111827" text-anchor="middle">chip(3,8)</text>
  <text x="748" y="578" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="380" y="604" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="424" y="621" font-size="9.5" fill="#111827" text-anchor="middle">chip(0,9)</text>
  <text x="424" y="634" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="488" y="604" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="532" y="621" font-size="9.5" fill="#111827" text-anchor="middle">chip(1,9)</text>
  <text x="532" y="634" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="596" y="604" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="640" y="621" font-size="9.5" fill="#111827" text-anchor="middle">chip(2,9)</text>
  <text x="640" y="634" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="704" y="604" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="748" y="621" font-size="9.5" fill="#111827" text-anchor="middle">chip(3,9)</text>
  <text x="748" y="634" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="380" y="660" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="424" y="677" font-size="9.5" fill="#111827" text-anchor="middle">chip(0,10)</text>
  <text x="424" y="690" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="488" y="660" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="532" y="677" font-size="9.5" fill="#111827" text-anchor="middle">chip(1,10)</text>
  <text x="532" y="690" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="596" y="660" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="640" y="677" font-size="9.5" fill="#111827" text-anchor="middle">chip(2,10)</text>
  <text x="640" y="690" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="704" y="660" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="748" y="677" font-size="9.5" fill="#111827" text-anchor="middle">chip(3,10)</text>
  <text x="748" y="690" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="380" y="716" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="424" y="733" font-size="9.5" fill="#111827" text-anchor="middle">chip(0,11)</text>
  <text x="424" y="746" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="488" y="716" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="532" y="733" font-size="9.5" fill="#111827" text-anchor="middle">chip(1,11)</text>
  <text x="532" y="746" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="596" y="716" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="640" y="733" font-size="9.5" fill="#111827" text-anchor="middle">chip(2,11)</text>
  <text x="640" y="746" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <rect x="704" y="716" width="88" height="38" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="748" y="733" font-size="9.5" fill="#111827" text-anchor="middle">chip(3,11)</text>
  <text x="748" y="746" font-size="8.5" fill="#94a3b8" text-anchor="middle">2×5 core</text>
  <line x1="468" y1="119" x2="488" y2="119" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="576" y1="119" x2="596" y2="119" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="684" y1="119" x2="704" y2="119" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="468" y1="175" x2="488" y2="175" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="576" y1="175" x2="596" y2="175" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="684" y1="175" x2="704" y2="175" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="468" y1="231" x2="488" y2="231" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="576" y1="231" x2="596" y2="231" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="684" y1="231" x2="704" y2="231" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="468" y1="287" x2="488" y2="287" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="576" y1="287" x2="596" y2="287" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="684" y1="287" x2="704" y2="287" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="468" y1="343" x2="488" y2="343" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="576" y1="343" x2="596" y2="343" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="684" y1="343" x2="704" y2="343" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="468" y1="399" x2="488" y2="399" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="576" y1="399" x2="596" y2="399" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="684" y1="399" x2="704" y2="399" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="468" y1="455" x2="488" y2="455" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="576" y1="455" x2="596" y2="455" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="684" y1="455" x2="704" y2="455" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="468" y1="511" x2="488" y2="511" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="576" y1="511" x2="596" y2="511" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="684" y1="511" x2="704" y2="511" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="468" y1="567" x2="488" y2="567" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="576" y1="567" x2="596" y2="567" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="684" y1="567" x2="704" y2="567" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="468" y1="623" x2="488" y2="623" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="576" y1="623" x2="596" y2="623" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="684" y1="623" x2="704" y2="623" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="468" y1="679" x2="488" y2="679" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="576" y1="679" x2="596" y2="679" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="684" y1="679" x2="704" y2="679" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="468" y1="735" x2="488" y2="735" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="576" y1="735" x2="596" y2="735" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="684" y1="735" x2="704" y2="735" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="424" y1="138" x2="424" y2="156" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="424" y1="194" x2="424" y2="212" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="424" y1="250" x2="424" y2="268" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="424" y1="306" x2="424" y2="324" stroke="#b45309" marker-start="url(#p1s)" marker-end="url(#p1)"/>
  <line x1="424" y1="362" x2="424" y2="380" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="424" y1="418" x2="424" y2="436" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="424" y1="474" x2="424" y2="492" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="424" y1="530" x2="424" y2="548" stroke="#b45309" marker-start="url(#p1s)" marker-end="url(#p1)"/>
  <line x1="424" y1="586" x2="424" y2="604" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="424" y1="642" x2="424" y2="660" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="424" y1="698" x2="424" y2="716" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="532" y1="138" x2="532" y2="156" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="532" y1="194" x2="532" y2="212" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="532" y1="250" x2="532" y2="268" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="532" y1="306" x2="532" y2="324" stroke="#b45309" marker-start="url(#p1s)" marker-end="url(#p1)"/>
  <line x1="532" y1="362" x2="532" y2="380" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="532" y1="418" x2="532" y2="436" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="532" y1="474" x2="532" y2="492" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="532" y1="530" x2="532" y2="548" stroke="#b45309" marker-start="url(#p1s)" marker-end="url(#p1)"/>
  <line x1="532" y1="586" x2="532" y2="604" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="532" y1="642" x2="532" y2="660" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="532" y1="698" x2="532" y2="716" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="640" y1="138" x2="640" y2="156" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="640" y1="194" x2="640" y2="212" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="640" y1="250" x2="640" y2="268" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="640" y1="306" x2="640" y2="324" stroke="#b45309" marker-start="url(#p1s)" marker-end="url(#p1)"/>
  <line x1="640" y1="362" x2="640" y2="380" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="640" y1="418" x2="640" y2="436" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="640" y1="474" x2="640" y2="492" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="640" y1="530" x2="640" y2="548" stroke="#b45309" marker-start="url(#p1s)" marker-end="url(#p1)"/>
  <line x1="640" y1="586" x2="640" y2="604" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="640" y1="642" x2="640" y2="660" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="640" y1="698" x2="640" y2="716" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="748" y1="138" x2="748" y2="156" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="748" y1="194" x2="748" y2="212" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="748" y1="250" x2="748" y2="268" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="748" y1="306" x2="748" y2="324" stroke="#b45309" marker-start="url(#p1s)" marker-end="url(#p1)"/>
  <line x1="748" y1="362" x2="748" y2="380" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="748" y1="418" x2="748" y2="436" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="748" y1="474" x2="748" y2="492" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="748" y1="530" x2="748" y2="548" stroke="#b45309" marker-start="url(#p1s)" marker-end="url(#p1)"/>
  <line x1="748" y1="586" x2="748" y2="604" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="748" y1="642" x2="748" y2="660" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="748" y1="698" x2="748" y2="716" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <rect x="228" y="102" width="76" height="92" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="266" y="130" font-size="9" fill="#111827" text-anchor="middle">PCIe SW</text>
  <text x="266" y="145" font-size="8.5" fill="#94a3b8" text-anchor="middle">层0 / 层1</text>
  <line x1="306" y1="119" x2="378" y2="119" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="306" y1="175" x2="378" y2="175" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <rect x="866" y="102" width="76" height="92" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="904" y="130" font-size="9" fill="#111827" text-anchor="middle">PCIe SW</text>
  <text x="904" y="145" font-size="8.5" fill="#94a3b8" text-anchor="middle">层0 / 层1</text>
  <line x1="794" y1="119" x2="864" y2="119" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="794" y1="175" x2="864" y2="175" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <rect x="228" y="214" width="76" height="92" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="266" y="242" font-size="9" fill="#111827" text-anchor="middle">PCIe SW</text>
  <text x="266" y="257" font-size="8.5" fill="#94a3b8" text-anchor="middle">层2 / 层3</text>
  <line x1="306" y1="231" x2="378" y2="231" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="306" y1="287" x2="378" y2="287" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <rect x="866" y="214" width="76" height="92" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="904" y="242" font-size="9" fill="#111827" text-anchor="middle">PCIe SW</text>
  <text x="904" y="257" font-size="8.5" fill="#94a3b8" text-anchor="middle">层2 / 层3</text>
  <line x1="794" y1="231" x2="864" y2="231" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="794" y1="287" x2="864" y2="287" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <rect x="228" y="326" width="76" height="92" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="266" y="354" font-size="9" fill="#111827" text-anchor="middle">PCIe SW</text>
  <text x="266" y="369" font-size="8.5" fill="#94a3b8" text-anchor="middle">层4 / 层5</text>
  <line x1="306" y1="343" x2="378" y2="343" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="306" y1="399" x2="378" y2="399" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <rect x="866" y="326" width="76" height="92" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="904" y="354" font-size="9" fill="#111827" text-anchor="middle">PCIe SW</text>
  <text x="904" y="369" font-size="8.5" fill="#94a3b8" text-anchor="middle">层4 / 层5</text>
  <line x1="794" y1="343" x2="864" y2="343" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="794" y1="399" x2="864" y2="399" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <rect x="228" y="438" width="76" height="92" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="266" y="466" font-size="9" fill="#111827" text-anchor="middle">PCIe SW</text>
  <text x="266" y="481" font-size="8.5" fill="#94a3b8" text-anchor="middle">层6 / 层7</text>
  <line x1="306" y1="455" x2="378" y2="455" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="306" y1="511" x2="378" y2="511" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <rect x="866" y="438" width="76" height="92" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="904" y="466" font-size="9" fill="#111827" text-anchor="middle">PCIe SW</text>
  <text x="904" y="481" font-size="8.5" fill="#94a3b8" text-anchor="middle">层6 / 层7</text>
  <line x1="794" y1="455" x2="864" y2="455" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="794" y1="511" x2="864" y2="511" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <rect x="228" y="550" width="76" height="92" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="266" y="578" font-size="9" fill="#111827" text-anchor="middle">PCIe SW</text>
  <text x="266" y="593" font-size="8.5" fill="#94a3b8" text-anchor="middle">层8 / 层9</text>
  <line x1="306" y1="567" x2="378" y2="567" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="306" y1="623" x2="378" y2="623" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <rect x="866" y="550" width="76" height="92" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="904" y="578" font-size="9" fill="#111827" text-anchor="middle">PCIe SW</text>
  <text x="904" y="593" font-size="8.5" fill="#94a3b8" text-anchor="middle">层8 / 层9</text>
  <line x1="794" y1="567" x2="864" y2="567" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="794" y1="623" x2="864" y2="623" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <rect x="228" y="662" width="76" height="92" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="266" y="690" font-size="9" fill="#111827" text-anchor="middle">PCIe SW</text>
  <text x="266" y="705" font-size="8.5" fill="#94a3b8" text-anchor="middle">层10 / 层11</text>
  <line x1="306" y1="679" x2="378" y2="679" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="306" y1="735" x2="378" y2="735" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <rect x="866" y="662" width="76" height="92" fill="#f8fafc" stroke="#374151" rx="3"/>
  <text x="904" y="690" font-size="9" fill="#111827" text-anchor="middle">PCIe SW</text>
  <text x="904" y="705" font-size="8.5" fill="#94a3b8" text-anchor="middle">层10 / 层11</text>
  <line x1="794" y1="679" x2="864" y2="679" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <line x1="794" y1="735" x2="864" y2="735" stroke="#475569" marker-start="url(#p0s)" marker-end="url(#p0)"/>
  <rect x="40" y="100" width="160" height="86" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="54" y="124" font-size="11" fill="#7c2d12">入口桩（每 GPU 一个）</text>
  <text x="54" y="144" font-size="9" fill="#92400e">注入表 · 两层 credit</text>
  <text x="54" y="160" font-size="9" fill="#92400e">GPU / DPU / ETH / CPU</text>
  <text x="54" y="176" font-size="9" fill="#92400e">的行为都收在这里</text>
  <line x1="202" y1="128" x2="226" y2="128" stroke="#475569" marker-end="url(#p0)"/>
  <text x="212" y="122" font-size="9" fill="#6b7280">ETH 链路</text>
  <rect x="976" y="660" width="160" height="86" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="990" y="684" font-size="11" fill="#7c2d12">出口桩</text>
  <text x="990" y="704" font-size="9" fill="#92400e">按 (gpu_id, token_id) 重组</text>
  <text x="990" y="720" font-size="9" fill="#92400e">逐 bit 比对 · 完成集合</text>
  <text x="990" y="736" font-size="9" fill="#92400e">retired 回报入口桩</text>
  <line x1="942" y1="688" x2="974" y2="688" stroke="#475569" marker-end="url(#p0)"/>
  <polyline points="1056,748 1056,780 120,780 120,188" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#p0)"/>
  <text x="300" y="794" font-size="9" fill="#6b7280">retired（回入口桩记 credit）</text>
  <text x="424" y="82" font-size="9.5" fill="#b45309" text-anchor="middle">global_top_left：外部数据从西侧进</text>
  <text x="748" y="794" font-size="9.5" fill="#b45309" text-anchor="middle">global_bottom_right：最终结果从东侧出</text>
  <text x="20" y="828" font-size="10.5" fill="#374151">橙色纵向连线是跨 tray 的两处：上一 tray 最底层 chip 的 S 口接下一 tray 最顶层 chip 的 N 口，链路参数与 tray 内不同。</text>
  <text x="20" y="850" font-size="10.5" fill="#374151">LPU 不是模块，是装配容器：构造 48 个 Chip、按网格接 C2C、把每层两端接 PCIe Switch、把片外桩挂上去。</text>
</svg>
```

***

## 2　接口

LPU 对外只有片外桩那一侧；chip 之间、chip 与 Switch 之间的连接都在 LPU 内部。

```
port ext[p] (双向, credit/release, clk)          // PCIe Switch 上接片外桩的端口，字段同链路
  in  flit_valid · flit_vc[1:0] · flit_head · flit_tail · flit_bytes[8:0] · flit_msg
  in  stream_release_valid · reduce_release_valid · vc_release_valid
  out 同字段
port chip.c2c[i][d] (双向, credit/release, clk)  // 第 i 颗 chip 的四个 C2C 端口，d ∈ {N, E, W, S}，由 L1 接线
port sw.port[p] (双向, credit/release, clk)      // PCIe Switch 的端口，由 L1 接线
```

***

## 3　存储器

```
mem grid          FF 阵列   48 × {tray[1:0], layer[1:0], col[1:0], gx[1:0], gy[3:0]}   1R   编译侧读入   复位由输入给   // 全局坐标换算表
mem harvest       FF 阵列   48 × 10 b                                1R   编译侧读入   复位由输入给   // 每 chip 的坏核位图
mem logical_map   FF 阵列   48 × 10 × {logical_core[3:0], role[2:0]}  1R   编译侧读入   复位由输入给   // 逻辑 core 编号与角色
mem entry_exit    FF        {global_top_left{gx,gy}, global_bottom_right{gx,gy}}  1R  编译侧读入  复位由输入给
mem link_param    FF 阵列   每条链路一项 {bw, latency}                1R   参数表       复位由输入给
```

***

## 4　流水线总览

LPU 没有自己的一拍工作，全部逐拍行为在 chip 内各模块、PCIe Switch 与片外桩里。构造期两级：L1 接线，L2 读入坐标与角色表。不画第 1 层图。

***

## 5　逐级行为

### L1 · LPU 装配与接线（构造期，不逐拍）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `grid`、`harvest`、`link_param` | 1. `Build`：对 48 颗 chip，把 `harvest[i]` 交给第 i 个 Chip 构造<br>2. `WireRow`：同层左右，`(gx, gy)` 的 `c2c[E]` 与 `(gx+1, gy)` 的 `c2c[W]` 用一对 Link 对接，C2C 参数<br>3. `WireCol`：同列上下，`(gx, gy)` 的 `c2c[S]` 与 `(gx, gy+1)` 的 `c2c[N]` 对接；`gy` 与 `gy+1` 跨 tray 时（`gy mod 4 == 3`）换纵向链路参数<br>4. `WireEdge`：每层 `gx == 0` 的 `c2c[W]`、`gx == 3` 的 `c2c[E]` 接本 tray 那一侧的 PCIe Switch，一个 Switch 接两层<br>5. `WireExt`：入口桩与出口桩各挂一个 Switch 端口，走 ETH 参数的 Link | 模块实例与端口连接 | — |

### L2 · 坐标与角色表读入（构造期，不逐拍）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `grid`、`logical_map`、`entry_exit` | 1. `gy = tray 序号 × 4 + tray 内层号`（0～11），`gx = 层内列号`（0～3）<br>2. 断言：core 数量 = 48 × 10；每颗 chip 的可用 core 数为 8；`gx ∈ {0, 3}` 的 chip 只允许 A 型或 B 型<br>3. 断言：`gx == 0` 的 chip 逻辑 core 8 是 EP broadcast core，`gx == 3` 的是 EP reduction core，且它占的物理 core 不在 compute 集合里<br>4. 把换算好的 `(gx, gy, core_id)` 交给各 Router 的坐标换算表，包头里的目的坐标按这张表解释 | 静态表 | — |

***

## 6　参数汇总

```
CHIPS         48 = 3 tray × 4 层 × 4 chip；全局 12 × 4 网格
CORES         每 chip 2×5，Harvest 后保证 8 个可用；全 LPU 480 个物理 core、384 个可用
TRAY          4 层，每层一行 4 chip；4 个 PCIe Switch，左右各 2，每个接两层
GLOBAL_Y      tray 序号 × 4 + tray 内层号，0～11
GLOBAL_X      层内列号，0～3
ENTRY         global_top_left 西侧进，沿第一列自上而下
PARTIAL       partial result 沿最后一列自上而下
EXIT          global_bottom_right 东侧出，经 Switch 到 ETH
CHIP_TYPE     A 型 0 个坏核、B 型 1 个、C 型 2 个；gx ∈ {0, 3} 只允许 A / B
DISPATCH      LPU 广播（当前选定的派遣方式）
链路参数       见链路的参数汇总：同层与同列用 C2C，跨 tray 用纵向参数，边缘用 PCIe ↔ Router
```

***

## 7　机制覆盖

| 机制 | 落点 | 用例 |
| - | - | - |
| 48 chip = 3 tray × 4 层 × 4 chip，全局 12 × 4 网格 | L1 第 1 条 + L2 第 2 条 | `lpu_grid` |
| `global_chip_y = tray 序号 × 4 + tray 内层号` | L2 第 1 条 | `global_coord` |
| 同层左右、同列上下直连，不经 Switch | L1 第 2、3 条 | `chip_direct_link` |
| 跨 tray：上一 tray 最底层 chip 的 S 口接下一 tray 最顶层 chip 的 N 口，链路参数另取 | L1 第 3 条 | `tray_vertical_link` |
| 每层两端 chip 的边缘口接 PCIe Switch，一个 Switch 接两层 | L1 第 4 条 | `chip_edge_switch` |
| 外部数据从 `global_top_left` 西侧进入 | L1 第 5 条 + 注入表 | `lpu_entry` |
| partial result 沿最后一列自上而下，最终结果从 `global_bottom_right` 东侧出 | L1 第 4、5 条 | `lpu_exit` |
| LPU 广播：token 只送进左上角第一个 B core，B core 留一份再沿第一列往下传，跨 tray 走纵向链路 | 注入表 + B core 的 kernel | `lpu_broadcast` |
| core 数量 = chip 数 × 10；每 chip 保证 8 个可用 | L2 第 2 条 | `core_count` |
| `gx ∈ {0, 3}` 的边界 chip 只允许 A / B 型 | L2 第 2 条 | `chip_type_position` |
| 逻辑 core 8 是 special core：第一列 EP broadcast、最后一列 EP reduction，按 special 优先四步分配 | 编译侧，L2 第 3 条查 | `logical_map` |
| EP special core 始终占一个 good core，不映射为 logical compute core | L2 第 3 条 | `special_reserved` |
| 坏核不能承担 compute / B core / R core | Chip 的 A1 第 2 条 | `harvest_roles` |

***

## 8　取舍

* **LPU 为什么是装配容器而不是模块**
  * 它没有自己的一拍工作，全部逐拍行为在 chip 内各模块、PCIe Switch 与片外桩里
* **tray 与“层”为什么不单独建对象**
  * 它们对模型只起两个作用：给 chip 定全局坐标、指出哪两处纵向链路换参数
  * 两个作用都落在坐标换算表和链路参数里，再多一级容器是空壳
* **为什么只建一个 LPU**
  * 一个 LPU 装模型的一层 MoE，跨 LPU 走 ETH
  * 跨 LPU 的那一段在片外桩里表达成注入表的时刻与一条链路实例
