// The leaner log loader (RESOLVE.md, "Done: a faster, leaner log loader"): File > Load Log File of long GVRET CSVs
// (600 000 and 3 000 000 frames: Leaf-style 11-bit classic IDs, J1939-style 29-bit IDs, CAN FD 64 bytes with BRS on
// 11- and 29-bit IDs, a 12-byte FD ID, Tx rows), timed, with the page's memory: the heap in use before, after and at
// its highest while loading (polled: the loader hands the page a turn every 100 ms), malloc's peak footprint and the
// wasm memory (results/loader-600k.json, loader-3M.json). Every frame must arrive, the newest exactly as written.
// Then the other formats (candump, CRTD: SavvyCAN's autodetect through the in-memory file system) and a broken file
// (the salvage question: No keeps the trace as it was, Yes loads the rows that could be read).
import { join } from 'node:path';
import { mkdirSync, openSync, writeSync, closeSync, statSync, rmSync, writeFileSync } from 'node:fs';
import { test, expect, RESULTS, saveResult } from '../lib/harness.mjs';

const hex = (n, w) => n.toString(16).toUpperCase().padStart(w, '0');
const MB = (b) => Math.round(b / 1e5) / 10;
const memory = (page) => page.evaluate(() => window.webSavvyCAN.memory());
const stats = (page) => page.evaluate(() => window.webSavvyCAN.stats());

const LEAF = [0x11a, 0x1d4, 0x1da, 0x1db, 0x1dc, 0x1f2, 0x284, 0x390, 0x393, 0x45e, 0x481, 0x50a, 0x50b, 0x50c, 0x539,
  0x54a, 0x54b, 0x54c, 0x54f, 0x55a, 0x55b, 0x56e, 0x59e, 0x5a9];
// 32 kinds, round robin: 24 Leaf IDs (8 bytes), 4 J1939-style 29-bit (8 bytes, one of them Tx), 0x7E8 and 0x18DAF110
// (CAN FD 64 bytes, BRS), 0x6F0 (CAN FD 12 bytes, no BRS), 0x7DF (classic, 3 bytes).
const KINDS = [...LEAF.map((id) => ({ id, ext: false, len: 8 })),
  ...[0x0cf00400, 0x0cf00300, 0x18feee00, 0x18fef100].map((id, k) => ({ id, ext: true, len: 8, tx: k === 3 })),
  { id: 0x7e8, ext: false, len: 64, fd: true, brs: true }, { id: 0x18daf110, ext: true, len: 64, fd: true, brs: true },
  { id: 0x6f0, ext: false, len: 12, fd: true }, { id: 0x7df, ext: false, len: 3 }];
const T0 = 1_000_000, STEP = 1_200; // µs: 3 000 000 frames = one hour

/** Frame i of the generated logs: its kind, time and bytes (byte j = (i * 7 + j) & 0xFF, bytes 1..3 the frame number). */
function frame(i) {
  const k = KINDS[i % KINDS.length];
  const data = Array.from({ length: k.len }, (_, j) => (i * 7 + j) & 0xff);
  if (k.len >= 4) { data[1] = i & 0xff; data[2] = (i >> 8) & 0xff; data[3] = (i >> 16) & 0xff; }
  return { ...k, time: T0 + i * STEP, data };
}

/** SavvyCAN's native GVRET CSV (as the app logs it) of n frames, written in pieces; returns its path and size. */
function writeLog(name, n) {
  mkdirSync(RESULTS, { recursive: true });
  const file = join(RESULTS, name);
  const fd = openSync(file, 'w');
  writeSync(fd, 'Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8\n');
  let out = [];
  for (let i = 0; i < n; i++) {
    const f = frame(i);
    const cols = f.data.map((b) => hex(b, 2));
    while (cols.length < 8) cols.push('00');
    out.push(`${f.time},${hex(f.id, 8)},${f.ext},${f.tx ? 'Tx' : 'Rx'},0,${f.len},${cols.join(',')},` +
      (f.fd ? (f.brs ? 'FD+BRS,' : 'FD,') : ''));
    if (out.length === 50_000) { writeSync(fd, out.join('\n') + '\n'); out = []; }
  }
  if (out.length) writeSync(fd, out.join('\n') + '\n');
  closeSync(fd);
  return { file, bytes: statSync(file).size };
}

/**
 * File > Load Log File of `file` (fetched from the test server, as the user's pick would hand it over), polling the
 * page's memory every 250 ms until `n` frames are in the trace. -> { seconds, peakUsed, samples }
 */
