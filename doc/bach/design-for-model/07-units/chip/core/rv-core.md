# RV Core（DTE / MU / VU 各一）

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **RV core ×3**

给实现 RV core 的人：一个独立打拍的模块做哪些事、端口与存储怎么定。三个实例硬件相同、接口相同，区别只在绑定的 DSA、ITCM 里的镜像、可见的地址空间。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《Core 内硬件》“RV Core”全节：指令集、自定义指令、task 下发与完成、dsa_iss 的下发规则、LSU 与访存分流
* 《软件栈》：“每个 task 的共同形状”“各类 core 的软件流程”

***

## 1　定位与边界

RV core 是 TS 与 DSA 之间的桥梁：从 TS 收 task，按 `task_pc` 跑 ITCM 里的 kernel，配置 DSA 执行任务。它跑两类指令：

* **custom-0 自定义指令**：配置对应的 DSA、读 DSA 寄存器、结束 task、查映射表、循环
* **普通 load / store**：访问 DTCM、Share Mem，以及只有 DTE core 有的 Core Mem 与 Router I/O reg

指令逐条执行，不建流水线：`src/rv32` 的 `SystemRv32` 提供 RV32IMC、M 态 CSR 与译码，本模块覆盖 `Decode` 接入自定义指令，外面包一层 task_queue、dsa_iss、dsa_rq、lsq 与 gpr 就绪表做逐拍记账。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1420 930" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" role="img" aria-label="RV core 第 0 层">
<title>RV core 第 0 层</title>
<defs><marker id="a" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#475569"/></marker><marker id="as" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#475569"/></marker><marker id="g" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0f766e"/></marker><marker id="gs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0f766e"/></marker><marker id="o" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#b45309"/></marker><marker id="os" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#b45309"/></marker><marker id="p" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#7c3aed"/></marker><marker id="ps" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#7c3aed"/></marker><marker id="i" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#4338ca"/></marker><marker id="is" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#4338ca"/></marker><marker id="t" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0d9488"/></marker><marker id="ts" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0d9488"/></marker><marker id="r" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#be123c"/></marker><marker id="rs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#be123c"/></marker><marker id="b" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#2563eb"/></marker><marker id="bs" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#2563eb"/></marker><marker id="m" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#d97706"/></marker><marker id="ms" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#d97706"/></marker><marker id="l" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#9aa1ad"/></marker><marker id="ls" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#9aa1ad"/></marker></defs>
<rect x="0" y="0" width="1420" height="930" fill="#ffffff"/>
<text x="20" y="26" font-size="12" fill="#111827">RV core · 第 0 层（DTE / MU / VU 各一个实例，硬件相同、接口相同。方位：TS 在上，DSA 在下，Share Mem 与 Core Mem 在右，Router 在下，cfg 从上进）</text>
<text x="947" y="26" font-size="9.5" fill="#6b7280">黄色虚线框内是折算成每条指令 1 拍、不建流水线的部分</text>
<polygon points="249,44 360,44 351,76 240,76" fill="#f8fafc" stroke="#374151"/>
<text x="300.0" y="59.0" font-size="9" fill="#374151" text-anchor="middle">task_cmd / task_ack</text>
<text x="300.0" y="70.0" font-size="7.5" fill="#6b7280" text-anchor="middle">← TS</text>
<polygon points="509,44 620,44 611,76 500,76" fill="#f8fafc" stroke="#374151"/>
<text x="560.0" y="63.5" font-size="9" fill="#374151" text-anchor="middle">rv_done → TS</text>
<polygon points="1209,44 1320,44 1311,74 1200,74" fill="#f8fafc" stroke="#374151"/>
<text x="1260.0" y="62.5" font-size="9" fill="#374151" text-anchor="middle">cfg（ctrl_noc）</text>
<rect x="160" y="110" width="280" height="127.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="172" y="131" font-size="11" fill="#111827" font-weight="600">task_queue</text>
<text x="172.0" y="148.0" font-size="8.5" fill="#475569">提前接收 TS 下发的 task，做到用户之间</text>
<text x="172.0" y="161.5" font-size="8.5" fill="#475569">　task 的无 bubble 调度</text>
<text x="172.0" y="175.0" font-size="8.5" fill="#475569">按是否有空槽产生 task_ack；未被接收时</text>
<text x="172.0" y="188.5" font-size="8.5" fill="#475569">　TS 不能释放该 task 跳到下一个</text>
<text x="172.0" y="202.0" font-size="8.5" fill="#475569">队头 task 的 task_pc 驱动取指</text>
<text x="172.0" y="215.5" font-size="8.5" fill="#475569">深度 2（待定）</text>
<rect x="1160" y="110" width="200" height="140.5" rx="4" fill="#f5f3ff" stroke="#7c3aed"/>
<text x="1172" y="131" font-size="11" fill="#111827" font-weight="600">ITCM / DTCM</text>
<text x="1172.0" y="148.0" font-size="8.5" fill="#475569">ITCM 4 KB，8 B/T，1 拍</text>
<text x="1172.0" y="161.5" font-size="8.5" fill="#475569">　firmware · kernel</text>
<text x="1172.0" y="175.0" font-size="8.5" fill="#475569">　· bootloader</text>
<text x="1172.0" y="188.5" font-size="8.5" fill="#475569">DTCM 8 KB，32 bit × 4 bank</text>
<text x="1172.0" y="202.0" font-size="8.5" fill="#475569">　BSS 段 · 寄存器溢出 · 堆栈</text>
<text x="1172.0" y="215.5" font-size="8.5" fill="#475569">由 ctrl_noc 装载</text>
<text x="1172.0" y="229.0" font-size="8.5" fill="#475569">装载拍数 = 字节数 / 4 B</text>
<rect x="160" y="320" width="560" height="194.5" rx="4" fill="#fefce8" stroke="#a16207" stroke-dasharray="5 4"/>
<text x="172" y="341" font-size="11" fill="#111827" font-weight="600">指令执行器（src/rv32 的 SystemRv32）</text>
<text x="172.0" y="358.0" font-size="8.5" fill="#475569">RV32IMC，只支持 M 态，实现 M 态 CSR，不支持 S / U / H；fence 实现为 nop</text>
<text x="172.0" y="371.5" font-size="8.5" fill="#475569">覆盖 Decode 接入 custom-0 自定义指令：</text>
<text x="172.0" y="385.0" font-size="8.5" fill="#475569">　dsar / dsari 读 DSA 寄存器（不会被阻塞）</text>
<text x="172.0" y="398.5" font-size="8.5" fill="#475569">　dsaw.s / dsaw.d / dsawi.s / dsawi.d 写 DSA 寄存器</text>
<text x="172.0" y="412.0" font-size="8.5" fill="#475569">　task_done（带 TS 标志位）· flag_check · loop</text>
<text x="172.0" y="425.5" font-size="8.5" fill="#475569">每条指令 1 拍；访存与 DSA 读的延迟记在 gpr 就绪表上</text>
<text x="172.0" y="439.0" font-size="8.5" fill="#475569">不建流水线：pc_gen / loop_bp / decode / dispatch / 双发射 /</text>
<text x="172.0" y="452.5" font-size="8.5" fill="#475569">　gpr 端口 / SEU 的乘除多拍 / DTCM 的 bank 冲突都折算成 1 拍</text>
<text x="172.0" y="466.0" font-size="8.5" fill="#475569">复位后按 io_reg 的 boot_pc 启动；收到 task 后按 task_pc 起始执行</text>
<text x="172.0" y="479.5" font-size="8.5" fill="#475569">task_done：队列有待执行 task 则跳到队头 task 起始 PC，</text>
<text x="172.0" y="493.0" font-size="8.5" fill="#475569">　否则阻塞取指等待；带 TS 标志时通知 TS</text>
<rect x="780" y="320" width="250" height="100.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="792" y="341" font-size="11" fill="#111827" font-weight="600">gpr 就绪表</text>
<text x="792.0" y="358.0" font-size="8.5" fill="#475569">32 × 32 bit</text>
<text x="792.0" y="371.5" font-size="8.5" fill="#475569">DSA 读返回与访存返回未到时</text>
<text x="792.0" y="385.0" font-size="8.5" fill="#475569">　把对应寄存器标为未就绪</text>
<text x="792.0" y="398.5" font-size="8.5" fill="#475569">读到未就绪的源就等</text>
<rect x="1160" y="320" width="200" height="100.0" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1172" y="341" font-size="11" fill="#111827" font-weight="600">CSR</text>
<text x="1172.0" y="358.0" font-size="8.5" fill="#475569">M 态 CSR + 自定义 CSR</text>
<text x="1172.0" y="371.5" font-size="8.5" fill="#475569">stream_id 只读（4 bit）</text>
<text x="1172.0" y="385.0" font-size="8.5" fill="#475569">local_user_id 可读写（12 bit）</text>
<text x="1172.0" y="398.5" font-size="8.5" fill="#475569">由 ctrl_noc 直接配置，不经流水线</text>
<rect x="160" y="584.5" width="320" height="167.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="172" y="605.5" font-size="11" fill="#111827" font-weight="600">dsa_iss / dsa_rq</text>
<text x="172.0" y="622.5" font-size="8.5" fill="#475569">dsa_iss：DSA 调用指令下发通道</text>
<text x="172.0" y="636.0" font-size="8.5" fill="#475569">　每拍最多一条配置或 trigger 指令</text>
<text x="172.0" y="649.5" font-size="8.5" fill="#475569">　按下发通道是否反压阻塞判断是否下发成功</text>
<text x="172.0" y="663.0" font-size="8.5" fill="#475569">dsa_rq：8 项，按顺序记录已下发的读指令</text>
<text x="172.0" y="676.5" font-size="8.5" fill="#475569">　返回数据后按记录的目的寄存器编号写回 gpr</text>
<text x="172.0" y="690.0" font-size="8.5" fill="#475569">DSA 读寄存器指令不支持同步读返回，</text>
<text x="172.0" y="703.5" font-size="8.5" fill="#475569">　软件要查询状态只能轮询</text>
<text x="172.0" y="717.0" font-size="8.5" fill="#475569">任务启动靠写 DSA 的 trigger 寄存器；</text>
<text x="172.0" y="730.5" font-size="8.5" fill="#475569">　last 标志包含在 trigger 里</text>
<rect x="560" y="584.5" width="520" height="167.5" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="572" y="605.5" font-size="11" fill="#111827" font-weight="600">lsq（顺序发射）</text>
<text x="572.0" y="622.5" font-size="8.5" fill="#475569">sm_lsq 16 项：Share Mem，5～10 拍</text>
<text x="572.0" y="636.0" font-size="8.5" fill="#475569">cm_lsq 16 项：Core Mem，15～25 拍</text>
<text x="572.0" y="649.5" font-size="8.5" fill="#475569">　只有 DTE core 有；Router I/O reg 复用它</text>
<text x="572.0" y="663.0" font-size="8.5" fill="#475569">DTCM：4 bank 单端口 SRAM，3 拍</text>
<text x="572.0" y="676.5" font-size="8.5" fill="#475569">　可同时接收 2 个不冲突 bank 的请求</text>
<text x="572.0" y="690.0" font-size="8.5" fill="#475569">　同 bank 冲突则阻塞第二条</text>
<text x="572.0" y="703.5" font-size="8.5" fill="#475569">访存带宽 32 bit</text>
<text x="572.0" y="717.0" font-size="8.5" fill="#475569">写回优先级：share_mem / core_mem 优先于 DTCM</text>
<text x="572.0" y="730.5" font-size="8.5" fill="#475569">DTE core 读 Core Mem 固定回 1056 bit，不 burst</text>
<polygon points="269,792.0 380,792.0 371,824.0 260,824.0" fill="#f8fafc" stroke="#374151"/>
<text x="320.0" y="807.0" font-size="9" fill="#374151" text-anchor="middle">dsa_cfg / dsa_rdata</text>
<text x="320.0" y="818.0" font-size="7.5" fill="#6b7280" text-anchor="middle">→ DSA</text>
<polygon points="769,792.0 880,792.0 871,824.0 760,824.0" fill="#f8fafc" stroke="#374151"/>
<text x="820.0" y="807.0" font-size="9" fill="#374151" text-anchor="middle">io_reg</text>
<text x="820.0" y="818.0" font-size="7.5" fill="#6b7280" text-anchor="middle">→ Router</text>
<polygon points="1289,598.5 1400,598.5 1391,630.5 1280,630.5" fill="#f8fafc" stroke="#374151"/>
<text x="1340.0" y="613.5" font-size="9" fill="#374151" text-anchor="middle">sm_lsq</text>
<text x="1340.0" y="624.5" font-size="7.5" fill="#6b7280" text-anchor="middle">→ Share Mem</text>
<polygon points="1289,706.0 1400,706.0 1391,738.0 1280,738.0" fill="#f8fafc" stroke="#374151"/>
<text x="1340.0" y="721.0" font-size="9" fill="#374151" text-anchor="middle">cm_lsq</text>
<text x="1340.0" y="732.0" font-size="7.5" fill="#6b7280" text-anchor="middle">→ Core Mem</text>
<path d="M295.6 77.0 L299.9 109.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M300.0 237.0 L300.0 319.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="302.8" y="229.4" width="10.5" height="98.1" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 308 278.5)" x="308" y="281.5" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">队头 task 的 task_pc</text>
<path d="M560.0 320.0 L555.5 77.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)"/>
<rect x="562.8" y="148.5" width="10.5" height="138.9" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 568 218.0)" x="568" y="221.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">rv_done 由 task_done 指令产生</text>
<path d="M721.0 378.2 L779.0 370.1" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="734.2" y="364.9" width="31.5" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="750.0" y="372.35" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">就绪位</text>
<path d="M327.9 515.5 L320.1 583.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="322.8" y="499.1" width="10.5" height="100.9" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 328 549.5)" x="328" y="552.5" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">dsa 指令 / 读回写 gpr</text>
<path d="M320.1 753.0 L324.4 791.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M636.0 515.5 L636.0 583.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="638.8" y="538.0" width="10.5" height="23.0" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 644 549.5)" x="644" y="552.5" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">访存</text>
<path d="M1081.0 614.5 L1283.5 614.5" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="1110.4" y="601.0" width="143.7" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1182.25" y="608.5" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">sm_lsq 16 项，32 bit，5～10 拍</text>
<path d="M1081.0 722.0 L1283.5 722.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="1110.4" y="708.5" width="143.7" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1182.25" y="716.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">cm_lsq 16 项，只有 DTE core 有</text>
<path d="M820.1 753.0 L824.4 791.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<path d="M1260.0 251.5 L1260.0 300.0 L664.0 300.0 L664.0 319.0" stroke="#475569" stroke-width="1.3" fill="none" stroke-linejoin="round" marker-end="url(#a)" marker-start="url(#as)"/>
<rect x="961.0" y="288.5" width="78.1" height="10.5" fill="#ffffff" opacity="0.92"/>
<text x="1000" y="296" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#475569" text-anchor="middle">取指 8 B/T，1 拍</text>
<path d="M1255.5 74.0 L1259.9 109.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<rect x="1262.8" y="81.5" width="10.5" height="23.0" fill="#ffffff" opacity="0.92"/>
<text transform="rotate(-90 1268 93.0)" x="1268" y="96.0" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" font-size="8.5" fill="#7c3aed" text-anchor="middle">装载</text>
<path d="M1160.0 370.0 L1100.0 370.0 L1100.0 312.0 L703.2 312.0 L703.2 319.0" stroke="#7c3aed" stroke-width="1.3" fill="none" stroke-linejoin="round" stroke-dasharray="4 3" marker-end="url(#p)"/>
<text x="20" y="892" font-size="10.5" fill="#374151" text-anchor="start">三个实例的区别只在绑定的 DSA、ITCM 里的镜像、以及可见的地址空间：只有 DTE core 有 cm_lsq 与 Router I/O reg，Matrix Mem 对三个 RV core 都不可见。</text>
<text x="20" y="914" font-size="10.5" fill="#374151" text-anchor="start">firmware 程序结束时要执行一条不通知 TS 的 task_done，等待业务流 task。gpr 就绪表承载访存与 DSA 读的延迟。CSR 由 ctrl_noc 直接配置。</text>
</svg>
```

***

## 2　功能清单

一功能一条，编号供“机制覆盖”一章引用。

### task_queue 与 task 下发

| 编号 | 功能 |
| - | - |
| F1 | 提前接收 TS 下发的 task，前一个 task 完成后立刻执行队头缓存的那个，做到用户之间 task 的无 bubble 调度 |
| F2 | 按 task_queue 是否有空槽产生 `task_ack`；未被接收时 TS 不能释放该 task 跳到下一个 |
| F3 | 下发信息六个字段：`task_pc` 是起始取指 PC；`stream_id` 4 bit，用于算该用户的 Core Mem 与 Share Mem 区域基址，硬件写入自定义 CSR 且只读；`local_user_id` 12 bit，用于 R core 用户映射表和 Matrix Mem 地址计算，可读写，R core 执行 `flag_check` 后由软件写入；`task_id` 6 bit 与 `user_id` 16 bit 由 TS 从 stream_table 取出一起下发，硬件写入自定义 CSR；`task_dsa_en` |
| F4 | `local_user_id` 与 `user_id` **互不相干，不存在换算关系**。`user_id` 16 bit 是全局编号，Router 与 credit 记账认它；`local_user_id` 12 bit 是 core 内部软件自己编的号，只用于 R core 的用户映射表与 Matrix Mem 地址计算。两者各走各的，硬件不做任何转换 |
| F5 | 自启动的 B core 与 R core 反过来：TS 下发时没有用户身份，软件在 `flag_check` 认出这一笔属于哪个用户后，把 12 位 `local_user_id` 写进自定义 CSR，随 `task_done` 经 `rv_done` 回 TS，由 TS 的 `completion` 写口补进 stream_table。`rv_done` 不带 `user_id`，也没有专用的 bind 通路 |
| F6 | 完成信息三个字段：`stream_id`、`local_user_id`、`task_id`。`task_id` 只读，异步 datain 任务是例外，由软件识别包头后写入，用于告诉 TS 是任务链中哪一步完成 |
| F7 | RV core **不向 DSA 输出任务身份**。DSA 报 `dsa_done` 时填的 `stream_id` 与 `task_id`，来自软件在启动这一笔 DSA 任务之前写进 DSA 自己寄存器的值，三个 DSA 一样：DTE 写 `TASK_CFG_PACK`，MU 与 VU 写各自的动态配置寄存器。RV core 这一侧只把 TS 下发的 `stream_id` / `task_id` / `user_id` 放进自定义 CSR 供软件读，读出来再写给 DSA |

### 指令执行

| 编号 | 功能 |
| - | - |
| F8 | 指令集 RV32IMC：I 基本指令集、M 整型乘除法、C 压缩指令集；不支持 F 与 D，A 考虑支持 |
| F9 | 特权级只支持 M 态，实现 M 态 CSR，不支持 S / U / H；`fence` 指令实现为 nop |
| F10 | 每条指令 1 拍。不建流水线：pc_gen、loop_bp、decode、dispatch、双发射、gpr 端口、SEU 的乘除多拍、DTCM 的 bank 冲突都折算进这 1 拍 |
| F11 | 复位后按 io_reg 的 `boot_pc` 启动；收到 TS 下发的 task 后按 `task_pc` 起始执行 |
| F12 | 自定义指令 `dsar` / `dsari`：读 DSA 寄存器，地址分别来自 rs1 与立即数 `reg_addr1[4:0]` |
| F13 | 自定义指令 `dsaw.s` / `dsawi.s`：写 1 个 DSA 寄存器；`dsaw.d` / `dsawi.d` 一次写 2 个。按 RV Core MAS 的“每条最多配置 1 个 DSA 寄存器”建模，`dsawi.d` 先当两条 `dsawi.s` |
| F14 | 自定义指令 `task_done`：通知当前 task 完成。队列有待执行 task 则跳转到队头 task 起始 PC，否则阻塞取指等待；带 `TS` 标志时通知 TS。firmware 程序结束时要执行一条不通知 TS 的 `task_done`，等待业务流 task |
| F15 | 自定义指令 `flag_check`：从 Share Mem 的起始地址查到结束地址，找第一个 1 并把位置偏移量写回 rd，查到结束地址仍没找到则返回全 1。B core 与 R core 轮询软件映射表靠它 |
| F16 | 这是唯一一条不止 1 拍的指令：它按 4 B 一步扫，复用 `sm_lsq` 每拍发一个 Share Mem 读，找到第一个 1 就停。拍数 = 实际扫过的步数 + Share Mem 的一次访问延迟，最坏是 `ceil(扫描长度 / 4 B) + 10`。扫描期间该 RV core 不取下一条指令，`gpr_ready[rd]` 保持为 0 |
| F17 | 自定义指令 `loop`：rs1 是最大循环次数、rs2 是当前循环次数，rs2 ≥ rs1 时退出循环，imm 是分支偏移 |
| F18 | 寄存器分静态配置与动态配置：静态配置基本不随用户变化，初始化阶段配好、业务流阶段快速调用；动态配置随用户变化，跟随任务下发，含静态配置的选择 |
| F19 | 任务的启动靠写 DSA 的 trigger 寄存器；last 标志（该任务包是 task 的最后一个，DSA 执行完后通知 TS task 完成）包含在 trigger 寄存器里 |
| F20 | 性能约束：单个用户各 DSA 对应的软件调度程序在 RV core 上执行时间不超过 200 cycle |

### 指令完成标识

RV Core 顺序派遣、没有 ROB 重排序，会出现乱序写回，所以每条指令要分别定义完成点。

| 编号 | 功能 |
| - | - |
| F20a | 理论完成标识：指令派遣后经过不再会被 cancel 的那一点（派遣后一拍），用来更新 CSR 的指令计数器 |
| F20b | 实际完成标识：指令的活真正做完的那一点，有依赖的指令据此认到数据已更新，验证也按这一点采样 |
| F20c | 各类指令的实际完成点：通用寄存器写回指令为写入目的寄存器；CSR 写回指令为写入 CSR；PC 更新指令为产生 PC 更新（分支预测正确时 PC 在预测阶段就更新，实际完成点是派遣下一拍在 ALU 执行完）；mem 写入指令为真正写进对应 mem |
| F20d | 五类特殊指令的实际完成点：`FENCE` / `FENCE.I` 在派遣阶段收到前序全部访存完成标识后；`ECALL` / `EBREAK` 在译码阶段检出并产生异常后；`task_done` 在译码阶段检出并通知取指切 task 后；`dsaw` / `dsawi` 在产生配置 DSA 的写请求后；`dsar` / `dsari` 在读 DSA 寄存器写回通用目的寄存器后 |
| F20e | 一条指令可以同时需要上面多个完成标识 |

### dsa_iss 与 dsa_rq

| 编号 | 功能 |
| - | - |
| F21 | dsa_iss 每拍最多下发一条配置或 trigger 指令 |
| F22 | DSA 任务配置下发指令按下发通道是否反压阻塞判断是否下发成功 |
| F23 | DSA 读寄存器指令不会被阻塞 |
| F24 | dsa_rq 8 项，按顺序记录已下发的读指令信息，返回数据后按记录的目的寄存器编号写回 gpr |
| F25 | DSA 读寄存器指令不支持同步读返回，软件要查询状态只能轮询 |

### gpr 就绪表

| 编号 | 功能 |
| - | - |
| F26 | 32 × 32 bit 的通用寄存器；DSA 读返回与访存返回未到时把对应寄存器标为未就绪 |
| F27 | 指令读到未就绪的源寄存器就等，等到写回才继续。访存与 DSA 读的延迟就记在这张表上 |

### lsq 与访存分流

| 编号 | 功能 |
| - | - |
| F28 | 按地址范围把访存分到四个目标：DTCM、Share Mem、Core Mem、Router I/O reg |
| F29 | DTCM 是 4 bank 单端口 SRAM、8 KB、32 bit × 4 bank，3 拍；可同时接收 2 个不冲突 bank 的请求，同 bank 冲突则阻塞第二条 |
| F30 | `sm_lsq` 16 项，访问 Share Mem，5～10 拍，顺序执行，每拍仅发一个读或写请求 |
| F31 | `cm_lsq` 16 项，访问 Core Mem，15～25 拍，顺序执行，每拍仅发一个请求；只有 DTE core 有 |
| F32 | Router I/O reg 复用 `cm_lsq`，仅 DTE core 需要 |
| F33 | 访存带宽 32 bit |
| F34 | 写回优先级：DTCM 读出数据与 Share Mem / Core Mem 数据同时需写回时，优先写回 Share Mem / Core Mem，阻塞 DTCM |
| F35 | DTE core 访问 Core Mem 的接口与其余通路不同：一次读请求固定读回 1056 bit，不支持 burst，按 32 bit / 拍返回；地址 18 bit，4 B 粒度；写请求带 4 bit 字节使能 |
| F35a | 读写<b>不同</b>数据缓存时，请求与返回都可以乱序发出 |
| F35b | 读写<b>同一</b>数据缓存时：地址相关必须顺序发出请求与返回；地址无关允许乱序 |

### 存储与配置

| 编号 | 功能 |
| - | - |
| F36 | ITCM 4 KB，8 B/T，1 拍，存 firmware、kernel、DTE core 的 bootloader |
| F37 | DTCM 8 KB，存初始化 BSS 数据段、寄存器溢出与堆栈 |
| F38 | ITCM 与 DTCM 由 ctrl_noc 装载，装载拍数按镜像字节数除以 4 B 计 |
| F39 | CSR 由 ctrl_noc 直接配置，不经流水线 |
| F40 | 三个实例可见的地址空间：各自的 ITCM、DTCM、Share Mem、Core Mem 与对应 DSA 的 IO reg；Matrix Mem 对三个 RV core 都不可见 |
| F41 | 异常九类（Load / Store ECC Error、Load / Store Address Misaligned、Load / Store Access Fault、Fetch Address Misaligned、Fetch Access Fault、Fetch ECC Error、Illegal Instruction、Environment Call、Breakpoint），本轮只留状态位与接口名，不实现行为 |

***

## 3　接口

```
port task_cmd (slave, valid/ready, clk)           // TS → RV core：task 下发
  in  cmd_valid · task_pc[31:0] · stream_id[3:0] · local_user_id[11:0] · task_id[5:0] · user_id[15:0] · task_dsa_en
  out cmd_ready                                     // = task_queue 有空槽（raw ACCEPT）
