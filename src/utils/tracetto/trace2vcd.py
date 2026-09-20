#!/usr/bin/env python3
"""把 latch 的 .trace 转成 VCD，给 GTKWave / Surfer / Verdi 这类波形工具看。

    python3 src/utils/tracetto/trace2vcd.py moe_lpu

产物落在波形旁边：`<波形名>.vcd`，直接拖进 GTKWave 就能看。

写法照 SystemC 的 sc_trace：`$scope`/`$upscope` 摆出模块层次，`$var` 一条信号一行，
数据区每个时间点一条 `#t`、之后是这一刻变化的信号各占一行 —— 1 位写 `0!`，多位写
`b1010 !`（末尾那个 `!` 是 VCD 的标识符码，多值必须拿空格和它隔开）。

**这里没有 latch 的语义了，只剩信号级波形。** 想看「77 号用户那笔 MU 任务等了多久」
要用 tracetto 或 trace2perfetto —— 那是从信号里推出来的任务区间，VCD 表达不了。
两件事由此而来：

  · **只看得见模型记了的那点信号**：core.h:80 的 TraceOffScope 把 Core 以下关掉了，
    所以 408 个 core 只有二十几个信号/core 在动。想看模块内部信号，得先跑对应的
    单模块用例；
  · latch 的时间是拍号，没有物理时间。这里给 `$timescale 1ns`、**1 拍 = 1 ns**，
    纯粹为了不让工具抱怨 —— 读出来的数就是拍号本身。

值只有 64 位整数（字符串走旁路 .strings.json，这里不转）。**位宽是推出来的**：
整条信号只取过 0/1 就当 1 位（写 `0!` / `1!`），否则按这条信号实际取到的最大值取
最小位宽（写 `b101 !`）。.trace 里不存位宽，所以看到的宽度是"这份波形里用到的"，
不是 RTL 里声明的 —— 要真位宽得从模型那边补。

不写 `$date` / `$version`：内容是随运行变的，写了同一份波形两次导出的结果就不一样，
而这两行没有任何工具依赖。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Dict, List, Tuple

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from tracetto.reader import TraceReader   # noqa: E402

# VCD 的标识符码是 33~126 这些可打印 ASCII。94 个一位码不够两万条信号用，所以按
# 94 进制往上涨：`!`、`"`…`~`，然后 `!!`、`!"`… 变长是合法的（码以空白结尾）。
ID_BASE = 33
ID_RADIX = 94


def id_code(i: int) -> str:
    """第 i 条信号的标识符码。"""
    out = ""
    while True:
        out = chr(ID_BASE + i % ID_RADIX) + out
        i //= ID_RADIX
        if i == 0:
            return out


def path_of(r: TraceReader, sid: int) -> List[str]:
    """从模块树往上走到根，返回 [scope..., 信号名]。照 tree_paths() 那套：父节点是
    0、是自己、或不在表里就当到根了。多一个 seen 兜环。"""
    names: List[str] = []
    seen = set()
    mid = sid
    while mid in r.mods:
        pid, name = r.mods[mid]
        names.append(name)
        if pid == 0 or pid == mid or pid in seen or pid not in r.mods:
            break
        seen.add(mid)
        mid = pid
    names.reverse()
    return names


def collect(r: TraceReader) -> List[tuple]:
    """把每条信号摆成 (scope, 名, 码, 位宽, 信号号)。按 scope 排，好让同一层的信号
    挨着，一趟就能把 $scope/$upscope 配对写完。"""
    rows = []
    for i, sid in enumerate(r.signals()):
        names = path_of(r, sid)
        if not names:
            continue                      # 模块表里没有它，摆不进层次
        vs = r.events(sid)[1]
        # 只取过 0/1 就当 1 位；否则按最大值取最小位宽。见文件头的说明。
        width = 1 if all(v in (0, 1) for v in vs) else max(1, max(vs).bit_length())
        rows.append((tuple(names[:-1]), names[-1], id_code(i), width, sid))
    rows.sort(key=lambda row: (row[0], row[1]))
    return rows


def write_vcd(r: TraceReader, rows: List[tuple], out) -> Tuple[int, int, int]:
    """先写声明区（层次 + $var），再按时间点写数据区。返回 (变化数, 时间点数, 信号数)。"""
    out.write("$timescale 1ns $end\n")

    # 声明区：scope 用栈比对，只写变化的那一截，同名外层不重复展开。
    stack: List[str] = []
    for scope, name, code, width, _sid in rows:
        keep = 0
        while keep < len(stack) and keep < len(scope) and stack[keep] == scope[keep]:
            keep += 1
        for _ in range(len(stack) - keep):
            out.write("$upscope $end\n")
        for seg in scope[keep:]:
            out.write(f"$scope module {seg} $end\n")
        stack = list(scope)
        out.write(f"$var wire {width} {code} {name} $end\n")
    for _ in range(len(stack)):
        out.write("$upscope $end\n")
    out.write("$enddefinitions $end\n")

    # 数据区：把所有信号的变化按时间归堆，再按时间升序写。每条信号自己的事件本来
    # 就是升序的，归堆是为了让同一个时刻的变化挨在一条 `#t` 底下。
    per_time: Dict[int, List[Tuple[str, int, int]]] = {}
    n_change = 0
    for _scope, _name, code, width, sid in rows:
        ts, vs = r.events(sid)
        for t, v in zip(ts, vs):
            per_time.setdefault(t, []).append((code, v, width))
            n_change += 1
    for t in sorted(per_time):
        out.write(f"#{t}\n")
        for code, v, width in per_time[t]:
            # 1 位直接写值拼码；多位要 `b<bits> <空格> <码>`。
            out.write(f"{v}{code}\n" if width == 1 else f"b{v:b} {code}\n")
    return n_change, len(per_time), len(rows)


def vcd_path(trace: Path) -> Path:
    """产物落在波形旁边：`<波形名>.vcd`。

    不用 with_suffix：它只收一个点开头、且不许再含点的后缀。
    """
    return Path(str(trace.with_suffix("")) + ".vcd")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        prog="trace2vcd",
        description="把 latch 的波形转成 VCD（GTKWave / Surfer / Verdi 能看）")
    p.add_argument("prefix", nargs="?", default=None,
                   help="波形路径或名字里的一截；不给就把当前目录下的列出来挑")
    p.add_argument("-o", "--out", default=None, help="输出路径，默认落在波形旁边")
    p.add_argument("-q", "--quiet", action="store_true")
    args = p.parse_args(argv)

    from tracetto.reader import resolve_trace
    trace = resolve_trace(args.prefix)
    prefix = str(trace.with_suffix(""))
    path = Path(args.out) if args.out else vcd_path(trace)

    with TraceReader(prefix) as r:
        rows = collect(r)
        with open(path, "w", encoding="utf-8") as f:
            n_change, n_time, n_sig = write_vcd(r, rows, f)

    if not args.quiet:
        size = path.stat().st_size
        print(f"[trace2vcd] {n_sig} 个信号 / {n_change} 次变化 / {n_time} 个时间点 "
              f"→ {path}（{size} B）")
        print("   1 拍 = 1 ns；GTKWave 里 File → Read VCD")
    return 0


if __name__ == "__main__":
    sys.exit(main())
