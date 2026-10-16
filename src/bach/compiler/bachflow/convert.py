"""Convert one packed BachFlow layer into a BACHIR 12 text bundle.

The bundle is the latch model's input. Every record is either a packed word
decoded through the defs layouts, a file range the runtime already exposes, or
a forward entry derived for a core the planner left without a table. Nothing is
inferred from pcode, from route JSON, or from the old exporter.

A condition the model cannot represent fails the conversion. The text is
returned only after every check has passed, so a failure never leaves a
runnable bundle behind.

``legacy_reduce_mapping`` is off. It is an experimental simulator translation of
one shape the old exporter pre-merged: a forwarding core with two distinct
external lanes and no table CORE injection, sending both packets onward to a
reduce that also adds its own input. The packed routes are not rewritten. The
extra rounding boundary that translation inserts stays visible in the bundle.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from bachflow.hw.topology import build_topology
from bachflow.hw.types import Coord, Direction
from bachflow.router_word import DIR_CORE_MASK, DIRECTION_BITS
from bachflow.segments import SEGMENT_BY_NAME

from .decode import datain_attr, entries, release_groups, router_word, task_attr, u32, u32s
from .faces import CORE_BIT, LEFT, MID, RIGHT, STATION_BIT, Faces
from .layer import ROLE, SMEM_LIMIT, STREAM_INIT_RESET, Core, Layer, chip_of, open_layer

HEADER = "BACHIR 12"

# Memory segments the model copies from a file. mmem and mmem_sf are weight
# images; the rest are raw images.
MAPPED = {"mmem", "mmem_sf"}
RAWCOPY = {
    "smem",
    "itcm.dte", "itcm.mu", "itcm.vu",
    "dtcm.dte", "dtcm.mu", "dtcm.vu",
}
FILE_SEGMENTS = MAPPED | RAWCOPY

UNIT = {"dte": 0, "mu": 1, "vu": 2}
# Native RecvUnit is 0 RV, 1 DSA, 2 both. Packed 3 is not that enum.

# Router word operation 0..3 is the model's Operation. op_type is transfer
# unless the word says reduce.
OP_TRANSFER = 1
OP_REDUCE = 2
# Old exporter reduce VC. Only the synthesized predecessor add uses it. A packed
# reduce keeps the word's own VC, and so does every transfer.
REDUCE_VC = 3


class ConvertError(ValueError):
    """The packed layer cannot be represented. No bundle was written."""


@dataclass
class Route:
    """One RouterTable entry, in the model's field order."""

    path: int
    op_type: int
    flow: int
    cur_vc: int
    nxt_vc: tuple[int, ...]
    mask_en: int
    mask_idx: int
    bypass: int
    need_buffer: int
    stream_need: int
    credit: tuple[int, ...]
    reduce_dtype: int
    reduce_odtype: int
    reduce_in: int
    operation: int
    stall: int
    ext_dst: int
    enters: bool = False
    derived: bool = False
    packed_vc: int = 0

    def line(self, chip: int, p: int) -> str:
        nxt = " ".join(str(v) for v in self.nxt_vc)
        credit = " ".join(str(v) for v in self.credit)
        return (
            f"RTAB {chip} {p} {self.path} {self.op_type} {self.flow} {self.cur_vc} "
            f"{nxt} {self.mask_en} {self.mask_idx} {self.bypass} {self.need_buffer} "
            f"{self.stream_need} {credit} {self.reduce_dtype} {self.reduce_odtype} "
            f"{self.reduce_in} {self.operation} {self.stall} {self.ext_dst}"
        )


def _route_of(path: int, word: int) -> Route:
    f = router_word(word)
    if not f["VALID"]:
        raise ConvertError(f"path {path}: a zero word is not a route")
    flow, lost = _flow(f["directionMask"], None)
    return Route(
        path=path,
        op_type=OP_TRANSFER if f["operation"] == 0 else OP_REDUCE,
        flow=flow,
        cur_vc=f["curVC"],
        nxt_vc=(f["nxtVC"],) * 5,
        mask_en=f["use_core_mask"],
        mask_idx=f["core_mask_idx"],
        bypass=0 if f["directionMask"] & DIR_CORE_MASK else 1,
        need_buffer=f["stallWay"],
        stream_need=0,
        credit=(0, 0, 0, 0, 0, 0, 0, 0),
        reduce_dtype=0,
        reduce_odtype=f["reduceOutPrecision"],
        reduce_in=0,
        operation=f["operation"],
        stall=f["stallWay"],
        ext_dst=0,
        enters=bool(f["directionMask"] & DIR_CORE_MASK),
        packed_vc=f["nxtVC"],
    )


