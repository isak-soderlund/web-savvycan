// The Web Serial bridge (webserial.js) in Node against the navigator.serial mock (mock/mock-serial.js), standing in
// for the C++ side: what SavvyCAN's GVRET does through the QSerialPort stand-in, as calls and callbacks.
//   node --test wasm/webserial/tests/bridge.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

const here = dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const BRIDGE = readFileSync(join(here, '../webserial.js'), 'utf8');
const { installMockSerial } = require('../mock/mock-serial.js');

// A fresh bridge per test: webserial.js runs as Emscripten's --pre-js would, with `Module` in scope.
function loadBridge() {
  const events = [];
  const waiters = [];
  const push = (e) => {
    events.push(e);
    for (const w of waiters.splice(0)) w();
  };
  const Module = {
    webSerialOpened: (h, ok, err, msg) => push({ type: 'opened', h, ok, err, msg }),
    webSerialData: (h, bytes) => push({ type: 'data', h, bytes: Uint8Array.from(bytes) }),
    webSerialWritten: (h, n) => push({ type: 'written', h, n }),
    webSerialError: (h, err, msg) => push({ type: 'error', h, err, msg }),
    webSerialReadError: (h, msg) => push({ type: 'readError', h, msg }),
    webSerialPortsChanged: () => push({ type: 'ports' }),
    webSerialPortChosen: (name, error) => push({ type: 'chosen', name, error }),
  };
  const bridge = vm.runInThisContext(`(function (Module) { ${BRIDGE}\n; return webSerialBridge; })`)(Module);
  const until = async (pred, ms = 2000) => {
    const end = Date.now() + ms;
    for (;;) {
      const hit = events.find(pred);
      if (hit) return hit;
      if (Date.now() > end) throw new Error('timeout; events: ' + JSON.stringify(events.map((e) => e.type)));
      await new Promise((r) => { waiters.push(r); setTimeout(r, 20); });
    }
  };
  const received = (h) => {
    const all = events.filter((e) => e.type === 'data' && e.h === h).flatMap((e) => Array.from(e.bytes));
    return Uint8Array.from(all);
  };
  return { bridge, events, until, received };
}

