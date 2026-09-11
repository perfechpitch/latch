# TS 任务调度器

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../../../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → chip → core → **TS**

给实现 TS 的人：各模块做哪些事、端口与存储怎么定。模块划分照 TS MAS 的顶层模块表，模块内部的做法照 TS 详细设计（下称 LLD）；两者不一致的地方按 MAS，冲突记在第 8 章。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《Task Scheduler MAS》全篇，寄存器、配置检查与配置顺序以 Programming Model 一章为准
* 《Task Scheduler 详细设计》v0.5：Data In、Task Done、Task Generator、DTE Arbiter、Stream Map、MU/VU Arbitor、Task LUT 各节
* 《TS_通信机制》：`B_CORE_DIRECTION`、`trigger_task_chain_en`、超前发送窗口
* 《归约的完整过程》：“TS 与 DTE 侧的配合”
* 《软件栈》：“四类 core 的 TS 配置”“每个 task 的共同形状”

***

## 1　定位与边界

TS 是 core 的控制单元，一块配好就按固定逻辑跑的硬件，不是可编程的调度器。它的全部工作收在两张表和四个动作里。

* **两张表**：`task_chain` 是静态的，说清这类 core 的操作流长什么样；`stream_table` 是运行时的，说清每个在途用户走到了哪一步
* **四个动作**：数据进来时按用户建表并找出要派的搬入任务；判当前这一项能不能发；同一个执行单元有多个候选时选最老的；收到完成事件推进度，第 0 项到 End 全部做完时退休

边界上的接口：与 Router 的五路控制通路、给三个 RV core 的 task 下发、三个 RV core 与三个 DSA 的完成、配置口 `apb_cfg_ch`、异常上报口 `ts2corestatus_int_ch`。

三种工作模式由两个配置项选定，配好之后运行期间不变：