def _flow(direction_mask: int, faces_of) -> tuple[int, list[str]]:
    """Latch flow bits of a hardware direction mask. ``faces_of`` None defers the
    station lookup to the caller, which only happens while building."""
    flow = 0
    lost = []
    for name, bit in DIRECTION_BITS:
        if name == "CORE" or not direction_mask & bit:
            continue
        if faces_of is None:
            continue
        station = faces_of(Direction[name])
        if station is None:
            lost.append(name)
        else:
            flow |= STATION_BIT[station]
    return flow, lost


_PREC_BIT = {Direction.N: 0, Direction.S: 1, Direction.W: 2, Direction.E: 3}
_CORE_PREC = 4


def _dtype(precision: int, lanes: list[tuple[int, int]]) -> int:
    """Input precision of the active lanes only.

    ``lanes`` is ``(native station, hardware precision bit)``. A bit of the
    packed mask whose face is not one of these inputs is unused and ignored.
    One precision on every active lane is 0 or 1. A mix sets bit 8 and marks
    each FP32 station, which is the model's mid, left, right order.
    """
    bits = [((precision >> bit) & 1, lane) for lane, bit in lanes]
    if not bits or all(bit == 0 for bit, _lane in bits):
        return 0
    if all(bit == 1 for bit, _lane in bits):
        return 1
    return 0x100 | sum(1 << lane for bit, lane in bits if bit)


@dataclass
class LegacyReduce:
    """One experimental predecessor pre-merge. Not a packed route change.

    ``forward`` is the core that only forwarded two external packets. ``target``
    is the reduce those packets were headed for. Precisions are the model's
    reduce dtype of each rewritten entry after the translation, not the packed
    word: the packed words are left as decoded.
    """

    chip: int
    path: int
    forward_core: int
    forward_lanes: tuple[int, ...]
    forward_dtype: int
    forward_odtype: int
    target_core: int
    target_lane: int
    target_own: int
    target_dtype: int
    target_odtype: int
    packed_target_operation: int
    forward_vc: int
    target_vc: int

    def as_dict(self) -> dict:
        return {
            "chip": self.chip,
            "path": self.path,
            "forward_core": self.forward_core,
            "forward_lanes": list(self.forward_lanes),
            "forward_dtype": self.forward_dtype,
            "forward_odtype": self.forward_odtype,
            "forward_vc": self.forward_vc,
            "target_core": self.target_core,
            "target_lane": self.target_lane,
            "target_own": self.target_own,
            "target_dtype": self.target_dtype,
            "target_odtype": self.target_odtype,
            "target_vc": self.target_vc,
            "packed_target_operation": self.packed_target_operation,
            "experimental": True,
            "note": (
                "simulator-only predecessor add; packed route unchanged; "
                "forward output precision follows the target input and is an "
                "extra rounding boundary; forward VC is the old reduce VC, "
                "the target keeps its packed VC"
            ),
        }


@dataclass
class Inventory:
    """What the packed layer carries, and what it does not."""

    cores: list[dict] = field(default_factory=list)
    missing: list[str] = field(default_factory=list)
    legacy_reduces: list[dict] = field(default_factory=list)

    def as_dict(self) -> dict:
        out = {"cores": self.cores, "missing": self.missing}
        if self.legacy_reduces:
            out["experimental"] = (
                "legacy_reduce_mapping rewrote simulator entries only; "
                "packed routes and the native router are unchanged"
            )
            out["legacy_reduces"] = self.legacy_reduces
        return out


@dataclass
class Bundle:
    text: str
    inventory: Inventory


def convert(buff, bwgt, hw=None, *, legacy_reduce_mapping: bool = False) -> Bundle:
    """Convert one packed layer. Raises ``ConvertError`` with no bundle on failure.

    ``legacy_reduce_mapping`` defaults off. The default still rejects a reduce
    whose external packets are not one per station. Turned on, it applies the
    old exporter's predecessor add and records each rewrite on the inventory.
    """
    layer = open_layer(buff, bwgt, hw)
    try:
        return _convert(layer, legacy_reduce_mapping=legacy_reduce_mapping)
    finally:
        _drop_views(layer)
        layer.close()


def _drop_views(layer) -> None:
    """Release every extent view so the runtime can unmap on the way out."""
    for chip in layer.chips:
        for core in chip.cores:
            core.extents = {}


