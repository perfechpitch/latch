# MU DSA

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **MU DSA**

给实现 MU 的人：七个独立打拍的模块各自的端口、存储器、流水线与逐级行为、参数与机制。

七个模块：regfile、issue_q、gen_ep_info、agu ×3 与 acu、ldq ×2、matrix exe、stq。

两条建模前提：

* 阵列按 32 物理 lane、原语 K128×N64 建
* Matrix Mem 按 32 bank 与 lane 一对一

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《执行单元与存储》“MU DSA（矩阵单元）”全部小节
* 《软件栈》“MU core 100 T 内三件事”

***

## 1　定位与边界

MU 做 GEMV，一趟数据流：

* **读**：token 从 Core Mem，weight 从 Matrix Mem（32 bank 一对一，无 crossbar）
* **算**：32 lane 各 10 级流水的 MAC 阵列，算 `C = A × B` 或 `C = C + (A × B) × W_ep`
* **写**：结果经 stq 拼成 1 KB 写回 Core Mem

任务由 MU RV core 写寄存器加 trigger 下发，走 `regfile → issue_q → gen_ep_info → agu → ldq → matrix exe → stq`。load、计算、写回三段在相邻 task 之间重叠。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1180 560" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs>
    <marker id="u0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker>
    <marker id="u0s" markerWidth="9" markerHeight="9" refX="1" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#475569"/></marker>
  </defs>
  <rect x="0" y="0" width="1180" height="560" fill="#ffffff"/>
  <text x="20" y="28" font-size="12" fill="#111827">MU DSA · 第 0 层</text>

  <polygon points="30,90 130,90 120,126 20,126" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="112" font-size="10.5" fill="#374151" text-anchor="middle">dsa_cfg · dsa_rsp</text>
  <text x="75" y="144" font-size="9" fill="#6b7280" text-anchor="middle">← MU RV core</text>
  <polygon points="30,200 130,200 120,236 20,236" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="222" font-size="10.5" fill="#374151" text-anchor="middle">topk_wr</text>
  <text x="75" y="254" font-size="9" fill="#6b7280" text-anchor="middle">← DTE</text>
  <polygon points="30,470 130,470 120,506 20,506" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="492" font-size="10.5" fill="#374151" text-anchor="middle">cfg</text>

  <rect x="200" y="70" width="170" height="70" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="212" y="94" font-size="12" fill="#111827">regfile</text>
  <text x="212" y="112" font-size="10" fill="#475569">配置寄存器 · 收齐即下发</text>
  <text x="212" y="128" font-size="10" fill="#475569">静态 / 动态划分</text>
  <line x1="132" y1="108" x2="198" y2="108" stroke="#475569" marker-start="url(#u0s)" marker-end="url(#u0)"/>
  <rect x="200" y="170" width="170" height="60" fill="#f8fafc" stroke="#374151"/>
  <rect x="204" y="174" width="162" height="52" fill="none" stroke="#374151"/>
  <g stroke="#374151"><line x1="220" y1="174" x2="220" y2="226"/></g>
  <text x="228" y="194" font-size="11" fill="#111827">issue_q · FIFO 16</text>
  <text x="228" y="212" font-size="10" fill="#475569">顺序执行 · 满则反压</text>
  <line x1="285" y1="142" x2="285" y2="168" stroke="#475569" marker-end="url(#u0)"/>
  <rect x="200" y="260" width="170" height="70" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="212" y="284" font-size="12" fill="#111827">gen_ep_info</text>
  <text x="212" y="302" font-size="10" fill="#475569">topK global → local index</text>
  <text x="212" y="318" font-size="10" fill="#475569">weight 地址</text>
  <line x1="285" y1="232" x2="285" y2="258" stroke="#475569" marker-end="url(#u0)"/>
  <rect x="200" y="360" width="170" height="80" fill="#f8fafc" stroke="#374151"/>
  <rect x="204" y="364" width="162" height="72" fill="none" stroke="#374151"/>
  <text x="212" y="384" font-size="11" fill="#111827">topk_ep_table · 16 × 16 项</text>
  <text x="212" y="400" font-size="11" fill="#111827">local_ep_table · 256 项</text>
  <text x="212" y="418" font-size="10" fill="#475569">DTE 写 / 软件初始化</text>
  <line x1="132" y1="218" x2="198" y2="380" stroke="#475569" marker-end="url(#u0)"/>
  <line x1="285" y1="358" x2="285" y2="332" stroke="#475569" stroke-dasharray="3 2" marker-end="url(#u0)"/>

  <rect x="440" y="260" width="200" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="452" y="284" font-size="12" fill="#111827">agu ×3 · acu</text>
  <text x="452" y="302" font-size="10" fill="#475569">token / weight / store</text>
  <text x="452" y="318" font-size="10" fill="#475569">先 tile_K 再 tile_N</text>
  <text x="452" y="334" font-size="10" fill="#475569">acu 对齐与越界</text>
  <line x1="372" y1="295" x2="438" y2="295" stroke="#475569" marker-end="url(#u0)"/>

  <rect x="700" y="160" width="200" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="712" y="184" font-size="12" fill="#111827">ldq ×2</text>
  <text x="712" y="202" font-size="10" fill="#475569">Token ldq 16 + outstanding 16×256 B</text>
  <text x="712" y="218" font-size="10" fill="#475569">Weight ldq 4 + 乒乓 2 级</text>
  <text x="712" y="234" font-size="10" fill="#475569">vlane MUX · 非对齐移位</text>
  <line x1="642" y1="290" x2="700" y2="240" stroke="#475569" marker-end="url(#u0)"/>
  <polygon points="950,90 1060,90 1050,126 940,126" fill="#f8fafc" stroke="#374151"/>
  <text x="1000" y="112" font-size="10.5" fill="#374151" text-anchor="middle">cmem_rd</text>
  <polygon points="950,160 1060,160 1050,196 940,196" fill="#f8fafc" stroke="#374151"/>
  <text x="1000" y="182" font-size="10.5" fill="#374151" text-anchor="middle">mmem_rd</text>
  <line x1="902" y1="190" x2="940" y2="110" stroke="#475569" marker-start="url(#u0s)" marker-end="url(#u0)"/>
  <line x1="902" y1="210" x2="940" y2="180" stroke="#475569" marker-start="url(#u0s)" marker-end="url(#u0)"/>

  <rect x="700" y="290" width="200" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="712" y="314" font-size="12" fill="#111827">matrix exe</text>
  <text x="712" y="332" font-size="10" fill="#475569">32 lane × 10 级 · CSA 树</text>
  <text x="712" y="348" font-size="10" fill="#475569">MX scale 每 32 MAC 一组</text>
  <text x="712" y="364" font-size="10" fill="#475569">vlane 旁路 · Ksplit_acc</text>
  <line x1="800" y1="252" x2="800" y2="288" stroke="#475569" marker-end="url(#u0)"/>

  <rect x="700" y="420" width="200" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="712" y="444" font-size="12" fill="#111827">stq</text>
  <text x="712" y="462" font-size="10" fill="#475569">stq 16 · concat buffer 每 lane</text>
  <text x="712" y="478" font-size="10" fill="#475569">凑 1 KB 突发写 · 输出类型转换</text>
  <text x="712" y="494" font-size="10" fill="#475569">task_finish → issue_q</text>
  <line x1="800" y1="382" x2="800" y2="418" stroke="#475569" marker-end="url(#u0)"/>
  <line x1="640" y1="330" x2="700" y2="450" stroke="#475569" stroke-dasharray="3 2" marker-end="url(#u0)"/>
  <polygon points="950,440 1060,440 1050,476 940,476" fill="#f8fafc" stroke="#374151"/>
  <text x="1000" y="462" font-size="10.5" fill="#374151" text-anchor="middle">cmem_wr</text>
  <line x1="902" y1="458" x2="940" y2="458" stroke="#475569" marker-start="url(#u0s)" marker-end="url(#u0)"/>
  <polyline points="700,500 600,500 600,220 372,220" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#u0)"/>
  <text x="480" y="216" font-size="9" fill="#6b7280">task_finish</text>
  <polygon points="950,520 1060,520 1050,556 940,556" fill="#f8fafc" stroke="#374151"/>
  <text x="1000" y="542" font-size="10.5" fill="#374151" text-anchor="middle">ts_done</text>
  <polyline points="372,200 400,200 400,540 940,540" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#u0)"/>
  <line x1="132" y1="488" x2="200" y2="120" stroke="#475569" stroke-dasharray="2 3"/>
