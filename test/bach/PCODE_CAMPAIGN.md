# EP6/TP8 单 token 扩展数值测试

配套分支均为本地 `pcode_dev_zlm`。每个用例启动一个新的 latch 进程，加载 BachFlow pcode 实际导出的 BACHIR、DSA 寄存器配置与 RISC-V HEX，再注入一个 token；没有使用 latch 自带编译器替代 pcode 程序。

全部 134 个集成用例使用这组 EP6/TP8 pcode 算子程序，分别改变运行时 token、topK 和矩阵数据。测试入口只调用 `LoadBundle(..., "pcode_ep6_tp8")`，不调用旧 MoE 用例的 `LoadLpu`。原 latch 的 DTE/VU 等单元测试仅用于修复回归，不计入这 134 个 pcode→latch 拉通用例。

## 覆盖范围

默认共 134 个用例，均使用 48 chip、EP6、TP8、隐藏维度 6144、专家中间维度 2048。

| 类型 | 数量 | 内容 |
| --- | ---: | --- |
| 路由 | 63 | 遍历非空 EP mask `0x01` 到 `0x3f`；八个路由专家分布在活跃 EP，验证跨越空 EP 的归约及选路 |
| 边界 | 7 | 六个仅共享专家用例，分别由 EP0–EP5 执行；一个 user ID 为 1023 的全 EP 用例 |
| 数值 | 64 | 固定种子 `20261010`–`20261073`，随机专家分布、专家编号、非均匀 topK 权重、输入、矩阵权重及 scale |

数值输入覆盖正数、负数、混合符号、全零、小幅值、大幅值、抵消和不同 scale。随机专家编号覆盖各 EP 的最后合法专家。普通数值用例每个矩阵输出行有四个非零 K 位置，分布于不同 tile 和不同 scale block；`random_000/016/032/048` 在所有 K tile 上放置非零值，检查跨完整 K 维度的累加。

权重是受控稀疏矩阵，矩阵尺寸和执行链路使用实际配置。该测试不等同于加载真实模型的完整稠密检查点，也不覆盖多 token 或吞吐率。

输入通过实际进核路径注入，矩阵权重预置到 MatrixMem。外部权重加载、转存重发不在本轮覆盖范围内。

## 本地实测结果

2026-10-11，完整集合 **134/134 通过，0 失败**。逐例记录见 [pcode_campaign_results.json](pcode_campaign_results.json)。全部集成结果均来自 `pcode_ep6_tp8` 导出的实际程序；原 latch 的单元测试没有计入这个数量。

| 集合 | 实际完成 | 通过 | 数值结果 |
| --- | ---: | ---: | --- |
| 所有非空 EP mask | 63 | 63 | 每例 6144 个 BF16 值逐位一致 |
| 共享专家与 user ID 边界 | 7 | 7 | 每例 6144 个 BF16 值逐位一致 |
| 固定种子随机数值 | 64 | 64 | 非零输出最低余弦相似度 `0.9999999508227205`；八个全零输入严格输出零 |

全体最大相对 L2 误差为 `0.00031361530006680837`。100 例输出逐位一致；其余 34 例满足原有余弦和误差判据，不宣称逐位一致。逐例 BF16 差异数量、数值误差和原始输出摘要均保留在报告中。

同时独立检查实际 BACHIR 中的 768 个 MU 模板：384 个 FC13 模板均为 `512×768`、FP8 专家步长 `393216` B；384 个 FC2 模板均为 `768×256`、FP8 专家步长 `196608` B。这项检查按模型预期尺寸与步长验收，避免仅按错误的配置预置权重而掩盖配置错误。

程序来源：BACHIR 11，`SOURCE pcode/output/ep6_single_token`，48 chip，855 个 RISC-V HEX。所有用例的 bundle、模拟器、测试脚本和精度参考摘要一致。已核对每例 fixture 与实际输出的 SHA256，均与其验收报告相符。测试环境为隔离的 Python 3.12.13 / NumPy 2.5.3，arm64 macOS；每例两个模拟线程，最多四例并发。

原 `full/gap` 拓扑分别由 `mask_3f/mask_2b` 覆盖，最终输出 SHA256 与原确定性用例相同。对应周期为 9726 / 8869；修复后的周期计数与此前报告可以不同，不作为性能比较。

修复回归另计：pcode 68 项通过、14 项因旧 PP 快照缺失跳过；latch 的 DTE、VU、DTE lane、RISC-V core、Router reduction、TS done 六个测试程序全部通过；独立参考验收正负例七项通过。

