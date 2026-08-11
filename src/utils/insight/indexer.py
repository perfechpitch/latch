from __future__ import annotations

import hashlib
import json
import multiprocessing as mp
import os
import struct
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
from functools import partial
from pathlib import Path
from typing import Dict, List, Tuple

from .reader import Reader

DOM_K = 8
BUCKET_FMT = "<QQQQQQQ" + "Q" * DOM_K + "H" * DOM_K
BUCKET_SIZE = struct.calcsize(BUCKET_FMT)
assert BUCKET_SIZE == 136
DOM_W_ONE = 65535

MAX_LEVEL = 3

LEVEL_BASE_CELLS = 262144
LEVEL_RATIO = 8

FORMAT_VERSION = 7
VALUES_LISTED = 64

@dataclass
class SignalSummary:
    levels: List[int]
    t_first: int
    t_last: int
    total: int
    runs: int
    min_dt: int
    unique: int
    unique_capped: bool
    monotone: bool
    values: List[int]

    def to_dict(self) -> dict:
        return {
            "levels": self.levels,
            "t_first": self.t_first,
            "t_last": self.t_last,
            "total": self.total,
            "runs": self.runs,
            "min_dt": self.min_dt,
            "unique": self.unique,
            "unique_capped": self.unique_capped,
            "monotone": self.monotone,
            "values": self.values,
        }

    @staticmethod
    def from_dict(d: dict) -> "SignalSummary":

        return SignalSummary(
            levels=list(d["levels"]),
            t_first=int(d["t_first"]),
            t_last=int(d["t_last"]),
            total=int(d["total"]),
            runs=int(d["runs"]),
            min_dt=int(d["min_dt"]),
            unique=int(d["unique"]),
            unique_capped=bool(d["unique_capped"]),
            monotone=bool(d["monotone"]),
            values=[int(v) for v in d["values"]],
        )

UNIQUE_CAP = 256

@dataclass
class Manifest:
    source_path: str
    source_size: int
    source_mtime: float
    signals: Dict[int, SignalSummary]

    def to_dict(self) -> dict:
        return {
            "format": FORMAT_VERSION,
            "source_path": self.source_path,
            "source_size": self.source_size,
            "source_mtime": self.source_mtime,
            "signals": {str(k): v.to_dict() for k, v in self.signals.items()},
        }

    @staticmethod
    def from_dict(d: dict) -> "Manifest":
        if d.get("format") != FORMAT_VERSION:
            raise ValueError("stale index format; rebuild required")
        sigs = d["signals"]
        if sigs and isinstance(next(iter(sigs.values())), list):
            raise ValueError("legacy manifest schema; rebuild required")
        if "source_size" not in d:
            raise ValueError("legacy two-file manifest; rebuild required")
        return Manifest(
            source_path=d["source_path"],
            source_size=d["source_size"],
            source_mtime=d["source_mtime"],
            signals={int(k): SignalSummary.from_dict(v) for k, v in sigs.items()},
        )

def _source_hash(prefix: str) -> str:
    st = os.stat(prefix + ".trace")
    h = hashlib.sha1()
    h.update(os.path.abspath(prefix).encode())
    h.update(str(st.st_size).encode())
    h.update(str(int(st.st_mtime_ns)).encode())
    return h.hexdigest()[:16]

def cache_dir(prefix: str, root: str | None = None) -> Path:
    root_p = Path(root) if root else Path("/tmp/insight")
    return root_p / _source_hash(prefix)

def rle_runs(ts, vs, tail_end: int | None = None):
    import numpy as np
    n = int(ts.size)
    if n == 0:
        e = np.empty(0, dtype=np.uint64)
        return e, e.copy(), e.copy(), np.empty(0, dtype=np.int64)
    if n > 1 and not bool((ts[:-1] <= ts[1:]).all()):
        order = np.argsort(ts, kind="stable")
        ts = ts[order]
        vs = vs[order]
    if n > 1:
        keep = np.empty(n, dtype=bool)
        keep[:-1] = ts[:-1] != ts[1:]
        keep[-1] = True
        kept_idx = np.flatnonzero(keep)
        ts2 = ts[kept_idx]
        vs2 = vs[kept_idx]
    else:
        kept_idx = np.zeros(1, dtype=np.int64)
        ts2, vs2 = ts, vs
    m = int(ts2.size)
    if m > 1:
        rkeep = np.empty(m, dtype=bool)
        rkeep[0] = True
        rkeep[1:] = vs2[1:] != vs2[:-1]
        ridx = np.flatnonzero(rkeep)
    else:
        ridx = np.zeros(1, dtype=np.int64)
    run_ts = ts2[ridx]
    run_vs = vs2[ridx]
    raw_pos = kept_idx[ridx].astype(np.int64)
    raw_pos[0] = 0
    run_counts = np.diff(np.append(raw_pos, n))
    run_te = np.empty_like(run_ts)
    run_te[:-1] = run_ts[1:]
    run_te[-1] = max(tail_end or 0, int(run_ts[-1]) + 1)
    return run_ts, run_te, run_vs, run_counts

