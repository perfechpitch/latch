"""KN 拆分那一段在 CPU 上的实现：没有 CUDA 时 `vectors.py` 用它。

与 `kn_gpu.py` 同一套运算，张量放在 NumPy 的 float32 上。逐 bit 相同靠同样的四条：
每一步是单精度的一次乘或加，不用矩阵乘；块内 32 个乘积从 0 起顺序加，块间、
tile 间顺序加，每加一次 Clamp 一次；MXFP8 与 E8M0 查 `numeric_ref` 算出的表；
门控的 sigmoid 仍调 `vectors.kn_gate`。

权重与 token 无关，每组只生成、解码一次。
"""

import numpy as np

import numeric_ref as n

F32_MAX_BITS = np.int32(n.F32_MAX)
SIGN_BIT = np.int32(-0x80000000)

LCG_MUL = np.uint64(1103515245)
LCG_ADD = np.uint64(12345)


def clamp(x):
    """numeric_ref.clamp_nan_inf：NaN 与 Inf 换成带原符号的最大有限值。"""
    x = np.ascontiguousarray(x, dtype=np.float32)
    bad = np.isnan(x) | np.isinf(x)
    if not bool(bad.any()):
        return x
    bits = x.view(np.int32)
    sign = bits & SIGN_BIT
    big = (sign | F32_MAX_BITS).view(np.float32)
    return np.where(bad, big, x)


def to_bf16(x):
    """numeric_ref.to_bf16：RNE，NaN 截断。返回 int32，低 16 位是编码。"""
    x = np.ascontiguousarray(x, dtype=np.float32)
    b = x.view(np.int32).astype(np.int64) & 0xFFFFFFFF
    lsb = (b >> 16) & 1
    r = ((b + 0x7FFF + lsb) >> 16) & 0xFFFF
    nan = np.isnan(x)
    return np.where(nan, b >> 16, r).astype(np.int32)


def from_bf16(h):
    """BF16 编码（int 数组）解成 FP32。"""
    b = (np.ascontiguousarray(h).astype(np.int64) & 0xFFFF) << 16
    b = np.where(b >= 0x80000000, b - 0x100000000, b)
    return np.ascontiguousarray(b, dtype=np.int32).view(np.float32)


