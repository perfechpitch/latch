#!/usr/bin/env python3
"""Tracetto 的自检。

造几份合成波形，把读波形、分段、标签、九条行、建索引、窗口查询、服务各查一遍，不碰 C++
那一边的构建产物。全过就退出码 0，有一处不对就打印差在哪并退出码非零 —— 与
compiler/selftest_hwconfig.py 收尾方式一致。
"""

import hashlib
import json
import os
import shutil
import struct
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from tracetto import index, idxbuild, server, spans as S   # noqa: E402
from tracetto.reader import TraceReader                    # noqa: E402

FAILS = []


def expect(got, want, what):
    if got != want:
        FAILS.append(f"{what}：得到 {got!r}，期望 {want!r}")


def expect_true(cond, what):
    if not cond:
        FAILS.append(what)


# ── 造波形 ──
# 与 latch 的 .trace 同一个格式，够 Reader 解就行：段在前，然后是 trailer、meta，
# 最后 16 字节是两个偏移。


def _varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def write_trace(path, modules, signals):
    """modules: [(id, parent, name)]；signals: {sig_id: [(t, v), ...]}。

    每个信号写一段。值按差值走 zigzag + varint，与模型写出来的编码一致。
    """
    out = bytearray()
    offs = {}
    for sig_id, events in signals.items():
        events = list(events)
        body = bytearray()
        for i in range(1, len(events)):
            dt = events[i][0] - events[i - 1][0]
            dv = events[i][1] - events[i - 1][1]
            body += _varint(dt) + _varint((dv << 1) ^ (dv >> 63))
        vs = [v for _, v in events]
        offs[sig_id] = len(out)
        out += struct.pack("<4sQIIQQQQQ", b"SMLS", sig_id, len(body), len(events),
                           events[0][0], events[0][1], events[-1][0],
                           min(vs), max(vs))
        out += body
    trailer_off = len(out)
    out += struct.pack("<4sI", b"SMLT", len(signals))
    for sig_id in signals:
        out += struct.pack("<QI", sig_id, 1) + struct.pack("<Q", offs[sig_id])
    meta_off = len(out)
    out += b"SMLM" + struct.pack("<I", 1) + struct.pack("<Q", len(modules))
    for mid, pid, name in modules:
        nb = name.encode()
        out += struct.pack("<QQH", mid, pid, len(nb)) + nb
    out += struct.pack("<QQ", trailer_off, meta_off)
    Path(path).write_bytes(bytes(out))


def lanes_to_signals(evs, lanes=3):
    """[(t, lane, task, user)] → (位掩码, 打包 task, 打包 user) 三条信号。

    与 Core 那边每拍把三路打包、本拍没有事件的那一路填占位（0xFF / 0xFFFF）的写法
    一致。同一拍有多路事件时一起并进同一个值。
    """
    by_t = {}
    for t, lane, task, user in evs:
        by_t.setdefault(t, {})[lane] = (task, user)
    mask, pack_t, pack_u = [], [], []
    for t in sorted(by_t):
        hit = by_t[t]
        m = pt = pu = 0
        for lane in range(lanes):
            task, user = hit.get(lane, (0xFF, 0xFFFF))
            m |= (1 if lane in hit else 0) << lane
            pt |= (task & 0xFF) << (8 * lane)
            pu |= (user & 0xFFFF) << (16 * lane)
        mask.append((t, m))
        pack_t.append((t, pt))
        pack_u.append((t, pu))
    return mask, pack_t, pack_u


# 一份合成波形：chip0.core0 上两笔下发（DTE 与 MU 各一笔），各带一对 RV 边沿与一对
# DSA 边沿；外加一路 VU，只有 RV 边沿、身份是占位 —— 查「认不出配对」那一支。
# DSA 那一行的段由起止两条边沿折出来，与真波形上「一个单元压着好几笔」的折法一致。
RV_EV = [(11, 0, 3, 77), (41, 1, 5, 77), (60, 2, 0xFF, 0xFFFF)]
RV_DONE_EV = [(20, 0, 3, 77), (50, 1, 5, 77), (70, 2, 0xFF, 0xFFFF)]
DSA_EV = [(14, 0, 3, 77), (44, 1, 5, 77)]
DSA_DONE_EV = [(30, 0, 3, 77), (70, 1, 5, 77)]

