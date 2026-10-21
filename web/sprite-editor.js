// The playground's sprite editor. It reads the cart's SPRITES table from the code, lets you draw
// its 8x8 sprites in the cart's 16 colors, and writes the table back as you go, a sprite a line,
// its rows spaced so the code reads as the picture (API 4). spr(n, x, y) draws sprite n.
// Uses the playground's globals: $, code, lw, say, typing; and Tables (web/tables.js).
"use strict";

const SpriteEditor = (() => {
  const { readTable, writeTable, withApi } = Tables;
  const COUNT = 64;  // sprites 0-63: the ones the engine keeps decoded
  const INTRO = "sprites (the playground's sprite editor writes this table)";

  // 64 color numbers <-> the table's text: 8 rows of 8 hex digits, spaces between rows.
  const decode = (text) => {
    const d = text.replace(/\s+/g, "");
    return /^[0-9a-fA-F]{64}$/.test(d) ? [...d].map((h) => parseInt(h, 16)) : null;
  };
  const encode = (px) => Array.from({ length: 8 }, (_, r) => px.slice(r * 8, r * 8 + 8).map((v) => v.toString(16)).join("")).join(" ");

  let sprites = new Map();  // number -> {text, name}
  let slot = 0, color = 7, tool = "pencil", clipboard = null, problem = "";
  let past = [], future = [], written = null;
  let colors = [];

  // ---- the code ---------------------------------------------------------------------------------
  function load() {
    const t = readTable(code.value, "SPRITES");
    problem = t.problem || "";
    sprites = t.map || new Map();
    for (const [n, s] of sprites) {
      if (!decode(s.text)) problem ||= `SPRITES[${n}] isn't 64 hex digits; fix it in the code, or delete it there.`;
    }
    if (code.value !== written) {
      past = [];
      future = [];
      written = code.value;
    }
    colors = lw ? lw.colors() : [];
  }
  function save() {
    let src = writeTable(code.value, "SPRITES", sprites, INTRO);
    if (sprites.size) src = withApi(src, 4);  // spaced sprite strings need API 4
    written = src;
    if (src === code.value) return;
    code.value = src;
    code.dispatchEvent(new Event("input"));
  }
  const pixels = () => (sprites.has(slot) ? decode(sprites.get(slot).text) : null);
  function put(px) {
    if (!sprites.has(slot)) sprites.set(slot, { text: "", name: "" });
    sprites.get(slot).text = encode(px);
    save();
    $("spr-count").textContent = `${sprites.size} of ${COUNT} used`;
    buttons();
    draw();
  }

  // ---- undo ---------------------------------------------------------------------------------------
  const snapshot = () => JSON.stringify({ s: [...sprites], slot });
  function remember() {
    past.push(snapshot());
    if (past.length > 200) past.shift();
    future = [];
    buttons();
  }
  function restore(state) {
    const d = JSON.parse(state);
    sprites = new Map(d.s);
    slot = d.slot;
    save();
    show();
  }
  const undo = () => { if (past.length) { future.push(snapshot()); restore(past.pop()); } };
  const redo = () => { if (future.length) { past.push(snapshot()); restore(future.pop()); } };

  // ---- drawing ----------------------------------------------------------------------------------
  const GRID = 320, CELL = GRID / 8, SHEET = 256;
  function checker(g, x, y, size) {  // color 0 is transparent: shown as a checkerboard
    const h = size / 2;
    g.fillStyle = "#1c2128";
    g.fillRect(x, y, size, size);
    g.fillStyle = "#2d333b";
    g.fillRect(x, y, h, h);
    g.fillRect(x + h, y + h, h, h);
  }
  function drawSprite(g, px, x, y, size) {  // size: a pixel's
    for (let i = 0; i < 64; i++) {
      const v = px[i], cx = x + (i % 8) * size, cy = y + Math.floor(i / 8) * size;
      if (v) {
        g.fillStyle = colors[v] || "#fff";
        g.fillRect(cx, cy, size, size);
      } else if (size >= 8) checker(g, cx, cy, size);
    }
  }
  function draw() {
    const px = pixels() || new Array(64).fill(0);
    const g = $("spr-grid").getContext("2d");
    for (let i = 0; i < 64; i++) checker(g, (i % 8) * CELL, Math.floor(i / 8) * CELL, CELL);
    drawSprite(g, px, 0, 0, CELL);
    g.strokeStyle = "rgba(255,255,255,0.08)";
    for (let k = 1; k < 8; k++) {
      g.beginPath(); g.moveTo(k * CELL, 0); g.lineTo(k * CELL, GRID); g.stroke();
      g.beginPath(); g.moveTo(0, k * CELL); g.lineTo(GRID, k * CELL); g.stroke();
    }
    for (const [id, size] of [["spr-1x", 1], ["spr-4x", 4]]) {
      const c = $(id), p = c.getContext("2d");
      p.fillStyle = "#0d1117";
      p.fillRect(0, 0, c.width, c.height);
      drawSprite(p, px, (c.width - 8 * size) / 2, (c.height - 8 * size) / 2, size);
    }
    drawSheet();
  }
  function drawSheet() {
    const g = $("spr-sheet").getContext("2d"), cell = SHEET / 8;
    g.fillStyle = "#0d1117";
    g.fillRect(0, 0, SHEET, SHEET);
    g.font = "10px monospace";
    for (let n = 0; n < COUNT; n++) {
      const x = (n % 8) * cell, y = Math.floor(n / 8) * cell, px = sprites.has(n) ? decode(sprites.get(n).text) : null;
      if (px) drawSprite(g, px, x + 4, y + 4, 3);
      else {
        g.fillStyle = "#30363d";
        g.fillText(String(n), x + 9, y + 20);
      }
      if (n === slot) {
        g.strokeStyle = "#f0b429";
        g.lineWidth = 2;
        g.strokeRect(x + 1, y + 1, cell - 2, cell - 2);
        g.lineWidth = 1;
      }
    }
  }

  function cellAt(e) {
    const r = $("spr-grid").getBoundingClientRect();
    const x = Math.floor((e.clientX - r.left) / r.width * 8), y = Math.floor((e.clientY - r.top) / r.height * 8);
    return x >= 0 && x < 8 && y >= 0 && y < 8 ? y * 8 + x : -1;
  }
  function fill(px, at, to) {  // the area of one color around `at`, 4 ways
    const from = px[at];
    if (from === to) return;
    const stack = [at];
    while (stack.length) {
      const i = stack.pop();
      if (px[i] !== from) continue;
      px[i] = to;
      const x = i % 8, y = Math.floor(i / 8);
      if (x > 0) stack.push(i - 1);
      if (x < 7) stack.push(i + 1);
      if (y > 0) stack.push(i - 8);
      if (y < 7) stack.push(i + 8);
    }
  }
  let dragging = false;
  function paint(e, first) {
    const at = cellAt(e);
    if (at < 0) return;
    const px = pixels() || new Array(64).fill(0);
    if (e.altKey || e.button === 2) {  // pick the color up
      if (first) { color = px[at]; palette(); }
      return;
    }
    if (tool === "fill") { if (first) fill(px, at, color); }
    else px[at] = color;
    $("spr-info").textContent = `pixel ${at % 8}, ${Math.floor(at / 8)}: color ${color}`;
    put(px);
  }

  function edit(fn) {  // a whole-sprite change: flip, shift, clear, paste
    const px = pixels();
    if (!px && fn !== "paste") return;
    remember();
    const p = px || new Array(64).fill(0);
    const at = (x, y) => p[((y + 8) % 8) * 8 + ((x + 8) % 8)];
    let out;
    if (fn === "clear") out = new Array(64).fill(0);
    else if (fn === "flipx") out = p.map((_, i) => at(7 - (i % 8), Math.floor(i / 8)));
    else if (fn === "flipy") out = p.map((_, i) => at(i % 8, 7 - Math.floor(i / 8)));
    else if (fn === "left") out = p.map((_, i) => at(i % 8 + 1, Math.floor(i / 8)));
    else if (fn === "right") out = p.map((_, i) => at(i % 8 - 1, Math.floor(i / 8)));
    else if (fn === "up") out = p.map((_, i) => at(i % 8, Math.floor(i / 8) + 1));
    else if (fn === "down") out = p.map((_, i) => at(i % 8, Math.floor(i / 8) - 1));
    else if (fn === "paste") out = clipboard.slice();
    put(out);
  }

  // ---- the tab ------------------------------------------------------------------------------------
  function palette() {
    $("spr-colors").replaceChildren(...colors.map((c, i) => {
      const b = document.createElement("button");
      b.className = "swatch" + (i === color ? " on" : "");
      b.style.background = i ? c : "repeating-conic-gradient(#2d333b 0 25%, #1c2128 0 50%) 0 0 / 12px 12px";
      b.style.color = luma(c) > 140 ? "#000" : "#fff";
      b.textContent = i;
      b.title = i ? `color ${i}` : "color 0: transparent in spr()";
      b.onclick = () => { color = i; palette(); };
      return b;
    }));
  }
  const luma = (hex) => {
    const n = parseInt(hex.slice(1), 16);
    return 0.3 * (n >> 16) + 0.59 * ((n >> 8) & 255) + 0.11 * (n & 255);
  };
  function buttons() {
    const has = sprites.has(slot);
    for (const id of ["spr-clear", "spr-flipx", "spr-flipy", "spr-left", "spr-right", "spr-up", "spr-down", "spr-copy", "spr-delete", "spr-name"]) $(id).disabled = !has;
    $("spr-paste").disabled = !clipboard;
    $("spr-undo").disabled = !past.length;
    $("spr-redo").disabled = !future.length;
    $("spr-pencil").classList.toggle("on", tool === "pencil");
    $("spr-fill").classList.toggle("on", tool === "fill");
  }
  function show() {
    load();
    $("spr-problem").textContent = problem;
    $("spr-problem").hidden = !problem;
    $("spr-body").hidden = !!problem;
    if (problem) return;
    $("spr-num").textContent = slot;
    $("spr-call").textContent = `spr(${slot}, x, y)`;
    $("spr-count").textContent = `${sprites.size} of ${COUNT} used`;
    $("spr-name").value = sprites.has(slot) ? sprites.get(slot).name : "";
    palette();
    buttons();
    draw();
  }

  function setUp() {
    const grid = $("spr-grid"), sheet = $("spr-sheet");
    grid.width = grid.height = GRID;
    sheet.width = sheet.height = SHEET;
    grid.addEventListener("contextmenu", (e) => e.preventDefault());
    grid.addEventListener("pointerdown", (e) => {
      grid.setPointerCapture(e.pointerId);
      if (!(e.altKey || e.button === 2)) remember();  // a stroke is one undo step
      dragging = true;
      paint(e, true);
    });
    grid.addEventListener("pointermove", (e) => { if (dragging && tool === "pencil") paint(e, false); });
    grid.addEventListener("pointerup", () => { dragging = false; buttons(); });
    grid.addEventListener("pointercancel", () => { dragging = false; });
    sheet.addEventListener("click", (e) => {
      const r = sheet.getBoundingClientRect();
      const n = Math.floor((e.clientY - r.top) / r.height * 8) * 8 + Math.floor((e.clientX - r.left) / r.width * 8);
      if (n >= 0 && n < COUNT) { slot = n; show(); }
    });
    $("spr-pencil").onclick = () => { tool = "pencil"; buttons(); };
    $("spr-fill").onclick = () => { tool = "fill"; buttons(); };
    for (const fn of ["clear", "flipx", "flipy", "left", "right", "up", "down"]) $("spr-" + fn).onclick = () => edit(fn);
    $("spr-copy").onclick = () => { clipboard = pixels(); buttons(); say(`Copied sprite ${slot}.`); };
    $("spr-paste").onclick = () => { if (clipboard) edit("paste"); };
    $("spr-undo").onclick = undo;
    $("spr-redo").onclick = redo;
    $("spr-delete").onclick = () => {
      if (!sprites.has(slot) || !confirm(`Delete sprite ${slot}?`)) return;
      remember();
      sprites.delete(slot);
      save();
      show();
    };
    let naming = false;
    $("spr-name").onfocus = () => { naming = false; };
    $("spr-name").oninput = () => {
      if (!sprites.has(slot)) return;
      if (!naming) { remember(); naming = true; }
      sprites.get(slot).name = $("spr-name").value.replace(/[\n"]/g, "").slice(0, 24);
      save();
    };
    addEventListener("keydown", (e) => {  // on this tab: undo, copy, paste; 0-9 and a-f pick a color
      if (!SoundEditor || SoundEditor.tab !== "sprites" || typing(e)) return;
      const mod = e.ctrlKey || e.metaKey, key = e.key.toLowerCase();
      if (mod && key === "z") { e.preventDefault(); e.shiftKey ? redo() : undo(); }
      else if (mod && key === "y") { e.preventDefault(); redo(); }
      else if (mod && key === "c" && !getSelection().toString()) { e.preventDefault(); $("spr-copy").click(); }
      else if (mod && key === "v") { e.preventDefault(); $("spr-paste").click(); }
      else if (!mod && /^[0-9a-f]$/.test(key)) { color = parseInt(key, 16); palette(); }
    });
  }

  return { setUp, show };
})();

SpriteEditor.setUp();
