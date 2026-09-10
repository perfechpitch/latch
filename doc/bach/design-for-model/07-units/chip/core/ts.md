# TS 任务调度器

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **TS**

给实现 TS 的人：各模块做哪些事、端口与存储怎么定。模块划分照 TS MAS，分两层，顶层九个，`Stream_table` 内再展开七个。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《TS 任务调度器》全篇
* 《归约的完整过程》：“TS 与 DTE 侧的配合”
* 《软件栈》：“四类 core 的 TS 配置”“每个 task 的共同形状”

***

## 1　定位与边界

TS 是 core 的控制单元，一块上电配好就按固定逻辑跑的硬件，不是可编程的调度器。它的全部工作收在两张表和四个动作里。

* **两张表**：`task_chain` 是静态的，说清这类 core 的操作流长什么样；`stream_table` 是运行时的，说清每个在途用户走到了哪一步
* **四个动作**：新用户到了建表、判当前这一步的前置条件齐了没有、同一个执行单元有多个候选时选最老的、收到完成事件推进度并在链尾退休

边界是五组接口：与 Router 的五组信号、与三个 RV core 的 task 下发与 ack、六路 DSA 与 RV core 的完成（MAS 的 `completion_ack[5:0]` 六个物理端口，Router 的 Reduce Done 另走 `rmem2ts_done_ch`，合起来是七路）、ctrl_noc 的配置口、异常上报口。

四种工作模式由两个配置项选定，上电配好之后运行期间不变：

