# 7　DTE DSA

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../07-latch-建模计划.md)）的建模方式之上

给实现 DTE 的人：八个独立打拍的模块各自的端口、存储器、流水线与逐级行为、参数与机制。

八个模块：

* 任务入口：Header Parser、Commit
* 数据通路：TaskQueue ×4、Lane ×4（含 AGCU）、中间 Buffer
* 完成：Completion RS 与 Done Pending
* 附属存储与旁路：Hmem 与 LUT、topK 与 shareMem 写

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《DTE 数据搬运引擎》全篇
* 《Core 内硬件》：“进 Core”“出 Core”
* 《软件栈》：“阻塞重传四步”

***

## 1　定位与边界

DTE 只做搬运，两个物理 Channel 拆出四条 Lane：

* **inbound**
  * RD_CH0：从 CoreStation 收帧
  * WR_CH0：写 Core Mem / Matrix Mem
* **outbound**
  * RD_CH1：读 Core Mem / Matrix Mem
  * WR_CH1：发 CoreStation 或写回 Core Mem

任务从两个入口来，都在 Commit 配对接纳成一对 RD / WR 子上下文：

* Router 入站帧的 Header 经 Header Parser 生成 Descriptor
* DTE RV core 经寄存器写加 trigger 生成 Descriptor

