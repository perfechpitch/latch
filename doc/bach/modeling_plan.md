# Bach 在 latch 上的建模方案

> **文档模式：** 设计。含建模动机与取舍，供评审与实现前对齐。
> **文档层级：** 详细实现。它只服务于 Bach 在 latch 上的重建这一件事。
> **目的：** 给在 latch 上重建 Bach 模拟器的实现者，定下模型形态、代码结构、输入输出、分期与验收判据。它不复述 Bach 的规格细节，也不复述 latch 的建模规则，中间文件的逐条字段定义在《Bach 中间文件格式》里。
> **依据材料：** `/home/colin/develop/bach/doc/` 全套文档

***

## 1. 建模对象

### 1.1 Bach 推进的三类量

Bach 是非数值离散事件架构模拟器。它推进的量只有三类：事件发生时刻、资源占用区间、符号化封包的身份与元数据。

封包不携带张量、权重或激活值。所有计算都是占住一个容量为 1 的资源若干 ns。数据量只以字节数参与拍数与延迟换算，不参与任何数值运算。

它回答的问题是：在这组明确的架构与 timing 假设下，哪个事件何时能发生、它在等什么、identity 怎么传递、哪个资源或依赖改变了结果。

### 1.2 一次 run 的四步

1. 加载：读入 JSON Map，建立拓扑尺寸、坐标换算、外部节点、PCIe 拓扑与 MoE 配置。
2. 编译：把 Map 的 timeline 展开成每个 Core 一张顺序任务表、一张 credit 表和一份每任务元数据。
3. 装配：实例化 Chip、Core、Router、PcieSwitch、Host、Out，接好片内 mesh 与片间 PCIe。
4. 推进：Host 或 MoEDispatcher 注入 user，推进到完成判据满足。

时间全部是非负整数 ns。浮点只出现在利用率统计与 MoE 权重里，不参与时间推进。

### 1.3 三张互不推导的边集

模型同时维护三张边集，内容不能互相推导，实现时必须分开存：

| 边集 | 含义 |
| - | - |
| 物理 fabric 图 | 谁和谁之间有一根线，接在哪个端口 |
| 软件 credit 图 | 谁向谁申请与归还 credit，下游核是什么类型 |
| 任务表 | 每个 Core 按序执行哪些任务，每个任务的目标是谁 |

路由只看物理图与坐标，credit 只看软件图，任务顺序只看任务表。

### 1.4 组件清单

**Core 内部**：TaskScheduler（顺序取指、stream 槽位、依赖等待、退休）、DTE（本地搬运、收发、reduce）、Router（NoC 路由与链路）、MatrixCore 与 VectorCore（计算服务时间）、CreditUnit（下游流控）、MemorySystem（CoreMem 与 MatrixMem 及其接口）、MoEBitMap（每个 uid 的专家数表）。

**Core 之间与外部**：Chip（片内 mesh）、PcieSwitch（片外交换网与它的显式路由表）、Host（注入与 credit 回收）、Out（结果收敛）、MoEDispatcher（MoE 分发策略）。

**可选子模型**：DTE.DSA 五路径仲裁、EthSwitch、Phase1 lane 与 sink、Phase2 ingress 与 result bridge、Phase3 join、Phase 生命周期外壳。

### 1.5 建模范围

范围是上面的全部组件，包含可选子模型。落地按分期推进，每期能独立跑通并验收。

Phase 生命周期外壳在 Bach 里不参与当前 CLI 路径，只提供 Phase 端口与生命周期钩子，本方案照此建模，不重复扣传输时间。

***

## 2. 建模方式

### 2.1 定位：逐拍 cycle 模型

latch 的建模方法分三段：功能模型（指令加 oper）、factory 轨（oper 打到带宽加延迟的服务台）、逐拍 cycle 模型（手写模块，直接用 runtime 原语，不经 factory）。

Bach 有仲裁、有排队、有 credit 闭环、有按 stream 优先级排队的资源，`Factory` 那种带宽加延迟、乱序完成的服务台表达不了优先级排队。因此 Bach 建成逐拍 cycle 模型，范本是 `src/module/llc.h` 与 rv32 流水线，遵循 `module_writing_guide.md` 的全部规则。

