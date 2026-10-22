# GLM5 EP6+TP8：10 组 × 16 token

每组在同一个原生 latch 实例中运行 16 个不同用户；期间不复位芯片、不重载程序和模型。全部执行配套 BachFlow 导出的 BACHIR、DSA 配置和 RISC-V HEX，沿用完整固定伪随机 FC13/FC2 模型；不调用 latch 内部 MoE 编译器。

模型配置保持 hidden=6144、intermediate=2048、256 个 routed expert、一个逻辑 shared expert、每 token 8 个不同 routed expert。专家按 43/43/43/43/43/41 连续部署；shared 在 6 个 EP 复制，只在 `user_id % 6` 执行一次。TopK 系数非负且总和 1，shared 系数 1。每组预置全部 768 个模型内存镜像，权重正文 9,890,168,832 字节，另有 scale。

## 飞书设计依据

[Task Scheduler 设计](https://pcng0ddyhlxs.feishu.cn/wiki/VzcMwirkEivgG5knckxcTe7Cnig)，文档 token `S1jydcHJsohSHDxsLVmcfflNnFc`，本次只读核对 revision **18686**：

- `stream_num` 支持 1–16，stream table 顺序 FIFO，最多 16 个用户并行。
- 按 User ID 匹配/注册用户；不同在途 token 使用不同 User ID。stream ID 是本地动态分配的槽位，不等于 User ID。
- 释放需满足末任务完成，以及 Router 返回同用户的 RMEM 资源释放信号。

因此测试不扩大硬件表容量，也不以宿主线程数代表硬件并发。读取已有打拍后的 TS snapshot，每 16 cycle 采样全芯片计算核用户集合，记录全局和单 core 的在途峰值、观测到的 stream 槽位。全局 16 用户同时出现不自动代表任意一个 core 同时有 16 用户，报告分别列出这两项。

## 10 组设计

| 组名 | 16 个 token 的差异 | 请求注入间隔/cycle |
| --- | --- | ---: |
| `expert_bank_0` | 每包 8 个连续专家，覆盖 0–127；shared 轮换 | 1 |
| `expert_bank_1` | 每包 8 个连续专家，覆盖 128–255；shared 轮换 | 128 |
| `same_experts_contention` | 全部选择 `[0,1,43,86,129,172,215,255]`、shared EP3；不同输入争用同一批专家；前两包 payload 相同但 UID=3/1023 | 1 |
| `shared_permutation_overlap` | 6 包固定输入/TopK，分别选 shared EP0–5；6 包配套 TopK 逆序且系数随专家反转；4 包部分专家重叠 | 64 |
| `mask_sweep_0` | routed mask 0x01–0x10，随机合法专家和轮换 shared | 1 |
| `mask_sweep_1` | routed mask 0x11–0x20，随机合法专家和轮换 shared | 128 |
| `mask_sweep_2` | routed mask 0x21–0x30，随机合法专家和轮换 shared | 256 |
| `mask_sweep_3` | routed mask 0x31–0x3f，最后一包再取 0x3f，专家和输入独立 | 1 |
| `boundary_ring` | 单 token 设计的前 16 个边界输入：各 EP 首尾、前四个相邻 EP 分界；包含历史 `boundary_pair_15` 两份输入 | 128 |
| `numeric_isolation` | 8 种输入类型各 2 个种子；其中两包含零融合系数；另两包选择 EP4/5 分界和全局首尾专家 | 64 |

间隔控制的是输入源提交队列的最早时间，入口仍按有限 flit credit 发送。原生 `pcode sent token` 记录源提交；`pcode wire token ... head/tail` 记录实际首尾 flit 上链路；输出按 `(user_id, token_id)` 关联，不要求按发送顺序返回。

## 验收

- 每组恰好 16 个唯一正确身份的输出，每个为 6144 个 BF16 值。
- 逐包重建独立参考：非零参考余弦 ≥0.99999，相对 L2 ≤0.02；零参考要求实际严格全零。
- 逐包 fixture 独立解码，核对 8 个不同专家、TopK 系数、输入及 scale；记录并核对 `routed_mask`、`shared_ep`、`actual_ep_mask`。
- **要求观测到全部 16 个用户同时占用计算 TS 表，且发生在首个输出之前**；仅仅排队提交 16 包不算通过。
- 16 包均记录真实入口首尾 flit，满足提交→入口首→入口尾→对应输出的时间顺序；入口不交织不同包。
- 全部计算任务退休，归约任务及输入/输出队列清空，Router overflow、DTE exception、MU drain 均为 0；必须在 cycle 上限之前完成。
- 不采用双 token 驱动的自动换序/改间隔重试。功能失败和并发证据不足均直接报告失败，原始现场保留。

每组有模拟 cycle 上限和宿主超时。超时本身只能说明该轮未通过；需要结合原生任务、队列、归约和 Xbar 等诊断才能判断是否存在死锁。

## 运行和复核

```bash
# 使用默认 pcode/output 产物；也可用 --bundle / --catalog 显式选择配套产物。
bash test/bach/run_pcode_glm5_multi_token.sh --workers 2 --threads 4

# 选择一组；--resume 只复用身份和全部产物摘要一致的已通过组。
bash test/bach/run_pcode_glm5_multi_token.sh --filter same_experts_contention --workers 1 --threads 4

# 只生成设计清单，不启动模型或模拟器。
bash test/bach/run_pcode_glm5_multi_token.sh --plan-only

# 重新解码全部 fixture、重新计算全部参考、检查输出及原生证据。
bash test/bach/run_pcode_glm5_multi_token.sh --verify-only
```

`--verify-only` 要求所选集合全部通过。如果集合中出现原生断言或其他失败，用
`pcode_glm5_multi_token_audit.py`，传入相同的 `--bundle`、`--catalog`、`--out`、
`--model`、`--simulator`：它重新计算全部 160 份参考，核对全部失败记录与日志，
检查实际存在的输出，并将 `audit_passed`（记录和产物一致）与
`campaign_passed`（10 组功能全部通过）分别记录，不能混为一谈。

默认 `--max-cycles 80000`、`--timeout 2400` 秒；`--out` 默认 `build-ep6/glm5-16-token`。默认关闭 trace，不自动生成大波形。输出包括 `planned_cases.json`、`program_audit.json`、逐组 `report.json`/`native.log`、逐包参数/fixture/参考/实际输出、`summary.json`、`results.txt`，复核后生成 `verification.json`。只有 `total == planned == passed == 10` 且 `token_outputs == 160` 才是完整集合通过。

## 本次执行位置

当前工作区的本轮产物保存在两个仓库外的 `../temp/glm5-16token-20261021/results`；配套程序来自 `../temp/ep6-route3f-20261021/bundle/pcode_ep6_tp8`，catalog 为 `../temp/ep6-route3f-20261021/generated/route/route.resolved.json`。源码基于 BachFlow `e296b2ab7cc9401bd82ff19d98797572904c8571` 和 latch `830346074ccbda9feda052ed7b95d98548ae59cf`，本次未提交修改。最终结果以该目录的汇总及独立复核文件为准，不能把运行中的局部结果视作 10 组通过。

## 2026-10-21 本轮结论

10组原计划全部执行，严格结果7/10：另1个边界组的16份数值和硬件状态检查通过，但计算用户占用峰值14，未达到额外要求的16；2组触发归约顺序断言。即功能正确的组为8组，实际保存的128份完整输出全部通过独立复核；不是160/160通过。

最低非零余弦0.9999999519686039，最大相对L2为0.00030997499977063187，两份零参考严格全零。shared/TopK组另有11项对照通过，单core峰值16且覆盖全部16个stream槽位。原单token route_3f和双token boundary_pair_15兼容性回归通过；Python20项检查通过。本轮没有重新运行完整96+96集合。

两处只加日志的复现定位：cycle7941，chip29/physical core5、UID909，FC2 PathID9先占RMEM后又收到FC13 PathID2；cycle10154，chip31/physical core1、UID400，FC13 PathID2尚未收齐又收到FC2 PathID21。当前设计/配置与模型在这些负载下没有满足同用户跨阶段归约的顺序前提；尚不能只凭断言确定修改归属，不加入乱序重排或额外上下文绕过。

保持原计划结果不替换；边界组另用相同fixture/程序/模型把间隔128压到1做补充，16份输出仍正确，计算峰值15，未达额外16计算占用目标。没有发现运行至cycle上限的停滞；断言中止的两组不能用于证明无死锁。

工作区完整结果、失败分析和逐包证据见 `../temp/glm5-16token-20261021/TEST_RESULTS.md`。以上记录对应2026-10-21执行时的未提交工作区；本次整理将测试源码和说明纳入版本管理。完整输出、模型和trace仍保留在仓库外。


## 后续入口修改的范围

2026-10-22的单入口Router广播改动及两组64token验收见 [PCODE_GLM5_64_TOKEN.md](PCODE_GLM5_64_TOKEN.md)。本次整理没有重新执行完整10组16token集合，以上历史测试结果仍对应原冻结程序。
