"""BACHIR 11 packaging of a converted packed layer."""

from __future__ import annotations

from types import SimpleNamespace

import pytest
from bachflow.segments import SEGMENT_BY_NAME
from bach.compiler.bachflow import ConvertError
from bach.compiler.bachflow.legacy_input import (
    PackageError,
    _span,
    decode_ihex,
    encode_ihex,
    package_input,
)
from pack import build, datain, release, route, task, zero_path


def lines_of(text, tag):
    return [ln.split() for ln in text.splitlines() if ln.startswith(tag + " ")]


def test_ihex_roundtrip_keeps_bytes_addresses_and_a_short_tail():
    itcm = bytes(range(20))
    dtcm = b"\x00\x11\x22"
    text = encode_ihex([(0, itcm), (0x0010_0000, dtcm)])
    assert any(ln[7:9] == "04" for ln in text.splitlines())
    assert text.splitlines()[-1] == ":00000001FF"
    got = decode_ihex(text)
    assert bytes(got[i] for i in range(20)) == itcm
    assert bytes(got[0x0010_0000 + i] for i in range(3)) == dtcm
    assert 20 not in got
    assert 0x0002_0000 not in got


def test_ihex_keeps_bss_zeros_and_does_not_fill_the_gap():
    itcm = b"\x13\x01\x00\x00"
    dtcm = b"\x00" * 8 + b"\x7f"
    got = decode_ihex(encode_ihex([(0, itcm), (0x0010_0000, dtcm)], width=4))
    assert bytes(got[0x0010_0000 + i] for i in range(9)) == dtcm
    assert 4 not in got
    assert 0xFFFF not in got


@pytest.mark.parametrize("segment", ["itcm", "dtcm"])
def test_shifted_image_span_keeps_bytes_at_its_hex_address(tmp_path, segment):
    offset = 0x100
    data = b"\x00\x11\x00\x7f\x00"
    spec = {"task": [(offset + 0x10, task(TASK_SEND_UNIT=2, TASK_END=1))],
            "init_finish": 1, "itcm": b"\x13\x00\x00\x00"}
    spec[segment] = data
    spec[f"{segment}_off"] = offset
    buff, bwgt = build(tmp_path, cores={(0, 1): spec})
    packed = package_input((buff, bwgt), tmp_path / "out", legacy_reduce_mapping=False)
    kernel = lines_of(packed.text, "KERNELCORE")[0]
    image = decode_ihex((packed.out_dir / kernel[4]).read_text())
    base = 0 if segment == "itcm" else 0x0010_0000
    assert bytes(image[base + i] for i in range(offset)) == bytes(offset)
    assert bytes(image[base + offset + i] for i in range(len(data))) == data
    assert base + offset + len(data) not in image
    assert lines_of(packed.text, "TCHAIN")[0][4] == "0x110"


@pytest.mark.parametrize("name,limit", [("itcm.vu", 4096), ("dtcm.vu", 8192)])
@pytest.mark.parametrize("case", ["negative", "overflow", "empty"])
def test_image_span_rejects_invalid_bounds(name, limit, case):
    offset, data = {"negative": (-1, b"\x13"), "overflow": (limit, b"\x13"),
                    "empty": (0, b"")}[case]
    core = SimpleNamespace(chip=0, p=1)
    ext = SimpleNamespace(addr=SEGMENT_BY_NAME[name].base + offset, size=len(data), data=data)
    with pytest.raises(PackageError, match="non-empty span.*within"):
        _span(core, ext, name, limit)


def test_ihex_emits_an_extended_address_past_64k_and_checks_the_sum():
    blob = b"\xab" * 17
    text = encode_ihex([(0x1_0000 - 8, blob)], width=16)
    lines = text.splitlines()
    assert lines[0][7:9] == "04"
    assert decode_ihex(text) == {0x1_0000 - 8 + i: 0xAB for i in range(17)}
    broken = lines[1][:-2] + "00"
    try:
        decode_ihex(broken + "\n")
    except PackageError as exc:
        assert "checksum" in str(exc)
    else:
        raise AssertionError("a bad checksum was accepted")


def _active(tmp):
    path = zero_path()
    path[3] = route(directionMask=0b1_0000 | 0b1000, nxtVC=1, streamCheck=1)
    return build(tmp, cores={(0, 1): {
        "release": release(0, 0, 0, 0),
        "path": path,
        "task": [(0x20, task(TASK_SEND_UNIT=2, TASK_PATH_ID=3, TASK_END=1))],
        "stream_num": 4,
        "self_start": 0,
        "init_finish": 1,
        "itcm": b"\x13\x05\x00\x00\x01",
        "smem": b"\x01\x00\x00\x00\x00\x00\x00\x00",
        "dsa": [(0x10, [7, 8])],
    }})


