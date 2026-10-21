"""Latch station of each hardware face, taken from the topology.

Latch names a core's three router-to-router stations mid, left and right
(``Dir`` 0, 1, 2 in the recovered model). On the Bach 2x5 grid those stations
are the faces the topology actually connects:

* mid is the vertical face, south on the top row and north on the bottom row
* left is west, and also north on the north port core (the chip's north link
  attaches to that core's west station)
* right is east, and also south on the south port core

Which physical core carries a chip port comes from ``hw.chip_ports``. A face
the topology does not connect has no station, so a release or a route that
uses it cannot be expressed and is reported rather than dropped.
"""

from __future__ import annotations

from bachflow.hw import HardwareConfig
from bachflow.hw.types import Direction

MID, LEFT, RIGHT = 0, 1, 2
STATION_BIT = {MID: 1 << MID, LEFT: 1 << LEFT, RIGHT: 1 << RIGHT}
CORE_BIT = 1 << 3


class Faces:
    def __init__(self, hw: HardwareConfig) -> None:
        rows, cols = hw.topology.core_shape
        if (rows, cols) != (2, 5):
            raise ValueError(f"latch stations are defined for a 2x5 core, not {rows}x{cols}")
        ports = hw.chip_ports
        self._of: dict[int, dict[Direction, int]] = {}
        for p in range(rows * cols):
            row, col = divmod(p, cols)
            face: dict[Direction, int] = {}
            if row == 0:
                face[Direction.S] = MID
            else:
                face[Direction.N] = MID
            if col > 0:
                face[Direction.W] = LEFT
            if col + 1 < cols:
                face[Direction.E] = RIGHT
            if p == ports.north:
                face[Direction.N] = LEFT
            if p == ports.south:
                face[Direction.S] = RIGHT
            if p == ports.east:
                face[Direction.E] = RIGHT
            if p == ports.west:
                face[Direction.W] = LEFT
            self._of[p] = face

    def station(self, p: int, direction: Direction) -> int | None:
        """The latch station ``direction`` leaves ``p`` on, or None."""
        return self._of[p].get(direction)

    def faces(self, p: int) -> dict[Direction, int]:
        return dict(self._of[p])