| 模式 | 怎么进入 | TS 在这个模式下做什么 |
| - | - | - |
| weights 加载 | `WEIGHTS_MODE = 1` | 不启动 task_chain。只有 datain_task 工作，Router 来了数据就派给 DTE 搬运，不建 stream 表项、不推进任何任务链 |
| 普通计算 core | `CORE_TYPE` = 普通，`WEIGHTS_MODE = 0` | 完整的四个动作：Router trigger 建表、按 task_chain 逐 task 推进、三条通路发射、完成后推进度并在链尾退休 |
| B core | `CORE_TYPE` = B core | 复位后直接自启动 16 个表项，不等 Router trigger。datain 收到的数据不建表，完成标志由软件维护在 Share Mem 里；task 0 循环查那个标志，查到才往下走 |
| R core | `CORE_TYPE` = R core | 同 B core 的自启动与双链结构，区别在 task 0 查的是“两笔数据是否集齐”，推进顺序由软件映射表决定，不是 TS 的年龄优先 |

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 2000 1130" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" role="img" aria-label="TS 第 0 层">
<title>TS 第 0 层</title>
<defs><marker id="a" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#475569"/></marker><marker id="as" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#475569"/></marker><marker id="g" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0f766e"/></marker><marker id="gs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0f766e"/></marker><marker id="o" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#b45309"/></marker><marker id="os" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#b45309"/></marker><marker id="p" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#7c3aed"/></marker><marker id="ps" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#7c3aed"/></marker><marker id="i" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#4338ca"/></marker><marker id="is" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#4338ca"/></marker><marker id="t" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0d9488"/></marker><marker id="ts" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0d9488"/></marker><marker id="r" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#be123c"/></marker><marker id="rs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#be123c"/></marker><marker id="b" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#2563eb"/></marker><marker id="bs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#2563eb"/></marker><marker id="m" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#d97706"/></marker><marker id="ms" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#d97706"/></marker><marker id="l" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#9aa1ad"/></marker><marker id="ls" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#9aa1ad"/></marker></defs>
<rect x="0" y="0" width="2000" height="1130" fill="#ffffff"/>
<text x="20" y="26" font-size="12" fill="#111827">TS 任务调度器 · 第 0 层（九个逐拍推进的模块。方位：TS 在 core 顶边，RV core 在下方；Router 的四条通路竖着穿过 core，本图画在右侧）</text>
<text x="849" y="26" font-size="9.5" fill="#6b7280">绿线 = 与 Router 的控制通路　灰线 = TS 内部与 RV core 的下发 / 完成　橙线 = credit 与 retire　紫虚线 = ctrl_noc 配置</text>
<rect x="160" y="110" width="450" height="181.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="172" y="131" font-size="11" fill="#111827" font-weight="600">Task_ctrl</text>
<text x="172.0" y="148.0" font-size="8.5" fill="#475569">当前任务进 TASK_FINISH 后才生成后继</text>
<text x="172.0" y="161.5" font-size="8.5" fill="#475569">每个 stream 独立推进，不需要全局 Task Pointer</text>
<text x="172.0" y="175.0" font-size="8.5" fill="#475569">从 head_ptr 环形扫描，只选 valid=1 且 FINISH 且 end=0</text>
<text x="172.0" y="188.5" font-size="8.5" fill="#475569">一次 64 bit 优先编码，一拍跳过所有可跳过的 task：</text>
<text x="172.0" y="202.0" font-size="8.5" fill="#475569">　SKIP_MASK = ~END_MASK &amp; ( (DATA_IN_MASK &amp; done_bitmap)</text>
<text x="172.0" y="215.5" font-size="8.5" fill="#475569">　　　　　　　　　　　| (REISSUE_MASK &amp; ~stream.reissue) )</text>
<text x="172.0" y="229.0" font-size="8.5" fill="#475569">连续 skip 不增加周期</text>
<text x="172.0" y="242.5" font-size="8.5" fill="#475569">End task 即使已提前完成也不能跳，且不再生成后继</text>
<text x="172.0" y="256.0" font-size="8.5" fill="#475569">新任务的 task_id · task_fsm · end 与全部下发属性一起原子写入</text>
<text x="172.0" y="269.5" font-size="8.5" fill="#475569">初始状态：Generated → READY，DataIn / Reissue → WAIT</text>
<rect x="650" y="110" width="300" height="167.5" rx="4" fill="#f5f3ff" stroke="#7c3aed"/>
<text x="662" y="131" font-size="11" fill="#111827" font-weight="600">CFG_REG</text>
<text x="662.0" y="148.0" font-size="8.5" fill="#475569">task_chain 64 项 × 64 bit（写一项自动置 VALID）</text>
<text x="662.0" y="161.5" font-size="8.5" fill="#475569">datain_task 1 项：task_pc · task_unit · weights_mode</text>
<text x="662.0" y="175.0" font-size="8.5" fill="#475569">stream_num 1～16 · CORE_TYPE · B_CORE_DIRECTION</text>
<text x="662.0" y="188.5" font-size="8.5" fill="#475569">trigger_task_chain_en · TS_INIT_FINISH · TS_STATE</text>
<text x="662.0" y="202.0" font-size="8.5" fill="#475569">写 TS_INIT_FINISH 后查五项合规性，结论写 TS_STATE</text>
<text x="662.0" y="215.5" font-size="8.5" fill="#475569">并派生三张 64 位掩码供 Task_ctrl 一拍算 SKIP_MASK：</text>
<text x="662.0" y="229.0" font-size="8.5" fill="#475569">　DATA_IN_MASK · REISSUE_MASK · END_MASK</text>
<text x="662.0" y="242.5" font-size="8.5" fill="#475569">Task LUT：按 task_id 查出下发属性</text>
<text x="662.0" y="256.0" font-size="8.5" fill="#475569">软件侧属性另存：exe_dest · reduce_num · dsa_en</text>
<rect x="1000" y="110" width="430" height="181.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1012" y="131" font-size="11" fill="#111827" font-weight="600">credit</text>
<text x="1012.0" y="148.0" font-size="8.5" fill="#475569">向 Router 注册资源申请：UserID · StreamID · TaskID · PathID</text>
<text x="1012.0" y="161.5" font-size="8.5" fill="#475569">收 Router 的 credit 到手通知，唤醒对应 task 置 READY</text>
<text x="1012.0" y="175.0" font-size="8.5" fill="#475569">credit 粒度是 stream 不是 task；同一 stream 的任务链里</text>
<text x="1012.0" y="188.5" font-size="8.5" fill="#475569">　只在第一次往下游发数据时检查，之后不再检查</text>
<text x="1012.0" y="202.0" font-size="8.5" fill="#475569">reduce task：按 reduce_num 顺序连续下发 N 笔 credit 请求</text>
<text x="1012.0" y="215.5" font-size="8.5" fill="#475569">Head-only 退休：只有 head_ptr 指向的项可退休，条件是</text>
<text x="1012.0" y="229.0" font-size="8.5" fill="#475569">　valid=1 且 end=1 且 task_fsm=FINISH</text>
<text x="1012.0" y="242.5" font-size="8.5" fill="#475569">先向 Router 持续发 credit 返还请求，Router 接收后</text>
<text x="1012.0" y="256.0" font-size="8.5" fill="#475569">　才清该槽位的 valid 并推进 head_ptr</text>
<text x="1012.0" y="269.5" font-size="8.5" fill="#475569">B core 的搬出 task 按 B_CORE_DIRECTION 查下游 credit</text>
<rect x="160" y="420" width="430" height="249.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="172" y="441" font-size="11" fill="#111827" font-weight="600">Stream_table</text>
<text x="172.0" y="458.0" font-size="8.5" fill="#475569">16 项顺序 FIFO，head_ptr 与 tail_ptr 环形推进</text>
<text x="172.0" y="471.5" font-size="8.5" fill="#475569"></text>
<text x="172.0" y="485.0" font-size="8.5" fill="#475569">用户级标记（建表写入，整链期间基本不动）：</text>
<text x="172.0" y="498.5" font-size="8.5" fill="#475569">　valid · user_id · reissue · compute</text>
<text x="172.0" y="512.0" font-size="8.5" fill="#475569">进度：</text>
<text x="172.0" y="525.5" font-size="8.5" fill="#475569">　task_id · task_fsm · done_bitmap（64 位对应 64 个 task）</text>
<text x="172.0" y="539.0" font-size="8.5" fill="#475569">　异步 datain 提前完成 = 某位先亮而 task_id 还没走到</text>
<text x="172.0" y="552.5" font-size="8.5" fill="#475569">当前 task 的属性（随 task_id 索引 task_chain 得到）：</text>
<text x="172.0" y="566.0" font-size="8.5" fill="#475569">　task_unit · task_dsa_en · task_pc · is_reissue · end</text>
<text x="172.0" y="579.5" font-size="8.5" fill="#475569"></text>
<text x="172.0" y="593.0" font-size="8.5" fill="#475569">task_fsm 五态：IDLE → WAIT → READY → INFLY → FINISH</text>
<text x="172.0" y="606.5" font-size="8.5" fill="#475569">六个写口在此仲裁，每口一拍一笔，请求保持到 accepted</text>
<text x="172.0" y="620.0" font-size="8.5" fill="#475569">　整项写入失败要重读最新表内容再来</text>
<text x="172.0" y="633.5" font-size="8.5" fill="#475569">　单字段写失败只重试这一笔，不重发已被接收的任务</text>
<text x="578" y="660.0" font-size="8.5" fill="#9ca3af" text-anchor="end">stream_num 可配 1～16</text>
<rect x="640" y="420" width="450" height="181.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="652" y="441" font-size="11" fill="#111827" font-weight="600">Task_done</text>
<text x="652.0" y="458.0" font-size="8.5" fill="#475569">七路完成合流，直接写 stream_table，不设统一 Update 模块</text>
<text x="652.0" y="471.5" font-size="8.5" fill="#475569">　DTE / MU / VU 各有 RV core ack 与 DSA ack，共六路</text>
<text x="652.0" y="485.0" font-size="8.5" fill="#475569">　DTE 这一路额外接收 Router 的 Reduce Done</text>
<text x="652.0" y="498.5" font-size="8.5" fill="#475569">按当前任务的 task_recv_type 判哪一路才算数：</text>
<text x="652.0" y="512.0" font-size="8.5" fill="#475569">　00 = 只调 RV core / 01 = 调 DSA / 10 = DTE DSA + Rmem 两者都要</text>
<text x="652.0" y="525.5" font-size="8.5" fill="#475569">Reduce 拆两半：DTE ack 只 consume_only，不改 stream 状态；</text>
<text x="652.0" y="539.0" font-size="8.5" fill="#475569">　只有 Router Reduce Done 才能置 TASK_FINISH</text>
<text x="652.0" y="552.5" font-size="8.5" fill="#475569">　两者可任意顺序；Router Done 可被 Hold，但要等匹配的</text>
<text x="652.0" y="566.0" font-size="8.5" fill="#475569">　DTE ack 被消费后才提交</text>
<text x="652.0" y="579.5" font-size="8.5" fill="#475569">Router 不携带 stream_id，按 user_id 找对应 Stream</text>
<rect x="160" y="760" width="300" height="140.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="172" y="781" font-size="11" fill="#111827" font-weight="600">User_Match</text>
<text x="172.0" y="798.0" font-size="8.5" fill="#475569">拿请求里的 user_id 与 stream_table 比对</text>
<text x="172.0" y="811.5" font-size="8.5" fill="#475569">没匹配上 → 新用户，发建表请求</text>
<text x="172.0" y="825.0" font-size="8.5" fill="#475569">匹配上 → 老用户，复用原 stream_id</text>
<text x="172.0" y="838.5" font-size="8.5" fill="#475569">　当前任务、状态、完成位一律不动</text>
<text x="172.0" y="852.0" font-size="8.5" fill="#475569">　只有带重发标记时才置 reissue</text>
<text x="172.0" y="865.5" font-size="8.5" fill="#475569">建表四条同时满足：新用户 · 不在 weights 模式</text>
<text x="172.0" y="879.0" font-size="8.5" fill="#475569">　· trigger_task_chain_en · tail−head &lt; stream_num</text>
<rect x="500" y="760" width="300" height="127.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="512" y="781" font-size="11" fill="#111827" font-weight="600">DataIn_task_table</text>
<text x="512.0" y="798.0" font-size="8.5" fill="#475569">只有 1 项（Depth-1 Hold）</text>
<text x="512.0" y="811.5" font-size="8.5" fill="#475569">空闲时把 datain 任务与请求信息一起登记</text>
<text x="512.0" y="825.0" font-size="8.5" fill="#475569">被占住时反压 Router 的新请求</text>
<text x="512.0" y="838.5" font-size="8.5" fill="#475569">仲裁成功并被 DTE RV core 接收后立即释放</text>
<text x="512.0" y="852.0" font-size="8.5" fill="#475569">datain 的 task_pc 进这里，Task 0 的进 stream_table</text>
<text x="512.0" y="865.5" font-size="8.5" fill="#475569">B core 与 R core 的 datain 任务不建表</text>
<rect x="840" y="760" width="340" height="181.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="852" y="781" font-size="11" fill="#111827" font-weight="600">MU_Arb</text>
<text x="852.0" y="798.0" font-size="8.5" fill="#475569">候选：valid=1 且 task_fsm=READY</text>
<text x="852.0" y="811.5" font-size="8.5" fill="#475569">　且 task_unit=MU</text>
<text x="852.0" y="825.0" font-size="8.5" fill="#475569">从 head_ptr 开始环形年龄优先，</text>
<text x="852.0" y="838.5" font-size="8.5" fill="#475569">选最老的 Stream</text>
<text x="852.0" y="852.0" font-size="8.5" fill="#475569">发射宽度 1</text>
<text x="852.0" y="865.5" font-size="8.5" fill="#475569">非抢占保持到 raw ACCEPT</text>
<text x="852.0" y="879.0" font-size="8.5" fill="#475569">RV core 的 task_queue 满时会反压</text>
<text x="852.0" y="892.5" font-size="8.5" fill="#475569">ACCEPT 后提交 READY → INFLY</text>
<text x="852.0" y="906.0" font-size="8.5" fill="#475569">Map 返回未接受时只重试这笔</text>
<text x="852.0" y="919.5" font-size="8.5" fill="#475569">　状态写，不重发已被接收的任务</text>
<rect x="1220" y="760" width="340" height="181.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1232" y="781" font-size="11" fill="#111827" font-weight="600">VU_Arb</text>
<text x="1232.0" y="798.0" font-size="8.5" fill="#475569">候选：valid=1 且 task_fsm=READY</text>
<text x="1232.0" y="811.5" font-size="8.5" fill="#475569">　且 task_unit=VU</text>
<text x="1232.0" y="825.0" font-size="8.5" fill="#475569">规则同 MU_Arb</text>
<text x="1232.0" y="838.5" font-size="8.5" fill="#475569">三条发射通路各自逐拍推进，</text>
<text x="1232.0" y="852.0" font-size="8.5" fill="#475569">同一拍可以并行下发 3 个 task</text>
<text x="1232.0" y="865.5" font-size="8.5" fill="#475569"></text>
<text x="1232.0" y="879.0" font-size="8.5" fill="#475569">B core：task 0 借 VU core 跑</text>
<text x="1232.0" y="892.5" font-size="8.5" fill="#475569">　纯标量的 check_flag</text>
<text x="1232.0" y="906.0" font-size="8.5" fill="#475569">R core：task 0 借 MU core 跑</text>
<text x="1232.0" y="919.5" font-size="8.5" fill="#475569">　check flag，求和交给 VU</text>
<rect x="1600" y="760" width="340" height="194.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1612" y="781" font-size="11" fill="#111827" font-weight="600">DTE_Arb</text>
<text x="1612.0" y="798.0" font-size="8.5" fill="#475569">候选：valid=1 且 task_fsm=READY 且 task_unit=DTE</text>
<text x="1612.0" y="811.5" font-size="8.5" fill="#475569">三类任务的优先级：</text>
<text x="1612.0" y="825.0" font-size="8.5" fill="#475569">　1. Reissue 任务优先级最高，从 head_ptr 选最老的</text>
<text x="1612.0" y="838.5" font-size="8.5" fill="#475569">　2. 没有 Reissue 时，DataIn 与普通 Generated 按相对</text>
<text x="1612.0" y="852.0" font-size="8.5" fill="#475569">　　 head_ptr 的 Stream 年龄比较，较老者优先</text>
<text x="1612.0" y="865.5" font-size="8.5" fill="#475569">　3. 同一 Stream 时优先选 Generated</text>
<text x="1612.0" y="879.0" font-size="8.5" fill="#475569">选中后非抢占保持：锁定任务上下文，命令与相关字段</text>
<text x="1612.0" y="892.5" font-size="8.5" fill="#475569">　保持稳定直到 RV core 返回 raw ACCEPT</text>
<text x="1612.0" y="906.0" font-size="8.5" fill="#475569">收到 ACCEPT 后：Generated 提交 READY → INFLY；</text>
<text x="1612.0" y="919.5" font-size="8.5" fill="#475569">　DataIn 只通知 DataIn_task_table 出槽，不改 stream 状态</text>
<text x="1612.0" y="933.0" font-size="8.5" fill="#475569">task_dsa_en=0 的 Generated 下发给 DTE RV core，不配 DSA</text>
<polygon points="749,44 860,44 851,74 740,74" fill="#f8fafc" stroke="#374151"/>
<text x="800.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">cfg（ctrl_noc）</text>
<polygon points="29,300 140,300 131,332 20,332" fill="#f8fafc" stroke="#374151"/>
<text x="80.0" y="319.5" font-size="9" fill="#374151" text-anchor="middle">ts2corestatus</text>
<polygon points="244,1040 385,1040 376,1070 235,1070" fill="#f8fafc" stroke="#374151"/>
<text x="310.0" y="1058.5" font-size="9" fill="#374151" text-anchor="middle">router2ts_trigger_ch</text>
<polygon points="949,1040 1090,1040 1081,1070 940,1070" fill="#f8fafc" stroke="#374151"/>
<text x="1015.0" y="1058.5" font-size="9" fill="#374151" text-anchor="middle">task_cmd / task_ack［MU］</text>
<polygon points="1329,1040 1470,1040 1461,1070 1320,1070" fill="#f8fafc" stroke="#374151"/>
<text x="1395.0" y="1058.5" font-size="9" fill="#374151" text-anchor="middle">task_cmd / task_ack［VU］</text>
<polygon points="1709,1040 1850,1040 1841,1070 1700,1070" fill="#f8fafc" stroke="#374151"/>
<text x="1775.0" y="1058.5" font-size="9" fill="#374151" text-anchor="middle">task_cmd / task_ack［DTE］</text>
<polygon points="1859,157.35 1980,157.35 1971,189.35 1850,189.35" fill="#f8fafc" stroke="#374151"/>
<text x="1915.0" y="176.8" font-size="9" fill="#374151" text-anchor="middle">router2ts_credit_ch</text>
<polygon points="1859,229.75 1980,229.75 1971,261.75 1850,261.75" fill="#f8fafc" stroke="#374151"/>
<text x="1915.0" y="249.2" font-size="9" fill="#374151" text-anchor="middle">ts2router</text>
<polygon points="1859,467.35 1980,467.35 1971,499.35 1850,499.35" fill="#f8fafc" stroke="#374151"/>
<text x="1915.0" y="486.9" font-size="9" fill="#374151" text-anchor="middle">rmem2ts_done_ch</text>
<polygon points="1859,539.75 1980,539.75 1971,571.75 1850,571.75" fill="#f8fafc" stroke="#374151"/>
<text x="1915.0" y="559.2" font-size="9" fill="#374151" text-anchor="middle">rv / dsa done ×6</text>
<path d="M795.5 74.0 L799.9 109.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<path d="M650.0 193.8 L611.0 200.3" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<rect x="542.0" y="94.5" width="176.0" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="630" y="102" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#7c3aed" text-anchor="middle">task_chain 属性 / 三张掩码 / Task LUT</text>
<path d="M710.0 277.5 L710.0 376.0 L547.0 376.0 L547.0 419.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<rect x="579.9" y="364.5" width="120.2" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="640" y="372" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#7c3aed" text-anchor="middle">stream_num · datain_task</text>
<path d="M482.5 420.0 L497.4 292.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="500.2" y="317.0" width="10.5" height="77.1" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 505.5 355.5)" x="505.5" y="358.5" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">候选 task 与属性</text>
<path d="M160.0 200.5 L110.0 200.5 L110.0 720.0 L1770.0 720.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round"/>
<path d="M1010.0 720.0 L1010.0 759.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M1390.0 720.0 L1390.0 759.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M1770.0 720.0 L1770.0 759.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<text transform="rotate(-90 104 525.5)" x="104" y="525.5" font-size="8" fill="#6b7280" text-anchor="middle">下一条可跳过后的 task，按 task_unit 分到三条发射通路</text>
<path d="M640.0 510.5 L590.8 543.9" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="571.5" y="404.5" width="86.9" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="615" y="412" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">task_state_update</text>
<path d="M1064.5 291.0 L1064.0 396.0 L568.5 396.0 L568.5 419.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="751.4" y="384.5" width="97.1" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="800" y="392" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">READY（credit 到手）</text>
<path d="M310.0 760.0 L374.4 669.8" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="312.8" y="697.2" width="10.5" height="34.6" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 318 714.5)" x="318" y="717.5" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">create</text>
<path d="M460.0 830.2 L499.0 823.7" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="462.2" y="810.0" width="34.6" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="479.5069706146114" y="817.5" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">datain</text>
<path d="M314.5 1040.0 L310.0 901.5" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#g)"/>
<rect x="312.8" y="902.6" width="10.5" height="135.2" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 318 970.25)" x="318" y="973.2" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#0f766e" text-anchor="middle">user_id · path_id · 重发标记</text>
<path d="M1854.5 483.4 L1091.0 483.4" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#g)"/>
<rect x="1369.6" y="469.9" width="200.8" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1470.0" y="477.35" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#0f766e" text-anchor="middle">rmem2ts_done_ch 按 user_id 匹配对应 Stream</text>
<path d="M1854.5 555.8 L1091.0 555.8" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="1383.7" y="542.2" width="172.6" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1470.0" y="549.75" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">rv_done ×3 · dsa_done ×3 · VU Event</text>
<path d="M1010.1 942.0 L1019.4 1039.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1390.1 942.0 L1399.4 1039.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1770.1 955.5 L1779.4 1039.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1854.5 173.3 L1431.0 173.3" stroke="#0f766e" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#g)"/>
<rect x="1573.4" y="159.8" width="133.2" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1640.0" y="167.35" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#0f766e" text-anchor="middle">收 Router 的 credit 到手通知</text>
<path d="M1430.0 245.8 L1853.5 245.8" stroke="#b45309" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#o)"/>
<rect x="1566.3" y="232.2" width="147.4" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1640.0" y="239.75" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#b45309" text-anchor="middle">资源注册 / retire / credit 返还</text>
<path d="M160.0 494.7 L150.0 494.7 L150.0 316.0 L136.5 316.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#a)"/>
<rect x="153.2" y="345.4" width="9.5" height="120.0" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 158 405.35)" x="158" y="408.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="7.5" fill="#475569" text-anchor="middle">异常上报 → Core Status → SCP</text>
<text x="20" y="348" font-size="8" fill="#6b7280" text-anchor="start">（本轮只留接口名）</text>
<text x="20" y="1114" font-size="10.5" fill="#374151" text-anchor="start">TS 是一块固定的硬件逻辑，不是可编程的调度器：上电配好几张表、写 TS_INIT_FINISH 之后就按固定逻辑跑，运行期不接受软件干预，也没有指令可执行。三条发射通路各自逐拍推进。Router 在 core 底边，四条通路物理上竖穿 core。</text>
</svg>
```

***

## 2　功能清单

模块划分照 TS MAS 的两张表，分两层。下面的小节按这个划分组织。

| 层 | 模块 |
| - | - |
| 顶层（九个） | `User_Match`、`CFG_REG`、`DataIn_task_table`、`Stream_table`、`Task_ctrl`、`DTE_Arb`、`MU_Arb`、`VU_Arb`、`Except Check` |
| `Stream_table` 内（七个） | `user_LUT`、`ptr_ctrl`、`stream_id_map`、`task_state_update`、`task_rdy_check`、`retire`、`except_check` |
| `task_state_update` 内（三个） | `task_ctrl`、`task_done`、`credit` |

MAS 里的 `Credit_monitor` 标题划了删除线，已不再是独立模块：出核前的资源监听在 Router 的 `CoreMemCreditMonitor`，TS 这侧的 credit 处理落在 `task_state_update` 的 `credit` 子模块。`Task_ctrl` 与 `Except Check` 在 MAS 的两张表里各出现一次，顶层与子模块同名。


一功能一条，编号供“机制覆盖”一章引用。

### CFG_REG

| 编号 | 功能 |
| - | - |
| F1 | `task_chain` 最多 64 项，每项 64 bit，软件经 ctrl_noc 逐项写；每写一项硬件自动把该项的 `TASK_VALID` 置起来。这张表是一条全序链，位域里没有前驱表也没有后继表，依赖信息只有数组下标 |
| F2 | 表项位域：`TASK_PC`、`TASK_SEND_UNIT`（00=DTE / 01=MU / 10=VU）、`TASK_RECV_UNIT`（00=只调 RV core / 01=调 DSA / 10=DTE DSA 加 Router 的 Rmem）、`SELF_START`、`WAIT_WAKE`、`TASK_Broadcast_REISSUE`、`TASK_P2P_REISSUE`、`TASK_REDUCE`、`TASK_CREDIT_EN`、`TASK_EXE_MASK`、`TASK_PATH_ID`、`TASK_END`、`TASK_VALID` |
| F3 | `DATAIN_TASK` 是独立的一项寄存器，只有 1 项、只能绑定 DTE，四个字段 `TASK_PC` / `TASK_UNIT` / `WEIGHTS_MODE` / `TASK_VALID` |
| F4 | 其余配置项：`STREAM_NUM`（1～16）、`CORE_TYPE`、`B_CORE_DIRECTION`、`trigger_task_chain_en`、`TS_INIT_FINISH`、`TS_STATE`，另有按 `path_id` 索引的 `path_flowctl`（`flowctl_en` 加 `window_n`，见超前发送窗口一节） |
| F5 | 配置的下发顺序：先写全局项，再逐项写 `task_chain[i]`，再逐项写 `path_task_map[j]`，再写 `DATAIN_TASK`，最后写 `TS_INIT_FINISH`。最后一步之前硬件不做任何检查 |
| F6 | 看到 `TS_INIT_FINISH` 后查六项合规性并把结论写进 `TS_STATE`：是否配置、一条链上是否出现多个 `TASK_END`、valid 的项中间是否有空洞、一条链上是否出现多个 `SELF_START`、普通 core 是否配了 `SELF_START` |
| F7 | 派生四张 64 位掩码供 Task_ctrl 一拍算出 SKIP_MASK：`DATA_IN_MASK`、`REISSUE_MASK`、`END_MASK`、`EXE_MASK`。四张都是 `task_chain` 的纯函数，配完不再变，运行时的跳过判断只查掩码、`done_bitmap` 与两个用户级标记，不必逐项回读 `task_chain` |
| F8 | `EXE_MASK` 直接把每个 `task_chain[i].TASK_EXE_MASK` 这一位收拢成一张 64 位掩码，不经软件侧属性派生。极性照寄存器定义：**1 = 这个 task 不按用户区分，所有用户都做；0 = 按用户区分，只有 `compute = 1` 的用户做**。全 1 时这一项不起作用，非 DP+P2P 的 core 就配成全 1 |
| F9 | Task LUT：按 `task_id` 索引出该 task 的全部下发属性，供三条发射通路查 |
| F10 | `path_task_map`：64 项，按 `path_id` 索引出 `{task_id, valid}`，软件经 ctrl_noc 逐项写。Router 的请求只带 `path_id`，本表是把它翻译成任务链上第几步的唯一途径 |
| F11 | 三处用它：datain 请求进来时定这一笔属于哪个 datain 任务，取该项的 `task_pc`；完成事件回来时定该点亮 `done_bitmap` 的哪一位；P2P 不需要重发时 Router 按 `path_id` 告知哪个 task 已完成，本表定位到那一项 |
| F12 | 写 `TS_INIT_FINISH` 时一并查：`task_chain` 里每个带 `TASK_PATH_ID` 的项，其 `path_id` 在本表里必须有 valid 项且指回该项自己；查不过写进 `TS_STATE` |
| F13 | 硬件位域里没有的软件侧属性从软件侧任务链表另行读入，与硬件表分开存：`exe_dest`、`reduce_num`、`dsa_en`。分组不在这一档，它由硬件位域 `TASK_EXE_MASK` 承载 |
| F14 | `CORE_TYPE` 是 B core 或 R core 时，硬件在复位后直接自启动 16 个表项，每项按 task 0 的属性建立、`task_fsm` 置 `TASK_RDY`；此时还没有用户信息，等自启动任务的 RV core 返回 `user_id` 后补进表项 |

### User_Match

| 编号 | 功能 |
| - | - |
| F15 | 拿 Router 请求里的 `user_id` 与 `stream_table` 里的比对 |
| F16 | 没匹配上是新用户，向 Stream_table 发起建表请求 |
| F17 | 匹配上是老用户，复用原来的 `stream_id`，不改它当前的任务、状态、完成位；只有请求带着重发标记时才把该项的 `reissue` 置起来 |
| F18 | 四条同时满足才建表：判定是新用户、当前不在 weights 加载模式、`trigger_task_chain_en` 允许启动任务链、`tail_ptr − head_ptr < stream_num` |
| F19 | **trigger 口不设入口队列**。四条不全满足时直接拉低 `router2ts_trigger_ch` 的 `ready`，由 CoreStation 保持这一笔请求，每拍重判一次，不丢弃、不越过。这条通路上的 trigger 与 token 一一对应，丢一笔就等于丢一个 token，因此只许反压不许丢 |
| F20 | 不设队列的理由：请求的保持责任本来就在 CoreStation 那一侧，加一级队列只是把同一个反压点往后挪一格，既不改变正确性也不提高吞吐，反而多一处要维护的保序状态 |
| F21 | 建表时在 `tail_ptr` 指向的空槽一次性写入用户信息、Task 0 的状态与全部属性，随后推进 `tail_ptr` |
| F22 | Task 0 的初始状态按类型定：datain 或 reissue 置 `TASK_WAIT`，`self_start` 置 `TASK_RDY` |

### DataIn_task_table

| 编号 | 功能 |
| - | - |
| F23 | 只有 1 项。空闲时把 datain 任务信息与 Router 的请求信息一起登记进去 |
| F24 | 被占住时不再接收新的 datain 请求：直接拉低 `router2ts_trigger_ch` 的 `ready`。同一笔 Router 请求既要建表又要登记 datain，两边任一处不接受，这一笔就整体停住 |
| F25 | 任务仲裁成功并被 DTE RV core 接收后，该项立即释放 |
| F26 | 两种 PC 的去处不同：Task 0 的 `task_pc` 进 stream_table，datain 的 `task_pc` 进这里 |
| F27 | datain 的 `task_pc` 不取 `DATAIN_TASK` 那一项：先 `t = path_task_map[path_id].task_id`，再取 `task_chain[t].TASK_PC`，连同 `t` 一起登记。一条链上有多个 datain 任务（`tp_nk` 的 task 0、3、7、8）时，靠这一步分开。`DATAIN_TASK` 只在 weights 加载模式与 B / R core 上生效，那两种情况全链只有一个 datain 任务 |
| F28 | B core 与 R core 的 datain 任务不建 stream 表项 |

### Stream_table

| 编号 | 功能 |
| - | - |
| F29 | 16 项顺序 FIFO，每项对应一条完整用户业务流；`head_ptr` 与 `tail_ptr` 环形推进 |
| F30 | 用户级标记（建表写入，整条任务链期间基本不动）：`valid`、`user_id`、`reissue`、`compute`。用户号只有 `user_id` 一个：Router 与 credit 记账认它，软件也读它算 R core 的用户映射表与 Matrix Mem 地址。普通计算 core 上建表时从 Router 请求里取，自启动 core 上由软件 `flag_check` 之后写回 |
| F31 | 进度三字段合起来才是完整进度：`task_id` 是链上的第几步，`task_fsm` 是这一步的状态，`done_bitmap` 是 64 位对应 64 个 task 的数据齐没齐 |
| F32 | 异步 datain 提前完成表现为 `done_bitmap` 上某一位先亮而 `task_id` 还没走到那里 |
| F33 | 当前 task 的属性摊平存在表项里（`task_unit`、`task_dsa_en`、`task_pc`、`is_reissue`、`end`），每次更新 `task_id` 时索引 `task_chain` 得到，随 `task_id` 一起被覆盖 |
| F34 | `task_fsm` 五个状态：IDLE → WAIT → READY → INFLY → FINISH |
| F35 | **六个写口**，按来源命名，固定优先级仲裁，每口一拍一笔，请求保持到 `accepted` 才算生效。优先级由高到低：<br>1 `retirement`（Task_done 侧，清 `valid` 并推 `head_ptr`）<br>2 `completion`（Task_done 的三条 Completion Lane）<br>3 `install`（Task_ctrl 生成后继，整项写）<br>4 `issue`（三条发射通路收到 ACCEPT 后改 `TASK_READY → TASK_INFLY`）<br>5 `credit_wake`（`task_state_update` 的 `credit` 子模块收到 credit 后置 `TASK_READY`）<br>6 `create`（User_Match 建表，整项写）<br>次序的依据是让表项先腾空再填新的：回收类写口排在生成类前面，`create` 排最后，队头卡住时不会因为新用户不断插队而饿死 |
| F36 | 写失败分两种：`create`、`install` 这类整项写入失败后要重读最新表内容再来；`issue` 这类只改一个字段的失败后只重试这一笔写，不能重新下发已经被 RV core 接收的任务 |
| F37 | `completion` 口写两样东西，但改哪一样有条件：`done_bitmap[task_id]` 无条件置位；`task_fsm` **只有这一笔的 `task_id` 等于该 stream 当前的 `task_id` 时才改**。异步 datain 提前完成就落在这个分支上 —— 只亮一位，不动状态机 |
| F38 | `credit_wake` 口除了把 `task_fsm` 置 `TASK_READY`，在这一笔 credit 对应的是一个已经建过表的老用户时，还要把该表项的 `reissue` 置起来 |

### Stream_table 的七个子模块

MAS 把 `Stream_table` 单独展开一张表。功能描述照 MAS 原文，`-` 表示 MAS 那一栏是空的。

| 子模块 | MAS 给的功能描述 | 本文档对应的条目 |
| - | - | - |
| `user_LUT` | — | 建表时按 `user_id` 落项，见 F16～F22 |
| `ptr_ctrl` | — | `head_ptr` 与 `tail_ptr` 的环形推进，见 F29 |
| `stream_id_map` | 并行记录多用户的调度任务与执行状态：`task_id` 记当前执行到 `task_chain` 的哪一步，`task_fsm` 记这一步的状态，`done_flag` 记异步 datain 搬移完成的标志 | F30～F34 |
| `task_state_update` | 按执行单元返回与 Router 的信息更新 task_state。MAS 注明「这只是方便理解抽象出的模块，重点是下面的子模块」：`task_ctrl` 生成 `next_task_id` 并更新 `task_fsm`、`task_done` 收 DSA 与 RV core 的完成标识、`credit` 检测下游 core 的 credit 满足后更新 `task_fsm` | F39～F48、F60～F83 |
| `task_rdy_check` | 把 `task_fsm` 为 RDY 的用户送到 Task Iss 做发射仲裁 | F49～F52 |
| `retire` | 检查 `task_finish` 的用户当前任务是不是 `task_end` | F84～F88 |
| `except_check` | — | 与顶层的 `Except Check` 同名同责，见下一节 |

### Task_ctrl（对应 `task_state_update` 的 `task_ctrl` 子模块）

| 编号 | 功能 |
| - | - |
| F39 | 当前任务进入 `TASK_FINISH` 后才生成后继；每个 stream 独立推进，不需要全局 Task Pointer |
| F40 | 从 `head_ptr` 开始环形扫描，只选 `valid=1 && task_fsm=TASK_FINISH && end=0` 的 stream |
| F41 | 用一次 64 bit 优先编码一拍跳过所有可跳过的 task：<br>`GROUP_SKIP = stream.compute ? 0 : ~EXE_MASK`<br>`SKIP_MASK = ~END_MASK & ( (DATA_IN_MASK & done_bitmap) \| (REISSUE_MASK & ~stream.reissue) \| GROUP_SKIP )`<br>连续 skip 的数量不增加周期 |
| F42 | `GROUP_SKIP` 是 `compute` 这一位唯一被读的地方：`compute = 1` 的用户一个都不跳；`compute = 0` 的用户跳过所有 `TASK_EXE_MASK = 0` 的 task。`EXE_MASK` 全 1 时这一项恒为 0 |
| F43 | 可跳过的四种场景：异步 datain_task 已提前完成；B reissue 任务不需要重发；P2P 任务不需要重发（Router 通过匹配 `path_id` 告知哪个 P2P task 已完成）；DP+P2P 场景下有些用户只有 P2P 无计算 task、有些是计算无 P2P，Router 请求携带用户是否计算，其余任务可越过 |
| F44 | End task 即使已提前完成也不能被跳过，并且不会再生成后继任务 |
| F45 | 新任务的 `task_id`、`task_fsm`、`end` 与全部下发属性必须一起原子写入才算生成成功 |
| F46 | 生成后的初始状态按类型分：普通 Generated 置 `TASK_READY`，DataIn 置 `TASK_WAIT` 等 ack，Reissue 置 `TASK_WAIT` 等 credit |
| F47 | 就绪只有两个来源：前序任务都完成（`done_bitmap` 对应位拉高），以及 credit 到位（本任务不需要 credit，或需要的 credit 已经满足）。datain 任务是例外，被 Router trigger 后立即就绪 |
| F48 | 异步 datain 搬运完成后只更新 `done_bitmap` 对应位，不推进 `task_id`。datain 的执行时刻就是数据到达时刻，与主线走到哪无关：相对链序可以任意提前，也没有压着不搬、等主线走到这一步的机制，链上的位置只用来定点亮 `done_bitmap` 的第几位。数据晚到时停下来等的是主线，它卡在这一项的 `TASK_WAIT` 上，链上排在它后面的任务全部被挡住，不分需不需要这笔数据 |

### DTE_Arb

| 编号 | 功能 |
| - | - |
| F49 | 候选是 `valid=1 && task_fsm=TASK_READY && task_unit=DTE` 的 stream |
| F50 | Reissue 任务优先级最高，从 `head_ptr` 选最老的 Reissue |
| F51 | 没有 Reissue 时，DataIn 任务与普通 Generated 任务按相对 `head_ptr` 的 Stream 年龄比较，较老者优先；同一 Stream 时优先选 Generated |
| F52 | 选中后非抢占保持：锁定任务上下文，命令与相关字段保持稳定，直到对应 RV core 返回 `raw ACCEPT` |
| F53 | 收到 ACCEPT 后，Generated 任务向 Stream_table 提交 `TASK_READY → TASK_INFLY`；DataIn 任务只通知 DataIn_task_table 出槽，不改 stream_table 里的当前任务状态 |
| F54 | `task_dsa_en = 0` 的 Generated 任务下发给 DTE Local RV core，不配 DSA |
| F55 | 重发 task 不在用户主线任务链上，可与用户任务链并行执行 |

### MU_Arb / VU_Arb

| 编号 | 功能 |
| - | - |
| F56 | 候选是 `valid=1 && task_fsm=TASK_READY && task_unit=MU`（或 VU）的 stream |
| F57 | 从 `head_ptr` 开始环形年龄优先，选最老的 Stream；发射宽度各 1 |
| F58 | 三条发射通路各自逐拍推进，同一拍可以并行下发 3 个 task |
| F59 | RV core 按 task_queue 是否有空槽产生 `task_ack`；未被接收时 TS 不能释放该 task 跳到下一个 |

### credit（`task_state_update` 子模块）

MAS 顶层的 `Credit_monitor` 已划删除线，这些功能归 `task_state_update` 的 `credit` 子模块：检测 Router 送来的下游 core credit，满足后更新 `task_fsm`。

**与 DTE 的分工**：业务层的资源在这一级查完：TS 查 RouterTable 和对应的 stream 资源，**有资源才下发**；下发之后 DTE 只查 VC 通路上的 flit credit（见 DTE 一节 F53）。两类业务层 credit（下游的 coremem credit 与 reduce credit）都分方向，方向由 RouterTable 定。

配套一条软件约束：**软件要保证 TS 里的任务足够小，下发到 DTE 之后不需要 RV core 再拆**。TS 看到大任务、DTE 看到小任务时，一个大任务拆出的小任务数量不确定，可能填满 DTE 的 TaskQueue，把 TS 下一个大任务堵在外面；而下一个大任务恰好是释放下游资源的那一笔时就形成死锁。


| 编号 | 功能 |
| - | - |
| F60 | 向 Router 注册资源申请，带 UserID、StreamID、TaskID、PathID；Router 申请到后经反向控制通路通知，唤醒对应 task 置 READY |
| F61 | credit 的单位是 stream 不是 task：Core Mem 空间按 stream 申请和释放 |
| F62 | 发送分两道门，按这个 user 在目标方向上是否已经持有资源分：尚未持有走用户资源门控，查下游 stream credit，够才占用一个表项；已经持有走发送就绪门控，不查 stream credit，只受 VC credit 与链路握手的约束。因此同一个 stream 的任务链里，Router 只在第一次往下游发数据时检查 stream credit，之后发往同一下游的 DTE 任务都走发送就绪门控。免检的只是 stream credit，每个 flit 照样要查目标方向那个 VC 的 credit |
| F98 | 是否已持有按每个出方向各自独立记录。一次 fork 涉及多个方向时先滤掉已持有的方向，只对剩余未持有的方向查 stream credit，全部已持有则本次不查；待申请的这个子集要全部成功才发，任一方向不足则整体不发，已持有的方向也一起等，避免重复占用表项与数据重复 |
| F63 | 检查的发起方分两种：本 core Core Mem → 下游 Router 属于任务链里的 DTE 任务，由 TS 查下游 credit；本 core Router → 下游 Router 不属于任务链，由 Router 自己查下游 credit 并触发阻塞重发 |
| F64 | Broadcast Reissue：broadcast 数据到达 Router trigger TS 时下游无法接收则把重发标记置为有效；TS 查询下游 credit，可下发时按用户顺序选最老的重发用户，发起重发 task 到 DTE 去 Core Mem 搬 token 到 Router |
| F65 | 重发标记有效但未重发成功时，该用户的原始 token 数据不能被覆盖，也不能释放该用户 |
| F66 | P2P 重发：软件把 bypass 本 core 的任务配成 datain 与 dataout 两个 task。不需要重发时 Router 用 `path_id` 匹配告知已完成，执行到该 task 时直接越过；需要重发时 datain 由 Router 请求直接驱动 datain_task 搬运，dataout 等前序任务做完、执行到本 task 时向 Router 请求 credit，满足后标记就绪等待下发 |
| F67 | P2P 阻塞缓冲：软件可配开关、分给 P2P 缓存的 Core Mem 容量、开启的方向（最多 3 个）、每方向的容量与项数 |
| F68 | TS 侧为 P2P 阻塞缓冲维护每方向一张映射表（`p2p vld` / `User id` / `Down direction` / `Data addr`）与对应下游 core 的 credit 计数器；下游 credit 释放后发起一个调度 DTE 搬数据到 Router 继续传输的 task，并通知上游释放 credit |
| F69 | Reduce 任务的 credit：软件配 `reduce_num = N`，TS 发现是 reduce task 后顺序连续下发 N 笔 credit 请求，credit 满足即可顺序下发，直到收全 N 笔 Rmem finish。这 N 笔之间不要求前一笔完成才发下一笔，连发是为了压掉逐级 reduce 的延迟。N 由 Rmem 容量定：单用户 16 KB 装不下 32 KB 的一笔 reduce，软件按 8 KB 拆，`N = 4` |
| F70 | Head-only 退休：只允许 `head_ptr` 指向的那一项退休，条件是 `valid=1 && end=1 && task_fsm=TASK_FINISH` |
| F71 | 退休顺序：先向 Router 持续发 credit 返还请求，Router 接收后才清除该槽位的 `valid` 并推进 `head_ptr`；返还的 credit 经本 core 的 Router 通知上游 core，让上游的 TS credit 加一 |
| F72 | B core 的搬出 task 按 `B_CORE_DIRECTION` 查下游 core 的 TS credit；落 Matrix Mem 的那一侧不查 TS credit，反压由 GPU 到 Bach 的两层 credit 兜底 |
| F73 | 广播任务的 `CreditCounter[path_id][stream_id]` 初值等于目的 core 数量，P2P 任务初值为 1；够则一次扣掉全部目的数再下发，不够就等 credit 释放 |
| F74 | 不派角色的 core 只按路由表透传，不检查 credit、不支持阻塞重发；上游要查的 credit 对应它之后那个落地的 core |

### task_done（`task_state_update` 子模块）

| 编号 | 功能 |
| - | - |
| F75 | 七路完成事件：DTE、MU、VU 各有 RV core ack 与 DSA ack 共六路，DTE 这一路额外接收 Router 的 Reduce Done。三条 Completion Lane 直接写 stream_table，不设统一的 Update 模块。七路都是脉冲，Task_done 永远就绪，不向上游反压：完成事件在硬件里没有重发通路，接收方一旦拒收就等于把那个 stream 永远停在当前 task。同一拍多路同时到达时各自写各自的 stream，落到同一个 stream 的按写口优先级排队，请求保持到 `accepted` |
| F76 | 按当前任务的 `task_recv_type` 判断哪一路才算数：只调 RV core 或调了 DSA 但 RV core 会等 DSA 完成后再执行一段程序时由 RV core 收尾；异步配置调度 DSA 后 RV core 立刻结束 task 程序时由 DSA 收尾；RV core 异步配置 DSA 后不查询完成状态但还要再执行一段程序时两者都上报，TS 等二者都完成 |
| F77 | Reduce 任务的完成拆成两半：DTE ack 只代表搬运完成，执行 `consume_only`，不修改 stream 状态；只有 Router Reduce Done 才有权把 Reduce 任务置为 `TASK_FINISH` |
| F78 | 两个事件可以任意顺序到达。Router Done 可以被 Hold，但必须等匹配的 DTE ack 被消费后才提交任务完成 |
| F79 | `reduce_num = N` 时两半各有 N 笔，**按包一一配对**：包头带一个 `reduce_seq` 字段（0～N−1），DTE 发出时打上，Router 的 Reduce Done 原样带回。`reduce_pend[stream]` 里两张 N 位的位图 `dte_ack_map` 与 `router_done_map`，各按 `reduce_seq` 置位；只有两张位图的低 N 位全满才把该 task 置 `TASK_FINISH`，提交时清零。这样第 k 笔的 Router Done 只与第 k 笔的 DTE ack 配对，不会出现两边总数凑够、实际却漏了某一笔的情况 |
| F80 | 同一个 `reduce_seq` 的两个事件可以任意顺序到达，先到的那个照常置位、不必等；F78 说的“等匹配的 DTE ack 被消费”落在**同一位**上：`dte_ack_map[k]` 与 `router_done_map[k]` 都置起来，第 k 笔才算配上 |
| F81 | Router 不携带 `stream_id`，Task_done 内部按 `user_id` 找对应 Stream |
| F82 | datain 任务的完成事件带的 `task_id` 就是 F27 查出来的那个 `t`，随 `task_cmd` 下发进 DTE RV core 的 CSR、经 `dsa_ids` 直连到 DTE、再由 `dsa_done` 原样回来。`done_bitmap[t] = 1`，`task_id` 不推进 |
| F83 | 异常检测：异步 task 本该执行 1 次完成却返回多次；RV core 或 DSA 返回非法的 `task_id` / `stream_id`；异步任务长时间没有收到外部 trigger；用户长时间未 retire；Router 请求携带的 `path_id` 在 task_chain 里匹配不到。异常经 `ts2corestatus_int_ch` 上报，本轮只留接口名与状态位 |

### Except Check

MAS 顶层模块表列的第九个模块：「负责检查在调度过程中出现的非法完成，超时等异常」。`Stream_table` 展开表里另有一个同名的 `except_check`，MAS 没给功能描述，两处指的是同一件事。

| 编号 | 功能 |
| - | - |
| F83 | 三类异常，见 `task_done` 一节 |
| F83a | datain 的 token 用 `path_id` 索引出的 `task_id` 如果不是 DTE 任务，算一种错误 |
| F83b | 超时检测机制：异步任务长时间没有收到完成事件 |

### 自启动（B core 与 R core）

| 编号 | 功能 |
| - | - |
| F84 | 复位后直接自启动 `stream_num` 个 stream_table 表项，同时激活这些用户的自启动任务参与仲裁发射，执行过程同普通计算 core。B core 与 R core 的 `stream_num` 配成 16，自启动数因此是 16；`stream_num < 16` 时自启动数随之减少，`tail_ptr − head_ptr < stream_num` 这条约束在自启动路径上同样成立 |
| F85 | 启动时没有用户信息，等自启动任务的 RV core 返回 `user_id` 后更新 stream_table，再调度后续任务 |
| F86 | 这一路走 `completion` 写口，不另设专用写口：`rv_done` 带回 `user_id`，Task_done 按 `stream_id` 定位表项，把它写进表项的 `user_id` 字段并置 `user_id_vld`，同时按 F37 的规则处理 `done_bitmap` 与 `task_fsm`。补写之前该表项的 `user_id` 无效，软件的用户映射表查不到它，向 Router 申请资源时也没有可带的用户号 |
| F87 | 数据接收类 datain 任务由 datain task 支持，但完成 flag 由软件设置维护，不在 TS 里更新 |
| F88 | check flag 的 RV core 需要长期工作，该 task 不调度对应的 DSA。跑 check flag 的那个 RV core 在这类 core 的任务链上没有别的活：B core 的链只有 VU 的 check flag 与 DTE 的搬出，R core 的链是 MU 的 check flag、DTE 搬入、VU 求和、DTE 搬出。因此长期占用不会挡住同一个 core 上的其他 task |
| F89 | 任务链创建不受用户数据 trigger 影响，自动在每个 stream 表项创建起点 task，按 stream 顺序激活执行 check flag task |
| F90 | 前一个 stream check 通过后需清除 ready 用户的 flag，下一个 stream 才能继续 check |
| F91 | 一个 stream 的任务链完成后会在对应 stream 项自发创建新的任务链；表项从队头 retire 后再激活一个新表项，继续等待自启动任务 |

### 超前发送窗口

| 编号 | 功能 |
| - | - |
| F92 | 要解决的问题：一条“广播 → P2P → 广播”的路径上，前半段广播跑得比后半段快时，后半段 core 的 Core Mem 收不下，数据在物理通路上排队，后半段 core 出现计算空泡。缓解办法是让发起方推迟发起 P2P 传输 |
| F93 | 判断依据：一个 TP / PPTP 区域内每个用户在每个 core 上的计算量与计算时间基本相同，看一个 core 的计算进度就能大致推算同区域其他 core 的进度。原文举的例子是 Core2 与 Core4 同一时刻在算的 `user_id` 相距 5～6，Core2 在算 user10 时 Core4 大致在算 user4 / user5 |
| F94 | 配置：某个 path 在某个 core 上可以配“流量控制使能”，使能后软件再配一个允许超前发送的用户窗口尺寸 `N`，`N` 是编译期确定的值 |
| F95 | 判断：TS 查本 core 内正在计算的 `user_id = M`，阻塞后续 `user_id ≥ M + N` 的传输；`user_id = M` 的计算任务完成后重新唤醒 `user_id = M + N` 的传输 |
| F96 | 这一档只对 TP / PPTP 内部的 P2P 传输有意义，跨 TP 组的传输不适用，因为跨组的计算时间不可推算 |
| F97 | “流量控制使能”配在 TS 的 CFG_REG 还是 RouterTable，原始文档只写了“在一个 Core 配置”，没有指明。本文档按配在 TS 的 CFG_REG 建模，与 `path_task_map` 同一层，因为判断要读 stream_table 里正在计算的 `user_id`，这个信息只有 TS 有 |

***

## 3　接口

```
port router2ts_trigger_ch (slave, valid/ready, clk)  // Router 的 CoreStation，Header 就绪即通知
  in  valid · user_id[15:0] · path_id[7:0] · reissue · compute
  out ready                                         // = 建表四条与 datain_hold 空位同时满足。拉低时 CoreStation 保持本笔请求
