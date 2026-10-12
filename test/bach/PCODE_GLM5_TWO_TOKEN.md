# GLM5 EP6+TP8 双 token：pcode → latch

## 模型与程序

沿用 96 例单 token 的固定完整伪随机模型：隐藏维 6144、中间维 2048、256 个 routed expert、1 个逻辑 shared expert、每 token 外部给定 8 个不同专家与非负融合权重（总和 1）。EP0–EP4 各部署连续 43 个 routed expert，EP5 部署最后 41 个；6 份 shared 权重完全相同。每 token 只在 `user ID % 6` 的 EP 执行一次 shared，系数为 1。

FC13 合并不同的 FC1/FC3 系数，逻辑形状 4096×6144；FC2 为 6144×2048。沿用模型种子 `20261012`，所有矩阵位置有完整伪随机系数和 scale。每组只预置一次全部 256 个专家及 6 份 shared 的 768 个 MatrixMem 镜像，数据 9,890,168,832 字节，另有 1/32 scale。两包之间不重载程序、不复位芯片、不改权重。

只运行 BachFlow pcode 导出的 `pcode_ep6_tp8` BACHIR 11、DSA 配置、855 个 RV HEX；不调用 latch 内部 MoE 编译器或 `LoadLpu`。同时审计 12 行 FC13 的 384 个计算目的地 EP mask。

## 96 组并发用例

| 类别 | 组数 | 差异与覆盖 |
| --- | ---: | --- |
| mask/shared 交叉 | 36 | 遍历两个 token 的 shared 执行 EP 的全部 6×6 有序组合；两包 routed mask 共覆盖 0x01–0x3f 全部 63 种非空值；不同专家、输入和融合权重绑定 |
| 全专家覆盖 | 16 | 每包 8 个连续全局专家，每组相邻两块，共覆盖全部 256 个专家，包括 EP5 的最后合法编号 255 |
| 编号边界 | 18 | 各 EP 首 8 个、末 8 个、相邻 EP 的分界两侧、全局首尾；与另一边界包并发 |
| 数值与发送顺序 | 16 | 正、负、混合、零、小、大、不同 scale、交替符号；每种数值包与另一 profile 的包正向和反向发送 |
| 竞争与隔离 | 10 | 专家完全相同、部分重叠、不同输入、相同 payload 不同身份（UID 3/1023）、EP5↔EP0、相同 actual mask 不同 routed mask、相同 routed mask 不同 shared EP、TopK 重排、零融合系数、零输入与非零输入隔离 |

所有用例都是严格有效的 GLM5 双 token：每包 8 个不同 routed expert 加一次 shared。零系数和零输入仍保留专家与 EP mask，不转换成仅 shared 的诊断用例。

每组两个 user ID 不同，符合模拟器的在途用户唯一性约束；token ID 也不同。本轮不做同 UID 的在途并发。宿主可使用一个或两个线程；硬件模型的并发依据是同一拍的两份 TS 快照身份，不按宿主线程数推断。注入间隔请求取 1、128、512 周期；EP5→EP0 的专门竞争用例取 2048 周期，补偿远端与近端的到达时差，按实际入口 credit 发送。必须证明第二包已经发送且两个 user ID 同时存在于计算 core 的 TS 表快照中，并且这一时刻早于任一输出。快照通过模拟器已有打拍端口读取，不读取运行中的可变表对象。

每 token 单独记录 `routed mask`、`shared EP`、`actual EP mask = routed mask | (1 << shared EP)`，并与原生模拟器解析 fixture 后的记录逐一匹配。

如果两份数值输出和全部状态检查均通过、唯一失败是未观察到计算任务重叠，驱动允许将间隔改为 2048 或 4096 周期，或交换两包的注入顺序重试。近端先计算完、远端才开始时，先注入远端可以形成计算重叠。逻辑 token 的身份、专家与参考输出不变，报告记录实际 `injection_order`，原生日志不改写。所有调整前日志和实际输出单独保留并记录 SHA256；专家、输入、TopK、模型、程序、数值阈值保持一致。任何数值、身份、mask、队列或任务状态失败均不走此重试。最终每一组都必须有实际同时在途的计算任务证据；所有候选顺序与间隔均不能满足时仍判失败。

## 验收与证据

独立 NumPy 参考逐包计算完整矩阵、FC13 TP 归约、BF16/SiLU/MXFP8 转换、FC2 加权及 EP 合并。每包采用其实际 shared 执行位置的参考，不要求不同 shared 位置的最终 BF16 输出逐位相等。

非零参考要求余弦相似度至少 0.99999、相对 L2 不超过 0.02；全零参考要求实际严格数值全零。原生检查必须通过：恰好两份 6144 个 BF16 输出、身份正确、无重复/丢失、计算任务全部退休、所有归约上下文和输入/输出队列清空、无 MU drain、无 Router overflow。

