// Graphs (stage 2): a graph of a live bus keeps growing and follows the newest data (Follow End of Graph, on by
// default since 2026-10-29), as in SavvyCAN. The fake rc1 plays the Leaf bus; 0x1DA comes every 10 ms with byte 0
// counting up. Picture: results/graph-live.png.
// Bounded storage (RESOLVE.md stage 2): a graph keeps at most `cap` points (the newest samples exactly, older ones as
// a min/max envelope) and still spans the whole session, live and from a big log file, and the wasm heap levels off
// (results/graph-bounded*.png, graph-bounded.json, graph-bigfile.json).
// CAN FD: signals past byte 8, through the test hook and through the Add New Graph dialog with its 64-byte bit grid
// (results/graph-fd-dialog-*.png).
import { join } from 'node:path';
import { mkdirSync, openSync, writeSync, closeSync } from 'node:fs';
import { test, expect, fake, RESULTS, saveResult } from '../lib/harness.mjs';

const state = (page) => page.evaluate(() => window.webSavvyCAN.graphState());
const MB = (b) => (b / 1048576).toFixed(1);
const hex = (b) => b.toString(16).toUpperCase().padStart(2, '0');
/** A menu of the graphing window (its menu bar comes after the main window's in the accessibility tree, which has a
 * hidden View menu of its own). */
async function graphMenu(page, top, item) {
  await page.getByRole('menuitem', { name: new RegExp(`^${top}$`) }).last().click({ force: true });
  await page.getByRole('menuitem', { name: new RegExp(`^${item}$`) }).last().click({ force: true });
}

/**
 * A GVRET CSV as SavvyCAN saves it (FrameFileIO::nativeCSVRow), written in pieces: row(i) -> [time µs, id, bytes, fd].
 * Times from 1 s: SavvyCAN's loader takes a time of up to 3 characters as "none" and stamps the frame itself.
 */
function writeCsv(name, count, row) {
  mkdirSync(RESULTS, { recursive: true });
  const file = join(RESULTS, name);
  const fd = openSync(file, 'w');
  let out = 'Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8\n';
  for (let i = 0; i < count; i++) {
    const [t, id, data, isFd] = row(i);
    const cols = isFd ? data : [...data, ...Array(8 - data.length).fill(0)];
    out += `${t},${id.toString(16).toUpperCase().padStart(8, '0')},false,Rx,0,${data.length},${cols.map(hex).join(',')},${isFd ? 'FD+BRS,' : ''}\n`;
    if (out.length > 1 << 20) {
      writeSync(fd, out);
      out = '';
    }
  }
  writeSync(fd, out);
  closeSync(fd);
  return file;
}

test('a live graph grows with the bus and follows its end', async ({ app, page }) => {
  await app.open();
  await app.connect({ protocol: 'gvret', fd: false });
  await fake.setTraffic(page, 'leaf', { seconds: 8 });
  await page.waitForTimeout(1000);
  expect(await page.evaluate(() => window.webSavvyCAN.addGraph(0x1da, 0, 8, true)), 'graph created').toBe(true);
  const first = await state(page);
  await page.waitForTimeout(3000);
  const later = await state(page);
  expect(later.points, `points grow while the bus runs (${first.points} -> ${later.points})`).toBeGreaterThan(first.points + 200);
  // x is time in the app's time style (microseconds by default): 3 s of bus later, the newest point is >= 2 s newer.
  expect(later.lastKey - first.lastKey, 'newer data (µs)').toBeGreaterThan(2e6);
  // Follow End of Graph: the visible range ends at the newest point and moves with it.
  expect(Math.abs(later.xUpper - later.lastKey), `x range ends at the newest point (${later.xUpper} vs ${later.lastKey})`).toBeLessThan(1000);
  expect(later.xUpper, 'the x range moved').toBeGreaterThan(first.xUpper);
  // Byte 0 counts 0..255: the value axis has grown to take it in.
  expect(later.yUpper, `value axis up to ${later.yUpper}`).toBeGreaterThanOrEqual(255);
  expect(later.yLower, `value axis from ${later.yLower}`).toBeLessThanOrEqual(0);
  await page.screenshot({ path: join(RESULTS, 'graph-live.png') });
});

