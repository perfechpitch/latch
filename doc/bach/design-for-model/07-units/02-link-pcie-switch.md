# 2　链路与 PCIe Switch

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../07-latch-建模计划.md)）的建模方式之上

给实现链路与 PCIe Switch 的人：两种独立打拍的模块（链路、PCIe Switch）各自的

* 端口、存储器、逐拍行为、参数
* 它们承载的《系统与部署》《建模参数与性能模型》两章的机制

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《系统与部署》：“互联参数”“已知的板级问题”
* 《建模参数与性能模型》：延迟表
* 《软件栈》：“GPU → Bach 的两层 credit 反压”

***

## 1　定位与边界

**链路**是两个端口之间的一段带宽加延迟：

* 片内 R2R、PCIe ↔ Router、chip 间 C2C、GPU 的 ETH，都是同一个模块的不同参数实例
* 每条物理链路每个方向一个实例
* flit 进入时按带宽排队、按延迟定到达拍，到达拍才出现在对端端口

**PCIe Switch** 是 Node 里的交换节点：

* 双路 x16 各自独立计时，不保序
* 按端口输出队列转发
* 组播时复制到多个收端，按最慢收端反压

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1100 400" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs>
    <marker id="l0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker>
    <marker id="l0s" markerWidth="9" markerHeight="9" refX="1" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#475569"/></marker>
  </defs>
  <rect x="0" y="0" width="1100" height="400" fill="#ffffff"/>
  <text x="20" y="28" font-size="12" fill="#111827">链路与 PCIe Switch · 第 0 层</text>

  <polygon points="30,120 130,120 120,156 20,156" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="142" font-size="10.5" fill="#374151" text-anchor="middle">a.out</text>
  <text x="75" y="110" font-size="9" fill="#6b7280" text-anchor="middle">发送侧端口</text>
  <rect x="200" y="100" width="260" height="80" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="212" y="124" font-size="12" fill="#111827">Link（单向，每条物理链路每方向一个）</text>
  <text x="212" y="144" font-size="10" fill="#475569">in_flight · FIFO · {flit, arrive_cycle}</text>
  <text x="212" y="160" font-size="10" fill="#475569">last_busy_until · bw · latency</text>
  <line x1="132" y1="138" x2="198" y2="138" stroke="#475569" marker-end="url(#l0)"/>
  <polygon points="500,120 600,120 590,156 490,156" fill="#f8fafc" stroke="#374151"/>
  <text x="545" y="142" font-size="10.5" fill="#374151" text-anchor="middle">b.in</text>
  <text x="545" y="110" font-size="9" fill="#6b7280" text-anchor="middle">接收侧端口</text>
  <line x1="462" y1="138" x2="490" y2="138" stroke="#475569" marker-end="url(#l0)"/>
  <polyline points="545,158 545,200 75,200 75,158" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#l0)"/>
  <text x="310" y="196" font-size="9" fill="#6b7280" text-anchor="middle">release 反向走另一条 Link 实例（同参数）</text>

  <rect x="200" y="250" width="420" height="120" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="212" y="274" font-size="12" fill="#111827">PCIe Switch</text>
  <text x="212" y="294" font-size="10" fill="#475569">端口按名字开：GPU 侧 ×G · 出口侧 · chip 侧 ×C（每 chip 双路 x16）</text>
  <text x="212" y="310" font-size="10" fill="#475569">out_q[p] · 组播复制表 · 按最慢收端反压</text>
  <text x="212" y="326" font-size="10" fill="#475569">双路 x16 各自独立计时，不保序</text>
  <text x="212" y="354" font-size="9.5" fill="#9ca3af">与外侧的每个口各接一对 Link 实例</text>
  <polygon points="680,270 780,270 770,306 670,306" fill="#f8fafc" stroke="#374151"/>
  <text x="725" y="292" font-size="10.5" fill="#374151" text-anchor="middle">port[p]</text>
  <line x1="622" y1="288" x2="670" y2="288" stroke="#475569" marker-start="url(#l0s)" marker-end="url(#l0)"/>
  <text x="800" y="292" font-size="9" fill="#6b7280">↔ Link ↔ GPU 桩 / 出口桩 / chip 边界 Router</text>
</svg>
```

***

## 2　接口

```
port link.in (slave, credit/release, clk)         // 发送侧端口的镜像：Router link[d].out、桩的 tx、Switch 的 port[p].out
  in  flit_valid · flit_vc[1:0] · flit_head · flit_tail · flit_bytes[8:0] · flit_msg
  in  stream_release_valid · stream_release_user[15:0]
  in  reduce_release_valid · reduce_release_user[15:0]
  in  vc_release_valid · vc_release_vc[1:0]
