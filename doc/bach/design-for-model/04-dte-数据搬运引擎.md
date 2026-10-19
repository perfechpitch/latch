# DTE 数据搬运引擎

模式：design。层次：Core 内数据搬运。本文供搬运、软件和性能建模人员确定任务如何配置、搬运、完成和有序退休。

设计依据为原始库中的 [DTE DSA详细设计文档](</home/colin/develop/perfechpitch/Bach/05_五、详细设计/02_DTE DSA详细设计文档.md>)，同步基准 revision 24200。本文描述当前采用的详设基线；寄存器、lane、接口和验证细节在 [DTE 单元设计](07-units/chip/core/dte.md)。现有模型差异与原文未统一项分别登记，不用旧代码定义硬件行为。

## DTE 解决的问题

core 里 MU 算矩阵、VU 算向量。数据怎么进来、怎么出去、怎么在两块存储之间倒手，全归 DTE。它只做搬运，不做计算，职责是五件事：

* 接纳任务
* 生成访问命令
* 处理背压
* 追踪在途事务
* 向 TS 报告最终完成

难点在两端的节奏对不上：

* **Router 一侧**：流式到达，RX按valid/ready及head/tail接收；TX按每VC credit及head/tail发送，什么时候来由上游决定
* **存储一侧**：要过 DMA_XBAR 抢 bank，Core Mem 有 8 个 bank、Matrix Mem 有 64 个，命中冲突就得排队
* 把两端硬绑在一起，慢的一端会把快的一端拖住

DTE 的做法是**把一个搬运任务从中间劈开**：

* 读一半、写一半，各自排队各自推进
* 中间用 buffer 顶住速度差
* 完成以全局entry_id保持归属，WR完成回写后由SM/Ack按序退休

后面两节分别是这个“劈开”和这个“合上”。

支持五种搬运方向：

| Route | 走哪个通道 | 说明 |
| - | - | - |
| Router → MM | `in_ch` | 配置驱动：RV core 配 CFG + trigger，Commit 成对建立读写两侧 |
| Router → CM | `in_ch` | 同上 |
| MM → Router | `out_ch[n]`，n 由这条 path 的 VC 定 | 出口是 Router TX |
| CM → Router | `out_ch[n]`，n 由这条 path 的 VC 定 | 出口是 Router TX |
| MM → CM | `out_ch[3]` | 固定复用 VC3 那个出核通道，出口切到 `DMA WR1`，硬件 route mask 只允许 CoreMem |
| ~~CM → MM~~ | — | **本版本不支持**，XBar 不提供出核通道 write ctrl 到 Matrix Memory 的连接 |

每段执行连续一维搬运；CFG_STRIDEi描述不同SID的基址间距，不是段内多维步长。

模块组成与对外通道如下，布局与通道名照 MAS 的 Block Diagram 与 External Connections。

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="100%" viewBox="0.00 0.00 861.00 1160.00" role="img" aria-label="DTE 的模块组成与对外通道">
<title>DTE 的模块组成与对外通道</title>
<g id="diagram82ed021f8526-graph0" class="graph" transform="scale(1 1) rotate(0) translate(18 1141.52)">
<title>G</title>
<polygon fill="white" stroke="none" points="-18,18 -18,-1141.52 843.25,-1141.52 843.25,18 -18,18" />
<text xml:space="preserve" text-anchor="middle" x="412.62" y="-1104.32" font-family="Noto Sans CJK SC" font-size="16.00">DTE 的模块组成与对外通道</text>
<text xml:space="preserve" text-anchor="middle" x="412.62" y="-1080.32" font-family="Noto Sans CJK SC" font-size="16.00">RVCore IF与Config产生FullTaskDesc；Router、CM/MM、M Top、HeaderTable、ShareMem分别遵循各自接口。</text>

<g id="diagram82ed021f8526-node1" class="node">
<title>nrv</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M180.5,-1067.52C180.5,-1067.52 47.21,-1067.52 47.21,-1067.52 41.21,-1067.52 35.21,-1061.52 35.21,-1055.52 35.21,-1055.52 35.21,-1008.24 35.21,-1008.24 35.21,-1002.24 41.21,-996.24 47.21,-996.24 47.21,-996.24 180.5,-996.24 180.5,-996.24 186.5,-996.24 192.5,-1002.24 192.5,-1008.24 192.5,-1008.24 192.5,-1055.52 192.5,-1055.52 192.5,-1061.52 186.5,-1067.52 180.5,-1067.52" />
<text xml:space="preserve" text-anchor="middle" x="113.86" y="-1047.48" font-family="Noto Sans CJK SC" font-size="12.00">RVCore IF / AddrDecode</text>
<text xml:space="preserve" text-anchor="middle" x="113.86" y="-1029.48" font-family="Noto Sans CJK SC" font-size="12.00">请求与STUPV保持</text>
<text xml:space="preserve" text-anchor="middle" x="113.86" y="-1011.48" font-family="Noto Sans CJK SC" font-size="12.00">读Fire采样，下一拍返回</text>
</g>

<g id="diagram82ed021f8526-node2" class="node">
<title>ncfg</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M400.25,-933.24C400.25,-933.24 283.46,-933.24 283.46,-933.24 277.46,-933.24 271.46,-927.24 271.46,-921.24 271.46,-921.24 271.46,-873.96 271.46,-873.96 271.46,-867.96 277.46,-861.96 283.46,-861.96 283.46,-861.96 400.25,-861.96 400.25,-861.96 406.25,-861.96 412.25,-867.96 412.25,-873.96 412.25,-873.96 412.25,-921.24 412.25,-921.24 412.25,-927.24 406.25,-933.24 400.25,-933.24" />
<text xml:space="preserve" text-anchor="middle" x="341.86" y="-913.2" font-family="Noto Sans CJK SC" font-size="12.00">Config / Status</text>
<text xml:space="preserve" text-anchor="middle" x="341.86" y="-895.2" font-family="Noto Sans CJK SC" font-size="12.00">19字段、dirty、8模板</text>
<text xml:space="preserve" text-anchor="middle" x="341.86" y="-877.2" font-family="Noto Sans CJK SC" font-size="12.00">RUN / STOP / ERROR</text>
</g>

<g id="diagram82ed021f8526-edge1" class="edge">
<title>nrv-&gt;ncfg</title>
<path fill="none" stroke="#475569" d="M174.36,-995.78C205.01,-977.99 242.34,-956.33 273.98,-937.98" />
<polygon fill="#475569" stroke="#475569" points="274.99,-940.43 280.16,-934.4 272.36,-935.89 274.99,-940.43" />
<text xml:space="preserve" text-anchor="middle" x="259.82" y="-962.74" font-family="Noto Sans CJK SC" font-size="10.00">Config请求</text>
</g>

<g id="diagram82ed021f8526-node9" class="node">
<title>nht</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M171.13,-71.28C171.13,-71.28 56.59,-71.28 56.59,-71.28 50.59,-71.28 44.59,-65.28 44.59,-59.28 44.59,-59.28 44.59,-12 44.59,-12 44.59,-6 50.59,0 56.59,0 56.59,0 171.13,0 171.13,0 177.13,0 183.13,-6 183.13,-12 183.13,-12 183.13,-59.28 183.13,-59.28 183.13,-65.28 177.13,-71.28 171.13,-71.28" />
<text xml:space="preserve" text-anchor="middle" x="113.86" y="-51.24" font-family="Noto Sans CJK SC" font-size="12.00">HeaderTable</text>
<text xml:space="preserve" text-anchor="middle" x="113.86" y="-33.24" font-family="Noto Sans CJK SC" font-size="12.00">16组×32项，共800B</text>
<text xml:space="preserve" text-anchor="middle" x="113.86" y="-15.24" font-family="Noto Sans CJK SC" font-size="12.00">软件口与通道口仲裁</text>
</g>

<g id="diagram82ed021f8526-edge15" class="edge">
<title>nrv-&gt;nht</title>
<path fill="none" stroke="#475569" d="M108.28,-995.78C102.06,-956.59 92.86,-898.6 92.86,-898.6 92.86,-898.6 92.86,-898.6 92.86,-168.92 92.86,-168.92 100.81,-118.8 106.93,-80.26" />
<polygon fill="#475569" stroke="#475569" points="109.46,-81.05 108.05,-73.24 104.28,-80.23 109.46,-81.05" />
<text xml:space="preserve" text-anchor="middle" x="112.36" y="-531.76" font-family="Noto Sans CJK SC" font-size="10.00">软件读写</text>
</g>

<g id="diagram82ed021f8526-node3" class="node">
<title>nq</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M543.88,-798.96C543.88,-798.96 421.84,-798.96 421.84,-798.96 415.84,-798.96 409.84,-792.96 409.84,-786.96 409.84,-786.96 409.84,-739.68 409.84,-739.68 409.84,-733.68 415.84,-727.68 421.84,-727.68 421.84,-727.68 543.88,-727.68 543.88,-727.68 549.88,-727.68 555.88,-733.68 555.88,-739.68 555.88,-739.68 555.88,-786.96 555.88,-786.96 555.88,-792.96 549.88,-798.96 543.88,-798.96" />
<text xml:space="preserve" text-anchor="middle" x="482.86" y="-778.92" font-family="Noto Sans CJK SC" font-size="12.00">Glb Cfg TaskQ</text>
<text xml:space="preserve" text-anchor="middle" x="482.86" y="-760.92" font-family="Noto Sans CJK SC" font-size="12.00">16项FullTaskDesc</text>
<text xml:space="preserve" text-anchor="middle" x="482.86" y="-742.92" font-family="Noto Sans CJK SC" font-size="12.00">Tail / Ch0..4 / SM / Ack</text>
</g>

<g id="diagram82ed021f8526-edge2" class="edge">
<title>ncfg-&gt;nq</title>
<path fill="none" stroke="#475569" d="M379.27,-861.5C397.74,-844.17 420.13,-823.16 439.37,-805.12" />
<polygon fill="#475569" stroke="#475569" points="440.98,-807.21 444.65,-800.16 437.39,-803.38 440.98,-807.21" />
<text xml:space="preserve" text-anchor="middle" x="452.21" y="-828.46" font-family="Noto Sans CJK SC" font-size="10.00">Trigger快照入队</text>
</g>

<g id="diagram82ed021f8526-node4" class="node">
<title>nrd</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M341.13,-664.68C341.13,-664.68 208.59,-664.68 208.59,-664.68 202.59,-664.68 196.59,-658.68 196.59,-652.68 196.59,-652.68 196.59,-605.4 196.59,-605.4 196.59,-599.4 202.59,-593.4 208.59,-593.4 208.59,-593.4 341.13,-593.4 341.13,-593.4 347.13,-593.4 353.13,-599.4 353.13,-605.4 353.13,-605.4 353.13,-652.68 353.13,-652.68 353.13,-658.68 347.13,-664.68 341.13,-664.68" />
<text xml:space="preserve" text-anchor="middle" x="274.86" y="-644.64" font-family="Noto Sans CJK SC" font-size="12.00">RD AGCU / Ctrl ×5</text>
<text xml:space="preserve" text-anchor="middle" x="274.86" y="-626.64" font-family="Noto Sans CJK SC" font-size="12.00">驻留深1；Active/retiring</text>
<text xml:space="preserve" text-anchor="middle" x="274.86" y="-608.64" font-family="Noto Sans CJK SC" font-size="12.00">算址、预留、RX/读响应</text>
</g>