test('a live graph far past its cap stays bounded, spans the whole session, and the heap levels off', async ({ app, page }) => {
  test.setTimeout(300_000);
  await app.open();
  // A small trace (100 000 frames: File > Preferences' maximum frames) so the heap is level once the trace is full and
  // only the graphs could still grow.
  await page.evaluate(() => window.webSavvyCAN.setMaxFrames(100000));
  await app.reload();
  await app.connect({ protocol: 'gvret', bitrate: 1000000, fd: false });
  // One ID at rc1's full classic rate, 8 000 frames/s, with its sequence number in bytes 0..3 (little-endian).
  await fake.setTraffic(page, 'classic-full', { ids: 1 });
  await page.waitForTimeout(1000);
  expect(await page.evaluate(() => window.webSavvyCAN.addGraph(0x100, 0, 32, true)), 'graph of the sequence number').toBe(true);
  expect(await page.evaluate(() => window.webSavvyCAN.addGraph(0x100, 0, 8, true, true)), 'graph of byte 0, same window').toBe(true);
  const start = await state(page);
  expect(start.graphs, 'two graphs in the window').toBe(2);
  const cap = start.cap;
  const [seq0, byte0] = start.series;
  const target = Math.round(1.5 * cap); // past the cap, and past the envelope's first halving (~510 000 samples)

  const log = [];
  let s = start;
  const t0 = Date.now();
  while (s.series[0].samples < target && Date.now() - t0 < 200_000) {
    await page.waitForTimeout(2000);
    s = await state(page);
    const [a, b] = s.series;
    log.push({ t: (Date.now() - t0) / 1000, samples: a.samples, stored: [a.stored, b.stored], envelope: [a.envelope, b.envelope],
      bucket: [a.bucket, b.bucket], heapUsed: s.heapUsed, heapSize: s.heapSize });
    s.series.forEach((g, i) => {
      // at most the cap, plus the samples of one update (250 ms at 8 000/s) before they are folded
      expect(g.stored, `stored points (${g.stored}) within the cap ${cap}`).toBeLessThanOrEqual(cap + 4000);
      // the start of the session stays
      expect(g.firstKey, 'first time kept').toBe(start.series[i].firstKey);
    });
  }
  await fake.setTraffic(page, 'idle');
  await page.waitForTimeout(1000);
  const end = await state(page);
  saveResult('graph-bounded', { cap, log, end });
  const [a, b] = end.series;
  expect(a.samples, `samples taken in (${a.samples}) far past the cap`).toBeGreaterThanOrEqual(target);
  expect(a.envelope, 'older samples are in the envelope').toBeGreaterThan(0);
  expect(a.bucket, 'the envelope was halved (buckets of more samples)').toBeGreaterThan(4);
  // Every frame graphed (rc1's own loss allowed, < 1 %): the values are the sequence numbers.
  const span = a.lastValue - seq0.firstValue + 1;
  expect(a.samples, `samples vs. sequence numbers ${seq0.firstValue}..${a.lastValue}`).toBeLessThanOrEqual(span);
  expect(a.samples, `samples vs. sequence numbers ${seq0.firstValue}..${a.lastValue}`).toBeGreaterThan(0.99 * span);
  // The whole session: from the first value and time to the newest; the envelope keeps the extremes.
  expect(a.firstValue, 'first value kept').toBe(seq0.firstValue);
  expect(a.minValue, 'smallest value kept').toBe(seq0.firstValue);
  expect(a.maxValue, 'largest value kept').toBe(a.lastValue);
  expect((a.lastKey - a.firstKey) / 1e6, 'time span = samples at 8 000/s').toBeCloseTo(span / 8000, 0);
  expect([b.minValue, b.maxValue], 'byte 0: 0..255 still all there').toEqual([0, 255]);
  expect(b.firstKey, 'byte 0: first time kept').toBe(byte0.firstKey);
  // The heap: level from the first fold on (the graphs' containers are allocated once, at their cap). Without the
  // bound the two graphs would add 32 bytes a sample (here ~19 MB).
  const after = log.filter((r) => r.samples >= 0.75 * cap);
  const growth = Math.max(...after.map((r) => r.heapUsed)) - after[0].heapUsed;
  test.info().annotations.push({ type: 'result', description: `cap ${cap} points (${MB(cap * 16)} MB); ${a.samples} samples; ` +
    `stored ${a.stored} + ${b.stored}; bucket ${a.bucket} samples; heap in use ${MB(after[0].heapUsed)} -> max ` +
    `${MB(after[0].heapUsed + growth)} MB over the last ${after.length} polls, memory ${MB(end.heapSize)} MB` });
  expect(after.length, 'polls after the first fold').toBeGreaterThanOrEqual(5);
  expect(growth, `heap growth after the first fold (${MB(growth)} MB)`).toBeLessThan(4 * 1048576);

  // Zoomed out (View > Reset View): the whole session on the graph, drawn as the min/max envelope.
  await graphMenu(page, 'View', 'Reset View');
  await page.waitForTimeout(1000);
  const all = await state(page);
  expect(all.xLower, 'Reset View: from the first time').toBe(seq0.firstKey);
  expect(all.xUpper, 'Reset View: to the last time').toBe(a.lastKey);
  await page.screenshot({ path: join(RESULTS, 'graph-bounded.png') });
  await app.setTheme('dark');
  await page.waitForTimeout(1000);
  await page.screenshot({ path: join(RESULTS, 'graph-bounded-dark.png') });
  // Byte 0's sawtooth (0..255 every 32 ms): the whole session is a band from 0 to 255 (envelope, then samples); the
  // oldest 0.1 s are the envelope (buckets of 8 samples); the newest 0.1 s the samples themselves.
  const view = async (from, to, name) => {
    expect(await page.evaluate(([l, u]) => window.webSavvyCAN.setGraphView(l, u, -10, 265), [from, to]), name).toBe(true);
    await page.waitForTimeout(500);
    await page.screenshot({ path: join(RESULTS, `graph-bounded-${name}.png`) });
  };
  await view(a.firstKey, a.lastKey, 'session');
  await view(a.firstKey, a.firstKey + 100000, 'oldest');
  await view(a.lastKey - 100000, a.lastKey, 'newest');
});