def _convert(layer: Layer, *, legacy_reduce_mapping: bool = False) -> Bundle:
    hw = layer.runtime.hw
    faces = Faces(hw)
    topo = build_topology(hw)
    problems: list[str] = []
    routes: dict[tuple[int, int], dict[int, Route]] = {}
    releases: dict[tuple[int, int], dict[int, int]] = {}

    for chip in layer.chips:
        for core in chip.cores:
            try:
                routes[(chip.c, core.p)] = _routes(core, faces)
                releases[(chip.c, core.p)] = _release(core, faces)
            except ConvertError as exc:
                problems.append(f"chip {chip.c} core {core.p}: {exc}")
    if problems:
        raise ConvertError("\n".join(problems))

    _derive_passthrough(layer, topo, faces, routes, releases, problems)
    legacy: list[LegacyReduce] = []
    if legacy_reduce_mapping:
        legacy = _legacy_reduce_mapping(layer, topo, faces, routes, problems)
    _check_same_face(layer, topo, faces, routes, problems)
    _reduce_lanes(layer, topo, faces, routes, problems, legacy)
    _stream_need(layer, topo, faces, routes, problems)
    if problems:
        raise ConvertError("\n".join(problems))

    bundle = _render(layer, faces, routes, releases, problems)
    if legacy:
        _mark_legacy(bundle, legacy)
    bundle.inventory.legacy_reduces = [row.as_dict() for row in legacy]
    return bundle


def _mark_legacy(bundle, legacy) -> None:
    """Name each experimental rewrite in the bundle. The RTAB lines stay data."""
    notes = [
        "# LEGACY_REDUCE_MAPPING experimental: simulator entries only; "
        "packed routes unchanged; forwarder output precision is the target "
        "input precision and is an extra rounding boundary"
    ]
    for row in legacy:
        notes.append(
            f"# LEGACY chip {row.chip} path {row.path}: "
            f"forward p{row.forward_core} lanes {list(row.forward_lanes)} "
            f"dtype {row.forward_dtype} odtype {row.forward_odtype}; "
            f"target p{row.target_core} lane {row.target_lane} own {row.target_own} "
            f"dtype {row.target_dtype} odtype {row.target_odtype}; "
            f"packed target operation {row.packed_target_operation}"
        )
    bundle.text = bundle.text.replace(HEADER + "\n", HEADER + "\n" + "\n".join(notes) + "\n", 1)


def _routes(core: Core, faces: Faces) -> dict[int, Route]:
    ext = core.ext("router.path")
    if ext is None:
        return {}
    words = entries(ext.data)
    if len(words) != 256:
        raise ConvertError(f"router.path has {len(words)} entries, want 256")
    out = {}
    for path, (lo, hi) in enumerate(words):
        word = lo | hi << 32
        if word == 0:
            continue
        route = _route_of(path, word)
        flow, lost = _flow(router_word(word)["directionMask"], faces.faces(core.p).get)
        if lost:
            raise ConvertError(
                f"path {path} uses {lost}, which this core has no station for"
            )
        route.flow = flow
        out[path] = route
    return out


def _release(core: Core, faces: Faces) -> dict[int, int]:
    """Release CSR to ``{incoming station: outgoing station mask}``.

    NS, West and East groups leave on MID, LEFT and RIGHT. The p0 north
    and p9 south ports already use LEFT and RIGHT in ``Faces``. Source bits
    name incoming stations; Self is handled separately by native retirement.
    """
    ext = core.ext("router.release")
    if ext is None:
        return {}
    try:
        groups = release_groups(u32(ext.data))
    except ValueError as exc:
        raise ConvertError(str(exc)) from exc
    out: dict[int, int] = {}
    available = set(faces.faces(core.p).values())
    for station, bits in zip((MID, LEFT, RIGHT), groups):
        if station not in available:
            if bits:
                raise ConvertError(f"release group cites absent station {station}")
            continue
        for bit, src_station in ((1, RIGHT), (2, MID), (3, LEFT)):
            if not bits & (1 << bit):
                continue
            if src_station not in available:
                raise ConvertError(f"release cites absent source station {src_station}")
            out[src_station] = out.get(src_station, 0) | STATION_BIT[station]
    return out


def _legacy_reduce_mapping(layer, topo, faces, routes, problems) -> list[LegacyReduce]:
    """Pre-merge a two-lane forwarder into the reduce it feeds. Experimental.

    Phase one only reads. It keeps a reduce that is short exactly one external
    packet, whose single table sender is a same-chip transfer with no CORE
    injection and exactly one outgoing edge, and whose sender is itself reached
    by two distinct stations of one precision. Phase two writes those records.
    A same-face collision, a shortfall with any other shape, a forwarder that
    injects or fans out, a cross-chip sender, or a precision the one output
    dtype cannot say is not rewritten; the strict checks reject those. The
    forwarder's output dtype is that one input precision, which is the extra
    rounding boundary.
    """
    arrivals = _arrivals(layer, topo, faces, routes)
    pending = _legacy_reduce_records(layer, arrivals, routes)
    return _apply_legacy(layer, routes, pending, problems)


