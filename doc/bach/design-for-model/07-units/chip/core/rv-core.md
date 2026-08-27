# RV Core（DTE / MU / VU 各一）

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **RV core ×3**

给实现 RV core 的人：一个独立打拍的模块的端口、存储器、流水线与逐级行为、参数与机制。

* 三个实例，区别只在绑定的 DSA、ITCM 里的镜像、可见的地址空间
* 指令执行器是 `src/rv32` 的功能模型

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《Core 内硬件》“RV Core”：指令集、自定义指令、task 下发与完成、dsa_iss 的下发规则、LSU 与访存分流
* 《软件栈》：“软件执行模型”“各类 core 的软件流程”

***

## 1　定位与边界

RV core 从 TS 收 task，按 `task_pc` 跑 ITCM 里的 kernel，两类指令：

* **custom-0 自定义指令**：配置对应的 DSA、读 DSA 寄存器、结束 task
* **普通 load / store**：访问 DTCM、Share Mem，以及（只有 DTE core 有的）Core Mem 与 Router I/O reg

指令逐条执行，不建流水线：

* `src/rv32` 的 `SystemRv32` 提供 RV32IMC、M 态 CSR 与译码
* 本模块覆盖 `Decode` 接入自定义指令
* 外面包一层 task_queue、dsa_iss、lsq 与 gpr 就绪表做逐拍记账

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1100 480" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs>
    <marker id="v0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker>
    <marker id="v0s" markerWidth="9" markerHeight="9" refX="1" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#475569"/></marker>
  </defs>
  <rect x="0" y="0" width="1100" height="480" fill="#ffffff"/>
  <text x="20" y="28" font-size="12" fill="#111827">RV core · 第 0 层</text>

  <polygon points="30,90 130,90 120,126 20,126" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="112" font-size="10.5" fill="#374151" text-anchor="middle">task</text>
  <text x="75" y="80" font-size="9" fill="#6b7280" text-anchor="middle">← TS rv_task[u]</text>
  <polygon points="30,170 130,170 120,206 20,206" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="192" font-size="10.5" fill="#374151" text-anchor="middle">task_done</text>
  <text x="75" y="224" font-size="9" fill="#6b7280" text-anchor="middle">→ TS done_ack</text>
  <polygon points="30,300 130,300 120,336 20,336" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="322" font-size="10.5" fill="#374151" text-anchor="middle">cfg</text>
  <text x="75" y="354" font-size="9" fill="#6b7280" text-anchor="middle">ITCM / DTCM 装载 · CSR 查询</text>

  <rect x="220" y="60" width="520" height="300" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="232" y="84" font-size="12" fill="#111827">RvCore</text>
  <rect x="240" y="100" width="220" height="110" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="120" font-size="11" fill="#111827">SystemRv32（src/rv32）</text>
  <text x="250" y="138" font-size="10" fill="#475569">RV32IMC · M 态 CSR · 译码</text>
  <text x="250" y="154" font-size="10" fill="#475569">gpr 32×32 b · pc</text>
  <text x="250" y="170" font-size="10" fill="#475569">Decode 覆盖：custom-0 → bach_insts</text>
  <text x="250" y="196" font-size="9.5" fill="#9ca3af">每拍同步跑完一条指令</text>
  <rect x="480" y="100" width="240" height="110" fill="#f8fafc" stroke="#374151"/>
  <rect x="484" y="104" width="232" height="102" fill="none" stroke="#374151"/>
  <text x="492" y="122" font-size="11" fill="#111827">itcm · SRAM 4 KB · 1R1W</text>
  <text x="492" y="138" font-size="11" fill="#111827">dtcm · SRAM 8 KB · 4 bank · 1RW</text>
  <text x="492" y="158" font-size="10" fill="#475569">task_queue · FIFO 2</text>
  <text x="492" y="174" font-size="10" fill="#475569">dsa_rq 8 · sm_lsq 16 · cm_lsq 16</text>
  <text x="492" y="190" font-size="10" fill="#475569">gpr_ready 32 × 就绪拍 · 自定义 CSR</text>
  <rect x="240" y="230" width="480" height="110" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="250" font-size="11" fill="#111827">逐拍记账</text>
  <text x="250" y="268" font-size="10" fill="#475569">task_queue → Start：写 pc / stream_id / task_id / user_id CSR</text>
  <text x="250" y="284" font-size="10" fill="#475569">dsa_iss：每拍 1 条 dsaw / dsar；dsa_rq 按序写回 gpr_ready</text>
  <text x="250" y="300" font-size="10" fill="#475569">lsq：每拍 1 请求；load 的目的 gpr 记就绪拍</text>
  <text x="250" y="316" font-size="10" fill="#475569">阻塞：源 gpr 未就绪 / lsq 或 dsa_iss 满 / task_done 后队空</text>
  <line x1="132" y1="108" x2="218" y2="108" stroke="#475569" marker-end="url(#v0)"/>
  <line x1="218" y1="188" x2="132" y2="188" stroke="#475569" marker-end="url(#v0)"/>
  <line x1="132" y1="318" x2="218" y2="318" stroke="#475569" stroke-dasharray="2 3" marker-end="url(#v0)"/>

  <polygon points="800,70 900,70 890,106 790,106" fill="#f8fafc" stroke="#374151"/>
  <text x="845" y="92" font-size="10.5" fill="#374151" text-anchor="middle">dsa_cfg</text>
  <polygon points="800,130 900,130 890,166 790,166" fill="#f8fafc" stroke="#374151"/>
  <text x="845" y="152" font-size="10.5" fill="#374151" text-anchor="middle">dsa_rsp</text>
  <text x="920" y="92" font-size="9" fill="#6b7280">→ 对应 DSA 的寄存器口</text>
  <text x="920" y="152" font-size="9" fill="#6b7280">← DSA 读返回</text>
  <line x1="742" y1="90" x2="790" y2="90" stroke="#475569" marker-end="url(#v0)"/>
  <line x1="790" y1="150" x2="742" y2="150" stroke="#475569" marker-end="url(#v0)"/>
  <polygon points="800,210 900,210 890,246 790,246" fill="#f8fafc" stroke="#374151"/>
  <text x="845" y="232" font-size="10.5" fill="#374151" text-anchor="middle">sm · sm_find</text>
  <text x="920" y="232" font-size="9" fill="#6b7280">→ Share Mem smem_rv[u] / smem_find</text>
  <line x1="742" y1="228" x2="790" y2="228" stroke="#475569" marker-start="url(#v0s)" marker-end="url(#v0)"/>
  <polygon points="800,280 900,280 890,316 790,316" fill="#f8fafc" stroke="#374151"/>
  <text x="845" y="302" font-size="10.5" fill="#374151" text-anchor="middle">cm</text>
  <text x="920" y="302" font-size="9" fill="#6b7280">→ Core Mem cmem_rv（仅 DTE core）</text>
  <line x1="742" y1="298" x2="790" y2="298" stroke="#475569" stroke-dasharray="4 3" marker-start="url(#v0s)" marker-end="url(#v0)"/>
  <polygon points="800,350 900,350 890,386 790,386" fill="#f8fafc" stroke="#374151"/>
  <text x="845" y="372" font-size="10.5" fill="#374151" text-anchor="middle">io_reg</text>
  <text x="920" y="372" font-size="9" fill="#6b7280">→ Router cs_hdr（仅 DTE core）</text>
  <line x1="742" y1="340" x2="790" y2="368" stroke="#475569" stroke-dasharray="4 3" marker-start="url(#v0s)" marker-end="url(#v0)"/>

  <text x="20" y="420" font-size="10.5" fill="#374151">三个实例的差别：绑定的 DSA（dsa_cfg 接谁）、ITCM 里的镜像、cm 与 io_reg 只有 DTE core 接。</text>
  <text x="20" y="440" font-size="10.5" fill="#374151">DTCM 的 bank 冲突、ITCM 的 ECC 不建；ITCM 取指 1 拍折进每条指令的 1 拍。</text>
