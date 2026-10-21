// The playground's sound and music editor. It reads the cart's SOUNDS and MUSIC tables from the
// code, lets you draw sounds (pitch and volume per note, an instrument and an effect each) and
// lay out a song's patterns, and writes the tables back into the code as you go. The formats are
// engine/synth.hpp's; the engine plays what you hear here, as the board will.
// Uses the playground's globals: $, code, lw, sound, say.
"use strict";

const SoundEditor = (() => {
  const FIRST = 8, LAST = 63, NOTES = 32, PATTERNS = 64;
  const INSTRUMENTS = ["square", "pulse", "thin pulse", "triangle", "saw", "sine", "organ", "noise"];
  const EFFECTS = ["none", "slide", "vibrato", "drop", "fade in", "fade out", "arp fast", "arp slow"];
  const COLORS = ["#ff004d", "#ffa300", "#ffec27", "#00e436", "#29adff", "#83769c", "#ff77a8", "#c2c3c7"];
  const NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
  const noteName = (p) => NAMES[p % 12] + (2 + Math.floor(p / 12));
  const hex2 = (n) => n.toString(16).padStart(2, "0");

  // Sounds to start from: [speed, notes as [pitch, instrument, volume, effect]].
  const PRESETS = {
    "Blip": [2, [[36, 0, 5, 0], [36, 0, 3, 5]]],
    "Coin": [4, [[47, 1, 6, 0], [52, 1, 6, 5]]],
    "Jump": [3, [[24, 0, 5, 0], [36, 0, 5, 1], [43, 0, 4, 1], [48, 0, 2, 5]]],
    "Laser": [2, [[60, 4, 6, 3], [50, 4, 4, 3]]],
    "Hit": [2, [[20, 7, 7, 0], [10, 0, 5, 3]]],
    "Explosion": [6, [[30, 7, 7, 0], [24, 7, 6, 0], [18, 7, 4, 0], [12, 7, 3, 5]]],
    "Power-up": [3, [[24, 3, 5, 0], [28, 3, 5, 0], [31, 3, 5, 0], [36, 3, 5, 0], [40, 3, 5, 0], [43, 3, 5, 0], [48, 3, 5, 5]]],
    "Melody": [16, [[36, 1, 5, 0], [40, 1, 5, 0], [43, 1, 5, 2], [43, 1, 5, 5], [41, 1, 5, 0], [40, 1, 5, 0], [38, 1, 5, 1], [36, 1, 5, 5]]],
    "Bass line": [16, [[12, 3, 6, 0], [12, 3, 4, 0], [19, 3, 6, 0], [12, 3, 6, 0], [15, 3, 6, 0], [15, 3, 4, 0], [10, 3, 6, 0], [14, 3, 6, 0]]],
    "Drums": [16, [[12, 7, 6, 3], [0, 0, 0, 0], [36, 7, 4, 5], [0, 0, 0, 0], [12, 7, 6, 3], [12, 7, 6, 3], [36, 7, 4, 5], [0, 0, 0, 0]]],
    "Chord": [16, [[24, 6, 5, 6], [28, 6, 5, 6], [31, 6, 5, 6], [36, 6, 5, 6]]],
  };

  // ---- the cart's text ------------------------------------------------------------------------
  function decodeSound(text) {
    const d = text.replace(/\s+/g, "");
    if (!/^[0-9a-fA-F]+$/.test(d) || d.length < 11 || (d.length - 6) % 5) return null;
    const notes = [];
    for (let i = 6; i < d.length; i += 5) {
      notes.push({ p: parseInt(d.slice(i, i + 2), 16), i: parseInt(d[i + 2], 16), v: parseInt(d[i + 3], 16), e: parseInt(d[i + 4], 16) });
    }
    return { speed: parseInt(d.slice(0, 2), 16), ls: parseInt(d.slice(2, 4), 16), le: parseInt(d.slice(4, 6), 16), notes };
  }
  function encodeSound(s) {
    return hex2(s.speed) + " " + hex2(s.ls) + hex2(s.le) + " " +
      s.notes.map((n) => hex2(n.p) + n.i.toString(16) + n.v.toString(16) + n.e.toString(16)).join(" ");
  }
  function decodePattern(text) {
    const d = text.replace(/\s+/g, "");
    if (!/^[0-9a-fA-F]{2}([0-9a-fA-F]{2}|--){4}$/.test(d)) return null;
    const ch = [];
    for (let c = 0; c < 4; c++) {
      const t = d.slice(2 + c * 2, 4 + c * 2);
      ch.push(t === "--" ? -1 : parseInt(t, 16));
    }
    return { flags: parseInt(d.slice(0, 2), 16), ch };
  }
  const encodePattern = (p) => hex2(p.flags) + " " + p.ch.map((s) => (s < 0 ? "--" : hex2(s))).join(" ");

  // NAME = { [n] = "text", -- comment ... } in the code: the entries, or why the editor can't read it.
  function readTable(src, name) {
    const m = new RegExp(`^${name}\\s*=\\s*\\{([^]*?)^\\}`, "m").exec(src);
    if (!m) {
      return new RegExp(`(^|[^\\w.])${name}\\s*(=|\\[)`, "m").test(src)
        ? { problem: `${name} is set up in the code in a way the editor can't read. Change it there, or delete it to start over here.` }
        : { map: new Map() };
    }
    const map = new Map();
    for (const raw of m[1].split("\n")) {
      const line = raw.trim();
      if (!line || line.startsWith("--")) continue;
      const e = /^\[(\d+)\]\s*=\s*"([0-9a-fA-F \-]*)",?\s*(?:--\s*(.*))?$/.exec(line);
      if (!e) return { problem: `The editor can't read this line of ${name}: ${line}` };
      map.set(Number(e[1]), { text: e[2], name: (e[3] || "").trim() });
    }
    return { map };
  }

  function writeTable(src, name, map, intro) {
    const keys = [...map.keys()].sort((a, b) => a - b);
    const body = keys.map((k) => `  [${k}] = "${map.get(k).text}",${map.get(k).name ? " -- " + map.get(k).name : ""}`).join("\n");
    const block = keys.length ? `${name} = {\n${body}\n}` : "";
    const said = intro.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");  // the intro as a pattern
    const re = new RegExp(`(^-- ${said}\\n)?^${name}\\s*=\\s*\\{[^]*?^\\}\\n?`, "m");
    if (re.test(src)) return src.replace(re, block ? `-- ${intro}\n${block}\n` : "");
    if (!block) return src;
    // After the header (the comment lines at the top) and a SOUNDS table if there is one.
    const lines = src.split("\n");
    let at = 0;
    while (at < lines.length && lines[at].startsWith("--")) at++;
    const sounds = /^SOUNDS\s*=\s*\{[^]*?^\}\n?/m.exec(src);
    if (name === "MUSIC" && sounds) {
      const end = sounds.index + sounds[0].length;
      return src.slice(0, end) + `-- ${intro}\n${block}\n` + src.slice(end);
    }
    lines.splice(at, 0, "", `-- ${intro}`, block);
    return lines.join("\n");
  }

  // Sounds need API 3: the header says so, or the board would run it as an older cart.
  function withApi(src) {
    const lines = src.split("\n");
    for (let i = 0; i < Math.min(lines.length, 12); i++) {
      const m = /^--\s*api\s*:\s*(\d+)\s*$/.exec(lines[i]);
      if (m) {
        if (Number(m[1]) < 3) lines[i] = "-- api: 3";
        return lines.join("\n");
      }
    }
    let at = 0;
    while (at < lines.length && lines[at].startsWith("--") && at < 12) at++;
    lines.splice(at, 0, "-- api: 3");
    return lines.join("\n");
  }

  // ---- state ----------------------------------------------------------------------------------
  let sounds = new Map(), music = new Map();  // number -> {text, name}
  let slot = FIRST;                            // the sound being edited, or an empty number to make one at
  let brush = { i: 0, e: 0, v: 5 };
  let playing = null;                          // {start, speed, ls, le, count} for the playhead
  let problem = "";

  function load() {
    const a = readTable(code.value, "SOUNDS"), b = readTable(code.value, "MUSIC");
    problem = a.problem || b.problem || "";
    sounds = a.map || new Map();
    music = b.map || new Map();
    if (slot < FIRST || slot > LAST) slot = sounds.size ? Math.min(...sounds.keys()) : FIRST;
  }

  function save() {
    let src = writeTable(code.value, "SOUNDS", sounds, "sounds (the playground's sound editor writes this table)");
    src = writeTable(src, "MUSIC", music, "the song (the playground's music editor writes this table)");
    if (sounds.size || music.size) src = withApi(src);
    if (src === code.value) return;
    code.value = src;
    code.dispatchEvent(new Event("input"));  // kept as typed code is
  }

  const current = () => (slot >= 0 && sounds.has(slot) ? decodeSound(sounds.get(slot).text) : null);
  function put(s) {
    sounds.get(slot).text = encodeSound(s);
    save();
    draw();
  }

  // ---- sounds ---------------------------------------------------------------------------------
  const W = 640, PITCH_H = 64 * 4, VOL_Y = PITCH_H + 8, VOL_H = 8 * 6, STRIP_Y = VOL_Y + VOL_H + 8, H = STRIP_Y + 32;

  function fillSlots() {
    const sel = $("snd-slot"), options = [];
    for (let n = FIRST; n <= LAST; n++) {  // every number a sound can have; the unused ones say so
      const s = sounds.get(n);
      options.push(new Option(s ? `${n}${s.name ? " " + s.name : ""}` : `${n} (empty)`, n));
    }
    sel.replaceChildren(...options);
    sel.value = slot;
    $("snd-count").textContent = `${sounds.size} of ${LAST - FIRST + 1} used`;
    $("snd-empty-num").textContent = slot;
    const s = current();
    for (const id of ["snd-play", "snd-delete", "snd-speed", "snd-len", "snd-ls", "snd-le", "snd-name"]) $(id).disabled = !s;
    $("snd-empty").hidden = !!s;
    $("snd-canvas").hidden = !s;
    if (!s) return;
    $("snd-name").value = sounds.get(slot).name;
    $("snd-speed").value = s.speed;
    $("snd-len").value = s.notes.length;
    $("snd-ls").value = s.ls;
    $("snd-le").value = s.le;
    $("snd-call").textContent = `sfx(${slot})`;
  }

  function draw() {
    const cv = $("snd-canvas"), g = cv.getContext("2d"), s = current();
    if (!s) return;
    g.fillStyle = "#0d1117";
    g.fillRect(0, 0, W, H);
    for (let o = 0; o <= 5; o++) {  // a line at each C
      const y = PITCH_H - o * 12 * 4;
      g.fillStyle = "#30363d";
      g.fillRect(0, y - 1, W, 1);
      g.fillStyle = "#8b949e";
      g.font = "10px monospace";
      if (o < 5) g.fillText("C" + (2 + o), 2, y - 3);
    }
    const cols = Math.max(16, s.notes.length), cw = W / cols;  // a short sound gets wide cells
    let at = -1;
    if (playing && sound.ctx) {
      const ticks = (sound.ctx.currentTime - playing.start) * 120 / playing.speed;
      let n = Math.floor(ticks);
      if (playing.le > playing.ls && n >= playing.le) n = playing.ls + (n - playing.ls) % (playing.le - playing.ls);
      if (n < playing.count) at = n; else playing = null;
    }
    for (let k = 0; k < cols; k++) {
      const x = k * cw, n = s.notes[k], inside = k < s.notes.length, pad = Math.min(2, cw / 8);
      if (k === at) {
        g.fillStyle = "#30363d";
        g.fillRect(x, 0, cw, H);
      }
      if (s.le > s.ls && (k === s.ls || k === s.le)) {
        g.fillStyle = "#f0b429";
        g.fillRect(x, 0, 1, H);
      }
      if (!inside) {
        g.fillStyle = "#161b22";
        g.fillRect(x + pad, VOL_Y, cw - 2 * pad, VOL_H);
        continue;
      }
      if (n.v > 0) {
        g.fillStyle = COLORS[n.i];
        g.fillRect(x + pad, PITCH_H - (n.p + 1) * 4 - 2, cw - 2 * pad, 6);
      }
      g.fillStyle = "#161b22";
      g.fillRect(x + pad, VOL_Y, cw - 2 * pad, VOL_H);
      g.fillStyle = n.v ? COLORS[n.i] : "#30363d";
      g.fillRect(x + pad, VOL_Y + VOL_H - n.v * 6, cw - 2 * pad, n.v * 6);
      g.fillStyle = COLORS[n.i];
      g.fillRect(x + pad, STRIP_Y, cw - 2 * pad, 12);
      g.fillStyle = "#e6edf3";
      g.font = "11px monospace";
      if (n.e) g.fillText(String(n.e), x + cw / 2 - 3, STRIP_Y + 26);
    }
    if (playing) requestAnimationFrame(draw);
  }

  // Drawing with the pointer: the pitch area sets a note's pitch (with the brush's instrument and
  // effect, and its volume if the note was silent); the volume area sets volume; the strip paints.
  let dragging = false;
  function paint(e) {
    const s = current();
    if (!s) return;
    const r = $("snd-canvas").getBoundingClientRect();
    const x = (e.clientX - r.left) * W / r.width, y = (e.clientY - r.top) * H / r.height;
    const k = Math.floor(x / (W / Math.max(16, s.notes.length)));
    if (k < 0 || k >= s.notes.length) return;
    const n = s.notes[k];
    if (y < PITCH_H) {
      Object.assign(n, { p: Math.max(0, Math.min(63, Math.floor((PITCH_H - y) / 4))), i: brush.i, e: brush.e });
      if (!n.v) n.v = brush.v;
    } else if (y >= VOL_Y && y < VOL_Y + VOL_H + 4) {
      n.v = Math.max(0, Math.min(7, Math.round((VOL_Y + VOL_H - y) / 6)));
      if (n.v) brush.v = n.v;
    } else if (y >= STRIP_Y) {
      Object.assign(n, { i: brush.i, e: brush.e });
    }
    $("snd-info").textContent = `note ${k + 1}: ${n.v ? noteName(n.p) + ", " + INSTRUMENTS[n.i] + ", volume " + n.v + (n.e ? ", " + EFFECTS[n.e] : "") : "rest"}`;
    put(s);
  }

  function brushes() {
    const make = (id, names, key) => {
      $(id).replaceChildren(...names.map((name, i) => {
        const b = document.createElement("button");
        b.textContent = (key === "i" ? "" : i + " ") + name;
        if (key === "i") b.style.borderLeft = `4px solid ${COLORS[i]}`;
        b.classList.toggle("on", brush[key] === i);
        b.onclick = () => { brush[key] = i; brushes(); };
        return b;
      }));
    };
    make("snd-inst", INSTRUMENTS, "i");
    make("snd-fx", EFFECTS, "e");
  }

  function playSound() {
    const s = current();
    if (!s) return;
    sound.unlock();
    sound.stop();
    const why = lw.play_sound(sounds.get(slot).text);
    if (why) { say(`SOUNDS[${slot}] ${why}`, true); return; }
    playing = { start: sound.ctx.currentTime + 0.03, speed: s.speed, ls: s.ls, le: s.le, count: s.le > s.ls ? Infinity : s.notes.length };
    draw();
  }

  // A new sound at the empty number picked in the list, else at the first empty one.
  function newSound(preset) {
    let n = sounds.has(slot) ? FIRST : slot;
    while (sounds.has(n) && n <= LAST) n++;
    if (n > LAST) { say("A cart has room for sounds 8 to 63, and they're all used.", true); return; }
    const [speed, notes] = PRESETS[preset || "Blip"];
    sounds.set(n, { text: encodeSound({ speed, ls: 0, le: 0, notes: notes.map(([p, i, v, e]) => ({ p, i, v, e })) }), name: (preset || "").toLowerCase() });
    slot = n;
    save();
    show();
  }

  function setField(fn) {
    const s = current();
    if (!s) return;
    fn(s);
    put(s);
    fillSlots();
  }

  // ---- music ----------------------------------------------------------------------------------
  function fillMusic() {
    const rows = [...music.keys()].sort((a, b) => a - b);
    const list = $("mus-list");
    list.replaceChildren();
    $("mus-empty").hidden = rows.length > 0;
    for (const n of rows) {
      const p = decodePattern(music.get(n).text) || { flags: 0, ch: [-1, -1, -1, -1] };
      const row = document.createElement("div");
      row.className = "pattern";
      const label = document.createElement("span");
      label.textContent = n;
      label.className = "num";
      row.appendChild(label);
      p.ch.forEach((s, c) => {
        const sel = document.createElement("select");
        sel.setAttribute("aria-label", `pattern ${n} channel ${c + 1}`);
        sel.append(new Option("—", -1), ...[...sounds.keys()].sort((a, b) => a - b).map((k) => new Option(`${k}${sounds.get(k).name ? " " + sounds.get(k).name : ""}`, k)));
        sel.value = s;
        sel.onchange = () => { p.ch[c] = Number(sel.value); music.get(n).text = encodePattern(p); save(); };
        row.appendChild(sel);
      });
      for (const [bit, name] of [[1, "loop start"], [2, "loop back"], [4, "stop"]]) {
        const l = document.createElement("label");
        const box = document.createElement("input");
        box.type = "checkbox";
        box.checked = !!(p.flags & bit);
        box.onchange = () => { p.flags = box.checked ? p.flags | bit : p.flags & ~bit; music.get(n).text = encodePattern(p); save(); };
        l.append(box, " " + name);
        row.appendChild(l);
      }
      const play = document.createElement("button");
      play.textContent = "Play from here";
      play.onclick = () => playSong(n);
      const del = document.createElement("button");
      del.textContent = "Delete";
      del.onclick = () => { music.delete(n); save(); fillMusic(); };
      row.append(play, del);
      list.appendChild(row);
    }
    $("mus-call").textContent = rows.length ? `music(${rows[0]})` : "music(0)";
  }

  function playSong(start) {
    sound.unlock();
    sound.stop();
    const json = { music: {}, sounds: {} };
    for (const [k, v] of music) json.music[k] = v.text;
    for (const [k, v] of sounds) json.sounds[k] = v.text;
    const why = lw.play_song(JSON.stringify(json), start);
    if (why) say(`music(${start}): ${why}`, true);
  }

  function addPattern() {
    const n = music.size ? Math.max(...music.keys()) + 1 : 0;
    if (n >= PATTERNS) { say("A song has room for patterns 0 to 63.", true); return; }
    const last = music.size ? decodePattern(music.get(n - 1)?.text || "") : null;
    music.set(n, { text: encodePattern(last ? { flags: 0, ch: last.ch } : { flags: 0, ch: [sounds.size ? Math.min(...sounds.keys()) : -1, -1, -1, -1] }), name: "" });
    save();
    fillMusic();
  }

  // ---- tabs -----------------------------------------------------------------------------------
  let tab = "code";
  function show(which = tab) {
    tab = which;
    for (const t of ["code", "sounds", "music"]) $("tab-" + t).classList.toggle("on", t === tab);
    code.hidden = tab !== "code";
    $("sounds").hidden = tab !== "sounds";
    $("music").hidden = tab !== "music";
    if (tab === "code") return;
    load();
    $("snd-problem").textContent = $("mus-problem").textContent = problem;
    $("snd-problem").hidden = $("mus-problem").hidden = !problem;
    $("snd-body").hidden = $("mus-body").hidden = !!problem;
    if (problem) return;
    if (tab === "sounds") { fillSlots(); brushes(); draw(); } else fillMusic();
  }

  function setUp() {
    const presets = $("snd-preset");
    presets.append(...Object.keys(PRESETS).map((k) => new Option(k, k)));
    presets.onchange = () => { if (presets.value) newSound(presets.value); presets.value = ""; };
    for (const t of ["code", "sounds", "music"]) $("tab-" + t).onclick = () => { sound.stop(); show(t); };
    $("snd-slot").onchange = () => { slot = Number($("snd-slot").value); fillSlots(); draw(); };
    $("snd-new").onclick = () => newSound();
    $("snd-play").onclick = playSound;
    $("snd-stop").onclick = $("mus-stop").onclick = () => { sound.stop(); playing = null; draw(); };
    $("snd-delete").onclick = () => {
      if (!confirm(`Delete sound ${slot}?`)) return;
      sounds.delete(slot);  // the number stays picked, empty now
      save();
      show();
    };
    $("snd-name").oninput = () => { sounds.get(slot).name = $("snd-name").value.replace(/[\n"]/g, "").slice(0, 24); save(); };
    $("snd-name").onchange = fillSlots;
    const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, Number(v) || lo));
    $("snd-speed").onchange = () => setField((s) => { s.speed = clamp($("snd-speed").value, 1, 255); });
    $("snd-len").onchange = () => setField((s) => {
      const len = clamp($("snd-len").value, 1, NOTES);
      while (s.notes.length < len) s.notes.push({ p: s.notes.length ? s.notes[s.notes.length - 1].p : 24, i: brush.i, v: 0, e: 0 });
      s.notes.length = len;
      if (s.le > len) s.le = len;
      if (s.ls >= s.le) s.ls = s.le = 0;
    });
    const loop = () => setField((s) => {
      s.ls = clamp($("snd-ls").value, 0, s.notes.length);
      s.le = clamp($("snd-le").value, 0, s.notes.length);
      if (s.le <= s.ls) s.ls = s.le = 0;
    });
    $("snd-ls").onchange = $("snd-le").onchange = loop;
    const cv = $("snd-canvas");
    cv.width = W;
    cv.height = H;
    cv.addEventListener("pointerdown", (e) => { dragging = true; cv.setPointerCapture(e.pointerId); paint(e); });
    cv.addEventListener("pointermove", (e) => { if (dragging) paint(e); });
    cv.addEventListener("pointerup", () => { dragging = false; });
    cv.addEventListener("pointercancel", () => { dragging = false; });
    $("mus-add").onclick = addPattern;
    $("mus-play").onclick = () => { if (music.size) playSong(Math.min(...music.keys())); };
    addEventListener("keydown", (e) => {  // Space plays the sound being edited
      if (tab === "sounds" && e.code === "Space" && !typing(e)) {
        e.preventDefault();
        playSound();
      }
    });
  }

  return { setUp, show, get tab() { return tab; } };
})();

SoundEditor.setUp();
