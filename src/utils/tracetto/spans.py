"""从波形里推出每个 core 的九条派生行。

波形是逐拍采样的，一个信号只在值变化时记一笔。所以「某一位连续为 1」就是它相邻两笔之间
的那段，段的拍数是两个时间戳之差。段的形状统一是 `[t0, t1, user, task]`，`user` / `task`
为 -1 表示认不出配对上下发（真波形上这是常态，不是边缘情况）。

这一层是**参考实现**：索引里存的就是它算出来的段，自检拿它对拍。

单元侧的段不读模型里的电平忙位，读的是两对边沿信号：起点是各家「过门槛」那一拍，
终点是各家把完成报回去那一拍。一条边沿事件带 8 bit 的 task 与 16 bit 的 user，本拍
没有事件的那一路填占位（0xFF / 0xFFFF），与 `ts_task` / `ts_user` 同一套打包。
"""

from __future__ import annotations

import bisect
import re
from typing import Dict, List, Tuple

from .reader import TraceReader

UNITS = ("DTE", "MU", "VU")

SIG_TS_UNIT = "ts_unit"
SIG_TS_TASK = "ts_task"
SIG_TS_USER = "ts_user"
SIG_TS_DONE = "ts_done"
SIG_TS_INFLIGHT = "ts_inflight"
# RV core 的两端：起点是执行器接下队头那笔（PC 跳到 task_pc），终点是 kernel 交还。
SIG_RV_START = "rv_start"
SIG_RV_START_TASK = "rv_start_task"
SIG_RV_START_USER = "rv_start_user"
SIG_RV_DONE = "rv_done"
SIG_RV_DONE_TASK = "rv_done_task"
SIG_RV_DONE_USER = "rv_done_user"
# DSA 的两端：起点是各家过门槛那一拍（DTE 过 Commit 准入、MU 进 issue_q、VU 被
# ISQ 收下），终点是各家把完成报回去那一拍（VU 只记最后一条宏指令）。
SIG_DSA_START = "dsa_start"
SIG_DSA_START_TASK = "dsa_start_task"
SIG_DSA_START_USER = "dsa_start_user"
SIG_DSA_DONE = "dsa_done"
SIG_DSA_DONE_TASK = "dsa_done_task"
SIG_DSA_DONE_USER = "dsa_done_user"

# 两对边沿，每组六条：起点的位掩码 / task / user，终点的位掩码 / task / user。
RV_EDGE = (SIG_RV_START, SIG_RV_START_TASK, SIG_RV_START_USER,
           SIG_RV_DONE, SIG_RV_DONE_TASK, SIG_RV_DONE_USER)
DSA_EDGE = (SIG_DSA_START, SIG_DSA_START_TASK, SIG_DSA_START_USER,
            SIG_DSA_DONE, SIG_DSA_DONE_TASK, SIG_DSA_DONE_USER)

READ_SIGS = (SIG_TS_UNIT, SIG_TS_TASK, SIG_TS_USER, SIG_TS_DONE) + RV_EDGE + DSA_EDGE
# 2026-09 之后加的：比这更早的波形里一个都没有，认出来好把话说清楚。
NEW_SIGS = (SIG_TS_USER,) + RV_EDGE + DSA_EDGE

# 索引里一个 core 的九条通道：(通道名, core_spans 里的那一组, 单元在位掩码里的位序)。
# 顺序就是通道号的顺序 —— 定死，别改（改了索引与前端都要跟着动）。
LANES = (
    ("TS · DTE", "chain", 0), ("TS · MU", "chain", 1), ("TS · VU", "chain", 2),
    ("DTE_Core", "core", 0), ("VU_Core", "core", 2), ("MU_Core", "core", 1),
    ("DTE_DSA", "dsa", 0), ("VU_DSA", "dsa", 2), ("MU_DSA", "dsa", 1),
)
LANES_PER_CORE = len(LANES)