test('a big log file graphs within the cap and spans the whole file', async ({ app, page }) => {
  test.setTimeout(300_000);
  // 600 000 frames of 0x1DA every ms (600 s from 1 s), bytes 0..3 the frame number (little-endian).
  const frames = 600000;
  const file = writeCsv('graph-bigfile.csv', frames, (i) => [1e6 + i * 1000, 0x1da, [i & 0xff, (i >> 8) & 0xff, (i >> 16) & 0xff, 0, 0, 0, 0, 0]]);
  await app.open();
  const t0 = Date.now();
  await app.loadFile(file, frames, { timeout: 240_000 });
  const loaded = (Date.now() - t0) / 1000;
  await page.waitForTimeout(1000);
  // The window first (a graph of an ID the file doesn't have: one dummy point), then the graph of the file.
  expect(await page.evaluate(() => window.webSavvyCAN.addGraph(0x7ff, 0, 8, true)), 'graph window').toBe(true);
  await page.waitForTimeout(1000);
  const before = await state(page);
  expect(await page.evaluate(() => window.webSavvyCAN.addGraph(0x1da, 0, 32, true, true)), 'graph created').toBe(true);
  await page.waitForTimeout(1000);
  const g = await state(page);
  const s = g.series[1];
  saveResult('graph-bigfile', { frames, loaded, before: { heapUsed: before.heapUsed, heapSize: before.heapSize }, after: g });
  expect(s.samples, 'every frame of the file taken in').toBe(frames);
  expect(s.stored, `stored points (${s.stored}) within the cap ${g.cap}`).toBeLessThanOrEqual(g.cap);
  expect([s.firstKey, s.lastKey], 'from the first to the last time of the file').toEqual([1e6, 1e6 + (frames - 1) * 1000]);
  expect([s.firstValue, s.minValue, s.maxValue, s.lastValue], 'first, smallest, largest, last value').toEqual([0, 0, frames - 1, frames - 1]);
  const added = g.heapUsed - before.heapUsed;
  test.info().annotations.push({ type: 'result', description: `${frames} frames loaded in ${loaded.toFixed(0)} s; graph: ` +
    `${s.stored} points, ${s.envelope} in the envelope (buckets of ${s.bucket}); heap in use +${MB(added)} MB ` +
    `(${MB(before.heapUsed)} -> ${MB(g.heapUsed)}), memory ${MB(g.heapSize)} MB` });
  // The graph's points (its container, allocated at the cap), nothing else: without the bound (and the copies) the
  // 600 000 points took 32 bytes each plus a copy of every matching frame.
  expect(added, `heap in use added by the graph (${MB(added)} MB)`).toBeLessThan(g.cap * 16 * 1.1 + 2 * 1048576);
  await graphMenu(page, 'View', 'Reset View');
  await page.waitForTimeout(1000);
  await page.screenshot({ path: join(RESULTS, 'graph-bigfile.png') });
});

