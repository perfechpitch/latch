from __future__ import annotations

import bisect
import io
import struct
from pathlib import Path
from typing import Dict, List, Tuple

from .indexer import (BUCKET_FMT, BUCKET_SIZE, DOM_K, DOM_W_ONE,
                      FORMAT_VERSION, LEVEL_BASE_CELLS, IndexView, build,
                      ensure, quantize_runs, rle_runs)
from .reader import Reader

WEB_DIR = Path(__file__).resolve().parent / "web"

def _load_labels(prefix: str) -> Dict[str, Dict[int, str]]:
    import json
    path = Path(prefix + ".strings.json")
    if not path.exists():
        return {}
    try:
        raw = json.loads(path.read_text())
    except (OSError, ValueError):
        return {}
    out: Dict[str, Dict[int, str]] = {}
    for sig_id, table in (raw.get("labels") or {}).items():
        out[str(sig_id)] = {int(v): name for v, name in table.items()}
    return out

def _build_tree(reader: Reader) -> list[dict]:
    paths = reader.tree_paths()
    sigs = set(reader.signals())
    out = []
    for m in reader.modules():
        out.append({
            "id": m.id,
            "parent_id": m.parent_id,
            "name": m.name,
            "path": paths.get(m.id, m.name),
            "is_signal": m.id in sigs,
            "event_count": reader.event_count(m.id) if m.id in sigs else 0,
        })
    return out

def _signal_unique_count(reader: Reader, signal_id: int,
                          t0: int | None = None, t1: int | None = None,
                          cap: int = 256) -> tuple[int, list[tuple[int, int]]]:
    ts, vs = reader.events(signal_id)
    lo = bisect.bisect_left(ts, t0) if t0 is not None else 0
    hi = bisect.bisect_right(ts, t1) if t1 is not None else len(ts)
    counts: Dict[int, int] = {}
    over_cap = False
    for v in vs[lo:hi]:
        counts[v] = counts.get(v, 0) + 1
        if len(counts) > cap:
            over_cap = True
    top = sorted(counts.items(), key=lambda kv: -kv[1])[:16]
    unique = len(counts) if not over_cap else cap + 1
    return unique, top

def _runs_to_buckets(run_ts, run_te, run_vs, run_cnt, t0: int, t1: int
                     ) -> List[Tuple[int, int, int, int, int, int, int]]:
    import numpy as np
    if run_ts.size == 0:
        return []

    q = np.array([max(0, t0), max(0, t1)], dtype=np.uint64)
    lo_i, hi_i = np.searchsorted(run_ts, q, side="right")
    lo = max(0, int(lo_i) - 1)
    hi = int(hi_i)
    out: List[Tuple[int, ...]] = []
    zeros = (0,) * (DOM_K - 1)
    for i in range(lo, hi):
        v = int(run_vs[i])
        out.append((int(run_ts[i]), int(run_te[i]), v, v, v, v,
                    int(run_cnt[i]),
                    v, *zeros, DOM_W_ONE, *zeros))
    return out