def _signal_stats(ts, vs, tail_end: int = 0):
    import numpy as np
    n = int(ts.size)
    if n == 0:
        return None, 0, 0, 0, 0, 0, 0, False, False, []

    if n > 1 and not bool((ts[:-1] <= ts[1:]).all()):
        order = np.argsort(ts, kind="stable")
        ts = ts[order]
        vs = vs[order]
    t_first, t_last = int(ts[0]), int(ts[-1])

    uniq_arr = np.unique(vs)
    if uniq_arr.size > UNIQUE_CAP:
        unique = UNIQUE_CAP + 1
        unique_capped = True
    else:
        unique = int(uniq_arr.size)
        unique_capped = False

    values = ([int(v) for v in uniq_arr]
              if not unique_capped and uniq_arr.size <= VALUES_LISTED else [])

    monotone = n > 1 and bool((vs[:-1] <= vs[1:]).all())

    min_dt = 0
    if n > 1:
        d = np.diff(ts)
        d = d[d > 0]
        if d.size:
            min_dt = int(d.min())

    run_arrays = rle_runs(ts, vs, max(t_last, tail_end))
    runs = int(run_arrays[0].size)
    return (run_arrays, t_first, t_last, n, runs, min_dt,
            unique, unique_capped, monotone, values)

def quantize_runs(run_ts, run_te, run_vs, run_cnt, stride: int
                  ) -> List[Tuple[int, int, int, int, int, int, int]]:
    import numpy as np
    r = int(run_ts.size)
    if r == 0:
        return []
    st = np.uint64(max(1, stride))
    is_long = (run_te - run_ts) >= st
    cell = run_ts // st
    boundary = np.empty(r, dtype=bool)
    boundary[0] = True
    if r > 1:
        boundary[1:] = is_long[1:] | is_long[:-1] | (cell[1:] != cell[:-1])
    starts = np.flatnonzero(boundary)
    ends = np.append(starts[1:], r) - 1

    gid = np.cumsum(boundary) - 1
    dur = (run_te - run_ts).astype(np.int64)
    order = np.lexsort((run_vs, gid))
    g2, v2, d2 = gid[order], run_vs[order], dur[order]
    pair_new = np.empty(r, dtype=bool)
    pair_new[0] = True
    pair_new[1:] = (g2[1:] != g2[:-1]) | (v2[1:] != v2[:-1])
    pstart = np.flatnonzero(pair_new)
    ptot = np.add.reduceat(d2, pstart)
    pg = g2[pstart]
    pv = v2[pstart]
    order2 = np.lexsort((-ptot, pg))
    pg2, pv2, pt2 = pg[order2], pv[order2], ptot[order2]
    gnew = np.empty(pg2.size, dtype=bool)
    gnew[0] = True
    gnew[1:] = pg2[1:] != pg2[:-1]
    gfirst = np.flatnonzero(gnew)
    pgid = np.cumsum(gnew) - 1
    rank = np.arange(pg2.size) - gfirst[pgid]
    ngroups = int(starts.size)
    dom_vs = np.zeros((ngroups, DOM_K), dtype=np.uint64)
    dom_ds = np.zeros((ngroups, DOM_K), dtype=np.float64)
    sel = rank < DOM_K
    dom_vs[pgid[sel], rank[sel]] = pv2[sel]
    dom_ds[pgid[sel], rank[sel]] = pt2[sel]
    gtot = np.add.reduceat(dur, starts).astype(np.float64)
    gtot[gtot <= 0] = 1.0
    dom_ws = np.minimum(
        DOM_W_ONE, np.round(dom_ds / gtot[:, None] * DOM_W_ONE)
    ).astype(np.int64)
    dom_vs_l = dom_vs.tolist()
    dom_ws_l = dom_ws.tolist()
    t_starts = run_ts[starts].tolist()
    t_ends   = run_te[ends].tolist()
    v_mins   = np.minimum.reduceat(run_vs, starts).tolist()
    v_maxs   = np.maximum.reduceat(run_vs, starts).tolist()
    v_lasts  = run_vs[ends].tolist()
    counts   = np.add.reduceat(run_cnt, starts).tolist()
    return [(t_starts[i], t_ends[i], v_mins[i], v_maxs[i],
             v_lasts[i], dom_vs_l[i][0], int(counts[i]),
             *dom_vs_l[i], *dom_ws_l[i])
            for i in range(len(starts))]

