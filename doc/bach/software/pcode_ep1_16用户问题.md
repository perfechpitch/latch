# EP1 用例改成 16 用户之后的问题

用例是 `test/bach/ip/pcode_bundle.cpp` 的 `PcodeBundle.Ep1OneToken`。它装 BachFlow `pcode/output/ep1_pp3_tp16` 导出的 BACHIR 11 bundle（48 chip、480 核），从 chip 0 的 W 口、path 63 注入 token。用户号从 1 起，`PCODE_USERS` 可改人数，默认 16。通过条件是停钟拍小于 `PCODE_MAX_CYCLES`（默认 400000），并且每个用户的 FC1 部分和、chip 47 核 8 上 concat 的 6144 个 BF16，与 golden 的余弦都 ≥ 0.99。

硬件侧 `stream_num`、stream 表、B core 的 check-flag 环都已经是 16。用例以前把人数写成 2。2 用户能过，不代表 16 用户只是把循环上界改大。

本文只记录已经在波形和停钟 dump 里对上的问题，以及试过但不能留在树上的改法。第 7 节是逻辑核 2 的 path 7 还被改成加法时的全流程现场。第 9 节把 13 个 Step 拆成从 Step0 累加的前缀，分别用 16、32、128 用户跑 latch。当前源码也没有一版同时让 2 用户和 16 用户通过。

---

## 1. 已经对上、2 用户因此能过的问题：topK 被写进 Matrix Mem

DTE 出站任务可以一段在 Core Mem、一段在 Matrix Mem。EP1 的 kernel 把 DST header、topK 写到 Core Mem（`0x48400000`、`0x48408000`），数据写到 Matrix Mem（`0x48c00000`）。`Agcu::FromDoc` 按地址高 4 位打内存种类：topK 是 Core Mem，偏移 `0x8000 + n*56`；数据是 Matrix Mem，偏移 `n*6336`。

改之前，lane 按 route 选存储，不按段的种类。route 是 RouterToMm 时，topK 那段也被写进 Matrix Mem 的 `0x8000`。用户序号 `n=5` 的数据从 `5*6336 = 0x7bc0` 起、长 6336，覆盖整个 topK 区 `0x8000..0x8380`。广播再把这 56 字节当 topK 读走，专家号变成数据图案 `0x30` 拼出来的 `u16`（例如 `0x3332`），权重地址越出 Matrix Mem。SRAM 日志的顺序是：先在 `0x8000` 写下 topK 的 `08 00`，再被数据 `30 31` 盖掉，随后读回来仍是 `30 31`。

只有 `n=5` 和 topK 窗口重叠。2 用户只用 `n=0,1`，所以 2 用户在这条上本来就不会爆。修法在 `src/bach/ip/chip/core/dte/lane.h`：读写都按段的种类选 Core Mem 或 Matrix Mem，并用每段自己的响应队列对齐两种延迟（Core Mem 读 13 拍，Matrix Mem 读 8 拍）。`dte_lane`、`dte` 单测在这次修改之后通过。

这版上的 2 用户（`/tmp/ep1_2.log`）：

| 拍 | 指令数 | 在途 |
|---|---|---|
| 8192 | 11201 | 16 |
| 10041（停钟） | 11627 | 16 |

B core 指令 2449。FC1 与 concat 余弦都是 1。ShareMem 的 head/tail 都是 2。停钟早于周期上限。

16 用户的专家序号在用例里已经按 `local % 19` 折回前 20 个物理专家，避免再走出只铺了 20 个专家的 Matrix Mem。这一条不是 16 用户现在的卡点。

---

## 2. 16 用户第一道卡点：包头队列 16 深，pcode 不 pop

`CoreStation` 的包头 FIFO 深度是 `kHeaderFifoDepth`。当前源码是 16。就绪条件是 `size + 2 <= depth`，所以深度 16 时，队列到 15 就不再收新的包头。

latch 自己的 `kernel_dte.c` 在任务开始时写 `hdr_pop`。pcode 导出的 DTE kernel 不写。槽要等到这个用户 Retire 才释放。16 个用户同时在途，每人还留着前一条 path 的包头，15 个槽被占满之后，归约结果的包头也进不了核。等这份结果的任务停在读包头上，结果又交不完。

`PCODE_MAX_CYCLES=16384` 的 16 用户（`/tmp/ep1_16b.log`）停在这个状态：包头队列顶在 15，交叉开关 hold why = 1（归约准入没过），归约输出停在半包（例如 `out u7 512/2304`，`lv 000`）。在途大约 1012。concat 余弦约 0.28。FC1 余弦是 1，但每个用户的 token 字节和折回后的专家都一样，FC1 golden 相同，这个余弦分不出用户。

试验时把深度改成 128 再跑到 80000 拍（`/tmp/ep1_16c.log`）：在途从约 1012 降到 432，然后从第 16384 拍一直停在 432。每 8192 拍指令大约增加 1890，是 check-flag 在转，不是任务在往下走。concat 余弦升到约 0.5。包头队列最深约 48，没有再顶到 128，所以包头深度不再是这之后的上限。ShareMem head = tail = 16，16 个 token 都已经进 B core 并被 check-flag 消化。

源码里的常数现在仍是 16。注释写了「16 用户 × 每用户若干包要留余量」，和常数不一致。深度 128 只存在于那次试验的二进制里，没有留在当前文件中。只加深度也不够：128 那次把大约 580 个在途放出去之后，剩下的 432 卡在第 4 节。这 32 个包头是谁，见第 3 节。

---

## 3. 卡在哪颗核、哪一步

16 用户跑到第 80000 拍仍停着的那次（`/tmp/ep1_16c.log`，包头深度试验值 128），现场落在 **Step10：FC2 部分和归约**。场景文件 `glm5_ep1_pp3_tp16.py` 的注释叫 Step10，路由着色函数写的是 `step(11)`，对应源码第 246 行 `reduce(fc2_psum, fc2_out, ...)`。这一步还没做完，Step11 的 concat（chip 47、物理核 p8、逻辑核 7）没有收齐，所以 concat 余弦停在约 0.5。

坐标先对齐。latch 的 chip 号是 `gy * 4 + gx`，和 BachFlow 的 `tray * 16 + chip_in_tray` 是同一个数。最东列（`gx = 3`）10 个物理核全好：p0..p3 是逻辑核 0..3，p4 不派角色，p5..p8 是逻辑核 4..7，p9 是 R core。

### 3.1 对得最清楚的一颗：chip 35 物理核 p2（逻辑核 2）

chip 35 是 tray 2、层 0、东列，BachFlow 坐标 `(2, 3)`。它落在 `FC2_CHIPS` 里（tray 2 的 chip 0..3 与 12..15），做 FC2，不做 FC1 / FC3，也不做 silu。导出用的 harvest 是 `disable_mask = 0` 的 h0。这颗核的任务链（`task_meta.json` 键 `352,0`，物理 rank = 35×10+2）是：

| 任务 | 算子 | path | 方向 | 场景步骤 |
|---|---|---|---|---|
| t0 / t1 | token 广播搬入、再发出 | 0 | Router→CM，CM→Router | Step2，第 73 行 |
| t2 / t3 | FC2 输入广播搬入、再发出 | 3 | 同上 | Step8，第 193 行 |
| t4 | FC2 GEMV，1536 B（768 个 BF16，隐藏维的 1/8） | — | Matrix Mem × Core Mem → Core Mem | Step9，第 215 行 |
| t5 / t6 | 归约中转 | 4 | Router→CM，CM→Router | Step10，第 246 行 |
| t7 / t8 | 归约中转 | 5 | 同上 | Step10 |
| t9 | 本核把部分和送进归约 | 6 | CM→Router，`task_type = 4` | Step10 |
| t10 起 | 后续中转 | 7、10、11 | Router↔CM | Step10，停钟时还没走到 |

停钟时这颗核上 16 个用户分成两截，都在 Step10，没有人还停在 GEMV：

- 用户 1、2 都在 `t9`：`fsm = Infly`，`rmem_busy = 1`，`done_bitmap = 0x1ff`（t0..t8 都已完成）。归约上下文在做 path 6。路由表 h0 上这颗核的 path 6 是 `operation = 3`（本核一份加上两路上游），`expect_mask = 0x7`，出方向向东，下一跳是同 chip 的物理核 p3（逻辑核 3）。用户 1 已到两路（`got 0x6`），用户 2 只到一路（`got 0x4`）。
- 用户 3..16：`t5`，`fsm = Wait`，`task_type = 3`（归约中转的搬入，要等 path 4 的包来唤醒），`done_bitmap = 0x1f`（t0..t4 完成）。Step9 的 GEMV 已经做完，卡在 Step10 的第一跳搬入，等的那一包还没到。