def create_app(prefix: str, cycle_time: int = 1):
    try:
        from fastapi import FastAPI, HTTPException, Response
        from fastapi.responses import FileResponse, JSONResponse
        from fastapi.staticfiles import StaticFiles
    except ImportError as e:
        raise SystemExit(
            "insight serve requires fastapi + uvicorn:\n"
            "    pip install fastapi uvicorn\n"
            f"(import error: {e})"
        )

    manifest = ensure(prefix, progress=True)
    reader = Reader(prefix, cache_events=False)
    view = IndexView(prefix, manifest)
    labels = _load_labels(prefix)

    trace_t_end = max((s.t_last for s in manifest.signals.values()), default=0)

    app = FastAPI(title="insight")

    if WEB_DIR.exists():
        app.mount("/static", StaticFiles(directory=str(WEB_DIR)), name="static")

    @app.get("/")
    def root():
        index = WEB_DIR / "index.html"
        if index.exists():

            return FileResponse(str(index),
                                headers={"Cache-Control": "no-cache"})
        return JSONResponse({"status": "ok", "msg": "web/ not bundled"},
                            status_code=200)

    @app.get("/api/tree")
    def api_tree():
        return _build_tree(reader)

    @app.post("/api/reload")
    def api_reload():
        nonlocal manifest, reader, view, trace_t_end, rle_cache, labels
        manifest = build(prefix, progress=False)
        reader = Reader(prefix, cache_events=False)
        view = IndexView(prefix, manifest)
        labels = _load_labels(prefix)
        trace_t_end = max((s.t_last for s in manifest.signals.values()),
                          default=0)
        rle_cache = {}
        _warm()
        return {"ok": True, "wire_format": FORMAT_VERSION,
                "signal_count": len(manifest.signals)}

    @app.get("/api/range")
    def api_range(sig: int):
        if sig not in manifest.signals:
            raise HTTPException(404, "signal not found")
        t0, t1 = view.time_range(sig)
        return {"t0": t0, "t1": t1, "event_count": view.total_events(sig)}

    @app.get("/api/init")
    def api_init():
        tree = _build_tree(reader)
        signals = {}
        for sig_id, summary in manifest.signals.items():
            entry = {
                "t0": summary.t_first,
                "t1": summary.t_last,
                "event_count": summary.total,
                "runs": summary.runs,
                "min_dt": summary.min_dt,
                "unique_values": summary.unique,
                "unique_capped": summary.unique_capped,
                "monotone": summary.monotone,
                "values": summary.values,
            }

            sig_labels = labels.get(str(sig_id))
            if sig_labels:
                entry["labels"] = sig_labels
            signals[str(sig_id)] = entry
        return {"tree": tree, "signals": signals,
                "wire_format": FORMAT_VERSION,
                "cycle_time": cycle_time}

    RLE_CACHE_MAX_RUNS = 2_000_000
    rle_cache: Dict[int, tuple] = {}

    def _signal_runs(sig: int, t0: int, t1: int) -> tuple:
        cached = rle_cache.get(sig)
        if cached is not None:
            return cached
        s = manifest.signals[sig]
        if s.runs <= RLE_CACHE_MAX_RUNS:
            ts, vs = reader.events_window(sig, s.t_first, s.t_last)
            runs = rle_runs(ts, vs, trace_t_end)
            rle_cache[sig] = runs

            reader.drop_segment_cache(sig)
            return runs
        ts, vs = reader.events_window(sig, t0, t1)
        return rle_runs(ts, vs, trace_t_end)

    def _warm():
        import concurrent.futures
        import os as _os
        import time as _time

        def warm_one(sig):
            s = manifest.signals[sig]
            try:
                if s.runs <= RLE_CACHE_MAX_RUNS:
                    _signal_runs(sig, s.t_first, s.t_last)
                else:

                    for i in reader.pass_segment_indices(sig):
                        reader._decode_segment_cached(sig, i)
            except Exception:
                pass

        sigs = sorted(manifest.signals)
        workers = max(1, min(len(sigs), (_os.cpu_count() or 4) // 2))
        t0 = _time.time()
        print(f"[insight] warming {len(sigs)} signals on {workers} threads ...",
              flush=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
            list(ex.map(warm_one, sigs))
        print(f"[insight] warmed in {_time.time() - t0:.1f}s", flush=True)

    _warm()

    SMALL_DECODE_EVENTS = 2_000_000

    INLINE_DECODE_SEGMENTS = 3

    def _stride1(sig: int) -> int:
        s = manifest.signals[sig]
        span = max(1, s.t_last - s.t_first)
        return max(1, -(-span // LEVEL_BASE_CELLS))

    def _window_segments(sig: int, t0: int, t1: int) -> int:
        segs = reader.segments(sig)
        return sum(1 for i in reader.pass_segment_indices(sig)
                   if segs[i].t_first <= t1 and segs[i].t_last >= t0)

    def _runs_for_window(sig: int, t0: int, t1: int, need_exact: bool):
        cached = rle_cache.get(sig)
        if cached is not None:
            return cached
        s = manifest.signals[sig]
        if s.total <= SMALL_DECODE_EVENTS or not s.levels:
            return _signal_runs(sig, t0, t1)
        if (s.runs > RLE_CACHE_MAX_RUNS
                or (need_exact
                    and _window_segments(sig, t0, t1)
                        <= INLINE_DECODE_SEGMENTS)):

            ts, vs = reader.events_window(sig, t0, t1)
            return rle_runs(ts, vs, trace_t_end)
        return None

    def _quantized_buckets(runs, t0: int, t1: int, stride: int) -> list:
        import numpy as np
        run_ts, run_te, run_vs, run_cnt = runs
        if run_ts.size == 0:
            return []
        q = np.array([max(0, t0), max(0, t1)], dtype=np.uint64)
        lo_i, hi_i = np.searchsorted(run_ts, q, side="right")
        lo = max(0, int(lo_i) - 1)
        hi = int(hi_i)
        return quantize_runs(run_ts[lo:hi], run_te[lo:hi],
                             run_vs[lo:hi], run_cnt[lo:hi], stride)

    def _samples_for(sig: int, t0: int, t1: int, width: int
                     ) -> tuple[int, list]:
        sig_t0, sig_t1 = view.time_range(sig)
        runs_total = view.total_runs(sig)
        if sig_t1 > sig_t0 and runs_total > 0:
            frac = max(0.0, min(1.0, (t1 - t0) / (sig_t1 - sig_t0)))
            est = max(1, int(runs_total * frac))
        else:
            est = runs_total
        req_px = max(1, -(-(t1 - t0) // max(1, width)))
        need_exact = _stride1(sig) > req_px
        if est <= width or need_exact:
            runs = _runs_for_window(sig, t0, t1, need_exact)
            if runs is not None:
                if est <= width:
                    return 0, _runs_to_buckets(*runs, t0, t1)
                return 1, _quantized_buckets(runs, t0, t1, req_px)

        lvl, buckets = view.query(sig, t0, t1, width)
        out = [(b.t_start, b.t_end, b.v_min, b.v_max, b.v_last, b.v_dom,
                b.count, *b.dom_v, *b.dom_w) for b in buckets]
        return lvl, out

    def _check_fmt(fmt: int | None) -> None:
        if fmt != FORMAT_VERSION:
            raise HTTPException(
                426, f"frontend wire format {fmt} != server "
                     f"{FORMAT_VERSION}; hard-refresh the page (Ctrl+Shift+R)")

    @app.get("/api/samples")
    def api_samples(sig: int, t0: int, t1: int, width: int = 1024,
                    fmt: int | None = None):
        _check_fmt(fmt)
        if sig not in manifest.signals:
            raise HTTPException(404, "signal not found")
        if t1 < t0:
            raise HTTPException(400, "t1 < t0")
        width = max(1, min(width, 8192))
        lvl, buckets = _samples_for(sig, t0, t1, width)
        buf = io.BytesIO()
        buf.write(struct.pack("<II", lvl, len(buckets)))
        for b in buckets:
            buf.write(struct.pack(BUCKET_FMT, *b))
        return Response(buf.getvalue(), media_type="application/octet-stream")

    @app.get("/api/samples_multi")
    def api_samples_multi(sigs: str, t0: int, t1: int, width: int = 1024,
                          fmt: int | None = None):
        _check_fmt(fmt)
        if t1 < t0:
            raise HTTPException(400, "t1 < t0")
        width = max(1, min(width, 8192))
        try:
            sig_ids = [int(s) for s in sigs.split(",") if s]
        except ValueError:
            raise HTTPException(400, "invalid sigs list")

        buf = io.BytesIO()
        buf.write(struct.pack("<I", len(sig_ids)))
        for sig in sig_ids:
            if sig not in manifest.signals:
                buf.write(struct.pack("<QII", sig, 0, 0))
                continue
            lvl, buckets = _samples_for(sig, t0, t1, width)
            buf.write(struct.pack("<QII", sig, lvl, len(buckets)))
            for b in buckets:
                buf.write(struct.pack(BUCKET_FMT, *b))
        return Response(buf.getvalue(), media_type="application/octet-stream")

    @app.get("/api/point")
    def api_point(sig: int, t: int):
        if sig not in manifest.signals:
            raise HTTPException(404, "signal not found")
        import numpy as np

        if t < 0:
            raise HTTPException(404, "no event at or before t")

        runs = rle_cache.get(sig)
        if runs is not None:
            run_ts, run_te, run_vs, run_cnt = runs
            if run_ts.size:
                i = int(np.searchsorted(run_ts, np.uint64(t),
                                        side="right")) - 1
                if i < 0:
                    raise HTTPException(404, "no event at or before t")
                return {"t": int(run_ts[i]), "v": int(run_vs[i])}
        ts, vs = reader.events_window(sig, t, t)
        if ts.size == 0:
            raise HTTPException(404, "signal has no events")

        if ts.size > 1 and not bool((ts[:-1] <= ts[1:]).all()):
            order = np.argsort(ts, kind="stable")
            ts = ts[order]
            vs = vs[order]
        q = np.uint64(t)
        i = int(np.searchsorted(ts, q, side="right")) - 1
        if i < 0:
            raise HTTPException(404, "no event at or before t")
        return {"t": int(ts[i]), "v": int(vs[i])}

    @app.get("/api/stats")
    def api_stats(sig: int, t0: int | None = None, t1: int | None = None):
        if sig not in manifest.signals:
            raise HTTPException(404, "signal not found")

        if t0 is None and t1 is None:
            unique, capped = view.unique_values(sig)
            return {
                "event_count": view.total_events(sig),
                "unique_values": unique,
                "unique_capped": capped,
                "top": [],
            }
        unique, top = _signal_unique_count(reader, sig, t0, t1)
        return {
            "event_count": sum(c for _, c in top),
            "unique_values": unique,
            "top": [{"v": v, "count": c} for v, c in top],
        }

    def _merge_windows(wins):
        merged = []
        for w0, w1 in sorted(wins):
            if merged and w0 <= merged[-1][1]:
                merged[-1] = (merged[-1][0], max(merged[-1][1], w1))
            else:
                merged.append((w0, w1))
        return merged

    def _signal_window_stats(sig: int, wins) -> dict:
        import numpy as np
        ct = max(1, cycle_time)
        if not wins:
            s = manifest.signals[sig]
            w0 = (max(0, s.t_first) // ct) * ct
            w1 = -(-max(w0 + 1, s.t_last) // ct) * ct
            wins = [(w0, w1)]
        tot_dur = 0.0
        zero_dur = 0.0
        integral = 0.0
        for w0, w1 in wins:
            run_ts, run_te, run_vs, _ = _signal_runs(sig, w0, w1)
            if run_ts.size == 0:
                continue
            q = np.array([w0, w1], dtype=np.uint64)
            lo_i, hi_i = np.searchsorted(run_ts, q, side="right")
            lo = max(0, int(lo_i) - 1)
            hi = int(hi_i)
            if hi <= lo:
                continue
            ts = run_ts[lo:hi].astype(np.float64)
            te = run_te[lo:hi].astype(np.float64)
            vs = run_vs[lo:hi].astype(np.float64)
            a = np.maximum(ts, float(w0))
            b = np.minimum(te, float(w1))
            dur = np.maximum(0.0, b - a)
            tot_dur += float(dur.sum())
            zero_dur += float(dur[vs == 0.0].sum())
            integral += float((vs * dur).sum())
        nonzero_dur = max(0.0, tot_dur - zero_dur)
        return {
            "sig": sig,
            "total_cycles": tot_dur / ct,
            "zero_cycles": zero_dur / ct,
            "zero_ratio": (zero_dur / tot_dur) if tot_dur > 0 else 0.0,
            "nonzero_cycles": nonzero_dur / ct,
            "nonzero_ratio": (nonzero_dur / tot_dur) if tot_dur > 0 else 0.0,
            "avg_per_cycle": (integral / tot_dur) if tot_dur > 0 else 0.0,
        }

    @app.get("/api/select_stats")
    def api_select_stats(sigs: str, ranges: str = ""):
        ct = max(1, cycle_time)
        try:
            sig_ids = [int(s) for s in sigs.split(",") if s]
        except ValueError:
            raise HTTPException(400, "invalid sigs list")

        wins = []
        for part in ranges.split(","):
            if not part:
                continue
            try:
                a_str, b_str = part.split(":")
                a, b = int(a_str), int(b_str)
            except ValueError:
                raise HTTPException(400, "invalid ranges list")
            if b < a:
                a, b = b, a
            w0 = (max(0, a) // ct) * ct
            w1 = -(-max(w0 + 1, b) // ct) * ct
            wins.append((w0, w1))
        wins = _merge_windows(wins)
        signals = [_signal_window_stats(sig, wins)
                   for sig in sig_ids if sig in manifest.signals]
        return {"ranges": [[a, b] for a, b in wins], "signals": signals}

    return app

def _find_free_port(host: str, port: int, tries: int = 100) -> int:
    import socket
    for cand in range(port, port + tries):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                s.bind((host, cand))
                return cand
            except OSError:
                continue
    raise SystemExit(
        f"[insight] no free port in [{port}, {port + tries}) on {host}"
    )

def serve(prefix: str, host: str = "127.0.0.1", port: int = 8765,
          cycle_time: int = 1) -> None:
    try:
        import uvicorn
    except ImportError:
        raise SystemExit(
            "insight serve requires uvicorn:\n    pip install uvicorn"
        )
    chosen = _find_free_port(host, port)
    if chosen != port:
        print(f"[insight] port {port} in use, falling back to {chosen}")
    port = chosen
    app = create_app(prefix, cycle_time=cycle_time)
    print(f"[insight] serving {prefix!r} at http://{host}:{port}/")
    uvicorn.run(app, host=host, port=port,
                log_level="warning", access_log=False)
