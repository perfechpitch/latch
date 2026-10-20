# GLM5 EP6+TP8：保留 planner VC 与 Reduce RIU 接口验证

> 实验日期：2026-10-19–2026-10-20（Asia/Shanghai）。最终 C3 fresh 回归于本地10月19日20:58:47开始，独立审计于10月20日01:51:34完成（UTC `2026-10-19T17:51:34.634716+00:00`）。96个单token、96组双token及全部288参考独立审计通过。实验及审计期间未提交或push；随后按用户要求整理提交源码、测试与本报告，排除大trace、权重和build产物。未修改飞书设计文档、历史修改说明。下文HEAD和“未提交”描述均指实验时快照。

## 1. 验证问题与范围

取消 pcode 导出器的网络 VC 强制策略，使输出保留 route planner 原始 VC；将 latch 中 Reduce 与 Router 之间的物理单 VC 接口，与网络上的目标 VC 分开。先检查 `boundary_pair_15`，再重新运行完整 96 个单 token 用例、96 组双 token 用例，独立重算全部 288 份输出。

本轮使用此前已经通过动态 TYPE_VL 回归的 pcode 生成程序。工作区 BachFlow HEAD 为 `2834213b22dc725b102033734aa309f108bf53fb`，但没有用该提交新增性能修改重新生成算子，因此本文不能证明其新性能程序已经通过回归。latch 工作区 HEAD 为 `138a9f1f764a28250285182d128795777ed3f9b5`，实验修改均在其上保持未提交。

旧动态 VL 审计（2026-10-15）记录当时生成侧 base_commit 为 BachFlow `6889dd868fbac79a3605b8c6ae110c1fb3e9e734`、latch `774690623ef131f67526672387b8340a8d2be057`，动态 VL 改动当时尚未提交；之后的提交状态不替代生成产物身份。本轮以相同 catalog SHA、855 份实际引用 HEX、DSA 字段和所选 harvest 的 1278 个 C++/regs 文件快照明确固定程序，相关 SHA 存于实验目录。

模型配置为 dim=6144、moe_inter_dim=2048、256 个 routed expert、1 个 shared expert、topK=8。EP0–EP4 各 43 个 routed expert，EP5 为 41 个；shared 在六个 EP 上相同部署，单 token 只由 `user_id % 6` 选中的 EP 执行一次。权重是固定完整伪随机 FC13/FC2 矩阵，每次预置全部部署专家的 768 个 MatrixMem 镜像；它不是实际模型检查点。

## 2. 设计依据：只引用具体接口条款

下表使用同次线上读取缓存 `temp/feishu-vc-source-check-20261019`，可按 block ID 与 revision 核对原文。本文没有替硬件设计补充未明确规定的能力。

