#!/usr/bin/env node
// Luanette's command-line tool: the console's own engine (WebAssembly, the playground's) in Node,
// with nothing to build or install. For people and their coding agents:
//
//   luanette check CART.lua [--frames N] [--input FILE]     what check.py says, as JSON
//   luanette run CART.lua [--frames N] [--input FILE] [--shots 1,30,90] [--out DIR] [--gif]
//   luanette sound "C4 pulse 5, E4 slide, -, G4 saw 7 fadeout" [--speed N] [--loop A-B]
//   luanette sound CART.lua N --wav OUT.wav                 a cart's sound, as a WAV
//   luanette song CART.lua [--from N] --wav OUT.wav         its song
//   luanette link CART.lua [--name NAME] [--play]            a playground share link (--play: opens in play mode)
//   luanette new NAME [--template blank|platformer|shooter|puzzle]
//   luanette mcp                                             an MCP server over stdio (for agents)
//
// docs/cli.md has the details and the input script's format.
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import { pyRandom } from "./pyrandom.mjs";

const require = createRequire(import.meta.url);
const HERE = path.dirname(fileURLToPath(import.meta.url));
// The package carries its parts beside this file; in the repository they're where the build leaves them.
const find = (...tries) => tries.map((t) => path.join(HERE, t)).find((p) => fs.existsSync(p));
const ENGINE = find("engine/luanette-node.js", "../web/build/luanette-node.js");
const LIB = find("lib", "../web");
const TEMPLATES = find("templates", "../templates");
export const PLAYGROUND = "https://geofresh.github.io/luanette/";
const Gif = require(path.join(LIB, "gif.js"));
const Tables = require(path.join(LIB, "tables.js"));

export const BUDGET = 30000, MAX_SOURCE = 32 * 1024, API_VERSION = 4;
const W = 410, H = 502;  // the glass
const BUTTONS = { left: 1, right: 2, up: 4, down: 8, a: 16, b: 32 };

// ---- the engine ------------------------------------------------------------------------------------
let engine = null;
export async function loadEngine() {
  if (engine) return engine;
  if (!ENGINE) throw new Error("the engine isn't built: run web/build.sh (in the repository), or install the npm package");
  const m = await require(ENGINE)();
  const c = (n, r, a) => m.cwrap(n, r, a);
  engine = {
    m,
    start: c("lw_start", "number", ["string", "string"]),
    frame: c("lw_frame", "number", ["number", "number", "number", "number", "number", "number"]),
    error: c("lw_error", "string", []),
    ops: c("lw_ops", "number", []),
    fps: c("lw_fps", "number", []),
    lazy: c("lw_lazy", "number", []),
    pixels: c("lw_pixels", "number", []),
    logCount: c("lw_log_count", "number", []),
    log: c("lw_log", "string", []),
    sound: c("lw_sound", "number", []),
    shipped: c("lw_shipped", "string", ["string"]),
    clock: c("lw_clock", null, ["number"]),
    compile: c("lw_compile", "string", ["string"]),
    renderAudio: c("lw_render_audio", "string", ["string", "number", "number", "number", "number"]),
    pcmData: c("lw_pcm_data", "number", []),
    pcmLen: c("lw_pcm_len", "number", []),
    colors: () => m.ccall("lw_colors", "string", [], []).split(" ").map((h) => "#" + h),
    glass: () => new Uint8Array(m.HEAPU8.buffer, engine.pixels(), W * H * 4),
  };
  return engine;
}

// ---- input: check.py's script, or yours ------------------------------------------------------------
// check.py's (tools/check.py script_input), the same each run: buttons on a 120-frame cycle, A now and
// then at random (Python's own generator, seeded 7), a tap and a drag, a slow tilt.
function checkInput() {
  const rng = pyRandom(7);
  return (f) => {
    const phase = f % 120;
    let held = 0;
    [[40, 52], [8, 20], [56, 68], [24, 36]].forEach(([a, z], b) => { if (a <= phase && phase < z) held |= 1 << b; });
    if ([5, 6, 30, 31, 61, 62, 90, 91].includes(phase) || rng() < 0.03) held |= BUTTONS.a;  // (as Python: rng only when needed)
    if ([75, 76, 105, 106].includes(phase)) held |= BUTTONS.b;
    let touch = null;
    if (phase >= 70 && phase < 73) touch = [64, 64];
    else if (phase >= 80 && phase < 100) touch = [64, 110 - (phase - 80) * 4];
    const r = (v) => Math.round(v * 1000) / 1000;
    return { held, touch, tilt: [r(Math.sin(f / 20) * 0.6), r(Math.cos(f / 25) * 0.6)] };
  };
}

