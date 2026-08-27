# 存储子系统

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **Core Mem / Matrix Mem / Share Mem**

给实现 Core 内三块存储的人：Core Mem、Matrix Mem、Share Mem 三个独立打拍的模块各自的端口、存储器、流水线与逐级行为、参数与机制。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《执行单元与存储》“存储子系统”：Core Mem、Matrix Mem、Share Mem
* 《Core 内硬件》：“Core Mem 的硬件多用户管理”“core 内部通路带宽”

***

## 1　定位与边界

三块存储各是一个模块，边界是各自的 master 端口组：

| 存储 | 接哪些 master |
| - | - |
| Core Mem | DTE、MU、VU 三个 DSA，加 DTE RV core、ctrl_noc |
| Matrix Mem | DTE、MU、ctrl_noc；MU 只读 |
| Share Mem | 三个 RV core 与 DTE |

内部结构与两条约定：

* 每个模块内部是 bank 阵列，加每 bank 一个仲裁器
* 请求在模块内按固定拍数走完，返回数据经出口端口交还 master
* stream_id 分片（`base(stream_id) = 分片大小 × stream_id`）在 master 侧的地址计算里做，存储模块只看物理地址

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1100 520" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs>
    <marker id="m0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker>
    <marker id="m0s" markerWidth="9" markerHeight="9" refX="1" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#475569"/></marker>
  </defs>
  <rect x="0" y="0" width="1100" height="520" fill="#ffffff"/>
  <text x="20" y="30" font-size="12" fill="#111827">存储子系统 · 第 0 层</text>

  <!-- masters -->
  <rect x="40" y="70" width="150" height="70" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="52" y="94" font-size="12" fill="#111827">DTE DSA</text>
  <text x="52" y="114" font-size="10" fill="#475569">Lane WR0 / RD1 / WR1</text>
  <rect x="40" y="170" width="150" height="70" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="52" y="194" font-size="12" fill="#111827">MU DSA</text>
  <text x="52" y="214" font-size="10" fill="#475569">Token ldq · stq · Weight ldq</text>
  <rect x="40" y="270" width="150" height="70" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="52" y="294" font-size="12" fill="#111827">VU DSA</text>
  <text x="52" y="314" font-size="10" fill="#475569">LU · SU</text>
  <rect x="40" y="370" width="150" height="70" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="52" y="394" font-size="12" fill="#111827">RV core ×3</text>
  <text x="52" y="414" font-size="10" fill="#475569">sm_lsq ×3 · cm_lsq（DTE core）</text>
  <polygon points="44,468 134,468 126,506 36,506" fill="#f8fafc" stroke="#374151"/>
  <text x="85" y="491" font-size="10.5" fill="#374151" text-anchor="middle">cfg</text>
  <text x="150" y="491" font-size="9" fill="#6b7280">ctrl_noc 4 B/T</text>

  <!-- Core Mem -->
  <rect x="380" y="60" width="300" height="150" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="384" y="64" width="292" height="142" fill="none" stroke="#374151"/>
  <text x="396" y="86" font-size="12" fill="#111827">Core Mem</text>
  <text x="396" y="106" font-size="10" fill="#475569">cmem_bank ×8 · SRAM 1024×128 B · 1RW</text>
  <text x="396" y="122" font-size="10" fill="#475569">cmem_scale ×8 · FF 1024×4 B · 1RW</text>
  <text x="396" y="138" font-size="10" fill="#475569">bank_arb ×8 · 优先级 MU &gt; VU = DTE = RV</text>
  <text x="396" y="154" font-size="10" fill="#475569">rsp_q ×7 · 延迟线</text>
  <text x="396" y="190" font-size="9.5" fill="#9ca3af">(1 KB + 32 B)/T</text>

  <!-- Matrix Mem -->
  <rect x="380" y="240" width="300" height="130" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="384" y="244" width="292" height="122" fill="none" stroke="#374151"/>
  <text x="396" y="266" font-size="12" fill="#111827">Matrix Mem</text>
  <text x="396" y="286" font-size="10" fill="#475569">mmem_bank ×32 · SRAM 4096×256 B · 1RW</text>
  <text x="396" y="302" font-size="10" fill="#475569">mmem_scale ×32 · SRAM 512×256 B · 1RW</text>
  <text x="396" y="318" font-size="10" fill="#475569">bank_arb ×32 · 同 bank 只执行 MU</text>
  <text x="396" y="354" font-size="9.5" fill="#9ca3af">(8 KB + 1 KB)/T</text>

  <!-- Share Mem -->
  <rect x="380" y="400" width="300" height="100" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="384" y="404" width="292" height="92" fill="none" stroke="#374151"/>
  <text x="396" y="426" font-size="12" fill="#111827">Share Mem</text>
  <text x="396" y="446" font-size="10" fill="#475569">smem · SRAM 8192×4 B · 1RW</text>
  <text x="396" y="462" font-size="10" fill="#475569">smem_arb · 4 请求者轮询</text>

  <!-- edges -->
  <line x1="192" y1="90" x2="378" y2="90" stroke="#475569" marker-start="url(#m0s)" marker-end="url(#m0)"/>
  <text x="285" y="84" font-size="9" fill="#6b7280" text-anchor="middle">cmem_dte_rd · cmem_dte_wr</text>
  <polyline points="192,120 300,120 300,290 378,290" fill="none" stroke="#475569" marker-start="url(#m0s)" marker-end="url(#m0)"/>
  <text x="304" y="200" font-size="9" fill="#6b7280">mmem_dte_rd · mmem_dte_wr</text>
  <line x1="192" y1="190" x2="378" y2="130" stroke="#475569" marker-start="url(#m0s)" marker-end="url(#m0)"/>
  <text x="300" y="150" font-size="9" fill="#6b7280" text-anchor="middle">cmem_mu_rd · cmem_mu_wr</text>
  <line x1="378" y1="320" x2="192" y2="215" stroke="#475569" marker-end="url(#m0)"/>
  <text x="285" y="280" font-size="9" fill="#6b7280" text-anchor="middle">mmem_mu_rd</text>
  <line x1="192" y1="300" x2="378" y2="160" stroke="#475569" marker-start="url(#m0s)" marker-end="url(#m0)"/>
  <text x="250" y="240" font-size="9" fill="#6b7280">cmem_vu_ld · cmem_vu_st</text>
  <polyline points="192,395 250,395 250,180 378,180" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-start="url(#m0s)" marker-end="url(#m0)"/>
  <text x="256" y="176" font-size="9" fill="#6b7280">cmem_rv</text>
  <line x1="192" y1="430" x2="378" y2="430" stroke="#475569" marker-start="url(#m0s)" marker-end="url(#m0)"/>
  <text x="285" y="424" font-size="9" fill="#6b7280" text-anchor="middle">smem_rv ×3 · smem_find</text>
  <polyline points="192,135 230,135 230,470 378,470" fill="none" stroke="#475569" marker-end="url(#m0)"/>
  <text x="300" y="488" font-size="9" fill="#6b7280" text-anchor="middle">smem_dte_wr</text>
  <polyline points="136,487 350,487 350,200 378,200" fill="none" stroke="#475569" stroke-dasharray="2 3" marker-end="url(#m0)"/>
  <polyline points="350,340 378,340" fill="none" stroke="#475569" stroke-dasharray="2 3" marker-end="url(#m0)"/>

  <text x="720" y="90" font-size="11" fill="#374151">每个箭头是一个端口组，声明在“接口”章；</text>
  <text x="720" y="108" font-size="11" fill="#374151">Core Mem 六个 DSA 口加 RV 口与 cfg 口，Matrix Mem 三个口，Share Mem 五个口。</text>
  <text x="720" y="126" font-size="11" fill="#374151">MU 不能读 Matrix Mem 以外的权重来源；VU 不接 Matrix Mem。</text>
