# TS 任务调度器

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **TS**

给实现 TS 的人：九个独立打拍的模块各自的端口、存储器、流水线与逐级行为、参数与机制。

九个模块：

* 配置与入口：CFG_REG、User_Match、DataIn_task_table
* 状态：Stream_table、Task_ctrl
* 发射：DTE_Arb、MU_Arb / VU_Arb
* 资源与完成：Credit_monitor、Task_done

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《TS 任务调度器》全篇
* 《软件栈》：“专用 core 在一次实际运行中的轨迹”

***

## 1　定位与边界

TS 是 core 的控制单元，一块上电配好就按固定逻辑跑的硬件。一个用户在它手里走这几步：

1. Router 经 `router2ts_trigger_ch` 直接把 trigger 送进来，建用户，占 stream_table 一项
2. 按 task_chain 逐 task 推进这个用户的 `task_fsm`
3. 把 READY 的 task 经三条发射通路派给三个 RV core
4. 收 RV core / DSA / Router 的完成事件，推进状态
5. 用户链尾退休时经 Router 归还 credit

边界是四组接口：

* 与 Router 的六组信号
* 与三个 RV core 的 task 下发口
* 六路完成 ack
* ctrl_noc 的配置口

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1180 560" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs>
    <marker id="t0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker>
    <marker id="t0s" markerWidth="9" markerHeight="9" refX="1" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#475569"/></marker>
  </defs>
  <rect x="0" y="0" width="1180" height="560" fill="#ffffff"/>
  <text x="20" y="28" font-size="12" fill="#111827">TS · 第 0 层</text>

  <!-- Router 侧端口 -->
  <polygon points="30,80 130,80 120,116 20,116" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="102" font-size="10.5" fill="#374151" text-anchor="middle">rt_trigger</text>
  <polygon points="30,150 130,150 120,186 20,186" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="172" font-size="10.5" fill="#374151" text-anchor="middle">rt_credit_pulse</text>
  <polygon points="30,220 130,220 120,256 20,256" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="242" font-size="10.5" fill="#374151" text-anchor="middle">rt_complete</text>
  <polygon points="30,290 130,290 120,326 20,326" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="312" font-size="10.5" fill="#374151" text-anchor="middle">rt_reduce_done</text>
  <polygon points="30,360 130,360 120,396 20,396" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="382" font-size="10.5" fill="#374151" text-anchor="middle">rt_credit_req</text>
  <polygon points="30,430 130,430 120,466 20,466" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="452" font-size="10.5" fill="#374151" text-anchor="middle">rt_retire</text>
  <polygon points="30,500 130,500 120,536 20,536" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="522" font-size="10.5" fill="#374151" text-anchor="middle">cfg</text>

  <!-- 模块 -->
  <rect x="200" y="70" width="180" height="60" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="212" y="94" font-size="12" fill="#111827">User_Match</text>
  <text x="212" y="112" font-size="10" fill="#475569">新旧用户判定（全相连比较）</text>
  <line x1="132" y1="98" x2="198" y2="98" stroke="#475569" marker-end="url(#t0)"/>
  <rect x="200" y="150" width="180" height="60" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="212" y="174" font-size="12" fill="#111827">DataIn_task_table</text>
  <text x="212" y="192" font-size="10" fill="#475569">Depth-1 Hold → DTE_Arb</text>
  <line x1="380" y1="98" x2="440" y2="98" stroke="#475569" marker-end="url(#t0)"/>
  <line x1="290" y1="132" x2="290" y2="148" stroke="#475569" marker-end="url(#t0)"/>

  <rect x="440" y="60" width="300" height="130" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="444" y="64" width="292" height="122" fill="none" stroke="#374151"/>
  <text x="456" y="86" font-size="12" fill="#111827">Stream_table</text>
  <text x="456" y="106" font-size="10" fill="#475569">stream_tbl · FF 16 × entry · 6W 多 R</text>
  <text x="456" y="122" font-size="10" fill="#475569">head_ptr / tail_ptr · task_fsm · done_bitmap[64]</text>
  <text x="456" y="138" font-size="10" fill="#475569">写口：create · install · issue×3 · completion×3 · credit_wake · retirement</text>
  <text x="456" y="170" font-size="9.5" fill="#9ca3af">被动存储模块，年龄从 head_ptr 环扫</text>

  <rect x="200" y="240" width="180" height="60" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="212" y="264" font-size="12" fill="#111827">Task_ctrl</text>
  <text x="212" y="282" font-size="10" fill="#475569">SKIP_MASK 一拍跳过 · 原子安装</text>
  <line x1="380" y1="270" x2="470" y2="192" stroke="#475569" marker-start="url(#t0s)" marker-end="url(#t0)"/>

  <rect x="200" y="330" width="180" height="60" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="212" y="354" font-size="12" fill="#111827">Credit_monitor</text>
  <text x="212" y="372" font-size="10" fill="#475569">Reissue 唤醒 · credit 申请 · Head-only 退休</text>
  <line x1="132" y1="168" x2="198" y2="350" stroke="#475569" marker-end="url(#t0)"/>
  <line x1="198" y1="378" x2="132" y2="378" stroke="#475569" marker-end="url(#t0)"/>
  <line x1="198" y1="386" x2="132" y2="448" stroke="#475569" marker-end="url(#t0)"/>
  <line x1="380" y1="360" x2="500" y2="192" stroke="#475569" marker-start="url(#t0s)" marker-end="url(#t0)"/>

  <rect x="200" y="420" width="180" height="60" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="212" y="444" font-size="12" fill="#111827">Task_done</text>
  <text x="212" y="462" font-size="10" fill="#475569">七路合流 · Reduce 完成分离</text>
  <line x1="132" y1="238" x2="198" y2="440" stroke="#475569" marker-end="url(#t0)"/>
  <line x1="132" y1="308" x2="198" y2="450" stroke="#475569" marker-end="url(#t0)"/>
  <line x1="380" y1="450" x2="530" y2="192" stroke="#475569" marker-end="url(#t0)"/>
  <polygon points="200,500 300,500 290,536 190,536" fill="#f8fafc" stroke="#374151"/>
  <text x="245" y="522" font-size="10.5" fill="#374151" text-anchor="middle">done_ack[6]</text>
  <line x1="260" y1="498" x2="270" y2="482" stroke="#475569" marker-end="url(#t0)"/>

  <rect x="820" y="60" width="160" height="50" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="832" y="82" font-size="12" fill="#111827">DTE_Arb</text>
  <text x="832" y="98" font-size="10" fill="#475569">Reissue &gt; DataIn / Generated</text>
  <rect x="820" y="130" width="160" height="50" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="832" y="152" font-size="12" fill="#111827">MU_Arb</text>
  <text x="832" y="168" font-size="10" fill="#475569">年龄优先，非抢占</text>
  <rect x="820" y="200" width="160" height="50" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="832" y="222" font-size="12" fill="#111827">VU_Arb</text>
  <text x="832" y="238" font-size="10" fill="#475569">年龄优先，非抢占</text>
  <line x1="742" y1="90" x2="818" y2="85" stroke="#475569" marker-start="url(#t0s)" marker-end="url(#t0)"/>
  <line x1="742" y1="130" x2="818" y2="155" stroke="#475569" marker-start="url(#t0s)" marker-end="url(#t0)"/>
  <line x1="742" y1="170" x2="818" y2="225" stroke="#475569" marker-start="url(#t0s)" marker-end="url(#t0)"/>
  <polyline points="380,180 400,180 400,240 820,240 820,200" fill="none" stroke="#475569" stroke-dasharray="4 3"/>
  <text x="600" y="236" font-size="9" fill="#6b7280" text-anchor="middle">datain_offer / datain_pop</text>

  <polygon points="1030,70 1140,70 1130,106 1020,106" fill="#f8fafc" stroke="#374151"/>
  <text x="1080" y="92" font-size="10.5" fill="#374151" text-anchor="middle">rv_task[DTE]</text>
  <polygon points="1030,140 1140,140 1130,176 1020,176" fill="#f8fafc" stroke="#374151"/>
  <text x="1080" y="162" font-size="10.5" fill="#374151" text-anchor="middle">rv_task[MU]</text>
  <polygon points="1030,210 1140,210 1130,246 1020,246" fill="#f8fafc" stroke="#374151"/>
  <text x="1080" y="232" font-size="10.5" fill="#374151" text-anchor="middle">rv_task[VU]</text>
  <line x1="982" y1="88" x2="1020" y2="88" stroke="#475569" marker-end="url(#t0)"/>
  <line x1="982" y1="158" x2="1020" y2="158" stroke="#475569" marker-end="url(#t0)"/>
  <line x1="982" y1="228" x2="1020" y2="228" stroke="#475569" marker-end="url(#t0)"/>

  <rect x="820" y="330" width="320" height="110" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="824" y="334" width="312" height="102" fill="none" stroke="#374151"/>
  <text x="836" y="356" font-size="12" fill="#111827">CFG_REG 与 Task LUT</text>
  <text x="836" y="376" font-size="10" fill="#475569">task_chain · FF 64 × 64 b · 1W 多 R</text>
  <text x="836" y="392" font-size="10" fill="#475569">datain_task · stream_num · core_type · trigger_task_chain_en</text>
  <text x="836" y="408" font-size="10" fill="#475569">派生掩码 DATA_IN / REISSUE / END · task_lut_query 一拍返回</text>
  <polyline points="132,518 160,518 160,560 700,560 700,470 830,470 830,442" fill="none" stroke="#475569" stroke-dasharray="2 3" marker-end="url(#t0)"/>
  <line x1="900" y1="328" x2="900" y2="252" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#t0)"/>
  <line x1="600" y1="328" x2="600" y2="192" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#t0)"/>
  <polyline points="820,400 600,400 600,328" fill="none" stroke="#475569" stroke-dasharray="4 3"/>
  <text x="700" y="396" font-size="9" fill="#6b7280" text-anchor="middle">task_lut_query（各模块）</text>