</svg>
```

***

## 2　接口

```
port dsa_cfg (slave, valid/ready, clk)            // MU RV core 的 dsaw / dsar
  in  req_valid
  out req_ready                                     // = !STATUS.QUEUE_FULL（issue_q 未满）
  in  req_we · req_addr[15:0] · req_data[31:0] · req_stream_id[3:0] · req_user_id[15:0] · req_task_id[5:0] · req_rq_idx[2:0]
port dsa_rsp (master, 脉冲, clk)
  out valid · rq_idx[2:0] · data[31:0]
port topk_wr (slave, valid/ready, clk)            // DTE 写 topK 表
  in  req_valid · req_stream_id[3:0] · req_idx[3:0] · req_expert[15:0] · req_weight[31:0]
  out req_ready                                     // = 1（表写口空闲）
port cmem_rd (master, valid/ready, clk)           // 存储文档 cmem_mu_rd 的镜像，132 B
port cmem_wr (master, valid/ready, clk)           // 存储文档 cmem_mu_wr 的镜像，132 B；1 KB 突发 = 8 拍
port mmem_rd (master, valid/ready, clk)           // 存储文档 mmem_mu_rd 的镜像：一个行号广播 32 bank，回 8 KB + 1 KB
port ts_done (master, 脉冲, clk)                  // → TS done_ack[MU_DSA]，trigger 含 last 时
  out valid · stream_id[3:0] · task_id[5:0] · user_id[15:0]
