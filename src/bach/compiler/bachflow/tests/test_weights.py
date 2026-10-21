"""NK retile and padded-SF extraction, then one real packed expert."""

from __future__ import annotations

import os
import struct
from pathlib import Path

import pytest

from bach.compiler.bachflow.weights import (
    EXPERT_BYTES,
    EXPERT_SCALES,
    MMEM_BASE,
    MU_BASE,
    PRIM_K,
    PRIM_N,
    TEMPLATE_BASE,
    WeightError,
    _selected_slots,
    _geometry,
    scale_extract,
    weights_to_tiles,
    write_weights,
    _template,
)
from bach.compiler.bachflow.layer import open_layer

HALF = 16 * 1024 * 1024
SF_BYTES = 4 * 1024 * 1024
SCENE = Path(os.environ.get("BACHFLOW_EP1_RUN", "output/latch-validation/ep1_pp3_tp16"))
BUFF = SCENE / "GLM5-744B-MoE-EP1PP3TP16-v0.1-FP8-L00003-of-00078.buff"
TOKEN = Path(os.environ.get("BACHFLOW_GOLDEN_DIR", str(SCENE / "validation/checkpoint-golden"))) / "user101.token"


def _nk(n, k, fill):
    return bytes(fill(row, col) & 0xFF for row in range(n) for col in range(k))


def _sf_index(addr):
    return (addr % HALF) // 128 * 32 + (16 if addr >= HALF else 0) + (addr % 128) // 32