MODULES = [
    (1, 0, "chip0"),
    (2, 1, "core0"),
    (100, 2, "ts_unit"), (101, 2, "ts_task"), (102, 2, "ts_user"),
    (103, 2, "ts_done"), (104, 2, "ts_inflight"),
    (105, 2, "rv_start"), (106, 2, "rv_start_task"), (107, 2, "rv_start_user"),
    (108, 2, "rv_done"), (109, 2, "rv_done_task"), (110, 2, "rv_done_user"),
    (111, 2, "dsa_start"), (112, 2, "dsa_start_task"), (113, 2, "dsa_start_user"),
    (114, 2, "dsa_done"), (115, 2, "dsa_done_task"), (116, 2, "dsa_done_user"),
]
SIGNALS = {
    100: [(10, 1), (40, 2)],                      # DTE 一发，MU 一发
    101: [(10, 3), (40, 0x500)],                  # DTE task 3，MU task 5
    102: [(10, 77), (40, 77 << 16)],              # 都是用户 77
    103: [(35, 1), (80, 2)],                      # 完成 1 → 2
    104: [(10, 1), (35, 0), (40, 1), (80, 0)],    # ts_inflight
}
for _base, _ev, _done in ((105, RV_EV, RV_DONE_EV), (111, DSA_EV, DSA_DONE_EV)):
    _m, _t, _u = lanes_to_signals(_ev)
    _dm, _dt, _du = lanes_to_signals(_done)
    SIGNALS[_base], SIGNALS[_base + 1], SIGNALS[_base + 2] = _m, _t, _u
    SIGNALS[_base + 3], SIGNALS[_base + 4], SIGNALS[_base + 5] = _dm, _dt, _du


def with_trace(fn):
    """把合成波形写到临时文件，把前缀交给 fn。"""
    with tempfile.TemporaryDirectory() as d:
        prefix = str(Path(d) / "synth")
        write_trace(prefix + ".trace", MODULES, SIGNALS)
        return fn(prefix)


def synth_scaled(path, cores, rounds):
    """放大版：cores 个 core，每条行上 rounds 个段（段长 2、间隔 6）。"""
    modules, signals = [], {}
    mid = 1
    for c in range(cores):
        chip_id, core_id = mid, mid + 1
        mid += 2
        modules.append((chip_id, 0, f"chip{c}"))
        modules.append((core_id, chip_id, "core0"))
        sig = {}
        for name in ("ts_unit", "ts_task", "ts_user", "ts_done", "ts_inflight",
                     "rv_start", "rv_start_task", "rv_start_user",
                     "rv_done", "rv_done_task", "rv_done_user",
                     "dsa_start", "dsa_start_task", "dsa_start_user",
                     "dsa_done", "dsa_done_task", "dsa_done_user"):
            sig[name] = mid
            modules.append((mid, core_id, name))
            mid += 1
        unit, task, user, done, fly = [], [], [], [], []
        rv_ev, rv_done_ev, dsa_ev, dsa_done_ev = [], [], [], []
        t = 10
        for i in range(rounds):
            for u in range(3):
                unit += [(t, 1 << u), (t + 1, 0)]
                task.append((t, (i % 7) << (8 * u)))
                user.append((t, (i % 5 + 1) << (16 * u)))
                rv_ev.append((t, u, i % 7, i % 5 + 1))
                rv_done_ev.append((t + 5, u, i % 7, i % 5 + 1))
                dsa_ev.append((t + 2, u, i % 7, i % 5 + 1))
                dsa_done_ev.append((t + 6, u, i % 7, i % 5 + 1))
                t += 8
            done.append((t, i + 1))
            fly += [(t, 1), (t + 1, 0)]
            t += 2
        signals[sig["ts_unit"]] = unit
        signals[sig["ts_task"]] = task
        signals[sig["ts_user"]] = user
        signals[sig["ts_done"]] = done
        signals[sig["ts_inflight"]] = fly
        for base, evs in (("rv_start", rv_ev), ("rv_done", rv_done_ev),
                          ("dsa_start", dsa_ev), ("dsa_done", dsa_done_ev)):
            m, pt, pu = lanes_to_signals(evs)
            signals[sig[base]] = m
            signals[sig[base + "_task"]] = pt
            signals[sig[base + "_user"]] = pu
    write_trace(path, modules, signals)


