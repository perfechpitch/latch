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

### 9.11 按飞书把整包改送进 Core 之后，Step7 的 128 用户仍不走空

飞书 Router F-025：下游 Stream 不够且 `stallWay=1` 时，还没有向原目标交出任何 flit 的完整 Packet 进 Core，原目标一个 flit 都不收。flit 数不限。已经向原目标送出过头的包，保持原路径直到包尾。VC credit 不够不触发这次重发。

latch 原先只在 `head && tail` 的单 flit 整包上走溢流口，进的是 `CoreMemReissue`，多 flit 留在 VC。这是 `xbar.h` 自己的限制，飞书没有这一条。`CoreMemReissue` 的出线没有接到 DTE，按 flit 计数，默认每个 VC 两笔就断言。所以整包不能走那条口。

改成直接从交叉开关送进本核的 CoreStation，一拍一个 flit，直到包尾：

- 包头这一拍定下来。条件是 `stallWay=1`、这一拍原目标拿不到资源、出口不是被别的入口占着、本核收得下。后面的 flit 跟进同一核，不再试原目标。已经发出过头的包身不改道。
- 送进本核的包头打上 `reissue=1`，打在这一份副本上。多播的几个方向共享同一个 `Message`，改共享对象会把还在别的路口上的副本也标成重发。
- 本核自己发出去的包不再送回本核。表项里的进核位描述的是从 mesh 进来的那一跳，送回去就是同一个用户的第二个 token。同一条 VC 上，包身若还排在别的包后面，把同一用户、同一 path 的后续 flit 抽出来跟着进核；别的 VC 继续走。
- 触发仍是「原目标这一拍资源不够」，Stream 槽满和 VC credit 用光都会进来。飞书写的是只由 Stream 不足触发。这次没有把触发收窄到只有 Stream。`BachRouterAsm.StallWayStoresWhenCreditIsGone` 因此改成：40 个 credit 用完后，剩下的 10 笔单 flit 整包进核，不再进溢流口。`BachRouterAsm` 14 个测试通过。

用改过的模型重跑 Step7、128 用户。bundle 是 `/tmp/latch_ep1_s07`，上限 32768，`PCODE_CHECK=idle`。日志 `/tmp/ep1_unit_logs/s07_u128_divert.log`。

第 8192 拍 head=128、tail=100、在途 362、指令 38765。第 16384 拍 head=128、tail=102、在途 363、指令 38817。之后到第 32768 拍这三个数不动。快照 0 份。gtest 失败。B 核指令 8490。128 个用户都进了 B 核的环，tail 停在 102，还有 26 个没有离开这颗核。指令从第 16384 拍起完全不增加，核不再空转。

停钟时 61 颗核有任务。当前任务条数：path 1、type 3、fsm Wait 136 条；path 0、type 1、fsm Wait、`done 0x1` 114 条；path 1、type 4 73 条；path 2、type 4、fsm Infly 18 条；path 0、type 0 16 条（都在 B 核）；path 0、type 2 6 条。

path 0 的 type 1 是重发搬出。`done 0x1` 表示搬入已经做完，配对的搬出没有被跳过，所以这些包是带着重发标记进的核。这些核的四个口是空的，包在核里，搬出停在 Wait，等 credit。16 个 stream 槽被这些还没退休的用户占满。

代表现场：

- 全局 chip 0（tray 0，片内 0，gx=0，gy=0）物理核 p9，逻辑核 7。16 条 stream 里 15 条是 path 0、type 1、fsm Wait，用户 24–38。包头队列 16，都是这些 path 0。mid 口 40 个 flit 是 path 1 用户 39 的包头，出方向是归约口，hold 原因是 1。path 1 归约是 `exp 0x7 got 0x3`。
- 全局 chip 4（tray 0，片内 4，gx=0，gy=1）物理核 p1。西口 60 个 flit，path 0 用户 16，掩码 `0xd`（北、东、核），`own 2`。包头已经向原目标交过，包身留在这条 VC 上，这一跳不再整包改送进核。
- 全局 chip 0 物理核 p5，B 核。用户 87–102 停在 path 0、type 0、fsm Wait、`done 0x1`。包头队列 42，是 path 63 的用户 87–128。

整包进核把还没发出去的 path 0 包头送进了核。改之前同一步的 128 用户已经是 head=128、tail=96，指令停在 212631 并且核还在空转；这次 tail 走到 102，指令从第 16384 拍起不再增加。搬出等的 credit 没有回来：已经发出包头的多播包身仍占着 VC，下游 stream 槽也被还没退休的用户占着。path 1 的归约输入因此仍到不了。Step7 的 128 用户没有走空。

### 9.12 同一条 VC 上，进核的包没完成时后面的包不能越过它