</svg>
```

***

## 2　接口

信号名即模型里端口束的字段名。所有 valid/ready 组的 `ready` 都是上一拍锁存的值（《latch 建模计划》“跨模块按硬件的握手协议与打拍规则”）。

### Core Mem

```
port cmem_dte_rd (slave, valid/ready, clk)        // DTE Lane RD1 读 Core Mem，一拍 256 B
  in  req_valid
  out req_ready                                     // = req_slot[dte_rd] 空
  in  req_addr[19:0]                                // 字节地址，256 B 对齐
  in  req_tag[7:0]                                  // DTE 的 outstanding 编号，随响应原样返回
  out rsp_valid                                     // 请求进模块后第 CMEM_LAT_DTE 拍拉高一拍
  out rsp_data[2047:0]                              // 256 B
  out rsp_tag[7:0]
port cmem_dte_wr (slave, valid/ready, clk)        // DTE Lane WR0 / WR1 写 Core Mem，一拍 256 B
  in  req_valid
  out req_ready                                     // = req_slot[dte_wr] 空
  in  req_addr[19:0]
  in  req_data[2047:0]
  in  req_mask[7:0]                                 // 32 B 粒度，位为 0 该 32 B 不写
  out rsp_valid                                     // 写完成（bvalid），第 CMEM_LAT_DTE 拍
port cmem_mu_rd (slave, valid/ready, clk)         // MU Token ldq 读，一拍 128 B + 4 B scale
  in  req_valid
  out req_ready                                     // = req_slot[mu_rd] 空
  in  req_addr[19:0]                                // 128 B 对齐
  in  req_scale_en                                  // 1 = 同时读 cmem_scale
  in  req_tag[3:0]
  out rsp_valid                                     // 第 CMEM_LAT_MU 拍
  out rsp_data[1023:0]
  out rsp_scale[31:0]
  out rsp_tag[3:0]
port cmem_mu_wr (slave, valid/ready, clk)         // MU stq 写，一拍 128 B + 4 B scale
  in  req_valid
  out req_ready                                     // = req_slot[mu_wr] 空
  in  req_addr[19:0]
  in  req_data[1023:0]
  in  req_scale[31:0]
  in  req_scale_en
  in  req_byte_mask[127:0]                          // 非全 1 时该行留存 mask 记录
  out rsp_valid                                     // 第 CMEM_LAT_MU 拍
port cmem_vu_ld (slave, valid/ready, clk)         // VU LU 读，一次 1056 bit，不 burst
  in  req_valid
  out req_ready                                     // = req_slot[vu_ld] 空
  in  req_addr[19:0]                                // 128 B 对齐
  in  req_scale_en
  out rsp_valid                                     // 第 CMEM_LAT_VU 拍
  out rsp_data[1023:0]
  out rsp_scale[31:0]
port cmem_vu_st (slave, valid/ready, clk)         // VU SU 写，一次 1056 bit
  in  req_valid
  out req_ready                                     // = req_slot[vu_st] 空
  in  req_addr[19:0]
  in  req_data[1023:0]
  in  req_scale[31:0]
  in  req_scale_en
  in  req_byte_mask[127:0]
  out rsp_valid                                     // 第 CMEM_LAT_VU 拍
port cmem_rv (slave, valid/ready, clk)            // DTE RV core 的 cm_lsq，4 B 粒度
  in  req_valid
  out req_ready                                     // = req_slot[rv] 空
  in  req_addr[19:0]
  in  req_we                                        // 1 = 写
  in  req_wdata[31:0]
  in  req_be[3:0]
  out rsp_valid                                     // 第 CMEM_LAT_RV 拍
  out rsp_data[1023:0]                              // 读固定回整行 128 B，RV 侧按 32 bit / 拍取用
port cmem_cfg (slave, ctrl_noc 写事务, clk)       // ctrl_noc 后门读写，4 B/T
  in  cfg_valid
  in  cfg_addr[19:0]
  in  cfg_we
  in  cfg_wdata[31:0]
  out cfg_rdata[31:0]                               // 下一拍有效
