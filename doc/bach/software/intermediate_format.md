# Bach 中间文件格式

> **文档模式：** 规格。只给格式规则，不含动机与取舍。
> **文档层级：** 详细实现。它只服务于 Bach 在 latch 上的重建这一件事。
> **目的：** 给写生成侧（Python）与解析侧（C++）的实现者，定下这份文件的编码、记录类型、字段含义与校验规则。两侧按同一份规则编解码，两边跑的才是同一份配置。
> **后缀：** `.bachir`

***

## 1. 文件形态

* UTF-8 编码，LF 换行
* 一行一条记录：行首是记录标签，其后是字段，字段之间用一个或多个空格分隔
* 首行必须是版本行：

```
BACHIR 6
```

* 版本号是单个整数，格式的任何变化都让它加一。解析器只接受它认识的那个版本号，不做向前或向后兼容
* 第二行必须是档次行：

```
PROFILE cycle
```

* 取 `task_level` 或 `cycle`，一份文件只属于一档：

| 取值 | 这一档的模型怎么读 |
| --- | --- |
| `task_level` | 每个 core 的活按任务序列给，单元的执行时间直接写在任务行上 |
| `cycle` | 每个 core 的活按硬件寄存器的取值给，执行时间由模型逐拍算出 |

* 两档各自要哪些记录、不许出现哪些记录，见“必需记录”与“两档各自的记录集”两节
* 大块数据不写进这份文件：kernel 镜像、权重分片、注入表与期望输出各自是独立的二进制文件，本文件只登记它们的路径与形状
* 空行忽略，以 `#` 开头的行是注释，忽略
* 字段不含空格。唯一的例外是 `META` 记录的值，它吃到行尾，可以含空格
* 数值一律十进制整数，可带负号。格式里没有浮点：
  * Bach 的仿真时间轴上只出现非负整数
  * 字节数、拍数与延迟都是整数
  * MoE 的权重与 softmax 不进入这份文件，随机决策已经在生成侧算完
* 字符串字段只能由字母、数字、下划线、连字符与点组成

***

## 2. 来源与参数

### 2.1 `META`

```
META <key> <值，吃到行尾>
```

* 只读的来源信息，不影响模型行为
* 解析器把它原样存下，写进观测产物的 provenance
* 已定义的 key：`map_path`、`map_sha256`、`bach_commit`、`generated_at`、`generator`
* 未定义的 key 允许出现，解析器一并存下

### 2.2 `PARAM`

```
PARAM <名字> <整数值>
```

* 覆盖参数表里的同名默认值
* 名字用 `common/params.h` 里的字段名，例如 `dte_setup_time`、`stream_count`、`noc_bandwidth`
* 未定义的名字直接报错：静默忽略一个不认识的参数，等于两边跑的不是同一份配置

### 2.3 `MODE`

```
MODE <名字> <取值>
```

非数值的模式参数：

| 名字 | 取值 |
| --- | --- |
| `dte_execution_mode` | `split` 或 `shared` |
| `dte_dsa_mode` | `off` 或 `five_route` |

***

## 3. 拓扑与每核属性

### 3.1 `DIM`

```
DIM <chip_rows> <chip_cols> <core_rows_per_chip> <core_cols_per_chip> <node_chip_rows> <node_chip_cols>
```

* 恰好一条
* 六个值都是正整数，`node_chip_rows` 整除 `chip_rows`，`node_chip_cols` 整除 `chip_cols`
* Core 总数与全局坐标换算由这六个值推出，不另写进文件

### 3.2 `CORE`

```
CORE <core_id> <group_id> <core_type>
```

* 每个活跃 Core 一条。只有活跃 Core 生成任务表，因此只有活跃 Core 出现在这里
* `group_id`：该核所属的 MoE EPGroup id，非 MoE Map 写 `-1`
* `core_type`：取 `NORMAL`、`BROADCAST`、`REDUCTION` 之一，它决定两件事
  * 上游给该核开的 credit 额度
  * 直接驱动它的 Host 的 credit 容量

### 3.3 `GRID`

```
GRID <chip_id> <tray> <layer> <col>
```

* `cycle` 档每个 chip 一条，`task_level` 档不写
* 全局坐标由三个字段算出：`gy = tray × 4 + layer`，`gx = col`。`gy` 是全局行、`gx` 是全局列，与 `TASK` 行的 `dst_row` / `dst_col` 同一套坐标
* `(gx, gy)` 不重复

### 3.4 `HARVEST`

```
HARVEST <chip_id> <mask>
```

* `cycle` 档每个 chip 一条，`task_level` 档不写
* `mask` 是片内坏核位图的十进制值，第 i 位为 1 表示片内第 i 个 core 是坏核
* 每 chip 至多 2 个坏核；`gx ∈ {0, 3}` 的 chip 至多 1 个
* 坏核不写 `CORE` 记录：它只构造 Router，不领任务

### 3.5 `LOGICAL`

```
LOGICAL <core_id> <logical_core>
```

* `cycle` 档每个好核一条，`task_level` 档不写
* `logical_core` 取 0～8。同一 chip 内 0～7 各出现一次，8 只在 `gx ∈ {0, 3}` 的 chip 上出现
* `logical_core` 为 8 的核，它的 `CORE` 记录里 `core_type` 是 `BROADCAST` 或 `REDUCTION`；0～7 是 `NORMAL`

### 3.6 `SPLIT`

```
SPLIT <ep> <tp> <pp> <dp> <mode> <gpu_num> <batch>
```

* `cycle` 档恰好一条，`task_level` 档不写
* `mode` 取 `EPTP_NN`、`EPTP_NK`、`PPTP_NN`、`PPTP_NK` 之一，是四种 core 级切分方式

### 3.7 `ENTRYEXIT`

```
ENTRYEXIT <in_gx> <in_gy> <out_gx> <out_gy>
```

* `cycle` 档恰好一条，`task_level` 档不写
* 两组坐标是 chip 的全局坐标：外部数据从 `(in_gx, in_gy)` 那颗 chip 的西侧进，结果从 `(out_gx, out_gy)` 那颗 chip 的东侧出
* 权重注入按“最远路径优先”排序时以进口那颗 chip 为起点

***

## 4. 任务表

本章的记录只属 `task_level` 档。

### 4.1 `TASK`

```
TASK <core_id> <task_id> <unit> <opcode> <tag> <up_cid> <down_cid> <dst_row> <dst_col> <time_or_vol>
```

一行是任务表的一个条目。

