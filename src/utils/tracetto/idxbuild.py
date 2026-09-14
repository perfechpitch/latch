"""建索引：按 core 并行、原子发布、复用没变的。

- **按 core 切**：一个 core 的六个信号 → 九条行是天然不可分的单位（`core_spans` 要这六个），
  408 个 core 就是 408 个任务；先拿每个 core 的事件数当权重，降序动态派发，免得一个重
  core 拖尾。
- **用 spawn 不用 fork**：`/api/reload` 触发的重建是在服务已经起了线程之后，fork 只继承
  调用线程、别的线程可能正握着锁 → 经典死锁；而这里 fork 又几乎没有好处（worker 反正要
  自己开 `TraceReader`）。代价是每个 worker 重新 import 一次（~20 ms）。
- **原子发布**：先写进 `.building.<pid>.<xx>/`，全部写完、**再确认一次波形没被改过**、
  最后写 manifest（它就是完成标记），然后 `os.rename` 过去。目标是「另一个进程正在建的
  半成品永远不会被读到」，以及「两个进程同时建，只有一个赢」。
"""

from __future__ import annotations

import multiprocessing as mp
import os
import random
import shutil
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from . import index, spans as S
from .reader import TraceReader

BUILDING_PREFIX = ".building."
STALE_SECONDS = 3600


def _write_one(reader: TraceReader, job) -> Tuple[int, List[int]]:
    """解一个 core 的九条行，写成 `cores/<idx>.bin`。"""
    _prefix, core_idx, sigs, t_end, cores_dir, lane_base = job
    rows = S.lanes_of(S.core_spans(reader, sigs, t_end))
    (Path(cores_dir) / f"{core_idx:05d}.bin").write_bytes(
        index.encode_core(core_idx, rows, lane_base, t_end))
    # 一个 core 的信号不会出现在别的 core 上，解完就把缓存放了：内存不随波形长大。
    reader.cache.clear()
    reader._segs.clear()
    return core_idx, [len(sp) for sp in rows]


def _worker(chunk):
    """子进程：开一次波形，把它分到的几个 core 全写完。

    一个进程只开一次文件 —— 打开要解 meta 表（上万个模块名），每个 core 开一次的话
    光这一项就是几百毫秒到几秒的纯开销。
    """
    out = []
    with TraceReader(chunk[0][0]) as r:
        for job in chunk:
            out.append(_write_one(r, job))
    return out


def plan(trace: Path) -> Dict:
    """开一遍波形：树、每个 core 要读哪几个信号、末尾、缺哪些信号、每个 core 多重。"""
    prefix = str(trace.with_suffix(""))
    with TraceReader(prefix) as r:
        chips, core_sig = S.core_paths(r)
        t_end = S.trace_t_end(r, core_sig)
        missing = S.missing_signals(r, core_sig)
        weights = {}
        for key, sigs in core_sig.items():
            n = 0
            for sid in set(sigs.values()):
                segs = r.one_pass(sid)
                n += sum(s.event_count for s in segs)
            weights[key] = n
    keys = sorted(chips, key=lambda c: c)                       # chip 号
    ordered: List[Tuple[int, str]] = []
    for chip in keys:
        for core in sorted(chips[chip]):
            ordered.append((chip, f"{chip}.{core}"))
    return {
        "prefix": prefix, "chips": chips, "core_sig": core_sig, "t_end": t_end,
        "missing": missing, "ordered": ordered, "weights": weights,
    }


def _clean_stale(parent: Path) -> None:
    """清掉以前留下的临时目录（进程没了、或者放了一天以上）。"""
    try:
        entries = list(parent.iterdir())
    except OSError:
        return
    now = time.time()
    for p in entries:
        if not p.name.startswith(BUILDING_PREFIX) or not p.is_dir():
            continue
        parts = p.name.split(".")
        pid = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else -1
        alive = (pid > 0 and os.path.exists(f"/proc/{pid}")) \
            if sys.platform.startswith("linux") else True
        try:
            old = now - p.stat().st_mtime > STALE_SECONDS
        except OSError:
            continue
        if old or not alive:
            shutil.rmtree(p, ignore_errors=True)


def _try_lock(path: Path):
    """抢一把文件锁。抢不到返回 None（靠 rename 兜底，不会挂死）。"""
    try:
        import fcntl
    except ImportError:
        return None
    f = open(path, "a+")
    try:
        fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return f
    except OSError:
        f.close()
        return None


