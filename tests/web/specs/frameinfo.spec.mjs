// Frame Data Analysis (RE Tools > Frame Data Analysis, "the heatmap tool"; stage 4, switched on 2026-10-29) with CAN FD:
// the bit-change heatmap counts every bit of a frame's length (512 bits of a 64-byte FD frame, 64 of a classic one), the
// bit histogram and the details every byte, the byte graphs one per byte. Checked against the frames themselves: a log
// file with classic and FD frames whose bits change in a known pattern (byte 40 bit 3 toggling every frame, byte 63
// constant, ...), and a live fd-mix bus (the ID list fills as frames come; an ID's analysis = the trace's frames of it).
// Pictures: results/frameinfo-light.png, results/frameinfo-dark.png, results/frameinfo-classic-dark.png,
// results/frameinfo-live.png.
import { join } from 'node:path';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { test, expect, fake, settle, RESULTS } from '../lib/harness.mjs';

const hex = (b) => b.toString(16).toUpperCase().padStart(2, '0');
// A row of SavvyCAN's native GVRET CSV (FrameFileIO::nativeCSVRow): classic rows 8 data columns, FD rows max(8, LEN)
// then the flags column.
const csvRow = (t, { id, ext = false, fd = false, brs = false, data }) => {
  const cols = fd ? Math.max(8, data.length) : 8;
  const bytes = Array.from({ length: cols }, (_, i) => (i < data.length ? hex(data[i]) : '00'));
  return `${t},${id.toString(16).toUpperCase().padStart(8, '0')},${ext},Rx,0,${data.length},${bytes.join(',')},` +
    (fd ? `FD${brs ? '+BRS' : ''},` : '');
};

/** What the window should show for these frames (payloads, oldest first), as SavvyCAN's analysis counts: per bit the
 * changes from one frame of the ID to the next (the heatmap), the frames with the bit set, the heatmap's level. */
function expected(payloads) {
  const flips = new Array(512).fill(0), sets = new Array(512).fill(0);
  const ref = new Array(64).fill(null);
  let minLen = 64, maxLen = 0;
  for (const d of payloads) {
    minLen = Math.min(minLen, d.length);
    maxLen = Math.max(maxLen, d.length);
    d.forEach((v, c) => {
      if (ref[c] === null) ref[c] = v;
      for (let l = 0; l < 8; l++) {
        if (v & (1 << l)) sets[c * 8 + l]++;
        if ((ref[c] ^ v) & (1 << l)) flips[c * 8 + l]++;
      }
      ref[c] = v;
    });
  }
  if (maxLen === 0) minLen = 0;
  const n = payloads.length;
  const heat = flips.map((f) => {
    const ratio = f / n;
    let h = Math.floor(ratio * 255);
    if (h < 1 && ratio > 0.0001) h = 1;
    return h;
  });
  return { frames: n, minLen, maxLen, flips, sets, heat };
}

const info = (page) => page.evaluate(() => window.webSavvyCAN.frameInfo());
async function select(page, id) {
  expect(await page.evaluate((i) => window.webSavvyCAN.frameInfoSelect(i), id), `ID 0x${id.toString(16)} in the list`).toBe(true);
  const s = await info(page);
  expect(s.valid && s.id === id, `ID 0x${id.toString(16)} analysed`).toBe(true);
  return s;
}
function checkAnalysis(s, want, what) {
  expect(s.frames, `${what}: frames`).toBe(want.frames);
  expect([s.minLen, s.maxLen], `${what}: data length`).toEqual([want.minLen, want.maxLen]);
  expect(s.flips, `${what}: bit changes (heatmap) of all 512 bits`).toEqual(want.flips);
  expect(s.sets, `${what}: frames with each bit set (bit histogram) of all 512 bits`).toEqual(want.sets);
  expect(s.heat, `${what}: heatmap levels of all 512 bits`).toEqual(want.heat);
  // The grid as large as the frame (CANDataGrid: 8, 16, 32 or 64 bytes) and a byte graph for each byte.
  const gridBytes = want.maxLen <= 8 ? 8 : want.maxLen;
  expect(s.gridBytes, `${what}: heatmap grid bytes`).toBe(gridBytes);
  expect(s.byteGraphs, `${what}: byte graphs`).toBe(Math.max(8, want.maxLen));
}