缺的那一路就在本核入口上，没有丢到别的 chip。入口站 `st_mid` 里堆着用户 1、path 6 的 40 个 flit。p2 在 2×5 阵列的第 0 行，南口走 mid；南邻物理核 p7（逻辑核 6）的 path 6 出方向是北。这 40 个 flit 就是 p7 发来、归约还没收下的那一路。另外两路（本核自己的部分和，以及西邻 p1 已经进来的那一路）已经记进 `0x6`。

堵住它的是同一用户的下一条 path，也在本核上。`st_left` 里是用户 1、path 7 的 46 个 flit，来自西邻 p1（path 7 向东）。交叉开关两个入口的队头都是这包 path 7，输出掩码 `0x10` / `0x20`（两个归约口），hold why = 1。path 6 要进的也是归约 VC，入口门限是 2，path 7 占满之后 `st_mid` 里的 path 6 发不进去。path 6 的第三路不进归约，t9 不结束，上下文不放，path 7 就一直不准入。

包头队列在这颗核上是 32，内容是 16 个用户各两份：path 0 和 path 3，也就是已经做完的 Step2、Step8 两次广播的包头。pcode 不 pop，所以它们还占着槽。深度 128 时这 32 个槽够用，归约结果还能再进来，卡点才落到上面的 VC。深度改回 16 时，`size + 2 <= 16` 在 15 就停收，第三次包头（Step10 的归约结果）排不进去。第 2 节的包头顶满和第 4 节的队头阻塞是同一步的两道门，不是两个算子。

### 3.2 同一步、晚一跳：chip 35 物理核 p5（逻辑核 4）

同一颗 chip、h0 的任务链上，p5 的本核归约是 `t13`、path 8（同样是 Step10、CM→Router、`operation = 3`、向东）。停钟时：

- 用户 4、5、6 停在 `t13`，`fsm = Infly`，path 8，`exp 0x7 got 0x6`。
- 用户 7 停在更早的 path 6 搬入（`t9`，`task_type = 3`，`fsm = Wait`）。
- 用户 8..16 停在 path 4 搬入（`t5`，`fsm = Wait`），和 p2 上的后排用户一样，GEMV 已完成。

这颗核的四个入口站里看不到 path 8 缺的那一路（mid / right / core 都是空的）。`st_left` 上是用户 7 的 path 9，60 个 flit，交叉开关那一拍的 hold why 是 4（已经发出），不是 p2 那种「下一跳占着归约口不放」。p5 是在等上游把 path 8 的第三路送来；上游就是还卡在 path 6 的那些核，包括 p2。

### 3.3 这一步之前已经走过的，以及 2 用户后来死在更早的一步

chip 35 上能看任务号的这些用户，`done_bitmap` 都盖过了 t4。Step2 的 token 广播、Step8 的 FC2 输入广播、Step9 的 FC2 GEMV，在这颗核上已经做完。FC1 余弦是 1，只说明被抽查的那份 FC1 部分和写出来了；它不经过 chip 35，也不能说明 Step10 做完了。

后来几次把下一包挪开的试验里，2 用户死在另一处，不要和上面混成同一颗核。`/tmp/ep1_2k.log` 一类 dump 里，chip 1 物理核 p4（中间列，逻辑核 3，FC1 chip）停在 `t3`、path 1、`task_type = 4`。那条链是 Step2 广播、Step3 FC1 GEMV（第 101 行）、然后 Step5 的 FC1 部分和归约（第 158 行，`reduce(fc1_psum, fc1_out)`）。`exp 0x3 got 0x1`，在途 564。第 10041 拍通过的那次 2 用户是走完 concat 的；这几次没有走到 Step10。

---

## 4. 16 用户第二道卡点：同一用户的下一条 path 堵住当前 path 缺的输入

这一节是第 3.1 节那一拍的机制。核和步骤已经定了：chip 35 物理核 p2（逻辑核 2）的 Step10、path 6。停钟行是：

```text
q 40(p6u1) 46(p7u1) 0 0
xb 2hu1p7:10w1 2hu1p7:20w1 0 0 0
red u1 p6 busy exp 0x7 got 0x6
red u2 p6 busy exp 0x7 got 0x4
hdr 32
```

含义：

- 用户 1 的归约上下文正在做 path 6。`expect_mask = 0x7` 要三路，`in_done_mask = 0x6` 只到了两路，还缺一路。
- 交叉开关两个入口的队头都是用户 1 的 path 7，各 2 个 flit，输出掩码是归约口（`0x10` 是 `kOutReduce0`，`0x20` 是 `kOutReduce1`）。hold why = 1，表示 `reduce_admission` 返回了假，这一拍没有发出去。
- `st_mid`（方向 0，p2 的南口）还堆着 path 6 用户 1 的 40 个 flit，来自南邻 p7。`st_left`（方向 1，西口）堆着 path 7 用户 1 的 46 个 flit，来自西邻 p1。path 6 这 40 个就是归约还缺的那一路。
- 入口缓冲门限 `kXbarInMark = 2`，深度 `kXbarInDepth = 4`。队头这两个 path 7 把 VC 占到门限，站不再往这个 VC 送。path 6 出不了站。

path 7 会进这颗核的归约口，不是因为逻辑核 2 的表项本来就要做 path 7。按场景，path 6 是逻辑核 2 的归约，path 7 是逻辑核 3 的归约。chip 35 上（东列，p2 = 逻辑核 2，p3 = 逻辑核 3）目录里的原表是：

- p3 的 path 7：`operation = 3`，向南。归约在逻辑核 3 上做，结果向南送。
- p2 的 path 7：`operation = 0`，只向东。逻辑核 2 不参与这级加法，把经过它的包原样转给逻辑核 3。

转到逻辑核 3 之前，有两包 path 7 先在逻辑核 2 上汇合，而且是从两个面进来的：

- 逻辑核 1（p1）向东，落到 p2 的西口（left，lane 1）。
- 逻辑核 6（p7）向北，落到 p2 的南口（mid，lane 0）。p2 在 2×5 的第 0 行，南口走 mid。

pcode 允许中间核把这两包都向东转发，目的核靠 `reduce_sel` 把同一入口面上的两包分开。latch 的归约按到达方向分路，同一面两包分不开。`latch_export.py` 的 `_fill_reduce_lanes` 曾经因此把 p2 的 path 7 改成不带本核操作数的加法：`operation` 从 0 改成 2，`reduce_in_mask = 0x3`，结果仍向东交给 p3，p3 从「两路上游加本核」改成「一路上游加本核」。改过之后到达的 flit 走归约口（南口掩码 `0x10`，西口掩码 `0x20`），不再走东口。`/tmp/ep1_16c.log` 里交叉开关上的 `2hu1p7:10` 和 `2hu1p7:20` 就是这一版。

这一改已经撤掉。逻辑核 2 的 path 7 按原表留在转发：`operation = 0`，`flow` 向东（`0x4`），`reduce_in_mask = 0`，`nxtVC = 0`。`OutMaskOf` 只出东口，不进归约上下文。原表 `stallWay = 1`，东口没有资源时可以转存再 reissue。chip 23、chip 35 的物理核 p2 都是这样。逻辑核 3（p3）的 path 7 仍是 `operation = 3`，在它自己的归约上下文里做。两包从逻辑核 2 的东口进到逻辑核 3 的同一面，原表靠 `reduce_sel` 区分；latch 仍按方向分路，这一面只有一条 lane。

这两包进不了归约，是因为同一用户的 path 6 还占着上下文。path 6 才是这颗核自己的归约（改写后仍是 `operation = 3`，`reduce_in_mask = 0x7`）。`CanAcceptHead` 在 `busy` 时只收同一条 path、同一个 `reduce_seq`。`core.h` 里还有一层：本地 type-4 没做完时，别的 path 直接拒绝。path 7 因此停在归约口上，hold why = 1，把归约 VC 占到入口门限，南口上 path 6 还缺的那 40 个 flit 发不进去。

这是同一条 VC 里的队头阻塞，不是两个入口各走各的。`TryInput` 在准入失败时会换下一个 VC，但换不了同一条 VC 里排在队头后面的 flit。path 7 不离开队头，path 6 就进不了归约，path 6 的 `in_done_mask` 补不齐，上下文不释放，path 7 就一直不准入。

第 10041 拍通过的那次 2 用户，没有把「下一条 path」和「当前 path 还缺的那一路」排进同一条已经满的 VC，归约能在下一条 path 堵死入口之前做完。16 用户把这张时序铺开了。

