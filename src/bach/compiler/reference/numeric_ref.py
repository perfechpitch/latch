"""数值格式的参考实现。

按 OCP MX 规范与 IEEE 754 从头写，不照着 `src/bach/common/numeric/` 那一份翻译：
两份实现独立写出来，逐 bit 比对才检得出其中一份的错。硬件在规范之外定死的几处
（BF16 的舍入取 RNE、E4M3 上溢 Clamp 到最大有限值、E2M1 等距时取偶数编码）在
各函数上方注明，那几处两份必须做同样的选择。
"""

import math
import struct

# ── FP32 的位 ──


def bits_of(v):
    """一个 float 的 32 位模式。"""
    return struct.unpack("<I", struct.pack("<f", v))[0]


def float_of(b):
    """32 位模式还原成 float。"""
    return struct.unpack("<f", struct.pack("<I", b & 0xFFFFFFFF))[0]


def split_f32(v):
    """拆成符号、指数字段、尾数字段三段。"""
    b = bits_of(v)
    return b >> 31, (b >> 23) & 0xFF, b & 0x7FFFFF


def is_nan(v):
    s, e, m = split_f32(v)
    return e == 0xFF and m != 0


def is_inf(v):
    s, e, m = split_f32(v)
    return e == 0xFF and m == 0


F32_NAN = 0x7FC00000
F32_MAX = 0x7F7FFFFF


def round_to_nearest_even(value, quantum):
    """把 value 量化成 quantum 的整数倍，正中间时取偶数倍。"""
    if quantum == 0:
        return 0
    q = value / quantum
    lo = math.floor(q)
    frac = q - lo
    if frac > 0.5:
        return lo + 1
    if frac < 0.5:
        return lo
    return lo if lo % 2 == 0 else lo + 1


# ── BF16：FP32 的高 16 位 ──
#
# 硬件选的是 round-to-nearest-even：截断会让误差单向累积。NaN 直接截，不让进位
# 把它变成 Inf。


def to_bf16(v):
    b = bits_of(v)
    if is_nan(v):
        return b >> 16
    lsb = (b >> 16) & 1
    return ((b + 0x7FFF + lsb) >> 16) & 0xFFFF


def from_bf16(h):
    return float_of((h & 0xFFFF) << 16)


# ── FP8 E4M3：1 符号 4 指数 3 尾数，偏置 7 ──
#
# OCP MX 规范里这一档没有 Inf，指数与尾数同时全 1 才是 NaN，所以最大有限值是
# 0x7E（2^8 × 1.75 = 448）。上溢 Clamp 到它，不产生 Inf。

E4M3_BIAS = 7
E4M3_MAX = 448.0
E4M3_MIN_SUBNORMAL = 2.0 ** -9   # 非规格化的最小步长


def from_fp8_e4m3(v):
    v &= 0xFF
    sign, exp, man = v >> 7, (v >> 3) & 0xF, v & 0x7
    if exp == 0xF and man == 0x7:
        return float_of(F32_NAN | (sign << 31))
    if exp == 0:
        val = man * E4M3_MIN_SUBNORMAL
    else:
        val = (1.0 + man / 8.0) * 2.0 ** (exp - E4M3_BIAS)
    return -val if sign else val


def to_fp8_e4m3(v):
    sign = bits_of(v) >> 31
    if is_nan(v) or is_inf(v):
        return (sign << 7) | 0x7F      # 这一档只有这一个 NaN 编码
    mag = abs(v)
    if mag > E4M3_MAX:
        return (sign << 7) | 0x7E      # Clamp
    if mag < E4M3_MIN_SUBNORMAL / 2:
        return sign << 7               # 下溢到零
    # 先按规格化那一档定阶，指数落到 0 以下的走非规格化。
    exp = math.floor(math.log2(mag)) if mag > 0 else 0
    if exp < 1 - E4M3_BIAS:
        q = round_to_nearest_even(mag, E4M3_MIN_SUBNORMAL)
        return (sign << 7) | min(q, 7)
    quantum = 2.0 ** (exp - 3)         # 这一档的尾数步长
    q = round_to_nearest_even(mag, quantum)
    if q >= 16:                        # 进位把尾数顶出这一档
        exp += 1
        q = round_to_nearest_even(mag, 2.0 ** (exp - 3))
    field = exp + E4M3_BIAS
    man = q - 8
    if field > 0xF or (field == 0xF and man == 7):
        return (sign << 7) | 0x7E      # 顶格那一位是 NaN 的编码，有限值不能落在那里
    if field <= 0:
        q = round_to_nearest_even(mag, E4M3_MIN_SUBNORMAL)
        return (sign << 7) | min(q, 7)
    return (sign << 7) | (field << 3) | man