| 模式 | 怎么进入 | TS 在这个模式下做什么 |
| - | - | - |
| 权重加载 | `DATAIN_TASK_ATTR` 的 `WEIGHTS_MODE = 1` | 不启动任务链，不查配置。Router 来了数据就按 `DATAIN_TASK_PC` 派 DTE 搬运，不建 stream 表项；这期间回来的完成事件全部丢掉 |
| 普通 | `SELF_START = 0` | 完整的四个动作：Router trigger 建表、按 `task_chain` 逐项推进、三条通路发射、完成后推进度并退休 |
| 自启动 | `SELF_START = 1`，只有 B core 与 R core 用 | 配好之后直接建 `stream_num` 个表项，Task 0 不等 trigger 就发。进来的数据走 Bypass，不建表，数据收齐的标志由软件维护在 Share Mem 里；Task 0 循环查那个标志，查到才往下走 |

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1720 850" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif" role="img" aria-label="TS 第 0 层">
<title>TS 第 0 层</title>
<defs><marker id="eg" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#0f766e"/></marker><marker id="sg" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#0f766e"/></marker><marker id="ek" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#475569"/></marker><marker id="sk" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#475569"/></marker><marker id="eo" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#b45309"/></marker><marker id="so" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#b45309"/></marker><marker id="ep" markerWidth="10" markerHeight="10" refX="8.5" refY="4" orient="auto"><path d="M0,0 L9,4 L0,8 z" fill="#7c3aed"/></marker><marker id="sp" markerWidth="10" markerHeight="10" refX="0.5" refY="4" orient="auto"><path d="M9,0 L0,4 L9,8 z" fill="#7c3aed"/></marker></defs>
<rect x="0" y="0" width="1720" height="850" fill="#ffffff"/>
<text x="20" y="28" font-size="13" font-weight="700" fill="#111827">TS 任务调度器 · 第 0 层（Router 的控制通路画在右侧，RV core 与 DSA 在下方与左侧）</text>
<text x="20" y="48" font-size="10" fill="#6b7280">绿线 = 与 Router 的控制通路　灰线 = TS 内部与 RV core 的下发、完成　橙线 = credit 与退休　紫虚线 = 配置口</text>
<rect x="230" y="70" width="400" height="145" rx="4" fill="#f5f3ff" stroke="#7c3aed"/>
<text x="244" y="90" font-size="12" font-weight="700" fill="#111827">CFG_REG</text>
<text x="244" y="108" font-size="9.5" fill="#475569">TASK_CHAIN_0～63：PC 与 ATTR 两个字，写 ATTR 置 VALID</text>
<text x="244" y="123" font-size="9.5" fill="#475569">DATAIN_TASK · STREAM_NUM · SELF_START · TS_STATE</text>
<text x="244" y="138" font-size="9.5" fill="#475569">ROUTER_TABLE_0～63：按 PID 存 TASK_DIR · TASK_VCID</text>
<text x="244" y="153" font-size="9.5" fill="#475569">B_CORE_DIRECTION · trigger_task_chain_en · 超前窗口</text>
<text x="244" y="168" font-size="9.5" fill="#475569">写 TS_INIT_FINISH 后查配置，结论写 TS_STATE</text>
<text x="244" y="183" font-size="9.5" fill="#475569">派生 THROUGH_END_MASK：第 0 项到唯一 End</text>
<text x="244" y="198" font-size="9.5" fill="#475569">按 PID 找搬入任务：WAIT_WAKE 项里没做完的最低一项</text>
<rect x="660" y="70" width="400" height="130" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="674" y="90" font-size="12" font-weight="700" fill="#111827">Task_ctrl</text>
<text x="674" y="108" font-size="9.5" fill="#475569">只选 FINISH 且 end = 0 的 stream</text>
<text x="674" y="123" font-size="9.5" fill="#475569">search = after_current &amp; THROUGH_END_MASK</text>
<text x="674" y="138" font-size="9.5" fill="#475569">　　　　 &amp; ~done_bitmap，取最低位，一拍算出</text>
<text x="674" y="153" font-size="9.5" fill="#475569">搜索不回绕、不越过 End</text>
<text x="674" y="168" font-size="9.5" fill="#475569">初态：WAIT_WAKE 或 CREDIT_EN → WAIT，否则 READY</text>
<text x="674" y="183" font-size="9.5" fill="#475569">PID 更新任务的紧邻后继继承新 PID；整项原子写入</text>
<rect x="1090" y="70" width="320" height="145" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1104" y="90" font-size="12" font-weight="700" fill="#111827">credit 与退休</text>
<text x="1104" y="108" font-size="9.5" fill="#475569">CREDIT_EN 的普通任务向 Router 申请</text>
<text x="1104" y="123" font-size="9.5" fill="#475569">　{user, stream, task, path}，到手置 READY</text>
<text x="1104" y="138" font-size="9.5" fill="#475569">逐级 reduce：Rmem credit 每用户一份</text>
<text x="1104" y="153" font-size="9.5" fill="#475569">　下发占掉，Reduce Done 还</text>
<text x="1104" y="168" font-size="9.5" fill="#475569">Head-only 退休：第 0 项到 End 全部完成</text>
<text x="1104" y="183" font-size="9.5" fill="#475569">先发退休请求，Router 收下才清 valid</text>
<text x="1104" y="198" font-size="9.5" fill="#475569">自启动模式退休后补一个表项</text>
<rect x="230" y="320" width="400" height="130" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="244" y="340" font-size="12" font-weight="700" fill="#111827">完成事件合流</text>
<text x="244" y="358" font-size="9.5" fill="#475569">七路：三个 RV core、三个 DSA、Reduce Done</text>
<text x="244" y="373" font-size="9.5" fill="#475569">RECV_UNIT 00：RV core 的完成即完成</text>
<text x="244" y="388" font-size="9.5" fill="#475569">RECV_UNIT 01：同一执行单元两路都到才完成</text>
<text x="244" y="403" font-size="9.5" fill="#475569">TASK_TYPE 4：只认 Reduce Done，按 user_id 完成当前任务</text>
<text x="244" y="418" font-size="9.5" fill="#475569">当前任务 → FINISH；后面的项只点亮那一位</text>
<text x="244" y="433" font-size="9.5" fill="#475569">丢弃：权重加载期间全部；自启动模式 SID 15 / TID 63</text>
<rect x="660" y="320" width="400" height="160" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="674" y="340" font-size="12" font-weight="700" fill="#111827">Stream_table</text>
<text x="674" y="358" font-size="9.5" fill="#475569">16 项顺序 FIFO，在途上限 stream_num</text>
<text x="674" y="373" font-size="9.5" fill="#475569">用户级：valid · user_id · user_id_vld · reissue</text>
<text x="674" y="388" font-size="9.5" fill="#475569">进度：task_id · task_fsm · done_bitmap（64 位）</text>
<text x="674" y="403" font-size="9.5" fill="#475569">当前任务：task_unit · task_recv · task_type · task_pc</text>
<text x="674" y="418" font-size="9.5" fill="#475569">　　　　　task_path_id · pid_pending · end</text>
<text x="674" y="433" font-size="9.5" fill="#475569">Rmem credit：rmem_busy</text>
<text x="674" y="448" font-size="9.5" fill="#475569">task_fsm：IDLE → WAIT → READY → INFLY → FINISH</text>
<text x="674" y="463" font-size="9.5" fill="#475569">八个写口按固定优先级仲裁，请求保持到 accepted</text>
<rect x="1090" y="320" width="320" height="145" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1104" y="340" font-size="12" font-weight="700" fill="#111827">User_Match</text>
<text x="1104" y="358" font-size="9.5" fill="#475569">user_id 比对：老用户复用，新用户建表</text>
<text x="1104" y="373" font-size="9.5" fill="#475569">按 PID 找搬入任务 t，按 t 的 TASK_TYPE</text>
<text x="1104" y="388" font-size="9.5" fill="#475569">　与 reissue 定派不派 DTE、跳过哪几项</text>
<text x="1104" y="403" font-size="9.5" fill="#475569">跳过位并进 done_bitmap</text>
<text x="1104" y="418" font-size="9.5" fill="#475569">自启动与权重加载模式走 Bypass：</text>
<text x="1104" y="433" font-size="9.5" fill="#475569">　不建表，PC 取 DATAIN_TASK_PC，SID 15 / TID 63</text>
<text x="1104" y="448" font-size="9.5" fill="#475569">不设入口队列，条件不满足拉低 ready</text>
<rect x="230" y="600" width="190" height="100" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="244" y="620" font-size="12" font-weight="700" fill="#111827">MU_Arb</text>
<text x="244" y="638" font-size="9.5" fill="#475569">候选：READY 且 unit = MU</text>
<text x="244" y="653" font-size="9.5" fill="#475569">按年龄选最老，宽度 1</text>
<text x="244" y="668" font-size="9.5" fill="#475569">非抢占保持到 raw ACCEPT</text>
<text x="244" y="683" font-size="9.5" fill="#475569">ACCEPT 后置 INFLY</text>
<rect x="440" y="600" width="190" height="100" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="454" y="620" font-size="12" font-weight="700" fill="#111827">VU_Arb</text>
<text x="454" y="638" font-size="9.5" fill="#475569">候选：READY 且 unit = VU</text>
<text x="454" y="653" font-size="9.5" fill="#475569">按年龄选最老，宽度 1</text>
<text x="454" y="668" font-size="9.5" fill="#475569">非抢占保持到 raw ACCEPT</text>
<text x="454" y="683" font-size="9.5" fill="#475569">ACCEPT 后置 INFLY</text>
<rect x="660" y="600" width="400" height="130" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="674" y="620" font-size="12" font-weight="700" fill="#111827">DTE_Arb</text>
<text x="674" y="638" font-size="9.5" fill="#475569">候选：READY 且 unit = DTE 的 stream，加搬入那一格</text>
<text x="674" y="653" font-size="9.5" fill="#475569">全部按相对 head_ptr 的年龄，同一 stream 时 Generated 先</text>
<text x="674" y="668" font-size="9.5" fill="#475569">Bypass 那一格不占 stream，按最老算</text>
<text x="674" y="683" font-size="9.5" fill="#475569">非抢占保持到 raw ACCEPT</text>
<text x="674" y="698" font-size="9.5" fill="#475569">vcid = ROUTER_TABLE[task_path_id].TASK_VCID</text>
<text x="674" y="713" font-size="9.5" fill="#475569">ACCEPT 后：Generated 置 INFLY，reduce 占 Rmem credit</text>
<rect x="1090" y="600" width="320" height="100" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="1104" y="620" font-size="12" font-weight="700" fill="#111827">DataIn_task_table</text>
<text x="1104" y="638" font-size="9.5" fill="#475569">1 项：{task_pc, task_id, user_id, path_id, stream_id}</text>
<text x="1104" y="653" font-size="9.5" fill="#475569">要派 DTE 的那一笔登记，只跳过的不占</text>
<text x="1104" y="668" font-size="9.5" fill="#475569">被占住时反压要派 DTE 的 trigger</text>
<text x="1104" y="683" font-size="9.5" fill="#475569">DTE RV core 收下即释放</text>
<rect x="20" y="600" width="180" height="85" rx="4" fill="#f8fafc" stroke="#374151"/>
<text x="34" y="620" font-size="12" font-weight="700" fill="#111827">Except Check</text>
<text x="34" y="638" font-size="9.5" fill="#475569">多次完成 · 非法 ID</text>
<text x="34" y="653" font-size="9.5" fill="#475569">PID 匹配不到 · 超时</text>
<text x="34" y="668" font-size="9.5" fill="#475569">本轮只留接口</text>
<polygon points="30,120 200,120 190,150 20,150" fill="#ffffff" stroke="#7c3aed"/>
<text x="110" y="139" font-size="9.5" fill="#7c3aed" text-anchor="middle">apb_cfg_ch</text>
<polygon points="30,345 200,345 190,375 20,375" fill="#ffffff" stroke="#475569"/>
<text x="110" y="364" font-size="9.5" fill="#475569" text-anchor="middle">三个 RV core、三个 DSA 的完成</text>
<polygon points="30,405 200,405 190,435 20,435" fill="#ffffff" stroke="#0f766e"/>
<text x="110" y="424" font-size="9.5" fill="#0f766e" text-anchor="middle">rmem2ts_done_ch</text>
<polygon points="30,790 200,790 190,820 20,820" fill="#ffffff" stroke="#475569"/>
<text x="110" y="809" font-size="9.5" fill="#475569" text-anchor="middle">ts2corestatus_int_ch</text>
<polygon points="1460,85 1700,85 1690,115 1450,115" fill="#ffffff" stroke="#b45309"/>
<text x="1575" y="104" font-size="9.5" fill="#b45309" text-anchor="middle">ts2router_req</text>
<polygon points="1460,130 1700,130 1690,160 1450,160" fill="#ffffff" stroke="#b45309"/>
<text x="1575" y="149" font-size="9.5" fill="#b45309" text-anchor="middle">router2ts_credit_ch</text>
<polygon points="1460,175 1700,175 1690,205 1450,205" fill="#ffffff" stroke="#b45309"/>
<text x="1575" y="194" font-size="9.5" fill="#b45309" text-anchor="middle">ts2router_credit_release_ch</text>
<polygon points="1460,360 1700,360 1690,390 1450,390" fill="#ffffff" stroke="#0f766e"/>
<text x="1575" y="379" font-size="9.5" fill="#0f766e" text-anchor="middle">router2ts_trigger_ch</text>
<polygon points="240,790 420,790 410,820 230,820" fill="#ffffff" stroke="#475569"/>
<text x="325" y="809" font-size="9.5" fill="#475569" text-anchor="middle">ts2mucore_task_ch</text>
<polygon points="450,790 630,790 620,820 440,820" fill="#ffffff" stroke="#475569"/>
<text x="535" y="809" font-size="9.5" fill="#475569" text-anchor="middle">ts2vucore_task_ch</text>
<polygon points="770,790 960,790 950,820 760,820" fill="#ffffff" stroke="#475569"/>
<text x="860" y="809" font-size="9.5" fill="#475569" text-anchor="middle">ts2dtecore_task_ch</text>
<path d="M198 135 L229 135" stroke="#7c3aed" stroke-width="1.4" fill="none" stroke-dasharray="5 3" marker-end="url(#ep)"/>
<path d="M196 360 L229 360" stroke="#475569" stroke-width="1.4" fill="none" marker-end="url(#ek)"/>
<path d="M196 420 L229 420" stroke="#0f766e" stroke-width="1.4" fill="none" marker-end="url(#eg)"/>
<path d="M110 685 L110 789" stroke="#475569" stroke-width="1.4" fill="none" marker-end="url(#ek)"/>
<path d="M1410 100 L1452 100" stroke="#b45309" stroke-width="1.4" fill="none" marker-end="url(#eo)"/>
<path d="M1452 145 L1411 145" stroke="#b45309" stroke-width="1.4" fill="none" marker-end="url(#eo)"/>
<path d="M1410 190 L1448 190" stroke="#b45309" stroke-width="1.4" fill="none" marker-end="url(#eo)"/>
<path d="M1452 375 L1411 375" stroke="#0f766e" stroke-width="1.4" fill="none" marker-end="url(#eg)"/>
<path d="M325 700 L325 789" stroke="#475569" stroke-width="1.4" fill="none" marker-end="url(#ek)"/>
<path d="M535 700 L535 789" stroke="#475569" stroke-width="1.4" fill="none" marker-end="url(#ek)"/>
<path d="M860 730 L860 789" stroke="#475569" stroke-width="1.4" fill="none" marker-end="url(#ek)"/>
<path d="M630 140 L659 140" stroke="#7c3aed" stroke-width="1.4" fill="none" stroke-dasharray="5 3" marker-end="url(#ep)"/>
<path d="M430 215 L430 319" stroke="#7c3aed" stroke-width="1.4" fill="none" stroke-dasharray="5 3" marker-end="url(#ep)"/>
<rect x="433.0" y="261" width="115.6" height="12" fill="#ffffff" opacity="0.92"/>
<text x="436" y="270" font-size="9" fill="#7c3aed" text-anchor="start">RECV_UNIT · TASK_TYPE</text>
<path d="M580 215 L580 290 L1250 290 L1250 319" stroke="#7c3aed" stroke-width="1.4" fill="none" stroke-dasharray="5 3" marker-end="url(#ep)"/>
<rect x="822.4" y="285" width="165.3" height="12" fill="#ffffff" opacity="0.92"/>
<text x="905" y="294" font-size="9" fill="#7c3aed" text-anchor="middle">按 PID 找搬入任务 · Task 0 的属性</text>
<path d="M800 200 L800 319" stroke="#475569" stroke-width="1.4" fill="none" marker-end="url(#ek)"/>
<rect x="803.0" y="241" width="42.5" height="12" fill="#ffffff" opacity="0.92"/>
<text x="806" y="250" font-size="9" fill="#475569" text-anchor="start">install</text>
<path d="M940 320 L940 201" stroke="#475569" stroke-width="1.4" fill="none" marker-end="url(#ek)"/>
<rect x="943.0" y="241" width="24.0" height="12" fill="#ffffff" opacity="0.92"/>
<text x="946" y="250" font-size="9" fill="#475569" text-anchor="start">快照</text>
<path d="M1090 205 L1075 205 L1075 340 L1061 340" stroke="#b45309" stroke-width="1.4" fill="none" marker-start="url(#so)" marker-end="url(#eo)"/>
<rect x="1079.0" y="241" width="131.3" height="12" fill="#ffffff" opacity="0.92"/>
<text x="1082" y="250" font-size="9" fill="#b45309" text-anchor="start">credit_wake · retirement</text>
<path d="M1090 400 L1061 400" stroke="#475569" stroke-width="1.4" fill="none" marker-end="url(#ek)"/>
<path d="M630 385 L659 385" stroke="#475569" stroke-width="1.4" fill="none" marker-end="url(#ek)"/>
<path d="M1250 465 L1250 599" stroke="#475569" stroke-width="1.4" fill="none" marker-end="url(#ek)"/>
<rect x="1253.0" y="526" width="42.0" height="12" fill="#ffffff" opacity="0.92"/>
<text x="1256" y="535" font-size="9" fill="#475569" text-anchor="start">登记搬入</text>
<path d="M820 480 L820 599" stroke="#475569" stroke-width="1.4" fill="none" marker-end="url(#ek)"/>
<rect x="823.0" y="531" width="55.3" height="12" fill="#ffffff" opacity="0.92"/>
<text x="826" y="540" font-size="9" fill="#475569" text-anchor="start">READY 候选</text>
<path d="M940 600 L940 481" stroke="#475569" stroke-width="1.4" fill="none" marker-end="url(#ek)"/>
<rect x="943.0" y="531" width="32.1" height="12" fill="#ffffff" opacity="0.92"/>
<text x="946" y="540" font-size="9" fill="#475569" text-anchor="start">issue</text>
<path d="M700 480 L700 560 L325 560 L325 599" stroke="#475569" stroke-width="1.4" fill="none" marker-end="url(#ek)"/>
<rect x="563.5" y="555" width="97.1" height="12" fill="#ffffff" opacity="0.92"/>
<text x="612" y="564" font-size="9" fill="#475569" text-anchor="middle">READY 候选 · issue</text>
<path d="M535 560 L535 599" stroke="#475569" stroke-width="1.4" fill="none" marker-end="url(#ek)"/>
<path d="M1090 640 L1061 640" stroke="#475569" stroke-width="1.4" fill="none" marker-end="url(#ek)"/>
<path d="M1060 680 L1089 680" stroke="#475569" stroke-width="1.4" fill="none" marker-end="url(#ek)"/>
</svg>
```

***

## 2　功能清单

模块划分照 TS MAS 的顶层模块表，下面的小节按这个划分组织。LLD 按功能分节，与这些模块的对应如下。

| MAS 模块 | LLD 对应的节 | 模型文件（`src/bach/ip/chip/core/ts/`） |
| - | - | - |
| `CFG_REG` | Task LUT，加各配置寄存器 | `cfg_reg.h` |
| `User_Match` | Data In | `user_match.h` |
| `DataIn_task_table` | Data In 的 Candidate Hold | `user_match.h` |
| `Stream_table` | Stream Map | `stream_table.h` |
| `Task_ctrl` | Task Generator | `task_ctrl.h` |
| `DTE_Arb` | DTE Arbiter | `dte_arb.h` |
| `MU_Arb`、`VU_Arb` | MU/VU Arbitor | `mu_vu_arb.h` |
| `Except Check` | EXCEPT_CHECK（TBD） | 未建 |

MAS 把完成事件的处理、credit 与退休都写在 `Stream_table` 的功能里，模型各设一个模块：完成事件合流对应 MAS 的 `Task_done` 一节与 LLD 的 Task Done，模型文件是 `task_done.h`；credit 与退休对应 MAS 功能清单的 credit 机制与 `Stream_table` 的用户释放，模型文件是 `credit_monitor.h`。

一功能一条，编号供“机制覆盖”一章引用。

### CFG_REG

| 编号 | 功能 |
| - | - |
| F1 | `TASK_CHAIN_0～63` 各占两个 32 位寄存器：`TASK_CHAIN_n_PC` 在 `0x000 + 8n`，`TASK_CHAIN_n_ATTR` 在 `0x004 + 8n`。软件每项先写 PC 再写 ATTR，写 ATTR 时硬件把 `TASK_VALID` 置 1。这张表是一条全序链，位域里没有前驱表也没有后继表，依赖信息只有数组下标 |
| F2 | ATTR 位域：`TASK_SEND_UNIT[1:0]`（00 DTE、01 MU、10 VU）；`TASK_RECV_UNIT[3:2]`（00 只调 RV core，收到 RV core 的完成即完成；01 调 RV core 与 DSA，两路完成都到才完成）；`WAIT_WAKE[4]`（要等 Router 送来的数据唤醒，即搬入任务）；`TASK_TYPE[7:5]`；`TASK_P2P_REISSUE_TID[13:8]`（重发搬入配对的搬出任务）；`TASK_CREDIT_EN[14]`；`TASK_PATH_ID[22:15]`；`TASK_END[23]`；`TASK_VALID[31]` 只读。写不进的编码（`TASK_SEND_UNIT` 的 11，`TASK_RECV_UNIT` 的 10 与 11，`TASK_TYPE` 的 6 与 7）保持旧值 |
| F3 | `TASK_TYPE` 六档：0 普通；1 Broadcast 或 P2P 重发的搬出；2 Broadcast 重发的搬入；3 P2P 重发的搬入；4 逐级 reduce；5 PID 更新，这一项完成时带回新 PID，交给紧邻的后继 |
| F4 | `DATAIN_TASK_PC`（`0x200`）与 `DATAIN_TASK_ATTR`（`0x204`）：`WEIGHTS_MODE[0]`、`TASK_SEND_UNIT[2:1]` 只读且固定为 DTE、`TASK_VALID[31]`。只在权重加载与自启动两种模式下用 |
| F5 | 全局项：`STREAM_NUM`（`0x208`，`[4:0]`，超过 16 的值只写入 16）、`TS_INIT_FINISH`（`0x20C`）、`TS_STATE`（`0x210`，`[0]` 是 `TASK_CHAIN_ERROR`）、`SELF_START`（`0x214`） |
| F6 | `ROUTER_TABLE_0～63`（`0x400～0x4FC`）按 PID 索引，每项 `TASK_DIR[3:0]`（bit0 上下、bit1 左、bit2 右、bit3 本 core）与 `TASK_VCID[5:4]`。VCID 随 DTE 任务下发，方向只保存与读回 |
| F7 | 取自《TS_通信机制》、MAS 地址映射里没有的三项：`B_CORE_DIRECTION`（B core 的广播方向）、`trigger_task_chain_en`（允不允许 trigger 建表）、按 PID 索引的超前发送窗口 `{flowctl_en, window_n}` |
| F8 | 配置顺序照 MAS 的 Programming Sequence：先读 `TS_INIT_FINISH` 与 `TS_STATE`，确认都是 0。权重加载阶段只写 `DATAIN_TASK_PC`、置 `WEIGHTS_MODE`，搬完清掉。业务流配置先写 `STREAM_NUM` 与 `SELF_START` 并读回核对；B core 与 R core 重写 `DATAIN_TASK_PC`；逐项写任务链，每项先 PC 后 ATTR；最后 `TS_INIT_FINISH` 写 1。读 `TS_STATE` 有错就把 `TS_INIT_FINISH` 清零、改任务链、重来，没有错才放行 Router 的业务数据 |
| F9 | 写 `TS_INIT_FINISH` 后查整张配置表，权重加载时不查。查出任一项置 `TASK_CHAIN_ERROR`：自启动模式下 `task_chain` 与 `DATAIN_TASK` 没有都配；`TASK_TYPE` 为 1 或 4 却没标 `TASK_CREDIT_EN`；`TASK_TYPE` 为 3 却没标 `WAIT_WAKE`；多个 End，或 End 后面还有 valid 项；valid 项不连续；重发搬入的 `TASK_P2P_REISSUE_TID` 指向的不是类型 1 的项。模型另把“有 valid 项却没有 End”也算错 |
| F10 | 查表的同时派生两张 64 位掩码：`THROUGH_END_MASK` 是第 0 项到唯一 End 项，Task_ctrl 找后继、退休判断都只看这个范围；`DATA_IN_MASK` 是 `WAIT_WAKE = 1` 的那几项 |
| F11 | 按 PID 找搬入任务：在 `WAIT_WAKE = 1` 的项里找 `TASK_PATH_ID` 等于这个 PID、这个用户还没做完的最低一项。同一个 PID 可以对多项，按做完没有依次取；找不到就不派 DTE |

### User_Match

| 编号 | 功能 |
| - | - |
| F12 | trigger 带 `{user_id, path_id, reissue}`，valid/ready 握手。不设入口队列：条件不满足就拉低 `ready`，由 CoreStation 保持这一笔，每拍重判，不丢弃、不越过。这条通路上的 trigger 与 token 一一对应，丢一笔就等于丢一个 token |
| F13 | 普通模式下拿 `user_id` 与 `stream_table` 里已绑定的用户号比对：命中是老用户，复用原来的 `stream_id`；没命中是新用户 |
| F14 | 按 F11 找出搬入任务 t，再按 t 的 `TASK_TYPE` 与 trigger 的 `reissue` 定这一次派不派 DTE、跳过哪几项，取法见下表。t 的配对搬出是 t 的 `TASK_P2P_REISSUE_TID` 指的那一项 |
| F15 | 跳过的位直接并进这个用户的 `done_bitmap`；跳过的里含当前任务时，当前任务同时算做完 |
| F16 | 老用户不改当前任务与状态，只并跳过位；`reissue = 1` 时把表项的 `reissue` 置起来 |
| F17 | 新用户同时满足两条才建表：`trigger_task_chain_en = 1`，`tail_ptr − head_ptr < stream_num`。这一次要派 DTE 时还要 `DataIn_task_table` 空着，否则拉低 `ready` |
| F18 | 建表时在 `tail_ptr` 那一格一次写入：`valid`、`user_id`、`user_id_vld = 1`、`reissue`、`task_id = 0`、`done_bitmap` 等于跳过位、Task 0 的全部属性、`task_fsm`（跳过位含 Task 0 就是 FINISH，否则按 F38 由 Task 0 定），随后推进 `tail_ptr`。这一次不派 DTE 也照样建表 |
| F19 | 自启动与权重加载两种模式走 Bypass：不查 `stream_table`、不建表、不跳过，只把一笔搬入登记进 `DataIn_task_table`，PC 取 `DATAIN_TASK_PC`，身份用保留的 `stream_id = 15`、`task_id = 63` |
| F20 | 同一笔 trigger 会连着几拍出现在端口上，按序号认它，处理过的不再处理 |

| t 的 `TASK_TYPE` | trigger 的 `reissue` | 派 DTE | 跳过 |
| - | - | - | - |
| 2（Broadcast 重发的搬入） | 0 | 派 t | 配对的搬出 |
| 3（P2P 重发的搬入） | 0 | 不派 | t 与配对的搬出 |
| 2 或 3 | 1 | 派 t | 不跳 |
| 其余 | 0 或 1 | 派 t | 不跳 |

### DataIn_task_table

| 编号 | 功能 |
| - | - |
| F21 | 只有 1 项：`{valid, task_pc, task_id, user_id, path_id, stream_id}`。要派 DTE 的那一笔登记进来，只跳过的不占它 |
| F22 | 被占住时，要派 DTE 的 trigger 拉低 `ready`；只跳过、不派 DTE 的照常收下 |
| F23 | PC 的来源：普通模式取匹配到的那一项的 `TASK_CHAIN_n_PC`，Bypass 取 `DATAIN_TASK_PC`。Task 0 的 PC 进 `stream_table`，搬入任务的 PC 进这里 |
| F24 | DTE RV core 收下后立即释放，不改 `stream_table` 里的当前任务状态 |

### Stream_table

| 编号 | 功能 |
| - | - |
| F25 | 16 项顺序 FIFO，每项对应一个在途用户；`head_ptr` 与 `tail_ptr` 环形推进，在途上限是 `stream_num` |
| F26 | 用户级字段：`valid`、`user_id`、`user_id_vld`、`reissue`。用户号只有 `user_id` 一个：Router 与 credit 记账认它，软件读它算 R core 的用户映射表与 Matrix Mem 地址。普通模式建表时从 trigger 取；自启动模式建表时还没有，等 Task 0 的 RV core 完成带回来 |
| F27 | 进度三字段合起来才是完整进度：`task_id` 是链上的第几项，`task_fsm` 是这一项的状态，`done_bitmap` 是 64 位对应 64 项做完没有，做完的与跳过的都置位。提前完成的搬入任务表现为 `done_bitmap` 上某一位先亮而 `task_id` 还没走到那里 |
| F28 | 当前任务的属性摊平存在表项里，每次换 `task_id` 时从 `task_chain` 取：`task_unit`、`task_recv`、`task_type`、`task_pc`、`end`，以及实际 PID `task_path_id` |
| F29 | `task_path_id` 取自那一项的 `TASK_PATH_ID`；上一项是 PID 更新任务、这一项又紧邻它时，取它完成时带回的新 PID。`pid_pending = 1` 表示 `task_path_id` 里存的是还没被继承的新 PID |
| F30 | `rmem_busy` 是本级 Rmem 的 credit，每个用户一份：发出一笔逐级 reduce 就占掉，Router 报回这一笔做完才还 |
| F31 | `task_fsm` 五个状态：IDLE、WAIT（等 Router 送数据，或等 credit）、READY、INFLY、FINISH |
| F32 | **八个写口**，按来源命名，固定优先级仲裁，每口一拍一笔，请求保持到 `accepted` 才算生效。优先级由高到低：`retirement`（清 `valid` 并推 `head_ptr`）、`completion`（完成事件）、`install`（Task_ctrl 生成后继，整项写）、三个 `issue`（三条发射通路收到 ACCEPT 后置 INFLY）、`credit_wake`（credit 到了置 READY）、`create`（User_Match 建表或补跳过位）。落到不同 stream 的写同一拍都做 |
| F33 | 写失败分两种：整项写（新用户建表、`install`）失败后要重读最新表内容再来；只改几个字段的写失败后只重试这一笔，不能重新下发已经被 RV core 接收的任务 |
| F34 | `completion` 口的写：`done_bitmap[task_id]` 无条件置位；`task_fsm` 只有这一笔的 `task_id` 等于当前 `task_id` 时才改成 FINISH。同一笔还可以带三样：自启动 core 补 `user_id`、PID 更新任务写新 PID、逐级 reduce 还 Rmem credit |
| F35 | `credit_wake` 口：Router 授予 credit 时置 READY 并把 `reissue` 置起来；Rmem credit 到位时只置 READY |

### Task_ctrl

| 编号 | 功能 |
| - | - |
| F36 | 每个 stream 独立推进，只选 `valid = 1`、`task_fsm = FINISH`、`end = 0` 的 stream |
| F37 | 后继是当前任务之后、`THROUGH_END_MASK` 以内、`done_bitmap` 还没置位的最低一项：`search = after_current & THROUGH_END_MASK & ~done_bitmap`，一次 64 位优先编码一拍算出，连续跳过几项都不增加周期。搜索不回绕、不越过 End；`search = 0` 就不生成后继，由退休收尾 |
| F38 | 后继的初始状态：`WAIT_WAKE = 1` 或 `TASK_CREDIT_EN = 1` 置 WAIT，否则置 READY |
| F39 | PID 继承：上一项是 PID 更新任务、`pid_pending = 1`、后继正好紧邻它时，后继继承新 PID；跳过了紧邻那一项就用所选任务自己的 `TASK_PATH_ID`。装好后清 `pid_pending` |
| F40 | 新任务的 `task_id`、`task_fsm`、`end` 与全部属性经 `install` 口一起原子写入才算生成成功 |
| F41 | 模型每拍按年龄选一个 stream 装后继；LLD 是 16 个槽位各一份组合逻辑，同一拍各装各的 |

### DTE_Arb

| 编号 | 功能 |
| - | - |
| F42 | 候选：`valid = 1`、`task_fsm = READY`、`task_unit = DTE` 的 stream，加 `DataIn_task_table` 那一格 |
| F43 | 全部按相对 `head_ptr` 的 stream 年龄比，较老者优先；同一个 stream 上两者都在时先选 Generated。Bypass 那一格不占 stream，按最老算 |
| F44 | 选中后非抢占保持：命令与字段保持不变，直到 DTE RV core 回 `raw ACCEPT` |
| F45 | 命令带 `task_pc`、`stream_id`、`task_id`、`user_id`、`path_id`（当前任务的实际 PID）、`task_dsa_en`（`TASK_RECV_UNIT = 01` 时为 1）与 `vcid`。`vcid` 按实际 PID 查 `ROUTER_TABLE` 取 `TASK_VCID`；搬入那一格不查表，填 0 |
| F46 | 收到 ACCEPT 后：Generated 向 `stream_table` 提交 READY → INFLY，逐级 reduce 任务的这一笔同时占掉这个用户的 Rmem credit；搬入那一格只让 `DataIn_task_table` 出槽 |

### MU_Arb / VU_Arb

| 编号 | 功能 |
| - | - |
| F47 | 候选：`valid = 1`、`task_fsm = READY`、`task_unit = MU`（或 VU）；从 `head_ptr` 起选最老的 stream，发射宽度各 1；`task_dsa_en` 的取法同 DTE |
| F48 | 三条发射通路各自逐拍推进，同一拍可以并行下发 3 个 task |
| F49 | RV core 按 `task_queue` 是否有空槽回 `ready`；没收下之前 TS 不释放这一笔、不跳到下一个 |

### 完成事件合流

| 编号 | 功能 |
| - | - |
| F50 | 七路完成事件：DTE、MU、VU 各自的 RV core 完成与 DSA 完成共六路，加 Router 的 Reduce Done。七路都是脉冲，这里永远就绪、不向上游反压：完成事件没有重发通路，拒收就等于把那个 stream 永远停在当前任务 |
| F51 | 哪几路算数由那一项的 `TASK_RECV_UNIT` 定：00 只认 RV core 的完成；01 要同一个执行单元的两路都到，先到的一半按 `{执行单元, stream_id, task_id}` 记下，另一半到了才算完成，两半同拍到可以直接完成 |
| F52 | 逐级 reduce 任务（`TASK_TYPE = 4`）只认 Router 的 Reduce Done：按 `user_id` 找到那个 stream，完成的是它的当前任务，PID 不参与匹配。同一个用户同一时刻最多一笔 reduce 在做，由 F30 的 Rmem credit 保证。本地两路完成对 reduce 任务只算搬完，不改状态 |
| F53 | 完成的是当前任务就置 FINISH；是后面某一项（提前完成的搬入任务）就只点亮那一位，不推进 `task_id` |
| F54 | 两类完成直接丢掉：权重加载期间的全部完成事件；自启动模式下带 `stream_id = 15`、`task_id = 63` 的完成，它来自 Bypass 那一路搬入 |
| F55 | 自启动模式的表项没有用户号，Task 0 的 RV core 完成带回 `user_id` 时写进表项并置 `user_id_vld` |
| F56 | PID 更新任务（`TASK_TYPE = 5`）完成时，把 RV core 完成带回的 PID 写进 `task_path_id` 并置 `pid_pending` |
| F57 | VU 的 DSA 完成带 `event` 位，VU 的 `EVENT_EN` 置位时随完成一起拉高；TS 收下不处理 |

### credit 与退休

| 编号 | 功能 |
| - | - |
| F58 | `TASK_CREDIT_EN = 1` 的普通任务停在 WAIT 时向 Router 注册资源申请，带 `user_id`、`stream_id`、`task_id`、`path_id`，一拍一笔、按年龄选；Router 申请到后经 `router2ts_credit_ch` 通知，走 `credit_wake` 口置 READY |
| F59 | 逐级 reduce 任务要的是本级 Rmem 的 credit，TS 自己记，不向 Router 申请：停在 WAIT 且这个用户的 `rmem_busy = 0` 就置 READY。一笔 reduce 装不下时软件在链上配几项逐级 reduce 任务，一项一包，前一项的 Reduce Done 回来后一项才发 |
| F60 | credit 的单位是 stream 不是 task：Core Mem 空间按 stream 申请和释放 |
| F61 | Broadcast 重发：软件配一项 Broadcast 重发的搬入（类型 2）与它配对的搬出（类型 1，标 `TASK_CREDIT_EN`）。trigger 的 `reissue = 0` 表示下游收得下，搬出那一项在 trigger 那一刻就跳过；`reissue = 1` 表示要重发，走到搬出那一项时按 F58 申请 credit，到手后下发，由 DTE 从 Core Mem 把 token 搬回 Router |
| F62 | P2P 重发：软件把 bypass 本 core 的一段配成搬入（类型 3，标 `WAIT_WAKE`）与搬出（类型 1，标 `TASK_CREDIT_EN`）两项。`reissue = 0` 时两项都跳过、不派 DTE；`reissue = 1` 时搬入由 trigger 直接驱动，搬出走到时申请 credit，满足后下发 |
| F63 | 重发标记有效但还没重发成功时，这个用户的原始 token 数据不能被覆盖，用户也不能退休 |
| F64 | P2P 阻塞缓冲：软件可配开关、分给 P2P 缓存的 Core Mem 容量、开启的方向（最多 3 个）、每方向的容量与项数。TS 为每方向维护一张映射表（`p2p_vld`、`user_id`、`down_direction`、`data_addr`）与对应下游 core 的 credit 计数器，下游 credit 释放后发起一个 DTE 把数据搬回 Router 的任务，并通知上游释放 credit |
| F65 | B core 的搬出按 `B_CORE_DIRECTION` 查下游 core 的 credit；落 Matrix Mem 的那一侧不查，反压由 GPU 到 Bach 的两层 credit 兜底 |
| F66 | 广播任务的 `CreditCounter[path_id][stream_id]` 初值等于目的 core 数量，P2P 任务初值为 1；够了一次扣掉全部目的数再下发，不够就等 credit 释放 |
| F67 | 不派角色的 core 只按路由表透传，不检查 credit；上游要查的 credit 对应它之后那个落地的 core |
| F68 | Head-only 退休：只有 `head_ptr` 那一项能退休，条件是 `valid = 1` 且第 0 项到 End 的完成位全部置起。End 提前完成、当前任务停在 End 前面的也照样退休 |
| F69 | 退休顺序：先向 Router 持续发退休请求（带 `user_id`），Router 收下后才清 `valid`、推 `head_ptr`；退休请求经本 core 的 Router 通知上游，让上游的 credit 加一 |
| F70 | 自启动模式下表项从队头退休后，在 `tail_ptr` 补一个新表项：Task 0 的属性、没有用户号，接着等自启动任务 |

### Except Check

| 编号 | 功能 |
| - | - |
| F71 | MAS 列的异常：同一个任务返回多次完成；RV core 或 DSA 返回非法的 `task_id`、`stream_id`；Router 送来的 PID 在任务链里匹配不到，或匹配到的不是 DTE 任务；超时（搬入任务长时间等不到 trigger、用户长时间不退休）。经 `ts2corestatus_int_ch` 上报，本轮只留接口名。模型里 PID 匹配不到时不派 DTE，新用户照样建表 |

### 自启动（B core 与 R core）

| 编号 | 功能 |
| - | - |
| F72 | `SELF_START = 1` 时写完配置就建 `stream_num` 个表项，每项按 Task 0 的属性建、`task_fsm` 按 F38 定，激活这些表项的 Task 0 参与仲裁。B core 与 R core 的 Task 0 既不标 `WAIT_WAKE` 也不标 `TASK_CREDIT_EN`，所以一建好就是 READY。两者的 `stream_num` 配 16，自启动数因此是 16 |
| F73 | 进来的数据走 Bypass（F19），数据收齐的标志由软件维护，不在 TS 里更新 |
| F74 | 查标志的那一项占着一个 RV core 长期工作，不调 DSA（`TASK_RECV_UNIT = 00`）。它在这类 core 的链上没有别的活：B core 的链是 VU 查标志与 DTE 搬出，下面还有 EP 组时再加一项转发；R core 的链是 MU 查标志、DTE 搬入、VU 求和、DTE 搬出 |
| F75 | 表项按 stream 顺序激活查标志的任务；前一个 stream 查到之后软件清掉那个用户的标志，下一个 stream 才能接着查 |

### 超前发送窗口

| 编号 | 功能 |
| - | - |
| F76 | 判断依据：一个 TP / PPTP 区域内每个用户在每个 core 上的计算量与计算时间基本相同，看一个 core 的计算进度就能大致推算同区域其他 core 的进度。原文举的例子是 Core2 与 Core4 同一时刻在算的 `user_id` 相距 5～6 |
| F77 | 配置：某个 path 在某个 core 上配“流量控制使能”，使能后再配一个允许超前发送的用户窗口 `N`，`N` 编译期确定 |
| F78 | 判断：TS 查本 core 正在计算的 `user_id = M`，阻塞 `user_id ≥ M + N` 的传输；`user_id = M` 的计算任务完成后重新唤醒 `user_id = M + N` 的传输 |
| F79 | 只对 TP / PPTP 内部的 P2P 传输生效；跨 TP 组的计算时间不可推算，不适用 |
| F80 | 原始文档只写了“在一个 Core 配置”流量控制使能，没有指明配在 TS 还是 RouterTable。本文档按配在 TS 建模，因为判断要读 `stream_table` 里正在计算的 `user_id`，这个信息只有 TS 有 |

***

## 3　接口

```
port router2ts_trigger_ch (slave, valid/ready, clk)     // Router 的 CoreStation：Header 就绪即通知
  in  valid · user_id[15:0] · path_id[7:0] · reissue
  out ready                                             // 普通模式 = (老用户 || 建表两条满足) && (不派 DTE || DataIn_task_table 空)；Bypass = DataIn_task_table 空
