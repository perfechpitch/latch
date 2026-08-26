# 第 7 章　latch 建模计划

**模式**：design（陈述当前设计，动机与取舍收在各节的“取舍”段落里）

给在 latch 上按逐拍 cycle 模型重建 Bach 的实现者，定下六件事：

1. 要建哪些对象
2. 每个对象在模型里是什么
3. 前面几章定义的机制各落在模型的哪里
4. 代码怎么组织
5. 按什么顺序建
6. 怎么验收

三条边界：

* 模型的对象从 GPU 桩到 core 内的每个模块，粒度与《Core 内硬件》《执行单元与存储》的子块一致
* 机制全部实现，数值按 bit 级与参考实现比对
* 《建模参数与性能模型》的参数直接成为各单元的输入

本章不复述前面几章的规则，只写模型怎么承载它们。

《Bach 硬件设计建模参考》第 7 章，全套目录见 [README](README.md)。

***

## 建模范围与对象清单

### 四条总决定

1. **逐拍 cycle 模型**：功能与时序一起建，每个仲裁点、每条通路都有拍数。
2. **机制全覆盖**：第 2 至 4 章定义的每一条机制都落到一个单元的一个函数，并有一个用例；覆盖关系在各单元文档的“机制覆盖”表里逐条登记。
3. **bit 级数值**：MU 的累加顺序、MXFP8 的 scale block、VU 的三处舍入、Router reduce 的 FP32 中间精度都按硬件规则实现；参考实现按同样的顺序计算，逐元素比对不留容差。
4. **RV core 跑真实 RV32 程序，不建流水线**：kernel 编译成 RV32 程序，由 `src/rv32` 的功能模型逐条执行，每条指令 1 拍，访存与 DSA 读的延迟另计；task_queue、dsa_iss、CSR、task_done 这些与 TS 和 DSA 交互的机制照建。

### 对象清单

“形态”一列：模块 = 独立打拍的 `ClkModule`；装配 = 只做构造与接线的容器；静态 = 仿真前算好的表；桩 = 只模仿接口行为的模块。

| 层 | 对象 | 对应设计 | 形态 |
| - | - | - | - |
| 系统 | GPU / DPU 桩 | 第 6 章“GPU → Bach 的两层 credit 反压”、第 2 章 Node 组成 | 桩 |
| 系统 | ETH 链路、PCIe 链路、C2C 链路 | 第 2 章互连参数、第 5 章延迟表 | 模块（带宽、延迟、到达时刻） |
| 系统 | PCIe Switch | 第 2 章 Node 组成、第 6 章两层 credit | 模块 |
| 系统 | 出口桩 | 第 6 章输出包格式 | 桩 |
| chip | Chip（2×5 阵列、Harvest mask、C2C 端口） | 第 2 章“Chip 与 Harvest” | 装配 |
| chip | SCP 与 ctrl_noc | 第 2 章 Boot、第 3 章 Bach Core 顶层 | 桩 + 配置总线 |
| core | Core | 第 3 章 Bach Core 顶层 | 装配 |
| core | Router：RouterTable 与 CSR、RouterStation ×3、Xbar、CoreStation、CoreMem 重发、ReduceModule、Retire、CoreMemCreditMonitor | 第 3 章 Router | 模块 ×8 |
| core | TS：CFG_REG、User_Match、DataIn_task_table、Stream_table、Task_ctrl、MU_Arb / VU_Arb、DTE_Arb、Credit_monitor、Task_done | 第 3 章 TS 任务调度器 | 模块 ×9 |
| core | RV core ×3：task_queue、指令执行器（`src/rv32`）、dsa_iss、访存（sm_lsq / cm_lsq）、CSR | 第 3 章 RV Core | 模块 ×3 |
| core | DTE：Header Parser、Commit、TaskQueue ×4、Lane ×4（含 AGCU）、中间 Buffer、Completion RS、Hmem 与 LUT、topK 与 shareMem 写 | 第 4 章 DTE DSA | 模块 ×8 |
| core | MU：regfile、issue_q、gen_ep_info、agu ×3、ldq ×2、matrix exe、stq | 第 4 章 MU DSA | 模块 ×7 |
| core | VU：config_register、ISQ、pipe_ctrl 与 Scoreboard、LU、SU、SMUX / DMUX、VALU0 / VALU1 / VALU2 / VSFU、MEXE、SEXE、VRF / MRF / SRF、Profile | 第 4 章 VU DSA | 模块 ×11 |
| core | Core Mem、Matrix Mem、Share Mem 及各自的仲裁器 | 第 4 章存储子系统 | 模块 ×3 |
| 静态 | 拓扑与地址空间、切分与权重分配、路由表与任务链生成、参考实现 | 第 2 章切分、第 6 章编译器产物 | 静态（编译侧） |

