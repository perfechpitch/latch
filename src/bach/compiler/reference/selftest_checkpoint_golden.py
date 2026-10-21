"""Numpy-only selftest for the full-global golden.

No pytest, no BachFlow importer, no real safetensors. The checkpoint double
below implements ``info`` and ``raw`` and nothing else. A missing check prints
a line and exits 1.
"""

from __future__ import annotations

import sys

import numpy as np

import checkpoint_golden as g

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)


def check_codecs():
    codes = np.arange(256, dtype=np.uint8)
    vec = g.decode_e4m3_vector(codes)
    for c in range(256):
        s = g.e4m3_decode(int(c))
        v = float(vec[c])
        if s != s:
            check(v != v, f"E4M3 0x{c:02x} vector decode is not NaN")
        else:
            check(s == v, f"E4M3 0x{c:02x} scalar {s} != vector {v}")
            if c not in (0x7F, 0xFF):
                check(g.e4m3_encode(s) == c, f"E4M3 0x{c:02x} did not round-trip")
    svec = g.decode_e8m0_vector(codes)
    for c in range(256):
        s = g.e8m0_decode(int(c))
        v = float(svec[c])
        if s != s:
            check(v != v, f"E8M0 0x{c:02x} vector decode is not NaN")
        else:
            check(s == v, f"E8M0 0x{c:02x} scalar {s} != vector {v}")
    check(g.e4m3_decode(0x7E) == 448.0, "E4M3 max finite is not 448")
    check(g.e4m3_encode(1e30) == 0x7E, "E4M3 overflow did not clamp")
    raw = np.array([0x01, 0x02, 0x03, 0x04], dtype=np.uint8)
    got = list(g.unpack_u32_e4m3(raw.reshape(1, 4)).reshape(-1))
    check(got == [0x01, 0x02, 0x03, 0x04], f"U32 pack order is {got}")