<g id="diagram82ed021f8526-edge3" class="edge">
<title>nq-&gt;nrd</title>
<path fill="none" stroke="#475569" d="M409.66,-733.17C372.71,-718.45 335.61,-703.68 335.61,-703.68 335.61,-703.68 323.02,-688.42 309.3,-671.8" />
<polygon fill="#475569" stroke="#475569" points="311.57,-670.42 304.77,-666.3 307.52,-673.76 311.57,-670.42" />
<text xml:space="preserve" text-anchor="middle" x="351.73" y="-694.18" font-family="Noto Sans CJK SC" font-size="10.00">RD驻留</text>
</g>

<g id="diagram82ed021f8526-node5" class="node">
<title>nwrq</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M608,-569.4C608,-569.4 489.71,-569.4 489.71,-569.4 483.71,-569.4 477.71,-563.4 477.71,-557.4 477.71,-557.4 477.71,-510.12 477.71,-510.12 477.71,-504.12 483.71,-498.12 489.71,-498.12 489.71,-498.12 608,-498.12 608,-498.12 614,-498.12 620,-504.12 620,-510.12 620,-510.12 620,-557.4 620,-557.4 620,-563.4 614,-569.4 608,-569.4" />
<text xml:space="preserve" text-anchor="middle" x="548.86" y="-549.36" font-family="Noto Sans CJK SC" font-size="12.00">WR TaskQ / AGCU ×5</text>
<text xml:space="preserve" text-anchor="middle" x="548.86" y="-531.36" font-family="Noto Sans CJK SC" font-size="12.00">深4；按Channel保序</text>
<text xml:space="preserve" text-anchor="middle" x="548.86" y="-513.36" font-family="Noto Sans CJK SC" font-size="12.00">目的地址与段context</text>
</g>

<g id="diagram82ed021f8526-edge4" class="edge">
<title>nq-&gt;nwrq</title>
<path fill="none" stroke="#475569" d="M446.32,-727.19C433.33,-714.7 421.86,-703.68 421.86,-703.68 421.86,-703.68 421.86,-688.68 421.86,-688.68 421.86,-688.68 423.86,-593.4 423.86,-593.4 423.86,-593.4 444.57,-583.68 469.14,-572.16" />
<polygon fill="#475569" stroke="#475569" points="470.19,-574.57 475.86,-569 467.96,-569.81 470.19,-574.57" />
<text xml:space="preserve" text-anchor="middle" x="448.86" y="-694.18" font-family="Noto Sans CJK SC" font-size="10.00">同拍WR入队</text>
</g>

<g id="diagram82ed021f8526-node11" class="node">
<title>nsm</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M527.13,-664.68C527.13,-664.68 448.59,-664.68 448.59,-664.68 442.59,-664.68 436.59,-658.68 436.59,-652.68 436.59,-652.68 436.59,-605.4 436.59,-605.4 436.59,-599.4 442.59,-593.4 448.59,-593.4 448.59,-593.4 527.13,-593.4 527.13,-593.4 533.13,-593.4 539.13,-599.4 539.13,-605.4 539.13,-605.4 539.13,-652.68 539.13,-652.68 539.13,-658.68 533.13,-664.68 527.13,-664.68" />
<text xml:space="preserve" text-anchor="middle" x="487.86" y="-644.64" font-family="Noto Sans CJK SC" font-size="12.00">ShareMem</text>
<text xml:space="preserve" text-anchor="middle" x="487.86" y="-626.64" font-family="Noto Sans CJK SC" font-size="12.00">4B可选控制写</text>
<text xml:space="preserve" text-anchor="middle" x="487.86" y="-608.64" font-family="Noto Sans CJK SC" font-size="12.00">等待真实bresp</text>
</g>

<g id="diagram82ed021f8526-edge19" class="edge">
<title>nq-&gt;nsm</title>
<path fill="none" stroke="#475569" d="M482.41,-727.19C482.25,-714.7 482.11,-703.68 482.11,-703.68 482.11,-703.68 482.11,-688.68 482.11,-688.68 482.11,-688.68 482.72,-682.48 483.54,-674.11" />
<polygon fill="#475569" stroke="#475569" points="486.15,-674.39 484.26,-666.67 480.92,-673.88 486.15,-674.39" />
<text xml:space="preserve" text-anchor="middle" x="501.98" y="-694.18" font-family="Noto Sans CJK SC" font-size="10.00">SM Ptr写</text>
</g>

<g id="diagram82ed021f8526-node12" class="node">
<title>nts</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M683.38,-664.68C683.38,-664.68 576.34,-664.68 576.34,-664.68 570.34,-664.68 564.34,-658.68 564.34,-652.68 564.34,-652.68 564.34,-605.4 564.34,-605.4 564.34,-599.4 570.34,-593.4 576.34,-593.4 576.34,-593.4 683.38,-593.4 683.38,-593.4 689.38,-593.4 695.38,-599.4 695.38,-605.4 695.38,-605.4 695.38,-652.68 695.38,-652.68 695.38,-658.68 689.38,-664.68 683.38,-664.68" />
<text xml:space="preserve" text-anchor="middle" x="629.86" y="-644.64" font-family="Noto Sans CJK SC" font-size="12.00">TS done</text>
<text xml:space="preserve" text-anchor="middle" x="629.86" y="-626.64" font-family="Noto Sans CJK SC" font-size="12.00">UID16 / SID4 / TID6</text>
<text xml:space="preserve" text-anchor="middle" x="629.86" y="-608.64" font-family="Noto Sans CJK SC" font-size="12.00">Ack按序valid-only</text>
</g>

<g id="diagram82ed021f8526-edge21" class="edge">
<title>nq-&gt;nts</title>
<path fill="none" stroke="#475569" d="M526.59,-727.19C542.13,-714.7 555.86,-703.68 555.86,-703.68 555.86,-703.68 571.46,-688.15 588.34,-671.35" />
<polygon fill="#475569" stroke="#475569" points="590.05,-673.35 593.52,-666.2 586.35,-669.63 590.05,-673.35" />
<text xml:space="preserve" text-anchor="middle" x="595.89" y="-694.18" font-family="Noto Sans CJK SC" font-size="10.00">Ack Ptr退休</text>
</g>

<g id="diagram82ed021f8526-node6" class="node">
<title>nbuf</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M513.63,-474.12C513.63,-474.12 396.09,-474.12 396.09,-474.12 390.09,-474.12 384.09,-468.12 384.09,-462.12 384.09,-462.12 384.09,-414.84 384.09,-414.84 384.09,-408.84 390.09,-402.84 396.09,-402.84 396.09,-402.84 513.63,-402.84 513.63,-402.84 519.63,-402.84 525.63,-408.84 525.63,-414.84 525.63,-414.84 525.63,-462.12 525.63,-462.12 525.63,-468.12 519.63,-474.12 513.63,-474.12" />
<text xml:space="preserve" text-anchor="middle" x="454.86" y="-454.08" font-family="Noto Sans CJK SC" font-size="12.00">Buffer ×5</text>
<text xml:space="preserve" text-anchor="middle" x="454.86" y="-436.08" font-family="Noto Sans CJK SC" font-size="12.00">每Channel32×256B</text>
<text xml:space="preserve" text-anchor="middle" x="454.86" y="-418.08" font-family="Noto Sans CJK SC" font-size="12.00">free / reserved / valid</text>
</g>

<g id="diagram82ed021f8526-edge5" class="edge">
<title>nrd-&gt;nbuf</title>
<path fill="none" stroke="#475569" d="M303.87,-593.22C335.93,-554.74 383.11,-498.12 383.11,-498.12 383.11,-498.12 392.89,-490.13 405.23,-480.04" />
<polygon fill="#475569" stroke="#475569" points="406.56,-482.34 410.71,-475.56 403.24,-478.28 406.56,-482.34" />
<text xml:space="preserve" text-anchor="middle" x="423.98" y="-531.76" font-family="Noto Sans CJK SC" font-size="10.00">数据/响应填预留槽</text>
</g>

<g id="diagram82ed021f8526-node7" class="node">
<title>nwr</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M520.38,-339.84C520.38,-339.84 389.34,-339.84 389.34,-339.84 383.34,-339.84 377.34,-333.84 377.34,-327.84 377.34,-327.84 377.34,-280.56 377.34,-280.56 377.34,-274.56 383.34,-268.56 389.34,-268.56 389.34,-268.56 520.38,-268.56 520.38,-268.56 526.38,-268.56 532.38,-274.56 532.38,-280.56 532.38,-280.56 532.38,-327.84 532.38,-327.84 532.38,-333.84 526.38,-339.84 520.38,-339.84" />
<text xml:space="preserve" text-anchor="middle" x="454.86" y="-319.8" font-family="Noto Sans CJK SC" font-size="12.00">WR Ctrl ×5</text>
<text xml:space="preserve" text-anchor="middle" x="454.86" y="-301.8" font-family="Noto Sans CJK SC" font-size="12.00">Active与上一任务Hold</text>
<text xml:space="preserve" text-anchor="middle" x="454.86" y="-283.8" font-family="Noto Sans CJK SC" font-size="12.00">写响应与RD drain_done</text>
</g>

<g id="diagram82ed021f8526-edge8" class="edge">
<title>nrd-&gt;nwr</title>
<path fill="none" stroke="#475569" d="M275.82,-593.35C277.57,-530.18 281.11,-402.84 281.11,-402.84 281.11,-402.84 336.66,-371.62 384.94,-344.49" />
<polygon fill="#475569" stroke="#475569" points="386.05,-346.88 391.3,-340.92 383.48,-342.3 386.05,-346.88" />
<text xml:space="preserve" text-anchor="middle" x="326.48" y="-436.48" font-family="Noto Sans CJK SC" font-size="10.00">同Entry drain_done</text>
</g>

<g id="diagram82ed021f8526-node8" class="node">
<title>nmem</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M264.25,-205.56C264.25,-205.56 153.46,-205.56 153.46,-205.56 147.46,-205.56 141.46,-199.56 141.46,-193.56 141.46,-193.56 141.46,-146.28 141.46,-146.28 141.46,-140.28 147.46,-134.28 153.46,-134.28 153.46,-134.28 264.25,-134.28 264.25,-134.28 270.25,-134.28 276.25,-140.28 276.25,-146.28 276.25,-146.28 276.25,-193.56 276.25,-193.56 276.25,-199.56 270.25,-205.56 264.25,-205.56" />
<text xml:space="preserve" text-anchor="middle" x="208.86" y="-185.52" font-family="Noto Sans CJK SC" font-size="12.00">端口仲裁与适配</text>
<text xml:space="preserve" text-anchor="middle" x="208.86" y="-167.52" font-family="Noto Sans CJK SC" font-size="12.00">CM/MM双128B</text>
<text xml:space="preserve" text-anchor="middle" x="208.86" y="-149.52" font-family="Noto Sans CJK SC" font-size="12.00">M Top MM/topK共享</text>
</g>

