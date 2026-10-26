// The playground: loads the engine (luanette.js / .wasm), runs a cart at its own frame rate, and
// turns the browser's input into the board's: a tap is A and a drag is the d-pad (as the board's
// player does it), keys stand in for the buttons, and a phone's tilt (or Shift+arrows) for tilt().
// Where the browser has Web Serial, it also installs the firmware (ESP Web Tools) and sends the
// cart being edited to a board on USB (board.js).
"use strict";

const CARTS = [
  ["lockload", "Lock & Load"], ["bubbles", "Bubbles"], ["night_swarm", "Night Swarm"],
  ["dice", "Dice"], ["mochi", "Mochi"], ["overdrive", "Overdrive"], ["overworld", "Overworld"],
];
const W = 410, H = 502;                  // the glass
const WIN_X = 12, WIN_Y = 60, SCALE = 3;  // where the 128x128 picture sits on it, and its scale
const TAP_MAX_MS = 350, DRAG_PX = 18;     // the board's: a tap lifts this soon without moving this far
const LEFT = 1, RIGHT = 2, UP = 4, DOWN = 8, A = 16, B = 32;

const $ = (id) => document.getElementById(id);
const glass = $("glass"), ctx2d = glass.getContext("2d"), status = $("status");
const image = ctx2d.createImageData(W, H);

// ---- the code editor (CodeMirror 5, web/vendor/codemirror) -------------------------------------
// `code` stands in for the textarea it replaced: .value, .hidden, and "input" events, which fire
// when the person types and when other code says so (the sound editor writes the code, then does).
const cm = CodeMirror.fromTextArea($("code"), {
  mode: "lua", theme: "luanette", lineNumbers: true, matchBrackets: true, indentUnit: 2, tabSize: 2,
  gutters: ["CodeMirror-linenumbers", "compile-gutter"],
  extraKeys: {
    "Ctrl-Enter": () => run(code.value), "Cmd-Enter": () => run(code.value),
    Tab: (ed) => (ed.somethingSelected() ? ed.indentSelection("add") : ed.replaceSelection("  ")),
    "Shift-Tab": (ed) => ed.indentSelection("subtract"),
    "Ctrl-Space": (ed) => suggest(ed, true),
    "Ctrl-H": "replace", "Cmd-Alt-F": "replace",  // find is Ctrl+F; replace is Ctrl+Shift+F too
  },
});
const inputs = [];
const code = {
  get value() { return cm.getValue(); },
  set value(text) { if (text !== cm.getValue()) cm.setValue(text); },
  // Another cart's code: what was typed before it can't be undone into this one.
  load(text) { cm.setValue(text); cm.clearHistory(); },
  get hidden() { return cm.getWrapperElement().hidden; },
  set hidden(h) { cm.getWrapperElement().hidden = h; if (!h) cm.refresh(); },
  addEventListener(type, fn) { if (type === "input") inputs.push(fn); },
  dispatchEvent(e) { if (e.type === "input") for (const fn of inputs) fn(e); },
};
cm.on("changes", (_, changes) => {
  if (changes.some((c) => c.origin !== "setValue")) code.dispatchEvent(new Event("input"));
});
// Whether a key is going into the editor or a form field rather than to the cart.
const typing = (e) => !!e.target.closest?.(".CodeMirror, input, textarea, select");

// ---- API help: names, signatures and what they do, from docs/api.md (api.json, tools/api_index.py)
const API = new Map();
fetch("api.json").then((r) => r.json()).then((list) => { for (const e of list) API.set(e.name, e); }).catch(() => {});

// The API name (dots and all, "hd.text") around column ch of a line: [start, end].
function nameAt(line, ch) {
  let a = ch, b = ch;
  while (a > 0 && /[\w.]/.test(line[a - 1])) a--;
  while (b < line.length && /[\w.]/.test(line[b])) b++;
  return [a, b];
}
const inCode = (ed, pos) => !/comment|string/.test(ed.getTokenTypeAt(pos) || "");

// Autocomplete: the API's names that start with what's typed, each with its signature.
function apiHint(ed) {
  const cur = ed.getCursor(), line = ed.getLine(cur.line);
  const [start] = nameAt(line, cur.ch);
  const typed = line.slice(start, cur.ch);
  if (!typed) return null;
  const list = [...API.values()].filter((e) => e.name.startsWith(typed) && e.name !== typed).map((e) => ({
    text: e.name,
    render(el) {
      const sig = document.createElement("span");
      sig.className = "hint-sig";
      sig.textContent = e.help[0].sig + (e.help.length > 1 ? `  +${e.help.length - 1}` : "");
      const does = document.createElement("span");
      does.className = "hint-does";
      does.textContent = e.help[0].does.length > 70 ? e.help[0].does.slice(0, 68) + "…" : e.help[0].does;
      el.append(sig, does);
    },
  }));
  return list.length ? { list, from: CodeMirror.Pos(cur.line, start), to: cur } : null;
}
function suggest(ed, asked) {
  if (!API.size || ed.state.completionActive || !inCode(ed, ed.getCursor())) return;
  ed.showHint({ hint: apiHint, completeSingle: false, alignWithWord: true, closeCharacters: /[\s()\[\]{};:>,=+\-*\/]/ });
}
cm.on("inputRead", (ed, change) => {  // as you type a name: two letters, or a dot (hd.)
  if (change.origin !== "+input" || !/^[\w.]$/.test(change.text[0])) return;
  const cur = ed.getCursor(), line = ed.getLine(cur.line), [start] = nameAt(line, cur.ch);
  if (cur.ch - start >= 2 || change.text[0] === ".") suggest(ed);
});

// Hover help: what a name under the mouse does.
const tip = document.createElement("div");
tip.className = "api-tip";
tip.hidden = true;
document.body.appendChild(tip);
let tipFor = "";
cm.getWrapperElement().addEventListener("mousemove", (e) => {
  const pos = cm.coordsChar({ left: e.clientX, top: e.clientY }, "window");
  const line = cm.getLine(pos.line) || "", [a, b] = nameAt(line, pos.ch);
  const name = line.slice(a, b).replace(/\.$/, "");
  const entry = pos.ch < b && pos.ch >= a && inCode(cm, { line: pos.line, ch: a + 1 }) ? API.get(name) : null;
  if (!entry) { tip.hidden = true; tipFor = ""; return; }
  if (tipFor !== name) {
    tipFor = name;
    tip.replaceChildren(...entry.help.flatMap((h) => {
      const sig = document.createElement("code");
      sig.textContent = h.sig;
      const does = document.createElement("p");
      does.textContent = h.does;
      return [sig, does];
    }));
  }
  tip.hidden = false;
  tip.style.left = Math.min(e.clientX + 14, innerWidth - tip.offsetWidth - 8) + "px";
  tip.style.top = (e.clientY + 18 + tip.offsetHeight > innerHeight ? e.clientY - tip.offsetHeight - 10 : e.clientY + 18) + "px";
});
cm.getWrapperElement().addEventListener("mouseleave", () => { tip.hidden = true; tipFor = ""; });
cm.on("keydown", () => { tip.hidden = true; });

// ---- errors as you type: the code compiled (not run) half a second after typing stops ----------
let compiling = 0, compileMark = null;
function checkCompile() {
  if (compileMark !== null) {
    cm.setGutterMarker(compileMark, "compile-gutter", null);
    cm.removeLineClass(compileMark, "text", "compile-error");
    compileMark = null;
  }
  const msg = lw ? lw.compile(code.value) : "";
  $("compile-msg").hidden = !msg;
  if (!msg) return;
  const m = /cart:(\d+):\s*(.*)/.exec(msg);
  $("compile-msg").textContent = m ? `Line ${m[1]}: ${m[2]}` : msg;
  if (!m) return;
  const line = Math.min(Number(m[1]), cm.lineCount()) - 1;
  const dot = document.createElement("div");
  dot.className = "compile-dot";
  dot.title = m[2];
  dot.textContent = "●";
  compileMark = cm.setGutterMarker(line, "compile-gutter", dot);
  cm.addLineClass(compileMark, "text", "compile-error");
}
cm.on("changes", () => { clearTimeout(compiling); compiling = setTimeout(checkCompile, 500); });