飞书 Router F-027：一个 VC 上有一包进了核、重发还没完成，后面的包不能绕过它直接发送。包身还排在后到的包头后面时，要先把这一包剩下的 flit 抽出来，让整包进核。

上一节停钟时，全局 chip 4（tray 0，片内 4，gx=0，gy=1）物理核 p1 就是这个形状：path 1 用户 8 的包头已经进核，包身还在西口，排在 path 0 用户 16 的包头后面。交叉开关的水位把西口挡住，包尾进不了核，type 3 停在 Wait。

`xbar.h` 和 `router_station.h` 按这条改了：

- 一包已经向本核交出包头、包尾还没到时，这条 VC 上别的包不发。station 从队列中间把这一包剩下的 flit 抽出来先送。交叉开关即使已经被后到的包占到水位，也给这一包留一个位置。
- 这一包是改送进核的，后续 flit 只进核。这一包本来就要进核，后续 flit 仍走原来的目标；目标里有一个方向这一拍发不走，整组都不发，不把包拆开。
- 改送的整包进核之后，这条 VC 继续挡住后面的包，直到这笔重发从本核再发完包尾。`BachRouterAsm.StallWayStoresWhenCreditIsGone` 因此改成：40 个 credit 用完后只有第 41 笔进核，同 VC 上剩下的 9 笔留着。`BachRouterAsm` 14 个测试通过。

第一次重跑在包尾校验上断言。核里正在收用户 20 的 path 0（6392 字节），用户 12 的 path 1 包尾插进了同一条进核通路，帧被提前收尾。进核通路不按 VC 分缓冲，一包没到尾不能再收下一包。补上「核里这包的用户和 path 对上才续发」之后，这个断言不再出现。

用这版重跑 Step7、128 用户。bundle 是 `/tmp/latch_ep1_s07`，上限 32768，`PCODE_CHECK=idle`。日志 `/tmp/ep1_unit_logs/s07_u128_f027.log`。

| 拍 | 指令 | 在途 | head | tail |
|---|---|---|---|---|
| 8192 | 48208 | 220 | 128 | 50 |
| 16384 | 48208 | 219 | 128 | 50 |
| 24576 | 48208 | 219 | 128 | 50 |
| 32768 | 48208 | 219 | 128 | 50 |

快照 0 份。gtest 失败。B 核指令 6382。128 个用户都进了 B 核的环，tail 停在 50。指令从第 8192 拍起不再增加。上一节同一步的 tail 是 102，这一版更早停住。

代表现场是全局 chip 4 上相邻的两颗核：

- 物理核 p1。西口 60 个 flit 是 path 0 用户 18。交叉开关西口深度 2，是 path 1 用户 9 的包身，出方向只有东（掩码 `0x4`）。`own 2`，包头队列里有 path 0 用户 18。path 0 用户 18 的包头已经进核，包尾还没到；path 1 用户 9 排在它后面，按 F-027 不能先走。
- 物理核 p2，p1 的东邻。包头队列里有 path 1 用户 9（`h9:1`），type 3 用户 9–16 仍是 Wait，`own 2`。用户 9 的包头已经进了这颗核，包身还在 p1。西口 40 个 flit 是 path 0 用户 17，这一跳的 VC 已经满。

p1 上 path 0 要同时往东和进核。东向 credit 没有了，整组发不出去，包身就留在 p1，核里的这包收不到包尾。path 1 用户 9 又不能越过它。p2 一直等用户 9 的包尾，西口腾不出来。两边互相等。Step7 的 128 用户没有走空。

### 9.13 归约包头按 PhyMemTable 未命中分配

飞书 Rmem：收到上游分量时按 UserID 查 PhyMemTable，没有命中就分配新表项，最多 16 项。latch 原先先要求这个用户已经在本核任务表里，任务表没有就直接拒绝，Rmem 的 16 项空位用不上。全局 chip 0（tray 0，片内 0，gx=0，gy=0）的 p7、p2 上，path 1 用户 7 就这样停在交叉开关里。

`core.h` 的 `WireRouterToTs` 改成：任务表里没有这个用户时，直接交给 `CanAcceptHead`，未命中就分配 Rmem 分区，不新建 TS stream。用户已经在任务表里、并且还有未做完的本地归约时，仍然只收那条 path（`w1` 过滤保留）。新加单元测试 `BachBcore.ReduceAllocatesWithoutATask`：任务表为空时用户 7 的 path 1 包头可以收，`ReserveHead` 之后占 1 项；填满 16 项后用户 200 不再分配。

Step7、128 用户重跑，日志 `/tmp/ep1_unit_logs/s07_u128_rmem.log`。第 8192 拍起不动，停在 32768，snaps 0，在途 54，指令 20822，ShareMem head 128、tail 26。p7、p2 的交叉开关已经空了，path 1 进了 Rmem。卡点移到广播的 VC 0：chip 0 p7 西口 40 个 flit，队头 path 0 用户 7，交叉开关空，header 0，station 不发。