def _legacy_reduce_records(layer, arrivals, routes) -> list[tuple]:
    """Rewrite records, computed before any route is changed.

    A predecessor is the sender on the arrival list, never a chip index chosen
    for this scene. Each tuple is ``(target, sender, predecessor hops, dtype)``.
    """
    out = []
    for (c, p, path), hops in sorted(arrivals.items()):
        route = routes.get((c, p), {}).get(path)
        if route is None or route.derived or route.operation < 2:
            continue
        stations = [lane for lane, _bit, _src in hops]
        if len(stations) != len(set(stations)):
            continue
        if len(stations) != route.operation - 2 or len(hops) != 1:
            continue
        _lane, bit, src = hops[0]
        if src[0] != c:
            continue
        forward = routes.get(src, {}).get(path)
        if (
            forward is None or forward.derived or forward.operation != 0
            or forward.enters or bin(forward.flow).count("1") != 1
        ):
            continue
        pred = arrivals.get((*src, path), [])
        pred_lanes = [item[0] for item in pred]
        if len(pred) != 2 or len(set(pred_lanes)) != 2:
            continue
        # The target's packed mask has one bit for the face the forwarder uses.
        # Both packets the forwarder adds must be that precision. Reading the
        # mask with the forwarder's own faces would invent a mixed dtype, and
        # the model's output precision is one bit, so a mix is rejected.
        precision = _word(layer, c, p, path)["reduceInPrecision"]
        uniform = (precision >> bit) & 1
        if any(((precision >> item[1]) & 1) != uniform for item in pred):
            continue
        dtype = uniform
        out.append(((c, p, path), src, pred, dtype, hops[0][0]))
    return out


def _apply_legacy(layer, routes, pending, problems) -> list[LegacyReduce]:
    """Write the recorded pre-merges. The target's own lane is filled later.

    The forwarder becomes an upstream-only reduce on its two external lanes, on
    VC 3. That VC belongs to this synthesized add only. The target's operation
    drops by one external packet and it keeps the packed VC. Its mask, own lane
    and dtype are left at zero so ``_reduce_lanes`` fills them from the one lane
    that remains; that fill does not compare the lane count with the packed
    operation.
    """
    records = []
    for (c, p, path), src, pred, dtype, external in pending:
        route = routes[c, p][path]
        forward = routes[src][path]
        packed = route.operation
        lanes = tuple(sorted({item[0] for item in pred}))
        forward.op_type = OP_REDUCE
        forward.operation = 2
        forward.cur_vc = REDUCE_VC
        forward.nxt_vc = (REDUCE_VC,) * 5
        forward.reduce_in = sum(1 << lane for lane in lanes)
        forward.reduce_dtype = dtype
        forward.reduce_odtype = dtype
        route.operation = packed - 1
        records.append(LegacyReduce(
            chip=c, path=path,
            forward_core=src[1], forward_lanes=lanes,
            forward_dtype=dtype, forward_odtype=dtype, forward_vc=REDUCE_VC,
            target_core=p, target_lane=external, target_own=-1,
            target_dtype=0, target_odtype=route.reduce_odtype,
            target_vc=route.cur_vc,
            packed_target_operation=packed,
        ))
    return records


def _check_same_face(layer, topo, faces, routes, problems) -> None:
    """Two inputs of one path arriving on one station cannot be told apart.

    The model has one lane per station. Counting both as one lane would change
    the sum, so the conversion stops instead of rewiring the addition.
    """
    arrivals = _arrivals(layer, topo, faces, routes)
    for (c, p, path), lanes in arrivals.items():
        route = routes[c, p].get(path)
        if route is None or route.operation == 0 or route.derived:
            continue
        if len([lane for lane, _bit, _src in lanes]) != len(
            {lane for lane, _bit, _src in lanes}
        ):
            problems.append(
                f"chip {c} core {p} path {path}: two reduce inputs arrive on one "
                "face; the model has one lane per station, so the addition "
                "would change"
            )


def _word(layer: Layer, c: int, p: int, path: int) -> dict:
    ext = layer.chips[c].cores[p].ext("router.path")
    lo, hi = entries(ext.data)[path]
    return router_word(lo | hi << 32)


def _station(layer, c, p, direction) -> int:
    faces = Faces(layer.runtime.hw)
    station = faces.station(p, direction)
    if station is None:
        raise ConvertError(f"chip {c} core {p} has no station for {direction.name}")
    return station


