// The Web Serial bridge (webserial.js) in Node against the test harness's fake rc1 and navigator.serial mock
// (tests/web/fake/rc1.js: the firmware's GVRET/slcan, Chrome's Web Serial behaviour), standing in for the C++ side:
// what SavvyCAN's GVRET/LAWICEL do through the QSerialPort stand-in, as calls and callbacks.
//   node --test tests/web/unit/bridge.test.mjs   (tests/web/run.sh runs it)
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

const here = dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const BRIDGE = readFileSync(join(here, '../../../wasm/webserial/webserial.js'), 'utf8');
const RC1Fake = require('../fake/rc1.js');

const RC1 = 'WebSerial 1209:0001';
const GVRET_HELLO = [0xe7, 0xe7, 0xf1, 0x0c, 0xf1, 0x06, 0xf1, 0x07, 0xf1, 22, 0xf1, 0x01, 0xf1, 0x09];
const ESP_OPTIONS = { baudRate: 1000000, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none' };

// A fresh mock and bridge per test: webserial.js runs as Emscripten's --pre-js would, with `Module` in scope.
function fresh(serialOptions = {}) {
  const rc1 = RC1Fake.installWebSerial({ requireGesture: false, ...serialOptions });
  const events = [];
  const waiters = [];
  const push = (e) => {
    events.push(e);
    for (const w of waiters.splice(0)) w();
  };
  const Module = {
    webSerialOpened: (h, ok, err, msg) => push({ type: 'opened', h, ok, err, msg }),
    webSerialData: (h, bytes) => push({ type: 'data', h, bytes: Uint8Array.from(bytes) }),
    webSerialError: (h, err, msg) => push({ type: 'error', h, err, msg }),
    webSerialReadError: (h, msg) => push({ type: 'readError', h, msg }),
    webSerialPortsChanged: () => push({ type: 'ports' }),
    webSerialPortChosen: (name, error) => push({ type: 'chosen', name, error }),
  };
  const bridge = vm.runInThisContext(`(function (Module) { ${BRIDGE}\n; return globalThis.resolveWebSerial; })`)(Module);
  const until = async (pred, ms = 3000) => {
    const end = Date.now() + ms;
    for (;;) {
      const hit = pred() || events.find((e) => false);
      if (hit) return hit;
      if (Date.now() > end) throw new Error('timeout; events: ' + JSON.stringify(events.map((e) => e.type)));
      await new Promise((r) => { waiters.push(r); setTimeout(r, 10); });
    }
  };
  const event = (pred, ms) => until(() => events.find(pred), ms);
  const received = (h) => Uint8Array.from(events.filter((e) => e.type === 'data' && e.h === h).flatMap((e) => Array.from(e.bytes)));
  const closeAll = async () => {
    for (let h = 1; h < 10; h++) bridge.close(h);
    await new Promise((r) => setTimeout(r, 30));
  };
  return { rc1, bridge, events, until, event, received, closeAll };
}

// "f1 0c 01" in the byte stream, from `at`
function find(rx, bytes, at = 0) {
  for (let k = at; k + bytes.length <= rx.length; k++) if (bytes.every((b, j) => rx[k + j] === b)) return k;
  return -1;
}

test('granted ports are listed under stable names, without a prompt', async () => {
  const { bridge, event } = fresh({ granted: true, ports: 2 });
  await bridge.ready();
  await event((e) => e.type === 'ports');
  assert.deepEqual(bridge.ports().map((p) => [p.name, p.description]), [
    [RC1, 'resolve-can (rc1)'], [RC1 + ' #2', 'resolve-can (rc1)'],
  ]);
});

test('GVRET session: 16 MiB buffer, DTR/RTS low, handshake, bus set up, frames both ways, close', async () => {
  const { rc1, bridge, until, event, received } = fresh({ granted: true });
  await bridge.ready();
  bridge.open(1, RC1, ESP_OPTIONS);
  bridge.setSignals(1, false, false, false); // before the open finished: applied right after it
  const opened = await event((e) => e.type === 'opened');
  assert.equal(opened.ok, true, opened.msg);
  const port = rc1.ports[0];
  assert.equal(port._opt.baudRate, 1000000);
  assert.equal(port._opt.bufferSize, 16 * 1024 * 1024); // Chrome's maximum: the most room while the page stalls
  assert.deepEqual(port._signals, { dataTerminalReady: false, requestToSend: false, break: false });

  bridge.write(1, Uint8Array.from(GVRET_HELLO));
  await until(() => find(received(1), [0xf1, 0x09, 0xde, 0xad]) >= 0);
  const rx = received(1);
  assert.ok(find(rx, [0xf1, 0x0c, 1]) >= 0, 'one bus');
  assert.ok(find(rx, [0xf1, 22]) >= 0, 'FD settings answered');
  assert.equal(rc1.stats().mode, 'gvret');

  // SETUP_CANBUS: 500 kbit/s, enabled; SETUP_FD: FD at 2 Mbit/s.
  const word = (0x80000000 | 0x40000000 | 500000) >>> 0;
  bridge.write(1, Uint8Array.from([0xf1, 5, word & 0xff, (word >>> 8) & 0xff, (word >>> 16) & 0xff, word >>> 24, 0, 0, 0, 0]));
  bridge.write(1, Uint8Array.from([0xf1, 21, 0, 0x03, 0x80, 0x84, 0x1e, 0x00]));
  await until(() => rc1.stats().channel.open && rc1.stats().channel.fdMode);

  const before = received(1).length;
  rc1.inject({ id: 0x123, data: [1, 2, 3, 4, 5, 6, 7, 8] });
  rc1.inject({ id: 0x456, fd: true, brs: true, data: Array.from({ length: 64 }, (_, i) => i) });
  await until(() => received(1).length >= before + 20 + 77);
  const frames = received(1).slice(before);
  assert.deepEqual(Array.from(frames.slice(0, 2)), [0xf1, 0]);
  assert.deepEqual(Array.from(frames.slice(6, 10)), [0x23, 0x01, 0, 0]);
  assert.ok(find(frames, [0xf1, 20]) > 0, 'the FD frame');

  bridge.write(1, Uint8Array.from([0xf1, 0, 0x21, 0x03, 0, 0, 0, 4, 0xde, 0xad, 0xbe, 0xef, 0]));
  await until(() => rc1.requests().some((r) => r.kind === 'send'));

  bridge.close(1);
  await until(() => !port._isOpen);
  assert.equal(bridge.sessions(), 0);
  assert.equal(rc1.stats().lost, 0);
});

test('full-load traffic: every frame reaches the bridge, none lost in rc1', async () => {
  const { rc1, bridge, until, event, received, closeAll } = fresh({ granted: true });
  await bridge.ready();
  bridge.open(1, RC1, ESP_OPTIONS);
  await event((e) => e.type === 'opened');
  const word = (0x80000000 | 0x40000000 | 1000000) >>> 0;
  bridge.write(1, Uint8Array.from([0xe7, 0xe7, 0xf1, 5, word & 0xff, (word >>> 8) & 0xff, (word >>> 16) & 0xff, word >>> 24, 0, 0, 0, 0]));
  await until(() => rc1.stats().channel.open);
  rc1.setTraffic('classic-full', { seconds: 0.5 });
  await new Promise((r) => setTimeout(r, 800));
  received(1); // (bytes so far)
  const st = rc1.stats();
  assert.ok(st.encoded > 1000, `frames encoded: ${st.encoded}`);
  assert.equal(st.lost, 0);
  await until(() => received(1).length >= st.encoded * 20);
  await closeAll();
});

test('close then reopen at once: the reopen waits for the close (SavvyCAN retries this way)', async () => {
  const { rc1, bridge, event, closeAll } = fresh({ granted: true });
  await bridge.ready();
  bridge.open(1, RC1, ESP_OPTIONS);
  await event((e) => e.type === 'opened' && e.h === 1);
  bridge.close(1);
  bridge.open(2, RC1, { ...ESP_OPTIONS, flowControl: 'hardware' });
  const o2 = await event((e) => e.type === 'opened' && e.h === 2);
  assert.equal(o2.ok, true, o2.msg);
  assert.equal(rc1.stats().sessions, 2);
  assert.equal(rc1.ports[0]._opt.flowControl, 'hardware');
  await closeAll();
});

test('close while the open is still running: the port ends up closed, no answer', async () => {
  const { rc1, bridge, events } = fresh({ granted: true });
  await bridge.ready();
  bridge.open(1, RC1, ESP_OPTIONS);
  bridge.close(1);
  await new Promise((r) => setTimeout(r, 100));
  assert.equal(rc1.ports[0]._isOpen, false);
  assert.equal(events.filter((e) => e.type === 'opened').length, 0);
});

test('a port that is not there: DeviceNotFoundError (1)', async () => {
  const { bridge, event } = fresh({ granted: false });
  bridge.open(7, RC1, ESP_OPTIONS);
  const o = await event((e) => e.type === 'opened');
  assert.equal(o.ok, false);
  assert.equal(o.err, 1);
  assert.match(o.msg, /not available/);
});

test('unplugged while open: ResourceError (9) once; replugged: opens again', async () => {
  const { rc1, bridge, until, event, events, closeAll } = fresh({ granted: true });
  await bridge.ready();
  bridge.open(1, RC1, ESP_OPTIONS);
  await event((e) => e.type === 'opened');
  rc1.unplug();
  const err = await event((e) => e.type === 'error');
  assert.equal(err.err, 9);
  await new Promise((r) => setTimeout(r, 50));
  assert.equal(events.filter((e) => e.type === 'error').length, 1);
  await until(() => bridge.ports().length === 0);
  bridge.close(1); // SavvyCAN closes the port after ResourceError: must not throw

  rc1.replug();
  await until(() => bridge.ports().length === 1);
  bridge.open(2, RC1, ESP_OPTIONS);
  const o = await event((e) => e.type === 'opened' && e.h === 2);
  assert.equal(o.ok, true, o.msg);
  await closeAll();
});

test('requestPort: filtered to rc1 by default, every port on request', async () => {
  const { rc1, bridge, event } = fresh({ granted: false });
  await bridge.ready();
  assert.equal(bridge.ports().length, 0);
  await bridge.requestPort(false);
  const asked = rc1.events.filter((e) => e.type === 'requestPort');
  assert.deepEqual(asked[0].filters, [{ usbVendorId: 0x1209, usbProductId: 0x0001 }]);
  const chosen = await event((e) => e.type === 'chosen');
  assert.deepEqual([chosen.name, chosen.error], [RC1, '']);
  assert.deepEqual(bridge.ports().map((p) => p.name), [RC1]);
  await bridge.requestPort(true);
  assert.equal(rc1.events.filter((e) => e.type === 'requestPort')[1].filters, null);
});

test('requestPort cancelled: chosen with no name and no error', async () => {
  const { bridge, event } = fresh({ granted: false, chooser: 'cancel' });
  await bridge.requestPort(false);
  const chosen = await event((e) => e.type === 'chosen');
  assert.deepEqual([chosen.name, chosen.error], ['', '']);
});

test('LAWICEL session: slcan answers CR, frames as lines', async () => {
  const { rc1, bridge, until, event, received, closeAll } = fresh({ granted: true });
  await bridge.ready();
  bridge.open(3, RC1, { ...ESP_OPTIONS, flowControl: 'hardware' });
  await event((e) => e.type === 'opened');
  bridge.write(3, new TextEncoder().encode('C\rS6\rO\r'));
  await until(() => received(3).length >= 3);
  assert.deepEqual(Array.from(received(3)), [13, 13, 13]);
  rc1.inject({ id: 0x7df, data: [2, 1, 5, 0, 0, 0, 0, 0] });
  await until(() => received(3).length >= 3 + 22);
  assert.match(new TextDecoder().decode(received(3).slice(3)), /^t7DF80201050000000000/);
  await closeAll();
});

test('a 1 s stall of the page at full load: the 16 MiB buffer holds it, rc1 loses nothing', async () => {
  const { rc1, bridge, until, event, received, closeAll } = fresh({ granted: true });
  await bridge.ready();
  bridge.open(1, RC1, ESP_OPTIONS);
  await event((e) => e.type === 'opened');
  const word = (0x80000000 | 0x40000000 | 1000000) >>> 0;
  bridge.write(1, Uint8Array.from([0xe7, 0xe7, 0xf1, 5, word & 0xff, (word >>> 8) & 0xff, (word >>> 16) & 0xff, word >>> 24, 0, 0, 0, 0]));
  await until(() => rc1.stats().channel.open);
  rc1.setTraffic('fd64-full', { seconds: 1.5 });
  await new Promise((r) => setTimeout(r, 200));
  const end = Date.now() + 1000;
  while (Date.now() < end) { /* the page's one thread busy (a long Qt call): nothing reads */ }
  await new Promise((r) => setTimeout(r, 1000));
  const st = rc1.stats();
  assert.ok(st.encoded > 5000, `FD-64 frames encoded: ${st.encoded}`);
  assert.equal(st.lost, 0, `rc1 lost ${st.lost} frames`);
  await until(() => received(1).length >= st.encoded * 77);
  await closeAll();
});
