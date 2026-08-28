# Chip

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → **chip**（每 LPU 48 颗）

给实现 Chip 装配、SCP 桩、ctrl_noc 端点与 C2C Bridge 的人：这一层有哪些对象、各自做哪些事、端口与存储怎么定。core 内部各单元在 `core/` 下各有一份文档。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《系统与部署》：“Chip 内结构”“Boot 流程”“Harvest（良率方案）”“Chip 内地址划分”
* 《Router 片上交换与归约》：“坏核与跨 chip”的 Skip 与 C2C Bridge
* 《软件栈》：“部署阶段：kernel 与 weights 走两条不同的路”

***

## 1　定位与边界

一颗 chip 是 2×5 的 Core 阵列，加四个 C2C Bridge、一个 SCP、一条 ctrl_noc。48 颗 chip 怎么摆、四个 chip 口接到哪，是 LPU 的事；chip 只把四个口露出来。

这一层有四个对象：

| 对象 | 形态 | 数量 |
| - | - | - |
| Chip | 装配容器，不打拍 | 1 |
| SCP 桩 | 独立打拍的模块 | 1 |
| ctrl_noc 端点 | 独立打拍的模块 | 10，每 core 一个 |
| C2C Bridge | 独立打拍的模块 | 4，每行左右两端各一个 |

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1390 720" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
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
  <rect x="0" y="0" width="1390" height="720" fill="#ffffff"/>
  <text x="20" y="26" font-size="12" fill="#111827">Chip · 第 0 层</text>
  <text x="152" y="26" font-size="9.5" fill="#6b7280">2×5 core 阵列，row-major 编号；每行左右两端接一个 C2C Bridge</text>
  <rect x="268" y="112" width="146" height="106" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="280" y="133" font-size="10.5" fill="#111827">core0</text>
  <text x="280" y="150" font-size="8.5" fill="#475569">Router · TS</text>
  <text x="280" y="163.5" font-size="8.5" fill="#475569">RV core ×3</text>
  <text x="280" y="177.0" font-size="8.5" fill="#475569">DTE / MU / VU DSA</text>
  <text x="280" y="190.5" font-size="8.5" fill="#475569">Cmem · Mmem · Smem</text>
  <rect x="432" y="112" width="146" height="106" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="444" y="133" font-size="10.5" fill="#111827">core1</text>
  <text x="444" y="150" font-size="8.5" fill="#475569">Router · TS</text>
  <text x="444" y="163.5" font-size="8.5" fill="#475569">RV core ×3</text>
  <text x="444" y="177.0" font-size="8.5" fill="#475569">DTE / MU / VU DSA</text>
  <text x="444" y="190.5" font-size="8.5" fill="#475569">Cmem · Mmem · Smem</text>
  <rect x="596" y="112" width="146" height="106" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="608" y="133" font-size="10.5" fill="#111827">core2</text>
  <text x="608" y="150" font-size="8.5" fill="#475569">Router · TS</text>
  <text x="608" y="163.5" font-size="8.5" fill="#475569">RV core ×3</text>
  <text x="608" y="177.0" font-size="8.5" fill="#475569">DTE / MU / VU DSA</text>
  <text x="608" y="190.5" font-size="8.5" fill="#475569">Cmem · Mmem · Smem</text>
  <rect x="760" y="112" width="146" height="106" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="772" y="133" font-size="10.5" fill="#111827">core3</text>
  <text x="772" y="150" font-size="8.5" fill="#475569">Router · TS</text>
  <text x="772" y="163.5" font-size="8.5" fill="#475569">RV core ×3</text>
  <text x="772" y="177.0" font-size="8.5" fill="#475569">DTE / MU / VU DSA</text>
  <text x="772" y="190.5" font-size="8.5" fill="#475569">Cmem · Mmem · Smem</text>
  <rect x="924" y="112" width="146" height="106" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="936" y="133" font-size="10.5" fill="#111827">core4</text>
  <text x="936" y="150" font-size="8.5" fill="#475569">Router · TS</text>
  <text x="936" y="163.5" font-size="8.5" fill="#475569">RV core ×3</text>
  <text x="936" y="177.0" font-size="8.5" fill="#475569">DTE / MU / VU DSA</text>
  <text x="936" y="190.5" font-size="8.5" fill="#475569">Cmem · Mmem · Smem</text>
  <rect x="268" y="300" width="146" height="106" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="280" y="321" font-size="10.5" fill="#111827">core5</text>
  <text x="280" y="338" font-size="8.5" fill="#475569">Router · TS</text>
  <text x="280" y="351.5" font-size="8.5" fill="#475569">RV core ×3</text>
  <text x="280" y="365.0" font-size="8.5" fill="#475569">DTE / MU / VU DSA</text>
  <text x="280" y="378.5" font-size="8.5" fill="#475569">Cmem · Mmem · Smem</text>
  <rect x="432" y="300" width="146" height="106" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="444" y="321" font-size="10.5" fill="#111827">core6</text>
  <text x="444" y="338" font-size="8.5" fill="#475569">Router · TS</text>
  <text x="444" y="351.5" font-size="8.5" fill="#475569">RV core ×3</text>
  <text x="444" y="365.0" font-size="8.5" fill="#475569">DTE / MU / VU DSA</text>
  <text x="444" y="378.5" font-size="8.5" fill="#475569">Cmem · Mmem · Smem</text>
  <rect x="596" y="300" width="146" height="106" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="608" y="321" font-size="10.5" fill="#111827">core7</text>
  <text x="608" y="338" font-size="8.5" fill="#475569">Router · TS</text>
  <text x="608" y="351.5" font-size="8.5" fill="#475569">RV core ×3</text>
  <text x="608" y="365.0" font-size="8.5" fill="#475569">DTE / MU / VU DSA</text>
  <text x="608" y="378.5" font-size="8.5" fill="#475569">Cmem · Mmem · Smem</text>
  <rect x="760" y="300" width="146" height="106" fill="#fdf6ec" stroke="#b45309" rx="4"/>
  <text x="772" y="321" font-size="10.5" fill="#111827">core8</text>
  <text x="772" y="338" font-size="8.5" fill="#475569">只构造 Router 八个模块</text>
  <text x="772" y="351.5" font-size="8.5" fill="#475569">local 侧禁用 · 不接收溢流</text>
  <text x="772" y="365.0" font-size="8.5" fill="#475569">credit 跨过它透传</text>
  <text x="772" y="378.5" font-size="8.5" fill="#475569">只按路由表转发</text>
  <text x="894" y="397" font-size="8.5" fill="#9ca3af" text-anchor="end">坏核示例</text>
  <rect x="924" y="300" width="146" height="106" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="936" y="321" font-size="10.5" fill="#111827">core9</text>
  <text x="936" y="338" font-size="8.5" fill="#475569">Router · TS</text>
  <text x="936" y="351.5" font-size="8.5" fill="#475569">RV core ×3</text>
  <text x="936" y="365.0" font-size="8.5" fill="#475569">DTE / MU / VU DSA</text>
  <text x="936" y="378.5" font-size="8.5" fill="#475569">Cmem · Mmem · Smem</text>
  <polyline points="414,142 432,142" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="578,142 596,142" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="742,142 760,142" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="906,142 924,142" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="414,330 432,330" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="578,330 596,330" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="742,330 760,330" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="906,330 924,330" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <text x="700" y="100" font-size="8.5" fill="#6b7280" text-anchor="middle">left / right：同行相邻 Router，256 B/T，40T</text>
  <polyline points="382,218 382,300" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="546,218 546,300" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="710,218 710,300" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="874,218 874,300" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="1038,218 1038,300" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <text x="1024" y="258" font-size="8.5" fill="#6b7280" text-anchor="start">mid：另一行对称位置的 Router，mid-to-mid 直连</text>
  <rect x="106" y="112" width="124" height="106" fill="#eef2ff" stroke="#4338ca" rx="4"/>
  <text x="118" y="133" font-size="10.5" fill="#111827">C2C Bridge</text>
  <text x="118" y="150" font-size="8" fill="#475569">简化 Router（RC/VA/SA）</text>
  <text x="118" y="163.5" font-size="8" fill="#475569">TX：4 KB 拆包 + seq_id</text>
  <text x="118" y="177.0" font-size="8" fill="#475569">RX：按 seq_id 拼包</text>
  <text x="118" y="190.5" font-size="8" fill="#475569">AXI Bridge · credit 透传</text>
  <text x="218" y="209" font-size="8.5" fill="#9ca3af" text-anchor="end">接 core0 左端</text>
  <polygon points="22,148 86,148 77,180 13,180" fill="#f8fafc" stroke="#374151"/>
  <text x="50" y="168" font-size="9" fill="#374151" text-anchor="middle">c2c[N]</text>
  <rect x="1110" y="112" width="124" height="106" fill="#eef2ff" stroke="#4338ca" rx="4"/>
  <text x="1122" y="133" font-size="10.5" fill="#111827">C2C Bridge</text>
  <text x="1122" y="150" font-size="8" fill="#475569">简化 Router（RC/VA/SA）</text>
  <text x="1122" y="163.5" font-size="8" fill="#475569">TX：4 KB 拆包 + seq_id</text>
  <text x="1122" y="177.0" font-size="8" fill="#475569">RX：按 seq_id 拼包</text>
  <text x="1122" y="190.5" font-size="8" fill="#475569">AXI Bridge · credit 透传</text>
  <text x="1222" y="209" font-size="8.5" fill="#9ca3af" text-anchor="end">接 core4 右端</text>
  <polygon points="1288,148 1352,148 1343,180 1279,180" fill="#f8fafc" stroke="#374151"/>
  <text x="1316" y="168" font-size="9" fill="#374151" text-anchor="middle">c2c[E]</text>
  <rect x="106" y="300" width="124" height="106" fill="#eef2ff" stroke="#4338ca" rx="4"/>
  <text x="118" y="321" font-size="10.5" fill="#111827">C2C Bridge</text>
  <text x="118" y="338" font-size="8" fill="#475569">简化 Router（RC/VA/SA）</text>
  <text x="118" y="351.5" font-size="8" fill="#475569">TX：4 KB 拆包 + seq_id</text>
  <text x="118" y="365.0" font-size="8" fill="#475569">RX：按 seq_id 拼包</text>
  <text x="118" y="378.5" font-size="8" fill="#475569">AXI Bridge · credit 透传</text>
  <text x="218" y="397" font-size="8.5" fill="#9ca3af" text-anchor="end">接 core5 左端</text>
  <polygon points="22,336 86,336 77,368 13,368" fill="#f8fafc" stroke="#374151"/>
  <text x="50" y="356" font-size="9" fill="#374151" text-anchor="middle">c2c[W]</text>
  <rect x="1110" y="300" width="124" height="106" fill="#eef2ff" stroke="#4338ca" rx="4"/>
  <text x="1122" y="321" font-size="10.5" fill="#111827">C2C Bridge</text>
  <text x="1122" y="338" font-size="8" fill="#475569">简化 Router（RC/VA/SA）</text>
  <text x="1122" y="351.5" font-size="8" fill="#475569">TX：4 KB 拆包 + seq_id</text>
  <text x="1122" y="365.0" font-size="8" fill="#475569">RX：按 seq_id 拼包</text>
  <text x="1122" y="378.5" font-size="8" fill="#475569">AXI Bridge · credit 透传</text>
  <text x="1222" y="397" font-size="8.5" fill="#9ca3af" text-anchor="end">接 core9 右端</text>
  <polygon points="1288,336 1352,336 1343,368 1279,368" fill="#f8fafc" stroke="#374151"/>
  <text x="1316" y="356" font-size="9" fill="#374151" text-anchor="middle">c2c[S]</text>
  <polyline points="230,178 268,178" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="1070,178 1110,178" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="230,366 268,366" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="1070,366 1110,366" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="86,164 106,164" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="1234,164 1279,164" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="86,352 106,352" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="1234,352 1279,352" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <text x="700" y="438" font-size="8.5" fill="#6b7280" text-anchor="middle">四个 chip 口由 LPU 接到相邻 chip 或 PCIe Switch；C2C 当作一种长延迟的 R2R，Router 到 Router 400T</text>
  <rect x="96" y="516" width="208" height="124" fill="#fbf3df" stroke="#b45309" rx="4"/>
  <text x="108" y="537" font-size="10.5" fill="#7c2d12">SCP 桩（每 chip 一个）</text>
  <text x="108" y="554" font-size="8.5" fill="#92400e">boot：自启动 → PCIe 训练</text>
  <text x="108" y="567.5" font-size="8.5" fill="#92400e">→ 顺序配 core0～core7</text>
  <text x="108" y="581.0" font-size="8.5" fill="#92400e">初始化六步 · 广播开关</text>
  <text x="108" y="594.5" font-size="8.5" fill="#92400e">weights 模式 ↔ 业务模式</text>
  <rect x="384" y="516" width="700" height="58" fill="#f5f3ff" stroke="#7c3aed" rx="4"/>
  <text x="396" y="537" font-size="11" fill="#111827">ctrl_noc（32 bit/T，每笔事务一拍）</text>
  <text x="396" y="554" font-size="8.5" fill="#475569">按地址分发 → 每 core 一个 ctrl_noc 端点 → 各模块的 cfg 口；core id 只读</text>
  <polyline points="304,551 344,551 344,545 384,545" fill="none" stroke="#7c3aed" marker-end="url(#p)"/>
  <text x="344" y="530" font-size="8.5" fill="#7c3aed" text-anchor="middle">scp_ctrl</text>
  <polyline points="298,516 298,461 297,461 297,406" fill="none" stroke="#7c3aed" stroke-dasharray="2 3" marker-end="url(#p)"/>
  <polyline points="462,516 462,461 461,461 461,406" fill="none" stroke="#7c3aed" stroke-dasharray="2 3" marker-end="url(#p)"/>
  <polyline points="626,516 626,461 625,461 625,406" fill="none" stroke="#7c3aed" stroke-dasharray="2 3" marker-end="url(#p)"/>
  <polyline points="790,516 790,461 789,461 789,406" fill="none" stroke="#7c3aed" stroke-dasharray="2 3" marker-end="url(#p)"/>
  <polyline points="954,516 954,461 953,461 953,406" fill="none" stroke="#7c3aed" stroke-dasharray="2 3" marker-end="url(#p)"/>
  <text x="1094" y="540" font-size="8.5" fill="#7c3aed" text-anchor="start">cfg 写事务 ↑ 每列两个 core</text>
  <polygon points="1220,600 1340,600 1331,630 1211,630" fill="#f8fafc" stroke="#374151"/>
  <text x="1276" y="619" font-size="8.5" fill="#374151" text-anchor="middle">async_int</text>
  <polyline points="1211,615 1189,615 1189,662 242,662 242,640" fill="none" stroke="#b45309" stroke-dasharray="4 3" marker-end="url(#o)"/>
  <text x="700" y="660" font-size="8.5" fill="#b45309" text-anchor="middle">async_int：core_status → SCP（本轮只留接口名）</text>
  <text x="20" y="700" font-size="10.5" fill="#374151">Chip 不打拍，是装配容器：按 harvest mask 构造 10 个 Core、接 mesh、接四个 C2C Bridge、接 ctrl_noc。SCP 桩、ctrl_noc 端点、C2C Bridge 是本层三种独立打拍的模块。</text>