port router2ts_credit_ch (slave, 脉冲, clk)       // Router 的 CoreMemCreditMonitor：资源到手
  in  valid · stream_id[3:0] · task_id[5:0] · path_id[7:0]
port rmem2ts_done_ch (slave, 脉冲, clk)           // ReduceModule：一个整包 reduce 完成，按 user_id 匹配
  in  valid · user_id[15:0]
port ts2router_req (master, valid/ready, clk)     // 注册资源申请
  out req_valid · user_id[15:0] · stream_id[3:0] · task_id[5:0] · path_id[7:0]
  in  req_ready                                     // = Router 的监听事件队列有空项
port ts2router_retire (master, valid/ready, clk)   // 用户退休与 credit 返还
  out valid · user_id[15:0]
  in  accepted                                      // 即 ready：Router 接收后才清 valid 并推进 head_ptr
port task_cmd[u] (master, valid/ready, clk)       // u ∈ {DTE, MU, VU}：task 下发
  out cmd_valid · task_pc[31:0] · stream_id[3:0] · task_id[5:0] · user_id[15:0] · path_id[7:0] · task_dsa_en · seq
  in  cmd_ready                                     // = 该 RV core 的 task_queue 有空槽（raw ACCEPT）
port rv_done[u] (slave, 脉冲, clk)                // RV core 报完成；自启动的表项靠它补 user_id
  in  valid · stream_id[3:0] · user_id[15:0] · task_id[5:0] · reduce_seq[5:0]
