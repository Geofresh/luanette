// A small GIF encoder, for the playground's Record GIF and the command-line tool's `run --gif`.
// Frames are RGBA; colors are matched to a palette of the cart's 16 colors and a 6x6x6 cube (so a
// cart that draws only its 16 comes out exact), then LZW-compressed. The GIF loops forever.
"use strict";

const Gif = (() => {
  // The palette: the cart's colors ("#rrggbb" each, up to 16), then a 6x6x6 cube; 256 entries in all.
  function palette(colors) {
    const pal = [];
    for (const c of colors.slice(0, 16)) {
      const n = parseInt(c.replace("#", ""), 16);
      pal.push([n >> 16, (n >> 8) & 255, n & 255]);
    }
    for (let r = 0; r < 6; r++) for (let g = 0; g < 6; g++) for (let b = 0; b < 6; b++) pal.push([r * 51, g * 51, b * 51]);
    while (pal.length < 256) pal.push([0, 0, 0]);
    return pal;
  }

  // Each pixel's palette index: an exact color first, else the nearest (remembered by color).
  function indexer(pal) {
    const memo = new Map();
    pal.forEach(([r, g, b], i) => { const k = (r << 16) | (g << 8) | b; if (!memo.has(k)) memo.set(k, i); });
    return (r, g, b) => {
      const k = (r << 16) | (g << 8) | b;
      let i = memo.get(k);
      if (i !== undefined) return i;
      let best = 1e9;
      for (let j = 0; j < pal.length; j++) {
        const dr = pal[j][0] - r, dg = pal[j][1] - g, db = pal[j][2] - b, d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
        if (d < best) { best = d; i = j; }
      }
      memo.set(k, i);
      return i;
    };
  }

  // GIF's LZW: 8-bit pixels, codes from 9 to 12 bits, a clear code when the table fills.
  function lzw(pixels) {
    const out = [];
    let acc = 0, bits = 0;
    const put = (code, size) => {
      acc |= code << bits;
      bits += size;
      while (bits >= 8) { out.push(acc & 255); acc >>>= 8; bits -= 8; }
    };
    const CLEAR = 256, END = 257;
    let dict = new Map(), next = 258, size = 9;
    put(CLEAR, size);
    let prefix = pixels[0];
    for (let i = 1; i < pixels.length; i++) {
      const p = pixels[i], key = prefix * 256 + p, hit = dict.get(key);
      if (hit !== undefined) { prefix = hit; continue; }
      put(prefix, size);
      if (next < 4096) {
        dict.set(key, next++);
        if (next > (1 << size) && size < 12) size++;
      } else {  // full: start the table again
        put(CLEAR, size);
        dict = new Map();
        next = 258;
        size = 9;
      }
      prefix = p;
    }
    put(prefix, size);
    put(END, size);
    if (bits) out.push(acc & 255);
    return out;
  }

  // frames: [{rgba (Uint8ClampedArray or Uint8Array, w*h*4)}]; delay in 1/100 s. A Uint8Array.
  function encode(frames, w, h, delay, colors) {
    const pal = palette(colors), index = indexer(pal);
    const bytes = [];
    const word = (n) => bytes.push(n & 255, (n >> 8) & 255);
    const text = (s) => { for (const c of s) bytes.push(c.charCodeAt(0)); };
    text("GIF89a");
    word(w); word(h);
    bytes.push(0xf7, 0, 0);  // a global table of 256 colors
    for (const [r, g, b] of pal) bytes.push(r, g, b);
    bytes.push(0x21, 0xff, 11); text("NETSCAPE2.0"); bytes.push(3, 1, 0, 0, 0);  // loop forever
    const px = new Uint8Array(w * h);
    for (const f of frames) {
      for (let i = 0, j = 0; i < px.length; i++, j += 4) px[i] = index(f.rgba[j], f.rgba[j + 1], f.rgba[j + 2]);
      bytes.push(0x21, 0xf9, 4, 0); word(delay); bytes.push(0, 0);  // its delay
      bytes.push(0x2c); word(0); word(0); word(w); word(h); bytes.push(0);
      bytes.push(8);  // LZW minimum code size
      const data = lzw(px);
      for (let i = 0; i < data.length; i += 255) {
        const n = Math.min(255, data.length - i);
        bytes.push(n);
        for (let k = 0; k < n; k++) bytes.push(data[i + k]);
      }
      bytes.push(0);
    }
    bytes.push(0x3b);
    return Uint8Array.from(bytes);
  }

  return { encode, palette };
})();

if (typeof module !== "undefined") module.exports = Gif;
