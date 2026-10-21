# 用 HF 权重跑 EP1 和单元用例

EP1 全流程和各步单元共用同一个 gtest：`PcodeBundle.Ep1OneToken`（`test/bach/ip/pcode_bundle.cpp`）。换的是导出的 bundle 和 `PCODE_USERS`。权重默认来自 Hugging Face 上的 `mlx-community/GLM-5-8bit-MXFP8` 第 3 层。

两条仓库：

- latch：`/mnt/c/Users/kim01/Desktop/latch`，下面记为 `$LATCH`
- BachFlow：`/mnt/c/Users/kim01/Desktop/BachFlow`，下面记为 `$BACH`

本文的二进制按这次的构建目录写：`/root/build/latch/test/bach/pcode_bundle`。换了构建目录就改这一处。

## 1. 编译

```bash
cmake --build /root/build/latch --target pcode_bundle -j 8
```

目标名是 `pcode_bundle`。源码在 `test/bach/ip/pcode_bundle.cpp`。

## 2. 下载第 3 层权重

测试台在 `latch/index.txt` 不存在时会自己调下载脚本。也可以先单独下好。公开仓库不需要登录；gated 仓库要设 `HF_TOKEN`。

```bash
cd "$BACH"
PYTHONPATH=importer/src:pcode/src:defs:route/src \
python3 -u "$LATCH/test/bach/ip/fetch_ep1_hf_weights.py" \
  --out "$BACH/pcode/output/ep1_hf_weights"
```

脚本做两件事：

1. `python -m bachflow_importer --scenario glm5_ep1_pp3_tp16 --layers 3 --checkpoint mlx-community/GLM-5-8bit-MXFP8 --revision main`，按场景里的 `hf_name` 切到每个逻辑核。原始切分在 `<out>/weights/L00003/`（`mmem.c{g}.l{l}.bin`、`mmem_sf`、`weights.manifest.json`）。中断后重跑会从已下到的字节续传。
2. 用 `latch_export.logical_core_of` 把逻辑核映到物理核，把编译器文档 §3.3.1 的 scale 槽展开成「每 32 字节数据一个 E8M0」，写到 `<out>/latch/`。

产物：

```
pcode/output/ep1_hf_weights/latch/index.txt
pcode/output/ep1_hf_weights/latch/c{chip}p{phys}.dat
pcode/output/ep1_hf_weights/latch/c{chip}p{phys}.sf
```

`index.txt` 一行一个核：`chip phys offset data scale`。384 个计算核，磁盘上整份约 9.3 GiB。已有 `weights.manifest.json` 或 `latch/index.txt` 时脚本直接复用，不会重新下载。

不要把 `--out` 指到 `pcode/output/ep1_pp3_tp16`。那个目录是路由和 pcode 场景，权重放在旁边的 `ep1_hf_weights`。

从 Windows 盘读这批文件较慢。可以拷到本机盘再跑，测试台认的是 `PCODE_HF_WEIGHTS` 下面的 `latch/index.txt`：

```bash
mkdir -p /tmp/ep1_hf_weights
cp -a "$BACH/pcode/output/ep1_hf_weights/latch" /tmp/ep1_hf_weights/latch
```

## 3. 测试台灌进去的是哪一段

专家按 `0x18000` 从偏移 0 排开。物理序号 0 是共享专家，序号 `i`（`i ≥ 1`）是路由专家 `i - 1`。topK 的组内序号折进前 20 个路由专家，加上共享专家共 21 个物理序号。

Matrix Mem 按 128 字节一行稀疏存放。开钟前每个核只读、只 `Poke` `[0, 21 * 0x18000)` 的数据和同样长度的 scale。384 个核合计 756 MiB。后面的路由专家留在磁盘文件里，不进模拟器。没写过的行读出来是 0。

token 正文仍是测试台里的固定 E4M3 图案（`0x30..0x37`），topK 56 B。checkpoint 里没有 token。token 的 scale 按 1 写进 Core Mem。HF 权重对不上固定图案的参考，余弦不参与判断。要回到固定图案权重并比余弦，设 `PCODE_SYNTHETIC_WEIGHTS=1`。

## 4. 导出 bundle

单元和全流程都导出成名字 `pcode_ep1` 的 BACHIR 11 bundle。导出时 chip 23、chip 35 的 p2 path 7 会各打一行警告：逻辑核 2 按原表向东转发。这是预期的，留下即可。

### 4.1 全流程 EP1

场景目录是已经生成好的 `pcode/output/ep1_pp3_tp16`。

```bash
cd "$BACH"
PYTHONPATH=pcode/src:defs:route/src \
python3 -u -m bachflow_pcode.latch_export \
  pcode/output/ep1_pp3_tp16 \
  --out /tmp/latch_ep1 --name pcode_ep1
```

### 4.2 单元用例

单元是从 Step0 累加到某一步的前缀，路由复用 `pcode/output/ep1_pp3_tp16/route/route.resolved.json`，不重新搜路。

