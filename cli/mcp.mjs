// `luanette mcp`: the command-line tool as an MCP server over stdio, for coding agents. JSON-RPC 2.0, a
// message a line (the MCP stdio transport), no dependencies. Its tools check a cart, run it with input
// and hand back what it drew as images, describe and render its sounds, make a share link, look up the
// API, and add libraries. Requests are handled one at a time (the engine is one).
//
//   claude mcp add luanette -- npx -y luanette mcp
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import readline from "node:readline";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import * as L from "./luanette.mjs";

const require = createRequire(import.meta.url);
const HERE = path.dirname(fileURLToPath(import.meta.url));
const VERSION = JSON.parse(fs.readFileSync(path.join(HERE, "package.json"), "utf8")).version;
const API = (() => {
  const file = [path.join(HERE, "api.json"), path.join(HERE, "../web/build/site/api.json")].find((p) => fs.existsSync(p));
  return file ? JSON.parse(fs.readFileSync(file, "utf8")) : [];
})();

const CART = {
  source: { type: "string", description: "The cart's Lua source (or give path)" },
  path: { type: "string", description: "A .lua file to read instead of source" },
};
const INPUT = {
  type: "array",
  description: 'Input by frame, as docs/cli.md: [{"frames":"1-30","hold":["right"]},{"frame":40,"tap":[64,100]},' +
    '{"frames":"50-80","drag":[[64,110],[64,40]]},{"frames":"1-200","tilt":[0.4,0]}]. Buttons: left, right, up, down, a, b. ' +
    "Positions are picture pixels, 0-127. A tap is A on the frame it lifts; a drag is the d-pad.",
  items: { type: "object" },
};
const TOOLS = [
  { name: "check_cart", description: "Checks a Luanette cart as tools/check.py does: size, header, that it loads and runs 150 frames with scripted input inside the 30,000-instruction frame budget; errors with their line, warnings, the cart's log(). Run this after every change.",
    inputSchema: { type: "object", properties: { ...CART, frames: { type: "integer", description: "Frames to run (150)" }, input: INPUT } } },
  { name: "run_cart", description: "Runs a cart with input you choose and returns what the screen showed at chosen frames (images), the log() lines by frame, and each frame's cost. Use it to see your cart and to play it.",
    inputSchema: { type: "object", properties: { ...CART, frames: { type: "integer", description: "Frames to run (90)" }, input: INPUT,
      shots: { type: "array", items: { type: "integer" }, description: "Frames to show (the first, middle and last)" },
      full: { type: "boolean", description: "Each shot at full size (410x502) instead of one strip at half size" } } } },
  { name: "render_sound", description: "Describes one of a cart's sounds (SOUNDS[n]) or its song (MUSIC) note by note, and how long it plays; can write it to a WAV file to listen to.",
    inputSchema: { type: "object", properties: { ...CART, sound: { type: "integer", description: "The sound, 8-63; leave out for the song" },
      from: { type: "integer", description: "The song's first pattern" }, wav: { type: "string", description: "A file to write the WAV to" } } } },
  { name: "make_sound", description: 'Turns readable notes into the SOUNDS text a cart keeps: "C4 pulse 5, E4 slide, -, G4 saw 7 fadeout". A note is a pitch (C2-D#7), then any of: instrument (square, pulse, thin, triangle, saw, sine, organ, noise), volume 0-7, effect (slide, vibrato, drop, fadein, fadeout, arp, arpslow); left out, they carry on from the note before. "-" is a rest.',
    inputSchema: { type: "object", required: ["notes"], properties: { notes: { type: "string" },
      speed: { type: "integer", description: "Ticks of 1/120 s a note, 1-255 (8)" },
      loop: { type: "array", items: { type: "integer" }, description: "[first, end]: loop those notes for good" } } } },
  { name: "share_link", description: "A link that opens the cart in the playground (https://geofresh.github.io/luanette/), to play it, edit it or send it to a board. With play, it opens in play mode: the screen alone, with buttons on a phone.",
    inputSchema: { type: "object", properties: { ...CART, name: { type: "string" }, play: { type: "boolean", description: "Open in play mode" } } } },
  { name: "api_help", description: "The cart API from docs/api.md: a call's signatures and what it does, or every call whose name or description has a word in it, or (with nothing) the list of names.",
    inputSchema: { type: "object", properties: { name: { type: "string" }, search: { type: "string" } } } },
  { name: "new_cart", description: "A new cart's source from a template (blank, platformer, shooter, puzzle): full-screen, with sprites, sounds and log() to build on.",
    inputSchema: { type: "object", required: ["name"], properties: { name: { type: "string", description: "a-z, 0-9, _ and -" },
      template: { type: "string", enum: ["blank", "platformer", "shooter", "puzzle"] } } } },
  { name: "add_library", description: "Puts tested library code into a cart (or updates it): actor (moving through the map, stopping at solid cells), anim (sprite animations), cam (a following camera), dialog (a text box), particles, state (scenes), timer, tween (easing), vec (2D math). With no library, lists them with what each adds in bytes. Use these instead of writing your own. The library's comments say how to call it. With path, the file is written.",
    inputSchema: { type: "object", properties: { ...CART, library: { type: "array", items: { type: "string" }, description: "The libraries to add, by name" } } } },
];

