# Bach 建模的测试

**模式**：spec（陈述现在有哪些测试、各验哪一段）

这份给两种人：要跑测试确认改动没破坏什么的，和要加测试的。读完知道现在有哪些目标、每个验的是哪一段硬件、判据从哪里来、新加一份该放在哪。

***

## 怎么跑

CMake out-of-tree 构建，配一次之后增量编：

```shell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build/test && ctest --output-on-failure
```

构建类型不给就是 `Debug`。跑三层整体那几份要给 `Release`：48 颗 chip 那两份要推 15398 拍与 41653 拍，`Debug` 下一份就要跑掉好几分钟。这台机器（16 核 32 线程）上 `ctest -j 4` 全量一遍约 4 分钟，最长的是 `moe_lpu_tokens`，单独跑约 2 分半，`moe_lpu` 约 45 秒。

`test/bach/` 下每个 `.cpp` 自动成为一个 CTest 目标，目标名取文件名。跑单个目标：

```shell
ctest --output-on-failure -R '^moe_chip$'
./build/test/bach/moe_chip --gtest_filter='*BcoreStartsTheBroadcast*'
```

跑装 kernel 的那几份之前 kernel 要先编出来，改过 `src/bach/compiler/kernel/` 下任何一处都要重编：

```shell
make -C src/bach/compiler/kernel
```

四样产物都落在 `kernel/build/`：`.hex` 是装载用的镜像，`.sym` 是编译器取 `TASK_PC` 用的
符号表，`.elf` 与 `.dump` 是编译中间件与对着看用的。镜像不在就跳过，用例自己会报
`kernel 还没编`。

`kernel/` 里动过任何一处，**重编之后还要重跑 `gen_hwconfig.py`**：

```shell
cd src/bach/compiler && python3 gen_hwconfig.py --topo topo/<拓扑名>.json
```

`_start` 或任一个 `task_*` 的代码尺寸一变，后面所有 task 入口都平移；bundle 里 `.bachir`
的 `TASK_PC` / `DATAIN` 是生成时按 `.sym` **写死**的，`.hex` 也是那一步拷进去的。只重编不
重跑，`moe_chip` / `moe_lpu` / `moe_lpu_tokens` 会成对地停在旧镜像上——**用例照绿，但跑的
不是新 kernel**。只换其中一半更糟：PC 与镜像差几个字节，核从指令中间起跑。

***

## 三层整体用例

这几份装真实的 RV32 kernel，走的是真模块：TS 建 stream 表项并下发 task，RV core 跑 kernel 配 DSA，DSA 搬运并报完成，Router 转发与归约。桩只接在被测那一层的外面。

### core 层

| 目标 | 用例数 | 验什么 |
| - | - | - |
| `e2e` | 9 | 一个 token 从 Router 进来走完整条链：建 stream、下发 task、跑 kernel、搬运、退休。含四个 token 连着跑那一个 |
| `bcore` | 2 | B core 的两条链：datain 把这一格是哪个用户记进 Share Mem，自启动那条按到达顺序广播出去。token 是 6144 个 MXFP8，scale 随它进出 Matrix Mem |
| `rcore` | 3 | R core 的两条链：一个用户的本行结果与上一行送来的累加结果各一包，从两个方向来，等齐了按 BF16 相加送下一行。含乱序到达与只到一笔 |
| `moe` | 3 | 一个 core 上 KN 拆分那一段的数值：槽位 s 的 FC1、FC3 部分和，silu·dot·量化，FC2 第 s 段。前两个用例只走计算通路，MU 任务与 VU 宏指令由用例按顺序下发；第三个装 kernel、走 dot core 的任务链 |

### chip 层

| 目标 | 用例 | 规模与拍数 |
| - | - | - |
| `chip_e2e` | `TokenCrossesTwoChips` | 一个 token 过 C2C 从一颗中间列 chip 到另一颗，片内经过坏 core7，中间隔一段 300 拍的 PCIe 链路 |
| `moe_chip` | `BcoreStartsTheBroadcast` | 一颗第一列 chip 八个计算 core：B core 广播 token，chip 内归约进 dot core，FC2 输入广播回本 chip，concat 从 E 口出去，1940 拍 |
| `moe_chip` | `OneRowOfTwoChipsLandsInTheReductionCore` | 一行两颗 chip，左边那颗带坏 core2、core7，行链两跳落进右边那颗的 R core，3045 拍 |
| `moe_chip` | `OneEpGroupHasTwoReductionCores` | 一个 EP 组八颗 chip 六十四个计算 core，中间两列 chip 带坏 core2、core7，两行各进本行 R core，两个 R core 串链，4958 拍 |
| `moe_chip` | `WeightsComeInBeforeTheFirstToken` | 权重与 scale 先走数据面进 Matrix Mem，切业务模式之后再发 token，8704 拍 |