</svg>
```

***

## 2　功能清单

一功能一条，编号供“机制覆盖”一章引用。

### Chip 装配（构造期，不打拍）

| 编号 | 功能 |
| - | - |
| F1 | 按本 chip 的 `harvest_mask` 构造 10 个 Core；mask 标为坏的那几个只构造 Router 的八个模块，不构造 TS、RV core、DSA 与三块存储 |
| F2 | 构造期断言：坏核不承担 logical compute core、EP broadcast core、EP reduction core，也不承担任何需要访问 local memory 的源或目的 |
| F3 | 同行相邻 core 的 Router `left` 与 `right` 端口用一对 Link 对接，走 R2R 参数 |
| F4 | `core[i]` 与 `core[i+5]` 的 `mid` 端口对接，即另一行对称位置的 Router，mid-to-mid 直连，同样走 R2R 参数 |
| F5 | 每行左右两端的 core 各接一个 C2C Bridge：core0 引到 chip 的 N 口、core4 引到 E、core5 引到 W、core9 引到 S |
| F6 | 每 core 构造一个 ctrl_noc 端点，按各单元文档声明的 `cfg` 口接线 |
| F7 | chip 类型由 `harvest_mask` 推出：0 个坏核是 A 型、1 个是 B 型、2 个是 C 型；每 chip 至多 2 个坏核，保证 8 个可用 |
| F8 | 坏核位置不同会让每颗 chip 的路由表不同，路由表按 chip 从编译侧读入，不写死 |

### SCP 桩

| 编号 | 功能 |
| - | - |
| F9 | boot 序列：自启动 → 完成 PCIe 链路训练 → 顺序配置启动 core0～core7 |
| F10 | ctrl_noc 广播开关：关时依次配每个 core，开时只发一次带广播标记的请求给 core0，由 core0 依次广播；默认关 |
| F11 | 初始化六步，按序做完：RV core firmware 写入 ITCM → DTE bootloader 写入 DTE RV core 的 ITCM → 配置 Bach core 解复位 → TS 初始化（任务链）→ Router 初始化（路由表）→ kernel 初始化 |
| F12 | RouterTable 的三份副本：等 Router 内部多副本提交完成后，软件才写 DTE 与 ReduceModule 的那两份，硬件不代为同步 |
| F13 | core 内 boot：把启动程序搬进三个 RV core 的 ITCM，启动三个 RV core 进 wait，确认 `ready` 全高后开放业务接收权限，Router 才开始接收业务 |
| F14 | TS 没有控制核，只有寄存器，复位清 0 后等外部启动，不需要装载程序 |
| F15 | 装载拍数按镜像字节数除以 4 B 计，与业务段用同一把尺 |
| F16 | weights 加载模式的配置：Router 路由表配成 weights 专用的 P2P 路径且只用 1 条 path，TS 的 datain 任务 `pc` 指向 weights loader、`trigger_task_chain_en = 0` |
| F17 | 加载一笔 weights 的四步：Router 收到数据通知 TS 触发 datain 任务 → TS 通知 DTE core 执行 → DTE core 跑 weights loader 算出落 Matrix Mem 的地址再发 DTE 指令搬运 → datain 完成通知 TS 释放，不触发任务链 |
| F18 | 切到业务模式改三处：Router 路由表换成业务路径、TS 的 datain `pc` 指向 token 搬移入口且 `trigger_task_chain_en = 1`、各 DSA 写入业务场景的静态配置 |
| F19 | 异常与中断从 `async_int` 收，转报给上层。本轮只留接口名与状态位，不实现行为 |

### ctrl_noc 端点

| 编号 | 功能 |
| - | - |
| F20 | 收 `scp_ctrl` 事务，`cfg_core` 命中本 core 或带广播标记时锁存，按 `addr_map` 查出目的模块 |
| F21 | 下一拍把事务写到目的模块的 `cfg` 口：TS 的 CFG_REG、RouterTable 的 CSR、三个 DSA 的寄存器、Share Mem、RV core 的 ITCM 与 DTCM、Core Mem 与 Matrix Mem 的后门 |
| F22 | 地址空间视野检查：地址不在本 core 视野内时记地址错。三个 RV core 各自看到 ITCM、DTCM、Share Mem、Core Mem 与对应 DSA 的 IO reg；DTE DSA 另可见 Matrix Mem；MU / VU DSA 只读 Matrix Mem；SCP 看到 core 内全部地址空间 |
| F23 | `core_id` 是只读寄存器，SCP 经 ctrl_noc 读 MMIO 取得，软件不可修改；weights 落到哪个 core 全靠它 |
| F24 | 读事务转给目的模块，`rdata` 下一拍回 |

### C2C Bridge

| 编号 | 功能 |
| - | - |
| F25 | 简化 Router：RC / VA / SA 完整流水线，与 core 内 Router 同一套逻辑 |
| F26 | TX Engine 拆包：按 4 KB 边界拆分，加 4-bit `seq_id` 与 tail 标记；位宽 2048 转 1024 |
| F27 | RX Engine 拼包：按 `seq_id` 缓存，tail 到齐后还原原始包；位宽 1024 转 2048 |
| F28 | AXI Bridge 做 credit 与 AXI4 的协议转换 |
| F29 | 同向的数据与 credit release 之间做仲裁，小包优先；反向按类型 demux 分流 |
| F30 | TX 方向的 AXI write 是 posted，写响应可以丢 |
| F31 | RX 方向的 AXI 需要响应，由 AXI Bridge 返回 dummy response，释放 PCIe 的 outstanding 资源 |
| F32 | VC Buffer 按方向分档：TX 每 VC private 20 flit 共 4 个，加 shared 约 20 flit，覆盖本级 R2R 往返约 20 cycle；RX private 80 flit，加 shared 约 300 flit，覆盖 PCIe 往返 600 ns @1024-bit |
| F33 | 跨 chip 时同步上下游的 Reduce credit，防止上游超发；release 的粒度是 flit，在 C2C 上压缩包数量后再传 |
| F34 | 三类 credit 的 release 一律透传，Bridge 自身不建 Stream 资源表，也不参与 Reduce 累加 |

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
mem harvest_mask   FF        10 b                                                  1R    编译侧读入      复位由输入给   // 本 chip 的坏核位图
mem chip_type      FF        {A, B, C}                                             1R    由 harvest_mask 推出            // 0 / 1 / 2 个坏核
mem logical_map    FF 阵列   10 × {logical_core[3:0], role[2:0]}                    1R    编译侧读入      复位由输入给   // 逻辑 core 编号与角色
mem addr_map       FF 阵列   N × {base[23:0], size, target_module, target_core}     1R    静态            复位由输入给   // ctrl_noc 地址分发表
mem core_id_reg[10] FF       只读 core id                                           1R    SCP 经 ctrl_noc 读，不可改     复位固定
mem scp_fsm        FF        {state[3:0], core_idx[3:0], step[2:0], cursor[31:0]}   1RW   boot 序列       复位 自启动
mem scp_img        FF 阵列   配置事务序列（firmware、bootloader、任务链、路由表、kernel、DSA 静态配置）  1R  编译侧读入  复位由输入给
mem noc_latch[10]  级间 latch {valid, addr[23:0], we, wdata[31:0]}                  —     每拍覆写        —              // 端点 → 目的模块 cfg 口
mem noc_rdata      1-deep 寄存器 {rdata[31:0]}                                      1W1R  每拍覆写        —
mem tx_vc_buf[4]   FIFO      每 VC 20 flit                                          1W1R  满 → 不再准入   复位空         // C2C Bridge TX 的 private buffer
mem tx_shared      FIFO      约 20 flit                                             1W1R  private 满时借用 复位空
mem rx_vc_buf[4]   FIFO      每 VC 80 flit                                          1W1R  满 → 向 PCIe 侧反压  复位空
mem rx_shared      FIFO      约 300 flit                                            1W1R  同上            复位空
mem rx_reasm       FF 阵列   按 seq_id 的重组缓冲，16 项                             1RW   tail 到齐即还原 复位空         // C2C Bridge RX 拼包
mem c2c_credit     FF 阵列   每方向每 VC 一个计数器，加每 UserID 的 Reduce credit    1RW   透传与同步      复位由配置给
```

