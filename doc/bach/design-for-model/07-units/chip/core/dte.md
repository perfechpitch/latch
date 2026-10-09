# DTE DSA

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **DTE DSA**

给实现 DTE 的人：三个模块各自做哪些事、端口与存储怎么定。数据通路收敛成一个 `Mover`，按任务顺序逐拍推进。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《DTE 数据搬运引擎》全篇
* 《Router 片上交换与归约》：“进 core”“出 core”“Reduce”的 DTE 侧职责
* 《归约的完整过程》：“TS 与 DTE 侧的配合”

***

## 1　定位与边界

DTE 只做搬运，不做计算，职责五件：接纳任务、生成访问命令、处理背压、追踪在途事务、向 TS 报告最终完成。

难点在两端的节奏对不上：Router 一侧流式到达、什么时候来由上游决定；存储一侧要过 DMA_XBAR 抢 bank。这一版的做法是**把数据通路收敛成一个 `Mover` 模块**：一个任务从接纳到报完成都在一个模块里按顺序走完，不再拆读写两半、不再有中间 Buffer、credit、drain 队列这些逐拍协调细节，读侧保留 outstanding（一块存储一队按序在途读，掩盖读延迟）。代价是去掉读与写/发之间的流水重叠后 cycle 数会变，功能结果（字节级落点、包内容、事件计数）不变。

DTE 由三个模块组成：

| 模块 | 职责 |
| - | - |
| `Hmem` | 几张表：硬件/软件合并包头、RouterTable 副本、stream_cache、path→task 反查 |
| `DteRegfile` | 配置前端：19 项配置 + 模板 dirty 合并 + trigger Fire + held 反压，起好的任务经 `DescPort` 交给 Mover |
| `Mover` | 数据通路：中央任务队列、5 个通道槽、收帧、逐段读写、发拍、完成上报，全部内联 |

支持五种搬运方向：

| Route | 走哪个通道 | 说明 |
| - | - | - |
| Router → MM | 进核通道 `in_ch` | 配置驱动：RV core 配 CFG + trigger |
| Router → CM | 进核通道 `in_ch` | 同上 |
| MM → Router | 出核通道 `out_ch[n]`，n 由这条 path 的 VC 定 | 出口是 Router TX |
| CM → Router | 出核通道 `out_ch[n]`，n 由这条 path 的 VC 定 | 出口是 Router TX |
| MM → CM | 固定走 `out_ch[3]` | 出口切到 DMA WR1，硬件 route mask 只允许 CoreMem |
| CM → MM | — | 本版本不支持，XBar 不提供 WR_CH1 到 Matrix Memory 的连接 |