def _derive_passthrough(layer, topo, faces, routes, releases, problems) -> None:
    """Fill a core a path crosses but the planner gave no entry.

    The planner omits the table on a pass-through core. The model drops a packet
    whose path has no entry, so the forward is derived from the release CSR: data
    leaves on the face whose release row names the face it arrived on. The VC is
    the upstream entry's VC. A core that already has the entry is left as packed.
    """
    coord = {
        (c, p): Coord(*layer.chips[c].coord, p)
        for c in range(len(layer.chips))
        for p in range(len(layer.chips[c].cores))
    }
    index = {v: k for k, v in coord.items()}

    pending = True
    while pending:
        pending = False
        for (c, p), table in list(routes.items()):
            for path, route in list(table.items()):
                for direction in _out_dirs(faces, p, route.flow):
                    nxt = topo.neighbor(coord[(c, p)], direction)
                    if nxt is None:
                        continue
                    key = index.get(nxt)
                    if key is None or path in routes.get(key, {}):
                        continue
                    arrived = faces.station(key[1], direction.opposite)
                    if arrived is None:
                        problems.append(
                            f"chip {c} core {p} path {path} goes {direction.name} onto "
                            f"chip {key[0]} core {key[1]}, which has no station there"
                        )
                        continue
                    onward = releases.get(key, {})
                    if not onward:
                        continue
                    fwd = _forward(onward, arrived)
                    if fwd is None:
                        problems.append(
                            f"chip {c} core {p} path {path} goes {direction.name} onto "
                            f"chip {key[0]} core {key[1]}, whose release does not "
                            "forward that face"
                        )
                        continue
                    nxt_vc = route.packed_vc
                    routes.setdefault(key, {})[path] = Route(
                        path=path, op_type=OP_TRANSFER, flow=fwd, cur_vc=nxt_vc,
                        nxt_vc=(nxt_vc,) * 5, mask_en=0, mask_idx=0, bypass=1,
                        need_buffer=0, stream_need=0, credit=(0, 0, 0, 0, 0, 0, 0, 0),
                        reduce_dtype=0, reduce_odtype=0, reduce_in=0, operation=0,
                        stall=0, ext_dst=0, derived=True, packed_vc=nxt_vc,
                    )
                    pending = True


def _out_dirs(faces: Faces, p: int, flow: int) -> list[Direction]:
    return [d for d, station in faces.faces(p).items() if flow & STATION_BIT[station]]


def _forward(release: dict[int, int], arrived: int) -> int | None:
    """Outgoing station mask for data that arrived on ``arrived``.

    Release from face F toward face G means data arrived on G and leaves on F.
    """
    mask = 0
    for src, out in release.items():
        if out & STATION_BIT[arrived]:
            mask |= STATION_BIT[src]
    return mask or None


def _arrivals(layer, topo, faces, routes) -> dict[tuple, list[tuple]]:
    """``(chip, core, path)`` to one hop per table send onto that core.

    A hop is ``(station, precision bit, sender)``. ``sender`` is the ``(chip,
    core)`` whose table names the hop. Release forwarding is not an arrival:
    a core the planner left without a table is filled separately.
    """
    coord = {
        (c, p): Coord(*layer.chips[c].coord, p)
        for c in range(len(layer.chips))
        for p in range(len(layer.chips[c].cores))
    }
    index = {v: k for k, v in coord.items()}
    incoming: dict[tuple, list[tuple]] = {}
    for (c, p), table in routes.items():
        for path, route in table.items():
            for direction in _out_dirs(faces, p, route.flow):
                nxt = topo.neighbor(coord[(c, p)], direction)
                if nxt is None or index.get(nxt) is None:
                    continue
                recv = direction.opposite
                lane = faces.station(index[nxt][1], recv)
                incoming.setdefault((*index[nxt], path), []).append(
                    (lane, _PREC_BIT[recv], (c, p))
                )
    return incoming


