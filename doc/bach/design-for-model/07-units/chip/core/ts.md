# TS 任务调度器

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **TS**

给实现 TS 的人：九个独立打拍的模块各自做哪些事、端口与存储怎么定。

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

边界是五组接口：与 Router 的五组信号、与三个 RV core 的 task 下发与 ack、六路 DSA 与 RV core 的完成、ctrl_noc 的配置口、异常上报口。

四种工作模式由两个配置项选定，上电配好之后运行期间不变：

| 模式 | 怎么进入 | TS 在这个模式下做什么 |
| - | - | - |
| weights 加载 | `WEIGHTS_MODE = 1` | 不启动 task_chain。只有 datain_task 工作，Router 来了数据就派给 DTE 搬运，不建 stream 表项、不推进任何任务链 |
| 普通计算 core | `CORE_TYPE` = 普通，`WEIGHTS_MODE = 0` | 完整的四个动作：Router trigger 建表、按 task_chain 逐 task 推进、三条通路发射、完成后推进度并在链尾退休 |
| B core | `CORE_TYPE` = B core | 复位后直接自启动 16 个表项，不等 Router trigger。datain 收到的数据不建表，完成标志由软件维护在 Share Mem 里；task 0 循环查那个标志，查到才往下走 |
| R core | `CORE_TYPE` = R core | 同 B core 的自启动与双链结构，区别在 task 0 查的是“两笔数据是否集齐”，推进顺序由软件映射表决定，不是 TS 的年龄优先 |

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1630 1120" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
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
  <rect x="0" y="0" width="1630" height="1120" fill="#ffffff"/>
  <text x="20" y="26" font-size="12" fill="#111827">TS 任务调度器 · 第 0 层（九个独立打拍的模块）</text>
  <text x="317" y="26" font-size="9.5" fill="#6b7280">绿线 = 与 Router 的控制通路　灰线 = TS 内部与 RV core 的下发 / 完成　橙线 = credit 与 retire　紫虚线 = ctrl_noc 配置</text>
  <polygon points="36,104 204,104 195,134 27,134" fill="#f8fafc" stroke="#374151"/>
  <text x="116" y="123" font-size="8.5" fill="#374151" text-anchor="middle">router2ts_trigger_ch</text>
  <polygon points="36,648 204,648 195,678 27,678" fill="#f8fafc" stroke="#374151"/>
  <text x="116" y="667" font-size="8.5" fill="#374151" text-anchor="middle">router2ts_credit_ch</text>
  <polygon points="36,700 204,700 195,730 27,730" fill="#f8fafc" stroke="#374151"/>
  <text x="116" y="719" font-size="8.5" fill="#374151" text-anchor="middle">rmem2ts_done_ch</text>
  <rect x="258" y="84" width="300" height="146" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="270" y="105" font-size="11" fill="#111827">User_Match</text>
  <text x="270" y="122" font-size="8.5" fill="#475569">拿请求里的 user_id 与 stream_table 比对</text>
  <text x="270" y="135.5" font-size="8.5" fill="#475569">没匹配上 → 新用户，发建表请求</text>
  <text x="270" y="149.0" font-size="8.5" fill="#475569">匹配上 → 老用户，复用原 stream_id</text>
  <text x="270" y="162.5" font-size="8.5" fill="#475569">　当前任务、状态、完成位一律不动</text>
  <text x="270" y="176.0" font-size="8.5" fill="#475569">　只有带重发标记时才置 reissue</text>
  <text x="270" y="189.5" font-size="8.5" fill="#475569">建表四条同时满足：新用户 · 不在 weights 模式</text>
  <text x="270" y="203.0" font-size="8.5" fill="#475569">　· trigger_task_chain_en · tail−head &lt; stream_num</text>
  <rect x="258" y="262" width="300" height="126" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="270" y="283" font-size="11" fill="#111827">DataIn_task_table</text>
  <text x="270" y="300" font-size="8.5" fill="#475569">只有 1 项（Depth-1 Hold）</text>
  <text x="270" y="313.5" font-size="8.5" fill="#475569">空闲时把 datain 任务与请求信息一起登记</text>
  <text x="270" y="327.0" font-size="8.5" fill="#475569">被占住时反压 Router 的新请求</text>
  <text x="270" y="340.5" font-size="8.5" fill="#475569">仲裁成功并被 DTE RV core 接收后立即释放</text>
  <text x="270" y="354.0" font-size="8.5" fill="#475569">datain 的 task_pc 进这里，Task 0 的进 stream_table</text>
  <text x="270" y="367.5" font-size="8.5" fill="#475569">B core 与 R core 的 datain 任务不建表</text>
  <rect x="618" y="84" width="430" height="304" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="630" y="105" font-size="11" fill="#111827">Stream_table</text>
  <text x="630" y="122" font-size="8.5" fill="#475569">16 项顺序 FIFO，head_ptr 与 tail_ptr 环形推进</text>
  <text x="630" y="135.5" font-size="8.5" fill="#475569"></text>
  <text x="630" y="149.0" font-size="8.5" fill="#475569">用户级标记（建表写入，整链期间基本不动）：</text>
  <text x="630" y="162.5" font-size="8.5" fill="#475569">　valid · user_id · reissue · compute</text>
  <text x="630" y="176.0" font-size="8.5" fill="#475569">进度：</text>
  <text x="630" y="189.5" font-size="8.5" fill="#475569">　task_id · task_fsm · done_bitmap（64 位对应 64 个 task）</text>
  <text x="630" y="203.0" font-size="8.5" fill="#475569">　异步 datain 提前完成 = 某位先亮而 task_id 还没走到</text>
  <text x="630" y="216.5" font-size="8.5" fill="#475569">当前 task 的属性（随 task_id 索引 task_chain 得到）：</text>
  <text x="630" y="230.0" font-size="8.5" fill="#475569">　task_unit · task_dsa_en · task_pc · is_reissue · end</text>
  <text x="630" y="243.5" font-size="8.5" fill="#475569"></text>
  <text x="630" y="257.0" font-size="8.5" fill="#475569">task_fsm 五态：IDLE → WAIT → READY → INFLY → FINISH</text>
  <text x="630" y="270.5" font-size="8.5" fill="#475569">六个写口在此仲裁，每口一拍一笔，请求保持到 accepted</text>
  <text x="630" y="284.0" font-size="8.5" fill="#475569">　整项写入失败要重读最新表内容再来</text>
  <text x="630" y="297.5" font-size="8.5" fill="#475569">　单字段写失败只重试这一笔，不重发已被接收的任务</text>
  <text x="1036" y="379" font-size="8.5" fill="#9ca3af" text-anchor="end">stream_num 可配 1～16</text>
  <rect x="1108" y="84" width="450" height="214" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="1120" y="105" font-size="11" fill="#111827">Task_ctrl</text>
  <text x="1120" y="122" font-size="8.5" fill="#475569">当前任务进 TASK_FINISH 后才生成后继</text>
  <text x="1120" y="135.5" font-size="8.5" fill="#475569">每个 stream 独立推进，不需要全局 Task Pointer</text>
  <text x="1120" y="149.0" font-size="8.5" fill="#475569">从 head_ptr 环形扫描，只选 valid=1 且 FINISH 且 end=0</text>
  <text x="1120" y="162.5" font-size="8.5" fill="#475569">一次 64 bit 优先编码，一拍跳过所有可跳过的 task：</text>
  <text x="1120" y="176.0" font-size="8.5" fill="#475569">　SKIP_MASK = ~END_MASK &amp; ( (DATA_IN_MASK &amp; done_bitmap)</text>
  <text x="1120" y="189.5" font-size="8.5" fill="#475569">　　　　　　　　　　　| (REISSUE_MASK &amp; ~stream.reissue) )</text>
  <text x="1120" y="203.0" font-size="8.5" fill="#475569">连续 skip 不增加周期</text>
  <text x="1120" y="216.5" font-size="8.5" fill="#475569">End task 即使已提前完成也不能跳，且不再生成后继</text>
  <text x="1120" y="230.0" font-size="8.5" fill="#475569">新任务的 task_id · task_fsm · end 与全部下发属性一起原子写入</text>
  <text x="1120" y="243.5" font-size="8.5" fill="#475569">初始状态：Generated → READY，DataIn / Reissue → WAIT</text>
  <rect x="258" y="430" width="300" height="196" fill="#f5f3ff" stroke="#7c3aed" rx="4"/>
  <text x="270" y="451" font-size="11" fill="#111827">CFG_REG</text>
  <text x="270" y="468" font-size="8.5" fill="#475569">task_chain 64 项 × 64 bit（写一项自动置 VALID）</text>
  <text x="270" y="481.5" font-size="8.5" fill="#475569">datain_task 1 项：task_pc · task_unit · weights_mode</text>
  <text x="270" y="495.0" font-size="8.5" fill="#475569">stream_num 1～16 · CORE_TYPE · B_CORE_DIRECTION</text>
  <text x="270" y="508.5" font-size="8.5" fill="#475569">trigger_task_chain_en · TS_INIT_FINISH · TS_STATE</text>
  <text x="270" y="522.0" font-size="8.5" fill="#475569">写 TS_INIT_FINISH 后查五项合规性，结论写 TS_STATE</text>
  <text x="270" y="535.5" font-size="8.5" fill="#475569">并派生三张 64 位掩码供 Task_ctrl 一拍算 SKIP_MASK：</text>
  <text x="270" y="549.0" font-size="8.5" fill="#475569">　DATA_IN_MASK · REISSUE_MASK · END_MASK</text>
  <text x="270" y="562.5" font-size="8.5" fill="#475569">Task LUT：按 task_id 查出下发属性</text>
  <text x="270" y="576.0" font-size="8.5" fill="#475569">软件侧属性另存：exe_dest · task_group_id · reduce_num · dsa_en</text>
  <rect x="618" y="430" width="430" height="196" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="630" y="451" font-size="11" fill="#111827">Credit_monitor</text>
  <text x="630" y="468" font-size="8.5" fill="#475569">向 Router 注册资源申请：UserID · StreamID · TaskID · PathID</text>
  <text x="630" y="481.5" font-size="8.5" fill="#475569">收 Router 的 credit 到手通知，唤醒对应 task 置 READY</text>
  <text x="630" y="495.0" font-size="8.5" fill="#475569">credit 粒度是 stream 不是 task；同一 stream 的任务链里</text>
  <text x="630" y="508.5" font-size="8.5" fill="#475569">　只在第一次往下游发数据时检查，之后不再检查</text>
  <text x="630" y="522.0" font-size="8.5" fill="#475569">reduce task：按 reduce_num 顺序连续下发 N 笔 credit 请求</text>
  <text x="630" y="535.5" font-size="8.5" fill="#475569">Head-only 退休：只有 head_ptr 指向的项可退休，条件是</text>
  <text x="630" y="549.0" font-size="8.5" fill="#475569">　valid=1 且 end=1 且 task_fsm=FINISH</text>
  <text x="630" y="562.5" font-size="8.5" fill="#475569">先向 Router 持续发 credit 返还请求，Router 接收后</text>
  <text x="630" y="576.0" font-size="8.5" fill="#475569">　才清该槽位的 valid 并推进 head_ptr</text>
  <text x="630" y="589.5" font-size="8.5" fill="#475569">B core 的搬出 task 按 B_CORE_DIRECTION 查下游 credit</text>
  <rect x="1108" y="430" width="450" height="196" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="1120" y="451" font-size="11" fill="#111827">Task_done</text>
  <text x="1120" y="468" font-size="8.5" fill="#475569">七路完成合流，直接写 stream_table，不设统一 Update 模块</text>
  <text x="1120" y="481.5" font-size="8.5" fill="#475569">　DTE / MU / VU 各有 RV core ack 与 DSA ack，共六路</text>
  <text x="1120" y="495.0" font-size="8.5" fill="#475569">　DTE 这条 Lane 额外接收 Router 的 Reduce Done</text>
  <text x="1120" y="508.5" font-size="8.5" fill="#475569">按当前任务的 task_recv_type 判哪一路才算数：</text>
  <text x="1120" y="522.0" font-size="8.5" fill="#475569">　00 = 只调 RV core / 01 = 调 DSA / 10 = DTE DSA + Rmem 两者都要</text>
  <text x="1120" y="535.5" font-size="8.5" fill="#475569">Reduce 拆两半：DTE ack 只 consume_only，不改 stream 状态；</text>
  <text x="1120" y="549.0" font-size="8.5" fill="#475569">　只有 Router Reduce Done 才能置 TASK_FINISH</text>
  <text x="1120" y="562.5" font-size="8.5" fill="#475569">　两者可任意顺序；Router Done 可被 Hold，但要等匹配的</text>
  <text x="1120" y="576.0" font-size="8.5" fill="#475569">　DTE ack 被消费后才提交</text>
  <text x="1120" y="589.5" font-size="8.5" fill="#475569">Router 不携带 stream_id，按 user_id 找对应 Stream</text>
  <rect x="258" y="800" width="390" height="206" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="270" y="821" font-size="11" fill="#111827">DTE_Arb</text>
  <text x="270" y="838" font-size="8.5" fill="#475569">候选：valid=1 且 task_fsm=READY 且 task_unit=DTE</text>
  <text x="270" y="851.5" font-size="8.5" fill="#475569">三类任务的优先级：</text>
  <text x="270" y="865.0" font-size="8.5" fill="#475569">　1. Reissue 任务优先级最高，从 head_ptr 选最老的</text>
  <text x="270" y="878.5" font-size="8.5" fill="#475569">　2. 没有 Reissue 时，DataIn 与普通 Generated 按相对</text>
  <text x="270" y="892.0" font-size="8.5" fill="#475569">　　 head_ptr 的 Stream 年龄比较，较老者优先</text>
  <text x="270" y="905.5" font-size="8.5" fill="#475569">　3. 同一 Stream 时优先选 Generated</text>
  <text x="270" y="919.0" font-size="8.5" fill="#475569">选中后非抢占保持：锁定任务上下文，命令与相关字段</text>
  <text x="270" y="932.5" font-size="8.5" fill="#475569">　保持稳定直到 RV core 返回 raw ACCEPT</text>
  <text x="270" y="946.0" font-size="8.5" fill="#475569">收到 ACCEPT 后：Generated 提交 READY → INFLY；</text>
  <text x="270" y="959.5" font-size="8.5" fill="#475569">　DataIn 只通知 DataIn_task_table 出槽，不改 stream 状态</text>
  <text x="270" y="973.0" font-size="8.5" fill="#475569">task_dsa_en=0 的 Generated 下发给 DTE RV core，不配 DSA</text>
  <rect x="690" y="800" width="340" height="206" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="702" y="821" font-size="11" fill="#111827">MU_Arb</text>
  <text x="702" y="838" font-size="8.5" fill="#475569">候选：valid=1 且 task_fsm=READY</text>
  <text x="702" y="851.5" font-size="8.5" fill="#475569">　且 task_unit=MU</text>
  <text x="702" y="865.0" font-size="8.5" fill="#475569">从 head_ptr 开始环形年龄优先，</text>
  <text x="702" y="878.5" font-size="8.5" fill="#475569">选最老的 Stream</text>
  <text x="702" y="892.0" font-size="8.5" fill="#475569">发射宽度 1</text>
  <text x="702" y="905.5" font-size="8.5" fill="#475569">非抢占保持到 raw ACCEPT</text>
  <text x="702" y="919.0" font-size="8.5" fill="#475569">RV core 的 task_queue 满时会反压</text>
  <text x="702" y="932.5" font-size="8.5" fill="#475569">ACCEPT 后提交 READY → INFLY</text>
  <text x="702" y="946.0" font-size="8.5" fill="#475569">Map 返回未接受时只重试这笔</text>
  <text x="702" y="959.5" font-size="8.5" fill="#475569">　状态写，不重发已被接收的任务</text>
  <rect x="1072" y="800" width="340" height="206" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="1084" y="821" font-size="11" fill="#111827">VU_Arb</text>
  <text x="1084" y="838" font-size="8.5" fill="#475569">候选：valid=1 且 task_fsm=READY</text>
  <text x="1084" y="851.5" font-size="8.5" fill="#475569">　且 task_unit=VU</text>
  <text x="1084" y="865.0" font-size="8.5" fill="#475569">规则同 MU_Arb</text>
  <text x="1084" y="878.5" font-size="8.5" fill="#475569">三条发射通路各自独立打拍，</text>
  <text x="1084" y="892.0" font-size="8.5" fill="#475569">同一拍可以并行下发 3 个 task</text>
  <text x="1084" y="905.5" font-size="8.5" fill="#475569"></text>
  <text x="1084" y="919.0" font-size="8.5" fill="#475569">B core：task 0 借 VU core 跑</text>
  <text x="1084" y="932.5" font-size="8.5" fill="#475569">　纯标量的 check_flag</text>
  <text x="1084" y="946.0" font-size="8.5" fill="#475569">R core：task 0 借 MU core 跑</text>
  <text x="1084" y="959.5" font-size="8.5" fill="#475569">　check flag，求和交给 VU</text>
  <polyline points="204,119 231,119 231,125 258,125" fill="none" stroke="#0f766e" marker-end="url(#g)"/>
  <polyline points="558,128 588,128 588,133 618,133" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <text x="588" y="132" font-size="8.5" fill="#6b7280" text-anchor="middle">create</text>
  <polyline points="366,230 366,262" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="558,307 588,307 588,285 618,285" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <text x="588" y="318" font-size="8.5" fill="#6b7280" text-anchor="middle">datain</text>
  <polyline points="1048,175 1078,175 1078,148 1108,148" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="204,663 661,663 661,626" fill="none" stroke="#0f766e" marker-end="url(#g)"/>
  <polyline points="204,715 1198,715 1198,626" fill="none" stroke="#0f766e" marker-end="url(#g)"/>
  <text x="1092" y="730" font-size="8.5" fill="#0f766e" text-anchor="end">rmem2ts_done_ch 按 user_id 匹配对应 Stream</text>
  <polyline points="773,430 773,388" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="1252,430 1252,320 1495,320 1495,298" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-start="url(#as)" marker-end="url(#a)"/>
  <polyline points="1108,469 1078,469 1078,345 1048,345" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#a)"/>
  <polyline points="558,571 618,571" fill="none" stroke="#7c3aed" stroke-dasharray="4 3" marker-end="url(#p)"/>
  <polyline points="444,430 444,414 644,414 644,388" fill="none" stroke="#7c3aed" stroke-dasharray="4 3" marker-end="url(#p)"/>
  <polyline points="330,430 330,388" fill="none" stroke="#7c3aed" stroke-dasharray="4 3" marker-end="url(#p)"/>
  <polyline points="543,430 543,400 1130,400 1130,298" fill="none" stroke="#7c3aed" stroke-dasharray="4 3" marker-end="url(#p)"/>
  <text x="1142" y="406" font-size="8.5" fill="#7c3aed" text-anchor="start">task_chain 属性 / 三张掩码 / Task LUT</text>
  <polyline points="618,340 596,340 596,766 453,766 453,800" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="618,362 578,362 578,782 860,782 860,800" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <polyline points="1048,340 1070,340 1070,772 1242,772 1242,800" fill="none" stroke="#475569" marker-end="url(#a)"/>
  <text x="292" y="692" font-size="8.5" fill="#6b7280" text-anchor="start">从 Stream_table 取候选与任务属性；三条发射通路各自独立打拍</text>
  <polygon points="369.0,1048 537.0,1048 528.0,1078 360.0,1078" fill="#f8fafc" stroke="#374151"/>
  <text x="449" y="1067" font-size="8.5" fill="#374151" text-anchor="middle">task_cmd / task_ack［DTE］</text>
  <polyline points="453,1006 453,1027 448,1027 448,1048" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polygon points="776.0,1048 944.0,1048 935.0,1078 767.0,1078" fill="#f8fafc" stroke="#374151"/>
  <text x="856" y="1067" font-size="8.5" fill="#374151" text-anchor="middle">task_cmd / task_ack［MU］</text>
  <polyline points="860,1006 860,1027 856,1027 856,1048" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polygon points="1158.0,1048 1326.0,1048 1317.0,1078 1149.0,1078" fill="#f8fafc" stroke="#374151"/>
  <text x="1238" y="1067" font-size="8.5" fill="#374151" text-anchor="middle">task_cmd / task_ack［VU］</text>
  <polyline points="1242,1006 1242,1027 1238,1027 1238,1048" fill="none" stroke="#475569" marker-start="url(#as)" marker-end="url(#a)"/>
  <polygon points="1440,762 1600,762 1591,792 1431,792" fill="#f8fafc" stroke="#374151"/>
  <text x="1516" y="781" font-size="9" fill="#374151" text-anchor="middle">ts2router</text>
  <polyline points="1048,606 1100,606 1100,777 1431,777" fill="none" stroke="#b45309" marker-end="url(#o)"/>
  <text x="1436" y="754" font-size="8.5" fill="#b45309" text-anchor="end">资源注册 / retire / credit 返还</text>
  <polygon points="1440,340 1590,340 1581,370 1431,370" fill="#f8fafc" stroke="#374151"/>
  <text x="1511" y="359" font-size="9" fill="#374151" text-anchor="middle">ts2corestatus</text>
  <polyline points="1522,430 1522,400 1510,400 1510,370" fill="none" stroke="#b45309" stroke-dasharray="4 3" marker-end="url(#o)"/>
  <text x="1436" y="306" font-size="8.5" fill="#b45309" text-anchor="end">异常上报 → Core Status → SCP（本轮只留接口名）</text>
  <polygon points="1440,676 1600,676 1591,706 1431,706" fill="#f8fafc" stroke="#374151"/>
  <text x="1516" y="695" font-size="9" fill="#374151" text-anchor="middle">rv / dsa done ×6</text>
  <polyline points="1516,676 1516,651 1495,651 1495,626" fill="none" stroke="#0f766e" marker-end="url(#g)"/>
  <polygon points="36,780 186,780 177,810 27,810" fill="#f8fafc" stroke="#374151"/>
  <text x="107" y="799" font-size="9" fill="#374151" text-anchor="middle">cfg（ctrl_noc）</text>
  <polyline points="186,795 306,795 306,626" fill="none" stroke="#7c3aed" stroke-dasharray="2 3" marker-end="url(#p)"/>
  <text x="20" y="1100" font-size="10.5" fill="#374151">TS 是一块固定的硬件逻辑，不是可编程的调度器：上电配好几张表、写 TS_INIT_FINISH 之后就按固定逻辑跑，运行期不接受软件干预，也没有指令可执行。</text>