port dsa_done[u] (slave, 脉冲, clk)               // DSA 报完成
  in  valid · stream_id[3:0] · task_id[5:0] · event   // event 只在 u = VU 有效，EVENT_EN 置位时随完成一起拉高
port ts2corestatus_int_ch (master, 电平, clk)     // 异常上报，本轮只留接口名
  out int_valid · int_code[7:0] · stream_id[3:0]
port cfg (slave, ctrl_noc 写事务, clk)            // CFG_REG 的配置口
  in  cfg_valid · cfg_addr[23:0] · cfg_we · cfg_wdata[31:0]
  out cfg_rdata[31:0]
```

***

## 4　存储器

```
mem task_chain     FF 阵列   64 × 64 bit（位域见 F2）                                     1R1W  软件逐项写，硬件自动置 VALID  复位 0
mem datain_task    FF        {task_pc[31:0], task_unit[1:0], weights_mode, valid}          1R1W  软件写                        复位 0
mem cfg_misc       FF        {stream_num[4:0], core_type[1:0], b_core_dir[6:0], trigger_task_chain_en, ts_init_finish, ts_state[7:0]}  1R1W  软件写  复位 0
mem sw_attr        FF 阵列   64 × {exe_dest, reduce_num[5:0], dsa_en}                      1R    编译侧读入                    复位由输入给   // 硬件位域里没有的软件侧属性
mem task_masks     FF        {DATA_IN_MASK[63:0], REISSUE_MASK[63:0], END_MASK[63:0], EXE_MASK[63:0]}  1R1W  写 TS_INIT_FINISH 时由 task_chain 派生  复位 0
mem path_task_map  FF 阵列   64 × {task_id[5:0], valid}，按 path_id 索引                    1R1W  软件逐项写                    复位 0
mem path_flowctl   FF 阵列   64 × {flowctl_en, window_n[7:0]}，按 path_id 索引            1R1W  软件逐项写                    复位 0    // 超前发送窗口，配在哪一张表原文未指明，见 F97
mem stream_table   FF 阵列   16 × {valid, user_id[15:0], user_id_vld, reissue, compute, task_id[5:0], task_fsm[2:0], done_bitmap[63:0], task_unit[1:0], task_dsa_en, task_pc[31:0], is_reissue, end}  6W1R  六个写口按固定优先级仲裁  复位空
mem stream_ptr     FF        {head_ptr[4:0], tail_ptr[4:0]}                                1RW   建表推 tail，退休推 head       复位 0
mem datain_hold    FF        1 项 {valid, task_pc[31:0], user_id[15:0], path_id[7:0]}      1RW   占住即反压 Router              复位空
mem credit_cnt     FF 阵列   每 {path_id, stream_id} 一个计数器                             1RW   广播初值 = 目的 core 数，P2P = 1  复位由输入给
mem p2p_buf_map    FF 阵列   每方向一张 × {p2p_vld, user_id[15:0], down_dir[2:0], data_addr[17:0]}  1RW  最多 3 个方向  复位空
mem p2p_dn_credit  FF 阵列   每方向对应下游 core 的 credit 计数器                            1RW   —                             复位由配置给
mem reduce_pend    FF 阵列   每 stream 一项 {reduce_num[5:0], issued[5:0], dte_ack_map[63:0], router_done_map[63:0]}  1RW  按 reduce_seq 逐位配对，低 N 位两张都满才置 FINISH，提交时清零  复位 0
mem 级间 latch      级间 latch 各模块之间的命令与状态                                        —     每拍覆写                      —
```

### 编译侧读入的表

`task_chain`、`sw_attr`、`path_task_map`、`datain_task`、`cfg_misc` 五张都由编译侧算好，boot 期经 ctrl_noc 按 F5 的顺序写入。另有一张 `credit_init`：每 `{path_id, stream_id}` 一个初值，广播任务等于目的 core 数量，P2P 任务是 1，每 core 一份。

`path_task_map` 是 `path_id` 与任务链的接口。一个 `path_id` 在一个 core 上最多对应一个 `task_id`；同一条 path 在不同 core 上落到不同的 `task_id`，因此这张表也是每 core 一份。

### 四种 core 级切分模式的任务链

bring-up 用 **EPTP-NN**，它每个 core 的任务链相同，是能跑通第一个 token 的最小实例；跑通后换 EPTP-NK 的三种 core 角色，再上两种 PPTP。

**EPTP-NN：每个 core 任务相同**

| task_id | unit | recv_unit | end | 内容 |
| - | - | - | - | - |
| 0 | DTE | dsa | 0 | token data in |
| 1 | MU | dsa | 0 | FC1 / FC3 计算 |
| 2 | VU | dsa | 0 | FC1 silu × FC3 加量化 |
| 3 | MU | dsa | 0 | FC2 计算加 EP Reduce |
| 4 | DTE | rmem | 1 | FC2 data out，在 Router 上完成 reduce |

**EPTP-NK：三种 core 角色**。一条 14 步的链，三种角色各取其中一部分，`TASK_VALID` 为 0 的项由 SKIP_MASK 跳过。

| task_id | unit | 内容 | Normal | Concat | Chip Reduce |
| - | - | - | - | - | - |
| 0 | DTE | token data in | ✔ | ✔ | ✔ |
| 1 | MU | FC1 / FC3 计算 | ✔ | ✔ | ✔ |
| 2 | DTE | FC1 / FC3 out，Router 上 reduce | ✔ | ✔ | ✔ |
| 3 | DTE | FC1 / FC3 reduce 结果 data in | | ✔ | ✔ |
| 4 | VU | FC1 silu × FC3 加量化 | | ✔ | ✔ |
| 5 | DTE | FC2 input data out（广播） | | ✔ | ✔ |
| 6 | DTE | FC2 input data in | ✔ | | |
| 7 | MU | FC2 计算加 EP Reduce | ✔ | ✔ | ✔ |
| 8 | DTE | FC2 data out（concat） | ✔ | | |
| 9 | DTE | 与 chip 内其他 core concat，只在 chip 内最后一个 core 上做 | | ✔ | ✔ |
| 10 | DTE | chip FC2 结果 data out（chip reduce） | | ✔ | |
| 11 | DTE | chip FC2 结果 data in | | | ✔ |
| 12 | VU | chip FC2 结果 reduce | | | ✔ |
| 13 | DTE | FC2 结果 data out | | | ✔ |

**PPTP 两种模式的共同点**：PP 把 FC1、FC3、FC2 摆在不同行的 chip 上，**silu、dot、量化三步统一落在 FC2 段 chip 的逻辑 core 0**。FC1 段与 FC3 段各自把本段的结果送到那一个 core，dot 完再由它把结果当作 FC2 的 token 在本 chip 内广播。原始文档另有两处把 dot 摆在 FC3 chip，本套文档不采用，冲突记在第 8 章。

**PPTP-NK**：FC1 / FC3 chip 内切 K，每 core 出的是部分和，要先 chip 内逐级 reduce；FC2 chip 内切 N，出的是 y 的一段，所以是 concat。

| 段 | task_id | unit | 内容 |
| - | - | - | - |
| FC1 段 | 0 | DTE | token data in |
| FC1 段 | 1 | MU | FC1 计算 |
| FC1 段 | 2 | DTE | FC1 部分和 out，Router 上逐级 reduce |
| FC1 段 | 3 | DTE | reduce 结果 data in（只在 chip 内最后一个 core 上） |
| FC1 段 | 4 | DTE | reduce 结果 out 到 FC2 段 chip 的逻辑 core 0 |
| FC3 段 | 0～4 | 同上 | 把 FC1 换成 FC3，其余相同 |
| FC2 段 core 0 | 5 | DTE | 收 FC1 段的 reduce 结果 |
| FC2 段 core 0 | 6 | DTE | 收 FC3 段的 reduce 结果 |
| FC2 段 core 0 | 7 | VU | silu · dot · 量化 |
| FC2 段 core 0 | 8 | DTE | 结果作为 FC2 的 token 在本 chip 内广播 |
| FC2 段各 core | 9 | DTE | FC2 token data in |
| FC2 段各 core | 10 | MU | FC2 计算加 EP Reduce |
| FC2 段各 core | 11 | DTE | FC2 结果 out（concat） |
| FC2 段 concat core | 12 | DTE | 与 chip 内其他 core concat |
| FC2 段 concat core | 13 | DTE | concat 结果 out，跨 chip reduce |

**PPTP-NN**：FC1 / FC3 chip 内也切 N，每 core 出的就是 y 的一段，chip 内做 concat 而不是 reduce；FC2 chip 内切 K，出的是部分和，所以是逐级 reduce。

| 段 | task_id | unit | 内容 |
| - | - | - | - |
| FC1 段 | 0 | DTE | token data in |
| FC1 段 | 1 | MU | FC1 计算 |
| FC1 段 | 2 | DTE | FC1 结果 out（concat） |
| FC1 段 concat core | 3 | DTE | 与 chip 内其他 core concat |
| FC1 段 concat core | 4 | DTE | concat 结果 out 到 FC2 段 chip 的逻辑 core 0 |
| FC3 段 | 0～4 | 同上 | 把 FC1 换成 FC3，其余相同 |
| FC2 段 core 0 | 5 | DTE | 收 FC1 段的 concat 结果 |
| FC2 段 core 0 | 6 | DTE | 收 FC3 段的 concat 结果 |
| FC2 段 core 0 | 7 | VU | silu · dot · 量化 |
| FC2 段 core 0 | 8 | DTE | 结果作为 FC2 的 token 在本 chip 内广播 |
| FC2 段各 core | 9 | DTE | FC2 token data in |
| FC2 段各 core | 10 | MU | FC2 计算加 EP Reduce |
| FC2 段各 core | 11 | DTE | FC2 部分和 out，Router 上逐级 reduce |
| FC2 段 reduce core | 12 | DTE | chip 内 reduce 结果 data in |
| FC2 段 reduce core | 13 | DTE | 结果 out，跨 chip reduce |

### 四类 core 的 task_chain 位域

`xxx` 表示由 kernel 决定的 `TASK_PC` 或 `path_id`。

**normal core**：`datain_task` 不用手动配，TS 自动把 `task_chain` 里的 datain 任务调度到 `datain_task`；这些任务统一标 `WAIT_WAKE = 1`。用 Core Mem 阻塞重传时要配 task 1'，它把数据从本 core 的 Core Mem 搬到下游 core。

| task | unit | valid | self_start | task_pc | wait_wake | recv_unit | end | B_reissue | P2P_reissue | path_id | credit_en | reduce_iss |
| - | - | - | - | - | - | - | - | - | - | - | - | - |
| 1　broadcast datain | DTE | 1 | 0 | xxx | 1 | dsa | 0 | 0 | 0 | xxx | 0 | 0 |
| 1'　broadcast reissue | DTE | 1 | 0 | xxx | 0 | dsa | 0 | 1 | 0 | xxx | 1 | 0 |
| 2　FC1 / FC3 gemm | MU | 1 | 0 | xxx | 0 | dsa | 0 | 0 | 0 | xxx | 0 | 0 |
| 3　FC1 silu dot FC3 | VU | 1 | 0 | xxx | 0 | dsa | 0 | 0 | 0 | xxx | 0 | 0 |
| 4　FC2 gemm | MU | 1 | 0 | xxx | 0 | dsa | 0 | 0 | 0 | xxx | 0 | 0 |
| 5　core 内 reduce | VU | 1 | 0 | xxx | 0 | dsa | 0 | 0 | 0 | xxx | 0 | 0 |
| 6　逐级 reduce | DTE | 1 | 0 | xxx | 0 | rmem | 0 | 0 | 0 | xxx | 1 | 1 |

**B core**：`datain_task` 必须手动配且只能是 DTE，代表持续从上游取数据进本 core 的 Matrix Mem。`task_chain` 里不可再配 DTE 的 datain 任务。task 0 循环查有没有 ready 的数据，`SELF_START = 1`；task 0 与 task 1 的 `TASK_EXE_MASK` 都是 1，表示不按用户区分。

| task | unit | valid | self_start | task_pc | wait_wake | dsa_en | end | B_reissue | P2P_reissue | path_id | credit_en | exe_mask |
| - | - | - | - | - | - | - | - | - | - | - | - | - |
| 0 | VU | 1 | 1 | xxx | 0 | 0 | 0 | 0 | 0 | xxx | 0 | 1 |
| 1 | DTE | 1 | 0 | xxx | 0 | 1 | 1 | 0 | 0 | xxx | 1 | 1 |

**R core**：`datain_task` 同样必须手动配且只能是 DTE。

| task | unit | valid | self_start | task_pc | wait_wake | dsa_en | end | B_reissue | P2P_reissue | path_id | credit_en |
| - | - | - | - | - | - | - | - | - | - | - | - |
| 0 | MU | 1 | 1 | xxx | 0 | 0 | 0 | 0 | 0 | xxx | 0 |
| 1 | DTE | 1 | 0 | xxx | 0 | 1 | 0 | 0 | 0 | xxx | 0 |
| 2 | VU | 1 | 0 | xxx | 0 | 1 | 0 | 0 | 0 | xxx | 0 |
| 3 | DTE | 1 | 0 | xxx | 0 | 1 | 1 | 0 | 0 | xxx | 1 |

**DP + P2P reissue core**：一个 core 要做四件事，透传上游数据、接收上游数据用于本 core 的 Norm、算 Norm、把结果发往下游。P2P 的 reissue 拆成 datain 与 dataout 两条任务。

| task | unit | valid | self_start | task_pc | wait_wake | exe_dest | end | B_reissue | P2P_reissue | path_id | credit_en | exe_mask |
| - | - | - | - | - | - | - | - | - | - | - | - | - |
| 0　P2P reissue datain | DTE | 1 | 0 | xxx | 1 | 1 | 0 | 0 | 1 | xxx | 1 | 0 |
| 1　P2P reissue dataout | DTE | 1 | 0 | xxx | 0 | 1 | 1 | 0 | 1 | xxx | 1 | 0 |
| 2　datain | DTE | 1 | 0 | xxx | 1 | 1 | 0 | 0 | 0 | xxx | 0 | 1 |
| 3　Norm | MU | 1 | 0 | xxx | 0 | 1 | 0 | 0 | 0 | xxx | 0 | 1 |
| 4　dataout | DTE | 1 | 0 | xxx | 0 | 1 | 0 | 0 | 0 | xxx | 1 | 1 |

***

## 5　流水线总览

TS 的三套时延数字口径不同：TS MAS 的 2～3 cycle 是硬件目标值，Top 模拟器的 16 T 是含 RV core 往返的端到端值，TS LLD 时序图给的是逐级拍数。第 1 层图按 LLD 那一套画，另两套在图下的短句里对上。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1010 522" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="artov" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="1010" height="522" fill="#ffffff"/>

  <text x="20" y="26" font-size="12" fill="#111827">TS · 第 1 层流水线总览（拍数取 TS LLD 时序图的逐级值）</text>
  <text x="20" y="42" font-size="9.5" fill="#6b7280">横向是级序，不是拍序；每级的拍数在右上角 Dx。橙色虚线框是变长级，非按比例。</text>
  <path d="M150 52 L150 70" stroke="#e5e7eb" fill="none"/>
<path d="M150 126 L150 156" stroke="#e5e7eb" fill="none"/>
<path d="M150 212 L150 242" stroke="#e5e7eb" fill="none"/>
<path d="M150 298 L150 328" stroke="#e5e7eb" fill="none"/>
<path d="M150 384 L150 414" stroke="#e5e7eb" fill="none"/>
  <path d="M316 52 L316 70" stroke="#e5e7eb" fill="none"/>
<path d="M316 126 L316 156" stroke="#e5e7eb" fill="none"/>
<path d="M316 212 L316 414" stroke="#e5e7eb" fill="none"/>
  <path d="M482 52 L482 156" stroke="#e5e7eb" fill="none"/>
<path d="M482 212 L482 414" stroke="#e5e7eb" fill="none"/>
  <path d="M648 52 L648 156" stroke="#e5e7eb" fill="none"/>
<path d="M648 212 L648 414" stroke="#e5e7eb" fill="none"/>
  <path d="M814 52 L814 156" stroke="#e5e7eb" fill="none"/>
<path d="M814 212 L814 414" stroke="#e5e7eb" fill="none"/>
  <text x="20" y="102" font-size="10.5" fill="#6b7280">建表</text>
  <rect x="150" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="84" font-size="8.5" fill="#6b7280">M1</text>
  <text x="292" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="104" font-size="11" fill="#111827">trigger 入队</text>
  <text x="160" y="118" font-size="11" fill="#111827">与 User_Match</text>
  <rect x="316" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="84" font-size="8.5" fill="#6b7280">M2</text>
  <text x="458" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D3</text>
  <text x="326" y="104" font-size="11" fill="#111827">CREATE 建表</text>
  <path d="M300 98 L315 98" stroke="#475569" marker-end="url(#artov)" fill="none"/>
  <text x="20" y="188" font-size="10.5" fill="#6b7280">推进</text>
  <rect x="150" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="170" font-size="8.5" fill="#6b7280">M3</text>
  <text x="292" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D2</text>
  <text x="160" y="190" font-size="11" fill="#111827">WAKE 判就绪</text>
  <rect x="316" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="170" font-size="8.5" fill="#6b7280">M4</text>
  <text x="458" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="190" font-size="11" fill="#111827">选最老发射</text>
  <path d="M300 184 L315 184" stroke="#475569" marker-end="url(#artov)" fill="none"/>
  <rect x="482" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="492" y="170" font-size="8.5" fill="#6b7280">M5</text>
  <text x="624" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="492" y="190" font-size="11" fill="#111827">ACCEPT</text>
  <text x="492" y="204" font-size="11" fill="#111827">回写 INFLY</text>
  <path d="M466 184 L481 184" stroke="#475569" marker-end="url(#artov)" fill="none"/>
  <rect x="648" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="658" y="170" font-size="8.5" fill="#6b7280">M6</text>
  <text x="790" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D3</text>
  <text x="658" y="190" font-size="11" fill="#111827">completion 七路合流</text>
  <path d="M632 184 L647 184" stroke="#475569" marker-end="url(#artov)" fill="none"/>
  <rect x="814" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="824" y="170" font-size="8.5" fill="#6b7280">M7</text>
  <text x="956" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D4</text>
  <text x="824" y="190" font-size="11" fill="#111827">INSTALL</text>
  <text x="824" y="204" font-size="11" fill="#111827">生成后继</text>
  <path d="M798 184 L813 184" stroke="#475569" marker-end="url(#artov)" fill="none"/>
  <text x="20" y="274" font-size="10.5" fill="#6b7280">退休</text>
  <rect x="150" y="242" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="256" font-size="8.5" fill="#6b7280">M8</text>
  <text x="292" y="256" font-size="8.5" fill="#6b7280" text-anchor="end">D6</text>
  <text x="160" y="276" font-size="11" fill="#111827">RETIRE</text>
  <text x="20" y="360" font-size="10.5" fill="#6b7280">credit</text>
  <rect x="150" y="328" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="342" font-size="8.5" fill="#6b7280">M9</text>
  <text x="292" y="342" font-size="8.5" fill="#6b7280" text-anchor="end">D2</text>
  <text x="160" y="362" font-size="11" fill="#111827">注册与唤醒</text>
  <text x="20" y="438" font-size="10.5" fill="#374151">一个 task 的调度间隔 16 T 是端到端值：M6 三拍加 M7 四拍加 M3 两拍加 M4 与 M5 各一拍，共 11 拍，余下的是 RV core 取指与回 ack 的往返。</text>
  <text x="20" y="466" font-size="10.5" fill="#374151">需与 Router 通信时的 21 T 多出的是 M9 的注册与等 Router 通知那一段。</text>
  <text x="20" y="494" font-size="10.5" fill="#374151">M1、M4、M5 的拍数设计未给，本轮各取 1 拍（待定）；M9 取 2 拍（待定）。</text>
</svg>
```