port rmem2ts_done_ch (slave, 脉冲, clk)                // ReduceModule：一笔逐级 reduce 做完
  in  valid · user_id[15:0] · path_id[7:0]              // 按 user_id 找 stream，path_id 不参与匹配
port ts2router_req (master, valid/ready, clk)          // 注册资源申请
  out req_valid · user_id[15:0] · stream_id[3:0] · task_id[5:0] · path_id[7:0]
  in  req_ready                                         // = Router 的监听事件队列有空项
port router2ts_credit_ch (slave, 脉冲, clk)            // Router 的 CoreMemCreditMonitor：资源到手
  in  valid · stream_id[3:0] · task_id[5:0] · path_id[7:0]
port ts2router_credit_release_ch (master, valid/ready, clk)   // 用户退休，经 Router 给上游还 credit
  out valid · user_id[15:0]
  in  ready                                             // Router 收下后 TS 才清 valid 并推 head_ptr
port ts2<u>core_task_ch (master, valid/ready, clk)     // u ∈ {dte, mu, vu}：task 下发
  out valid · task_pc[31:0] · stream_id[3:0] · task_id[5:0] · user_id[15:0] · path_id[7:0] · task_dsa_en · vcid[1:0] · seq
  in  ready                                             // = 该 RV core 的 task_queue 有空槽（raw ACCEPT）；vcid 只在 u = dte 时有效
