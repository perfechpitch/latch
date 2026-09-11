"""FFN 各算子的参考实现，累加顺序与 `numeric/` 那一套一一对应。

覆盖 gemm、逐元素运算、量化写回、专家间归约、门控五档，都能与 C++ 那一份逐
bit 对上。

门控里的 sigmoid 走 ctypes 调 libm 的 `expf`，与 VU 那边 `std::exp` 收 float
入参时调的是同一个函数，所以两侧同一个 bit。硬件真身用查表加插值，拟合方式
设计未给，所以这一档比的是模型与本层的一致，不是模型与硅的一致。
"""

import ctypes
import ctypes.util
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numeric_ref as n

_libm = ctypes.CDLL(ctypes.util.find_library("m"))
_libm.expf.restype = ctypes.c_float
_libm.expf.argtypes = [ctypes.c_float]


def expf(x):
    """单精度的 exp，与 C++ 里 std::exp 收 float 入参时同一个实现。"""
    return n.f32(_libm.expf(n.f32(x)))


def sigmoid(x):
    """VSFU 的 sigmoid：1 / (1 + exp(-x))，每一步都落回 FP32。"""
    return n.f32(n.f32(1.0) / n.f32(n.f32(1.0) + expf(n.f32(-x))))


def silu(values):
    """SwiGLU 的门控：x × sigmoid(x)。

    VSFU 先算出 sigmoid，VALU 再乘回 x，两级各自舍入一次，不合成一步。
    """
    return [n.f32(x * sigmoid(x)) for x in values]


def gemm(dtype, token, weight, scale, k, count_n, out_bf16):
    """MU 的一条原语：一个 1×K 的 token 乘一个 K×N 的权重块。

    权重按列存，一列 K 个元素连着。块内先把乘积加完再乘 scale，块间顺序加，
    与硬件的 CSA 树同一个顺序。乘积先逐个算出来再加，不合成积和融合：那样少一
    次舍入，与硬件先乘后加差一个 bit。
    """
    block = n.SCALE_BLOCK[dtype]
    elem_bits = n.ELEM_BITS[dtype]
    a = n.decode(dtype, token, k)
    sc = n.decode_scale(dtype, scale, k // block) if block else []

    out = []
    col_bytes = k * elem_bits // 8
    for j in range(count_n):
        col = weight[j * col_bytes:(j + 1) * col_bytes]
        b = n.decode(dtype, col, k)
        prod = [n.f32(a[i] * b[i]) for i in range(k)]
        if block == 0:
            acc = n.accum_in_order(prod)
        else:
            acc = n.accum_by_scale_block(prod, sc, block)
        r = n.clamp_nan_inf(acc)
        if out_bf16:
            r = n.from_bf16(n.to_bf16(r))
        out.append(r)
    return out


def elemwise(op, a, b):
    """VALU 的逐元素两操作数运算。"""
    if op == "add":
        return [n.f32(x + y) for x, y in zip(a, b)]
    if op == "sub":
        return [n.f32(x - y) for x, y in zip(a, b)]
    if op == "mul":
        return [n.f32(x * y) for x, y in zip(a, b)]
    if op == "max":
        return [x if x > y else y for x, y in zip(a, b)]
    if op == "min":
        return [x if x < y else y for x, y in zip(a, b)]
    raise ValueError(f"不认识的逐元素运算 {op}")


def quantize(dtype, values):
    """VU 的 SU 写回 Core Mem：算出 scale、编成字节。

    返回 (scale 字节, 数据字节)。两样都要，收方按 scale 解才还原得出值。
    """
    scale = n.make_scale(dtype, values)
    return n.encode_scale(dtype, scale) if scale else b"", \
        n.encode(dtype, values, scale)


def reduce_experts(parts):
    """专家间求和：几份等长的结果逐元素相加，按专家的先后顺序加。

    Router 的 reduce 是逐跳做的，一跳加一个分量，所以顺序就是分量到达的顺序。
    """
    if not parts:
        return []
    out = list(parts[0])
    for part in parts[1:]:
        out = [n.f32(x + y) for x, y in zip(out, part)]
    return out


def reduce_lanes(values, lane_num):
    """VU 的 vfredusum：LANES 内先顺序归约，段间再走树。"""
    return n.reduce_tree(values, lane_num)