# ── 读波形与分段（参考实现）─────────────────────────────────────────────

def check_tree(prefix):
    r = TraceReader(prefix)
    try:
        paths = r.tree_paths()
        expect(paths[100], "chip0.core0.ts_unit", "层次名")
        expect(r.signals(), list(range(100, 117)), "信号号列表")
        ts, vs = r.events(100)
        expect(list(zip(ts, vs)), [(10, 1), (40, 2)], "ts_unit 的事件")
        expect(len(r.segments(100)), 1, "一个信号的段数")
    finally:
        r.close()


def check_bits(prefix):
    """两对边沿折成段：一起一完是一段，压着好几笔就并成一段。"""
    r = TraceReader(prefix)
    try:
        rst, rsv = r.events(105)     # rv_start
        rtt, rtv = r.events(106)     # rv_start_task
        rut, ruv = r.events(107)     # rv_start_user
        rdt, rdv = r.events(108)     # rv_done
        dtt2, dtv2 = r.events(109)   # rv_done_task
        dut, duv = r.events(110)     # rv_done_user
    finally:
        r.close()
    starts = S.issue_events(rst, rsv, rtt, rtv, rut, ruv, 0)
    dones = S.issue_events(rdt, rdv, dtt2, dtv2, dut, duv, 0)
    expect(starts, [{"t": 11, "task": 3, "user": 77}], "DTE 的 RV 起手")
    expect(dones, [{"t": 20, "task": 3, "user": 77}], "DTE 的 RV 交还")
    expect(S.edge_spans(starts, dones, 80), [[11, 20, 77, 3]], "DTE 的 RV 段")

    # 一个单元压着好几笔：MU 两笔首尾相接，折成一段，身份取最早那笔的。
    many_starts = [{"t": 10, "task": 3, "user": 77},
                   {"t": 14, "task": 5, "user": 88}]
    many_dones = [{"t": 30, "task": 3, "user": 77},
                  {"t": 40, "task": 5, "user": 88}]
    expect(S.edge_spans(many_starts, many_dones, 80), [[10, 40, 77, 3]],
           "压着两笔 → 一段")
    # 同一拍完成与起手撞上：先算完成，两段首尾相接而不是并成一段。
    expect(S.edge_spans([{"t": 10, "task": 3, "user": 77},
                         {"t": 20, "task": 5, "user": 88}],
                        [{"t": 20, "task": 3, "user": 77},
                         {"t": 40, "task": 5, "user": 88}], 80),
           [[10, 20, 77, 3], [20, 40, 88, 5]], "同拍交接")
    # 起点没配上完成的只记那一拍；末尾还压着的延到波形末。
    expect(S.edge_spans([], [{"t": 7, "task": 1, "user": 9}], 80),
           [[7, 8, 9, 1]], "孤立的完成")
    expect(S.edge_spans([{"t": 60, "task": 1, "user": 9}], [], 80),
           [[60, 80, 9, 1]], "末尾还高着")
    # 占位身份（0xFF / 0xFFFF）一律折成 -1。
    expect(S.edge_spans([{"t": 60, "task": 0xFF, "user": 0xFFFF}],
                        [{"t": 70, "task": 0xFF, "user": 0xFFFF}], 80),
           [[60, 70, -1, -1]], "认不出的身份")


def check_issues(prefix):
    r = TraceReader(prefix)
    try:
        ut, uv = r.events(100)
        tt, tv = r.events(101)
        xt, xv = r.events(102)
    finally:
        r.close()
    expect(S.issue_events(ut, uv, tt, tv, xt, xv, 0),
           [{"t": 10, "task": 3, "user": 77}], "DTE 的下发")
    expect(S.issue_events(ut, uv, tt, tv, xt, xv, 1),
           [{"t": 40, "task": 5, "user": 77}], "MU 的下发")
    expect(S.issue_events(ut, uv, tt, tv, xt, xv, 2), [], "VU 的下发")


