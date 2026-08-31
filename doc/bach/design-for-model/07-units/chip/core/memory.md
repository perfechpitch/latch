# 存储子系统

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **Core Mem / Matrix Mem / Share Mem**

给实现 core 内三块存储的人：三个独立打拍的模块各自做哪些事、端口与存储怎么定。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《执行单元与存储》“存储子系统”：Core Mem、Matrix Mem、Share Mem
* 《Core 内硬件》：“Core Mem 的硬件多用户管理”“内存结构与容量”

***

## 1　定位与边界

三块存储各是一个模块，边界是各自的 master 端口组：

| 存储 | 接哪些 master |
| - | - |
| Core Mem | DTE、MU、VU 三个 DSA，加 DTE RV core、Router 的 CoreMem 重发、ctrl_noc |
| Matrix Mem | DTE、ctrl_noc、MU；MU 只读 |
| Share Mem | 三个 RV core 的 sm_lsq，加 DTE DSA 的 shareMem 写 |

内部结构与两条约定：

* 每个模块内部是 bank 阵列，加每 bank 一个仲裁器
* 请求在模块内按固定拍数走完，返回数据经出口端口交还 master
* `stream_id` 分片（`base(stream_id) = 分片大小 × stream_id`）在 master 侧的地址计算里做，存储模块只看物理地址

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1500 980" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
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
  <rect x="0" y="0" width="1500" height="980" fill="#ffffff"/>
  <text x="20" y="26" font-size="12" fill="#111827">存储子系统 · 第 0 层</text>
  <text x="163" y="26" font-size="9.5" fill="#6b7280">每个模块内部是 bank 阵列加每 bank 一个仲裁器；stream_id 分片在 master 侧的地址计算里做，存储模块只看物理地址</text>
  <rect x="300" y="60" width="420" height="410" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="304" y="64" width="412" height="402" fill="none" stroke="#374151"/>
  <text x="312" y="81" font-size="11" fill="#111827">Core Mem（Cmem）</text>
  <text x="312" y="98" font-size="8.5" fill="#475569">容量 (128 KB + 4 KB) × 8 bank = 1 MB + 32 KB，其中 32 KB 是寄存器</text>
  <text x="312" y="111.5" font-size="8.5" fill="#475569">每 bank：深度 1024、位宽 128 B 的 SRAM，另有 4 KB 寄存器存 scale</text>
  <text x="312" y="125.0" font-size="8.5" fill="#475569">　（地址深度 1024 × 4 B，与 SRAM 地址一一映射，128 B : 4 B）</text>
  <text x="312" y="138.5" font-size="8.5" fill="#475569">SRAM 单元 1024 × 139 bit = 128 data + 10 ecc + 1 mask 标志</text>
  <text x="312" y="152.0" font-size="8.5" fill="#475569">最大访存带宽 (128 + 4) B × 8 bank = (1 KB + 32 B)/T</text>
  <text x="312" y="165.5" font-size="8.5" fill="#475569">访问延迟：请求进 CM 到读出或返回 bvalid，15T 以内</text>
  <text x="312" y="179.0" font-size="8.5" fill="#475569">地址粒度 128 B + 4 B，支持按 Byte mask 读写；时钟域 1 GHz</text>
  <rect x="314" y="300" width="42" height="150" fill="#eef2ff" stroke="#4338ca" rx="3"/>
  <text x="335" y="318" font-size="8" fill="#312e81" text-anchor="middle">bank0</text>
  <text x="335" y="338" font-size="7.5" fill="#4338ca" text-anchor="middle">SRAM</text>
  <text x="335" y="350" font-size="7.5" fill="#4338ca" text-anchor="middle">1024</text>
  <text x="335" y="362" font-size="7.5" fill="#4338ca" text-anchor="middle">×128B</text>
  <text x="335" y="382" font-size="7.5" fill="#4338ca" text-anchor="middle">scale</text>
  <text x="335" y="394" font-size="7.5" fill="#4338ca" text-anchor="middle">4 KB</text>
  <text x="335" y="416" font-size="7.5" fill="#818cf8" text-anchor="middle">arb</text>
  <text x="335" y="432" font-size="7.5" fill="#818cf8" text-anchor="middle">二选一</text>
  <rect x="364" y="300" width="42" height="150" fill="#eef2ff" stroke="#4338ca" rx="3"/>
  <text x="385" y="318" font-size="8" fill="#312e81" text-anchor="middle">bank1</text>
  <text x="385" y="338" font-size="7.5" fill="#4338ca" text-anchor="middle">SRAM</text>
  <text x="385" y="350" font-size="7.5" fill="#4338ca" text-anchor="middle">1024</text>
  <text x="385" y="362" font-size="7.5" fill="#4338ca" text-anchor="middle">×128B</text>
  <text x="385" y="382" font-size="7.5" fill="#4338ca" text-anchor="middle">scale</text>
  <text x="385" y="394" font-size="7.5" fill="#4338ca" text-anchor="middle">4 KB</text>
  <text x="385" y="416" font-size="7.5" fill="#818cf8" text-anchor="middle">arb</text>
  <text x="385" y="432" font-size="7.5" fill="#818cf8" text-anchor="middle">二选一</text>
  <rect x="414" y="300" width="42" height="150" fill="#eef2ff" stroke="#4338ca" rx="3"/>
  <text x="435" y="318" font-size="8" fill="#312e81" text-anchor="middle">bank2</text>
  <text x="435" y="338" font-size="7.5" fill="#4338ca" text-anchor="middle">SRAM</text>
  <text x="435" y="350" font-size="7.5" fill="#4338ca" text-anchor="middle">1024</text>
  <text x="435" y="362" font-size="7.5" fill="#4338ca" text-anchor="middle">×128B</text>
  <text x="435" y="382" font-size="7.5" fill="#4338ca" text-anchor="middle">scale</text>
  <text x="435" y="394" font-size="7.5" fill="#4338ca" text-anchor="middle">4 KB</text>
  <text x="435" y="416" font-size="7.5" fill="#818cf8" text-anchor="middle">arb</text>
  <text x="435" y="432" font-size="7.5" fill="#818cf8" text-anchor="middle">二选一</text>
  <rect x="464" y="300" width="42" height="150" fill="#eef2ff" stroke="#4338ca" rx="3"/>
  <text x="485" y="318" font-size="8" fill="#312e81" text-anchor="middle">bank3</text>
  <text x="485" y="338" font-size="7.5" fill="#4338ca" text-anchor="middle">SRAM</text>
  <text x="485" y="350" font-size="7.5" fill="#4338ca" text-anchor="middle">1024</text>
  <text x="485" y="362" font-size="7.5" fill="#4338ca" text-anchor="middle">×128B</text>
  <text x="485" y="382" font-size="7.5" fill="#4338ca" text-anchor="middle">scale</text>
  <text x="485" y="394" font-size="7.5" fill="#4338ca" text-anchor="middle">4 KB</text>
  <text x="485" y="416" font-size="7.5" fill="#818cf8" text-anchor="middle">arb</text>
  <text x="485" y="432" font-size="7.5" fill="#818cf8" text-anchor="middle">二选一</text>
  <rect x="514" y="300" width="42" height="150" fill="#eef2ff" stroke="#4338ca" rx="3"/>
  <text x="535" y="318" font-size="8" fill="#312e81" text-anchor="middle">bank4</text>
  <text x="535" y="338" font-size="7.5" fill="#4338ca" text-anchor="middle">SRAM</text>
  <text x="535" y="350" font-size="7.5" fill="#4338ca" text-anchor="middle">1024</text>
  <text x="535" y="362" font-size="7.5" fill="#4338ca" text-anchor="middle">×128B</text>
  <text x="535" y="382" font-size="7.5" fill="#4338ca" text-anchor="middle">scale</text>
  <text x="535" y="394" font-size="7.5" fill="#4338ca" text-anchor="middle">4 KB</text>
  <text x="535" y="416" font-size="7.5" fill="#818cf8" text-anchor="middle">arb</text>
  <text x="535" y="432" font-size="7.5" fill="#818cf8" text-anchor="middle">二选一</text>
  <rect x="564" y="300" width="42" height="150" fill="#eef2ff" stroke="#4338ca" rx="3"/>
  <text x="585" y="318" font-size="8" fill="#312e81" text-anchor="middle">bank5</text>
  <text x="585" y="338" font-size="7.5" fill="#4338ca" text-anchor="middle">SRAM</text>
  <text x="585" y="350" font-size="7.5" fill="#4338ca" text-anchor="middle">1024</text>
  <text x="585" y="362" font-size="7.5" fill="#4338ca" text-anchor="middle">×128B</text>
  <text x="585" y="382" font-size="7.5" fill="#4338ca" text-anchor="middle">scale</text>
  <text x="585" y="394" font-size="7.5" fill="#4338ca" text-anchor="middle">4 KB</text>
  <text x="585" y="416" font-size="7.5" fill="#818cf8" text-anchor="middle">arb</text>
  <text x="585" y="432" font-size="7.5" fill="#818cf8" text-anchor="middle">二选一</text>
  <rect x="614" y="300" width="42" height="150" fill="#eef2ff" stroke="#4338ca" rx="3"/>
  <text x="635" y="318" font-size="8" fill="#312e81" text-anchor="middle">bank6</text>
  <text x="635" y="338" font-size="7.5" fill="#4338ca" text-anchor="middle">SRAM</text>
  <text x="635" y="350" font-size="7.5" fill="#4338ca" text-anchor="middle">1024</text>
  <text x="635" y="362" font-size="7.5" fill="#4338ca" text-anchor="middle">×128B</text>
  <text x="635" y="382" font-size="7.5" fill="#4338ca" text-anchor="middle">scale</text>
  <text x="635" y="394" font-size="7.5" fill="#4338ca" text-anchor="middle">4 KB</text>
  <text x="635" y="416" font-size="7.5" fill="#818cf8" text-anchor="middle">arb</text>
  <text x="635" y="432" font-size="7.5" fill="#818cf8" text-anchor="middle">二选一</text>
  <rect x="664" y="300" width="42" height="150" fill="#eef2ff" stroke="#4338ca" rx="3"/>
  <text x="685" y="318" font-size="8" fill="#312e81" text-anchor="middle">bank7</text>
  <text x="685" y="338" font-size="7.5" fill="#4338ca" text-anchor="middle">SRAM</text>
  <text x="685" y="350" font-size="7.5" fill="#4338ca" text-anchor="middle">1024</text>
  <text x="685" y="362" font-size="7.5" fill="#4338ca" text-anchor="middle">×128B</text>
  <text x="685" y="382" font-size="7.5" fill="#4338ca" text-anchor="middle">scale</text>
  <text x="685" y="394" font-size="7.5" fill="#4338ca" text-anchor="middle">4 KB</text>
  <text x="685" y="416" font-size="7.5" fill="#818cf8" text-anchor="middle">arb</text>
  <text x="685" y="432" font-size="7.5" fill="#818cf8" text-anchor="middle">二选一</text>
  <rect x="1020" y="60" width="460" height="410" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="1024" y="64" width="452" height="402" fill="none" stroke="#374151"/>
  <text x="1032" y="81" font-size="11" fill="#111827">Matrix Mem（Mmem）</text>
  <text x="1032" y="98" font-size="8.5" fill="#475569">容量 32 + 4 MB。scale 模式下划出 4 MB 存 scale（scale : data = 1 : 8），</text>
  <text x="1032" y="111.5" font-size="8.5" fill="#475569">　32 MB 存正常数据；非 scale 模式下 36 MB 全存数据</text>
  <text x="1032" y="125.0" font-size="8.5" fill="#475569">按 64 个 lane 分成 64 bank，每 bank 0.5625 MB</text>
  <text x="1032" y="138.5" font-size="8.5" fill="#475569">每 bank 与 MU 的 lane 匹配，顶层拉齐不同 lane 的延迟</text>
  <text x="1032" y="152.0" font-size="8.5" fill="#475569">SRAM 单元 2048 × 128 bit，ECC 按 128 bit 一组</text>
  <text x="1032" y="165.5" font-size="8.5" fill="#475569">最大访存带宽 (8 + 1) KB/T（非 scale 模式 8 KB/T）</text>
  <text x="1032" y="179.0" font-size="8.5" fill="#475569">访问延迟：master 请求进 MM 到读出或 bvalid，50T 以内</text>
  <text x="1032" y="192.5" font-size="8.5" fill="#475569">地址粒度 128 B，不支持按 Byte mask 读写；时钟域 1 GHz</text>
  <rect x="1034" y="300" width="46" height="150" fill="#ecfdf5" stroke="#047857" rx="3"/>
  <text x="1057" y="318" font-size="8" fill="#064e3b" text-anchor="middle">bank0</text>
  <text x="1057" y="338" font-size="7.5" fill="#047857" text-anchor="middle">SRAM</text>
  <text x="1057" y="350" font-size="7.5" fill="#047857" text-anchor="middle">2048</text>
  <text x="1057" y="362" font-size="7.5" fill="#047857" text-anchor="middle">×128bit</text>
  <text x="1057" y="382" font-size="7.5" fill="#047857" text-anchor="middle">0.5625</text>
  <text x="1057" y="394" font-size="7.5" fill="#047857" text-anchor="middle">MB</text>
  <text x="1057" y="416" font-size="7.5" fill="#34d399" text-anchor="middle">与 lane</text>
  <text x="1057" y="432" font-size="7.5" fill="#34d399" text-anchor="middle">一对一</text>
  <rect x="1089" y="300" width="46" height="150" fill="#ecfdf5" stroke="#047857" rx="3"/>
  <text x="1112" y="318" font-size="8" fill="#064e3b" text-anchor="middle">bank1</text>
  <text x="1112" y="338" font-size="7.5" fill="#047857" text-anchor="middle">SRAM</text>
  <text x="1112" y="350" font-size="7.5" fill="#047857" text-anchor="middle">2048</text>
  <text x="1112" y="362" font-size="7.5" fill="#047857" text-anchor="middle">×128bit</text>
  <text x="1112" y="382" font-size="7.5" fill="#047857" text-anchor="middle">0.5625</text>
  <text x="1112" y="394" font-size="7.5" fill="#047857" text-anchor="middle">MB</text>
  <text x="1112" y="416" font-size="7.5" fill="#34d399" text-anchor="middle">与 lane</text>
  <text x="1112" y="432" font-size="7.5" fill="#34d399" text-anchor="middle">一对一</text>
  <rect x="1144" y="300" width="46" height="150" fill="#ecfdf5" stroke="#047857" rx="3"/>
  <text x="1167" y="318" font-size="8" fill="#064e3b" text-anchor="middle">bank2</text>
  <text x="1167" y="338" font-size="7.5" fill="#047857" text-anchor="middle">SRAM</text>
  <text x="1167" y="350" font-size="7.5" fill="#047857" text-anchor="middle">2048</text>
  <text x="1167" y="362" font-size="7.5" fill="#047857" text-anchor="middle">×128bit</text>
  <text x="1167" y="382" font-size="7.5" fill="#047857" text-anchor="middle">0.5625</text>
  <text x="1167" y="394" font-size="7.5" fill="#047857" text-anchor="middle">MB</text>
  <text x="1167" y="416" font-size="7.5" fill="#34d399" text-anchor="middle">与 lane</text>
  <text x="1167" y="432" font-size="7.5" fill="#34d399" text-anchor="middle">一对一</text>
  <rect x="1199" y="300" width="46" height="150" fill="#ecfdf5" stroke="#047857" rx="3"/>
  <text x="1222" y="318" font-size="8" fill="#064e3b" text-anchor="middle">bank3</text>
  <text x="1222" y="338" font-size="7.5" fill="#047857" text-anchor="middle">SRAM</text>
  <text x="1222" y="350" font-size="7.5" fill="#047857" text-anchor="middle">2048</text>
  <text x="1222" y="362" font-size="7.5" fill="#047857" text-anchor="middle">×128bit</text>
  <text x="1222" y="382" font-size="7.5" fill="#047857" text-anchor="middle">0.5625</text>
  <text x="1222" y="394" font-size="7.5" fill="#047857" text-anchor="middle">MB</text>
  <text x="1222" y="416" font-size="7.5" fill="#34d399" text-anchor="middle">与 lane</text>
  <text x="1222" y="432" font-size="7.5" fill="#34d399" text-anchor="middle">一对一</text>
  <rect x="1254" y="300" width="46" height="150" fill="#ecfdf5" stroke="#047857" rx="3"/>
  <text x="1277" y="318" font-size="8" fill="#064e3b" text-anchor="middle">bank4</text>
  <text x="1277" y="338" font-size="7.5" fill="#047857" text-anchor="middle">SRAM</text>
  <text x="1277" y="350" font-size="7.5" fill="#047857" text-anchor="middle">2048</text>
  <text x="1277" y="362" font-size="7.5" fill="#047857" text-anchor="middle">×128bit</text>
  <text x="1277" y="382" font-size="7.5" fill="#047857" text-anchor="middle">0.5625</text>
  <text x="1277" y="394" font-size="7.5" fill="#047857" text-anchor="middle">MB</text>
  <text x="1277" y="416" font-size="7.5" fill="#34d399" text-anchor="middle">与 lane</text>
  <text x="1277" y="432" font-size="7.5" fill="#34d399" text-anchor="middle">一对一</text>
  <rect x="1309" y="300" width="46" height="150" fill="#ecfdf5" stroke="#047857" rx="3"/>
  <text x="1332" y="318" font-size="8" fill="#064e3b" text-anchor="middle">bank5</text>
  <text x="1332" y="338" font-size="7.5" fill="#047857" text-anchor="middle">SRAM</text>
  <text x="1332" y="350" font-size="7.5" fill="#047857" text-anchor="middle">2048</text>
  <text x="1332" y="362" font-size="7.5" fill="#047857" text-anchor="middle">×128bit</text>
  <text x="1332" y="382" font-size="7.5" fill="#047857" text-anchor="middle">0.5625</text>
  <text x="1332" y="394" font-size="7.5" fill="#047857" text-anchor="middle">MB</text>
  <text x="1332" y="416" font-size="7.5" fill="#34d399" text-anchor="middle">与 lane</text>
  <text x="1332" y="432" font-size="7.5" fill="#34d399" text-anchor="middle">一对一</text>
  <rect x="1364" y="300" width="46" height="150" fill="#ecfdf5" stroke="#047857" rx="3"/>
  <text x="1387" y="318" font-size="8" fill="#064e3b" text-anchor="middle">bank6</text>
  <text x="1387" y="338" font-size="7.5" fill="#047857" text-anchor="middle">SRAM</text>
  <text x="1387" y="350" font-size="7.5" fill="#047857" text-anchor="middle">2048</text>
  <text x="1387" y="362" font-size="7.5" fill="#047857" text-anchor="middle">×128bit</text>
  <text x="1387" y="382" font-size="7.5" fill="#047857" text-anchor="middle">0.5625</text>
  <text x="1387" y="394" font-size="7.5" fill="#047857" text-anchor="middle">MB</text>
  <text x="1387" y="416" font-size="7.5" fill="#34d399" text-anchor="middle">与 lane</text>
  <text x="1387" y="432" font-size="7.5" fill="#34d399" text-anchor="middle">一对一</text>
  <rect x="1419" y="300" width="46" height="150" fill="#ecfdf5" stroke="#047857" rx="3"/>
  <text x="1442" y="318" font-size="8" fill="#064e3b" text-anchor="middle">… bank63</text>
  <text x="1442" y="338" font-size="7.5" fill="#047857" text-anchor="middle">SRAM</text>
  <text x="1442" y="350" font-size="7.5" fill="#047857" text-anchor="middle">2048</text>
  <text x="1442" y="362" font-size="7.5" fill="#047857" text-anchor="middle">×128bit</text>
  <text x="1442" y="382" font-size="7.5" fill="#047857" text-anchor="middle">0.5625</text>
  <text x="1442" y="394" font-size="7.5" fill="#047857" text-anchor="middle">MB</text>
  <text x="1442" y="416" font-size="7.5" fill="#34d399" text-anchor="middle">与 lane</text>
  <text x="1442" y="432" font-size="7.5" fill="#34d399" text-anchor="middle">一对一</text>
  <polygon points="56,96 226,96 217,126 47,126" fill="#f8fafc" stroke="#374151"/>
  <text x="137" y="115" font-size="8.5" fill="#374151" text-anchor="middle">cmem_dte_rd / wr</text>
  <text x="60" y="90" font-size="8.5" fill="#6b7280" text-anchor="start">256 B/T，13T</text>
  <polygon points="56,148 226,148 217,178 47,178" fill="#f8fafc" stroke="#374151"/>
  <text x="137" y="167" font-size="8.5" fill="#374151" text-anchor="middle">cmem_mu_rd / wr</text>
  <text x="60" y="142" font-size="8.5" fill="#6b7280" text-anchor="start">132 B/T，11T</text>
  <polygon points="56,200 226,200 217,230 47,230" fill="#f8fafc" stroke="#374151"/>
  <text x="137" y="219" font-size="8.5" fill="#374151" text-anchor="middle">cmem_vu_ld / st</text>
  <text x="60" y="194" font-size="8.5" fill="#6b7280" text-anchor="start">132 B/T，14T</text>
  <polygon points="56,252 226,252 217,282 47,282" fill="#f8fafc" stroke="#374151"/>
  <text x="137" y="271" font-size="8.5" fill="#374151" text-anchor="middle">cmem_rv</text>
  <text x="60" y="246" font-size="8.5" fill="#6b7280" text-anchor="start">128 B / 16 B / 2 B</text>
  <polygon points="56,304 226,304 217,334 47,334" fill="#f8fafc" stroke="#374151"/>
  <text x="137" y="323" font-size="8.5" fill="#374151" text-anchor="middle">cmem_reissue</text>
  <text x="60" y="298" font-size="8.5" fill="#6b7280" text-anchor="start">Router 重发，256 B</text>
  <polygon points="56,356 226,356 217,386 47,386" fill="#f8fafc" stroke="#374151"/>
  <text x="137" y="375" font-size="8.5" fill="#374151" text-anchor="middle">cmem_cfg</text>
  <text x="60" y="350" font-size="8.5" fill="#6b7280" text-anchor="start">ctrl_noc 4 B/T</text>
  <polyline points="226,111 300,111" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="226,163 300,163" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="226,215 300,215" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="226,267 300,267" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="226,319 300,319" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="226,371 300,371" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polygon points="790,150 960,150 951,180 781,180" fill="#f8fafc" stroke="#374151"/>
  <text x="871" y="169" font-size="8.5" fill="#374151" text-anchor="middle">mmem_dte_rd / wr</text>
  <text x="794" y="144" font-size="8.5" fill="#6b7280" text-anchor="start">256 B/T，读 8T / 写 9T</text>
  <polygon points="790,240 960,240 951,270 781,270" fill="#f8fafc" stroke="#374151"/>
  <text x="871" y="259" font-size="8.5" fill="#374151" text-anchor="middle">mmem_mu_rd</text>
  <text x="794" y="234" font-size="8.5" fill="#6b7280" text-anchor="start">只读 (8+1) KB/T，8T</text>
  <polygon points="790,330 960,330 951,360 781,360" fill="#f8fafc" stroke="#374151"/>
  <text x="871" y="349" font-size="8.5" fill="#374151" text-anchor="middle">mmem_cfg</text>
  <text x="794" y="324" font-size="8.5" fill="#6b7280" text-anchor="start">ctrl_noc 4 B/T，128 B 对齐，burst ≤ 32</text>
  <polyline points="960,165 1020,165" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="960,255 1020,255" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="960,345 1020,345" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <rect x="300" y="530" width="420" height="190" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="304" y="534" width="412" height="182" fill="none" stroke="#374151"/>
  <text x="312" y="551" font-size="11" fill="#111827">Share Mem</text>
  <text x="312" y="568" font-size="8.5" fill="#475569">容量 32 KB，访问延迟 5～10 拍，远短于 Core Mem 的 15～25 拍</text>
  <text x="312" y="581.5" font-size="8.5" fill="#475569">不需要初始化</text>
  <text x="312" y="595.0" font-size="8.5" fill="#475569">三种用途：task 之间的共享数据；B core / R core 的用户数据</text>
  <text x="312" y="608.5" font-size="8.5" fill="#475569">　映射表的更新与查询；标量数据</text>
  <text x="312" y="622.0" font-size="8.5" fill="#475569">master：三个 RV core 的 sm_lsq（32 bit）加 DTE DSA 的 shareMem 写</text>
  <text x="312" y="635.5" font-size="8.5" fill="#475569">四个 master 的仲裁规则原文未给，建模按轮询（待定）</text>
  <polygon points="56,552 226,552 217,582 47,582" fill="#f8fafc" stroke="#374151"/>
  <text x="137" y="571" font-size="8.5" fill="#374151" text-anchor="middle">sm_lsq［DTE］</text>
  <polyline points="226,567 300,567" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polygon points="56,592 226,592 217,622 47,622" fill="#f8fafc" stroke="#374151"/>
  <text x="137" y="611" font-size="8.5" fill="#374151" text-anchor="middle">sm_lsq［MU］</text>
  <polyline points="226,607 300,607" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polygon points="56,632 226,632 217,662 47,662" fill="#f8fafc" stroke="#374151"/>
  <text x="137" y="651" font-size="8.5" fill="#374151" text-anchor="middle">sm_lsq［VU］</text>
  <polyline points="226,647 300,647" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polygon points="56,672 226,672 217,702 47,702" fill="#f8fafc" stroke="#374151"/>
  <text x="137" y="691" font-size="8.5" fill="#374151" text-anchor="middle">smem_dte_wr</text>
  <polyline points="226,687 300,687" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <rect x="790" y="530" width="690" height="190" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="802" y="551" font-size="11" fill="#111827">仲裁规则</text>
  <text x="802" y="568" font-size="8.5" fill="#475569">Core Mem：每组读写端口有 bank 冲突时 arb 二选一，无冲突可同时访问</text>
  <text x="802" y="581.5" font-size="8.5" fill="#475569">　　　　　DTE 端口部分 bank 冲突时只反压冲突的那个 bank</text>
  <text x="802" y="595.0" font-size="8.5" fill="#475569">　　　　　同组内相同优先级：DTE 端口先做读写各自的 bank 冲突判断，再做 wr 与 rd 之间的冲突判断</text>
  <text x="802" y="608.5" font-size="8.5" fill="#475569">　　　　　非同组的优先级：MU &gt; VU = DTE；不增加 bank 冲突计数器</text>
  <text x="802" y="622.0" font-size="8.5" fill="#475569">Matrix Mem：DTE、ctrl_noc、MU 三者不能出现两个 master 同时访问相同 bank</text>
  <text x="802" y="635.5" font-size="8.5" fill="#475569">　　　　　　同时访问时只执行 MU 请求，并通过计数器记录报错。这是建模仲裁逻辑时必须体现的硬约束</text>
  <rect x="300" y="760" width="1180" height="180" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="312" y="781" font-size="11" fill="#111827">scale 与 ECC</text>
  <text x="312" y="798" font-size="8.5" fill="#475569">Core Mem 的 scale : data 比例最大 1 : 32；MU / VU 访问 CM 按 132 B 读写，只访问 SRAM 部分时有效带宽 128 B；scale 读写使能拉高时同时读写对应地址的 scale 寄存器</text>
  <text x="312" y="811.5" font-size="8.5" fill="#475569">byte_mask 非全 1 时 SRAM 内部留存记录，读取时不做 ECC 检测；全 1 时做 ECC 检测。4 B 的 scale 部分由寄存器搭建，不参与 ECC 机制</text>
  <text x="312" y="825.0" font-size="8.5" fill="#475569">Core Mem 的 ECC 按 128 bit 一组，编解码在 SRAM 接口处处理（而非随数据到各访问源端口），能减少 8% 数据传输功耗，代价是面积增加</text>
  <text x="312" y="838.5" font-size="8.5" fill="#475569">Core Mem 的 ECC 1 bit 错用计数器计数（每读端口 1 个、DTE / MU / VU 读写各 1 个），可经 NOC 读取；2 bit 错报错</text>
  <text x="312" y="852.0" font-size="8.5" fill="#475569">Matrix Mem 的 SRAM 内部支持单 bit 自纠错：读出时检测到单 bit 错，纠错后在 SRAM 空闲时写回对应地址覆盖原有错误数据</text>
  <text x="312" y="865.5" font-size="8.5" fill="#475569">Matrix Mem 按 core 角色扮演三种角色：普通计算 core 存 weight 供 MU 做 MoE 计算；reduction core 存 reduction 数据不存权重；</text>
  <text x="312" y="879.0" font-size="8.5" fill="#475569">　broadcast core 存 token 数据，防止专家不均衡影响其他 EP group 的 token 广播</text>