# ── FP8 E5M2：1 符号 5 指数 2 尾数，偏置 15。这一档有 Inf 与 NaN ──

E5M2_BIAS = 15


def from_fp8_e5m2(v):
    v &= 0xFF
    sign, exp, man = v >> 7, (v >> 2) & 0x1F, v & 0x3
    if exp == 0x1F:
        if man == 0:
            return float_of((sign << 31) | 0x7F800000)
        return float_of(F32_NAN | (sign << 31))
    if exp == 0:
        val = man * 2.0 ** -16
    else:
        val = (1.0 + man / 4.0) * 2.0 ** (exp - E5M2_BIAS)
    return -val if sign else val


# ── FP4 E2M1：十六个取值 ±{0, 0.5, 1, 1.5, 2, 3, 4, 6} ──
#
# 取最近的那一档，与两档等距时取偶数编码。

E2M1_TABLE = [0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0]


def from_fp4_e2m1(v):
    v &= 0xF
    mag = E2M1_TABLE[v & 0x7]
    return -mag if (v >> 3) else mag


def to_fp4_e2m1(v):
    # 按符号位取而不按 v < 0：负零比较出来是 false，那样会把 −0 编成 +0。
    sign = bits_of(v) >> 31
    mag = -v if sign else v
    # 超出这一档能表示的范围就 Clamp 到最大的那一格，与 E4M3 上溢的处理一致。
    if not mag < 6.0:
        return (sign << 3) | 7
    best, best_d = 0, None
    for i, x in enumerate(E2M1_TABLE):
        d = abs(mag - x)
        if best_d is None or d < best_d or (d == best_d and i % 2 == 0):
            best, best_d = i, d
    return (sign << 3) | best


# ── E8M0：只有 8 位指数的 scale，值是 2 的幂。0xFF 是 NaN ──


def from_e8m0(v):
    v &= 0xFF
    if v == 0xFF:
        return float_of(F32_NAN)
    e = v - 127
    if e < -126:
        return 0.0
    return 2.0 ** e


def to_e8m0(v):
    return (bits_of(v) >> 23) & 0xFF


# ── 五档数据类型与它们的 block ──

BF16 = "bf16"
MXFP8 = "mxfp8"
MXFP4 = "mxfp4"
NVFP4 = "nvfp4"
FP32 = "fp32"

SCALE_BLOCK = {BF16: 0, MXFP8: 32, MXFP4: 16, NVFP4: 16, FP32: 0}
ELEM_BITS = {BF16: 16, MXFP8: 8, MXFP4: 4, NVFP4: 4, FP32: 32}
# 每档能表示的最大绝对值，MakeScale 用它把块内峰值压进范围。
ELEM_MAX = {MXFP8: E4M3_MAX, MXFP4: 6.0, NVFP4: 6.0}


def decode(dtype, data, count):
    """一段字节按格式解成 FP32 的一组。给不够的位置补零。"""
    out = []
    for i in range(count):
        if dtype == BF16:
            off = i * 2
            if off + 1 >= len(data):
                out.append(0.0)
            else:
                out.append(from_bf16(data[off] | (data[off + 1] << 8)))
        elif dtype == MXFP8:
            out.append(from_fp8_e4m3(data[i]) if i < len(data) else 0.0)
        elif dtype in (MXFP4, NVFP4):
            off = i // 2
            if off >= len(data):
                out.append(0.0)
            else:
                # 一个字节装两个，低半字节在前。
                nib = data[off] & 0xF if i % 2 == 0 else data[off] >> 4
                out.append(from_fp4_e2m1(nib))
        else:
            off = i * 4
            if off + 3 >= len(data):
                out.append(0.0)
            else:
                word = 0
                for k in range(4):
                    word |= data[off + k] << (8 * k)
                out.append(float_of(word))
    return out


