# Tracetto

起一个本地服务，把 latch 的 `.trace` 里每个 core 的 user / task 分段看成一页波形。

自己一套：自己的波形解码、自己的索引、自己的协议、自己的前端（`web/`），不依赖仓库里别的
Python、也不用第三方包。左边树是 `chip → core → 十一行`，右边是波形；一笔画成一段，
段上写 `User_id 77 CORE-MU 5 12拍` —— 77 是 user_id、`CORE-MU` 是单元（前缀标明哪一行：
`TS-` / `CORE-` / `DSA-`）、5 是 task 号、12 是持续多少拍。
行名只在左边树上写一遍，画布里不重复，所以波形是从画布最左边开始铺的。

## 用法

```shell
python3 src/utils/tracetto/tracetto.py                 # 从当前目录往下找 .trace，一份就直接用
python3 src/utils/tracetto/tracetto.py moe_lpu         # 名字里带这一截的
python3 src/utils/tracetto/tracetto.py moe_lpu --port 8899
python3 src/utils/tracetto/tracetto.py moe_lpu --reindex      # 不管现成的索引，重建一次
python3 src/utils/tracetto/tracetto.py moe_lpu --index-only   # 只建索引，不起服务
python3 src/utils/tracetto/tracetto.py moe_lpu --dump         # 把索引的 manifest 打成 JSON
```

也可以 `python3 -m src.utils.tracetto moe_lpu`（在仓库根下）。默认监听 `127.0.0.1:8766`，占了就往上
找；`--host` / `--port` / `--workers`（建索引用几个进程）可以改。

## 十一行与十三条通道

core 底下列的不是 `cmcm_q` 那些信号，而是十一条派生行：

| 行 | 画什么 |
| - | - |
| `TS-DTE-DATAIN` | **Router 触发**的那一步（搬入任务）从**被下发**到**这一路的 RV core 接下它**（`rv_start`）—— 这类任务在 Router 的数据到达时就直接下发，没有（或晚于下发才有）「装进 stream」那一拍，所以起点只能取下发那一刻 |
| `TS-DTE` | 主线那一步从 **TaskCtrl 把它装进 stream** 到**这一路的 RV core 接下它** |
| `TS-MU` / `TS-VU` | 同上；MU / VU 没有 Router 触发那一路，各只有一行 |
| `X-Core` | 这一路 RV core 执行一笔 task 的那几拍（`rv_start`→`rv_done`）。每个用户各一段，时间上可以重叠 |
| `VU-DSA-ISQ` | 一笔任务从**被单元收下**到**真正发行进执行通路**的那一段 —— 收下了为什么还不算，看这一行。VU 的收下是 `config_register` 收到那条宏指令 |
| `X-DSA` | 这一路 DSA 真正发行之后手上一直有活的那几拍，到单元把完成报回去为止。每个用户各一段，时间上可以重叠。DTE 没有这一行（它的访存拆到 Perfetto 的 DTE 十一条轨道里看，见下） |
| `MU-DSA-CALC` | 只有 MU 有。这笔任务**在矩阵执行单元里进出所跨的那一段**（第一个 tile 进 → 最后一个 tile 的后一拍），量的是真实 MAC 时间。**它不是切分，是嵌在 `MU-DSA` 里面的子区间** |

TS 那一族量的都是「这一步在 TS 里等了多久」，终点都是这一路的 `rv_start`，所以每一行都跟
下面 `X-Core` 那一行首尾相接、不重叠。DTE 拆成两行是因为它既有主线又有 Router 触发的搬入：
`TS-DTE-DATAIN` 摆在 `TS-DTE` 上面。MU / VU 的仲裁器没有 DataIn 通路，所以不拆。

只有 VU 的 DSA 那一行拆成了两段（MU 的 `MU-DSA-ISQ` 行已删 —— MU 不再拆，`MU-DSA` 直接从
「真正发行」起算；DTE 的 `DTE-DSA` 行也已删）：

- `VU-DSA-ISQ` = 「`config_register` 收下 trigger → 宏指令进执行流水」，
  `VU-DSA` = 「真正发行 → EVENT_EN 的宏指令退休报 `dsa_done`」。

两段首尾相接，起点那一拍是同一个。

