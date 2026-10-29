// Temporal Graph (stage 4, switched on 2026-10-29): RE Tools > Temporal Graph shows which IDs appear when, as a colour
// map of frame counts per (time column, ID) with a bounded size (G1-G4 of resolve-can docs/research/savvycan-improve.md).
// SavvyCAN's own window aborted the page on 29-bit IDs (its map was ID range / 30 x seconds x 4 cells: 20 000 frames
// of J1939 IDs over 60 s = 1.6 G cells; "Aborted()" in the browser, measured 2026-10-29), plotted every frame as a
// circle, was frameless over the main window's menus and never updated live.
// Pictures: results/temporal-file-{light,dark}.png, results/temporal-live-{light,dark}.png; numbers:
// results/temporal-file.json, results/temporal-live.json.
import { join } from 'node:path';
import { mkdirSync, writeFileSync, statSync } from 'node:fs';
import { test, expect, fake, RESULTS, saveResult } from '../lib/harness.mjs';

const state = (page) => page.evaluate(() => window.webSavvyCAN.temporalState());
const hex = (n, w) => n.toString(16).toUpperCase().padStart(w, '0');
const byKey = (a, b) => (a.ext - b.ext) || (a.id - b.id); // the window's row order: 11-bit, then 29-bit, by ID

// The Leaf AZE0 EV-CAN's IDs (fake/rc1.js LEAF_TABLE).
const LEAF = [0x11a, 0x1d4, 0x1da, 0x1db, 0x1dc, 0x1f2, 0x284, 0x390, 0x393, 0x45e, 0x481, 0x50a, 0x50b, 0x50c, 0x539,
  0x54a, 0x54b, 0x54c, 0x54f, 0x55a, 0x55b, 0x56e, 0x59e, 0x5a9, 0x5b9, 0x5bc, 0x5c0, 0x60d];

/** Waits until rc1 has nothing queued, the app nothing in its queue and the trace stops growing (a few GUI ticks). */
async function quiet(page, ms = 30000) {
  const end = Date.now() + ms;
  let last = -1, same = 0;
  for (;;) {
    const [s, a] = [await fake.stats(page), await fake.appStats(page)];
    same = s.queuedBytes === 0 && a.queued === 0 && a.framesReceived === last ? same + 1 : 0;
    last = a.framesReceived;
    if (same >= 4 || Date.now() > end) return a;
    await page.waitForTimeout(250);
  }
}