// An input script (docs/cli.md): a list of steps, each for `frame` N or `frames` "A-B":
//   {"hold": ["right", "a"]}  buttons down     {"touch": [x, y]}  a finger there (picture pixels)
//   {"tap": [x, y]}  touch, then lift: A on the frame it lifts (as the board does)
//   {"drag": [[x0, y0], [x1, y1]]}  a finger moving: the d-pad from its direction, as the board does
//   {"tilt": [ax, ay]}  each -1 to 1
export function scriptInput(steps) {
  if (!Array.isArray(steps)) throw new Error("an input script is a JSON list of steps");
  const span = (s) => {
    if (s.frame !== undefined) return [Number(s.frame), Number(s.frame)];
    const m = /^(\d+)\s*-\s*(\d+)$/.exec(String(s.frames || ""));
    if (!m) throw new Error(`a step needs "frame": N or "frames": "A-B": ${JSON.stringify(s)}`);
    return [Number(m[1]), Number(m[2])];
  };
  const bits = (names) => (names || []).reduce((b, n) => {
    if (!(n in BUTTONS)) throw new Error(`a button is left, right, up, down, a or b, not ${n}`);
    return b | BUTTONS[n];
  }, 0);
  return (f) => {
    let held = 0, touch = null, tilt = [0, 0];
    for (const s of steps) {
      const [a, z] = span(s);
      if (s.tap && f >= a && f <= a + 2) {  // down for two frames, then lifted: A for that frame
        if (f < a + 2) touch = s.tap; else held |= BUTTONS.a;
      }
      if (f < a || f > z) continue;
      held |= bits(s.hold || s.press);
      if (s.touch) touch = s.touch;
      if (s.tilt) tilt = s.tilt;
      if (s.drag) {
        const t = z > a ? (f - a) / (z - a) : 1, [[x0, y0], [x1, y1]] = s.drag;
        touch = [x0 + (x1 - x0) * t, y0 + (y1 - y0) * t];
        const dx = touch[0] - x0, dy = touch[1] - y0;
        if (dx * dx + dy * dy > 36) {  // past 6 picture pixels (the board's 18 on the glass): the d-pad
          const ax = Math.abs(dx), ay = Math.abs(dy);
          if (ax * 5 > ay * 2) held |= dx < 0 ? BUTTONS.left : BUTTONS.right;
          if (ay * 5 > ax * 2) held |= dy < 0 ? BUTTONS.up : BUTTONS.down;
        }
      }
    }
    return { held, touch, tilt };
  };
}

// ---- running a cart --------------------------------------------------------------------------------
const strip = (e) => e.split("\nstack traceback")[0].trim();
// The runtime's words, as check.py puts them.
function startError(e) {
  e = strip(e);
  const m = /^in (the top of the cart|_init): ([^]*)$/.exec(e);
  return m ? `error in ${m[1]}: ${m[2]}` : e;
}

// Runs `frames` frames of the cart, its clock a frame's time each (as check.py's and cartrun's), and
// calls onFrame(f, input) after each. {error?, frame?, log: [{frame, line}], ops: [...], fps}
export async function runCart(src, name, frames, input, onFrame) {
  const e = await loadEngine();
  const log = [], ops = [];
  const drain = (f) => { for (let n = e.logCount(); n > 0; n--) log.push({ frame: f, line: e.log() }); };
  e.clock(0);
  if (!e.start(name, src)) {
    drain(0);
    return { error: startError(e.error()), log, ops, fps: e.fps() };
  }
  drain(0);
  let baseUs = 0, baseN = 0, rate = e.fps(), cur = 0;
  const timeUs = () => baseUs + Math.floor((cur - baseN) * 1e6 / rate);
  for (let f = 1; f <= frames; f++) {
    if (e.fps() !== rate) { baseUs = timeUs(); baseN = f - 1; rate = e.fps(); }
    cur = f;
    e.clock(timeUs());
    const i = input(f);
    const alive = e.frame(i.held, i.touch ? 1 : 0, i.touch ? i.touch[0] : 0, i.touch ? i.touch[1] : 0, i.tilt[0], i.tilt[1]);
    drain(f);
    while (e.sound() >= 0);  // (sounds aren't heard here)
    ops.push(e.ops());
    if (!alive) {
      return { error: `error in frame ${f}: ` + strip(e.error()).replace(/^in (_update60|_update|_draw): /, ""), frame: f, log, ops, fps: rate };
    }
    if (onFrame) onFrame(f, i);
  }
  return { log, ops, fps: rate };
}