### 2.2 驱动粒度：独立物理节点挂时钟，从属单元跟着上级走

`tick` 决定 `ClkModule` 的构造函数要不要 `clk->Bind(...)`，也就是谁持有推进它的协程。挂时钟的模块自己有一个常驻协程；不挂的跟着上级节点的协程走，共用同一个 `ClockPtr`，行为同样是逐拍的。

规则：

> 能独立被调度的物理节点用 `tick=true`：Core、PcieSwitch、EthSwitch、Host、Out、Phase1 lane 与 sink。
> 从属于某个节点、与它同拍工作的内部单元用 `tick=false`：Core 里的八个单元。
> 判据是它有没有一个不属于任何上级节点的物理位置。

`Core::Cycle()` 里按 stage 顺序调用八个内部单元的 `Step()`。由此得到三件事：波形层次仍然是 `chip0.core3.dte.<信号>`；常驻协程数等于节点数；八个单元的内部状态只被 `Core::Cycle()` 一个调用点触碰，因此内部用裸 `std::vector` 与标量即可，不需要任何同步。

这样分的第一条理由是保住 Core 内的同刻语义。Bach 里这八个单元之间是同一 ns 内的函数调用：TaskScheduler 派发给 DTE 同刻进入，DTE 收齐包后同刻调 `scheduler.ack()`，DTE 收到 RETIRE 包同刻调 `CreditUnit.return_credit()`，Router 的本地投递直接调 `DTE.handle_comm`。各自挂时钟就必须走 `Fifo`，上拍写下拍读，一次跨核搬运在收发两端多出四到八拍，这些拍不可消除。合在一个 `Cycle()` 里，同刻调用原样保留。

第二条理由是每拍开销。runtime 是严格 lockstep，每拍所有协程一起过一次 barrier，同步点数量等于挂时钟的模块数。协程池与物理线程数都可以调大，槽位上限是 65535，所以这不是容量上限，是开销：barrier 的成本随模块数线性增长，而线程数超过物理核数反而更慢。装配时 `RT::Reset(sub, co)` 两个数相乘要不小于挂时钟的模块数，少于模块数会直接死锁。空闲模块睡眠这条路 latch 实现并验证过，因维护成本移除，所以框架不会跳过空闲模块。512 核 Map 按节点粒度是五百多个协程，按单元粒度是四千多个。

驱动粒度是构造参数。若 Core 级驱动在大规模 Map 上开销过大，把 `Chip` 改成 `tick=true`、在它的 `Cycle()` 里顺序调各 Core 的 `Step()`，单元代码不动。

单元单测时给它传 `tick=true`，它就自己挂时钟单独跑，不需要拉起整个 Core。

从属单元把一拍的工作写在 `Step()` 里，`Cycle()` 只决定这一拍由谁让出：挂时钟时自己起手 `DelayCycle(1)` 再调 `Step()`，不挂时钟时由上级让出，`Step()` 里一次也不许让出。这条分工写在 `ip/sub_unit.h` 的基类里，两种驱动方式共用同一份单元代码。

### 2.3 等待改写成跨拍状态机

`Cycle()` 体内除起手的 `DelayCycle(1)` 外不得再 yield，否则该协程挂起会卡住 OldestStamp。Bach 的等待全部是协程让出点，逐个改写成跨拍状态机，每拍判断一次：

| Bach 的让出点 | 状态机 |
| - | - |
| `DTE.comm` 六处：等准入令牌、等 setup 资源、setup 计时、等执行通道、等 function 资源、逐拍发送 | DTE 任务状态机六个状态，后两者带剩余拍数 |
| `BaseComputeCore.compute` 五处，加 MoEBitMap 读的两处 | 计算任务状态机七个状态 |
| `CreditUnit.consume_credit` 三处：等查账锁、固定 4 ns、等全部下游 credit | 三个状态，查账锁在整个等待期间持有 |
| `TaskScheduler` 取指循环四处：取 stream 槽位、每轮 16 ns、等自身前序任务、等前一 stream 的对应任务 | 每个 stream 槽位一个 workflow 状态机 |

每个 Core 的状态机数量有界：workflow 上限是 `STREAM_COUNT`（默认 8），加上在飞的收包处理。仲裁器相应地从阻塞式获取改成每拍 `TryGrant` 一次。

