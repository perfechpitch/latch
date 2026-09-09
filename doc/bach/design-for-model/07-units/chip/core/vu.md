# VU DSA

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **VU DSA**

给实现 VU 的人：十一个逐拍推进的模块各自做哪些事、端口与存储怎么定。寄存器地址按《VU-DSA 寄存器整理》。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《执行单元与存储》“VU DSA（向量单元）”全部小节
* 《软件栈》各 VU 算子的宏指令拆分

***

## 1　定位与边界

VU 服务 LayerNorm、RMSNorm、Softmax、SwiGLU、MoE-Router、Sigmoid、ReLU 这类算子。它与 MU、DTE 最不一样的地方是抽象层次：**VU-Core 与 VU-DSA 之间的交互抽象是宏指令**。

一条宏指令由两半组成：

* **静态配置**：8 组模板之一，定各执行单元的连接与 op
* **动态参数**：地址、索引、VL / 精度 / 舍入

一条宏指令怎么跑起来：VU-Core 写动态参数，再写 `macro_inst_trigger` → 硬件锁存并与静态配置的指针打包压入 ISQ → pipe_ctrl 展开成各单元的微指令，Scoreboard 管 VRF / MRF / SRF 依赖 → 最多两条相邻宏指令重叠。从 CM 读入、多级流水计算、写回 CM 的全过程由硬件自己走完，VU-Core 不感知周期级的控制细节。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1700 1200" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" role="img" aria-label="VU DSA 第 0 层">
<title>VU DSA 第 0 层</title>
<defs><marker id="a" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#475569"/></marker><marker id="as" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#475569"/></marker><marker id="g" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0f766e"/></marker><marker id="gs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0f766e"/></marker><marker id="o" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#b45309"/></marker><marker id="os" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#b45309"/></marker><marker id="p" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#7c3aed"/></marker><marker id="ps" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#7c3aed"/></marker><marker id="i" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#4338ca"/></marker><marker id="is" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#4338ca"/></marker><marker id="t" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0d9488"/></marker><marker id="ts" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0d9488"/></marker><marker id="r" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#be123c"/></marker><marker id="rs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#be123c"/></marker><marker id="b" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#2563eb"/></marker><marker id="bs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#2563eb"/></marker><marker id="m" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#d97706"/></marker><marker id="ms" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#d97706"/></marker><marker id="l" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#9aa1ad"/></marker><marker id="ls" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#9aa1ad"/></marker></defs>
<rect x="0" y="0" width="1700" height="1200" fill="#ffffff"/>
<text x="20" y="26" font-size="12" fill="#111827">VU DSA · 第 0 层（十一个逐拍推进的模块。方位：RV core 与 TS 在上，Core Mem 在下，cfg 从上进）</text>
<text x="627" y="26" font-size="9.5" fill="#6b7280">一条宏指令 = 1 组静态配置（计算图通路的模板）+ 1 组动态参数（地址、索引、向量长度 / 精度）</text>
<rect x="160" y="110" width="350" height="194.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="172" y="131" font-size="11" fill="#111827" font-weight="600">config_register</text>
<text x="172.0" y="148.0" font-size="8.5" fill="#475569">8 组静态配置模板（默认全 0）+ 12 个动态参数寄存器</text>
<text x="172.0" y="161.5" font-size="8.5" fill="#475569">macro_inst_trigger 是唯一的启动寄存器，写一次执行一次</text>
<text x="172.0" y="175.0" font-size="8.5" fill="#475569">　字段：CONFIG_IDX · STATIC_DYNAMIC_MASK · EVENT_EN</text>
<text x="172.0" y="188.5" font-size="8.5" fill="#475569">　· STREAM_ID_OVERRIDE · DATA_BROADCAST · MACRO_INST_FENCE</text>
<text x="172.0" y="202.0" font-size="8.5" fill="#475569">TYPE_VL 一个寄存器含 VL、DATA_TYPE、ROUND_MODE 三个字段</text>
<text x="172.0" y="215.5" font-size="8.5" fill="#475569">stream_id 与 task_id 没有寄存器，软件不配：经 dsa_ids 从 VU RV</text>
<text x="172.0" y="229.0" font-size="8.5" fill="#475569">　core 的 CSR 直连过来，硬件在写 trigger 那一拍自动采样</text>
<text x="172.0" y="242.5" font-size="8.5" fill="#475569">静态配置改写：目标组正被未完成的宏指令引用时，硬件把这次</text>
<text x="172.0" y="256.0" font-size="8.5" fill="#475569">　配置写阻塞在配置通路上，等引用它的宏指令退休后写入生效</text>
<text x="172.0" y="269.5" font-size="8.5" fill="#475569">in-flight 的宏指令始终按改写前的配置执行完毕</text>
<text x="172.0" y="283.0" font-size="8.5" fill="#475569">三条配置通路（VU-Core / Ctrl-NOC / Debug Module）共享同一份</text>
<text x="172.0" y="296.5" font-size="8.5" fill="#475569">　寄存器视图、权限一致，流控彼此独立</text>
<rect x="560" y="110" width="300" height="167.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="572" y="131" font-size="11" fill="#111827" font-weight="600">ISQ</text>
<text x="572.0" y="148.0" font-size="8.5" fill="#475569">VU-Core 写完动态参数后写 macro_inst_trigger</text>
<text x="572.0" y="161.5" font-size="8.5" fill="#475569">硬件锁存当前动态参数，与对应静态配置的</text>
<text x="572.0" y="175.0" font-size="8.5" fill="#475569">　指针打包压入内部执行队列</text>
<text x="572.0" y="188.5" font-size="8.5" fill="#475569">深度 8（待定）</text>
<text x="572.0" y="202.0" font-size="8.5" fill="#475569">status 实时回传 BUSY · ISQ_FULL · ISQ_EMPTY</text>
<text x="572.0" y="215.5" font-size="8.5" fill="#475569">　· ERROR_FLAG</text>
<text x="572.0" y="229.0" font-size="8.5" fill="#475569">判断全部宏指令是否完成用 macro_inst_left 或 BUSY</text>
<text x="572.0" y="242.5" font-size="8.5" fill="#475569">从 CM 读入、多级流水计算、写回 CM 的全过程</text>
<text x="572.0" y="256.0" font-size="8.5" fill="#475569">　由硬件自己走完，VU-Core 不感知周期级细节</text>
<rect x="910" y="110" width="420" height="208.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="922" y="131" font-size="11" fill="#111827" font-weight="600">pipe_ctrl 与 Scoreboard</text>
<text x="922.0" y="148.0" font-size="8.5" fill="#475569">把宏指令展开成各执行单元的微指令</text>
<text x="922.0" y="161.5" font-size="8.5" fill="#475569">Scoreboard 对 VRF / MRF / SRF 实时读写状态追踪，</text>
<text x="922.0" y="175.0" font-size="8.5" fill="#475569">　检测 RAW / WAR / WAW</text>
<text x="922.0" y="188.5" font-size="8.5" fill="#475569">重叠执行：前后宏指令无数据依赖、无执行资源冲突时，</text>
<text x="922.0" y="202.0" font-size="8.5" fill="#475569">　后续宏指令无需等前一条完全结束即可重叠发射微操作</text>
<text x="922.0" y="215.5" font-size="8.5" fill="#475569">　最多两条相邻宏指令重叠</text>
<text x="922.0" y="229.0" font-size="8.5" fill="#475569">CM 访存依赖不追踪：存在冲突的宏指令之间须由软件置</text>
<text x="922.0" y="242.5" font-size="8.5" fill="#475569">　MACRO_INST_FENCE = 1，等此前全部宏指令完成后才派发</text>
<text x="922.0" y="256.0" font-size="8.5" fill="#475569">含 Vector 数据广播的宏指令须置 DATA_BROADCAST，它不参与</text>
<text x="922.0" y="269.5" font-size="8.5" fill="#475569">　Scoreboard 乱序调度，等除 CM-Load 外的前序宏指令全完成才派发</text>
<text x="922.0" y="283.0" font-size="8.5" fill="#475569">配平计算依赖树是软件的责任：硬件只提供 bypass 与广播，</text>
<text x="922.0" y="296.5" font-size="8.5" fill="#475569">　不提供软件可见的缓冲队列</text>
<polygon points="189,44 300,44 291,74 180,74" fill="#f8fafc" stroke="#374151"/>
<text x="240.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">cfg（ctrl_noc）</text>
<polygon points="339,44 480,44 471,74 330,74" fill="#f8fafc" stroke="#374151"/>
<text x="405.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">dsa_cfg / dsa_rdata</text>
<polygon points="649,44 780,44 771,74 640,74" fill="#f8fafc" stroke="#374151"/>
<text x="710.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">dsa_done / Event → TS</text>
<polygon points="29,120 140,120 131,150 20,150" fill="#f8fafc" stroke="#374151"/>
<text x="80.0" y="138.5" font-size="9" fill="#374151" text-anchor="middle">dsa_ids</text>
<path d="M136.0 135.0 L159.0 135.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#p)"/>
<rect x="160" y="380" width="150" height="140.5" rx="4" fill="#f5f3ff" stroke="#7c3aed"/>
<text x="172" y="401" font-size="11" fill="#111827" font-weight="600">Profile</text>
<text x="172.0" y="418.0" font-size="8.5" fill="#475569">profile_ctrl 在 0x4000</text>
<text x="172.0" y="431.5" font-size="8.5" fill="#475569">计数器从 0x4008 起</text>
<text x="172.0" y="445.0" font-size="8.5" fill="#475569">只能用 profile_ctrl.CLEAR</text>
<text x="172.0" y="458.5" font-size="8.5" fill="#475569">　清零</text>
<text x="172.0" y="472.0" font-size="8.5" fill="#475569">软件写 macro_inst_left、</text>
<text x="172.0" y="485.5" font-size="8.5" fill="#475569">　status 与 Profile</text>
<text x="172.0" y="499.0" font-size="8.5" fill="#475569">　计数器无效，不报错</text>
<rect x="410" y="380" width="320" height="86.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="422" y="401" font-size="11" fill="#111827" font-weight="600">MEXE（15 条）</text>
<text x="422.0" y="418.0" font-size="8.5" fill="#475569">Mask 逻辑运算 and/nand/andn/xor/or/nor/orn/xnor</text>
<text x="422.0" y="431.5" font-size="8.5" fill="#475569">vcpop.m · vfirst.m · vmsbf/vmsif/vmsof.m</text>
<text x="422.0" y="445.0" font-size="8.5" fill="#475569">vmiuset.mv / vmiset.mv：按 16 个索引清 / 置 Mask 位</text>
<rect x="410" y="498.5" width="320" height="113.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="422" y="519.5" font-size="11" fill="#111827" font-weight="600">SEXE（7 条）</text>
<text x="422.0" y="536.5" font-size="8.5" fill="#475569">fadd / fsub / fmul / fdiv / fsqrt / frsqrt / frcp（.s）</text>
<text x="422.0" y="550.0" font-size="8.5" fill="#475569">物理上只有一组，SEXE0/1/2 是同一物理单元在一条</text>
<text x="422.0" y="563.5" font-size="8.5" fill="#475569">　宏指令内的 3 次串行迭代，迭代之间天然链式依赖</text>
<text x="422.0" y="577.0" font-size="8.5" fill="#475569">操作数只有三处来源：SRF 读端口、VALU1 的归约输出、</text>
<text x="422.0" y="590.5" font-size="8.5" fill="#475569">　前一次 SEXE 迭代的结果；不支持立即数，不能取 MEXE</text>
<rect x="410" y="644.0" width="320" height="181.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="422" y="665.0" font-size="11" fill="#111827" font-weight="600">DMUX</text>
<text x="422.0" y="682.0" font-size="8.5" fill="#475569">结果路由：写回 VRF / MRF / SRF 或交给 SU</text>
<text x="422.0" y="695.5" font-size="8.5" fill="#475569">两个 VRF 写口须指向不同执行单元，同时使能时</text>
<text x="422.0" y="709.0" font-size="8.5" fill="#475569">　写区间不重叠；来源只能是 LU 或 VALU0/1/2/VSFU0/1，</text>
<text x="422.0" y="722.5" font-size="8.5" fill="#475569">　其余置 CFG_ERROR</text>
<text x="422.0" y="736.0" font-size="8.5" fill="#475569">VALU1 的归约标量结果走 SRF 虚拟写口</text>
<text x="422.0" y="749.5" font-size="8.5" fill="#475569">MRF 唯一写口的来源是 LU 的 ld.vm_mask、VALU0 的</text>
<text x="422.0" y="763.0" font-size="8.5" fill="#475569">　比较类与 vfclass.mv、MEXE 三者之一，同一宏指令内</text>
<text x="422.0" y="776.5" font-size="8.5" fill="#475569">　不能同时写回</text>
<text x="422.0" y="790.0" font-size="8.5" fill="#475569">SRF 6 个写口按 PRF_op.SRF_WT_EN 位图使能：</text>
<text x="422.0" y="803.5" font-size="8.5" fill="#475569">　bit0 LU · bit1 VALU1 · bit2 MEXE · bit3～5 SEXE0/1/2</text>
<rect x="760" y="380" width="300" height="86.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="772" y="401" font-size="11" fill="#111827" font-weight="600">VALU0（29 条独有）</text>
<text x="772.0" y="418.0" font-size="8.5" fill="#475569">加减乘 · 最值 · MACC · 除法（非全吞吐）</text>
<text x="772.0" y="431.5" font-size="8.5" fill="#475569">符号注入 · 比较生成 Mask · vfclass</text>
<text x="772.0" y="445.0" font-size="8.5" fill="#475569">vfmerge · 标量广播 / 搬入</text>
<rect x="760" y="500" width="300" height="86.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="772" y="521" font-size="11" fill="#111827" font-weight="600">VALU1（6 条独有）</text>
<text x="772.0" y="538.0" font-size="8.5" fill="#475569">加减乘 · 最值 · 标量广播 / 搬出</text>
<text x="772.0" y="551.5" font-size="8.5" fill="#475569">跨元素归约（求和 / 最大 / 最小）</text>
<text x="772.0" y="565.0" font-size="8.5" fill="#475569">Top-16 排序，同时输出 16 个 INT16 索引</text>
<rect x="760" y="620" width="300" height="86.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="772" y="641" font-size="11" fill="#111827" font-weight="600">VALU2（1 条独有）</text>
<text x="772.0" y="658.0" font-size="8.5" fill="#475569">加减乘 · 最值 · 标量广播</text>
<text x="772.0" y="671.5" font-size="8.5" fill="#475569">vmv.v.v 向量直通缓冲，可当延迟对齐用</text>
<text x="772.0" y="685.0" font-size="8.5" fill="#475569">代价是这条宏指令不能再用 VALU2 计算</text>
<rect x="760" y="740" width="300" height="86.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="772" y="761" font-size="11" fill="#111827" font-weight="600">VSFU0 / VSFU1（12 条）</text>
<text x="772.0" y="778.0" font-size="8.5" fill="#475569">sin / cos / tanh / exp / exp2 / ln / log2</text>
<text x="772.0" y="791.5" font-size="8.5" fill="#475569">rcp / rsqrt / sqrt / sigmoid</text>
<text x="772.0" y="805.0" font-size="8.5" fill="#475569">源不能取自身的输出</text>
<rect x="1110" y="380" width="170" height="167.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1122" y="401" font-size="11" fill="#111827" font-weight="600">SMUX</text>
<text x="1122.0" y="418.0" font-size="8.5" fill="#475569">源路由</text>
<text x="1122.0" y="431.5" font-size="8.5" fill="#475569">执行单元之间允许</text>
<text x="1122.0" y="445.0" font-size="8.5" fill="#475569">　bypass 与广播，</text>
<text x="1122.0" y="458.5" font-size="8.5" fill="#475569">　且不消耗 RF 端口</text>
<text x="1122.0" y="472.0" font-size="8.5" fill="#475569">一条宏指令内多条</text>
<text x="1122.0" y="485.5" font-size="8.5" fill="#475569">　并行通路经过的</text>
<text x="1122.0" y="499.0" font-size="8.5" fill="#475569">　执行分组级数不同时，</text>
<text x="1122.0" y="512.5" font-size="8.5" fill="#475569">　合并点的两个源操作数</text>
<text x="1122.0" y="526.0" font-size="8.5" fill="#475569">　会不同拍到达</text>
<rect x="1330" y="380" width="300" height="127.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1342" y="401" font-size="11" fill="#111827" font-weight="600">VRF / MRF / SRF</text>
<text x="1342.0" y="418.0" font-size="8.5" fill="#475569">VRF 64 KB</text>
<text x="1342.0" y="431.5" font-size="8.5" fill="#475569">　128 B/entry × 512 entry</text>
<text x="1342.0" y="445.0" font-size="8.5" fill="#475569">　2R + 2W，读写索引连续</text>
<text x="1342.0" y="458.5" font-size="8.5" fill="#475569">　允许完全重叠或完全不</text>
<text x="1342.0" y="472.0" font-size="8.5" fill="#475569">　重叠，不允许部分重叠</text>
<text x="1342.0" y="485.5" font-size="8.5" fill="#475569">MRF 4 KB，2R + 1W</text>
<text x="1480.0" y="418.0" font-size="8.5" fill="#475569">　Mask 不能广播，一条宏</text>
<text x="1480.0" y="431.5" font-size="8.5" fill="#475569">　指令内最多两处用 Mask</text>
<text x="1480.0" y="445.0" font-size="8.5" fill="#475569">SRF 256 B（4 B × 64 entry）</text>
<text x="1480.0" y="458.5" font-size="8.5" fill="#475569">　8 逻辑读 / 6 逻辑写，</text>
<text x="1480.0" y="472.0" font-size="8.5" fill="#475569">　虚拟端口时分复用</text>
<rect x="1110" y="865.0" width="250" height="167.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1122" y="886.0" font-size="11" fill="#111827" font-weight="600">LU（6 条指令）</text>
<text x="1122.0" y="903.0" font-size="8.5" fill="#475569">从 CM 读向量 / Mask / 标量</text>
<text x="1122.0" y="916.5" font-size="8.5" fill="#475569">格式转换 FP8_e4m3 / MXFP8 / BF16</text>
<text x="1122.0" y="930.0" font-size="8.5" fill="#475569">　→ BF16 / FP32，精确扩宽</text>
<text x="1122.0" y="943.5" font-size="8.5" fill="#475569">ld.fp32.vm 在 DATA_TYPE=BF16 下按</text>
<text x="1122.0" y="957.0" font-size="8.5" fill="#475569">　TYPE_VL.ROUND_MODE 把 FP32 窄化为</text>
<text x="1122.0" y="970.5" font-size="8.5" fill="#475569">　BF16，结果为 NaN 时置 DATA_CVT_ERROR</text>
<text x="1122.0" y="984.0" font-size="8.5" fill="#475569">CM 侧数据格式：FP8_e4m3 / MXFP8 /</text>
<text x="1122.0" y="997.5" font-size="8.5" fill="#475569">　BF16 / FP32</text>
<text x="1122.0" y="1011.0" font-size="8.5" fill="#475569">跨 128 B 边界的拆分与重组由 LU 完成</text>
<rect x="410" y="865.0" width="250" height="167.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="422" y="886.0" font-size="11" fill="#111827" font-weight="600">SU（6 条指令）</text>
<text x="422.0" y="903.0" font-size="8.5" fill="#475569">向 CM 写回</text>
<text x="422.0" y="916.5" font-size="8.5" fill="#475569">格式转换 BF16 / FP32 →</text>
<text x="422.0" y="930.0" font-size="8.5" fill="#475569">　FP8_e4m3 / MXFP8 / BF16 / FP32</text>
<text x="422.0" y="943.5" font-size="8.5" fill="#475569">高转低按 TYPE_VL.ROUND_MODE 舍入</text>
<text x="422.0" y="957.0" font-size="8.5" fill="#475569">跨 128 B 边界的拆分与重组由 SU 完成</text>
<text x="422.0" y="970.5" font-size="8.5" fill="#475569">CM 端口每周期 1 次 Load + 1 次 Store，</text>
<text x="422.0" y="984.0" font-size="8.5" fill="#475569">　各 128 B，与访问格式无关</text>
<text x="422.0" y="997.5" font-size="8.5" fill="#475569">一次请求固定 1024 bit，不支持 burst</text>
<text x="422.0" y="1011.0" font-size="8.5" fill="#475569">数据信号 1056 bit = 128 B data + 4 B scale</text>
<polygon points="1184,1072.5 1295,1072.5 1286,1102.5 1175,1102.5" fill="#f8fafc" stroke="#374151"/>
<text x="1235.0" y="1091.0" font-size="9" fill="#374151" text-anchor="middle">cmem_ld</text>
<polygon points="484,1072.5 595,1072.5 586,1102.5 475,1102.5" fill="#f8fafc" stroke="#374151"/>
<text x="535.0" y="1091.0" font-size="9" fill="#374151" text-anchor="middle">cmem_st</text>
<path d="M235.5 74.0 L239.9 109.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<path d="M400.6 75.0 L404.9 109.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M510.0 200.5 L559.0 193.9" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="489.2" y="84.5" width="91.7" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="535" y="92" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">macro_inst_trigger</text>
<path d="M860.0 193.8 L909.1 213.6" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="868.8" y="200.5" width="31.5" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="884.5365649781239" y="208.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">宏指令</text>
<path d="M710.0 110.0 L705.6 75.0" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#g)"/>
<rect x="806.4" y="84.5" width="267.1" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="940" y="92" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#0f766e" text-anchor="middle">ISQ 空且 store 落地 → dsa_done；EVENT_EN 置位时另发 Event</text>
<path d="M1195.6 318.0 L1195.0 379.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#a)"/>
<rect x="1197.8" y="309.1" width="10.5" height="79.8" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 1203 349.0)" x="1203" y="352.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">微指令 / 依赖检查</text>
<path d="M1254.0 865.0 L1254.5 548.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1256.8" y="683.4" width="10.5" height="45.8" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 1262 706.25)" x="1262" y="709.2" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">CM 读向量</text>
<path d="M1239.5 1072.5 L1235.1 1033.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M1330.0 418.1 L1281.0 430.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1285.5" y="416.8" width="40.0" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1305.4858609299645" y="424.25" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">寄存器源</text>
<path d="M1110.0 423.2 L1061.0 423.2" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M1110.0 543.2 L1061.0 543.2" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M1152.5 547.5 L1152.0 663.2 L1061.0 663.2" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M1212.0 547.5 L1212.0 783.2 L1061.0 783.2" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M1135.5 380.0 L1135.0 352.0 L714.0 352.0 L714.0 379.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="829.7" y="340.5" width="140.6" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="900" y="348" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">源操作数（SMUX 广播 / bypass）</text>
<path d="M760.0 423.2 L740.0 423.2 L740.0 671.1 L731.0 671.1" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M760.0 543.2 L748.0 543.2 L748.0 707.4 L731.0 707.4" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M760.0 663.2 L756.0 663.2 L756.0 743.5 L731.0 743.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M760.0 783.2 L752.0 783.2 L752.0 779.8 L731.0 779.8" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M410.0 423.2 L380.0 423.2 L380.0 788.8 L409.0 788.8" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M410.0 555.2 L390.0 555.2 L390.0 752.6 L409.0 752.6" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<text x="372" y="636.0" font-size="8.5" fill="#6b7280" text-anchor="end">结果 → DMUX</text>
<path d="M506.0 825.0 L534.4 864.2" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<text x="547" y="848.0" font-size="8.5" fill="#6b7280" text-anchor="start">DMUX → SU：写回通路</text>
<path d="M535.0 1032.5 L539.4 1071.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M442.0 825.0 L442.0 849.0 L360.0 849.0 L360.0 336.0 L1480.0 336.0 L1480.0 379.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="850.4" y="324.5" width="99.2" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="900" y="332" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">写回 VRF / MRF / SRF</text>
<text x="20" y="1162" font-size="10.5" fill="#374151" text-anchor="start">单条宏指令的容量上限：CM 端口 1 Load + 1 Store · VRF 2R+2W · MRF 2R+1W · SRF 8 逻辑读 / 6 逻辑写 · 每个执行单元各 1 次（SEXE 例外，同一物理单元 3 次串行迭代）。SEXE 的操作数来自 SRF、VALU1 归约结果或前一次迭代（图中省略连线）。</text>
<text x="20" y="1184" font-size="10.5" fill="#374151" text-anchor="start">向量位宽 1024 bit/cycle（32 个 FP32 或 64 个 BF16）；向量长度 1～16384 element，单条宏指令内完成；内部计算精度 FP32 或 BF16，单条宏指令内不支持混合精度。</text>
</svg>
```

***

## 2　功能清单

一功能一条，编号供“机制覆盖”一章引用。

### config_register

| 编号 | 功能 |
| - | - |
| F1 | 8 组静态配置模板，默认全 0；软件需要的组数不超过 8 时运行中无需改写 |
| F2 | 12 个动态参数寄存器（0x0000～0x002C）：`macro_inst_trigger`、`TYPE_VL`、`LD_addr`、`ST_addr`、`VRF_rd_index`、`VRF_wt_index`、`MRF_rd_index`、`MRF_wt_index`、`SRF_rd_index_0/1`、`SRF_wt_index_0/1`。静态模板区从 `N×0x100 + 0x1000` 起。`dsawi` 的立即数是 16 bit 字节地址、覆盖 0～64K，两个区都能直接用立即数寻址 |
| F3 | `macro_inst_trigger` 是唯一的启动寄存器，写一次执行一次；两次写之间没有其他配置也启动两次 |
| F4 | trigger 的六个字段：`CONFIG_IDX`（选静态配置组）、`STATIC_DYNAMIC_MASK`（逐参数选静态模板值还是动态寄存器值）、`EVENT_EN`、`STREAM_ID_OVERRIDE`、`DATA_BROADCAST`、`MACRO_INST_FENCE` |
| F5 | 宏指令的 `stream_id` 有两个来源：`STREAM_ID_OVERRIDE` 为 `0` 时沿用 VU-Core CSR 中自带的那一个，为 `1` 时改用 `macro_inst_trigger.STREAM_ID` 字段（4 bit，共 16 个 stream）给出的值，用来访问不属于本 task 的 stream。`task_id` 始终取 VU-Core CSR 那一份，不受这一位影响。`dsa_done` 回给 TS 的就是这一组 |
| F5a | VU-Core CSR 那一组从 VU RV core 经 `dsa_ids` 直连过来，每拍有效，写 `macro_inst_trigger` 那一拍采样。**`task_id` 不是软件配置项**：VU 的寄存器空间里没有它，软件写不进来，硬件在采样那一拍自动填进宏指令描述符，`dsa_done` 回 TS 时原样带出 |
| F6 | `TYPE_VL` 一个寄存器含 VL、DATA_TYPE、ROUND_MODE 三个字段，随 `STATIC_DYNAMIC_MASK.bit[0]` 一起在静态模板与动态寄存器之间切换 |
| F7 | 静态配置的改写规则：目标组正被未完成的宏指令引用时，硬件把这次配置写阻塞在配置通路上，等引用它的宏指令退休后写入生效、解除阻塞 |
| F8 | in-flight 的宏指令始终按改写前的配置执行完毕 |
| F9 | 三条配置通路（VU-Core / Ctrl-NOC / Debug Module）共享同一份寄存器视图、权限一致，流控彼此独立；VU-Core 的配置写因静态配置组被引用而阻塞时，Debug Module 与 Ctrl-NOC 仍能读出现场 |
| F10 | 全部寄存器的全部位域均为 RW。`macro_inst_left`、`status` 与 Profile 计数器由硬件维护，软件写入无效、不报错 |
| F11 | 经 `reg_file_addr` / `reg_file_data` 可读写 VRF / MRF / SRF，该通路与宏指令异步，须由软件保证访问期间目标 RF 不被 in-flight 宏指令读写 |

### ISQ

| 编号 | 功能 |
| - | - |
| F12 | VU-Core 写完动态参数后写 `macro_inst_trigger`，硬件锁存当前动态参数，与对应静态配置的指针打包压入内部执行队列 |
| F13 | 队列深度 8（待定） |
| F14 | `status` 寄存器实时回传 `BUSY`、`ISQ_FULL`、`ISQ_EMPTY`、`ERROR_FLAG`；判断全部宏指令是否完成用 `macro_inst_left` 或 `BUSY` |
| F15 | 读 `error_code` 时其全部异常位清零并同时清 `status.ERROR_FLAG`；Profile 计数器只能用 `profile_ctrl.CLEAR` 清零 |

### pipe_ctrl 与 Scoreboard

| 编号 | 功能 |
| - | - |
| F15a | 静态配置组每组 23 个寄存器：前 12 个是没有动态副本的 `LU_op` / `SU_op` / `VALU0-2_op` / `VSFU_op` / `MEXE_op` / `SEXE0-2_op` / `mask_op` / `PRF_op`，后 11 个是动态参数寄存器的静态副本、与动态版本逐位相同，组内偏移 = 对应动态寄存器地址 + `0x2C` |
| F15b | `STATIC_DYNAMIC_MASK` 的 7 个有效位各控制哪个参数：bit[0] `TYPE_VL`、bit[1] `LD_addr`、bit[2] `ST_addr`、bit[3] `VRF_rd_index`、bit[4] `VRF_wt_index`、bit[5] MRF 读写两个索引、bit[6] SRF 读写四个索引；bit[7] 保留。位为 1 取动态副本，为 0 取所属静态组里的那一份。全部 `*_op` / `mask_op` / `PRF_op` 没有动态副本，不在覆盖范围内 |
| F16 | 把宏指令展开成各执行单元的微指令 |
| F17 | Scoreboard 对 VRF / MRF / SRF 实时读写状态追踪，检测 RAW / WAR / WAW |
| F18 | 重叠执行：前后宏指令无数据依赖、无执行资源冲突时，后续宏指令无需等前一条完全结束即可重叠发射微操作，最多两条相邻宏指令重叠 |
| F19 | CM 访存依赖不追踪。存在冲突的宏指令之间须由软件置 `MACRO_INST_FENCE = 1`，该宏指令等此前全部宏指令完成后才开始派发 |
| F20 | 含 Vector 数据广播的宏指令须置 `DATA_BROADCAST`，它不参与 Scoreboard 乱序调度，等除 CM-Load 之外的前序宏指令全部完成后才派发 |
| F21 | 单条宏指令的容量上限：CM 端口 1 次 Load + 1 次 Store（仅支持单一基地址上的连续地址访问，scale 区不参与软件编址，MXFP8 时 scale 地址由硬件按一一映射推断）；VRF 2R + 2W；MRF 2R + 1W；SRF 8 逻辑读 / 6 逻辑写；每个执行单元 1 次，SEXE 例外 |
| F22 | 配平计算依赖树是软件的责任：硬件在执行单元之间只提供 bypass 与广播，不提供软件可见的缓冲队列。级数差一级时用 VALU2 的 `vmv.v.v` 当延迟对齐缓冲；级数差超出可配平范围时拆成多条宏指令，由 Scoreboard 经 RF 传中间结果 |
| F23 | 各执行单元 `*_op.OPCODE` 的未分配编码以及本单元不支持的编码一律按无操作处理，与 `0x00` 等效，不置位任何异常；`error_code` 没有 ILLEGAL_OPCODE 位 |

### LU 与 SU

| 编号 | 功能 |
| - | - |
| F24 | LU 6 条指令：从 CM 读向量 / Mask / 标量；格式转换 FP8_e4m3 / MXFP8 / BF16 → BF16 / FP32 为精确扩宽 |
| F25 | `ld.fp32.vm` 在 DATA_TYPE=BF16 下按 `TYPE_VL.ROUND_MODE` 把 FP32 窄化为 BF16，结果为 NaN 时置 `DATA_CVT_ERROR` |
| F26 | SU 6 条指令：向 CM 写回；格式转换 BF16 / FP32 → FP8_e4m3 / MXFP8 / BF16 / FP32，高转低按 `TYPE_VL.ROUND_MODE` 舍入 |
| F27 | CM 接口读写各一条独立通路，一次请求固定 1024 bit，不支持 burst；地址 32 bit 按 128 B 对齐，向量与掩码按 32 B 对齐、标量按 4 B 对齐 |
| F28 | 跨 128 B 边界的拆分与重组由 LU / SU 完成 |
| F29 | CM 数据信号 1056 bit = 128 B data + 4 B scale，scale 段仅 MXFP8 有效 |

### SMUX / DMUX

| 编号 | 功能 |
| - | - |
| F30 | SMUX 做源路由：执行单元之间允许 bypass 与广播，且不消耗 RF 端口 |
| F31 | DMUX 做结果路由：写回 VRF / MRF / SRF 或交给 SU |
| F32 | VRF 两个写口须指向不同执行单元，同时使能时写区间不重叠；来源只能是 LU 或 VALU0 / VALU1 / VALU2 / VSFU0 / VSFU1，其余置 `CFG_ERROR` |
| F33 | VRF 允许读写寄存器完全重叠或完全不重叠，不允许部分重叠。硬件不检查，由软件保证 |
| F34 | VALU1 的归约标量结果走 SRF 虚拟写口 |
| F35 | MRF 唯一写口的来源是 LU 的 `ld.vm_mask`、VALU0 的比较类与 `vfclass.mv`、MEXE 三者之一，同一宏指令内不能同时写回；Mask 不能广播，一条宏指令内最多两处使用 Mask |
| F36 | SRF 6 个写口按 `PRF_op.SRF_WT_EN` 位图使能：bit0 LU、bit1 VALU1、bit2 MEXE、bit3～5 SEXE0/1/2 |

### 执行单元

| 编号 | 功能 |
| - | - |
| F37 | VALU0（29 条独有）：加减乘、最值、MACC、除法（非全吞吐）、符号注入、比较生成 Mask、`vfclass`、`vfmerge`、标量广播 / 搬入 |
| F38 | VALU1（6 条独有）：加减乘、最值、跨元素归约（求和 / 最大 / 最小）、Top-16 排序（同时输出 16 个 INT16 索引）、标量广播 / 搬出 |
| F39 | VALU2（1 条独有）：加减乘、最值、标量广播、`vmv.v.v` 向量直通缓冲 |
| F40 | VSFU（12 条）：sin / cos / tanh / exp / exp2 / ln / log2 / rcp / rsqrt / sqrt / sigmoid。源不能取自身的输出；自定义拟合函数暂定不实现 |
| F40a | VSFU 有 `VSFU0` 与 `VSFU1` 两个功能一致的单元：FP32 精度下两者独立工作，BF16 精度下两者拼接成一个逻辑单元 |
| F40b | 除 SEXE 外，每个执行单元在单条宏指令里只能被调用 1 次 |
| F41 | MEXE（15 条）：Mask 逻辑运算（and / nand / andn / xor / or / nor / orn / xnor）、`vcpop.m`、`vfirst.m`、`vmsbf/vmsif/vmsof.m`、`vmiuset.mv` / `vmiset.mv`（按 16 个 INT16 索引清 / 置 Mask 位，配合 Top-K 做迭代查找） |
| F42 | SEXE（7 条）：fadd / fsub / fmul / fdiv / fsqrt / frsqrt / frcp（.s）。物理上只有一组，SEXE0/1/2 是同一物理单元在一条宏指令内的 3 次串行迭代 |
| F43 | SEXE 迭代之间天然链式依赖：SEXE1 的操作数可来自 SEXE0，SEXE2 可来自 SEXE1，因此第 2、3 次迭代只需 1 个额外的 SRF 读端口 |
| F44 | SEXE 操作数来源有四处：SRF 读端口、VALU1 的归约输出、LU 的 `ld.s.fp32` 结果、前一次 SEXE 迭代的结果。不支持立即数，也不能取 MEXE 为源，因为 MEXE 的标量输出是整数而 SEXE 只有浮点通路 |
| F45 | bit 级归约顺序：LANES 内归约再 ⌈log2 SEG⌉ 级累加，参考实现必须用同一顺序 |

### 数据类型与舍入

| 编号 | 功能 |
| - | - |
| F46 | `TYPE_VL.DATA_TYPE` 为 1 bit（bit16：0 = FP32，1 = BF16），只作用于向量通路；标量只有 FP32 一种精度 |
| F47 | `TYPE_VL.ROUND_MODE` 在 bit[19:17]，只作用于三处高转低转换：LU 的 `ld.fp32.vm` 在 DATA_TYPE=BF16 下把 FP32 窄化为 BF16；SU 的高转低写出（`st.fp8e4m3.vm` / `st.mxfp8.vm` / `st.bf16.vm`）；DATA_TYPE=BF16 时标量进入向量通路的 FP32 → BF16 转换 |
| F48 | 向量长度 VL 为 1～16384 element，单条宏指令内完成；`0` 等效于 `1`，大于 `16384` 等效于 `16384`，不报错 |
| F49 | VL 取上限 16384 时单个 FP32 Token 恰好占满全部 VRF；VL 更小时按实际长度占用，剩余容量可同时驻留多个 Token 或宏指令之间传递的中间结果 |

### 状态与同步

| 编号 | 功能 |
| - | - |
| F50 | 可向 TS 发送 Event 硬件同步信号，用于更细粒度的任务调度与软硬件解耦 |
| F51 | Profile 计数器区从 0x4000 起（`profile_ctrl` 在 0x4000，计数器从 0x4008 起） |

***

## 3　接口

```
port dsa_cfg (slave, valid/ready, clk)            // VU RV core 的 dsa_iss；Ctrl-NOC 与 Debug Module 共享同一份寄存器视图
  in  req_valid · req_we · req_addr[15:0] · req_wdata[31:0]
  out req_ready                                     // = 配置通路未阻塞；目标静态配置组被 in-flight 宏指令引用时拉低
