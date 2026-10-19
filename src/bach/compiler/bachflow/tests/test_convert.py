"""The converter against hand-built packs."""

from __future__ import annotations

import pytest

from bach.compiler.bachflow import ConvertError, convert
from pack import build, datain, release, route, task, zero_path


def lines_of(text, tag):
    return [ln.split() for ln in text.splitlines() if ln.startswith(tag + " ")]


def test_one_task_core_renders_the_chain(tmp_path):
    path = zero_path()
    path[3] = route(directionMask=0b1_0000 | 0b1000, nxtVC=1, streamCheck=1)
    buff, bwgt = build(tmp_path, cores={(0, 1): {
            "release": release(0, 0, 0),
            "path": path,
            "task": [(0x20, task(TASK_SEND_UNIT=2, TASK_PATH_ID=3, TASK_END=1))],
            "stream_num": 4,
            "self_start": 0,
            "init_finish": 1,
            "itcm": b"VUIMG",
            "smem": b"\x01\x00\x00\x00",
            "dsa": [(0x10, [7, 8])],
        },
    })
    bundle = convert(buff, bwgt)
    chain = lines_of(bundle.text, "TCHAIN")
    assert chain == [["TCHAIN", "0", "1", "0", "0x20", "2", "0", "0", "0", "0", "0", "3", "1"]]
    assert lines_of(bundle.text, "CFGMISC") == [["CFGMISC", "0", "1", "4", "0", "0x0", "1"]]
    assert lines_of(bundle.text, "DTEIN") == [["DTEIN", "0", "1", "0", "0", "0", "0"]]
    assert lines_of(bundle.text, "INITFINISH") == [["INITFINISH", "0", "1", "1"]]
    assert lines_of(bundle.text, "ABI") == [["ABI", "0", "1", "bachflow"]]
    assert [ln[3] for ln in lines_of(bundle.text, "FILESPAN")] == ["itcm.vu", "smem"]
    assert lines_of(bundle.text, "DSAREG") == [["DSAREG", "0", "1", "0", "16", "7"],
                                               ["DSAREG", "0", "1", "0", "20", "8"]]
    assert lines_of(bundle.text, "RTABDTE") == [["RTABDTE", "0", "1", "3"]]
    assert lines_of(bundle.text, "PATHTASK") == []


def test_bad_core_gets_no_span_and_a_derived_forward(tmp_path):
    path = zero_path()
    path[4] = route(directionMask=0b0100, nxtVC=2)
    buff, bwgt = build(tmp_path, mask=0x084, cores={
        (1, 3): {"release": release(0, 0, 0), "path": path,
            "task": [(0x10, task(TASK_PATH_ID=4, TASK_END=1))]},
        (1, 2): {"release": release(0, 0, 0x8)},
    })
    bundle = convert(buff, bwgt)
    assert all(ln[1:3] != ["1", "2"] for ln in lines_of(bundle.text, "FILESPAN"))
    assert all(ln[1:3] != ["1", "2"] for ln in lines_of(bundle.text, "RTABDTE"))
    assert ["COREMAP", "1", "2", "-1"] in lines_of(bundle.text, "COREMAP")
    assert ["CORE", "1", "2", "3"] in lines_of(bundle.text, "CORE")
    derived = [ln for ln in lines_of(bundle.text, "RTAB") if ln[1:3] == ["1", "2"]]
    assert derived and derived[0][3] == "4"


def test_stream_init_other_than_16_fails(tmp_path):
    buff, bwgt = build(tmp_path, cores={
        (0, 0): {"stream_init": 8, "task": [(0x10, task(TASK_END=1))]},
    })
    with pytest.raises(ConvertError, match="stream_init"):
        convert(buff, bwgt)


def test_smem_past_32k_fails(tmp_path):
    buff, bwgt = build(tmp_path, cores={
        (0, 0): {"smem": b"\x01\x00\x00\x00", "smem_off": 32 * 1024,
            "task": [(0x10, task(TASK_END=1))]},
    })
    with pytest.raises(ConvertError, match="ShareMem"):
        convert(buff, bwgt)


def test_same_face_reduce_fails(tmp_path, monkeypatch):
    # The 2x5 port map gives every face its own station, so a packed word cannot
    # put two inputs on one face by itself. The collision is two hops arriving
    # on one station; here that arrival list is supplied directly.
    path = zero_path()
    path[1] = route(directionMask=0b1000, operation=3, nxtVC=3)
    buff, bwgt = build(tmp_path, cores={
        (0, 1): {"path": path, "task": [(0x10, task(TASK_END=1))]},
    })
    import importlib
    monkeypatch.setattr(
        importlib.import_module("bach.compiler.bachflow.convert"), "_arrivals",
        lambda *args: {(0, 1, 1): [(0, 0, (0, 0)), (0, 1, (0, 2))]},
    )
    with pytest.raises(ConvertError, match="one face"):
        convert(buff, bwgt)


def test_reduce2_with_one_incoming_lane_is_rejected(tmp_path):
    upstream, downstream = zero_path(), zero_path()
    upstream[7] = route(directionMask=0b1000, nxtVC=0)
    downstream[7] = route(directionMask=0b0010, operation=3, nxtVC=0)
    buff, bwgt = build(tmp_path, cores={
        (0, 2): {"path": upstream},
        (0, 3): {"path": downstream},
    })
    with pytest.raises(ConvertError, match="reduce2 requires 2 external packets"):
        convert(buff, bwgt)


