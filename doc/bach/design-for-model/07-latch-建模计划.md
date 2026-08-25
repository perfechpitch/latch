# 第 7 章　latch 建模计划

**模式**：design（陈述当前设计，动机与取舍收在各节的“取舍”段落里）

给在 latch 上按逐拍 cycle 模型重建 Bach 的实现者，定下要建哪些对象、每个对象在模型里是什么、第 2 至 4 章定义的机制各落在模型的哪里、代码怎么组织、按什么顺序建、怎么验收。
模型的对象从 GPU 桩到 core 内的每个子块，粒度与第 3、4 章的子块一致；机制全部实现，数值按 bit 级与参考实现比对；第 5 章的参数直接成为各单元的输入。
本章不复述第 2 至 4 章的规则，只写模型怎么承载它们。

《Bach 硬件设计建模参考》第 7 章，全套目录见 [README](README.md)。

***

## 建模范围与对象清单

### 四条总决定

1. **逐拍 cycle 模型**：功能与时序一起建，每个仲裁点、每条通路都有拍数。
2. **机制全覆盖**：第 2 至 4 章定义的每一条机制都落到一个单元的一个函数，并有一个用例；覆盖关系在各单元的“机制覆盖”表里逐条登记。
3. **bit 级数值**：MU 的累加顺序、MXFP8 的 scale block、VU 的三处舍入、Router reduce 的 FP32 中间精度都按硬件规则实现；参考实现按同样的顺序计算，逐元素比对不留容差。
4. **RV core 不建流水线**：kernel 写成 C++ 例程，RV core 按指令预算与访存延迟计拍；task_queue、dsa_iss、CSR、task_done 这些与 TS 和 DSA 交互的机制照建。

### 对象清单

“形态”一列：节点 = 挂时钟的 `ClkModule`；从属单元 = 跟着上级走的 `SubUnit`；静态 = 仿真前算好的表；桩 = 只模仿接口行为的节点。

| 层 | 对象 | 对应设计 | 形态 |
| - | - | - | - |
| 系统 | GPU / DPU 桩 | 第 6 章“GPU → Bach 的两层 credit 反压”、第 2 章 Node 组成 | 桩（节点） |
| 系统 | ETH 链路、PCIe 链路、C2C 链路 | 第 2 章互连参数、第 5 章延迟表 | 链路（Fifo + 到达时刻） |
| 系统 | PCIe Switch | 第 2 章 Node 组成、第 6 章两层 credit | 节点 |
| 系统 | 出口桩 | 第 6 章输出包格式 | 桩（节点） |
| chip | Chip（2×5 阵列、Harvest mask、C2C 端口） | 第 2 章“Chip 与 Harvest” | 装配 |
| chip | SCP 与 ctrl_noc | 第 2 章 Boot、第 3 章 Bach Core 顶层 | 桩（节点）+ 配置总线 |
| core | Core | 第 3 章 Bach Core 顶层 | 节点 |
| core | Router：RouterTable 与 CSR、RouterStation ×4、Xbar、CoreStation、CoreMem 重发、ReduceModule、Retire、CoreMemCreditMonitor | 第 3 章 Router | 从属单元（8 个子块） |
| core | TS：CFG_REG、User_Match、DataIn_task_table、Stream_table、Task_ctrl、MU_Arb / VU_Arb、DTE_Arb、Credit_monitor、Task_done | 第 3 章 TS 任务调度器 | 从属单元（9 个子块） |
| core | RV core ×3：task_queue、kernel 执行器、dsa_iss、访存（sm_lsq / cm_lsq）、CSR | 第 3 章 RV Core | 从属单元 |
| core | DTE：Header Parser、Commit、TaskQueue ×4、Lane ×4（含 AGCU）、中间 Buffer、Completion RS、Hmem 与 LUT、topK 与 shareMem 写 | 第 4 章 DTE DSA | 从属单元（8 个子块） |
| core | MU：regfile、issue_q、gen_ep_info、agu ×3、ldq ×2、matrix exe、stq | 第 4 章 MU DSA | 从属单元（7 个子块） |
| core | VU：config_register、ISQ、pipe_ctrl 与 Scoreboard、LU、SU、SMUX / DMUX、VALU0 / VALU1 / VALU2 / VSFU、MEXE、SEXE、VRF / MRF / SRF、Profile | 第 4 章 VU DSA | 从属单元（11 个子块） |
| core | Core Mem、Matrix Mem、Share Mem 及各自的仲裁器 | 第 4 章存储子系统 | 从属单元 |
| 静态 | 拓扑与地址空间、切分与权重分配、路由表与任务链生成、参考实现 | 第 2 章切分、第 6 章编译器产物 | 静态（编译侧） |

### 本轮不建的部分

* Host CPU、Node CPU、Board CPU 的业务流控与退出、动态专家调度（第 2 章“业务流控与退出”）。LPU Dispatch 的派遣规则（所有 R core 有余量才派遣）放在 GPU / DPU 桩里。
* Debug Module、DTM、GDB。
* 异常、ECC、看门狗、功耗类机制（RV core 九类异常、MU Drain & Trap、VU error_code、TS Except_Check、DTE 首错保留、DIDT 分级、零输入门控、MU 与 VU 错峰）。各单元给这些机制留出状态位与接口名，本轮不实现其行为。
* RV core 的流水线细节：pc_gen、loop_bp、ITCM、decode、dispatch、gpr、SEU、DTCM。它们折算进 kernel 例程的指令预算。

***

## 建模方式

### 逐拍 cycle 模型

latch 的建模方法分三段：功能模型（指令加 oper）、factory 轨（oper 打到带宽加延迟的服务台）、逐拍 cycle 模型（手写模块，直接用 runtime 原语，不经 factory）。

Bach core 里的 TS 按 stream 年龄仲裁发射、三块存储按 bank 仲裁多 master、Router 有 credit 闭环与多播原子准入、VU 有 Scoreboard，`Factory` 那种带宽加延迟、乱序完成的服务台表达不了优先级排队与原子准入。因此 Bach 建成逐拍 cycle 模型，范本是 `src/module/llc.h` 与 rv32 流水线，遵循 `module_writing_guide.md` 的全部规则。

### 挂时钟粒度：独立物理节点挂时钟，从属单元跟着上级走

`tick` 决定 `ClkModule` 的构造函数要不要 `clk->Bind(...)`，也就是谁持有推进它的协程。挂时钟的模块自己有一个常驻协程；不挂的跟着上级节点的协程走，共用同一个 `ClockPtr`，行为同样是逐拍的。

> 能独立被调度的物理节点用 `tick=true`：Core、PCIe Switch、GPU / DPU 桩、出口桩、SCP 桩。
> 从属于某个节点、与它同拍工作的内部单元用 `tick=false`：Core 里的全部子块。
> 判据是它有没有一个不属于任何上级节点的物理位置。

`Core::Cycle()` 里按 stage 顺序调用各子块的 `Step()`。由此得到三件事：波形层次是 `chip<i>.core<j>.<块>.<子块>.<信号>`；常驻协程数等于节点数；子块的状态只被 `Core::Cycle()` 一个调用点触碰，因此内部用裸 `std::vector` 与标量即可，不需要任何同步。

**取舍**：这样分的第一条理由是保住 Core 内的同拍语义。存储的 bank 仲裁要在同一拍看到全部 master 的请求，TS 的 Task_done 要在同一拍看到 RV core、DSA、Router 三路 ack，Router 的 CoreStation 本拍投递的包 DTE 本拍就要处理。各子块各自挂时钟就必须走 `Fifo`，上拍写下拍读，一次搬运在收发两端多出四到八拍，这些拍不可消除。合在一个 `Cycle()` 里，同拍调用原样保留。
第二条理由是每拍开销。runtime 是严格 lockstep，每拍所有协程一起过一次 barrier，同步点数量等于挂时钟的模块数。协程池与物理线程数都可以调大，槽位上限是 65535，所以这不是容量上限，是开销：barrier 的成本随模块数线性增长，而线程数超过物理核数反而更慢。装配时 `RT::Reset(sub, co)` 两个数相乘要不小于挂时钟的模块数，少于模块数会直接死锁。框架不跳过空闲模块。48 chip 的 Node 按 Core 粒度是四百多个协程。

驱动粒度是构造参数。若 Core 级驱动在 48 chip 上开销过大，把 `Chip` 改成 `tick=true`、在它的 `Cycle()` 里顺序调各 Core 的 `Step()`，子块代码不动。

子块单测时给它传 `tick=true`，它就自己挂时钟单独跑，不需要拉起整个 Core。

从属单元把一拍的工作写在 `Step()` 里，`Cycle()` 只决定这一拍由谁让出：挂时钟时自己起手 `DelayCycle(1)` 再调 `Step()`，不挂时钟时由上级让出，`Step()` 里一次也不许让出。这条分工写在 `ip/sub_unit.h` 的基类里，两种驱动方式共用同一份单元代码。

### 等待改写成跨拍状态机

`Cycle()` 体内除起手的 `DelayCycle(1)` 外不得再 yield，否则该协程挂起会卡住 OldestStamp。硬件里的每一处等待都写成跨拍状态机，每拍判断一次：

| 单元 | 等待点 | 状态机 |
| - | - | - |
| TS | 等前序 task 完成、等 datain、等 credit、等 RV core 空闲、等 `raw ACCEPT` | 每个 stream 表项一个 `task_fsm`：IDLE → WAIT → RDY → INFLY → FINISH |
| RV core | 等 task、等 DSA 读寄存器返回、等 lsq 访存返回、等 dsa_iss 通道 | kernel 执行器的当前操作状态 |
| DTE | 等 Commit 配对资源、等 Lane 激活、等访存返回、逐拍搬运、等 Router 反压解除、等 Completion RS 的 Join | 每条 Lane 一个 Active Context 状态机，每个任务一个完成状态（queued → active → issue_done → drained → join_done → task_done） |
| MU | 等 issue_q 出队、等 Matrix Mem / Core Mem 返回、流水推进、等 stq 拼满 | 原语状态机，按 lane 深度与 outstanding 计拍 |
| VU | ISQ 排队、pipe_ctrl 合法性检查、Scoreboard 依赖、等 CM Load / Store 返回、宏指令重叠 | 宏指令状态机，最多两条相邻在飞 |
| Router | 等 VC credit、等 Stream 坑、等 Reduce credit、多播原子准入、Reduce 累加、AbsorbReissue | Message 状态机：Idle → Lookup → DeliverLocal / Forward / ReduceAccum / AbsorbReissue |
| 存储 | bank 冲突 | 仲裁器每拍 `TryGrant` 一次，被拒的请求下一拍再来 |

每个 Core 的状态机数量有界：stream 表项 16、DTE TaskQueue 4 × 16、VU 在飞宏指令 2、MU issue_q 16、Router 每方向 4 个 VC 各一个队首上下文。

`common/arbiter.h` 提供四种排队语义：

| 硬件里的资源 | 仲裁器 |
| - | - |
| 独占且先到先得（bank 端口、Lane、Xbar 出口） | 独占仲裁器，等待队列先进先出 |
| 按年龄或固定优先级（TS 三条发射通路、Cmem 的 MU > VU = DTE、DTE Commit 的 Bank0 > Bank1） | 优先级仲裁器，等待队列按优先级、请求拍、插入序排 |
| 计数额度（Stream 坑、VC credit、Reduce credit、TaskQueue 项数、DTE Buffer credit、CH1 outstanding） | 计数信号量，get 队列队首阻塞 |
| 多方向一次性扣减（多播原子准入、广播任务一次扣掉全部目的数、Commit 的三项配对接纳） | 全部方向都够才一起扣，任一不足整体不发 |

### 跨模块只走 Fifo

跨 ClkModule 的数据一律走 `Fifo<Pkt>`，Pkt 继承 `Logic`，字段在构造函数里 `Fields(...)` 注册。链路上的封包按 flit 传：

```cpp
class Flit : public Logic {
 public:
  Logic64 path_id, user_id, vc, flit_id, total_flits, is_head, is_tail, arrive_cycle;
  LogicPtr<Message> msg;   // 包头 + payload 字节，同一 Message 的全部 flit 共享
  explicit Flit(ClockPtr c) : /* ... */ { Fields(/* ... */); }
};
```

`LogicPtr` 让同一个 Message 的多个 flit 共享一份 payload，且不深拷字节。Message 里的 payload 是真实字节（bit 级比对的依据），Header 与 Payload 两条总线合成一个 flit 序列，首 flit 携带 Header。

`Fifo` 是单 producer 单 consumer，因此每条链路每个方向一个 Fifo；VC Release、Stream Release、Reduce Release 三种 credit 回传各自一个 Fifo。节点按接进来的线数开入口，一条线一个。Router 的上、下、左、右四个 RouterStation 各接一条线，Core 方向在 Core 内部同拍调用。同一拍多个入口都有包时按入口的登记顺序处理，顺序固定，所以一次 run 的结果与线程调度无关。

发包时在 `Cycle()` 体内现建一个 `Flit`，写完字段再 `Push`，不复用成员。`Latch::Get()` 对本协程本拍刚写的值有转发，所以现写现读拿得到；但没被本拍写过的字段会回落到上一拍的值，复用一个成员包会把上一拍的旧字段悄悄带出去。

### 可观测量与禁用清单

跨拍可读的计数、占用与标志一律用 `Logic64`，配一个 cycle 内的裸 `uint64_t` 累加器，在 `Cycle()` 末尾一次性 commit。`Logic64` 一拍只能 Set 一次，同拍多次 Set 触发断言。

`Logic64::Get()` 在主线程读到的是 t=0 的值。因此完成集合、结束时刻、credit 余额这些验收产物必须在协程内 snapshot 到普通变量，不能在 `JoinAll()` 之后直接读。

不出现 `std::mutex`、`std::lock_guard`、`std::condition_variable`、`std::atomic`。

### Core 一拍的 stage 顺序

`Core::Cycle()` 按“末级先做”排：先让已经在做的事完成，再让新的事派下去。计数 commit 在所有累加之后、发 Trace 之前。

| 次序 | stage | 子块顺序 | 它让什么落在同一拍 |
| - | - | - | - |
| 1 | Router | RouterStation ×4 收包与 VC 入队 → Xbar 仲裁转发 → ReduceModule 累加与输出 → CoreStation 投递与通知 → Retire 与 CreditMonitor | 本拍到点的 flit 交给 DTE 或 ReduceModule，`reduce_done`、trigger、credit 脉冲发出 |
| 2 | 存储 | Cmem 仲裁 → Mmem 仲裁 → Share Mem | 访存到点的返回数据、释放 bank 端口，本拍排在后面的请求就能接上 |
| 3 | DSA | DTE（Lane 完成 → Completion RS → Commit → Header Parser）→ MU（stq → matrix exe → ldq → agu → issue_q → regfile）→ VU（SU → 执行单元 → LU → pipe_ctrl → ISQ → config_register） | 搬完或算完的 ack 出去，DTE 写 shareMem 后的通知出去；每个 DSA 内部也按末级先做 |
| 4 | RV core ×3 | 访存返回 → kernel 执行器 → dsa_iss → task_queue | 看到本拍 DSA 的读返回与访存返回，执行到 `task_done` 的通知出去，新的 DSA 配置指令下发 |
| 5 | TS | Task_done → Task_ctrl → DTE_Arb / MU_Arb / VU_Arb → Credit_monitor → User_Match 与 DataIn_task_table → CFG_REG | 把本拍 RV core、DSA、Router 三路 ack 一起看进来，推进各 stream 的 `task_fsm`，派下一批 task |