port dsa_rdata (master, 脉冲, clk)                // 读寄存器的异步返回
  out valid · rdata[31:0]
port dsa_ids (slave, 电平, clk)                   // VU RV core 的 CSR 直连；写 macro_inst_trigger 那一拍采样
  in  stream_id[3:0] · task_id[5:0]                 // RV core 侧驱动四项，VU 只取这两项
port dsa_done (master, 脉冲, clk)                 // → TS：宏指令退休；EVENT_EN 置位时另发 Event 同步信号
  out valid · stream_id[3:0] · task_id[5:0] · event   // 取自 dsa_ids 采样的那一组，STREAM_ID_OVERRIDE 置位时 stream_id 改用 trigger 里显式给定的值
port cmem_ld (master, valid/ready, clk)           // LU → Core Mem，一次固定 1024 bit，不 burst
  out req_valid · req_addr[31:0]
  in  req_ready · rsp_valid · rsp_rdata[1023:0] · rsp_scale[31:0]
port cmem_st (master, valid/ready, clk)           // SU → Core Mem，一次固定 1024 bit
  out req_valid · req_addr[31:0] · req_wdata[1023:0] · req_scale[31:0]
  in  req_ready
port cfg (slave, ctrl_noc 写事务, clk)            // 静态配置模板与 RF 后门
  in  cfg_valid · cfg_addr[23:0] · cfg_we · cfg_wdata[31:0]
  out cfg_rdata[31:0]