完成经 Completion RS 按任务 Join，Done Pending 与 TS 握手 Exactly-once。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1180 620" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs>
    <marker id="d0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker>
    <marker id="d0s" markerWidth="9" markerHeight="9" refX="1" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#475569"/></marker>
  </defs>
  <rect x="0" y="0" width="1180" height="620" fill="#ffffff"/>
  <text x="20" y="28" font-size="12" fill="#111827">DTE DSA · 第 0 层</text>

  <polygon points="30,90 130,90 120,126 20,126" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="112" font-size="10.5" fill="#374151" text-anchor="middle">cs_datain</text>
  <polygon points="30,170 130,170 120,206 20,206" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="192" font-size="10.5" fill="#374151" text-anchor="middle">dsa_cfg · dsa_rsp</text>
  <text x="75" y="224" font-size="9" fill="#6b7280" text-anchor="middle">← DTE RV core</text>
  <polygon points="30,290 130,290 120,326 20,326" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="312" font-size="10.5" fill="#374151" text-anchor="middle">rt_credit_view</text>
  <polygon points="30,500 130,500 120,536 20,536" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="522" font-size="10.5" fill="#374151" text-anchor="middle">cfg</text>

  <rect x="200" y="70" width="170" height="60" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="212" y="94" font-size="12" fill="#111827">Header Parser</text>
  <text x="212" y="112" font-size="10" fill="#475569">一帧一任务 · Drop Frame</text>
  <line x1="132" y1="108" x2="198" y2="108" stroke="#475569" marker-end="url(#d0)"/>

  <rect x="200" y="160" width="170" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="212" y="184" font-size="12" fill="#111827">Commit</text>
  <text x="212" y="202" font-size="10" fill="#475569">Bank0 / Bank1 · 配对接纳</text>
  <text x="212" y="218" font-size="10" fill="#475569">地址展开 · PendingTaskQ</text>
  <text x="212" y="234" font-size="10" fill="#475569">task_id 分配</text>
  <line x1="132" y1="188" x2="198" y2="188" stroke="#475569" marker-start="url(#d0s)" marker-end="url(#d0)"/>
  <line x1="285" y1="132" x2="285" y2="158" stroke="#475569" marker-end="url(#d0)"/>
  <line x1="132" y1="308" x2="198" y2="240" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#d0)"/>

  <rect x="420" y="60" width="180" height="100" fill="#f8fafc" stroke="#374151"/>
  <rect x="424" y="64" width="172" height="92" fill="none" stroke="#374151"/>
  <g stroke="#374151"><line x1="440" y1="64" x2="440" y2="156"/></g>
  <text x="448" y="84" font-size="11" fill="#111827">tq[RD0] · tq[WR0]</text>
  <text x="448" y="100" font-size="11" fill="#111827">tq[RD1] · tq[WR1]</text>
  <text x="448" y="118" font-size="10" fill="#475569">FIFO 各 16 × 子上下文</text>
  <text x="448" y="134" font-size="10" fill="#475569">1W1R · 同拍配对入队</text>
  <line x1="372" y1="200" x2="418" y2="120" stroke="#475569" marker-end="url(#d0)"/>

  <rect x="420" y="190" width="180" height="70" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="432" y="214" font-size="12" fill="#111827">Lane RD0 + AGCU</text>
  <text x="432" y="232" font-size="10" fill="#475569">收 Payload → inbound_buf</text>
  <rect x="420" y="280" width="180" height="70" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="432" y="304" font-size="12" fill="#111827">Lane WR0 + AGCU</text>
  <text x="432" y="322" font-size="10" fill="#475569">inbound_buf → CM / MM 写</text>
  <text x="432" y="338" font-size="10" fill="#475569">topK 复制 · shareMem 写</text>
  <rect x="420" y="380" width="180" height="70" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="432" y="404" font-size="12" fill="#111827">Lane RD1 + AGCU</text>
  <text x="432" y="422" font-size="10" fill="#475569">CM / MM 读 · outstanding</text>
  <rect x="420" y="470" width="180" height="70" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="432" y="494" font-size="12" fill="#111827">Lane WR1 + AGCU</text>
  <text x="432" y="512" font-size="10" fill="#475569">→ Router TX 或 WR1 → CM</text>
  <text x="432" y="528" font-size="10" fill="#475569">vc_out_buf ×4</text>
  <line x1="510" y1="162" x2="510" y2="188" stroke="#475569" marker-end="url(#d0)"/>
  <line x1="530" y1="162" x2="530" y2="278" stroke="#475569" stroke-dasharray="2 3" marker-end="url(#d0)"/>
  <line x1="560" y1="162" x2="560" y2="378" stroke="#475569" stroke-dasharray="2 3" marker-end="url(#d0)"/>
  <line x1="580" y1="162" x2="580" y2="468" stroke="#475569" stroke-dasharray="2 3" marker-end="url(#d0)"/>

  <rect x="660" y="190" width="150" height="160" fill="#f8fafc" stroke="#374151"/>
  <rect x="664" y="194" width="142" height="152" fill="none" stroke="#374151"/>
  <g stroke="#374151"><line x1="680" y1="194" x2="680" y2="346"/></g>
  <text x="688" y="214" font-size="11" fill="#111827">inbound_buf</text>
  <text x="688" y="230" font-size="10" fill="#475569">FIFO 16×256 B</text>
  <text x="688" y="270" font-size="11" fill="#111827">outbound_buf</text>
  <text x="688" y="286" font-size="10" fill="#475569">FIFO 16×256 B</text>
  <text x="688" y="320" font-size="10" fill="#475569">credit 背压 · 任务边界</text>
  <line x1="602" y1="225" x2="658" y2="225" stroke="#475569" marker-end="url(#d0)"/>
  <line x1="658" y1="315" x2="602" y2="315" stroke="#475569" marker-end="url(#d0)"/>
  <line x1="602" y1="415" x2="700" y2="352" stroke="#475569" marker-end="url(#d0)"/>
  <line x1="720" y1="352" x2="602" y2="505" stroke="#475569" marker-end="url(#d0)"/>

  <rect x="860" y="60" width="200" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="872" y="84" font-size="12" fill="#111827">Completion RS · Done Pending</text>
  <text x="872" y="102" font-size="10" fill="#475569">四源事件 · 按 task Join</text>
  <text x="872" y="118" font-size="10" fill="#475569">Exactly-once 握手</text>
  <polyline points="602,200 630,200 630,110 858,110" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#d0)"/>
  <text x="740" y="104" font-size="9" fill="#6b7280" text-anchor="middle">lane 事件 ×4</text>
  <polygon points="1080,80 1170,80 1160,116 1070,116" fill="#f8fafc" stroke="#374151"/>
  <text x="1120" y="102" font-size="10.5" fill="#374151" text-anchor="middle">ts_done</text>
  <line x1="1062" y1="98" x2="1070" y2="98" stroke="#475569" marker-end="url(#d0)"/>

  <rect x="860" y="190" width="200" height="100" fill="#f8fafc" stroke="#374151"/>
  <rect x="864" y="194" width="192" height="92" fill="none" stroke="#374151"/>
  <text x="872" y="214" font-size="11" fill="#111827">Hmem · LUT</text>
  <text x="872" y="230" font-size="10" fill="#475569">sw_hdr 16×64×16 B · hw_static 64</text>
  <text x="872" y="246" font-size="10" fill="#475569">hw_dyn 16 · path_id / task_len 表</text>
  <text x="872" y="262" font-size="10" fill="#475569">rt_copy 64 · stream_ledger</text>
  <line x1="812" y1="240" x2="858" y2="240" stroke="#475569" stroke-dasharray="4 3" marker-start="url(#d0s)" marker-end="url(#d0)"/>

  <polygon points="880,390 980,390 970,426 870,426" fill="#f8fafc" stroke="#374151"/>
  <text x="925" y="412" font-size="10.5" fill="#374151" text-anchor="middle">cmem_rd/wr · mmem_rd/wr</text>
  <polygon points="880,460 980,460 970,496 870,496" fill="#f8fafc" stroke="#374151"/>
  <text x="925" y="482" font-size="10.5" fill="#374151" text-anchor="middle">cs_dataout</text>
  <polygon points="880,530 980,530 970,566 870,566" fill="#f8fafc" stroke="#374151"/>
  <text x="925" y="552" font-size="10.5" fill="#374151" text-anchor="middle">smem_wr · topk_wr</text>
  <line x1="602" y1="330" x2="870" y2="400" stroke="#475569" marker-end="url(#d0)"/>
  <line x1="602" y1="430" x2="870" y2="410" stroke="#475569" marker-start="url(#d0s)"/>
  <line x1="602" y1="500" x2="870" y2="478" stroke="#475569" marker-end="url(#d0)"/>
  <line x1="602" y1="520" x2="870" y2="420" stroke="#475569" marker-end="url(#d0)"/>
  <line x1="602" y1="340" x2="870" y2="545" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#d0)"/>
  <line x1="132" y1="518" x2="870" y2="240" stroke="#475569" stroke-dasharray="2 3"/>

  <text x="20" y="600" font-size="10.5" fill="#374151">实线 = 256 B/拍数据；虚线 = 控制。Router 入口只在 CH0，RV core 配置的任务按 Route 走 CH0 或 CH1。</text>
</svg>
```

***

## 2　接口

```
port cs_datain (slave, AXI-Stream-like, clk)      // CoreStation → RD_CH0，每拍 1 flit（256 B）
  in  valid
  out ready                                         // = Header Parser 空闲且 Commit 已接纳（Payload 门控）或正在 Drop
  in  last · bytes[8:0] · msg
port cs_dataout (master, AXI-Stream-like + credit, clk)  // WR_CH1 → CoreStation
  out valid
  in  ready
  out vc[1:0] · last · bytes[8:0] · msg
  in  vc_credit_release_valid · vc_credit_release_vc[1:0]
port dsa_cfg (slave, valid/ready, clk)            // DTE RV core 的 dsaw / dsar
  in  req_valid
  out req_ready                                     // = 配置 Bank 有空（Bank0 或 Bank1）
  in  req_we · req_addr[15:0] · req_data[31:0] · req_stream_id[3:0] · req_user_id[15:0] · req_task_id[5:0] · req_rq_idx[2:0]
