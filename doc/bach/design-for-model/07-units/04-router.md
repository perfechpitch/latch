# 4　Router

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../07-latch-建模计划.md)）的建模方式之上

给实现 Router 的人：八种独立打拍的模块各自的端口、存储器、流水线与逐级行为、参数与机制。每个 Core 一份，坏核也有。

八种模块：

* 数据面：RouterStation ×3、Xbar、CoreStation、ReduceModule
* 控制面：RouterTable / CSR、CoreMemCreditMonitor、Retire、CoreMem 重发

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《Router 片上交换与归约》全篇
* 《软件栈》：“阻塞重传四步”“Reduce 阶段”

***

## 1　定位与边界

Router 是 core 与 mesh 之间的交换点，三组对外连接：

* **三个 R2R 方向**（left / right / mid）各一条 256 B/T 的双向链路，接相邻 core 的 Router，或 Chip 边界的 C2C 链路
* **进 core 与出 core 各一条通路**，接本 core 的 DTE
* **控制信号**接 TS

数据面按 flit 走：

1. RouterStation 收包入 VC、查 RouterTable 判资源
2. Xbar 按输出方向仲裁
3. 出到相邻 Router、本 core（CoreStation）或 ReduceModule
4. ReduceModule 的结果再回 Xbar

控制面三件事：三种 credit 的 release 通道、TS 的注册与通知、RouterTable 的多副本提交。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1180 640" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs>
    <marker id="r0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker>
    <marker id="r0s" markerWidth="9" markerHeight="9" refX="1" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#475569"/></marker>
  </defs>
  <rect x="0" y="0" width="1180" height="640" fill="#ffffff"/>
  <text x="20" y="28" font-size="12" fill="#111827">Router · 第 0 层</text>

  <!-- 三个方向链路端口 -->
  <polygon points="470,50 570,50 560,86 460,86" fill="#f8fafc" stroke="#374151"/>
  <text x="515" y="72" font-size="10.5" fill="#374151" text-anchor="middle">link[mid]</text>
  <polygon points="30,300 130,300 120,336 20,336" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="322" font-size="10.5" fill="#374151" text-anchor="middle">link[left]</text>
  <polygon points="1050,300 1150,300 1140,336 1040,336" fill="#f8fafc" stroke="#374151"/>
  <text x="1095" y="322" font-size="10.5" fill="#374151" text-anchor="middle">link[right]</text>

  <!-- RouterStation ×3 -->
  <rect x="440" y="110" width="150" height="70" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="452" y="134" font-size="12" fill="#111827">RouterStation[mid]</text>
  <text x="452" y="152" font-size="10" fill="#475569">vc_buf ×4 · 查表 · 资源判定</text>
  <rect x="160" y="280" width="150" height="70" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="172" y="304" font-size="12" fill="#111827">RouterStation[left]</text>
  <text x="172" y="322" font-size="10" fill="#475569">vc_buf ×4 · 查表 · 资源判定</text>
  <rect x="860" y="280" width="150" height="70" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="872" y="304" font-size="12" fill="#111827">RouterStation[right]</text>
  <text x="872" y="322" font-size="10" fill="#475569">vc_buf ×4 · 查表 · 资源判定</text>
  <line x1="515" y1="88" x2="515" y2="108" stroke="#475569" marker-start="url(#r0s)" marker-end="url(#r0)"/>
  <line x1="132" y1="318" x2="158" y2="318" stroke="#475569" marker-start="url(#r0s)" marker-end="url(#r0)"/>
  <line x1="1012" y1="318" x2="1038" y2="318" stroke="#475569" marker-start="url(#r0s)" marker-end="url(#r0)"/>

  <!-- Xbar -->
  <rect x="440" y="250" width="200" height="130" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="452" y="274" font-size="12" fill="#111827">Xbar</text>
  <text x="452" y="294" font-size="10" fill="#475569">5 入 × 5 出，每出口独占仲裁</text>
  <text x="452" y="310" font-size="10" fill="#475569">多播全有全无 · 进 core / Reduce 锁定到尾 flit</text>
  <text x="452" y="326" font-size="10" fill="#475569">每出口每拍 1 flit</text>
  <text x="440" y="500" font-size="10" fill="#6b7280">2×5 Mesh 里一个 Router 只有三个 R2R 邻居：</text>
  <text x="440" y="516" font-size="10" fill="#6b7280">同行 left / right，加另一行对称位的 mid。</text>
  <text x="440" y="532" font-size="10" fill="#6b7280">边沿 Router 的 left 或 right 接 C2C Bridge</text>
  <line x1="515" y1="182" x2="515" y2="248" stroke="#475569" marker-start="url(#r0s)" marker-end="url(#r0)"/>
  <line x1="312" y1="318" x2="438" y2="318" stroke="#475569" marker-start="url(#r0s)" marker-end="url(#r0)"/>
  <line x1="642" y1="318" x2="858" y2="318" stroke="#475569" marker-start="url(#r0s)" marker-end="url(#r0)"/>
  <text x="375" y="312" font-size="9" fill="#6b7280" text-anchor="middle">xbar_req / xbar_grant</text>

  <!-- CoreStation -->
  <rect x="680" y="440" width="200" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="692" y="464" font-size="12" fill="#111827">CoreStation</text>
  <text x="692" y="482" font-size="10" fill="#475569">hdr_fifo · out_buf · 进 core 表</text>
  <text x="692" y="498" font-size="10" fill="#475569">出 core VC credit</text>
  <line x1="642" y1="360" x2="720" y2="438" stroke="#475569" marker-start="url(#r0s)" marker-end="url(#r0)"/>
  <polygon points="700,580 800,580 790,616 690,616" fill="#f8fafc" stroke="#374151"/>
  <text x="745" y="602" font-size="10.5" fill="#374151" text-anchor="middle">cs_hdr · cs_datain · cs_dataout</text>
  <line x1="745" y1="532" x2="745" y2="578" stroke="#475569" marker-start="url(#r0s)" marker-end="url(#r0)"/>
  <text x="760" y="560" font-size="9" fill="#6b7280">→ DTE DSA / DTE RV core</text>

  <!-- ReduceModule -->
  <rect x="680" y="130" width="200" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="692" y="154" font-size="12" fill="#111827">ReduceModule</text>
  <text x="692" y="172" font-size="10" fill="#475569">uct 16 · ctx SRAM 16×16 KiB</text>
  <text x="692" y="188" font-size="10" fill="#475569">RMW FP32 · reduce credit map · out_q</text>
  <line x1="642" y1="270" x2="720" y2="222" stroke="#475569" marker-start="url(#r0s)" marker-end="url(#r0)"/>

  <!-- RouterTable / CSR -->
  <rect x="160" y="110" width="200" height="80" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="164" y="114" width="192" height="72" fill="none" stroke="#374151"/>
  <text x="172" y="134" font-size="12" fill="#111827">RouterTable / CSR</text>
  <text x="172" y="152" font-size="10" fill="#475569">rt_copy ×4 · 64 项 · 多副本提交 FSM</text>
  <text x="172" y="168" font-size="10" fill="#475569">bypass_route · 只读 readback</text>
  <polygon points="30,130 110,130 102,166 22,166" fill="#f8fafc" stroke="#374151"/>
  <text x="66" y="152" font-size="10.5" fill="#374151" text-anchor="middle">cfg</text>
  <line x1="112" y1="150" x2="158" y2="150" stroke="#475569" stroke-dasharray="2 3" marker-end="url(#r0)"/>
  <line x1="362" y1="150" x2="438" y2="150" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#r0)"/>
  <text x="400" y="144" font-size="9" fill="#6b7280" text-anchor="middle">rt_lookup ×4</text>

  <!-- CreditMonitor / Retire -->
  <rect x="160" y="440" width="200" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="172" y="464" font-size="12" fill="#111827">CoreMemCreditMonitor</text>
  <text x="172" y="482" font-size="10" fill="#475569">events 16 全相连 · 每拍通知 1 笔</text>
  <text x="172" y="498" font-size="10" fill="#475569">CoreMem 重发登记</text>
  <rect x="160" y="560" width="200" height="60" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="172" y="584" font-size="12" fill="#111827">Retire</text>
  <text x="172" y="602" font-size="10" fill="#475569">广播到 3 Station + ReduceModule</text>
  <line x1="362" y1="480" x2="438" y2="480" stroke="#475569" stroke-dasharray="4 3" marker-start="url(#r0s)" marker-end="url(#r0)"/>
  <text x="400" y="474" font-size="9" fill="#6b7280" text-anchor="middle">credit_view</text>
  <line x1="362" y1="590" x2="438" y2="530" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#r0)"/>
  <text x="380" y="548" font-size="9" fill="#6b7280">retire_bcast</text>

  <!-- TS ports -->
  <polygon points="30,440 110,440 102,476 22,476" fill="#f8fafc" stroke="#374151"/>
  <text x="66" y="462" font-size="10.5" fill="#374151" text-anchor="middle">ts_credit</text>
  <line x1="112" y1="470" x2="158" y2="470" stroke="#475569" marker-start="url(#r0s)" marker-end="url(#r0)"/>
  <polygon points="30,560 110,560 102,596 22,596" fill="#f8fafc" stroke="#374151"/>
  <text x="66" y="582" font-size="10.5" fill="#374151" text-anchor="middle">ts_retire</text>
  <line x1="112" y1="590" x2="158" y2="590" stroke="#475569" marker-end="url(#r0)"/>
  <polygon points="910,470 1010,470 1000,506 900,506" fill="#f8fafc" stroke="#374151"/>
  <text x="955" y="492" font-size="10.5" fill="#374151" text-anchor="middle">ts_notify</text>
  <line x1="882" y1="485" x2="898" y2="485" stroke="#475569" marker-end="url(#r0)"/>
  <polygon points="910,150 1010,150 1000,186 900,186" fill="#f8fafc" stroke="#374151"/>
  <text x="955" y="172" font-size="10.5" fill="#374151" text-anchor="middle">ts_reduce_done</text>
  <line x1="882" y1="170" x2="898" y2="170" stroke="#475569" marker-end="url(#r0)"/>

  <text x="20" y="632" font-size="10.5" fill="#374151">实线 = flit 数据通路（256 B/拍）；虚线 = 控制信号。ts_notify 汇总 trigger、credit_pulse、complete 三组脉冲。</text>
