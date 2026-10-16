"""Literal 12-bit CSR words translated to native release stations."""

import importlib
from types import SimpleNamespace

import pytest

from bach.compiler.bachflow import ConvertError
from bach.compiler.bachflow.decode import release_groups
from bach.compiler.bachflow.faces import Faces
from bachflow.hw import HardwareConfig


@pytest.mark.parametrize("p,word,expected", [
    (0, 0x040, {0: 2}),  # south MID release goes out north LEFT
    (0, 0x800, {1: 1}),  # north LEFT release goes out south MID
    (9, 0x004, {0: 4}),  # north MID release goes out south RIGHT
    (9, 0x200, {2: 1}),  # south RIGHT release goes out north MID
    (1, 0x800, {1: 1}),  # west release goes out south
    (6, 0x200, {2: 1}),  # east release goes out north
    (0, 0x010, {}),     # Self is not a forwarded source
    (9, 0x001, {}),
])
def test_release_to_stations(p, word, expected):
    convert = importlib.import_module("bach.compiler.bachflow.convert")
    ext = SimpleNamespace(data=word.to_bytes(4, "little"))
    core = SimpleNamespace(p=p, ext=lambda name: ext)
    assert convert._release(core, Faces(HardwareConfig.bach_default(tray_count=1))) == expected


def test_release_groups_are_high_to_low():
    assert release_groups(0x811) == (8, 1, 1)


def test_overflow_is_not_masked():
    convert = importlib.import_module("bach.compiler.bachflow.convert")
    ext = SimpleNamespace(data=(0x1000).to_bytes(4, "little"))
    core = SimpleNamespace(p=0, ext=lambda name: ext)
    with pytest.raises(ConvertError):
        convert._release(core, Faces(HardwareConfig.bach_default(tray_count=1)))