### 9.14 credit 不够时包留在 VC 里等

飞书 Router 阻塞表（`BachFlow/pcode/refs/09_router.md` 第 505–528 行）把两件事分开：

- 下游 Stream 不够、`stallWay=1`、这一包还没向原目标交过 flit，整包进 Core，原 VC 进入 Reissue 保序（F-025、F-027）。
- 下一跳 VC credit 不够，暂停并留下上下文，下一 flit 从暂停处继续，同一 VC 后面的包不越过它，别的就绪 VC 可以发。不触发 Reissue。

latch 原先只要这一拍资源不够（含 credit 为 0）且 `stall_way` 为真就 `DivertToCore`。改成 `StreamShortOnly`：只有某个网格出口的 Stream 不够、其余出口都拿得到资源时才整包改送进核。`BachRouterAsm.StallWayStoresWhenCreditIsGone` 改成：40 个 credit 用完后，剩下的包留在 VC 里等，进核数 0，溢流 0。9.21 又让 credit 用完的包头改送进核，这个测试改名为 `StallWayDivertsWhenCreditIsGone`。

同一轮还放宽了一条：一包包头已发、包身还在上游没到（`FlightPending` 为假）时，交叉开关不再挡这条 VC，`flight_` 也从 station 的 `hold_block` 里拿掉。这条放宽是为 p7「西口满、交叉开关空」加的，后来查明那次的真因是 9.15 的 `order_` 没清。这条放宽在 Step8 上造成两包交错（9.16），9.17 已经撤回。

Step7、128 用户重跑，日志 `/tmp/ep1_unit_logs/s07_u128_credit.log`。第 16384 拍起不动，指令 74662，在途 24，tail 45。p7 上用户 7 已经离开西口，停点移到更东边。

### 9.15 本口重发包的包尾从哪条路径发出都要清保序锁

停钟 dump 加了交叉开关各入口的锁（`lock` 行）。全局 chip 0（tray 0，片内 0，gx=0，gy=0）的 p9：西口 VC 0 有 40 个 flit，队头是用户 23 path 0 的包头；交叉开关空；西入口挂着 `ord0:u22p0`。往南的出口接全局 chip 4（tray 0，片内 4，gx=0，gy=1）的 p0，VC credit 20+20、Core Mem credit 128 KB 都是满的，stream 占用 0。`TryInput` 遇到 `order_` 就跳过这条 VC，用户 23 一直发不出去。

这把锁是用户 22 留下的。用户 22 到 p9 时往南 Stream 不够，整包改送进核，包尾置上 `order_ = {u22, p0}`。重发其实早已完成，用户 22 已经走到全局 chip 12（tray 0，片内 12，gx=0，gy=3）。漏洞在清锁：`ReleaseOrder` 只在 `TryInput` 普通授予末尾调用。本口重发的 25 个 flit 里只有包头走普通授予，其余 24 个连同包尾都从 `ContinueFlight` 发出，`ContinueFlight` 不清锁。chip 0 的 p4（`ord0:u25p0`）和全局 chip 1（tray 0，片内 1，gx=1，gy=0）的 p9（`ord0:u22p0`）是同一个原因。西边每一跳的东向 credit 都被这几口按住，B 核 tail 停在 45。

`xbar.h` 新加 `AfterGrant(in, v)`。改送进核、`ContinueOpen`、`ContinueFlight`、`TryInput` 普通授予、bypass、aside 六处授予之后都调它：记 `in_packet`，本口发出包尾时调 `ReleaseOrder`。

Step7、128 用户重跑，日志 `/tmp/ep1_unit_logs/s07_u128_release.log`。第 29881 拍走空，上限 32768，snaps 128，ShareMem head 128、tail 128，指令 415660。

| | 改之前 | 改之后 |
|---|---|---|
| 第 8192 拍 tail | 45 | 91 |
| 第 16384 拍 tail | 45 | 128 |
| 停钟 | 32768（上限） | 29881 |
| snaps | 0 | 128 |
| 全部指令 | 74662 | 415660 |

### 9.16 Step8：同一输出 VC 上两包交错

Step8 的单元原先是 10 月 11 日生成的，早于 harvest 对齐。按 Step7 的办法重新生成，导出到 `/tmp/latch_ep1_s08`：PCIe 入口 chip 0 p5 path 63，warnings 387，tasks 1612。

| 用户数 | 停钟 | snaps | head / tail | 在途 | 结论 |
|---|---|---|---|---|---|
| 16 | 11769 | 16 | 16 / 16 | 16 | 走空 |
| 32 | 32768（上限） | 0 | 32 / 32 | 248 | 卡死 |
| 128 | 32768（上限） | 0 | 128 / 81 | 366 | 卡死 |