def check_labels(prefix):
    """DSA 那两行的标签直接来自起手那条边沿，不用再贴下发事件。"""
    r = TraceReader(prefix)
    try:
        st, sv = r.events(111)       # dsa_start
        stt, stv = r.events(112)     # dsa_start_task
        sut, suv = r.events(113)     # dsa_start_user
        dt, dv = r.events(114)       # dsa_done
        dtt, dtv = r.events(115)     # dsa_done_task
        dut, duv = r.events(116)     # dsa_done_user
    finally:
        r.close()
    dsa = S.edge_spans(S.issue_events(st, sv, stt, stv, sut, suv, 0),
                       S.issue_events(dt, dv, dtt, dtv, dut, duv, 0), 80)
    expect(dsa, [[14, 30, 77, 3]], "DTE 的 DSA 段带身份")
    expect(S.edge_spans([], [], 80), [], "没有边沿就没有段")


def check_chain():
    ev0 = [{"t": 10, "task": 3, "user": 77}]
    ev1 = [{"t": 40, "task": 5, "user": 77}]
    expect(S.pair_chain([(14, 30)], ev0), [[10, 30, 77, 3]], "DTE 的全程")
    expect(S.pair_chain([(44, 70)], ev1), [[40, 70, 77, 5]], "MU 的全程")
    expect(S.pair_chain([(44, 70)], [{"t": 10, "task": 3, "user": 77}]),
           [[10, 70, 77, 3]], "下发早于段")
    expect(S.pair_chain([(60, 70)], []), [[60, 70, -1, -1]], "没被认领的段")
    # 一笔 task 的 DSA 碎成几段：后面那几段一并吃进同一笔，段末取最后一段的末。
    expect(S.pair_chain([(521, 1334), (1405, 2218)],
                        [{"t": 458, "task": 1, "user": 77}]),
           [[458, 2218, 77, 1]], "碎成两段的同一笔")
    expect(S.pair_chain([(2671, 2972), (3045, 3346), (3419, 3720)],
                        [{"t": 2606, "task": 3, "user": 77}]),
           [[2606, 3720, 77, 3]], "碎成三段的同一笔")
    expect(S.pair_chain([(521, 1334), (1405, 2218), (2671, 2972)],
                        [{"t": 458, "task": 1, "user": 77},
                         {"t": 2606, "task": 3, "user": 77}]),
           [[458, 2218, 77, 1], [2606, 2972, 77, 3]], "两笔各吃各的碎段")
    expect(S.pair_chain([(0, 1), (14, 30)], ev0),
           [[0, 1, -1, -1], [10, 30, 77, 3]], "下发之前的孤段")
    expect(S.pair_chain([(44, 70)], [{"t": 10, "task": 3, "user": 77},
                                     {"t": 40, "task": 5, "user": 77}]),
           [[40, 70, 77, 5]], "不越过下一笔去认段")


def check_done(prefix):
    r = TraceReader(prefix)
    try:
        ts, vs = r.events(103)
    finally:
        r.close()
    expect(S.delta_ticks(ts, vs), [35, 80], "完成的增量")


def check_rows(prefix):
    """九条行：段与标签。"""
    plan = idxbuild.plan(Path(prefix + ".trace"))
    with TraceReader(prefix) as r:
        nodes = S.core_spans(r, plan["core_sig"]["0.0"], plan["t_end"])
    rows = S.lanes_of(nodes)
    expect(plan["t_end"], 80, "波形末尾（只按要读的信号算）")
    expect(rows[0], [[10, 30, 77, 3]], "TS · DTE")
    expect(rows[1], [[40, 70, 77, 5]], "TS · MU")
    expect(rows[2], [], "TS · VU")
    expect(rows[3], [[11, 20, 77, 3]], "DTE_Core")
    expect(rows[4], [[60, 70, -1, -1]], "VU_Core（身份是占位）")
    expect(rows[5], [[41, 50, 77, 5]], "MU_Core")
    expect(rows[6], [[14, 30, 77, 3]], "DTE_DSA")
    expect(rows[7], [], "VU_DSA")
    expect(rows[8], [[44, 70, 77, 5]], "MU_DSA")
    expect(S.LANES[5][0], "MU_Core", "第 5 条通道是 MU_Core")
    expect(S.UNITS[S.LANES[5][2]], "MU", "第 5 条通道的单元")


# ── 索引 ──────────────────────────────────────────────────────────────

def _rows_for(plan, key):
    with TraceReader(plan["prefix"]) as r:
        return S.lanes_of(S.core_spans(r, plan["core_sig"][key], plan["t_end"]))