## 独立参考和通过条件

`pcode_campaign.py` 使用 NumPy 从输入与权重独立计算 GEMV、SiLU、逐元素乘法、MXFP8 量化、专家权重和最终归约。它不导入 pcode 或 latch 的算子实现，也不读取模拟器输出作为参考。

`pcode_precision_reference.py` 从导出配置读取归约图与传输精度，独立模拟 FP32/BF16 精度转换。每次 TP 归约还检查八个源的贡献各出现一次，缺失或重复均报错。读取的是执行配置；被比较的数值全部由参考计算产生。

每例必须满足：

- 模拟器正常退出，所有计算任务退休，没有 MU drain 或 Router 转存。
- 最终只有一个输出包，user ID 与 token ID 正确，输出恰好为 6144 个 BF16 值。
- 非零参考输出的余弦相似度至少为 `0.99999`，相对 L2 误差不超过 `0.02`。L2 检查用于排除数值整体缩放错误，因为余弦相似度无法发现这种错误。
- 路由和边界用例额外要求 BF16 逐位一致。
- 零参考输出要求实际输出全部为零。其余弦相似度记为 `null`，不将未定义的值写成 1。

阈值没有为通过测试而降低。参考本身也有负例：漏掉贡献、重复贡献、将正确输出放大两倍、零参考却产生非零输出都不能通过。

## 扩展测试发现的修复

1. DTE 的 pcode 包头未保留 user ID，非零 ID 输出变为 0。补齐包头读写与传递，并增加多个 ID 的单元测试。
2. pcode 向量算子的两个 macro 通过 VRF 交换 FP32 数值，第一个却配置为 BF16。改为一致的 FP32 VRF；LU 仍按实际 BF16 输入解码。集中专家和随机输入曾分别得到约 `0.99666`、`0.96513` 的余弦相似度，扩展测试发现了原两个用例未揭示的错误。
3. latch 的 VU LU 没有执行 run/skip stride，导致 gate/up 交错输入读错。实现按配置跳读及部分 block 收集，并修复分段 MXFP8 读取的 scale 索引。新增九专家 BF16、非对齐短 stride、分段 MXFP8 的测试。

另修正了独立参考省略中间 BF16 传输舍入的问题。参考按照导出配置执行舍入，而不是降低验收阈值或替换实际输出。历史失败的输出与报告保留在 `build-ep6/campaign/history/` 及 `*.before-*-fix.*` 中。

## 复现

先在配套 BachFlow 工作树执行 `./pcode/run_ep6_single_token.sh`，生成最新 bundle 并运行原来的 `full/gap` 用例。再在 latch 工作树执行：

```bash
./test/bach/run_pcode_campaign.sh --threads 2 --workers 4 \
  --out build-ep6/campaign
```

脚本使用配套 BachFlow 的 `.venv`，依赖不安装到全局环境。可通过 `BACH_ROOT` 指定其路径。`--threads` 是每个模拟器进程的工作线程数，`--workers` 是同时运行的用例数。每例模拟周期上限 30000，进程超时 600 秒。

断点续跑：

```bash
./test/bach/run_pcode_campaign.sh --threads 2 --workers 4 \
  --out build-ep6/campaign --resume
```

只有此前通过、且 bundle（含 HEX）、模拟器、测试脚本、精度参考及线程数一致的结果才会复用；其余重新执行。`--filter random_` 可只运行随机数值用例，`--random-cases 128` 可扩展固定种子数量。

参考单元测试：

```bash
../BachFlow-ep6-single-token/.venv/bin/python -m unittest discover \
  -s test/bach -p 'test_pcode*.py' -v
```

## 产物

`build-ep6/campaign/summary.json` 随每个完成的用例更新。`total` 是已完成数量，`planned` 是计划数量；只有 `total == planned` 且 `failed` 为空，才能说明整轮通过。

使用 `--filter` 时，`planned` 只表示该筛选集合；默认完整覆盖应为 `total == planned == 134`。提前执行随机集合后，再执行不带 `--filter` 的 `--resume`，可汇总完整结果并补跑剩余用例。

每例保留 `.fixture` 输入与权重、`.json` 参数、`.expected.bf16` 独立参考、`.actual.bf16` 模拟器输出、`.log` 执行日志、`.report.json` 数值验收及各程序 SHA256。`.intermediates.npz` 是参考计算的中间值，仅供定位差异。