`MU-DSA-CALC` 与上面所有行**形态不同**：那几行都是把一段切成首尾相接的两半，而 CALC 是**嵌在
`MU-DSA` 里面的子区间** —— 它整段落在 `MU-DSA` 里面（起点比它晚、终点比它早），画面上两者重叠。
它回答的是「`MU-DSA` 那一段里，真正在执行单元里算的是哪几拍」：实测 `chip0.core1` 一笔 75 拍的
`MU-DSA` 里，CALC 只占中间一段，前面在发读请求等数据、后面在收尾。

索引里一条 core 是**十三条通道**（`TS·DTE`/`TS·MU`/`TS·VU`/`DTE_DATAIN` 四条，加每单元各一对
Core/DSA，加 MU 与 VU 各一条 DSA-ISQ、加 MU 一条 DSA-CALC），十一行只是呈现层：TS 那四行各取一条
通道，其余各取一条。**通道号与显示行序不是一回事** —— 新通道一律追加在末尾，行序可以另排。
删行只删 ROWS 里的那一项、**通道留着不动**（免得后面的通道号跟着变）：`DTE_DSA`（第 6 条）与
`MU_DSA_ISQ`（第 11 条）两条通道仍照算，只是不再被任何一行引用。显示顺序是 `TS-DTE-DATAIN` /
`TS-DTE` / `TS-MU` / `TS-VU` / `DTE-Core` / `MU-Core` / `MU-DSA` / `MU-DSA-CALC` / `VU-Core` /
`VU-DSA-ISQ` / `VU-DSA`。

**颜色一共十一种**（空出 `DSA-DTE` 与 `DSA-ISQ-MU` 两个槽），只按“行”与“单元”分，与 user
无关：同一个颜色下可以有很多个 user，谁是谁看段上的 `User_id` 字。用户上千个，按 user 上色
必然撞色。

只有 Router 在用的 core（坏 core 与 TS 没配过任务的好 core，没有 `ts_inflight`）没有数据，树上标“只有 Router”，点不开。

## 索引

一份波形一份索引，落在波形旁边：`<波形名>.tracetto-index/`（`.gitignore` 里已经忽略）。

```
moe_lpu.tracetto-index/
  manifest.json          格式版本 + 波形身份 + 树 + 每条行的段数
  cores/<core_idx>.bin   一个 core 的十三条通道放一个文件
```

- **什么时候建**：第一次起服务自动建（多进程并行，打进度）；之后波形没变就直接复用
  （重载也不再重算），波形变了自动重建。建的时候先写进 `.building.<pid>.<xx>/`，全部写完
  **再确认一次波形没被改过**、最后写 manifest（它就是完成标记），然后整体 rename 过去 ——
  半成品永远不会被读到，两个进程同时建也只有一个赢。
- **怎么判“波形变了”**：realpath + dev/ino + 大小 + mtime_ns + 头尾各 64 KB 的 sha1。
  只看大小和时间挡不住“同大小同 mtime 被覆盖”，多读这 128 KB 换一个几乎不漏的判据；
  索引文件被人截断或删掉也会当场发现并重建。
  **波形没变但行的结构变了**（通道数、行数）靠 manifest 里的格式号挡：格式号对不上
  就重来一次。改 `LANES` / `ROWS` 时记得把 `index.INDEX_FORMAT` 加一 —— 只加通道不
  加号的话，旧索引在波形身份上完全匹配，会被静默复用。
- **里面是什么**：段记录 = `varint(空档) varint(持续) 3 字节标签`，标签内联
  （`u16 user + u8 task`，`(0xFFFF, 0xFF)` 表示“认不出是哪一笔”）。单元名由行号推出来、
  拍数就是“持续”，都不用存 —— 于是建索引没有全局状态，**并行到什么程度结果都一样**
  （自检里拿 1 进程与 4 进程的 sha1 对拍）。每 128 段一条 blk 表当二分定位用；段数超过
  4096 的行再写一层定长稠密的 L1 粗层，缩得很远时一次切一片、不解任何 varint。

## 协议

自己的四个端点（跟 insight 没关系）：

| 端点 | 内容 |
| - | - |
| `GET /api/init` | 树 + 每条行的段数 + build 号。**与波形多大无关的十几 KB**，段与标签一概不带 |
| `GET /api/window?lanes=0-8,27&t0=&t1=&px=` | 小 JSON 头 + 原始字节切片（切的就是索引里的段流，客户端自己解 varint）。服务端不做逐段循环：两次二分 + 一次 pread。行号是“core 序号 × `lanes_per_core` + 通道号”，与显示成几行无关 |
| `GET /api/status` | 建索引/重载的进度 |
| `POST /api/reload` | 只重新校验波形身份；没变直接回，变了才重建 |

