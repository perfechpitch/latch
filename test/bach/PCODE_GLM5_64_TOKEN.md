# GLM5 EP6+TP8：10组 × 64 token 功能及死锁测试

每组在同一个 latch 实例中连续提交64个不同用户，不在组内复位芯片或重载程序。执行的是 BachFlow/pcode 导出的 BACHIR、DSA 配置及 RISC-V HEX，不调用 latch 内置 MoE 编译器。

配置沿用已确认版本：hidden=6144，intermediate=2048，256个 routed expert 按43/43/43/43/43/41部署，1个逻辑 shared expert 在6个EP复制，每token选择8个不同 routed expert，非负融合系数之和为1，shared系数1且只在 `UserID % 6` 执行。模型为固定种子20261012的完整伪随机FC13/FC2矩阵，预置全部768个内存镜像，权重正文9,890,168,832字节，另有scale。输入、专家和权重系数按用例变化，模型权重跨用例保持相同。

## 批次数量与物理并行槽位

64是提交批次长度。TS物理槽位仍为16，CM仍按16个stream窗口分配，所有入口、Router和TS反压保持。没有把STREAM_NUM、RM容量或credit改为64。B/R-core软件缓冲环沿用pcode的R_CORE_STREAM_NUM=1024，本批仅64份输入；此值与计算TS的16槽不同。飞书软件流revision14130还定义GPU预授权额度不得超过MM token pool，本轮只测合法64包批次的pcode→latch链路，不覆盖GPU/DPU的外部refill协议。