### LPU 层

| 目标 | 用例 | 规模 |
| - | - | - |
| `lpu_e2e` | 2 | 一个 token 从入口桩进阵列、穿 15 颗 chip、从出口桩出来。走 `Lpu` 那一层的装配与 PCIe Switch，只验链路不算数 |
| `moe_lpu` | `OneLayerAcrossFortyEightChips` | 48 颗 chip 摆成 12 层 × 4 列，每颗 2×5，共 480 个 core、三百八十四个计算 core，两层一个 EP 组共 6 组，12 个 R core 逐行串链；发一个 token，逐颗 chip、逐行核对中间量，15398 拍 |
| `moe_lpu_tokens` | `ThirtyTwoTokensWithSixteenCredits` | 同 `moe_lpu` 的 48 颗 chip；GPU 一侧有 16 份额度，连续发 32 个 token，额度一直用满。每个 token 一个用户号，R core 上落同一个槽的两个 token 前一个的结果出来才发后一个；核对出口上的每一包，41653 拍 |

`moe_chip`、`moe_lpu` 与 `moe_lpu_tokens` 的配置不写在用例里：任务链、路由表、进核配置、TS 的全局项与
三份 kernel 都由 `LoadBundle()` 装。一份拓扑描述编出一套 bundle，落在
`src/bach/compiler/bundle/<拓扑名>/` 下，里面是 `<拓扑名>.bachir` 加三份 kernel 镜像，
每套自己带全。`compiler/topo/` 下四份拓扑描述各对一套：`moe_chip`、`moe_two_groups`、
`moe_group`、`moe_lpu`，`moe_lpu_tokens` 装的也是 `moe_lpu` 那一套。改了拓扑或者改了 kernel 都要重编一次：

```shell
make -C src/bach/compiler/kernel                                    # kernel 变了
python3 src/bach/compiler/gen_hwconfig.py --topo src/bach/compiler/topo/<拓扑名>.json
```

权重、topK 表、本组专家表，以及第 0 行 R core 在 Share Mem 里表示“只等本行结果”的那个标志，仍由用例铺：那几样是模型参数不是配置。
weights 加载那一条 path 只在那个用例里用，不进 bundle，由用例自己铺。

`moe_lpu` 与 `moe_lpu_tokens` 的 chip 之间由用例直接对接，走的不是 `Lpu` 那一层的装配，也没有 PCIe Switch 与进出口桩。算得对与装配对这两件因此分在不同的用例里。

***

## 各模块自己的基线

按被测模块归类。这些拿桩喂自己那一段，跑得快，改一个模块先跑它自己那份。

| 模块 | 目标 | 用例数 |
| - | - | - |
| Router | `router_basic`、`router_config`、`router_arbiter`、`router_core_station`、`router_reduce`、`router_reissue`、`router_assembly`、`router_scenarios` | 77 |
| TS | `ts`、`ts_config`、`ts_chain`、`ts_issue`、`ts_done`、`ts_credit` | 59 |
| DTE | `dte`、`dte_inbound`、`dte_commit`、`dte_lane`、`dte_completion`、`dte_tables` | 53 |
| MU | `mu`、`mu_issue` | 27 |
| VU | `vu` | 47 |
| RV core | `rv_core`、`rv_lsq`、`rv_task`、`custom0` | 25 |
| 三块存储 | `memory` | 23 |
| 数值格式 | `numeric`、`numeric_cross` | 21 |
| 片外与链路 | `link`、`external`、`pcie_switch` | 21 |

## 装配基线

这几份自己不打拍，判据是结构性的：接线对不对、坏 core 进没进透传档、boot 序列走没走完、坐标换算对不对、bundle 与模型对不对得上。