port rv_done (master, 脉冲, clk)                  // RV core → TS：task_done 指令带 TS 标志时产生
  out valid · stream_id[3:0] · local_user_id[11:0] · task_id[5:0]
port dsa_cfg (master, valid/ready, clk)           // dsa_iss → 对应 DSA：配置写与 trigger
  out req_valid · req_we · req_addr[11:0] · req_wdata[31:0]
  in  req_ready                                     // = DSA 的配置通路未反压
port dsa_rdata (slave, 脉冲, clk)                 // DSA → dsa_rq：读寄存器的返回，异步
  in  valid · rdata[31:0]
port sm_lsq (master, valid/ready, clk)            // → Share Mem，32 bit
  out req_valid · req_we · req_addr[14:0] · req_wdata[31:0] · req_be[3:0]
  in  req_ready · rsp_valid · rsp_rdata[31:0]
port cm_lsq (master, valid/ready, clk)            // → Core Mem 与 Router I/O reg，只有 DTE core 有
  out req_valid · req_we · req_addr[17:0] · req_wdata[31:0] · req_be[3:0]
  in  req_ready · rsp_valid · rsp_rdata[1055:0]     // 读固定回 1056 bit，按 32 bit / 拍取用
port cfg (slave, ctrl_noc 写事务, clk)            // ITCM / DTCM 装载与 CSR 配置
  in  cfg_valid · cfg_addr[23:0] · cfg_we · cfg_wdata[31:0]
  out cfg_rdata[31:0]