```

***

## 4　存储器

```
mem static_cfg    FF 阵列   8 组 × 23 个（12 个 *_op / mask_op / PRF_op + 11 个副本）  1R1W  被 in-flight 引用时写阻塞  复位 0
mem dyn_param     FF 阵列   12 个动态参数寄存器（含 TYPE_VL）                        1R1W  dsa_cfg 写                复位 0
mem isq           FIFO      8 × {动态参数快照, 静态配置指针, stream_id, task_id}      1W1R  写 trigger 时压入          复位空
mem scoreboard    FF 阵列   VRF / MRF / SRF 的读写状态位图                            1RW   检测 RAW / WAR / WAW      复位 0
mem vrf           SRAM      64 KB：128 B/entry × 512 entry                            2R2W  读写索引连续              复位未定义
mem mrf           SRAM      4 KB                                                      2R1W  唯一写口三选一            复位未定义
mem srf           FF 阵列   256 B：4 B/entry × 64 entry                                8R6W  虚拟端口时分复用，无争用  复位未定义
mem status_reg    FF        {BUSY, ISQ_FULL, ISQ_EMPTY, ERROR_FLAG, macro_inst_left}   1R    硬件维护，软件写无效      复位 0
mem error_code    FF        异常位图                                                   1RW   读时全部清零并清 ERROR_FLAG  复位 0
mem profile_cnt   FF 阵列   计数器组，0x4008 起                                        1RW   只能用 profile_ctrl.CLEAR 清零  复位 0
mem 级间 latch     级间 latch 各执行单元之间的微操作与数据                              —     每拍覆写                  —
```

***

## 5　流水线总览

VU 与 VU-Core 之间的交互抽象是宏指令：一条宏指令一次配好整张计算图的通路，从 CM 读入、多级流水计算、写回 CM 的全过程由硬件自己走完。第 1 层图按发射、取数、算、写回四段画，各执行单元的级数设计未给，图上标 D变长。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 678 522" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arqov" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="678" height="522" fill="#ffffff"/>

  <text x="20" y="26" font-size="12" fill="#111827">VU · 第 1 层流水线总览（一条宏指令展开成各单元的微指令，最多两条相邻宏指令重叠）</text>
  <text x="20" y="42" font-size="9.5" fill="#6b7280">横向是级序，不是拍序；每级的拍数在右上角 Dx。橙色虚线框是变长级，非按比例。</text>
  <path d="M150 52 L150 70" stroke="#e5e7eb" fill="none"/>
<path d="M150 126 L150 242" stroke="#e5e7eb" fill="none"/>
<path d="M150 298 L150 414" stroke="#e5e7eb" fill="none"/>
  <path d="M316 52 L316 70" stroke="#e5e7eb" fill="none"/>
<path d="M316 126 L316 328" stroke="#e5e7eb" fill="none"/>
<path d="M316 384 L316 414" stroke="#e5e7eb" fill="none"/>
  <path d="M482 52 L482 70" stroke="#e5e7eb" fill="none"/>
<path d="M482 126 L482 242" stroke="#e5e7eb" fill="none"/>
<path d="M482 298 L482 414" stroke="#e5e7eb" fill="none"/>
  <text x="20" y="102" font-size="10.5" fill="#6b7280">发射</text>
  <rect x="150" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="84" font-size="8.5" fill="#6b7280">M1</text>
  <text x="292" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="104" font-size="11" fill="#111827">config_register</text>
  <text x="160" y="118" font-size="11" fill="#111827">写与 trigger</text>
  <rect x="316" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="84" font-size="8.5" fill="#6b7280">M2</text>
  <text x="458" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="104" font-size="11" fill="#111827">ISQ 压入出队</text>
  <path d="M300 98 L315 98" stroke="#475569" marker-end="url(#arqov)" fill="none"/>
  <rect x="482" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="492" y="84" font-size="8.5" fill="#6b7280">M3</text>
  <text x="624" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="492" y="104" font-size="11" fill="#111827">pipe_ctrl 展开</text>
  <text x="492" y="118" font-size="11" fill="#111827">与 Scoreboard</text>
  <path d="M466 98 L481 98" stroke="#475569" marker-end="url(#arqov)" fill="none"/>
  <text x="20" y="188" font-size="10.5" fill="#6b7280">取数</text>
  <rect x="150" y="156" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="160" y="170" font-size="8.5" fill="#92400e">M4</text>
  <text x="292" y="170" font-size="8.5" fill="#92400e" text-anchor="end">D14</text>
  <text x="160" y="190" font-size="11" fill="#7c2d12">LU 读 CM</text>
  <text x="20" y="274" font-size="10.5" fill="#6b7280">算</text>
  <rect x="150" y="242" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="256" font-size="8.5" fill="#6b7280">M5</text>
  <text x="292" y="256" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="276" font-size="11" fill="#111827">SMUX 源路由</text>
  <rect x="316" y="242" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="326" y="256" font-size="8.5" fill="#92400e">M6</text>
  <text x="458" y="256" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="326" y="276" font-size="11" fill="#7c2d12">执行单元</text>
  <path d="M300 270 L315 270" stroke="#475569" marker-end="url(#arqov)" fill="none"/>
  <rect x="482" y="242" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="492" y="256" font-size="8.5" fill="#6b7280">M7</text>
  <text x="624" y="256" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="492" y="276" font-size="11" fill="#111827">DMUX 结果路由</text>
  <path d="M466 270 L481 270" stroke="#475569" marker-end="url(#arqov)" fill="none"/>
  <text x="20" y="360" font-size="10.5" fill="#6b7280">写回</text>
  <rect x="150" y="328" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="160" y="342" font-size="8.5" fill="#92400e">M8</text>
  <text x="292" y="342" font-size="8.5" fill="#92400e" text-anchor="end">D14</text>
  <text x="160" y="362" font-size="11" fill="#7c2d12">SU 写 CM</text>
  <rect x="316" y="328" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="342" font-size="8.5" fill="#6b7280">M9</text>
  <text x="458" y="342" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="362" font-size="11" fill="#111827">退休与 dsa_done</text>
  <path d="M300 356 L315 356" stroke="#475569" marker-end="url(#arqov)" fill="none"/>
  <text x="20" y="438" font-size="10.5" fill="#374151">M6 里 VALU0 / VALU1 / VALU2 / VSFU / MEXE / SEXE 的级数各不相同，设计未给值，本轮各取 4 拍（待定）；SEXE0/1/2 是同一物理单元的三次串行迭代，因此是 3 倍。</text>
  <text x="20" y="466" font-size="10.5" fill="#374151">一条宏指令内多条并行通路经过的执行分组级数不同，合并点的两个源操作数会不同拍到达，配平是软件的责任：差一级用 VALU2 的 vmv.v.v 对齐，差得多就拆成多条宏指令。</text>
  <text x="20" y="494" font-size="10.5" fill="#374151">CM 访存依赖硬件不追踪，靠 MACRO_INST_FENCE；建模时若默认硬件会挡，结果会偏乐观。</text>
</svg>
```