test('a log of classic and CAN FD frames: the heatmap counts every bit, 512 of an FD ID and 64 of a classic one', async ({ app, page }) => {
  const N = 300;
  const fdId = 0x18daf110, classicId = 0x123, fd12Id = 0x200;
  const fdData = [], classicData = [], fd12Data = [];
  const rows = ['Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8'];
  let t = 1000000; // from 1 s: the loader stamps short times itself
  for (let i = 0; i < N; i++) {
    // 64-byte FD frame, 29-bit ID, BRS: byte 0 counts, byte 8 bit 0 toggles every 4th frame, byte 12 = i % 3,
    // byte 40 bit 3 toggles every frame, byte 63 constant 0xA5, the rest constant (byte index).
    const d = Array.from({ length: 64 }, (_, j) => j);
    d[0] = i & 0xff;
    d[8] = (Math.floor(i / 4) & 1) | 0x10;
    d[12] = i % 3;
    d[40] = 0x40 | (i & 1 ? 0x08 : 0);
    d[63] = 0xa5;
    fdData.push(d);
    rows.push(csvRow(t, { id: fdId, ext: true, fd: true, brs: true, data: d }));
    t += 1000;
    // classic 8-byte frame: byte 0 counts, byte 7 bit 7 toggles every frame, the rest 0x5A
    const c = [i & 0xff, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a | (i & 1 ? 0x80 : 0)];
    classicData.push(c);
    rows.push(csvRow(t, { id: classicId, data: c }));
    t += 1000;
    // 12-byte FD frame (no BRS), 11-bit ID: byte 11 counts down
    const e = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, (255 - i) & 0xff];
    fd12Data.push(e);
    rows.push(csvRow(t, { id: fd12Id, fd: true, data: e }));
    t += 1000;
  }
  mkdirSync(RESULTS, { recursive: true });
  const file = join(RESULTS, 'frameinfo-load.csv');
  writeFileSync(file, rows.join('\n') + '\n');

  await app.open();
  await app.loadFile(file, 3 * N);
  await app.menu('RE Tools', 'Frame Data Analysis');
  await expect.poll(async () => (await info(page)).open, { message: 'the Frame Data Analysis window opened' }).toBe(true);
  const opened = await info(page);
  expect([...opened.ids].sort((a, b) => a - b), 'every ID of the file in the list').toEqual([classicId, fd12Id, fdId]);
  expect(opened.valid, 'the first ID analysed as the window opens').toBe(true);
  // Browser: a framed dialog (Qt's title bar) inside the page, not a frameless window over the menus.
  expect(opened.y, 'below the top of the page (title bar)').toBeGreaterThan(0);

  // The classic ID: 64 bits.
  const classic = await select(page, classicId);
  checkAnalysis(classic, expected(classicData), 'classic 0x123');
  expect(classic.flips[7 * 8 + 7], 'byte 7 bit 7 toggles every frame').toBe(N - 1);
  expect(classic.flips.slice(64).every((v) => v === 0) && classic.heat.slice(64).every((v) => v === 0), 'nothing past byte 8').toBe(true);
  expect(classic.bytePoints, 'a point per frame in the byte graphs').toBe(N);

  // The 12-byte FD ID: 96 bits, a 16-byte grid.
  checkAnalysis(await select(page, fd12Id), expected(fd12Data), 'FD 12 bytes 0x200');

  // The 64-byte FD ID: all 512 bits.
  const fd = await select(page, fdId);
  checkAnalysis(fd, expected(fdData), 'FD 64 bytes 0x18DAF110');
  expect(fd.flips[40 * 8 + 3], 'byte 40 bit 3 toggles every frame').toBe(N - 1);
  expect(fd.heat[40 * 8 + 3], 'byte 40 bit 3: the hottest level').toBe(Math.floor(((N - 1) / N) * 255));
  expect(fd.flips.slice(63 * 8), 'byte 63 constant: no changes').toEqual(new Array(8).fill(0));
  expect(fd.sets.slice(63 * 8), 'byte 63 = 0xA5: bits 0, 2, 5, 7 set in every frame').toEqual([N, 0, N, 0, 0, N, 0, N]);
  expect(fd.flips[8 * 8], 'byte 8 bit 0 toggles every 4th frame').toBe(Math.floor((N - 1) / 4));
  expect(fd.details, 'details: frames').toContain(`# of frames: ${N}`);
  expect(fd.details, 'details: length').toContain('Data Length: 64');
  expect(fd.details, 'details: byte 63 listed').toContain('Data Byte 63');
  expect(fd.details.some((l) => l.startsWith('J1939')), 'details: 29-bit ID decoded as J1939').toBe(true);
  expect(fd.bytePoints, 'a point per frame in the byte graphs').toBe(N);

  await page.waitForTimeout(800);
  await page.screenshot({ path: join(RESULTS, 'frameinfo-light.png') });
  await app.setTheme('dark');
  await page.waitForTimeout(1200);
  await page.screenshot({ path: join(RESULTS, 'frameinfo-dark.png') });
  await select(page, classicId);
  await page.waitForTimeout(800);
  await page.screenshot({ path: join(RESULTS, 'frameinfo-classic-dark.png') });
  await app.setTheme('light');
});