port ready (master, 电平, clk)                    // 进 wait 状态后拉高，SCP 据此开放业务接收
  out ready
```

***

## 4　存储器

```
mem itcm        SRAM        4 KB，8 B/T，1 拍                                  1R1W  ctrl_noc 装载        复位未定义   // firmware · kernel · bootloader
mem dtcm        SRAM        8 KB，4 bank × 32 bit，3 拍                        1R1W  同 bank 冲突阻塞第二条  复位未定义   // BSS · 寄存器溢出 · 堆栈
mem gpr         FF 阵列     32 × 32 bit                                        —     由指令执行器读写      复位 0
mem gpr_ready   FF          32 b 就绪位图                                       1RW   发出访存 / DSA 读时清，写回时置  复位 全 1
mem task_q      FIFO        深 2 × {task_pc[31:0], stream_id[3:0], local_user_id[11:0], task_id[5:0], user_id[15:0], task_dsa_en}  1W1R  满 → task_ack 拉低  复位空
mem dsa_rq      FIFO        8 × {rd_idx[4:0]}                                  1W1R  顺序记录，返回时按序写回 gpr  复位空
mem sm_lsq      FIFO        16 × {we, addr[14:0], wdata[31:0], rd_idx[4:0]}     1W1R  顺序发射，每拍一个    复位空
mem cm_lsq      FIFO        16 × {we, addr[17:0], wdata[31:0], be[3:0], rd_idx[4:0]}  1W1R  顺序发射，每拍一个；只有 DTE core 有  复位空
mem csr         FF 阵列     M 态 CSR + 自定义 CSR                              1R1W  stream_id 只读，local_user_id 可读写  复位 0
mem pc          FF          {pc[31:0], state[2:0]}                             1RW   复位取 boot_pc；收 task 取 task_pc  复位 boot_pc
```

### 编译侧读入的 kernel 镜像

每类 core 一个 RV32 ELF，代码段进 ITCM、数据段进 DTCM，boot 期经 ctrl_noc 装载。配套一张 `task_pc` 表（64 项），把 TS 的 `task_chain` 里每一项的 `TASK_PC` 指到该 kernel 的一个入口地址。

kernel 清单按 RV core 分：

| kernel | 跑在哪个 RV core | 做什么 | 谁向 TS 报完成 |
| - | - | - | - |
| `weights_loader` | DTE core | 用标量指令算出这一片权重落 Matrix Mem 的地址，再发 DTE 指令把数据从 Router 搬过去 | DTE DSA |
| `bcore_datain` | DTE core | 把 Share Mem 里 `head` 指的槽位地址配给 DTE DSA，DSA 搬完置 `head = head + 1` | DTE DSA |
| `broadcast` | DTE core | 把 `tail` 指的槽位配给 DTE DSA 搬到 Router，搬完置 `tail = tail + 1` | DTE DSA |
| `check_flag` | B core 上是 VU core，R core 上是 MU core | B core 上循环比较 `head` 与 `tail`；R core 上循环扫 `arrive_num` 找等于 2 的项，找到就清零并写 `tmp_info1[stream_id]` | RV core 自己 |
| `token_datain` | DTE core | 按包头判断这是任务链里哪一步的数据，把 Router buffer 里的数据配给 DTE DSA 搬进 Core Mem | DTE DSA |
| `dataout` | DTE core | 按 `task_id` 查 `path_id` 与 `size` 改写硬件包头，把 Core Mem 里的结果配给 DTE DSA 搬到 Router | DTE DSA |
| `mu_gemv` | MU core | 判断 8 个激活专家里哪些落在本 EP 组、挑出加权权重、配好 Mmem 与 Cmem 地址，写 `dsawi.d topk_stream_stride, trigger` 启动 | MU DSA |
| `vu_macro` | VU core | 写 12 个动态参数寄存器，再写 `macro_inst_trigger` | VU DSA |

指令预算按 RV core 主频 1 GHz、目标 5 MTPS 摊：一个 token 总共 200 T 指令，DTE 每 task 约 33 T、MU 约 100 T、VU 约 66 T。MU 那 100 T 是最紧的一格。

***

## 5　流水线总览

本模块不建流水线，第 1 层图画的是四段的拍数记账关系：取 task、执行、异步返回、报完成。执行段每条指令 1 拍，两条异步返回段的拍数由被访问方给。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 678 436" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arvov" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="678" height="436" fill="#ffffff"/>

  <text x="20" y="26" font-size="12" fill="#111827">RV core · 第 1 层流水线总览（不建流水线，每条指令 1 拍，延迟记在 gpr 就绪表上）</text>
  <text x="20" y="42" font-size="9.5" fill="#6b7280">横向是级序，不是拍序；每级的拍数在右上角 Dx。橙色虚线框是变长级，非按比例。</text>
  <path d="M150 52 L150 70" stroke="#e5e7eb" fill="none"/>
<path d="M150 126 L150 328" stroke="#e5e7eb" fill="none"/>
  <path d="M316 52 L316 156" stroke="#e5e7eb" fill="none"/>
<path d="M316 212 L316 328" stroke="#e5e7eb" fill="none"/>
  <path d="M482 52 L482 156" stroke="#e5e7eb" fill="none"/>
<path d="M482 212 L482 328" stroke="#e5e7eb" fill="none"/>
  <text x="20" y="102" font-size="10.5" fill="#6b7280">取 task</text>
  <rect x="150" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="84" font-size="8.5" fill="#6b7280">M1</text>
  <text x="292" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="104" font-size="11" fill="#111827">task_queue 出队</text>
  <text x="160" y="118" font-size="11" fill="#111827">身份写自定义 CSR</text>
  <text x="20" y="188" font-size="10.5" fill="#6b7280">执行</text>
  <rect x="150" y="156" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="160" y="170" font-size="8.5" fill="#92400e">M2</text>
  <text x="292" y="170" font-size="8.5" fill="#92400e" text-anchor="end">D1/指令</text>
  <text x="160" y="190" font-size="11" fill="#7c2d12">取指与执行</text>
  <rect x="316" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="170" font-size="8.5" fill="#6b7280">M3</text>
  <text x="458" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="190" font-size="11" fill="#111827">dsa_iss 下发</text>
  <path d="M300 184 L315 184" stroke="#475569" marker-end="url(#arvov)" fill="none"/>
  <rect x="482" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="492" y="170" font-size="8.5" fill="#6b7280">M6</text>
  <text x="624" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="492" y="190" font-size="11" fill="#111827">task_done</text>
  <path d="M466 184 L481 184" stroke="#475569" marker-end="url(#arvov)" fill="none"/>
  <text x="20" y="274" font-size="10.5" fill="#6b7280">异步返回</text>
  <rect x="150" y="242" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="160" y="256" font-size="8.5" fill="#92400e">M4</text>
  <text x="292" y="256" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="160" y="276" font-size="11" fill="#7c2d12">dsa_rq 写回 gpr</text>
  <rect x="316" y="242" width="150" height="56" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="326" y="256" font-size="8.5" fill="#92400e">M5</text>
  <text x="458" y="256" font-size="8.5" fill="#92400e" text-anchor="end">D变长</text>
  <text x="326" y="276" font-size="11" fill="#7c2d12">lsq 发射与返回</text>
  <path d="M300 270 L315 270" stroke="#475569" marker-end="url(#arvov)" fill="none"/>
  <text x="20" y="352" font-size="10.5" fill="#374151">M2 一拍一条：pc_gen、译码、双发射、乘除多拍、DTCM bank 冲突都折算进这一拍。</text>
  <text x="20" y="380" font-size="10.5" fill="#374151">M4 与 M5 的拍数由被访问方给：ITCM 1、DTCM 3、Share Mem 5～10、Core Mem 15～25、DSA 读寄存器由该 DSA 决定。</text>
  <text x="20" y="408" font-size="10.5" fill="#374151">M2 读到未就绪的源寄存器就原地等，等待时长就是 M4 或 M5 的拍数。</text>
</svg>
```