def _reduce_lanes(layer, topo, faces, routes, problems, legacy=()) -> None:
    """Name the lanes a reduce entry waits on. The operation is not changed.

    An incoming lane is the station the upstream entry sends on, seen from this
    core. The core's own input takes the lowest free lane, and the reduce flow
    bit records which one. Two arrivals on one station were already rejected.

    A legacy forwarder is the exception. It adds only the external lanes it was
    given and has no own input, so its lane count is not checked against an
    operation: the packed word was a transfer. A legacy target is an ordinary
    reduce of whatever external lanes remain, and this same choice fills the
    record's own lane and dtype.
    """
    incoming = _arrivals(layer, topo, faces, routes)
    rewritten = {(row.chip, row.forward_core, row.path): row for row in legacy}
    targets = {(row.chip, row.target_core, row.path): row for row in legacy}
    for (c, p), table in routes.items():
        for path, route in table.items():
            if route.operation == 0 or route.derived:
                continue
            lanes = incoming.get((c, p, path), [])
            stations = [lane for lane, _bit, _src in lanes]
            if len(stations) != len(set(stations)):
                continue
            row = rewritten.get((c, p, path))
            if row is not None:
                if sorted(stations) != list(row.forward_lanes):
                    problems.append(
                        f"chip {c} core {p} path {path}: legacy forwarder lanes "
                        f"{sorted(stations)} are not the recorded "
                        f"{list(row.forward_lanes)}"
                    )
                continue
            expected = route.operation - 1
            if len(stations) != expected:
                problems.append(
                    f"chip {c} core {p} path {path}: reduce{expected} requires "
                    f"{expected} external packets, but only {len(stations)} distinct "
                    "incoming lanes are represented; same-face packet multiplicity "
                    "is unsupported and must not be silently reduced"
                )
                continue
            own = next((n for n in (MID, 1, 2) if n not in stations), None)
            if own is None:
                problems.append(
                    f"chip {c} core {p} path {path}: all three stations carry an "
                    "input, so the core's own input has no lane"
                )
                continue
            mask = 1 << own
            for lane in stations:
                mask |= 1 << lane
            route.reduce_in = mask
            route.reduce_dtype = _dtype(
                _word(layer, c, p, path)["reduceInPrecision"],
                [(lane, bit) for lane, bit, _src in lanes] + [(own, _CORE_PREC)],
            )
            if own == 1:
                route.flow |= 1 << 3
            elif own == 2:
                route.flow |= 1 << 4
            target = targets.get((c, p, path))
            if target is not None:
                target.target_own = own
                target.target_dtype = route.reduce_dtype
                target.target_odtype = route.reduce_odtype


def _stream_need(layer, topo, faces, routes, problems) -> None:
    """Stream credit follows the packed streamCheck, not a recomputed mask.

    streamCheck set means every direction the entry sends on takes credit, and
    so does the core station when the entry enters the core. The model has no
    separate core bit in flow, so the core station is bit 3.
    """
    for (c, p), table in routes.items():
        for path, route in table.items():
            if route.derived:
                continue
            if not _word(layer, c, p, path)["streamCheck"]:
                continue
            route.stream_need = route.flow & 0x7
            if route.enters:
                route.stream_need |= CORE_BIT


def _render(layer, faces, routes, releases, problems) -> Bundle:
    inv = Inventory()
    lines = [HEADER, f"SOURCE {layer.buff}"]
    lines.append("# CHIP <chip> <disable_mask>")
    for chip in layer.chips:
        lines.append(f"CHIP {chip.c} 0x{chip.mask:03x}")

    lines.append("# CORE <chip> <core> <role 0 compute 1 broadcast 2 reduce 3 spare>")
    lines.append("# COREMAP <chip> <core> <logical, -1 unused or bad>")
    for chip in layer.chips:
        for core in chip.cores:
            role = ROLE.get(core.role)
            if role is None:
                problems.append(f"chip {chip.c} core {core.p}: unknown role {core.role!r}")
                continue
            if core.bad and role != ROLE["unused"]:
                problems.append(
                    f"chip {chip.c} core {core.p}: bad core has role {core.role}"
                )
            logical = -1 if core.bad else core.logical
            lines.append(f"CORE {chip.c} {core.p} {role}")
            lines.append(f"COREMAP {chip.c} {core.p} {logical}")
            inv.cores.append({
                "chip": chip.c, "p": core.p, "role": core.role,
                "logical": logical, "bad": core.bad,
                "extents": sorted(core.extents),
            })

    _render_files(layer, lines, problems)
    _render_tasks(layer, lines, problems)
    _render_routes(layer, routes, releases, lines)
    _render_regs(layer, lines, problems)
    if problems:
        raise ConvertError("\n".join(problems))
    inv.missing = _missing(layer)
    return Bundle("\n".join(lines) + "\n", inv)


def _render_files(layer, lines, problems) -> None:
    lines.append(
        "# FILESPAN <chip> <core> <segment> <dest_offset> <file> <file_offset> <bytes>"
    )
    lines.append("# FORMAT <chip> <core> nk")
    lines.append("# ABI <chip> <core> bachflow")
    for chip in layer.chips:
        for core in chip.cores:
            if core.bad:
                continue
            mapped = False
            imaged = False
            for name, ext in core.extents.items():
                if name not in FILE_SEGMENTS or ext.size == 0:
                    continue
                if name == "smem" and ext.addr - SEGMENT_BY_NAME["smem"].base + ext.size > SMEM_LIMIT:
                    problems.append(
                        f"chip {chip.c} core {core.p}: smem span reaches "
                        f"{ext.addr - SEGMENT_BY_NAME['smem'].base + ext.size:#x}, past "
                        f"the model's {SMEM_LIMIT:#x} ShareMem"
                    )
                dest = ext.addr - SEGMENT_BY_NAME[name].base
                lines.append(
                    f"FILESPAN {chip.c} {core.p} {name} {dest} {ext.path.resolve()} {ext.offset} {ext.size}"
                )
                mapped = mapped or name in MAPPED
                imaged = imaged or name.startswith("itcm.")
            if mapped:
                lines.append(f"FORMAT {chip.c} {core.p} nk")
            if imaged or _active(core):
                lines.append(f"ABI {chip.c} {core.p} bachflow")