port <u>core2ts_done_ch (slave, 脉冲, clk)             // RV core 报完成；自启动的表项靠它补 user_id，PID 更新任务靠它带回新 PID
  in  valid · stream_id[3:0] · task_id[5:0] · user_id[15:0] · pid[7:0]
port <u>2ts_done_ch (slave, 脉冲, clk)                 // DSA 报完成
  in  valid · stream_id[3:0] · task_id[5:0] · event     // event 只在 u = vu 时有效
port ts2corestatus_int_ch (master, 电平, clk)          // 异常上报，本轮只留接口名
  out int_valid · int_code[7:0] · stream_id[3:0]
port apb_cfg_ch (slave, APB4, clk)                     // CFG_REG 的配置口，32 位整字写
  in  psel · penable · pwrite · paddr[14:0] · pwdata[31:0] · pstrb[3:0]
  out prdata[31:0] · pready · pslverr                   // pslverr：地址未对齐、未映射、写只读寄存器
```

***

## 4　存储器

```
mem task_chain     FF 阵列   64 × {valid, task_pc[31:0], send_unit[1:0], recv_unit[1:0], wait_wake, task_type[2:0], p2p_reissue_tid[5:0], credit_en, path_id[7:0], end}  1W 多读  写 ATTR 时硬件置 valid  复位 0
mem datain_task    FF        {task_pc[31:0], weights_mode, valid}                                    1R1W  软件写                        复位 0
mem ts_route       FF 阵列   64 × {dir[3:0], vcid[1:0]}，按 PID 索引                                  1R1W  软件逐项写                    复位 0    // ROUTER_TABLE
mem cfg_misc       FF        {stream_num[4:0], self_start, init_finish, ts_state, b_core_dir[2:0], trigger_task_chain_en}  1R1W  软件写  复位见 F5
mem task_masks     FF        {THROUGH_END_MASK[63:0], DATA_IN_MASK[63:0]}                             1R1W  写 TS_INIT_FINISH 时由 task_chain 派生  复位 0
mem path_flowctl   FF 阵列   64 × {flowctl_en, window_n[7:0]}，按 PID 索引                            1R1W  软件逐项写                    复位 0    // 超前发送窗口，配在哪一张表见 F80
mem stream_table   FF 阵列   16 × {valid, user_id[15:0], user_id_vld, reissue, task_id[5:0], task_fsm[2:0], done_bitmap[63:0], task_unit[1:0], task_recv[1:0], task_type[2:0], task_pc[31:0], task_path_id[7:0], pid_pending, end, rmem_busy}  8W 多读  八个写口按固定优先级仲裁  复位空
mem stream_ptr     FF        {head_ptr[4:0], tail_ptr[4:0]}                                           1RW   建表推 tail，退休推 head       复位 0
mem datain_hold    FF        1 项 {valid, task_pc[31:0], task_id[5:0], user_id[15:0], path_id[7:0], stream_id[3:0]}  1RW  占住即反压要派 DTE 的 trigger  复位空
mem ack_half       FF 阵列   每个未配齐的 {执行单元, stream_id, task_id} 一项 {core, dsa, user_id[15:0], pid[7:0]}  1RW  两半都到即清  复位空   // LLD 每个执行单元一项，模型不限项数
mem credit_cnt     FF 阵列   每 {path_id, stream_id} 一个计数器                                        1RW   广播初值 = 目的 core 数，P2P = 1  复位由输入给
mem p2p_buf_map    FF 阵列   每方向一张 × {p2p_vld, user_id[15:0], down_dir[2:0], data_addr[17:0]}      1RW   最多 3 个方向                 复位空
mem p2p_dn_credit  FF 阵列   每方向对应下游 core 的 credit 计数器                                      1RW   —                             复位由配置给
mem MATCH_CRE      级间 latch {is_new, user_id[15:0], stream_id[3:0], reissue, skip[63:0]}          —     每拍覆写                       —         // User_Match → create 口
mem 其余级间 latch  级间 latch 三条发射通路的命令保持、各写口的请求保持                                 —     保持到对方收下                 —
```

### 编译侧读入的表

`task_chain`、`datain_task`、`ts_route`、`cfg_misc` 四张由编译侧算好，产物里对应 `TCHAIN`、`DATAIN`、`TSRTAB`、`CFGMISC` 四种记录，boot 期经配置口按 F8 的顺序写入。`TSRTAB` 的 `TASK_DIR` 取这个 core 上那条 path 的路由表项（出方向，加上进不进本 core），`TASK_VCID` 按 PID 对 VC 数取模。另有一张 `credit_init`：每 `{path_id, stream_id}` 一个初值，广播任务等于目的 core 数量，P2P 任务是 1，每 core 一份。

DTE 另有一张按 path 查 `task_id` 的表（产物的 `PATHTASK` 记录），给进核那一笔的完成填 `task_id`，TS 这一侧不用它。

### 模型里三类 core 的任务链

`recv` 一栏是 `TASK_RECV_UNIT`，`type` 一栏是 `TASK_TYPE`。

**计算 core（EPTP-NN）**：FC1 与 FC3、门控、FC2 三项各在一个 task 里发几笔 DSA 任务，每笔都报一次完成，所以只收 RV core 那一路。

| 项 | 内容 | unit | recv | wait_wake | type | credit_en | path | end |
| - | - | - | - | - | - | - | - | - |
| 0 | token 搬入（`task_dte_user_init`） | DTE | 01 | 1 | 0 | 0 | 进核 path | 0 |
| 1 | FC1 / FC3 计算（`task_mu_fc13`） | MU | 00 | 0 | 0 | 0 | — | 0 |
| 2 | silu × FC3 加量化（`task_vu_gate`） | VU | 00 | 0 | 0 | 0 | — | 0 |
| 3 | FC2 计算加 EP Reduce（`task_mu_fc2`） | MU | 00 | 0 | 0 | 0 | — | 0 |
| 4～6 | FC2 结果出核，逐级 reduce，一项一包（`task_dte_send_moe_p0～p2`） | DTE | 01 | 0 | 4 | 1 | 归约 path | 第 6 项为 1 |

**B core**：`SELF_START = 1`，`STREAM_NUM = 16`，`B_CORE_DIRECTION` 配广播方向，`DATAIN_TASK_PC` 指 `task_dte_bc_datain`。

| 项 | 内容 | unit | recv | wait_wake | type | credit_en | path | end |
| - | - | - | - | - | - | - | - | - |
| 0 | 查有没有 ready 的数据（`task_bc_wait`） | VU | 00 | 0 | 0 | 0 | — | 0 |
| 1 | 广播出去（`task_dte_bc_send`） | DTE | 01 | 0 | 0 | 1 | 广播 path | 链尾那一组为 1 |
| 2 | 转给下一组的 B core（`task_dte_bc_relay`），下面还有 EP 组时才配 | DTE | 01 | 0 | 0 | 0 | 转发 path | 1 |

**R core**：`SELF_START = 1`，`STREAM_NUM = 16`，`DATAIN_TASK_PC` 指 `task_dte_rc_datain`。

| 项 | 内容 | unit | recv | wait_wake | type | credit_en | path | end |
| - | - | - | - | - | - | - | - | - |
| 0 | 查两笔是否集齐（`task_rc_find`） | MU | 00 | 0 | 0 | 0 | — | 0 |
| 1 | 从 Matrix Mem 搬两笔进 Core Mem（`task_dte_rc_load`） | DTE | 01 | 0 | 0 | 0 | — | 0 |
| 2 | 求和（`task_vu_add`） | VU | 00 | 0 | 0 | 0 | — | 0 |
| 3 | 结果出核（`task_dte_rc_send`） | DTE | 01 | 0 | 0 | 0 | 出核 path | 1 |

### 四种 core 级切分模式的任务链

bring-up 用 **EPTP-NN**，它每个 core 的任务链相同，就是上面计算 core 那一条，是能跑通第一个 token 的最小实例；跑通后换 EPTP-NK 的三种 core 角色，再上两种 PPTP。

**EPTP-NK：三种 core 角色**。三种角色各配各的链：取下表里打勾的项，按原顺序排。

| 步 | unit | 内容 | Normal | Concat | Chip Reduce |
| - | - | - | - | - | - |
| 0 | DTE | token data in | ✔ | ✔ | ✔ |
| 1 | MU | FC1 / FC3 计算 | ✔ | ✔ | ✔ |
| 2 | DTE | FC1 / FC3 out，Router 上 reduce | ✔ | ✔ | ✔ |
| 3 | DTE | FC1 / FC3 reduce 结果 data in | | ✔ | ✔ |
| 4 | VU | FC1 silu × FC3 加量化 | | ✔ | ✔ |
| 5 | DTE | FC2 input data out（广播） | | ✔ | ✔ |
| 6 | DTE | FC2 input data in | ✔ | | |
| 7 | MU | FC2 计算加 EP Reduce | ✔ | ✔ | ✔ |
| 8 | DTE | FC2 data out（concat） | ✔ | | |
| 9 | DTE | 与 chip 内其他 core concat，只在 chip 内最后一个 core 上做 | | ✔ | ✔ |
| 10 | DTE | chip FC2 结果 data out（chip reduce） | | ✔ | |
| 11 | DTE | chip FC2 结果 data in | | | ✔ |
| 12 | VU | chip FC2 结果 reduce | | | ✔ |
| 13 | DTE | FC2 结果 data out | | | ✔ |

**PPTP 两种模式的共同点**：PP 把 FC1、FC3、FC2 摆在不同行的 chip 上，**silu、dot、量化三步统一落在 FC2 段 chip 的逻辑 core 0**。FC1 段与 FC3 段各自把本段的结果送到那一个 core，dot 完再由它把结果当作 FC2 的 token 在本 chip 内广播。原始文档另有两处把 dot 摆在 FC3 chip，本套文档不采用，冲突记在第 8 章。

**PPTP-NK**：FC1 / FC3 chip 内切 K，每 core 出的是部分和，要先 chip 内逐级 reduce；FC2 chip 内切 N，出的是 y 的一段，所以是 concat。

| 段 | 步 | unit | 内容 |
| - | - | - | - |
| FC1 段 | 0 | DTE | token data in |
| FC1 段 | 1 | MU | FC1 计算 |
| FC1 段 | 2 | DTE | FC1 部分和 out，Router 上逐级 reduce |
| FC1 段 | 3 | DTE | reduce 结果 data in（只在 chip 内最后一个 core 上） |
| FC1 段 | 4 | DTE | reduce 结果 out 到 FC2 段 chip 的逻辑 core 0 |
| FC3 段 | 0～4 | 同上 | 把 FC1 换成 FC3，其余相同 |
| FC2 段 core 0 | 5 | DTE | 收 FC1 段的 reduce 结果 |
| FC2 段 core 0 | 6 | DTE | 收 FC3 段的 reduce 结果 |
| FC2 段 core 0 | 7 | VU | silu · dot · 量化 |
| FC2 段 core 0 | 8 | DTE | 结果作为 FC2 的 token 在本 chip 内广播 |
| FC2 段各 core | 9 | DTE | FC2 token data in |
| FC2 段各 core | 10 | MU | FC2 计算加 EP Reduce |
| FC2 段各 core | 11 | DTE | FC2 结果 out（concat） |
| FC2 段 concat core | 12 | DTE | 与 chip 内其他 core concat |
| FC2 段 concat core | 13 | DTE | concat 结果 out，跨 chip reduce |

**PPTP-NN**：FC1 / FC3 chip 内也切 N，每 core 出的就是 y 的一段，chip 内做 concat 而不是 reduce；FC2 chip 内切 K，出的是部分和，所以是逐级 reduce。

| 段 | 步 | unit | 内容 |
| - | - | - | - |
| FC1 段 | 0 | DTE | token data in |
| FC1 段 | 1 | MU | FC1 计算 |
| FC1 段 | 2 | DTE | FC1 结果 out（concat） |
| FC1 段 concat core | 3 | DTE | 与 chip 内其他 core concat |
| FC1 段 concat core | 4 | DTE | concat 结果 out 到 FC2 段 chip 的逻辑 core 0 |
| FC3 段 | 0～4 | 同上 | 把 FC1 换成 FC3，其余相同 |
| FC2 段 core 0 | 5 | DTE | 收 FC1 段的 concat 结果 |
| FC2 段 core 0 | 6 | DTE | 收 FC3 段的 concat 结果 |
| FC2 段 core 0 | 7 | VU | silu · dot · 量化 |
| FC2 段 core 0 | 8 | DTE | 结果作为 FC2 的 token 在本 chip 内广播 |
| FC2 段各 core | 9 | DTE | FC2 token data in |
| FC2 段各 core | 10 | MU | FC2 计算加 EP Reduce |
| FC2 段各 core | 11 | DTE | FC2 部分和 out，Router 上逐级 reduce |
| FC2 段 reduce core | 12 | DTE | chip 内 reduce 结果 data in |
| FC2 段 reduce core | 13 | DTE | 结果 out，跨 chip reduce |

***

## 5　流水线总览

TS 的三套时延数字口径不同：TS MAS 的 2～3 cycle 是硬件目标值，Top 模拟器的 16 T 是含 RV core 往返的端到端值，LLD 给的是逐级拍数。第 1 层图按 LLD 那一套画，另两套在图下的短句里对上。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1010 500" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="tsov" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="1010" height="500" fill="#ffffff"/>
  <text x="20" y="26" font-size="12" fill="#111827">TS · 第 1 层流水线总览（拍数取 TS LLD 的逐级值）</text>
  <text x="20" y="42" font-size="9.5" fill="#6b7280">横向是级序，不是拍序；每级的拍数在右上角 Dx。橙色虚线框是变长级，非按比例。</text>
  <path d="M150 52 L150 414" stroke="#e5e7eb" fill="none"/>
  <path d="M316 52 L316 414" stroke="#e5e7eb" fill="none"/>
  <path d="M482 52 L482 414" stroke="#e5e7eb" fill="none"/>
  <path d="M648 52 L648 414" stroke="#e5e7eb" fill="none"/>
  <path d="M814 52 L814 414" stroke="#e5e7eb" fill="none"/>
  <text x="20" y="102" font-size="10.5" fill="#6b7280">建表</text>
  <rect x="150" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="84" font-size="8.5" fill="#6b7280">M1</text>
  <text x="292" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="104" font-size="11" fill="#111827">trigger 与</text>
  <text x="160" y="118" font-size="11" fill="#111827">User_Match</text>
  <rect x="316" y="70" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="84" font-size="8.5" fill="#6b7280">M2</text>
  <text x="458" y="84" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="104" font-size="11" fill="#111827">CREATE 建表</text>
  <text x="326" y="118" font-size="11" fill="#111827">或补跳过位</text>
  <path d="M300 98 L315 98" stroke="#475569" marker-end="url(#tsov)" fill="none"/>
  <text x="20" y="188" font-size="10.5" fill="#6b7280">推进</text>
  <rect x="150" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="160" y="170" font-size="8.5" fill="#6b7280">M3</text>
  <text x="292" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="160" y="190" font-size="11" fill="#111827">WAKE</text>
  <text x="160" y="204" font-size="11" fill="#111827">credit 唤醒</text>
  <rect x="316" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="326" y="170" font-size="8.5" fill="#6b7280">M4</text>
  <text x="458" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="326" y="190" font-size="11" fill="#111827">选最老发射</text>
  <rect x="482" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="492" y="170" font-size="8.5" fill="#6b7280">M5</text>
  <text x="624" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="492" y="190" font-size="11" fill="#111827">ACCEPT</text>
  <text x="492" y="204" font-size="11" fill="#111827">回写 INFLY</text>
  <rect x="648" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="658" y="170" font-size="8.5" fill="#6b7280">M6</text>
  <text x="790" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="658" y="190" font-size="11" fill="#111827">完成事件合流</text>
  <rect x="814" y="156" width="150" height="56" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="824" y="170" font-size="8.5" fill="#6b7280">M7</text>
  <text x="956" y="170" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="824" y="190" font-size="11" fill="#111827">INSTALL</text>
  <text x="824" y="204" font-size="11" fill="#111827">生成后继</text>
  <path d="M300 184 L315 184" stroke="#475569" marker-end="url(#tsov)" fill="none"/>
  <path d="M466 184 L481 184" stroke="#475569" marker-end="url(#tsov)" fill="none"/>
  <path d="M632 184 L647 184" stroke="#475569" marker-end="url(#tsov)" fill="none"/>
  <path d="M798 184 L813 184" stroke="#475569" marker-end="url(#tsov)" fill="none"/>
  <text x="20" y="274" font-size="10.5" fill="#6b7280">退休</text>
  <rect x="150" y="242" width="150" height="56" fill="#fbf3df" stroke="#b45309" rx="4" stroke-dasharray="5 3"/>
  <text x="160" y="256" font-size="8.5" fill="#6b7280">M8</text>
  <text x="292" y="256" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="160" y="276" font-size="11" fill="#7c2d12">RETIRE</text>
  <text x="20" y="360" font-size="10.5" fill="#6b7280">credit</text>
  <rect x="150" y="328" width="150" height="56" fill="#fbf3df" stroke="#b45309" rx="4" stroke-dasharray="5 3"/>
  <text x="160" y="342" font-size="8.5" fill="#6b7280">M9</text>
  <text x="292" y="342" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="160" y="362" font-size="11" fill="#7c2d12">credit 申请</text>
  <text x="20" y="438" font-size="10.5" fill="#374151">完成到下一项被 RV core 收下是 M6、M7、M4、M5 各一拍，共 4 拍，与 LLD 流水级波形的 C0～C3 一致；Top 模拟器的 16 T 余下的</text>
  <text x="20" y="456" font-size="10.5" fill="#374151">是 RV core 取指与回完成的往返。M3 只管要 credit 的项，不要 credit 的后继由 M7 直接装成 READY。</text>
  <text x="20" y="484" font-size="10.5" fill="#374151">M8 等 Router 收下退休请求，M9 等 Router 授予 credit，这两级的拍数由 Router 定。</text>
</svg>
```