</svg>
```

***

## 2　接口

```
port rt_trigger (slave, valid/ready, clk)         // Router CoreStation 按包头顺序通知，每拍一笔
  in  valid
  out ready                                         // = DataIn_task_table 的 Hold 空
  in  user_id[15:0] · path_id[5:0] · reissue · p2p_reissue · compute   // compute：DP+P2P 场景该用户是否有计算 task
port rt_credit_pulse (slave, 脉冲, clk)           // Router CreditMonitor：注册的事件已满足
  in  valid · stream_id[3:0]
port rt_complete (slave, 脉冲, clk)               // 出 core 搬运（含重发）完成
  in  valid · user_id[15:0] · path_id[5:0]
port rt_reduce_done (slave, 脉冲, clk)            // ReduceModule 完成
  in  valid · user_id[15:0] · task_id[5:0]
port rt_credit_req (master, valid/ready, clk)     // 向 Router CreditMonitor 注册；Stream credit 归还
  out req_valid
  in  req_ready
  out req_stream[3:0] · req_user[15:0] · req_task[5:0] · req_path[5:0] · req_dir[4:0] · req_reissue
  out return_valid · return_user[15:0]             // 持续拉高到 accepted
  in  return_accepted
port rt_retire (master, 脉冲, clk)                // User Retire 广播
  out valid · user_id[15:0]

port rv_task[u] (master, valid/ready, clk)        // u ∈ {DTE, MU, VU}：task 下发
  out cmd_valid                                     // 持续到 cmd_ready（raw ACCEPT），非抢占
  in  cmd_ready                                     // = 该 RV core 的 task_queue 有空槽（上拍值）
  out task_pc[31:0] · stream_id[3:0] · user_id[15:0] · task_id[5:0] · stream_num[4:0] · dsa_en · is_datain