| 字段 | 含义 |
| --- | --- |
| `core_id` | 所属 Core，必须有对应的 `CORE` 记录 |
| `task_id` | 该 Core 内的行号，从 0 开始连续 |
| `unit` | `DTE`、`MC`、`VC`、`CU`、`SKIP` 之一，决定派给哪个单元 |
| `opcode` | 见下 |
| `tag` | 对 `REDUCE`、`REDUCTION`、`CONCAT`、`RES` 的发送行，存放接收核上对应任务的 task id；其余行为 0 |
| `up_cid` | 仅 `RETIRE` 行有意义，指向要归还 credit 的上游 core id；上游是 Host 时写 `-1` |
| `down_cid` | 仅 `DTE` 行有意义。整数是目标 core id，标识符是某条 `EXTNODE` 的 name |
| `dst_row` `dst_col` | 目标的全局坐标，路由只用这两项 |
| `time_or_vol` | `MC` 与 `VC` 行是计算时间 ns；`DTE` 与 `SKIP` 行是字节数 |

`opcode` 取 `USER_INIT`、`MOVE`、`REDUCE`、`REDUCTION`、`CONCAT`、`RETIRE`、`FIFO_IN`、`FIFO_OUT`、`RES`、`BYPASS`、`DONTCARE` 之一。`REDUCE` 与 `REDUCTION` 是两种任务语义，不能互相替换。

三条读法：

* 路由完全看 `dst_row` 与 `dst_col`：落在核阵列内是某个 Core，落在核阵列外是某个外部节点。`down_cid` 只用于 credit 归属与诊断
* `up_cid` 与 `down_cid` 在与它们无关的行上仍会带一个值，解析器不读那些位置
* `RETIRE` 行的 `up_cid` 为 `-1` 时上游是 Host，`dst_row` 与 `dst_col` 就是该 Host 的坐标

### 4.2 `TMETA`

```
TMETA <core_id> <task_id> <key> <value>
```

任务元数据，稀疏，只有取值非空的键才写。它不参与时间计算，但决定若干运行时分支。

| key | value | 作用 |
| --- | --- | --- |
| `recv_init` | `1` | 该 `RES` 包是本核的真实激活入口 |
| `no_credit_return` | `1` | `RETIRE` 只做本地释放，不发包 |
| `local` | `1` | `RES_SUM` 走本地计算分支 |
| `local_compute` | `1` | 同上 |
| `res_sum_local` | `1` | 同上 |
| `semantic_op` | `moe_send`、`router_softmax_topk`、`concat_router_logits` | 见下 |
| `require_dynamic_hitmap` | `1` | 发包前取运行时 HitMap |
| `dynamic_hitmap` | `1` | 计算完成后生成 HitMap |
| `hitmap_source` | 名字 | 运行时 HitMap 的来源 |
| `dsa_route` | `R2M`、`R2C`、`M2R`、`C2R`、`M2C` | 五路径模式下的显式路径 |
| `dsa_local_transfer` | `1` | `M2C` 的前置条件 |
| `source_endpoint` | `M` 或 `C` | `M2C` 的端点 |
| `destination_endpoint` | `M` 或 `C` | 同上 |
| `wire_tag` | 整数 | `RES` 发送包的 wire tag |
| `payload_role` | 名字 | `BYPASS` 合法性检查读它 |

未定义的 key 报错。

### 4.3 `CREDIT`

```
CREDIT <core_id> <task_id> <target_cid>...
```

* 该 `CU` 行要一次性验资的全部下游，至少一个
* 所有下游的额度同时到位才放行，任何一个堵住整条任务就卡住
* 对应的 `TASK` 行的 `unit` 必须是 `CU`

### 4.4 `CREDITEDGE`

```
CREDITEDGE <src_core> <dst_core> <dst_core_type>
```

* 软件 credit 图的一条边：`src_core` 向 `dst_core` 申请与归还额度
* 额度上限由 `dst_core_type` 决定，取 `NORMAL`、`BROADCAST`、`REDUCTION` 之一
* 这张图与物理连线、与任务表都不互相推导，所以单独给
* 它与 `CREDIT` 说的是两件事：
  * `CREDIT`：某一条验资任务要验哪几个下游
  * `CREDITEDGE`：开了哪些户。有的核一条验资任务都没有，却仍要接收下游退休时还回来的额度

### 4.5 `SKIPSRC`

```
SKIPSRC <core_id> <task_id> <sender_cid> <phase_type> <phase_idx> [group_id...]
```

* 每条屏障 `SKIP` 行的来源
* `group_id` 列表是沿入边树递归收集的上游 group，可能为空
* 运行时用它判断这条入边在本次 HitMap 下会不会真的来包，不会来就自 ack 放行
* 对应的 `TASK` 行的 `unit` 必须是 `SKIP`

***

## 5. 外部节点与互连

### 5.1 `EXTNODE`

```
EXTNODE <name> <kind> <row> <col> <target_core> <volume> <port> <pcie_bw> <pcie_delay>
```

坐标不在核阵列内的设备。

| 字段 | 含义 |
| --- | --- |
| `name` | 标识，`HOSTBIND` 与 `USERTARGET` 按它引用 |
| `kind` | `HOST`、`OUT`、`PHASE1_LANE`、`PHASE1_SINK` 之一 |
| `row` `col` | 全局坐标，不落在核阵列内 |
| `target_core` | 接入的网关 Core |
| `volume` | 字节数 |
| `port` | 接进 mesh 的端口名，或被 PCIe 拓扑回填后的端口 |
| `pcie_bw` `pcie_delay` | 该节点自己的带宽与延迟，`0` 表示用默认值 |

端口名取这十二个之一：`LOCAL`、`NORTH`、`EAST`、`SOUTH`、`WEST`、`PCIE_UP`、`PCIE_DOWN`、`PCIE_NORTH`、`PCIE_SOUTH`、`PCIE_EAST`、`PCIE_WEST`、`UNKNOWN`。

### 5.2 `GATEWAY`

```
GATEWAY <chip_id> <direction> <local_core_id>...
```

* `chip_rules` 解析后的出口网关
* `direction` 取 `LEFT`、`RIGHT`、`TOP`、`BOTTOM`
* `local_core_id` 是片内一维 core id，可以有多个
* 该方向没有网关时不写这条记录

### 5.3 `DIRATTR`

```
DIRATTR <chip_id> <direction> <bandwidth> <delay>
```

覆盖该方向链路的默认带宽与延迟，`0` 表示不覆盖。

### 5.4 `CHIPLINK`