`common/arbiter.h` 提供三种排队语义，对应 Bach 用到的 SimPy 资源：

| Bach 用的 | 本方案的 |
| - | - |
| `Resource(cap=1)` | 独占仲裁器，等待队列先进先出 |
| `PriorityResource` | 等待队列按优先级、请求拍、插入序排 |
| `Container`（credit 与 stream 槽位） | 计数信号量，get 队列队首阻塞 |
| `all_of([get(1)…])` | 多下游一次性扣减，先到手的先扣住再等其余 |
| `Store` | 直接用 `Fifo` 或裸队列 |

### 2.4 跨模块只走 Fifo

跨 ClkModule 的数据一律走 `Fifo<Pkt>`，Pkt 继承 `Logic`，字段在构造函数里 `Fields(...)` 注册。链路封包：

```cpp
class RoutedPkt : public Logic {
 public:
  Logic64 dst_row, dst_col, size, byte_offset, fragment_id,
          total_fragments, is_tail, arrive_cycle;
  LogicPtr<CommInst> payload;
  explicit RoutedPkt(ClockPtr c) : /* ... */ { Fields(/* ... */); }
};
```

`LogicPtr` 对应 Bach 里多个链路片共享同一个 `CommInstPacket` 的语义，且不深拷字节。

`Fifo` 是单 producer 单 consumer，因此每条链路每个方向一个 Fifo。节点按接进来的线数开入口，一条线一个，不是一个节点一个：两条线往同一个入口里写就违反了这一条。Router 的十二个入端口天然是十二路，不违反这条。同一拍多个入口都有包时按入口的登记顺序处理，顺序固定，所以一次 run 的结果与线程调度无关。

Bach 用 Python 对象身份 `id(payload)` 作为转发重组的一维 key。C++ 侧改为每一拍显式分配一个传输实例号。

发包时在 `Cycle()` 体内现建一个 `RoutedPkt`，写完字段再 `Push`，不复用成员。`Latch::Get()` 对本协程本拍刚写的值有转发，所以现写现读拿得到；但没被本拍写过的字段会回落到上一拍的值，复用一个成员包会把上一拍的旧字段悄悄带出去。

### 2.5 可观测量与禁用清单

跨拍可读的计数、占用与标志一律用 `Logic64`，配一个 cycle 内的裸 `uint64_t` 累加器，在 `Cycle()` 末尾一次性 commit。`Logic64` 一拍只能 Set 一次，同拍多次 Set 触发断言。

`Logic64::Get()` 在主线程读到的是 t=0 的值。因此完成集合、结束时刻、丢包集合这些验收产物必须在协程内 snapshot 到普通变量，不能在 `JoinAll()` 之后直接读。

不出现 `std::mutex`、`std::lock_guard`、`std::condition_variable`、`std::atomic`。

### 2.6 Core 一拍的 stage 顺序

`Core::Cycle()` 按"末级先做"排：先让已经在做的事完成，再让新的事派下去。计数 commit 在所有累加之后、发 Trace 之前。

| 次序 | stage | 它让什么落在同一拍 |
| - | - | - |
| 1 | Router | 本拍到点的封包重组齐了交给 DTE，各入端口的新包进队列，仲裁转发 |
| 2 | 存储 | 访存到点的释放仲裁器，本拍排在后面的就能接上 |
| 3 | 倍率表 | 读写到点的落表，本拍要用它的计算与搬运才拿得到值 |
| 4 | 两个计算核 | 算完的 ack 出去 |
| 5 | DTE | 发完或收完的 ack 出去，收到的 RETIRE 把额度还给 CreditUnit |
| 6 | CreditUnit | 拿上一步刚还回来的额度验资，验过的 ack 出去 |
| 7 | TaskScheduler | 把本拍所有 ack 一起看进来，推进各条流水，派下一批任务 |

排在最后的 TaskScheduler 看得到本拍全部完成，所以依赖链不掉拍；代价是它本拍派下去的任务，各单元要到下一拍才开始动。两者只能取一个：依赖链掉拍会直接改变屏障解开与退休发生的时刻，而派发晚一拍相对 setup 的几十拍可以忽略。