排在最后的 TS 看得到本拍全部完成，所以依赖链不掉拍；代价是它本拍派下去的 task，RV core 要到下一拍才开始执行。两者只能取一个：依赖链掉拍会直接改变 task 之间的先后与退休时刻，而派发晚一拍相对几十拍的启动开销可以忽略。

Router 排在 DTE 之前是同一个取舍：本拍投递到的包 DTE 本拍就处理，而 DTE 本拍交出去的包 Router 下一拍才仲裁。

***

## 模型结构

### 目录按对象清单

沿用 `src/bach/` 现有的目录组织。`ip/` 下每个头文件对应对象清单里的一个子块；Python 版模拟器遗留的单元（`credit_unit.h`、`moe_bitmap.h`、`compute/`、`eth_switch/`、`external/phase*`、`dispatcher.h`）保留不动，新单元不依赖它们。

```
src/bach/
  ip/
    sub_unit.h                     从属单元基类，Step 与 Cycle 的分工
    node_context.h                 每个节点都有的编号、参数表与记录器
    route_config.h                 外部节点与跨 chip 网关的坐标换算表
    wiring.h                       接一条双向物理链路（Fifo 对 + 三种 release Fifo）
    system.h                       持有全部节点，做跨 chip 与阵列外的接线
    link/
      link.h                       链路模型：带宽、延迟、arrive_cycle 计算
    external/
      gpu.h                        GPU / DPU 桩：两层 credit、自定义包头、retired 汇总、LPU Dispatch 派遣
      out.h                        出口桩：收结果、按 (gpu_id, token_id) 与参考实现比对
      scp.h                        SCP 桩：boot 序列、ctrl_noc 配置事务
    node/
      pcie_switch.h                双路 x16、组播复制、按最慢收端反压
    chip/
      chip.h                       2×5 阵列、Harvest mask、四个边界 core 的 C2C 端口、ctrl_noc
      core/
        core.h                     节点，tick=true，按 stage 顺序调各子块
        core_context.h             各子块共用的只读上下文（core id、角色、参数表）
        ports.h                    子块之间的同拍调用口（纯虚接口）
        ctrl_noc_endpoint.h        ctrl_noc 在 core 内的落点：寄存器写入分发
        router/
          router_table.h           RouterTable 与 CSR、Credit Bypass Route、多副本提交状态机
          router_station.h         ×4：Header Parser、VC Buffer、Packet Context、Stream Resource Table、VC Credit
          xbar.h                   按输出仲裁、多播全有全无、入口锁定
          core_station.h           HeaderFIFO、OutputBuffer、三态准入、TS 通知、出 core VC 流控
          coremem_reissue.h        stallWay、Bypass 映射成进核加出核、同 VC 保序
          reduce_module.h          16 用户上下文、RMW 累加、Reduce credit、输出队列
          retire.h                 Retire 广播与两处回收
          credit_monitor.h         监听事件队列 16 项全相连
        ts/
          cfg_reg.h                task_chain 64 项、datain_task、stream_num、Task LUT
          user_match.h
          datain_task_table.h      Depth-1 Hold
          stream_table.h           16 项 FIFO、task_fsm、六个写端口
          task_ctrl.h              SKIP_MASK 一拍跳过、原子安装
          dte_arb.h  mu_vu_arb.h
          credit_monitor.h         Reissue 唤醒、Head-only 退休、credit 申请
          task_done.h              七路完成合流
        rv_core/
          rv_core.h                task_queue、kernel 执行器、dsa_iss、访存、CSR
          kernel_api.h             kernel 例程可调用的操作集
          kernels/                 各 kernel 例程（bcore_datain、check_flag、broadcast、weights loader、计算 core 各 task）
        dte/
          header_parser.h  commit.h  task_queue.h  lane.h  agcu.h  buffer.h  completion_rs.h  hmem.h
        mu/
          regfile.h  issue_q.h  gen_ep_info.h  agu.h  ldq.h  matrix_exe.h  stq.h
        vu/
          config_register.h  isq.h  pipe_ctrl.h  lu.h  su.h  mux.h  valu.h  vsfu.h  mexe.h  sexe.h  regfiles.h  profile.h
        memory/
          core_mem.h  matrix_mem.h  share_mem.h  bank_arbiter.h
  common/
    flit.h  message.h              封包与 Message
    params.h                       全部参数的唯一出处
    regmap.h                       DSA 寄存器地址映射（VU 按 MAS；MU、DTE 为临时映射）
    arbiter.h                      四种仲裁器
    numeric/                       FP8_e4m3 / MXFP8 / MXFP4 / BF16 / FP32 的编解码、舍入、CSA 累加顺序
  tables/
    router_table.h  task_chain.h  header_tables.h  loader.h      编译侧产物的读入
  observer/
    span_recorder.h  jsonl_writer.h
  sim/
    build.h                        从编译侧产物装配出整套硬件
    completion.h                   完成判据与全局生命周期看门狗
  reference/
    ffn_reference.py               参考实现（与 numeric/ 同一套累加顺序）
```

子块之间是同拍调用，所以口是一组纯虚方法而不是 Fifo。抽接口而不是互相持有具体类型，是因为调用是双向的：TS 派 task 给 RV core，RV core 配置 DSA，DSA 完成后又要回调 TS。接口把回调那一侧收窄成被调用方真正需要的能力，装配顺序也就不受构造顺序牵制。

`test/bach/ip/` 按 `ip/` 的一级镜像分目录，每个子块旁边有它自己的测试。

### 物理归属

**Router 属于 Core。** Core 构造时创建各子块，Router 的八个子块是其中一组，每个 Core 一份。片内 mesh 的连线动作在 Chip 里做，但被连的端口长在 RouterStation 上。坏核只构造 Router 的子块，不构造 TS、RV core、DSA 与存储。

**PCIe Switch 属于 node。** 它是片外的交换节点，坐标是不与核阵列重叠的锚点；端口由拓扑起名，核那一侧仍是四个方向之一，两侧各用各的端口标识，接线时对接。

**GPU / DPU 桩、出口桩、SCP 桩属于 system。** 它们的坐标不在核阵列内，登记在外部节点表里。GPU / DPU 桩与出口桩挂在 PCIe Switch 上，各自带一个路由器：包先进自己那个路由器的本地口，由它按链路的带宽与延迟送到网关核；回来的包也在这个路由器上落地重组，再交给设备本身。SCP 桩每 chip 一个，只接 ctrl_noc。

### 同拍调用口与 Fifo 口

| 口 | 形式 | 一拍最多 |
| - | - | - |
| RouterStation ↔ 相邻 Router 的 RouterStation | Fifo（数据）+ 三个 release Fifo | 1 flit + 各 1 笔 release |
| RouterStation ↔ PCIe Switch | 同上 | 同上 |
| Router 子块之间、Router ↔ CoreStation ↔ DTE | 同拍调用 | Xbar 每出口 1 flit |
| CoreStation → TS（trigger）、Router → TS（credit 脉冲、reduce_done）、TS → Router（credit 请求、stream_credit_return、Retire） | 同拍调用 | 各 1 笔 |
| TS ↔ RV core（task 下发 / task_done）、TS ↔ DSA（done） | 同拍调用 | 每通路 1 笔 |
| RV core → DSA（配置 / trigger / 读） | 同拍调用 | 1 条 |
| DSA / RV core ↔ 存储 | 同拍调用（请求）+ 延迟队列（返回） | 按端口数 |
| ctrl_noc → core 内寄存器 | Fifo（SCP 桩到 Chip）+ 同拍分发 | 32 bit |

***

## 各单元的建模规格

每个子块固定五段：对应设计（指向第 2 至 4 章的小节）、状态、接口、机制覆盖、参数与简化。机制覆盖表的“落点”是模型里承载该机制的函数或 stage；“用例”是 `test/bach/ip/` 下的测试名。设计未给值的参数写默认值并标“待定”，全部待定值汇总在本章末尾。

### 1　GPU / DPU 桩

**对应设计**：第 6 章“GPU → Bach 的两层 credit 反压”“运行时约定”；第 2 章“Node 组成”“EP Reduction 死锁与 LPU Dispatch 派遣机制”。

**状态**

| 项 | 内容 |
| - | - |
| 注入表 | 每个 `(gpu_id, token_id)` 的注入拍与 6368 B 级联包（激活 FP8 6144 B + scale FP32 192 B + 专家序号 Int16 16 B + 专家权重 BF16 16 B） |
| 两层 credit | `buffer_depth_tokens[g]`、`buffer_used[g]`、`pool_total`、`inflight_bach[g]`、`grant_tokens[g]`；序号位宽 W，比较用模 2^W 差值 |
| 派遣余量 | 每个 R core 的 reduce-buffer 余量计数（EP6+TP8 用） |
| retired 汇总 | 各收端上报的 `retired_token`，取 min |
| 包头计数器 | per-GPU 自增 256 项 |

**接口**：出口经自带路由器接 PCIe Switch（Fifo）；收端 retire 信息经反向数据通路回到桩；与出口桩共享完成集合。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 第一层 GPU 本地门控 `buffer_used < buffer_depth_tokens` | `Gpu::Step()` 注入判定 | `gpu_credit_local` |
| 第二层 Bach 全局 `pool_avail = pool_total − Σ inflight_bach`，公平发 grant | `Gpu::Grant()` 轮询 | `gpu_credit_pool` |
| 两道闸门都开才流；1 credit = 1 token | 同上 | 同上 |
| 序号回绕模 2^W 比较 | `common/seq.h` | `seq_wrap` |
| DPU 自定义包头 `gpu_id(8) + token_id(16)`，Payload ≤ 64 KB | `Gpu::Encapsulate()` | `gpu_header` |
| 完成通知：Router 侧按已接收字节数与包头 payload 大小判 token 收完 | RouterStation 的 Header Parser 上报，桩侧记账 | `token_complete` |
| 组播反压按最慢收端聚合，retired 取 min | `Gpu::CollectRetired()` | `gpu_retired_min` |
| LPU Dispatch 派遣：所有 R core 有余量才派，派时各减 1，最终 R core 返回后各加 1，顺序调度 | `Gpu::Dispatch()` | `dispatch_reduce_slots` |
| B core Matrix Mem 槽位 ≥ 32 × 26 = 832 | 参数校验 | `bcore_slots` |

**参数与简化**：ETH 注入延迟 3 μs 折 3000T，50 GB/s 每口；输入 PCIe x16 54.4 GB/s。桩不算 Attention，按注入表的时刻发包；GPU 数量与每 GPU 的 batch 由输入给出。

### 2　链路与 PCIe Switch

**对应设计**：第 2 章“互连、带宽、延迟”“已知板级问题”；第 5 章延迟表。

**状态**：每条链路的 `last_busy_until`；PCIe Switch 每端口的输出队列与组播复制表。

**接口**：链路两端各一个 Fifo；PCIe Switch 端口按名字开。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 链路占用与到达时刻：`arrive_cycle = max(now, last_busy_until) + ceil(size / bw) + latency` | `Link::Send()` | `link_arrive` |
| 片内 R2R 256 B/T、40T | 参数 | `link_r2r` |
| PCIe ↔ Router 128 B/T，左右 10T + 25T、上下 10T + 50T | 参数 | `link_pcie_router` |
| chip 间 Router 到 Router 400T；PCIe C2C 64 GB/s、300 ns | 参数 | `link_c2c` |
| 双路 x16 各自独立计时，不保序 | 两条 `Link` 实例 | `pcie_two_lanes_unordered` |
| 组播：一份数据复制到多个收端 Matrix Mem，反压按最慢收端 | `PcieSwitch::Step()` | `pcie_multicast` |
| 输出 x32 108.8 GB/s | 参数 | `link_out` |

**参数与简化**：组播能力是开关，默认关（第 6 章“先假设 PCIe 没有组播能力”）。

### 3　Chip、SCP 与 ctrl_noc

**对应设计**：第 2 章“Chip 与 Harvest”“Boot”“初始化配置”“weights 加载模式”；第 3 章“Bach Core 顶层”；第 6 章“部署阶段”。

**状态**

| 项 | 内容 |
| - | - |
| 阵列 | 2×5 row-major 编号；边界 core0 = North、core4 = East、core5 = West、core9 = South |
| Harvest mask | 每 chip 的坏核位图；坏核只构造 Router 子块 |
| 逻辑 ↔ 物理映射 | 由编译侧给出，`(tray, chip, logical_core)`，逻辑 8 = B core / R core |
| ctrl_noc | 32 bit/T 的配置总线；SCP 桩发出的写事务按地址分发到 TS CFG_REG、RouterTable CSR、DSA 寄存器、Share Mem、RV core 的 kernel 表 |
| 模式 | weights 加载模式 / 业务模式，由 TS 的 `trigger_task_chain_en` 与 RouterTable 内容体现 |

**接口**：Chip 持有 10 个 Core，接 mesh；四个边界 core 各接一条 C2C 链路；SCP 桩 → Chip 一条 Fifo。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 2×5 阵列与边界 core 连接规则 | `Chip::Wire()` | `chip_mesh_2x5` |
| Harvest disable mask 只关 EngineNode，RouterNode 仍可用 | `Chip::Build()` | `harvest_router_only` |
| 坏核不能承担 compute / B core / R core；可承担转发、多播、router reduce | 编译侧校验 + `Chip::Build()` 断言 | `harvest_roles` |
| 逻辑 core 8 = special core，special 优先四步 | 编译侧 | `logical_map` |
| core id 由 SCP 经 ctrl_noc 读 MMIO，不可修改 | `CtrlNocEndpoint` 只读寄存器 | `core_id_readonly` |
| SCP boot 序列：自启动 → PCIe 训练 → 顺序配 core0～core7 | `Scp::Step()` 状态机 | `boot_sequence` |
| Core 内 boot：ITCM 装载（kernel 表绑定）→ 三个 RV core 进 wait → ready 全高 → 开放业务接收 | `Scp::Step()` + `Core::Ready()` | `core_boot` |
| 初始化六步：firmware → DTE bootloader → 解复位 → TS 任务链 → router 路由表 → kernel | `Scp::Step()` 按序发配置事务 | `init_six_steps` |
| Router 多副本提交完成后软件再写 DTE 与 ReduceModule 副本 | `Scp::Step()` 等 CSR Status | `router_table_three_copies` |
| weights 加载模式：1 条 P2P path、datain pc 指向 weights loader、`trigger_task_chain_en = 0`、四步加载 | `Scp` 配置 + 真实 datain 任务 | `weights_load` |
| 先发最远路径的数据 | GPU 桩注入表顺序 | `weights_far_first` |
| 切到业务模式：换路由表、datain pc、`trigger_task_chain_en = 1`、DSA 静态配置 | `Scp::Step()` | `switch_to_business` |
| 地址空间视野 | `CtrlNocEndpoint` 与各 RV core 访存的地址分发表 | `address_map` |

