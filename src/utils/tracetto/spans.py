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
# 完成了、但**不报 TS** 的那一批（ack_ts_en = 0）：自启动 core 的 Bypass DataIn（链一，
# 保留号 63）与权重加载阶段的 datain。只有 DTE 抬 bit0。与上面那对**互斥** —— 一个完成
# 只走一支，所以两条流不相交，DTE-DSA 的终点直接并起来当一条流用即可。
SIG_DSA_NOACK = "dsa_done_noack"
SIG_DSA_NOACK_TASK = "dsa_done_noack_task"
SIG_DSA_NOACK_USER = "dsa_done_noack_user"
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
# 一笔任务在矩阵执行单元里进出所跨那一段的两端。只有 MU 有（恒抬 bit1）：起点是
# 第一个 tile 进 exe、终点是最后一个 tile 的后一拍，量的是这笔任务的真实 MAC 时间
# （kblock × 专家数 × nblock 拍），不把 10 级流水深度算进去。
SIG_CALC_START = "dsa_calc_start"
SIG_CALC_START_TASK = "dsa_calc_start_task"
SIG_CALC_START_USER = "dsa_calc_start_user"
SIG_CALC_DONE = "dsa_calc_done"
SIG_CALC_DONE_TASK = "dsa_calc_done_task"
SIG_CALC_DONE_USER = "dsa_calc_done_user"
# CALC 起点的尺寸与数据类型（标量，只有 MU 有）：专家数 / K / N（M 恒为 1，不存）、
# K/N 各分几块、原语本身的 K/N、token/weight 的 dtype 码、输出是否 BF16。在 calc_start
# 那一拍与 task / user 一起锁存（mu.h Ctrl::Compute），Perfetto 的 MU-DSA-CALC args
# 靠它们带上 expert_num 与 M×K×N 等 GEMM 规模。
SIG_CALC_START_EXPERT = "dsa_calc_start_expert"
SIG_CALC_START_K = "dsa_calc_start_k"
SIG_CALC_START_N = "dsa_calc_start_n"
SIG_CALC_START_KBLOCK = "dsa_calc_start_kblock"
SIG_CALC_START_NBLOCK = "dsa_calc_start_nblock"
SIG_CALC_START_PRIM_K = "dsa_calc_start_prim_k"
SIG_CALC_START_PRIM_N = "dsa_calc_start_prim_n"
SIG_CALC_START_ADTYPE = "dsa_calc_start_adtype"
SIG_CALC_START_BDTYPE = "dsa_calc_start_bdtype"
SIG_CALC_START_OUTBF16 = "dsa_calc_start_outbf16"

# 三对边沿，每组六条：起点的位掩码 / task / user，终点的位掩码 / task / user。
RV_EDGE = (SIG_RV_START, SIG_RV_START_TASK, SIG_RV_START_USER,
           SIG_RV_DONE, SIG_RV_DONE_TASK, SIG_RV_DONE_USER)
DSA_EDGE = (SIG_DSA_START, SIG_DSA_START_TASK, SIG_DSA_START_USER,
            SIG_DSA_DONE, SIG_DSA_DONE_TASK, SIG_DSA_DONE_USER)
# DTE-DSA 的**另一个**终点：完成了但不报的那一批。它不是一对边沿（起点的另一半），
# 而是与 DSA_EDGE 的后三条**并列**的另一条完成流。
NOACK_EDGE = (SIG_DSA_NOACK, SIG_DSA_NOACK_TASK, SIG_DSA_NOACK_USER)
# MU / VU 那一对：收下 → 真正发行。
TASK_START_EDGE = (SIG_TASK_TRIG, SIG_TASK_TRIG_TASK, SIG_TASK_TRIG_USER,
                   SIG_TASK_DISP, SIG_TASK_DISP_TASK, SIG_TASK_DISP_USER)
# MU 那一对：第一个 tile 进执行单元 → 最后一个 tile 后一拍。**这不是一段的切分，而是
# 嵌在 MU-DSA 里的子区间** —— 画面上它整段落在 MU-DSA 里面。
CALC_EDGE = (SIG_CALC_START, SIG_CALC_START_TASK, SIG_CALC_START_USER,
             SIG_CALC_DONE, SIG_CALC_DONE_TASK, SIG_CALC_DONE_USER)