const GVRET_HELLO = [0xe7, 0xe7, 0xf1, 0x0c, 0xf1, 0x06, 0xf1, 0x07, 0xf1, 22, 0xf1, 0x01, 0xf1, 0x09];
const ESP_OPTIONS = { baudRate: 1000000, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none' };

function fresh(opts) {
  const mock = installMockSerial(opts);
  const b = loadBridge();
  return { mock, ...b };
}

test('a granted port is listed under a stable name, without a prompt', async () => {
  const { bridge, until } = fresh({ granted: true, otherPort: true });
  await bridge.ready();
  await until((e) => e.type === 'ports');
  assert.deepEqual(bridge.ports().map((p) => [p.name, p.description]), [
    ['WebSerial 1209:0001', 'resolve-can (rc1)'],
    ['WebSerial 0403:6001', 'USB serial port'],
  ]);
});

test('GVRET session: open, DTR/RTS low, handshake answered, bus set up, frames both ways, close', async () => {
  const { bridge, mock, until, received } = fresh({ granted: true });
  await bridge.ready();
  bridge.open(1, 'WebSerial 1209:0001', ESP_OPTIONS);
  bridge.setSignals(1, false, false, false); // before the open finished: applied right after it
  const opened = await until((e) => e.type === 'opened');
  assert.equal(opened.ok, true);
  assert.equal(mock.port.options.baudRate, 1000000);
  assert.equal(mock.port.options.bufferSize, 1 << 20);
  assert.deepEqual(mock.port.signals[0], { dataTerminalReady: false, requestToSend: false, break: false });

  bridge.write(1, Uint8Array.from(GVRET_HELLO));
  await until((e) => e.type === 'written' && e.n === GVRET_HELLO.length);
  // Replies: 0C (1 bus), 06 (bus params), 07 (device info), 16 (FD settings), 01 (time), 09 (validation).
  const want = [[0xf1, 0x0c, 1], [0xf1, 0x06], [0xf1, 0x07, 100, 0], [0xf1, 22, 1], [0xf1, 0x01], [0xf1, 0x09, 0xde, 0xad]];
  await until(() => received(1).length >= 3 + 12 + 8 + 8 + 6 + 4);
  const rx = Array.from(received(1));
  let at = 0;
  for (const w of want) {
    const i = rx.findIndex((_, k) => k >= at && w.every((b, j) => rx[k + j] === b));
    assert.ok(i >= 0, `reply ${w.map((b) => b.toString(16))} in ${rx.map((b) => b.toString(16))}`);
    at = i + 1;
  }
  assert.ok(mock.device.gvret);

  // SETUP_CANBUS: 500 kbit/s, enabled.
  const word = (0x80000000 | 0x40000000 | 500000) >>> 0;
  bridge.write(1, Uint8Array.from([0xf1, 5, word & 0xff, (word >>> 8) & 0xff, (word >>> 16) & 0xff, word >>> 24, 0, 0, 0, 0]));
  await until(() => mock.device.log.includes('setup bus0 enabled=1 listen_only=0 bitrate=500000'));

  const before = received(1).length;
  mock.device.busFrame({ id: 0x123, data: [1, 2, 3, 4, 5, 6, 7, 8] });
  mock.device.busFrame({ id: 0x456, fd: true, brs: true, data: Array.from({ length: 64 }, (_, i) => i) });
  await until(() => received(1).length >= before + (2 + 9 + 8 + 1) + (2 + 10 + 64 + 1));
  const frames = received(1).slice(before);
  assert.deepEqual(Array.from(frames.slice(0, 2)), [0xf1, 0]);
  assert.deepEqual(Array.from(frames.slice(6, 10)), [0x23, 0x01, 0, 0]);

  bridge.write(1, Uint8Array.from([0xf1, 0, 0x21, 0x03, 0, 0, 0, 4, 0xde, 0xad, 0xbe, 0xef, 0]));
  await until(() => mock.device.log.includes('tx 321 deadbeef'));

  bridge.close(1);
  await until(() => mock.device.log.includes('closed'));
  assert.equal(mock.port.readable, null);
  assert.equal(bridge.sessions(), 0);
});

test('close then reopen at once: the reopen waits for the close (SavvyCAN retries this way)', async () => {
  const { bridge, mock, until } = fresh({ granted: true });
  await bridge.ready();
  bridge.open(1, 'WebSerial 1209:0001', ESP_OPTIONS);
  await until((e) => e.type === 'opened' && e.h === 1);
  bridge.close(1);
  bridge.open(2, 'WebSerial 1209:0001', { ...ESP_OPTIONS, flowControl: 'hardware' });
  const o2 = await until((e) => e.type === 'opened' && e.h === 2);
  assert.equal(o2.ok, true, o2.msg);
  assert.equal(mock.port.openCount, 2);
  assert.equal(mock.port.options.flowControl, 'hardware');
  bridge.write(2, Uint8Array.from([0xf1, 0x09]));
  await until((e) => e.type === 'written' && e.h === 2);
  bridge.close(2);
});

test('close while the open is still running: the port ends up closed, no answer', async () => {
  const { bridge, mock, events } = fresh({ granted: true });
  await bridge.ready();
  bridge.open(1, 'WebSerial 1209:0001', ESP_OPTIONS);
  bridge.close(1);
  await new Promise((r) => setTimeout(r, 100));
  assert.equal(mock.port.readable, null);
  assert.equal(events.filter((e) => e.type === 'opened').length, 0);
});

test('a port that is not there: DeviceNotFoundError (1)', async () => {
  const { bridge, until } = fresh({ granted: false });
  bridge.open(7, 'WebSerial 1209:0001', ESP_OPTIONS);
  const o = await until((e) => e.type === 'opened');
  assert.equal(o.ok, false);
  assert.equal(o.err, 1);
  assert.match(o.msg, /not available/);
});

test('unplugged while open: ResourceError (9) once; replugged: opens again', async () => {
  const { bridge, mock, until, events } = fresh({ granted: true });
  await bridge.ready();
  bridge.open(1, 'WebSerial 1209:0001', ESP_OPTIONS);
  await until((e) => e.type === 'opened');
  mock.port.unplug();
  const err = await until((e) => e.type === 'error');
  assert.equal(err.err, 9);
  await new Promise((r) => setTimeout(r, 50));
  assert.equal(events.filter((e) => e.type === 'error').length, 1);
  await until(() => bridge.ports().length === 0);
  bridge.close(1); // SavvyCAN closes the port after ResourceError: must not throw

  mock.port.replug();
  await until(() => bridge.ports().length === 1);
  bridge.open(2, 'WebSerial 1209:0001', ESP_OPTIONS);
  const o = await until((e) => e.type === 'opened' && e.h === 2);
  assert.equal(o.ok, true, o.msg);
  bridge.close(2);
});

test('requestPort: rc1 filter by default, any port on request, cancel reported as such', async () => {
  const { bridge, mock, until, events } = fresh({ granted: false, otherPort: true });
  await bridge.ready();
  assert.equal(bridge.ports().length, 0);
  await bridge.requestPort(false);
  assert.deepEqual(mock.serial.requests[0], { filters: [{ usbVendorId: 0x1209, usbProductId: 0x0001 }] });
  const chosen = await until((e) => e.type === 'chosen');
  assert.equal(chosen.name, 'WebSerial 1209:0001');
  assert.equal(chosen.error, '');
  assert.deepEqual(bridge.ports().map((p) => p.name), ['WebSerial 1209:0001']);

  mock.serial.cancelNext = true;
  await bridge.requestPort(true);
  assert.deepEqual(mock.serial.requests[1], {});
  const cancelled = events.filter((e) => e.type === 'chosen')[1];
  assert.deepEqual([cancelled.name, cancelled.error], ['', '']);
});

test('LAWICEL session: slcan answers CR', async () => {
  const { bridge, mock, until, received } = fresh({ granted: true });
  await bridge.ready();
  bridge.open(3, 'WebSerial 1209:0001', { ...ESP_OPTIONS, flowControl: 'hardware' });
  await until((e) => e.type === 'opened');
  bridge.write(3, new TextEncoder().encode('C\rS6\rO\r'));
  await until(() => received(3).length >= 3);
  assert.deepEqual(Array.from(received(3)), [13, 13, 13]);
  mock.device.busFrame({ id: 0x7df, data: [2, 1, 5, 0, 0, 0, 0, 0] });
  await until(() => received(3).length >= 3 + 22);
  assert.equal(new TextDecoder().decode(received(3).slice(3)), 't7DF80201050000000000\r');
  bridge.close(3);
});