```
CHIPLINK <src_core> <src_port> <dst_core> <dst_port> <bandwidth> <delay>
```

* 一条片间 PCIe 链路的一个方向：从 `src_core` 的 `src_port` 出去，到 `dst_core` 的 `dst_port`
* `bandwidth` 与 `delay` 是发送侧那个端口上的取值，`bandwidth` 为 `0` 表示用默认的 PCIe 带宽
* 每条物理链路写两条记录，两个方向各一条：两端的带宽与延迟取自各自 chip 的方向规则，可以不对称
* 接线关系已经在这里算完，解析侧照表连，不从 `GATEWAY` 与尺寸去推导
* `GATEWAY` 供跨 chip 路由时选本 chip 的出口网关用，与物理链路是两件事

### 5.5 PCIe 交换拓扑

```
PCIESW    <sw_id> <chip> <row> <col>
PCIELINK  <link_id> <a_kind> <a_ref> <a_port> <b_kind> <b_ref> <b_port> <bandwidth> <delay>
PCIEROUTE <sw_id> <dst_row> <dst_col> <out_port>
PCIEIROUTE <sw_id> <in_port> <dst_row> <dst_col> <out_port>
HOSTGROUP <host_name> <group_id> <target_core>
```

`PCIESW`：

* `chip` 是绑定的 chip 序号，未绑定写 `-1`
* `row` 与 `col` 是不与核阵列重叠的锚点坐标
* 同一个 `sw_id` 只能出现一次

`PCIELINK`：

* 端点 kind 取 `SW`、`CORE`、`HOST`、`OUT`，一条链路至少有一端是 `SW`
* `a_ref` 按 kind 取值：`SW` 是交换节点 id，`CORE` 是 core id，`HOST` 与 `OUT` 是 `EXTNODE` 的 name
* 端口字段两侧的写法不同：

| 端点 kind | 端口字段 |
| --- | --- |
| `SW` | Map 给这个口起的名字，任意标识符，不是十二个方向之一 |
| `CORE` | 核那一头的端口，取 `PCIE_UP`、`PCIE_DOWN`、`PCIE_NORTH`、`PCIE_SOUTH`、`PCIE_EAST`、`PCIE_WEST` 之一 |
| `HOST`、`OUT` | 外部设备自己那个路由器上的端口，与 `EXTNODE` 里登记的核侧端口互为反向 |

`PCIEROUTE` 与 `PCIEIROUTE`：

* `out_port` 与 `in_port` 都是交换节点自己的端口名
* 两张表里 `PCIEIROUTE` 优先于 `PCIEROUTE`
* 有 `PCIELINK` 就必须有其中至少一张表的记录：交换节点没有默认路由

`HOSTGROUP` 给出每个 Host 的每个 EPGroup 应该注入哪个 Core：

* 没有 Phase 时，每个 user 实际注入哪个核由 `USERTARGET` 逐条给出，装配只读那一张
* 开着 Phase 的注入时读的是这一张：那时一个 user 进哪个核，要等交换节点把专家换算成 EPGroup 之后才知道
* 一个 Host 只有一个注入目标，所以它名下那几个 group 都指向同一个核

出现在 `PCIELINK` 里的 `HOST` 与 `OUT`：

* 不再由 `EXTNODE` 的 `port` 直接接到网关核上，进出阵列的那条线由 `PCIELINK` 给出
* `EXTNODE` 的 `port` 此时仍然有用：跨 chip 路由要靠它把这个外部节点换算成网关核

### 5.6 Phase1 那一段

```
PHASE1SINK <name> <role>
PHASE1LANE <name> <layer_id> <group_id> <bypass_sink> <moe_sink> <bypass_vol> <moe_vol> <fc0> <res> <norm> <router> <push>
PHASE1HIT  <lane_name> <expert_id>...
PHASE1USER <lane_name> <uid>...
```

通道与落点自己的坐标、网关、端口、包长与链路参数走 `EXTNODE`，`kind` 分别是 `PHASE1_LANE` 与 `PHASE1_SINK`。这里补的是它们的行为。

`PHASE1SINK` 的 `role` 决定这个落点把收齐的包交到以太网交换节点的哪个入口：

| `role` | 交到哪 |
| --- | --- |
| `MOE` | `phase1_moe_ingress` |
| `BYPASS` | `res_ingress` |
| `GENERIC` | 不往下交 |

`PHASE1LANE` 的字段：

| 字段 | 含义 |
| --- | --- |
| `layer_id` | 这条通道处理第几层 |
| `group_id` | 它属于哪个 chip 组，没有写 `-1` |
| `bypass_sink` `moe_sink` | 两路各落在哪个落点，写 `PHASE1SINK` 的 name。这一路不发时写一个减号 |
| `bypass_vol` `moe_vol` | 两路各发多少字节 |
| `fc0` `res` | 发残差那一份之前的两段延迟 |
| `norm` `router` | 发 MoE 请求那一份之前的两段延迟 |
| `push` | 每产一个 user 的间隔 |

两路落点的约束：

* 不能落在同一处：它们在下一跳走的是交换节点的两条不同入口
* 也不能都写减号，那条通道什么也不发

另两条记录：

* `PHASE1HIT`：这条通道选中的那几个专家，交换节点按它换算成 EPGroup。要发 MoE 请求就必须有这一条
* `PHASE1USER`：这条通道负责哪些 uid，顺序就是推进去的顺序。各条通道的 uid 集合互不重叠

### 5.7 以太网交换节点

```
ETHSW    <processing_delay> <header_bytes> <payload_numel> <payload_bytes_per_elem> <expert_group_size> <expert_count>
ETHPORT  <port_id> <bandwidth> <propagation> <queue_cap> <pending_cap>
ETHROUTE <in_port> <out_port>
EXPGROUP <expert_id> <group_id>...
ETHMODE  <boundary> <phase2_ingress> <inject_host> <result_bridge>
```

`ETHSW`：

* 至多一条
* 一包的字节数 = `header_bytes` + `payload_numel` × `payload_bytes_per_elem`，交换节点上的串行时间按它算
* `expert_count` 写 `0` 表示不限

`ETHPORT` 的 `port_id` 取五个固定名字之一，五个各写一条：

| 端口 | 方向 |
| --- | --- |
| `res_ingress` | 入，收残差 |
| `phase1_moe_ingress` | 入，收 MoE 请求 |
| `phase2_moe_result_ingress` | 入，收 Phase2 算完的结果 |
| `phase2_moe_request_egress` | 出，把 MoE 请求交给 Phase2 |
| `phase3_res_join_egress` | 出，把残差与结果交给汇合点 |

