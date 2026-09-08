# 片外桩

**模式**：design（陈述当前设计，取舍收在“取舍”段落里）
**层**：详细实现，建立在《latch 建模计划》（[`07-latch-建模计划.md`](../07-latch-建模计划.md)）的建模方式之上
**在硬件里的位置**：LPU 之外：**片外桩**

给实现片外桩的人：入口桩与出口桩两个独立打拍的模块各自的端口、存储器、逐拍行为、参数，以及它们承载的机制。

LPU 之外的全部硬件（GPU、SmartNIC 里的 DPU、ETH 交换机、tray 上的 CPU 与 DDR）都收在这两个桩里，只模仿接口行为：

* 按注入表发 token
* 做两层 credit
* 收结果比对

不算 Attention，不算 FC0 输入的乱序重排，不做 CPU 侧的缓存管理。

章节与画法按《硬件电路设计描述规范》（`/home/colin/develop/forge/fuse/gmp/uarch/硬件电路说明.md`）。

**对应设计**：

* 《软件栈》：“GPU → Bach 的两层 credit 反压”“运行时约定”“输出包格式”
* 《系统与部署》：“Node 组成”“EP Reduction 死锁与 LPU Dispatch 派遣机制”

***

## 1　定位与边界

**入口桩**站在 LPU 的入口，一个 GPU 一个实例：

* 按注入表在给定拍，把一个 token 的 6368 B 级联包封成 MSG 发向 PCIe Switch
* 发前过两道闸门：GPU 本地 credit、Bach 全局 credit 池
* 收端回来的 retired 信息更新 credit
* EP6+TP8 下还做 LPU Dispatch 派遣

**出口桩**站在 LPU 的出口：

* 从 PCIe Switch 收结果包
* 按 `(gpu_id, token_id)` 与参考实现逐 bit 比对
* 记完成集合

两者各带一个路由器（自己那一侧的 RouterStation 与链路模型），包在这里进出链路。

片外那几样硬件在桩里的落点：

