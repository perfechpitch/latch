"""KN 拆分那一段在 GPU 上的实现：本机既装了 CUDA、卡的算力又在当前 torch 编译
支持的范围内时，`vectors.py` 用它，结果与纯 Python 那一份逐 bit 相同。判断条件
见 `available()`——只看 `cuda.is_available()` 会在「有卡但这个 torch 跑不动它」
的机器上误判。

与纯 Python 那一份是同一套运算，只是把逐元素的循环换成张量上的逐元素运算，一批
token 一起算。逐 bit 相同靠四条：

* 每一步都是单精度的一次运算：乘、加各是一个张量运算，得出来就是 IEEE 单精度
  舍入后的值，与纯 Python 先按双精度算再截回 FP32 相同（单精度的加减乘在双精度
  里算完再截，只舍入一次的效果）。不用矩阵乘库：它会改累加顺序，还可能走 TF32
  或乘加融合
* 累加顺序照搬：块内 32 个乘积从 +0 起顺序加，块间顺序加，tile 之间顺序加，每
  加一次 Clamp 一次，专家之间按顺序合并
* 格式编解码查表：MXFP8 元素与 E8M0 scale 各 256 项，表由 `numeric_ref` 算出；
  BF16 的编码用整数运算照 `numeric_ref.to_bf16` 做
* 门控里的 sigmoid 要调 libm 的 `expf`，GPU 上没有同一个函数，这一步连同量化
  回 CPU，直接调 `vectors.kn_gate`

权重与 token 无关，每组只生成、解码一次，留在显存里；线性同余在 CPU 上用 numpy
按 tile 并行地跑。
"""

import numpy as np
import torch

import numeric_ref as n

F32_MAX_BITS = n.F32_MAX


def available():
    """本机能不能用 GPU 算这一段。

    光看 `cuda.is_available()` 不够：卡在、torch 也带 CUDA，但卡的算力不在这个
    torch 编译支持的列表里时，kernel 根本编不出来，一调就报
    `CUDA error: no kernel image is available for execution on the device`。
    所以还要把算力对上 `get_arch_list()`，对不上就回退纯 Python。
    """
    if not torch.cuda.is_available():
        return False
    tag = "sm_%d%d" % torch.cuda.get_device_capability(0)
    return any(a.startswith(tag) for a in torch.cuda.get_arch_list())


# ── 格式 ──

def e4m3_table():
    return torch.tensor([n.from_fp8_e4m3(c) for c in range(256)],
                        dtype=torch.float32)


def e8m0_table():
    return torch.tensor([n.from_e8m0(c) for c in range(256)],
                        dtype=torch.float32)


def clamp(x):
    """numeric_ref.clamp_nan_inf：NaN 与 Inf 换成带原符号的最大有限值。"""
    bad = torch.isnan(x) | torch.isinf(x)
    if not bool(bad.any()):
        return x
    bits = x.view(torch.int32)
    sign = bits & torch.tensor(-0x80000000, dtype=torch.int32, device=x.device)
    big = (sign | F32_MAX_BITS).view(torch.float32)
    return torch.where(bad, big, x)


def to_bf16(x):
    """numeric_ref.to_bf16：RNE，NaN 截断。返回 int32 张量，低 16 位是编码。"""
    b = x.view(torch.int32).to(torch.int64) & 0xFFFFFFFF
    lsb = (b >> 16) & 1
    r = ((b + 0x7FFF + lsb) >> 16) & 0xFFFF
    nan = torch.isnan(x)
    return torch.where(nan, b >> 16, r).to(torch.int32)


def from_bf16(h):
    """BF16 编码（int 张量）解成 FP32。"""
    b = (h.to(torch.int64) & 0xFFFF) << 16
    # 高位是 1 的换成对应的负数再转 int32，不靠越界转换的截断行为
    b = torch.where(b >= 0x80000000, b - 0x100000000, b)
    return b.to(torch.int32).view(torch.float32)