# 画面上一个 core 的九行：(显示名, ((通道号, 颜色号), ...))。颜色一共九种。
#
# TS 按设计文档拆成三行，DTE / MU / VU 各取一条通道。拆开之前是三条通道叠在同一
# 格里靠颜色分 —— 那靠的是「实测三者在时间上从不重叠」（moe_lpu 全波形 1531756 个
# 忙拍里 0 拍重叠）；即便如此，一眼答「哪一路在忙」仍要先认色，分开画就不用。其余
# 六行各取一条通道，现在每行都只有一条 —— 多通道叠一行的机制（row_parts 一行列多
# 个 part）留着，索引与前端都不用动它。
#
# 颜色只按「行」与「单元」分，与 user 无关：同一个颜色下可以有很多个 user，谁是谁
# 靠段上的 User_id 字认。用户数上千，按 user 上色必然撞色，撞了反而更认不出来。
ROWS = (
    ("TS-DTE", ((0, 0),)),
    ("TS-MU", ((1, 1),)),
    ("TS-VU", ((2, 2),)),
    ("DTE-Core", ((3, 3),)),
    ("DTE-DSA", ((6, 4),)),
    ("MU-Core", ((5, 5),)),
    ("MU-DSA", ((8, 6),)),
    ("VU-Core", ((4, 7),)),
    ("VU-DSA", ((7, 8),)),
)
ROW_NAMES = tuple(name for name, _ in ROWS)


# 段上印的单元名：把「哪一行」也写进去（TS-DTE / CORE-DTE / DSA-DTE …）。
#
# 不只是好看：Perfetto 那种按名字上色的工具，名字一样就同一个颜色。原来 TS 那一行
# 的 DTE 段与 DTE-Core 那一行的段都叫 `… DTE …`，于是同色；带上行名之后九个槽的
# 名字两两不同。顺序与 ROWS 里的颜色槽一致。
SLOT_UNITS = ("TS-DTE", "TS-MU", "TS-VU",
              "CORE-DTE", "DSA-DTE", "CORE-MU", "DSA-MU", "CORE-VU", "DSA-VU")


def row_parts() -> List[List[dict]]:
    """每一行画哪几条通道、各用什么颜色、段上印什么单元名 —— 给前端的那一份。"""
    out: List[List[dict]] = []
    for _name, parts in ROWS:
        out.append([{"lane": lane, "color": color,
                     "unit": SLOT_UNITS[color % len(SLOT_UNITS)]}
                    for lane, color in parts])
    return out

RE_CORE = re.compile(r"^chip(\d+)\.core(\d+)\.(\w+)$")


def val_at(ts: list, vs: list, t: int) -> int:
    """取不晚于 t 的最后一笔的值。波形开头之前一律当 0。"""
    if not ts:
        return 0
    i = bisect.bisect_right(ts, t) - 1
    return vs[i] if i >= 0 else 0


def issue_events(unit_ts: list, unit_vs: list, task_ts: list, task_vs: list,
                 user_ts: list, user_vs: list, bit: int) -> List[dict]:
    """某一位上 0→1 的事件，带上那一拍该路的 task 与 user。

    `ts_unit` 与两条打包信号都是「本拍这一路有没有」的形状，所以下发、RV 起、RV 完、
    DSA 起、DSA 完这五种事件都走这一个函数。
    """
    out: List[dict] = []
    prev = 0
    for t, v in zip(unit_ts, unit_vs):
        on = (v >> bit) & 1
        if on and not prev:
            out.append({
                "t": t,
                "task": (val_at(task_ts, task_vs, t) >> (8 * bit)) & 0xFF,
                "user": (val_at(user_ts, user_vs, t) >> (16 * bit)) & 0xFFFF,
            })
        prev = on
    return out


def _task_tag(task: int) -> int:
    """0xFF 是「这一路本拍没有事件」的填充，认不出来时统一写 -1。"""
    return -1 if task == 0xFF else task


def _user_tag(user: int) -> int:
    return -1 if user == 0xFFFF else user