</svg>
```

***

## 2　功能清单

一功能一条，编号供“机制覆盖”一章引用。

### Core Mem

| 编号 | 功能 |
| - | - |
| F1 | 容量 (128 KB + 4 KB) × 8 bank = 1 MB + 32 KB，其中 32 KB 是寄存器 |
| F2 | 每 bank 是深度 1024、位宽 128 B 的 SRAM，另有 4 KB 寄存器存 scale（地址深度 1024 × 4 B，与 SRAM 地址一一映射，128 B : 4 B） |
| F3 | SRAM 单元 1024 × 139 bit（63 × 211 μm），139 bit = 128 data + 10 ecc + 1 mask 标志；每 bank 由 8 个 SRAM 加 4 KB scale 寄存器组成 |
| F4 | 最大访存带宽 (128 + 4) B × 8 bank = (1 KB + 32 B)/T |
| F5 | 访问延迟：数据请求进 CM 到读出或返回 bvalid，15T 以内 |
| F6 | 地址粒度 128 B + 4 B，支持按 Byte mask 读写 |
| F7 | 各 master 的带宽与延迟：DTE 读写各 256 B/T、13T；MU 读写各 132 B/T、**16T**；VU 读写各 132 B/T、14T；ctrl_noc 读写 4 B/T；DTE RV core 三种带宽 128 B / 16 B / 2 B |
| F8 | 每组读写端口有 bank 冲突时 arb 二选一，无冲突可同时访问 |
| F9 | DTE 端口部分 bank 冲突时只反压冲突的那个 bank |
| F10 | 同组内相同优先级：DTE 端口先做读写各自的 bank 冲突判断，再做 wr 与 rd 之间的冲突判断 |
| F11 | 非同组的优先级：MU > VU = DTE > {Router 的 CoreMem 重发、DTE RV core、ctrl_noc}。**后三个是平级的，谁先到谁先得**，不再分先后。定这个次序的依据：MU 的读是关键路径，前两档必须分开；后三档的流量都很小，排不排先后对吞吐没有可测的影响，平级反而少一套优先级逻辑 |
| F12 | 六个 master 都按 valid/ready 握手，未获授权的请求原地保持，本模块不丢请求。同优先级的 master 在 bank 内按先到先得排队，不允许任何一个被长期饿死 |
| F13 | 不增加 bank 冲突计数器 |
| F14 | scale : data 比例最大 1 : 32。MU / VU 访问 CM 按 132 B 读写，只访问 SRAM 部分时有效带宽 128 B；scale 读写使能拉高时同时读写对应地址的 scale 寄存器 |
| F15 | `byte_mask` 非全 1 时 SRAM 内部留存记录，读取时不做 ECC 检测；全 1 时做 ECC 检测。4 B 的 scale 部分由寄存器搭建，不参与 ECC 机制 |
| F16 | ECC 按 128 bit 一组，编解码在 SRAM 接口处处理而非随数据到各访问源端口 |
| F17 | ECC 1 bit 错用计数器计数（每读端口 1 个、DTE / MU / VU 读写各 1 个），可经 NOC 读取；2 bit 错报错。本轮只留状态位与接口名 |
| F18 | 按 `stream_num` 均等切分，单用户独占空间等于总容量除以 `stream_num`；分片的基址由 master 侧算好，本模块只看物理地址 |
| F19 | 存三类内容：业务流 token（message + data）、MU 计算结果、VU 计算结果；另按软件配置留出 P2P 阻塞缓冲与 broadcast 重发的暂存空间 |
| F20 | 分区由 `cmem_part` 一组配置寄存器定，boot 期经 ctrl_noc 写入，运行期不变：`stream_base` 与 `stream_stride`（16 个 stream 分片）、`scale_base`、`topk_base`、`header_base`、`reissue_base` 与 `reissue_pkts_per_vc`（Router 溢流重发的暂存区，每 VC 几个整包）、`p2p_base[3]` 与 `p2p_entries[3]`（每方向一张 P2P 阻塞缓冲）|
| F21 | 本模块只按物理地址读写，不检查请求落在哪个分区。分区之间不重叠由软件保证，写 `TS_INIT_FINISH` 那一步不查这组寄存器 |

### Matrix Mem

| 编号 | 功能 |
| - | - |
| F22 | 容量 32 + 4 MB。scale 模式下划出 4 MB 存 scale（scale : data = 1 : 8）、32 MB 存正常数据；非 scale 模式下 36 MB 全存数据 |
| F23 | 按 64 个 lane 分成 64 bank，每 bank 0.5625 MB；每 bank 与 MU 的 lane 匹配，顶层拉齐不同 lane 的延迟 |
| F24 | SRAM 单元 2048 × 128 bit，ECC 按 128 bit 一组 |
| F25 | 最大访存带宽 (8 + 1) KB/T（scale 模式；非 scale 模式最大 8 KB/T） |
| F26 | 访问延迟：master 请求进 MM 到读出或 bvalid，50T 以内 |
| F27 | 地址粒度 128 B，不支持按 Byte mask 读写 |
| F27a | DTE 与 ctrl_noc 按 128 B 写入。顺序写会先写满前 8 个 SRAM，再写 9～16 个，最后两个 scale SRAM 一次只收得下 128 bit × 2 = 32 B，接口利用率掉下来 |
| F27b | 改成循环写：把每个 SRAM（2048 × 128 bit）的深度按 512 拆成 4 组，轮流写，就能按 128 B 的带宽写满所有 SRAM。4608 个地址分成 9 组、每组深度 512，一次同时访问 8 个 128 bit 的 SRAM，合起来 128 B |
| F28 | 各 master 的带宽与 lane 内延迟：DTE 读写各 256 B/T（写 9T、读 8T）；MU 只读 (8 + 1) KB/T（8T）；ctrl_noc 读写 4 B/T，地址对齐 128 B、数据粒度 4 B、burst len 最大 32 |
| F29 | 硬约束：DTE、ctrl_noc、MU 三者不能出现两个 master 同时访问相同 bank；同时访问时只执行 MU 请求，并通过计数器记录报错 |
| F30 | 被让路的那一笔**直接丢弃**并计一次数，硬件不重试。这是硬约束被违反的表现，不是正常工作点：软件排算子时就该保证三个 master 不会撞同一个 bank。模型遇到这一笔**直接断言失败**，不做等价的重试掩盖 —— 真硬件上丢一笔就是 DTE 少搬一段数据，而 DTE 没有重传通路，结果直接错 |
| F31 | SRAM 内部支持单 bit 自纠错：读出时检测到单 bit 错，纠错后在 SRAM 空闲时写回对应地址覆盖原有错误数据 |
| F32 | 不做 stream 分片，每个用户看到的是相同的权重 |
| F33 | 按 core 角色扮演三种角色：普通计算 core 在权重加载阶段接收 DTE 从 Router 搬来的 weight 供 MU 做 MoE 计算；reduction core 存 reduction 数据不存权重；broadcast core 存 token 数据，防止专家不均衡影响其他 EP group 的 token 广播 |

### Share Mem

| 编号 | 功能 |
| - | - |
| F34 | 容量 32 KB，访问延迟 5～10 拍，远短于 Core Mem 的 15～25 拍 |
| F35 | 不需要初始化 |
| F36 | 只被三个 RV core 的访存指令与 DTE DSA 的 shareMem 写口读写 |
| F37 | 三种用途：task 之间的共享数据；B core / R core 的用户数据映射表的更新与查询；标量数据 |
| F38 | B core 用它存一对 `head` / `tail` 指针把收发两条链耦合起来；R core 用它存 `arrive_num[gpu_id][token_id]` 与两张 `tmp_info[stream_id]` |
| F39 | 四个 master 的仲裁规则原文未给，建模按轮询（待定） |

### Core Mem 容量口径的来源

| 编号 | 功能 |
| - | - |
| F40 | Core Mem 承担四类功能，容量推导按这四类相加：给 MU 计算提供 token；给 VU 计算提供 token；给 Concat / Reduce 提供缓存；给 MSG 提供 credit 缓存 |
| F41 | 第三类只落在最后一个 core 上：做 Concat 或 Reduce 的**最后一个** core，它的 token 要进 Core Memory 做汇总与流量控制缓存；中途逐级归约在 Router 里做，不占 Core Mem |
| F42 | 第四类里 broadcast 是必须的，P2P 是可选的（对应 `cmem_part` 里的 `p2p_base` / `p2p_entries`） |
| F43 | **Broadcast MSG 默认不能修改**，因为本 core 不知道下一级 core 什么时候能接收，原包要一直留着 |
| F44 | 容量按 MSG 算时，默认长度取“数据量 + 1 KB”，那 1 KB 是包头等额外信息的余量 |
| F45 | VU-DSA 内部有独立缓存，Core Mem **不需要**缓存 VU 计算的中间值，只存最终输出 |
| F46 | 精度对容量的影响：chip 内 Reduce 出于精度考虑要用 FP32，chip 间可以用 BF16。两者都在 Router 里做时不占额外 Core Mem 容量；改在 VU 里做就要额外一份，按 6144 个数算是 24 KiB（FP32）或 12 KiB（BF16） |
| F47 | 单用户容量按角色的分档（原始文档逐场景推出来的值）：FC0 DP2 TP4 约 30 KiB；Norm 约 25 KiB；Router core 约 21 KiB；EP-TP（chip 内外都切 inter）约 31.3 KiB；EP-TP（chip 内切 embedding）约 33.25 KiB；EP-PPTP FC3 约 16 KiB；EP-PPTP FC2 约 28.3 KiB。各档里列的分项之间可以部分复用 |
| F48 | 带宽的另一条算法：按容量需求乘 5 MTPS 折算，物理带宽 256 B/cycle @1 GHz 的前提下，每个用户允许写入 25 KiB 加读出 25 KiB。两种算法的误差来源是 MSG 包头，以及 Concat 时本 core 的数据被算了两次 |

***

## 3　接口

```
port cmem_dte_rd / cmem_dte_wr (slave, valid/ready, clk)   // DTE DSA，256 B/T
  in  req_valid · req_addr[17:0] · req_wdata[2047:0] · req_be[255:0] · req_scale_en
  out req_ready · rsp_valid · rsp_rdata[2047:0] · rsp_scale[63:0]
