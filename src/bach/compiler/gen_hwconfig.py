#!/usr/bin/env python3
"""把一份输入编成一套硬件配置，与 kernel 一起放进 bundle。

用法：
    python3 gen_hwconfig.py --topo <拓扑描述.json> [--bundle <目录>]

一份拓扑描述编出一套 bundle，落在 `bundle/<拓扑名>/` 下，装着两样：一份 .bachir
（一行一条记录，算好的 RouterTable、任务链与每个 core 的全局项）与三份 kernel
镜像。一套 bundle 自己带全，与真机上一次 launch 装的东西对齐。
"""

import argparse
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from hwconfig import bachir, kernelmap, topology

# 每份拓扑描述编出一套，各占一个子目录
BUNDLE = Path(__file__).resolve().parent / "bundle"


def build_topo(topo_path):
    """按一份拓扑描述展开，写成产物行。返回 Plan 与产物行。"""
    kmap = kernelmap.KernelMap().load()
    plan = topology.load(topo_path, kmap)
    images = [(kind, src.name) for kind, src in sorted(kmap.images.items())]
    return plan, bachir.render_plan(plan, topo_path, images)


def main():
    ap = argparse.ArgumentParser(description="编出一套硬件配置放进 bundle")
    ap.add_argument("--topo", required=True, help="拓扑描述")
    ap.add_argument("--bundle", default=str(BUNDLE), help="bundle 根目录")
    ap.add_argument("--quiet", action="store_true", help="只报错，不打摘要")
    args = ap.parse_args()

    inp = args.topo
    try:
        plan, lines = build_topo(inp)
    except (ValueError, KeyError) as err:
        print(f"读入失败：{err}", file=sys.stderr)
        return 1
    summary = (f"  chip {plan.chip_num} 颗，core {len(plan.role)} 个，"
               f"任务链 {len(plan.chains)} 条，记录 {len(lines)} 行")

    out_dir = Path(args.bundle) / plan.name
    out_dir.mkdir(parents=True, exist_ok=True)
    dst = out_dir / (plan.name + ".bachir")
    dst.write_text("\n".join(lines) + "\n", encoding="utf-8")
    # kernel 编一份，每套 bundle 各拿一份副本：装载那一侧只认自己这个目录。
    kmap = kernelmap.KernelMap().load()
    for src in sorted(kmap.images.values()):
        shutil.copyfile(src, out_dir / src.name)

    if not args.quiet:
        kinds = {}
        for line in lines:
            if line and not line.startswith("#"):
                kinds[line.split()[0]] = kinds.get(line.split()[0], 0) + 1
        print(f"写出 {out_dir}")
        print(summary)
        print("  " + "　".join(f"{k} {v}" for k, v in sorted(kinds.items())
                               if k not in ("BACHIR", "SOURCE", "NAME")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