Router 排在 DTE 之前是同一个取舍：本拍投递到的包 DTE 本拍就处理，而 DTE 本拍交出去的包 Router 下一拍才仲裁。

***

## 3. 模型结构

### 3.1 目录按物理拓扑

```
src/bach/
  ip/
    sub_unit.h                     从属单元基类，Step 与 Cycle 的分工
    node_context.h                 每个节点都有的编号、参数表与记录器
    route_config.h                 外部节点与跨 chip 网关的坐标换算表
    wiring.h                       接一条双向物理链路
    system.h                       持有全部节点，做跨 chip 与阵列外的接线
    eth_switch/
      eth_switch.h                 节点，tick=true
    external/
      host.h  out.h                节点，tick=true，各自带一个路由器
      phase1_lane.h  phase1_sink.h 节点，tick=true
    node/
      pcie_switch.h                节点，tick=true，端口按名字开，没有默认路由
    chip/
      chip.h                       片内核阵列与 mesh 连线，预留聚合驱动
      core/
        core.h                     节点，tick=true
        core_context.h             各单元共用的只读上下文
        ports.h                    单元之间的调用口与对外收发口
                                   以下八个单元从属于 Core，一律 tick=false
        task_scheduler.h  credit_unit.h  moe_bitmap.h
        router/
          router.h                   十二端口、切分与重组、出口仲裁
        dte/
          dte.h                      两个方向的搬运与四段闸门
          dsa.h                      五路径的定路、仲裁与占用分析
        compute/
          base_compute_core.h  matrix_core.h  vector_core.h
        memory/
          memory_system.h  mem_arbiter.h  mem_interface.h
  common/
    packet.h  params.h  arbiter.h
  tables/
    task_table.h  credit_table.h  task_meta.h  loader.h
  observer/
    span_recorder.h  jsonl_writer.h
  sim/
    build.h                        从中间文件读入的结果装配出整套硬件
    completion.h                   完成判据与全局生命周期看门狗
    phase_shell.h
```

单元之间是同刻调用，所以口是一组纯虚方法而不是 Fifo。抽接口而不是互相持有具体类型，是因为调用是双向的：TaskScheduler 派任务给 DTE 与两个计算核，它们完成后又要回调它。接口把回调那一侧收窄成被调用方真正需要的能力，装配顺序也就不受构造顺序牵制。

`test/bach/ip/` 按 `ip/` 的一级镜像分目录，每个 IP 旁边有它自己的测试。

### 3.2 物理归属的三处判定

**Router 属于 Core。** Core 构造时创建五个 child，Router 是其中之一，每个 Core 一个。片内 mesh 的连线动作在 Chip 里做，但被连的端口长在 Core 上。

**PcieSwitch 属于 node。** 它可以绑定到某个 chip，绑定后只能接同一 chip 的 Core，但它自己是片外的交换节点，坐标是不与核阵列重叠的锚点。它的端口是 Map 起的名字，一个交换节点有几个口、各叫什么都由拓扑决定；核那一侧仍然是十二个方向之一，两侧各用各的端口标识，接线时对接。

**Host、Out、Phase1 端点属于 system。** 它们的坐标不在核阵列内，登记在外部节点表里，靠几何推断接进某个网关 Core 的 PCIe 端口，或者挂在 PcieSwitch 上，不属于任何 chip。

它们各自带一个路由器，不直接挂在网关核的端口上：包先进自己那个路由器的本地口，由它按 PCIe 链路的带宽与延迟送到网关核；回来的包也在自己这个路由器上落地重组，再交给设备本身。少了这一层，链路上被切开的片就没有地方攒回整拍，设备会收到同一拍的好几份。

### 3.3 交换网上的每一条路由都由 Map 指名

核阵列上的路由是先列后行算出来的，交换网没有几何可依循，查不到路由就停机，不挑一个口出去。

核那一侧对应地多一张表：某个目的地在与它相连的那个交换节点的路由表里，本核就从那个口进交换网，不再按方向算。这张表压过跨 chip 的网关换算，因为交换网是片外的直达，不必先回到本 chip 的出口网关。

交换节点自己有两张表，按入口分的那张压过按目的地的那张：同一个目的地，从不同的口进来可以走不同的出口。

