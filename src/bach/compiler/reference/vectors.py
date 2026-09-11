"""产出比对向量：参考实现算一遍，C++ 那一份读进来逐 bit 对。

三份文件各管一层：`scalar.txt` 是六种格式的标量编解码，`block.txt` 是 MX 的
分块编解码与 scale，`accum.txt` 是三条累加顺序。文件都是文本，每行一条，
第一列是这一条的种类。数值一律写成 FP32 的 32 位模式，不写十进制：十进制
的字面量在两侧解析出来未必是同一个 bit。
"""

import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ffn_reference as ffn
import numeric_ref as n

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vectors")


def h32(v):
    return f"{n.bits_of(v):08x}"


def hbytes(data):
    return "".join(f"{b:02x}" for b in data)


# 覆盖边界的一组输入：零与负零、非规格化、各档的最小与最大、上溢、舍入正中间
# 点、NaN 与 Inf。后面再补一批随机值。
def scalar_inputs():
    fixed = [
        0.0, -0.0,
        1.0, -1.0, 0.5, 2.0, -2.0,
        n.E4M3_MIN_SUBNORMAL, n.E4M3_MIN_SUBNORMAL / 2,
        n.E4M3_MIN_SUBNORMAL * 1.5,        # 非规格化的舍入正中间点
        448.0, 449.0, 512.0, -448.0, -512.0,
        0.0625, 0.09375, 6.0, 6.5, 7.0,    # E2M1 的档与档之间
        1.5, 3.0, 4.0, -6.0,
        3.14159265, 2.718281828,
        1e-30, 1e30, -1e30,
        float("inf"), float("-inf"), float("nan"),
        n.float_of(0x3F800001),            # 1.0 之上的一个 ulp
        n.float_of(0x00000001),            # FP32 的最小非规格化
        n.float_of(0x7F7FFFFF),            # FP32 的最大有限值
        n.float_of(0x3F808000),            # BF16 舍入的正中间点
        n.float_of(0x3F818000),
    ]
    rng = random.Random(20260908)
    rand = []
    for _ in range(4000):
        # 指数扫遍整个 FP32 的范围，尾数全随机：各档的上溢、下溢、非规格化与
        # 舍入正中间点都要扫到。
        e = rng.randint(0, 254)
        m = rng.getrandbits(23)
        s = rng.getrandbits(1)
        rand.append(n.float_of((s << 31) | (e << 23) | m))
    # 每一档的每个编码解出来的那个值，再编回去应当原样回来。
    for code in range(256):
        rand.append(n.from_fp8_e4m3(code))
    for code in range(16):
        rand.append(n.from_fp4_e2m1(code))
    # 相邻两档的正中间点：舍入规则在这里最容易分叉。
    for code in range(255):
        a, b = n.from_fp8_e4m3(code), n.from_fp8_e4m3(code + 1)
        if n.is_nan(a) or n.is_nan(b):
            continue
        rand.append(n.f32((a + b) / 2.0))
    for code in range(15):
        a, b = n.from_fp4_e2m1(code), n.from_fp4_e2m1(code + 1)
        rand.append(n.f32((a + b) / 2.0))
    return fixed + rand


SCALAR_CODECS = [
    # 名字, 编码, 解码
    ("bf16", n.to_bf16, n.from_bf16),
    ("fp8e4m3", n.to_fp8_e4m3, n.from_fp8_e4m3),
    ("fp4e2m1", n.to_fp4_e2m1, n.from_fp4_e2m1),
    ("e8m0", n.to_e8m0, n.from_e8m0),
]


def write_scalar(path):
    lines = ["# codec in_bits encoded back_bits"]
    for name, enc, dec in SCALAR_CODECS:
        for v in scalar_inputs():
            e = enc(v)
            lines.append(f"{name} {h32(v)} {e:04x} {h32(dec(e))}")
    # E5M2 只有解码那一半：硬件不往这一档写。
    for code in range(256):
        lines.append(f"fp8e5m2 00000000 {code:04x} {h32(n.from_fp8_e5m2(code))}")
    write(path, lines)


