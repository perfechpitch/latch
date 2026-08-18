# 测试用的 Map

这里放的是 latch 侧自己造的 Map，用来覆盖 Bach 仓库里现有 Map 没有的拓扑。其余 fixture
的 Map 都在 Bach 仓库里，那些不往这边拷。

## `pcie_switch.json`

从 Bach 的 `bach_topology.json` 派生：一行四块 chip、每块 2×4 共三十二个核，一个注入源
一个汇聚点。改了三处，把注入源与汇聚点从直连网关核改成挂在一个 PCIe 交换节点上：

| 改动 | 内容 |
| --- | --- |
| `pcie_switches` | 一个交换节点 `SW0`，锚点坐标 `[-1, 0]`，不与核阵列重叠 |
| `pcie_links` | 四条：注入源、汇聚点、核 16、核 15 各接一个口 |
| `pcie_routes` | 四条，每个目的地各一条。交换网没有默认路由，缺一条就跑不起来 |
| `chip_rules` | chip 0 的 `LEFT` 与 chip 3 的 `RIGHT` 各填一个核号 |

最后一处是前提：核接交换节点的那个端口必须在 chip 规则里显式列出来。这两个方向都在阵列
最外侧，没有相邻 chip，所以不会多出片间链路。

## `phase1_eth.json`

从 Bach 的 `test_regre/nano-moe.map` 派生：2×2 块 chip、每块 2×4 共三十二个核，两个
注入源一个汇聚点，两个 EPGroup 各绑一个注入源。加了 Phase1 那一段：

| 改动 | 内容 |
| --- | --- |
| `phase1_lanes` | 一条通道 `L0`，坐标 `[-1, 0]`，接核 0 |
| `phase1_sinks` | 两个落点：`S_RES` 收残差接核 1，`S_MOE` 收 MoE 请求接核 2 |

通道选中的三个专家都落在同一个 EPGroup 上。这不是凑数：结果那条回程要求算完的结果
带着它是哪几个 group 算出来的，而那份 group 由最后归约的那个核补，它只留得住自己收
到的那一份。一次工作的 group 落在两个注入源上时，回来的结果就与当初发出去的请求对
不上，Bach 那边也是同一条边界。

三个节点各接一个核，端口都由几何推断成 `PCIE_NORTH`。那三个核都在阵列最上一行，
`TOP` 方向没有相邻 chip，所以这个口本来就空着。

## 各份 fixture 怎么导出来的

格式版本变了就要照这里全部重导一次，从仓库根目录跑。现在是版本 5：

```shell
B=/home/colin/develop/bach
E="python3 src/bach/tools/export_bachir.py"
F=test/bach/fixture

$E --map $B/bach_topology.json      --users 1 --streams 1 --out $F/bach_topology.bachir
$E --map $B/moe_bc_core.map         --users 2 --streams 2 --out $F/moe_bc_core.bachir
$E --map $B/test_regre/nano-moe.map --users 2 --streams 2 --out $F/nano-moe.bachir
$E --map $B/test_regre/neo-moe.map  --users 2 --streams 2 --out $F/neo-moe.bachir
$E --map $F/maps/pcie_switch.json   --users 1 --streams 1 --out $F/pcie_switch.bachir
$E --map $B/bach_topology.json      --users 1 --streams 1 --dsa five_route \
   --out $F/dsa_five_route.bachir
$E --map $F/maps/phase1_eth.json    --users 2 --streams 2 --phase full \
   --out $F/phase1_eth.bachir
$E --map $F/maps/phase1_eth.json    --users 2 --streams 2 --phase boundary \
   --out $F/phase1_boundary.bachir
```

`dsa_five_route` 与第一份是同一张 Map，只把执行通道那一段换成五路径仲裁，用来验证换掉
排队方式不改变跑出来的结果。

## 与 Bach 实跑的对表结果

`../expect/*.expect` 是 Bach 跑同一张 Map 得到的完成集合、丢包集合与每个 user 的端到端
延迟，`test/bach/sim/compare.cpp` 读它对表。换了 Map、换了参数或者改了模型行为，都要照
下面重跑一次。命令与上面那些一一对应，只是脚本换成 `run_bach.py`：

```shell
B=/home/colin/develop/bach
R="python3 src/bach/tools/run_bach.py --outdir /tmp/bach_runs"
E=test/bach/fixture/expect
F=test/bach/fixture

$R --map $B/bach_topology.json      --users 1 --streams 1 --out $E/bach_topology.expect
$R --map $B/moe_bc_core.map         --users 2 --streams 2 --out $E/moe_bc_core.expect
$R --map $B/test_regre/nano-moe.map --users 2 --streams 2 --out $E/nano-moe.expect
$R --map $B/test_regre/neo-moe.map  --users 2 --streams 2 --out $E/neo-moe.expect
$R --map $F/maps/pcie_switch.json   --users 1 --streams 1 --out $E/pcie_switch.expect
$R --map $B/bach_topology.json      --users 1 --streams 1 --dsa five_route \
   --out $E/dsa_five_route.expect
$R --map $F/maps/phase1_eth.json    --users 2 --streams 2 --phase full \
   --out $E/phase1_eth.expect
$R --map $F/maps/phase1_eth.json    --users 2 --streams 2 --phase boundary \
   --out $E/phase1_boundary.expect
```

`--outdir` 是 Bach 的报告、快照与诊断落脚的地方，指到仓库外面：这个脚本只读 Bach 仓库，
不往那边写任何东西。

最后两份是同一张 Map 的两种完成层级：`full` 一路走到 Phase3 汇合，完成权在汇合点；
`boundary` 只判 Phase1 边界，两路都投出去就算一次工作完成，Phase2 与汇合点都不接。