// The line an error names ("cart:16: ..."), shown and highlighted.
let marked = null;
function showLine(n) {
  if (typeof SoundEditor !== "undefined" && SoundEditor.tab !== "code") SoundEditor.show("code");
  if (marked !== null) cm.removeLineClass(marked, "background", "error-line");
  marked = cm.addLineClass(n - 1, "background", "error-line");
  cm.setCursor({ line: n - 1, ch: 0 });
  cm.scrollIntoView({ line: n - 1, ch: 0 }, 80);
  cm.focus();
}

let lw = null;            // the engine's exports
let current = null;       // the example being shown
const cartName = () => plainName(current);  // for the sound editor, where `current` is its own
let running = false, last = 0, spare = 0;
const originals = {};     // each example's source as shipped

// ---- input ---------------------------------------------------------------------------------
const touch = { down: false, x: 0, y: 0, downX: 0, downY: 0, downAt: 0, moved: false, tap: false };
const keys = new Set();
let phoneTilt = null;     // [ax, ay] from the phone's orientation, once allowed
let keyTilt = [0, 0];     // Shift+arrows, eased
let shiftHeld = false;

function glassPoint(e) {
  const r = glass.getBoundingClientRect();
  return [(e.clientX - r.left) * W / r.width, (e.clientY - r.top) * H / r.height];
}
glass.addEventListener("pointerdown", (e) => {
  glass.setPointerCapture(e.pointerId);
  [touch.x, touch.y] = glassPoint(e);
  Object.assign(touch, { down: true, downX: touch.x, downY: touch.y, downAt: performance.now(), moved: false });
  sound.unlock();
});
glass.addEventListener("pointermove", (e) => { if (touch.down) [touch.x, touch.y] = glassPoint(e); });
const lift = () => {
  if (touch.down && !touch.moved && performance.now() - touch.downAt < TAP_MAX_MS) touch.tap = true;
  touch.down = false;
};
glass.addEventListener("pointerup", lift);
glass.addEventListener("pointercancel", () => { touch.down = false; });

const KEYMAP = { ArrowLeft: LEFT, ArrowRight: RIGHT, ArrowUp: UP, ArrowDown: DOWN, KeyZ: A, Space: A, Enter: A, KeyX: B };
addEventListener("keydown", (e) => {
  if (typing(e)) return;
  if (e.code in KEYMAP) { e.preventDefault(); keys.add(e.code); sound.unlock(); }
});
addEventListener("keyup", (e) => keys.delete(e.code));
addEventListener("keydown", (e) => { if (e.key === "Shift") shiftHeld = true; });
addEventListener("keyup", (e) => { if (e.key === "Shift") shiftHeld = false; });
addEventListener("blur", () => { keys.clear(); shiftHeld = false; });

// The input for one frame, as the board's player builds it.
function input() {
  let held = 0, touching = 0, tx = 0, ty = 0;
  const full = lw.lw_full_canvas();
  if (touch.down) {
    const inWindow = touch.x >= WIN_X && touch.x < WIN_X + 128 * SCALE && touch.y >= WIN_Y && touch.y < WIN_Y + 128 * SCALE;
    if (full || inWindow) {
      touching = 1;
      tx = (touch.x - WIN_X) / SCALE;
      ty = (touch.y - WIN_Y) / SCALE;
    }
    const dx = touch.x - touch.downX, dy = touch.y - touch.downY;
    if (dx * dx + dy * dy > DRAG_PX * DRAG_PX) touch.moved = true;
    if (touch.moved) {  // the d-pad: the drag's direction from where it came down, diagonals too
      const ax = Math.abs(dx), ay = Math.abs(dy);
      if (ax * 5 > ay * 2) held |= dx < 0 ? LEFT : RIGHT;
      if (ay * 5 > ax * 2) held |= dy < 0 ? UP : DOWN;
    }
  } else if (touch.tap) {
    held |= A;  // a tap: A for one frame, as it lifts
    touch.tap = false;
  }
  const want = [0, 0];
  for (const k of keys) {
    if (shiftHeld) {
      if (k === "ArrowLeft") want[0] -= 0.8; else if (k === "ArrowRight") want[0] += 0.8;
      else if (k === "ArrowUp") want[1] -= 0.8; else if (k === "ArrowDown") want[1] += 0.8;
      else held |= KEYMAP[k] || 0;
    } else {
      held |= KEYMAP[k] || 0;
    }
  }
  keyTilt = keyTilt.map((v, i) => v + (want[i] - v) * 0.2);  // eased, as a hand tilts
  const pad = gamepad();
  held |= pad.held | padHeld;
  const [ax, ay] = pad.tilt || phoneTilt || keyTilt;
  return [held, touching, tx, ty, ax, ay];
}

// A gamepad: the d-pad or the left stick is the d-pad, the bottom face button A and the right one B
// (buttons 0 and 1; on an Xbox pad A and B, on a PlayStation pad cross and circle), and the right
// stick tilts while it's pushed. Pads without the standard layout still get the left stick and
// their first two buttons.
const STICK = 0.5, TILT_DEAD = 0.2;
function gamepad() {
  let held = 0, tilt = null;
  for (const pad of navigator.getGamepads ? navigator.getGamepads() : []) {
    if (!pad || !pad.connected) continue;
    const on = (i) => !!pad.buttons[i]?.pressed, std = pad.mapping === "standard";
    const [lx = 0, ly = 0, rx = 0, ry = 0] = pad.axes;
    if ((std && on(14)) || lx < -STICK) held |= LEFT;
    if ((std && on(15)) || lx > STICK) held |= RIGHT;
    if ((std && on(12)) || ly < -STICK) held |= UP;
    if ((std && on(13)) || ly > STICK) held |= DOWN;
    if (on(0)) held |= A;
    if (on(1)) held |= B;
    if (std && Math.hypot(rx, ry) > TILT_DEAD) tilt = [rx, ry];
  }
  return { held, tilt };
}
addEventListener("gamepadconnected", (e) => {
  $("gamepad").hidden = false;
  $("gamepad").textContent = `Gamepad: ${e.gamepad.id.replace(/\s*\(.*$/, "") || "connected"}. D-pad or left stick moves, A and B are the bottom and right buttons` +
    (e.gamepad.mapping === "standard" ? ", the right stick tilts." : ".");
});
addEventListener("gamepaddisconnected", () => {
  $("gamepad").hidden = ![...(navigator.getGamepads?.() || [])].some((p) => p && p.connected);
});

// ---- play mode: the screen alone; on a touch screen, a d-pad and A and B below it ------------------
let padHeld = 0, wentFullscreen = false;
const playing = () => document.body.classList.contains("playing");

function enterPlay() {
  if (!current) return;
  closeMenus();
  document.body.classList.add("playing");
  document.body.classList.toggle("touchpad", matchMedia("(pointer: coarse)").matches);
  $("play-title").textContent = titleOf(current);
  if (paused) setPaused(false);
  wentFullscreen = false;
  const el = document.documentElement;
  if (el.requestFullscreen && !document.fullscreenElement) {
    el.requestFullscreen({ navigationUI: "hide" }).then(() => { wentFullscreen = true; }).catch(() => {});
  }
}
function exitPlay() {
  if (!playing()) return;
  document.body.classList.remove("playing");
  padHeld = 0;
  for (const el of document.querySelectorAll(".pad .held")) el.classList.remove("held");
  if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
  cm.refresh();
}