```

### Matrix Mem

```
port mmem_dte_rd (slave, valid/ready, clk)        // DTE Lane RD1 读，一拍 256 B = 一个 bank 行
  in  req_valid
  out req_ready                                     // = req_slot[dte_rd] 空
  in  req_addr[25:0]                                // 字节地址，256 B 对齐，不支持 byte mask
  in  req_tag[7:0]
  out rsp_valid                                     // 第 MMEM_LAT_DTE_RD 拍
  out rsp_data[2047:0]
  out rsp_tag[7:0]
port mmem_dte_wr (slave, valid/ready, clk)        // DTE Lane WR0 写，一拍 256 B
  in  req_valid
  out req_ready
  in  req_addr[25:0]
  in  req_data[2047:0]
  out rsp_valid                                     // 第 MMEM_LAT_DTE_WR 拍
port mmem_mu_rd (slave, valid/ready, clk)         // MU Weight ldq 读，一个地址广播到 32 bank，每 bank 回 256 B
  in  req_valid
  out req_ready                                     // = req_slot[mu_rd] 空
  in  req_row[11:0]                                 // bank 内行号，32 个 bank 同行
  in  req_scale_en                                  // 1 = 同时读 mmem_scale 的对应行
  out rsp_valid                                     // 第 MMEM_LAT_MU 拍
  out rsp_data[31][2047:0]                          // 32 lane 各 256 B，共 8 KB
  out rsp_scale[31][255:0]                          // 每 lane 32 B，共 1 KB
port mmem_cfg (slave, ctrl_noc 写事务, clk)       // ctrl_noc 读写，4 B/T，地址 128 B 对齐，burst ≤ 32
  in  cfg_valid
  in  cfg_addr[25:0]
  in  cfg_we
  in  cfg_wdata[31:0]
  out cfg_rdata[31:0]
```

### Share Mem

```
port smem_rv[3] (slave, valid/ready, clk)         // 三个 RV core 的 sm_lsq，各一组，4 B 粒度
  in  req_valid
  out req_ready                                     // = req_slot[rv_i] 空
  in  req_addr[14:0]
  in  req_we
  in  req_wdata[31:0]
  in  req_be[3:0]
  out rsp_valid                                     // 第 SM_LATENCY 拍
  out rsp_rdata[31:0]
port smem_dte_wr (slave, valid/ready, clk)        // DTE 搬运完成后写 valid 标志
  in  req_valid
  out req_ready
  in  req_addr[14:0]
  in  req_wdata[31:0]
port smem_find (slave, valid/ready, clk)          // RV core 的 flag_check：从 begin 起找第一个非零字
  in  req_valid
  out req_ready                                     // = 上一笔 find 已回
  in  req_begin[14:0]
  in  req_end[14:0]
  out rsp_valid                                     // 第 SM_LATENCY 拍
  out rsp_offset[31:0]                              // 找不到为全 1
```

***

## 3　存储器

```
mem cmem_bank[8]   SRAM        1024×128 B                 1RW   byte_mask 写，非全 1 留存 mask 记录   复位 0x55 填充   // 数据 bank，行 128 B；bank = addr[9:7]
mem cmem_scale[8]  FF 阵列     1024×4 B                   1RW   scale_en 时整字写                    复位未定义       // 与数据行一一映射的 scale
mem cmem_mask[8]   FF 阵列     1024×1 b                   1RW   随数据写                             复位 0           // 该行是否存在过非全 1 的 byte_mask（读时跳过 ECC 检查）
mem cmem_req_slot[7] 1-deep 寄存器 {port, addr[19:0], we, data[2047:0], mask[127:0], scale[31:0], scale_en, tag[7:0]}  1W1R  满 → 该口 req_ready=0  复位空  // M1 → M2，每个 master 口一个
mem cmem_grant[8]  级间 latch   {port[2:0], slot 内容}     —     每拍覆写                             —               // M2 → M3，每 bank 一个
mem cmem_rdout[8]  级间 latch   {port[2:0], data[1023:0], scale[31:0], tag[7:0]}  —  每拍覆写         —               // M3 → M4
mem cmem_rsp_q[7]  FIFO        深 CMEM_LAT_MAX × {data[2047:0], scale[31:0], tag[7:0], due[31:0]}  1W1R  不会满（每口每拍最多进一项、出一项）  复位空  // M4 延迟线，每个 master 口一个
mem cmem_cfg_rd    1-deep 寄存器 {rdata[31:0]}             1W1R  每拍覆写                             —               // cfg 读返回

mem mmem_bank[32]  SRAM        4096×256 B                 1RW   整行写，无 byte mask                复位 0x55 填充   // 每 bank 1 MB 数据，与 MU lane 一对一
mem mmem_scale[32] SRAM        512×256 B                  1RW   整行写                               复位 0x55 填充   // 每 bank 128 KB scale（scale 模式）
mem mmem_req_slot[3] 1-deep 寄存器 {port, addr[25:0] 或 row[11:0], we, data[2047:0], scale_en, tag[7:0]}  1W1R  满 → req_ready=0  复位空  // M1 → M2
mem mmem_grant[32] 级间 latch   {port[1:0], row[11:0], we, data[2047:0]}  —  每拍覆写                —               // M2 → M3
mem mmem_rdout[32] 级间 latch   {port[1:0], data[2047:0], scale[255:0]}   —  每拍覆写                —               // M3 → M4
mem mmem_rsp_q[3]  FIFO        深 MMEM_LAT_MAX × {data, scale, tag, due}  1W1R  不会满               复位空          // M4 延迟线
mem mmem_conflict_cnt FF        32 b                       1RW   同 bank 双 master 时 +1              复位 0           // 报错计数，cfg 可读