***

## 6　逐级行为

### M1 · trigger 与 User_Match

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1111 278" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="tsm1" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="1111" height="278" fill="#ffffff"/>
  <polygon points="30,20 208,20 198,96 20,96" fill="#f8fafc" stroke="#374151"/>
  <text x="114" y="39" font-size="10.5" fill="#374151" text-anchor="middle">router2ts_trigger_ch</text>
  <text x="114" y="57" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · user_id[15:0]</text>
  <text x="114" y="75" font-size="9.5" fill="#6b7280" text-anchor="middle">path_id[7:0] · reissue</text>
  <text x="114" y="93" font-size="9.5" fill="#6b7280" text-anchor="middle">ready</text>
  <rect x="20" y="108" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="112" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="133" font-size="10" fill="#374151" text-anchor="middle">task_chain · FF 64 项 · 1R</text>
  <rect x="20" y="162" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="166" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="187" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF 16 项 · 1R</text>
  <rect x="20" y="216" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="220" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="241" font-size="10" fill="#374151" text-anchor="middle">cfg_misc · FF · 1R</text>
  <rect x="889" y="20" width="202" height="114" fill="#f1f5f9" stroke="#334155"/>
  <rect x="889" y="20" width="202" height="18" fill="#334155"/>
  <text x="990" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">MATCH_CRE</text>
  <text x="990" y="60" font-size="10" fill="#334155" text-anchor="middle">is_new</text>
  <text x="990" y="82" font-size="10" fill="#334155" text-anchor="middle">user_id[15:0] · stream_id[3:0]</text>
  <text x="990" y="104" font-size="10" fill="#334155" text-anchor="middle">reissue</text>
  <text x="990" y="126" font-size="10" fill="#334155" text-anchor="middle">skip[63:0]</text>
  <rect x="889" y="146" width="202" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="893" y="150" width="194" height="34" fill="none" stroke="#374151"/>
  <text x="990" y="171" font-size="10" fill="#374151" text-anchor="middle">datain_hold · FF 1 项 · 1W</text>
  <rect x="252" y="20" width="593" height="238" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="270" y="36" font-size="8.5" fill="#6b7280">M1</text>
  <text x="831" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="270" y="56" font-size="12" fill="#111827">User_Match · 判用户、找搬入任务、定跳过</text>
  <text x="270" y="78" font-size="10.5" fill="#475569">1. bypass = WEIGHTS_MODE || SELF_START → datain_hold = {DATAIN_TASK_PC,</text>
  <text x="282" y="98" font-size="10.5" fill="#475569">stream_id = 15, task_id = 63, user_id, path_id}</text>
  <text x="270" y="118" font-size="10.5" fill="#475569">2. hit = ∃i: valid &amp;&amp; user_id_vld &amp;&amp; user_id 相同；done = hit ? done_bitmap[i] : 0</text>
  <text x="270" y="138" font-size="10.5" fill="#475569">3. t = 最低 {j | WAIT_WAKE[j] &amp;&amp; PATH_ID[j] == path_id &amp;&amp; !done[j]}；o = P2P_REISSUE_TID[t]</text>
  <text x="270" y="158" font-size="10.5" fill="#475569">4. !reissue &amp;&amp; TYPE[t] == 3 → skip = {t, o}，不派；!reissue &amp;&amp; TYPE[t] == 2 → skip = {o}；</text>
  <text x="282" y="178" font-size="10.5" fill="#475569">派 t 时 datain_hold = {TASK_PC[t], t, user_id, path_id, stream_id}</text>
  <text x="270" y="202" font-size="10" fill="#9ca3af">ready = (hit || 建表两条满足) &amp;&amp; (不派 || !datain_hold.valid)</text>
  <path d="M208 58 L251 58" stroke="#475569" marker-end="url(#tsm1)" fill="none"/>
  <path d="M208 129 L251 129" stroke="#475569" marker-end="url(#tsm1)" fill="none"/>
  <path d="M208 183 L251 183" stroke="#475569" marker-end="url(#tsm1)" fill="none"/>
  <path d="M208 237 L251 237" stroke="#475569" marker-end="url(#tsm1)" fill="none"/>
  <path d="M845 77 L888 77" stroke="#475569" marker-end="url(#tsm1)" fill="none"/>
  <path d="M845 167 L888 167" stroke="#475569" marker-end="url(#tsm1)" fill="none"/>