port cfg (slave, ctrl_noc 写事务, clk)            // 静态寄存器、local_ep_table 初始化
  in  cfg_valid · cfg_addr[15:0] · cfg_we · cfg_wdata[31:0]
  out cfg_rdata[31:0]
```

***

## 3　存储器

```
mem regs            FF        {SYS_CTRL, TASK_CFG{prim[2:0], vlane, acc, out_type}, TASK_BLOCK{K[15:0], N[15:0]}, ADDR_TOKEN[19:0], ADDR_WEIGHT[25:0], ADDR_SCALE[25:0], ADDR_OUT[19:0], expert_en_config[15:0], topk_table_addr, topk_stream_stride, router_expert_count[3:0], trigger{last, no_ack}, STATUS{QUEUE_FULL, BUSY}}  1RW  dsaw 写  复位 0
mem local_ep_table  FF 阵列   256 × local_idx[7:0]                1R1W   cfg 初始化                    复位 0
mem topk_ep_table   FF 阵列   16 × 16 × {expert[15:0], weight[31:0]}  1R1W  DTE 写，按 stream_id        复位 0
mem issue_q         FIFO      深 16 × cfg_info{regs 快照, ids}    1W1R   满 → STATUS.QUEUE_FULL       复位空
mem ep_latch        级间 latch {valid, task, expert_local[15:0], w_addr[25:0], w_ep[31:0]}  —  每拍覆写  —   // gen_ep_info → agu
mem tile_ctx        FF        {task, k_idx, n_idx, k_tiles, n_tiles}  1RW  agu 拆分游标                复位空
mem tok_ldq         FIFO      深 16 × {addr[19:0], bytes[8:0], k_idx}  1W1R  满 → agu 等              复位空
mem tok_outstanding FIFO      深 16 × 256 B                        1W1R   先发后回填                    复位空     // Rd outstanding buffer
mem wt_ldq          FIFO      深 4 × {row[11:0], k_idx}            1W1R   满 → agu 等                   复位空
mem wt_pingpong     FF 阵列   2 × 32 × 256 B                       1W1R   MAC 入口乒乓                  复位空
mem tok_latch       级间 latch {valid, a[2047:0], vlane, acc, out_type, k_idx}  —  每拍覆写  —          // ldq → matrix exe
mem mac_pipe        级间 latch 32 lane × 10 级 × {partial, exp}    —      每拍推进                      —          // 单 lane 10 级
mem ksplit_acc      FF 阵列   32 × 2 × FP32                        1RW    vlane 分组累加                复位 0
mem exe_out_latch   级间 latch {valid, lane_out[32][31:0], n_idx, last}  —  每拍覆写  —                // matrix exe → stq
mem stq             FIFO      深 16 × {addr[19:0], n_idx, out_type}  1W1R  双条件启动                   复位空
mem concat_buf      FF 阵列   32 lane × 深 1～16 × 32 b            1W1R   按 lane 距离线性分布          复位空     // Wr concat buffer
mem assemble_latch  级间 latch {valid, data[8191:0], mask[31:0]}    —      每拍覆写                      —          // 1 KB 拼接器
```

***

## 4　流水线总览

一个 task 走这几步：

1. Q1 issue_q 出队
2. G1 gen_ep_info
3. A1 agu 拆 tile
4. 两条 load 并行：token 经 Cmem 16 拍，weight 经 Mmem 4 拍
5. X1 matrix exe：10 级 + `ceil(K × N / 算力)`
6. S1 stq 拼 1 KB：vlane=1 用 8 拍，vlane=2 用 4 拍
7. Cmem 写 16 拍 → finish

task N 计算时 task N+1 load，三段重叠。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1180 400" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="h0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="1180" height="400" fill="#ffffff"/>
  <text x="20" y="26" font-size="12" fill="#111827">MU · 第 1 层（前三级按比例；load、计算、写回段标非按比例，拍数见 Dx）</text>
  <g stroke="#e5e7eb"><line x1="140" y1="40" x2="140" y2="360"/><line x1="216" y1="40" x2="216" y2="360"/><line x1="292" y1="40" x2="292" y2="360"/><line x1="368" y1="40" x2="368" y2="360"/></g>
  <g font-size="8.5" fill="#6b7280"><text x="140" y="50">t0</text><text x="216" y="50">t1</text><text x="292" y="50">t2</text><text x="368" y="50">t3</text></g>

  <polygon points="24,90 122,90 114,122 16,122" fill="#f8fafc" stroke="#374151"/>
  <text x="69" y="110" font-size="10" fill="#374151" text-anchor="middle">dsa_cfg</text>
  <line x1="122" y1="106" x2="138" y2="106" stroke="#475569" marker-end="url(#h0)"/>
  <rect x="144" y="80" width="68" height="52" fill="#f8fafc" stroke="#374151"/>
  <rect x="148" y="84" width="60" height="44" fill="none" stroke="#374151"/>
  <g stroke="#374151"><line x1="160" y1="84" x2="160" y2="128"/></g>
  <text x="166" y="102" font-size="9" fill="#111827">issue_q</text>
  <text x="166" y="116" font-size="8.5" fill="#475569">FIFO 16</text>
  <rect x="220" y="80" width="68" height="52" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="224" y="76" font-size="8.5" fill="#6b7280">G1</text><text x="286" y="76" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="226" y="100" font-size="11" fill="#111827">gen_ep_info</text>
  <text x="226" y="116" font-size="9.5" fill="#475569">local idx · w 地址</text>
  <line x1="212" y1="106" x2="218" y2="106" stroke="#475569" marker-end="url(#h0)"/>
  <rect x="296" y="80" width="68" height="52" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="300" y="76" font-size="8.5" fill="#6b7280">A1</text><text x="362" y="76" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="302" y="100" font-size="11" fill="#111827">agu / acu</text>
  <text x="302" y="116" font-size="9.5" fill="#475569">tile_K → tile_N</text>
  <line x1="288" y1="106" x2="294" y2="106" stroke="#475569" marker-end="url(#h0)"/>

  <rect x="400" y="60" width="150" height="44" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="404" y="56" font-size="8.5" fill="#6b7280">T1</text><text x="548" y="56" font-size="8.5" fill="#6b7280" text-anchor="end">D16</text>
  <text x="406" y="80" font-size="10.5" fill="#7c2d12">Token ldq：Cmem 读</text>
  <text x="406" y="94" font-size="9.5" fill="#92400e">outstanding 16 · vlane MUX</text>
  <rect x="400" y="114" width="150" height="44" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="404" y="110" font-size="8.5" fill="#6b7280">W1</text><text x="548" y="110" font-size="8.5" fill="#6b7280" text-anchor="end">D4</text>
  <text x="406" y="134" font-size="10.5" fill="#7c2d12">Weight ldq：Mmem 读</text>
  <text x="406" y="148" font-size="9.5" fill="#92400e">一地址广播 32 bank · 乒乓</text>
  <line x1="364" y1="98" x2="398" y2="82" stroke="#475569" marker-end="url(#h0)"/>
  <line x1="364" y1="112" x2="398" y2="136" stroke="#475569" marker-end="url(#h0)"/>
  <polygon points="570,60 660,60 652,92 562,92" fill="#f8fafc" stroke="#374151"/>
  <text x="611" y="80" font-size="10" fill="#374151" text-anchor="middle">cmem_rd</text>
  <polygon points="570,114 660,114 652,146 562,146" fill="#f8fafc" stroke="#374151"/>
  <text x="611" y="134" font-size="10" fill="#374151" text-anchor="middle">mmem_rd</text>
  <line x1="550" y1="76" x2="562" y2="76" stroke="#475569" marker-end="url(#h0)"/>
  <line x1="550" y1="130" x2="562" y2="130" stroke="#475569" marker-end="url(#h0)"/>

  <rect x="400" y="200" width="250" height="60" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="404" y="196" font-size="8.5" fill="#6b7280">X1</text><text x="648" y="196" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="406" y="220" font-size="10.5" fill="#7c2d12">matrix exe：32 lane × 10 级</text>
  <text x="406" y="236" font-size="9.5" fill="#92400e">拍数 = 启动 + ceil(K×N / 算力) + 写回</text>
  <text x="406" y="250" font-size="9.5" fill="#92400e">token 逐 lane 错 1 拍脉动；CSA 树 + MX scale</text>
  <line x1="470" y1="104" x2="470" y2="198" stroke="#475569" marker-end="url(#h0)"/>
  <line x1="500" y1="158" x2="500" y2="198" stroke="#475569" marker-end="url(#h0)"/>

  <rect x="400" y="290" width="250" height="60" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="404" y="286" font-size="8.5" fill="#6b7280">S1</text><text x="648" y="286" font-size="8.5" fill="#6b7280" text-anchor="end">D8 / D4 + 16</text>
  <text x="406" y="310" font-size="10.5" fill="#7c2d12">stq：concat 拼 1 KB → Cmem 写</text>
  <text x="406" y="326" font-size="9.5" fill="#92400e">vlane=1 横切 8T · vlane=2 纵向 4T</text>
  <text x="406" y="340" font-size="9.5" fill="#92400e">写延迟 16 · 不足按 mask</text>
  <line x1="525" y1="262" x2="525" y2="288" stroke="#475569" marker-end="url(#h0)"/>
  <polygon points="700,300 790,300 782,332 692,332" fill="#f8fafc" stroke="#374151"/>
  <text x="741" y="320" font-size="10" fill="#374151" text-anchor="middle">cmem_wr</text>
  <line x1="650" y1="316" x2="692" y2="316" stroke="#475569" marker-end="url(#h0)"/>
  <polygon points="700,360 790,360 782,392 692,392" fill="#f8fafc" stroke="#374151"/>
  <text x="741" y="380" font-size="10" fill="#374151" text-anchor="middle">ts_done</text>
  <polyline points="600,350 600,376 692,376" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#h0)"/>
  <text x="820" y="380" font-size="9" fill="#6b7280">写完成 → task_finish → issue_q 退出 → last 时报 TS</text>

  <text x="820" y="220" font-size="10" fill="#374151">重叠：task0 写回 ‖ task1 计算 ‖ task2 load。</text>
  <text x="820" y="238" font-size="10" fill="#374151">issue_q 允许执行通路提前取下一 task。</text>
</svg>
```