def bf16_bytes(h):
    """一行 BF16 编码转成小端字节。"""
    a = np.ascontiguousarray(h).astype(np.uint16)
    return a.astype("<u2").tobytes()


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
        d[tk * tk_n:(tk + 1) * tk_n, tn * tn_n:(tn + 1) * tn_n] = \
            data[idx].reshape(tn_n, tk_n).T
        s[tk * (tk_n // 32):(tk + 1) * (tk_n // 32),
          tn * tn_n:(tn + 1) * tn_n] = \
            scale[idx].reshape(tn_n, tk_n // 32).T
    return out


class KnCpu:
    """一组 KN 权重解码后留在内存里，按组懒加载。"""

    def __init__(self, v):
        self.v = v
        self.e4m3 = np.array([n.from_fp8_e4m3(c) for c in range(256)],
                             dtype=np.float32)
        self.e8m0 = np.array([n.from_e8m0(c) for c in range(256)],
                             dtype=np.float32)
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
        return self.e4m3[d.astype(np.int64)], self.e8m0[s.astype(np.int64)]

    def token_vals(self, tokens):
        """一批 (数据, scale) 字节解成 [T, K] 的元素与 [T, K / 32] 的 scale。"""
        d = np.stack([np.frombuffer(t[0], dtype=np.uint8) for t in tokens])
        s = np.stack([np.frombuffer(t[1], dtype=np.uint8) for t in tokens])
        return self.e4m3[d.astype(np.int64)], self.e8m0[s.astype(np.int64)]

    def mu_accum(self, a, asc, w, wsc, rows_of, tile_k, kblock):
        """一个专家、一批 core 的 kn_blocked 累加。

        a [T, C, K]、asc [T, C, K / 32] 是每个 core 自己那一段输入；w [R, N]、
        wsc [R / 32, N] 是完整矩阵，rows_of(c) 给出第 c 个 core 从哪一行起；返回
        [T, C, N] 上每个 core 对每一列的结果。
        """
        t_n, c_n, _ = a.shape
        n_n = w.shape[1]
        base = np.array([rows_of(c) for c in range(c_n)], dtype=np.int64)
        acc = None
        for ki in range(kblock):
            total = np.zeros((t_n, c_n, n_n), dtype=np.float32)
            for b in range(tile_k // 32):
                part = np.zeros((t_n, c_n, n_n), dtype=np.float32)
                for i in range(32):
                    kk = ki * tile_k + b * 32 + i
                    wrow = w[base + kk]
                    part = part + a[:, :, kk:kk + 1] * wrow
                blk = ki * tile_k // 32 + b
                s = asc[:, :, blk:blk + 1] * wsc[(base + ki * tile_k + b * 32)
                                                 // 32]
                total = total + part * s
            r = clamp(total)
            acc = r if acc is None else clamp(acc + r)
        return acc

    def chips(self, tokens, g, keep=True):
        """一批 token 在第 g 组 8 颗 chip 上走完。返回值与 KnGpu.chips 相同。"""
        v = self.v
        wts = self.weights(g)
        t_n = len(tokens)
        c_n = v.KN_SLOTS
        seg_e, seg_i = v.KN_SEG_EMBED, v.KN_SEG_INTER
        tok, tsc = self.token_vals(tokens)
        a = np.stack([tok[:, s * seg_e:(s + 1) * seg_e] for s in range(c_n)],
                     axis=1)
        asc = np.stack([tsc[:, s * seg_e // 32:(s + 1) * seg_e // 32]
                        for s in range(c_n)], axis=1)
        parts13 = []
        for key in ("w1", "w3"):
            for e in range(v.KN_EXPERTS):
                w, wsc = wts[key][e]
                acc = self.mu_accum(a, asc, w, wsc, lambda s: s * seg_e,
                                    v.FC13_K, seg_e // v.FC13_K)
                parts13.append(to_bf16(acc))
        part = np.stack([np.concatenate([p[:, :, c * seg_i:(c + 1) * seg_i]
                                         for p in parts13], axis=2)
                         for c in range(c_n)], axis=1)
        chain = v.KN_CHIP_CHAIN
        red = part[:, :, chain[0]]
        for s in chain[1:]:
            red = to_bf16(clamp(from_bf16(red) + from_bf16(part[:, :, s])))
        acts = []
        for t in range(t_n):
            row = []
            for c in range(c_n):
                row.append(v.kn_gate(bf16_bytes(red[t, c])))
            acts.append(row)
        ad = np.zeros((t_n, c_n, v.KN_EXPERTS, seg_i), dtype=np.int64)
        asd = np.zeros((t_n, c_n, v.KN_EXPERTS, seg_i // 32), dtype=np.int64)
        for t in range(t_n):
            for c in range(c_n):
                for e in range(v.KN_EXPERTS):
                    ad[t, c, e] = np.frombuffer(acts[t][c][e][0], dtype=np.uint8)
                    asd[t, c, e] = np.frombuffer(acts[t][c][e][1],
                                                 dtype=np.uint8)
        av = self.e4m3[ad]
        asv = self.e8m0[asd]
        ep = None
        for e in range(v.KN_EXPERTS):
            w, wsc = wts["w2"][e]
            acc = self.mu_accum(av[:, :, e], asv[:, :, e], w, wsc,
                                lambda c: c * seg_i, v.FC2_K,
                                seg_i // v.FC2_K)
            wep = np.float32(n.f32(v.KN_W_EP[e]))
            one = clamp(acc * wep)
            ep = one if ep is None else clamp(ep + one)
        concat = to_bf16(ep)
        out = []
        for t in range(t_n):
            row = []
            for c in range(c_n):
                one = {"concat": bf16_bytes(concat[t, c])}
                if keep:
                    one["parts"] = [bf16_bytes(part[t, c, s]) for s in range(c_n)]
                    one["red"] = bf16_bytes(red[t, c])
                    one["act"] = acts[t][c]
                    one["fc2"] = [bf16_bytes(concat[t, c, s * seg_e:
                                                    (s + 1) * seg_e])
                                  for s in range(c_n)]
                row.append(one)
            out.append(row)
        return out, concat

    def rows(self, tokens, groups, keep=True):
        """vectors.kn_rows 的批量版：返回每个 token 的 (chips, rows, rcores)。"""
        v = self.v
        t_n = len(tokens)
        chips = [[] for _ in range(t_n)]
        rows = [[] for _ in range(t_n)]
        rcores = [[] for _ in range(t_n)]
        prev = None
        for g in range(groups):
            one, concat = self.chips(tokens, g, keep)
            for layer in range(2):
                cols = range(layer * 4, layer * 4 + 4)
                row = concat[:, cols[0]]
                for c in list(cols)[1:]:
                    row = to_bf16(clamp(from_bf16(row) + from_bf16(concat[:, c])))
                prev = row if prev is None else \
                    to_bf16(clamp(from_bf16(row) + from_bf16(prev)))
                for t in range(t_n):
                    chips[t].append([one[t][c] for c in cols])
                    rows[t].append(bf16_bytes(row[t]))
                    rcores[t].append(bf16_bytes(prev[t]))
        return [(chips[t], rows[t], rcores[t]) for t in range(t_n)]