</svg>
```

***

## 2　接口

```
port task (slave, valid/ready, clk)               // TS rv_task[u]
  in  cmd_valid
  out cmd_ready                                     // = task_queue 有空槽（上拍值）
  in  task_pc[31:0] · stream_id[3:0] · user_id[15:0] · task_id[5:0] · stream_num[4:0] · dsa_en · is_datain
port task_done (master, 脉冲, clk)                // task_done 指令带 TS 标志时
  out valid · stream_id[3:0] · task_id[5:0] · user_id[15:0]
port dsa_cfg (master, valid/ready, clk)           // dsa_iss：dsaw.* / dsar.* 下发，每拍最多 1 条
  out req_valid
  in  req_ready                                     // = DSA 下发通道未反压（上拍值）
  out req_we                                        // 1 = 写（dsaw），0 = 读（dsar）
  out req_addr[15:0]                                // DSA 寄存器编号（dsawi 5 bit 立即数或 rs1 的 byte 地址）
  out req_data[31:0]
  out req_stream_id[3:0] · req_user_id[15:0] · req_task_id[5:0]   // 随指令附带，取自自定义 CSR
  out req_rq_idx[2:0]                               // 读指令在 dsa_rq 的编号
port dsa_rsp (slave, 脉冲, clk)                   // DSA 读返回，按 dsa_rq 编号写回
  in  valid · rq_idx[2:0] · data[31:0]
