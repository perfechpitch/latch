from __future__ import annotations

import mmap
import struct
from dataclasses import dataclass, field
from typing import Iterator, List, Dict, Tuple

def _decode_varint_stream(arr):
    import numpy as np
    if arr.size == 0:
        return np.empty(0, dtype=np.uint64)
    low = arr & 0x7f
    is_term = (arr & 0x80) == 0
    is_start = np.empty(arr.size, dtype=bool)
    is_start[0] = True
    is_start[1:] = is_term[:-1]
    value_idx = np.cumsum(is_start, dtype=np.int64) - 1
    starts = np.where(is_start)[0]
    byte_pos = (np.arange(arr.size, dtype=np.uint64)
                - starts[value_idx].astype(np.uint64))
    contribs = low.astype(np.uint64) << (byte_pos * np.uint64(7))
    return np.add.reduceat(contribs, starts)

def varint_decode(buf: bytes, off: int) -> Tuple[int, int]:
    v = 0
    shift = 0
    pos = off
    while True:
        b = buf[pos]
        v |= (b & 0x7F) << shift
        pos += 1
        if (b & 0x80) == 0:
            return v, pos
        shift += 7
        if shift >= 64:
            raise ValueError(f"varint at offset {off} exceeds 64 bits")

def zigzag_decode(u: int) -> int:
    return (u >> 1) ^ -(u & 1)

@dataclass
class ModuleRecord:
    id: int
    parent_id: int
    name: str

@dataclass
class SegmentHeader:
    signal_id: int
    body_len: int
    event_count: int
    t_first: int
    v_first: int
    t_last: int
    v_min: int
    v_max: int
    body_off: int

_SEG_HEADER_FMT = "<4sQII QQQQQ"
_SEG_HEADER_SIZE = struct.calcsize(_SEG_HEADER_FMT)
assert _SEG_HEADER_SIZE == 60, _SEG_HEADER_SIZE

