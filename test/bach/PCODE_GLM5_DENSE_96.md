# GLM5 MoE：96 个完整权重单 token 用例

用户确认的配置：hidden=6144、intermediate=2048、256 个 routed expert、一个 shared expert、每 token 8 个不同的 routed expert；48 chip、EP6、TP8。

全局专家连续分配：EP0=0–42、EP1=43–85、EP2=86–128、EP3=129–171、EP4=172–214、EP5=215–255。shared 在六组各有一个逐字节相同的副本，只在 `user ID % 6` 执行一次，系数为 1。八个 routed 系数非负，总和为 1，TopK 由外部输入提供。

## 固定完整模型

合成模型种子为 20261012。系数由全局专家身份、阶段、逻辑 TP 分片和 core 槽位决定，与 token、TopK 位置及发送顺序无关。shared 的种子不含部署 EP。所有 K 位置均生成非零、带符号的合法 MXFP8 系数，每 32 个系数有一个 E8M0 scale；不存在稀疏 K 采样。它是功能测试模型，不是真实 checkpoint。

FC13 逻辑形状为 4096×6144，FC1/FC3 合并。每个核的分片为 512×768；前 256 行为该 TP 分片的 FC1，后 256 行为对应 FC3，两半独立生成。FC2 为 6144×2048，每核为 768×256。按 MU 导出的 N128×K64 tile 布局写入。

每例预置全部 256 个 routed expert 和全部六份 shared 副本，包含 768 个 stage/core 内存镜像，权重正文 9,890,168,832 B，另有 1/32 的 scale 旁带。全部镜像按只读文件映射作为 SRAM 初始内容，写入仍建立可变行覆盖；所有 MU/DTE 访问使用原来的银行仲裁、冲突检查和延迟路径。未选择的专家也有完整、可寻址的数据。

模型 manifest 记录每个镜像的 SHA256 和每个专家分片的 SHA256。预置时核对实际导出的地址、shape、专家步长、全部专家数量、文件长度和读回数据。源模型的 shared 分片在六组中逐字节核对。

## 用例分组

| 类别 | 数量 | 定义 |
| --- | ---: | --- |
| 专家覆盖 | 32 | 全局编号按八个一组：0–7、8–15、…、248–255；全部 256 个恰好覆盖 |
| EP 分布 | 18 | 六个单 EP、五个相邻 EP、四个隔空 EP、两个交错 EP、一个全 EP；循环分配八个不同专家 |
| 编号边界 | 18 | 六组首八个/末八个共十二例；五个相邻组分界各前四/后四；全局首四/末四 |
| 数值输入 | 16 | 正、负、混合符号、零、小、大、不同 scale、交替符号，各两个输入种子 |
| TopK 顺序 | 6 | 集中、跨空、全 EP 各一组正序/逆序，专家与系数绑定，不改变模型或输入 |
| shared 副本 | 6 | 相同输入/TopK，依次选择六个 shared 执行 EP |

每例严格八个 routed 加一个 shared；没有仅 shared 用例。UID 0/1023 边界合并到集合中。EP mask 采用代表性覆盖，不声明全部 63 个 mask 的完整遍历。

每例同时记录 `routed_mask`、`shared_ep`、`actual_ep_mask`（整数和十六进制）：

```text
routed_mask = OR(1 << ep_of(expert) for expert in chosen_8)
actual_ep_mask = routed_mask | (1 << shared_ep)
```

全零输入和系数为零都不清除已选专家的 EP 位。未选计算 EP 不得留下归约任务。

## 独立参考和验收

参考用 NumPy 从完整模型镜像及输入独立计算 FC13、TP 归约、BF16 传输、SiLU×FC3、MXFP8 量化、FC2、TopK 加权和 EP 合并。不会使用 latch 计算结果作为参考，也不调用 latch/pcode 的算子实现。解析路由配置仅用于传输精度和 TP 来源覆盖检查。

每例只加载 pcode 导出的 `pcode_ep6_tp8` BACHIR、DSA 配置与 RISC-V HEX，在新 latch 进程中执行一个 token。原 latch MoE 编译器和 `LoadLpu` 不参与。