[飞书TS设计](https://pcng0ddyhlxs.feishu.cn/wiki/VzcMwirkEivgG5knckxcTe7Cnig) revision18689：初始化条款 `YICddJrBAootCoxrAMhc2tVynMb` 为stream_num=1–16；`UnEpdvYVRoRDC2xmn3ock49Unyh` 为最多16用户的FIFO；`IZo5dkEYBoOkfHxccosc2NtKnAc` 规定退休释放并通知上游credit；`SOdZdhpLtoNMAXxEl6ucyUzFnah` 规定DataIn表忙时反压Router。原始读取响应保存在本轮temp目录 `feishu-ts.json`。

数值、完整输出和排空决定功能PASS；全局/单core用户峰值、参与重叠用户和16个槽位使用掩码分别记录，不要求同一时刻全局驻留64个用户。未达到某个并发峰值不单独判功能失败。

## 10组

| 名称 | 每组64个token的差异 | 请求注入间隔/cycle |
|---|---|---:|
| expert_bank_forward | 按8专家连续块正序，全部256专家覆盖两轮；shared轮换 | 1 |
| expert_bank_reverse | 专家块和块内排列均逆序，全部256专家覆盖两轮；shared反向轮换 | 64 |
| same_experts_hotspot | 固定同一批8专家、shared EP3；64个不同用户；前两份payload相同、UID3/1023 | 1 |
| shared_permutation | 五批“六个shared位置＋六个匹配TopK逆序”；另四份部分专家重叠输入 | 64 |
| mask_sweep_forward | routed mask 0x01–0x3f，末包再取0x3f；专家、shared和输入变化 | 1 |
| mask_sweep_reverse | routed mask 0x3f–0x01，末包再取0x01，shared反向轮换 | 128 |
| boundary_ring | 循环18个边界设计，覆盖各EP首尾、相邻EP分界、全局首尾；后续轮改变输入种子 | 128 |
| concentrated_ep_round_robin | 每包8个routed及shared均位于同一EP，EP5–EP0轮换，系数均匀 | 1 |
| numeric_isolation | 8种输入类型各8个种子，全EP路由，均匀/递增/部分零系数 | 64 |
| alternating_masks | 近/远、隔空、交错、全EP mask交替；shared在路由集合内外；含one-hot系数 | 1 |

注入间隔约束源提交的最早时刻，实际flit依赖入口credit，不能用提交时间代替真实进入时间。TokenID限定为60000–60639（16位范围）。全组使用64个合法且不同的UserID，避免把同UID重用问题混入本轮。

## 验收与失败解释

- 恰好64个正确 `(UserID, TokenID)` 的唯一输出，每个为6144个BF16值；允许乱序返回。
- 逐token独立参考：非零参考余弦≥0.99999、相对L2≤0.02；零参考要求严格全零。
- 独立解码fixture，核对8个不同专家、输入/scale、融合系数；每例保存routed mask、shared执行EP、actual EP mask。
- 64份输入均有实际首尾flit记录，满足提交→首flit→尾flit→对应输出，入口包不交织。
- 计算TS退休、归约任务及归约输入/输出队列清空；Router overflow、DTE异常、MU drain均为0；在cycle上限前正常结束。
- 不进行自动换序、减并发、改间隔或重试以替换失败结果。不修改pcode任务或RM硬件逻辑绕过已知阶段顺序问题。
- 归约断言属于执行失败，不能据此证明数值正确或无死锁。cycle/宿主超时仅作为失败，需要结合任务和队列诊断再确认死锁。
- 原生断言提前终止时，尚未到达入口的token和未产生的输出不能计作已验证；报告分别记录计划数、源提交数、入口首尾数、保存并校验的输出数。

## 运行

```bash
# 全部10组；24GB机器建议一个实例、4线程
bash test/bach/run_pcode_glm5_64_token.sh --workers 1 --threads 4
# 指定一组
bash test/bach/run_pcode_glm5_64_token.sh --filter boundary_ring
# 只输出设计，不运行模型或模拟器
bash test/bach/run_pcode_glm5_64_token.sh --plan-only
# 全部完成后，独立重建640份fixture/参考，核对失败记录和存在的输出
bash test/bach/run_pcode_glm5_64_token.sh --audit-only --out /absolute/path/to/results
```

可传 `--bundle`、`--catalog`、`--model`、`--simulator`、`--out` 指定同一套产物。默认cycle上限160000、每组宿主超时3600秒。并发workers范围1–2，TS槽位与workers无关。默认关闭trace，不生成新模型副本。

本轮执行位置：仓库外 `../temp/glm5-64token-20261021/results`。配套冻结程序 `../temp/ep6-route3f-20261021/bundle/pcode_ep6_tp8`，catalog `../temp/ep6-route3f-20261021/generated/route/route.resolved.json`，模型 `build-ep6/glm5-dense-model`。BachFlow HEAD为e296b2ab7cc9401bd82ff19d98797572904c8571，latch HEAD为830346074ccbda9feda052ed7b95d98548ae59cf，工作区包含16-token测试框架及本轮64-token扩展。执行时归档源码并记录SHA256；该次2026-10-21测试执行时尚未提交或push。

结果以该目录 `summary.json`、`results.txt`、逐组 `native.log`/`report.json` 和复核后的 `audit.json` 为准。只有10组全部通过且640个实际输出全部通过，才能声称完整集合通过。`audit_passed`表示复核记录一致，不等于`campaign_passed`。

### 本轮实际cycle预算

首组用160000cycle长时间观察；确认其21000cycle后输出不再增加后，剩余组先观察40000cycle。若输出在35000cycle以后仍有进展，保留初次运行目录，再用相同输入、间隔、模型和程序扩展到160000cycle；不以改变输入或降低并发替换失败。实际预算记录在每组report.json的maximum_cycles中；宿主timeout不变。此策略用于区分计算仍在前进与长期停滞，不以cycle上限单独证明死锁。

转存统计 `Xbar::OverflowCount` 在当前latch中按改送进CM的flit累加，不直接等于丢包。当前原生harness仍保留旧回归的零转存断言，因此如出现“64个输出与数值均正确、仅零转存断言失败”的情形，须单列为测试判据不适配，不能自动认定功能缺陷；需要检查转存重发完成及状态排空。暂不更改运行中的冻结harness或硬件行为。


## 2026-10-22：单入口 Router 广播配套与两组验收

配套 BachFlow 将输入改为：单个片外入口 → Router 广播树 → 12个B-core，所有B-core（含入口处）按 `CoreMask` 过滤；每核独立DATAIN接收，普通任务链为 `check_flag → 行内广播 END`。
BACHIR新增注释 `# PCIE_INGRESS <chip> <physical_router_core> <path_id>`；本驱动读取该值，从单个片外端口注入每个token一份包，复制由Router完成。旧bundle缺该注释时仍使用原来的Path63。
本轮配套入口为chip0/W、物理Router5、Path0；未修改Router、TS、DTE或RM硬件模型行为。

仅重跑原先两组转存停滞用例，各64token。128份fixture与原测试逐字节一致，仍使用原完整固定模型的768份镜像。

| 组名 | 原测试 | 新入口程序结果 |
|---|---|---|
| concentrated_ep_round_robin | 停在33/64 | **PASS，64/64，18204 cycle**；数值、唯一身份、计算TS退休、RM任务及输入/输出队列排空检查通过 |
| expert_bank_forward | 停在32/64 | **FAIL，尚无最终输出**；9778 cycle触发归约配置冲突 |

通过组最小余弦为0.9999999811132185，最大相对L2为0.00019435597658037647，满足已确认阈值。
失败定位为UID132/token60054，chip5/物理core5：远端FC2部分和Path9（3072 B）先占RM，随后本核FC13部分和Path2（18432 B）到达，触发原有 `SameNet` 断言。
该用户routed mask为0x10、shared EP0、实际mask为0x11，EP0正常激活；属于跨阶段归约顺序冲突。
诊断只在仓库外的影子头文件增加错误日志，保留断言和状态逻辑，与正式失败运行的全部pcode事件一致。

此结果不能表述为两组均通过，也不能表述为10组64token集合已回归通过。下一步需处理FC2输入的阶段隔离。
本轮证据位于仓库外 `../temp/ddr-bcore-broadcast-20261022/`：`ACCEPTANCE.md`、`acceptance.json`、`forward_conflict.json`、两组逐token报告及原生日志。
这里只提交源代码、测试定义和说明；生成的trace、模型权重、fixture、期望/实际输出及编译产物留在本地。
