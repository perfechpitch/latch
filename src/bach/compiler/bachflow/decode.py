"""Packed register words, decoded with the defs layouts and nothing else.

The Router word and the TS ATTR come from ``bachflow.router_word`` and
``bachflow.ts_attr``. The release CSR comes from ``bachflow.hw.release``.
Field positions are not copied here.
"""

from __future__ import annotations

import struct

from bachflow.hw.release import unpack_rows
from bachflow.router_word import unpack_word
from bachflow.ts_attr import DATAIN_TASK_ATTR, TASK_CHAIN_ATTR, unpack_attr


def u32(data: bytes | memoryview) -> int:
    if len(data) != 4:
        raise ValueError(f"expected 4 bytes, got {len(data)}")
    return struct.unpack("<I", bytes(data))[0]


def u32s(data: bytes | memoryview) -> list[int]:
    if len(data) % 4:
        raise ValueError(f"word blob is {len(data)} bytes, not a multiple of 4")
    return list(struct.unpack(f"<{len(data) // 4}I", bytes(data)))


def entries(data: bytes | memoryview) -> list[tuple[int, int]]:
    """64-bit little-endian words as ``(pc, attr)`` pairs, in index order."""
    if len(data) % 8:
        raise ValueError(f"entry blob is {len(data)} bytes, not a multiple of 8")
    out = []
    for i in range(0, len(data), 8):
        word = struct.unpack_from("<Q", bytes(data), i)[0]
        out.append((word & 0xFFFF_FFFF, word >> 32))
    return out


def task_attr(attr: int) -> dict[str, int]:
    return unpack_attr(TASK_CHAIN_ATTR, attr)


def datain_attr(attr: int) -> dict[str, int]:
    return unpack_attr(DATAIN_TASK_ATTR, attr)


def router_word(word: int) -> dict[str, int]:
    return unpack_word(word)


def release_rows(word: int) -> tuple[int, int, int, int]:
    """Four 5-bit rows, low to high: Up, Down, Left, Right."""
    return unpack_rows(word & ((1 << 20) - 1))
