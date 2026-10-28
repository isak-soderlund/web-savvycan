// The fake rc1 against the firmware's own unit-test vectors (resolve-can, branch `firmware`:
// firmware/proto/src/serial.rs, gvret.rs, slcan.rs `mod tests`). Run: node --test tests/web/unit/
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const R = require('../fake/rc1.js');

// The firmware tests' DeviceInfo: serial 0123456789ABCDEF01234567, version 1.2.3.
const INFO = { serial: '0123456789ABCDEF01234567', version: [1, 2, 3] };
const ok = () => 'ok';

/** serial.rs test_util::drive: bytes in, every request answered by `dev`, -> [requests, host bytes]. */
function drive(e, bytes, dev = ok) {
  const reqs = [], host = [];
  for (const b of typeof bytes === 'string' ? Buffer.from(bytes, 'latin1') : bytes) {
    const out = [];
    let next = e.feed(b, out);
    while (next) {
      reqs.push(next);
      next = e.answer(dev(next), out);
    }
    host.push(...out);
  }
  return [reqs, host];
}
const str = (a) => Buffer.from(a).toString('latin1');
const kinds = (reqs) => reqs.map((r) => (r.kind === 'open' ? `open${r.listenOnly ? ' lo' : ''}` : r.rate !== undefined ? `${r.kind} ${r.rate}` : r.kind));
const le = (v) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
const setup = (bus0) => [0xf1, 5, ...le(bus0), ...le(0x80000000), 0];
const hostFrame = (op, id, ext, lenByte, data) => [0xf1, op, ...le((id | (ext ? 0x80000000 : 0)) >>> 0), 0, lenByte, ...data, 0];
function enabled() {
  const g = new R.Gvret(INFO);
  const [reqs, host] = drive(g, setup(500000 | 0xc0000000));
  assert.deepEqual(kinds(reqs), ['bitrate 500000', 'dataBitrate 2000000', 'open']);
  assert.equal(host.length, 0);
  assert.ok(g.open);
  return g;
}
function sent(e, bytes, dev = ok) {
  const [reqs] = drive(e, bytes, dev);
  assert.ok(reqs.length <= 1, JSON.stringify(reqs));
  return reqs.length ? reqs[0].frame : null;
}
const event = (e, f, ts) => {
  const out = [];
  e.event({ frame: f, timestampUs: ts, echo: false }, out);
  return out;
};

test('gvret: SavvyCAN master connect', () => {
  const g = new R.Gvret(INFO);
  g.nowUs = 0x0a0b0c0d;
  const [reqs, host] = drive(g, [0xf1, 0x0c, 0xf1, 0x06, 0xf1, 0x07, 0xf1, 0x01, 0xf1, 0x09]);
  assert.equal(reqs.length, 0);
  assert.deepEqual(host, [0xf1, 0x0c, 1, 0xf1, 0x06, 0x00, 0x20, 0xa1, 0x07, 0x00, 0, 0, 0, 0, 0,
    0xf1, 0x07, 123, 0, 0x20, 0, 0, 0, 0xf1, 0x01, 0x0d, 0x0c, 0x0b, 0x0a, 0xf1, 0x09, 0xde, 0xad]);
  assert.deepEqual(drive(g, [0xf1, 0x0d])[1], [0xf1, 0x0d, ...new Array(15).fill(0)]);
});

test('gvret: QT6WIP connect asks GET_FD', () => {
  const g = new R.Gvret(INFO);
  const [, host] = drive(g, [0xf1, 0x0c, 0xf1, 0x06, 0xf1, 0x07, 0xf1, 22, 0xf1, 0x01, 0xf1, 0x09]);
  assert.ok(str(host).includes(str([0xf1, 22, 1, 0x03, 0x80, 0x84, 0x1e, 0x00])));
  assert.equal(host.length, 3 + 12 + 8 + 8 + 6 + 4);
});