def _progress_line(done: int, total: int, t0: float) -> str:
    used = time.time() - t0
    eta = used / done * (total - done) if done else 0.0
    pct = 100 * done // max(1, total)
    return (f"[tracetto]   建索引 {done}/{total}  {pct}%  已用 {used:.1f}s"
            f"  还要 ~{eta:.0f}s")


def build(trace: Path, workers: Optional[int] = None, progress: bool = True,
          quiet: bool = False) -> Dict:
    """建一份索引并发布，返回 manifest。"""
    out_dir = index.index_dir(str(trace.with_suffix("")))
    parent = out_dir.parent
    progress = progress and not quiet
    plan_info = plan(trace)
    ordered = plan_info["ordered"]
    core_sig = plan_info["core_sig"]
    n_cores = len(ordered)
    if workers is None:
        workers = max(1, min(os.cpu_count() or 1, n_cores))
    workers = max(1, min(workers, n_cores))

    for attempt in range(2):
        if not quiet:
            print(f"[tracetto] 建索引 {trace.name}：{n_cores} 个 core / {workers} 进程",
                  flush=True)
        ident_before = index.source_identity(trace)
        tmp = parent / f"{BUILDING_PREFIX}{os.getpid()}.{random.randrange(1 << 20):x}"
        cores_dir = tmp / index.CORES_DIR
        cores_dir.mkdir(parents=True, exist_ok=False)

        jobs = []
        lane_base_of: Dict[int, int] = {}
        data_ordinal = 0
        for ci, (_chip, key) in enumerate(ordered):
            sigs = core_sig.get(key)
            if not sigs:
                continue                      # 只转发的 core：只有树上一个名字，没有数据
            lane_base_of[ci] = data_ordinal * S.LANES_PER_CORE
            data_ordinal += 1
            jobs.append((plan_info["prefix"], ci, sigs, plan_info["t_end"],
                         str(cores_dir), lane_base_of[ci]))
        jobs.sort(key=lambda j: -plan_info["weights"].get(ordered[j[1]][1], 0))

        lane_n: List[int] = [0] * (data_ordinal * S.LANES_PER_CORE)
        t0 = time.time()
        done = 0
        tty = progress and sys.stderr.isatty()

        def record(ci: int, counts: List[int]) -> None:
            lane_n[lane_base_of[ci]:lane_base_of[ci] + len(counts)] = counts

        try:
            if workers == 1:
                with TraceReader(plan_info["prefix"]) as r:
                    for job in jobs:
                        ci, counts = _write_one(r, job)
                        record(ci, counts)
                        done += 1
                        if progress:
                            _tick(done, len(jobs), t0, tty)
            else:
                # jobs 已经按权重降序排好，交错分（jobs[i::workers]）每个进程的轻重就均匀。
                chunks = [c for c in (jobs[i::workers] for i in range(workers)) if c]
                ctx = mp.get_context("spawn")
                with ctx.Pool(processes=min(workers, len(chunks))) as pool:
                    for part in pool.imap_unordered(_worker, chunks, chunksize=1):
                        for ci, counts in part:
                            record(ci, counts)
                            done += 1
                        if progress:
                            _tick(done, len(jobs), t0, tty)
            if progress and tty:
                sys.stderr.write("\n")

            missing_files = [j[1] for j in jobs
                             if not (cores_dir / f"{j[1]:05d}.bin").exists()]
            if missing_files:
                raise RuntimeError(f"有 {len(missing_files)} 个 core 的索引文件没写出来")

            if index.source_identity(trace) != ident_before:
                raise _Changed()

            manifest = _manifest(trace, plan_info, ordered, lane_base_of,
                                 lane_n, cores_dir)
            index.write_manifest(tmp, manifest)
            try:
                os.rename(tmp, out_dir)
            except OSError:
                # 目标目录已经在。**先验它是不是这份波形的好索引**：是的话说明另一个
                # 进程刚建好，直接用它；不是的话它是过期的（波形改过）或者坏掉的，
                # 清掉再把我们的换上去 —— 不能因为它「读得动」就当成果收下。
                other = index.read_manifest(out_dir)
                if (other is not None
                        and index.identity_matches(trace, other.get("source", {}))
                        and core_files_ok(out_dir, other)):
                    shutil.rmtree(tmp, ignore_errors=True)
                    if not quiet:
                        print("[tracetto] 索引已经被另一个进程建好了，直接用", flush=True)
                    return other
                shutil.rmtree(out_dir, ignore_errors=True)
                try:
                    os.rename(tmp, out_dir)
                except OSError:
                    shutil.rmtree(tmp, ignore_errors=True)
                    if attempt == 0:
                        continue
                    raise
            if not quiet:
                size = sum(f.stat().st_size for f in (out_dir / index.CORES_DIR).iterdir())
                print(f"[tracetto] 索引建好：{n_cores} 个 core / {len(lane_n)} 条行 / "
                      f"{size / 1048576:.1f} MB / {time.time() - t0:.1f}s",
                      file=sys.stderr if tty else sys.stdout, flush=True)
            return manifest
        except _Changed:
            shutil.rmtree(tmp, ignore_errors=True)
            if attempt == 0:
                if not quiet:
                    print("[tracetto] 建的过程中波形被改了，重来一次", file=sys.stderr,
                          flush=True)
                continue
            raise
        except BaseException:
            shutil.rmtree(tmp, ignore_errors=True)
            raise
    raise RuntimeError("索引建不起来")