test('CAN FD: a signal past byte 8 graphs, and Add New Graph shows the 64-byte frame', async ({ app, page }) => {
  // 3 000 FD frames of 0x123 with 64 bytes every ms: byte 40 counts 0..199, byte 63 is 0x5A, the others fixed.
  const frames = 3000;
  const file = writeCsv('graph-fd.csv', frames, (i) => {
    const d = Array.from({ length: 64 }, (_, j) => (j * 3 + 1) & 0xff);
    d[40] = i % 200;
    d[63] = 0x5a;
    return [1e6 + i * 1000, 0x123, d, true];
  });
  await app.open();
  await app.loadFile(file, frames);
  expect((await page.evaluate(() => window.webSavvyCAN.traceTail(1)))[0].data.length, 'FD frames of 64 bytes in the trace').toBe(64);

  // Byte 40, Intel: start bit 320, 8 bits.
  expect(await page.evaluate(() => window.webSavvyCAN.addGraph(0x123, 320, 8, true)), 'graph created').toBe(true);
  await expect.poll(async () => (await state(page)).points, { message: 'every frame plotted' }).toBe(frames);
  const [intel] = (await state(page)).series;
  expect([intel.minValue, intel.maxValue, intel.firstValue, intel.lastValue], 'byte 40: 0..199').toEqual([0, 199, 0, (frames - 1) % 200]);

  // Graph > Add New Graph (exec()): the ID's frames are 64 bytes, so its bit grid shows 64 bytes (512 bits).
  await graphMenu(page, 'Graph', 'Add New Graph');
  await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.graphDialog())).open, { message: 'dialog open' }).toBe(true);
  expect((await page.evaluate(() => window.webSavvyCAN.graphDialog())).gridBytes, 'no ID yet: the classic 8-byte grid').toBe(8);
  expect(await page.evaluate(() => window.webSavvyCAN.setGraphDialogField('id', 0x123)), 'ID typed').toBe(true);
  // Byte 40 again, Motorola (the dialog's default byte order): start bit 327 (byte 40's top bit), 8 bits.
  await page.evaluate(() => window.webSavvyCAN.setGraphDialogField('startBit', 327));
  await page.evaluate(() => window.webSavvyCAN.setGraphDialogField('bits', 8));
  await page.waitForTimeout(500);
  const dlg = await page.evaluate(() => window.webSavvyCAN.graphDialog());
  expect(dlg.gridBytes, 'the bit grid covers the 64-byte frame').toBe(64);
  expect([dlg.txtStartBit, dlg.txtDataLen], 'start bit and length as typed (not clamped to 8 bytes)').toEqual(['327', '8']);
  await page.screenshot({ path: join(RESULTS, 'graph-fd-dialog-light.png') });
  await app.setTheme('dark');
  await page.waitForTimeout(800);
  await page.screenshot({ path: join(RESULTS, 'graph-fd-dialog-dark.png') });
  await app.click('button', 'Add this graph');
  await expect.poll(async () => (await state(page)).graphs, { message: 'the dialog added a graph' }).toBe(2);
  const moto = (await state(page)).series[1];
  expect([moto.startBit, moto.numBits], 'the dialog\'s graph').toEqual([327, 8]);
  expect([moto.stored, moto.minValue, moto.maxValue, moto.lastValue], 'byte 40 through the dialog: 0..199').toEqual([frames, 0, 199, (frames - 1) % 200]);
  await page.waitForTimeout(500);
  await page.screenshot({ path: join(RESULTS, 'graph-fd-dark.png') });
});
