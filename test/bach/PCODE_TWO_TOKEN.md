# EP6/TP8 双 token 的 pcode → latch 集成验证

每个用例只启动一次 latch，加载 BachFlow pcode 导出的 `pcode_ep6_tp8` BACHIR、DSA 配置和 855 个 RISC-V HEX，在同一组 48 chip 上发送两个 token。没有调用 latch 自带的 MoE 编译器或 `LoadLpu`；两包之间不重新加载程序、不复位芯片、不更换模型权重。

配置保持 EP6、TP8、隐藏维度 6144、专家中间维度 2048。矩阵是受控稀疏测试权重，预置到 MatrixMem；输入通过实际入口、Router 和 DTE 进入程序。

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

模拟器必须正常退出，恰好输出两包，计算任务全部退休，没有 MU drain 或 Router overflow。输出按 `(user ID, token ID)` 配对，拒绝未知身份、重复输出、漏输出；不依赖两包完成顺序。每包必须有 6144 个 BF16 值。

- 非零参考：余弦相似度至少 `0.99999`，相对 L2 误差不超过 `0.02`。
- 路由和边界 token：额外要求 BF16 逐位一致。
- 全零参考：实际输出严格全零，余弦相似度记为 `null`。
- 发送/输出周期必须满足用例声明的重叠或顺序条件。

## 双 token 工作发现的修复

1. BachFlow `latch_export.py`：pcode 的 packet mask 筛选既有 CORE 目的地，而 latch 的 mask-enable 覆盖 bypass。只转发的广播源启用 mask 会变成进核目的地，引起自身包重新进入并重复广播。转换时只有原始方向包含 CORE 才启用 mask。
2. latch `dte_types.h`：pcode 的 TopK 表下标使用 AGCU 展开的地址，包含 `SID × stride`，不再只用原始基址。否则第二个 stream 写入第一份 TopK 表。新增 SID 0、1、15 的进核和出核检查。

测试驱动新增按入口 credit 发送两包的回调；没有使用旧 MoE 用例的 B-core 落点规则，也没有在两包之间修改硬件配置。

## 复现和证据

先在 BachFlow 工作树重新生成、导出配套程序：

```bash
./pcode/run_ep6_single_token.sh
```

然后在 latch 工作树运行：

```bash
./test/bach/run_pcode_two_token.sh --threads 2 --workers 4 \
  --out build-ep6/two-token
```

脚本使用配套 BachFlow `.venv`。`--filter route_overlap_00` 可运行一组；`--resume` 仅复用已通过、程序/脚本/线程数一致且 fixture/实际输出 SHA256 匹配的结果。

`build-ep6/two-token/summary.json` 只有 `total == planned == 52` 且 `failed` 为空才表示完整集合通过。每组保留两份 fixture、参数、独立参考、实际输出、日志和验收报告；报告含发送/接收周期、两包的独立数值指标、共享矩阵审计及程序 SHA256。

本轮覆盖两包同进程运行，不覆盖 16 个计算 stream 的环回复用、长 token 流、真实稠密权重、外部权重加载或性能验收。