port cmem_mu_rd / cmem_mu_wr (slave, valid/ready, clk)     // MU DSA，132 B/T
  in  req_valid · req_addr[17:0] · req_wdata[1023:0] · req_scale[31:0] · req_be[127:0]
  out req_ready · rsp_valid · rsp_rdata[1023:0] · rsp_scale[31:0]
port cmem_vu_ld / cmem_vu_st (slave, valid/ready, clk)     // VU DSA，1056 bit，不 burst
  in  req_valid · req_addr[31:0] · req_wdata[1023:0] · req_scale[31:0]
  out req_ready · rsp_valid · rsp_rdata[1023:0] · rsp_scale[31:0]
port cmem_rv (slave, valid/ready, clk)                     // DTE RV core 的 cm_lsq，32 bit 取用
  in  req_valid · req_we · req_addr[17:0] · req_wdata[31:0] · req_be[3:0]
  out req_ready · rsp_valid · rsp_rdata[1055:0]              // 读固定回 1056 bit
port cmem_reissue (slave, valid/ready, clk)                // Router 的 CoreMem 重发暂存，256 B
  in  req_valid · req_we · req_addr[17:0] · req_wdata[2047:0]
  out req_ready · rsp_valid · rsp_rdata[2047:0]
port mmem_dte_rd / mmem_dte_wr (slave, valid/ready, clk)   // DTE DSA，256 B/T
  in  req_valid · req_addr[24:0] · req_wdata[2047:0]
  out req_ready · rsp_valid · rsp_rdata[2047:0]