32 和 128 用户卡在同一个环上，在全局 chip 5（tray 0，片内 5，gx=1，gy=1），全局 chip 6（tray 0，片内 6，gx=2，gy=1）的状态逐字相同。chip 5 是 h23，p2、p7 只透传。dump 加了逐 VC 的队列内容（`vcq` 行），日志 `/tmp/ep1_unit_logs/s08_u32_vcq.log`。

1. p9 的核口被用户 13 path 1 占着：包头已经进核，包尾还没到。核口按 F-013 一包没完不让别的包插进来，p9 西口用户 20 path 0 的包头、VC 3 上用户 12 path 2 的归约结果都进不了核。
2. 用户 13 path 1 的包尾在 p3 西口 VC 0，排在用户 22 广播后面：`u22p0 ×8 → u13p1 ×1（包尾） → u23p0 ×26 → u14p1 ×5`。p3 西入口的 `flight_` 已经改记成用户 22，没人再把这个包尾抽出来先发。
3. 用户 22 在 p3 要往南、往东、进核一起走，往南 credit 为 0。
4. p8 mid 入口 VC 0 满 40（用户 21、22），用户 21 往东到 p9 的 credit 为 0。
5. p9 西口被 VC 3 上用户 13、14 的 path 2 归约输入吃满共享窗口。用户 13 path 2 进不了归约：任务表里还有没做完的 path 1 归约，按 F-030 先拒绝 path 2（`w1`）。这又回到第 1 步。

交错产生在 chip 5 的 p0。p1 西口 VC 0 的顺序是 `u16p1 ×3 → u25p0 ×26（整包） → u16p1 ×1（包尾）`。path 1 从全局 chip 1 的 p9 来，是那里的归约结果，在 VC 3 上；path 0 在 VC 0 上；两者往东都用输出 VC 0，进的是同一个 `in_q[西][0]`。用户 16 path 1 发出包头和两个包身后，后面的 flit 还在上游，`FlightPending` 为假。按 9.14 的放宽，交叉开关不挡这条 VC，station 也不挡，用户 25 的包头就插了进来。`vc_owner` 只记入口号，同一入口来的另一包照样过 `OwnerOk`。

飞书第 505–506 行和 F-012：同一条 VC 上一包发出包头后发到包尾，后面的包不能越过它；被挡住时可以换别的就绪 VC。9.14 的放宽违反了这一条。

### 9.17 恢复同一输出 VC 的整包独占，并修 TS 重复申请 credit

**交叉开关和 station。** 撤掉 `TryInput` 里的 `FlightPending` 条件，`flight_` 那条 VC 重新放回 `hold_block`，`PullOpen` / `ContinueFlight` 抽包身先发照旧。改了三版：

1. 第一版把有包没发完的 VC 整条放进 `hold_block`，连这一包自己的包身也挡住。station 每个入口一拍只有一个 `hold_pull`，同一入口两条 VC 各开一包时，另一包的包身永远轮不到。全局 chip 4（tray 0，片内 4，gx=0，gy=1）p4 西口 VC 3 上用户 30 path 2 的最后几个 flit 就这样卡住。16 用户 11833、32 用户 14521 走空，128 用户卡在 tail 71。
2. 第二版改成 `hold_block` 只挡包头。交叉开关的 `flight_` 要晚一拍才发布到 station，这一拍里 station 又把另一包的包头插进同一条 VC，包身跟着把队列填到水位，两包互等。16 用户在全局 chip 19（tray 1，片内 3，gx=3，gy=4）p0 卡死。
3. 第三版（保留）在 station 本地按交叉开关 VC 记 `open_ = {user, path}`：发出无包尾的包头时置上，包尾清掉。这条 VC 开着包时只发同一 user、同一 path 的包身，别的包头不发。`IssueRequest` 和 `SendAt` 两处发射都更新它。16 用户 11769、32 用户 14457 走空。128 用户在第 11918 拍断言。

**128 用户的断言。** 全局 chip 30（tray 1，片内 14，gx=2，gy=7）p9 的 UserMatch：用户 58 path 0 又来一个包头，reissue 0，done 0x3，这个用户已经没有没做的搬入任务。往上游，全局 chip 10（tray 0，片内 10，gx=2，gy=2）、全局 chip 26（tray 1，片内 10，gx=2，gy=6）一直到 chip 30 的 p0、p3、p8、p9，每一核都收到两份。源头是全局 chip 6（tray 0，片内 6，gx=2，gy=1）p9：它把用户 58 path 0 整包改送进核（第 9437–9462 拍），之后本口往南把这一包发了两次（第 9544–9569 拍和第 9601–9626 拍）。别的改送进核的核都只重发一次。

chip 6 p9 上 TS 的时间线（stream 9，t1 是 `broadcast_reissue_t1`）：

