"use strict";
// Tracetto 的前端：左树（虚拟列表）+ 右边 canvas 时间轴（按窗口懒加载）。
//
// 规矩：
//   · 时间↔像素只走 t2x / x2t 两个函数，别处一律不许自己换算；
//   · 画多少就要多少 —— 只请求「当前能看见的那些行 + 当前窗口」，请求去抖合并；
//   · 段与段之间没有东西就是空档（不是「上一个值保持」），空档不画。
// GUTTER 是画布左边留出来的一条边距：行名只在左边那棵树上写一遍，画布里不再重复，
// 这条边距就只剩「不让时间标尺最左那个刻度被裁掉」这一个用途。
const ROW_H = 20, GUTTER = 0, RULER_H = 26, PRE = 6;
const MIN_SPAN = 8;
const MAX_LANES = 200;           // 一次最多请求几条行，与服务端的 MAX_LANES 对齐
const PAD = 1.5;                 // 请求窗口在视图两边各留的比例

// 九种颜色，按下标取：0~2 是 TS 那一行里的 DTE / MU / VU，3~8 是另外六行。
// 颜色只跟「行」与「单元」走，不跟 user 走 —— 用户上千个，按 user 上色必然撞色，
// 撞了反而认不出来；谁是谁看段上的 User_id 字。
const PALETTE = ["#1a7f37", "#0a66c2", "#8250df", "#d4a72c", "#cf222e",
                 "#0d8e6b", "#a04100", "#3192aa", "#df3079"];

const $ = (id) => document.getElementById(id);
const cv = $("lane-canvas");
const ctx = cv.getContext("2d");
const scroll = $("scroll");

const S = {
  build: null, t_end: 1, rows: [], row_parts: [], lanes_per_core: 9,
  chips: [], lane_n: [], missing: [],
  entries: [], expanded: new Set(), collapsedChips: new Set(),
  view: { t0: 0, t1: 1 }, cache: new Map(), sel: null, hover: null,
  timer: null, inflight: null, loading: false, first: true,
};

// ── 波形上印的字 ──────────────────────────────────────────────────────
// `User_id 77 MU 5 12拍`：单元从通道来（TS 那一行的三条通道分别是 DTE / MU / VU），
// 不从行名里猜。认不出配对的段画淡一点、字写 `?`，颜色仍是本行的颜色。
function labelOf(unit, seg) {
  if (seg.user < 0 || seg.task < 0) return "?";
  return `User_id ${seg.user} ${unit} ${seg.task} ${seg.t1 - seg.t0}拍`;
}
function colorOf(part) { return PALETTE[part.color % PALETTE.length]; }

// ── 取 init ──────────────────────────────────────────────────────────
async function loadInit() {
  const r = await fetch("/api/init", { cache: "no-store" });
  if (!r.ok) throw new Error("/api/init " + r.status);
  const j = await r.json();
  S.build = j.build; S.t_end = Math.max(1, j.t_end); S.rows = j.rows;
  S.row_parts = j.row_parts; S.lanes_per_core = j.lanes_per_core;
  S.chips = j.chips; S.lane_n = j.lane_n; S.missing = j.missing;
  S.cache.clear(); S.sel = null; S.hover = null;
  if (S.first) {                       // 第一次进来：看全；之后重载保留当前视窗
    S.first = false;
    S.view = { t0: 0, t1: S.t_end };
    for (const [chip] of j.chips) S.collapsedChips.add(chip);   // 起手只列 chip
  }
  if (!(S.view.t1 > S.view.t0) || S.view.t1 > S.t_end * 1.5) S.view = { t0: 0, t1: S.t_end };
  buildEntries();
  $("title").textContent = "Tracetto · " + j.name;
  const ncore = S.chips.reduce((a, c) => a + c[1].length, 0);
  const nrole = S.chips.reduce((a, c) => a + c[1].filter((x) => x[1]).length, 0);
  $("sub").textContent = `${S.chips.length} chip / ${nrole} 个派角色的 core（共 ${ncore}）/ `
    + `${S.lane_n.length / S.lanes_per_core} × ${S.rows.length} 行 / t_end ${S.t_end} · build ${S.build}`;
  const b = $("banner");
  if (S.missing.length) {
    b.hidden = false; b.className = "";
    b.textContent = `这份波形里没有 ${S.missing.join(" / ")}：是加信号之前生成的，`
      + `缺的信号对应的行会是空的。`;
  } else { b.hidden = true; }
}