| 目标 | 用例数 | 验什么 |
| - | - | - |
| `chip` | 22 | core 与 chip 两层的装配，含写 `core_bad_mask` 之后坏 core 进透传档、C2C 上的包拆了又拼回来还是不是原来那一个 |
| `lpu` | 17 | 48 颗 chip 的坐标换算、按列的 `core_bad_mask` 与角色分配表、同层左右与同列上下的直连、跨 tray 那两处的参数、每层两端接的 PCIe Switch |
| `bundle_load` | 13 | 装载检查：坏 core 的个数与位置、坏 core 上只有 Router 的配置、角色与体现角色的那几项配置对得上，每条一个反例 |

## 两条 Python 自检

| 目标 | 跑的是 | 查什么 |
| - | - | - |
| `hwconfig` | `src/bach/compiler/selftest_hwconfig.py` | 硬件配置生成器：对 `compiler/topo/` 下的每份拓扑描述跑一遍生成，查产物记录齐全、查同一种记录的字段数一致、查同一份描述两次跑出来逐字节一致 |
| `reference` | `src/bach/compiler/reference/selftest.py` | 参考实现自身，兼查比对向量与代码同步 |

比对向量不同步的话，C++ 那一侧比的就是旧结果，所以后一条查的是同步而不只是自检。

***

## 波形

九份 moe 用例各写一份波形，落在跑测试时的当前目录：

| 目标 | 波形 |
| - | - |
| `moe` | `moe_one_core.trace`、`moe_vu_gate.trace`、`moe_dot_chain.trace` |
| `moe_chip` | `moe_chip_bcast.trace`、`moe_chip_group.trace`、`moe_chip_two_groups.trace`、`moe_chip_weights.trace` |
| `moe_lpu` | `moe_lpu.trace` |
| `moe_lpu_tokens` | `moe_lpu_tokens.trace` |

打开：

```shell
python3 -m src.utils.insight serve moe_lpu        # 网页，不给名字就挑当前目录下最新那份
python3 -m src.utils.insight inspect moe_lpu      # 命令行列模块与信号
python3 src/bach/replay.py                        # 列出当前目录下的波形挑一份，回放成一页 HTML 动画
```

前两条是 latch 通用的读波形工具。第三条 `src/bach/replay.py` 只管 Bach：它认的是 chip 与
core 的摆法和 `Core::EmitTrace()` 那几个信号名，换个项目不成立，所以放在 bach 这一侧。
顶层模块下面挂着 `coreN`、`c2c_*` 或 `scp` 的就算一颗 chip，名字不限（`chip0`、`chip`、`g0`
都认）；名字末尾带编号的按编号排，其余按名字排，页面上显示原名。
它是一个独立脚本，读波形那一段按 latch 的 `.trace` 格式自己解，不依赖仓库里别的 Python
代码，也不用装第三方包，从哪个目录执行都行。

`replay.py` 不带参数就把执行时所在目录（连同子目录）下的波形按时间列出来，新的在前，挑一个就生成，产物落在波形旁边、同名换成 `.html`，最后打印它的完整路径。给一截名字就只列名字里带它的；`-o` 另指产物路径。

产出是本地单文件，事件数据内联在里面，不依赖外部资源，双击就能看。四级视图逐级点进去：

| 视图 | 画什么 | 一「步」是 |
| - | - | - |
| 阵列 | 所有 chip 排成格子，方块深浅是本拍片内转发的 flit 数，线亮是 C2C 上有数据在途 | 一帧 |
| chip | 片内 core 与它们之间的 left / right / mid 链路 | 一帧 |
| core | 上半是 core 内的方框图，正在跑的那几笔点亮各自经手的 TS → RV core → DSA；下半是甘特图，DTE / MU / VU 各一行，每笔 task 画成从下发到 RV core 交还且 DSA 报完成的一段，DSA 在算的那几截画深色，TS 记完成的那几拍画成时间轴上的刻度 | TS 下发一笔 task |
| Router | 在 core 的方框图里点 Router 进来。方框与连线照 `09-router-*.html`：三个 RouterStation、Xbar、CoreStation、ReduceModule、CoreMem 重发、Retire、CoreMemCreditMonitor、RouterTable，每个框写本拍的量，本拍有 flit 经过的线点亮；下面是九个口（进 mid / left / right / core，出 mid / left / right / core / 归约）的逐拍活动条 | 一帧 |

空格播放暂停，左右方向键单步，Esc 退上一级。底下的进度条按住拖动，滚轮单帧。甘特图上按住拖动逐拍走，点一段跳到它下发的那一拍。甘特图的时间轴只铺这个 core 真正活动的那一段。