***

## 6　逐级行为

### M1 · task_queue 出队

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 932 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arv1" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="932" height="198" fill="#ffffff"/>

  <polygon points="30,33 188,33 178,109 20,109" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="52" font-size="10.5" fill="#374151" text-anchor="middle">task_cmd</text>
  <text x="104" y="70" font-size="9.5" fill="#6b7280" text-anchor="middle">cmd_valid · task_pc[31:0]</text>
  <text x="104" y="88" font-size="9.5" fill="#6b7280" text-anchor="middle">stream_id[3:0] · task_id[5:0]</text>
  <text x="104" y="106" font-size="9.5" fill="#6b7280" text-anchor="middle">user_id[15:0] · cmd_ready</text>
  <rect x="20" y="121" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="125" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="142" font-size="10" fill="#374151" text-anchor="middle">task_q · FIFO 2 项 · 1W1R</text>
  <rect x="736" y="28" width="176" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="736" y="28" width="176" height="18" fill="#334155"/>
  <text x="824" y="41" font-size="10.5" fill="#ffffff" text-anchor="middle">PC_ST</text>
  <text x="824" y="68" font-size="10" fill="#334155" text-anchor="middle">pc[31:0]</text>
  <text x="824" y="90" font-size="10" fill="#334155" text-anchor="middle">state[2:0]</text>
  <rect x="736" y="110" width="176" height="58" fill="#f1f5f9" stroke="#334155"/>
  <rect x="736" y="110" width="176" height="18" fill="#334155"/>
  <text x="824" y="123" font-size="10.5" fill="#ffffff" text-anchor="middle">CSR</text>
  <text x="824" y="146" font-size="10" fill="#334155" text-anchor="middle">stream_id · task_id</text>
  <text x="824" y="163" font-size="10" fill="#334155" text-anchor="middle">local_user_id · user_id</text>
  <rect x="232" y="20" width="460" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M1</text>
  <text x="678" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">task_queue · 取队头并置身份</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. cmd_ready = task_q.free &gt; 0；cmd_valid &amp;&amp; cmd_ready → task_q.push(cmd)</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. 前一个 task 执行完 → cur = task_q.pop()</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. pc = cur.task_pc；csr.stream_id = cur.stream_id（只读）</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. csr.{task_id, user_id, local_user_id} = cur 的对应字段，供软件读出后写给 DSA</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">提前接收让用户之间的切换无 bubble</text>
  <path d="M188 71 L231 71" stroke="#475569" marker-end="url(#arv1)" fill="none"/>
  <path d="M188 142 L231 142" stroke="#475569" marker-end="url(#arv1)" fill="none"/>
  <path d="M692 63 L735 63" stroke="#475569" marker-end="url(#arv1)" fill="none"/>
  <path d="M692 139 L735 139" stroke="#475569" marker-end="url(#arv1)" fill="none"/>