// The d-pad: a finger on it holds the direction from its middle, diagonals too, and can slide round.
function setUpPad() {
  const dpad = $("dpad"), part = { [LEFT]: ".l", [RIGHT]: ".r", [UP]: ".u", [DOWN]: ".d" };
  const aim = (e) => {
    const r = dpad.getBoundingClientRect(), dx = e.clientX - r.left - r.width / 2, dy = e.clientY - r.top - r.height / 2;
    let d = 0;
    if (dx * dx + dy * dy > 14 * 14) {
      const ax = Math.abs(dx), ay = Math.abs(dy);
      if (ax * 5 > ay * 2) d |= dx < 0 ? LEFT : RIGHT;
      if (ay * 5 > ax * 2) d |= dy < 0 ? UP : DOWN;
    }
    padHeld = (padHeld & ~(LEFT | RIGHT | UP | DOWN)) | d;
    for (const [bit, sel] of Object.entries(part)) dpad.querySelector(sel).classList.toggle("held", !!(d & bit));
  };
  dpad.addEventListener("pointerdown", (e) => { e.preventDefault(); dpad.setPointerCapture(e.pointerId); sound.unlock(); aim(e); });
  dpad.addEventListener("pointermove", (e) => { if (dpad.hasPointerCapture(e.pointerId)) aim(e); });
  const off = () => {
    padHeld &= ~(LEFT | RIGHT | UP | DOWN);
    for (const el of dpad.querySelectorAll(".held")) el.classList.remove("held");
  };
  dpad.addEventListener("pointerup", off);
  dpad.addEventListener("pointercancel", off);
  for (const [id, bit] of [["pad-a", A], ["pad-b", B]]) {
    const b = $(id);
    b.addEventListener("pointerdown", (e) => { e.preventDefault(); b.setPointerCapture(e.pointerId); sound.unlock(); padHeld |= bit; b.classList.add("held"); });
    const up = () => { padHeld &= ~bit; b.classList.remove("held"); };
    b.addEventListener("pointerup", up);
    b.addEventListener("pointercancel", up);
    b.addEventListener("contextmenu", (e) => e.preventDefault());
  }
  $("pad").addEventListener("contextmenu", (e) => e.preventDefault());
  $("play").onclick = () => { sound.unlock(); enterPlay(); };
  $("play-exit").onclick = exitPlay;
  addEventListener("keydown", (e) => { if (e.key === "Escape" && playing()) exitPlay(); });
  document.addEventListener("fullscreenchange", () => {
    if (!document.fullscreenElement && wentFullscreen && playing()) exitPlay();  // Esc in fullscreen leaves both
  });
}

// A phone's tilt: which way things would roll, as the board's accelerometer gives it.
function useOrientation() {
  addEventListener("deviceorientation", (e) => {
    if (e.beta == null) return;
    const clamp = (v) => Math.max(-1, Math.min(1, v));
    phoneTilt = [clamp((e.gamma || 0) / 45), clamp(((e.beta || 0) - 40) / 45)];
  });
  $("tilt").hidden = true;
}
if (typeof DeviceOrientationEvent !== "undefined" && matchMedia("(pointer: coarse)").matches) {
  const btn = $("tilt");
  btn.hidden = false;
  btn.onclick = async () => {
    if (DeviceOrientationEvent.requestPermission) {
      try { if (await DeviceOrientationEvent.requestPermission() !== "granted") return; } catch { return; }
    }
    useOrientation();
  };
}

// ---- sound: the board's own synth --------------------------------------------------------------
// The built-in effects are rendered once each; the cart's own sounds and song come from the engine's
// tracker synth (synth.hpp), rendered a little ahead in chunks and queued back to back.
const AHEAD_S = 0.15, CHUNK = 1024;
const sound = {
  ctx: null, out: null, buffers: [], next: 0, queued: [],
  level: 0.7,  // the page's volume, 0-1 (the slider); everything plays through `out`
  unlock() {
    if (this.ctx || !window.AudioContext || !lw) return;
    this.ctx = new AudioContext();
    this.out = this.ctx.createGain();
    this.out.gain.value = this.gainFor(this.level);
    this.out.connect(this.ctx.destination);
    lw.audio_start(this.ctx.sampleRate);
    const rate = 16000;
    for (let n = 0; n < 8; n++) {
      const ptr = lw.lw_pcm(n, rate), len = lw.lw_pcm_len();
      const pcm = new Int16Array(lw.mod.HEAP16.buffer, ptr, len);
      const buf = this.ctx.createBuffer(1, len, rate);
      const ch = buf.getChannelData(0);
      for (let i = 0; i < len; i++) ch[i] = pcm[i] / 32768;
      this.buffers.push(buf);
    }
  },
  play(n) {
    if (!this.ctx || !this.buffers[n]) return;
    const src = this.ctx.createBufferSource();
    src.buffer = this.buffers[n];
    src.connect(this.out);
    src.start();
  },
  gainFor: (level) => level * level,  // heard loudness follows the square more closely than the level
  setLevel(level) {
    this.level = level;
    try { localStorage.setItem("luanette.volume", String(level)); } catch {}
    if (this.out) this.out.gain.setTargetAtTime(this.gainFor(level), this.ctx.currentTime, 0.02);  // no click
  },
  // Keeps AHEAD_S of the synth queued while it has anything to play; every frame.
  pump() {
    if (!this.ctx || !lw.audio_busy()) return;
    const now = this.ctx.currentTime, rate = this.ctx.sampleRate;
    if (this.next < now + 0.01) this.next = now + 0.03;  // starting (again): a moment to get going
    while (this.next < now + AHEAD_S) {
      const ptr = lw.audio_render(CHUNK);
      const buf = this.ctx.createBuffer(1, CHUNK, rate);
      buf.copyToChannel(new Float32Array(lw.mod.HEAPF32.buffer, ptr, CHUNK), 0);
      const src = this.ctx.createBufferSource();
      src.buffer = buf;
      src.connect(this.out);
      src.start(this.next);
      src.onended = () => this.queued.splice(this.queued.indexOf(src), 1);
      this.queued.push(src);
      this.next += CHUNK / rate;
    }
  },
  // Silences the synth now, what's queued too (a restart, a stop button).
  stop() {
    if (!lw) return;
    lw.audio_stop();
    for (const src of this.queued.splice(0)) { try { src.stop(); } catch {} }
    this.next = 0;
  },
};

