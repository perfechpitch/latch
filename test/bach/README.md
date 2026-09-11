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

构建类型不给就是 `Debug`。跑三层整体那几份要给 `Release`：48 颗 chip 那一份要推 39404 拍，`Debug` 下一份就要跑掉好几分钟。这台机器（16 核 32 线程）上 `ctest -j 8` 全量一遍约 9 分钟，其中大半花在 `reference` 那份 Python 自检上，C++ 这一侧最长的 `moe_lpu` 约 2 分钟。

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
符号表，`.elf` 与 `.dump` 是编译中间件与对着看用的。`.hex` 由 `gen_hwconfig.py` 拷进
bundle 下各套配置的目录。镜像不在就跳过，用例自己会报 `kernel 还没编`。

***

## 三层整体用例

这几份装真实的 RV32 kernel，走的是真模块：TS 建 stream 表项并下发 task，RV core 跑 kernel 配 DSA，DSA 搬运并报完成，Router 转发与归约。桩只接在被测那一层的外面。

### core 层

| 目标 | 用例数 | 验什么 |
| - | - | - |
| `e2e` | 9 | 一个 token 从 Router 进来走完整条链：建 stream、下发 task、跑 kernel、搬运、退休。含四个 token 连着跑那一个 |
| `bcore` | 2 | B core 的两条链：datain 把这一格是哪个用户记进 Share Mem，自启动那条按到达顺序广播出去 |
| `rcore` | 3 | R core 的两条链：一个用户的两笔从两个方向来，等齐了相加送下一组。含乱序到达与只到一笔 |
| `moe` | 3 | 一个 core 上那一段 MoE 的数值。只走计算通路，不经 TS 与 RV core，三笔 MU 任务与门控那几条 VU 宏指令由用例按顺序下发 |

### chip 层

| 目标 | 用例 | 规模与拍数 |
| - | - | - |
| `chip_e2e` | `TokenCrossesTwoChips` | 一个 token 过 C2C 从一颗 chip 到另一颗，中间隔一段 300 拍的 PCIe 链路 |
| `moe_chip` | `BcoreStartsTheBroadcast` | 一颗 chip 八个 core，5164 拍 |
| `moe_chip` | `OneEpGroupReducesSixtyFourCores` | 一个 EP 组八颗 chip 六十四个 core，11810 拍 |
| `moe_chip` | `TwoEpGroupsMeetAtTheReductionCore` | 两个 EP 组在 R core 上汇合，8967 拍 |
| `moe_chip` | `WeightsComeInBeforeTheFirstToken` | 权重先走数据面进 Matrix Mem，切业务模式之后再发 token，12042 拍 |

### LPU 层

| 目标 | 用例 | 规模 |
| - | - | - |
| `lpu_e2e` | 2 | 一个 token 从入口桩进阵列、穿 15 颗 chip、从出口桩出来。走 `Lpu` 那一层的装配与 PCIe Switch，只验链路不算数 |
| `moe_lpu` | `OneLayerAcrossFortyEightChips` | 48 颗 chip 三百八十四个 core 摆成 12 层 × 4 列，两层一个 EP 组共 6 组，39404 拍 |

`moe_chip` 的头一个用例与 `moe_lpu` 的配置不写在用例里：任务链、路由表、TS 的全局项与
三份 kernel 都由 `LoadBundle()` 装。一份拓扑描述编出一套 bundle，落在
`src/bach/compiler/bundle/<拓扑名>/` 下，里面是 `<拓扑名>.bachir` 加三份 kernel 镜像，
每套自己带全。改了拓扑或者改了 kernel 都要重编一次：

```shell
make -C src/bach/compiler/kernel                                    # kernel 变了
python3 src/bach/compiler/gen_hwconfig.py --topo src/bach/compiler/topo/moe_lpu.json
```

权重、topK 表、本组专家表与 Share Mem 的初值仍由用例铺：那几样是模型参数不是配置。

`moe_lpu` 的 chip 之间由用例直接对接，走的不是 `Lpu` 那一层的装配，也没有 PCIe Switch 与进出口桩。算得对与装配对这两件因此分在两份用例里。

***

## 各模块自己的基线

按被测模块归类。这些拿桩喂自己那一段，跑得快，改一个模块先跑它自己那份。