***

## 6　逐级行为

### M1 · trigger 入队与 User_Match

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 978 334" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="art1" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="978" height="334" fill="#ffffff"/>

  <polygon points="30,20 188,20 178,96 20,96" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="39" font-size="10.5" fill="#374151" text-anchor="middle">router2ts_trigger_ch</text>
  <text x="104" y="57" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · user_id[15:0]</text>
  <text x="104" y="75" font-size="9.5" fill="#6b7280" text-anchor="middle">path_id[7:0] · reissue</text>
  <text x="104" y="93" font-size="9.5" fill="#6b7280" text-anchor="middle">compute · ready</text>
  <rect x="20" y="108" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="112" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="129" font-size="10" fill="#374151" text-anchor="middle">path_task_map · FF 64 项 · 1R</text>
  <rect x="20" y="162" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="166" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="183" font-size="10" fill="#374151" text-anchor="middle">task_chain · FF 64 项 · 1R</text>
  <rect x="20" y="216" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="220" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="237" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF 16 项 · 1R</text>
  <rect x="782" y="72" width="176" height="136" fill="#f1f5f9" stroke="#334155"/>
  <rect x="782" y="72" width="176" height="18" fill="#334155"/>
  <text x="870" y="85" font-size="10.5" fill="#ffffff" text-anchor="middle">MATCH_CRE</text>
  <text x="870" y="112" font-size="10" fill="#334155" text-anchor="middle">is_new</text>
  <text x="870" y="134" font-size="10" fill="#334155" text-anchor="middle">user_id[15:0]</text>
  <text x="870" y="156" font-size="10" fill="#334155" text-anchor="middle">path_id[7:0]</text>
  <text x="870" y="178" font-size="10" fill="#334155" text-anchor="middle">task_id[5:0]</text>
  <text x="870" y="200" font-size="10" fill="#334155" text-anchor="middle">reissue · compute</text>
  <rect x="782" y="220" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="786" y="224" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="870" y="241" font-size="10" fill="#374151" text-anchor="middle">datain_hold · FF 1 项 · 1W</text>
  <rect x="232" y="78" width="506" height="178" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="94" font-size="8.5" fill="#6b7280">M1</text>
  <text x="724" y="94" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="114" font-size="12" fill="#111827">User_Match · 翻译 path_id 并判新老用户</text>
  <text x="250" y="136" font-size="10.5" fill="#475569">1. ready = 建表四条 &amp;&amp; !datain_hold.valid；!ready → CoreStation 保持请求</text>
  <text x="250" y="156" font-size="10.5" fill="#475569">2. t = path_task_map[path_id]；!t.valid → 记 path_id 匹配不到的异常并丢弃本笔</text>
  <text x="250" y="176" font-size="10.5" fill="#475569">3. hit = ∃i: stream_table[i].valid &amp;&amp; user_id 相同 → is_new = 0，只有 reissue 可写</text>
  <text x="250" y="196" font-size="10.5" fill="#475569">4. datain_hold.valid ? 拉低 ready 让 CoreStation 保持 : datain_hold = {t.task_id,</text>
  <text x="262" y="216" font-size="10.5" fill="#475569">task_chain[t.task_id].TASK_PC, user_id, path_id}</text>
  <text x="250" y="240" font-size="10" fill="#9ca3af">一条链上多个 datain 任务靠这一步分开</text>
  <line x1="188" y1="58" x2="228" y2="58" stroke="#475569" marker-end="url(#art1)"/>
  <path d="M188 129 L231 129" stroke="#475569" marker-end="url(#art1)" fill="none"/>
  <path d="M188 183 L231 183" stroke="#475569" marker-end="url(#art1)" fill="none"/>
  <path d="M188 237 L231 237" stroke="#475569" marker-end="url(#art1)" fill="none"/>
  <path d="M738 140 L781 140" stroke="#475569" marker-end="url(#art1)" fill="none"/>
  <path d="M738 241 L781 241" stroke="#475569" marker-end="url(#art1)" fill="none"/>
