"""索引的格式与查询。

一份波形一份索引，落在波形旁边：`<波形名>.tracetto-index/`。

```
moe_lpu.tracetto-index/
  manifest.json          格式版本 + 波形身份 + 树 + 每条行的段数
  cores/<core_idx>.bin   一个 core 九条行放一个文件（一个 core 的九条行永远一起被看）
  .building.<pid>.<xx>/  建索引时的临时目录，建成再 rename 过去
```

段记录是**变长**的：`varint(gap)` `varint(dur)` `3 字节标签`。
- `gap = t0 - prev_t1`（空档不存 —— 段与段之间没有东西就是空档），首个段的 `prev_t1`
  取它所在块的 `t_base`；
- `dur = t1 - t0`，恒 ≥ 1；
- 标签内联定长 3 字节 `u16 user + u8 task`，`(0xFFFF, 0xFF)` 表示「认不出是哪一笔」。
  单元名由行号推出来、拍数就是 `dur`，都不用存 —— 于是建索引没有全局状态，
  并行到什么程度结果都一样。

变长没法二分，所以每 `SEG_BLK` 段记一条 `blk` 表（段起点 + 字节偏移）当定位用；
段数特别多的行再写一层定长稠密的 `L1`（按 `t // l1_cell` 直接下标），用来在缩得很远时
一次切一片、不用解任何 varint。**服务端因此不做逐段循环**：两种模式都是两次二分 + 一次
pread，解 varint 的活全在浏览器里。
"""

from __future__ import annotations

import bisect
import hashlib
import json
import os
import struct
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from . import spans as S

INDEX_FORMAT = 3   # 2：manifest 改成七行显示 + 九条通道；3：段上印的单元名
                   #    带上行名（TS-DTE / CORE-DTE …）。索引本体都没变，
                   #    升版本只是为了把 manifest 里那份展示用的数据换掉 ——
                   #    复用判据只看波形身份，改展示不会自动重建。
INDEX_SUFFIX = ".tracetto-index"
MANIFEST_NAME = "manifest.json"
CORES_DIR = "cores"

CORE_MAGIC = b"TCTC"
CORE_HDR_FMT = "<4sHHIIQ" + "Q" * S.LANES_PER_CORE  # magic, format, flags, core_idx, nlanes, t_end, lane_off[9]
LANE_FMT = "<IIQQBBHIQIIQQQ"                       # 见下
LANE_HDR_SIZE = struct.calcsize(LANE_FMT)
BLK_FMT = "<QQQ"                                   # t_base, t0_first, 字节偏移
L1_STRIDE = 1 + 3                                   # u8 density + 3 字节标签
TAG_SIZE = 3

SEG_BLK = 128            # 每多少段写一条 blk
L1_MIN_SEG = 4096        # 段数超过这个才写 L1
L1_MAX_CELL = 4096       # L1 最多这么多格

# 标签哨兵：认不出是哪一笔的段。
TAG_NONE = (0xFFFF, 0xFF)


# ── 波形身份与目录 ──

def head_fp(path: Path) -> str:
    """头尾各 64 KB + 大小的 sha1。

    只看 size/mtime 挡不住「同大小同 mtime 被覆盖」；多读这 128 KB 换一个几乎不漏的判据。
    """
    size = path.stat().st_size
    h = hashlib.sha1()
    h.update(str(size).encode())
    with open(path, "rb") as f:
        h.update(f.read(65536))
        if size > 65536:
            f.seek(max(0, size - 65536))
            h.update(f.read(65536))
    return h.hexdigest()


def source_identity(trace: Path) -> Dict:
    """波形身份。realpath + dev/ino 是为了「同一份波形从两个路径进」也能复用同一份索引。"""
    st = os.stat(trace)
    return {
        "realpath": os.path.realpath(trace),
        "size": st.st_size,
        "mtime_ns": st.st_mtime_ns,
        "dev": st.st_dev,
        "ino": st.st_ino,
        "head_fp": head_fp(trace),
    }


def identity_matches(trace: Path, was: Dict) -> bool:
    """波形还是当初那份吗。stat 那几项先比（便宜），最后才比 head_fp。"""
    try:
        st = os.stat(trace)
    except OSError:
        return False
    if (os.path.realpath(trace) != was.get("realpath")
            or st.st_size != was.get("size")
            or st.st_mtime_ns != was.get("mtime_ns")
            or st.st_dev != was.get("dev")
            or st.st_ino != was.get("ino")):
        return False
    try:
        return head_fp(trace) == was.get("head_fp")
    except OSError:
        return False