test('gvret: setup enable, disable, restart', () => {
  const g = enabled();
  assert.deepEqual(drive(g, [0xf1, 6])[1], [0xf1, 6, 0x01, 0x20, 0xa1, 0x07, 0, 0, 0, 0, 0, 0]);
  assert.deepEqual(kinds(drive(g, setup(250000 | 0xc0000000))[0]), ['close', 'bitrate 250000', 'dataBitrate 2000000', 'open']);
  assert.deepEqual(kinds(drive(g, setup(250000 | 0xe0000000))[0]).pop(), 'open lo');
  assert.equal(drive(g, [0xf1, 6])[1][2], 0x11);
  assert.deepEqual(kinds(drive(g, setup(250000 | 0x80000000))[0]), ['close']);
  assert.ok(!g.open);
  assert.equal(kinds(drive(g, setup(125000))[0]).pop(), 'open');
  assert.deepEqual(kinds(drive(g, setup(0))[0]), ['close']);
  assert.equal(drive(g, setup(0xc0000000 | 0xfffff))[0][0].rate, 1000000);
  assert.deepEqual(kinds(drive(g, setup(0x80000000))[0]), ['close']);
  assert.equal(drive(g, setup(0xc0000000))[0][0].rate, 1000000);
});

test('gvret: setup refused (gs_usb owns the channel)', () => {
  const g = new R.Gvret(INFO);
  const [reqs, host] = drive(g, setup(500000 | 0xc0000000), () => 'refused');
  assert.deepEqual([kinds(reqs), host], [['bitrate 500000'], []]);
  assert.ok(!g.open);
  assert.equal(drive(g, [0xf1, 6])[1][2], 0);
  const [r2] = drive(g, setup(500000 | 0xc0000000), (r) => (r.kind === 'open' ? 'refused' : 'ok'));
  assert.equal(r2.length, 3);
  assert.ok(!g.open);
  assert.equal(sent(g, hostFrame(0, 0x123, false, 1, [1])), null);
});

test('gvret: SETUP_FD', () => {
  const g = enabled();
  assert.deepEqual(kinds(drive(g, [0xf1, 21, 0, 0x02, ...le(5000000)])[0]), ['close', 'bitrate 500000', 'dataBitrate 5000000', 'open']);
  assert.deepEqual(drive(g, [0xf1, 22])[1], [0xf1, 22, 1, 0x03, 0x40, 0x4b, 0x4c, 0x00]);
  assert.equal(drive(g, [0xf1, 21, 0, 0x00, 0, 0, 0, 0])[0][2].rate, 0);
  assert.equal(drive(g, [0xf1, 22])[1][3], 0x01);
  assert.equal(sent(g, hostFrame(20, 0x10, false, 0x81, [7])), null);
  assert.equal(drive(g, [0xf1, 21, 0, 0x02, 0x40, 0x42, 0x0f, 0x00])[0][2].rate, 1000000);
  assert.equal(drive(g, [0xf1, 21, 0, 0x02, 1, 0, 0, 0])[0].length, 0);
  assert.equal(drive(g, [0xf1, 21, 1, 0x00, 0, 0, 0, 0])[0].length, 0);
  drive(g, setup(0));
  assert.equal(drive(g, [0xf1, 21, 0, 0x02, 0x00, 0x12, 0x7a, 0x00])[0].length, 0);
  assert.equal(drive(g, setup(500000 | 0xc0000000))[0][1].rate, 8000000);
});

test('gvret: host classic and FD frames', () => {
  const g = enabled();
  let f = sent(g, hostFrame(0, 0x123, false, 3, [1, 2, 3]));
  assert.deepEqual([f.id, f.ext, f.fd, f.dlc, [...f.data.slice(0, 3)]], [0x123, false, false, 3, [1, 2, 3]]);
  f = sent(g, hostFrame(0, 0x1abcdef0, true, 8, new Array(8).fill(9)));
  assert.deepEqual([f.id, f.ext, f.dlc, f.data[7]], [0x1abcdef0, true, 8, 9]);
  const burst = [...hostFrame(0, 1, false, 1, [0xf1]), ...hostFrame(0, 2, false, 2, [0xf1, 0xf1])];
  assert.equal(drive(g, burst)[0].length, 2);
  const two = hostFrame(0, 3, false, 1, [3]);
  two.pop();
  assert.equal(drive(g, [...two, ...hostFrame(0, 4, false, 0, [])])[0].length, 2);
  assert.equal(sent(g, hostFrame(0, 0x800, false, 1, [0])), null);
  assert.equal(sent(g, hostFrame(0, 0x20000000, true, 1, [0])), null);
  const other = hostFrame(0, 0x10, false, 1, [0]);
  other[6] = 1;
  assert.equal(sent(g, other), null);
  assert.ok(sent(g, hostFrame(0, 0x10, false, 1, [0])));
  for (let len = 0; len <= 64; len++) {
    const data = Array.from({ length: len }, (_, i) => i);
    for (const brs of [false, true]) {
      f = sent(g, hostFrame(20, 0x12345678, true, len | (brs ? 0x80 : 0), data));
      assert.ok(f.fd && f.ext && f.brs === brs);
      assert.equal(f.dlc, R.lenToDlc(len));
      assert.deepEqual([...f.data.slice(0, len)], data);
    }
  }
  f = sent(g, hostFrame(0, 0x10, false, 0x80 | 12, new Array(12).fill(0)));
  assert.deepEqual([f.fd, f.brs, f.dlc], [true, false, 9]);
  const [reqs, host] = drive(g, [...hostFrame(20, 0x10, false, 65, new Array(65).fill(0)), 0xf1, 0x09]);
  assert.equal(reqs.length, 0);
  assert.deepEqual(host, [0xf1, 0x09, 0xde, 0xad]);
  drive(g, setup(500000 | 0xe0000000));
  assert.equal(sent(g, hostFrame(0, 0x10, false, 1, [0])), null);
});

