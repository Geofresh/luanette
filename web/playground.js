// The playground: loads the engine (luanette.js / .wasm), runs a cart at its own frame rate, and
// turns the browser's input into the board's: a tap is A and a drag is the d-pad (as the board's
// player does it), keys stand in for the buttons, and a phone's tilt (or Shift+arrows) for tilt().
// Where the browser has Web Serial, it also installs the firmware (ESP Web Tools) and sends the
// cart being edited to a board on USB (board.js).
"use strict";

const CARTS = [
  ["lockload", "Lock & Load"], ["bubbles", "Bubbles"], ["night_swarm", "Night Swarm"],
  ["dice", "Dice"], ["mochi", "Mochi"],
];
const W = 410, H = 502;                  // the glass
const WIN_X = 12, WIN_Y = 60, SCALE = 3;  // where the 128x128 picture sits on it, and its scale
const TAP_MAX_MS = 350, DRAG_PX = 18;     // the board's: a tap lifts this soon without moving this far
const LEFT = 1, RIGHT = 2, UP = 4, DOWN = 8, A = 16, B = 32;

const $ = (id) => document.getElementById(id);
const glass = $("glass"), ctx2d = glass.getContext("2d"), code = $("code"), status = $("status");
const image = ctx2d.createImageData(W, H);

let lw = null;            // the engine's exports
let current = null;       // the example being shown
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
  if (e.target === code) {
    if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) { e.preventDefault(); run(code.value); }
    return;
  }
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
  const [ax, ay] = phoneTilt || keyTilt;
  return [held, touching, tx, ty, ax, ay];
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

// ---- sound: the board's own synth, rendered once per effect --------------------------------
const sound = {
  ctx: null, buffers: [],
  unlock() {
    if (this.ctx || !window.AudioContext) return;
    this.ctx = new AudioContext();
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
    src.connect(this.ctx.destination);
    src.start();
  },
};

// ---- the board: install the firmware, send carts (Web Serial: Chrome and Edge) -------------
const ESPRESSIF = 0x303a;  // the USB vendor of the S3's own port
let sending = false;

// The board's port: the one this page was allowed before, or the one the person picks.
async function boardPort() {
  const known = (await navigator.serial.getPorts()).filter((p) => p.getInfo().usbVendorId === ESPRESSIF);
  return known[0] || navigator.serial.requestPort({ filters: [{ usbVendorId: ESPRESSIF }] });
}

// The cart in the editor, to the board under the example's name (as push.py names a cart by its
// file), so it takes the built-in one's place. Checked first as far as a page can: it runs here,
// has its header, and fits; check.py's instruction budget is the one check it skips.
async function send() {
  if (sending || !current) return;
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
    await Board.sendCart(port, { name: current, title, lua: src });
    say(`${title} is on the board and open there. It stays in its flash, in place of the built-in one.`);
  } catch (e) {
    say(e instanceof Board.BoardError ? e.message
      : `Couldn't use the board's port (${e.message}). Is the installer, another tab or push.py using it?`, true);
  } finally {
    sending = false;
    $("send").disabled = false;
  }
}

async function setUpBoard() {
  if (!("serial" in navigator)) return;
  $("no-serial").hidden = true;
  $("has-serial").hidden = false;
  $("send").hidden = false;
  $("send").onclick = send;
  try {  // the Install button, once there's a release behind it (web/firmware.sh)
    const r = await fetch("firmware/manifest.json");
    if (!r.ok) return;
    const { version } = await r.json();
    await import("https://unpkg.com/esp-web-tools@10.4.0/dist/web/install-button.js?module");
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
  status.textContent = text;
  status.classList.toggle("error", !!error);
}

function run(src) {
  const name = current || "playground";
  const ok = lw.ccall("lw_start", "number", ["string", "string"], [name, src]);
  show();
  if (!ok) { running = false; say(lw.error(), true); return; }
  say("");
  running = true;
  last = performance.now();
  spare = 0;
  try { localStorage.setItem("luanette.draft." + name, src); } catch {}
}

function tick(now) {
  requestAnimationFrame(tick);
  if (!running) return;
  const period = 1000 / lw.lw_fps();
  spare += now - last;
  last = now;
  if (spare > 250) spare = period;  // back from a hidden tab: don't race to catch up
  let frames = 0;
  while (spare >= period && frames < 2) {
    spare -= period;
    frames++;
    const alive = lw.lw_frame(...input());
    for (let n = lw.lw_sound(); n >= 0; n = lw.lw_sound()) sound.play(n);
    if (!alive) {
      running = false;
      const err = lw.error();
      say(err || "The cart ended.", !!err);
      break;
    }
  }
  if (frames) show();
}

async function select(name) {
  current = name;
  for (const b of $("carts").children) b.classList.toggle("on", b.dataset.name === name);
  if (location.hash !== "#" + name) history.replaceState(null, "", "#" + name);
  if (!(name in originals)) {
    const r = await fetch("examples/" + name + ".lua");
    originals[name] = await r.text();
  }
  let draft = null;
  try { draft = localStorage.getItem("luanette.draft." + name); } catch {}
  code.value = draft || originals[name];
  run(code.value);
}

async function main() {
  for (const [name, title] of CARTS) {
    const b = document.createElement("button");
    b.textContent = title;
    b.dataset.name = name;
    b.onclick = () => select(name);
    $("carts").appendChild(b);
  }
  $("run").onclick = () => run(code.value);
  $("reset").onclick = () => {
    if (!current) return;
    try { localStorage.removeItem("luanette.draft." + current); } catch {}
    code.value = originals[current];
    run(code.value);
  };
  code.addEventListener("keydown", (e) => {  // Tab indents instead of leaving the editor
    if (e.key !== "Tab" || e.shiftKey) return;
    e.preventDefault();
    code.setRangeText("  ", code.selectionStart, code.selectionEnd, "end");
  });
  say("Loading the engine…");
  const mod = await Luanette();
  lw = {
    mod,
    lw_frame: mod.cwrap("lw_frame", "number", ["number", "number", "number", "number", "number", "number"]),
    lw_fps: mod.cwrap("lw_fps", "number", []),
    lw_full_canvas: mod.cwrap("lw_full_canvas", "number", []),
    lw_pixels: mod.cwrap("lw_pixels", "number", []),
    lw_sound: mod.cwrap("lw_sound", "number", []),
    lw_pcm: mod.cwrap("lw_pcm", "number", ["number", "number"]),
    lw_pcm_len: mod.cwrap("lw_pcm_len", "number", []),
    error: mod.cwrap("lw_error", "string", []),
    shipped: mod.cwrap("lw_shipped", "string", ["string"]),
    max_source: mod.cwrap("lw_max_source", "number", []),
    ccall: mod.ccall,
  };
  requestAnimationFrame(tick);
  setUpBoard();
  const fromHash = location.hash.slice(1);
  select(CARTS.some(([n]) => n === fromHash) ? fromHash : CARTS[0][0]);
}

main();
