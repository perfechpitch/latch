"""参考实现的自检。

两件事：一是编解码的往返（编回去再解出来落在同一格）与各档的边界值；二是比对
向量与当前代码一致，也就是有人改了参考实现却忘了重新产出向量的话这里会报。

`vectors.py` 产出的那几份文件由 C++ 那一侧读进来做逐 bit 比对，所以文件与代码
不同步会让比对比的是旧结果。
"""

import filecmp
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import ffn_reference as ffn
import numeric_ref as n
import vectors

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)


def check_roundtrip():
    """每个编码解出来的值，编回去要落回同一格。"""
    for code in range(256):
        v = n.from_fp8_e4m3(code)
        if n.is_nan(v):
            continue
        back = n.to_fp8_e4m3(v)
        check(back == code, f"E4M3 0x{code:02x} 往返成了 0x{back:02x}")
    for code in range(16):
        v = n.from_fp4_e2m1(code)
        back = n.to_fp4_e2m1(v)
        # ±0 编回来都落在 +0 那一格之外：0x8 是 −0，往返保住符号。
        check(back == code, f"E2M1 0x{code:x} 往返成了 0x{back:x}")
    for code in range(255):
        v = n.from_e8m0(code)
        if v == 0.0:
            continue
        check(n.to_e8m0(v) == code, f"E8M0 0x{code:02x} 往返错")


def check_bounds():
    """各档的边界：最大有限值、上溢 Clamp、下溢到零。"""
    check(n.from_fp8_e4m3(0x7E) == 448.0, "E4M3 的最大有限值不是 448")
    check(n.to_fp8_e4m3(1e30) == 0x7E, "E4M3 上溢没 Clamp 到最大有限值")
    check(n.to_fp8_e4m3(-1e30) == 0xFE, "E4M3 负向上溢没 Clamp")
    check(n.is_nan(n.from_fp8_e4m3(0x7F)), "E4M3 的 0x7F 不是 NaN")
    check(n.to_fp8_e4m3(0.0) == 0x00 and n.to_fp8_e4m3(-0.0) == 0x80,
          "E4M3 没保住负零")
    check(n.to_fp4_e2m1(1e30) == 0x7, "E2M1 上溢没 Clamp 到 6.0")
    check(n.to_fp4_e2m1(-0.0) == 0x8, "E2M1 没保住负零")
    check(n.from_fp4_e2m1(0x7) == 6.0, "E2M1 的最大档不是 6.0")
    check(n.is_nan(n.from_e8m0(0xFF)), "E8M0 的 0xFF 不是 NaN")
    check(n.to_bf16(n.float_of(0x3F808000)) == 0x3F80,
          "BF16 舍入正中间点没取偶")
    check(n.to_bf16(n.float_of(0x3F818000)) == 0x3F82,
          "BF16 舍入正中间点没取偶")


def check_accum_order():
    """顺序换一个，结果就该不一样 —— 不然逐 bit 比对根本没在比顺序。"""
    # 1 加三个 2^-24：顺序加时每一个都被舍掉，两两先加则凑出一个 2^-23 留下。
    one, tiny = n.float_of(0x3F800000), n.float_of(0x33800000)
    vals = [one, tiny, tiny, tiny]
    forward = n.accum_in_order(vals)
    backward = n.accum_in_order(list(reversed(vals)))
    check(n.bits_of(forward) != n.bits_of(backward),
          "顺序加与倒序加算出了同一个 bit，这组输入验不出顺序")
    tree = n.reduce_tree(vals, 1)
    check(n.bits_of(tree) != n.bits_of(forward),
          "归约树与顺序加算出了同一个 bit，这组输入验不出树形")
    # lane 内是顺序的：lane 开到全长时树就退化成顺序加。
    check(n.bits_of(n.reduce_tree(vals, len(vals))) == n.bits_of(forward),
          "lane 覆盖全长时归约树没退化成顺序加")


def check_gemm():
    """一条手算得出的 gemm。"""
    token = n.encode(n.BF16, [1.0, 2.0, 3.0, 4.0])
    weight = n.encode(n.BF16, [1.0, 1.0, 1.0, 1.0] + [2.0, 0.0, 0.0, 0.0])
    got = ffn.gemm(n.BF16, token, weight, b"", 4, 2, False)
    check(got == [10.0, 2.0], f"gemm 算出 {got}，应当是 [10.0, 2.0]")
    parts = [[1.0, 2.0], [0.5, 0.25], [0.125, 0.0625]]
    got = ffn.reduce_experts(parts)
    check(got == [1.625, 2.3125], f"专家归约算出 {got}")


def check_vectors_fresh():
    """比对向量与当前代码一致。"""
    tmp = tempfile.mkdtemp()
    try:
        saved = vectors.OUT_DIR
        vectors.OUT_DIR = tmp
        try:
            vectors.main(skip_slow=True)
        finally:
            vectors.OUT_DIR = saved
        for name in os.listdir(saved):
            if not name.endswith(".txt") or name in vectors.SLOW:
                continue
            a, b = os.path.join(saved, name), os.path.join(tmp, name)
            if not os.path.exists(b):
                FAILS.append(f"{name} 现在不产了，vectors/ 里还留着")
                continue
            check(filecmp.cmp(a, b, shallow=False),
                  f"{name} 与当前代码对不上，重跑 vectors.py")
    finally:
        shutil.rmtree(tmp)


def main():
    check_roundtrip()
    check_bounds()
    check_accum_order()
    check_gemm()
    check_vectors_fresh()
    for msg in FAILS:
        print(f"  失败  {msg}")
    if FAILS:
        print(f"{len(FAILS)} 项不通过")
        return 1
    print("参考实现自检通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