port done_ack[k] (slave, 脉冲, clk)               // k ∈ {DTE_RV, MU_RV, VU_RV, DTE_DSA, MU_DSA, VU_DSA}
  in  valid · stream_id[3:0] · task_id[5:0] · user_id[15:0]
port cfg (slave, ctrl_noc 写事务, clk)            // task_chain、datain_task、STREAM_NUM、TS_INIT_FINISH、CORE_TYPE …
  in  cfg_valid · cfg_addr[15:0] · cfg_we · cfg_wdata[31:0]
  out cfg_rdata[31:0]                               // TS_STATE 等只读寄存器，下一拍
```

***

## 3　存储器

```
mem task_chain        FF 阵列   64 × 64 b {TASK_PC[31:0], SEND_UNIT[1:0], RECV_UNIT[1:0], SELF_START, WAIT_WAKE, B_REISSUE, P2P_REISSUE, REDUCE, CREDIT_EN, EXE_MASK, PATH_ID[5:0], END, VALID}  1W nR  cfg 写，VALID 自动置位  复位 0
mem datain_task       FF        {TASK_PC[31:0], UNIT=DTE, WEIGHTS_MODE, VALID}   1W1R  cfg 写   复位 0
mem ts_regs           FF        {STREAM_NUM[4:0], TS_INIT_FINISH, TS_STATE[3:0], CORE_TYPE[1:0], B_core_direction[2:0], trigger_task_chain_en}  1W1R  cfg 写  复位 0
mem chain_masks       FF        {DATA_IN_MASK[63:0], REISSUE_MASK[63:0], END_MASK[63:0]}  1W nR  TS_INIT_FINISH 时算出  复位 0
mem chain_sw          FF 阵列   64 × {exe_dest, task_group_id, reduce_num}  1W1R  tables/task_chain.h 读入   复位 0     // 软件侧属性，硬件位域无
mem lut_rsp[4]        级间 latch {valid, entry[63:0]}                    —     每拍覆写                       —          // task_lut_query → 各模块，固定下一拍
mem stream_tbl        FF 阵列   16 × {valid, user_id[15:0], task_id[5:0], task_unit[1:0], task_dsa_en, task_pc[31:0], is_reissue, task_fsm[2:0], done_bitmap[63:0], reissue, end, compute}  6W nR  每写口一拍一笔  复位空
mem stream_ptr        FF        {head_ptr[3:0], tail_ptr[3:0]}           1RW   create 推 tail，retire 推 head  复位 0
mem datain_hold       1-deep 寄存器 {valid, stream_id[3:0], user_id[15:0], task_pc[31:0]}  1W1R  满 → rt_trigger.ready=0  复位空
mem um_latch          级间 latch {valid, user_id[15:0], path_id[5:0], reissue, p2p_reissue, compute, hit, slot[3:0]}  —  每拍覆写  —   // User_Match → create / Hold
mem create_ctx        FF        {state[1:0], slot[3:0], entry}            1RW   C1 → C3 三拍安装             复位 IDLE
mem tc_ctx            FF        {state[2:0], slot[3:0], next_task[5:0], next_fsm[2:0]}  1RW  C1 → C4 四拍   复位 IDLE  // Task_ctrl 处理中的上下文
mem arb_ctx[3]        FF        {locked, slot[3:0], task_id[5:0], task_pc[31:0], is_datain}  1RW  锁定到 raw ACCEPT  复位空  // DTE / MU / VU 各一
mem cm_reissue_ctx    FF        {locked, slot[3:0]}                       1RW   锁定到 credit_pulse           复位空
mem cm_retire_ctx     FF        {state[2:0], slot[3:0]}                   1RW   C6 → C11 六拍                 复位 IDLE
mem cm_reduce_ctx     FF        {slot[3:0], sent[3:0], finished[3:0], n[3:0]}  1RW  reduce_num 笔 credit 顺序下发  复位空
mem td_hold[3]        1-deep 寄存器 {valid, src[1:0], stream_id[3:0], task_id[5:0], user_id[15:0]}  1W1R  每 Lane 一个  复位空  // Task_done 三条 Lane
mem td_reduce_hold    1-deep 寄存器 {valid, user_id[15:0], task_id[5:0]}   1W1R  等匹配的 DTE ACK             复位空
mem p2p_block_tbl[3]  FF 阵列   N × {p2p_vld, user_id[15:0], down_dir[2:0], data_addr[19:0]}  1R1W  P2P 阻塞缓冲映射表，每方向一张  复位空
mem p2p_credit[3]     FF        计数                                       1RW   对应下游 core 的 credit       复位初值
```

***

## 4　流水线总览

TS 不是一条直线流水，是围绕 `stream_tbl` 的一个环：

* 建表 → 派发 → 完成 → 生成下一 task → 派发
* 下图按拍对齐画环上的各级
* 六个写端口同拍各可写一笔，冲突按端口固定优先级排队（待定）

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1180 470" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="s0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="1180" height="470" fill="#ffffff"/>
  <text x="20" y="26" font-size="12" fill="#111827">TS · 第 1 层（按拍对齐；每行是环上的一段，行间经 stream_tbl 衔接）</text>
  <g stroke="#e5e7eb"><line x1="140" y1="40" x2="140" y2="440"/><line x1="216" y1="40" x2="216" y2="440"/><line x1="292" y1="40" x2="292" y2="440"/><line x1="368" y1="40" x2="368" y2="440"/><line x1="444" y1="40" x2="444" y2="440"/><line x1="520" y1="40" x2="520" y2="440"/><line x1="596" y1="40" x2="596" y2="440"/><line x1="672" y1="40" x2="672" y2="440"/></g>
  <g font-size="8.5" fill="#6b7280"><text x="140" y="50">t0</text><text x="216" y="50">t1</text><text x="292" y="50">t2</text><text x="368" y="50">t3</text><text x="444" y="50">t4</text><text x="520" y="50">t5</text><text x="596" y="50">t6</text><text x="672" y="50">t7</text></g>

  <!-- 行 1：建表 -->
  <polygon points="24,70 122,70 114,102 16,102" fill="#f8fafc" stroke="#374151"/>
  <text x="69" y="90" font-size="10" fill="#374151" text-anchor="middle">rt_trigger</text>
  <line x1="122" y1="86" x2="138" y2="86" stroke="#475569" marker-end="url(#s0)"/>
  <rect x="144" y="62" width="68" height="48" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="148" y="58" font-size="8.5" fill="#6b7280">U1</text><text x="210" y="58" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="150" y="82" font-size="11" fill="#111827">User_Match</text>
  <text x="150" y="98" font-size="9.5" fill="#475569">全相连比较</text>
  <rect x="220" y="62" width="220" height="48" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="224" y="58" font-size="8.5" fill="#6b7280">U2</text><text x="438" y="58" font-size="8.5" fill="#6b7280" text-anchor="end">D3</text>
  <text x="226" y="82" font-size="11" fill="#111827">Stream create</text>
  <text x="226" y="98" font-size="9.5" fill="#475569">Task0 全属性一次写入 · tail_ptr++</text>
  <line x1="212" y1="86" x2="218" y2="86" stroke="#475569" marker-end="url(#s0)"/>
  <rect x="448" y="62" width="68" height="48" fill="#f1f5f9" stroke="#334155"/>
  <rect x="448" y="62" width="68" height="14" fill="#334155"/>
  <text x="482" y="73" font-size="9.5" fill="#ffffff" text-anchor="middle">create 写口</text>
  <text x="452" y="92" font-size="9" fill="#475569">→ stream_tbl</text>
  <line x1="440" y1="86" x2="446" y2="86" stroke="#475569" marker-end="url(#s0)"/>
  <rect x="220" y="120" width="140" height="36" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="224" y="116" font-size="8.5" fill="#6b7280">D1</text><text x="358" y="116" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="226" y="142" font-size="10.5" fill="#111827">DataIn Hold → datain_offer</text>
  <line x1="178" y1="110" x2="178" y2="138" stroke="#475569"/><line x1="178" y1="138" x2="218" y2="138" stroke="#475569" marker-end="url(#s0)"/>

  <!-- 行 2：派发 -->
  <rect x="144" y="190" width="68" height="48" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="148" y="186" font-size="8.5" fill="#6b7280">A1</text><text x="210" y="186" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="150" y="210" font-size="11" fill="#111827">Arb 选中</text>
  <text x="150" y="226" font-size="9.5" fill="#475569">年龄环扫</text>
  <rect x="220" y="190" width="68" height="48" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="224" y="186" font-size="8.5" fill="#6b7280">A2</text><text x="286" y="186" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="226" y="210" font-size="11" fill="#111827">LUT 查询</text>
  <text x="226" y="226" font-size="9.5" fill="#475569">task_chain[pc]</text>
  <line x1="212" y1="214" x2="218" y2="214" stroke="#475569" marker-end="url(#s0)"/>
  <rect x="296" y="190" width="144" height="48" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="300" y="186" font-size="8.5" fill="#6b7280">A3</text><text x="438" y="186" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="302" y="210" font-size="11" fill="#7c2d12">发 rv_task，等 ACCEPT</text>
  <text x="302" y="226" font-size="9.5" fill="#92400e">非抢占保持</text>
  <line x1="288" y1="214" x2="294" y2="214" stroke="#475569" marker-end="url(#s0)"/>
  <rect x="448" y="190" width="68" height="48" fill="#f1f5f9" stroke="#334155"/>
  <rect x="448" y="190" width="68" height="14" fill="#334155"/>
  <text x="482" y="201" font-size="9.5" fill="#ffffff" text-anchor="middle">issue 写口</text>
  <text x="452" y="220" font-size="9" fill="#475569">READY→INFLY</text>
  <line x1="440" y1="214" x2="446" y2="214" stroke="#475569" marker-end="url(#s0)"/>
  <polygon points="540,198 638,198 630,230 532,230" fill="#f8fafc" stroke="#374151"/>
  <text x="585" y="218" font-size="10" fill="#374151" text-anchor="middle">rv_task[u]</text>
  <line x1="440" y1="205" x2="530" y2="205" stroke="#475569" marker-end="url(#s0)"/>

  <!-- 行 3：完成 -->
  <polygon points="24,278 122,278 114,310 16,310" fill="#f8fafc" stroke="#374151"/>
  <text x="69" y="298" font-size="10" fill="#374151" text-anchor="middle">done_ack[k]</text>
  <line x1="122" y1="294" x2="138" y2="294" stroke="#475569" marker-end="url(#s0)"/>
  <rect x="144" y="270" width="220" height="48" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="148" y="266" font-size="8.5" fill="#6b7280">T1</text><text x="362" y="266" font-size="8.5" fill="#6b7280" text-anchor="end">D3</text>
  <text x="150" y="290" font-size="11" fill="#111827">Task_done</text>
  <text x="150" y="306" font-size="9.5" fill="#475569">Classify · Join（Reduce 分离）· 三条 Lane</text>
  <rect x="372" y="270" width="68" height="48" fill="#f1f5f9" stroke="#334155"/>
  <rect x="372" y="270" width="68" height="14" fill="#334155"/>
  <text x="406" y="281" font-size="9.5" fill="#ffffff" text-anchor="middle">completion 写口</text>
  <text x="376" y="300" font-size="9" fill="#475569">INFLY→FINISH</text>
  <line x1="364" y1="294" x2="370" y2="294" stroke="#475569" marker-end="url(#s0)"/>
  <polygon points="24,330 122,330 114,362 16,362" fill="#f8fafc" stroke="#374151"/>
  <text x="69" y="350" font-size="10" fill="#374151" text-anchor="middle">rt_reduce_done</text>
  <line x1="122" y1="346" x2="200" y2="318" stroke="#475569" marker-end="url(#s0)"/>

  <!-- 行 4：生成下一 task -->
  <rect x="144" y="380" width="296" height="48" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="148" y="376" font-size="8.5" fill="#6b7280">C1</text><text x="438" y="376" font-size="8.5" fill="#6b7280" text-anchor="end">D4</text>
  <text x="150" y="400" font-size="11" fill="#111827">Task_ctrl</text>
  <text x="150" y="416" font-size="9.5" fill="#475569">Pick FINISH → SKIP_MASK 一拍跳过 → 查 LUT → Install（READY / WAIT）</text>
  <rect x="448" y="380" width="68" height="48" fill="#f1f5f9" stroke="#334155"/>
  <rect x="448" y="380" width="68" height="14" fill="#334155"/>
  <text x="482" y="391" font-size="9.5" fill="#ffffff" text-anchor="middle">install 写口</text>
  <text x="452" y="410" font-size="9" fill="#475569">下一 task</text>
  <line x1="440" y1="404" x2="446" y2="404" stroke="#475569" marker-end="url(#s0)"/>
  <polyline points="516,404 560,404 560,250 482,250 482,240" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#s0)"/>
  <text x="566" y="330" font-size="9" fill="#6b7280">READY 的 task 进 A1 候选</text>

  <!-- 右侧：credit / 退休 -->
  <rect x="700" y="190" width="200" height="48" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="704" y="186" font-size="8.5" fill="#6b7280">K1</text><text x="898" y="186" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="706" y="210" font-size="11" fill="#7c2d12">Credit_monitor：注册 → 等 pulse</text>
  <text x="706" y="226" font-size="9.5" fill="#92400e">WAIT → READY（credit_wake 写口）</text>
  <rect x="700" y="270" width="200" height="48" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="704" y="266" font-size="8.5" fill="#6b7280">K2</text><text x="898" y="266" font-size="8.5" fill="#6b7280" text-anchor="end">D6</text>
  <text x="706" y="290" font-size="11" fill="#111827">Head-only 退休</text>
  <text x="706" y="306" font-size="9.5" fill="#475569">credit_return 到 accepted → 清 valid → head_ptr++ → rt_retire</text>
  <polygon points="940,198 1060,198 1050,230 930,230" fill="#f8fafc" stroke="#374151"/>
  <text x="995" y="218" font-size="10" fill="#374151" text-anchor="middle">rt_credit_req / pulse</text>
  <polygon points="940,278 1060,278 1050,310 930,310" fill="#f8fafc" stroke="#374151"/>
  <text x="995" y="298" font-size="10" fill="#374151" text-anchor="middle">rt_retire</text>
  <line x1="900" y1="214" x2="930" y2="214" stroke="#475569" marker-end="url(#s0)"/>
  <line x1="900" y1="294" x2="930" y2="294" stroke="#475569" marker-end="url(#s0)"/>

  <text x="20" y="456" font-size="10" fill="#374151">拍数来自第 3 章 TS 的 LLD 时序：建表 3 拍、唤醒 2 拍（A2 → A3）、完成 3 拍、安装 4 拍、退休 6 拍。</text>
</svg>
```