# 「装进 stream」那两条：也是六条，但是计数器不是位掩码，凑不成一对边沿，单独成组。
STEP_SIGS = (SIG_TS_CREATE, SIG_TS_CREATE_TASK, SIG_TS_CREATE_USER,
             SIG_TS_INSTALL, SIG_TS_INSTALL_TASK, SIG_TS_INSTALL_USER)
# CALC 起点的尺寸与数据类型（标量）。老波形没有，缺了 MU-DSA-CALC 的 args 里就没有尺寸。
CALC_DIM_SIGS = (SIG_CALC_START_EXPERT, SIG_CALC_START_K, SIG_CALC_START_N,
                 SIG_CALC_START_KBLOCK, SIG_CALC_START_NBLOCK,
                 SIG_CALC_START_PRIM_K, SIG_CALC_START_PRIM_N,
                 SIG_CALC_START_ADTYPE, SIG_CALC_START_BDTYPE,
                 SIG_CALC_START_OUTBF16)
# MU 发行那一拍的配置寄存器数（标量，只有 MU 有）。在 dsa_task_dispatch 同一拍锁存，
# Perfetto 的 MU-Core args 靠它带上「这笔任务配置了多少个寄存器」。
SIG_DISP_CFG_COUNT = "dsa_dispatch_cfg_count"

# ── DTE 十条轨道的信号（挂在 chip<i>.core<j>.dte.<regfile|lane<k>> 下，不在 core
#    级的 RE_CORE 里，所以由 dte_signal_paths 单独扫）──
# 写 trigger 那一拍带身份，标量 dsa_trigger_cfg_count 在同一拍锁存「这笔任务配置过
# 多少个寄存器」，Python 侧按 (user, task) 挂到 DTE-Core 那一行的 args。原来还有一条
# 「配置 → trigger」的 dte_rvcore 轨道，已删（dsa_cfg_start 那条信号 C++ 里还照发，
# 只是 Python 不再读它）。
SIG_DTE_TRIGGER = "dsa_trigger"
SIG_DTE_TRIGGER_TASK = "dsa_trigger_task"
SIG_DTE_TRIGGER_USER = "dsa_trigger_user"
# 写 trigger 那一拍的配置寄存器数（标量，与 dsa_trigger 同一拍锁存）。Perfetto 的
# DTE-Core args 靠它带上「这笔任务配置了多少个寄存器」。
SIG_DTE_TRIGGER_CFG_COUNT = "dsa_trigger_cfg_count"
# 每条 lane 拆读/写两条轨道，两端都用这一半自己的「激活 → 做完」，身份取 desc 的
# task_id / user_id：
#   读轨道 = rd_start（RD 半激活）→ rd_done（RD 半读完，数据全回来）
#   写轨道 = wr_start（WR 半激活）→ wr_done（发完最后一拍）
SIG_DTE_RD_START = "rd_start"
SIG_DTE_RD_START_TASK = "rd_start_task"
SIG_DTE_RD_START_USER = "rd_start_user"
SIG_DTE_RD_DONE = "rd_done"
SIG_DTE_RD_DONE_TASK = "rd_done_task"
SIG_DTE_RD_DONE_USER = "rd_done_user"
SIG_DTE_WR_START = "wr_start"
SIG_DTE_WR_START_TASK = "wr_start_task"
SIG_DTE_WR_START_USER = "wr_start_user"
SIG_DTE_WR_DONE = "wr_done"
SIG_DTE_WR_DONE_TASK = "wr_done_task"
SIG_DTE_WR_DONE_USER = "wr_done_user"
# rd_start / wr_start 那一拍的四份内容（head/data/scale/topk）各多少字节。head 是
# 固定 48 B 的包头上下文（段 0 恒参与），写这一半 MM→CM 不落包头记 0。
SIG_DTE_RD_START_HEAD = "rd_start_head"
SIG_DTE_RD_START_DATA = "rd_start_data"
SIG_DTE_RD_START_SCALE = "rd_start_scale"
SIG_DTE_RD_START_TOPK = "rd_start_topk"
SIG_DTE_WR_START_HEAD = "wr_start_head"
SIG_DTE_WR_START_DATA = "wr_start_data"
SIG_DTE_WR_START_SCALE = "wr_start_scale"
SIG_DTE_WR_START_TOPK = "wr_start_topk"
# 同一拍各份内容从哪里读（rd）/写到哪（wr）的位置编码，见 lane.h 的 DteLoc：
# 0 = none，1 = Hmem，2 = Core Mem，3 = Matrix Mem，4 = Router，5 = MU（topK_ep_table）。
SIG_DTE_RD_START_HEAD_LOC = "rd_start_head_loc"
SIG_DTE_RD_START_DATA_LOC = "rd_start_data_loc"
SIG_DTE_RD_START_SCALE_LOC = "rd_start_scale_loc"
SIG_DTE_RD_START_TOPK_LOC = "rd_start_topk_loc"
SIG_DTE_WR_START_HEAD_LOC = "wr_start_head_loc"
SIG_DTE_WR_START_DATA_LOC = "wr_start_data_loc"
SIG_DTE_WR_START_SCALE_LOC = "wr_start_scale_loc"
SIG_DTE_WR_START_TOPK_LOC = "wr_start_topk_loc"