`reduce_seq` 不能用来把三路输入收成一笔。出核时 `commit.h` 把发送方的 `task_id` 打进包头的 `reduce_seq`。注释写的是：同一条归约链上各核的任务链一样，同一笔任务的 `task_id` 也一样；本地任务号则不能拿发送方的 `reduce_seq` 去对（`core.h`）。当前 path 和下一条 path 用 `path_id` 区分，同一笔的三路输入应当共享 `reduce_seq`。把「`reduce_seq` 不同」一律当成下一笔收进旁边，会把当前这笔另一路输入一起收走，当前这笔永远等齐不了。这是后面停车试验里 2 用户新死锁的一种形态：`exp 0x3 got 0x1`，旁边堆着同一条 path、另一个 seq 的十几个 flit，站和交叉开关的队列却是空的。

---

## 5. 试过、不能留的改法

目标都是让 path 7 不要占着 VC，好让 path 6 进去，同时不把还没轮到的用户整段放行。这几版都让 2 用户从「第 10041 拍、在途 16」变成「第 8192 拍在途 564，之后不再下降」。指令数也从 11201 掉到 6955：任务占住了 stream，核不再往下执行，只剩空转。

| 改法 | 2 用户 | 16 用户 |
|---|---|---|
| 归约准入改成下一笔也收，整包按 `(user, path, seq)` 收进 `park`，当前这笔做完再 `Apply` | 第 8192 拍在途 564 | 未再跑到停钟 |
| 准入仍拒绝，但交叉开关把被拒的整包挪到 VC 旁的 `aside`，腾出队头 | 同样是 564。现场是 `red u1 p1 seq 3 exp 0x3 got 0x1`，`aside` 里 18 个 flit | 未单独跑完 |
| 只允许「上下文正在等的那一笔」（`WaitingInput`：该用户已有 busy 分区，且 path、seq 都相同）从队头后面插过去；新用户不准插 | 第 8192 拍仍是指令 6955、在途 564，和上面几版的前 8192 拍相同 | 未单独跑完 |
| 交叉开关改回「准入失败就留在队头」，归约里不同 path / 不同 seq 的 flit 也留在输入缓冲队头、不收进 `park` | `/tmp/ep1_2k.log`：第 8192 拍仍是 6955 / 564，第 14000 拍被周期上限打断 | 未再跑 |

最后一行说明：2 用户回不去，不能只记在「把下一条 path 从 VC 里拿走」上。第 10041 拍那次通过之后，树上还有包头在 Retire 时释放、Core 方向数据 flit 与包头分开门限、归约 `head_notify`、按 lane 的数据类型和 `payload_aux` 这些改动。当前源码相对那次通过的二进制已经对不上，2 用户本身也还没恢复。

停车和旁路会放大流量的原因，在 2 用户的 dump 里是一样的：被拒的下一包一旦离开队头，入口门限就重新打开，站把后面的包继续送进来。这些包里既有当前归约缺的那一路，也有还没占上上下文的新用户。新用户的 `CanAcceptHead` 在 16 个分区没满时返回真，于是被一起放行。16 个用户、几十个核的任务链一旦同时占上 stream，在途会到 564，并且不再下来。只挪「上下文正在等的那一笔」在试验里没有改变前 8192 拍的在途数，说明早段卡住的并不只是这一条插队。

归约单元测试 `router_reduce` 在「下一笔收进 park、不覆盖当前分区」那版上 16 项都过。那是直接往 `ReduceModule` 灌 flit，不经过交叉开关的准入和入口门限，覆盖不了上面的队头阻塞，也覆盖不了 2 用户整片在途涨到 564。

---

## 6. 当前源码落在哪

和这个问题直接相关的行为，以文件为准，不以前面试验的二进制为准。

- `src/bach/ip/chip/core/dte/lane.h`：读写按段的种类选 Core Mem / Matrix Mem。第 1 节的修复还在。
- `src/bach/ip/chip/core/router/core_station.h`：`kHeaderFifoDepth = 16`。Retire 时会丢掉这个用户剩下的包头槽。pcode 仍不写 `hdr_pop`。深度 128 没有留在源码里。
- `src/bach/ip/chip/core/router/reduce_module.h`：`CanAcceptHead` 在分区忙时只接受同一 path、同一 `reduce_seq`。已经进到输入缓冲、但属于别的 path 或别的 seq 的 flit，停在该路队头并计一次 stall，不写进当前分区，也不从缓冲里拿走。文件头注释仍写着「收进 park」，和 `AcceptFrom` 不一致。`park`、`WaitingInput`、`PromoteParked` 还在类型和函数里，这条路径现在走不到。
- `src/bach/ip/chip/core/router/xbar.h`：归约准入失败时记下 hold why = 1，flit 留在该 VC 队头，同一入口的其它 VC 继续试。`GrantPastHold` 直接 `return false`，不从队头后面抽 flit。
- 用例默认人数是 16。`PCODE_USERS=2` 仍可跑 2 用户，但当前树上第 8192 拍在途是 564，到不了第 10041 拍那次通过。

16 用户因此同时压着两件还没同时满足的事：包头槽要容得下 16 个用户、且不靠把下一条 path 提前算完来腾槽；当前 path 缺的那一路要能越过堵在同一条 VC 队头的下一条 path，又不能把还没轮到的用户整段放进归约。

---

## 7. 中间核改成加法之后，16 用户还在的问题

下面是逻辑核 2 的 path 7 还被改成加法时的现场。`/tmp/ep1_16c.log` 用的就是那份表。第 80000 拍在途停在 432，concat 余弦约 0.5。当前导出已经把这一跳改回原表的东转发，这节的死锁还没有用新表重跑。

全目录里这种改写只有两处，拓扑一样，都是东列 FC2 chip 上逻辑核 2 的 path 7：

- chip 35 物理核 p2，转给同 chip 的 p3。停钟 dump 对过的是这一颗。
- chip 23 物理核 p2，同样转给同 chip 的 p3。表和 chip 35 相同，那次 dump 没有单独展开。

下面三件是改写之后仍然把 16 用户卡住的原因。topK 被写进 Matrix Mem 已经不在这里面。

### 7.1 同一用户的 path 6 和 path 7 抢逻辑核 2 的一个归约上下文

改写让逻辑核 2 同时背上两笔归约，而且可以叠在同一个用户上：

| path | 谁的归约 | 改写后在逻辑核 2 上 | 要几路 |
|---|---|---|---|
| 6 | 逻辑核 2 自己 | 原表就是加法，`operation = 3`，`reduce_in_mask = 0x7` | 本核一份 + 两路上游 |
| 7 | 逻辑核 3 | 原表只向东转发；改写后 `operation = 2`，`reduce_in_mask = 0x3`，加完再向东 | 两路上游，没有本核这份 |

归约模块一个用户只有一个上下文。用户 1 的上下文已经锁在 path 6 上，三路里到了两路（`got 0x6`），缺的是南邻逻辑核 6 的 40 个 flit，就在 `st_mid` 里。与此同时，path 7 的两包已经进了交叉开关的两个归约口（南口掩码 `0x10`，西口掩码 `0x20`），准入失败（hold why = 1），因为上下文不收另一条 path。

入口门限是 2。path 7 把归约 VC 占到门限之后，站不再往这个 VC 送，path 6 缺的那一路出不了 `st_mid`。path 6 补不齐，上下文不释放，path 7 就一直不准入。两笔都做不完：逻辑核 2 的部分和发不出去，逻辑核 3 也等不到 path 7 加完后向东送的那一包。

用户 2 在同一颗核上更早，path 6 只到了一路（`got 0x4`）。用户 3 到 16 的 GEMV 已经做完，停在 path 4 的搬入上（`t5`，`fsm = Wait`），等的包要等前面这些归约做完才有。

### 7.2 下游在等这一跳，concat 收不齐

chip 35 逻辑核 4（物理核 p5）的本核归约是 path 8。用户 4、5、6 停在这里，`exp 0x7 got 0x6`，缺的那一路不在本核的入口站里。逻辑核 4 的西口上是用户 7 的 path 9，hold why = 4，这一拍已经发出，不是再占着归约口。它是在等上游。上游就是还卡在 path 6 的逻辑核 2。

Step11 的 concat 在 chip 47 物理核 p8（逻辑核 7）。Step10 没走完，这份 concat 不是最终结果，余弦停在约 0.5。FC1 余弦是 1，只说明被抽查的 FC1 部分和写出来了，FC1 不经过这颗核。

### 7.3 包头深度 16 仍会先挡住同一步