***

## 5　逐级行为

### R1 · regfile（寄存器写与下发）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `dsa_cfg`、`regs`、`issue_q` 余量 | 1. `req_we` → 写 `regs[addr]`；写 `trigger`（`dsawi.d topk_stream_stride, trigger`）→ 收齐一个 task：`issue_q.push({regs 快照, ids 取随指令附带的 stream / user / task id, last, no_ack})`<br>2. `STATUS.QUEUE_FULL = issue_q 满`；`req_ready = !QUEUE_FULL`（反压 RV core 的 dsa_iss）<br>3. `!req_we` → 下拍 `dsa_rsp`<br>4. `router_expert_count == 0` → 该 task 忽略 topK 相关寄存器 | `regs`、`issue_q`、`dsa_rsp` | D1 |

### Q1 · issue_q（顺序执行与 finish）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `issue_q`、stq 的 `task_finish` | 1. 队首 task 交给 G1；执行通路可提前取下一 task（load 段），队首仍占位到 finish<br>2. `task_finish(task)` → 出队；`last ∧ !no_ack` → `ts_done = {stream_id, task_id, user_id}` | G1 入口、`ts_done`、`issue_q` | D1 |

### G1 · gen_ep_info

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| 当前 task 的 `regs` 快照、`topk_ep_table[stream_id]`、`local_ep_table` | 1. 对 topK 的每个 `{expert_global, weight}`：`local = local_ep_table[expert_global]`；`w_addr = ADDR_WEIGHT + local × 专家权重步长`<br>2. FC1 / FC3 只用 ids；FC2 用 ids 与 weights（`W_ep` FP32）<br>3. `ep_latch = {task, local, w_addr, w_ep}` | `ep_latch` | D1 |