* `bandwidth` 的单位是字节每拍
* `queue_cap` 与 `pending_cap` 是出口上排队的两级深度，两级都满就报错，写 `0` 表示用默认值

另两条记录：

* `ETHROUTE`：起点必须是入口、终点必须是出口，一条入口只能有一条出路，至少有一条
* `EXPGROUP`：一个专家落在哪几个 EPGroup 上。不写这张表时按 `expert_group_size` 整除，同一个专家只能写一条

`ETHMODE` 的四个开关取 `0` 或 `1`：

| 开关 | 含义 |
| --- | --- |
| `boundary` | 交换节点自己判 Phase1 边界：残差与 MoE 请求都投出去了算一次工作完成 |
| `phase2_ingress` | 把换算完的 MoE 请求注入 Phase2 的注入源 |
| `inject_host` | 注入时真的把包推给注入源，而不是只记账 |
| `result_bridge` | 汇聚点把算完的结果交回交换节点，与残差汇合 |

开关之间的前置条件：

* `result_bridge` 开着时，另外三条同时成立：
  * `phase2_ingress` 与 `inject_host` 必须开着
  * `boundary` 必须关着
  * `EXTNODE` 里 `kind` 为 `OUT` 的恰好一个
  * 全局完成权只能有一处，`boundary` 与汇合点是同一件事的两种判法
* `phase2_ingress` 开着时：
  * 要有 `HOSTBIND` 说明哪个 EPGroup 归哪个注入源
  * 要有 `HOSTGROUP` 说明推到哪个核

***

## 6. MoE 与预生成的随机决策

本章的记录只属 `task_level` 档。生成侧把 MoE 的分发结果与动态 HitMap 全部算完写进文件，解析侧按 uid 查表。随机数发生器不在解析侧复现。

### 6.1 `EPGROUP`

```
EPGROUP <group_id> <shared_ep_id> <routed_ep_num>
```

`shared_ep_id` 为空时写 `-1`。

### 6.2 `HOSTBIND`

```
HOSTBIND <host_name> <group_id>
```

一个 group 只能绑一个 Host。

### 6.3 `TOTALUSERS`

```
TOTALUSERS <n>
```

恰好一条。一次 run 里有多少个 user。

### 6.4 `TOTALPACKETS`

```
TOTALPACKETS <n>
```

* 恰好一条。一次 run 里各汇聚点一共会收到几包，时钟等这些都到齐才停
* 它与完成判据是两回事：
  * 一个 user 的结果可以落在几个汇聚点上，收到第一个就算它完成了
  * 余下那些仍然会来，只是不再改结论
* 判完成的是以太网交换节点时，这个数没人读，写 `0`

### 6.5 `USER`

```
USER <uid> <src_kind> <src_name>
```

* 每个 uid 一条，给出它由谁注入
* `src_kind` 取 `HOST`、`DISPATCHER`、`PHASE1_LANE`，三类注入源产生的 uid 集合互不重叠
* `src_kind` 是 `PHASE1_LANE` 的 uid 不写 `USERTARGET`：它进哪个核由 `HOSTBIND` 与 `HOSTGROUP` 在运行期定，编译期定不下来

### 6.6 `USERTARGET`

```
USERTARGET <uid> <host_name> <target_core> <tag>
```

* 该 uid 要从哪个 Host 发往哪个 Core
* `tag` 是该 Host 上的命中次数之和
* 一个 uid 可以有多条，对应 MoE 分发到多个 Host 与目标的情形；非 MoE 的 uid 恰好一条
* 注入侧照这张表发包，不复现四种 routing policy

### 6.7 `HITMAP`

```
HITMAP <uid> [group_id...]
```

该 uid 本次激活的 EPGroup id，可能为空。它随包传播，接收核用它做入边过滤。

### 6.8 `BITMAP`

```
BITMAP <uid> <group_id> <count>
```

该 uid 在该 group 上命中的专家数，稀疏。接收核用它作为计算时间的倍率，倍率为 0 时按 1 算。

### 6.9 `OUTFRAG`

```
OUTFRAG <uid> <expected_fragments>
```

* 该 uid 要在全部 Out 上一共收满几个分片才算完成，不是每个 Out 各收这么多：一个 uid 的结果可以落在好几个 Out 上
* 取值按注入源分两种：
  * **注入源开跑的 uid**：恒为 `1`。结果可以落在几个汇聚点上，但注入源不数它们，收到第一个就算这个 user 完成
  * **MoE 分发器开跑的 uid**：timeline 里有 `REDUCTION` 时取 Out 的个数，否则取本次命中的 EPGroup 个数

### 6.10 `DHITMAP`

```
DHITMAP <uid> <layer_id> <core_id> [group_id...]
```

* VectorCore 在 `router_softmax_topk` 完成那一刻生成的 HitMap，按 uid、层号与核 id 索引
* 内容预先算好，可见时刻仍由模型控制：它要等那次计算结束后才对下游可见

***

## 7. 每 core 的硬件配置

本章的记录只属 `cycle` 档，给出 boot 期经 ctrl_noc 写进各 core 的全部配置。每条记录的 `core_id` 是全局编号，都必须有对应的 `CORE` 记录：坏核只构造 Router，不领配置，因此不写本章任何记录。

字段一律按名字给值，不按寄存器位打包。位域怎么摆、写哪个地址，由解析侧按寄存器映射决定。

### 7.1 方向的位序

出方向的位序照 DATA_NOC HAS 的 `Flow dir`：

| bit | 方向 |
| --- | --- |
| 0 | 上下（mid） |
| 1 | 左 |
| 2 | 右 |
| 3 | reduce1 |
| 4 | reduce2 |

**进本 core 不占方向位**，它由 `path_core_mask_enable` 与 `path_core_bypass` 单独判定：

```
进核 = path_core_mask_enable ? path_core_mask 的第 path_core_mask_idx 位 : !path_core_bypass
```

只涉及三个 R2R 方向的字段用低三位。

### 7.2 Router 的三张表

```
RTAB <core_id> <path_id> <op_type> <flow_dir> <cur_vc> <nxt_vc> <path_core_mask_enable> <path_core_mask_idx> <path_core_bypass> <need_buffer> <stream_table_enable> <cur_credit_type> <cur_credit_require> <nxt_credit_type> <nxt_credit_require> <reduce_data_type> <reduce_outdata_type> <reduce_in_mask> <operation> <stall_way>
SKIPMASK <core_id> <mask>
CREDITBYPASS <core_id> <in_port> <out_mask>
```

