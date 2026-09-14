"""解 latch 的 `.trace`。

latch 的 .trace：一串信号段，文件尾 16 字节是 trailer 与 meta 两段的偏移。meta 段记模块树
（每个信号也是一个节点），trailer 段记每个信号有哪几段。

**打开文件是 O(信号数)，不是 O(全文件段数)**：trailer 里每个信号都跟着一串段偏移，把那些
偏移一个个解成 `Segment` 对象是 O(全文件段数) 的（实测 ~0.5 µs/段）—— 一份几十 GB 的波形
光打开就要几十秒、几 GB 内存。所以这里只记「这个信号的段表在哪儿、有几段」，真要读某个
信号时才去解它那几张段。
"""

from __future__ import annotations

import mmap
import struct
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple

MASK64 = (1 << 64) - 1
SEG_HEADER_FMT = "<4sQII QQQQQ"
SEG_HEADER_SIZE = struct.calcsize(SEG_HEADER_FMT)
FOOTER_SIZE = 16
VALUE_MASK = 0xFFFFFFFFFFFFFFFF


@dataclass
class Segment:
    signal_id: int
    body_len: int
    event_count: int
    t_first: int
    v_first: int
    t_last: int
    body_off: int


def zigzag_decode(u: int) -> int:
    """ZigZag 的反向：无符号折成有符号。meta 里父与自己的差要走它。"""
    return (u >> 1) ^ -(u & 1)


def varint_decode(buf, off: int) -> Tuple[int, int]:
    """无符号 LEB128。返回 (值, 下一个字节的偏移)。"""
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