<g id="diagram82ed021f8526-edge10" class="edge">
<title>nrd-&gt;nmem</title>
<path fill="none" stroke="#475569" d="M221.54,-592.91C202.59,-580.42 185.86,-569.4 185.86,-569.4 185.86,-569.4 148.61,-378.84 148.61,-378.84 148.61,-378.84 148.61,-363.84 148.61,-363.84 148.61,-363.84 189.86,-229.56 189.86,-229.56 189.86,-229.56 191.95,-223.1 194.75,-214.46" />
<polygon fill="#475569" stroke="#475569" points="197.19,-215.43 197.01,-207.49 192.2,-213.81 197.19,-215.43" />
<text xml:space="preserve" text-anchor="middle" x="163.23" y="-369.34" font-family="Noto Sans CJK SC" font-size="10.00">读请求</text>
</g>

<g id="diagram82ed021f8526-edge6" class="edge">
<title>nwrq-&gt;nwr</title>
<path fill="none" stroke="#475569" d="M545.91,-497.94C542.65,-459.46 537.86,-402.84 537.86,-402.84 537.86,-402.84 509.86,-363.84 509.86,-363.84 509.86,-363.84 502.9,-356.42 493.93,-346.86" />
<polygon fill="#475569" stroke="#475569" points="495.88,-345.11 488.84,-341.43 492.05,-348.7 495.88,-345.11" />
<text xml:space="preserve" text-anchor="middle" x="570.45" y="-436.48" font-family="Noto Sans CJK SC" font-size="10.00">目的context</text>
</g>

<g id="diagram82ed021f8526-edge7" class="edge">
<title>nbuf-&gt;nwr</title>
<path fill="none" stroke="#475569" d="M454.86,-402.38C454.86,-385.95 454.86,-366.23 454.86,-348.85" />
<polygon fill="#475569" stroke="#475569" points="457.48,-349.02 454.86,-341.52 452.23,-349.02 457.48,-349.02" />
<text xml:space="preserve" text-anchor="middle" x="479.23" y="-369.34" font-family="Noto Sans CJK SC" font-size="10.00">按段序消费</text>
</g>

<g id="diagram82ed021f8526-edge9" class="edge">
<title>nwr-&gt;nq</title>
<path fill="none" stroke="#475569" d="M532.75,-321.69C614.68,-339.03 731.86,-363.84 731.86,-363.84 731.86,-363.84 731.86,-378.84 731.86,-378.84 731.86,-378.84 707.86,-664.68 707.86,-664.68 707.86,-664.68 627.86,-703.68 627.86,-703.68 627.86,-703.68 597.27,-716.05 564.2,-729.43" />
<polygon fill="#475569" stroke="#475569" points="563.58,-726.84 557.61,-732.09 565.55,-731.71 563.58,-726.84" />
<text xml:space="preserve" text-anchor="middle" x="755.91" y="-531.76" font-family="Noto Sans CJK SC" font-size="10.00">lane_finish ×5</text>
</g>

<g id="diagram82ed021f8526-edge12" class="edge">
<title>nwr-&gt;nmem</title>
<path fill="none" stroke="#475569" d="M377.09,-270.45C345.19,-257.02 315.61,-244.56 315.61,-244.56 315.61,-244.56 292.1,-228.34 267.14,-211.13" />
<polygon fill="#475569" stroke="#475569" points="268.64,-208.97 260.98,-206.87 265.66,-213.29 268.64,-208.97" />
<text xml:space="preserve" text-anchor="middle" x="330.23" y="-235.06" font-family="Noto Sans CJK SC" font-size="10.00">写请求</text>
</g>

<g id="diagram82ed021f8526-node10" class="node">
<title>nrouter</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M593.75,-205.56C593.75,-205.56 479.96,-205.56 479.96,-205.56 473.96,-205.56 467.96,-199.56 467.96,-193.56 467.96,-193.56 467.96,-146.28 467.96,-146.28 467.96,-140.28 473.96,-134.28 479.96,-134.28 479.96,-134.28 593.75,-134.28 593.75,-134.28 599.75,-134.28 605.75,-140.28 605.75,-146.28 605.75,-146.28 605.75,-193.56 605.75,-193.56 605.75,-199.56 599.75,-205.56 593.75,-205.56" />
<text xml:space="preserve" text-anchor="middle" x="536.86" y="-185.52" font-family="Noto Sans CJK SC" font-size="12.00">Router RX / TX</text>
<text xml:space="preserve" text-anchor="middle" x="536.86" y="-167.52" font-family="Noto Sans CJK SC" font-size="12.00">RX valid/ready</text>
<text xml:space="preserve" text-anchor="middle" x="536.86" y="-149.52" font-family="Noto Sans CJK SC" font-size="12.00">TX每VC信用、flit交织</text>
</g>

<g id="diagram82ed021f8526-edge17" class="edge">
<title>nwr-&gt;nrouter</title>
<path fill="none" stroke="#475569" d="M486.61,-268.07C497.89,-255.58 507.86,-244.56 507.86,-244.56 507.86,-244.56 513.55,-230.1 519.89,-214" />
<polygon fill="#475569" stroke="#475569" points="522.18,-215.36 522.48,-207.42 517.29,-213.44 522.18,-215.36" />
<text xml:space="preserve" text-anchor="middle" x="536.65" y="-235.06" font-family="Noto Sans CJK SC" font-size="10.00">实际TX flit</text>
</g>

<g id="diagram82ed021f8526-edge11" class="edge">
<title>nmem-&gt;nrd</title>
<path fill="none" stroke="#475569" d="M213.9,-205.84C225.76,-288.01 255.43,-493.48 268.5,-584.02" />
<polygon fill="#475569" stroke="#475569" points="265.9,-584.38 269.57,-591.43 271.1,-583.63 265.9,-584.38" />
<text xml:space="preserve" text-anchor="middle" x="253.44" y="-369.34" font-family="Noto Sans CJK SC" font-size="10.00">读响应</text>
</g>

<g id="diagram82ed021f8526-edge13" class="edge">
<title>nmem-&gt;nwr</title>
<path fill="none" stroke="#475569" d="M276.64,-199.72C311.54,-214.55 346.86,-229.56 346.86,-229.56 346.86,-229.56 370.85,-245.92 396.21,-263.21" />
<polygon fill="#475569" stroke="#475569" points="394.46,-265.2 402.14,-267.25 397.42,-260.86 394.46,-265.2" />
<text xml:space="preserve" text-anchor="middle" x="382.14" y="-235.06" font-family="Noto Sans CJK SC" font-size="10.00">写响应</text>
</g>

<g id="diagram82ed021f8526-edge14" class="edge">
<title>nmem-&gt;nht</title>
<path fill="none" stroke="#475569" d="M183.65,-133.82C171.42,-116.79 156.64,-96.21 143.84,-78.39" />
<polygon fill="#475569" stroke="#475569" points="146.24,-77.24 139.74,-72.68 141.98,-80.3 146.24,-77.24" />
<text xml:space="preserve" text-anchor="middle" x="183.71" y="-100.78" font-family="Noto Sans CJK SC" font-size="10.00">通道读写</text>
</g>

<g id="diagram82ed021f8526-edge16" class="edge">
<title>nrouter-&gt;nrd</title>
<path fill="none" stroke="#475569" d="M554.23,-206.05C560.4,-218.54 565.86,-229.56 565.86,-229.56 565.86,-229.56 632.86,-498.12 632.86,-498.12 632.86,-498.12 632.86,-569.4 632.86,-569.4 632.86,-569.4 423.86,-593.4 423.86,-593.4 423.86,-593.4 394.39,-600.25 361.66,-607.86" />
<polygon fill="#475569" stroke="#475569" points="361.47,-605.21 354.76,-609.46 362.66,-610.32 361.47,-605.21" />
<text xml:space="preserve" text-anchor="middle" x="634.32" y="-369.34" font-family="Noto Sans CJK SC" font-size="10.00">RX头/payload</text>
</g>

<g id="diagram82ed021f8526-edge18" class="edge">
<title>nrouter-&gt;nwr</title>
<path fill="none" stroke="#475569" d="M484.89,-206.05C466.42,-218.54 450.11,-229.56 450.11,-229.56 450.11,-229.56 450.11,-244.56 450.11,-244.56 450.11,-244.56 450.61,-250.76 451.29,-259.13" />
<polygon fill="#475569" stroke="#475569" points="448.67,-259.3 451.89,-266.56 453.9,-258.88 448.67,-259.3" />
<text xml:space="preserve" text-anchor="middle" x="475.98" y="-235.06" font-family="Noto Sans CJK SC" font-size="10.00">每VC credit</text>
</g>