| 拍 | 事件 |
|---|---|
| 9486 | t1 装进 stream 9，WAIT |
| 9487 | TS 向 Router 申请 credit，Router 排进监听队列 |
| 9503 | Router 回 grant，唤醒把 t1 置 READY。这一拍 TS 读的快照里 t1 还是 WAIT，又申请了一次 |
| 9504 | Router 只按「上一笔收下的 (user, task)」去重，上一笔是用户 46 的 task 7，于是又排了一笔；DTE 第一次锁定 t1 |
| 9505 | DTE 收到 ACCEPT，t1 改 INFLY；Router 回第二个 grant |
| 9506 | 第二个 grant 的唤醒把 t1 从 INFLY 写回 READY |
| 9507 | DTE 第二次锁定 t1，同一个重发任务下发两次 |

唤醒写落表一拍、进快照一拍，这两拍里快照上任务仍是 WAIT，TS 会对同一个任务再申请。16、32 用户时申请稀疏，两次申请之间没有别的用户插进来，Router 的去重挡得住。第二个 grant 在 Router 侧还会多占一份资源。这个竞争和 VC 独占无关，是 128 用户的流量时序把它暴露出来的。

`ts/credit_monitor.h` 的 `DoAsk` 改成每个 stream 记下已经申请过的 task（`asked` / `asked_task`）。快照里那一项离开 WAIT、换了 task 或者失效，才放开标记。这个检查每拍都做，请求还在等 Router 收下的那几拍也做。

**结果。** `PCODE_CHECK=idle`，上限 32768，bundle `/tmp/latch_ep1_s08`。

| 用户数 | 停钟 | snaps | head / tail | 指令 | 日志 |
|---|---|---|---|---|---|
| 16 | 11769 | 16 | 16 / 16 | 60591 | `s08_u16_ask.log` |
| 32 | 14457 | 32 | 32 / 32 | 119338 | `s08_u32_ask.log` |
| 128 | 30329 | 128 | 128 / 128 | 470647 | `s08_u128_ask.log` |

三档都走空。128 用户停在 30329 拍，离上限只差约 2400 拍。

单元测试：`router_assembly` 14 个、`router_core_station`、`router_reduce`、`router_scenarios`、`dte`、`dte_inbound`、`ts`、`ts_config`、`ts_issue`、`ts_done`、`ts_chain`、`ts_credit` 全过。`bcore` 的 `BachBcore.TokensGoOutInArrivalOrder` 失败，撤掉这些改动后同样失败，与本节无关。

这几次都在 `PCODE_CHECK=idle` 下跑，fc1 / concat 余弦没有算。Step7 的 16、32 用户在 9.15 之后没有重跑，Step7+128 在 9.17 之后也没有重跑。

### 9.18 Step9：16、32、128 用户都走空

代码与 9.17 相同。Step9 单元重新生成，导出到 `/tmp/latch_ep1_s09`：PCIe 入口 chip 0 p5 path 63，warnings 387，tasks 1740。`PCODE_CHECK=idle`，上限 32768。

| 用户数 | 停钟 | snaps | head / tail | 指令 | 在途 |
|---|---|---|---|---|---|
| 16 | 11961 | 16 | 16 / 16 | 66780 | 16 |
| 32 | 14585 | 32 | 32 / 32 | 131662 | 16 |
| 128 | 30457 | 128 | 128 / 128 | 519451 | 16 |

在途 16 是 B 核 stream 表走空后的残留。128 用户离上限只差约 2300 拍。日志 `/tmp/ep1_unit_logs/s09_u{16,32,128}.log`。

### 9.19 Step10：path 7 的汇点从同一面收到两包

Step10 加进 FC2 归约（path 4–11）。单元重新生成，导出到 `/tmp/latch_ep1_s10`：warnings 387，tasks 3316。代码与 9.17 相同。

| 用户数 | 停钟 | 在途 | head / tail | 说明 |
|---|---|---|---|---|
| 16 | 32768（上限） | 413 | 16 / 16 | 第 16384 拍起在途不变，指令只有空转的慢涨 |
| 32 | 32768（上限） | 971 | 32 / 32 | 同上 |
| 128 | 32768（上限） | 2293 | 128 / 111 | 第 16384 拍起完全不变 |

三档卡在同一处。FC2 归约的 8 条 path 在全局 chip 35（tray 2，片内 3，gx=3，gy=8）上各在一颗核收齐：path 4 在 p0、5 在 p1、6 在 p2、7 在 p3、8 在 p5、9 在 p6、10 在 p7、11 在 p8。北邻全局 chip 31（tray 1，片内 15，gx=3，gy=7）和南面的 chip 39、43、47（tray 2，片内 7、11、15，gx=3，gy=9–11）都在这一列上。

