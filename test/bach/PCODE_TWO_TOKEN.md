# EP6/TP8 双 token 的 pcode → latch 集成验证

每个用例只启动一次 latch，加载 BachFlow pcode 导出的 `pcode_ep6_tp8` BACHIR、DSA 配置和 855 个 RISC-V HEX，在同一组 48 chip 上发送两个 token。没有调用 latch 自带的 MoE 编译器或 `LoadLpu`；两包之间不重新加载程序、不复位芯片、不更换模型权重。

配置保持 EP6、TP8、隐藏维度 6144、专家中间维度 2048。矩阵是受控稀疏测试权重，预置到 MatrixMem；输入通过实际入口、Router 和 DTE 进入程序。

## 本轮实测结果

独立复核时间：`2026-10-11T19:24:50.420473+08:00`。同一修复版本的 52/52 组全部通过，104 个输出已重新对比独立生成的参考；全部 fixture、实际输出和程序摘要通过一致性检查。

| 指标 | 实测 |
| --- | ---: |
| 非零输出最低余弦相似度（96 个输出） | `0.9999999895415747` |
| 最大相对 L2 误差 | `0.0001446541747489629`（约 0.0145%） |
| BF16 逐位一致的输出 | 88/104 |
| 全零参考且实际严格全零 | 8 |
| 单输出最多不同的 BF16 元素 | 4/6144 |
| 每组完成周期范围 | 7923–19494 |

37 组重叠、15 组顺序、9 组同 UID 复用、8 组全 K tile 加强采样均通过；无漏包、重复输出、未知身份、未退休的计算任务、残留归约任务、MU drain 或 Router overflow。独立复核再次审计 52 组固定模型，并核对 855 个 RV HEX、4514 个路由项、384 个 FC13 计算目的地的 EP 掩码及 768 份 MU 模板。

生成程序对应 BachFlow `59d02f534277b8c47b170cf93cec55ac336d4c0e`，模拟器对应 latch `0d282d6da42a7593898fa83d9b0ed00e85ad651f`；两者分支均为 `pcode_dev_zlm`。完整参数、每包指标、发送/接收周期、程序和文件 SHA256 见 [`pcode_two_token_results.json`](pcode_two_token_results.json)。本轮受影响模块回归 74 项通过，fixture/参考/用例驱动 Python 回归 9 项通过。

## 用例设计

默认 52 组、104 个 token：

| 集合 | 组数 | 内容 |
| --- | ---: | --- |
| 路由切换 | 12 | full/gap、EP0/EP5 集中专家、两组间隔 EP；各测试连续发送及完成后再发送 |
| 随机数值 | 24 | 八种输入 profile，各测试连续发送、512 拍间隔的反向发送、同 user ID 在完成后复用 |
| 共享专家边界 | 12 | EP0–EP5 的仅共享专家 → full，以及 full → 仅共享专家 |
| 身份及零值隔离 | 4 | UID 1023 的重叠执行和复用；非零 → 零、零 → 非零 |

其中 37 组要求两个 token 确实重叠：第二包发送周期必须早于第一包输出周期。15 组要求第一包输出且计算 core 的任务退休后才发送第二包，其中 9 组复用同一个 user ID，但使用不同 token ID。重叠执行采用不同 user ID，符合模拟器的在途用户唯一性约束。

两包的输入、路由专家分布、专家编号和 TopK 权重可以不同。每组共享固定模型：随机矩阵按 `(matrix_seed, chip, core, stage, expert)` 生成，与 token 输入、发送顺序和 TopK 无关。加载前直接解析两份 fixture，检查重叠专家的矩阵位置、FP8 编码和 scale 逐字节一致。此检查有“同一专家权重被替换”的负例。

随机矩阵使用按行变化的 scale。普通随机矩阵每行四个采样位置；八组加强用例在每个 K tile 中采样 `0/31/32/63` 四个位置。全部使用实际矩阵尺寸，但不等同于真实模型稠密检查点测试。

## 逐 token 验收

独立 NumPy 参考分别从每包输入、专家选择及固定权重计算结果，不调用 pcode/latch 算子实现。参考依据导出配置模拟中间 FP32/BF16 转换，并检查每次 TP 归约的八个来源各贡献一次。

模拟器必须正常退出，恰好输出两包，计算任务全部退休，没有 MU drain 或 Router overflow，所有核的归约任务及归约输入/输出队列均清空。输出按 `(user ID, token ID)` 配对，拒绝未知身份、重复输出、漏输出；不依赖两包完成顺序。每包必须有 6144 个 BF16 值。

- 非零参考：余弦相似度至少 `0.99999`，相对 L2 误差不超过 `0.02`。
- 路由和边界 token：额外要求 BF16 逐位一致。
- 全零参考：实际输出严格全零，余弦相似度记为 `null`。
- 发送/输出周期必须满足用例声明的重叠或顺序条件。

## 双 token 工作发现的修复