class Reader:

    _FOOTER_SIZE = 16

    def __init__(self, prefix: str, cache_events: bool = True):
        self.prefix = prefix
        self._modules: List[ModuleRecord] = []
        self._segments_by_sig: Dict[int, List[SegmentHeader]] = {}
        self._mm: mmap.mmap | None = None
        self._fp = None

        self._events_cache: Dict[int, Tuple[list, list]] = {}

        self._seg_cache: Dict[Tuple[int, int], Tuple] = {}

        self._pass_indices: Dict[int, List[int]] = {}
        self._open()
        if cache_events:
            for sig_id in self.signals():
                self._ensure_events(sig_id)

    def close(self) -> None:
        if self._mm is not None:
            self._mm.close()
            self._mm = None
        if self._fp is not None:
            self._fp.close()
            self._fp = None

    def __enter__(self) -> "Reader":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def modules(self) -> List[ModuleRecord]:
        return list(self._modules)

    def signals(self) -> List[int]:
        return sorted(self._segments_by_sig.keys())

    def segments(self, signal_id: int) -> List[SegmentHeader]:
        return list(self._segments_by_sig.get(signal_id, []))

    def pass_segment_indices(self, signal_id: int) -> List[int]:
        cached = self._pass_indices.get(signal_id)
        if cached is not None:
            return cached
        segs = self._segments_by_sig.get(signal_id, [])
        passes: List[List[int]] = []
        last_t = None
        for i, s in enumerate(segs):

            if last_t is None or s.t_first < last_t:
                passes.append([])
            passes[-1].append(i)
            last_t = s.t_last
        best: List[int] = []
        best_span = -1
        for p in passes:
            span = segs[p[-1]].t_last - segs[p[0]].t_first
            if span >= best_span:
                best, best_span = p, span
        self._pass_indices[signal_id] = best
        return best

    def pass_segments(self, signal_id: int) -> List[SegmentHeader]:
        segs = self._segments_by_sig.get(signal_id, [])
        return [segs[i] for i in self.pass_segment_indices(signal_id)]

    def iter_events(self, signal_id: int) -> Iterator[Tuple[int, int]]:
        ts, vs = self._ensure_events(signal_id)
        for i in range(len(ts)):
            yield ts[i], vs[i]

    def events(self, signal_id: int) -> Tuple[list, list]:
        return self._ensure_events(signal_id)

    def event_count(self, signal_id: int) -> int:
        return sum(s.event_count for s in self._segments_by_sig.get(signal_id, ()))

    def events_arrays(self, signal_id: int):
        import numpy as np
        out_ts: list = []
        out_vs: list = []
        for seg in self.pass_segments(signal_id):
            ts, vs = self._decode_segment_arrays(seg)
            out_ts.append(ts)
            out_vs.append(vs)
        if not out_ts:
            return np.empty(0, dtype=np.uint64), np.empty(0, dtype=np.uint64)
        return np.concatenate(out_ts), np.concatenate(out_vs)

    def _decode_segment_cached(self, signal_id: int, seg_idx: int):
        key = (signal_id, seg_idx)
        cached = self._seg_cache.get(key)
        if cached is not None:
            return cached
        seg = self._segments_by_sig[signal_id][seg_idx]
        result = self._decode_segment_arrays(seg)
        self._seg_cache[key] = result
        return result

    def warm_all_segments(self, max_workers: int = 4) -> None:
        import concurrent.futures

        def warm(sig_id: int) -> None:
            segs = self._segments_by_sig.get(sig_id, ())
            for i in range(len(segs)):
                self._decode_segment_cached(sig_id, i)

        with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as ex:
            list(ex.map(warm, self.signals()))

    def drop_segment_cache(self, signal_id: int) -> None:
        for i in range(len(self._segments_by_sig.get(signal_id, ()))):
            self._seg_cache.pop((signal_id, i), None)

    def events_window(self, signal_id: int, t0: int, t1: int):
        import numpy as np
        all_segs = self._segments_by_sig.get(signal_id, ())
        pass_idx = self.pass_segment_indices(signal_id)
        if not pass_idx:
            return np.empty(0, dtype=np.uint64), np.empty(0, dtype=np.uint64)

        if t0 < 0: t0 = 0
        if t1 < 0: t1 = 0
        keep = [i for i in pass_idx
                if all_segs[i].t_first <= t1 and all_segs[i].t_last >= t0]

        before = [i for i in pass_idx if all_segs[i].t_last < t0]
        if before:
            keep.append(max(before, key=lambda i: all_segs[i].t_last))
        after = [i for i in pass_idx if all_segs[i].t_first > t1]
        if after:
            keep.append(min(after, key=lambda i: all_segs[i].t_first))
        segs = all_segs

        q = np.array([t0, t1], dtype=np.uint64)
        parts_t: list = []
        parts_v: list = []
        for i in sorted(set(keep)):
            ts_seg, vs_seg = self._decode_segment_cached(signal_id, i)
            ia, ib = np.searchsorted(ts_seg, q, side="right")
            a = max(0, int(ia) - 1)
            b = min(ts_seg.size, int(ib) + 1)
            if a < b:
                parts_t.append(ts_seg[a:b])
                parts_v.append(vs_seg[a:b])
        if not parts_t:
            return np.empty(0, dtype=np.uint64), np.empty(0, dtype=np.uint64)
        if len(parts_t) == 1:
            return parts_t[0], parts_v[0]
        return np.concatenate(parts_t), np.concatenate(parts_v)

    def _decode_segment_arrays(self, seg: SegmentHeader):
        import numpy as np
        n = seg.event_count
        ts = np.empty(n, dtype=np.uint64)
        vs = np.empty(n, dtype=np.uint64)
        if n == 0:
            return ts, vs
        ts[0] = seg.t_first
        vs[0] = seg.v_first
        if n <= 1:
            return ts, vs
        assert self._mm is not None
        body = self._mm[seg.body_off : seg.body_off + seg.body_len]
        arr = np.frombuffer(body, dtype=np.uint8)

        if arr.size == 2 * (n - 1) and arr.size > 0 and int(arr.max()) < 128:

            dts  = arr[0::2].astype(np.uint64)
            dvzs = arr[1::2].astype(np.uint64)
        else:
            decoded = _decode_varint_stream(arr)
            if decoded.size != 2 * (n - 1):
                raise ValueError(
                    f"segment {seg.signal_id}@{seg.body_off}: decoded "
                    f"{decoded.size} varints, expected {2 * (n - 1)}"
                )
            dts  = decoded[0::2]
            dvzs = decoded[1::2]

        dvz_i = dvzs.astype(np.int64)
        dvs = (dvzs >> np.uint64(1)).astype(np.int64) ^ (-(dvz_i & 1))

        ts[1:] = (seg.t_first + np.cumsum(dts.astype(np.int64))).astype(np.uint64)
        vs_acc = np.cumsum(dvs) + np.int64(seg.v_first)
        vs[1:] = vs_acc.astype(np.uint64)
        return ts, vs

    def _ensure_events(self, signal_id: int) -> Tuple[list, list]:
        cached = self._events_cache.get(signal_id)
        if cached is not None:
            return cached
        ts: list = []
        vs: list = []
        for seg in self.pass_segments(signal_id):
            for t, v in self._iter_segment(seg):
                ts.append(t)
                vs.append(v)
        self._events_cache[signal_id] = (ts, vs)
        return self._events_cache[signal_id]

    def tree_paths(self) -> Dict[int, str]:
        by_id = {m.id: m for m in self._modules}
        out: Dict[int, str] = {}

        def path(mid: int) -> str:
            if mid in out:
                return out[mid]
            m = by_id[mid]
            if m.parent_id == 0 or m.parent_id == mid or m.parent_id not in by_id:
                out[mid] = m.name
            else:
                out[mid] = path(m.parent_id) + "." + m.name
            return out[mid]

        for mid in by_id:
            path(mid)
        return out

    def _open(self) -> None:
        path = self.prefix + ".trace"
        self._fp = open(path, "rb")
        size = self._fp.seek(0, 2)
        self._fp.seek(0)
        if size < self._FOOTER_SIZE:
            raise ValueError(f"{path}: too small ({size}B) — needs ≥16B footer")
        self._mm = mmap.mmap(self._fp.fileno(), 0, access=mmap.ACCESS_READ)
        mm = self._mm
        trailer_off, meta_off = struct.unpack_from(
            "<QQ", mm, size - self._FOOTER_SIZE)

        if not (0 <= trailer_off <= meta_off <= size - self._FOOTER_SIZE):
            raise ValueError(
                f"{path}: bad footer offsets trailer={trailer_off} "
                f"meta={meta_off} size={size}")

        self._parse_meta(meta_off, size - self._FOOTER_SIZE)
        self._parse_trailer(trailer_off, meta_off)

    def _parse_meta(self, start: int, end: int) -> None:
        mm = self._mm
        if mm[start : start + 4] != b"SMLM":
            raise ValueError(f"{self.prefix}.trace: bad meta magic at {start}")
        version, = struct.unpack_from("<I", mm, start + 4)
        if version != 1:
            raise ValueError(
                f"{self.prefix}.trace: unsupported meta version {version}")
        cnt, = struct.unpack_from("<Q", mm, start + 8)
        off = start + 16
        for _ in range(cnt):
            mid, pid = struct.unpack_from("<QQ", mm, off)
            nlen, = struct.unpack_from("<H", mm, off + 16)
            name = bytes(mm[off + 18 : off + 18 + nlen]).decode("utf-8")
            self._modules.append(ModuleRecord(id=mid, parent_id=pid, name=name))
            off += 18 + nlen
        if off != end:
            raise ValueError(
                f"{self.prefix}.trace: meta trailing bytes at {off}/{end}")

    def _parse_trailer(self, start: int, end: int) -> None:
        mm = self._mm
        if mm[start : start + 4] != b"SMLT":
            raise ValueError(
                f"{self.prefix}.trace: bad trailer magic at {start}")
        signal_cnt, = struct.unpack_from("<I", mm, start + 4)
        cur = start + 8
        for _ in range(signal_cnt):
            sig_id, seg_count = struct.unpack_from("<QI", mm, cur)
            cur += 12
            seg_offs = struct.unpack_from(f"<{seg_count}Q", mm, cur)
            cur += seg_count * 8
            self._segments_by_sig[sig_id] = [
                self._read_segment_header(o) for o in seg_offs
            ]
        if cur != end:
            raise ValueError(
                f"{self.prefix}.trace: trailer trailing bytes at {cur}/{end}")

    def _read_segment_header(self, off: int) -> SegmentHeader:
        assert self._mm is not None
        (magic, signal_id, body_len, event_count,
         t_first, v_first, t_last, v_min, v_max) = struct.unpack_from(
            _SEG_HEADER_FMT, self._mm, off)
        if magic != b"SMLS":
            raise ValueError(f"bad segment magic at offset {off}: {magic!r}")
        return SegmentHeader(
            signal_id=signal_id, body_len=body_len, event_count=event_count,
            t_first=t_first, v_first=v_first, t_last=t_last,
            v_min=v_min, v_max=v_max,
            body_off=off + _SEG_HEADER_SIZE,
        )

    def _iter_segment(self, seg: SegmentHeader) -> Iterator[Tuple[int, int]]:
        assert self._mm is not None
        yield (seg.t_first, seg.v_first)
        if seg.event_count <= 1:
            return

        t = seg.t_first
        v = seg.v_first
        pos = seg.body_off
        end = seg.body_off + seg.body_len
        mm = self._mm
        for _ in range(seg.event_count - 1):
            dt, pos = varint_decode(mm, pos)
            dvz, pos = varint_decode(mm, pos)
            t += dt
            dv = zigzag_decode(dvz)

            v = (v + dv) & 0xFFFFFFFFFFFFFFFF
            yield (t, v)
        if pos != end:
            raise ValueError(
                f"segment {seg.signal_id}@{seg.body_off}: "
                f"body has {pos - seg.body_off} consumed bytes, expected {seg.body_len}"
            )
