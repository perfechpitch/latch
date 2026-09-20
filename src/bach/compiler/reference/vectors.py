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
    # MXFP8 × MXFP8：token 与权重各一组 E8M0 scale。extra 一栏前一半是 token 的，
    # 后一半是权重的。另用一个随机源，前面几种的输入不跟着变。
    rng2 = random.Random(20260917)
    for nblock in (1, 2, 4):
        prods = []
        for _ in range(32 * nblock):
            prods.append(n.float_of((rng2.getrandbits(1) << 31)
                                    | (rng2.randint(112, 136) << 23)
                                    | rng2.getrandbits(23)))
        a_scale = [n.from_e8m0(rng2.randint(120, 130)) for _ in range(nblock)]
        w_scale = [n.from_e8m0(rng2.randint(120, 130)) for _ in range(nblock)]
        lines.append(
            f"by_block2 32 {','.join(h32(x) for x in prods)} "
            f"{','.join(h32(x) for x in a_scale + w_scale)} "
            f"{h32(n.accum_by_scale_block2(prods, a_scale, w_scale, 32))}")
    write(path, lines)


def write_ffn(path):
    """FFN 那几档算子：gemm、逐元素、量化写回、专家间归约。

    这一份是步 11 的判据：模型注入一个 token 收到的结果，要与这里算出来的逐
    bit 相同。数据用线性同余造，跑两次一样。
    """
    lines = ["# kind dtype args... | inputs... | result..."]
    # 前两种形状是 MU 的两条原语：物理阵列 K128×N64，开 vlane = 2 就是
    # K64×N128。模型那一侧直接拿这几条喂 matrix exe；其余几种只验算子本身。
    shapes = [(64, 128), (128, 64), (32, 2), (64, 4), (128, 8)]
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

    # MXFP8 × MXFP8：权重也带 scale，按列排，一列 K / 32 个。块内部分和乘
    # token 与权重两个 scale 之积。
    dtype = n.MXFP8
    block = n.SCALE_BLOCK[dtype]
    for idx, (k, cnt) in enumerate(shapes):
        token = tame(dtype, pattern(k, 0x2234 + k))
        weight = tame(dtype, pattern(cnt * k, 0x6678 + cnt))
        scale = tame_scale(dtype, pattern(k // block, 0xAABC + idx))
        wscale = tame_scale(dtype, pattern(cnt * (k // block), 0xBBCD + idx))
        for out_bf16 in (0, 1):
            r = ffn.gemm(dtype, token, weight, scale, k, cnt, out_bf16 == 1,
                         wscale)
            lines.append(" ".join([
                "gemm_ws", dtype, str(k), str(cnt), str(out_bf16),
                hbytes(token), hbytes(weight), hbytes(scale), hbytes(wscale),
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

    一条 MU 原语 1×K128×N64：MXFP8 的 token 乘 MXFP8 的权重，两边的 scale 都随
    数据从 scale 旁带读出来，结果按 BF16 写回，64 个。模型注入的字节与收到的字节
    都在这里给出，收到的那一份要与模型跑出来的逐 bit 相同。
    """
    k, cnt = 128, 64
    token = kn_data(k, 0xE2E0)
    tscale = kn_scale(k // 32, 0xE2E0)
    weight = kn_data(cnt * k, 0xE2E1)
    wscale = kn_scale(cnt * (k // 32), 0xE2E1)
    out = ffn.gemm(n.MXFP8, token, weight, tscale, k, cnt, True, wscale)
    out_bytes = n.encode(n.BF16, out)
    # VU 那一步读 BF16、逐元素平方、按 BF16 写回。选四则运算而不是 silu：超越函数
    # 硬件用查表加插值，拟合方式设计未给，拿它做逐 bit 判据立不住。
    act = [n.clamp_nan_inf(x) for x in ffn.elemwise("mul", out, out)]
    act_bytes = n.encode(n.BF16, act)
    lines = [
        "# 一条 1×K128×N64 的 MXFP8 原语，结果 BF16；VU 再逐元素平方一遍，仍写 BF16",
        f"dtype {n.MXFP8}",
        f"k {k}",
        f"n {cnt}",
        f"token {hbytes(token)}",
        f"token_scale {hbytes(tscale)}",
        f"weight {hbytes(weight)}",
        f"weight_scale {hbytes(wscale)}",
        f"out_bits {','.join(h32(x) for x in out)}",
        f"out_bytes {hbytes(out_bytes)}",
        f"act_bytes {hbytes(act_bytes)}",
    ]
    # 放大到 N 个：几个 token 连着进来，各自算各自的，权重共用一份。
    for i in range(4):
        tok = kn_data(k, 0xE2F0 + i)
        tsc = kn_scale(k // 32, 0xE2F0 + i)
        r = ffn.gemm(n.MXFP8, tok, weight, tsc, k, cnt, True, wscale)
        lines.append(f"token{i} {hbytes(tok)}")
        lines.append(f"token_scale{i} {hbytes(tsc)}")
        lines.append(f"out_bytes{i} {hbytes(n.encode(n.BF16, r))}")
    write(path, lines)


# ── 一层 MoE：EP6+TP8 的 KN 拆分 ──
#
# 一个 EP 组两层 × 4 列共 8 颗 chip，每颗 chip 8 个计算 core。chip 在组里的序号
# c（层 × 4 + 列）定它分到 FC1、FC3 的哪一段 N 与 FC2 的哪一段 K；core 的逻辑槽位
# s 定它分到 FC1、FC3 的哪一段 K 与 FC2 的哪一段 N。槽位 7 是 dot core。
# 与 compiler/kernel/bach.h 的 MOE_* 同源，改一处要一起改。
KN_EMBED = 6144
KN_INTER = 2048
KN_SLOTS = 8
KN_DOT_SLOT = 7
KN_EXPERTS = 2
KN_SEG_EMBED = KN_EMBED // KN_SLOTS     # 768
KN_SEG_INTER = KN_INTER // KN_SLOTS     # 256
# FC1 与 FC3 按 1×K128×N64 切块，FC2 按 1×K64×N128（K128×N64 阵列开 vlane = 2）
FC13_K, FC13_N = 128, 64
FC2_K, FC2_N = 64, 128
# topK 里两个专家的权重
KN_W_EP = [0.75, 0.25]
# chip 内那条归约链的逻辑槽位次序，链尾是 dot core
KN_CHIP_CHAIN = [6, 5, 4, 0, 1, 2, 3, 7]


def kn_data(count, seed):
    """一段 MXFP8 元素：线性同余造字节，抹掉 NaN 编码。

    与 C++ 那一侧的 `KnData` 逐字节相同：权重与 token 两侧各自按同一个种子生成，
    向量里只留种子与结果。
    """
    return bytes((b & 0xFE) if (b & 0x7F) == 0x7F else b
                 for b in pattern(count, seed))


def kn_scale(count, seed):
    """一段 E8M0 scale：收进 2^-14～2^-7，元素乘上它落在个位数上下，sigmoid 不至于
    全饱和。

    种子与同一段数据的种子相同，另异或一个常数分开两条序列。与 C++ 那一侧的
    `KnScale` 逐字节相同。
    """
    return bytes(113 + (b % 8) for b in pattern(count, seed ^ 0x5CA1E))


def kn_seed(base, group, expert):
    """一个矩阵在第 group 个 EP 组、topK 里第 expert 个专家上的种子。

    一个矩阵的 tile 编号不到 0x1000，各组各专家隔开 0x1000。"""
    return base + (group * KN_EXPERTS + expert) * 0x1000


class KnMatrix:
    """一个完整形状的矩阵：rows × cols，按 tile_k × tile_n 分块播种。

    第 (r0, c0) 起的那个 tile 用 `seed + tile 编号` 生成，编号只由它在完整矩阵里
    的位置定，所以一个元素的值只由它在完整矩阵里的位置定，切到哪个 core 上都
    一样；每个 core 也只需生成自己那几个 tile。tile 里的数据列优先，scale 每列
    tile_k / 32 个。
    """

    def __init__(self, rows, cols, tile_k, tile_n, seed):
        self.rows, self.cols = rows, cols
        self.tile_k, self.tile_n = tile_k, tile_n
        self.seed = seed

    def tile(self, r0, c0):
        tid = (c0 // self.tile_n) * (self.rows // self.tile_k) + r0 // self.tile_k
        count = self.tile_k * self.tile_n
        return (kn_data(count, self.seed + tid),
                kn_scale(self.tile_n * (self.tile_k // 32), self.seed + tid))


# 三个矩阵的基种子
KN_W1 = 0x100000
KN_W3 = 0x200000
KN_W2 = 0x300000
KN_TOKEN_SEED = 0x4001


def kn_w13(base, group, expert):
    """W1 或 W3：KN_EMBED 行（token 维）× KN_INTER 列，按 K128×N64 分块。"""
    return KnMatrix(KN_EMBED, KN_INTER, FC13_K, FC13_N,
                    kn_seed(base, group, expert))


def kn_w2(group, expert):
    """W2：KN_INTER 行 × KN_EMBED 列，按 K64×N128 分块。"""
    return KnMatrix(KN_INTER, KN_EMBED, FC2_K, FC2_N,
                    kn_seed(KN_W2, group, expert))


def kn_token(k=0):
    """第 k 个 token：数据与 scale 都用种子 KN_TOKEN_SEED + k 生成。只发一个 token
    的那几份用第 0 个。"""
    seed = KN_TOKEN_SEED + k
    return kn_data(KN_EMBED, seed), kn_scale(KN_EMBED // 32, seed)


def bf16_decode(data):
    return n.decode(n.BF16, data, len(data) // 2)


def kn_blocked(tokens, matrix, row0, col0, k, cnt, kblock, nblock, w_ep=None):
    """MU 的一笔任务：K 切 kblock 段、N 切 nblock 块，逐 tile 算，结果 BF16。

    tokens 是 topK 里每个专家一份 (数据, scale)：各出一份的那一档几个专家共用
    一份，合并成一份的那一档每个专家一份。matrix 是每个专家一个 KnMatrix。
    这一个 core 分到的那一片从完整矩阵的第 row0 行、第 col0 列起。

    循环顺序与 AGU 同：由内往外是 tile_K、专家、tile_N。同一列的几段顺序相加，
    每加一次 Clamp 一次；合并成一份时这一列的部分和乘上这个专家的权重再顺序加，
    每一步也 Clamp。结果在一列算完之后才转 BF16。w_ep 为 None 时各出一份。
    """
    experts = len(matrix)
    outs = [[] for _ in range(experts)]
    merged = []
    for ni in range(nblock):
        ep_acc = None
        for e in range(experts):
            tok, tsc = tokens[e] if len(tokens) > 1 else tokens[0]
            acc = None
            for ki in range(kblock):
                data, wsc = matrix[e].tile(row0 + ki * k, col0 + ni * cnt)
                seg = tok[ki * k:(ki + 1) * k]
                sc = tsc[ki * (k // 32):(ki + 1) * (k // 32)]
                r = ffn.gemm(n.MXFP8, seg, data, sc, k, cnt, False, wsc)
                acc = r if ki == 0 else [n.clamp_nan_inf(n.f32(x + y))
                                         for x, y in zip(acc, r)]
            if w_ep is None:
                outs[e].extend(acc)
                continue
            w = [n.clamp_nan_inf(n.f32(x * n.f32(w_ep[e]))) for x in acc]
            ep_acc = w if e == 0 else [n.clamp_nan_inf(n.f32(x + y))
                                       for x, y in zip(ep_acc, w)]
        if w_ep is not None:
            merged.extend(ep_acc)
    if w_ep is None:
        return [n.encode(n.BF16, o) for o in outs]
    return n.encode(n.BF16, merged)


def kn_part(token, group, chip, slot):
    """一个 core 的 FC1、FC3 部分和：token 第 slot 段乘本 core 那一片 W1、W3。

    返回部分和那一包里的数据段：两个专家的 FC1，再两个专家的 FC3，各 256 个 BF16。
    """
    tok, tsc = token
    k0 = slot * KN_SEG_EMBED
    seg = (tok[k0:k0 + KN_SEG_EMBED], tsc[k0 // 32:(k0 + KN_SEG_EMBED) // 32])
    col0 = chip * KN_SEG_INTER
    out = b""
    for base in (KN_W1, KN_W3):
        mats = [kn_w13(base, group, e) for e in range(KN_EXPERTS)]
        for one in kn_blocked([seg], mats, k0, col0, FC13_K, FC13_N,
                              KN_SEG_EMBED // FC13_K, KN_SEG_INTER // FC13_N):
            out += one
    return out


def router_reduce(parts):
    """Router 上逐跳的 reduce：每一跳把上一跳的 BF16 结果与本 core 那一份解成 FP32
    相加、Clamp，再按 BF16 写出去。链首只有本 core 一份，原样出去。"""
    acc = parts[0]
    for one in parts[1:]:
        a, b = bf16_decode(acc), bf16_decode(one)
        acc = n.encode(n.BF16, [n.clamp_nan_inf(n.f32(x + y))
                                for x, y in zip(a, b)])
    return acc


def kn_gate(red):
    """dot core 的 silu·dot·量化：归约出来的 FC1、FC3 按 BF16 读，按 FP32 算，
    量化成 MXFP8。返回每个专家一份 (数据, scale)。"""
    vals = bf16_decode(red)
    out = []
    for e in range(KN_EXPERTS):
        fc1 = vals[e * KN_SEG_INTER:(e + 1) * KN_SEG_INTER]
        fc3 = vals[(KN_EXPERTS + e) * KN_SEG_INTER:
                   (KN_EXPERTS + e + 1) * KN_SEG_INTER]
        act = [n.clamp_nan_inf(x)
               for x in ffn.elemwise("mul", ffn.silu(fc1), fc3)]
        scale = n.make_scale(n.MXFP8, act)
        out.append((n.encode(n.MXFP8, act, scale),
                    n.encode_scale(n.MXFP8, scale)))
    return out


def kn_fc2(act, group, chip, slot):
    """一个 core 的 FC2 第 slot 段：每个专家一份 FC2 输入，乘本 core 那一片 W2，
    按 topK 权重在 MU 内合并成一份，768 个 BF16。"""
    mats = [kn_w2(group, e) for e in range(KN_EXPERTS)]
    return kn_blocked(act, mats, chip * KN_SEG_INTER, slot * KN_SEG_EMBED,
                      FC2_K, FC2_N, KN_SEG_INTER // FC2_K,
                      KN_SEG_EMBED // FC2_N, KN_W_EP)


def rcore_add(row, prev):
    """R core 的两半求和：两份 BF16 读进来按 FP32 相加、Clamp，按 BF16 写回。链首
    没有上一行，另一半一直是 0。"""
    a = bf16_decode(row)
    b = bf16_decode(prev) if prev is not None else [0.0] * len(a)
    return n.encode(n.BF16, [n.clamp_nan_inf(n.f32(x + y))
                             for x, y in zip(a, b)])


def kn_chip(token, group, chip):
    """一颗 chip 上的整段：8 个 core 的部分和沿 chip 内归约链归约进 dot core，dot
    core 做 silu·dot·量化并广播，8 个 core 各算 FC2 一段，在 dot core 上拼成 6144
    个 BF16。返回各步的中间量。"""
    parts = [kn_part(token, group, chip, s) for s in range(KN_SLOTS)]
    red = router_reduce([parts[s] for s in KN_CHIP_CHAIN])
    act = kn_gate(red)
    fc2 = [kn_fc2(act, group, chip, s) for s in range(KN_SLOTS)]
    return {"parts": parts, "red": red, "act": act, "fc2": fc2,
            "concat": b"".join(fc2)}


def kn_header(note, extra):
    lines = [
        f"# {note}",
        f"embed {KN_EMBED}",
        f"inter {KN_INTER}",
        f"slots {KN_SLOTS}",
        f"experts {KN_EXPERTS}",
        f"token_seed {KN_TOKEN_SEED:x}",
        f"w1_seed {KN_W1:x}",
        f"w3_seed {KN_W3:x}",
        f"w2_seed {KN_W2:x}",
        f"w_ep {','.join(h32(x) for x in KN_W_EP)}",
    ]
    tok, tsc = kn_token()
    lines.append(f"token {hbytes(tok)}")
    lines.append(f"token_scale {hbytes(tsc)}")
    return lines + extra


def kn_chip_lines(tag, one):
    """一颗 chip 的中间量写进向量：各 core 的部分和与 FC2 段、归约结果、FC2 输入。"""
    lines = []
    for s in range(KN_SLOTS):
        lines.append(f"{tag}part{s} {hbytes(one['parts'][s])}")
    lines.append(f"{tag}red {hbytes(one['red'])}")
    for e in range(KN_EXPERTS):
        lines.append(f"{tag}act{e} {hbytes(one['act'][e][0])}")
        lines.append(f"{tag}act_scale{e} {hbytes(one['act'][e][1])}")
    for s in range(KN_SLOTS):
        lines.append(f"{tag}fc2_{s} {hbytes(one['fc2'][s])}")
    return lines


def write_moe(path):
    """一个 core 上的 KN 那几步：它当 dot core 用，自己一个 core 就是整条 chip 内
    归约链。FC1、FC3 部分和，归约（只有自己一份，原样），silu·dot·量化，FC2 第 7
    段。组 0 的第 0 颗 chip。"""
    token = kn_token()
    part = kn_part(token, 0, 0, KN_DOT_SLOT)
    red = router_reduce([part])
    act = kn_gate(red)
    fc2 = kn_fc2(act, 0, 0, KN_DOT_SLOT)
    lines = kn_header("一个 core 的 KN 那几步：槽位 7 的部分和、silu·dot·量化、FC2 第 7 段",
                      ["group 0", "chip 0", f"slot {KN_DOT_SLOT}",
                       f"part {hbytes(part)}"])
    for e in range(KN_EXPERTS):
        lines.append(f"act{e} {hbytes(act[e][0])}")
        lines.append(f"act_scale{e} {hbytes(act[e][1])}")
    lines.append(f"fc2 {hbytes(fc2)}")
    write(path, lines)


def write_moe_chip(path):
    """一颗第一列 chip：B core 起头广播，chip 内归约进 dot core，广播 FC2 输入，
    concat 拼成 6144 个 BF16，从 E 口出去。它是一行的行首，行链只有它一跳，结果
    原样出去。"""
    one = kn_chip(kn_token(), 0, 0)
    row = router_reduce([one["concat"]])
    lines = kn_header("一颗第一列 chip 的 KN 那一段：chip 内归约、dot core、concat",
                      ["group 0", "chips 0"] + kn_chip_lines("c0.", one) +
                      [f"out {hbytes(row)}"])
    write(path, lines)


def write_moe_two_groups(path):
    """一行两颗 chip：中间列（组内第 1 颗）加最后一列（组内第 3 颗）。两颗的结果
    沿行链逐跳归约进最后一列那颗 chip 的 R core，它是链首，另一半一直是 0。"""
    token = kn_token()
    chips = [1, 3]
    ones = [kn_chip(token, 0, c) for c in chips]
    row = router_reduce([o["concat"] for o in ones])
    out = rcore_add(row, None)
    lines = []
    for c, o in zip(chips, ones):
        lines += kn_chip_lines(f"c{c}.", o)
    lines = kn_header("一行两颗 chip：行链两跳落进 R core",
                      ["group 0", f"chips {','.join(str(c) for c in chips)}"]
                      + lines + [f"row0 {hbytes(row)}", f"out {hbytes(out)}"])
    write(path, lines)


def kn_rows(token, groups):
    """几个 EP 组：每组两行，每行 4 颗 chip 的结果沿行链归约进本行 R core，各行的
    R core 逐行相加。返回每颗 chip 的中间量、每行的行链结果与每个 R core 的结果。"""
    chips, rows, rcores = [], [], []
    prev = None
    for g in range(groups):
        for layer in range(2):
            ones = [kn_chip(token, g, layer * 4 + col) for col in range(4)]
            chips.append(ones)
            row = router_reduce([o["concat"] for o in ones])
            rows.append(row)
            prev = rcore_add(row, prev)
            rcores.append(prev)
    return chips, rows, rcores


def write_moe_group(path):
    """一个 EP 组：两层 × 4 列 8 颗 chip。两行各沿行链归约进本行 R core，两个 R core
    串成一条链，第二个的结果从右下角那颗 chip 的东口出去。"""
    chips, rows, rcores = kn_rows(kn_token(), 1)
    lines = []
    for layer in range(2):
        for col in range(4):
            lines += kn_chip_lines(f"c{layer * 4 + col}.", chips[layer][col])
    lines = kn_header("一个 EP 组 8 颗 chip：两行各进本行 R core，两个 R core 串链",
                      ["group 0", "chips 0,1,2,3,4,5,6,7"] + lines +
                      [f"row{r} {hbytes(rows[r])}" for r in range(2)] +
                      [f"rcore{r} {hbytes(rcores[r])}" for r in range(2)] +
                      [f"out {hbytes(rcores[-1])}"])
    write(path, lines)


# 一个 LPU：48 颗 chip 摆成 12 层 × 4 列，两层一个 EP 组，共 6 组、12 行。
MOE_LPU_GROUPS = 6


def write_moe_lpu(path):
    """48 颗 chip、6 个 EP 组、12 个 R core 的那一段。

    每行 4 颗 chip 的结果进本行 R core，12 个 R core 逐行相加，第 11 行 R core 的
    结果出核。每颗 chip 只留 concat，每行留行链结果与 R core 的结果。
    """
    chips, rows, rcores = kn_rows(kn_token(), MOE_LPU_GROUPS)
    lines = []
    for r, ones in enumerate(chips):
        for col, one in enumerate(ones):
            lines.append(f"concat{r * 4 + col} {hbytes(one['concat'])}")
    lines = kn_header("48 颗 chip、12 行的 KN 那一段：12 个 R core 逐行相加",
                      [f"groups {MOE_LPU_GROUPS}"] + lines +
                      [f"row{r} {hbytes(rows[r])}" for r in range(len(rows))] +
                      [f"rcore{r} {hbytes(rcores[r])}"
                       for r in range(len(rcores))] +
                      [f"out {hbytes(rcores[-1])}"])
    write(path, lines)


# 同一个 LPU 上连续跑的那一份有几个 token。
MOE_LPU_TOKENS = 32


def kn_lpu_out(k):
    """第 k 个 token 走完 48 颗 chip、从第 11 行 R core 出来的结果。"""
    return kn_rows(kn_token(k), MOE_LPU_GROUPS)[2][-1]


def moe_lpu_tokens_lines(outs):
    """连续跑那一份的全部行：outs 是各 token 出口上的结果，按序号排。"""
    return kn_header("48 颗 chip 上连续跑 32 个 token：每个 token 出口上的结果",
                     [f"groups {MOE_LPU_GROUPS}", f"tokens {len(outs)}"] +
                     [f"out{k} {hbytes(o)}" for k, o in enumerate(outs)])


def write_moe_lpu_tokens(path):
    """48 颗 chip 上连续跑 32 个 token，各 token 内容不同、topK 相同，每个只留
    出口上的结果。"""
    outs = [kn_lpu_out(k) for k in range(MOE_LPU_TOKENS)]
    write(path, moe_lpu_tokens_lines(outs))


def write(path, lines):
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"  {os.path.basename(path):<12} {len(lines) - 1} 条")


# 一次要跑很久的那几份。一颗 chip 的参考实现要算几秒，8 颗与 48 颗那三份自检默认
# 跳过：同一段代码在 moe_chip.txt 与 moe_two_groups.txt 上已经查过，跳的只是这三份
# 的新鲜度。
SLOW = ("moe_group.txt", "moe_lpu.txt", "moe_lpu_tokens.txt")


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
    write_moe_two_groups(os.path.join(OUT_DIR, "moe_two_groups.txt"))
    if not skip_slow:
        write_moe_group(os.path.join(OUT_DIR, "moe_group.txt"))
        write_moe_lpu(os.path.join(OUT_DIR, "moe_lpu.txt"))
        write_moe_lpu_tokens(os.path.join(OUT_DIR, "moe_lpu_tokens.txt"))


if __name__ == "__main__":
    main()
