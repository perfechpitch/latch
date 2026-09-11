#!/usr/bin/env python3
"""把一份 Bach 的波形回放成一页自包含的 HTML 动画。

独立脚本，不依赖仓库里别的 Python 代码，也不用装第三方包，直接执行：

    python3 src/bach/replay.py              # 在哪个目录执行，就从哪个目录往下找波形
    python3 src/bach/replay.py moe_lpu      # 只列名字里带这一截的
    python3 src/bach/replay.py moe_lpu -o out.html

四级视图，逐级点进去：阵列（所有 chip 排成格子）→ 一颗 chip（片内的 core 与它们
之间的链路）→ 一个 core（方框图与甘特图）→ core 里的 Router。画面与数字全部由
波形驱动，页面里不留任何写死的结论。

样式与操作照 doc/bach/design-for-model/09-*.html 那几份动画讲解：左边舞台、右边
信息栏、底下播放条，空格播放暂停、左右方向键单步。

只认 Bach 的摆法：chip / core 的编号与 Core::EmitTrace() 那几个信号名，换个项目
就不成立。读波形那一段按 latch 的 .trace 格式自己解，格式与
src/utils/insight/reader.py 同一套。

产物是本地单文件，事件数据内联在里面，不依赖任何外部资源。默认落在波形旁边，
同名换成 .html。
"""

from __future__ import annotations

import argparse
import json
import mmap
import struct
import sys
from datetime import datetime
import re
from pathlib import Path
from dataclasses import dataclass
from typing import Dict, List, Tuple

# ---- 读波形 ----
# latch 的 .trace：一串信号段，文件尾 16 字节是 trailer 与 meta 两段的偏移。
# meta 段记模块树（每个信号也是一个节点），trailer 段记每个信号有哪几段。
# 这里只留回放要用的：按信号号取层次名、取一个信号的全部事件。

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


def varint_decode(buf, off: int) -> Tuple[int, int]:
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


class Reader:
    """signals() 列出信号号，tree_paths() 把号翻成层次名，events() 给出一个
    信号的时间与值两列。事件在第一次取时才解，解过的留着。"""

    def __init__(self, prefix: str):
        self.prefix = prefix
        self.path = prefix + ".trace"
        self.mods: Dict[int, Tuple[int, str]] = {}      # id → (父节点 id, 名字)
        self.segs_by_sig: Dict[int, List[Segment]] = {}
        self.cache: Dict[int, Tuple[list, list]] = {}
        self.fp = open(self.path, "rb")
        self.mm = None
        try:
            self.load()
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

    def __enter__(self) -> "Reader":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def signals(self) -> List[int]:
        return sorted(self.segs_by_sig.keys())

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
        segs = self.segs_by_sig.get(signal_id, [])
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
        for i in range(seg.event_count - 1):
            dt, pos = varint_decode(mm, pos)
            dvz, pos = varint_decode(mm, pos)
            t += dt
            v = (v + ((dvz >> 1) ^ -(dvz & 1))) & VALUE_MASK
            yield t, v
        if pos != seg.body_off + seg.body_len:
            raise ValueError(f"{self.path}: 信号 {seg.signal_id} 的段在 "
                             f"{seg.body_off} 处长度对不上")

    def load(self) -> None:
        size = self.fp.seek(0, 2)
        if size < FOOTER_SIZE:
            raise ValueError(f"{self.path}: 只有 {size} B，放不下 16 B 的文件尾")
        self.mm = mmap.mmap(self.fp.fileno(), 0, access=mmap.ACCESS_READ)
        trailer_off, meta_off = struct.unpack_from("<QQ", self.mm, size - FOOTER_SIZE)
        if not (0 <= trailer_off <= meta_off <= size - FOOTER_SIZE):
            raise ValueError(f"{self.path}: 文件尾的偏移不对")
        self.load_meta(meta_off, size - FOOTER_SIZE)
        self.load_trailer(trailer_off, meta_off)

    def load_meta(self, start: int, end: int) -> None:
        mm = self.mm
        if mm[start:start + 4] != b"SMLM":
            raise ValueError(f"{self.path}: meta 段的标记不对")
        version, = struct.unpack_from("<I", mm, start + 4)
        if version != 1:
            raise ValueError(f"{self.path}: 不认识的 meta 版本 {version}")
        cnt, = struct.unpack_from("<Q", mm, start + 8)
        off = start + 16
        for i in range(cnt):
            mid, pid = struct.unpack_from("<QQ", mm, off)
            nlen, = struct.unpack_from("<H", mm, off + 16)
            name = bytes(mm[off + 18:off + 18 + nlen]).decode("utf-8")
            self.mods[mid] = (pid, name)
            off += 18 + nlen
        if off != end:
            raise ValueError(f"{self.path}: meta 段末尾多出字节")

    def load_trailer(self, start: int, end: int) -> None:
        mm = self.mm
        if mm[start:start + 4] != b"SMLT":
            raise ValueError(f"{self.path}: trailer 段的标记不对")
        signal_cnt, = struct.unpack_from("<I", mm, start + 4)
        cur = start + 8
        for i in range(signal_cnt):
            sig_id, seg_count = struct.unpack_from("<QI", mm, cur)
            cur += 12
            offs = struct.unpack_from(f"<{seg_count}Q", mm, cur)
            cur += seg_count * 8
            self.segs_by_sig[sig_id] = [self.load_segment(o) for o in offs]
        if cur != end:
            raise ValueError(f"{self.path}: trailer 段末尾多出字节")

    def load_segment(self, off: int) -> Segment:
        (magic, signal_id, body_len, event_count, t_first, v_first, t_last,
         v_min, v_max) = struct.unpack_from(SEG_HEADER_FMT, self.mm, off)
        if magic != b"SMLS":
            raise ValueError(f"{self.path}: {off} 处的段标记不对")
        return Segment(signal_id, body_len, event_count, t_first, v_first,
                       t_last, off + SEG_HEADER_SIZE)

# Router 那一组，与 Core::EmitTrace() / EmitRouter() 同源，每个 core 都有。
ROUTER_SIGS = [
    "fwd_mid", "fwd_left", "fwd_right", "fwd_core",
    "occ_mid", "occ_left", "occ_right", "occ_core",
    "out_mid", "out_left", "out_right", "out_core", "out_rdc",
    "xbar_stall", "core_in", "cs_trig", "reduce_q", "rdc_ctx",
    "reissue", "retire", "cmcm_q",
]
# 一个 core 记的全部信号：Router 那一组，加 core_out 与 ts_*。后面这几个不派
# 角色的 core 没有。
CORE_SIGS = ROUTER_SIGS + [
    "core_out", "ts_inflight", "ts_issue", "ts_done",
    # 这两个是打包值：ts_unit 是位掩码，ts_task 三路各占 8 bit。不画成波形，
    # 解成「本拍哪一路下发了第几号 task」，core 那一级按它分步。
    "ts_unit", "ts_task",
]
# 画成波形看的那些，打包的两个不在里面。
WAVE_SIGS = [s for s in CORE_SIGS if s not in ("ts_unit", "ts_task")]
# 判定一个 core 派没派角色：不派角色的只建 Router，没有这个信号。
ROLE_SIG = "ts_inflight"

# 这几个在模型里是只加不清零的累计计数器，画面上要取相邻两拍的差才是「本拍
# 发生了多少」。其余那几个本身就是水位（队列长度、缓冲占用），直接取值。
CUM_SIGS = ["fwd_mid", "fwd_left", "fwd_right", "fwd_core",
            "out_mid", "out_left", "out_right", "out_core", "out_rdc",
            "xbar_stall", "cs_trig", "retire", "ts_issue", "ts_done"]

# 一颗 chip 四个 C2C 口的方位，与 chip.h 的 BuildBridges 同序。
PORTS = ["n", "e", "w", "s"]

RE_CORE = re.compile(r"^chip(\d+)\.core(\d+)\.(\w+)$")
RE_C2C = re.compile(r"^chip(\d+)\.c2c_([news])\.(\w+)\.(\w+)$")
RE_SCP = re.compile(r"^chip(\d+)\.scp\.(\w+)$")