test('a large capture of one FD ID: analysed in one go, byte graphs bounded, counts exact', async ({ app, page }) => {
  // 30 000 frames of a 64-byte FD ID: past 4 000 frames each byte graph point pair is a run's min and max.
  const N = 30000, id = 0x18ff0001;
  const payloads = [];
  const rows = ['Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8'];
  for (let i = 0; i < N; i++) {
    const d = new Array(64).fill(0);
    d[0] = i & 0xff;
    d[1] = (i >> 8) & 0xff;
    d[40] = i & 1 ? 0x08 : 0;
    d[63] = 0x3c;
    payloads.push(d);
    rows.push(csvRow(1000000 + i * 100, { id, ext: true, fd: true, brs: true, data: d }));
  }
  rows.push(csvRow(1000000 + N * 100, { id: 0x100, data: [1, 2, 3, 4, 5, 6, 7, 8] }));
  mkdirSync(RESULTS, { recursive: true });
  const file = join(RESULTS, 'frameinfo-large.csv');
  writeFileSync(file, rows.join('\n') + '\n');

  await app.open();
  await app.loadFile(file, N + 1);
  await app.menu('RE Tools', 'Frame Data Analysis');
  await expect.poll(async () => (await info(page)).open, { message: 'the window opened' }).toBe(true);
  await select(page, 0x100);
  const ms = await page.evaluate((i) => {
    const t0 = performance.now();
    window.webSavvyCAN.frameInfoSelect(i);
    return performance.now() - t0;
  }, id);
  const s = await info(page);
  expect(s.id, 'the large ID analysed').toBe(id);
  checkAnalysis(s, expected(payloads), 'FD 64 bytes, 30 000 frames');
  expect(s.flips[40 * 8 + 3], 'byte 40 bit 3 toggles every frame').toBe(N - 1);
  expect(s.bytePoints, 'byte graphs bounded (min/max pairs)').toBeLessThanOrEqual(8000);
  expect(s.bytePoints, 'byte graphs: still about 8 000 points').toBeGreaterThan(7000);
  test.info().annotations.push({ type: 'result', description: `analysis of ${N} 64-byte frames: ${ms.toFixed(0)} ms` });
  expect(ms, 'analysis time (the browser build has one thread)').toBeLessThan(10000);
});