port sm (master, valid/ready, clk)                // Share Mem smem_rv[u]，字段同该口
  out req_valid · req_addr[14:0] · req_we · req_wdata[31:0] · req_be[3:0]
  in  req_ready · rsp_valid · rsp_rdata[31:0]
port sm_find (master, valid/ready, clk)           // flag_check → Share Mem smem_find
  out req_valid · req_begin[14:0] · req_end[14:0]
  in  req_ready · rsp_valid · rsp_offset[31:0]
port cm (master, valid/ready, clk)                // 仅 DTE core：Core Mem cmem_rv，字段同该口
  out req_valid · req_addr[19:0] · req_we · req_wdata[31:0] · req_be[3:0]
  in  req_ready · rsp_valid · rsp_data[1023:0]
port io_reg (master, valid/ready, clk)            // 仅 DTE core：Router cs_hdr（读包头、写 1 弹出），复用 cm_lsq
  out rd_valid · pop_valid
  in  rd_ready · rd_data[63:0]
port cfg (slave, ctrl_noc 写事务, clk)            // ITCM / DTCM 装载、boot_pc、自定义 CSR 读、ready 电平
  in  cfg_valid · cfg_addr[15:0] · cfg_we · cfg_wdata[31:0]
  out cfg_rdata[31:0]
  out ready                                         // 电平：进入 wait 后为 1