***

## 6　逐级行为

### M1 · config_register 写与 trigger

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 873 272" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arq1" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="873" height="272" fill="#ffffff"/>

  <polygon points="30,20 188,20 178,96 20,96" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="39" font-size="10.5" fill="#374151" text-anchor="middle">dsa_cfg</text>
  <text x="104" y="57" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · req_we</text>
  <text x="104" y="75" font-size="9.5" fill="#6b7280" text-anchor="middle">req_addr[15:0]</text>
  <text x="104" y="93" font-size="9.5" fill="#6b7280" text-anchor="middle">req_wdata[31:0] · req_ready</text>
  <rect x="20" y="108" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="112" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="129" font-size="10" fill="#374151" text-anchor="middle">static_cfg · FF 8 组 · 1R1W</text>
  <rect x="20" y="162" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="166" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="183" font-size="10" fill="#374151" text-anchor="middle">dyn_param · FF 12 个 · 1R1W</text>
  <polygon points="30,216 188,216 178,262 20,262" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="234" font-size="10.5" fill="#374151" text-anchor="middle">dsa_ids</text>
  <text x="104" y="252" font-size="9.5" fill="#6b7280" text-anchor="middle">stream_id[3:0] · task_id[5:0]</text>
  <rect x="677" y="92" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="681" y="96" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="765" y="113" font-size="10" fill="#374151" text-anchor="middle">isq · FIFO 8 项 · 1W</text>
  <rect x="232" y="34" width="401" height="174" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="50" font-size="8.5" fill="#6b7280">M1</text>
  <text x="619" y="50" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="70" font-size="12" fill="#111827">config_register · 写一次执行一次</text>
  <text x="250" y="92" font-size="10.5" fill="#475569">1. dsa_cfg.req_we → 动态参数区或静态模板区按 req_addr 写入</text>
  <text x="250" y="112" font-size="10.5" fill="#475569">2. 目标静态组正被在飞宏指令引用 → req_ready = 0，阻塞这次写</text>
  <text x="250" y="132" font-size="10.5" fill="#475569">3. 写 macro_inst_trigger → 锁存当前 12 个动态参数为一份快照，同拍采样 dsa_ids</text>
  <text x="250" y="152" font-size="10.5" fill="#475569">4. stream_id = STREAM_ID_OVERRIDE ? trigger.STREAM_ID : dsa_ids 那一份；task_id 只取后者</text>
  <text x="250" y="172" font-size="10.5" fill="#475569">5. inst = {快照, CONFIG_IDX 指针, STATIC_DYNAMIC_MASK, 六个字段, stream_id, task_id}</text>
  <text x="250" y="192" font-size="10" fill="#9ca3af">在飞宏指令按改写前的配置执行完毕</text>
  <path d="M188 58 L231 58" stroke="#475569" marker-end="url(#arq1)" fill="none"/>
  <path d="M188 129 L231 129" stroke="#475569" marker-end="url(#arq1)" fill="none"/>
  <path d="M188 183 L231 183" stroke="#475569" marker-end="url(#arq1)" fill="none"/>
  <path d="M188 239 L210 239 L210 200 L231 200" stroke="#475569" marker-end="url(#arq1)" fill="none"/>
  <path d="M633 113 L676 113" stroke="#475569" marker-end="url(#arq1)" fill="none"/>
