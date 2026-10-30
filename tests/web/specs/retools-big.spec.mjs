// The file-based RE tools (stage 4) on a big capture: a 1-hour log of 600 000 frames (30 IDs: Leaf-style 11-bit,
// J1939-style 29-bit, 11- and 29-bit CAN FD 64 bytes with BRS; 0x7E8 only in the second half hour), as the temporal
// and loader specs use. Capture Bisector (open, split by percentage and by ID), Frame Search (a value at byte 40 of the
// FD frames) and File Comparison (this log against a variant of it, both through the file picker): exact results, the
// time each takes, the longest pause of the page's event loop, and the memory they keep (results/retools-big.json).
import { join } from 'node:path';
import { test, expect, fake } from '../lib/harness.mjs';
import { saveResult } from '../lib/harness.mjs';
import { writeCsv, nextPick, node, kids, hexNum, canId, waitOpen } from '../lib/retools.mjs';

const N = 600_000, T0 = 5_000_000, SPAN = 3_600_000_000;
const LEAF = [0x11a, 0x1d4, 0x1da, 0x1db, 0x1dc, 0x1f2, 0x284, 0x390, 0x393, 0x45e, 0x481, 0x50a, 0x50b, 0x50c, 0x539,
  0x54a, 0x54b, 0x54c, 0x54f, 0x55a, 0x55b, 0x56e, 0x59e, 0x5a9];
const IDS = [...LEAF.map((id) => ({ id })), ...[0x0cf00400, 0x0cf00300, 0x18feee00, 0x18fef100].map((id) => ({ id, ext: true })),
  { id: 0x7e8, fd: true, late: true }, { id: 0x18daf110, ext: true, fd: true }];
const FD64 = Array.from({ length: 64 }, (_, j) => j);
/** Frame i (variant: 0x7E8's byte 40 is 0xEE and 0x123 takes 0x11A's place in the second file). */
function frame(i, variant = false) {
  const time = T0 + Math.round((i * SPAN) / (N - 1));
  let d = IDS[i % IDS.length];
  if (d.late && time < T0 + SPAN / 2) d = IDS[0];
  if (variant && d.id === 0x11a) return { time, id: 0x123, data: [i & 0xff, 0, 0, 0, 0, 0, 0, 0] };
  if (d.fd) {
    const data = FD64.slice();
    data[0] = i & 0xff;
    data[40] = variant && d.id === 0x7e8 ? 0xee : (i >> 4) & 0x3f;
    return { time, id: d.id, ext: !!d.ext, fd: true, brs: true, data };
  }
  return { time, id: d.id, ext: !!d.ext, data: [i & 0xff, 0, 0, 0, 0, 0, 0, 0] };
}

const memory = (page) => page.evaluate(() => window.webSavvyCAN.memory());
const MB = (b) => Math.round(b / 1e5) / 10;
async function timed(page, what, run) {
  await page.evaluate(() => window.__probe.reset());
  const t = Date.now();
  const out = await run();
  const s = (Date.now() - t) / 1000;
  const gap = Math.max(0, ...(await fake.probe(page)).longGaps);
  expect(gap, `${what}: longest event-loop pause ${gap} ms`).toBeLessThan(1000);
  return { s, gapMs: gap, out };
}

