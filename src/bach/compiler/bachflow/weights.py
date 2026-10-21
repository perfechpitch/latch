"""Packed MatrixMem MXFP8 weights, retiled for the baseline latch MU.

The pack stores one expert as compact NK bytes (row-major, N rows of K) plus
scales in the padded ``mmem_sf`` segment. Baseline latch reads one primitive
at ``b_addr + slot * b_expert_stride + (n_idx * kblock + k_idx) * PrimK * PrimN``,
column-major inside the tile (column ``j`` at ``j * PrimK``), and appends that
tile's scales in the same column order, one E8M0 byte per 32 weights.

Geometry comes only from the bound core's ``dsa.mu.run*`` extents. This module
accepts the EP1 MXFP8 case: primitive type 1 (K64 x N128), N and K divisible by
those primitives, B stride 98304, share mask bit 0 and MXFP8 on both expert
dtypes. The mode word's routed count is how many experts one token activates,
not how many are stored: this scene stores one shared expert plus 256 routed
ones, slots 0..256. A token id in 0..255 selects physical slot 1+id,
without compact renumbering. The original codes are copied; nothing is requantized. Cores with no
MU template or no MatrixMem weights are skipped, not filled out to 32 MiB of
zeros.
"""

from __future__ import annotations

import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path

from bachflow.mmem_layout import sf_offset
from bachflow.segments import SEGMENT_BY_NAME

from .decode import u32
from .layer import Layer, open_layer

MU_BASE = SEGMENT_BY_NAME["dsa.mu"].base  # 0x48030000
MMEM_BASE = SEGMENT_BY_NAME["mmem"].base
TEMPLATE_BASE = 0x1000
TEMPLATE_STRIDE = 0x80
TEMPLATE_WORDS = 10