### A1 · agu ×3 与 acu

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `ep_latch`、`tile_ctx`、`tok_ldq` / `wt_ldq` / `stq` 余量 | 1. `Split`：按原语（K128×N64 等）`k_tiles = K / tile_K`，`n_tiles = N / tile_N`；先循环 tile_K 再 tile_N<br>2. Token agu：`tok_ldq.push({ADDR_TOKEN + k_idx × tile_K × 元素字节, bytes, k_idx})`，burst，byte 对齐<br>3. Weight agu：`wt_ldq.push({row = (w_addr + k_idx × tile_K × tile_N × 字节) / 256, k_idx})`，256 B 对齐，一个行号逐级脉动到各 lane<br>4. Store agu：`stq.push({ADDR_OUT + n_idx × 256 B, n_idx, out_type})`，单 lane 4 B（vlane=2 时 8 B）<br>5. acu：对齐校验、越界捕获 → 断言（异常不建） | `tok_ldq`、`wt_ldq`、`stq`、`tile_ctx` | D1 |

### T1 · Token ldq

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `tok_ldq`、`tok_outstanding` 余量、`cmem_rd` | 1. 每拍从队头发 1 个 `cmem_rd.req`（`tok_outstanding` 有空则先发后回填）<br>2. `rsp_valid` → 按 k_idx 回填 `tok_outstanding`；非对齐移位拼接<br>3. vlane MUX：只读 `256 B / vlane_num`，复制扩展到 256 B → `tok_latch = {a, vlane, acc, out_type, k_idx}`；控制随数据 | `cmem_rd`、`tok_outstanding`、`tok_latch` | D16（Cmem 读延迟，待定） |

