"""服务：四个端点 + 伺服自己的前端。

协议（自己的，跟 insight 没关系）：

- `GET /api/init` → 树 + 每条行的段数 + build 号。**与波形多大无关的十几 KB**，
  段与标签一概不带（那是按窗口取的）。
- `GET /api/window?lanes=0-8,27&t0=..&t1=..&px=..` → 一帧小 JSON 头 + 原始字节切片，
  切的是索引文件里的段流本身，客户端按同一套布局自己解 varint。服务端不做逐段循环。
- `GET /api/status` → 建索引/重载的进度，前端轮询用。
- `POST /api/reload` → 只重新校验波形身份；没变就直接回，变了才重建。

`/` 与 `/static/*` 伺服 `tracetto/web/`。
"""

from __future__ import annotations

import gzip
import io
import json
import socket
import struct
import sys
import threading
import urllib.parse
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Dict, List, Optional

from . import index, idxbuild

WEB_DIR = Path(__file__).resolve().parent / "web"
FRAME_MAGIC = b"TCW1"
MAX_LANES = 400          # 一次请求最多要几条行；前端自己限在 200
MAX_OPEN_CORES = 64      # 同时开着的 core 文件数（其余靠 LRU 关掉）


def parse_lanes(raw: str) -> List[int]:
    """`lanes=0-8,27` → [0..8, 27]。"""
    out: List[int] = []
    for part in raw.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            lo, hi = int(a), int(b)
            if hi < lo:
                lo, hi = hi, lo
            out.extend(range(lo, hi + 1))
        else:
            out.append(int(part))
    return out


class TraceServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, addr, handler, trace: Path, manifest: Dict,
                 workers: Optional[int]) -> None:
        super().__init__(addr, handler)
        self.trace = trace
        self.prefix = str(trace.with_suffix(""))
        self.workers = workers
        self.lock = threading.Lock()
        self._cores: Dict[int, index.CoreFile] = {}
        self._core_order: List[int] = []
        self.progress = 1.0
        self.building = False
        self.set_manifest(manifest)

    def set_manifest(self, manifest: Dict) -> None:
        self.manifest = manifest
        self.dir = index.index_dir(self.prefix)
        self.build = index.build_id(manifest["source"])
        for cf in self._cores.values():
            cf.close()
        self._cores.clear()
        self._core_order.clear()

    def core(self, ci: int) -> index.CoreFile:
        """按需打开一个 core 文件，开着的最多 `MAX_OPEN_CORES` 个。"""
        got = self._cores.get(ci)
        if got is not None:
            return got
        while len(self._core_order) >= MAX_OPEN_CORES:
            old = self._core_order.pop(0)
            self._cores.pop(old, None).close()
        cf = index.CoreFile(self.dir / index.CORES_DIR / f"{ci:05d}.bin")
        self._cores[ci] = cf
        self._core_order.append(ci)
        return cf