test('gvret: frames to the host', () => {
  const g = enabled();
  let f = R.frame({ id: 0x123, data: [1, 2, 3] });
  assert.deepEqual(event(g, f, 0x11223344), [0xf1, 0, 0x44, 0x33, 0x22, 0x11, 0x23, 0x01, 0, 0, 3, 1, 2, 3, 0]);
  f = R.frame({ id: 0x1abcdef0, ext: true, data: [1, 2, 3] });
  assert.deepEqual(event(g, f, 0).slice(6, 11), [0xf0, 0xde, 0xbc, 0x9a, 3]);
  f = R.frame({ id: 0x1abcdef0, ext: true, rtr: true, dlc: 3 });
  assert.deepEqual(event(g, f, 0).slice(10), [0, 0]);
  for (let dlc = 0; dlc < 16; dlc++) {
    const len = R.dlcToLen(dlc, true);
    const fd = R.frame({ id: 0x7ff, fd: true, brs: dlc % 2 === 1, dlc, data: Array.from({ length: 64 }, (_, i) => i) });
    const want = [0xf1, 20, 7, 0, 0, 0, 0xff, 0x07, 0, 0, len | (fd.brs ? 0x80 : 0), 0, ...Array.from({ length: len }, (_, i) => i), 0];
    assert.deepEqual(event(g, fd, 7), want);
  }
  assert.ok(!g.event({ frame: f, timestampUs: 0, echo: true }, []));
  assert.ok(!g.event({ frame: R.ERROR_FRAMES.busoff(), timestampUs: 0, echo: false }, []));
  drive(g, setup(0));
  assert.equal(event(g, f, 0).length, 0);
});

test('gvret: unknown and ignored commands keep sync, echo', () => {
  const g = new R.Gvret(INFO);
  const bytes = [0x00, 0xe7, 0xf1, 0x55, 0x09, 0xf1, 0xf1, 0x09, 0xf1, 4, 0x09, 0xf1, 8, 0x10, 0xf1, 10, 0x09,
    0xf1, 14, ...new Array(12).fill(0x09), 0, 0xf1, 0x09];
  const [reqs, host] = drive(g, bytes);
  assert.equal(reqs.length, 0);
  assert.deepEqual(host, [0xf1, 0x09, 0xde, 0xad, 0xf1, 0x09, 0xde, 0xad]);
  assert.deepEqual(drive(g, [0xf1, 2])[1], [0xf1, 2, 0, 0]);
  const e = new R.Gvret(INFO);
  e.nowUs = 5;
  assert.deepEqual(drive(e, hostFrame(11, 0x55, false, 2, [0xaa, 0xbb])), [[], [0xf1, 0, 5, 0, 0, 0, 0x55, 0, 0, 0, 2, 0xaa, 0xbb, 0]]);
});

test('gvret: random bytes never throw', () => {
  const g = new R.Gvret(INFO);
  let x = 0x0badcafe;
  const rnd = () => ((x ^= x << 13), (x ^= x >>> 17), (x ^= x << 5), x >>> 0);
  for (let i = 0; i < 100000; i++) {
    const r = rnd();
    const b = r % 5 === 0 ? 0xf1 : r % 5 === 1 ? (r >>> 8) % 24 : (r >>> 16) & 0xff;
    const out = [];
    let next = g.feed(b, out);
    while (next) next = g.answer(r & 0x100 ? 'ok' : 'refused', out);
  }
});

