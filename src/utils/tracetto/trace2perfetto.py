#!/usr/bin/env python3
"""把 latch 的 .trace 转成 Perfetto（Chrome Trace Event Format）的 JSON。

    python3 src/utils/tracetto/trace2perfetto.py moe_lpu

产物落在波形旁边：`<波形名>.perfetto.json`，直接拖进 ui.perfetto.dev 或
chrome://tracing。

**与 tracetto 显示的是同一份数据**：段与标签都走 `spans.py` 里那套（`core_spans`
折段、`ROWS` 那十三行、每行由哪几条通道叠出来），所以看到的是同样的区间、同样的 `User_id 77 CORE-MU 5 813拍`。

两边的对应关系（Perfetto 只有“process → thread”两级，tracetto 是
“chip → core → 十三行”三级，压掉最上面一级）：

    pid  = 一个派角色的 core，名字写成 `chip0.core1`，左栏能直接搜
    tid  = 那条 core 的十三行之一，名字就是行名（TS-DTE / DTE-Core / …）
    X 事件 = 一个段，`User_id 77 CORE-MU 5 813拍`

**顺序**：进程按 chip 升序、再按 core 升序，每个进程里十三行按 tracetto 的行序。
这两层各自再发一条排序键（`process_sort_index` / `thread_sort_index`）钉死 ——
**Perfetto 是按名字排轨道的，不按 pid / tid**（实测：不发键时 chip10 跑到 chip2
前面、TS 掉到第五行），而名字的字典序本来就排不对。试过把序号写进名字里（`chip00`
/ `1 TS`）来代替这两组键，也不行，所以键留着（258 KB）。

**颜色**：Perfetto 是**按段上的名字上色**的，所以“哪一行”必须写进那段字里 ——
单元名带前缀（`TS-DTE` / `CORE-DTE` / `DSA-DTE` …），十三个槽的名字两两不同，十三行
才分得开。不给 cname（只能填十来个具名色，里面近似的不少）、也不给 cat（不参与
上色）。

时间：**1 拍 = 1 ns**。Perfetto 的 JSON 里时间戳单位固定是微秒，没有别的选择，
所以这里的 ts / dur 要把拍号乘 1/1000 折成微秒（1 ns = 0.001 µs），Perfetto 里
读到的才是纳秒。

精简（都是实测省下来的）：
  · 字段只留 name/ph/ts/dur/pid/tid 六样，外加 args 里 user/task 两个键值（段名里
    已有，这里作为结构化字段，点选 slice 时在详情面板看）；MU-Core 那段多一个
    cfg_count（这一笔任务配置了多少个寄存器），MU-DSA-CALC 那段多 expert_num / M /
    K / N / block_k / block_n / prim_k / prim_N / a_dtype / b_dtype / out_dtype
    十一个键值（GEMM 规模：全尺寸、K/N 分块、原语尺寸、三处数据类型），DTE 那十
    条轨道多 head/data/scale/topk 各多少字节，以及各份内容从哪读（rd 轨道，`*_rd`）
    /写到哪（wr 轨道，`*_wr`）：hmem / cmem / mmem / router / mu，这一半不搬的
    内容长度 0、位置 none；
  · 不发 cat（140 KB）：它不参与上色（颜色按段上的名字走），只对“按分类过滤”
    有用，而这个转换器给不出有意义的分类；
  · 分隔符用最紧的写法，一行一个事件（方便 grep，代价 ~1 B/行）。时间按 1 拍 = 1 ns
    折成微秒，所以 ts / dur 是带小数的微秒（1.319 µs = 1319 ns）。

要更小就把整份 gzip：实测 1.31 MB → ~89 KB（它太重复了 —— 九千多个段只有 72 种
不同的名字，408 个 core 的元数据块字面一样）。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import List

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from tracetto import spans as S          # noqa: E402
from tracetto.reader import TraceReader   # noqa: E402

# 段上印的单元名与 tracetto 共用一张表（spans.SLOT_UNITS）：每个颜色槽一个名字，
# 带着行前缀（TS-DTE / CORE-DTE / DSA-DTE …）。Perfetto 按段上的名字上色，十三个槽
# 名字两两不同，十三行才分得开 —— 原来 TS 行的 DTE 段与 DTE-Core 行的段都叫
# `User_id x DTE y`，就撞成了一个色。


# DataType 码 → 名字（bach/common/numeric/mx.h 的 DataType 枚举）。a_dtype / b_dtype
# 在波形里存的是这个码，args 里转成人能读的名字。
DTYPE_NAME = {0: "BF16", 1: "MXFP8", 2: "MXFP4", 3: "NVFP4", 4: "FP32"}

# DTE 十条轨道（lan0~4 各拆读/写）。挂在 chip.core.dte.<regfile|lane<k>>
# 下，段由 spans.dte_lane_spans 折好；它们与上面那十三行同属一个 core 进程，排在十三行
# 之后。原来的 dte_rvcore 轨道已删，配置寄存器数改挂 DTE-Core 那一行的 args。
DTE_TRACKS = ("lan0_rd", "lan0_wr", "lan1_rd", "lan1_wr",
              "lan2_rd", "lan2_wr", "lan3_rd", "lan3_wr",
              "lan4_rd", "lan4_wr")
# 段上印的单元名，与 DTE_TRACKS 一一对应。Perfetto 按名字上色，这十个名字两两不同。
DTE_SLOT_UNITS = ("DTE-L0-RD", "DTE-L0-WR", "DTE-L1-RD", "DTE-L1-WR",
                  "DTE-L2-RD", "DTE-L2-WR", "DTE-L3-RD", "DTE-L3-WR",
                  "DTE-L4-RD", "DTE-L4-WR")

# 时间：1 拍 = 1 ns。Perfetto 的 ts / dur 单位固定是微秒，1 ns = 0.001 µs，所以把
# 拍号乘这个系数折成微秒，Perfetto 里读到的才是纳秒。
NS_TO_US = 1e-3


def label_of(unit: str, seg) -> str:
    """段上那行字，与 tracetto 逐字一致：`User_id 77 CORE-DTE 5 813拍`。
    单元名里带着行号（TS- / CORE- / DSA-），Perfetto 按名字上色，靠它把十三行分开。"""
    t0, t1, user, task = seg
    if user < 0 or task < 0:
        return "?"
    return f"User_id {user} {unit} {task} {t1 - t0}拍"


def dte_label(unit: str, seg) -> str:
    """DTE 十条轨道的段上那行字，格式同 `label_of`：`User_id 1 DTE-L0-RD 2 7拍`。
    lane 段是 8 元，这里只取前四项。"""
    t0, t1, user, task = seg[0], seg[1], seg[2], seg[3]
    if user < 0 or task < 0:
        return "?"
    return f"User_id {user} {unit} {task} {t1 - t0}拍"


# DteLoc 位置编码 → 名字（与 lane.h 的 DteLoc 一致）：0 = none（这一半不搬这份内容，
# 长度 0），1 = Hmem，2 = Core Mem，3 = Matrix Mem，4 = Router，5 = MU（topK_ep_table）。
DTE_LOC_NAME = {0: "none", 1: "hmem", 2: "cmem", 3: "mmem", 4: "router", 5: "mu"}


def lane_args(seg, suffix: str) -> dict:
    """lane 轨道的 args：head/data/scale/topk 各多少字节、各从哪读（`suffix="rd"`）/
    写到哪（`suffix="wr"`）。位置编码经 DTE_LOC_NAME 转成 hmem/cmem/mem/router/mu 之
    类；这一半不搬的内容长度 0、位置 none。`bytes` 是 data+scale+topk 的 payload 字节
    数，不含包头上下文（与包 size 一致）。"""
    _t0, _t1, user, task, head, data, scale, topk, \
        head_loc, data_loc, scale_loc, topk_loc = seg
    loc = lambda c: DTE_LOC_NAME.get(c, "none")
    return {
        "user": user, "task": task,
        "bytes": data + scale + topk,
        "head": head, f"head_{suffix}": loc(head_loc),
        "data": data, f"data_{suffix}": loc(data_loc),
        "scale": scale, f"scale_{suffix}": loc(scale_loc),
        "topk": topk, f"topk_{suffix}": loc(topk_loc),
    }


def build(prefix: str) -> dict:
    out = {"traceEvents": []}
    meta = out["traceEvents"]
    slices: List[dict] = []

    with TraceReader(prefix) as r:
        chips, core_sig = S.core_paths(r)
        t_end = S.trace_t_end(r, core_sig)

        # 与索引那边同一个顺序：按 chip 升序，再按 core 升序。
        ordered = []
        for chip in sorted(chips):
            for core in sorted(chips[chip]):
                key = f"{chip}.{core}"
                if key in core_sig:
                    ordered.append((chip, core, key))

        # DTE 十条轨道的段先折好：没段（DTE 没干活）的 core 不发那十条线程。
        dte_sigs = S.dte_signal_paths(r)
        dte_spans_by_core = {}
        dte_cfg_by_core = {}
        for _chip, _core, key in ordered:
            if key not in dte_sigs:
                continue
            rd_lanes, wr_lanes = S.dte_lane_spans(r, dte_sigs[key], t_end)
            if any(rd_lanes) or any(wr_lanes):
                dte_spans_by_core[key] = (rd_lanes, wr_lanes)
            # DTE-Core 那一行的 args 要带配置寄存器数，按 (user, task) 对上 trigger 拍。
            dte_cfg_by_core[key] = S.dte_cfg_count(r, dte_sigs[key])

        # 元数据：每个 core 一个进程名，十三行各一条 thread_name。
        #
        # 顺序靠这两组排序键钉死：**Perfetto 是按名字排轨道的，不按 pid / tid**
        # （实测：不给键时 chip10 跑到 chip2 前面、TS 掉到第五行）。而名字的字典序
        # 本来就排不对，所以这两组键不能省 —— 试过把序号写进名字里，也不行。
        for ordinal, (chip, core, key) in enumerate(ordered):
            pid = ordinal + 1
            meta.append({"ph": "M", "pid": pid, "tid": 0, "name": "process_name",
                         "args": {"name": f"chip{chip}.core{core}"}})
            meta.append({"ph": "M", "pid": pid, "name": "process_sort_index",
                         "args": {"sort_index": ordinal}})
            for row, row_name in enumerate(S.ROW_NAMES):
                # 行号从 1 编：tid=0 在 Chrome 这套格式里是“进程级”那条轨道，
                # Perfetto 会把 tid=0 的段并进进程那一行，行名就不成一条独立轨道了。
                tid = row + 1
                meta.append({"ph": "M", "pid": pid, "tid": tid,
                             "name": "thread_name", "args": {"name": row_name}})
                meta.append({"ph": "M", "pid": pid, "tid": tid,
                             "name": "thread_sort_index",
                             "args": {"sort_index": tid}})
            # DTE 十条轨道作为本 core 进程的十条线程，排在十三行之后。
            if key in dte_spans_by_core:
                for t, tname in enumerate(DTE_TRACKS):
                    tid = len(S.ROWS) + 1 + t
                    meta.append({"ph": "M", "pid": pid, "tid": tid,
                                 "name": "thread_name", "args": {"name": tname}})
                    meta.append({"ph": "M", "pid": pid, "tid": tid,
                                 "name": "thread_sort_index",
                                 "args": {"sort_index": tid}})

        # 段按 chip → core → 行 分组着写：文件里的顺序就是画面上从左到右的顺序，
        # 想按 core 找一段也能直接 grep。组内按时间升序。
        #
        # 整场没跑过的行不补占位：Perfetto 只画有事件的轨道，那些行会整条不出现
        # （moe_lpu 上 2856 条轨道里有 108 条），这一点与 tracetto 的固定十三行不同。
        spans_cnt = 0
        for ordinal, (_chip, _core, key) in enumerate(ordered):
            pid = ordinal + 1
            lanes = S.lanes_of(S.core_spans(r, core_sig[key], t_end))
            # MU-DSA-CALC 那一段的 GEMM 尺寸：按 calc_start 那拍取，键是起点时间。
            # 只有 MU 有，M 恒为 1（见 spans.calc_dims）。老波形缺这几条时是空 dict。
            dims = S.calc_dims(r, core_sig[key])
            # MU-Core 那一段的配置寄存器数：按 (user, task) 对上发行拍，键是身份。
            mucfg = S.mu_cfg_count(r, core_sig[key])
            dtecfg = dte_cfg_by_core.get(key, {})
            for row, (_name, parts) in enumerate(S.ROWS):
                tid = row + 1
                row_events: List[dict] = []
                for lane, slot in parts:
                    unit = S.SLOT_UNITS[slot % len(S.SLOT_UNITS)]
                    for seg in lanes[lane]:
                        t0, t1, user, task = seg
                        args = {"user": user, "task": task}
                        if _name == "MU-Core":
                            c = mucfg.get((user, task))
                            if c is not None:
                                args["cfg_count"] = c
                        if _name == "DTE-Core":
                            c = dtecfg.get((user, task))
                            if c is not None:
                                args["cfg_count"] = c
                        if _name == "MU-DSA-CALC" and t0 in dims:
                            d = dims[t0]
                            args.update({
                                "expert_num": d["expert"], "M": 1,
                                "K": d["k"], "N": d["n"],
                                "block_k": d["kblock"], "block_n": d["nblock"],
                                "prim_k": d["prim_k"], "prim_N": d["prim_n"],
                                "a_dtype": DTYPE_NAME.get(d["a_dtype"], "?"),
                                "b_dtype": DTYPE_NAME.get(d["b_dtype"], "?"),
                                "out_dtype": "BF16" if d["out_bf16"] else "FP32",
                            })
                        ev = {"name": label_of(unit, seg), "ph": "X",
                              "ts": t0 * NS_TO_US, "dur": (t1 - t0) * NS_TO_US,
                              "pid": pid, "tid": tid,
                              "args": args}
                        row_events.append(ev)
                        spans_cnt += 1
                row_events.sort(key=lambda e: e["ts"])
                slices.extend(row_events)
            # DTE 十条轨道：lan0~4 各拆读/写。轨道号按 DTE_TRACKS 的交错顺序
            # （lan0_rd, lan0_wr, …），段要按同一顺序排，否则挂到错的轨道名上。
            if key in dte_spans_by_core:
                rd_lanes, wr_lanes = dte_spans_by_core[key]
                dte_events: List[dict] = []
                lane_segs = []
                for ln in range(5):
                    lane_segs.append(rd_lanes[ln])
                    lane_segs.append(wr_lanes[ln])
                for t, segs in enumerate(lane_segs):
                    tid = len(S.ROWS) + 1 + t
                    for seg in segs:
                        t0, t1 = seg[0], seg[1]
                        args = lane_args(seg, "rd" if t % 2 == 0 else "wr")
                        dte_events.append({
                            "name": dte_label(DTE_SLOT_UNITS[t], seg),
                            "ph": "X", "ts": t0 * NS_TO_US,
                            "dur": (t1 - t0) * NS_TO_US,
                            "pid": pid, "tid": tid, "args": args})
                        spans_cnt += 1
                dte_events.sort(key=lambda e: e["ts"])
                slices.extend(dte_events)

    out["traceEvents"] = meta + slices
    return out, len(ordered), spans_cnt


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
        print(f"[trace2perfetto] {n_core} 个 core / {n_span} 段 → {path}（{size} B）")
        print("   1 拍 = 1 ns；拖进 https://ui.perfetto.dev")
    return 0


def path_of(trace: Path) -> Path:
    """产物落在波形旁边：`<波形名>.perfetto.json`。

    不用 with_suffix：它只收一个点开头、且不许再含点的后缀。
    """
    return Path(str(trace.with_suffix("")) + ".perfetto.json")


if __name__ == "__main__":
    sys.exit(main())