class Handler(SimpleHTTPRequestHandler):
    server_version = "tracetto"
    protocol_version = "HTTP/1.1"

    def __init__(self, *args, **kw):
        kw["directory"] = str(WEB_DIR)
        super().__init__(*args, **kw)

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        self._route(True)

    def do_HEAD(self):
        self._route(False)

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n:
            self.rfile.read(n)          # 不读掉的话 keep-alive 会把 body 当下一个请求
        if urllib.parse.urlsplit(self.path).path == "/api/reload":
            return self._reload()
        self.send_error(404, "not found")

    # ── 路由 ──

    def _route(self, want_body: bool) -> None:
        parts = urllib.parse.urlsplit(self.path)
        if parts.path.startswith("/api/"):
            query = urllib.parse.parse_qs(parts.query, keep_blank_values=True)
            return self._api(parts.path, query, want_body)
        if parts.path in ("/", "/index.html"):
            return self._send_file(WEB_DIR / "index.html", "text/html; charset=utf-8",
                                   head_only=not want_body, cache="no-cache")
        if parts.path.startswith("/static/"):
            rest = parts.path[len("/static"):]
            if rest.endswith("/"):
                return self.send_error(404, "not found")     # 不列目录
            self.path = rest
            return super().do_GET() if want_body else super().do_HEAD()
        self.send_error(404, "not found")

    def _api(self, route: str, query: dict, want_body: bool) -> None:
        head_only = not want_body
        if route == "/api/init":
            return self._init(head_only)
        if route == "/api/window":
            return self._window(query, head_only)
        if route == "/api/status":
            srv = self.server
            return self._send_json({"build": srv.build, "building": srv.building,
                                    "progress": srv.progress}, head_only)
        self.send_error(404, "not found")

    @staticmethod
    def _one(query: dict, key: str) -> Optional[str]:
        got = query.get(key)
        return got[0] if got else None

    def _init(self, head_only: bool) -> None:
        srv = self.server
        m = srv.manifest
        payload = {
            "format": index.INDEX_FORMAT,
            "build": srv.build,
            "name": m["name"],
            "t_end": m["t_end"],
            "units": m["units"],
            "rows": m["rows"],
            "row_parts": m["row_parts"],
            "lanes_per_core": m["lanes_per_core"],
            "missing": m["missing"],
            "chips": m["chips"],
            "lane_n": m["lane_n"],
        }
        body = json.dumps(payload, separators=(",", ":"),
                          ensure_ascii=False).encode("utf-8")
        accept = self.headers.get("Accept-Encoding") or ""
        if want_gzip := ("gzip" in accept and len(body) > 1024):
            body = gzip.compress(body, 6)
        extra = {"Content-Encoding": "gzip"} if want_gzip else None
        return self._send(body, "application/json; charset=utf-8", head_only,
                          "no-cache", extra)

    def _window(self, query: dict, head_only: bool) -> None:
        srv = self.server
        try:
            lanes = parse_lanes(self._one(query, "lanes") or "")
            t0 = int(self._one(query, "t0") or 0)
            t1 = int(self._one(query, "t1") or 0)
            px = max(1, min(8192, int(self._one(query, "px") or 1024)))
        except ValueError:
            return self.send_error(400, "bad number in query")
        if t1 < t0:
            return self.send_error(400, "t1 < t0")
        if len(lanes) > MAX_LANES:
            return self.send_error(400, f"too many lanes (>{MAX_LANES})")
        want_build = self._one(query, "build")
        if want_build is not None and want_build != srv.build:
            # 索引换代了：让前端重拉 init 再来（不是错误，是「你手上的那份过期了」）
            return self.send_error(409, f"stale build {want_build} != {srv.build}")

        entries = []
        chunks = []
        pos = 0
        try:
            for lane_id in lanes:
                # 通道号是「core 序号 × 一个 core 几条通道 + 通道号」，步长与
                # 显示成几行无关（TS 那一行是由三条通道叠出来的）。
                ci, row = divmod(lane_id, srv.manifest["lanes_per_core"])
                if ci >= len(srv.manifest["core_size"]):
                    continue
                # 不派角色的 core 没有索引文件（manifest 里 core_size 记 0），它本来
                # 就没有行：跳过。不能落到下面那个 FileNotFoundError 上 —— 那是留给
                # 「索引文件在跑的时候被人删了」的。
                if srv.manifest["core_size"][ci] == 0:
                    continue
                cf = srv.core(ci)
                w = cf.window(row, t0, t1, px)
                if w is None:
                    continue
                data = cf.read(w["off"], w["end"])
                entry = {"lane": lane_id, "mode": w["mode"], "off": pos, "len": len(data)}
                for k in ("n", "t_base", "cell", "c0", "c1", "stride"):
                    if k in w:
                        entry[k] = w[k]
                entries.append(entry)
                chunks.append(data)
                pos += len(data)
        except FileNotFoundError:
            # 索引文件在跑的时候被人删了：不 500，让前端去 /api/reload
            return self.send_error(503, "index files vanished; POST /api/reload")
        except (OSError, ValueError) as e:
            return self.send_error(500, f"read index failed: {e}")

        head = json.dumps({"build": srv.build, "t0": t0, "t1": t1, "lanes": entries},
                          separators=(",", ":")).encode("utf-8")
        buf = io.BytesIO()
        buf.write(FRAME_MAGIC)
        buf.write(struct.pack("<I", len(head)))
        buf.write(head)
        for c in chunks:
            buf.write(c)
        return self._send(buf.getvalue(), "application/octet-stream", head_only)

    def _reload(self) -> None:
        srv = self.server
        try:
            with srv.lock:
                srv.building = True
                srv.progress = 0.0
                manifest, rebuilt = idxbuild.ensure(
                    srv.trace, workers=srv.workers, quiet=True)
                if rebuilt:
                    srv.set_manifest(manifest)
                srv.building = False
                srv.progress = 1.0
        except Exception as e:                      # noqa: BLE001 —— 旧索引留着，别把页面弄白
            srv.building = False
            print(f"[tracetto] 重载失败：{e}", file=sys.stderr)
            return self.send_error(500, f"reload failed: {e}")
        if rebuilt:
            print(f"[tracetto] 波形变了，重建了索引（{srv.build}）", flush=True)
        self._send_json({"ok": True, "rebuilt": rebuilt, "build": srv.build})

    # ── 发送 ──

    def _send(self, body: bytes, ctype: str, head_only: bool = False,
              cache: Optional[str] = None, extra: Optional[dict] = None) -> None:
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        if cache:
            self.send_header("Cache-Control", cache)
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if body and not head_only:
            self.wfile.write(body)

    def _send_json(self, obj, head_only: bool = False) -> None:
        self._send(json.dumps(obj, separators=(",", ":"), ensure_ascii=False)
                   .encode("utf-8"), "application/json; charset=utf-8", head_only)

    def _send_file(self, path: Path, ctype: str, head_only: bool,
                   cache: Optional[str] = None) -> None:
        try:
            body = path.read_bytes()
        except OSError:
            return self.send_error(404, "not found")
        self._send(body, ctype, head_only, cache)