任务只有一个入口：DTE RV core 配寄存器 + 写 `CFG_TRIGGER`，Regfile 快照 Descriptor 送进 Mover 的中央任务队列（深度 16）。Router 入站帧不是任务入口——它只与“进核任务”按到达顺序 FIFO 配对，一帧对应一笔进核任务。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1560 900" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" role="img" aria-label="DTE DSA 第 0 层">
<title>DTE DSA 第 0 层</title>
<defs><marker id="a" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#475569"/></marker><marker id="as" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#475569"/></marker></defs>
<rect x="0" y="0" width="1560" height="900" fill="#ffffff"/>
<text x="20" y="26" font-size="12" fill="#111827">DTE DSA · 第 0 层（三个模块：Hmem 表、Mover 数据通路、DteRegfile 配置前端。方位：RV core 与 TS 在上，存储与 Router 在下）</text>
<text x="20" y="46" font-size="9.5" fill="#6b7280">一个任务从接纳到报完成都在 Mover 里按顺序走完，不拆读写两半，没有中间 Buffer / credit / drain / Join；读侧保留 outstanding</text>
<rect x="160" y="110" width="300" height="113.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="172" y="131" font-size="11" fill="#111827" font-weight="600">DteRegfile</text>
<text x="172.0" y="148.0" font-size="8.5" fill="#475569">19 项配置 + 8 套模板，dirty 掩码合并</text>
<text x="172.0" y="161.5" font-size="8.5" fill="#475569">写 CFG_TRIGGER 提交任务，采样这笔写</text>
<text x="172.0" y="175.0" font-size="8.5" fill="#475569">　带进来的 STUPV 身份</text>
<text x="172.0" y="188.5" font-size="8.5" fill="#475569">拼出 4 段位 Descriptor 经 DescPort</text>
<text x="172.0" y="202.0" font-size="8.5" fill="#475569">　交给 Mover</text>
<rect x="500" y="110" width="300" height="127.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="512" y="131" font-size="11" fill="#111827" font-weight="600">Hmem</text>
<text x="512.0" y="148.0" font-size="8.5" fill="#475569">16 项 × {core_mask 2 B, hardware_used 1 B,</text>
<text x="512.0" y="161.5" font-size="8.5" fill="#475569">　sw_header 16 B}，按 stream_id 索引</text>
<text x="512.0" y="175.0" font-size="8.5" fill="#475569">硬件只改 core_mask 与 hardware_used，</text>
<text x="512.0" y="188.5" font-size="8.5" fill="#475569">　RV core 改软件包头</text>
<text x="512.0" y="202.0" font-size="8.5" fill="#475569">RouterTable 副本、stream_cache、</text>
<text x="512.0" y="215.5" font-size="8.5" fill="#475569">　path→task 反查表</text>
<rect x="840" y="110" width="400" height="181.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="852" y="131" font-size="11" fill="#111827" font-weight="600">Mover（数据通路）</text>
<text x="852.0" y="148.0" font-size="8.5" fill="#475569">中央任务队列 pending（深度 16）+ 5 个通道槽</text>
<text x="852.0" y="161.5" font-size="8.5" fill="#475569">　（in_ch + out_ch[0..3]），每槽一个在途任务</text>
<text x="852.0" y="175.0" font-size="8.5" fill="#475569">收帧 rx_frames 与进核任务 FIFO 配对</text>
<text x="852.0" y="188.5" font-size="8.5" fill="#475569">逐段读写存储，一拍搬一拍；出核读侧按序流水</text>
<text x="852.0" y="202.0" font-size="8.5" fill="#475569">存储读/写口与 Router 出核单口内联仲裁</text>
<text x="852.0" y="215.5" font-size="8.5" fill="#475569">done_pend 串行化完成上报，shareMem 写在前</text>
<text x="852.0" y="229.0" font-size="8.5" fill="#475569">ack_ts_en 恰好一次，向 DonePort 一拍报一笔</text>
<text x="852.0" y="242.5" font-size="8.5" fill="#475569">观测计数：parsed / admitted / joined / sent / reported / occupancy</text>
<text x="852.0" y="256.0" font-size="8.5" fill="#475569">另记 lane 起终点 / 配置突发起点→trigger，供 Perfetto 6 条 DTE 轨道</text>
<polygon points="599,44 710,44 701,74 590,74" fill="#f8fafc" stroke="#374151"/>
<text x="650.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">dsa_done → TS</text>
<polygon points="1289,44 1400,44 1391,74 1280,74" fill="#f8fafc" stroke="#374151"/>
<text x="1340.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">dsa_cfg（RV core）</text>
<rect x="300" y="400" width="520" height="127.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="312" y="421" font-size="11" fill="#111827" font-weight="600">Mover 逐拍推进（Step 顺序）</text>
<text x="312.0" y="438.0" font-size="8.5" fill="#475569">ReceiveFromRouter（收帧 + 反压 + 序号去重）</text>
<text x="312.0" y="451.5" font-size="8.5" fill="#475569">Dispatch（pending → 空闲槽；进核要等帧配对）</text>
<text x="312.0" y="465.0" font-size="8.5" fill="#475569">CollectResponses（读响应按“一块存储一队按序在途读”认归属）</text>
<text x="312.0" y="478.5" font-size="8.5" fill="#475569">AdvanceAll（逐槽推进读/写）</text>
<text x="312.0" y="492.0" font-size="8.5" fill="#475569">SendStep（Router 单口，帧粘连不插拍）</text>
<text x="312.0" y="505.5" font-size="8.5" fill="#475569">Report（done_pend 串行化，shareMem 写在前）</text>
<polygon points="440,640 680,640 670,700 430,700" fill="#f8fafc" stroke="#374151"/>
<text x="560" y="662" font-size="10.5" fill="#374151" text-anchor="middle">cmem_rd / cmem_wr · mmem_rd / mmem_wr</text>
<text x="560" y="686" font-size="9.5" fill="#6b7280" text-anchor="middle">smem_wr · mu_topk</text>
<polygon points="720,640 960,640 950,700 710,700" fill="#f8fafc" stroke="#374151"/>
<text x="840" y="662" font-size="10.5" fill="#374151" text-anchor="middle">in_core_data_ch</text>
<text x="840" y="686" font-size="9.5" fill="#6b7280" text-anchor="middle">out_core_data_ch</text>
<rect x="620" y="770" width="320" height="60" rx="4" fill="#f1f5f9" stroke="#334155"/>
<text x="780" y="796" font-size="10.5" fill="#334155" text-anchor="middle">经 CoreStation 到 Router</text>
<text x="780" y="816" font-size="9.5" fill="#6b7280" text-anchor="middle">经 DMA_XBAR 到 Core Mem / Matrix Mem</text>
<path d="M310 223.5 L840 200" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<text x="430" y="210" font-size="8.5" fill="#6b7280" text-anchor="start">DescPort：起好的 Descriptor</text>
<path d="M650 237 L900 200" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<text x="660" y="230" font-size="8.5" fill="#6b7280" text-anchor="start">查表（Hmem）</text>
<path d="M560 527 L560 639" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<path d="M840 527 L840 639" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<text x="20" y="860" font-size="10.5" fill="#374151" text-anchor="start">五种搬运方向：Router → MM、Router → CM 走进核通道；MM → Router、CM → Router、MM → CM 走出核通道。本版本不支持 CM → MM。</text>
<text x="20" y="882" font-size="10.5" fill="#374151" text-anchor="start">数据布局仅支持连续一维搬运，不支持 stride；单个 DTE 任务的搬运量上限 32 KB（256 B × 128 拍），超过的拆成多个任务包下发。</text>
</svg>
```

***

## 2　功能清单

一功能一条，编号供“机制覆盖”一章引用。

### 收帧与接纳（原 Header Parser + Commit 合并进 Mover）

| 编号 | 功能 |
| - | - |
| F1 | 首拍锁存 Header，检查长度和帧格式 |
| F2 | 检查 `byte_count`：不超过单任务上限 32 KB（`kMaxTaskBytes`），与后续 Payload 的 TKEEP 累计值及 TLAST 位置一致 |
| F3 | 包头上下文（core_mask / hardware_used / gpu_id / token_id）随进核任务落库，逐拍转发 payload；不生成 Descriptor——任务由 RV core 配置驱动 |
| F3a | Descriptor 的 `stream_id` / `task_id` / `user_id` / `path_id` / `vcid` 由 RV core 随配置写送来（F14a），不取自包头 |
| F3c | Descriptor 的 `dst_addr` 由软件配 `CFG_ADDRx_DST` + `stream_id × stride` 展开（F46/F47），落哪块存储由 `CFG_TRANS_MODE` 的 route 编码决定。回不回 Ack（F3d）是本 core 的进核配置，由 SCP 逐 core 写 |
| F3d | B core 与 R core 的进核配置是不回 Ack：这两种 core 上进来的包不建 stream 表项，进核那一笔的完成没有可报的对象 |
| F3b | 每一帧另编一个帧号，从这里发给进核通道。进核那一路按帧号认“这几拍属于哪一帧” |
| F4 | 一帧一任务：同一个 AXI-Stream Frame 只属于一个 Router → MM 或 Router → CM 任务，不允许任务间交织 |
| F5 | 首拍固定为 Header，靠“上一帧 TLAST 已接受”判断下一拍是新 Header |
| F6 | Payload Fire 由收帧槽是否有空决定（满则 TREADY 反压）；TLAST 标识最后一个 Payload beat；`byte_count` 为 0 时可由 Header beat 同时携带 TLAST |
| F7 | TKEEP 按字节粒度生效，每个 Payload Fire 累计 TKEEP 有效字节，TLAST 时与 `byte_count` 比较 |
| F8 | 非法 Header 直接断言。进核任务与数据包按到达顺序一一配对，丢掉一帧，之后的配对整体错开 |
| F9 | 任务接纳：写 Trigger 起的 Descriptor 进 Mover 的中央任务队列 `pending`（深度 16）；进核任务要等到有收到的帧才放进通道槽（FIFO 配对），出核任务直接放 |
| F10 | 中央队列满时拉低 `dsa_cfg.req_ready`，反压 DTE RV core；Router 收帧槽满（`rx_frames` 到 16）时拉低 `tready`，反压 Router |
| F12 | `pending` 从队头往后扫，把任务填进空闲的通道槽；槽被占或进核无帧可配时该任务留在队列里等 |
| F13 | 只有一个任务入口：所有任务（进核 + 出核）都由 RV core 配 CFG + trigger 起，汇成同一套内部任务模型 |
| F14 | RV core 侧的配置序列：逐段写 19 项任务配置寄存器，最后写 `CFG_TRIGGER`（0x0000）提交任务。必须最后写 Trigger |
| F14a | 写 Trigger 被收下那一拍把 19 项配置与这笔写带进来的五个身份一起采下来拼成 4 段位 Descriptor |
| F14b | 一笔配置写在被收下之前一直保持同一个序号，按序号去重 |

**Fast LUT（待评估）**：是否保留待评估，模型里所有任务都由 RV core 配寄存器起。

### 五个通道与通道槽

| 编号 | 功能 |
| - | - |
| F15 | 五个物理通道：一个进核通道 `in_ch`，四个出核通道 `out_ch[0..3]` **与 Router 的四个 VC 一一对应**。`MM → CM` 不另开通道，固定复用 `out_ch[3]` |
| F17 | 每个通道一个在途任务槽（`slots[lane]`，`std::optional<Task>`），同一通道内同一时刻只推进一个任务，任务搬完才放下一个 |
| F18 | **通道之间可以乱序执行**：某个 VC 阻塞只堵住对应的那个出核通道，别的通道照发。**向 TS 反馈完成按任务搬完的顺序** |
| F22 | 数据布局仅支持连续一维搬运，当前不支持 stride |
| F23 | 单个 DTE 任务的搬运量上限是 32 KB（256 B × 128 拍），比包上限小，超过的要拆成多个任务包下发 |

### 三条数据流

| 编号 | 功能 |
| - | - |
| F32 | Inbound（Router → MM / CM）：RV core 配 CFG + trigger 起任务 → 任务进中央队列 → 收到帧后与进核任务 FIFO 配对放进通道槽 → 逐段写存储（包头落库、数据段写、scale 走 WriteScale、topK 走旁带写 MU）→ 报完成 |
| F33 | Outbound（MM / CM → Router）：任务进中央队列 → 放进通道槽建包（MakeOutboundMsg：回读包头上下文、填身份与长度、带 scale / topK）→ 逐段读存储填 payload（读侧按序流水，一块存储一队按序在途读）→ 逐拍发 Router（数据段 + scale 段分开发 beat，带 topK 的补最后一拍）→ 报完成 |
| F34 | Inner（MM → CM）：完全走出核通道，固定占 `out_ch[3]`。读 Mmem（按序流水）→ 写 Cmem → 报完成 |
| F35 | 各槽之间彼此独立、按序推进；每拍各口仍遵守 valid / ready |

### 完成上报

| 编号 | 功能 |
| - | - |
| F27 | 任务搬完（读/写/发三相交替推进到头）即报完成，不再有“RD/WR 两侧 Join”——一个任务就是一个上下文，没有两半要合 |
| F28 | 同一拍多个任务搬完时全部写进 `done_pend` 队列，由它串行化，不覆盖不丢失 |
| F29 | 向 TS 的报告是 exactly-once |
| F31 | `ack_ts_en`（= `CFG_TRANS_MODE[9]`，即旧命名 `task_last`）标记一个 task 拆成几笔搬运时的最后一笔，只有带这个标记的那一笔完成后才通知 TS；`no_ack` 置位的任务不回 Ack |

### Hmem 与四类内容的存放

| 编号 | 功能 |
| - | - |
| F36 | 一次搬运的对象是一个 MSG 包，进了 core 就按内容归到 4 个段位、各落各的存储：段 0 绑定包头，段 1~3 通用、内容由软件约定装 data / scale / topK。每个段落的存储由该段地址高 4 bit tag 译码决定（0x0 Cmem 数据 / 0x1 Mmem 数据 / 0x2 scale 旁带 / 0x3 topK_table / 0x4 header_table） |
| F37 | topK 段由地址译码命中 `topK_table`（tag 0x3），进核落地时经旁带 `mu_topk` 写进 MU 的 `topK_ep_table`（256 B 一拍、整笔只写一次），出核时从表里取出附回包里。表下标取 topK 段地址的端内偏移：计算 core 是 `stream_id`，B core 是 `user_id` |
| F38 | 包头落在哪块存储由段 0 地址高 4 bit tag 译码决定：计算 core 打 header tag（0x4）落 Hmem（按 stream_id 索引）；B/R core 写 Core Mem 纯地址（tag 0）、按用户分一块，不进 Hmem |
| F39 | B core / R core 上：包头进 Core Mem，按用户分一块、不占 Hmem 那张表。data 落 Matrix Mem，scale 随它存进 Matrix Mem 的 scale 部分；topK 经旁带写 MU |
| F40 | **DTE 内不再存 `path_id_table` 与 `task_len_table`**：`path_id` 随配置写送来，`size` 由 RV core 配寄存器给，或按 `data_len` 算出来 |
| F41 | 包头分工：硬件只改硬件包头（`core_mask` 与 `hardware_used`），RV core 改软件包头 |
| F42 | 每段长度由各自的 `CFG_DATA_LENi`（字节）定：段 0 是包头（18 B），段 1~3 装 data / scale / topK 的内容由软件约定 |
| F43 | 进核时计算 core 只在这个 token 需要分配新 `stream_id` 时才存包头（`hw_header_op = 1`），中间环节的 reduce 与 concat 任务直接丢弃 |
| F44 | 出核时改写硬件包头：`path_id` 用 TS 送来的那个，`size` 用 RV core 配的寄存器 |
| F44b | 出核不改的包头字段沿用进核那一笔的：DPU 写的 `gpu_id` 与 `token_id` 在进核时记进 Hmem 里这个 `stream_id`，出核造包时取回来填上 |
| F45 | 支持纯包头任务（`data_len = 0`），进出 core 都可以 |
| F46 | 地址的一条规矩：软件逐段配基址（`CFG_ADDRi`）与 stride（`CFG_STRIDEi`），偏移由硬件用 `stream_id` 算出来：`stream_start_i = CFG_ADDRi + stream_id × CFG_STRIDEi` |
| F47 | 通用寻址式子是 `stream_start_i = CFG_ADDRi + stream_id × CFG_STRIDEi`，**Cmem 一侧用低 20 bit、Mmem 一侧用低 26 bit**；每段落哪块存储由该段地址高 4 bit tag 译码 |
| F48 | scale 的长度由软件配段 2 的 `CFG_DATA_LEN2`（字节），硬件不再自己算：默认每 32 个元素 1 B，即 `data_len / 32`。topK 是旁带、长度记 0、内容随包整笔写 MU |
| F49 | Matrix Mem 一侧不加 stream 偏移，Core Mem 一侧加 |
| F49a | MXFP8 数据的 scale 随数据走：scale 是 E8M0，每 32 个元素 1 B；MXFP8 数据在 Core Mem 与 Matrix Mem 里都按每 128 B 配 4 B scale |

### shareMem 写

| 编号 | 功能 |
| - | - |
| F50 | 任务数据传输完成后，按 `sharemem_waddr` / `sharemem_data` 写 shareMem，然后通知 TS |
| F51 | 只在 B core 与 R core 使用，存 user_id 与 token entry 的 valid 标志 |
| F51a | 搬入那一笔的标志由 kernel 配 CFG 表达，第几项按落点除以槽位大小算，写进去的值是 valid |

### 出核前的资源与流控

**分工**：下游的 Stream 资源与 Rmem 资源由 TS 在下发前查；**DTE 这一侧 dispatch 时不查 VC credit**。业务层资源以 stream 为单位、生命期跨整条任务链，只能在 TS 下发前查好；而 DTE 到 Router 那一条本地链路的 VC 反压逐拍变化、真正发 flit 那一刻才知道，由 CoreStation 的 `DteReady()` 在 flit 层端到端兜住。

**`stream_cache`**：DTE 里另存一份 Router 那张 stream 表的副本，**只跟随、不分配**，靠 Router 各方向送回的 release 同步。

| 编号 | 功能 |
| - | - |
| F52 | DTE 内维护一份 RouterTable 副本，按 PathID 查到 VC 与资源需求；软件负责写入并保证与 Router、ReduceModule 三方一致 |
| F53 | dispatch 时不查 VC credit：由 CoreStation 的 `DteReady()` 在 flit 层端到端兜住；下游 Stream / Rmem 资源不在这里查 |
| F54 | 中央任务队列排在 dispatch **之前**：RV core 配好任务后 Descriptor 先进 `pending`，进核任务还要等帧配对 |
| F55 | 中央队列满（16）时拉低 `dsa_cfg` 的 `req_ready`，反压 DTE RV core |
| F55b | 四个出核通道对 Router 只有一个 `out_core_data_ch`，轮转仲裁；授权粘在一个通道上直到它把带 `tlast` 的那一拍发完，一个包的几拍中间不会插进别的包 |
| F56 | Reduce 包与其他出核包一样 dispatch 时不查 VC credit；本级 Rmem 的 credit 由 TS 按用户记 |
| F61 | 业务层 credit 只有一类：下游那个 core 的 coremem credit，分方向，由 TS 在下发前查。DTE 只负责 credit 回程 |
| F62 | credit 回程：解析本级 Router 各方向传进来的 core credit release，按 action 更新本地表 |

### 软件配置的寄存器

| 编号 | 功能 |
| - | - |
| F64 | `CFG_TRANS_MODE`（0x04C，10 bit）位域：`transfer_mode`[2:0]、`addr_valid`[6:3]、`hw_header_op`[7]、`wr_sharemem_flag`[8]、`ack_ts_en`[9] |
| F65 | `CFG_TRIGGER`（0x0000，WO，4 bit）位域：`temp_valid`[0]、`temp_index`[3:1]。写 0x0000 这个动作本身 = 提交任务 |
| F66 | 19 项任务配置寄存器（0x004~0x04C）：每段 i 一组 `{CFG_ADDRi_SRC, CFG_ADDRi_DST, CFG_STRIDEi, CFG_DATA_LENi}`，加 `CFG_SM_W_ADDR` / `CFG_SM_W_DATA` / `CFG_TRANS_MODE`；另有 8 套模板与 Header Table |
| F67 | 不随任务变的控制与观测寄存器本轮**不落地**，只保留地址区间占位 |
| F68 | 异常四类本轮**不落地**，不做行为实现 |

### 包的边界与读写通路

| 编号 | 功能 |
| - | - |
| F69 | 包长范围：最短 16 B，即只含包头与路由信息的空包；最长允许 64 KB，实际支持到 (16 K + 32) B |
| F70 | 不设包尾。包的结束靠包长度计数 |
| F71 | reduce 包的软件辅助信息长度**必须固定 16 B** |
| F72 | Router 把数据送给 DTE 时**不剥离任何数据** |
| F73 | 软件处理一个包用两条通路：包头路由信息用标量 store / load，用户 token 数据用 DMA 搬 |
| F74 | reduce 包出核时 DTE 在硬件字段里打上 `reduce_seq`，取的是发这一包的 `task_id` |

***

## 3　接口

```
port in_core_data_ch (slave, AXI-Stream-Like, clk)    // CoreStation → DTE：进 core 的整包
  in  tvalid · tdata[2047:0] · tkeep[255:0] · tlast · thdr
  out tready                                            // = 收帧槽有空（rx_frames 未满 16）