port dsa_rsp (master, 脉冲, clk)
  out valid · rq_idx[2:0] · data[31:0]
port rt_credit_view (slave, 电平, clk)            // Router 广播的 credit 视图，供出 core 前置申请判断
  in  stream_ok[16][4] · reduce_credit[16][3] · vc_credit[4]   // 下游方向：stream 按 left/right/mid/Core，reduce 按三个 R2R 方向
port cmem_rd / cmem_wr (master, valid/ready, clk) // 存储文档 cmem_dte_rd / cmem_dte_wr 的镜像
port mmem_rd / mmem_wr (master, valid/ready, clk) // 存储文档 mmem_dte_rd / mmem_dte_wr 的镜像
port smem_wr (master, valid/ready, clk)           // 存储文档 smem_dte_wr 的镜像
port topk_wr (master, valid/ready, clk)           // → MU topK_ep_table
  out req_valid · req_stream_id[3:0] · req_idx[3:0] · req_expert[15:0] · req_weight[31:0]
  in  req_ready
port ts_done (master, 脉冲, clk)                  // → TS done_ack[DTE_DSA]
  out valid · stream_id[3:0] · task_id[5:0] · user_id[15:0]
port cfg (slave, ctrl_noc 写事务, clk)            // RouterTable 副本、硬件包头静态表、软件包头表、LUT
  in  cfg_valid · cfg_addr[15:0] · cfg_we · cfg_wdata[31:0]
  out cfg_rdata[31:0]
