"""Write existing latch inputs from a packed layer and exact saved token files.

    python -m bach.compiler.bachflow.input layer.buff layer.bwgt \
        --token user101.token --out-dir bundle --legacy-reduce-mapping

The output is bundle/ep1_adapter/, used by the adjacent packed-input test.
Weights are converted only for shared and token-selected physical expert slots.
The reduce workaround is experimental and requires an explicit flag.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from .layer import open_layer
from .legacy_input import package_input
from .weights import EXPERT_BYTES, EXPERT_SCALES, write_weights


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("buff")
    parser.add_argument("bwgt")
    parser.add_argument("--token", action="append", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--hw")
    parser.add_argument("--legacy-reduce-mapping", action="store_true")
    args = parser.parse_args(argv)
    dest = Path(args.out_dir) / "ep1_adapter"
    if dest.exists():
        parser.error(f"{dest} already exists; use a new directory")
    layer = open_layer(args.buff, args.bwgt, args.hw)
    try:
        bundle = package_input(layer, args.out_dir,
                               legacy_reduce_mapping=args.legacy_reduce_mapping)
        rows = write_weights(layer, args.token, bundle.out_dir)
        if not rows:
            raise ValueError("the packed input contains no selected expert weights")
        (bundle.out_dir / "weights.txt").write_text("".join(
            f"{r.chip} {r.p} {r.base} {EXPERT_BYTES} {EXPERT_SCALES} "
            f"{r.data_name} {r.scale_name}\n" for r in rows
        ))
        print(f"{bundle.bachir}: {len(rows)} converted weight records")
    finally:
        layer.close()


if __name__ == "__main__":
    main()