mem smem           SRAM        8192×4 B                   1RW   be[3:0] 字节写                       复位未定义       // 32 KB，不需要初始化
mem smem_req_slot[5] 1-deep 寄存器 {port, addr[14:0], we, wdata[31:0], be[3:0], begin[14:0], end[14:0]}  1W1R  满 → req_ready=0  复位空  // M1 → M2，rv ×3、dte_wr、find
mem smem_grant     级间 latch   {port[2:0], addr[14:0], we, wdata[31:0], be[3:0]}  —  每拍覆写         —               // M2 → M3
mem smem_rdout     级间 latch   {port[2:0], rdata[31:0]}   —     每拍覆写                             —               // M3 → M4
mem smem_rsp_q[4]  FIFO        深 SM_LATENCY × {rdata[31:0], due[31:0]}  1W1R  不会满                复位空          // M4 延迟线，rv ×3 与 find
mem smem_find_ctx  1-deep 寄存器 {cur[14:0], end[14:0], base[14:0]}  1W1R  find 在飞时占用            复位空          // flag_check 的逐字扫描游标
```

***

## 4　流水线总览

三个模块同一条四级骨架：

* M1 请求锁存 → M2 bank 仲裁 → M3 SRAM 读写 → M4 延迟线与响应
* 每个 master 口一个请求槽与一条延迟线，bank 仲裁按 bank 独立

下图是 Core Mem。Matrix Mem 与 Share Mem 的差别只在口数、bank 数、优先级规则与拍数，列在图后的表里，不另画。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1180 470" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs>
    <marker id="p0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker>
  </defs>
  <rect x="0" y="0" width="1180" height="470" fill="#ffffff"/>
  <text x="20" y="26" font-size="12" fill="#111827">Core Mem · 第 1 层（DTE 口按比例 13 拍；MU 11、VU 14、RV 15～25 为 M4 延迟线长度不同）</text>

  <!-- t 标尺：格宽 76，t0 在 x=130 -->
  <g stroke="#e5e7eb">
    <line x1="130" y1="40" x2="130" y2="430"/><line x1="206" y1="40" x2="206" y2="430"/><line x1="282" y1="40" x2="282" y2="430"/><line x1="358" y1="40" x2="358" y2="430"/><line x1="434" y1="40" x2="434" y2="430"/><line x1="510" y1="40" x2="510" y2="430"/><line x1="586" y1="40" x2="586" y2="430"/><line x1="662" y1="40" x2="662" y2="430"/><line x1="738" y1="40" x2="738" y2="430"/><line x1="814" y1="40" x2="814" y2="430"/><line x1="890" y1="40" x2="890" y2="430"/><line x1="966" y1="40" x2="966" y2="430"/><line x1="1042" y1="40" x2="1042" y2="430"/><line x1="1118" y1="40" x2="1118" y2="430"/>
  </g>
  <g font-size="8.5" fill="#6b7280">
    <text x="130" y="50">t0</text><text x="206" y="50">t1</text><text x="282" y="50">t2</text><text x="358" y="50">t3</text><text x="434" y="50">t4</text><text x="510" y="50">t5</text><text x="586" y="50">t6</text><text x="662" y="50">t7</text><text x="738" y="50">t8</text><text x="814" y="50">t9</text><text x="890" y="50">t10</text><text x="966" y="50">t11</text><text x="1042" y="50">t12</text><text x="1118" y="50">t13</text>
  </g>

  <!-- 端口（左） -->
  <polygon points="20,88 118,88 110,120 12,120" fill="#f8fafc" stroke="#374151"/>
  <text x="65" y="108" font-size="10" fill="#374151" text-anchor="middle">cmem_dte_rd/wr</text>
  <polygon points="20,140 118,140 110,172 12,172" fill="#f8fafc" stroke="#374151"/>
  <text x="65" y="160" font-size="10" fill="#374151" text-anchor="middle">cmem_mu_rd/wr</text>
  <polygon points="20,192 118,192 110,224 12,224" fill="#f8fafc" stroke="#374151"/>
  <text x="65" y="212" font-size="10" fill="#374151" text-anchor="middle">cmem_vu_ld/st</text>
  <polygon points="20,244 118,244 110,276 12,276" fill="#f8fafc" stroke="#374151"/>
  <text x="65" y="264" font-size="10" fill="#374151" text-anchor="middle">cmem_rv</text>
  <polygon points="20,296 118,296 110,328 12,328" fill="#f8fafc" stroke="#374151"/>
  <text x="65" y="316" font-size="10" fill="#374151" text-anchor="middle">cmem_cfg</text>

  <!-- M1 请求槽 -->
  <rect x="134" y="70" width="68" height="260" fill="#f1f5f9" stroke="#334155"/>
  <rect x="134" y="70" width="68" height="16" fill="#334155"/>
  <text x="168" y="82" font-size="10" fill="#ffffff" text-anchor="middle">req_slot[7]</text>
  <text x="139" y="66" font-size="8.5" fill="#6b7280">M1</text>
  <text x="200" y="66" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="140" y="104" font-size="9.5" fill="#475569">addr[19:0]</text>
  <text x="140" y="118" font-size="9.5" fill="#475569">we · mask</text>
  <text x="140" y="132" font-size="9.5" fill="#475569">data[2047:0]</text>
  <text x="140" y="146" font-size="9.5" fill="#475569">scale · en</text>
  <text x="140" y="160" font-size="9.5" fill="#475569">tag[7:0]</text>
  <text x="140" y="300" font-size="9" fill="#9ca3af">每口一槽</text>
  <line x1="120" y1="104" x2="132" y2="104" stroke="#475569" marker-end="url(#p0)"/>
  <line x1="120" y1="156" x2="132" y2="156" stroke="#475569" marker-end="url(#p0)"/>
  <line x1="120" y1="208" x2="132" y2="208" stroke="#475569" marker-end="url(#p0)"/>
  <line x1="120" y1="260" x2="132" y2="260" stroke="#475569" marker-end="url(#p0)"/>
  <line x1="120" y1="312" x2="132" y2="312" stroke="#475569" marker-end="url(#p0)"/>

  <!-- M2 bank 仲裁 -->
  <rect x="210" y="70" width="68" height="260" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="215" y="66" font-size="8.5" fill="#6b7280">M2</text>
  <text x="276" y="66" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="218" y="92" font-size="12" fill="#111827">bank_arb</text>
  <text x="218" y="108" font-size="9.5" fill="#475569">×8</text>
  <text x="218" y="300" font-size="9" fill="#9ca3af">MU &gt; VU = DTE = RV</text>
  <line x1="202" y1="200" x2="208" y2="200" stroke="#475569" marker-end="url(#p0)"/>

  <!-- grant latch -->
  <rect x="284" y="70" width="70" height="260" fill="#f1f5f9" stroke="#334155"/>
  <rect x="284" y="70" width="70" height="16" fill="#334155"/>
  <text x="319" y="82" font-size="10" fill="#ffffff" text-anchor="middle">grant[8]</text>
  <text x="290" y="104" font-size="9.5" fill="#475569">port[2:0]</text>
  <text x="290" y="118" font-size="9.5" fill="#475569">槽内容</text>
  <line x1="278" y1="200" x2="282" y2="200" stroke="#475569" marker-end="url(#p0)"/>

  <!-- M3 SRAM -->
  <rect x="362" y="70" width="144" height="260" fill="#f8fafc" stroke="#374151"/>
  <rect x="366" y="74" width="136" height="252" fill="none" stroke="#374151"/>
  <text x="367" y="66" font-size="8.5" fill="#6b7280">M3</text>
  <text x="504" y="66" font-size="8.5" fill="#6b7280" text-anchor="end">D2</text>
  <text x="374" y="96" font-size="11" fill="#111827">cmem_bank[8]</text>
  <text x="374" y="112" font-size="9.5" fill="#475569">SRAM 1024×128 B · 1RW</text>
  <text x="374" y="136" font-size="11" fill="#111827">cmem_scale[8]</text>
  <text x="374" y="152" font-size="9.5" fill="#475569">FF 1024×4 B · 1RW</text>
  <text x="374" y="176" font-size="11" fill="#111827">cmem_mask[8]</text>
  <text x="374" y="192" font-size="9.5" fill="#475569">FF 1024×1 b · 1RW</text>
  <text x="374" y="300" font-size="9" fill="#9ca3af">1W · wen=byte_mask</text>
  <line x1="354" y1="200" x2="360" y2="200" stroke="#475569" marker-end="url(#p0)"/>

  <!-- rdout latch -->
  <rect x="512" y="70" width="70" height="260" fill="#f1f5f9" stroke="#334155"/>
  <rect x="512" y="70" width="70" height="16" fill="#334155"/>
  <text x="547" y="82" font-size="10" fill="#ffffff" text-anchor="middle">rdout[8]</text>
  <text x="518" y="104" font-size="9.5" fill="#475569">port[2:0]</text>
  <text x="518" y="118" font-size="9.5" fill="#475569">data[1023:0]</text>
  <text x="518" y="132" font-size="9.5" fill="#475569">scale[31:0]</text>
  <text x="518" y="146" font-size="9.5" fill="#475569">tag[7:0]</text>
  <line x1="506" y1="200" x2="510" y2="200" stroke="#475569" marker-end="url(#p0)"/>

  <!-- M4 延迟线 -->
  <rect x="590" y="70" width="450" height="260" fill="#f8fafc" stroke="#374151"/>
  <rect x="594" y="74" width="442" height="252" fill="none" stroke="#374151"/>
  <g stroke="#374151"><line x1="640" y1="74" x2="640" y2="326"/><line x1="686" y1="74" x2="686" y2="326"/><line x1="732" y1="74" x2="732" y2="326"/></g>
  <text x="595" y="66" font-size="8.5" fill="#6b7280">M4</text>
  <text x="1038" y="66" font-size="8.5" fill="#6b7280" text-anchor="end">D变长（DTE 9）</text>
  <text x="760" y="96" font-size="11" fill="#111827">rsp_q[7]</text>
  <text x="760" y="112" font-size="9.5" fill="#475569">FIFO 深 CMEM_LAT_MAX · 1W1R</text>
  <text x="760" y="128" font-size="9.5" fill="#475569">{data, scale, tag, due}</text>
  <text x="760" y="300" font-size="9" fill="#9ca3af">due = 进模块拍 + CMEM_LAT_&lt;port&gt;</text>
  <line x1="582" y1="200" x2="588" y2="200" stroke="#475569" marker-end="url(#p0)"/>

  <!-- 端口（右） -->
  <polygon points="1058,180 1156,180 1148,212 1050,212" fill="#f8fafc" stroke="#374151"/>
  <text x="1103" y="200" font-size="10" fill="#374151" text-anchor="middle">rsp_*（各口）</text>
  <line x1="1040" y1="196" x2="1048" y2="196" stroke="#475569" marker-end="url(#p0)"/>

  <!-- 反压 -->
  <polyline points="168,340 168,380 65,380 65,332" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#p0)"/>
  <text x="180" y="384" font-size="9" fill="#6b7280">req_ready = 槽空（上拍锁存）</text>
  <polyline points="244,340 244,400 168,400" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#p0)"/>
  <text x="256" y="404" font-size="9" fill="#6b7280">未获授予的槽保持，下拍再仲裁</text>
  <text x="590" y="420" font-size="9" fill="#6b7280">写请求的 rsp 只有 valid（bvalid），M4 同样按 due 拍出队</text>
</svg>
```