`RTAB` 一条是 RouterTable 的一个表项，按 `path_id` 索引，每 core 最多 64 条。同一个 `path_id` 在不同 core 上表项不同，所以每 core 各写各的，解析侧不从拓扑反推。

字段照 DATA_NOC HAS 的 `Routing table field`，VC 与阻塞那几项照 Router MAS 的 `Table Entry`：

| 字段 | 位宽 | 出处 | 含义 |
| --- | --- | --- | --- |
| `path_id` | 6 | 两者 | 表项下标，0～63 |
| `op_type` | 2 | HAS | 0 kernel / weight 搬运、1 transfer、2 reduce、3 reduce_twice。0 那一档的包进 core 时跳过 TS 直接唤醒 DTE |
| `flow_dir` | 5 | HAS | 出方向掩码，按上面的位序。单个有效位是单播，多个有效位是多播 |
| `cur_vc` | 2 | MAS | 包进入本 Router 时用的 VC，给上一级无法指定 VC 的入口用 |
| `nxt_vc` | 10 | MAS | 五个出方向各 2 bit，第 i 个方向占 bit[2i+1:2i]。相互依赖的流不排进同一个 VC |
| `path_core_mask_enable` | 1 | HAS | 0 时按 `path_core_bypass` 判进核，1 时按 MSG 里 `path_core_mask` 的第 `path_core_mask_idx` 位 |
| `path_core_mask_idx` | 4 | HAS | 看 `path_core_mask` 的哪一位。位到 core 的对应由每个 core 自己指定，不是固定编码 |
| `path_core_bypass` | 1 | HAS | **0 进 core，1 bypass** |
| `need_buffer` | 1 | HAS | 这条 path 允许进 core 缓存，后续由 DTE 重发 |
| `stream_table_enable` | 1 | HAS | 这个包要不要查对应输出端的 stream credit table |
| `cur_credit_type` | 1 | HAS | 进核占用的 credit 池：0 broadcast 走 vc0，1 P2P 走 vc1 |
| `cur_credit_require` | 6 | Top 模拟器详设 | 进核占用额度，语义是上游已拨给本核的额度，不是再向下游申请 |
| `nxt_credit_type` | 3 | HAS | 三个 R2R 方向各一位，取值同 `cur_credit_type` |
| `nxt_credit_require` | 18 | Top 模拟器详设 | 三个 R2R 方向各 6 bit，第 i 个方向占 bit[6i+5:6i]。某方向为 0 表示该方向不查 credit，只受链路反压 |
| `reduce_data_type` | 3 | HAS | reduce 计算的输入精度，0 BF16、1 FP32 |
| `reduce_outdata_type` | 1 | HAS | reduce 输出精度，0 BF16、1 FP32。中间累加固定 FP32 |
| `reduce_in_mask` | 3 | 见下 | 这条 path 在本级会有哪几个相邻方向送来分量 |
| `operation` | 2 | MAS | 本级在这条 path 上的角色：0 普通转发、1 Reduce0、2 Reduce1、3 Reduce2 |
| `stall_way` | 1 | MAS | 0 留在当前 VC 等，1 转 Core Mem 暂存由 DTE 重发 |

`flow_dir` 与 `reduce_in_mask` 一个管出一个管进：前者是这条 path 从本级往哪几个方向发，后者是这条 path 在本级要等哪几个相邻方向的分量。`reduce_in_mask` 的取法是纯拓扑推导：在这条 path 的图上，把本核作为下一跳、且操作是 reduce 的那些相邻核，它们所在的方向置位。出分量的源核填 0，坏核与纯透传的中继核也填 0。

**待确认**：`reduce_in_mask` 与 `operation` 的三档 reduce 取值，两张权威表都没有。前者是逐级归约判断收齐所必需的，后者的 Reduce0 / Reduce1 / Reduce2 含义原始文档未定义，本格式按源分量 / 中继累加 / 最终汇聚给。

`SKIPMASK` 每个好核一条，`mask` 是本 chip 的坏核位图，与该 chip 的 `HARVEST` 取值相同。它与 RouterTable 分开配。

`CREDITBYPASS` 每个业务 credit 输入端口一条：`in_port` 取 `MID`、`LEFT`、`RIGHT` 之一，`out_mask` 用五位的位序，给出这个口收到的 credit 静态转发到哪些方向。

### 7.3 TS 的七张表

```
CFGMISC     <core_id> <stream_num> <b_core_dir> <trigger_task_chain_en>
TASKCHAIN   <core_id> <task_id> <send_unit> <recv_unit> <task_pc> <self_start> <wait_wake> <b_reissue> <p2p_reissue> <reduce> <credit_en> <exe_mask> <path_id> <end>
TASKSW      <core_id> <task_id> <exe_dest> <reduce_num> <dsa_en>
PATHTASK    <core_id> <path_id> <task_id>
PATHFLOW    <core_id> <path_id> <flowctl_en> <window_n>
DATAINTASK  <core_id> <task_pc> <weights_mode>
CREDITINIT  <core_id> <path_id> <stream_id> <init>
```

`CFGMISC` 每核一条：`stream_num` 取 1～16，`b_core_dir` 是 `B_CORE_DIRECTION` 的七位方向掩码，`trigger_task_chain_en` 取 0 或 1。这条记录不重复给 `core_type`，它在 `CORE` 记录里。

`TASKCHAIN` 一条是任务链的一项，每核最多 64 条，`task_id` 从 0 开始连续。`TASK_VALID` 不写进文件：硬件在软件写一项时自动置位。

| 字段 | 取值 | 含义 |
| --- | --- | --- |
| `send_unit` | `DTE`、`MU`、`VU` | 派给哪个单元 |
| `recv_unit` | `RV`、`DSA`、`RMEM` | `RV` 只调 RV core，`DSA` 调 DSA，`RMEM` 是 DTE DSA 加 Router 的 Rmem |
| `task_pc` | 整数 | kernel 入口地址，在该角色的 `TASKPC` 表里有对应项 |
| `self_start` | 0 / 1 | 复位后直接自启动，只有 B core 与 R core 用 |
| `wait_wake` | 0 / 1 | 建表时不就绪，等唤醒 |
| `b_reissue` | 0 / 1 | 广播重发任务 |
| `p2p_reissue` | 0 / 1 | P2P 重发任务 |
| `reduce` | 0 / 1 | 逐级 reduce 任务，完成要等 Router 的 reduce_done |
| `credit_en` | 0 / 1 | 出核前要验资 |
| `exe_mask` | 0 / 1 | 1 = 这个 task 不按用户区分，所有用户都做；0 = 只有 `compute` 为 1 的用户做 |
| `path_id` | 整数 | 这一项绑定的 path，不绑时写 `-1` |
| `end` | 0 / 1 | 任务链的最后一项，一条链上只能有一项 |