</svg>
```

### M2 · 取指与执行

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 872 272" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arv2" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="872" height="272" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">PC_ST</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">pc[31:0]</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">state[2:0]</text>
  <rect x="20" y="102" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="106" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="123" font-size="10" fill="#374151" text-anchor="middle">itcm · SRAM 4 KB · 1R</text>
  <rect x="20" y="156" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="160" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="177" font-size="10" fill="#374151" text-anchor="middle">gpr · FF 32×32b · 读</text>
  <rect x="20" y="210" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="214" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="231" font-size="10" fill="#374151" text-anchor="middle">gpr_ready · FF 32 b · 1R</text>
  <rect x="676" y="85" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="680" y="89" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="764" y="106" font-size="10" fill="#374151" text-anchor="middle">gpr · FF 32×32b · 写</text>
  <rect x="676" y="139" width="176" height="48" fill="#f1f5f9" stroke="#334155"/>
  <rect x="676" y="139" width="176" height="18" fill="#334155"/>
  <text x="764" y="152" font-size="10.5" fill="#ffffff" text-anchor="middle">PC_ST</text>
  <text x="764" y="179" font-size="10" fill="#334155" text-anchor="middle">pc[31:0]</text>
  <rect x="232" y="57" width="400" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="73" font-size="8.5" fill="#6b7280">M2</text>
  <text x="618" y="73" font-size="8.5" fill="#6b7280" text-anchor="end">D1/指令</text>
  <text x="250" y="93" font-size="12" fill="#111827">指令执行器 · src/rv32 逐条跑完</text>
  <text x="250" y="115" font-size="10.5" fill="#475569">1. inst = itcm[pc]</text>
  <text x="250" y="135" font-size="10.5" fill="#475569">2. src_ok = gpr_ready[rs1] &amp;&amp; gpr_ready[rs2]；!src_ok → pc 保持</text>
  <text x="250" y="155" font-size="10.5" fill="#475569">3. src_ok → SystemRv32.Run(inst)，直接读写 gpr、csr、访存</text>
  <text x="250" y="175" font-size="10.5" fill="#475569">4. pc = 分支成立 ? target : pc + (压缩指令 ? 2 : 4)</text>
  <text x="250" y="199" font-size="10" fill="#9ca3af">流水线细节全部折算进这一拍</text>
  <line x1="188" y1="55" x2="228" y2="55" stroke="#475569" marker-end="url(#arv2)"/>
  <path d="M188 123 L231 123" stroke="#475569" marker-end="url(#arv2)" fill="none"/>
  <path d="M188 177 L231 177" stroke="#475569" marker-end="url(#arv2)" fill="none"/>
  <line x1="188" y1="231" x2="228" y2="231" stroke="#475569" marker-end="url(#arv2)"/>
  <path d="M632 106 L675 106" stroke="#475569" marker-end="url(#arv2)" fill="none"/>
  <path d="M632 163 L675 163" stroke="#475569" marker-end="url(#arv2)" fill="none"/>
</svg>
```