</svg>
```

***

## 2　接口

信号名即模型里端口束的字段名。`link[d]` 三个方向同构；`d ∈ {left, right, mid}`。

```
port link[d].in (slave, credit/release, clk)      // 相邻 Router 或 C2C 链路 → 本 Router，每拍最多 1 flit
  in  flit_valid
  in  flit_vc[1:0]                                  // 目标 VC，进对应 vc_buf
  in  flit_head                                     // 1 = 携带 Header 的首 flit
  in  flit_tail
  in  flit_bytes[8:0]                               // 尾 flit 的有效字节数，其余 256
  in  flit_msg                                      // LogicPtr<Message>：包头 + payload 字节
  out vc_release_valid                              // 本方向某 VC 出队一 flit 后回一拍脉冲
  out vc_release_vc[1:0]
  in  stream_release_valid                          // 下游回收 Stream 表项
  in  stream_release_user[15:0]
  in  reduce_release_valid                          // 下游 ReduceModule 归还 Reduce credit
  in  reduce_release_user[15:0]
port link[d].out (master, credit/release, clk)    // 本 Router → 相邻 Router，镜像 link[d].in
  out flit_valid · flit_vc[1:0] · flit_head · flit_tail · flit_bytes[8:0] · flit_msg
  in  vc_release_valid · vc_release_vc[1:0]        // 下游 VC 出队，本地 vc_credit[d][vc] += 1
  out stream_release_valid · stream_release_user[15:0]
  out reduce_release_valid · reduce_release_user[15:0]

port cs_hdr (slave, valid/ready, clk)             // DTE RV core 经 io_reg 读包头、写 1 弹出
  in  rd_valid
  out rd_ready                                      // = hdr_fifo 非空
  out rd_data[63:0]                                 // 包头字段：path_id[5:0] user_id[15:0] size[15:0] op[1:0] vc[1:0] …
  in  pop_valid                                     // 写 1：hdr_fifo 出队一项
port cs_datain (master, AXI-Stream-like, clk)     // CoreStation → DTE Lane RD0，每拍 1 flit
  out valid
  in  ready
  out last · bytes[8:0] · msg
port cs_dataout (slave, AXI-Stream-like + credit, clk)  // DTE Lane WR1 → CoreStation
  in  valid
  out ready                                         // = out_vc_credit[vc] > 0 且 out_buf 未满
  in  vc[1:0] · last · bytes[8:0] · msg
  out vc_credit_release_valid                       // 该 VC 一 flit 离开 out_buf 后回一拍
  out vc_credit_release_vc[1:0]

port ts_notify (master, 脉冲, clk)                // Router → TS，三组脉冲各自独立
  out trigger_valid · trigger_user[15:0] · trigger_path[5:0] · trigger_reissue · trigger_p2p_reissue
  out credit_pulse_valid · credit_pulse_stream[3:0] // CreditMonitor：注册的事件满足
  out complete_valid · complete_user[15:0] · complete_path[5:0]  // 出 core 搬运（含重发）完成
