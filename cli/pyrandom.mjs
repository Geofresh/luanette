// Python's random.Random(seed).random(), exactly: the Mersenne Twister, seeded as Python seeds it
// from an int (init_by_array). check.py's scripted input draws taps from it, and the command-line
// tool feeds a cart the same input.
export function pyRandom(seed) {
  const N = 624, mt = new Uint32Array(N);
  let mti = N + 1;
  const init = (s) => {
    mt[0] = s >>> 0;
    for (mti = 1; mti < N; mti++) {
      const p = mt[mti - 1] ^ (mt[mti - 1] >>> 30);
      mt[mti] = (Math.imul(1812433253, p) + mti) >>> 0;
    }
  };
  init(19650218);
  const key = [seed >>> 0];
  let i = 1, j = 0;
  for (let k = Math.max(N, key.length); k; k--) {
    const p = mt[i - 1] ^ (mt[i - 1] >>> 30);
    mt[i] = ((mt[i] ^ Math.imul(p, 1664525)) + key[j] + j) >>> 0;
    i++; j++;
    if (i >= N) { mt[0] = mt[N - 1]; i = 1; }
    if (j >= key.length) j = 0;
  }
  for (let k = N - 1; k; k--) {
    const p = mt[i - 1] ^ (mt[i - 1] >>> 30);
    mt[i] = ((mt[i] ^ Math.imul(p, 1566083941)) - i) >>> 0;
    i++;
    if (i >= N) { mt[0] = mt[N - 1]; i = 1; }
  }
  mt[0] = 0x80000000;
  const next = () => {
    if (mti >= N) {
      for (let kk = 0; kk < N; kk++) {
        const y = (mt[kk] & 0x80000000) | (mt[(kk + 1) % N] & 0x7fffffff);
        mt[kk] = mt[(kk + 397) % N] ^ (y >>> 1) ^ (y & 1 ? 0x9908b0df : 0);
      }
      mti = 0;
    }
    let y = mt[mti++];
    y ^= y >>> 11;
    y ^= (y << 7) & 0x9d2c5680;
    y ^= (y << 15) & 0xefc60000;
    y ^= y >>> 18;
    return y >>> 0;
  };
  return () => {
    const a = next() >>> 5, b = next() >>> 6;
    return (a * 67108864 + b) / 9007199254740992;
  };
}