装配期查两件事：一条路由的出口不能是它自己的入口，出口后面接的必须是目的地本人或者另一个交换节点。第二条挡住的是出了 PCIe 又回到 NoC，那一段本该由核阵列自己走。

### 3.4 DTE 的执行通道可以按五条路径排队

一次搬运的两端各是 Router、MatrixMem、Core 本体三处之一，由此得到五条路径，每条占进出两条物理通道中的一条或两条。掩码相交的两条不能同时占用，不相交的可以：一进一出跑得起来，两头都在本地的那条一开跑，整个 DTE 的搬运就停了。

放行规则是最老且不冲突者优先。少了「最老」这一条，一条要两条通道的路径会被源源不断的单向路径饿死。

这一段默认关着，关着时进出两个方向各自先进先出，与原来一样。开着时它取代的只是排队方式，要做的事一件不变。

### 3.5 非物理部分

四个目录不在 `ip/` 下：`common/` 是封包类型、参数表与仲裁器；`tables/` 是 Map 编译出的软件产物；`observer/` 是旁路观测，按 Bach 的规范不得改变模型结果；`sim/` 是一次 run 的装配、完成判据、看门狗与 Phase 生命周期外壳。

***

## 4. 输入与输出

### 4.1 输入：Map 的编译产物由 Python 侧预生成

Map 不在 C++ 侧解析。用 Bach 现有的编译流程把 Map 编译成中间文件，latch 侧只读它。这样编译规则里最容易出错的几处直接复用已验证的实现：四张辅助表的写入顺序与覆盖规则、接收侧 task id 在编译期重放接收核展开规则算出的 tag、SKIP 行的上游 group 递归收集。两边跑同一份任务表，时序差异才是干净的。

文件是行式文本，一行一条记录，行首是记录标签。逐条字段定义、校验规则与一个最小完整例子在《Bach 中间文件格式》里，那份文档是格式的唯一出处。它覆盖拓扑尺寸、每核属性、任务表与它的元数据、credit 表、SKIP 行来源、外部节点、网关与片间链路、PCIe 交换拓扑、MoE 编组，以及一份参数快照。

### 4.1.1 编译产物里必须有、不能反推的几样

任务表、credit 图、交换网拓扑、MoE 的分发结果各自单独给，谁也不从别处推：

| 这一样 | 为什么不能推 |
| - | - |
| 软件 credit 图 | 有的核一条验资任务都没有，却仍要接收下游退休时还回来的额度。从验资任务反推会漏掉这些边，漏掉的那一刻额度就还不回去 |
| PCIe 交换网的连线与路由 | 交换网上没有几何可依循，一个交换节点有几个口、每个口通向哪里、哪个目的地走哪个口，全部由 Map 指名。核阵列那套先列后行在这里一条也用不上 |
| 一次搬运的两端 | 五路径要知道两端各是 Router、MatrixMem 还是 Core 本体。同一个 opcode 在不同的 Map 上两端可以不同，只认任务元数据里写明的那条，没写就只按进出方向归类 |
| 注入源发哪种入口包 | 普通核收 `USER_INIT`，广播核收 `FIFO_IN`。这一条不另外登记，看入口核任务表第一行写的是什么，因为那里本来就有，再记一份就多一处会对不上的地方 |
| 每个 user 的期望分片数 | dense 与广播下等于任务表里发往 Out 的行数，MoE 下由本次命中了哪几个 EPGroup 决定 |

### 4.2 随机决策一并预生成

MoEDispatcher 的四种 routing policy 与 VectorCore 的动态 HitMap 都取自同一个 `random.Random(RANDOM_SEED)` 实例。在 C++ 里得到同一串结果需要复刻 MT19937 与 CPython 的 sample 和 shuffle 实现。

因此每个 user 的 HitMap 与 MoEBitMap 由 Python 侧一并预生成写进中间文件，latch 侧按 uid 查表。HitMap 是本次激活的 EPGroup id 集合，MoEBitMap 是每个 group 上命中的专家数。可见时刻仍按模型规则控制：动态 HitMap 要等 VectorCore 那次计算结束后才对下游可见，只是内容不再由 latch 现算。