port mmem_mu_rd (slave, valid/ready, clk)                  // MU DSA 只读，一个行地址广播到各 bank
  in  req_valid · req_addr[24:0]
  out req_ready · rsp_valid · rsp_rdata[65535:0] · rsp_scale[8191:0]
port smem_rv[u] (slave, valid/ready, clk)                  // u ∈ {DTE, MU, VU} 的 sm_lsq，32 bit
  in  req_valid · req_we · req_addr[14:0] · req_wdata[31:0] · req_be[3:0]
  out req_ready · rsp_valid · rsp_rdata[31:0]
port smem_dte_wr (slave, valid/ready, clk)                 // DTE DSA 的 shareMem 表项写
  in  req_valid · req_addr[14:0] · req_wdata[31:0]
  out req_ready
port cfg (slave, ctrl_noc 写事务, clk)                     // 三块存储各一个后门口，4 B/T
  in  cfg_valid · cfg_addr[23:0] · cfg_we · cfg_wdata[31:0]
  out cfg_rdata[31:0]
```

***

## 4　存储器

```
mem cmem_sram[8]    SRAM      每 bank 1024 × 139 bit（128 data + 10 ecc + 1 mask 标志），合计 1 MB   1R1W  每 bank 二选一  复位未定义
mem cmem_scale[8]   FF 阵列   每 bank 1024 × 4 B = 4 KB，与 SRAM 地址一一映射                        1R1W  scale 使能时同读同写  复位未定义
mem cmem_part       FF        {stream_base, stream_stride, scale_base, topk_base, header_base, reissue_base, reissue_pkts_per_vc[3:0], p2p_base[3], p2p_entries[3]}  1R1W  ctrl_noc 于 boot 期写入  复位 0
mem cmem_ecc_cnt    FF 阵列   每读端口 1 个、DTE / MU / VU 读写各 1 个                               1RW   1 bit 错计数    复位 0
mem cmem_arb[8]     FF        每 bank 一个仲裁器状态                                                 1RW   —              复位 空闲
mem mmem_sram[64]   SRAM      每 bank 2048 × 128 bit × N，合计 0.5625 MB / bank                     1R1W  同 bank 不许两个 master  复位未定义
mem mmem_scale      SRAM      4 MB（scale : data = 1 : 8）                                          1R1W  scale 模式下划出  复位未定义
mem mmem_err_cnt    FF        同 bank 冲突计数器                                                     1RW   冲突时只执行 MU 并计数  复位 0
mem smem_sram       SRAM      32 KB                                                                 1R1W  四个 master 轮询  复位未定义（不需要初始化）
mem 延迟线           FF 阵列   每个 master 端口一条，按各自的固定拍数把响应推回出口                     1RW   —              复位空
```

***

## 5　流水线总览

三块存储在模型里是同一套四级：请求锁存、bank 仲裁、SRAM 读写、延迟线与响应。差别只在 M3 与 M4 合起来的拍数，按 master 取值，四级之和等于该 master 的端到端延迟。

| 存储 | master | 四级之和 |
| - | - | - |
| Core Mem | DTE DSA | 13 T |
| Core Mem | MU DSA | 11 T |
| Core Mem | VU DSA | 14 T |
| Core Mem | DTE RV core、CoreMem 重发、ctrl_noc | 15 T（取 Cmem 的上界） |
| Matrix Mem | DTE DSA | 写 9 T、读 8 T |
| Matrix Mem | MU DSA | 8 T |
| Matrix Mem | ctrl_noc | 50 T（取 Mmem 的上界） |
| Share Mem | 三个 RV core、DTE DSA | 5～10 T |

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 844 436" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arsov" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="844" height="436" fill="#ffffff"/>

  <text x="20" y="26" font-size="12" fill="#111827">存储子系统 · 第 1 层流水线总览（三块存储同一套四级，差别在 SRAM 段的拍数）</text>
  <text x="20" y="42" font-size="9.5" fill="#6b7280">横向是级序，不是拍序；每级的拍数在右上角 Dx。橙色虚线框是变长级，非按比例。</text>
  <line x1="150" y1="52" x2="150" y2="328" stroke="#e5e7eb"/>
  <line x1="316" y1="52" x2="316" y2="328" stroke="#e5e7eb"/>
  <line x1="482" y1="52" x2="482" y2="328" stroke="#e5e7eb"/>
  <line x1="648" y1="52" x2="648" y2="328" stroke="#e5e7eb"/>
  <text x="20" y="102" font-size="10.5" fill="#6b7280">Core Mem</text>
  <rect x="150" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="84" font-size="8.5" fill="#6b7280">M1</text>
  <text x="292" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="104" font-size="11" fill="#111827">请求锁存</text>
  <rect x="316" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="84" font-size="8.5" fill="#6b7280">M2</text>
  <text x="458" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="104" font-size="11" fill="#111827">bank 仲裁</text>
  <line x1="300" y1="98" x2="314" y2="98" stroke="#475569" marker-end="url(#arsov)"/>
  <rect x="482" y="70" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="492" y="84" font-size="8.5" fill="#92400e">M3</text>
  <text x="624" y="84" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="492" y="104" font-size="11" fill="#7c2d12">SRAM 读写</text>
  <line x1="466" y1="98" x2="480" y2="98" stroke="#475569" marker-end="url(#arsov)"/>
  <rect x="648" y="70" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="658" y="84" font-size="8.5" fill="#92400e">M4</text>
  <text x="790" y="84" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="658" y="104" font-size="11" fill="#7c2d12">延迟线与响应</text>
  <line x1="632" y1="98" x2="646" y2="98" stroke="#475569" marker-end="url(#arsov)"/>
  <text x="20" y="188" font-size="10.5" fill="#6b7280">Matrix Mem</text>
  <rect x="150" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="170" font-size="8.5" fill="#6b7280">M1</text>
  <text x="292" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="190" font-size="11" fill="#111827">请求锁存</text>
  <rect x="316" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="170" font-size="8.5" fill="#6b7280">M2</text>
  <text x="458" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="190" font-size="11" fill="#111827">bank 仲裁</text>
  <line x1="300" y1="184" x2="314" y2="184" stroke="#475569" marker-end="url(#arsov)"/>
  <rect x="482" y="156" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="492" y="170" font-size="8.5" fill="#92400e">M3</text>
  <text x="624" y="170" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="492" y="190" font-size="11" fill="#7c2d12">SRAM 读写</text>
  <line x1="466" y1="184" x2="480" y2="184" stroke="#475569" marker-end="url(#arsov)"/>
  <rect x="648" y="156" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="658" y="170" font-size="8.5" fill="#92400e">M4</text>
  <text x="790" y="170" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="658" y="190" font-size="11" fill="#7c2d12">延迟线与响应</text>
  <line x1="632" y1="184" x2="646" y2="184" stroke="#475569" marker-end="url(#arsov)"/>
  <text x="20" y="274" font-size="10.5" fill="#6b7280">Share Mem</text>
  <rect x="150" y="242" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="256" font-size="8.5" fill="#6b7280">M1</text>
  <text x="292" y="256" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="276" font-size="11" fill="#111827">请求锁存</text>
  <rect x="316" y="242" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="256" font-size="8.5" fill="#6b7280">M2</text>
  <text x="458" y="256" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="276" font-size="11" fill="#111827">bank 仲裁</text>
  <line x1="300" y1="270" x2="314" y2="270" stroke="#475569" marker-end="url(#arsov)"/>
  <rect x="482" y="242" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="492" y="256" font-size="8.5" fill="#92400e">M3</text>
  <text x="624" y="256" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="492" y="276" font-size="11" fill="#7c2d12">SRAM 读写</text>
  <line x1="466" y1="270" x2="480" y2="270" stroke="#475569" marker-end="url(#arsov)"/>
  <rect x="648" y="242" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="658" y="256" font-size="8.5" fill="#92400e">M4</text>
  <text x="790" y="256" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="658" y="276" font-size="11" fill="#7c2d12">延迟线与响应</text>
  <line x1="632" y1="270" x2="646" y2="270" stroke="#475569" marker-end="url(#arsov)"/>
  <text x="20" y="352" font-size="10.5" fill="#374151">M3 与 M4 合起来的拍数按 master 定，四级之和等于该 master 的端到端延迟：Core Mem 的 DTE 13、MU 11、VU 14；Matrix Mem 的 DTE 写 9 读 8、MU 读 8；Share Mem 5～10。</text>
  <text x="20" y="380" font-size="10.5" fill="#374151">M2 每拍 TryGrant 一次，被拒的请求原地保持，下一拍重来，本模块不丢请求。</text>
  <text x="20" y="408" font-size="10.5" fill="#374151">Matrix Mem 同 bank 冲突时只执行 MU，被让路的 master 在 M2 停一拍，不丢那一笔。</text>
</svg>
```

