"""命令行。

    python3 src/utils/tracetto/tracetto.py                 # 从当前目录往下找 .trace，一份就直接用
    python3 src/utils/tracetto/tracetto.py moe_lpu         # 名字里带这一截的
    python3 src/utils/tracetto/tracetto.py moe_lpu --port 8899
    python3 src/utils/tracetto/tracetto.py moe_lpu --reindex      # 不管现成的索引，重建一次
    python3 src/utils/tracetto/tracetto.py moe_lpu --index-only   # 只建索引，不起服务
    python3 src/utils/tracetto/tracetto.py moe_lpu --dump         # 把索引的 manifest 打成 JSON

也可以 `python3 -m src.utils.tracetto …`（在仓库根下）。
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Optional

from . import idxbuild, server
from .reader import resolve_trace


def main(argv: Optional[list] = None) -> int:
    p = argparse.ArgumentParser(
        prog="tracetto", description="起一个服务，把 latch 的波形按 user / task 分段看")
    p.add_argument("prefix", nargs="?", default=None,
                   help="波形路径或名字里的一截，不给就把当前目录下的列出来挑")
    p.add_argument("--host", default="127.0.0.1", help="监听地址，默认 127.0.0.1")
    p.add_argument("--port", type=int, default=8766, help="监听端口，默认 8766；占了就往上找")
    p.add_argument("--workers", type=int, default=None, help="建索引用几个进程，默认按 CPU 数")
    p.add_argument("--reindex", action="store_true", help="不管现成的索引，重建一次")
    p.add_argument("--index-only", action="store_true", help="只建索引，不起服务")
    p.add_argument("--dump", action="store_true", help="把索引的 manifest 打成 JSON")
    args = p.parse_args(argv)

    trace = resolve_trace(args.prefix)
    try:
        if args.dump:
            manifest, _ = idxbuild.ensure(trace, reindex=args.reindex, workers=args.workers)
            json.dump(manifest, sys.stdout, ensure_ascii=False, indent=2)
            sys.stdout.write("\n")
            return 0
        if args.index_only:
            manifest, rebuilt = idxbuild.ensure(trace, reindex=args.reindex,
                                                workers=args.workers)
            n_core = sum(1 for size in manifest.get("core_size", []) if size)
            print(f"[tracetto] {trace.name}：{len(manifest['chips'])} chip / {n_core} core / "
                  f"{len(manifest['lane_n'])} 条行"
                  f"{'（刚建的）' if rebuilt else '（现成的）'}")
            return 0
        return server.serve(trace, host=args.host, port=args.port,
                            workers=args.workers, reindex=args.reindex)
    except ValueError as e:
        sys.exit(f"[tracetto] {trace}.trace 读不动：{e}")


if __name__ == "__main__":
    sys.exit(main())
