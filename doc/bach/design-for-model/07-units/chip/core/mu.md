# MU DSA

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **MU DSA**

给实现 MU 的人：七个独立打拍的模块各自做哪些事、端口与存储怎么定。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《执行单元与存储》“MU DSA（矩阵单元）”全部小节
* 《归约的完整过程》：“第一层：core 内的专家间求和”
* 《软件栈》：“计算 Core”的中间若干 task

***

## 1　定位与边界

MU 是为 MoE 算子深度定制的 GEMV 加速核心，服务 Batch = 1（Token = 1）的极低延时推理。一趟数据流：

* **读**：token 从 Core Mem，weight 从 Matrix Mem（bank 与 lane 一对一垂直贴合，无 crossbar）
* **算**：32 个物理 lane 各 10 级流水的 MAC 阵列
* **写**：结果经 stq 拼成 1 KB 写回 Core Mem

支持两种矩阵运算形式：

| 形式 | 说明 |
| - | - |
| `C = A × B` | 普通 GEMV |
| `C = C + (A × B) × W_ep` | 矩阵乘加专家间 reduce，**不支持初始 C 加载**：第一个专家的结果直接写 `C`，后续专家逐个累加 |

第二种形式把 EP reduce 从 VU 挪进 MU，Core Mem 不必为每个用户缓存所有激活专家的中间结果。MoE 多专家场景默认走它。