port ts_reduce_done (master, 脉冲, clk)           // ReduceModule → TS
  out valid · user_id[15:0] · task_id[5:0]
port ts_credit (slave, valid/ready, clk)          // TS → CreditMonitor 的注册与 Stream credit 归还
  in  req_valid
  out req_ready                                     // = events 有空项
  in  req_stream[3:0] · req_user[15:0] · req_task[5:0] · req_path[5:0] · req_dir[3:0] · req_reissue
  in  return_valid                                  // stream_credit_return
  out return_accepted                               // 下拍
  in  return_user[15:0]
port ts_retire (slave, 脉冲, clk)                 // TS → Retire：User Retire 广播
  in  valid · user_id[15:0]
port cfg (slave, ctrl_noc 写事务, clk)            // RouterTable 写入 / Commit / Status / Bypass Route / Readback
  in  cfg_valid · cfg_addr[15:0] · cfg_we · cfg_wdata[31:0]
  out cfg_rdata[31:0]                               // 下一拍
```

***

## 3　存储器

```
mem rt_copy[4]         FF 阵列     64 × {cur_vc[1:0], dir_mask[3:0], nxt_vc[4][1:0], stream_need[3:0], op[1:0], stall_way, reduce_prec[1:0]}  4R1W  Commit 期间逐副本写  复位全 0  // 3 RouterStation + CoreStation 各一份；dir_mask 位序 left/right/mid/Core
mem rt_shadow          1-deep 寄存器 {path_id[5:0], 表项}          1W1R   cfg 写入，Commit 取走           复位空     // 待提交的表项
mem rt_commit_fsm      FF          {state[1:0], copy_idx[2:0]}    1RW    IDLE → WRITE(i) → DONE          复位 IDLE
mem bypass_route[8]    FF 阵列     8 × dir_mask[3:0]               1R1W   cfg 写，无在途 release 才可改  复位 0     // 每个 credit 输入端口一个静态 Mask
mem vc_buf[3][4]       FIFO        深 VC_BUF_DEPTH × flit          1W1R   满 → 不再发 vc_release         复位空     // 每方向每 VC
mem pkt_ctx[3][4]      1-deep 寄存器 {vc[1:0], dir_mask[3:0], remain[8:0], in_pkt, locked}  1RW  头 flit 建，尾 flit 清  复位空  // 每 VC 队首包上下文
mem stream_tbl[3]      FF 阵列     STREAM_SLOTS_PER_DIR × {valid, user_id[15:0], dir_mask[3:0]}  1R1W  Acquire 写、release 清  复位空  // 每输入方向的下游 Stream 授权
mem ds_stream_map[3][4] FF 阵列    16 × valid                      1R1W   与 stream_tbl 同步            复位 0     // 输入方向 × 下游方向 × UserID 是否持有资源
mem vc_credit[3][4][4] FF          计数 [0, VC_BUF_DEPTH]           1RW    发 −1，release +1              复位 VC_BUF_DEPTH  // 输入方向 × 下游方向 × VC
mem order_flag[3][4]   FF          1 b                             1RW    重发 Packet 落 CoreMem 置、重发完成清  复位 0  // 同 VC 保序
mem rs_out[3]          级间 latch   {valid, vc[1:0], dir_mask[3:0], flit}   —  每拍覆写            —          // RouterStation → Xbar 请求
mem xbar_grant[5]      级间 latch   {valid, src[2:0], flit}          —      每拍覆写                        —          // Xbar → 五个出口 left/right/mid/Core/Reduce
mem xbar_lock[2]       FF          {locked, src[2:0]}               1RW    进 core / Reduce 头 flit 置、尾 flit 清  复位 0
mem hdr_fifo           FIFO        深 HDR_FIFO_DEPTH × header       1W1R   满 → CoreStation 不准入        复位空
mem out_buf            FIFO        深 OUT_BUF_DEPTH × flit          1W1R   满 → Xbar Core 出口不授予       复位空
mem cs_stream_tbl      FF 阵列     16 × {valid, user_id[15:0]}      1R1W   与 TS stream_table 同逻辑       复位空     // 进 core 表
mem out_vc_credit[4]   FF          计数                             1RW    DTE 发 −1，出 out_buf +1        复位 OUT_BUF_DEPTH/4
mem reissue_tbl        FF 阵列     16 × {valid, user_id[15:0], path_id[5:0], size[15:0], cm_addr[19:0]}  1R1W  Absorb 写，重发完成清  复位空
mem rm_uct             FF 阵列     16 × {valid, user_id[15:0], in_done[2:0], out_state[1:0], retire}  1R1W  首份输入建   复位空  // in_done 对应三路并发输入
mem rm_ctx             SRAM        16 × 16 KiB（FP32）              1RW    RMW 原位                        复位未定义 // Reduce Context
mem rm_rt              FF 阵列     64 × {dir_mask[3:0], nxt_vc[1:0], op[1:0], out_prec}  1R1W  软件经 cfg 写  复位 0  // ReduceModule 的 RouterTable 副本
mem rm_credit_map      FF 阵列     16 × 3 × 计数                    1RW    发 −1，reduce_release +1        复位 REDUCE_ENTRY_CREDIT  // 用户 × 三个 R2R 下游方向
mem rm_in_latch        级间 latch   {valid, src[1:0], flit}          —      每拍覆写                        —          // 输入仲裁 → RMW
mem rm_rmw_latch       级间 latch   {valid, user[15:0], off[13:0], sum[2047:0]}  —  每拍覆写            —          // RMW 两级
mem rm_out_q           FIFO        深 REDUCE_OUT_DEPTH × flit       1W1R   满 → 输出前置检查不通过        复位空
mem cm_events          FF 阵列     16 × {valid, user[15:0], stream[3:0], task[5:0], path[5:0], dir[3:0], vc_need, stream_need, reissue}  全相连  注册写、通知清  复位空
mem retire_pending     FF 阵列     16 × {user_id[15:0], rm_wait}    1R1W   ts_retire 写，credit 归位清     复位空
```

***

## 4　流水线总览

一条 flit 从 `link[d].in` 到 `link[d'].out` 走四级：

* R1 收包入 VC → R2 包头解析与资源判定 → R3 Xbar 仲裁 → R4 出口

三条分支：

* **进 core**：R3 之后走 C1 准入 → `hdr_fifo` / `out_buf` → `cs_datain`
* **进 Reduce**：M1 输入仲裁 → M2 RMW → M3 输出前置检查 → 回 R3
* **出 core**：从 `cs_dataout` 进 C2 → R3

控制面的 RouterTable 提交、CreditMonitor、Retire 不在这条流水线上，在“逐级行为”里单列。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1180 520" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="f0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="1180" height="520" fill="#ffffff"/>
  <text x="20" y="26" font-size="12" fill="#111827">Router · 第 1 层（bypass 路径按比例 4 拍；进 core 与 Reduce 分支标非按比例）</text>
  <g stroke="#e5e7eb"><line x1="140" y1="40" x2="140" y2="500"/><line x1="216" y1="40" x2="216" y2="500"/><line x1="292" y1="40" x2="292" y2="500"/><line x1="368" y1="40" x2="368" y2="500"/><line x1="444" y1="40" x2="444" y2="500"/><line x1="520" y1="40" x2="520" y2="500"/></g>
  <g font-size="8.5" fill="#6b7280"><text x="140" y="50">t0</text><text x="216" y="50">t1</text><text x="292" y="50">t2</text><text x="368" y="50">t3</text><text x="444" y="50">t4</text><text x="520" y="50">t5</text></g>

  <polygon points="24,90 122,90 114,122 16,122" fill="#f8fafc" stroke="#374151"/>
  <text x="69" y="110" font-size="10" fill="#374151" text-anchor="middle">link[d].in</text>
  <line x1="122" y1="106" x2="138" y2="106" stroke="#475569" marker-end="url(#f0)"/>

  <rect x="144" y="70" width="68" height="72" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="148" y="66" font-size="8.5" fill="#6b7280">R1</text><text x="210" y="66" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="150" y="92" font-size="11" fill="#111827">收包</text>
  <text x="150" y="108" font-size="9.5" fill="#475569">入 vc_buf</text>
  <rect x="144" y="160" width="68" height="60" fill="#f8fafc" stroke="#374151"/>
  <rect x="148" y="164" width="60" height="52" fill="none" stroke="#374151"/>
  <g stroke="#374151"><line x1="160" y1="164" x2="160" y2="216"/><line x1="172" y1="164" x2="172" y2="216"/></g>
  <text x="178" y="186" font-size="9" fill="#111827">vc_buf</text>
  <text x="178" y="198" font-size="8.5" fill="#475569">4×32 flit</text>
  <text x="178" y="210" font-size="8.5" fill="#475569">1W1R</text>
  <line x1="178" y1="142" x2="178" y2="158" stroke="#475569" marker-end="url(#f0)"/>

  <rect x="220" y="70" width="68" height="72" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="224" y="66" font-size="8.5" fill="#6b7280">R2</text><text x="286" y="66" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="226" y="92" font-size="11" fill="#111827">解析判定</text>
  <text x="226" y="108" font-size="9.5" fill="#475569">rt_lookup</text>
  <text x="226" y="122" font-size="9.5" fill="#475569">Stream/VC/Rdc</text>
  <line x1="212" y1="106" x2="218" y2="106" stroke="#475569" marker-end="url(#f0)"/>
  <rect x="220" y="160" width="68" height="44" fill="#f8fafc" stroke="#374151"/>
  <rect x="224" y="164" width="60" height="36" fill="none" stroke="#374151"/>
  <text x="228" y="180" font-size="9" fill="#111827">rt_copy[d]</text>
  <text x="228" y="192" font-size="8.5" fill="#475569">64 项 · 1R</text>
  <line x1="254" y1="158" x2="254" y2="144" stroke="#475569" stroke-dasharray="3 2" marker-end="url(#f0)"/>

  <rect x="296" y="70" width="68" height="72" fill="#f1f5f9" stroke="#334155"/>
  <rect x="296" y="70" width="68" height="14" fill="#334155"/>
  <text x="330" y="81" font-size="9.5" fill="#ffffff" text-anchor="middle">rs_out[d]</text>
  <text x="300" y="100" font-size="9" fill="#475569">vc · dir_mask</text>
  <text x="300" y="112" font-size="9" fill="#475569">flit</text>
  <line x1="288" y1="106" x2="294" y2="106" stroke="#475569" marker-end="url(#f0)"/>

  <rect x="372" y="70" width="68" height="72" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="376" y="66" font-size="8.5" fill="#6b7280">R3</text><text x="438" y="66" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="378" y="92" font-size="11" fill="#111827">Xbar 仲裁</text>
  <text x="378" y="108" font-size="9.5" fill="#475569">每出口独占</text>
  <text x="378" y="122" font-size="9.5" fill="#475569">多播全有全无</text>
  <line x1="364" y1="106" x2="370" y2="106" stroke="#475569" marker-end="url(#f0)"/>

  <rect x="448" y="70" width="68" height="72" fill="#f1f5f9" stroke="#334155"/>
  <rect x="448" y="70" width="68" height="14" fill="#334155"/>
  <text x="482" y="81" font-size="9.5" fill="#ffffff" text-anchor="middle">xbar_grant[o]</text>
  <text x="452" y="100" font-size="9" fill="#475569">src · flit</text>
  <line x1="440" y1="106" x2="446" y2="106" stroke="#475569" marker-end="url(#f0)"/>

  <rect x="524" y="70" width="68" height="72" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="528" y="66" font-size="8.5" fill="#6b7280">R4</text><text x="590" y="66" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="530" y="92" font-size="11" fill="#111827">出口</text>
  <text x="530" y="108" font-size="9.5" fill="#475569">扣 vc_credit</text>
  <text x="530" y="122" font-size="9.5" fill="#475569">发 release</text>
  <line x1="516" y1="106" x2="522" y2="106" stroke="#475569" marker-end="url(#f0)"/>
  <polygon points="612,90 710,90 702,122 604,122" fill="#f8fafc" stroke="#374151"/>
  <text x="657" y="110" font-size="10" fill="#374151" text-anchor="middle">link[d'].out</text>
  <line x1="592" y1="106" x2="602" y2="106" stroke="#475569" marker-end="url(#f0)"/>

  <!-- 进 core 分支（非按比例） -->
  <rect x="448" y="250" width="120" height="72" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="452" y="246" font-size="8.5" fill="#6b7280">C1</text><text x="566" y="246" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="456" y="272" font-size="11" fill="#7c2d12">CoreStation 准入</text>
  <text x="456" y="288" font-size="9.5" fill="#92400e">三态：已分配 / 占用 / 拒</text>
  <text x="456" y="302" font-size="9.5" fill="#92400e">hdr_fifo · out_buf</text>
  <polyline points="482,142 482,180 508,180 508,248" fill="none" stroke="#475569" marker-end="url(#f0)"/>
  <text x="512" y="200" font-size="9" fill="#6b7280">Core 出口</text>
  <polygon points="612,270 710,270 702,302 604,302" fill="#f8fafc" stroke="#374151"/>
  <text x="657" y="290" font-size="10" fill="#374151" text-anchor="middle">cs_datain</text>
  <line x1="570" y1="286" x2="602" y2="286" stroke="#475569" marker-end="url(#f0)"/>
  <polygon points="24,270 122,270 114,302 16,302" fill="#f8fafc" stroke="#374151"/>
  <text x="69" y="290" font-size="10" fill="#374151" text-anchor="middle">cs_dataout</text>
  <rect x="144" y="250" width="120" height="72" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="148" y="246" font-size="8.5" fill="#6b7280">C2</text><text x="262" y="246" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="152" y="272" font-size="11" fill="#111827">出 core 收包</text>
  <text x="152" y="288" font-size="9.5" fill="#475569">查 out_vc_credit</text>
  <text x="152" y="302" font-size="9.5" fill="#475569">查表 → rs_out[C]</text>
  <line x1="122" y1="286" x2="142" y2="286" stroke="#475569" marker-end="url(#f0)"/>
  <polyline points="264,286 330,286 330,144" fill="none" stroke="#475569" marker-end="url(#f0)"/>

  <!-- Reduce 分支 -->
  <rect x="448" y="380" width="120" height="72" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="452" y="376" font-size="8.5" fill="#6b7280">M1</text><text x="566" y="376" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="456" y="402" font-size="11" fill="#7c2d12">Reduce 输入仲裁</text>
  <text x="456" y="418" font-size="9.5" fill="#92400e">三路争 bank</text>
  <polyline points="500,142 500,190 540,190 540,378" fill="none" stroke="#475569" marker-end="url(#f0)"/>
  <text x="544" y="330" font-size="9" fill="#6b7280">Reduce 出口</text>
  <rect x="600" y="380" width="120" height="72" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="604" y="376" font-size="8.5" fill="#6b7280">M2</text><text x="718" y="376" font-size="8.5" fill="#6b7280" text-anchor="end">D2</text>
  <text x="608" y="402" font-size="11" fill="#7c2d12">RMW 累加</text>
  <text x="608" y="418" font-size="9.5" fill="#92400e">rm_ctx 读 · 加 · 写</text>
  <text x="608" y="432" font-size="9.5" fill="#92400e">FP32，256 B/拍</text>
  <line x1="570" y1="416" x2="598" y2="416" stroke="#475569" marker-end="url(#f0)"/>
  <rect x="752" y="380" width="130" height="72" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="756" y="376" font-size="8.5" fill="#6b7280">M3</text><text x="880" y="376" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="760" y="402" font-size="11" fill="#7c2d12">输出前置检查</text>
  <text x="760" y="418" font-size="9.5" fill="#92400e">全方向输入完成</text>
  <text x="760" y="432" font-size="9.5" fill="#92400e">VC credit · Reduce credit</text>
  <line x1="722" y1="416" x2="750" y2="416" stroke="#475569" marker-end="url(#f0)"/>
  <polyline points="882,416 940,416 940,60 404,60 404,68" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#f0)"/>
  <text x="950" y="240" font-size="9" fill="#6b7280">rm_out_q → 回 Xbar（第五路输入）</text>

  <text x="20" y="490" font-size="10" fill="#374151">R2 只对 VC 队首的头 flit 判定；同一 VC 后续 flit 沿用 pkt_ctx。R3 授予后 R4 扣 credit；进 core 与 Reduce 出口自头 flit 起锁定到尾 flit。</text>
</svg>
```

R1～R4 每级 1 拍是本文定的（待定）：MAS 只给 R2R 40T 端到端，Router 内部级数未给。

***

## 5　逐级行为

每级四要素：入口存储、逻辑（有序编号的表达式）、出口存储、Dx。

### R1 · 收包入 VC（RouterStation[d]）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `link[d].in` 的 flit_* | 1. `flit_valid` → `vc_buf[d][flit_vc].push(flit)`<br>2. `flit_head` → 记 `pkt_ctx[d][vc].in_pkt = 0`（待 R2 解析）<br>3. `vc_buf` 满时不发生：上游按 `vc_credit` 不会发 | `vc_buf[d][4]` | D1 |

### R2 · 包头解析与资源判定（RouterStation[d]）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `vc_buf[d][v]` 队首、`pkt_ctx[d][v]`、`rt_copy[d]`、`stream_tbl[d]`、`ds_stream_map[d]`、`vc_credit[d]`、`order_flag[d][v]`、CreditMonitor 的 `credit_view` | 1. 对每个 VC v：队首为头 flit 且 `!pkt_ctx.in_pkt` → `e = rt_copy[d][hdr.path_id]`；`pkt_ctx = {vc, e.dir_mask, hdr.size/256, in_pkt=1}`<br>2. `need_ok = ∀ dir ∈ e.dir_mask: vc_credit[d][dir][e.nxt_vc[dir]] > 0 ∧ (e.stream_need[dir] → ds_stream_map[d][dir][hdr.user])`<br>3. `e.op == Reduce` 时 `need_ok ∧= reduce_credit_ok(hdr.user)`（ReduceModule 的 `rm_credit_map` 视图）<br>4. `!need_ok ∧ e.stall_way == ToCoreMem` → 改 `dir_mask = Core`，`reissue = 1`（CoreMem 重发：Bypass 映射成进 core + 出 core）<br>5. `order_flag[d][v] ∧ !reissue` → 不发（同 VC 保序）<br>6. 选一个 `need_ok` 的 VC（轮询，待定）：`rs_out[d] = {valid, v, dir_mask, flit}`；数据 flit 沿用 `pkt_ctx` | `rs_out[d]`、`pkt_ctx[d][v]` | D1 |

### R3 · Xbar 仲裁（Xbar）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `rs_out[left/right/mid]`、`rs_out[C]`（CoreStation 出 core）、`rm_out_q` 队首、`xbar_lock[2]`、`out_buf` 余量、ReduceModule 的 `rm_in_ready` | 1. 对每个出口 o ∈ {left, right, mid, Core, Reduce}：`cand[o] = { s : rs_out[s].valid ∧ o ∈ rs_out[s].dir_mask }`<br>2. `o ∈ {Core, Reduce} ∧ xbar_lock[o].locked` → `cand[o] = {xbar_lock[o].src}`<br>3. `win[o] = 轮询(cand[o])`（待定）<br>4. 多播：源 s 的全部目标出口都选中 s 才授予，否则本拍全部不授予（全有全无）<br>5. 授予：`xbar_grant[o] = {src, flit}`；头 flit 进 Core / Reduce → `xbar_lock[o] = {1, s}`，尾 flit 清；源侧 `vc_buf[s][v].pop()`，发 `vc_release`（对 link 入口） | `xbar_grant[5]`、`xbar_lock[2]` | D1 |

### R4 · 出口（RouterStation[d'] 的发送侧）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `xbar_grant[d']`、`vc_credit[d']`、`stream_tbl` | 1. `link[d'].out.flit_* = grant.flit`，`flit_vc = e.nxt_vc[d']`<br>2. `vc_credit[src][d'][nxt_vc] -= 1`<br>3. 尾 flit 且该 user 在 `stream_tbl` 无后续 → 不动（Stream 表项由下游 release 回收）<br>4. 收到 `link[d'].out.vc_release` → `vc_credit[*][d'][vc] += 1`；收到 `stream_release` / `reduce_release` → 按 `bypass_route[port]` 复制到各方向的 `link.out`，UserID 与类型不变 | `link[d'].out` | D1 |

### C1 · CoreStation 准入（CoreStation）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `xbar_grant[Core]`、`cs_stream_tbl`、`hdr_fifo`、`out_buf` | 1. 头 flit：`slot = cs_stream_tbl.find(user)`；已分配 → 准入；未分配且有空项 → 占用并准入；无空项 → 拒（Xbar 出口不授予，该 VC 仍可收数）<br>2. 准入的头 flit：`hdr_fifo.push(hdr)`；全部 flit `out_buf.push(flit)`<br>3. `ts_notify.trigger = {user, path, reissue, p2p_reissue}` 按 `hdr_fifo` 顺序，每拍一笔<br>4. `cs_datain.valid = out_buf 非空`；`ready` 为上拍值 → `out_buf.pop()`<br>5. 尾 flit 且累计字节 = 包头 payload 大小 → `ts_notify.complete = {user, path}` | `hdr_fifo`、`out_buf`、`cs_datain`、`ts_notify` | D变长（hdr_fifo / out_buf 排队） |

### C2 · 出 core 收包（CoreStation）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `cs_dataout`、`out_vc_credit[4]`、`rt_copy[C]` | 1. `cs_dataout.ready = out_vc_credit[vc] > 0`（上拍值）<br>2. 收下的头 flit 查 `rt_copy[C]` 得 `dir_mask`；下游 Stream / Rmem 资源由 DTE 在发前申请（本级不查）<br>3. `rs_out[C] = {valid, vc, dir_mask, flit}`；`out_vc_credit[vc] -= 1`；flit 被 Xbar 授予后 `vc_credit_release` 回一拍 | `rs_out[C]`、`cs_dataout` | D1 |

### M1 · Reduce 输入仲裁（ReduceModule）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `xbar_grant[Reduce]`（最多三路：三个方向的 Reduce 输入）、`rm_uct`、`rm_ctx` bank 忙 | 1. 头 flit：`rm_uct.find(user)` 无 → 分配空项，`in_done = 0`；有且 `out_state != Idle` → 不接（上下文保护，反压）<br>2. 三路争同一 bank 时轮询（待定）；`rm_in_ready = 有空 bank ∧ 上下文可接`<br>3. `rm_in_latch = {src, flit, first = 首份输入}` | `rm_in_latch` | D1 |

### M2 · RMW 累加（ReduceModule）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `rm_in_latch`、`rm_ctx` | 1. `off = flit_idx × 256`；跳过软件辅助信息 16 B<br>2. `first` → `rm_ctx[user][off] = to_fp32(flit)`；否则 `rm_ctx[user][off] = rm_ctx[user][off] + to_fp32(flit)`（按到达顺序 FP32 累加，与参考实现同序）<br>3. 尾 flit → `rm_uct[user].in_done[src] = 1`<br>4. 同地址冲突串行化：每拍 256 B、1 个加法端口 | `rm_rmw_latch`、`rm_ctx`、`rm_uct` | D2 |

### M3 · 输出前置检查（ReduceModule）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `rm_uct`、`rm_ctx`、`rm_rt`、`rm_credit_map`、Xbar 的 `vc_credit` 视图、`rm_out_q` | 1. `rm_uct[user].in_done == 全部输入方向` → `out_state = Emit`<br>2. 每拍一 flit：`vc_credit[Reduce][dir][rm_rt.nxt_vc] > 0 ∧ rm_credit_map[user][dir] > 0` → `rm_out_q.push(from_fp32(rm_ctx[user][off], rm_rt.out_prec))`，`rm_credit_map -= 1`<br>3. 最后一 flit 出队后 `ts_reduce_done = {user, task}`；`out_state = Idle`<br>4. 输出 flit 被下游接受 → `reduce_release(user)` 经 `bypass_route` 回上游<br>5. `retire_pending[user].rm_wait ∧ rm_credit_map[user][*] == 初值` → 删 `rm_uct[user]` | `rm_out_q`、`ts_reduce_done`、`link.out.reduce_release` | D变长 |

### T1 · RouterTable 提交（RouterTable / CSR）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `cfg`、`rt_shadow`、`rt_commit_fsm` | 1. `cfg_addr` 为 Write Data → `rt_shadow = {path_id, 表项}`<br>2. `cfg_addr` 为 Commit ∧ `fsm == IDLE` → `fsm = WRITE(0)`；否则拒绝重叠提交（Status 报错）<br>3. `WRITE(i)`：`rt_copy[i][path_id] = rt_shadow`，每副本 `RT_COPY_WRITE_CYCLES` 拍；`i == 3` → `DONE`<br>4. `DONE`：Status 置完成，一拍后 `IDLE`；提交期间数据面仍读旧表项（副本写入是整项覆写，同一拍读到的是旧值或新值之一，不会读到半新半旧）<br>5. `cfg_addr` 为 Bypass Route → 该端口无在途 release 时才写 `bypass_route`，否则报错 | `rt_copy[4]`、`cfg_rdata` | D变长（4 副本 × 写入拍数） |

### K1 · 监听与通知（CoreMemCreditMonitor）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `ts_credit.req_*`、`cm_events`、各 Station 的 `vc_credit` / `stream_tbl` 视图 | 1. `req_valid ∧ req_ready` → `cm_events.alloc({stream, user, task, path, dir, vc_need, stream_need, reissue})`<br>2. 每拍对全部 16 项判 `satisfied = ∀ dir: vc_credit 与 Stream 满足`<br>3. 多项满足 → 选 `stream` 最老的一项：`ts_notify.credit_pulse = {stream}`，清该项<br>4. `return_valid` → 下拍 `return_accepted = 1`，`stream_tbl` 对应项清 | `ts_notify.credit_pulse`、`cm_events`、`ts_credit.return_accepted` | D1（注册）/ D变长（等待） |

### E1 · Retire 广播（Retire）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `ts_retire`、`retire_pending` | 1. `valid` → 三个 `stream_tbl[d]` 与 `cs_stream_tbl` 删该 user 的全部表项、停止其新发送<br>2. `retire_pending[user] = {rm_wait = 1}`（ReduceModule 在 M3 第 5 条延迟回收）<br>3. 向三个方向 `link.out.stream_release(user)`（坏核也透传） | `stream_tbl[3]`、`cs_stream_tbl`、`retire_pending`、`link.out` | D1 |

### X1 · CoreMem 重发登记（CoreMem 重发）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| R2 第 4 条的 `reissue` 标志、C1 的准入、`reissue_tbl` | 1. 带 `reissue` 的包进 core：`reissue_tbl.alloc({user, path, size, cm_addr = 软件预留区})`，`order_flag[d][v] = 1`<br>2. `ts_notify.trigger.reissue = 1`；TS 安排 DTE 的重发 task，重发走 `cs_dataout` 正常出 core，重发时按 `path_id` 重查表<br>3. 重发的尾 flit 出 Xbar → `reissue_tbl` 清项，`order_flag` 清，`ts_notify.complete = {user, path}` | `reissue_tbl`、`order_flag`、`ts_notify` | D1 |

***

## 6　参数汇总

```
VC_BUF_DEPTH            32 flit（8 KB）   // 待定
STREAM_SLOTS_PER_DIR    16                // 待定
VC_CREDIT_INIT          VC_BUF_DEPTH      // 待定
RT_ENTRIES              64                // 待定
RT_COPIES               5（4 Station + CoreStation）// 待定
RT_COPY_WRITE_CYCLES    1                 // 待定
XBAR_ARB                轮询              // 待定，原文未给
HDR_FIFO_DEPTH          16                // 待定
OUT_BUF_DEPTH           32 flit           // 待定
REDUCE_ENTRY_CREDIT     64 flit（16 KiB / 256 B）// 待定
REDUCE_BANKS            4                 // 待定
REDUCE_RMW_CYCLES       2                 // 待定
REDUCE_OUT_DEPTH        8                 // 待定
REDUCE_IN_ARB           轮询              // 待定
CM_EVENTS               16（全相连）
ROUTER_PIPE_CYCLES      4（R1～R4 各 1）   // 待定，MAS 只给 R2R 40T
FLIT_BYTES              256；尾 flit 带有效字节数（不建输出移位拼接）
每方向每拍 1 flit；进 core 与出 core 并行；Xbar 每出口每拍 1 flit
operation 的 Reduce0 / Reduce1 / Reduce2 = 源分量 / 中继累加 / 最终汇聚   // 待定，第 8 章
```

***

## 7　机制覆盖

### RouterTable 与 CSR

| 机制 | 落点 | 用例 |
| - | - | - |
| 路由表查询、统一查询语义（转发与重发都用 PathID 查表） | R2 第 1 条、X1 第 2 条 | `rt_lookup` |
| 静态表，不存动态状态 | `rt_copy` 字段 | — |
| 多副本原子提交：更新期间数据面仍用旧表，全部副本写完才报告完成 | T1 | `rt_commit_atomic` |
| 提交互斥、外部副本软件同步 | T1 第 2 条；DTE 与 ReduceModule 副本由 SCP 桩另写 | `rt_commit_reject`、`router_table_three_copies` |
| Credit 路由修改约束（无在途 release 才可改） | T1 第 5 条 | `rt_bypass_route_change` |
| 业务 Credit Bypass：release 按静态 Mask 单播 / 多播，UserID 与类型不变 | R4 第 4 条 | `credit_bypass_multicast` |

### RouterStation（×3）

| 机制 | 落点 | 用例 |
| - | - | - |
| 包头检测与解析（PathID、UserID、包长、operation、方向 mask、VC） | R2 第 1 条 | `rs_parse` |
| VC 分配：flit 携带 VC 号入对应 Buffer；`curVC` 用于入口 | R1 | `rs_vc_assign` |
| 资源与阻塞解析：按表项解析各方向 Stream / VC / Reduce 需求 | R2 第 2、3 条 | `rs_resolve` |
| 全方向满足才发 | R2 第 2 条 | `rs_all_dirs` |
| VC credit 扣与还：发前查、发后扣，下游 flit 离开 VC 经 release 通道还 | R2 第 2 条、R4 第 2、4 条 | `rs_vc_credit` |
| 独立信用：每下游方向每 VC 独立计数 | `vc_credit[3][4][4]` | `rs_vc_independent` |
| Stream 唯一状态、Stream 授权（UserID + PathID + 方向）、禁止超额 / 重复 | `stream_tbl` 的 Acquire | `rs_stream_grant` |
| Stream 释放：携 UserID 的 release 回收 | K1 第 4 条 / `link.in.stream_release` | `rs_stream_release` |
| 下游映射表与 DTE 的 User Resource Cache Table 行为一致 | `ds_stream_map` 与 DTE 共用 `StreamLedger` 类型 | `rs_dte_ledger_consistent` |
| 多播原子准入：每 flit 全部目标同时取得资源才发 | R2 第 2 条 + R3 第 4 条 | `rs_multicast_atomic` |
| Router 间交织：flit 边界可切换 Packet | `pkt_ctx` 每 VC 一份 | `rs_interleave` |
| 整包粒度仲裁（只对包头仲裁）与模块入口锁定 | R3 第 2、5 条 | `xbar_lock` |
| 反压稳定 | credit 语义 | — |
| 同 VC 保序：有未完成重发 Packet 时后续不得越过 | R2 第 5 条 | `rs_vc_order` |
| 业务 Credit Bypass 与多播 | R4 第 4 条 | `credit_bypass_multicast` |
| 输出移位拼接 | 不建：flit 定长 256 B，尾 flit 带有效字节数 | — |

### Xbar

| 机制 | 落点 | 用例 |
| - | - | - |
| 按输出独立仲裁，无关输出不串行化 | R3 第 1、3 条 | `xbar_per_output` |
| 五输入并行 | R3 逐出口 | `xbar_parallel` |
| 多播复制、全有全无握手 | R3 第 4 条 | `xbar_multicast_all_or_nothing` |
| Header / Payload 一致转发 | 单一 flit 序列 | — |
| Reduce 路径：operation 为 Reduce 导向 ReduceModule，结果回注输出 | R2 第 1 条的 `dir_mask = Reduce`；`rm_out_q` 为第五路输入 | `xbar_reduce_path` |
| 入口锁定与释放：进 Core / ReduceModule 后锁定到尾 flit，只约束该组合 | R3 第 2、5 条 | `xbar_lock` |
| 握手后统一更新 credit 与上下文 | R3 第 5 条、R4 第 2 条 | — |
| 仲裁算法 | 轮询（待定） | `xbar_round_robin` |

### CoreStation

| 机制 | 落点 | 用例 |
| - | - | - |
| Core 准入三态 | C1 第 1 条 | `cs_admit_three_states` |
| Core 容量保证：过 Stream 检查后不查 Core 方向 VC credit | C1 不查 credit | `cs_no_vc_check` |
| 两表一致申请：进 core 表与 TS stream_table 同逻辑，通知必可接收 | `cs_stream_tbl` 与 TS `StreamTable` 共用分配算法 | `cs_ts_consistent` |
| 按包头顺序通知 TS | C1 第 3 条 | `cs_notify_order` |
| 包头弹出：DTE core 读完写 1 | `cs_hdr.pop_valid` | `cs_pop_header` |
| 搬运接口契约：反压不丢不重不跨包 | `cs_datain` / `cs_dataout` 的 AXI-Stream-like 语义 | `cs_backpressure` |
| 整包进 Core：不交织 | R3 锁定 | `xbar_lock` |
| 出 Core 按 VC 流控 | C2 第 1、3 条 | `cs_out_vc` |
| 出 Core 前置申请：下游 Stream / Rmem 资源先申请到才发，否则在 DTE 的 PendingTaskQ 等待 | DTE 文档 Commit 一节 | `dte_pending_taskq` |
| DTE 侧 VC Buffer：4 个，单 VC 阻塞不影响其他 | DTE 文档 Lane 一节 | `dte_vc_buffers` |
| 双向完全并行 | C1 与 C2 两条独立通路 | `cs_full_duplex` |
| token 收完判定：已接收字节数 = 包头 payload 大小 | C1 第 5 条 | `token_complete` |

### CoreMem 重发

| 机制 | 落点 | 用例 |
| - | - | - |
| 阻塞缓存选择：按 stallWay 留 VC 或转 Core Mem | R2 第 4 条 | `reissue_stallway` |
| Bypass 映射成进 core + 出 core | R2 第 4 条 + X1 | `reissue_absorb` |
| Packet 保存与重查：只存 Packet，重发时重查表 | X1 第 2 条 | `reissue_relookup` |
| 同 VC 保序 | R2 第 5 条 | `rs_vc_order` |
| 完成通知：直接或重发完成后向 TS 返 UserID + PathID | C1 第 5 条、X1 第 3 条 | `reissue_complete_notify` |
| 重发注册监听 | K1 第 1 条带 `reissue` | `cm_reissue_register` |
| 软件须预留 Core Mem 空间并在任务链安排 reissue 任务 | 编译侧产物校验 | `reissue_reserved` |

### ReduceModule

| 机制 | 落点 | 用例 |
| - | - | - |
| 上下文分配：同 User 同 Packet 首份输入分配并写入 FP32 | M1 第 1 条、M2 第 2 条 | `rm_alloc` |
| RMW 原位累加；上下文保护（未输出前不覆盖） | M2 第 2 条、M1 第 1 条 | `rm_rmw`、`rm_protect` |
| 必须执行 Reduce：资源不可用反压，不降级转发 | M1 第 2 条 | `rm_no_bypass` |
| 计算精度：BF16 → FP32，累加 FP32，输出可配 FP32 / BF16 | `common/numeric` | `rm_precision_bits` |
| 输出前置检查：全部方向输入完成后进输出队列；发前查目标 VC credit 与下游 Reduce credit；每完成一 flit 可发一 flit | M3 第 1、2 条 | `rm_emit_per_flit` |
| Reduce credit 逐 flit 扣，按 release(UserID) 恢复；Router 不维护 | M3 第 2、4 条 | `rm_credit` |
| Credit 释放：输出 flit 被接受后产生携 UserID 的 release | M3 第 4 条 | `rm_release` |
| 本级 credit（DTE 侧）：DTE 持每项 credit，整包够才发；每 Reduce 一 flit 还一个 | DTE 文档 Commit + M3 第 4 条 | `dte_reduce_credit` |
| ReduceModule 间双 credit（Reduce credit + VC credit） | M3 第 2 条 | `rm_dual_credit` |
| Entry 分配与删除：创建 Stream 时分配，Retire 且 credit 归位时删除 | M1 第 1 条、M3 第 5 条 | `rm_entry_lifecycle` |
| 完成返回 UserID | M3 第 3 条 | `rm_done` |
| 三路输入仲裁 | M1 第 2 条 | `rm_arb` |
| 吞吐：256 B/cycle、1 个加法端口，N 字节耗 `ceil(N / 256)` 拍，同地址冲突串行化 | M2 第 4 条 | `rm_throughput` |
| reduce 加法跳过软件辅助信息 16 B | M2 第 1 条 | `rm_skip_sw_header` |
| Rmem 与 CoreMem 共用同一套 credit（单账） | Retire 与 Stream 表 | `rm_single_ledger` |

### Retire

| 机制 | 落点 | 用例 |
| - | - | - |
| Core 的保证：全部进出搬运完成且不再发起后才 Retire | TS 文档 Credit_monitor 的 Head-only 退休条件 | `retire_core_guarantee` |
| Retire 广播到 Router 与 ReduceModule | E1 | `retire_broadcast` |
| Router 回收：停新发送，删全部 Stream 表项 | E1 第 1 条 | `retire_router` |
| ReduceModule 延迟回收：credit 全部恢复初值后删映射 | M3 第 5 条 | `retire_reduce_delayed` |
| 向所有相邻上游发 release；坏核透传 | E1 第 3 条 | `retire_release_upstream`、`badcore_release_passthrough` |

### CoreMemCreditMonitor

| 机制 | 落点 | 用例 |
| - | - | - |
| 注册：TS 任务前序无依赖时登记 | K1 第 1 条 | `cm_register` |
| 资源查询与即时通知 | K1 第 2、3 条 | `cm_immediate` |
| 挂起监听，满足后通知 | K1 每拍重查 | `cm_pending` |
| 最老 StreamID 仲裁 | K1 第 3 条 | `cm_oldest` |
| 重发任务注册 | K1 第 1 条带 `reissue` | `cm_reissue_register` |
| 同 VC 保序 | R2 第 5 条 | `rs_vc_order` |
| credit 广播参与仲裁 | 各 Station 读同一份 `vc_credit` 视图（`credit_view`） | — |

### 坏核

坏核只有 Router 的八种模块，其中 CoreStation 永远不准入、ReduceModule 不累加、CreditMonitor 空转。数据面只按 RouterTable 做 router→router 转发，不查 Stream、不占坑；控制面把 Stream / Reduce release 按 Credit Bypass Route 透传。用例：`badcore_transit`、`badcore_release_passthrough`、A15 / A16。

***

## 8　取舍

* **R2 为什么只对 VC 队首的头 flit 做完整判定，数据 flit 沿用 `pkt_ctx`**
  * 这是两条规则的最小实现：flit 级交织（Router 间可切包）与整包锁定（进 core / Reduce）
  * 交织只发生在 R3 的出口选择上，锁定只是 `xbar_lock` 两项
* **credit 的 release 为什么走独立的脉冲通道，不由 flit 反向携带**
  * 三种 credit 各有自己的静态 Bypass Route，与数据路径无关
