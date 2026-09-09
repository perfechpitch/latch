# MU DSA

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **MU DSA**

给实现 MU 的人：七个逐拍推进的模块各自做哪些事、端口与存储怎么定。MU MAS 的模块表分八项（`config regfile`、`issue_q`、`gen_ep_info`、`agu`、`acu`、`ldq`、`matrix exe`、`stq`），本文档把 `agu` 与 `acu` 合在一节讲，其余一一对应。

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
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1700 930" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" role="img" aria-label="MU DSA 第 0 层">
<title>MU DSA 第 0 层</title>
<defs><marker id="a" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#475569"/></marker><marker id="as" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#475569"/></marker><marker id="g" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0f766e"/></marker><marker id="gs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0f766e"/></marker><marker id="o" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#b45309"/></marker><marker id="os" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#b45309"/></marker><marker id="p" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#7c3aed"/></marker><marker id="ps" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#7c3aed"/></marker><marker id="i" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#4338ca"/></marker><marker id="is" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#4338ca"/></marker><marker id="t" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0d9488"/></marker><marker id="ts" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0d9488"/></marker><marker id="r" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#be123c"/></marker><marker id="rs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#be123c"/></marker><marker id="b" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#2563eb"/></marker><marker id="bs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#2563eb"/></marker><marker id="m" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#d97706"/></marker><marker id="ms" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#d97706"/></marker><marker id="l" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#9aa1ad"/></marker><marker id="ls" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#9aa1ad"/></marker></defs>
<rect x="0" y="0" width="1700" height="930" fill="#ffffff"/>
<text x="20" y="26" font-size="12" fill="#111827">MU DSA · 第 0 层（七个逐拍推进的模块。方位：RV core 与 TS 在上，Matrix Mem / Core Mem 在下，cfg 从上进）</text>
<text x="702" y="26" font-size="9.5" fill="#6b7280">执行流水 regfile → issue_q → gen_ep_info → agu / acu → ldq → matrix exe → stq；load、计算、写回三段在相邻 task 之间重叠</text>
<rect x="250" y="110" width="260" height="140.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="262" y="131" font-size="11" fill="#111827" font-weight="600">regfile</text>
<text x="262.0" y="148.0" font-size="8.5" fill="#475569">静态配置：基本不随用户变化，</text>
<text x="262.0" y="161.5" font-size="8.5" fill="#475569">　初始化阶段配好，业务流阶段快速调用</text>
<text x="262.0" y="175.0" font-size="8.5" fill="#475569">动态配置：随用户变化，跟随任务下发</text>
<text x="262.0" y="188.5" font-size="8.5" fill="#475569">启动：dsawi 先写 topk_stream_stride，后写 trigger</text>
<text x="262.0" y="202.0" font-size="8.5" fill="#475569">trigger 含 last 标志</text>
<text x="262.0" y="215.5" font-size="8.5" fill="#475569">streamID / taskID / userID 由 DSA 自己读，</text>
<text x="262.0" y="229.0" font-size="8.5" fill="#475569">　不需要软件配置</text>
<rect x="556" y="110" width="230" height="154.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="568" y="131" font-size="11" fill="#111827" font-weight="600">issue_q</text>
<text x="568.0" y="148.0" font-size="8.5" fill="#475569">队列深度 16</text>
<text x="568.0" y="161.5" font-size="8.5" fill="#475569">顺序执行与 finish</text>
<text x="568.0" y="175.0" font-size="8.5" fill="#475569">与启动延时有关</text>
<text x="568.0" y="188.5" font-size="8.5" fill="#475569">任务切换无 bubble</text>
<text x="568.0" y="202.0" font-size="8.5" fill="#475569">task 间在执行通路上不同操作</text>
<text x="568.0" y="215.5" font-size="8.5" fill="#475569">类型可重叠：task0 load →</text>
<text x="568.0" y="229.0" font-size="8.5" fill="#475569">{task0 算 ‖ task1 load} →</text>
<text x="568.0" y="242.5" font-size="8.5" fill="#475569">{task0 写回 ‖ task1 算} → …</text>
<rect x="832" y="110" width="330" height="154.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="844" y="131" font-size="11" fill="#111827" font-weight="600">gen_ep_info</text>
<text x="844.0" y="148.0" font-size="8.5" fill="#475569">按任务信息索引 topK 激活专家信息</text>
<text x="844.0" y="161.5" font-size="8.5" fill="#475569">用 topK 里的 global index 索引 local_ep_table</text>
<text x="844.0" y="175.0" font-size="8.5" fill="#475569">　转成 local index，方便算 weight 访存地址</text>
<text x="844.0" y="188.5" font-size="8.5" fill="#475569">local_ep_table 记录当前 EP Group 内有哪些专家</text>
<text x="844.0" y="202.0" font-size="8.5" fill="#475569">　及各自在组内的序号</text>
<text x="844.0" y="215.5" font-size="8.5" fill="#475569">topK_ep_table：从 Core Mem 载入的 topK，每 stream ≤ 256 B</text>
<text x="844.0" y="229.0" font-size="8.5" fill="#475569">　FC1 / FC3 只需 ids，FC2 需 ids 与 weights</text>
<text x="844.0" y="242.5" font-size="8.5" fill="#475569">router_expert_count = 0 时忽略 topK 相关寄存器</text>
<polygon points="319,44 450,44 441,74 310,74" fill="#f8fafc" stroke="#374151"/>
<text x="380.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">dsa_cfg / dsa_rdata</text>
<polygon points="620,44 731,44 722,74 611,74" fill="#f8fafc" stroke="#374151"/>
<text x="671.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">dsa_done → TS</text>
<polygon points="946,44 1057,44 1048,74 937,74" fill="#f8fafc" stroke="#374151"/>
<text x="997.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">cfg（ctrl_noc）</text>
<rect x="250" y="330" width="340" height="194.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="262" y="351" font-size="11" fill="#111827" font-weight="600">agu ×3 与 acu</text>
<text x="262.0" y="368.0" font-size="8.5" fill="#475569">agu ×3：分别算 token、weight、结果的访存地址</text>
<text x="262.0" y="381.5" font-size="8.5" fill="#475569">任务拆分顺序：先循环 tile_K，再循环 tile_N</text>
<text x="262.0" y="395.0" font-size="8.5" fill="#475569">acu：检查地址越界与对齐</text>
<text x="262.0" y="408.5" font-size="8.5" fill="#475569">异常时向阵列发排空指令（Drain）：</text>
<text x="262.0" y="422.0" font-size="8.5" fill="#475569">　1. 阻塞任务下发</text>
<text x="262.0" y="435.5" font-size="8.5" fill="#475569">　2. 清理已发出的访存请求（已请求的回复照常</text>
<text x="262.0" y="449.0" font-size="8.5" fill="#475569">　　 处理，不再发起新的）</text>
<text x="262.0" y="462.5" font-size="8.5" fill="#475569">　3. 排空计算流水线</text>
<text x="262.0" y="476.0" font-size="8.5" fill="#475569">　4. 恢复默认状态</text>
<text x="262.0" y="489.5" font-size="8.5" fill="#475569">允许已进入脉动通路的合法数据正常算完并写回，</text>
<text x="262.0" y="503.0" font-size="8.5" fill="#475569">　仅丢弃越界任务数据，防止状态机死锁</text>
<rect x="636" y="330" width="280" height="154.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="648" y="351" font-size="11" fill="#111827" font-weight="600">Token ldq</text>
<text x="648.0" y="368.0" font-size="8.5" fill="#475569">队列深度 16（取决于读延时）</text>
<text x="648.0" y="381.5" font-size="8.5" fill="#475569">Core Mem 读带宽 256 B（接口 512 B / 1 KB）</text>
<text x="648.0" y="395.0" font-size="8.5" fill="#475569">　与 MAC 阵列接口 256 B</text>
<text x="648.0" y="408.5" font-size="8.5" fill="#475569">Core Mem 读延迟 16（待定）</text>
<text x="648.0" y="422.0" font-size="8.5" fill="#475569">Rd outstanding buffer 16 × 256 B = 4 KB</text>
<text x="648.0" y="435.5" font-size="8.5" fill="#475569">　用来掩盖 latency</text>
<text x="648.0" y="449.0" font-size="8.5" fill="#475569">vlane 机制影响 Load token：从 buffer 只读取</text>
<text x="648.0" y="462.5" font-size="8.5" fill="#475569">　256 B / vlane_num 字节，再 copy 扩展到 256 B</text>
<rect x="962" y="330" width="300" height="140.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="974" y="351" font-size="11" fill="#111827" font-weight="600">Weight ldq</text>
<text x="974.0" y="368.0" font-size="8.5" fill="#475569">队列深度 4</text>
<text x="974.0" y="381.5" font-size="8.5" fill="#475569">Matrix Mem 读带宽 8 KB</text>
<text x="974.0" y="395.0" font-size="8.5" fill="#475569">　bank 与 lane 一对一垂直贴合，无 crossbar</text>
<text x="974.0" y="408.5" font-size="8.5" fill="#475569">Matrix Mem 读延迟 4T（读 sram 2T + 打拍 2T）</text>
<text x="974.0" y="422.0" font-size="8.5" fill="#475569">各 lane 访存地址相同，只需发一个地址</text>
<text x="974.0" y="435.5" font-size="8.5" fill="#475569">　然后逐级脉动到各 lane</text>
<text x="974.0" y="449.0" font-size="8.5" fill="#475569">MAC 入口用乒乓 2 级缓存掩盖 Mmem 读出延迟</text>
<rect x="250" y="584.5" width="680" height="208.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="262" y="605.5" font-size="11" fill="#111827" font-weight="600">matrix exe</text>
<text x="262.0" y="622.5" font-size="8.5" fill="#475569">32 个物理 Lane，左右镜像各 16 lane；单 Lane 内 10 级混合高频流水</text>
<text x="262.0" y="636.0" font-size="8.5" fill="#475569">物理阵列规格二选一：1×K256×N32（输出带宽 128 B）或 1×K128×N64（输出带宽 256 B）</text>
<text x="262.0" y="649.5" font-size="8.5" fill="#475569">两种运算形式：C = A × B，以及 C = C + (A × B) × W_ep（矩阵乘加专家间 reduce，不支持初始 C 加载）</text>
<text x="262.0" y="663.0" font-size="8.5" fill="#475569">算力：BF16×BF16 4K MACs · MXFP8×MXFP8 8K MACs（scale block 32，E8M0）</text>
<text x="262.0" y="676.5" font-size="8.5" fill="#475569">　　　MXFP8×MXFP4 与 MXFP8×NVFP4 各 16K MACs（scale block 16，FP8）· BF16×MXFP4 与 BF16×NVFP4 各 8K MACs</text>
<text x="262.0" y="690.0" font-size="8.5" fill="#475569">八种计算原语：MXFP8 的 1×K128×N64 与 1×K64×N128；BF16 的 1×K64×N64 与 1×K32×N128；</text>
<text x="262.0" y="703.5" font-size="8.5" fill="#475569">　　　　　　　W4A8 的 1×K256×N64 与 1×K128×N128；W4A16 的 1×K128×N64 与 1×K64×N128</text>
<text x="262.0" y="717.0" font-size="8.5" fill="#475569">vlane 机制：把 MAC 按 vlane 分组，在 CSA 加法树的第 128 输入层级节点插旁路 MUX，配上对应 vlane 分组的</text>
<text x="262.0" y="730.5" font-size="8.5" fill="#475569">　MUX 逻辑和 Ksplit_acc 寄存器，做到单 lane 同时输出多个结果；vlane 有 1 和 2 两种模式</text>
<text x="262.0" y="744.0" font-size="8.5" fill="#475569">数据类型：token(A) / weight(B) 输入 BF16 或 MXFP8；W_ep 输入 FP32；输出 FP32 或 BF16</text>
<text x="262.0" y="757.5" font-size="8.5" fill="#475569">bit 级累加顺序：CSA 树按 scale block 分组累加，参考实现必须用同一顺序</text>
<text x="262.0" y="771.0" font-size="8.5" fill="#475569">计算异常 MATH_NAN_INF 不阻塞流水，由硬件自动 Clamp；零输入旁路 + 特殊值穿透；DIDT 分级启动，最小分级为单 lane</text>
<rect x="1300" y="584.5" width="300" height="221.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1312" y="605.5" font-size="11" fill="#111827" font-weight="600">stq</text>
<text x="1312.0" y="622.5" font-size="8.5" fill="#475569">队列深度 16</text>
<text x="1312.0" y="636.0" font-size="8.5" fill="#475569">Core Mem 写带宽 256 B（接口 1 KB）</text>
<text x="1312.0" y="649.5" font-size="8.5" fill="#475569">　不足 1 KB 按实际传输并标记 mask</text>
<text x="1312.0" y="663.0" font-size="8.5" fill="#475569">Core Mem 写延迟 16（待定）</text>
<text x="1312.0" y="676.5" font-size="8.5" fill="#475569">Wr concat buffer 1～2 KB：</text>
<text x="1312.0" y="690.0" font-size="8.5" fill="#475569">　各 lane buffer 深度不同，取决于物理距离，</text>
<text x="1312.0" y="703.5" font-size="8.5" fill="#475569">　最远 16 拍、最近 1 拍，越近 buffer 越大，</text>
<text x="1312.0" y="717.0" font-size="8.5" fill="#475569">　最大深度 16</text>
<text x="1312.0" y="730.5" font-size="8.5" fill="#475569">Store concat 按 vlane 分两种拼装：</text>
<text x="1312.0" y="744.0" font-size="8.5" fill="#475569">　vlane=1 步进横切，所有 lane buffer 并行 128 B</text>
<text x="1312.0" y="757.5" font-size="8.5" fill="#475569">　　截面，连续取 8 次攒满 1024 B（8T）</text>
<text x="1312.0" y="771.0" font-size="8.5" fill="#475569">　vlane=2 纵向整块，每 lane 一次取 8 B、共 256 B</text>
<text x="1312.0" y="784.5" font-size="8.5" fill="#475569">　　截面，连续取 4 次攒满 1024 B（4T）</text>
<polygon points="894,846.0 1005,846.0 996,876.0 885,876.0" fill="#f8fafc" stroke="#374151"/>
<text x="945.0" y="864.5" font-size="9" fill="#374151" text-anchor="middle">cmem_rd</text>
<polygon points="1569,384.25 1680,384.25 1671,416.25 1560,416.25" fill="#f8fafc" stroke="#374151"/>
<text x="1620.0" y="399.2" font-size="9" fill="#374151" text-anchor="middle">mmem_rd</text>
<text x="1620.0" y="410.2" font-size="7.5" fill="#6b7280" text-anchor="middle">Matrix Mem 在下方</text>
<polygon points="1399,846.0 1510,846.0 1501,876.0 1390,876.0" fill="#f8fafc" stroke="#374151"/>
<text x="1450.0" y="864.5" font-size="9" fill="#374151" text-anchor="middle">cmem_wr</text>
<path d="M375.6 75.0 L379.9 109.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M992.5 74.0 L996.9 109.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<rect x="1017.0" y="84.5" width="119.9" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1077" y="92" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#7c3aed" text-anchor="middle">local_ep_table 与静态配置</text>
<path d="M510.0 180.2 L555.0 186.9" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="512.8" y="173.5" width="39.3" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="532.5052976910911" y="181.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">trigger</text>
<path d="M786.0 187.0 L831.0 187.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="788.5" y="173.5" width="40.0" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="808.5" y="181.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">任务信息</text>
<path d="M671.0 110.0 L666.6 75.0" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#g)"/>
<rect x="748.3" y="84.5" width="165.5" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="831" y="92" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#0f766e" text-anchor="middle">finish 与 stq 写回完成合成 dsa_done</text>
<path d="M931.0 264.0 L931.0 300.0 L420.0 300.0 L420.0 329.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="607.2" y="288.5" width="85.6" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="650" y="296" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">EP 信息 / 地址参数</text>
<path d="M590.0 427.2 L635.1 407.4" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="586.8" y="393.5" width="51.6" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="612.5423791639885" y="401.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">token 地址</text>
<path d="M522.0 330.0 L522.0 316.0 L1007.0 316.0 L1007.0 329.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="771.8" y="304.5" width="56.3" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="800" y="312" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">weight 地址</text>
<path d="M776.0 484.0 L776.0 583.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="778.8" y="519.4" width="10.5" height="29.8" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 784 534.25)" x="784" y="537.2" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">token</text>
<path d="M992.0 470.5 L992.0 560.5 L909.6 560.5 L909.6 583.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="992.7" y="549.0" width="34.6" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1010" y="556.5" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">weight</text>
<path d="M930.0 688.5 L1299.0 695.2" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1088.7" y="681.8" width="51.6" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1114.500083183218" y="689.25" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">结果 → stq</text>
<path d="M917.0 437.8 L945.0 437.8 L949.5 845.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="947.8" y="614.8" width="10.5" height="174.3" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 953 701.9)" x="953" y="704.9" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">Token ldq ↔ Core Mem，132 B/T（11T）</text>
<path d="M1564.5 400.2 L1263.0 400.2" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1386.1" y="386.8" width="55.3" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1413.75" y="394.25" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">读带宽 8 KB</text>
<path d="M1450.0 806.0 L1454.4 845.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<text x="1462" y="829.0" font-size="8.5" fill="#6b7280" text-anchor="start">Wr concat 后写回 Core Mem</text>
<text x="20" y="914" font-size="10.5" fill="#374151" text-anchor="start">异常统一走 Drain &amp; Trap 四步，覆盖 Load / Store misalign、access fault、bus·ecc error 三类；本轮只留状态位与接口名。trigger 里的 last 标志决定这一笔要不要报 TS。</text>
</svg>
```

***

## 2　功能清单

一功能一条，编号供“机制覆盖”一章引用。

### config regfile

| 编号 | 功能 |
| - | - |
| F1 | 寄存器分静态配置与动态配置：静态配置基本不随用户变化，初始化阶段配好、业务流阶段快速调用；动态配置随用户变化，跟随任务下发，含静态配置的选择 |
| F2 | 任务启动写两条 `dsawi`：先 `topk_stream_stride`，最后 `trigger`；trigger 寄存器含 last 标志 |
| F3 | `streamID` / `taskID` / `userID` 由**软件写进动态配置寄存器**，不来自硬件通路：MU RV core 从自定义 CSR 读出 TS 下发的这三个值，在写 trigger 之前用配置指令写给 MU。`dsa_done` 回给 TS 的 `stream_id` 与 `task_id` 就是寄存器里的这一组 |
| F4 | 寄存器地址映射本轮用临时映射（`regmap.h`）。原来等的《MU/DTE 寄存器配置参数》已改名为《DTE 寄存器配置参数》，只剩 DTE 那一半（地址空间三段加寄存器模板，见《DTE 数据搬运引擎》），**MU 侧的寄存器地址映射仍无着落** |

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
| F12 | `topK_ep_table` 是 MU 自己从 Core Mem 载入的一份副本：DTE 进核时把 topK 写进 Core Mem 的 topK 区，MU 在 task 启动时经 `cmem_rd` 按 `topk_base + stream_id × 256 B` 读进来。载入未完成时 gen_ep_info 等着，不会读到半新半旧的一组专家 |
| F13 | token 数据与 topK 信息分开存放：FC1 / FC3 只需 topK ids，FC2 需 ids 与 weights |
| F14 | `router_expert_count = 0` 时忽略 topK 相关寄存器 |

### agu ×3 与 acu

| 编号 | 功能 |
| - | - |
| F15 | 三个 agu 分别算 token、weight、结果的访存地址 |
| F16 | 任务拆分的循环顺序由内往外是 tile_K、专家、tile_N。两级累加寄存器都只存一列，所以一列的几段与这一列的几个专家要连着算完 |
| F17 | acu 检查地址越界与对齐 |
| F18 | 异常时向阵列发排空指令（Drain），四步：阻塞任务下发 → 清理已发出的访存请求（已请求的回复照常处理，不再发起新的）→ 排空计算流水线 → 恢复默认状态 |
| F19 | Drain 期间允许已进入脉动通路的合法数据正常算完并写回，仅丢弃越界任务数据，防止状态机死锁 |
| F20 | 走 Drain & Trap 的异常三类：Load / Store misalign、Load / Store access fault、Load / Store bus·ecc error。本轮只留状态位与接口名 |

### ldq ×2

| 编号 | 功能 |
| - | - |
| F21 | Token ldq 队列深度 16，取决于读延时；Core Mem 读带宽 256 B（接口 512 B / 1 KB），与 MAC 阵列接口 256 B；读延迟 16（待定） |
| F22 | Rd outstanding buffer 16 × 256 B = 4 KB，用来掩盖 latency |
| F23 | Weight ldq 队列深度 4；Matrix Mem 读带宽 8 KB，bank 与 lane 一对一垂直贴合、无 crossbar；读延迟 4T（读 sram 2T + 打拍 2T） |
| F24 | Weight 各 lane 访存地址相同，只需发一个地址然后逐级脉动到各 lane |
| F24a | 三组地址的专家偏移：权重按专家在本 EP Group 内的序号隔开（`B_expert_stride`），激活与结果按 topK 里的先后隔开（`AC_expert_stride`）。使能专家间 reduce 时偏移算在激活这一侧（每个专家一份输入、合并成一份输出），不使能时算在结果这一侧（几个专家共用一份输入、各出一份） |
| F25 | MAC 入口用乒乓 2 级缓存掩盖 Mmem 读出延迟 |
| F26 | vlane 机制对 Load token 的影响：从 buffer 只读取 `256 B / vlane_num` 字节，再 copy 扩展到 256 B 输出 |

### matrix exe

| 编号 | 功能 |
| - | - |
| F27 | 32 个物理 Lane，左右镜像各 16 lane；单 Lane 内 10 级混合高频流水 |
| F28 | 物理阵列规格二选一：`1×K256×N32`（输出带宽 128 B）或 `1×K128×N64`（输出带宽 256 B） |
| F29 | 算力按精度组合分六档：BF16×BF16 4K MACs；MXFP8×MXFP8 8K MACs（scale block 32，E8M0）；MXFP8×MXFP4 与 MXFP8×NVFP4 各 16K MACs（scale block 16，FP8）；BF16×MXFP4 与 BF16×NVFP4 各 8K MACs |
| F30 | 八种计算原语：MXFP8 的 `1×K128×N64` 与 `1×K64×N128`；BF16 的 `1×K64×N64` 与 `1×K32×N128`；W4A8 的 `1×K256×N64` 与 `1×K128×N128`；W4A16 的 `1×K128×N64` 与 `1×K64×N128` |
| F31 | 数据类型：token(A) 与 weight(B) 输入 BF16 或 MXFP8，`W_ep` 输入 FP32，输出 FP32 或 BF16 |
| F32 | vlane 机制：把 MAC 按 vlane 分组，在 CSA 加法树的第 128 输入层级节点插旁路 MUX，配上对应 vlane 分组的 MUX 逻辑和 `Ksplit_acc` 寄存器，做到单 lane 同时输出多个结果。vlane 有 1 和 2 两种模式 |
| F33 | bit 级累加顺序：CSA 树按 scale block 分组累加，参考实现必须用同一顺序 |
| F33a | 两级累加寄存器，都是每 lane 一组，都只存一列那么宽：`kblock_acc` 收一列切出来的几段部分和，`ep_acc` 收这一列几个专家各乘上 `W_ep` 之后的加权和。段间与专家间都是顺序相加，每加一次 Clamp 一次；参考实现按同一个顺序算 |
| F33b | 走完一列才产出结果：一列的几段与这一列的几个专家都算完，才按 `DTYPE_C` 转成 FP32 或 BF16 交给 stq |
| F34 | 计算异常 `MATH_NAN_INF` 不走 Drain & Trap，不阻塞流水，由硬件自动 Clamp |
| F35 | 单 lane MAC 阵列 bitmask 计算，零输入旁路加特殊值（NaN / Inf）穿透 |
| F36 | DIDT 分级启动，分级模式可配置，最小分级为单 lane 启动；Matrix 与 Vector 错峰启动，防止二者功耗陡升叠加。本轮只留状态位与接口名 |
| F37 | 性能目标：MXFP8 下 Primitive K256×N32 和 K128×N64 阵列利用率 100%，BF16 与 MXFP4 同样 100%；Primitive `1×64×128` 且 K=64 时有 50% 性能损失；Tile K×N 过小会有性能损失，由 RV core 配置延时和 MU 内启动延时决定 |

### stq

| 编号 | 功能 |
| - | - |
| F38 | 队列深度 16；Core Mem 写带宽 256 B（接口 1 KB），不足 1 KB 按实际传输并标记 mask；写延迟 16（待定） |
| F39 | Wr concat buffer 1～2 KB：各 lane buffer 深度不同，取决于物理距离，最远 16 拍、最近 1 拍，越近 buffer 越大，最大深度 16 |
| F40 | Store concat 按 vlane 分两种拼装方式：`vlane=1` 步进横切，所有 lane buffer 并行 128 B 截面，连续取 8 次攒满 1024 B（8T）；`vlane=2` 纵向整块，每 lane 一次取 8 B、共 256 B 截面，连续取 4 次攒满 1024 B（4T） |
| F41 | 结果写回 Core Mem 后与 issue_q 的 finish 合成 `dsa_done`；trigger 里的 last 标志决定这一笔要不要报 TS |
| F41a | `SYS_STATUS`（Offset `0x004`）的 `BUSY` 位给软件轮询：写 `SYS_CTRL` 的 `TASK_START` 起置位，到 F41 那一刻清，也就是结果写回 Core Mem 之后。一个 task 里连发几笔 MU 任务时，软件靠它等前一笔做完再配下一笔。位域表在原始文档的表格附件里，本地库没同步到，`BUSY` 本轮取 bit0（**待确认**） |

***

## 3　接口

```
port dsa_cfg (slave, valid/ready, clk)            // MU RV core 的 dsa_iss
  in  req_valid · req_we · req_addr[11:0] · req_wdata[31:0]
  out req_ready                                     // = regfile 配置通路未反压