DTE_RD_EDGE = (SIG_DTE_RD_START, SIG_DTE_RD_START_TASK, SIG_DTE_RD_START_USER,
               SIG_DTE_RD_DONE, SIG_DTE_RD_DONE_TASK, SIG_DTE_RD_DONE_USER)
DTE_WR_EDGE = (SIG_DTE_WR_START, SIG_DTE_WR_START_TASK, SIG_DTE_WR_START_USER,
               SIG_DTE_WR_DONE, SIG_DTE_WR_DONE_TASK, SIG_DTE_WR_DONE_USER)
DTE_RD_BYTE = (SIG_DTE_RD_START_HEAD, SIG_DTE_RD_START_DATA,
               SIG_DTE_RD_START_SCALE, SIG_DTE_RD_START_TOPK)
DTE_WR_BYTE = (SIG_DTE_WR_START_HEAD, SIG_DTE_WR_START_DATA,
               SIG_DTE_WR_START_SCALE, SIG_DTE_WR_START_TOPK)
DTE_RD_LOC = (SIG_DTE_RD_START_HEAD_LOC, SIG_DTE_RD_START_DATA_LOC,
              SIG_DTE_RD_START_SCALE_LOC, SIG_DTE_RD_START_TOPK_LOC)
DTE_WR_LOC = (SIG_DTE_WR_START_HEAD_LOC, SIG_DTE_WR_START_DATA_LOC,
              SIG_DTE_WR_START_SCALE_LOC, SIG_DTE_WR_START_TOPK_LOC)

READ_SIGS = ((SIG_TS_UNIT, SIG_TS_TASK, SIG_TS_USER, SIG_TS_DONE) + RV_EDGE +
             DSA_EDGE + TASK_START_EDGE + STEP_SIGS + CALC_EDGE + NOACK_EDGE +
             CALC_DIM_SIGS + (SIG_DISP_CFG_COUNT,))
# 2026-09 之后加的：比这更早的波形里一个都没有，认出来好把话说清楚。末尾那些也在
# 这里 —— TS-MU / TS-VU 与 MU-DSA-CALC 现在要读它们，老波形缺了那些行就是空的。
NEW_SIGS = ((SIG_TS_USER,) + RV_EDGE + DSA_EDGE + TASK_START_EDGE + STEP_SIGS +
            CALC_EDGE + NOACK_EDGE + CALC_DIM_SIGS + (SIG_DISP_CFG_COUNT,))

# 索引里一个 core 的十三条通道：(通道名, core_spans 里的那一组, 单元在位掩码里的位序)。
# 顺序就是通道号的顺序 —— 定死，别改（改了索引与前端都要跟着动）。新加的通道一律
# 追加在末尾，前面十二条的通道号才不动（旧波形、旧断言都按号认）。注意**通道号与行号
# 不是一回事**：行序是纯呈现层，见下面的 ROWS。十三条通道都有对应的行。
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

# 画面上一个 core 的十三行：(显示名, ((通道号, 颜色号), ...))。颜色一共十三种，
# 每条通道一个颜色槽，槽表 SLOT_UNITS 按颜色号索引。
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
    # tile 在矩阵执行单元里进出所跨的那一段（真实 MAC 时间），整段落在 MU-DSA 里面。
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
# 的 DTE 段与 DTE-Core 那一行的段都叫 `… DTE …`，于是同色；带上行名之后十三个槽
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
RE_DTE_REGFILE = re.compile(r"^chip(\d+)\.core(\d+)\.dte\.regfile\.(\w+)$")
RE_DTE_LANE = re.compile(r"^chip(\d+)\.core(\d+)\.dte\.lane(\d+)\.(\w+)$")