</svg>
```

### M2 · CREATE 建表

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 916 218" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="art2" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="916" height="218" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">MATCH_CRE</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">is_new</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">user_id[15:0]</text>
  <rect x="20" y="102" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="106" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="123" font-size="10" fill="#374151" text-anchor="middle">task_chain · FF 64 项 · 1R</text>
  <rect x="20" y="156" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="160" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="177" font-size="10" fill="#374151" text-anchor="middle">stream_ptr · FF · 1RW</text>
  <rect x="720" y="88" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="724" y="92" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="808" y="109" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF 16 项 · 1W（create 口）</text>
  <rect x="232" y="20" width="444" height="178" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M2</text>
  <text x="662" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D3</text>
  <text x="250" y="56" font-size="12" fill="#111827">Stream_table · 一次性写入整项</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. can = is_new &amp;&amp; !weights_mode &amp;&amp; trigger_task_chain_en</text>
  <text x="262" y="98" font-size="10.5" fill="#475569">&amp;&amp; (tail_ptr − head_ptr) &lt; stream_num</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">2. can → stream_table[tail_ptr] = {valid=1, user_id, reissue, compute,</text>
  <text x="262" y="138" font-size="10.5" fill="#475569">task_id=0, task_fsm=Task0 类型定, done_bitmap=0, task_chain[0] 的属性}</text>
  <text x="250" y="158" font-size="10.5" fill="#475569">3. can → tail_ptr += 1；!can → 拉低 ready，CoreStation 保持本笔，下拍重判</text>
  <text x="250" y="182" font-size="10" fill="#9ca3af">datain 或 reissue 起 TASK_WAIT，self_start 起 TASK_RDY</text>
  <path d="M188 55 L231 55" stroke="#475569" marker-end="url(#art2)" fill="none"/>
  <path d="M188 123 L231 123" stroke="#475569" marker-end="url(#art2)" fill="none"/>
  <path d="M188 177 L231 177" stroke="#475569" marker-end="url(#art2)" fill="none"/>
  <path d="M676 109 L719 109" stroke="#475569" marker-end="url(#art2)" fill="none"/>
</svg>
```