**chip 35 p3 的现场。** 西口（来自 p2）VC 0 共 52 个 flit：

```
u1p7 ×4（包尾） → u3p4 → u2p7 → u2p7 → u3p7 → u1p11 → u3p7
```

交叉开关西入口的队头是用户 1 path 7 的包头，要进归约（mask 32），准入拒绝（`w1`）。每个用户的 path 7 都从西面来了两包。

p3 的任务链（h0）：t3/t4 是 path 4 转发，t5/t6 是 path 6 转发，t7 `reduce_t7` 是 path 7 本核归约，t8/t9 是 path 11 转发。用户 1 停在 t8，done `0xff`：t7 已经做完、结果已经发走，现在等 path 11。准入只收当前未完成的那条 path，path 7 的第二包被拒。用户 1 要的 `u1p11` 排在同一条 VC 后面，过不来。

**倒灌。**

- p2 的南口（来自 p7）40 个 flit、西口（来自 p1）46 个，队头都是用户 4 path 7，往东进 p3，东向 credit `0+0`。
- p1、p0 西口满，往东 credit 为 0。p5、p6、p7 同样堵住，p7 的 path 7 往北进 p2。
- chip 31 的 p2、p3、p8、p9 往南进 chip 35 p0，一路倒回去。
- chip 35 的 p0、p1、p5–p8 和 chip 47 各核的 Rmem 上下文停在 `got 0x5` / `0x6`，少的那一路分量在这片拥堵里。

32、128 用户时，全局 chip 23（tray 1，片内 7，gx=3，gy=5）的 p3 是同一个现场：用户 3 path 7 的第二包在队头 `w1`，后面还排着 `u4p7 ×2`。chip 35 p3 与 16 用户相同。

**为什么来两包。** chip 35 上 path 7 的表项：

| 核 | 表项 |
|---|---|
| p1 | transfer，向东，进 p2 西口 |
| p7 | transfer，向北，进 p2 南口 |
| p2（逻辑核 2） | transfer，向东，两包原样交给 p3 |
| p3 | reduce，`operation 3`，`rdc_in_mask 0x3` |

pcode 的设计（`route/DESIGN.md` 第 80、189 行）：没有本核数据的核不做 Router-reduce，收两包就原样交给下一跳的源核；源核按 reduce2 配，同一面的两包用 `reduce_sel` 分。latch 的 ReduceModule 按到达方向分路，同一面两包分不开。`latch_export.py` 的 `_fill_reduce_lanes` 把这类中间核改成中间相加，逻辑核 2 的 path 7 按要求保留原表（导出时 chip 23、chip 35 p2 path 7 两条警告）。p3 的 `rdc_in_mask` 因此只有本核加西面两位，`operation` 仍是 3：第一包到了就收齐、发出结果，第二包没有上下文可进，堵在队头。不堵的情况下结果也少加一项。

Step9 之前没有 path 4–11 的归约，碰不到。日志 `/tmp/ep1_unit_logs/s10_u{16,32,128}.log`。

**能走的两条改法（暂不改）：**

1. 导出侧：去掉逻辑核 2 path 7 的例外，chip 23、chip 35 的 p2 也改成中间相加（两路来自西和南，方向不同），p3 只收一路。改动小，偏离 pcode 原表，也违反第 189 行「没有本核数据的核不做 Router-reduce」。
2. latch 侧：ReduceModule 实现 `reduce_sel`，同一面同一 user、path 按两包分别计数，p3 按 reduce2 收西面两包。与 pcode 原表一致，要动归约分路、准入和 Rmem 的期望掩码。

### 9.20 改 BachFlow 路由：让 p3 的两路从不同面进来

提议（tray2/chip3 = 全局 chip 35，h0 无坏核）：

- 输入 1：tray1/chip15（chip 31）p8 → p9 → chip 35 p0 → p1 → p2 → p3，到 p3 西面。
- 输入 2：tray2/chip2（全局 chip 34，tray 2，片内 2，gx=2，gy=8）p4 → chip 35 p5 → p6 → p7 → p8 → p3，到 p3 南面。
- 输出：p3 → p4 → p9 → tray2/chip7（全局 chip 39）p0。

**能成立的部分。**

- p3 两路分别从西、南进，方向不同，不需要 `reduce_sel`，latch 的 ReduceModule 能模拟。p3 配 reduce2，`operation 3`、`rdc_in_mask 0x7`，与 chip 35 p0 的 path 4 同一种配法。p2 不再收两包，少一组 `reduce_reissue_t12/t13`。
- p8（逻辑核 7）和 p3（逻辑核 3）是对面邻居。`DESIGN.md` 第 197、244 行：片内搜索图按逻辑邻居建边，p8→p3 是合法直连边。
- 第 189 行写的就是先让源核自己汇合，无解才让上一跳收两包，提议正是源核汇合。
- 跳数不变：输入 2 和现在一样 5 跳，输出 3 跳。

