// The cart's data tables as the playground's editors read and write them: SOUNDS, MUSIC, SPRITES,
// each `NAME = { [n] = "text", -- name ... }` in the cart's code, and FLAGS, `{ [n] = number }`. Shared by the sound and sprite
// editors (and the command-line tool).
"use strict";

const Tables = (() => {
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

  // NAME = { [n] = number, ... } (FLAGS): on one line or many, a value a number or numbers joined
  // with | (`1 | 4`), comments anywhere. The entries, or why the editor can't read it.
  function readNumbers(src, name) {
    const m = new RegExp(`^${name}\\s*=\\s*\\{([^}]*)\\}`, "m").exec(src);
    if (!m) {
      return new RegExp(`(^|[^\\w.])${name}\\s*(=|\\[)`, "m").test(src)
        ? { problem: `${name} is set up in the code in a way the editor can't read. Change it there, or delete it to start over here.` }
        : { map: new Map() };
    }
    const map = new Map();
    const body = m[1].split("\n").map((l) => l.replace(/--.*$/, "")).join(" ");
    for (const raw of body.split(",")) {
      const item = raw.trim();
      if (!item) continue;
      const e = /^\[(\d+)\]\s*=\s*((?:0x[0-9a-fA-F]+|\d+)(?:\s*\|\s*(?:0x[0-9a-fA-F]+|\d+))*)$/.exec(item);
      if (!e) return { problem: `The editor can't read this part of ${name}: ${item}` };
      map.set(Number(e[1]), e[2].split("|").reduce((v, t) => v | Number(t.trim()), 0));
    }
    return { map };
  }

  // Writes NAME = { [n] = v, -- note(n, v) ... } (its zeros left out), in place of the old one, or
  // after the table named `after` (else after the header).
  function writeNumbers(src, name, map, intro, note, after) {
    const keys = [...map.keys()].filter((k) => map.get(k)).sort((a, b) => a - b);
    const body = keys.map((k) => `  [${k}] = ${map.get(k)},${note ? " -- " + note(k, map.get(k)) : ""}`).join("\n");
    const block = keys.length ? `${name} = {\n${body}\n}` : "";
    const said = intro.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    const re = new RegExp(`(^-- ${said}\\n)?^${name}\\s*=\\s*\\{[^}]*\\}\\n?`, "m");
    if (re.test(src)) return src.replace(re, block ? `-- ${intro}\n${block}\n` : "");
    if (!block) return src;
    const prev = after && new RegExp(`^${after}\\s*=\\s*\\{[^]*?^\\}\\n?`, "m").exec(src);
    if (prev) {
      const end = prev.index + prev[0].length;
      return src.slice(0, end) + `-- ${intro}\n${block}\n` + src.slice(end);
    }
    const lines = src.split("\n");
    let at = 0;
    while (at < lines.length && lines[at].startsWith("--")) at++;
    lines.splice(at, 0, "", `-- ${intro}`, block);
    return lines.join("\n");
  }

  // A library (libs/*.lua): its first line says what it is, `-- lib: NAME VERSION, api N: about`,
  // and `-- end lib: NAME` ends it. null when `text` doesn't start that way.
  function libInfo(text) {
    const m = /^-- lib: (\w+) (\d+), api (\d+): (.*)$/m.exec(text.split("\n")[0]);
    return m ? { name: m[1], version: Number(m[2]), api: Number(m[3]), about: m[4] } : null;
  }

  // The cart with library `text` in it: in place of the version it has (between its markers), or
  // after the libraries it has, or after the header. The header asks for the API it needs.
  // Returns {src, name, was} (was: the version replaced, 0 for none).
  function addLib(src, text) {
    const info = libInfo(text);
    if (!info) throw new Error("that isn't a library: its first line should be '-- lib: NAME VERSION, api N: ...'");
    const block = text.replace(/\n*$/, "\n");
    const old = new RegExp(`^-- lib: ${info.name} (\\d+)[^\\n]*\\n[^]*?^-- end lib: ${info.name}[ \\t]*(\\n|$)`, "m").exec(src);
    let out, was = 0;
    if (old) {
      was = Number(old[1]);
      out = src.slice(0, old.index) + block + src.slice(old.index + old[0].length);
    } else {
      const ends = [...src.matchAll(/^-- end lib: \w+[ \t]*\n?/gm)];
      let at;
      if (ends.length) {
        const e = ends[ends.length - 1];
        at = e.index + e[0].length;
        if (!e[0].endsWith("\n")) { src = src + "\n"; at = at + 1; }
      } else {
        const lines = src.split("\n");
        let n = 0;
        // the header: its `-- key: value` lines (not a table's comment that follows it)
        while (n < lines.length && /^--\s*[a-z][\w ]*:/i.test(lines[n]) && !/^-- (end )?lib:/.test(lines[n])) n++;
        at = lines.slice(0, n).join("\n").length + (n ? 1 : 0);
      }
      const before = src.slice(0, at), after = src.slice(at);
      out = before + "\n" + block + (after.startsWith("\n") || !after ? "" : "\n") + after;
    }
    return { src: withApi(out, info.api), name: info.name, was };
  }

  // The header asks for API `min` at least (sounds and music need 3, spaced sprites 4), or a board
  // with older firmware would run the cart as one that doesn't need it.
  function withApi(src, min) {
    const lines = src.split("\n");
    for (let i = 0; i < Math.min(lines.length, 12); i++) {
      const m = /^--\s*api\s*:\s*(\d+)\s*$/.exec(lines[i]);
      if (m) {
        if (Number(m[1]) < min) lines[i] = "-- api: " + min;
        return lines.join("\n");
      }
    }
    let at = 0;
    while (at < lines.length && lines[at].startsWith("--") && at < 12) at++;
    lines.splice(at, 0, "-- api: " + min);
    return lines.join("\n");
  }

  return { readTable, writeTable, readNumbers, writeNumbers, withApi, libInfo, addLib };
})();

if (typeof module !== "undefined") module.exports = Tables;