<g id="diagram82ed021f8526-edge20" class="edge">
<title>nsm-&gt;nq</title>
<path fill="none" stroke="#475569" d="M509.42,-665.17C517.09,-677.66 523.86,-688.68 523.86,-688.68 523.86,-688.68 523.86,-703.68 523.86,-703.68 523.86,-703.68 518.96,-710.68 512.56,-719.84" />
<polygon fill="#475569" stroke="#475569" points="510.43,-718.3 508.28,-725.95 514.73,-721.31 510.43,-718.3" />
<text xml:space="preserve" text-anchor="middle" x="536.61" y="-694.18" font-family="Noto Sans CJK SC" font-size="10.00">bresp</text>
</g>
</g>
</svg>
```

***

## 拆开与合上

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="100%" viewBox="0.00 0.00 1295.00 181.00" role="img" aria-label="一个搬运任务怎么被拆开又合上">
<title>一个搬运任务怎么被拆开又合上</title>
<g id="diagram8f7c411cea9a-graph0" class="graph" transform="scale(1 1) rotate(0) translate(18 163.28)">
<title>G</title>
<polygon fill="white" stroke="none" points="-18,18 -18,-163.28 1276.95,-163.28 1276.95,18 -18,18" />
<text xml:space="preserve" text-anchor="middle" x="629.47" y="-126.08" font-family="Noto Sans CJK SC" font-size="16.00">一个搬运任务怎么被拆开又合上</text>
<text xml:space="preserve" text-anchor="middle" x="629.47" y="-102.08" font-family="Noto Sans CJK SC" font-size="16.00">归属键是全局entry_id；WR收齐本任务RD与写侧完成条件后回写，全局SM/Ack按序退休。</text>

<g id="diagram8f7c411cea9a-node1" class="node">
<title>n0</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M189.54,-89.28C189.54,-89.28 12,-89.28 12,-89.28 6,-89.28 0,-83.28 0,-77.28 0,-77.28 0,-12 0,-12 0,-6 6,0 12,0 12,0 189.54,0 189.54,0 195.54,0 201.54,-6 201.54,-12 201.54,-12 201.54,-77.28 201.54,-77.28 201.54,-83.28 195.54,-89.28 189.54,-89.28" />
<text xml:space="preserve" text-anchor="middle" x="100.77" y="-69.24" font-family="Noto Sans CJK SC" font-size="12.00">M2 · 全局Q / dispatch</text>
<text xml:space="preserve" text-anchor="middle" x="100.77" y="-51.24" font-family="Noto Sans CJK SC" font-size="12.00">16 Entry从接纳持有到退休</text>
<text xml:space="preserve" text-anchor="middle" x="100.77" y="-33.24" font-family="Noto Sans CJK SC" font-size="12.00">Ch Ptr同时交付RD驻留与WR Q</text>
<text xml:space="preserve" text-anchor="middle" x="100.77" y="-15.24" font-family="Noto Sans CJK SC" font-size="12.00">同Channel保序，跨Channel独立</text>
</g>

<g id="diagram8f7c411cea9a-node2" class="node">
<title>n1</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M453.33,-89.28C453.33,-89.28 281.04,-89.28 281.04,-89.28 275.04,-89.28 269.04,-83.28 269.04,-77.28 269.04,-77.28 269.04,-12 269.04,-12 269.04,-6 275.04,0 281.04,0 281.04,0 453.33,0 453.33,0 459.33,0 465.33,-6 465.33,-12 465.33,-12 465.33,-77.28 465.33,-77.28 465.33,-83.28 459.33,-89.28 453.33,-89.28" />
<text xml:space="preserve" text-anchor="middle" x="367.18" y="-69.24" font-family="Noto Sans CJK SC" font-size="12.00">M4 · 读请求 / 响应</text>
<text xml:space="preserve" text-anchor="middle" x="367.18" y="-51.24" font-family="Noto Sans CJK SC" font-size="12.00">请求Fire前预留Buffer槽</text>
<text xml:space="preserve" text-anchor="middle" x="367.18" y="-33.24" font-family="Noto Sans CJK SC" font-size="12.00">CM/MM双128B，topK两笔128B</text>
<text xml:space="preserve" text-anchor="middle" x="367.18" y="-15.24" font-family="Noto Sans CJK SC" font-size="12.00">响应按端点FIFO恢复原预留项</text>
</g>

<g id="diagram8f7c411cea9a-edge1" class="edge">
<title>n0-&gt;n1</title>
<path fill="none" stroke="#475569" d="M201.73,-44.64C220.76,-44.64 240.73,-44.64 259.94,-44.64" />
<polygon fill="#475569" stroke="#475569" points="259.69,-47.27 267.19,-44.64 259.69,-42.02 259.69,-47.27" />
<text xml:space="preserve" text-anchor="middle" x="235.29" y="-50.14" font-family="Noto Sans CJK SC" font-size="10.00">推进</text>
</g>

<g id="diagram8f7c411cea9a-node3" class="node">
<title>n2</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M698.37,-89.28C698.37,-89.28 544.83,-89.28 544.83,-89.28 538.83,-89.28 532.83,-83.28 532.83,-77.28 532.83,-77.28 532.83,-12 532.83,-12 532.83,-6 538.83,0 544.83,0 544.83,0 698.37,0 698.37,0 704.37,0 710.37,-6 710.37,-12 710.37,-12 710.37,-77.28 710.37,-77.28 710.37,-83.28 704.37,-89.28 698.37,-89.28" />
<text xml:space="preserve" text-anchor="middle" x="621.6" y="-69.24" font-family="Noto Sans CJK SC" font-size="12.00">M6 · WR / 数据重对齐</text>
<text xml:space="preserve" text-anchor="middle" x="621.6" y="-51.24" font-family="Noto Sans CJK SC" font-size="12.00">内存写等待真实写响应</text>
<text xml:space="preserve" text-anchor="middle" x="621.6" y="-33.24" font-family="Noto Sans CJK SC" font-size="12.00">Router TX按VC credit发flit</text>
<text xml:space="preserve" text-anchor="middle" x="621.6" y="-15.24" font-family="Noto Sans CJK SC" font-size="12.00">prev/residue跨拍与跨段保留</text>
</g>

<g id="diagram8f7c411cea9a-edge2" class="edge">
<title>n1-&gt;n2</title>
<path fill="none" stroke="#475569" d="M465.78,-44.64C484.7,-44.64 504.53,-44.64 523.44,-44.64" />
<polygon fill="#475569" stroke="#475569" points="523.33,-47.27 530.83,-44.64 523.33,-42.02 523.33,-47.27" />
<text xml:space="preserve" text-anchor="middle" x="499.08" y="-50.14" font-family="Noto Sans CJK SC" font-size="10.00">推进</text>
</g>

<g id="diagram8f7c411cea9a-node4" class="node">
<title>n3</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M962.91,-89.28C962.91,-89.28 789.87,-89.28 789.87,-89.28 783.87,-89.28 777.87,-83.28 777.87,-77.28 777.87,-77.28 777.87,-12 777.87,-12 777.87,-6 783.87,0 789.87,0 789.87,0 962.91,0 962.91,0 968.91,0 974.91,-6 974.91,-12 974.91,-12 974.91,-77.28 974.91,-77.28 974.91,-83.28 968.91,-89.28 962.91,-89.28" />
<text xml:space="preserve" text-anchor="middle" x="876.39" y="-69.24" font-family="Noto Sans CJK SC" font-size="12.00">M7 · lane_finish</text>
<text xml:space="preserve" text-anchor="middle" x="876.39" y="-51.24" font-family="Noto Sans CJK SC" font-size="12.00">WR等待写响应与RD drain_done</text>
<text xml:space="preserve" text-anchor="middle" x="876.39" y="-33.24" font-family="Noto Sans CJK SC" font-size="12.00">同Entry唯一一次finish</text>
<text xml:space="preserve" text-anchor="middle" x="876.39" y="-15.24" font-family="Noto Sans CJK SC" font-size="12.00">五路同拍分别更新全局Entry</text>
</g>

<g id="diagram8f7c411cea9a-edge3" class="edge">
<title>n2-&gt;n3</title>
<path fill="none" stroke="#475569" d="M710.66,-44.64C729.44,-44.64 749.47,-44.64 768.86,-44.64" />
<polygon fill="#475569" stroke="#475569" points="768.7,-47.27 776.2,-44.64 768.7,-42.02 768.7,-47.27" />
<text xml:space="preserve" text-anchor="middle" x="744.12" y="-50.14" font-family="Noto Sans CJK SC" font-size="10.00">推进</text>
</g>

<g id="diagram8f7c411cea9a-node5" class="node">
<title>n4</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M1246.95,-89.28C1246.95,-89.28 1054.41,-89.28 1054.41,-89.28 1048.41,-89.28 1042.41,-83.28 1042.41,-77.28 1042.41,-77.28 1042.41,-12 1042.41,-12 1042.41,-6 1048.41,0 1054.41,0 1054.41,0 1246.95,0 1246.95,0 1252.95,0 1258.95,-6 1258.95,-12 1258.95,-12 1258.95,-77.28 1258.95,-77.28 1258.95,-83.28 1252.95,-89.28 1246.95,-89.28" />
<text xml:space="preserve" text-anchor="middle" x="1150.68" y="-69.24" font-family="Noto Sans CJK SC" font-size="12.00">M8 · SM / Ack退休</text>
<text xml:space="preserve" text-anchor="middle" x="1150.68" y="-51.24" font-family="Noto Sans CJK SC" font-size="12.00">SM Ptr按序可选4B写并等bresp</text>
<text xml:space="preserve" text-anchor="middle" x="1150.68" y="-33.24" font-family="Noto Sans CJK SC" font-size="12.00">Ack Ptr按接纳顺序释放Entry</text>
<text xml:space="preserve" text-anchor="middle" x="1150.68" y="-15.24" font-family="Noto Sans CJK SC" font-size="12.00">ack_ts_en=1才发valid-only TS ACK</text>
</g>

<g id="diagram8f7c411cea9a-edge4" class="edge">
<title>n3-&gt;n4</title>
<path fill="none" stroke="#475569" d="M975.31,-44.64C994.08,-44.64 1013.91,-44.64 1033.21,-44.64" />
<polygon fill="#475569" stroke="#475569" points="1033.01,-47.27 1040.51,-44.64 1033.01,-42.02 1033.01,-47.27" />
<text xml:space="preserve" text-anchor="middle" x="1008.66" y="-50.14" font-family="Noto Sans CJK SC" font-size="10.00">推进</text>
</g>
</g>
</svg>
```

### 五个物理通道

| Route 逻辑编码 | 方向 | 通道与端点 |
| - | - | - |
| 000 | Router→CM | InCh；各段目的端由地址窗口确定 |
| 001 | Router→MM | InCh；各段目的端由地址窗口确定 |
| 010 | CM→Router | OutCh0～3，由 VCID 选择 |
| 011 | MM→Router | OutCh0～3，由 VCID 选择 |
| 100 | Core 内搬运 | OutCh3；支持 MM→CM 和 CM→HeaderTable |

Route 确定整体形态和 Channel。每段的 CM、MM、topK 或 HeaderTable 端点仍由该段地址窗口决定。CM→MM 的写路径不在当前 XBar 中，非法配置必须在第一笔外部请求前拦截。

每 Channel 有独立的 RD 和 WR issue lane，共十条。下发要求本 Channel 的 RD AGCU 驻留空闲、WR TaskQ 有空间，同拍交付两侧，携带同一 entry_id。RD AGCU 深度 1，WR TaskQ 深度 4。不同 Channel 可以乱序下发和物理完成，同一 Channel 保持任务顺序。

全局 TaskQ 是容量、任务身份、异常现场和退休的所有者。Tail 接纳任务，五个 Ch Ptr 分别扫描和下发，SM Ptr 处理完成后的 ShareMem 写，Ack Ptr 按接纳顺序释放条目。循环序满足 `Ack Ptr ≤ SM Ptr ≤ ChX Ptr ≤ Tail Ptr`。已下发条目不能因为 lane 空闲或物理完成提前复用。
### VC独立与Core内搬运
四个OutCh分别绑定VC0～3；某个VC缺credit时不阻塞其他VC，复用点可以按flit交织。一个VC内部Packet不交织。OutCh3另支持MM→CM及CM→HeaderTable，写路径不能到MM；普通TX相对OutCh0～2多一拍。

TS先完成业务方向或RMEM许可；DTE下发只检查本Channel RD驻留与WR队列空间。Router发送时才逐flit消费本地VC credit，这与TS许可是两类资源。

### Commit：配对接纳，不产生半任务

Trigger Fire先接纳完整不可变Entry到全局16项Q。目标Channel的Ch Ptr命中Ready且RD AGCU空闲、WR TaskQ有位时，同拍交付两侧，并使Entry进入RS状态。两个条件只满足一个时不下发，其他Channel可独立扫描。

Entry自身承担容量、身份和异常现场；dispatch不释放Entry。模型Commit与Completion RS分别保存同一逻辑Q的下发视图和完成视图，不是另加16项独立任务容量。任何响应、Buffer或Hold都要保留entry_id，直到Ack退休才可复用。

### 中间 Buffer 与 read-ahead

每个Channel独立32×256B=8KiB，五Channel合计40KiB。Buffer只存FIFO数据与归属元数据，重对齐使用Buffer外的prev/residue寄存器。

读Fire前预留槽，响应到达后置valid，写侧消费后归还free。free+reserved+valid始终等于32。读侧领先受Buffer credit、最多32个读Outstanding、WR队列4项以及Active/Hold容量共同限制。

当前任务的最后请求发出后可进入retiring/Hold，让下一任务激活；上一任务迟到响应不能写入新任务的槽。Hold未排空时，不能再释放另一个任务覆盖它。

### WR完成回写与全局有序退休

各Channel由WR汇合实际写完成与同任务RD drain_done，只生成一次lane_finish。entry_id是内部归属键，SID/TID/UID是TS身份；同TID不同用户或多搬运条目不能只按task_id匹配。