各组保留两份 fixture、独立参考、实际输出、发送/接收周期、同时在途计算的证据周期、原生日志及 SHA256。最终复核重新生成全部 192 份参考，解码并核对全部 fixture、重新对比实际输出、核验模拟器/脚本/程序/模型/日志摘要。

全专家覆盖的 32 个 token 沿用上轮单 token 的输入、专家、融合权重和 shared 位置；除了独立数值参考，还与保存的单 token 实际输出比较，检查并发没有引入串扰。核对单 token 报告、模型、程序及输出 SHA256。由于本轮修正了导出的 VC 映射，若上一轮 bundle 摘要不同，必须提供原 bundle，并证明 BACHIR 的差异仅为 RTAB/TSRTAB 的 VC 字段，855 个 RV HEX 和全部非 VC 程序记录完全一致；独立参考也必须逐位保持一致。相同 payload 的两包另作直接输出对照。

## 复现

先在 BachFlow 工作树重新导出配套程序：

```bash
./pcode/run_ep6_single_token.sh
```

然后在 latch 工作树使用已建虚拟环境运行：

```bash
./test/bach/run_pcode_glm5_two_token.sh --workers 4 --threads 2
```

筛选一组用 `--filter contention_ep5_to_ep0`；`--resume` 核验执行身份和产物 SHA256。仅测试驱动的时序控制改变时，可复用同一模拟器、程序、模型、用例计划与独立参考的已通过结果；驱动原源码保存在 `runner_sources/<SHA256>.py`，每组保留实际执行的驱动摘要，最终复核逐组校验。模拟器、程序、模型、用例或参考发生改变时，不复用旧结果。

全部组完成后进行独立复核：

本机复核还使用上一轮 `build-ep6/glm5-dense-96` 的单 token 实际输出及 `pcode_glm5_dense_results.json`；两者需配套保留，以核验 32 份基线摘要。本机还保留改 VC 前的 bundle 于 `build-ep6/glm5-two-token-before-transfer-vc-fix/exported_bundle`；用 `--single-baseline-bundle` 可指定其位置。新环境若使用本轮 bundle 重跑单 token 基线，则不需要旧 bundle 的 VC 等价性对照。如果没有该基线，应先按 `PCODE_GLM5_DENSE_96.md` 完成单 token 集合及独立复核。附加回归证据按下列命令生成，单 token 和稀疏双 token 回归不计入主测的 96 组：

```bash
../BachFlow-ep6-single-token/.venv/bin/cmake --build build-ep6 \
  --target memory router_reduce router_scenarios ts_credit bundle_load \
    router_basic router_arbiter router_assembly dte_inbound dte_lane -j 4
for regression_target in memory router_reduce router_scenarios ts_credit bundle_load \
    router_basic router_arbiter router_assembly dte_inbound dte_lane; do
  ./build-ep6/"$regression_target" \
    > "build-ep6/glm5-two-token-$regression_target-regression.log" 2>&1 || break
done
./test/bach/run_pcode_two_token.sh --filter route_overlap_03 --threads 2 --workers 1 \
  --out build-ep6/glm5-two-token-legacy-compat
./test/bach/run_pcode_glm5_dense.sh --filter route_20 --threads 2 --workers 1 \
  --out build-ep6/glm5-two-token-single-compat
```

```bash
PYTHONNOUSERSITE=1 ../BachFlow-ep6-single-token/.venv/bin/python -u \
  test/bach/pcode_glm5_two_token_verify.py \
  --bundle ../BachFlow-ep6-single-token/pcode/output/latch_bundle/pcode_ep6_tp8 \
  --catalog ../BachFlow-ep6-single-token/pcode/output/ep6_single_token/route/route.resolved.json
```

本轮仅验证两包并发的一个 MoE 层；模型为完整合成权重，不是 GLM5 真实检查点，不涵盖更长 token 流、16 stream 环回复用、外部模型搬运或性能验收。

## 本轮发现的 Router 修复

`expert_blocks_09` 的两包 routed mask 均为 `0x08`，shared 分别在 EP0/EP1，实际 mask 为 `0x09`/`0x0a`。原模型在 chip31/core4 的入核数据通路把两个 12,288 字节的包交织：第一帧累计 15,616 字节，第二帧只剩 8,960 字节，Header Parser 因包尾长度不足而中止。原生诊断和修复前全部产物保留在 `build-ep6/glm5-two-token-before-header-fix` 与 `build-ep6/glm5-two-token-header-diag`。

原因是 RouterStation 对各输入 VC 轮询，而不同输入 VC 可以映射到相同的下一跳 VC。Xbar 的同输出 VC 归属只记录输入方向，无法识别同一个输入方向已经混入其 FIFO 的两个包。RouterStation 现在按 `(输出方向, 下一跳 VC)` 保持输入 VC 的归属，直到该包尾 flit 入队后才释放；不同方向或不同下一跳 VC 仍可前进。新增三项测试确认映射碰撞的包连续性，以及独立 VC、独立方向不会受阻。

