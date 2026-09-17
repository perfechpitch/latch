#!/usr/bin/env python3
"""把 latch 的 .trace 转成 Perfetto（Chrome Trace Event Format）的 JSON。

    python3 src/utils/tracetto/trace2perfetto.py moe_lpu

产物落在波形旁边：`<波形名>.perfetto.json`，直接拖进 ui.perfetto.dev 或
chrome://tracing。

**与 tracetto 显示的是同一份数据**：段与标签都走 `spans.py` 里那套（`core_spans`
折段、`ROWS` 那七行、每行由哪几条通道叠出来），所以看到的是同样的区间、同样的 `User_id 77 CORE-MU 5 813拍`。

两边的对应关系（Perfetto 只有「process → thread」两级，tracetto 是
「chip → core → 七行」三级，压掉最上面一级）：

    pid  = 一个配了任务的 core，名字写成 `chip0.core1`，左栏能直接搜
    tid  = 那条 core 的七行之一，名字就是行名（TS / DTE-Core / …）
    X 事件 = 一个段，`User_id 77 CORE-MU 5 813拍`

**顺序**：进程按 chip 升序、再按 core 升序，每个进程里七行按 tracetto 的行序。
这两层各自再发一条排序键（`process_sort_index` / `thread_sort_index`）钉死 ——
**Perfetto 是按名字排轨道的，不按 pid / tid**（实测：不发键时 chip10 跑到 chip2
前面、TS 掉到第五行），而名字的字典序本来就排不对。试过把序号写进名字里（`chip00`
/ `1 TS`）来代替这两组键，也不行，所以键留着（258 KB）。

**颜色**：Perfetto 是**按段上的名字上色**的，所以「哪一行」必须写进那段字里 ——
单元名带前缀（`TS-DTE` / `CORE-DTE` / `DSA-DTE` …），九个槽的名字两两不同，九行
才分得开。不给 cname（只能填十来个具名色，里面近似的不少）、也不给 cat（不参与
上色）。

时间：**1 拍 = 1 µs**。Perfetto 的 JSON 里时间戳单位固定是微秒，没有别的选择，
所以这里的 ts / dur 就是拍号本身，读数的时候把 ms 看成千拍。

精简（都是实测省下来的）：
  · 不写 args —— Perfetto 自己会显示起止与时长，再塞一份 user/task 是白给；
  · 字段只留 name/ph/ts/dur/pid/tid 六样；
  · 不发 cat（140 KB）：它不参与上色（颜色按段上的名字走），只对「按分类过滤」
    有用，而这个转换器给不出有意义的分类；
  · 分隔符用最紧的写法，整数不写小数，一行一个事件（方便 grep，代价 ~1 B/行）。

要更小就把整份 gzip：实测 1.24 MB → ~89 KB（它太重复了 —— 八千多个段只有 113 种
不同的名字，402 个 core 的元数据块字面一样）。
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
# 带着行前缀（TS-DTE / CORE-DTE / DSA-DTE …）。Perfetto 按段上的名字上色，九个槽
# 名字两两不同，九行才分得开 —— 原来 TS 行的 DTE 段与 DTE-Core 行的段都叫
# `User_id x DTE y`，就撞成了一个色。


def label_of(unit: str, seg) -> str:
    """段上那行字，与 tracetto 逐字一致：`User_id 77 CORE-DTE 5 813拍`。
    单元名里带着行号（TS- / CORE- / DSA-），Perfetto 按名字上色，靠它把九行分开。"""
    t0, t1, user, task = seg
    if user < 0 or task < 0:
        return "?"
    return f"User_id {user} {unit} {task} {t1 - t0}拍"


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

        # 元数据：每个 core 一个进程名，七行各一条 thread_name。
        #
        # 顺序靠这两组排序键钉死：**Perfetto 是按名字排轨道的，不按 pid / tid**
        # （实测：不给键时 chip10 跑到 chip2 前面、TS 掉到第五行）。而名字的字典序
        # 本来就排不对，所以这两组键不能省 —— 试过把序号写进名字里，也不行。
        for ordinal, (chip, core, _key) in enumerate(ordered):
            pid = ordinal + 1
            meta.append({"ph": "M", "pid": pid, "tid": 0, "name": "process_name",
                         "args": {"name": f"chip{chip}.core{core}"}})
            meta.append({"ph": "M", "pid": pid, "name": "process_sort_index",
                         "args": {"sort_index": ordinal}})
            for row, row_name in enumerate(S.ROW_NAMES):
                # 行号从 1 编：tid=0 在 Chrome 这套格式里是「进程级」那条轨道，
                # Perfetto 会把 tid=0 的段并进进程那一行，行名就不成一条独立轨道了。
                tid = row + 1
                meta.append({"ph": "M", "pid": pid, "tid": tid,
                             "name": "thread_name", "args": {"name": row_name}})
                meta.append({"ph": "M", "pid": pid, "tid": tid,
                             "name": "thread_sort_index",
                             "args": {"sort_index": tid}})

        # 段按 chip → core → 行 分组着写：文件里的顺序就是画面上从左到右的顺序，
        # 想按 core 找一段也能直接 grep。组内按时间升序。
        #
        # 整场没跑过的行不补占位：Perfetto 只画有事件的轨道，那些行会整条不出现
        # （moe_lpu 上 2856 条轨道里有 108 条），这一点与 tracetto 的固定七行不同。
        spans_cnt = 0
        for ordinal, (_chip, _core, key) in enumerate(ordered):
            pid = ordinal + 1
            lanes = S.lanes_of(S.core_spans(r, core_sig[key], t_end))
            for row, (_name, parts) in enumerate(S.ROWS):
                tid = row + 1
                row_events: List[dict] = []
                for lane, slot in parts:
                    unit = S.SLOT_UNITS[slot % len(S.SLOT_UNITS)]
                    for seg in lanes[lane]:
                        t0, t1, _user, _task = seg
                        ev = {"name": label_of(unit, seg), "ph": "X",
                              "ts": t0, "dur": t1 - t0, "pid": pid, "tid": tid}
                        row_events.append(ev)
                        spans_cnt += 1
                row_events.sort(key=lambda e: e["ts"])
                slices.extend(row_events)

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
        print("   1 拍 = 1 µs；拖进 https://ui.perfetto.dev")
    return 0


def path_of(trace: Path) -> Path:
    """产物落在波形旁边：`<波形名>.perfetto.json`。

    不用 with_suffix：它只收一个点开头、且不许再含点的后缀。
    """
    return Path(str(trace.with_suffix("")) + ".perfetto.json")


if __name__ == "__main__":
    sys.exit(main())