def check_index_roundtrip(prefix):
    """建索引 → 逐行读回来，与参考实现逐段对拍。"""
    trace = Path(prefix + ".trace")
    manifest, _ = idxbuild.ensure(trace, workers=1, quiet=True)
    d = index.index_dir(prefix)
    plan = idxbuild.plan(trace)
    expect(manifest["t_end"], plan["t_end"], "manifest 里的 t_end")
    expect(len(manifest["lane_n"]), 9, "一条 core 九条行")
    expect_true(manifest["core_size"][0] > 0, "core0 的索引文件大小记下来了")

    bad = 0
    lanes_checked = 0
    for ci, (_chip, key) in enumerate(plan["ordered"]):
        want = _rows_for(plan, key)
        cf = index.CoreFile(d / index.CORES_DIR / f"{ci:05d}.bin")
        for row in range(9):
            w = cf.window(row, 0, plan["t_end"], px=1)
            got = [] if w is None else index.decode_segments(
                cf.read(w["off"], w["end"]), w["t_base"], w["n"])
            lanes_checked += 1
            if got != [list(x) for x in want[row]]:
                bad += 1
                if bad <= 2:
                    FAILS.append(f"索引往返 {key} row{row}：{got[:3]} vs {want[row][:3]}")
        cf.close()
    expect(lanes_checked, 9, "查过的行数")
    expect(bad, 0, "索引往返应当逐段一致")


def check_window():
    """窗口查询：exact（含跨界段与 t_base）、coarse（格数上界与守恒）、边界形状。"""
    work = Path(tempfile.mkdtemp(prefix="tracetto_win_"))
    index.SEG_BLK = 8
    index.L1_MIN_SEG = 16
    try:
        prefix = str(work / "synth")
        synth_scaled(prefix + ".trace", cores=1, rounds=400)
        trace = Path(prefix + ".trace")
        idxbuild.ensure(trace, workers=1, quiet=True)
        plan = idxbuild.plan(trace)
        want = [list(x) for x in _rows_for(plan, plan["ordered"][0][1])[0]]
        t_end = plan["t_end"]
        cf = index.CoreFile(index.index_dir(prefix) / index.CORES_DIR / "00000.bin")
        saw_coarse = False
        for (t0, t1, px) in ((0, t_end, 4), (100, 900, 4), (500, 520, 8),
                             (t_end - 5, t_end + 20, 8)):
            w = cf.window(0, t0, t1, px)
            if w is None:
                continue
            if w["mode"] == "exact":
                got = index.decode_segments(cf.read(w["off"], w["end"]), w["t_base"], w["n"])
                got = [g for g in got if g[1] > t0 and g[0] < t1]
                expect(got, [s for s in want if s[1] > t0 and s[0] < t1],
                       f"exact 窗口 [{t0},{t1})")
            else:
                saw_coarse = True
                cells = index.decode_l1(cf.read(w["off"], w["end"]), w["cell"], w["c0"])
                expect_true(len(cells) <= index.L1_MAX_CELL, "粗层格数有上界")
                busy = sum(c[2] / 255 * w["cell"] for c in cells)
                exact = sum(s[1] - s[0] for s in want if s[1] > 0)
                expect_true(abs(busy - exact) <= len(cells) * 2 + 4,
                            f"粗层忙拍大致守恒：约 {busy:.0f} vs {exact}")
        expect_true(saw_coarse, "缩到很小时应当走 coarse")
        expect(cf.window(0, t_end, t_end + 100, 8), None, "整段在窗口外 → None")
        expect(cf.window(9, 0, t_end, 8), None, "越界的行号 → None")
        expect(cf.window(-1, 0, t_end, 8), None, "负的行号 → None")
        cf.close()
    finally:
        index.SEG_BLK = 128
        index.L1_MIN_SEG = 4096
        shutil.rmtree(work, ignore_errors=True)