***

## 6　逐级行为

### M1 · 请求锁存

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 879 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="ars1" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="879" height="198" fill="#ffffff"/>

  <polygon points="30,33 188,33 178,109 20,109" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="52" font-size="10.5" fill="#374151" text-anchor="middle">cmem_* / mmem_* / smem_*</text>
  <text x="104" y="70" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · req_addr</text>
  <text x="104" y="88" font-size="9.5" fill="#6b7280" text-anchor="middle">req_we · req_wdata</text>
  <text x="104" y="106" font-size="9.5" fill="#6b7280" text-anchor="middle">req_be · req_ready</text>
  <rect x="20" y="121" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="125" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="142" font-size="10" fill="#374151" text-anchor="middle">cmem_part · FF · 1R</text>
  <rect x="683" y="42" width="176" height="114" fill="#f1f5f9" stroke="#334155"/>
  <rect x="683" y="42" width="176" height="18" fill="#334155"/>
  <text x="771" y="55" font-size="10.5" fill="#ffffff" text-anchor="middle">REQ</text>
  <text x="771" y="82" font-size="10" fill="#334155" text-anchor="middle">bank[5:0]</text>
  <text x="771" y="104" font-size="10" fill="#334155" text-anchor="middle">addr · we</text>
  <text x="771" y="126" font-size="10" fill="#334155" text-anchor="middle">wdata · be</text>
  <text x="771" y="148" font-size="10" fill="#334155" text-anchor="middle">src[2:0]</text>
  <rect x="232" y="20" width="407" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M1</text>
  <text x="625" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">端口 · 锁存请求并算 bank 号</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. req_valid &amp;&amp; req_ready → 锁存 {addr, we, wdata, be, scale_en}</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. bank = addr[bank 位段]（Core Mem 8 bank、Matrix Mem 64 bank）</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. 分区由 cmem_part 定，基址由 master 侧算好，本模块只看物理地址</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. req_ready = 该 master 在 M2 没有被拒的请求压着</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">地址粒度：Core Mem 128 B + 4 B，Matrix Mem 128 B</text>
  <line x1="188" y1="71" x2="228" y2="71" stroke="#475569" marker-end="url(#ars1)"/>
  <line x1="188" y1="142" x2="228" y2="142" stroke="#475569" marker-end="url(#ars1)"/>
  <line x1="639" y1="99" x2="679" y2="99" stroke="#475569" marker-end="url(#ars1)"/>