async function timedLoad(app, page, file, n, timeoutMs) {
  const name = file.split('/').pop();
  await page.evaluate(([src, nm]) => {
    const real = window.showOpenFilePicker;
    window.showOpenFilePicker = async () => {
      window.showOpenFilePicker = real;
      const f = new File([await (await fetch(src)).blob()], nm);
      window.__loadStart = performance.now();
      return [{ kind: 'file', name: nm, getFile: async () => f }];
    };
  }, ['/__harness/results/' + name, name]);
  await app.menu('File', 'Load Log File');
  const end = Date.now() + timeoutMs;
  let peakUsed = 0, polls = 0;
  for (;;) {
    const m = await memory(page);
    peakUsed = Math.max(peakUsed, m.heapUsed);
    polls++;
    if ((await stats(page)).framesReceived >= n) break;
    if (Date.now() > end) throw new Error(`not loaded in ${timeoutMs / 1000} s`);
    await page.waitForTimeout(250);
  }
  const seconds = await page.evaluate(() => (performance.now() - window.__loadStart) / 1000);
  return { seconds: Math.round(seconds * 100) / 100, peakUsed, polls };
}

/** The newest frames in the trace equal the file's last rows (IDs, flags, bytes, times). */
async function checkTail(page, n) {
  const tail = await page.evaluate(() => window.webSavvyCAN.traceTail(40));
  expect(tail.length).toBe(40);
  tail.forEach((row, j) => {
    const f = frame(n - 40 + j);
    expect({ id: row.id, ext: row.ext, fd: row.fd, brs: row.brs, time: row.time, data: row.data },
      `frame ${n - 40 + j}`).toEqual({ id: f.id, ext: f.ext, fd: !!f.fd, brs: !!f.brs, time: f.time, data: f.data });
  });
}

// [label, frames, load time limit (s), tag]. 'limit': a file near the browser's load limit (kMaxBrowserFileBytes,
// mainwindow.cpp; ~69 bytes a row here: 14 M frames ~ 0.97 GB), opt-in (WSC_RUN=large, run.sh --large).
const SIZES = [['600k', 600_000, 5, ''], ['3M', 3_000_000, 15, ''],
  ['limit', Number(process.env.WSC_LOADER_LIMIT_FRAMES || 14_000_000), 90, ' @large']];