### 4.3 输出：波形与精简 JSONL

波形走 latch 原生的 Trace 与 insight。

另出一个目录，三个事件桶加一份来源信息：

| 文件 | 内容 |
| - | - |
| `unit_spans.jsonl` | 单元占用区间：node_id、unit、uid、tid、start、end、state、volume |
| `unit_waits.jsonl` | 等待区间、归因 reason，以及它属于依赖还是资源 |
| `global_latency.jsonl` | 每个 uid 的端到端延迟与期望分片数 |
| `run_meta.json` | 中间文件的来源、参数快照、结束时刻、完成与丢失的 uid |

`run_meta.json` 不是可选的。没有它，一份观测产物没法追回是哪张 Map、哪套参数跑出来的，结论就没有适用范围。

等待归因的 reason 分类照搬 Bach，并分成依赖与资源两类：等自身前序任务、等前一 stream 的对应任务、等对端数据到达属于依赖，等 stream 槽位、等准入令牌、等 setup 单元、等执行通道、等功能单元、等内存仲裁器、等查账锁、等下游 credit 属于资源。这是瓶颈能定位到具体资源而不只是慢的原因，而且只有资源那一半是改参数动得了的。

两条发射端的过滤规则照搬 Bach：占用区间丢弃 end 小于 start 的，以及 end 等于 start 且没有显式允许零长的；等待区间丢弃 end 不大于 start 的。被丢的条数单独计数，用来区分"没记到"与"没发生"。

事件在仿真期记在各模块自己的裸 vector 里，`JoinAll()` 之后由主线程汇总落盘。`Cycle()` 里不做文件 IO，那会拖住每拍推进。汇总时按节点与时刻排全序，否则收集顺序取决于线程调度，产物无法逐行 diff。

***

## 5. 时序对齐与验收

### 5.1 对齐目标：语义等价，时序可解释

完成集合、丢包集合与等待归因与 Bach 一致。结束时刻允许有差异，但每一处差异都要能在偏差清单里找到出处，并能算出理论差值。

不追求逐 ns 复刻 Bach 的结束时刻。那要求在 latch 上复刻 SimPy 的同刻定序，包括全局事件序号、URGENT 与 NORMAL 两档优先级、回调表顺序与字典插入序，实际等于在 latch 上再写一个单线程事件队列。

### 5.2 时间轴与量纲

Clock 周期取 1 ns，一拍就是一 ns。Bach 的全部时间参数是整数 ns，直接变成拍数，不需要重新标定。

拍数换算统一为 `ceil(size / bw)`，且 size 小于等于 0 时算一拍。

参数默认值以 `common/params.h` 为唯一出处，按 Bach 规格里的三级分级标注每个值的来历：声称来自硬件规格、结构性取值、代码里自标为临时的取值。第三级的四项在实现时保留原注释，它们是已知不确定项。

latch 的 `Time` 有效范围是 32 位，1 ns 一拍下约 4.29e9 拍。Bach 一次 run 的结束时刻在 1e4 到 1e5 ns 量级，余量充足。

### 5.3 链路到达时刻

发送侧按 Bach 的公式算出到达时刻写进 `arrive_cycle`，接收侧 peek `Front()`，只有当前拍不早于 `arrive_cycle` 才 Pop。Fifo 本身固有的一拍延迟被吸收，时序 owner 仍然唯一是发送侧的链路模型，不额外扣时。

Bach 的最小跳延迟是每跳最小服务时间加线延迟，远大于一拍，吸收得下。

同一条链路上到达时刻单调递增，因为 Router 的 `last_busy_until` 单调，所以队首不会阻塞更早到达的包。这一条加断言。

### 5.4 偏差清单