反压只有一处：请求槽满则该口 `req_ready` 拉低；槽里未获授予的请求原地保持，下拍再参加仲裁。M4 出队按 `due` 拍，每口每拍最多一项，所以延迟线不会溢出。

| 模块 | 口数 | bank 数 | M2 规则 | M3 | M4 拍数（端到端 = 1 + 1 + 2 + M4） |
| - | - | - | - | - | - |
| Core Mem | 7 + cfg | 8 | 每 bank 独占；MU > VU = DTE = RV；DTE 的读写各自判 bank 冲突，再判读写之间；只反压冲突的那个口 | 1RW，byte_mask，scale 同拍 | DTE 9、MU 7、VU 10、RV `CM_RV_LATENCY` − 4 |
| Matrix Mem | 3 + cfg | 32 | 每 bank 独占；MU 请求一次占全部 32 bank；同 bank 双 master 时只执行 MU，另一方保持并 `mmem_conflict_cnt` +1 | 1RW，整行 256 B | DTE 写 5、DTE 读 4、MU 4 |
| Share Mem | 5 | 1 | 轮询（待定）；find 占用期间每拍读一字 | 1RW，4 B，be 字节写 | `SM_LATENCY` − 4 |

***

## 5　逐级行为

Core Mem 的四级；Matrix Mem 与 Share Mem 同一套级，差别在上表。