def _paint(base, n, k):
    """Unique scale per 32-byte group, placed with the packed slot formula."""
    sf = bytearray(SF_BYTES)
    for row in range(n):
        for col in range(0, k, 32):
            addr = base + row * k + col
            sf[_sf_index(addr)] = 0x11 + (row * 3 + col // 32) % 20
    return bytes(sf)


def test_tiles_follow_column_major_both_geometries():
    for n, k in ((128, 768), (768, 128)):
        raw = _nk(n, k, lambda row, col: row * 13 + col * 7 + 1)
        tiled = weights_to_tiles(raw, n, k)
        assert len(tiled) == n * k
        k_tiles = k // PRIM_K
        tile = PRIM_K * PRIM_N
        for n_idx in range(n // PRIM_N):
            for k_idx in range(k_tiles):
                at = (n_idx * k_tiles + k_idx) * tile
                for j in range(PRIM_N):
                    expect = raw[(n_idx * PRIM_N + j) * k + k_idx * PRIM_K:][:PRIM_K]
                    assert tiled[at + j * PRIM_K:at + (j + 1) * PRIM_K] == expect
        # A row-major copy would keep the first K bytes as row 0. Column-major does not.
        assert tiled[:PRIM_K] == raw[:PRIM_K]
        assert tiled[PRIM_K:PRIM_K * 2] == raw[k:k + PRIM_K]


def test_scale_extract_crosses_the_16mib_half_and_slot_padding():
    n, k = 128, 768
    base = HALF - 64  # one group in the low half, the rest in the high half
    sf = bytearray(_paint(base, n, k))
    # Padding inside every touched 32 B slot, and one untouched slot, must not leak.
    for addr in range(base, base + n * k, 32):
        slot = _sf_index(addr) - (addr % 128) // 32
        for pad in range(4, 16):
            sf[slot + pad] = 0xA5
            sf[slot + 16 + pad] = 0x5A
    sf[_sf_index(base + n * k)] = 0xEE
    got = scale_extract(sf, base, n, k)
    assert len(got) == n * k // 32
    cursor = 0
    k_tiles = k // PRIM_K
    for n_idx in range(n // PRIM_N):
        for k_idx in range(k_tiles):
            for j in range(PRIM_N):
                for g in range(PRIM_K // 32):
                    addr = base + (n_idx * PRIM_N + j) * k + k_idx * PRIM_K + g * 32
                    assert got[cursor] == sf[_sf_index(addr)]
                    assert got[cursor] not in (0xA5, 0x5A, 0xEE)
                    cursor += 1
    assert cursor == len(got)
    assert any(addr >= HALF for addr in range(base, base + 128, 32))
    assert any(addr < HALF for addr in range(base, base + 128, 32))


def test_nonzero_slot_uses_its_own_stride():
    n, k = 128, 768
    slot = 9
    base = slot * EXPERT_BYTES + 4096
    raw = _nk(n, k, lambda row, col: (row + slot) ^ col)
    sf = _paint(base, n, k)
    tiled = weights_to_tiles(raw, n, k)
    scales = scale_extract(sf, base, n, k)
    assert tiled[0] == raw[0]
    assert scales[0] == sf[_sf_index(base)]
    assert scales[0] != sf[_sf_index(0)]


def test_rejects_bad_shape_and_a_scale_past_the_span():
    with pytest.raises(WeightError):
        weights_to_tiles(b"\x00" * (127 * 768), 127, 768)
    with pytest.raises(WeightError):
        scale_extract(bytes(SF_BYTES), 32 * 1024 * 1024 - 32, 128, 768)
    with pytest.raises(WeightError):
        scale_extract(bytes(100), 0, 128, 768)


@pytest.mark.parametrize("nblock,kblock,n,k", [(1, 12, 128, 768), (6, 2, 768, 128)])
def test_register_geometry_accepts_both_scene_projections(nblock, kblock, n, k):
    fields = {i * 4: 0 for i in range(10)}
    fields[0] = nblock | kblock << 16
    fields[24] = MMEM_BASE
    fields[28] = EXPERT_BYTES
    fields[36] = 1 | 2 | 1 << 3 | 1 << 5 | 1 << 7 | 8 << 15
    geometry = _geometry(fields)
    assert (geometry.n, geometry.k) == (n, k)
    fields[36] |= 1 << 2  # FC2 writes BF16; output precision does not change weight layout.
    assert _geometry(fields) == geometry
    fields[0] = 1 | 2 << 16  # Smaller partial shape must not use a 98304-byte expert.
    with pytest.raises(WeightError):
        _geometry(fields)


def test_token_count_is_checked(tmp_path):
    path = tmp_path / "wrong-count.token"
    raw = bytearray(_token(tuple(range(8))))
    raw[0] = 4
    path.write_bytes(raw)
    with pytest.raises(WeightError, match="expected 8 selected experts"):
        _selected_slots([path])


def _token(ids):
    raw = bytearray(6392)
    raw[0:2] = b"\x08\x00"
    struct.pack_into("<8H", raw, 2, *ids)
    return bytes(raw)


def test_real_chip0_p1_shared_slot_matches_the_packed_template(tmp_path):
    if not BUFF.exists() or not TOKEN.exists():
        pytest.skip("packed layer-3 scene is not present")
    layer = open_layer(BUFF, BUFF.with_suffix(".bwgt"), None)
    records = write_weights(layer, [TOKEN], tmp_path, cores={(0, 1)}, slots_only={0})
    slot0 = [rec for rec in records if rec.chip == 0 and rec.p == 1 and rec.slot == 0]
    assert len(slot0) == 1
    rec = slot0[0]
    assert rec.base == 0
    assert rec.geometry.n == 128 and rec.geometry.k == 768
    assert rec.geometry.n_tiles == 1 and rec.geometry.k_tiles == 12
    assert rec.geometry.b_stride == EXPERT_BYTES
    assert rec.geometry.share_mask == 1 and rec.geometry.shared == 1
    assert rec.geometry.activated == 8 and rec.geometry.routed_slots == 256
    data = (tmp_path / rec.data_name).read_bytes()
    scale = (tmp_path / rec.scale_name).read_bytes()
    assert len(data) == EXPERT_BYTES and len(scale) == EXPERT_SCALES
    # Column 0 of tile 0 is the first 64 of packed row 0; column 1 starts a row later.
    assert data[:PRIM_K] != data[PRIM_K:PRIM_K * 2]
    assert any(scale)
    ids = struct.unpack_from("<8H", TOKEN.read_bytes(), 2)
    assert 0 not in ids and all(0 <= expert < 256 for expert in ids)
    # The template address the geometry came from is the global MU window.
    assert MU_BASE + TEMPLATE_BASE == 0x48031000
    # Overlapping ids select one shared slot plus each distinct routed slot.
    raw = TOKEN.read_bytes()
    one = _selected(tmp_path, raw)
    two = _selected(tmp_path, raw, _token((ids[0], 4, 5, 6, 7, 8, 9, 10)))
    assert one == {0} | {1 + expert for expert in ids}
    assert two == one | {1 + expert for expert in (4, 5, 6, 7, 8, 9, 10)}
    assert len(two) == len(set(two))


def test_real_fc2_high_half_slot_matches_packed_data_and_scales(tmp_path):
    if not BUFF.exists() or not TOKEN.exists():
        pytest.skip("packed layer-3 scene is not present")
    layer = open_layer(BUFF, BUFF.with_suffix(".bwgt"), None)
    try:
        core = next(core for chip in layer.chips for core in chip.cores
                    if (fields := _template(core)) is not None and fields[36] & (1 << 23))
        fields = _template(core)
        assert fields[36] & (1 << 2)  # Actual FC2 writes BF16.
        slot = max(_selected_slots([TOKEN]))
        offset = fields[24] - MMEM_BASE + slot * EXPERT_BYTES
        assert offset >= HALF
        nk = bytes(core.ext("mmem").data[offset:offset + EXPERT_BYTES])
        sf = core.ext("mmem_sf").data
        records = write_weights(layer, [TOKEN], tmp_path, cores={(core.chip, core.p)}, slots_only={slot})
        assert len(records) == 1
        rec = records[0]
        assert rec.base == offset
        assert (rec.geometry.n, rec.geometry.k) == (768, 128)
        data = (tmp_path / rec.data_name).read_bytes()
        scales = (tmp_path / rec.scale_name).read_bytes()
        assert len(data) == EXPERT_BYTES and len(scales) == EXPERT_SCALES
        for nt in range(6):
            for kt in range(2):
                for row in range(128):
                    source = (nt * 128 + row) * 128 + kt * 64
                    target = (nt * 2 + kt) * 8192 + row * 64
                    assert data[target:target + 64] == nk[source:source + 64]
                    target_sf = (nt * 2 + kt) * 256 + row * 2
                    for group in range(2):
                        assert scales[target_sf + group] == sf[_sf_index(offset + source + group * 32)]
        del sf
    finally:
        layer.close()


def _selected(directory, *raws):
    paths = []
    for i, raw in enumerate(raws):
        path = directory / f"ids-{i}.token"
        path.write_bytes(raw)
        paths.append(path)
    return set(_selected_slots(paths))