</svg>
```

### M2 · bank 仲裁

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 881 218" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="ars2" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="881" height="218" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">REQ</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">bank[5:0]</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">src[2:0]</text>
  <rect x="20" y="102" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="106" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="123" font-size="10" fill="#374151" text-anchor="middle">cmem_arb[8] · FF 每 bank 1 份 · 1RW</text>
  <rect x="20" y="156" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="160" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="177" font-size="10" fill="#374151" text-anchor="middle">mmem_err_cnt · FF · 1RW</text>
  <rect x="685" y="63" width="176" height="92" fill="#f1f5f9" stroke="#334155"/>
  <rect x="685" y="63" width="176" height="18" fill="#334155"/>
  <text x="773" y="76" font-size="10.5" fill="#ffffff" text-anchor="middle">GRANT</text>
  <text x="773" y="103" font-size="10" fill="#334155" text-anchor="middle">bank[5:0]</text>
  <text x="773" y="125" font-size="10" fill="#334155" text-anchor="middle">src[2:0]</text>
  <text x="773" y="147" font-size="10" fill="#334155" text-anchor="middle">addr · we · wdata</text>
  <rect x="232" y="30" width="409" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="46" font-size="8.5" fill="#6b7280">M2</text>
  <text x="627" y="46" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="66" font-size="12" fill="#111827">bank_arbiter · 每 bank 每拍授一个</text>
  <text x="250" y="88" font-size="10.5" fill="#475569">1. Core Mem 同组内：DTE 端口先判读写各自的 bank 冲突，再判读写之间</text>
  <text x="250" y="108" font-size="10.5" fill="#475569">2. Core Mem 非同组：MU &gt; VU = DTE &gt; 重发 &gt; DTE RV core &gt; ctrl_noc</text>
  <text x="250" y="128" font-size="10.5" fill="#475569">3. Matrix Mem：同 bank 有两个 master → 只授 MU，mmem_err_cnt += 1</text>
  <text x="250" y="148" font-size="10.5" fill="#475569">4. 未获授权 → 该 master 的 req_ready = 0，请求原地保持下拍重试</text>
  <text x="250" y="172" font-size="10" fill="#9ca3af">同优先级按先到先得排队，不允许长期饿死</text>
  <line x1="188" y1="55" x2="228" y2="55" stroke="#475569" marker-end="url(#ars2)"/>
  <line x1="188" y1="123" x2="228" y2="123" stroke="#475569" marker-end="url(#ars2)"/>
  <line x1="188" y1="177" x2="228" y2="177" stroke="#475569" marker-end="url(#ars2)"/>
  <line x1="641" y1="109" x2="681" y2="109" stroke="#475569" marker-end="url(#ars2)"/>
</svg>
```