port dsa_rdata (master, 脉冲, clk)                // 读寄存器的异步返回
  out valid · rdata[31:0]
port dsa_done (master, 脉冲, clk)                 // → TS：trigger 的 last 标志置位的那一笔完成时报
  out valid · stream_id[3:0] · task_id[5:0]       // 取自软件写入的动态配置寄存器
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
mem local_ep_table  FF 阵列   当前 EP Group 内的专家与组内序号                      1R    编译侧算好，boot 期经 ctrl_noc 写入  复位由输入给
mem topK_ep_table   FF 阵列   16 stream × 256 B，每项 {expert_id 2 B, weight 4 B}   1R1W  task 启动时经 cmem_rd 从 Core Mem 载入  复位空
mem token_ldq       FIFO      16 × {addr[17:0], tag}                                1W1R  取决于读延时           复位空
mem rd_outstanding  FF 阵列   16 × 256 B = 4 KB                                     1RW   掩盖 latency          复位空
mem weight_ldq      FIFO      4 × {addr[24:0]}                                      1W1R  各 lane 地址相同       复位空
mem mac_ping_pong   FF 阵列   每 lane 2 级缓存                                      1RW   掩盖 Mmem 读出延迟     复位空
mem ksplit_acc      FF 阵列   每 lane 一组，vlane 分组的部分和                       1RW   vlane=2 时使用         复位 0
mem kblock_acc      FF 阵列   每 lane 一组 FP32，一列切出来的几段的部分和            1RW   kblock > 1 时使用      复位 0
mem ep_acc          FF 阵列   每 lane 一组 FP32，一列几个专家的加权和                1RW   专家间 reduce 时使用   复位 0
mem stq             FIFO      16 × {addr[17:0], data}                               1W1R  —                     复位空
mem wr_concat       FF 阵列   各 lane 1～2 KB，深度 1～16 不等（越近越大）           1RW   按 vlane 两种拼装      复位空
mem 级间 latch       级间 latch 单 lane 内 10 级                                     —     每拍覆写              —
```

***

## 5　流水线总览

load、计算、写回三段在相邻 task 之间重叠，第 1 层图按 t 标尺把三段对齐。四级配置段（M1 到 M4）合起来就是内部启动延迟 40 T 里的“20 条指令算地址 30 T 加流水启动 5 T”那一部分，另 5 T 是 MU RV core 的发射。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 844 522" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="aruov" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="844" height="522" fill="#ffffff"/>

  <text x="20" y="26" font-size="12" fill="#111827">MU · 第 1 层流水线总览（load、计算、写回三段在相邻 task 之间重叠）</text>
  <text x="20" y="42" font-size="9.5" fill="#6b7280">横向是级序，不是拍序；每级的拍数在右上角 Dx。橙色虚线框是变长级，非按比例。</text>
  <path d="M150 52 L150 70" stroke="#e5e7eb" fill="none"/>
<path d="M150 126 L150 414" stroke="#e5e7eb" fill="none"/>
  <path d="M316 52 L316 70" stroke="#e5e7eb" fill="none"/>
<path d="M316 126 L316 414" stroke="#e5e7eb" fill="none"/>
  <path d="M482 52 L482 70" stroke="#e5e7eb" fill="none"/>
<path d="M482 126 L482 414" stroke="#e5e7eb" fill="none"/>
  <path d="M648 52 L648 70" stroke="#e5e7eb" fill="none"/>
<path d="M648 126 L648 414" stroke="#e5e7eb" fill="none"/>
  <text x="20" y="102" font-size="10.5" fill="#6b7280">配置</text>
  <rect x="150" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="84" font-size="8.5" fill="#6b7280">M1</text>
  <text x="292" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="104" font-size="11" fill="#111827">regfile 写</text>
  <text x="160" y="118" font-size="11" fill="#111827">与 trigger</text>
  <rect x="316" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="84" font-size="8.5" fill="#6b7280">M2</text>
  <text x="458" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="104" font-size="11" fill="#111827">issue_q 出队</text>
  <path d="M300 98 L315 98" stroke="#475569" marker-end="url(#aruov)" fill="none"/>
  <rect x="482" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="492" y="84" font-size="8.5" fill="#6b7280">M3</text>
  <text x="624" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="492" y="104" font-size="11" fill="#111827">gen_ep_info</text>
  <path d="M466 98 L481 98" stroke="#475569" marker-end="url(#aruov)" fill="none"/>
  <rect x="648" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="658" y="84" font-size="8.5" fill="#6b7280">M4</text>
  <text x="790" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="658" y="104" font-size="11" fill="#111827">agu 算三组地址</text>
  <path d="M632 98 L647 98" stroke="#475569" marker-end="url(#aruov)" fill="none"/>
  <text x="20" y="188" font-size="10.5" fill="#6b7280">load</text>
  <rect x="150" y="156" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="160" y="170" font-size="8.5" fill="#92400e">M5</text>
  <text x="292" y="170" font-size="8.5" fill="#92400e" text-anchor="end">D11</text>
  <text x="160" y="190" font-size="11" fill="#7c2d12">Token ldq</text>
  <text x="160" y="204" font-size="11" fill="#7c2d12">读 Core Mem</text>
  <rect x="316" y="156" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="326" y="170" font-size="8.5" fill="#92400e">M6</text>
  <text x="458" y="170" font-size="8.5" fill="#92400e" text-anchor="end">D8</text>
  <text x="326" y="190" font-size="11" fill="#7c2d12">Weight ldq</text>
  <text x="326" y="204" font-size="11" fill="#7c2d12">读 Matrix Mem</text>
  <path d="M300 184 L315 184" stroke="#475569" marker-end="url(#aruov)" fill="none"/>
  <text x="20" y="274" font-size="10.5" fill="#6b7280">计算</text>
  <rect x="150" y="242" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="160" y="256" font-size="8.5" fill="#92400e">M7</text>
  <text x="292" y="256" font-size="8.5" fill="#92400e" text-anchor="end">D10</text>
  <text x="160" y="276" font-size="11" fill="#7c2d12">matrix exe</text>
  <text x="160" y="290" font-size="11" fill="#7c2d12">单 lane 10 级</text>
  <text x="20" y="360" font-size="10.5" fill="#6b7280">写回</text>
  <rect x="150" y="328" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="160" y="342" font-size="8.5" fill="#92400e">M8</text>
  <text x="292" y="342" font-size="8.5" fill="#92400e" text-anchor="end">D8 / D4</text>
  <text x="160" y="362" font-size="11" fill="#7c2d12">stq concat</text>
  <rect x="316" y="328" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="326" y="342" font-size="8.5" fill="#92400e">M9</text>
  <text x="458" y="342" font-size="8.5" fill="#92400e" text-anchor="end">D11</text>
  <text x="326" y="362" font-size="11" fill="#7c2d12">写 Core Mem</text>
  <text x="326" y="376" font-size="11" fill="#7c2d12">与 dsa_done</text>
  <path d="M300 356 L315 356" stroke="#475569" marker-end="url(#aruov)" fill="none"/>
  <text x="20" y="438" font-size="10.5" fill="#374151">三段重叠：task0 计算与 task1 load 同拍，task0 写回与 task1 计算同拍，任务切换无 bubble。</text>
  <text x="20" y="466" font-size="10.5" fill="#374151">M5 与 M9 的 11 拍取 Cmem MAS 的 MU 侧延迟；MU MAS 记读写各 16 拍，多出的是 ldq 与 stq 的排队，本轮按 11 拍加队列建（待定）。</text>
  <text x="20" y="494" font-size="10.5" fill="#374151">M7 的 10 级取 matrix exe 一节的单 lane 深度；参数表另记流水延时 9 T，差的一拍是脉动输出那一级，以 10 级为准（待定）。</text>
</svg>
```

