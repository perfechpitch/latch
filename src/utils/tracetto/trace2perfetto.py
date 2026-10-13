#!/usr/bin/env python3
"""把 latch 的 .trace 转成 Perfetto（Chrome Trace Event Format）的 JSON。

    python3 src/utils/tracetto/trace2perfetto.py moe_lpu

产物落在波形旁边：`<波形名>.perfetto.json`，直接拖进 ui.perfetto.dev 或
chrome://tracing。

时间区间仍从 `spans.py` 来。Perfetto 这边另排轨道、另写标签：

    一个 core 一个进程，名字是 `trayX.chipY.phyZ(logicN)`。
    TS 的各步收成一行，线程名 `TS`。段名是生命周期，不是 DTE / MU / VU：
    `Wait`（装进 stream 到下发）、`Issue`（下发到 RV core 接下）、
    `TaskDone`（这一笔完成的那一拍）。是哪一路写在 args 的 `unit` 里。
    DTE 的 Queue / Read / Write 收成一条线程 `DTE-DSA`，段名仍是各步骤。
    MU 的 Load / Compute / Store 收成一条线程 `MU-DSA`，段名仍是各步骤。
    VU 的 ISQ / Exec 收成一条线程 `VU-DSA`，段名仍是各步骤。
    （波形里还没有步骤信号时，DTE/MU 的 DSA 退回整段。）
    线程名：DTE-RVCore / MU-RVCore / VU-RVCore，最后是 `Router`。
    段上的字只到 `Y: uid=…, sid=…, tid=…`。Y 是 Wait、Issue、TaskDone、RVCore，
    或这一步的名字。字节数、矩阵维度、专家数、所属单元写在 args 里，
    Perfetto 的 Details 才看得到。
    Credit、Busy、R2R 是计数器（`ph=C`），不是一段时间的段。计数挂在对应线程上：
    TS 的 `Credit` 是还剩几个 stream 槽（16 减去在途）；
    MU-DSA 的 `Busy` 在有 Compute 时为 1，否则为 0。两段 Compute 只隔 1 拍时
    保持 1，这一拍是上一段的结束沿，轨道上两段是连着的；
    Router 的 `R2R mid` / `R2R left` / `R2R right` 是这一拍往该方向发出的字节，
    一条 flit 按链路宽度 256 B 计，没发的下一拍回到 0。
    args 里只放 `value`，轨道名就是计数器自己的名字。

**顺序**：进程按 chip 升序、再按 core 升序。一个进程里先 TS，再 DTE、MU、VU。
这两层各自再发一条排序键（`process_sort_index` / `thread_sort_index`）钉死 ——
**Perfetto 是按名字排轨道的，不按 pid / tid**（实测：不发键时 chip10 跑到 chip2
前面、TS 掉到第五行），而名字的字典序本来就排不对。试过把序号写进名字里（`chip00`
/ `1 TS`）来代替这两组键，也不行，所以键留着（258 KB）。

**颜色**：Perfetto 按段上的名字上色。名字以步骤名开头（`Wait` / `Issue` /
`TaskDone` / `RVCore` / `Read` / `Compute` …），同一种步骤同色。不给 cname，
也不给 cat。

时间：**1 拍 = 1 ns**。JSON 的 `ts` / `dur` 单位固定是微秒，Perfetto 读入后再
乘 1000 存成纳秒，所以这里写成 `(拍 + 0.5) / 1000` 微秒，取整后正好是拍数个纳秒。
满 1000 拍时界面仍会进位成 µs，那是 1 µs = 1000 ns。

精简：没有额外字段的段不写 args，不发 cat，
分隔符用最紧的写法。一段一个 `ph=X`，挂在对应线程上。不用异步 begin/end：
Perfetto 把 `ph=b/e` 挂到进程级轨道，名字取事件名，线程那一行是空的。
同一条线程上叠在一起的段，Perfetto 会挪到溢出轨道再画回这条线程。

要更小就把整份 gzip：实测 1.31 MB → ~89 KB（它太重复了 —— 九千多个段只有 72 种
不同的名字，408 个 core 的元数据块字面一样）。
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict, deque
from pathlib import Path
from typing import List

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from tracetto import spans as S          # noqa: E402
from tracetto.reader import TraceReader   # noqa: E402

# 段上印的单元名与 tracetto 共用一张表（spans.SLOT_UNITS）：每个颜色槽一个名字，
# 带着行前缀（TS-DTE / CORE-DTE / DSA-DTE …）。Perfetto 按段上的名字上色，九个槽
# 名字两两不同，十行才分得开 —— 原来 TS 行的 DTE 段与 DTE-Core 行的段都叫
# `User_id x DTE y`，就撞成了一个色。


# 48 颗 chip 是 3×(4 层×4 列)，chip 号 = gy*4+gx。
# 逻辑号与 pcode 的 core_map 同一套，不跟 lpu_grid.h 的 RoleOf：
# 第一列 B 核优先落西口物理核 5，最后一列 R 核优先落东口物理核 4，两者都是逻辑 8。
# 计算核 0～7 在剩下的好核里每行取 4 个，按光栅顺序编号。
# 坏核，以及没派上角色的好核，没有逻辑号。
_GRID_X = 4
_LAYER_PER_TRAY = 4
_CHIPS_PER_TRAY = _GRID_X * _LAYER_PER_TRAY
_CORE_COLS = 5
_MIDDLE_BAD = 0x084
_LOGIC_OF_GX: dict = {}


def _bad_of(gx: int) -> set:
    mask = 0 if gx == 0 or gx + 1 == _GRID_X else _MIDDLE_BAD
    return {c for c in range(10) if (mask >> c) & 1}


def _pick_special(gx: int, bad: set):
    """B 核候选是物理 0 与 5，优先 5。R 核候选是物理 4 与 9，优先 4。"""
    if gx == 0:
        candidates, preferred = (0, 5), 5
    elif gx + 1 == _GRID_X:
        candidates, preferred = (4, 9), 4
    else:
        return None
    ordered = [preferred] if preferred in candidates else []
    ordered.extend(p for p in candidates if p != preferred)
    for p in ordered:
        if p in bad:
            continue
        rows = [0, 0]
        for q in range(10):
            if q in bad or q == p:
                continue
            rows[q // _CORE_COLS] += 1
        if rows[0] >= 4 and rows[1] >= 4:
            return p
    return None


def _pick_compute(available: set) -> list:
    """每行 4 个计算核。两行都能凑成矩形时跳过第 4 列，否则跟着对面缺的那一列跳。"""
    rows = [[p for p in sorted(available) if p // _CORE_COLS == r] for r in range(2)]

    def missing_col(row):
        miss = set(range(_CORE_COLS)) - {p % _CORE_COLS for p in row}
        if len(row) == 4 and len(miss) == 1:
            return next(iter(miss))
        return None

    def take4(row, skip):
        if len(row) == 4:
            return sorted(row)
        if skip is not None:
            picked = [p for p in row if p % _CORE_COLS != skip]
            if len(picked) >= 4:
                return sorted(picked)[:4]
        for col in (4, 0):
            picked = [p for p in row if p % _CORE_COLS != col]
            if len(picked) == 4:
                return sorted(picked)
        return sorted(row)[:4]

    s0, s1 = missing_col(rows[0]), missing_col(rows[1])
    if s0 is None and s1 is None:
        return take4(rows[0], 4) + take4(rows[1], 4)
    return take4(rows[0], s1) + take4(rows[1], s0 if s0 is not None else s1)


def _logic_of(gx: int, core: int):
    """物理核的逻辑号。没有角色时返回 None。"""
    table = _LOGIC_OF_GX.get(gx)
    if table is None:
        bad = _bad_of(gx)
        special = _pick_special(gx, bad)
        available = {c for c in range(10) if c not in bad and c != special}
        table = {c: None for c in range(10)}
        for logical, phys in enumerate(_pick_compute(available)):
            table[phys] = logical
        if special is not None:
            table[special] = 8
        _LOGIC_OF_GX[gx] = table
    return table[core]


def core_name(chip: int, core: int) -> str:
    """`tray0.chip0.phy5(logic8)`：tray、tray 内 chip、物理核、逻辑核。"""
    tray = chip // _CHIPS_PER_TRAY
    local = chip % _CHIPS_PER_TRAY
    logic = _logic_of(chip % _GRID_X, core)
    tag = "-" if logic is None else str(logic)
    return f"tray{tray}.chip{local}.phy{core}(logic{tag})"


def _events(reader, sigs: dict, name: str):
    sid = sigs.get(name)
    if sid is None:
        return [], []
    return reader.events(sid)


def _rises(ts, vs) -> List[int]:
    out: List[int] = []
    prev = 0
    for t, v in zip(ts, vs):
        if v > prev:
            out.append(t)
        prev = v
    return out


def _at(ts, vs, t: int, empty: int) -> int:
    if not ts:
        return empty
    return S.val_at(ts, vs, t)


def slice_name(kind: str, user: int, stream, task: int) -> str:
    """`Y: uid=X, sid=X, tid=X`。认不出的身份写成 `?`。

    字节数、维度这些不进名字，走事件的 args，Perfetto 在 Details 里显示。
    """
    uid = "?" if user < 0 else str(user)
    sid = "?" if stream is None or stream < 0 else str(stream)
    tid = "?" if task < 0 else str(task)
    return f"{kind}: uid={uid}, sid={sid}, tid={tid}"


def _packed_stream(reader, sigs, name: str, bit: int, t: int):
    ts, vs = _events(reader, sigs, name)
    if not ts:
        return None
    s = (S.val_at(ts, vs, t) >> (8 * bit)) & 0xFF
    return None if s == 0xFF else s


def _scalar_stream_at(reader, sigs, counter: str, ident: str, t: int):
    ts, vs = _events(reader, sigs, counter)
    if t not in _rises(ts, vs):
        return None
    its, ivs = _events(reader, sigs, ident)
    s = _at(its, ivs, t, 0xFF) & 0xFF
    return None if s == 0xFF else s


def _stream_for(reader, sigs, kind: str, bit: int, t0: int):
    """一段的起点那一拍上的 stream_id。老波形没有这些信号时返回 None。"""
    if kind == "ts":
        if bit == 0:
            return _packed_stream(reader, sigs, "ts_stream", bit, t0)
        for counter, ident in (("ts_create", "ts_create_stream"),
                               ("ts_install", "ts_install_stream")):
            got = _scalar_stream_at(reader, sigs, counter, ident, t0)
            if got is not None:
                return got
        return None
    table = {
        "rv": "rv_start_stream",
        "dsa": "dsa_start_stream",
        "isq": "dsa_task_trigger_stream",
        "exec": "dsa_task_dispatch_stream",
    }
    name = table.get(kind)
    if name is None:
        return None
    return _packed_stream(reader, sigs, name, bit, t0)


def pair_phases(begins: List[dict], ends: List[dict], t_end: int) -> List[dict]:
    """按 (user, task, stream) 把一步的起点配上下一次终点。

    同一拍起又停的（一个 tile 的加载）画成 1 拍。终点早于起点的丢掉。
    起点没有终点的延到波形末；终点没有起点的只画 1 拍。
    """
    ends_by: dict = {}
    for e in ends:
        ends_by.setdefault((e["user"], e["task"], e["stream"]), []).append(e["t"])
    for q in ends_by.values():
        q.sort()
    spans: List[dict] = []
    for b in sorted(begins, key=lambda x: x["t"]):
        key = (b["user"], b["task"], b["stream"])
        q = ends_by.get(key)
        t1 = None
        if q:
            while q and q[0] < b["t"]:
                early = q.pop(0)
                spans.append({"t0": early, "t1": early + 1, "user": b["user"],
                              "task": b["task"], "stream": b["stream"],
                              "detail": {}})
            if q:
                t1 = q.pop(0)
        if t1 is None:
            t1 = t_end
        if t1 <= b["t"]:
            t1 = b["t"] + 1
        spans.append({"t0": b["t"], "t1": t1, "user": b["user"],
                      "task": b["task"], "stream": b["stream"],
                      "detail": b.get("detail") or {}})
    for (user, task, stream), q in ends_by.items():
        for t in q:
            spans.append({"t0": t, "t1": t + 1, "user": user, "task": task,
                          "stream": stream, "detail": {}})
    spans.sort(key=lambda s: s["t0"])
    return spans


def _phase_edges(reader, sigs, prefix: str, with_bytes=False, with_dims=False):
    bts, bvs = _events(reader, sigs, prefix + "_begin")
    ets, evs = _events(reader, sigs, prefix + "_end")
    ut, uv = _events(reader, sigs, prefix + "_b_user")
    tt, tv = _events(reader, sigs, prefix + "_b_task")
    st, sv = _events(reader, sigs, prefix + "_b_stream")
    bt, bv = _events(reader, sigs, prefix + "_b_bytes") if with_bytes else ([], [])
    dims = {}
    if with_dims:
        for leaf in ("m", "k", "n", "experts", "kblock", "nblock"):
            dims[leaf] = _events(reader, sigs, prefix + "_" + leaf)
    eut, euv = _events(reader, sigs, prefix + "_e_user")
    ett, etv = _events(reader, sigs, prefix + "_e_task")
    est, esv = _events(reader, sigs, prefix + "_e_stream")

    def ident(t, uts, uvs):
        if not uts:
            return -1
        return S.val_at(uts, uvs, t)

    begins = []
    for t in _rises(bts, bvs):
        detail = {}
        if with_bytes and bt:
            detail["bytes"] = _at(bt, bv, t, 0)
        if with_dims and dims["k"][0]:
            detail = {
                "M": _at(*dims["m"], t, 1),
                "K": _at(*dims["k"], t, 0),
                "N": _at(*dims["n"], t, 0),
                "experts": _at(*dims["experts"], t, 0),
                "kblock": _at(*dims["kblock"], t, 1),
                "nblock": _at(*dims["nblock"], t, 1),
            }
        begins.append({"t": t, "user": ident(t, ut, uv), "task": ident(t, tt, tv),
                       "stream": ident(t, st, sv), "detail": detail})
    ends = [{"t": t, "user": ident(t, eut, euv), "task": ident(t, ett, etv),
             "stream": ident(t, est, esv)} for t in _rises(ets, evs)]
    return begins, ends


def _dte_steps(reader, sigs, t_end: int):
    """五个通道的读、写叠到各一条轨道上；排队是进中央队列到 dispatch。"""
    if "dte_q_begin" not in sigs and "dte_l0_rd_begin" not in sigs:
        return None
    qb, qe = _phase_edges(reader, sigs, "dte_q")
    out = {"Queue": pair_phases(qb, qe, t_end), "Read": [], "Write": []}
    for i in range(5):
        for half, key in (("rd", "Read"), ("wr", "Write")):
            prefix = f"dte_l{i}_{half}"
            if prefix + "_begin" not in sigs and prefix + "_end" not in sigs:
                continue
            b, e = _phase_edges(reader, sigs, prefix, with_bytes=True)
            out[key].extend(pair_phases(b, e, t_end))
    for key in out:
        out[key].sort(key=lambda s: s["t0"])
    return out


def _mu_steps(reader, sigs, t_end: int):
    if "mu_load_begin" not in sigs and "mu_comp_begin" not in sigs:
        return None
    out = {}
    for key, prefix, dims in (("Load", "mu_load", False),
                              ("Compute", "mu_comp", True),
                              ("Store", "mu_store", False)):
        b, e = _phase_edges(reader, sigs, prefix, with_dims=dims)
        out[key] = pair_phases(b, e, t_end)
    return out


def _us(cycles: int) -> float:
    """拍号 → JSON 微秒。Perfetto 乘 1000 后取整，结果是这么多纳秒。

    加 0.5 再除，是为了躲开 `拍/1000` 的二进制误差：有的拍直接除完再乘 1000
    会掉到下一纳秒下面，取整就少 1。"""
    return (cycles + 0.5) / 1000.0


def _append_span(row: List[dict], name: str, t0: int, t1: int, pid: int,
                 tid: int, detail: dict) -> None:
    if t1 <= t0:
        t1 = t0 + 1
    ev = {"name": name, "ph": "X", "ts": _us(t0), "dur": _us(t1 - t0),
          "pid": pid, "tid": tid}
    if detail:
        ev["args"] = detail
    row.append(ev)


_TS_UNIT = ("DTE", "MU", "VU")


def _mask_events(reader, sigs, sig: str, bit: int) -> List[dict]:
    """某一位上 0→1 的事件，带上那一拍的 user / task / stream。

    `ts_unit` 的身份在 `ts_task` / `ts_user` / `ts_stream`；其余边沿的身份
    信号是 `<sig>_task` 这一组。没有这条信号时身份留空，不当成 stream 0。
    """
    ts, vs = _events(reader, sigs, sig)
    if sig == "ts_unit":
        task_n, user_n, stream_n = "ts_task", "ts_user", "ts_stream"
    else:
        task_n, user_n, stream_n = sig + "_task", sig + "_user", sig + "_stream"
    tt, tv = _events(reader, sigs, task_n)
    ut, uv = _events(reader, sigs, user_n)
    st, sv = _events(reader, sigs, stream_n)
    out: List[dict] = []
    prev = 0
    for t, v in zip(ts, vs):
        on = (v >> bit) & 1
        if on and not prev:
            task = (S.val_at(tt, tv, t) >> (8 * bit)) & 0xFF if tt else 0xFF
            user = (S.val_at(ut, uv, t) >> (16 * bit)) & 0xFFFF if ut else 0xFFFF
            if st:
                stream = (S.val_at(st, sv, t) >> (8 * bit)) & 0xFF
                stream = None if stream == 0xFF else stream
            else:
                stream = None
            out.append({
                "t": t,
                "task": -1 if task == 0xFF else task,
                "user": -1 if user == 0xFFFF else user,
                "stream": stream,
            })
        prev = on
    return out


def _load_events(reader, sigs, counter: str) -> List[dict]:
    """`ts_create` / `ts_install` 涨一笔，就是一笔装进 stream。身份是标量。"""
    ts, vs = _events(reader, sigs, counter)
    tt, tv = _events(reader, sigs, counter + "_task")
    ut, uv = _events(reader, sigs, counter + "_user")
    st, sv = _events(reader, sigs, counter + "_stream")
    out: List[dict] = []
    prev = 0
    for t, v in zip(ts, vs):
        if v > prev:
            task = _at(tt, tv, t, 0xFF) & 0xFF
            user = _at(ut, uv, t, 0xFFFF) & 0xFFFF
            if st:
                stream = _at(st, sv, t, 0xFF) & 0xFF
                stream = None if stream == 0xFF else stream
            else:
                stream = None
            out.append({
                "t": t,
                "task": -1 if task == 0xFF else task,
                "user": -1 if user == 0xFFFF else user,
                "stream": stream,
            })
        prev = v
    return out


def _by_ident(events: List[dict]):
    buckets = defaultdict(deque)
    for e in events:
        buckets[(e["user"], e["task"])].append(e)
    return buckets


def _take_at(q, t: int):
    """弹出第一笔不早于 t 的事件。更早的是上一笔剩下的，丢掉。"""
    if not q:
        return None
    while q and q[0]["t"] < t:
        q.popleft()
    if not q:
        return None
    return q.popleft()


def _ts_steps(reader, sigs) -> List[tuple]:
    """TS 一条线程上的生命周期，按笔拆开，不按 DTE / MU / VU 拆。

    `Wait`：装进 stream 到下发。同一拍装完就下发的不画。
    `Issue`：下发到 RV core 接下。还没人接的只画下发那一拍。
    `TaskDone`：这一笔完成的那一拍。有 DSA 完成就用它（DTE / MU 的任务在
    DSA 交回时才结束），否则用 RV 交回。
    是哪一路只写进 args 的 `unit`，名字里不出现 DTE / MU / VU。
    """
    loads = _load_events(reader, sigs, "ts_create") + _load_events(reader, sigs, "ts_install")
    loads.sort(key=lambda e: e["t"])
    waiting = _by_ident(loads)
    issues = []
    for bit in range(3):
        for e in _mask_events(reader, sigs, "ts_unit", bit):
            e["unit"] = _TS_UNIT[bit]
            e["bit"] = bit
            issues.append(e)
    issues.sort(key=lambda e: e["t"])
    starts = [_by_ident(_mask_events(reader, sigs, "rv_start", bit)) for bit in range(3)]
    rv_dones = [_by_ident(_mask_events(reader, sigs, "rv_done", bit)) for bit in range(3)]
    dsa_dones = [_by_ident(_mask_events(reader, sigs, "dsa_done", bit)) for bit in range(3)]

    out = []
    for iss in issues:
        key = (iss["user"], iss["task"])
        load_q = waiting.get(key)
        load = _take_at(load_q, 0) if load_q and load_q[0]["t"] <= iss["t"] else None
        stream = iss["stream"]
        if stream is None and load is not None:
            stream = load["stream"]
        detail = {"unit": iss["unit"]}
        user, task, t = iss["user"], iss["task"], iss["t"]
        if load is not None and load["t"] < t:
            out.append((load["t"], t, slice_name("Wait", user, stream, task), detail))
        rv = _take_at(starts[iss["bit"]].get(key), t)
        if stream is None and rv is not None:
            stream = rv["stream"]
        t1 = rv["t"] if rv is not None and rv["t"] > t else t + 1
        out.append((t, t1, slice_name("Issue", user, stream, task), dict(detail)))
        done = _take_at(dsa_dones[iss["bit"]].get(key), t)
        if done is None:
            done = _take_at(rv_dones[iss["bit"]].get(key), t)
        else:
            _take_at(rv_dones[iss["bit"]].get(key), t)
        if done is not None:
            out.append((done["t"], done["t"] + 1,
                        slice_name("TaskDone", user, stream, task), dict(detail)))
    out.sort(key=lambda s: s[0])
    return out


# TS 的 stream 表深。剩余 credit = 表深减去在途项。软件把 0 配成 16，与这一档相同。
_STREAM_SLOTS = 16
# R2R 三个方向各 256 B/拍。波形里记的是发出的 flit 数，一拍一条按这个宽度折成字节。
_FLIT_BYTES = 256
_R2R_LEAF = {"out_mid": "R2R mid", "out_left": "R2R left", "out_right": "R2R right"}


def _append_counter(row: List[dict], name: str, t: int, value: int, pid: int,
                    tid: int) -> None:
    """线程上的一个计数。args 只用 value，轨道名就是 `name`，不会拼成两遍。"""
    row.append({"name": name, "ph": "C", "ts": _us(t), "pid": pid, "tid": tid,
                "args": {"value": int(value)}})


def _credit_samples(ts, vs) -> List[tuple]:
    """ts_inflight 的每个采样 → 还剩几个 stream 槽。"""
    return [(t, max(0, _STREAM_SLOTS - v)) for t, v in zip(ts, vs)]


def _busy_samples(spans) -> List[tuple]:
    """Compute 盖住时为 1，结束回到 0。

    同一拍先结束再开始的，保持 1。只隔 1 拍的也保持 1：结束沿的那一拍
    下一段紧跟着开始，Compute 轨道上两段是连着的，计数不该在中间掉到 0。
    """
    ordered = sorted(spans, key=lambda s: (s["t0"], s["t1"]))
    merged = []
    for s in ordered:
        if merged and s["t0"] <= merged[-1][1] + 1:
            merged[-1][1] = max(merged[-1][1], s["t1"])
        else:
            merged.append([s["t0"], s["t1"]])
    ev = []
    for t0, t1 in merged:
        ev.append((t0, 1))
        ev.append((t1, -1))
    ev.sort(key=lambda x: x[0])
    depth = 0
    last = None
    out = []
    i = 0
    while i < len(ev):
        t = ev[i][0]
        while i < len(ev) and ev[i][0] == t:
            depth += ev[i][1]
            i += 1
        level = 1 if depth > 0 else 0
        if level != last:
            out.append((t, level))
            last = level
    return out


def _bw_samples(ts, vs) -> List[tuple]:
    """累计 flit 数 → 这一拍发出的字节。两拍之间没再发就补一个 0，计数不会粘在高位。"""
    out = []
    prev = 0
    last_t = None
    for t, v in zip(ts, vs):
        dv = v - prev
        prev = v
        if dv <= 0:
            continue
        if last_t is not None and t > last_t + 1:
            out.append((last_t + 1, 0))
        out.append((t, dv * _FLIT_BYTES))
        last_t = t
    if last_t is not None:
        out.append((last_t + 1, 0))
    return out


def _router_samples(reader) -> dict:
    """`"chip.core"` → {计数名: [(拍, 字节)]}。没发出过 flit 的方向不收。"""
    out: dict = {}
    for sid, name in reader.tree_paths().items():
        m = S.RE_CORE.match(name)
        if not m or m.group(3) not in _R2R_LEAF:
            continue
        samples = _bw_samples(*reader.events(sid))
        if not samples:
            continue
        series = _R2R_LEAF[m.group(3)]
        out.setdefault(f"{m.group(1)}.{m.group(2)}", {})[series] = samples
    return out


def _classic(reader, sigs, lanes, row_idx: int, kind: str, bit: int, y: str):
    """tracetto 那一行折出来的段，换成 Perfetto 的标签。"""
    _name, parts = S.ROWS[row_idx]
    out = []
    for lane, _slot in parts:
        for seg in lanes[lane]:
            t0, t1, user, task = seg
            stream = _stream_for(reader, sigs, kind, bit, t0)
            out.append((t0, t1, slice_name(y, user, stream, task), None))
    out.sort(key=lambda s: s[0])
    return out


def build(prefix: str) -> dict:
    out = {"traceEvents": []}
    meta = out["traceEvents"]
    slices: List[dict] = []

    with TraceReader(prefix) as r:
        chips, core_sig = S.core_paths(r)
        t_end = S.trace_t_end(r, core_sig)
        router = _router_samples(r)

        # 与索引那边同一个顺序：按 chip 升序，再按 core 升序。
        # 只转发、自己没有 task 的核也收进来：R2R 带宽在它的出口上。
        ordered = []
        for chip in sorted(chips):
            for core in sorted(chips[chip]):
                key = f"{chip}.{core}"
                if key in core_sig or key in router:
                    ordered.append((chip, core, key))

        # 一个 core 一个进程，名字就是 tray/chip/phy。顺序靠
        # process_sort_index / thread_sort_index 钉死：Perfetto 按名字排轨道，
        # 不按 pid / tid。
        # tid 从 1 编：tid=0 是进程级那条轨道，段会并进进程那一行。
        # 整段没事件的线程不写：Perfetto 只画有事件的轨道。
        spans_cnt = 0
        pid_n = 0
        for ordinal, (chip, core, key) in enumerate(ordered):
            sigs = core_sig.get(key, {})
            lanes = S.lanes_of(S.core_spans(r, sigs, t_end))
            dte_steps = _dte_steps(r, sigs, t_end)
            mu_steps = _mu_steps(r, sigs, t_end)
            base = core_name(chip, core)

            def named(spans, y):
                return [(s["t0"], s["t1"],
                         slice_name(y, s["user"], s["stream"], s["task"]),
                         s.get("detail") or None)
                        for s in spans]

            # TS 按生命周期拆：Wait / Issue / TaskDone。Credit 是这条线程上的计数。
            # DTE / MU / VU 的各步骤收成 DTE-DSA / MU-DSA / VU-DSA。
            # Busy 是 MU-DSA 上的计数。R2R 带宽是 Router 上的计数。
            # 第三项：(计数名, [(拍, 值)])。
            inflight = sigs.get(S.SIG_TS_INFLIGHT)
            credit = (_credit_samples(*r.events(inflight))
                      if inflight is not None else [])
            rows = [("TS", _ts_steps(r, sigs),
                     [("Credit", credit)] if credit else [])]
            rows.append(("DTE-RVCore",
                         _classic(r, sigs, lanes, 3, "rv", 0, "RVCore"), []))
            if dte_steps is None:
                rows.append(("DTE-DSA",
                             _classic(r, sigs, lanes, 4, "dsa", 0, "DSA"), []))
            else:
                dte_segs = (named(dte_steps["Queue"], "Queue")
                            + named(dte_steps["Read"], "Read")
                            + named(dte_steps["Write"], "Write"))
                dte_segs.sort(key=lambda s: s[0])
                rows.append(("DTE-DSA", dte_segs, []))
            rows.append(("MU-RVCore",
                         _classic(r, sigs, lanes, 5, "rv", 1, "RVCore"), []))
            if mu_steps is None:
                rows.append(("MU-DSA",
                             _classic(r, sigs, lanes, 6, "dsa", 1, "DSA"), []))
            else:
                mu_segs = (named(mu_steps["Load"], "Load")
                           + named(mu_steps["Compute"], "Compute")
                           + named(mu_steps["Store"], "Store"))
                mu_segs.sort(key=lambda s: s[0])
                busy = _busy_samples(mu_steps["Compute"])
                rows.append(("MU-DSA", mu_segs,
                             [("Busy", busy)] if busy else []))
            rows.append(("VU-RVCore",
                         _classic(r, sigs, lanes, 7, "rv", 2, "RVCore"), []))
            vu_segs = (_classic(r, sigs, lanes, 8, "isq", 2, "ISQ")
                       + _classic(r, sigs, lanes, 9, "exec", 2, "Exec"))
            vu_segs.sort(key=lambda s: s[0])
            rows.append(("VU-DSA", vu_segs, []))
            r2r = router.get(key, {})
            rows.append(("Router", [],
                         [(series, r2r[series]) for series in
                          ("R2R mid", "R2R left", "R2R right") if series in r2r]))
            live = [(name, segs, ctr) for name, segs, ctr in rows if segs or ctr]
            if not live:
                continue
            pid_n += 1
            pid = pid_n
            meta.append({"ph": "M", "pid": pid, "tid": 0, "name": "process_name",
                         "args": {"name": base}})
            meta.append({"ph": "M", "pid": pid, "name": "process_sort_index",
                         "args": {"sort_index": ordinal}})
            for tid0, (thread, segs, ctr) in enumerate(live):
                tid = tid0 + 1
                meta.append({"ph": "M", "pid": pid, "tid": tid,
                             "name": "thread_name", "args": {"name": thread}})
                meta.append({"ph": "M", "pid": pid, "tid": tid,
                             "name": "thread_sort_index",
                             "args": {"sort_index": tid}})
                row_events: List[dict] = []
                for t0, t1, label, detail in segs:
                    _append_span(row_events, label, t0, t1, pid, tid, detail)
                    spans_cnt += 1
                for series, samples in ctr:
                    for t, value in samples:
                        _append_counter(row_events, series, t, value, pid, tid)
                row_events.sort(key=lambda e: e["ts"])
                slices.extend(row_events)

    out["traceEvents"] = meta + slices
    return out, pid_n, spans_cnt


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        prog="trace2perfetto",
        description="把 latch 的波形转成 Perfetto 能打开的 JSON")
    p.add_argument("prefix", nargs="?", default=None,
                   help="波形路径或名字里的一截；不给就把当前目录下的列出来挑")
    p.add_argument("-o", "--out", default=None, help="输出路径，默认落在波形旁边")
    p.add_argument("-q", "--quiet", action="store_true")
    args = p.parse_args(argv)

    from tracetto.reader import resolve_trace
    trace = resolve_trace(args.prefix)
    prefix = str(trace.with_suffix(""))
    path = Path(args.out) if args.out else path_of(trace)

    data, n_core, n_span = build(prefix)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, separators=(",", ":"))
        f.write("\n")

    if not args.quiet:
        size = path.stat().st_size
        n_ctr = sum(1 for ev in data["traceEvents"] if ev.get("ph") == "C")
        print(f"[trace2perfetto] {n_core} 个 core / {n_span} 段 / {n_ctr} 个计数"
              f" → {path}（{size} B）")
        print("   1 拍 = 1 ns；拖进 https://ui.perfetto.dev")
    return 0


def path_of(trace: Path) -> Path:
    """产物落在波形旁边：`<波形名>.perfetto.json`。

    不用 with_suffix：它只收一个点开头、且不许再含点的后缀。
    """
    return Path(str(trace.with_suffix("")) + ".perfetto.json")


if __name__ == "__main__":
    sys.exit(main())