### M3 · SRAM 读写

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 887 326" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="ars3" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="887" height="326" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">GRANT</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">bank[5:0]</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">addr · we · wdata</text>
  <rect x="20" y="102" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="106" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="123" font-size="10" fill="#374151" text-anchor="middle">cmem_sram[8] · SRAM 1024×139 b · 1R1W</text>
  <rect x="20" y="156" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="160" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="177" font-size="10" fill="#374151" text-anchor="middle">cmem_scale[8] · FF 4 KB · 1R1W</text>
  <rect x="20" y="210" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="214" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="231" font-size="10" fill="#374151" text-anchor="middle">mmem_sram[64] · SRAM · 1R1W</text>
  <rect x="20" y="264" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="268" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="285" font-size="10" fill="#374151" text-anchor="middle">smem_sram · SRAM 32 KB · 1R1W</text>
  <rect x="691" y="117" width="176" height="92" fill="#f1f5f9" stroke="#334155"/>
  <rect x="691" y="117" width="176" height="18" fill="#334155"/>
  <text x="779" y="130" font-size="10.5" fill="#ffffff" text-anchor="middle">RDATA</text>
  <text x="779" y="157" font-size="10" fill="#334155" text-anchor="middle">rdata · scale</text>
  <text x="779" y="179" font-size="10" fill="#334155" text-anchor="middle">src[2:0]</text>
  <text x="779" y="201" font-size="10" fill="#334155" text-anchor="middle">ecc_err[1:0]</text>
  <rect x="232" y="84" width="415" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="100" font-size="8.5" fill="#6b7280">M3</text>
  <text x="633" y="100" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="250" y="120" font-size="12" fill="#111827">SRAM 阵列 · 按 bank 读写并处理 scale 与 ECC</text>
  <text x="250" y="142" font-size="10.5" fill="#475569">1. we → sram[bank][addr] = wdata（byte_mask 非全 1 时内部留存记录）</text>
  <text x="250" y="162" font-size="10.5" fill="#475569">2. !we → rdata = sram[bank][addr]；byte_mask 全 1 才做 ECC 检测</text>
  <text x="250" y="182" font-size="10.5" fill="#475569">3. scale_en → 同地址的 scale 寄存器一并读写（Core Mem 128 B : 4 B）</text>
  <text x="250" y="202" font-size="10.5" fill="#475569">4. Matrix Mem 读出单 bit 错 → 纠错，并在 SRAM 空闲时写回覆盖</text>
  <text x="250" y="226" font-size="10" fill="#9ca3af">ECC 按 128 bit 一组，编解码在 SRAM 接口处</text>
  <line x1="188" y1="55" x2="228" y2="55" stroke="#475569" marker-end="url(#ars3)"/>
  <line x1="188" y1="123" x2="228" y2="123" stroke="#475569" marker-end="url(#ars3)"/>
  <line x1="188" y1="177" x2="228" y2="177" stroke="#475569" marker-end="url(#ars3)"/>
  <line x1="188" y1="231" x2="228" y2="231" stroke="#475569" marker-end="url(#ars3)"/>
  <line x1="188" y1="285" x2="228" y2="285" stroke="#475569" marker-end="url(#ars3)"/>
  <line x1="647" y1="163" x2="687" y2="163" stroke="#475569" marker-end="url(#ars3)"/>