***

## 5　逐级行为

### U1 · User_Match

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `rt_trigger`、`stream_tbl.user_id[16]`、`ts_regs.CORE_TYPE` | 1. `hit = ∃ i: stream_tbl[i].valid ∧ user_id == in.user_id`；`slot = i`<br>2. `um_latch = {valid, user_id, path_id, reissue, p2p_reissue, compute, hit, slot}`<br>3. `CORE_TYPE ∈ {B, R}` → 不建表，只进 DataIn Hold（datain 不建表）<br>4. `hit ∧ reissue` → 该 slot `reissue = 1`（Broadcast Reissue 置标，不改当前任务 / FSM / bitmap）<br>5. `rt_trigger.ready = !datain_hold.valid` | `um_latch`、`stream_tbl.reissue`（credit_wake 写口） | D1 |

### U2 · Stream create（Stream_table 的 create 写口）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `um_latch`、`stream_ptr`、`task_chain[0]`、`chain_masks`、`ts_regs` | 1. `!hit ∧ !weights_mode ∧ trigger_task_chain_en` → `create_ctx = {C1, slot = tail_ptr, entry}`；`tail_ptr − head_ptr < STREAM_NUM` 才开始，否则等<br>2. C1～C3：`entry = {valid, user_id, task_id = 0, task_chain[0] 的 unit / dsa_en / pc, compute, fsm = (Task0 为 datain 或 reissue → WAIT；self_start → RDY), done_bitmap = 0, reissue = 0, end = task_chain[0].END}`；一次写入 `stream_tbl[slot]`<br>3. C3：`tail_ptr += 1` | `stream_tbl`、`stream_ptr` | D3 |

