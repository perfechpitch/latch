// insight frontend — layered canvas (bg + overlay), rAF-coalesced draws,
// batched /api/samples_multi fetch. Optimized for low-latency hover/pan.
//
// Layout: sidebar (tree of module + signal rows) · stage (two stacked
// canvases) · bottom (event-detail panel).
//
// Why two canvases:
//   - bg holds lanes, grid, ruler, selected-row tint. Heavy to draw but
//     only changes when view/data/selection/scroll changes.
//   - ov holds the crosshair only — cheap, gets cleared+redrawn on every
//     mousemove without re-painting any data.
//
// Why batch fetch:
//   - 25 lanes × per-lane fetch = 25 HTTP RTTs each pan/zoom. The
//     browser caps concurrent connections, so half of those queue. One
//     /api/samples_multi call cuts it to a single round trip.
//
// Why rAF coalescing:
//   - mousemove and rapid pan events fire faster than 60 Hz. Without
//     coalescing each one triggers a full repaint; coalescing collapses
//     them to one repaint per frame.

// Wire format version this frontend parses. Must equal the server's
// FORMAT_VERSION (insight/indexer.py); /api/init echoes it and main()
// refuses to run on mismatch — silently parsing buckets with the wrong
// layout renders garbage that looks like "random colors at every zoom".
const WIRE_FORMAT = 7;
// Frontend build number, stamped in the timeline's top-right corner so
// "which code is this page actually running" is answerable at a glance.
// Bump together with the ?v= suffixes in index.html.
const APP_BUILD = 56;
const DOM_K = 8;           // (value, share) pairs per bucket on the wire
const BUCKET_SIZE = 56 + DOM_K * 10;   // 7×u64 + DOM_K×u64 + DOM_K×u16
const DOM_W_ONE = 65535;   // u16 fixed-point share of 1.0
const HDR_PREFIX_SINGLE = 8;   // single-sig response prefix (u32 level + u32 count)

const TIMELINE_H = 28;
const LANE_H = 26;
const LANE_GAP = 1;
const ROW_H = LANE_H + LANE_GAP;
// No-signal grouping rows are shorter than signal lanes. Must match the
// `.row.module` height in style.css (border-box, border-bottom included)
// or the sidebar and the canvas lanes drift out of alignment.
const MODULE_H = 18;
const INDENT_PX = 10;
const CYCLE_GAP = 2;
const CYCLE_MIN_PX = 3;
// ×1 time-unit gridlines appear once a single unit is at least this
// many CSS px wide (deep zoom only — any denser and the ruling reads
// as texture, not as a grid).
const UNIT_GRID_MIN_PX = 5;
const MIN_VIEW_CYCLES = 5;     // never zoom in past this many cycles visible
// Bar lanes always render a per-pixel folded underlay (stable at any
// zoom — sub-pixel buckets can never overdraw-flicker); buckets at
// least this many CSS px wide additionally get crisp cell decorations
// (trailing gap + value label) painted on top. Wave lanes still use it
// as their cell↔fold switch threshold.
const FOLD_MAX_BUCKET_PX = 6;

const CROSSHAIR = "#cf222e";
// Time anchors: user-dropped vertical markers (Shift + left click drops one
// at the clicked time). A violet line hangs from a round head down the whole
// chart; the head sits in the ruler's lower band, below the tick numbers, so
// it never covers them. The head is the only clickable part — click it to
// select (white centre pip + property-box readout), then Delete/Backspace
// removes it. Drawn on the overlay layer so they track pan/zoom for free.
const ANCHOR_COLOR = "#8250df";
const ANCHOR_DOT_Y = 22;     // head centre y: ruler's lower band, under the tick numbers
const ANCHOR_R = 4;          // head radius
const ANCHOR_HIT_R = 8;      // pointer hit radius for selecting the head
const STATE_COLORS = [
  "#a8b1bd", "#1a7f37", "#bf8700", "#cf222e",
  "#8250df", "#0a66c2", "#d4a72c", "#df3079",
  "#6f42c1", "#3192aa", "#9b8255", "#54aeff",
  "#dd7815", "#0d8e6b", "#a04100", "#656d76",
];

// A hidden lane isn't drawn cell-by-cell at all — it collapses to a single
// flat grey bar across its data extent (see drawHiddenLane). The eye toggle
// in the sidebar mutes a signal this way without removing it from the list.
const HIDDEN_LANE_FILL = "#c2c8d0";

// ── state ─────────────────────────────────────────────────────────────

const S = {
  tree: [],
  treeById: new Map(),
  childrenOf: new Map(),
  hasSignal: new Map(),
  collapsed: new Set(),
  laneById: new Map(),
  laneData: new Map(),
  // Per-signal fetched-range cache. Records [t0, t1] of the *fetched*
  // window (which is wider than the current view — we prefetch ±0.5×
  // span as headroom). When the view stays inside this range, refetch
  // is skipped entirely, so small zooms / pans don't trigger a network
  // round-trip and the previously-fetched buckets keep displaying.
  laneFetched: new Map(),
  selectedLanes: new Set(),
  // Lanes the user toggled "hidden" via the per-row eye button. A hidden
  // lane stays in the list and keeps its place — only its waveform is
  // painted flat grey instead of in colour, so signals you don't care
  // about fade back while the ones you do keep standing out. Default:
  // empty, i.e. everything shown in colour.
  hiddenLanes: new Set(),
  // The "armed" module row: a single click on a module row sets this
  // (just highlights it); only a second click on the same already-armed
  // row toggles its collapsed state. Prevents single accidental clicks
  // from collapsing a busy hierarchy.
  selectedModuleId: null,
  entries: [],
  allSignals: [],
  view: { t0: 0, t1: 1 },
  bounds: { t0: 0, t1: 1 },
  cursorX: -1,
  cursorY: -1,
  hex: false,
  keyDown: new Set(),
  cyclePeriod: 0,
  // One cycle = this many raw trace-time units (the clock period). Comes
  // from the server (`serve --cycle-time`, default 1). Internal coords
  // stay in raw time; only on-screen time readouts are shown in cycles.
  cycleTime: 1,
  // Blocks the user clicked on the waveform. A plain click selects one
  // (replacing the set); cmd / ctrl-click toggles individual blocks in/out
  // so several stay lit at once. Each entry is { lane, t, v, dur }, keyed by
  // its (lane id, t) pair — see findEventIdx.
  events: [],
  // Finalized time-range bands. A plain drag replaces the set with one band;
  // cmd / ctrl-drag pushes another so several segments can be lit at once.
  // Each entry is { t0, t1 }.
  selections: [],
  selPending: null,       // {t0, t1} during the drag (single, in-flight)
  // User-dropped time markers (Shift + click). Each is { t } in raw time;
  // selectedAnchor holds the object reference that is currently picked (so
  // Delete can remove it) — null when nothing is selected.
  anchors: [],
  selectedAnchor: null,
  // Per-signal per-cycle aggregate over the highlighted region (dragged
  // band(s), else each picked signal's whole extent), served by
  // /api/select_stats. Shape { ranges, signals:[...], bySig:Map }. null when
  // no signal is selected / the first fetch hasn't returned. See
  // refreshSelectStats.
  selStats: null,
};

// ── DOM ───────────────────────────────────────────────────────────────