### M3 · dsa_iss 下发

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 829 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arv3" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="829" height="198" fill="#ffffff"/>

  <rect x="20" y="48" width="168" height="48" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="48" width="168" height="18" fill="#334155"/>
  <text x="104" y="61" font-size="10.5" fill="#ffffff" text-anchor="middle">PC_ST</text>
  <text x="104" y="88" font-size="10" fill="#334155" text-anchor="middle">pc[31:0]</text>
  <rect x="20" y="108" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="112" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="129" font-size="10" fill="#374151" text-anchor="middle">dsa_rq · FIFO 8 项 · 1W</text>
  <polygon points="643,33 809,33 799,109 633,109" fill="#f8fafc" stroke="#374151"/>
  <text x="721" y="52" font-size="10.5" fill="#374151" text-anchor="middle">dsa_cfg</text>
  <text x="721" y="70" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · req_we</text>
  <text x="721" y="88" font-size="9.5" fill="#6b7280" text-anchor="middle">req_addr[11:0]</text>
  <text x="721" y="106" font-size="9.5" fill="#6b7280" text-anchor="middle">req_wdata[31:0] · req_ready</text>
  <rect x="633" y="121" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="637" y="125" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="721" y="142" font-size="10" fill="#374151" text-anchor="middle">gpr_ready · FF · 1W</text>
  <rect x="232" y="20" width="357" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M3</text>
  <text x="575" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">dsa_iss · 每拍最多一条配置或 trigger</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. 指令是 dsaw/dsawi → dsa_cfg = {we=1, addr, wdata}</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. req_ready=0 → 本条阻塞，pc 保持（DSA 配置通路满时反压）</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. 指令是 dsar/dsari → dsa_cfg = {we=0, addr}，不阻塞</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. dsar 同时 dsa_rq.push(rd)，并把 gpr_ready[rd] 清零</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">dsawi.d 按两条 dsawi.s 建</text>
  <path d="M188 72 L231 72" stroke="#475569" marker-end="url(#arv3)" fill="none"/>
  <path d="M188 129 L231 129" stroke="#475569" marker-end="url(#arv3)" fill="none"/>
  <path d="M589 71 L637 71" stroke="#475569" marker-end="url(#arv3)" fill="none"/>
  <path d="M589 142 L632 142" stroke="#475569" marker-end="url(#arv3)" fill="none"/>