</svg>
```

### M2 · CREATE 建表或补跳过位

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1177 240" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="tsm2" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="1177" height="240" fill="#ffffff"/>
  <rect x="20" y="20" width="202" height="92" fill="#f1f5f9" stroke="#334155"/>
  <rect x="20" y="20" width="202" height="18" fill="#334155"/>
  <text x="121" y="33" font-size="10.5" fill="#ffffff" text-anchor="middle">MATCH_CRE</text>
  <text x="121" y="60" font-size="10" fill="#334155" text-anchor="middle">is_new</text>
  <text x="121" y="82" font-size="10" fill="#334155" text-anchor="middle">user_id[15:0] · stream_id[3:0]</text>
  <text x="121" y="104" font-size="10" fill="#334155" text-anchor="middle">reissue · skip[63:0]</text>
  <rect x="20" y="124" width="202" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="128" width="194" height="34" fill="none" stroke="#374151"/>
  <text x="121" y="149" font-size="10" fill="#374151" text-anchor="middle">task_chain · FF 64 项 · 1R</text>
  <rect x="20" y="178" width="202" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="182" width="194" height="34" fill="none" stroke="#374151"/>
  <text x="121" y="203" font-size="10" fill="#374151" text-anchor="middle">stream_ptr · FF · 1RW</text>
  <rect x="898" y="20" width="259" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="902" y="24" width="251" height="34" fill="none" stroke="#374151"/>
  <text x="1028" y="45" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF 16 项 · 1W（create 口）</text>
  <rect x="266" y="20" width="588" height="200" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="284" y="36" font-size="8.5" fill="#6b7280">M2</text>
  <text x="840" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="284" y="56" font-size="12" fill="#111827">Stream_table · create 口</text>
  <text x="284" y="78" font-size="10.5" fill="#475569">1. is_new → stream_table[tail_ptr] = {valid, user_id, user_id_vld = 1, reissue,</text>
  <text x="296" y="98" font-size="10.5" fill="#475569">task_id = 0, done_bitmap = skip, task_chain[0] 的属性}；tail_ptr += 1</text>
  <text x="284" y="118" font-size="10.5" fill="#475569">2. is_new → task_fsm = skip[0] ? FINISH : (WAIT_WAKE[0] || CREDIT_EN[0] ? WAIT : READY)</text>
  <text x="284" y="138" font-size="10.5" fill="#475569">3. !is_new → done_bitmap |= skip；skip[task_id] → task_fsm = FINISH；reissue → reissue = 1</text>
  <text x="284" y="158" font-size="10.5" fill="#475569">4. 写口被高优先级占住 → 保持本笔，下一拍再写</text>
  <text x="284" y="182" font-size="10" fill="#9ca3af">这一次不派 DTE 的新用户也照样建表</text>
  <path d="M222 66 L265 66" stroke="#475569" marker-end="url(#tsm2)" fill="none"/>
  <path d="M222 145 L265 145" stroke="#475569" marker-end="url(#tsm2)" fill="none"/>
  <path d="M222 199 L265 199" stroke="#475569" marker-end="url(#tsm2)" fill="none"/>
  <path d="M854 41 L897 41" stroke="#475569" marker-end="url(#tsm2)" fill="none"/>
</svg>
```

### M3 · WAKE credit 唤醒

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1143 178" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="tsm3" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="1143" height="178" fill="#ffffff"/>
  <rect x="20" y="20" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="24" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="45" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF 16 项 · 1R</text>
  <polygon points="30,74 208,74 198,132 20,132" fill="#f8fafc" stroke="#374151"/>
  <text x="114" y="93" font-size="10.5" fill="#374151" text-anchor="middle">router2ts_credit_ch</text>
  <text x="114" y="111" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · stream_id[3:0]</text>
  <text x="114" y="129" font-size="9.5" fill="#6b7280" text-anchor="middle">task_id[5:0] · path_id[7:0]</text>
  <rect x="835" y="20" width="288" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="839" y="24" width="280" height="34" fill="none" stroke="#374151"/>
  <text x="979" y="45" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF 16 项 · 1W（credit_wake 口）</text>
  <rect x="252" y="20" width="539" height="138" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="270" y="36" font-size="8.5" fill="#6b7280">M3</text>
  <text x="777" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="270" y="56" font-size="12" fill="#111827">credit · 把等 credit 的项置 READY</text>
  <text x="270" y="78" font-size="10.5" fill="#475569">1. 授予到 → stream_table[stream_id] = {task_fsm = READY, reissue = 1}</text>
  <text x="270" y="98" font-size="10.5" fill="#475569">2. 否则按年龄找 task_fsm == WAIT &amp;&amp; task_type == 4 &amp;&amp; !rmem_busy → task_fsm = READY</text>
  <text x="270" y="118" font-size="10.5" fill="#475569">3. 一拍一笔，走 credit_wake 口</text>
  <text x="270" y="142" font-size="10" fill="#9ca3af">搬入任务不经这一级：它的完成由 M6 直接置 FINISH</text>
  <path d="M208 41 L251 41" stroke="#475569" marker-end="url(#tsm3)" fill="none"/>
  <path d="M208 103 L251 103" stroke="#475569" marker-end="url(#tsm3)" fill="none"/>
  <path d="M791 41 L834 41" stroke="#475569" marker-end="url(#tsm3)" fill="none"/>
</svg>
```

### M4 · 选最老发射

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1119 244" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="tsm4" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="1119" height="244" fill="#ffffff"/>
  <rect x="20" y="20" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="24" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="45" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF 16 项 · 1R</text>
  <rect x="20" y="74" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="78" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="99" font-size="10" fill="#374151" text-anchor="middle">stream_ptr · FF · 1R</text>
  <rect x="20" y="128" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="132" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="153" font-size="10" fill="#374151" text-anchor="middle">datain_hold · FF 1 项 · 1R</text>
  <rect x="20" y="182" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="186" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="207" font-size="10" fill="#374151" text-anchor="middle">ts_route · FF 64 项 · 1R</text>
  <polygon points="910,20 1099,20 1089,114 900,114" fill="#f8fafc" stroke="#374151"/>
  <text x="1000" y="39" font-size="10.5" fill="#374151" text-anchor="middle">ts2&lt;u&gt;core_task_ch</text>
  <text x="1000" y="57" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · task_pc[31:0]</text>
  <text x="1000" y="75" font-size="9.5" fill="#6b7280" text-anchor="middle">stream_id · task_id · user_id</text>
  <text x="1000" y="93" font-size="9.5" fill="#6b7280" text-anchor="middle">path_id · task_dsa_en · vcid</text>
  <text x="1000" y="111" font-size="9.5" fill="#6b7280" text-anchor="middle">ready</text>
  <rect x="252" y="20" width="604" height="204" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="270" y="36" font-size="8.5" fill="#6b7280">M4</text>
  <text x="842" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="270" y="56" font-size="12" fill="#111827">三条 Arb · 各选最老的一个</text>
  <text x="270" y="78" font-size="10.5" fill="#475569">1. cand[u] = {i | valid &amp;&amp; task_fsm == READY &amp;&amp; task_unit == u}</text>
  <text x="270" y="98" font-size="10.5" fill="#475569">2. age(i) = (i − head_ptr) mod 16，取最小；DTE 另按 datain_hold 所在 stream 的年龄比，</text>
  <text x="282" y="118" font-size="10.5" fill="#475569">同一 stream 时 Generated 先，Bypass 那一格按最老</text>
  <text x="270" y="138" font-size="10.5" fill="#475569">3. cmd[u] = {task_pc, stream_id, task_id, user_id, task_path_id, dsa_en = (task_recv == 01)}</text>
  <text x="270" y="158" font-size="10.5" fill="#475569">4. DTE：vcid = ts_route[task_path_id].vcid，搬入那一格填 0</text>
  <text x="270" y="182" font-size="10" fill="#9ca3af">选中后非抢占保持，字段到 ACCEPT 前不变</text>
  <path d="M208 41 L251 41" stroke="#475569" marker-end="url(#tsm4)" fill="none"/>
  <path d="M208 95 L251 95" stroke="#475569" marker-end="url(#tsm4)" fill="none"/>
  <path d="M208 149 L251 149" stroke="#475569" marker-end="url(#tsm4)" fill="none"/>
  <path d="M208 203 L251 203" stroke="#475569" marker-end="url(#tsm4)" fill="none"/>
  <path d="M856 67 L899 67" stroke="#475569" marker-end="url(#tsm4)" fill="none"/>
</svg>
```