窗口有两种模式，服务端按“这一窗有多少段 vs 一屏多少像素”自己选：段不多时给精确段，
段多得多的时给 L1 粗层的格子（一格一个 `u8 密度 + 3 字节标签`）。**粗层的传输上限是
“每行 ≤ 4096 格”，与波形多长无关**。

## 取值口径

- **五种事件**（下发、RV 起、RV 完、DSA 起、DSA 完）都是同一个形状：某条位掩码信号
  的第 u 位从 0 跳到 1 的那一拍，身份取同拍的打包信号
  `task = (X_task >> 8u) & 0xFF`、`user = (X_user >> 16u) & 0xFFFF`，
  本拍没有事件的那一路填占位 `0xFF` / `0xFFFF`。
- **段**：同一 user、同一 task 的起手对上下一次完成，区间 `[t0, t1)`，**拍数 = t1 - t0**。
  一个核上几个用户叠在一起时各画一段，不并成最早那笔的长段。同一拍上完成与起手撞在
  一起时先算完成。段与段之间没有东西就是空档（画面上不画），**不是**“上一个值保持”。
- **段的标签**：直接来自起手那条边沿（不是贴下发事件）。身份是占位就写 `-1`，画面上是 `?`。
  DTE 那一行有个缺口：Router 入站那一路按约定不算一笔 DTE task，它有完成没有起手，
  只记那一拍。
- **TS 那几行的配对**：起点那条流是 `ts_create`（第一个 task 走建表）与 `ts_install`
  （装后继）两条**单调计数器**合起来的 —— 它是**整核一份、不分路**的，所以要先按身份跟
  **本单元的下发**（`ts_unit` 这一位）一一对上。对不上的是别的单元的活，丢掉（实测 MU
  上整核装进 82231 笔、本单元只下发 25224 笔，不筛的话多出来的会每笔从装进来那拍画到
  波形末）。对上之后再与 `rv_start` 配，段 = `[装进 stream 那一刻, 这一路 RV core 接下
  那一刻)`。装进来了、下发了、这一路还没接下的延到波形末；`rv_start` 起了却没有装进来
  那一拍的只记一拍。
- **DTE 那一路的起点按「装进 stream 早不早于下发」分成两行**。搬入任务（chain 里的
  `wait_wake` 项）是 Router 的数据一到就下发，与主线走到哪无关，所以它在**下发那一刻**
  认领不到自己那一笔 —— 这些归 `TS-DTE-DATAIN`，起点退回**下发**那一刻；认领得到的
  归 `TS-DTE`。两个队列都**按位置对齐**（同一身份的第 i 次下发对第 i 笔 load / 第 i 个
  `rv_start`），不是“取最早那个还没用掉的” —— 后者会漂：一笔晚到的 load 若谁都不要，
  会一直挂在队首、被很远以后的同身份下发认走，画出一根几千拍的长条。
  MU / VU 的仲裁器（`mu_vu_arb.h`）**没有 DataIn 通路** —— 它的候选只从 stream_table
  的快照取（`if (!e.valid || e.task_fsm != kReady || e.task_unit != unit) continue;`），
  DataIn 那条路严格只喂 DTE。所以它们不拆行；那两行里「没装进来那一拍」的单拍段
  **不是 DataIn，而是 `ts_install` 的身份流有洞**（见下一条）。
- **`ts_install` 的笔数与身份必须一起定格**。`Installed()` 数的是「这一拍表收下了一笔
  装后继」，而 `TaskCtrl::Step()` 里收下之后**同一拍**又 `Generate()` 出一个新候选，
  `next_task` / `next_user` 当场就被换成下一笔了。早先 `EmitStep()` 拿 `NextTask()` 配
  `Installed()`，于是**计数涨的那一拍报出来的是下一笔的身份**，被收下那一笔的身份
  从头到尾没进过波形。后果：少数任务被误判成「没装进 stream」—— 实测 `moe_lpu_tokens`
  上 VU 62 笔、MU 1510 笔（MU / VU 全是这个洞，不存在别的通路），DTE 上则与真正的
  DataIn 叠在一起。现在身份改取 `AcceptedTask()`（在 `install->Accepted()` 那一支里先
  截下来），两边就对齐了。