def bf16_bytes(h):
    """一行 BF16 编码转成小端字节。"""
    a = h.to(torch.int64).cpu().numpy().astype(np.uint16)
    return a.astype("<u2").tobytes()


def bf16_of_bytes(data, device):
    a = np.frombuffer(data, dtype="<u2").astype(np.int64)
    return torch.from_numpy(a).to(device)


# ── 权重 ──

LCG_MUL = np.uint64(1103515245)
LCG_ADD = np.uint64(12345)


def lcg_bytes(seeds, count):
    """vectors.pattern 对一组种子各跑 count 步：返回 [len(seeds), count] 的字节。"""
    s = np.array(seeds, dtype=np.uint64)
    out = np.empty((len(seeds), count), dtype=np.uint8)
    with np.errstate(over="ignore"):
        for i in range(count):
            s = s * LCG_MUL + LCG_ADD
            out[:, i] = ((s >> np.uint64(16)) & np.uint64(0xFF)).astype(np.uint8)
    return out


def matrix_codes(mats):
    """几个 KnMatrix 一起生成：返回每个的 (数据编码 [rows, cols], scale 编码
    [rows / 32, cols])，与 KnMatrix.tile 逐字节相同。"""
    seeds, sc_seeds, place = [], [], []
    for m, mat in enumerate(mats):
        tiles_k = mat.rows // mat.tile_k
        tiles_n = mat.cols // mat.tile_n
        for tn in range(tiles_n):
            for tk in range(tiles_k):
                tid = tn * tiles_k + tk
                seeds.append(mat.seed + tid)
                sc_seeds.append((mat.seed + tid) ^ 0x5CA1E)
                place.append((m, tk, tn))
    mat0 = mats[0]
    count = mat0.tile_k * mat0.tile_n
    sc_count = mat0.tile_n * (mat0.tile_k // 32)
    data = lcg_bytes(seeds, count)
    # kn_data：抹掉 NaN 编码
    nan = (data & 0x7F) == 0x7F
    data = np.where(nan, data & 0xFE, data).astype(np.uint8)
    scale = (113 + lcg_bytes(sc_seeds, sc_count) % 8).astype(np.uint8)

    out = []
    for mat in mats:
        out.append((np.empty((mat.rows, mat.cols), dtype=np.uint8),
                    np.empty((mat.rows // 32, mat.cols), dtype=np.uint8)))
    tk_n, tn_n = mat0.tile_k, mat0.tile_n
    for idx, (m, tk, tn) in enumerate(place):
        d, s = out[m]
        # tile 里列优先：一列 tile_k 个元素连着，scale 每列 tile_k / 32 个
        d[tk * tk_n:(tk + 1) * tk_n, tn * tn_n:(tn + 1) * tn_n] = \
            data[idx].reshape(tn_n, tk_n).T
        s[tk * (tk_n // 32):(tk + 1) * (tk_n // 32),
          tn * tn_n:(tn + 1) * tn_n] = \
            scale[idx].reshape(tn_n, tk_n // 32).T
    return out


class KnGpu:
    """一组 KN 权重解码后留在显存里，按组懒加载。"""

    def __init__(self, v, device="cuda"):
        self.v = v                 # vectors 模块：常量、KnMatrix、kn_gate
        self.dev = torch.device(device)
        self.e4m3 = e4m3_table().to(self.dev)
        self.e8m0 = e8m0_table().to(self.dev)
        self.groups = {}

    def weights(self, g):
        """第 g 组：{"w1": [专家], "w3": [...], "w2": [...]}，每项 (值, scale)。"""
        if g in self.groups:
            return self.groups[g]
        v = self.v
        mats = []
        for e in range(v.KN_EXPERTS):
            mats += [v.kn_w13(v.KN_W1, g, e), v.kn_w13(v.KN_W3, g, e)]
        w13 = matrix_codes(mats)
        w2 = matrix_codes([v.kn_w2(g, e) for e in range(v.KN_EXPERTS)])
        dec = self.decode_pair
        one = {"w1": [dec(w13[2 * e]) for e in range(v.KN_EXPERTS)],
               "w3": [dec(w13[2 * e + 1]) for e in range(v.KN_EXPERTS)],
               "w2": [dec(w2[e]) for e in range(v.KN_EXPERTS)]}
        self.groups[g] = one
        return one

    def decode_pair(self, codes):
        d, s = codes
        dt = torch.from_numpy(d.astype(np.int64)).to(self.dev)
        st = torch.from_numpy(s.astype(np.int64)).to(self.dev)
        return self.e4m3[dt], self.e8m0[st]

    def token_vals(self, tokens):
        """一批 (数据, scale) 字节解成 [T, K] 的元素与 [T, K / 32] 的 scale。"""
        d = np.stack([np.frombuffer(t[0], dtype=np.uint8) for t in tokens])
        s = np.stack([np.frombuffer(t[1], dtype=np.uint8) for t in tokens])
        dt = torch.from_numpy(d.astype(np.int64)).to(self.dev)
        st = torch.from_numpy(s.astype(np.int64)).to(self.dev)
        return self.e4m3[dt], self.e8m0[st]

    # ── MU 的一片：块内、块间、tile 间逐级顺序累加 ──

    def mu_accum(self, a, asc, w, wsc, rows_of, tile_k, kblock):
        """一个专家、一批 core 的 kn_blocked 累加。

        a [T, C, K]、asc [T, C, K / 32] 是每个 core 自己那一段输入；w [R, N]、
        wsc [R / 32, N] 是完整矩阵，rows_of(c) 给出第 c 个 core 从哪一行起；返回
        [T, C, N] 上每个 core 对每一列的结果（列按完整矩阵编号，调用方取自己那
        一片）。
        """
        T, C, _ = a.shape
        N = w.shape[1]
        base = torch.tensor([rows_of(c) for c in range(C)], device=self.dev)
        acc = None
        for ki in range(kblock):
            total = torch.zeros((T, C, N), dtype=torch.float32, device=self.dev)
            for b in range(tile_k // 32):
                part = torch.zeros((T, C, N), dtype=torch.float32,
                                   device=self.dev)
                for i in range(32):
                    kk = ki * tile_k + b * 32 + i
                    wrow = w[base + kk]                     # [C, N]
                    part = part + a[:, :, kk:kk + 1] * wrow
                blk = ki * tile_k // 32 + b
                s = asc[:, :, blk:blk + 1] * wsc[(base + ki * tile_k + b * 32)
                                                 // 32]
                total = total + part * s
            r = clamp(total)
            acc = r if acc is None else clamp(acc + r)
        return acc

    # ── 一组 8 颗 chip ──

    def chips(self, tokens, g, keep=True):
        """一批 token 在第 g 组 8 颗 chip 上走完：返回每个 token 每颗 chip 的中间
        量（与 vectors.kn_chip 同形状的 dict，字节）与 concat 的 BF16 编码
        [T, 8, KN_EMBED]。keep 为假时 dict 里只留 concat。"""
        v = self.v
        W = self.weights(g)
        T = len(tokens)
        C = v.KN_SLOTS
        seg_e, seg_i = v.KN_SEG_EMBED, v.KN_SEG_INTER
        tok, tsc = self.token_vals(tokens)            # [T, 6144], [T, 192]
        # FC1、FC3：第 s 个槽位读 token 第 s 段，各 chip 分列
        a = tok.view(T, 1, v.KN_EMBED).expand(T, C, v.KN_EMBED)
        a = torch.stack([a[:, s, s * seg_e:(s + 1) * seg_e]
                         for s in range(C)], dim=1)    # [T, 8 槽位, 768]
        asc = torch.stack([tsc[:, s * seg_e // 32:(s + 1) * seg_e // 32]
                           for s in range(C)], dim=1)
        parts13 = []                                  # 顺序 W1 e0、W1 e1、W3 e0、W3 e1
        for key in ("w1", "w3"):
            for e in range(v.KN_EXPERTS):
                w, wsc = W[key][e]
                acc = self.mu_accum(a, asc, w, wsc, lambda s: s * seg_e,
                                    v.FC13_K, seg_e // v.FC13_K)
                parts13.append(to_bf16(acc))           # [T, 8 槽位, 2048]
        # 一个 core（chip c、槽位 s）的部分和：四段各取第 c 片 256 列
        part = torch.stack([torch.cat([p[:, :, c * seg_i:(c + 1) * seg_i]
                                       for p in parts13], dim=2)
                            for c in range(C)], dim=1)  # [T, chip, 槽位, 1024]
        # chip 内归约链
        chain = v.KN_CHIP_CHAIN
        red = part[:, :, chain[0]]
        for s in chain[1:]:
            red = to_bf16(clamp(from_bf16(red) + from_bf16(part[:, :, s])))
        # 门控回 CPU
        acts = []
        for t in range(T):
            row = []
            for c in range(C):
                row.append(v.kn_gate(bf16_bytes(red[t, c])))
            acts.append(row)
        # FC2：第 c 颗 chip 读自己那一份激活，各槽位分列
        ad = np.zeros((T, C, v.KN_EXPERTS, seg_i), dtype=np.int64)
        asd = np.zeros((T, C, v.KN_EXPERTS, seg_i // 32), dtype=np.int64)
        for t in range(T):
            for c in range(C):
                for e in range(v.KN_EXPERTS):
                    ad[t, c, e] = np.frombuffer(acts[t][c][e][0], dtype=np.uint8)
                    asd[t, c, e] = np.frombuffer(acts[t][c][e][1],
                                                 dtype=np.uint8)
        av = self.e4m3[torch.from_numpy(ad).to(self.dev)]
        asv = self.e8m0[torch.from_numpy(asd).to(self.dev)]
        ep = None
        for e in range(v.KN_EXPERTS):
            w, wsc = W["w2"][e]
            acc = self.mu_accum(av[:, :, e], asv[:, :, e], w, wsc,
                                lambda c: c * seg_i, v.FC2_K,
                                seg_i // v.FC2_K)
            wep = torch.tensor(n.f32(v.KN_W_EP[e]), dtype=torch.float32,
                               device=self.dev)
            one = clamp(acc * wep)
            ep = one if ep is None else clamp(ep + one)
        concat = to_bf16(ep)                           # [T, chip, 6144]
        out = []
        for t in range(T):
            row = []
            for c in range(C):
                one = {"concat": bf16_bytes(concat[t, c])}
                if keep:
                    one["parts"] = [bf16_bytes(part[t, c, s]) for s in range(C)]
                    one["red"] = bf16_bytes(red[t, c])
                    one["act"] = acts[t][c]
                    one["fc2"] = [bf16_bytes(concat[t, c, s * seg_e:
                                                    (s + 1) * seg_e])
                                  for s in range(C)]
                row.append(one)
            out.append(row)
        return out, concat

    def rows(self, tokens, groups, keep=True):
        """vectors.kn_rows 的批量版：返回每个 token 的 (chips, rows, rcores)。"""
        v = self.v
        T = len(tokens)
        chips = [[] for _ in range(T)]
        rows = [[] for _ in range(T)]
        rcores = [[] for _ in range(T)]
        prev = None
        for g in range(groups):
            one, concat = self.chips(tokens, g, keep)
            for layer in range(2):
                cols = range(layer * 4, layer * 4 + 4)
                row = concat[:, cols[0]]
                for c in list(cols)[1:]:
                    row = to_bf16(clamp(from_bf16(row) + from_bf16(concat[:, c])))
                # R core：链首那一行只有本行一份，原样出去；其余行与上一行相加
                prev = row if prev is None else \
                    to_bf16(clamp(from_bf16(row) + from_bf16(prev)))
                for t in range(T):
                    chips[t].append([one[t][c] for c in cols])
                    rows[t].append(bf16_bytes(row[t]))
                    rcores[t].append(bf16_bytes(prev[t]))
        return [(chips[t], rows[t], rcores[t]) for t in range(T)]
