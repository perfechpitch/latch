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

TOPO = HERE / "topo"

# 每份产物至少要有的记录，缺一样就说明那一层没产出来
REQUIRED = ("CHIP", "CORE", "CFGMISC", "TCHAIN", "TSRTAB", "DATAIN", "RTAB",
            "RTABDTE", "PATHTASK", "KERNEL")


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
        _, lines = build_topo(path)
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