小回归修复前顺序为 `A,B,A,B,A,B`，修复后为 `A,A,A,B,B,B`。原失败用例在修复版本上通过，随后重新执行整个 96 组，修复前通过的用例不复用。模拟器宿主线程数可为 1 或 2，硬件时钟模型中的两包是否并发由 TS 快照与发送/接收周期验证。


## 归约输入的跨 VC 包边界

Xbar 的每个 reduce 输入 lane 只有一个接收 FIFO。原逻辑只对相同 VC 保持包的输出归属，来自不同 VC 的包可以变成 `A,B,A,B`。本轮对每个 reduce lane 应用与 core 入口相同的包独占规则，直到尾 flit 才放行另一 VC；不同 reduce lane 仍然独立前进。两项回归分别验证同 lane 的包连续性与不同 lane 的独立性。此修正补齐了包边界，但单独应用后 `boundary_pair_15` 仍未完成，剩余阻塞由下述 VC 映射修正解决。

## pcode 导出 VC 与流式归约的冲突

`boundary_pair_15` 的两包分别为 routed `0x18` / shared EP0 / actual `0x19`，以及 routed `0x04` / shared EP3 / actual `0x0c`。第一包的 EP 传输 `path33` 与第二包的 FC13 流式归约 `path2` 在 chip23/core2 相遇。目录把普通传输下一跳映射到 VC3，latch 的 ReduceModule 也固定在 VC3 输出。现场记录 `path33` 包头等待，而 VC3 的输出归属为 ReduceModule，链路还有 private/shared credit 各 20；FC13 流式结果又在等待被前者阻挡的剩余输入，形成循环等待。

BachFlow 的 `pcode/src/bachflow_pcode/latch_export.py` 现在为 latch 保留 VC3 给归约流量，普通传输的 planner VC3 映射到 VC0，planner VC0–2 保持一致。表项的 `RTAB` 与供 TS 查 credit 的 `TSRTAB` 同步变化，归约合成透传项继承上游 VC。归约保持流式输出。相同输入下，修正前模拟 16,000 周期无输出，修正后第 8,652/8,876 周期收到两份输出，且第 2,415 周期已证明同时计算。

重新导出后已核验：只修改了 253 个 RTAB 与对应 253 个 TSRTAB 的 VC 字段；855 个 RV HEX、DSA 参数及其他程序记录均一致。原失败现场保存在 `build-ep6/glm5-two-token-boundary15-reduce-owner-fixed`，配套原 bundle 保留在前述目录。修改模拟器或 bundle 后，完整 96 组使用最终版本重新执行，前一版本的已通过用例不复用。

## 最终结果

独立复核时间：2026-10-12T22:11:37.542994+08:00。主测 **96/96 组通过，192/192 个输出通过**；每组均执行 pcode 导出程序，证明两份计算任务同拍在途，并通过身份、长度、任务退休和归约队列清空检查。

| 项目 | 结果 |
| --- | --- |
| 非零参考最低余弦 | 0.9999999513380896，要求 ≥0.99999 |
| 最大相对 L2 | 0.000312042448837（0.031204245%），要求 ≤2% |
| 零参考输出 | 5 份，全部严格数值全零；余弦未定义 |
| 与独立参考逐位一致 | 18/192；其余每份最多 14/6144 个 BF16 值不同，均满足数值条件 |
| 专家覆盖 | 全部 256 个 routed expert |
| routed / actual EP mask | 63 种非空 routed mask；60 种 actual mask |
| shared EP 有序配对 | 全部 36 种 |
| 计算重叠证据周期 | 359–4222 |
| 单 token 基线对照 | 32/32 通过，其中 31 份逐位一致；最低余弦 0.9999999993674606 |
| 相同 payload、不同身份对照 | 两份实际输出逐位一致 |
| 原生回归 | 138 项通过 |
| Python 测试驱动检查 | 24 项通过 |
| pcode 测试 | 72 项通过，14 项跳过 |
| 兼容回归 | 最终版本的稀疏 pcode 双 token、完整权重单 token 各 1 组通过 |

`boundary_pair_01` 的三次原顺序尝试均通过数值和状态检查，但计算阶段未重叠。最终交换注入顺序、间隔 2048 周期后通过，保存三次尝试的日志和实际输出并独立复核。其他 95 组按原定时序通过；最终所有组均有计算重叠证据。

本机目录中有 1578 个 HEX 缓存文件，实际 BACHIR 引用 855 个不同的 RV 镜像。VC 修改前后全部缓存字节一致，复核明确区分目录缓存数量与程序引用数量。

逐包报告：`pcode_glm5_two_token_results.txt`，列出全局专家、输入 profile/seed、routed mask、shared EP、actual mask、误差、发送/输出周期和计算重叠周期。完整证据与摘要保存在 `pcode_glm5_two_token_results.json`。

32 份单 token 基线中，`experts_128_135` 有 1/6144 个 BF16 值不同，相对 L2 为 0.0000355684672（0.00355685%），余弦为 0.9999999993674606；符合数值验收。其余 31 份逐位一致。