</svg>
```

### M2 · ISQ 压入出队

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 916 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arq2" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="916" height="198" fill="#ffffff"/>

  <rect x="20" y="42" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="46" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="63" font-size="10" fill="#374151" text-anchor="middle">isq · FIFO 8 项 · 1W1R</text>
  <rect x="20" y="104" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="108" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="125" font-size="10" fill="#374151" text-anchor="middle">动态参数 · FF · 1R</text>
  <rect x="720" y="42" width="176" height="114" fill="#f1f5f9" stroke="#334155"/>
  <rect x="720" y="42" width="176" height="18" fill="#334155"/>
  <text x="808" y="55" font-size="10.5" fill="#ffffff" text-anchor="middle">VU_INST</text>
  <text x="808" y="82" font-size="10" fill="#334155" text-anchor="middle">cfg_idx[2:0]</text>
  <text x="808" y="104" font-size="10" fill="#334155" text-anchor="middle">stream_id[3:0]</text>
  <text x="808" y="126" font-size="10" fill="#334155" text-anchor="middle">task_id[5:0]</text>
  <text x="808" y="148" font-size="10" fill="#334155" text-anchor="middle">VL[13:0] · dtype · round</text>
  <rect x="232" y="20" width="444" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M2</text>
  <text x="662" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">ISQ · 排队并回状态</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. isq.push(inst)；macro_inst_left += 1</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. status = {BUSY, ISQ_FULL=isq.full, ISQ_EMPTY=isq.empty, ERROR_FLAG}</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. 在飞宏指令数 &lt; 2 → cur = isq.pop()</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. stream_id 与 task_id 随描述符原样出队，是 M1 那一拍定下的那一组</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">判全部宏指令是否完成用 macro_inst_left 或 BUSY</text>
  <path d="M188 63 L231 63" stroke="#475569" marker-end="url(#arq2)" fill="none"/>
  <path d="M188 125 L231 125" stroke="#475569" marker-end="url(#arq2)" fill="none"/>
  <path d="M676 99 L719 99" stroke="#475569" marker-end="url(#arq2)" fill="none"/>
</svg>
```

