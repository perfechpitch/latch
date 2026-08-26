# Router 片上交换与归约

**模式**：design（陈述当前设计，动机与取舍收在各节的“取舍”段落里）

给要实现或建模 Router 的人：Router 解决什么问题、靠什么解决、每条机制归在哪一处。

* Router 在 core 内的位置与对外通道：《Core 内硬件》
* 逐级拍数、接口位宽、机制覆盖表：[Router 建模单元](07-units/04-router.md)

内容来自两份源文档，在若干点上口径不同，已裁定的取值见文末“两份源文档的口径”：

* `04_四、MAS/04_Router.md`
* `Bach项目文档/02_Architecture/03_HAS/02_DATA_NOC_DE_HAS.md`

《Bach 硬件设计建模参考》Router 专题，全套目录见 [README](README.md)。

***

## Router 解决的问题

* 一个 chip 上是 2 行 × 5 列共 10 个 Bach core，Harvest 后保证 8 个可用
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

模块组成与对外通道如下，数据面是一条链，下面三块控制面撑着它。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1320 660" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" role="img" aria-label="Router 的模块组成与对外通道">
<title>Router 的模块与对外通道</title><defs>
<marker id="d" markerWidth="9" markerHeight="9" refX="7.5" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#2563eb"/></marker>
<marker id="ds" markerWidth="9" markerHeight="9" refX="0.5" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#2563eb"/></marker>
<marker id="c" markerWidth="9" markerHeight="9" refX="7.5" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#d97706"/></marker>
<marker id="cs" markerWidth="9" markerHeight="9" refX="0.5" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#d97706"/></marker>
<marker id="g" markerWidth="9" markerHeight="9" refX="7.5" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#6b7280"/></marker>
<marker id="gs" markerWidth="9" markerHeight="9" refX="0.5" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#6b7280"/></marker>
</defs>
<rect width="1320" height="660" fill="#ffffff"/>
<text x="24" y="28" font-size="14.5" fill="#111827" font-weight="600">Router 的模块与对外通道</text>
<text x="24" y="47" font-size="10" fill="#475569">上半是数据面一条链，下半是撑着它的三块控制面。信号名、位宽与逐级拍数见 Router 建模单元</text>
<rect x="196" y="62" width="912" height="556" rx="8" fill="none" stroke="#374151" stroke-width="1.7"/>
<text x="208" y="80" font-size="11" fill="#6b7280">Router</text>
<rect x="212" y="92" width="880" height="268" rx="6" fill="#eef6fb" stroke="#b7d4e6" stroke-width="1.1"/>
<text x="222" y="109" font-size="10.5" fill="#6b7280" font-weight="600">数据面：收包 → 查表与资源判定 → 交换 → 出口</text>
<rect x="212" y="376" width="880" height="226" rx="6" fill="#fdf6ec" stroke="#e4c99b" stroke-width="1.1"/>
<text x="222" y="393" font-size="10.5" fill="#6b7280" font-weight="600">控制面</text>
<rect x="226" y="118" width="196" height="66" rx="4" fill="#f8fafc" stroke="#374151" stroke-width="1.2"/>
<text x="236" y="136" font-size="11.5" fill="#111827" font-weight="600">RouterStation[left]</text>
<text x="236" y="151" font-size="8.5" fill="#475569">vc_buf ×4，收包入 VC</text>
<text x="236" y="163" font-size="8.5" fill="#475569">查表定去向 · 资源判定（同行左邻）</text>
<rect x="226" y="196" width="196" height="66" rx="4" fill="#f8fafc" stroke="#374151" stroke-width="1.2"/>
<text x="236" y="214" font-size="11.5" fill="#111827" font-weight="600">RouterStation[right]</text>
<text x="236" y="229" font-size="8.5" fill="#475569">vc_buf ×4，收包入 VC</text>
<text x="236" y="241" font-size="8.5" fill="#475569">查表定去向 · 资源判定（同行右邻）</text>
<rect x="226" y="274" width="196" height="66" rx="4" fill="#f8fafc" stroke="#374151" stroke-width="1.2"/>
<text x="236" y="292" font-size="11.5" fill="#111827" font-weight="600">RouterStation[mid]</text>
<text x="236" y="307" font-size="8.5" fill="#475569">vc_buf ×4，收包入 VC</text>
<text x="236" y="319" font-size="8.5" fill="#475569">查表定去向 · 资源判定（另一行对称位）</text>
<rect x="450" y="150" width="210" height="150" rx="4" fill="#f8fafc" stroke="#374151" stroke-width="1.2"/>
<text x="460" y="169" font-size="11.5" fill="#111827" font-weight="600">Xbar</text>
<text x="460" y="184" font-size="8.5" fill="#475569">每个出口一个独立仲裁器</text>
<text x="460" y="196" font-size="8.5" fill="#475569">每拍独立仲裁，不跨拍锁定</text>
<text x="460" y="208" font-size="8.5" fill="#475569">贪婪整包：能凑整包的先命中</text>
<text x="460" y="220" font-size="8.5" fill="#475569">多播全有或全无，同拍 1→N 复制</text>
<text x="460" y="232" font-size="8.5" fill="#475569">五路输入无冲突时同周期并行</text>
<rect x="688" y="118" width="200" height="106" rx="4" fill="#f8fafc" stroke="#374151" stroke-width="1.2"/>
<text x="698" y="137" font-size="11.5" fill="#111827" font-weight="600">CoreStation</text>
<text x="698" y="152" font-size="8.5" fill="#475569">进 core：包头进 HeaderFIFO、</text>
<text x="698" y="164" font-size="8.5" fill="#475569">数据进 OutputBuffer，整包不交织</text>
<text x="698" y="176" font-size="8.5" fill="#475569">出 core：按包头的 VC 写入目标</text>
<text x="698" y="188" font-size="8.5" fill="#475569">进出两条通路完全并行</text>
<rect x="688" y="240" width="200" height="106" rx="4" fill="#f8fafc" stroke="#374151" stroke-width="1.2"/>
<text x="698" y="259" font-size="11.5" fill="#111827" font-weight="600">ReduceModule</text>
<text x="698" y="274" font-size="8.5" fill="#475569">16 用户 × 16 KiB 上下文 SRAM</text>
<text x="698" y="286" font-size="8.5" fill="#475569">首份输入建上下文，之后原位累加</text>
<text x="698" y="298" font-size="8.5" fill="#475569">中间累加固定 FP32</text>
<text x="698" y="310" font-size="8.5" fill="#475569">不允许绕过归约降级为转发</text>
<rect x="226" y="402" width="268" height="182" rx="4" fill="#f8fafc" stroke="#374151" stroke-width="1.2"/>
<text x="236" y="421" font-size="11.5" fill="#111827" font-weight="600">RouterTable / CSR</text>
<text x="236" y="436" font-size="8.5" fill="#475569">64 条表项，按 path_id 索引</text>
<text x="236" y="447" font-size="8.5" fill="#475569">给出目标端口、下一跳 VC、</text>
<text x="236" y="458" font-size="8.5" fill="#475569">各方向的资源需求、操作类型、</text>
<text x="236" y="469" font-size="8.5" fill="#475569">资源不足时留 VC 还是转 CoreMem</text>
<text x="236" y="480" font-size="8.5" fill="#475569">内部各查询点各持一份副本，</text>
<text x="236" y="491" font-size="8.5" fill="#475569">全部写完才向软件返回完成</text>
<text x="236" y="502" font-size="8.5" fill="#475569">DTE 与 ReduceModule 的副本</text>
<text x="236" y="513" font-size="8.5" fill="#475569">由软件保证三方一致</text>
<rect x="516" y="402" width="268" height="182" rx="4" fill="#f8fafc" stroke="#374151" stroke-width="1.2"/>
<text x="526" y="421" font-size="11.5" fill="#111827" font-weight="600">三层 credit 的维护点</text>
<text x="526" y="436" font-size="8.5" fill="#475569">VC credit：每个下游方向每个 VC</text>
<text x="526" y="447" font-size="8.5" fill="#475569">一个计数器，硬件自动</text>
<text x="526" y="458" font-size="8.5" fill="#475569">Stream 资源：按 UserID 加方向，</text>
<text x="526" y="469" font-size="8.5" fill="#475569">Router 是唯一有效状态</text>
<text x="526" y="480" font-size="8.5" fill="#475569">Reduce credit 由 ReduceModule</text>
<text x="526" y="491" font-size="8.5" fill="#475569">自己维护，Router 不管</text>
<text x="526" y="502" font-size="8.5" fill="#475569">CoreMemCreditMonitor：16 项全相连</text>
<text x="526" y="513" font-size="8.5" fill="#475569">监听队列，满足后通知 TS</text>
<rect x="806" y="402" width="272" height="182" rx="4" fill="#f8fafc" stroke="#374151" stroke-width="1.2"/>
<text x="816" y="421" font-size="11.5" fill="#111827" font-weight="600">Retire</text>
<text x="816" y="436" font-size="8.5" fill="#475569">Core 判定任务链结束后广播</text>
<text x="816" y="447" font-size="8.5" fill="#475569">Router 收到就停发该用户的新包，</text>
<text x="816" y="458" font-size="8.5" fill="#475569">删掉它的全部 Stream 授权表项</text>
<text x="816" y="469" font-size="8.5" fill="#475569">ReduceModule 延迟回收：等下游</text>
<text x="816" y="480" font-size="8.5" fill="#475569">各方向的 reduce credit 都回到</text>
<text x="816" y="491" font-size="8.5" fill="#475569">初始值才删用户映射</text>
<text x="816" y="502" font-size="8.5" fill="#475569">广播到 3 个 Station 与 ReduceModule</text>
<rect x="30" y="118" width="152" height="66" rx="4" fill="#eef2f7" stroke="#374151" stroke-width="1.2"/>
<text x="40" y="136" font-size="11.5" fill="#111827" font-weight="600">link[left]</text>
<text x="40" y="151" font-size="8.5" fill="#475569">256 B/T 双向</text>
<text x="40" y="163" font-size="8.5" fill="#475569">flit 与 credit 归还</text>
<rect x="30" y="196" width="152" height="66" rx="4" fill="#eef2f7" stroke="#374151" stroke-width="1.2"/>
<text x="40" y="214" font-size="11.5" fill="#111827" font-weight="600">link[right]</text>
<text x="40" y="229" font-size="8.5" fill="#475569">边沿的一端接 C2C</text>
<text x="40" y="241" font-size="8.5" fill="#475569">全 chip 4 个 C2C</text>
<rect x="30" y="274" width="152" height="66" rx="4" fill="#eef2f7" stroke="#374151" stroke-width="1.2"/>
<text x="40" y="292" font-size="11.5" fill="#111827" font-weight="600">link[mid]</text>
<text x="40" y="307" font-size="8.5" fill="#475569">连另一行对称位</text>
<text x="40" y="319" font-size="8.5" fill="#475569">两排南北对称直连</text>
<rect x="30" y="424" width="152" height="66" rx="4" fill="#eef2f7" stroke="#374151" stroke-width="1.2"/>
<text x="40" y="442" font-size="11.5" fill="#111827" font-weight="600">cfg（R2CU）</text>
<text x="40" y="457" font-size="8.5" fill="#475569">SCP 经 ctrl_noc</text>
<text x="40" y="469" font-size="8.5" fill="#475569">写路由表与 CSR</text>
<rect x="1136" y="118" width="158" height="106" rx="4" fill="#eef2f7" stroke="#374151" stroke-width="1.2"/>
<text x="1146" y="136" font-size="11.5" fill="#111827" font-weight="600">DTE</text>
<text x="1146" y="151" font-size="8.5" fill="#475569">进 core：包头用</text>
<text x="1146" y="163" font-size="8.5" fill="#475569">AXI-Full 类接口读，</text>
<text x="1146" y="175" font-size="8.5" fill="#475569">数据用 AXI-Stream-Like</text>
<text x="1146" y="187" font-size="8.5" fill="#475569">出 core：先拿 credit</text>
<rect x="1136" y="246" width="158" height="100" rx="4" fill="#eef2f7" stroke="#374151" stroke-width="1.2"/>
<text x="1146" y="264" font-size="11.5" fill="#111827" font-weight="600">TS</text>
<text x="1146" y="279" font-size="8.5" fill="#475569">按收包顺序通知它</text>
<text x="1146" y="291" font-size="8.5" fill="#475569">调度 DTE 搬运</text>
<text x="1146" y="303" font-size="8.5" fill="#475569">资源满足的通知</text>
<text x="1146" y="315" font-size="8.5" fill="#475569">Reduce 完成</text>
<text x="1146" y="327" font-size="8.5" fill="#475569">用户退休</text>
<path d="M186 151 L222 151" fill="none" stroke="#2563eb" stroke-width="1.5" marker-end="url(#d)" marker-start="url(#ds)"/>
<path d="M426 151 L438 151 L438 180 L446 180" fill="none" stroke="#2563eb" stroke-width="1.5" marker-end="url(#d)"/>
<path d="M186 229 L222 229" fill="none" stroke="#2563eb" stroke-width="1.5" marker-end="url(#d)" marker-start="url(#ds)"/>
<path d="M426 229 L438 229 L438 225 L446 225" fill="none" stroke="#2563eb" stroke-width="1.5" marker-end="url(#d)"/>
<path d="M186 307 L222 307" fill="none" stroke="#2563eb" stroke-width="1.5" marker-end="url(#d)" marker-start="url(#ds)"/>
<path d="M426 307 L438 307 L438 270 L446 270" fill="none" stroke="#2563eb" stroke-width="1.5" marker-end="url(#d)"/>
<path d="M664 160 L684 160" fill="none" stroke="#2563eb" stroke-width="1.5" marker-end="url(#d)" marker-start="url(#ds)"/>
<rect x="671.0" y="142.5" width="6.0" height="13.5" fill="#ffffff" opacity="0.95"/>
<text x="674" y="152" font-size="8.5" fill="#2563eb" text-anchor="middle"></text>
<path d="M664 290 L684 290" fill="none" stroke="#2563eb" stroke-width="1.5" marker-end="url(#d)" marker-start="url(#ds)"/>
<path d="M892 160 L1132 160" fill="none" stroke="#2563eb" stroke-width="1.5" marker-end="url(#d)" marker-start="url(#ds)"/>
<rect x="953.665" y="142.5" width="116.67" height="13.5" fill="#ffffff" opacity="0.95"/>
<text x="1012" y="152" font-size="8.5" fill="#2563eb" text-anchor="middle">local 通道 256 B/T，进出并行</text>
<path d="M892 200 L1000 200 L1000 262 L1132 262" fill="none" stroke="#d97706" stroke-width="1.5" marker-end="url(#c)"/>
<rect x="978.555" y="236.5" width="42.89" height="13.5" fill="#ffffff" opacity="0.95"/>
<text x="1000" y="246" font-size="8.5" fill="#d97706" text-anchor="middle">trigger</text>
<path d="M892 320 L1132 320" fill="none" stroke="#d97706" stroke-width="1.5" marker-end="url(#c)"/>
<rect x="980.015" y="304.5" width="63.97" height="13.5" fill="#ffffff" opacity="0.95"/>
<text x="1012" y="314" font-size="8.5" fill="#d97706" text-anchor="middle">reduce done</text>
<path d="M186 457 L222 457" fill="none" stroke="#6b7280" stroke-width="1.5" stroke-dasharray="5 3" marker-end="url(#g)"/>
<path d="M360 398 L360 346" fill="none" stroke="#6b7280" stroke-width="1.5" stroke-dasharray="5 3" marker-end="url(#g)"/>
<rect x="338.49" y="362.5" width="143.02" height="13.5" fill="#ffffff" opacity="0.95"/>
<text x="410" y="372" font-size="8.5" fill="#6b7280" text-anchor="middle">各 Station 与 CoreStation 查表</text>
<path d="M650 398 L650 372 L560 372 L560 304" fill="none" stroke="#d97706" stroke-width="1.5" marker-end="url(#c)"/>
<rect x="591.46" y="358.5" width="27.08" height="13.5" fill="#ffffff" opacity="0.95"/>
<text x="605" y="368" font-size="8.5" fill="#d97706" text-anchor="middle">资源判定</text>
<path d="M942 398 L942 372 L800 372 L800 350" fill="none" stroke="#d97706" stroke-width="1.5" marker-end="url(#c)"/>
<rect x="863.825" y="358.5" width="32.35" height="13.5" fill="#ffffff" opacity="0.95"/>
<text x="880" y="368" font-size="8.5" fill="#d97706" text-anchor="middle">停发与回收</text>
<path d="M1078 470 L1104 470 L1104 340 L1132 340" fill="none" stroke="#d97706" stroke-width="1.5" marker-end="url(#c)"/>
<rect x="1101" y="390.5" width="27.08" height="13.5" fill="#ffffff" opacity="0.95"/>
<text x="1104" y="400" font-size="8.5" fill="#d97706" text-anchor="start">退休完成</text>
<text x="24" y="640" font-size="10" fill="#475569">连线：</text>
<path d="M68 636 L98 636" stroke="#2563eb" stroke-width="1.5" marker-end="url(#d)"/>
<text x="106" y="640" font-size="10" fill="#475569">flit 数据通路</text>
<path d="M258 636 L288 636" stroke="#d97706" stroke-width="1.5" marker-end="url(#c)"/>
<text x="296" y="640" font-size="10" fill="#475569">与 core 的控制交互</text>
<path d="M448 636 L478 636" stroke="#6b7280" stroke-width="1.5" stroke-dasharray="5 3" marker-end="url(#g)"/>
<text x="486" y="640" font-size="10" fill="#475569">配置</text>
<text x="660" y="640" font-size="10" fill="#475569">2×5 Mesh 里一个 Router 只有三个 R2R 邻居；边沿 Router 的 left 或 right 接 C2C Bridge</text>
</svg>
```

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
| `curVC` | 包进入当前 Router 时使用的 VC 类型 | 输入 VC 分配 |
| `directionMask` | 各目标方向加 Core 的有效位；单位有效是单播，多位有效是多播 | 输出仲裁、Crossbar |
| `nxtVC` | 各目标方向下一跳使用的 VC 类型 | 输出 Header、下游 VC credit 查询 |
| `streamNeedMask` | 各目标方向是否需要 Stream 授权，**Core 方向的需求必须在本级检查** | Stream 资源表 |
| `operation` | 普通转发还是 Reduce，选择 Bypass / Core / ReduceModule 路径 | 路径选择、ReduceModule |
| `stallWay` | 资源不足时留在当前 VC 等待，还是转入 CoreMem 暂存由 DTE 重发 | 阻塞处理 |
| `reducePrecision` | Reduce 输入与输出精度，**中间累加精度固定 FP32** | ReduceModule |

RouterTable 支持 64 条表项，软件通过 R2CU 接口配置，中间节点可以按表改写 VC。

### 第二关：拿资源才放行

VA 阶段做两件事：检查资源，然后在本 input port 内的多个 VC 之间选一个。

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
<path d="M224 129 L274 129" fill="none" stroke="#6b7280" stroke-width="1.6" marker-end="url(#ag)"/>
<path d="M484 129 L554 129" fill="none" stroke="#6b7280" stroke-width="1.6" marker-end="url(#ag)"/>
<path d="M764 129 L834 129" fill="none" stroke="#6b7280" stroke-width="1.6" marker-end="url(#ag)"/>
<path d="M660 90 L660 74 L1180 74 L1180 88" fill="none" stroke="#6b7280" stroke-width="1.4" stroke-dasharray="5 3" marker-end="url(#ag)"/>
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
<text x="40" y="374" font-size="12.5" fill="#d97706" font-weight="600">Stream 资源</text>
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

Stream 授权有两条硬规则：

* **只有 Router 负责真正申请表项**。DTE 要发数据必须先从 Router 拿到指定 user 的授权，禁止超额分配或重复授权
* **Router 的进 core 表和 TS 内部的 Stream 资源表按完全一致的逻辑申请空项**。分配因此不会多于实际资源数，这保证了“Router 通知 TS 的包一定能被 TS 接收”

业务 credit 的 release 走一条特殊路径：

* 软件通过 CSR 为每个业务 credit 输入端口配置**静态输出方向 Mask**
* 转发时不查 RouterTable，也不做动态路径选择
* Mask 含多个方向时，同一笔 release 复制到所有指定方向，UserID 与 credit 类型保持不变

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

## 四条传输路径

### Router 到 Router（Bypass）

* 包经数据总线传到下一级 Router，自动检测包头，按 `PathID` 查到路由信息和资源需求
* 数据总线每个 flit 携带 VC 通道号，到达后自动找到对应 VC 存放位置
* 输入方向的 RouterStation 维护所有下游方向的 Stream 资源映射表，**只有所有需求方向都满足才允许发送**

**交织规则**（直接决定建模时 buffer 的组织方式）：

* Router 到 Router 的通路**允许在 flit 边界切换包**，需保存 VC、输出方向、剩余长度和包边界上下文
* 包一旦开始进入 Core 或 ReduceModule 就**锁定到尾 flit**

### 进 Core

* 进 Core 对 Router 而言也是一个输出方向，要维护本级 Core 的 Stream 资源
* 已通过 Stream 检查，因此**不再检查对 Core 的 VC credit**，一定有 CM 空间

按 `PathID` 查到需进 Core 后，检查本级 Stream 资源表，按三种结果分别处理：

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

### 出 Core

由 DataOut DTE 发起：

* DTE 内按 Router 一个方向的 VC 数各有一个 Buffer，某个 VC 阻塞只阻塞对应的那个 Buffer
* DTE 发数据到 Router 时与 CoreStation 有 credit 协议，保证 VC 有容量才发
* 包对下游 Stream 或 Rmem 资源有需求时，DTE 必须先申请到才能发，否则任务在 `PendingTaskQ` 等待
* DTE 中也要有一份 RouterTable，按 PathID 查到 VC 和资源需求
* 进 Core 与出 Core 的数据通路**完全并行**，互不共享数据通路仲裁状态

### 进 CoreMem 暂存与重发

下游资源不满足时，`stallWay` 二选一：

* 留在当前 VC 等
* 把包重定向到本地 Core Mem 缓存，等资源就绪后重发。此时**Router 上的 Bypass 操作被映射成“进 core 加出 core”**

* CoreMem 中**只保存包**，包头含 UserID、PathID、size
  * 重发时用 PathID 重新查 RouterTable，不重复保存 VC 和路由信息
* **同 VC 保序**：同一 VC 存在未完成的 CoreMem 重发包时，后续包不得越过
  * 按 VC 粒度维护 `pending_reinject` 计数器防超车
  * 包进 core 暂存时改写 `overflow_reinject=1`，出 core 重发时 Router 改回 0
  * Output Port 识别到重注入标记才扣减 credit
* 无论直接发送还是经 CoreMem 重发，完成后都向 core 内 TS 返回至少含 UserID 加 PathID 的完成信息
* **坏核不接收溢流**：Router 对坏核不发起进 core 缓存处理，此时 coremem credit 直接 bypass

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
| Input/Output Arbiter | 仲裁最多三路输入的 SRAM、Bank 与计算资源 |

三条硬约束：

* **上下文保护**：当前包的全部输入完成并输出前，同一 User 的下一个包不得覆盖该上下文
* **必须执行 Reduce**：SRAM、Bank 或计算单元暂不可用时对输入反压，
  **不允许绕过 Reduce 降级为直接存储或转发**
* **精度**：输入 FP32 或 BF16，BF16 转 FP32 后参与计算，中间累加统一 FP32，输出可配 FP32 或 BF16

性能指标：Reduce 输入三路各 160 GB/s，输出 160 GB/s，算力 80 GFLOPS（FP32 / BF16）。

### 用户退休

资源回收是一份时序契约，建模时是一个明确的状态机：

1. ReduceModule 完成计算并发出全部包后向 Core 返回 UserID
2. Core 判定任务链结束后**向 Router 和 ReduceModule 广播 User Retire**
3. **Core 的保证**：仅可在该 UserID 的全部进 core、出 core 数据搬运完成、
   且不会再发起新搬运后发 Retire。Retire 发出后，Router 上不得再出现以该 Core 为源或目标的该用户包
4. **Router 的动作**：停止该 UserID 的新发送，删除其全部 Stream 资源授权表项
5. **ReduceModule 的动作**：**延迟回收**，先记录 Retire，
   待相邻下游各方向 Reduce Credit 全部恢复到初始值后才删除对应用户映射

***

## 坏核、跨 chip 与 credit 的回程

### Skip 与 Partially Good

* 每 chip 至多 2 个坏 core，由 fuse `core_bad_mask[9:0]` 标记
* **Router 数据通路不坏，正常 R2R 转发**
* 坏 core 上：local 侧禁用，不接收溢流，credit pulse 无效，`coremem_credit` 上电默认 0
* credit 跨过坏核走：
  * 上游要检查的 credit 对应的是坏核之后那个好核
  * 下游返还 credit 也跨过坏核直接给上游好核
  * 坏核只按路由表透传，不检查 credit、不支持阻塞重发
* 坏点位置不同会导致每颗 chip 的路由表不同，**路由表必须作为建模输入参数，不能写死**

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

### credit release 的回程

三类 credit 的 release 都要往上游走，走法不同。

**VC credit** 最简单：

* flit 从 VC Buffer 读出时，通过共享总线（`credit_return_vld` 加 `credit_return_vc_id`）向上游归还一个
* 每个 input port 同一拍最多一个 VC 的 flit 被读出，共享总线无冲突

**core credit** 走一条脉冲链：

* 每个 Router 部署一个组合逻辑的 core credit crossbar
* 汇总本级 core 的 pulse 加 user，与所有下级出口的 pulse 加 user
* 发往除当前输入方向外的另两个 R2R port：从 left 来就发往 right 和 mid，逐跳传播到上游
* 跨 chip 时经 C2C Bridge 透传到对端
* 转发通路的静态切换由寄存器配置，这是坏核场景下切换 credit 路径的手段

Router 另外输出 per-port 的 `coremem_credit` 同步信息给 core 与 DTE，用于判断重注入。

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

core 对外发数据要同时满足 VC 资源与 Stream 资源。监听这两项资源的职责在 Router，一次监听走三步：

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
| VC Buffer | Private 每 VC 深度 2（防死锁），Shared Pool 约 20 flits（覆盖 credit 往返） |
| Reduce 输入 | 三路各 160 GB/s |
| Reduce 输出 | 160 GB/s |
| Reduce 算力 | 80 GFLOPS（FP32 / BF16） |
| ReduceModule 上下文 | 16 用户 × 16 KiB |
| 监听事件队列 | 16 项全相连 |
| C2C Bridge | 全 chip 4 个，VC Buffer 合计约 138.7 KB |
| 坏核上限 | 每 chip 至多 2 个 |

包结构：

| 字段 | 宽度 |
| - | - |
| `path_id` | 8-bit（有效 6-bit），RouterTable 索引 |
| `path_core_mask` | 16-bit，EP 广播时按 bit 选目标 core |
| `user_id` 加 `task_id` | 10-bit |
| `vc_id` | 目标 VC 标识 |
| `overflow_reinject` | 1-bit，重注入标记 |
| 包长度 | 16-bit |
| 软件 payload | 0～16 B |
| 业务数据 payload | 0 B～64 KB |

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
| Stream 资源表 | Router 是唯一有效状态，DTE 持 cache（MAS） | HAS 写 Router 输出单元与 core 内各持一份 credit table，靠 credit release 接口同步 |
| 资源监听队列 | 在 Router，16 项全相连（MAS） | 同一份 MAS 另一处写“功能已转移到 DTE 中” |
| 仲裁粒度 | flit 级，整包只作为贪婪仲裁的优先级偏好 | MAS 有一节“Interleave 和整包的对比”只列两案优劣、未给结论；MAS 正文与 HAS 都是 flit 级 |
| R2R 带宽 | 256 GB/s（接口理论值）与 210 GB/s（HAS 的有效带宽）两个都记 | 两份分别只给其中一个 |
| `vc_id` 位宽 | 按 VC0～3 | HAS 的包格式表里 `vc_id 5-bit（V≤24）` 是 2026/08/19 缩减 VC 之前的残留 |

***

## 取舍

* **为什么把 VC credit 和 Stream 资源分成两层**
  * 两者管的东西时间尺度差着数量级
  * VC credit 管下游 Router 的 buffer 槽位，flit 一进一出就归还，快
  * Stream 资源管下游 core 的 Core Mem 空间，要等那个用户在下游 core 上跑完整条任务链才释放，慢
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

配图：[Router 12 张](<../../../../perfechpitch/Bach/04_四、MAS（Micro Architecture SPEC）/04_Router>)

| 编号 | 内容 |
| - | - |
| `01` | Block Diagram（5×7 CrossBar / Router Station / Interconnect Matrix / Stream Resource Map） |
| `02` | Packet 在 Router 上传输 |
| `03` | 进 core 机制 |
| `04` | 出 core 机制 |
| `05` | Core 出 Reduce |
| `06` | ReduceModule 之间 |
| `07~12` | RouterTable / CSR / 各 Station 与 Xbar 细节 |

另有 [Router OLD 1 张](<../../../../perfechpitch/Bach/04_四、MAS（Micro Architecture SPEC）/05_Router OLD>)。

内嵌表格：

* [通信机制 6 子表](../../../../perfechpitch/_sheets/_II1Rs6)（每个 path_id 在各 core 上的进出方向：broadcast、reduce、p2p、EP 多播各一张；`N8MvQ8` 是 path_id 加 path_core_mask 的编码对照）
* [通信机制分析过程 52 子表](../../../../perfechpitch/_sheets/_BTlDsC)（36 张同构的 path_id 路径表覆盖各切分场景，另有 message 动态字段、logic op 说明、合并前后对照）

来源：

* `04_四、MAS/04_Router.md`：前 400 行为有效内容，Programming Model 之后为 eFUSE 模板残留
* `Bach项目文档/02_Architecture/03_HAS/02_DATA_NOC_DE_HAS.md`
* `06_第四阶段/02_通信机制（分析过程）.md`