### W1 · Weight ldq

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `wt_ldq`、`wt_pingpong`、`mmem_rd` | 1. 每拍发 1 个 `mmem_rd.req{row}`（乒乓有空页时）<br>2. `rsp_valid`（8 KB + 1 KB scale）→ 写入乒乓的空页；MAC 独享 Mmem 带宽<br>3. 乒乓 2 级掩盖 4 拍读出延迟（读 SRAM 2T + 打拍 2T） | `mmem_rd`、`wt_pingpong` | D4 |

### X1 · matrix exe

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `tok_latch`、`wt_pingpong`、`mac_pipe`、`ksplit_acc`、`ep_latch.w_ep` | 1. Token 脉动：lane i 在第 i 拍拿到 `a`（逐 lane 错 1 拍）<br>2. 每 lane 每拍：MAC 后指数对齐，无符号 CSA 树压缩累加；OCP MX scale 每 32 MAC 一组，组内累加后乘 scale，组间累加；`MATH_NAN_INF` Clamp<br>3. vlane=2：CSA 第 128 层旁路 MUX，`ksplit_acc` 两组，单 lane 双结果<br>4. 原语模式 `C = A × B` 或 `C = C + (A × B) × W_ep`（无初始 C，`W_ep` FP32）；bit 级顺序与参考实现同<br>5. 一次原语拍数 = 启动 + `ceil(K × N / 算力)` + 写回；算力 BF16 4K / MXFP8 8K / W4A8 16K / W4A16 8K MAC/T<br>6. 末 K tile → `exe_out_latch = {lane_out[32], n_idx, last}` | `mac_pipe`、`ksplit_acc`、`exe_out_latch` | D变长 |