### 本轮不建的部分

* Host CPU、Node CPU、Board CPU 的业务流控与退出、动态专家调度（第 2 章“业务流控与退出”）。LPU Dispatch 的派遣规则（所有 R core 有余量才派遣）放在 GPU / DPU 桩里。
* Debug Module、DTM、GDB。
* 异常、ECC、看门狗、功耗类机制（RV core 九类异常、MU Drain & Trap、VU error_code、TS Except_Check、DTE 首错保留、DIDT 分级、零输入门控、MU 与 VU 错峰）。各单元给这些机制留出状态位与接口名，本轮不实现其行为。
* RV core 的流水线细节：pc_gen、loop_bp、decode、dispatch、双发射、gpr 端口、SEU 的乘除多拍、DTCM 的 bank 冲突。它们折算成每条指令 1 拍。

***

## 建模方式

### 逐拍 cycle 模型

latch 的建模方法分两段：功能模型（`src/rv32`：逐条指令执行，无时序）与逐拍 cycle 模型（手写模块，直接用 runtime 原语）。

Bach core 里的 TS 按 stream 年龄仲裁发射、三块存储按 bank 仲裁多 master、Router 有 credit 闭环与多播原子准入、VU 有 Scoreboard，这些都要逐拍表达优先级排队与原子准入。因此 Bach 建成逐拍 cycle 模型，范本是 `src/module/llc.h` 与 `src/module/rv32_pipeline.h`，遵循 `module_writing_guide.md` 的全部规则。RV core 是唯一的例外：它把 `src/rv32` 的功能模型当指令执行器，外面包一层逐拍的 task_queue、dsa_iss、lsq 与延迟记账。

### 挂时钟粒度：图上的每个模块独立打拍

硬件框图里的每个模块都是一个 `tick=true` 的 `ClkModule`，各自持有推进它的协程：Core 内的 Router 八个模块、TS 九个模块、三个 RV core、DTE / MU / VU 的各模块、三块存储；Core 外的 PCIe Switch、链路、GPU / DPU 桩、出口桩、SCP 桩。Core 与 Chip 不是模块，是装配容器：构造各模块、按各单元文档的接口把端口对接起来，自身没有 `Cycle()`。

一个模块一拍做的事全部写在它的 `Step()` 里：读入口端口上一拍锁存的值，算本拍的组合逻辑，把结果写到出口端口，下拍对方才看得到。`Cycle()` 只做起手的 `DelayCycle(1)` 再调 `Step()`，`Step()` 里一次也不许让出。模块之间没有调用关系，同一拍里各模块的执行顺序不影响结果。这条分工写在 `ip/module_base.h` 的基类里。

波形层次是 `chip<i>.core<j>.<单元>.<模块>.<信号>`。常驻协程数等于模块数，48 chip 的规模见“风险”。模块单测时直接拉起该模块和驱动它的测试桩，不需要装配整个 Core。

### 等待改写成跨拍状态机

`Cycle()` 体内除起手的 `DelayCycle(1)` 外不得再 yield，否则该协程挂起会卡住 OldestStamp。硬件里的每一处等待都写成跨拍状态机，每拍判断一次：

| 单元 | 等待点 | 状态机 |
| - | - | - |
| TS | 等前序 task 完成、等 datain、等 credit、等 RV core 空闲、等 `raw ACCEPT` | 每个 stream 表项一个 `task_fsm`：IDLE → WAIT → RDY → INFLY → FINISH |
| RV core | 等 task、等源 gpr 就绪（DSA 读、访存返回）、等 lsq 与 dsa_iss 有空 | 执行状态加 gpr 就绪表 |
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

### 跨模块按硬件的握手协议与打拍规则

模块之间只通过端口相连。端口上的每个信号都是上拍写、下拍读的寄存器（`Logic64` / `LogicCell`，在端口束里 `Fields(...)` 注册）；在 latch 里一个端口组实现成一组 `Logic` 字段或一个深度 1 的 `Fifo`，两者都是上拍写下拍读，选哪个是实现细节，不出现在接口声明里。一个端口组用什么协议，照该接口在第 2 至 4 章里的定义：

| 协议 | 用在 | 规则 |
| - | - | - |
| valid/ready | TS ↔ RV core 的 task 下发、RV core → DSA 的配置下发、DSA / RV core ↔ 存储的请求、Xbar 的出口授予 | 发送方拉 valid 并保持数据不变直到看见 ready；接收方的 ready 是它上一拍锁存的值（等价于硬件里带 skid buffer 的注册 ready），一次握手最少两拍 |
| credit / release | Router 链路的 VC credit、Stream credit、Reduce credit；CoreStation ↔ DTE 的 VC credit；DTE 中间 Buffer 的 credit | 发送方本地扣 credit 后才发，不等对方应答；release 经独立通道回传，回传也打一拍 |
| AXI-Stream-like | CoreStation ↔ DTE 的 DataIn / DataOut | valid/ready 加 last 与有效字节数；反压时数据保持 |
| 脉冲 | credit_issue_pulse、reduce_done、trigger、Retire 广播、task_done | 单拍有效，接收方当拍锁存 |
| 电平 | ready 全高、busy、状态寄存器 | 持续有效，接收方任意拍读 |
| ctrl_noc 写事务 | SCP → core 内寄存器 | 每笔事务一拍，按地址分发到目的模块的 `cfg` 端口 |

