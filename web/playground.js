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
const storeGet = (k) => { try { return localStorage.getItem(k); } catch { return null; } };
const storeSet = (k, v) => { try { localStorage.setItem(k, v); return true; } catch { return false; } };
const storeDel = (k) => { try { localStorage.removeItem(k); } catch {} };
// An example's edits are a draft over the original; your own cart's source is all there is.
const sourceKey = (name) => (isExample(name) ? "luanette.draft." : "luanette.cart.") + name;

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
  const kept = saveMine() && storeSet(sourceKey(name), src);
  renderCarts();
  select(name, src);
  if (!kept) say("This browser isn't keeping data for this page, so the cart goes when the tab does. Save .lua keeps a copy.", true);
}

function newCart() {
  $("newcart").hidden = false;
  $("newname").value = "";
  $("newname").focus();
  say("");
}
function createCart() {
  const name = $("newname").value.trim().toLowerCase();
  if (mine.includes(name)) { say(`You already have ${name}.`, true); return; }
  const problem = nameProblem(name);
  if (problem) { say(problem, true); return; }
  $("newcart").hidden = true;
  addCart(name, TEMPLATE(name));
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
  a.download = current + ".lua";
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

function deleteCart() {
  if (!confirm(`Delete ${current} from this browser? Save .lua first if you want to keep a copy.`)) return;
  storeDel(sourceKey(current));
  mine = mine.filter((n) => n !== current);
  saveMine();
  renderCarts();
  select(CARTS[0][0]);
}

function renderCarts() {
  const row = $("carts");
  row.replaceChildren();
  const add = (name, title, cls) => {
    const b = document.createElement("button");
    b.textContent = title;
    b.dataset.name = name;
    if (cls) b.classList.add(cls);
    b.onclick = () => select(name);
    row.appendChild(b);
  };
  for (const [name, title] of CARTS) add(name, title);
  for (const name of mine) add(name, name, "mine");
  const b = document.createElement("button");
  b.textContent = "+ New cart";
  b.onclick = newCart;
  row.appendChild(b);
  for (const c of row.children) c.classList.toggle("on", c.dataset.name === current);
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
    await Board.removeCart(port, current, isExample(current));
    say(isExample(current) ? `The board has its built-in ${current} again.` : `${current} is off the board.`);
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
  storeSet(sourceKey(name), src);
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

async function select(name, src) {
  current = name;
  for (const b of $("carts").children) b.classList.toggle("on", b.dataset.name === name);
  if (location.hash !== "#" + name) history.replaceState(null, "", "#" + name);
  const example = isExample(name);
  $("reset").textContent = example ? "Reset to original" : "Delete";
  $("unsend").textContent = example ? "Restore the built-in on the board" : "Remove from board";
  $("newcart").hidden = true;
  if (src === undefined) {
    if (example && !(name in originals)) {
      const r = await fetch("examples/" + name + ".lua");
      originals[name] = await r.text();
    }
    src = storeGet(sourceKey(name)) || (example ? originals[name] : TEMPLATE(name));
  }
  code.value = src;
  run(code.value);
}

async function main() {
  renderCarts();
  $("run").onclick = () => run(code.value);
  $("reset").onclick = () => {
    if (!current) return;
    if (!isExample(current)) { deleteCart(); return; }
    storeDel(sourceKey(current));
    code.value = originals[current];
    run(code.value);
  };
  $("create").onclick = createCart;
  $("newcancel").onclick = () => { $("newcart").hidden = true; };
  $("newname").addEventListener("keydown", (e) => {
    if (e.key === "Enter") createCart();
    if (e.key === "Escape") $("newcart").hidden = true;
  });
  $("open").onclick = () => $("file").click();
  $("file").onchange = () => { openFile($("file").files[0]); $("file").value = ""; };
  $("save").onclick = saveFile;
  let saving = 0;  // edits are kept as you type, not only when run
  code.addEventListener("input", () => {
    clearTimeout(saving);
    saving = setTimeout(() => current && storeSet(sourceKey(current), code.value), 400);
  });
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
  select(isExample(fromHash) || mine.includes(fromHash) ? fromHash : CARTS[0][0]);
}

main();