### M3 · pipe_ctrl 展开与 Scoreboard

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 847 218" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arq3" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="847" height="218" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">VU_INST</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">cfg_idx[2:0]</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">VL[13:0]</text>
  <rect x="20" y="102" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="106" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="123" font-size="10" fill="#374151" text-anchor="middle">static_cfg · FF 8 组 · 1R</text>
  <rect x="20" y="156" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="160" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="177" font-size="10" fill="#374151" text-anchor="middle">scoreboard · FF 位图 · 1RW</text>
  <rect x="651" y="52" width="176" height="114" fill="#f1f5f9" stroke="#334155"/>
  <rect x="651" y="52" width="176" height="18" fill="#334155"/>
  <text x="739" y="65" font-size="10.5" fill="#ffffff" text-anchor="middle">UOPS</text>
  <text x="739" y="92" font-size="10" fill="#334155" text-anchor="middle">lu_op · su_op</text>
  <text x="739" y="114" font-size="10" fill="#334155" text-anchor="middle">valu_op[2:0]</text>
  <text x="739" y="136" font-size="10" fill="#334155" text-anchor="middle">vsfu_op · mexe_op</text>
  <text x="739" y="158" font-size="10" fill="#334155" text-anchor="middle">sexe_op[2:0]</text>
  <rect x="232" y="30" width="375" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="46" font-size="8.5" fill="#6b7280">M3</text>
  <text x="593" y="46" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="66" font-size="12" fill="#111827">pipe_ctrl · 展开微指令并查依赖</text>
  <text x="250" y="88" font-size="10.5" fill="#475569">1. uops = Expand(static_cfg[cfg_idx], 快照)，逐单元一条</text>
  <text x="250" y="108" font-size="10.5" fill="#475569">2. dep = RAW | WAR | WAW（对 VRF / MRF / SRF 逐区间比对）</text>
  <text x="250" y="128" font-size="10.5" fill="#475569">3. MACRO_INST_FENCE → 等此前全部宏指令完成才派发</text>
  <text x="250" y="148" font-size="10.5" fill="#475569">4. DATA_BROADCAST → 等除 CM-Load 外的前序宏指令全部完成才派发</text>
  <text x="250" y="172" font-size="10" fill="#9ca3af">最多两条相邻宏指令重叠</text>
  <path d="M188 55 L231 55" stroke="#475569" marker-end="url(#arq3)" fill="none"/>
  <path d="M188 123 L231 123" stroke="#475569" marker-end="url(#arq3)" fill="none"/>
  <path d="M188 177 L231 177" stroke="#475569" marker-end="url(#arq3)" fill="none"/>
  <path d="M607 109 L650 109" stroke="#475569" marker-end="url(#arq3)" fill="none"/>
</svg>
```

### M4 · LU 读 CM

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 938 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arq4" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="938" height="198" fill="#ffffff"/>

  <rect x="20" y="30" width="168" height="48" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="30" width="168" height="18" fill="#334155"/>
  <text x="104" y="43" font-size="10.5" fill="#ffffff" text-anchor="middle">UOPS</text>
  <text x="104" y="70" font-size="10" fill="#334155" text-anchor="middle">lu_op</text>
  <polygon points="30,90 188,90 178,166 20,166" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="109" font-size="10.5" fill="#374151" text-anchor="middle">cmem_ld</text>
  <text x="104" y="127" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · req_addr[31:0]</text>
  <text x="104" y="145" font-size="9.5" fill="#6b7280" text-anchor="middle">rsp_valid · rsp_rdata[1023:0]</text>
  <text x="104" y="163" font-size="9.5" fill="#6b7280" text-anchor="middle">rsp_scale[31:0]</text>
  <rect x="742" y="24" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="746" y="28" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="830" y="45" font-size="10" fill="#374151" text-anchor="middle">vrf · SRAM 64 KB · 1W</text>
  <rect x="742" y="78" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="746" y="82" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="830" y="99" font-size="10" fill="#374151" text-anchor="middle">mrf · SRAM 4 KB · 1W</text>
  <rect x="742" y="132" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="746" y="136" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="830" y="153" font-size="10" fill="#374151" text-anchor="middle">srf · FF 256 B · 1W</text>
  <rect x="232" y="20" width="466" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M4</text>
  <text x="684" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D14</text>
  <text x="250" y="56" font-size="12" fill="#111827">LU · 取向量、Mask 与标量并扩宽</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. cmem_ld.req = {addr}，一次固定 1024 bit，不 burst</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. 跨 128 B 边界的访问由本级拆分再重组</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. v = Widen(rsp_rdata, FP8_e4m3 / MXFP8 / BF16 → BF16 / FP32)，精确扩宽</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. ld.fp32.vm 且 DATA_TYPE=BF16 → 按 ROUND_MODE 窄化，NaN 置 DATA_CVT_ERROR</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">14 拍是 Cmem 侧的 VU 访问延迟</text>
  <path d="M188 54 L231 54" stroke="#475569" marker-end="url(#arq4)" fill="none"/>
  <path d="M188 128 L231 128" stroke="#475569" marker-end="url(#arq4)" fill="none"/>
  <path d="M698 45 L741 45" stroke="#475569" marker-end="url(#arq4)" fill="none"/>
  <path d="M698 99 L741 99" stroke="#475569" marker-end="url(#arq4)" fill="none"/>
  <path d="M698 153 L741 153" stroke="#475569" marker-end="url(#arq4)" fill="none"/>
</svg>
```