port out_core_data_ch (master, AXI-Stream-Like, clk)  // DTE → CoreStation：出 core 的整包
  out tvalid · tdata[2047:0] · tkeep[255:0] · tlast · thdr · vc_id[1:0]
  in  tready                                            // = 该 VC 的 Core 方向输入 VC 有空
port router_credit (slave, 电平 + 脉冲, clk)          // Router 侧回来的三类信息
  in  stream_credit_vld[2:0] · stream_credit_user[2:0][15:0]
  in  reduce_release_vld · reduce_release_user[15:0]
port dsa_cfg (slave, valid/ready, clk)                // DTE RV core 的 dsa_iss，身份随这笔请求走
  in  req_valid · req_we · req_addr[13:0] · req_wdata[31:0]
  in  stream_id[3:0] · task_id[5:0] · user_id[15:0] · path_id[7:0] · vcid[1:0]
  out req_ready                                         // = 配置通路未反压；中央任务队列满（16）时拉低
port dsa_rdata (master, 脉冲, clk)                    // 读寄存器的异步返回
  out valid · rdata[31:0]
port dsa_done (master, 脉冲, clk)                     // → TS：ack_ts_en 的那一笔完成时报
  out valid · stream_id[3:0] · task_id[5:0]
port cmem_rd / cmem_wr (master, valid/ready, clk)     // 经 DMA_XBAR，256 B
port mmem_rd / mmem_wr (master, valid/ready, clk)     // 经 DMA_XBAR，256 B
port smem_wr (master, valid/ready, clk)               // shareMem 表项写，只有 B core / R core 用
port mu_topk (master, 脉冲, clk)                      // → MU：进核包里的 topK 经旁带写 topK_ep_table
port cfg (slave, ctrl_noc 写事务, clk)                // 静态寄存器、Hmem、RouterTable 副本
```

***

## 4　存储器

```
mem hmem             FF 阵列   16 项 × {core_mask 2 B, sw_header 16 B} = 288 B         1R1W  按 stream_id 索引；硬件写 core_mask，RV core 写 sw_header  复位 0
mem rtab_copy        FF 阵列   64 项，RouterTable 的外部副本                           1R1W  软件写，三方一致        复位 0
mem path_task_map    FF 阵列   64 × task_id[5:0]，按 path_id 索引                      1R1W  boot 期由软件配          复位 0
mem inbound_cfg      FF        {route, no_ack, flag_base, flag_entry_bytes}            1R1W  SCP 在 core 配置阶段写    复位 0
mem stream_cache     FF 阵列   3 方向 × 16 项 × {valid, user_id[15:0]}                 1R1W  Router 的 User Resource Allocation Table 的 cache，只跟随不分配  复位空
mem cfg_file         FF 阵列   19 项 × 32 bit + 19 bit dirty 掩码                      1RW   显式写过的字段覆盖模板；Trigger Fire 后清 dirty  复位 0
mem template[8]      FF 阵列   8 套 × 19 项 × 32 bit，每套 128 B 对齐                  1RW   RV core 写；temp_valid 时以它为底  复位 0