五路WR同拍完成不同Entry时都要保存。年轻Entry先完成只置自身Done，SM/Ack不能越过队头。只有Ack Ptr按接纳顺序退休、且ack_ts_en=1时才产生一拍TS完成；TS端没有Ready。

### 完成的六个层级

| 状态 | 粒度 | 释放或可见条件 |
| - | - | - |
| queued | 全局Entry／WR工作项 | 已接纳、尚未激活 |
| active | RD／WR上下文 | 正在算址、接收或发送 |
| issue_done | 单侧上下文 | 最后请求已Fire，可在Hold有空间时换任务 |
| drained | 响应及Buffer归属 | 本侧所有在途响应和该任务数据均收敛 |
| lane_finish | WR所拥有的Entry | 写侧完成且对应RD drain_done已到 |
| retired / task_done | 全局Entry／TS任务 | SM响应完成、Ack按序退休；ACK使能决定是否通知TS |

issue_done不是写响应完成。Router侧最后flit必须在实际TX信用允许的拍发出，不能以排入中间队列代替。多搬运TS任务只由最后一条带ACK的Entry通知完成。

### 并发约束

| 场景 | 是否允许 | 边界 |
| - | - | - |
| RD领先WR | 允许 | credit、Outstanding、WR Q和Hold共同限制 |
| 五Channel并行 | 允许 | 共用端口按仲裁保持请求归属 |
| Router→MM与MM→CM | 可重叠 | MM读/写不同口；同bank访问由存储仲裁 |
| Router→CM与MM→CM | 竞争 | 共用CM写通路，落选者保持valid/地址/mask/数据 |
| 同Channel任务乱序 | 不允许 | RD与WR可处于不同任务，但各侧激活保序 |
| 多路同拍finish | 允许 | 分别回写Entry；最终SM/Ack各按序推进 |
| 四VC TX交织 | 允许 | 每VC包内保序、hflit上下文稳定 |
| 后继覆盖旧Hold | 不允许 | 上一任务响应与drain先收敛 |

## 三条数据流

### Inbound：Router → MM / CM

RV Core 配好各段的目的地址和长度并提交任务。RD 侧驻留任务后接收 Packet。RX 是单通道 valid/ready，首 flit 的 32B hflit 与 256B pflit 并行。保存包头时先停一拍，将包头写入 Buffer，再接收首拍 payload；丢弃包头时直接接收 payload。

WR 侧按四段的目的端拆分数据，以 8B 粒度重对齐并形成字节 mask。包头可以写 HeaderTable、CM 或 MM。topK 与其他数据段使用同一段位机制，经 M Top 写入对应端点。

RX 在首 flit Fire 时校验 pkt_length，并在 tail 时校验 flit 数。无驻留任务、未收到 tail 又出现 head、长度不符均保留任务现场。写侧待全部写响应返回，并取得同任务的 RD drain_done 后才产生 lane_finish。
1. RV Trigger快照已入全局Q；Channel驻留任务后才允许对应RX Packet进入。
2. 首拍锁存32B hflit，同时保存首pflit资格；保存头时先把头写Buffer再推进payload。
3. WR按四段独立地址/长度拆分，CM/MM、topK、HeaderTable由目的地址选择。
4. 长度校验分别检查首包声明长度、实际tail位置与flit数，不因flit数相同而接受错误字节长。
5. 全部写响应与RD drain_done到齐后WR回写Entry；SM写完成后按全局顺序退休。

### Outbound：MM / CM → Router

RD 侧按段顺序从对应端点发读请求，Fire 当拍为响应预留 Buffer 槽。各端点响应恢复 entry_id、段号和槽号，按预留顺序交给 WR。段 0 读回后重写 UID、PID、VCID 与 pkt_length，再保存为本 Packet 的包头。

WR 先装载包头寄存器，该动作不输出 flit。后续每个 flit 同时携带 hflit 和 pflit，head/tail 标识本 VC 的 Packet 边界。段间的 residue 保留未输出字节，直到最后一个 flit Fire 才清空。纯包头任务也发送一个 head/tail 同时有效的 flit。

TX 使用每 VC 的 credit，没有 Ready。四条 OutCh 在复用点按 flit 交织，Packet 的所有权仅限制同一 VC，其他 VC 可以继续发送。最后一个 flit 发出即完成 Router 写侧发送，但 lane_finish 仍需等待对应 RD drain_done。
读到段0后重写硬件PID、UID、VCID和总包长，保持core_mask及16B sw_aux。每个TX flit携带同一hflit；段1～3在residue中按段序拼接，无效尾字节不得计入Packet长度。

四个OutCh都能保留自己VC的包上下文。仲裁可在合法flit边界切换VC，但同一VC的两Packet不得交织。

### Inner：MM → CM 与 CM → HeaderTable

OutCh3 将 Router 发包路径与 Core 内写路径分开选择。Core 内模式对四段源端和目的端分别译码，源地址按软件给出的物理地址，目的地址叠加 SID×stride。MM→CM 搬包头和各使能数据段；CM→HeaderTable 将段 0 写入指定组号与项号。完成等待实际目的端的写响应。

OutCh3 对外发包相对 OutCh0～2 多一拍延迟，软件优先将普通输出放在 VC0～2。该差异不能解释为四个通道共享一个整包发送锁。
MM→CM和CM→HeaderTable均走OutCh3 Core内写路径；源使用软件物理地址，目的叠SID×stride。源与目的不是固定同一段端点，仍逐段译码。没有Router TX，不消费Router信用；写响应全部返回才能完成。

## 一个任务搬的是什么

一次搬运的对象是一个 MSG 包。包在 Router 线上连成一条，进了 core 就**按内容拆成四份、各存各的地方**。

拆开存是因为读者不同：

* **data 与 scale**：给 MU 和 VU 算
* **topK**：给 MU 查专家
* **包头**：只在这个 token 再出核时用来重写路由

四份各有各的地址，这是下一节地址算法要分五类的根源。拆到哪里去、地址怎么来，一张图看完。

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="100%" viewBox="0.00 0.00 918.00 181.00" role="img" aria-label="一个任务搬的四类内容与各自的地址">
<title>一个任务搬的四类内容与各自的地址</title>
<g id="diagram7e133923141a-graph0" class="graph" transform="scale(1 1) rotate(0) translate(18 163.28)">
<title>G</title>
<polygon fill="white" stroke="none" points="-18,18 -18,-163.28 899.91,-163.28 899.91,18 -18,18" />
<text xml:space="preserve" text-anchor="middle" x="440.95" y="-126.08" font-family="Noto Sans CJK SC" font-size="16.00">一个任务搬的四类内容与各自的地址</text>
<text xml:space="preserve" text-anchor="middle" x="440.95" y="-102.08" font-family="Noto Sans CJK SC" font-size="16.00">数据类型是软件约定；四段均按各自的端点、地址和长度执行，不依方向隐式推算长度。</text>

<g id="diagram7e133923141a-node1" class="node">
<title>n0</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M152.79,-89.28C152.79,-89.28 12,-89.28 12,-89.28 6,-89.28 0,-83.28 0,-77.28 0,-77.28 0,-12 0,-12 0,-6 6,0 12,0 12,0 152.79,0 152.79,0 158.79,0 164.79,-6 164.79,-12 164.79,-12 164.79,-77.28 164.79,-77.28 164.79,-83.28 158.79,-89.28 152.79,-89.28" />
<text xml:space="preserve" text-anchor="middle" x="82.39" y="-69.24" font-family="Noto Sans CJK SC" font-size="12.00">段0 · 32B包头</text>
<text xml:space="preserve" text-anchor="middle" x="82.39" y="-51.24" font-family="Noto Sans CJK SC" font-size="12.00">HT索引或CM/MM字节地址</text>
<text xml:space="preserve" text-anchor="middle" x="82.39" y="-33.24" font-family="Noto Sans CJK SC" font-size="12.00">HT：16组 × 32项</text>
<text xml:space="preserve" text-anchor="middle" x="82.39" y="-15.24" font-family="Noto Sans CJK SC" font-size="12.00">共享core_mask与sw_aux</text>
</g>

<g id="diagram7e133923141a-node2" class="node">
<title>n1</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M403.83,-89.28C403.83,-89.28 244.29,-89.28 244.29,-89.28 238.29,-89.28 232.29,-83.28 232.29,-77.28 232.29,-77.28 232.29,-12 232.29,-12 232.29,-6 238.29,0 244.29,0 244.29,0 403.83,0 403.83,0 409.83,0 415.83,-6 415.83,-12 415.83,-12 415.83,-77.28 415.83,-77.28 415.83,-83.28 409.83,-89.28 403.83,-89.28" />
<text xml:space="preserve" text-anchor="middle" x="324.06" y="-69.24" font-family="Noto Sans CJK SC" font-size="12.00">段1 · data</text>
<text xml:space="preserve" text-anchor="middle" x="324.06" y="-51.24" font-family="Noto Sans CJK SC" font-size="12.00">CM：基址+SID×stride</text>
<text xml:space="preserve" text-anchor="middle" x="324.06" y="-33.24" font-family="Noto Sans CJK SC" font-size="12.00">MM：软件物理地址、stride=0</text>
<text xml:space="preserve" text-anchor="middle" x="324.06" y="-15.24" font-family="Noto Sans CJK SC" font-size="12.00">长度由DATA_LEN1配置</text>
</g>

<g id="diagram7e133923141a-edge1" class="edge">
<title>n0-&gt;n1</title>
<path fill="none" stroke="#475569" d="M165.2,-44.64C183.9,-44.64 203.98,-44.64 223.33,-44.64" />
<polygon fill="#475569" stroke="#475569" points="223.15,-47.27 230.65,-44.64 223.15,-42.02 223.15,-47.27" />
<text xml:space="preserve" text-anchor="middle" x="198.54" y="-50.14" font-family="Noto Sans CJK SC" font-size="10.00">推进</text>
</g>

<g id="diagram7e133923141a-node3" class="node">
<title>n2</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M622.62,-89.28C622.62,-89.28 495.33,-89.28 495.33,-89.28 489.33,-89.28 483.33,-83.28 483.33,-77.28 483.33,-77.28 483.33,-12 483.33,-12 483.33,-6 489.33,0 495.33,0 495.33,0 622.62,0 622.62,0 628.62,0 634.62,-6 634.62,-12 634.62,-12 634.62,-77.28 634.62,-77.28 634.62,-83.28 628.62,-89.28 622.62,-89.28" />
<text xml:space="preserve" text-anchor="middle" x="558.97" y="-69.24" font-family="Noto Sans CJK SC" font-size="12.00">段2 · scale</text>
<text xml:space="preserve" text-anchor="middle" x="558.97" y="-51.24" font-family="Noto Sans CJK SC" font-size="12.00">每32个MXFP8元素1B</text>
<text xml:space="preserve" text-anchor="middle" x="558.97" y="-33.24" font-family="Noto Sans CJK SC" font-size="12.00">8B补齐长度由软件配置</text>
<text xml:space="preserve" text-anchor="middle" x="558.97" y="-15.24" font-family="Noto Sans CJK SC" font-size="12.00">CM scale或MM软件区域</text>
</g>