***

## 6　逐级行为

### M1 · regfile 写与 trigger

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 938 244" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="aru1" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="938" height="244" fill="#ffffff"/>

  <polygon points="30,20 188,20 178,96 20,96" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="39" font-size="10.5" fill="#374151" text-anchor="middle">dsa_cfg</text>
  <text x="104" y="57" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · req_we</text>
  <text x="104" y="75" font-size="9.5" fill="#6b7280" text-anchor="middle">req_addr[11:0]</text>
  <text x="104" y="93" font-size="9.5" fill="#6b7280" text-anchor="middle">req_wdata[31:0] · req_ready</text>
  <rect x="20" y="118" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="122" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="139" font-size="10" fill="#374151" text-anchor="middle">regfile · FF · 1R1W</text>
  <rect x="742" y="101" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="746" y="105" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="830" y="122" font-size="10" fill="#374151" text-anchor="middle">issue_q · FIFO 16 项 · 1W</text>
  <rect x="232" y="43" width="466" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="59" font-size="8.5" fill="#6b7280">M1</text>
  <text x="684" y="59" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="79" font-size="12" fill="#111827">regfile · 静态选组加动态参数</text>
  <text x="250" y="101" font-size="10.5" fill="#475569">1. dsa_cfg.req_we → regfile[req_addr] = req_wdata</text>
  <text x="250" y="121" font-size="10.5" fill="#475569">2. req_ready = 配置通路未反压</text>
  <text x="250" y="141" font-size="10.5" fill="#475569">3. 写 trigger 寄存器 → 锁存当前动态参数为一个任务描述</text>
  <text x="250" y="161" font-size="10.5" fill="#475569">4. desc.last = trigger.last；desc.{stream_id, task_id, user_id} = 软件写入的寄存器值</text>
  <text x="250" y="185" font-size="10" fill="#9ca3af">启动用两条 dsawi，最后写 trigger</text>
  <path d="M188 58 L231 58" stroke="#475569" marker-end="url(#aru1)" fill="none"/>
  <path d="M188 139 L231 139" stroke="#475569" marker-end="url(#aru1)" fill="none"/>
  <path d="M698 122 L741 122" stroke="#475569" marker-end="url(#aru1)" fill="none"/>