def _write_buckets(path: Path, buckets: List[Tuple[int, ...]]) -> int:
    with open(path, "wb") as f:
        for b in buckets:
            f.write(struct.pack(BUCKET_FMT, *b))
    return len(buckets) * BUCKET_SIZE

def _build_signals(prefix: str, out_dir_str: str, trace_t_end: int,
                   sig_ids: List[int]) -> Dict[int, SignalSummary]:
    out_dir = Path(out_dir_str)
    r = Reader(prefix, cache_events=False)
    out: Dict[int, SignalSummary] = {}
    for sig_id in sig_ids:
        sig_dir = out_dir / f"sig_{sig_id}"
        sig_dir.mkdir(exist_ok=True)
        levels: List[int] = []
        ts, vs = r.events_arrays(sig_id)
        (run_arrays, t_first, t_last, total, runs, min_dt,
         unique, unique_capped, monotone, values) = _signal_stats(
             ts, vs, trace_t_end)
        if run_arrays is not None:
            span = max(1, t_last - t_first)
            stride = max(1, -(-span // LEVEL_BASE_CELLS))
            for lvl in range(1, MAX_LEVEL + 1):
                cur = quantize_runs(*run_arrays, stride)
                levels.append(_write_buckets(sig_dir / f"L{lvl}.bin", cur))
                if len(cur) <= 1:
                    break
                stride *= LEVEL_RATIO
        out[sig_id] = SignalSummary(
            levels=levels, t_first=t_first, t_last=t_last,
            total=total, runs=runs, min_dt=min_dt,
            unique=unique, unique_capped=unique_capped,
            monotone=monotone, values=values,
        )
    return out

def build(prefix: str, root: str | None = None, *,
          progress: bool = False) -> Manifest:
    out_dir = cache_dir(prefix, root)
    out_dir.mkdir(parents=True, exist_ok=True)

    with Reader(prefix, cache_events=False) as r:
        sigs = r.signals()

        trace_t_end = max((seg.t_last for sig in sigs
                           for seg in r.pass_segments(sig)), default=0)

    signals_info: Dict[int, SignalSummary] = {}

    workers = max(1, min(len(sigs), (os.cpu_count() or 4) // 2))
    if progress:
        print(f"[indexer] building {len(sigs)} signals on "
              f"{workers} processes ...", flush=True)
    if workers > 1 and len(sigs) > 1:

        chunks = [c for c in (sigs[i::workers] for i in range(workers)) if c]
        worker = partial(_build_signals, prefix, str(out_dir), trace_t_end)
        with ProcessPoolExecutor(max_workers=len(chunks),
                                 mp_context=mp.get_context("spawn")) as ex:
            for part in ex.map(worker, chunks):
                signals_info.update(part)
    else:
        signals_info.update(
            _build_signals(prefix, str(out_dir), trace_t_end, sigs))

    st = os.stat(prefix + ".trace")
    manifest = Manifest(
        source_path=os.path.abspath(prefix),
        source_size=st.st_size,
        source_mtime=st.st_mtime,
        signals=signals_info,
    )
    with open(out_dir / "manifest.json", "w") as f:
        json.dump(manifest.to_dict(), f, indent=2)
    return manifest

def ensure(prefix: str, root: str | None = None, *,
           progress: bool = False) -> Manifest:
    out_dir = cache_dir(prefix, root)
    manifest_path = out_dir / "manifest.json"
    if manifest_path.exists():
        try:
            with open(manifest_path) as f:
                m = Manifest.from_dict(json.load(f))
            st = os.stat(prefix + ".trace")
            mtime_ok = (
                m.source_size == st.st_size
                and abs(m.source_mtime - st.st_mtime) < 1e-6
            )
            if mtime_ok and _content_matches(prefix, m):
                return m
        except Exception:
            pass
    return build(prefix, root, progress=progress)

def _content_matches(prefix: str, m: Manifest) -> bool:
    try:
        with Reader(prefix, cache_events=False) as r:
            sig_ids = set(r.signals())
            if sig_ids != set(m.signals.keys()):
                return False
            for sig_id in sig_ids:
                expected = sum(s.event_count for s in r.segments(sig_id))
                if m.signals[sig_id].total != expected:
                    return False
        return True
    except Exception:
        return False

@dataclass
class Bucket:
    t_start: int
    t_end: int
    v_min: int
    v_max: int
    v_last: int
    v_dom: int
    count: int
    dom_v: Tuple[int, ...]
    dom_w: Tuple[int, ...]

class IndexView:

    def __init__(self, prefix: str, manifest: Manifest, root: str | None = None):
        self._dir = cache_dir(prefix, root)
        self._manifest = manifest
        self._buckets: Dict[Tuple[int, int], List[Bucket]] = {}

        self._tstarts: Dict[Tuple[int, int], "object"] = {}

    def summary(self, signal_id: int) -> SignalSummary | None:
        return self._manifest.signals.get(signal_id)

    def levels(self, signal_id: int) -> List[int]:
        s = self._manifest.signals.get(signal_id)
        return list(s.levels) if s else []

    def total_events(self, signal_id: int) -> int:
        s = self._manifest.signals.get(signal_id)
        return s.total if s else 0

    def total_runs(self, signal_id: int) -> int:
        s = self._manifest.signals.get(signal_id)
        return s.runs if s else 0

    def time_range(self, signal_id: int) -> Tuple[int, int]:
        s = self._manifest.signals.get(signal_id)
        return (s.t_first, s.t_last) if s else (0, 0)

    def unique_values(self, signal_id: int) -> Tuple[int, bool]:
        s = self._manifest.signals.get(signal_id)
        return (s.unique, s.unique_capped) if s else (0, False)

    def read_level(self, signal_id: int, level: int) -> List[Bucket]:
        key = (signal_id, level)
        cached = self._buckets.get(key)
        if cached is not None:
            return cached
        levels = self.levels(signal_id)
        if level < 1 or level > len(levels):
            return []
        size = levels[level - 1]
        if size == 0:
            self._buckets[key] = []
            return []
        path = self._dir / f"sig_{signal_id}" / f"L{level}.bin"
        with open(path, "rb") as f:
            data = f.read()
        n = size // BUCKET_SIZE
        out: List[Bucket] = [None] * n
        unpack = struct.Struct(BUCKET_FMT).unpack_from
        for i in range(n):
            f = unpack(data, i * BUCKET_SIZE)
            out[i] = Bucket(*f[:7], f[7:7 + DOM_K], f[7 + DOM_K:])
        self._buckets[key] = out
        return out

    def _level_tstarts(self, signal_id: int, level: int):
        import numpy as np
        key = (signal_id, level)
        cached = self._tstarts.get(key)
        if cached is not None:
            return cached
        buckets = self.read_level(signal_id, level)
        arr = np.fromiter((b.t_start for b in buckets),
                          dtype=np.uint64, count=len(buckets))
        self._tstarts[key] = arr
        return arr

    def pick_level(self, signal_id: int, t0: int, t1: int, width: int) -> int:
        MAX_BUCKETS_PER_PIXEL = 2
        s = self._manifest.signals.get(signal_id)
        if not s or not s.levels:
            return 0
        span = max(1, s.t_last - s.t_first)
        if t1 > t0:
            frac = min(1.0, max(0.0, (t1 - t0) / span))
        else:
            frac = 1.0
        budget = max(1, width) * MAX_BUCKETS_PER_PIXEL
        k = len(s.levels)
        for cand in range(1, len(s.levels) + 1):
            n_k = s.levels[cand - 1] // BUCKET_SIZE
            if n_k * frac <= budget:
                k = cand
                break
        stride1 = max(1, -(-span // LEVEL_BASE_CELLS))
        req_px = max(1.0, (t1 - t0) / max(1, width))
        while k > 1 and stride1 * LEVEL_RATIO ** (k - 1) > req_px:
            k -= 1
        return k

    def query(self, signal_id: int, t0: int, t1: int, width: int
              ) -> Tuple[int, List[Bucket]]:
        import numpy as np
        lvl = self.pick_level(signal_id, t0, t1, width)
        if lvl == 0:
            return 0, []
        buckets = self.read_level(signal_id, lvl)
        if not buckets:
            return lvl, []
        tstarts = self._level_tstarts(signal_id, lvl)

        q = np.array([max(0, t0), max(0, t1)], dtype=np.uint64)
        lo_i, hi_i = np.searchsorted(tstarts, q, side="right")
        lo = max(0, int(lo_i) - 1)
        return lvl, buckets[lo:int(hi_i)]