test('a 1-hour log of 600 000 frames (30 IDs, 29-bit, CAN FD 64 bytes) opens without a stall: every ID, the whole hour',
  async ({ app, page }) => {
    test.setTimeout(300_000);
    // A GVRET CSV as SavvyCAN saves it: 24 Leaf IDs and 4 J1939-style 29-bit IDs (classic, 8 bytes), 0x7E8 (FD 64 bytes
    // with BRS, only in the second half hour) and 0x18DAF110 (29-bit FD 64 bytes), round robin over one hour.
    const N = 600_000, T0 = 5_000_000, SPAN = 3_600_000_000;
    const ids = [...LEAF.slice(0, 24).map((id) => ({ id, ext: false })),
      ...[0x0cf00400, 0x0cf00300, 0x18feee00, 0x18fef100].map((id) => ({ id, ext: true })),
      { id: 0x7e8, ext: false, fd: true, late: true }, { id: 0x18daf110, ext: true, fd: true }];
    const expected = new Map(ids.map((d) => [d.id, { id: d.id, ext: d.ext, frames: 0 }]));
    const fd64 = Array.from({ length: 63 }, (_, j) => hex(j + 1, 2)).join(',');
    const rows = ['Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8'];
    for (let i = 0; i < N; i++) {
      const t = T0 + Math.round((i * SPAN) / (N - 1));
      let d = ids[i % ids.length];
      if (d.late && t < T0 + SPAN / 2) d = ids[0];
      expected.get(d.id).frames++;
      const b0 = hex(i & 0xff, 2);
      rows.push(d.fd ? `${t},${hex(d.id, 8)},${d.ext},Rx,0,64,${b0},${fd64},FD+BRS,`
        : `${t},${hex(d.id, 8)},${d.ext},Rx,0,8,${b0},00,00,00,00,00,00,00,`);
    }
    mkdirSync(RESULTS, { recursive: true });
    const file = join(RESULTS, 'temporal-1h.csv');
    writeFileSync(file, rows.join('\n') + '\n');
    const fileMB = statSync(file).size / 1e6;

    await app.open();
    let t = Date.now();
    await app.loadFile(file, N, 240_000);
    const loadS = (Date.now() - t) / 1000;
    expect((await fake.appStats(page)).framesReceived, 'every frame of the file in the trace').toBe(N);

    // RE Tools > Temporal Graph. No "Danger Will Robinson" question any more; the map is built in slices.
    await page.evaluate(() => window.__probe.reset());
    t = Date.now();
    await app.menu('RE Tools', 'Temporal Graph');
    await page.waitForFunction(() => { const s = window.webSavvyCAN.temporalState(); return s.open && !s.building; },
      null, { timeout: 60_000, polling: 100 });
    const openS = (Date.now() - t) / 1000;
    const probe = await fake.probe(page);
    const s = await state(page);

    expect(s.frames, 'every frame counted').toBe(N);
    expect(s.hiddenFrames, 'no ID left out').toBe(0);
    expect(s.plotted, 'every frame in the colour map').toBe(N);
    expect(s.ids.map(({ id, ext, frames }) => ({ id, ext, frames })), 'every ID, its frames').toEqual([...expected.values()].sort(byKey));
    // When: the whole hour on the time axis (µs, the app's default time style), to within one column at each end.
    const tLast = T0 + SPAN;
    expect(s.firstUs).toBe(T0);
    expect(s.lastUs).toBe(tLast);
    expect(s.xLower, 'x axis from the first frame').toBeLessThanOrEqual(T0);
    expect(s.xLower).toBeGreaterThan(T0 - s.binUs - 1);
    expect(s.xUpper, 'x axis to the last frame').toBeGreaterThanOrEqual(tLast);
    expect(s.xUpper).toBeLessThan(tLast + s.binUs + 1);
    const late = s.ids.find((r) => r.id === 0x7e8);
    expect(late.fromUs, '0x7E8 only from the second half hour').toBeGreaterThanOrEqual(T0 + SPAN / 2 - s.binUs);
    for (const r of s.ids.filter((x) => x.id !== 0x7e8)) {
      expect(r.fromUs, `0x${hex(r.id, 3)} from the start`).toBeLessThanOrEqual(T0);
      expect(r.toUs, `0x${hex(r.id, 3)} to the end`).toBeGreaterThanOrEqual(tLast);
    }
    // Bounded: <= 512 columns x one row per ID, one plottable (no point per frame), well under a megabyte.
    expect(s.columns).toBeLessThanOrEqual(s.maxColumns);
    expect(s.mapKeySize).toBeLessThanOrEqual(s.maxColumns);
    expect(s.mapValueSize).toBe(30);
    expect([s.plottables, s.graphs, s.items], 'one colour map, no graphs, no items').toEqual([1, 0, 0]);
    expect(s.memoryBytes, `counts + map: ${s.memoryBytes} bytes`).toBeLessThan(1_000_000);
    // Opens within seconds and the page keeps running meanwhile (slices of ~25 ms).
    expect(openS, `opened in ${openS} s`).toBeLessThan(15);
    const maxGap = Math.max(0, ...probe.longGaps);
    expect(maxGap, `longest event-loop gap while opening: ${maxGap} ms`).toBeLessThan(1000);

    await page.waitForTimeout(800);
    await page.screenshot({ path: join(RESULTS, 'temporal-file-light.png') });
    await app.setTheme('dark');
    await page.waitForTimeout(1000);
    await page.screenshot({ path: join(RESULTS, 'temporal-file-dark.png') });
    await app.setTheme('light');
    const result = { frames: N, ids: s.ids.length, fileMB, loadS, openS, maxGapMs: maxGap, eventLoop: probe.timer,
      binUs: s.binUs, columns: s.columns, memoryBytes: s.memoryBytes };
    saveResult('temporal-file', result);
    test.info().annotations.push({ type: 'result', description: JSON.stringify(result) });
  });