```

***

## 3　存储器

```
mem cfg_bank[2]      FF        {valid, src[1:0], transfer_mode[2:0], scale_valid, topk_valid, router_ep_count[3:0], src_addr[25:0], dst_addr[25:0], src_base_addr[25:0], dst_base_addr[25:0], stream_stride[19:0], src_stream_stride[19:0], dst_stream_stride[19:0], data_len[15:0], hw_header_op, header_addr[25:0], header_base_addr[19:0], scale_base_addr[19:0], scale_stride[19:0], topk_base_addr[19:0], sharemem_waddr[14:0], sharemem_data[31:0], trigger{last, no_ack}, ids}  1RW  写 trigger 提交  复位空  // Bank0 > Bank1
mem hp_latch         级间 latch {valid, header{version, route[2:0], dst_addr[25:0], byte_count[15:0], task_id[5:0], stream_id[3:0], attr}, drop}  —  每拍覆写  —   // Header Parser → Commit
mem hp_state         FF        {state[1:0], acc_bytes[15:0]}      1RW    等 Header / 收 Payload / Drop   复位 等 Header
mem task_id_alloc    FF        bitmap 64                          1RW    Commit 分配，Done 释放           复位 0
mem pending_taskq    FIFO      深 PENDING_DEPTH × descriptor       1W1R   出 core 任务等 Stream / Rmem credit  复位空
mem tq[4]            FIFO      深 DTE_TQ_DEPTH × {task_id[5:0], route[2:0], addr[25:0], len[15:0], mask[7:0], first, last, ids}  1W1R  满 → Commit 保持  复位空
mem active_ctx[4]    1-deep 寄存器 {valid, task_id[5:0], route[2:0], cursor[25:0], remain[15:0], first, last}  1W1R  Lane 激活写，issue_done 清  复位空
mem agcu_cmd[4]      级间 latch {valid, addr[25:0], bytes[8:0], last}   —   每拍覆写               —          // AGCU → 访存 / 流口
mem rd1_outstanding  FF        计数 ≤ CH1_OUTSTANDING_MAX          1RW    rd_req +1，response −1          复位 0
mem rd1_rsp_hold     1-deep 寄存器 {valid, tag[7:0], data[2047:0]}  1W1R   response 到 → 下拍入 outbound_buf  复位空
mem wr_outstanding[2] FF       计数                                1RW    wr_req +1，bvalid −1            复位 0
mem inbound_buf      FIFO      深 DTE_BUF_DEPTH × {data[2047:0], task_id[5:0], bytes[8:0], last}  1W1R  满 → cs_datain.ready=0  复位空
mem outbound_buf     FIFO      深 DTE_BUF_DEPTH × 同上             1W1R   credit 满 → RD1 不再发           复位空
mem vc_out_buf[4]    FIFO      深 VC_OUT_DEPTH × flit              1W1R   单 VC 阻塞只阻塞本队列           复位空
mem completion_rs    FF 阵列   DTE_RS_DEPTH × {valid, task_id[5:0], rd_issue, rd_drain, wr_issue, wr_drain, buf_drained, last, no_ack, ids}  多W1R  四源事件同拍写  复位空
mem done_pending     FIFO      深 DTE_DP_DEPTH × {task_id[5:0], ids}  1W1R  多命中同拍全部入队              复位空
mem sw_hdr           SRAM      16 × 64 × 16 B                      1RW    cfg 初始化                      复位未定义 // 软件包头表
mem hw_static        FF 阵列   64 × {path_id[5:0], size[15:0]}     1RW    boot 配置                       复位 0
mem hw_dyn           FF 阵列   16 × path_core_mask[15:0]           1RW    DTE core 配置                   复位 0
mem lut              FF 阵列   {path_id_table 64, task_len_table 64}  1RW  cfg                             复位 0
mem rt_copy          FF 阵列   64 × 表项                           1R1W   软件经 cfg 写（与 Router 同内容） 复位 0
mem stream_ledger    FF 阵列   16 × 4 × valid                      1RW    随 rt_credit_view 更新            复位 0     // User Resource Cache Table，方向按 left/right/mid/Core
mem topk_stage       1-deep 寄存器 {stream_id[3:0], n[3:0], entries[16]}  1W1R  RD0 尾段抽出，WR0 末段写 MU  复位空
mem profile          FF        {setup, issue, drain, report, active, stall}[31:0] 每 task  1RW  剖析计数   复位 0
```

***

## 4　流水线总览

* inbound 一条、outbound 一条，共用 Commit 与 Completion RS
* 一次搬运 `ceil(size / 256)` 拍，加两端延迟：Cmem 13、Mmem 端到端 50、Router 10

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1180 470" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="g0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="1180" height="470" fill="#ffffff"/>
  <text x="20" y="26" font-size="12" fill="#111827">DTE · 第 1 层（前段按比例；Lane 的搬运段与访存延迟标非按比例）</text>
  <g stroke="#e5e7eb"><line x1="140" y1="40" x2="140" y2="440"/><line x1="216" y1="40" x2="216" y2="440"/><line x1="292" y1="40" x2="292" y2="440"/><line x1="368" y1="40" x2="368" y2="440"/></g>
  <g font-size="8.5" fill="#6b7280"><text x="140" y="50">t0</text><text x="216" y="50">t1</text><text x="292" y="50">t2</text><text x="368" y="50">t3</text></g>

  <!-- inbound -->
  <polygon points="24,80 122,80 114,112 16,112" fill="#f8fafc" stroke="#374151"/>
  <text x="69" y="100" font-size="10" fill="#374151" text-anchor="middle">cs_datain</text>
  <line x1="122" y1="96" x2="138" y2="96" stroke="#475569" marker-end="url(#g0)"/>
  <rect x="144" y="70" width="68" height="52" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="148" y="66" font-size="8.5" fill="#6b7280">H1</text><text x="210" y="66" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="150" y="90" font-size="11" fill="#111827">Header</text>
  <text x="150" y="104" font-size="9.5" fill="#475569">Parser</text>
  <rect x="220" y="70" width="68" height="52" fill="#f1f5f9" stroke="#334155"/>
  <rect x="220" y="70" width="68" height="14" fill="#334155"/>
  <text x="254" y="81" font-size="9.5" fill="#ffffff" text-anchor="middle">hp_latch</text>
  <text x="224" y="98" font-size="9" fill="#475569">header · drop</text>
  <line x1="212" y1="96" x2="218" y2="96" stroke="#475569" marker-end="url(#g0)"/>
  <rect x="296" y="70" width="68" height="52" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="300" y="66" font-size="8.5" fill="#6b7280">C1</text><text x="362" y="66" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="302" y="90" font-size="11" fill="#111827">Commit</text>
  <text x="302" y="104" font-size="9.5" fill="#475569">配对接纳</text>
  <line x1="288" y1="96" x2="294" y2="96" stroke="#475569" marker-end="url(#g0)"/>
  <rect x="372" y="60" width="90" height="72" fill="#f8fafc" stroke="#374151"/>
  <rect x="376" y="64" width="82" height="64" fill="none" stroke="#374151"/>
  <g stroke="#374151"><line x1="388" y1="64" x2="388" y2="128"/></g>
  <text x="394" y="84" font-size="9" fill="#111827">tq[RD0]</text>
  <text x="394" y="98" font-size="9" fill="#111827">tq[WR0]</text>
  <text x="394" y="114" font-size="8.5" fill="#475569">FIFO 16</text>
  <line x1="364" y1="96" x2="370" y2="96" stroke="#475569" marker-end="url(#g0)"/>

  <rect x="490" y="70" width="140" height="52" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="494" y="66" font-size="8.5" fill="#6b7280">L1</text><text x="628" y="66" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="496" y="90" font-size="11" fill="#7c2d12">RD0：收 Payload</text>
  <text x="496" y="104" font-size="9.5" fill="#92400e">每拍 1 flit → inbound_buf</text>
  <line x1="462" y1="96" x2="488" y2="96" stroke="#475569" marker-end="url(#g0)"/>
  <rect x="650" y="60" width="100" height="72" fill="#f8fafc" stroke="#374151"/>
  <rect x="654" y="64" width="92" height="64" fill="none" stroke="#374151"/>
  <g stroke="#374151"><line x1="666" y1="64" x2="666" y2="128"/><line x1="678" y1="64" x2="678" y2="128"/></g>
  <text x="684" y="86" font-size="9" fill="#111827">inbound_buf</text>
  <text x="684" y="100" font-size="8.5" fill="#475569">16×256 B</text>
  <text x="684" y="114" font-size="8.5" fill="#475569">1W1R</text>
  <line x1="630" y1="96" x2="648" y2="96" stroke="#475569" marker-end="url(#g0)"/>
  <rect x="770" y="70" width="150" height="52" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="774" y="66" font-size="8.5" fill="#6b7280">L2</text><text x="918" y="66" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="776" y="90" font-size="11" fill="#7c2d12">WR0：写 CM / MM</text>
  <text x="776" y="104" font-size="9.5" fill="#92400e">每拍 1 请求；末段 topK / shareMem</text>
  <line x1="750" y1="96" x2="768" y2="96" stroke="#475569" marker-end="url(#g0)"/>
  <polygon points="940,80 1040,80 1030,112 930,112" fill="#f8fafc" stroke="#374151"/>
  <text x="985" y="100" font-size="10" fill="#374151" text-anchor="middle">cmem_wr / mmem_wr</text>
  <line x1="920" y1="96" x2="930" y2="96" stroke="#475569" marker-end="url(#g0)"/>

  <!-- outbound -->
  <polygon points="24,200 122,200 114,232 16,232" fill="#f8fafc" stroke="#374151"/>
  <text x="69" y="220" font-size="10" fill="#374151" text-anchor="middle">dsa_cfg</text>
  <line x1="122" y1="216" x2="138" y2="216" stroke="#475569" marker-end="url(#g0)"/>
  <rect x="144" y="190" width="68" height="52" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="148" y="186" font-size="8.5" fill="#6b7280">C0</text><text x="210" y="186" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="150" y="210" font-size="11" fill="#111827">寄存器写</text>
  <text x="150" y="224" font-size="9.5" fill="#475569">cfg_bank</text>
  <polyline points="212,216 254,216 254,150 330,150 330,124" fill="none" stroke="#475569" marker-end="url(#g0)"/>
  <text x="258" y="146" font-size="9" fill="#6b7280">trigger → C1</text>
  <rect x="372" y="180" width="90" height="72" fill="#f8fafc" stroke="#374151"/>
  <rect x="376" y="184" width="82" height="64" fill="none" stroke="#374151"/>
  <g stroke="#374151"><line x1="388" y1="184" x2="388" y2="248"/></g>
  <text x="394" y="204" font-size="9" fill="#111827">tq[RD1]</text>
  <text x="394" y="218" font-size="9" fill="#111827">tq[WR1]</text>
  <text x="394" y="234" font-size="8.5" fill="#475569">FIFO 16</text>
  <line x1="330" y1="124" x2="330" y2="216" stroke="#475569"/><line x1="330" y1="216" x2="370" y2="216" stroke="#475569" marker-end="url(#g0)"/>
  <rect x="490" y="190" width="140" height="52" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="494" y="186" font-size="8.5" fill="#6b7280">L3</text><text x="628" y="186" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="496" y="210" font-size="11" fill="#7c2d12">RD1：读 CM / MM</text>
  <text x="496" y="224" font-size="9.5" fill="#92400e">outstanding ≤ buf credit</text>
  <line x1="462" y1="216" x2="488" y2="216" stroke="#475569" marker-end="url(#g0)"/>
  <rect x="650" y="180" width="100" height="72" fill="#f8fafc" stroke="#374151"/>
  <rect x="654" y="184" width="92" height="64" fill="none" stroke="#374151"/>
  <g stroke="#374151"><line x1="666" y1="184" x2="666" y2="248"/><line x1="678" y1="184" x2="678" y2="248"/></g>
  <text x="684" y="206" font-size="9" fill="#111827">outbound_buf</text>
  <text x="684" y="220" font-size="8.5" fill="#475569">16×256 B</text>
  <line x1="630" y1="216" x2="648" y2="216" stroke="#475569" marker-end="url(#g0)"/>
  <rect x="770" y="190" width="150" height="52" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="774" y="186" font-size="8.5" fill="#6b7280">L4</text><text x="918" y="186" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="776" y="210" font-size="11" fill="#7c2d12">WR1：Router TX / CM</text>
  <text x="776" y="224" font-size="9.5" fill="#92400e">vc_out_buf ×4 · Route 定出口</text>
  <line x1="750" y1="216" x2="768" y2="216" stroke="#475569" marker-end="url(#g0)"/>
  <polygon points="940,200 1040,200 1030,232 930,232" fill="#f8fafc" stroke="#374151"/>
  <text x="985" y="220" font-size="10" fill="#374151" text-anchor="middle">cs_dataout / cmem_wr</text>
  <line x1="920" y1="216" x2="930" y2="216" stroke="#475569" marker-end="url(#g0)"/>
  <polygon points="940,130 1040,130 1030,162 930,162" fill="#f8fafc" stroke="#374151"/>
  <text x="985" y="150" font-size="10" fill="#374151" text-anchor="middle">cmem_rd / mmem_rd</text>
  <polyline points="560,190 560,146 930,146" fill="none" stroke="#475569" marker-end="url(#g0)"/>

  <!-- completion -->
  <rect x="490" y="310" width="200" height="52" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="494" y="306" font-size="8.5" fill="#6b7280">R1</text><text x="688" y="306" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="496" y="330" font-size="11" fill="#111827">Completion RS：Join</text>
  <text x="496" y="344" font-size="9.5" fill="#475569">RD 与 WR 都满足；outbound 还要 buf 排空</text>
  <polyline points="560,242 560,280 540,280 540,308" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#g0)"/>
  <polyline points="845,122 845,280 600,280 600,308" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#g0)"/>
  <polyline points="845,242 845,290 640,290 640,308" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#g0)"/>
  <text x="700" y="276" font-size="9" fill="#6b7280">issue_done / drain_done ×4</text>
  <rect x="710" y="310" width="80" height="52" fill="#f8fafc" stroke="#374151"/>
  <rect x="714" y="314" width="72" height="44" fill="none" stroke="#374151"/>
  <g stroke="#374151"><line x1="726" y1="314" x2="726" y2="358"/></g>
  <text x="732" y="334" font-size="9" fill="#111827">done_pending</text>
  <text x="732" y="348" font-size="8.5" fill="#475569">FIFO 16</text>
  <line x1="690" y1="336" x2="708" y2="336" stroke="#475569" marker-end="url(#g0)"/>
  <rect x="810" y="310" width="110" height="52" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="814" y="306" font-size="8.5" fill="#6b7280">P1</text><text x="918" y="306" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="816" y="330" font-size="11" fill="#111827">Done Pending</text>
  <text x="816" y="344" font-size="9.5" fill="#475569">逐项 · last 才报</text>
  <line x1="790" y1="336" x2="808" y2="336" stroke="#475569" marker-end="url(#g0)"/>
  <polygon points="940,320 1040,320 1030,352 930,352" fill="#f8fafc" stroke="#374151"/>
  <text x="985" y="340" font-size="10" fill="#374151" text-anchor="middle">ts_done</text>
  <line x1="920" y1="336" x2="930" y2="336" stroke="#475569" marker-end="url(#g0)"/>

  <text x="20" y="410" font-size="10" fill="#374151">Read-ahead：RD 领先 WR 受 Buffer credit、outstanding 上限、任务边界数约束；四条 Lane 独立激活，只暂停受影响的 Lane。</text>
  <text x="20" y="428" font-size="10" fill="#374151">MM → CM（Inner Flow）走 RD1 + WR1，WR1 出口切到 cmem_wr；读、Buffer、写重叠。</text>
</svg>
```

