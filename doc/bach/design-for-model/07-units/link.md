# 链路

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：各级共用。chip 内 core 之间的 R2R、chip 之间的 C2C、chip 到 PCIe Switch、片外桩到 PCIe Switch，都是本模块的参数实例

给实现链路的人：一个独立打拍的模块的端口、存储器、逐拍行为、参数，以及它承载的机制。

**对应设计**：

* 《系统与部署》：“互联参数”
* 《建模参数与性能模型》：延迟表

***

## 1　定位与边界

链路是两个端口之间的一段带宽加延迟：

* 每条物理链路每个方向一个实例
* flit 进入时按带宽排队、按延迟定到达拍，到达拍才出现在对端端口
* 数据与三种 release 各走各的实例，参数相同

模型里全部链路都是这一个模块，差别只在带宽、延迟两个参数：

| 实例 | 接哪两端 |
| - | - |
| R2R | 同一 chip 内相邻 core 的 Router |
| C2C | 相邻 chip 边界 core 的 Router；同层左右相邻、同列上下相邻、跨 tray 纵向 |
| PCIe ↔ Router | chip 边界 core 的 Router 与 PCIe Switch |
| ETH | 片外桩与 PCIe Switch |

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1100 260" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs>
    <marker id="l0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker>
  </defs>
  <rect x="0" y="0" width="1100" height="260" fill="#ffffff"/>
  <text x="20" y="28" font-size="12" fill="#111827">链路 · 第 0 层</text>

  <polygon points="30,90 130,90 120,126 20,126" fill="#f8fafc" stroke="#374151"/>
  <text x="75" y="112" font-size="10.5" fill="#374151" text-anchor="middle">link.in</text>
  <text x="75" y="80" font-size="9" fill="#6b7280" text-anchor="middle">发送侧端口</text>
  <rect x="230" y="70" width="300" height="80" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="242" y="94" font-size="12" fill="#111827">Link（单向）</text>
  <text x="242" y="114" font-size="10" fill="#475569">in_flight · FIFO · {item, arrive_cycle}</text>
  <text x="242" y="130" font-size="10" fill="#475569">last_busy_until · bw · latency</text>
  <line x1="132" y1="108" x2="228" y2="108" stroke="#475569" marker-end="url(#l0)"/>
  <polygon points="600,90 700,90 690,126 590,126" fill="#f8fafc" stroke="#374151"/>
  <text x="645" y="112" font-size="10.5" fill="#374151" text-anchor="middle">link.out</text>
  <text x="645" y="80" font-size="9" fill="#6b7280" text-anchor="middle">接收侧端口</text>
  <line x1="532" y1="108" x2="588" y2="108" stroke="#475569" marker-end="url(#l0)"/>

  <text x="760" y="98" font-size="10" fill="#374151">一条物理链路 = 两个实例，方向相反</text>
  <text x="760" y="118" font-size="10" fill="#374151">三种 release 各走自己的实例</text>

  <text x="20" y="200" font-size="10.5" fill="#374151">到达拍在发送侧算好写进 item，链路只负责到点交付；端口固有的一拍延迟被吸收，时序 owner 唯一是本模块。</text>
  <text x="20" y="222" font-size="10.5" fill="#374151">同一实例上到达拍单调递增（发送侧占用时刻单调），队首不会挡住更早到达的项，这一条加断言。</text>
</svg>
```

***

## 2　接口

```
port link.in (slave, credit/release, clk)         // 发送侧端口的镜像：Router link[d].out、片外桩的 tx、Switch 的 port[p].out
  in  flit_valid · flit_vc[1:0] · flit_head · flit_tail · flit_bytes[8:0] · flit_msg
  in  stream_release_valid · stream_release_user[15:0]
  in  reduce_release_valid · reduce_release_user[15:0]
  in  vc_release_valid · vc_release_vc[1:0]
port link.out (master, credit/release, clk)       // 接收侧端口的镜像，同字段；到达拍才拉 valid
```

***

## 3　存储器

```
mem in_flight        FIFO   深 LINK_Q_DEPTH × {flit 或 release, arrive_cycle[31:0]}  1W1R  不会满：上游按 credit 发  复位空   // 在途队列
mem last_busy_until  FF     32 b                                    1RW   每笔 = max(now, last) + ceil(size / bw)   复位 0
```

***

## 4　流水线总览

只有一级：K1 入队定到达拍，到达拍出队到 `link.out`。不另画第 1 层图。

***

## 5　逐级行为

### K1 · 入队与到点交付

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `link.in`、`last_busy_until`、`in_flight` | 1. `flit_valid` 或 release → `arrive = max(now, last_busy_until) + ceil(size / bw) + latency`；`size ≤ 0` 算一拍<br>2. `last_busy_until = arrive − latency`；`in_flight.push({item, arrive})`<br>3. `in_flight.front.arrive ≤ now` → 出队到 `link.out`（每拍最多 1 项） | `in_flight`、`last_busy_until`、`link.out` | D变长（`ceil(size / bw)` + latency） |

***

## 6　参数汇总

```
R2R          256 B/T，40T                       // chip 内 Router ↔ Router
C2C          Router ↔ Router 400T；PCIe C2C 64 GB/s、300 ns（Chip ↔ Chip、PCIe Switch → Chip）
C2C_VERT     tray 间纵向链路 120 GB/s（试算值，待定）
PCIE_ROUTER  128 B/T；左右 10T + 25T，上下 10T + 50T
ETH_IN       50 GB/s 每口，3 μs → 3000T
PCIE_IN      x16 54.4 GB/s；PCIE_OUT x32 108.8 GB/s
LINK_Q_DEPTH 按 latency / 每拍 1 flit 上限取（40T → 40；400T → 400）
单位换算     1 T = 1 ns；拍数 = ceil(size / bw)，size ≤ 0 算一拍
```

***

## 7　机制覆盖

| 机制 | 落点 | 用例 |
| - | - | - |
| 链路占用与到达时刻：`arrive_cycle = max(now, last_busy_until) + ceil(size / bw) + latency` | K1 第 1 条 | `link_arrive` |
| 到达拍单调递增，队首不挡更早的项 | K1 第 3 条的断言 | `link_monotonic` |
| chip 内 R2R 256 B/T、40T | 参数 | `link_r2r` |
| chip 间 Router 到 Router 400T；PCIe C2C 64 GB/s、300 ns | 参数 | `link_c2c` |
| tray 间纵向链路 120 GB/s | 参数 | `link_tray_vertical` |
| PCIe ↔ Router 128 B/T，左右 10T + 25T、上下 10T + 50T | 参数 | `link_pcie_router` |
| 输出 x32 108.8 GB/s | 参数 | `link_out` |

***

## 8　取舍

* **四种链路为什么用同一个模块**
  * 它们对模型的差别只有带宽与延迟
  * chip 间 C2C 按《系统与部署》的说法“当作一种长延迟的 R2R”
* **到达拍为什么算在发送侧而不是接收侧**
  * 这样链路的占用（`last_busy_until`）只有一个 owner