// 下面这些都在 Mover 内部，是任务级状态，不是独立存储器
mem pending          deque     中央任务队列，16 项 × TaskDesc（已快照、尚未开始搬）     1W1R  满则拉低 dsa_cfg.req_ready  复位空
mem rx_frames        deque     已收齐、待配对的进核帧（Message 里已带完整 payload）     1W1R  满则拉低 tready            复位空
mem slots[lane]      FF        5 个通道各一个在途任务槽（optional<Task>）              1RW   任务搬完才放下一个          复位空
mem done_pend        deque     完成上报队列（stream_id / task_id / user_id / 标志）     1W1R  串行化同拍多个完成          复位空
mem pmu_cnt          （不落地） Profile 区 11 个计数器本轮不实现，0x0800~0x0FFF 整段占位   —     —                          —
```

***

## 5　流水线总览

一个任务从接纳到报完成都在 Mover 里按顺序走完。性能剖析按下面这套口径分段：

```
setup_cycles  = task_slot_assign - task_accept_fire   // Regfile Fire 到放进通道槽
issue_cycles  = data_moved_done  - task_slot_assign   // 逐段读写
report_cycles = done_fire        - data_moved_done    // done_pend 串行化 + shareMem 写
total_latency = setup + issue + report
```

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 678 420" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="areov" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="678" height="420" fill="#ffffff"/>
  <text x="20" y="26" font-size="12" fill="#111827">DTE · 第 1 层流水线总览（一个任务在 Mover 里顺序走完）</text>
  <text x="20" y="42" font-size="9.5" fill="#6b7280">横向是级序，不是拍序。橙色虚线框是变长级，非按比例。</text>
  <text x="20" y="110" font-size="10.5" fill="#6b7280">接纳</text>
  <rect x="150" y="78" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="92" font-size="8.5" fill="#6b7280">M1</text>
  <text x="292" y="92" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="112" font-size="11" fill="#111827">Regfile Fire</text>
  <rect x="316" y="78" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="92" font-size="8.5" fill="#6b7280">M2</text>
  <text x="458" y="92" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="112" font-size="11" fill="#111827">进中央队列</text>
  <path d="M300 106 L315 106" stroke="#475569" marker-end="url(#areov)" fill="none"/>
  <rect x="482" y="78" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="492" y="92" font-size="8.5" fill="#6b7280">M3</text>
  <text x="624" y="92" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="492" y="112" font-size="11" fill="#111827">放进通道槽</text>
  <path d="M466 106 L481 106" stroke="#475569" marker-end="url(#areov)" fill="none"/>
  <text x="20" y="200" font-size="10.5" fill="#6b7280">搬运</text>
  <rect x="150" y="168" width="316" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="160" y="182" font-size="8.5" fill="#92400e">M4</text>
  <text x="458" y="182" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="160" y="202" font-size="11" fill="#7c2d12">逐段读 / 写 / 发，一拍搬一拍</text>
  <text x="20" y="290" font-size="10.5" fill="#6b7280">完成</text>
  <rect x="150" y="258" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="272" font-size="8.5" fill="#6b7280">M5</text>
  <text x="292" y="272" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="292" font-size="11" fill="#111827">Finish 入 done_pend</text>
  <rect x="316" y="258" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="272" font-size="8.5" fill="#6b7280">M6</text>
  <text x="458" y="272" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="292" font-size="11" fill="#111827">shareMem 写 + 报 TS</text>
  <path d="M300 286 L315 286" stroke="#475569" marker-end="url(#areov)" fill="none"/>
  <text x="20" y="360" font-size="10.5" fill="#374151">各通道槽彼此独立：一个通道搬完立刻能放下一个任务，不等别的通道。</text>
  <text x="20" y="392" font-size="10.5" fill="#374151">去掉读写两半与中间 Buffer 后，读与写/发不再重叠，cycle 数仍比旧版多，但字节级结果不变；读侧本身按存储带宽推进。</text>
</svg>
```