### S1 · stq 与 concat

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `stq`、`exe_out_latch`、`concat_buf`、`assemble_latch`、`cmem_wr` | 1. 双条件启动：`stq` 队首（store-acu 地址）与 `exe_out_latch.valid`（启动信息）都到<br>2. 各 lane 结果进 `concat_buf[lane]`（深度按到 Cmem 的距离 1～16）<br>3. 拼装：vlane=1 横切，128 B 截面取 8 次凑 1 KB（8T）；vlane=2 纵向，256 B 截面取 4 次（4T）<br>4. 输出类型转换 FP32 / BF16（`numeric`）；`assemble_latch = {data, mask}`，不足 1 KB 按实际标 mask<br>5. `cmem_wr` 突发写（1 KB = 8 拍 132 B）；按绝对地址顺序<br>6. 写完成（bvalid）→ `task_finish(task)` 给 issue_q | `cmem_wr`、`concat_buf`、`assemble_latch`、`task_finish` | D8 / D4 + 16（Cmem 写延迟，待定） |

***

## 6　参数汇总

```
ISSUE_Q_DEPTH        16
TOK_LDQ_DEPTH        16；TOK_OUTSTANDING 16 × 256 B（4 KB）
WT_LDQ_DEPTH         4；WT_PINGPONG 2 级
STQ_DEPTH            16；CONCAT_DEPTH 每 lane 1～16 线性分布（最远 16 拍、最近 1 拍）
LANES                32（左右镜像各 16）；单 lane 10 级
PRIMITIVE            K128×N64（输出带宽 256 B）；8 种原语按 TASK_CFG.prim
MAC_RATE             BF16 4K / MXFP8 8K / W4A8 16K / W4A16 8K MAC/T
CMEM_RD_LATENCY      16          // 待定
CMEM_WR_LATENCY      16          // 待定，第 8 章
MMEM_RD_LATENCY      4（读 SRAM 2T + 打拍 2T）
CMEM_TOKEN_BW        256 B（接口 512 B / 1 KB）
MMEM_WEIGHT_BW       8 KB（32 bank × 256 B/T，无 crossbar）
CMEM_STORE_BW        256 B（接口 1 KB）
Primitive 1×64×128 且 K=64 时 50% 性能损失；Tile 过小的损失由 RV core 配置延时与启动延时决定
DIDT 分级启动、零输入旁路、错峰启动、Drain & Trap   不建
```

***

## 7　机制覆盖

### regfile 与 issue_q

| 机制 | 落点 | 用例 |
| - | - | - |
| 收齐即下发：收完一个 task 全部寄存器（写 `trigger`）后发 issue_q | R1 第 1 条 | `mu_regfile_trigger` |
| 启动指令 `dsawi.d topk_stream_stride, trigger`；stream / task / user id 由 DSA 从 RV core CSR 读 | R1 第 1 条（随指令附带的 ID） | `mu_ids_from_csr` |
| `router_expert_count = 0` 忽略 topK | R1 第 4 条 | `mu_no_topk` |
| 顺序执行、满则拒收（反压 RV core 的 dsa_iss）、not_empty 才发 | R1 第 2 条、Q1 | `mu_issue_q` |
| 提前调度：task N 计算时 task N+1 load，三段重叠 | Q1 第 1 条 | `mu_overlap` |
| finish 收集：收 stq 的 task_finish，退出并通知 TS（trigger 含 last） | Q1 第 2 条 | `mu_finish` |
| 静态 / 动态寄存器划分 | `regmap.h` | `rv_static_dynamic` |

### gen_ep_info、topK 表与专家加权累加