def val_at(ts: list, vs: list, t: int) -> int:
    """取不晚于 t 的最后一笔的值。波形开头之前一律当 0。"""
    if not ts:
        return 0
    i = bisect.bisect_right(ts, t) - 1
    return vs[i] if i >= 0 else 0


def issue_events(unit_ts: list, unit_vs: list, task_ts: list, task_vs: list,
                 user_ts: list, user_vs: list, bit: int) -> List[dict]:
    """某一位上的事件，带上那一拍该路的 task 与 user。

    `ts_unit` 与两条打包信号都是“本拍这一路有没有”的形状，所以下发、RV 起、RV 完、
    DSA 起、DSA 完这五种事件都走这一个函数。

    这些位掩码是**一拍一笔的脉冲**（各家 `EmitXxx` 里每拍从 0 重新算、只在笔数变了的
    那一拍置位）：同一路连着两拍都有完成时，掩码两拍同值（都是 1）、波形上只有一段，
    按 0→1 边沿只数得到第一笔，第二笔会被并进上一笔，段就一直延到波形末。身份信号
    task / user 每拍跟着变，把它们的时间点并进来就能把这一段展开成一拍一笔
    （moe_lpu_tokens 上 DTE-DSA 那 2032 段正是这么漏的）。
    """
    out: List[dict] = []
    times = sorted(set(unit_ts) | set(task_ts) | set(user_ts))
    for t in times:
        if (val_at(unit_ts, unit_vs, t) >> bit) & 1:
            out.append({
                "t": t,
                "task": (val_at(task_ts, task_vs, t) >> (8 * bit)) & 0xFF,
                "user": (val_at(user_ts, user_vs, t) >> (16 * bit)) & 0xFFFF,
            })
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
        # DTE 那一路的 DSA 有**两个**终点：`dsa_done`（报给 TS 的）与 `dsa_done_noack`
        # （完成了但不报的，链一/自启动 Bypass 走这条）。两者互斥，所以按时间并成一条
        # 完成流就完事 —— 不并的话那 1279 笔只能延到波形末。
        if u == 0:
            na = issue_events(*ev(SIG_DSA_NOACK), *ev(SIG_DSA_NOACK_TASK),
                              *ev(SIG_DSA_NOACK_USER), 0)
            dsa_dones = sorted(dsa_dones + na, key=lambda e: e["t"])
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


def calc_dims(reader: TraceReader, sigs: Dict[str, int]) -> Dict[int, Dict[str, int]]:
    """每个 CALC 起点那一拍的尺寸字典，键是起点时间。M 恒为 1，不存。

    这些是标量信号：只在某笔任务 calc_start（mu.h Ctrl::Compute 的 computed == 0）
    那一拍锁存一次，所以起点那拍 `val_at` 取到的正好是这笔的值；相邻两笔尺寸相同也
    不会再记一笔，`val_at` 仍取到上一次的值。老波形缺这几条时返回空 dict。
    """
    out: Dict[int, Dict[str, int]] = {}

    def ev(name: str) -> Tuple[list, list]:
        sid = sigs.get(name)
        if sid is None:
            return [], []
        return reader.events(sid)

    st, sv = ev(SIG_CALC_START)
    fields = {
        "expert": ev(SIG_CALC_START_EXPERT),
        "k": ev(SIG_CALC_START_K),
        "n": ev(SIG_CALC_START_N),
        "kblock": ev(SIG_CALC_START_KBLOCK),
        "nblock": ev(SIG_CALC_START_NBLOCK),
        "prim_k": ev(SIG_CALC_START_PRIM_K),
        "prim_n": ev(SIG_CALC_START_PRIM_N),
        "a_dtype": ev(SIG_CALC_START_ADTYPE),
        "b_dtype": ev(SIG_CALC_START_BDTYPE),
        "out_bf16": ev(SIG_CALC_START_OUTBF16),
    }
    if any(not ts for ts, _ in fields.values()):
        return out
    prev = 0
    for t, v in zip(st, sv):
        on = (v >> 1) & 1          # bit1 = MU
        if on and not prev:
            out[t] = {name: val_at(ts, vs, t) for name, (ts, vs) in fields.items()}
        prev = on
    return out