源码里 `kHeaderFifoDepth` 现在是 16。pcode 的 DTE kernel 不写 `hdr_pop`，Step2（path 0）和 Step8（path 3）的包头要留到用户 Retire。16 个用户各两份，就是 chip 35 逻辑核 2 上看到的那 32 个槽。深度 16 在 15 个槽时就不再收新包头，Step10 归约结果的包头排不进去。

把深度临时改成 128 之后，这 32 个槽放得下，在途从约 1012 降到 432，然后从第 16384 拍停到第 80000 拍。停住的原因是第 7.1 节，不是包头又满了（最深约 48）。深度 128 没有留在源码里。所以按现在的源码重跑，16 用户会先撞上包头；把深度加大之后，path 6 / path 7 这道门还在。

把 path 7 从队头挪开、或收进归约旁边，没有让这道门过去。那几版 2 用户在第 8192 拍停在在途 564，死在 Step5 的 FC1 归约（chip 1 物理核 p4，path 1），到不了 Step10。第 10041 拍、在途 16、余弦为 1 的那次 2 用户没有再出现。

---

## 8. 拿掉第 246 行之后，16 用户仍然跑不完

试过只生成到第 215 行的 FC2 GEMV：`glm5_ep1_pp3_tp16.py` 在 GEMV 的 `release()` 之后返回，第 246 行的 `reduce`、concat、pcie 出核都不生成。路由表复用原来的 catalog，导出到 `/tmp/latch_bundle_gemv`。任务数从 3349 降到 1740。chip 35 物理核 p2 的链只剩两次广播和一次 GEMV（t0..t4，最后一项是第 215 行），没有 path 6 / path 7。FC1 归约（第 158 行）、FC3 归约（第 169 行）和 silu 还在。场景文件试完已改回完整流程。

`PCODE_USERS=16`、`PCODE_MAX_CYCLES=80000`，日志 `/tmp/ep1_16_gemv.log`。没有空闲停钟。从第 8192 拍到第 73728 拍，在途一直是 4207，每 8192 拍指令增加约 1890，是 check-flag 在转。第 80000 拍被周期上限打断，快照 0 份，用例失败。ShareMem head = tail = 16，token 都进了 B core。FC1 余弦是 1。concat 余弦约 0.28，因为这一步没有生成，chip 47 上仍是数据图案。

停住的地方不是第 215 行，也不是第 246 行。chip 1 物理核 p4（中间列，逻辑核 3，FC1 chip）上，用户 1 到 15 停在 `t3`、path 1、`task_type = 4`，这是第 158 行的 FC1 部分和归约。`exp 0x3 got 0x1`，`deny 2`（同一条 path，另一笔 seq）。西口堆着用户 1、path 1 的 40 个 flit，交叉开关上是 `2hu1p1:20w1`。包头队列是 15，内容是 path 0 的广播包头，Step5 的归约结果再塞不进去。同 chip 的 p9 上 path 1 是 `exp 0x7 got 0x6`，包头同样是 15。

这次用的是源码里的包头深度 16。先前能走到 Step10 的那次 16 用户，深度临时是 128。深度 16 时，16 用户在 FC1 归约上就已经停住，到不了「第 215 行 GEMV 做完、后面不再有事」那种空闲。

---

## 9. 十三个 Step 拆成前缀之后

每个单元都从 Step0 生成到某一个 Step（含）。后面的 Step 用前面的张量，所以不能单步生成。入口是 `pcode/src/bachflow_pcode/scenarios/ep1_step_units.py`，`generate(..., until_step=)` 在下一步的 `step()` 处停住。路由复用 `pcode/output/ep1_pp3_tp16/route/route.resolved.json`。

产物在 `BachFlow/pcode/output/ep1_units/<name>/`：寄存器、kernel、`task_meta.json`，HTML 是同目录的 `pcode_ep1_<name>.html`。

| 目录 | 含到 | HTML |
|---|---|---|
| `s00_pcie_in` | Step0 接收 | `pcode_ep1_s00_pcie_in.html` |
| `s01_check_flag` | Step1 check flag | `pcode_ep1_s01_check_flag.html` |
| `s02_token_bcast` | Step2 token 广播 | `pcode_ep1_s02_token_bcast.html` |
| `s03_fc1_gemv` | Step3 FC1 GEMV | `pcode_ep1_s03_fc1_gemv.html` |
| `s04_fc3_gemv` | Step4 FC3 GEMV | `pcode_ep1_s04_fc3_gemv.html` |
| `s05_fc1_reduce` | Step5 FC1 psum reduce | `pcode_ep1_s05_fc1_reduce.html` |
| `s06_fc3_reduce` | Step6 FC3 psum reduce | `pcode_ep1_s06_fc3_reduce.html` |
| `s07_silu` | Step7 silu | `pcode_ep1_s07_silu.html` |
| `s08_fc2_bcast` | Step8 FC2 广播 | `pcode_ep1_s08_fc2_bcast.html` |
| `s09_fc2_gemv` | Step9 FC2 GEMV | `pcode_ep1_s09_fc2_gemv.html` |
| `s10_fc2_reduce` | Step10 FC2 reduce | `pcode_ep1_s10_fc2_reduce.html` |
| `s11_concat` | Step11 concat | `pcode_ep1_s11_concat.html` |
| `s12_pcie_out` | Step12 pcie 出 | `pcode_ep1_s12_pcie_out.html` |

latch 仍用 `PcodeBundle.Ep1OneToken`。`PCODE_CHECK=idle` 只看计算核有没有在周期上限前走空，不把 FC1/concat 余弦当成这几级前缀的通过条件。Step1 另开 `PCODE_WATCH_B=1`，把自启动的 B 核算进空闲，否则计算核没有任务时第 3000 拍就会被判停。周期上限：Step0 为 8000，其余 16384。第一次扫描的日志在 `/tmp/ep1_unit_logs/`。下表 Step5 到 Step12 是 9.4.4 改完准入之后的重跑，日志在 `/tmp/ep1_unit_logs/seqfix/`。9.4 到 9.6 仍是改准入之前的记录。

走空：停钟拍小于上限，而且快照份数等于人数。停钟时在途剩 16，是 B 核的 stream 表还占着，计算核已经空了。死锁：8192 拍和 16384 拍的在途相同，这两拍之间指令只增加约 1900，是 check-flag 在空转。

| 前缀 | 16 用户 | 32 用户 | 128 用户 |
|---|---|---|---|
| Step0 | 第 3003 拍走空，head=tail=0，指令 0 | 同左 | 同左 |
| Step1 | head=tail=16，在途 16，打到上限 | head=tail=32，在途 16 | head=tail=128，在途 16 |
| Step2 广播 | 第 9657 拍走空，head=16 | 第 10809 拍走空，head=32 | 第 16384 拍 head=128，在途 321→16 |
| Step3 FC1 GEMV | 第 9721 拍走空 | 第 11449 拍走空 | 在途 1526→634，head=128 |
| Step4 FC3 GEMV | 第 10105 拍走空 | 第 11833 拍走空 | 在途 1269，head=128 |
| Step5 FC1 reduce | 第 10169 拍走空 | 第 11961 拍走空，见 9.9 | 第 22585 拍走空，见 9.9 |
| Step6 FC3 reduce | 第 10233 拍走空 | 第 16384 拍起在途钉在 111，见 9.10 | 第 24576 拍起在途钉在 293，见 9.10 |
| Step7 silu | 在途 111 不动，见 9.10 | 在途 270 不动 | head 停在 51，在途 279 不动 |
| Step8 FC2 广播 | 在途 437→418 后不动 | 在途钉在 521 | head 停在 51，在途钉在 556 |
| Step9 FC2 GEMV | 停在 418，与 Step8 相同 | 在途钉在 509 | head 停在 51，在途钉在 544 |
| Step10 FC2 reduce | 在途钉在 1012 | 在途钉在 1134 | head 停在 51，在途钉在 1163 |
| Step11 concat | 与 Step10 相同 | 与 Step10 相同 | 与 Step10 相同 |
| Step12 pcie 出 | 与 Step10 相同 | 与 Step10 相同 | 与 Step10 相同 |

### 9.1 Step0：datain 没有装进 bundle

Step0 的 pcode 有 `pcie_sw_data_in` 的 DTE kernel，`task_meta` 里是 datain。`SELF_START` 要到 Step1 的 check flag 才写。导出按「有自启动才是 B 核」分类，这颗核留成不派角色，DATAIN 没有写进 bachir。`s00` 的 bundle 是 tasks=0、kernels=0，也没有 `pcie in` 那条落核表项。

16、32、128 用户都在第 3003 拍走空：计算核没有任务。B core ShareMem head=tail=0，B core 指令 0。token 从 chip 0 的 W 口注进去了，没有核来收。