任务由 MU RV core 写寄存器加 trigger 下发，走 `regfile → issue_q → gen_ep_info → agu / acu → ldq → matrix exe → stq`；load、计算、写回三段在相邻 task 之间重叠。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1520 900" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
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
  <rect x="0" y="0" width="1520" height="900" fill="#ffffff"/>
  <text x="20" y="26" font-size="12" fill="#111827">MU DSA · 第 0 层（七个独立打拍的模块）</text>
  <text x="295" y="26" font-size="9.5" fill="#6b7280">执行流水 regfile → issue_q → gen_ep_info → agu / acu → ldq → matrix exe → stq；load、计算、写回三段在相邻 task 之间重叠</text>
  <polygon points="36,116 196,116 187,146 27,146" fill="#f8fafc" stroke="#374151"/>
  <text x="112" y="135" font-size="9" fill="#374151" text-anchor="middle">dsa_cfg / dsa_rdata</text>
  <rect x="250" y="84" width="260" height="156" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="262" y="105" font-size="11" fill="#111827">regfile</text>
  <text x="262" y="122" font-size="8.5" fill="#475569">静态配置：基本不随用户变化，</text>
  <text x="262" y="135.5" font-size="8.5" fill="#475569">　初始化阶段配好，业务流阶段快速调用</text>
  <text x="262" y="149.0" font-size="8.5" fill="#475569">动态配置：随用户变化，跟随任务下发</text>
  <text x="262" y="162.5" font-size="8.5" fill="#475569">启动：dsawi.d topk_stream_stride, trigger</text>
  <text x="262" y="176.0" font-size="8.5" fill="#475569">trigger 含 last 标志</text>
  <text x="262" y="189.5" font-size="8.5" fill="#475569">streamID / taskID / userID 由 DSA 自己读，</text>
  <text x="262" y="203.0" font-size="8.5" fill="#475569">　不需要软件配置</text>
  <rect x="556" y="84" width="230" height="156" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="568" y="105" font-size="11" fill="#111827">issue_q</text>
  <text x="568" y="122" font-size="8.5" fill="#475569">队列深度 16</text>
  <text x="568" y="135.5" font-size="8.5" fill="#475569">顺序执行与 finish</text>
  <text x="568" y="149.0" font-size="8.5" fill="#475569">与启动延时有关</text>
  <text x="568" y="162.5" font-size="8.5" fill="#475569">任务切换无 bubble</text>
  <text x="568" y="176.0" font-size="8.5" fill="#475569">task 间在执行通路上不同操作</text>
  <text x="568" y="189.5" font-size="8.5" fill="#475569">类型可重叠：task0 load →</text>
  <text x="568" y="203.0" font-size="8.5" fill="#475569">{task0 算 ‖ task1 load} →</text>
  <text x="568" y="216.5" font-size="8.5" fill="#475569">{task0 写回 ‖ task1 算} → …</text>
  <rect x="832" y="84" width="330" height="156" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="844" y="105" font-size="11" fill="#111827">gen_ep_info</text>
  <text x="844" y="122" font-size="8.5" fill="#475569">按任务信息索引 topK 激活专家信息</text>
  <text x="844" y="135.5" font-size="8.5" fill="#475569">用 topK 里的 global index 索引 local_ep_table</text>
  <text x="844" y="149.0" font-size="8.5" fill="#475569">　转成 local index，方便算 weight 访存地址</text>
  <text x="844" y="162.5" font-size="8.5" fill="#475569">local_ep_table 记录当前 EP Group 内有哪些专家</text>
  <text x="844" y="176.0" font-size="8.5" fill="#475569">　及各自在组内的序号</text>
  <text x="844" y="189.5" font-size="8.5" fill="#475569">topK_ep_table：DTE 搬进来的 topK，每 stream ≤ 256 B</text>
  <text x="844" y="203.0" font-size="8.5" fill="#475569">　FC1 / FC3 只需 ids，FC2 需 ids 与 weights</text>
  <text x="844" y="216.5" font-size="8.5" fill="#475569">router_expert_count = 0 时忽略 topK 相关寄存器</text>
  <rect x="250" y="300" width="340" height="196" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="262" y="321" font-size="11" fill="#111827">agu ×3 与 acu</text>
  <text x="262" y="338" font-size="8.5" fill="#475569">agu ×3：分别算 token、weight、结果的访存地址</text>
  <text x="262" y="351.5" font-size="8.5" fill="#475569">任务拆分顺序：先循环 tile_K，再循环 tile_N</text>
  <text x="262" y="365.0" font-size="8.5" fill="#475569">acu：检查地址越界与对齐</text>
  <text x="262" y="378.5" font-size="8.5" fill="#475569">异常时向阵列发排空指令（Drain）：</text>
  <text x="262" y="392.0" font-size="8.5" fill="#475569">　1. 阻塞任务下发</text>
  <text x="262" y="405.5" font-size="8.5" fill="#475569">　2. 清理已发出的访存请求（已请求的回复照常</text>
  <text x="262" y="419.0" font-size="8.5" fill="#475569">　　 处理，不再发起新的）</text>
  <text x="262" y="432.5" font-size="8.5" fill="#475569">　3. 排空计算流水线</text>
  <text x="262" y="446.0" font-size="8.5" fill="#475569">　4. 恢复默认状态</text>
  <text x="262" y="459.5" font-size="8.5" fill="#475569">允许已进入脉动通路的合法数据正常算完并写回，</text>
  <text x="262" y="473.0" font-size="8.5" fill="#475569">　仅丢弃越界任务数据，防止状态机死锁</text>
  <rect x="636" y="300" width="280" height="196" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="648" y="321" font-size="11" fill="#111827">Token ldq</text>
  <text x="648" y="338" font-size="8.5" fill="#475569">队列深度 16（取决于读延时）</text>
  <text x="648" y="351.5" font-size="8.5" fill="#475569">Core Mem 读带宽 256 B（接口 512 B / 1 KB）</text>
  <text x="648" y="365.0" font-size="8.5" fill="#475569">　与 MAC 阵列接口 256 B</text>
  <text x="648" y="378.5" font-size="8.5" fill="#475569">Core Mem 读延迟 16（待定）</text>
  <text x="648" y="392.0" font-size="8.5" fill="#475569">Rd outstanding buffer 16 × 256 B = 4 KB</text>
  <text x="648" y="405.5" font-size="8.5" fill="#475569">　用来掩盖 latency</text>
  <text x="648" y="419.0" font-size="8.5" fill="#475569">vlane 机制影响 Load token：从 buffer 只读取</text>
  <text x="648" y="432.5" font-size="8.5" fill="#475569">　256 B / vlane_num 字节，再 copy 扩展到 256 B</text>
  <rect x="962" y="300" width="300" height="196" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="974" y="321" font-size="11" fill="#111827">Weight ldq</text>
  <text x="974" y="338" font-size="8.5" fill="#475569">队列深度 4</text>
  <text x="974" y="351.5" font-size="8.5" fill="#475569">Matrix Mem 读带宽 8 KB</text>
  <text x="974" y="365.0" font-size="8.5" fill="#475569">　bank 与 lane 一对一垂直贴合，无 crossbar</text>
  <text x="974" y="378.5" font-size="8.5" fill="#475569">Matrix Mem 读延迟 4T（读 sram 2T + 打拍 2T）</text>
  <text x="974" y="392.0" font-size="8.5" fill="#475569">各 lane 访存地址相同，只需发一个地址</text>
  <text x="974" y="405.5" font-size="8.5" fill="#475569">　然后逐级脉动到各 lane</text>
  <text x="974" y="419.0" font-size="8.5" fill="#475569">MAC 入口用乒乓 2 级缓存掩盖 Mmem 读出延迟</text>
  <rect x="250" y="556" width="720" height="236" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="262" y="577" font-size="11" fill="#111827">matrix exe</text>
  <text x="262" y="594" font-size="8.5" fill="#475569">32 个物理 Lane，左右镜像各 16 lane；单 Lane 内 10 级混合高频流水</text>
  <text x="262" y="607.5" font-size="8.5" fill="#475569">物理阵列规格二选一：1×K256×N32（输出带宽 128 B）或 1×K128×N64（输出带宽 256 B）</text>
  <text x="262" y="621.0" font-size="8.5" fill="#475569">两种运算形式：C = A × B，以及 C = C + (A × B) × W_ep（矩阵乘加专家间 reduce，不支持初始 C 加载）</text>
  <text x="262" y="634.5" font-size="8.5" fill="#475569">算力：BF16×BF16 4K MACs · MXFP8×MXFP8 8K MACs（scale block 32，E8M0）</text>
  <text x="262" y="648.0" font-size="8.5" fill="#475569">　　　MXFP8×MXFP4 与 MXFP8×NVFP4 各 16K MACs（scale block 16，FP8）· BF16×MXFP4 与 BF16×NVFP4 各 8K MACs</text>
  <text x="262" y="661.5" font-size="8.5" fill="#475569">八种计算原语：MXFP8 的 1×K128×N64 与 1×K64×N128；BF16 的 1×K64×N64 与 1×K32×N128；</text>
  <text x="262" y="675.0" font-size="8.5" fill="#475569">　　　　　　　W4A8 的 1×K256×N64 与 1×K128×N128；W4A16 的 1×K128×N64 与 1×K64×N128</text>
  <text x="262" y="688.5" font-size="8.5" fill="#475569">vlane 机制：把 MAC 按 vlane 分组，在 CSA 加法树的第 128 输入层级节点插旁路 MUX，配上对应 vlane 分组的</text>
  <text x="262" y="702.0" font-size="8.5" fill="#475569">　MUX 逻辑和 Ksplit_acc 寄存器，做到单 lane 同时输出多个结果；vlane 有 1 和 2 两种模式</text>
  <text x="262" y="715.5" font-size="8.5" fill="#475569">数据类型：token(A) / weight(B) 输入 BF16 或 MXFP8；W_ep 输入 FP32；输出 FP32 或 BF16</text>
  <text x="262" y="729.0" font-size="8.5" fill="#475569">bit 级累加顺序：CSA 树按 scale block 分组累加，参考实现必须用同一顺序</text>
  <text x="262" y="742.5" font-size="8.5" fill="#475569">计算异常 MATH_NAN_INF 不阻塞流水，由硬件自动 Clamp；零输入旁路 + 特殊值穿透；DIDT 分级启动，最小分级为单 lane</text>
  <rect x="1010" y="556" width="300" height="236" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="1022" y="577" font-size="11" fill="#111827">stq</text>
  <text x="1022" y="594" font-size="8.5" fill="#475569">队列深度 16</text>
  <text x="1022" y="607.5" font-size="8.5" fill="#475569">Core Mem 写带宽 256 B（接口 1 KB）</text>
  <text x="1022" y="621.0" font-size="8.5" fill="#475569">　不足 1 KB 按实际传输并标记 mask</text>
  <text x="1022" y="634.5" font-size="8.5" fill="#475569">Core Mem 写延迟 16（待定）</text>
  <text x="1022" y="648.0" font-size="8.5" fill="#475569">Wr concat buffer 1～2 KB：</text>
  <text x="1022" y="661.5" font-size="8.5" fill="#475569">　各 lane buffer 深度不同，取决于物理距离，</text>
  <text x="1022" y="675.0" font-size="8.5" fill="#475569">　最远 16 拍、最近 1 拍，越近 buffer 越大，</text>
  <text x="1022" y="688.5" font-size="8.5" fill="#475569">　最大深度 16</text>
  <text x="1022" y="702.0" font-size="8.5" fill="#475569">Store concat 按 vlane 分两种拼装：</text>
  <text x="1022" y="715.5" font-size="8.5" fill="#475569">　vlane=1 步进横切，所有 lane buffer 并行 128 B</text>
  <text x="1022" y="729.0" font-size="8.5" fill="#475569">　　截面，连续取 8 次攒满 1024 B（8T）</text>
  <text x="1022" y="742.5" font-size="8.5" fill="#475569">　vlane=2 纵向整块，每 lane 一次取 8 B、共 256 B</text>
  <text x="1022" y="756.0" font-size="8.5" fill="#475569">　　截面，连续取 4 次攒满 1024 B（4T）</text>
  <polyline points="196,131 223,131 223,128 250,128" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="510,154 556,154" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="786,154 832,154" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="924,240 924,272 461,272 461,300" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="590,378 636,378" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="916,378 962,378" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="737,496 737,528 624,528 624,556" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="1088,496 1088,528 869,528 869,556" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="970,627 1010,627" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polygon points="1360,340 1490,340 1481,370 1351,370" fill="#f8fafc" stroke="#374151"/>
  <text x="1421" y="359" font-size="9" fill="#374151" text-anchor="middle">mmem_rd</text>
  <polygon points="1360,620 1490,620 1481,650 1351,650" fill="#f8fafc" stroke="#374151"/>
  <text x="1421" y="639" font-size="9" fill="#374151" text-anchor="middle">cmem_rd</text>
  <polygon points="1360,700 1490,700 1481,730 1351,730" fill="#f8fafc" stroke="#374151"/>
  <text x="1421" y="719" font-size="9" fill="#374151" text-anchor="middle">cmem_wr</text>
  <polyline points="1262,355 1351,355" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="1310,702 1330,702 1330,715 1351,715" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="877,496 877,518 1329,518 1329,635 1351,635" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <text x="1320" y="512" font-size="8.5" fill="#6b7280" text-anchor="end">Token ldq ↔ Core Mem，MU 侧 132 B/T（11T）</text>
  <polygon points="36,834 186,834 177,864 27,864" fill="#f8fafc" stroke="#374151"/>
  <text x="107" y="853" font-size="9" fill="#374151" text-anchor="middle">dsa_done → TS</text>
  <polyline points="293,792 293,849 186,849" fill="none" stroke="#0f766e" marker-end="url(#g)"/>
  <text x="300" y="828" font-size="8.5" fill="#0f766e" text-anchor="start">issue_q 的 finish 与 stq 写回完成合成 dsa_done；trigger 里的 last 标志决定这一笔要不要报 TS</text>
  <polygon points="1360,120 1490,120 1481,150 1351,150" fill="#f8fafc" stroke="#374151"/>
  <text x="1421" y="139" font-size="9" fill="#374151" text-anchor="middle">cfg（ctrl_noc）</text>
  <polyline points="1420,120 1420,66 1116,66 1116,84" fill="none" stroke="#7c3aed" stroke-dasharray="2 3" marker-end="url(#p)"/>
  <text x="1180" y="66" font-size="8.5" fill="#7c3aed" text-anchor="end">local_ep_table 与静态配置由 ctrl_noc 写入</text>
  <text x="20" y="884" font-size="10.5" fill="#374151">异常统一走 Drain &amp; Trap 四步，覆盖 Load / Store misalign、access fault、bus·ecc error 三类；本轮只留状态位与接口名。</text>