def encode(dtype, values, block_scale=None):
    """decode 的对偶。给了 block_scale 的话元素先除以本块的 scale 再编。"""
    block = SCALE_BLOCK[dtype]

    def scaled(i):
        if block == 0 or not block_scale:
            return values[i]
        b = i // block
        s = block_scale[b] if b < len(block_scale) else 1.0
        return 0.0 if s == 0.0 else values[i] / s

    out = bytearray()
    if dtype == BF16:
        for i in range(len(values)):
            h = to_bf16(values[i])
            out += bytes([h & 0xFF, h >> 8])
    elif dtype == MXFP8:
        for i in range(len(values)):
            out.append(to_fp8_e4m3(scaled(i)))
    elif dtype in (MXFP4, NVFP4):
        out.extend(bytes((len(values) + 1) // 2))
        for i in range(len(values)):
            nib = to_fp4_e2m1(scaled(i)) & 0xF
            if i % 2 == 0:
                out[i // 2] = (out[i // 2] & 0xF0) | nib
            else:
                out[i // 2] = (out[i // 2] & 0x0F) | (nib << 4)
    else:
        for i in range(len(values)):
            b = bits_of(values[i])
            out += bytes([(b >> (8 * k)) & 0xFF for k in range(4)])
    return bytes(out)


def decode_scale(dtype, data, nblock):
    """scale 字节解成 FP32。MXFP8 那一档是 E8M0，FP4 两档是 FP8 E4M3。"""
    out = []
    for i in range(nblock):
        if i >= len(data):
            out.append(1.0)
        elif dtype == MXFP8:
            out.append(from_e8m0(data[i]))
        else:
            out.append(from_fp8_e4m3(data[i]))
    return out


def encode_scale(dtype, scale):
    """decode_scale 的对偶。"""
    if dtype == MXFP8:
        return bytes(to_e8m0(s) for s in scale)
    return bytes(to_fp8_e4m3(s) for s in scale)


def make_scale(dtype, values):
    """按 block 算各块的 scale：块内绝对值最大的那个定阶。

    算出来的 raw 还要编回 scale 自己那一档再解出来：真正生效的是能表示的
    那个值，不是 raw。
    """
    block = SCALE_BLOCK[dtype]
    if block == 0:
        return []
    emax = ELEM_MAX[dtype]
    out = []
    for b in range((len(values) + block - 1) // block):
        peak = max((abs(x) for x in values[b * block:(b + 1) * block]),
                   default=0.0)
        if peak == 0.0:
            out.append(1.0)
            continue
        raw = peak / emax
        if dtype == MXFP8:
            out.append(from_e8m0(to_e8m0(raw)))
        else:
            out.append(from_fp8_e4m3(to_fp8_e4m3(raw)))
    return out


# ── 累加顺序 ──
#
# 浮点加法不满足结合律，顺序是结果的一部分。这三条与硬件的顺序一一对应，
# 参考实现照抄同一个顺序，逐 bit 比对才立得住。


def f32(v):
    """把中间结果截回 FP32：Python 的 float 是双精度，不截就多带了 29 位。

    超出 FP32 范围时给该符号的 Inf，与硬件的浮点乘加溢出一致；Python 的 struct
    在这里是抛异常的。
    """
    try:
        return struct.unpack("<f", struct.pack("<f", v))[0]
    except OverflowError:
        return float("inf") if v > 0 else float("-inf")


def accum_by_scale_block(prods, block_scale, block):
    """MU 的 CSA 树：块内先把乘积加完再乘 scale，块间顺序加。"""
    total = 0.0
    for b in range(len(block_scale)):
        part = 0.0
        for i in range(b * block, min((b + 1) * block, len(prods))):
            part = f32(part + prods[i])
        total = f32(total + f32(part * block_scale[b]))
    return total


def accum_in_order(values):
    """没有 block scale 时（BF16 × BF16）就是顺序加。"""
    total = 0.0
    for x in values:
        total = f32(total + x)
    return total


def reduce_tree(lanes, lane_num):
    """VU 的归约：LANES 内先顺序归约，段间再走 ceil(log2 SEG) 级树。

    段数不是 2 的幂时，最后一级的落单项直接带到下一级。
    """
    seg = []
    for i in range(0, len(lanes), lane_num):
        s = 0.0
        for x in lanes[i:i + lane_num]:
            s = f32(s + x)
        seg.append(s)
    while len(seg) > 1:
        nxt = [f32(seg[i] + seg[i + 1]) for i in range(0, len(seg) - 1, 2)]
        if len(seg) % 2 == 1:
            nxt.append(seg[-1])
        seg = nxt
    return seg[0] if seg else 0.0


def clamp_nan_inf(v):
    """计算异常由硬件自动 Clamp，不走 Drain & Trap、不阻塞流水。"""
    if not (is_nan(v) or is_inf(v)):
        return v
    sign = bits_of(v) >> 31
    return float_of((sign << 31) | F32_MAX)