```

***

## 3　存储器

```
mem itcm          SRAM        1024×4 B（4 KB）          1R1W   cfg 写，取指读                        复位未定义   // kernel 镜像代码段
mem dtcm          SRAM        4 bank × 512×4 B（8 KB）   1RW/bank  load/store 与 cfg 写              复位未定义   // 数据段、栈；bank 冲突不建
mem gpr           FF 阵列     32×32 b（SystemRv32）      2R1W   x0 恒 0                              复位 0
mem pc            FF          32 b（SystemRv32）         1RW    每条指令更新                          复位 boot_pc
mem csr_custom    FF          {stream_id[3:0], task_id[5:0], user_id[15:0], stream_num[4:0], status[1:0]}  1RW  task 开始硬件写；user_id / task_id 软件可写  复位 0
mem task_queue    FIFO        深 TASK_QUEUE_DEPTH × {task_pc[31:0], stream_id[3:0], user_id[15:0], task_id[5:0], stream_num[4:0], dsa_en, is_datain}  1W1R  满 → cmd_ready=0  复位空
mem exec_state    FF          {state[1:0]}                1RW    WAIT_TASK / RUN / BLOCKED / HALT      复位 WAIT_TASK
mem gpr_ready     FF 阵列     32 × 就绪拍[31:0]           1RW    load / dsar 写，退休清                复位 0        // 值 ≤ now 视为就绪
mem dsa_rq        FIFO        深 8 × {rd[4:0]}            1W1R   满 → dsar 等待                        复位空        // 在途 DSA 读的目的 gpr
mem sm_lsq        FIFO        深 16 × {addr[14:0], we, wdata[31:0], be[3:0], rd[4:0]}  1W1R  满 → 等待   复位空
mem cm_lsq        FIFO        深 16 × {addr[19:0], we, wdata[31:0], be[3:0], rd[4:0], is_io}  1W1R  满 → 等待  复位空   // 仅 DTE core，io_reg 复用
mem inst_cnt      FF          {retired[31:0], cycles[31:0]}  1RW  每 task 清                          复位 0        // 指令计数与拍数
```

***

## 4　流水线总览

一条指令一拍：

* E1 取指、译码、执行同拍完成，`SystemRv32` 同步跑完
* 访存与 DSA 读只发射不等待，延迟落在 gpr 就绪表上
* 后续指令的源 gpr 未就绪才停

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1180 400" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="e0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="1180" height="400" fill="#ffffff"/>
  <text x="20" y="26" font-size="12" fill="#111827">RV core · 第 1 层（每条指令 1 拍；访存与 DSA 读的返回标非按比例）</text>
  <g stroke="#e5e7eb"><line x1="140" y1="40" x2="140" y2="370"/><line x1="216" y1="40" x2="216" y2="370"/><line x1="292" y1="40" x2="292" y2="370"/><line x1="368" y1="40" x2="368" y2="370"/></g>
  <g font-size="8.5" fill="#6b7280"><text x="140" y="50">t0</text><text x="216" y="50">t1</text><text x="292" y="50">t2</text><text x="368" y="50">t3</text></g>

  <polygon points="24,80 122,80 114,112 16,112" fill="#f8fafc" stroke="#374151"/>
  <text x="69" y="100" font-size="10" fill="#374151" text-anchor="middle">task</text>
  <line x1="122" y1="96" x2="138" y2="96" stroke="#475569" marker-end="url(#e0)"/>
  <rect x="144" y="70" width="68" height="52" fill="#f8fafc" stroke="#374151"/>
  <rect x="148" y="74" width="60" height="44" fill="none" stroke="#374151"/>
  <g stroke="#374151"><line x1="160" y1="74" x2="160" y2="118"/></g>
  <text x="166" y="92" font-size="9" fill="#111827">task_queue</text>
  <text x="166" y="106" font-size="8.5" fill="#475569">FIFO 2 · 1W1R</text>
  <rect x="220" y="70" width="68" height="52" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="224" y="66" font-size="8.5" fill="#6b7280">S1</text><text x="286" y="66" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="226" y="90" font-size="11" fill="#111827">Start</text>
  <text x="226" y="106" font-size="9.5" fill="#475569">pc ← task_pc</text>
  <text x="226" y="118" font-size="9.5" fill="#475569">写 CSR</text>
  <line x1="212" y1="96" x2="218" y2="96" stroke="#475569" marker-end="url(#e0)"/>

  <rect x="296" y="150" width="220" height="110" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="300" y="146" font-size="8.5" fill="#6b7280">E1</text><text x="514" y="146" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="302" y="170" font-size="11" fill="#111827">取指 · 译码 · 执行</text>
  <text x="302" y="188" font-size="9.5" fill="#475569">itcm[pc] → Decode → Run</text>
  <text x="302" y="204" font-size="9.5" fill="#475569">源 gpr 未就绪 → 本拍不执行</text>
  <text x="302" y="220" font-size="9.5" fill="#475569">load / dsar：发射，记就绪拍</text>
  <text x="302" y="236" font-size="9.5" fill="#475569">store / dsaw：进 lsq / dsa_iss</text>
  <text x="302" y="252" font-size="9.5" fill="#475569">task_done：报 TS，取下一 task</text>
  <polyline points="254,122 254,205 294,205" fill="none" stroke="#475569" marker-end="url(#e0)"/>
  <rect x="144" y="150" width="120" height="52" fill="#f8fafc" stroke="#374151"/>
  <rect x="148" y="154" width="112" height="44" fill="none" stroke="#374151"/>
  <text x="152" y="172" font-size="9" fill="#111827">itcm · SRAM 4 KB · 1R</text>
  <text x="152" y="186" font-size="9" fill="#111827">gpr · FF 32×32 b · 2R1W</text>
  <line x1="264" y1="176" x2="294" y2="176" stroke="#475569" marker-end="url(#e0)"/>
  <rect x="144" y="220" width="120" height="40" fill="#f8fafc" stroke="#374151"/>
  <rect x="148" y="224" width="112" height="32" fill="none" stroke="#374151"/>
  <text x="152" y="244" font-size="9" fill="#111827">gpr_ready · FF 32×32 b</text>
  <line x1="264" y1="240" x2="294" y2="240" stroke="#475569" stroke-dasharray="3 2" marker-end="url(#e0)"/>

  <rect x="540" y="130" width="130" height="44" fill="#f8fafc" stroke="#374151"/>
  <rect x="544" y="134" width="122" height="36" fill="none" stroke="#374151"/>
  <g stroke="#374151"><line x1="556" y1="134" x2="556" y2="170"/></g>
  <text x="562" y="150" font-size="9" fill="#111827">dsa_rq · FIFO 8</text>
  <text x="562" y="164" font-size="8.5" fill="#475569">在途 DSA 读</text>
  <rect x="540" y="184" width="130" height="44" fill="#f8fafc" stroke="#374151"/>
  <rect x="544" y="188" width="122" height="36" fill="none" stroke="#374151"/>
  <g stroke="#374151"><line x1="556" y1="188" x2="556" y2="224"/></g>
  <text x="562" y="204" font-size="9" fill="#111827">sm_lsq · FIFO 16</text>
  <text x="562" y="218" font-size="8.5" fill="#475569">每拍发 1 请求</text>
  <rect x="540" y="238" width="130" height="44" fill="#f8fafc" stroke="#374151"/>
  <rect x="544" y="242" width="122" height="36" fill="none" stroke="#374151"/>
  <g stroke="#374151"><line x1="556" y1="242" x2="556" y2="278"/></g>
  <text x="562" y="258" font-size="9" fill="#111827">cm_lsq · FIFO 16</text>
  <text x="562" y="272" font-size="8.5" fill="#475569">DTE core · io_reg 复用</text>
  <line x1="516" y1="152" x2="538" y2="152" stroke="#475569" marker-end="url(#e0)"/>
  <line x1="516" y1="206" x2="538" y2="206" stroke="#475569" marker-end="url(#e0)"/>
  <line x1="516" y1="260" x2="538" y2="260" stroke="#475569" marker-end="url(#e0)"/>

  <rect x="700" y="130" width="140" height="44" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="704" y="126" font-size="8.5" fill="#6b7280">I1</text><text x="838" y="126" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="706" y="150" font-size="10.5" fill="#7c2d12">dsa_iss 每拍 1 条</text>
  <text x="706" y="166" font-size="9.5" fill="#92400e">反压则等；读返回按 rq 写回</text>
  <rect x="700" y="184" width="140" height="44" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="704" y="180" font-size="8.5" fill="#6b7280">L1</text><text x="838" y="180" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="706" y="204" font-size="10.5" fill="#7c2d12">lsq 顺序发射</text>
  <text x="706" y="220" font-size="9.5" fill="#92400e">SM 5～10 · CM 15～25</text>
  <line x1="670" y1="152" x2="698" y2="152" stroke="#475569" marker-end="url(#e0)"/>
  <line x1="670" y1="206" x2="698" y2="206" stroke="#475569" marker-end="url(#e0)"/>
  <line x1="670" y1="260" x2="698" y2="220" stroke="#475569" marker-end="url(#e0)"/>
  <polygon points="870,138 968,138 960,170 862,170" fill="#f8fafc" stroke="#374151"/>
  <text x="915" y="158" font-size="10" fill="#374151" text-anchor="middle">dsa_cfg / dsa_rsp</text>
  <polygon points="870,192 968,192 960,224 862,224" fill="#f8fafc" stroke="#374151"/>
  <text x="915" y="212" font-size="10" fill="#374151" text-anchor="middle">sm · cm · io_reg</text>
  <line x1="842" y1="152" x2="860" y2="152" stroke="#475569" marker-end="url(#e0)"/>
  <line x1="842" y1="206" x2="860" y2="206" stroke="#475569" marker-end="url(#e0)"/>
  <polyline points="915,226 915,300 204,300 204,262" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#e0)"/>
  <text x="560" y="296" font-size="9" fill="#6b7280">返回 → gpr_ready[rd] = 就绪拍（DTCM 当拍 + 3 直接记，不经端口）</text>

  <polygon points="24,340 122,340 114,372 16,372" fill="#f8fafc" stroke="#374151"/>
  <text x="69" y="360" font-size="10" fill="#374151" text-anchor="middle">task_done</text>
  <polyline points="300,262 300,356 124,356" fill="none" stroke="#475569" marker-end="url(#e0)"/>
  <text x="130" y="384" font-size="9" fill="#6b7280">task_done 指令带 TS 标志 → 脉冲；队空则回 S1 等 task</text>
</svg>
```

