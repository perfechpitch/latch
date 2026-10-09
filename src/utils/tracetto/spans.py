"""从波形里推出每个 core 的十三条派生行。

波形是逐拍采样的，一个信号只在值变化时记一笔。所以“某一位连续为 1”就是它相邻两笔之间
的那段，段的拍数是两个时间戳之差。段的形状统一是 `[t0, t1, user, task]`，`user` / `task`
为 -1 表示认不出配对上下发（真波形上这是常态，不是边缘情况）。

这一层是**参考实现**：索引里存的就是它算出来的段，自检拿它对拍。

单元侧的段不读模型里的电平忙位，读的是边沿信号：起点是各家“过门槛”那一拍，终点是
各家把完成报回去那一拍。一条边沿事件带 8 bit 的 task 与 16 bit 的 user，本拍没有事件
的那一路填占位（0xFF / 0xFFFF），与 `ts_task` / `ts_user` 同一套打包。

TS 那一族（TS-DTE-DATAIN / TS-DTE / TS-MU / TS-VU）量的都是**「这一步在 TS 里等了多久」**：
起点是它进入 TS、可以被下发那一刻，终点是这一路的 RV core 接下它（`rv_start`）。
DTE 那一路有两类任务（主线 + Router 触发的搬入），所以拆成两行 —— 主线那刻是「装进
stream」，搬入那刻是「被下发」，见 `dte_spans()`。

起点那一侧是**单调计数器 + 标量身份**（不是位掩码），终点那一侧是位掩码 + 按位打包，
形状完全不同，所以配对一律按 (user, task)。
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
# TaskCtrl 把一步装进 stream。两条来源：第一个 task 走 create（建表），其余走
# install（装后继）—— core.h 的 EmitStep() 说这是“同一件事的两种来源”。都是**单调
# 计数器**（只加不清零，涨了的那一拍就是装进去一笔），一拍最多一笔；身份是标量而不是
# 位掩码：task 8 bit、user 16 bit，本拍没有就填 0xFF / 0xFFFF。TS-MU / TS-VU 的起点。
SIG_TS_CREATE = "ts_create"
SIG_TS_CREATE_TASK = "ts_create_task"
SIG_TS_CREATE_USER = "ts_create_user"
SIG_TS_INSTALL = "ts_install"
SIG_TS_INSTALL_TASK = "ts_install_task"
SIG_TS_INSTALL_USER = "ts_install_user"
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
# 一笔任务在单元里的两端，比 DSA 那对更靠前：起点是单元把它收进自己的配置入口那一拍
# （MU 写 TASK_TRIGGER 被 regfile 锁成一笔、VU 被 config_register 收下 trigger），
# 终点是它真正发行进执行通路那一拍（MU 第一个 tile 开始发起访存、VU 宏指令进流水）。
# MU 抬 bit1、VU 抬 bit2，DTE 恒 0。
SIG_TASK_TRIG = "dsa_task_trigger"
SIG_TASK_TRIG_TASK = "dsa_task_trigger_task"
SIG_TASK_TRIG_USER = "dsa_task_trigger_user"
SIG_TASK_DISP = "dsa_task_dispatch"
SIG_TASK_DISP_TASK = "dsa_task_dispatch_task"
SIG_TASK_DISP_USER = "dsa_task_dispatch_user"
# 一笔任务在矩阵执行单元里进出所跨那一段的两端。只有 MU 有（恒抬 bit1）：起点是它
# 第一个**真正产出**的 prim 进 exe —— 不是第一个 tile，一列算完那一遍才产 prim；
# 终点是最后一个 prim 被取走那一拍（取走之后流水线那一格才真的空出来）。
SIG_CALC_START = "dsa_calc_start"
SIG_CALC_START_TASK = "dsa_calc_start_task"
SIG_CALC_START_USER = "dsa_calc_start_user"
SIG_CALC_DONE = "dsa_calc_done"
SIG_CALC_DONE_TASK = "dsa_calc_done_task"
SIG_CALC_DONE_USER = "dsa_calc_done_user"

# 三对边沿，每组六条：起点的位掩码 / task / user，终点的位掩码 / task / user。
RV_EDGE = (SIG_RV_START, SIG_RV_START_TASK, SIG_RV_START_USER,
           SIG_RV_DONE, SIG_RV_DONE_TASK, SIG_RV_DONE_USER)
DSA_EDGE = (SIG_DSA_START, SIG_DSA_START_TASK, SIG_DSA_START_USER,
            SIG_DSA_DONE, SIG_DSA_DONE_TASK, SIG_DSA_DONE_USER)
# MU / VU 那一对：收下 → 真正发行。
TASK_START_EDGE = (SIG_TASK_TRIG, SIG_TASK_TRIG_TASK, SIG_TASK_TRIG_USER,
                   SIG_TASK_DISP, SIG_TASK_DISP_TASK, SIG_TASK_DISP_USER)
# MU 那一对：第一个 prim 进执行单元 → 最后一个 prim 取走。**这不是一段的切分，而是
# 嵌在 MU-DSA 里的子区间** —— 画面上它整段落在 MU-DSA 里面。
CALC_EDGE = (SIG_CALC_START, SIG_CALC_START_TASK, SIG_CALC_START_USER,
             SIG_CALC_DONE, SIG_CALC_DONE_TASK, SIG_CALC_DONE_USER)

# 「装进 stream」那两条：也是六条，但是计数器不是位掩码，凑不成一对边沿，单独成组。
STEP_SIGS = (SIG_TS_CREATE, SIG_TS_CREATE_TASK, SIG_TS_CREATE_USER,
             SIG_TS_INSTALL, SIG_TS_INSTALL_TASK, SIG_TS_INSTALL_USER)

READ_SIGS = ((SIG_TS_UNIT, SIG_TS_TASK, SIG_TS_USER, SIG_TS_DONE) + RV_EDGE +
             DSA_EDGE + TASK_START_EDGE + STEP_SIGS + CALC_EDGE)
# 2026-09 之后加的：比这更早的波形里一个都没有，认出来好把话说清楚。末尾那些也在
# 这里 —— TS-MU / TS-VU 与 MU-DSA-CALC 现在要读它们，老波形缺了那些行就是空的。
NEW_SIGS = ((SIG_TS_USER,) + RV_EDGE + DSA_EDGE + TASK_START_EDGE + STEP_SIGS +
            CALC_EDGE)

# 索引里一个 core 的十三条通道：(通道名, core_spans 里的那一组, 单元在位掩码里的位序)。
# 顺序就是通道号的顺序 —— 定死，别改（改了索引与前端都要跟着动）。新加的通道一律
# 追加在末尾，前面十二条的通道号才不动（旧波形、旧断言都按号认）。注意**通道号与行号
# 不是一回事**：行序是纯呈现层，见下面的 ROWS。
LANES = (
    ("TS · DTE", "chain", 0), ("TS · MU", "chain", 1), ("TS · VU", "chain", 2),
    ("DTE_Core", "core", 0), ("VU_Core", "core", 2), ("MU_Core", "core", 1),
    ("DTE_DSA", "dsa", 0), ("VU_DSA", "dsa", 2), ("MU_DSA", "dsa", 1),
    ("VU_DSA_ISQ", "vuisq", 2),
    ("DTE_DATAIN", "datain", 0),
    ("MU_DSA_ISQ", "muisq", 1),
    ("MU_DSA_CALC", "calc", 1),
)
LANES_PER_CORE = len(LANES)

# 画面上一个 core 的十三行：(显示名, ((通道号, 颜色号), ...))。颜色一共十三种。
#
# TS 那一族行都量「这一步在 TS 里等了多久」，按起点分：DTE 拆成主线（TS-DTE）与
# Router 触发的搬入（TS-DTE-DATAIN）两行，MU / VU 各一行。DTE-DATAIN 摆在 TS-DTE
# **正上方**，读起来连得上。拆开之前是几条通道叠在同一格里靠颜色分 —— 那靠的是
# “实测它们在时间上从不重叠”；即便如此，一眼答“哪一路在忙”仍要先认色，分开画就不
# 用。其余几行各取一条通道，现在每行都只有一条 —— 多通道叠一行的机制（row_parts
# 一行列多个 part）留着，索引与前端都不用动它。
#
# 颜色只按“行”与“单元”分，与 user 无关：同一个颜色下可以有很多个 user，谁是谁
# 靠段上的 User_id 字认。用户数上千，按 user 上色必然撞色，撞了反而更认不出来。
ROWS = (
    # 摆在 TS-DTE 上方：同一族里先看「Router 送来的」再看「主线自己的」。
    ("TS-DTE-DATAIN", ((10, 10),)),
    ("TS-DTE", ((0, 0),)),
    ("TS-MU", ((1, 1),)),
    ("TS-VU", ((2, 2),)),
    ("DTE-Core", ((3, 3),)),
    ("DTE-DSA", ((6, 4),)),
    ("MU-Core", ((5, 5),)),
    # MU 的 DSA 那一行也拆成两段：MU-DSA-ISQ 是「被 regfile 收下 → 真正发行进执行
    # 通路」，MU-DSA 是「真正发行 → 完成」。与 VU 那两行同构。
    ("MU-DSA-ISQ", ((11, 11),)),
    ("MU-DSA", ((8, 6),)),
    # 与上面几行不同：**这不是切分，是嵌在 MU-DSA 里面的子区间** —— 量的是这笔任务的
    # prim 在矩阵执行单元里进出所跨的那一段，整段落在 MU-DSA 里面。
    ("MU-DSA-CALC", ((12, 12),)),
    ("VU-Core", ((4, 7),)),
    # VU 的 DSA 那一行拆成两段：VU-DSA-ISQ 是「trigger 收下 → 真正发行进执行流水」，
    # VU-DSA 是「真正发行 → EVENT_EN 退休」。两行首尾相接，起点那一拍是同一个。
    ("VU-DSA-ISQ", ((9, 9),)),
    ("VU-DSA", ((7, 8),)),
)
ROW_NAMES = tuple(name for name, _ in ROWS)


# 段上印的单元名：把“哪一行”也写进去（TS-DTE / CORE-DTE / DSA-DTE …）。
#
# 不只是好看：Perfetto 那种按名字上色的工具，名字一样就同一个颜色。原来 TS 那一行
# 的 DTE 段与 DTE-Core 那一行的段都叫 `… DTE …`，于是同色；带上行名之后十三个槽的
# 名字两两不同。顺序与 ROWS 里的颜色槽一致（按颜色号索引，不是按行序）。
SLOT_UNITS = ("TS-DTE", "TS-MU", "TS-VU",
              "CORE-DTE", "DSA-DTE", "CORE-MU", "DSA-MU", "CORE-VU", "DSA-VU",
              "DSA-ISQ-VU", "DATAIN-DTE", "DSA-ISQ-MU", "DSA-CALC-MU")


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

    `ts_unit` 与两条打包信号都是“本拍这一路有没有”的形状，所以下发、RV 起、RV 完、
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


def step_events(counter_ts: list, counter_vs: list, task_ts: list,
                task_vs: list, user_ts: list, user_vs: list) -> List[dict]:
    """单调计数器 → 「装进去一笔」的事件，带上那一笔的身份。

    `issue_events` 的孪生兄弟，形状不一样：那几条是位掩码 + 按位打包的 task / user，
    这几条是**只加不清零的笔数**，身份是标量。所以判据是“这一拍的值比上一拍大”，
    而不是某一位 0→1。一拍最多装一笔，不用管一次涨多笔。

    身份在计数器涨的那一拍取 —— `ts_create_task` / `ts_install_task` 那几条只在涨的
    那一拍带真值，其余拍是 0xFF / 0xFFFF 的占位。
    """
    out: List[dict] = []
    prev = 0
    for t, v in zip(counter_ts, counter_vs):
        if v > prev:
            out.append({
                "t": t,
                "task": val_at(task_ts, task_vs, t) & 0xFF,
                "user": val_at(user_ts, user_vs, t) & 0xFFFF,
            })
        prev = v
    return out


def _task_tag(task: int) -> int:
    """0xFF 是“这一路本拍没有事件”的填充，认不出来时统一写 -1。"""
    return -1 if task == 0xFF else task


def _user_tag(user: int) -> int:
    return -1 if user == 0xFFFF else user


def edge_spans(starts: List[dict], dones: List[dict],
               t_end: int) -> List[List[int]]:
    """一对边沿 → 互不相交的区间：手上还压着没做完的就一直是忙。

    不是“一起一完对一段”。一个单元在同一段时间里可以压着好几笔 —— MU 的
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