### M5 · SMUX 源路由

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 881 272" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arq5" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="881" height="272" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">UOPS</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">valu_op[2:0]</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">sexe_op[2:0]</text>
  <rect x="20" y="102" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="106" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="123" font-size="10" fill="#374151" text-anchor="middle">vrf · SRAM · 2R</text>
  <rect x="20" y="156" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="160" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="177" font-size="10" fill="#374151" text-anchor="middle">mrf · SRAM · 2R</text>
  <rect x="20" y="210" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="214" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="231" font-size="10" fill="#374151" text-anchor="middle">srf · FF · 8R</text>
  <rect x="685" y="90" width="176" height="92" fill="#f1f5f9" stroke="#334155"/>
  <rect x="685" y="90" width="176" height="18" fill="#334155"/>
  <text x="773" y="103" font-size="10.5" fill="#ffffff" text-anchor="middle">SRC</text>
  <text x="773" y="130" font-size="10" fill="#334155" text-anchor="middle">a[1023:0] · b[1023:0]</text>
  <text x="773" y="152" font-size="10" fill="#334155" text-anchor="middle">mask[*]</text>
  <text x="773" y="174" font-size="10" fill="#334155" text-anchor="middle">scalar[31:0]</text>
  <rect x="232" y="57" width="409" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="73" font-size="8.5" fill="#6b7280">M5</text>
  <text x="627" y="73" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="93" font-size="12" fill="#111827">SMUX · 选源，bypass 与广播不占 RF 端口</text>
  <text x="250" y="115" font-size="10.5" fill="#475569">1. src[u] = static_cfg 指定的来源：RF 读口、别的单元的输出、或广播</text>
  <text x="250" y="135" font-size="10.5" fill="#475569">2. bypass 与广播直接从产生方取，不消耗 RF 端口</text>
  <text x="250" y="155" font-size="10.5" fill="#475569">3. VRF 2R、MRF 2R、SRF 8 逻辑读，超出上限的配置在 M3 已被拒</text>
  <text x="250" y="175" font-size="10.5" fill="#475569">4. SEXE 的源只有三处：SRF 读口、VALU1 的归约输出、前一次 SEXE 的结果</text>
  <text x="250" y="199" font-size="10" fill="#9ca3af">Mask 不能广播，一条宏指令内最多两处使用</text>
  <line x1="188" y1="55" x2="228" y2="55" stroke="#475569" marker-end="url(#arq5)"/>
  <path d="M188 123 L231 123" stroke="#475569" marker-end="url(#arq5)" fill="none"/>
  <path d="M188 177 L231 177" stroke="#475569" marker-end="url(#arq5)" fill="none"/>
  <line x1="188" y1="231" x2="228" y2="231" stroke="#475569" marker-end="url(#arq5)"/>
  <path d="M641 136 L684 136" stroke="#475569" marker-end="url(#arq5)" fill="none"/>
</svg>
```

### M6 · 执行单元

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 947 216" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arq6" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="947" height="216" fill="#ffffff"/>

  <rect x="20" y="62" width="168" height="92" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="62" width="168" height="18" fill="#334155"/>
  <text x="104" y="75" font-size="10.5" fill="#ffffff" text-anchor="middle">SRC</text>
  <text x="104" y="102" font-size="10" fill="#334155" text-anchor="middle">a[1023:0] · b[1023:0]</text>
  <text x="104" y="124" font-size="10" fill="#334155" text-anchor="middle">mask[*]</text>
  <text x="104" y="146" font-size="10" fill="#334155" text-anchor="middle">scalar[31:0]</text>
  <rect x="751" y="62" width="176" height="92" fill="#f1f5f9" stroke="#334155"/>
  <rect x="751" y="62" width="176" height="18" fill="#334155"/>
  <text x="839" y="75" font-size="10.5" fill="#ffffff" text-anchor="middle">RES</text>
  <text x="839" y="102" font-size="10" fill="#334155" text-anchor="middle">vres[1023:0]</text>
  <text x="839" y="124" font-size="10" fill="#334155" text-anchor="middle">mres[*]</text>
  <text x="839" y="146" font-size="10" fill="#334155" text-anchor="middle">sres[31:0]</text>
  <rect x="232" y="20" width="475" height="176" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M6</text>
  <text x="693" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="250" y="56" font-size="12" fill="#111827">VALU / VSFU / MEXE / SEXE · 各自算一拍组</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. VALU0：加减乘、最值、MACC、除法、符号注入、比较生成 Mask、vfclass</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. VALU1：加减乘、最值、跨元素归约、Top-16 排序；归约走 SRF 虚拟写口</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. VSFU：sin/cos/tanh/exp/exp2/ln/log2/rcp/rsqrt/sqrt/sigmoid，源不取自身输出</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. SEXE：同一物理单元串行迭代三次，第 2、3 次的源可取前一次结果</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">归约顺序：LANES 内归约再 ⌈log2 SEG⌉ 级累加，与参考实现同序</text>
  <text x="250" y="180" font-size="10" fill="#9ca3af">未分配与本单元不支持的 OPCODE 按无操作处理，不置异常</text>
  <path d="M188 108 L231 108" stroke="#475569" marker-end="url(#arq6)" fill="none"/>
  <path d="M707 108 L750 108" stroke="#475569" marker-end="url(#arq6)" fill="none"/>
</svg>
```

### M7 · DMUX 结果路由

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 876 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arq7" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="876" height="198" fill="#ffffff"/>

  <rect x="20" y="53" width="168" height="92" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="53" width="168" height="18" fill="#334155"/>
  <text x="104" y="66" font-size="10.5" fill="#ffffff" text-anchor="middle">RES</text>
  <text x="104" y="93" font-size="10" fill="#334155" text-anchor="middle">vres[1023:0]</text>
  <text x="104" y="115" font-size="10" fill="#334155" text-anchor="middle">mres[*]</text>
  <text x="104" y="137" font-size="10" fill="#334155" text-anchor="middle">sres[31:0]</text>
  <rect x="680" y="24" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="684" y="28" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="768" y="45" font-size="10" fill="#374151" text-anchor="middle">vrf · SRAM 64 KB · 2W</text>
  <rect x="680" y="78" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="684" y="82" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="768" y="99" font-size="10" fill="#374151" text-anchor="middle">mrf · SRAM 4 KB · 1W</text>
  <rect x="680" y="132" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="684" y="136" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="768" y="153" font-size="10" fill="#374151" text-anchor="middle">srf · FF 256 B · 6W</text>
  <rect x="232" y="20" width="404" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M7</text>
  <text x="622" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">DMUX · 写回 RF 或交给 SU</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. VRF 两个写口指向不同执行单元，且写区间不重叠</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. MRF 唯一写口三选一：LU 的 ld.vm_mask、VALU0 的比较类、MEXE</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. SRF 六个写口按 PRF_op.SRF_WT_EN 位图使能，虚拟端口时分复用无争用</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. 目标是 SU → 直接交给 M8，不经 RF</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">VRF 允许读写完全重叠或完全不重叠，不允许部分重叠</text>
  <path d="M188 99 L231 99" stroke="#475569" marker-end="url(#arq7)" fill="none"/>
  <path d="M636 45 L679 45" stroke="#475569" marker-end="url(#arq7)" fill="none"/>
  <path d="M636 99 L679 99" stroke="#475569" marker-end="url(#arq7)" fill="none"/>
  <path d="M636 153 L679 153" stroke="#475569" marker-end="url(#arq7)" fill="none"/>
</svg>
```

### M8 · SU 写 CM

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 899 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arq8" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="899" height="198" fill="#ffffff"/>

  <rect x="20" y="48" width="168" height="48" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="48" width="168" height="18" fill="#334155"/>
  <text x="104" y="61" font-size="10.5" fill="#ffffff" text-anchor="middle">RES</text>
  <text x="104" y="88" font-size="10" fill="#334155" text-anchor="middle">vres[1023:0]</text>
  <rect x="20" y="108" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="112" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="129" font-size="10" fill="#374151" text-anchor="middle">vrf · SRAM · 2R</text>
  <polygon points="713,60 879,60 869,136 703,136" fill="#f8fafc" stroke="#374151"/>
  <text x="791" y="79" font-size="10.5" fill="#374151" text-anchor="middle">cmem_st</text>
  <text x="791" y="97" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · req_addr[31:0]</text>
  <text x="791" y="115" font-size="9.5" fill="#6b7280" text-anchor="middle">req_wdata[1023:0]</text>
  <text x="791" y="133" font-size="9.5" fill="#6b7280" text-anchor="middle">req_scale[31:0] · req_ready</text>
  <rect x="232" y="20" width="427" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M8</text>
  <text x="645" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D14</text>
  <text x="250" y="56" font-size="12" fill="#111827">SU · 转精度后写回 Core Mem</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. o = Narrow(v, BF16/FP32 → FP8_e4m3 / MXFP8 / BF16 / FP32)</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. 高转低按 TYPE_VL.ROUND_MODE 舍入</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. cmem_st.req = {addr, wdata 1024 b, scale 32 b}，一次固定 1024 bit</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. 跨 128 B 边界的写由本级拆分</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">MXFP8 的 scale 地址由硬件按一一映射推断，不参与软件编址</text>
  <path d="M188 72 L231 72" stroke="#475569" marker-end="url(#arq8)" fill="none"/>
  <path d="M188 129 L231 129" stroke="#475569" marker-end="url(#arq8)" fill="none"/>
  <path d="M659 98 L707 98" stroke="#475569" marker-end="url(#arq8)" fill="none"/>
</svg>
```