<g id="diagram7e133923141a-edge2" class="edge">
<title>n1-&gt;n2</title>
<path fill="none" stroke="#475569" d="M416.11,-44.64C435.28,-44.64 455.41,-44.64 474.29,-44.64" />
<polygon fill="#475569" stroke="#475569" points="474.14,-47.27 481.64,-44.64 474.14,-42.02 474.14,-47.27" />
<text xml:space="preserve" text-anchor="middle" x="449.58" y="-50.14" font-family="Noto Sans CJK SC" font-size="10.00">推进</text>
</g>

<g id="diagram7e133923141a-node4" class="node">
<title>n3</title>
<path fill="#f0fdfa" stroke="#0f766e" d="M869.91,-89.28C869.91,-89.28 714.12,-89.28 714.12,-89.28 708.12,-89.28 702.12,-83.28 702.12,-77.28 702.12,-77.28 702.12,-12 702.12,-12 702.12,-6 708.12,0 714.12,0 714.12,0 869.91,0 869.91,0 875.91,0 881.91,-6 881.91,-12 881.91,-12 881.91,-77.28 881.91,-77.28 881.91,-83.28 875.91,-89.28 869.91,-89.28" />
<text xml:space="preserve" text-anchor="middle" x="792.01" y="-69.24" font-family="Noto Sans CJK SC" font-size="12.00">段3 · topK</text>
<text xml:space="preserve" text-anchor="middle" x="792.01" y="-51.24" font-family="Noto Sans CJK SC" font-size="12.00">MU topK_table：16 × 256B</text>
<text xml:space="preserve" text-anchor="middle" x="792.01" y="-33.24" font-family="Noto Sans CJK SC" font-size="12.00">经M Top拆成两笔128B</text>
<text xml:space="preserve" text-anchor="middle" x="792.01" y="-15.24" font-family="Noto Sans CJK SC" font-size="12.00">B Core也可连续保存到MM</text>
</g>