非零参考余弦相似度至少 0.99999，相对 L2 不超过 0.02；零参考严格输出零。还要求恰好一个 6144 元素 BF16 输出、身份正确、全部计算任务退休、归约任务及输入/输出队列清空、没有 MU drain 或 Router overflow。native 检查 fixture 的专家编号、数量、权重和实际 EP mask，并记录实际加载的全部镜像数量。

shared 的模型分片和独立 shared 贡献必须相同。TopK 顺序对照使用数值阈值。shared 执行位置改变会移动 BF16 归约舍入位置，因此分别比较每例实际输出与各自参考，另检查实际输出差异与参考舍入差异的残差；不能把两个不同舍入路径的最终输出直接套用单例余弦阈值。直接对比指标仍作为诊断保存。

## 2026-10-12 最终结果

**96/96 native 用例通过，0 失败；全部 96 个输出重新生成独立参考复核通过。** 每例完整预置模型，严格八个 routed expert 加一个 shared；原 latch MoE 编译器未参与。

| 指标 | 实测 |
| --- | ---: |
| 非零输出最低余弦（94 例） | 0.9999999777156279 |
| 最大相对 L2 | 0.0002111749947904292（约 0.0211%） |
| BF16 逐位一致 | 8/96 |
| 零参考且实际严格为零 | 2 |
| 单例最多 BF16 差异 | 21/6144 |
| 完成周期范围 | 7823–9740 |

全部计算任务退休，无残留归约任务及输入/输出队列，无 MU drain 或 Router overflow；全部 native mask 日志与 fixture/参数相符。覆盖 30 个不同 routed mask、46 个不同实际 EP mask，不声明全部 63 个 mask 的遍历。

程序审计核对 855 个 RV HEX、4514 个路由项、12 行的 384 个 FC13 计算目的地掩码、768 个 MU 模板。完整模型的 768 个镜像及 33536 个专家分片摘要一致；六组 shared 的 128 个逻辑分片逐字节一致。回归：存储 25、MU 18、DTE lane 15、bundle 13，共 71 项 native 测试通过；Python 17 项通过。

逐例三个 mask 字段和数值结果见 [`pcode_glm5_dense_results.txt`](pcode_glm5_dense_results.txt)；完整参数、摘要和复核证据见 [`pcode_glm5_dense_results.json`](pcode_glm5_dense_results.json)。

执行过程中，附加的 shared 对照曾错误地要求不同 EP 的最终输出直接达到单例余弦阈值。实际输出与各自独立参考均通过，独立参考之间也出现同样的 BF16 舍入差异；shared 权重及其独立贡献完全一致。修正仅涉及这个附加后处理，未改变程序、模型、输入、数值计算或单例验收阈值，未重跑 native 96 例；重新生成全部 96 份参考并验证实际差异符合参考差异后，全部对照通过。最大全局归一化差异残差约 0.0105%。报告保留直接对比指标及执行时源文件摘要。

执行时生成器源文件已保存在 `build-ep6/glm5-dense-96/executed_campaign.py`，其摘要与 96 例记录相符；报告另记录修正后的复核及后处理源码摘要。

## 运行

先在配套 BachFlow 工作树生成并导出 pcode 程序，再在 latch 工作树执行：

```bash
./test/bach/run_pcode_glm5_dense.sh --threads 2 --workers 2 \
  --out build-ep6/glm5-dense-96
```

脚本使用既有隔离 `.venv`，不安装全局依赖。完整模型一次生成并保存，后续先核对全部镜像摘要再复用。`--filter route_20` 可试跑一例；`--resume` 仅复用程序、模拟器、生成器、参考、模型和线程配置一致且实际产物摘要匹配的通过结果。

逐例保存参数、三个 mask 字段、输入 fixture、独立 BF16 参考、真实模拟器输出、日志、数值误差和来源摘要。`summary.json` 必须 `total == planned == passed == 96` 且 failed 为空才表示全部通过。实现及测试进行中的局部结果不能代替完整结果。

完成后执行全量复核：

```bash
../BachFlow-ep6-single-token/.venv/bin/python test/bach/pcode_glm5_dense_verify.py \
  --bundle ../BachFlow-ep6-single-token/pcode/output/latch_bundle/pcode_ep6_tp8 \
  --catalog ../BachFlow-ep6-single-token/pcode/output/ep6_single_token/route/route.resolved.json
```