def write_block(path):
    """MX 的分块：一组值算出 scale、编成字节、再解回来。"""
    lines = ["# dtype count scale_hex data_hex scale_bits... | back_bits..."]
    rng = random.Random(20260908)
    cases = []
    for dtype in (n.MXFP8, n.MXFP4, n.NVFP4, n.BF16, n.FP32):
        block = n.SCALE_BLOCK[dtype]
        # 长度覆盖到不满一块与跨块，指数范围覆盖到各档的上溢与下溢。
        for count in (1, 8, 15, 16, 17, 31, 32, 33, 64, 256):
            for lo, hi in ((118, 132), (60, 200), (0, 254), (126, 128)):
                vals = []
                for i in range(count):
                    e = rng.randint(lo, hi)
                    m = rng.getrandbits(23)
                    s = rng.getrandbits(1)
                    vals.append(n.float_of((s << 31) | (e << 23) | m))
                cases.append((dtype, vals, block))
            # 一块里全是零、全是同一个值、只有一个大值：scale 的三种边界。
            cases.append((dtype, [0.0] * count, block))
            cases.append((dtype, [1.0] * count, block))
            spike = [0.25] * count
            spike[0] = 1e30
            cases.append((dtype, spike, block))
    for dtype, vals, block in cases:
        scale = n.make_scale(dtype, vals)
        sbytes = n.encode_scale(dtype, scale) if scale else b""
        data = n.encode(dtype, vals, scale)
        back = n.decode(dtype, data, len(vals))
        nblock = len(scale)
        dsc = n.decode_scale(dtype, sbytes, nblock) if nblock else []
        lines.append(" ".join([
            dtype, str(len(vals)),
            hbytes(sbytes) if sbytes else "-",
            hbytes(data),
            ",".join(h32(x) for x in vals),
            ",".join(h32(x) for x in dsc) if dsc else "-",
            ",".join(h32(x) for x in back),
        ]))
    write(path, lines)


def write_accum(path):
    """三条累加顺序。顺序不同结果就差一个 bit，所以这一份是硬判据。"""
    lines = ["# kind arg inputs... | extra... | result_bits"]
    rng = random.Random(20260908)

    def rand_vals(k, lo=112, hi=136):
        out = []
        for _ in range(k):
            e = rng.randint(lo, hi)
            m = rng.getrandbits(23)
            s = rng.getrandbits(1)
            out.append(n.float_of((s << 31) | (e << 23) | m))
        return out

    for k in (1, 2, 3, 4, 16, 32, 64, 255, 256):
        for lo, hi in ((112, 136), (100, 150), (60, 180)):
            vals = rand_vals(k, lo, hi)
            lines.append(f"in_order 0 {','.join(h32(x) for x in vals)} - "
                         f"{h32(n.accum_in_order(vals))}")
    for block in (16, 32):
        for nblock in (1, 2, 4, 8):
            prods = rand_vals(block * nblock)
            scale = rand_vals(nblock, 120, 130)
            lines.append(
                f"by_block {block} {','.join(h32(x) for x in prods)} "
                f"{','.join(h32(x) for x in scale)} "
                f"{h32(n.accum_by_scale_block(prods, scale, block))}")
    for lane in (1, 2, 3, 4, 8, 16):
        for total in (1, 5, 8, 16, 24, 33, 64, 129):
            lanes = rand_vals(total)
            lines.append(f"reduce_tree {lane} {','.join(h32(x) for x in lanes)} - "
                         f"{h32(n.reduce_tree(lanes, lane))}")
    # Clamp：NaN 与 Inf 都夹到该符号的最大有限值。
    for v in (float("nan"), float("inf"), float("-inf"), 1.0, -1.0, 0.0):
        lines.append(f"clamp 0 {h32(v)} - {h32(n.clamp_nan_inf(v))}")
    write(path, lines)