- **`ts_unit` 数下发要逐拍展开**。一条命令在端口上被持有期间不会被重新驱动
  （`mu_vu_arb.h` 的 `Select()` 里 `if (held) return;`），所以那一位抬起来就是这一拍
  新驱动了一条命令；同一路连着几拍都下发时位一直是 1、波形上只有一段，**事件却是一拍
  一笔**（`moe_lpu_tokens` 上是 91016 笔，按 0→1 边沿数只有 90465 笔）。本工具按 0→1
  认 —— 与 `rv_start` / `dsa_start` / `dsa_done` 那些“计数器变了才抬一位”、一拍一个的
  脉冲同一个口径；要精确笔数就按非零段逐拍展开。
- **重叠**：同一条通道里几笔可以叠在一起。段间隔用 zigzag 存，下一段的起点可以早于
  上一段的末拍。`moe_lpu_tokens` 里 chip0 core9 的用户 23～27 就是这样：VU-DSA 五段
  互相搭着，不再画成用户 23 的一条 365 拍。
- **波形的末尾**：只按要读的那几个信号算（`moe_lpu` 是 35672）。**不能用全信号的
  max**，别的模块会把它顶上去（实测全信号是 36194）。

## 波形要带的信号

工具读下面这些信号，另拿 `ts_inflight` 认一个 core 是不是只有 Router 在用：

| 组 | 信号 |
| - | - |
| TS 下发 | `ts_unit` / `ts_task` / `ts_user` |
| 装进 stream | `ts_create` / `ts_create_task` / `ts_create_user`（建表）、`ts_install` / `ts_install_task` / `ts_install_user`（装后继）—— 两条都是单调计数器，身份是标量 |
| RV core | `rv_start` / `rv_start_task` / `rv_start_user`、`rv_done` / `rv_done_task` / `rv_done_user` |
| DSA | `dsa_start` / `dsa_start_task` / `dsa_start_user`、`dsa_done` / `dsa_done_task` / `dsa_done_user` |
| DTE 那个不报的完成 | `dsa_done_noack` / `dsa_done_noack_task` / `dsa_done_noack_user` —— 只有 DTE 抬 bit0 |
| 一笔任务在单元里的两端 | `dsa_task_trigger` / `dsa_task_trigger_task` / `dsa_task_trigger_user`（被收下）、`dsa_task_dispatch` / `dsa_task_dispatch_task` / `dsa_task_dispatch_user`（真正发行）|
| MU 那段计算的两端 | `dsa_calc_start` / `dsa_calc_start_task` / `dsa_calc_start_user`（第一个 tile 进矩阵执行单元）、`dsa_calc_done` / `dsa_calc_done_task` / `dsa_calc_done_user`（最后一个 tile 的后一拍）；另带 `dsa_calc_start_expert` / `dsa_calc_start_k` / `dsa_calc_start_n` / `dsa_calc_start_kblock` / `dsa_calc_start_nblock` / `dsa_calc_start_prim_k` / `dsa_calc_start_prim_n` / `dsa_calc_start_adtype` / `dsa_calc_start_bdtype` / `dsa_calc_start_outbf16` 十条标量（GEMM 规模：专家数、全尺寸 K/N、K/N 分块、原语 K/N、token/weight dtype 码、输出是否 BF16，M 恒为 1）|
| MU 发行那拍的配置寄存器数 | `dsa_dispatch_cfg_count`（标量，只有 MU 有）—— 一笔任务配置过多少个配置寄存器（写 `0x004`~`0x028` 每写一次记一笔，`TASK_TRIGGER` 是启动位不算、静态配置区 `0x400+` 也不计）。`trace2perfetto.py` 把它写进 `MU-DSA` 段的 args（`cfg_count`），tracetto 的索引与前端不存这一条 |
| DTE 十一条轨道（只有 `trace2perfetto.py` 读） | 挂在 `chip<i>.core<j>.dte.<regfile|lane<k>>` 下：`dsa_cfg_start` / `dsa_trigger`（各自带 `_task` / `_user`）量「第一次写 Config → 写 CFG_TRIGGER」；每条 lane 的 `rd_start`→`rd_done`、`wr_start`→`wr_done`（各带 `_task` / `_user`）量读/写两半；起点那一拍另带字节画像 `rd_start_data` / `rd_start_scale` / `rd_start_topk`（写侧 `wr_start_*`）与落库标志 `rd_start_cmem` / `wr_start_cmem`（1 = Core Mem，0 = Matrix Mem；scale 随 data 落同一块、topK 恒走 topk_table）|