***

## 5　流水线总览

第 1 层图待各模块的逐级拍数定下来后补。

***

## 6　逐级行为

第 2 层图与每级的四要素待第 1 层图完成后补，级编号回标到第 1 层图。

***

## 7　参数汇总

```
ARRAY             2×5，row-major；core0 = 行 0 左端 → chip 的 N 口，core4 = 行 0 右端 → E，core5 = 行 1 左端 → W，core9 = 行 1 右端 → S
MESH              同行 left / right 相邻，跨行 mid 接另一行对称位置；每方向 256 B/T、40T
HARVEST           每 chip 至多 2 个坏核（暂定方案 3），保证 8 个可用；A 型 0 个、B 型 1 个、C 型 2 个
CTRL_NOC_BW       32 bit/T（待定）；每笔事务一拍
CTRL_NOC_BCAST    开关，默认关
PCIE_TRAIN_CYCLES 待定
ITCM 装载拍数      镜像字节数 / 4 B
C2C_BRIDGE        每 chip 4 个，分布在 mesh 两侧，不是每 core 一个
C2C_SPLIT         4 KB 边界拆包，seq_id 4 bit
C2C_WIDTH         TX 2048 → 1024，RX 1024 → 2048
C2C_VC_BUF        合计约 138.7 KB；TX private 20 flit/VC ×4 + shared 约 20，RX private 80 + shared 约 300
C2C_LATENCY       Router 到 Router 400T，PCIe C2C 64 GB/s、300 ns
```