链路上的封包按 flit 传，flit 是 `Logic` 的派生类，`LogicPtr<Message>` 让同一 Message 的多个 flit 共享 payload 字节（bit 级比对的依据）；Message 的 Header 与 Payload 合成一个 flit 序列，首 flit 携带 Header。发包时在 `Step()` 体内现建一个 flit，写完字段再送进端口，不复用成员：`Latch::Get()` 对本拍未写的字段回落到上一拍的值，复用会把旧字段带出去。

`Fifo` 是模块内部的存储结构（VC Buffer、TaskQueue、ISQ、链路的在途队列），不是模块的接口。

### 可观测量与禁用清单

跨拍可读的计数、占用与标志一律用 `Logic64`，配一个 cycle 内的裸 `uint64_t` 累加器，在 `Step()` 末尾一次性 commit。`Logic64` 一拍只能 Set 一次，同拍多次 Set 触发断言。

`Logic64::Get()` 在主线程读到的是 t=0 的值。因此完成集合、结束时刻、credit 余额这些验收产物必须在协程内 snapshot 到普通变量，不能在 `JoinAll()` 之后直接读。

不出现 `std::mutex`、`std::lock_guard`、`std::condition_variable`、`std::atomic`。

***

## 模型结构

### 目录按对象清单

沿用 `src/bach/` 现有的目录组织。`ip/` 下每个头文件对应对象清单里的一个模块；Python 版模拟器遗留的单元（`credit_unit.h`、`moe_bitmap.h`、`compute/`、`eth_switch/`、`external/phase*`、`dispatcher.h`）保留不动，新单元不依赖它们。