test('live CAN FD (fd-mix): the ID list fills while the window is open, an ID\'s analysis matches the trace', async ({ app, page }) => {
  await app.open();
  await app.connect({ protocol: 'gvret', fd: true });
  await app.menu('RE Tools', 'Frame Data Analysis');
  await expect.poll(async () => (await info(page)).open, { message: 'the window opened' }).toBe(true);
  expect((await info(page)).ids, 'no frames yet').toEqual([]);

  const traceFrames = async () => {
    const n = (await fake.appStats(page)).framesInTrace;
    return page.evaluate((k) => window.webSavvyCAN.traceTail(k), n);
  };
  const check = async (round) => {
    const frames = await traceFrames();
    const ids = [...new Set(frames.map((f) => f.id))].sort((a, b) => a - b);
    const s = await info(page);
    expect([...s.ids].sort((a, b) => a - b), `${round}: the list has every ID of the trace (${ids.length})`).toEqual(ids);
    // 0x70F: 64-byte FD (11-bit, BRS on and off); 0x1234576: 64-byte FD, 29-bit; 0x208: classic 8 bytes.
    for (const id of [0x70f, 0x1234576, 0x208]) {
      const want = expected(frames.filter((f) => f.id === id).map((f) => f.data));
      expect(want.frames, `${round}: frames of 0x${id.toString(16)} in the trace`).toBeGreaterThan(5);
      checkAnalysis(await select(page, id), want, `${round}: 0x${id.toString(16)}`);
    }
    return frames.length;
  };

  await fake.setTraffic(page, 'fd-mix', { seconds: 4 });
  await page.waitForTimeout(4500);
  await settle(page);
  const first = await check('first 4 s');
  // More traffic: new frames, the same IDs; analysed again with the newer frames.
  await fake.setTraffic(page, 'fd-mix', { seconds: 2, seqStart: 2000 });
  await page.waitForTimeout(2500);
  await settle(page);
  expect(await check('2 s more'), 'the trace grew').toBeGreaterThan(first);
  await page.screenshot({ path: join(RESULTS, 'frameinfo-live.png') });
});

test('Save details to file: the analysis of every ID as a text file from the browser', async ({ app, page }) => {
  const rows = ['Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8'];
  for (let i = 0; i < 20; i++) {
    const d = Array.from({ length: 64 }, (_, j) => (j === 50 ? i : j));
    rows.push(csvRow(1000000 + i * 2000, { id: 0x7e8, fd: true, brs: true, data: d }));
    rows.push(csvRow(1001000 + i * 2000, { id: 0x7df, data: [2, 1, i, 0, 0, 0, 0, 0] }));
  }
  mkdirSync(RESULTS, { recursive: true });
  const file = join(RESULTS, 'frameinfo-save.csv');
  writeFileSync(file, rows.join('\n') + '\n');
  await app.open();
  await app.loadFile(file, 40);
  await app.menu('RE Tools', 'Frame Data Analysis');
  await expect.poll(async () => (await info(page)).open, { message: 'the window opened' }).toBe(true);

  // Qt hands the text to the browser: a download, or Chrome's save picker (answered here, as the user would).
  await page.evaluate(() => {
    window.__saved = null;
    window.showSaveFilePicker = async (opts) => {
      const parts = [];
      return {
        kind: 'file', name: opts?.suggestedName || 'saved.txt',
        createWritable: async () => ({
          write: async (chunk) => { parts.push(chunk?.data ?? chunk); },
          close: async () => { window.__saved = await new Blob(parts).text(); },
        }),
      };
    };
  });
  const download = page.waitForEvent('download', { timeout: 20000 }).catch(() => null);
  await app.click('button', 'Save details to file');
  const picked = page.waitForFunction(() => window.__saved, null, { timeout: 20000 }).then((h) => h.jsonValue()).catch(() => null);
  const downloaded = download.then(async (d) => (d ? readFileSync(await d.path(), 'utf8') : null));
  // whichever way it came (the other one gives null when it times out)
  const text = await Promise.race([
    picked.then((v) => v || downloaded),
    downloaded.then((v) => v || picked),
  ]);
  expect(text, 'the details came out').toBeTruthy();
  writeFileSync(join(RESULTS, 'frameinfo-details.txt'), text);
  expect(text).toContain('ID: 0x7E8');
  expect(text).toContain('ID: 0x7DF');
  expect(text).toContain('Data Length: 64');
  expect(text).toContain('Data Byte 63');
  expect(text).toMatch(/406 \(Byte 50 Bit 6\) : /); // a bit past byte 8 in the bit lists
});