### M5 · ACCEPT 回写

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 992 174" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="tsm5" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="992" height="174" fill="#ffffff"/>
  <polygon points="30,20 188,20 178,60 20,60" fill="#f8fafc" stroke="#374151"/>
  <text x="104" y="39" font-size="10.5" fill="#374151" text-anchor="middle">ts2&lt;u&gt;core_task_ch</text>
  <text x="104" y="57" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · ready</text>
  <rect x="701" y="20" width="271" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="705" y="24" width="263" height="34" fill="none" stroke="#374151"/>
  <text x="836" y="45" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF 16 项 · 1W（issue 口 ×3）</text>
  <rect x="701" y="74" width="271" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="705" y="78" width="263" height="34" fill="none" stroke="#374151"/>
  <text x="836" y="99" font-size="10" fill="#374151" text-anchor="middle">datain_hold · FF 1 项 · 1W</text>
  <rect x="232" y="20" width="425" height="134" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="250" y="36" font-size="8.5" fill="#6b7280">M5</text>
  <text x="643" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="250" y="56" font-size="12" fill="#111827">三条 Arb · 按来源收尾</text>
  <text x="250" y="78" font-size="10.5" fill="#475569">1. accept = valid &amp;&amp; ready（raw ACCEPT）</text>
  <text x="250" y="98" font-size="10.5" fill="#475569">2. Generated → task_fsm = INFLY；task_type == 4 → rmem_busy = 1</text>
  <text x="250" y="118" font-size="10.5" fill="#475569">3. 搬入那一格 → datain_hold.valid = 0，不改 stream_table</text>
  <text x="250" y="138" font-size="10.5" fill="#475569">4. 回写没被收下时只重试这一笔字段写，不重新下发</text>
  <path d="M188 40 L231 40" stroke="#475569" marker-end="url(#tsm5)" fill="none"/>
  <path d="M657 41 L700 41" stroke="#475569" marker-end="url(#tsm5)" fill="none"/>
  <path d="M657 95 L700 95" stroke="#475569" marker-end="url(#tsm5)" fill="none"/>
</svg>
```

### M6 · 完成事件合流

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1206 274" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="tsm6" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="1206" height="274" fill="#ffffff"/>
  <polygon points="30,20 208,20 198,78 20,78" fill="#f8fafc" stroke="#374151"/>
  <text x="114" y="39" font-size="10.5" fill="#374151" text-anchor="middle">&lt;u&gt;core2ts_done_ch</text>
  <text x="114" y="57" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · stream_id · task_id</text>
  <text x="114" y="75" font-size="9.5" fill="#6b7280" text-anchor="middle">user_id[15:0] · pid[7:0]</text>
  <polygon points="30,90 208,90 198,148 20,148" fill="#f8fafc" stroke="#374151"/>
  <text x="114" y="109" font-size="10.5" fill="#374151" text-anchor="middle">&lt;u&gt;2ts_done_ch</text>
  <text x="114" y="127" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · stream_id[3:0]</text>
  <text x="114" y="145" font-size="9.5" fill="#6b7280" text-anchor="middle">task_id[5:0] · event</text>
  <polygon points="30,160 208,160 198,200 20,200" fill="#f8fafc" stroke="#374151"/>
  <text x="114" y="179" font-size="10.5" fill="#374151" text-anchor="middle">rmem2ts_done_ch</text>
  <text x="114" y="197" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · user_id · path_id</text>
  <rect x="20" y="212" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="216" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="237" font-size="10" fill="#374151" text-anchor="middle">ack_half · FF · 1RW</text>
  <rect x="904" y="20" width="282" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="908" y="24" width="274" height="34" fill="none" stroke="#374151"/>
  <text x="1045" y="45" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF 16 项 · 1W（completion 口）</text>
  <rect x="252" y="20" width="608" height="234" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="270" y="36" font-size="8.5" fill="#6b7280">M6</text>
  <text x="846" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="270" y="56" font-size="12" fill="#111827">完成合流 · 按 TASK_RECV_UNIT 与 TASK_TYPE 判</text>
  <text x="270" y="78" font-size="10.5" fill="#475569">1. WEIGHTS_MODE → 全部丢；SELF_START &amp;&amp; {stream_id, task_id} == {15, 63} → 丢</text>
  <text x="270" y="98" font-size="10.5" fill="#475569">2. TYPE[t] == 4 → 只认 Reduce Done：i = user_id 命中的 stream，t = task_id[i]，rmem_busy = 0</text>
  <text x="270" y="118" font-size="10.5" fill="#475569">3. RECV_UNIT[t] == 00 → RV core 的完成即完成；== 01 → ack_half 里两半都到才完成</text>
  <text x="270" y="138" font-size="10.5" fill="#475569">4. done_bitmap[t] = 1；t == task_id → FINISH；!user_id_vld → 写 user_id；TYPE[t] == 5 → 写 pid</text>
  <text x="270" y="162" font-size="10" fill="#9ca3af">七路都是脉冲，本级永远就绪，不向上游反压</text>
  <path d="M208 49 L251 49" stroke="#475569" marker-end="url(#tsm6)" fill="none"/>
  <path d="M208 119 L251 119" stroke="#475569" marker-end="url(#tsm6)" fill="none"/>
  <path d="M208 180 L251 180" stroke="#475569" marker-end="url(#tsm6)" fill="none"/>
  <path d="M208 233 L251 233" stroke="#475569" marker-end="url(#tsm6)" fill="none"/>
  <path d="M860 41 L903 41" stroke="#475569" marker-end="url(#tsm6)" fill="none"/>
</svg>
```

### M7 · INSTALL 生成后继

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1134 218" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="tsm7" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="1134" height="218" fill="#ffffff"/>
  <rect x="20" y="20" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="24" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="45" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF 16 项 · 1R</text>
  <rect x="20" y="74" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="78" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="99" font-size="10" fill="#374151" text-anchor="middle">task_masks · FF · 1R</text>
  <rect x="20" y="128" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="132" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="153" font-size="10" fill="#374151" text-anchor="middle">task_chain · FF 64 项 · 1R</text>
  <rect x="849" y="20" width="265" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="853" y="24" width="257" height="34" fill="none" stroke="#374151"/>
  <text x="982" y="45" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF 16 项 · 1W（install 口）</text>
  <rect x="252" y="20" width="553" height="178" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="270" y="36" font-size="8.5" fill="#6b7280">M7</text>
  <text x="791" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D1</text>
  <text x="270" y="56" font-size="12" fill="#111827">Task_ctrl · 一拍找后继再原子安装</text>
  <text x="270" y="78" font-size="10.5" fill="#475569">1. 候选：valid &amp;&amp; task_fsm == FINISH &amp;&amp; end == 0，按年龄选一个</text>
  <text x="270" y="98" font-size="10.5" fill="#475569">2. search = after(task_id) &amp; THROUGH_END_MASK &amp; ~done_bitmap；search == 0 → 不装</text>
  <text x="270" y="118" font-size="10.5" fill="#475569">3. n = 最低置位；path = (pid_pending &amp;&amp; n == task_id + 1) ? task_path_id : PATH_ID[n]</text>
  <text x="270" y="138" font-size="10.5" fill="#475569">4. 整项写 {task_id = n, task_chain[n] 的属性, task_path_id = path, pid_pending = 0,</text>
  <text x="282" y="158" font-size="10.5" fill="#475569">task_fsm = WAIT_WAKE[n] || CREDIT_EN[n] ? WAIT : READY}</text>
  <text x="270" y="182" font-size="10" fill="#9ca3af">搜索不回绕、不越过 End；End 以前都做完了由退休收尾</text>
  <path d="M208 41 L251 41" stroke="#475569" marker-end="url(#tsm7)" fill="none"/>
  <path d="M208 95 L251 95" stroke="#475569" marker-end="url(#tsm7)" fill="none"/>
  <path d="M208 149 L251 149" stroke="#475569" marker-end="url(#tsm7)" fill="none"/>
  <path d="M805 41 L848 41" stroke="#475569" marker-end="url(#tsm7)" fill="none"/>
</svg>
```

### M8 · RETIRE

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1166 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="tsm8" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="1166" height="198" fill="#ffffff"/>
  <rect x="20" y="20" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="24" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="45" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF 16 项 · 1R</text>
  <rect x="20" y="74" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="78" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="99" font-size="10" fill="#374151" text-anchor="middle">stream_ptr · FF · 1RW</text>
  <rect x="20" y="128" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="132" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="153" font-size="10" fill="#374151" text-anchor="middle">task_masks · FF · 1R</text>
  <polygon points="874,20 1146,20 1136,78 864,78" fill="#f8fafc" stroke="#374151"/>
  <text x="1005" y="39" font-size="10.5" fill="#374151" text-anchor="middle">ts2router_credit_release_ch</text>
  <text x="1005" y="57" font-size="9.5" fill="#6b7280" text-anchor="middle">valid · user_id[15:0]</text>
  <text x="1005" y="75" font-size="9.5" fill="#6b7280" text-anchor="middle">ready</text>
  <rect x="864" y="90" width="282" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="868" y="94" width="274" height="34" fill="none" stroke="#374151"/>
  <text x="1005" y="115" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF 16 项 · 1W（retirement 口）</text>
  <rect x="252" y="20" width="568" height="158" fill="#fbf3df" stroke="#b45309" rx="4" stroke-dasharray="5 3"/>
  <text x="270" y="36" font-size="8.5" fill="#6b7280">M8</text>
  <text x="806" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="270" y="56" font-size="12" fill="#7c2d12">retire · 先发退休请求再清 valid</text>
  <text x="270" y="78" font-size="10.5" fill="#92400e">1. 条件：i == head_ptr &amp;&amp; valid &amp;&amp; (done_bitmap &amp; THROUGH_END_MASK) == THROUGH_END_MASK</text>
  <text x="270" y="98" font-size="10.5" fill="#92400e">2. ts2router_credit_release_ch = {valid = 1, user_id}，保持到 ready</text>
  <text x="270" y="118" font-size="10.5" fill="#92400e">3. ready → stream_table[i].valid = 0；head_ptr += 1</text>
  <text x="270" y="138" font-size="10.5" fill="#92400e">4. SELF_START → 在 tail_ptr 补一项：Task 0 的属性，user_id_vld = 0（create 口）</text>
  <text x="270" y="162" font-size="10" fill="#9ca3af">只允许队头退休，head_ptr 才能单调推进</text>
  <path d="M208 41 L251 41" stroke="#475569" marker-end="url(#tsm8)" fill="none"/>
  <path d="M208 95 L251 95" stroke="#475569" marker-end="url(#tsm8)" fill="none"/>
  <path d="M208 149 L251 149" stroke="#475569" marker-end="url(#tsm8)" fill="none"/>
  <path d="M820 49 L863 49" stroke="#475569" marker-end="url(#tsm8)" fill="none"/>
  <path d="M820 111 L863 111" stroke="#475569" marker-end="url(#tsm8)" fill="none"/>
</svg>
```

### M9 · credit 申请

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 966 198" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs><marker id="tsm9" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker></defs>
  <rect x="0" y="0" width="966" height="198" fill="#ffffff"/>
  <rect x="20" y="20" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="24" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="45" font-size="10" fill="#374151" text-anchor="middle">stream_table · FF 16 项 · 1R</text>
  <rect x="20" y="74" width="188" height="42" fill="#ffffff" stroke="#374151"/>
  <rect x="24" y="78" width="180" height="34" fill="none" stroke="#374151"/>
  <text x="114" y="99" font-size="10" fill="#374151" text-anchor="middle">task_chain · FF 64 项 · 1R</text>
  <polygon points="757,20 946,20 936,96 747,96" fill="#f8fafc" stroke="#374151"/>
  <text x="846" y="39" font-size="10.5" fill="#374151" text-anchor="middle">ts2router_req</text>
  <text x="846" y="57" font-size="9.5" fill="#6b7280" text-anchor="middle">req_valid · user_id[15:0]</text>
  <text x="846" y="75" font-size="9.5" fill="#6b7280" text-anchor="middle">stream_id · task_id · path_id</text>
  <text x="846" y="93" font-size="9.5" fill="#6b7280" text-anchor="middle">req_ready</text>
  <rect x="252" y="20" width="451" height="158" fill="#fbf3df" stroke="#b45309" rx="4" stroke-dasharray="5 3"/>
  <text x="270" y="36" font-size="8.5" fill="#6b7280">M9</text>
  <text x="689" y="36" font-size="8.5" fill="#6b7280" text-anchor="end">D变长</text>
  <text x="270" y="56" font-size="12" fill="#7c2d12">credit · 向 Router 要资源</text>
  <text x="270" y="78" font-size="10.5" fill="#92400e">1. 按年龄找 task_fsm == WAIT &amp;&amp; CREDIT_EN[task_id] &amp;&amp; task_type != 4</text>
  <text x="270" y="98" font-size="10.5" fill="#92400e">2. ts2router_req = {user_id, stream_id, task_id, path_id}</text>
  <text x="270" y="118" font-size="10.5" fill="#92400e">3. req_ready == 0 → 本笔保持，不发下一笔</text>
  <text x="270" y="138" font-size="10.5" fill="#92400e">4. 授予从 router2ts_credit_ch 回来，由 M3 置 READY</text>
  <text x="270" y="162" font-size="10" fill="#9ca3af">逐级 reduce 的 credit 由 M3 按 rmem_busy 判，不走这一路</text>
  <path d="M208 41 L251 41" stroke="#475569" marker-end="url(#tsm9)" fill="none"/>
  <path d="M208 95 L251 95" stroke="#475569" marker-end="url(#tsm9)" fill="none"/>
  <path d="M703 58 L746 58" stroke="#475569" marker-end="url(#tsm9)" fill="none"/>