def edge_spans(starts: List[dict], dones: List[dict],
               t_end: int) -> List[List[int]]:
    """一对边沿 → 互不相交的区间：手上还压着没做完的就一直是忙。

    不是「一起一完对一段」。一个单元在同一段时间里可以压着好几笔 —— MU 的
    issue_q 深 16、VU 的一条 task 要连发好几条宏指令、RV core 的队列深 2 —— 折
    成一段之后这几种情况画出来都一样：从最早那笔接过手，到最后一笔交回去。身份
    取最早那笔的。

    同一拍上完成与起手撞在一起时先算完成，两段因此在同一拍首尾相接，而不是并成
    一段。起点没配上完成的（波形从中间开始记）只记那一拍，标签取完成那笔的。
    """
    ev = [(e["t"], 1, e) for e in starts] + [(e["t"], 0, e) for e in dones]
    ev.sort(key=lambda x: (x[0], x[1]))       # 同一拍：完成（0）排在起手（1）前面
    out: List[List[int]] = []
    depth = 0
    open_t = open_task = open_user = 0
    for t, kind, e in ev:
        if kind == 1:
            if depth == 0:
                open_t, open_task, open_user = t, e["task"], e["user"]
            depth += 1
            continue
        if depth == 0:
            out.append([t, t + 1, _user_tag(e["user"]), _task_tag(e["task"])])
            continue
        depth -= 1
        if depth == 0:
            out.append([open_t, t, _user_tag(open_user),
                        _task_tag(open_task)])
    if depth > 0:
        out.append([open_t, t_end, _user_tag(open_user), _task_tag(open_task)])
    return out


def pair_chain(spans: List[Tuple[int, int]],
               events: List[dict]) -> List[List[int]]:
    """TS 三行里的任一行（按单元各跑一次）：每笔从下发到它做完 DSA 的全程，
    所以段起点前移到配对的下发那一刻。

    一笔 task 的 DSA 活儿会碎成好几段 —— 真波形上这是常态。所以认领不是「一段对一
    笔」：按时间序贪心，每个下发事件认领「起点不早于它、还没被认领的」第一个段，
    再把「下一个下发之前」起头的后续段一并吃进同一段，段 =
    [下发那一刻, 最后吃进来那一段的末)。认领不上的下发事件丢掉（有的 task 不经过
    DSA）；没被认领的段退回原区间并标 -1 —— 下发之前就有的那些走这一支。不按下发
    序号与 ts_done 硬配 —— ts_done 不分路，几笔并发时分不清哪次完成是哪一笔的。
    """
    out: List[List[int]] = []
    used = [False] * len(spans)
    for i, e in enumerate(events):
        # 下一笔下发的那一刻就是这一笔的吸收上界：它之后起头的碎段归下一笔。
        nxt = events[i + 1]["t"] if i + 1 < len(events) else None
        end = None
        for j, (t0, t1) in enumerate(spans):
            if used[j] or t0 < e["t"]:
                continue
            # 段按起点升序，撞上下一笔那一刻之后就不用再看了：后面都归下一笔。
            if nxt is not None and t0 >= nxt:
                break
            used[j] = True
            end = t1
        if end is not None:
            out.append([e["t"], end, e["user"], e["task"]])
    for j, (t0, t1) in enumerate(spans):
        if not used[j]:
            out.append([t0, t1, -1, -1])
    out.sort(key=lambda s: s[0])
    return out


def delta_ticks(ts: list, vs: list) -> List[int]:
    """只加不清零的累计量，取相邻两拍的差，差大于 0 的地方就是一次完成。"""
    out: List[int] = []
    prev = 0
    for t, v in zip(ts, vs):
        if v > prev:
            out.append(t)
        prev = v
    return out