```
src/bach/
  ip/
    module_base.h                  模块基类：Cycle() = DelayCycle(1) + Step()，Step() 内不让出
    node_context.h                 每个节点都有的编号、参数表与记录器
    route_config.h                 外部节点与跨 chip 网关的坐标换算表
    wiring.h                       接一条双向物理链路（数据端口对 + 三种 release 端口）
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
        core.h                     装配容器：构造 core 内全部模块，按各单元文档的接口对接端口
        core_context.h             各模块共用的只读上下文（core id、角色、参数表）
        ports.h                    端口束：valid/ready、credit/release、AXI-Stream-like、脉冲、电平各一种字段结构
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
          rv_core.h                驱动 src/rv32 的 SystemRv32 逐条执行；task_queue、dsa_iss、dsa_rq、lsq、gpr 就绪表、自定义 CSR
          bach_insts.h             custom-0 自定义指令（dsar、dsari、dsaw.s、dsaw.d、dsawi.s、dsawi.d、task_done、flag_check、loop）
          rv_ports.h               RV core 地址空间：ITCM、DTCM、Share Mem、Core Mem、Router I/O reg 各一个 MemoryPort
          kernel/
            kernel_api.h           kernel 源码侧头文件：自定义指令的 inline asm 封装、DSA 寄存器地址、自定义 CSR 编号
            *.c  link.ld  Makefile 各 kernel（bcore_datain、check_flag、broadcast、weights loader、计算 core 各 task），riscv gcc 编译成每类 core 一个 ELF
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

模块之间只有端口。装配容器把生产者的出口端口和消费者的入口端口对接，两侧都只看到端口束的字段，不持有对方的类型，装配顺序不受构造顺序牵制。

`test/bach/ip/` 按 `ip/` 的一级镜像分目录，每个模块旁边有它自己的测试。

### 物理归属

**Router 属于 Core。**

* Core 构造时创建各模块，Router 的八个模块是其中一组，每个 Core 一份
* 片内 mesh 的连线动作在 Chip 里做，但被连的端口长在 RouterStation 上
* 坏核只构造 Router 的模块，不构造 TS、RV core、DSA 与存储

**PCIe Switch 属于 node。**

* 它是片外的交换节点，坐标是不与核阵列重叠的锚点
* 端口由拓扑起名，核那一侧仍是 chip 边界四个 C2C 端口之一
* 两侧各用各的端口标识，接线时对接

**GPU / DPU 桩、出口桩、SCP 桩属于 system。**

* 它们的坐标不在核阵列内，登记在外部节点表里
* GPU / DPU 桩与出口桩挂在 PCIe Switch 上，各自带一个路由器
  * 包先进自己那个路由器的本地口，由它按链路的带宽与延迟送到网关核
  * 回来的包也在这个路由器上落地重组，再交给设备本身
* SCP 桩每 chip 一个，只接 ctrl_noc

### 接口的声明方式

每个模块的接口按《硬件电路设计描述规范》的“接口”模板声明在该单元的文档里：每个端口组一个声明块，注明 master / slave、协议、时钟域，valid/ready 组注明 ready 的成立条件；信号名与代码一致。本章不汇总各口。

***

## 各单元的建模规格

每个单元一份文档，放在 `07-units/` 下，按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）的六章写：定位与边界（第 0 层图：本单元的模块与邻居）、接口（每个端口组一个声明块）、存储器（含级间 latch）、流水线总览（第 1 层图）、逐级行为（第 2 层图，每级四要素）、参数汇总；之后加本章要求的两段：机制覆盖（“落点”是模型里承载该机制的模块与函数，“用例”是 `test/bach/ip/` 下的测试名）、参数与简化（设计未给值的参数写默认值并标“待定”，全部待定值汇总在本章末尾）。图一律手写 SVG，用该规范的 stencil；上层盒子名 = 下层图标题，上层箭头上的信号名 = 下层图的端口组名。

### 总结构图

Core 的第 0 层图：Core 内全部单元与它们之间的端口组。每个盒子对应一份单元文档，盒子里列的是该单元独立打拍的模块；箭头上的名字是端口组名，在两侧单元文档的“接口”章里各有一个声明块。Core 外的 GPU / DPU 桩、链路与 PCIe Switch、Chip / SCP 在各自单元文档的第 0 层图里。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1240 900" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs>
    <marker id="c0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker>
    <marker id="c0s" markerWidth="9" markerHeight="9" refX="1" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#475569"/></marker>
  </defs>
  <rect x="0" y="0" width="1240" height="900" fill="#ffffff"/>
  <text x="20" y="30" font-size="12" fill="#111827">Bach Core · 第 0 层</text>

  <!-- 对外端口 -->
  <polygon points="560,50 660,50 650,90 550,90" fill="#f8fafc" stroke="#374151"/>
  <text x="605" y="74" font-size="10.5" fill="#374151" text-anchor="middle">data_UD</text>
  <polygon points="30,180 130,180 120,220 20,220" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="204" font-size="10.5" fill="#374151" text-anchor="middle">data_L</text>
  <polygon points="1120,180 1220,180 1210,220 1110,220" fill="#f8fafc" stroke="#374151"/>
  <text x="1165" y="204" font-size="10.5" fill="#374151" text-anchor="middle">data_R</text>
  <polygon points="30,800 130,800 120,840 20,840" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="824" font-size="10.5" fill="#374151" text-anchor="middle">scp_ctrl</text>

  <!-- Router -->
  <rect x="200" y="120" width="380" height="150" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="216" y="144" font-size="12" fill="#111827">Router</text>
  <text x="216" y="166" font-size="10" fill="#475569">RouterStation ×3 · Xbar · CoreStation</text>
  <text x="216" y="182" font-size="10" fill="#475569">ReduceModule · RouterTable / CSR</text>
  <text x="216" y="198" font-size="10" fill="#475569">CoreMemCreditMonitor · Retire · CoreMem 重发</text>
  <text x="216" y="222" font-size="9.5" fill="#9ca3af">三个方向各 256 B/T，进 core 与出 core 并行</text>
  <line x1="605" y1="92" x2="520" y2="118" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <line x1="132" y1="200" x2="198" y2="200" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <polyline points="1108,200 1000,200 1000,100 590,100 590,118" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="640" y="96" font-size="9" fill="#6b7280">flit + vc_release / stream_release / reduce_release</text>

  <!-- TS -->
  <rect x="760" y="120" width="380" height="150" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="776" y="144" font-size="12" fill="#111827">TS 任务调度器</text>
  <text x="776" y="166" font-size="10" fill="#475569">CFG_REG · User_Match · DataIn_task_table</text>
  <text x="776" y="182" font-size="10" fill="#475569">Stream_table · Task_ctrl · DTE_Arb / MU_Arb / VU_Arb</text>
  <text x="776" y="198" font-size="10" fill="#475569">Credit_monitor · Task_done</text>
  <text x="776" y="222" font-size="9.5" fill="#9ca3af">stream 16 项，任务链 64 项</text>
  <line x1="582" y1="170" x2="758" y2="170" stroke="#475569" marker-end="url(#c0)"/>
  <text x="670" y="164" font-size="9" fill="#6b7280" text-anchor="middle">trigger · credit_pulse · reduce_done</text>
  <line x1="758" y1="240" x2="582" y2="240" stroke="#475569" marker-end="url(#c0)"/>
  <text x="670" y="256" font-size="9" fill="#6b7280" text-anchor="middle">credit_req · stream_credit_return · retire</text>

  <!-- RV core ×3 -->
  <rect x="200" y="340" width="220" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="216" y="364" font-size="12" fill="#111827">DTE RV core</text>
  <text x="216" y="384" font-size="10" fill="#475569">src/rv32 · ITCM 4 KB · DTCM 8 KB</text>
  <text x="216" y="400" font-size="10" fill="#475569">task_queue · dsa_iss · sm_lsq · cm_lsq</text>
  <rect x="480" y="340" width="220" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="496" y="364" font-size="12" fill="#111827">MU RV core</text>
  <text x="496" y="384" font-size="10" fill="#475569">src/rv32 · ITCM 4 KB · DTCM 8 KB</text>
  <text x="496" y="400" font-size="10" fill="#475569">task_queue · dsa_iss · sm_lsq</text>
  <rect x="760" y="340" width="220" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="776" y="364" font-size="12" fill="#111827">VU RV core</text>
  <text x="776" y="384" font-size="10" fill="#475569">src/rv32 · ITCM 4 KB · DTCM 8 KB</text>
  <text x="776" y="400" font-size="10" fill="#475569">task_queue · dsa_iss · sm_lsq</text>

  <!-- TS ↔ RV core -->
  <polyline points="850,272 850,300 310,300 310,338" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <polyline points="870,272 870,310 590,310 590,338" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <line x1="890" y1="272" x2="890" y2="338" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="600" y="296" font-size="9" fill="#6b7280" text-anchor="middle">task_cmd / task_ack · task_done</text>

  <!-- Share Mem -->
  <rect x="1040" y="340" width="170" height="90" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="1044" y="344" width="162" height="82" fill="none" stroke="#374151"/>
  <text x="1056" y="366" font-size="12" fill="#111827">Share Mem</text>
  <text x="1056" y="386" font-size="10" fill="#475569">smem · SRAM 32 KB</text>
  <text x="1056" y="402" font-size="10" fill="#475569">RV ×3 + DTE 写 · 仲裁</text>
  <line x1="982" y1="385" x2="1038" y2="385" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <polyline points="700,385 730,385 730,320 1010,320 1010,395 1038,395" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <polyline points="420,385 450,385 450,325 1020,325 1020,405 1038,405" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="1010" y="380" font-size="9" fill="#6b7280" text-anchor="end">sm_lsq ×3</text>

  <!-- DSA ×3 -->
  <rect x="200" y="500" width="220" height="110" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="216" y="524" font-size="12" fill="#111827">DTE DSA</text>
  <text x="216" y="544" font-size="10" fill="#475569">Header Parser · Commit · TaskQueue ×4</text>
  <text x="216" y="560" font-size="10" fill="#475569">Lane ×4（AGCU）· Buffer · Completion RS</text>
  <text x="216" y="576" font-size="10" fill="#475569">Hmem / LUT · topK / shareMem 写</text>
  <rect x="480" y="500" width="220" height="110" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="496" y="524" font-size="12" fill="#111827">MU DSA</text>
  <text x="496" y="544" font-size="10" fill="#475569">regfile · issue_q · gen_ep_info</text>
  <text x="496" y="560" font-size="10" fill="#475569">agu ×3 · ldq ×2 · matrix exe · stq</text>
  <text x="496" y="576" font-size="10" fill="#475569">32 lane × 10 级</text>
  <rect x="760" y="500" width="220" height="110" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="776" y="524" font-size="12" fill="#111827">VU DSA</text>
  <text x="776" y="544" font-size="10" fill="#475569">config_register · ISQ · pipe_ctrl</text>
  <text x="776" y="560" font-size="10" fill="#475569">LU · SU · MUX · VALU ×3 · VSFU</text>
  <text x="776" y="576" font-size="10" fill="#475569">MEXE · SEXE · VRF / MRF / SRF · Profile</text>

  <!-- RV → DSA, DSA → TS -->
  <line x1="310" y1="432" x2="310" y2="498" stroke="#475569" marker-end="url(#c0)"/>
  <line x1="590" y1="432" x2="590" y2="498" stroke="#475569" marker-end="url(#c0)"/>
  <line x1="870" y1="432" x2="870" y2="498" stroke="#475569" marker-end="url(#c0)"/>
  <text x="600" y="470" font-size="9" fill="#6b7280" text-anchor="middle">dsa_cfg（dsaw / dsar）</text>
  <polyline points="422,530 460,530 460,455 1005,455 1005,272" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#c0)"/>
  <polyline points="702,530 740,530 740,455" fill="none" stroke="#475569" stroke-dasharray="4 3"/>
  <polyline points="982,530 1005,530 1005,455" fill="none" stroke="#475569" stroke-dasharray="4 3"/>
  <text x="1010" y="450" font-size="9" fill="#6b7280">dsa_done ×3（脉冲）</text>

  <!-- Router ↔ DTE DSA / DTE RV -->
  <polyline points="230,272 160,272 160,555 198,555" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="165" y="420" font-size="9" fill="#6b7280" transform="rotate(-90 165 420)" text-anchor="middle">hdr · datain · dataout（+ vc credit）</text>
  <polyline points="250,272 180,272 180,400 198,400" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="185" y="300" font-size="9" fill="#6b7280" transform="rotate(-90 185 300)" text-anchor="middle">io_reg</text>

  <!-- Core Mem / Matrix Mem -->
  <rect x="200" y="700" width="380" height="110" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="204" y="704" width="372" height="102" fill="none" stroke="#374151"/>
  <text x="216" y="726" font-size="12" fill="#111827">Core Mem</text>
  <text x="216" y="746" font-size="10" fill="#475569">cmem_bank ×8 · SRAM 1024×128 B + scale 1024×4 B</text>
  <text x="216" y="762" font-size="10" fill="#475569">6 个 master 口 · 每 bank 独占仲裁 · (1 KB + 32 B)/T</text>
  <text x="216" y="784" font-size="9.5" fill="#9ca3af">stream_id 分片由地址计算侧完成</text>
  <rect x="620" y="700" width="360" height="110" fill="#f8fafc" stroke="#374151" rx="4"/>
  <rect x="624" y="704" width="352" height="102" fill="none" stroke="#374151"/>
  <text x="636" y="726" font-size="12" fill="#111827">Matrix Mem</text>
  <text x="636" y="746" font-size="10" fill="#475569">mmem_bank ×32 · SRAM 1 MB + scale 128 KB / bank</text>
  <text x="636" y="762" font-size="10" fill="#475569">DTE 读写 · MU 只读（lane 一对一）· ctrl_noc · (8 + 1 KB)/T</text>

  <!-- DSA ↔ Mem -->
  <line x1="290" y1="612" x2="290" y2="698" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="296" y="660" font-size="9" fill="#6b7280">cmem_dte_rd / wr 256 B</text>
  <polyline points="380,612 380,650 700,650 700,698" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="560" y="646" font-size="9" fill="#6b7280" text-anchor="middle">mmem_dte_rd / wr 256 B</text>
  <polyline points="540,612 540,630 500,630 500,698" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="504" y="690" font-size="9" fill="#6b7280">cmem_mu_rd / wr 132 B</text>
  <line x1="640" y1="612" x2="760" y2="698" stroke="#475569" marker-start="url(#c0s)"/>
  <text x="716" y="640" font-size="9" fill="#6b7280">mmem_mu_rd 8 KB + 1 KB</text>
  <polyline points="820,612 820,670 560,670 560,698" fill="none" stroke="#475569" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="700" y="684" font-size="9" fill="#6b7280" text-anchor="middle">cmem_vu_ld / st 1056 bit</text>
  <polyline points="240,432 240,470 140,470 140,720 198,720" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-start="url(#c0s)" marker-end="url(#c0)"/>
  <text x="145" y="600" font-size="9" fill="#6b7280" transform="rotate(-90 145 600)" text-anchor="middle">cm_lsq</text>
  <polyline points="422,590 1000,590 1000,432" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#c0)"/>
  <text x="900" y="586" font-size="9" fill="#6b7280">sm_wr</text>

  <!-- ctrl_noc -->
  <rect x="30" y="700" width="90" height="70" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="40" y="722" font-size="11" fill="#111827">ctrl_noc</text>
  <text x="40" y="740" font-size="10" fill="#475569">端点</text>
  <text x="40" y="756" font-size="10" fill="#475569">32 bit/T</text>
  <line x1="75" y1="798" x2="75" y2="772" stroke="#475569" marker-end="url(#c0)"/>
  <polyline points="75,698 75,640 100,640 100,120 200,120" fill="none" stroke="#475569" stroke-dasharray="2 3" marker-end="url(#c0)"/>
  <text x="104" y="135" font-size="9" fill="#6b7280">cfg 写事务 → 各模块的 cfg 端口</text>
</svg>
```