</svg>
```

***

## 2　功能清单

一功能一条，编号供“机制覆盖”一章引用。

### CFG_REG

| 编号 | 功能 |
| - | - |
| F1 | `task_chain` 最多 64 项，每项 64 bit，软件经 ctrl_noc 逐项写；每写一项硬件自动把该项的 `TASK_VALID` 置起来 |
| F2 | 表项位域：`TASK_PC`、`TASK_SEND_UNIT`（00=DTE / 01=MU / 10=VU）、`TASK_RECV_UNIT`（00=只调 RV core / 01=调 DSA / 10=DTE DSA 加 Router 的 Rmem）、`SELF_START`、`WAIT_WAKE`、`TASK_Broadcast_REISSUE`、`TASK_P2P_REISSUE`、`TASK_REDUCE`、`TASK_CREDIT_EN`、`TASK_EXE_MASK`、`TASK_PATH_ID`、`TASK_END`、`TASK_VALID` |
| F3 | `DATAIN_TASK` 是独立的一项寄存器，只有 1 项、只能绑定 DTE，四个字段 `TASK_PC` / `TASK_UNIT` / `WEIGHTS_MODE` / `TASK_VALID` |
| F4 | 其余配置项：`STREAM_NUM`（1～16）、`CORE_TYPE`、`B_CORE_DIRECTION`、`trigger_task_chain_en`、`TS_INIT_FINISH`、`TS_STATE` |
| F5 | 配置的下发顺序：先写全局项，再逐项写 `task_chain[i]`，再写 `DATAIN_TASK`，最后写 `TS_INIT_FINISH`。最后一步之前硬件不做任何检查 |
| F6 | 看到 `TS_INIT_FINISH` 后查五项合规性并把结论写进 `TS_STATE`：是否配置、一条链上是否出现多个 `TASK_END`、valid 的项中间是否有空洞、一条链上是否出现多个 `SELF_START`、普通 core 是否配了 `SELF_START` |
| F7 | 派生三张 64 位掩码供 Task_ctrl 一拍算出 SKIP_MASK：`DATA_IN_MASK`、`REISSUE_MASK`、`END_MASK`。三张掩码是 `task_chain` 的纯函数，配完不再变，运行时的跳过判断只查掩码与 `done_bitmap`，不必逐项回读 `task_chain` |
| F8 | Task LUT：按 `task_id` 索引出该 task 的全部下发属性，供三条发射通路查 |
| F9 | 硬件位域里没有的软件侧属性从软件侧任务链表另行读入，与硬件表分开存：`exe_dest`、`task_group_id`、`reduce_num`、`dsa_en` |
| F10 | `CORE_TYPE` 是 B core 或 R core 时，硬件在复位后直接自启动 16 个表项，每项按 task 0 的属性建立、`task_fsm` 置 `TASK_RDY`；此时还没有用户信息，等自启动任务的 RV core 返回 `user_id` 后再补进表项 |

### User_Match

| 编号 | 功能 |
| - | - |
| F11 | 拿 Router 请求里的 `user_id` 与 `stream_table` 里的比对 |
| F12 | 没匹配上是新用户，向 Stream_table 发起建表请求 |
| F13 | 匹配上是老用户，复用原来的 `stream_id`，不改它当前的任务、状态、完成位；只有请求带着重发标记时才把该项的 `reissue` 置起来 |
| F14 | 四条同时满足才建表：判定是新用户、当前不在 weights 加载模式、`trigger_task_chain_en` 允许启动任务链、`tail_ptr − head_ptr < stream_num`。否则请求在入口等着 |
| F15 | 建表时在 `tail_ptr` 指向的空槽一次性写入用户信息、Task 0 的状态与全部属性，随后推进 `tail_ptr` |
| F16 | Task 0 的初始状态按类型定：datain 或 reissue 置 `TASK_WAIT`，`self_start` 置 `TASK_RDY` |

### DataIn_task_table

| 编号 | 功能 |
| - | - |
| F17 | 只有 1 项。空闲时把 datain 任务信息与 Router 的请求信息一起登记进去 |
| F18 | 被占住时反压 Router 的新请求 |
| F19 | 任务仲裁成功并被 DTE RV core 接收后，该项立即释放 |
| F20 | 两种 PC 的去处不同：Task 0 的 `task_pc` 进 stream_table，datain 的 `task_pc` 进这里 |
| F21 | B core 与 R core 的 datain 任务不建 stream 表项 |

### Stream_table

| 编号 | 功能 |
| - | - |
| F22 | 16 项顺序 FIFO，每项对应一条完整用户业务流；`head_ptr` 与 `tail_ptr` 环形推进 |
| F23 | 用户级标记（建表写入，整条任务链期间基本不动）：`valid`、`user_id`、`reissue`、`compute` |
| F24 | 进度三字段合起来才是完整进度：`task_id` 是链上的第几步，`task_fsm` 是这一步的状态，`done_bitmap` 是 64 位对应 64 个 task 的数据齐没齐 |
| F25 | 异步 datain 提前完成表现为 `done_bitmap` 上某一位先亮而 `task_id` 还没走到那里 |
| F26 | 当前 task 的属性摊平存在表项里（`task_unit`、`task_dsa_en`、`task_pc`、`is_reissue`、`end`），每次更新 `task_id` 时索引 `task_chain` 得到，随 `task_id` 一起被覆盖 |
| F27 | `task_fsm` 五个状态：IDLE → WAIT → READY → INFLY → FINISH |
| F28 | 六个写口在这里仲裁，每口一拍一笔，请求保持到 `accepted` 才算生效；同一拍多个写口冲突时按端口的固定优先级排队 |
| F29 | 写失败分两种：`create`、`install` 这类整项写入失败后要重读最新表内容再来；`issue` 这类只改一个字段的失败后只重试这一笔写，不能重新下发已经被 RV core 接收的任务 |

### Task_ctrl

| 编号 | 功能 |
| - | - |
| F30 | 当前任务进入 `TASK_FINISH` 后才生成后继；每个 stream 独立推进，不需要全局 Task Pointer |
| F31 | 从 `head_ptr` 开始环形扫描，只选 `valid=1 && task_fsm=TASK_FINISH && end=0` 的 stream |
| F32 | 用一次 64 bit 优先编码一拍跳过所有可跳过的 task：`SKIP_MASK = ~END_MASK & ( (DATA_IN_MASK & done_bitmap) \| (REISSUE_MASK & ~stream.reissue) )`。连续 skip 的数量不增加周期 |
| F33 | 可跳过的四种场景：异步 datain_task 已提前完成；B reissue 任务不需要重发；P2P 任务不需要重发（Router 通过匹配 `path_id` 告知哪个 P2P task 已完成）；DP+P2P 场景下有些用户只有 P2P 无计算 task、有些是计算无 P2P，Router 请求携带用户是否计算，其余任务可越过 |
| F34 | End task 即使已提前完成也不能被跳过，并且不会再生成后继任务 |
| F35 | 新任务的 `task_id`、`task_fsm`、`end` 与全部下发属性必须一起原子写入才算生成成功 |
| F36 | 生成后的初始状态按类型分：普通 Generated 置 `TASK_READY`，DataIn 置 `TASK_WAIT` 等 ack，Reissue 置 `TASK_WAIT` 等 credit |
| F37 | 就绪只有两个来源：前序任务都完成（`done_bitmap` 对应位拉高），以及 credit 到位（本任务不需要 credit，或需要的 credit 已经满足）。datain 任务是例外，被 Router trigger 后立即就绪 |
| F38 | 异步 datain 搬运完成后只更新 `done_bitmap` 对应位，不推进 `task_id` |

### DTE_Arb

| 编号 | 功能 |
| - | - |
| F39 | 候选是 `valid=1 && task_fsm=TASK_READY && task_unit=DTE` 的 stream |
| F40 | Reissue 任务优先级最高，从 `head_ptr` 选最老的 Reissue |
| F41 | 没有 Reissue 时，DataIn 任务与普通 Generated 任务按相对 `head_ptr` 的 Stream 年龄比较，较老者优先；同一 Stream 时优先选 Generated |
| F42 | 选中后非抢占保持：锁定任务上下文，命令与相关字段保持稳定，直到对应 RV core 返回 `raw ACCEPT` |
| F43 | 收到 ACCEPT 后，Generated 任务向 Stream_table 提交 `TASK_READY → TASK_INFLY`；DataIn 任务只通知 DataIn_task_table 出槽，不改 stream_table 里的当前任务状态 |
| F44 | `task_dsa_en = 0` 的 Generated 任务下发给 DTE Local RV core，不配 DSA |
| F45 | 重发 task 不在用户主线任务链上，可与用户任务链并行执行 |

### MU_Arb / VU_Arb

| 编号 | 功能 |
| - | - |
| F46 | 候选是 `valid=1 && task_fsm=TASK_READY && task_unit=MU`（或 VU）的 stream |
| F47 | 从 `head_ptr` 开始环形年龄优先，选最老的 Stream；发射宽度各 1 |
| F48 | 三条发射通路各自独立打拍，同一拍可以并行下发 3 个 task |
| F49 | RV core 按 task_queue 是否有空槽产生 `task_ack`；未被接收时 TS 不能释放该 task 跳到下一个 |

### Credit_monitor

| 编号 | 功能 |
| - | - |
| F50 | 向 Router 注册资源申请，带 UserID、StreamID、TaskID、PathID；Router 申请到后经反向控制通路通知，唤醒对应 task 置 READY |
| F51 | credit 的单位是 stream 不是 task：Core Mem 空间按 stream 申请和释放 |
| F52 | 同一个 stream 的任务链里，Router 只在第一次往下游发数据时检查 credit；第一次满足之后后续发往同一下游的 DTE 任务不再检查 |
| F53 | 检查的发起方分两种：本 core Core Mem → 下游 Router 属于任务链里的 DTE 任务，由 TS 查下游 credit；本 core Router → 下游 Router 不属于任务链，由 Router 自己查下游 credit 并触发阻塞重发 |
| F54 | Broadcast Reissue：broadcast 数据到达 Router trigger TS 时下游无法接收则把重发标记置为有效；TS 查询下游 credit，可下发时按用户顺序选最老的重发用户，发起重发 task 到 DTE 去 Core Mem 搬 token 到 Router |
| F55 | 重发标记有效但未重发成功时，该用户的原始 token 数据不能被覆盖，也不能释放该用户 |
| F56 | P2P 重发：软件把 bypass 本 core 的任务配成 datain 与 dataout 两个 task。不需要重发时 Router 用 `path_id` 匹配告知已完成，执行到该 task 时直接越过；需要重发时 datain 由 Router 请求直接驱动 datain_task 搬运，dataout 等前序任务做完、执行到本 task 时向 Router 请求 credit，满足后标记就绪等待下发 |
| F57 | P2P 阻塞缓冲：软件可配开关、分给 P2P 缓存的 Core Mem 容量、开启的方向（最多 3 个）、每方向的容量与项数 |
| F58 | TS 侧为 P2P 阻塞缓冲维护每方向一张映射表（`p2p vld` / `User id` / `Down direction` / `Data addr`）与对应下游 core 的 credit 计数器；下游 credit 释放后发起一个调度 DTE 搬数据到 Router 继续传输的 task，并通知上游释放 credit |
| F59 | Reduce 任务的 credit：软件配 `reduce_num = N`，TS 发现是 reduce task 后顺序连续下发 N 笔 credit 请求，credit 满足即可顺序下发，直到收全 N 笔 Rmem finish |
| F60 | Head-only 退休：只允许 `head_ptr` 指向的那一项退休，条件是 `valid=1 && end=1 && task_fsm=TASK_FINISH` |
| F61 | 退休顺序：先向 Router 持续发 credit 返还请求，Router 接收后才清除该槽位的 `valid` 并推进 `head_ptr`；返还的 credit 经本 core 的 Router 通知上游 core，让上游的 TS credit 加一 |
| F62 | B core 的搬出 task 按 `B_CORE_DIRECTION` 查下游 core 的 TS credit；落 Matrix Mem 的那一侧不查 TS credit，反压由 GPU 到 Bach 的两层 credit 兜底 |
| F63 | 广播任务的 `CreditCounter[path_id][stream_id]` 初值等于目的 core 数量，P2P 任务初值为 1；够则一次扣掉全部目的数再下发，不够就等 credit 释放 |
| F64 | 坏核只按路由表透传，不检查 credit、不支持阻塞重发；上游要查的 credit 对应坏核之后那个好核 |

### Task_done

| 编号 | 功能 |
| - | - |
| F65 | 七路完成事件：DTE、MU、VU 各有 RV core ack 与 DSA ack 共六路，DTE 这条 Lane 额外接收 Router 的 Reduce Done。三条 Completion Lane 直接写 stream_table，不设统一的 Update 模块 |
| F66 | 按当前任务的 `task_recv_type` 判断哪一路才算数：只调 RV core 或调了 DSA 但 RV core 会等 DSA 完成后再执行一段程序时由 RV core 收尾；异步配置调度 DSA 后 RV core 立刻结束 task 程序时由 DSA 收尾；RV core 异步配置 DSA 后不查询完成状态但还要再执行一段程序时两者都上报，TS 等二者都完成 |
| F67 | Reduce 任务的完成拆成两半：DTE ack 只代表搬运完成，执行 `consume_only`，不修改 stream 状态；只有 Router Reduce Done 才有权把 Reduce 任务置为 `TASK_FINISH` |
| F68 | 两个事件可以任意顺序到达。Router Done 可以被 Hold，但必须等匹配的 DTE ack 被消费后才提交任务完成 |
| F69 | Router 不携带 `stream_id`，Task_done 内部按 `user_id` 找对应 Stream |
| F70 | 异常检测：异步 task 本该执行 1 次完成却返回多次；RV core 或 DSA 返回非法的 `task_id` / `stream_id`；异步任务长时间没有收到外部 trigger；用户长时间未 retire；Router 请求携带的 `path_id` 在 task_chain 里匹配不到。异常经 `ts2corestatus_int_ch` 上报，本轮只留接口名与状态位 |

### 自启动（B core 与 R core）

| 编号 | 功能 |
| - | - |
| F71 | 复位后直接自启动 16 个 stream_table 表项，同时激活 16 个用户的自启动任务参与仲裁发射，执行过程同普通计算 core |
| F72 | 启动时没有用户信息，等自启动任务的 RV core 返回 `user_id` 后更新 stream_table，再调度后续任务 |
| F73 | 数据接收类 datain 任务由 datain task 支持，但完成 flag 由软件设置维护，不在 TS 里更新 |
| F74 | check flag 的 RV core 需要长期工作，该 task 不调度对应的 DSA |
| F75 | 任务链创建不受用户数据 trigger 影响，自动在每个 stream 表项创建起点 task，按 stream 顺序激活执行 check flag task |
| F76 | 前一个 stream check 通过后需清除 ready 用户的 flag，下一个 stream 才能继续 check |
| F77 | 一个 stream 的任务链完成后会在对应 stream 项自发创建新的任务链；表项从队头 retire 后再激活一个新表项，继续等待自启动任务 |

***

## 3　接口

```
port router2ts_trigger_ch (slave, 脉冲, clk)      // Router 的 CoreStation 收满一个包
  in  valid · user_id[15:0] · path_id[7:0] · reissue · compute