| 模块 | 目标 | 用例数 |
| - | - | - |
| Router | `router_basic`、`router_config`、`router_arbiter`、`router_core_station`、`router_reduce`、`router_reissue`、`router_assembly`、`router_scenarios` | 71 |
| TS | `ts`、`ts_config`、`ts_chain`、`ts_issue`、`ts_done`、`ts_credit` | 40 |
| DTE | `dte`、`dte_inbound`、`dte_commit`、`dte_lane`、`dte_completion`、`dte_tables` | 51 |
| MU | `mu`、`mu_issue` | 25 |
| VU | `vu` | 34 |
| RV core | `rv_core`、`rv_lsq`、`rv_task` | 18 |
| 三块存储 | `memory` | 18 |
| 数值格式 | `numeric`、`numeric_cross` | 20 |
| 片外与链路 | `link`、`external`、`pcie_switch` | 20 |

## 装配基线

这两层自己不打拍，判据是结构性的：接线对不对、少构造了哪些模块、boot 序列走没走完、坐标换算对不对。

| 目标 | 用例数 | 验什么 |
| - | - | - |
| `chip` | 22 | core 与 chip 两层的装配，含 C2C 上的包拆了又拼回来还是不是原来那一个 |
| `lpu` | 17 | 48 颗 chip 的坐标换算、同层左右与同列上下的直连、跨 tray 那两处的参数、每层两端接的 PCIe Switch |

## 两条 Python 自检

| 目标 | 跑的是 | 查什么 |
| - | - | - |
| `hwconfig` | `src/bach/compiler/selftest_hwconfig.py` | 硬件配置生成器：对 `compiler/topo/` 下的每份拓扑描述跑一遍生成，查产物记录齐全、查同一种记录的字段数一致、查同一份描述两次跑出来逐字节一致 |
| `reference` | `src/bach/compiler/reference/selftest.py` | 参考实现自身，兼查比对向量与代码同步 |

比对向量不同步的话，C++ 那一侧比的就是旧结果，所以后一条查的是同步而不只是自检。

***

## 波形

七份 moe 用例各写一份波形，落在跑测试时的当前目录：

| 目标 | 波形 |
| - | - |
| `moe` | `moe_one_core.trace`、`moe_vu_gate.trace`、`moe_five_step.trace` |
| `moe_chip` | `moe_chip_bcast.trace`、`moe_chip_group.trace`、`moe_chip_two_groups.trace`、`moe_chip_weights.trace` |
| `moe_lpu` | `moe_lpu.trace` |

打开：

```shell
python3 -m src.utils.insight serve moe_lpu        # 网页，不给名字就挑当前目录下最新那份
python3 -m src.utils.insight inspect moe_lpu      # 命令行列模块与信号
python3 src/bach/replay.py                        # 列出当前目录下的波形挑一份，回放成一页 HTML 动画
```

前两条是 latch 通用的读波形工具。第三条 `src/bach/replay.py` 只管 Bach：它认的是 chip 与
core 的摆法和 `Core::EmitTrace()` 那几个信号名，换个项目不成立，所以放在 bach 这一侧。
它是一个独立脚本，读波形那一段按 latch 的 `.trace` 格式自己解，不依赖仓库里别的 Python
代码，也不用装第三方包，从哪个目录执行都行。

`replay.py` 不带参数就把执行时所在目录（连同子目录）下的波形按时间列出来，新的在前，挑一个就生成，产物落在波形旁边、同名换成 `.html`，最后打印它的完整路径。给一截名字就只列名字里带它的；`-o` 另指产物路径。

产出是本地单文件，事件数据内联在里面，不依赖外部资源，双击就能看。四级视图逐级点进去：

| 视图 | 画什么 | 一「步」是 |
| - | - | - |
| 阵列 | 所有 chip 排成格子，方块深浅是本拍片内转发的 flit 数，线亮是 C2C 上有数据在途 | 一帧 |
| chip | 片内 core 与它们之间的 left / right / mid 链路 | 一帧 |
| core | 上半是 core 内的方框图，正在跑的那一笔点亮它经手的 TS → RV core → DSA；下半是甘特图，DTE / MU / VU 各一行，每笔 task 画成从下发到完成的一段 | TS 下发一笔 task |
| Router | 在 core 的方框图里点 Router 进来。方框与连线照 `09-router-*.html`：三个 RouterStation、Xbar、CoreStation、ReduceModule、CoreMem 重发、Retire、CoreMemCreditMonitor、RouterTable，每个框写本拍的量，本拍有 flit 经过的线点亮；下面是九个口（进 mid / left / right / core，出 mid / left / right / core / 归约）的逐拍活动条 | 一帧 |

