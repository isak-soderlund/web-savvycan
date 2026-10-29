// Reference implementations of the live RE tools' searches, from what was sent, for the browser specs:
// SavvyCAN's Utility::processIntegerSignal and RangeStateWindow::processSignal (Range State), and Single/Multi State's
// logged search (re/retools.cpp discreteLogged). Plain JS, written from the C++ line by line.

/** Utility::processIntegerSignal: a signal's value (Intel or Motorola bit numbering), 0 if the frame is too short. */
export function signalValue(data, startBit, sigSize, littleEndian, isSigned) {
  let result = 0;
  if (data.length < Math.floor((startBit + sigSize) / 8)) return 0;
  let bit = startBit;
  for (let bitpos = 0; bitpos < sigSize; bitpos++) {
    if (bit < 512) {
      const byte = bit >> 3;
      if (byte >= data.length) return 0;
      if (data[byte] & (1 << (bit % 8))) result += littleEndian ? 2 ** bitpos : 2 ** (sigSize - bitpos - 1);
    }
    if (littleEndian) bit++;
    else if (bit % 8 === 0) bit += 15;
    else bit--;
  }
  if (isSigned && result >= 2 ** (sigSize - 1)) result -= 2 ** sigSize;
  return result;
}

const lerp = (a, b, t) => a * (1 - t) + b * t;

/** RangeStateWindow::processSignal (with 64-bit arithmetic, as the fork has it). */
export function rangeLooksGood(frames, startBit, bitLength, sensitivity, bigEndian, isSigned) {
  const n = frames.length;
  if (n < 1) return false;
  const t = (sensitivity - 10) / 240;
  const v = frames.map((d) => signalValue(d, startBit, bitLength, !bigEndian, isSigned));
  let lo = Infinity, hi = -Infinity;
  for (const x of v) { if (x < lo) lo = x; if (x > hi) hi = x; }
  if (lo === hi) return false;
  const range = hi - lo;
  const maxRange = isSigned ? 2 ** (bitLength - 1) : 2 ** bitLength;
  if (range < Math.trunc(lerp(maxRange * 0.01, maxRange * 0.2, t))) return false;
  let cmp = Math.trunc(lerp(range * 0.55, 0, t));
  let overs = 0;
  for (let i = 1; i < n; i++) if (Math.abs(v[i - 1] - v[i]) > cmp) overs++;
  let maxOvers = Math.trunc(lerp(n / 30, 2, t));
  if (overs > maxOvers) return false;
  cmp = Math.trunc(lerp(range * 0.2, 1, t));
  maxOvers = Math.trunc(lerp(8, 2, t));
  overs = 0;
  for (let i = 2; i < n; i++) if (Math.abs((v[i - 2] - v[i - 1]) - (v[i - 1] - v[i])) > cmp) overs++;
  return overs <= maxOvers;
}

/**
 * Range State's candidates for a set of frames: byId = Map(id -> [data arrays]) of the IDs searched; mode 0 big,
 * 1 little, 2 both; sign 0 signed, 1 unsigned, 2 both (the window's combo indexes). SavvyCAN's order.
 */
export function rangeSearch(byId, { minBits, maxBits, granularity, mode, sign, sensitivity }) {
  const sigType = mode + 1, signedType = sign + 1;
  const out = [];
  for (const id of [...byId.keys()].sort((a, b) => a - b)) {
    const frames = byId.get(id);
    const maxLen = Math.max(0, ...frames.map((d) => d.length));
    for (let size = maxBits; size >= minBits; size -= granularity) {
      for (let start = 0; start < maxLen * 8; start += granularity) {
        const tryIt = (big, sgn) => {
          if (rangeLooksGood(frames, start, size, sensitivity, big, sgn)) out.push({ id, startBit: start, bits: size, signed: sgn, bigEndian: big });
        };
        if (sigType & 1) { if (signedType & 1) tryIt(true, true); if (signedType & 2) tryIt(true, false); }
        if (sigType & 2) { if (signedType & 1) tryIt(false, true); if (signedType & 2) tryIt(false, false); }
      }
    }
  }
  return out;
}

const bitOf = (mask, bit) => bit < 512 && (mask[bit >> 3] >> (bit % 8)) & 1;
function field(d, start, bits) {
  if (Math.floor((start + bits + 7) / 8) > d.length) return -1;
  let v = 0;
  for (let k = 0; k < bits; k++) if ((d[(start + k) >> 3] >> ((start + k) % 8)) & 1) v |= 1 << k;
  return v;
}

/**
 * Single/Multi State "From logged data" for one ID's frames: fields of minBits..maxBits bits (Intel numbering) whose
 * first and last bits change, that take exactly `states` values; smallest first, none containing one found.
 */
export function discreteLogged(id, ext, frames, states, minBits, maxBits) {
  const out = [];
  if (frames.length < 2) return out;
  const stride = Math.max(...frames.map((d) => d.length));
  const row = (d) => { const r = new Uint8Array(stride); r.set(d); return r; };
  const first = row(frames[0]);
  const mask = new Uint8Array(64);
  for (const d of frames) { const r = row(d); for (let b = 0; b < stride; b++) mask[b] |= r[b] ^ first[b]; }
  for (let bits = minBits; bits <= maxBits; bits++) {
    for (let start = 0; start + bits <= stride * 8; start++) {
      if (!bitOf(mask, start) || !bitOf(mask, start + bits - 1)) continue;
      if (out.some((m) => m.startBit >= start && m.startBit + m.bits <= start + bits)) continue;
      const seen = new Set();
      let n = 0;
      for (const d of frames) {
        const v = field(d, start, bits);
        if (v < 0) continue;
        n++;
        seen.add(v);
        if (seen.size > states) break;
      }
      if (seen.size !== states) continue;
      out.push({ id, ext, startBit: start, bits, values: [...seen].sort((a, b) => a - b) });
    }
  }
  return out;
}