def _active(core: Core) -> bool:
    chain = core.ext("ts.task_chain")
    if chain is not None and any(attr & (1 << 31) for _pc, attr in entries(chain.data)):
        return True
    datain = core.ext("ts.datain_task")
    return datain is not None and bool(u32s(datain.data)[1] & (1 << 31))


def _render_tasks(layer, lines, problems) -> None:
    lines.append(
        "# CFGMISC <chip> <core> <stream_num> <self_start> <b_core_dir> <trigger_chain_en>"
    )
    lines.append(
        "# TCHAIN <chip> <core> <idx> <task_pc> <unit> <recv_unit> <wait_wake> "
        "<task_type> <p2p_reissue_tid> <credit_en> <path_id> <end>"
    )
    lines.append("# DATAIN <chip> <core> <task_pc> <weights_mode>")
    lines.append("# DTEIN <chip> <core> <route 0 Core Mem 1 Matrix Mem> 0 0 0")
    lines.append("# INITFINISH <chip> <core> <0 write zero, 1 finish>")
    for chip in layer.chips:
        for core in chip.cores:
            if core.bad:
                continue
            chain = _chain(core, problems, chip.c)
            datain = _datain(core, problems, chip.c)
            if core.role in ("bcore", "rcore") and chain and datain is None:
                problems.append(
                    f"chip {chip.c} core {core.p}: {core.role} has tasks but no datain"
                )
            if not chain and datain is None:
                continue
            stream = _word_or(core, "ts.stream_num", 16)
            start = _word_or(core, "ts.self_start", 0)
            lines.append(f"CFGMISC {chip.c} {core.p} {stream} {int(bool(start))} 0x0 1")
            for idx, pc, attr in chain:
                lines.append(
                    f"TCHAIN {chip.c} {core.p} {idx} 0x{pc:x} {attr['TASK_SEND_UNIT']} "
                    f"{attr['TASK_RECV_UNIT']} {attr['WAIT_WAKE']} {attr['TASK_TYPE']} "
                    f"{attr['TASK_P2P_REISSUE_TID']} {attr['TASK_CREDIT_EN']} "
                    f"{attr['TASK_PATH_ID']} {attr['TASK_END']}"
                )
            if datain is not None:
                pc, weights = datain
                lines.append(f"DATAIN {chip.c} {core.p} 0x{pc:x} {weights}")
            route = 0 if core.role == "normal" else 1
            lines.append(f"DTEIN {chip.c} {core.p} {route} 0 0 0")
            _init(core, chip.c, lines, problems)


def _chain(core: Core, problems, chip: int) -> list[tuple[int, int, dict]]:
    """Valid tasks in chain order.

    The chain must be contiguous from task 0. A hole is rejected rather than
    closed over. An R core whose task 0 waits to be woken is the shape the old
    exporter rewrote into DATAIN; this layer never does that, and the packed EP1
    scene has no such chain, so the shape fails the conversion.
    """
    ext = core.ext("ts.task_chain")
    if ext is None:
        return []
    slots = [(pc, task_attr(attr)) for pc, attr in entries(ext.data)]
    valid = [i for i, (_pc, attr) in enumerate(slots) if attr["TASK_VALID"]]
    if not valid:
        return []
    if valid != list(range(len(valid))):
        problems.append(
            f"chip {chip} core {core.p}: the valid tasks are not contiguous"
        )
        return []
    if core.role == "rcore" and slots[0][1]["WAIT_WAKE"]:
        problems.append(
            f"chip {chip} core {core.p}: an R core whose first task waits to be "
            "woken is not converted; the pack carries no such chain to check it "
            "against"
        )
        return []
    out = []
    for idx, (pc, attr) in enumerate(slots[:len(valid)]):
        if attr["TASK_RECV_UNIT"] > 2:
            problems.append(
                f"chip {chip} core {core.p} task {idx}: recv unit "
                f"{attr['TASK_RECV_UNIT']} is not 0, 1 or 2"
            )
        out.append((idx, pc, dict(attr)))
    return out


