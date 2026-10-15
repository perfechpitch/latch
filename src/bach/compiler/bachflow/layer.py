"""One packed layer, read through the runtime and nothing else.

Inputs are the ``.buff``, the ``.bwgt``, their ``.sha256`` sidecars and, optionally,
a hardware TOML. The runtime verifies the sidecars, ``general.bwgt_sha256`` and
``general.bwgt_size`` while opening, and resolves the harvest case from the
packed ``disable_mask``.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from bachflow.hw import HardwareConfig, load_hardware
from bachflow.hw.types import ChipId
from bachflow_runtime import Runtime
from bachflow_runtime.load import Extent

# Latch's ShareMem is 32 KiB (share_mem.h kShareMemBytes). The compiled segment
# is 128 KiB. A span past 32 KiB cannot be loaded, so it fails the conversion.
SMEM_LIMIT = 32 * 1024

# Router window + 0x024. pcode writes STREAM_NUM here; the model has no register
# and no write path for it, and its stream table resets to 16.
STREAM_INIT_RESET = 16

ROLE = {"normal": 0, "bcore": 1, "rcore": 2, "unused": 3}


@dataclass
class Core:
    chip: int
    p: int
    logical: int
    role: str
    bad: bool
    extents: dict[str, Extent] = field(default_factory=dict)

    def ext(self, name: str) -> Extent | None:
        return self.extents.get(name)


@dataclass
class Chip:
    c: int
    coord: tuple[int, int, int]
    mask: int
    cores: list[Core]


@dataclass
class Layer:
    runtime: Runtime
    buff: Path
    bwgt: Path
    chips: list[Chip]

    def close(self) -> None:
        for chip in self.chips:
            for core in chip.cores:
                core.extents = {}
        self.runtime.close()


# Boundary columns are all good. A middle column's physical bad cores are 2 and 7.
BOUNDARY_MASK = 0
MIDDLE_MASK = 0x084


def open_layer(buff: str | Path, bwgt: str | Path, hw: str | Path | None) -> Layer:
    """Open a packed layer.

    ``hw`` None accepts only the Bach mesh the file itself declares: chip shape
    4x4 and core shape 2x5, with the file's tray count. Any other shape needs
    ``--hw``. The bound harvest is the physical mask of the chip's column, not
    whichever case was packed first.
    """
    buff, bwgt = Path(buff), Path(bwgt)
    config = _hardware(buff, bwgt, hw)
    rt = Runtime.open(buff, bwgt, config)
    chips = []
    cores = []
    cols = int(rt.meta["hw.chip_shape"][1])
    try:
        for c in range(rt.chip_count):
            tray, y, x = rt.coord(c)
            mask = BOUNDARY_MASK if x in (0, cols - 1) else MIDDLE_MASK
            rt.bind({c: mask})
            if rt.disable_mask(c) != mask:
                raise ValueError(
                    f"chip {c}: bound mask {rt.disable_mask(c):#x} != {mask:#x}"
                )
            cores = []
            for p in range(rt.cores_per_chip):
                cores.append(Core(
                    chip=c,
                    p=p,
                    logical=rt.logical(c, p),
                    role=rt.role(c, p),
                    bad=bool(mask & (1 << p)),
                    extents={e.name: e for e in rt.extents(c, p)},
                ))
            chips.append(Chip(c, (tray, y, x), mask, cores))
    except Exception:
        for core in cores:
            core.extents = {}
        for chip in chips:
            for core in chip.cores:
                core.extents = {}
        chips.clear()
        rt.close()
        raise
    return Layer(rt, buff.resolve(), bwgt.resolve(), chips)


def _hardware(buff: Path, bwgt: Path, hw: str | Path | None) -> HardwareConfig:
    if hw is not None:
        return load_hardware(hw)
    from bachflow_runtime.file import BuffMap

    with BuffMap.open(buff, bwgt) as files:
        shape = [int(n) for n in files.meta["hw.chip_shape"]]
        core = [int(n) for n in files.meta["hw.core_shape"]]
        if shape != [4, 4] or core != [2, 5]:
            raise ValueError(
                f"packed hw.chip_shape={shape} hw.core_shape={core} is not the Bach "
                "4x4 / 2x5 mesh; pass --hw for this layer"
            )
        trays = int(files.meta["hw.chip_count"]) // (shape[0] * shape[1])
    return HardwareConfig.bach_default(tray_count=trays)


def chip_of(layer: Layer, c: int) -> ChipId:
    tray, y, x = layer.chips[c].coord
    return ChipId(tray, y, x)
