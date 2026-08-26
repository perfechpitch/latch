# Bach 中间文件格式

> **文档模式：** 规格。只给格式规则，不含动机与取舍。
> **文档层级：** 详细实现。它只服务于 Bach 在 latch 上的重建这一件事。
> **目的：** 给写生成侧（Python）与解析侧（C++）的实现者，定下这份文件的编码、记录类型、字段含义与校验规则。两侧按同一份规则编解码，任务表才是两边共用的同一份。
> **后缀：** `.bachir`

***

## 1. 文件形态

* UTF-8 编码，LF 换行
* 一行一条记录：行首是记录标签，其后是字段，字段之间用一个或多个空格分隔
* 首行必须是版本行：

```
BACHIR 5
```

* 版本号是单个整数，格式的任何变化都让它加一。解析器只接受它认识的那个版本号，不做向前或向后兼容
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

***

## 4. 任务表

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

生成侧把 MoE 的分发结果与动态 HitMap 全部算完写进文件，解析侧按 uid 查表。随机数发生器不在解析侧复现。

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

## 7. 解析规则

### 7.1 顺序无关

* 除首行版本行外，记录之间顺序无关
* 解析分两遍：第一遍读入全部记录，第二遍做校验

### 7.2 遇到不认识的就停

一律报错退出：

* 未定义的记录标签
* 未定义的 `PARAM` 名字
* 未定义的 `TMETA` key
* 未定义的枚举取值
* 字段数不符
* 非整数字段解析失败
* 版本号不匹配

`META` 的 key 是唯一的例外。

### 7.3 必需记录

* `DIM` 恰好一条
* `TOTALUSERS` 恰好一条
* 至少一条 `CORE`
* 至少一条 `EXTNODE`
* 至少一条 `USER`

### 7.4 引用完整性

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

***

## 8. 一个最小完整例子

一个 chip、两个 Core、一个 Host、一个 Out、一个 user 的 dense 拓扑：

* Core 0 收 Host 的包，算一次 Matrix，验资后发给 Core 1
* Core 1 搬给 Out
* 两个核各自退休

```
BACHIR 5

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