</svg>
```

***

## 7　参数汇总

```
task_chain          64 项，每项 PC 与 ATTR 两个 32 位字
datain_task         1 项
ROUTER_TABLE        64 项，按 PID 索引 {TASK_DIR[3:0], TASK_VCID[1:0]}
path_flowctl        64 项，按 PID 索引 {flowctl_en, window_n}
DataIn_task_table   1 项
stream_table        16 项，建表与退休各一拍一项；stream_num 软件配 1～16，超过 16 写入 16；自启动数等于 stream_num
Bypass 的保留身份    stream_id = 15、task_id = 63
八个写口优先级       retirement > completion > install > issue × 3 > credit_wake > create
done_bitmap         64 位，对应 64 项
DTE / MU / VU Arb   发射宽度各 1
task_fsm            五态 IDLE / WAIT / READY / INFLY / FINISH
Rmem credit         每个用户一份
P2P 阻塞缓冲         方向数 ≤ 3，容量与项数软件配
超前发送窗口 N       编译期确定，软件按 path 配
task 唤醒延迟        2～3 cycle（MAS 的硬件目标值）
逐级拍数            M1～M7 各 1 拍（LLD：当拍组合，下一上升沿写入）；M8、M9 等 Router，变长
调度间隔            16 T；需与 Router 通信时 21 T；retire 5 T（Top 模拟器的端到端建模值）
```

***

## 8　机制覆盖

“用例”一栏是 `test/bach/ip/chip/core/ts/` 下的 GoogleTest 用例名，`BachTsCfg`、`BachTsIssue`、`BachStreamTable`、`BachTaskCtrl`、`BachTsDone`、`BachTsCredit`、`BachTs` 七组，分别在 `ts_config.cpp`、`ts_issue.cpp`、`ts_chain.cpp`（后两组）、`ts_done.cpp`、`ts_credit.cpp`、`ts.cpp`；B core 与 R core 的两条在 `moe_chip`。

| 机制 | 功能 | 用例 |
| - | - | - |
| 写 ATTR 自动置 `TASK_VALID`，位域照写照读 | F1、F2、F3 | `BachTsCfg.WriteSetsValidAutomatically`、`BachTsCfg.AttrFieldsAreStoredAsWritten` |
| `STREAM_NUM` 超过 16 写入 16 | F5 | `BachTsCfg.StreamNumAbove16IsWrittenAs16` |
| `ROUTER_TABLE` 按 PID 存方向与 VCID | F6 | `BachTsCfg.RouterTableHoldsDirAndVcid` |
| 写 `TS_INIT_FINISH` 后的配置检查，权重加载不查 | F9 | `BachTsCfg.ChainShapeIsChecked`、`BachTsCfg.TaskTypeRulesAreChecked`、`BachTsCfg.SelfStartNeedsBothChainAndDatain`、`BachTsCfg.WeightsModeSkipsTheCheck`、`BachTs.RejectsTwoEndTasks`、`BachTs.RejectsHoleInChain` |
| 两张掩码是 `task_chain` 的纯函数 | F10 | `BachTsCfg.MasksArePureFunctionsOfTheChain` |
| 按 PID 找这个用户没做完的最低一项搬入任务 | F11 | `BachTsCfg.MatchDatainPicksTheLowestUndoneTaskOfThatPid` |
| trigger 不设队列，表满时反压，不丢 | F12、F17 | `BachTs.BackpressuresTriggerWhenTableIsFull` |
| 老用户按 PID 找到它没做完的搬入任务，跳过位补进原来那一项 | F13、F16 | `BachTsIssue.ExistingUserMatchesByPid` |
| Broadcast 重发的搬入且不重发：搬入照做，配对的搬出跳过 | F14、F15、F61 | `BachTsIssue.BroadcastReissueInSkipsOnlyTheEgress` |
| P2P 重发的搬入且不重发：两项都跳过，不派 DTE，照样建表 | F14、F18、F62 | `BachTsIssue.P2pReissueInSkipsBothWithoutDte` |
| 跳过位含当前任务时当前任务算做完 | F15 | `BachStreamTable.SkipMaskFinishesOnlyTheCurrentTask` |
| 自启动 core 上进来的包走 Bypass，不建表，身份用 SID 15 / TID 63 | F19、F23 | `BachTsIssue.SelfStartCoreDatainDoesNotCreateAStream` |
| 八个写口的优先级，不同 stream 同拍都写 | F32 | `BachTs.WritePortPriorityOnSameStream`、`BachStreamTable.SameStreamGoesByPriority`、`BachStreamTable.DifferentStreamsAreWrittenTogether` |
| `completion` 口无条件置位，只在是当前任务时改状态 | F34 | `BachStreamTable.CompletionSetsDoneBitButGuardsTheFsm` |
| 后继是 End 以内没做完的最低一项，一拍跳完 | F37 | `BachTaskCtrl.SkipsDoneTasksInOneStep`、`BachTs.WalksTheWholeChain` |
| 第 0 项到 End 都做完时不生成后继 | F37 | `BachTaskCtrl.NoSuccessorWhenEverythingThroughEndIsDone` |
| 后继初态由 `WAIT_WAKE` 与 `TASK_CREDIT_EN` 定 | F38 | `BachTaskCtrl.InitialStateFollowsWaitWakeAndCredit` |
| 新 PID 只由紧邻的后继继承 | F39 | `BachTaskCtrl.NewPidGoesOnlyToTheAdjacentTask` |
| 后继整项原子写入 | F40 | `BachTaskCtrl.InstallWritesTheWholeEntryAtOnce` |
| 全部按年龄选最老，搬入那一格也按年龄比 | F43、F47 | `BachTsIssue.OldestStreamGoesFirst`、`BachTsIssue.DataInGoesByAgeAgainstGenerated` |
| 非抢占保持到 raw ACCEPT | F44 | `BachTsIssue.HeldCommandStaysUntilAccept` |
| DTE 命令带 `ROUTER_TABLE` 的 VCID，`task_dsa_en` 看 `TASK_RECV_UNIT` | F45 | `BachTsIssue.DteCmdCarriesVcidAndRecvUnit` |
| 三条发射通路同一拍并行下发 | F48 | `BachTsIssue.ThreeUnitsIssueInTheSameCycle` |
| 00 只认 RV core 的完成，01 要同一执行单元的两路 | F51 | `BachTsDone.RvOnlyTakesTheRvAck`、`BachTsDone.BothAcksAreNeededWhenRecvIsDsa`、`BachTsDone.AcksOfDifferentUnitsDoNotPair` |
| 逐级 reduce 只认 Reduce Done，按 `user_id` 找 stream | F52 | `BachTsDone.ReduceNeedsRouterDoneToFinish`、`BachTsDone.RouterDoneFindsTheStreamByUserId` |
| 提前完成的搬入只点亮一位，主线走到时跳过 | F53、F37 | `BachTsDone.DataInOnlyLightsTheBit`、`BachTs.SkipsCompletedDatainTask` |
| 权重加载期间与 Bypass 那一路的完成丢掉 | F54 | `BachTsDone.WeightsModeDropsEveryAck`、`BachTsDone.BypassAckIsDroppedOnSelfStartCore` |
| 自启动表项由 Task 0 的 RV core 完成补 `user_id` | F55 | `BachTsDone.Task0CoreAckFillsTheUserOnSelfStartCore` |
| PID 更新任务的新 PID 由 RV core 完成带回 | F56 | `BachTsDone.PidUpdateTakesThePidFromTheCoreAck` |
| credit 申请带四个号，授予后置 READY 并置 `reissue` | F58、F35 | `BachTsCredit.RequestCarriesTheFourIds`、`BachTsCredit.GrantWakesTheTaskAndSetsReissue` |
| 逐级 reduce 用本级 Rmem credit，一项做完才发下一项 | F30、F46、F52、F59 | `BachTsCredit.ReduceTakesTheLocalRmemCredit`、`BachTs.ReduceIssuesOneTaskAtATime` |
| Head-only 退休，第 0 项到 End 全做完才退，先还 credit 再清 valid | F68、F69 | `BachTsCredit.OnlyTheHeadEntryRetires`、`BachTsCredit.ClearsOnlyAfterRouterAccepts`、`BachTsCredit.NextEntryRetiresAfterTheHead`、`BachTsCredit.RetiresOnceEveryTaskThroughEndIsDone`、`BachTs.OneUserRunsFromTriggerToRetire` |
| 自启动建满 `stream_num` 项，退休后补一项 | F70、F72 | `BachStreamTable.SelfStartFillsEveryEntry`、`BachMoeChip.BcoreStartsTheBroadcast`、`BachMoeChip.TwoEpGroupsMeetAtTheReductionCore` |
| 超前发送窗口按 path 配 | F77 | `BachTsCfg.FlowControlWindowIsPerPath` |
| 重发未成功前数据不覆盖；P2P 阻塞缓冲；B core 按方向查 credit；广播 CreditCounter；异常上报；窗口判断 | F63～F67、F71、F78 | 未建 |

***

## 9　取舍

* **为什么搬入任务按 PID 找，而不设 path 到任务的反查表**
  * 一个用户的链上同一条 path 可以对多项，按“这个用户还没做完的最低一项”取，靠的就是它自己的 `done_bitmap`，不必另存状态；反查表一个 path 只能指一项
  * 代价是配置检查查不出“PID 在链上找不到”，只能运行时发现
* **为什么要不要重发在 trigger 那一刻就换成跳过位**
  * 要不要重发只有 Router 在 trigger 那一刻知道。把它换成 `done_bitmap` 上的几位，Task_ctrl 找后继时就只看一张位图，不必再为每种重发类型各设一张掩码
* **为什么逐级 reduce 的完成只认 Router**
  * DTE 把数据搬到 Router 就结束了自己的活，但 reduce 是在 Router 的 Rmem 里做的，此时结果还没算出来
  * 若让本地的完成直接结束这一项，后继任务会读到还没归约完的数据
* **为什么同一个用户的逐级 reduce 一项做完才发下一项**
  * Rmem 给每个用户一个 32 KiB 分区，同一时刻只装得下这个用户的一笔 reduce；上一笔的结果没有全部交付之前，下一笔进来会覆盖它的上下文
  * 所以本级 credit 每个用户只有一份，下发占掉、Rmem 做完才还。代价是几项之间多出一段 Rmem 往返
* **为什么提前完成的搬入不推进 task_id**
  * 推进 `task_id` 意味着承认任务链走到了那一项，而搬入的到达顺序与链序无关
  * 只落 `done_bitmap` 的一位，就把“这一项的数据齐了”与“任务链走到了这一项”分开记，两者在 Task_ctrl 生成后继时才合到一起判断
* **为什么退休只允许 head**
  * `stream_table` 是顺序 FIFO，credit 也按 stream 记账
  * 只让队头退休，`head_ptr` 才能单调推进，年龄比较才有确定的基准。代价是队头卡住时后面已经完成的 stream 也得等着
* **为什么先还 credit 再清 valid**
  * 反过来做，槽位已经放开而上游还没拿到 credit，中间这段时间上游发来的数据在本 core 没有落脚点
* **为什么用逐跳 credit 而不是端到端**
  * 端到端 credit 要经过整条路径的往返延迟才能释放，路径长到一定程度，16 个 stream 的执行耗时盖不住这个延迟，就会出空泡
  * 逐跳把一段长往返切成每跳各自的短往返，稳态吞吐与跳数无关，代价是要处理中途的阻塞
* **为什么 B core 与 R core 要自启动加软件映射表**
  * 收发绑在一条链上时，能缓的用户数被 `stream_table` 卡在 16，而它们的用户数远超 16
  * 拆开的是“在 TS 里占一个 stream 项”与“数据在本核停留”这两段时间：在途用户上限换成 Matrix Mem 的容量，推进顺序换成软件表说了算
* **为什么要超前发送窗口**
  * 一条“广播 → P2P → 广播”的路径上，前半段广播跑得比后半段快时，后半段 core 的 Core Mem 收不下，数据在物理通路上排队，后半段 core 出现计算空泡
  * 让发起方按本 core 的计算进度推迟发起 P2P 传输，排队就留在发起方的 Core Mem 里
* **为什么做成硬化调度器**
  * 各场景的 core 内任务流固定、任务类型不多，硬化足够覆盖
  * 代价是任务流的形状被写死在 64 项的表里，换场景要重配；换来的是调度延时压到 2～3 拍