port router2ts_credit_ch (slave, 脉冲, clk)       // Router 的 CoreMemCreditMonitor：资源到手
  in  valid · stream_id[3:0] · task_id[5:0] · path_id[7:0]
port rmem2ts_done_ch (slave, 脉冲, clk)           // ReduceModule：一个整包 reduce 完成，按 user_id 匹配
  in  valid · user_id[15:0]
port ts2router_req (master, valid/ready, clk)     // 注册资源申请
  out req_valid · user_id[15:0] · stream_id[3:0] · task_id[5:0] · path_id[7:0]
  in  req_ready                                     // = Router 的监听事件队列有空项
port ts2router_retire (master, 脉冲, clk)         // 用户退休与 credit 返还
  out valid · user_id[15:0]
  in  accepted                                      // Router 接收后才清 valid 并推进 head_ptr
port task_cmd[u] (master, valid/ready, clk)       // u ∈ {DTE, MU, VU}：task 下发
  out cmd_valid · task_pc[31:0] · stream_id[3:0] · local_user_id[11:0] · task_dsa_en
  in  cmd_ready                                     // = 该 RV core 的 task_queue 有空槽（raw ACCEPT）
port rv_done[u] (slave, 脉冲, clk)                // RV core 报完成
  in  valid · stream_id[3:0] · local_user_id[11:0] · task_id[5:0]