### M1 · 请求锁存

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 970 200" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="q1" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="970" height="200" fill="#ffffff"/>
  <polygon points="30,70 150,70 140,120 20,120" fill="#f8fafc" stroke="#374151"/>
  <text x="85" y="99" font-size="10.5" fill="#374151" text-anchor="middle">cmem_*（7 口）</text>
  <text x="85" y="150" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · req_addr · req_data …</text>
  <line x1="152" y1="95" x2="248" y2="95" stroke="#475569" marker-end="url(#q1)"/>
  <rect x="252" y="36" width="380" height="128" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="260" y="50" font-size="8.5" fill="#6b7280">M1</text>
  <text x="624" y="50" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="268" y="70" font-size="12" fill="#111827">请求锁存</text>
  <text x="268" y="90" font-size="10.5" fill="#475569">1. req_ready[p] = !req_slot[p].valid（上拍值）</text>
  <text x="268" y="106" font-size="10.5" fill="#475569">2. req_valid[p] &amp;&amp; req_ready[p] → req_slot[p] = {addr, we, data, mask, scale, scale_en, tag}</text>
  <text x="268" y="122" font-size="10.5" fill="#475569">3. req_slot[p].bank = addr[9:7]；DTE 的 256 B 请求占 bank 与 bank+1 两个</text>
  <text x="268" y="138" font-size="10.5" fill="#475569">4. req_slot[p].t_in = now</text>
  <line x1="634" y1="95" x2="740" y2="95" stroke="#475569" marker-end="url(#q1)"/>
  <rect x="744" y="60" width="200" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="744" y="60" width="200" height="16" fill="#334155"/>
  <text x="844" y="72" font-size="10.5" fill="#ffffff" text-anchor="middle">cmem_req_slot[7]</text>
  <text x="752" y="92" font-size="10" fill="#475569">valid · bank[2:0] · addr[19:0] · we</text>
  <text x="752" y="106" font-size="10" fill="#475569">data[2047:0] · mask[127:0] · scale[31:0]</text>
  <text x="752" y="120" font-size="10" fill="#475569">scale_en · tag[7:0] · t_in[31:0]</text>
</svg>
```

### M2 · bank 仲裁

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 970 220" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="q2" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="970" height="220" fill="#ffffff"/>
  <rect x="20" y="60" width="200" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="60" width="200" height="16" fill="#334155"/>
  <text x="120" y="72" font-size="10.5" fill="#ffffff" text-anchor="middle">cmem_req_slot[7]</text>
  <text x="28" y="92" font-size="10" fill="#475569">valid · bank[2:0] · we · port</text>
  <text x="28" y="106" font-size="10" fill="#475569">（其余字段随授予转发）</text>
  <line x1="222" y1="95" x2="248" y2="95" stroke="#475569" marker-end="url(#q2)"/>
  <rect x="252" y="30" width="400" height="160" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="260" y="44" font-size="8.5" fill="#6b7280">M2</text>
  <text x="644" y="44" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="268" y="64" font-size="12" fill="#111827">bank 仲裁（每 bank 独立）</text>
  <text x="268" y="84" font-size="10.5" fill="#475569">1. cand[b] = { p : slot[p].valid &amp;&amp; slot[p] 覆盖 bank b }</text>
  <text x="268" y="100" font-size="10.5" fill="#475569">2. win[b] = 按优先级 MU &gt; VU = DTE = RV 选一；同级按 slot 的 t_in 先到先得</text>
  <text x="268" y="116" font-size="10.5" fill="#475569">   2.1 DTE 读与 DTE 写先各自与其他口判冲突，再在两者之间判（同组同优先级）</text>
  <text x="268" y="132" font-size="10.5" fill="#475569">3. 占两个 bank 的 DTE 请求：两个 bank 都赢才授予，否则整体保持（多方向一次性扣减）</text>
  <text x="268" y="148" font-size="10.5" fill="#475569">4. 授予的 slot 清 valid；grant[b] = {port, slot 内容}；未授予的 slot 保持</text>
  <text x="268" y="176" font-size="9.5" fill="#9ca3af">cfg 口不经仲裁：空拍插入，同 bank 有授予时顺延一拍</text>
  <line x1="654" y1="95" x2="740" y2="95" stroke="#475569" marker-end="url(#q2)"/>
  <rect x="744" y="60" width="200" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="744" y="60" width="200" height="16" fill="#334155"/>
  <text x="844" y="72" font-size="10.5" fill="#ffffff" text-anchor="middle">cmem_grant[8]</text>
  <text x="752" y="92" font-size="10" fill="#475569">valid · port[2:0] · row[9:0] · we</text>
  <text x="752" y="106" font-size="10" fill="#475569">data[1023:0] · mask[127:0] · scale[31:0]</text>
  <text x="752" y="120" font-size="10" fill="#475569">scale_en · tag[7:0] · t_in[31:0]</text>
</svg>
```