***

## 5　逐级行为

### S1 · Start

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `task_queue` 队首、`exec_state` | 1. `exec_state == WAIT_TASK ∧ task_queue 非空` → 出队<br>2. `pc = task_pc`；`csr_custom = {stream_id, task_id, user_id, stream_num}`（`stream_id` / `task_id` 只读位，硬件写）<br>3. `inst_cnt = 0`；`exec_state = RUN`<br>4. `task.cmd_ready = task_queue 未满`（task_queue 提前接收，无 bubble） | `pc`、`csr_custom`、`exec_state`、`task.cmd_ready` | D1 |

### E1 · 取指、译码、执行

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `itcm[pc]`、`gpr`、`gpr_ready`、`csr_custom`、`dtcm`、`sm_lsq` / `cm_lsq` / `dsa_rq` 余量、`dsa_cfg.req_ready` | 1. `exec_state != RUN` → 空拍<br>2. `inst = Decode(itcm[pc])`：opcode 为 custom-0 → `bach_insts`（dsar、dsari、dsaw.s、dsaw.d、dsawi.s、dsawi.d、task_done、flag_check、loop），否则 `src/rv32` 译码表<br>3. `∃ rs ∈ inst.src: gpr_ready[rs] > now` → 本拍不执行（BLOCKED，下拍重试）<br>4. `inst 为 store ∧ 目标 lsq 满`、`inst 为 dsaw ∧ !dsa_cfg.req_ready`、`inst 为 dsar ∧ dsa_rq 满` → 本拍不执行<br>5. `Run(inst)`：`SystemRv32` 同步执行；load 取当拍的值并 `gpr_ready[rd] = now + LAT(目标)`（DTCM +3、SM +`SM_LATENCY`、CM +`CM_RV_LATENCY`）；store 进对应 lsq；`dsaw` → `dsa_cfg.req = {we=1, addr, data, ids}`（`dsaw.d` / `dsawi.d` 拆成两拍两条）；`dsar` → `dsa_cfg.req = {we=0, addr, rq_idx}`，`dsa_rq.push(rd)`，`gpr_ready[rd] = ∞` 直到 `dsa_rsp`；`flag_check` → `sm_find.req`，`gpr_ready[rd] = ∞` 直到 `rsp`；`loop`：`rs2 ≥ rs1` 退出否则按 imm 跳，1 拍无冲刷<br>6. `task_done`：`ts` 标志 → `task_done` 脉冲 `{stream_id, task_id, user_id}`；`exec_state = WAIT_TASK`（S1 下拍取队头；队空则等）<br>7. `pc += 宽度` 或分支目标；`inst_cnt.retired += 1`；`inst_cnt.cycles` 每拍 +1 | `pc`、`gpr`、`gpr_ready`、`dtcm`、`sm_lsq`、`cm_lsq`、`dsa_rq`、`dsa_cfg`、`sm_find`、`task_done`、`exec_state` | D1 |