def index_dir(prefix: str) -> Path:
    """`<波形>.tracetto-index/`。"""
    return Path(str(prefix) + INDEX_SUFFIX)


def build_id(ident: Dict) -> str:
    """给前端认的 build 号：波形身份变了它就变，前端据此清缓存。"""
    raw = f"{ident['realpath']}|{ident['size']}|{ident['mtime_ns']}|{ident['ino']}|{ident['head_fp']}"
    return hashlib.sha1(raw.encode()).hexdigest()[:12]


# ── 编码：段 ──

def encode_varint(n: int, out: bytearray) -> None:
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return


def decode_varint(buf, off: int) -> Tuple[int, int]:
    v = 0
    shift = 0
    while True:
        b = buf[off]
        v |= (b & 0x7F) << shift
        off += 1
        if (b & 0x80) == 0:
            return v, off
        shift += 7
        if shift >= 64:
            raise ValueError("varint 超过 64 位")


def tag_of(user: int, task: int) -> bytes:
    """段标签：3 字节。认得出是哪一笔就写 user/task，认不出写哨兵。"""
    if user < 0 or task < 0:
        user, task = TAG_NONE
    return struct.pack("<HB", user & 0xFFFF, task & 0xFF)


def tag_read(buf, off: int) -> Tuple[int, int]:
    user, task = struct.unpack_from("<HB", buf, off)
    return (-1, -1) if (user, task) == TAG_NONE else (user, task)


def encode_segments(seg_spans: List[List[int]]) -> Tuple[bytes, List[int], List[Tuple[int, int, int]]]:
    """一条行的段 → (段流字节, 每条 blk 的段号, blk 表)。

    段流 = `varint(gap) varint(dur) tag` 依次相接；blk 表每 `SEG_BLK` 段一条，
    记 `(t_base, t0_first, 字节偏移)`，其中 `t_base` 是这一块**首段之前那一刻**
    （也就是上一段的末拍），客户端从头解 gap 时要拿它当起点。
    """
    out = bytearray()
    blks: List[Tuple[int, int, int]] = []
    blk_at: List[int] = []
    prev_t1 = 0
    for i, (t0, t1, user, task) in enumerate(seg_spans):
        dur = t1 - t0
        if dur < 1:
            dur = 1                      # 长度 0 的段撑成一拍，不跳过
        if i % SEG_BLK == 0:
            blk_at.append(i)
            blks.append((prev_t1, t0, len(out)))
        encode_varint(max(0, t0 - prev_t1), out)
        encode_varint(dur, out)
        out += tag_of(user, task)
        prev_t1 = t0 + dur
    return bytes(out), blk_at, blks


def decode_segments(payload, t_base: int, count: int) -> List[List[int]]:
    """段流 → `[[t0, t1, user, task], ...]`。自检与 --dump 用；前端在 JS 里另解一遍。"""
    out: List[List[int]] = []
    off = 0
    prev_t1 = t_base
    for _ in range(count):
        if off + 2 + TAG_SIZE > len(payload):
            break                    # 切多了（或者上游给了个截断的切片）：能解多少解多少
        gap, off = decode_varint(payload, off)
        dur, off = decode_varint(payload, off)
        user, task = tag_read(payload, off)
        off += TAG_SIZE
        t0 = prev_t1 + gap
        out.append([t0, t0 + dur, user, task])
        prev_t1 = t0 + dur
    return out