</svg>
```

### M2 · issue_q 出队

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 819 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="aru2" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="819" height="198" fill="#ffffff"/>

  <rect x="20" y="78" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="82" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="99" font-size="10" fill="#374151" text-anchor="middle">issue_q · FIFO 16 项 · 1R</text>
  <rect x="623" y="42" width="176" height="114" fill="#f1f5f9" stroke="#334155"/>
  <rect x="623" y="42" width="176" height="18" fill="#334155"/>
  <text x="711" y="55" font-size="10.5" fill="#ffffff" text-anchor="middle">MU_TASK</text>
  <text x="711" y="82" font-size="10" fill="#334155" text-anchor="middle">stream_id[3:0]</text>
  <text x="711" y="104" font-size="10" fill="#334155" text-anchor="middle">task_id[5:0]</text>
  <text x="711" y="126" font-size="10" fill="#334155" text-anchor="middle">tile_k · tile_n</text>
  <text x="711" y="148" font-size="10" fill="#334155" text-anchor="middle">last</text>
  <rect x="232" y="20" width="347" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M2</text>
  <text x="565" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">issue_q · 顺序执行顺序 finish</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. 前一任务的 load 段已让出 → cur = issue_q.pop()</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. 顺序执行：队列内不重排</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. 任务切换不插泡：本拍出队，下拍即进 M3</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. issue_q 满 → dsa_cfg.req_ready = 0，反压 MU RV core</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">深度 16，与启动延时匹配</text>
  <path d="M188 99 L231 99" stroke="#475569" marker-end="url(#aru2)" fill="none"/>
  <path d="M579 99 L622 99" stroke="#475569" marker-end="url(#aru2)" fill="none"/>
</svg>
```