def check_reuse(prefix):
    """复用与失效：没变不重建，变了/坏了才重建。"""
    trace = Path(prefix + ".trace")
    d = index.index_dir(prefix)
    _, rebuilt = idxbuild.ensure(trace, workers=1, quiet=True)
    expect_true(rebuilt, "第一次要建")
    _, rebuilt = idxbuild.ensure(trace, workers=1, quiet=True)
    expect_true(not rebuilt, "第二次直接复用")

    time.sleep(0.01)
    os.utime(trace, None)                     # 内容没变、mtime 变了
    _, rebuilt = idxbuild.ensure(trace, workers=1, quiet=True)
    expect_true(rebuilt, "波形 mtime 变了要重建")

    # 同大小同 mtime、内容变了 → head_fp 必须发现。
    # 只翻最低位：varint 的续位不动，格式仍然合法，但解出来的时间确实变了。
    st = trace.stat()
    raw = bytearray(trace.read_bytes())
    raw[56] ^= 0x01                      # 第一个段的第一个字节（段体开头）
    trace.write_bytes(bytes(raw))
    os.utime(trace, ns=(st.st_atime_ns, st.st_mtime_ns))
    expect_true(not index.identity_matches(trace, index.read_manifest(d)["source"]),
                "同大小同 mtime、内容变了要能发现")
    m, rebuilt = idxbuild.ensure(trace, workers=1, quiet=True)
    expect_true(rebuilt, "内容变了要重建")

    m["format"] = 999                          # 格式版本不对
    index.write_manifest(d, m)
    m2, rebuilt = idxbuild.ensure(trace, workers=1, quiet=True)
    expect_true(rebuilt and m2["format"] == index.INDEX_FORMAT, "格式版本不对要重建")

    core0 = d / index.CORES_DIR / "00000.bin"
    good = core0.read_bytes()
    core0.write_bytes(good[:len(good) // 2])   # 索引文件被截断
    _, rebuilt = idxbuild.ensure(trace, workers=1, quiet=True)
    expect_true(rebuilt, "索引文件被截断要重建")
    core0.unlink()                             # 索引文件被删
    _, rebuilt = idxbuild.ensure(trace, workers=1, quiet=True)
    expect_true(rebuilt and core0.exists(), "索引文件被删要重建")

    dead = d.parent / f"{idxbuild.BUILDING_PREFIX}999999.dead"
    dead.mkdir(exist_ok=True)
    idxbuild.ensure(trace, workers=1, quiet=True)
    expect_true(not dead.exists(), "没人要的 .building.* 会被清掉")


def check_parallel_determinism(prefix):
    """并行建索引的结果必须与进程数无关。"""
    trace = Path(prefix + ".trace")

    def digest():
        d = index.index_dir(prefix)
        h = hashlib.sha1()
        for f in sorted((d / index.CORES_DIR).iterdir()):
            h.update(f.name.encode())
            h.update(f.read_bytes())
        h.update(json.dumps(index.read_manifest(d)["lane_n"]).encode())
        return h.hexdigest()

    shutil.rmtree(index.index_dir(prefix), ignore_errors=True)
    idxbuild.ensure(trace, workers=1, quiet=True)
    one = digest()
    shutil.rmtree(index.index_dir(prefix), ignore_errors=True)
    idxbuild.ensure(trace, workers=4, quiet=True)
    four = digest()
    expect(one, four, "1 个进程与 4 个进程建出来的索引应当逐字节一致")


def check_scale(workdir):
    """规模检查：索引每段字节有上界，init 与窗口传输不随段数涨。

    时间只打印不断言（机器忙闲差太多，断言会变成 flaky）。
    """
    old_blk, old_l1 = index.SEG_BLK, index.L1_MIN_SEG
    index.SEG_BLK, index.L1_MIN_SEG = 16, 64
    sizes = {}
    for k in (1, 8):
        prefix = str(Path(workdir) / f"scale{k}")
        trace = Path(prefix + ".trace")
        synth_scaled(str(trace), cores=2, rounds=40 * k)
        t0 = time.perf_counter()
        manifest, _ = idxbuild.ensure(trace, workers=1, quiet=True)
        dt = time.perf_counter() - t0
        d = index.index_dir(prefix)
        nseg = sum(manifest["lane_n"])
        ivec = sum(f.stat().st_size for f in (d / index.CORES_DIR).iterdir())
        init = len(json.dumps({"chips": manifest["chips"], "lane_n": manifest["lane_n"]},
                              separators=(",", ":")).encode())
        # 缩到最小（整条波形铺在 200 px 上）取一次：段比像素多得多时走粗层，
        # 传输永远不超过「这一窗的精确段」那条线。
        cf = index.CoreFile(d / index.CORES_DIR / "00000.bin")
        w = cf.window(0, 0, manifest["t_end"], 8)      # 一屏只放 8 像素
        nbytes = 0 if w is None else w["end"] - w["off"]
        mode = None if w is None else w["mode"]
        cf.close()
        sizes[k] = (nseg, ivec, init, nbytes, mode, manifest["lane_n"][0])
        print(f"    K={k}: 段 {nseg:>6}  索引 {ivec / 1024:7.1f}KB"
              f"（{ivec / max(1, nseg):5.1f} B/段）  init {init / 1024:5.1f}KB"
              f"  窗口 {nbytes / 1024:5.1f}KB（{mode}）  建索引 {dt:5.2f}s")
        shutil.rmtree(d, ignore_errors=True)

    index.SEG_BLK, index.L1_MIN_SEG = old_blk, old_l1
    per_seg = sizes[8][1] / max(1, sizes[8][0])
    expect_true(per_seg < 24, f"每段索引字节 {per_seg:.1f} 应当有上界")
    expect_true(sizes[8][2] < sizes[1][2] * 1.5, "init 体积不该随段数涨")
    for k in (1, 8):
        # 传输 ≤ 「这一窗的精确段」的上界（粗层只会更小）
        cap = max(2 * 8, sizes[k][5]) * 8 + 64
        expect_true(sizes[k][3] <= cap,
                    f"K={k} 的窗口传输 {sizes[k][3]} 超过上界 {cap}")
    expect_true(sizes[8][4] == "coarse", f"K=8 段比像素多得多时应当走粗层，得到 {sizes[8][4]}")
    expect_true(sizes[1][4] == "exact", f"K=1 整条行比一屏还小，直接给精确段，得到 {sizes[1][4]}")


# ── 服务 ──────────────────────────────────────────────────────────────

def check_http(prefix):
    trace = Path(prefix + ".trace")
    manifest, _ = idxbuild.ensure(trace, workers=1, quiet=True)
    srv = server.TraceServer(("127.0.0.1", 0), server.Handler, trace, manifest, 1)
    port = srv.server_address[1]
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    base = f"http://127.0.0.1:{port}"

    def get(path):
        return urllib.request.urlopen(base + path, timeout=10)

    try:
        init = json.loads(get("/api/init").read())
        expect(init["format"], index.INDEX_FORMAT, "/api/init 的格式版本")
        expect(init["build"], index.build_id(manifest["source"]), "/api/init 的 build")
        expect(init["t_end"], 80, "/api/init 的 t_end")
        expect(init["rows"], ["TS", "DTE-Core", "DTE-DSA", "MU-Core", "MU-DSA",
                              "VU-Core", "VU-DSA"], "/api/init 的七行与顺序")
        expect(init["chips"][0][1][0][1], 1, "core0 派了角色")
        expect(len(init["lane_n"]), 9, "/api/init 的行段数")
        expect(init["lanes_per_core"], 9, "/api/init 的通道步长")
        # TS 那一行由三条通道叠出来，颜色 0/1/2 分给 DTE / MU / VU；九种颜色各有其主。
        expect(init["row_parts"][0], [{"lane": 0, "color": 0, "unit": "TS-DTE"},
                                      {"lane": 1, "color": 1, "unit": "TS-MU"},
                                      {"lane": 2, "color": 2, "unit": "TS-VU"}],
               "/api/init 里 TS 那一行的三条通道")
        used = [p["color"] for row in init["row_parts"] for p in row]
        expect(sorted(used), list(range(9)), "/api/init 的九种颜色各用一次")

        with get("/") as r:
            page = r.read()
            expect(r.status, 200, "/ 的状态码")
            expect(r.headers.get("Cache-Control"), "no-cache", "/ 的缓存头")
            expect_true(b"Tracetto" in page, "页面标题里没有 Tracetto")
            expect_true(b"/static/app.js" in page, "页面里没有引用前端脚本")
            expect_true(b"insight" not in page.lower(), "页面里不该再出现 insight")
        with urllib.request.urlopen(
                urllib.request.Request(base + "/static/app.js", method="HEAD")) as r:
            expect(r.status, 200, "HEAD /static/app.js")
            expect_true("javascript" in (r.headers.get("Content-Type") or ""),
                        "/static/app.js 的类型")

        cf = index.CoreFile(index.index_dir(prefix) / index.CORES_DIR / "00000.bin")
        w0 = cf.window(0, 0, 80, 1200)
        want_bytes = cf.read(w0["off"], w0["end"])
        cf.close()
        with get("/api/window?lanes=0&t0=0&t1=80&px=1200") as r:
            raw = r.read()
            expect(r.status, 200, "/api/window 的状态码")
            expect(raw[:4], b"TCW1", "帧头的魔数")
            n = struct.unpack_from("<I", raw, 4)[0]
            head = json.loads(raw[8:8 + n])
            e = head["lanes"][0]
            expect(e["lane"], 0, "回来的行号")
            expect(raw[8 + n + e["off"]:8 + n + e["off"] + e["len"]], want_bytes,
                   "/api/window 的切片")
            expect((head["t0"], head["t1"]), (0, 80), "帧头里的窗口")
        with get("/api/window?lanes=0-8&t0=0&t1=80&px=1200") as r:
            raw = r.read()
            n = struct.unpack_from("<I", raw, 4)[0]
            head = json.loads(raw[8:8 + n])
            expect(sorted(e["lane"] for e in head["lanes"]), [0, 1, 3, 4, 5, 6, 8],
                   "有段的行才回，空的跳过")

        st = json.loads(get("/api/status").read())
        expect((st["building"], st["progress"]), (False, 1.0), "/api/status")
        with urllib.request.urlopen(urllib.request.Request(
                base + "/api/reload", method="POST", data=b"")) as r:
            j = json.loads(r.read())
            expect((j["ok"], j["rebuilt"]), (True, False), "波形没变时 reload 不重建")

        for path, code in (("/api/window?lanes=0&t0=9&t1=5", 400),
                           ("/api/window?lanes=x&t0=0&t1=5", 400),
                           ("/api/window?lanes=0-999&t0=0&t1=5", 400),
                           ("/api/nope", 404),
                           ("/api/window?lanes=0&t0=0&t1=80&build=deadbeef", 409)):
            try:
                get(path)
                FAILS.append(f"{path} 应当回 {code}")
            except urllib.error.HTTPError as e:
                expect(e.code, code, f"{path} 的状态码")
    finally:
        srv.shutdown()
        srv.server_close()
        th.join(timeout=2)


def check_no_insight():
    """不该再依赖 insight 的任何东西。"""
    for gone in ("WIRE_FORMAT", "BUCKET_FMT", "samples_multi_bytes", "init_json",
                 "select_stats_json", "build_html", "web_dir_ok"):
        expect_true(not hasattr(server, gone), f"server 里还留着 {gone}")
    text = (HERE / "server.py").read_text(encoding="utf-8")
    for bad in ("utils/insight", "import insight", "BUCKET_FMT", "samples_multi"):
        expect_true(bad not in text, f"server.py 里不该再有 {bad}")
    expect(server.WEB_DIR, HERE / "web", "伺服的应当是 tracetto 自己的 web/")
    for name in ("index.html", "style.css", "app.js"):
        expect_true((HERE / "web" / name).is_file(), f"web/{name} 在")


def check_cmake_path():
    """ctest 那条得指着小写的 tracetto 这一份。"""
    cmake = HERE.parents[2] / "test" / "CMakeLists.txt"     # src/utils/tracetto → 仓库根
    if not cmake.is_file():
        return
    text = cmake.read_text(encoding="utf-8")
    expect_true("Tracetto/" not in text, "CMakeLists 里还写着大写的 Tracetto/")
    expect_true("../src/utils/tracetto/selftest_tracetto.py" in text,
                "CMakeLists 没指着 selftest")


def main():
    with_trace(check_tree)
    with_trace(check_bits)
    with_trace(check_issues)
    with_trace(check_done)
    with_trace(check_rows)
    with_trace(check_index_roundtrip)
    with_trace(check_reuse)
    with_trace(check_parallel_determinism)
    with_trace(check_http)
    with_trace(check_labels)
    check_chain()
    check_window()
    with tempfile.TemporaryDirectory() as d:
        check_scale(d)
    check_no_insight()
    check_cmake_path()
    if FAILS:
        print(f"Tracetto 自检：{len(FAILS)} 处不对")
        for f in FAILS:
            print("  - " + f)
        return 1
    print("Tracetto 自检通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
