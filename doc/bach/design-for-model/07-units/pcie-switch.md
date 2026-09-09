# PCIe Switch

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU → **PCIe Switch**（每 tray 4 个，接两层 chip 的边缘口，并接片外桩）

给实现 PCIe Switch 的人：一个逐拍推进的模块的端口、存储器、逐拍行为、参数，以及它承载的机制。

**对应设计**：

* 《系统与部署》：“tray 组成：4 层 × 4 chip”“已知的板级问题”
* 《软件栈》：“GPU → Bach 的两层 credit 反压”

***

## 1　定位与边界

PCIe Switch 是 chip 阵列与片外之间的交换节点：

* 每层 chip 的两端各接一个 Switch，一个 Switch 接两层
* 双路 x16 各自独立计时，不保序
* 按端口输出队列转发
* 组播时复制到多个收端，按最慢收端反压

同层 chip 之间、同列 chip 之间是直连，不经 Switch。

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1100 320" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs>
    <marker id="w0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker>
    <marker id="w0s" markerWidth="9" markerHeight="9" refX="1" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#475569"/></marker>
  </defs>
  <rect x="0" y="0" width="1100" height="320" fill="#ffffff"/>
  <text x="20" y="28" font-size="12" fill="#111827">PCIe Switch · 第 0 层</text>

  <rect x="330" y="80" width="420" height="130" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="342" y="104" font-size="12" fill="#111827">PCIe Switch</text>
  <text x="342" y="126" font-size="10" fill="#475569">out_q[p] 每端口一个输出队列</text>
  <text x="342" y="142" font-size="10" fill="#475569">sw_mcast_tbl 组播复制表 · 按最慢收端反压</text>
  <text x="342" y="158" font-size="10" fill="#475569">sw_lane_busy[2]：双路 x16 各自独立计时，不保序</text>
  <text x="342" y="186" font-size="9.5" fill="#9ca3af">与每个外侧端口各接一对 Link 实例</text>

  <polygon points="60,110 190,110 180,146 50,146" fill="#f8fafc" stroke="#374151"/>
  <text x="120" y="132" font-size="10.5" fill="#374151" text-anchor="middle">port[chip]</text>
  <text x="120" y="170" font-size="9" fill="#6b7280" text-anchor="middle">两层 chip 的边缘口</text>
  <path d="M186 128 L329 128" stroke="#475569" marker-start="url(#w0s)" marker-end="url(#w0)" fill="none"/>

  <polygon points="890,110 1020,110 1010,146 880,146" fill="#f8fafc" stroke="#374151"/>
  <text x="950" y="132" font-size="10.5" fill="#374151" text-anchor="middle">port[stub]</text>
  <text x="950" y="170" font-size="9" fill="#6b7280" text-anchor="middle">片外桩</text>
  <path d="M751 128 L884 128" stroke="#475569" marker-start="url(#w0s)" marker-end="url(#w0)" fill="none"/>

  <text x="20" y="260" font-size="10.5" fill="#374151">同层 chip 左右直连、同列 chip 上下直连，都不经 Switch；Switch 只在阵列边缘与片外之间。</text>
  <text x="20" y="282" font-size="10.5" fill="#374151">当前部署选定 LPU 广播，token 只从一个入口进阵列，组播开关默认关。</text>
</svg>
```

***

## 2　接口

```
port port[p] (双向, credit/release, clk)          // 每端口一对 in / out，字段同链路；外侧各接一对 Link 实例
  in  flit_valid · flit_vc[1:0] · flit_head · flit_tail · flit_bytes[8:0] · flit_msg
  in  stream_release_valid · stream_release_user[15:0]
  in  reduce_release_valid · reduce_release_user[15:0]
  in  vc_release_valid · vc_release_vc[1:0]
  out 同字段
port cfg (slave, ctrl_noc 写事务, clk)            // 组播复制表与组播开关的配置口
```

***

## 3　存储器

```
mem sw_out_q[p]      FIFO      深 SW_Q_DEPTH × flit      1W1R   满 → 上游 credit 不归还（反压）   复位空
mem sw_mcast_tbl     FF 阵列   N × {dst_mask[C:0]}       1R1W   配置                              复位 0    // 组播复制表
mem sw_lane_busy[2]  FF        每路 x16 一个 last_busy_until  1RW   双路独立计时                  复位 0
```

***

## 4　流水线总览

两级：W1 收包查目的端口（组播复制到多个 `sw_out_q`），W2 每端口每拍出 1 flit 到对应 Link。不另画第 1 层图。

***

## 5　逐级行为

### W1 · 收包与复制

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `port[p].in`、`sw_mcast_tbl`、`sw_out_q[*]` 余量 | 1. 头 flit 按包头 `(gpu_id, dst)` 查目的端口集合；组播开关关时集合只有一个端口<br>2. 全部目的 `sw_out_q` 都有空才收（全有全无，按最慢收端反压）；否则本拍不收<br>3. 收下的 flit 复制进每个目的 `sw_out_q` | `sw_out_q[*]` | D1 |

### W2 · 出口

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `sw_out_q[p]`、`sw_lane_busy` | 1. 每端口每拍出 1 flit 到 `port[p].out`（接对应 Link）<br>2. chip 侧双路 x16：按 flit 轮流走两路，各自独立计时，不保序 | `port[p].out` | D1 |

***

## 6　参数汇总

```
SW_PER_TRAY  4 个，左右各 2，每个接两层 chip 的边缘口
LANES        双路 x16，不支持 x32；C2C 是否支持 x32 待定
SW_Q_DEPTH   32 flit                            // 待定
MULTICAST    开关，默认关（《软件栈》“先假设 PCIe 没有组播能力”）
```

***

## 7　机制覆盖

| 机制 | 落点 | 用例 |
| - | - | - |
| 双路 x16 各自独立计时，不保序 | W2 第 2 条，两条 Link 实例 | `pcie_two_lanes_unordered` |
| 组播：一份数据复制到多个收端 Matrix Mem，反压按最慢收端 | W1 第 2、3 条 | `pcie_multicast` |
| 只支持双路 x16，不支持 x32 | 参数 | `pcie_lane_width` |
| 同层与同列 chip 直连不经 Switch | LPU 的接线 | `chip_direct_link` |

***

## 8　取舍

* **组播开关为什么默认关**
  * 当前选定 LPU 广播：CPU 只把 token 送进入口 chip，chip 之间自己传播
  * Smart NIC 能否 bypass CPU 做乱序多播没有结论，乱序多播下 reduction buffer 的资源回收无法支持
* **CPU 与 SmartNIC 为什么不在这里**
  * 它们在阵列之外，行为并进片外桩
