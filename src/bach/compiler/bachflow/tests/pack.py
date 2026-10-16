"""Tiny synthetic packs for the converter. No route search, no weight build.

A pack is a ``.buff`` / ``.bwgt`` pair written with the codegen writer and the
defs layouts, small enough to read by eye. Each test builds one and checks one
behavior.
"""

from __future__ import annotations

import struct
from pathlib import Path

from bachflow.buff import BlobKind
from bachflow.hw.release import pack_rows
from bachflow.router_word import pack_word
from bachflow.segments import segment_meta
from bachflow.ts_attr import DATAIN_TASK_ATTR, TASK_CHAIN_ATTR, pack_attr, pack_entry
from bachflow_codegen.buff import BlobInfo, write_buff, write_bwgt, write_checksum_sidecar

CHIPS = 16
CORES = 10


def _meta(masks: dict[int, int]) -> dict:
    meta: dict = {
        "general.alignment": 32,
        "hw.chip_shape": [4, 4],
        "hw.core_shape": [2, 5],
        "hw.chip_count": CHIPS,
        **segment_meta(),
    }
    for c in range(CHIPS):
        meta[f"hw.{c}.coord"] = [0, c // 4, c % 4]
        # Both column masks, so binding a chip to its column always resolves.
        meta[f"harvest.{c}.case_count"] = 2
        meta[f"harvest.{c}.0.disable_mask"] = 0
        meta[f"harvest.{c}.1.disable_mask"] = 0x084
        logical = [-1] * CORES
        role = ["unused"] * CORES
        for n, p in enumerate((0, 1, 2, 3, 5, 6, 7, 8)):
            logical[p] = n
            role[p] = "normal"
        if c % 4 == 3:
            role[4] = "rcore"
            logical[4] = 8
        if c % 4 == 0:
            role[0] = "bcore"
            logical[0] = 8
        meta[f"core_map.{c}.0.phys2logical"] = logical
        meta[f"core_map.{c}.0.phys_role"] = role
        bad = 0x084
        meta[f"core_map.{c}.1.phys2logical"] = [
            -1 if bad & (1 << p) else logical[p] for p in range(CORES)
        ]
        meta[f"core_map.{c}.1.phys_role"] = [
            "unused" if bad & (1 << p) else role[p] for p in range(CORES)
        ]
    return meta


def _blob(meta, blobs, key, kind, data) -> None:
    """``key`` is the metadata key; the blob is named from it and content-addressed by hand."""
    if not data:
        return
    name = key.removesuffix(".blob")
    blobs.append(BlobInfo(name, kind, len(data), 0, data))
    meta[key] = name


def build(tmp: Path, *, mask: int = 0, cores: dict[tuple[int, int], dict] | None = None,
          bwgt: bytes = b"") -> tuple[Path, Path]:
    """Write a one-tray pack. ``cores`` maps ``(chip, physical core)`` to its parts."""
    # The column decides the mask: boundary 0, middle 0x084. A chip the test
    # leaves out of ``cores`` is the other mask, so both cases are catalogued.
    used = {c for c, _p in (cores or {})}
    masks = {c: mask for c in used} | {c: mask ^ 0x084 for c in range(CHIPS) if c not in used}
    meta = _meta(masks)
    blobs: list[BlobInfo] = []
    for (c, p), spec in (cores or {}).items():
        _fill(meta, blobs, c, p, spec)
    meta["general.bwgt_size"] = len(bwgt)
    meta["general.bwgt_sha256"] = __import__("hashlib").sha256(bwgt).hexdigest()
    buff, weight = tmp / "layer.buff", tmp / "layer.bwgt"
    if bwgt:
        blobs.append(BlobInfo("mmem.blob", BlobKind.WEIGHT, len(bwgt), 0, bwgt))
    write_bwgt(weight, blobs)
    write_buff(buff, meta, blobs)
    write_checksum_sidecar(buff)
    write_checksum_sidecar(weight)
    return buff, weight


def _fill(meta, blobs, c, p, spec) -> None:
    want = 0 if c % 4 in (0, 3) else 0x084
    h = next(
        i for i in range(meta[f"harvest.{c}.case_count"])
        if meta[f"harvest.{c}.{i}.disable_mask"] == want
    )
    if "release" in spec:
        _blob(meta, blobs, f"router.{c}.{h}.{p}.release.blob", BlobKind.ROUTE,
              struct.pack("<I", spec["release"]))
    if "path" in spec:
        _blob(meta, blobs, f"router.{c}.{h}.{p}.path.blob", BlobKind.ROUTE,
              struct.pack("<256Q", *spec["path"]))
    if "stream_init" in spec:
        _blob(meta, blobs, f"router.{c}.{h}.{p}.stream_init.blob", BlobKind.REG,
              struct.pack("<I", spec["stream_init"]))
    chain = b"".join(struct.pack("<Q", pack_entry(*(item or (0, 0))))
                     for item in spec.get("task", []))
    if chain:
        _blob(meta, blobs, f"ts.{c}.{h}.{p}.task_chain.blob", BlobKind.TS, chain)
    if "datain" in spec:
        pc, attr = spec["datain"]
        _blob(meta, blobs, f"ts.{c}.{h}.{p}.datain_task.blob", BlobKind.TS,
              struct.pack("<Q", pack_entry(pc, attr)))
    for name in ("stream_num", "self_start", "init_finish"):
        if name in spec and spec[name] is not None:
            _blob(meta, blobs, f"ts.{c}.{h}.{p}.{name}.blob", BlobKind.TS,
                  struct.pack("<I", spec[name]))
    if spec.get("smem"):
        meta[f"smem.{c}.{h}.{p}.span.blob"] = f"smem.{p}"
        meta[f"smem.{c}.{h}.{p}.span.offset"] = spec.get("smem_off", 0)
        blobs.append(BlobInfo(f"smem.{p}", BlobKind.DATA, len(spec["smem"]), 0, spec["smem"]))
    if spec.get("itcm"):
        meta[f"itcm.vu.{c}.{h}.{p}.span.blob"] = f"itcm.{p}"
        meta[f"itcm.vu.{c}.{h}.{p}.span.offset"] = spec.get("itcm_off", 0)
        blobs.append(BlobInfo(f"itcm.{p}", BlobKind.KERNEL, len(spec["itcm"]), 0, spec["itcm"]))
    if spec.get("dtcm"):
        meta[f"dtcm.vu.{c}.{h}.{p}.span.blob"] = f"dtcm.{p}"
        meta[f"dtcm.vu.{c}.{h}.{p}.span.offset"] = spec.get("dtcm_off", 0)
        blobs.append(BlobInfo(f"dtcm.{p}", BlobKind.DATA, len(spec["dtcm"]), 0, spec["dtcm"]))
    if spec.get("mmem_off") is not None:
        meta[f"mmem.{c}.{h}.{p}.span.blob"] = "mmem.blob"
        meta[f"mmem.{c}.{h}.{p}.span.offset"] = spec["mmem_off"]
    for off, words in spec.get("dsa", []):
        data = struct.pack(f"<2I{len(words)}I", off, len(words), *words)
        _blob(meta, blobs, f"dsa.dte.{c}.{h}.{p}.runs.blob", BlobKind.REG, data)


def task(**fields) -> int:
    fields.setdefault("TASK_VALID", 1)
    return pack_attr(TASK_CHAIN_ATTR, fields)


def datain(**fields) -> int:
    fields.setdefault("TASK_VALID", 1)
    fields.setdefault("TASK_SEND_UNIT", 0)
    return pack_attr(DATAIN_TASK_ATTR, fields)


def route(**fields) -> int:
    fields.setdefault("VALID", 1)
    return pack_word(fields)


def release(*rows) -> int:
    return pack_rows(list(rows))


def zero_path() -> list[int]:
    return [0] * 256