三条读图的提示：

* Router 的三个 R2R 方向端口（left / right / mid）接 mesh 上相邻 core 的 Router，或 Chip 边界上的 C2C 链路
* Core Mem 与 Matrix Mem 只有 DTE、MU、VU 与 ctrl_noc 这几个 master
* ctrl_noc 的配置写事务按地址分发到每个模块的 `cfg` 端口，图上只画到端点

### 单元文档索引

| 编号 | 单元 | 文档 |
| - | - | - |
| 1 | GPU / DPU 桩 | [`07-units/01-gpu-dpu-stub.md`](07-units/01-gpu-dpu-stub.md) |
| 2 | 链路与 PCIe Switch | [`07-units/02-link-pcie-switch.md`](07-units/02-link-pcie-switch.md) |
| 3 | Chip、SCP 与 ctrl_noc | [`07-units/03-chip-scp-ctrl-noc.md`](07-units/03-chip-scp-ctrl-noc.md) |
| 4 | Router | [`07-units/04-router.md`](07-units/04-router.md) |
| 5 | TS 任务调度器 | [`07-units/05-ts.md`](07-units/05-ts.md) |
| 6 | RV Core（DTE / MU / VU 各一） | [`07-units/06-rv-core.md`](07-units/06-rv-core.md) |
| 7 | DTE DSA | [`07-units/07-dte.md`](07-units/07-dte.md) |
| 8 | MU DSA | [`07-units/08-mu.md`](07-units/08-mu.md) |
| 9 | VU DSA | [`07-units/09-vu.md`](07-units/09-vu.md) |
| 10 | 存储子系统 | [`07-units/10-memory.md`](07-units/10-memory.md) |