def mu_cfg_count(reader: TraceReader, sigs: Dict[str, int]) -> Dict[Tuple[int, int], int]:
    """每个 MU 任务配置过的寄存器数，键是 (user, task)。发行那一拍（dsa_task_dispatch
    bit1 = MU）带身份，标量 `dsa_dispatch_cfg_count` 在同一拍锁存。键按 (user, task)
    而不是时间 —— Perfetto 里它挂到 MU-Core 那一行（rv_start → rv_done），那一段的
    起点不是发行拍，只能靠 user/task 对上。老波形缺这条时返回空 dict。
    """
    out: Dict[Tuple[int, int], int] = {}
    sid = sigs.get(SIG_DISP_CFG_COUNT)
    disp_sid = sigs.get(SIG_TASK_DISP)
    disp_task_sid = sigs.get(SIG_TASK_DISP_TASK)
    disp_user_sid = sigs.get(SIG_TASK_DISP_USER)
    if None in (sid, disp_sid, disp_task_sid, disp_user_sid):
        return out
    cts, cvs = reader.events(sid)
    disp = issue_events(*reader.events(disp_sid), *reader.events(disp_task_sid),
                        *reader.events(disp_user_sid), 1)          # bit1 = MU
    for e in disp:
        out[(e["user"], e["task"])] = val_at(cts, cvs, e["t"])
    return out


def dte_cfg_count(reader: TraceReader, sigs: Dict) -> Dict[Tuple[int, int], int]:
    """每个 DTE 任务配置过的寄存器数，键是 (user, task)。写 trigger 那一拍带身份，
    标量 `dsa_trigger_cfg_count` 在同一拍锁存。`sigs` 是 `dte_signal_paths()` 里一个
    core 的整份（`regfile` + `lane`），这里只用 `regfile` 那张。老波形缺这条时返回空
    dict。
    """
    out: Dict[Tuple[int, int], int] = {}
    flat = sigs.get("regfile", {})
    sid = flat.get(SIG_DTE_TRIGGER_CFG_COUNT)
    trig_sid = flat.get(SIG_DTE_TRIGGER)
    trig_task_sid = flat.get(SIG_DTE_TRIGGER_TASK)
    trig_user_sid = flat.get(SIG_DTE_TRIGGER_USER)
    if None in (sid, trig_sid, trig_task_sid, trig_user_sid):
        return out
    cts, cvs = reader.events(sid)
    trig = issue_events(*reader.events(trig_sid), *reader.events(trig_task_sid),
                        *reader.events(trig_user_sid), 0)          # dsa_trigger 单 bit
    for e in trig:
        out[(e["user"], e["task"])] = val_at(cts, cvs, e["t"])
    return out


def lane_user_spans(starts: List[dict], dones: List[dict],
                    t_end: int) -> List[List[int]]:
    """`user_spans` 的 lane 版：段多带起点那一拍四份内容（head/data/scale/topk）的
    字节数与位置，形状
    `[t0, t1, user, task, head, data, scale, topk, head_loc, data_loc, scale_loc,
    topk_loc]`。配对规则同 `user_spans`。

    `*_loc` 是这份内容从哪里读（rd）/写到哪（wr）的位置编码（见 lane.h 的 DteLoc）：
    0 = none（这一半不搬这份内容，长度 0），1 = Hmem，2 = Core Mem，3 = Matrix Mem，
    4 = Router，5 = MU（topK_ep_table）。
    """
    def content(e) -> List[int]:
        return [e.get(n, 0) for n in
                ("head", "data", "scale", "topk",
                 "head_loc", "data_loc", "scale_loc", "topk_loc")]

    ev = [(e["t"], 1, e) for e in starts] + [(e["t"], 0, e) for e in dones]
    ev.sort(key=lambda x: (x[0], x[1]))
    pending: Dict[Tuple[int, int], List[dict]] = {}
    out: List[List[int]] = []
    for t, kind, e in ev:
        key = (e["user"], e["task"])
        if kind == 1:
            pending.setdefault(key, []).append(e)
            continue
        q = pending.get(key)
        if not q:
            out.append([t, t + 1, _user_tag(e["user"]), _task_tag(e["task"])]
                       + content(e))
            continue
        s0 = q.pop(0)
        out.append([s0["t"], t, _user_tag(e["user"]), _task_tag(e["task"])]
                   + content(s0))
    for (user, task), evs in pending.items():
        for s0 in evs:
            out.append([s0["t"], t_end, _user_tag(user), _task_tag(task)]
                       + content(s0))
    out.sort(key=lambda s: (s[0], s[1]))
    return out