// ── 树（扁平条目 + 只渲染看得见的那一段）────────────────────────────
function buildEntries() {
  S.entries = [];
  for (const [chip, cores] of S.chips) {
    S.entries.push({ k: "chip", chip, cores });
    if (S.collapsedChips.has(chip)) continue;
    for (const [core, role, lane_base] of cores) {
      S.entries.push({ k: "core", chip, core, role, lane_base });
      if (!role || !S.expanded.has(lane_base)) continue;
      for (let row = 0; row < S.rows.length; row++) {
        // 一行可能由好几条通道画出来（TS 那一行是三条），n 是它们之和。
        const parts = S.row_parts[row].map((p) => ({
          ...p, lane: lane_base + p.lane, n: S.lane_n[lane_base + p.lane] || 0,
        }));
        S.entries.push({ k: "row", chip, core, row, parts,
                         n: parts.reduce((a, p) => a + p.n, 0) });
      }
    }
  }
  $("right-inner").style.height = (S.entries.length * ROW_H) + "px";
  $("side-inner").style.height = (S.entries.length * ROW_H) + "px";
}

function renderTree() {
  const top = Math.max(0, scroll.scrollTop - ROW_H);
  const h = scroll.clientHeight;
  const a = Math.max(0, Math.floor(top / ROW_H) - 2);
  const b = Math.min(S.entries.length, Math.ceil((top + h) / ROW_H) + 2);
  const box = $("side-inner");
  box.textContent = "";
  for (let i = a; i < b; i++) {
    const e = S.entries[i];
    const d = document.createElement("div");
    d.className = "entry " + e.k + (!e.role && e.k === "core" ? " ro" : "");
    d.style.top = (i * ROW_H) + "px";
    if (e.k === "chip") {
      const open = !S.collapsedChips.has(e.chip);
      d.innerHTML = `<span class="caret">${open ? "▾" : "▸"}</span>chip${e.chip}`
        + ` <span class="n">${e.cores.filter((c) => c[1]).length} 个 core</span>`;
      d.onclick = () => {
        if (open) S.collapsedChips.add(e.chip); else S.collapsedChips.delete(e.chip);
        buildEntries(); renderTree(); scheduleDraw();
      };
    } else if (e.k === "core") {
      d.innerHTML = `<span class="caret">${e.role ? (S.expanded.has(e.lane_base) ? "▾" : "▸") : " "}</span>`
        + `core${e.core}` + (e.role ? "" : " <span class=\"n\">只转发</span>");
      if (e.role) d.onclick = () => {
        if (S.expanded.has(e.lane_base)) S.expanded.delete(e.lane_base);
        else S.expanded.add(e.lane_base);
        buildEntries(); renderTree(); scheduleDraw();
      };
    } else {
      d.innerHTML = `${S.rows[e.row]} <span class="n">${e.n}</span>`;
      if (S.sel && e.parts.some((p) => p.lane === S.sel.lane)) d.classList.add("on");
      d.onclick = () => { S.sel = null; draw(); };
    }
    box.appendChild(d);
  }
}

// ── 视窗 ─────────────────────────────────────────────────────────────
function dpr() { return window.devicePixelRatio || 1; }
function plotW() { return Math.max(80, cv.width / dpr() - GUTTER); }
function t2x(t) { return GUTTER + (t - S.view.t0) / (S.view.t1 - S.view.t0) * plotW(); }
function x2t(x) { return S.view.t0 + (x - GUTTER) / plotW() * (S.view.t1 - S.view.t0); }
function setView(t0, t1) {
  let span = Math.max(MIN_SPAN, t1 - t0);
  span = Math.min(span, S.t_end * 1.5);
  const c = (t0 + t1) / 2;
  t0 = c - span / 2; t1 = c + span / 2;
  if (t0 < -span * 0.02) { t1 += -span * 0.02 - t0; t0 = -span * 0.02; }
  S.view = { t0, t1 };
}
function fitAll() { setView(0, S.t_end); scheduleDraw(); }
function zoomAt(cx, factor) {
  const t = x2t(cx);
  setView(t - (t - S.view.t0) * factor, t + (S.view.t1 - t) * factor);
  scheduleDraw();
}
function panBy(dx) {
  const dt = -dx / plotW() * (S.view.t1 - S.view.t0);
  setView(S.view.t0 + dt, S.view.t1 + dt);
  scheduleDraw();
}