class _Changed(Exception):
    """建的过程中波形被改了（多半是仿真还在写）。"""


def _tick(done: int, total: int, t0: float, tty: bool) -> None:
    if not tty and (done * 10) % max(1, total) != 0 and done != total:
        return
    line = _progress_line(done, total, t0)
    sys.stderr.write(("\r" + line) if tty else (line + "\n"))
    sys.stderr.flush()


def core_files_ok(out_dir: Path, manifest: Dict) -> bool:
    """索引文件还在不在、有没有被截断。

    manifest 里记了每个 core 文件的字节数，开机时比一遍（408 次 stat，~1 ms）——
    被删掉或者截断过就能当场发现，不用等到读某个窗口时炸。
    """
    sizes = manifest.get("core_size")
    if not sizes:
        return False
    for ci, size in enumerate(sizes):
        if size == 0:
            continue
        p = out_dir / index.CORES_DIR / f"{ci:05d}.bin"
        try:
            if p.stat().st_size != size:
                return False
        except OSError:
            return False
    return True


def _manifest(trace: Path, plan_info: Dict, ordered, lane_base_of: Dict[int, int],
              lane_n: List[int], cores_dir: Path) -> Dict:
    chips: List[list] = []
    for ci, (chip, key) in enumerate(ordered):
        sigs = plan_info["core_sig"].get(key) or {}
        role = 1 if S.SIG_TS_INFLIGHT in sigs else 0
        if not chips or chips[-1][0] != chip:
            chips.append([chip, []])
        chips[-1][1].append([int(key.split(".")[1]), role, lane_base_of.get(ci, -1)])
    return {
        "format": index.INDEX_FORMAT,
        "name": Path(plan_info["prefix"]).name,
        "t_end": plan_info["t_end"],
        "units": list(S.UNITS),
        "rows": list(S.ROW_NAMES),
        "row_parts": S.row_parts(),
        "lanes_per_core": S.LANES_PER_CORE,
        "missing": plan_info["missing"],
        "source": index.source_identity(trace),
        "chips": chips,
        "lane_n": lane_n,
        "core_size": [
            (cores_dir / f"{ci:05d}.bin").stat().st_size
            if (cores_dir / f"{ci:05d}.bin").exists() else 0
            for ci in range(len(ordered))
        ],
    }


def ensure(trace: Path, reindex: bool = False, workers: Optional[int] = None,
           progress: bool = True, quiet: bool = False) -> Tuple[Dict, bool]:
    """要一份能用的索引：没变的直接复用，变了/没有就建。

    返回 `(manifest, 有没有重建)`。
    """
    out_dir = index.index_dir(str(trace.with_suffix("")))
    parent = out_dir.parent
    _clean_stale(parent)                  # 每次启动都扫一遍，别人留下的半成品别攒着
    if not reindex:
        manifest = index.read_manifest(out_dir)
        if (manifest is not None
                and index.identity_matches(trace, manifest.get("source", {}))
                and core_files_ok(out_dir, manifest)):
            return manifest, False
    lock_path = parent / (out_dir.name + ".lock")
    lock = _try_lock(lock_path)
    if lock is None:                      # 别人在建：等他一会儿，中途他建好了就直接用
        deadline = time.time() + 60
        while time.time() < deadline:
            time.sleep(0.2)
            manifest = index.read_manifest(out_dir)
            if (manifest is not None
                    and index.identity_matches(trace, manifest.get("source", {}))
                    and core_files_ok(out_dir, manifest)):
                return manifest, False
            lock = _try_lock(lock_path)
            if lock is not None:
                break
    try:
        return build(trace, workers=workers, progress=progress, quiet=quiet), True
    finally:
        if lock is not None:
            lock.close()