</svg>
```

***

## 2　功能清单

一功能一条，编号供“机制覆盖”一章引用。

### regfile

| 编号 | 功能 |
| - | - |
| F1 | 寄存器分静态配置与动态配置：静态配置基本不随用户变化，初始化阶段配好、业务流阶段快速调用；动态配置随用户变化，跟随任务下发，含静态配置的选择 |
| F2 | 任务启动用 `dsawi.d topk_stream_stride, trigger`，trigger 寄存器含 last 标志 |
| F3 | `streamID` / `taskID` / `userID` 由 DSA 自己读取，不需要软件配置 |
| F4 | 寄存器地址映射本轮用临时映射（`regmap.h`），等《MU/DTE 寄存器配置参数》到手后改 |

### issue_q

| 编号 | 功能 |
| - | - |
| F5 | 队列深度 16，与启动延时有关 |
| F6 | 顺序执行与 finish |
| F7 | 任务切换无 bubble |
| F8 | 支持 task 间在执行通路上不同操作类型的重叠：`task0 load → {task0 计算 ‖ task1 load} → {task0 写回 ‖ task1 计算} → …` |

### gen_ep_info

| 编号 | 功能 |
| - | - |
| F9 | 按任务信息索引 topK 激活专家信息 |
| F10 | 用 topK 里的 global index 索引 `local_ep_table` 转成 local index，方便算 weight 访存地址 |
| F11 | `local_ep_table` 记录当前 EP Group 内有哪些专家以及各自在组内的序号 |
| F12 | token 数据与 topK 信息分开存放：FC1 / FC3 只需 topK ids，FC2 需 ids 与 weights |
| F13 | `router_expert_count = 0` 时忽略 topK 相关寄存器 |

### agu ×3 与 acu

| 编号 | 功能 |
| - | - |
| F14 | 三个 agu 分别算 token、weight、结果的访存地址 |
| F15 | 任务拆分顺序：先循环 tile_K，再循环 tile_N |
| F16 | acu 检查地址越界与对齐 |
| F17 | 异常时向阵列发排空指令（Drain），四步：阻塞任务下发 → 清理已发出的访存请求（已请求的回复照常处理，不再发起新的）→ 排空计算流水线 → 恢复默认状态 |
| F18 | Drain 期间允许已进入脉动通路的合法数据正常算完并写回，仅丢弃越界任务数据，防止状态机死锁 |
| F19 | 走 Drain & Trap 的异常三类：Load / Store misalign、Load / Store access fault、Load / Store bus·ecc error。本轮只留状态位与接口名 |

### ldq ×2

| 编号 | 功能 |
| - | - |
| F20 | Token ldq 队列深度 16，取决于读延时；Core Mem 读带宽 256 B（接口 512 B / 1 KB），与 MAC 阵列接口 256 B；读延迟 16（待定） |
| F21 | Rd outstanding buffer 16 × 256 B = 4 KB，用来掩盖 latency |
| F22 | Weight ldq 队列深度 4；Matrix Mem 读带宽 8 KB，bank 与 lane 一对一垂直贴合、无 crossbar；读延迟 4T（读 sram 2T + 打拍 2T） |
| F23 | Weight 各 lane 访存地址相同，只需发一个地址然后逐级脉动到各 lane |
| F24 | MAC 入口用乒乓 2 级缓存掩盖 Mmem 读出延迟 |
| F25 | vlane 机制对 Load token 的影响：从 buffer 只读取 `256 B / vlane_num` 字节，再 copy 扩展到 256 B 输出 |

### matrix exe

| 编号 | 功能 |
| - | - |
| F26 | 32 个物理 Lane，左右镜像各 16 lane；单 Lane 内 10 级混合高频流水 |
| F27 | 物理阵列规格二选一：`1×K256×N32`（输出带宽 128 B）或 `1×K128×N64`（输出带宽 256 B） |
| F28 | 算力按精度组合分六档：BF16×BF16 4K MACs；MXFP8×MXFP8 8K MACs（scale block 32，E8M0）；MXFP8×MXFP4 与 MXFP8×NVFP4 各 16K MACs（scale block 16，FP8）；BF16×MXFP4 与 BF16×NVFP4 各 8K MACs |
| F29 | 八种计算原语：MXFP8 的 `1×K128×N64` 与 `1×K64×N128`；BF16 的 `1×K64×N64` 与 `1×K32×N128`；W4A8 的 `1×K256×N64` 与 `1×K128×N128`；W4A16 的 `1×K128×N64` 与 `1×K64×N128` |
| F30 | 数据类型：token(A) 与 weight(B) 输入 BF16 或 MXFP8，`W_ep` 输入 FP32，输出 FP32 或 BF16 |
| F31 | vlane 机制：把 MAC 按 vlane 分组，在 CSA 加法树的第 128 输入层级节点插旁路 MUX，配上对应 vlane 分组的 MUX 逻辑和 `Ksplit_acc` 寄存器，做到单 lane 同时输出多个结果。vlane 有 1 和 2 两种模式 |
| F32 | bit 级累加顺序：CSA 树按 scale block 分组累加，参考实现必须用同一顺序 |
| F33 | 计算异常 `MATH_NAN_INF` 不走 Drain & Trap，不阻塞流水，由硬件自动 Clamp |
| F34 | 单 lane MAC 阵列 bitmask 计算，零输入旁路加特殊值（NaN / Inf）穿透 |
| F35 | DIDT 分级启动，分级模式可配置，最小分级为单 lane 启动；Matrix 与 Vector 错峰启动，防止二者功耗陡升叠加。本轮只留状态位与接口名 |
| F36 | 性能目标：MXFP8 下 Primitive K256×N32 和 K128×N64 阵列利用率 100%，BF16 与 MXFP4 同样 100%；Primitive `1×64×128` 且 K=64 时有 50% 性能损失；Tile K×N 过小会有性能损失，由 RV core 配置延时和 MU 内启动延时决定 |

### stq

| 编号 | 功能 |
| - | - |
| F37 | 队列深度 16；Core Mem 写带宽 256 B（接口 1 KB），不足 1 KB 按实际传输并标记 mask；写延迟 16（待定） |
| F38 | Wr concat buffer 1～2 KB：各 lane buffer 深度不同，取决于物理距离，最远 16 拍、最近 1 拍，越近 buffer 越大，最大深度 16 |
| F39 | Store concat 按 vlane 分两种拼装方式：`vlane=1` 步进横切，所有 lane buffer 并行 128 B 截面，连续取 8 次攒满 1024 B（8T）；`vlane=2` 纵向整块，每 lane 一次取 8 B、共 256 B 截面，连续取 4 次攒满 1024 B（4T） |
| F40 | 结果写回 Core Mem 后与 issue_q 的 finish 合成 `dsa_done`；trigger 里的 last 标志决定这一笔要不要报 TS |

***

## 3　接口

```
port dsa_cfg (slave, valid/ready, clk)            // MU RV core 的 dsa_iss
  in  req_valid · req_we · req_addr[11:0] · req_wdata[31:0]
  out req_ready                                     // = regfile 配置通路未反压
