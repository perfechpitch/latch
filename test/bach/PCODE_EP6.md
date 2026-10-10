# pcode EP6/TP8 单 token 功能验收

分支 `pcode_dev_zlm` 基于 latch 的 `pcode_dev_jxc`，起始提交为 `198c8dfaff331cb9c0d33a0004cf65f2e5052e1b`。配套 BachFlow 分支同名，起始提交为 `96853a104c0d803184a2b633ab973b2aed75c970`。

从配套 BachFlow 工作树执行 `./pcode/run_ep6_single_token.sh`，可完成生成、RISC-V 编译、latch 构建、两个单 token 用例和余弦验收。完整配置说明见 BachFlow 的 `pcode/EP6_SINGLE_TOKEN.md`。

已有 bundle 时，在当前 latch 根目录执行：

```bash
PCODE_BUNDLE_ROOT='../BachFlow-ep6-single-token/pcode/output/latch_bundle' \
PCODE_CASE=full ./build-ep6/pcode_bundle
../BachFlow-ep6-single-token/.venv/bin/python test/bach/pcode_reference.py \
  build-ep6/actual-full.bf16 --case full --report build-ep6/numerical-full.json

PCODE_BUNDLE_ROOT='../BachFlow-ep6-single-token/pcode/output/latch_bundle' \
PCODE_CASE=gap ./build-ep6/pcode_bundle
../BachFlow-ep6-single-token/.venv/bin/python test/bach/pcode_reference.py \
  build-ep6/actual-gap.bf16 --case gap --report build-ep6/numerical-gap.json
```

`pcode_bundle.cpp` 加载 pcode 导出的各核任务链、寄存器和真实 RISC-V 程序，初始化受控非零稀疏权重，然后注入一个 token。未调用 latch 内置 MoE 编译器生成替代任务链。`pcode_reference.py` 独立计算 6144 维参考向量，报告余弦相似度、最大绝对误差、BF16 不一致数与 SHA256。当前阈值为 `0.99999`；确定性 fixture 还要求逐位一致。

两例都选八个路由专家和一个共享专家：`full` 激活六组 EP，`gap` 跨过 EP2、EP4，检查 dataout A 和 B 的独立 PID 更新。每次都是单 token。

2026-10-10 本地实测，两例各产生一个 12288 B 最终输出包，均与独立参考的 6144 个 BF16 值逐位一致，余弦相似度为 **1.0**、最大绝对误差为 **0.0**。`full` 在周期 9700 完成，`gap` 在周期 8842 完成。使用默认 30000 周期上限；8000 周期不足以完成该链路。数值报告与输出分别保存在 `build-ep6/numerical-*.json` 和 `build-ep6/actual-*.bf16`。

参考脚本自身的正、负例检查：

```bash
../BachFlow-ep6-single-token/.venv/bin/python -m unittest discover \
  -s test/bach -p test_pcode_reference.py -v
```

调试归约 beat 可设置 `PCODE_TRACE_REDUCE=1`。默认日志每 1000 个周期打印进度，失败时打印未完成任务。该验收不覆盖真实模型检查点、128 token 或转存重发压力。
