"""Command line for the packed-layer converter.

    python -m bach.compiler.bachflow layer.buff layer.bwgt --out bundle.bachir

``--hw FILE.toml`` overrides the Bach default hardware. The bundle is written
only when the conversion succeeds. ``--inventory FILE.json`` writes the harness
inventory beside it. ``--legacy-reduce-mapping`` is off by default and is an
experimental simulator translation; without it a same-face reduce still fails.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .convert import ConvertError, convert


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("buff", help="packed .buff")
    ap.add_argument("bwgt", help="packed .bwgt")
    ap.add_argument("--hw", default=None, help="hardware TOML; default is the Bach mesh")
    ap.add_argument("--out", required=True, help="where to write the .bachir")
    ap.add_argument("--inventory", default=None, help="where to write the harness inventory")
    ap.add_argument(
        "--legacy-reduce-mapping",
        action="store_true",
        help="experimental: pre-merge a two-lane forwarding predecessor; "
             "default rejects that shape",
    )
    args = ap.parse_args(argv)
    try:
        bundle = convert(
            args.buff, args.bwgt, args.hw,
            legacy_reduce_mapping=args.legacy_reduce_mapping,
        )
    except ConvertError as exc:
        print(exc, file=sys.stderr)
        return 2
    dest = Path(args.out)
    dest.write_text(bundle.text, encoding="utf-8")
    if args.inventory:
        Path(args.inventory).write_text(
            json.dumps(bundle.inventory.as_dict(), indent=2) + "\n", encoding="utf-8"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