function cartOf(a) {
  if (typeof a.source === "string") return { src: a.source, name: a.name || "cart" };
  if (typeof a.path === "string") {
    const file = path.resolve(a.path);
    if (!fs.existsSync(file)) throw new Error(`no file ${a.path}`);
    return { src: fs.readFileSync(file, "utf8"), name: path.basename(file, ".lua") };
  }
  throw new Error("give the cart as source or path");
}
const text = (t) => ({ type: "text", text: typeof t === "string" ? t : JSON.stringify(t, null, 1) });

const NOTE = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const INST = ["square", "pulse", "thin", "triangle", "saw", "sine", "organ", "noise"];
const FX = ["", "slide", "vibrato", "drop", "fadein", "fadeout", "arp", "arpslow"];
function describe(textHex) {  // SOUNDS text, note by note, in make_sound's words
  const d = textHex.replace(/\s+/g, ""), notes = [];
  for (let i = 6; i + 5 <= d.length; i += 5) {
    const p = parseInt(d.slice(i, i + 2), 16), inst = parseInt(d[i + 2], 16), v = parseInt(d[i + 3], 16), e = parseInt(d[i + 4], 16);
    notes.push(v ? `${NOTE[p % 12]}${2 + Math.floor(p / 12)} ${INST[inst]} ${v}${FX[e] ? " " + FX[e] : ""}` : "-");
  }
  const ls = parseInt(d.slice(2, 4), 16), le = parseInt(d.slice(4, 6), 16);
  return { speed: parseInt(d.slice(0, 2), 16), loop: le > ls ? [ls, le] : null, notes: notes.join(", ") };
}