### I1 · dsa_iss 与读返回

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| E1 的 `dsa_cfg.req`、`dsa_rsp`、`dsa_rq` | 1. 每拍最多下发 1 条；`req_ready` 为上拍值，反压时 E1 第 4 条挡住下一条<br>2. `dsa_rsp.valid` → `rd = dsa_rq[rq_idx]`，`gpr[rd] = data`，`gpr_ready[rd] = now`，`dsa_rq.pop()`（按序写回） | `dsa_cfg`、`gpr`、`gpr_ready`、`dsa_rq` | D变长（DSA 定返回拍） |

### L1 · lsq 顺序发射

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `sm_lsq`、`cm_lsq`、`sm.req_ready`、`cm.req_ready`、`sm.rsp_*`、`cm.rsp_*` | 1. 每个 lsq 每拍从队头发 1 个请求（`req_ready` 上拍值为 1 时）<br>2. store：`rsp_valid` 到 → 出队<br>3. load 的值已在 E1 第 5 条取得，`rsp_valid` 只用于释放 lsq 项；`gpr_ready` 由 E1 记的就绪拍决定<br>4. `cm_lsq` 项 `is_io` → 走 `io_reg` 口（读包头 / 写 1 弹出） | `sm`、`cm`、`io_reg`、`sm_lsq`、`cm_lsq` | D变长 |