**输出 p3 → p4 → p9 不成立。**

1. p4、p9 在 h0 是 unused 核。catalog 里 p4 是 `entry_count 0`、`release 0`，p9 是 `entry_count 0`、`release 2048`。第 39、187 行：透传核不配 RouterTable，按 release CSR 一进一出，只出现在预排路径上。第 244 行明确 `p0↔p5` / `p4↔p9` 不是预排 2-path，不能单独当搜索边。h0 下预排的只有 `p3↔p4`（E 口 attach）和 `p8↔p9`（S 口 attach），p4 从西面收进来只能往东。
2. p9 会成为合两路上游的透传核。p8 上 path 4、5、6、8、9、10、11 的出片仍走 p8 → p9 → S，再加 p4 → p9 → S，p9 就有西、北两个入口共用 S。正向能转，反向的 release 不行：p9 当前 `2048` 是「S 来的 release 弹回西面给 p8」，加北面后要同时弹回西和北，透传核分不清这笔 credit 属于哪个上游。第 234 行「同一透传核上两条不相干的弯路不能交叉」禁止的就是这个。

**几种绕法也不行。**

- 输出改回 p3 → p8 → p9：p8 的 path 7 要同时把西面来的送北、北面来的送东。RouterTable 每个 PathID 一项，归约结果沿用同一个 PathID，配不出来。
- 输入 2 从东面进 p3（p8 → p9 → p4 → p3）：p9 西面要同时往北（输入 2）和往南（其他 path 输出）。第 189 行规定一个入口面对所有 PathID 只有一种角色，透传也不多播。

gx=3 这一列东边没有邻片，p4、p9 只剩南口这一个出路，几种绕法都会撞到 p8 或 p9。

**其他要考虑的。**

- 只对 h0 成立。边列芯片有 11 个 harvest 掩码，逻辑核映射随掩码变。p8 坏时 p8→p3 不存在；p4、p9 被映射成计算核时要配表、角色也变。要作为规则交给 `harvest_search` 每个掩码重搜，不能写死物理路径。tray1/chip7（全局 chip 23）是同一个形状。
- p8 西口原本走 path 5、8、9、10、11（往东或本核归约），加上往北的 path 7。p3 南口因准入的 path 过滤暂时不收时（这个用户的 path 4、6 还没过 p3），path 7 会在 p8 西口队头压住后面用户的 path 8–11，包括 p8 自己 path 11 归约的输入。推算下游是往南的无环链，只会变慢、不成环，没有仿真验证。

**可选做法。**

- 在 latch 实现 `reduce_sel`（9.19 的第 2 条），按 pcode 原表，路由不动。
- 沿这个思路改规划器：允许 unused 核做 `p4↔p9` 纵向透传，并且 release CSR 能在同一透传核上区分两路上游。两条都要硬件确认，单靠规划器做不到。
- 导出时把 p2 改成中间相加（9.19 的第 1 条），latch 能跑通，但违反第 189 行，只是模型侧的近似。

### 9.21 改送按 (出口, VC) 计数，下游 credit 用完也改送

全图 128 用户、不限在途。改送规则换成硬件提的计数方案后，只完成 33 个用户，第 15364 拍起 MU 计数不变。卡点在全局 chip 43 的 VC 0：`u347p0` 在 p0 分三路（本核、往 p5、往 p1），往 p5 的分支包头已经锁住 p5–p8 和 chip 47 的 VC 0，包身还在 p0；往 p1 的分支没有 credit，多播 flit 要每个分支都有 credit 才发，两个分支一起停。p1、p2、p3 的 left 入口 VC 0 各满 40 个 flit，p3 队头要进 p8；p8 的 mid 入口队头要用的出口正被 `u347p0` 的往 p5 分支锁着，这个分支又在等 p0 送后面的 flit，成环。环上只有 VC credit 和包头到包尾的锁，没有包在等重发，Stream 都够。改送规则不在环里，原规则能跑完是时序上没撞到。

**计数。** `xbar.h` 的 `reissue_wait_` 只当成每个 (出口, VC) 的计数用：改送进核时 +1，那一包的重发包头从本核发出时 -1。latch 用 (user, path) 认出本核发出的是不是那一包重发，代替硬件包头上的重发标记，所以换成 `multimap`。`WaitAhead`：

- 网格进来的包（`from_mesh`），计数不为 0 就改送进核排在后面，占没占下游一项都一样。
- 本核发出的包（`from_local`）不在这里挡。重发包本身不能挡；普通发送由 TS 按用户年龄授权：`CreditMonitor::SetReissueBusy` 查 Router 的 `ReissueBusyFor`，这几个出口上有计数时，年长的一笔没授权，年轻的不能先拿。
- reduce 结果不能改送，已占下游一项的照走，没占的等。
- 只在本核发出的包头经过时减计数（`Consume` 查 `from_local`）。