| 机制 | 落点 | 用例 |
| - | - | - |
| gen_ep_info：global index 查 local_ep_table 得 local index，算 weight 地址 | G1 第 1 条 | `mu_gen_ep_info` |
| FC1 / FC3 只用 ids，FC2 用 ids 与 weights | G1 第 2 条 | `mu_fc_modes` |
| token 与 topK 分存 | `topk_ep_table` 独立 | — |
| 专家加权累加 `C = C + (A × B) × W_ep`，无初始 C，W_ep FP32 | X1 第 4 条 | `mu_expert_weighted_sum_bits` |
| 完整式子 `D = sf_A × sf_B × A × B + C` | X1 + `common/numeric` | `mu_gemv_bits` |

### agu ×3 与 acu

| 机制 | 落点 | 用例 |
| - | - | - |
| 任务拆分：先 tile_K 再 tile_N | A1 第 1 条 | `mu_split` |
| Token agu：burst，byte 对齐，Cmem 侧移位拼接 | A1 第 2 条 + T1 第 2 条 | `mu_token_agu` |
| Weight agu：256 B 对齐，一个地址逐级脉动到各 lane，32 bank 同步寻址 | A1 第 3 条 + W1 | `mu_weight_agu` |
| Store agu | A1 第 4 条 | `mu_store_agu` |
| acu 对齐校验、越界捕获 | A1 第 5 条 | — |

### ldq ×2 与 Rd outstanding buffer

| 机制 | 落点 | 用例 |
| - | - | - |
| outstanding 掩盖延迟：先发后回填，按使能 K 值读 buffer 输出 | T1 第 1、2 条 | `mu_ldq_outstanding` |
| 非对齐移位 | T1 第 2 条 | `mu_token_agu` |
| vlane MUX | T1 第 3 条 | `mu_vlane` |
| 控制随数据 | `tok_latch` 字段 | — |
| Weight 乒乓 | W1 第 2、3 条 | `mu_weight_pingpong` |
| Token 读延迟 16、Weight 读延迟 4T | T1 / W1 的 Dx | `mu_ldq_latency` |

### matrix exe

| 机制 | 落点 | 用例 |
| - | - | - |
| 指数提前 + 纯定点 CSA 树 | X1 第 2 条 + `common/numeric/csa_tree.h` | `mu_csa_bits` |
| OCP MX scale 每 32 MAC 一组 | X1 第 2 条 | `mu_mx_scale_bits` |
| 8 种原语、四种精度组合、算力 | X1 第 5 条 | `mu_primitives` |
| vlane 分组 | X1 第 3 条 | `mu_vlane` |
| Token 脉动：各 lane 1 拍错位 | X1 第 1 条 | `mu_systolic_skew` |
| 单 lane 10 级流水；一次原语的拍数 | X1 第 5 条 | `mu_primitive_cycles` |
| MATH_NAN_INF Clamp | X1 第 2 条 | `mu_nan_clamp` |
| DIDT 分级启动、零输入旁路、错峰启动 | 不建 | — |

### stq 与 Wr concat buffer

| 机制 | 落点 | 用例 |
| - | - | - |
| 双条件启动 | S1 第 1 条 | `mu_stq_start` |
| vlane=1 横切 8T；vlane=2 纵向 4T | S1 第 3 条 | `mu_stq_assemble` |
| 凑 1 KB 突发写；不足按实际标 mask | S1 第 4、5 条 | `mu_stq_burst` |
| 输出类型转换 FP32 / BF16 | S1 第 4 条 | `mu_out_type_bits` |
| 写完成上报 task_finish 给 issue_q | S1 第 6 条 | `mu_finish` |

***

## 8　取舍

* **Token 与 Weight 两条 load 通路为什么各自独立打拍，在 matrix exe 入口会合**
  * 两者延迟差一个量级（16 与 4），且由不同存储供给
  * 把等待放在 `tok_outstanding` 与 `wt_pingpong` 两个缓冲上
  * matrix exe 本身只在两边都有数据时推进
* **concat buffer 为什么按 lane 距离给不同深度**
  * 照搬 MAS 的“越近 buffer 越大”
  * 让脉动阵列的错拍在写回侧被吸收，而不是在计算侧插空拍
