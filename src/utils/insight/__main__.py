from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .reader import Reader

def _strip_trace(p: Path) -> str:
    return str(p.with_suffix("")) if p.suffix == ".trace" else str(p)

def _pick_interactive(matches: list[Path], label: str) -> Path:
    if not sys.stdin.isatty():
        print(f"[insight] {label}; stdin is not a tty, picking newest:",
              file=sys.stderr)
        print(f"  {matches[0]}", file=sys.stderr)
        return matches[0]

    print(f"[insight] {label}:", file=sys.stderr)
    for i, p in enumerate(matches):
        marker = " (newest)" if i == 0 else ""
        print(f"  [{i}] {p}{marker}", file=sys.stderr)
    while True:
        try:
            raw = input(
                f"[insight] choose [0-{len(matches) - 1}, default 0, q to abort]: "
            ).strip()
        except (EOFError, KeyboardInterrupt):
            sys.exit("\n[insight] aborted")
        if raw == "":
            return matches[0]
        if raw in ("q", "Q"):
            sys.exit("[insight] aborted")
        try:
            idx = int(raw)
        except ValueError:
            print(f"[insight] '{raw}' is not a number, try again",
                  file=sys.stderr)
            continue
        if 0 <= idx < len(matches):
            return matches[idx]
        print(f"[insight] {idx} out of range, try again", file=sys.stderr)

def _resolve_prefix(arg: str | None) -> str:
    cwd = Path.cwd()

    if arg:
        for cand in (Path(arg), Path(arg + ".trace")):
            if cand.is_file():
                return _strip_trace(cand)
        traces = sorted(cwd.rglob("*.trace"),
                        key=lambda p: -p.stat().st_mtime)
        matches = [p for p in traces if arg in str(p)]
        if not matches:
            sys.exit(f"[insight] no .trace file matching '{arg}' under {cwd}")
        if len(matches) == 1:
            return _strip_trace(matches[0])
        return _strip_trace(
            _pick_interactive(matches, f"multiple traces match '{arg}'")
        )

    traces = sorted(cwd.rglob("*.trace"),
                    key=lambda p: -p.stat().st_mtime)
    if not traces:
        sys.exit(f"[insight] no .trace file found under {cwd}; "
                 f"pass a prefix or run from a directory that contains one")
    if len(traces) == 1:
        return _strip_trace(traces[0])
    return _strip_trace(
        _pick_interactive(traces, f"found {len(traces)} traces under {cwd}")
    )

def cmd_inspect(args: argparse.Namespace) -> int:
    prefix = _resolve_prefix(args.prefix)
    with Reader(prefix) as r:
        paths = r.tree_paths()

        print(f"=== trace: {prefix}.trace ===")
        print(f"=== modules ({len(r.modules())}) ===")
        for m in r.modules():
            tag = " <signal>" if m.id in r.signals() else ""
            print(f"  id={m.id:<6d} parent={m.parent_id:<6d} {paths[m.id]}{tag}")

        sigs = r.signals()
        print(f"\n=== signals ({len(sigs)}) ===")
        for sig_id in sigs:
            segs = r.segments(sig_id)
            total = sum(s.event_count for s in segs)
            if not segs:
                continue
            t_first = segs[0].t_first
            t_last  = segs[-1].t_last
            v_min   = min(s.v_min for s in segs)
            v_max   = max(s.v_max for s in segs)
            body_bytes = sum(s.body_len for s in segs)
            name = paths.get(sig_id, f"?({sig_id})")
            print(
                f"  {name}  id={sig_id}  segs={len(segs)}  events={total}  "
                f"t=[{t_first}..{t_last}]  v=[{v_min}..{v_max}]  body={body_bytes}B"
            )
    return 0

def cmd_events(args: argparse.Namespace) -> int:
    prefix = _resolve_prefix(args.prefix)
    with Reader(prefix) as r:
        for i, (t, v) in enumerate(r.iter_events(args.signal_id)):
            print(f"{t}\t{v}")
            if args.limit is not None and i + 1 >= args.limit:
                break
    return 0

def cmd_index(args: argparse.Namespace) -> int:
    from .indexer import build
    prefix = _resolve_prefix(args.prefix)
    m = build(prefix, progress=True)
    print(f"[index] built; signals={list(m.signals)}")
    return 0

def cmd_serve(args: argparse.Namespace) -> int:
    from .server import serve
    prefix = _resolve_prefix(args.prefix)
    serve(prefix, host=args.host, port=args.port, cycle_time=args.cycle_time)
    return 0

_SUBCOMMANDS = {"inspect", "events", "index", "serve"}

def main(argv: list[str] | None = None) -> int:
    if argv is None:
        argv = sys.argv[1:]

    if not argv or argv[0] not in _SUBCOMMANDS and argv[0] not in ("-h",
                                                                   "--help"):
        argv = ["serve"] + list(argv)

    p = argparse.ArgumentParser(prog="insight",
                                description="latch trace reader / inspector")
    sub = p.add_subparsers(dest="cmd", required=True)

    pi = sub.add_parser("inspect", help="summarize a trace file")
    pi.add_argument("prefix", nargs="?", default=None,
                    help="trace path / prefix / hint (default: autodetect)")
    pi.set_defaults(func=cmd_inspect)

    pe = sub.add_parser("events", help="dump (t, v) for a single signal")
    pe.add_argument("signal_id", type=int)
    pe.add_argument("prefix", nargs="?", default=None,
                    help="trace path / prefix / hint (default: autodetect)")
    pe.add_argument("--limit", type=int, default=None,
                    help="stop after this many events")
    pe.set_defaults(func=cmd_events)

    px = sub.add_parser("index", help="build or refresh the pyramid index")
    px.add_argument("prefix", nargs="?", default=None,
                    help="trace path / prefix / hint (default: autodetect)")
    px.set_defaults(func=cmd_index)

    ps = sub.add_parser("serve", help="serve the web UI")
    ps.add_argument("prefix", nargs="?", default=None,
                    help="trace path / prefix / hint (default: autodetect)")
    ps.add_argument("--host", default="127.0.0.1")
    ps.add_argument("--port", type=int, default=8765)
    ps.add_argument("--cycle-time", type=int, default=1,
                    help="clock period in raw trace-time units; the time "
                         "ruler is labelled in cycles (1 = cycle-time ticks). "
                         "Default 1, matching MakeClock(0, 1).")
    ps.set_defaults(func=cmd_serve)

    args = p.parse_args(argv)
    return args.func(args)

if __name__ == "__main__":
    sys.exit(main())