**参数与简化**：SCP 桩按 32 bit/T 发事务，每笔事务一拍；SCP 自身不计算。ctrl_noc 的广播作为开关，默认关。ITCM 装载不搬字节，只把 kernel 例程表绑定到 RV core，占用的拍数按 kernel 字节数除以 32 bit/T 计。

***

### 4　Router

Router 建成八个子块，每个 Core 一份，坏核也有。全部子块在 `Core::Cycle()` 的第 1 个 stage 内按“RouterStation 收包 → Xbar → ReduceModule → CoreStation → Retire 与 CreditMonitor”的顺序 `Step()`。

#### 4.1　RouterTable 与 CSR

**对应设计**：第 3 章“路由表与三方副本”“业务 Credit 的静态 Bypass”。

**状态**：以 `PathID` 为索引的表项（`curVC`、`directionMask`、`nxtVC`、`streamNeedMask`、`operation`、`stallWay`、`reducePrecision`）；提交状态机（空闲 → 逐副本写入 → 汇总 → 报告完成）；Credit Bypass Route[i]（每个 Stream / Reduce credit 输入端口一个静态方向 Mask）；Readback 视图。

**接口**：ctrl_noc 写（Write Data、Commit、Status、Bypass Route、Readback）；RouterStation、CoreStation、ReduceModule、DTE 的查表调用（同拍）。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 路由表查询、统一查询语义（转发与重发都用 PathID 查表） | `RouterTable::Lookup()` | `rt_lookup` |
| 静态表，不存动态状态 | 数据结构 | — |
| 多副本原子提交：更新期间数据面仍用旧表，全部副本写完才报告完成 | `RouterTable::Commit()` 状态机，副本数与每副本写入拍数为参数 | `rt_commit_atomic` |
| 提交互斥、外部副本软件同步 | `Commit()` 拒绝重叠提交；DTE 与 ReduceModule 副本由 SCP 桩另写 | `rt_commit_reject`、`router_table_three_copies` |
| Credit 路由修改约束（无在途 release 才可改） | `SetBypassRoute()` 断言 | `rt_bypass_route_change` |
| 业务 Credit Bypass：release 按静态 Mask 单播 / 多播，UserID 与类型不变 | `RouterTable::BypassRoute()`，RouterStation 转发 release 时调用 | `credit_bypass_multicast` |

**参数与简化**：表项数 64（待定）；副本数 4 + CoreStation 1（待定）；每副本写入 1 拍（待定）。

#### 4.2　RouterStation（×4）

**对应设计**：第 3 章“Router 的子模块”“两级流控：Stream 与 VC Credit”“交织规则”“同 VC 保序”。

**状态**

| 项 | 内容 |
| - | - |
| VC Buffer ×4 | 每 VC 一个 flit 队列，深度 `VC_BUF_DEPTH` |
| Packet Context ×4 | 每 VC 队首 Packet 的 VC、目标方向 Mask、剩余长度、包边界 |
| Stream Resource Table | 本方向的下游 Stream 授权：UserID × 目标方向 → 已授权；项数 `STREAM_SLOTS_PER_DIR` |
| 下游 Stream 映射表 | 每个下游方向，UserID 是否持有资源 |
| VC Credit Counter | 下游方向 × VC 的可用 flit 数 |
| 同 VC 保序标记 | 该 VC 有未完成的 CoreMem 重发 Packet |

**接口**：入 Fifo（相邻 Router / PCIe）；出 Xbar 请求（同拍）；VC Release、Stream Release、Reduce Release 三个 Fifo 的收发；RouterTable 查表。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 包头检测与解析（PathID、UserID、包长、operation、方向 mask、VC） | `RouterStation::ParseHeader()` | `rs_parse` |
| VC 分配：flit 携带 VC 号入对应 Buffer；`curVC` 用于入口 | `RouterStation::Enqueue()` | `rs_vc_assign` |
| 资源与阻塞解析：按表项解析各方向 Stream / VC / Reduce 需求 | `RouterStation::Resolve()` | `rs_resolve` |
| 全方向满足才发：所有需求方向的 VC credit、Stream、Reduce 都满足才进仲裁 | `RouterStation::Ready()` | `rs_all_dirs` |
| VC credit 扣与还：发前查、发后扣，下游 flit 离开 VC 经 release 通道还 | `Ready()` / `Xbar` 握手后 `Consume()` / `OnVcRelease()` | `rs_vc_credit` |
| 独立信用：每下游方向每 VC 独立计数 | 数据结构 | `rs_vc_independent` |
| Stream 唯一状态、Stream 授权（UserID + PathID + 方向）、禁止超额 / 重复 | `StreamResourceTable::Acquire()` | `rs_stream_grant` |
| Stream 释放：携 UserID 的 release 回收 | `OnStreamRelease()` | `rs_stream_release` |
| 下游映射表与 DTE 的 User Resource Cache Table 行为一致 | 两者共用 `StreamLedger` 类型，DTE 持副本 | `rs_dte_ledger_consistent` |
| 多播原子准入：每 flit 全部目标同时取得资源才发 | `Ready()` 用多方向一次性扣减仲裁 | `rs_multicast_atomic` |
| Router 间交织：flit 边界可切换 Packet | 每 VC 的 Packet Context | `rs_interleave` |
| 整包粒度仲裁（只对包头仲裁）与模块入口锁定 | Xbar 一节 | `xbar_lock` |
| 反压稳定 | `Fifo` 语义 | — |
| 同 VC 保序：有未完成重发 Packet 时后续不得越过 | `Ready()` 查保序标记 | `rs_vc_order` |
| 业务 Credit Bypass 与多播 | `ForwardRelease()` | `credit_bypass_multicast` |
| 输出移位拼接 | 不建：flit 定长 256 B，尾 flit 带有效字节数 | — |

**参数与简化**：`VC_BUF_DEPTH` 32 flit（8 KB，待定）；`STREAM_SLOTS_PER_DIR` 16（待定）；VC credit 初值 = 下游 `VC_BUF_DEPTH`（待定）；每拍每方向 1 flit。

#### 4.3　Xbar

**对应设计**：第 3 章“InterconnectMatrix”“交织规则”。

**状态**：每个输出方向一个独占仲裁器；Core 与 ReduceModule 两个端点的“当前 Packet 锁定”；本拍已握手集合。

**接口**：五个输入（4 个 RouterStation + CoreStation 的出 core 通路）、六个输出（4 方向 + Core + ReduceModule），同拍调用；ReduceModule 的输出重新进入输出仲裁。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 按输出独立仲裁，无关输出不串行化 | `Xbar::Step()` 逐出口 `TryGrant` | `xbar_per_output` |
| 五输入并行 | 同上 | `xbar_parallel` |
| 多播复制、全有全无握手 | `Xbar::Step()` 多方向一次性扣减 | `xbar_multicast_all_or_nothing` |
| Header / Payload 一致转发 | 单一 flit 序列 | — |
| Reduce 路径：operation 为 Reduce 导向 ReduceModule，结果回注输出 | `Xbar::Route()` | `xbar_reduce_path` |
| 入口锁定与释放：进 Core / ReduceModule 后锁定到尾 flit，只约束该组合 | `Xbar::Lock()` | `xbar_lock` |
| 握手后统一更新 credit 与上下文 | `Xbar::Commit()` | — |
| 仲裁算法 | 轮询（待定，原文未给） | `xbar_round_robin` |

**参数与简化**：每出口每拍 1 flit。

#### 4.4　CoreStation

**对应设计**：第 3 章“进 core 与出 core”“Core 准入三态”“搬运接口契约”。

**状态**：HeaderFIFO（深度 `HDR_FIFO_DEPTH`）；OutputBuffer（flit 队列，深度 `OUT_BUF_DEPTH`）；本级 Stream 表（进 core 表，16 项，与 TS 的 stream_table 同逻辑）；出 core 方向的 4 个 VC credit。

**接口**：入 Xbar（同拍）；出 DTE：Header 读口（DTE core 读包头、写 1 弹出）与 DataIn 流（每拍一 flit）；入 DTE：DataOut 流（每拍一 flit，受 VC credit）；TS：trigger（UserID、reissue 标志、p2p_reissue 标志）、完成信息（UserID + PathID）；Release / Retire 出口。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| Core 准入三态：已分配直接进；未分配有空项则占用；无空项该 VC 不进 Core 但可收数 | `CoreStation::Admit()` | `cs_admit_three_states` |
| Core 容量保证：过 Stream 检查后不查 Core 方向 VC credit | `Admit()` | `cs_no_vc_check` |
| 两表一致申请：进 core 表与 TS stream_table 同逻辑，通知必可接收 | `CoreStation` 与 `StreamTable` 共用分配算法 | `cs_ts_consistent` |
| 按包头顺序通知 TS | `CoreStation::NotifyTs()` 按 HeaderFIFO 顺序 | `cs_notify_order` |
| 包头弹出：DTE core 读完写 1 | `CoreStation::PopHeader()` | `cs_pop_header` |
| 搬运接口契约：反压不丢不重不跨包 | DataIn / DataOut 流的 valid/ready 语义 | `cs_backpressure` |
| 整包进 Core：不交织 | Xbar 锁定 | `xbar_lock` |
| 出 Core 按 VC 流控：按 Header VC 写目标 VC，有空间才允许 Core 发 | `CoreStation::AcceptOut()` | `cs_out_vc` |
| 出 Core 前置申请：下游 Stream / Rmem 资源先申请到才发，否则在 DTE 的 PendingTaskQ 等待 | DTE Commit 一节 | `dte_pending_taskq` |
| DTE 侧 VC Buffer：4 个，单 VC 阻塞不影响其他 | DTE Lane 一节 | `dte_vc_buffers` |
| 双向完全并行 | 进出两条独立通路 | `cs_full_duplex` |
| token 收完判定：已接收字节数 = 包头 payload 大小 | `CoreStation::OnTail()` | `token_complete` |

**参数与简化**：`HDR_FIFO_DEPTH` 16、`OUT_BUF_DEPTH` 32 flit（待定）；Header 读为同拍调用，不计 AXI 事务拍数（待定）。

#### 4.5　CoreMem 重发

**对应设计**：第 3 章“阻塞处理”“同 VC 保序”；第 6 章“阻塞重传四步”。

**状态**：每 VC 的“有未完成重发 Packet”标记；落 Core Mem 的 Packet 登记（UserID、PathID、size、Core Mem 地址）。

**接口**：RouterStation 的阻塞判定（同拍）；CreditMonitor 注册；TS trigger 携带 reissue 标志；DTE 的重发任务走正常出 core 通路。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 阻塞缓存选择：按 stallWay 留 VC 或转 Core Mem | `RouterStation::OnBlocked()` | `reissue_stallway` |
| Bypass 映射成进 core + 出 core | `CoreMemReissue::Absorb()` | `reissue_absorb` |
| Packet 保存与重查：只存 Packet，重发时重查表 | `Absorb()` / DTE 重发任务查 RouterTable | `reissue_relookup` |
| 同 VC 保序 | RouterStation `Ready()` | `rs_vc_order` |
| 完成通知：直接或重发完成后向 TS 返 UserID + PathID | `CoreStation::Complete()` | `reissue_complete_notify` |
| 重发注册监听 | CreditMonitor 一节 | `cm_reissue_register` |
| 软件须预留 Core Mem 空间并在任务链安排 reissue 任务 | 编译侧产物校验 | `reissue_reserved` |

#### 4.6　ReduceModule

**对应设计**：第 3 章“ReduceModule”“Reduce Credit”；第 6 章“Reduce 阶段”。

**状态**

| 项 | 内容 |
| - | - |
| User Context Table | 16 项：UserID、当前 Packet 状态、各方向输入完成位、输出状态、Retire 标记 |
| Reduce Context SRAM | 16 × 16 KiB，FP32 |
| RouterTable 副本 | 输出方向、下一跳 VC、operation、输出精度 |
| Downstream Reduce Credit Map | UserID × 目标方向 → 剩余 credit（flit 粒度） |
| 输入仲裁器 | 三路输入争 bank 与加法端口 |
| 输出队列 | 深度 `REDUCE_OUT_DEPTH` |

**接口**：入 Xbar（锁定到尾 flit）；出 Xbar（结果重新仲裁）；Reduce Release Fifo 的收发；向 Core 返回 UserID（reduce_done → TS）；接收 Retire 广播。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 上下文分配：同 User 同 Packet 首份输入分配并写入 FP32 | `ReduceModule::OnFlit()` | `rm_alloc` |
| RMW 原位累加；上下文保护（未输出前不覆盖） | `ReduceModule::Accumulate()` | `rm_rmw`、`rm_protect` |
| 必须执行 Reduce：资源不可用反压，不降级转发 | `OnFlit()` 返回 not-ready | `rm_no_bypass` |
| 计算精度：BF16 → FP32，累加 FP32，输出可配 FP32 / BF16；FP32 ↔ BF16 双向转换 | `common/numeric` | `rm_precision_bits` |
| 输出前置检查：全部方向输入完成后进输出队列；发前查目标 VC credit 与下游 Reduce credit；每完成一 flit 可发一 flit | `ReduceModule::Emit()` | `rm_emit_per_flit` |
| Reduce credit 逐 flit 扣，按 release(UserID) 恢复；Router 不维护 | `Emit()` / `OnReduceRelease()` | `rm_credit` |
| Credit 释放：输出 flit 被接受后产生携 UserID 的 release | `Emit()` 握手后 | `rm_release` |
| 本级 credit（DTE 侧）：DTE 持每项 credit，整包够才发；每 Reduce 一 flit 还一个 | DTE Commit 与 `ReduceModule::Emit()` | `dte_reduce_credit` |
| ReduceModule 间双 credit（Reduce credit + VC credit） | `Emit()` | `rm_dual_credit` |
| Entry 分配与删除：创建 Stream 时分配，Retire 且 credit 归位时删除 | `OnStreamCreate()` / `Retire` 一节 | `rm_entry_lifecycle` |
| 完成返回 UserID | `ReduceModule::Done()` → TS `reduce_done` | `rm_done` |
| 三路输入仲裁 | 独占仲裁器，轮询（待定） | `rm_arb` |
| 吞吐：256 B/cycle、1 个加法端口，N 字节耗 `ceil(N / 256)` 拍，同地址冲突串行化 | `Accumulate()` 计拍 | `rm_throughput` |
| reduce 加法跳过软件辅助信息 16 B | `Accumulate()` 偏移 | `rm_skip_sw_header` |
| Rmem 与 CoreMem 共用同一套 credit（单账） | Retire 与 Stream 表 | `rm_single_ledger` |