***

## 6　逐级行为

Mover 每拍按固定顺序走一遍（`Step()`）：`ReceiveFromRouter → Dispatch → CollectResponses → AdvanceAll → SendStep → Report`，末尾把当拍没用到的存储口/旁带口置闲（`IdleReq`）。

### ReceiveFromRouter · 收进核帧

* 从 `from_router`（CoreDataPort）读一拍。`valid` 为 0 就 `DriveReady(true)` 返回。
* 按序号去重：同一拍数据会连着两拍出现在端口上，`seq == last_router_seq` 时只回 ready、不重复收。
* 收新帧（首拍是 Header）：`rx_frames` 已满（16）就 `DriveReady(false)` 反压、不记序号。
* 首拍做合法性检查：`Message` 必须带且 `size ≤ kMaxTaskBytes`，否则断言（F8）。合法就记 `rx_msg`、`++parsed`。
* `d.last` 时把整帧推进 `rx_frames`、清 `rx_msg`。
* `rx_frames` 未满就 `DriveReady(true)`。

### Dispatch · 任务进槽

* `TakeFromRv`：读 Regfile 的 `DescPort`。按序号去重，`pending` 满则 `DriveAccepted(false)`（反压 Regfile）。收下的 Descriptor 先过 `MarkReducePkt`（归约路径出核打 `reduce_seq = task_id`）再进 `pending`。
* 从 `pending` 队头扫：槽空闲且（进核任务有帧可配 / 出核任务直接放）才放进 `slots[lane]`。放进后 `++admitted`，记 `start_task` / `start_user`。
* 进核任务：取 `rx_frames` 队头一帧作 `msg`，`phase = kWrite`。出核任务：`MakeOutboundMsg` 建包，`phase = kRead`。
* 有任务可放但一个都没放出去（槽满或没帧）就 `++stalled`。