### D1 · DataIn Hold（DataIn_task_table）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `um_latch`、`datain_task`、`datain_hold` | 1. `um_latch.valid ∧ !datain_hold.valid` → `datain_hold = {1, slot, user_id, task_pc = datain_task.TASK_PC}`（Global DataIn PC，不进 stream）<br>2. `datain_offer = datain_hold`（给 DTE_Arb）；`datain_pop` → `datain_hold.valid = 0`<br>3. datain 立即就绪，不走 READY → INFLY，DTE 接收不改 stream 项 | `datain_hold`、`datain_offer` | D1 |

### A1 · Arb 选中（DTE_Arb / MU_Arb / VU_Arb）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `stream_tbl`、`stream_ptr.head_ptr`、`datain_offer`、`arb_ctx[u]`、`cm_reissue_ctx` | 1. `arb_ctx[u].locked` → 保持（非抢占）<br>2. `cand = { i : valid ∧ fsm == RDY ∧ task_unit == u }`；从 `head_ptr` 环扫取最老<br>3. DTE：Reissue 候选（`cm_reissue_ctx` 已获 pulse）最高；否则 DataIn（`datain_offer.valid`）与 Generated 按各自 stream 相对 `head_ptr` 的年龄，同一 stream 时 Generated 优先<br>4. `task_dsa_en == 0` 的 Generated 任务同样发给 DTE RV core（DTE Local RV）<br>5. `arb_ctx[u] = {locked, slot, task_id, task_pc, is_datain}` | `arb_ctx[u]` | D1 |