for (const [label, N, limitS, tag] of SIZES) {
  test(`a ${label}-frame GVRET CSV loads fast and lean: every frame, the newest exact, time and memory recorded${tag}`,
    async ({ app, page }) => {
      test.setTimeout(900_000);
      const { file, bytes } = writeLog(`loader-${label}.csv`, N);
      try {
        await app.open();
        const before = await memory(page);
        const load = await timedLoad(app, page, file, N, 600_000);
        const st = await stats(page);
        expect(st.framesReceived, 'every frame taken in').toBe(N);
        expect(st.framesInTrace, 'every frame in the trace').toBe(N);
        expect(st.framesEvicted).toBe(0);
        await checkTail(page, N);
        await page.waitForTimeout(500);
        const after = await memory(page);
        const perFrame = (after.heapUsed - before.heapUsed) / N;
        const result = { frames: N, fileMB: MB(bytes), loadSeconds: load.seconds, polls: load.polls,
          heapUsedBeforeMB: MB(before.heapUsed), heapUsedAfterMB: MB(after.heapUsed), bytesPerFrame: Math.round(perFrame),
          peakHeapUsedMB: MB(load.peakUsed), maxFootprintMB: MB(after.maxFootprint), heapSizeMB: MB(after.heapSize),
          storeMB: MB(after.storeBytes), storeBytesPerFrame: Math.round(after.storeBytes / N) };

        // The RE tools on the whole log: RE Tools > Temporal Graph, a graph of one ID, Frame Data Analysis of the
        // 64-byte FD ID.
        let t = Date.now();
        await app.menu('RE Tools', 'Temporal Graph');
        await page.waitForFunction(() => { const s = window.webSavvyCAN.temporalState(); return s.open && !s.building; },
          null, { timeout: 300_000, polling: 250 });
        result.temporalSeconds = (Date.now() - t) / 1000;
        const tg = await page.evaluate(() => window.webSavvyCAN.temporalState());
        expect(tg.frames, 'Temporal Graph counts every frame').toBe(N);
        expect(tg.ids.length).toBe(KINDS.length);
        t = Date.now();
        expect(await page.evaluate(() => window.webSavvyCAN.addGraph(0x1da, 8, 16, true))).toBe(true);
        const perId = Math.floor(N / KINDS.length) + (N % KINDS.length > 2 ? 1 : 0); // 0x1DA is kind 2
        await page.waitForFunction((n) => { const g = window.webSavvyCAN.graphState(); return g.open && g.series?.[0]?.samples >= n; },
          perId, { timeout: 300_000, polling: 250 });
        result.graphSeconds = (Date.now() - t) / 1000;
        const g = await page.evaluate(() => window.webSavvyCAN.graphState());
        expect(g.series[0].samples, 'the graph takes in every frame of 0x1DA').toBe(perId);
        t = Date.now();
        await app.menu('RE Tools', 'Frame Data Analysis');
        await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.frameInfo())).open,
          { timeout: 300_000 }).toBe(true);
        expect(await page.evaluate(() => window.webSavvyCAN.frameInfoSelect(0x18daf110))).toBe(true);
        const fi = await page.evaluate(() => window.webSavvyCAN.frameInfo());
        result.frameInfoSeconds = (Date.now() - t) / 1000;
        const fdId = KINDS.findIndex((k) => k.id === 0x18daf110);
        expect(fi.frames, 'Frame Data Analysis of 0x18DAF110: every frame').toBe(Math.floor(N / KINDS.length) + (N % KINDS.length > fdId ? 1 : 0));
        expect([fi.minLen, fi.maxLen]).toEqual([64, 64]);
        const end = await memory(page);
        Object.assign(result, { withToolsHeapUsedMB: MB(end.heapUsed), withToolsHeapSizeMB: MB(end.heapSize) });
        saveResult(`loader-${label}`, result);
        test.info().annotations.push({ type: 'loader', description: JSON.stringify(result) });

        // Targets (RESOLVE.md): 3 M frames in under ~15 s headless Chromium, at most a few hundred bytes of heap a
        // frame in steady state (measured ~60; asserted < 150), a bounded peak while loading (the file's bytes, the
        // frames staged and the trace together: under the file + 100 bytes a frame in use, malloc's footprint under
        // twice the file + 60 bytes a frame + 50 MB).
        expect(load.seconds, `loaded in ${load.seconds} s`).toBeLessThan(limitS);
        expect(perFrame, `${Math.round(perFrame)} bytes of heap a frame`).toBeLessThan(150);
        expect(load.peakUsed - before.heapUsed, 'peak in use while loading').toBeLessThan(bytes + N * 100);
        expect(after.maxFootprint, "malloc's peak footprint").toBeLessThan(2 * bytes + N * 60 + 50e6);
        expect(end.heapSize, 'the wasm memory, also with the tools open (the page has 4 GB)').toBeLessThan(3.2e9);
      } finally {
        rmSync(file, { force: true });
      }
    });
}

test('other formats still load by autodetect: candump (classic and FD) and CRTD', async ({ app, page }) => {
  mkdirSync(RESULTS, { recursive: true });
  // candump -L: "(seconds.µs) iface id#data", CAN FD "id##<flags><data>"
  const dump = join(RESULTS, 'loader-candump.log');
  writeFileSync(dump, ['(1436509052.249713) vcan0 044#2A366C2BBA',
    '(1436509052.449847) vcan0 0F6#7ADFE07BD2', '(1436509052.650004) vcan0 12345678#0102030405060708',
    '(1436509052.850000) can1 7E8##1' + Array.from({ length: 16 }, (_, j) => hex(j * 3, 2)).join('')].join('\n') + '\n');
  await app.open();
  await app.loadFile(dump, 4);
  let tail = await page.evaluate(() => window.webSavvyCAN.traceTail(4));
  expect(tail.map((r) => [r.id, r.ext, r.fd, r.data])).toEqual([
    [0x044, false, false, [0x2a, 0x36, 0x6c, 0x2b, 0xba]], [0x0f6, false, false, [0x7a, 0xdf, 0xe0, 0x7b, 0xd2]],
    [0x12345678, true, false, [1, 2, 3, 4, 5, 6, 7, 8]], [0x7e8, false, true, Array.from({ length: 16 }, (_, j) => j * 3)]]);
  expect(Math.abs(tail[0].time - 1436509052249713), 'candump time in µs').toBeLessThanOrEqual(1);
  // CRTD: a header line, then "time R11|R29|T11 id bytes..."
  const crtd = join(RESULTS, 'loader-crtd.crtd');
  writeFileSync(crtd, ['1320745424.000 CXX CRTD log', '1320745424.000100 R11 100 01 02 03',
    '1320745424.002100 R29 18FEF100 0A 0B 0C 0D 0E 0F 10 11', '1320745424.004100 T11 7DF 02 01 0D'].join('\n') + '\n');
  await app.loadFile(crtd, 3);
  await page.waitForFunction(() => window.webSavvyCAN.traceTail(1)[0]?.id === 0x7df, null, { timeout: 15_000 });
  expect((await stats(page)).framesReceived, 'the trace holds the CRTD file only').toBe(3);
  tail = await page.evaluate(() => window.webSavvyCAN.traceTail(3));
  expect(tail.map((r) => [r.id, r.ext, r.data])).toEqual([[0x100, false, [1, 2, 3]],
    [0x18fef100, true, [0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11]], [0x7df, false, [2, 1, 0x0d]]]);
});