def test_missing_ingress_is_reported_not_invented(tmp_path):
    buff, bwgt = build(tmp_path, cores={
        (3, 4): {"self_start": 1, "datain": (0x30, datain()),
            "task": [(0x40, task(TASK_END=1))]},
    })
    bundle = convert(buff, bwgt)
    assert bundle.inventory.missing
    assert not any(ln[3] == "63" for ln in lines_of(bundle.text, "RTAB"))


def test_r_core_wait_wake_lead_is_not_rewritten(tmp_path):
    buff, bwgt = build(tmp_path, cores={
        (3, 4): {
            "task": [
                (0x30, task(WAIT_WAKE=1, TASK_END=0)),
                (0x40, task(TASK_END=1)),
            ],
        },
    })
    with pytest.raises(ConvertError, match="not converted"):
        convert(buff, bwgt)


def test_pathtask_only_for_an_inbound_task(tmp_path):
    buff, bwgt = build(tmp_path, cores={
        (0, 1): {"task": [
            (0x20, task(TASK_PATH_ID=3, TASK_END=0)),
            (0x30, task(TASK_TYPE=3, WAIT_WAKE=1, TASK_P2P_REISSUE_TID=0,
                        TASK_PATH_ID=5, TASK_END=1)),
        ]},
    })
    bundle = convert(buff, bwgt)
    assert lines_of(bundle.text, "PATHTASK") == [["PATHTASK", "0", "1", "5", "1"]]


def test_recv_unit_3_and_a_hole_fail(tmp_path):
    buff, bwgt = build(tmp_path, cores={
        (0, 1): {"task": [(0x10, task(TASK_RECV_UNIT=3, TASK_END=1))]},
    })
    with pytest.raises(ConvertError, match="recv unit"):
        convert(buff, bwgt)
    hole = [None, None, (0x30, task(TASK_END=1))]
    buff, bwgt = build(tmp_path, cores={(0, 1): {"task": [(0x10, task()), *hole]}})
    with pytest.raises(ConvertError, match="not contiguous"):
        convert(buff, bwgt)


def test_unused_precision_bit_is_ignored(tmp_path):
    # Packed reduceInPrecision 16 is CORE=FP32. The N/S/W/E bits are unused and
    # must not make the word a mixed precision.
    path = zero_path()
    path[5] = route(directionMask=0b1_0000 | 0b1000, operation=1, reduceInPrecision=0x10)
    buff, bwgt = build(tmp_path, cores={
        (0, 1): {"path": path, "task": [(0x10, task(TASK_END=1))]},
    })
    bundle = convert(buff, bwgt)
    rtab = lines_of(bundle.text, "RTAB")
    assert rtab and rtab[0][25] == "1"


def test_inactive_broadcast_core_has_no_cfg(tmp_path):
    buff, bwgt = build(tmp_path, cores={(0, 1): {"task": [(0x10, task(TASK_END=1))]}})
    bundle = convert(buff, bwgt)
    assert all(ln[1:3] != ["0", "0"] for ln in lines_of(bundle.text, "CFGMISC"))
    assert all(ln[1:3] != ["0", "0"] for ln in lines_of(bundle.text, "DTEIN"))


def test_zero_init_finish_is_kept(tmp_path):
    buff, bwgt = build(tmp_path, cores={(0, 1): {"task": [(0x10, task(TASK_END=1))], "init_finish": 0},
    })
    bundle = convert(buff, bwgt)
    assert lines_of(bundle.text, "INITFINISH") == [["INITFINISH", "0", "1", "0"]]


def _rtab(text, chip, core, path):
    rows = [
        ln for ln in lines_of(text, "RTAB")
        if ln[1:4] == [str(chip), str(core), str(path)]
    ]
    assert len(rows) == 1
    return rows[0]


def _split_pack(tmp, task_type=4):
    """p2 sends two packets east into p3 under PathIDs 7 and 9.

    They land on one face, so p3's reduce2 has an entry for each PathID. The
    words differ only in input precision: 9 is FP32 on west and CORE. The
    reduce task names 7, the core's own operand.
    """
    upstream, target = zero_path(), zero_path()
    upstream[7] = route(directionMask=0b1000)
    upstream[9] = route(directionMask=0b1000)
    target[7] = route(directionMask=0b0010, operation=3)
    target[9] = route(directionMask=0b0010, operation=3, reduceInPrecision=0b1_0100)
    return build(tmp, cores={
        (0, 2): {"path": upstream},
        (0, 3): {"path": target, "task": [
            (0x10, task(TASK_TYPE=task_type, TASK_PATH_ID=7, TASK_END=1)),
        ]},
    })


def test_split_paths_on_one_face_are_one_net(tmp_path):
    bundle = convert(*_split_pack(tmp_path))
    own, spare = _rtab(bundle.text, 0, 3, 7), _rtab(bundle.text, 0, 3, 9)
    assert own[28] == spare[28] == "3"
    # One lane mask and one flow for the net; the model checks they agree.
    assert own[27] == spare[27] and own[5] == spare[5]
    assert bin(int(own[27])).count("1") == 2
    # Precision stays per entry.
    assert [own[25], spare[25]] == ["0", "1"]


def test_split_paths_without_one_reduce_task_fail(tmp_path):
    with pytest.raises(ConvertError, match="0 reduce tasks name them"):
        convert(*_split_pack(tmp_path, task_type=0))


def test_path_id_past_the_model_table_fails(tmp_path):
    path = zero_path()
    path[64] = route(directionMask=0b1000)
    buff, bwgt = build(tmp_path, cores={(0, 2): {"path": path}})
    with pytest.raises(ConvertError, match="below 64"):
        convert(buff, bwgt)