### CollectResponses · 读响应归属

* 一块存储一队按序在途读：`cmem_rd_owners` / `mmem_rd_owners` 是 FIFO，队头是下一笔响应属于哪个槽。
* 响应回来把数据填进出核任务的 `msg->payload`（从 `filled` 处往后填），`filled` 推进；读游标 `off` / `seg` 在发出当拍已推进，响应不再动它。

### AdvanceAll · 逐槽推进

* 每槽按 `phase` 调 `ReadStep`（kRead）或 `WriteStep`（kWrite）。

**ReadStep（出核读存储填 payload）**：跳过非读段（`ReadSeg` = 非 header 且 src 是 data/scale）。scale 段走 `ReadScale`（每 32 组一 B），data 段走 `Read`。读游标 `off` / `seg` 在发出的当拍推进，`owner` 进队列、`rd_outstanding` 加一；一个口一拍只发一笔（`*_used` 标记），不等响应。段都发完且 `rd_outstanding` 归零才 `phase` 切 kSend（出核）或 kWrite（MM→CM）。

**WriteStep（进核写存储 / MM→CM 写）**：进核第一拍先落包头上下文（`StoreHeader`，只一次）。跳过非写段（`WriteSeg` = 非 header 且 dst 是 data/scale/topk）。topK 段走旁带 `mu_topk->Drive` 写 MU，不落存储。scale 段走 `WriteScale`，data 段走 `Write`。段写完 `Finish(slot)` 报完成。