class TraceReader:
    """signals() 列信号号，tree_paths() 把号翻成层次名，events() 给出一个信号的
    时间与值两列。事件第一次取时才解，解过的留着。"""

    def __init__(self, prefix: str):
        self.prefix = prefix
        self.path = prefix + ".trace"
        self.mods: Dict[int, Tuple[int, str]] = {}      # id → (父节点 id, 名字)
        self._seg_table: Dict[int, Tuple[int, int]] = {}   # 信号号 → (段表偏移, 段数)
        self._segs: Dict[int, List[Segment]] = {}          # 按需解的段，解过就留着
        self.cache: Dict[int, Tuple[list, list]] = {}
        self._seg_fmt = 1        # 段区布局，_load_meta 里按 meta 版本定
        self.fp = open(self.path, "rb")
        self.mm = None
        try:
            self._load()
        except Exception:
            self.close()
            raise

    def close(self) -> None:
        if self.mm is not None:
            self.mm.close()
            self.mm = None
        if self.fp is not None:
            self.fp.close()
            self.fp = None

    def __enter__(self) -> "TraceReader":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def signals(self) -> List[int]:
        return sorted(self._seg_table.keys())

    def tree_paths(self) -> Dict[int, str]:
        out: Dict[int, str] = {}

        def path_of(mid: int) -> str:
            if mid in out:
                return out[mid]
            pid, name = self.mods[mid]
            if pid == 0 or pid == mid or pid not in self.mods:
                out[mid] = name
            else:
                out[mid] = path_of(pid) + "." + name
            return out[mid]

        for mid in self.mods:
            path_of(mid)
        return out

    def segments(self, signal_id: int) -> List[Segment]:
        """这个信号的段表。第一次问的时候才解，之后就留着。"""
        got = self._segs.get(signal_id)
        if got is not None:
            return got
        place = self._seg_table.get(signal_id)
        if place is None:
            return []
        off, count = place
        offs = struct.unpack_from(f"<{count}Q", self.mm, off)
        segs = [self._load_segment(o, signal_id) for o in offs]
        self._segs[signal_id] = segs
        return segs

    def events(self, signal_id: int) -> Tuple[list, list]:
        got = self.cache.get(signal_id)
        if got is not None:
            return got
        ts: list = []
        vs: list = []
        for seg in self.one_pass(signal_id):
            for t, v in self.decode(seg):
                ts.append(t)
                vs.append(v)
        self.cache[signal_id] = (ts, vs)
        return ts, vs

    def one_pass(self, signal_id: int) -> List[Segment]:
        """同一个信号可能被写过几遍（时间回到前面就是新的一遍），取跨度最大的
        那一遍；跨度相同取后写的。"""
        segs = self.segments(signal_id)
        passes: List[List[Segment]] = []
        last_t = None
        for s in segs:
            if last_t is None or s.t_first < last_t:
                passes.append([])
            passes[-1].append(s)
            last_t = s.t_last
        best: List[Segment] = []
        best_span = -1
        for p in passes:
            span = p[-1].t_last - p[0].t_first
            if span >= best_span:
                best, best_span = p, span
        return best

    def decode(self, seg: Segment):
        yield seg.t_first, seg.v_first
        if seg.event_count <= 1:
            return
        mm = self.mm
        t, v = seg.t_first, seg.v_first
        pos = seg.body_off
        for _ in range(seg.event_count - 1):
            dt, pos = varint_decode(mm, pos)
            dvz, pos = varint_decode(mm, pos)
            t += dt
            v = (v + ((dvz >> 1) ^ -(dvz & 1))) & VALUE_MASK
            yield t, v
        if pos != seg.body_off + seg.body_len:
            raise ValueError(f"{self.path}: 信号 {seg.signal_id} 的段在 "
                             f"{seg.body_off} 处长度对不上")

    def _load(self) -> None:
        size = self.fp.seek(0, 2)
        if size < FOOTER_SIZE:
            raise ValueError(f"{self.path}: 只有 {size} B，放不下 16 B 的文件尾")
        self.mm = mmap.mmap(self.fp.fileno(), 0, access=mmap.ACCESS_READ)
        trailer_off, meta_off = struct.unpack_from("<QQ", self.mm,
                                                   size - FOOTER_SIZE)
        if not (0 <= trailer_off <= meta_off <= size - FOOTER_SIZE):
            raise ValueError(f"{self.path}: 文件尾的偏移不对")
        self._load_meta(meta_off, size - FOOTER_SIZE)
        self._load_trailer(trailer_off, meta_off)

    def _load_meta(self, start: int, end: int) -> None:
        mm = self.mm
        if mm[start:start + 4] != b"SMLM":
            raise ValueError(f"{self.path}: meta 段的标记不对")
        version, = struct.unpack_from("<I", mm, start + 4)
        # 段区与 trailer 的布局由 meta 版本定：>=3 是 varint 瘦身过的那一套。
        self._seg_fmt = version
        cnt, = struct.unpack_from("<Q", mm, start + 8)
        off = start + 16
        if version == 1:
            # 每条记录自带名字，18 字节定长加名字本身。
            for _ in range(cnt):
                mid, pid = struct.unpack_from("<QQ", mm, off)
                nlen, = struct.unpack_from("<H", mm, off + 16)
                name = bytes(mm[off + 18:off + 18 + nlen]).decode("utf-8")
                self.mods[mid] = (pid, name)
                off += 18 + nlen
        elif version >= 2:
            # 名字表在前（v2 与 v3 的 meta 正文同一套，差别在段区与 trailer），记录里只放下标；id 与 parent 都走与自己的差。
            name_cnt, = struct.unpack_from("<I", mm, off)
            off += 4
            names = []
            for _ in range(name_cnt):
                nlen, = struct.unpack_from("<H", mm, off)
                off += 2
                names.append(bytes(mm[off:off + nlen]).decode("utf-8"))
                off += nlen
            prev = 0
            for _ in range(cnt):
                d, off = varint_decode(mm, off)
                z, off = varint_decode(mm, off)
                name_idx, off = varint_decode(mm, off)
                mid = prev + d
                self.mods[mid] = (mid + zigzag_decode(z), names[name_idx])
                prev = mid
        else:
            raise ValueError(f"{self.path}: 不认识的 meta 版本 {version}")
        if off != end:
            raise ValueError(f"{self.path}: meta 段末尾多出字节")

    def _load_trailer(self, start: int, end: int) -> None:
        mm = self.mm
        if mm[start:start + 4] != b"SMLT":
            raise ValueError(f"{self.path}: trailer 段的标记不对")
        signal_cnt, = struct.unpack_from("<I", mm, start + 4)
        cur = start + 8
        prev_id = 0
        for _ in range(signal_cnt):
            if self._seg_fmt >= 3:
                # 信号号与段数走 varint；段偏移仍是定长绝对偏移，所以拿到位置
                # 就能直接切，不用顺着链解。
                delta, cur = varint_decode(mm, cur)
                seg_count, cur = varint_decode(mm, cur)
                sig_id = prev_id + delta
            else:
                sig_id, seg_count = struct.unpack_from("<QI", mm, cur)
                cur += 12
            self._seg_table[sig_id] = (cur, seg_count)   # 段表先只记位置
            cur += 8 * seg_count
            prev_id = sig_id
        if cur != end:
            raise ValueError(f"{self.path}: trailer 段末尾多出字节")

    def _load_segment(self, off: int, signal_id: int) -> Segment:
        """`signal_id` 由调用方给：段头里不写它了，trailer 那边知道段属于谁。"""
        mm = self.mm
        if self._seg_fmt >= 3:
            cur = off
            body_len, cur = varint_decode(mm, cur)
            event_count, cur = varint_decode(mm, cur)
            t_first, cur = varint_decode(mm, cur)
            v_first, cur = varint_decode(mm, cur)
            t_last, cur = varint_decode(mm, cur)
            # v_min / v_max 这一层不用，但偏移要跟着走 —— varint_decode 返回
            # 新偏移，不会原地推进。
            _, cur = varint_decode(mm, cur)
            _, cur = varint_decode(mm, cur)
            return Segment(signal_id, body_len, event_count, t_first,
                           zigzag_decode(v_first) & MASK64, t_last, cur)
        (magic, sid, body_len, event_count, t_first, v_first, t_last,
         v_min, v_max) = struct.unpack_from(SEG_HEADER_FMT, mm, off)
        if magic != b"SMLS":
            raise ValueError(f"{self.path}: {off} 处的段标记不对")
        return Segment(sid, body_len, event_count, t_first, v_first,
                       t_last, off + SEG_HEADER_SIZE)