## 输入

模型读入四类东西，格式由《software/》下的文档定义，本章只规定内容要求。

| 类 | 内容 | 来源 |
| - | - | - |
| 拓扑与部署 | rack 数、chip 形状 2×5、Harvest mask、逻辑 ↔ 物理 core 映射、切分参数（EP / TP / PP / DP 与四种模式之一）、chip 数 48、GPU 数与每 GPU 的 batch | 编译侧 |
| 每 core 配置 | RouterTable（每 path 一表项、三份副本一致）、Credit Bypass Route、task_chain（≤ 64 项，含软件属性 `exe_dest` / `task_group_id` / `reduce_num`）、datain_task、`stream_num`、`CORE_TYPE`、`B_core_direction`、`trigger_task_chain_en`、DTE 包头表（硬件包头静态表 64 项、软件包头 16 × 64 项）、MU `local_ep_table`、VU 8 组静态配置、Core Mem 的 reissue 预留空间 | 编译侧 |
| kernel 镜像 | 每类 core 一个 RV32 ELF（代码段进 ITCM、数据段进 DTCM），与 task_pc → kernel 入口地址表 | 编译侧 |
| 数据 | 每 core 27 MiB 权重分片（含共享专家）与落 Matrix Mem 的地址；注入表（每 token 的 6368 B 级联包与注入拍）；参考实现的期望输出 | 编译侧 + `reference/` |