### G1 · cfg

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `cfg` | 1. 地址落 ITCM / DTCM 区 → 写入（镜像装载，4 B/T）<br>2. 写 `boot_pc`；解复位后 `pc = boot_pc`，firmware 跑到末尾的不通知 TS 的 `task_done` → `WAIT_TASK`，`ready = 1`<br>3. 读自定义 CSR → `cfg_rdata` 下一拍 | `itcm`、`dtcm`、`pc`、`ready`、`cfg_rdata` | D1 |

***

## 6　参数汇总

```
TASK_QUEUE_DEPTH    2                 // 待定，原文未给
DSA_RQ_DEPTH        8
SM_LSQ_DEPTH        16
CM_LSQ_DEPTH        16
DTCM_LATENCY        3
SM_LATENCY          5～10（取 5，待定）
CM_RV_LATENCY       15～25（取 15，待定）
ITCM_BYTES          4 KB；DTCM_BYTES 8 KB（镜像硬上限，超出编译失败）
每条指令 1 拍（双发射、乘法 3 拍、除法多拍、分支预测与冲刷、DTCM bank 冲突不体现）
指令预算（第 6 章 DTE 33 T、MU 100 T、VU 66 T）是对 inst_cnt 的断言，不是输入
自定义指令字段布局：funct3 按第 3 章表；rd / rs1 / rs2 / imm 按 R 型与 I 型标准布局   // 待定，等 ISA 描述表
kernel：C 源码，riscv64-unknown-elf-gcc -march=rv32imc -mabi=ilp32，每类 core 一个 ELF；task_pc = kernel 函数地址，编译侧从符号表导出
kernel_api.h：kernel 源码侧头文件（自定义指令 inline asm、DSA 寄存器地址、自定义 CSR 编号），模型侧不包含
```

***

## 7　机制覆盖