def is_latch_trace(p: Path) -> bool:
    """只认 latch 写的波形。别的工具也用 .trace 这个后缀（3rdparties 里就有），
    选中那种会在读的时候崩。认法照上面的 TraceReader：文件尾 16 字节是两个偏移，
    meta 段以 SMLM 开头。"""
    try:
        size = p.stat().st_size
        if size < 16:
            return False
        with open(p, "rb") as f:
            f.seek(size - 16)
            trailer_off, meta_off = struct.unpack("<QQ", f.read(16))
            if not (0 <= trailer_off <= meta_off <= size - 16):
                return False
            f.seek(meta_off)
            return f.read(4) == b"SMLM"
    except OSError:
        return False


def list_traces(cwd: Path) -> List[Path]:
    """当前目录下所有 latch 的波形，新的排前面。"""
    found = [p for p in cwd.rglob("*.trace") if is_latch_trace(p)]
    return sorted(found, key=lambda p: -p.stat().st_mtime)


def show(p: Path, cwd: Path) -> str:
    try:
        return str(p.relative_to(cwd))
    except ValueError:
        return str(p)


def pick(traces: List[Path], cwd: Path) -> Path:
    """列出来让人挑一份。管道里跑就挑最新那份。"""
    if len(traces) == 1:
        return traces[0]
    if not sys.stdin.isatty():
        print(f"[tracetto] 有 {len(traces)} 份波形，不在终端里，挑最新的一份："
              f"{show(traces[0], cwd)}", file=sys.stderr)
        return traces[0]
    print(f"[tracetto] {cwd} 下有 {len(traces)} 份波形：", file=sys.stderr)
    iw = len(str(len(traces) - 1))
    nw = max(len(show(p, cwd)) for p in traces)
    for i, p in enumerate(traces):
        stamp = datetime.fromtimestamp(p.stat().st_mtime).strftime("%m-%d %H:%M")
        size = p.stat().st_size / 1024
        mark = "  ← 最新" if i == 0 else ""
        print(f"  [{i:>{iw}}] {show(p, cwd):<{nw}s}  {stamp}  {size:7.0f} KB{mark}",
              file=sys.stderr)
    while True:
        try:
            raw = input(f"[tracetto] 选一个 [0-{len(traces) - 1}，直接回车用 0，q 退出]："
                        ).strip()
        except (EOFError, KeyboardInterrupt):
            sys.exit("\n[tracetto] 算了")
        if raw == "":
            return traces[0]
        if raw in ("q", "Q"):
            sys.exit("[tracetto] 算了")
        if raw.isdigit() and int(raw) < len(traces):
            return traces[int(raw)]
        print(f"[tracetto] '{raw}' 不在 0～{len(traces) - 1} 里，重来", file=sys.stderr)


def resolve_trace(arg: Optional[str]) -> Path:
    """把命令行给的那一截认成一份波形。不给就把当前目录下的列出来挑。"""
    cwd = Path.cwd()
    if arg:
        for cand in (Path(arg), Path(arg + ".trace")):
            if cand.is_file():
                return cand
        hits = [p for p in list_traces(cwd) if arg in str(p)]
        if not hits:
            sys.exit(f"[tracetto] {cwd} 下没有名字里带 '{arg}' 的 .trace")
        return pick(hits, cwd)

    traces = list_traces(cwd)
    if not traces:
        sys.exit(f"[tracetto] {cwd} 下一个 .trace 都没有")
    return pick(traces, cwd)
