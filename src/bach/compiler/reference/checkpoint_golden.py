"""Full-global safetensors golden for one EP1 MoE token.

Independent of the planner, the weight importer, and the latch numeric model.
Weights come only from ``Checkpoint.info`` / ``Checkpoint.raw``. MLX stores each
MXFP8 matrix as little-endian U32 packs of four E4M3 bytes, plus one E8M0 scale
byte per 32 elements along K.

The math is one full-global float64 matvec per selected expert. SiLU is the
stable form. A routed expert's coefficient is the serialized token weight, used once on
the down projection. This module does not read a scaling factor out of the
config and apply it again. No packed weight image, shard map, MatrixMem
layout, or route plan is read.
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
from dataclasses import dataclass

import numpy as np

GROUP = 32
TOKEN_BYTES = 6392
HEADER_BYTES = 56
N_SELECTED = 8
SHARED = 1
N_EXPERTS = SHARED + N_SELECTED  # 9: shared, then the eight routed ids in order

# Acceptance budgets. Fixed here, before any checkpoint is read.
#
# Provisional. These are the initial conservative window from the plan
# (rel L2 <= 0.15, norm ratio in [0.85, 1.15], cosine >= 0.99). They are not
# fitted to a simulator and not fitted to the float32-versus-float64 gap of
# this formula. That gap is a different rounding, not an isolated quantization
# measurement, so it does not set or tighten this window. Revisit the numbers
# only with a separate quantization fixture. Until then a caller compares
# against these constants as written.
#
# A pure scale error s keeps cosine at 1. rel L2 is |1 - s| and the norm ratio
# is |s|, so both trip at ±15%. A doubled vector is s = 2 and fails. A zero
# reference against a nonzero vector fails; cosine is not reported as 1 there.
REL_L2 = 0.15
NORM_RATIO_MIN = 0.85
NORM_RATIO_MAX = 1.15
COSINE_MIN = 0.99

# Names in mlx-community/GLM-5-8bit-MXFP8. ``{layer}`` is the model's layer index.
MATRICES = {
    "W_gate": "model.layers.{layer}.mlp.{kind}.gate_proj",
    "W_up": "model.layers.{layer}.mlp.{kind}.up_proj",
    "W_down": "model.layers.{layer}.mlp.{kind}.down_proj",
}
KINDS = ("shared_experts", "switch_mlp")


@dataclass(frozen=True)
class Ep1Shapes:
    hidden: int = 6144
    inter: int = 2048
    n_routed: int = 256
    n_shared: int = 1
    n_selected: int = 8
    group: int = GROUP

    @classmethod
    def from_config(cls, cfg: dict) -> Ep1Shapes:
        q = cfg.get("quantization_config") or cfg.get("quantization") or {}
        group = int(q.get("group_size", GROUP))
        if group != GROUP:
            raise ValueError(f"group_size {group} is not {GROUP}")
        mode = q.get("mode")
        if mode is not None and mode != "mxfp8":
            raise ValueError(f"quant mode {mode!r} is not mxfp8")
        return cls(
            hidden=int(cfg["hidden_size"]),
            inter=int(cfg["moe_intermediate_size"]),
            n_routed=int(cfg["n_routed_experts"]),
            n_shared=int(cfg["n_shared_experts"]),
            n_selected=int(cfg["num_experts_per_tok"]),
            group=group,
        )


# ── codecs, written from the OCP MX / IEEE layout, not imported ──────────────

def _f32_bits(v: float) -> int:
    return struct.unpack("<I", struct.pack("<f", float(v)))[0]


def _f32_of(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def e4m3_decode(code: int) -> float:
    """One OCP E4M3 byte. Bias 7. 0x7F / 0xFF are the only NaNs; no Inf."""
    code &= 0xFF
    sign = code >> 7
    exp = (code >> 3) & 0xF
    man = code & 0x7
    if exp == 0xF and man == 0x7:
        return math.nan
    if exp == 0:
        mag = man * 2.0**-9
    else:
        mag = (1.0 + man / 8.0) * 2.0 ** (exp - 7)
    return -mag if sign else mag


def e4m3_encode(v: float) -> int:
    """Nearest E4M3, ties to even. Overflow and Inf clamp to ±448 (0x7E / 0xFE).

    A subnormal that rounds up to the first normal (mantissa quantum 8) is
    0x08 / 0x88, not clamped back into the subnormal codes.
    """
    if math.isnan(v):
        return 0x7F
    sign = int(math.copysign(1.0, float(v)) < 0)
    if math.isinf(v) or abs(float(v)) > 448.0:
        return (sign << 7) | 0x7E
    mag = abs(float(v))
    if mag < 2.0**-10:
        return sign << 7
    # Subnormal quantum is 2^-9. Eight of those is the first normal, 2^-6.
    if mag < 2.0**-6:
        q = _rne(mag, 2.0**-9)
        if q >= 8:
            return (sign << 7) | 0x08
        return (sign << 7) | q
    exp = math.floor(math.log2(mag))
    quantum = 2.0 ** (exp - 3)
    q = _rne(mag, quantum)
    if q >= 16:
        exp += 1
        q = _rne(mag, 2.0 ** (exp - 3))
    field = exp + 7
    man = q - 8
    if field > 0xF or (field == 0xF and man == 7):
        return (sign << 7) | 0x7E
    return (sign << 7) | (field << 3) | (man & 7)


def _rne(value: float, quantum: float) -> int:
    q = value / quantum
    lo = math.floor(q)
    frac = q - lo
    if frac > 0.5:
        return lo + 1
    if frac < 0.5:
        return lo
    return lo if lo % 2 == 0 else lo + 1


def e8m0_decode(code: int) -> float:
    """Power-of-two scale, exact. 0xFF is NaN. Code 0 is 2^-127, not flushed.

    Flush-to-zero belongs to a hardware diagnostic, not this reference.
    """
    code &= 0xFF
    if code == 0xFF:
        return math.nan
    return 2.0 ** (code - 127)


def e8m0_encode(v: float) -> int:
    """Exponent byte of the FP32 encoding of ``v`` (NaN and Inf included)."""
    return (_f32_bits(v) >> 23) & 0xFF


def decode_e4m3_vector(codes: np.ndarray) -> np.ndarray:
    """Vector decode. Must match ``e4m3_decode`` on every byte."""
    c = np.asarray(codes, dtype=np.uint8)
    sign = (c >> 7).astype(np.int8)
    exp = ((c >> 3) & 0xF).astype(np.int16)
    man = (c & 0x7).astype(np.int16)
    nan = (exp == 0xF) & (man == 0x7)
    sub = exp == 0
    mag = np.where(
        sub,
        man.astype(np.float64) * 2.0**-9,
        (1.0 + man.astype(np.float64) / 8.0) * np.ldexp(np.ones(c.shape, dtype=np.float64), exp - 7),
    )
    out = np.where(sign != 0, -mag, mag)
    out = out.astype(np.float64, copy=False)
    out[nan] = np.nan
    return out


def decode_e8m0_vector(codes: np.ndarray) -> np.ndarray:
    c = np.asarray(codes, dtype=np.uint8).astype(np.int16)
    out = np.ldexp(np.ones(c.shape, dtype=np.float64), c - 127)
    out[c == 0xFF] = np.nan
    return out


def unpack_u32_e4m3(raw: np.ndarray) -> np.ndarray:
    """U32 packs, four little-endian E4M3 bytes, back to one byte per element.

    ``raw`` is the uint8 view ``Checkpoint.raw`` returns: last axis is
    ``shape[-1] * 4`` bytes, which is already K elements.
    """
    flat = np.ascontiguousarray(raw, dtype=np.uint8).reshape(-1)
    if flat.size % 4:
        raise ValueError(f"U32 pack is {flat.size} bytes, not a multiple of 4")
    words = flat.view("<u4")
    b0 = (words & 0xFF).astype(np.uint8)
    b1 = ((words >> 8) & 0xFF).astype(np.uint8)
    b2 = ((words >> 16) & 0xFF).astype(np.uint8)
    b3 = ((words >> 24) & 0xFF).astype(np.uint8)
    return np.stack((b0, b1, b2, b3), axis=-1).reshape(raw.shape)


# ── token: 2 + 8*2 + 8*4 = 50, pad to 56, then 6144 E4M3 and 192 E8M0 ────────

@dataclass(frozen=True)
class Token:
    count: int
    ids: tuple[int, ...]
    weights: tuple[float, ...]
    values: np.ndarray  # float64, length hidden, decoded
    scales: np.ndarray  # float64, length hidden/32
    raw: bytes  # the exact 6392 bytes


def pack_token(
    ids: list[int] | tuple[int, ...],
    weights: list[float] | tuple[float, ...],
    values: np.ndarray,
    scales: np.ndarray,
    *,
    count: int = N_SELECTED,
) -> bytes:
    """Build the 6392-byte EP1 token. ``values`` / ``scales`` are already codes."""
    if len(ids) != N_SELECTED or len(weights) != N_SELECTED:
        raise ValueError(f"need {N_SELECTED} ids and weights, got {len(ids)} / {len(weights)}")
    codes = np.asarray(values, dtype=np.uint8).reshape(-1)
    sc = np.asarray(scales, dtype=np.uint8).reshape(-1)
    if codes.size != 6144 or sc.size != 192:
        raise ValueError(f"token payload {codes.size}+{sc.size}, want 6144+192")
    if count != N_SELECTED:
        raise ValueError(f"count {count} is not {N_SELECTED}")
    header = bytearray(HEADER_BYTES)
    struct.pack_into("<H", header, 0, count)
    for i, expert in enumerate(ids):
        struct.pack_into("<H", header, 2 + 2 * i, int(expert))
    for i, w in enumerate(weights):
        struct.pack_into("<f", header, 18 + 4 * i, float(w))
    blob = bytes(header) + codes.tobytes() + sc.tobytes()
    if len(blob) != TOKEN_BYTES:
        raise ValueError(f"token is {len(blob)} bytes, want {TOKEN_BYTES}")
    return blob


def parse_token(blob: bytes) -> Token:
    if len(blob) != TOKEN_BYTES:
        raise ValueError(f"token is {len(blob)} bytes, want {TOKEN_BYTES}")
    count = struct.unpack_from("<H", blob, 0)[0]
    if count != N_SELECTED:
        raise ValueError(f"token count {count} is not {N_SELECTED}")
    ids = tuple(struct.unpack_from("<8H", blob, 2))
    weights = tuple(struct.unpack_from("<8f", blob, 18))
    if blob[50:HEADER_BYTES] != b"\x00" * (HEADER_BYTES - 50):
        raise ValueError("token pad bytes 50..56 are not zero")
    codes = np.frombuffer(blob, dtype=np.uint8, count=6144, offset=HEADER_BYTES)
    sc = np.frombuffer(blob, dtype=np.uint8, count=192, offset=HEADER_BYTES + 6144)
    values = decode_e4m3_vector(codes) * np.repeat(decode_e8m0_vector(sc), GROUP)
    return Token(count, ids, weights, values, decode_e8m0_vector(sc), blob)


def deterministic_token(seed: int, shapes: Ep1Shapes | None = None) -> bytes:
    """One reproducible token.

    Scales are the constant E8M0 code 127 (value 1), all 192 of them. Element
    codes are E4M3 encodings of signed normal draws kept in about 0.1..2, so
    the activation is non-periodic, has no signed zero, and cannot overflow a