// ---- check -----------------------------------------------------------------------------------------
// check.py's checks and words (tools/check.py check()), through the console's own engine. Its warnings
// are a subset of check.py's (docs/cli.md lists them).
export async function check(src, name, opts = {}) {
  const e = await loadEngine();
  const frames = opts.frames || 150;
  const shipped = e.shipped(src);
  const res = { ok: false, cart: opts.file || name, bytes: Buffer.byteLength(shipped), source_bytes: Buffer.byteLength(src) };
  if (res.bytes > MAX_SOURCE) return { ...res, error: `cart is ${res.bytes} bytes without its comments; the limit is ${MAX_SOURCE}` };
  const head = shipped.split("\n").slice(0, 12).join("\n");
  const missing = ["title", "by", "about"].filter((k) => !new RegExp(`^--\\s*${k}\\s*:\\s*\\S`, "m").test(head));
  if (missing.length) return { ...res, error: "missing header comment(s) at the top: " + missing.map((k) => `-- ${k}: ...`).join(", ") };
  res.title = /^--\s*title\s*:\s*(.+)$/m.exec(head)[1].trim();
  let api = 1, m = /^--\s*api\s*:\s*(.*?)\s*$/m.exec(head);
  if (m) {
    if (!/^\d+$/.test(m[1]) || Number(m[1]) < 1) return { ...res, error: `its header says '-- api: ${m[1]}'; that should be a whole number (1 to ${API_VERSION})` };
    api = Number(m[1]);
    if (api > API_VERSION) return { ...res, error: `it needs API ${api}; the board's runtime has ${API_VERSION}` };
  }
  res.api = api;
  let full = false;
  m = /^--\s*canvas\s*:\s*(.*?)\s*$/m.exec(head);
  if (m) {
    if (m[1] !== "full" && m[1] !== "square") return { ...res, error: `its header says '-- canvas: ${m[1]}'; that should be full or square` };
    full = m[1] === "full";
  }
  if (full) res.canvas = "full";
  const compiled = e.compile(shipped);
  if (compiled) return { ...res, error: `doesn't load: ${compiled}` };
  const code = shipped.replace(/--[^\n]*/g, "");
  if (!/\b_(update60|update|draw)\b/.test(code)) return { ...res, error: "the cart defines none of _update, _update60, _draw" };

  let quiet = 0, idle = 0, prev = null, last = null;
  const hash = (px) => { let h = 2166136261; const u = new Uint32Array(px.buffer, px.byteOffset, px.length / 4); for (let i = 0; i < u.length; i++) h = Math.imul(h ^ u[i], 16777619); return h >>> 0; };
  const input = opts.input || checkInput();
  const run = await runCart(shipped, name, frames, input, (f, i) => {
    const px = e.glass(), h = hash(px);
    if (e.fps() >= 20 && !e.lazy() && !i.touch && !i.held) {  // (as check.py: a lazy cart has an idle rate)
      quiet++;
      if (h === prev) idle++;
    }
    prev = h;
    last = px;
  });
  if (run.log.length) res.log = run.log.map((l) => l.line);
  if (run.error) return { ...res, error: run.error, ...(run.frame ? { frame: run.frame } : {}) };
  const ops = run.ops;
  Object.assign(res, { ok: true, frames, fps: run.fps, max_ops: Math.max(0, ...ops), avg_ops: Math.round(ops.reduce((a, b) => a + b, 0) / frames) });
  const warn = (w) => (res.warnings = res.warnings || []).push(w);
  if (api < 2 && /\bui\s*\./.test(code)) warn("it uses the runtime's ui helpers (API 2): add '-- api: 2' to its header, so a board with older firmware says it needs an update instead of failing at its first ui call");
  else if (api < 2 && /\bhd\s*\./.test(code)) warn("it uses the full-color layer (API 2): add '-- api: 2' to its header, so a board with older firmware says it needs an update instead of failing at its first hd call");
  if (api < 4 && /^\s*\[?\d*\]?\s*=?\s*"[0-9a-fA-F]+\s+[0-9a-fA-F]/m.test(code.split(/^SPRITES\s*=/m)[1] || "")) warn("its sprites have spaces between rows (API 4): add '-- api: 4' to its header, so a board with older firmware says it needs an update instead of failing to draw them");
  if (api < 4 && run.log.length) warn("it calls log() (API 4): add '-- api: 4' to its header, so a board with older firmware says it needs an update instead of failing at its first log call");
  if (api < 3 && /\bmusic\s*\(|^\s*(SOUNDS|MUSIC)\s*=/m.test(code)) warn("it has its own sounds or music (API 3): add '-- api: 3' to its header, so a board with older firmware says it needs an update instead of failing at its first sfx or music call");
  if (!full) warn("it shows in the 384 x 384 window, about 30% of the glass left dark: add '-- canvas: full' (and '-- api: 2') and use the whole glass, laid out from caps(): the picture keeps its window, and the layer has the bands above and below it for a title, the score, what to do (docs/DESIGN.md, 1)");
  if (quiet >= 20 && idle >= quiet * 0.6 && !/\bhd\s*\./.test(code)) warn(`${idle} of its ${quiet} frames without input (at ${run.fps} fps) changed nothing on the screen: give it a lazy rate, fps(60, idle) in _init (60 while touched, a few frames a second otherwise), and move things by time() (docs/DESIGN.md, 2 and 3)`);
  if (last && new Uint32Array(last.buffer, last.byteOffset, last.length / 4).every((v, i, a) => v === a[0])) warn("the last frame is a single color: does _draw draw anything?");
  return res;
}

// ---- images out: PNG and GIF -----------------------------------------------------------------------
const CRC = new Uint32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c; });
const crc32 = (buf) => { let c = 0xffffffff; for (const b of buf) c = CRC[(c ^ b) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
export function png(w, h, rgba) {
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  const chunk = (type, data) => {
    const len = Buffer.alloc(4), crc = Buffer.alloc(4), body = Buffer.concat([Buffer.from(type), data]);
    len.writeUInt32BE(data.length);
    crc.writeUInt32BE(crc32(body));
    return Buffer.concat([len, body, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", zlib.deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
}
const half = (rgba, w, h) => {  // a 2x2 average
  const w2 = w >> 1, h2 = h >> 1, out = new Uint8Array(w2 * h2 * 4);
  for (let y = 0; y < h2; y++) for (let x = 0; x < w2; x++) for (let c = 0; c < 4; c++) {
    const i = ((2 * y) * w + 2 * x) * 4 + c;
    out[(y * w2 + x) * 4 + c] = (rgba[i] + rgba[i + 4] + rgba[i + w * 4] + rgba[i + w * 4 + 4] + 2) >> 2;
  }
  return out;
};

// ---- run: frames to look at ------------------------------------------------------------------------
export async function run(src, name, opts = {}) {
  const e = await loadEngine();
  const frames = opts.frames || 90;
  const shots = new Set(opts.shots || [1, Math.max(1, frames >> 1), frames]);
  const out = opts.out || "luanette-run";
  fs.mkdirSync(out, { recursive: true });
  const saved = [], images = [], gif = [];
  const res = await runCart(src, name, frames, opts.input || scriptInput([]), (f) => {
    const px = e.glass();
    if (shots.has(f)) {
      const file = path.join(out, `frame-${f}.png`), copy = px.slice();
      fs.writeFileSync(file, png(W, H, copy));
      saved.push(file);
      images.push({ frame: f, rgba: copy });
    }
    if (opts.gif && f % 2 === 0) gif.push({ rgba: half(px, W, H) });
  });
  const result = { ok: !res.error, frames, fps: res.fps, shots: saved, log: res.log, max_ops: Math.max(0, ...res.ops), ops: res.ops };
  if (res.error) Object.assign(result, { error: res.error, frame: res.frame });
  if (images.length > 1) {  // the shots side by side at half size
    const w2 = W >> 1, h2 = H >> 1, gap = 8, sw = images.length * (w2 + gap) + gap, sh = h2 + 2 * gap;
    const sheet = new Uint8Array(sw * sh * 4).fill(24);
    for (let i = 3; i < sheet.length; i += 4) sheet[i] = 255;
    images.forEach((im, k) => {
      const small = half(im.rgba, W, H);
      for (let y = 0; y < h2; y++) sheet.set(small.subarray(y * w2 * 4, (y + 1) * w2 * 4), ((y + gap) * sw + gap + k * (w2 + gap)) * 4);
    });
    result.sheet = path.join(out, "shots.png");
    fs.writeFileSync(result.sheet, png(sw, sh, sheet));
  }
  if (gif.length) {
    result.gif = path.join(out, "run.gif");
    fs.writeFileSync(result.gif, Gif.encode(gif, W >> 1, H >> 1, Math.round(200 / res.fps), e.colors()));
  }
  return result;
}

// ---- sounds: readable notes to SOUNDS text, and WAVs -----------------------------------------------
const NOTE = { c: 0, d: 2, e: 4, f: 5, g: 7, a: 9, b: 11 };
const INSTRUMENTS = { square: 0, sq: 0, pulse: 1, thin: 2, "thin-pulse": 2, triangle: 3, tri: 3, saw: 4, sine: 5, organ: 6, noise: 7 };
const EFFECTS = { none: 0, slide: 1, vibrato: 2, vib: 2, drop: 3, fadein: 4, "fade-in": 4, fadeout: 5, "fade-out": 5, arp: 6, arpfast: 6, "arp-fast": 6, arpslow: 7, "arp-slow": 7 };
const hex2 = (n) => n.toString(16).padStart(2, "0");
// "C4 pulse 5, E4 slide, -, G4 saw 7 fadeout": a note is a pitch (C2 to D#7, # or b), then any of an
// instrument, a volume (0-7) and an effect; one left out is the note before's (to start: square, 5,
// none). "-" is a rest. The result is SOUNDS text (engine/synth.hpp).
export function notesToSound(text, speed = 8, loop = null) {
  let inst = 0, vol = 5;
  const notes = text.split(",").map((s) => s.trim()).filter(Boolean).map((s) => {
    const t = s.toLowerCase().split(/\s+/);
    if (t[0] === "-" || t[0] === "rest") return [0, inst, 0, 0];
    const m = /^([a-g])(#|b)?(\d)$/.exec(t[0]);
    if (!m) throw new Error(`a note starts with its pitch, like C4 or F#5: ${s}`);
    const pitch = NOTE[m[1]] + (m[2] === "#" ? 1 : m[2] === "b" ? -1 : 0) + (Number(m[3]) - 2) * 12;
    if (pitch < 0 || pitch > 63) throw new Error(`${t[0]} is outside C2 to D#7`);
    let fx = 0;
    for (const w of t.slice(1)) {
      if (w in INSTRUMENTS) inst = INSTRUMENTS[w];
      else if (/^[0-7]$/.test(w)) vol = Number(w);
      else if (w in EFFECTS) fx = EFFECTS[w];
      else throw new Error(`"${w}" isn't an instrument (${Object.keys(INSTRUMENTS).join(", ")}), a volume (0-7) or an effect (${Object.keys(EFFECTS).join(", ")})`);
    }
    return [pitch, inst, vol, fx];
  });
  if (!notes.length || notes.length > 32) throw new Error("a sound is 1 to 32 notes");
  if (!(speed >= 1 && speed <= 255)) throw new Error("the speed is 1 to 255 (ticks of 1/120 s a note)");
  const [ls, le] = loop || [0, 0];
  return `${hex2(speed)} ${hex2(ls)}${hex2(le)} ` + notes.map(([p, i, v, e]) => hex2(p) + i.toString(16) + v.toString(16) + e.toString(16)).join(" ");
}

function decodeSound(text) {
  const d = text.replace(/\s+/g, "");
  return { speed: parseInt(d.slice(0, 2), 16), ls: parseInt(d.slice(2, 4), 16), le: parseInt(d.slice(4, 6), 16), notes: (d.length - 6) / 5 };
}
const soundSeconds = (s) => ((s.le > s.ls ? s.le + (s.le - s.ls) : s.notes) * s.speed) / 120;
function songSeconds(music, sounds, from) {  // to its end, or round its loop once more (the editor's rule)
  let n = from, loopTo = -1, loops = 0, t = 0;
  for (let guard = 0; guard < 10000 && t < 180; guard++) {
    if (!music.has(n)) break;
    const d = music.get(n).text.replace(/\s+/g, ""), flags = parseInt(d.slice(0, 2), 16);
    let ticks = 0;
    for (let c = 0; c < 4; c++) {
      const k = d.slice(2 + c * 2, 4 + c * 2);
      if (k !== "--" && sounds.has(parseInt(k, 16))) { const s = decodeSound(sounds.get(parseInt(k, 16)).text); ticks = Math.max(ticks, s.notes * s.speed); }
    }
    if (flags & 1) loopTo = n;
    t += ticks ? ticks / 120 : 0.25;
    if (flags & 4) break;
    if ((flags & 2) && loopTo >= 0) { if (++loops > 1) break; n = loopTo; } else n++;
  }
  return Math.min(t, 180);
}
export function wav(pcm, rate) {
  const b = Buffer.alloc(44 + pcm.length * 2);
  b.write("RIFF", 0); b.writeUInt32LE(36 + pcm.length * 2, 4); b.write("WAVE", 8); b.write("fmt ", 12);
  b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22); b.writeUInt32LE(rate, 24);
  b.writeUInt32LE(rate * 2, 28); b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34); b.write("data", 36); b.writeUInt32LE(pcm.length * 2, 40);
  for (let i = 0; i < pcm.length; i++) b.writeInt16LE(pcm[i], 44 + i * 2);
  return b;
}
// A cart's sound n, or its song (n null, from pattern `from`), as 16-bit samples at `rate`.
export async function renderAudio(src, n, from = null, rate = 44100) {
  const e = await loadEngine();
  const sounds = Tables.readTable(src, "SOUNDS"), music = Tables.readTable(src, "MUSIC");
  if (sounds.problem || music.problem) throw new Error(sounds.problem || music.problem);
  let why, secs;
  if (n !== null) {
    if (!sounds.map.has(n)) throw new Error(`the cart has no SOUNDS[${n}]`);
    secs = soundSeconds(decodeSound(sounds.map.get(n).text));
    why = e.renderAudio(sounds.map.get(n).text, 0, 0, rate, secs);
  } else {
    if (!music.map.size) throw new Error("the cart has no MUSIC");
    const start = from ?? Math.min(...music.map.keys()), json = { music: {}, sounds: {} };
    for (const [k, v] of music.map) json.music[k] = v.text;
    for (const [k, v] of sounds.map) json.sounds[k] = v.text;
    secs = songSeconds(music.map, sounds.map, start);
    why = e.renderAudio(JSON.stringify(json), 1, start, rate, secs);
  }
  if (why) throw new Error(why);
  return { pcm: new Int16Array(e.m.HEAP16.buffer, e.pcmData(), e.pcmLen()).slice(), rate, seconds: e.pcmLen() / rate };
}

// ---- links and new carts ---------------------------------------------------------------------------
// "&play" after it opens the cart in play mode: the screen alone, with buttons on a phone.
export function shareLink(src, name, base = PLAYGROUND, play = false) {
  const data = zlib.deflateRawSync(Buffer.from(JSON.stringify({ name, src })), { level: 9 });
  return base + "#cart=d" + data.toString("base64url") + (play ? "&play" : "");
}
export function newCart(name, template = "blank") {
  if (!/^[a-z0-9_-]{1,32}$/.test(name)) throw new Error("a cart's name is 1 to 32 of a-z, 0-9, _ and -");
  const file = path.join(TEMPLATES, template + ".lua");
  if (!fs.existsSync(file)) throw new Error(`no template ${template}: ${fs.readdirSync(TEMPLATES).map((f) => f.replace(".lua", "")).join(", ")}`);
  return fs.readFileSync(file, "utf8").replace(/^-- title: .*$/m, "-- title: " + name);
}
export const templates = () => fs.readdirSync(TEMPLATES).filter((f) => f.endsWith(".lua")).map((f) => f.replace(".lua", ""));

// ---- the command line ------------------------------------------------------------------------------
function args(list) {
  const pos = [], opt = {};
  for (let i = 0; i < list.length; i++) {
    const a = list[i];
    if (a.startsWith("--")) {
      const k = a.slice(2);
      opt[k] = i + 1 < list.length && !list[i + 1].startsWith("--") ? list[++i] : true;
    } else pos.push(a);
  }
  return { pos, opt };
}
const USAGE = fs.readFileSync(fileURLToPath(import.meta.url), "utf8").split("\n").slice(1, 15).map((l) => l.replace(/^\/\/ ?/, "")).join("\n");
const readCart = (file) => {
  if (!file || !fs.existsSync(file)) throw new Error(`no cart ${file || "given"}`);
  return { src: fs.readFileSync(file, "utf8"), name: path.basename(file, ".lua") };
};
const readInput = (file) => (file ? scriptInput(JSON.parse(fs.readFileSync(file, "utf8"))) : null);
const print = (o) => process.stdout.write(JSON.stringify(o, null, 1) + "\n");

async function main() {
  const [cmd, ...rest] = process.argv.slice(2);
  const { pos, opt } = args(rest);
  if (cmd === "check") {
    const { src, name } = readCart(pos[0]);
    const res = await check(src, name, { file: pos[0], frames: Number(opt.frames) || 150, input: readInput(opt.input) });
    print(res);
    process.exitCode = res.ok ? 0 : 1;
  } else if (cmd === "run") {
    const { src, name } = readCart(pos[0]);
    const shots = opt.shots ? String(opt.shots).split(",").map(Number) : null;
    const res = await run(src, name, { frames: Number(opt.frames) || 90, input: readInput(opt.input), shots, out: opt.out, gif: !!opt.gif });
    print(res.ops.length > 20 ? { ...res, ops: undefined } : res);
    process.exitCode = res.ok ? 0 : 1;
  } else if (cmd === "sound" && pos[0] && pos[0].endsWith(".lua")) {
    const { src } = readCart(pos[0]);
    const r = await renderAudio(src, Number(pos[1]), null);
    fs.writeFileSync(opt.wav || `sound-${pos[1]}.wav`, wav(r.pcm, r.rate));
    print({ ok: true, wav: opt.wav || `sound-${pos[1]}.wav`, seconds: Number(r.seconds.toFixed(2)) });
  } else if (cmd === "sound") {
    const loop = opt.loop ? String(opt.loop).split("-").map(Number) : null;
    process.stdout.write(notesToSound(pos.join(" "), Number(opt.speed) || 8, loop) + "\n");
  } else if (cmd === "song") {
    const { src } = readCart(pos[0]);
    const r = await renderAudio(src, null, opt.from !== undefined ? Number(opt.from) : null);
    fs.writeFileSync(opt.wav || "song.wav", wav(r.pcm, r.rate));
    print({ ok: true, wav: opt.wav || "song.wav", seconds: Number(r.seconds.toFixed(2)) });
  } else if (cmd === "link") {
    const { src, name } = readCart(pos[0]);
    process.stdout.write(shareLink(src, opt.name || name, opt.base || PLAYGROUND, opt.play === true) + "\n");
  } else if (cmd === "new") {
    const name = pos[0], file = opt.out || `${name}.lua`;
    if (fs.existsSync(file)) throw new Error(`${file} is there already`);
    fs.writeFileSync(file, newCart(name, opt.template || "blank"));
    process.stdout.write(`wrote ${file} (from the ${opt.template || "blank"} template)\n`);
  } else if (cmd === "mcp") {
    await (await import("./mcp.mjs")).serve();
  } else {
    process.stdout.write(USAGE + "\n");
    process.exitCode = cmd && cmd !== "help" && cmd !== "--help" ? 1 : 0;
  }
}

if (process.argv[1] && fs.realpathSync(process.argv[1]) === fileURLToPath(import.meta.url)) {
  process.stdout.on("error", (err) => { if (err.code === "EPIPE") process.exit(0); throw err; });  // `| head` closed it: fine
  main().catch((err) => { process.stderr.write(`luanette: ${err.message}\n`); process.exitCode = 1; });
}