def find_free_port(host: str, port: int, tries: int = 100) -> int:
    for cand in range(port, port + tries):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                s.bind((host, cand))
                return cand
            except OSError:
                continue
    raise OSError(f"{host} 上从 {port} 起的 {tries} 个端口都占了")


def serve(trace: Path, host: str = "127.0.0.1", port: int = 8766,
          workers: Optional[int] = None, reindex: bool = False,
          progress: bool = True) -> int:
    manifest, rebuilt = idxbuild.ensure(trace, reindex=reindex, workers=workers,
                                        progress=progress)
    n_core = sum(1 for size in manifest.get("core_size", []) if size)
    print(f"[tracetto] {trace.name}：{len(manifest['chips'])} chip / {n_core} core / "
          f"{len(manifest['lane_n'])} 条行 / t_end={manifest['t_end']}"
          f"{'（刚建的索引）' if rebuilt else '（用现成的索引）'}", flush=True)
    if manifest["missing"]:
        print(f"[tracetto] 提醒：这份波形里没有 {' / '.join(manifest['missing'])}，"
              f"缺的信号对应的行会是空的。", file=sys.stderr)

    if port != 0:
        try:
            chosen = find_free_port(host, port)
        except OSError as e:
            print(f"[tracetto] {e}", file=sys.stderr)
            return 2
        if chosen != port:
            print(f"[tracetto] 端口 {port} 占了，改用 {chosen}", file=sys.stderr)
        port = chosen
    try:
        httpd = TraceServer((host, port), Handler, trace, manifest, workers)
    except OSError as e:
        print(f"[tracetto] 绑不上 {host}:{port}：{e}", file=sys.stderr)
        return 2
    port = httpd.server_address[1]
    print(f"[tracetto] 服务 http://{host}:{port}/    （Ctrl-C 停）", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[tracetto] 算了")
    finally:
        httpd.server_close()
    return 0