| 机制 | 落点 | 用例 |
| - | - | - |
| RV32IMC 全部指令、M 态 CSR、`fence` 为 nop | `src/rv32`（`inst_define/`） | `Rv32UnittestsGen` |
| custom-0 自定义指令按第 3 章表实现 | E1 第 2 条 + `bach_insts.h` | `rv_custom_insts` |
| task_queue 提前接收，无 bubble | S1 第 1、4 条 | `rv_task_queue` |
| task_ack 握手：有空槽才 ack | `task.cmd_ready` | `ts_arb_backpressure` |
| task 开始写 CSR：`task_pc` 设 PC，`stream_id` / `task_id` 写只读 CSR | S1 第 2 条 | `rv_start_csr` |
| 完成上报：`task_done` 带 TS 标志则上报三个 ID | E1 第 6 条 | `rv_task_done` |
| task_done 后队空则阻塞等待；firmware 末尾不通知 TS 的 task_done | E1 第 6 条 + S1 + G1 第 2 条 | `rv_task_done_wait` |
| dsa_iss 每拍一条；配置按反压判成功；读不阻塞，`dsa_rq` 8 项按序写回 | I1 + E1 第 4、5 条 | `rv_dsa_iss` |
| DSA 指令附带 stream_id / user_id / task_id | E1 第 5 条从 `csr_custom` 取 | `rv_dsa_ids` |
| `dsaw.d` 当两条 `dsaw.s` | E1 第 5 条 | `rv_dsaw_d` |
| lsq 顺序执行，每拍 1 请求，等返回释放 | L1 | `rv_lsq` |
| 访存延迟 ITCM 1 / DTCM 3 / SM 5～10 / CM 15～25 | ITCM 折进每条 1 拍；其余 E1 第 5 条的就绪拍 | `rv_latency` |
| Core Mem 读固定 1056 bit，不 burst，32 bit / 拍返回 | 存储文档 `cmem_rv` 口 + L1 | `rv_cm_read` |
| 静态 / 动态配置寄存器：静态初始化配，动态随任务 | kernel 按第 4 章的寄存器划分写 | `rv_static_dynamic` |
| trigger / last：trigger 启动；last 标志包含在 trigger 寄存器里，DSA 完成后通知 TS | DSA 侧 | `dsa_trigger_last` |
| DSA 读需轮询 | `dsar` 语义 + `gpr_ready` | `rv_dsa_poll` |
| `loop` 退出 100% 预测、`flag_check` | E1 第 5 条 | `rv_flag_check` |
| 单用户各 DSA 的调度程序 ≤ 200 cycle；DTE 约 33 T、MU 约 100 T、VU 约 66 T | `inst_cnt` + 断言 | `rv_budget` |
| MU core 100 T 内三件事 | `fc_gemv.c` | `kernel_mu_fc` |
| 两处阻塞：RV core 忙则 task 等；DSA 指令 buffer 满反压 RV core | S1 第 4 条 / E1 第 4 条 | `rv_two_blocks` |
| 标量 task 不调 DSA，RV core 自报完成 | `check_flag.c` 等 | `bcore_two_chains` |
| 异常期不受调度、不发 DSA | 不建（本轮） | — |

**kernel 清单**（`rv_core/kernel/`）

| kernel | core | 内容 |
| - | - | - |
| `weights_loader` | 全部 | 算这一片落 Matrix Mem 的地址，配 DTE 做 router → MM |
| `token_datain` | 计算 core | 读包头判链中哪一步，配 DTE 做 router → CM（含 scale、包头、shareMem 写），异步 datain 时写 `task_id` CSR |
| `fc_gemv` | 计算 core（MU） | 判激活专家落组、挑权重、配 MU 原语与 topK 表 |
| `silu_dot_quant`、`situ_glu_quant`、`core_reduce` | 计算 core（VU） | 配 VU 宏指令（静态组选择 + 动态参数 + trigger） |
| `dataout`、`reissue_out` | 计算 core（DTE） | 配 DTE 做 CM → router，改硬件包头 path_id |
| `bcore_datain`、`check_flag`、`broadcast` | B core | 第 6 章 B core 两条链 |
| `rcore_datain`、`rcore_arrive_inc`、`rcore_scan`、`rcore_mm2cm`、`rcore_sum`、`rcore_out` | R core | 第 6 章 R core 两条链 |

***

## 8　取舍

* **load 与 `dsar` 为什么取发射拍的值、延迟只落在 gpr 就绪表上**
  * 这是“功能模型一条指令一次跑完”这个前提下最贴硬件的做法
  * 硬件里 `dsa_rq` 也是按目的寄存器编号写回，就绪表就是它的等价物
  * 代价是发射拍到就绪拍之间存储内容的变化不体现
* **自定义指令为什么走 `Decode` 覆盖，不改 `src/rv32` 的译码表**
  * 生成器不在本仓，译码表按源码维护