def user_spans(starts: List[dict], dones: List[dict],
               t_end: int) -> List[List[int]]:
    """每个用户各成一段。几笔叠在一起时不并成一段，身份取起点那一笔。

    起点对上同一 user、同一 task 的下一次完成。同一拍先算完成再算起手。
    完成没配上起点的只记那一拍；起点没配上完成的延到波形末。
    """
    ev = [(e["t"], 1, e) for e in starts] + [(e["t"], 0, e) for e in dones]
    ev.sort(key=lambda x: (x[0], x[1]))
    pending: Dict[Tuple[int, int], List[int]] = {}
    out: List[List[int]] = []
    for t, kind, e in ev:
        key = (e["user"], e["task"])
        if kind == 1:
            pending.setdefault(key, []).append(t)
            continue
        q = pending.get(key)
        if not q:
            out.append([t, t + 1, _user_tag(e["user"]), _task_tag(e["task"])])
            continue
        t0 = q.pop(0)
        out.append([t0, t, _user_tag(e["user"]), _task_tag(e["task"])])
    for (user, task), times in pending.items():
        for t0 in times:
            out.append([t0, t_end, _user_tag(user), _task_tag(task)])
    out.sort(key=lambda s: (s[0], s[1]))
    return out


def step_chain(load: List[dict], issue: List[dict],
               rv_starts: List[dict], t_end: int) -> List[List[int]]:
    """TS-MU / TS-VU 那一行：一步从「TaskCtrl 把它装进 stream」到「这一路的 RV core
    接下它」。

    量的是这一步在 TS 里等的时间 —— 等 credit、等发射通路空出来、等前一笔从 RV core
    那边腾出槽位。不含 RV core 与 DSA 的任何时间。

    三条流的形状各不相同：`load` 是装进 stream（整核一份的单调计数器），`issue` 是
    下发到本单元（`ts_unit` 的这一位），`rv_starts` 是本单元 RV core 接下。

    **先按身份把 `load` 与本单元的下发一一对上**（装进来一定在下发之前，先到先得），
    对不上的丢掉 —— 那是别的单元的活。不这么筛会很难看：实测 MU 上整核装进 82231
    笔、本单元只下发 25224 笔；只按“身份在不在下发集合里”筛的话，会多出 654 笔配不
    上的（身份跟别的 task 撞了），每笔从它装进来那一拍一直画到波形末，整行都是假的。

    配好的起点再与 `rv_start` 配 —— 这两条都是本单元的，实测 MU 上 25224 对 25170，
    近乎一一对应。两种配不上都不丢：
      - 装进来了、下发了、这一路还没接下 → 延到波形末（那笔确实还在等）
      - RV 起了、压根没有装进来那一拍 → 只记一拍（DataIn 任务既没建表也没装后继）
    """
    queues: Dict[Tuple[int, int], List[int]] = {}
    for e in load:
        queues.setdefault((e["user"], e["task"]), []).append(e["t"])
    started: List[dict] = []
    for e in issue:
        q = queues.get((e["user"], e["task"]))
        if q and q[0] <= e["t"]:
            started.append({"t": q.pop(0), "user": e["user"],
                            "task": e["task"]})
    return user_spans(started, rv_starts, t_end)