**StoreHeader（包头上下文落库）**：段 0 dst 是 header（tag 0x4，计算 core）→ 写 Hmem 对应 `stream_id` 项；否则（B/R core）→ `cmem_sync->Poke` 48 B 序列化块。

### SendStep · 出核发拍

* Router 只有一个口，四个出核通道共用。`router_owner` 粘在一个通道上，直到它发完带 `tlast` 的拍（含 topK 补拍）才撒手。
* 数据段 + scale 段分开发 beat（每个 `ReadSeg` 段各发各的）；带 topK 的包最后一拍 payload 不打 `last`，补一笔 topK 拍（字节随 `topk` 字段）打 `last`。
* 发完 `Finish(slot)` 报完成。

### Report · 完成上报

* `done_pend` 队头：`smem` 置位的先写 shareMem（`smem_wr->Write`，等 ready），写前不报 TS；`notify` 置位的（`ack_ts_en`）`to_ts->Drive(stream_id, task_id)` 报一笔，`++reported`；不报的直接出队。

***

## 7　参数汇总

```
物理通道           5（进核 `in_ch` 1 条 + 出核 `out_ch[0..3]` 4 条，与 4 个 VC 一一对应）；每条一个在途任务槽
中央任务队列       16 项（kCentralTaskQDepth），已快照、尚未开始搬的 TaskDesc
收帧队列           rx_frames 16 项，已收齐、待配对的进核帧
完成上报队列       done_pend，串行化同拍多个完成
与 Cmem 接口宽度    256 B/T，双向（DTE MAS 与 Cmem MAS 一致）
与 Mmem 接口宽度    256 B，双向；写 9T、读 8T
与 Router 接口宽度  256 B，双向（看不到 scale）
Hmem               288 B = 16 项 × {core_mask 2 B, sw_header 16 B}，按 stream_id 索引
寄存器地址空间     16 KB：Config 0x0000~0x03FF（CFG_TRIGGER + 19 项配置）、Ctrl/Status 0x0400、Profile 0x0800、Debug 0x0C00（后三段不落地）、Template 0x1000（8×128 B）、TaskQ 0x2000（只读回读）、Header Table 0x3000（16×128 B）
stream_cache       3 方向 × 16 项 × {valid, user_id}，Router 那张 stream 表的只读副本
Fast LUT           待评估；候选为 64 项 × {valid, length, ctrl_flags}，按 task_id 索引
DTE Setup Time     随 Fast LUT 待评估；模型不走 Fast LUT
单任务最大搬运量    32 KB（256 B × 128 拍）
MSG 包结构          包头标记 2 B + Router 信息 4 B + 包长度 2 B + 软件辅助信息 0～16 B + 业务数据 0～(64 K − 24) B
                   reduce 包另在硬件字段里带 reduce_seq 6 bit，取发这一包的 task_id
包长范围            最短 16 B，最长 64 KB、实际支持到 (16 K + 32) B；不设包尾，结束靠包长度计数
reduce 包           软件辅助信息固定 16 B，Router 做加法时跳过这 16 B
scale / topK 长度   scale 由软件配段 2 的 CFG_DATA_LEN（默认 data_len / 32）；topK 是旁带长度 0，每项 {local_ep_index 2 B, weight 4 B}、每 stream 上限 256 B
```

***

## 8　机制覆盖