PRIM_K = 64
PRIM_N = 128
SCALE_GROUP = 32
EXPERT_BYTES = PRIM_K * PRIM_N * (768 // PRIM_K) * (128 // PRIM_N)  # 98304, either NK
EXPERT_SCALES = EXPERT_BYTES // SCALE_GROUP  # 3072
ROUTED_EXPERTS = 256
SHARED_SLOTS = 1
MXFP8 = 1

TOKEN_BYTES = 6392
TOKEN_ID_OFF = 2
TOKEN_IDS = 8


class WeightError(ValueError):
    """A packed weight the baseline latch cannot be handed as-is."""


@dataclass(frozen=True)
class Geometry:
    n: int
    k: int
    n_tiles: int
    k_tiles: int
    primitive_type: int
    b_base: int
    b_stride: int
    share_mask: int
    activated: int
    shared: int
    routed_slots: int

    def as_dict(self) -> dict[str, int]:
        return {
            "n": self.n, "k": self.k,
            "n_tiles": self.n_tiles, "k_tiles": self.k_tiles,
            "primitive_type": self.primitive_type,
            "b_base": self.b_base, "b_stride": self.b_stride,
            "share_mask": self.share_mask, "activated": self.activated,
            "shared": self.shared, "routed_slots": self.routed_slots,
        }


@dataclass(frozen=True)
class LoadRecord:
    chip: int
    p: int
    slot: int
    base: int
    data_name: str
    scale_name: str
    geometry: Geometry


def weights_to_tiles(data: bytes | memoryview, n: int, k: int) -> bytes:
    """Retile compact NK bytes into baseline latch order.

    Tile ``(n_idx, k_idx)`` is ``PrimK * PrimN`` bytes, stored at
    ``(n_idx * k_tiles + k_idx)``. Inside a tile, column ``j`` occupies
    ``j * PrimK`` bytes: the K elements of that column, in order.
    """
    _divisible(n, k)
    raw = memoryview(data)
    if len(raw) != n * k:
        raise WeightError(f"NK data is {len(raw)} bytes, expected {n * k}")
    k_tiles = k // PRIM_K
    out = bytearray(n * k)
    tile = PRIM_K * PRIM_N
    for n_idx in range(n // PRIM_N):
        for k_idx in range(k_tiles):
            at = (n_idx * k_tiles + k_idx) * tile
            row0 = n_idx * PRIM_N
            col0 = k_idx * PRIM_K
            for j in range(PRIM_N):
                src = (row0 + j) * k + col0
                dst = at + j * PRIM_K
                out[dst:dst + PRIM_K] = raw[src:src + PRIM_K]
    return bytes(out)


def scale_extract(
    sf: bytes | memoryview, data_off: int, n: int, k: int,
) -> bytes:
    """Pull the E8M0 byte of every 32 weight bytes, in tile-column order.

    Packed SF for MatrixMem offset ``A`` is
    ``((A % 16MiB) // 128) * 32 + (16 if A >= 16MiB else 0) + ((A % 128) // 32)``.
    The converted scale of column ``j`` in a tile is the ``PrimK / 32`` bytes of
    that column, so a read of the tile appends them after the column-major body.
    """
    _divisible(n, k)
    if data_off < 0 or data_off % SCALE_GROUP:
        raise WeightError(f"weight offset {data_off:#x} is not 32-byte aligned")
    need = n * k
    if data_off + need > SEGMENT_BY_NAME["mmem"].size:
        raise WeightError(
            f"expert [{data_off:#x}, {data_off + need:#x}) leaves the 32 MiB MatrixMem"
        )
    groups = need // SCALE_GROUP
    raw = bytearray(groups)
    view = memoryview(sf)
    try:
        if len(view) < SEGMENT_BY_NAME["mmem_sf"].size:
            raise WeightError(f"SF span is {len(view)} bytes, expected 4 MiB")
        for i in range(groups):
            raw[i] = view[_sf_at(data_off + i * SCALE_GROUP)]
    finally:
        del view
    return _scales_to_tiles(bytes(raw), n, k)


def write_weights(
    layer: Layer, tokens: list[Path | str], out_dir: Path | str,
    *,
    cores: set[tuple[int, int]] | None = None,
    slots_only: set[int] | None = None,
) -> list[LoadRecord]:
    """Convert every packed expert a bound core's MU template names.

    ``tokens`` are the saved 6392-byte inputs. Their 8 expert ids (u16 at byte 2)
    select the union; slot 0 is the shared expert and is always written. A routed
    id selects physical slot 1+id. Record bases are MatrixMem byte offsets for
    baseline Poke/PokeScale. The caller owns and closes the layer. ``cores`` limits the walk to those
    ``(chip, physical core)`` pairs, and ``slots_only`` further limits slots.
    Identical converted bytes are stored once. Returns one record per written
    ``(chip, physical core, slot)``.
    """
    slots = _selected_slots(tokens)
    if slots_only is not None:
        # Slot 0 is the shared expert. It is stored on every such core, whether
        # or not a token names it.
        allowed = set(slots) | {0}
        unknown = slots_only - allowed
        if unknown:
            raise WeightError(f"slots {sorted(unknown)} are not in the token union")
        slots = [slot for slot in sorted(allowed) if slot in slots_only]
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    written: dict[str, str] = {}
    records: list[LoadRecord] = []
    for chip in layer.chips:
        for core in chip.cores:
            if cores is not None and (chip.c, core.p) not in cores:
                continue
            template = _template(core)
            if template is None:
                continue
            geometry = _geometry(template)
            experts = _take_experts(core, geometry, slots)
            if experts is None:
                continue
            for slot, (data_off, body, groups) in experts.items():
                data = weights_to_tiles(body, geometry.n, geometry.k)
                scale = _scales_to_tiles(groups, geometry.n, geometry.k)
                data_name = _store(out, written, data, "w")
                scale_name = _store(out, written, scale, "s")
                records.append(LoadRecord(
                    chip.c, core.p, slot,
                    data_off - MMEM_BASE, data_name, scale_name, geometry,
                ))
                del data, scale
            del experts
    _manifest(out, records)
    return records


def _divisible(n: int, k: int) -> None:
    if n <= 0 or k <= 0 or n % PRIM_N or k % PRIM_K:
        raise WeightError(
            f"N {n} x K {k} is not a multiple of the K{PRIM_K} x N{PRIM_N} primitive"
        )


def _sf_at(addr: int) -> int:
    block = addr - addr % 128
    return sf_offset(block) + (addr % 128) // SCALE_GROUP


def _template(core) -> dict[int, int] | None:
    runs = [(name, core.extents[name]) for name in list(core.extents)
            if name.startswith("dsa.mu.run")]
    if not runs:
        return None
    words: dict[int, int] = {}
    for _name, ext in runs:
        if ext.addr < MU_BASE or (ext.addr - MU_BASE) % 4 or ext.size % 4:
            raise WeightError(
                f"chip {core.chip} p{core.p}: MU run at {ext.addr:#x} "
                f"({ext.size} bytes) is not a word run inside dsa.mu"
            )
        raw = bytes(ext.data)
        for i in range(len(raw) // 4):
            off = ext.addr - MU_BASE + 4 * i
            value = u32(raw[4 * i:4 * i + 4])
            if off in words and words[off] != value:
                raise WeightError(
                    f"chip {core.chip} p{core.p}: MU word {off:#x} conflicts"
                )
            words[off] = value
    present = [
        t for t in range(4)
        if any(TEMPLATE_BASE + t * TEMPLATE_STRIDE + 4 * i in words
               for i in range(TEMPLATE_WORDS))
    ]
    if not present:
        return None
    if present != [0]:
        raise WeightError(
            f"chip {core.chip} p{core.p}: MU templates {present}, expected only template 0"
        )
    base = TEMPLATE_BASE
    missing = [4 * i for i in range(TEMPLATE_WORDS) if base + 4 * i not in words]
    if missing:
        raise WeightError(
            f"chip {core.chip} p{core.p}: template 0 missing offsets {missing}"
        )
    return {4 * i: words[base + 4 * i] for i in range(TEMPLATE_WORDS)}


def _geometry(fields: dict[int, int]) -> Geometry:
    dim, _a, _astr, _c, _cstr, _ac, b_base, b_stride, _topk, mode = (
        fields[4 * i] for i in range(TEMPLATE_WORDS)
    )
    nblock, kblock = dim & 0xFFFF, dim >> 16
    primitive_type = mode & 1
    a_type = (mode >> 1) & 0x1
    share_type = (mode >> 3) & 0x3
    router_type = (mode >> 5) & 0x3
    share_mask = (mode >> 7) & 0xFF
    activated = (mode >> 15) & 0xFF
    if primitive_type != 1 or a_type != MXFP8:
        raise WeightError(
            f"primitive {primitive_type}, A {a_type}: "
            "only K64xN128 with MXFP8 activation"
        )
    if share_type != MXFP8 or router_type != MXFP8:
        raise WeightError(
            f"share dtype {share_type}, router dtype {router_type}: only MXFP8"
        )
    shared = bin(share_mask).count("1")
    if share_mask != 1 or shared != SHARED_SLOTS or activated != TOKEN_IDS:
        raise WeightError(
            f"share mask {share_mask:#x} ({shared} experts), activated {activated}: "
            f"expected mask 0x1 and {TOKEN_IDS} activated"
        )
    if b_stride != EXPERT_BYTES:
        raise WeightError(f"B stride {b_stride} is not one expert ({EXPERT_BYTES})")
    if (nblock, kblock) not in ((1, 12), (6, 2)):
        raise WeightError(
            f"Nblock {nblock} x Kblock {kblock} is not 128x768 or 768x128"
        )
    n, k = nblock * PRIM_N, kblock * PRIM_K
    if b_base < MMEM_BASE or (b_base - MMEM_BASE) % SCALE_GROUP:
        raise WeightError(f"B base {b_base:#x} is not a MatrixMem weight address")
    # Stored capacity is not the activated count. EP1 keeps the shared expert
    # in slot 0 and routed experts 0..255 in slots 1..256. 257 slots fit; the
    # stride does not by itself prove there is no room for another.
    routed_slots = ROUTED_EXPERTS
    last = b_base + (shared + routed_slots) * b_stride
    if last > MMEM_BASE + SEGMENT_BY_NAME["mmem"].size:
        raise WeightError(f"slots end at {last:#x}, past the 32 MiB MatrixMem")
    return Geometry(n, k, nblock, kblock, primitive_type, b_base, b_stride,
                    share_mask, activated, shared, routed_slots)


def _take_experts(core, geometry: Geometry, slots: list[int]):
    """Copy the selected experts out of the mapped spans, then drop the views.

    Returns ``{slot: (absolute base, NK bytes, compact scale bytes)}``, or None
    when this core has no MatrixMem weights.
    """
    mmem = core.extents.get("mmem")
    sf = core.extents.get("mmem_sf")
    if mmem is None and sf is None:
        return None
    if mmem is None or sf is None:
        raise WeightError(
            f"chip {core.chip} p{core.p}: MatrixMem data and SF must both be packed"
        )
    if mmem.addr != MMEM_BASE or mmem.size != SEGMENT_BY_NAME["mmem"].size:
        raise WeightError(
            f"chip {core.chip} p{core.p}: mmem is {mmem.addr:#x}+{mmem.size}, "
            "not the full 32 MiB span"
        )
    if sf.addr != SEGMENT_BY_NAME["mmem_sf"].base or sf.size != SEGMENT_BY_NAME["mmem_sf"].size:
        raise WeightError(
            f"chip {core.chip} p{core.p}: mmem_sf is {sf.addr:#x}+{sf.size}, "
            "not the full 4 MiB span"
        )
    data_view, sf_view = mmem.data, sf.data
    copied = {}
    try:
        for slot in slots:
            data_off = geometry.b_base + slot * geometry.b_stride
            off = data_off - MMEM_BASE
            body = bytes(data_view[off:off + EXPERT_BYTES])
            groups = bytearray(EXPERT_SCALES)
            for i in range(EXPERT_SCALES):
                groups[i] = sf_view[_sf_at(off + i * SCALE_GROUP)]
            copied[slot] = (data_off, body, bytes(groups))
    finally:
        del data_view, sf_view
    return copied


def _scales_to_tiles(scales: bytes, n: int, k: int) -> bytes:
    """Same tile order as the data: column ``j`` contributes its ``PrimK / 32`` bytes."""
    _divisible(n, k)
    if len(scales) != n * k // SCALE_GROUP:
        raise WeightError(f"scale bytes {len(scales)}, expected {n * k // SCALE_GROUP}")
    out = bytearray(len(scales))
    k_tiles = k // PRIM_K
    per_col = PRIM_K // SCALE_GROUP
    per_tile = PRIM_N * per_col
    cols = k // SCALE_GROUP
    for n_idx in range(n // PRIM_N):
        for k_idx in range(k_tiles):
            at = (n_idx * k_tiles + k_idx) * per_tile
            for j in range(PRIM_N):
                src = (n_idx * PRIM_N + j) * cols + k_idx * per_col
                dst = at + j * per_col
                out[dst:dst + per_col] = scales[src:src + per_col]
    return bytes(out)


def _selected_slots(tokens: list[Path | str]) -> list[int]:
    if not tokens:
        raise WeightError("no token files")
    chosen = {0}
    for path in tokens:
        raw = Path(path).read_bytes()
        if len(raw) != TOKEN_BYTES:
            raise WeightError(f"{path}: {len(raw)} bytes, expected {TOKEN_BYTES}")
        if struct.unpack_from("<H", raw)[0] != TOKEN_IDS:
            raise WeightError(f"{path}: expected {TOKEN_IDS} selected experts")
        ids = struct.unpack_from(f"<{TOKEN_IDS}H", raw, TOKEN_ID_OFF)
        for expert in ids:
            if not 0 <= expert < ROUTED_EXPERTS:
                raise WeightError(f"{path}: expert id {expert} is outside 0..255")
            chosen.add(SHARED_SLOTS + expert)
    return sorted(chosen)


def _store(out: Path, written: dict[str, str], blob: bytes, kind: str) -> str:
    digest = hashlib.sha256(blob).hexdigest()
    name = f"{kind}.{digest[:16]}.bin"
    previous = written.get(digest)
    if previous is not None:
        if previous != name:
            raise WeightError(f"hash collision between {previous} and {name}")
        return name
    path = out / name
    if path.exists():
        if path.read_bytes() != blob:
            raise WeightError(f"{path} already holds different bytes")
    else:
        path.write_bytes(blob)
    written[digest] = name
    return name


def _manifest(out: Path, records: list[LoadRecord]) -> None:
    rows = [{
        "chip": rec.chip,
        "p": rec.p,
        "slot": rec.slot,
        "base": rec.base,
        "data": rec.data_name,
        "scale": rec.scale_name,
        "geometry": rec.geometry.as_dict(),
    } for rec in records]
    (out / "weights.manifest.json").write_text(json.dumps(rows, indent=2) + "\n")


def main(argv: list[str] | None = None) -> None:
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("buff")
    parser.add_argument("bwgt")
    parser.add_argument("out")
    parser.add_argument("tokens", nargs="+")
    parser.add_argument("--hw", default=None)
    args = parser.parse_args(argv)
    layer = open_layer(args.buff, args.bwgt, args.hw)
    try:
        records = write_weights(layer, args.tokens, args.out)
    finally:
        layer.close()
    print(f"{len(records)} weight records in {args.out}")


if __name__ == "__main__":
    main()