// ---- your carts: made here or opened from a file, kept in this browser ----------------------
const NAME_RE = /^[a-z0-9_-]{1,32}$/;  // the board keys a cart by name (under 48 bytes); also its file name
const TEMPLATE = (name) => `-- title: ${name}
-- by: you
-- about: a dot you move with your finger

x, y = 64, 64

function _update()
  if btn(0) then x = x - 1 end
  if btn(1) then x = x + 1 end
  if btn(2) then y = y - 1 end
  if btn(3) then y = y + 1 end
end

function _draw()
  cls(1)
  circfill(x, y, 4, 8)
  print("hello", 2, 2, 7)
end
`;
const isExample = (name) => CARTS.some(([n]) => n === name);
// The gallery's carts (gallery/, gallery.json from tools/gallery_index.py), named "gallery/NAME" here
// so they can't meet your own carts' names; on a board, in a file, they're NAME.
let GALLERY = [];
const isGallery = (name) => typeof name === "string" && name.startsWith("gallery/") && GALLERY.some((g) => "gallery/" + g.name === name);
const isShipped = (name) => isExample(name) || isGallery(name);  // comes with the page: your edits are a draft over it
const shippedPath = (name) => (isGallery(name) ? name : "examples/" + name) + ".lua";
const plainName = (name) => (name || "").replace(/^gallery\//, "");
const storeGet = (k) => { try { return localStorage.getItem(k); } catch { return null; } };
const storeSet = (k, v) => { try { localStorage.setItem(k, v); return true; } catch { return false; } };
const storeDel = (k) => { try { localStorage.removeItem(k); } catch {} };
// An example's (or gallery cart's) edits are a draft over the original; your own cart's source is all there is.
const sourceKey = (name) => (isShipped(name) ? "luanette.draft." : "luanette.cart.") + name;

let mine = [];  // your carts' names, in the order made
try { mine = JSON.parse(storeGet("luanette.mine") || "[]").filter((n) => NAME_RE.test(n) && !isExample(n)); } catch {}
const saveMine = () => storeSet("luanette.mine", JSON.stringify(mine));

function nameProblem(name) {
  if (!NAME_RE.test(name)) return "A name is 1 to 32 characters: a-z, 0-9, _ and -.";
  if (isExample(name)) return `${name} is an example's name. Pick another, or edit that example.`;
  if (name === "launcher") return "That's the launcher's name. Pick another.";
  return "";
}
// A file's name made into a cart's: "My Game.lua" -> "my_game".
const slug = (file) => file.toLowerCase().replace(/\.lua$/, "").replace(/[^a-z0-9_-]+/g, "_").replace(/^_+|_+$/g, "").slice(0, 32);

function addCart(name, src) {
  if (!mine.includes(name)) mine.push(name);
  const kept = saveMine() && keep(name, src);
  renderCarts();
  const opened = select(name, src);
  if (!kept) say("This browser isn't keeping data for this page, so the cart goes when the tab does. Save .lua keeps a copy.", true);
  return opened;
}

function newCart() {
  $("newname").value = "";
  $("newproblem").hidden = true;
  $("newcart").showModal();
  $("newname").focus();
}
// A new cart from a template (templates/: blank, platformer, shooter, puzzle), its title its name.
async function createCart() {
  const name = $("newname").value.trim().toLowerCase();
  const problem = mine.includes(name) ? `You already have ${name}.` : nameProblem(name);
  $("newproblem").textContent = problem;
  $("newproblem").hidden = !problem;
  if (problem) { $("newname").focus(); return; }
  $("newcart").close();
  history.pushState(null, "", "#" + name);  // Back returns to where it was made from
  let src = TEMPLATE(name);
  try {
    const r = await fetch("templates/" + $("newtemplate").value + ".lua");
    if (r.ok) src = (await r.text()).replace(/^-- title: .*$/m, "-- title: " + name);
  } catch {}
  addCart(name, src);
}

// ---- libraries (libs/): code a cart includes, between `-- lib:` and `-- end lib:` lines ---------------
let libs = null;
async function openLibs() {
  if (!libs) {
    try { libs = await (await fetch("libs.json")).json(); } catch { libs = []; }
  }
  renderLibs();
  $("libs").showModal();
}
function renderLibs() {
  $("libs-list").replaceChildren(...libs.map((l) => {
    const has = new RegExp(`^-- lib: ${l.name} (\\d+)`, "m").exec(code.value);
    const row = document.createElement("div"), b = document.createElement("button");
    row.className = "lib";
    row.innerHTML = `<strong></strong><span class="note"></span>`;
    row.querySelector("strong").textContent = l.name;
    row.querySelector(".note").textContent = `${l.about} (${(l.bytes / 1024).toFixed(1)} KB${l.api > 1 ? `, API ${l.api}` : ""})`;
    const now = has ? Number(has[1]) : -1;  // the version the cart has (-1: none)
    b.textContent = !has ? "Add" : now < l.version ? `Update to ${l.version}` : "Added";
    b.disabled = now >= l.version;
    b.className = has ? "" : "primary";
    b.onclick = () => {
      code.value = Tables.addLib(code.value, l.text).src;
      code.dispatchEvent(new Event("input"));
      say(`${has ? "Updated" : "Added"} the ${l.name} library: its comments in the code say how to use it.`);
      renderLibs();
    };
    row.append(b);
    return row;
  }));
}

async function openFile(file) {
  if (!file) return;
  if (file.size > 256 * 1024) { say(`${file.name} is too big to be a cart.`, true); return; }
  const src = await file.text();
  let name = slug(file.name) || "cart";
  if (isExample(name) || name === "launcher") name = (name + "_mine").slice(0, 32);
  if (mine.includes(name) && storeGet(sourceKey(name)) !== src &&
      !confirm(`You already have a cart called ${name} here. Replace it with ${file.name}?`)) return;
  addCart(name, src);
}

function saveFile() {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([code.value], { type: "text/plain" }));
  a.download = plainName(current) + ".lua";
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

// ---- share links: a cart in a URL --------------------------------------------------------------
// "#cart=" then the cart ({name, src}: its code, which holds its sounds and song) as JSON, deflated
// and in base64url ("d" first), or not deflated where the browser can't ("u"). After the "#", so
// it never reaches a server: the link is the only copy.
const b64url = (bytes) => {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
};
const unb64url = (text) => Uint8Array.from(atob(text.replace(/-/g, "+").replace(/_/g, "/")), (c) => c.charCodeAt(0));
async function through(bytes, stream) {
  return new Uint8Array(await new Response(new Blob([bytes]).stream().pipeThrough(stream)).arrayBuffer());
}

async function makeLink() {
  const raw = new TextEncoder().encode(JSON.stringify({ name: current, src: code.value }));
  const data = typeof CompressionStream === "function"
    ? "d" + b64url(await through(raw, new CompressionStream("deflate-raw")))
    : "u" + b64url(raw);
  return location.href.split("#")[0] + "#cart=" + data;
}

// "&play" after a link opens the cart in play mode: the screen alone, with buttons on a phone.
async function shareLink(play) {
  const url = (await makeLink()) + (play ? "&play" : "");
  let copied = false;
  try { await navigator.clipboard.writeText(url); copied = true; } catch {}
  $("sharelink").value = url;
  $("sharebox").hidden = copied;
  say(copied ? `Link copied: ${plainName(current)} with its code, sounds and song (${url.length} characters). Opening it adds the cart to that browser's carts${play ? " and plays it" : ""}.`
    : "Copy this link: opening it adds the cart to that browser's carts.");
}

async function readLink(data) {
  const bytes = unb64url(data.slice(1));
  const raw = data[0] === "d" ? await through(bytes, new DecompressionStream("deflate-raw")) : bytes;
  const cart = JSON.parse(new TextDecoder().decode(raw));
  if (typeof cart.name !== "string" || typeof cart.src !== "string") throw new Error("not a cart");
  return cart;
}

// A link's cart among your carts: under its name, or a new one when that's taken by something else.
async function openLink(data) {
  let cart;
  try { cart = await readLink(data); } catch {
    say("This link's cart couldn't be read. It may have been cut short when it was copied.", true);
    return false;
  }
  if (isGallery(cart.name)) {  // a gallery cart as it ships opens itself, as an example does
    const original = await shippedSource(cart.name), draft = storeGet(sourceKey(cart.name));
    if (original === cart.src && (!draft || draft === cart.src)) {
      await select(cart.name);
      return true;
    }
  }
  const base = slug(plainName(cart.name)) || "shared";
  if (isExample(base)) {  // an example as it ships: the example itself, unless you've edits of your own to it
    if (!(base in originals)) originals[base] = await (await fetch("examples/" + base + ".lua")).text();
    const draft = storeGet(sourceKey(base));
    if (originals[base] === cart.src && (!draft || draft === cart.src)) {
      await select(base);
      return true;
    }
  }
  let name = nameProblem(base) ? (base + "_shared").slice(0, 32) : base;
  for (let k = 2; mine.includes(name) && storeGet(sourceKey(name)) !== cart.src; k++) name = (base.slice(0, 28) + "_" + k);
  await addCart(name, cart.src);
  const title = (/^--\s*title\s*:\s*(.+)$/m.exec(cart.src) || [, name])[1].trim();
  say(`Opened ${title} from a link. It's in your carts as ${name} now.`);
  return true;
}

// A shipped cart's code as it comes (examples/, gallery/), fetched once.
async function shippedSource(name) {
  if (!(name in originals)) originals[name] = await (await fetch(shippedPath(name))).text();
  return originals[name];
}

// Submit to gallery: a GitHub issue with the cart's link in it, for a maintainer to add
// (tools/gallery_add.py). A link too long for a URL asks for the .lua file instead.
async function submitToGallery() {
  if (isShipped(current)) {
    say("That one's in the playground already. Submit a cart of your own: + New cart, or Open .lua.", true);
    return;
  }
  const link = await makeLink();
  const title = (/^--\s*title\s*:\s*(.+)$/m.exec(code.value) || [, current])[1].trim();
  const by = (/^--\s*by\s*:\s*(.+)$/m.exec(code.value) || [, ""])[1].trim();
  const checklist = "- [ ] It's my own work (or I can share it), and it can be in the repository under the MIT license\n" +
    "- [ ] It runs in the playground, and Send to board takes it\n- [ ] Its header has -- title:, -- by: and -- about:\n";
  let body = `**${title}**${by ? " by " + by : ""}\n\nThe cart: ${link}\n\n${checklist}\nAnything you'd like to say about it:\n`;
  const base = "https://github.com/Geofresh/luanette/issues/new?labels=gallery&title=" + encodeURIComponent("Gallery: " + title) + "&body=";
  if ((base + encodeURIComponent(body)).length > 7500) {
    body = `**${title}**${by ? " by " + by : ""}\n\nThe cart is too big for a link: it's attached (Save .lua in the playground, then drag the file here).\n\n${checklist}`;
  }
  open(base + encodeURIComponent(body), "_blank", "noopener");
  say("A GitHub issue is open in a new tab, with your cart in it. Submit it there.");
}

function deleteCart() {
  if (!confirm(`Delete ${current} from this browser? Save .lua first if you want to keep a copy.`)) return;
  storeDel(sourceKey(current));
  storeDel("luanette.when." + current);
  mine = mine.filter((n) => n !== current);
  saveMine();
  renderCarts();
  select(CARTS[0][0]);
}

// A cart's name as people read it: an example's or gallery cart's title, your own cart's name.
function titleOf(name) {
  const ex = CARTS.find(([n]) => n === name);
  if (ex) return ex[1];
  const g = GALLERY.find((g) => "gallery/" + g.name === name);
  return g ? g.title : plainName(name);
}

// Whether the cart in the editor differs from how it ships: the "edited" mark by its name.
function showEdited() {
  const shipped = current && isShipped(current) && current in originals;
  const was = $("edited").hidden;
  $("edited").hidden = !shipped || code.value === originals[current];
  if (was !== $("edited").hidden) renderCarts();  // the cart menu marks it too
}
// Keeps the editor's text as this cart's, and when; the cart browser shows the time.
function keep(name, src) {
  storeSet("luanette.when." + name, String(Date.now()));
  return storeSet(sourceKey(name), src);
}

// The cart menu under the cart's name: examples, the gallery, your carts, and a new one.
function renderCarts() {
  const list = $("carts");
  const group = (text) => Object.assign(document.createElement("div"), { className: "group", textContent: text });
  const item = (name, title, aside, tag) => {
    const b = document.createElement("button");
    const t = document.createElement("span");
    t.textContent = title;
    b.append(t);
    if (tag) {  // "-- made with:" in its header: made with an agent
      const s = document.createElement("span");
      s.className = "agent-tag";
      s.textContent = tag;
      t.append(" ", s);
    }
    if (aside) b.append(Object.assign(document.createElement("span"), { className: "aside", textContent: aside }));
    b.dataset.name = name;
    b.classList.toggle("on", name === current);
    b.onclick = () => { closeMenus(); sound.unlock(); select(name); };  // a click: the browser allows sound from here
    return b;
  };
  const draft = (name) => {  // a draft the same as the original (kept by older pages) isn't an edit
    const d = storeGet(sourceKey(name));
    return d && (!(name in originals) || d !== originals[name]) ? "edited" : "";
  };
  const rows = [group("Examples"), ...CARTS.map(([name, title]) => item(name, title, draft(name)))];
  if (GALLERY.length) {
    rows.push(group("Gallery"), ...GALLERY.map((g) => {
      const b = item("gallery/" + g.name, g.title, draft("gallery/" + g.name) || "by " + g.by, g.made_with ? "agent-made" : "");
      b.title = g.about;
      return b;
    }));
  }
  rows.push(group("Your carts"), ...mine.map((name) => item(name, name)));
  const add = document.createElement("button");
  add.textContent = "+ New cart…";
  add.onclick = () => { closeMenus(); newCart(); };
  rows.push(add);
  list.replaceChildren(...rows);
}

// The address after "#": "carts", a cart's name, or "cart=" and a shared cart; "&play" after either
// of the last two opens it in play mode.
function address() {
  const raw = decodeURIComponent(location.hash.slice(1));
  return { h: raw.replace(/&play$/, ""), play: raw.endsWith("&play") };
}

// ---- the cart browser: where to start, and a card for each cart ------------------------------------
let EXAMPLES = {};  // name -> {title, by, about}, from examples.json (web/build.sh)
let resumeOnBack = false;
const browsing = () => document.body.classList.contains("browsing");

function ago(ms) {
  const m = Math.round((Date.now() - ms) / 60000);
  if (m < 1) return "just now";
  if (m < 60) return m + " min ago";
  const h = Math.round(m / 60);
  if (h < 24) return h + (h === 1 ? " hour ago" : " hours ago");
  const d = Math.round(h / 24);
  return d === 1 ? "yesterday" : d + " days ago";
}

function card(name, title, about, pic, extra = {}) {
  const b = document.createElement("button");
  b.className = "card";
  b.dataset.name = name;
  b.dataset.find = (title + " " + about + " " + (extra.by || "")).toLowerCase();
  if (pic) {
    const img = Object.assign(document.createElement("img"), { className: "pic", src: pic, alt: "", loading: "lazy" });
    img.onerror = () => img.replaceWith(Object.assign(document.createElement("div"), { className: "pic none", textContent: "no picture yet" }));
    b.append(img);
  } else {
    b.append(Object.assign(document.createElement("div"), { className: "pic none", textContent: "run it for a picture" }));
  }
  const words = Object.assign(document.createElement("span"), { className: "words" });
  const t = document.createElement("strong");
  t.textContent = title;
  if (extra.tag) t.append(Object.assign(document.createElement("span"), { className: "agent-tag", textContent: extra.tag }));
  words.append(t);
  if (about) words.append(Object.assign(document.createElement("span"), { className: "about", textContent: about }));
  if (extra.byLine) words.append(Object.assign(document.createElement("span"), { className: "about", textContent: "by " + extra.by }));
  if (extra.when) words.append(Object.assign(document.createElement("span"), { className: "when", textContent: extra.when }));
  b.append(words);
  b.onclick = async () => {
    sound.unlock();
    history.pushState(null, "", "#" + name);
    await select(name);
    if (matchMedia("(pointer: coarse)").matches) enterPlay();  // on a phone, a card is for playing first
  };
  return b;
}
function addCard(cls, svg, title, note, onclick) {
  const b = document.createElement("button");
  b.className = "card add " + cls;
  b.innerHTML = svg;
  b.append(Object.assign(document.createElement("strong"), { textContent: title }),
    Object.assign(document.createElement("span"), { textContent: note }));
  b.onclick = onclick;
  return b;
}

function renderBrowser() {
  const edited = (name) => {
    const d = storeGet(sourceKey(name)), when = Number(storeGet("luanette.when." + name));
    return d && (!(name in originals) || d !== originals[name]) ? "edited" + (when ? " " + ago(when) : "") : "";
  };
  const byWhen = [...mine].sort((a, b) => Number(storeGet("luanette.when." + b) || 0) - Number(storeGet("luanette.when." + a) || 0));
  $("cards-mine").replaceChildren(
    addCard("new", '<svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="M12 5v14M5 12h14"/></svg>',
      "New cart", "blank, platformer, shooter or puzzle", newCart),
    ...byWhen.map((name) => {
      const src = storeGet(sourceKey(name)) || "", when = Number(storeGet("luanette.when." + name));
      const title = (/^--\s*title\s*:\s*(.+)$/m.exec(src) || [, name])[1].trim();
      const about = (/^--\s*about\s*:\s*(.+)$/m.exec(src) || [, ""])[1].trim();
      return card(name, title, about, storeGet("luanette.thumb." + name), { when: when ? "edited " + ago(when) : "" });
    }),
    addCard("open", '<svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="M4 4h10l6 6v10H4zM14 4v6h6"/></svg>',
      "Open a .lua file", "or drop one on the page", () => $("file").click()));
  $("cards-examples").replaceChildren(...CARTS.map(([name, title]) =>
    card(name, title, EXAMPLES[name]?.about || "", "thumbs/" + name + ".png", { when: edited(name), by: EXAMPLES[name]?.by })));
  $("gallery-head").hidden = !GALLERY.length;
  $("cards-gallery").replaceChildren(...GALLERY.map((g) =>
    card("gallery/" + g.name, g.title, g.about, "thumbs/gallery-" + g.name + ".png",
      { tag: g.made_with ? "agent-made" : "", by: g.by, byLine: true, when: edited("gallery/" + g.name) })));
  findCarts();
}

function findCarts() {
  const q = $("find").value.trim().toLowerCase();
  let shown = 0;
  for (const c of document.querySelectorAll(".browser .card")) {
    const hit = !q || (c.dataset.find || "").includes(q);
    c.hidden = !hit;
    if (hit && c.dataset.find) shown++;
  }
  $("find-none").hidden = !q || shown > 0;
}

function showBrowser() {
  closeMenus();
  exitPlay();
  if (!browsing() && running && !paused) { setPaused(true); resumeOnBack = true; }
  sound.stop();
  document.body.classList.add("browsing");
  $("browser").hidden = false;
  $("back").hidden = !current;
  $("back-title").textContent = current ? titleOf(current) : "";
  document.title = "Luanette playground";
  renderBrowser();
}
function showEditor() {
  if (!browsing()) return;
  document.body.classList.remove("browsing");
  $("browser").hidden = true;
  if (resumeOnBack) setPaused(false);
  resumeOnBack = false;
  if (current) document.title = titleOf(current) + " · Luanette";
  cm.refresh();
}

// A picture of your own cart for its card: the screen a couple of seconds in, small.
function keepThumb() {
  if (!current || isShipped(current)) return;
  const c = document.createElement("canvas");
  c.width = 205;
  c.height = 251;
  c.getContext("2d").drawImage(glass, 0, 0, c.width, c.height);
  storeSet("luanette.thumb." + current, c.toDataURL("image/jpeg", 0.8));
}

// ---- the menus: the cart's, File, Share, Board ----------------------------------------------------
function closeMenus(except) {
  if (!except) {
    document.body.classList.remove("menus-open");
    $("more").setAttribute("aria-expanded", "false");
  }
  for (const m of document.querySelectorAll(".menu")) {
    if (m === except) continue;
    m.querySelector(".menu-list").hidden = true;
    m.querySelector("button").setAttribute("aria-expanded", "false");
  }
}
function setUpMenus() {
  for (const m of document.querySelectorAll(".menu")) {
    const btn = m.querySelector("button"), list = m.querySelector(".menu-list");
    btn.addEventListener("click", (e) => {
      e.stopPropagation();
      const open = list.hidden;
      closeMenus(m);
      list.hidden = !open;
      btn.setAttribute("aria-expanded", String(open));
      if (open) (list.querySelector("button.on") || list.querySelector("button:not([hidden])"))?.focus();
    });
    // a menu's own items close it; what's inside the installer's button is the installer's
    list.addEventListener("click", (e) => { if (e.target.closest("button") && !e.target.closest("esp-web-install-button")) closeMenus(); });
    list.addEventListener("keydown", (e) => {  // arrows move between the items
      if (e.key !== "ArrowDown" && e.key !== "ArrowUp") return;
      e.preventDefault();
      const items = [...list.querySelectorAll("button")].filter((b) => !b.hidden && b.offsetParent);
      const i = items.indexOf(document.activeElement);
      items[(i + (e.key === "ArrowDown" ? 1 : items.length - 1)) % items.length]?.focus();
    });
  }
  $("more").addEventListener("click", (e) => {  // a phone: File, Share and Board in one list
    e.stopPropagation();
    const open = !document.body.classList.contains("menus-open");
    closeMenus();
    document.body.classList.toggle("menus-open", open);
    $("more").setAttribute("aria-expanded", String(open));
    if (open) $("menus").querySelector(".menu-list button:not([hidden])")?.focus();
  });
  document.addEventListener("click", (e) => { if (!e.target.closest(".menu-list")) closeMenus(); });
  addEventListener("keydown", (e) => {
    if (e.key === "Escape" && document.body.classList.contains("menus-open")) { closeMenus(); $("more").focus(); return; }
    if (e.key !== "Escape" || !document.querySelector(".menu-list:not([hidden])")) return;
    const open = document.querySelector(".menu-list:not([hidden])").parentElement.querySelector("button");
    closeMenus();
    open.focus();
  });
}

// ---- the split between the screen and the editor: drag it, or arrows on it; kept in this browser --
function setUpSplit() {
  const app = $("app"), split = $("split");
  const set = (w) => {
    w = Math.max(300, Math.min(w, innerWidth - 460));
    app.style.setProperty("--dev-w", w + "px");
    cm.refresh();
    return w;
  };
  const kept = Number(storeGet("luanette.split"));
  if (kept) set(kept);
  let from = null;
  split.addEventListener("pointerdown", (e) => {
    split.setPointerCapture(e.pointerId);
    split.classList.add("dragging");
    from = { x: e.clientX, w: app.querySelector(".device").offsetWidth };
  });
  split.addEventListener("pointermove", (e) => { if (from) set(from.w + e.clientX - from.x); });
  const done = () => {
    if (!from) return;
    from = null;
    split.classList.remove("dragging");
    storeSet("luanette.split", String(app.querySelector(".device").offsetWidth));
  };
  split.addEventListener("pointerup", done);
  split.addEventListener("pointercancel", done);
  split.addEventListener("keydown", (e) => {
    if (e.key !== "ArrowLeft" && e.key !== "ArrowRight") return;
    e.preventDefault();
    storeSet("luanette.split", String(set(app.querySelector(".device").offsetWidth + (e.key === "ArrowLeft" ? -20 : 20))));
  });
  addEventListener("resize", () => cm.refresh());
}

// ---- the board: install the firmware, send carts (Web Serial: Chrome and Edge) -------------
const ESPRESSIF = 0x303a;  // the USB vendor of the S3's own port
let sending = false;

// The board's port: the one this page was allowed before, or the one the person picks.
async function boardPort() {
  const known = (await navigator.serial.getPorts()).filter((p) => p.getInfo().usbVendorId === ESPRESSIF);
  return known[0] || navigator.serial.requestPort({ filters: [{ usbVendorId: ESPRESSIF }] });
}

// The cart in the editor, to the board under its name (as push.py names a cart by its file): an
// example's takes the built-in one's place. Checked first as far as a page can: it runs here,
// has its header, and fits; check.py's instruction budget is the one check it skips.
async function send() {
  if (sending || !current) return;
  if (overBudget === code.value) {  // before running it again, which would only stop it again
    say("This cart went over 30,000 Lua instructions in a frame here, so check.py would refuse it and the board " +
      "could fall behind. Make the slow frame do less, then send it.", true);
    return;
  }
  run(code.value);
  if (!running) return;  // run() says why
  const src = lw.shipped(code.value);
  const head = src.split("\n").slice(0, 12).join("\n");
  const missing = ["title", "by", "about"].filter((k) => !new RegExp(`^--\\s*${k}\\s*:\\s*\\S`, "m").test(head));
  if (missing.length) {
    say("The board wants these at the top first: " + missing.map((k) => `-- ${k}: ...`).join(", "), true);
    return;
  }
  const bytes = new TextEncoder().encode(src).length;
  if (bytes > lw.max_source()) {
    say(`The cart is ${bytes} bytes without its comments; the board takes ${lw.max_source()}.`, true);
    return;
  }
  const title = /^--\s*title\s*:\s*(.+)$/m.exec(head)[1].trim();
  let port;
  try { port = await boardPort(); } catch { return; }  // the chooser was closed
  sending = true;
  $("send").disabled = true;
  say(`Sending ${title} to the board…`);
  try {
    await Board.sendCart(port, { name: plainName(current), title, lua: src });
    say(`${title} is on the board and open there. It stays in its flash` +
      (isExample(current) ? ", in place of the built-in one." : " until you remove it."));
  } catch (e) {
    portTrouble(e);
  } finally {
    sending = false;
    $("send").disabled = false;
  }
}

const portTrouble = (e) => say(e instanceof Board.BoardError ? e.message
  : `Couldn't use the board's port (${e.message}). Is the installer, another tab or push.py using it?`, true);

// Takes the cart of this name out of the board's flash; for an example, the built-in one is back.
async function removeFromBoard() {
  if (sending || !current) return;
  let port;
  try { port = await boardPort(); } catch { return; }
  sending = true;
  $("unsend").disabled = true;
  try {
    await Board.removeCart(port, plainName(current), isExample(current));
    say(isExample(current) ? `The board has its built-in ${current} again.` : `${plainName(current)} is off the board.`);
  } catch (e) {
    portTrouble(e);
  } finally {
    sending = false;
    $("unsend").disabled = false;
  }
}

async function setUpBoard() {
  if (!("serial" in navigator)) return;
  $("no-serial").hidden = true;
  $("has-serial").hidden = false;
  $("send").hidden = false;
  $("send").onclick = send;
  $("unsend").hidden = false;
  $("unsend").onclick = removeFromBoard;
  try {  // the Install button, once there's a release behind it (web/firmware.sh)
    const r = await fetch("firmware/manifest.json");
    if (!r.ok) return;
    const { version } = await r.json();
    await import("./vendor/esp-web-tools/install-button.js");  // 10.4.0, kept in the repo (its README says why)
    $("install").querySelector("button").textContent = `Install Luanette ${version} on a board`;
    $("board").hidden = false;
  } catch {}
}

// ---- running -------------------------------------------------------------------------------
function show() {
  image.data.set(new Uint8ClampedArray(lw.mod.HEAPU8.buffer, lw.lw_pixels(), W * H * 4));
  ctx2d.putImageData(image, 0, 0);
}

function say(text, error) {
  status.classList.toggle("error", !!error);
  if (marked !== null) { cm.removeLineClass(marked, "background", "error-line"); marked = null; }
  const m = error ? /cart:(\d+):/.exec(text) : null;
  if (!m) { status.textContent = text; return; }
  const a = document.createElement("a");  // the line, as a link to it
  a.href = "#";
  a.textContent = m[0];
  a.title = "Show line " + m[1];
  a.onclick = (e) => { e.preventDefault(); showLine(Number(m[1])); };
  status.replaceChildren(text.slice(0, m.index), a, text.slice(m.index + m[0].length));
}

let runSrc = "", overBudget = null;  // the code running, and code that went over the frame budget here

// ---- the console: what the cart log()s, with the frame it came in -------------------------------
const CONSOLE_KEEP = 500;
let frameNo = 0, logged = 0;
function drainLog() {
  const n = lw.log_count();
  if (!n) return;
  const box = $("console-lines"), frag = document.createDocumentFragment();
  for (let i = 0; i < n; i++) {
    const row = document.createElement("div");
    const at = document.createElement("span");
    at.className = "at";
    at.textContent = frameNo + " ";
    row.append(at, lw.log());
    frag.appendChild(row);
  }
  box.appendChild(frag);
  while (box.childElementCount > CONSOLE_KEEP) box.firstChild.remove();
  logged += n;
  $("console-count").textContent = `(${logged})`;
  if (!$("console").open && !$("console").dataset.closed) $("console").open = true;  // the first lines open it, once
  box.scrollTop = box.scrollHeight;
}
function clearLog() {
  $("console-lines").replaceChildren();
  logged = 0;
  $("console-count").textContent = "";
}
function run(src) {
  runSrc = src;
  frameNo = 0;
  clearLog();
  meter.reset();
  setPaused(false);
  const name = current || "playground";
  sound.stop();
  const ok = lw.ccall("lw_start", "number", ["string", "string"], [name, src]);
  show();
  drainLog();  // what the top of the cart and _init logged: frame 0
  colorStrip();
  if (!ok) { running = false; say(lw.error(), true); return; }
  say("");
  running = true;
  last = performance.now();
  spare = 0;
  if (!isShipped(name) || src !== originals[name]) keep(name, src);
}

// One frame of the cart: its input, its log, its sounds; false once it has stopped.
function frameOnce() {
  frameNo++;
  if (frameNo === 60) keepThumb();
  const alive = lw.lw_frame(...input());
  drainLog();
  for (let n = lw.lw_sound(); n >= 0; n = lw.lw_sound()) sound.play(n);
  meter.count(lw.lw_ops());
  if (!alive) {
    running = false;
    const err = lw.error();
    if (err.includes("over budget")) overBudget = runSrc;  // Send won't take this code
    say(err || "The cart ended.", !!err);
  }
  return alive;
}

function tick(now) {
  requestAnimationFrame(tick);
  if (!paused) sound.pump();
  meter.show(now);
  if (!running || paused) { last = now; return; }
  const period = 1000 / lw.lw_fps();
  spare += now - last;
  last = now;
  if (spare > 250) spare = period;  // back from a hidden tab: don't race to catch up
  let frames = 0;
  while (spare >= period && frames < 2) {
    spare -= period;
    frames++;
    if (!frameOnce()) break;
  }
  if (frames) show();
}

// ---- run controls: pause, a frame at a time, restart; the frame's cost against the budget ------
let paused = false;
function setPaused(p) {
  paused = p;
  $("pause").classList.toggle("paused", p);
  $("pause").setAttribute("aria-label", p ? "Resume" : "Pause");
  $("pause").title = p ? "Resume" : "Pause: stops the frames, the cart keeps its state";
  $("step").disabled = !p;
  if (!p) sound.next = 0;  // the synth picks up where it stopped
}
const meter = {
  ops: 0, max: 0, frames: 0, since: 0, fps: 0, shown: 0,
  count(ops) { this.ops = ops; this.max = Math.max(this.max, ops); this.frames++; },
  reset() { this.ops = this.max = this.frames = 0; this.since = performance.now(); },
  show(now) {  // a few times a second
    if (now - this.shown < 250) return;
    if (now - this.since >= 1000) { this.fps = this.frames * 1000 / (now - this.since); this.frames = 0; this.since = now; }
    this.shown = now;
    const share = Math.min(1, this.ops / 30000);
    $("meter-bar").style.width = (share * 100).toFixed(1) + "%";
    $("meter-bar").classList.toggle("hot", share > 0.8);
    const n = (v) => (v ? v.toLocaleString() : "under 100");  // counted in hundreds
    $("meter-text").textContent = running ? `${n(this.ops)} of 30,000 · ${paused ? "paused" : Math.round(this.fps) + " fps"}` : "stopped";
    $("meter-text").parentElement.title = `Lua instructions this frame: ${n(this.ops)}, at most ${n(this.max)} since it started, of the 30,000 a frame may have`;
  },
};

// ---- Record GIF: 5 s of the screen at 15 fps and half size, for a jam page or a post (web/gif.js) --
const GIF_FPS = 15, GIF_SECONDS = 5;
let recording = false;
async function recordGif() {
  if (recording || !lw) return;
  recording = true;
  const w = Math.round(W / 2), h = Math.round(H / 2), off = document.createElement("canvas");
  off.width = w;
  off.height = h;
  const g = off.getContext("2d", { willReadFrequently: true });
  const frames = [];
  $("gif").disabled = true;
  for (let i = 0; i < GIF_FPS * GIF_SECONDS; i++) {
    g.drawImage(glass, 0, 0, w, h);
    frames.push({ rgba: g.getImageData(0, 0, w, h).data });
    if (i % GIF_FPS === 0) say(`Recording a GIF: ${GIF_SECONDS - i / GIF_FPS} s to go…`);
    await new Promise((r) => setTimeout(r, 1000 / GIF_FPS));
  }
  say("Making the GIF…");
  await new Promise((r) => setTimeout(r, 30));  // let the button say so
  const bytes = Gif.encode(frames, w, h, Math.round(100 / GIF_FPS), lw.colors());
  const name = (plainName(current) || "cart") + ".gif", a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([bytes], { type: "image/gif" }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  say(`Saved ${name}: ${GIF_SECONDS} s, ${Math.round(bytes.length / 1024)} KB.`);
  $("gif").disabled = false;
  recording = false;
}

// ---- the color strip: the cart's 16 colors and their numbers; a click types the number ----------
let stripColors = "";
function colorStrip() {
  const now = lw.colors(), key = now.join();
  if (key === stripColors) return;
  stripColors = key;
  $("color-strip").replaceChildren(...now.map((c, i) => {
    const b = document.createElement("button");
    b.className = "swatch";
    b.style.background = c;
    const n = parseInt(c.slice(1), 16), luma = 0.3 * (n >> 16) + 0.59 * ((n >> 8) & 255) + 0.11 * (n & 255);
    b.style.color = luma > 140 ? "#000" : "#fff";
    b.textContent = i;
    b.title = `color ${i} (${c}): click to type ${i}`;
    b.onclick = () => { cm.replaceSelection(String(i)); cm.focus(); };
    return b;
  }));
}

let engineReady;
const engine = new Promise((r) => { engineReady = r; });
async function select(name, src) {
  await engine;  // a card picked while the engine is still loading waits for it
  current = name;
  resumeOnBack = false;
  showEditor();
  for (const b of $("carts").querySelectorAll("button")) b.classList.toggle("on", b.dataset.name === name);
  $("cart-title").textContent = titleOf(name);
  document.title = titleOf(name) + " · Luanette";
  if (location.hash !== "#" + name) history.replaceState(null, "", "#" + name);
  const example = isShipped(name);
  $("reset").textContent = example ? "Reset to original" : "Delete this cart…";
  $("unsend").textContent = isExample(name) ? "Restore the built-in on the board" : "Remove from board";
  if (src === undefined) {
    if (example) await shippedSource(name);
    src = storeGet(sourceKey(name)) || (example ? originals[name] : TEMPLATE(name));
    if (example && src === originals[name]) storeDel(sourceKey(name));
  }
  code.load(src);
  run(code.value);
  showEdited();
  if (typeof SoundEditor !== "undefined" && SoundEditor.tab !== "code") SoundEditor.show();
}

async function main() {
  renderCarts();
  setUpMenus();
  setUpSplit();
  $("new").onclick = newCart;
  $("home").onclick = () => { if (!browsing()) location.hash = "carts"; };
  $("back").onclick = () => { location.hash = current; };
  $("find").oninput = findCarts;
  $("start-play").onclick = () => $("cards-examples").scrollIntoView({ behavior: "smooth", block: "start" });
  setUpPad();
  $("start-board").onclick = () => {
    if (!current) select(CARTS[0][0]);
    else showEditor();
    setTimeout(() => (($("more").offsetParent ? $("more") : $("menus").querySelector(".menu:last-child > button"))).click(), 0);
  };
  // a .lua file dropped anywhere opens it as a cart of your own
  let drag = 0;
  addEventListener("dragenter", (e) => { if (e.dataTransfer?.types.includes("Files")) { drag++; document.body.classList.add("dropping"); } });
  addEventListener("dragleave", () => { if (--drag <= 0) { drag = 0; document.body.classList.remove("dropping"); } });
  addEventListener("dragover", (e) => { if (e.dataTransfer?.types.includes("Files")) e.preventDefault(); });
  addEventListener("drop", (e) => {
    if (!e.dataTransfer?.files.length) return;
    e.preventDefault();
    drag = 0;
    document.body.classList.remove("dropping");
    openFile(e.dataTransfer.files[0]);
  });
  $("help-btn").onclick = () => $("help").showModal();
  $("run").onclick = () => { sound.unlock(); run(code.value); };
  $("reset").onclick = () => {
    if (!current) return;
    if (!isShipped(current)) { deleteCart(); return; }
    storeDel(sourceKey(current));
    code.value = originals[current];
    run(code.value);
    showEdited();
    renderCarts();
  };
  $("create").onclick = createCart;
  $("newcancel").onclick = () => $("newcart").close();
  $("newname").addEventListener("keydown", (e) => { if (e.key === "Enter") createCart(); });
  $("open").onclick = () => $("file").click();
  $("file").onchange = () => { openFile($("file").files[0]); $("file").value = ""; };
  $("save").onclick = saveFile;
  $("add-lib").onclick = openLibs;
  try {
    const kept = parseFloat(localStorage.getItem("luanette.volume"));
    if (kept >= 0 && kept <= 1) sound.level = kept;
  } catch {}
  const showLevel = () => {
    $("volume").value = Math.round(sound.level * 100);
    $("volume-pct").textContent = sound.level ? Math.round(sound.level * 100) + "%" : "off";
    $("play-mute").classList.toggle("muted", !sound.level);
    $("play-mute").setAttribute("aria-label", sound.level ? "Sound off" : "Sound on");
  };
  showLevel();
  $("volume").oninput = () => { sound.unlock(); sound.setLevel($("volume").value / 100); showLevel(); };
  let unmuted = sound.level || 0.7;
  $("play-mute").onclick = () => {
    sound.unlock();
    if (sound.level) { unmuted = sound.level; sound.setLevel(0); } else sound.setLevel(unmuted);
    showLevel();
  };
  $("share").onclick = () => shareLink(false);
  $("share-play").onclick = () => shareLink(true);
  $("pause").onclick = () => setPaused(!paused);
  $("gif").onclick = recordGif;
  $("step").onclick = () => { if (paused && running && frameOnce()) show(); meter.shown = 0; meter.show(performance.now()); };
  $("restart").onclick = () => { sound.unlock(); run(code.value); };
  setInterval(() => { if (lw && running) colorStrip(); }, 1000);  // palette() may change them as the cart runs
  $("console-clear").onclick = clearLog;
  $("console").addEventListener("toggle", () => { if (!$("console").open) $("console").dataset.closed = "1"; });
  $("submit").onclick = submitToGallery;
  $("sharelink").onfocus = () => $("sharelink").select();
  let saving = 0;  // edits are kept as you type, not only when run
  code.addEventListener("input", () => {
    clearTimeout(saving);
    saving = setTimeout(() => {
      if (!current) return;
      if (isShipped(current) && code.value === originals[current]) storeDel(sourceKey(current));  // back as it ships
      else keep(current, code.value);
      showEdited();
    }, 400);
  });
  // The browser's lists before the engine: with no cart in the address, they're the first thing shown.
  try { GALLERY = await (await fetch("gallery.json")).json(); } catch { GALLERY = []; }  // before a link: it may be one of them
  try { for (const e of await (await fetch("examples.json")).json()) EXAMPLES[e.name] = e; } catch {}
  renderCarts();
  const { h: fromHash, play } = address();
  const opening = fromHash.startsWith("cart=") || isShipped(fromHash) || mine.includes(fromHash);
  if (!opening) showBrowser();
  say("Loading the engine…");
  const mod = await Luanette();
  lw = {
    mod,
    lw_frame: mod.cwrap("lw_frame", "number", ["number", "number", "number", "number", "number", "number"]),
    lw_fps: mod.cwrap("lw_fps", "number", []),
    lw_ops: mod.cwrap("lw_ops", "number", []),
    log_count: mod.cwrap("lw_log_count", "number", []),
    colors: () => mod.ccall("lw_colors", "string", [], []).split(" ").map((h) => "#" + h),
    compile: mod.cwrap("lw_compile", "string", ["string"]),
    log: mod.cwrap("lw_log", "string", []),
    lw_full_canvas: mod.cwrap("lw_full_canvas", "number", []),
    lw_pixels: mod.cwrap("lw_pixels", "number", []),
    lw_sound: mod.cwrap("lw_sound", "number", []),
    lw_pcm: mod.cwrap("lw_pcm", "number", ["number", "number"]),
    lw_pcm_len: mod.cwrap("lw_pcm_len", "number", []),
    error: mod.cwrap("lw_error", "string", []),
    shipped: mod.cwrap("lw_shipped", "string", ["string"]),
    max_source: mod.cwrap("lw_max_source", "number", []),
    audio_start: mod.cwrap("lw_audio_start", null, ["number"]),
    audio_busy: mod.cwrap("lw_audio_busy", "number", []),
    audio_render: mod.cwrap("lw_audio_render", "number", ["number"]),
    audio_stop: mod.cwrap("lw_audio_stop", null, []),
    play_sound: mod.cwrap("lw_play_sound", "string", ["string"]),
    play_song: mod.cwrap("lw_play_song", "string", ["string", "number"]),
    render_audio: mod.cwrap("lw_render_audio", "string", ["string", "number", "number", "number", "number"]),
    pcm_data: mod.cwrap("lw_pcm_data", "number", []),
    ccall: mod.ccall,
  };
  requestAnimationFrame(tick);
  setUpBoard();
  // The address: "#carts" is the cart browser, "#NAME" a cart, "#cart=..." a shared one; Back and
  // Forward move between them, and a link pasted into the address bar opens.
  addEventListener("hashchange", () => {
    const { h, play } = address();
    const then = (opened) => { if (opened && play) enterPlay(); };
    if (h.startsWith("cart=")) openLink(h.slice(5)).then(then);
    else if (h === "carts" || !h) showBrowser();
    else if (h === current) { showEditor(); then(true); }
    else if (isShipped(h) || mine.includes(h)) select(h).then(() => then(true));
  });
  engineReady();
  say("");
  if (fromHash.startsWith("cart=") && await openLink(fromHash.slice(5))) { if (play) enterPlay(); return; }
  if (isShipped(fromHash) || mine.includes(fromHash)) { await select(fromHash); if (play) enterPlay(); }
  else if (!browsing()) showBrowser();  // no cart named (or a link that couldn't be read): the cart browser is the way in
}

main();