### M3 · SRAM 读写

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 970 220" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="q3" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="970" height="220" fill="#ffffff"/>
  <rect x="20" y="30" width="200" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="30" width="200" height="16" fill="#334155"/>
  <text x="120" y="42" font-size="10.5" fill="#ffffff" text-anchor="middle">cmem_grant[8]</text>
  <text x="28" y="62" font-size="10" fill="#475569">port · row[9:0] · we · data · mask</text>
  <text x="28" y="76" font-size="10" fill="#475569">scale · scale_en · tag · t_in</text>
  <rect x="20" y="120" width="200" height="70" fill="#f8fafc" stroke="#374151"/>
  <rect x="24" y="124" width="192" height="62" fill="none" stroke="#374151"/>
  <text x="28" y="144" font-size="10.5" fill="#111827">cmem_bank[b] · SRAM 1024×128 B · 1RW</text>
  <text x="28" y="160" font-size="10" fill="#475569">cmem_scale[b] · FF 1024×4 B · 1RW</text>
  <text x="28" y="176" font-size="10" fill="#475569">cmem_mask[b] · FF 1024×1 b · 1RW</text>
  <line x1="222" y1="65" x2="248" y2="65" stroke="#475569" marker-end="url(#q3)"/>
  <line x1="222" y1="155" x2="248" y2="155" stroke="#475569" marker-end="url(#q3)"/>
  <rect x="252" y="30" width="400" height="160" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="260" y="44" font-size="8.5" fill="#6b7280">M3</text>
  <text x="644" y="44" font-size="8.5" fill="#6b7280" text-anchor="end">D2</text>
  <text x="268" y="64" font-size="12" fill="#111827">SRAM 读写（每 bank）</text>
  <text x="268" y="84" font-size="10.5" fill="#475569">1. we=0：rdout[b].data = bank[b][row]；scale_en → rdout[b].scale = scale[b][row]</text>
  <text x="268" y="100" font-size="10.5" fill="#475569">2. we=1：bank[b][row][i] = data[i] if mask[i]（逐字节）；scale_en → scale[b][row] = scale</text>
  <text x="268" y="116" font-size="10.5" fill="#475569">   2.1 mask 非全 1 → cmem_mask[b][row] = 1</text>
  <text x="268" y="132" font-size="10.5" fill="#475569">3. rdout[b] = {port, data, scale, tag, t_in}；写请求只带 {port, tag, t_in}</text>
  <text x="268" y="160" font-size="9.5" fill="#9ca3af">ECC 与 mask 记录只留位，不建校验；读出 2 拍</text>
  <line x1="654" y1="110" x2="740" y2="110" stroke="#475569" marker-end="url(#q3)"/>
  <rect x="744" y="75" width="200" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="744" y="75" width="200" height="16" fill="#334155"/>
  <text x="844" y="87" font-size="10.5" fill="#ffffff" text-anchor="middle">cmem_rdout[8]</text>
  <text x="752" y="107" font-size="10" fill="#475569">valid · port[2:0] · data[1023:0]</text>
  <text x="752" y="121" font-size="10" fill="#475569">scale[31:0] · tag[7:0] · t_in[31:0]</text>
</svg>
```

### M4 · 延迟线与响应

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 970 220" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="q4" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="970" height="220" fill="#ffffff"/>
  <rect x="20" y="30" width="200" height="60" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="30" width="200" height="16" fill="#334155"/>
  <text x="120" y="42" font-size="10.5" fill="#ffffff" text-anchor="middle">cmem_rdout[8]</text>
  <text x="28" y="62" font-size="10" fill="#475569">port · data · scale · tag · t_in</text>
  <rect x="20" y="120" width="200" height="70" fill="#f8fafc" stroke="#374151"/>
  <rect x="24" y="124" width="192" height="62" fill="none" stroke="#374151"/>
  <g stroke="#374151"><line x1="60" y1="124" x2="60" y2="186"/><line x1="90" y1="124" x2="90" y2="186"/></g>
  <text x="100" y="150" font-size="10.5" fill="#111827">cmem_rsp_q[p]</text>
  <text x="100" y="166" font-size="10" fill="#475569">FIFO 深 CMEM_LAT_MAX · 1W1R</text>
  <line x1="222" y1="60" x2="248" y2="60" stroke="#475569" marker-end="url(#q4)"/>
  <line x1="222" y1="155" x2="248" y2="155" stroke="#475569" marker-end="url(#q4)"/>
  <rect x="252" y="30" width="400" height="160" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="260" y="44" font-size="8.5" fill="#6b7280">M4</text>
  <text x="644" y="44" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="268" y="64" font-size="12" fill="#111827">延迟线与响应（每口）</text>
  <text x="268" y="84" font-size="10.5" fill="#475569">1. rdout[b].valid → rsp_q[port].push({data, scale, tag, due = t_in + CMEM_LAT_&lt;port&gt;})</text>
  <text x="268" y="100" font-size="10.5" fill="#475569">   1.1 DTE 的两个 bank 合并成一项 256 B（bank 与 bank+1 的 rdout 同拍到）</text>
  <text x="268" y="116" font-size="10.5" fill="#475569">2. rsp_q[p].front.due == now → 出队，rsp_valid[p] = 1，rsp_data / rsp_scale / rsp_tag = 该项</text>
  <text x="268" y="132" font-size="10.5" fill="#475569">3. 否则 rsp_valid[p] = 0</text>
  <text x="268" y="160" font-size="9.5" fill="#9ca3af">端到端拍数由 CMEM_LAT_&lt;port&gt; 决定，M1～M3 固定 4 拍，此级补足其余</text>
  <line x1="654" y1="110" x2="740" y2="110" stroke="#475569" marker-end="url(#q4)"/>
  <polygon points="750,85 940,85 930,135 740,135" fill="#f8fafc" stroke="#374151"/>
  <text x="845" y="114" font-size="10.5" fill="#374151" text-anchor="middle">cmem_*（rsp_* 字段）</text>
</svg>
```

**Matrix Mem 的差别**：M1 的 `mmem_mu_rd` 请求不带地址而带行号，M2 里它一次占全部 32 个 bank；M2 的同 bank 冲突不排队，只执行 MU、另一方保持并计数；M3 整行 256 B 写、无 byte mask；M4 的 due 按 `MMEM_LAT_DTE_RD / MMEM_LAT_DTE_WR / MMEM_LAT_MU`。