test('slcan: SavvyCAN and slcand connect', () => {
  let s = new R.Slcan(INFO);
  let [reqs, host] = drive(s, 'C\rS6\rY2\rO\r');
  assert.deepEqual(kinds(reqs), ['close', 'bitrate 500000', 'dataBitrate 2000000', 'bitrate 500000', 'dataBitrate 2000000', 'open']);
  assert.equal(str(host), '\r\r\r\r');
  assert.ok(s.open);
  assert.deepEqual(kinds(drive(s, 'C\r')[0]), ['close']);
  s = new R.Slcan(INFO);
  [reqs, host] = drive(s, 'C\rS5\rF\rL\r');
  assert.deepEqual(kinds(reqs), ['close', 'bitrate 250000', 'bitrate 250000', 'dataBitrate 2000000', 'open lo']);
  assert.equal(str(host), '\r\rF00\r\r');
  assert.equal(str(drive(s, 't1230\r')[1]), '\x07');
  s = new R.Slcan(INFO);
  assert.equal(drive(s, 'C\rs031C\r')[0][1].rate, 125000);
});

test('slcan: bit rates, refusals, misc commands, version', () => {
  const s = new R.Slcan(INFO);
  [10000, 20000, 50000, 100000, 125000, 250000, 500000, 800000, 1000000, 83333].forEach((r, i) =>
    assert.deepEqual([kinds(drive(s, `S${i}\r`)[0]), str(drive(s, `S${i}\r`)[1])], [[`bitrate ${r}`], '\r']));
  for (const bad of ['S\r', 'SA\r', 'S10\r', 'Y0\r', 'Y9\r', 's01\r', 's0G1C\r']) assert.equal(str(drive(s, bad)[1]), '\x07', bad);
  assert.equal(drive(s, 's001C\r')[0][0].rate, 500000);
  assert.equal(drive(s, 's0014\r')[0][0].rate, 1000000);
  const [r2, h2] = drive(s, 'S4\r', () => 'refused');
  assert.deepEqual([r2.length, str(h2)], [1, '\x07']);
  assert.equal(s.bitrate, 1000000);
  for (const c of ['M0', 'M00000000', 'm7FFfffff', 'A1', 'X1', 'X0', 'U1', 'Z1', 'Z0', '']) assert.equal(str(drive(s, `${c}\r`)[1]), '\r', c);
  for (const c of ['A0', 'P', 'A', 'Q1', 'W2', 'E', 'Z2', 'M2', 'M0000000G', 'x', 'O1', 'C1']) assert.equal(str(drive(s, `${c}\r`)[1]), '\x07', c);
  assert.equal(str(drive(s, 'V\r')[1]), 'V1012\r');
  assert.equal(str(drive(s, 'v\r')[1]), 'v0102\r');
  assert.equal(str(drive(s, 'N\r')[1]), 'N0123456789ABCDEF01234567\r');
});

test('slcan: transmit lines', () => {
  const s = new R.Slcan(INFO);
  drive(s, 'O\r');
  let [reqs, host] = drive(s, 't12381122334455667788\r');
  assert.deepEqual([reqs[0].frame.id, reqs[0].frame.dlc, str(host)], [0x123, 8, 'z\r']);
  [reqs, host] = drive(s, 'T1ABCDEF02aa55\r');
  assert.deepEqual([reqs[0].frame.id, reqs[0].frame.ext, str(host)], [0x1abcdef0, true, 'Z\r']);
  assert.equal(drive(s, 'T923456780\r')[0][0].frame.id, 0x12345678);
  for (const bad of ['t8000', 't1231', 't1231111', 't12311122', 't1239', 't123A', 't12G0', 't1231GG', 'r1231AA', 'T1234567', 'd1239', 'd12310', 'd12316', 't', 'T']) {
    assert.deepEqual(drive(s, `${bad}\r`), [[], [7]], bad);
  }
  for (let dlc = 0; dlc < 16; dlc++) {
    const len = R.dlcToLen(dlc, true);
    const data = Array.from({ length: len }, (_, i) => ((i ^ 0xa5) & 0xff).toString(16).padStart(2, '0').toUpperCase()).join('');
    const f = drive(s, `B1FFFFFFF${dlc.toString(16).toUpperCase()}${data}\r`)[0][0].frame;
    assert.deepEqual([f.fd, f.brs, f.ext, f.dlc, R.frameLen(f)], [true, true, true, dlc, len]);
    if (dlc >= 10) assert.equal(drive(s, `d7FF${dlc}${data}\r`)[0][0].frame.dlc, dlc);
  }
});

