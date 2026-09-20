# 参考实现

**模式**：spec（陈述当前的规则，取舍收在“取舍”一节里）
**层**：详细实现，建立在《latch 建模计划》（[`doc/bach/design-for-model/07-latch-建模计划.md`](../../../doc/bach/design-for-model/07-latch-建模计划.md)）的验收判据之上
**在代码里的位置**：`src/bach/common/numeric/` 的对照实现

给改数值代码的人：改完 `numeric/` 或本层任一份代码之后，照“改了之后要做的事”一节走一遍。逐 bit 比对是硬件模型数值正确性的唯一判据，两份实现对不上就是其中一份错了。

***

## 1　这一层是什么

同一套数值规则的第二份实现，用 Python 写，与 `src/bach/common/numeric/` 那份 C++ 实现互不引用。

两份各自算一遍，结果逐 bit 相同才算模型的数值行为立得住。本层产出比对向量，C++ 那一侧的测试读进来复算，比对不上就报。

覆盖六档：

| 档 | 内容 |
| - | - |
| 标量编解码 | BF16、FP8 E4M3、FP8 E5M2、FP4 E2M1、E8M0 |
| 分块编解码 | MXFP8、MXFP4、NVFP4 的 block scale 与数据字节 |
| 累加顺序 | MU 的按块累加（只乘 token 的 scale，或乘 token 与权重两个 scale 之积）、顺序累加、VU 的归约树、异常 Clamp |
| FFN 算子 | gemm（含 MXFP8 × MXFP8 两个 scale 那一档）、逐元素运算、量化写回、专家间归约、门控、VU 的 vfredusum |
| 端到端 | 一个 token 进 core、MU 算一条 1×K128×N64 的 MXFP8 原语、VU 再算一遍，出 core 的那几个字节 |
| MoE | EP6+TP8 的 KN 拆分：一个 core 的 FC1、FC3 部分和与 FC2 那一段，Router 的逐跳归约，dot core 的 silu·dot·量化与 concat，R core 的逐行相加 |

门控里的 sigmoid 两侧调的是同一个 libm `expf`：C++ 那一侧是 `std::exp` 收 float 入参，本层经 ctypes 调它。这一档比的因此是模型与本层一致；硬件真身用查表加插值，拟合方式设计未给。端到端那一档的 VU 算子取逐元素平方。

***

## 2　四份文件

| 文件 | 管什么 |
| - | - |
| `numeric_ref.py` | 五种格式的编解码、MX 的分块编解码、三条累加顺序 |
| `ffn_reference.py` | FFN 那几档算子，累加顺序取自 `numeric_ref` |
| `vectors.py` | 产出比对向量，落进 `vectors/` |
| `selftest.py` | 本层自检，挂在 CTest 的 `reference` 上 |

`vectors/` 下的文本文件由 `vectors.py` 产出，改代码之后要重新产一遍，内容进版本库。

***

## 3　比对向量的格式

比对向量都是文本，一行一条，`#` 开头的是表头。列之间用空格分，一列里的多个值用逗号分，多组值用分号分。空缺的列写 `-`。

数值一律写成位模式，不写十进制：十进制的字面量在 Python 与 C++ 里解析出来未必是同一个 bit。标量写 FP32 的 32 位模式，存储里的数据写原样字节。

| 文件 | 一行的形状 |
| - | - |
| `scalar.txt` | 格式名、输入的 32 位模式、编码结果、编码再解回来的 32 位模式 |
| `block.txt` | 格式名、元素数、scale 字节、数据字节、输入值、解出的 scale、解回的值 |
| `accum.txt` | 累加种类、参数、输入值、附加输入、结果 |
| `ffn.txt` | 算子名、算子各自的参数与输入、结果 |
| `e2e.txt` | 一行一个键值对，给出注入的 token、权重与两边的 scale，MU 算完的那一段，VU 算完的那一段 |
| `moe.txt` | 一行一个键值对，给出种子、token 与它的 scale、一个 core 的部分和、每个专家的 FC2 输入与它的 scale、FC2 那一段 |
| `moe_chip.txt`、`moe_two_groups.txt`、`moe_group.txt` | 一行一个键值对，给出种子、token 与它的 scale，每颗 chip 以 `c<组内序号>.` 开头的各 core 部分和、归约结果、FC2 输入、FC2 各段，出口上的结果；`moe_two_groups.txt` 另给行链结果，`moe_group.txt` 另给每行的行链结果与每个 R core 的结果 |
| `moe_lpu.txt` | 同 `moe_group.txt`，每颗 chip 只给 concat |
| `moe_lpu_tokens.txt` | 一行一个键值对，给出种子、第 0 个 token 与它的 scale、组数与 token 数，第 k 个 token 出口上的结果 `out<k>`。第 k 个 token 的种子是 `token_seed` 加 k |