1. BachFlow `latch_export.py`：pcode 的 packet mask 筛选既有 CORE 目的地，而 latch 的 mask-enable 覆盖 bypass。只转发的广播源启用 mask 会变成进核目的地，引起自身包重新进入并重复广播。转换时只有原始方向包含 CORE 才启用 mask。
2. latch `dte_types.h`：pcode 的 TopK 表下标使用 AGCU 展开的地址，包含 `SID × stride`，不再只用原始基址。否则第二个 stream 写入第一份 TopK 表。新增 SID 0、1、15 的进核和出核检查。
3. latch `core_station.h` / `bundle_load.h`：pcode 包头随 DTE 数据通路写入 Hmem（计算 core）或 CoreMem（B/R core），身份由 TS CSR 提供，无需 RV 读取 Router 软件包头 FIFO。装载 pcode 时关闭无人读取的软件包头副本，保留完整数据和 TS trigger；原 latch 程序仍使用软件 FIFO。此前 concat 两包累计超过 16 个包头后，软件 FIFO 满导致后续数据停滞。新增超过 FIFO 深度的 20 包测试，并保留原软件 FIFO 反压测试。
4. latch `agcu.h`：pcode 的 scale 地址和 stride 以 scale 字节计量，DTE lane 则以对应数据字节定位旁带。地址转换时同时把 stride 乘 32，使 SID 1 的 scale 落在数据偏移 65536，而非 2048。新增 SID 0、1、15 的双向地址测试，修复前 SID 1/15 测试失败。
5. BachFlow `glm5_ep6_tp8.py` / `route_api.py`：PCIe 入口也是 EP0 第一行的广播源，未选 EP0 的输入仍会启动这一行，而第二行被 EP 掩码过滤。这留下无法收齐的半份 TP 归约，后续选 EP0 的 token 被堵住。给全部 12 行的 FC13 计算目的地设置各自 EP 掩码，并补齐适配器原先遗漏的 `core_mask_idx` 转换：仅修改既有 CORE 目的地，同时重打包 catalog word 和路由 bin，让 pcode 寄存器与 latch 配置一致。新增测试核对掩码 0/5、二进制和寄存器的对应值，确认源与纯转发节点不被改成进核目的地。修复前 EP5 → EP0 的重叠和顺序用例都只有一包输出；50000 周期诊断显示 EP0 归约中残留第一包 user 461，第二包 user 78 等待剩余方向。
6. BachFlow `route_api.py`：复用路由前校验请求的 SHA256，包含目标坐标和 EP 掩码。仅有旧 catalog 不足以复用；源代码行号变化不触发重搜。新增测试确认更改 packet mask 必须重新搜索，避免修改场景后仍导出旧路由。
7. latch `reduce_module.h` / `core.h`：pcode 的多条 FC2 归约会经过同一核。原先整个用户只允许一条归约路径占用上下文；例如 chip 13 core 8 的 path 10 尚未收齐时，path 11 的两路包无法进入，阻塞又沿上游反向传播。随机输入反向发送暴露了这个问题。pcode 模式下按 `(user ID, path ID)` 保存独立累加状态，共享同一用户的 Rmem 分区；同时检查所有在途路径合计不超过 32 KiB、最多 16 个用户，同一路径仍保护尚未完成的归约序列，用户退休时处理其全部路径。原 latch 模式保留按用户串行的行为。新增交错路径、用户分区数量、共享字节预算和序列保护测试。

8. latch `credit_monitor.h`：原先一直保留上一次收到的 `(user ID, task ID)` 去重记录；同 user ID 的下一包再次执行相同广播任务时，请求被丢弃。现在按请求持续有效的区间去重，端口变为无效时清除记录，同时检查 stream 和 path 身份。新增同一请求保持多拍只授权一次、空闲后完全相同的四个 ID 再次授权的测试。该测试修复前只有一次 grant，修复后两次；同 UID 的双包失败诊断在 19000 周期只有一包输出，第二包停在源核的 task 2 credit 等待。

测试驱动新增按入口 credit 发送两包的回调；没有使用旧 MoE 用例的 B-core 落点规则，也没有在两包之间修改硬件配置。

## 复现和证据

先在 BachFlow 工作树重新生成、导出配套程序：

```bash
./pcode/run_ep6_single_token.sh
```

然后在 latch 工作树运行：

```bash
./test/bach/run_pcode_two_token.sh --threads 2 --workers 2 \
  --out build-ep6/two-token-verified
```

脚本使用配套 BachFlow `.venv`。`--filter route_overlap_00` 可运行一组；`--resume` 仅复用已通过、程序/脚本/线程数一致且 fixture/实际输出 SHA256 匹配的结果。

`build-ep6/two-token-verified/summary.json` 只有 `total == planned == 52` 且 `failed` 为空才表示完整集合通过。每组保留两份 fixture、参数、独立参考、实际输出、日志和验收报告；报告含发送/接收周期、两包的独立数值指标、共享矩阵审计及程序 SHA256。

集合完成后再执行独立复核，重新生成全部 104 份参考及 fixture，并与保存的模拟器输出比较；同时核对各用例使用的模拟器、脚本和导出程序摘要，以及 12 行 FC13 的 384 个带 EP 掩码的计算目的地：

```bash
PYTHONNOUSERSITE=1 ../BachFlow-ep6-single-token/.venv/bin/python \
  test/bach/pcode_two_token_verify.py \
  --bundle ../BachFlow-ep6-single-token/pcode/output/latch_bundle/pcode_ep6_tp8 \
  --catalog ../BachFlow-ep6-single-token/pcode/output/ep6_single_token/route/route.resolved.json \
  --out build-ep6/two-token-verified \
  --report test/bach/pcode_two_token_results.json
```

本轮覆盖两包同进程运行，不覆盖 16 个计算 stream 的环回复用、长 token 流、真实稠密权重、外部权重加载或性能验收。