### M9 · 退休与 dsa_done

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 877 218" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arq9" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="877" height="218" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">VU_INST</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">stream_id[3:0]</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">task_id[5:0]</text>
  <rect x="20" y="102" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="106" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="123" font-size="10" fill="#374151" text-anchor="middle">scoreboard · FF · 1W</text>
  <rect x="20" y="156" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="160" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="177" font-size="10" fill="#374151" text-anchor="middle">status_reg · FF · 1W</text>
  <polygon points="691,52 857,52 847,110 681,110" fill="#f8fafc" stroke="#374151"/>
  <text x="769" y="71" font-size="10.5" fill="#374151" text-anchor="middle">dsa_done</text>
  <text x="769" y="89" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · stream_id[3:0]</text>
  <text x="769" y="107" font-size="9.5" fill="#6b7280" text-anchor="middle">task_id[5:0] · event</text>
  <rect x="681" y="122" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="685" y="126" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="769" y="143" font-size="10" fill="#374151" text-anchor="middle">static_cfg · FF · 引用释放</text>
  <rect x="232" y="30" width="405" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="46" font-size="8.5" fill="#6b7280">M9</text>
  <text x="623" y="46" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="66" font-size="12" fill="#111827">宏指令退休 · 释放配置组并报完成</text>
  <text x="250" y="88" font-size="10.5" fill="#475569">1. 该宏指令的全部微指令都完成 → macro_inst_left −= 1</text>
  <text x="250" y="108" font-size="10.5" fill="#475569">2. 释放对 static_cfg[cfg_idx] 的引用，被阻塞的配置写这时生效</text>
  <text x="250" y="128" font-size="10.5" fill="#475569">3. dsa_done = {valid=1, stream_id, task_id}，取自 M2 锁存的那一组</text>
  <text x="250" y="148" font-size="10.5" fill="#475569">4. EVENT_EN → event = 1，与完成同拍发给 TS</text>
  <text x="250" y="172" font-size="10" fill="#9ca3af">退休后被阻塞的静态配置写入生效并解除阻塞</text>
  <path d="M188 55 L231 55" stroke="#475569" marker-end="url(#arq9)" fill="none"/>
  <path d="M188 123 L231 123" stroke="#475569" marker-end="url(#arq9)" fill="none"/>
  <path d="M188 177 L231 177" stroke="#475569" marker-end="url(#arq9)" fill="none"/>
  <path d="M637 81 L685 81" stroke="#475569" marker-end="url(#arq9)" fill="none"/>
  <path d="M637 143 L680 143" stroke="#475569" marker-end="url(#arq9)" fill="none"/>
</svg>
```

***

## 7　参数汇总

```
向量位宽 VW        1024 bit/cycle，等效每周期 32 个 FP32 或 64 个 BF16
向量长度 VL        1～16384 element，单条宏指令内完成；0 等效 1，>16384 等效 16384，不报错
CM 访存带宽        每周期 1 次 Load + 1 次 Store，各 128 B，与访问格式无关
CM 侧数据格式       FP8_e4m3 / MXFP8 / BF16 / FP32
CM 数据信号        1056 bit = 128 B data + 4 B scale，scale 段仅 MXFP8 有效；地址 32 bit 按 128 B 对齐
CM 访问延迟        VU 侧 14T
内部计算精度        FP32 或 BF16，单条宏指令内不支持混合精度
片内寄存器          VRF 64 KB（128 B/entry × 512 entry，2R2W）· MRF 4 KB（2R1W）· SRF 256 B（4 B × 64 entry，8 逻辑读 / 6 逻辑写）
执行单元           VEXE（VALU0 / VALU1 / VALU2 / VSFU）· MEXE · SEXE
宏指令配置          8 组静态配置模板 + 12 个动态参数寄存器；8 组模板由编译侧算好，boot 期经 ctrl_noc 的 cfg 口写入
宏指令重叠          最多两条相邻
ISQ 深度           8（待定）
算力               32 MAC/T（FP32 口径）
内部启动延迟        40T（流水启动 5T + 20 条指令算地址 30T + core 发射 5T）
寄存器偏移          动态参数区 0x0000～0x002C；静态配置区 N×0x100 + 0x1000 起；Profile 区 0x4000 起
Top-K              K 固定为 16
```

***

## 8　机制覆盖

| 机制 | 功能 | 用例 |
| - | - | - |
| 8 组静态模板 + 12 个动态参数，trigger 写一次执行一次 | F1、F3 | `macro_inst_trigger` |
| 静态组 23 个 = 12 个 op 加 11 个动态副本，副本偏移 = 动态地址 + 0x2C | F15a | `static_dup_layout` |
| STATIC_DYNAMIC_MASK 逐位控制哪个参数走动态，op 类不在覆盖范围内 | F15b | `static_dynamic_mask` |
| stream_id 取自 VU-Core CSR 或 trigger 的 STREAM_ID 字段，task_id 只取前者 | F5 | `vu_ids_source` |
| VU-Core CSR 那一组走 dsa_ids 直连，写 trigger 那一拍采样 | F5a | `vu_ids_direct` |
| 动态参数区与静态模板区都能用 dsawi 的 16 bit 字节地址直接寻址 | F2 | `reg_addressing` |
| TYPE_VL 三字段随 STATIC_DYNAMIC_MASK 切换 | F6 | `type_vl_switch` |
| 静态配置组被引用时配置写阻塞，in-flight 按改写前执行完 | F7、F8 | `static_cfg_block` |
| 三条配置通路共享寄存器视图，流控独立 | F9 | `three_cfg_paths` |
| status / macro_inst_left / Profile 软件写无效不报错 | F10 | `readonly_status` |
| RF 后门通路与宏指令异步，由软件保证不冲突 | F11 | `rf_backdoor` |
| Scoreboard 检测 RAW / WAR / WAW | F17 | `scoreboard_dep` |
| 最多两条相邻宏指令重叠 | F18 | `macro_overlap_two` |
| CM 访存冲突硬件不追踪，靠 MACRO_INST_FENCE | F19 | `macro_inst_fence` |
| DATA_BROADCAST 不参与乱序调度 | F20 | `data_broadcast` |
| 单条宏指令的五类容量上限 | F21 | `macro_resource_cap` |
| 配平依赖树是软件的责任，硬件只提供 bypass 与广播 | F22 | `no_hw_buffer_queue` |
| 未分配与不支持的 OPCODE 按无操作处理，不报异常 | F23 | `opcode_nop` |
| LU / SU 的格式转换与三处舍入 | F24～F26、F47 | `vu_convert_round` |
| CM 一次固定 1024 bit，不支持 burst | F27 | `cm_no_burst` |
| 跨 128 B 边界的拆分与重组由 LU / SU 完成 | F28 | `cross_128b` |
| VRF 两个写口指向不同单元且写区间不重叠 | F32 | `vrf_write_ports` |
| VRF 读写允许完全重叠或完全不重叠，不允许部分重叠 | F33 | `vrf_overlap_rule` |
| VALU1 的归约标量走 SRF 虚拟写口 | F34 | `valu1_srf_write` |
| MRF 唯一写口三选一，同一宏指令内不能同时写回 | F35 | `mrf_write_source` |
| SRF 6 个写口按位图使能，无端口争用 | F36 | `srf_write_enable` |
| SEXE 是同一物理单元的 3 次串行迭代 | F42、F43 | `sexe_iteration` |
| SEXE 操作数只有三处来源，不支持立即数与 MEXE | F44 | `sexe_operand_source` |
| 归约按 LANES 内再 ⌈log2 SEG⌉ 级累加，与参考实现同序 | F45 | `reduce_tree_order` |
| VL 边界：0 等效 1，超上限等效 16384，不报错 | F48 | `vl_clamp` |
| 可向 TS 发 Event 同步信号 | F50 | `vu_event` |

***

## 9　取舍

* **为什么 VU 与 VU-Core 之间用宏指令而不是逐条微指令**
  * 一条宏指令一次配好整张计算图的通路，VU-Core 不感知周期级的控制细节
  * 代价是拆分边界由硬件资源上限决定，软件要自己算一个算子拆成几条
* **为什么 CM 访存依赖不追踪**
  * 追踪 CM 冲突要在 DSA 里维护地址范围的重叠判断，成本高
  * 交给软件用 `MACRO_INST_FENCE` 显式隔离。建模时若默认硬件会挡，结果会偏乐观
* **为什么归一化用求倒数加向量乘标量而不是向量除法**
  * 避免全吞吐的向量流水去等约 20～30 cycle 的除法
* **为什么执行单元之间不提供软件可见的缓冲队列**
  * 提供队列就要提供队列的调度与观测，接口面积大
  * 只提供 bypass 与广播，配平交给软件：级数差一级用 `vmv.v.v` 对齐，差得多就拆成多条宏指令