编译侧产物里必须有、不能反推的几样：每个 core 的 RouterTable（同一 path_id 在不同 core 上表项不同）、每个 core 的 task_chain 与 datain_task、每 core 的权重分片与角色、坏核 mask、`CreditCounter[path_id][stream_id]` 初值（广播 = 目的 core 数，P2P = 1）。

参数表 `common/params.h` 是全部拍数、带宽、深度的唯一出处，每个值标注来历：MAS 给的、性能需求规格说明书给的、第 8 章冲突项按“建议”取的、本章“待定”默认值。

***

## 时间轴与链路

Clock 周期取 1 T，即 1 GHz 下的 1 ns，一拍就是一 T。第 5 章的参数凡是按 T 给的直接变成拍数；按 ns 或 μs 给的（PCIe C2C 300 ns、GPU 注入 3 μs）按 1 T = 1 ns 折算。

拍数换算统一为 `ceil(size / bw)`，且 size 小于等于 0 时算一拍。

latch 的 `Time` 有效范围是 32 位，1 T 一拍下约 4.29e9 拍。一层 FFN 的端到端延迟在 1e4 到 1e5 拍量级，余量充足。

发送侧按链路带宽与延迟算出到达时刻写进 flit 的 `arrive_cycle`，链路模块把 flit 放进自己的在途队列，只有当前拍不早于 `arrive_cycle` 才把它送到出口端口。端口固有的一拍延迟被吸收，时序 owner 仍然唯一是发送侧的链路模型，不额外扣时。R2R 40T、C2C 400T 都远大于一拍，吸收得下。同一条链路上到达时刻单调递增，因为发送侧的占用时刻单调，所以队首不会阻塞更早到达的包。这一条加断言。

***

## 输出与观测

波形走 latch 原生的 Trace 与 insight，层次为 `chip<i>.core<j>.<单元>.<模块>.<信号>`。

另出一个目录，三个事件桶加一份来源信息：

| 文件 | 内容 |
| - | - |
| `unit_spans.jsonl` | 单元占用区间：node_id、unit、user_id、task_id、start、end、state、volume |
| `unit_waits.jsonl` | 等待区间、归因 reason，以及它属于依赖还是资源 |
| `global_latency.jsonl` | 每个 `(gpu_id, token_id)` 的端到端延迟、经过的 core 序列与每段时刻 |
| `run_meta.json` | 编译侧产物的来源、参数快照、结束时刻、完成与丢失的 token、credit 终态 |