### M3 · WAKE 判就绪

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 895 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="art3" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="895" height="198" fill="#ffffff"/>

  <rect x="20" y="51" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="55" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="72" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF · 1R</text>
  <rect x="20" y="105" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="109" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="126" font-size="10" fill="#374151" text-anchor="middle">credit_cnt · FF · 1R</text>
  <rect x="699" y="78" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="703" y="82" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="787" y="99" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF · 1W（credit_wake 口）</text>
  <rect x="232" y="20" width="423" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M3</text>
  <text x="641" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D2</text>
  <text x="250" y="56" font-size="12" fill="#111827">Task_ctrl · 前序齐了且 credit 到位</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. dep_ok = (done_bitmap &amp; 前序 task 位) == 前序 task 位</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. cr_ok = !task_credit_en || credit_cnt[path_id][stream_id] 已满足</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. ready = dep_ok &amp;&amp; cr_ok；datain 被 trigger 后直接 ready</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. ready → task_fsm = TASK_READY（issue 口写一个字段）</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">就绪只有前序完成与 credit 到位两个来源</text>
  <path d="M188 72 L231 72" stroke="#475569" marker-end="url(#art3)" fill="none"/>
  <path d="M188 126 L231 126" stroke="#475569" marker-end="url(#art3)" fill="none"/>
  <path d="M655 99 L698 99" stroke="#475569" marker-end="url(#art3)" fill="none"/>
</svg>
```

### M4 · 选最老发射

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 962 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="art4" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="962" height="198" fill="#ffffff"/>

  <rect x="20" y="24" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="28" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="45" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF · 1R</text>
  <rect x="20" y="78" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="82" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="99" font-size="10" fill="#374151" text-anchor="middle">stream_ptr · FF · 1R</text>
  <rect x="20" y="132" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="136" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="153" font-size="10" fill="#374151" text-anchor="middle">datain_hold · FF 1 项 · 1R</text>
  <polygon points="776,60 942,60 932,136 766,136" fill="#f8fafc" stroke="#374151"/>
  <text x="854" y="79" font-size="10.5" fill="#374151" text-anchor="middle">task_cmd[u]</text>
  <text x="854" y="97" font-size="9.5" fill="#6b7280" text-anchor="middle">cmd_valid · task_pc[31:0]</text>
  <text x="854" y="115" font-size="9.5" fill="#6b7280" text-anchor="middle">stream_id[3:0] · task_id[5:0]</text>
  <text x="854" y="133" font-size="9.5" fill="#6b7280" text-anchor="middle">user_id[15:0] · cmd_ready</text>
  <rect x="232" y="20" width="490" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M4</text>
  <text x="708" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">三条 Arb · 环形年龄优先各选一个</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. cand[u] = {i | valid &amp;&amp; task_fsm==READY &amp;&amp; task_unit==u}</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. DTE：先取 is_reissue 的最老者，否则 DataIn 与 Generated 比年龄</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. age(i) = (i − head_ptr) mod 16，取最小</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. task_cmd[u] = {task_pc, stream_id, task_id, user_id, path_id, dsa_en}</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">选中后非抢占保持，命令字段到 ACCEPT 前不变</text>
  <path d="M188 45 L231 45" stroke="#475569" marker-end="url(#art4)" fill="none"/>
  <path d="M188 99 L231 99" stroke="#475569" marker-end="url(#art4)" fill="none"/>
  <path d="M188 153 L231 153" stroke="#475569" marker-end="url(#art4)" fill="none"/>
  <path d="M722 98 L770 98" stroke="#475569" marker-end="url(#art4)" fill="none"/>
</svg>
```

### M5 · ACCEPT 回写

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 867 180" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="art5" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="867" height="180" fill="#ffffff"/>

  <polygon points="30,69 188,69 178,109 20,109" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="88" font-size="10.5" fill="#374151" text-anchor="middle">task_cmd[u]</text>
  <text x="104" y="106" font-size="9.5" fill="#6b7280" text-anchor="middle">cmd_valid · cmd_ready</text>
  <rect x="671" y="42" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="675" y="46" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="759" y="63" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF · 1W（issue 口）</text>
  <rect x="671" y="96" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="675" y="100" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="759" y="117" font-size="10" fill="#374151" text-anchor="middle">datain_hold · FF · 1W</text>
  <rect x="232" y="20" width="395" height="140" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M5</text>
  <text x="613" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">三条 Arb · 按类型分别收尾</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. accept = cmd_valid &amp;&amp; cmd_ready（raw ACCEPT）</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. Generated → stream_table[i].task_fsm = TASK_INFLY</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. DataIn → datain_hold.valid = 0，不改 stream_table 的当前状态</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. 写未被接受时只重试这一笔字段写，不重新下发已被接收的 task</text>
  <path d="M188 89 L231 89" stroke="#475569" marker-end="url(#art5)" fill="none"/>
  <path d="M627 63 L670 63" stroke="#475569" marker-end="url(#art5)" fill="none"/>
  <path d="M627 117 L670 117" stroke="#475569" marker-end="url(#art5)" fill="none"/>
</svg>
```

### M6 · completion 七路合流

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 912 238" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="art6" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="912" height="238" fill="#ffffff"/>

  <polygon points="30,35 188,35 178,93 20,93" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="54" font-size="10.5" fill="#374151" text-anchor="middle">rv_done[u] · dsa_done[u]</text>
  <text x="104" y="72" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · stream_id[3:0]</text>
  <text x="104" y="90" font-size="9.5" fill="#6b7280" text-anchor="middle">task_id[5:0] · user_id</text>
  <polygon points="30,105 188,105 178,145 20,145" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="124" font-size="10.5" fill="#374151" text-anchor="middle">rmem2ts_done_ch</text>
  <text x="104" y="142" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · user_id[15:0]</text>
  <rect x="20" y="157" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="161" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="178" font-size="10" fill="#374151" text-anchor="middle">reduce_pend · FF 每 stream 1 项 · 1RW</text>
  <rect x="716" y="98" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="720" y="102" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="804" y="119" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF · 1W（completion 口）</text>
  <rect x="232" y="20" width="440" height="198" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M6</text>
  <text x="658" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D3</text>
  <text x="250" y="56" font-size="12" fill="#111827">Task_done · 按 task_recv_type 判哪一路算数</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. 六路 ack 按 stream_id 定位表项，Reduce Done 按 user_id 定位</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. recv_type=RV → rv_done 到即完成；=DSA → dsa_done 到即完成；</text>
  <text x="262" y="118" font-size="10.5" fill="#475569">=两者 → 两个都到才完成</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">3. reduce task：dte_ack_cnt += 1（consume_only），router_done_cnt += 1</text>
  <text x="250" y="158" font-size="10.5" fill="#475569">4. done_bitmap[task_id] 无条件置位；task_id == 当前 task_id 才改 task_fsm</text>
  <text x="262" y="178" font-size="10.5" fill="#475569">自启动 core：把 rv_done.user_id 写进该 stream 的同名字段</text>
  <text x="250" y="202" font-size="10" fill="#9ca3af">七路都是脉冲，本级永远就绪，不向上游反压</text>
  <path d="M188 64 L231 64" stroke="#475569" marker-end="url(#art6)" fill="none"/>
  <path d="M188 125 L231 125" stroke="#475569" marker-end="url(#art6)" fill="none"/>
  <path d="M188 178 L231 178" stroke="#475569" marker-end="url(#art6)" fill="none"/>
  <path d="M672 119 L715 119" stroke="#475569" marker-end="url(#art6)" fill="none"/>
</svg>
```

### M7 · INSTALL 生成后继

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 905 218" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="art7" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="905" height="218" fill="#ffffff"/>

  <rect x="20" y="34" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="38" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="55" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF · 1R</text>
  <rect x="20" y="88" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="92" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="109" font-size="10" fill="#374151" text-anchor="middle">task_masks · FF 五张 · 1R</text>
  <rect x="20" y="142" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="146" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="163" font-size="10" fill="#374151" text-anchor="middle">task_chain · FF 64 项 · 1R</text>
  <rect x="709" y="88" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="713" y="92" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="797" y="109" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF · 1W（install 口）</text>
  <rect x="232" y="20" width="433" height="178" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M7</text>
  <text x="651" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D4</text>
  <text x="250" y="56" font-size="12" fill="#111827">Task_ctrl · 一拍跳过再原子安装</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. 候选：valid &amp;&amp; task_fsm==FINISH &amp;&amp; end==0，从 head_ptr 环形扫</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. GROUP_SKIP = compute ? 0 : ~EXE_MASK</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. SKIP_MASK = ~END_MASK &amp; ((DATA_IN_MASK &amp; done_bitmap)</text>
  <text x="262" y="138" font-size="10.5" fill="#475569">| (REISSUE_MASK &amp; ~reissue) | GROUP_SKIP)</text>
  <text x="250" y="158" font-size="10.5" fill="#475569">4. next_id = PriorityEncode(~SKIP_MASK &amp; task_id 之后的位)，原子写整项</text>
  <text x="250" y="182" font-size="10" fill="#9ca3af">End task 即使已完成也不跳过，且不再生成后继</text>
  <path d="M188 55 L231 55" stroke="#475569" marker-end="url(#art7)" fill="none"/>
  <path d="M188 109 L231 109" stroke="#475569" marker-end="url(#art7)" fill="none"/>
  <path d="M188 163 L231 163" stroke="#475569" marker-end="url(#art7)" fill="none"/>
  <path d="M665 109 L708 109" stroke="#475569" marker-end="url(#art7)" fill="none"/>