`TASKSW` 给硬件位域里没有的三个软件侧属性，与 `TASKCHAIN` 一一对应：`exe_dest` 是执行去向，`reduce_num` 是这一项要收几个分量，`dsa_en` 取 0 或 1。

`PATHTASK` 把 `path_id` 翻译成任务链上的第几项。一个 `path_id` 在一个 core 上最多一条，`task_id` 指回的那一项其 `path_id` 必须等于本条的 `path_id`。

`PATHFLOW` 是超前发送窗口，按 `path_id` 索引：`flowctl_en` 取 0 或 1，`window_n` 是超前几个用户。不写这条记录等于不开。这张表配在 TS 还是 RouterTable，原始设计未指明，本格式按配在 TS 给。

`DATAINTASK` 每核至多一条。`TASK_UNIT` 恒为 DTE，不写进文件。`weights_mode` 取 0 或 1。

`CREDITINIT` 每 `{path_id, stream_id}` 一条：广播任务的 `init` 等于目的 core 数量，P2P 任务是 1。

### 7.4 DSA 与 Core Mem 的五张表

```
DTELUT   <core_id> <task_id> <path_id> <size>
MUEP     <core_id> <slot> <expert_id>
VUSTATIC <core_id> <cfg_idx> <reg_off> <value>
CMEMPART <core_id> <stream_base> <stream_stride> <scale_base> <topk_base> <header_base> <reissue_base> <reissue_pkts_per_vc>
CMEMP2P  <core_id> <dir> <base> <entries>
```

`DTELUT` 是硬件包头的静态部分，按 `task_id` 索引，每核最多 64 条。两个字段就是软件文档里说的 `path_id_table` 与 `task_len_table`，不是两张独立的表。动态部分 `path_core_mask` 由 DTE core 在运行期配，不进文件。

`MUEP` 是本 core 所在 EP Group 内的专家清单：`slot` 是组内序号，`expert_id` 是全局专家号。只有 `core_type` 为 `NORMAL` 的核写这张表。

`VUSTATIC` 按寄存器偏移逐个给静态模板的取值：`cfg_idx` 取 0～7 选哪一组，`reg_off` 是组内字节偏移，该组的绝对地址是 `cfg_idx × 0x100 + 0x1000 + reg_off`。没写到的偏移取 0。

`CMEMPART` 每核一条，给 Core Mem 的分区。各分区互不重叠且都落在 Core Mem 的 1 MB 之内，用不上的分区基址写 `-1`。`reissue_pkts_per_vc` 是溢流重发暂存区每 VC 留几个整包，`stall_way` 选转存的 path 要求它不为 0。

`CMEMP2P` 是 P2P 阻塞缓冲，一个方向一条，最多三条：`dir` 取 `MID`、`LEFT`、`RIGHT` 之一。这一档是可选的，不开就不写。

***

## 8. 外部数据

本章的记录只属 `cycle` 档。四类大块数据各自是独立文件，本章只登记路径与形状。路径相对于这份 `.bachir` 文件所在的目录。

```
KERNEL  <core_type> <itcm_file> <dtcm_file>
TASKPC  <core_type> <task_id> <entry_addr>
WEIGHT  <core_id> <file> <mmem_addr>
INJECT  <file> <token_num> <payload_bytes>
EXPECT  <file> <token_num> <token_bytes>
```

`KERNEL` 每类 core 一条，`core_type` 取 `NORMAL`、`BROADCAST`、`REDUCTION` 之一。两个文件是该角色 RV32 ELF 的代码段与数据段镜像，boot 期分别装进 ITCM 与 DTCM。

`TASKPC` 把任务链每一项的 `task_pc` 指到该角色 kernel 的一个入口地址，每类 core 最多 64 条。

`WEIGHT` 每个好核一条，`file` 是该核的权重分片，`mmem_addr` 是它落 Matrix Mem 的起始地址。

`INJECT` 恰好一条。注入表是定长记录的二进制文件，每条 `{inject_cycle 4 B, gpu_id 1 B, token_id 2 B, payload}`，`payload_bytes` 给出 payload 长度。

`EXPECT` 恰好一条。期望输出是定长记录的二进制文件，每条 `token_bytes` 字节，按 token 顺序排。它由参考实现按与模型完全同一套累加顺序算出。

***

## 9. 解析规则

### 9.1 顺序无关

* 除首行版本行外，记录之间顺序无关
* 解析分两遍：第一遍读入全部记录，第二遍做校验

### 9.2 遇到不认识的就停

一律报错退出：

* 未定义的记录标签
* 记录标签不属于本文件声明的那一档
* 未定义的 `PARAM` 名字
* 未定义的 `TMETA` key
* 未定义的枚举取值
* 字段数不符
* 非整数字段解析失败
* 版本号不匹配

`META` 的 key 是唯一的例外。

### 9.3 必需记录

两档共用：

* `PROFILE` 恰好一条，在版本行的下一行
* `DIM` 恰好一条
* 至少一条 `CORE`
* 至少一条 `EXTNODE`

`task_level` 档另要：

* `TOTALUSERS` 恰好一条
* 至少一条 `USER`

`cycle` 档另要：

* `SPLIT` 与 `ENTRYEXIT` 各恰好一条
* 每个 chip 一条 `GRID`、一条 `HARVEST`
* 每个好核一条 `LOGICAL`、一条 `CFGMISC`、一条 `CMEMPART`、一条 `WEIGHT`
* `INJECT` 与 `EXPECT` 各恰好一条
* 每个出现过的 `core_type` 一条 `KERNEL`

### 9.4 引用完整性

第二遍校验这些。

任务表：

* 每条 `TASK` 的 `core_id` 有对应的 `CORE` 记录
* 每个 Core 的 `task_id` 从 0 开始连续，不重复
* 每条 `TMETA`、`CREDIT`、`SKIPSRC` 的 `(core_id, task_id)` 存在
* `CREDIT` 指向的行 `unit` 是 `CU`，`SKIPSRC` 指向的行 `unit` 是 `SKIP`
* 每条 `CREDITEDGE` 的两个 `core_id` 都有对应的 `CORE` 记录

拓扑与外部节点：