| 目录名 | 含到 |
| --- | --- |
| `s00_pcie_in` | Step0 |
| `s01_check_flag` | Step1 |
| `s02_token_bcast` | Step2 |
| `s03_fc1_gemv` | Step3 |
| `s04_fc3_gemv` | Step4 |
| `s05_fc1_reduce` | Step5 |
| `s06_fc3_reduce` | Step6 |
| `s07_silu` | Step7 |
| `s08_fc2_bcast` | Step8 |
| `s09_fc2_gemv` | Step9 |
| `s10_fc2_reduce` | Step10 |
| `s11_concat` | Step11 |
| `s12_pcie_out` | Step12，即全流程 |

以 Step9 为例。生成写到 `pcode/output/ep1_units/s09_fc2_gemv`，再导出：

```bash
cd "$BACH"
PYTHONPATH=pcode/src:defs:route/src \
python3 -u -m bachflow_pcode.scenarios.ep1_step_units s09_fc2_gemv

PYTHONPATH=pcode/src:defs:route/src \
python3 -u -m bachflow_pcode.latch_export \
  pcode/output/ep1_units/s09_fc2_gemv \
  --out /tmp/latch_ep1_s09 --name pcode_ep1
```

Step10 把上面的单元名换成 `s10_fc2_reduce`，导出目录换成 `/tmp/latch_ep1_s10`。一次生成全部单元：`ep1_step_units --all`。

## 5. 开跑

判断走空用 `PCODE_CHECK=idle`：在 `PCODE_MAX_CYCLES` 之前停钟即通过。16 用户一次大约几分钟；128 用户一次大约十几分钟。稀疏权重下，一次仿真权重约占 756 MiB，整进程大约数 GiB。两路并行放得下，三路会顶满 15 GiB 内存。

Step9、16 用户：

```bash
PCODE_BUNDLE_ROOT=/tmp/latch_ep1_s09 \
PCODE_BUNDLE_NAME=pcode_ep1 \
PCODE_HF_WEIGHTS=/tmp/ep1_hf_weights \
PCODE_USERS=16 \
PCODE_MAX_CYCLES=65536 \
PCODE_CHECK=idle \
stdbuf -oL /root/build/latch/test/bach/pcode_bundle \
  --gtest_filter=PcodeBundle.Ep1OneToken
```

32、128 用户只改 `PCODE_USERS`。128 用户把 `PCODE_MAX_CYCLES` 放到 `131072`。全流程把 `PCODE_BUNDLE_ROOT` 换成 `/tmp/latch_ep1`。

日志里先有一行：

```
HF 权重 384 个核，共享专家加前 20 个路由专家，756 MiB，/tmp/ep1_hf_weights
```

结束有一行 `EP1_RESULT`。走空的样子（Step9、16 用户）：

```
EP1_RESULT check idle stop 12153 limit 65536 snaps 16 inflight 16 insts 66822 fc1 -1 concat -1 head 16 tail 16
```

- `stop < limit`，并且 gtest 为 `PASSED`。
- `snaps` 等于用户数，`head` 和 `tail` 也等于用户数。
- `inflight 16` 是 chip 0 上自启动 B 核占着的 stream，计算核已经空了。
- `fc1` / `concat` 为 `-1`：HF 权重不做固定图案的余弦。

停在上限、`snaps 0`、在途不再下降，就是没走空。现场在 `EP1_RESULT` 前面的 `stuck chip` 行里。

## 6. 环境变量

| 变量 | 默认 | 作用 |
| --- | --- | --- |
| `PCODE_BUNDLE_ROOT` | `/tmp/latch_bundle` | 导出目录，里面要有 `pcode_ep1/pcode_ep1.bachir` |
| `PCODE_BUNDLE_NAME` | `pcode_ep1` | bundle 名字 |
| `PCODE_HF_WEIGHTS` | `$BACH/pcode/output/ep1_hf_weights` | 其下 `latch/index.txt` 为权重索引 |
| `PCODE_USERS` | `16` | 用户数 |
| `PCODE_MAX_CYCLES` | `400000` | 周期上限 |
| `PCODE_CHECK` | `full` | `idle` 只看走空；`fc1` / `full` 还比余弦。HF 权重下余弦期望被跳过 |
| `PCODE_SYNTHETIC_WEIGHTS` | 未设置 | 设为 `1` 时改回固定图案权重 |
| `BACHFLOW_ROOT` | `/mnt/c/Users/kim01/Desktop/BachFlow` | 找下载脚本和默认权重目录 |
| `LATCH_ROOT` | `/mnt/c/Users/kim01/Desktop/latch` | 找 `fetch_ep1_hf_weights.py` |
| `PCODE_IDLE_CYCLES` | `3000` | 连续这么多拍没有进展才算停钟 |
| `HF_TOKEN` | 空 | 下载 gated 仓库时用 |