test('a 1-hour log of 600 000 frames: bisector, frame search and file comparison, exact, without a stall, bounded memory',
  async ({ app, page }) => {
    test.setTimeout(420_000);
    const { file, bytes } = writeCsv('retools-1h.csv', (i) => frame(i), N);
    const variant = writeCsv('retools-1h-variant.csv', (i) => frame(i, true), N).file;
    const result = { frames: N, fileMB: MB(bytes) };
    let want7e8 = 0, wantB40 = 0;
    for (let i = 0; i < N; i++) {
      const f = frame(i);
      if (f.id === 0x7e8) want7e8++;
      if (f.fd && f.data[40] === 0x2a) wantB40++;
    }

    await app.open();
    await app.loadFile(file, N, 240_000);
    const base = await memory(page);

    // ---- Capture Bisector
    let r = await timed(page, 'bisector opens', async () => {
      await app.menu('RE Tools', 'Capture Bisector');
      await waitOpen(page, 'bisect');
      await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.bisect())).ids.length,
        { timeout: 60_000 }).toBe(30);
    });
    result.bisectOpenS = r.s;
    r = await timed(page, 'split at 50 %', async () => {
      await page.evaluate(() => window.webSavvyCAN.bisectSet({ mode: 'percent', percent: 50, lower: false }));
      await app.click('button', 'Calculate Split');
      await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.bisect())).splitCount,
        { timeout: 60_000 }).toBe(N / 2);
    });
    let b = await page.evaluate(() => window.webSavvyCAN.bisect());
    result.bisectSplitS = r.s;
    result.bisectSplitMs = b.calculateMs;
    result.bisectSplitGapMs = r.gapMs;
    expect(b.splitIds[0x7e8], 'all of 0x7E8 in the second half').toBe(want7e8);
    expect(b.splitFirstUs).toBe(frame(N / 2).time);
    const afterSplit = await memory(page);
    result.splitStoreMB = MB(b.splitBytes);
    result.splitHeapMB = MB(afterSplit.heapUsed - base.heapUsed);
    // the split in the compact store: < 100 bytes a frame (SavvyCAN's QVector<CommFrame>: ~100-160 in the browser)
    expect(afterSplit.heapUsed - base.heapUsed, `the split's memory: ${result.splitHeapMB} MB`).toBeLessThan(N / 2 * 100);
    await page.evaluate((id) => window.webSavvyCAN.bisectSet({ mode: 'id', lowId: id, highId: id, lower: true }), canId(0x7e8));
    await app.click('button', 'Calculate Split');
    await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.bisect())).splitCount, { timeout: 60_000 }).toBe(want7e8);
    b = await page.evaluate(() => window.webSavvyCAN.bisect());
    expect(b.splitFirstUs, '0x7E8 from the second half hour').toBeGreaterThanOrEqual(T0 + SPAN / 2);
    await page.keyboard.press('Escape');

    // ---- Frame Search: byte 40 of the FD frames = 42
    await app.menu('RE Tools', 'Frame Search');
    await waitOpen(page, 'search');
    await page.evaluate(() => window.webSavvyCAN.searchSet({ id: null, len: 64, mode: 'specific', startBit: 320, bits: 8,
      intel: true, signed: false, scale: 1, bias: 0, value: '42' }));
    r = await timed(page, 'search', async () => {
      await app.click('button', 'Search');
      await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.search())).label, { timeout: 60_000 })
        .toBe(`Match 1 of ${wantB40}`);
    });
    const sr = await page.evaluate(() => window.webSavvyCAN.search());
    result.searchS = r.s;
    result.searchMs = sr.searchMs;
    result.searchGapMs = r.gapMs;
    result.searchMatches = sr.count;
    for (const row of sr.rows.slice(0, 50)) expect(frame(row).data[40], `row ${row}`).toBe(0x2a);
    await page.keyboard.press('Escape');

    // ---- File Comparison: the log against its variant, both through the picker, then summaries only
    const beforeCompare = await memory(page);
    await app.menu('RE Tools', 'File Comparison');
    await waitOpen(page, 'fileCompare');
    r = await timed(page, 'file comparison, side 1', async () => {
      await nextPick(page, file);
      await app.click('button', 'Load New File');
      await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.fileCompare())).side1.frames,
        { timeout: 120_000 }).toBe(N);
    });
    result.compareLoad1S = r.s;
    result.compareLoad1GapMs = r.gapMs;
    r = await timed(page, 'file comparison, side 2', async () => {
      await nextPick(page, variant);
      await app.click('button', 'Load A File');
      await expect.poll(async () => (await page.evaluate(() => window.webSavvyCAN.fileCompare())).side2.frames,
        { timeout: 120_000 }).toBe(N);
    });
    result.compareLoad2S = r.s;
    result.compareLoad2GapMs = r.gapMs;
    const fc = await page.evaluate(() => window.webSavvyCAN.fileCompare());
    result.compareMs = fc.compareMs;
    expect(kids(node(fc.tree, 'IDs found only in retools-1h.csv'))).toEqual([hexNum(0x11a)]);
    expect(kids(node(fc.tree, 'IDs found only in Side 2'))).toEqual([hexNum(0x123)]);
    expect(kids(node(fc.tree, 'IDs found on both sides', hexNum(0x7e8), 'Byte 40', 'Values found only in Side 2'))).toEqual(['0xEE']);
    expect(kids(node(fc.tree, 'IDs found on both sides', hexNum(0x7e8), 'Byte 40', 'Values found only in retools')).length).toBe(64);
    const afterCompare = await memory(page);
    result.compareKeptMB = MB(afterCompare.heapUsed - beforeCompare.heapUsed);
    result.compareSummaryBytes = fc.side1.memoryBytes + fc.side2.memoryBytes;
    result.peakFootprintMB = MB(afterCompare.maxFootprint);
    result.heapSizeMB = MB(afterCompare.heapSize);
    // what stays: the two summaries and the tree, not the 2 x 600 000 frames (~50 MB as compact frames)
    expect(afterCompare.heapUsed - beforeCompare.heapUsed, `kept after both files: ${result.compareKeptMB} MB`).toBeLessThan(5e6);
    expect(result.compareSummaryBytes).toBeLessThan(200_000);
    saveResult('retools-big', result);
    test.info().annotations.push({ type: 'result', description: JSON.stringify(result) });
  });