| 来源 | 处理 | 是否消除 |
| - | - | - |
| Core 内部单元之间的同刻调用 | 八个单元同在 `Core::Cycle()` 里，仍是同拍 | 消除 |
| 跨 Core 的封包到达时刻 | 由 `arrive_cycle` 精确到拍，与 Bach 的 ns 一致 | 消除 |
| Router 一次唤醒服务完所有活跃端口 | 在 `Cycle()` 里用循环服务到空，仍在同一拍 | 消除 |
| 资源释放与授予的同拍性 | 靠 stage 顺序保证释放排在授予之前。存在环形依赖时无法全序，需逐条核对 | 逐条核对后确定 |
| Router 与 PcieSwitch 的入端口队列 | Fifo 深度有限，深度不足触发断言，不静默丢包 | 深度给足则消除 |
| 时刻 0 各注入源的发包先后 | Bach 由外部设备列表顺序决定，本方案由 Router 的端口轮转顺序决定 | 已知差异，两边都确定 |
| MoE 分发与动态 HitMap 的随机序列 | 由 Python 侧预生成，与 Bach 同种子同序列 | 消除 |
| TaskScheduler 派下去的任务 | 它排在各单元之后，所以派发晚一拍生效。一条任务一拍，相对 setup 的几十拍可忽略 | 已知差异，每条任务恰好一拍 |
| 等自身前序任务的多条记录 | Bach 逐条 yield，记若干条等待，其中多数零长；本方案合并成一条 | 归因总量一致，条数不同 |
| 完成表不随退休清空 | Bach 的等待方挂在事件上，事件先唤醒表后清；逐拍查表时清表会让还在等它的流水永远等不到 | 已知差异，代价是表随 user 数增长 |
| 交给路由器的封包 | 路由器排在交包方之前，所以本拍交出去的包下一拍才被仲裁 | 已知差异，每拍恰好一拍 |
| 归约窗口认领一个 user 时的建档 | Bach 走一次 MoEBitMap 写口占一拍，本方案直接建档不占口 | 已知差异，每次认领一拍 |
| 交换节点与核之间的端口对接 | Bach 用一个适配器对象把交换节点的动态端口桥到路由器的固定端口，本方案两侧各用各的端口标识，接线时直接对接 | 不影响时序，消除 |
| 五路径的物理占用 | Bach 在放行那一刻另起两条 span 记进出两条通道各自的占用，本方案只记一条逻辑占用，进出各占哪条由掩码还原 | 归因总量一致，条数不同 |

### 5.5 分期

| 期 | 内容 | 验收 |
| - | - | - |
| 0 | 地基：仲裁器三件套、参数表、封包与身份、中间文件读入、事件记录器 | 一张手写最小任务表，单 user 单 stream，逐段时间手算对得上 |
| 1 | Core 主线：TaskScheduler 取指循环与两条等待、屏障 SKIP、退休顺序、DTE 出入方向、MatrixCore 与 VectorCore、CreditUnit、MemorySystem、MoEBitMap、Host 与 Out | 单核加 Host 与 Out 跑完整张任务表，credit 闭环成立，槽位分配与队首退休的约束生效 |
| 2 | 互连：Router 十二端口、先列后行加 chip_rules 网关、单服务器记账、三层切分与逐跳重组、片间 PCIe、多 chip 装配、完成判据、三个看门狗与各项致命检查 | `bach_topology.json` 三十二核跑通，完成集合与丢包集合与 Bach 一致 |
| 3 | MoE：EPGroup、HitMap 过滤、计算倍率、按 HitMap 判定某条入边本次不会来包时自 ack 放行、FIFO 与 Reduction 两种存储模式下同时可激活的 user 数受 stream 数约束、Out 的期望分片数 | `neo-moe`、`nano-moe`、`moe_bc_core` 跑通，完成集合一致 |
| 4 | 显式 PCIe switch 拓扑与 DTE.DSA 五路径仲裁 | 五路径占用与十个交点可分析，掩码冲突与最老且不冲突者优先有单测 |
| 5 | EthSwitch、Phase1 lane 与 sink、Phase2 ingress 与 result bridge、Phase3 join、Phase 生命周期外壳 | group envelope 校验通过，residual 与 result 任意顺序到达都能判定 join，完成层级正确 |

### 5.6 验收判据

四层，由粗到细：

1. 任务表一致。两边读同一份中间文件，天然成立，是后面三层的前提。
2. 完成集合一致。`completed_uids` 与 `lost_uids` 与 Bach 相同。
3. 逐段时间手算。单 user 单 stream 下，Host 推包间隔加 DTE setup 加拍数加逐跳链路时间加计算时间，与模型输出逐段对上。
4. 等待归因一致。`unit_waits` 的 reason 分布与 Bach 同量级，差异能落到偏差清单里。

