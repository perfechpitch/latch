"""Focused tests for the full-global golden. No live checkpoint, no simulator.

The tiny checkpoint speaks only ``info`` and ``raw``, the two calls the golden
is allowed to make. Shapes are a scaled EP1: hidden 64, inter 32, group 32,
one shared expert and eight routed experts. The wire token keeps the real
6392-byte layout; the tiny math fixture explicitly supplies a 64-element input.
"""

from __future__ import annotations

import math
import struct
import sys
from pathlib import Path

import numpy as np
import pytest

HERE = Path(__file__).resolve()
SRC = HERE.parents[4]
sys.path.insert(0, str(SRC))

import bach.compiler.reference.checkpoint_golden as g  # noqa: E402


# ── codecs ───────────────────────────────────────────────────────────────────

def test_e4m3_known_codes():
    assert g.e4m3_decode(0x00) == 0.0
    assert g.e4m3_decode(0x80) == -0.0 and math_signbit(g.e4m3_decode(0x80))
    assert g.e4m3_decode(0x38) == 1.0          # exp 7, man 0 -> 2^(7-7) = 1
    assert g.e4m3_decode(0x7E) == 448.0
    assert g.e4m3_decode(0x01) == 2.0**-9      # smallest subnormal
    assert np.isnan(g.e4m3_decode(0x7F))
    assert np.isnan(g.e4m3_decode(0xFF))


def math_signbit(v: float) -> bool:
    return struct.pack("<d", float(v))[7] & 0x80 != 0 or struct.pack("<d", float(v)) == struct.pack("<d", -0.0)