* 每条 `CHIPLINK` 的两个 core id 都落在核阵列内，两条方向相反的记录成对出现
* 每条 `TASK` 里作为 `down_cid` 的标识符有对应的 `EXTNODE`
* 每条 `USERTARGET`、`HOSTBIND` 的 `host_name` 有对应的 `EXTNODE`，且 `kind` 是 `HOST`
* `DTE` 行落在核阵列外的 `dst_row` 与 `dst_col` 有对应的 `EXTNODE` 或 `PCIESW`。其余行的坐标不检查，那些位置本来就不读

user 与随机决策：

* 每个 `USER` 的 uid 至少有一条 `USERTARGET`，`src_kind` 为 `PHASE1_LANE` 的除外
* `HITMAP`、`BITMAP`、`OUTFRAG`、`DHITMAP` 的 uid 有对应的 `USER`

PCIe：

* 每个 `PCIESW` 的 `sw_id` 只出现一次
* 每条 `PCIELINK` 至少一端是 `SW`
  * `SW` 端点指向已声明的 `PCIESW`
  * `CORE` 端点的 core id 落在核阵列内且写了端口
  * `HOST` 与 `OUT` 端点指向 `kind` 相符的 `EXTNODE`
* 每条 `PCIEROUTE` 与 `PCIEIROUTE` 的 `sw_id` 有对应的 `PCIESW`。有 `PCIELINK` 就至少有一条路由记录

Phase1 与以太网：

* 每条 `PHASE1LANE` 与 `PHASE1SINK` 的 name 有对应的 `EXTNODE`，且 `kind` 相符
* 每条 `PHASE1LANE` 的两个落点都有 `PHASE1SINK` 记录，两者不同，至少有一个。要发 MoE 请求就有 `PHASE1HIT`，每条通道都有 `PHASE1USER`
* `PHASE1HIT` 与 `PHASE1USER` 的 `lane_name` 有对应的 `PHASE1LANE`
* `role` 不是 `GENERIC` 的落点要有 `ETHSW`
* 有 `ETHSW` 就有五条 `ETHPORT`、至少一条 `ETHROUTE`；没有 `ETHSW` 就不能有 `ETHPORT`、`ETHROUTE` 或 `EXPGROUP`
* `ETHROUTE` 的起点是入口、终点是出口，一条入口只有一条出路
* `ETHMODE` 的四个开关满足它们之间的前置条件

不在这一层查的：端口有没有真接过线，以及一条路由的出口落在哪。这两项要等装配期拿到实际拓扑才查得了。

`cycle` 档另查这些。

每 core 的配置：

* 第 7 章与第 8 章每条记录的 `core_id` 都有对应的 `CORE` 记录。坏核不出现在这两章的任何记录里
* 每个 core 的 `TASKCHAIN` 的 `task_id` 从 0 开始连续，不重复；一条链上 `end` 恰好一项，`self_start` 至多一项，且只有 `core_type` 不是 `NORMAL` 的核才允许有
* 每条 `TASKSW` 与 `DTELUT` 的 `(core_id, task_id)` 有对应的 `TASKCHAIN`
* 每条 `TASKCHAIN` 里 `path_id` 不为 `-1` 的项，本 core 有对应的 `PATHTASK`，且该 `PATHTASK` 的 `task_id` 指回这一项
* 每条 `PATHFLOW` 与 `CREDITINIT` 的 `path_id`，本 core 有对应的 `RTAB`
* 每条 `RTAB` 里 `stall_way` 为 1 的表项，本 core 的 `TASKCHAIN` 有对应的 reissue 任务，且 `CMEMPART` 的 `reissue_pkts_per_vc` 不为 0
* 每条 `CMEMPART` 里基址不为 `-1` 的分区互不重叠，都落在 Core Mem 的 1 MB 之内
* 坏核的 `RTAB` 不置 core 位、`stream_need_mask` 全不置位、`stall_way` 只能是 0

拓扑与部署：

* `GRID` 覆盖每个 chip，`(gx, gy)` 不重复
* 每个 chip 的 `HARVEST` 坏核数不超过 2，`gx ∈ {0, 3}` 的 chip 不超过 1
* 每个 chip 的 `LOGICAL` 覆盖该 chip 的全部好核，逻辑编号从 0 开始连续、不重复；`core_type` 不是 `NORMAL` 的核占最大的那个逻辑编号，且只在 `gx ∈ {0, 3}` 的 chip 上出现

外部数据：

* 每条 `TASKPC` 的 `core_type` 有对应的 `KERNEL`
* 每条 `TASKCHAIN` 的 `task_pc`，在本核 `core_type` 那一份 `TASKPC` 表里有对应的 `entry_addr`
* `INJECT` 与 `EXPECT` 的 `token_num` 相等
* 五种外部文件都存在且长度与登记的形状相符

### 9.5 两档各自的记录集

记录标签出现在不属于它的那一档里，报错退出。

| 档 | 只属这一档的记录 |
| --- | --- |
| `task_level` | `TASK`、`TMETA`、`CREDIT`、`CREDITEDGE`、`SKIPSRC`、`PHASE1SINK`、`PHASE1LANE`、`PHASE1HIT`、`PHASE1USER`、`ETHSW`、`ETHPORT`、`ETHROUTE`、`EXPGROUP`、`ETHMODE`、`EPGROUP`、`HOSTBIND`、`TOTALUSERS`、`TOTALPACKETS`、`USER`、`USERTARGET`、`HITMAP`、`BITMAP`、`OUTFRAG`、`DHITMAP` |
| `cycle` | `GRID`、`HARVEST`、`LOGICAL`、`SPLIT`、`ENTRYEXIT`、`RTAB`、`SKIPMASK`、`CREDITBYPASS`、`CFGMISC`、`TASKCHAIN`、`TASKSW`、`PATHTASK`、`PATHFLOW`、`DATAINTASK`、`CREDITINIT`、`DTELUT`、`MUEP`、`VUSTATIC`、`CMEMPART`、`CMEMP2P`、`KERNEL`、`TASKPC`、`WEIGHT`、`INJECT`、`EXPECT` |

其余记录两档共用：`META`、`PARAM`、`MODE`、`DIM`、`CORE`、`EXTNODE`、`GATEWAY`、`DIRATTR`、`CHIPLINK`、`PCIESW`、`PCIELINK`、`PCIEROUTE`、`PCIEIROUTE`、`HOSTGROUP`。

***

## 10. 两档各一个最小完整例子

### 10.1 `task_level` 档