一笔 task 的那一段按“一笔 task 的七个时刻”那几个信号配：RV core 的接下与交还按同一路的先后对回下发的那一笔，DSA 的开始与报完成按路、task 号、user_id 对回同一笔。B core 与 R core 同时挂着十几个 stream，也照样一笔一段。逐级 reduce 在 TS 那边要等 Router 报完成，那一拍不在这几个信号里，段只画到 DTE 本地报完成，TS 记完成的那一拍看时间轴上的刻度。画面与数字全部
由波形驱动，累计那一类画的是相邻两拍的差。

网页那一侧起来时会建一次索引，之后一直用它，波形换了要重起或者打 `/api/reload`。
索引缓存在 `/tmp/insight/` 下按文件内容哈希分目录。

用例里那两句管两件事：`TraceInto("<名字>")` 在跑之前换前缀，一个进程里几个用例才不互相
覆盖（默认前缀恒为 `Recorder`）；`TraceDone()` 在模块析构之后、`RT::Reset()` 之前收尾，
因为信号名与层次是从模块表写进波形的，而 `RT::Reset()` 会把那张表清掉。少了后一句，波形
里有数据但一个信号名都没有，网页打开是空的。

其余用例没加这两句，它们的波形仍共用 `Recorder` 前缀、互相覆盖，且模块表是空的。

### 层次

```
chip0
├─ core0 … core9      每个 core 下面直接是信号，单元与模块不在层次里占一级
├─ c2c_n / c2c_e / c2c_w / c2c_s
└─ scp
```

网页那一侧照这个层次折叠，打开时折在顶层，一颗 chip 一行。

### 一个 core 记哪 46 个信号

一处定完，写在 `Core::EmitTrace()` 与它调的 `EmitRouter()`、`EmitIssue()`、`EmitStep()`、
`EmitDsa()`、`EmitRv()` 里，分两组：

* Router 那一组 21 个，每个 core 都记，看的是数据在这个 core 上流没流动、堵没堵
* 任务那一组 25 个，只在 TS 配过任务链或 datain 任务的好 core 上记：4 个计数与水位看任务堵没堵，21 个打包的记一笔 task 经过的七个时刻

坏 core 与 TS 没配过任务的好 core 只有 Router 在用，记 Router 那一组 21 个。

#### 计数与水位

Router 那一组照设计文档 `09-router-*.html` 画的方框拆，每个方框一两个量；表里 TS 与 DTE 那 4 行属于任务那一组：

| 单元 | 信号 | 是什么 | 类 |
| - | - | - | - |
| RouterStation | `fwd_mid`、`fwd_left`、`fwd_right`、`fwd_core` | 从三个 R2R 方向、以及本 core 的 DTE 出核那一路收进 Router、交给 Xbar 的 flit 数 | 累计 |
| RouterStation | `occ_mid`、`occ_left`、`occ_right`、`occ_core` | 这四个入口的 VC Buffer 现在占了多少 | 水位 |
| Xbar | `out_mid`、`out_left`、`out_right`、`out_core` | 往这四个出口各发出的 flit 数 | 累计 |
| Xbar | `out_rdc` | 送进 ReduceModule 三条 lane 的 flit 数之和 | 累计 |
| Xbar | `xbar_stall` | 想出去没出去的笔数 | 累计 |
| CoreStation | `core_in` | 进核那一段现在压着几个 flit | 水位 |
| CoreStation | `cs_trig` | 发给 TS 的 trigger 数 | 累计 |
| ReduceModule | `reduce_q` | 累加完等着发出去的笔数 | 水位 |
| ReduceModule | `rdc_ctx` | 占着几个用户上下文 | 水位 |
| CoreMem 重发 | `reissue` | 暂存着、还没重发出去的笔数 | 水位 |
| Retire | `retire` | 广播过的用户数 | 累计 |
| CoreMemCreditMonitor | `cmcm_q` | 排队等资源的申请数 | 水位 |
| TS | `ts_issue` | 发给 DTE、MU、VU 的 task 数之和 | 累计 |
| TS | `ts_done` | 收回的完成数 | 累计 |
| TS | `ts_inflight` | 手上在飞的 stream 笔数 | 水位 |
| DTE | `core_out` | 出核那一段的 DTE Buffer 现在占了多少 | 水位 |

RouterTable 是静态配置，不记。

累计那一类在模型里是只加不清零的计数器（`forwarded_pending` 这些），要看「这一拍发生
了多少」得取相邻两拍的差。水位那一类是队列长度与缓冲占用，直接读。