***

## 8　机制覆盖

| 机制 | 功能 | 用例 |
| - | - | - |
| 2×5 阵列与三个 R2R 方向的连接规则 | F3、F4 | `chip_mesh_2x5` |
| Harvest disable mask 只关 EngineNode，RouterNode 仍可用 | F1 | `harvest_router_only` |
| 坏核不能承担 compute / B core / R core；可承担转发、多播、router reduce | F2 | `harvest_roles` |
| chip 类型从 mask 推出，路由表按 chip 读入不写死 | F7、F8 | `chip_type_from_mask` |
| 每行左右两端接 C2C Bridge，全 chip 共 4 个 | F5 | `c2c_bridge_four` |
| core id 由 SCP 经 ctrl_noc 读 MMIO，不可修改 | F23 | `core_id_readonly` |
| SCP boot 序列：自启动 → PCIe 训练 → 顺序配 core0～core7 | F9 | `boot_sequence` |
| ctrl_noc 广播开关 | F10 | `ctrl_noc_bcast` |
| 初始化六步 | F11 | `init_six_steps` |
| Router 多副本提交完成后软件再写 DTE 与 ReduceModule 副本 | F12 | `router_table_three_copies` |
| Core 内 boot：ITCM 装载 → 三个 RV core 进 wait → ready 全高 → 开放业务接收 | F13、F14 | `core_boot` |
| weights 加载模式：只用 1 条 P2P path，不启动任务链 | F16、F17 | `weights_load` |
| 切到业务模式 | F18 | `switch_to_business` |
| 地址空间视野 | F22 | `address_map` |
| C2C 拆包：4 KB 边界 + seq_id + tail | F26 | `c2c_split` |
| C2C 拼包：按 seq_id 缓存，tail 到齐还原 | F27 | `c2c_reassemble` |
| 同向数据与 credit release 仲裁，小包优先 | F29 | `c2c_arb_small_first` |
| TX posted write 丢响应，RX 返回 dummy response | F30、F31 | `c2c_axi_response` |
| 跨 chip 同步上下游 Reduce credit，release 按 flit 压缩后再传 | F33 | `c2c_reduce_credit` |

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