一个 chip、两个 Core、一个 Host、一个 Out、一个 user 的 dense 拓扑：

* Core 0 收 Host 的包，算一次 Matrix，验资后发给 Core 1
* Core 1 搬给 Out
* 两个核各自退休

```
BACHIR 6
PROFILE task_level

META map_path /home/me/maps/minimal.map
META map_sha256 59e128d6cb3dcfb5067774f0c1dd5e7ad0f5eb82912fc0ecaf203229964825a8
META compiler_sha256 6ec90383e5db3e3163ea6e2a448e3a2097b6673c5a9bcb7201e825e8f5763f0b
META generated_at 2026-08-17T11:09:07
META generator src/bach/tools/export_bachir.py

PARAM stream_count 1
PARAM num_users 1
MODE dte_execution_mode split
MODE dte_dsa_mode off

DIM 1 1 1 2 1 1

CORE 0 -1 NORMAL
CORE 1 -1 NORMAL

# Core 0：等 Host 的包，验资，发给 Core 1，算一次 Matrix，退休回 Host
TASK 0 0 SKIP USER_INIT 0 0 0 0 0 1024
TASK 0 1 CU DONTCARE 0 0 0 0 0 0
CREDIT 0 1 1
TASK 0 2 DTE USER_INIT 0 0 1 0 1 1024
TASK 0 3 MC DONTCARE 0 0 0 0 0 96
TASK 0 4 DTE RETIRE 0 -1 0 0 -1 0

# Core 1：等 Core 0 的包，搬给 Out，退休回 Core 0
TASK 1 0 SKIP USER_INIT 0 0 1 0 1 1024
TASK 1 1 DTE MOVE 0 0 out 0 2 1024
TASK 1 2 DTE RETIRE 0 0 0 0 0 0

EXTNODE host HOST 0 -1 0 1024 PCIE_WEST 0 0
EXTNODE out OUT 0 2 1 1024 PCIE_EAST 0 0

TOTALUSERS 1
TOTALPACKETS 1
USER 0 HOST host
USERTARGET 0 host 0 0
HITMAP 0
OUTFRAG 0 1
```

对着例子看四处：

* Core 0 那条 `RETIRE` 的 `up_cid` 是 `-1`，目标坐标 `0 -1` 是 Host 的坐标，因为这个核由 Host 直接驱动。Core 1 那条 `RETIRE` 的 `up_cid` 是 `0`，目标坐标就是 Core 0 的坐标
* Core 1 那条 `MOVE` 的 `down_cid` 是标识符 `out`，对应下面那条 `EXTNODE`；它的目标坐标 `0 2` 落在核阵列外
* 单个 chip 没有片间链路，所以没有 `CHIPLINK` 记录
* `HITMAP 0` 后面没有 group id，表示这个 dense user 不激活任何 EPGroup

### 10.2 `cycle` 档

同样一个 chip、两个 Core：core 0 是 B core，收 Host 的 token 后广播给 core 1；core 1 算一次 FFN 把结果发给 Out。一条 path，每核两项任务链。

```
BACHIR 6
PROFILE cycle

META generator src/bach/tools/export_bachir.py
META generated_at 2026-09-01T10:22:31

DIM 1 1 1 2 1 1

GRID 0 0 0 0
HARVEST 0 0
SPLIT 1 1 1 1 EPTP_NN 1 1
ENTRYEXIT 0 0 0 0

CORE 0 0 BROADCAST
CORE 1 0 NORMAL
LOGICAL 0 1
LOGICAL 1 0

# path 0：core 0 向右广播到 core 1，core 1 收进本核
RTAB 0 0 1 4 0 0 0 0 1 0 1 0 0 0 4096 0 0 0 0 0
RTAB 1 0 1 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0
SKIPMASK 0 0
SKIPMASK 1 0

# core 0：task 0 轮询有没有数据，task 1 广播出去
CFGMISC 0 1 0 0
TASKCHAIN 0 0 VU RV 4096 1 0 0 0 0 0 1 -1 0
TASKCHAIN 0 1 DTE DSA 4224 0 0 0 0 0 1 1 0 1
TASKSW 0 0 0 0 0
TASKSW 0 1 1 0 1
PATHTASK 0 0 1
DATAINTASK 0 4352 1
DTELUT 0 1 0 6368
CMEMPART 0 0 16384 -1 -1 262144 -1 0

# core 1：task 0 收数据，task 1 算完发走
CFGMISC 1 1 0 0
TASKCHAIN 1 0 DTE DSA 8192 0 1 0 0 0 0 1 0 0
TASKCHAIN 1 1 MU DSA 8320 0 0 0 0 0 0 1 -1 1
TASKSW 1 0 1 0 1
TASKSW 1 1 1 0 1
PATHTASK 1 0 0
CREDITINIT 0 0 0 1
DTELUT 1 0 0 6368
MUEP 1 0 0
CMEMPART 1 0 16384 262144 294912 327680 360448 2

KERNEL BROADCAST bcore.itcm bcore.dtcm
KERNEL NORMAL ncore.itcm ncore.dtcm
TASKPC BROADCAST 0 4096
TASKPC BROADCAST 1 4224
TASKPC NORMAL 0 8192
TASKPC NORMAL 1 8320
WEIGHT 0 w0.bin 0
WEIGHT 1 w1.bin 0

EXTNODE host HOST 0 -1 0 6368 PCIE_WEST 0 0
EXTNODE out OUT 0 2 1 12288 PCIE_EAST 0 0

INJECT inject.bin 1 6368
EXPECT expect.bin 1 12288
```

对着例子看五处：

* 两条 `RTAB` 是同一个 `path_id` 在两个核上的不同表项：core 0 的 `flow_dir` 是 4（右），`path_core_bypass` 为 1 表示数据不再进自己这个核；core 1 的 `flow_dir` 是 0（末端不再外发），`path_core_bypass` 为 0 表示进本核
* core 0 的 `nxt_credit_require` 是 4096：右方向占 bit[17:12]，值 1 表示向右发要一个坑。core 1 是末端，它向下游不要 credit，进核占 1 记在 `cur_credit_require`
* core 0 的 task 0 是 `self_start` 为 1 的轮询任务，只有 B core 与 R core 允许这么配
* 两个核的 `PATHTASK` 都指 path 0，但落到各自任务链的不同项：core 0 是 task 1，core 1 是 task 0
* core 0 的 `CMEMPART` 里 scale、topk 与溢流重发三段写 `-1`：B core 的数据落 Matrix Mem，Core Mem 上只留 stream 分片与包头