### A2 · LUT 查询（CFG_REG）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `arb_ctx[u]`、`task_chain` | 1. `lut_rsp[u] = task_chain[arb_ctx.task_id]`（固定下一拍返回） | `lut_rsp[u]` | D1 |

### A3 · 发命令，等 ACCEPT（Arb 的发送侧）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `arb_ctx[u]`、`lut_rsp[u]`、`rv_task[u].cmd_ready`、`stream_tbl[slot]` | 1. `rv_task[u].cmd_valid = 1`，字段 `{task_pc, stream_id = slot, user_id, task_id, stream_num, dsa_en, is_datain}`，保持不变<br>2. `cmd_ready`（上拍值）→ 握手：Generated → `issue_update[u]`（READY → INFLY）；DataIn → `datain_pop`（只 pop 不改 Map）；`arb_ctx[u].locked = 0`<br>3. 写口失败（同拍冲突）只重试写，不重新仲裁 | `rv_task[u]`、`stream_tbl`（issue 写口）、`datain_pop` | D变长（等 ACCEPT） |

### T1 · Task_done

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `done_ack[6]`、`rt_reduce_done`、`td_hold[3]`、`td_reduce_hold`、`stream_tbl` | 1. 每 Lane（DTE / MU / VU）收本 Lane 的 RV ack 与 DSA ack：`td_hold[l] = {src, stream_id, task_id, user_id}`<br>2. `Classify`：按 `task_chain[task_id].RECV_UNIT` 得 `task_recv_type`（RV 收尾 / DSA 收尾 / 两者都要），非期望来源的 ack 忽略；两者都要时等第二个 ack<br>3. Reduce 任务：DTE ack 只 `consume_only`（不改状态）；`rt_reduce_done` 进 `td_reduce_hold`，按 `user_id` 找 slot，等匹配的 DTE ack 消费后才提交<br>4. `task_id != stream_tbl[slot].task_id`（Future DataIn）→ 只置 `done_bitmap[task_id]`<br>5. C1～C3：`completion_write[l]`：`fsm = FINISH`，`done_bitmap[task_id] = 1`；同 Lane 串行，三 Lane 并行 | `stream_tbl`（completion 写口 ×3） | D3 |

### C1 · Task_ctrl

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `stream_tbl`、`stream_ptr`、`chain_masks`、`task_chain`、`tc_ctx` | 1. C1：从 `head_ptr` 环扫第一个 `valid ∧ fsm == FINISH ∧ end == 0` 的 slot；每拍处理一条<br>2. C2：`SKIP = ~END_MASK & ((DATA_IN_MASK & done_bitmap) \| (REISSUE_MASK & ~reissue))`，DP+P2P 时按 `compute` 匹配计算 task；`next = 优先编码(从 task_id + 1 起第一个 !SKIP 的位)`，一拍完成，连续 skip 不加拍<br>3. C3：`lut_rsp = task_chain[next]`<br>4. C4：`install`：`task_id = next`，unit / dsa_en / pc / is_reissue / end 更新，`fsm = (datain 未完成 → WAIT；reissue → WAIT；否则 RDY)`；End task 不可跳，不生成后继；写口保持到 accepted | `stream_tbl`（install 写口） | D4 |

### K1 · Credit_monitor：Reissue 与 credit 申请

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `stream_tbl`、`cm_reissue_ctx`、`cm_reduce_ctx`、`rt_credit_pulse`、`chain_sw.reduce_num`、`ts_regs.B_core_direction`、`p2p_block_tbl` | 1. `cand = { i : fsm == WAIT ∧ is_reissue ∧ reissue }`，从 `head_ptr` 取最老 → `cm_reissue_ctx = {locked, slot}`；`rt_credit_req.req = {stream, user, task, path, dir, reissue = 1}` 持续拉高到 `req_ready`<br>2. `rt_credit_pulse.stream == slot` → `credit_wake`：`fsm = RDY`，解锁<br>3. 需要 credit 的 dataout 任务（`CREDIT_EN`）：安装后按 `{stream, path, user, dir}` 注册；B core 按 `B_core_direction` 查下游<br>4. Reduce 任务：`cm_reduce_ctx.n = reduce_num`，顺序连续下发 n 笔请求，收全 n 笔 `rt_reduce_done` 才算完成<br>5. P2P 阻塞缓冲：下游无 credit 时任务转“搬进 core mem”并记 `p2p_block_tbl[dir]`；credit 到后续传并归还上游 credit | `rt_credit_req`、`stream_tbl`（credit_wake 写口）、`p2p_block_tbl` | D变长 |

### K2 · Head-only 退休

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `stream_tbl[head_ptr]`、`cm_retire_ctx`、`rt_credit_req.return_accepted` | 1. C6：`stream_tbl[head_ptr].valid ∧ fsm == FINISH ∧ end` → 开始退休；只退队头<br>2. C7～C9：`return_valid = 1, return_user = user_id` 持续到 `return_accepted`（经 Router 向上游归还 Stream credit）<br>3. C10：`rt_retire = {1, user_id}`（Router 与 ReduceModule 各自回收）<br>4. C11：`retirement_update`：清 `valid`，`head_ptr += 1`；`CORE_TYPE ∈ {B, R}` → 该项立即 `SelfStart`（自启动 task0，无用户信息） | `rt_credit_req.return_*`、`rt_retire`、`stream_tbl`（retirement 写口）、`stream_ptr` | D6 |