def _datain(core: Core, problems, chip: int) -> tuple[int, int] | None:
    ext = core.ext("ts.datain_task")
    if ext is None:
        return None
    pc, attr = u32s(ext.data)
    decoded = datain_attr(attr)
    if not decoded["TASK_VALID"]:
        return None
    return pc, decoded["WEIGHTS_MODE"]


def _init(core: Core, chip: int, lines, problems) -> None:
    ext = core.ext("ts.init_finish")
    if ext is None:
        return
    word = u32(ext.data)
    if word not in (0, 1):
        problems.append(
            f"chip {chip} core {core.p}: TS_INIT_FINISH is {word}, not 0 or 1"
        )
        return
    lines.append(f"INITFINISH {chip} {core.p} {word}")


def _word_or(core: Core, name: str, default: int) -> int:
    ext = core.ext(name)
    return u32(ext.data) if ext is not None else default


def _render_routes(layer, routes, releases, lines) -> None:
    lines.append(
        "# RTAB <chip> <core> <path> <op_type> <flow_dir> <cur_vc> <nxt_vc x5> "
        "<mask_en> <mask_idx> <bypass> <need_buffer> <stream_need> <credit x6> "
        "<reduce_dtype> <reduce_odtype> <reduce_in> <operation> <stall> <ext_dst>"
    )
    lines.append("# RTABDTE <chip> <core> <path>")
    lines.append("# TSRTAB <chip> <core> <path> <dir> <vcid>")
    lines.append("# PATHTASK <chip> <core> <path> <task>")
    lines.append("# RELROUTE <chip> <core> <incoming station> <outgoing mask>")
    for chip in layer.chips:
        for core in chip.cores:
            table = routes.get((chip.c, core.p), {})
            for path, route in sorted(table.items()):
                lines.append(route.line(chip.c, core.p))
                if core.bad:
                    continue
                lines.append(f"RTABDTE {chip.c} {core.p} {path}")
                direction = route.flow & 0x7
                if route.enters:
                    direction |= CORE_BIT
                lines.append(f"TSRTAB {chip.c} {core.p} {path} {direction} {route.cur_vc}")
            if not core.bad:
                for path, task in _path_tasks(core).items():
                    lines.append(f"PATHTASK {chip.c} {core.p} {path} {task}")
            for station, mask in sorted(releases.get((chip.c, core.p), {}).items()):
                lines.append(f"RELROUTE {chip.c} {core.p} {station} {mask}")


def _path_tasks(core: Core) -> dict[int, int]:
    """Path to the task an arriving packet wakes.

    The DTE copy of the path map is read when a packet arrives and names the
    chain task that receives it. Only an inbound task is such a receiver:
    broadcast reissue in, P2P reissue in, and a task that waits to be woken.
    A task that only sends is not entered, so the map stays 0 there.
    """
    out = {}
    for idx, _pc, attr in _chain(core, [], core.chip):
        if attr["TASK_TYPE"] not in (2, 3) and not attr["WAIT_WAKE"]:
            continue
        out.setdefault(attr["TASK_PATH_ID"], idx)
    return out


def _render_regs(layer, lines, problems) -> None:
    lines.append("# DSAREG <chip> <core> <unit 0 dte 1 mu 2 vu> <off> <val>")
    for chip in layer.chips:
        for core in chip.cores:
            if core.bad:
                continue
            _stream_init(core, chip.c, problems)
            for unit, number in UNIT.items():
                for name, ext in core.extents.items():
                    if not name.startswith(f"dsa.{unit}.run"):
                        continue
                    base = SEGMENT_BY_NAME[f"dsa.{unit}"].base
                    off = ext.addr - base
                    for i, val in enumerate(u32s(ext.data)):
                        lines.append(
                            f"DSAREG {chip.c} {core.p} {number} {off + 4 * i} {val}"
                        )


def _stream_init(core: Core, chip: int, problems) -> None:
    ext = core.ext("router.stream_init")
    if ext is None:
        return
    word = u32(ext.data)
    if word != STREAM_INIT_RESET:
        problems.append(
            f"chip {chip} core {core.p}: router.stream_init is {word}, not "
            f"{STREAM_INIT_RESET}; the model has no register for Router +0x024 "
            "and resets its stream table to 16, so the value would be lost"
        )


def _missing(layer: Layer) -> list[str]:
    """Endpoints the pack does not carry, named so a harness can supply them."""
    out = []
    for chip in layer.chips:
        for core in chip.cores:
            if core.role in ("bcore", "rcore") and core.ext("ts.datain_task") is None:
                out.append(
                    f"chip {chip.c} core {core.p}: {core.role} names no ingress address; "
                    "the pack has no path into the chip"
                )
    return out
