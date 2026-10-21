// The board over Web Serial: sends a cart the way tools/push.py does, from the page.
//
// The console takes messages as lines on its USB serial port. A message goes as `cart msg begin`,
// its parts `cart msg <offset> <crc32> <base64>` (four in flight; a part the board finds damaged or
// out of place is answered "@error at N", and sending goes back to byte N), then
// `cart msg end <crc32>`. The log shares the port: only lines starting with "@" are answers.
// ARCHITECTURE.md ("Carts and messages") has the whole protocol.
"use strict";

const Board = (() => {
  const CRC_TABLE = new Uint32Array(256).map((_, n) => {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c;
  });
  function crc32(bytes) {
    let c = 0xffffffff;
    for (const b of bytes) c = CRC_TABLE[(c ^ b) & 255] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  }
  const hex8 = (n) => n.toString(16).padStart(8, "0");
  function base64(bytes) {
    let s = "";
    for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
    return btoa(s);
  }
  // JSON as Python's json.dumps writes it (", " and ": ", non-ASCII as \uXXXX), so a cart sent from
  // here is byte for byte what push.py would send.
  function pyJson(value) {
    if (typeof value === "string") {
      return JSON.stringify(value).replace(/[\u007f-￿]/g, (ch) => "\\u" + ch.charCodeAt(0).toString(16).padStart(4, "0"));
    }
    if (typeof value !== "object" || value === null) return JSON.stringify(value);
    return "{" + Object.entries(value).map(([k, v]) => pyJson(k) + ": " + pyJson(v)).join(", ") + "}";
  }

  // This browser's timezone as a POSIX TZ rule, for the board's date(): push.py reads the same rule
  // from /etc/localtime; a page has only the offsets, so the rule is rebuilt from where they change.
  function posixTz(year = new Date().getFullYear()) {
    const offsetAt = (t) => -new Date(t).getTimezoneOffset();  // minutes east of UTC
    const name = (m) => {
      const a = Math.abs(m), h = String(Math.floor(a / 60)).padStart(2, "0"), mm = a % 60;
      return "<" + (m < 0 ? "-" : "+") + h + (mm ? String(mm).padStart(2, "0") : "") + ">";
    };
    const posixOffset = (m) => {  // POSIX counts west of UTC
      const a = Math.abs(m), s = m > 0 ? "-" : "";
      return s + Math.floor(a / 60) + (a % 60 ? ":" + String(a % 60).padStart(2, "0") : "");
    };
    // Every change in the year, to the minute.
    const changes = [];
    const start = Date.UTC(year, 0, 1), end = Date.UTC(year + 1, 0, 1), HOUR = 3600e3;
    for (let t = start; t < end; t += HOUR) {
      if (offsetAt(t) === offsetAt(t + HOUR)) continue;
      let lo = t, hi = t + HOUR;
      while (hi - lo > 60e3) {
        const mid = lo + Math.floor((hi - lo) / 120e3) * 60e3;
        if (offsetAt(mid) === offsetAt(lo)) lo = mid; else hi = mid;
      }
      changes.push({ at: hi, from: offsetAt(lo), to: offsetAt(hi) });
    }
    const jan = offsetAt(start);
    if (changes.length !== 2) return name(jan) + posixOffset(jan);  // no DST (or a one-off change): the offset now
    const std = Math.min(changes[0].from, changes[0].to), dst = Math.max(changes[0].from, changes[0].to);
    // A change as POSIX writes it: month, week (5 = the last), weekday, at the wall time it happens.
    const rule = (c) => {
      const wall = new Date(c.at + c.from * 60e3);
      const day = wall.getUTCDate(), days = new Date(Date.UTC(year, wall.getUTCMonth() + 1, 0)).getUTCDate();
      const week = day + 7 > days ? 5 : Math.ceil(day / 7);
      const h = wall.getUTCHours(), m = wall.getUTCMinutes();
      return `M${wall.getUTCMonth() + 1}.${week}.${wall.getUTCDay()}/${h}${m ? ":" + String(m).padStart(2, "0") : ""}`;
    };
    const into = changes.find((c) => c.to === dst), out = changes.find((c) => c.to === std);
    return name(std) + posixOffset(std) + name(dst) + posixOffset(dst) + "," + rule(into) + "," + rule(out);
  }

  class BoardError extends Error {}

  // One open connection: answers ("@...") are queued as they arrive; the rest is the board's log.
  class Link {
    constructor(port) {
      this.port = port;
      this.answers = [];
      this.waiters = [];
      this.pending = "";
      this.decoder = new TextDecoder();
      this.encoder = new TextEncoder();
    }

    async open() {
      if (!this.port.readable) await this.port.open({ baudRate: 115200 });
      this.writer = this.port.writable.getWriter();
      this.reader = this.port.readable.getReader();
      this.reading = this.readLoop();
      // A newline first: it ends any line an earlier connection left half sent (the board would glue
      // this one's first command onto it and refuse it), and the board ignores an empty line.
      await this.write("\n");
      await sleep(200);
      this.answers.length = 0;
    }

    async close() {
      try { await this.reader.cancel(); } catch {}
      await this.reading;
      this.reader.releaseLock();
      this.writer.releaseLock();
      try { await this.port.close(); } catch {}
    }

    async readLoop() {
      try {
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          this.pending += this.decoder.decode(value, { stream: true });
          let nl;
          while ((nl = this.pending.indexOf("\n")) >= 0) {
            const line = this.pending.slice(0, nl).trim();
            this.pending = this.pending.slice(nl + 1);
            if (!line.startsWith("@")) continue;
            const w = this.waiters.shift();
            if (w) w(line); else this.answers.push(line);
          }
        }
      } catch {}  // the port went away; answer() times out
    }

    write(text) { return this.writer.write(this.encoder.encode(text)); }
    send(text) { return this.write(text + "\n"); }

    // The next "@..." line from the board, or null after `timeout` ms.
    answer(timeout = 3000) {
      if (this.answers.length) return Promise.resolve(this.answers.shift());
      return new Promise((resolve) => {
        const w = (line) => { clearTimeout(timer); resolve(line); };
        const timer = setTimeout(() => {
          const i = this.waiters.indexOf(w);
          if (i >= 0) this.waiters.splice(i, 1);
          resolve(null);
        }, timeout);
        this.waiters.push(w);
      });
    }

    async ask(text, timeout = 3000) {
      for (let i = 0; i < 3; i++) {
        await this.send(text);
        const a = await this.answer(timeout);
        if (a) return a;
      }
      throw new BoardError("No answer from the board. Is it running the Luanette firmware?");
    }

    // How much of the message the board has (after a lost answer): asked with a part that can't fit.
    async where() {
      await sleep(300);
      this.answers.length = 0;
      const m = /^@error at (\d+)/.exec(await this.ask("cart msg ffffffff 0 AA=="));
      if (!m) throw new BoardError("The board lost its place in the message.");
      return Number(m[1]);
    }

    // One message; returns the times it went back.
    async message(msg) {
      const raw = this.encoder.encode(pyJson(msg));
      if (!(await this.ask("cart msg begin")).startsWith("@ok")) throw new BoardError("The board refused a message.");
      let off = 0, inflight = [], ignore = 0, back = 0;
      while (off < raw.length || inflight.length) {
        while (off < raw.length && inflight.length < 4) {  // 4 lines of ~2.8 KB: inside the board's 16 KB buffer
          const part = raw.subarray(off, off + 2048);
          await this.send(`cart msg ${off.toString(16)} ${hex8(crc32(part))} ${base64(part)}`);
          inflight.push(off);
          off += part.length;
        }
        const a = await this.answer();
        if (a === null) {  // an answer lost: ask where it stands
          off = await this.where();
          inflight = [];
          ignore = 0;
          back++;
          continue;
        }
        if (ignore) { ignore--; continue; }  // answers to parts sent after one that failed
        inflight.shift();
        const m = /^@error at (\d+)/.exec(a);
        if (m) {
          off = Number(m[1]);
          ignore = inflight.length;
          inflight = [];
          back++;
        }
      }
      const a = await this.ask(`cart msg end ${hex8(crc32(raw))}`, 10000);
      if (!a.startsWith("@ok")) throw new BoardError("The board refused the cart: " + a);
      return back;
    }
  }

  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  // Sends a cart (its source as it ships: comments out) to the board on `port`, sets the board's
  // clock and timezone on the way, and opens it there. The port is closed again after, so other
  // tools (the installer, push.py) can have it.
  async function sendCart(port, { name, title, lua }) {
    const link = new Link(port);
    await link.open();
    try {
      await link.ask(`time ${Math.floor(Date.now() / 1000)}`);
      const tz = posixTz();
      if (/^[A-Za-z<>+\-0-9,./:]+$/.test(tz)) await link.ask(`tz ${tz}`);
      return await link.message({ type: "cart", name, title, lua, run: true });
    } finally {
      await link.close();
    }
  }

  return { sendCart, posixTz, crc32, pyJson, BoardError, Link };
})();

if (typeof module !== "undefined") module.exports = Board;