test('a broken GVRET CSV: the salvage question; No keeps the trace, Yes loads the rows that could be read',
  async ({ app, page }) => {
    mkdirSync(RESULTS, { recursive: true });
    const good = writeLog('loader-good.csv', 1000).file;
    // 500 rows, every 50th cut short (fewer than 5 fields): 490 frames can be read
    const rows = ['Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8'];
    for (let i = 0; i < 500; i++)
      rows.push(i % 50 === 25 ? `${T0 + i},00000123,false` : `${T0 + i},00000321,false,Rx,0,2,${hex(i & 0xff, 2)},77,`);
    const broken = join(RESULTS, 'loader-broken.csv');
    writeFileSync(broken, rows.join('\n') + '\n');
    try {
      await app.open();
      await app.loadFile(good, 1000);
      const pick = async (answer) => {
        const name = broken.split('/').pop();
        await page.evaluate(([src, nm]) => {
          const real = window.showOpenFilePicker;
          window.showOpenFilePicker = async () => {
            window.showOpenFilePicker = real;
            const f = new File([await (await fetch(src)).blob()], nm);
            return [{ kind: 'file', name: nm, getFile: async () => f }];
          };
        }, ['/__harness/results/' + name, name]);
        await app.menu('File', 'Load Log File');
        const button = app.role('button', answer); // the salvage question (QMessageBox), it must come
        await button.waitFor({ state: 'attached', timeout: 15_000 });
        await button.click({ force: true });
        await page.waitForTimeout(1000);
      };
      await pick('No');
      expect((await stats(page)).framesReceived, 'No: the trace as it was').toBe(1000);
      await pick('Yes');
      await page.waitForFunction(() => window.webSavvyCAN.stats().framesReceived === 490, null, { timeout: 10_000 });
      const tail = await page.evaluate(() => window.webSavvyCAN.traceTail(1));
      expect(tail[0]).toMatchObject({ id: 0x321, time: T0 + 499, data: [499 & 0xff, 0x77] });
    } finally {
      rmSync(good, { force: true });
    }
  });

test('the load limit of other formats: a candump log over 200 MB is refused with a message, the trace stays',
  async ({ app, page }) => {
    test.setTimeout(180_000);
    mkdirSync(RESULTS, { recursive: true });
    // 201 MB of candump rows (SavvyCAN's candump loader would need ~1 GB of the page for it)
    const big = join(RESULTS, 'loader-toolarge.log');
    const line = Buffer.from('(1436509052.249713) vcan0 044#2A366C2BBA00112233\n');
    const block = Buffer.concat(Array(20_000).fill(line));
    const fd = openSync(big, 'w');
    for (let n = 0; n * block.length <= 201e6; n++) writeSync(fd, block);
    closeSync(fd);
    try {
      await app.open();
      await app.loadFile(writeLog('loader-small.csv', 100).file, 100);
      const name = big.split('/').pop();
      await page.evaluate(([src, nm]) => {
        const real = window.showOpenFilePicker;
        window.showOpenFilePicker = async () => {
          window.showOpenFilePicker = real;
          const f = new File([await (await fetch(src)).blob()], nm);
          return [{ kind: 'file', name: nm, getFile: async () => f }];
        };
      }, ['/__harness/results/' + name, name]);
      await app.menu('File', 'Load Log File');
      const ok = app.role('button', 'OK'); // QMessageBox "File too large"
      await ok.waitFor({ state: 'attached', timeout: 60_000 });
      await ok.click({ force: true });
      await page.waitForTimeout(500);
      expect((await stats(page)).framesReceived, 'the trace as it was').toBe(100);
    } finally {
      rmSync(big, { force: true });
      rmSync(join(RESULTS, 'loader-small.csv'), { force: true });
    }
  });