const $ = (s) => document.querySelector(s);
const sidebarEl = $("#sidebar");
const treeEl    = $("#tree");
const stageEl   = $("#stage");
const bgCv      = $("#cv-bg");
const selCv     = $("#cv-sel");
const ovCv      = $("#cv-ov");
const bgCtx     = bgCv.getContext("2d");
const selCtx    = selCv.getContext("2d");
const ovCtx     = ovCv.getContext("2d");
const propboxEl = $("#propbox");
const escapeHtml = (s) => String(s).replace(/[&<>"]/g, (c) =>
  ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

// Index of the clicked-block entry for (lane, start-time) in S.events, or -1.
// A block is identified by that pair; the canvas click logic uses this to
// toggle a block in/out of the lit set on a cmd / ctrl-click.
const findEventIdx = (laneId, t) =>
  S.events.findIndex((ev) => ev.lane.id === laneId && ev.t === t);

// A clicked bucket as an S.events entry. A solid bucket pins its one
// (t, v); a mixed aggregate has no single v, so it pins the bucket's
// dom composition instead (mix + raw event count) — the panel lists
// the values with their time shares, which IS the block's content.
function eventFromBucket(lane, b) {
  const ev = { lane, t: b.t_start, v: b.v_last, dur: b.t_end - b.t_start };
  if (b.v_min !== b.v_max) { ev.mix = b.dom || []; ev.count = b.count; }
  return ev;
}

// ── fetch ─────────────────────────────────────────────────────────────

async function jget(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url} -> ${r.status}`);
  return await r.json();
}

// Post-fetch preprocessing: merge consecutive buckets that carry one
// identical value into a single block. Holes between them are value-
// holds of that same value, so they're swallowed too. The server
// already RLEs raw events into runs at the source (and builds the
// pyramid over runs), so this is a safety net for any residual
// adjacent constant buckets — plus the one place every incoming value
// registers with the global color table, in data order.
function mergeConstantRuns(buckets, registerColors) {
  const out = [];
  for (const b of buckets) {
    const p = out[out.length - 1];
    if (p && p.v_min === p.v_max && b.v_min === b.v_max && p.v_min === b.v_min) {
      p.t_end = b.t_end;
      p.v_last = b.v_last;
      p.count += b.count;
    } else {
      out.push(b);
    }
    // Register values with the global color table in data order, so
    // color assignment doesn't depend on which lane happens to draw
    // first. Only block-style lanes register: a waveform lane (counter,
    // PC) would flood the 16-slot table with thousands of one-off
    // values and randomize the state lanes' colors via collisions.
    if (registerColors) {
      colorIdxOf(b.v_min);
      if (b.v_max !== b.v_min) colorIdxOf(b.v_max);
      if (b.v_dom !== b.v_min && b.v_dom !== b.v_max) colorIdxOf(b.v_dom);
    }
  }
  return out;
}

// Full-screen red banner for a frontend/server version mismatch. Shown
// at load (wire_format check in main) and at runtime when a data fetch
// returns 426 — the latter catches the "old tab kept open across a
// server upgrade" case, which otherwise renders with stale logic and
// looks exactly like the upgrade never happened.
let versionBannerShown = false;
function showVersionBanner(serverVer) {
  if (versionBannerShown) return;
  versionBannerShown = true;
  const div = document.createElement("div");
  div.style.cssText =
    "position:fixed;inset:0;z-index:99;display:flex;align-items:center;" +
    "justify-content:center;background:rgba(255,255,255,0.96);" +
    "font:14px/1.8 sans-serif;color:#cf222e;text-align:center;";
  div.innerHTML =
    "前端与服务端版本不一致" +
    `（页面 v${WIRE_FORMAT} / 服务 v${serverVer ?? "未知"}）。<br>` +
    "请强制刷新本页（Ctrl+Shift+R）；若仍出现，重启 insight 服务。";
  document.body.appendChild(div);
}

// Batch fetch: one HTTP request returns buckets for all listed signals.
async function fetchSamplesMulti(sigIds, t0, t1, width) {
  if (sigIds.length === 0) return new Map();
  const r = await fetch(
    `/api/samples_multi?sigs=${sigIds.join(",")}&t0=${t0}&t1=${t1}` +
    `&width=${width}&fmt=${WIRE_FORMAT}`);
  if (r.status === 426) { showVersionBanner(null); throw new Error("wire format mismatch"); }
  if (!r.ok) throw new Error("samples_multi fetch failed");
  const buf = await r.arrayBuffer();
  const dv = new DataView(buf);
  const out = new Map();
  let o = 0;
  const sigCount = dv.getUint32(o, true); o += 4;
  for (let s = 0; s < sigCount; ++s) {
    const sigId = Number(dv.getBigUint64(o, true)); o += 8;
    const level = dv.getUint32(o, true);             o += 4;
    const count = dv.getUint32(o, true);             o += 4;
    const buckets = new Array(count);
    for (let i = 0; i < count; ++i) {
      // dom: the wire's top-8 (value, time-share) pairs (offsets 56..136),
      // weight-ranked, zero-weight tail dropped. The sampling renderer
      // doesn't use it; the event panel lists it when a clicked block is
      // a mixed aggregate (no single value to show, but the composition
      // is known).
      const dom = [];
      for (let k = 0; k < DOM_K; ++k) {
        const wgt = dv.getUint16(o + 56 + DOM_K * 8 + k * 2, true);
        if (wgt === 0) break;
        dom.push({ v: Number(dv.getBigUint64(o + 56 + k * 8, true)), w: wgt });
      }
      buckets[i] = {
        t_start: Number(dv.getBigUint64(o + 0, true)),
        t_end:   Number(dv.getBigUint64(o + 8, true)),
        v_min:   Number(dv.getBigUint64(o + 16, true)),
        v_max:   Number(dv.getBigUint64(o + 24, true)),
        v_last:  Number(dv.getBigUint64(o + 32, true)),
        v_dom:   Number(dv.getBigUint64(o + 40, true)),
        count:   Number(dv.getBigUint64(o + 48, true)),
        dom,
      };
      o += BUCKET_SIZE;
    }
    const lane = S.laneById.get(sigId);
    out.set(sigId, {
      level,
      buckets: mergeConstantRuns(buckets, !lane || lane.kind === "bar"),
    });
  }
  return out;
}

// ── formatting ────────────────────────────────────────────────────────

// Pick the largest SI prefix where step/unit ≥ 0.5 — so zooming in
// naturally swaps "1.50m" for "1500k" once the resolution makes the
// big-unit label start losing precision.
function chooseUnit(step) {
  if (step >= 5e8) return { div: 1e9, suffix: "g" };
  if (step >= 5e5) return { div: 1e6, suffix: "m" };
  if (step >= 5e2) return { div: 1e3, suffix: "k" };
  return { div: 1, suffix: "" };
}
function fmtT(t, unit) {
  const r = Math.round(t);
  const sign = r < 0 ? "-" : "";
  const a = Math.abs(r);
  if (!unit) unit = chooseUnit(a || 1);
  if (!unit.suffix) return `${sign}${a}`;
  let s = (a / unit.div).toFixed(2).replace(/\.?0+$/, "");
  if (s === "" || s === "-") s = "0";
  return `${sign}${s}${unit.suffix}`;
}
// Raw trace-time → cycle count (one cycle = S.cycleTime ticks). All
// time shown to the user — ruler, cursor, selection — goes through this,
// so the whole UI reads in cycles while the data stays in raw time.
function tToCyc(t) { return t / S.cycleTime; }
// Cycle count as a label: a plain integer when it lands on a cycle edge
// (events do), else trimmed to 2 decimals (a cursor/selection mid-cycle).
function fmtCyc(t) {
  const c = t / S.cycleTime;
  if (Number.isInteger(c)) return `${c}`;
  return c.toFixed(2).replace(/\.?0+$/, "");
}
// String-signal label: a value-interned trace (e.g. the Comment print
// instruction) carries a value→name map; show the name instead of the
// integer id. Returns null when this lane/value has no label.
function laneLabelOf(lane, v) {
  return lane && lane.labels && lane.labels.has(v) ? lane.labels.get(v) : null;
}
function fmtV(v, lane) {
  const lbl = laneLabelOf(lane, v);
  if (lbl != null) return lbl;
  if (S.hex && lane && lane.kind === "wave") return "0x" + v.toString(16);
  if (Math.abs(v) >= 1e9) return v.toExponential(2);
  return `${v}`;
}
// A statistic readout (cycle counts, mean value): integers print plain, an
// integer-valued float collapses to that integer, otherwise trim to 3
// decimals; very large / tiny magnitudes fall back to exponential so the
// box width stays bounded.
function fmtStatNum(x) {
  if (!isFinite(x)) return "—";
  const a = Math.abs(x);
  if (a !== 0 && (a >= 1e7 || a < 1e-3)) return x.toExponential(2);
  if (Math.abs(x - Math.round(x)) < 1e-9) return String(Math.round(x));
  return x.toFixed(3).replace(/\.?0+$/, "");
}

// ── tree / entries ────────────────────────────────────────────────────

function buildChildrenIndex() {
  S.childrenOf.clear();
  for (const r of S.tree) {
    if (!S.childrenOf.has(r.parent_id)) S.childrenOf.set(r.parent_id, []);
    S.childrenOf.get(r.parent_id).push(r);
  }
  // Default order: signals within a level sort by name (natural compare,
  // so sig2 < sig10); module rows keep their trace-given positions —
  // group order often follows the pipeline, alphabetizing it would lose
  // that. Runs once at boot, so drag-reorder afterwards still wins.
  for (const kids of S.childrenOf.values()) {
    const sigs = kids.filter((r) => r.is_signal).sort(
      (a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }));
    let i = 0;
    for (let k = 0; k < kids.length; ++k) {
      if (kids[k].is_signal) kids[k] = sigs[i++];
    }
  }
  computeHasSignal();
}
// hasSignal[id] is true iff the subtree rooted at this node contains at
// least one is_signal leaf. Modules whose entire subtree is empty are
// suppressed from the sidebar — they'd otherwise show up as dead headers
// the user can never expand into a useful row.
function computeHasSignal() {
  S.hasSignal = new Map();
  const dfs = (id) => {
    if (S.hasSignal.has(id)) return S.hasSignal.get(id);
    let r = false;
    for (const c of (S.childrenOf.get(id) || [])) {
      if (c.is_signal) r = true;
      else if (dfs(c.id)) r = true;
    }
    S.hasSignal.set(id, r);
    return r;
  };
  for (const r of S.tree) dfs(r.id);
}
function depthOf(id) {
  let d = 0;
  let cur = S.treeById.get(id);
  while (cur && cur.parent_id !== 0 && S.treeById.has(cur.parent_id)) {
    d += 1;
    cur = S.treeById.get(cur.parent_id);
  }
  return d;
}
function splitPath(path) {
  const i = path.lastIndexOf(".");
  if (i < 0) return { prefix: "", leaf: path };
  return { prefix: path.slice(0, i + 1), leaf: path.slice(i + 1) };
}
function rebuildEntries() {
  S.entries = [];
  const walk = (parentId, hiddenUnder) => {
    const kids = S.childrenOf.get(parentId) || [];
    for (const k of kids) {
      // Skip modules whose subtree has no real signals — those are
      // empty containers from the meta block and would otherwise add
      // dead rows to the sidebar.
      if (!k.is_signal && !S.hasSignal.get(k.id)) continue;
      const hidden = hiddenUnder ||
                     (k.parent_id !== 0 && S.collapsed.has(k.parent_id));
      if (k.is_signal) {
        if (!hidden) {
          const lane = S.laneById.get(k.id);
          if (lane) S.entries.push({ type: "signal", lane, depth: depthOf(k.id) });
        }
      } else {
        if (!hidden) S.entries.push({ type: "module", row: k, depth: depthOf(k.id) });
        walk(k.id, hidden || S.collapsed.has(k.id));
      }
    }
  };
  walk(0, false);
  recomputeEntryTops();
}

// Pointer-based drag-reorder for any row — groups and signals alike.
//
// We deliberately do NOT use native HTML5 drag-and-drop here: it's flaky
// for in-app list reordering (small movements get reinterpreted as clicks
// or text-selection, drop acceptance depends on per-element preventDefault
// timing, and behaviour varies with drag direction). A plain mousedown →
// mousemove → mouseup state machine makes a drag fire reliably regardless
// of start position, direction, or distance beyond the threshold.
//
// rowDrag is the in-flight drag; suppressRowClick swallows the click that
// the browser fires after a same-row mouseup so a finished drag doesn't
// also toggle/select the row.
let rowDrag = null;          // { id, parent, startX, startY, active } | null
let suppressRowClick = false;
const ROW_DRAG_THRESHOLD = 5;

function clearDragVisuals() {
  treeEl.querySelectorAll(
    ".row.drag-source, .row.drag-over-before, .row.drag-over-after"
  ).forEach((el) => el.classList.remove(
      "drag-source", "drag-over-before", "drag-over-after"));
}

function parentIdOf(id) {
  const n = S.treeById.get(id);
  return n ? n.parent_id : 0;
}

// Given a pointer Y, find the sibling row to drop against. Insertion is
// decided purely by vertical position — horizontal position is irrelevant,
// which is what makes the drag direction-agnostic. Returns null if the
// dragged row has no visible siblings.
function rowDropTarget(clientY, drag) {
  const sibs = (S.childrenOf.get(drag.parent) || [])
    .map((r) => r.id)
    .filter((sid) => sid !== drag.id)
    .map((sid) => treeEl.querySelector(`.row[data-drag-id="${sid}"]`))
    .filter(Boolean)
    .map((el) => ({ el, id: Number(el.dataset.dragId), r: el.getBoundingClientRect() }))
    .sort((a, b) => a.r.top - b.r.top);
  if (!sibs.length) return null;
  for (const s of sibs) {
    if (clientY < s.r.top + s.r.height / 2) {
      return { id: s.id, before: true, el: s.el };
    }
  }
  const last = sibs[sibs.length - 1];
  return { id: last.id, before: false, el: last.el };
}

function paintDropIndicator(drag, clientY) {
  treeEl.querySelectorAll(".row.drag-over-before, .row.drag-over-after")
    .forEach((el) => el.classList.remove("drag-over-before", "drag-over-after"));
  const t = rowDropTarget(clientY, drag);
  drag.drop = t;
  if (t) t.el.classList.add(t.before ? "drag-over-before" : "drag-over-after");
}

// Block text selection for the whole press once a row is armed. This must
// fire on `selectstart` (not just user-select:none) because the browser
// begins the selection gesture in the pre-threshold pixels, and flipping
// user-select mid-gesture can't abort a selection already in progress.
// rowDrag is set on mousedown, so this catches it from the very first move.
document.addEventListener("selectstart", (ev) => {
  if (rowDrag) ev.preventDefault();
});

// One-time document listeners drive the in-flight drag. Per-row mousedown
// (wired in wireDragReorder) only arms a candidate; the drag promotes here
// once the pointer travels past the threshold.
document.addEventListener("mousemove", (ev) => {
  if (!rowDrag) return;
  if (!rowDrag.active) {
    if (Math.abs(ev.clientX - rowDrag.startX) < ROW_DRAG_THRESHOLD &&
        Math.abs(ev.clientY - rowDrag.startY) < ROW_DRAG_THRESHOLD) return;
    rowDrag.active = true;
    const src = treeEl.querySelector(`.row[data-drag-id="${rowDrag.id}"]`);
    if (src) src.classList.add("drag-source");
    // Kill any selection the browser started in the pre-threshold pixels,
    // then block further selection page-wide for the rest of the drag.
    window.getSelection().removeAllRanges();
    document.body.classList.add("row-dragging");
  }
  ev.preventDefault();        // suppress text selection during the drag
  paintDropIndicator(rowDrag, ev.clientY);
});

document.addEventListener("mouseup", () => {
  const drag = rowDrag;
  rowDrag = null;
  document.body.classList.remove("row-dragging");
  if (!drag || !drag.active) { clearDragVisuals(); return; }
  // A real drag happened — swallow the click the browser may fire next so
  // the row doesn't also collapse/select. Reset on the next tick in case
  // no click follows (drop landed on a different row than mousedown).
  suppressRowClick = true;
  setTimeout(() => { suppressRowClick = false; }, 0);
  const drop = drag.drop;
  clearDragVisuals();
  if (drop && drop.id !== drag.id) reorderSiblings(drag.id, drop.id, drop.before);
});

// Reorder `sourceId` relative to `targetId` within their shared parent's
// child list. Reordering is sibling-only — a drop onto a row in a different
// group is a no-op (we don't reparent across the hierarchy). Works for
// both group rows and signal rows since both live in S.childrenOf.
function reorderSiblings(sourceId, targetId, before) {
  const parent = parentIdOf(sourceId);
  if (parent !== parentIdOf(targetId)) { clearDragVisuals(); return; }
  const sibs = S.childrenOf.get(parent) || [];
  const sIdx = sibs.findIndex((r) => r.id === sourceId);
  if (sIdx < 0) return;
  const [moved] = sibs.splice(sIdx, 1);
  // findIndex re-runs after splice — the target may have shifted by one.
  let tIdx = sibs.findIndex((r) => r.id === targetId);
  if (tIdx < 0) { sibs.splice(sIdx, 0, moved); return; }
  sibs.splice(before ? tIdx : tIdx + 1, 0, moved);
  S.childrenOf.set(parent, sibs);
  // Reorder moves rows vertically too — refetch any lane it pushed into view.
  rebuildEntries(); renderTree(); scheduleDrawBg(); scheduleRefetch();
}

// Arm a row as a drag candidate on left-button mousedown. The actual drag
// promotion + drop is handled by the document-level listeners above. `id`
// is the tree-node id (module id or signal/lane id — both index
// S.treeById / S.childrenOf).
function wireDragReorder(row, id) {
  row.dataset.dragId = String(id);
  row.addEventListener("mousedown", (ev) => {
    if (ev.button !== 0) return;
    rowDrag = { id, parent: parentIdOf(id), startX: ev.clientX,
                startY: ev.clientY, active: false, drop: null };
  });
}

// Per-signal visibility toggle. "Hidden" never drops the row — it paints
// the lane grey (see drawBarLane / drawWaveLane) so an unwanted signal
// fades into the background while the ones you care about keep their
// colour. The icon is an open eye; when the signal is hidden a diagonal
// slash is shown across it (CSS, via the `.off` class).
const EYE_SVG =
  '<svg viewBox="0 0 16 16" width="13" height="13" fill="none" ' +
  'stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round">' +
  '<path class="eye-shape" d="M1 8s2.6-4.3 7-4.3S15 8 15 8s-2.6 4.3-7 4.3S1 8 1 8z"/>' +
  '<circle class="eye-pupil" cx="8" cy="8" r="1.7"/>' +
  '<line class="eye-slash" x1="2.5" y1="13.5" x2="13.5" y2="2.5"/>' +
  '</svg>';

function makeEyeButton(lane) {
  const off = S.hiddenLanes.has(lane.id);
  const eye = document.createElement("span");
  eye.className = "eye" + (off ? " off" : "");
  eye.title = off ? "signal hidden (greyed) — click to show"
                  : "click to hide (grey out) this signal";
  eye.innerHTML = EYE_SVG;
  // Swallow mousedown so the row's drag-reorder candidate never arms, and
  // click so toggling visibility doesn't also select the row.
  eye.addEventListener("mousedown", (ev) => ev.stopPropagation());
  eye.addEventListener("click", (ev) => {
    ev.stopPropagation();
    if (S.hiddenLanes.has(lane.id)) S.hiddenLanes.delete(lane.id);
    else S.hiddenLanes.add(lane.id);
    renderTree(); refreshToggleAll(); scheduleDrawBg();
  });
  return eye;
}

// ── global show/hide-all toggle (sidebar header) ────────────────────
// One button in the top-left header strip that hides or shows every
// signal at once. It mirrors the per-row eyes: the next click hides
// everything until all lanes are hidden, then flips to "show all".
const toggleAllEl = $("#toggle-all");

function allLanesHidden() {
  return S.allSignals.length > 0 && S.hiddenLanes.size >= S.allSignals.length;
}
// Reflect the all-hidden state on the header button: slashed eye + "Show
// all" once everything is hidden, open eye + "Hide all" otherwise (the
// mixed case counts as "not all hidden" — the next click hides the rest).
function refreshToggleAll() {
  if (!toggleAllEl) return;
  const off = allLanesHidden();
  toggleAllEl.classList.toggle("off", off);
  const label = toggleAllEl.querySelector(".toggle-all-label");
  if (label) label.textContent = off ? "Show all" : "Hide all";
  toggleAllEl.title = off ? "show all signals" : "hide all signals";
}
function setAllHidden(hide) {
  S.hiddenLanes.clear();
  if (hide) for (const lane of S.allSignals) S.hiddenLanes.add(lane.id);
  renderTree(); refreshToggleAll(); scheduleDrawBg();
}
if (toggleAllEl) {
  toggleAllEl.innerHTML = EYE_SVG + '<span class="toggle-all-label">Hide all</span>';
  toggleAllEl.addEventListener("click", () => setAllHidden(!allLanesHidden()));
}

// ── global collapse / expand-all toggle (sidebar header) ────────────
// One header button that folds every group down to its top-level headers,
// then flips to expand-all once everything is collapsed. Drives the same
// S.collapsed set the per-row group clicks use.
// Chevrons span y 3..13 (symmetric about the viewBox centre 8) so the glyph
// fills the button like the refresh icon beside it — a small glyph reads as
// off-centre even when its box is perfectly centred.
const COLLAPSE_SVG =   // chevrons pointing inward = collapse
  '<svg viewBox="0 0 16 16" width="13" height="13" fill="none" ' +
  'stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">' +
  '<path d="M3.5 3 L8 6 L12.5 3"/><path d="M3.5 13 L8 10 L12.5 13"/></svg>';
const EXPAND_SVG =     // chevrons pointing outward = expand
  '<svg viewBox="0 0 16 16" width="13" height="13" fill="none" ' +
  'stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">' +
  '<path d="M3.5 6 L8 3 L12.5 6"/><path d="M3.5 10 L8 13 L12.5 10"/></svg>';
const collapseAllEl = $("#collapse-all");

// Every module node that actually has signals under it — the only ones the
// sidebar draws as foldable rows.
function collapsibleModuleIds() {
  return S.tree.filter((r) => !r.is_signal && S.hasSignal.get(r.id)).map((r) => r.id);
}
function allCollapsed() {
  const ids = collapsibleModuleIds();
  return ids.length > 0 && ids.every((id) => S.collapsed.has(id));
}
// When everything is collapsed show the expand icon (+ "expand all"); else
// the collapse icon. Called on boot, reload, and after any fold change so
// the button always reflects the current state.
function refreshCollapseAll() {
  if (!collapseAllEl) return;
  const folded = allCollapsed();
  collapseAllEl.innerHTML = folded ? EXPAND_SVG : COLLAPSE_SVG;
  collapseAllEl.title = folded ? "expand all groups" : "collapse all groups";
}
function setAllCollapsed(collapse) {
  if (collapse) for (const id of collapsibleModuleIds()) S.collapsed.add(id);
  else S.collapsed.clear();
  // Like the per-row fold: revealed lanes need a fetch, not just a repaint.
  rebuildEntries(); renderTree(); refreshCollapseAll();
  scheduleDrawBg(); scheduleRefetch();
}
if (collapseAllEl) {
  refreshCollapseAll();
  collapseAllEl.addEventListener("click", () => setAllCollapsed(!allCollapsed()));
}

// ── reload-from-disk button (sidebar header) ────────────────────────
// Top-left refresh button. POST /api/reload makes the server re-read the
// .trace file (rebuilds the index when it changed), then we re-bootstrap
// the page from a fresh /api/init: new tree, new signal list, new buckets.
// Current zoom/pan and the collapsed / hidden / selected sets are kept
// (those live in S keyed by id and aren't rebuilt), so a re-run sim shows
// up in place without restarting the server or hard-refreshing the tab.
const REFRESH_SVG =
  '<svg viewBox="0 0 16 16" width="13" height="13" fill="none" ' +
  'stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round">' +
  '<path d="M13.65 6.4A5.5 5.5 0 1 0 14 8.5"/>' +
  '<path d="M14 2.5V6.4H10.1"/>' +
  '</svg>';
const refreshAllEl = $("#refresh-all");
let reloading = false;

async function reloadFromDisk() {
  if (reloading) return;
  reloading = true;
  if (refreshAllEl) refreshAllEl.classList.add("spin");
  const prevView = { ...S.view };  // keep the user's zoom/pan across reload
  try {
    await fetch("/api/reload", { method: "POST" });
    const init = await jget("/api/init");
    if (init.wire_format !== WIRE_FORMAT) {
      showVersionBanner(init.wire_format);
      return;
    }
    S.tree = init.tree;
    S.treeById = new Map(S.tree.map((r) => [r.id, r]));
    buildChildrenIndex();
    if (init.cycle_time > 0) S.cycleTime = init.cycle_time;
    S.cyclePeriod = S.cycleTime;
    // Drop cached waveforms so fresh buckets are pulled for the new data.
    S.laneData.clear();
    S.laneFetched.clear();
    await buildAllLanes(init.signals);  // overwrites S.bounds and S.view
    // Restore the prior view, clamped into the (possibly grown) bounds.
    const lo = Math.max(S.bounds.t0, Math.min(prevView.t0, S.bounds.t1 - 1));
    const hi = Math.min(S.bounds.t1, Math.max(prevView.t1, lo + 1));
    S.view = { t0: lo, t1: hi };
    rebuildEntries();
    renderTree();
    refreshToggleAll();
    refreshCollapseAll();
    await fetchAllVisible();
    renderEventPanel();
    drawBg(); drawSel(); drawOv();
  } catch (e) {
    console.error("reload failed", e);
  } finally {
    reloading = false;
    if (refreshAllEl) refreshAllEl.classList.remove("spin");
  }
}

if (refreshAllEl) {
  refreshAllEl.innerHTML = REFRESH_SVG;
  refreshAllEl.addEventListener("click", reloadFromDisk);
}

function renderTree() {
  treeEl.innerHTML = "";
  const frag = document.createDocumentFragment();
  for (const e of S.entries) {
    const row = document.createElement("div");
    if (e.type === "module") {
      row.className = "row module";
      const isCollapsed = S.collapsed.has(e.row.id);
      if (isCollapsed) row.classList.add("collapsed");
      row.title = e.row.path + (isCollapsed ? " (collapsed)" : "");
      const indent = document.createElement("span");
      indent.className = "indent"; indent.style.width = `${e.depth * INDENT_PX}px`;
      row.appendChild(indent);
      // Caret showing collapse state: ▸ collapsed, ▾ expanded.
      const caret = document.createElement("span");
      caret.className = "caret";
      caret.textContent = isCollapsed ? "▸" : "▾";
      row.appendChild(caret);
      const lf = document.createElement("span"); lf.className = "leaf";
      lf.textContent = e.row.name; row.appendChild(lf);
      row.addEventListener("click", () => {
        if (suppressRowClick) return;   // a drag just ended on this row
        // No-signal grouping rows collapse on a single click — they hold
        // no waveform, so there's nothing to accidentally lose by toggling.
        if (S.collapsed.has(e.row.id)) S.collapsed.delete(e.row.id);
        else S.collapsed.add(e.row.id);
        // Folding shifts every lane's Y, so lanes that were off-screen
        // (never fetched, or scrolled past) can move into the viewport.
        // scheduleDrawBg alone would paint them empty — scheduleRefetch
        // pulls the newly-visible lanes' buckets, same as a scroll does.
        rebuildEntries(); renderTree(); refreshCollapseAll();
        scheduleDrawBg(); scheduleRefetch();
      });

      // Groups are draggable to reorder among their siblings.
      wireDragReorder(row, e.row.id);
    } else {
      const lane = e.lane;
      row.className = "row signal";
      if (S.selectedLanes.has(lane.id)) row.classList.add("selected");
      row.title = lane.name;
      const indent = document.createElement("span");
      indent.className = "indent"; indent.style.width = `${e.depth * INDENT_PX}px`;
      row.appendChild(indent);
      row.appendChild(makeEyeButton(lane));
      const { prefix, leaf } = splitPath(lane.name);
      const pre = document.createElement("span"); pre.className = "prefix";
      pre.textContent = e.depth >= 1 ? "" : prefix;
      const lf  = document.createElement("span"); lf.className  = "leaf"; lf.textContent = leaf;
      row.appendChild(pre); row.appendChild(lf);
      row.addEventListener("click", (ev) => {
        if (suppressRowClick) return;   // a drag just ended on this row
        // Clicking a signal disarms any armed module so the highlight
        // doesn't linger across unrelated interactions.
        S.selectedModuleId = null;
        if (ev.shiftKey || ev.ctrlKey || ev.metaKey) {
          if (S.selectedLanes.has(lane.id)) S.selectedLanes.delete(lane.id);
          else S.selectedLanes.add(lane.id);
          // Additive pick (cmd / ctrl / shift): keep the blocks the user already
          // clicked on other lanes — they stay in the active selection.
        } else {
          if (S.selectedLanes.size === 1 && S.selectedLanes.has(lane.id)) {
            S.selectedLanes.clear();
          } else {
            S.selectedLanes.clear(); S.selectedLanes.add(lane.id);
          }
          // Plain pick replaces the selection, so it also drops clicked blocks
          // on other lanes — they no longer belong to the active selection.
          S.events = S.events.filter((evt) => evt.lane.id === lane.id);
        }
        renderTree(); renderEventPanel(); scheduleDrawSel();
        scheduleSelectStats();   // the selected-signal set changed
      });
      // Signals are draggable to reorder among their siblings.
      wireDragReorder(row, lane.id);
    }
    frag.appendChild(row);
  }
  treeEl.appendChild(frag);
}

// Deselect every lane picked in the left tree (and drop the derived
// per-cycle stats). Shared by the keyboard Escape path and the
// "click on empty / non-signal space" deselect paths. Idempotent: a no-op
// when nothing is picked, so callers can fire it unconditionally.
function clearLaneSelection() {
  if (S.selectedLanes.size === 0) return;
  S.selectedLanes.clear();
  S.selectedModuleId = null;
  renderTree();
  renderEventPanel();
  scheduleDrawSel();
  scheduleSelectStats();   // selection cleared → drop the stats section
}

// Click on empty sidebar space or a non-signal (module) row deselects the
// lane highlight — the standard "click outside the list to clear selection".
// Signal rows manage their own selection (toggle / shift-multiselect) so are
// skipped; the header buttons and an in-flight row drag are skipped too.
sidebarEl.addEventListener("click", (ev) => {
  if (suppressRowClick) return;                    // a drag just ended
  if (ev.shiftKey || ev.ctrlKey || ev.metaKey) return;  // additive: keep picks
  if (ev.target.closest(".row.signal")) return;    // handled by the row itself
  if (ev.target.closest("#sidebar-head")) return;  // header buttons
  clearLaneSelection();
});

// ── lanes ─────────────────────────────────────────────────────────────

// Bootstrap from /api/init — one request gives the module tree plus
// every signal's time range + unique-values count. Used to be
// 1 + 2*N requests (tree + range + stats per signal), which is the
// reason "first page open" felt slow at 30+ signals.
async function buildAllLanes(initSignals) {
  const ordered = [];
  const dfs = (id) => {
    for (const c of (S.childrenOf.get(id) || [])) {
      if (c.is_signal) ordered.push(c);
      dfs(c.id);
    }
  };
  dfs(0);
  const sigs = ordered.length ? ordered : S.tree.filter((r) => r.is_signal);

  S.laneById.clear(); S.allSignals = [];
  let t0 = Infinity, t1 = -Infinity;
  for (const sig of sigs) {
    const info = initSignals[String(sig.id)];
    if (!info) continue;
    // Every lane defaults to state-style blocks; waveform is per-lane
    // opt-in via the right-click override. (Many-valued lanes as blocks
    // do crowd the 16-color palette — accepted as the default trade.)
    const kind = "bar";
    // String signals ship a {value: name} table (Comment text etc.) —
    // keep it as a Number-keyed Map so the discrete lane / tooltip print
    // the name instead of the interned id.
    const labels = info.labels
      ? new Map(Object.entries(info.labels).map(([k, v]) => [Number(k), v]))
      : null;
    const lane = {
      id: sig.id, name: sig.path, shortName: sig.name,
      t0: info.t0, t1: info.t1, count: info.event_count,
      kind, unique: info.unique_values, labels,
    };
    S.laneById.set(sig.id, lane);
    S.allSignals.push(lane);
    // Register block-lane values with the color table NOW, in the
    // server's fixed (sorted) order — a value then keeps one color
    // across zoom paths, page reloads, and lanes. Lazy fetch-order
    // registration made the same value land on different palette slots
    // in different sessions.
    if (kind === "bar" && info.values) {
      for (const v of info.values) colorIdxOf(v);
    }
    // (Cycle grid + zoom floor key off S.cyclePeriod, which is set from
    // the server's cycle_time — the authoritative clock period — in
    // main()/reload, not auto-detected from event spacing.)
    if (info.t0 < t0) t0 = info.t0;
    if (info.t1 > t1) t1 = info.t1;
  }
  if (!S.allSignals.length) { t0 = 0; t1 = 1; }
  if (t1 <= t0) t1 = t0 + 1;
  S.bounds = { t0, t1 };
  S.view = { ...S.bounds };
}

// Prefetch factor: how much wider than the view we fetch on each call.
// 2× = pad both sides by 0.5× span. Small zooms/pans then stay inside
// the cached range, so re-fetch is skipped and the user sees data
// instantly instead of waiting for a network round-trip to fill the
// newly-revealed region. Higher would skip more refetches but bloats
// each request; lower would refetch more often.
const PREFETCH_FACTOR = 2;

async function fetchAllVisible() {
  if (S.allSignals.length === 0) return;
  const w = chartWidth();
  const viewT0 = Math.max(0, Math.floor(S.view.t0));
  const viewT1 = Math.max(viewT0 + 1, Math.ceil(S.view.t1));
  // Quantize the fetch window: span snapped to a power of two, start
  // aligned to a quarter-span grid. Every wheel notch inside one zoom
  // band then issues the *identical* request — same window, same
  // server-side level pick, same buckets — so the rendering cannot
  // flip styles while scrolling. The level only changes when the zoom
  // crosses a discrete band edge, once, instead of jittering around
  // the raw/aggregate threshold on every refetch.
  const viewSpanQ = viewT1 - viewT0;
  const fetchSpan = Math.pow(2,
    Math.ceil(Math.log2(Math.max(2, viewSpanQ * PREFETCH_FACTOR))));
  const grid = fetchSpan / 4;
  const center = (viewT0 + viewT1) / 2;
  const fetchT0 = Math.max(0, Math.floor((center - fetchSpan / 2) / grid) * grid);
  const fetchT1 = fetchT0 + fetchSpan;
  // Request ~2 buckets per screen pixel (the sampling renderer needs no
  // more — it's sub-pixel already). w*4 used to over-fetch 4 buckets/pixel,
  // which on a whole-view fetch forced the level picker one band finer
  // (L2≈19k buckets/signal instead of L3≈4k), bloating the first paint ~5×
  // across all signals for no visible gain.
  const fetchW = Math.min(8192, w * 2);

  // Only re-fetch signals whose cached range doesn't already cover the
  // current view. The cached data outside the view is the headroom that
  // makes the next small zoom/pan a no-op.
  //
  // Coverage alone is NOT enough: a cached fetch from another zoom
  // band holds buckets aggregated at another resolution. Reuse is
  // therefore allowed ONLY within the same band — the cached window's
  // span must equal the span this view would fetch right now. This
  // makes the on-screen representation a function of the view alone:
  // however the user arrived at a view (zooming in, zooming out,
  // jumping), the data backing it is identical, so the rendering is
  // identical. The earlier "within 4× is close enough" rule let two
  // paths to the same view hold windows from neighbouring bands, which
  // the server serves at different aggregation strides — same screen,
  // two (sub-pixel, but real) different pictures.
  // Virtualize: only fetch lanes within (or one screenful either side of) the
  // viewport. Off-screen lanes aren't drawn (drawBg clips by Y), so fetching +
  // parsing all of them just to show ~30 was the bulk of first paint on big
  // traces (parsing 50M-event signals' buckets dominates). The one-screen
  // look-ahead means a scroll reveals already-fetched lanes; scrolling further
  // triggers scheduleRefetch() to pull the next batch.
  const H = bgCv.clientHeight;
  const buf = H;
  const ids = [];
  for (let idx = 0; idx < S.entries.length; idx++) {
    const e = S.entries[idx];
    if (!e.lane) continue;
    const yTop = entryYTop(idx);
    if (yTop + LANE_H < TIMELINE_H - buf || yTop > H + buf) continue;
    const lane = e.lane;
    const f = S.laneFetched.get(lane.id);
    const covered = f && f.t0 <= viewT0 && f.t1 >= viewT1;
    const sameBand = f && f.span === fetchSpan;
    if (covered && sameBand) continue;
    ids.push(lane.id);
  }
  if (ids.length === 0) return;

  const result = await fetchSamplesMulti(ids, fetchT0, fetchT1, fetchW);
  for (const [id, data] of result) {
    S.laneData.set(id, data);
    S.laneFetched.set(id, { t0: fetchT0, t1: fetchT1, span: fetchT1 - fetchT0 });
  }
  detectCyclePeriod();
}
function detectCyclePeriod() {
  // No-op: the cycle period is the server's cycle_time (set in
  // main()/reload), not auto-detected from event spacing. Kept as a
  // hook in case a future trace carries no cycle_time and the spacing
  // heuristic is wanted as a fallback.
  if (S.cyclePeriod) return;
  let mind = Infinity;
  for (const lane of S.allSignals) {
    const d = S.laneData.get(lane.id);
    if (!d || d.level !== 0 || d.buckets.length < 2) continue;
    for (let i = 1; i < d.buckets.length; ++i) {
      const dt = d.buckets[i].t_start - d.buckets[i - 1].t_start;
      if (dt > 0 && dt < mind) mind = dt;
    }
  }
  if (mind !== Infinity) S.cyclePeriod = mind;
}

function chartWidth() { return Math.max(64, bgCv.clientWidth); }
function chartHeight() { return bgCv.clientHeight - TIMELINE_H; }
function tToX(t) { return ((t - S.view.t0) / (S.view.t1 - S.view.t0)) * chartWidth(); }
function xToT(x) { return S.view.t0 + (x / chartWidth()) * (S.view.t1 - S.view.t0); }

// Device-pixel colored extent of a time span [t_start, t_end), in CSS px —
// matching EXACTLY how drawBarLaneFolded rasterizes a block. The fold colors
// each device column whose left-edge instant lands in the span, i.e. columns
// ceil(f0) .. ceil(f1)-1 with f = (t - view.t0) * nCols/(view span). So the
// painted color occupies CSS [ceil(f0)/dpr, ceil(f1)/dpr]. Anything that must
// sit flush with the fill — the click-highlight outline (bucketRect) and the
// inter-cell gap — derives its x from THIS, not from the fractional tToX:
// tToX(t_end) = f1/dpr lands a fraction of a device pixel INSIDE the block
// (since ceil(f1) >= f1), which is what made the highlight's right border read
// as shifted left of the color.
function tSpanPx(t_start, t_end) {
  const dpr = window.devicePixelRatio || 1;
  const nCols = Math.max(1, Math.round(chartWidth() * dpr));
  const scale = nCols / (S.view.t1 - S.view.t0);
  const xl = Math.ceil((t_start - S.view.t0) * scale) / dpr;
  const xr = Math.ceil((t_end   - S.view.t0) * scale) / dpr;
  return { xl, xr };
}
// Per-entry height: signal lanes are full ROW_H, no-signal grouping rows
// are the shorter MODULE_H. Both the sidebar (CSS) and the canvas use
// these same numbers, so vertical positions stay in lockstep.
function entryHeightOf(e) { return e.type === "module" ? MODULE_H : ROW_H; }
// Cumulative top offset of each entry (excludes TIMELINE_H/scroll). Rebuilt
// whenever S.entries changes so canvas lane Y positions track the variable
// row heights instead of assuming a uniform ROW_H.
function recomputeEntryTops() {
  S.entryTops = new Array(S.entries.length);
  let acc = 0;
  for (let i = 0; i < S.entries.length; i++) {
    S.entryTops[i] = acc;
    acc += entryHeightOf(S.entries[i]);
  }
  S.contentH = acc;
}
function entryYTop(idx) {
  const top = S.entryTops && idx < S.entryTops.length ? S.entryTops[idx] : 0;
  return TIMELINE_H + top - getScrollY();
}
function entryAtY(y) {
  if (y < TIMELINE_H) return -1;
  const tops = S.entryTops;
  if (!tops || !tops.length) return -1;
  const yRel = y - TIMELINE_H + getScrollY();
  // Last entry whose top is ≤ yRel (binary search over the sorted tops).
  let lo = 0, hi = tops.length - 1, ans = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (tops[mid] <= yRel) { ans = mid; lo = mid + 1; }
    else hi = mid - 1;
  }
  if (ans < 0) return -1;
  // Reject clicks in the empty space past the final row.
  if (yRel >= tops[ans] + entryHeightOf(S.entries[ans])) return -1;
  return ans;
}
function getScrollY() { return sidebarEl.scrollTop; }
function setScrollY(v) { sidebarEl.scrollTop = v; }

// ── DPR-aware canvas sizing ──────────────────────────────────────────

// Track last-applied sizes so we don't waste a full reupload+clear on
// every draw — only when the client really resized.
const _sizeCache = new WeakMap();
function ensureCanvasSize(cv, c) {
  const dpr = window.devicePixelRatio || 1;
  const w = cv.clientWidth, h = cv.clientHeight;
  const cached = _sizeCache.get(cv);
  if (cached && cached.w === w && cached.h === h && cached.dpr === dpr) {
    return false;
  }
  cv.width  = w * dpr;
  cv.height = h * dpr;
  c.setTransform(dpr, 0, 0, dpr, 0, 0);
  _sizeCache.set(cv, { w, h, dpr });
  return true;
}

window.addEventListener("resize", () => { scheduleDrawBg(); });

// ── rAF-coalesced draws ──────────────────────────────────────────────
//
// Three independent layers:
//   bg  — timeline + cycle grid + lane data. Heavy, only redraws when
//         view, scroll, data, or visible-lane set changes.
//   sel — selection band + Y/X-AND block highlights + clicked-event
//         lift. Redraws on selection state change (drag-select runs
//         here every frame instead of repainting all lanes).
//   ov  — cursor pill. Redraws on mousemove.
//
// scheduleDrawBg() implies a sel + ov redraw too because block
// positions move with the view. scheduleDrawSel() / scheduleDrawOv()
// only redraw their own layer.

let _bgScheduled = false, _selScheduled = false, _ovScheduled = false;
function scheduleDrawBg() {
  if (_bgScheduled) return;
  _bgScheduled = true;
  requestAnimationFrame(() => {
    _bgScheduled = false; _selScheduled = false; _ovScheduled = false;
    drawBg(); drawSel(); drawOv();
  });
}
function scheduleDrawSel() {
  if (_bgScheduled || _selScheduled) return;
  _selScheduled = true;
  requestAnimationFrame(() => {
    _selScheduled = false;
    if (_bgScheduled) return;   // bg's rAF will redraw sel anyway
    drawSel();
  });
}
function scheduleDrawOv() {
  if (_bgScheduled || _ovScheduled) return;
  _ovScheduled = true;
  requestAnimationFrame(() => {
    _ovScheduled = false;
    if (_bgScheduled) return;
    drawOv();
  });
}

// ── draw: background (lanes, grid, ruler, selection bands) ───────────

function pickStep(span) {
  const target = span / 10;
  const exp = Math.pow(10, Math.floor(Math.log10(target)));
  const mant = target / exp;
  const nice = mant < 2 ? 1 : mant < 5 ? 2 : 5;
  return Math.max(1, Math.round(nice * exp));
}

function drawTimeline(ctx, w) {
  ctx.fillStyle = "#ffffff"; ctx.fillRect(0, 0, w, TIMELINE_H);
  ctx.strokeStyle = "#e1e4e8";
  ctx.beginPath();
  ctx.moveTo(0, TIMELINE_H - 0.5); ctx.lineTo(w, TIMELINE_H - 0.5); ctx.stroke();
  // Build stamp — settles "which code is this page running" instantly.
  ctx.fillStyle = "#8a93a0"; ctx.font = "10px 'JetBrains Mono', monospace";
  ctx.textAlign = "right"; ctx.textBaseline = "alphabetic";
  ctx.fillText(`v${WIRE_FORMAT}.${APP_BUILD}`, w - 4, 11);
  ctx.textAlign = "start";
  if (S.allSignals.length === 0) return;
  // Tick on whole-cycle multiples and label in cycles: pick a nice step
  // in cycle space, then map back to raw time only for x positioning.
  const period = S.cycleTime;
  const stepCyc = pickStep((S.view.t1 - S.view.t0) / period);
  const stepT = stepCyc * period;
  const start = Math.ceil(S.view.t0 / stepT) * stepT;
  const unit = chooseUnit(stepCyc);
  ctx.fillStyle = "#57606a"; ctx.font = "12px 'Inter', sans-serif";
  ctx.textBaseline = "alphabetic";
  for (let t = start; t <= S.view.t1; t += stepT) {
    const x = tToX(t);
    ctx.strokeStyle = "#c8ced6";
    ctx.beginPath(); ctx.moveTo(x + 0.5, TIMELINE_H - 7); ctx.lineTo(x + 0.5, TIMELINE_H); ctx.stroke();
    ctx.fillText(fmtT(t / period, unit), x + 3, TIMELINE_H - 10);
  }
}

function drawCycleGrid(ctx, w, H) {
  if (!S.cyclePeriod) return;
  const span = S.view.t1 - S.view.t0;
  // The whole grid is gated on the ×1 spacing: ×10/×100 lines without
  // their ×1 ruling read as unexplained stripes, so all tiers appear
  // and disappear together.
  const unitPx = w / span;
  if (unitPx < UNIT_GRID_MIN_PX) return;
  ctx.save();
  ctx.lineWidth = 1;
  // ×1 time-unit lines — the lightest tier, drawn first so the cycle
  // (×10) and decade (×100) lines paint over them where they coincide.
  ctx.strokeStyle = "#eceff2";
  ctx.beginPath();
  for (let t = Math.ceil(S.view.t0); t <= S.view.t1; t += 1) {
    const x = Math.round(tToX(t)) + 0.5;
    ctx.moveTo(x, TIMELINE_H); ctx.lineTo(x, H);
  }
  ctx.stroke();
  const p = S.cyclePeriod;
  const start = Math.ceil(S.view.t0 / p) * p;
  for (let t = start; t <= S.view.t1; t += p) {
    const x = Math.round(tToX(t)) + 0.5;
    const isStrong = Math.round(t / p) % 10 === 0;
    ctx.strokeStyle = isStrong ? "#b6bdc6" : "#ccd2d9";
    ctx.beginPath(); ctx.moveTo(x, TIMELINE_H); ctx.lineTo(x, H); ctx.stroke();
  }
  ctx.restore();
}

// Global value → palette-index registry. Every distinct value gets one
// palette slot on first sight (insertion order), shared by all signals
// and all zoom levels — so a given number paints the same color
// everywhere, instead of the `v % 16` arithmetic that made neighboring
// counter values cycle through unrelated hues.
const VALUE_COLOR = new Map();
function colorIdxOf(v) {
  let i = VALUE_COLOR.get(v);
  if (i === undefined) {
    i = VALUE_COLOR.size % STATE_COLORS.length;
    VALUE_COLOR.set(v, i);
  }
  return i;
}
function stateColor(v) { return STATE_COLORS[colorIdxOf(v)]; }
// Pre-seed slots for the small values: v in 0..15 always owns palette
// slot v, so the everyday pairs (0/1 flags, small state ids) can never
// collide with each other. Without this, the boot-time registration of
// every lane's value list (all lanes are blocks by default now) can
// land 0 and 1 a multiple of 16 slots apart — same color.
for (let v = 0; v < STATE_COLORS.length; ++v) colorIdxOf(v);

function bucketRect(b) {
  // Device-pixel-snapped to the fold's colored extent so the highlight
  // outline sits flush with the painted block (see tSpanPx).
  const { xl: xa, xr: xb } = tSpanPx(b.t_start, b.t_end);
  const wPx = Math.max(1, xb - xa);
  // Always carve out the trailing gap when there's room, regardless of
  // count — aggregated buckets need to read as discrete cells too, not
  // as one continuous slab. Sub-CYCLE_MIN_PX buckets stay full-width so
  // they don't vanish entirely at extreme zoom-out.
  let drawW = wPx;
  if (wPx > CYCLE_MIN_PX) drawW = Math.max(1, wPx - CYCLE_GAP);
  return { x: xa, w: drawW };
}

// Does a bar lane draw a white cell-separator gap on the RIGHT of buckets[bi]?
// The single source of truth shared by the fill (drawBarLane) and the click
// highlight (collectLitRects), so the selection outline's right border lands
// on the same edge the fill does. A gap needs a wide single-value block AND a
// wide next neighbour; the LAST block, or one followed by a sub-pixel bucket,
// gets none — its color runs to the full xr. That asymmetry is exactly why
// the rightmost block's highlight looked different from the interior ones.
function barBlockHasRightGap(buckets, bi) {
  const b = buckets[bi];
  if (!b || b.v_min !== b.v_max) return false;   // mixed: never a crisp cell
  const sp = tSpanPx(b.t_start, b.t_end);
  if (sp.xr - sp.xl < FOLD_MAX_BUCKET_PX) return false;
  const nx = buckets[bi + 1];
  if (!nx) return false;                          // last block: no separator
  const nsp = tSpanPx(nx.t_start, nx.t_end);
  if (nsp.xr - nsp.xl < FOLD_MAX_BUCKET_PX) return false;
  return true;
}

// Cache (font, label) → measured width. Hot path: every drawBarLane
// frame would otherwise call measureText hundreds of times with the
// same small set of integer labels.
const _textWidthCache = new Map();
function measureLabel(ctx, label) {
  let m = _textWidthCache.get(label);
  if (m === undefined) {
    m = ctx.measureText(label).width;
    if (_textWidthCache.size > 2048) _textWidthCache.clear();
    _textWidthCache.set(label, m);
  }
  return m;
}
// The first draws happen before the monospace webfont arrives, so the
// cache pins FALLBACK-font widths and the early frames rasterize labels
// in the fallback face. When the real font lands: drop the stale
// measurements and repaint once — otherwise label geometry silently
// differs between before/after, i.e. the same view renders differently
// depending on when it was first drawn.
if (document.fonts && document.fonts.ready) {
  document.fonts.ready.then(() => {
    _textWidthCache.clear();
    scheduleDrawBg();
  });
}

// ── per-pixel column fold (zoomed-out rendering) ─────────────────────
//
// When buckets shrink below a few px each, per-bucket cells stop being
// honest: every bucket gets a ≥1px rect, so several buckets overdraw
// the same pixel and the surviving color is decided by palette order,
// not by the data; the 8px stripe pattern for mixed buckets degrades
// into noise at 1–2px; and labels vanish because no single bucket is
// wide enough to carry one, even where the value is constant for
// thousands of cycles.
//
// The fold replaces all of that with one pass per lane: every *device*
// pixel column aggregates the buckets overlapping its time slice into
// a (v_min, v_max, v_last) envelope. Holes between buckets are bridged
// with the previous bucket's v_last — step semantics, the value holds
// until the next event — which matches what the server's raw mode
// shows when zoomed in. Bar lanes blit the columns through a 2-row
// ImageData stretched to lane height (top row = v_max color, bottom
// row = v_min color: a single-value column reads solid, a column
// hiding transitions reads two-tone). Runs of columns holding one
// constant value are then merged and labelled, so numbers stay
// readable at any zoom where the data really is stable.

// STATE_COLORS packed as RGBA bytes for direct ImageData writes.
const PALETTE_U32 = (() => {
  const out = new Uint32Array(STATE_COLORS.length);
  const u8 = new Uint8Array(out.buffer);
  for (let i = 0; i < STATE_COLORS.length; ++i) {
    u8[i * 4 + 0] = parseInt(STATE_COLORS[i].slice(1, 3), 16);
    u8[i * 4 + 1] = parseInt(STATE_COLORS[i].slice(3, 5), 16);
    u8[i * 4 + 2] = parseInt(STATE_COLORS[i].slice(5, 7), 16);
    u8[i * 4 + 3] = 255;
  }
  return out;
})();

// Wave-lane cell↔fold switch (bar lanes always fold — see drawBarLane):
//  - Aggregated levels (level ≥ 1) always fold.
//  - Raw data folds below FOLD_MAX_BUCKET_PX, with hysteresis (exit at
//    +4px) so a wheel notch hovering at the threshold doesn't flicker
//    between the two renderings.
function laneFolds(lane, data, avgPx) {
  if (data.level >= 1) { lane.folded = true; return true; }
  const thr = lane.folded ? FOLD_MAX_BUCKET_PX + 4 : FOLD_MAX_BUCKET_PX;
  lane.folded = avgPx < thr;
  return lane.folded;
}

// Scratch buffers reused across lanes and frames — drawBg renders lanes
// serially, so one set is enough and per-frame allocation stays zero.
//
// The lane image is written to the TARGET canvas with one direct
// putImageData — never via an intermediate canvas + drawImage. The old
// "putImageData into a shared scratch canvas, immediately drawImage it,
// repeat per lane" pattern raced the GPU's texture upload on
// accelerated 2D canvases: lanes intermittently sampled the PREVIOUS
// lane's pixels (several lanes showing one lane's content) or got the
// upload's channel order wrong (red/blue swapped colors) — timing
// dependent, machine dependent, invisible under software rendering.
const foldBuf = {
  nCols: 0,
  present: null,            // Uint8Array  — column has data
  vMin: null, vMax: null,   // Float64Array — value envelope per column
  vLast: null,              // Float64Array — latest value per column
  // The step function SAMPLED at each column's left edge: the value the
  // signal actually holds at that instant. This is the column's color —
  // no shares, no blending, no thresholds. NaN = the column's start
  // instant precedes all painted content (fall back to vLast).
  sampleV: null,            // Float64Array
  rendV: null,              // Float64Array — value a label would print
  rendShare: null,          // Uint8Array   — 255 = transition-free column
  u32: null,                // Uint32Array — 2×nCols (row 0 top, row 1 bottom)
  hDev: 0,
  img: null, imgU32: null,  // nCols×hDev ImageData written to the target
};

function foldEnsure(nCols) {
  if (foldBuf.nCols !== nCols) {
    foldBuf.nCols = nCols;
    foldBuf.present = new Uint8Array(nCols);
    foldBuf.vMin = new Float64Array(nCols);
    foldBuf.vMax = new Float64Array(nCols);
    foldBuf.vLast = new Float64Array(nCols);
    foldBuf.sampleV = new Float64Array(nCols);
    foldBuf.rendV = new Float64Array(nCols);
    foldBuf.rendShare = new Uint8Array(nCols);
    foldBuf.u32 = new Uint32Array(nCols * 2);
    foldBuf.img = null;
    foldBuf.hDev = 0;
  }
}

function foldEnsureImage(nCols, hDev) {
  if (!foldBuf.img || foldBuf.hDev !== hDev) {
    foldBuf.img = new ImageData(nCols, hDev);
    foldBuf.imgU32 = new Uint32Array(foldBuf.img.data.buffer);
    foldBuf.hDev = hDev;
  }
}

// [lo, hi) of buckets intersecting the view. Buckets are sorted and
// non-overlapping, so t_start and t_end are both monotonic.
function visibleBucketRange(buckets) {
  const t0 = S.view.t0, t1 = S.view.t1;
  let a = 0, b = buckets.length;
  while (a < b) {
    const m = (a + b) >> 1;
    if (buckets[m].t_end < t0) a = m + 1; else b = m;
  }
  const lo = a;
  b = buckets.length;
  while (a < b) {
    const m = (a + b) >> 1;
    if (buckets[m].t_start <= t1) a = m + 1; else b = m;
  }
  return [lo, a];
}

// Fold buckets[lo..hi) — plus one bucket of left headroom for the
// prior state — into per-column envelopes. Only a span's first column
// can collide with earlier content (buckets are sorted and
// non-overlapping), so interior columns are bulk-filled.
function foldColumns(buckets, lo, hi, nCols) {
  const t0 = S.view.t0;
  const scale = nCols / (S.view.t1 - S.view.t0);
  const present = foldBuf.present,
        vMin = foldBuf.vMin, vMax = foldBuf.vMax, vLast = foldBuf.vLast,
        sampleV = foldBuf.sampleV;
  present.fill(0);
  sampleV.fill(NaN);
  const paint = (ts, te, mn, mx, lv) => {
    let f0 = (ts - t0) * scale;
    let f1 = (te - t0) * scale;
    if (f1 < 0 || f0 >= nCols) return;
    if (f0 < 0) f0 = 0;
    if (f1 > nCols) f1 = nCols;
    let c0 = Math.floor(f0);
    let c1 = Math.min(nCols - 1, Math.floor(f1));
    if (c1 < c0) c1 = c0;
    // Columns whose LEFT-EDGE instant falls inside [f0, f1) sample this
    // span's value — the signal's step function evaluated at the
    // pixel's start. Spans tile the axis, so every column start lands
    // in exactly one span: the assignment is exact and unambiguous,
    // with no aggregation involved. (For a sub-pixel aggregated bucket
    // `lv` is the value at the bucket's end — the sampling instant is
    // off by less than one pixel, never more.)
    const cs = Math.ceil(f0);
    const ce = Math.min(nCols - 1, Math.ceil(f1) - 1);
    for (let c = cs; c <= ce; ++c) sampleV[c] = lv;
    const upd = (c, mnv, mxv, lvv) => {
      if (present[c]) {
        if (mnv < vMin[c]) vMin[c] = mnv;
        if (mxv > vMax[c]) vMax[c] = mxv;
        vLast[c] = lvv;
      } else {
        present[c] = 1;
        vMin[c] = mnv; vMax[c] = mxv; vLast[c] = lvv;
      }
    };
    if (c0 === c1) {
      upd(c0, mn, mx, lv);
      return;
    }
    upd(c0, mn, mx, lv);
    upd(c1, mn, mx, lv);
    if (c1 > c0 + 1) {
      // interior columns — fully covered by this span
      present.fill(1, c0 + 1, c1);
      vMin.fill(mn, c0 + 1, c1);
      vMax.fill(mx, c0 + 1, c1);
      vLast.fill(lv, c0 + 1, c1);
    }
  };
  for (let i = Math.max(0, lo - 1); i < hi; ++i) {
    const b = buckets[i];
    paint(b.t_start, b.t_end, b.v_min, b.v_max, b.v_last);
    // Bridge the hole to the next bucket: the value holds until the
    // next event (step semantics).
    if (i + 1 < buckets.length) {
      const nx = buckets[i + 1];
      if (nx.t_start > b.t_end) {
        paint(b.t_end, nx.t_start, b.v_last, b.v_last, b.v_last);
      }
    }
  }
}

function drawBarLaneFolded(ctx, lane, y, w, buckets, lo, hi) {
  const dpr = window.devicePixelRatio || 1;
  const nCols = Math.max(1, Math.round(w * dpr));
  foldEnsure(nCols);
  foldColumns(buckets, lo, hi, nCols);
  const present = foldBuf.present,
        vMin = foldBuf.vMin, vMax = foldBuf.vMax, vLast = foldBuf.vLast,
        sampleV = foldBuf.sampleV,
        rendV = foldBuf.rendV, rendShare = foldBuf.rendShare;
  const u32 = foldBuf.u32;
  for (let c = 0; c < nCols; ++c) {
    if (!present[c]) { u32[c] = 0; u32[nCols + c] = 0; rendShare[c] = 0; continue; }
    // Sampling semantics — the classic waveform-viewer rule, with an
    // ABSOLUTE guarantee and no thresholds of any kind: the column's
    // color is the color of the value the signal holds at the column's
    // start instant. Consequences, all exact:
    //   - a value held for ≥1 screen pixel paints its own color at
    //     EVERY zoom level (its run covers the column start);
    //   - colors on screen are always real values' colors;
    //   - aggregation (shares, majorities, blends) never touches the
    //     hue, so there is nothing that can flip when zoom changes.
    // Sub-pixel content is sampled like any oscilloscope/waveform tool:
    // it contributes only where a column start happens to land on it.
    let sv = sampleV[c];
    if (sv !== sv) sv = vLast[c];   // NaN: content begins mid-column
    rendV[c] = sv;
    // 255 ⇔ the column contains no transition at all — exact predicate
    // (v_min == v_max over everything overlapping the column), used by
    // the label pass and the transition shading below.
    rendShare[c] = vMin[c] === vMax[c] ? 255 : 0;
    const col = PALETTE_U32[colorIdxOf(sv)];
    u32[c] = col; u32[nCols + c] = col;
  }
  // Columns that contain at least one transition get a darkened bottom
  // half (fixed strength — presence of an edge is a yes/no fact, not a
  // proportion). Transition-free stretches stay untouched, so a block
  // of one value reads as one clean solid block.
  for (let c = 0; c < nCols; ++c) {
    if (!present[c] || rendShare[c] === 255) continue;
    const m = u32[nCols + c];
    const f = 200;                     // ≈ −22%
    u32[nCols + c] = (0xff000000
      | (((((m >> 16) & 0xff) * f) >> 8) << 16)
      | (((((m >> 8) & 0xff) * f) >> 8) << 8)
      | (((m & 0xff) * f) >> 8)) >>> 0;
  }
  // Write the lane image with ONE direct putImageData: row 0 replicated
  // over the top half, row 1 over the bottom half, restricted to the
  // columns that actually hold data (putImageData replaces pixels
  // including alpha, so transparent edge columns must not erase the
  // background). No intermediate canvas, no drawImage, no GPU texture
  // round-trip — see foldBuf's comment for the race this avoids.
  const topDev = Math.round((y + 3) * dpr);
  const botDev = Math.round((y + LANE_H - 3) * dpr);
  const hDev = Math.max(2, botDev - topDev);
  const halfDev = Math.max(1, Math.round(hDev / 2));
  let cFirst = -1, cLast = -1;
  for (let c = 0; c < nCols; ++c) {
    if (present[c]) { if (cFirst < 0) cFirst = c; cLast = c; }
  }
  if (cFirst >= 0) {
    foldEnsureImage(nCols, hDev);
    const full = foldBuf.imgU32;
    const rowTop = u32.subarray(0, nCols);
    const rowBot = u32.subarray(nCols, nCols * 2);
    for (let r = 0; r < hDev; ++r) {
      full.set(r < halfDev ? rowTop : rowBot, r * nCols);
    }
    // putImageData ignores the canvas clip, so a lane scrolled partly
    // under the ruler would blit straight over it — drop the rows above
    // TIMELINE_H via the dirty rect instead (dirtyY shifts the
    // destination down by the same amount).
    const cut = Math.max(0, Math.ceil(TIMELINE_H * dpr) - topDev);
    if (cut < hDev) {
      ctx.putImageData(foldBuf.img, 0, topDev,
                       cFirst, cut, cLast - cFirst + 1, hDev - cut);
    }
  }

  // Labels: only on maximal stretches of transition-free columns
  // holding one value (rendShare 255 = exact no-edge predicate). A
  // number printed on a region asserts "this is all that value", so a
  // stretch containing any transition gets no label; the label run
  // splits exactly at real transition positions, which are fixed
  // points of the data — not of the zoom.
  ctx.font = "11px 'JetBrains Mono', monospace";
  ctx.textBaseline = "middle";
  ctx.textAlign = "center";
  ctx.fillStyle = "#24292f";
  ctx.strokeStyle = "rgba(255,255,255,0.8)";
  ctx.lineWidth = 3;
  ctx.lineJoin = "round";
  const cy = y + 3 + (LANE_H - 6) / 2 + 1;
  for (let c = 0; c < nCols; ) {
    if (!present[c] || rendShare[c] < 254) { ++c; continue; }
    const v = rendV[c];
    let e = c + 1;
    while (e < nCols && present[e] && rendShare[e] >= 254 && rendV[e] === v) ++e;
    const x0 = c / dpr, x1 = e / dpr;
    const label = laneLabelOf(lane, v) ?? `${v}`;
    if (x1 - x0 >= measureLabel(ctx, label) + 8) {
      const cx = (Math.max(x0, 0) + Math.min(x1, w)) / 2;
      ctx.strokeText(label, cx, cy);
      ctx.fillText(label, cx, cy);
    }
    c = e;
  }
  ctx.textAlign = "start";
  ctx.lineWidth = 1;
}

function drawWaveLaneFolded(ctx, lane, y, w, buckets, lo, hi) {
  const dpr = window.devicePixelRatio || 1;
  const nCols = Math.max(1, Math.round(w * dpr));
  foldEnsure(nCols);
  foldColumns(buckets, lo, hi, nCols);
  const present = foldBuf.present, vMn = foldBuf.vMin,
        vMx = foldBuf.vMax, vLs = foldBuf.vLast;
  // Vertical scale comes from the whole fetched cache — same source as
  // the per-bucket path, so autoscale doesn't jump when the mode flips.
  let vmin = Infinity, vmax = -Infinity;
  for (const b of buckets) {
    if (b.v_min < vmin) vmin = b.v_min;
    if (b.v_max > vmax) vmax = b.v_max;
  }
  if (vmin === vmax) { vmin -= 1; vmax += 1; }
  const topDev = (y + 3) * dpr, botDev = (y + LANE_H - 3) * dpr;
  const spanDev = botDev - topDev;
  const denom = vmax - vmin;
  const bandPath = new Path2D();
  const linePath = new Path2D();
  let started = false;
  for (let c = 0; c < nCols; ++c) {
    if (!present[c]) continue;
    const yh = botDev - (vMx[c] - vmin) / denom * spanDev;
    const yl = botDev - (vMn[c] - vmin) / denom * spanDev;
    bandPath.rect(c, yh, 1, Math.max(1, yl - yh));
    const ly = botDev - (vLs[c] - vmin) / denom * spanDev;
    if (!started) { linePath.moveTo(c + 0.5, ly); started = true; }
    else            linePath.lineTo(c + 0.5, ly);
  }
  ctx.save();
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.fillStyle = "rgba(10, 102, 194, 0.45)";
  ctx.fill(bandPath);
  ctx.strokeStyle = "#0a66c2"; ctx.lineWidth = 1.2 * dpr;
  ctx.stroke(linePath);
  ctx.restore();

  ctx.fillStyle = "#8a93a0"; ctx.font = "10.5px 'JetBrains Mono', monospace";
  ctx.textBaseline = "alphabetic"; ctx.textAlign = "right";
  ctx.fillText(fmtV(vmax, lane), w - 4, y + 3 + 8);
  ctx.fillText(fmtV(vmin, lane), w - 4, y + LANE_H - 3 - 1);
  ctx.textAlign = "start";
}

function drawBarLane(ctx, lane, y, w) {
  const data = S.laneData.get(lane.id);
  if (!data || data.buckets.length === 0) return;
  const buckets = data.buckets;
  const [lo, hi] = visibleBucketRange(buckets);
  ctx.save();
  ctx.beginPath(); ctx.rect(0, y, w, LANE_H); ctx.clip();

  // Always start from the per-pixel folded underlay. There is no
  // cell-mode/fold-mode switch any more: a per-bucket cell pass drew a
  // ≥1px rect for every bucket, so wherever buckets were sub-pixel
  // (dense bursts inside a lane whose *average* bucket is wide) several
  // rects overdrew one pixel and the surviving color was decided by
  // draw order — wheel-zooming shifted the overlap sets every frame and
  // the burst regions flickered through colors. The fold resolves each
  // pixel column from the data envelope instead, which is stable under
  // any sub-pixel shift, and draws the labels too. (When hi <= lo the
  // view sits in a fetch-edge hole; the fold bridges the held value
  // from bucket lo-1.)
  drawBarLaneFolded(ctx, lane, y, w, buckets, lo, hi);

  // Transition gaps on top — only between two buckets that are BOTH
  // individually visible. A gap after a wide block whose neighbour is
  // sub-pixel would chop a "mostly one value" stretch into pieces for
  // no readable reason; the sub-pixel content already shows as a
  // blended sliver where significant.
  const top = y + 3, h = LANE_H - 6;
  ctx.fillStyle = "#ffffff";
  for (let bi = lo; bi < hi; ++bi) {
    if (!barBlockHasRightGap(buckets, bi)) continue;
    // Device-pixel colored right edge (see tSpanPx). The gap carves the last
    // CYCLE_GAP px off it; the same edge the highlight aligns its right
    // border to, via the shared barBlockHasRightGap decision.
    const { xr: xb } = tSpanPx(buckets[bi].t_start, buckets[bi].t_end);
    if (xb < 0 || xb - CYCLE_GAP > w) continue;
    ctx.fillRect(xb - CYCLE_GAP, top, CYCLE_GAP, h);
  }
  ctx.restore();
}

function drawWaveLane(ctx, lane, y, w) {
  const data = S.laneData.get(lane.id);
  if (!data || data.buckets.length === 0) return;
  const buckets = data.buckets;
  const [lo, hi] = visibleBucketRange(buckets);
  ctx.save();
  ctx.beginPath(); ctx.rect(0, y, w, LANE_H); ctx.clip();
  if (hi <= lo) {
    // Hole between buckets — bridge the held value via the fold path.
    if (lo > 0 && lo < buckets.length) {
      drawWaveLaneFolded(ctx, lane, y, w, buckets, lo, lo);
    }
    ctx.restore();
    return;
  }
  const xA = Math.max(0, tToX(buckets[lo].t_start));
  const xB = Math.min(w, tToX(buckets[hi - 1].t_end));
  if (laneFolds(lane, data, (xB - xA) / (hi - lo))) {
    drawWaveLaneFolded(ctx, lane, y, w, buckets, lo, hi);
    ctx.restore();
    return;
  }
  let vmin = Infinity, vmax = -Infinity;
  for (const b of buckets) { if (b.v_min < vmin) vmin = b.v_min; if (b.v_max > vmax) vmax = b.v_max; }
  if (vmin === vmax) { vmin -= 1; vmax += 1; }
  const top = y + 3, bot = y + LANE_H - 3;
  const span = bot - top;
  const denom = vmax - vmin;
  const yOf = (v) => bot - (v - vmin) / denom * span;

  // All vmin-vmax bands share a single color → one Path2D + one fill.
  // Stroke path is built in the same pass to skip a second iteration.
  // One bucket of headroom on each side keeps the v_last line entering
  // and leaving the view without a visible cut.
  const bandPath = new Path2D();
  const linePath = new Path2D();
  let started = false;
  for (let bi = Math.max(0, lo - 1); bi < Math.min(buckets.length, hi + 1); ++bi) {
    const b = buckets[bi];
    const r = bucketRect(b);
    if (r.x + r.w < 0 || r.x > w) continue;
    const yh = yOf(b.v_max), yl = yOf(b.v_min);
    bandPath.rect(r.x, yh, r.w, Math.max(1, yl - yh));
    const lx = tToX((b.t_start + b.t_end) / 2);
    const ly = yOf(b.v_last);
    if (!started) { linePath.moveTo(lx, ly); started = true; }
    else            linePath.lineTo(lx, ly);
  }
  ctx.fillStyle = "rgba(10, 102, 194, 0.45)";
  ctx.fill(bandPath);
  ctx.strokeStyle = "#0a66c2"; ctx.lineWidth = 1.2;
  ctx.stroke(linePath);

  ctx.fillStyle = "#8a93a0"; ctx.font = "10.5px 'JetBrains Mono', monospace";
  ctx.textBaseline = "alphabetic"; ctx.textAlign = "right";
  ctx.fillText(fmtV(vmax, lane), w - 4, top + 8);
  ctx.fillText(fmtV(vmin, lane), w - 4, bot - 1);
  ctx.textAlign = "start";
  ctx.restore();
}

// Hidden lane: one flat grey bar spanning the signal's data extent, drawn
// instead of its per-value cells / waveform. A single continuous strip —
// no gaps, colours, stripes, or labels — so a muted signal reads as one
// quiet grey line. Spans from the first on-screen bucket to the last; when
// the view sits inside the data it fills the whole visible row.
function drawHiddenLane(ctx, lane, y, w) {
  const data = S.laneData.get(lane.id);
  if (!data || data.buckets.length === 0) return;
  let x0 = Infinity, x1 = -Infinity;
  for (const b of data.buckets) {
    const xa = tToX(b.t_start), xb = tToX(b.t_end);
    if (xb < 0 || xa > w) continue;
    if (xa < x0) x0 = xa;
    if (xb > x1) x1 = xb;
  }
  const left = Math.max(x0, 0), right = Math.min(x1, w);
  if (right <= left) return;
  ctx.save();
  ctx.fillStyle = HIDDEN_LANE_FILL;
  ctx.fillRect(left, y + 3, right - left, LANE_H - 6);
  ctx.restore();
}

// Module rows: nothing to paint. Leave the white background + cycle grid
// visible through them so a collapsed module looks like a clean gap with
// the same grid lines as the data rows.
function drawModuleDivider(/* ctx, y, w */) {}

// Highlight the single bucket the user clicked on — shadow + heavy
// outline lift, used only when zoomed in enough that the bucket
// represents one discrete event (v_min == v_max). Aggregate / mixed
// buckets are skipped: outlining a striped cell as "the selected
// state" would misrepresent it.
// Gather the on-screen rects that should stay lit; the spotlight veils
// everything else. Covers every selection mode at once — a clicked block,
// an X-axis time-range drag, Y-axis lane picks, and their intersection.
// A clicked-block rect also carries {lane, target, xa, bw} so its detail
// tip can be drawn; range/lane rects carry geometry only.
// Every time-range band that should paint right now: the finalized ones in
// S.selections plus the in-flight drag, each filtered to a visible width
// (>= 1 raw-time unit). Shared by the spotlight rects and the dashed edges.
function selectionBands() {
  const bands = S.selPending ? [...S.selections, S.selPending] : S.selections;
  return bands.filter((s) => Math.abs(s.t1 - s.t0) >= 1);
}

function collectLitRects(w, H) {
  const rects = [];

  // 1) Clicked blocks (S.events) — each lit individually so a cmd-built
  //    multi-pick shows every block at once.
  for (const ev of S.events) {
    const idx = S.entries.findIndex(
      (e) => e.type === "signal" && e.lane.id === ev.lane.id);
    const data = idx >= 0 ? S.laneData.get(ev.lane.id) : null;
    if (!data || !data.buckets.length) continue;
    let target = null, targetIdx = -1;
    for (let bi = 0; bi < data.buckets.length; ++bi) {
      const b = data.buckets[bi];
      if (b.t_start <= ev.t && ev.t < b.t_end) { target = b; targetIdx = bi; break; }
      if (b.t_start > ev.t) break;
      target = b; targetIdx = bi;
    }
    // A solid bucket always lights; a mixed aggregate only when it was
    // pinned as such (ev.mix) — a solid pick whose bucket re-aggregated
    // into a mixed one on zoom-out still hides, as before.
    if (target && (target.v_min === target.v_max || ev.mix)) {
      const y = entryYTop(idx);
      if (!(y + LANE_H < TIMELINE_H || y > H)) {
        // Align the outline to the block's VISIBLE color edge. The right
        // edge is the device-pixel colored extent xr, minus the cell gap
        // ONLY when the fill actually carves one (shared decision, so the
        // last block / a narrow-neighbour block — which keep their full xr —
        // no longer highlight differently from the interior ones).
        let xa, bw;
        if (ev.lane.kind === "bar") {
          const { xl, xr } = tSpanPx(target.t_start, target.t_end);
          xa = xl;
          bw = Math.max(1, (barBlockHasRightGap(data.buckets, targetIdx)
                            ? xr - CYCLE_GAP : xr) - xl);
        } else {
          // Wave lane: the cell band is drawn via bucketRect, so match it.
          const r = bucketRect(target); xa = r.x; bw = r.w;
        }
        if (xa + bw >= 0 && xa <= w) {
          rects.push({ x: xa, y: y + 3, w: bw, h: LANE_H - 6,
                       lane: ev.lane, target, xa, bw });
        }
      }
    }
  }

  // 2) X-axis time ranges ∧ Y-axis lane picks — one rect per (band, covered
  //    lane), clipped to that lane's data extent. Every finalized band plus
  //    the in-flight drag contributes; with no band but lanes picked, the
  //    whole lane lights (an open-ended [-∞, +∞] band).
  const bands = selectionBands();
  const hasLane = S.selectedLanes.size > 0;
  if (bands.length || hasLane) {
    const ranges = bands.length ? bands : [{ t0: -Infinity, t1: Infinity }];
    S.entries.forEach((e, idx) => {
      if (e.type !== "signal") return;
      const lane = e.lane;
      if (hasLane && !S.selectedLanes.has(lane.id)) return;
      const y = entryYTop(idx);
      if (y + LANE_H < TIMELINE_H || y > H) return;
      const data = S.laneData.get(lane.id);
      if (!data || data.buckets.length === 0) return;
      const first = data.buckets[0], last = data.buckets[data.buckets.length - 1];
      for (const r of ranges) {
        const a = Math.min(r.t0, r.t1), b = Math.max(r.t0, r.t1);
        const x0 = Math.max(tToX(Math.max(a, first.t_start)), 0);
        const x1 = Math.min(tToX(Math.min(b, last.t_end)), w);
        if (x1 <= x0) continue;
        rects.push({ x: x0, y: y + 3, w: x1 - x0, h: LANE_H - 6 });
      }
    });
  }

  return rects;
}

// Dashed vertical lines at the time-range selection's two boundaries, so
// the exact t0/t1 stay readable across the dark veil and the gaps between
// lanes. Only for an X-axis (time) selection; full chart height.
function drawSelectionEdges(ctx, w, H) {
  const bands = selectionBands();
  if (!bands.length) return;
  ctx.save();
  ctx.strokeStyle = "rgba(88, 166, 255, 0.9)";
  ctx.lineWidth = 1;
  ctx.setLineDash([3, 2]);
  ctx.beginPath();
  for (const s of bands) {
    const xa = tToX(Math.min(s.t0, s.t1));
    const xb = tToX(Math.max(s.t0, s.t1));
    if (xb < 0 || xa > w) continue;
    ctx.moveTo(xa + 0.5, TIMELINE_H); ctx.lineTo(xa + 0.5, H);
    ctx.moveTo(xb - 0.5, TIMELINE_H); ctx.lineTo(xb - 0.5, H);
  }
  ctx.stroke();
  ctx.restore();
}

function drawBg() {
  ensureCanvasSize(bgCv, bgCtx);
  const W = bgCv.clientWidth, H = bgCv.clientHeight;
  const w = chartWidth();
  bgCtx.fillStyle = "#ffffff"; bgCtx.fillRect(0, 0, W, H);
  drawCycleGrid(bgCtx, w, H);
  drawTimeline(bgCtx, w);
  bgCtx.save();
  bgCtx.beginPath(); bgCtx.rect(0, TIMELINE_H, W, H - TIMELINE_H); bgCtx.clip();
  S.entries.forEach((e, idx) => {
    const y = entryYTop(idx);
    if (y + LANE_H < TIMELINE_H || y > H) return;
    if (e.type === "module") {
      drawModuleDivider();
    } else {
      const lane = e.lane;
      if (S.hiddenLanes.has(lane.id)) drawHiddenLane(bgCtx, lane, y, w);
      else if (lane.kind === "bar")   drawBarLane(bgCtx, lane, y, w);
      else                            drawWaveLane(bgCtx, lane, y, w);
    }
  });
  bgCtx.restore();
}

// All selection visuals on their own canvas so drag-select can update
// at full frame rate without re-rendering the lane data underneath.
function drawSel() {
  ensureCanvasSize(selCv, selCtx);
  const W = selCv.clientWidth, H = selCv.clientHeight;
  const w = chartWidth();
  selCtx.clearRect(0, 0, W, H);

  // Spotlight (light theme), used identically by all selection modes: lay a
  // WHITE veil over the whole chart, then punch a hole at every lit rect so the
  // selection reads as a picked-out window. A white veil fades the unselected
  // colour blocks (block + translucent white = washed-out) while leaving the
  // pure-white background untouched (white + white = white) — so the
  // background never darkens, only the unselected blocks recede. An accent
  // outline crisps each hole so the selected blocks pop and adjacent
  // same-colour ones stay distinct.
  const lit = collectLitRects(w, H);
  // Veil whenever a selection is active — lanes picked, blocks lit, or a time
  // band drawn — NOT merely when a lit rect happens to be on-screen right now.
  // collectLitRects only emits a rect for a *visible* selected lane/block, so
  // gating the veil on lit.length left the chart un-dimmed once the selection
  // scrolled out of view: scrolling down past the picked signals un-greyed
  // every unselected lane below. Gate on the selection state so the veil holds
  // across scroll; the holes (lit) simply vanish when none are visible.
  const hasSel = S.selectedLanes.size > 0 || S.events.length > 0 ||
                 selectionBands().length > 0;
  if (hasSel) {
    selCtx.save();
    selCtx.beginPath(); selCtx.rect(0, TIMELINE_H, w, H - TIMELINE_H); selCtx.clip();
    selCtx.fillStyle = "rgba(255, 255, 255, 0.62)";
    selCtx.fillRect(0, TIMELINE_H, w, H - TIMELINE_H);
    for (const r of lit) {
      const hx = Math.max(0, r.x), hr = Math.min(w, r.x + r.w);
      if (hr > hx) selCtx.clearRect(hx, r.y, hr - hx, r.h);
    }
    selCtx.lineWidth = 1.5;
    selCtx.strokeStyle = "rgba(10, 102, 194, 0.95)";
    for (const r of lit) {
      const hx = Math.max(0, r.x), hr = Math.min(w, r.x + r.w);
      if (hr > hx) selCtx.strokeRect(hx + 0.75, r.y + 0.75, hr - hx - 1.5, r.h - 1.5);
    }
    selCtx.restore();
  }

  drawSelectionEdges(selCtx, w, H);
}

// ── draw: overlay (crosshair only) ───────────────────────────────────

// The clickable head of a time anchor lives in the ruler band; return the
// topmost (last-drawn) anchor whose head contains (x, y), or null. Off-screen
// anchors (scrolled out of the time view) report no hit — their head isn't
// painted, so it can't be clicked.
function anchorAtPoint(x, y) {
  for (let i = S.anchors.length - 1; i >= 0; i--) {
    const ax = tToX(S.anchors[i].t);
    const dx = x - ax, dy = y - ANCHOR_DOT_Y;
    if (dx * dx + dy * dy <= ANCHOR_HIT_R * ANCHOR_HIT_R) return S.anchors[i];
  }
  return null;
}

// Vertical marker lines + their ruler-band heads. Drawn on the overlay,
// before (under) the cursor crosshair, so a fresh hover still reads on top.
function drawAnchors(ctx, W, H) {
  for (const a of S.anchors) {
    const x = Math.round(tToX(a.t)) + 0.5;
    if (x < 0 || x > W) continue;  // scrolled out of view — keep in state, skip paint
    const selected = a === S.selectedAnchor;
    // Line hangs from the head (just under the tick numbers) to the chart
    // bottom — it no longer runs up through the ruler labels.
    ctx.lineWidth = selected ? 1.5 : 1;
    ctx.strokeStyle = selected ? "rgba(130,80,223,0.95)" : "rgba(130,80,223,0.5)";
    ctx.beginPath();
    ctx.moveTo(x, ANCHOR_DOT_Y); ctx.lineTo(x, H);
    ctx.stroke();
    // Head: a filled disc. Selected grows it and punches a white centre pip
    // (a target look) — the cue stays inside the disc, never reaching the
    // numbers above.
    ctx.beginPath();
    ctx.arc(x, ANCHOR_DOT_Y, selected ? ANCHOR_R + 1 : ANCHOR_R, 0, Math.PI * 2);
    ctx.fillStyle = ANCHOR_COLOR; ctx.fill();
    ctx.lineWidth = selected ? 2 : 1.5; ctx.strokeStyle = "#ffffff"; ctx.stroke();
    if (selected) {
      ctx.beginPath();
      ctx.arc(x, ANCHOR_DOT_Y, 1.7, 0, Math.PI * 2);
      ctx.fillStyle = "#ffffff"; ctx.fill();
    }
  }
}

function drawOv() {
  ensureCanvasSize(ovCv, ovCtx);
  const W = ovCv.clientWidth, H = ovCv.clientHeight;
  ovCtx.clearRect(0, 0, W, H);
  drawAnchors(ovCtx, W, H);  // persistent markers; independent of the cursor
  if (S.cursorX < 0 || S.cursorX > W) return;
  // Cursor indicator. The ruler tick + the time readout always track the
  // cursor so the top time axis keeps showing where the pointer is. The
  // full-height vertical line down through the waveforms is the only opt-in
  // part: it shows only while Shift is held, so the chart stays unobstructed
  // by default. Shift keydown/keyup schedule an overlay redraw so the line
  // appears/disappears the moment Shift changes, not just on the next move.
  const w = chartWidth();
  const cyc = xToT(S.cursorX) / S.cycleTime;
  ovCtx.save();
  const x = Math.round(S.cursorX) + 0.5;
  ovCtx.lineWidth = 1;
  ovCtx.strokeStyle = CROSSHAIR;
  ovCtx.beginPath();
  ovCtx.moveTo(x, TIMELINE_H - 10); ovCtx.lineTo(x, TIMELINE_H);
  ovCtx.stroke();
  if (S.keyDown.has("shift")) {
    ovCtx.strokeStyle = "rgba(207, 34, 46, 0.4)";
    ovCtx.beginPath();
    ovCtx.moveTo(x, TIMELINE_H); ovCtx.lineTo(x, H);
    ovCtx.stroke();
  }
  ovCtx.font = "500 12px 'JetBrains Mono', monospace";
  const label = fmtT(cyc);
  const tw = ovCtx.measureText(label).width;
  let lx = S.cursorX + 6;
  if (lx + tw > w) lx = S.cursorX - tw - 6;
  // Blank the ruler's label row under the number with the ruler's own
  // background, so the cursor readout never overprints a tick label —
  // the ruler reads as auto-hiding where the mouse is. Only the text
  // row is covered; the tick marks on the lower edge stay visible.
  ovCtx.fillStyle = "#ffffff";
  ovCtx.fillRect(lx - 4, 2, tw + 8, 18);
  ovCtx.fillStyle = CROSSHAIR; ovCtx.textBaseline = "middle";
  ovCtx.fillText(label, lx, 12);
  ovCtx.restore();
}

// ── interaction ──────────────────────────────────────────────────────

// Refetch policy: THROTTLE, not debounce, and strictly serialized.
//
// - Debounce (reset the timer on every event) never fires during a
//   continuous slow wheel/trackpad gesture — real mice emit dozens of
//   small deltas per second, so the view drifted further and further
//   from the cached window while rendering stretched stale data, then
//   snapped when the hand paused. Throttling guarantees a fetch at
//   least every REFETCH_MS during the gesture.
// - Serialization prevents out-of-order responses: two overlapping
//   fetches can resolve in either order, and the older window's data
//   would overwrite the newer one's — a backwards visual jump.
const REFETCH_MS = 50;
let refetchTimer = null;
let refetchInFlight = false;
let refetchDirty = false;
let lastRefetchAt = 0;

async function refetchNow() {
  if (refetchInFlight) { refetchDirty = true; return; }
  refetchInFlight = true;
  lastRefetchAt = performance.now();
  try {
    await fetchAllVisible();
  } catch (e) { /* transient network failure — next throttle retries */ }
  refetchInFlight = false;
  scheduleDrawBg();
  // The view window may have moved — refresh the selected-signal stats too
  // (deduped by key, so a scroll that didn't change the window is a no-op).
  scheduleSelectStats();
  if (refetchDirty) { refetchDirty = false; scheduleRefetch(); }
}

function scheduleRefetch() {
  if (refetchTimer) return;            // one pending call is enough
  const wait = Math.max(0, REFETCH_MS - (performance.now() - lastRefetchAt));
  refetchTimer = setTimeout(() => {
    refetchTimer = null;
    refetchNow();
  }, wait);
}

// ── selected-signal per-cycle stats ─────────────────────────────────
//
// Stats follow the HIGHLIGHTED region, reported PER SIGNAL (the mean value,
// the zero-cycle count + share, the total cycle count). Picking signals on
// the left is itself a highlight: with no time-range drag the spotlight lights
// each whole curve, so each signal's stats span its whole extent; once a band
// (S.selections) is dragged, the stats narrow to the union of the bands. The
// server computes it exactly from the RLE runs, so the numbers are right at
// any zoom and even for selected lanes scrolled off-screen (which the
// virtualized fetch never pulled). Throttled + keyed: a band/selection change
// triggers one request; a request the selection has already moved past is
// dropped via the token. (Plain pan/zoom changes neither the picked signals
// nor the bands, so its key matches and the call is a no-op.)
let statsTimer = null, statsToken = 0, statsKey = "";
function scheduleSelectStats() {
  if (statsTimer) return;
  statsTimer = setTimeout(() => { statsTimer = null; refreshSelectStats(); }, 90);
}
async function refreshSelectStats() {
  // Nothing picked → no stats. Otherwise the highlighted region = the dragged
  // band(s) if any, else each signal's whole extent (handled server-side when
  // ranges is empty).
  if (S.selectedLanes.size === 0) {
    statsKey = "";
    if (S.selStats) { S.selStats = null; renderEventPanel(); }
    return;
  }
  const ids = [...S.selectedLanes].sort((a, b) => a - b);
  const ranges = S.selections
    .filter((s) => Math.abs(s.t1 - s.t0) >= 1)
    .map((s) => {
      const a = Math.max(0, Math.floor(Math.min(s.t0, s.t1)));
      const b = Math.max(a + 1, Math.ceil(Math.max(s.t0, s.t1)));
      return `${a}:${b}`;
    });
  const key = `${ids.join(",")}|${ranges.join(",")}`;
  if (key === statsKey && S.selStats) return;   // already showing this set
  statsKey = key;
  const token = ++statsToken;
  try {
    const r = await jget(
      `/api/select_stats?sigs=${ids.join(",")}&ranges=${ranges.join(",")}`);
    if (token !== statsToken) return;            // a newer selection superseded us
    r.bySig = new Map(r.signals.map((s) => [s.sig, s]));
    S.selStats = r;
    renderEventPanel();
  } catch (e) { /* transient — the next band/selection change retries */ }
}

function minViewSpan() {
  // Floor on zoom-in: at least N cycles must remain visible.
  return Math.max(1, S.cyclePeriod * MIN_VIEW_CYCLES);
}
function maxViewSpan() {
  // Ceiling on zoom-out: the waveform must always occupy at least 1/5
  // of the chart, so view span ≤ 5 × bounds span. Falls back to a soft
  // limit before bounds is known.
  const bs = S.bounds.t1 - S.bounds.t0;
  return Math.max(1, bs * 5);
}

// Zoom anchored at the visible-view center: +/- keys, and the W/S
// fallback when the mouse is off the chart.
function zoomViewCenter(factor) {
  if (S.allSignals.length === 0) return;
  const span = S.view.t1 - S.view.t0;
  const center = (S.view.t0 + S.view.t1) / 2;
  const newSpan = Math.min(maxViewSpan(),
                           Math.max(minViewSpan(), span * factor));
  S.view = { t0: center - newSpan / 2, t1: center + newSpan / 2 };
}

// Mouse wheel zoom: anchored at the cursor x position — the point under
// the cursor stays put.
function zoomAt(cx, factor) {
  if (S.allSignals.length === 0) return;
  const cursorT = xToT(cx);
  const span = S.view.t1 - S.view.t0;
  const newSpan = Math.min(maxViewSpan(),
                           Math.max(minViewSpan(), span * factor));
  // Preserve the relative position of cursorT within the new view.
  const rel = span > 0 ? (cursorT - S.view.t0) / span : 0.5;
  S.view = { t0: cursorT - rel * newSpan, t1: cursorT + (1 - rel) * newSpan };
}
function panBy(deltaT) {
  if (S.allSignals.length === 0) return;
  // No bounds clamp — pan freely; whitespace shows past data extent.
  S.view = { t0: S.view.t0 + deltaT, t1: S.view.t1 + deltaT };
}

let dragMode = null, dragStart = null, mouseDownInfo = null;
// Drag-vs-click discrimination needs *both* a distance and a time
// threshold. Distance alone fails on "quick flick" gestures — a press
// that travels 30+ px in 80 ms is the user clicking, not drag-selecting.
// Time alone fails on slow, deliberate clicks where the finger lingers
// on a precision mouse. Requiring both keeps real drags responsive
// (small, sustained motion still escalates instantly) while reading
// any short-duration mousedown as a click no matter how far it slid.
const DRAG_THRESHOLD_PX = 6;
const DRAG_THRESHOLD_MS = 180;

// Mouse position in bgCv DRAWING coordinates (the [0..chartWidth] /
// [0..clientHeight] space the lanes, crosshair and selection bands are
// painted in). Two things matter:
//
//  1. Use the canvas's own bounding rect, not e.offsetX / e.offsetY.
//     offsetX/Y are measured relative to whatever node the event fired on,
//     and some browsers round them onto the device-pixel grid.
//
//  2. Scale the visual hit position into the drawing space by the ratio of
//     clientWidth (drawing width) to rect.width (on-screen width). When the
//     VIEWING layer applies any uniform scale — device-pixel-ratio, browser
//     zoom, or a CSS transform/zoom on an ancestor (which is how an embedded
//     webview such as VS Code's forwarded-port Simple Browser renders the
//     page) — rect.width drifts away from clientWidth, and a click read in the
//     visual box but mapped against clientWidth lands off by a factor that
//     GROWS across the canvas: the "selected point sits to the right of the
//     cursor" offset. The ratio cancels that exactly. With no scaling
//     rect.width === clientWidth, so the ratio is 1 and this is just
//     clientX − rect.left.
function evXY(e) {
  const r = bgCv.getBoundingClientRect();
  const sx = r.width  ? bgCv.clientWidth  / r.width  : 1;
  const sy = r.height ? bgCv.clientHeight / r.height : 1;
  return { x: (e.clientX - r.left) * sx, y: (e.clientY - r.top) * sy };
}

// Listen on the bg canvas (overlay has pointer-events: none).
bgCv.addEventListener("wheel", (e) => {
  e.preventDefault();
  if (e.shiftKey) {
    // While Shift is held most browsers remap the vertical wheel onto
    // deltaX ("horizontal scroll" convention), zeroing deltaY. Fold both
    // axes so Shift+wheel always scrolls the lane list vertically.
    setScrollY(getScrollY() + (e.deltaY || e.deltaX));
    scheduleDrawBg();
  } else {
    // Wheel zoom anchors at the cursor position. Keyboard W/S still
    // anchors at the view center — see handlers below.
    zoomAt(evXY(e).x, Math.exp(e.deltaY * 0.0015));
    scheduleDrawBg(); scheduleRefetch();
  }
}, { passive: false });

bgCv.addEventListener("mousedown", (e) => {
  stageEl.focus();
  if (S.allSignals.length === 0) return;
  const p = evXY(e);
  if (e.button === 1) {
    // Middle button = pan. Suppress the auto-scroll bubble that some
    // browsers show on middle-click.
    e.preventDefault();
    dragMode = "pan";
    dragStart = { x: p.x, view: { ...S.view } };
    return;
  }
  if (e.button === 0) {
    // Time-anchor interactions take priority over selection:
    //  - click on an existing head → pick it (Delete then removes it).
    //  - Shift + click on empty space → drop a new anchor at that time,
    //    pre-selected so it can be deleted right away.
    const hitAnchor = anchorAtPoint(p.x, p.y);
    if (hitAnchor) {
      S.selectedAnchor = hitAnchor;
      renderEventPanel(); scheduleDrawOv();
      return;
    }
    if (e.shiftKey && !(e.metaKey || e.ctrlKey)) {
      // Plain Shift + click drops a time anchor. Cmd/Ctrl + Shift + click
      // falls through to the select path instead — mouseup pins the clicked
      // block AND snaps the time-range band to its extent.
      const a = { t: Math.round(xToT(p.x)) };
      S.anchors.push(a);
      S.selectedAnchor = a;
      renderEventPanel(); scheduleDrawOv();
      return;
    }
    // Plain click on empty space deselects any picked anchor, then behaves
    // as before. Left button = selection drag (works anywhere — ruler or
    // chart). If the user doesn't actually drag we treat it as a block click
    // in mouseup. Don't pre-create selPending here — it would visually wipe
    // out the existing selection bands the moment the user clicks. We
    // create it lazily once movement crosses the drag threshold.
    if (S.selectedAnchor) { S.selectedAnchor = null; renderEventPanel(); scheduleDrawOv(); }
    dragMode = "select";
    mouseDownInfo = { x: p.x, y: p.y, t: performance.now(),
                      moved: false, frame: e.shiftKey && (e.metaKey || e.ctrlKey) };
    return;
  }
});

// Block the browser's middle-click autoscroll widget.
bgCv.addEventListener("auxclick", (e) => { if (e.button === 1) e.preventDefault(); });

bgCv.addEventListener("mousemove", (e) => {
  // Safety net: if a previous mousedown's mouseup happened outside the
  // canvas and we missed it (only the canvas had a mouseup listener for
  // a long time, see history), dragMode would stay "live" and the next
  // unrelated mousemove would resurrect a phantom drag/pan. `e.buttons`
  // is a bitmask of currently-held buttons; if none of the buttons we
  // care about are still down, reset the drag state machine before
  // doing anything else.
  if (dragMode && (e.buttons & 0b101) === 0) {
    dragMode = null; dragStart = null; mouseDownInfo = null; S.selPending = null;
    scheduleDrawSel();
  }
  const p = evXY(e);
  // Only update crosshair if x truly changed (avoid spurious overlay
  // redraws when the OS coalesces same-pixel mousemove events).
  const newX = p.x;
  if (newX !== S.cursorX) {
    S.cursorX = newX; S.cursorY = p.y;
    scheduleDrawOv();
  }
  if (dragMode === "pan" && dragStart) {
    const dx = p.x - dragStart.x;
    if (mouseDownInfo && Math.abs(dx) > DRAG_THRESHOLD_PX) mouseDownInfo.moved = true;
    const span = dragStart.view.t1 - dragStart.view.t0;
    const dt = -dx / chartWidth() * span;
    S.view = { t0: dragStart.view.t0 + dt, t1: dragStart.view.t1 + dt };
    scheduleDrawBg();
    scheduleRefetch();
  } else if (dragMode === "select" && mouseDownInfo) {
    // Promote to a real drag once *both* thresholds clear: enough pixel
    // travel AND enough press duration. A fast flick that crosses the
    // distance line in <180ms is still a click — the band only appears
    // once the user has visibly committed to holding the button.
    const dx = Math.abs(p.x - mouseDownInfo.x);
    const dt = performance.now() - mouseDownInfo.t;
    if (!mouseDownInfo.moved && dx > DRAG_THRESHOLD_PX && dt > DRAG_THRESHOLD_MS) {
      mouseDownInfo.moved = true;
      S.selPending = { t0: xToT(mouseDownInfo.x), t1: xToT(p.x) };
    }
    if (S.selPending) {
      S.selPending.t1 = xToT(Math.max(0, p.x));
      scheduleDrawSel();
    }
  }
});

// Bind mouseup on window, not bgCv: a mousedown that started on the
// canvas can release anywhere (e.g. user drags off-canvas before
// letting go). If we only listened on bgCv, the off-canvas release
// would never fire and dragMode / mouseDownInfo would stay live,
// causing the next innocent mousemove to revive a phantom drag.
window.addEventListener("mouseup", async (e) => {
  if (dragMode === "select" && mouseDownInfo) {
    // cmd / ctrl held → additive: a drag adds another band, a click toggles
    // one block. Plain → single-select: replace the bands / blocks. (Shift
    // never reaches here; it drops a time anchor in mousedown.)
    const additive = e.metaKey || e.ctrlKey;
    if (mouseDownInfo.moved && S.selPending) {
      // True drag → finalize the time-range band. Persists until the next
      // plain drag-select; single clicks below won't clear it.
      const a = Math.min(S.selPending.t0, S.selPending.t1);
      const b = Math.max(S.selPending.t0, S.selPending.t1);
      S.selPending = null;
      if (b - a >= 1) {
        const band = { t0: Math.round(a), t1: Math.round(b) };
        if (additive) {
          S.selections.push(band);   // add another segment, keep the rest
        } else {
          S.selections = [band];     // replace, and drop clicked blocks
          S.events = [];
        }
      }
      renderEventPanel();
      scheduleDrawSel();
    } else {
      // Single click (no movement). Two flavours depending on what the
      // click landed on:
      //  - solid bucket (raw event or homogeneous aggregate) → pin the
      //    discrete (t, v) as a lit block in S.events (plain click replaces
      //    the set, cmd / ctrl-click toggles this one), lift it with the
      //    outline.
      //  - mixed bucket (striped, multiple values aggregated) → pin it too;
      //    there is no single (t, v), but the bucket knows its composition,
      //    so the panel lists the values inside with their time shares.
      //  - click missed a signal row → just clear any active range.
      S.selPending = null;
      const hadSelection = S.selections.length > 0;
      const idx = entryAtY(mouseDownInfo.y);
      let handled = false;
      if (idx >= 0 && idx < S.entries.length && S.entries[idx].type === "signal") {
        const lane = S.entries[idx].lane;
        // Pick the bucket the cursor is ACTUALLY over — use the exact click
        // time, not one snapped to the nearest whole cycle. Math.round here
        // pulled the lookup up to half a cycle toward the nearest cycle line,
        // so a click in the right half of a cycle landed on the next block:
        // the selected block sat ~half a cycle-cell to the right of the
        // pointer, a gap that grew (in pixels) the further you zoomed in.
        // t is used only to locate the bucket (its own t_start/v are used
        // afterwards), and bucket bounds are integers, so a fractional t
        // resolves the containing [t_start, t_end) exactly.
        const t = xToT(mouseDownInfo.x);
        const data = S.laneData.get(lane.id);
        let clicked = null;
        if (data) {
          for (const b of data.buckets) {
            if (b.t_start <= t && t < b.t_end) { clicked = b; break; }
          }
        }
        // Any hit bucket pins as a block: a solid one with its single
        // (t, v), a mixed aggregate with its dom composition (the panel
        // then lists the values inside with their time shares). Only a
        // blank gap has nothing to pin.
        if (mouseDownInfo.frame) {
          // Cmd/Ctrl + Shift + click on a block — pin it like a plain
          // pick AND snap the time-range band to the block's [t_start, t_end),
          // so the selection column frames exactly this block across the chart.
          // On a blank gap there's no block to frame, so the gesture is a
          // no-op that leaves the current selection untouched.
          if (clicked) {
            S.events = [eventFromBucket(lane, clicked)];
            S.selections = [{ t0: clicked.t_start, t1: clicked.t_end }];
            clearLaneSelection();
            renderEventPanel();
            scheduleDrawSel();
          }
          handled = true;
        } else if (additive) {
          // Additive — toggle the hit block in/out of the lit set. On a
          // blank gap there's nothing to add, so the click is a no-op that
          // leaves the current selection untouched.
          if (clicked) {
            const at = findEventIdx(lane.id, clicked.t_start);
            if (at >= 0) S.events.splice(at, 1);          // already lit → off
            else S.events.push(eventFromBucket(lane, clicked));
            renderEventPanel();
            scheduleDrawSel();
          }
          handled = true;
        } else if (clicked) {
          // Plain pick replaces the whole block set with just this one (or
          // toggles off if it was the only block already lit) and drops the
          // lane picks.
          S.selections = [];
          const only = S.events.length === 1 &&
                       S.events[0].lane.id === lane.id &&
                       S.events[0].t === clicked.t_start;
          S.events = only ? [] : [eventFromBucket(lane, clicked)];
          clearLaneSelection();
          renderEventPanel();
          scheduleDrawSel();
          handled = true;
        } else {
          // Click landed between buckets — nothing is drawn at that x,
          // so the click reads as "blank space": deselect. No server
          // round-trip to snap to a neighbouring event; re-highlighting
          // a block the user just clicked away from would make blank
          // clicks unable to clear the selection.
          const hadEvent = S.events.length > 0;
          S.selections = [];
          S.events = [];
          if (hadSelection || hadEvent) { renderEventPanel(); scheduleDrawSel(); }
          clearLaneSelection();   // blank x on a lane reads as a deselect too
          handled = true;
        }
      }
      if (!handled && !additive) {
        // Plain click missed every signal row (module row, area below the
        // lanes, ...) — blank space clears the range band, the clicked blocks,
        // and the left-tree lane picks. A modifier+click out here has nothing
        // to add, so it leaves the current selection in place.
        if (hadSelection || S.events.length > 0) {
          S.selections = [];
          S.events = [];
          renderEventPanel();
          scheduleDrawSel();
        }
        clearLaneSelection();
      }
    }
    // A band may have been created or cleared above; the per-signal stats
    // follow the highlighted band(s), so refresh them (keyed → a no-op when
    // nothing relevant changed).
    scheduleSelectStats();
  }
  dragMode = null; dragStart = null; mouseDownInfo = null;
});

bgCv.addEventListener("mouseleave", () => {
  S.cursorX = -1; S.cursorY = -1;
  scheduleDrawOv();
});

// ── per-lane context menu ──────────────────────────────────────────
// Right-click a lane to override its display mode (bar ↔ wave). The
// initial kind comes from /api/init heuristics (few distinct values →
// bar, unless the series is monotone i.e. counter-like → wave) — this
// lets the user flip it either way.
const ctxMenu = document.createElement("div");
ctxMenu.className = "ctx-menu";
ctxMenu.style.display = "none";
document.body.appendChild(ctxMenu);

function hideCtxMenu() {
  if (ctxMenu.style.display !== "none") {
    ctxMenu.style.display = "none";
    ctxMenu.innerHTML = "";
  }
}

function showLaneCtxMenu(clientX, clientY, lane) {
  ctxMenu.innerHTML = "";
  const header = document.createElement("div");
  header.className = "ctx-menu-header";
  header.textContent = lane.name || lane.shortName || "signal";
  ctxMenu.appendChild(header);
  const opts = [
    { kind: "bar",  label: "Display as blocks" },
    { kind: "wave", label: "Display as waveform" },
  ];
  for (const o of opts) {
    const item = document.createElement("div");
    item.className = "ctx-menu-item" + (lane.kind === o.kind ? " active" : "");
    const check = document.createElement("span");
    check.className = "check";
    check.textContent = lane.kind === o.kind ? "✓" : "";
    const label = document.createElement("span");
    label.textContent = o.label;
    item.appendChild(check); item.appendChild(label);
    item.addEventListener("click", () => {
      if (lane.kind !== o.kind) {
        lane.kind = o.kind;
        scheduleDrawBg();
      }
      hideCtxMenu();
    });
    ctxMenu.appendChild(item);
  }
  // Show first, then measure, then clamp into the viewport.
  ctxMenu.style.left = "0px"; ctxMenu.style.top = "0px";
  ctxMenu.style.display = "block";
  const r = ctxMenu.getBoundingClientRect();
  const x = Math.min(clientX, window.innerWidth  - r.width  - 4);
  const y = Math.min(clientY, window.innerHeight - r.height - 4);
  ctxMenu.style.left = Math.max(0, x) + "px";
  ctxMenu.style.top  = Math.max(0, y) + "px";
}

bgCv.addEventListener("contextmenu", (e) => {
  if (S.allSignals.length === 0) return;
  const idx = entryAtY(evXY(e).y);
  if (idx < 0 || idx >= S.entries.length) return;
  const ent = S.entries[idx];
  if (ent.type !== "signal") return;
  e.preventDefault();
  showLaneCtxMenu(e.clientX, e.clientY, ent.lane);
});

// Dismiss on outside click, Escape, or any scroll.
window.addEventListener("mousedown", (e) => {
  if (ctxMenu.style.display === "none") return;
  if (!ctxMenu.contains(e.target)) hideCtxMenu();
});
window.addEventListener("keydown", (e) => {
  if (e.key === "Escape") hideCtxMenu();
});
window.addEventListener("scroll", hideCtxMenu, true);

// Sidebar scroll → only needs to repaint lane backgrounds; the browser
// renders the native scrollbar.
sidebarEl.addEventListener("scroll", () => { scheduleDrawBg(); scheduleRefetch(); });

// ── WASD + H ─────────────────────────────────────────────────────────

const handlers = {
  a: () => { const span = S.view.t1 - S.view.t0; const accel = S.keyDown.has("shift") ? 4 : 1; panBy(-span * 0.06 * accel); scheduleDrawBg(); scheduleRefetch(); },
  d: () => { const span = S.view.t1 - S.view.t0; const accel = S.keyDown.has("shift") ? 4 : 1; panBy(+span * 0.06 * accel); scheduleDrawBg(); scheduleRefetch(); },
  w: () => { zoomKeyboard(0.92);     scheduleDrawBg(); scheduleRefetch(); },
  s: () => { zoomKeyboard(1 / 0.92); scheduleDrawBg(); scheduleRefetch(); },
};
// W/S anchor at the mouse position when it's over the chart (same feel
// as wheel zoom); fall back to the view center when it isn't.
function zoomKeyboard(factor) {
  if (S.cursorX >= 0) zoomAt(S.cursorX, factor);
  else zoomViewCenter(factor);
}
function keyLoopStart(h) { if (h._timer) return; h(); h._timer = setInterval(h, 60); }
function keyLoopStop(h)  { if (h._timer) { clearInterval(h._timer); h._timer = null; } }

window.addEventListener("keydown", (e) => {
  const k = e.key.toLowerCase();
  if (e.shiftKey) {
    if (!S.keyDown.has("shift")) scheduleDrawOv();  // reveal the crosshair
    S.keyDown.add("shift");
  }
  if (k in handlers) { e.preventDefault(); S.keyDown.add(k); keyLoopStart(handlers[k]); return; }
  if (k === "f") { S.view = { ...S.bounds }; scheduleDrawBg(); scheduleRefetch(); }
  else if (k === "+" || k === "=") { zoomViewCenter(0.7); scheduleDrawBg(); scheduleRefetch(); }
  else if (k === "-") { zoomViewCenter(1.4); scheduleDrawBg(); scheduleRefetch(); }
  else if (k === "h") { S.hex = !S.hex; scheduleDrawBg(); renderEventPanel(); }
  else if ((k === "delete" || k === "backspace") && S.selectedAnchor) {
    e.preventDefault();
    const i = S.anchors.indexOf(S.selectedAnchor);
    if (i >= 0) S.anchors.splice(i, 1);
    S.selectedAnchor = null;
    renderEventPanel(); scheduleDrawOv();
  }
  else if (k === "escape") {
    S.selectedLanes.clear(); S.events = []; S.selections = [];
    S.selectedAnchor = null;
    renderTree(); renderEventPanel(); scheduleDrawSel(); scheduleDrawOv();
    scheduleSelectStats();   // selection cleared → drop the stats section
  }
});
window.addEventListener("keyup", (e) => {
  const k = e.key.toLowerCase();
  if (k === "shift") { S.keyDown.delete("shift"); scheduleDrawOv(); }  // hide the crosshair
  if (k in handlers) { S.keyDown.delete(k); keyLoopStop(handlers[k]); }
});
window.addEventListener("blur", () => {
  for (const k of Object.keys(handlers)) keyLoopStop(handlers[k]);
  S.keyDown.clear();
  scheduleDrawOv();   // drop the crosshair if focus is lost while Shift was held
});

// ── property box (floating, bottom-right) ───────────────────────────

const COPY_SVG =
  '<svg viewBox="0 0 16 16" width="16" height="16" fill="none" ' +
  'stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">' +
  '<rect x="5.5" y="5.5" width="8" height="8" rx="1.5"/>' +
  '<path d="M3.4 10.5h-.9A1.5 1.5 0 0 1 1 9V2.5A1.5 1.5 0 0 1 2.5 1H9a1.5 1.5 0 0 1 1.5 1.5v.9"/>' +
  '</svg>';
const CHECK_SVG =
  '<svg viewBox="0 0 16 16" width="16" height="16" fill="none" ' +
  'stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">' +
  '<path d="M3 8.5l3.5 3.5L13 4.5"/></svg>';

// Plain-text snapshot of what's currently in the box; the copy button
// writes exactly this. Rebuilt every render from the same section data.
let lastPropText = "";

// Clipboard write with a fallback: navigator.clipboard needs a secure
// context (it is granted on http://localhost, but not on a bare LAN IP),
// so fall back to a throwaway <textarea> + execCommand when it rejects.
async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch (err) {
    const ta = document.createElement("textarea");
    ta.value = text;
    ta.style.position = "fixed"; ta.style.top = "-1000px"; ta.style.opacity = "0";
    document.body.appendChild(ta);
    ta.select();
    let ok = false;
    try { ok = document.execCommand("copy"); } catch (e) { ok = false; }
    document.body.removeChild(ta);
    return ok;
  }
}

let copyFlashTimer = null;
function flashCopyButton(btn, ok) {
  btn.classList.toggle("copied", ok);
  btn.classList.toggle("failed", !ok);
  btn.innerHTML = ok ? CHECK_SVG : COPY_SVG;
  clearTimeout(copyFlashTimer);
  copyFlashTimer = setTimeout(() => {
    btn.classList.remove("copied", "failed");
    btn.innerHTML = COPY_SVG;
  }, 1100);
}

// Fill the property box from whatever is selected and show it; when
// nothing is selected, hide it entirely. A clicked block, a time-range
// drag, a picked anchor, and picked signals each contribute a section
// (they can stack). Each section is { title, tv?, rows: [[k, v, opts?], ...] }
// — one source feeds both the HTML and the copy-all plain text. `opts` is
// an optional { copy, cyc } bag: copy === false excludes a row from the
// copy-all text (e.g. the keyboard hint — still rendered); cyc === true
// marks a cycle-domain value (a raw cycle count, same axis as the ruler) —
// rendered in the v-cyc style (see style.css) instead of a "(cyc)" text
// suffix, so the unit reads from typography, not from a repeated label.
// tv is the section's time extent ("dur : start -> end" for a block /
// range, a lone t for an event / anchor), rendered on the title line
// itself (right-aligned, v-cyc typography) instead of as an own row —
// the title already says what the value is, so a separate row would
// just spend a line repeating it.
function renderEventPanel() {
  const sections = [];

  for (const ev of S.events) {
    // A block carries a duration → its extent "dur : start -> end" goes on
    // the title line (duration first — it's usually what the reader is
    // after); a durationless event puts just its t there.
    const tv = ev.dur != null
      ? `${fmtCyc(ev.dur)} : ${fmtCyc(ev.t)} -> ${fmtCyc(ev.t + ev.dur)}`
      : fmtCyc(ev.t);
    const rows = [];
    if (ev.mix) {
      // Mixed aggregate: no single value — list the block's composition,
      // one row per contained value, keyed by its share of the block's
      // time (weight-ranked, as shipped in the bucket's dom pairs).
      for (const d of ev.mix) {
        const pct = (d.w / DOM_W_ONE * 100).toFixed(1).replace(/\.0$/, "");
        rows.push([`${pct}%`, fmtV(d.v, ev.lane)]);
      }
      // The wire caps dom at 8 pairs; own up to the cut-off share so the
      // list never silently reads as the whole composition.
      const restW = DOM_W_ONE - ev.mix.reduce((a, d) => a + d.w, 0);
      if (ev.mix.length === DOM_K && restW > 0) {
        const pct = (restW / DOM_W_ONE * 100).toFixed(1).replace(/\.0$/, "");
        rows.push([`${pct}%`, "…other values"]);
      }
      rows.push(["events", String(ev.count)]);
    } else {
      rows.push(["value", fmtV(ev.v, ev.lane)]);
    }
    sections.push({ title: ev.lane.name, tv, rows });
  }

  S.selections.forEach((s, i) => {
    const title = S.selections.length > 1 ? `time range ${i + 1}` : "time range";
    // Same "dur : start -> end" shape as a block event's title line above —
    // a range is exactly that triple.
    sections.push({ title,
      tv: `${fmtCyc(s.t1 - s.t0)} : ${fmtCyc(s.t0)} -> ${fmtCyc(s.t1)}`,
      rows: [] });
  });

  if (S.selectedAnchor) {
    sections.push({ title: "time anchor", tv: fmtCyc(S.selectedAnchor.t), rows: [
      ["delete", "Del / Backspace", { copy: false }],  // keyboard hint — shown, not copied
    ] });
  }

  if (S.selectedLanes.size > 0) {
    const st = S.selStats;
    if (st && st.bySig) {
      // A highlight band is active → one stats group per selected signal,
      // each aggregated over the band(s) by refreshSelectStats. (With no
      // band the else-branch shows just the count — see scheduleSelectStats.)
      const ids = [...S.selectedLanes].sort((a, b) => a - b);
      for (const id of ids) {
        const lane = S.laneById.get(id);
        // Same text the left tree shows for this lane (the path leaf).
        const name = lane ? splitPath(lane.name).leaf : `sig ${id}`;
        const s = st.bySig.get(id);
        if (!s || !(s.total_cycles > 0)) {
          sections.push({ title: name, rows: [["", "no data in range"]] });
          continue;
        }
        const pct = (s.zero_ratio * 100).toFixed(1).replace(/\.0$/, "");
        const nzPct = (s.nonzero_ratio * 100).toFixed(1).replace(/\.0$/, "");
        sections.push({ title: name, rows: [
          ["total", fmtStatNum(s.total_cycles), { cyc: true }],
          // avg/cyc is a rate (signal units per cycle), not itself a cycle
          // count — keep the explicit "/ cyc" unit, no v-cyc styling.
          ["avg / cyc", fmtStatNum(s.avg_per_cycle)],
          ["zero", `${fmtStatNum(s.zero_cycles)} (${pct}%)`, { cyc: true }],
          ["non-zero", `${fmtStatNum(s.nonzero_cycles)} (${nzPct}%)`, { cyc: true }],
        ] });
      }
    } else {
      sections.push({ title: "signals",
                      rows: [["selected", String(S.selectedLanes.size)]] });
    }
  }

  if (sections.length === 0) {
    propboxEl.hidden = true;
    propboxEl.innerHTML = "";
    lastPropText = "";
    return;
  }

  const body = sections.map((s) => {
    const head = `<div class="pb-title"><span>${escapeHtml(s.title)}</span>` +
      (s.tv != null ? `<span class="tv">${escapeHtml(s.tv)}</span>` : "") +
      `</div>`;
    const kv = s.rows.length === 0 ? "" : `<div class="kv">` +
      s.rows.map(([k, v, o]) => {
        const vSpan = `<span class="v${o && o.cyc ? " v-cyc" : ""}${k ? "" : " v-solo"}">${escapeHtml(String(v))}</span>`;
        return k ? `<span class="k">${escapeHtml(k)}</span>${vSpan}` : vSpan;
      }).join("") + `</div>`;
    return `<div class="pb-sec">${head}${kv}</div>`;
  }).join("");
  lastPropText = sections.map((s) =>
    [s.title + (s.tv != null ? `  ${s.tv}` : ""),
     ...s.rows.filter((r) => !(r[2] && r[2].copy === false))
       .map(([k, v]) => (k ? `${k}: ${v}` : String(v)))].join("\n")
  ).join("\n\n");

  propboxEl.innerHTML =
    `<button class="pb-copy" type="button" title="copy all">${COPY_SVG}</button>` + body;
  propboxEl.hidden = false;
}

// The box is interactive (mouse-selectable text); a click anywhere in it
// bubbles up here, and only a hit on the copy button actually does anything.
propboxEl.addEventListener("click", async (e) => {
  const btn = e.target.closest(".pb-copy");
  if (!btn) return;
  flashCopyButton(btn, await copyText(lastPropText));
});

// ── sidebar width resize ────────────────────────────────────────────

const sbResizeEl = $("#sb-resize");
let sbWidthDrag = null;
sbResizeEl.addEventListener("mousedown", (e) => {
  e.preventDefault();
  const sbRect = sidebarEl.getBoundingClientRect();
  sbWidthDrag = { startX: e.clientX, startW: sbRect.width };
  sbResizeEl.classList.add("dragging");
  document.body.style.cursor = "ew-resize";
});
window.addEventListener("mousemove", (e) => {
  if (!sbWidthDrag) return;
  const newW = Math.max(160, Math.min(500, sbWidthDrag.startW + (e.clientX - sbWidthDrag.startX)));
  document.body.style.setProperty("--sidebar-w", `${newW}px`);
  scheduleDrawBg();
});
window.addEventListener("mouseup", () => {
  if (!sbWidthDrag) return;
  sbWidthDrag = null;
  sbResizeEl.classList.remove("dragging");
  document.body.style.cursor = "";
});

// ── boot ─────────────────────────────────────────────────────────────

// Self-updating page: poll the served app.js's Last-Modified and reload
// when it changes. An open tab otherwise keeps running whatever build it
// loaded — every "the fix changed nothing" round in this code's history
// traces back to a tab silently lagging behind the code on disk.
(function watchFrontendBuild() {
  let last = null;
  setInterval(async () => {
    try {
      const r = await fetch(`/static/app.js?v=${APP_BUILD}`,
                            { method: "HEAD", cache: "no-store" });
      const lm = r.headers.get("last-modified");
      if (last && lm && lm !== last) location.reload();
      if (lm) last = lm;
    } catch (e) { /* server briefly down (restart) — retry next tick */ }
  }, 4000);
})();

(async function main() {
  ensureCanvasSize(bgCv, bgCtx);
  ensureCanvasSize(selCv, selCtx);
  ensureCanvasSize(ovCv, ovCtx);

  const init = await jget("/api/init");
  if (init.wire_format !== WIRE_FORMAT) {
    showVersionBanner(init.wire_format);
    return;
  }
  S.tree = init.tree;
  S.treeById = new Map(S.tree.map((r) => [r.id, r]));
  buildChildrenIndex();

  // Clock period (one cycle = this many ticks); drives the cycle-labelled
  // ruler, the cycle grid, and the zoom floor.
  if (init.cycle_time > 0) S.cycleTime = init.cycle_time;
  S.cyclePeriod = S.cycleTime;

  await buildAllLanes(init.signals);
  rebuildEntries();
  renderTree();
  refreshToggleAll();
  refreshCollapseAll();

  await fetchAllVisible();
  renderEventPanel();
  drawBg(); drawSel(); drawOv();
})();