### 9.2 Step1：16、32、128 的 token 都进了 B 核

三种人数在第 8192 拍都已经 head=tail=人数。B 核的 ShareMem 环是 1024 深，128 个用户放得下，check flag 把 tail 推到了同一个数。

停钟没有出现。自启动的 check flag 把 stream 表占满 16 项，之后指令按空转在涨（16 用户从第 8192 拍的 2486 条到上限附近的 5212 条）。`PCODE_WATCH_B` 把这颗核算进在途，所以打到周期上限，快照 0 份。数据已经收完。

### 9.3 Step2 到 Step4：16 和 32 用户走空

token 广播、FC1 GEMV、FC3 GEMV 这三档，16 用户在第 9657 / 9721 / 10105 拍走空，32 用户在第 10809 / 11449 / 11833 拍走空。head=tail=人数。停钟时在途是 16，计算核的 dump 里没有还占着的 stream。

stream 表深 16，32 个用户仍能走完这三档：先进入的用户做完就退休，后面的用户再占表项。

128 用户在第 16384 拍被上限截住，head 已经是 128。在途还在降：广播从 321 降到 16，FC1 GEMV 从 1526 降到 634，FC3 GEMV 从 2858 降到 1269。FC1 GEMV 停钟 dump 里占着的是最后几名用户的 path 0、type 0，fsm 还在 Infly，例如 chip 14 物理核 p0 上用户 128。这三档没有出现归约口 `deny`，也没有在途从 8192 拍钉死到 16384 拍。128 用户需要比 16384 更多的拍才能把队尾做完。

### 9.4 Step5：FC1 归约从这里钉死

16 用户：第 8192 拍在途 2034、指令 37788，第 16384 拍在途仍是 2034、指令 39678。head=tail=16。快照 0 份。

32 用户同样钉住：在途 2076，只比 16 用户多 42。head=tail=32。dump 里用户号 1 到 32 都出现了，多出来的人没有再铺开一倍归约。

128 用户：第 8192 拍 head=tail=60、在途 2131，到第 16384 拍这两个数不变。入口在 60 个人的时候已经转不动，后面的 68 个人进不来。

停住的 path 全是 path 1（FC1 psum reduce）。16 用户的 dump 有 140 颗核：

- 逻辑核 3，15 颗。代表是 chip 1 物理核 p4（中间列）。用户 1–15 在 type 4、fsm Infly，归约 `exp 0x3 got 0x1`，`deny 2`。西口队列 `40(p1u1)`，缺的那一路就堆在这里。包头队列 15，再收不进归约结果的包头。
- 逻辑核 7，代表 chip 1 物理核 p9。`exp 0x7 got 0x6`，三路里缺一路。队列里夹着 path 0 用户 16 的 31 个 flit。包头同样是 15。
- 逻辑核 0、1、2、3 上有 60 颗核停在 path 1、type 3、fsm Wait。这是过路核上的 P2P reissue datain，还在等一包触发，触发没有来。
- 包头深度 ≥ 14 的核有 119 颗。深度 16 在 15 个槽时就不再收新包头。占满这些槽的是 Step2 广播留下来的 path 0 包头。

这和先前全流程、以及只生成到 FC2 GEMV 时看到的是同一处：FC1 归约缺输入，缺的 flit 因为包头队列和归约准入出不了站。Step3 的 GEMV 本身在 16 和 32 用户上是走空的，问题从 Step5 的归约开始。

#### 9.4.1 reissue 有没有发生，停住是不是 credit

停住的原因是归约准入和包头队列。credit 这一关在已经跑起来的归约任务上是过了的，reissue 任务没有被触发。

任务链里 reissue 是成对写上的。以 chip 5 物理核 p0 为例，Step5 前缀的链是：

| 序号 | type | 含义 | credit_en | path |
|---|---|---|---|---|
| 0 | 2 | 广播 reissue datain | 0 | 0 |
| 1 | 1 | 广播 reissue dataout | 1 | 0 |
| 2 | 0 | GEMV | 0 | 0 |
| 3 | 3 | path 1 的 P2P reissue datain | 0 | 1 |
| 4 | 1 | path 1 的 reissue dataout | 1 | 1 |

16 用户停钟时，这颗核上用户 1–15 的当前任务都是 t3，fsm 是 Wait，done 位图是 `0x7`。t0、t1、t2 已经结束，当前停在 path 1 的 reissue datain 上，后面那条带 `credit_en` 的 reissue dataout 还没轮到。全场 140 颗核里，当前任务是 type 1（reissue dataout）的一条都没有。没有人停在“等 credit 再把 reissue 发出去”这一步。

type 3 在 Wait，表示这一对 reissue 既没有被执行，也没有被跳过。包以 `reissue=1` 重新注入时，datain 会被唤醒，接着跑 credit_en=1 的 dataout。包作为纯转发经过、并且 credit 够用时，这一对会被标完成、DTE 不启动。这两种结果都没出现：60 颗停在 type 3 的核，自己的 router 队列和 xbar 里都没有 path 1 的 flit。该来触发它们的那一包还在上游。

那一包停在归约核的西口。20 颗核的队列都是 `40(p1u1)`，xbar 上是 `2hu1p1:20w1`。`w1` 是归约准入失败。credit 不够时的 hold 原因是 `w3`，这 20 颗核上一条 `w3` 都没有。准入失败在查 VC credit 和核内 credit 之前就返回了，所以这包没有进入“credit 不够就转存 reissue”的分支。

准入失败的原因码是 `deny 2`：这个用户的归约分区已经占着，队头这一包的 `reduce_seq` 和分区里记下的不是同一笔。20 颗核都是 path 1、`exp 0x3 got 0x1`，只收进了一路，缺的那一路就以 40 个 flit 顶在归约口。40 个 flit 也进不了 reissue 转存：转存只收 head 和 tail 同拍的单 flit 整包，多 flit 留在 VC 里。

归约任务本身已经过了 credit。chip 1 物理核 p4 的 t3 是 type 4、`credit_en=1`、path 1，用户 1–15 都在 fsm Infly，`rmem 1`。它们在等缺的那一路输入，不是在等 credit。逻辑核 7 上 type 4 还在 Wait 的（例如 chip 5 物理核 p9 的 t3），`credit_en=0`、`wait_wake=1`，是归约目的端在等结果包，同样不是 credit。

所以 Step5 的停钟是：path 1 缺的那路输入被归约分区拒绝（`deny 2`），包头队列又停在 15，过路核上的 reissue 对一直等不到触发。credit 不满足没有发生，reissue 任务也没有发生。占分区的是谁、被拒的是谁，见 9.4.2。

#### 9.4.2 归约准入失败：同一用户的两路输入，reduce_seq 差 1

`deny 2` 的含义是：这个用户的分区已经在忙，新来的包 path 对得上，`reduce_seq` 对不上。停钟 dump 里 21 颗核都是这一条，而且占分区的 path 和被拒包的 path 都是 path 1。不是另一个用户，也不是另一条 path。

`reduce_seq` 用的是发送核上这笔归约任务的 task id。飞书侧没有规定同一笔归约各核的 task id 必须相同，见 9.4.3。latch 的 `commit.h` 自己加了这句假设，于是两路输入的序号必须一样才会被收进同一个分区。实际任务链有两种：

| 核 | 任务链 | 归约 task id，也就是发出去的 reduce_seq |
|---|---|---|
| 102 颗 | t0 广播 reissue datain，t1 reissue dataout，t2 GEMV，t3 归约 | 3 |
| 26 颗 | t0 普通 datain，t1 GEMV，t2 归约 | 2 |

26 颗短链核的 Step2 没有那对 reissue，少一笔任务，归约就落到 t2。它们是：

- gy 0：chip 1 的 p0/p1/p3（逻辑核 0/1/2），chip 2 同样三颗，chip 3 的 p0（逻辑核 0）和 p3（逻辑核 3）。
- gy 3、6、9：西列（gx 0）的 p4（逻辑核 3）；中间两列的 p4（逻辑核 3）和 p6（逻辑核 5）；东列的 p3（逻辑核 3）。chip 号是 12–15、24–27、36–39。

本级要收两路（`reduce_in_mask = 3`）。本核自己那一份从 core 口进来，表项没有把本核操作数错开到 reduce1/reduce2，所以落在 lane 0。西边邻居那一份从 left 口进来，落在 lane 1。先到的一路把分区打开，并把 `reduce_seq` 记成自己的 task id。后到的一路 task id 不同，准入直接拒绝，包留在西口。现场全是 `exp 0x3 got 0x1`：lane 0 已经进了，lane 1 被拒。用户 1–15 每人都有这样一个分区。队头是用户 1，所以 dump 上钉住的 `deny 2 u1 p1 held 1` 是用户 1；用户 2–15 的第二路排在这 40 个 flit 后面。