def test_e4m3_not_periodic():
    """A short period would make a later block equal an earlier one for every code."""
    decoded = [g.e4m3_decode(c) for c in range(256)]
    finite = [v for v in decoded if v == v]
    # 256 codes, one NaN pattern twice (0x7F, 0xFF): 254 finite, many duplicates of
    # magnitude across the sign, but the sequence itself is not a repeat of a
    # prefix shorter than 128 (sign lives in the top bit, so period 128 is the
    # sign split — anything shorter is a real period).
    seq = bytes(range(256))
    for period in (1, 2, 3, 4, 8, 16, 32):
        assert seq[:period] * (256 // period) != seq
    values = np.array([0.0 if v != v else v for v in decoded])
    for period in (1, 2, 4, 8, 16):
        assert not np.array_equal(values[:period], values[period: 2 * period]) or period == 0


def test_scalar_matches_vector_codec():
    codes = np.arange(256, dtype=np.uint8)
    vec = g.decode_e4m3_vector(codes)
    for c in range(256):
        s = g.e4m3_decode(c)
        v = vec[c]
        if np.isnan(s):
            assert np.isnan(v)
        else:
            assert s == v
    scales = np.arange(256, dtype=np.uint8)
    svec = g.decode_e8m0_vector(scales)
    for c in range(256):
        s = g.e8m0_decode(c)
        v = svec[c]
        if np.isnan(s):
            assert np.isnan(v)
        else:
            assert s == v


def test_e4m3_roundtrip_finite_and_clamp():
    for c in range(256):
        v = g.e4m3_decode(c)
        if v != v:
            continue
        assert g.e4m3_encode(v) == c
    assert g.e4m3_encode(1e30) == 0x7E
    assert g.e4m3_encode(-1e30) == 0xFE
    assert g.e4m3_encode(0.0) == 0x00
    assert g.e4m3_encode(-0.0) == 0x80


def test_e4m3_subnormal_promotes_and_inf_clamps():
    # Eight subnormal quanta is the first normal, 2^-6, code 0x08. min(q, 7)
    # would have left this at 0x07.
    assert g.e4m3_encode(8 * 2.0**-9) == 0x08
    assert g.e4m3_encode(-(8 * 2.0**-9)) == 0x88
    assert g.e4m3_encode(math.inf) == 0x7E
    assert g.e4m3_encode(-math.inf) == 0xFE
    # Tie on a subnormal quantum: 1.5 quanta is equally far from 1 and 2, and
    # 2 is the even multiple.
    assert g.e4m3_encode(1.5 * 2.0**-9) == 0x02
    # Tie inside a normal bin: 1 + 0.5/8 = 1.0625 sits between mantissas 0 and 1
    # of exponent field 7 (code 0x38 is 1.0). Even mantissa wins: 0x38.
    assert g.e4m3_encode(1.0 + 0.5 / 8.0) == 0x38


def test_e8m0_code_zero_is_exact_power():
    assert g.e8m0_decode(0) == 2.0**-127
    assert g.decode_e8m0_vector(np.array([0], dtype=np.uint8))[0] == 2.0**-127
    assert np.isnan(g.e8m0_decode(0xFF))


def test_u32_pack_is_four_little_endian_bytes():
    # One word 0x04030201 -> bytes 01 02 03 04, elements in that order.
    raw = np.array([0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB, 0xCC, 0xDD], dtype=np.uint8)
    got = g.unpack_u32_e4m3(raw.reshape(1, 8))
    assert got.shape == (1, 8)
    assert list(got.reshape(-1)) == [0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB, 0xCC, 0xDD]
    # A big-endian reading would have reversed each group of four.
    assert list(got.reshape(-1)[:4]) != [0x04, 0x03, 0x02, 0x01]


def test_scale_groups_of_32_not_a_per_element_scale():
    codes = np.zeros(64, dtype=np.uint8)
    codes[:] = 0x38  # 1.0
    scales = np.array([128, 129], dtype=np.uint8)  # 2, 4
    elem = g.decode_e4m3_vector(codes)
    sc = g.decode_e8m0_vector(scales)
    values = elem * np.repeat(sc, g.GROUP)
    assert values.shape == (64,)
    assert np.all(values[:32] == 2.0)
    assert np.all(values[32:] == 4.0)
    # One scale covering all 64 would have made the two halves equal.
    assert values[0] != values[32]


# ── token ────────────────────────────────────────────────────────────────────

def test_token_layout_is_6392_and_roundtrips():
    rng = np.random.Generator(np.random.PCG64(1))
    ids = [3, 10, 11, 40, 41, 100, 200, 255]
    weights = [float(np.float32(w)) for w in (0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8)]
    codes = rng.integers(0, 0x7E, size=6144, dtype=np.uint32).astype(np.uint8)
    scales = rng.integers(100, 140, size=192, dtype=np.uint32).astype(np.uint8)
    blob = g.pack_token(ids, weights, codes, scales)
    assert len(blob) == 6392
    assert struct.unpack_from("<H", blob, 0)[0] == 8
    assert struct.unpack_from("<8H", blob, 2) == tuple(ids)
    got_w = struct.unpack_from("<8f", blob, 18)
    for a, b in zip(got_w, weights):
        assert a == pytest.approx(b, abs=0)
    assert blob[50:56] == b"\x00" * 6
    assert blob[56:56 + 6144] == codes.tobytes()
    assert blob[56 + 6144:] == scales.tobytes()
    tok = g.parse_token(blob)
    assert tok.ids == tuple(ids)
    assert tok.raw == blob
    # Decoded activation is code * its group scale, not the raw code.
    expect = g.decode_e4m3_vector(codes) * np.repeat(g.decode_e8m0_vector(scales), 32)
    assert np.array_equal(tok.values, expect, equal_nan=True)


def test_deterministic_token_scale_is_one_and_not_periodic():
    blob = g.deterministic_token(7)
    assert blob == g.deterministic_token(7)
    assert blob != g.deterministic_token(8)
    assert len(blob) == 6392
    codes = np.frombuffer(blob, dtype=np.uint8, count=6144, offset=56)
    scales = np.frombuffer(blob, dtype=np.uint8, count=192, offset=56 + 6144)
    assert scales.tolist() == [127] * 192
    assert 0x80 not in codes
    values = g.parse_token(blob).values
    assert np.linalg.norm(values) > 0.0
    assert np.max(np.abs(values)) <= 2.0
    payload = bytes(codes)
    for period in (1, 2, 4, 8, 16, 32, 64):
        assert payload[:period] * (6144 // period) != payload


# ── tiny checkpoint: 9 nonuniform experts, no full-tensor decode ─────────────

class TinyCheckpoint:
    """Fake checkpoint. ``info`` and ``raw`` only, no safetensors reader.

    hidden 64, inter 32, group 32. One shared expert and eight routed experts,
    each with a distinct first-group scale so a swapped expert changes ``y``.
    """

    def __init__(self):
        self.calls = []
        self.touched = {}  # tensor name -> expert indexes raw()[i] served
        self.n_routed = 4
        self.hidden = 64
        self.inter = 32
        rng = np.random.Generator(np.random.PCG64(99))
        self._weights = {}
        self._scales = {}
        self._build("shared_experts", None, rng)
        for e in range(self.n_routed):
            self._build("switch_mlp", e, rng)

    def _build(self, kind, expert, rng):
        # Distinct per expert: gate row 0 is (expert+1) along K, so the matvec
        # cannot hide a missing expert inside a uniform matrix.
        tag = 0 if expert is None else expert + 1
        for logical, n, k in (
            ("W_gate", self.inter, self.hidden),
            ("W_up", self.inter, self.hidden),
            ("W_down", self.hidden, self.inter),
        ):
            codes = rng.integers(0, 120, size=(n, k), dtype=np.uint32).astype(np.uint8)
            codes[0, :] = np.uint8(0x38)  # 1.0, so the first row is exactly the scale pattern
            # Make the first row's first group a unique power of two per (kind, expert, matrix).
            scales = np.full((n, k // g.GROUP), 127, dtype=np.uint8)  # 1.0
            scales[0, 0] = np.uint8(127 + tag + {"W_gate": 1, "W_up": 2, "W_down": 3}[logical])
            name_w, name_s = _names(logical, kind)
            packed = _pack(codes)
            if expert is None:
                self._weights[name_w] = packed
                self._scales[name_s] = scales
            else:
                self._weights.setdefault(name_w, []).append(packed)
                self._scales.setdefault(name_s, []).append(scales)

    def info(self, name):
        self.calls.append(("info", name))
        if name not in self._weights and name not in self._scales:
            raise KeyError(name)
        if name in self._weights:
            arr = self._weights[name]
            raw_shape = ((len(arr),) + arr[0].shape) if isinstance(arr, list) else arr.shape
            # info() reports the safetensors shape: U32 packs, four bytes each.
            return _Info(name, "U32", raw_shape[:-1] + (raw_shape[-1] // 4,))
        arr = self._scales[name]
        if isinstance(arr, list):
            shape = (len(arr),) + arr[0].shape
        else:
            shape = arr.shape
        return _Info(name, "U8", shape)

    def raw(self, name):
        self.calls.append(("raw", name))
        if name in self._weights:
            arr = self._weights[name]
            if isinstance(arr, list):
                return _Tracking(np.stack([_as_bytes(a) for a in arr], axis=0), self.touched, name)
            return _as_bytes(arr)
        arr = self._scales[name]
        if isinstance(arr, list):
            return _Tracking(np.stack(arr, axis=0).astype(np.uint8), self.touched, name)
        return np.ascontiguousarray(arr, dtype=np.uint8)


class _Info:
    def __init__(self, name, dtype, shape):
        self.name = name
        self.dtype = dtype
        self.shape = shape


class _Tracking(np.ndarray):
    """Counts every integer expert index, including after the array is copied."""

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


def _names(logical, kind):
    stem = g.MATRICES[logical].format(layer=3, kind=kind)
    return stem + ".weight", stem + ".scales"


def _pack(codes: np.ndarray) -> np.ndarray:
    """[N, K] E4M3 bytes -> [N, K/4] little-endian U32 stored as uint8 bytes."""
    n, k = codes.shape
    assert k % 4 == 0
    grouped = codes.reshape(n, k // 4, 4).astype(np.uint32)
    words = grouped[..., 0] | (grouped[..., 1] << 8) | (grouped[..., 2] << 16) | (grouped[..., 3] << 24)
    return words.view(np.uint8).reshape(n, k)  # Checkpoint.raw last axis is shape[-1] * 4, and shape[-1] is K/4, so last axis is K


def _as_bytes(words_as_uint8_rows: np.ndarray) -> np.ndarray:
    return np.ascontiguousarray(words_as_uint8_rows, dtype=np.uint8)


def test_forward_token_nine_matches_a_hand_sum(tmp_path):
    """Nine experts. The sum is a dot product written in this test, not y.sum()."""
    shapes = g.Ep1Shapes(hidden=64, inter=32, n_routed=8, n_shared=1, n_selected=8, group=32)
    weights = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8]
    blob = _token_with_weights(list(range(8)), weights)
    tok = g.parse_token(blob)
    x = np.linspace(-1.0, 1.0, 64)
    ckpt = _eight_expert_checkpoint()
    result = g.forward_token(ckpt, blob, layer=3, shapes=shapes, activation=x)
    touched_once = {name: list(idx) for name, idx in ckpt.touched.items()}
    raw_once = [name for kind, name in ckpt.calls if kind == "raw"]
    assert result["y"].shape == (9, 64)
    assert result["sum"].shape == (64,)
    assert list(result["ids"]) == list(tok.ids)
    expected = _hand_sum(ckpt, x, weights)
    assert np.allclose(result["sum"], expected)
    assert not np.allclose(result["sum"], _hand_sum(ckpt, x, [1.0] * 8))
    assert np.allclose(result["y"][0], _one_expert(ckpt, "shared_experts", None, x, 1.0))
    for slot, w in enumerate(weights):
        assert np.allclose(result["y"][slot + 1], _one_expert(ckpt, "switch_mlp", slot, x, w))
    for name, touched in touched_once.items():
        if "switch_mlp" not in name:
            continue
        assert touched == [int(i) for i in tok.ids]
    for logical in ("W_gate", "W_up", "W_down"):
        weight, scales = _names(logical, "shared_experts")
        assert raw_once.count(weight) == 1 and raw_once.count(scales) == 1
        rw, rs = _names(logical, "switch_mlp")
        assert raw_once.count(rw) == 8 and raw_once.count(rs) == 8
    cfg = {
        "hidden_size": 64, "moe_intermediate_size": 32, "n_routed_experts": 8,
        "n_shared_experts": 1, "num_experts_per_tok": 8,
        "routed_scaling_factor": 99.0,
        "quantization_config": {"group_size": 32, "mode": "mxfp8"},
    }
    other = g.Ep1Shapes.from_config(cfg)
    assert not hasattr(other, "routed_scaling")
    again = g.forward_token(_eight_expert_checkpoint(), blob, layer=3, shapes=other, activation=x)
    assert np.array_equal(again["sum"], result["sum"])
    path = tmp_path / "golden.npz"
    g.save_npz(str(path), result)
    loaded = np.load(path)
    assert loaded["sum"].shape == (64,) and "diag32_y" in loaded.files


def _decode_expert_raw(ckpt, kind, logical, expert, n, k):
    """Decode one matrix from info/raw bytes without load_expert_matrix.

    U32 little-endian packs and E8M0 groups of 32, written out here so the
    expected sum is not the function under test calling itself.
    """
    weight_name, scale_name = _names(logical, kind)
    wraw = ckpt.raw(weight_name)
    sraw = ckpt.raw(scale_name)
    if expert is not None:
        wraw = wraw[expert]
        sraw = sraw[expert]
    flat = np.ascontiguousarray(wraw, dtype=np.uint8).reshape(-1)
    words = flat.view(np.uint32)
    codes = np.stack((words & 0xFF, (words >> 8) & 0xFF, (words >> 16) & 0xFF, (words >> 24) & 0xFF), axis=-1)
    codes = codes.astype(np.uint8).reshape(n, k)
    elem = np.array([g.e4m3_decode(int(c)) for c in codes.reshape(-1)], dtype=np.float64).reshape(n, k)
    scales = np.array([g.e8m0_decode(int(c)) for c in np.asarray(sraw, dtype=np.uint8).reshape(-1)], dtype=np.float64)
    scales = scales.reshape(n, k // 32)
    return elem * np.repeat(scales, 32, axis=1)


def _one_expert(ckpt, kind, expert, x, coeff):
    gate = _decode_expert_raw(ckpt, kind, "W_gate", expert, 32, 64)
    up = _decode_expert_raw(ckpt, kind, "W_up", expert, 32, 64)
    down = _decode_expert_raw(ckpt, kind, "W_down", expert, 64, 32)
    gate_v = gate @ x
    up_v = up @ x
    # Stable silu, written here rather than calling g.silu.
    hidden = np.empty_like(gate_v)
    pos = gate_v >= 0
    hidden[pos] = gate_v[pos] / (1.0 + np.exp(-gate_v[pos]))
    ez = np.exp(gate_v[~pos])
    hidden[~pos] = gate_v[~pos] * ez / (1.0 + ez)
    hidden = hidden * up_v
    return coeff * (down @ hidden)


def _hand_sum(ckpt, x, weights):
    total = _one_expert(ckpt, "shared_experts", None, x, 1.0)
    for slot, w in enumerate(weights):
        total = total + _one_expert(ckpt, "switch_mlp", slot, x, w)
    return total


def _token_with_weights(ids, weights) -> bytes:
    codes = np.full(6144, 0x38, dtype=np.uint8)
    scales = np.full(192, 127, dtype=np.uint8)
    return g.pack_token(list(ids), list(weights), codes, scales)


def _eight_expert_checkpoint():
    ckpt = TinyCheckpoint()
    ckpt.n_routed = 8
    rng = np.random.Generator(np.random.PCG64(99))
    ckpt._weights.clear()
    ckpt._scales.clear()
    ckpt._build("shared_experts", None, rng)
    for e in range(8):
        ckpt._build("switch_mlp", e, rng)
    return ckpt


def test_shuffled_ids_match_checkpoint_experts_not_token_slots():
    shapes = g.Ep1Shapes(hidden=64, inter=32, n_routed=8)
    ids = [6, 1, 7, 0, 4, 2, 5, 3]
    weights = [0.17, 0.81, 0.23, 0.52, 0.39, 0.61, 0.09, 0.72]
    blob = _token_with_weights(ids, weights)
    tok = g.parse_token(blob)
    x = np.linspace(-0.7, 1.1, 64)
    ckpt = _eight_expert_checkpoint()
    result = g.forward_token(ckpt, blob, layer=3, shapes=shapes, activation=x)
    expected = _one_expert(ckpt, "shared_experts", None, x, 1.0)
    for slot, expert in enumerate(ids):
        expected += _one_expert(ckpt, "switch_mlp", expert, x, tok.weights[slot])
    np.testing.assert_allclose(result["sum"], expected, rtol=1e-12)
    wrong = _hand_sum(ckpt, x, tok.weights)
    assert not np.allclose(result["sum"], wrong)


def test_one_expert_index_does_not_walk_the_others():
    """raw() may return the whole expert axis; the loader must index one id.

    Touching any other expert, or the whole stack, fails this."""
    ckpt = _eight_expert_checkpoint()
    wname, sname = g.matrix_names(3, "switch_mlp")["W_gate"]
    g.load_expert_matrix(ckpt, wname, sname, 5, 32, 64)
    assert ckpt.touched[wname] == [5]
    assert ckpt.touched[sname] == [5]


def test_budgets_are_declared_and_a_double_fails():
    """The numbers are the module constants. A pure scale error of 2 has
    cosine 1 and must still fail. The same vector passes.
    """
    assert g.REL_L2 == 0.15
    assert g.NORM_RATIO_MIN == 0.85 and g.NORM_RATIO_MAX == 1.15
    assert g.COSINE_MIN == 0.99
    rng = np.random.Generator(np.random.PCG64(5))
    x = rng.normal(size=8)
    wg, wu, wd = rng.normal(size=(4, 8)), rng.normal(size=(4, 8)), rng.normal(size=(8, 4))
    once = g.expert_forward(x, wg, wu, wd, 0.5)
    twice = g.expert_forward(x, wg, wu, wd, 1.0)
    assert np.allclose(twice["y"], 2 * once["y"])
    report = g.compare(once["y"], twice["y"])
    assert report["cosine"] == pytest.approx(1.0, abs=1e-12)
    assert report["norm_ratio"] == pytest.approx(2.0, abs=1e-12)
    assert report["rel_l2"] == pytest.approx(1.0, abs=1e-12)
    assert report["ok"] is False
    same = g.compare(once["y"], once["y"].copy())
    assert same["ok"] is True and same["rel_l2"] == 0.0
    # Sensitivity of the declared window: ±15% is the edge, just past it fails.
    inside = g.compare(once["y"], once["y"] * 1.15)
    outside = g.compare(once["y"], once["y"] * 1.16)
    assert inside["ok"] is True
    assert outside["ok"] is False


def test_f32_diagnostic_is_not_the_reference():
    """diag32_* is a separate name. The provisional budget is not fitted to it."""
    rng = np.random.Generator(np.random.PCG64(11))
    out = g.expert_forward(rng.normal(size=64), rng.normal(size=(32, 64)),
                           rng.normal(size=(32, 64)), rng.normal(size=(64, 32)), 0.3)
    assert "diag32_y" in out
    assert not np.shares_memory(out["y"], out["diag32_y"])
    # The declared window stays the provisional constants, whatever this gap is.
    assert g.REL_L2 == 0.15


def test_routed_coefficient_is_the_serialized_weight_once():
    """Shared coeff is 1. Routed coeff is the number passed in, once.

    0.5 is not a stand-in for 2.5 * something. Squaring it must not match.
    """
    x = np.ones(4)
    wg = np.zeros((2, 4))
    wg[0, 0] = 1.0
    wu = np.ones((2, 4))
    wd = np.eye(4, 2)
    shared = g.expert_forward(x, wg, wu, wd, 1.0)
    routed = g.expert_forward(x, wg, wu, wd, 0.5)
    assert np.allclose(routed["y"], shared["y"] * 0.5)
    assert not np.allclose(routed["y"], shared["y"] * 0.5 * 0.5)


def test_silu_stable_matches_direct_form_on_moderate_inputs():
    z = np.array([-20.0, -1.0, 0.0, 1.0, 20.0, 1000.0, -1000.0])
    got = g.silu(z)
    # Direct form overflows at 1000; stable form stays finite and equals x or 0.
    assert np.isfinite(got).all()
    assert got[5] == pytest.approx(1000.0)
    assert got[6] == pytest.approx(0.0, abs=1e-300)
    moderate = z[:5]
    sig = 1.0 / (1.0 + np.exp(-moderate))
    direct = moderate * sig
    assert np.allclose(g.silu(moderate), direct, rtol=1e-12)




def test_compare_reports_max_abs_and_rejects_zero_reference():
    a = np.array([1.0, 0.0, -1.0])
    report = g.compare(a, a + np.array([0.0, 0.25, 0.0]))
    assert report["max_abs"] == pytest.approx(0.25)
    zero = np.zeros(4)
    bad = g.compare(zero, np.ones(4))
    assert bad["ok"] is False
    assert bad["cosine"] == 0.0
    assert g.compare(zero, zero)["ok"] is True


def test_source_does_not_call_split_or_layout(monkeypatch):
    """If the golden imported the importer's split or layout, this raises."""
    import bachflow_importer.layout as layout
    import bachflow_importer.split as split

    def _boom(*_a, **_k):
        raise AssertionError("golden called a forbidden importer helper")

    monkeypatch.setattr(split, "import_weights", _boom)
    monkeypatch.setattr(split, "pieces_of", _boom)
    monkeypatch.setattr(split, "scale_name", _boom)
    monkeypatch.setattr(layout, "sf_offset", _boom)
    monkeypatch.setattr(layout, "sf_extent", _boom)
    shapes = g.Ep1Shapes(hidden=64, inter=32, n_routed=8, n_shared=1, n_selected=8, group=32)
    blob = _token_with_weights(list(range(8)), [0.2] * 8)
    g.forward_token(_eight_expert_checkpoint(), blob, layer=3, shapes=shapes,
                    activation=np.ones(64))
    text = (SRC / "bach/compiler/reference/checkpoint_golden.py").read_text()
    head = text.split("def _cli")[0]
    for banned in (
        "bachflow_importer", "mmem_layout", "sf_offset", "bwgt",
        "bach_route_planner", "ShardyRoute", "numeric_ref", "ffn_reference",
        "routed_scale", "routed_scaling",
    ):
        assert banned not in head, banned