</svg>
```

### M8 · RETIRE

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 843 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="art8" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="843" height="198" fill="#ffffff"/>

  <rect x="20" y="51" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="55" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="72" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF · 1R</text>
  <rect x="20" y="105" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="109" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="126" font-size="10" fill="#374151" text-anchor="middle">stream_ptr · FF · 1RW</text>
  <polygon points="657,42 823,42 813,100 647,100" fill="#f8fafc" stroke="#374151"/>
  <text x="735" y="61" font-size="10.5" fill="#374151" text-anchor="middle">ts2router_retire</text>
  <text x="735" y="79" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · user_id[15:0]</text>
  <text x="735" y="97" font-size="9.5" fill="#6b7280" text-anchor="middle">accepted</text>
  <rect x="647" y="112" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="651" y="116" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="735" y="133" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF · 1W（retirement 口）</text>
  <rect x="232" y="20" width="371" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M8</text>
  <text x="589" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D6</text>
  <text x="250" y="56" font-size="12" fill="#111827">retire · 先还 credit 再清 valid</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. 条件：i == head_ptr &amp;&amp; valid &amp;&amp; end &amp;&amp; task_fsm==FINISH</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. ts2router_retire = {valid=1, user_id}</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. accepted → stream_table[i].valid = 0；head_ptr += 1</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. !accepted → 保持 valid 与请求，head_ptr 不动</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">只允许队头退休，head_ptr 才能单调推进</text>
  <path d="M188 72 L231 72" stroke="#475569" marker-end="url(#art8)" fill="none"/>
  <path d="M188 126 L231 126" stroke="#475569" marker-end="url(#art8)" fill="none"/>
  <path d="M603 71 L651 71" stroke="#475569" marker-end="url(#art8)" fill="none"/>
  <path d="M603 133 L646 133" stroke="#475569" marker-end="url(#art8)" fill="none"/>
</svg>
```

### M9 · credit 注册与唤醒

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 878 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="art9" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="878" height="198" fill="#ffffff"/>

  <rect x="20" y="51" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="55" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="72" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF · 1R</text>
  <rect x="20" y="105" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="109" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="126" font-size="10" fill="#374151" text-anchor="middle">reduce_pend · FF · 1RW</text>
  <polygon points="692,33 858,33 848,109 682,109" fill="#f8fafc" stroke="#374151"/>
  <text x="770" y="52" font-size="10.5" fill="#374151" text-anchor="middle">ts2router_req</text>
  <text x="770" y="70" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · user_id[15:0]</text>
  <text x="770" y="88" font-size="9.5" fill="#6b7280" text-anchor="middle">stream_id[3:0] · task_id[5:0]</text>
  <text x="770" y="106" font-size="9.5" fill="#6b7280" text-anchor="middle">req_ready</text>
  <polygon points="692,121 858,121 848,161 682,161" fill="#f8fafc" stroke="#374151"/>
  <text x="770" y="140" font-size="10.5" fill="#374151" text-anchor="middle">router2ts_credit_ch</text>
  <text x="770" y="158" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · stream_id[3:0]</text>
  <rect x="232" y="20" width="406" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M9</text>
  <text x="624" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D2</text>
  <text x="250" y="56" font-size="12" fill="#111827">credit · 向 Router 要资源</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. reduce task → 顺序连续下发 reduce_num 笔注册请求</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. ts2router_req = {user_id, stream_id, task_id, path_id}</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. req_ready=0 → 本笔保持，不发下一笔</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. router2ts_credit_ch 到 → 对应 task 走 M3 的 cr_ok 分支置 READY</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">credit 的单位是 stream，同一 stream 只在第一次发数据时查</text>
  <path d="M188 72 L231 72" stroke="#475569" marker-end="url(#art9)" fill="none"/>
  <path d="M188 126 L231 126" stroke="#475569" marker-end="url(#art9)" fill="none"/>
  <path d="M638 71 L686 71" stroke="#475569" marker-end="url(#art9)" fill="none"/>
  <path d="M638 141 L686 141" stroke="#475569" marker-end="url(#art9)" fill="none"/>
</svg>
```

***

## 7　参数汇总

```
task_chain          64 项 × 64 bit
path_task_map       64 项，按 path_id 索引出 task_id
path_flowctl        64 项，按 path_id 索引出 {flowctl_en, window_n}
datain_task         1 项
DataIn_task_table   1 项（Depth-1 Hold）
stream_table        16 项，注册与释放深度各 1；stream_num 软件配 1～16；自启动数等于 stream_num
六个写口优先级       retirement > completion > install > issue > credit_wake > create
done_bitmap         64 位，对应 64 个 task
DTE / MU / VU Arb   发射宽度各 1
task_fsm            五态 IDLE / WAIT / READY / INFLY / FINISH
P2P 阻塞缓冲         方向数 ≤ 3，容量与项数软件配
超前发送窗口 N       编译期确定，软件按 path 配；使能位与 N 都在 CFG_REG（待定，见 F97）
task 唤醒延迟        2～3 cycle（硬件目标值）
调度间隔            16 T；需与 Router 通信时 21 T；retire 5 T（端到端建模值）
```

***

## 8　机制覆盖

| 机制 | 功能 | 用例 |
| - | - | - |
| task_chain 64 项，写一项自动置 VALID | F1、F2 | `task_chain_write` |
| 配置下发顺序与写 TS_INIT_FINISH 后的五项检查 | F5、F6 | `ts_init_check` |
| 四张掩码是 task_chain 的纯函数，运行时只查掩码与两个用户级标记 | F7、F9 | `derive_masks` |
| compute = 0 的用户跳过所有 TASK_EXE_MASK = 0 的 task | F8、F42 | `group_skip` |
| path_task_map 把 path_id 翻译成 task_id，三处用它 | F10、F11、F12 | `path_task_map` |
| 一条链上多个 datain 任务靠 path_id 分开取各自的 task_pc | F27 | `multi_datain_by_path` |
| datain 完成时按查出的 task_id 点亮 done_bitmap，不推进 task_id | F82 | `datain_done_bit` |
| 软件侧属性与硬件位域分开存 | F13 | `sw_attr_separate` |
| Router 经 router2ts_trigger_ch 直接通知 TS，不经软件 | F15 | `trigger_entry` |
| 老用户复用 stream_id，只可能改 reissue | F17 | `user_match_reuse` |
| 建表四条同时满足 | F18 | `create_conditions` |
| trigger 口不设队列，条件不满足直接反压 Router，不丢 trigger | F19、F20、F22 | `trigger_backpressure` |
| 建表一次性写入用户信息与 Task 0 的全部属性 | F21 | `create_atomic` |
| DataIn_task_table 只有 1 项，占住就反压 Router | F23、F24 | `datain_hold` |
| B core 与 R core 上进来的包不建 stream 表项，只登记 DATAIN_TASK 那一项 | F27、F28 | `selfstart_datain` |
| 16 项顺序 FIFO，进度靠三个字段合起来判断 | F29、F31 | `stream_table_progress` |
| 用户号只有 user_id 一个，两条写入路径写同一个字段 | F30 | `one_user_id` |
| 六个写口的固定优先级与两种失败处理 | F35、F36 | `six_write_ports` |
| completion 口无条件置 done_bitmap，只在是当前 task 时改 fsm | F37 | `completion_fsm_guard` |
| credit_wake 对老用户同时置 reissue | F38 | `credit_wake_reissue` |
| 自启动的表项经 completion 口补写 user_id | F86 | `self_start_writeback` |
| SKIP_MASK 一拍跳过，连续 skip 不增加周期 | F41、F43 | `skip_mask` |
| End task 即使已完成也不能跳，且不再生成后继 | F44 | `end_task_no_skip` |
| 后继任务的全部字段一起原子写入 | F45 | `install_atomic` |
| 异步 datain 只更新 done_bitmap，不推进 task_id | F48 | `datain_no_advance` |
| datain 的执行时刻由数据到达定，不受链序推后；数据晚到时等的是主线 | F48 | `datain_timing` |
| DTE 三类任务的优先级 | F50、F51 | `dte_arb_priority` |
| 选中后非抢占保持到 raw ACCEPT | F52 | `non_preemptive` |
| ACCEPT 后 Generated 提交 INFLY，DataIn 只出槽 | F53 | `accept_handling` |
| 三条发射通路同一拍可并行下发 3 个 task | F58 | `parallel_issue` |
| credit 粒度是 stream，首次查 stream credit、之后走发送就绪门控 | F61、F62、F98 | `credit_per_stream` |
| Broadcast Reissue：置标记、查 credit、选最老、并行于主链 | F64、F55 | `broadcast_reissue` |
| 未重发成功前原始数据不能被覆盖，用户不能释放 | F65 | `reissue_hold_data` |
| P2P 重发拆成 datain 与 dataout 两个 task | F66 | `p2p_reissue` |
| P2P 阻塞缓冲的四项配置与映射表 | F67、F68 | `p2p_block_buffer` |
| reduce_num = N 时顺序连续下发 N 笔 credit 请求 | F69 | `reduce_credit_n` |
| Head-only 退休，先还 credit 再清 valid | F70、F71 | `head_only_retire` |
| 广播 CreditCounter 初值 = 目的 core 数，一次扣全部 | F73 | `broadcast_credit_init` |
| 七路完成合流，按 task_recv_type 判定 | F75、F76 | `task_done_seven` |
| Reduce 完成拆两半，只有 Router Done 能置 FINISH | F77、F78 | `reduce_done_split` |
| reduce_num = N 时按 reduce_seq 逐包配对，两张位图都满才置 FINISH | F79、F80 | `reduce_done_pair_by_seq` |
| Router 不带 stream_id，按 user_id 找 Stream | F81 | `done_by_user_id` |
| B core / R core 复位后自启动 stream_num 个表项 | F14、F84、F85 | `self_start_count` |
| check flag 的 task 不调度 DSA | F88 | `check_flag_no_dsa` |
| 一个 stream 的任务链完成后自发创建新任务链 | F91 | `self_recreate_chain` |
| B core 搬出按 B_CORE_DIRECTION 查下游 credit，落 MM 不查 | F72 | `bcore_direction_credit` |
| 超前发送窗口：正在算 user M 时阻塞 user ≥ M+N 的传输 | F94、F95 | `lookahead_window` |
| 窗口只在 TP / PPTP 内部生效 | F96 | `lookahead_scope` |

***

## 9　取舍

* **为什么 Reduce 的完成要拆成两个事件**
  * DTE 把数据搬到 Router 就结束了自己的活，但 reduce 是在 Router 的 Rmem 里做的，此时结果还没算出来
  * 若让 DTE ack 直接结束这个 task，后继任务会读到还没归约完的数据
* **为什么 datain 不推进 task_id**
  * 推进 `task_id` 意味着承认任务链走到了那一步，而异步 datain 的到达顺序与链序无关
  * 只落 `done_bitmap` 的一位，就把“这一步的数据齐了”与“任务链走到了这一步”分开记，两者在 Task_ctrl 生成后继时才合到一起判断
* **为什么退休只允许 head**
  * stream_table 是顺序 FIFO，credit 也按 stream 记账
  * 只让队头退休，`head_ptr` 才能单调推进，年龄比较才有确定的基准。代价是队头卡住时后面已经完成的 stream 也得等着
* **为什么先还 credit 再清 valid**
  * 反过来做，槽位已经放开而上游还没拿到 credit，中间这段时间上游发来的数据在本 core 没有落脚点
* **为什么用逐跳 credit 而不是端到端**
  * 端到端 credit 要经过整条路径的往返延迟才能释放，路径长到一定程度，16 个 stream 的执行耗时盖不住这个延迟，就会出空泡
  * 逐跳把一段长往返切成每跳各自的短往返，稳态吞吐与跳数无关，代价是要处理中途的阻塞
* **为什么 B core 与 R core 要双任务链加软件映射表**
  * 收发绑在一条链上时，能缓的用户数被 stream_table 卡在 16，而它们的用户数远超 16
  * 拆开的是“在 TS 里占一个 stream 项”与“数据在本核停留”这两段时间：在途用户上限换成 Matrix Mem 的容量，推进顺序换成软件表说了算
* **为什么做成硬化调度器**
  * 各场景的 core 内任务流固定、任务类型不多，硬化足够覆盖
  * 代价是任务流的形状被写死在 64 项的表里，换场景要重配；换来的是调度延时压到 2～3 拍