// ── 取窗口 ───────────────────────────────────────────────────────────
function fetchWin() {
  const span = S.view.t1 - S.view.t0;
  let ft0 = S.view.t0 - span * PAD, ft1 = S.view.t1 + span * PAD;
  const q = Math.max(MIN_SPAN, span / 8);                 // 量化：1 px 的平移不换窗口
  ft0 = Math.floor(ft0 / q) * q; ft1 = Math.ceil(ft1 / q) * q;
  return { t0: Math.max(0, ft0), t1: ft1 };
}
function covered(lane, w) {
  const c = S.cache.get(lane);
  if (!c) return false;
  // 「这一窗我拿全了」看的是请求过的窗口，不是返回段的跨度 —— 否则行尾之后没有段，
  // 每次移动都会以为是空白重取一遍。
  if (!(c.win[0] <= w.t0 + 1e-9 && c.win[1] >= w.t1 - 1e-9)) return false;
  // 粗层是缩得很远时用的：一旦放大到两倍以内，就重新要一遍（那时服务端会给精确段）。
  const span = S.view.t1 - S.view.t0;
  if (c.kind === "coarse" && span * 2 < c.span) return false;
  return true;
}
function visibleLanes() {
  const top = Math.max(0, scroll.scrollTop - ROW_H);
  const a = Math.max(0, Math.floor(top / ROW_H) - 2);
  const b = Math.min(S.entries.length, Math.ceil((top + scroll.clientHeight) / ROW_H) + 2);
  const out = [];
  for (let i = a; i < b && out.length < MAX_LANES; i++) {
    if (S.entries[i].k !== "row") continue;
    for (const p of S.entries[i].parts) {
      if (out.length >= MAX_LANES) break;
      out.push(p.lane);
    }
  }
  return out;
}
function squash(ids) {
  const s = ids.slice().sort((a, b) => a - b), out = [];
  for (let i = 0; i < s.length; i++) {
    let j = i;
    while (j + 1 < s.length && s[j + 1] === s[j] + 1) j++;
    out.push(j > i ? `${s[i]}-${s[j]}` : `${s[i]}`);
    i = j;
  }
  return out.join(",");
}
function vint(u8, off) {
  let v = 0, shift = 0, b;
  do { b = u8[off++]; v |= (b & 0x7F) << shift; shift += 7; } while (b & 0x80);
  return [v, off];
}
function decodeSegs(u8, tBase, n) {
  const out = [];
  let off = 0, prev = tBase;
  for (let i = 0; i < n && off + 5 <= u8.length; i++) {
    let gap, dur;
    [gap, off] = vint(u8, off);
    [dur, off] = vint(u8, off);
    const u = u8[off] | (u8[off + 1] << 8), t = u8[off + 2];
    off += 3;
    const t0 = prev + gap, t1 = t0 + dur;
    const none = (u === 0xFFFF && t === 0xFF);
    out.push({ t0, t1, user: none ? -1 : u, task: none ? -1 : t });
    prev = t1;
  }
  return out;
}
function parseFrame(buf) {
  const dv = new DataView(buf);
  const magic = String.fromCharCode(dv.getUint8(0), dv.getUint8(1), dv.getUint8(2), dv.getUint8(3));
  if (magic !== "TCW1") throw new Error("帧头不对：" + magic);
  const hlen = dv.getUint32(4, true);
  const head = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, 8, hlen)));
  const base = 8 + hlen;
  if (head.build !== S.build) { loadInit().then(() => { scheduleDraw(); }); return; }
  const win = [head.t0 || 0, head.t1 || 0], span = (head.t1 || 0) - (head.t0 || 0);
  for (const e of head.lanes) {
    const u8 = new Uint8Array(buf, base + e.off, e.len);
    if (e.mode === "exact") {
      const segs = decodeSegs(u8, e.t_base || 0, e.n);
      S.cache.set(e.lane, { kind: "exact", segs, win, span });
    } else {
      const cells = [];
      for (let i = 0; i + 4 <= u8.length; i += e.stride) {
        const c = e.c0 + i / e.stride;
        const d = u8[i], u = u8[i + 1] | (u8[i + 2] << 8), t = u8[i + 3];
        cells.push({ t0: c * e.cell, t1: (c + 1) * e.cell, d,
                     user: (u === 0xFFFF && t === 0xFF) ? -1 : u,
                     task: (t === 0xFF && u === 0xFFFF) ? -1 : t });
      }
      S.cache.set(e.lane, { kind: "coarse", cells, win, span, cell: e.cell });
    }
  }
}
async function ensureData() {
  const lanes = visibleLanes();
  const have = new Set();
  for (const id of lanes) if (covered(id, fetchWin())) have.add(id);
  const need = lanes.filter((id) => !have.has(id));
  if (!need.length) return;
  if (S.inflight) S.inflight.abort();
  const w = fetchWin();
  const ac = new AbortController();
  S.inflight = ac;
  S.loading = true;
  try {
    const r = await fetch(`/api/window?lanes=${squash(need)}&t0=${Math.floor(w.t0)}`
      + `&t1=${Math.ceil(w.t1)}&px=${Math.round(plotW())}&build=${encodeURIComponent(S.build)}`,
      { signal: ac.signal, cache: "no-store" });
    if (r.status === 409 || r.status === 503) {           // 索引换代 / 文件没了
      await loadInit(); scheduleDraw(); return;
    }
    if (!r.ok) throw new Error("/api/window " + r.status);
    parseFrame(await r.arrayBuffer());
    // 这条窗口的空白行也要记下来，不然每次移动都会重新请求它们
    for (const id of need) if (!S.cache.has(id)) S.cache.set(id, { kind: "exact", segs: [], covers: [w.t0, w.t1] });
  } catch (e) {
    if (e.name !== "AbortError") { $("sub").textContent = "取数失败：" + e.message; }
  } finally {
    if (S.inflight === ac) { S.inflight = null; S.loading = false; }
    scheduleDraw();
  }
}
function scheduleFetch() {
  if (S.timer) clearTimeout(S.timer);
  S.timer = setTimeout(() => { S.timer = null; ensureData(); }, 120);
}