**credit 用完也改送。** `StreamShortOnly` 加 `MeshCreditShort`：某个网格出口这条 VC 私有、共享 credit 都是 0，表项 `stallWay=1`，其余出口都拿得到资源时，包头整包进核，也记一次计数。这个出口正被别的包锁着时只等它发完，不改送。9.14 按飞书阻塞表把触发收窄到只有 Stream，这里又放宽了：环上每一步都是在等下游 VC 0 的 credit，包头在 credit 用完时能进核，环上就有一处能把整包让出去。这一条飞书 Router 文档没有写，要硬件确认。

`BachRouterAsm.StallWayStoresWhenCreditIsGone` 改名 `StallWayDivertsWhenCreditIsGone`，对 `stallWay` 0 和 1 各跑一遍：40 个 credit 用完后，`stallWay=1` 剩下的 10 笔整包进核，`stallWay=0` 留在 VC 里等，溢流都是 0。router 和 TS 的 154 个测试通过。

全图 128 用户不限在途，BachFlow bundle 为 `a4fc8a6` / `9cd3a29` 之后的构建：128/128 完成，最后一个在第 35133 拍，用户 101、202 的 cosine 是 0.99972、0.99974。改送 17842 次，credit 引起的 26 次，其余是 Stream 不够。egress 按用户顺序输出，相邻颠倒 0 处。

### 9.22 归约结果按表项 nxtVC 发出，VC 由 BachFlow 分配

**latch 改动。**

- `reduce_module.h`：Rmem 发归约结果用本核表项的 nxtVC（飞书 Router F-039，所有出方向共用一个 VC），取 mid、left、right 中第一个置位出口的那一项；只送本核时仍用 `kReduceVc`。原来一律写死 `kReduceVc = 3`。
- 9.21 的 credit 用完改送留作保底，默认开。`BACHFLOW_CREDIT_DIVERT=0` 关掉：`Xbar::SetCreditDivert`，bundle 测试读这个环境变量设到每个核，写进 transport.txt 的 `credit_divert` 行，`run-exact-native.sh` 校验并透传。latch 不区分 BachFlow 用哪种 VC 方案，只按表项走。

**旧 bundle 不能再用。** 旧规划器给 400 条 reduce 表项都写 nxtVC=0，transfer 也大多在 VC 0（1387 条里 1223 条）。latch 写死 VC 3 时，reduce 自成一条通道，才跑得通。改按表项之后，16 用户只完成 6 个，第 13312 拍起停住，改送开关都一样。

**BachFlow 的两种 VC 分配。**

1. 默认：reduce 表项独占 VC 3，其余表项在 VC 0–2 上按 net 序轮转（`router_table.py` 的 `REDUCE_VC`、`DATA_VCS`），三个 VC 都被路过 hop 占了才升 reissue。reduce 结果不能改送进核，和 transfer 共用 VC 会互等成环。先试过让 reduce 也参加 4 个 VC 的轮转，16 用户一个都没完成，chip 31 的 path 7 归约上下文占满。
2. `BACHFLOW_VC_ACYCLIC=1`：每个 net 一个 VC，按等待图（`vc_cdg.py`：通道是 (核, 面, VC)，多播各分支同进同退，进核分支也算）挑负载最轻、加进去不成环的 VC。

**静态等待图。** 方案 2 各种建模都无环。方案 1 不算进核分支时无环；算上进核分支（各 VC 共用一个进核口）时有一个 702 个通道的环：net 1 的多播（VC 0）经 chip 43、47、35、39 各核的进核口和别的 net 耦合，与 9.21 chip 43 是同一个形状，不经过 reduce。

**仿真。** 全图不限在途，表中是最后一个用户完成的拍数。所有用户的数值都通过，用户 101、202 的 cosine 是 0.99972、0.99974。

| VC 分配 | credit 改送 | 16 | 32 | 64 | 128 |
|---|---|---|---|---|---|
| 9.21：旧 bundle，latch 写死 reduce VC 3 | 开 | 9906 | 14000 | 21164 | 35133 |
| 方案 1：reduce 独占 VC 3 | 开 | 10134 | 13330 | 20107 | 33608 |
| 方案 1 | 关 | 9995 | | 20018 | |
| 方案 2：等待图无环 | 关 | 10020 | 13288 | 19938 | 33462 |

方案 1 关掉改送，16、64 用户也跑完，静态图上那个经进核口的环这两组没有撞上；不能由此去掉保底，默认仍开。

`BACHFLOW_CREDIT_DIVERT=0` 与编译期关掉改送逐拍一致（方案 2，16 用户都是 10020）。nxtVC 按跳分配暂不做，记在对接文档 F18。