test('slcan: receive lines, timestamps, error lines', () => {
  const s = new R.Slcan(INFO);
  drive(s, 'O\r');
  const line = (f, ts = 0) => str(event(s, f, ts));
  assert.equal(line(R.frame({ id: 0x123, data: [1, 0xab, 0xff] })), 't123301ABFF\r');
  assert.equal(line(R.frame({ id: 0x1abcdef0, ext: true, data: [1, 0xab, 0xff] })), 'T1ABCDEF0301ABFF\r');
  assert.equal(line(R.frame({ id: 0x1abcdef0, ext: true, rtr: true, dlc: 3 })), 'R1ABCDEF03\r');
  assert.equal(line(R.frame({ id: 0x7ff, rtr: true, dlc: 0 })), 'r7FF0\r');
  const big = R.frame({ id: 0x1234567, ext: true, fd: true, brs: true, dlc: 15, data: Array.from({ length: 64 }, (_, i) => i) });
  const hex = Array.from({ length: 64 }, (_, i) => i.toString(16).padStart(2, '0').toUpperCase()).join('');
  assert.equal(line(big), `B01234567F${hex}\r`);
  drive(s, 'Z1\r');
  const t = R.frame({ id: 0x10 });
  const ts = new R.Slcan(INFO);
  drive(ts, 'O\rZ1\r');
  const tl = (x) => str(event(ts, t, x));
  assert.equal(tl(1234567), 't010004D2\r');
  assert.equal(tl(59999999), 't0100EA5F\r');
  assert.equal(tl(60000000), 't01000000\r');
  const w = new R.Slcan(INFO);
  drive(w, 'O\rZ1\r');
  const wl = (x) => str(event(w, t, x));
  assert.equal(wl(0xffffffff - 4999999), `t0100${(29967).toString(16).toUpperCase().padStart(4, '0')}\r`);
  assert.equal(wl(0xffffffff), `t0100${(34967).toString(16).toUpperCase().padStart(4, '0')}\r`);
  assert.equal(wl(999), `t0100${(34968).toString(16).toUpperCase().padStart(4, '0')}\r`);
  assert.equal(wl(0), `t0100${(34967).toString(16).toUpperCase().padStart(4, '0')}\r`);
  const e = new R.Slcan(INFO);
  drive(e, 'O\r');
  assert.equal(str(event(e, R.ERROR_FRAMES.warning(97, 5), 0)), 'sw005097\r');
  assert.equal(str(event(e, R.ERROR_FRAMES.busoff(), 0)), 'sb000255\r');
  assert.equal(str(event(e, R.ERROR_FRAMES.overflow(), 0)), 'e1o\r');
  assert.equal(str(drive(e, 'F\r')[1]), 'F8C\r');
  assert.equal(str(drive(e, 'F\r')[1]), 'F00\r');
});

test('port: starts in slcan, E7 E7 to GVRET, reset back', () => {
  const p = new R.Port(INFO);
  assert.deepEqual([kinds(drive(p, 'S4\r')[0]), str(drive(p, 'S4\r')[1])], [['bitrate 125000'], '\r']);
  const [reqs, host] = drive(p, [0xe7, 0xe7, 0xf1, 0x0c]);
  assert.deepEqual([reqs.length, host], [0, [0xf1, 0x0c, 1]]);
  assert.equal(p.mode, 'gvret');
  assert.deepEqual(drive(p, [...Buffer.from('O\r'), 0xe7, 0xe7]), [[], []]);
  p.reset();
  assert.equal(p.mode, 'slcan');
  assert.equal(str(drive(p, 'V\r')[1]), 'V1012\r');
  const q = new R.Port(INFO);
  assert.equal(str(drive(q, [0xe7, ...Buffer.from('O\r'), 0xe7, 0x0d])[1]), '\x07\x07');
  assert.equal(q.mode, 'slcan');
  const c = new R.Port(INFO);
  drive(c, 'O\r');
  assert.deepEqual(drive(c, [0xe7, 0xe7]), [[{ kind: 'close' }], []]);
  assert.equal(drive(c, [0xf1, 6])[1][2], 0);
});