port dsa_rdata (master, 脉冲, clk)                // 读寄存器的异步返回
  out valid · rdata[31:0]
port dsa_done (master, 脉冲, clk)                 // → TS：trigger 的 last 标志置位的那一笔完成时报
  out valid · stream_id[3:0] · task_id[5:0]
port cmem_rd (master, valid/ready, clk)           // Token ldq → Core Mem，132 B（128 B data + 4 B scale）
  out req_valid · req_addr[17:0] · req_scale_en
  in  req_ready · rsp_valid · rsp_rdata[1023:0] · rsp_scale[31:0]
port cmem_wr (master, valid/ready, clk)           // stq → Core Mem，132 B；1 KB 突发按 8 拍发
  out req_valid · req_addr[17:0] · req_wdata[1023:0] · req_scale[31:0] · req_be[127:0]
  in  req_ready
port mmem_rd (master, valid/ready, clk)           // Weight ldq → Matrix Mem，只读；一个行地址广播到各 bank
  out req_valid · req_addr[24:0]
  in  req_ready · rsp_valid · rsp_rdata[65535:0] · rsp_scale[8191:0]   // 8 KB data + 1 KB scale
port cfg (slave, ctrl_noc 写事务, clk)            // local_ep_table 与静态配置
  in  cfg_valid · cfg_addr[23:0] · cfg_we · cfg_wdata[31:0]
  out cfg_rdata[31:0]