// ── 画 ───────────────────────────────────────────────────────────────
let raf = 0;
function scheduleDraw() {
  if (raf) return;
  raf = requestAnimationFrame(() => { raf = 0; draw(); });
}
const _tw = new Map();
function textW(s) {
  let w = _tw.get(s);
  if (w === undefined) {
    let n = 0;
    for (const ch of s) n += ch.charCodeAt(0) > 255 ? 8.5 : 6.4;
    w = n + 8; _tw.set(s, w);
  }
  return w;
}
function resize() {
  const w = $("right").clientWidth, h = scroll.clientHeight;
  const d = dpr();
  cv.width = Math.max(120, Math.round(w * d));
  cv.height = Math.max(40, Math.round(h * d));
  cv.style.width = w + "px";
  cv.style.height = h + "px";
  ctx.setTransform(d, 0, 0, d, 0, 0);
  $("right-inner").style.width = w + "px";
}
function draw() {
  const d = dpr();
  const W = cv.width / d, H = cv.height / d;
  ctx.setTransform(d, 0, 0, d, 0, 0);
  ctx.clearRect(0, 0, W, H);
  const top = scroll.scrollTop;
  cv.style.top = top + "px";           // 画布跟着滚动贴在视口顶上
  const a = Math.max(0, Math.floor((top - ROW_H) / ROW_H));
  const b = Math.min(S.entries.length, Math.ceil((top + H) / ROW_H));
  let drew = 0;
  for (let i = a; i < b; i++) {
    const e = S.entries[i];
    const y = i * ROW_H - top;
    if (e.k !== "row") continue;
    drew++;
    drawLane(y, e);
  }
  drawRuler(W);
  if (!drew) {
    ctx.fillStyle = "#8b95a5";
    ctx.font = "13px system-ui, sans-serif";
    ctx.fillText("左边点开一颗 chip、再点一个 core，这里就出它那七行", 12, 34);
  }
  $("legend").textContent = `${Math.round(S.view.t0)} ～ ${Math.round(S.view.t1)} 拍`
    + `　滚轮缩放 · 拖拽平移 · F 看全`;
}
function drawRuler(W) {
  ctx.fillStyle = "#f7f8fa";
  ctx.fillRect(0, 0, W, RULER_H);
  ctx.strokeStyle = "#d8dce3";
  ctx.beginPath(); ctx.moveTo(GUTTER, RULER_H - 0.5); ctx.lineTo(W, RULER_H - 0.5); ctx.stroke();
  const span = S.view.t1 - S.view.t0;
  const raw = span / 7, mag = Math.pow(10, Math.floor(Math.log10(raw || 1)));
  let step = 10 * mag;
  for (const m of [1, 2, 5, 10]) if (m * mag >= raw) { step = m * mag; break; }
  ctx.font = "10px ui-monospace, monospace";
  ctx.fillStyle = "#5c6370";
  ctx.textAlign = "center";
  for (let t = Math.ceil(S.view.t0 / step) * step; t <= S.view.t1; t += step) {
    const x = t2x(t);
    if (x < GUTTER - 1 || x > W) continue;
    ctx.strokeStyle = "#c9d1dc";
    ctx.beginPath(); ctx.moveTo(x, RULER_H - 5); ctx.lineTo(x, RULER_H); ctx.stroke();
    // 刻度照原位画，字往右让一点：GUTTER 归零之后，最左那个刻度的字会有一半
    // 落在画布外面。
    ctx.fillText(String(Math.round(t)), Math.max(14, x), 11);
  }
  ctx.textAlign = "start";
}
function drawLane(y, e) {
  const W = cv.width / dpr();
  ctx.strokeStyle = "#e6e9ef";
  ctx.beginPath(); ctx.moveTo(GUTTER, y + ROW_H - 0.5); ctx.lineTo(W, y + ROW_H - 0.5); ctx.stroke();
  ctx.font = "10.5px ui-monospace, monospace";   // 段上那行字用
  if (!e.n) return;
  ctx.save();
  ctx.beginPath(); ctx.rect(GUTTER, y, W - GUTTER, ROW_H); ctx.clip();
  for (const part of e.parts) {
    const ent = S.cache.get(part.lane);
    if (!ent) continue;
    const col = colorOf(part);
    if (ent.kind === "exact") {
      for (const s of ent.segs) {
        if (s.t1 <= S.view.t0 || s.t0 >= S.view.t1) continue;
        const xa = t2x(s.t0), xb = t2x(s.t1);
        const w = Math.max(1, xb - xa);
        ctx.fillStyle = col;
        ctx.globalAlpha = s.user < 0 ? 0.45 : 1;
        ctx.fillRect(xa, y + 3, w, ROW_H - 6);
        ctx.globalAlpha = 1;
        if (S.sel && S.sel.lane === part.lane && S.sel.seg === s) {
          ctx.strokeStyle = "#2563eb"; ctx.lineWidth = 2;
          ctx.strokeRect(xa - 1, y + 2, w + 2, ROW_H - 4);
        }
        const full = labelOf(part.unit, s);
        if (w >= textW(full)) {
          ctx.fillStyle = "rgba(255,255,255,.92)";
          ctx.fillText(full, xa + 4, y + ROW_H / 2 + 3.5);
        }
      }
    } else if (ent.kind === "coarse") {
      for (const c of ent.cells) {
        if (c.t1 <= S.view.t0 || c.t0 >= S.view.t1 || !c.d) continue;
        const xa = t2x(c.t0), xb = t2x(c.t1);
        ctx.fillStyle = col;
        ctx.globalAlpha = Math.max(0.15, c.d / 255);
        ctx.fillRect(xa, y + 3, Math.max(1, xb - xa), ROW_H - 6);
      }
      ctx.globalAlpha = 1;
    }
  }
  ctx.restore();
}