#### 一笔 task 的七个时刻

每个时刻三个信号：头一个说这一拍有没有，`_task` 记 task 号，`_user` 记 user_id。

| 时刻 | 信号 | 这一拍发生了什么 |
| - | - | - |
| 建表 | `ts_create`、`ts_create_task`、`ts_create_user` | TS 建了一个表项：新用户到了，或者自启动 core 上一条链退休后原地重新激活 |
| 装后继 | `ts_install`、`ts_install_task`、`ts_install_user` | 上一项做完，Task_ctrl 把下一项写进表项 |
| 下发 | `ts_unit`、`ts_task`、`ts_user` | TS 把一笔 task 发给 RV core |
| RV core 接下 | `rv_start`、`rv_start_task`、`rv_start_user` | 执行器接下队头那一笔，PC 跳到 `task_pc` |
| RV core 交还 | `rv_done`、`rv_done_task`、`rv_done_user` | kernel 执行 `task_done`，把这一笔交还 |
| DSA 开始 | `dsa_start`、`dsa_start_task`、`dsa_start_user` | 这一笔过了单元自己的门槛，真正开始算 |
| DSA 报完成 | `dsa_done`、`dsa_done_task`、`dsa_done_user` | 单元把完成报回 TS |

编码：

| 信号 | 编码 |
| - | - |
| `ts_create`、`ts_install` | 笔数，只加不清零 |
| `ts_unit`、`rv_start`、`rv_done`、`dsa_start`、`dsa_done` | 位掩码，bit0 DTE、bit1 MU、bit2 VU。0 表示本拍没有 |
| `ts_create_task`、`ts_install_task` | task 号 8 bit，本拍没有就填 `0xFF`。自启动 core 原地重新激活的那一步是 task 0 |
| `ts_create_user`、`ts_install_user` | user_id 16 bit，本拍没有就填 `0xFFFF`。自启动 core 建表时还没有用户身份，也填 `0xFFFF` |
| `ts_task` 与 `rv_*`、`dsa_*` 的 `_task` | 三路的 task 号各占 8 bit：`dte │ mu << 8 │ vu << 16`。那一路本拍没有就填 `0xFF` |
| `ts_user` 与 `rv_*`、`dsa_*` 的 `_user` | 三路的 user_id 各占 16 bit：`dte │ mu << 16 │ vu << 32`。那一路本拍没有就填 `0xFFFF` |

认「新的一笔」看的是三条发射通路各自的 `seq`：一笔命令会在端口上连着摆几拍等 RV core
收下，只看 `cmd_valid` 会把同一笔数很多遍。三条通路各管各的 stream，同一拍可以各发各的，
所以三个信号都按路分位。其余几个时刻同样按各自的序号或笔数认，不看端口电平。

波形只在值变了的那一拍记一个事件。同一路连着几拍都下发时，`ts_unit` 几拍的值相同，事件只
落在头一拍；三个信号每拍都发，数下发要把非零的那一段逐拍展开，一拍一笔。

两个时刻相减，读出一笔 task 在各段花的时间：

* 建表或装后继到下发：这一步在 TS 里等的时间，等 credit、等发射通路空出来、等前一笔从 RV core 那边腾出槽位。这一条只对主线下发的 task 成立。搬入任务在数据到 Router 时就下发，与主线走到哪无关：普通模式下 task 0 之外的搬入任务，装后继可能落在下发之后，也可能被一拍跳过；自启动 core 与权重加载模式下的 datain 任务既不建表也不装后继。比如 `moe_lpu` 的 `chip0.core9`（dot core）：task 3 第 1276 拍下发，1303 拍才装后继；7 项 concat 搬入在 2060～2330 拍之间按到达次序下发，装后继从 1907 拍的 task 7 一步跳到 2352 拍的 task 14。
* RV core 接下到交还：这个 RV core 对这笔 task 的执行时间，含 kernel 里轮询 DSA 的时间（MU 的 `mu_wait()`、VU 轮询 `MACRO_INST_LEFT`），不含 DSA 自己算的时间。交还一定发生，通不通知 TS 由 kernel 写进 `task_done` 的值定，所以不通知 TS 的那几笔也有结束的那一拍。
* DSA 开始到报完成：DSA 自己的执行时间。开始取过门槛那一拍，不取写 trigger 那一拍，三个单元的门槛与报完成的时机各不相同：

