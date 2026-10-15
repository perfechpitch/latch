"""Package a packed layer as a BACHIR 11 bundle the baseline loader accepts.

``convert`` writes BACHIR 12. That text names files by span, carries a core map,
and gives every imaged core its true role. The baseline ``LoadBundle`` (latch
pcode branch, BACHIR 10 and 11) rejects those records and checks a core's role
against the task records it also carries. This module is the boundary between
the two: it runs the current conversion, then rewrites the text.

What the baseline keeps is left as the converter wrote it. Task chains, route
tables, release rows, DTE tables, path maps and DSA words are copied through.
What the baseline does not know is moved to a sidecar and is not loaded:

* ``COREMAP`` is the true logical index. The ``.bachir`` ``CORE`` role is not
  that truth for an inactive core (below). Both live in ``core-map.txt``.
* ``FORMAT`` and ``ABI`` name a weight layout and a calling convention. The
  wrapper sets the ABI itself; weight records are a separate conversion.
* ``FILESPAN`` is a file range. Images become Intel HEX beside the bundle, and
  ShareMem becomes ``SMEM`` words.
* ``INITFINISH`` is not a baseline record. The restored loader calls
  ``InitFinish`` only when the core has a task chain or a datain task, so an
  inactive core is left unfinished on purpose. A packed finish word other than
  1 on a core that does have a task is rejected rather than dropped.

An inactive core is one with no ``CFGMISC``, no task chain and no datain task.
The baseline role check rejects an inactive compute, broadcast or reduce core,
and the restored ``HasTask`` guard means such a core never reaches
``InitFinish``. The ``.bachir`` therefore calls that core spare (role 3). The
sidecar keeps the packed role. A core that is inactive but still carries a task
record is not that shape and fails the packaging.

Images are the packed ITCM and DTCM bytes, at the address the kernel was linked
for: ITCM at 0, DTCM at ``0x100000``. Nothing is relocated or recompiled. Bytes
the pack already stored as zero, including BSS, stay zero in the HEX. One HEX
file is written per distinct (unit, content). A core names the file for each
unit it actually has; a unit the pack left out is absent, not filled with an
idle image.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from pathlib import Path

from bachflow.segments import SEGMENT_BY_NAME

from .convert import HEADER, ConvertError, _convert
from .layer import open_layer

BACHIR11 = "BACHIR 11"
# Where the BachFlow firmware sees DTCM. The baseline maps the same bytes at
# 0x20000 as well; writing the alias is what the packed image was linked for.
DTCM_HEX_BASE = 0x0010_0000
ITCM_LIMIT = 4 * 1024
DTCM_LIMIT = 8 * 1024
UNITS = ("dte", "mu", "vu")
# Records the baseline loader parses. Everything else in the BACHIR 12 text is
# either rewritten here or rejected as unknown.
KEPT = frozenset({
    "CHIP", "CORE", "CFGMISC", "TCHAIN", "DATAIN", "DTEIN", "TSRTAB",
    "PATHTASK", "RELROUTE", "RTABDTE", "VUSTATIC", "DSAREG", "RTAB",
})
DROPPED = frozenset({"COREMAP", "FORMAT", "ABI", "FILESPAN", "INITFINISH"})
# A packed finish word the loader will not honor. 1 is what it writes itself
# once a core has a task. Anything else would be lost, so it is not packaged.
FINISH_HONORED = 1


class PackageError(ConvertError):
    """The packed layer cannot be packaged for the baseline loader."""


@dataclass
class InputBundle:
    """One packaged topology, plus the sidecar the ``.bachir`` cannot carry."""

    name: str
    out_dir: Path
    bachir: Path
    core_map: Path
    inventory: Path
    text: str
    cores: list[dict] = field(default_factory=list)
    images: list[dict] = field(default_factory=list)
    init_finish: list[dict] = field(default_factory=list)
    experimental: str | None = None
    legacy_reduces: list[dict] = field(default_factory=list)
    missing: list[str] = field(default_factory=list)

    def as_dict(self) -> dict:
        out = {
            "name": self.name,
            "bachir": self.bachir.name,
            "core_map": self.core_map.name,
            "cores": self.cores,
            "images": self.images,
            "init_finish": self.init_finish,
            "missing": self.missing,
            "compat": _COMPAT,
        }
        if self.experimental:
            out["experimental"] = self.experimental
            out["legacy_reduces"] = self.legacy_reduces
        return out


_COMPAT = [
    "BACHIR 11 sets DteInboundDropsTail; the wrapper clears it after LoadBundle. "
    "The baseline DTE mode mask stays 0x3bf.",
    "An inactive core is CORE role 3 in the .bachir. core-map.txt keeps the packed "
    "role. The restored loader calls InitFinish only when HasTask is set.",
    "No ABI record. The wrapper sets the BachFlow ABI on every core that has a "
    "KERNELCORE image, before execution.",
    "Weights are not in this bundle. FORMAT and mmem spans are inventory only.",
    "A retained LEGACY_REDUCE_MAPPING comment is the experimental simulator "
    "pre-merge. It adds a rounding boundary and does not change the packed route.",
]


def package_input(
    layer,
    out_dir,
    *,
    legacy_reduce_mapping: bool = False,
    name: str = "ep1_adapter",
) -> InputBundle:
    """Write ``out_dir/name/`` as a BACHIR 11 bundle.

    ``layer`` is an open packed layer (``open_layer``) or ``(buff, bwgt)``. The
    current converter runs first. The experimental reduce pre-merge requires
    explicit ``legacy_reduce_mapping=True``; the default keeps strict rejection.
    A failure of that conversion, or of a check below, writes nothing.
    """
    owns = False
    if isinstance(layer, (tuple, list)):
        layer = open_layer(layer[0], layer[1], None)
        owns = True
    try:
        bundle = _convert(layer, legacy_reduce_mapping=legacy_reduce_mapping)
        return _package(layer, bundle, Path(out_dir), name)
    finally:
        if owns:
            layer.close()


def _package(layer, bundle, out_dir: Path, name: str) -> InputBundle:
    records = _records(bundle.text)
    cores = _cores(records)
    _check_inactive(records, cores)
    finish = _finish(records, cores)
    images = _images(layer, cores)
    smem = _smem(layer, cores)
    text = _render(bundle.text, records, cores, images, smem)
    return _write(out_dir, name, text, bundle, cores, images, finish)


def _records(text: str) -> list[tuple[str, list[str], str]]:
    """``(tag, fields, raw line)`` for every record. Comments stay attached."""
    out = []
    for raw in text.splitlines():
        if not raw or raw.startswith("#"):
            out.append(("#", [], raw))
            continue
        fields = raw.split()
        out.append((fields[0], fields, raw))
    if not out or out[0][0] != "BACHIR" or out[0][2] != HEADER:
        raise PackageError(f"converter text does not start with {HEADER}")
    return out


@dataclass
class _Core:
    chip: int
    p: int
    role: int
    logical: int
    bad: bool
    active: bool
    loader_role: int


def _cores(records) -> dict[tuple[int, int], _Core]:
    logical = {}
    roles = {}
    for tag, fields, raw in records:
        if tag == "COREMAP":
            chip, p, value = (int(fields[1]), int(fields[2]), int(fields[3]))
            logical[chip, p] = value
        elif tag == "CORE":
            roles[int(fields[1]), int(fields[2])] = int(fields[3])
    if set(logical) != set(roles):
        raise PackageError("CORE and COREMAP do not name the same cores")
    active = _active(records)
    out = {}
    for key, role in roles.items():
        if role not in (0, 1, 2, 3):
            raise PackageError(f"chip {key[0]} core {key[1]}: role {role} is not 0..3")
        bad = logical[key] < 0
        is_active = key in active
        if bad and is_active:
            raise PackageError(
                f"chip {key[0]} core {key[1]}: a bad core carries a task record"
            )
        # The loader checks the .bachir role. An inactive good core cannot be
        # compute, broadcast or reduce there, so the record says spare.
        loader = role if is_active or bad else 3
        out[key] = _Core(*key, role, logical[key], bad, is_active, loader)
    return out


def _active(records) -> set[tuple[int, int]]:
    """Cores the converter gave a task configuration.

    That is exactly the cores with a chain or a datain task: the converter emits
    ``CFGMISC`` and ``DTEIN`` for those and for no one else. The set is the
    chain and the datain, not the config, so a stray config is still caught.
    """
    out = set()
    for tag, fields, _raw in records:
        if tag in ("TCHAIN", "DATAIN"):
            out.add((int(fields[1]), int(fields[2])))
    return out


def _check_inactive(records, cores: dict[tuple[int, int], _Core]) -> None:
    """An inactive core may carry routes and nothing that configures a task.

    The baseline spare check rejects ``CFGMISC``, ``TCHAIN``, ``DATAIN``,
    ``DTEIN`` and ``VUSTATIC``. A core classified inactive that still has one of
    those is not spare, and calling it spare would hide the record.
    """
    banned = {"CFGMISC", "TCHAIN", "DATAIN", "DTEIN", "VUSTATIC"}
    for tag, fields, _raw in records:
        if tag not in banned:
            continue
        key = (int(fields[1]), int(fields[2]))
        core = cores.get(key)
        if core is None:
            raise PackageError(f"{tag} names chip {key[0]} core {key[1]}, which has no CORE")
        if not core.active:
            raise PackageError(
                f"chip {key[0]} core {key[1]}: inactive core still has {tag}; "
                "it cannot be packaged as spare and it cannot keep a compute role"
            )


def _finish(records, cores) -> list[dict]:
    """Packed ``TS_INIT_FINISH`` words. Not loaded.

    The restored loader writes the finish itself, and only on a core that has a
    task. A packed 1 on such a core is what that call produces. A packed 0, or
    any other value, would be overwritten, so the packaging stops. An inactive
    core has no finish record: the converter only emits one beside a task.
    """
    out = []
    seen = set()
    for tag, fields, _raw in records:
        if tag != "INITFINISH":
            continue
        key = (int(fields[1]), int(fields[2]))
        word = int(fields[3], 0)
        core = cores[key]
        if not core.active:
            raise PackageError(
                f"chip {key[0]} core {key[1]}: INITFINISH on a core with no task"
            )
        if word != FINISH_HONORED:
            raise PackageError(
                f"chip {key[0]} core {key[1]}: packed TS_INIT_FINISH is {word}, "
                "not 1; the baseline loader writes 1 itself and would ignore it"
            )
        seen.add(key)
        out.append({"chip": key[0], "p": key[1], "word": word, "loaded": False})
    missing = [key for key, core in cores.items() if core.active and key not in seen]
    if missing:
        chip, p = missing[0]
        raise PackageError(
            f"chip {chip} core {p}: an active core has no packed TS_INIT_FINISH"
        )
    return out


@dataclass
class _Image:
    """One RV image. ITCM is at 0. DTCM, when the pack has it, is at the alias."""

    unit: str
    itcm: bytes
    dtcm: bytes = b""
    name: str = ""

    @property
    def digest(self) -> str:
        body = self.unit.encode() + b"\0" + self.itcm + b"\0" + self.dtcm
        return hashlib.sha256(body).hexdigest()[:12]


def _image_of(core, unit: str) -> _Image | None:
    itcm = core.ext(f"itcm.{unit}")
    dtcm = core.ext(f"dtcm.{unit}")
    if itcm is None and dtcm is None:
        return None
    if itcm is None:
        raise PackageError(
            f"chip {core.chip} core {core.p}: dtcm.{unit} without itcm.{unit}"
        )
    return _Image(
        unit,
        _span(core, itcm, f"itcm.{unit}", ITCM_LIMIT),
        b"" if dtcm is None else _span(core, dtcm, f"dtcm.{unit}", DTCM_LIMIT),
    )


def _span(core, ext, name: str, limit: int) -> bytes:
    offset = ext.addr - SEGMENT_BY_NAME[name].base
    if offset != 0 or ext.size > limit or ext.size == 0:
        raise PackageError(
            f"chip {core.chip} core {core.p}: {name} is {ext.size} bytes at "
            f"+{offset:#x}, not a non-empty span from 0 within {limit:#x}"
        )
    return bytes(ext.data)


def _images(layer, cores) -> dict[tuple[int, int], dict[str, _Image]]:
    """Packed ITCM and DTCM per active core. One image per unit and content."""
    found: dict[tuple[str, bytes, bytes], _Image] = {}
    per_core: dict[tuple[int, int], dict[str, _Image]] = {}
    for (chip, p), core in cores.items():
        if not core.active:
            continue
        packed = layer.chips[chip].cores[p]
        for unit in UNITS:
            image = _image_of(packed, unit)
            if image is None:
                continue
            shared = found.get((unit, image.itcm, image.dtcm))
            if shared is None:
                found[unit, image.itcm, image.dtcm] = image
                shared = image
            per_core.setdefault((chip, p), {})[unit] = shared
    for image in found.values():
        image.name = f"k_{image.digest}.hex"
    return per_core


def _smem(layer, cores) -> dict[tuple[int, int], list[tuple[int, int]]]:
    """ShareMem as ``(byte offset, little-endian word)``, gaps included.

    The packed span is already contiguous and zero-filled. Every word is emitted,
    zeros included: the baseline does not promise that ShareMem starts at zero,
    and a gap in the record list would leave the previous contents. A span that
    is not a whole number of words, or that does not start on a word, cannot be
    poked as a word.
    """
    out = {}
    for (chip, p), core in cores.items():
        if not core.active:
            continue
        ext = layer.chips[chip].cores[p].ext("smem")
        if ext is None or ext.size == 0:
            continue
        base = ext.addr - SEGMENT_BY_NAME["smem"].base
        if base % 4 or ext.size % 4:
            raise PackageError(
                f"chip {chip} core {p}: smem is {ext.size} bytes at +{base:#x}, "
                "not a run of 32-bit words"
            )
        raw = bytes(ext.data)
        words = [
            (base + 4 * i, int.from_bytes(raw[4 * i:4 * i + 4], "little"))
            for i in range(len(raw) // 4)
        ]
        out[chip, p] = words
    return out


def _render(text, records, cores, images, smem) -> str:
    lines = [BACHIR11]
    for tag, fields, raw in records:
        if tag == "#":
            if raw.startswith("# LEGACY"):
                lines.append(raw)
            continue
        if tag in ("BACHIR", "SOURCE"):
            continue
        if tag in DROPPED:
            continue
        if tag == "CORE":
            core = cores[int(fields[1]), int(fields[2])]
            lines.append(f"CORE {core.chip} {core.p} {core.loader_role}")
            continue
        if tag not in KEPT:
            raise PackageError(f"BACHIR 12 record {tag} has no baseline equivalent")
        lines.append(raw)
    _append_kernels(lines, images)
    _append_smem(lines, smem)
    if "LEGACY_REDUCE_MAPPING" in text and not any(
        ln.startswith("# LEGACY") for ln in lines
    ):
        raise PackageError("the experimental pre-merge marker was lost")
    return "\n".join(lines) + "\n"


def _append_kernels(lines, images) -> None:
    rows = []
    for (chip, p), units in images.items():
        for unit, image in units.items():
            rows.append((chip, p, UNITS.index(unit), image.name))
    for chip, p, rv, name in sorted(rows):
        lines.append(f"KERNELCORE {chip} {p} {rv} {name}")


def _append_smem(lines, smem) -> None:
    for (chip, p), words in sorted(smem.items()):
        for offset, value in words:
            lines.append(f"SMEM {chip} {p} {offset} {value}")


def _write(out_dir, name, text, bundle, cores, images, finish) -> InputBundle:
    dest = out_dir / name
    dest.mkdir(parents=True, exist_ok=True)
    written = {}
    image_rows = []
    for units in images.values():
        for image in units.values():
            if image.name in written:
                continue
            (dest / image.name).write_text(_encode(image), encoding="ascii")
            written[image.name] = image
    for name_hex, image in sorted(written.items()):
        image_rows.append({
            "file": name_hex,
            "unit": image.unit,
            "bytes": len(image.itcm) + len(image.dtcm),
            "itcm": len(image.itcm),
            "dtcm": len(image.dtcm),
        })
    core_rows = [
        {
            "chip": core.chip, "p": core.p, "logical": core.logical,
            "role": core.role, "loader_role": core.loader_role,
            "bad": core.bad, "active": core.active,
        }
        for core in cores.values()
    ]
    result = InputBundle(
        name=name,
        out_dir=dest,
        bachir=dest / f"{name}.bachir",
        core_map=dest / "core-map.txt",
        inventory=dest / "inventory.txt",
        text=text,
        cores=core_rows,
        images=image_rows,
        init_finish=finish,
        experimental=bundle.inventory.as_dict().get("experimental"),
        legacy_reduces=list(bundle.inventory.legacy_reduces),
        missing=list(bundle.inventory.missing),
    )
    result.bachir.write_text(text, encoding="utf-8")
    result.core_map.write_text(_core_map(cores), encoding="utf-8")
    result.inventory.write_text(_inventory(result), encoding="utf-8")
    return result


def _core_map(cores) -> str:
    """``c p logical role``, one core per line, role the packed role.

    ``logical`` is -1 for a bad or unused core. ``role`` is 0 compute, 1
    broadcast, 2 reduce, 3 spare. This is the packed truth, not the role the
    ``.bachir`` told the loader.
    """
    lines = [
        "# c p logical role",
        "# role: 0 compute, 1 broadcast, 2 reduce, 3 spare",
        "# logical -1 is unused or bad; role here is the packed role",
    ]
    for core in cores.values():
        lines.append(f"{core.chip} {core.p} {core.logical} {core.role}")
    return "\n".join(lines) + "\n"


def _inventory(result: InputBundle) -> str:
    lines = [
        f"# {result.name}: sidecar the baseline loader does not read",
        f"# bachir: {result.bachir.name}",
        f"# core-map: {result.core_map.name}  (c p logical packed-role)",
        "# init-finish: packed TS_INIT_FINISH. The loader writes 1 itself on a",
        "# core that has a task and does not call it on a core that has none.",
        "# A packed word other than 1 on an active core is rejected, not stored.",
    ]
    if result.experimental:
        lines.append(f"# experimental: {result.experimental}")
    lines.append("# compat:")
    for note in _COMPAT:
        lines.append(f"# - {note}")
    lines.append("# INIT chip p word")
    for row in result.init_finish:
        lines.append(f"INIT {row['chip']} {row['p']} {row['word']}")
    if result.missing:
        lines.append("# missing:")
        for row in result.missing:
            lines.append(f"# {row}")
    return "\n".join(lines) + "\n"


def encode_ihex(chunks: list[tuple[int, bytes]], width: int = 16) -> str:
    """Intel HEX for ``(address, bytes)`` chunks. Gaps are not filled.

    A data record is at most ``width`` bytes and never crosses a 64 KiB
    boundary. An address at or above 64 KiB emits a type 04 extended linear
    address record first. The file ends with a type 01 record.
    """
    if width < 1 or width > 255:
        raise PackageError(f"HEX record width {width} is outside 1..255")
    lines = []
    upper = None
    for address, data in sorted(chunks):
        if address < 0 or address > 0xFFFF_FFFF:
            raise PackageError(f"HEX address {address:#x} is outside 32 bits")
        offset = 0
        while offset < len(data):
            here = address + offset
            room = 0x1_0000 - (here & 0xFFFF)
            take = min(width, room, len(data) - offset)
            high = here >> 16
            if high != upper:
                lines.append(_record(0, 0x04, bytes((high >> 8, high & 0xFF))))
                upper = high
            lines.append(_record(here & 0xFFFF, 0x00, data[offset:offset + take]))
            offset += take
    lines.append(_record(0, 0x01, b""))
    return "\n".join(lines) + "\n"


def _encode(image: _Image) -> str:
    chunks = [(0, image.itcm)]
    if image.dtcm:
        chunks.append((DTCM_HEX_BASE, image.dtcm))
    return encode_ihex(chunks)


def _record(address: int, kind: int, data: bytes) -> str:
    body = bytes((len(data), (address >> 8) & 0xFF, address & 0xFF, kind)) + data
    checksum = (-sum(body)) & 0xFF
    return ":" + (body + bytes((checksum,))).hex().upper()


def decode_ihex(text: str) -> dict[int, int]:
    """Bytes a baseline ``IhexParser`` would keep, keyed by absolute address.

    The checks match that parser: line length, checksum, and record types
    00..05. A type 04 record sets the high 16 bits. A type 02 record sets a
    segment address. Bytes the file does not name are absent, not zero.
    """
    data: dict[int, int] = {}
    upper = 0
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line[0] != ":" or len(line) < 11 or len(line) % 2 != 1:
            raise PackageError(f"not an Intel HEX record: {line!r}")
        try:
            raw_bytes = bytes.fromhex(line[1:])
        except ValueError as exc:
            raise PackageError(f"not an Intel HEX record: {line!r}") from exc
        count = raw_bytes[0]
        if len(raw_bytes) != count + 5:
            raise PackageError(f"HEX record length {count} does not match {line!r}")
        if (-sum(raw_bytes[:-1])) & 0xFF != raw_bytes[-1]:
            raise PackageError(f"HEX checksum mismatch: {line!r}")
        address = (raw_bytes[1] << 8) | raw_bytes[2]
        kind = raw_bytes[3]
        payload = raw_bytes[4:-1]
        if kind == 0x00:
            at = upper + address
            for byte in payload:
                data[at] = byte
                at += 1
        elif kind == 0x01:
            break
        elif kind == 0x02 and count == 2:
            upper = ((payload[0] << 8) | payload[1]) << 4
        elif kind == 0x04 and count == 2:
            upper = ((payload[0] << 8) | payload[1]) << 16
        elif kind not in (0x02, 0x03, 0x04, 0x05):
            raise PackageError(f"HEX record type {kind:#x} is unknown")
    return data
