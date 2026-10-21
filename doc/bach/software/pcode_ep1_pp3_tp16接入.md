# EP1+PP3+TP16 接入 latch：要做的事、与参考文档的差异、灌入路径

对照三份材料：

- 当前要改的模拟器：`/mnt/c/Users/kim01/Desktop/latch`（只认 `BACHIR 10`，没有 pcode bundle 装载）
- pcode 与飞书参考文档快照：`/mnt/c/Users/kim01/Desktop/BachFlow/pcode`（`refs/` 是 2026-10-05 从 [飞书 wiki](https://pcng0ddyhlxs.feishu.cn/wiki/LRYTwsnWGihPtckq8mHc1r6Rnub) 导出的 Markdown；在线页需要登录，本文以这份快照为准）
- 已跑通过一版的参考实现：`/mnt/c/Users/kim01/Desktop/latch_20261009`（`doc/bach/software/pcode对接与模拟器修订.md`、`test/bach/ip/pcode_bundle.cpp`）。只作对照，不要求当前仓库与它对齐

pcode 侧的导出规则和当时的调试记录在 BachFlow `pcode/LATCH_INTEGRATION.md`。那份记录里“仓库 catalog 精度全 0”已经过时：现在的 `pcode/output/ep1_pp3_tp16/route/route.resolved.json` 是 `release_spec.width = 20`，8672 条 reduce 表项的 `reduceInPrecision` 都非 0（抽查为 `16`，即本核 FP32、mesh 方向 BF16，`reduceOutPrecision = 0` 即出片 BF16）。

---

## 1. 要把这个用例接到模拟器上，需要做哪些事

目标分两档。时序观察只要求 token 在 48 chip 上走完广播、GEMV、归约、concat，能在波形或停钟统计里看到各核的 TS / RV / DSA 段。功能观察还要求 Matrix Mem 里有合法权重、topK 是合法专家号，出口数据能和参考值对上。当前 pcode 产物和 `latch_20261009` 的用例只覆盖第一档。

### 1.1 pcode 侧（已经有，导出时用）

产物在 `BachFlow/pcode/output/ep1_pp3_tp16/`：每核 `regs.t*.json`、`kernel.*.{dte,mu,vu}.cpp`、`kernel_common.h`、`task_meta.json`、`route/route.resolved.json`。

导出器 `python3 -m bachflow_pcode.latch_export` 把它们收成 latch 能读的目录：一份 `BACHIR 11` 的 `.bachir`，加上按源码去重的 RV32 Intel HEX。EP1 单 token 的包长是 topK 56 B + data 6336 B，从 chip 0 的 W 口、path 63 进 B core（物理核 p5）。

导出器已经做的适配（不必在模拟器里再做一遍）：

- 按 latch 坏核掩码选 harvest：边列全好，中间两列 `0x084`（core2、core7）
- 方向：pcode N/S/W/E/Core 换成 latch 的 mid / left / right；角核 p0 的北走 left，p9 的南走 right
- R core 链拆开：链上第 0 项 wait_wake 搬入改记 `DATAIN`，`SELF_START` 保持 1，Task 0 从 `check_flag` 起
- 首个 B core 补 path 63 落核项；透传核按 release 补只转发的 `RTAB`（latch Router 没有“不查表直通”）
- `transfer_mode` 按流向写 0..4，不再全写 1
- kernel 编成 ihex 时把 `.bss` 以零字节装进镜像

### 1.2 当前 latch 侧（还没做）

`src/bach/ip/bundle_load.h` 只认 `BACHIR 10`，不认识 `KERNELCORE` / `DSAREG` / `SMEM`。pcode 的 kernel 用文档绝对地址和 `csrr 0xFC0` 这一组 CSR，当前模型译不了。不改这些，bundle 装不进去，装进去也跑不对。

建议按这个顺序做。前四项不完成，激励进核之后地址和身份都是错的；后面几项是 `latch_20261009` 上 EP1 单 token 能在约 9593 拍空闲停钟之前撞过的行为差，可以等第一条 token 跑起来再按现场补。

| 顺序 | 改哪里 | 做什么 |
|---|---|---|
| 1 | `bundle_load.h` | 同时认 `BACHIR 10` 和 `11`。11 增加每核镜像、DSA 初值、Share Mem 初值。`BACHIR 11` 时归约 payload 前缀改为 0 字节（10 仍是 16） |
| 2 | DTE `Agcu::Decode` | 文档绝对地址与现有高 4 bit tag 两套都认，展开 stride 之后再译成 tag |
| 3 | RV 地址窗 | `0x4830_0000` 作 Share Mem 别名，`0x4840_0000` 作 Core Mem 别名；Core Mem 三个 RV 都可见（MU 上的 `check_flag` 要读包头） |
| 4 | RV CSR | `csrr/csrw` 的 `0xFC0/0xFC1/0x7C0/0x7C1/0x7C2` 译到现有 task-ctrl MMIO，其余 CSR 仍走功能模型 |
| 5 | MU | `primitive_mode` 改成参考文档 v0.2 的位序；`0x1000` 起 4 套模版，`TASK_TRIGGER[2:1]` 选模版再和动态区合并 |
| 6 | 用例 | 仿 `latch_20261009/test/bach/ip/pcode_bundle.cpp`：装 bundle、第 2 拍从 chip 0 W 口注入一包、空闲停钟、打印每核指令数与在途 stream。`test/bach/CMakeLists.txt` 按 `*.cpp` 自动挂目标，放进 `test/bach/ip/` 即可 |
| 7 | 行为差 | 段 0 只作 32 B 包头；出核跨 Core Mem / Matrix Mem 时按发出顺序配对响应；归约按用户与 lane 配对，不用发送方 task_id；MU 越界走 Drain 并仍报 `dsa_done`；进核 reduce datain 不占 Rmem；`stall_way` 的多 flit 包整包留在 VC；纯转发的 type-4 不等 Reduce Done；task 0 的 PC 为 0 时不建链。细节见参考仓库那份修订记录 §1–§15 |

时序怎么看：模型里已有 `.trace`，`src/utils/tracetto/` 按 chip → core 画 `TS-DTE/MU/VU`、`X-Core`、`X-DSA`。用例侧再保留停钟拍数、每核 RV 指令数、TS `InFlight`、Router 转发 flit，用来判断是排空了还是停在半路。

功能怎么看：这一档现成路径做不到。pcode 只分配 Matrix Mem 地址，不生成权重字节；用例里的 topK 是合成字节，不是专家号。GEMV 会按非法专家号走 Drain，归约结果没有参考值，片外出口是 0 包（`pcie_sw_data_out` 的 Router→PCIe 段 pcode 明确不模拟）。要对数值，需要另做第 8 项。

| 顺序 | 改哪里 | 做什么 |
|---|---|---|
| 8 | 激励与存储初值 | 按场景形状生成合法 topK（组内专家号）和 mxfp8 权重，在开钟前写入各核 Matrix Mem 的 `b_addr`；出核若仍不建模，功能检查停在 concat 目的核的 Core Mem，不指望片外收到包 |

`latch_20261009` 上 EP1 单 token 的现场是：第 9593 拍空闲停钟，计算核 stream 没有停在等待或在途，chip 0 p5 自启动占满 16 条 stream，片外 0 包。那是时序排空，不是功能对拍。

---

## 2. 激励、寄存器、kernel 各自怎么进 latch

三条路径分开。pcode 生成的是配置和程序，不是 token 字节，也不是权重字节。

```
场景 glm5_ep1_pp3_tp16.py
        │  两遍：先登记路由，再重放填寄存器 / 拼 kernel
        ▼
regs.*.json + kernel.*.cpp + route.resolved.json + task_meta.json
        │  latch_export.py（RISC-V 工具链编 ihex）
        ▼
<bundle>/pcode_ep1.bachir + k_*.hex
        │  LoadBundle：开钟前写入各 IP
        ▼
Router / TS / DSA 模版 / Share Mem / 每核 ITCM+DTCM
        │  开钟后，用例在第 2 拍从 chip0 W 口注入 1 个 Message
        ▼
B core datain kernel 用文档绝对地址把 topK、data 搬进 Core Mem / Matrix Mem
```

### 2.1 激励数据从哪来

不是从 `regs.*.json` 来的。JSON 里只有地址和任务链，没有 token 正文。

`latch_20261009` 的 `MakePcodeToken()` 在测试里现造一包：

| 字段 | EP1 取值 | 作用 |
|---|---|---|
| `path_id` | 63 | 与导出器 `--pcie-path` 一致。导出器给 chip 0 的 B core（p5）补一条 path 63 落核表项，因为 pcode 路由表不管片外入口 |
| `user_id` | 1 | 进 stream 表 |
| `path_core_mask` | `0x3F` | 路由表开了 `use_core_mask` 的落点看包头这一字段。B core 把它写进包头再转发出去；为 0 时下游会 Drop |
| payload | 56 B topK + 6336 B data | 字节是 `(i * 7 + 3) & 0xFF`，没有业务含义 |
| 注入时刻 | 第 2 拍，chip 0 的 W 口，单 flit（head=tail） | 进 B core 的 DTE datain |

56 和 6336 来自场景形状，不是来自文件：

- topK 逻辑长度 `2 + 8 * (2 + 4) = 50` 字节（`ep_counter` + 8 个专家的 index/weight），pcode 按 8 B 对齐收成 56
- data 是 `dim + dim/32 = 6144 + 192 = 6336`（token + mxfp8 scale）

这和 latch 自己的 MoE 用例不是同一条路。`test/bach/ip/chip/moe_common.h` 的 `MakeToken` 用 `kn::TokenData` / `TokenTopk` 造合法 token，权重走另一条 path，由 DTE weights loader 搬进 Matrix Mem，最后和参考向量逐字节比。pcode 这条路没有 weights loader，也没有参考向量。

权重在 pcode 里只做地址分配。`tensor.py` 对 `mem_kind data=MatrixMem` 从 `Addr.MATRIXMEM (0x48C0_0000)` 往上加计数器，`gemv` 把该地址写进 MU 的 `B_ADDR`。Matrix Mem 本体保持复位值。合成 topK 被当成专家号之后，读地址越界，参考实现里走 Drain，不算一次成功的 GEMV。

Share Mem 的 head/tail 初值来自寄存器 JSON 里的非零字，经 `SMEM` 记录在开钟前写入。token 到来之后，B core kernel 再写 head，计算核 `check_flag` 轮询这块内存。

### 2.2 初始化寄存器怎么灌进去

真机上是 SCP 经 ctrl_noc 在 boot 期写。模型不过 ctrl_noc，`LoadBundle` 直接调各 IP 的配置口，顺序不变：先每颗 chip 的 `core_bad_mask`，再 Router 表与 release，好核再写 kernel、DSA、Share Mem、TS，最后 `TS_INIT_FINISH`。自启动核在这一刻建满 stream 表项。

`latch_export.py` 读 `regs.t{t}.c{c}.h{h}.p{p}.json` 的稀疏字典（键是文档绝对地址），按段拆开：

| JSON 地址 | bundle 记录 | 参考实现里的写入 |
|---|---|---|
| TS `0x4800_8000 + 0x000..0x1FF` | `TCHAIN`。PC 是函数名，导出时用 `nm` 换成 ITCM 地址，并拆成 unit / recv / path / end 等位 | `Ts::Cfg().WriteTask` |
| TS `+0x200/+0x204` | `DATAIN` | `WriteDatainTask` |
| TS `+0x208` | `CFGMISC` 的 `stream_num` | `SetStreamNum` |
| TS `+0x214` | `CFGMISC` 的 `self_start` | `SetSelfStart` |
| TS `+0x20C` `TS_INIT_FINISH`、`+0x424` 一带的 `RTR_STREAM_INIT` | 不导出 | 装载器自己写 `InitFinish`。pcode 把 `RTR_STREAM_INIT` 放在 TS+0x424，与 TS 寄存器不是同一个东西，导出器丢掉 |
| DTE DSA `0x4802_8000` 模版区（`0x1000` 起，到 header table `0x3000` 之前） | `DSAREG chip core 0 off val` | `Dte::Regfile().CfgWrite` |
| MU DSA `0x4803_0000` 模版区（`0x1000` 起，到 topK table `0x3000` 之前） | `DSAREG ... 1 ...` | `Mu::Regfile().CfgWrite` |
| VU DSA `0x4803_8000` 起 | `DSAREG ... 2 ...` | `Vu::Preload` |
| Share Mem `0x4830_0000` 起、值非 0 的字 | `SMEM chip core off val` | `PokeSmemWord`。复位是 0，只装非零字 |
| Router 表 | 不从 JSON 的 `RTR_PATH_*` 读，从 `route.resolved.json` 换算成 `RTAB` / `TSRTAB` / `RELROUTE` | `Router::Preload`、`SetCreditBypass`、`Ts::Cfg().WriteRouterTable`、DTE `PreloadRtab` |

运行期 kernel 再用 `dsawi` 写动态区（模版差集、本笔 stream 的地址）。所以 boot 灌进去的是模版和静态表，不是每一笔任务的最终配置。

当前 latch 的 `LoadBundle` 没有 `DSAREG` / `SMEM` 这两步。DTE 模版区它已经认（`kDteTemplateBase = 0x1000`），但没有入口把 pcode 的字写进去。MU 的 `WriteReg` 对 `0x1000` 落到 `default` 后直接丢掉。

### 2.3 C++ kernel 怎么灌进去

pcode 每个物理核、每个 harvest、每个单元各一份 `kernel.t*.{dte,mu,vu}.cpp`，函数带 `__KERNEL__`。`kernel_common.h` 把 `dsawi` / `task_done` 编成 custom-0（opcode `0x0B`），把身份读编成 `csrr`。

导出器对每份源码：

1. 加上 `start.s` 和 `link.ld`。ITCM 4 KB 从 0，DTCM 8 KB 从 `0x20000`，与 `rv_ports.h` 一致。
2. `riscv64-unknown-elf-g++ -march=rv32im_zicsr -mabi=ilp32 -Os -nostdlib -ffreestanding` 编 ELF。
3. `objcopy` 出 ihex，并强制 `.bss` 可加载。模拟器里的 RV 不跑 `_start`，没人清 bss；DTCM 复位是 `0x55`。stream 计数器这类 static 不进镜像就会用垃圾初值，DTE 地址会算飞。
4. `nm` 取出函数入口，填进 `TCHAIN` / `DATAIN` 的 PC。
5. 源码 SHA1 相同的核共用一份 hex。没有 kernel 的 RV 装一份空转镜像。

bundle 里是 `KERNELCORE <chip> <core> <rv 0/1/2> <file>`。参考实现的装载是：该核该 RV 有自己的文件就用它，否则退回 `BACHIR 10` 那种全局三份 `KERNEL`。写入是 `Rv(i).LoadImage`，进 ITCM 和 DTCM。

`start.s` 只设 `sp` / `gp`，执行一条 `task_done`，然后原地跳。TS 之后按任务链里的 PC 跳进具体的 `__KERNEL__`。所以镜像里的 `_start` 不是业务入口。

当前 latch 只有全局 `KERNEL <rv> <file>`：同一份 DTE/MU/VU 镜像铺到所有好核。pcode 每个核的任务不同，不能共用这一份。

---

## 3. 当前 latch 与参考文档的差异

“参考文档”指 `BachFlow/pcode/refs/` 那套飞书导出，外加主文档在 `PLAN.md` 里已经记过的矛盾取舍。下表只列会影响 EP1 接入的项。同一行若 `latch_20261009` 已改，写在最后一列，当前仓库还没有这些改动。

已经对齐、不用再改的：

- custom-0 编码。当前 `custom0.h` 与 `kernel_common.h` 都是 opcode `0x0B`，`task_done` 的 funct3 为 2。
- DTE `CFG_TRANS_MODE` 的模版示例位序。当前模型和 `bach.h` 都是 `transfer_mode[2:0]`、`seg_valid[5:3]`（段 0 恒使能）、`wr_sharemem_flag[8]`、`ack_ts_en[9]`。文档另有一版 `transfer_mode[9:7]`，与模版示例十六进制矛盾，pcode 与 latch 都没采用。
- TS 任务链 ATTR 位序、64 槽、`STREAM_NUM` / `SELF_START` / `DATAIN` 的偏移。
- DTE 模版区 8 套 × 128 B，`CFG_TRIGGER[3:1]` 选模版。
- 拓扑可以一一对应：pcode `chip_global = t*16+c`，latch `g = gy*4+gx`，`gx = c%4`，`gy = t*4+c//4`。

还没对齐的：

| # | 参考文档 / pcode | 当前 latch | 接入时会怎样 | `latch_20261009` |
|---|---|---|---|---|
| 1 | 每核一份 kernel，boot 写每核 ITCM | 全局三份 `KERNEL`，所有好核共用 | 计算核跑到别人的任务，或 PC 为 0 | 已加 `KERNELCORE` |
| 2 | DSA 模版、Share Mem 初值在 boot 写入 | 无 `DSAREG` / `SMEM` 记录。MU `0x1000` 写入被丢掉 | 模版触发读到空配置；head/tail 初值为 0 或 `0x55` | 已加，装载时 `CfgWrite` / `PokeSmemWord` |
| 3 | 地址是全局绝对地址：Share `0x4830_0000`、Core `0x4840_0000`、Core SF `0x4850_0000`、Matrix `0x48C0_0000`、Matrix SF `0x4AC0_0000`、header table `DTE_DSA+0x3000`、topK `MU_DSA+0x3000` | DTE 用高 4 bit tag（0 Cmem、1 Mmem、2 scale、3 topK、4 header）。RV 的 Share / Core 窗口是 `0x40000` / `0x80000`，且 Core 只给 DTE RV | kernel 里的 `dsawi(0x48400000)`、`*(0x48300000)` 落在窗口外 | 绝对地址与 tag 两套都认；三个 RV 都能访问 Core Mem 别名 |
| 4 | RV 用 `csrr 0xFC0/0xFC1/0x7C0/0x7C1/0x7C2` 读 stream / task / user / path / vc | 身份只在 task-ctrl MMIO `0x30000`。功能模型的 CSR 文件读不到这些号 | `check_flag`、datain 里的用户号和 stream 号是垃圾 | 这五条 CSR 译到 task-ctrl 对应偏移 |
| 5 | MU `primitive_mode`（主文档公式，29 bit 里实际用到 25）：`a[1]`、`c[2]`、`share_dt[4:3]`、`router_dt[6:5]`、`share_en_mask[14:7]`、`n_experts[22:15]`、`reduce_en[23]`、`task_last[24]` | 模型是旧位序：`a[2:1]`、`c[3]`、`share_dt[6:4]`、`router_dt[9:7]`、`share_ep_count[13:10]`、`expert_count[21:14]`、`share_ep_en[22]`。编译器头 `bach.h` 又是第三套（没有 share 域，`router_dt` 从 bit 7 起） | 同一份 `primitive_mode` 字会被解成另一种精度和专家数 | 已改成文档位序。当前仓库内部两套位序也要收成一套，否则原有 MoE kernel 和 pcode 不能同时对 |
| 6 | MU 模版 `TEMPLATE_BASE 0x1000`，4 套，`TASK_TRIGGER[2:1]` 选模版 | 无模版存储，触发只看 bit0 | 模版任务等于没配 | 已加 4 套 × 128 B，触发时与动态区合并 |
| 7 | `hw_header_op` 在 `CFG_TRANS_MODE[7]`：源端为 Router 时 0 保存包头、1 丢弃 | 位序注释里有这一位，模型不消费 | 进核包头镜像可能不按 kernel 的意图落 Cmem | 已解码，段 0 另写 32 B 镜像，不进 payload |
| 8 | pcode 数据段是纯张量。32 B 包头在独立 header 段，payload 前没有 16 B 软件辅助信息 | `ReduceModule` 固定跳过 payload 前 `kReduceSwHeaderBytes`（16） | EP1 源端 4608 B FP32 收成 BF16 会多出 8 B，目的端段长 2304 对不上，进核断言 | `BACHIR 11` 把前缀改成 0，`BACHIR 10` 保持 16，避免拆掉现有 MoE 用例 |
| 9 | Share Mem 容量在地址编址里写 128 KB（`addr.py` 同样标 128 KB） | 模型窗口 32 KB | `0x4830_1000` 的 head 还在窗口内；超出 32 KB 的字会越界。EP1 的 head/tail 用的是低地址 | 仍是 32 KB，只是认绝对地址 |
| 10 | Matrix Mem：DTE 章节写 36 MB，pcode 地址常量写 32 MB | tag 注释写低 26 bit、36 MB | 地址换算时容量上限要定一个，否则 SF 基址 `0x4AC0_0000` 和 36 MB 数据区会重叠判断不一致 | 按 32 MB 数据 + 4 MB SF 认文档地址 |
| 11 | Router 表字段是 `directionMask / nxtVC / streamCheck / operation / stallWay / use_core_mask / reduceInPrecision` | `RouteEntry` 是 `flow_dir` 的 mid/left/right、`reduce_in_mask`、`stream_need` 等 30 个字段 | 不能把 JSON 原样写进 `Preload` | 换算在导出器，不在模拟器。归约输入精度各路不一致时用 `reduce_data_type` bit8 加分路掩码，这是导出约定，文档里没有这个打包 |
| 12 | release：现行 20 bit，四行各 5 bit（Up/Down/Left/Right）。旧 catalog 是 12 bit | `SetCreditBypass(入口方向, 出方向掩码)`，方向是 mid/left/right | 20 bit 字不能直接当掩码写 | 解码在导出器。当前这份 EP1 catalog 已是 20 bit |
| 13 | 透传核不配 Router 表，下一跳由 release 反推 | Router 每跳都查表 | 包停在没有表项的核上 | 导出器补只转发的 `RTAB` |
| 14 | B/R 的搬入可以写在任务链第 0 项（wait_wake） | 自启动核只从上电派 Task 0；Router 触发的搬入只走 `DATAIN`，完成不推进链 | R core 的 `check_flag` 永远不派 | 导出器拆链。模拟器这条约定保持不变 |
| 15 | `pcie_sw_data_out` 的 Router→PCIe→DDR 暂不模拟 | 没有对应的出核消费者。边缘核（EP1 为 chip 47 p4）没有 datain，task 0 的 PC 仍是 0 | 若给它建链，RV 从 0 取指走出 ITCM | 参考实现在 UserMatch：PC 为 0 只消费 trigger，不建 stream |
| 16 | 归约按方向收齐再加，本核 task 号与上游无关 | 旧逻辑用发送方 `task_id`（`reduce_seq`）配对 | pcode 各核任务号各自编号，同一笔配不上 | 已改成按用户、head flit、lane 是否已在 `in_done_mask` |
| 17 | 出核可以一段在 Core Mem、一段在 Matrix Mem | Core Mem 读延迟 13 拍、Matrix Mem 8 拍。响应只保持一拍，旧逻辑只看队头那一块 | 后回来的响应被丢掉，读侧 outstanding 不归零 | Lane 先分存储排队，再按发出顺序配对 |
| 18 | Core Status、Reduce Mem `0x4880_0000`、SCP ctrl_noc、权重经 ctrl_noc 写入 Matrix Mem | 无 Core Status 口；Rmem 在 Router 内按用户分区；权重在 MoE 用例里走 msg 流，pcode 路径两边都没灌字节 | 功能对拍缺数据面，不是缺一条寄存器 | 参考实现同样没灌权重 |

文档内部还有几处 pcode 已经选边、模拟器不必再选的矛盾，记在 `PLAN.md` §3：`AC_expert_stride` 按半字拆 token / output；`data_type` 3/4 按 1 bit 截断；Step5 的 `fc1_out` placement 照原文放在 `FC3_CHIPS`；Step10 场景把 concat 前 7 片改成 `dim/8*7`，与主文档整段 `dim` 不同。

---

## 4. 和参考仓库的关系

`latch_20261009` 已经把第 3 节里标成“已改”的项做进模型，并有 `test/bach/ip/pcode_bundle.cpp`。当前仓库不要整目录覆盖，原因有三条：

- 当前 latch 的 MoE 用例、`BACHIR 10` bundle、`bach.h` 的 `primitive_mode` 位序还在用。`primitive_mode` 和归约 16 B 前缀如果无条件改掉，现有 `moe_lpu` / `moe_chip` 的数值比对会变。参考实现用 bundle 版本把 16 B 前缀限在 `BACHIR 10`，这个分界值得留。
- 参考实现的 EP1 结果是链路排空，合成 topK 走 Drain，出口 0 包。把它当功能黄金会把“没算”记成通过。
- 当前模型里 DTE 模版、段 0 标成 header、`seg_valid[5:3]` 已经和文档一致，不必按那份修订记录从更老的 `addr_valid[6:3]` 再改一遍。

可以照着搬的是装载记录、地址别名、CSR 译码、MU 模版，以及那份修订记录里每条都对应过一次现场的行为差。搬的时候用现有 MoE 单测当回归，用 EP1 bundle 当新用例。

上面三条是对照 `latch_20261009` 时的取舍。针对第 3 节差异表，已经定下来的改法在 §5。`primitive_mode` 不再保留旧位序：模型和 `bach.h`、`kernel_mu.c` 一起改成文档位序，并重编现有 MoE bundle。

---

## 5. 已定修改方案

2026-10-09 定。代码还没改。目标是 EP1+PP3+TP16 一个 token 在当前 latch 上能看时序，并且 concat 目的核的 Core Mem 能和 CPU 参考对上。现有 `BACHIR 10` 的 MoE 用例要在改完后仍然通过。

### 5.1 模型与编译器一起改

`primitive_mode` 收成参考文档那一套，三处同时改，然后重编 `src/bach/compiler/bundle/` 里的 MoE bundle，再跑 moe 用例：

- 模型 `src/bach/ip/chip/core/mu/regfile.h` 的解码
- `src/bach/compiler/kernel/bach.h` 的宏
- `src/bach/compiler/kernel/kernel_mu.c` 里组这个字的地方

文档位序：`primitive_type[0]`、`a_data_type[1]`、`c_data_type[2]`、`share_ep_data_type[4:3]`、`router_ep_data_type[6:5]`、`share_en_mask[14:7]`、`n_experts[22:15]`、`router_reduce_en[23]`、`task_last[24]`。

现有 `kernel_mu.c` 把 C 精度写在 bit 3、权重精度写在 bit 7。模型若只改解码、宏不改，moe 的输出会从 BF16 变成 FP32，权重会从 MXFP8 变成 BF16。所以不采用“BACHIR 10 旧位序、BACHIR 11 文档位序”两套解码。

### 5.2 第一层：装进模型，访存和身份读对

对应差异表 1、2、3、4、6、7、8。BACHIR 10 的记录和三份全局 `KERNEL` 保留。

| 改动 | 做法 |
|---|---|
| 认 `BACHIR 11` | `LoadBundle` 同时认 10 和 11。11 增加 `KERNELCORE`（每核每 RV 一份 ihex）、`DSAREG`（unit 0/1/2 的模版初值）、`SMEM`（Share Mem 非零字） |
| 文档绝对地址 | DTE `Agcu` 与 RV 窗口两套都认。`0x4830_0000` 别名 Share Mem，`0x4840_0000` 别名 Core Mem，Core SF / Matrix / Matrix SF / header table / topK table 按 `addr.py` 换算成现有 tag。Core Mem 别名三个 RV 都可见 |
| CSR | `csrr/csrw` 的 `0xFC0/0xFC1/0x7C0/0x7C1/0x7C2` 译到现有 task-ctrl MMIO。原来的 MMIO 写法留给现有 kernel |
| MU 模版 | `0x1000` 起 4 套 × 128 B。`TASK_TRIGGER[2:1]` 选模版，再与动态区合并。现在写入 `0x1000` 被丢掉 |
| `hw_header_op` | 消费 `CFG_TRANS_MODE[7]`。源端为 Router 时 0 保存 32 B 包头、1 丢弃。段 0 不进 payload |
| 归约前缀 | `BACHIR 10` 仍跳过 payload 前 16 B。`BACHIR 11` 不跳过。pcode 数据段是纯张量，16 B 会让 EP1 的 4608 B FP32 收成 BF16 后比目的端 2304 B 长 8 B |

Share Mem 容量保持 32 KB（差异 9）。EP1 的 head/tail 在 `0x4830_0000` 附近，在这个窗口里。文档写的 128 KB 这一轮不扩。

Matrix Mem 容量保持现在的 36 MB 检查上限（差异 10）。文档绝对地址按 pcode 的 32 MB 数据 + 4 MB SF 认，避免 `0x4AC0_0000` 和 36 MB 数据区叠在一起。`BankedMem` 的 `store` 已经按行用 `std::map` 稀疏存放，未写入的行不占堆。

### 5.3 第二层：和第一层一起改的行为差

对应差异表 15、16、17，再加上 `latch_20261009` 上 EP1 停钟前改过、不改就到不了 concat 的几处。改完用现有 moe 用例回归。

| 项 | 改法 |
|---|---|
| 归约配对 | 同一用户、head flit、这条 lane 已经在 `in_done_mask` 里才算新的一笔。不用发送核的 `task_id` |
| 跨存储读响应 | Lane 先把 Core Mem 和 Matrix Mem 本拍的响应分开排队，再按发出顺序配对 |
| PC 为 0 | 只消费 trigger，不建 stream，也不从 0 取指。EP1 片外那一跳把包送进 chip 47 p4，该核没有 datain |
| 进核 reduce datain | 只等包，不占 Rmem。占了又不还的话，下一条真正的出核 reduce 停在等待 |
| `stall_way` 多 flit | 只在单 flit 整包时转存。多 flit 留在 VC 里等资源，避免下游先看到没有分区的非首 flit |
| 纯转发的 type-4 | 本核 `operation` 为 forward 时，DTE 发完即完成，不等 Router Reduce Done |
| 归约收下 | 已有 stream 上的 P2P reissue 一对被跳过。不新建 stream，避免和正在跑的广播抢同一笔 |
| `reduce_data_type` bit8 | bit8 置位时 bit0..2 按 mid/left/right 标哪一路是 FP32，其余是 BF16。当前模型把整个字段当 0/1，EP1 混精度会把元素个数算飞。各路一致时仍是 0 或 1，现有 MoE bundle 走这条 |

### 5.4 第三层：一层权重，稀疏占用

对应差异表 18 里的权重。这一轮要做数值对拍，范围限定如下。

地址按 pcode 已经排好的一整层来：`B_ADDR + 组内专家号 × B_expert_stride`，FC1 / FC3 / FC2 各在自己那组 chip 上。256 个路由专家的地址跨度保留，这样 kernel 里的专家号不用改。

物理上只写入被这个 token 激活的专家：1 个共享专家 + 8 个路由专家，外加它们的 mxfp8 scale。未激活的专家不 `Poke`，`BankedMem` 里就没有这些行。topK 写成这些专家的组内序号，不再用 `(i * 7 + 3) & 0xFF` 那种合成字节。

参考计算走和模拟器相同的 mxfp8 / bf16 数值函数，形状和分片与场景一致：FC1 GEMV → 片内归约 → silu·dot·quant → FC2 GEMV → 归约 → concat。比对向量是 concat 目的核 Core Mem 里的那一段 BF16，对余弦相似度，不要求逐字节相同。通过线先定在 **≥ 0.99**。目的核是 tray 2、chip 15 的逻辑核 7，物理核和字节偏移等 bundle 导出后从 catalog 的 `core_map` 查，不事先写死。

激励：topK 按 MU 文档排，`ep_counter` 2 B、8 个组内序号各 2 B、8 个 FP32 权重各 4 B，再补齐到 56 B。组内序号用 0..7，另加 1 个共享专家。权重和 scale 用固定图案，只写入这 9 个专家。片外不对。`pcie_sw_data_out` 的 Router→PCIe 段 pcode 不模拟，出口 0 包是预期。

Core Status、Reduce Mem 绝对地址 `0x4880_0000`、SCP ctrl_noc 这一轮不建。

### 5.5 留在导出器，模拟器不改

| # | 原因 |
|---|---|
| 11 | `directionMask` 等到 `RouteEntry` 的换算已经在 `latch_export.py`。分路精度用 `reduce_data_type` bit8，这是导出约定 |
| 12 | 20 bit release 在导出器解成 `RELROUTE`。当前 EP1 catalog 已是 20 bit |
| 13 | 透传核的只转发表项由导出器按 release 补 |
| 14 | R core 链上第 0 项搬入由导出器改记 `DATAIN`。模拟器仍是：自启动核上电派 Task 0，Router 触发的搬入走 `DATAIN` 且不推进链 |

### 5.6 实施时的回归

1. 改完 `primitive_mode` 和归约前缀之后，先跑现有 moe 用例（至少 `moe_chip` 这一档），确认 `BACHIR 10` 数值还在。
2. 再装 EP1 的 `BACHIR 11` bundle：空闲停钟、计算核没有停在等待或在途、concat 目的核那段 BF16 与 CPU 参考的余弦相似度 ≥ 0.99。
3. 时序先看用例打印的停钟拍数、每核指令数和在途 stream。48 颗 chip 全开 `.trace` 太大，要看波形时再对少数 chip 开 `tracetto`。