### M3 · gen_ep_info

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 845 218" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="aru3" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="845" height="218" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">MU_TASK</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">stream_id[3:0]</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">task_id[5:0]</text>
  <rect x="20" y="102" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="106" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="123" font-size="10" fill="#374151" text-anchor="middle">topK_ep_table · FF 16×256 B · 1R</text>
  <rect x="20" y="156" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="160" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="177" font-size="10" fill="#374151" text-anchor="middle">local_ep_table · FF · 1R</text>
  <rect x="649" y="63" width="176" height="92" fill="#f1f5f9" stroke="#334155"/>
  <rect x="649" y="63" width="176" height="18" fill="#334155"/>
  <text x="737" y="76" font-size="10.5" fill="#ffffff" text-anchor="middle">EP_INFO</text>
  <text x="737" y="103" font-size="10" fill="#334155" text-anchor="middle">local_idx[7:0][*]</text>
  <text x="737" y="125" font-size="10" fill="#334155" text-anchor="middle">w_ep[*] FP32</text>
  <text x="737" y="147" font-size="10" fill="#334155" text-anchor="middle">ep_num[3:0]</text>
  <rect x="232" y="30" width="373" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="46" font-size="8.5" fill="#6b7280">M3</text>
  <text x="591" y="46" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="66" font-size="12" fill="#111827">gen_ep_info · 全局专家号转组内序号</text>
  <text x="250" y="88" font-size="10.5" fill="#475569">1. router_expert_count == 0 → 跳过本级，忽略 topK 相关寄存器</text>
  <text x="250" y="108" font-size="10.5" fill="#475569">2. ids = topK_ep_table[stream_id] 的 expert_id 列表</text>
  <text x="250" y="128" font-size="10.5" fill="#475569">3. local_idx[j] = local_ep_table[ids[j]]</text>
  <text x="250" y="148" font-size="10.5" fill="#475569">4. FC2 另取 weight 字段；FC1 与 FC3 只取 ids</text>
  <text x="250" y="172" font-size="10" fill="#9ca3af">token 数据与 topK 分开存放</text>
  <path d="M188 55 L231 55" stroke="#475569" marker-end="url(#aru3)" fill="none"/>
  <path d="M188 123 L231 123" stroke="#475569" marker-end="url(#aru3)" fill="none"/>
  <path d="M188 177 L231 177" stroke="#475569" marker-end="url(#aru3)" fill="none"/>
  <path d="M605 109 L648 109" stroke="#475569" marker-end="url(#aru3)" fill="none"/>