const handlers = {
  async check_cart(a) {
    const { src, name } = cartOf(a);
    const res = await L.check(src, name, { file: a.path || name, frames: a.frames || 150, input: a.input ? L.scriptInput(a.input) : null });
    return { content: [text(res)], isError: !res.ok };
  },
  async run_cart(a) {
    const { src, name } = cartOf(a);
    const out = fs.mkdtempSync(path.join(os.tmpdir(), "luanette-"));
    try {
      const res = await L.run(src, name, { frames: a.frames || 90, input: L.scriptInput(a.input || []), shots: a.shots, out });
      const summary = { ok: res.ok, error: res.error, frame: res.frame, fps: res.fps, max_ops: res.max_ops, log: res.log.slice(0, 100),
        shots: res.shots.map((f) => Number(/frame-(\d+)/.exec(f)[1])) };
      const images = (a.full || !res.sheet ? res.shots : [res.sheet]).map((f) => ({ type: "image", mimeType: "image/png", data: fs.readFileSync(f).toString("base64") }));
      return { content: [text(summary), ...images], isError: !res.ok };
    } finally {
      fs.rmSync(out, { recursive: true, force: true });
    }
  },
  async render_sound(a) {
    const { src } = cartOf(a);
    const r = await L.renderAudio(src, a.sound ?? null, a.from ?? null);
    if (a.wav) fs.writeFileSync(path.resolve(a.wav), L.wav(r.pcm, r.rate));
    let peak = 0;
    for (const s of r.pcm) peak = Math.max(peak, Math.abs(s));
    const sounds = require(path.join(HERE, fs.existsSync(path.join(HERE, "lib")) ? "lib" : "../web", "tables.js")).readTable(src, "SOUNDS").map;
    const about = a.sound != null ? describe(sounds.get(a.sound).text) : { patterns: "the song from MUSIC", sounds: Object.fromEntries([...sounds].map(([k, v]) => [k, describe(v.text).notes])) };
    return { content: [text({ seconds: Number(r.seconds.toFixed(2)), loudest: Number((peak / 32768).toFixed(2)), silent: peak === 0, ...about, wav: a.wav || undefined })] };
  },
  async make_sound(a) {
    return { content: [text(L.notesToSound(a.notes, a.speed || 8, a.loop || null))] };
  },
  async share_link(a) {
    const { src, name } = cartOf(a);
    return { content: [text(L.shareLink(src, a.name || name, undefined, a.play === true))] };
  },
  async api_help(a) {
    if (a.name) {
      const e = API.find((x) => x.name === a.name);
      return { content: [text(e || `no call named ${a.name}; api_help with nothing lists them`)], isError: !e };
    }
    if (a.search) {
      const w = a.search.toLowerCase();
      return { content: [text(API.filter((x) => x.name.toLowerCase().includes(w) || x.help.some((h) => h.does.toLowerCase().includes(w))))] };
    }
    return { content: [text(API.map((x) => x.name).join(" "))] };
  },
  async add_library(a) {
    if (!a.library || !a.library.length) return { content: [text(await L.libraries())] };
    const { src } = cartOf(a);
    const r = L.addLibraries(src, a.library);
    if (a.path) fs.writeFileSync(path.resolve(a.path), r.src);
    return { content: [text({ added: r.added, wrote: a.path || undefined }), ...(a.path ? [] : [text(r.src)])] };
  },
  async new_cart(a) {
    return { content: [text(L.newCart(a.name, a.template || "blank"))] };
  },
};

async function handle(msg) {
  const { id, method, params } = msg;
  if (method === "initialize") {
    return { protocolVersion: params?.protocolVersion || "2025-06-18", capabilities: { tools: {} },
      serverInfo: { name: "luanette", version: VERSION },
      instructions: "Tools for making Luanette carts (Lua games for a small console). Read AGENTS.md in the Luanette repository for the cart format. Check after every change; run with input and look at the shots." };
  }
  if (method === "ping") return {};
  if (method === "tools/list") return { tools: TOOLS };
  if (method === "tools/call") {
    const h = handlers[params?.name];
    if (!h) throw Object.assign(new Error(`no tool ${params?.name}`), { code: -32602 });
    try {
      return await h(params.arguments || {});
    } catch (e) {
      return { content: [text(e.message)], isError: true };
    }
  }
  if (id === undefined) return undefined;  // a notification (notifications/initialized and the like)
  throw Object.assign(new Error(`no method ${method}`), { code: -32601 });
}

export async function serve() {
  console.log = console.info = (...a) => console.error(...a);  // stdout carries the protocol and nothing else
  const rl = readline.createInterface({ input: process.stdin });
  let queue = Promise.resolve();
  const send = (o) => process.stdout.write(JSON.stringify(o) + "\n");
  rl.on("line", (line) => {
    if (!line.trim()) return;
    queue = queue.then(async () => {
      let msg;
      try { msg = JSON.parse(line); } catch { send({ jsonrpc: "2.0", id: null, error: { code: -32700, message: "not JSON" } }); return; }
      try {
        const result = await handle(msg);
        if (msg.id !== undefined && result !== undefined) send({ jsonrpc: "2.0", id: msg.id, result });
      } catch (e) {
        if (msg.id !== undefined) send({ jsonrpc: "2.0", id: msg.id, error: { code: e.code || -32603, message: e.message } });
      }
    });
  });
  await new Promise((r) => rl.on("close", r));
  await queue;
}