| 机制 | 功能 | 用例 |
| - | - | - |
| Header 首拍锁存与长度检查 | F1、F2 | `header_check` |
| 一帧一任务，不允许任务间交织 | F4 | `one_frame_one_task` |
| 靠上一帧 TLAST 判断下一拍是新 Header | F5 | `frame_boundary` |
| TKEEP 累计与 byte_count 比较 | F7 | `tkeep_count` |
| 非法 Header 直接断言 | F8 | `illegal_header` |
| 任务进中央队列，进核等帧配对 | F9、F12 | `central_taskq` |
| 中央队列满反压 Regfile，收帧满反压 Router | F10 | `central_q_backpressure` |
| 进核任务 ↔ 数据包按到达顺序 FIFO 配对 | F3b | `fifo_pairing` |
| 写 Trigger 时取这笔写带进来的五个身份（STUPV） | F14a | `trigger_samples_ids` |
| 一笔配置写在被收下之前保持同一个序号 | F14b | `cfg_seq_stable` |
| 通道槽彼此独立，通道间乱序 | F17、F18 | `channel_ooo` |
| 四个出核通道轮转仲裁 Router 那一个口，一个包不被插断 | F55b | `out_arb_frame` |
| 出核包在放进槽时建好，读回的数据排进它的 payload | F33 | `outbound_packing` |
| 所有任务统一走 RV core 配 CFG + trigger | F13 | `central_taskq` |
| 必须最后写 CFG_TRIGGER，写 0x0000 提交任务 | F14 | `trigger_order` |
| temp_valid 时以模板为底、显式写字段覆盖 | F14a | `template_override` |
| reduce 包软件辅助信息固定 16 B | F71 | `reduce_hdr_fixed_16b` |
| reduce 包出核打 reduce_seq，Router 原样带回 | F74 | `reduce_seq_stamp` |
| Router 送 DTE 时不剥离任何数据 | F72 | `no_strip_from_router` |
| 包头走标量 load / store，数据走 DMA | F73 | `header_vs_data_path` |
| 单任务上限 32 KB，超过拆成多个任务包 | F23 | `task_split_32k` |
| 收帧满时经 TREADY 反压 Router | F10 | `buffer_backpressure` |
| 任务搬完即报完成，不再两侧 Join | F27 | `completion_join` |
| 同拍多个完成全部写入 done_pend，不丢失 | F28 | `join_serialize` |
| 向 TS exactly-once | F29 | `exactly_once` |
| ack_ts_en 才通知 TS，no_ack 不回 Ack | F31 | `task_last_ack` |
| 三条数据流各自的通路与出口绑定 | F32～F34 | `three_flows` |
| MM → CM 的 route mask 只允许 CoreMem | F34 | `wr1_route_mask` |
| topK 经旁带写进 MU 的 topK_ep_table | F37 | `topk_to_mu` |
| 一个包进核拆成四份分开存 | F36、F39 | `packet_split_four` |
| 包头落点由段 0 地址 tag 选：计算 core 合并进 Hmem、按 stream_id 索引，B/R core 进 Core Mem 按用户分块 | F38 | `hmem_merged` |
| path_id 随配置写送来、size 由 RV core 配，不再有查找表 | F40 | `no_lut_table` |
| 每段长度由各自的 CFG_DATA_LENi 配 | F42 | `data_len_scope` |
| hw_header_op 决定存不存包头 | F43 | `hw_header_op` |
| 出核改写 path_id / size / core_mask | F41、F44 | `header_rewrite` |
| B core 与 R core 上进核那一笔不回 Ack | F3d | `inbound_no_ack` |
| 进核落点、回不回 Ack 与标志表几何由 SCP 按 `DTEIN` 逐 core 配 | F3c、F51a | `inbound_cfg` |
| DPU 的 gpu_id 与 token_id 随数据出核 | F44b | `dpu_header_relay` |
| 纯包头任务 data_len = 0 | F45 | `header_only_task` |
| 软件逐段配基址与 stride，硬件按 CFG_ADDRi + SID × CFG_STRIDEi 算偏移 | F46、F47 | `stream_offset` |
| scale 与 topK 的长度由软件逐段配 | F48 | `seg_length` |
| 端点由段地址高 4 bit tag 译码决定 | F36、F47 | `endpoint_decode` |
| Matrix Mem 侧不加偏移，Core Mem 侧加 | F49 | `mm_no_offset` |
| MXFP8 的 scale 随数据进出核、在 Matrix Mem 与 Router 之间搬运 | F49a | `scale_with_data` |
| shareMem 写：搬入置 valid、搬出置 invalid | F50、F51 | `sharemem_flag` |
| 搬入那一笔的标志项按落点算，不逐笔由软件配 | F51a | `inbound_flag_index` |
| 三份 RouterTable 副本一致 | F52 | `dte_rtab_copy` |
| dispatch 不查 VC credit | F53 | `central_taskq` |
| Reduce 包与其他出核包一样 dispatch 不查 VC credit | F56 | `dte_reduce_vc_only` |
| 业务层 credit 分方向，先查 routing table | F61 | `credit_by_direction` |
| stream_cache 只跟随不分配，按 action 更新 | F62 | `credit_release_action` |

***

## 9　取舍

* **为什么把数据通路收敛成一个 `Mover`**
  * 旧版拆读写两半、中间 Buffer、credit、drain 队列，是为了让两端节奏对不上的流水线还能重叠推进，代价是 8 个模块 + 6 个端口类、靠多拍握手互相协调，结构复杂难读
  * 这一版按任务顺序逐拍推进，一个任务在模块里走完；代价是去掉读与写/发之间的流水重叠，cycle 数会变，但字节级落点、包内容、事件计数不变
  * 存储口与 Router 口仍保留逐拍 valid/ready，外部接口的逐拍搬运不动，只是内部不再有协调细节
* **为什么读侧保留 outstanding**
  * 读响应按“一块存储一队按序在途读”认归属：读游标在发出的当拍推进，响应按 FIFO 填 payload，读延迟被后续读掩盖，出核读侧回到按存储带宽推进（约 256B/拍）
  * 代价是读响应归属要维护一条按序队列；读与写/发之间仍不重叠（读完才写/发）
* **为什么通道按 VC 切、出核四份进核一份**
  * 出核任务共用一条出口，某个 VC 的 credit 耗尽会把排在后面、走别的 VC 的任务一起堵死
  * 现在把出核那条按 VC 分成四个通道槽，一个 VC 阻塞只影响自己那条；进核方向没有这个问题，仍是一条
  * `MM → CM` 不占 VC，固定复用 `out_ch[3]`，在目的端 MUX 到 Core Mem
* **为什么完成上报要串行化**
  * 同一拍可能有多个任务同时搬完，`done_pend` 队列把同拍多个完成串成逐拍上报，向 TS exactly-once
  * `ack_ts_en` 只标在一笔 task 拆几笔搬运时的最后一笔，只有那笔完成才通知 TS
* **为什么一个包进核要拆成四份分开存**
  * 四段的读者不同：data 与 scale 给 MU 和 VU 算，topK 经旁带写进 MU 的 topK_ep_table、MU 直接拿去查专家，包头只在这个 token 再出核时用来重写路由