`run_meta.json` 不是可选的。没有它，一份观测产物没法追回是哪套切分、哪套参数跑出来的，结论就没有适用范围。

等待归因按模块细化，分依赖与资源两类：

| 类 | reason |
| - | - |
| 依赖 | 等前序 task 完成、等 datain 数据到达、等 Router `reduce_done`、等 DSA 读寄存器返回、等 Completion RS 的 Join、等 Scoreboard 依赖、等 B core 的 head ≠ tail、等 R core 的 arrive_num == 2 |
| 资源 | 等 stream 坑、等 VC credit、等 Reduce credit、等 GPU grant、等 TaskQueue / ISQ / issue_q 项、等 Lane、等 bank 端口、等 RV core 空闲、等 dsa_iss 通道、等 DTE Buffer credit、等 Xbar 出口、等 ReduceModule 上下文 |

瓶颈由此能定位到具体资源而不只是慢，而且只有资源那一半是改参数动得了的。

两条发射端的过滤规则：占用区间丢弃 end 小于 start 的，以及 end 等于 start 且没有显式允许零长的；等待区间丢弃 end 不大于 start 的。被丢的条数单独计数，用来区分“没记到”与“没发生”。

事件在仿真期记在各模块自己的裸 vector 里，`JoinAll()` 之后由主线程汇总落盘。`Cycle()` 里不做文件 IO。汇总时按节点与时刻排全序，否则收集顺序取决于线程调度，产物无法逐行 diff。

***

## 建模顺序

本节留空，待各单元的结构图完成后再定。

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
| 边界与异常 | Stream 耗尽正确排队；Credit 耗尽正确阻塞上游；ready 拉低正确背压；无效 path_id / 重复 User ID 被拒 |
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

驱动本身是 `ClkModule`，通过对方声明的端口交互；至少一个 `sub_thread` 大于 1 的并发用例；每个用例连跑二十次无 flake；断言的量在协程内 snapshot，不在 `JoinAll()` 之后读 `Logic64`。机制覆盖表里的每个用例名对应一个测试函数，覆盖率以“机制覆盖表里没有用例为空的行”为准。

***

## 边界与风险

### 简化

* **RV core 不建流水线**：kernel 是真实 RV32 程序，每条指令 1 拍，访存与 DSA 读的延迟记在 gpr 就绪表上；双发射、分支预测、流水冲刷、乘除多拍、DTCM bank 冲突、gpr 端口竞争不体现。
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
| custom-0 自定义指令的字段布局 | funct3 按第 3 章表；rd / rs1 / rs2 / imm 按 R 型与 I 型标准布局，等 ISA 描述表到手后改 `bach_insts.h` |
| MU、DTE 的寄存器地址映射 | `regmap.h` 临时映射 |
| Mmem MU 读延迟 | 8T |
| Cmem 的 MU 写延迟 | 16T |
| `operation` 的 Reduce0 / Reduce1 / Reduce2 含义 | 源分量 / 中继累加 / 最终汇聚 |
| `exe_dest`、`task_group_id`、`reduce_num` 的承载 | task_chain 的软件侧属性 |

### 风险

**规模。** 每拍一次全局 barrier，同步点数量等于模块数。48 chip 按每 core 约五十个模块是两万多个协程（槽位上限 65535），每拍开销未实测。应对写在 `module_base.h`：先在单 chip 上测出每拍开销，过大就让 Core 或 Chip 挂时钟、在它的 `Cycle()` 里顺序调各模块的 `Step()`，模块代码不动；因为跨模块信号全部打拍，两种驱动方式的结果逐拍相同。

**状态机改写的语义等价。** 把硬件里的等待写成跨拍状态机是工作量最大、也最容易引入语义差异的一块。每个状态机要有对照 MAS 时序图的单测，尤其是 Reduce 完成的无序汇合、多播原子准入、Retire 的三方时序、DTE 的配对接纳与 Join 这四条跨阶段持有的语义。

**bit 级一致的累加顺序。** MU 的 CSA 树、VSFU 的查表拟合、VU 的归约树顺序在 MAS 里只给了原则没给细节，参考实现与模型只能按同一份 `numeric/` 实现对齐，与真实硬件是否一致要等 RTL 出来核对。

**握手多出的拍数。** 跨模块信号全部打拍，硬件里同拍完成的组合握手在模型里最少两拍。第 2 至 4 章给了时序图的接口按时序图核对拍数；没给的按注册 ready 建，并在等待归因里单列，便于校准。

***

本章依据第 2 至 6 章整理；建模方式、建模顺序与验收判据以本章为准。