def encode_l1(seg_spans: List[List[int]], t_end: int) -> Tuple[bytes, int, int]:
    """粗层：定长稠密的格子，一格一个 `u8 密度 + 3 字节标签`。

    密度 = 这一格里忙了多少（0～255）；标签取格里占时最长的那一段。格数封顶 `L1_MAX_CELL`，
    所以这一层的体积与波形多长无关。
    """
    n_cell = min(L1_MAX_CELL, max(1, len(seg_spans) // 4))
    cell = max(1, -(-t_end // n_cell))
    n_cell = max(1, -(-t_end // cell))
    dense = bytearray(n_cell * L1_STRIDE)
    for c in range(n_cell):
        dense[c * L1_STRIDE + 1:c * L1_STRIDE + 4] = tag_of(*TAG_NONE)
    busy = [0] * n_cell
    best = [0] * n_cell
    for t0, t1, user, task in seg_spans:
        c0, c1 = t0 // cell, min(n_cell - 1, max(t0, t1 - 1) // cell)
        for c in range(c0, c1 + 1):
            lo = max(t0, c * cell)
            hi = min(t1, (c + 1) * cell)
            d = hi - lo
            if d <= 0:
                continue
            busy[c] += d
            if d > best[c]:
                best[c] = d
                dense[c * L1_STRIDE + 1:c * L1_STRIDE + 4] = tag_of(user, task)
    for c in range(n_cell):
        dense[c * L1_STRIDE] = min(255, round(255 * busy[c] / cell))
    return bytes(dense), cell, n_cell


# ── 编码：一个 core 的文件 ──

def encode_core(core_idx: int, lane_spans: List[List[List[int]]],
                lane_base: int, t_end: int) -> bytes:
    """一个 core 的九条行 → 一个完整文件的字节。"""
    n_lanes = S.LANES_PER_CORE
    if len(lane_spans) != n_lanes:
        raise ValueError(f"core {core_idx} 只给了 {len(lane_spans)} 条行")
    head_size = struct.calcsize(CORE_HDR_FMT)
    body = bytearray()
    lane_off: List[int] = []
    for row, seg_spans in enumerate(lane_spans):
        base = head_size + len(body)          # 这条行在文件里的绝对起点
        lane_off.append(base)
        blob = bytearray()
        if seg_spans:
            stream, _blk_at, blks = encode_segments(seg_spans)
            n_blk = len(blks) if len(seg_spans) > SEG_BLK else 0
            blk_len = n_blk * struct.calcsize(BLK_FMT)
            l1 = b""
            l1_cell = l1_cnt = 0
            if len(seg_spans) > L1_MIN_SEG:
                l1, l1_cell, l1_cnt = encode_l1(seg_spans, t_end)
            flags = (1 if n_blk else 0) | (2 if l1 else 0)
            # 头里存的是**文件里的绝对偏移** —— 读的时候直接 seek，不用再加行起点。
            # 段流是最后一段，它的起点要先按前两段的长度算出来，blk 表才知道该写什么偏移。
            blk_off = base + LANE_HDR_SIZE
            l1_off = blk_off + blk_len
            seg_off = l1_off + len(l1)
            seg_end = seg_off + len(stream)
            blk_bytes = b"".join(struct.pack(BLK_FMT, tb, t0f, seg_off + o)
                                 for tb, t0f, o in blks) if n_blk else b""
            blob += struct.pack(
                LANE_FMT, lane_base + row, len(seg_spans),
                seg_spans[0][0], seg_spans[-1][1], flags, 0,
                SEG_BLK if n_blk else 0, n_blk, blk_off,
                l1_cell, l1_cnt, l1_off, seg_off, seg_end)
            blob += blk_bytes
            blob += l1
            blob += stream
        else:
            blob += struct.pack(LANE_FMT, lane_base + row, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0)
        body += blob
    head = struct.pack(CORE_HDR_FMT, CORE_MAGIC, INDEX_FORMAT, 0, core_idx,
                       n_lanes, t_end, *lane_off)
    return bytes(head + body)


class LaneView:
    """一条行在索引里的样子（只读头，不碰段流）。"""

    __slots__ = ("lane_id", "row", "seg_cnt", "t_first", "t_last", "flags",
                 "blk_span", "blk_cnt", "blk_off", "l1_cell", "l1_cnt",
                 "l1_off", "seg_off", "seg_end")

    def __init__(self, lane_id: int, row: int, fields) -> None:
        (self.lane_id, self.seg_cnt, self.t_first, self.t_last, self.flags,
         _pad, self.blk_span, self.blk_cnt, self.blk_off, self.l1_cell,
         self.l1_cnt, self.l1_off, self.seg_off, self.seg_end) = fields
        self.row = row

    def has_blk(self) -> bool:
        return bool(self.flags & 1)

    def has_l1(self) -> bool:
        return bool(self.flags & 2)


class CoreFile:
    """一个 core 的索引文件：头 + 九条行的头 + 按窗口切一段字节出来。"""

    def __init__(self, path: Path):
        self.path = path
        self.fp = open(path, "rb")
        head = self.fp.read(struct.calcsize(CORE_HDR_FMT))
        if head[:4] != CORE_MAGIC:
            raise ValueError(f"{path}: 不是 tracetto 的索引文件")
        fields = struct.unpack(CORE_HDR_FMT, head)
        magic, fmt, _flags, self.core_idx, nlanes, self.t_end = fields[:6]
        if fmt != INDEX_FORMAT:
            raise ValueError(f"{path}: 索引格式 {fmt} != {INDEX_FORMAT}，重建一次")
        self.nlanes = nlanes
        offs = fields[6:]
        self.lanes: List[Optional[LaneView]] = []
        for row in range(nlanes):
            if offs[row] == 0:
                self.lanes.append(None)
                continue
            self.fp.seek(offs[row])
            blob = self.fp.read(LANE_HDR_SIZE)
            self.lanes.append(LaneView(row, row, struct.unpack(LANE_FMT, blob)))

    def close(self) -> None:
        if self.fp is not None:
            self.fp.close()
            self.fp = None

    def lane(self, row: int) -> Optional[LaneView]:
        return self.lanes[row] if 0 <= row < len(self.lanes) else None

    def read(self, off: int, end: int) -> bytes:
        self.fp.seek(off)
        return self.fp.read(end - off)

    def blk_table(self, lane: LaneView) -> List[Tuple[int, int, int]]:
        raw = self.read(lane.blk_off, lane.blk_off + lane.blk_cnt * struct.calcsize(BLK_FMT))
        return [struct.unpack_from(BLK_FMT, raw, i * struct.calcsize(BLK_FMT))
                for i in range(lane.blk_cnt)]

    def window(self, row: int, t0: int, t1: int, px: int) -> Optional[dict]:
        """这条行在 `[t0, t1)` 里的段，切成可以直接发给前端的字节。

        返回 `{mode, t_base, off, end, n, cell, c0, c1, stride}`，`None` 表示这一行
        这段窗口里什么都没有。**不做逐段循环**：exact 走 blk 表二分，coarse 走 L1 直接下标。
        """
        lane = self.lane(row)
        if lane is None or lane.seg_cnt == 0 or t1 <= lane.t_first or t0 >= lane.t_last:
            return None
        want = max(2 * px, 256)
        if lane.seg_cnt <= want or not lane.has_blk():
            return {"mode": "exact", "t_base": 0, "off": lane.seg_off,
                    "end": lane.seg_end, "n": lane.seg_cnt}
        blks = self.blk_table(lane)
        starts = [b[1] for b in blks]
        b0 = max(0, bisect.bisect_right(starts, t0) - 1)
        b1 = min(len(blks) - 1, bisect.bisect_right(starts, t1))
        if (b1 - b0 + 1) * lane.blk_span <= want:
            if b1 + 1 < len(blks):
                end = blks[b1 + 1][2]
                n = (b1 - b0 + 1) * lane.blk_span      # 都是满块
            else:
                # 最后一块可能不满：段数按「这条行一共几段」倒着算，不能拿块跨度乘。
                end = lane.seg_end
                n = lane.seg_cnt - b0 * lane.blk_span
            return {"mode": "exact", "t_base": blks[b0][0], "off": blks[b0][2],
                    "end": end, "n": n}
        if not lane.has_l1():
            # 没有粗层就没法降采样：从这块起把整条行的尾巴都给出去，客户端自己裁。
            return {"mode": "exact", "t_base": blks[b0][0], "off": blks[b0][2],
                    "end": lane.seg_end, "n": lane.seg_cnt - b0 * lane.blk_span}
        c0 = max(0, t0 // lane.l1_cell)
        c1 = min(lane.l1_cnt - 1, max(c0, (max(t0, t1 - 1)) // lane.l1_cell))
        off = lane.l1_off + c0 * L1_STRIDE
        end = lane.l1_off + (c1 + 1) * L1_STRIDE
        return {"mode": "coarse", "cell": lane.l1_cell, "c0": c0, "c1": c1,
                "stride": L1_STRIDE, "off": off, "end": end, "n": c1 - c0 + 1}


def decode_l1(payload, cell: int, c0: int) -> List[List[int]]:
    """粗层切片 → `[[t0, t1, density, user, task], ...]`（自检用；前端在 JS 里另解）。"""
    out: List[List[int]] = []
    for i in range(0, len(payload), L1_STRIDE):
        c = c0 + i // L1_STRIDE
        density = payload[i]
        user, task = tag_read(payload, i + 1)
        out.append([c * cell, (c + 1) * cell, density, user, task])
    return out


# ── manifest ──

def read_manifest(dir_path: Path) -> Optional[Dict]:
    """索引目录里的 manifest。读不动就返回 None（当成没有索引）。"""
    path = dir_path / MANIFEST_NAME
    try:
        with open(path, encoding="utf-8") as f:
            m = json.load(f)
    except (OSError, ValueError):
        return None
    if m.get("format") != INDEX_FORMAT:
        return None
    return m


def write_manifest(dir_path: Path, manifest: Dict) -> None:
    tmp = dir_path / (MANIFEST_NAME + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(manifest, f, separators=(",", ":"), ensure_ascii=False)
    os.replace(tmp, dir_path / MANIFEST_NAME)