**本核序号是 3，西边邻居序号是 2。** 三颗：

- chip 1 物理核 p4，逻辑核 3。用户 1–15 的分区是 `p1 seq 3 busy exp 0x3 got 0x1`。seq 3 是这颗核自己的 t3。西口 `40(p1u1)` 来自同 chip 的 p3，逻辑核 2。p3 是短链，归约是 t2，这包的 `reduce_seq` 是 2。用户 1 的分区被用户 1 自己从本核送进去的那一路占着，西边逻辑核 2 的那一路进不去。
- chip 2 物理核 p4，逻辑核 3。同样：本核 seq 3，西边 p3（逻辑核 2）seq 2，西口 `40(p1u1)`。
- chip 3 物理核 p1，逻辑核 1。本核 seq 3。西边 p0 是逻辑核 0，短链，序号 2。西口同样是用户 1 的 40 个 flit。

**本核序号是 2，西边邻居序号是 3。** 18 颗，就是上面 gy 3/6/9 那 18 颗短链核，它们自己就是这一级的归约点。代表是 chip 13 物理核 p4，逻辑核 3：分区 `seq 2`，是它自己的 t2 占着 lane 0。西边 p3 是逻辑核 2，长链，序号 3。西口 `40(p1u1)` 就是这包，用户 1，path 1，被自己的分区拒绝。同形的还有各层的逻辑核 5（物理核 p6，西边是 p5，序号 3）和东列的逻辑核 3（物理核 p3，西边是 p2，序号 3）。

这 18 颗里 chip 12 物理核 p4 有一处差别。分区同样是自己的 seq 2、`got 0x1`，最后一次准入拒绝也是 `deny 2 u1 p1`。停钟时西口队头不是这包 path 1，而是 path 0 用户 16 的 60 个 flit。逻辑核 2 的 path 1 还没排到归约口的队头。

其余短链核（gy 0 上的逻辑核 0/1/2）不是拒绝方，它们是发出 seq 2 的那一侧，包已经顶在邻居的西口。

#### 9.4.3 reduce_seq 的出处：没有飞书条文要求各核 task id 相同

飞书导出里没有 `reduce_seq` 这个字段，也没有“同一笔归约各核 task id 必须相同”这一条。`BachFlow/pcode/refs/` 的《DTE DSA》《DTE DSA详细设计文档》全文都没有 `reduce_seq`。

latch 建模文档把它写成模型自己的取法：

- `doc/bach/design-for-model/07-units/chip/core/dte.md` 的 F74：出核时 DTE 把发这一包的 `task_id` 打进包头的 `reduce_seq`，Router 的 Reduce Done 原样带回，TS 按 `user_id` 找 stream，不看这个号。文首对应的设计是《DTE 数据搬运引擎》《Router 片上交换与归约》《归约的完整过程》，F74 本身没有引飞书章节号。
- `doc/bach/design-for-model/07-units/chip/core/router.md` 的 F48：整包发出后经 `rmem2ts_done_ch` 带回 UserID、PathID 和包头里的 `reduce_seq`。TS 按 UserID 完成当前任务。
- `doc/bach/design-for-model/03-reduce-归约的完整过程.md`“TS 与 DTE 侧的配合”：包头带 `reduce_seq`，TS 不用它配对。
- `doc/bach/design-for-model/08-文档缺口与-TBD.md`：Router MAS 没写一笔 Reduce 任务含几个 Packet、任务边界靠什么标出来。同一节把“DTE 把发这一包的 `task_id` 打进 `reduce_seq`”记成模型里的做法。修订记录 `10-修订记录.md` 把分区和 Reduce Done 的依据写成 Router MAS F-029、F-030、F-034、F-037，这几条管的是分区分配、到达顺序和做完回报，不包含各核 task id 对齐。

“一条归约链上各 core 的任务链一样，同一笔任务的 task_id 也一样”原先只写在 `src/bach/ip/chip/core/dte/commit.h` 的 `MarkReducePkt` 注释里，旁边是 `d.reduce_seq = d.task_id`。没有飞书引用。同仓库 `src/bach/ip/chip/core/core.h` 写的是相反的一句：同一笔归约里各核的 task_id 各自编号，不能拿发送方的 `reduce_seq` 对本地任务号。`doc/bach/software/pcode_ep1_pp3_tp16接入.md` 第 16 行也记过：pcode 各核任务号各自编号，用发送方 `task_id` 配对对不上。

所以 Step5 的 `deny 2` 是这条代码假设和 pcode 实际任务链不一致，不是飞书规定被违反。9.4.4 按 user id 收各路输入，并改掉了这句注释。

#### 9.4.4 按 user id 收同一笔的各路输入

同一 user、同一 path 上还没到的那一路，就是这一笔归约的输入，不再看 `reduce_seq`。`reduce_seq` 仍由发送核打上自己的 task id，Reduce Done 原样带回，TS 不拿它配对。另一条 path 仍是下一笔，留在输入队头，不写进当前分区。同一路已经收过一份之后再来的包也是下一笔，避免盖掉已经加进去的数。

改动：

- `src/bach/ip/chip/core/router/reduce_module.h`：`CanAcceptHead`、`WaitingInput` 只比 path。`AcceptFrom` 里，path 不同或这一路的 `in_done_mask` 已经置位才算下一笔。
- `src/bach/ip/chip/core/dte/commit.h`：删掉“各核 task id 必须相同”的注释。发包仍写 `reduce_seq = task_id`。

`BachReduce.DifferentTaskIdSameUserAdds`：同一 user、同一 path，一路 `reduce_seq = 3` 值为 10，另一路 `reduce_seq = 2` 值为 32，结果一包 42。`BachReduce` 17 项都过。同一路已经收过再来不同 seq 的包，仍是先出 42、再出 1000，不会盖掉前一笔。

Step5 前缀、16 用户重跑（`PCODE_CHECK=idle`，上限 16384，日志 `/tmp/ep1_unit_logs/s05_fc1_reduce_u16_seqfix.log`）：第 8192 拍在途已经是 16、head=tail=16，第 10169 拍走空，快照 16 份，指令 41022。gtest 通过。这次没有算 FC1/concat 余弦。32 用户和 128 用户还是修正前的记录，没有重跑。Step6 及以后的死锁记录也是修正前的。

### 9.5 Step6：FC3 归约用同样的方式再钉一处

16 用户第 8192 拍在途 2415，第 16384 拍仍是 2415，指令 37944→39834，和 Step5 停住时的指令数差不多。head=tail=16。path 1 的现场还在：chip 1 物理核 p4 仍是 `exp 0x3 got 0x1`、`deny 2`、队列 `40(p1u1)`、包头 15。

FC3 的 path 2 在 FC3 的 chip 上同时钉住。逻辑核 5，代表 chip 5 物理核 p6：path 2、`exp 0x3 got 0x1`、`deny 2`、队列 `40(p2u1)`、包头 15。逻辑核 6（chip 5 物理核 p8）同样是 path 2、`exp 0x3 got 0x1`。两笔归约都缺一路输入，包头队列都被前面的广播包头顶在 15。

32 用户的在途是 2437，形态和 16 用户相同。128 用户 head 停在 55、在途 2446、指令 43882。silu 和 FC2 要等这两笔归约结束才有输入。

### 9.6 Step7 到 Step12：指令数停在 Step6，后面的任务只是占着表

Step7 的 16 用户停钟 dump 与 Step6 逐项相同：在途 2415、指令 39834、head=tail=16。32 用户同样是在途 2437、指令 42157。128 用户与 Step6 的 128 用户相同，head 停在 55。silu 没有人做进去。

Step8 把 FC2 的 token 广播放进任务链之后，16 用户的在途从 2415 升到 4207，指令数仍是 39834。新占着的是 path 3：type 2、fsm Wait 有 1224 项，type 0、fsm Wait 有 568 项。32 用户在途 4229、指令 42157。128 用户 head 仍是 55、在途 4238、指令 43882。Step9 与 Step8 这三组数字相同。FC2 GEMV 排在这条广播后面，广播本身还在 Wait。

Step10 的在途再多大约一百：16 用户 4309，32 用户 4331，128 用户 4340。三种人数的指令数仍是 39834 / 42157 / 43882。dump 里没有 path 6，也没有 path 7。原先会做 FC2 归约的 chip 35 物理核 p2（逻辑核 2）停在 path 3、type 2、fsm Wait，用户 1 起一串都在等广播。FC2 归约没有变成当前任务。