</svg>
```

### M4 · dsa_rq 写回

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 812 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arv4" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="812" height="198" fill="#ffffff"/>

  <polygon points="30,51 188,51 178,91 20,91" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="70" font-size="10.5" fill="#374151" text-anchor="middle">dsa_rdata</text>
  <text x="104" y="88" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · rdata[31:0]</text>
  <rect x="20" y="103" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="107" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="124" font-size="10" fill="#374151" text-anchor="middle">dsa_rq · FIFO 8 项 · 1R</text>
  <rect x="616" y="51" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="620" y="55" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="704" y="72" font-size="10" fill="#374151" text-anchor="middle">gpr · FF · 1W</text>
  <rect x="616" y="105" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="620" y="109" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="704" y="126" font-size="10" fill="#374151" text-anchor="middle">gpr_ready · FF · 1W</text>
  <rect x="232" y="20" width="340" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M4</text>
  <text x="558" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="250" y="56" font-size="12" fill="#111827">dsa_rq · 按记录顺序写回 gpr</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. dsa_rdata.valid → idx = dsa_rq.pop()</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. gpr[idx] = dsa_rdata.rdata</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. gpr_ready[idx] = 1</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. 读寄存器不支持同步返回，软件要查状态只能轮询</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">拍数由对应 DSA 决定</text>
  <path d="M188 71 L231 71" stroke="#475569" marker-end="url(#arv4)" fill="none"/>
  <path d="M188 124 L231 124" stroke="#475569" marker-end="url(#arv4)" fill="none"/>
  <path d="M572 72 L615 72" stroke="#475569" marker-end="url(#arv4)" fill="none"/>
  <path d="M572 126 L615 126" stroke="#475569" marker-end="url(#arv4)" fill="none"/>
</svg>
```

### M5 · lsq 发射与返回

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 867 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arv5" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="867" height="198" fill="#ffffff"/>

  <rect x="20" y="24" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="28" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="45" font-size="10" fill="#374151" text-anchor="middle">sm_lsq · FIFO 16 项 · 1W1R</text>
  <rect x="20" y="78" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="82" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="99" font-size="10" fill="#374151" text-anchor="middle">cm_lsq · FIFO 16 项 · 1W1R</text>
  <rect x="20" y="132" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="136" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="153" font-size="10" fill="#374151" text-anchor="middle">dtcm · SRAM 8 KB 4 bank · 1R1W</text>
  <polygon points="681,42 847,42 837,100 671,100" fill="#f8fafc" stroke="#374151"/>
  <text x="759" y="61" font-size="10.5" fill="#374151" text-anchor="middle">sm_lsq / cm_lsq</text>
  <text x="759" y="79" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · req_addr</text>
  <text x="759" y="97" font-size="9.5" fill="#6b7280" text-anchor="middle">rsp_valid · rsp_rdata</text>
  <rect x="671" y="112" width="176" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="675" y="116" width="168" height="34" fill="none" stroke="#374151"/>
  <text x="759" y="133" font-size="10" fill="#374151" text-anchor="middle">gpr · FF · 1W</text>
  <rect x="232" y="20" width="395" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M5</text>
  <text x="613" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="250" y="56" font-size="12" fill="#111827">sm_lsq / cm_lsq · 顺序发射每拍一个</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. 按地址范围分流：DTCM / Share Mem / Core Mem / Router I/O reg</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. 每拍发队头一个请求，req_ready=0 则保持</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. rsp_valid → gpr[rd_idx] = rsp_rdata；gpr_ready[rd_idx] = 1</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. DTCM 与 Share Mem / Core Mem 同拍要写回时优先后者，阻塞 DTCM</text>
  <text x="250" y="162" font-size="10" fill="#9ca3af">DTE core 读 Core Mem 固定回 1056 bit，按 32 bit 逐拍取用</text>
  <path d="M188 45 L231 45" stroke="#475569" marker-end="url(#arv5)" fill="none"/>
  <path d="M188 99 L231 99" stroke="#475569" marker-end="url(#arv5)" fill="none"/>
  <path d="M188 153 L231 153" stroke="#475569" marker-end="url(#arv5)" fill="none"/>
  <path d="M627 71 L675 71" stroke="#475569" marker-end="url(#arv5)" fill="none"/>
  <path d="M627 133 L670 133" stroke="#475569" marker-end="url(#arv5)" fill="none"/>
</svg>
```

### M6 · task_done

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 947 218" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="arv6" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="947" height="218" fill="#ffffff"/>

  <rect x="20" y="20" width="168" height="70" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="168" height="18" fill="#334155"/>
  <text x="104" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">PC_ST</text>
  <text x="104" y="60" font-size="10" fill="#334155" text-anchor="middle">pc[31:0]</text>
  <text x="104" y="82" font-size="10" fill="#334155" text-anchor="middle">state[2:0]</text>
  <rect x="20" y="102" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="106" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="123" font-size="10" fill="#374151" text-anchor="middle">task_q · FIFO 2 项 · 1R</text>
  <rect x="20" y="156" width="168" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="160" width="160" height="34" fill="none" stroke="#374151"/>
  <text x="104" y="177" font-size="10" fill="#374151" text-anchor="middle">csr · FF · 1R</text>
  <polygon points="761,34 927,34 917,128 751,128" fill="#f8fafc" stroke="#374151"/>
  <text x="839" y="53" font-size="10.5" fill="#374151" text-anchor="middle">rv_done</text>
  <text x="839" y="71" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · stream_id[3:0]</text>
  <text x="839" y="89" font-size="9.5" fill="#6b7280" text-anchor="middle">local_user_id[11:0]</text>
  <text x="839" y="107" font-size="9.5" fill="#6b7280" text-anchor="middle">task_id[5:0]</text>
  <text x="839" y="125" font-size="9.5" fill="#6b7280" text-anchor="middle">user_id[15:0] · bind</text>
  <polygon points="761,140 927,140 917,180 751,180" fill="#f8fafc" stroke="#374151"/>
  <text x="839" y="159" font-size="10.5" fill="#374151" text-anchor="middle">ready</text>
  <text x="839" y="177" font-size="9.5" fill="#6b7280" text-anchor="middle">ready</text>
  <rect x="232" y="30" width="475" height="158" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="46" font-size="8.5" fill="#6b7280">M6</text>
  <text x="693" y="46" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="66" font-size="12" fill="#111827">task_done 指令 · 交还自己或通知 TS</text>
  <text x="250" y="88" font-size="10.5" fill="#475569">1. 带 TS 标志 → rv_done = {stream_id, local_user_id, task_id, user_id, bind}</text>
  <text x="250" y="108" font-size="10.5" fill="#475569">2. task_q 非空 → pc = 队头 task_pc，走 M1</text>
  <text x="250" y="128" font-size="10.5" fill="#475569">3. task_q 空 → state = wait，阻塞取指</text>
  <text x="250" y="148" font-size="10.5" fill="#475569">4. 三个 RV core 都进 wait → ready 拉高，SCP 才开放业务接收</text>
  <text x="250" y="172" font-size="10" fill="#9ca3af">firmware 结束时执行一条不通知 TS 的 task_done</text>
  <path d="M188 55 L231 55" stroke="#475569" marker-end="url(#arv6)" fill="none"/>
  <path d="M188 123 L231 123" stroke="#475569" marker-end="url(#arv6)" fill="none"/>
  <path d="M188 177 L231 177" stroke="#475569" marker-end="url(#arv6)" fill="none"/>
  <path d="M707 81 L755 81" stroke="#475569" marker-end="url(#arv6)" fill="none"/>
  <path d="M707 160 L755 160" stroke="#475569" marker-end="url(#arv6)" fill="none"/>
</svg>
```

***

## 7　参数汇总

```
指令集          RV32IMC，只支持 M 态；fence = nop
每条指令        1 拍（flag_check 例外，按实际扫过的步数记）
ITCM / DTCM     4 KB / 8 KB
gpr             32 × 32 bit
task_queue      深度 2（待定）
dsa_rq          8 项
sm_lsq / cm_lsq 16 / 16 项
访存延迟        ITCM 1 拍 · DTCM 3 拍 · Share Mem 5～10 拍 · Core Mem 15～25 拍
访存带宽        32 bit；DTE core 读 Core Mem 固定回 1056 bit，按 32 bit / 拍
dsa_iss         每拍最多一条配置或 trigger 指令
内部启动延迟     MU 40T · VU 40T · DTE 85T（含 core 发射 5T）
性能约束        单个用户各 DSA 对应的软件调度程序执行时间不超过 200 cycle
custom-0 字段布局   见下一节
```