def dte_signal_paths(reader: TraceReader) -> Dict[str, Dict]:
    """扫模块树，找出 DTE 十条轨道要的信号。

    返回 `{"chip.core": {"regfile": {sig: id}, "lane": [{sig: id} × 5]}}`。DTE 每个
    core 都有这一组信号，但没派角色的 core 一整场都不动、信号全是常数，读回来也是
    空；路径这里仍收进来，有没有段由调用方折段时看。
    """
    paths = reader.tree_paths()
    out: Dict[str, Dict] = {}
    for sig_id in reader.signals():
        name = paths.get(sig_id)
        if not name:
            continue
        m = RE_DTE_REGFILE.match(name)
        if m:
            c, k, leaf = int(m.group(1)), int(m.group(2)), m.group(3)
            key = f"{c}.{k}"
            out.setdefault(key, {"regfile": {}, "lane": [{} for _ in range(5)]})
            out[key]["regfile"][leaf] = sig_id
            continue
        m = RE_DTE_LANE.match(name)
        if m:
            c, k, ln, leaf = (int(m.group(1)), int(m.group(2)),
                              int(m.group(3)), m.group(4))
            key = f"{c}.{k}"
            out.setdefault(key, {"regfile": {}, "lane": [{} for _ in range(5)]})
            out[key]["lane"][ln][leaf] = sig_id
    return out


def dte_lane_spans(reader: TraceReader, sigs: Dict, t_end: int):
    """一个 core 的 DTE 十条轨道。`sigs` 是 `dte_signal_paths()` 里那一个 core 的值。

    返回 `([读段 × 5], [写段 × 5])`。每条 lane 拆读/写两条（读 = rd_start → rd_done，
    写 = wr_start → wr_done），段形状
    `[t0, t1, user, task, head, data, scale, topk, head_loc, data_loc, scale_loc,
    topk_loc]`，后面八项是起点那一拍四份内容的字节数与位置（从哪读/写到哪）。原来
    还有一条「配置 → trigger」的 dte_rvcore 轨道，已删；配置寄存器数改挂到 DTE-Core
    那一行的 args（见 `dte_cfg_count`）。
    """

    def ev(sid):
        if sid is None:
            return [], []
        return reader.events(sid)

    def ends(group: Tuple[str, ...], flat: Dict, bit: int = 0):
        starts = issue_events(*ev(flat.get(group[0])), *ev(flat.get(group[1])),
                              *ev(flat.get(group[2])), bit)
        dones = issue_events(*ev(flat.get(group[3])), *ev(flat.get(group[4])),
                             *ev(flat.get(group[5])), bit)
        return starts, dones

    def ends_bytes(group: Tuple[str, ...], flat: Dict,
                   byte_group: Tuple[str, ...], loc_group: Tuple[str, ...]):
        starts, dones = ends(group, flat)
        for name, bname, lname in zip(
                ("head", "data", "scale", "topk"), byte_group, loc_group):
            bts, bvs = ev(flat.get(bname))
            lts, lvs = ev(flat.get(lname))
            for s in starts:
                s[name] = val_at(bts, bvs, s["t"])
                s[name + "_loc"] = val_at(lts, lvs, s["t"])
        return starts, dones

    rd_lanes, wr_lanes = [], []
    for k in range(5):
        rd_starts, rd_dones = ends_bytes(DTE_RD_EDGE, sigs["lane"][k],
                                         DTE_RD_BYTE, DTE_RD_LOC)
        wr_starts, wr_dones = ends_bytes(DTE_WR_EDGE, sigs["lane"][k],
                                         DTE_WR_BYTE, DTE_WR_LOC)
        rd_lanes.append(lane_user_spans(rd_starts, rd_dones, t_end))
        wr_lanes.append(lane_user_spans(wr_starts, wr_dones, t_end))
    return rd_lanes, wr_lanes


def missing_signals(reader: TraceReader, core_sig: Dict[str, Dict[str, int]]) -> List[str]:
    """这份波形里没有的新信号（加信号之前生成的波形一个都没有）。"""
    have = set()
    for sigs in core_sig.values():
        have |= set(sigs)
    return [n for n in NEW_SIGS if n not in have]