| 硬件 | 在桩里是什么 |
| - | - |
| GPU | 注入表的一个 `gpu_id`，加它那份本地 credit |
| SmartNIC 里的 DPU | 入口桩封包时写的自定义包头 `gpu_id(8) + token_id(16)` |
| ETH 交换机与 ETH 口 | 桩到 PCIe Switch 之间那条链路实例的带宽与延迟（50 GB/s、3 μs） |
| tray 上的 CPU 与 DDR | 注入表的发包顺序，即当前选定的 LPU 广播下“只把 token 送进入口 chip”这一条 |

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1100 420" font-family="PingFang SC, Noto Sans CJK SC, Microsoft YaHei, sans-serif">
  <defs>
    <marker id="a0" markerWidth="9" markerHeight="9" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#475569"/></marker>
    <marker id="a0s" markerWidth="9" markerHeight="9" refX="1" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 z" fill="#475569"/></marker>
  </defs>
  <rect x="0" y="0" width="1100" height="420" fill="#ffffff"/>
  <text x="20" y="28" font-size="12" fill="#111827">片外桩 · 第 0 层</text>

  <rect x="60" y="80" width="300" height="150" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="72" y="104" font-size="12" fill="#7c2d12">入口桩（每 GPU 一个实例）</text>
  <text x="72" y="124" font-size="10" fill="#92400e">inject_tbl · 注入拍 + 6368 B 级联包</text>
  <text x="72" y="140" font-size="10" fill="#92400e">两层 credit：buffer_used / pool_avail</text>
  <text x="72" y="156" font-size="10" fill="#92400e">DPU 包头 gpu_id(8) + token_id(16)</text>
  <text x="72" y="172" font-size="10" fill="#92400e">LPU Dispatch 派遣余量（EP6+TP8）</text>
  <text x="72" y="188" font-size="10" fill="#92400e">retired 汇总取 min</text>
  <text x="72" y="216" font-size="9.5" fill="#9ca3af">不算 Attention，按注入表的时刻发包</text>

  <rect x="60" y="270" width="300" height="100" fill="#fbf3df" stroke="#b45309" stroke-dasharray="4 3" rx="4"/>
  <text x="72" y="294" font-size="12" fill="#7c2d12">出口桩</text>
  <text x="72" y="314" font-size="10" fill="#92400e">收结果包 → 按 (gpu_id, token_id) 重组</text>
  <text x="72" y="330" font-size="10" fill="#92400e">与参考实现逐 bit 比对 · 完成集合</text>
  <text x="72" y="346" font-size="10" fill="#92400e">retired_token 回报入口桩</text>

  <rect x="440" y="120" width="180" height="160" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="452" y="144" font-size="12" fill="#111827">桩侧路由器</text>
  <text x="452" y="164" font-size="10" fill="#475569">本地口 + 一个 RouterStation</text>
  <text x="452" y="180" font-size="10" fill="#475569">链路模型：带宽、延迟</text>
  <text x="452" y="196" font-size="10" fill="#475569">回包落地重组</text>
  <path d="M362 150 L439.03 170.27" stroke="#475569" marker-end="url(#a0)" fill="none"/>
  <text x="380" y="146" font-size="9" fill="#6b7280">tx（MSG）</text>
  <path d="M438 240 L360.69 321.38" stroke="#475569" marker-end="url(#a0)" fill="none"/>
  <text x="380" y="300" font-size="9" fill="#6b7280">rx（结果包）</text>

  <polygon points="680,170 790,170 780,206 670,206" fill="#f8fafc" stroke="#374151"/>
  <text x="730" y="192" font-size="10.5" fill="#374151" text-anchor="middle">link（ETH / PCIe）</text>
  <path d="M621 188 L674 188" stroke="#475569" marker-start="url(#a0s)" marker-end="url(#a0)" fill="none"/>
  <rect x="840" y="150" width="200" height="80" fill="#f8fafc" stroke="#374151" rx="4"/>
  <text x="852" y="174" font-size="12" fill="#111827">PCIe Switch</text>
  <text x="852" y="194" font-size="10" fill="#475569">双路 x16 · 组播复制</text>
  <text x="852" y="210" font-size="10" fill="#475569">→ chip 阵列</text>
  <path d="M786 188 L839 188" stroke="#475569" marker-start="url(#a0s)" marker-end="url(#a0)" fill="none"/>

  <path d="M360 340 L700 340 L700 232 L730 232 L730 207" fill="none" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#a0)"/>
  <text x="420" y="336" font-size="9" fill="#6b7280">retired（反向数据通路）</text>
  <path d="M200 270 L200 231" stroke="#475569" stroke-dasharray="4 3" marker-end="url(#a0)" fill="none"/>
  <text x="206" y="256" font-size="9" fill="#6b7280">done 集合 / retired 汇总</text>

  <text x="20" y="400" font-size="10.5" fill="#374151">桩是模块（tick=true），坐标登记在片外节点表里，挂在 PCIe Switch 上；链路与 Switch 各有一份文档。</text>
</svg>
```

***

## 2　接口

```
port tx (master, credit/release, clk)             // 桩侧路由器的本地口 → 链路，每拍最多 1 flit
  out flit_valid · flit_vc[1:0] · flit_head · flit_tail · flit_bytes[8:0] · flit_msg
  in  vc_release_valid · vc_release_vc[1:0]
port rx (slave, credit/release, clk)              // 链路 → 桩侧路由器，结果包与 retired 信息
  in  flit_valid · flit_vc[1:0] · flit_head · flit_tail · flit_bytes[8:0] · flit_msg
  out vc_release_valid · vc_release_vc[1:0]
port retired (slave, 脉冲, clk)                   // 收端（出口桩 / 各 R core）上报的 retired_token
  in  valid · gpu_id[7:0] · retired_token[15:0] · src[7:0]
port dispatch (master, valid/ready, clk)          // EP6+TP8：LPU Dispatch 派遣到 R core 的 reduce-buffer 余量记账
  out req_valid · req_gpu[7:0] · req_token[15:0]
  in  req_ready                                     // = 所有 R core 余量 > 0
port done (master, 电平, clk)                     // 出口桩：完成集合与比对结果，验收读取
  out all_done · mismatch_cnt[31:0] · last_cycle[31:0]