**参数与简化**：每用户 Entry credit = 16 KiB / 256 B = 64 flit（待定）；bank 数 4（待定）；RMW 流水 2 拍（待定）；`REDUCE_OUT_DEPTH` 8（待定）；`operation` 的 Reduce0 / Reduce1 / Reduce2 按“源分量 / 中继累加 / 最终汇聚”实现（待定，第 8 章）。

#### 4.7　Retire

**对应设计**：第 3 章“用户退休”。

**状态**：Router 侧 Stream 授权表项；ReduceModule 侧 Retire 标记与 credit 归位计数。

**接口**：TS → Router 的 Retire 广播（同拍，携 UserID）；`stream_credit_return` / `stream_return_accepted`。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| Core 的保证：全部进出搬运完成且不再发起后才 Retire | TS Credit_monitor 的 Head-only 退休条件 | `retire_core_guarantee` |
| Retire 广播到 Router 与 ReduceModule | `Retire::Broadcast()` | `retire_broadcast` |
| Router 回收：停新发送，删全部 Stream 表项 | `Retire::OnRetire()` → 各 RouterStation | `retire_router` |
| ReduceModule 延迟回收：credit 全部恢复初值后删映射 | `ReduceModule::OnRetire()` | `retire_reduce_delayed` |
| 向所有相邻上游发 release；坏核透传 | `ForwardRelease()` | `retire_release_upstream`、`badcore_release_passthrough` |

#### 4.8　CoreMemCreditMonitor

**对应设计**：第 3 章“数据包监听机制”。归属按 Router 建。

**状态**：监听事件队列 16 项全相连（UserID、StreamID、TaskID、PathID、方向、VC 需求、Stream 需求）；各方向 CoreMemCredit 视图。

**接口**：TS 注册（同拍）；反向通知 TS（credit 脉冲，同拍）。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 注册：TS 任务前序无依赖时登记 | `CreditMonitor::Register()` | `cm_register` |
| 资源查询与即时通知 | `CreditMonitor::Step()` 查 RouterTable 与各 Station 的 credit | `cm_immediate` |
| 挂起监听，满足后通知 | `Step()` 每拍重查 | `cm_pending` |
| 最老 StreamID 仲裁 | `Step()` 按 StreamID 年龄选一个 | `cm_oldest` |
| 重发任务注册 | `Register()` 带 reissue 标志 | `cm_reissue_register` |
| 同 VC 保序 | RouterStation | `rs_vc_order` |
| credit 广播参与仲裁 | 各 Station 直接读同一份计数 | — |

**参数与简化**：每拍最多通知 TS 一笔。

#### 4.9　坏核

坏核只有 Router 的八个子块，其中 CoreStation 永远不准入、ReduceModule 不累加、CreditMonitor 空转。数据面只按 RouterTable 做 router→router 转发，不查 Stream、不占坑；控制面把 Stream / Reduce release 按 Credit Bypass Route 透传。用例：`badcore_transit`、`badcore_release_passthrough`、A15 / A16。

### 5　TS 任务调度器

TS 建成九个子块，在 `Core::Cycle()` 的第 5 个 stage 内按“Task_done → Task_ctrl → 三个 Arb → Credit_monitor → User_Match 与 DataIn_task_table → CFG_REG”的顺序 `Step()`。Stream_table 是被动的数据结构，六个写端口在同一拍内按上述顺序被写。

#### 5.1　CFG_REG 与 Task LUT

**对应设计**：第 3 章“任务链与配置寄存器”。

**状态**：`TASK_CHAIN_0～63`（64 bit 位域：TASK_PC、TASK_SEND_UNIT、TASK_RECV_UNIT、SELF_START、WAIT_WAKE、TASK_Broadcast_REISSUE、TASK_P2P_REISSUE、TASK_REDUCE、TASK_CREDIT_EN、TASK_EXE_MASK、TASK_PATH_ID、TASK_END、TASK_VALID）；`DATAIN_TASK`（TASK_PC、TASK_UNIT = DTE、WEIGHTS_MODE、TASK_VALID）；`STREAM_NUM`、`TS_INIT_FINISH`、`TS_STATE`、`CORE_TYPE`、`B_core_direction`、`trigger_task_chain_en`；派生视图 `DATA_IN_MASK` / `REISSUE_MASK` / `END_MASK`（各 64 bit）。

**接口**：ctrl_noc 写；各子块的 `task_lut_query`（固定下一拍返回）。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| TASK_VALID 自动置位 | `CfgReg::Write()` | `ts_cfg_valid` |
| 配置合规检查（多笔 end、不连续 valid、多笔 self_start、普通 core 出现 self_start）写 `TS_STATE` | `CfgReg::OnInitFinish()` | `ts_cfg_check` |
| WEIGHTS_MODE / `trigger_task_chain_en = 0` 不启动任务链 | `DataInTaskTable` 建表判定 | `weights_load` |
| Task LUT 一拍返回 | `CfgReg::Query()` 延迟队列 1 拍 | `ts_lut_latency` |
| 软件列名对照（task_unit / task_recv_unit / B_reissue / P2P_reissue / task_reduce_iss / credit_en / exe_mask / path_id / self_start / wait_wake / dsa_en / task_end） | `tables/task_chain.h` 读入时映射 | `task_chain_load` |

**参数与简化**：`exe_dest`、`task_group_id` 两列硬件位域没有，读入时保存在软件侧属性里供 DP+P2P 场景的“是否计算”匹配用（待定）。

#### 5.2　User_Match 与 DataIn_task_table

**对应设计**：第 3 章“建表”“异步 DataIn 机制”。

**状态**：User_Match 无独立表，全相连比较 stream_table 的 user_id 列；DataIn_task_table 1 项 Hold（valid、stream_id、user_id、task_pc = Global DataIn PC）。

**接口**：CoreStation 的 trigger（UserID、reissue、p2p_reissue）与 ready 反压；`stream_create_request` / `response`；`datain_offer` / `datain_pop` 给 DTE_Arb。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 新旧用户判定；老用户复用 SID 且不改当前任务 / FSM / bitmap / reissue | `UserMatch::Step()` | `ts_user_match` |
| datain 不建表（B / R core） | `UserMatch` 按 `CORE_TYPE` 分支 | `ts_datain_no_create` |
| 注册与反压：Hold 空则登记，满则反压 Router | `DataInTaskTable::Step()` | `ts_datain_hold` |
| 新 Stream 原子创建：一次写 Task0 全部属性；DataIn Task0 初始 WAIT，self_start Task0 初始 READY | `StreamTable::Create()` | `ts_stream_create` |
| 两种 PC 语义：Task0 PC 入 Stream，Global DataIn PC 入 Hold | `DataInTaskTable` | `ts_two_pcs` |
| DataIn 独立下发，不走 READY → INFLY，DTE 接收不改 Stream | `DteArb` 的 DataIn 出槽 | `ts_datain_independent` |
| 释放：DTE ack 后清 valid，Router 释放 Payload | `DataInTaskTable::Pop()` | `ts_datain_release` |
| datain 立即就绪 | `Step()` | 同上 |

**参数与简化**：user_id 位宽按 TS 接口取 10 bit（第 8 章有 10 vs 12 的冲突）。

#### 5.3　Stream_table

**对应设计**：第 3 章“stream_table”“task_fsm”。

**状态**：16 项 FIFO，`head_ptr` / `tail_ptr`；每项 `valid`、`user_id`、`task_id`、`task_unit`、`task_dsa_en`、`task_pc`、`task_is_reissue`、`task_fsm`、`done_bitmap[64]`、`reissue`、`end`；上限 `stream_num`。

**接口**：六个写端口（create、install、issue ×3、completion ×3、credit_wake、retirement），每个请求保持到 `accepted`；三种只读视图。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| FIFO 指针：tail 注册、head 释放，粒度 1，上限 stream_num | `StreamTable` | `ts_stream_fifo` |
| 年龄优先环扫，不按 SID 数值 | `StreamTable::OldestMatching()` | `ts_age_scan` |
| 用户创建、用户释放（end task 完成 → 清 valid、推 head、上游 credit++） | `Create()` / `Retire()` | `ts_stream_lifecycle` |
| task_fsm 全部迁移条件 | `StreamTable::Fsm()` | `ts_task_fsm` |
| 任务就绪条件：前序完成且（不需 credit 或 credit 满足） | `Fsm()` | `ts_ready_cond` |
| 写端口原子性：请求保持到 accepted；失败重读或只重试写 | 每端口一拍一笔，冲突时按 stage 顺序排队 | `ts_write_ports` |
| Head-only 退休：先还 credit 再清 | `Credit_monitor` 一节 | `ts_head_retire` |
| Core Mem 多用户管理：stream_id 绑定分片，统一偏移映射，任务结束释放 | `CoreContext::StreamBase(stream_id)` 供 DSA 地址计算 | `cm_stream_slicing` |

**参数与简化**：安装到 READY 3 拍、完成写入 3 拍（第 3 章 LLD 时序）。

#### 5.4　Task_ctrl

**对应设计**：第 3 章“任务生成与跳过”。

**状态**：每 Stream 的 `next_task_id` / `next_task_fsm`；处理中的上下文。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 独立推进，无全局 Task Pointer | 数据结构 | — |
| 选择 Stream：head_ptr 环扫 valid && FINISH && end=0 | `TaskCtrl::Pick()` | `ts_taskctrl_pick` |
| SKIP_MASK 一拍跳过：`~END & ((DATAIN & done) \| (REISSUE & ~reissue))`，64 bit 优先编码 | `TaskCtrl::Skip()` | `ts_skip_mask` |
| 四种跳过场景（datain 已完成 / B 不重发 / P2P 不重发 / DP+P2P 按用户类型） | `Skip()` + Router trigger 携带的标志 | `ts_skip_cases` |
| 状态选择：Generated → READY，DataIn → WAIT，Reissue → WAIT | `TaskCtrl::Install()` | `ts_install_state` |
| End 不可跳，不生成后继 | `Skip()` | `ts_end_not_skipped` |
| 原子安装 | `Install()` 保持到 accepted | `ts_install_atomic` |

**参数与简化**：每拍处理一条 FINISH Stream，安装 4 拍（C1～C4）。

#### 5.5　MU_Arb / VU_Arb 与 DTE_Arb

**对应设计**：第 3 章“三条发射通路”；DTE 优先级按方案 2。

**状态**：每个 Arb 锁定的胜出上下文，保持到 `raw ACCEPT`；发射宽度 1。

**接口**：`xxx_rv_command` / `xxx_rv_accept`（同拍，RV core 的 task_queue 有空槽才 accept）；`issue_update` 写端口；`datain_offer` / `datain_pop`。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 筛选候选：valid && READY && unit 匹配 | `Arb::Candidates()` | `ts_arb_filter` |
| 环形年龄优先 | `StreamTable::OldestMatching()` | `ts_age_scan` |
| 非抢占保持到 raw ACCEPT | `Arb::Step()` | `ts_arb_hold` |
| ACCEPT 后 READY → INFLY；失败只重试写 | `Arb::Commit()` | `ts_arb_commit` |
| RV core task_queue 满则反压 | `RvCore::Accept()` | `ts_arb_backpressure` |
| DTE 三源：Reissue 最高；DataIn 与 Generated 按年龄，同 SID Generated 优先 | `DteArb::Pick()` | `ts_dte_arb_priority` |
| DataIn 出槽只 pop 不改 Map | `DteArb::Commit()` | `ts_datain_independent` |
| `task_dsa_en=0` 的 Generated 任务下发 DTE Local RV | `DteArb` | `ts_dte_local_rv` |
| 并行 3 个 task 下发 | 三个 Arb 各自 `Step()` | `ts_three_issue` |

**参数与简化**：唤醒延迟按 C2 查 LUT → C3 发命令建，2 拍。

#### 5.6　Credit_monitor

**对应设计**：第 3 章“Credit 与重发”“用户退休”。

**状态**：锁定的 Reissue 候选；退休事务上下文。

**接口**：`reissue_req` 持续拉高 / `credit_issue_pulse`（Router CreditMonitor，同拍）；`stream_credit_return` / `accepted`（Router，同拍）；`reissue_wake_update`、`retirement_update` 写端口。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 筛选 Reissue：WAIT && is_reissue && reissue 最老 | `CreditMonitor::PickReissue()` | `ts_reissue_pick` |
| 持续请求到脉冲；Router 统一记账；唤醒 WAIT → READY | `Step()` | `ts_reissue_wake` |
| Head-only 退休：先持续发 `stream_credit_return` 到 accepted，再清 valid、head+1 | `CreditMonitor::Retire()` | `ts_head_retire` |
| 双路径并行 | 两个独立状态机 | `ts_cm_parallel` |
| dataout credit 申请：按 stream_id / path_id / user_id 向 Router 请求 | `CreditMonitor::RequestCredit()` → Router CreditMonitor | `ts_credit_request` |
| Broadcast Reissue：下游不可收置标；查 credit 选最老重发；并行、高优先；未成功不覆盖不释放 | `PickReissue()` + `DteArb` + Core Mem 保留 | `broadcast_reissue` |
| P2P 重发：datain + dataout 两 task；path_id 匹配越过；需重发时 dataout 请求 credit | Task_ctrl 的 Skip + `RequestCredit()` | `p2p_reissue` |
| DP+P2P：按“是否计算”匹配计算任务 | `Skip()` | `dp_p2p_skip` |
| Reduce credit N 笔：顺序连续下发 N 笔请求，收全 N 笔 Rmem finish | `CreditMonitor::ReduceRequests()`，`reduce_num` 从 task_chain 软件属性读 | `reduce_n_credits` |
| P2P 阻塞缓冲：下游无 credit 时搬到缓冲区记表；释放后续传并还上游 credit | `CoreMemReissue` + `RequestCredit()` | `p2p_block_buffer` |
| TS credit 返还：退休时经 Router 向上游返还 | `Retire()` | `ts_credit_return` |
| B core 搬出前按 `B_core_direction` 查下游 credit | `RequestCredit()` | `bcore_downstream_credit` |

**参数与简化**：退休事务 C6～C11 共 6 拍。

#### 5.7　Task_done

**对应设计**：第 3 章“完成事件的合流（Task_done）”。

**状态**：每 Lane 的 Hold；Router Reduce Done 的 Hold；`task_recv_type`。