空格播放暂停，左右方向键单步，Esc 退上一级。底下的进度条按住拖动，滚轮单帧。甘特图上按住拖动逐拍走，点一段跳到它下发的那一拍。甘特图的时间轴只铺这个 core 真正活动的那一段。

完成配给下发只在同一时刻最多一笔在飞的 core 上做：`ts_done` 不分路，几笔并发时分不清哪次完成是哪一笔的。B core 与 R core 同时挂着十几个 stream，那种 core 只画下发与完成的刻度。画面与数字全部
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

### 一个 core 记哪 27 个信号

一处定完，写在 `Core::EmitTrace()` 与它调的 `EmitRouter()`、`EmitIssue()` 里。看的是数据
与任务在这个 core 上流没流动、堵没堵。Router 那一组照设计文档 `09-router-*.html` 画的方框
拆，每个方框一两个量：

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

另有两个打包的，记的是本拍新下发了哪几笔 task：

| 信号 | 编码 |
| - | - |
| `ts_unit` | 位掩码，bit0 DTE、bit1 MU、bit2 VU。0 表示本拍没有新下发 |
| `ts_task` | 三路的 task 号各占 8 bit：`dte │ mu << 8 │ vu << 16`。那一路本拍没发就填 `0xFF` |

认「新的一笔」看的是三条发射通路各自的 `seq`：一笔命令会在端口上连着摆几拍等 RV core
收下，只看 `cmd_valid` 会把同一笔数很多遍。三条通路各管各的 stream，同一拍可以各发各的，
所以两个信号都按路分位。

这两个是唯一能把仿真跑出来的次序和 bundle 里 `TCHAIN` 逐项对上的东西。比如 `moe_lpu` 的
`chip0.core2` 跑出来是第 347 拍 DTE task 0、458 拍 MU task 1、2237 拍 VU task 2、
2606 拍 MU task 3，3740、6234、7039 拍 DTE task 4、5、6，与
`awk '$1=="TCHAIN" && $2==0 && $3==2'` 打出来的七项一致。

不派角色的 core 只有 Router，记 Router 那一组 21 个。

chip 这一级另记四座 C2C 桥各 5 个，加 `scp.state` 一个。

### 记更多信号

一个模块记不记波形，在它建出来那一刻定下来。`Core` 的构造把底下所有单元包在
`TraceOffScope`（`src/base/signal_tracer.h`）里，所以经装配建出来的模块一个信号
都不发，波形上只有 `EmitTrace()` 那一组。

要多记几个，往 `EmitTrace()` 里加一行；值从各模块的观测访问器读。要看某一个模块
自己的全部信号，跑 `test/bach/ip/chip/core/` 下对应的那份单模块用例，它不经装配
这一层，模块的信号一个不落。

`SetTraceDisabled(true)` 管的是同一件事的另一头，按实例挑：片内那五十多条链路与
`noc` 端点就是这样关掉的。`moe_lpu` 的 `kTraceChips` 也走它，48 颗全记是 12538 个
信号、1.8 MB，所以默认全记；只想看某一段就把这个数调小。

***

## 判据从哪来

算数值的那几份（`moe`、`moe_chip`、`moe_lpu`、`e2e` 的后三个、`numeric`）不写期望值，与 `src/bach/compiler/reference/` 的 Python 参考实现逐 bit 比对。向量存在 `src/bach/compiler/reference/vectors/`，由 `vectors.py` 产出：

```shell
python3 src/bach/compiler/reference/vectors.py     # 全部重算，约 46 分钟
```

权重几百 KB 到几 MB，向量里只给种子，两侧按同一个规则各自生成。改了尺寸才需要重算。

***

## 加一个测试

放进 `test/bach/` 下与被测模块对应的目录，文件名就是目标名。共用的东西按 `test/...` 引：

* `test/bach/ip/chip/moe_common.h`：MoE 那几份共用的比对向量读入、拓扑换算、路由铺法、注入与收取的 harness
* `test/bach/ip/chip/core/` 下按模块分目录，与 `src/bach/ip/chip/core/` 一一对应

协程槽位要自己数够：每个挂时钟的模块占一个常驻协程，槽位不够时多出来的协程永远等不到空位，表现是进程卡住而不是报错。`moe_common.h` 的 `EnsureSlots(n)` 收的是本次要各占协程的 core 数。