```

***

## 4　存储器

```
mem regfile         FF 阵列   静态配置组 + 动态参数寄存器                          1R1W  dsa_cfg 写            复位 0
mem issue_q         FIFO      16 × 任务描述                                        1W1R  顺序执行              复位空
mem local_ep_table  FF 阵列   当前 EP Group 内的专家与组内序号                      1R    ctrl_noc 配置         复位由输入给
mem topK_ep_table   FF 阵列   16 stream × 256 B，每项 {expert_id 2 B, weight 4 B}   1R1W  DTE 写入              复位空
mem token_ldq       FIFO      16 × {addr[17:0], tag}                                1W1R  取决于读延时           复位空
mem rd_outstanding  FF 阵列   16 × 256 B = 4 KB                                     1RW   掩盖 latency          复位空
mem weight_ldq      FIFO      4 × {addr[24:0]}                                      1W1R  各 lane 地址相同       复位空
mem mac_ping_pong   FF 阵列   每 lane 2 级缓存                                      1RW   掩盖 Mmem 读出延迟     复位空
mem ksplit_acc      FF 阵列   每 lane 一组，vlane 分组的部分和                       1RW   vlane=2 时使用         复位 0
mem stq             FIFO      16 × {addr[17:0], data}                               1W1R  —                     复位空
mem wr_concat       FF 阵列   各 lane 1～2 KB，深度 1～16 不等（越近越大）           1RW   按 vlane 两种拼装      复位空
mem 级间 latch       级间 latch 单 lane 内 10 级                                     —     每拍覆写              —
```

***

## 5　流水线总览

第 1 层图待单 lane 10 级与三段重叠的拍数定下来后补，届时按 t 标尺把 load、计算、写回三段按拍对齐画在一张图上。

参数表记的“执行拍数 / 流水延时 9T”与 matrix exe 一节的“单 Lane 内深度 10 级”差 1 拍，可能是含或不含脉动输出那一级，第 1 层图定稿时一并核定。

***

## 6　逐级行为

第 2 层图与每级的四要素待第 1 层图完成后补，级编号回标到第 1 层图。

***

## 7　参数汇总

```
物理 lane          32（左右镜像各 16），单 lane 10 级流水
物理阵列规格        1×K256×N32（输出 128 B）或 1×K128×N64（输出 256 B），二选一
算力               BF16 4K · MXFP8 8K · W4A8 16K · W4A16 8K MAC/T
issue_q            16
Token ldq / Weight ldq / stq   16 / 4 / 16
Rd outstanding buffer          16 × 256 B = 4 KB
Wr concat buffer   1～2 KB（各 lane 深度 1～16 不等）
Core Mem 读 / 写    256 B（接口 512 B / 1 KB）；MU 侧 132 B/T，延迟 11T；MU 读延迟 16、写延迟 16（待定）
Matrix Mem 读       8 KB（+1 KB scale），lane 内延迟 8T；ldq 侧记 4T（读 sram 2T + 打拍 2T）
内部启动延迟        40T（流水启动 5T + 20 条指令算地址 30T + core 发射 5T，文档注“偏小”）
MU 配置耗时上限     < 64 T（未来可能要求 < 32 T）
单 token 执行耗时   f(N, X) = 4096 × X / N（T），N 是每 core 的专家数、X 是激活数
Matrix Mem bank 数  **口径冲突**：MU MAS 记 32 bank 与 32 lane 一对一，Mmem MAS 记 64 bank。未解，直接影响 8 KB/T 的组织方式
寄存器地址映射      临时映射（regmap.h），等《MU/DTE 寄存器配置参数》
```

***

## 8　机制覆盖

| 机制 | 功能 | 用例 |
| - | - | - |
| 静态与动态配置分开，trigger 含 last 标志 | F1、F2 | `mu_regfile_split` |
| streamID / taskID / userID 由 DSA 自读 | F3 | `mu_ids_selfread` |
| issue_q 顺序执行，任务切换无 bubble | F6、F7 | `mu_issue_q` |
| task 间三段重叠 | F8 | `mu_three_stage_overlap` |
| topK 的 global index 经 local_ep_table 转 local index | F10、F11 | `gen_ep_info` |
| router_expert_count = 0 时忽略 topK 寄存器 | F13 | `no_topk` |
| 先循环 tile_K 再循环 tile_N | F15 | `tile_order` |
| acu 越界 / 对齐检查触发 Drain 四步 | F16、F17 | `mu_drain_trap` |
| Drain 时已进入脉动通路的合法数据照常算完写回 | F18 | `drain_keep_valid` |
| Weight 各 lane 地址相同，发一个地址逐级脉动 | F23 | `weight_systolic_addr` |
| MAC 入口乒乓 2 级缓存掩盖 Mmem 读延迟 | F24 | `mac_pingpong` |
| vlane 影响 Load token 的读取与扩展 | F25 | `vlane_load` |
| 32 lane × 10 级，六档精度组合的算力 | F26、F28 | `mu_array_spec` |
| 八种计算原语 | F29 | `mu_primitives` |
| C = C + (A × B) × W_ep，不支持初始 C 加载 | 定位与边界 | `ep_reduce_in_mu` |
| vlane 的 CSA 旁路 MUX 与 Ksplit_acc | F31 | `vlane_split` |
| CSA 树按 scale block 分组累加，与参考实现同序 | F32 | `csa_order` |
| MATH_NAN_INF 不阻塞流水，硬件自动 Clamp | F33 | `nan_clamp` |
| 零输入旁路与特殊值穿透 | F34 | `zero_bypass` |
| stq 不足 1 KB 按实际传输并标记 mask | F37 | `stq_mask` |
| Wr concat buffer 按物理距离定深度 | F38 | `concat_buffer_depth` |
| Store concat 按 vlane 分两种拼装 | F39 | `vlane_store` |
| last 标志决定这一笔要不要报 TS | F40 | `mu_dsa_done` |

***

## 9　取舍

* **为什么把专家间累加挪进 MU**
  * `C = C + (A × B) × W_ep` 让 Core Mem 不必为每个用户缓存所有激活专家的中间结果，显著减少容量和带宽需求
  * VU 保留跨元素归约原语作为备用路径
* **为什么 Matrix Mem 的 bank 与 lane 一对一垂直贴合**
  * 省掉 crossbar；各 lane 的 weight 地址相同，发一个地址逐级脉动即可
* **为什么 Wr concat buffer 越近的 lane 越大**
  * 各 lane 到 stq 的物理距离不同，最远 16 拍、最近 1 拍
  * 近的 lane 先到，要多缓一会儿等远的，所以 buffer 反而要大