def collect(prefix: str) -> dict:
    """从波形里把 chip / core / 链路三档数据取出来，顺带算出拓扑。"""
    with Reader(prefix) as r:
        paths = r.tree_paths()
        core: Dict[str, List[int]] = {}
        c2c: Dict[str, List[int]] = {}
        scp: Dict[str, List[int]] = {}
        chips: Dict[int, set] = {}
        has_role: Dict[str, bool] = {}
        t_end = 0
        frames: set = set()

        for sig_id in r.signals():
            name = paths.get(sig_id)
            if not name:
                continue
            m = RE_CORE.match(name)
            if m and m.group(3) in CORE_SIGS:
                c, k, s = int(m.group(1)), int(m.group(2)), m.group(3)
                ts, vs = r.events(sig_id)
                if not ts:
                    continue
                flat: List[int] = []
                for t, v in zip(ts, vs):
                    flat.append(int(t))
                    flat.append(int(v))
                    frames.add(int(t))
                core[f"{c}.{k}.{s}"] = flat
                chips.setdefault(c, set()).add(k)
                if s == ROLE_SIG:
                    has_role[f"{c}.{k}"] = True
                t_end = max(t_end, int(ts[-1]))
                continue

            m = RE_C2C.match(name)
            if m:
                c, p, sub, s = int(m.group(1)), m.group(2), m.group(3), m.group(4)
                # 出片那一路用 axi_out 的在途数，进片那一路用 axi_in 的。
                if not (sub in ("axi_out", "axi_in") and s == "inflight"):
                    continue
                ts, vs = r.events(sig_id)
                if not ts:
                    continue
                flat = []
                for t, v in zip(ts, vs):
                    flat.append(int(t))
                    flat.append(int(v))
                    frames.add(int(t))
                c2c[f"{c}.{p}.{'out' if sub == 'axi_out' else 'in'}"] = flat
                chips.setdefault(c, set())
                t_end = max(t_end, int(ts[-1]))
                continue

            m = RE_SCP.match(name)
            if m:
                ts, vs = r.events(sig_id)
                if not ts:
                    continue
                flat = []
                for t, v in zip(ts, vs):
                    flat.append(int(t))
                    flat.append(int(v))
                    frames.add(int(t))
                scp[f"{int(m.group(1))}.{m.group(2)}"] = flat
                chips.setdefault(int(m.group(1)), set())
                t_end = max(t_end, int(ts[-1]))

    ids = sorted(chips)
    cols = 4 if len(ids) > 4 else max(1, len(ids))
    topo = []
    for c in ids:
        ks = sorted(chips[c])
        n = len(ks)
        # 片内 core 的列数：10 个 core 摆 2×5，8 个摆 2×4，与 chip.h 的 ColsOf 一致。
        ccols = 5 if n > 8 else max(1, (n + 1) // 2)
        topo.append({
            "id": c,
            "gx": ids.index(c) % cols,
            "gy": ids.index(c) // cols,
            "cols": ccols,
            "cores": [{"i": k, "role": bool(has_role.get(f"{c}.{k}"))} for k in ks],
        })

    return {
        "cols": cols,
        "rows": (len(ids) + cols - 1) // cols,
        "t_end": t_end,
        "frames": sorted(frames),
        "chips": topo,
        "core": core,
        "c2c": c2c,
        "scp": scp,
    }


# 样式照 09-*.html 那几份，改动只在多了一个格子视图要用的类。
CSS = """
:root{
  --bg:#f7f8fa; --panel:#ffffff; --ink:#16181d; --dim:#5c6370; --line:#d8dce3;
  --accent:#2563eb; --warn:#d97706; --ok:#0d9488; --bad:#dc2626; --violet:#7c3aed;
  --box:#f8fafc; --boxb:#374151; --hi:#dbeafe; --hib:#2563eb;
  --mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
  --sans:"Noto Sans CJK SC","PingFang SC","Microsoft YaHei",-apple-system,sans-serif;
}
@media (prefers-color-scheme:dark){
  :root{--bg:#14161a; --panel:#1b1e24; --ink:#e6e8ec; --dim:#9aa1ad; --line:#2c313a;
        --box:#22262e; --boxb:#5b6472; --hi:#1e3a5f; --hib:#60a5fa; --accent:#60a5fa;}
}
*{box-sizing:border-box}
html,body{margin:0;height:100%}
body{background:var(--bg);color:var(--ink);font-family:var(--sans);font-size:14px;line-height:1.6;
     display:flex;flex-direction:column;overflow:hidden}
header{padding:10px 18px;border-bottom:1px solid var(--line);background:var(--panel);
       display:flex;align-items:center;gap:9px 18px;flex-wrap:wrap}
h1{font-size:15px;margin:0;font-weight:700;letter-spacing:.02em}
h1 small{font-weight:400;color:var(--dim);font-size:12px;margin-left:8px}
nav{display:flex;gap:4px;flex-wrap:wrap;flex-basis:100%;align-items:center}
nav button{font-family:var(--sans);font-size:12.5px;padding:5px 12px;border:1px solid var(--line);
  background:transparent;color:var(--dim);border-radius:999px;cursor:pointer;transition:.15s}
nav button:hover{color:var(--ink);border-color:var(--boxb)}
nav button.on{background:var(--accent);border-color:var(--accent);color:#fff;font-weight:600}
nav span.sep{color:var(--dim);font-size:12px}
main{flex:1;display:flex;min-height:0}
#stagewrap{flex:1;min-width:0;display:flex;flex-direction:column;padding:10px 14px 0}
#stage{flex:1;min-height:0}
#stage svg{width:100%;height:100%;display:block}
aside{width:420px;flex-shrink:0;border-left:1px solid var(--line);background:var(--panel);
      padding:14px 16px 24px;overflow-y:auto}
h3.sec{font-size:11px;margin:14px 0 5px;color:var(--dim);font-weight:600;letter-spacing:.06em}
h3.sec:first-child{margin-top:0}
#pickname{font-size:15.5px;font-weight:700;margin:2px 0 8px;line-height:1.4}
#picksub{font-size:12px;color:var(--dim);margin-bottom:10px}
table.lg{width:100%;border-collapse:collapse;font-family:var(--mono);font-size:11px;
  background:var(--box);border:1px solid var(--line);border-radius:5px;overflow:hidden}
table.lg th{text-align:left;color:var(--dim);font-weight:600;padding:5px 9px 4px;
  border-bottom:1px solid var(--line);white-space:nowrap}
table.lg td{padding:5px 9px;border-top:1px solid var(--line);vertical-align:top;
  line-height:1.6;color:var(--ink)}
table.lg td.n{text-align:right;font-variant-numeric:tabular-nums;width:72px}
table.lg tr.chg td{background:var(--hi);color:var(--hib);font-weight:700}
.spark{display:block;width:100%;height:22px;margin-top:2px}
.hint{font-size:11.5px;color:var(--dim);border-left:2px solid var(--warn);
  padding-left:9px;line-height:1.55;margin-top:12px}
footer{border-top:1px solid var(--line);background:var(--panel);padding:8px 18px;
       display:flex;align-items:center;gap:12px;flex-wrap:wrap}
footer button{font-family:var(--sans);font-size:13px;padding:5px 13px;border:1px solid var(--line);
  background:var(--box);color:var(--ink);border-radius:6px;cursor:pointer}
footer button:hover{border-color:var(--accent)}
footer button.pri{background:var(--accent);border-color:var(--accent);color:#fff;
  font-weight:600;min-width:74px}
#bar{flex:1;min-width:160px;height:24px;position:relative;cursor:pointer;
  touch-action:none;user-select:none}
#bar::before{content:'';position:absolute;left:0;right:0;top:10px;height:4px;
  background:var(--line);border-radius:2px;transition:top .1s,height .1s}
#bar:hover::before,#bar.drag::before{top:9px;height:6px}
#barfill{position:absolute;left:0;top:10px;height:4px;width:0;background:var(--accent);
  border-radius:2px;pointer-events:none;transition:top .1s,height .1s}
#bar:hover #barfill,#bar.drag #barfill{top:9px;height:6px}
#thumb{position:absolute;top:5px;left:0;width:14px;height:14px;margin-left:-7px;
  border-radius:50%;background:var(--panel);border:2px solid var(--accent);
  box-shadow:0 1px 3px rgba(0,0,0,.3);pointer-events:none;transition:transform .1s}
#bar:hover #thumb,#bar.drag #thumb{transform:scale(1.25)}
.gbg{fill:transparent;cursor:ew-resize}
.gbar{cursor:pointer}
.gbar rect{stroke-width:1}
.gbar.done rect{fill:color-mix(in srgb,var(--accent) 28%,var(--box));stroke:var(--accent)}
.gbar.run rect{fill:var(--hi);stroke:var(--hib);stroke-width:2}
.gbar.todo rect{fill:var(--box);stroke:var(--boxb);stroke-dasharray:3 2;opacity:.75}
.gbar text{font-family:var(--sans);font-size:10px;fill:var(--ink);pointer-events:none}
.gbar.run text{fill:var(--hib);font-weight:700}
.glane{fill:var(--dim);font-size:10.5px;font-weight:600}
.gtick{fill:var(--dim);font-size:9px;font-family:var(--mono)}
.gcur{stroke:var(--bad);stroke-width:1.6;pointer-events:none}
#cyc{font-family:var(--mono);font-size:12px;color:var(--ink);
  font-variant-numeric:tabular-nums;min-width:150px}
#kbd{font-size:11.5px;color:var(--dim);font-family:var(--mono)}
label.spd{font-size:12px;color:var(--dim);display:flex;align-items:center;gap:5px}
label.spd input{width:76px}
.nd rect{fill:var(--box);stroke:var(--boxb);stroke-width:1;cursor:pointer}
.nd text{fill:var(--ink);font-family:var(--sans);pointer-events:none}
.nd.act rect{fill:var(--hi);stroke:var(--hib);stroke-width:2}
.nd.act text.t{fill:var(--hib);font-weight:700}
.nd.blk rect{stroke:var(--bad);stroke-width:2}
.nd.sel rect{stroke:var(--violet);stroke-width:2.5}
.sub{fill:var(--dim);font-size:9px;pointer-events:none}
.grp{fill:none;stroke:var(--line);stroke-width:1;stroke-dasharray:4 3}
.grpl{fill:var(--dim);font-size:10px;letter-spacing:.06em}
.ed{fill:none;stroke:var(--boxb);stroke-width:1.1;opacity:.35}
.ed.act{stroke:var(--accent);stroke-width:2.4;opacity:1}
.ed.jam{stroke:var(--bad);stroke-width:2.4;opacity:1}
.edl{fill:var(--dim);font-size:9px;font-family:var(--mono);
     paint-order:stroke;stroke:var(--bg);stroke-width:3px;stroke-linejoin:round}
.cnt{font-family:var(--sans);font-variant-numeric:tabular-nums;font-size:10px;fill:var(--dim);
  pointer-events:none}
.cnt.chg{fill:var(--hib);font-weight:700}
.legend{fill:var(--dim);font-size:10px;font-family:var(--sans)}
"""


JS = r"""
// ── 取值：每个信号是一串扁平的 [t0,v0,t1,v1,...]，取不晚于 t 的最后一次 ──
function valAt(a, t){
  if(!a || !a.length) return 0;
  if(t < a[0]) return 0;
  let lo = 0, hi = (a.length >> 1) - 1, r = 0;
  while(lo <= hi){
    const m = (lo + hi) >> 1;
    if(a[m*2] <= t){ r = a[m*2+1]; lo = m + 1; } else hi = m - 1;
  }
  return r;
}
const CS = (c,k,s) => valAt(D.core[c+"."+k+"."+s], T);
const XS = (c,p,d) => valAt(D.c2c[c+"."+p+"."+d], T);
// 累计计数器取相邻两拍的差；水位类直接取值。画面上一律用这个。
function nowAt(a, t){ return valAt(a, t) - valAt(a, t - 1); }
const CN = (c,k,s) => {
  const a = D.core[c+"."+k+"."+s];
  return CUM.has(s) ? nowAt(a, T) : valAt(a, T);
};

// ── 状态 ──
let T = 0;                       // 当前拍
let view = {k:'grid'};           // {k:'grid'} | {k:'chip',c} | {k:'core',c,k2}
let sel = null;                  // 右栏盯着的对象
let playing = false, fps = 24, stride = 1, timer = null;

const chipById = {};
D.chips.forEach(ch => chipById[ch.id] = ch);

// 一颗 chip 本拍的三个汇总量：转发量、堵住的笔数、在飞的 stream 数。
function chipStat(ch){
  let flow = 0, jam = 0, task = 0, occ = 0;
  for(const co of ch.cores){
    const k = co.i;
    flow += CN(ch.id,k,'fwd_mid') + CN(ch.id,k,'fwd_left') + CN(ch.id,k,'fwd_right');
    occ  += CS(ch.id,k,'occ_mid') + CS(ch.id,k,'occ_left') + CS(ch.id,k,'occ_right');
    jam  += CN(ch.id,k,'xbar_stall');
    if(co.role) task += CS(ch.id,k,'ts_inflight');
  }
  return {flow, jam, task, occ};
}
function coreStat(c,k){
  const o = {};
  for(const s of SIGS) o[s] = CN(c,k,s);
  o.tot = {};
  for(const s of SIGS) if(CUM.has(s)) o.tot[s] = CS(c,k,s);
  o.flow = o.fwd_mid + o.fwd_left + o.fwd_right;
  o.occ  = o.occ_mid + o.occ_left + o.occ_right;
  return o;
}

// ── 分步：ts_unit 是位掩码，ts_task 三路各占 8 bit，0xFF 表示那一路没发 ──
const UNAME = ['DTE', 'MU', 'VU'];
const stepCache = {};
function stepsOf(c, k){
  const key = c + "." + k;
  if(stepCache[key]) return stepCache[key];
  const um = D.core[key + ".ts_unit"] || [], tk = D.core[key + ".ts_task"] || [];
  const issue = [];
  for(let j = 0; j < um.length; j += 2){
    const mask = um[j+1]; if(!mask) continue;
    const t = um[j], pack = valAt(tk, t);
    for(let u = 0; u < 3; ++u)
      if(mask & (1 << u)) issue.push({t, u, task: (pack >> (8*u)) & 0xFF});
  }
  // 完成数是累计的，取增量当完成事件。
  const dn = D.core[key + ".ts_done"] || [], done = [];
  let prev = 0;
  for(let j = 0; j < dn.length; j += 2){
    const d = dn[j+1] - prev; prev = dn[j+1];
    if(d > 0) done.push({t: dn[j], n: d});
  }
  return stepCache[key] = {issue, done};
}
// 当前停在第几步：取时刻不晚于 T 的最后一笔。
function stepAt(c, k){
  const st = stepsOf(c, k); let i = -1;
  for(let j = 0; j < st.issue.length; ++j) if(st.issue[j].t <= T) i = j;
  return i;
}

// ── 画：三个视图共用一套节点/连线的画法 ──
const SVGNS = "http://www.w3.org/2000/svg";
function el(n, a){ const e = document.createElementNS(SVGNS, n);
  for(const k in a) e.setAttribute(k, a[k]); return e; }

// 流量映射成填充深浅：0 不填，越大越深，8 以上封顶。
function heat(v){
  if(v <= 0) return 0;
  return Math.min(1, 0.18 + 0.82 * Math.min(1, v / 8));
}

function drawGrid(){
  const CW = 108, CH = 46, GX = 30, GY = 20, PADX = 40, PADY = 34;
  const W = PADX*2 + D.cols*CW + (D.cols-1)*GX;
  const H = PADY*2 + D.rows*CH + (D.rows-1)*GY + 22;
  const svg = el('svg', {viewBox:`0 0 ${W} ${H}`, preserveAspectRatio:'xMidYMid meet'});
  const at = ch => ({x: PADX + ch.gx*(CW+GX), y: PADY + ch.gy*(CH+GY)});

  // 先画链路，再画方块，方块盖住线头。
  for(const ch of D.chips){
    for(const nb of D.chips){
      if(nb.gy === ch.gy && nb.gx === ch.gx + 1){
        const a = at(ch), b = at(nb);
        const act = XS(ch.id,'e','out') + XS(nb.id,'w','out');
        svg.appendChild(el('path', {class:'ed' + (act ? ' act' : ''),
          d:`M${a.x+CW},${a.y+CH/2} L${b.x},${b.y+CH/2}`}));
      }
      if(nb.gx === ch.gx && nb.gy === ch.gy + 1){
        const a = at(ch), b = at(nb);
        const act = XS(ch.id,'s','out') + XS(nb.id,'n','out');
        svg.appendChild(el('path', {class:'ed' + (act ? ' act' : ''),
          d:`M${a.x+CW/2},${a.y+CH} L${b.x+CW/2},${b.y}`}));
      }
    }
  }
  for(const ch of D.chips){
    const p = at(ch), st = chipStat(ch);
    const g = el('g', {class:'nd' + (st.jam ? ' blk' : '') +
      (sel && sel.k==='chip' && sel.c===ch.id ? ' sel' : '')});
    const r = el('rect', {x:p.x, y:p.y, width:CW, height:CH, rx:6});
    const h = heat(st.flow);
    if(h) r.setAttribute('fill', `color-mix(in srgb, var(--accent) ${Math.round(h*100)}%, var(--box))`);
    g.appendChild(r);
    g.appendChild(el('text', {class:'t', x:p.x+9, y:p.y+18, 'font-size':12,
      'font-weight':600})).textContent = 'chip' + ch.id;
    g.appendChild(el('text', {class:'cnt', x:p.x+9, y:p.y+34}))
      .textContent = `fwd ${st.flow}  occ ${st.occ}  st ${st.jam}`;
    g.appendChild(el('text', {class:'cnt', x:p.x+CW-9, y:p.y+18,
      'text-anchor':'end'})).textContent = st.task ? ('在飞 ' + st.task) : '';
    g.onclick = () => { view = {k:'chip', c:ch.id}; sel = {k:'chip', c:ch.id}; render(); };
    svg.appendChild(g);
  }
  const lg = el('text', {class:'legend', x:PADX, y:H-14});
  lg.textContent = '方块深浅 = 本拍片内转发的 flit 数　红框 = Xbar 有笔没发出去　'
                 + '蓝线 = C2C 上有数据在途　点一颗 chip 进片内';
  svg.appendChild(lg);
  return svg;
}

function drawChip(cid){
  const ch = chipById[cid];
  const cols = ch.cols, CW = 150, CH = 74, GX = 54, GY = 62, PADX = 46, PADY = 40;
  const W = PADX*2 + cols*CW + (cols-1)*GX;
  const H = PADY*2 + 2*CH + GY + 26;
  const svg = el('svg', {viewBox:`0 0 ${W} ${H}`, preserveAspectRatio:'xMidYMid meet'});
  const slot = k => ({x: PADX + (k % cols)*(CW+GX), y: PADY + Math.floor(k/cols)*(CH+GY)});
  const have = {}; ch.cores.forEach(c => have[c.i] = c);

  // 同行相邻两个 core 一对链路，上下同列的一对 mid 直连。线的活跃度取收方那一
  // 侧的入口站：fwd_left 是从左邻收进来的，fwd_right 是从右邻收进来的。
  for(const co of ch.cores){
    const k = co.i, p = slot(k);
    if(have[k+1] && (k+1) % cols !== 0){
      const q = slot(k+1);
      const act = CN(cid,k+1,'fwd_left') + CN(cid,k,'fwd_right');
      const jam = CS(cid,k+1,'occ_left') + CS(cid,k,'occ_right');
      svg.appendChild(el('path', {class:'ed' + (act ? ' act' : (jam ? ' jam' : '')),
        d:`M${p.x+CW},${p.y+CH/2} L${q.x},${q.y+CH/2}`}));
    }
    if(have[k+cols]){
      const q = slot(k+cols);
      const act = CN(cid,k+cols,'fwd_mid') + CN(cid,k,'fwd_mid');
      svg.appendChild(el('path', {class:'ed' + (act ? ' act' : ''),
        d:`M${p.x+CW/2},${p.y+CH} L${q.x+CW/2},${q.y}`}));
    }
  }
  for(const co of ch.cores){
    const k = co.i, p = slot(k), st = coreStat(cid, k);
    const g = el('g', {class:'nd' + (st.xbar_stall ? ' blk' : '') +
      (sel && sel.k==='core' && sel.c===cid && sel.k2===k ? ' sel' : '')});
    const r = el('rect', {x:p.x, y:p.y, width:CW, height:CH, rx:6});
    const h = heat(st.flow);
    if(h) r.setAttribute('fill', `color-mix(in srgb, var(--accent) ${Math.round(h*100)}%, var(--box))`);
    g.appendChild(r);
    const tag = co.role ? '' : ' · 只转发';
    g.appendChild(el('text', {class:'t', x:p.x+9, y:p.y+17, 'font-size':12,
      'font-weight':600})).textContent = 'core' + k + tag;
    g.appendChild(el('text', {class:'cnt', x:p.x+9, y:p.y+33}))
      .textContent = `mid ${st.fwd_mid}  left ${st.fwd_left}  right ${st.fwd_right}`;
    g.appendChild(el('text', {class:'cnt', x:p.x+9, y:p.y+47}))
      .textContent = `occ ${st.occ}  stall ${st.xbar_stall}  rdc ${st.reduce_q}`;
    g.appendChild(el('text', {class:'cnt', x:p.x+9, y:p.y+61}))
      .textContent = co.role
        ? `in ${st.core_in}  out ${st.core_out}  fly ${st.ts_inflight}`
        : `in ${st.core_in}`;
    g.onclick = () => { view = {k:'core', c:cid, k2:k}; sel = {k:'core', c:cid, k2:k}; render(); };
    svg.appendChild(g);
  }
  const lg = el('text', {class:'legend', x:PADX, y:H-14});
  lg.textContent = 'mid / left / right = 本拍从那个方向收进来交给 Xbar 的 flit 数　'
                 + '红线 = 那个方向的 VC Buffer 有积压　点一个 core 看它的时间线';
  svg.appendChild(lg);
  return svg;
}

// core 视图：上半是 core 内的方框图，下半是这个 core 的甘特图。
//
// 方框的摆法与 09-*.html 那几份讲解一致：TS 一横条在最上，三个 RV core 一排，
// 三个 DSA 一排，两块存储，最下面是 Router。正在跑的那一笔，把它经手的 TS、
// RV core、DSA 与它们之间的三条线点亮。
const BOX = [
  ['ts',  70,  20,  630, 50, 'TS　任务调度器'],
  ['sm',  712, 20,  258, 50, 'Share Mem'],
  ['rv1', 70,  84,  270, 44, 'MU RV Core'],
  ['rv2', 360, 84,  270, 44, 'VU RV Core'],
  ['rv0', 690, 84,  280, 44, 'DTE RV Core'],
  ['u1',  70,  148, 270, 42, 'MU DSA'],
  ['u2',  360, 148, 270, 42, 'VU DSA'],
  ['u0',  690, 148, 280, 42, 'DTE DSA'],
  ['mm',  70,  208, 430, 40, 'Matrix Mem'],
  ['cm',  520, 208, 450, 40, 'Core Mem'],
  ['xb',  70,  260, 900, 16, 'DTE Xbar'],
  ['rt',  70,  290, 900, 50, 'Router　点这个框看内部 ▸'],
];
const BOXAT = {}; for(const b of BOX) BOXAT[b[0]] = b;

// 每笔下发配上它的完成，画成甘特图上的一段。只有这个 core 同一时刻最多一笔在
// 飞时才配得准：ts_done 不分路，几笔并发时分不清哪次完成是哪一笔的。B core 与
// R core 同时挂着十几个 stream，那种 core 只画下发与完成的刻度，不配对。
const barCache = {};
function barsOf(c, k){
  const key = c + "." + k;
  if(barCache[key]) return barCache[key];
  const sp = stepsOf(c, k);
  const fl = D.core[key + ".ts_inflight"] || [];
  let fmax = 0;
  for(let j = 1; j < fl.length; j += 2) if(fl[j] > fmax) fmax = fl[j];
  const serial = fmax <= 1;
  const fins = [];
  for(const d of sp.done) for(let n = 0; n < d.n; ++n) fins.push(d.t);
  const bars = sp.issue.map(e => ({u: e.u, task: e.task, t0: e.t, t1: null}));
  if(serial){
    let j = 0;
    for(const b of bars){
      while(j < fins.length && fins[j] < b.t0) ++j;
      if(j < fins.length) b.t1 = fins[j++];
    }
  }
  return barCache[key] = {bars, serial, fmax, fins};
}

// 刻度取 1、2、5 乘 10 的幂，一条轴上六个上下。
function niceTicks(a, b){
  const raw = (b - a) / 6, mag = Math.pow(10, Math.floor(Math.log10(raw || 1)));
  let step = 10 * mag;
  for(const m of [1, 2, 5, 10]) if(m * mag >= raw){ step = m * mag; break; }
  const out = [];
  for(let t = Math.ceil(a / step) * step; t <= b; t += step) out.push(t);
  return out;
}
// 估一段文字多宽：中文按两个字宽算。
function textW(str){
  let n = 0;
  for(const ch of str) n += ch.charCodeAt(0) > 255 ? 10.5 : 6.2;
  return n + 8;
}

let drag = null;   // 正在甘特图上拖：记下按下那一刻的像素与拍数的对应

function drawCore(cid, k){
  const co = (chipById[cid].cores.find(x => x.i === k)) || {role:true};
  const st = coreStat(cid, k), sp = stepsOf(cid, k), si = stepAt(cid, k);
  const bo = barsOf(cid, k);
  const cur = si >= 0 ? sp.issue[si] : null;
  const W = 1040, H = 510;
  const svg = el('svg', {viewBox:`0 0 ${W} ${H}`, preserveAspectRatio:'xMidYMid meet'});

  // 正在跑的那一笔：配得上完成的按区间认，配不上的退回「刚下发的 40 拍内」。
  let runU = null;
  if(bo.serial){
    const b = bo.bars.find(b => b.t0 <= T && (b.t1 === null || T < b.t1));
    if(b) runU = b.u;
  } else if(cur && T - cur.t < 40){
    runU = cur.u;
  }
  const on = new Set();
  if(runU !== null){ on.add('ts'); on.add('rv' + runU); on.add('u' + runU); }

  const note = {
    ts: co.role ? [`在飞 ${st.ts_inflight} 笔 · 发出 ${st.tot.ts_issue} · 完成 ${st.tot.ts_done}`,
                   'task_chain 全序链，一笔完了才走下一笔'] : ['不派角色，本 core 没有 TS'],
    rt: [`mid ${st.fwd_mid} · left ${st.fwd_left} · right ${st.fwd_right}　(本拍收进来的 flit)`,
         `VC 占用 ${st.occ} · Xbar 没发出去 ${st.xbar_stall} · 进核压着 ${st.core_in} · 归约队列 ${st.reduce_q}`],
    u0: [`出核缓冲占用 ${st.core_out}`],
  };

  for(let u = 0; u < 3; ++u){
    const rv = BOXAT['rv' + u], ds = BOXAT['u' + u];
    const xc = rv[1] + 52, xd = rv[1] + rv[3] - 52;
    const act = runU === u ? ' act' : '';
    svg.appendChild(el('path', {class:'ed' + act,
      d:`M${xc},${BOXAT.ts[2]+BOXAT.ts[4]} L${xc},${rv[2]}`}));
    svg.appendChild(el('text', {class:'edl', x:xc+5, y:rv[2]-4})).textContent = 'task_cmd';
    svg.appendChild(el('path', {class:'ed' + act,
      d:`M${xc},${rv[2]+rv[4]} L${xc},${ds[2]}`}));
    svg.appendChild(el('text', {class:'edl', x:xc+5, y:ds[2]-4})).textContent = 'dsa_cfg';
    svg.appendChild(el('path', {class:'ed' + act,
      d:`M${xd},${ds[2]} L${xd},${ds[2]-14} L${xd+14},${ds[2]-14} `
      + `L${xd+14},${BOXAT.ts[2]+BOXAT.ts[4]}`}));
    svg.appendChild(el('text', {class:'edl', x:xd+18, y:ds[2]-4})).textContent = 'done';
  }
  for(const [id, x, y, w, h, t] of BOX){
    const g = el('g', {class:'nd' + (on.has(id) ? ' act' : '')});
    g.appendChild(el('rect', {x, y, width:w, height:h, rx:5}));
    g.appendChild(el('text', {class:'t', x:x+9, y:y+(h > 20 ? 17 : 12),
      'font-size':h > 20 ? 12 : 9.5, 'font-weight':600})).textContent = t;
    (note[id] || []).forEach((line, n) =>
      g.appendChild(el('text', {class:'cnt', x:x+9, y:y+31+n*13})).textContent = line);
    if(id === 'rt') g.addEventListener('pointerdown', () => {
      view = {k:'router', c:cid, k2:k}; render(); });
    svg.appendChild(g);
  }

  // ── 甘特图：DTE / MU / VU 各一行，时间轴只铺这个 core 真正活动的那一段 ──
  // 从上往下：标题一行（TY + 8），当前拍的标注一行（GY - 4），再往下是底板与三行。
  const GX0 = 70, GX1 = 970, TY = 350, LH = 18, GY = TY + 32, AXIS = TY + 120;
  const LANE = [TY + 42, TY + 68, TY + 94];
  const ends = [];
  for(const b of bo.bars){ ends.push(b.t0); if(b.t1 !== null) ends.push(b.t1); }
  for(const f of bo.fins) ends.push(f);
  let w0 = 0, w1 = D.t_end;
  if(ends.length){ w0 = Math.min(...ends); w1 = Math.max(...ends); }
  const pad = Math.max(10, Math.round((w1 - w0) * 0.04));
  w0 = Math.max(0, w0 - pad); w1 = Math.min(D.t_end, w1 + pad);
  if(w1 <= w0) w1 = w0 + 1;
  const tx = t => GX0 + (GX1 - GX0) * (t - w0) / (w1 - w0);

  svg.appendChild(el('text', {class:'grpl', x:GX0, y:TY + 8})).textContent =
    `甘特图　${sp.issue.length} 笔下发 · ${bo.fins.length} 次完成 · 第 ${w0} ～ ${w1} 拍`
    + (bo.serial ? '' : `　同时最多 ${bo.fmax} 笔在飞，完成不配对`);

  // 底板：在这上面按住拖动，逐拍移动当前拍。
  const bg = el('rect', {class:'gbg', x:GX0 - 6, y:GY, width:GX1 - GX0 + 12,
                         height:AXIS - GY + 4});
  bg.addEventListener('pointerdown', (e) => {
    play(false);
    const r = svg.getBoundingClientRect();
    const s = Math.min(r.width / W, r.height / H);
    drag = {ox: r.left + (r.width - W * s) / 2, s, x0: GX0, x1: GX1, w0, w1};
    ganttMove(e);
    e.preventDefault();
  });
  svg.appendChild(bg);

  for(let u = 0; u < 3; ++u){
    svg.appendChild(el('path', {class:'ed', opacity:.25,
      d:`M${GX0},${LANE[u] + LH/2} L${GX1},${LANE[u] + LH/2}`}));
    svg.appendChild(el('text', {class:'glane', x:GX0 - 12, y:LANE[u] + 13,
      'text-anchor':'end'})).textContent = UNAME[u];
  }
  svg.appendChild(el('path', {class:'ed', opacity:.8, d:`M${GX0},${AXIS} L${GX1},${AXIS}`}));
  for(const t of niceTicks(w0, w1)){
    const X = tx(t);
    svg.appendChild(el('path', {class:'ed', opacity:.8, d:`M${X},${AXIS} L${X},${AXIS + 4}`}));
    svg.appendChild(el('text', {class:'gtick', x:X, y:AXIS + 15,
      'text-anchor':'middle'})).textContent = t;
  }

  if(bo.serial){
    const lastEnd = [-1e9, -1e9, -1e9];
    bo.bars.forEach((b, n) => {
      const X0 = tx(b.t0), X1 = b.t1 === null ? GX1 : tx(b.t1);
      const w = Math.max(4, X1 - X0);
      const state = T < b.t0 ? 'todo' : (b.t1 === null || T < b.t1 ? 'run' : 'done');
      const g = el('g', {class:'gbar ' + state});
      g.appendChild(el('rect', {x:X0, y:LANE[b.u], width:w, height:LH, rx:3}));
      const dur = b.t1 === null ? '未完成' : `${b.t1 - b.t0} 拍`;
      const long = `${UNAME[b.u]} ${b.task} · ${dur}`, short = `${UNAME[b.u]} ${b.task}`;
      // 标签先往段里放，放不下放到段右边；右边挨着同一行的下一段也放不下，就不写，
      // 悬停看得到，右栏的表里也有。
      const nxt = bo.bars.slice(n + 1).find(x => x.u === b.u);
      const room = (nxt ? tx(nxt.t0) - 6 : GX1) - (X0 + w) - 4;
      let txt = null, lx = 0;
      if(w >= textW(long)){ txt = long; lx = X0 + 5; }
      else if(w >= textW(short)){ txt = short; lx = X0 + 5; }
      else if(room >= textW(long) && X0 + w + 4 > lastEnd[b.u]){ txt = long; lx = X0 + w + 4; }
      else if(room >= textW(short) && X0 + w + 4 > lastEnd[b.u]){ txt = short; lx = X0 + w + 4; }
      if(txt){
        g.appendChild(el('text', {x:lx, y:LANE[b.u] + 13})).textContent = txt;
        lastEnd[b.u] = lx + textW(txt);
      }
      g.appendChild(el('title', {})).textContent = long + `，第 ${b.t0} 拍下发`
        + (b.t1 === null ? '' : `、第 ${b.t1} 拍完成`);
      g.addEventListener('pointerdown', (e) => {
        e.stopPropagation(); play(false); T = b.t0; syncFi(); render();
      });
      svg.appendChild(g);
    });
  } else {
    for(const e of sp.issue){
      const X = tx(e.t);
      svg.appendChild(el('path', {class:'ed act', 'stroke-width':1.4,
        d:`M${X},${LANE[e.u]} L${X},${LANE[e.u] + LH}`}));
    }
    for(const f of bo.fins){
      const X = tx(f);
      svg.appendChild(el('path', {d:`M${X},${AXIS - 7} L${X},${AXIS}`,
        stroke:'var(--ok)', 'stroke-width':1.4}));
    }
  }

  if(T >= w0 && T <= w1){
    const X = tx(T);
    svg.appendChild(el('path', {class:'gcur', d:`M${X},${GY} L${X},${AXIS + 4}`}));
    svg.appendChild(el('text', {class:'gtick', x:X, y:GY - 4, 'text-anchor':'middle',
      style:'fill:var(--bad)'})).textContent = T;
  } else {
    const left = T < w0;
    svg.appendChild(el('text', {class:'gtick', x:left ? GX0 : GX1, y:GY - 4,
      'text-anchor':left ? 'start' : 'end', style:'fill:var(--bad)'})).textContent =
      left ? `◂ 当前第 ${T} 拍，还没到这一段` : `当前第 ${T} 拍，这一段已经过去 ▸`;
  }
  svg.appendChild(el('text', {class:'legend', x:GX0, y:AXIS + 32})).textContent =
    '实心 = 已完成　高亮 = 正在跑　虚框 = 还没下发　红线 = 当前拍　'
    + '在图上按住拖动逐拍走，点一段跳到它下发的那一拍';
  return svg;
}

// Router 视图：方框与连线照设计文档那五份 router 讲解（09-router-*.html）的摆法，
// 纵坐标整体上移 190，去掉了最上面上游 / 本 / 下游三个 core 那一排。每个框写本拍
// 的量，本拍有 flit 经过的线点亮。下面是九个口的逐拍活动条，按住拖动逐拍走。
const RBOX = [
  ['ts',   40,  16,  160, 40,  'TS'],
  ['drv',  212, 16,  148, 40,  'DTE RV Core'],
  ['dte',  372, 16,  148, 40,  'DTE DSA'],
  ['cm',   532, 16,  128, 40,  'Core Mem'],
  ['cmcm', 40,  72,  190, 88,  'CoreMemCreditMonitor'],
  ['cs',   300, 72,  320, 88,  'CoreStation'],
  ['rtab', 690, 72,  310, 88,  'CSR / RouterTable'],
  ['rs_l', 40,  178, 240, 102, 'RouterStation[left]'],
  ['xb',   380, 178, 180, 72,  'Xbar'],
  ['rs_r', 660, 178, 200, 72,  'RouterStation[right]'],
  ['rs_m', 320, 272, 240, 66,  'RouterStation[mid]'],
  ['rmod', 660, 272, 340, 110, 'ReduceModule'],
  ['reis', 40,  306, 240, 76,  'CoreMem 重发'],
  ['ret',  320, 354, 240, 56,  'Retire'],
];
const REDGE = [
  ['e_in_l',   [[18,202],[40,202]],     '进',       [24, 199]],
  ['e_out_l',  [[40,256],[18,256]],     '出',       [24, 253]],
  ['e_in_r',   [[1032,202],[860,202]],  'right 进', [866, 199]],
  ['e_out_r',  [[860,230],[1032,230]],  'right 出'],
  ['e_in_m',   [[600,288],[560,288]],   'mid 进',   [566, 285]],
  ['e_out_m',  [[560,316],[600,316]],   'mid 出'],
  ['e_l_xb',   [[280,202],[380,202]],   ''],
  ['e_xb_l',   [[380,230],[280,230]],   ''],
  ['e_r_xb',   [[660,202],[560,202]],   ''],
  ['e_xb_r',   [[560,222],[660,222]],   ''],
  ['e_m_xb',   [[440,272],[440,250]],   ''],
  ['e_xb_m',   [[490,250],[490,272]],   ''],
  ['e_xb_cs',  [[440,178],[440,160]],   '进 core',  [444, 171]],
  ['e_cs_xb',  [[500,160],[500,178]],   'local',    [504, 171]],
  ['e_xb_rm',  [[560,238],[610,238],[610,300],[660,300]], 'Reduce Data ×3'],
  ['e_rm_xb',  [[660,330],[600,330],[600,244],[560,244]], '结果回注', [604, 342]],
  ['e_rtab',   [[760,160],[760,175],[270,175],[270,178]], ''],
  ['e_cs_dte', [[420,72],[420,56]],     'in_core',  [424, 70]],
  ['e_dte_cs', [[470,56],[470,72]],     'out_core', [474, 70]],
  ['e_cs_drv', [[320,56],[320,72]],     ''],
  ['e_cs_ts',  [[300,116],[248,116],[248,64],[180,64],[180,56]], 'trigger', [251, 110]],
  ['e_rm_ts',  [[660,350],[630,350],[630,418],[6,418],[6,36],[40,36]], 'rmem2ts_done', [470, 429]],
  ['e_ts_cm',  [[100,56],[100,72]],     ''],
  ['e_cm_ts',  [[160,72],[160,56]],     'notify',   [156, 69, 'end']],
  ['e_ts_ret', [[40,50],[12,50],[12,388],[320,388]], 'user_retire', [16, 399]],
  ['e_reis',   [[280,344],[292,344],[292,62],[532,62],[532,56]], '暂存 / 取出', [288, 298, 'end']],
];
// 活动条上的九个口：前四个是进 Router，后五个是 Xbar 往外发。
const RPORT = [['进 mid','fwd_mid'], ['进 left','fwd_left'], ['进 right','fwd_right'],
               ['进 core','fwd_core'], ['出 mid','out_mid'], ['出 left','out_left'],
               ['出 right','out_right'], ['出 core','out_core'], ['出 归约','out_rdc']];
// 累计数换成每次的增量：[t0, dv0, t1, dv1, ...]，只留增量大于 0 的。
const pulseCache = {};
function pulsesOf(c, k, s){
  const key = c + "." + k + "." + s;
  if(pulseCache[key]) return pulseCache[key];
  const a = D.core[key] || [], out = [];
  let prev = 0;
  for(let j = 0; j < a.length; j += 2){
    const dv = a[j+1] - prev; prev = a[j+1];
    if(dv > 0) out.push(a[j], dv);
  }
  return pulseCache[key] = out;
}

function drawRouter(cid, k){
  const co = (chipById[cid].cores.find(x => x.i === k)) || {role:true};
  const W = 1040, H = 630;
  const svg = el('svg', {viewBox:`0 0 ${W} ${H}`, preserveAspectRatio:'xMidYMid meet'});
  const R = s => CN(cid, k, s), Tot = s => CS(cid, k, s);

  const eact = {
    e_in_l: R('fwd_left'),   e_l_xb: R('fwd_left'),
    e_out_l: R('out_left'),  e_xb_l: R('out_left'),
    e_in_r: R('fwd_right'),  e_r_xb: R('fwd_right'),
    e_out_r: R('out_right'), e_xb_r: R('out_right'),
    e_in_m: R('fwd_mid'),    e_m_xb: R('fwd_mid'),
    e_out_m: R('out_mid'),   e_xb_m: R('out_mid'),
    e_xb_cs: R('out_core'),
    e_cs_xb: R('fwd_core'),  e_dte_cs: R('fwd_core'),
    e_xb_rm: R('out_rdc'),   e_rm_xb: R('reduce_q'),
    e_cs_ts: R('cs_trig'),   e_ts_ret: R('retire'),
    e_reis: R('reissue'),    e_ts_cm: R('cmcm_q'), e_cm_ts: R('cmcm_q'),
  };
  for(const [id, pts] of REDGE){
    const d = pts.map((p, n) => (n ? 'L' : 'M') + p[0] + ',' + p[1]).join(' ');
    svg.appendChild(el('path', {class:'ed' + (eact[id] ? ' act' : ''), d}));
  }
  // 标注在全部连线之后画，别的线从它底下过，描边把线遮住。标注默认挨着起点；
  // 起点在框边上、标注会压进框里或压到别的线上的那几条另给了位置。
  for(const [, pts, lbl, at] of REDGE){
    const [lx, ly, anchor] = at || [pts[0][0] + 4, pts[0][1] - 3];
    if(lbl) svg.appendChild(el('text', {class:'edl', x:lx, y:ly,
      'text-anchor':anchor || 'start'})).textContent = lbl;
  }

  const inR = R('fwd_mid') + R('fwd_left') + R('fwd_right') + R('fwd_core');
  const outR = R('out_mid') + R('out_left') + R('out_right') + R('out_core') + R('out_rdc');
  const bact = {
    rs_l: R('fwd_left') + R('out_left'), rs_r: R('fwd_right') + R('out_right'),
    rs_m: R('fwd_mid') + R('out_mid'),   xb: inR + outR,
    cs: R('out_core') + R('fwd_core') + R('cs_trig'),
    rmod: R('out_rdc') + R('reduce_q'), reis: R('reissue'), ret: R('retire'),
    cmcm: R('cmcm_q'), ts: co.role ? R('cs_trig') + R('retire') : 0,
    dte: co.role ? R('fwd_core') : 0,
  };
  const note = {
    ts:   [co.role ? `在飞 ${R('ts_inflight')} 笔` : '不派角色，没有 TS'],
    dte:  [co.role ? `出核缓冲 ${R('core_out')}` : '不派角色，没有 DTE'],
    cmcm: [`排队等资源 ${R('cmcm_q')} 笔`, '出核前查下游 stream credit'],
    cs:   [`进核那一段压着 ${R('core_in')} flit`,
           `出核进 Router 本拍 ${R('fwd_core')} · VC 占用 ${R('occ_core')}`,
           `发给 TS 的 trigger 累计 ${Tot('cs_trig')}`],
    rtab: ['静态配置，不记波形', '64 条，按 path_id 查'],
    rs_l: [`进 本拍 ${R('fwd_left')} · 累计 ${Tot('fwd_left')}`, `VC 占用 ${R('occ_left')}`,
           `出 本拍 ${R('out_left')} · 累计 ${Tot('out_left')}`],
    rs_r: [`进 ${R('fwd_right')}（累计 ${Tot('fwd_right')}）· VC ${R('occ_right')}`,
           `出 ${R('out_right')}（累计 ${Tot('out_right')}）`],
    rs_m: [`进 ${R('fwd_mid')}（累计 ${Tot('fwd_mid')}）· VC ${R('occ_mid')}`,
           `出 ${R('out_mid')}（累计 ${Tot('out_mid')}）`],
    xb:   [`没发出去 本拍 ${R('xbar_stall')} · 累计 ${Tot('xbar_stall')}`,
           `往 core ${R('out_core')} · 往归约 ${R('out_rdc')}`],
    rmod: [`用户上下文 ${R('rdc_ctx')} 个 · 出口队列 ${R('reduce_q')}`,
           `收进来 本拍 ${R('out_rdc')} · 累计 ${Tot('out_rdc')}`, 'RMW 原位累加，中间 FP32'],
    reis: [`暂存着 ${R('reissue')} 笔`, 'stall_way 转存时走这里'],
    ret:  [`广播过 ${Tot('retire')} 个用户`],
  };
  for(const [id, x, y, w, h, t] of RBOX){
    const g = el('g', {class:'nd' + (id === 'xb' && R('xbar_stall') ? ' blk'
                                   : (bact[id] ? ' act' : ''))});
    g.appendChild(el('rect', {x, y, width:w, height:h, rx:5}));
    g.appendChild(el('text', {class:'t', x:x+9, y:y+17, 'font-size':11.5,
      'font-weight':600})).textContent = t;
    (note[id] || []).forEach((line, n) =>
      g.appendChild(el('text', {class:'cnt', x:x+9, y:y+31+n*13})).textContent = line);
    svg.appendChild(g);
  }

  // ── 端口活动条：九个口各一行，每根竖线是一拍里过了几个 flit ──
  const GX0 = 90, GX1 = 990, PY = 452, RH = 14, AXIS = PY + RPORT.length * RH + 4;
  let w0 = Infinity, w1 = -Infinity;
  for(const [, s] of RPORT){
    const p = pulsesOf(cid, k, s);
    if(p.length){ w0 = Math.min(w0, p[0]); w1 = Math.max(w1, p[p.length - 2]); }
  }
  if(!isFinite(w0)){ w0 = 0; w1 = D.t_end; }
  const pad = Math.max(10, Math.round((w1 - w0) * 0.04));
  w0 = Math.max(0, w0 - pad); w1 = Math.min(D.t_end, w1 + pad);
  if(w1 <= w0) w1 = w0 + 1;
  const tx = t => GX0 + (GX1 - GX0) * (t - w0) / (w1 - w0);

  svg.appendChild(el('text', {class:'grpl', x:GX0, y:PY - 8})).textContent =
    `端口活动　每根竖线是一拍里过了几个 flit　第 ${w0} ～ ${w1} 拍`;
  const bg = el('rect', {class:'gbg', x:GX0 - 6, y:PY - 2, width:GX1 - GX0 + 12,
                         height:AXIS - PY + 4});
  bg.addEventListener('pointerdown', (e) => {
    play(false);
    const r = svg.getBoundingClientRect();
    const s = Math.min(r.width / W, r.height / H);
    drag = {ox: r.left + (r.width - W * s) / 2, s, x0: GX0, x1: GX1, w0, w1};
    ganttMove(e);
    e.preventDefault();
  });
  svg.appendChild(bg);
  RPORT.forEach(([lbl, s], i) => {
    const y = PY + i * RH;
    svg.appendChild(el('text', {class:'glane', x:GX0 - 10, y:y + 10,
      'text-anchor':'end'})).textContent = lbl;
    svg.appendChild(el('path', {class:'ed', opacity:.2, d:`M${GX0},${y + RH - 2} L${GX1},${y + RH - 2}`}));
    const p = pulsesOf(cid, k, s);
    let d = '';
    for(let j = 0; j < p.length; j += 2){
      const X = tx(p[j]), hgt = Math.min(1, p[j+1] / 4) * (RH - 3) + 2;
      d += `M${X},${y + RH - 2} L${X},${y + RH - 2 - hgt} `;
    }
    if(d) svg.appendChild(el('path', {class:'ed act', d, 'stroke-width':1.3, opacity:1}));
  });
  svg.appendChild(el('path', {class:'ed', opacity:.8, d:`M${GX0},${AXIS} L${GX1},${AXIS}`}));
  for(const t of niceTicks(w0, w1)){
    const X = tx(t);
    svg.appendChild(el('path', {class:'ed', opacity:.8, d:`M${X},${AXIS} L${X},${AXIS + 4}`}));
    svg.appendChild(el('text', {class:'gtick', x:X, y:AXIS + 14,
      'text-anchor':'middle'})).textContent = t;
  }
  if(T >= w0 && T <= w1){
    const X = tx(T);
    svg.appendChild(el('path', {class:'gcur', d:`M${X},${PY - 2} L${X},${AXIS + 4}`}));
  }
  svg.appendChild(el('text', {class:'legend', x:GX0, y:AXIS + 32})).textContent =
    '上面：本拍有 flit 经过的线点亮，Xbar 红框 = 本拍有笔没发出去　'
    + '下面：在活动条上按住拖动逐拍走，红线 = 当前拍';
  return svg;
}

function ganttMove(e){
  if(!drag) return;
  const x = (e.clientX - drag.ox) / drag.s;
  const f = Math.max(0, Math.min(1, (x - drag.x0) / (drag.x1 - drag.x0)));
  T = Math.round(drag.w0 + f * (drag.w1 - drag.w0));
  syncFi(); schedule();
}

// ── 右栏 ──
function panel(){
  const name = document.getElementById('pickname');
  const sub  = document.getElementById('picksub');
  const body = document.getElementById('pickbody');
  const hint = document.getElementById('pickhint');
  body.innerHTML = ''; hint.innerHTML = '';
  const rows = (pairs) => {
    const t = document.createElement('table'); t.className = 'lg';
    t.innerHTML = '<tr><th>信号</th><th>本拍</th><th>累计</th></tr>' + pairs.map(
      ([a,b,c]) => `<tr><td>${a}</td><td class="n">${b}</td>` +
                   `<td class="n">${c === undefined ? '' : c}</td></tr>`).join('');
    body.appendChild(t);
  };
  const kv = (pairs) => {
    const t = document.createElement('table'); t.className = 'lg';
    t.innerHTML = pairs.map(([a,b]) => `<tr><td>${a}</td><td>${b}</td></tr>`).join('');
    body.appendChild(t);
  };
  if(view.k === 'grid'){
    name.textContent = '整座阵列';
    sub.textContent = `${D.chips.length} 颗 chip · 共 ${D.t_end} 拍`;
    let f = 0, j = 0, tk = 0;
    for(const ch of D.chips){ const s = chipStat(ch); f += s.flow; j += s.jam; tk += s.task; }
    rows([['本拍片内转发 flit', f], ['本拍 Xbar 没发出去', j], ['在飞的 stream', tk]]);
    hint.innerHTML = '点一颗 chip 进片内，再点一个 core 看它的分步。'
      + '底下的进度条按住拖动，滚轮单帧。';
    return;
  }
  if(view.k === 'chip'){
    const ch = chipById[view.c], st = chipStat(ch);
    name.textContent = 'chip' + ch.id;
    sub.textContent = `${ch.cores.length} 个 core · 摆成 2 × ${ch.cols}`;
    rows([['本拍片内转发 flit', st.flow], ['VC Buffer 占用合计', st.occ],
          ['Xbar 没发出去', st.jam], ['在飞的 stream', st.task],
          ['N 口在途', XS(ch.id,'n','out') + XS(ch.id,'n','in')],
          ['E 口在途', XS(ch.id,'e','out') + XS(ch.id,'e','in')],
          ['W 口在途', XS(ch.id,'w','out') + XS(ch.id,'w','in')],
          ['S 口在途', XS(ch.id,'s','out') + XS(ch.id,'s','in')]]);
    return;
  }
  if(view.k === 'router'){
    const rs = coreStat(view.c, view.k2);
    name.textContent = `chip${view.c} · core${view.k2} · Router`;
    sub.textContent = '照设计文档的方框摆，每个框写本拍的量，有 flit 经过的线点亮';
    const groups = [
      ['从哪进来', ['fwd_mid', 'fwd_left', 'fwd_right', 'fwd_core']],
      ['入口 VC Buffer 占用', ['occ_mid', 'occ_left', 'occ_right', 'occ_core']],
      ['Xbar 往哪发', ['out_mid', 'out_left', 'out_right', 'out_core', 'out_rdc', 'xbar_stall']],
      ['CoreStation', ['core_in', 'cs_trig']],
      ['ReduceModule', ['reduce_q', 'rdc_ctx']],
      ['重发 · Retire · CreditMonitor', ['reissue', 'retire', 'cmcm_q']],
    ];
    for(const [gname, list] of groups){
      const h = document.createElement('h3'); h.className = 'sec'; h.textContent = gname;
      body.appendChild(h);
      rows(list.map(s => [s, rs[s], CUM.has(s) ? rs.tot[s] : undefined]));
    }
    hint.innerHTML = '下面的端口活动条按住拖动逐拍走。RouterTable 是静态配置，不记波形。'
      + '方框与线的走法照 <code>09-router-*.html</code>，逐框对得上。';
    return;
  }
  const st = coreStat(view.c, view.k2);
  const co = chipById[view.c].cores.find(x => x.i === view.k2) || {role:true};
  const sp = stepsOf(view.c, view.k2), si = stepAt(view.c, view.k2);
  name.textContent = `chip${view.c} · core${view.k2}`;
  sub.textContent = co.role ? '派了角色，Router / TS / DTE 都在跑'
                            : '不派角色，只有 Router 转发';
  if(sp.issue.length){
    const h = document.createElement('h3'); h.className = 'sec';
    h.textContent = `第 ${si + 1} / ${sp.issue.length} 步`;
    body.appendChild(h);
    const bo = barsOf(view.c, view.k2);
    if(si >= 0){
      const e = sp.issue[si], b = bo.bars[si], nxt = sp.issue[si+1];
      const fin = !bo.serial ? '几笔并发，分不清是哪一笔的'
                : (b.t1 === null ? '到结束还没完成' : `第 ${b.t1} 拍，用了 ${b.t1 - b.t0} 拍`);
      kv([['下发给', UNAME[e.u]], ['task 号', e.task], ['下发', `第 ${e.t} 拍`],
          ['离本拍', `${T - e.t} 拍`], ['完成', fin],
          ['下一笔', nxt ? `第 ${nxt.t} 拍 · ${UNAME[nxt.u]} task ${nxt.task}` : '没有了']]);
    } else {
      kv([['还没下发', '第一笔在第 ' + sp.issue[0].t + ' 拍']]);
    }
    const h2 = document.createElement('h3'); h2.className = 'sec';
    h2.textContent = '整条链';
    body.appendChild(h2);
    const t = document.createElement('table'); t.className = 'lg';
    t.innerHTML = '<tr><th>#</th><th>单元</th><th>task</th><th>下发</th><th>用时</th></tr>' +
      sp.issue.map((e, n) => {
        const b = bo.bars[n];
        const d = !bo.serial ? '' : (b.t1 === null ? '未完成' : b.t1 - b.t0);
        return `<tr class="${n === si ? 'chg' : ''}">` +
          `<td>${n}</td><td>${UNAME[e.u]}</td><td class="n">${e.task}</td>` +
          `<td class="n">${e.t}</td><td class="n">${d}</td></tr>`; }).join('');
    body.appendChild(t);
  }
  const h3 = document.createElement('h3'); h3.className = 'sec';
  h3.textContent = '当拍的信号'; body.appendChild(h3);
  const list = (co.role ? SIGS : SIGS.filter(s => !s.startsWith('ts_') && s !== 'core_out'))
    .filter(s => !RSIG.has(s));
  rows(list.map(s => [s, st[s], CUM.has(s) ? st.tot[s] : undefined]));
  hint.innerHTML = '这一级的「步」是 TS 每下发一笔 task。'
    + '‹ 上一步 / 下一步 › 按笔走；在甘特图上按住拖动逐拍走，点一段跳到它下发的那一拍。'
    + '下发序列可以直接和 bundle 里的 <code>TCHAIN</code> 逐项对。'
    + 'Router 那一组信号点方框图里的 Router 进去看。';
}

// ── 导航与播放 ──
function nav(){
  const n = document.getElementById('nav'); n.innerHTML = '';
  const add = (label, on, fn) => {
    const b = document.createElement('button');
    b.textContent = label; if(on) b.className = 'on';
    b.onclick = fn; n.appendChild(b);
  };
  const sep = () => { const s = document.createElement('span');
    s.className = 'sep'; s.textContent = '›'; n.appendChild(s); };
  add('阵列', view.k === 'grid', () => { view = {k:'grid'}; sel = null; render(); });
  if(view.k !== 'grid'){
    sep(); add('chip' + view.c, view.k === 'chip',
      () => { view = {k:'chip', c:view.c}; sel = {k:'chip', c:view.c}; render(); });
  }
  if(view.k === 'core' || view.k === 'router'){
    sep(); add('core' + view.k2, view.k === 'core',
      () => { view = {k:'core', c:view.c, k2:view.k2}; render(); });
  }
  if(view.k === 'router'){ sep(); add('Router', true, () => {}); }
}

function render(){
  nav();
  const bystep = view.k === 'core' && stepsOf(view.c, view.k2).issue.length;
  document.getElementById('prev').textContent = bystep ? '‹ 上一步' : '‹ 上一帧';
  document.getElementById('next').textContent = bystep ? '下一步 ›' : '下一帧 ›';
  const stage = document.getElementById('stage');
  stage.innerHTML = '';
  stage.appendChild(view.k === 'grid' ? drawGrid()
                  : view.k === 'chip' ? drawChip(view.c)
                  : view.k === 'router' ? drawRouter(view.c, view.k2)
                                      : drawCore(view.c, view.k2));
  panel();
  document.getElementById('cyc').textContent = `第 ${T} / ${D.t_end} 拍`;
  const pct = (D.t_end ? 100 * T / D.t_end : 0) + '%';
  document.getElementById('barfill').style.width = pct;
  document.getElementById('thumb').style.left = pct;
}

// 播放按「有事件的那些拍」走，中间没有任何信号变化的拍直接跳过。
let fi = 0;
// 让「单帧」的位置跟上 T：取不晚于 T 的最后一个有事件的拍。
function syncFi(){
  fi = 0; let lo = 0, hi = D.frames.length - 1;
  while(lo <= hi){ const m = (lo+hi)>>1;
    if(D.frames[m] <= T){ fi = m; lo = m+1; } else hi = m-1; }
}
// 拖动时鼠标事件比屏幕刷新快得多，攒到下一帧只画一次。
let pending = false;
function schedule(){
  if(pending) return;
  pending = true;
  requestAnimationFrame(() => { pending = false; render(); });
}
function seek(t){ T = Math.max(0, Math.min(D.t_end, t)); syncFi(); render(); }
function seekSoon(t){ T = Math.max(0, Math.min(D.t_end, t)); syncFi(); schedule(); }
function stepBy(n){
  // core 那一级一步就是一笔 task 下发，别的两级一步是一帧。
  if(view.k === 'core'){
    const sp = stepsOf(view.c, view.k2);
    if(sp.issue.length){
      let i = stepAt(view.c, view.k2) + (n > 0 ? 1 : (n < 0 ? -1 : 0));
      i = Math.max(0, Math.min(sp.issue.length - 1, i));
      T = sp.issue[i].t; syncFi(); render(); return;
    }
  }
  fi = Math.max(0, Math.min(D.frames.length - 1, fi + n));
  T = D.frames[fi] ?? T; render();
}
function play(on){
  playing = on;
  document.getElementById('play').textContent = on ? '⏸ 暂停' : '▶ 播放';
  if(timer) clearInterval(timer);
  if(on) timer = setInterval(() => {
    if(fi >= D.frames.length - 1){ play(false); return; }
    stepBy(stride);
  }, 1000 / fps);
}

document.getElementById('play').onclick = () => play(!playing);
document.getElementById('prev').onclick = () => { play(false); stepBy(-1); };
document.getElementById('next').onclick = () => { play(false); stepBy(1); };
// 进度条：按下就跳，按住拖动一路跟着，滚轮单帧。按下之后抓住指针，拖出条外也不断。
const bar = document.getElementById('bar');
const barT = (e) => {
  const r = bar.getBoundingClientRect();
  return Math.round(D.t_end * Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)));
};
bar.addEventListener('pointerdown', (e) => {
  play(false); bar.classList.add('drag');
  if(bar.setPointerCapture) bar.setPointerCapture(e.pointerId);
  seekSoon(barT(e)); e.preventDefault();
});
bar.addEventListener('pointermove', (e) => {
  if(bar.classList.contains('drag')) seekSoon(barT(e));
});
const barEnd = (e) => {
  bar.classList.remove('drag');
  try{ if(bar.releasePointerCapture) bar.releasePointerCapture(e.pointerId); }catch(_){}
};
bar.addEventListener('pointerup', barEnd);
bar.addEventListener('pointercancel', barEnd);
bar.addEventListener('wheel', (e) => {
  e.preventDefault(); play(false); stepBy(e.deltaY > 0 ? 1 : -1);
}, {passive:false});
// 甘特图上的拖动：底板按下之后，移动与松开在整个窗口上收，画面重画了也接得上。
window.addEventListener('pointermove', ganttMove);
window.addEventListener('pointerup', () => { drag = null; });
window.addEventListener('pointercancel', () => { drag = null; });
document.getElementById('spd').oninput = (e) => {
  stride = Number(e.target.value);
  document.getElementById('spdv').textContent = '×' + stride;
};
window.addEventListener('keydown', (e) => {
  if(e.code === 'Space'){ e.preventDefault(); play(!playing); }
  if(e.code === 'ArrowLeft'){ play(false); stepBy(-1); }
  if(e.code === 'ArrowRight'){ play(false); stepBy(1); }
  if(e.code === 'Escape' && view.k !== 'grid'){
    view = view.k === 'router' ? {k:'core', c:view.c, k2:view.k2}
         : view.k === 'core' ? {k:'chip', c:view.c} : {k:'grid'};
    render(); }
});
seek(0);
"""


def build_html(prefix: str, data: dict) -> str:
    name = Path(prefix).name
    payload = json.dumps(data, separators=(",", ":"))
    sigs = json.dumps(WAVE_SIGS, ensure_ascii=False)
    cums = json.dumps(CUM_SIGS, ensure_ascii=False)
    rsigs = json.dumps(ROUTER_SIGS, ensure_ascii=False)
    return f"""<!doctype html>
<html lang="zh"><head><meta charset="utf-8">
<title>{name} · 波形动画</title>
<style>{CSS}</style></head>
<body>
<header>
  <h1>{name} · 波形动画<small>阵列 → chip → core，画面与数字全部由波形驱动</small></h1>
  <nav id="nav"></nav>
</header>
<main>
  <div id="stagewrap"><div id="stage"></div></div>
  <aside>
    <div id="pickname"></div>
    <div id="picksub"></div>
    <h3 class="sec">本拍的值</h3>
    <div id="pickbody"></div>
    <div id="pickhint" class="hint"></div>
  </aside>
</main>
<footer>
  <button class="pri" id="play">▶ 播放</button>
  <button id="prev">‹ 上一帧</button>
  <button id="next">下一帧 ›</button>
  <span id="cyc"></span>
  <div id="bar" title="按住拖动；滚轮单帧"><div id="barfill"></div><div id="thumb"></div></div>
  <label class="spd">步长 <input type="range" id="spd" min="1" max="40" step="1" value="1">
    <span id="spdv">×1</span></label>
  <span id="kbd">空格 播放/暂停　← → 单帧　Esc 退上一级</span>
</footer>
<script>
const D = {payload};
const SIGS = {sigs};
const CUM = new Set({cums});
const RSIG = new Set({rsigs});
{JS}
</script>
</body></html>
"""


def generate(prefix: str, out: str) -> str:
    data = collect(prefix)
    html = build_html(prefix, data)
    Path(out).write_text(html, encoding="utf-8")
    return out


def is_latch_trace(p: Path) -> bool:
    """只认 latch 写的波形。别的工具也用 .trace 这个后缀（3rdparties 里就有），
    选中那种会在读的时候崩。认法照上面的 Reader：文件尾 16 字节是两个偏移，
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
        print(f"[replay] 有 {len(traces)} 份波形，不在终端里，挑最新的一份："
              f"{show(traces[0], cwd)}", file=sys.stderr)
        return traces[0]

    print(f"[replay] {cwd} 下有 {len(traces)} 份波形：", file=sys.stderr)
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
            raw = input(f"[replay] 选一个 [0-{len(traces) - 1}，直接回车用 0，q 退出]："
                        ).strip()
        except (EOFError, KeyboardInterrupt):
            sys.exit("\n[replay] 算了")
        if raw == "":
            return traces[0]
        if raw in ("q", "Q"):
            sys.exit("[replay] 算了")
        if raw.isdigit() and int(raw) < len(traces):
            return traces[int(raw)]
        print(f"[replay] '{raw}' 不在 0～{len(traces) - 1} 里，重来", file=sys.stderr)


def resolve_trace(arg: str | None) -> Path:
    """把命令行给的那一截认成一份波形。不给就把当前目录下的列出来挑。"""
    cwd = Path.cwd()
    if arg:
        for cand in (Path(arg), Path(arg + ".trace")):
            if cand.is_file():
                return cand
        hits = [p for p in list_traces(cwd) if arg in str(p)]
        if not hits:
            sys.exit(f"[replay] {cwd} 下没有名字里带 '{arg}' 的 .trace")
        return pick(hits, cwd)

    traces = list_traces(cwd)
    if not traces:
        sys.exit(f"[replay] {cwd} 下一个 .trace 都没有")
    return pick(traces, cwd)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="replay", description="把 Bach 的波形回放成一页 HTML 动画")
    p.add_argument("prefix", nargs="?", default=None,
                   help="波形路径或名字里的一截，不给就把当前目录下的列出来挑")
    p.add_argument("-o", "--out", default=None,
                   help="产物路径，默认落在波形旁边，同名换成 .html")
    args = p.parse_args(argv)

    trace = resolve_trace(args.prefix)
    # 不指定就落在波形旁边，同名换成 .html。
    out = Path(args.out) if args.out else trace.with_suffix(".html")
    generate(str(trace.with_suffix("")), str(out))
    print(f"[replay] {out.resolve()}  ({out.stat().st_size / 1024:.0f} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