### custom-0 的字段布局

照《ISA 描述表》的 `RV Core` 表。`opcode` 取 custom-0，即 `0b0001011`。九条都按 32 位定长排，`loop` 用 B 型，其余用 R 型的字段位置。

两个高位标志把同一个 `funct3` 下的几条分开：

| bit | 含义 |
| - | - |
| 31 | 0 = DSA 寄存器地址取自通用寄存器，1 = 地址是立即数 |
| 30 | 0 = 一次访问 1 个 DSA 寄存器，1 = 一次 2 个 |

| 助记符 | 31 | 30 | 29:25 | 24:20 | 19:15 | 14:12 | 11:7 |
| - | - | - | - | - | - | - | - |
| `dsar` | 0 | 0 | 00000 | 00000 | rs1 | 000 | rd |
| `dsari` | 0 | 0 | 00000 | 00000 | `reg_addr1[4:0]` | 000 | rd |
| `dsaw.s` | 0 | 0 | 00000 | 00000 | rs1 | 001 | rd1 |
| `dsaw.d` | 0 | 1 | rd2 | rs2 | rs1 | 001 | rd1 |
| `dsawi.s` | 1 | 0 | 00000 | 00000 | rs1 | 001 | `reg_addr1[4:0]` |
| `dsawi.d` | 1 | 1 | `reg_addr2[4:0]` | rs2 | rs1 | 001 | `reg_addr1[4:0]` |
| `task_done` | TS | FC | 00000 | 00000 | 00000 | 010 | 00000 |
| `flag_check` | 0 | 0 | 00001 | rs2 | rs1 | 010 | rd1 |

`loop` 不在上表里，它按 B 型排：`imm[12]` 在 bit31、`imm[10:5]` 在 bit[30:25]、rs2 在 bit[24:20]、rs1 在 bit[19:15]、`funct3` 110、`imm[4:1]` 与 `imm[11]` 在 bit[11:7]。

读上表要注意三处字段位置：

* **写 DSA 的那四条，DSA 寄存器地址在 bit[11:7]，数据源在 bit[19:15]**。这个位置在读指令上是目的寄存器 `rd`，在写指令上是地址，不是目的寄存器
* `dsari` 与 `dsawi` 的立即数是 5 位，正好等于一个寄存器号字段的宽度，直接占那个字段
* `task_done` 与 `flag_check` 同为 `funct3` 010，靠 bit[29:25] 分开：`00000` 是 `task_done`，`00001` 是 `flag_check`

两处与本模型的建法有出入，按下面处理：

* `task_done` 的 `FC` 位在编码里有，RV Core MAS 的 Features 已删除 FC 标志。模型解码这一位但不实现它的 fence 语义
* `dsaw.d` / `dsawi.d` 一次配 2 个 DSA 寄存器，与 RV Core MAS 的“每条最多配置 1 个”不一致。模型按 MAS 把它们展开成两条 `.s` 执行，编码照上表解码

**待定**：`dsar` 与 `dsari` 在表里的编码完全相同，bit31 都是 0，没有区分两者的位。按其余四条的规律，`dsari` 应当是 bit31 为 1。模型先按这条规律解码，等设计方确认。

***

## 8　机制覆盖

| 机制 | 功能 | 用例 |
| - | - | - |
| task_queue 提前接收，做到用户之间无 bubble 调度 | F1 | `task_queue_prefetch` |
| 按空槽产生 task_ack，未接收时 TS 不能跳到下一个 | F2 | `task_ack_handshake` |
| 下发六字段与完成三字段，stream_id 只读、local_user_id 可写 | F3、F6 | `task_fields` |
| local_user_id 与 user_id 互不相干，硬件不换算 | F4 | `two_user_ids` |
| 自启动 core 由软件写 local_user_id CSR，随 rv_done 回 TS | F5 | `self_start_writeback` |
| DSA 的身份由软件写进 DSA 寄存器，RV core 不输出 | F7 | `dsa_id_by_software` |
| RV32IMC + 只支持 M 态 + fence 为 nop | F8、F9 | `isa_scope` |
| 每条指令 1 拍，流水线细节折算进这 1 拍 | F10 | `one_cycle_per_inst` |
| 八条 custom-0 自定义指令 | F12～F17 | `custom0_insts` |
| dsawi.d 按两条 dsawi.s 建 | F13 | `dsaw_double` |
| task_done 的三种行为：跳队头 / 阻塞等待 / 通知 TS | F14 | `task_done_inst` |
| flag_check 查第一个 1，查不到返回全 1 | F15 | `flag_check` |
| flag_check 是唯一的多拍指令，拍数按实际扫过的步数记 | F17 | `flag_check_cycles` |
| loop 指令按 rs1 / rs2 比较退出 | F17 | `loop_inst` |
| 任务启动靠写 trigger 寄存器，last 标志在 trigger 里 | F19 | `dsa_trigger` |
| dsa_iss 每拍最多一条，按反压判断是否下发成功 | F21、F22 | `dsa_iss_rate` |
| DSA 读不阻塞，dsa_rq 8 项按序写回 gpr | F23、F24 | `dsa_read_async` |
| 读寄存器不支持同步返回，软件轮询 | F25 | `dsa_poll` |
| gpr 就绪表承载访存与 DSA 读的延迟 | F26、F27 | `gpr_ready` |
| 访存按地址范围分流到四个目标 | F28 | `lsu_routing` |
| DTCM 可同时收 2 个不冲突 bank，同 bank 阻塞第二条 | F29 | `dtcm_bank` |
| sm_lsq / cm_lsq 顺序发射，每拍一个请求 | F30、F31 | `lsq_inorder` |
| Router I/O reg 复用 cm_lsq，仅 DTE core 有 | F32 | `io_reg_access` |
| 写回优先 share_mem / core_mem，阻塞 DTCM | F34 | `writeback_priority` |
| DTE core 读 Core Mem 固定 1056 bit，不 burst，32 bit / 拍 | F35 | `rv_cm_read` |
| ITCM / DTCM 由 ctrl_noc 装载，拍数按字节数 / 4 B | F38 | `tcm_load` |
| Matrix Mem 对三个 RV core 都不可见 | F40 | `no_mmem_visibility` |
| 复位取 boot_pc，收 task 取 task_pc | F11 | `boot_and_task_pc` |

***

## 9　取舍

* **为什么设 task_queue**
  * TS 与 RV core 之间有物理路径延时，“前一个 task 完成再通知 TS 下发下一个”会产生很长延迟
  * 提前接收把这段延迟藏在前一个 task 的执行里
* **为什么不建流水线**
  * 本轮关心的是 core 内的调度与访存排队，不是标量核自身的 IPC
  * kernel 是真实 RV32 程序，指令条数是真的；每条 1 拍加上访存与 DSA 读的真实延迟，已经能反映“配置耗时是否小于计算耗时”这一条约束
  * 代价是双发射、分支预测、流水冲刷、乘除多拍、DTCM bank 冲突、gpr 端口竞争都不体现
* **为什么 DSA 读寄存器不阻塞而配置写会阻塞**
  * 配置写要占 DSA 的配置通路，通路满了只能等
  * 读只是取一个状态，用 dsa_rq 记下目的寄存器就能异步返回，不必占住发射口