def core_paths(reader: TraceReader) -> Tuple[Dict[int, set], Dict[str, Dict[str, int]]]:
    """扫一遍模块树，把 core 找出来。

    返回 (每个 chip 有哪些 core, 每个 core 有哪些要读的信号)。
    不派角色的 core 只有 Router 那一组信号，一个要读的都没有，但仍然会出现在
    树里 —— 所以 core 的集合是从「任何挂在 chipN.coreM 下的信号」取的，不是
    从要读的那几个取的。
    """
    paths = reader.tree_paths()
    chips: Dict[int, set] = {}
    core_sig: Dict[str, Dict[str, int]] = {}
    wanted = set(READ_SIGS) | {SIG_TS_INFLIGHT}
    for sig_id in reader.signals():
        name = paths.get(sig_id)
        if not name:
            continue
        m = RE_CORE.match(name)
        if not m:
            continue
        c, k, leaf = int(m.group(1)), int(m.group(2)), m.group(3)
        chips.setdefault(c, set()).add(k)
        if leaf in wanted:
            core_sig.setdefault(f"{c}.{k}", {})[leaf] = sig_id
    return chips, core_sig


def core_sort_key(key: str) -> tuple:
    """`"12.3"` → `(12, 3)`。行号、core 号都按这个顺序发，定死。"""
    chip, core = key.split(".")
    return int(chip), int(core)


def trace_t_end(reader: TraceReader, core_sig: Dict[str, Dict[str, int]]) -> int:
    """波形的末尾 = 要读的那些信号里最大的 t_last。

    **不能用全信号的 max** —— 别的模块（sink、c2c 那些）会把它顶上去，实测
    moe_lpu 是要读的 38758、全信号是 39404。也不逐事件解：段头里就有 t_last。
    """
    out = 0
    seen = set()
    for sigs in core_sig.values():
        for sid in sigs.values():
            if sid in seen:
                continue
            seen.add(sid)
            segs = reader.one_pass(sid)
            if segs:
                out = max(out, int(segs[-1].t_last))
    return out


def core_spans(reader: TraceReader, sigs: Dict[str, int],
               t_end: int) -> Dict[str, List[List[List[int]]]]:
    """一个 core 的九条行。键是 `core` / `dsa` / `chain`，各三个单元一个 list。"""

    def ev(name: str) -> Tuple[list, list]:
        sid = sigs.get(name)
        if sid is None:
            return [], []
        return reader.events(sid)

    ut, uv = ev(SIG_TS_UNIT)
    tt, tv = ev(SIG_TS_TASK)
    xt, xv = ev(SIG_TS_USER)

    def ends_of(group: Tuple[str, ...], bit: int) -> Tuple[List[dict], List[dict]]:
        """一组六条边沿信号在 `bit` 那一路上的 (起点事件, 终点事件)。"""
        def one(base: int) -> List[dict]:
            return issue_events(*ev(group[base]), *ev(group[base + 1]),
                                *ev(group[base + 2]), bit)
        return one(0), one(3)

    core_rows: List[List[List[int]]] = []
    dsa_rows: List[List[List[int]]] = []
    chain_rows: List[List[List[int]]] = []
    for u in range(len(UNITS)):
        issue = issue_events(ut, uv, tt, tv, xt, xv, u)
        rv_starts, rv_dones = ends_of(RV_EDGE, u)
        dsa_starts, dsa_dones = ends_of(DSA_EDGE, u)
        dsa = edge_spans(dsa_starts, dsa_dones, t_end)
        core_rows.append(edge_spans(rv_starts, rv_dones, t_end))
        dsa_rows.append(dsa)
        chain_rows.append(pair_chain([(s[0], s[1]) for s in dsa], issue))
    return {"core": core_rows, "dsa": dsa_rows, "chain": chain_rows}


def lanes_of(spans: Dict[str, List[List[List[int]]]]) -> List[List[List[int]]]:
    """core_spans 的产物 → 一个 core 的九条通道，顺序与 `LANES` 一致。"""
    return [spans[group][u] for _, group, u in LANES]


def missing_signals(reader: TraceReader, core_sig: Dict[str, Dict[str, int]]) -> List[str]:
    """这份波形里没有的新信号（加信号之前生成的波形一个都没有）。"""
    have = set()
    for sigs in core_sig.values():
        have |= set(sigs)
    return [n for n in NEW_SIGS if n not in have]