```

***

## 3　存储器

```
mem inject_tbl      FF 阵列   N_token × {inject_cycle[31:0], gpu_id[7:0], token_id[15:0], dst[5:0], path_id[7:0], compute, payload 6368 B{act FP8 6144 B, scale FP32 192 B, expert Int16 16 B, weight BF16 16 B}}  1R  编译侧读入  复位由输入给
mem credit_local[G] FF        {buffer_depth_tokens[15:0], buffer_used[15:0], inflight_bach[15:0], grant_tokens[15:0]}  1RW  发 +1，retired −1  复位 0
mem pool            FF        {pool_total[15:0], pool_avail[15:0]}   1RW    Σ inflight_bach                复位 pool_total
mem seq_w           FF        W bit 序号，比较用模 2^W 差值           1RW    每发一 token +1                复位 0
mem hdr_cnt[G]      FF        计数（256 项自增）                       1RW    per-GPU 包头计数               复位 0
mem dispatch_slots[R] FF      每 R core 的 reduce-buffer 余量           1RW    派遣 −1，返回 +1               复位初值
mem retired_min[G]  FF        各收端上报的 retired_token 取 min         1RW    retired 到更新                 复位 0
mem tx_q            FIFO      深 TX_Q_DEPTH × flit                     1W1R   满 → 本拍不封包                复位空
mem rx_asm          FF 阵列   按 (gpu_id, token_id) 的重组缓冲          1RW    尾 flit 到 → 比对并释放         复位空
mem done_set        FF 阵列   N_token × {done, mismatch}               1RW    比对后写                        复位 0
```

另有两张只在读入时用到的表：

```
mem weight_shard    FF 阵列   48 × 10 × {字节流, 落 Matrix Mem 的地址}   1R   编译侧读入   复位由输入给   // 每 core 27 MiB，EP6+TP8 下另加共享专家 576 KiB
mem expect_out      FF 阵列   N_token × 12 KiB（[6144] @BF16）           1R   reference/ 生成  复位由输入给
```

`expect_out` 由 `reference/` 按与 `numeric/` 完全同一套累加顺序算出，否则逐 bit 比对没有意义。

注入的顺序有一条规矩：**weights 加载阶段先发最远路径的数据**。先发近的会让不进本核的 weights 卡在 Router 里，要等 DTE 把当前数据搬走才能继续收。

***

## 4　流水线总览

* **入口桩每拍**：G1 注入判定（两道闸门）→ G2 封包进 `tx_q` → 桩侧路由器按链路带宽与延迟送出
  * `retired` 到来时更新 credit
* **出口桩每拍**：X1 收 flit 重组 → X2 尾 flit 时比对、记完成、回报 retired

都是单级逐拍模块，不另画第 1 层图；级的四要素在“逐级行为”里。

***

## 5　逐级行为

### G1 · 注入判定（入口桩）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `inject_tbl` 游标、`credit_local[g]`、`pool`、`dispatch_slots`、`tx_q` 余量 | 1. `tbl[cur].inject_cycle ≤ now`<br>2. 第一层：`buffer_used[g] < buffer_depth_tokens[g]`<br>3. 第二层：`pool_avail = pool_total − Σ inflight_bach > 0`，grant 按 GPU 轮询公平发；两道闸门都开才流，1 credit = 1 token<br>4. EP6+TP8：`∀ r: dispatch_slots[r] > 0` 才派遣，派时各 −1（顺序调度）<br>5. 通过 → `buffer_used[g] += 1`，`inflight_bach[g] += 1`，`seq_w += 1`，进 G2；否则等 | `credit_local`、`pool`、`dispatch_slots`、`seq_w` | D1 |

### G2 · 封包（入口桩）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| G1 通过的 token、`hdr_cnt[g]`、`tx_q` | 1. `Encapsulate`：DPU 自定义包头 `gpu_id(8) + token_id(16)`，Payload ≤ 64 KB；MSG Header 按 `common/message.h`（加载 weights 时由 DPU 写 MSG Header）；`hdr_cnt[g] += 1`<br>2. 切成 `ceil(size / 256)` 个 flit 进 `tx_q`；首 flit 携带 Header<br>3. 桩侧路由器每拍从 `tx_q` 取 1 flit 走 `tx`，按链路模型算到达拍 | `tx_q`、`tx` | D1 |

### G3 · retired 记账（入口桩）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `retired`、`retired_min[g]`、`credit_local[g]`、`dispatch_slots` | 1. `retired_min[g] = min(各收端 retired_token)`（组播反压按最慢收端聚合）<br>2. `retired_min` 前进 k → `buffer_used[g] −= k`，`inflight_bach[g] −= k`；序号比较用模 2^W 差值<br>3. R core 返回 → `dispatch_slots[r] += 1` | `credit_local`、`pool`、`dispatch_slots` | D1 |

### X1 · 收包重组（出口桩）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| `rx`、`rx_asm` | 1. 头 flit：按 `(gpu_id, token_id)` 开一项重组缓冲<br>2. 每 flit 追加 payload 字节；`vc_release` 回一拍 | `rx_asm`、`rx.vc_release` | D1 |

### X2 · 比对与完成（出口桩）

| 入口 | 逻辑 | 出口 | Dx |
| - | - | - | - |
| X1 的尾 flit、`rx_asm`、参考实现输出、`done_set` | 1. 尾 flit → 与 `reference/` 的期望输出逐 bit 比对（同一 `numeric/` 累加顺序），`done_set[token] = {1, mismatch}`<br>2. `retired = {gpu_id, token_id, src = 出口}`<br>3. `all_done = ∀ token done`；`last_cycle = now` | `done_set`、`retired`、`done` | D1 |

***

## 6　参数汇总

```
TX_Q_DEPTH           32 flit           // 待定
SEQ_W                16 bit            // 序号位宽，回绕模 2^W
buffer_depth_tokens、pool_total          由输入给；GPU 数与每 GPU 的 batch 由输入给
BCORE_MM_SLOTS       ≥ 32 × 26 = 832（B core Matrix Mem 槽位，参数校验）
桩到 PCIe Switch 的那条链路   按 ETH 实例配（带宽与延迟见链路的参数）
```

***

## 7　机制覆盖

| 机制 | 落点 | 用例 |
| - | - | - |
| 第一层 GPU 本地门控 `buffer_used < buffer_depth_tokens` | G1 第 2 条 | `gpu_credit_local` |
| 第二层 Bach 全局 `pool_avail = pool_total − Σ inflight_bach`，公平发 grant | G1 第 3 条 | `gpu_credit_pool` |
| 两道闸门都开才流；1 credit = 1 token | G1 第 3、5 条 | 同上 |
| 序号回绕模 2^W 比较 | G3 第 2 条 + `common/seq.h` | `seq_wrap` |
| DPU 自定义包头 `gpu_id(8) + token_id(16)`，Payload ≤ 64 KB | G2 第 1 条 | `gpu_header` |
| 注入的包带 `dst` 与 `path_id`：PCIe Switch 按 `dst` 查目的端口，进了阵列之后每一跳的 Router 按 `path_id` 查自己的 RouterTable | G2 第 1 条 | `inject_header` |
| 完成通知：Router 侧按已接收字节数与包头 payload 大小判 token 收完 | Router 文档 C1 第 5 条上报，桩侧 G3 记账 | `token_complete` |
| 组播反压按最慢收端聚合，retired 取 min | G3 第 1 条 | `gpu_retired_min` |
| LPU Dispatch 派遣：所有 R core 有余量才派，派时各减 1，最终 R core 返回后各加 1，顺序调度 | G1 第 4 条、G3 第 3 条 | `dispatch_reduce_slots` |
| B core Matrix Mem 槽位 ≥ 32 × 26 = 832 | 参数校验 | `bcore_slots` |
| 发包时 MSG Header 由 DTE 写入；加载 weights 时由 DPU 写入 | G2 第 1 条 | `msg_header_writer` |
| 先发最远路径的数据 | `inject_tbl` 顺序 | `weights_far_first` |

***

## 8　取舍

* **桩为什么自带路由器，不直接接 PCIe Switch**
  * 让桩的包与 core 的包走同一套 flit 与链路模型
  * 出口桩的落地重组因此与 CoreStation 的进 core 通路同构
* **两层 credit 为什么全记在桩里，不分到 Switch**
  * 《软件栈》把两道闸门都定义在 GPU 侧
* **CPU、SmartNIC、ETH 交换机为什么不各建一个模块**
  * 它们在 LPU 之外，对 LPU 的作用只有三样：token 什么时候进来、包头写什么、这一段链路多宽多长
  * 三样都能表达成注入表的字段与一条链路实例的参数