### G1 · CFG_REG

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `cfg` | 1. 写 `task_chain[i]` → 同时 `VALID = 1`<br>2. 写 `TS_INIT_FINISH` → 检查五项（未配置、多笔 END、VALID 不连续、多笔 SELF_START、普通 core 出现 SELF_START）→ `TS_STATE`；算 `chain_masks`<br>3. `CORE_TYPE ∈ {B, R}` → 复位后直接自启动 16 项：`stream_tbl[0..15] = {valid, user_id = 无, task0 属性, fsm = RDY}`<br>4. `WEIGHTS_MODE ∨ !trigger_task_chain_en` → U2 不建表<br>5. `task_lut_query` 一拍返回 | `task_chain`、`ts_regs`、`chain_masks`、`stream_tbl`、`lut_rsp` | D1 |

***

## 6　参数汇总

```
STREAM_SLOTS          16
TASK_CHAIN_ENTRIES    64
STREAM_NUM            1～16，cfg 配
CREATE_CYCLES         3（安装到 READY）      // 第 3 章 LLD
WAKE_CYCLES           2（A2 查 LUT → A3 发命令）
DONE_CYCLES           3（C1～C3）
INSTALL_CYCLES        4（C1～C4）
RETIRE_CYCLES         6（C6～C11）
LUT_LATENCY           1
USER_ID_WIDTH         10 bit 按 TS 接口（第 8 章有 10 vs 12 的冲突）；端口字段留 16 bit
WRITE_PORT_PRIORITY   六个写口同拍冲突的固定优先级   // 待定，按 TS LLD 时序图
P2P_BLOCK_DIRS        ≤ 3；容量与项数 cfg 配
exe_dest / task_group_id / reduce_num   task_chain 的软件侧属性（chain_sw），硬件位域无   // 待定
```

***

## 7　机制覆盖

### CFG_REG 与 Task LUT

| 机制 | 落点 | 用例 |
| - | - | - |
| TASK_VALID 自动置位 | G1 第 1 条 | `ts_cfg_valid` |
| 配置合规检查写 `TS_STATE` | G1 第 2 条 | `ts_cfg_check` |
| WEIGHTS_MODE / `trigger_task_chain_en = 0` 不启动任务链 | G1 第 4 条 + U2 第 1 条 | `weights_load` |
| Task LUT 一拍返回 | A2 / G1 第 5 条 | `ts_lut_latency` |
| 软件列名对照 | `tables/task_chain.h` 读入时映射 | `task_chain_load` |

### User_Match 与 DataIn_task_table

| 机制 | 落点 | 用例 |
| - | - | - |
| 新旧用户判定；老用户复用 SID 且不改当前任务 / FSM / bitmap / reissue | U1 第 1、4 条 | `ts_user_match` |
| datain 不建表（B / R core） | U1 第 3 条 | `ts_datain_no_create` |
| 注册与反压：Hold 空则登记，满则反压 Router | D1 第 1 条、U1 第 5 条 | `ts_datain_hold` |
| 新 Stream 原子创建；DataIn Task0 初始 WAIT，self_start Task0 初始 READY | U2 第 2 条 | `ts_stream_create` |
| 两种 PC 语义：Task0 PC 入 Stream，Global DataIn PC 入 Hold | U2 / D1 | `ts_two_pcs` |
| DataIn 独立下发，不走 READY → INFLY，DTE 接收不改 Stream | D1 第 3 条、A3 第 2 条 | `ts_datain_independent` |
| 释放：DTE ack 后清 valid，Router 释放 Payload | D1 第 2 条 | `ts_datain_release` |
| datain 立即就绪 | D1 | 同上 |

### Stream_table

| 机制 | 落点 | 用例 |
| - | - | - |
| FIFO 指针：tail 注册、head 释放，粒度 1，上限 stream_num | U2 第 1、3 条、K2 第 4 条 | `ts_stream_fifo` |
| 年龄优先环扫，不按 SID 数值 | A1 第 2 条、C1 第 1 条、K1 第 1 条 | `ts_age_scan` |
| 用户创建、用户释放（end task 完成 → 清 valid、推 head、上游 credit++） | U2、K2 | `ts_stream_lifecycle` |
| task_fsm 全部迁移条件 | U2 第 2 条、C1 第 4 条、A3 第 2 条、T1 第 5 条、K1 第 2 条、K2 | `ts_task_fsm` |
| 任务就绪条件：前序完成且（不需 credit 或 credit 满足） | C1 第 4 条、K1 第 2 条 | `ts_ready_cond` |
| 写端口原子性：请求保持到 accepted；失败重读或只重试写 | 每写口一拍一笔，同拍多端口冲突按固定优先级排队（待定） | `ts_write_ports` |
| Head-only 退休：先还 credit 再清 | K2 | `ts_head_retire` |
| Core Mem 多用户管理：stream_id 绑定分片，统一偏移映射，任务结束释放 | `CoreContext::StreamBase(stream_id)` 供 DSA 地址计算 | `cm_stream_slicing` |

### Task_ctrl

| 机制 | 落点 | 用例 |
| - | - | - |
| 独立推进，无全局 Task Pointer | `stream_tbl.task_id` 每项独立 | — |
| 选择 Stream：head_ptr 环扫 valid && FINISH && end=0 | C1 第 1 条 | `ts_taskctrl_pick` |
| SKIP_MASK 一拍跳过，64 bit 优先编码 | C1 第 2 条 | `ts_skip_mask` |
| 四种跳过场景 | C1 第 2 条 + `um_latch.compute` / `reissue` | `ts_skip_cases` |
| 状态选择：Generated → READY，DataIn → WAIT，Reissue → WAIT | C1 第 4 条 | `ts_install_state` |
| End 不可跳，不生成后继 | C1 第 4 条 | `ts_end_not_skipped` |
| 原子安装 | C1 第 4 条 | `ts_install_atomic` |