测试写法按 latch 的规则：驱动本身是 `ClkModule`，通过对方暴露的 Fifo 交互；至少一个 `sub_thread` 大于 1 的并发用例；每个用例连跑二十次无 flake；断言的量在协程内 snapshot，不在 `JoinAll()` 之后读 `Logic64`。

***

## 6. 边界与风险

### 6.1 继承自 Bach 的边界

这些是 Bach 当前实现的边界，重建时原样继承：

**队列满不挡住上游。** Bach 的 Router 与 PcieSwitch 入端口队列达到阈值只打告警，不阻塞、不丢包，上游感知不到。唯一的反压来源是 credit 与 stream 槽位。本方案继承这条语义：Fifo 的深度只是容量保护，深度不足是实现错误，不作为反压通道。拥塞类结论的适用范围止于 credit 层。

**计算单元不访存。** MatrixCore 与 VectorCore 声明了 memory port，但计算过程不发起访存，计算时间只来自 Map，与数据量和内存带宽无关。真正发起访存的只有 DTE 的三条路径：reduction 首包写入、FIFO 压栈、FIFO 弹出。

**reduce 不与接收重叠。** 收齐整包之后固定耗 32 ns，不建模收到第一拍即可开始累加。

**MoE 倍率只乘在计算时间上。** 它不影响搬运量、不影响访存、不影响路由。MoEBitMap 表项写入后不随退休清除。

**时间常数未校准。** 85、40、32、400、5000 都是配置值，支持同参数下的相对比较，不是绝对性能预测。

**identity 不是全局唯一的。** uid 与 tid 在不同 Core 上可以重复，收包去重靠 uid、layer_id、tid、tag、opcode 五元组加拍号。

**额度还得回来但放不下时不报错。** credit 图与任务表可以对不齐：某个核收得到下游的退休信号，却没有一条验资任务扣过那份额度。Bach 那边的归还是一个会挂起的事件，放不下就一直等着，模型照跑。本方案照此实现，多出来的那份挂在账户上，等有空位再补进去。这是 Map 的问题，不是这一层该替它拿主意的地方。

**五路径的两端不从 opcode 猜。** 一次搬运的两端到底是 Router、MatrixMem 还是 Core 本体，只认任务元数据里写明的那条路径。同一个 opcode 在不同的 Map 上两端可以不同，猜出来的占用会把两条本可并行的路径判成互斥。没写就只按进出方向归类，落不到某一条具体路径上。

**Map 编译现在不写显式路径。** 编译链不产 `dsa_route`，所以真实 Map 上的搬运全都落在按方向兜底的那两条队列上，五条精确路径与它们之间那十个交点在真实 Map 上都是空的。这不影响仲裁本身：兜底的两条队列与原来那两条执行通道一一对应，同一张 Map 开与不开五路径，结束时刻一致。

### 6.2 本方案待验证的风险

**规模。** 每拍一次全局 barrier，同步点数量等于挂时钟的节点数。协程池与线程数可调，不是容量问题，是每拍开销随节点数线性增长的问题。三十二核 Map 约四十个节点，跑十万拍可接受。五百一十二核 Map 是五百多个节点，每拍开销未实测。应对是把驱动粒度上提到 Chip，单元代码不动。先在三十二核上测出每拍开销再决定。

**状态机改写的语义等价。** 把协程让出点改写成跨拍状态机是本方案工作量最大、也最容易引入语义差异的一块。每个状态机要有对照 Bach 让出点的单测，尤其是查账锁在整个等待期间持有、准入令牌持有到整个任务结束这两条跨阶段持有的语义。

**资源授予的 stage 全序。** 若出现环形依赖，则无法用一个 stage 顺序同时满足所有释放先于授予，届时相关资源的授予会晚一拍。需要在第 1 期把 Core 内的释放与授予关系画出来核对。

**Phase 相关子系统在 Bach 侧本身未闭合。** 完整分层 identity、共享 Eth ingress 序列化竞争、闭环上游反压、通用复制与多 Host fan-in 在 Bach 里都标为未完成。第 5 期只能对齐 Bach 的当前行为，不能对齐它的设计意图。