**接口**：`completion_ack[5:0]`（RV core ×3 + DSA ×3，同拍）；Router `reduce_done`（user_id + task_id）；`completion_write[2:0]` 写端口。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 七路独立处理 | 三条 Lane 各一个 `Step()` | `ts_done_seven_lanes` |
| 完成来源判定 `task_recv_type` | `TaskDone::Classify()` | `ts_done_recv_type` |
| Reduce 完成分离：DTE ACK consume_only，Router Done 才 FINISH | `TaskDone::OnAck()` | `ts_reduce_done_split` |
| 无序汇合：Router Done Hold 到 DTE ACK 消费 | `TaskDone::Join()` | `ts_reduce_unordered` |
| Router UID 匹配：按 user_id 找 SID | `Join()` | `ts_uid_match` |
| Future DataIn：只更新 done_bitmap | `OnAck()` | `ts_future_datain` |
| 直接写 Map，三条写 Lane，同 Lane 串行 | 写端口 | `ts_done_write_lanes` |
| 三种收尾：RV / DSA / 两者都上报等两个 | `Classify()` | `ts_three_endings` |
| 异步 DataIn：只更新 bitmap 不推 task_id；后续任务需前序 flag 全有效 | `OnAck()` + `Fsm()` | `ts_async_datain` |

**参数与简化**：单笔 3 拍（C1～C3）。

#### 5.8　自发创建任务链（B core / R core）

**对应设计**：第 3 章“自发创建任务链”；第 6 章“专用 core 的运行轨迹”。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 双任务链 + 软件映射表：datain flag 软件维护，后续链 RV 轮询 | kernel 例程 `check_flag` + Share Mem | `bcore_two_chains` |
| check task 不调 DSA，RV 长期工作 | kernel 例程 | 同上 |
| 复位自启动 16 项，无用户信息，等 RV 返回 user_id 后更新 | `StreamTable::SelfStart()` | `ts_self_start` |
| 顺序激活与清 flag | kernel 例程 + `SelfStart()` | `ts_self_start_order` |
| 退休后再激活 | `Retire()` → `SelfStart()` | `ts_self_start_reactivate` |
| B core 查下游 credit | 5.6 | `bcore_downstream_credit` |
| B core 链一不查 TS credit（落 Matrix Mem） | `DataInTaskTable` 按 `CORE_TYPE` | `bcore_datain_no_credit` |
| R core 乱序：谁先集齐谁先走 | kernel 例程扫 `arrive_num` | `rcore_out_of_order` |
| R core 搬运原子化：某方向的用户数据全部搬进 Matrix Mem 后才搬别的方向或用户 | DTE Commit 按 UserID + 方向串行 | `rcore_atomic_move` |

***

### 6　RV Core（DTE / MU / VU 各一）

**对应设计**：第 3 章“RV Core”（task_queue、dsa_iss、CSR、自定义指令、访存延迟）；第 6 章“软件执行模型”“各类 core 的软件流程”。

RV core 不建流水线。kernel 是 C++ 例程，通过 `kernel_api.h` 里的操作集与模型交互；RV core 逐条消费例程发出的操作，按下面的规则计拍。三个 RV core 是同一个类的三个实例，区别只在绑定的 DSA 与可见的地址空间。

**状态**

| 项 | 内容 |
| - | - |
| task_queue | 深度 `TASK_QUEUE_DEPTH`，每项 `{task_pc, stream_id, local_user_id, task_id, stream_num}` |
| 执行状态 | 等待 task / 执行中 / 阻塞在某个操作上；当前例程与它的续点 |
| CSR | `stream_id`（只读，task 开始时写）、`task_id`（只读，硬件写；异步 datain 时例程可改写）、`local_user_id`（可读写）、`stream_num` |
| dsa_iss | 每拍最多 1 条配置 / trigger；`dsa_rq` 8 项记录在途读 |
| sm_lsq / cm_lsq | 各 16 项，顺序发射，每拍 1 请求 |
| 指令预算计数 | 本 task 已消耗的标量拍数；每 token 累计 |

**kernel 操作集**（`kernel_api.h`）

| 操作 | 语义 | 拍数 |
| - | - | - |
| `Scalar(n)` | 例程声明一段纯标量计算 | n 拍，n 由例程按第 6 章的指令预算给出 |
| `DsaWrite(addr, val)` / `DsaWrite2(addr0, v0, addr1, v1)` | 写 DSA 寄存器；`Write2` 当两条 `dsaw.s` 处理 | 每条 1 拍，下发通道反压则等待 |
| `DsaRead(addr) → future` | 读 DSA 寄存器，不阻塞；结果在返回后可用；`dsa_rq` 满则等待 | 下发 1 拍，返回时刻由 DSA 给 |
| `SmRead / SmWrite` | Share Mem 32 bit 访问 | 5～10 拍（参数 `SM_LATENCY`），经 sm_lsq |
| `CmRead / CmWrite` | Core Mem 访问（仅 DTE core） | 15～25 拍（参数 `CM_RV_LATENCY`），经 cm_lsq |
| `RouterRegRead / RouterRegWrite` | Router I/O reg（仅 DTE core，读包头、写 1 弹出） | 复用 cm_lsq |
| `FlagCheck(begin, end) → offset` | Share Mem 查第一个 1，找不到返回全 1 | `SM_LATENCY` |
| `Loop(count)` | 循环边界，退出 100% 预测 | 0 拍 |
| `TaskDone(ts)` | 结束当前 task；`ts=1` 通知 TS（携 task_id、stream_id、local_user_id） | 1 拍 |
| `SetCsr(task_id / local_user_id)` | 例程写可写 CSR | 1 拍 |

每个操作的拍数从 `params.h` 取，例程里只写语义。DSA 读的返回经 `dsa_rq` 按序写回，例程在真正使用该值时才阻塞。

**接口**：TS 的 task 下发 / accept、`task_done`（同拍）；DSA 的配置 / trigger / 读（同拍）；Share Mem、Core Mem、Router reg 访存（同拍请求 + 延迟返回）；ctrl_noc 的 kernel 表绑定与 CSR 查询。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| task_queue 提前接收，无 bubble | `RvCore::Accept()` / `NextTask()` | `rv_task_queue` |
| task_ack 握手：有空槽才 ack | `Accept()` | `ts_arb_backpressure` |
| task 开始写 CSR：`task_pc` 选例程，`stream_id` / `task_id` 写只读 CSR | `RvCore::Start()` | `rv_start_csr` |
| 完成上报：`task_done(TS)` 上报三个 ID | `RvCore::OnTaskDone()` | `rv_task_done` |
| task_done 后队空则阻塞等待；firmware 末尾不通知 TS 的 task_done | `OnTaskDone()` | `rv_task_done_wait` |
| dsa_iss 每拍一条；配置按反压判成功；读不阻塞，`dsa_rq` 8 项按序写回 | `RvCore::IssueDsa()` | `rv_dsa_iss` |
| DSA 指令附带 stream_id / user_id / task_id | `IssueDsa()` 从 CSR 取 | `rv_dsa_ids` |
| `dsaw.d` 当两条 `dsaw.s` | `DsaWrite2()` | `rv_dsaw_d` |
| lsq 顺序执行，每拍 1 请求，等返回释放 | `RvCore::Access()` | `rv_lsq` |
| 访存延迟 ITCM 1 / DTCM 3 / SM 5～10 / CM 15～25 | 参数；ITCM 与 DTCM 折进 `Scalar(n)` | `rv_latency` |
| Core Mem 读固定 1056 bit，不 burst，32 bit / 拍返回 | `CmRead()` 计拍 | `rv_cm_read` |
| 静态 / 动态配置寄存器：静态初始化配，动态随任务 | kernel 例程按第 4 章的寄存器划分写 | `rv_static_dynamic` |
| trigger / last：trigger 启动；last 标志包含在 trigger 寄存器里，DSA 完成后通知 TS | DSA 侧 | `dsa_trigger_last` |
| DSA 读需轮询 | `DsaRead()` 语义 | `rv_dsa_poll` |
| `loop`、`flag_check` | `Loop()`、`FlagCheck()` | `rv_flag_check` |
| 单用户各 DSA 的调度程序 ≤ 200 cycle；DTE 约 33 T、MU 约 100 T、VU 约 66 T | 指令预算计数器 + 断言 | `rv_budget` |
| MU core 100 T 内三件事（判 8 个激活专家落组、挑加权权重、配 Mmem / Cmem 地址） | MU 计算 core 的 kernel 例程 | `kernel_mu_fc` |
| 两处阻塞：RV core 忙则 task 等；DSA 指令 buffer 满反压 RV core | `Accept()` / `IssueDsa()` | `rv_two_blocks` |
| 标量 task 不调 DSA，RV core 自报完成 | `check_flag` 等例程 | `bcore_two_chains` |
| 异常期不受调度、不发 DSA | 不建（本轮） | — |

**kernel 例程清单**（`rv_core/kernels/`）

| 例程 | core | 内容 |
| - | - | - |
| `weights_loader` | 全部 | 算这一片落 Matrix Mem 的地址，配 DTE 做 router → MM |
| `token_datain` | 计算 core | 读包头判链中哪一步，配 DTE 做 router → CM（含 scale、包头、shareMem 写），异步 datain 时写 `task_id` CSR |
| `fc_gemv` | 计算 core（MU） | 判激活专家落组、挑权重、配 MU 原语与 topK 表 |
| `silu_dot_quant`、`situ_glu_quant`、`core_reduce` | 计算 core（VU） | 配 VU 宏指令（静态组选择 + 动态参数 + trigger） |
| `dataout`、`reissue_out` | 计算 core（DTE） | 配 DTE 做 CM → router，改硬件包头 path_id |
| `bcore_datain`、`check_flag`、`broadcast` | B core | 第 6 章 B core 两条链 |
| `rcore_datain`、`rcore_arrive_inc`、`rcore_scan`、`rcore_mm2cm`、`rcore_sum`、`rcore_out` | R core | 第 6 章 R core 两条链 |

**参数与简化**：`TASK_QUEUE_DEPTH` 2（待定，原文未给）；例程的 `Scalar(n)` 取值来自第 6 章指令预算，按 task 拆分（DTE 33 T、MU 100 T、VU 66 T），每条 DSA 写另计 1 拍；不建分支预测、流水冲刷、ITCM / DTCM 访问。

### 7　DTE DSA

DTE 建成八个子块，在 `Core::Cycle()` 的第 3 个 stage 内按“Lane 完成 → Completion RS → Commit → Header Parser”的顺序 `Step()`。

#### 7.1　Header Parser（Router 入口）

**对应设计**：第 4 章“Router 入站”“MSG 包结构”。

**状态**：Header Hold（首 flit 的包头字段）；帧状态（等待 Header / 接收 Payload / Drop）；累计有效字节。

**接口**：CoreStation 的 DataIn 流（每拍一 flit，可反压）；→ Commit 的 Descriptor。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 一帧一任务；首拍即 Header（上一帧尾 flit 已接受后的第一个 flit） | `HeaderParser::Step()` | `dte_hp_frame` |
| Header Accept 条件：Parser 空闲且 RD_CH0 / WR_CH0 TaskQueue 与 Completion RS 可用 | `Step()` 查 Commit | `dte_hp_accept` |
| 解析检查（version、route 只收 router → MM / CM、dst_addr、byte_count、身份不重复） | `HeaderParser::Check()` | `dte_hp_check` |
| Payload 门控：Commit 成功前反压 | `Step()` | `dte_hp_gate` |
| byte_count = 0 的纯包头任务 | `Step()` | `dte_hp_zero_len` |
| 长度核对：尾 flit 时累计字节与 byte_count 比较 | `Step()` | `dte_hp_length` |
| Drop Frame、Credit 断续、帧边界恢复 | `Step()` 状态机 | `dte_hp_drop`、`dte_hp_credit` |
| MSG 包结构：包头标记 2 B + path_id 1 B + path_core_mask 2 B + reserved 1 B + 包长度 2 B + 软件辅助信息 0～16 B + 业务数据 | `common/message.h` | `msg_layout` |

#### 7.2　Commit 与软件寄存器序列

**对应设计**：第 4 章“Task Descriptor”“软件侧的寄存器序列”“各路径地址公式”“并发约束”。

**状态**：两个配置 Bank（Bank0、Bank1）；task_id 分配器；DTE 软件寄存器组（`transfer_mode`、`scale_valid`、`topK_valid`、`router_ep_count`、`dst_addr` / `dst_base_addr` / `src_base_addr`、`stream_stride` / `src_stream_stride` / `dst_stream_stride`、`data_len`、`hw_header_op`、`hw_header_addr`、`sw_header_addr`、`scale_addr`、`sharemem_waddr`、`sharemem_data`、`trigger`）；PendingTaskQ（出 core 任务等下游 Stream / Rmem credit）。

**接口**：DTE core 的寄存器写（同拍）；Header Parser 的 Descriptor；→ 四个 TaskQueue 与 Completion RS。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 双 Bank 优先级：Bank0 > Bank1；Router 优先 Bank0、RVCore 优先 Bank1；只剩一个时 Router 优先 | `Commit::Pick()` | `dte_commit_banks` |
| Route → Lane Pair：router → MM / CM 用 CH0；MM / CM → router 与 MM → CM 用 CH1，固定出口 | `Commit::LanePair()` | `dte_commit_route` |
| 配对接纳：RD TQ、WR TQ、Completion RS 三项同时可用才 Commit，否则整体保持 | `Commit::Step()` 多方向一次性扣减 | `dte_commit_atomic` |
| 校验 Route、地址、长度；CM → MM 拒绝 | `Commit::Validate()` | `dte_commit_validate` |
| 配对子上下文生成、task_id 分配、同拍原子写入两个 TaskQueue | `Commit::Step()` | `dte_commit_pair` |
| Router Header 接纳：两侧都 Commit 后 Payload 才进 inbound buffer | `Commit` ↔ `HeaderParser` | `dte_hp_gate` |
| 软件寄存器序列：`transfer_mode` 决定路径；写 `trigger` 提交；`trigger` 含 last 标志 | `Commit::OnRegWrite()` | `dte_sw_regs` |
| 硬件地址计算：`dst = base + stream_id × stride`；软件包头 `sw_header_addr + stream_id × 1KB + task_id × 16B`；硬件包头 `hw_header_addr + stream_id × 硬件包头长度`；scale `scale_addr + stream_id × data_len / 32`，长度 `data_len / 32` | `Commit::Expand()` | `dte_addr_formula` |
| data_len 含义：router → MM 与 MM → router 含 topk + scale + data；router → CM 只含 data | `Expand()` | `dte_data_len` |
| `hw_header_op`：Init token 为 1（保存），其他 0（丢弃） | `Expand()` | `dte_hw_header_op` |
| 单任务最大 32 KB；只支持连续一维 | `Validate()` | `dte_max_len` |
| 出 core 前置申请：下游 Stream / Rmem credit 先申请到才发，否则在 PendingTaskQ 等待 | `Commit::PendingTaskQ` + DTE 持有的 `StreamLedger` 与 Reduce credit | `dte_pending_taskq`、`dte_reduce_credit` |
| 两入口不保序；任务身份唯一；任务期配置冻结 | `Commit` | `dte_two_entries` |
| R core 搬运原子化 | `Commit` 按 UserID + 方向串行接纳 | `rcore_atomic_move` |