Step11 和 Step12 与 Step10 的停钟数字相同。concat 和 pcie 出没有开始。

### 9.7 改完 reduce_seq 之后，Step5 到 Step12 重跑

16 用户的 Step5、Step6 走空。Step5 第 10169 拍，指令 41022，快照 16 份。Step6 第 10233 拍，指令 47949，快照 16 份。停钟时在途都是 16，是 B 核的 stream 表。FC1、FC3 两笔归约不再因为 `reduce_seq` 不同被 `deny 2` 挡住。

当时把周期上限放在 16384。32 用户 Step5 的在途从 160 降到 50，128 用户从 1722 降到 643，看起来还在收尾。把上限放到 65536 之后，这两档都在 50 和 643 上钉死，见 9.8。9.9 按 `streamCheck` 补上出口 credit 之后，32 用户的 Step5 在第 11961 拍走空，128 用户在第 22585 拍走空。Step6 及之后用同一张导出规则重跑的结果在 9.10。

Step7 是改完之后第一处钉死。三种人数从第 8192 拍到第 16384 拍在途不变：16 用户 111，32 用户 268，128 用户 292，指令只增加约 1900。128 用户的 head 停在 60。

16 用户停钟时有 14 颗核。10 颗上的 path 2（FC3 归约）输入已经齐（`exp` 等于 `got`）。目的端是逻辑核 7，包头队列是 15：chip 4 物理核 p9 的包头是 path 0 的用户 7–16 和 path 1 的用户 7–11，归约结果 `out u7 512/2304`，只送出了一截。同形的还有 chip 5、6、7、18 的物理核 p9 或 p8。另外三颗还缺一路 path 2 输入，没有 `deny`。代表是 chip 5 物理核 p3（逻辑核 2），用户 8–16 都是 `exp 0x3 got 0x2`。silu 没有成为这些核的当前任务。

Step8、Step9 没有把 Step7 解开。16 用户在途从 437 降到 418，再从 448 降到 418，指令增量仍是空转那一档。32 和 128 用户的在途降得更多，128 的 head 停在 61。

Step10 把 FC2 归约的任务放进来之后又钉住。16 用户在途 1013→1012，指令 56454→58344。32 用户 1187→1103，128 用户 1213→1129，head 仍是 61。16 用户 dump 里新占多数的是 path 3、type 2（广播 reissue datain，40 颗核）和 path 4、type 3（39 颗核）。path 2 的归约现场还在。Step11、Step12 三种人数的停钟数字与 Step10 相同，concat 和 pcie 出没有再往下走。

### 9.8 Step5 单独拉长到 65536 拍

只跑 `s05_fc1_reduce`，`PCODE_CHECK=idle`。16 用户上限 32768，32 和 128 用户上限 65536。日志在 `/tmp/ep1_unit_logs/s05_rerun/`。这一节是补上 `stream_need` 之前的现场。32 用户和 128 用户在 9.9 里都已经走空。

16 用户走空。第 10169 拍停，快照 16 份，在途 16，指令 41022，和 9.7 里那次一样。

32 用户不走空。32 个 token 都进了 B 核，head=tail=32。在途在第 8192 拍是 160，第 16384 拍降到 50，之后一直到第 65536 拍都是 50。指令每 8192 拍只增加约 1890，是 check-flag 空转。快照 0 份。

停住的只有 chip 4（gy=1 的西列）上四颗核，全是 path 1，也就是 Step5 的 FC1 psum reduce。没有 `deny`。

| 核 | 逻辑核 | 当前任务 | 队列 |
|---|---|---|---|
| p2 | 1 | path 1、type 3、fsm Wait | 左口 16 个 flit，path 1 用户 31，交叉开关要往东送 |
| p3 | 2 | path 1、type 3、fsm Wait | 左口 40 个 flit，path 1 用户 26，往东 |
| p4 | 3 | path 1、type 3、fsm Wait | 左口 40 个 flit，path 1 用户 21，要往 mid（p9）送 |
| p9 | 7 | path 1、type 4、fsm Wait | 包头队列 15，全是 path 0 的用户 17–31。path 1 用户 17 的包已经在交叉开关上，出方向是 core |

逻辑核 1、2、3 停在 path 1 的 P2P reissue datain 上，等触发。这一包还在它们自己的西口，往逻辑核 7 送。逻辑核 7 的包头队列被 Step2 的 path 0 包头顶满，深度 16 在 15 个槽时就不再收新包头，path 1 进不了核。归约模块里没有未完成的分区，也没有 `deny`。

128 用户也不走空。128 个用户在第 129 拍之前都已经注入，但 B 核 ShareMem 的 head 从第 8192 拍起停在 78，并且和 tail 重合，一直保持到第 65536 拍。用户 79–128 没有进入这颗 B 核的环。在途在第 16384 拍降到 643 后不再变。停钟时 B 核交叉开关有一路深度 16，`credit_q` 为 1。

已经进环的用户卡在 path 1 和还占着包头槽的 path 0 上。312 颗核里，219 颗的当前任务是 path 0、type 2、fsm Wait，54 颗是 path 0、type 0、fsm Wait。path 1 上 type 3 Wait 有 22 颗，type 4 Wait 有 15 颗。

归约没做完的代表是 chip 0 物理核 p9（逻辑核 7）。用户 43–55 的 path 1 是 `exp 0x7 got 0x7`，输入齐了，结果只送出 `out u43 512/2304`。用户 56 是 `exp 0x7 got 0x3`。用户 57 的 path 1 包在归约口上，hold 原因是 1。包头队列 14 个，都是 path 0。同 chip 的 p7（逻辑核 5）、p8（逻辑核 6）是 path 1、`exp 0x3 got 0x2`，缺的那一路还没到，西口占着的是 path 0。chip 4 物理核 p9 和 32 用户是同一幅图：包头队列 15，全是 path 0，path 1 用户 21 的包在交叉开关上等着进核。

### 9.9 按 streamCheck 查出口 credit 之后，Step5 的 32 用户走空

`route.resolved.json` 里 chip 4 物理核 p2、p3 的 path 1 是 `streamCheck=1`、`stallWay=1`、只往东。规划的意思是：这一 bit 置上，就对 `directionMask` 里的每个出口查 Stream credit。latch 导出原先只在「本跳进核」时置 core 位，出方向只看下一跳自己有没有 core 位。p2 的下一跳 p3 不进核，所以 path 1 的 `stream_need` 写成 0，东向不查 credit，包只受 VC 深度限制，把西口顶满。

`latch_export.py` 的 `_fill_stream_need` 改成照抄这一 bit：`streamCheck=1` 时，mesh 上的出方向和进核的 core 位都写入 `stream_need`。改完后 chip 4 的 path 1 是 p2/p3 `stream_need=4`（东）、p4 `stream_need=1`（mid）、p9 `stream_need=8`（core）。路由表没有单独的 KB 额度字段，导出的 `nxt_credit_require` 仍是 0；置上 `stream_need` 之后，交叉开关查的是该出口的 stream 表（16 项）以及这个 0 额度。

用这份表重跑 Step5、32 用户，上限 65536，`PCODE_CHECK=idle`。日志 `/tmp/ep1_unit_logs/s05_rerun/u32_stream.log`。第 11961 拍走空，快照 32 份，在途 16，指令 80110，head=tail=32。第 8192 拍在途还是 126。gtest 通过。

同一张表再跑 128 用户，上限 262144。日志 `/tmp/ep1_unit_logs/s05_rerun/u128_stream.log`。第 8192 拍 head=tail=99、在途 2639，第 16384 拍 head=tail=128、在途 1391。第 22585 拍走空，快照 128 份，在途 16，指令 314596。gtest 通过。16 用户这次没有重跑。

### 9.10 补上 stream_need 之后，Step6 到 Step12

`s06` 到 `s12` 按 9.9 的导出规则重出表，上限 65536，`PCODE_CHECK=idle`。日志在 `/tmp/ep1_unit_logs/streamfix/`。16 用户的 Step6 走空，第 10233 拍，快照 16 份，在途 16，和改表前一样。从 Step6 的 32 用户起，其余组合都在上限前钉死，快照 0 份。