***

## 5　逐级行为

### H1 · Header Parser

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `cs_datain`、`hp_state`、Commit 的 `bank_free` 与 `tq/rs` 余量 | 1. `state == 等 Header ∧ valid`：`header = msg.header`；`Check`：version、route ∈ {router→MM, router→CM}、dst_addr、byte_count、身份不重复；不合法 → `drop = 1`，`state = Drop`<br>2. `ready = (state == 等 Header ∧ bank_free ∧ tq[RD0]/tq[WR0]/rs 可用) ∨ state == Drop`（Payload 门控：Commit 成功前不收 Payload）<br>3. `hp_latch = {valid, header, drop}`；`byte_count == 0` 的纯包头任务：Header beat 同时为 last<br>4. `state == 收 Payload`：`acc_bytes += bytes`；`last` → 与 `byte_count` 比较（不等报错），`state = 等 Header`<br>5. `state == Drop`：只消费到 `last`，不生成 Descriptor、不发存储器请求 | `hp_latch`、`hp_state`、`cs_datain.ready` | D1 |

### C0 · 寄存器写（Commit 的 RV core 入口）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `dsa_cfg`、`cfg_bank[2]` | 1. `req_ready = ∃ bank 空`；`req_we` → 写 `cfg_bank[b]` 对应字段（`b`：RV core 优先 Bank1，只剩一个时 Router 优先）<br>2. 写 `trigger` → `cfg_bank[b].valid = 1`，`ids` 取随指令附带的 `stream_id / user_id / task_id`，`last` / `no_ack` 取 trigger 位<br>3. `!req_we`（dsar）→ 下拍 `dsa_rsp = {rq_idx, 该寄存器值}` | `cfg_bank`、`dsa_rsp` | D1 |