| 单元 | 开始 | 报完成 |
| - | - | - |
| DTE | 在 PendingTaskQ 里等到 VC credit，再过 Commit 准入、拿到 Lane 与 Completion RS。只算 RV core 配的那一路，Router 入站那一路不算一笔 DTE task | 两侧完成条件配齐、带 `task_last` 的那一笔报到 TS；`no_ack` 的不报 |
| MU | 进 `issue_q`：drain 走完且队列有空位 | 整个 task 的 tile 都写回，写回落地后再空两拍 |
| VU | 被 ISQ 收下：静态配置已释放、上一条已被取走 | 每条宏指令退休报一次，一个 task 发几条就报几次，`dsa_done` 分不出哪一条是这笔 task 的最后一条。`recv_unit` 配 `kRvOnly` 的项，TS 等的是 RV core 轮询完之后的交还，看 `rv_done` |

三个单元的完成脉冲本身不带 user_id，`dsa_*_user` 填的是单元侧存下的那一份：MU 与 DTE 取 RV core 写进 DSA 的 user_id，VU 取写 trigger 那一拍从身份直连线上采下、随宏指令带到退休的那一份。

`ts_unit` 与 `ts_task` 能把仿真跑出来的下发次序和 bundle 里 `TCHAIN` 逐项对上。比如 `moe_lpu` 的
`chip0.core2` 跑出来是第 309 拍 DTE task 0、369 拍 MU task 1、811 拍 DTE task 2、
1808 拍 DTE task 3、1826 拍 MU task 4、2057 拍 DTE task 5，与
`awk '$1=="TCHAIN" && $2==0 && $3==2'` 打出来的六项一致。

chip 这一级另记四座 C2C 桥各 5 个，加 `scp.state` 一个。

### 记更多信号

一个模块记不记波形，在它建出来那一刻定下来。`Core` 的构造把底下所有单元包在
`TraceOffScope`（`src/base/signal_tracer.h`）里，所以经装配建出来的模块一个信号
都不发，波形上只有 `EmitTrace()` 记的那些。

要多记几个，往 `EmitTrace()` 里加一行；值从各模块的观测访问器读。要看某一个模块
自己的全部信号，跑 `test/bach/ip/chip/core/` 下对应的那份单模块用例，它不经装配
这一层，模块的信号一个不落。

`SetTraceDisabled(true)` 管的是同一件事的另一头，按实例挑：片内那五十多条链路与
`noc` 端点就是这样关掉的。`moe_lpu` 的 `kTraceChips` 也走它，48 颗全记是 21138 个
信号、2.9 MB，所以默认全记；只想看某一段就把这个数调小。

***

## 判据从哪来

算数值的那几份（`moe`、`moe_chip`、`moe_lpu`、`moe_lpu_tokens`、`e2e` 的后三个、`numeric`）不写期望值，与 `src/bach/compiler/reference/` 的 Python 参考实现逐 bit 比对。向量存在 `src/bach/compiler/reference/vectors/`，由 `vectors.py` 产出：

```shell
python3 src/bach/compiler/reference/vectors.py     # 全部重算，约 2 小时，几乎都花在 moe_lpu_tokens.txt 上
```

一个 core 的权重约 1.2 MiB，向量里只给种子，两侧按同一个规则逐 tile 生成。改了尺寸才需要重算。

***

## 加一个测试

放进 `test/bach/` 下与被测模块对应的目录，文件名就是目标名。共用的东西按 `test/...` 引：

* `test/bach/ip/chip/kn_data.h`：KN 拆分的尺寸、Core Mem 与 Matrix Mem 上的摆放、权重与 token 的生成，core 层与 chip 层的 MoE 用例共用
* `test/bach/ip/chip/moe_common.h`：chip 层与 LPU 层 MoE 用例共用的比对向量读入、每个 core 的数据、weights 加载阶段、48 颗 chip 的建立与装载、注入与收取的 harness（含连续发 token 时 GPU 一侧的额度），按 chip、按 R core 与按出口上的每一包核对
* `test/bach/ip/chip/core/` 下按模块分目录，与 `src/bach/ip/chip/core/` 一一对应

协程槽位要自己数够：每个挂时钟的模块占一个常驻协程，槽位不够时多出来的协程永远等不到空位，表现是进程卡住而不是报错。`moe_common.h` 的 `EnsureSlots(n)` 收的是本次要各占协程的 core 数。
