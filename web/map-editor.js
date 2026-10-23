// The playground's map editor. It reads a map table from the code (MAP, or any table of rows such
// as LEVEL1, a string a row, a character a cell), lets you paint it with the cart's sprites, and
// writes it back after each stroke. map() draws it; mget() and mset() read and change its cells.
// Uses the playground's globals: $, code, lw, say, typing; and Tables (web/tables.js).
"use strict";

const MapEditor = (() => {
  const { readTable, withApi } = Tables;
  const TILES = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ+/";  // sprites 0-63; "." is empty
  const MAX_W = 128, MAX_H = 64, NEW_W = 32, NEW_H = 16;
  const INTRO = "the map: a character a cell, \".\" empty, else a sprite (the playground's map editor writes this table)";
  // NAME = { then a row a line, "....", then } at the start of a line.
  const TABLE = /^([A-Za-z_]\w*)[ \t]*=[ \t]*\{[ \t]*\n((?:[ \t]*"[.0-9a-zA-Z+/]*",?[ \t]*(?:--[^\n]*)?\n)*)\}/gm;

  let maps = [];      // [{name, rows}] found in the code
  let name = null;    // the one being edited
  let rows = [];      // its rows: strings of cells, all the same width
  let tile = 1, tool = "paint", zoom = 0;
  let sprites = new Map(), colors = [];
  let past = [], future = [], written = null;

  // ---- the code ---------------------------------------------------------------------------------
  function find(src) {
    const out = [];
    for (const m of src.matchAll(TABLE)) {
      const r = [...m[2].matchAll(/"([.0-9a-zA-Z+/]*)"/g)].map((x) => x[1]);
      out.push({ name: m[1], rows: r, index: m.index, length: m[0].length });
    }
    // MAP first: it's the one map() draws
    return out.sort((a, b) => (b.name === "MAP") - (a.name === "MAP"));
  }
  const width = (r) => Math.max(1, ...r.map((s) => s.length));
  const square = (r) => { const w = width(r); return r.map((s) => s.padEnd(w, ".")); };

  function load() {
    maps = find(code.value);
    if (!maps.some((m) => m.name === name)) name = maps.length ? maps[0].name : null;
    rows = name ? square(maps.find((m) => m.name === name).rows) : [];
    if (code.value !== written) {
      past = [];
      future = [];
      written = code.value;
    }
    colors = lw ? lw.colors() : [];
    const t = readTable(code.value, "SPRITES");
    sprites = new Map();
    for (const [n, s] of t.map || []) {
      const d = s.text.replace(/\s+/g, "");
      if (/^[0-9a-fA-F]{64}$/.test(d) && n >= 0 && n < 64) sprites.set(n, { px: [...d].map((h) => parseInt(h, 16)), name: s.name });
    }
    if (!sprites.has(tile) && sprites.size) tile = [...sprites.keys()].sort((a, b) => a - b).find((n) => n > 0) ?? [...sprites.keys()][0];
  }
  const block = (n, r) => `${n} = {\n${r.map((s) => `  "${s}",`).join("\n")}\n}`;
  function save() {
    const src = code.value, m = find(src).find((x) => x.name === name);
    if (!m) return;
    let out = src.slice(0, m.index) + block(name, rows) + src.slice(m.index + m.length);
    out = withApi(out, 5);  // maps need API 5
    written = out;
    if (out === src) return;
    code.value = out;
    code.dispatchEvent(new Event("input"));
  }
  // A new MAP table, after SPRITES (or the header), and the api it needs.
  function create() {
    const src = code.value, r = Array.from({ length: NEW_H }, () => ".".repeat(NEW_W));
    const text = `-- ${INTRO}\n${block("MAP", r)}\n`;
    const sp = /^SPRITES\s*=\s*\{[^]*?^\}\n?/m.exec(src);
    let out;
    if (sp) out = src.slice(0, sp.index + sp[0].length) + text + src.slice(sp.index + sp[0].length);
    else {
      const lines = src.split("\n");
      let at = 0;
      while (at < lines.length && lines[at].startsWith("--")) at++;
      lines.splice(at, 0, "", text.trimEnd());
      out = lines.join("\n");
    }
    code.value = withApi(out, 5);
    code.dispatchEvent(new Event("input"));
    name = "MAP";
    show();
    say("A 32 x 16 map is in the code as MAP. Draw it here; map() draws it in the cart.");
  }

  // ---- undo ---------------------------------------------------------------------------------------
  const snapshot = () => JSON.stringify({ name, rows });
  function remember() {
    past.push(snapshot());
    if (past.length > 100) past.shift();
    future = [];
    buttons();
  }
  function restore(state) {
    const d = JSON.parse(state);
    name = d.name;
    rows = d.rows;
    save();
    show();
  }
  const undo = () => { if (past.length) { future.push(snapshot()); restore(past.pop()); } };
  const redo = () => { if (future.length) { past.push(snapshot()); restore(future.pop()); } };

  // ---- drawing ----------------------------------------------------------------------------------
  const cell = () => zoom || (matchMedia("(max-width: 600px)").matches ? 16 : 24);
  const at = (x, y) => { const ch = rows[y]?.[x]; return ch && ch !== "." ? TILES.indexOf(ch) : -1; };
  function drawTile(g, n, x, y, size) {  // sprite n, `size` pixels a side
    const s = sprites.get(n), p = size / 8;
    if (!s) {  // a cell naming a sprite SPRITES doesn't have: shown, so it can be fixed
      g.fillStyle = "#ff7b72";
      g.fillRect(x + 2, y + 2, size - 4, size - 4);
      return;
    }
    for (let i = 0; i < 64; i++) {
      const v = s.px[i];
      if (!v) continue;
      g.fillStyle = colors[v] || "#fff";
      g.fillRect(x + (i % 8) * p, y + Math.floor(i / 8) * p, Math.ceil(p), Math.ceil(p));
    }
  }
  function draw() {
    const cv = $("map-canvas"), c = cell(), w = width(rows), h = rows.length;
    cv.width = w * c;
    cv.height = h * c;
    const g = cv.getContext("2d");
    g.fillStyle = colors[0] && colors[0] !== "#000000" ? colors[0] : "#0b0e13";
    g.fillRect(0, 0, cv.width, cv.height);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) drawCell(g, x, y);
    lines(g);
  }
  function drawCell(g, x, y) {
    const c = cell();
    g.fillStyle = colors[0] && colors[0] !== "#000000" ? colors[0] : "#0b0e13";
    g.fillRect(x * c, y * c, c, c);
    const n = at(x, y);
    if (n >= 0) drawTile(g, n, x * c, y * c, c);
  }
  function lines(g) {
    const c = cell(), w = width(rows), h = rows.length;
    g.strokeStyle = "rgba(255,255,255,0.07)";
    g.lineWidth = 1;
    g.beginPath();
    for (let x = 1; x < w; x++) { g.moveTo(x * c + 0.5, 0); g.lineTo(x * c + 0.5, h * c); }
    for (let y = 1; y < h; y++) { g.moveTo(0, y * c + 0.5); g.lineTo(w * c, y * c + 0.5); }
    g.stroke();
    // a screen's worth (16 x 16 cells, the 128 x 128 picture) from the top left, and every screen after
    g.strokeStyle = "rgba(240,180,41,0.55)";
    g.setLineDash([4, 4]);
    g.beginPath();
    for (let x = 16; x < w; x += 16) { g.moveTo(x * c + 0.5, 0); g.lineTo(x * c + 0.5, h * c); }
    for (let y = 16; y < h; y += 16) { g.moveTo(0, y * c + 0.5); g.lineTo(w * c, y * c + 0.5); }
    g.stroke();
    g.setLineDash([]);
  }

  function cellAt(e) {
    const r = $("map-canvas").getBoundingClientRect(), c = cell();
    const x = Math.floor((e.clientX - r.left) / (r.width / ($("map-canvas").width / c)));
    const y = Math.floor((e.clientY - r.top) / (r.height / ($("map-canvas").height / c)));
    return x >= 0 && y >= 0 && x < width(rows) && y < rows.length ? [x, y] : null;
  }
  function set(x, y, ch) {
    if (rows[y][x] === ch) return false;
    rows[y] = rows[y].slice(0, x) + ch + rows[y].slice(x + 1);
    return true;
  }
  function fill(x, y, ch) {  // the area of one kind of cell around (x, y), 4 ways
    const from = rows[y][x];
    if (from === ch) return;
    const stack = [[x, y]], w = width(rows), h = rows.length;
    while (stack.length) {
      const [i, j] = stack.pop();
      if (i < 0 || j < 0 || i >= w || j >= h || rows[j][i] !== from) continue;
      set(i, j, ch);
      stack.push([i - 1, j], [i + 1, j], [i, j - 1], [i, j + 1]);
    }
  }
  function info(x, y) {
    const n = at(x, y), s = sprites.get(n);
    $("map-info").textContent = `cell ${x}, ${y} (pixel ${x * 8}, ${y * 8}): ` +
      (n < 0 ? "empty" : `sprite ${n}${s?.name ? " " + s.name : ""}${s ? "" : ", which SPRITES doesn't have"}`);
  }
  let stroke = null;  // a drag: changed cells are drawn as they go, the code written when it ends
  function paint(e, first) {
    const p = cellAt(e);
    if (!p) return;
    const [x, y] = p;
    info(x, y);
    if (e.altKey) {  // pick a cell's sprite up
      if (first) { const n = at(x, y); if (n >= 0) { tile = n; palette(); } }
      return;
    }
    const erase = tool === "erase" || e.button === 2 || (e.buttons & 2);
    const ch = erase ? "." : TILES[tile];
    if (tool === "fill" && !erase) {
      if (first) { fill(x, y, ch); draw(); stroke.changed = true; }
      return;
    }
    if (set(x, y, ch)) {
      const g = $("map-canvas").getContext("2d");
      drawCell(g, x, y);
      lines(g);
      stroke.changed = true;
    }
  }

  // ---- the tab ------------------------------------------------------------------------------------
  function palette() {
    const list = [...sprites.keys()].sort((a, b) => a - b);
    $("map-tiles").replaceChildren(...list.map((n) => {
      const b = document.createElement("button");
      b.className = "tile" + (n === tile && tool !== "erase" ? " on" : "");
      b.title = `sprite ${n}${sprites.get(n).name ? ": " + sprites.get(n).name : ""} (its character: ${TILES[n]})`;
      const c = document.createElement("canvas");
      c.width = c.height = 24;
      drawTile(c.getContext("2d"), n, 0, 0, 24);
      b.append(c);
      b.onclick = () => { tile = n; if (tool === "erase") tool = "paint"; palette(); buttons(); };
      return b;
    }));
    $("map-nosprites").hidden = list.length > 0;
  }
  function buttons() {
    for (const t of ["paint", "fill", "erase", "move"]) $("map-" + t).classList.toggle("on", tool === t);
    $("map-canvas").classList.toggle("moving", tool === "move");
    $("map-undo").disabled = !past.length;
    $("map-redo").disabled = !future.length;
  }
  function sizes() {
    $("map-w").value = width(rows);
    $("map-h").value = rows.length;
    const bytes = rows.reduce((s, r) => s + r.length + 5, 0);
    $("map-size").textContent = `${width(rows) * 8} x ${rows.length * 8} pixels, about ${bytes.toLocaleString()} bytes of the cart's 32 KB`;
  }
  function show() {
    load();
    $("map-empty").hidden = !!name;
    $("map-body").hidden = !name;
    if (!name) return;
    const sel = $("map-which");
    sel.replaceChildren(...maps.map((m) => new Option(m.name, m.name)));
    sel.value = name;
    $("map-which-label").hidden = maps.length < 2;
    $("map-call").textContent = name === "MAP" ? "map()" : `MAP = ${name}  then  map()`;
    palette();
    buttons();
    sizes();
    draw();
  }
  function resize() {
    const w = Math.max(1, Math.min(MAX_W, Math.floor(Number($("map-w").value)) || 1));
    const h = Math.max(1, Math.min(MAX_H, Math.floor(Number($("map-h").value)) || 1));
    if (w === width(rows) && h === rows.length) { sizes(); return; }
    remember();
    rows = Array.from({ length: h }, (_, y) => (rows[y] || "").padEnd(w, ".").slice(0, w));
    save();
    show();
  }

  function setUp() {
    const cv = $("map-canvas");
    cv.addEventListener("contextmenu", (e) => e.preventDefault());
    cv.addEventListener("pointerdown", (e) => {
      if (tool === "move") return;  // the canvas scrolls under the finger instead
      cv.setPointerCapture(e.pointerId);
      e.preventDefault();
      if (!e.altKey) remember();  // a stroke is one undo step
      stroke = { changed: false };
      paint(e, true);
    });
    cv.addEventListener("pointermove", (e) => {
      if (stroke && tool !== "fill") paint(e, false);
      else if (!stroke) { const p = cellAt(e); if (p) info(...p); }
    });
    const end = () => {
      if (!stroke) return;
      if (stroke.changed) save();
      else past.pop();  // nothing changed: no undo step
      stroke = null;
      buttons();
    };
    cv.addEventListener("pointerup", end);
    cv.addEventListener("pointercancel", end);
    for (const t of ["paint", "fill", "erase", "move"]) $("map-" + t).onclick = () => { tool = t; buttons(); palette(); };
    $("map-undo").onclick = undo;
    $("map-redo").onclick = redo;
    $("map-new").onclick = create;
    $("map-which").onchange = () => { name = $("map-which").value; show(); };
    $("map-w").onchange = resize;
    $("map-h").onchange = resize;
    $("map-zoom").oninput = () => { zoom = Number($("map-zoom").value); draw(); };
    addEventListener("keydown", (e) => {  // on this tab: undo, redo; P paint, F fill, E erase
      if (typeof SoundEditor === "undefined" || SoundEditor.tab !== "map" || typing(e)) return;
      const mod = e.ctrlKey || e.metaKey, key = e.key.toLowerCase();
      if (mod && key === "z") { e.preventDefault(); e.shiftKey ? redo() : undo(); }
      else if (mod && key === "y") { e.preventDefault(); redo(); }
      else if (!mod && key === "p") $("map-paint").click();
      else if (!mod && key === "f") $("map-fill").click();
      else if (!mod && key === "e") $("map-erase").click();
    });
  }

  return { setUp, show };
})();

MapEditor.setUp();