def write_ffn(path):
    """FFN 那几档算子：gemm、逐元素、量化写回、专家间归约。

    这一份是步 11 的判据：模型注入一个 token 收到的结果，要与这里算出来的逐
    bit 相同。数据用线性同余造，跑两次一样。
    """
    lines = ["# kind dtype args... | inputs... | result..."]
    # 前两种形状是 MU 的两条原语，模型那一侧直接拿这几条喂 matrix exe；
    # 其余几种只验算子本身。
    shapes = [(256, 32), (128, 64), (32, 2), (64, 4), (128, 8)]
    for dtype in (n.BF16, n.MXFP8, n.MXFP4, n.NVFP4):
        block = n.SCALE_BLOCK[dtype]
        for idx, (k, cnt) in enumerate(shapes):
            token = tame(dtype, pattern(k * n.ELEM_BITS[dtype] // 8,
                                        0x1234 + k))
            weight = tame(dtype, pattern(cnt * k * n.ELEM_BITS[dtype] // 8,
                                         0x5678 + cnt))
            scale = tame_scale(dtype, pattern(k // block, 0x9ABC + idx)) \
                if block else b""
            for out_bf16 in (0, 1):
                r = ffn.gemm(dtype, token, weight, scale, k, cnt,
                             out_bf16 == 1)
                lines.append(" ".join([
                    "gemm", dtype, str(k), str(cnt), str(out_bf16),
                    hbytes(token), hbytes(weight),
                    hbytes(scale) if scale else "-",
                    ",".join(h32(x) for x in r),
                ]))

    rng = random.Random(20260908)
    for op in ("add", "sub", "mul", "max", "min"):
        for k in (4, 16, 64):
            a = [n.float_of((rng.getrandbits(1) << 31)
                            | (rng.randint(110, 140) << 23)
                            | rng.getrandbits(23)) for _ in range(k)]
            b = [n.float_of((rng.getrandbits(1) << 31)
                            | (rng.randint(110, 140) << 23)
                            | rng.getrandbits(23)) for _ in range(k)]
            r = ffn.elemwise(op, a, b)
            lines.append(" ".join([
                "elemwise", op, str(k),
                ",".join(h32(x) for x in a), ",".join(h32(x) for x in b),
                ",".join(h32(x) for x in r),
            ]))

    # VU 的 vfredusum：LANES 固定 32，VL 是宏指令给的元素数。这几条由 VU 那一
    # 侧的测试读进去，用真模块跑一遍再比。
    for vl in (32, 64, 128):
        vals = []
        s = 0x301 + vl
        for _ in range(vl):
            s = (s * 1103515245 + 12345) & 0xFFFFFFFF
            man = (s >> 8) & 0x7FFFFF
            exp = 125 + ((s >> 3) % 3)
            sign = ((s >> 30) & 1) << 31
            vals.append(n.float_of(sign | (exp << 23) | man))
        r = n.clamp_nan_inf(n.f32(0.0 + n.reduce_tree(vals, 32)))
        lines.append(" ".join([
            "vu_reduce", "32", str(vl),
            ",".join(h32(x) for x in vals), h32(r),
        ]))

    for nparts in (2, 3, 8):
        for k in (4, 32):
            parts = []
            for p in range(nparts):
                parts.append([n.float_of((rng.getrandbits(1) << 31)
                                         | (rng.randint(115, 135) << 23)
                                         | rng.getrandbits(23))
                              for _ in range(k)])
            r = ffn.reduce_experts(parts)
            lines.append(" ".join([
                "reduce_experts", str(nparts), str(k),
                ";".join(",".join(h32(x) for x in p) for p in parts),
                ",".join(h32(x) for x in r),
            ]))
    write(path, lines)


def pattern(count, seed):
    """线性同余造一串字节，跑两次一样。"""
    out = bytearray()
    s = seed
    for _ in range(count):
        s = (s * 1103515245 + 12345) & 0xFFFFFFFFFFFFFFFF
        out.append((s >> 16) & 0xFF)
    return bytes(out)


def tame(dtype, data):
    """把随机字节压成能拿来比对的一批。

    两件事：抹掉 NaN 编码，NaN 一进累加链后面全是 NaN，比对就只在比 NaN 的
    载荷，那是实现细节；把 BF16 的指数压到 1 上下，随便取的 BF16 在一条 K=256
    的链上会把结果推到 ±Inf，比到最后只剩 Inf。
    """
    if dtype == n.MXFP8:
        # 0x7F / 0xFF 是这一档唯一的 NaN 编码。
        return bytes((b & 0xFE) if (b & 0x7F) == 0x7F else b for b in data)
    if dtype != n.BF16:
        return data
    out = bytearray(data)
    for i in range(1, len(out), 2):
        # 高字节的低 7 位是指数的高位，压到 0x3E~0x3F 那一档，值在 0.25~2 之间。
        out[i] = (out[i] & 0x80) | 0x3E | (out[i] & 1)
    return bytes(out)


def tame_cpp(dtype, count, seed):
    """按 C++ 那一侧 `TamePattern` 的规则造一批数，两边逐字节相同。

    一层 MoE 那一份向量的权重有几百 KB，写进文件不合适，所以两侧各自按同一个
    种子与同一套规则生成，向量里只留种子与结果。
    """
    bits = n.ELEM_BITS[dtype]
    v = bytearray(pattern(count * bits // 8, seed))
    if dtype == n.MXFP8:
        return bytes((b & 0xFE) if (b & 0x7F) == 0x7F else b for b in v)
    if dtype != n.BF16:
        return bytes(v)
    for i in range(0, len(v) - 1, 2):
        # 高字节装符号与指数的高 7 位，指数压到 126～128。
        sign = v[i + 1] & 0x80
        exp = 126 + (v[i] % 3)
        v[i + 1] = sign | (exp >> 1)
        v[i] = ((exp & 1) << 7) | (v[i] & 0x7F)
    return bytes(v)


def tame_scale(dtype, data):
    """scale 字节里的 NaN 编码同样要抹掉。

    MXFP8 的 scale 是 E8M0，0xFF 是它的 NaN；FP4 那两档的 scale 是 FP8 E4M3。
    另外把量级压到 1 上下：scale 太大时一条长累加链的结果会溢出到 Inf。
    """
    if dtype == n.MXFP8:
        return bytes(0x7F if b == 0xFF else (0x78 | (b & 0x07)) for b in data)
    return bytes(0x38 | (b & 0x07) for b in data)


def write_e2e(path):
    """端到端那一条链的期望输出。

    一个 K=256 的 BF16 token 进 core，MU 算一条 K=256 配 N=32 的原语，32 个
    FP32 结果出 core。模型注入的字节与收到的字节都在这里给出，收到的那一份要
    与模型跑出来的逐 bit 相同。
    """
    dtype = n.BF16
    k, cnt = 256, 32
    token = tame(dtype, pattern(k * 2, 0xE2E0))
    weight = tame(dtype, pattern(cnt * k * 2, 0xE2E1))
    out = ffn.gemm(dtype, token, weight, b"", k, cnt, False)
    # 结果按 FP32 写回 Core Mem，出核搬的就是这些字节。
    out_bytes = n.encode(n.FP32, out)
    # VU 那一步逐元素平方，源与目的都是这 32 个 FP32。选四则运算而不是 silu：
    # 超越函数硬件用查表加插值，拟合方式设计未给，拿它做逐 bit 判据立不住。
    act = ffn.elemwise("mul", out, out)
    act = [n.clamp_nan_inf(x) for x in act]
    lines = [
        "# 一条 K=256 配 N=32 的 BF16 原语，结果 FP32；VU 再逐元素平方一遍",
        f"dtype {dtype}",
        f"k {k}",
        f"n {cnt}",
        f"token {hbytes(token)}",
        f"weight {hbytes(weight)}",
        f"out_bits {','.join(h32(x) for x in out)}",
        f"out_bytes {hbytes(out_bytes)}",
        f"act_bits {','.join(h32(x) for x in act)}",
        f"act_bytes {hbytes(n.encode(n.FP32, act))}",
    ]
    # 放大到 N 个：几个 token 连着进来，各自算各自的，权重共用一份。
    for i in range(4):
        tok = tame(dtype, pattern(k * 2, 0xE2F0 + i))
        r = ffn.gemm(dtype, tok, weight, b"", k, cnt, False)
        lines.append(f"token{i} {hbytes(tok)}")
        lines.append(f"out_bytes{i} {hbytes(n.encode(n.FP32, r))}")
    write(path, lines)


# 一个 core 上那一段 MoE 的形状。三个矩阵按 EP6+TP8 切完之后每 core 拿到的
# 分片，方向与真实的一样。K 走完整的 embedding，中间维是 2048 按 TP8 切下来的
# 那一段，输出维是 MU 的一个 tile_N。
MOE_K = 6144         # 这个 core 分到的 embedding 那一段
MOE_INTER = 256      # 这个 core 分到的 intermediate 那一段
MOE_OUT = 6144       # 这个 core 要产出的 embedding 那一段
MOE_EXPERTS = 2      # 这个 token 在本 EP Group 内激活了几个专家

# MU 物理阵列的一笔原语：1×K256×N32。K 与 N 都按这个尺寸切块。
MU_TILE_K = 256
MU_TILE_N = 32


def moe_gemm(dtype, token, weight, k_total, n_total):
    """MU 的一笔任务：K 切 kblock 段、N 切 nblock 块，逐 tile 算。

    权重按 (n_idx, k_idx) 排，一个 tile 是 K256×N32 列优先，与 AGU 的
    WeightAddr 同序。同一列的几段顺序相加，每加一次 Clamp 一次，与 MatrixExe
    的 ksplit 累加同一条规矩。
    """
    ebytes = n.ELEM_BITS[dtype] // 8
    kb = k_total // MU_TILE_K
    nb = n_total // MU_TILE_N
    tile_bytes = MU_TILE_K * MU_TILE_N * ebytes
    seg_bytes = MU_TILE_K * ebytes
    out = []
    for ni in range(nb):
        acc = None
        for ki in range(kb):
            at = (ni * kb + ki) * tile_bytes
            tile = weight[at:at + tile_bytes]
            seg = token[ki * seg_bytes:(ki + 1) * seg_bytes]
            r = ffn.gemm(dtype, seg, tile, b"", MU_TILE_K, MU_TILE_N, False)
            acc = r if ki == 0 else [n.clamp_nan_inf(n.f32(x + y))
                                     for x, y in zip(acc, r)]
        out.extend(acc)
    return out


def moe_one_core(token, seeds, w_ep):
    """一个 core 上那五步算下来的中间量与结果。

    `seeds` 是 (w1, w3, w2) 三个种子，权重两侧各自按 `tame_cpp` 生成。返回
    每个专家的 FC1、FC3、量化后的激活，以及按 topK 权重合并之后的 32 个 FP32。
    """
    dtype = n.BF16
    parts = []
    act_bytes = []
    fc1_bytes, fc3_bytes = [], []
    for e in range(MOE_EXPERTS):
        w1 = tame_cpp(dtype, MOE_K * MOE_INTER, seeds[0] + e)
        w3 = tame_cpp(dtype, MOE_K * MOE_INTER, seeds[1] + e)
        w2 = tame_cpp(dtype, MOE_INTER * MOE_OUT, seeds[2] + e)

        fc1 = moe_gemm(dtype, token, w1, MOE_K, MOE_INTER)
        fc3 = moe_gemm(dtype, token, w3, MOE_K, MOE_INTER)
        fc1_bytes.append(n.encode(n.FP32, fc1))
        fc3_bytes.append(n.encode(n.FP32, fc3))
        # 门控：silu(FC1) 逐元素乘 FC3，再量化成 BF16 写回 Core Mem。
        act = ffn.elemwise("mul", ffn.silu(fc1), fc3)
        act = [n.clamp_nan_inf(x) for x in act]
        raw = n.encode(dtype, act)
        act_bytes.append(raw)
        # FC2 读回来的是量化之后的那一份。
        parts.append(moe_gemm(dtype, raw, w2, MOE_INTER, MOE_OUT))

    # 专家间合并：各自乘上 topK 权重再顺序相加，每一步都 Clamp。
    out = []
    for j in range(MOE_OUT):
        acc = 0.0
        for e in range(MOE_EXPERTS):
            v = n.clamp_nan_inf(n.f32(parts[e][j] * n.f32(w_ep[e])))
            acc = v if e == 0 else n.clamp_nan_inf(n.f32(acc + v))
        out.append(acc)
    return fc1_bytes, fc3_bytes, act_bytes, parts, out


def write_moe(path):
    """一个 core 上 EPTP-NN 那一段的期望输出。

    五步：token 进核、MU 算 FC1 与 FC3、VU 做门控与量化、MU 算 FC2 并按 topK
    权重合并几个专家、结果出核。

    权重有几百 KB，写进文件不合适，所以只给种子：两侧按 `tame_cpp` 的规则各自
    生成同一批字节。
    """
    dtype = n.BF16
    token = tame_cpp(dtype, MOE_K, 0x4001)
    w_ep = [0.75, 0.25]
    fc1_bytes, fc3_bytes, act_bytes, parts, out = moe_one_core(
        token, (0x4100, 0x4200, 0x4300), w_ep)

    lines = [
        "# 一个 core 上的 EPTP-NN 五步链：FC1 与 FC3、门控与量化、FC2 合并专家",
        f"dtype {dtype}",
        f"k {MOE_K}",
        f"inter {MOE_INTER}",
        f"out_n {MOE_OUT}",
        f"experts {MOE_EXPERTS}",
        "token_seed 4001",
        "w1_seed 4100",
        "w3_seed 4200",
        "w2_seed 4300",
        f"w_ep {','.join(h32(x) for x in w_ep)}",
        f"token {hbytes(token)}",
    ]
    for e in range(MOE_EXPERTS):
        lines.append(f"fc1_bytes{e} {hbytes(fc1_bytes[e])}")
        lines.append(f"fc3_bytes{e} {hbytes(fc3_bytes[e])}")
        lines.append(f"act_bytes{e} {hbytes(act_bytes[e])}")
        lines.append(f"part_bits{e} {','.join(h32(x) for x in parts[e])}")
    lines.append(f"out_bits {','.join(h32(x) for x in out)}")
    lines.append(f"out_bytes {hbytes(n.encode(n.FP32, out))}")
    write(path, lines)


# 一颗 chip 上 8 个计算 core。FC1 与 FC3 按中间维切成 8 份，FC2 因此按 K 切成
# 8 份，每个 core 出一份完整长度的部分和。
MOE_CHIP_CORES = 8

# chip 内那条归约链的走法。中间列 chip 是 2×4：同行相邻 core 有 left / right 一对
# 链路，core i 与 core i+4 有 mid 直连；四个 chip 口分别挂在 core0 的 left（N）、
# core3 的 right（E）、core4 的 left（W）、core7 的 right（S）。
#
# 链上每个 core 只能有一个上游：两个数相加与先后无关，三个数相加的结果就要看
# 谁先到了。一笔画过 8 个 core 的路径只在进出两个口一横一竖时存在（2×4 的格子
# 两染色，N 与 S 一色、E 与 W 一色，同色的两端画不出来），所以下面按转向列。
# 键是 (进哪个口, 出哪个口)，进的那一项为 None 表示本 chip 是整条链的起点。
MOE_CHAIN = {
    (None, "e"): [0, 4, 5, 1, 2, 6, 7, 3],
    (None, "s"): [4, 0, 1, 5, 6, 2, 3, 7],
    (None, "n"): [4, 5, 6, 7, 3, 2, 1, 0],
    ("w", "s"): [4, 0, 1, 5, 6, 2, 3, 7],
    ("n", "e"): [0, 4, 5, 1, 2, 6, 7, 3],
    ("w", "n"): [4, 5, 6, 7, 3, 2, 1, 0],
    ("s", "e"): [7, 6, 5, 4, 0, 1, 2, 3],
}
MOE_CHIP_ORDER = MOE_CHAIN[(None, "e")]

# 一个 EP 组的 8 颗 chip 摆成 2 层 × 4 列，层内左右相接（E 对 W）、层间上下相接
# （S 对 N），编号 gy * 4 + gx。部分和沿这条蛇形链逐跳相加，每颗 chip 进出各走
# 一横一竖，chip 内那条链才画得出来。
MOE_GROUP_CHIPS = 8
MOE_GROUP_CHIP_ORDER = [0, 4, 5, 1, 2, 6, 7, 3]
# 每颗 chip 在链上的进口与出口，次序同上。
MOE_GROUP_TURN = [
    (None, "s"), ("n", "e"), ("w", "n"), ("s", "e"),
    ("w", "s"), ("n", "e"), ("w", "n"), ("s", "e"),
]


def write_moe_spread(path, note, chips, order, seeds, w_ep, groups=None):
    """几个 core 各算一片、部分和沿一条链逐跳归约的期望输出。

    每个 core 的五步与 `write_moe` 相同，只是权重换成自己那一片。`order` 是
    全局 core 号（chip 号 × 8 加 chip 内号）排成的归约次序，链上每个 core 只有
    一个上游。C++ 那一侧读它铺路由表，并按模型的拓扑核对每一跳都落在真实链路上。
    """
    dtype = n.BF16
    token = tame_cpp(dtype, MOE_K, 0x4001)
    total = chips * MOE_CHIP_CORES

    parts = []
    for g in range(total):
        parts.append(moe_one_core(
            token, tuple(b + g * 0x10 for b in seeds), w_ep)[4])

    # Router 的 ReduceModule 逐跳做 Read-Modify-Write，中间累加固定 FP32。
    def reduce_chain(ids):
        out = list(parts[ids[0]])
        for g in ids[1:]:
            out = [n.clamp_nan_inf(n.f32(x + y)) for x, y in zip(out, parts[g])]
        return out

    if groups is None:
        acc = reduce_chain(order)
        group_out = []
    else:
        # 每组各归约成一份组结果，再在 R core 上逐组相加。
        group_out = [reduce_chain(chain) for chain in groups]
        acc = list(group_out[0])
        for one in group_out[1:]:
            acc = [n.clamp_nan_inf(n.f32(x + y)) for x, y in zip(acc, one)]

    lines = [
        f"# {note}",
        f"dtype {dtype}",
        f"k {MOE_K}",
        f"inter {MOE_INTER}",
        f"out_n {MOE_OUT}",
        f"experts {MOE_EXPERTS}",
        f"chips {chips}",
        f"cores {MOE_CHIP_CORES}",
        f"order {','.join(str(g) for g in order)}",
        "token_seed 4001",
        f"w1_seed {seeds[0]:x}",
        f"w3_seed {seeds[1]:x}",
        f"w2_seed {seeds[2]:x}",
        "seed_stride 10",
        f"w_ep {','.join(h32(x) for x in w_ep)}",
        f"token {hbytes(token)}",
    ]
    for g in range(total):
        lines.append(f"core_bits{g} {','.join(h32(x) for x in parts[g])}")
    for g, one in enumerate(group_out):
        lines.append(f"group_bits{g} {','.join(h32(x) for x in one)}")
    lines.append(f"out_bits {','.join(h32(x) for x in acc)}")
    write(path, lines)


def write_moe_chip(path):
    """一颗 chip 上 8 个 core 的那一段。链从 core0 起、在 core3 收尾，出 E 口。"""
    write_moe_spread(path, "一颗 chip 上 8 个 core 的 EPTP-NN 那一段，部分和沿链逐跳归约",
                     1, MOE_CHIP_ORDER, (0x5100, 0x5200, 0x5300), [0.75, 0.25])


# 一个 LPU：48 颗 chip 摆成 12 层 × 4 列，两层一个 EP 组，共 6 组。组内那条蛇形
# 链的走法与 MOE_GROUP_* 那一套同形，只是收尾改在最后一颗 chip 的 R core 上：链
# 尾那颗 chip 从 N 口进来，链在坐着 E 口那一侧的 core 收尾，再经不派角色的那个
# core 转两跳到 R core。
MOE_LPU_GROUPS = 6
MOE_LPU_CHIP_ORDER = [4, 0, 1, 5, 6, 2, 3, 7]
MOE_LPU_TURN = [
    (None, "n"), ("s", "e"), ("w", "s"), ("n", "e"),
    ("w", "n"), ("s", "e"), ("w", "s"), ("n", "e"),
]


def write_moe_lpu(path):
    """48 颗 chip、6 个 EP 组、384 个 core 的那一段。

    组内的部分和沿蛇形链归约成组结果，落进本组的 R core；六个组的 R core 串成
    一条链逐组相加，链首那一组的另一半一直是 0，加上去不改值。
    """
    groups = []
    for g in range(MOE_LPU_GROUPS):
        one = []
        for i, chip in enumerate(MOE_LPU_CHIP_ORDER):
            gy = g * 2 + chip // 4
            gx = chip % 4
            base = (gy * 4 + gx) * MOE_CHIP_CORES
            for c in MOE_CHAIN[MOE_LPU_TURN[i]]:
                one.append(base + c)
        groups.append(one)
    order = [g for one in groups for g in one]
    write_moe_spread(path, "48 颗 chip、6 个 EP 组的 EPTP-NN 那一段",
                     48, order, (0x20000, 0x40000, 0x60000), [0.75, 0.25],
                     groups=groups)


def write_moe_two_groups(path):
    """两个 EP 组，各一颗 chip 8 个 core，两组的结果在 R core 上相加。

    组内的部分和沿链逐跳归约成组结果。第一组是这个用户的链首，它的组结果直接
    送到第二组的 R core 的后一半；第二组的组结果落前一半。R core 上 VU 把两半
    相加，加法与 Router 上那一层同一条规矩：落回 FP32，每加一次 Clamp 一次。
    """
    chains = [[c for c in MOE_CHAIN[(None, "e")]],
              [MOE_CHIP_CORES + c for c in MOE_CHAIN[(None, "s")]]]
    order = [g for chain in chains for g in chain]
    write_moe_spread(path, "两个 EP 组各一颗 chip，组结果在 R core 上相加",
                     2, order, (0xC000, 0xE000, 0x10000), [0.75, 0.25],
                     groups=chains)


def write_moe_group(path):
    """一个 EP 组 8 颗 chip、64 个 core 的那一段。

    chip 之间的次序是 `MOE_GROUP_CHIP_ORDER`，每颗 chip 内部按它这一跳的转向从
    `MOE_CHAIN` 取一条，两级拼成一条 64 跳的链。
    """
    order = []
    for i, chip in enumerate(MOE_GROUP_CHIP_ORDER):
        for c in MOE_CHAIN[MOE_GROUP_TURN[i]]:
            order.append(chip * MOE_CHIP_CORES + c)
    write_moe_spread(path, "一个 EP 组 8 颗 chip、64 个 core 的 EPTP-NN 那一段",
                     MOE_GROUP_CHIPS, order, (0x6000, 0x8000, 0xA000),
                     [0.75, 0.25])


def write(path, lines):
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"  {os.path.basename(path):<12} {len(lines) - 1} 条")


# 一次要跑一分多钟的那几份。48 颗 chip 那一份要算 384 个 core，自检默认跳过它：
# 同一段代码在 moe.txt 与 moe_chip.txt 上已经查过，跳的只是这一份的新鲜度。
SLOW = ("moe_lpu.txt",)


def main(skip_slow=False):
    os.makedirs(OUT_DIR, exist_ok=True)
    print("比对向量：")
    write_scalar(os.path.join(OUT_DIR, "scalar.txt"))
    write_block(os.path.join(OUT_DIR, "block.txt"))
    write_accum(os.path.join(OUT_DIR, "accum.txt"))
    write_ffn(os.path.join(OUT_DIR, "ffn.txt"))
    write_e2e(os.path.join(OUT_DIR, "e2e.txt"))
    write_moe(os.path.join(OUT_DIR, "moe.txt"))
    write_moe_chip(os.path.join(OUT_DIR, "moe_chip.txt"))
    write_moe_group(os.path.join(OUT_DIR, "moe_group.txt"))
    write_moe_two_groups(os.path.join(OUT_DIR, "moe_two_groups.txt"))
    if not skip_slow:
        write_moe_lpu(os.path.join(OUT_DIR, "moe_lpu.txt"))


if __name__ == "__main__":
    main()
