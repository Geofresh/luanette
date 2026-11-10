// The playground's timeline: every frame's input since the cart started, and the engine's whole
// memory every KEY_EVERY frames, so any recent frame can be shown again: the memory as it was at
// the frame's last snapshot, then the frames from there run again with the same input. A cart runs
// the same each time it's given the same input (a frame clock, a fixed hash seed, the Lua heap at
// the same addresses: web/luanette_web.cpp), so the frame shown is the frame that was played.
//
// A snapshot keeps only the 4 KB pieces of memory that changed since the last one (a game changes
// about 150 KB a second); the rest are the last snapshot's. Snapshots past BUDGET bytes go, oldest
// first. The input stays from the start: a replay (Timeline.pack) is the cart, its start and it.
"use strict";
const Timeline = (() => {
  const CH = 4096, KEY_EVERY = 30, BUDGET = 160 << 20;
  let mod = null;
  let keys = [];       // {frame, chunks: [Uint8Array per 4 KB], bytes: its new pieces' size}
  let base = null;     // the memory as of `last` (what a new snapshot is compared against)
  let last = null;
  let bytes = 0;
  const T = {
    inputs: [],        // frame n's input at [n - 1]: [held, touching, tx, ty, ax, ay]
    clocks: [],        // and the clock it ran at (microseconds)
    start: null,       // {name, src, clock, unix, save}: how the run began
    edited: false,     // new code went in as it ran (a replay can't show that)
  };

  function snapshot(frame) {
    const mem = mod.HEAPU8, n = Math.ceil(mem.length / CH);
    const chunks = new Array(n);
    let fresh = 0;
    if (!base || base.length !== mem.length || !last) {
      for (let c = 0; c < n; c++) chunks[c] = mem.slice(c * CH, (c + 1) * CH);
      base = mem.slice();
      fresh = mem.length;
    } else {
      const a = new Int32Array(mem.buffer, 0, mem.length >> 2), b = new Int32Array(base.buffer, 0, base.length >> 2);
      const W = CH >> 2;
      for (let c = 0; c < n; c++) {
        let same = true;
        for (let i = c * W, e = Math.min(i + W, a.length); i < e; i++) if (a[i] !== b[i]) { same = false; break; }
        if (same && last.chunks[c]) { chunks[c] = last.chunks[c]; continue; }
        chunks[c] = mem.slice(c * CH, (c + 1) * CH);
        base.set(chunks[c], c * CH);
        fresh += CH;
      }
    }
    const k = { frame, chunks, bytes: fresh, len: mem.length };
    keys.push(k);
    last = k;
    bytes += fresh;
    // Over the budget: the oldest go (but one). Their pieces the next still uses stay with it.
    while (bytes > BUDGET && keys.length > 2) {
      const gone = keys.shift();
      bytes -= gone.bytes;
      const next = keys[0];
      let kept = 0;
      for (let c = 0; c < next.chunks.length; c++) if (next.chunks[c] === gone.chunks[c]) kept += CH;
      next.bytes += kept;
      bytes += kept;
    }
  }

  function restore(k) {
    const mem = mod.HEAPU8;
    for (let c = 0; c < k.chunks.length; c++) mem.set(k.chunks[c], c * CH);
    base = mem.slice();
    last = { ...k, chunks: k.chunks.slice() };
    for (let c = k.chunks.length; c < Math.ceil(mem.length / CH); c++) last.chunks[c] = mem.slice(c * CH, (c + 1) * CH);
  }

  // A new run: frame 0 is the memory now (after the cart's top level and _init).
  T.begin = (m, start) => {
    mod = m;
    keys = [];
    base = last = null;
    bytes = 0;
    T.inputs = [];
    T.clocks = [];
    T.start = start;
    T.edited = false;
    snapshot(0);
  };
  // Frame n ran with this input at this clock.
  T.record = (n, input, clock) => {
    T.inputs.length = T.clocks.length = n - 1;  // (after a rewind: the old future goes)
    T.inputs.push(input);
    T.clocks.push(clock);
    if (n % KEY_EVERY === 0) snapshot(n);
  };
  T.first = () => (keys.length ? keys[0].frame : 0);
  T.end = () => T.inputs.length;
  // Frame n shown: the snapshot at or before it, then the frames from there run quietly (step(input,
  // clock) runs one). False when n is before the oldest snapshot kept.
  T.seek = (n, step) => {
    let k = null;
    for (const key of keys) if (key.frame <= n) k = key;
    if (!k) return false;
    restore(k);
    for (let f = k.frame + 1; f <= n; f++) step(T.inputs[f - 1], T.clocks[f - 1]);
    return true;
  };
  // The run goes on from frame n with something new (play from a rewound frame, new code): the
  // snapshots after it and the input past it go, and frame n becomes a snapshot as it is now.
  T.branch = (n) => {
    const kept = keys.filter((k) => k.frame < n);
    for (const k of keys) if (k.frame >= n) bytes -= k.bytes;
    keys = kept;
    last = null;  // (the next snapshot compares against all of memory: restore() set base to an older one)
    if (keys.length) last = keys[keys.length - 1];
    T.inputs.length = T.clocks.length = n;
    base = null;
    snapshot(n);
  };
  T.memory = () => bytes;

  // ---- replays: the cart, how it started, and every frame's input, small enough for a link -----
  // Inputs run-length coded: [count, held, touching, tx, ty, ax, ay] for each run of the same input.
  T.pack = (hashes) => {
    const runs = [];
    for (const i of T.inputs) {
      const r = runs[runs.length - 1];
      if (r && r[1] === i[0] && r[2] === i[1] && r[3] === i[2] && r[4] === i[3] && r[5] === i[4] && r[6] === i[5]) r[0]++;
      else runs.push([1, ...i]);
    }
    return { v: 1, ...T.start, frames: T.inputs.length, runs, hashes };
  };
  T.unpack = (r) => {
    const inputs = [];
    for (const [n, ...i] of r.runs) for (let k = 0; k < n; k++) inputs.push(i);
    return inputs;
  };
  return T;
})();