Step6 的 32 用户在第 16384 拍把在途收到 111，之后到 65536 不再变，指令每 8192 拍只加约 1890。停在 chip 29、chip 30 共 11 颗核上，都是 path 2（FC3 psum reduce）和还占着路的 path 1。6 颗核的当前任务是 path 2、type 4、fsm Infly，`rmem` 仍占着。归约结果只送出一截：chip 29 物理核 p9 是 `out u19 512/2304`，p4 是 `out u25 768/2304`。这两颗核的包头队列分别是 15 和 8。p9 上 path 1 用户 22 的包要进核，出方向是 core。另一批 path 2 归约还缺一路输入，`exp 0x3 got 0x2`，西口被 path 1 的包占着，深度到 40。代表是 chip 29 物理核 p4，西口队头 path 1 用户 26。

Step6 的 128 用户在第 24576 拍收到在途 293 后钉住，head=tail=128。74 颗核里，34 颗的当前任务是 path 0、type 2、fsm Wait。path 2 的归约有 10 颗停在 Infly，结果同样只送出一截，例如 `out u98 3584/4608`。32 用户这一档的核、path 和原因见 9.10.1。

#### 9.10.1 Step6 的 32 用户

日志 `/tmp/ep1_unit_logs/streamfix/s06_fc3_reduce_u32.log`。32 个 token 都进了 B 核，head=tail=32。在途在第 8192 拍是 200，第 16384 拍收到 111，之后每 8192 拍指令只加约 1890，到第 65536 拍仍是 111。快照 0 份。这 111 条就是下面 11 颗核上的 95 条任务，加上 B 核的 16 条 stream。其余核已经空了。没有 `deny`。

11 颗核都在 gy=7 这一行的两片中间 chip 上：chip 29（gx=1）和 chip 30（gx=2）。中间列坏核是物理核 p2、p7，剩下的计算核按物理序对应逻辑核 0–7。path 1 是 FC1 psum reduce，path 2 是 FC3 psum reduce。两片的物理核 p9 都是逻辑核 7，path 2 的表项 `flow_dir 16`、进核、`stream_need 8`、`stallWay 0`：归约结果要进本核，不再往东送。

| 核 | 逻辑核 | 当前任务 | 现场 |
|---|---|---|---|
| chip 29 p9 | 7 | 用户 19–21：t4，path 2，type 4，fsm Infly，`rmem` 占着。用户 22–30：t3，path 1，type 4，fsm Wait | 包头队列 15。归约结果 `out u19 512/2304`，包头还在归约口上，出方向是 core。mid 口 60 个 flit，path 1 用户 22，也要进核。用户 19–21 的 path 2 输入已经齐（`exp 0x7 got 0x7`） |
| chip 30 p9 | 7 | 用户 14、15：path 2，type 4，fsm Infly。用户 16–26：path 1，type 4，fsm Wait | 同一幅图。`out u14 512/2304`。包头队列 15。mid 口是 path 1 用户 16 |
| chip 29 p4 | 3 | 用户 25：path 2，type 4，fsm Infly。用户 26–32：path 1，type 3，fsm Wait | 西口 40 个 flit，path 1 用户 26，要往 mid 送。`out u25 768/2304`，结果的包身也在往 mid 送。用户 26–30 的 path 2 是 `exp 0x3 got 0x2` |
| chip 30 p4 | 3 | 用户 19：path 2，Infly。用户 20–28：path 1，type 3，Wait | 四个口的队列是空的。`out u19 768/2304`，包身在归约口上，出方向是 mid |
| chip 29 p8 | 6 | 用户 23–31：path 2，Infly。用户 32：path 0，type 2，Wait | 输入除用户 32 外都齐。`out u23 1536/4608`，往东。用户 32 的 path 2 是 `got 0x2` |
| chip 30 p8 | 6 | 用户 18–26：path 2，Infly。用户 27：path 0，type 2，Wait | 用户 18–26 输入齐。`out u18 1536/4608`，往东。左口 40 个 flit 是 path 2 用户 28，出方向是归约口，hold 原因是 1。用户 27 是 `got 0x2` |
| chip 30 p6 | 5 | 用户 32：path 2，Infly | 输入齐（`exp 0x3 got 0x3`）。`out u32 3584/4608`，包身往东，东邻是 p8 |
| chip 29 p3 | 2 | 用户 31、32：path 1，type 3，Wait | 西口 path 1 用户 30 的包身，往东。path 2 用户 31、32 是 `got 0x2` |
| chip 30 p0 | 0 | 用户 24–32：path 1，type 3，Wait | 西口 40 个 flit，path 1 用户 24，往东。没有归约上下文 |
| chip 30 p1 | 1 | 用户 23–31：path 1，type 3，Wait | 西口 40 个 flit，path 1 用户 22，往东。用户 23 的 path 2 是 `got 0x2` |
| chip 30 p3 | 2 | 用户 20–28：path 1，type 3，Wait。用户 29：path 0，type 2，Wait | 西口 40 个 flit 是 path 0 用户 29。用户 20–22 的 path 2 是 `got 0x2` |

逻辑核 7 的任务链是 t0–t2 走 path 0，t3 是 path 1 的归约搬入，t4 是 path 2 的归约（`credit_en`，不 `wait_wake`），t5 才是 path 2 的归约搬入。用户 19–21 的 `done` 是 `0xf`，停在 t4，t5 还没排到。用户 22–30 的 `done` 是 `0x7`，还停在 t3。

包头队列的容量是 16。`HeaderReady` 要求 `size + 2 <= 16`：ready 电平晚一拍，路上还能再来两个包头，所以占到 15 个就不再往里送。停钟时占了 15，还空着 1 个槽，这个空槽是留给在途包头的余量。这一拍挡在包头就绪之前，还没查到 Core Mem credit。path 2 落地项的 `stallWay` 是 0，结果包头的 hold 原因是 0。chip 29 物理核 p9 的 15 个槽是 path 0 的用户 19–30，加上 path 1 的用户 19–21。这些用户都还在流表里，槽要等退休才还。path 2 用户 19 的结果已经在归约口排出 2 个 flit（512 字节，整包 2304 字节、9 个 flit），包头的出方向是 core，hold 原因是 0。新包头进不去，这一笔结果交不完，`rmem` 就不释放。用户 20、21 的 path 2 输入已经齐，排在它后面。

同一队列也挡住了 path 1。用户 22 的 path 1 包在 mid 口，出方向同样是 core，t3 一直停在 Wait，FC1 归约搬入没有被唤醒。西边逻辑核 0–3 的西口因此堆着 path 1：深度 40 是一个 VC 的 private 加 shared，这一跳的链路 credit 是 0。这些核上 path 1 的当前任务是 type 3（P2P reissue 搬入），fsm 仍是 Wait。包还在口上，转发没有发生，跳过 reissue 和触发 reissue 都还没到。path 2 在这些核上缺的那一路（`got 0x2`）就排在这批 path 1 后面。

逻辑核 5、6 是 path 2 的中间加法，结果往东，不进本核。p6 用户 32 的结果已经排出 3584/4608，东邻 p8 的西口被 path 2 用户 28 占着，而且那一包在归约口上进不去：p8 上用户 18–26 的分区还占着，输出 `out u18 1536/4608` 同样往东送不完。东边的尽头就是逻辑核 7 那个满的包头队列。

这条链上没有 credit 把 reissue 唤醒后再卡住。落地的 path 2 表项 `stallWay` 是 0，结果包头的 hold 原因是 0，归约模块也没有 `deny`。卡住的是包头队列：path 0 和已经做完的 path 1 包头占满 15 个槽，path 2 的结果进不了逻辑核 7，分区不释放，西边的 path 1、path 2 包就停在 VC 里。

Step7 起 16 用户也不走空。在途从第 8192 拍就是 111，一直保持到 65536。14 颗核里 12 颗停在 path 2、type 4、fsm Infly。chip 4 物理核 p9 的包头队列是 15，归约结果 `out u7 512/2304`。silu 没有成为当前任务。32 用户在途钉在 270。128 用户的 head 从第 16384 拍起停在 51，用户 52–128 没有进入 B 核的环，在途钉在 279。

Step8 没有把 Step7 解开。16 用户在途从 437 降到 418 后不动。新占多数的是 path 3、type 2、fsm Wait（22 颗），path 2 的归约现场还在（10 颗）。32 用户钉在 521，128 用户 head 仍是 51、在途 556。Step9 的 16 用户停在同一个 418。32 用户 509，128 用户 head 51、在途 544。

Step10 把 FC2 归约放进来之后在途变多，但仍然钉住：16 用户 1012，32 用户 1134，128 用户 head 51、在途 1163。16 用户 dump 里新占多数的是 path 4、type 3、fsm Wait（55 颗）。path 2 的归约还在（13 颗），path 4 上已经有 8 颗 type 4、fsm Infly。Step11、Step12 三种人数的停钟数字与 Step10 相同，concat 和 pcie 出没有再往下走。