port dsa_done[u] (slave, 脉冲, clk)               // DSA 报完成；VU 另有 Event 同步信号
  in  valid · stream_id[3:0] · task_id[5:0]
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
mem sw_attr        FF 阵列   64 × {exe_dest, task_group_id, reduce_num[5:0], dsa_en}       1R    编译侧读入                    复位由输入给   // 硬件位域里没有的软件侧属性
mem mask3          FF        {DATA_IN_MASK[63:0], REISSUE_MASK[63:0], END_MASK[63:0]}      1R1W  写 TS_INIT_FINISH 时派生      复位 0
mem stream_table   FF 阵列   16 × {valid, user_id[15:0], reissue, compute, task_id[5:0], task_fsm[2:0], done_bitmap[63:0], task_unit[1:0], task_dsa_en, task_pc[31:0], is_reissue, end}  6W1R  六个写口按固定优先级仲裁  复位空
mem stream_ptr     FF        {head_ptr[4:0], tail_ptr[4:0]}                                1RW   建表推 tail，退休推 head       复位 0
mem datain_hold    FF        1 项 {valid, task_pc[31:0], user_id[15:0], path_id[7:0]}      1RW   占住即反压 Router              复位空
mem credit_cnt     FF 阵列   每 {path_id, stream_id} 一个计数器                             1RW   广播初值 = 目的 core 数，P2P = 1  复位由输入给
mem p2p_buf_map    FF 阵列   每方向一张 × {p2p_vld, user_id[15:0], down_dir[2:0], data_addr[17:0]}  1RW  最多 3 个方向  复位空
mem p2p_dn_credit  FF 阵列   每方向对应下游 core 的 credit 计数器                            1RW   —                             复位由配置给
mem reduce_pend    FF 阵列   每 stream 一项 {issued[5:0], done[5:0]}                        1RW   下发 N 笔、收全 N 笔          复位 0
mem done_hold      FF 阵列   16 × {router_done_pending, dte_ack_consumed}                   1RW   Reduce 两半事件的配对          复位 0
mem 级间 latch      级间 latch 各模块之间的命令与状态                                        —     每拍覆写                      —
```

***

## 5　流水线总览

第 1 层图待逐级拍数定下来后补。三套时延数字口径不同，届时按 LLD 那一套逐级填：

| 数字 | 出处 | 口径 |
| - | - | - |
| task 唤醒延迟 2～3 cycle | TS MAS | 硬件目标值，时序满足前提下从条件成立到发出命令的最短拍数 |
| 调度间隔 16 T；需与 Router 通信时 21 T；retire 5 T | Top 模拟器详设 | 端到端建模值，含 RV core 取指与返回 ack 的往返 |
| CREATE 3 / WAKE 2 / DONE 3 / INSTALL 4 / RETIRE 6 | TS LLD 时序图 | 按 LLD 波形拆到每一级的拍数 |

***

## 6　逐级行为

第 2 层图与每级的四要素待第 1 层图完成后补，级编号回标到第 1 层图。

***

## 7　参数汇总

```
task_chain          64 项 × 64 bit
datain_task         1 项
DataIn_task_table   1 项（Depth-1 Hold）
stream_table        16 项，注册与释放深度各 1；stream_num 软件配 1～16
done_bitmap         64 位，对应 64 个 task
DTE / MU / VU Arb   发射宽度各 1
task_fsm            五态 IDLE / WAIT / READY / INFLY / FINISH
P2P 阻塞缓冲         方向数 ≤ 3，容量与项数软件配
task 唤醒延迟        2～3 cycle（硬件目标值）
调度间隔            16 T；需与 Router 通信时 21 T；retire 5 T（端到端建模值）
```

***

## 8　机制覆盖

| 机制 | 功能 | 用例 |
| - | - | - |
| task_chain 64 项，写一项自动置 VALID | F1、F2 | `task_chain_write` |
| 配置下发顺序与写 TS_INIT_FINISH 后的五项检查 | F5、F6 | `ts_init_check` |
| 三张掩码是 task_chain 的纯函数，运行时只查掩码与 done_bitmap | F7 | `derive_masks` |
| 软件侧属性与硬件位域分开存 | F9 | `sw_attr_separate` |
| Router 经 router2ts_trigger_ch 直接通知 TS，不经软件 | F11 | `trigger_entry` |
| 老用户复用 stream_id，只可能改 reissue | F13 | `user_match_reuse` |
| 建表四条同时满足 | F14 | `create_conditions` |
| 建表一次性写入用户信息与 Task 0 的全部属性 | F15 | `create_atomic` |
| DataIn_task_table 只有 1 项，占住就反压 Router | F17、F18 | `datain_hold` |
| 16 项顺序 FIFO，进度靠三个字段合起来判断 | F22、F24 | `stream_table_progress` |
| 六个写口的原子性与两种失败处理 | F28、F29 | `six_write_ports` |
| SKIP_MASK 一拍跳过，连续 skip 不增加周期 | F32、F33 | `skip_mask` |
| End task 即使已完成也不能跳，且不再生成后继 | F34 | `end_task_no_skip` |
| 后继任务的全部字段一起原子写入 | F35 | `install_atomic` |
| 异步 datain 只更新 done_bitmap，不推进 task_id | F38 | `datain_no_advance` |
| DTE 三类任务的优先级 | F40、F41 | `dte_arb_priority` |
| 选中后非抢占保持到 raw ACCEPT | F42 | `non_preemptive` |
| ACCEPT 后 Generated 提交 INFLY，DataIn 只出槽 | F43 | `accept_handling` |
| 三条发射通路同一拍可并行下发 3 个 task | F48 | `parallel_issue` |
| credit 粒度是 stream，同一 stream 只在第一次发数据时检查 | F51、F52 | `credit_per_stream` |
| Broadcast Reissue：置标记、查 credit、选最老、并行于主链 | F54、F45 | `broadcast_reissue` |
| 未重发成功前原始数据不能被覆盖，用户不能释放 | F55 | `reissue_hold_data` |
| P2P 重发拆成 datain 与 dataout 两个 task | F56 | `p2p_reissue` |
| P2P 阻塞缓冲的四项配置与映射表 | F57、F58 | `p2p_block_buffer` |
| reduce_num = N 时顺序连续下发 N 笔 credit 请求 | F59 | `reduce_credit_n` |
| Head-only 退休，先还 credit 再清 valid | F60、F61 | `head_only_retire` |
| 广播 CreditCounter 初值 = 目的 core 数，一次扣全部 | F63 | `broadcast_credit_init` |
| 七路完成合流，按 task_recv_type 判定 | F65、F66 | `task_done_seven` |
| Reduce 完成拆两半，只有 Router Done 能置 FINISH | F67、F68 | `reduce_done_split` |
| Router 不带 stream_id，按 user_id 找 Stream | F69 | `done_by_user_id` |
| B core / R core 复位后自启动 16 个表项 | F10、F71、F72 | `self_start_16` |
| check flag 的 task 不调度 DSA | F74 | `check_flag_no_dsa` |
| 一个 stream 的任务链完成后自发创建新任务链 | F77 | `self_recreate_chain` |
| B core 搬出按 B_CORE_DIRECTION 查下游 credit，落 MM 不查 | F62 | `bcore_direction_credit` |

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