MoE 那几份的权重按完整形状逐 tile 播种，向量里只给种子，两侧按同一个规则生成。

`ffn.txt` 里 `gemm` 那一类的前两种形状是 MU 的两条计算原语，K=64 配 N=128 与 K=128 配 N=64：物理阵列是 K128×N64，前者是它开 `vlane = 2`。MU 的测试只取这两种喂给 matrix exe，其余形状只验算子本身。`gemm_ws` 那一类是权重也带 scale 的 MXFP8 × MXFP8。`vu_reduce` 那一类由 VU 的测试喂给真模块跑。

***

## 4　判据

| 判据 | 在哪里比 |
| - | - |
| 五种标量格式的编解码逐 bit 对齐 | `test/bach/common/numeric_cross.cpp` |
| MX 的分块编解码与 scale 逐 bit 对齐 | 同上 |
| 三条累加顺序逐 bit 对齐 | 同上 |
| FFN 那几档算子逐 bit 对齐 | 同上 |
| MU 的 matrix exe 算出的结果与本层逐 bit 相同 | `test/bach/ip/chip/core/mu/mu.cpp` |
| VU 的 vfredusum 算出的结果与本层逐 bit 相同 | `test/bach/ip/chip/core/vu/vu.cpp` |
| 注入一个 token，出 core 的字节与本层逐 bit 相同 | `test/bach/ip/chip/core/e2e.cpp` |
| 一个 core 上 KN 那几步的中间量与本层逐 bit 相同 | `test/bach/ip/chip/core/moe.cpp` |
| 一颗 chip、一行两颗 chip、一个 EP 组的中间量与出口上的结果与本层逐 bit 相同 | `test/bach/ip/chip/moe_chip.cpp` |
| 48 颗 chip 的 concat、行链结果、R core 的结果与出口上的结果与本层逐 bit 相同 | `test/bach/ip/chip/moe_lpu.cpp` |
| 48 颗 chip 上连续跑 32 个 token，每个 token 出口上的结果与本层逐 bit 相同 | `test/bach/ip/chip/moe_lpu_tokens.cpp` |

NaN 只比“是不是 NaN”，不比载荷：多个编码都是 NaN，载荷是实现细节。

***

## 5　改了之后要做的事

改了 `numeric/`、`numeric_ref.py` 或 `ffn_reference.py` 中的任何一份：

1. 跑 `python3 src/bach/compiler/reference/vectors.py` 重新产出比对向量，全部产一遍约 2 小时，几乎都花在 `moe_lpu_tokens.txt` 上
2. 跑 `python3 src/bach/compiler/reference/selftest.py`
3. 跑 `ctest -R 'numeric_cross|mu|vu|e2e|moe|reference'`
4. 把 `vectors/` 下改动的文件一并提交

`selftest.py` 会把向量重新产一遍与在版本库里的那份比，忘了第 1 步的话它报错。`moe_group.txt`、`moe_lpu.txt` 一份要算一分钟到几分钟，`moe_lpu_tokens.txt` 要算约 2 小时，自检跳过这三份。

***

## 6　取舍

* **为什么两份实现要用不同的语言**
  * 同一个人用同一种语言写两遍，容易在两边犯同一个错，比对就成了自我印证
  * 换一种语言写，浮点的中间精度、库函数、整数溢出的行为都不同，逼着按格式规范推导而不是照抄

* **为什么向量落成文件而不是让 C++ 直接调 Python**
  * 落成文件之后，向量与产它的代码一起进版本库，改数值规则时 diff 上能看见哪些值变了
  * 跑测试不依赖 Python 环境

* **为什么 Python 这一份把中间结果截回 FP32**
  * Python 的 float 是双精度，不截就比硬件多带 29 位尾数，累加链上的每一步都会与硬件分叉
  * 截的位置与硬件的每一级流水一一对应：先乘后加，每一步各截一次