def test_ordinary_core_becomes_bachir11_without_native_records(tmp_path):
    buff, bwgt = _active(tmp_path)
    packed = package_input((buff, bwgt), tmp_path / "out", legacy_reduce_mapping=False)
    text = packed.text
    assert text.startswith("BACHIR 11\n")
    for tag in ("FILESPAN", "FORMAT", "ABI", "COREMAP", "INITFINISH", "KERNEL "):
        assert lines_of(text, tag.strip()) == [] or tag == "KERNEL "
        assert not any(ln.startswith(tag) for ln in text.splitlines())
    assert lines_of(text, "TCHAIN") == [
        ["TCHAIN", "0", "1", "0", "0x20", "2", "0", "0", "0", "0", "0", "3", "1"],
    ]
    assert lines_of(text, "CFGMISC") == [["CFGMISC", "0", "1", "4", "0", "0x0", "1"]]
    assert lines_of(text, "DSAREG") == [
        ["DSAREG", "0", "1", "0", "16", "7"],
        ["DSAREG", "0", "1", "0", "20", "8"],
    ]
    assert lines_of(text, "SMEM") == [
        ["SMEM", "0", "1", "0", "1"],
        ["SMEM", "0", "1", "4", "0"],
    ]
    assert ["CORE", "0", "1", "0"] in lines_of(text, "CORE")
    kernels = lines_of(text, "KERNELCORE")
    assert len(kernels) == 1 and kernels[0][3] == "2"
    image = decode_ihex((packed.out_dir / kernels[0][4]).read_text())
    assert bytes(image[i] for i in range(5)) == b"\x13\x05\x00\x00\x01"
    assert 0x0010_0000 not in image
    assert "INIT 0 1 1" in packed.inventory.read_text()
    roles = packed.core_map.read_text().splitlines()
    assert "0 1 1 0" in roles


def test_inactive_compute_is_spare_in_the_bundle_and_kept_in_the_sidecar(tmp_path):
    buff, bwgt = build(tmp_path, cores={
        (0, 1): {"task": [(0x10, task(TASK_END=1))], "init_finish": 1,
                 "itcm": b"\x13\x00"},
    })
    packed = package_input((buff, bwgt), tmp_path / "out", legacy_reduce_mapping=False)
    text = packed.text
    assert ["CORE", "0", "0", "3"] in lines_of(text, "CORE")
    assert all(ln[1:3] != ["0", "0"] for ln in lines_of(text, "CFGMISC"))
    assert all(ln[1:3] != ["0", "0"] for ln in lines_of(text, "KERNELCORE"))
    assert "0 0 8 1" in packed.core_map.read_text().splitlines()


def test_bad_core_stays_spare_and_keeps_only_its_forward(tmp_path):
    path = zero_path()
    path[4] = route(directionMask=0b0100, nxtVC=2)
    buff, bwgt = build(tmp_path, mask=0x084, cores={
        (1, 3): {"release": release(0, 0, 0, 0), "path": path,
                 "task": [(0x10, task(TASK_PATH_ID=4, TASK_END=1))], "init_finish": 1},
        (1, 2): {"release": release(0, 0, 0, 0x4)},
    })
    packed = package_input((buff, bwgt), tmp_path / "out", legacy_reduce_mapping=False)
    assert ["CORE", "1", "2", "3"] in lines_of(packed.text, "CORE")
    assert "1 2 -1 3" in packed.core_map.read_text().splitlines()
    assert any(ln[1:4] == ["1", "2", "4"] for ln in lines_of(packed.text, "RTAB"))
    assert all(ln[1:3] != ["1", "2"] for ln in lines_of(packed.text, "KERNELCORE"))


def test_zero_init_finish_on_an_active_core_is_rejected(tmp_path):
    buff, bwgt = build(tmp_path, cores={
        (0, 1): {"task": [(0x10, task(TASK_END=1))], "init_finish": 0},
    })
    try:
        package_input((buff, bwgt), tmp_path / "out", legacy_reduce_mapping=False)
    except PackageError as exc:
        assert "TS_INIT_FINISH" in str(exc)
    else:
        raise AssertionError("a packed finish of 0 was packaged")


def test_legacy_premerge_marker_is_kept(tmp_path):
    east, north = zero_path(), zero_path()
    east[7] = route(directionMask=0b1000, nxtVC=0)
    north[7] = route(directionMask=0b0001, nxtVC=0)
    forward, target = zero_path(), zero_path()
    forward[7] = route(directionMask=0b1000, nxtVC=0, stallWay=1)
    target[7] = route(directionMask=0b0010, operation=3, nxtVC=0, reduceOutPrecision=0)
    buff, bwgt = build(tmp_path, cores={
        (0, 1): {"path": east},
        (0, 7): {"path": north},
        (0, 2): {"path": forward},
        (0, 3): {"path": target, "task": [(0x10, task(TASK_END=1))], "init_finish": 1},
    })
    with pytest.raises(ConvertError, match="same-face packet multiplicity"):
        package_input((buff, bwgt), tmp_path / "out")
    assert not (tmp_path / "out").exists()
    packed = package_input((buff, bwgt), tmp_path / "out", legacy_reduce_mapping=True)
    assert packed.text.startswith("BACHIR 11\n# LEGACY_REDUCE_MAPPING")
    assert "extra rounding boundary" in packed.text
    forward_row = [
        ln for ln in lines_of(packed.text, "RTAB") if ln[1:4] == ["0", "2", "7"]
    ][0]
    assert [forward_row[4], forward_row[6], forward_row[28]] == ["2", "3", "2"]
    assert packed.legacy_reduces and packed.legacy_reduces[0]["experimental"] is True


def test_same_image_bytes_are_one_file(tmp_path):
    body = b"\x13\x05\x00\x00"
    buff, bwgt = build(tmp_path, cores={
        (0, 1): {"task": [(0x10, task(TASK_END=1))], "init_finish": 1, "itcm": body},
        (0, 2): {"task": [(0x10, task(TASK_END=1))], "init_finish": 1, "itcm": body},
    })
    packed = package_input((buff, bwgt), tmp_path / "out", legacy_reduce_mapping=False)
    names = {ln[4] for ln in lines_of(packed.text, "KERNELCORE")}
    assert len(names) == 1
    assert len(list(packed.out_dir.glob("*.hex"))) == 1