test('live: the map grows with the bus, follows its end, counts every frame once though the trace wraps, stays bounded',
  async ({ app, page }) => {
    test.setTimeout(180_000);
    // A small trace ring (File > Preferences: Maximum frames 50 000): the trace drops its oldest frames many times over
    // while the map keeps the whole session.
    await page.addInitScript(() => localStorage.setItem('qt-v0-EVTV-SavvyCAN-Main/MaximumFrames', '50000'));
    await app.open();
    await app.connect({ protocol: 'gvret', fd: true });
    await app.menu('RE Tools', 'Temporal Graph');
    await page.waitForFunction(() => window.webSavvyCAN.temporalState().open, null, { timeout: 10_000 });

    // The Leaf bus for 4 s.
    await fake.setTraffic(page, 'leaf', { seconds: 4 });
    await page.waitForTimeout(2000);
    const a = await state(page);
    const stA = await quiet(page);
    const b = await state(page);
    expect(b.frames, `grows (${a.frames} -> ${b.frames})`).toBeGreaterThan(a.frames + 1000);
    expect(b.lastUs - a.lastUs, 'newer frames (µs)').toBeGreaterThan(1e6);
    expect(b.frames, 'every frame of the bus counted once').toBe(stA.framesReceived);
    expect(b.ids.map((r) => r.id), 'every Leaf ID').toEqual([...LEAF].sort((x, y) => x - y));
    expect(b.xUpper, 'the view follows the newest frame').toBeGreaterThanOrEqual(b.lastUs);
    expect(b.xUpper - b.lastUs).toBeLessThanOrEqual(b.binUs);
    expect(b.xLower).toBeLessThanOrEqual(b.firstUs);
    await page.screenshot({ path: join(RESULTS, 'temporal-live-light.png') });
    await app.setTheme('dark');
    await page.waitForTimeout(1000);
    await page.screenshot({ path: join(RESULTS, 'temporal-live-dark.png') });

    // A busy CAN FD bus: 64-byte frames with BRS from 16 11-bit IDs and a 29-bit one, 20 s at 16 000 frames/s (twice
    // rc1's real FD rate; minutes of a normal bus).
    await fake.setTraffic(page, 'fd64-full', { seconds: 20, rate: 16000, extendedEvery: 7 });
    await page.waitForTimeout(10_000);
    const c = await state(page);
    await page.waitForTimeout(11_000);
    const st = await quiet(page);
    const d = await state(page);
    expect(st.framesEvicted, 'the trace ring wrapped').toBeGreaterThan(100_000);
    expect(d.frames, 'every frame counted once (none twice, none lost) across the wraps').toBe(st.framesReceived);
    expect(d.frames - b.frames, 'FD frames counted').toBeGreaterThan(100_000);
    expect(d.ids.length, 'Leaf + 16 FD IDs + 0x18DB33F1').toBe(28 + 17);
    expect(d.ids.some((r) => r.ext && r.id === 0x18db33f1), '29-bit FD ID').toBe(true);
    expect(d.plotted).toBe(d.frames);
    expect([d.plottables, d.graphs, d.items], 'one colour map, no graphs, no items').toEqual([1, 0, 0]);
    expect(d.columns).toBeLessThanOrEqual(d.maxColumns);
    expect(d.mapValueSize).toBe(d.ids.length);
    // Memory: the same IDs, so the counts didn't grow; the whole structure stays under 64 KB per ID.
    expect(d.memoryBytes, `${c.memoryBytes} -> ${d.memoryBytes} bytes`).toBeLessThan(d.ids.length * 65536);
    expect(d.memoryBytes).toBeLessThanOrEqual(c.memoryBytes * 2);
    expect(d.xUpper).toBeGreaterThanOrEqual(d.lastUs);
    await app.setTheme('light');
    const result = { leafFrames: b.frames, framesReceived: st.framesReceived, framesEvicted: st.framesEvicted,
      framesDropped: st.framesDropped, ids: d.ids.length, binUs: d.binUs, columns: d.columns,
      memoryBytesAt10s: c.memoryBytes, memoryBytesAt20s: d.memoryBytes };
    saveResult('temporal-live', result);
    test.info().annotations.push({ type: 'result', description: JSON.stringify(result) });
  });