### MU_Arb / VU_Arb 与 DTE_Arb

| 机制 | 落点 | 用例 |
| - | - | - |
| 筛选候选：valid && READY && unit 匹配 | A1 第 2 条 | `ts_arb_filter` |
| 环形年龄优先 | A1 第 2 条 | `ts_age_scan` |
| 非抢占保持到 raw ACCEPT | A1 第 1 条、A3 第 1 条 | `ts_arb_hold` |
| ACCEPT 后 READY → INFLY；失败只重试写 | A3 第 2、3 条 | `ts_arb_commit` |
| RV core task_queue 满则反压 | `rv_task[u].cmd_ready` | `ts_arb_backpressure` |
| DTE 三源：Reissue 最高；DataIn 与 Generated 按年龄，同 SID Generated 优先 | A1 第 3 条 | `ts_dte_arb_priority` |
| DataIn 出槽只 pop 不改 Map | A3 第 2 条 | `ts_datain_independent` |
| `task_dsa_en=0` 的 Generated 任务下发 DTE Local RV | A1 第 4 条 | `ts_dte_local_rv` |
| 并行 3 个 task 下发 | 三个 Arb 各自独立打拍 | `ts_three_issue` |

### Credit_monitor

| 机制 | 落点 | 用例 |
| - | - | - |
| 筛选 Reissue：WAIT && is_reissue && reissue 最老 | K1 第 1 条 | `ts_reissue_pick` |
| 持续请求到脉冲；Router 统一记账；唤醒 WAIT → READY | K1 第 1、2 条 | `ts_reissue_wake` |
| Head-only 退休：先持续发 `stream_credit_return` 到 accepted，再清 valid、head+1 | K2 | `ts_head_retire` |
| 双路径并行 | K1 与 K2 两个独立状态机 | `ts_cm_parallel` |
| dataout credit 申请 | K1 第 3 条 | `ts_credit_request` |
| Broadcast Reissue：下游不可收置标；查 credit 选最老重发；并行、高优先；未成功不覆盖不释放 | U1 第 4 条 + K1 第 1 条 + A1 第 3 条 + Core Mem 保留 | `broadcast_reissue` |
| P2P 重发：datain + dataout 两 task；path_id 匹配越过；需重发时 dataout 请求 credit | C1 第 2 条 + K1 第 3 条 | `p2p_reissue` |
| DP+P2P：按“是否计算”匹配计算任务 | C1 第 2 条 | `dp_p2p_skip` |
| Reduce credit N 笔 | K1 第 4 条 | `reduce_n_credits` |
| P2P 阻塞缓冲 | K1 第 5 条 + Router 文档的 CoreMem 重发 | `p2p_block_buffer` |
| TS credit 返还：退休时经 Router 向上游返还 | K2 第 2 条 | `ts_credit_return` |
| B core 搬出前按 `B_core_direction` 查下游 credit | K1 第 3 条 | `bcore_downstream_credit` |

### Task_done

| 机制 | 落点 | 用例 |
| - | - | - |
| 七路独立处理 | T1 第 1 条，三条 Lane | `ts_done_seven_lanes` |
| 完成来源判定 `task_recv_type` | T1 第 2 条 | `ts_done_recv_type` |
| Reduce 完成分离：DTE ACK consume_only，Router Done 才 FINISH | T1 第 3 条 | `ts_reduce_done_split` |
| 无序汇合：Router Done Hold 到 DTE ACK 消费 | T1 第 3 条 | `ts_reduce_unordered` |
| Router UID 匹配：按 user_id 找 SID | T1 第 3 条 | `ts_uid_match` |
| Future DataIn：只更新 done_bitmap | T1 第 4 条 | `ts_future_datain` |
| 直接写 Map，三条写 Lane，同 Lane 串行 | T1 第 5 条 | `ts_done_write_lanes` |
| 三种收尾 | T1 第 2 条 | `ts_three_endings` |
| 异步 DataIn：只更新 bitmap 不推 task_id；后续任务需前序 flag 全有效 | T1 第 4 条 + C1 第 4 条 | `ts_async_datain` |

### 自发创建任务链（B core / R core）

| 机制 | 落点 | 用例 |
| - | - | - |
| 双任务链 + 软件映射表：datain flag 软件维护，后续链 RV 轮询 | kernel `check_flag` + Share Mem | `bcore_two_chains` |
| check task 不调 DSA，RV 长期工作 | kernel | 同上 |
| 复位自启动 16 项，无用户信息，等 RV 返回 user_id 后更新 | G1 第 3 条 + T1 收到 RV 回的 user_id | `ts_self_start` |
| 顺序激活与清 flag | kernel + G1 第 3 条 | `ts_self_start_order` |
| 退休后再激活 | K2 第 4 条 | `ts_self_start_reactivate` |
| B core 查下游 credit | K1 第 3 条 | `bcore_downstream_credit` |
| B core 链一不查 TS credit（落 Matrix Mem） | U1 第 3 条 | `bcore_datain_no_credit` |
| R core 乱序：谁先集齐谁先走 | kernel 扫 `arrive_num` | `rcore_out_of_order` |
| R core 搬运原子化 | DTE 文档 Commit 按 UserID + 方向串行 | `rcore_atomic_move` |

***

## 8　取舍

* **Stream_table 为什么做成被动的存储模块，不把六个写口的逻辑拆到各模块里**
  * 写口原子性（请求保持到 accepted、失败只重试写）是一处集中的仲裁
  * 各模块只持有自己那一笔待写请求
* **Arb 的 A2 为什么单独占一拍查 LUT，不并进 A1**
  * 对应 LLD 时序里“唤醒 2 拍”的 C2 → C3