port link.out (master, credit/release, clk)       // 接收侧端口的镜像，同字段；到达拍才拉 valid
port switch.port[p] (双向, credit/release, clk)   // PCIe Switch 每端口一对 in / out，字段同 link
```

***

## 3　存储器

```
mem in_flight        FIFO      深 LINK_Q_DEPTH × {flit 或 release, arrive_cycle[31:0]}  1W1R  不会满：上游按 credit 发  复位空   // 在途队列
mem last_busy_until  FF        32 b                                         1RW    每笔 = max(now, last) + ceil(size / bw)   复位 0
mem sw_out_q[p]      FIFO      深 SW_Q_DEPTH × flit                         1W1R   满 → 上游 credit 不归还（反压）            复位空
mem sw_mcast_tbl     FF 阵列   N × {dst_mask[C:0]}                          1R1W   配置                                        复位 0     // 组播复制表
mem sw_lane_busy[2]  FF        每路 x16 一个 last_busy_until                 1RW    双路独立计时                                复位 0
```

***

## 4　流水线总览

* **链路**：只有一级。K1 入队定到达拍 → 到达拍出队到 `link.out`
* **PCIe Switch**：两级。W1 收包查目的端口（组播复制到多个 `sw_out_q`）→ W2 每端口每拍出 1 flit 到对应 Link

两者都不另画第 1 层图。

***

## 5　逐级行为

### K1 · 链路（Link）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `link.in`、`last_busy_until`、`in_flight` | 1. `flit_valid` 或 release → `arrive = max(now, last_busy_until) + ceil(size / bw) + latency`；`size ≤ 0` 算一拍<br>2. `last_busy_until = arrive − latency`；`in_flight.push({item, arrive})`<br>3. `in_flight.front.arrive ≤ now` → 出队到 `link.out`（每拍最多 1 项）；端口固有的一拍延迟被吸收，时序 owner 唯一是本模块 | `in_flight`、`last_busy_until`、`link.out` | D变长（`ceil(size / bw)` + latency） |

### W1 · 收包与复制（PCIe Switch）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `switch.port[p].in`、`sw_mcast_tbl`、`sw_out_q[*]` 余量 | 1. 头 flit 按包头 `(gpu_id, dst)` 查目的端口集合；组播开关关时集合只有一个端口<br>2. 全部目的 `sw_out_q` 都有空才收（全有全无，按最慢收端反压）；否则本拍不收<br>3. 收下的 flit 复制进每个目的 `sw_out_q` | `sw_out_q[*]` | D1 |

### W2 · 出口（PCIe Switch）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `sw_out_q[p]`、`sw_lane_busy` | 1. 每端口每拍出 1 flit 到 `switch.port[p].out`（接对应 Link）<br>2. chip 侧双路 x16：按 flit 轮流走两路，各自独立计时，不保序 | `switch.port[p].out` | D1 |

***

## 6　参数汇总

```
R2R          256 B/T，40T                       // 片内 Router ↔ Router
PCIE_ROUTER  128 B/T；左右 10T + 25T，上下 10T + 50T
C2C          Router ↔ Router 400T；PCIe C2C 64 GB/s、300 ns（PCIe Switch → Chip、Chip → Chip）
ETH_IN       50 GB/s 每口，3 μs → 3000T
PCIE_IN      x16 54.4 GB/s；PCIE_OUT x32 108.8 GB/s
LINK_Q_DEPTH 按 latency / 每拍 1 flit 上限取（40T → 40；400T → 400）
SW_Q_DEPTH   32 flit                            // 待定
MULTICAST    开关，默认关（第 6 章“先假设 PCIe 没有组播能力”）
单位换算     1 T = 1 ns；拍数 = ceil(size / bw)，size ≤ 0 算一拍
```

***

## 7　机制覆盖

| 机制 | 落点 | 用例 |
| - | - | - |
| 链路占用与到达时刻：`arrive_cycle = max(now, last_busy_until) + ceil(size / bw) + latency` | K1 第 1 条 | `link_arrive` |
| 片内 R2R 256 B/T、40T | 参数 | `link_r2r` |
| PCIe ↔ Router 128 B/T，左右 10T + 25T、上下 10T + 50T | 参数 | `link_pcie_router` |
| chip 间 Router 到 Router 400T；PCIe C2C 64 GB/s、300 ns | 参数 | `link_c2c` |
| 双路 x16 各自独立计时，不保序 | W2 第 2 条，两条 Link 实例 | `pcie_two_lanes_unordered` |
| 组播：一份数据复制到多个收端 Matrix Mem，反压按最慢收端 | W1 第 2、3 条 | `pcie_multicast` |
| 输出 x32 108.8 GB/s | 参数 | `link_out` |

***

## 8　取舍

* **四种链路为什么用同一个模块**
  * 它们对模型的差别只有带宽与延迟
  * chip 间 C2C 按《系统与部署》的说法“当作一种长延迟的 R2R”
* **到达拍为什么算在发送侧而不是接收侧**
  * 这样链路的占用（`last_busy_until`）只有一个 owner