<g id="diagram7e133923141a-edge3" class="edge">
<title>n2-&gt;n3</title>
<path fill="none" stroke="#475569" d="M634.99,-44.64C653.5,-44.64 673.6,-44.64 693.02,-44.64" />
<polygon fill="#475569" stroke="#475569" points="692.87,-47.27 700.37,-44.64 692.87,-42.02 692.87,-47.27" />
<text xml:space="preserve" text-anchor="middle" x="668.37" y="-50.14" font-family="Noto Sans CJK SC" font-size="10.00">推进</text>
</g>
</g>
</svg>
```

### MSG 包在 Router 线上的样子

包头Byte0是最高字节，字段位置与Router一致。

| 字节／位 | 内容 | 出Core操作 |
| - | - | - |
| Byte0，[255:248] | path_id | 改为任务PID |
| Byte1，Byte4，Byte10～15 | 保留 | HT来源为0，CM/MM来源保持 |
| Byte2～3，[239:224] | core_mask | 保持所读值 |
| Byte5，[215:208] | loopback／Hardware Used | 仅[3:2]改为任务VCID |
| Byte6～7，[207:192] | pkt_length | 改为32+使能数据段总长 |
| Byte8～9，[191:176] | user_id | 改为任务UID |
| Byte16～31，[127:0] | sw_aux | 保持所读16B |

CM/MM地址A+4k的32bit字等于header[32k+31:32k]，k=0～7，不按字段作字节翻转。故path_id在A+31，sw_aux在A+0～15。16bit字段的多字节含义按上表bit位置编解码，按固定 32B 位布局序列化。
Packet总长度为32B硬件头加各使能数据段长度。地址与长度按8B软件约束，16bit总长度最大65535，可编码的8B对齐总长度最大65528。大于32KiB的数据可流式经过Buffer；32KiB是每用户RMEM归约上下文容量，不是通用DTE搬运上限。

归约模型payload另有16B兼容前缀，Router加法跳过它；该前缀与32B头内sw_aux分别计算，硬件是否继续保留前缀尚未统一。

### 四类内容的长度与去处

| 内容 | 配置长度 | 计算Core典型端点 | B/R Core典型端点 |
| - | - | - | - |
| 段0包头 | 固定32B | HeaderTable，16组、每组32项，共800B | CM按UID保留32B头，软件可留64B间距 |
| data | 软件配置各段DATA_LEN | CM按SID分区 | MM软件槽 |
| scale | MXFP8每32元素1B，段长向上补齐8B | CM scale窗口 | B Core与数据连续保存MM；权重scale映射另配 |
| topK | 软件配置，MU表按256B整数倍 | MU topK_table，16×256B | B Core可作为普通256B段连续保存MM |

HeaderTable每组共享core_mask 2B和sw_aux 16B，有32个各1B Hardware Used；不是16个完整头项。段地址为9bit索引，软件回读地址0x3000+group×0x40是另一地址域。

四段没有按内容类型隐式生成长度。data、scale与topK落在同一个MM槽时仍可分三段，各自的地址、长度和有效位独立。

### 三类 core 各自怎么拆、怎么重组

| Core / 场景 | 入核配置 | 出核配置 |
| - | - | - |
| B Core token | 头到CM；6144B data、192B scale、256B topK连续到MM | 从相同三段读出；广播与组间转发各一条Entry |
| 计算Core广播 | 头到HeaderTable；data到CM；scale到CM scale；topK到MU表 | 软件按需要使能各段，硬件重写路由字段 |
| 计算Core归约/concat | 头可存配对HT项；BF16数据到CM，不自动产生scale | 读配对头和计算结果，包长由各段总长产生 |
| R Core | 头到CM；两输入半槽落MM、完成分别写flag | 一TS Task提交两Packet操作数到本级RMEM，仅最后Entry带ACK |
| 纯包头 | 段0长度32B，seg_valid=000 | 必须消费或发送一个head/tail Packet |

B Core逻辑有效载荷6592B，软件槽跨度6656B；跨度不是CFG_STRIDE，MM段仍配stride=0。R Core每半槽0x3080B，兼容归约数据段含16B前缀与12288B BF16数据，剩余槽空间是软件padding。

### 什么时候存，什么时候丢

Router源时hw_header_op=0保存段0，=1丢弃段0；后者仍消费Packet，但不写包头端点。Core源或Core内搬运始终搬32B段0，不能用这个位取消源头读取。

出Core重写path_id、user_id、loopback中的VCID与pkt_length，保持core_mask和sw_aux；CM/MM头中保留字节保持，HT生成头中保留字节为0。修改core_mask或软件辅助信息由软件显式写入所选包头端点，不靠DTE自动MoE路由。

软件为配对DataIn/DataOut选择HT组内项号。后到concat写某项会共享覆盖该组core_mask/sw_aux；不能假定32项各自都有独立软件头。

### shareMem 写

SM Ptr仅在指向Done时发可选4B控制写，addr/data来自Trigger快照，valid且未ready时保持。Fire后记录对应Entry outstanding，bresp到达置SM Resp Flag；Ack不能提前退休。一个使能Entry恰好写一次，即使ack_ts_en=0也完成SM副作用和有序退休。

TS输出UID16、SID4、TID6和valid1，valid-only无Ready。Ack Ptr对ack_ts_en=1的Entry产生一拍，随后释放；ack_ts_en=0仍按序释放。TS观察顺序为Trigger接纳顺序，与不同Channel的物理完成顺序分开。

SM物理接口对齐128B端口，但控制写有效4B，byte mask低4位为1，其余0；接口数据的有效控制字为低32bit。请求valid/ready，响应valid-only。SM payload物理宽度不等于控制字长度。
完成flag只在数据真实落地后写入，软件轮询不能把“提交DTE请求”当作ready。B Core完成地址按落点/6656计算，R Core按落点/0x3080计算；表基址、内容、是否写入均在对应Entry快照中保存。

搬入使能flag写，搬出后清标志由相应kernel的软件队列管理；不能笼统规定所有搬出DSA都自动写invalid。

## 软件怎么配一个任务

DTE 只有一个任务入口：所有任务（进核 + 出核）都由 RV core 配寄存器 + 写 `CFG_TRIGGER` 起，Regfile 快照 Descriptor 送进 Commit 的中央 TaskQueue。Router 入站帧不是任务入口——Header Parser 只存包头、转发 payload。

这一节讲 RV core 这一侧：写哪些寄存器、每个参数管什么、地址怎么算出来、五个方向各自怎么配。

### Fast LUT与Config Template的取舍

当前详设取消按TID绕过RV的Fast LUT，配置加速采用8套Config Template。模板只保存常量字段；RV每笔覆盖变化字段并最后写Trigger。每套19字段、128B跨度，模板不包含Trigger。

旧Fast LUT提出的命中4T、未命中Core Latency+4T和Setup<10T是候选方案估算；85T是旧RV配置路径的端到端估算。它们保留为方案评估背景，不能代替当前kernel指令统计和真实访存/信用等待。

### Config字段与Trigger

| 信号 | 方向 | 位宽 | 合同 |
| - | - | - | - |
| rvcore2dte_rw_req_vld / dte2rvcore_rw_req_rdy | 输入／输出 | 各1 | 每拍最多Fire一笔 |
| rvcore2dte_rw_type | 输入 | 1 | 0读，1写 |
| rvcore2dte_rw_addr / wr_data | 输入 | 各32 | 字节地址和写数据 |
| rvcore2dte_sid / tid / uid / pid / vcid | 输入 | 4/6/16/8/2 | 与请求一起保持，Trigger Fire快照 |
| dte2rvcore_rd_rsp / rd_data | 输出 | 1/32 | 下一拍返回，无Ready、无ID |

valid且未ready时全部请求字段保持。Fire后不可撤销；RV kill只影响未接受请求。ready由Status和TaskQ满状态寄存产生，不组合依赖地址、读写、数据或已有未返回读。RUN满队列反压所有访问；ERROR恒ready=1，Trigger不入队而清dirty。STOP队满按用户确认的建模基线反压全部RV访问；原文另一种行为继续登记TBD。

各目标Fire当拍采样读值到自己的读寄存器，AddrDecode保存目标选择，下一拍直接选择并输出，不额外寄存、不限制在途读。连续读可每拍接收、每拍返回；之后写不改变之前读值。非法地址写无状态变化，读下一拍返回0。

模板t的基址为`0x1000+t×0x80`，内部19字段偏移0x00～0x48，不含Trigger；模板字段顺序与Config字段0x04～0x4C一致。t范围0～7。

普通写Fire更新对应Config字段并置dirty。模板写只改模板。Trigger Fire时，temp_valid=1则取选定模板并以dirty字段覆盖，temp_valid=0则取Config当前值；与请求SID/TID/UID/PID/VCID共同形成完整快照，同拍写全局Q并清dirty。Config不再保留一份等待Commit的Descriptor。

已接受任务不受后续Config/模板/CSR写入影响。无效段字段可保留旧值，但不参与执行；软件优先将无关字段写0。FastLUT已取消，PID由请求旁带给出，长度由各段DATA_LEN给出。

一个TS任务可包含多个DTE搬运条目，仅最后一条开启ack_ts_en。全局Q对各条搬运独立保存、执行与有序退休。

### 一次配置要写的参数

| 范围 | 目标 | 有效资源 |
| - | - | - |
| 0x0000～0x03FF | Config | Trigger和19个任务字段 |
| 0x0400～0x07FF | Status Ctrl | SYS_CTRL、SYS_STATUS、EXCEPT_*；具体地址尚未冻结 |
| 0x0800～0x0BFF | PMU | 任务、Channel和有效拍计数；位域/地址待冻结 |
| 0x0C00～0x0FFF | Debug | 尚未冻结 |
| 0x1000～0x1FFF | Template | 8套×128B，其余保留 |
| 0x2000～0x2FFF | TaskQ回读 | FullTaskDesc、异常、SM响应和Entry状态；完整字偏移待冻结 |
| 0x3000～0x3FFF | HeaderTable | 16组×64B占0x3000～0x33FF，其余保留 |

| 字段 | 地址 | 有效位宽 |
| - | - | - |
| CFG_TRIGGER | 0x0000 | [0]temp_valid，[3:1]temp_index；虚拟写事件 |
| CFG_ADDR0_SRC / DST | 0x0004 / 0x0008 | 各32 |
| CFG_ADDR1_SRC / DST | 0x000C / 0x0010 | 各32 |
| CFG_ADDR2_SRC / DST | 0x0014 / 0x0018 | 各32 |
| CFG_ADDR3_SRC / DST | 0x001C / 0x0020 | 各32 |
| CFG_STRIDE0～3 | 0x0024、0x0028、0x002C、0x0030 | 各32 |
| CFG_DATA_LEN0～3 | 0x0034、0x0038、0x003C、0x0040 | 各16，字节单位 |
| CFG_SM_W_ADDR / DATA | 0x0044 / 0x0048 | 各32 |
| CFG_TRANS_MODE | 0x004C | 10，模型位序mode[2:0]、保留[3]、seg_valid[6:4]、header[7]、SM[8]、ACK[9] |

任务配置字段复位0。TRANS_MODE的逻辑字段为transfer_mode3、seg_valid3、hw_header_op1、wr_sharemem_flag1、ack_ts_en1及reserved1。段0恒为包头，段1～3独立使能。丢弃包头只对Router源生效，出Core和Core内搬运仍搬32B段0。ack_ts_en只决定退休时是否通知TS。
### 配置参数的分工
| 参数组 | 软件配置 | 硬件使用 |
| - | - | - |
| 方向与段有效性 | TRANS_MODE及四段使能 | 选择Channel、源/目的端和包头操作 |
| 数据地址与长度 | 每段SRC、DST、STRIDE、DATA_LEN | 只对有效端算址，逐段连续执行 |
| 包头与附加内容 | 段0=32；其余段内容由软件约定 | HT/CM/MM/topK逐端口访问，不隐式推算长度 |
| 完成副作用 | SM地址/数据、SM使能、ACK使能 | 实际完成后按全局次序写SM并退休 |
| 请求身份 | CSR旁带SID/TID/UID/PID/VCID | Trigger Fire当拍保存，后续CSR变化无影响 |

模型TRANS_MODE为mode[2:0]、bit3保留、seg_valid[6:4]、header[7]、SM[8]、ACK[9]。这是用户确认的建模基线，原文另一位序仍TBD。旧TASK_CFG_ADDR/TD/PACK/TRG四寄存器和64B模板表不是当前可执行接口，其功能分别由四段字段、STUPV快照与128B模板承担。

### 原配置字段与现行四段字段的对应

这张表保留旧稿各参数解决的问题，供读旧软件资料时转换；旧TASK_CFG_*和64B模板不是当前可执行接口。

| 旧资料参数 | 当前表达 | 变化 |
| - | - | - |
| TASK_CFG_ADDR及src_addr/dst_addr | 每段CFG_ADDRi_SRC/DST | 四段各自独立有效端 |
| TASK_CFG_TD.src_sel/dst_sel | Route与地址端点译码 | 端点不再用一个全局二选一字段 |
| queue_sel | Channel Sel由Route及VCID生成 | InCh、OutCh0～3 |
| cnt及32KiB上限 | 各段DATA_LEN字节、Packet总长校验 | 通用DTE可流式超过32KiB；RMEM容量另限 |
| 32B粒度mask | 按地址/长度派生8B keep/mask | 首尾拍和段间重对齐由硬件处理 |
| TASK_CFG_TD.path_id | Trigger旁带PID8 | 与任务身份一起锁存 |
| task_pack_id及last | 全局entry_id、Entry生命周期、ack_ts_en | 内部归属不是软件任意重排包号 |
| PACK.UserID/stream_id/task_id | Trigger UID16/SID4/TID6 | CSR旁带，不单独配置PACK |
| PACK.no_ack | ack_ts_en=0 | 无ACK仍执行SM及有序退休 |
| 单一stream_stride | 四个CFG_STRIDEi | Core内源不叠，目的叠 |
| 专用scale/topK地址 | 通用段SRC/DST、长度、使能 | 内容与端点独立，不隐式推算长度 |
| hw_header_addr/sw_header_addr | 32B段0及HT组/项 | 16B软件字段与core_mask在HT组内共享 |
| router_ep_count×6 | 软件生成具体topK段长 | MU表一次逻辑项256B，经M Top两笔 |
| sharemem_waddr/data | CFG_SM_W_ADDR/DATA、bit8 | 仅在实际完成后按序写入 |
| 原64B模板11字段 | 128B跨度、19字段 | 8套、不含Trigger、dirty逐字段覆盖 |
| SYS_CTRL的task_trigger表述 | CFG_TRIGGER负责任务提交，Status负责运行控制 | 不能把resume当作业务Task提交 |
| 11项旧性能计数 | 当前8项逻辑PMU口径及模型观测 | 有效字节、物理拍、队头等待分别统计 |

### 地址怎么算

每段配置 SRC、DST、STRIDE、DATA_LEN，长度单位为字节。一般存储端地址为 `CFG_ADDRi + SID × CFG_STRIDEi`；stride=0 为物理地址。Core 内搬运只对目的端叠 stride，源端按软件已算好的地址使用。

段 0 的长度必须为 32。段 1～3 由 seg_valid[i−1] 独立使能，地址和长度均须 8B 对齐；无效段不参与乘加、校验、读写或包长。HeaderTable 段 0 地址是索引，不检查 8B 对齐；CM/MM 包头地址按 8B 对齐。

| 端点窗口 | 默认基址 | 默认大小 | 地址含义 |
| - | - | - | - |
| CM | 0x00000000 | 2MB | bit20 选择数据／scale；scale 实际 256×128B |
| MM | 0x10000000 | 36MB | 字节地址，基址按 64MB 对齐 |
| topK_table | 0x20000000 | 4KB | 16×256B；按 SID 使用时 stride=256 |
| HeaderTable 段地址 | 0x30000000 | 512 个索引 | bit[8:5]组号、bit[4:0]项号；一个索引对应一个32B包头 |

窗口基址与大小是实现参数，按全局地址图配置。乘加溢出、地址跨端点、段范围越窗口、topK 起点或长度不满足 256B 对齐，均须在对应外部事务之前被拦截。HeaderTable 的段地址窗口与软件寄存器地址域是两套地址。
### 地址与长度的算例
取SID=3，CM数据基址0x2000，stride=0x10000，得到0x32000；MM软件槽基址直接给最终地址、stride=0。段内再按有效字节offset推进，不能在每拍重复加SID偏移。

HeaderTable item=2、stride0=32时索引为3×32+2=98，即组3项2；软件回读同组从0x3000+3×0x40开始。MU topK使用SID×256，SID3对应字节偏移768，两笔128B地址在[0]路连续发出。

scale是自己的字节地址与长度：6144B MXFP8对应192B scale，192已是8B倍数。若data不是256B倍数，软件按段对齐规则配置scale长度，保留逻辑有效长度用于软件解释。

### 五个方向的配置例子

| 场景 | 段0 | 数据段 | 关键约束 |
| - | - | - | - |
| Router→CM | DST=EP_HTAB_BASE+item，stride0=32，len0=32 | DST=EP_CM_BASE+offset，stride为用户跨度，len为8B倍数 | SRC无效，ack_ts_en按是否反馈TS |
| Router→MM | DST可HT/CM/MM | DST=EP_MM_BASE+软件槽地址，stride=0 | 接收任务先驻留，目标按段地址选择 |
| CM/MM→Router | SRC为配对HT索引或32B存储包头 | SRC逐段配置，DST无效 | 包长加32B，VCID来自Trigger |
| MM→CM | SRC物理包头，DST为目标头 | SRC物理MM、DST CM+SID×stride | transfer_mode100；源不叠stride |
| CM→HeaderTable | SRC为CM32B包头，DST=EP_HTAB_BASE+item+SID×32 | 可为无数据段 | OutCh3 core_mode，不产生Router TX |
| 纯包头／空接收 | len0=32；空接收hw_header_op=1 | seg_valid=000 | 都需消费一个Packet；空接收不写存储 |

以下示例使用已确认的模型位序；硬件原文差异仍记录TBD。无关字段写0，模板dirty覆盖按单字段记录；软件调度保证所有Packet长度、项号配对和SM地址有效。
### 可执行配置顺序示例
下例使用kernel里的DTE_*地址常量与dsa_write包装；sid/tid/uid/pid/vcid由执行任务CSR提供。

```c
/* Router -> MM，32B头 + 6144B数据；模型mode=1、seg1=bit4。 */
dsa_write(DTE_ADDR0_DST, dte_ep(DTE_EP_CMEM, header_addr));
dsa_write(DTE_DATA_LEN0, 32);
dsa_write(DTE_STRIDE0, 0);
dsa_write(DTE_ADDR1_DST, dte_ep(DTE_EP_MMEM, slot_addr));
dsa_write(DTE_STRIDE1, 0);
dsa_write(DTE_DATA_LEN1, 6144);
dsa_write(DTE_TRANS_MODE, 1u | (1u << 4) | DTE_ACK_TS_EN);
dsa_write(DTE_TRIGGER, 0);
```

B Core真实token还要使能scale和topK两段，并以flag写代替业务ACK。MM→Router与CM→Router的模式分别为3和2，填SRC而不是把Router目标地址当Memory地址；Core内mode=4填两端并走OutCh3。模板Trigger值为1|(template_index<<1)，仅dirty字段覆盖模板。

### 各方向完整最小配置

以下C示例使用`bach.h`中的模型寄存器地址、端点窗口和已确认TRANS_MODE位序。每次先清19字段，显式配置两段，最后Trigger；真实kernel通常用模板减少常量写。`seg_valid`仅使能段1，数据长度512B，无scale/topK。

```c
static void example_clear(void) {
  dsa_write(DTE_ADDR0_SRC, 0u);
  dsa_write(DTE_ADDR0_DST, 0u);
  dsa_write(DTE_ADDR1_SRC, 0u);
  dsa_write(DTE_ADDR1_DST, 0u);
  dsa_write(DTE_ADDR2_SRC, 0u);
  dsa_write(DTE_ADDR2_DST, 0u);
  dsa_write(DTE_ADDR3_SRC, 0u);
  dsa_write(DTE_ADDR3_DST, 0u);
  dsa_write(DTE_STRIDE0, 0u);
  dsa_write(DTE_STRIDE1, 0u);
  dsa_write(DTE_STRIDE2, 0u);
  dsa_write(DTE_STRIDE3, 0u);
  dsa_write(DTE_DATA_LEN0, 32u);
  dsa_write(DTE_DATA_LEN1, 0u);
  dsa_write(DTE_DATA_LEN2, 0u);
  dsa_write(DTE_DATA_LEN3, 0u);
  dsa_write(DTE_SM_W_ADDR, 0u);
  dsa_write(DTE_SM_W_DATA, 0u);
  dsa_write(DTE_TRANS_MODE, 0u);
}

