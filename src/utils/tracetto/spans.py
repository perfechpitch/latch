"""从波形里推出每个 core 的九条派生行。

波形是逐拍采样的，一个信号只在值变化时记一笔。所以「某一位连续为 1」就是它相邻两笔之间
的那段，段的拍数是两个时间戳之差。段的形状统一是 `[t0, t1, user, task]`，`user` / `task`
为 -1 表示认不出是哪一笔。

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
SIG_TS_INFLIGHT = "ts_inflight"
# RV core 的两端：起点是执行器接下队头那笔（PC 跳到 task_pc），终点是 kernel 交还。
SIG_RV_START = "rv_start"
SIG_RV_START_TASK = "rv_start_task"
SIG_RV_START_USER = "rv_start_user"
SIG_RV_DONE = "rv_done"
SIG_RV_DONE_TASK = "rv_done_task"
SIG_RV_DONE_USER = "rv_done_user"
# DSA 的两端：起点是各家过门槛那一拍（DTE 过 Commit 准入、MU 进 issue_q、VU 被
# ISQ 收下），终点是各家把完成报回去那一拍（VU 每条宏指令退休报一次）。
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

READ_SIGS = (SIG_TS_UNIT, SIG_TS_TASK, SIG_TS_USER) + RV_EDGE + DSA_EDGE
# 波形里缺了这几条，对应的行是空的，启动时要说清楚缺什么。
NEW_SIGS = (SIG_TS_USER,) + RV_EDGE + DSA_EDGE

# 索引里一个 core 的九条通道：(通道名, core_spans 里的那一组, 单元在位掩码里的位序)。
# 顺序就是通道号的顺序 —— 定死，别改（改了索引与前端都要跟着动）。
LANES = (
    ("TS · DTE", "chain", 0), ("TS · MU", "chain", 1), ("TS · VU", "chain", 2),
    ("DTE_Core", "core", 0), ("VU_Core", "core", 2), ("MU_Core", "core", 1),
    ("DTE_DSA", "dsa", 0), ("VU_DSA", "dsa", 2), ("MU_DSA", "dsa", 1),
)
LANES_PER_CORE = len(LANES)

# 画面上一个 core 的七行：(显示名, ((通道号, 颜色号), ...))。颜色一共九种。
#
# TS 那一条由三条通道叠起来 —— 实测这三个通道在时间上从不重叠（moe_lpu 全波形
# 347202 个忙拍里 0 拍重叠），所以直接画在同一格里，按单元用三种颜色区分；其余
# 六行各取一条通道。
#
# 颜色只按「行」与「单元」分，与 user 无关：同一个颜色下可以有很多个 user，谁是谁
# 靠段上的 User_id 字认。用户数上千，按 user 上色必然撞色，撞了反而更认不出来。
ROWS = (
    ("TS", ((0, 0), (1, 1), (2, 2))),
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


def pair_chain(issues: List[dict], starts: List[dict], dones: List[dict],
               t_end: int) -> List[List[int]]:
    """TS 那一行：每笔从下发到它做完 DSA 的全程。

    DSA 的起手与完成边沿都带 task 与 user，按身份认回下发的那一笔：同一 task、同一
    user 的下发里，下发时刻不晚于这条边沿的最后一笔；一笔都不早于它就归同一身份的
    第一笔。一笔 task 的 DSA 边沿可以有好几对（MU 一个 task 发几笔、VU 发几条宏指
    令），都认回同一笔，段 = [下发那一刻, 段末)：

      认到的起手多于完成     还没做完，段末是波形末
      认到了起手             段末是最后一次完成那一拍
      只认到完成             段末是最后一次完成的下一拍（DTE 的搬入只有完成）

    一条边沿也没认到的下发丢掉（有的 task 不经过 DSA）。身份是占位、或者没有同一身
    份的下发的边沿，按 edge_spans 折成段，标 -1。

    索引要求一条通道里的段互不相交。段按起点排好之后，前一段的末晚于后一段的起点
    时，前一段截在后一段起点那一拍：后一笔下发了，前一笔在 TS 这一行就到此为止，没
    画进来的那一截在 DSA 那一行看得到。
    """
    by_id: Dict[Tuple[int, int], List[int]] = {}
    for i, e in enumerate(issues):
        by_id.setdefault((e["task"], e["user"]), []).append(i)

    def owner(e: dict) -> int:
        idx = by_id.get((e["task"], e["user"]))
        if e["task"] == 0xFF or e["user"] == 0xFFFF or not idx:
            return -1
        hit = idx[0]
        for i in idx:
            if issues[i]["t"] <= e["t"]:
                hit = i
        return hit

    got_start = [0] * len(issues)
    got_done: List[List[int]] = [[] for _ in issues]
    lost_start: List[dict] = []
    lost_done: List[dict] = []
    for e in starts:
        i = owner(e)
        if i < 0:
            lost_start.append(e)
        else:
            got_start[i] += 1
    for e in dones:
        i = owner(e)
        if i < 0:
            lost_done.append(e)
        else:
            got_done[i].append(e["t"])

    out: List[List[int]] = []
    for i, e in enumerate(issues):
        if got_start[i] == 0 and not got_done[i]:
            continue
        if got_start[i] > len(got_done[i]):
            end = t_end
        elif got_start[i] == 0:
            end = max(got_done[i]) + 1
        else:
            end = max(got_done[i])
        out.append([e["t"], max(end, e["t"] + 1), e["user"], e["task"]])
    for s in edge_spans(lost_start, lost_done, t_end):
        out.append([s[0], s[1], -1, -1])
    out.sort(key=lambda s: s[0])
    for i in range(len(out) - 1):
        out[i][1] = min(out[i][1], out[i + 1][0])
    return [s for s in out if s[1] > s[0]]


def core_paths(reader: TraceReader) -> Tuple[Dict[int, set], Dict[str, Dict[str, int]]]:
    """扫一遍模块树，把 core 找出来。

    返回 (每个 chip 有哪些 core, 每个 core 有哪些要读的信号)。
    坏 core 与 TS 没配过任务的好 core 只有 Router 那一组信号，一个要读的都没有，但仍然会出现在
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
    moe_lpu 是要读的 35672、全信号是 36194。也不逐事件解：段头里就有 t_last。
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
        core_rows.append(edge_spans(rv_starts, rv_dones, t_end))
        dsa_rows.append(edge_spans(dsa_starts, dsa_dones, t_end))
        chain_rows.append(pair_chain(issue, dsa_starts, dsa_dones, t_end))
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