test('device: a SavvyCAN GVRET session, traffic, loss when the host does not read, session end', () => {
  let now = 0;
  const d = new R.Rc1Device({ now: () => now, bufferBytes: 4096 });
  d.setControl({ dtr: true, rts: true, coding: [1000000, 8, 1, 'none'] });
  d.setControl({ dtr: false, rts: true }); // ESP32 mode: DTR low -> falling edge: session reset (harmless here)
  d.setControl({ dtr: false, rts: false });
  d.write([0xe7, 0xe7, 0xf1, 0x0c, 0xf1, 0x06, 0xf1, 0x07, 0xf1, 22, 0xf1, 0x01, 0xf1, 0x09]);
  const p = new R.GvretHostParser();
  p.push(d.read());
  assert.deepEqual(p.replies[12], [1]);
  assert.deepEqual(p.replies[22], [1, 3, 0x80, 0x84, 0x1e, 0]);
  assert.deepEqual(p.replies[7], [13, 0, 0x20, 0, 0, 0], 'build 0*100+1*10+3');
  d.write(setup(500000 | 0xc0000000));
  d.write([0xf1, 21, 0, 0x02, ...le(2000000)]);
  assert.ok(d.chan.open && d.chan.fdMode);
  d.setTraffic('fd64-full');
  now = 999.9;
  // 1 s of 6 900 frames/s with nobody reading: the buffer holds ~4096/77 frames, the rest is lost.
  const st = d.stats();
  assert.equal(st.bus, 6900);
  assert.equal(st.encoded + st.lost, 6900);
  assert.equal(st.encoded, Math.floor(4096 / 77));
  // Now read as fast as it comes for 1 s: nothing more is lost.
  let frames = [];
  p.onFrame = (f) => frames.push(f);
  for (let t = 1001; t <= 2000; t++) {
    now = t;
    p.push(d.read(4096));
  }
  const st2 = d.stats();
  assert.ok(st2.lost - st.lost <= 7, 'only the frames due while the buffer was still full');
  assert.equal(st2.encoded, frames.length);
  const sc = R.seqCheck(frames.map((f) => f.data));
  assert.equal(sc.gaps, 1, 'one gap: the lost frames');
  assert.equal(sc.first, 0);
  assert.equal(sc.missing, st2.lost);
  assert.ok(frames.every((f) => f.fd && f.brs && f.data.length === 64));
  // The host closes the port: session over, channel closed, slcan again.
  d.hostClosed();
  assert.equal(d.port.mode, 'slcan');
  assert.ok(!d.chan.open);
});

test('device: slcan session with the leaf table and error lines', () => {
  let now = 0;
  const d = new R.Rc1Device({ now: () => now, bufferBytes: 1e6 });
  d.setControl({ dtr: true, rts: true, coding: [115200, 8, 1, 'none'] });
  d.write(Buffer.from('C\rS6\rO\r'));
  assert.equal(str(d.read()), '\r\r\r');
  d.setTraffic('errors');
  now = 1000;
  const text = str(d.read());
  const lines = text.split('\r').filter(Boolean);
  const frames = lines.filter((l) => l[0] === 't');
  const st = d.stats();
  assert.equal(frames.length, st.encoded);
  assert.ok(st.bus >= 830 && st.bus <= 840, `${st.bus} frames/s`);
  assert.ok(lines.some((l) => l.startsWith('sw')) && lines.some((l) => l.startsWith('sb')) && lines.includes('e1o'));
});

test('device: refused open (gs_usb owns the channel)', () => {
  const d = new R.Rc1Device({ refuseOpen: true });
  d.write(Buffer.from('O\r'));
  assert.equal(str(d.read()), '\x07');
  d.write([0xe7, 0xe7, ...setup(500000 | 0xc0000000), 0xf1, 6]);
  assert.equal(d.read()[2], 0, 'GVRET reports the bus disabled');
});

test('csv parser reads SavvyCAN native rows, FD flags included', () => {
  const t = 'Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8\n' +
    '1000,00000123,false,Rx,0,2,01,02,00,00,00,00,00,00,\n' +
    '2000,1ABCDEF0,true,Rx,0,12,00,01,02,03,04,05,06,07,08,09,0A,0B,FD+BRS,\n';
  const { frames } = R.parseSavvyCsv(t);
  assert.deepEqual(frames[0].data, [1, 2]);
  assert.deepEqual([frames[1].id, frames[1].ext, frames[1].fd, frames[1].brs, frames[1].data.length], [0x1abcdef0, true, true, true, 12]);
});