**参数与简化**：TS 直接启动 DTE 的入口不建（第 8 章待定），只有 DTE core 配置一种入口；`TASK_CFG_ADDR / TD / PACK / TRG` 不作为软件接口，Commit 内部用 Descriptor 结构承载同样的字段；MU / DTE 寄存器地址在 `regmap.h` 里为临时映射（待定）。

#### 7.3　TaskQueue ×4 与 Lane ×4（含 AGCU）

**对应设计**：第 4 章“四 Lane 模型”“三类数据流”“数据搬运”。

**状态**：4 个 TaskQueue，各 `DTE_TQ_DEPTH` 项；4 组 Active Context（任务身份、Route、地址游标、剩余长度、首尾 mask）；CH1 读侧 outstanding 计数与 Response Hold；写侧 outstanding 计数；DataOut 侧 4 个 VC Buffer。

**接口**：RD_CH0 ← CoreStation DataIn 流；WR_CH0 → Cmem / Mmem 写口；RD_CH1 → Cmem / Mmem 读口；WR_CH1 → CoreStation DataOut 流或 Cmem 写口（WR1）。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 同拍配对入队、按序出队、两侧独立退出 | `TaskQueue` | `dte_tq` |
| 独立激活：任一 Lane 释放即激活下一任务 | `Lane::Step()` | `dte_lane_independent` |
| Read-ahead：RD 领先 WR，受 Buffer credit、outstanding 限额、任务边界数约束 | `Lane::Step()` + `Buffer` | `dte_read_ahead` |
| Lane 内按序；三类顺序（Lane 顺序、RD/WR 关联、Buffer 边界顺序） | `TaskQueue` + `Buffer` | `dte_ordering` |
| 局部反压：只暂停受影响 Lane，CH0 与 CH1 独立 | 各 Lane 独立 `Step()` | `dte_local_backpressure` |
| CH0 Header / Payload 分离；不发 DMA 读 | `Lane::RdCh0()` | `dte_hp_frame` |
| CH1 credit / outstanding：rd_req 预留，Push 后释放；Response Hold | `Lane::RdCh1()` | `dte_ch1_outstanding` |
| 读侧完成分离：末 rd_req 为 issue_done，末 response 且 outstanding 0 为 drain_done | `Lane::RdCh1()` | `dte_completion_states` |
| Command / Data 配对；CH0 写完成（末 wr_data 为 issue_done，全部 response 为 drain_done） | `Lane::WrCh0()` | 同上 |
| CH1 出口选择：Route 任务期固定；Router 路径末拍 TX Fire 完成，WR1 等写响应 | `Lane::WrCh1()` | `dte_ch1_exit` |
| WR1 只允许写 Core Mem | `Lane::WrCh1()` 断言 | `dte_wr1_mask` |
| CM 写仲裁：WR0 → CM 与 WR1 → CM 竞争，未获 grant 保持 | Cmem 仲裁器（同组同优先级） | `cmem_dte_two_writers` |
| Inner Flow 流水：MM → CM 时读、Buffer、WR1 写重叠 | `Lane` 各级独立 valid/ready | `dte_inner_flow` |
| AGCU：锁存建首 command、握手推进、末 command 迁移到 Completion RS、只管本 Lane | `Agcu::Step()` | `dte_agcu` |
| DTE 侧 4 个 VC Buffer，单 VC 阻塞不影响其他 | `Lane::WrCh1()` 的 DataOut 队列 | `dte_vc_buffers` |
| 与存储接口 256 B/T；与 Router 256 B/T | 参数 | `dte_bandwidth` |
| 一次搬运拍数 = `ceil(size / 256B)` + 两端访存延迟（Cmem 13T、Mmem 50T 端到端、Router 10T） | 计拍 | `dte_transfer_cycles` |

**参数与简化**：`DTE_TQ_DEPTH` 16（待定）；CH1 outstanding 上限 = Buffer 深度；Data + scale 拼接访存按 Cmem MAS 的 256 B 建（scale 由独立请求搬）。

#### 7.4　中间 Buffer

**对应设计**：第 4 章“中间 Buffer”。

**状态**：inbound 与 outbound 各一个 flit 队列，每项 256 B + task_id + 有效字节 + 任务边界；credit 计数。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| Credit 背压：inbound 满经 DataIn ready 反压 Router；outbound 经 credit 限读领先 | `Buffer::Push()` 失败 → Lane 等待 | `dte_buffer_credit` |
| 按任务边界消费，不跨任务 | `Buffer::Pop(task_id)` | `dte_ordering` |
| 解耦配对 RD / WR：credit 允许时 RD 收 N+1，WR 仍排 N | `Buffer` | `dte_read_ahead` |
| 掩盖 32T 访存延迟 | 深度参数 | `dte_transfer_cycles` |

**参数与简化**：inbound、outbound 各 16 × 256 B（合计 8 KB，待定）。

#### 7.5　Completion RS 与 Done Pending

**对应设计**：第 4 章“完成与状态回报”。

**状态**：Completion RS（`DTE_RS_DEPTH` 项：task_id、Lane 事件位、drain 条件、Buffer 边界排空标志）；Done Pending 队列（`DTE_DP_DEPTH` 项）。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 四源事件同拍到达 | `CompletionRs::Step()` 收全部 Lane 事件 | `dte_rs_four_sources` |
| 读发完与写消费分离 | `Buffer` 上报两种事件 | 同上 |
| Join 条件：同 task_id RD 与 WR 都满足；outbound 还要 buffer 排空 | `CompletionRs::Join()` | `dte_join` |
| 多命中保存；单路逐项输出；Exactly-once 握手；payload 稳定 | `DonePending` + TS 完成口 | `dte_done_exactly_once` |
| task_last 才上报 TS；`no_ack` 不上报 | `DonePending::Emit()` 按 trigger 的 last 标志 | `dsa_trigger_last` |
| 剖析口径：setup / issue / drain / report / active / stall | 观测记录器 | `dte_profile` |

**参数与简化**：`DTE_RS_DEPTH` 16、`DTE_DP_DEPTH` 16（待定）。

#### 7.6　Hmem、LUT、topK 与 shareMem 写

**对应设计**：第 4 章“包头与 topK”“shareMem”“Hmem / LUT”。

**状态**：软件包头表 16 × 64 项 × 16 B；硬件包头动态部分（`path_core_mask`）16 项按 stream_id；硬件包头静态部分（path_id、size）64 项按 task_id；B core / R core 的包头在 Core Mem 独立空间；topK 独立 mem（MU 的 `topK_ep_table`）。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 进核存 / 丢：分配新 stream_id 才存包头（`hw_header_op = 1`），否则丢弃 | `Hmem::OnInbound()` | `dte_header_store` |
| 出核改头：按 task_id 改硬件包头 path_id；path_core_mask 与软件包头不改 | `Hmem::OnOutbound()` | `dte_header_rewrite` |
| 纯包头任务 | `Commit` | `dte_hp_zero_len` |
| 分开存储：软件 / 硬件包头独立，硬件包头分静态与动态 | 数据结构 | — |
| 发包时 MSG Header 由 DTE 写入；加载 weights 时由 DPU 写入 | `Hmem::OnOutbound()` / GPU 桩 | `msg_header_writer` |
| topK 复制到独立 mem 供 MU 读；进核必带 topK，result 出核不带 | `Lane::WrCh0()` 末段 + `Mu::TopkTable` | `dte_topk_copy` |
| 完成后写 shareMem 再通知 TS；valid 软件维护 | `Lane::WrCh0()` 完成 → `ShareMem::Write()` → Done Pending | `dte_sharemem_write` |

**参数与简化**：LUT 的 `task_mode_table` 与 TS 快速启动一起不建。

***

### 8　MU DSA

MU 建成七个子块，在第 3 个 stage 内按“stq → matrix exe → ldq → agu → issue_q → regfile”的顺序 `Step()`。阵列按 32 物理 lane、原语 K128×N64 建；Matrix Mem 按 32 bank 与 lane 一对一。

#### 8.1　regfile 与 issue_q

**对应设计**：第 4 章“MU 的配置与启动”“topK 信息与任务启动”。

**状态**：配置寄存器（`SYS_CTRL`、`TASK_CFG`、`TASK_BLOCK`、`ADDR_TOKEN`、`ADDR_WEIGHT`、`ADDR_SCALE`、`ADDR_OUT`、`expert_en_config`、`topk_table_addr`、`topk_stream_stride`、`trigger`、`router_expert_count`、`local_ep_table`）；issue_q 16 项，每项一个 task 的 `cfg_info`；`STATUS.QUEUE_FULL`。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 收齐即下发：收完一个 task 全部寄存器（写 `trigger`）后发 issue_q | `Regfile::OnWrite()` | `mu_regfile_trigger` |
| 启动指令 `dsawi.d topk_stream_stride, trigger`；stream / task / user id 由 DSA 从 RV core CSR 读 | `Regfile::OnWrite()` 取随指令附带的 ID | `mu_ids_from_csr` |
| `router_expert_count = 0` 忽略 topK | `Regfile` | `mu_no_topk` |
| 顺序执行、满则拒收（反压 RV core 的 dsa_iss）、not_empty 才发 | `IssueQ::Step()` | `mu_issue_q` |
| 提前调度：task N 计算时 task N+1 load，load / 计算 / 写回三段重叠 | `IssueQ` 允许执行通路提前取下一 task | `mu_overlap` |
| finish 收集：收 stq 的 task_finish，退出并通知 TS（trigger 含 last） | `IssueQ::OnFinish()` | `mu_finish` |
| 静态 / 动态寄存器划分 | `regmap.h` | `rv_static_dynamic` |

#### 8.2　gen_ep_info、topK 表与专家加权累加

**状态**：`topK_ep_table`（按 stream_id，DTE 写入，{expert_id: weight}）；`local_ep_table`（global index → local index，软件初始化）；`Ksplit_acc`。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| gen_ep_info：按 topK 的 global index 查 local_ep_table 得 local index，算 weight 地址 | `GenEpInfo::Step()` | `mu_gen_ep_info` |
| FC1 / FC3 只用 ids，FC2 用 ids 与 weights | `GenEpInfo` 按原语模式 | `mu_fc_modes` |
| token 与 topK 分存 | 数据结构 | — |
| 专家加权累加 `C = C + (A × B) × W_ep`，无初始 C，W_ep FP32 | `MatrixExe::Accumulate()`，bit 级顺序 | `mu_expert_weighted_sum_bits` |
| 完整式子 `D = sf_A × sf_B × A × B + C` | `MatrixExe` + `common/numeric` | `mu_gemv_bits` |

#### 8.3　agu ×3 与 acu

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 任务拆分：先 tile_K 再 tile_N，按原语拆 | `Agu::Split()` | `mu_split` |
| Token agu：burst 访存，byte 对齐，Cmem 侧移位拼接 | `Agu::Token()` | `mu_token_agu` |
| Weight agu：256 B 对齐，一个地址逐级脉动到各 lane，32 bank 同步寻址 | `Agu::Weight()` | `mu_weight_agu` |
| Store agu：256 B 总输出，单 lane 4 B（vlane=2 时 8 B），1 KB 接口不足标 mask，按绝对地址顺序 | `Agu::Store()` | `mu_store_agu` |
| acu 对齐校验、越界捕获 | 断言（异常不建） | — |

#### 8.4　ldq ×2 与 Rd outstanding buffer

**状态**：Token ldq 16 项，outstanding buffer 16 × 256 B；Weight ldq 4 项，MAC 入口乒乓 2 级。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| outstanding 掩盖延迟：先发后回填，按使能 K 值读 buffer 输出 | `Ldq::Step()` | `mu_ldq_outstanding` |
| 非对齐移位 | `Ldq::OnReturn()` | `mu_token_agu` |
| vlane MUX：只读 `256B / vlane_num`，复制扩展到 256 B | `Ldq::Emit()` | `mu_vlane` |
| 控制随数据（vlane、acc、输出类型） | 数据结构 | — |
| Weight 乒乓：MAC 独享 Mmem 带宽，2 级缓存掩盖读出延迟 | `Ldq::Weight()` | `mu_weight_pingpong` |
| Token 读延迟 16、Weight 读延迟 4T（读 SRAM 2T + 打拍 2T） | 参数 | `mu_ldq_latency` |

#### 8.5　matrix exe

**状态**：32 lane，每 lane 10 级流水；CSA 树；scale block 分组（每 32 MAC 一组）；`Ksplit_acc`。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 指数提前 + 纯定点 CSA 树：MAC 后指数对齐，无符号压缩树累加 | `common/numeric/csa_tree.h`，参考实现同序 | `mu_csa_bits` |
| OCP MX scale：每 32 MAC 一组，组内累加后乘 scale，组间累加 | `MatrixExe::Primitive()` | `mu_mx_scale_bits` |
| 8 种原语、四种精度组合、算力 BF16 4K / MXFP8 8K / W4A8 16K / W4A16 8K MAC/T | `MatrixExe::Primitive()` 按原语选拍数 | `mu_primitives` |
| vlane 分组：CSA 第 128 层旁路 MUX，单 lane 多结果 | `Primitive()` | `mu_vlane` |
| Token 脉动：各 lane 1 拍错位 | `MatrixExe::Step()` 逐 lane 延迟 | `mu_systolic_skew` |
| 单 lane 10 级流水；一次原语的拍数 = 启动 + `ceil(K × N / 算力)` + 写回 | 计拍 | `mu_primitive_cycles` |
| MATH_NAN_INF Clamp | `numeric` | `mu_nan_clamp` |
| DIDT 分级启动、零输入旁路、错峰启动 | 不建 | — |

#### 8.6　stq 与 Wr concat buffer

**状态**：stq 16 项；每 lane 一个 concat buffer（深度 1～16，按到 Cmem 的距离）；1 KB 拼接器。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 双条件启动：等 store-acu 地址与 matrix exe 启动信息 | `Stq::Step()` | `mu_stq_start` |
| vlane=1 横切拼装：128 B 截面取 8 次凑 1 KB，8T；vlane=2 纵向拼装：256 B 截面取 4 次，4T | `Stq::Assemble()` | `mu_stq_assemble` |
| 凑 1 KB 突发写；不足按实际标 mask | `Stq::Emit()` | `mu_stq_burst` |
| 输出类型转换 FP32 / BF16 | `numeric` | `mu_out_type_bits` |
| 写完成上报 task_finish 给 issue_q | `Stq::OnWriteResp()` | `mu_finish` |

**参数与简化**：lane 距离引起的 concat buffer 深度按线性分布 1～16；Cmem 写延迟 16（待定，第 8 章）。

### 9　VU DSA

VU 建成十一个子块，在第 3 个 stage 内按“SU → 执行单元 → LU → pipe_ctrl → ISQ → config_register”的顺序 `Step()`。寄存器地址按《VU-DSA 寄存器整理》。

#### 9.1　config_register、ISQ 与 `macro_inst_trigger`