/* CM和HT段按SID分区；MM示例的固定物理地址只用于单Entry。 */
static void example_router_to_cm(void) {
  example_clear();
  dsa_write(DTE_ADDR0_DST, dte_ep(DTE_EP_HDR, 2u));
  dsa_write(DTE_STRIDE0, 32u);
  dsa_write(DTE_ADDR1_DST, 0x2000u);
  dsa_write(DTE_STRIDE1, 0x10000u);
  dsa_write(DTE_DATA_LEN1, 512u);
  dsa_write(DTE_TRANS_MODE, 0x210u);
  dsa_write(DTE_TRIGGER, 0u);
}

static void example_router_to_mm(void) {
  example_clear();
  dsa_write(DTE_ADDR0_DST, 0x400u);
  dsa_write(DTE_ADDR1_DST, dte_ep(DTE_EP_MMEM, 0x10000u));
  dsa_write(DTE_DATA_LEN1, 512u);
  dsa_write(DTE_TRANS_MODE, 0x211u);
  dsa_write(DTE_TRIGGER, 0u);
}

static void example_cm_to_router(void) {
  example_clear();
  dsa_write(DTE_ADDR0_SRC, dte_ep(DTE_EP_HDR, 2u));
  dsa_write(DTE_STRIDE0, 32u);
  dsa_write(DTE_ADDR1_SRC, 0x2000u);
  dsa_write(DTE_STRIDE1, 0x10000u);
  dsa_write(DTE_DATA_LEN1, 512u);
  dsa_write(DTE_TRANS_MODE, 0x292u);
  dsa_write(DTE_TRIGGER, 0u);
}

static void example_mm_to_router(void) {
  example_clear();
  dsa_write(DTE_ADDR0_SRC, 0x400u);
  dsa_write(DTE_ADDR1_SRC, dte_ep(DTE_EP_MMEM, 0x10000u));
  dsa_write(DTE_DATA_LEN1, 512u);
  dsa_write(DTE_TRANS_MODE, 0x293u);
  dsa_write(DTE_TRIGGER, 0u);
}

static void example_mm_to_cm(void) {
  example_clear();
  dsa_write(DTE_ADDR0_SRC, dte_ep(DTE_EP_MMEM, 0x10000u));
  dsa_write(DTE_ADDR0_DST, dte_ep(DTE_EP_HDR, 2u));
  dsa_write(DTE_STRIDE0, 32u);
  dsa_write(DTE_ADDR1_SRC, dte_ep(DTE_EP_MMEM, 0x10100u));
  dsa_write(DTE_ADDR1_DST, 0x2000u);
  dsa_write(DTE_STRIDE1, 0x10000u);
  dsa_write(DTE_DATA_LEN1, 512u);
  dsa_write(DTE_TRANS_MODE, 0x214u);
  dsa_write(DTE_TRIGGER, 0u);
}

/* 同一Core内mode=4还可复制纯32B包头到HeaderTable。 */
static void example_cm_to_header_table(void) {
  example_clear();
  dsa_write(DTE_ADDR0_SRC, 0x400u);
  dsa_write(DTE_ADDR0_DST, dte_ep(DTE_EP_HDR, 2u));
  dsa_write(DTE_STRIDE0, 32u);
  dsa_write(DTE_TRANS_MODE, 0x204u);
  dsa_write(DTE_TRIGGER, 0u);
}
```

| 示例 | mode[2:0] | seg_valid[6:4] | hw_header_op | ACK | 外部路径 |
| - | - | - | - | - | - |
| Router→CM | 0 | 001 | 0保存 | 1 | InCh RX及CM/HT写 |
| Router→MM | 1 | 001 | 0保存 | 1 | InCh RX及MM/CM头写 |
| CM→Router | 2 | 001 | 1改写 | 1 | OutCh，VCID取CSR旁带 |
| MM→Router | 3 | 001 | 1改写 | 1 | OutCh，VCID取CSR旁带 |
| MM→CM | 4 | 001 | 0，Core内不丢头 | 1 | OutCh3内存写 |
| CM→HeaderTable | 4 | 000 | 0，Core内不丢头 | 1 | OutCh3 HT写 |

启用SM写时再填`DTE_SM_W_ADDR/DATA`并置bit8；多搬运的前条目清bit9。示例中的固定MM地址用于单Entry，真实软件按UID或接收序号求不同槽位。例子中HT item2必须与数据的配对任务一致，MM源头必须已保存32B合法头。Router出站目的由Packet PID/路由表决定，不能用无效DST字段创造Memory写。

### TS 直接启动

TS只向DTE RV Core下发task PC和身份，RV执行配置指令后Trigger才建立搬运Entry。当前没有另一个TS→DTE Descriptor入口；所有普通、Special、权重和Core内搬运都走同一RVCore IF快照合同。

多条搬运可属于同一TS Task，但条目各占全局Q容量、各自具有段位和真实完成；只有最后带ack_ts_en的条目回TS。

### 双 Bank 与优先级

CM/MM逻辑256B请求由两个连续128B地址组成，两路ready都满足才Fire。每路响应分别握手，先到半拍暂存，等另一半后拼成一个Buffer拍；无tag响应按端点接纳顺序归属。

MM遇到MU同bank访问时遵循M Top/存储仲裁与反压，DTE保持请求；不同端口的迟到响应不得串给下一Entry。topK与MM共用M Top端口，sel区分，topK仅[0]路连续两笔128B。MM mask写需计入RMW路径，不将256B写mask当作两块独立随意写入。

### ERROR状态的推进边界

| 范围 | ERROR中的行为 |
| - | - |
| RV配置入口 | ready恒高，普通寄存器读写继续接受；Trigger接收后丢弃并清dirty |
| Commit／全局Entry | 停止新的Channel下发，保留异常队头及原描述符 |
| 已下发Channel | 保留任务上下文和请求／响应归属，按原接口握手继续处理在途事务；已Fire响应必须收集 |
| SM／Ack | 停止正常SM写与Ack退休，已经Fire的SM响应继续收集；错误条目不产生正常TS完成 |
| 恢复 | 清首错不等于取消条目或完成排空；残包终止、恢复和完整Flush协议保留待定 |

## 与 Router 的边界

### 入站帧格式

WR 侧按四段的目的端拆分数据，以 8B 粒度重对齐并形成字节 mask。包头可以写 HeaderTable、CM 或 MM。topK 与其他数据段使用同一段位机制，经 M Top 写入对应端点。

RX 在首 flit Fire 时校验 pkt_length，并在 tail 时校验 flit 数。无驻留任务、未收到 tail 又出现 head、长度不符均保留任务现场。写侧待全部写响应返回，并取得同任务的 RD drain_done 后才产生 lane_finish。
RX不在DTE内重新构造48B C++头；32B hflit与payload分别按硬件边界传递。head/tail属于Packet，不由长度计数生成一个新的包间边界。长度计数用于校验，与tail缺失或提前到达的检查共同工作。

### core 侧承担的桥接职责

模型CoreStation连接Router与DTE，承担接口适配与Packet上下文，不扩大DTE容量。RX按native ready接入，TX按每VC native信用真正发出；排入内部CoreData队列不代表尾flit已交付。

诊断上下文可保留GPU/token、reduce_seq和远端落点，便于数值回归；这些字段不扩展32B硬件包头或800B HT容量。LogicalPayload移除scale补齐等线上padding供主机比较，wire字节数仍单独校验。

## 参数

| 资源／统计 | 基线 |
| - | - |
| 全局Q | 16项，直到Ack退休 |
| Channel／issue lane | 5／10 |
| RD AGCU／WR Q | 每Channel1项／4项 |
| Active/retiring或Hold | 每lane最多当前和上一任务 |
| Buffer | 每Channel32×256B=8KB，共40KB |
| RD Outstanding | 深32，与预留槽容量一致 |
| HeaderTable／模板 | 800B／8×128B |
| 包长 | 32B头+数据总和≤65535；8B对齐最大65528 |
| OutCh3发包 | 相对Out0～2多1拍 |
| PMU逻辑计数 | 总任务、InCh及Out0～3任务、读有效拍、写有效拍，共8项 |

SM控制字按4B统计，物理总线128B不能计成每Task搬128B有效数据。topK每256B耗两笔[0]路事务；MM mask写包含RMW成本。配置到下发、issue到drain、Done到SM/Ack分别统计，队头等待不能算作lane执行时间。
### 模型延迟与硬件容量分别标注
CM/MM接口双向256B，MM模型读8T、写9T，Ctrl_noc读写时延与DTE端口分开统计；存储Owner最终延迟和cache边界仍TBD。端口延迟不可用Setup85T替代。

硬件Status/Debug/PMU字段未冻结与模型可观察地址不是同一件事。模型实现首错、STOP/RUN、step/cancel/Flush及观测计数，软件必须使用标明为模型的地址。

### 性能剖析口径

```text
setup_cycles = first_external_fire - trigger_fire
issue_cycles = last_request_fire - first_external_fire
drain_cycles = wr_finish - last_request_fire
sm_wait_cycles = sm_response - sm_fire
retire_wait_cycles = ack_retire - wr_finish
active_cycles = count(any_data_fire)
stall_cycles = count(valid && !ready)
```

读/写响应排空、SM等待、全局队头等待分别记录。Packet的有效载荷、线上padding、32B头以及topK物理两笔请求各按自己的口径计数。total_latency=ack_retire-trigger_fire；retire_wait包含SM等待，不把sm_wait再重复相加。

## 取舍

### 出口串行导致的死锁，两个方案怎么合成一个
按存储方向共用一条出口时，一个VC阻塞可把后面的其他VC任务一并挡住。按VC复制四条OutCh并按flit仲裁，保留同VC Packet所有权，使其他VC继续推进；代价是每Channel独立Buffer和上下文。

### 配对下发与全局退休
RD先开始而WR无落脚点会占满Buffer。Ch Ptr必须同拍接纳两侧；dispatch后Entry继续保留，年轻先完成不能使身份提前复用。代价是队头SM或异常可阻塞退休，但数据通道仍可在容量内独立推进。

多Entry提交必须有数量和依赖边界；当前R Core只提交两条。软件不能以无限拆分填满16项Q，使释放某VC资源所需的后续任务无法进入。按VC分通道解决出口阻塞，不取消全局容量约束。

### Active、Hold与读写领先
请求发完允许切下一任务，上一任务迟到响应由Hold和entry_id归属保存。Buffer外prev/residue提供8B重对齐，避免多读口RAM；代价是需要额外保持跨拍及跨段字节。

### 四段通用配置与模板
按内容硬化scale/topK长度可减少字段，却限制端点和格式变化。四段独立地址/长度保留灵活性，8套模板减少常量重复配置；软件仍负责字段一致、包长与项号配对。

### 软件槽对齐
MM地址只要求8B合法，6592B槽合法但多数槽跨256B边界增加请求。6656B软件槽把起点对齐到物理拍，逻辑有效载荷仍6592B；额外64B占用与访存请求减少一起纳入容量/性能评估。