边沿的起点是各家“过门槛”那一拍（DTE 过 Commit 准入、MU 进 issue_q、VU 被 ISQ 收下；
RV core 是执行器接下队头那笔），终点是各家把完成报回去那一拍（VU 每条宏指令退休报一次）。
边沿自带身份，段的标签直接取自边沿。

**DTE 那一路的终点有两个**，因为它的完成分两种去向：`ack_ts_en` 置了的报回 TS（`dsa_done`），
没置的（自启动 core 的链一 Bypass DataIn、身份是保留号 63，以及别的不上报的搬入）在
`CompletionRs::Join()` 里就被判为不上报，`Report()` 那一步压根不会碰它。**不报不等于没完成**，
所以另发一条 `dsa_done_noack`，判据挂在 `Report()` 的 `if (!p.notify)` 那一支上 ——
`Report()` 一拍只处理一笔，所以这里抬计数天然是「一拍一笔」，波形上不会互相盖掉。
（试过把判据挪到 `Join()`：那里一拍按通道扫、能收好几笔，一笔会盖掉另一笔；加个队列
补上「一拍一发」也不行，背压一动就把段拉长了。）

两者**互斥** —— 一个完成只走一支 —— 所以 `DTE_DSA` 通道（那条行已删，但通道仍照算）的终点
就是把两条流**按时间并起来**，不用去重。不并的话那批只能延到波形末（`moe_lpu_tokens` 上实测：
链一那 928 段原来最长 17335 拍、并上之后 192 拍）。

「装进 stream」那六条不是边沿：它们是**只加不清零的计数器**，判据是“这一拍的值比上一拍
大”，身份在涨的那一拍单独取（标量，不是按位打包）。`TS-MU` / `TS-VU` 的起点从这里取。

最后那一对（`dsa_task_trigger` / `dsa_task_dispatch`）量的是**一笔任务在单元里的两端**，
MU 抬 bit1、VU 抬 bit2，DTE 恒 0：

- `dsa_task_trigger` = 单元把这一笔收进自己的配置入口那一拍。MU 是写 `TASK_TRIGGER` 被
  `regfile` 锁成一笔，VU 是被 `config_register` 收下那条宏指令（比 ISQ 收下还早）。
- `dsa_task_dispatch` = 它真正发行进执行通路那一拍。MU 是这一笔的第一个 tile 开始发起
  访存，VU 是宏指令进执行流水。

`VU-DSA-ISQ` 取这两拍，`VU-DSA` 从后一拍起算；`MU-DSA` 也从后一拍起算，只是 `MU-DSA-ISQ`
那一行已删，收下到发行那一截不再单独成行。

最后那一对（`dsa_calc_start` / `dsa_calc_done`）**只有 MU 抬 bit1**，量的是这笔任务的
真实 MAC 时间（`kblock × 专家数 × nblock` 拍），不把 10 级流水深度算进去：

- `dsa_calc_start` = 它第一个 **tile** 进 exe 那一拍（`computed == 0`）。
- `dsa_calc_done` = 它最后一个 tile 的**后一拍**。最后一个 tile 那一拍只记身份、置 pending，
  下一个 `Ctrl::Step()` 开头才补计数，好让段长按 tile 数闭合（而不是少一拍）。

两个点都按 tile 数走，与流水线深度那个常数无关；越界那一笔不经过正常计算，两端都不计
（它没有计算阶段）。`MU-DSA-CALC` 取这两拍，整段落在 `MU-DSA` 里面。

CALC 起点还另带十条标量尺寸 `dsa_calc_start_expert` / `dsa_calc_start_k` /
`dsa_calc_start_n` / `dsa_calc_start_kblock` / `dsa_calc_start_nblock` /
`dsa_calc_start_prim_k` / `dsa_calc_start_prim_n` / `dsa_calc_start_adtype` /
`dsa_calc_start_bdtype` / `dsa_calc_start_outbf16`，与 task / user 在
`computed == 0` 那一拍一起锁存：专家数 `Experts()`、`K = PrimK × kblock`、
`N = PrimN × nblock`、K/N 各分几块（`kblock` / `nblock`）、原语 `PrimK()` /
`PrimN()`、token/weight 的 dtype 码、输出是否 BF16，M 恒为 1（一条原语就是
1×K×N）。`trace2perfetto.py` 把它写进 `MU-DSA-CALC` 段的 args（`expert_num` / `M` /
`K` / `N` / `block_k` / `block_n` / `prim_k` / `prim_N` / `a_dtype` / `b_dtype` /
`out_dtype`），tracetto 的索引与前端不存这几条。