def check_token():
    blob = g.deterministic_token(7)
    check(blob == g.deterministic_token(7), "token seed is not stable")
    check(blob != g.deterministic_token(8), "two seeds produced one token")
    check(len(blob) == g.TOKEN_BYTES, f"token is {len(blob)} bytes")
    tok = g.parse_token(blob)
    check(tok.count == 8 and len(tok.ids) == 8 and len(tok.weights) == 8, "header width")
    codes = np.frombuffer(blob, dtype=np.uint8, count=6144, offset=56)
    scales = np.frombuffer(blob, dtype=np.uint8, count=192, offset=56 + 6144)
    check(scales.tolist() == [127] * 192, "scales are not the constant code 127")
    check(int(np.max(np.abs(tok.values))) <= 2, "activation left the 0.1..2 band")
    check(0x80 not in codes, "token contains an E4M3 signed zero")
    check(float(np.linalg.norm(tok.values)) > 0.0, "token activation has norm 0")
    payload = bytes(codes)
    check(payload[:32] * (6144 // 32) != payload, "token codes repeat every 32 bytes")


def _names(logical, kind):
    stem = g.MATRICES[logical].format(layer=3, kind=kind)
    return stem + ".weight", stem + ".scales"


def _pack(codes):
    n, k = codes.shape
    grouped = codes.reshape(n, k // 4, 4).astype(np.uint32)
    words = (grouped[..., 0] | (grouped[..., 1] << 8)
             | (grouped[..., 2] << 16) | (grouped[..., 3] << 24))
    return np.ascontiguousarray(words.view(np.uint8).reshape(n, k))


class _Tracking(np.ndarray):
    """Records every integer expert index, including across copies of the view."""

    def __new__(cls, arr, log, name):
        obj = np.asarray(arr).view(cls)
        obj._log = log
        obj._name = name
        return obj

    def __array_finalize__(self, obj):
        if obj is None:
            return
        self._log = getattr(obj, "_log", None)
        self._name = getattr(obj, "_name", None)

    def __getitem__(self, key):
        if isinstance(key, int) and self._log is not None:
            self._log.setdefault(self._name, []).append(key)
        return np.asarray(super().__getitem__(key))


class _Info:
    def __init__(self, name, dtype, shape):
        self.name = name
        self.dtype = dtype
        self.shape = shape


class FakeCheckpoint:
    """info/raw only. One shared expert, eight routed, hidden 64, inter 32."""

    def __init__(self):
        self.touched = {}
        self.hidden = 64
        self.inter = 32
        rng = np.random.Generator(np.random.PCG64(99))
        self._w = {}
        self._s = {}
        self._build("shared_experts", None, rng)
        for e in range(8):
            self._build("switch_mlp", e, rng)

    def _build(self, kind, expert, rng):
        tag = 0 if expert is None else expert + 1
        for logical, n, k in (
            ("W_gate", self.inter, self.hidden),
            ("W_up", self.inter, self.hidden),
            ("W_down", self.hidden, self.inter),
        ):
            codes = rng.integers(0, 120, size=(n, k), dtype=np.uint32).astype(np.uint8)
            codes[0, :] = np.uint8(0x38)
            scales = np.full((n, k // g.GROUP), 127, dtype=np.uint8)
            scales[0, 0] = np.uint8(127 + tag + {"W_gate": 1, "W_up": 2, "W_down": 3}[logical])
            nw, ns = _names(logical, kind)
            packed = _pack(codes)
            if expert is None:
                self._w[nw] = packed
                self._s[ns] = scales
            else:
                self._w.setdefault(nw, []).append(packed)
                self._s.setdefault(ns, []).append(scales)

    def info(self, name):
        if name in self._w:
            arr = self._w[name]
            raw_shape = ((len(arr),) + arr[0].shape) if isinstance(arr, list) else arr.shape
            return _Info(name, "U32", raw_shape[:-1] + (raw_shape[-1] // 4,))
        arr = self._s[name]
        shape = ((len(arr),) + arr[0].shape) if isinstance(arr, list) else arr.shape
        return _Info(name, "U8", shape)

    def raw(self, name):
        if name in self._w:
            arr = self._w[name]
            if isinstance(arr, list):
                return _Tracking(np.stack(arr, axis=0), self.touched, name)
            return arr
        arr = self._s[name]
        if isinstance(arr, list):
            return _Tracking(np.stack(arr, axis=0).astype(np.uint8), self.touched, name)
        return np.ascontiguousarray(arr, dtype=np.uint8)


def check_forward():
    ckpt = FakeCheckpoint()
    shapes = g.Ep1Shapes(hidden=64, inter=32, n_routed=8, n_shared=1, n_selected=8, group=32)
    blob = g.pack_token(list(range(8)), [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8],
                       np.full(6144, 0x38, dtype=np.uint8),
                       np.full(192, 127, dtype=np.uint8))
    tok = g.parse_token(blob)
    x = np.linspace(-1.0, 1.0, 64)
    result = g.forward_token(ckpt, blob, layer=3, shapes=shapes, activation=x)
    touched_once = {name: list(idx) for name, idx in ckpt.touched.items()}
    check(result["y"].shape == (9, 64), f"y shape {result['y'].shape}")
    check(result["gate"].shape == (9, 32), "gate shape")
    check(result["sum"].shape == (64,), "sum shape")
    check(np.allclose(result["sum"], result["y"].sum(axis=0)), "sum is not the nine rows")
    for name, touched in touched_once.items():
        if "switch_mlp" not in name:
            continue
        check(touched == [int(i) for i in tok.ids],
              f"{name} indexed {touched}, want {list(tok.ids)}")
    for i in range(9):
        check(np.isfinite(result["y"][i]).all(), f"expert {i} is not finite")
        for j in range(i):
            check(not np.allclose(result["y"][i], result["y"][j]),
                  f"expert {i} copies expert {j}")
    check(not np.allclose(result["y"][1], result["y"][0] * 0), "routed row is empty")
    # Coefficient is the serialized weight. Forcing every weight to 1 changes the sum.
    ones = g.pack_token(list(range(8)), [1.0] * 8,
                        np.full(6144, 0x38, dtype=np.uint8),
                        np.full(192, 127, dtype=np.uint8))
    plain = g.forward_token(FakeCheckpoint(), ones, layer=3, shapes=shapes, activation=x)
    check(not np.allclose(result["sum"], plain["sum"]), "serialized weights were ignored")
    check(np.allclose(result["y"][0], plain["y"][0]), "shared expert took a routed weight")


def check_budgets():
    check(g.REL_L2 == 0.15 and g.NORM_RATIO_MIN == 0.85 and g.NORM_RATIO_MAX == 1.15
          and g.COSINE_MIN == 0.99, "budgets moved off the declared window")
    rng = np.random.Generator(np.random.PCG64(5))
    x = rng.normal(size=8)
    wg, wu, wd = rng.normal(size=(4, 8)), rng.normal(size=(4, 8)), rng.normal(size=(8, 4))
    once = g.expert_forward(x, wg, wu, wd, 0.5)
    twice = g.expert_forward(x, wg, wu, wd, 1.0)
    report = g.compare(once["y"], twice["y"])
    check(abs(report["cosine"] - 1.0) < 1e-9, f"doubled cosine is {report['cosine']}")
    check(report["ok"] is False, "doubling passed the declared budgets")
    check("max_abs" in report and report["max_abs"] > 0.0, "max_abs missing on a mismatch")
    same = g.compare(once["y"], once["y"].copy())
    check(same["ok"] is True and same["rel_l2"] == 0.0, "identical vector was rejected")
    zero = g.compare(np.zeros(4), np.ones(4))
    check(zero["ok"] is False and zero["cosine"] == 0.0, "zero reference accepted a nonzero vector")
    # The window is provisional. Do not treat an fp32/fp64 gap as evidence for it.
    check(g.REL_L2 == 0.15, "provisional rel L2 moved")


def check_silu():
    z = np.array([-1000.0, 0.0, 1000.0])
    got = g.silu(z)
    check(np.isfinite(got).all(), "silu overflowed")
    check(abs(got[0]) < 1e-300, "silu(-1000) is not ~0")
    check(abs(got[2] - 1000.0) < 1e-6, "silu(1000) is not ~1000")


def main():
    check_codecs()
    check_token()
    check_forward()
    check_budgets()
    check_silu()
    for msg in FAILS:
        print(f"  fail  {msg}")
    if FAILS:
        print(f"{len(FAILS)} failed")
        return 1
    print("checkpoint golden selftest passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