</svg>
```

### M4 · 延迟线与响应

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 876 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="ars4" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="876" height="198" fill="#ffffff"/>

  <rect x="20" y="37" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="37" width="168" height="18" fill="#334155"/>
  <text x="104" y="50" font-size="10.5" fill="#ffffff" text-anchor="middle">RDATA</text>
  <text x="104" y="77" font-size="10" fill="#334155" text-anchor="middle">rdata · scale</text>
  <text x="104" y="99" font-size="10" fill="#334155" text-anchor="middle">src[2:0]</text>
  <rect x="20" y="119" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="123" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="140" font-size="10" fill="#374151" text-anchor="middle">延迟线 · FF 每端口一条 · 1RW</text>
  <polygon points="690,69 856,69 846,127 680,127" fill="#f8fafc" stroke="#374151"/>
  <text x="768" y="88" font-size="10.5" fill="#374151" text-anchor="middle">cmem_* / mmem_* / smem_*</text>
  <text x="768" y="106" font-size="9.5" fill="#6b7280" text-anchor="middle">rsp_valid · rsp_rdata</text>
  <text x="768" y="124" font-size="9.5" fill="#6b7280" text-anchor="middle">rsp_scale</text>
  <rect x="232" y="20" width="404" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M4</text>
  <text x="622" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="250" y="56" font-size="12" fill="#111827">延迟线 · 补齐到该 master 的端到端拍数</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. 每个 master 端口一条延迟线，长度 = 该 master 总延迟 − 已走的拍数</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. 到点 → rsp_valid = 1，rsp_rdata 与 rsp_scale 送回该端口</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. 写请求到点 → bvalid 回该端口</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. Matrix Mem 顶层拉齐 64 个 lane 的延迟，各 lane 同拍回数</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">Core Mem DTE 13 / MU 11 / VU 14；Matrix Mem DTE 写 9 读 8 / MU 8</text>
  <line x1="188" y1="72" x2="228" y2="72" stroke="#475569" marker-end="url(#ars4)"/>
  <line x1="188" y1="140" x2="228" y2="140" stroke="#475569" marker-end="url(#ars4)"/>
  <line x1="636" y1="98" x2="676" y2="98" stroke="#475569" marker-end="url(#ars4)"/>
</svg>
```

***

## 7　参数汇总

```
Core Mem 容量        (128 KB + 4 KB) × 8 bank = 1 MB + 32 KB
Core Mem 带宽        (1 KB + 32 B)/T；地址粒度 128 B + 4 B，支持 byte mask
Core Mem 延迟        DTE 13T · MU 16T · VU 14T（MU 这一档按 MU 侧的 16T 记，Cmem MAS 写的 11T 是它自己那一侧的口径）
Core Mem 优先级       非同组 MU > VU = DTE > {重发、DTE RV core、ctrl_noc} 三者平级先到先得；同组内 DTE 先判读写各自冲突再判读写之间
Matrix Mem 容量      32 + 4 MB，64 bank × 0.5625 MB
Matrix Mem 带宽      (8 + 1) KB/T；地址粒度 128 B，不支持 byte mask
Matrix Mem 延迟      50T 以内；DTE 写 9T 读 8T · MU 读 8T
Matrix Mem 硬约束    同一 bank 不许两个 master 同时访问，冲突时只执行 MU、被让路的一笔丢弃并计数；模型直接断言失败
Core Mem 分区        由 cmem_part 定：stream 分片、scale、topk、包头、溢流重发暂存（每 VC 2 个整包，待定）、P2P 阻塞缓冲（≤ 3 个方向）
Share Mem            32 KB，5～10 拍，不需要初始化；仲裁按轮询（待定）
时钟域               三块都是 1 GHz
ECC                  Core Mem 按 128 bit 一组，编解码在 SRAM 接口处；Matrix Mem 单 bit 自纠错并在空闲时写回
硬件带宽对照          Matrix Memory 8192 GB/s · Core Memory 512 GB/s · Router 双向各 256 GB/s · DTE 数据通路需求 190～320 GB/s
单用户 Core Mem 容量  按角色 16～36 KiB；推 Core Mem 容量按 16 用户并发 × 单用户 30～50 KB
                     逐场景值：FC0 DP2 TP4 30 · Norm 25 · Router 21 · EP-TP 切 inter 31.3 · EP-TP 切 embedding 33.25 · EP-PPTP FC3 16 · EP-PPTP FC2 28.3（KiB）
MSG 容量折算          按“数据量 + 1 KB”算，那 1 KB 是包头等额外信息的余量
Matrix Mem bank 数    **口径冲突**：MU MAS 记 32 bank 与 32 lane 一对一，Mmem MAS 记 64 bank。未解，直接影响 8 KB/T 的组织方式
```

***

## 8　机制覆盖

| 机制 | 功能 | 用例 |
| - | - | - |
| Core Mem 8 bank，每 bank SRAM 加 scale 寄存器 | F1、F2 | `cmem_bank_layout` |
| 最大带宽 (1 KB + 32 B)/T | F4 | `cmem_bw` |
| 支持 byte mask 读写 | F6 | `cmem_byte_mask` |
| 各 master 的带宽与延迟 | F7 | `cmem_master_latency` |
| bank 冲突时 arb 二选一，无冲突同时访问 | F8 | `cmem_bank_arb` |
| DTE 部分 bank 冲突时只反压冲突的那个 bank | F9 | `cmem_partial_stall` |
| 同组内 DTE 先判读写各自冲突再判读写之间 | F10 | `cmem_same_group` |
| 非同组优先级 MU > VU = DTE > 重发 > RV core > ctrl_noc | F11 | `cmem_priority` |
| 六个 master 都按 valid/ready 保持，本模块不丢请求 | F12 | `cmem_no_drop` |
| scale 使能时同时读写对应地址的 scale 寄存器 | F14 | `cmem_scale` |
| byte_mask 非全 1 时不做 ECC 检测 | F15 | `cmem_mask_ecc` |
| ECC 编解码在 SRAM 接口处，1 bit 计数、2 bit 报错 | F16、F17 | `cmem_ecc` |
| 按 stream_num 均等切分，分片基址在 master 侧算 | F18 | `cmem_stream_slice` |
| cmem_part 定分区，boot 期写入，运行期不变 | F22、F23 | `cmem_partition` |
| Matrix Mem 64 bank 与 lane 匹配，顶层拉齐延迟 | F23 | `mmem_bank_lane` |
| 最大带宽 (8 + 1) KB/T，地址粒度 128 B 不支持 byte mask | F25、F27 | `mmem_bw` |
| 同一 bank 不许两个 master，冲突时只执行 MU 并计数 | F29 | `mmem_single_master` |
| 被让路的一笔丢弃并计数，模型直接断言失败 | F30 | `mmem_conflict_assert` |
| SRAM 单 bit 自纠错并在空闲时写回 | F31 | `mmem_ecc_writeback` |
| Matrix Mem 不做 stream 分片 | F32 | `mmem_no_slice` |
| Matrix Mem 按 core 角色扮演三种角色 | F33 | `mmem_roles` |
| Share Mem 32 KB、5～10 拍、不需要初始化 | F34、F35 | `smem_basic` |
| Share Mem 只被三个 RV core 与 DTE 读写 | F36 | `smem_masters` |
| B core 的 head / tail 与 R core 的 arrive_num 存在 Share Mem | F38 | `smem_tables` |
| Core Mem 容量按四类功能相加，逐级归约不占容量 | F40、F41 | `cmem_capacity_four` |
| Broadcast MSG 在本 core 不能修改 | F43 | `broadcast_msg_immutable` |
| VU 中间值不占 Core Mem | F45 | `vu_no_intermediate` |

***

## 9　取舍

* **为什么 ECC 编解码放在 SRAM 接口处而不是各访问源端口**
  * 能减少 8% 数据传输功耗，代价是面积增加
* **为什么 Matrix Mem 同一 bank 不许两个 master**
  * bank 与 MU 的 lane 一对一垂直贴合，为 MU 的 8 KB/T 让路
  * 冲突时只执行 MU，是因为 MU 的读是关键路径，DTE 与 ctrl_noc 都可以等
* **为什么单独做一块 Share Mem**
  * Core Mem 容量大、物理距离远，访问延时 15～25 拍，顺序执行的 RV core 掩盖不了
  * Share Mem 容量小、距离近，延时 5～10 拍，用来加速三个 RV core 的 task 之间传数据
* **为什么 stream 分片放在 master 侧算而不是存储模块里**
  * 分片的基址与步长是软件配的任务参数，随方向与内容类型变（data、scale、topK、包头各有各的步长）
  * 存储模块只看物理地址，就不必知道这些