</svg>
```

### M4 · agu 算三组地址

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 893 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="aru4" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="893" height="198" fill="#ffffff"/>

  <rect x="20" y="37" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="37" width="168" height="18" fill="#334155"/>
  <text x="104" y="50" font-size="10.5" fill="#ffffff" text-anchor="middle">EP_INFO</text>
  <text x="104" y="77" font-size="10" fill="#334155" text-anchor="middle">local_idx[7:0][*]</text>
  <text x="104" y="99" font-size="10" fill="#334155" text-anchor="middle">ep_num[3:0]</text>
  <rect x="20" y="119" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="123" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="140" font-size="10" fill="#374151" text-anchor="middle">regfile · FF · 1R</text>
  <rect x="697" y="51" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="701" y="55" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="785" y="72" font-size="10" fill="#374151" text-anchor="middle">token_ldq · FIFO 16 项 · 1W</text>
  <rect x="697" y="105" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="701" y="109" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="785" y="126" font-size="10" fill="#374151" text-anchor="middle">weight_ldq · FIFO 4 项 · 1W</text>
  <rect x="232" y="20" width="421" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M4</text>
  <text x="639" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">agu ×3 与 acu · 算地址并查越界</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. token_addr = cmem_base + stream_id × stream_stride + k_off</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. weight_addr = mmem_base + local_idx × 专家步长 + tile 偏移</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. result_addr = cmem_out_base + stream_id × stream_stride + n_off</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. acu：越界或不对齐 → 触发 Drain，阻塞任务下发</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">拆分顺序：先循环 tile_K，再循环 tile_N</text>
  <path d="M188 72 L231 72" stroke="#475569" marker-end="url(#aru4)" fill="none"/>
  <path d="M188 140 L231 140" stroke="#475569" marker-end="url(#aru4)" fill="none"/>
  <path d="M653 72 L696 72" stroke="#475569" marker-end="url(#aru4)" fill="none"/>
  <path d="M653 126 L696 126" stroke="#475569" marker-end="url(#aru4)" fill="none"/>
</svg>
```

