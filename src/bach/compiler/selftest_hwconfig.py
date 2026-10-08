#!/usr/bin/env python3
"""硬件配置生成器的自测。

对 compiler/topo 下的每份拓扑描述跑一遍生成，要求三件事都成立：产物记录齐全、
同一种记录的字段数一致、同一份描述两次跑出来逐字节一致。
"""

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from gen_hwconfig import build_topo     # noqa: E402
from hwconfig import moe               # noqa: E402

TOPO = HERE / "topo"

# 每份产物至少要有的记录，缺一样就说明那一层没产出来
REQUIRED = ("CHIP", "CORE", "CFGMISC", "TCHAIN", "TSRTAB", "DATAIN", "DTEIN",
            "RTAB", "RTABDTE", "PATHTASK", "KERNEL", "VUSTATIC")


def kinds_of(lines):
    out = {}
    for line in lines:
        if line and not line.startswith("#"):
            head = line.split()[0]
            out[head] = out.get(head, 0) + 1
    return out


def check_one(path):
    """跑一份拓扑描述，返回失败原因，全过则返回 None。"""
    try:
        plan, lines = build_topo(path)
    except (ValueError, KeyError) as err:
        return f"展开失败：{err}"

    kinds = kinds_of(lines)
    missing = [k for k in REQUIRED if k not in kinds]
    if missing:
        return f"少了这些记录：{missing}"

    # 每条记录的字段数要一致：装载那一侧按位置取字段，多一个少一个都读错位
    widths = {}
    for line in lines:
        if not line or line.startswith("#"):
            continue
        tok = line.split()
        if tok[0] in ("BACHIR", "SOURCE", "NAME"):
            continue
        if tok[0] in widths and widths[tok[0]] != len(tok):
            return f"{tok[0]} 的字段数不一致：{widths[tok[0]]} 与 {len(tok)}"
        widths[tok[0]] = len(tok)

    if build_topo(path)[1] != lines:
        return "两次跑出来不一致"

    # 行链的上游分量直接进入 Rmem；本地只发一包，所有跳的任务边界一致。
    for chip, col in enumerate(plan.cols):
        key = (chip, moe.dot_core_of(col))
        entry = plan.entries[key][moe.ROW_PATH]
        if not entry.path_core_bypass or entry.operation == moe.OP_FORWARD:
            return f"{key} 行链分量没有直接进入 Rmem"
        if entry.flow_dir & (moe.FLOW_REDUCE1 | moe.FLOW_REDUCE2):
            return f"{key} 行链仍按本 core 多操作数发包"
        if moe.ROW_PATH in plan.path_task[key]:
            return f"{key} 行链仍配置了进核搬运任务"
        row = plan.chains[key][-1]
        if (row.idx != moe.DOT_ROW_TASK or row.path_id != moe.ROW_PATH or
                row.sym != ("dte", "task_dte_send_row") or
                row.task_type != "REDUCE" or not row.credit_en):
            return f"{key} 行链任务边界或本地发包配置不一致"
    return None


def main():
    files = sorted(TOPO.glob("*.json"))
    if not files:
        print(f"没有找到拓扑描述：{TOPO}", file=sys.stderr)
        return 1

    bad = 0
    for path in files:
        why = check_one(path)
        if why is None:
            print(f"  通过  {path.name}")
        else:
            print(f"  失败  {path.name}：{why}", file=sys.stderr)
            bad += 1

    print(f"{len(files) - bad}/{len(files)} 份拓扑描述通过")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