### C1 · Commit：配对接纳与地址展开

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `hp_latch`、`cfg_bank[2]`、`pending_taskq`、`tq[4]` 余量、`completion_rs` 余量、`task_id_alloc`、`rt_copy`、`stream_ledger`、`rt_credit_view` | 1. `Pick`：Bank0 > Bank1；Router 配置优先进 Bank0、RV core 优先进 Bank1；只剩一个 Bank 且竞争时 Router 优先；`hp_latch.valid` 视为 Router 配置<br>2. `LanePair = (route ∈ {router→MM, router→CM}) ? (RD0, WR0) : (RD1, WR1)`；出口 `route ∈ {MM→router, CM→router} ? RouterTX : WR1→CM`；`CM→MM` 拒绝<br>3. `Validate`：Route、地址、长度（≤ 32 KB，只支持连续一维）<br>4. 出 core 任务：`path = rt_copy[cfg.path_id]`；`∀ dir ∈ path.dir_mask: stream_ledger[user][dir] ∧ reduce_credit[user][dir] ≥ 整包 flit 数` 不满足 → 进 `pending_taskq`，每拍重查队首<br>5. `Expand`：CM 侧 `dst = dst_base_addr + stream_id × stream_stride`（src 同理），MM 侧 `dst_addr` / `src_addr` 直给不加偏移；软件包头 基址 `+ stream_id × 1 KB + task_id × 16 B`；硬件包头 `header_base_addr + stream_id × 硬件包头长度`；scale `scale_base_addr + stream_id × scale_stride`，长度 `data_len / 32`；topK `topk_base_addr + stream_id × 256 B`，长度 `router_ep_count × 6 B`；`data_len` 含义：router ↔ MM 含 topk + scale + data，router ↔ CM 只含 data<br>6. 接纳条件：`tq[RD].free ∧ tq[WR].free ∧ completion_rs.free`（三项同时，否则整体保持）；R core：同 UserID + 方向串行接纳<br>7. 分配 `task_id`，同拍写两个 `tq` 与 `completion_rs`，Bank 清 | `tq[4]`、`completion_rs`、`pending_taskq`、`task_id_alloc` | D1 |

### L1 · Lane RD0：收 Payload

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `tq[RD0]`、`active_ctx[RD0]`、`cs_datain`、`inbound_buf` 余量 | 1. `!active_ctx.valid ∧ tq 非空` → 激活（独立激活，不等配对 Lane）<br>2. 每拍收 1 flit：`inbound_buf.push({data, task_id, bytes, last})`；`inbound_buf` 满 → `cs_datain.ready = 0`（局部反压，只停本 Lane）<br>3. 尾 flit：`issue_done`，`active_ctx.valid = 0`；`topk_valid` 的任务把 topK 段抽到 `topk_stage`<br>4. Header / Payload 分离：不发 DMA 读 | `inbound_buf`、`topk_stage`、`completion_rs`（rd_issue / rd_drain） | D变长 |

### L2 · Lane WR0：写 CM / MM

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `tq[WR0]`、`active_ctx[WR0]`、`inbound_buf`、`agcu_cmd[WR0]`、`cmem_wr.req_ready` / `mmem_wr.req_ready`、`wr_outstanding[0]` | 1. 激活同 L1<br>2. AGCU：`agcu_cmd = {cursor, bytes, last}`，握手推进 `cursor += 256`，`remain -= 256`；按任务边界消费 `inbound_buf.pop(task_id)`<br>3. `cmem_wr` / `mmem_wr` 每拍 1 请求；`wr_outstanding += 1`，`rsp_valid` → `−1`<br>4. 末 wr_data 发出 → `wr_issue`；全部 bvalid → `wr_drain`（issue_done 后可提前激活下一任务）<br>5. 末段：`topk_stage` 非空 → `topk_wr` 逐项写 MU；`sharemem_waddr` 有效 → `smem_wr`；`hw_header_op == 1` → 存包头到 Hmem<br>6. WR0 → CM 与 WR1 → CM 竞争由 Core Mem 仲裁，未获 grant 保持 | `cmem_wr`、`mmem_wr`、`topk_wr`、`smem_wr`、Hmem、`completion_rs` | D变长（`ceil(size/256)` + Cmem 13 / Mmem 50） |