### M5 · Token ldq 读 Core Mem

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 884 200" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="aru5" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="884" height="200" fill="#ffffff"/>

  <rect x="20" y="52" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="56" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="73" font-size="10" fill="#374151" text-anchor="middle">token_ldq · FIFO 16 项 · 1R</text>
  <rect x="20" y="106" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="110" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="127" font-size="10" fill="#374151" text-anchor="middle">rd_outstanding · FF 16×256 B · 1RW</text>
  <polygon points="698,20 864,20 854,96 688,96" fill="#f8fafc" stroke="#374151"/>
  <text x="776" y="39" font-size="10.5" fill="#374151" text-anchor="middle">cmem_rd</text>
  <text x="776" y="57" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · req_addr[17:0]</text>
  <text x="776" y="75" font-size="9.5" fill="#6b7280" text-anchor="middle">rsp_valid · rsp_rdata[1023:0]</text>
  <text x="776" y="93" font-size="9.5" fill="#6b7280" text-anchor="middle">rsp_scale[31:0]</text>
  <rect x="688" y="108" width="176" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="688" y="108" width="176" height="18" fill="#334155"/>
  <text x="776" y="121" font-size="10.5" fill="#ffffff" text-anchor="middle">A_IN</text>
  <text x="776" y="148" font-size="10" fill="#334155" text-anchor="middle">a[1023:0]</text>
  <text x="776" y="170" font-size="10" fill="#334155" text-anchor="middle">sf_a[31:0]</text>
  <rect x="232" y="21" width="412" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="37" font-size="8.5" fill="#6b7280">M5</text>
  <text x="630" y="37" font-size="8.5" fill="#6b7280" text-anchor="end">D11</text>
  <text x="250" y="57" font-size="12" fill="#111827">Token ldq · 132 B 一拍取回</text>
  <text x="250" y="79" font-size="10.5" fill="#475569">1. cmem_rd.req = {addr=token_addr, scale_en=MXFP8}</text>
  <text x="250" y="99" font-size="10.5" fill="#475569">2. rd_outstanding 满 → 停发，等回数</text>
  <text x="250" y="119" font-size="10.5" fill="#475569">3. rsp_valid → rd_outstanding 写入 {rdata 128 B, scale 4 B}</text>
  <text x="250" y="139" font-size="10.5" fill="#475569">4. vlane_num &gt; 1 → 只取 256 B / vlane_num 字节再 copy 扩展到 256 B</text>
  <text x="250" y="163" font-size="10" fill="#9ca3af">11 拍是 Cmem 侧的 MU 读延迟</text>
  <path d="M188 73 L231 73" stroke="#475569" marker-end="url(#aru5)" fill="none"/>
  <path d="M188 127 L231 127" stroke="#475569" marker-end="url(#aru5)" fill="none"/>
  <path d="M644 58 L692 58" stroke="#475569" marker-end="url(#aru5)" fill="none"/>
  <path d="M644 143 L687 143" stroke="#475569" marker-end="url(#aru5)" fill="none"/>
</svg>
```

### M6 · Weight ldq 读 Matrix Mem

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 853 200" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="aru6" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="853" height="200" fill="#ffffff"/>

  <rect x="20" y="52" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="56" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="73" font-size="10" fill="#374151" text-anchor="middle">weight_ldq · FIFO 4 项 · 1R</text>
  <rect x="20" y="106" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="110" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="127" font-size="10" fill="#374151" text-anchor="middle">mac_ping_pong · FF 每 lane 2 级 · 1RW</text>
  <polygon points="667,20 833,20 823,96 657,96" fill="#f8fafc" stroke="#374151"/>
  <text x="745" y="39" font-size="10.5" fill="#374151" text-anchor="middle">mmem_rd</text>
  <text x="745" y="57" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · req_addr[24:0]</text>
  <text x="745" y="75" font-size="9.5" fill="#6b7280" text-anchor="middle">rsp_valid · rsp_rdata[65535:0]</text>
  <text x="745" y="93" font-size="9.5" fill="#6b7280" text-anchor="middle">rsp_scale[8191:0]</text>
  <rect x="657" y="108" width="176" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="657" y="108" width="176" height="18" fill="#334155"/>
  <text x="745" y="121" font-size="10.5" fill="#ffffff" text-anchor="middle">B_IN</text>
  <text x="745" y="148" font-size="10" fill="#334155" text-anchor="middle">b[lane][*]</text>
  <text x="745" y="170" font-size="10" fill="#334155" text-anchor="middle">sf_b[lane]</text>
  <rect x="232" y="21" width="381" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="37" font-size="8.5" fill="#6b7280">M6</text>
  <text x="599" y="37" font-size="8.5" fill="#6b7280" text-anchor="end">D8</text>
  <text x="250" y="57" font-size="12" fill="#111827">Weight ldq · 一个地址脉动到各 lane</text>
  <text x="250" y="79" font-size="10.5" fill="#475569">1. mmem_rd.req = {addr=weight_addr}，各 lane 地址相同只发一个</text>
  <text x="250" y="99" font-size="10.5" fill="#475569">2. rsp_rdata 8 KB 加 rsp_scale 1 KB 按 lane 切开</text>
  <text x="250" y="119" font-size="10.5" fill="#475569">3. mac_ping_pong[lane] 交替写入，掩盖读出延迟</text>
  <text x="250" y="139" font-size="10.5" fill="#475569">4. bank 与 lane 一对一垂直贴合，无 crossbar</text>
  <text x="250" y="163" font-size="10" fill="#9ca3af">8 拍是 Mmem 的 lane 内延迟</text>
  <path d="M188 73 L231 73" stroke="#475569" marker-end="url(#aru6)" fill="none"/>
  <path d="M188 127 L231 127" stroke="#475569" marker-end="url(#aru6)" fill="none"/>
  <path d="M613 58 L661 58" stroke="#475569" marker-end="url(#aru6)" fill="none"/>
  <path d="M613 143 L656 143" stroke="#475569" marker-end="url(#aru6)" fill="none"/>
</svg>
```

### M7 · matrix exe

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 883 246" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="aru7" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="883" height="246" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">A_IN</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">a[1023:0]</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">sf_a[31:0]</text>
  <rect x="20" y="102" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="102" width="168" height="18" fill="#334155"/>
  <text x="104" y="115" font-size="10.5" fill="#ffffff" text-anchor="middle">B_IN</text>
  <text x="104" y="142" font-size="10" fill="#334155" text-anchor="middle">b[lane][*]</text>
  <text x="104" y="164" font-size="10" fill="#334155" text-anchor="middle">sf_b[lane]</text>
  <rect x="20" y="184" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="188" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="205" font-size="10" fill="#374151" text-anchor="middle">ksplit_acc · FF 每 lane 一组 · 1RW</text>
  <rect x="687" y="88" width="176" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="687" y="88" width="176" height="18" fill="#334155"/>
  <text x="775" y="101" font-size="10.5" fill="#ffffff" text-anchor="middle">D_OUT</text>
  <text x="775" y="128" font-size="10" fill="#334155" text-anchor="middle">d[lane] FP32/BF16</text>
  <text x="775" y="150" font-size="10" fill="#334155" text-anchor="middle">lane_id[5:0]</text>
  <rect x="232" y="44" width="411" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="60" font-size="8.5" fill="#6b7280">M7</text>
  <text x="629" y="60" font-size="8.5" fill="#6b7280" text-anchor="end">D10</text>
  <text x="250" y="80" font-size="12" fill="#111827">matrix exe · 32 lane 各 10 级 MAC</text>
  <text x="250" y="102" font-size="10.5" fill="#475569">1. p[lane] = CSA(a × b)，按 scale block 分组累加，组内顺序固定</text>
  <text x="250" y="122" font-size="10.5" fill="#475569">2. d = sf_a × sf_b × p + (第一个专家 ? 0 : c)，c 取自上一专家的结果</text>
  <text x="250" y="142" font-size="10.5" fill="#475569">3. vlane=2 → 在第 128 输入层级插旁路 MUX，部分和进 ksplit_acc</text>
  <text x="250" y="162" font-size="10.5" fill="#475569">4. NaN 或 Inf → 硬件 Clamp，不阻塞流水；零输入旁路，特殊值穿透</text>
  <text x="250" y="186" font-size="10" fill="#9ca3af">参考实现必须用同一套 CSA 累加顺序</text>
  <path d="M188 55 L231 55" stroke="#475569" marker-end="url(#aru7)" fill="none"/>
  <path d="M188 137 L231 137" stroke="#475569" marker-end="url(#aru7)" fill="none"/>
  <line x1="188" y1="205" x2="228" y2="205" stroke="#475569" marker-end="url(#aru7)"/>
  <path d="M643 123 L686 123" stroke="#475569" marker-end="url(#aru7)" fill="none"/>