**对应设计**：第 4 章“宏指令模型”“静态配置与动态参数”“寄存器接口与状态”。

**状态**：8 组静态配置（每组 23 个）、12 个动态参数、`macro_inst_trigger` 六字段、状态寄存器（`status`、`macro_inst_left`、`error_code`）、逐组 in-flight 计数；ISQ 深度 `VU_ISQ_DEPTH`，表项 = 静态组号 + 动态参数快照 + 控制位。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 写 trigger 一次发一条；同拍锁存动态参数与静态组号打包入 ISQ | `ConfigRegister::OnTrigger()` | `vu_trigger` |
| `STATIC_DYNAMIC_MASK` 逐位选静态或动态；bit0 切 VL / DATA_TYPE / ROUND_MODE，bit5 切 MRF 两个，bit6 切 SRF 四个 | `OnTrigger()` 快照 | `vu_sd_mask` |
| 8 组并存；在用组改写阻塞（in-flight 计数非 0 时 `cfg_req_ready` 拉低，退休后生效，计入 `cfg_wr_stall_cycle`） | `ConfigRegister::OnWrite()` | `vu_static_rewrite_block` |
| 配置通路按序；三源固定优先级仲裁（VU core / ctrl_noc / debug） | `ConfigRegister::Step()` | `vu_cfg_paths` |
| ISQ 满回压 VU core、置 `ISQ_FULL` | `OnTrigger()` | `vu_isq_full` |
| ISQ 严格顺序、描述符自包含、纯 FIFO、出队与执行分离 | `Isq` | `vu_isq` |
| 状态寄存器写无效；`error_code` 读清；`macro_inst_left` 加减 | `ConfigRegister` | `vu_status_regs` |
| 静态区编号 ≥ 1024 用 byte 地址写；动态区编号在 5 bit 内 | `regmap.h` | `vu_regmap` |
| 保留编码处理：OPCODE 未定义按无操作；其他字段 Reserved 置 `CFG_ERROR` 且不派发 | `PipeCtrl::Validate()` | `vu_reserved_opcode` |
| VL 越界钳位（0 按 1，> 16384 按 16384） | `OnTrigger()` | `vu_vl_clamp` |

#### 9.2　pipe_ctrl 与 Scoreboard

**对应设计**：第 4 章“宏指令的派发与重叠”。

**状态**：最多 2 条 in-flight 宏指令上下文；Scoreboard（VRF / MRF / SRF 读写占用，4-entry 粒度）；执行分组与 RF 端口占用表；发射阻塞归因计数。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 宏指令即数据流图：静态模板 + 动态参数展开成各单元微指令 | `PipeCtrl::Expand()` | `vu_expand` |
| 静态配置合法性检查（src_sel 类别匹配、非 OPCODE 字段保留编码、RF 端口不超额、标量源硬连线）→ `CFG_ERROR` 放弃派发 | `PipeCtrl::Validate()` | `vu_validate` |
| Scoreboard RAW / WAR / WAW 按 4-entry 粒度，命中推迟；不追踪 CM 地址 | `Scoreboard::Check()` | `vu_scoreboard` |
| 两条重叠上限；后一条取数段掩盖前一条尾段 | `PipeCtrl::Step()` | `vu_overlap_two` |
| 结构冒险：同宏指令争用为 CFG_ERROR，相邻争用推迟 | `Validate()` / `Step()` | `vu_structural` |
| DATA_BROADCAST 串行：等除 CM-Load 外前序全部完成 | `Step()` | `vu_broadcast_serial` |
| Fence 串行：等此前全部宏指令退休（含 SU 写入 CM） | `Step()` | `vu_fence` |
| SEXE 三次迭代自动串行 | `Expand()` | `vu_sexe_iter` |
| 退休回报静态组号；EVENT_EN 退休时发单拍 Event（`ts_evt_id` = STREAM_ID） | `PipeCtrl::Retire()` | `vu_event` |
| 发射阻塞归因一拍一项：fence > bcast > dep > eu；`issue_starve_cycle` | `Step()` 计数 | `vu_stall_attrib` |
| 宏指令启动延迟 ≤ 20 拍；单条总周期 = 启动 + Σ 首拍延迟 + (SEG − 1) | 计拍 | `vu_macro_cycles` |

#### 9.3　LU、SU 与 CM 接口

**对应设计**：第 4 章“执行单元能力”“数据类型与舍入模式”“CM 端口”。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| CM 读写各一条独立通路，一次 1024 bit（+ 4 B scale），不 burst；128 B 对齐发出；软件地址 32 B / 4 B 对齐；跨 128 B 边界由 LU / SU 拆分重组 | `Lu::Issue()` / `Su::Issue()` | `vu_cm_split` |
| LU 格式由 OPCODE 给出；窄格式精确扩宽；MXFP8 scale 硬件应用；`ld.fp32.vm` 在 BF16 下按 ROUND_MODE 窄化 | `Lu::Convert()`（`numeric`） | `vu_lu_convert_bits` |
| 暂存与 bypass 并存：同一 Load 写 VRF 又 bypass，只读 CM 一次 | `Lu` → DMUX | `vu_lu_bypass` |
| LU 与 SU 并行 | 两条通路 | `vu_lu_su_parallel` |
| `ld.vm_mask` 占 MRF 写口；`ld.s.fp32` 占 SRF 写口 p0 | `Lu` | `vu_lu_mask_scalar` |
| SU 下转换与舍入：写 BF16 / FP8_e4m3 / MXFP8 按 ROUND_MODE；MXFP8 scale 按块统计，`MXFP8_SCALE_ROUND` 选向上 / 向下 | `Su::Convert()` | `vu_su_convert_bits`、`vu_mxfp8_scale_round` |
| 计算结果直出 SU 不经 VRF；三类写出（向量 / 掩码 / 标量）共用配置 | `Su` | `vu_su_direct` |
| 写响应齐后退休 | `Su::OnResp()` | `vu_su_retire` |
| ROUND_MODE 三作用点；八种舍入编码 | `numeric/round.h` | `vu_round_modes_bits` |
| MXFP8 在 CM 中 data 与 scale 一一映射，scale 地址硬件推导（`data_len / 32`） | `Lu::Issue()` | `vu_mxfp8_scale_addr` |
| LU ≈ 15 拍、SU ≈ 15 拍（含 Cmem 14T） | 参数 | `vu_lu_su_latency` |

#### 9.4　SMUX / DMUX、VALU0 / VALU1 / VALU2、VSFU、MEXE、SEXE

**对应设计**：第 4 章“执行单元能力”“单条宏指令的资源上限”。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 统一 8 bit `src_sel` 编码；纯路由 1 拍；广播不占端口；类别匹配检查；回环链式不自环不成环 | `Mux::Route()` | `vu_mux` |
| 写端口冲突显式检查（VRF 2 写、MRF 1 写、SRF 6 虚拟写）；结果可不暂存；标量结果自动归位 | `Mux::Writeback()` + `Validate()` | `vu_writeback_ports` |
| VALU 按能力分组：VALU0 独有 29 条（含 MACC、除法、比较、vfclass、vfmerge、vfmv.s.f）、VALU1 独有 6 条（归约、Top-16、vfmv.f.s）、VALU2 独有 1 条（vmv.v.v）、共享 12 条 | `Valu::Execute()` 按 OPCODE 表 | `vu_valu_ops_bits` |
| 内部精度 FP32 / BF16 由 DATA_TYPE 给出，不用舍入 | `numeric` | `vu_valu_ops_bits` |
| 全吞吐为常态：除法 20～30 拍非全吞吐、Top-16 排序 ∝ SEG，其余每周期 1 entry | 各单元拍数表 | `vu_eu_latency` |
| 标量源硬连线 p1 / p2 / p3 只 src1；FP32 → BF16 自动转换 | `Mux` + `numeric` | `vu_scalar_src` |
| 四分组独立流控 | 各单元 `Step()` | `vu_eu_flow` |
| VSFU 12 条查表 + 拟合，各函数同延迟；不能取自身输出 | `Vsfu::Execute()`，bit 级按参考实现的查表 | `vu_vsfu_bits` |
| 掩码作用：掩码位 0 的 element 不更新 | `Valu::Execute()` | `vu_mask` |
| 配平责任在软件：vmv.v.v 配平 1 级 | 不检查 | — |
| MEXE 15 条：掩码与向量通路分离；两类输出；与 VALU0 直串不反向；MRF 单写口三竞争者；索引清 / 置位；整数输出不作 SEXE 源 | `Mexe::Execute()` | `vu_mexe_bits` |
| SEXE 一个物理三次迭代；操作数三来源无立即数；后两次迭代一个额外读口；可消费归约结果；不占向量算力 | `Sexe::Execute()` | `vu_sexe_bits` |
| VL 尾块：超出 VL 的尾部 element 保持原值 | `Valu` / `Su` | `vu_vl_tail` |

#### 9.5　VRF / MRF / SRF 与 Profile

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| VRF 512 × 1024 bit 2R2W；端口静态分配；可完全旁路；写口来源互斥；读写区间不部分重叠（软件保证）；三操作数上限；越界回绕置 `RF_IDX_ERROR`；Scoreboard 保护 | `Regfiles::Vrf` | `vu_vrf` |
| MRF 512 × 64 bit 2R1W；最多 2 处使用不广播；单写口三竞争者；读写同 entry 允许 | `Regfiles::Mrf` | `vu_mrf` |
| SRF 64 × 4 B，8 逻辑读 / 6 虚拟写硬连线；时分复用；写口使能与 opcode 一致；写口索引互异；读口无互斥 | `Regfiles::Srf` | `vu_srf` |
| 宏指令内索引按 entry 自动递增；RF 占用 ⌈VL ÷ 32⌉ 或 ⌈VL ÷ 64⌉ | `Regfiles` | `vu_rf_occupancy` |
| DSA-RF 调试通路 | 不建 | — |
| Profile：`profile_ctrl` RUN / CLEAR，29 个 64 bit 计数器，六组口径 | `Profile::Step()` | `vu_profile` |
| 典型算子拆分（SiLU·点乘·MXFP8 2 条、SiTU-GLU 3 条、Softmax 3 条、RMS Norm 2 条、Top-K 1 条、Vector ADD 2 条、Weighted SUM 1 + N 条） | kernel 例程 + 参考实现 | `vu_operators_bits` |

**参数与简化**：`VU_ISQ_DEPTH` 8（待定）；各单元首拍延迟取第 4 章初估值（config_register 1、ISQ 1、pipe_ctrl 2、VALU 加减乘 2、MACC 4、VSFU 4、MEXE 1、SEXE 2、LU / SU 15）。

### 10　存储子系统

三块存储在第 2 个 stage 内按“Cmem → Mmem → Share Mem”顺序 `Step()`。

#### 10.1　Core Mem

**对应设计**：第 4 章“Core Mem（Cmem）”；第 3 章“Core Mem 的硬件多用户管理”。

**状态**：8 bank，每 bank 1024 × 128 B SRAM + 4 KB scale 寄存器；六个端口（DTE 读、DTE 写、MU 读、MU 写、VU 读、VU 写）各一个请求槽；每 bank 一个独占仲裁器；返回延迟队列。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| bank 冲突二选一，无冲突同时访问 | `CoreMem::Step()` 逐 bank `TryGrant` | `cmem_bank_arb` |
| DTE 局部反压：只反压冲突的 bank | `CoreMem::Step()` | `cmem_dte_partial` |
| 同组同优先级：DTE 先读写各自判断，再 wr 与 rd 判断 | 仲裁顺序 | `cmem_dte_two_writers` |
| 非同组优先级 MU > VU = DTE | 优先级仲裁器 | `cmem_priority` |
| 132 B 访问：MU / VU 按 132 B 读写，scale 使能时同读写 scale 寄存器；只访问 SRAM 时有效带宽 128 B | `CoreMem::Access()` | `cmem_scale_regs` |
| Byte mask 写 | `Access()` | `cmem_byte_mask` |
| 延迟 DTE 13T、MU 11T、VU 14T；带宽 DTE 256 B/T、MU / VU 128 + 4 B/T，总 1 KB + 32 B/T | 参数 | `cmem_latency` |
| stream_id 分片：`base(stream_id) = 分片大小 × stream_id`，统一偏移映射 | `CoreContext::StreamBase()` | `cm_stream_slicing` |
| ECC、计数器、ctrl_noc 后门 | ctrl_noc 后门读写按 4 B/T 建；ECC 不建 | `cmem_ctrl_noc` |

#### 10.2　Matrix Mem

**状态**：32 bank（与 MU lane 一对一），每 bank 1 MB + 128 KB scale；端口：DTE 读、DTE 写、MU 读、ctrl_noc；同 bank 冲突计数器；三种角色的内容布局由软件决定。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 同 bank 互斥：两个 master 同 bank 只执行 MU 请求，报错并计数 | `MatrixMem::Step()` | `mmem_conflict_mu_wins` |
| scale 地址计算 | `MatrixMem::ScaleAddr()` | `mmem_scale_addr` |
| 三种角色（权重 / reduction 数据 / token 缓冲） | 软件布局 | `bcore_slots`、`rcore_out_of_order` |
| 延迟：端到端 50T；DTE 写 9T / 读 8T、MU 读 8T（lane 内）；带宽 DTE 256 B/T、MU 8 + 1 KB/T | 参数 | `mmem_latency` |
| 地址粒度 128 B，不支持 Byte mask | `Access()` 断言 | `mmem_granule` |
| ECC 自纠回写、计数器 | 不建 | — |

**参数与简化**：MU 读延迟取 Mmem MAS 的 8T（第 8 章有 4T / 8T / 17T 三个值）。

#### 10.3　Share Mem

**状态**：32 KB；RV core 访问口 ×3；DTE 写口。

**机制覆盖**

| 机制 | 落点 | 用例 |
| - | - | - |
| 三种用途：task 间共享、用户映射表、标量 | 软件 | `bcore_two_chains`、`rcore_out_of_order` |
| RV core 访问 5～10 拍，每拍 1 请求 | 参数 | `rv_latency` |
| DTE 完成后写 valid 标志 | `ShareMem::Write()` | `dte_sharemem_write` |
| `flag_check` 查找 | `ShareMem::FindFirstSet()` | `rv_flag_check` |

***

## 输入

模型读入四类东西，格式由《software/》下的文档定义，本章只规定内容要求。