// ── 交互 ─────────────────────────────────────────────────────────────
function pick(px, py) {
  const top = scroll.scrollTop;
  const i = Math.floor((py + top) / ROW_H);
  const e = S.entries[i];
  if (!e || e.k !== "row") return null;
  const t = x2t(px);
  for (const part of e.parts) {
    const ent = S.cache.get(part.lane);
    if (!ent || ent.kind !== "exact") continue;
    const segs = ent.segs;
    let lo = 0, hi = segs.length - 1, hit = null;
    while (lo <= hi) {
      const m = (lo + hi) >> 1;
      if (segs[m].t1 <= t) lo = m + 1;
      else if (segs[m].t0 > t) hi = m - 1;
      else { hit = segs[m]; break; }
    }
    if (hit) return { lane: part.lane, row: e.row, chip: e.chip, core: e.core,
                      unit: part.unit, seg: hit };
  }
  return null;
}
function showProps(p) {
  if (!p) { $("props").textContent = "点一个段看它的来龙去脉"; return; }
  const s = p.seg;
  const what = s.user < 0 ? "<b>?</b>（没配对上下发）"
    : `User_id <b>${s.user}</b> · ${p.unit} · task <b>${s.task}</b>`;
  $("props").innerHTML = `chip${p.chip} · core${p.core} · ${S.rows[p.row]}　${what}　`
    + `起 ${s.t0} 止 ${s.t1}　共 <b>${s.t1 - s.t0}</b> 拍`;
}
cv.addEventListener("pointerdown", (ev) => {
  const r = cv.getBoundingClientRect();
  let last = ev.clientX, moved = 0;
  try { cv.setPointerCapture(ev.pointerId); } catch (e) { /* 合成的指针事件没有 id */ }
  const move = (e) => { moved += Math.abs(e.clientX - last); panBy(e.clientX - last); last = e.clientX; };
  const up = (e) => {
    cv.removeEventListener("pointermove", move);
    cv.removeEventListener("pointerup", up);
    if (moved < 3) {
      const p = pick(ev.clientX - r.left, ev.clientY - r.top);
      S.sel = p ? { lane: p.lane, row: p.row, seg: p.seg } : null;
      showProps(p); renderTree(); scheduleDraw();
    } else { scheduleFetch(); }
  };
  cv.addEventListener("pointermove", move);
  cv.addEventListener("pointerup", up);
});
cv.addEventListener("mousemove", (ev) => {
  const r = cv.getBoundingClientRect();
  const p = pick(ev.clientX - r.left, ev.clientY - r.top);
  const tip = $("tip");
  if (!p) { if (!tip.hidden) tip.hidden = true; return; }
  const s = p.seg;
  tip.hidden = false;
  tip.textContent = `${labelOf(p.unit, s)}　第 ${s.t0} ～ ${s.t1} 拍`;
  tip.style.left = Math.min(window.innerWidth - 240, ev.clientX + 14) + "px";
  tip.style.top = (ev.clientY + 16) + "px";
});
cv.addEventListener("mouseleave", () => { $("tip").hidden = true; });
cv.addEventListener("wheel", (ev) => {
  ev.preventDefault();
  const r = cv.getBoundingClientRect();
  zoomAt(ev.clientX - r.left, ev.deltaY > 0 ? 1.18 : 1 / 1.18);
  scheduleFetch();
}, { passive: false });
scroll.addEventListener("scroll", () => { scheduleDraw(); scheduleFetch(); });
window.addEventListener("resize", () => { resize(); scheduleDraw(); });
window.addEventListener("keydown", (ev) => {
  const k = ev.key.toLowerCase();
  if (k === "f") fitAll();
  else if (k === "arrowleft") { panBy(plotW() * 0.15); scheduleFetch(); }
  else if (k === "arrowright") { panBy(-plotW() * 0.15); scheduleFetch(); }
  else if (k === "+" || k === "=") { zoomAt(plotW() / 2 + GUTTER, 1 / 1.3); scheduleFetch(); }
  else if (k === "-") { zoomAt(plotW() / 2 + GUTTER, 1.3); scheduleFetch(); }
});
$("fit").onclick = fitAll;
$("reload").onclick = async () => {
  $("reload").disabled = true;
  try {
    const r = await fetch("/api/reload", { method: "POST" });
    const j = await r.json();
    await loadInit(); S.cache.clear(); scheduleDraw(); scheduleFetch();
  } finally { $("reload").disabled = false; }
};

// ── 起飞 ─────────────────────────────────────────────────────────────
(async function main() {
  resize();
  await loadInit();
  renderTree();
  scheduleDraw();
  scheduleFetch();
})();