</svg>
```

### M8 · stq concat

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 899 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="aru8" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="899" height="198" fill="#ffffff"/>

  <rect x="20" y="37" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="37" width="168" height="18" fill="#334155"/>
  <text x="104" y="50" font-size="10.5" fill="#ffffff" text-anchor="middle">D_OUT</text>
  <text x="104" y="77" font-size="10" fill="#334155" text-anchor="middle">d[lane] FP32/BF16</text>
  <text x="104" y="99" font-size="10" fill="#334155" text-anchor="middle">lane_id[5:0]</text>
  <rect x="20" y="119" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="123" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="140" font-size="10" fill="#374151" text-anchor="middle">wr_concat · FF 1～2 KB · 1RW</text>
  <rect x="703" y="78" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="707" y="82" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="791" y="99" font-size="10" fill="#374151" text-anchor="middle">stq · FIFO 16 项 · 1W</text>
  <rect x="232" y="20" width="427" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M8</text>
  <text x="645" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D8 / D4</text>
  <text x="250" y="56" font-size="12" fill="#111827">stq · 按 vlane 两种拼装攒满 1 KB</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. wr_concat[lane].push(d[lane])，各 lane buffer 深度按物理距离 1～16</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. vlane=1 → 步进横切，各 lane 并行 128 B 截面，连取 8 次攒满 1024 B</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. vlane=2 → 纵向整块，每 lane 一次取 8 B 共 256 B，连取 4 次攒满</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. 不足 1 KB 按实际长度传输并标记 mask</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">近的 lane 先到，buffer 反而要大</text>
  <path d="M188 72 L231 72" stroke="#475569" marker-end="url(#aru8)" fill="none"/>
  <path d="M188 140 L231 140" stroke="#475569" marker-end="url(#aru8)" fill="none"/>
  <path d="M659 99 L702 99" stroke="#475569" marker-end="url(#aru8)" fill="none"/>
</svg>
```

### M9 · 写 Core Mem 与 dsa_done

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 917 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="aru9" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="917" height="198" fill="#ffffff"/>

  <rect x="20" y="51" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="55" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="72" font-size="10" fill="#374151" text-anchor="middle">stq · FIFO 16 项 · 1R</text>
  <rect x="20" y="105" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="109" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="126" font-size="10" fill="#374151" text-anchor="middle">regfile · FF · 1R</text>
  <polygon points="731,24 897,24 887,100 721,100" fill="#f8fafc" stroke="#374151"/>
  <text x="809" y="43" font-size="10.5" fill="#374151" text-anchor="middle">cmem_wr</text>
  <text x="809" y="61" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · req_addr[17:0]</text>
  <text x="809" y="79" font-size="9.5" fill="#6b7280" text-anchor="middle">req_wdata[1023:0]</text>
  <text x="809" y="97" font-size="9.5" fill="#6b7280" text-anchor="middle">req_ready</text>
  <polygon points="731,112 897,112 887,170 721,170" fill="#f8fafc" stroke="#374151"/>
  <text x="809" y="131" font-size="10.5" fill="#374151" text-anchor="middle">dsa_done</text>
  <text x="809" y="149" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · stream_id[3:0]</text>
  <text x="809" y="167" font-size="9.5" fill="#6b7280" text-anchor="middle">task_id[5:0]</text>
  <rect x="232" y="20" width="445" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M9</text>
  <text x="663" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D11</text>
  <text x="250" y="56" font-size="12" fill="#111827">stq · 1 KB 分 8 拍写回并报完成</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. cmem_wr.req = {addr=result_addr, wdata 1024 b, scale 32 b, be}</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. 1 KB 按 8 拍发出，req_ready = 0 时保持</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. 写回完成 &amp;&amp; issue_q 的该任务 finish → done = 1</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. done &amp;&amp; desc.last → dsa_done = {stream_id, task_id}（取自 desc）</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">last 标志决定这一笔要不要报 TS</text>
  <path d="M188 72 L231 72" stroke="#475569" marker-end="url(#aru9)" fill="none"/>
  <path d="M188 125 L231 125" stroke="#475569" marker-end="url(#aru9)" fill="none"/>
  <path d="M677 62 L725 62" stroke="#475569" marker-end="url(#aru9)" fill="none"/>
  <path d="M677 141 L725 141" stroke="#475569" marker-end="url(#aru9)" fill="none"/>
</svg>
```

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
Core Mem 读 / 写    256 B（接口 512 B / 1 KB）；MU 侧 132 B/T，读写延迟均 16T
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
| streamID / taskID / userID 由软件写进动态配置寄存器 | F3 | `mu_ids_by_software` |
| issue_q 顺序执行，任务切换无 bubble | F6、F7 | `mu_issue_q` |
| task 间三段重叠 | F8 | `mu_three_stage_overlap` |
| topK 的 global index 经 local_ep_table 转 local index | F10、F11 | `gen_ep_info` |
| topK_ep_table 由 MU 自己从 Core Mem 载入 | F12 | `topk_load` |
| router_expert_count = 0 时忽略 topK 寄存器 | F14 | `no_topk` |
| 循环顺序由内往外是 tile_K、专家、tile_N | F16 | `tile_order` |
| 一列的几段攒在 kblock_acc，几个专家乘 W_ep 后攒在 ep_acc，走完一列才产出 | F33a、F33b | `expert_reduce` |
| 权重按组内序号隔开、激活与结果按 topK 先后隔开 | F24a | `expert_stride` |
| acu 越界 / 对齐检查触发 Drain 四步 | F17、F18 | `mu_drain_trap` |
| Drain 时已进入脉动通路的合法数据照常算完写回 | F19 | `drain_keep_valid` |
| Weight 各 lane 地址相同，发一个地址逐级脉动 | F24 | `weight_systolic_addr` |
| MAC 入口乒乓 2 级缓存掩盖 Mmem 读延迟 | F25 | `mac_pingpong` |
| vlane 影响 Load token 的读取与扩展 | F26 | `vlane_load` |
| 32 lane × 10 级，六档精度组合的算力 | F27、F29 | `mu_array_spec` |
| 八种计算原语 | F30 | `mu_primitives` |
| C = C + (A × B) × W_ep，不支持初始 C 加载 | 定位与边界 | `ep_reduce_in_mu` |
| vlane 的 CSA 旁路 MUX 与 Ksplit_acc | F32 | `vlane_split` |
| CSA 树按 scale block 分组累加，与参考实现同序 | F33 | `csa_order` |
| MATH_NAN_INF 不阻塞流水，硬件自动 Clamp | F34 | `nan_clamp` |
| 零输入旁路与特殊值穿透 | F35 | `zero_bypass` |
| stq 不足 1 KB 按实际传输并标记 mask | F38 | `stq_mask` |
| Wr concat buffer 按物理距离定深度 | F39 | `concat_buffer_depth` |
| Store concat 按 vlane 分两种拼装 | F40 | `vlane_store` |
| last 标志决定这一笔要不要报 TS | F41 | `mu_dsa_done` |
| BUSY 到结果写回 Core Mem 之后才清，软件轮询它等一笔做完 | F41a | `mu_busy_poll` |

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