| 类 | 内容 | 来源 |
| - | - | - |
| 拓扑与部署 | rack 数、chip 形状 2×5、Harvest mask、逻辑 ↔ 物理 core 映射、切分参数（EP / TP / PP / DP 与四种模式之一）、chip 数 48、GPU 数与每 GPU 的 batch | 编译侧 |
| 每 core 配置 | RouterTable（每 path 一表项、三份副本一致）、Credit Bypass Route、task_chain（≤ 64 项，含软件属性 `exe_dest` / `task_group_id` / `reduce_num`）、datain_task、`stream_num`、`CORE_TYPE`、`B_core_direction`、`trigger_task_chain_en`、DTE 包头表（硬件包头静态表 64 项、软件包头 16 × 64 项）、MU `local_ep_table`、VU 8 组静态配置、Core Mem 的 reissue 预留空间 | 编译侧 |
| kernel 表 | 每 core 每 task_pc 对应的 kernel 例程名与它的 `Scalar(n)` 预算 | 编译侧 |
| 数据 | 每 core 27 MiB 权重分片（含共享专家）与落 Matrix Mem 的地址；注入表（每 token 的 6368 B 级联包与注入拍）；参考实现的期望输出 | 编译侧 + `reference/` |

编译侧产物里必须有、不能反推的几样：每个 core 的 RouterTable（同一 path_id 在不同 core 上表项不同）、每个 core 的 task_chain 与 datain_task、每 core 的权重分片与角色、坏核 mask、`CreditCounter[path_id][stream_id]` 初值（广播 = 目的 core 数，P2P = 1）。

参数表 `common/params.h` 是全部拍数、带宽、深度的唯一出处，每个值标注来历：MAS 给的、性能需求规格说明书给的、第 8 章冲突项按“建议”取的、本章“待定”默认值。

***

## 时间轴与链路

Clock 周期取 1 T，即 1 GHz 下的 1 ns，一拍就是一 T。第 5 章的参数凡是按 T 给的直接变成拍数；按 ns 或 μs 给的（PCIe C2C 300 ns、GPU 注入 3 μs）按 1 T = 1 ns 折算。

拍数换算统一为 `ceil(size / bw)`，且 size 小于等于 0 时算一拍。

latch 的 `Time` 有效范围是 32 位，1 T 一拍下约 4.29e9 拍。一层 FFN 的端到端延迟在 1e4 到 1e5 拍量级，余量充足。

发送侧按链路带宽与延迟算出到达时刻写进 `arrive_cycle`，接收侧 peek `Front()`，只有当前拍不早于 `arrive_cycle` 才 Pop。Fifo 本身固有的一拍延迟被吸收，时序 owner 仍然唯一是发送侧的链路模型，不额外扣时。R2R 40T、C2C 400T 都远大于一拍，吸收得下。同一条链路上到达时刻单调递增，因为发送侧的占用时刻单调，所以队首不会阻塞更早到达的包。这一条加断言。

***

## 输出与观测

波形走 latch 原生的 Trace 与 insight，层次为 `chip<i>.core<j>.<块>.<子块>.<信号>`。

另出一个目录，三个事件桶加一份来源信息：

| 文件 | 内容 |
| - | - |
| `unit_spans.jsonl` | 单元占用区间：node_id、unit、user_id、task_id、start、end、state、volume |
| `unit_waits.jsonl` | 等待区间、归因 reason，以及它属于依赖还是资源 |
| `global_latency.jsonl` | 每个 `(gpu_id, token_id)` 的端到端延迟、经过的 core 序列与每段时刻 |
| `run_meta.json` | 编译侧产物的来源、参数快照、结束时刻、完成与丢失的 token、credit 终态 |

`run_meta.json` 不是可选的。没有它，一份观测产物没法追回是哪套切分、哪套参数跑出来的，结论就没有适用范围。

等待归因按子块细化，分依赖与资源两类：

| 类 | reason |
| - | - |
| 依赖 | 等前序 task 完成、等 datain 数据到达、等 Router `reduce_done`、等 DSA 读寄存器返回、等 Completion RS 的 Join、等 Scoreboard 依赖、等 B core 的 head ≠ tail、等 R core 的 arrive_num == 2 |
| 资源 | 等 stream 坑、等 VC credit、等 Reduce credit、等 GPU grant、等 TaskQueue / ISQ / issue_q 项、等 Lane、等 bank 端口、等 RV core 空闲、等 dsa_iss 通道、等 DTE Buffer credit、等 Xbar 出口、等 ReduceModule 上下文 |

瓶颈由此能定位到具体资源而不只是慢，而且只有资源那一半是改参数动得了的。

两条发射端的过滤规则：占用区间丢弃 end 小于 start 的，以及 end 等于 start 且没有显式允许零长的；等待区间丢弃 end 不大于 start 的。被丢的条数单独计数，用来区分“没记到”与“没发生”。

事件在仿真期记在各模块自己的裸 vector 里，`JoinAll()` 之后由主线程汇总落盘。`Cycle()` 里不做文件 IO。汇总时按节点与时刻排全序，否则收集顺序取决于线程调度，产物无法逐行 diff。

***

## 建模顺序

每个阶段结束时都应有一个能跑、能验收的东西。两种切分都要跑，阶段 4 与阶段 5 各对应一种。

| 阶段 | 建什么 | 跑通的标志 |
| - | - | - |
| 0 | `common/`：封包、参数表、寄存器映射、四种仲裁器、`numeric/`（FP8_e4m3 / MXFP8 / MXFP4 / BF16 / FP32 编解码、八种舍入、CSA 累加顺序、MX scale）；`reference/` 参考实现；`tables/` 读入 | `numeric` 与参考实现逐 bit 一致；一张手写最小任务链能读入 |
| 1 | 存储三块、RV core 执行器与 kernel 操作集、TS 九个子块、DTE / MU / VU 全部子块；Router 用直连桩（DTE 发出的搬运直接送到目的 core，只扣 R2R 延迟，不查 credit） | 单 core 跑完计算 core 单链（datain → FC1/FC3 → silu·dot·量化 → FC2 → dataout），结果与参考实现逐 bit 一致；TS 派发、DSA 启动、访存各段拍数与第 5 章一致；每条子块机制至少一个单测通过 |
| 2 | Router 八个子块、坏核、Chip 2×5 与 Harvest、ctrl_noc 与 SCP 桩、boot 与配置流程 | 单 chip（含 1～2 个坏核）跑通单播、多播、逐级 reduce、CoreMem 重发、Retire；credit 守恒；A1～A17 全部通过；每跳 40T、reduce 累加拍数与公式一致 |
| 3 | GPU / DPU 桩、链路、PCIe Switch、出口桩、48 chip 装配、weights 加载模式 | weights 按 P2P path 装进 384 个 core 的 Matrix Mem；GPU 两层 credit 闭环成立 |
| 4 | EP1+PP3+TP16：B core 两条链、path 0～12、中间 P2P core 的 reissue0 / reissue1、FC2 段 8 个 datain 的 concat | 整层结果逐 bit 一致；单用户端到端延迟落在第 5 章“单用户总延时”公式附近，差异逐段归因 |
| 5 | EP6+TP8：EPTP-NK 14 步链的三种 core 角色、R core 两条链、EP 组间 reduction 链、GPU 桩的 LPU Dispatch 派遣 | 整层结果逐 bit 一致；R core 乱序对齐；派遣顺序调度不死锁；多用户吞吐与第 5 章公式同量级 |
| 6 | 观测产物、性能分析 | 四个输出文件齐全，等待归因能解释吞吐与延迟差距 |

***

## 验收

### 四层判据

由粗到细：

1. 结果一致。注入 N 个 token，出口收到 N 个结果，逐 bit 等于参考实现。
2. 不变量成立。四条不变量在每个阶段都查，任何一条被破坏都说明模型有结构性错误。
3. 逐段时间手算。单 user 单 stream 下，GPU 注入加链路加 TS 派发加 RV core 配置加 DSA 启动加访存加逐跳链路时间加计算时间，与模型输出逐段对上，每一段的取值都能在第 5 章或参数表找到。
4. 等待归因。`unit_waits` 的 reason 分布能解释吞吐与延迟的差距，资源类等待能通过改参数消掉。

### 检查项

验收分五类，每一类都要有能自动跑的检查，跑完一轮全部通过才算这一阶段结束。

| 类别 | 检查项举例 |
| - | - |
| 拓扑结构 | core 数量 = chip 数 × 10；坏核集合与 mask 一致；边界 core 的 C2C 连接与 2×5 规则一致；逻辑 ↔ 物理映射满足 special 优先四步 |
| 数据流正确性 | **Credit 守恒**（初始 + 归还 = 消费 + 余额，无泄漏）；flit 组装正确；topK 正确传递；MU / VU 计算次数与预期一致；每 core 的 user init 数量符合预期 |
| 边界与异常 | Stream 耗尽正确排队；Credit 耗尽正确阻塞上游；Fifo 满正确背压；无效 path_id / 重复 User ID 被拒 |
| 时序行为 | 各启动延迟等于配置值；R2R 单跳 40T，跨 chip 400T；同一 user 的 task 之间先后与任务链一致；bank 冲突时按优先级授予；VU 两条宏指令重叠、MU 三段重叠可在波形上读出 |
| 性能指标 | 多用户吞吐与第 5 章“多用户吞吐”公式同量级；单 user 端到端延迟不低于理论下界；各单元占用率不超过 100%；RV core 每 task 的标量拍数不超过预算 |

四条不变量：

1. **credit 守恒**：一轮跑完后，所有方向、所有类型的 credit 计数回到初值。
2. **相同 `task_id` 在不同 user 之间时序无交叠**。
3. **每个目的 core 对同一个 user 的同一份数据只落一次**。
4. **注入 N 个 token，出口收到 N 个结果**，且逐 bit 等于参考实现。

Router 的验收场景 A1～A17 中，下面四个直接覆盖了最易实现错的语义，先跑这四个：

| 场景 | 查什么 |
| - | - |
| A2 | bypass 中间核不占用户坑 |
| A5 | 同用户二次发送余额不减 |
| A15 / A16 | 单坏核与多坏核串联时的 credit 透传 |
| A17 | reduce 完成 Ack 归 Router；缺 Ack 时任务链停在 reduce 处不前进 |

### 最容易实现错的几条

1. **Reduce 任务的完成判定**：DTE ACK 只代表搬运完成，**只有 Router Reduce Done 才能把任务置 FINISH**，且两者可任意顺序到达。判错会让 reduce 链提前推进。
2. **异步 datain 只更新 done_bitmap，不推进 task_id**。这是任务链能乱序完成又保持顺序语义的关键。
3. **Retire 的三方时序**：Core 保证不再有该 user 的搬运 → Router 立即删表项 → ReduceModule 延迟到 credit 全恢复才删。
4. **多播原子准入**：多播的每个 flit 必须同时取得所有目标方向资源才能发，任一方向不足则所有分支统一等待。不能让分支独立前进。
5. **进 core 后锁定到尾 flit**，而 Router-to-Router 可以在 flit 边界切换 Packet。两种粒度不能混用。
6. **Mmem 同 bank 不能有两个 master**，冲突时只执行 MU。
7. **VU 的 CM 访存冲突硬件不追踪**，靠软件的 `MACRO_INST_FENCE`。建模时若默认硬件会挡，结果会偏乐观。
8. **DTE 的配对接纳**：RD、WR 两个 TaskQueue 与 Completion RS 三项同时可用才接纳，不产生半任务。
9. **bit 级的累加顺序**：MU 的 CSA 树按 scale block 分组累加、Router reduce 按到达顺序 FP32 累加、VU 归约按 LANES 内归约再 ⌈log2 SEG⌉ 级累加。参考实现必须用同一顺序。

### 测试写法

驱动本身是 `ClkModule`，通过对方暴露的 Fifo 或同拍调用口交互；至少一个 `sub_thread` 大于 1 的并发用例；每个用例连跑二十次无 flake；断言的量在协程内 snapshot，不在 `JoinAll()` 之后读 `Logic64`。机制覆盖表里的每个用例名对应一个测试函数，覆盖率以“机制覆盖表里没有用例为空的行”为准。

***

## 边界与风险

### 简化

* **RV core 不建流水线**：kernel 是 C++ 例程，拍数由 `Scalar(n)` 的预算与访存延迟给出；分支预测、流水冲刷、ITCM / DTCM 访问、gpr 端口竞争不体现。
* **异常、ECC、看门狗、功耗类机制不建**：各单元留状态位与接口名。
* **TS 直接启动 DTE、DTE 的 3 Lane 方案、DSA-RF 调试通路不建**。
* **Router 的输出移位拼接不建**：flit 定长 256 B，尾 flit 带有效字节数。
* **时间常数未校准**：第 5 章标“偏小”“带问号”的值与第 8 章的冲突项都是配置值，支持同参数下的相对比较，不是绝对性能预测。

### 设计未给值、本章填了默认值的参数

全部标“待定”，在 `params.h` 里集中登记，向设计方要到值后只改参数表：

| 参数 | 默认值 |
| - | - |
| Router VC Buffer 深度 | 32 flit / VC |
| Stream Resource Table 项数（每方向） | 16 |
| VC credit 初值 | 下游 VC Buffer 深度 |
| RouterTable 表项数、副本数、每副本写入拍数 | 64、5、1 |
| Xbar 与 ReduceModule 三路输入的仲裁算法 | 轮询 |
| ReduceModule Entry credit、bank 数、RMW 拍数、输出队列深度 | 64 flit、4、2、8 |
| CoreStation HeaderFIFO、OutputBuffer 深度 | 16、32 flit |
| DTE TaskQueue、Buffer、Completion RS、Done Pending 深度 | 16、16 × 256 B × 2、16、16 |
| VU ISQ 深度 | 8 |
| RV core task_queue 深度 | 2 |
| MU、DTE 的寄存器地址映射 | `regmap.h` 临时映射 |
| Mmem MU 读延迟 | 8T |
| Cmem 的 MU 写延迟 | 16T |
| `operation` 的 Reduce0 / Reduce1 / Reduce2 含义 | 源分量 / 中继累加 / 最终汇聚 |
| `exe_dest`、`task_group_id`、`reduce_num` 的承载 | task_chain 的软件侧属性 |

### 风险

**规模。** 每拍一次全局 barrier，同步点数量等于挂时钟的节点数。48 chip 是四百多个 Core 节点加桩，每拍开销未实测。应对是把驱动粒度上提到 Chip，子块代码不动。先在单 chip 上测出每拍开销再决定。

**状态机改写的语义等价。** 把硬件里的等待写成跨拍状态机是工作量最大、也最容易引入语义差异的一块。每个状态机要有对照 MAS 时序图的单测，尤其是 Reduce 完成的无序汇合、多播原子准入、Retire 的三方时序、DTE 的配对接纳与 Join 这四条跨阶段持有的语义。

**bit 级一致的累加顺序。** MU 的 CSA 树、VSFU 的查表拟合、VU 的归约树顺序在 MAS 里只给了原则没给细节，参考实现与模型只能按同一份 `numeric/` 实现对齐，与真实硬件是否一致要等 RTL 出来核对。

**资源授予的 stage 全序。** 若出现环形依赖，则无法用一个 stage 顺序同时满足所有释放先于授予，届时相关资源的授予会晚一拍。需要在阶段 1 把 Core 内的释放与授予关系画出来核对。

***

本章依据第 2 至 6 章整理；建模方式、建模顺序与验收判据以本章为准。