**Share Mem 的差别**：单 bank；M2 四个请求者轮询（待定），`smem_find` 授予后占住 M2～M3 逐拍读一字，直到读到非零或 `cur == end`，`smem_find_ctx` 记游标；M4 的 due 按 `SM_LATENCY`，find 的 due 从最后一次读算。

***

## 6　参数汇总

```
CMEM_LAT_DTE      13    // 第 4 章 Cmem MAS：DTE 读写 13T
CMEM_LAT_MU       11    // MU 读写 11T
CMEM_LAT_VU       14    // VU 读写 14T
CM_RV_LATENCY     15    // 第 3 章 15～25T，取下限（待定）
CMEM_LAT_MAX      25    // rsp_q 深度
CMEM_BANKS        8     // bank = addr[9:7]（bank 摆放规则按 Cmem MAS 图，待定）
CMEM_ROW_BYTES    128
MMEM_LAT_DTE_WR   9     // 第 4 章 Mmem MAS，lane 内延迟
MMEM_LAT_DTE_RD   8
MMEM_LAT_MU       8     // 第 8 章有 4T / 8T / 17T 三个值，取 Mmem MAS 的 8T
MMEM_LAT_MAX      50    // 端到端上限，rsp_q 深度
MMEM_BANKS        32    // 与 MU 32 物理 lane 一对一；地址到 bank 的映射按 Mmem MAS 的 bank 摆放图（待定）
MMEM_ROW_BYTES    256
SM_LATENCY        5     // 第 3 章 5～10，取下限（待定）
SMEM_ARB          轮询  // 待定，原文未给
带宽：Core Mem (1 KB + 32 B)/T；Matrix Mem (8 KB + 1 KB)/T；Share Mem 4 B/T × 每拍 1 请求
```

M1～M3 固定 4 拍、其余落在 M4 的拆法是本文定的（待定）：MAS 只给端到端拍数，不给级间拆分。

***

## 7　机制覆盖

### Core Mem

| 机制 | 落点 | 用例 |
| - | - | - |
| bank 冲突二选一，无冲突同时访问 | M2 逐 bank 仲裁 | `cmem_bank_arb` |
| DTE 局部反压：只反压冲突的 bank | M2 第 3 条 + M1 的 `req_ready` | `cmem_dte_partial` |
| 同组同优先级：DTE 先读写各自判断，再 wr 与 rd 判断 | M2 第 2.1 条 | `cmem_dte_two_writers` |
| 非同组优先级 MU > VU = DTE | M2 第 2 条 | `cmem_priority` |
| 132 B 访问：MU / VU 按 132 B 读写，scale 使能时同读写 scale 寄存器；只访问 SRAM 时有效带宽 128 B | M3 第 1、2 条 | `cmem_scale_regs` |
| Byte mask 写；非全 1 留存记录 | M3 第 2、2.1 条 | `cmem_byte_mask` |
| 延迟 DTE 13T、MU 11T、VU 14T；带宽 DTE 256 B/T、MU / VU 128 + 4 B/T，总 1 KB + 32 B/T | M4 的 due + 每口每拍一请求 | `cmem_latency` |
| DTE RV core 读写 CM，读固定回整行、32 bit / 拍取用 | `cmem_rv` 口 + RV core 文档的 cm_lsq | `rv_cm_read` |
| stream_id 分片：`base(stream_id) = 分片大小 × stream_id`，统一偏移映射 | master 侧 `CoreContext::StreamBase()`，本模块只看物理地址 | `cm_stream_slicing` |
| ECC、计数器、ctrl_noc 后门 | `cmem_cfg` 口按 4 B/T；ECC 不建 | `cmem_ctrl_noc` |

### Matrix Mem

| 机制 | 落点 | 用例 |
| - | - | - |
| 同 bank 互斥：两个 master 同 bank 只执行 MU 请求，报错并计数 | M2 + `mmem_conflict_cnt` | `mmem_conflict_mu_wins` |
| MU 一次读 32 bank 同一行，8 KB + 1 KB scale | `mmem_mu_rd` 口 + M2 全 bank 授予 | `mmem_mu_row_read` |
| scale 地址计算 | `mmem_scale` 与数据行一一映射 | `mmem_scale_addr` |
| 三种角色（权重 / reduction 数据 / token 缓冲） | 软件布局 | `bcore_slots`、`rcore_out_of_order` |
| 延迟：端到端 50T 以内；DTE 写 9T / 读 8T、MU 读 8T（lane 内）；带宽 DTE 256 B/T、MU 8 + 1 KB/T | M4 的 due | `mmem_latency` |
| 地址粒度 128 B，不支持 Byte mask | M1 断言 | `mmem_granule` |
| ECC 自纠回写、计数器 | 不建 | — |

### Share Mem

| 机制 | 落点 | 用例 |
| - | - | - |
| 三种用途：task 间共享、用户映射表、标量 | 软件 | `bcore_two_chains`、`rcore_out_of_order` |
| RV core 访问 5～10 拍，每拍 1 请求 | M1 每口一槽 + M4 的 due | `rv_latency` |
| DTE 完成后写 valid 标志 | `smem_dte_wr` 口 | `dte_sharemem_write` |
| `flag_check` 查找第一个非零字，找不到返回全 1 | `smem_find` 口 + M2～M3 的扫描 | `rv_flag_check` |

***

## 8　取舍

* **四级骨架为什么对三块存储共用**
  * MAS 对三者都只给端到端拍数和仲裁规则，没有级间时序
  * 把级间拆分固定成“1 + 1 + 2 + 其余”，端到端拍数仍等于 MAS 值
  * 仲裁点的位置（进模块后第 2 拍）对三者一致
  * 日后拿到 MAS 的级间时序，只改参数表和 M4 的长度
* **延迟线为什么放在每个 master 口，不放在每个 bank 上**
  * 返回顺序按口保序，不按 bank 保序
  * 这与 DTE 的 outstanding 记账、MU ldq 的按序回填一致