### L3 · Lane RD1：读 CM / MM

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `tq[RD1]`、`active_ctx[RD1]`、`rd1_outstanding`、`outbound_buf` credit、`cmem_rd` / `mmem_rd`、`rd1_rsp_hold` | 1. 激活同 L1<br>2. `rd1_outstanding < min(CH1_OUTSTANDING_MAX, outbound_buf 余量)` → 发 `rd_req`（每拍 1），`outstanding += 1`，预留 buffer 一项<br>3. `rsp_valid` → `rd1_rsp_hold`，下拍 `outbound_buf.push`，`outstanding −= 1`<br>4. 末 rd_req 发出 → `rd_issue`；末 response 且 `outstanding == 0` → `rd_drain` | `cmem_rd`、`mmem_rd`、`outbound_buf`、`completion_rs` | D变长 |

### L4 · Lane WR1：Router TX 或 CM

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `tq[WR1]`、`active_ctx[WR1]`、`outbound_buf`、`vc_out_buf[4]`、`cs_dataout`、`cmem_wr`、Hmem | 1. 激活同 L1；出口按 Route 任务期固定<br>2. Router 路径：首 flit 携带 Header（`hw_static[task_id]` 改 `path_id`，`hw_dyn[stream_id]` 与软件包头不改）；按 Header VC 进 `vc_out_buf[vc]`，`cs_dataout.ready` 为 1 时每拍发 1 flit；单 VC 阻塞只阻塞本队列<br>3. WR1 → CM：`cmem_wr`（WR1 只允许写 Core Mem，断言）<br>4. 末拍 TX Fire → `wr_issue` 与 `wr_drain`（Router 路径）；CM 路径等全部 bvalid → `wr_drain` | `cs_dataout`、`cmem_wr`、`completion_rs` | D变长（`ceil(size/256)` + Router 10） |

### R1 · Completion RS：Join

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| 四条 Lane 的 `issue_done` / `drain_done` 事件、`inbound_buf` / `outbound_buf` 的“任务边界排空”事件、`completion_rs` | 1. 同拍收全部事件，按 `task_id` 写对应项的位（四源同拍到达）<br>2. `join = rd_drain ∧ wr_drain ∧ (outbound ? buf_drained : 1)`<br>3. 多项同拍命中 → 全部 `done_pending.push`，不覆盖不丢失；释放 `task_id_alloc`、`completion_rs` 项 | `done_pending`、`completion_rs` | D1 |

### P1 · Done Pending

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `done_pending`、`profile` | 1. 每拍出队一项：`last ∧ !no_ack` → `ts_done = {stream_id, task_id, user_id}`（Exactly-once，payload 稳定）；`!last` 或 `no_ack` → 不上报<br>2. 剖析：`setup = first_issue − accept`，`issue = issue_done − first_issue`，`drain = task_done − issue_done`，`report = done_fire − task_done`，`active`、`stall` 计数 | `ts_done`、`profile` | D1 |

***

## 6　参数汇总

```
DTE_TQ_DEPTH            16                 // 待定
DTE_BUF_DEPTH           16 × 256 B（inbound、outbound 各；合计 8 KB）// 待定，MAS 约 256 B × (20～30)T
CH1_OUTSTANDING_MAX     = DTE_BUF_DEPTH
DTE_RS_DEPTH            16                 // 待定
DTE_DP_DEPTH            16                 // 待定
PENDING_DEPTH           16                 // 待定
VC_OUT_DEPTH            8 flit             // 待定
MAX_TASK_BYTES          32 KB（256 B × 128）
接口带宽                与存储 256 B/T；与 Router 256 B/T
搬运拍数                ceil(size / 256) + 两端延迟（Cmem 13T、Mmem 50T 端到端、Router 10T）
Bank                    2（Bank0 > Bank1）
TS 直接启动 DTE 的入口  不建（第 8 章待定）
TASK_CFG_ADDR / TD / PACK / TRG  不作为软件接口；Commit 内部用 Descriptor 承载同样字段
MU / DTE 寄存器地址     regmap.h 临时映射   // 待定
Data + scale 拼接访存   按 Cmem MAS 的 256 B 建，scale 由独立请求搬
LUT 的 task_mode_table  与 TS 快速启动一起不建
```

***

## 7　机制覆盖

### Header Parser

| 机制 | 落点 | 用例 |
| - | - | - |
| 一帧一任务；首拍即 Header | H1 第 1 条 | `dte_hp_frame` |
| Header Accept 条件：Parser 空闲且 RD_CH0 / WR_CH0 TaskQueue 与 Completion RS 可用 | H1 第 2 条 | `dte_hp_accept` |
| 解析检查 | H1 第 1 条 | `dte_hp_check` |
| Payload 门控：Commit 成功前反压 | H1 第 2 条 | `dte_hp_gate` |
| byte_count = 0 的纯包头任务 | H1 第 3 条 | `dte_hp_zero_len` |
| 长度核对 | H1 第 4 条 | `dte_hp_length` |
| Drop Frame、Credit 断续、帧边界恢复 | H1 第 5 条 | `dte_hp_drop`、`dte_hp_credit` |
| MSG 包结构 | `common/message.h` | `msg_layout` |

### Commit 与软件寄存器序列