波形里缺 `ts_user`、边沿信号或「装进 stream」那几条时，启动会提醒缺哪几条，缺的那些行是空的。

## 性能与规模

实测（`moe_lpu.trace`，2.9 MB / 48 chip / 480 个 core 其中 402 个配了任务 / 5226 条通道）：

| 量 | 值 |
| - | - |
| 建索引 | 32 进程 **0.1 s**，索引 **0.3 MB** |
| 第二次起服务 | **几毫秒**（波形没变，直接用现成的索引） |
| `/api/init` | **13.0 KB**，gzip 之后 1.9 KB |
| 一次窗口查询（一个 core 的十三条通道，看全波形） | 计算 core **552 B**，dot core **888 B**（平移缩放走这条） |
| 服务常驻内存 | 几十 MB（索引文件按需打开，最多同时开 64 个） |

合成波形按“每条行的段数 ×K”量过（`selftest` 里就有这条）：

| | K=1 | K=8 |
| - | - | - |
| 索引 | 8.9 B/段 | 7.8 B/段（**线性**） |
| `init` | 0.1 KB | 0.1 KB（**不涨**） |
| 缩到最小的窗口传输 | 0.2 KB（精确段） | 0.3 KB（粗层，**基本不涨**） |

**仍然随规模线性涨的**（别当成已经解决）：索引体积 = O(段数)（7~9 B/段，本质的，原始段流
压不到更小）；建索引时间 = O(段数)（worker 数封顶在 CPU 数）；请求里 `lanes` 的数量 =
O(可见行数)（前端限一次 200 行）；浏览器每帧画的块数 = O(可见行数 × 像素)；还有
**`.trace` 自己的 trailer 是 8 B/段**（信号号与段数走 varint，段偏移仍是定长 8 字节）。
正因为最后一条，`TraceReader` 打开文件是 **O(信号数)** 而不是 O(全文件段数) —— 一份几十 GB
的波形要是把上万个信号的段表全解出来，光打开就要几十秒、几 GB。

波形本身的格式（meta 版本 3）：模块名做成串表、模块记录走 varint 差，段头只写 varint
（不再有每段重复的段标记与信号号，60 字节降到约 10 字节）。moe_lpu 上整份从 3.46 MB 降到
1.75 MB。早先的 meta v1 / v2 三个读者都还认，能读旧波形。

## 页面怎么用

- 左边点 chip 展开 core，点 core 展开它那十一行（开头只列 chip，免得一屏铺几千行）。
- 滚轮缩放（锚在鼠标处）、按住拖拽平移、`F` 看全、方向键平移、`+` / `-` 缩放。
- 悬停看某一段的全文与起止；点一下选中它，底下给出 `chip · core · 行`、
  `User_id · 单元 · task`、起止与拍数。
- 右上“重新读”重新校验波形（变了才重建索引）。
- 只请求**当前看得见**的那些行与那一窗：请求去抖合并、带 `AbortController` 取消在途，
  索引换代了（build 对不上）会自动重拉 init。

## 已知的取舍

- **段太多时只有密度**：缩到一屏几千段时服务端给的是粗层格子（一格一色 + 深浅表示忙的
  比例），段上的字与精确边界都没了 —— 放大回来就有。
- **颜色只有十一种**（每行一种），不区分 user：同一颜色下会有很多笔不同的
  task，认哪一笔靠段上印的 `User_id`。
- **段表没有了**：段是按窗口取的，所以只在点选时给单段的明细，不再列整条行的表。
- 属性框里的耗时是精确的（段就在手上），但没有“忙多少拍 / 占多少比例”这类统计 ——
  那是服务端的活，这一版没做。
- **不做按 user 过滤/搜索**：8000 个用户下迟早要，协议上留了口子（`/api/window` 加一个
  `user=` 参数即可，服务端只是多一个过滤判断），这一版不做 UI。
- 不做增量建索引：波形变了整份重建。

## 自检

```shell
python3 src/utils/tracetto/selftest_tracetto.py
```

造几份合成波形，把读波形、分段、十一行、索引往返、窗口查询（exact 与 coarse）、复用与
失效（含“同大小同 mtime 换内容”）、并行确定性、规模检查（init 与窗口传输不随段数涨）、
HTTP 端到端各查一遍。`ctest -R tracetto` 也是这一条。