def dte_spans(load: List[dict], issue: List[dict], rv_starts: List[dict],
              t_end: int) -> Tuple[List[List[int]], List[List[int]]]:
    """TS-DTE 那两行：一趟队列把 DTE 的下发分成主线与 Router 触发的搬入。

      主线（TS-DTE）        [TaskCtrl 把它装进 stream 那一刻, 这一路的 rv_start]
      搬入（TS-DTE-DATAIN） [Router 的数据到了、它被下发那一刻, 这一路的 rv_start]

    **怎么分**：搬入任务（chain 里的 wait_wake 项）是 Router 的数据一到就下发，与主线
    走到哪无关（`test/bach/README.md`），所以它在**下发那一刻**在 stream 里认领不到自己
    那一笔；主线任务则必然先装进 stream、再下发。于是走一遍 `load` 的队列、按“认领得上
    / 认领不上”分就够了。认领规则要求 `load <= 下发`，所以主线那一边的起点天然早于终点，
    不会出现反向段。

    量的是这一步在 TS 里等的时间（等 credit、等发射通路空出来、等前一笔从 RV core 那边
    腾出槽位），不含 RV core 与 DSA 的任何时间。

    **配对不能用两次 `user_spans`** —— 那会把同一批 `rv_start` 认领两遍。所以一笔下发
    只认领一个 `rv_start`，认完再按上面那条分左右。两个队列都**按位置对齐**（同一身份
    的第 i 次下发对第 i 笔 load / 第 i 个 rv_start），不是“取最早那个还没用掉的” ——
    后者会漂：一笔 load 若被自己那次下发跳过（晚到），它会一直挂在队首，被很远以后的
    同身份下发认走，画出一根几千拍的长条。实测 `synth_scaled` 上就这样漂出 909 拍的段。

    配不上下发的 `rv_start`（实测 557 笔）不画在这两行上，它们仍在 `DTE-Core` 那一行里；
    配不上 `rv_start` 的下发（实测 0 笔）延到波形末，那笔确实还在等。
    """
    loads: Dict[Tuple[int, int], List[int]] = {}
    for e in load:
        loads.setdefault((e["user"], e["task"]), []).append(e["t"])
    rvs: Dict[Tuple[int, int], List[int]] = {}
    for e in rv_starts:
        rvs.setdefault((e["user"], e["task"]), []).append(e["t"])

    main: List[List[int]] = []
    datain: List[List[int]] = []
    for e in issue:
        key = (e["user"], e["task"])
        rq = rvs.get(key)
        while rq and rq[0] < e["t"]:
            rq.pop(0)          # 早于这次下发的（波形开头就在飞）不认
        end = rq.pop(0) if rq else t_end
        lq = loads.get(key)
        t0 = lq.pop(0) if lq else None
        if t0 is not None and t0 <= e["t"]:
            main.append([t0, end, _user_tag(e["user"]), _task_tag(e["task"])])
        else:
            datain.append([e["t"], end, _user_tag(e["user"]),
                           _task_tag(e["task"])])
    main.sort(key=lambda s: (s[0], s[1]))
    datain.sort(key=lambda s: (s[0], s[1]))
    return main, datain


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
    树里 —— 所以 core 的集合是从“任何挂在 chipN.coreM 下的信号”取的，不是
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
    """一个 core 的十三条行。键是 `core` / `dsa` / `chain` / `datain` / `vuisq` /
    `muisq` / `calc`，各三个单元一个 list（`vuisq` 只有 VU 那条、`muisq` 与 `calc`
    只有 MU 那条、`datain` 只有 DTE 那条有数据，其余恒空）。"""

    def ev(name: str) -> Tuple[list, list]:
        sid = sigs.get(name)
        if sid is None:
            return [], []
        return reader.events(sid)

    ut, uv = ev(SIG_TS_UNIT)
    tt, tv = ev(SIG_TS_TASK)
    xt, xv = ev(SIG_TS_USER)

    # 这一步「装进 stream」的那一拍：create（第一个 task）与 install（装后继）合起来
    # 才是完整的一条流 —— 只认 install 的话，每个用户的第一笔没有起点。两条都是按
    # 时间升序的，合起来重排一次即可。
    load = sorted(
        step_events(*ev(SIG_TS_CREATE), *ev(SIG_TS_CREATE_TASK),
                    *ev(SIG_TS_CREATE_USER)) +
        step_events(*ev(SIG_TS_INSTALL), *ev(SIG_TS_INSTALL_TASK),
                    *ev(SIG_TS_INSTALL_USER)),
        key=lambda e: e["t"])

    def ends_of(group: Tuple[str, ...], bit: int) -> Tuple[List[dict], List[dict]]:
        """一组六条边沿信号在 `bit` 那一路上的 (起点事件, 终点事件)。"""
        def one(base: int) -> List[dict]:
            return issue_events(*ev(group[base]), *ev(group[base + 1]),
                                *ev(group[base + 2]), bit)
        return one(0), one(3)

    core_rows: List[List[List[int]]] = []
    dsa_rows: List[List[List[int]]] = []
    chain_rows: List[List[List[int]]] = []
    datain_rows: List[List[List[int]]] = []
    vuisq_rows: List[List[List[int]]] = []
    muisq_rows: List[List[List[int]]] = []
    calc_rows: List[List[List[int]]] = []
    for u in range(len(UNITS)):
        rv_starts, rv_dones = ends_of(RV_EDGE, u)
        dsa_starts, dsa_dones = ends_of(DSA_EDGE, u)
        # 一个核上可以同时压着几个用户。Core 与 DSA 都按用户各画一段，不并成
        # 最早那笔的长段。
        core_rows.append(user_spans(rv_starts, rv_dones, t_end))
        dsa = user_spans(dsa_starts, dsa_dones, t_end)
        issue = issue_events(ut, uv, tt, tv, xt, xv, u)
        # TS 那几行都是「这一步在 TS 里等了多久」：起点是它进入 TS、可以被下发那一刻，
        # 终点是这一路的 RV core 接下它。终点都一样（rv_start）；起点按主线 / Router
        # 触发的搬入分：MU / VU 只有主线，DTE 两样都有，所以 DTE 拆成两行。
        if u == 0:
            dte_main, dte_datain = dte_spans(load, issue, rv_starts, t_end)
            chain_rows.append(dte_main)
            datain_rows.append(dte_datain)
        else:
            chain_rows.append(step_chain(load, issue, rv_starts, t_end))
            datain_rows.append([])
        # MU / VU 的 DSA 那一行都拆成两段：起点从各家「过门槛」后移到「真正发行进
        # 执行通路」，被切掉的那一截（单元把这一笔收下 → 发行）归 `muisq` / `vuisq`
        # 那一行。发行那对边沿既是新行的终点，也是 DSA 行的新起点，两行首尾相接。
        # MU 收下的是写 trigger 被 regfile 锁成的那一笔，VU 是 config_register 收到
        # 的那条宏指令；DTE（u == 0）没有这一层，仍从过门槛那一拍起算。
        if u == 0:
            dsa_rows.append(dsa)
            vuisq_rows.append([])
            muisq_rows.append([])
            calc_rows.append([])
            continue
        trig_starts, disp_starts = ends_of(TASK_START_EDGE, u)
        isq = user_spans(trig_starts, disp_starts, t_end)
        if u == 1:
            muisq_rows.append(isq)
            vuisq_rows.append([])
        else:
            vuisq_rows.append(isq)
            muisq_rows.append([])
        dsa_rows.append(user_spans(disp_starts, dsa_dones, t_end))
        # MU 另有「这笔任务的 prim 在矩阵执行单元里进出所跨的那一段」。**它不是切分**
        # ——整段落在上面那个 MU-DSA 段里面（起点比它晚、终点比它早），只有 MU 有。
        if u == 1:
            calc_starts, calc_dones = ends_of(CALC_EDGE, u)
            calc_rows.append(user_spans(calc_starts, calc_dones, t_end))
        else:
            calc_rows.append([])
    return {"core": core_rows, "dsa": dsa_rows, "chain": chain_rows,
            "datain": datain_rows, "vuisq": vuisq_rows, "muisq": muisq_rows,
            "calc": calc_rows}


def lanes_of(spans: Dict[str, List[List[List[int]]]]) -> List[List[List[int]]]:
    """core_spans 的产物 → 一个 core 的十三条通道，顺序与 `LANES` 一致。"""
    return [spans[group][u] for _, group, u in LANES]


def missing_signals(reader: TraceReader, core_sig: Dict[str, Dict[str, int]]) -> List[str]:
    """这份波形里没有的新信号（加信号之前生成的波形一个都没有）。"""
    have = set()
    for sigs in core_sig.values():
        have |= set(sigs)
    return [n for n in NEW_SIGS if n not in have]