| 机制 | 落点 | 用例 |
| - | - | - |
| 双 Bank 优先级 | C0 第 1 条、C1 第 1 条 | `dte_commit_banks` |
| Route → Lane Pair | C1 第 2 条 | `dte_commit_route` |
| 配对接纳：三项同时可用才 Commit | C1 第 6 条 | `dte_commit_atomic` |
| 校验 Route、地址、长度；CM → MM 拒绝 | C1 第 2、3 条 | `dte_commit_validate` |
| 配对子上下文生成、task_id 分配、同拍原子写入两个 TaskQueue | C1 第 7 条 | `dte_commit_pair` |
| Router Header 接纳：两侧都 Commit 后 Payload 才进 inbound buffer | H1 第 2 条 | `dte_hp_gate` |
| 软件寄存器序列：`transfer_mode` 决定路径；写 `trigger` 提交；`trigger` 含 last | C0 | `dte_sw_regs` |
| 硬件地址计算：CM 侧加 stream 偏移、MM 侧直给；软件包头 stream 与 task 两级偏移 | C1 第 5 条 | `dte_addr_formula` |
| 搬运长度硬件算：scale 取 `data_len / 32`、topK 取 `router_ep_count × 6 B` | C1 第 5 条 | `dte_derived_len` |
| data_len 含义随方向变：router ↔ MM 含 topk + scale + data，router ↔ CM 只含 data | C1 第 5 条 | `dte_data_len` |
| `hw_header_op` | L2 第 5 条 | `dte_hw_header_op` |
| 单任务最大 32 KB；只支持连续一维 | C1 第 3 条 | `dte_max_len` |
| 出 core 前置申请 | C1 第 4 条 | `dte_pending_taskq`、`dte_reduce_credit` |
| 两入口不保序；任务身份唯一；任务期配置冻结 | C1 | `dte_two_entries` |
| R core 搬运原子化 | C1 第 6 条 | `rcore_atomic_move` |

### TaskQueue ×4 与 Lane ×4（含 AGCU）

| 机制 | 落点 | 用例 |
| - | - | - |
| 同拍配对入队、按序出队、两侧独立退出 | C1 第 7 条 + `tq` | `dte_tq` |
| 独立激活 | L1～L4 第 1 条 | `dte_lane_independent` |
| Read-ahead | L3 第 2 条 | `dte_read_ahead` |
| Lane 内按序；三类顺序 | `tq` + Buffer 按任务边界 | `dte_ordering` |
| 局部反压 | L1 第 2 条 | `dte_local_backpressure` |
| CH0 Header / Payload 分离；不发 DMA 读 | L1 第 4 条 | `dte_hp_frame` |
| CH1 credit / outstanding；Response Hold | L3 第 2、3 条 | `dte_ch1_outstanding` |
| 读侧完成分离 | L3 第 4 条 | `dte_completion_states` |
| Command / Data 配对；CH0 写完成 | L2 第 3、4 条 | 同上 |
| CH1 出口选择 | L4 第 1、4 条 | `dte_ch1_exit` |
| WR1 只允许写 Core Mem | L4 第 3 条 | `dte_wr1_mask` |
| CM 写仲裁 | L2 第 6 条 | `cmem_dte_two_writers` |
| Inner Flow 流水 | L3 + Buffer + L4 各自独立 | `dte_inner_flow` |
| AGCU | L2 第 2 条 | `dte_agcu` |
| DTE 侧 4 个 VC Buffer | L4 第 2 条 | `dte_vc_buffers` |
| 与存储接口 256 B/T；与 Router 256 B/T | 参数 | `dte_bandwidth` |
| 一次搬运拍数 | L2 / L4 的 Dx | `dte_transfer_cycles` |

### 中间 Buffer

| 机制 | 落点 | 用例 |
| - | - | - |
| Credit 背压 | L1 第 2 条、L3 第 2 条 | `dte_buffer_credit` |
| 按任务边界消费，不跨任务 | L2 第 2 条 | `dte_ordering` |
| 解耦配对 RD / WR | L3 第 2 条 | `dte_read_ahead` |
| 掩盖 32T 访存延迟 | `DTE_BUF_DEPTH` | `dte_transfer_cycles` |

### Completion RS 与 Done Pending

| 机制 | 落点 | 用例 |
| - | - | - |
| 四源事件同拍到达 | R1 第 1 条 | `dte_rs_four_sources` |
| 读发完与写消费分离 | Buffer 上报两种事件 | 同上 |
| Join 条件 | R1 第 2 条 | `dte_join` |
| 多命中保存；单路逐项输出；Exactly-once；payload 稳定 | R1 第 3 条 + P1 | `dte_done_exactly_once` |
| task_last 才上报 TS；`no_ack` 不上报 | P1 第 1 条 | `dsa_trigger_last` |
| 剖析口径 | P1 第 2 条 | `dte_profile` |

### Hmem、LUT、topK 与 shareMem 写

| 机制 | 落点 | 用例 |
| - | - | - |
| 进核存 / 丢 | L2 第 5 条 | `dte_header_store` |
| 出核改头 | L4 第 2 条 | `dte_header_rewrite` |
| 纯包头任务 | H1 第 3 条 | `dte_hp_zero_len` |
| 分开存储：软件 / 硬件包头独立，硬件包头分静态与动态 | `sw_hdr` / `hw_static` / `hw_dyn` | — |
| 发包时 MSG Header 由 DTE 写入；加载 weights 时由 DPU 写入 | L4 第 2 条 / GPU 桩 | `msg_header_writer` |
| topK 复制到独立 mem 供 MU 读；进核必带 topK，result 出核不带 | L1 第 3 条 + L2 第 5 条 | `dte_topk_copy` |
| 完成后写 shareMem 再通知 TS | L2 第 5 条 → R1 → P1 | `dte_sharemem_write` |

***

## 8　取舍

* **两个入口为什么在 Commit 汇成同一种 Descriptor**
  * 两者之后的路径完全一样：配对接纳、地址展开、Lane 激活、Join
  * 差别只在 Bank 优先级
* **Completion RS 为什么把四条 Lane 的事件同拍全收，而不是排队**
  * 为了“同拍多个 Join 命中全部写入 Done Pending”这条硬约束
  * 序列化只发生在 Done Pending 出队