later matvec by itself. ``weights`` are the serialized coefficients. Any
extra factor belongs in the generator, before these bytes are written.
    """
    shapes = shapes or Ep1Shapes()
    rng = np.random.Generator(np.random.PCG64(seed))
    ids = rng.choice(shapes.n_routed, size=shapes.n_selected, replace=False).astype(np.uint16)
    weights = rng.random(shapes.n_selected).astype(np.float64)
    draws = rng.normal(size=shapes.hidden)
    sign = np.where(draws < 0.0, -1.0, 1.0)
    mag = np.clip(np.abs(draws), 0.1, 2.0)
    codes = np.array([e4m3_encode(float(s * m)) for s, m in zip(sign, mag)], dtype=np.uint8)
    scales = np.full(shapes.hidden // GROUP, 127, dtype=np.uint8)
    return pack_token(ids.tolist(), [float(w) for w in weights], codes, scales)


# ── weights: one expert, never the whole routed tensor ───────────────────────

def matrix_names(layer: int, kind: str) -> dict[str, tuple[str, str]]:
    """Logical name -> (weight tensor, scale tensor) in the checkpoint."""
    if kind not in KINDS:
        raise ValueError(f"kind {kind!r} is not one of {KINDS}")
    out = {}
    for logical, template in MATRICES.items():
        stem = template.format(layer=layer, kind=kind)
        out[logical] = (stem + ".weight", stem + ".scales")
    return out


def expect_shapes(info_shape: tuple[int, ...], want: tuple[int, ...], name: str) -> None:
    if tuple(info_shape) != tuple(want):
        raise ValueError(f"{name}: checkpoint shape {info_shape} != {want}")


def load_expert_matrix(ckpt, weight_name: str, scale_name: str, expert: int | None,
                       n: int, k: int) -> tuple[np.ndarray, np.ndarray]:
    """One ``[N, K]`` matrix as float64 values and float64 per-group scales.

    ``expert is None`` selects a rank-2 shared matrix. A routed matrix is rank 3
    and only that expert's slice is decoded.
    """
    winfo = ckpt.info(weight_name)
    sinfo = ckpt.info(scale_name)
    if winfo.dtype != "U32":
        raise ValueError(f"{weight_name}: dtype {winfo.dtype}, want U32 packs")
    if sinfo.dtype != "U8":
        raise ValueError(f"{scale_name}: dtype {sinfo.dtype}, want U8 E8M0")
    if expert is None:
        expect_shapes(winfo.shape, (n, k // 4), weight_name)
        expect_shapes(sinfo.shape, (n, k // GROUP), scale_name)
        wraw = np.array(ckpt.raw(weight_name))
        sraw = np.array(ckpt.raw(scale_name))
    else:
        expect_shapes(winfo.shape[1:], (n, k // 4), weight_name)
        expect_shapes(sinfo.shape[1:], (n, k // GROUP), scale_name)
        if not 0 <= expert < winfo.shape[0]:
            raise IndexError(f"{weight_name}: expert {expert} outside {winfo.shape[0]}")
        wraw = np.array(ckpt.raw(weight_name)[expert])
        sraw = np.array(ckpt.raw(scale_name)[expert])
    codes = unpack_u32_e4m3(wraw)  # [N, K]
    if codes.shape != (n, k):
        raise ValueError(f"{weight_name}: unpacked {codes.shape}, want {(n, k)}")
    elem = decode_e4m3_vector(codes)
    scales = decode_e8m0_vector(sraw.reshape(n, k // GROUP))
    values = elem * np.repeat(scales, GROUP, axis=1)
    return values, scales


# ── math ─────────────────────────────────────────────────────────────────────

def silu(x: np.ndarray) -> np.ndarray:
    """Stable SiLU in float64: x / (1 + exp(-x)), with the large-|x| ends closed."""
    z = np.asarray(x, dtype=np.float64)
    out = np.empty_like(z)
    pos = z >= 0
    # x >= 0: x * sigmoid(x) = x / (1 + exp(-x)), exp(-x) in (0, 1].
    ez = np.exp(-z[pos])
    out[pos] = z[pos] / (1.0 + ez)
    # x < 0: x * exp(x) / (1 + exp(x)), exp(x) in (0, 1). No overflow.
    ez = np.exp(z[~pos])
    out[~pos] = z[~pos] * ez / (1.0 + ez)
    return out


def expert_forward(
    x: np.ndarray,
    w_gate: np.ndarray,
    w_up: np.ndarray,
    w_down: np.ndarray,
    coeff: float,
) -> dict[str, np.ndarray]:
    """One expert. ``coeff`` is 1 for the shared expert and the token weight for a routed one.

    ``w_*`` are ``[N, K]`` already scaled into float64. Returns the intermediates
    a caller writes into the npz, plus this expert's contribution ``y``.
    """
    gate = matvec_f64(w_gate, x)
    up = matvec_f64(w_up, x)
    hidden = silu(gate) * up
    y = coeff * matvec_f64(w_down, hidden)
    # Diagnostic only: the same formula with each product in float32.
    # Not read by compare() unless the caller names diag32_*.
    g32 = matvec_f32_diagnostic(w_gate, x)
    u32 = matvec_f32_diagnostic(w_up, x)
    h32 = (silu(g32) * u32).astype(np.float32)
    return {
        "gate": gate,
        "up": up,
        "hidden": hidden,
        "y": y,
        "diag32_gate": g32,
        "diag32_up": u32,
        "diag32_y": coeff * matvec_f32_diagnostic(w_down, h32),
    }


def forward_token(ckpt, blob: bytes, *, layer: int, shapes: Ep1Shapes | None = None,
                  activation: np.ndarray | None = None) -> dict[str, np.ndarray]:
    """Nine experts, full global, then the sum.

    Routed coefficient is ``tok.weights[slot]`` exactly once, on that expert's
    down projection. Shared coefficient is 1. No config factor is read.
    ``activation`` overrides the decoded token vector so a tiny checkpoint can
    run the same loop; the header is still parsed.
    """
    shapes = shapes or Ep1Shapes()
    tok = parse_token(blob)
    if shapes.n_shared != 1 or shapes.n_selected != 8 or shapes.group != 32:
        raise ValueError("EP1 reference requires one shared and eight routed experts, group 32")
    if any(expert >= shapes.n_routed for expert in tok.ids):
        raise ValueError("token expert ID is outside the checkpoint configuration")
    if len(set(tok.ids)) != len(tok.ids) or not np.isfinite(tok.weights).all():
        raise ValueError("token requires distinct expert IDs and finite coefficients")
    x = tok.values if activation is None else np.asarray(activation, dtype=np.float64)
    if x.shape != (shapes.hidden,):
        raise ValueError(f"activation {x.shape} != hidden {shapes.hidden}")
    shared = matrix_names(layer, "shared_experts")
    routed = matrix_names(layer, "switch_mlp")
    per = []
    g, _ = load_expert_matrix(ckpt, *shared["W_gate"], None, shapes.inter, shapes.hidden)
    u, _ = load_expert_matrix(ckpt, *shared["W_up"], None, shapes.inter, shapes.hidden)
    d, _ = load_expert_matrix(ckpt, *shared["W_down"], None, shapes.hidden, shapes.inter)
    per.append(expert_forward(x, g, u, d, 1.0))
    for slot, expert in enumerate(tok.ids):
        g, _ = load_expert_matrix(ckpt, *routed["W_gate"], expert, shapes.inter, shapes.hidden)
        u, _ = load_expert_matrix(ckpt, *routed["W_up"], expert, shapes.inter, shapes.hidden)
        d, _ = load_expert_matrix(ckpt, *routed["W_down"], expert, shapes.hidden, shapes.inter)
        per.append(expert_forward(x, g, u, d, float(tok.weights[slot])))
    out = {
        "ids": np.asarray(tok.ids, dtype=np.int32),
        "weights": np.asarray(tok.weights, dtype=np.float64),
        "x": x,
    }
    for key in ("gate", "up", "hidden", "y", "diag32_gate", "diag32_up", "diag32_y"):
        stacked = np.stack([p[key] for p in per], axis=0)
        out[key] = stacked
    out["sum"] = np.sum(out["y"], axis=0)
    out["diag32_sum"] = np.sum(out["diag32_y"], axis=0)
    return out


def save_npz(path: str, result: dict[str, np.ndarray]) -> None:
    np.savez(path, **result)


def compare(reference: np.ndarray, other: np.ndarray, *,
            rel_l2: float = REL_L2,
            norm_min: float = NORM_RATIO_MIN,
            norm_max: float = NORM_RATIO_MAX,
            cosine_min: float = COSINE_MIN) -> dict[str, float]:
    """Acceptance against the declared budgets.

    Cosine alone is not enough: a doubled vector has cosine 1 and fails the
    norm ratio and the relative L2. The budgets are the module constants;
    callers do not retune them from a measured run.
    """
    a = np.asarray(reference, dtype=np.float64).reshape(-1)
    b = np.asarray(other, dtype=np.float64).reshape(-1)
    if a.shape != b.shape:
        raise ValueError(f"compare shape {a.shape} != {b.shape}")
    if not np.isfinite(a).all() or not np.isfinite(b).all():
        raise ValueError("compare refuses non-finite values")
    diff = float(np.linalg.norm(a - b))
    max_abs = float(np.max(np.abs(a - b)))
    na = float(np.linalg.norm(a))
    nb = float(np.linalg.norm(b))
    # A zero reference must not report cosine 1 against a nonzero vector.
    if na == 0.0:
        ok = nb == 0.0
        return {
            "rel_l2": 0.0 if ok else math.inf,
            "max_abs": max_abs,
            "norm_ratio": 1.0 if ok else math.inf,
            "cosine": 1.0 if ok else 0.0,
            "rel_l2_max": float(rel_l2),
            "norm_min": float(norm_min),
            "norm_max": float(norm_max),
            "cosine_min": float(cosine_min),
            "ok": bool(ok),
        }
    rel = diff / na
    ratio = nb / na
    cos = float((a @ b) / (na * nb)) if nb > 0.0 else 0.0
    ok = rel <= rel_l2 and norm_min <= ratio <= norm_max and cos >= cosine_min
    return {
        "rel_l2": rel,
        "max_abs": max_abs,
        "norm_ratio": float(ratio),
        "cosine": cos,
        "rel_l2_max": float(rel_l2),
        "norm_min": float(norm_min),
        "norm_max": float(norm_max),
        "cosine_min": float(cosine_min),
        "ok": bool(ok),
    }


def matvec_f64(weight: np.ndarray, x: np.ndarray) -> np.ndarray:
    """The reference product: one unsplit float64 matvec. No block quant."""
    return np.asarray(weight, dtype=np.float64) @ np.asarray(x, dtype=np.float64)


def matvec_f32_diagnostic(weight: np.ndarray, x: np.ndarray) -> np.ndarray:
    """Not the reference. One float32 matvec, named so it cannot be read as one.

    Used only to show the declared budgets sit well above a same-formula
    precision change. A hardware reduction is a different question and is
    not computed here.
    """
    w = np.asarray(weight, dtype=np.float32)
    v = np.asarray(x, dtype=np.float32)
    return (w @ v).astype(np.float64)


def _cli(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description="EP1 full-global MXFP8 golden")
    sub = p.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("token", help="write one deterministic 6392-byte token")
    g.add_argument("--seed", type=int, required=True)
    g.add_argument("--out", required=True)
    c = sub.add_parser("compute", help="write per-expert intermediates and the sum")
    c.add_argument("--checkpoint", required=True)
    c.add_argument("--layer", type=int, required=True)
    c.add_argument("--token", required=True)
    c.add_argument("--out", required=True)
    c.add_argument("--config", default=None, help="config.json; default is shapes beside the checkpoint")
    m = sub.add_parser("compare", help="amplitude check of two float64 vectors in npz")
    m.add_argument("--ref", required=True)
    m.add_argument("--other", required=True)
    m.add_argument("--key", default="sum")
    args = p.parse_args(argv)
    if args.cmd == "token":
        blob = deterministic_token(args.seed)
        with open(args.out, "wb") as f:
            f.write(blob)
        return 0
    if args.cmd == "compare":
        ref = np.load(args.ref)[args.key]
        other = np.load(args.other)[args.key]
        report = compare(ref, other)
        print(
            f"rel_l2={report['rel_l2']:.6f}<={report['rel_l2_max']} "
            f"max_abs={report['max_abs']:.6e} "
            f"norm_ratio={report['norm_ratio']:.6f} "
            f"in[{report['norm_min']},{report['norm_max']}] "
            f"cosine={report['cosine']:.6f}>={report['cosine_min']} "
            f"ok={report['ok']}"
        )
        return 0 if report["ok"] else 1
    # compute. Import here so the math module does not depend on the importer
    # at import time; the only calls used are Checkpoint.info and Checkpoint.raw.
    from bachflow_importer.checkpoint import Checkpoint

    ckpt = Checkpoint(args.checkpoint)
    shapes = Ep1Shapes()
    cfg_path = args.config
    if cfg_path is None:
        from pathlib import Path
        guess = Path(args.checkpoint) / "config.json"
        cfg_path = str(guess) if guess.is_file() else None
    if cfg_path:
        import json
        shapes = Ep1Shapes.from_config(json.loads(open(cfg_path).read()))
    blob = open(args.token, "rb").read()
    save_npz(args.out, forward_token(ckpt, blob, layer=args.layer, shapes=shapes))
    return 0


if __name__ == "__main__":
    sys.exit(_cli(sys.argv[1:]))