| 来源 | 原文位置及含义 | 对本实现的约束 |
| --- | --- | --- |
| DATA_NOC_DE_HAS，revision 21458 | [Reduce/Router 接口](https://pcng0ddyhlxs.feishu.cn/wiki/TMGwwlKBPiDWi8kWBi9crCPmnuc#Fq1Ld8Hsjo5nR1xibXNcYUCinBc)：Router→Reduce 多 VC 映到单 VC0；Reduce→Router 单 VC0，RIU 输入级 routing table 实现 targetVC 重映射 | 单 VC0 是物理接口，并不意味着网络归约包必须走 VC3 |
| 同文 | [Reduce 输出](https://pcng0ddyhlxs.feishu.cn/wiki/TMGwwlKBPiDWi8kWBi9crCPmnuc#doxcnVUOqeNcWbruQRnN8UXYi2g)：每个 Reduce 输出 port 为单 VC0，V_REDUCE=1，无多 VC 交织 | 物理入口只有一个 FIFO，不能按目标 VC 额外增加并行队列 |
| 同文 | [顺序规则](https://pcng0ddyhlxs.feishu.cn/wiki/TMGwwlKBPiDWi8kWBi9crCPmnuc#OnnbdZ3sgocSyfxMy6dcychXnjf)：同 VC FIFO，不重排；跨 VC/跨 input 不保证顺序 | 不能让同一物理 FIFO 后面的请求越过被反压的头部 |
| 同文 | [死锁职责](https://pcng0ddyhlxs.feishu.cn/wiki/TMGwwlKBPiDWi8kWBi9crCPmnuc#AeB2dFNcUo3VoSxmANEcd8cNnPb)：软件保证单 VC 无物理依赖环，硬件不检测死锁 | 用例通过不能推广成任意路由拓扑的无死锁证明 |
| Router，revision 13330 | [整包服务](https://pcng0ddyhlxs.feishu.cn/wiki/WgF3w89d1itolgkLfF8cKtyZnOc#Ivv9d9sOVov9yExaV0Pc0tM0nfg)：Core/Rmem 首到尾整包服务，禁止交织；[上游顺序责任](https://pcng0ddyhlxs.feishu.cn/wiki/WgF3w89d1itolgkLfF8cKtyZnOc#J9JddRkvBopGBVxZq3Fc7LCwnbc)：上游/TS 负责跨输入 task/user 顺序 | 保留既有 Core/Reduce lane 跨 VC 整包锁 |
| DTE 详设，revision 26604 | [当前基线](https://pcng0ddyhlxs.feishu.cn/docx/MSIUdrrjZoKi8uxw6o8cjmSqnxe#doxcnA4Wie9WLr60xMWXdDK3e5f)：同 VC NoInterleave，跨 VC 可 flit 交织 | 不引入后续 P2 同 VC 交织能力 |

文档中还存在 Rmem 多 VC 的摘要以及附录交织讨论。本文只实现上表具体接口条款，不自行裁定这些段落的优先级。本次读取没有发现“归约独占网络 VC3”或“普通 VC3 必须改 VC0”的规定。

## 3. 代码修改

### 3.1 BachFlow：导出时保留 planner VC

文件：`pcode/src/bachflow_pcode/latch_export.py`。

```python
# 之前
out.vc = REDUCE_VC if out.op_type == OPTYPE_REDUCE else planner_vc % REDUCE_VC
# 实验版本，仍检查 planner_vc 在 [0, 3]
out.vc = planner_vc
```

同时删除两处在意图恢复时再次写死 VC3 的赋值：中继传输恢复成归约、局部双输入归约恢复。合成的归约透传继续继承上游 VC。RTAB 与 TS 查询 credit 的 TSRTAB 都取相同最终路由信息。

**此实验不是 `e2e78fd` 的精确逆向差异。** `e2e78fd` 引入普通 VC3→VC0；更早代码已强制归约 VC3。本轮为保留全部 planner 原 VC，同时移除了这两类策略。另建“仅撤销 e2 普通重映射”的辅助 bundle，避免混淆两项改变。

| RTAB 类型 | VC 变化 | 记录数 |
| --- | --- | ---: |
| 原始归约 | 3→0 / 3→1 / 3→2 | 785 / 3 / 4 |
| 原始普通传输 | 0→3 | 226 |
| 合成透传，继承普通传输 | 0→3 | 27 |
| 合成透传，继承归约 | 3→0 | 108 |
| 总计 | | 1153 |

共比较 4514 条 RTAB；1057 条 TSRTAB 同步改变。4029 条有效原始 planner 路由的全部 6 个导出 VC 字段逐项匹配 `nxtVC`。实际引用的 855 个 RV HEX 逐字一致；DSA 配置、所有非 VC 字段与其他 IR 记录一致。核对明细位于 `temp/riu-vc-20261019/bundle_vc_audit.json`。

### 3.2 latch：物理 FIFO 与网络 VC 分开

初始接口/RIU候选修改以下四个执行头文件；最终C3还修改 `router_ports.h`、`router_station.h`，并补齐 `xbar.h` 的source VC队列与包锁，合计六个执行头文件。后两阶段的原因及代码见§5.1–5.3。

| 文件 | 修改及目的 |
| --- | --- |
| `src/bach/ip/chip/core/router/router_table.h` | 新增 `kReduceInterfaceVc=0`；保留 `kReduceVc=3` 作为旧编译器默认值，而非硬件保留通道 |
| `.../reduce_module.h` | Reduce 输出请求 VC 与 room 查询均使用接口 VC0；入口检查收到的 VC 为 0；归约仍边接收边输出 |
| `.../xbar.h` | 发往 Reduce 的线统一输出 VC0；Reduce 输入按 path 查 RIU 目标 VC，用该目标申请网络 credit/仲裁，但只排入物理 FIFO0、只发布 FIFO0 的 room |
| `.../router.h` | 为实际 Router 的 Xbar 连接 Reduce 路由表，沿用既有查表副本 4 |

关键分离如下：

```text
Reduce ──物理 VC0──> FIFO0 ──RIU(path)──> target VC 0/1/2/3 ──> 网络
                             │
                    不新建目标 VC 的物理 FIFO
```

没有放松同 VC 包锁或 Core/Reduce lane 整包锁，没有等完整归约包缓存完再发，没有扩大 credit、队列深度、阈值，也没有删除生命周期或归约队列检查。

当前 Xbar 请求只携带一个网络 VC：如同一 Reduce 多播请求的不同网络出口要求不同 `nxt_vc`，实验版本明确报错。此限制必须保留在评估中，不能宣称覆盖所有 RIU 多播配置。RIU 查表集成在本模拟器现有 `Collect` 阶段，没有凭空补充文档未说明的额外时钟级；因此这里验证功能映射，不声称已标定 RTL 精确时序。

### 3.3 有意义的局部验证

- exporter 7 个测试：普通/归约保留 VC0/1/2/3、恢复意图保留 VC、非法 VC 拒绝。
- 最终Router共8个测试可执行文件、104个测试全部通过（初始接口候选100项，source VC队列增加3项，最终包锁反例增加1项）。新增检查：接口输出 VC0、RIU 映到全部 4 个网络 VC、目标 VC 反压只消耗正确 credit、FIFO0 满时不能误用其他 VC room、不同目标 VC 的队头阻塞仍保序；已有 Core/Reduce 整包锁继续验证。
- Python 用例驱动 15 个测试通过。
- 当前 HEAD283 加 exporter 修改的完整 pcode 测试为 76 passed / 14 skipped（主 agent 复核执行）；它属于生成侧检查，不代表本轮 native 使用了 HEAD283 新性能算子。
- 新增 `test/bach/pcode_riu_vc_verify.py`，显式传入 BachFlow 根路径，独立验证本次实际 bundle、模型与代码身份；对 alignment-only 失败重试单独审核唯一失败条件，最终成功记录仍要求全部 native 状态检查通过。

## 4. boundary_pair_15 对照

此用例的两个 token：routed mask 分别 `0x18` / `0x04`，shared EP 为 0 / 3，实际 EP mask 为 `0x19` / `0x0c`。同一完整模型与输入 fixture。

| 生成程序/VC | 模拟器 | 结果 | 两包输出 cycle |
| --- | --- | --- | --- |
| 当前动态 VL 冻结程序，全部原 planner VC | 修复前 | PASS | 8547 / 8903 |
| 同一程序，全部原 planner VC | 接口/RIU 修复后 | PASS | 8548 / 8904 |
| 同一程序，仅撤销 e2 普通重映射、归约仍强制 3 | 修复前 | PASS | 8547 / 8903 |
| 历史 before-transfer-vc-fix 程序及历史模拟器 | 历史版本 | FAIL，16000 cycle 输出 0 | 无 |
| 历史静态 VL 程序，仅恢复原 planner VC | 接口/RIU 修复后（075） | PASS | 8612 / 8888 |
| 同一历史原 planner VC 程序 | 归档历史模拟器（8efe） | FAIL，16000 cycle 输出 0 | 无 |
| 同一历史原 planner VC 程序 | 本轮直接前态模拟器（cfa） | FAIL，16000 cycle 输出 0 | 无 |

**前三项均通过，当前冻结动态 VL 程序没有复现历史死锁。** 不能将它描述成“先成功复现了死锁，再修复”。历史静态 VL 快照已复现死锁：chip23/core2 中 user774/path33 的普通 VC3 包排队，output2/VC3 被 Reduce 输入持锁，同时仍有 private/shared credit 20/20；user195/path2 归约 done=1、两路输入仅收到 18432/1024，最后没有输出。它与当前动态 VL 程序不同，不能混为同一程序的前后对照。进一步辅助对照先确认历史 4514 条 RTAB、4226 条 TSRTAB 的所有非 VC 字段与本轮 catalog 导出结果完全一致，再只恢复 900 条 RTAB、804 条 TSRTAB 的 VC；855 份引用 HEX、DSAREG、TCHAIN 和其余记录均保持历史原样。该辅助对照使用新 RIU binary，单独保存日志。另用完全相同历史原 VC 程序分别运行归档旧模拟器和本轮直接前态：8efe→075 之间还含更早既有修改，cfa→075 才隔离本轮 RIU 修改；直接前态 cfa 最终也在 16000 cycle 输出 0，呈现相同 chip23/core2 包锁与归约等待。因而，在完全相同历史原 VC 程序下，cfa→075 的本轮接口/RIU 修改使该输入从失败变为完成；这是本轮修正的直接对照证据。它仍不构成任意流水顺序无死锁的证明。三项当前程序中两份输出余弦分别为 0.999999997476549 / 0.9999999994306217；同拍在途 TS compute UID 重叠 2415 cycle。宿主并行负载不同，墙钟耗时不用于性能比较。

代码层面，旧 Reduce 输出直接设置 `req->vc = 3`，并查询 VC3 的 room；它虽然读取 path 表来决定方向，却没有按该表选择网络 VC。仅恢复导出 RTAB 不能使这个旧接口遵从 planner 的 VC0/1/2。新实现先使用物理 VC0，再在 Xbar 的 RIU 边界查 `nxt_vc[out]`，才将网络请求映到目标 VC。因此“某个当前程序在旧模拟器上数值通过”并不能代替目标 VC 接口检查；局部 0/1/2/3 映射与反压测试对此给出直接验证。上述历史固定程序的 cfa/075 对照进一步证明本轮修改能解除这个已复现输入的等待。

修复前 binary SHA256：`cfa0c4b7c3e42cc4259667095745c0851d1de74187a50415d7cf8f0422599266`。
修复后：`075c9391d666f5b6aef022a163ccef69196332ae0b8f0ab2ba992ff90826c36d`。
历史 binary：`8efe4539a0bd2b04a7ee82c5d14a70cc5eb5c58ee3cdb8cbc0d2306a372c237e`。

## 5. 全量与独立审计结果

候选3前置全部通过后，在以下新目录完成fresh回归，未复用旧native成功结果：

- 最终候选单 token：`build-ep6/riu-vc-source-owner-single-96`
- 最终候选双 token：`build-ep6/riu-vc-source-owner-two-token-96`
- 最终候选日志：根目录 `temp/riu-vc-20261019/candidate3/single-96.log`、`two-token-96.log`
- 原075候选目录 `riu-vc-single-96`、`riu-vc-two-token-96` 仅保留反例证据，不混入最终候选通过数。

原075候选已暴露真实失败并被后续候选替代，反例、停止原因及完整执行清单见下文。

最终状态 `passed`、`audit_completed=true`；两份native driver均返回0，独立审计也返回0。所有288份参考均重新从固定模型和输入计算，检查输出身份、mask、数值、生命周期及程序来源，未使用先前候选的成功结果替代本轮输出。

| 验收项 | 单token | 双token |
| --- | ---: | ---: |
| 用例 | 96/96 PASS | 96/96组 PASS |
| 输出 | 96/96 | 192/192 |
| 非零参考最小余弦 | 0.9999999777156279 | 0.9999999513380896 |
| 最大相对L2 | 0.0002111749947904292 | 0.00031204244883656005 |
| 零参考，实际严格全零 | 2/2 | 5/5 |
| BF16逐字相同输出（含零） | 8 | 18 |
| 输出缺失/非法 | 0/0 | 0/0 |

验收沿用cosine≥0.99999、相对L2≤0.02，零参考要求实际严格全零。全部输出唯一、身份正确、6144个BF16；任务退休、归约队列清空、无overflow/drain遗留。96组双token均有两个不同UID在TS compute槽同拍在途的证据，不扩大解释为双ALU同时busy或5MTPS。双例中的32份全专家token输出与本轮对应32个单例输出逐字一致；数值正反序、相同payload、TopK排列、shared副本等关系检查也通过。

| 单token分类 | 数量 | 覆盖 |
| --- | ---: | --- |
| 全专家 | 32 | 每例8个连续全局专家，合计全部256个 |
| routed分布 | 18 | 集中、相邻、隔空、交错、全EP的代表mask，不遍历全部63种 |
| 编号边界 | 18 | 各EP首尾、相邻EP分界、全局首尾 |
| 数值 | 16 | 8类输入各2例 |
| TopK排列 | 6 | 3组forward/reverse |
| shared副本 | 6 | 六个执行EP的副本一致性 |

| 双token分类 | 组数 | 覆盖 |
| --- | ---: | --- |
| mask/shared | 36 | 两token的shared执行EP有序对(a,b)完整6×6；这72个token合计覆盖全部63个非空routed mask，并非单token的routed EP×shared EP完整交叉 |
| 全专家 | 16 | 每组2token各8专家，合计全部256个 |
| 编号边界 | 18 | 两token专家与EP编号边界 |
| 数值 | 16 | 8类输入的forward/reverse |
| 争用/隔离 | 10 | 相同/部分重叠专家、相同payload、EP0↔EP5、mask/shared差异、TopK排列、零系数、零输入隔离 |

`boundary_pair_01`只有并发对齐需重试：最初gap1、同序gap2048/4096均已获得正确的两份数值输出并通过状态检查，但TS compute不重叠；最终原既定策略反序 `(1,0)`、gap2048后通过，overlap_cycle=3403。三次alignment-only失败的日志、返回码和唯一断言均逐一审计，不删除失败尝试，也不以重试掩盖数值/状态失败。其他95组无需对齐重试。

15项native辅助对照均完成独立身份与日志核对，其中10项PASS、5项预期负对照FAIL：历史原程序+历史模拟器、历史raw VC程序+历史模拟器、历史raw VC程序+本轮直接前态、C1 raw expert14、C1精确e2逆向expert14。它们是保留的真实失败证据，不写成15项native全部通过。source VC与包锁的局部负对照同样保留。

审计结果：[JSON](pcode_riu_vc_results.json)、[逐例摘要](pcode_riu_vc_results.txt)。JSON记录全部288个输出的routed mask/shared EP/实际mask、SHA、余弦/L2及native状态，也记录524份冻结执行源、工作树6头文件一致性、保护3文件、855HEX、4029原始planner VC、768模型镜像与33536专家分片等来源核对。

### 5.1 首个候选的反例与隔离队列验证

`expert_blocks_14` 的独立 16000 cycle 诊断正式失败，输出为 0；旧重映射同输入在 8406 cycle 完成。旧用例、本轮主回归、诊断的两份 fixture SHA256 完全一致。主回归仍保留原 50000 cycle 上限，诊断不替代其结果。

诊断在 chip47/physical core2 显示：user64/path34 普通包需要 output2/VC0，而该 VC 由 Reduce 输入持锁；private/shared credit 仍为 20/20。user503/path2 的归约已经收到 18432/1280/0 B，后续输入不足以完成结果尾 flit。core1 的同一归约已收齐，但下游 VC0 反压。所有 Reduce 输入 FIFO 为空，**该证据不支持将 Ctx 的跨 lane 锁认定为本例根因，本轮未修改该锁**。

进一步检查发现，模拟器在 RouterStation→Xbar 边界以 next-hop VC 查询 room、将 flit 放入按 target VC 划分的中间 FIFO。不同原始输入 VC 只要映射成相同 target VC，就会提前合队；反之同一输入 VC 的不同 target 包又可能进入不同 FIFO。该机制可能制造原输入 VC 之间的额外队头阻塞，也可能允许同原始 VC 越序。Feishu Router rev13330 [U4Pod2x7NohBcRxXV9wcWROOn4d](https://pcng0ddyhlxs.feishu.cn/wiki/WgF3w89d1itolgkLfF8cKtyZnOc#U4Pod2x7NohBcRxXV9wcWROOn4d) 规定输入按接口携带的 VC 进入对应 FIFO，[ODPFdzvU9oNTuLxPq3ncA7xGnec](https://pcng0ddyhlxs.feishu.cn/wiki/WgF3w89d1itolgkLfF8cKtyZnOc#ODPFdzvU9oNTuLxPq3ncA7xGnec) 规定阻塞时可服务其他已就绪 VC，同 VC 后包不得越过。这里的中间 FIFO 修改是为满足这些行为约束的实现方式；不声称文档规定了这一级 FIFO 的具体 RTL 结构。

在 `temp/riu-vc-20261019/candidate2/source` 建立隔离候选，将 source/input VC 与 network target VC 分开：输入 room/FIFO 使用 source VC，VA/SA 和下一跳 credit 使用 target VC。原来的每输入 FIFO、输出同 VC 整包锁、物理 Reduce lane 整包锁、容量和 credit 数保持原样。局部两例以旧合队机制运行均失败；仅换队列索引机制后均通过：①sourceVC3→targetVC0 阻塞时，sourceVC0→同 targetVC0 的其他出口仍能推进；②同 sourceVC3 的后包即使 targetVC 不同也不能越过阻塞头。负面对照与新候选日志分别保存在 `candidate2/unit_legacy_sourcevc.log`、`unit_candidate_sourcevc.log`。原075 binary与进行中的主回归未被替换。

此后用相同原planner bundle/fixture完成expert14与两种boundary15的候选2、候选3前置检查，并最终执行候选3的fresh96+96。另一个独立“只删除e2普通VC3→0、保留旧reduce固定3”的075对照也在16000cycle输出0（见§5.2）；该结果用于区分两种删改，不能直接认定e2策略是硬件要求。

### 5.2 候选2：source VC 与 target VC 分开

候选2 binary SHA256 为 `aa4722d2c2590ef183b832e42ad2349992226d1bbfc9ab86daaa76730982a4c3`。同原 planner bundle 和 fixture 的最小检查全部通过：expert14=8392 cycle，当前冻结 dynamic VL boundary15=8905 cycle，历史 static VL 原 VC boundary15=8889 cycle。相比之下，075 +“只撤销 e2 普通 VC3→0、保留旧 reduce 固定3”的 expert14 对照在 16000 cycle 输出0，失败保留。因此不能把075反例只归因于删除更早的归约固定3，也不能据此断言 e2 策略本身是硬件要求。

`router_ports.h` 增加原输入 VC 元数据；`router_station.h` 按原输入 VC 查询中间 FIFO 的 room；`xbar.h` 按原输入 VC 入队，保留 network target VC 用于输出 VC 所有权、VA/SA 与下一跳 credit。转存同时保留原输入 VC，避免重发顺序被 target VC 替换。新增3项局部测试分别检查跨 source 不串扰、同 source 不越序、转存保留 source；8个Router程序合计103条通过。局部测试增加不会改变已冻结的 native binary。

这些精确测试过的4个文件（3个头文件、1个单元测试）已从隔离目录同步到工作树。其余本轮RIU修改沿用，未改 protected 的3个BachFlow文件，未commit/push。原075单 driver 按“候选已被替代”停止：70例完成PASS，2例中断，24例未开始；双 driver 自然退出：54组完成PASS、1组真实FAIL、41组未开始。双 driver 原summary只收录52PASS+1FAIL；额外两个已运行 future 的PASS报告另存，完整产物/SHA与分类在 `temp/riu-vc-20261019/candidate1_case_index.json`。中断与未开始既不算PASS也不算功能FAIL；真实expert14失败不删除、不重跑掩盖。

候选2曾在 `riu-vc-source-queue-{single,two-token}-96` 启动 fresh 回归，随后因下述包锁局部反例被停止。保存清单 `candidate2_case_index.json`：单1PASS、2中断、93未开始；双2中断、94未开始。这些结果均不计入后续候选通过数。实际模拟程序仍是本文冻结程序，不能等价为HEAD283性能代码的新生成结果。

### 5.3 候选3：包锁同时区分输入端口和 source VC

新分队后还必须保持原本由“按target合队”隐含保证的包锁。`vc_owner` 原来只记输入端口，Station 的 `next_vc_owner` 又在 tail 入中间FIFO时释放；当同一input的sourceVC3包A映到networkVC0并多播到mid/right，A的tail虽然已入队，却可能因mid反压而尚未发出。此时sourceVC0的包B也映到right/VC0，旧OwnerOk把它误认成同一所有者，允许B插在A头尾之间。

局部负对照实际输出 user顺序 `{100,200,100}`，应为 `{100,100,200}`，见 `candidate3/unit_owner_before.log`；不是推测故障。依据仍是同输出VC的Packet不交织和Core/Reduce物理口整包服务。候选3保持已有 `PacketOwner` 输入端口诊断含义，另记owner的source VC：只有input端口与source VC都匹配才能续发；到tail**实际发出**才释放。原Core/Reduce跨网络VC物理口锁保持不变。没有增加buffer/credit，也没有放松包边界。修后负对照变为PASS，103+新反例共104条Router测试全部通过。

候选3 binary SHA256：`6745764fd672df7dad3abef0634a096089e966d4ab8f1ce49fdd8fd8ac09fc71`。执行源已在 `candidate3/source` 冻结，三个最小检查全部PASS：expert14=8392、dynamic boundary15=8905、historical raw boundary15=8889 cycle。精确2文件已同步工作树，6个执行头文件与冻结源逐字一致，保护3文件不变；已在独立 `source-owner` 目录完成fresh96+96及288参考审计。源program与模型不变，075与aa的旧PASS均不计入本次验收。候选2完整中断事件与进程清单见 `candidate2/superseded_stop_event.json`，已有产物不删除。

## 6. 本轮实际运行入口

从 `latch-ep6-single-token` 执行。**不要直接使用默认 bundle 来复现本轮策略**：canonical `pcode/output/latch_bundle/pcode_ep6_tp8` 保留旧 VC 重映射产物；本轮显式使用实验重导出 bundle，避免覆盖历史证据。

```sh
PY=../BachFlow-ep6-single-token/.venv/bin/python
BUNDLE=../temp/riu-vc-20261019/planner_bundle/pcode_ep6_tp8
CATALOG=../temp/riu-vc-20261019/generated_snapshot/route/route.resolved.json
SIM=../temp/riu-vc-20261019/candidate3/simulator

# 单例复现；删除 --filter 即为 96 例
"$PY" test/bach/pcode_glm5_two_token.py \
  --bundle "$BUNDLE" --catalog "$CATALOG" --simulator "$SIM" \
  --threads 1 --workers 1 --filter boundary_pair_15 \
  --out build-ep6/riu-vc-reproduce-boundary15

"$PY" test/bach/pcode_glm5_dense.py \
  --bundle "$BUNDLE" --catalog "$CATALOG" --simulator "$SIM" \
  --threads 1 --workers 2 --out build-ep6/riu-vc-reproduce-single-96

"$PY" test/bach/pcode_glm5_two_token.py \
  --bundle "$BUNDLE" --catalog "$CATALOG" --simulator "$SIM" \
  --threads 1 --workers 2 --out build-ep6/riu-vc-reproduce-two-token-96
```

独立复核本轮已生成结果（不重新执行 native）：

```sh
"$PY" test/bach/pcode_riu_vc_verify.py \
  --pcode-root ../BachFlow-ep6-single-token \
  --experiment-root ../temp/riu-vc-20261019 \
  --bundle "$BUNDLE" --catalog "$CATALOG" --simulator "$SIM" \
  --candidate-source ../temp/riu-vc-20261019/candidate3/source \
  --single build-ep6/riu-vc-source-owner-single-96 --two build-ep6/riu-vc-source-owner-two-token-96 \
  --model build-ep6/glm5-dense-model \
  --report test/bach/pcode_riu_vc_results.json
```

选择新 `--out` 以保留旧实验结果。权重继续读取既有 `build-ep6/glm5-dense-model`，不复制大矩阵、不安装全局 Python 依赖。

## 7. 删除重映射的结论与合并边界

**在本文冻结的pcode执行程序、完整合成模型、EP6+TP8路由和已覆盖两token组合内，可以删除e2e78fd的普通VC3→0策略；本轮更进一步恢复了更早被固定为VC3的归约原planner VC，也通过fresh96单+96双及独立288参考审计。** 这必须与latch的物理Reduce VC0/RIU映射、source VC保序队列、精确到source VC且持续到tail实际发出的包锁一起评审，不能只改导出器一行就假定其他模拟器版本也成立。

本轮支持去掉上述导出侧适配，不构成所有网络拓扑、长期流水或任意任务顺序的无死锁证明。合并时应单独审阅物理接口/RIU映射和planner依赖图；不同出口使用不同target VC的Reduce多播仍受本实现限制。后续若要覆盖HEAD283新性能程序，需要另行生成和回归，不能使用本轮通过数替代。现有canonical bundle仍保留旧策略，复现必须使用§6显式路径；本地实验快照、native日志及大模型不随源码提交，完整独立审计需要保留这些本地产物。提交范围仅为源码、测试、审计脚本、本报告及JSON/TXT结果摘要，大trace、权重和build产物不加入git。
